/**
 * Alice Senki 2 - Rematch Boundary Cleanup
 *
 * Centralizes the match-scoped reset work that is safe to perform when a
 * finished match hands back into the next front-end flow. The goal is to
 * remove rollback/input leftovers without touching live session ownership.
 */

#include "rollback/rematch_cleanup.h"

#include "input/input_system.h"
#include "net/charsel_sync.h"
#include "net/frontend_input_sync.h"
#include "net/match_lifecycle.h"
#include "net/pause_handler.h"
#include "net/pregame_sync.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/winscreen_sync.h"
#include "patches/memory_utils.h"
#include "patches/input_sync_hooks.h"
#include "rollback/netplay_log.h"
#include "rollback/owner_diagnostics.h"
#include "rollback/rollback_debug.h"
#include "core/as2_constants.h"
#include "core/game_state.h"
#include "ui/log_window.h"

#include <string.h>

namespace {

constexpr size_t kEffectStateBytes = EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE;
constexpr size_t kSummonStateBytes = SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE;
constexpr size_t kFrontendSafeInputBufferClearSize = ADDR_P1_INPUT_STATE - ADDR_P1_INPUT_BUFFER;
constexpr size_t kUnsafeInputTailSize =
    INPUT_BUFFER_SIZE - kFrontendSafeInputBufferClearSize - INPUT_STATE_SIZE;
constexpr uintptr_t kP1UnsafeInputTailStart = ADDR_P1_INPUT_STATE + INPUT_STATE_SIZE;
constexpr uintptr_t kP2UnsafeInputTailStart = ADDR_P2_INPUT_STATE + INPUT_STATE_SIZE;

static_assert(kEffectStateBytes == (ADDR_SUMMON_ARRAY - ADDR_EFFECT_ARRAY),
    "Effect array size should span exactly to summon array");
static_assert(kFrontendSafeInputBufferClearSize == 56,
    "Frontend-safe input clear must stop before shared/CharSel/player data");
static_assert(ADDR_P2_INPUT_STATE - ADDR_P2_INPUT_BUFFER == kFrontendSafeInputBufferClearSize,
    "P2 frontend-safe clear size must match P1");
static_assert(INPUT_STATE_SIZE == 20,
    "Input state size changed; verify frontend cleanup");
static_assert(kFrontendSafeInputBufferClearSize < INPUT_BUFFER_SIZE,
    "Frontend-safe clear must be smaller than the full match input buffer span");
static_assert(kUnsafeInputTailSize == 132,
    "Unsafe input tail size should cover the bytes the old 208-byte clear would have corrupted");

struct InputTailSnapshot {
    bool p1_readable;
    bool p2_readable;
    uint32_t p1_crc;
    uint32_t p2_crc;
    uint8_t p1[kUnsafeInputTailSize];
    uint8_t p2[kUnsafeInputTailSize];
};

static uint32_t CaptureTailCrc(uintptr_t addr,
                               uint8_t* out,
                               size_t size,
                               bool* outReadable) {
    const bool readable = CopyMemorySafe(out, reinterpret_cast<const void*>(addr), size);
    if (outReadable) {
        *outReadable = readable;
    }
    return readable ? CalcCRC32(out, size) : 0;
}

static InputTailSnapshot CaptureInputTailSnapshot() {
    InputTailSnapshot snap{};
    snap.p1_crc = CaptureTailCrc(
        kP1UnsafeInputTailStart,
        snap.p1,
        sizeof(snap.p1),
        &snap.p1_readable);
    snap.p2_crc = CaptureTailCrc(
        kP2UnsafeInputTailStart,
        snap.p2,
        sizeof(snap.p2),
        &snap.p2_readable);
    return snap;
}

static bool TailChanged(const InputTailSnapshot& before,
                        const InputTailSnapshot& after,
                        int player) {
    const bool beforeReadable = (player == 0) ? before.p1_readable : before.p2_readable;
    const bool afterReadable = (player == 0) ? after.p1_readable : after.p2_readable;
    if (!beforeReadable || !afterReadable) {
        return false;
    }

    const uint8_t* beforeBytes = (player == 0) ? before.p1 : before.p2;
    const uint8_t* afterBytes = (player == 0) ? after.p1 : after.p2;
    return memcmp(beforeBytes, afterBytes, kUnsafeInputTailSize) != 0;
}

static void LogOwnerAndTail(const char* label, const InputTailSnapshot& tail) {
    Rollback::OwnerDiag_LogSnapshot(
        label,
        Rollback::OwnerDiag_Capture(),
        true,
        tail.p1_crc,
        tail.p2_crc,
        tail.p1_readable,
        tail.p2_readable);
}

static void LogBoundaryState(const char* label) {
    Net::SessionSnapshot session{};
    Net::Session_GetSnapshot(&session);

    Net::PregameSnapshot pregame{};
    Net::PregameSync_GetSnapshot(&pregame);

    Net::MatchLifecycleSnapshot lifecycle{};
    Net::MatchLifecycle_GetSnapshot(&lifecycle);

    Rollback::NetplayLog_Write(
        "REMATCH", -1,
        "%s: mode=%u sub=%u type=%u session=%s role=%s pregame=%s lifecycle=%s "
        "charsel_active=%d winscreen_active=%d winscreen_local=%d winscreen_remote=%d "
        "pause_tracked=%d pause_was_quit=%d netplay_p1=%d netplay_p2=%d pause_blocked=%d "
        "load_freeze=%d timesync_freeze=%d control_swap=%d",
        label ? label : "state",
        GetGameMode(),
        GetSubstate(),
        GetGameType(),
        Net::SessionStateName(session.state),
        Net::SessionRoleName(session.role),
        Net::PregamePhaseName(pregame.phase),
        Net::MatchLifecyclePhaseName(lifecycle.phase),
        Net::CharSelSync_IsLockstepActive() ? 1 : 0,
        Net::WinScreenSync_IsActive() ? 1 : 0,
        Net::WinScreenSync_LocalConfirmed() ? 1 : 0,
        Net::WinScreenSync_RemoteConfirmed() ? 1 : 0,
        Net::PauseHandler_IsPauseTracked() ? 1 : 0,
        Net::PauseHandler_WasQuit() ? 1 : 0,
        InputSystem_IsNetplayInputActive(0) ? 1 : 0,
        InputSystem_IsNetplayInputActive(1) ? 1 : 0,
        InputSystem_IsPauseBlocked() ? 1 : 0,
        InputSyncHooks_IsLoadBarrierFrozen() ? 1 : 0,
        InputSyncHooks_IsTimesyncFrozen() ? 1 : 0,
        InputSystem_GetControlSwap() ? 1 : 0);
}

static void ClearInputResidue() {
    const InputTailSnapshot beforeTail = CaptureInputTailSnapshot();
    LogOwnerAndTail("before_cleanup", beforeTail);

    static const uint8_t zeroInputBuffer[kFrontendSafeInputBufferClearSize] = {};
    static const uint8_t zeroInputState[INPUT_STATE_SIZE] = {};

    const bool p1InputBufferCleared =
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_BUFFER, zeroInputBuffer, sizeof(zeroInputBuffer));
    const bool p2InputBufferCleared =
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_BUFFER, zeroInputBuffer, sizeof(zeroInputBuffer));
    const bool p1InputStateCleared =
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_STATE, zeroInputState, sizeof(zeroInputState));
    const bool p2InputStateCleared =
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_STATE, zeroInputState, sizeof(zeroInputState));

    Rollback::NetplayLog_Write(
        "REMATCH", -1,
        "ClearInputResidue frontend_safe bytes=%u states=%u p1Buf=%d p2Buf=%d "
        "p1State=%d p2State=%d full_clear=0 unsafe_tail_bytes=%u",
        (unsigned)sizeof(zeroInputBuffer),
        (unsigned)sizeof(zeroInputState),
        p1InputBufferCleared ? 1 : 0,
        p2InputBufferCleared ? 1 : 0,
        p1InputStateCleared ? 1 : 0,
        p2InputStateCleared ? 1 : 0,
        (unsigned)kUnsafeInputTailSize);

    InputSystem_ClearOverride(0);
    InputSystem_ClearOverride(1);
    InputSystem_ClearNetplayInput(0);
    InputSystem_ClearNetplayInput(1);
    InputSystem_ResetRepeatState(0);
    InputSystem_ResetRepeatState(1);
    InputSystem_SetPauseBlocked(false);
    if (InputSystem_GetControlSwap()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "Clearing stale control swap at rematch boundary");
    }
    InputSystem_SetControlSwap(false);

    const InputTailSnapshot afterTail = CaptureInputTailSnapshot();
    LogOwnerAndTail("after_cleanup", afterTail);

    const bool p1TailChanged = TailChanged(beforeTail, afterTail, 0);
    const bool p2TailChanged = TailChanged(beforeTail, afterTail, 1);
    Rollback::NetplayLog_Write(
        "REMATCH", -1,
        "INPUT_TAIL_GUARD %s label=ClearInputResidue p1_crc_before=0x%08X p1_crc_after=0x%08X "
        "p2_crc_before=0x%08X p2_crc_after=0x%08X safe_clear_bytes=%u old_full_clear_bytes=%u",
        (p1TailChanged || p2TailChanged) ? "ERROR" : "ok",
        beforeTail.p1_crc,
        afterTail.p1_crc,
        beforeTail.p2_crc,
        afterTail.p2_crc,
        (unsigned)kFrontendSafeInputBufferClearSize,
        (unsigned)INPUT_BUFFER_SIZE);
    Rollback::NetplayLog_Write(
        "OWNERCHK", -1,
        "tail_guard after_cleanup p1_changed=%d p2_changed=%d p1_crc=0x%08X->0x%08X "
        "p2_crc=0x%08X->0x%08X safe_clear_bytes=%u old_full_clear_bytes=%u",
        p1TailChanged ? 1 : 0,
        p2TailChanged ? 1 : 0,
        beforeTail.p1_crc,
        afterTail.p1_crc,
        beforeTail.p2_crc,
        afterTail.p2_crc,
        (unsigned)kFrontendSafeInputBufferClearSize,
        (unsigned)INPUT_BUFFER_SIZE);

    if (p1TailChanged || p2TailChanged) {
        Rollback::NetplayLog_Write(
            "OWNERCHK", -1,
            "ERROR rematch cleanup changed unsafe input tail bytes: p1_changed=%d p2_changed=%d",
            p1TailChanged ? 1 : 0,
            p2TailChanged ? 1 : 0);
    }
}

