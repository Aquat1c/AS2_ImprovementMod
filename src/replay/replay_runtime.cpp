#include "replay/replay_runtime.h"

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "core/mod_main.h"
#include "input/input_system.h"
#include "patches/memory_utils.h"
#include "patches/tick_hooks.h"
#include "rollback/game_snapshot.h"
#include "rollback/resimulation.h"
#include "ui/log_window.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <windows.h>

namespace Replay {

namespace {

namespace fs = std::filesystem;

constexpr uint16_t kReplayInputMask =
    INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT |
    INPUT_A | INPUT_B | INPUT_C | INPUT_D;
constexpr size_t kReplayHeaderSize = 0x48;
constexpr size_t kReplayTapeSize = static_cast<size_t>(INPUT_HISTORY_MAX) * 2;
constexpr size_t kReplayPlayerBlockSize = 22;
constexpr int32_t kReplayMenuSelectSubstate = 2;
constexpr int32_t kReplayMenuFadeOutSubstate = 3;
constexpr int32_t kCoarseCheckpointInterval = 600;
constexpr int32_t kSeekFramesPerHotkey = 60;
constexpr int32_t kTakeoverCountdownFrames = 60;
constexpr int32_t kReplayBrowserPageSize = 12;
constexpr float kSeekScale = 16.0f;
constexpr float kSpeedSteps[] = {0.5f, 1.0f, 1.25f, 1.5f, 2.0f, 4.0f};

constexpr int kHotkeyPause = VK_OEM_5;
constexpr int kHotkeyStepForward = VK_OEM_6;
constexpr int kHotkeyStepBackward = VK_OEM_4;
constexpr int kHotkeySpeedSlower = VK_OEM_MINUS;
constexpr int kHotkeySpeedFaster = VK_OEM_PLUS;
constexpr int kHotkeyTakeoverP1 = '1';
constexpr int kHotkeyTakeoverP2 = '2';
constexpr int kHotkeyTakeoverExit = '0';

struct PendingPreparedInputs {
    bool valid = false;
    int32_t frame = -1;
    TakeoverMode takeover_mode = TakeoverMode::None;
    uint16_t p1 = 0;
    uint16_t p2 = 0;
};

struct ReplayFileMetadata {
    bool valid = false;
    std::array<uint8_t, kReplayHeaderSize> header{};
    uint32_t p1_char = 0;
    uint32_t p2_char = 0;
    int32_t frames = 0;
    std::string modified_time;
};

struct ReplayBrowserEntry {
    fs::path full_path;
    std::string relative_path;
    ReplayFileMetadata metadata;
};

static bool s_initialized = false;
static bool s_replayMatchActive = false;
static bool s_paused = false;
static bool s_savedPauseBlocked = false;
static int32_t s_currentFrame = -1;
static int32_t s_totalFrames = 0;
static int32_t s_seekTargetFrame = -1;
static int32_t s_takeoverStartFrame = -1;
static int32_t s_takeoverCountdownRemaining = 0;
static int32_t s_lastLoggedFrame = -2;
static int s_speedIndex = 1;
static int s_stepBudget = 0;
static float s_savedGlobalTickScale = 1.0f;
static TakeoverMode s_takeoverMode = TakeoverMode::None;
static bool s_takeoverActive = false;
static PendingPreparedInputs s_pendingPreparedInputs{};
static std::unique_ptr<Rollback::GameSnapshot> s_takeoverStartSnapshot;
static std::map<int32_t, std::unique_ptr<Rollback::GameSnapshot>> s_coarseCheckpoints;
static std::map<int32_t, uint16_t> s_overrideP1;
static std::map<int32_t, uint16_t> s_overrideP2;

static std::vector<ReplayBrowserEntry> s_browserEntries;
static int32_t s_browserSelected = 0;
static int32_t s_browserScroll = 0;
static bool s_browserNeedsScan = true;
static bool s_browserMenuWasActive = false;
static std::string s_browserStatus;

static bool s_pauseKeyWasDown = false;
static bool s_stepForwardKeyWasDown = false;
static bool s_stepBackwardKeyWasDown = false;
static bool s_speedSlowerKeyWasDown = false;
static bool s_speedFasterKeyWasDown = false;
static bool s_takeoverP1KeyWasDown = false;
static bool s_takeoverP2KeyWasDown = false;
static bool s_takeoverExitKeyWasDown = false;

static bool s_menuUpWasDown = false;
static bool s_menuDownWasDown = false;
static bool s_menuPageUpWasDown = false;
static bool s_menuPageDownWasDown = false;
static bool s_menuHomeWasDown = false;
static bool s_menuEndWasDown = false;
static bool s_menuConfirmWasDown = false;
static bool s_menuCancelWasDown = false;
static bool s_menuBackWasDown = false;

static bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool ConsumeEdge(int vk, bool* wasDown) {
    const bool down = KeyDown(vk);
    const bool pressed = down && !*wasDown;
    *wasDown = down;
    return pressed;
}

static void ResetPreparedInputs() {
    s_pendingPreparedInputs = {};
}

static void ClearInjectedInputsOnly() {
    InputSystem_ClearNetplayInput(0);
    InputSystem_ClearNetplayInput(1);
}

static void ClearReplayDispatcherState() {
    ClearInjectedInputsOnly();
    ResetPreparedInputs();
}

static void ResetMatchHotkeyEdges() {
    s_pauseKeyWasDown = KeyDown(kHotkeyPause);
    s_stepForwardKeyWasDown = KeyDown(kHotkeyStepForward);
    s_stepBackwardKeyWasDown = KeyDown(kHotkeyStepBackward);
    s_speedSlowerKeyWasDown = KeyDown(kHotkeySpeedSlower);
    s_speedFasterKeyWasDown = KeyDown(kHotkeySpeedFaster);
    s_takeoverP1KeyWasDown = KeyDown(kHotkeyTakeoverP1);
    s_takeoverP2KeyWasDown = KeyDown(kHotkeyTakeoverP2);
    s_takeoverExitKeyWasDown = KeyDown(kHotkeyTakeoverExit);
}

static void ResetMenuHotkeyEdges() {
    s_menuUpWasDown = KeyDown(VK_UP);
    s_menuDownWasDown = KeyDown(VK_DOWN);
    s_menuPageUpWasDown = KeyDown(VK_PRIOR);
    s_menuPageDownWasDown = KeyDown(VK_NEXT);
    s_menuHomeWasDown = KeyDown(VK_HOME);
    s_menuEndWasDown = KeyDown(VK_END);
    s_menuConfirmWasDown = KeyDown(VK_RETURN);
    s_menuCancelWasDown = KeyDown(VK_ESCAPE);
    s_menuBackWasDown = KeyDown(VK_BACK);
}

static void ResetMatchRuntimeState() {
    s_replayMatchActive = false;
    s_paused = false;
    s_savedPauseBlocked = false;
    s_currentFrame = -1;
    s_totalFrames = 0;
    s_seekTargetFrame = -1;
    s_takeoverStartFrame = -1;
    s_takeoverCountdownRemaining = 0;
    s_lastLoggedFrame = -2;
    s_speedIndex = 1;
    s_stepBudget = 0;
    s_savedGlobalTickScale = 1.0f;
    s_takeoverMode = TakeoverMode::None;
    s_takeoverActive = false;
    s_takeoverStartSnapshot.reset();
    s_coarseCheckpoints.clear();
    s_overrideP1.clear();
    s_overrideP2.clear();
    ClearReplayDispatcherState();
}

static void ResetBrowserState() {
    s_browserEntries.clear();
    s_browserSelected = 0;
    s_browserScroll = 0;
    s_browserNeedsScan = true;
    s_browserMenuWasActive = false;
    s_browserStatus.clear();
}

static bool IsReplayMenuContext() {
    return GetGameMode() == MODE_REPLAY_SELECT;
}

static bool IsReplayMenuSelectContext() {
    return IsReplayMenuContext() && GetSubstate() == kReplayMenuSelectSubstate;
}

static bool IsReplayMatchContext() {
    if (GetGameType() != GAMETYPE_REPLAY) {
        return false;
    }

    if (GetGameMode() != MODE_MATCH) {
        return false;
    }

    const uint32_t substate = GetSubstate();
    return substate == MATCH_SUB_INIT ||
           substate == MATCH_SUB_GAMEPLAY ||
           substate == MATCH_SUB_PAUSE;
}

static const char* TakeoverModeName(TakeoverMode mode) {
    switch (mode) {
        case TakeoverMode::P1: return "P1";
        case TakeoverMode::P2: return "P2";
        case TakeoverMode::Both: return "BOTH";
        default: return "NONE";
    }
}

static void SetBrowserStatus(const char* format, ...) {
    char buffer[256] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_browserStatus = buffer;
}

static uint32_t ReadU32(const uint8_t* data) {
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

static uint16_t ReadU16(const uint8_t* data) {
    uint16_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

static std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

static std::string NormalizeDisplayPath(const fs::path& path) {
    std::string result = WideToUtf8(path.wstring());
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

static std::string MakeSortKey(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    return text;
}

static std::string FormatLastWriteTime(const fs::path& path) {
    std::error_code ec;
    const fs::file_time_type fileTime = fs::last_write_time(path, ec);
    if (ec) {
        return {};
    }

    const auto systemNow = std::chrono::system_clock::now();
    const auto fileNow = fs::file_time_type::clock::now();
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - fileNow + systemNow);

    const std::time_t rawTime = std::chrono::system_clock::to_time_t(systemTime);
    std::tm localTime = {};
    if (localtime_s(&localTime, &rawTime) != 0) {
        return {};
    }

    char buffer[32] = {};
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return buffer;
}

static bool IsReplayExtension(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return ext == L".rep";
}

static bool ReadReplayMetadata(const fs::path& path, ReplayFileMetadata* outMetadata) {
    if (!outMetadata) {
        return false;
    }

    *outMetadata = {};
    outMetadata->modified_time = FormatLastWriteTime(path);

    std::error_code ec;
    const uintmax_t fileSize = fs::file_size(path, ec);
    if (ec || fileSize < kReplayHeaderSize) {
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    stream.read(reinterpret_cast<char*>(outMetadata->header.data()), static_cast<std::streamsize>(outMetadata->header.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(outMetadata->header.size())) {
        return false;
    }

    const uint32_t magic = ReadU32(outMetadata->header.data() + 0x00);
    const uint32_t storedFrames = ReadU32(outMetadata->header.data() + 0x44);
    const uintmax_t availableFrames = (fileSize - kReplayHeaderSize) / 2;

    if (magic != 258 || storedFrames > INPUT_HISTORY_MAX || availableFrames < storedFrames) {
        return false;
    }

    outMetadata->valid = true;
    outMetadata->p1_char = ReadU32(outMetadata->header.data() + 0x04);
    outMetadata->p2_char = ReadU32(outMetadata->header.data() + 0x08);
    outMetadata->frames = static_cast<int32_t>(storedFrames);
    return true;
}

static void ClampBrowserSelection() {
    if (s_browserEntries.empty()) {
        s_browserSelected = 0;
        s_browserScroll = 0;
        return;
    }

    s_browserSelected = std::clamp(s_browserSelected, 0, static_cast<int32_t>(s_browserEntries.size() - 1));
    const int32_t maxScroll = (std::max)(0, static_cast<int32_t>(s_browserEntries.size()) - kReplayBrowserPageSize);
    if (s_browserSelected < s_browserScroll) {
        s_browserScroll = s_browserSelected;
    } else if (s_browserSelected >= s_browserScroll + kReplayBrowserPageSize) {
        s_browserScroll = s_browserSelected - kReplayBrowserPageSize + 1;
    }
    s_browserScroll = std::clamp(s_browserScroll, 0, maxScroll);
}

static void ScanReplayBrowser() {
    const std::string previousPath =
        (s_browserSelected >= 0 && s_browserSelected < static_cast<int32_t>(s_browserEntries.size()))
            ? s_browserEntries[s_browserSelected].relative_path
            : std::string();

    s_browserEntries.clear();
    s_browserSelected = 0;
    s_browserScroll = 0;
    s_browserStatus.clear();

    const fs::path root = fs::path(L"replay");
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        SetBrowserStatus("No replay directory found at replay/.");
        s_browserNeedsScan = false;
        return;
    }

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const fs::directory_entry entry = *it;
        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        if (!IsReplayExtension(entry.path())) {
            continue;
        }

        ReplayBrowserEntry browserEntry{};
        browserEntry.full_path = entry.path();

        fs::path relativePath = browserEntry.full_path.lexically_relative(root);
        if (relativePath.empty()) {
            relativePath = browserEntry.full_path.filename();
        }
        browserEntry.relative_path = NormalizeDisplayPath(relativePath);
        ReadReplayMetadata(browserEntry.full_path, &browserEntry.metadata);
        s_browserEntries.push_back(std::move(browserEntry));
    }

    std::sort(s_browserEntries.begin(), s_browserEntries.end(), [](const ReplayBrowserEntry& lhs, const ReplayBrowserEntry& rhs) {
        return MakeSortKey(lhs.relative_path) < MakeSortKey(rhs.relative_path);
    });

    if (!previousPath.empty()) {
        for (int32_t i = 0; i < static_cast<int32_t>(s_browserEntries.size()); i++) {
            if (s_browserEntries[i].relative_path == previousPath) {
                s_browserSelected = i;
                break;
            }
        }
    }

    ClampBrowserSelection();
    s_browserNeedsScan = false;

    if (s_browserEntries.empty()) {
        SetBrowserStatus("No replay files found under replay/.");
    } else {
        LOG_INFO("[Replay] Browser scanned %zu replay file(s)", s_browserEntries.size());
    }
}

static void EnsureBrowserScanned() {
    if (s_browserNeedsScan) {
        ScanReplayBrowser();
    }
}

static int32_t ClampReplayFrame(int32_t frame) {
    if (s_totalFrames <= 0) {
        return -1;
    }

    if (frame < -1) {
        return -1;
    }

    const int32_t maxFrame = s_totalFrames - 1;
    if (frame > maxFrame) {
        return maxFrame;
    }

    return frame;
}

static int32_t ReadObservedFrame() {
    const int32_t writeIdx = static_cast<int32_t>(ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX));
    return ClampReplayFrame(writeIdx - 1);
}

static uint16_t ReadBaseReplayInput(int player, int32_t frame) {
    if (frame < 0 || frame >= s_totalFrames) {
        return 0;
    }

    const uintptr_t addr = ADDR_REPLAY_INPUT_BASE + (frame * 2) + (player == 0 ? 0 : 1);
    return static_cast<uint16_t>(ReadMemory<uint8_t>(addr) & kReplayInputMask);
}

static uint16_t ReadLiveTakeoverInput() {
    return static_cast<uint16_t>(InputSystem_GetInput(0) & kReplayInputMask);
}

static void ApplySpeedScale() {
    float scale = kSpeedSteps[s_speedIndex];
    if (s_seekTargetFrame >= 0) {
        scale = kSeekScale;
    } else if (s_paused) {
        scale = 1.0f;
    }

    SetGlobalTickScale(scale);
}

static void EraseOverridesFrom(int32_t firstFrame) {
    s_overrideP1.erase(s_overrideP1.lower_bound(firstFrame), s_overrideP1.end());
    s_overrideP2.erase(s_overrideP2.lower_bound(firstFrame), s_overrideP2.end());
}

static void InvalidateCheckpointsFrom(int32_t firstFrame) {
    s_coarseCheckpoints.erase(s_coarseCheckpoints.lower_bound(firstFrame), s_coarseCheckpoints.end());
}

static void SetSparseOverride(std::map<int32_t, uint16_t>& overrides,
                              int32_t frame,
                              uint16_t effectiveInput,
                              uint16_t baseInput) {
    if (effectiveInput == baseInput) {
        overrides.erase(frame);
        return;
    }

    overrides[frame] = effectiveInput;
}

static void CommitPreparedOverrides(int32_t frame) {
    if (!s_pendingPreparedInputs.valid || s_pendingPreparedInputs.frame != frame) {
        return;
    }

    if (s_pendingPreparedInputs.takeover_mode == TakeoverMode::P1 ||
        s_pendingPreparedInputs.takeover_mode == TakeoverMode::Both) {
        SetSparseOverride(s_overrideP1, frame, s_pendingPreparedInputs.p1, ReadBaseReplayInput(0, frame));
    }

    if (s_pendingPreparedInputs.takeover_mode == TakeoverMode::P2 ||
        s_pendingPreparedInputs.takeover_mode == TakeoverMode::Both) {
        SetSparseOverride(s_overrideP2, frame, s_pendingPreparedInputs.p2, ReadBaseReplayInput(1, frame));
    }
}

static bool BuildEffectiveInputsForFrame(int32_t frame, PendingPreparedInputs* outPrepared) {
    if (!outPrepared) {
        return false;
    }

    *outPrepared = {};
    if (!s_replayMatchActive || frame < 0 || frame >= s_totalFrames) {
        return false;
    }

    const auto p1Override = s_overrideP1.find(frame);
    const auto p2Override = s_overrideP2.find(frame);
    uint16_t p1 = p1Override != s_overrideP1.end() ? p1Override->second : ReadBaseReplayInput(0, frame);
    uint16_t p2 = p2Override != s_overrideP2.end() ? p2Override->second : ReadBaseReplayInput(1, frame);
    TakeoverMode preparedTakeover = TakeoverMode::None;

    if (s_takeoverActive && s_seekTargetFrame < 0 && frame > s_takeoverStartFrame) {
        const uint16_t liveInput = ReadLiveTakeoverInput();
        if (s_takeoverMode == TakeoverMode::P1 || s_takeoverMode == TakeoverMode::Both) {
            p1 = liveInput;
            preparedTakeover = TakeoverMode::P1;
        }
        if (s_takeoverMode == TakeoverMode::P2 || s_takeoverMode == TakeoverMode::Both) {
            p2 = liveInput;
            preparedTakeover = preparedTakeover == TakeoverMode::P1 ? TakeoverMode::Both : TakeoverMode::P2;
        }
    }

    outPrepared->valid = true;
    outPrepared->frame = frame;
    outPrepared->takeover_mode = preparedTakeover;
    outPrepared->p1 = p1;
    outPrepared->p2 = p2;
    return true;
}

static void WriteReplayHistoryFrame(int32_t frame, uint16_t p1, uint16_t p2) {
    if (frame < 0 || frame >= INPUT_HISTORY_MAX) {
        return;
    }

    WriteMemory<uint16_t>(ADDR_P1_INPUT_HISTORY + (static_cast<uintptr_t>(frame) * sizeof(uint16_t)), p1);
    WriteMemory<uint16_t>(ADDR_P2_INPUT_HISTORY + (static_cast<uintptr_t>(frame) * sizeof(uint16_t)), p2);
}

static bool CaptureSnapshot(std::unique_ptr<Rollback::GameSnapshot>& outSnapshot, int32_t frame) {
    auto snapshot = std::make_unique<Rollback::GameSnapshot>();
    Rollback::GameSnapshot_Clear(snapshot.get());
    if (!Rollback::GameSnapshot_Capture(snapshot.get(), frame)) {
        return false;
    }

    outSnapshot = std::move(snapshot);
    return true;
}

static bool CaptureCoarseCheckpoint(int32_t frame, const char* reason) {
    if (frame < -1) {
        return false;
    }

    std::unique_ptr<Rollback::GameSnapshot> snapshot;
    if (!CaptureSnapshot(snapshot, frame)) {
        LOG_ERROR("[Replay] Failed to capture checkpoint at frame %d (%s)",
            frame,
            reason ? reason : "checkpoint");
        return false;
    }

    s_coarseCheckpoints[frame] = std::move(snapshot);
    LOG_INFO("[Replay] Captured checkpoint at frame %d (%s)", frame, reason ? reason : "checkpoint");
    return true;
}

static void AlignStateHistoryAfterRestore(int32_t frame) {
    Rollback::StateHistory_DiscardFramesAfter(frame);
    if (!Rollback::StateHistory_HasFrame(frame)) {
        Rollback::StateHistory_CaptureFrame(frame);
    }
}

static bool RestoreSnapshot(const Rollback::GameSnapshot& snapshot, int32_t frame, const char* reason) {
    if (!Rollback::GameSnapshot_Restore(&snapshot)) {
        LOG_ERROR("[Replay] Failed to restore snapshot at frame %d (%s)",
            frame,
            reason ? reason : "restore");
        return false;
    }

    AlignStateHistoryAfterRestore(frame);
    s_currentFrame = frame;
    s_seekTargetFrame = -1;
    s_stepBudget = 0;
    ClearReplayDispatcherState();
    LOG_INFO("[Replay] Restored frame %d (%s)", frame, reason ? reason : "restore");
    return true;
}

static void LogFrameAdvance(int32_t observedFrame) {
    if (observedFrame == s_lastLoggedFrame) {
        return;
    }

    s_lastLoggedFrame = observedFrame;
    if (observedFrame < 0 || (observedFrame % 120) != 0) {
        return;
    }

    LOG_INFO("[Replay] Frame=%d/%d paused=%d seeking=%d takeover=%s countdown=%d speed=%.2fx",
        observedFrame,
        s_totalFrames,
        s_paused ? 1 : 0,
        s_seekTargetFrame >= 0 ? 1 : 0,
        TakeoverModeName(s_takeoverMode),
        s_takeoverCountdownRemaining,
        s_seekTargetFrame >= 0 ? kSeekScale : kSpeedSteps[s_speedIndex]);
}

static void CaptureStableFrameState(int32_t observedFrame) {
    if (!Rollback::StateHistory_CaptureFrame(observedFrame)) {
        LOG_WARN("[Replay] Failed to capture recent state for frame %d", observedFrame);
    }

    if (observedFrame >= 0 && (observedFrame % kCoarseCheckpointInterval) == 0) {
        if (s_coarseCheckpoints.find(observedFrame) == s_coarseCheckpoints.end()) {
            CaptureCoarseCheckpoint(observedFrame, "periodic");
        }
    }
}

static void UpdateObservedFrame() {
    const int32_t observedFrame = ReadObservedFrame();
    if (observedFrame == s_currentFrame) {
        return;
    }

    if (observedFrame < s_currentFrame) {
        LOG_WARN("[Replay] Observed frame regressed unexpectedly: %d -> %d",
            s_currentFrame,
            observedFrame);
        s_currentFrame = observedFrame;
        ClearReplayDispatcherState();
        return;
    }

    if (observedFrame > s_currentFrame + 1) {
        LOG_WARN("[Replay] Observed frame skipped: %d -> %d",
            s_currentFrame,
            observedFrame);
    }

    CommitPreparedOverrides(observedFrame);
    ClearReplayDispatcherState();

    if (!s_takeoverActive && s_takeoverMode != TakeoverMode::None && s_takeoverCountdownRemaining > 0) {
        --s_takeoverCountdownRemaining;
        if (s_takeoverCountdownRemaining <= 0) {
            s_takeoverCountdownRemaining = 0;
            s_takeoverActive = true;
            LOG_INFO("[Replay] Takeover ACTIVE for %s at frame %d",
                TakeoverModeName(s_takeoverMode),
                observedFrame + 1);
        }
    }

    s_currentFrame = observedFrame;
    CaptureStableFrameState(observedFrame);
    LogFrameAdvance(observedFrame);

    if (s_stepBudget > 0) {
        s_stepBudget = 0;
    }

    if (s_seekTargetFrame >= 0 && observedFrame >= s_seekTargetFrame) {
        LOG_INFO("[Replay] Seek complete at frame %d", observedFrame);
        s_seekTargetFrame = -1;
        s_paused = true;
    }
}

static int32_t FindBestCheckpointFrame(int32_t targetFrame) {
    auto it = s_coarseCheckpoints.upper_bound(targetFrame);
    if (it == s_coarseCheckpoints.begin()) {
        return -1;
    }

    --it;
    return it->first;
}

static bool RewindToFrame(int32_t targetFrame, const char* reason) {
    targetFrame = ClampReplayFrame(targetFrame);
    if (targetFrame == s_currentFrame) {
        return true;
    }

    s_paused = true;
    s_seekTargetFrame = -1;
    s_stepBudget = 0;
    ClearReplayDispatcherState();

    if (Rollback::StateHistory_HasFrame(targetFrame) && Rollback::StateHistory_LoadFrame(targetFrame)) {
        Rollback::StateHistory_DiscardFramesAfter(targetFrame);
        s_currentFrame = targetFrame;
        LOG_INFO("[Replay] Rewound to recent frame %d (%s)", targetFrame, reason ? reason : "rewind");
        return true;
    }

    const int32_t checkpointFrame = FindBestCheckpointFrame(targetFrame);
    if (checkpointFrame < -1) {
        LOG_ERROR("[Replay] No checkpoint available for target frame %d", targetFrame);
        return false;
    }

    auto it = s_coarseCheckpoints.find(checkpointFrame);
    if (it == s_coarseCheckpoints.end() || !it->second) {
        LOG_ERROR("[Replay] Missing checkpoint storage for frame %d", checkpointFrame);
        return false;
    }

    if (!RestoreSnapshot(*it->second, checkpointFrame, reason)) {
        return false;
    }

    if (checkpointFrame != targetFrame) {
        s_seekTargetFrame = targetFrame;
        LOG_INFO("[Replay] Seeking from checkpoint %d to frame %d (%s)",
            checkpointFrame,
            targetFrame,
            reason ? reason : "seek");
    }

    return true;
}

static bool BeginTakeover(TakeoverMode mode) {
    if (!s_replayMatchActive || mode == TakeoverMode::None) {
        return false;
    }

    if (!CaptureSnapshot(s_takeoverStartSnapshot, s_currentFrame)) {
        LOG_ERROR("[Replay] Failed to capture takeover start snapshot at frame %d", s_currentFrame);
        return false;
    }

    s_takeoverStartFrame = s_currentFrame;
    s_takeoverMode = mode;
    s_takeoverActive = false;
    s_takeoverCountdownRemaining = kTakeoverCountdownFrames;
    s_paused = false;
    s_seekTargetFrame = -1;
    s_stepBudget = 0;
    EraseOverridesFrom(s_currentFrame + 1);
    InvalidateCheckpointsFrom(s_currentFrame + 1);
    ClearReplayDispatcherState();
    LOG_INFO("[Replay] Takeover armed for %s at frame %d (countdown=%d)",
        TakeoverModeName(mode),
        s_currentFrame,
        s_takeoverCountdownRemaining);
    return true;
}

static bool RetryTakeover() {
    if (!s_takeoverStartSnapshot || s_takeoverMode == TakeoverMode::None) {
        return false;
    }

    EraseOverridesFrom(s_takeoverStartFrame + 1);
    InvalidateCheckpointsFrom(s_takeoverStartFrame + 1);
    s_takeoverActive = false;
    s_takeoverCountdownRemaining = kTakeoverCountdownFrames;
    s_paused = false;
    if (!RestoreSnapshot(*s_takeoverStartSnapshot, s_takeoverStartFrame, "takeover retry")) {
        return false;
    }

    LOG_INFO("[Replay] Takeover retry from frame %d (%s)",
        s_takeoverStartFrame,
        TakeoverModeName(s_takeoverMode));
    return true;
}

static bool ExitTakeover() {
    if (!s_takeoverStartSnapshot || s_takeoverMode == TakeoverMode::None) {
        return false;
    }

    const int32_t restoreFrame = s_takeoverStartFrame;
    EraseOverridesFrom(restoreFrame + 1);
    InvalidateCheckpointsFrom(restoreFrame + 1);
    const bool restored = RestoreSnapshot(*s_takeoverStartSnapshot, restoreFrame, "takeover exit");
    s_takeoverMode = TakeoverMode::None;
    s_takeoverActive = false;
    s_takeoverCountdownRemaining = 0;
    s_takeoverStartFrame = -1;
    s_takeoverStartSnapshot.reset();
    s_paused = false;
    if (restored) {
        LOG_INFO("[Replay] Takeover exited to base replay at frame %d", restoreFrame);
    }
    return restored;
}

static void ChangeSpeed(int delta) {
    const int speedCount = static_cast<int>(sizeof(kSpeedSteps) / sizeof(kSpeedSteps[0]));
    const int nextIndex = std::clamp(s_speedIndex + delta, 0, speedCount - 1);
    if (nextIndex == s_speedIndex) {
        return;
    }

    s_speedIndex = nextIndex;
    LOG_INFO("[Replay] Speed set to %.2fx", kSpeedSteps[s_speedIndex]);
}

static void HandleMatchHotkeys() {
    const bool shiftDown = KeyDown(VK_SHIFT);

    if (ConsumeEdge(kHotkeyPause, &s_pauseKeyWasDown)) {
        s_paused = !s_paused;
        if (!s_paused) {
            s_seekTargetFrame = -1;
        }
        LOG_INFO("[Replay] %s at frame %d", s_paused ? "Paused" : "Resumed", s_currentFrame);
    }

    if (ConsumeEdge(kHotkeyStepForward, &s_stepForwardKeyWasDown)) {
        if (!s_paused) {
            s_paused = true;
        }
        s_stepBudget = 1;
        s_seekTargetFrame = -1;
        LOG_INFO("[Replay] Step forward armed from frame %d", s_currentFrame);
    }

    if (ConsumeEdge(kHotkeyStepBackward, &s_stepBackwardKeyWasDown)) {
        if (shiftDown) {
            RewindToFrame(s_currentFrame - kSeekFramesPerHotkey, "hotkey rewind");
        } else {
            RewindToFrame(s_currentFrame - 1, "frame-step backward");
        }
    }

    if (ConsumeEdge(kHotkeySpeedFaster, &s_speedFasterKeyWasDown)) {
        ChangeSpeed(+1);
    }

    if (ConsumeEdge(kHotkeySpeedSlower, &s_speedSlowerKeyWasDown)) {
        ChangeSpeed(-1);
    }

    if (ConsumeEdge(kHotkeyTakeoverP1, &s_takeoverP1KeyWasDown)) {
        if (s_takeoverMode == TakeoverMode::P1) {
            RetryTakeover();
        } else {
            BeginTakeover(TakeoverMode::P1);
        }
    }

    if (ConsumeEdge(kHotkeyTakeoverP2, &s_takeoverP2KeyWasDown)) {
        if (s_takeoverMode == TakeoverMode::P2) {
            RetryTakeover();
        } else {
            BeginTakeover(TakeoverMode::P2);
        }
    }

    if (ConsumeEdge(kHotkeyTakeoverExit, &s_takeoverExitKeyWasDown)) {
        ExitTakeover();
    }
}

static void TransitionReplayMenu(uint32_t resultValue) {
    WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_RESULT, resultValue);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, kReplayMenuFadeOutSubstate);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0);
}

