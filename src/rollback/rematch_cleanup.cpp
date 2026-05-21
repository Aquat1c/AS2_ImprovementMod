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
#include "rollback/rollback_debug.h"
#include "core/as2_constants.h"
#include "core/game_state.h"
#include "ui/log_window.h"

namespace {

constexpr size_t kEffectStateBytes = EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE;
constexpr size_t kSummonStateBytes = SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE;

static_assert(kEffectStateBytes == (ADDR_SUMMON_ARRAY - ADDR_EFFECT_ARRAY),
    "Effect array size should span exactly to summon array");

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
    static const uint8_t zeroInputBuffer[INPUT_BUFFER_SIZE] = {};
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
        "Clearing input residue: game_buffers=%d/%d game_states=%d/%d "
        "overrideP1/P2, netplayP1/P2, repeat-state, pause-block",
        p1InputBufferCleared ? 1 : 0,
        p2InputBufferCleared ? 1 : 0,
        p1InputStateCleared ? 1 : 0,
        p2InputStateCleared ? 1 : 0);

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

void RematchCleanup_PrepareForNextMatch(const char* reason) {
    Rollback::NetplayLog_Write(
        "REMATCH", -1,
        "=== NEXT MATCH CLEANUP BEGIN: reason=%s ===",
        reason ? reason : "unspecified");
    LogBoundaryState("BeforeCleanup");

    if (Net::WinScreenSync_IsActive()) {
        Rollback::NetplayLog_Write("REMATCH", -1,
            "Resetting win screen sync state before next match");
        Net::WinScreenSync_Abort();
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
    Rollback::NetplayLog_Write("REMATCH", -1,
        "=== NEXT MATCH CLEANUP END ===");
    Rollback::NetplayLog_Flush();

    LOG_NETPLAY(LOG_INFO,
        "[RematchCleanup] Prepared next match boundary (%s)",
        reason ? reason : "unspecified");
}

} // namespace Rollback