static void ClearMatchVolatileResidue() {
    static const uint8_t zeroEffects[kEffectStateBytes] = {};
    static const uint8_t zeroSummons[kSummonStateBytes] = {};

    const bool effectsCleared =
        WriteMemoryBlockSafe((void*)ADDR_EFFECT_ARRAY, zeroEffects, sizeof(zeroEffects));
    const bool summonsCleared =
        WriteMemoryBlockSafe((void*)ADDR_SUMMON_ARRAY, zeroSummons, sizeof(zeroSummons));

    Rollback::NetplayLog_Write(
        "REMATCH", -1,
        "Clearing stale match-owned volatile state: effects=%d summons=%d preserve_effect_index=1 preserve_frame_counters=1",
        effectsCleared ? 1 : 0,
        summonsCleared ? 1 : 0);
}

} // anonymous namespace

namespace Rollback {

static void PrepareMatchBoundaryForNextFlow(const char* banner, const char* reason) {
    Rollback::NetplayLog_Write(
        "REMATCH", -1,
        "=== %s BEGIN: reason=%s ===",
        banner ? banner : "MATCH BOUNDARY CLEANUP",
        reason ? reason : "unspecified");
    Rollback::OwnerDiag_Log("rematch_cleanup_begin");
    LogBoundaryState("BeforeCleanup");

    if (Net::WinScreenSync_IsActive()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "Resetting win screen sync state before next match");
        Net::WinScreenSync_Abort();
    } else {
        Net::FrontendInputSync_StopWinScreenInputPhase("rematch cleanup reset");
    }