static bool LaunchReplayEntry(const ReplayBrowserEntry& entry) {
    ReplayFileMetadata metadata;
    if (!ReadReplayMetadata(entry.full_path, &metadata) || !metadata.valid) {
        SetBrowserStatus("Cannot load invalid replay: %s", entry.relative_path.c_str());
        LOG_ERROR("[Replay] Invalid replay selected: %s", entry.relative_path.c_str());
        return false;
    }

    std::ifstream stream(entry.full_path, std::ios::binary);
    if (!stream) {
        SetBrowserStatus("Failed to open replay: %s", entry.relative_path.c_str());
        LOG_ERROR("[Replay] Failed to open replay: %s", entry.relative_path.c_str());
        return false;
    }

    stream.read(reinterpret_cast<char*>(metadata.header.data()), static_cast<std::streamsize>(metadata.header.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(metadata.header.size())) {
        SetBrowserStatus("Failed to read replay header: %s", entry.relative_path.c_str());
        LOG_ERROR("[Replay] Failed to read replay header: %s", entry.relative_path.c_str());
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(metadata.frames) * 2;
    std::vector<uint8_t> tape(frameBytes, 0);
    if (frameBytes > 0) {
        stream.read(reinterpret_cast<char*>(tape.data()), static_cast<std::streamsize>(tape.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(tape.size())) {
            SetBrowserStatus("Replay input data is truncated: %s", entry.relative_path.c_str());
            LOG_ERROR("[Replay] Replay input data truncated: %s", entry.relative_path.c_str());
            return false;
        }
    }

    if (!WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_REPLAY_HEADER_BASE),
                              metadata.header.data(),
                              metadata.header.size())) {
        SetBrowserStatus("Failed to write replay header into game memory.");
        return false;
    }

    if (frameBytes > 0 &&
        !WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_REPLAY_INPUT_BASE), tape.data(), tape.size())) {
        SetBrowserStatus("Failed to write replay input data into game memory.");
        return false;
    }

    if (frameBytes < kReplayTapeSize) {
        std::vector<uint8_t> zeros(kReplayTapeSize - frameBytes, 0);
        if (!WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_REPLAY_INPUT_BASE + frameBytes),
                                  zeros.data(),
                                  zeros.size())) {
            SetBrowserStatus("Failed to clear trailing replay input memory.");
            return false;
        }
    }

    WriteMemory<uint32_t>(ADDR_CHARSEL_P1_CHAR_ID, ReadU32(metadata.header.data() + 0x04));
    WriteMemory<uint32_t>(ADDR_CHARSEL_P2_CHAR_ID, ReadU32(metadata.header.data() + 0x08));
    WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_CHARSEL_P1_PALETTE),
                         metadata.header.data() + 0x0C,
                         kReplayPlayerBlockSize);
    WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_CHARSEL_P2_PALETTE),
                         metadata.header.data() + 0x22,
                         kReplayPlayerBlockSize);
    WriteMemory<uint16_t>(ADDR_CHARSEL_P1_VARIANT, ReadU16(metadata.header.data() + 0x38));
    WriteMemory<uint8_t>(ADDR_CHARSEL_P1_VARIANT_EXTRA, metadata.header[0x3A]);
    WriteMemory<uint16_t>(ADDR_CHARSEL_P2_VARIANT, ReadU16(metadata.header.data() + 0x3B));
    WriteMemory<uint8_t>(ADDR_CHARSEL_P2_VARIANT_EXTRA, metadata.header[0x3D]);
    WriteMemory<uint16_t>(ADDR_MATCH_CONFIG_FLAGS, ReadU16(metadata.header.data() + 0x3E));

    TransitionReplayMenu(0);
    ResetMenuHotkeyEdges();
    LOG_INFO("[Replay] Launching replay from browser: %s (%d frames)",
        entry.relative_path.c_str(),
        metadata.frames);
    return true;
}

