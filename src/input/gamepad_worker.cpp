/**
 * Alice Senki 2 - Background Gamepad I/O Worker (Implementation)
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "input/gamepad_worker.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace Input {

namespace {

enum class CommandType : uint8_t {
    Close,
    Open,
    CancelOpen,
    Bootstrap,
    Shutdown,
};

struct WorkerCommand {
    CommandType      type = CommandType::Close;
    SDL_Gamepad*     handle = nullptr;
    SDL_JoystickID   instance_id = 0;
    int              slot = -1;
    SDL_GUID         guid{};
    bool             guid_valid = false;
};

constexpr size_t MAX_PENDING_COMMANDS = 64;
constexpr size_t MAX_PENDING_RESULTS  = 64;
constexpr DWORD  WORKER_WAIT_MS = 5;
constexpr DWORD  HOTPLUG_SCAN_INTERVAL_MS = 500;

static std::thread              s_worker;
static std::mutex               s_commandMutex;
static std::condition_variable  s_commandCv;
static std::deque<WorkerCommand> s_commands;

static std::mutex               s_resultMutex;
static std::deque<GamepadWorkerResult> s_results;

static std::mutex               s_statsMutex;
static GamepadWorkerStats       s_stats{};

static std::mutex               s_slotSnapshotMutex;
static GamepadWorkerSlotSnapshot s_slotSnapshot{};

static SDL_JoystickID           s_workerPendingInstance[kMaxGamepads] = {};

static std::atomic<bool>        s_initialized{false};
static std::atomic<bool>        s_stopRequested{false};

static DWORD                    s_lastHotplugScanMs = 0;
static std::unordered_set<SDL_JoystickID> s_lastScanSnapshot;
static bool                     s_scanBaselineReady = false;

static std::atomic<DWORD>       s_churnActiveUntilMs{0};

static void MarkChurnActive(DWORD durationMs) {
    const DWORD tailMs = 250;
    const DWORD extraMs = durationMs >= 16 ? durationMs + tailMs : 500;
    const DWORD now = GetTickCount();
    const DWORD until = now + extraMs;
    DWORD previous = s_churnActiveUntilMs.load(std::memory_order_relaxed);
    while (until > previous &&
           !s_churnActiveUntilMs.compare_exchange_weak(
               previous, until, std::memory_order_relaxed)) {
    }
}

static void UpdateCommandDepthLocked() {
    s_stats.command_queue_depth = (uint32_t)s_commands.size();
}

static void UpdateResultDepthLocked() {
    s_stats.result_queue_depth = (uint32_t)s_results.size();
}

static bool GUIDEquals(const SDL_GUID& a, const SDL_GUID& b) {
    return memcmp(a.data, b.data, sizeof(a.data)) == 0;
}

static GamepadWorkerSlotSnapshot CopySlotSnapshot() {
    std::lock_guard<std::mutex> lock(s_slotSnapshotMutex);
    return s_slotSnapshot;
}

static void ClearWorkerPendingSlot(int slot) {
    if (slot >= 0 && slot < kMaxGamepads) {
        s_workerPendingInstance[slot] = 0;
    }
}

static void ClearWorkerPendingForInstance(SDL_JoystickID instanceId) {
    if (instanceId == 0) {
        return;
    }
    for (int slot = 0; slot < kMaxGamepads; slot++) {
        if (s_workerPendingInstance[slot] == instanceId) {
            s_workerPendingInstance[slot] = 0;
        }
    }
}

static bool IsInstanceTrackedOnWorker(SDL_JoystickID instanceId,
                                      const GamepadWorkerSlotSnapshot& snapshot) {
    if (instanceId == 0) {
        return true;
    }

    for (int slot = 0; slot < kMaxGamepads; slot++) {
        if (s_workerPendingInstance[slot] == instanceId) {
            return true;
        }
        if (snapshot.occupied[slot] && snapshot.live_instance_id[slot] == instanceId) {
            return true;
        }
    }

    return false;
}

static bool IsSlotAvailable(int slot, const GamepadWorkerSlotSnapshot& snapshot) {
    if (slot < 0 || slot >= kMaxGamepads) {
        return false;
    }
    if (snapshot.occupied[slot]) {
        return false;
    }
    if (s_workerPendingInstance[slot] != 0) {
        return false;
    }
    return true;
}

static int PickSlotForDevice(const SDL_GUID& deviceGuid,
                             bool deviceGuidValid,
                             const GamepadWorkerSlotSnapshot& snapshot) {
    if (deviceGuidValid) {
        for (int slot = 0; slot < kMaxGamepads; slot++) {
            if (!IsSlotAvailable(slot, snapshot)) {
                continue;
            }
            if (snapshot.guid_valid[slot] && GUIDEquals(snapshot.guid[slot], deviceGuid)) {
                return slot;
            }
        }
    }

    for (int slot = 0; slot < kMaxGamepads; slot++) {
        if (IsSlotAvailable(slot, snapshot)) {
            return slot;
        }
    }

    return -1;
}

static void PushAttachResult(const GamepadWorkerOpenResult& attachResult) {
    std::lock_guard<std::mutex> resultLock(s_resultMutex);
    std::lock_guard<std::mutex> statsLock(s_statsMutex);

    if (s_results.size() >= MAX_PENDING_RESULTS) {
        if (attachResult.gamepad) {
            SDL_CloseGamepad(attachResult.gamepad);
        }
        s_stats.result_drop_count++;
        UpdateResultDepthLocked();
        Rollback::NetplayLog_Write("INPUT", -1,
            "Gamepad worker dropped attach command: slot=%d id=%u",
            attachResult.slot,
            (unsigned)attachResult.instance_id);
        return;
    }

    GamepadWorkerResult result{};
    result.attach = attachResult;
    s_results.push_back(result);
    UpdateResultDepthLocked();
}

static bool EnqueueCommand(const WorkerCommand& cmd) {
    if (!s_initialized.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> commandLock(s_commandMutex);
        if (s_commands.size() >= MAX_PENDING_COMMANDS) {
            std::lock_guard<std::mutex> statsLock(s_statsMutex);
            s_stats.command_drop_count++;
            UpdateCommandDepthLocked();
            return false;
        }
        s_commands.push_back(cmd);
        std::lock_guard<std::mutex> statsLock(s_statsMutex);
        UpdateCommandDepthLocked();
    }

    s_commandCv.notify_one();
    return true;
}

static void CopyCommands(std::deque<WorkerCommand>* out) {
    if (!out) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_commandMutex);
    if (s_commands.empty()) {
        return;
    }

    out->insert(out->end(), s_commands.begin(), s_commands.end());
    s_commands.clear();

    std::lock_guard<std::mutex> statsLock(s_statsMutex);
    UpdateCommandDepthLocked();
}

static bool IsOpenCancelled(const std::unordered_set<SDL_JoystickID>& cancelled,
                            SDL_JoystickID instanceId) {
    return cancelled.find(instanceId) != cancelled.end();
}

static bool TryQueueConnectForDevice(SDL_JoystickID instanceId) {
    if (instanceId == 0) {
        return false;
    }

    const GamepadWorkerSlotSnapshot snapshot = CopySlotSnapshot();
    if (IsInstanceTrackedOnWorker(instanceId, snapshot)) {
        return false;
    }

    const SDL_GUID deviceGuid = SDL_GetGamepadGUIDForID(instanceId);
    const bool deviceGuidValid = true;
    const int slot = PickSlotForDevice(deviceGuid, deviceGuidValid, snapshot);
    if (slot < 0) {
        Rollback::NetplayLog_Write("INPUT", -1,
            "Gamepad worker ignored new device (no free slot): id=%u",
            (unsigned)instanceId);
        return false;
    }

    WorkerCommand cmd{};
    cmd.type = CommandType::Open;
    cmd.instance_id = instanceId;
    cmd.slot = slot;
    cmd.guid = deviceGuid;
    cmd.guid_valid = deviceGuidValid;
    if (!EnqueueCommand(cmd)) {
        return false;
    }

    if (cmd.type == CommandType::Open ||
        cmd.type == CommandType::Close ||
        cmd.type == CommandType::Bootstrap) {
        MarkChurnActive(500);
    }

    s_workerPendingInstance[slot] = instanceId;
    {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        s_stats.hotplug_connects_queued++;
    }

    Rollback::NetplayLog_Write("INPUT", -1,
        "Gamepad worker queued connect on background thread: slot=%d id=%u",
        slot,
        (unsigned)instanceId);
    return true;
}

static void BootstrapExistingGamepads() {
    const DWORD startMs = GetTickCount();
    int count = 0;
    // Every joystick, not just the ones SDL has a gamepad mapping for:
    // arcade sticks, hitboxes and custom boards have no mapping and would
    // otherwise never be seen at all.
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);

    std::unordered_set<SDL_JoystickID> current;
    if (ids) {
        for (int i = 0; i < count; i++) {
            if (ids[i] == 0) {
                continue;
            }
            current.insert(ids[i]);
            TryQueueConnectForDevice(ids[i]);
        }
        SDL_free(ids);
    }

    s_lastScanSnapshot = current;
    s_scanBaselineReady = true;
    s_lastHotplugScanMs = GetTickCount();

    const DWORD durationMs = GetTickCount() - startMs;
    MarkChurnActive(durationMs);

    Rollback::NetplayLog_Write("INPUT", -1,
        "Gamepad worker bootstrap scan complete: count=%d duration_ms=%lu",
        count,
        (unsigned long)durationMs);
}

static void ScanForNewGamepadsIfDue() {
    if (!s_scanBaselineReady) {
        return;
    }

    const DWORD now = GetTickCount();
    if (s_lastHotplugScanMs != 0 &&
        (DWORD)(now - s_lastHotplugScanMs) < HOTPLUG_SCAN_INTERVAL_MS) {
        return;
    }
    s_lastHotplugScanMs = now;

    const DWORD startMs = GetTickCount();
    int count = 0;
    // Every joystick, not just the ones SDL has a gamepad mapping for:
    // arcade sticks, hitboxes and custom boards have no mapping and would
    // otherwise never be seen at all.
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);

    std::unordered_set<SDL_JoystickID> current;
    if (ids) {
        for (int i = 0; i < count; i++) {
            if (ids[i] != 0) {
                current.insert(ids[i]);
            }
        }
        SDL_free(ids);
    }

    const DWORD durationMs = GetTickCount() - startMs;
    if (durationMs >= 16) {
        MarkChurnActive(durationMs);
    }
    {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        s_stats.hotplug_scans_completed++;
        s_stats.last_scan_duration_ms = durationMs;
    }

    if (durationMs >= 16) {
        LOG_INFO("[Input] Gamepad worker hotplug scan took %lu ms count=%d",
                 (unsigned long)durationMs,
                 count);
        Rollback::NetplayLog_Write("INPUT", -1,
            "Gamepad worker hotplug scan slow: duration_ms=%lu count=%d",
            (unsigned long)durationMs,
            count);
    }

    for (SDL_JoystickID id : current) {
        if (s_lastScanSnapshot.find(id) == s_lastScanSnapshot.end()) {
            TryQueueConnectForDevice(id);
        }
    }

    s_lastScanSnapshot = current;
}

static void ProcessCommands(std::deque<WorkerCommand>* commands,
                            std::unordered_set<SDL_JoystickID>* cancelledOpens,
                            bool honorShutdown) {
    if (!commands || !cancelledOpens) {
        return;
    }

    for (const WorkerCommand& cmd : *commands) {
        switch (cmd.type) {
        case CommandType::Bootstrap:
            BootstrapExistingGamepads();
            break;

        case CommandType::Close: {
            if (!cmd.handle) {
                break;
            }

            const DWORD startMs = GetTickCount();
            SDL_CloseGamepad(cmd.handle);
            const DWORD durationMs = GetTickCount() - startMs;
            MarkChurnActive(durationMs);

            {
                std::lock_guard<std::mutex> lock(s_statsMutex);
                s_stats.closes_completed++;
                s_stats.last_close_duration_ms = durationMs;
            }

            Rollback::NetplayLog_Write("INPUT", -1,
                "Gamepad worker close complete: handle=0x%p duration_ms=%lu",
                static_cast<void*>(cmd.handle),
                (unsigned long)durationMs);
            if (durationMs >= 16) {
                LOG_INFO("[Input] Gamepad worker close took %lu ms", (unsigned long)durationMs);
            }
            break;
        }

        case CommandType::Open: {
            if (cmd.slot < 0 || cmd.instance_id == 0) {
                ClearWorkerPendingForInstance(cmd.instance_id);
                break;
            }

            if (IsOpenCancelled(*cancelledOpens, cmd.instance_id)) {
                ClearWorkerPendingSlot(cmd.slot);
                std::lock_guard<std::mutex> lock(s_statsMutex);
                s_stats.opens_cancelled++;
                Rollback::NetplayLog_Write("INPUT", -1,
                    "Gamepad worker skipped cancelled open: slot=%d id=%u",
                    cmd.slot,
                    (unsigned)cmd.instance_id);
                break;
            }

            const DWORD startMs = GetTickCount();
            SDL_Gamepad* gp = nullptr;
            SDL_Joystick* js = nullptr;
            if (SDL_IsGamepad(cmd.instance_id)) {
                gp = SDL_OpenGamepad(cmd.instance_id);
            } else {
                // No standardized mapping exists and none is required: the
                // device is driven positionally, which is how an arcade stick
                // or a custom board is meant to work.
                js = SDL_OpenJoystick(cmd.instance_id);
            }
            const DWORD durationMs = GetTickCount() - startMs;
            MarkChurnActive(durationMs);

            if (IsOpenCancelled(*cancelledOpens, cmd.instance_id)) {
                cancelledOpens->erase(cmd.instance_id);
                if (gp) {
                    SDL_CloseGamepad(gp);
                }
                if (js) {
                    SDL_CloseJoystick(js);
                }
                ClearWorkerPendingSlot(cmd.slot);
                std::lock_guard<std::mutex> lock(s_statsMutex);
                s_stats.opens_cancelled++;
                Rollback::NetplayLog_Write("INPUT", -1,
                    "Gamepad worker closed open cancelled during open: slot=%d id=%u duration_ms=%lu",
                    cmd.slot,
                    (unsigned)cmd.instance_id,
                    (unsigned long)durationMs);
                break;
            }

            cancelledOpens->erase(cmd.instance_id);
            ClearWorkerPendingSlot(cmd.slot);

            {
                std::lock_guard<std::mutex> lock(s_statsMutex);
                s_stats.opens_completed++;
                s_stats.last_open_duration_ms = durationMs;
            }

            if (durationMs >= 16) {
                LOG_INFO("[Input] Gamepad worker open slot=%d id=%u took %lu ms ok=%d",
                         cmd.slot,
                         (unsigned)cmd.instance_id,
                         (unsigned long)durationMs,
                         (gp || js) ? 1 : 0);
            }

            GamepadWorkerOpenResult attachResult{};
            attachResult.slot = cmd.slot;
            attachResult.instance_id = cmd.instance_id;
            attachResult.gamepad = gp;
            attachResult.joystick = js;
            attachResult.guid = cmd.guid;
            attachResult.guid_valid = cmd.guid_valid;
            attachResult.duration_ms = durationMs;
            PushAttachResult(attachResult);

            Rollback::NetplayLog_Write("INPUT", -1,
                "Gamepad worker attach command ready: slot=%d id=%u handle=0x%p open_ms=%lu",
                attachResult.slot,
                (unsigned)attachResult.instance_id,
                static_cast<void*>(attachResult.gamepad),
                (unsigned long)durationMs);
            break;
        }

        case CommandType::CancelOpen:
            if (cmd.instance_id != 0) {
                cancelledOpens->insert(cmd.instance_id);
                ClearWorkerPendingForInstance(cmd.instance_id);
            }
            break;

        case CommandType::Shutdown:
            if (honorShutdown) {
                s_stopRequested.store(true);
            }
            break;
        }
    }
}

static void WorkerThreadMain() {
    std::unordered_set<SDL_JoystickID> cancelledOpens;

    {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        s_stats.worker_running = true;
    }

    while (!s_stopRequested.load()) {
        {
            std::unique_lock<std::mutex> lock(s_commandMutex);
            s_commandCv.wait_for(lock, std::chrono::milliseconds(WORKER_WAIT_MS),
                                 [] {
                                     return s_stopRequested.load() || !s_commands.empty();
                                 });
        }

        std::deque<WorkerCommand> localCommands;
        CopyCommands(&localCommands);
        if (!localCommands.empty()) {
            ProcessCommands(&localCommands, &cancelledOpens, true);
        }

        ScanForNewGamepadsIfDue();
    }

    for (;;) {
        std::deque<WorkerCommand> localCommands;
        CopyCommands(&localCommands);
        if (localCommands.empty()) {
            break;
        }
        ProcessCommands(&localCommands, &cancelledOpens, false);
    }

    {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        s_stats.worker_running = false;
    }
}

} // anonymous namespace

bool GamepadWorker_Init() {
    if (s_initialized.load()) {
        return true;
    }

    {
        std::lock_guard<std::mutex> commandLock(s_commandMutex);
        s_commands.clear();
    }
    {
        std::lock_guard<std::mutex> resultLock(s_resultMutex);
        s_results.clear();
    }
    {
        std::lock_guard<std::mutex> statsLock(s_statsMutex);
        memset(&s_stats, 0, sizeof(s_stats));
    }
    {
        std::lock_guard<std::mutex> lock(s_slotSnapshotMutex);
        memset(&s_slotSnapshot, 0, sizeof(s_slotSnapshot));
    }

    s_workerPendingInstance[0] = 0;
    s_workerPendingInstance[1] = 0;
    s_lastHotplugScanMs = 0;
    s_lastScanSnapshot.clear();
    s_scanBaselineReady = false;
    s_stopRequested.store(false);
    s_initialized.store(true);
    s_worker = std::thread(WorkerThreadMain);

    Rollback::NetplayLog_Write("INPUT", -1, "Gamepad worker thread started");
    LOG_INFO("[Input] Gamepad worker thread started");
    return true;
}

void GamepadWorker_Shutdown() {
    if (!s_initialized.load()) {
        return;
    }

    WorkerCommand shutdownCmd{};
    shutdownCmd.type = CommandType::Shutdown;
    EnqueueCommand(shutdownCmd);
    s_commandCv.notify_all();

    if (s_worker.joinable()) {
        s_worker.join();
    }

    {
        std::lock_guard<std::mutex> commandLock(s_commandMutex);
        s_commands.clear();
    }
    {
        std::lock_guard<std::mutex> resultLock(s_resultMutex);
        while (!s_results.empty()) {
            GamepadWorkerResult& pending = s_results.front();
            if (pending.attach.gamepad) {
                SDL_CloseGamepad(pending.attach.gamepad);
            }
            s_results.pop_front();
        }
    }
    {
        std::lock_guard<std::mutex> statsLock(s_statsMutex);
        s_stats.command_queue_depth = 0;
        s_stats.result_queue_depth = 0;
        s_stats.worker_running = false;
    }

    s_lastScanSnapshot.clear();
    s_scanBaselineReady = false;
    s_lastHotplugScanMs = 0;
    s_workerPendingInstance[0] = 0;
    s_workerPendingInstance[1] = 0;
    s_churnActiveUntilMs.store(0, std::memory_order_relaxed);
    s_initialized.store(false);
    Rollback::NetplayLog_Write("INPUT", -1, "Gamepad worker thread stopped");
    LOG_INFO("[Input] Gamepad worker thread stopped");
}

void GamepadWorker_UpdateSlotSnapshot(const GamepadWorkerSlotSnapshot* snapshot) {
    if (!snapshot) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_slotSnapshotMutex);
    s_slotSnapshot = *snapshot;
}

void GamepadWorker_RequestBootstrap() {
    WorkerCommand cmd{};
    cmd.type = CommandType::Bootstrap;
    if (EnqueueCommand(cmd)) {
        MarkChurnActive(750);
    }
}

bool GamepadWorker_RequestClose(SDL_Gamepad* gamepad) {
    if (!gamepad) {
        return true;
    }

    WorkerCommand cmd{};
    cmd.type = CommandType::Close;
    cmd.handle = gamepad;
    if (EnqueueCommand(cmd)) {
        MarkChurnActive(500);
        return true;
    }
    return false;
}

void GamepadWorker_CancelOpen(SDL_JoystickID instance_id) {
    if (instance_id == 0 || !s_initialized.load()) {
        return;
    }

    WorkerCommand cmd{};
    cmd.type = CommandType::CancelOpen;
    cmd.instance_id = instance_id;
    EnqueueCommand(cmd);
}

bool GamepadWorker_TryPopResult(GamepadWorkerResult* out) {
    if (!out) {
        return false;
    }

    std::lock_guard<std::mutex> resultLock(s_resultMutex);
    if (s_results.empty()) {
        return false;
    }

    *out = s_results.front();
    s_results.pop_front();

    std::lock_guard<std::mutex> statsLock(s_statsMutex);
    UpdateResultDepthLocked();
    return true;
}

void GamepadWorker_GetStats(GamepadWorkerStats* out) {
    if (!out) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_statsMutex);
    *out = s_stats;
}

bool GamepadWorker_IsChurnActive() {
    if (!s_initialized.load()) {
        return false;
    }

    const DWORD until = s_churnActiveUntilMs.load(std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    return until != 0 && static_cast<int32_t>(until - now) >= 0;
}

} // namespace Input