    if (Net::PauseHandler_IsPauseTracked()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "Pause handler still tracking a pause at rematch boundary; forcing resume cleanup");
        Net::PauseHandler_OnPauseExit(false);
    }

    if (Net::PregameSync_IsActive()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "Pregame sync unexpectedly active at rematch boundary; aborting stale state");
        Net::PregameSync_Abort("rematch cleanup reset");
    } else if (Net::CharSelSync_IsLockstepActive()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "CharSel lockstep still active without PregameSync owning it; aborting stale lockstep");
        Net::CharSelSync_Abort();
    }

    Net::FrontendInputSync_AbortEpoch("rematch cleanup reset");

    if (InputSyncHooks_IsLoadBarrierFrozen()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "Clearing stale load barrier freeze at rematch boundary");
    }
    InputSyncHooks_SetLoadBarrierFreeze(false);

    if (InputSyncHooks_IsTimesyncFrozen()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "Clearing stale timesync freeze at rematch boundary");
    }
    InputSyncHooks_SetTimesyncFreeze(false);

    Rollback::NetplayLog_Write("REMATCH", -1,
        "Disabling digest/debug emission for the finished match");
    Rollback::RollbackDebug_SetDigestEnabled(false);
    Rollback::RollbackDebug_ResetSession();

    ClearMatchVolatileResidue();
    ClearInputResidue();

    LogBoundaryState("AfterCleanup");
    Rollback::OwnerDiag_Log("rematch_cleanup_end");
    Rollback::NetplayLog_Write("REMATCH", -1,
        "=== %s END ===",
        banner ? banner : "MATCH BOUNDARY CLEANUP");
    Rollback::NetplayLog_Flush();

    LOG_NETPLAY(LOG_INFO,
        "[RematchCleanup] Prepared match boundary (%s): %s",
        banner ? banner : "cleanup",
        reason ? reason : "unspecified");
}

void RematchCleanup_PrepareForNextMatch(const char* reason) {
    PrepareMatchBoundaryForNextFlow("NEXT MATCH CLEANUP", reason);
}

void RematchCleanup_PrepareForNetplayLaunch(const char* reason) {
    PrepareMatchBoundaryForNextFlow("NETPLAY LAUNCH CLEANUP", reason);
}

} // namespace Rollback