static void CancelReplaySelection() {
    TransitionReplayMenu(1);
    ResetMenuHotkeyEdges();
    LOG_INFO("[Replay] Replay browser cancelled");
}

static void HandleReplayMenuInput() {
    EnsureBrowserScanned();

    if (ConsumeEdge(VK_UP, &s_menuUpWasDown)) {
        if (!s_browserEntries.empty()) {
            s_browserSelected = (s_browserSelected + static_cast<int32_t>(s_browserEntries.size()) - 1) %
                static_cast<int32_t>(s_browserEntries.size());
            ClampBrowserSelection();
        }
    }

    if (ConsumeEdge(VK_DOWN, &s_menuDownWasDown)) {
        if (!s_browserEntries.empty()) {
            s_browserSelected = (s_browserSelected + 1) % static_cast<int32_t>(s_browserEntries.size());
            ClampBrowserSelection();
        }
    }

    if (ConsumeEdge(VK_PRIOR, &s_menuPageUpWasDown)) {
        s_browserSelected -= kReplayBrowserPageSize;
        ClampBrowserSelection();
    }

    if (ConsumeEdge(VK_NEXT, &s_menuPageDownWasDown)) {
        s_browserSelected += kReplayBrowserPageSize;
        ClampBrowserSelection();
    }

    if (ConsumeEdge(VK_HOME, &s_menuHomeWasDown)) {
        s_browserSelected = 0;
        ClampBrowserSelection();
    }

    if (ConsumeEdge(VK_END, &s_menuEndWasDown)) {
        s_browserSelected = static_cast<int32_t>(s_browserEntries.size()) - 1;
        ClampBrowserSelection();
    }

    if (ConsumeEdge(VK_RETURN, &s_menuConfirmWasDown)) {
        if (s_browserEntries.empty()) {
            SetBrowserStatus("No replay files are available to load.");
        } else {
            LaunchReplayEntry(s_browserEntries[s_browserSelected]);
        }
    }

    const bool cancelPressed = ConsumeEdge(VK_ESCAPE, &s_menuCancelWasDown) ||
        ConsumeEdge(VK_BACK, &s_menuBackWasDown);
    if (cancelPressed) {
        CancelReplaySelection();
    }
}

static void UpdateReplayMenuContext() {
    const bool menuActive = IsReplayMenuSelectContext();
    if (menuActive && !s_browserMenuWasActive) {
        ResetBrowserState();
        ResetMenuHotkeyEdges();
    } else if (!menuActive && s_browserMenuWasActive) {
        ResetMenuHotkeyEdges();
    }

    s_browserMenuWasActive = menuActive;

    if (menuActive) {
        HandleReplayMenuInput();
    }
}

static void ActivateReplayMatch() {
    ResetMatchRuntimeState();
    s_replayMatchActive = true;
    s_savedGlobalTickScale = GetGlobalTickScale();
    s_savedPauseBlocked = InputSystem_IsPauseBlocked();
    s_totalFrames = static_cast<int32_t>(ReadMemory<uint32_t>(ADDR_REPLAY_ELEMENT_COUNT));
    s_currentFrame = ReadObservedFrame();
    InputSystem_SetPauseBlocked(true);
    ApplySpeedScale();
    ResetMatchHotkeyEdges();
    Rollback::StateHistory_Reset();
    CaptureCoarseCheckpoint(s_currentFrame, "match-start");
    AlignStateHistoryAfterRestore(s_currentFrame);
    LOG_INFO("[Replay] Activated: total_frames=%d start_frame=%d mode=%u sub=%u",
        s_totalFrames,
        s_currentFrame,
        GetGameMode(),
        GetSubstate());
}

static void DeactivateReplayMatch(const char* reason) {
    if (!s_replayMatchActive) {
        return;
    }

    LOG_INFO("[Replay] Deactivated at frame %d (%s)", s_currentFrame, reason ? reason : "inactive");
    ClearReplayDispatcherState();
    InputSystem_SetPauseBlocked(s_savedPauseBlocked);
    SetGlobalTickScale(s_savedGlobalTickScale);
    Rollback::StateHistory_Reset();
    ResetMatchRuntimeState();
    ResetMatchHotkeyEdges();
}

static void RenderReplayMatchHud() {
    if (!s_replayMatchActive) {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) {
        return;
    }

    char lines[7][128] = {};
    snprintf(lines[0], sizeof(lines[0]), "REPLAY %d / %d  %.2fx",
        (std::max)(s_currentFrame, 0),
        (std::max)(s_totalFrames - 1, 0),
        s_seekTargetFrame >= 0 ? kSeekScale : kSpeedSteps[s_speedIndex]);

    if (s_seekTargetFrame >= 0) {
        snprintf(lines[1], sizeof(lines[1]), "SEEKING -> %d", s_seekTargetFrame);
    } else if (s_paused) {
        snprintf(lines[1], sizeof(lines[1]), "PAUSED");
    } else {
        snprintf(lines[1], sizeof(lines[1]), "PLAYING");
    }

    if (s_takeoverMode != TakeoverMode::None) {
        if (s_takeoverActive) {
            snprintf(lines[2], sizeof(lines[2]), "TAKEOVER %s ACTIVE", TakeoverModeName(s_takeoverMode));
        } else {
            snprintf(lines[2], sizeof(lines[2]), "TAKEOVER %s IN %d", TakeoverModeName(s_takeoverMode), s_takeoverCountdownRemaining);
        }
    }

    snprintf(lines[3], sizeof(lines[3]), "\\ Pause  ] Step  [ Back  Shift+[ Rewind 60");
    snprintf(lines[4], sizeof(lines[4]), "= Faster  - Slower  1 P1 Takeover  2 P2 Takeover  0 Exit");
    snprintf(lines[5], sizeof(lines[5]), "Press the same takeover key again to retry from the takeover start.");

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float maxWidth = 0.0f;
    float lineHeight = 0.0f;
    int lineCount = 0;
    for (const auto& line : lines) {
        if (!line[0]) {
            continue;
        }
        const ImVec2 size = ImGui::CalcTextSize(line);
        maxWidth = (std::max)(maxWidth, size.x);
        lineHeight = (std::max)(lineHeight, size.y);
        ++lineCount;
    }

    if (lineCount == 0) {
        return;
    }

    const float pad = 8.0f * ModUI_GetScale();
    const float x = 12.0f * ModUI_GetScale();
    const float y = 28.0f * ModUI_GetScale();
    const float width = maxWidth + pad * 2.0f;
    const float height = lineCount * lineHeight + pad * 2.0f + (lineCount - 1) * 2.0f;

    drawList->AddRectFilled(
        ImVec2(x, y),
        ImVec2((std::min)(x + width, displaySize.x - 8.0f), y + height),
        IM_COL32(0, 0, 0, 170),
        6.0f);

    int lineIndex = 0;
    for (const auto& line : lines) {
        if (!line[0]) {
            continue;
        }
        const ImU32 color = (lineIndex == 1 && s_paused)
            ? IM_COL32(255, 255, 100, 255)
            : IM_COL32(235, 235, 235, 255);
        drawList->AddText(
            ImVec2(x + pad, y + pad + lineIndex * (lineHeight + 2.0f)),
            color,
            line);
        ++lineIndex;
    }
}

static void RenderReplayBrowserHud() {
    if (!IsReplayMenuSelectContext()) {
        return;
    }

    EnsureBrowserScanned();

    const float scale = ModUI_GetScale();
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(24.0f * scale, 24.0f * scale), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displaySize.x - 48.0f * scale, displaySize.y - 48.0f * scale), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove;

    if (!ImGui::Begin("Replay Browser", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Replay Browser");
    ImGui::SameLine();
    ImGui::TextDisabled("%d file(s)", static_cast<int>(s_browserEntries.size()));
    ImGui::Separator();

    const float detailsHeight = 118.0f * scale;
    const float statusHeight = 44.0f * scale;
    const float listHeight = (std::max)(80.0f * scale, ImGui::GetContentRegionAvail().y - detailsHeight - statusHeight);

    if (ImGui::BeginChild("ReplayList", ImVec2(0.0f, listHeight), true)) {
        if (s_browserEntries.empty()) {
            ImGui::TextUnformatted("No replay files were found under replay/.");
        } else {
            const int32_t listEnd = (std::min)(s_browserScroll + kReplayBrowserPageSize,
                static_cast<int32_t>(s_browserEntries.size()));
            for (int32_t i = s_browserScroll; i < listEnd; i++) {
                const ReplayBrowserEntry& entry = s_browserEntries[i];
                const bool selected = i == s_browserSelected;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 120, 255));
                } else if (!entry.metadata.valid) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 120, 120, 255));
                }

                char label[768] = {};
                snprintf(label, sizeof(label), "%c %s%s",
                    selected ? '>' : ' ',
                    entry.metadata.valid ? "" : "[invalid] ",
                    entry.relative_path.c_str());
                ImGui::TextUnformatted(label);

                if (selected || !entry.metadata.valid) {
                    ImGui::PopStyleColor();
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (!s_browserEntries.empty()) {
        const ReplayBrowserEntry& entry = s_browserEntries[s_browserSelected];
        ImGui::Text("Selected: %d / %d", s_browserSelected + 1, static_cast<int>(s_browserEntries.size()));
        ImGui::TextWrapped("Path: %s", entry.relative_path.c_str());
        if (entry.metadata.valid) {
            const int32_t seconds = entry.metadata.frames / 60;
            ImGui::Text("Frames: %d   Duration: %d:%02d", entry.metadata.frames, seconds / 60, seconds % 60);
            ImGui::Text("P1 #%u   P2 #%u", entry.metadata.p1_char, entry.metadata.p2_char);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "Replay header is invalid and cannot be launched.");
        }
        if (!entry.metadata.modified_time.empty()) {
            ImGui::Text("Modified: %s", entry.metadata.modified_time.c_str());
        }
    }

    if (!s_browserStatus.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.40f, 1.0f), "%s", s_browserStatus.c_str());
    } else {
        ImGui::TextUnformatted("Recursive scan includes nested folders and long filenames.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Up/Down Select  PgUp/PgDn Page  Home/End Jump  Enter Load  Esc/Backspace Back");

    ImGui::End();
}

} // namespace

void ReplayRuntime_Init() {
    ResetMatchRuntimeState();
    ResetBrowserState();
    ResetMatchHotkeyEdges();
    ResetMenuHotkeyEdges();
    s_initialized = true;
    LOG_INFO("[Replay] Runtime initialized (\\ pause, ] step, [ back, Shift+[ rewind, 1/2 takeover)");
}

void ReplayRuntime_Shutdown() {
    if (!s_initialized) {
        return;
    }

    DeactivateReplayMatch("shutdown");
    ResetBrowserState();
    s_initialized = false;
    LOG_INFO("[Replay] Runtime shutdown");
}

void ReplayRuntime_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    UpdateReplayMenuContext();

    if (!IsReplayMatchContext()) {
        DeactivateReplayMatch("left replay match");
        return;
    }

    if (!s_replayMatchActive) {
        ActivateReplayMatch();
    }

    UpdateObservedFrame();
    HandleMatchHotkeys();
    ApplySpeedScale();
    ClearInjectedInputsOnly();
}

void ReplayRuntime_RenderHUD() {
    if (!s_initialized) {
        return;
    }

    RenderReplayMatchHud();
    RenderReplayBrowserHud();
}

bool ReplayRuntime_ShouldFreezeFrame() {
    return s_replayMatchActive && s_paused && s_seekTargetFrame < 0 && s_stepBudget == 0;
}

bool ReplayRuntime_IsReplayMatchActive() {
    return s_replayMatchActive;
}

bool ReplayRuntime_IsReplayMenuActive() {
    return IsReplayMenuSelectContext();
}

bool ReplayRuntime_ShouldConsumeMenuInput() {
    return IsReplayMenuSelectContext();
}

void ReplayRuntime_OnDispatcherAdvance(int16_t* outputInputs) {
    if (!s_replayMatchActive || !outputInputs) {
        return;
    }

    const int32_t dispatchedFrame = ReadObservedFrame();
    PendingPreparedInputs prepared{};
    if (!BuildEffectiveInputsForFrame(dispatchedFrame, &prepared)) {
        ClearReplayDispatcherState();
        return;
    }

    outputInputs[0] = static_cast<int16_t>(prepared.p1);
    outputInputs[1] = static_cast<int16_t>(prepared.p2);

    WriteReplayHistoryFrame(dispatchedFrame, prepared.p1, prepared.p2);
    InputSystem_SetNetplayInput(0, prepared.p1);
    InputSystem_SetNetplayInput(1, prepared.p2);
    s_pendingPreparedInputs = prepared;
}

} // namespace Replay
