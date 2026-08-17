/**
 * Alice Senki 2 - Online Rollback Wiring Implementation
 *
 * This is the integration glue that connects all subsystems:
 *   - Bootstrap → RollbackSession handoff
 *   - Engine-facing packet sinks (dispatch lives in net/packet_router)
 *   - Lifecycle phase → rollback start/stop/pause
 *   - Disconnect → safe teardown
 *   - Post-match → clean handoff
 *   - Full-path logging throughout
 *   - Stress hook integration
 */

#include "rollback/online_wiring.h"
#include "rollback/rollback_session.h"
#include "rollback/rollback_debug.h"
#include "rollback/savestate.h"
#include "rollback/netplay_log.h"
#include "rollback/rematch_cleanup.h"
#include "rollback/stress_hooks.h"
#include "rollback/resimulation.h"
#include "rollback/determinism_verify.h"
#include "patches/input_sync_hooks.h"
#include "net/gameplay_bridge.h"
#include "net/packet_router.h"
#include "net/match_lifecycle.h"
#include "net/set_tracker.h"
#include "net/pregame_sync.h"
#include "net/frontend_input_sync.h"
#include "net/session_manager.h"
#include "net/connection_supervisor.h"
#include "net/transition_barrier.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/sync_policy.h"
#include "net/sync_trace.h"
#include "net/delay_policy.h"
#include "net/locked_match_config.h"
#include "net/enet_transport.h"
#include "net/netplay_pacing.h"
#include "net/churn_pause.h"
#include "net/game_settings_sync.h"
#include "net/player_side_mapping.h"
#include "net/charsel_sync.h"
#include "net/winscreen_sync.h"
#include "net/pause_handler.h"
#include "net/spectator_runtime.h"
#include "net/netplay_palette_runtime.h"
#include "patches/charsel_palette_select.h"
#include "input/input_system.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"

#include <string.h>
#include <stdio.h>
#include <windows.h>

namespace Rollback {

// ============================================================================
// Internal State
// ============================================================================

static bool     s_initialized            = false;
static bool     s_rollbackStarted        = false;   // Has rollback started this match
static bool     s_rollbackActive         = false;   // Is rollback running right now
static bool     s_gameplayActive         = false;   // Currently in the interactive/pacing phase
static bool     s_liveReleaseArmed       = false;   // Waiting for post-intro interactive release
static int32_t  s_frameOriginAbs         = -1;
static uint32_t s_baselineCRC            = 0;
static uint32_t s_configHash             = 0;
static int      s_handoffDelay           = 0;
static int      s_handoffBudget          = 0;
static int      s_remoteInputsReceived   = 0;
static int      s_packetsDispatched      = 0;
static uint32_t s_backgroundPollCount    = 0;
static uint32_t s_backgroundPollFailures = 0;

// Startup gameplay-entry barrier state (M5: rides TransitionBarrier kind
// GameplayStart — the 4-way propose/ack primitive that was modeled on the
// old GekkoReady exchange; GekkoReady itself is retired). Each peer proposes
// on reaching the FIRST interactive post-intro boundary; the commit
// (both proposed + acked) is the mutual release.
static bool     s_startupBarrierEntered    = false;
static bool     s_startupProposed          = false;  // local GameplayStart proposal sent
static bool     s_startupReleased          = false;  // gameplay-entry barrier released
static uint32_t s_startupBlockedLogCounter = 0;
static uint32_t s_introHoldLogCounter      = 0;

// Phase tracking for before/after logging
static Net::MatchLifecyclePhase s_lastLifecyclePhase = Net::MatchLifecyclePhase::Inactive;
static int      s_lastActiveDelay        = -1;
static int      s_lastRollbackBudget     = -1;

// Rollback start guard — prevent double-starting
static bool     s_rollbackBeginPending   = false;

static bool IsInteractiveRollbackPhase() {
    return s_rollbackActive &&
           Net::NetplayPhaseRuntime_IsInteractivePacingPhase(
               Net::NetplayPhaseRuntime_GetPhase());
}

static int32_t GetCurrentGameAbsFrameForLogs() {
    if (s_rollbackActive) {
        return RollbackSession_GetCurrentGameAbsFrame();
    }
    return (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

static int32_t GetStartupLogFrame() {
    if (s_rollbackActive) {
        return RollbackSession_GetCurrentFrame();
    }
    return (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

static bool IsContinuousInMatchRollbackPhase(Net::MatchLifecyclePhase phase) {
    switch (phase) {
        case Net::MatchLifecyclePhase::LoadingAssets:
        case Net::MatchLifecyclePhase::MatchSetup:
        case Net::MatchLifecyclePhase::MatchInit:
        case Net::MatchLifecyclePhase::IntroActive:
        case Net::MatchLifecyclePhase::PlayableGameplay:
        case Net::MatchLifecyclePhase::PauseActive:
        case Net::MatchLifecyclePhase::RoundTransition:
        case Net::MatchLifecyclePhase::MatchEnd:
            return true;
        default:
            return false;
    }
}

static void ResetStartupBarrierState(const char* reason) {
    const bool hadState =
        s_startupBarrierEntered ||
        s_startupProposed ||
        s_startupReleased;

    if (hadState) {
        NetplayLog_Write("STARTUP", GetStartupLogFrame(),
            "Startup barrier reset: reason=%s entered=%d proposed=%d released=%d "
            "remote_proposed=%d",
            reason ? reason : "unspecified",
            s_startupBarrierEntered ? 1 : 0,
            s_startupProposed ? 1 : 0,
            s_startupReleased ? 1 : 0,
            Net::TransitionBarrier_RemoteProposed(Net::NetTransitionKind::GameplayStart) ? 1 : 0);
    }

    s_startupBarrierEntered = false;
    s_startupProposed = false;
    s_startupReleased = false;
    s_startupBlockedLogCounter = 0;
    s_introHoldLogCounter = 0;
    // Clear the barrier slot so a stale proposal from a previous match can
    // never satisfy the next match's release (the old "ignore remote READY
    // before interactive boundary" guard, structurally).
    Net::TransitionBarrier_Clear(Net::NetTransitionKind::GameplayStart,
        reason ? reason : "startup barrier reset");
}

static void LogGameplayPacketAnomaly(const char* reason,
                                     Net::PacketType type,
                                     size_t payloadLen,
                                     size_t expectedLen) {
    NetplayLog_Write(
        "PACKET", GetStartupLogFrame(),
        "%s: type=%s payload=%zu expected=%zu rollback_active=%d gameplay_active=%d dispatched=%d remote_inputs=%d",
        reason ? reason : "gameplay packet anomaly",
        Net::PacketTypeName(type),
        payloadLen,
        expectedLen,
        s_rollbackActive ? 1 : 0,
        s_gameplayActive ? 1 : 0,
        s_packetsDispatched,
        s_remoteInputsReceived);
}

// ============================================================================
// Engine-facing packet sinks (dispatch itself lives in net/packet_router)
// ============================================================================

void OnlineWiring_HandleEngineDataPacket(const void* payload, size_t payloadLen) {
    // Engine internal protocol data (raw Gekko stream until the engine2
    // cutover) — buffer for the rollback session to drain.
    if (payloadLen == 0 || !payload) {
        LogGameplayPacketAnomaly("Empty InputStream", Net::PacketType::InputStream, payloadLen, 1);
        return;
    }

    if (!s_rollbackActive) {
        static uint32_t s_preLiveGekkoDrops = 0;
        s_preLiveGekkoDrops++;
        if (s_preLiveGekkoDrops <= 5 || (s_preLiveGekkoDrops % 120) == 0) {
            NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                "Dropping pre-live InputStream while rollback session is not active: "
                "len=%zu phase=%s",
                payloadLen,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
        }
        return;
    }

    RollbackSession_OnInputStreamPacket(payload, payloadLen);
    s_packetsDispatched++;
    s_remoteInputsReceived++;

    NetplayLog_Verbose("ENGINE", RollbackSession_GetCurrentFrame(),
        "Ingested InputStream packet (%zu bytes)", payloadLen);
}

// ============================================================================
// Bootstrap → Rollback Handoff
// ============================================================================

static bool PrepareBaselineForInteractiveRelease() {
    if (s_liveReleaseArmed) {
        return true;
    }

    const Net::LockedMatchConfig* config = Net::PregameSync_GetLockedConfig();
    if (!config) {
        NetplayLog_Write("HANDOFF", -1, "ERROR: No locked config available for startup handoff");
        LOG_ERROR("[OnlineWiring] No locked config for startup handoff");
        return false;
    }

    Net::PregameBootstrapInfo bootSnap{};
    Net::PregameSync_GetBootstrapInfo(&bootSnap);

    s_configHash = Net::LockedMatchConfig_Hash(config);
    s_baselineCRC = bootSnap.local_baseline_crc;
    s_handoffDelay = Net::DelayPolicy_GetEffectiveLocalDelay();
    s_handoffBudget = Net::DelayPolicy_GetRollbackBudget();
    const int32_t preIntroGameAbsFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
        "=== BOOTSTRAP -> INTRO HANDOFF ===");
    NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
        "Config hash=0x%08X baseline_crc=0x%08X bootstrap_frame_abs=%u gameplay_start_host_game_abs_frame=%d",
        s_configHash,
        s_baselineCRC,
        bootSnap.bootstrap_frame_abs,
        bootSnap.gameplay_start_host_game_abs_frame);
    NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
        "Load barrier sim: local=%d remote=%d | baseline sim: local=%d remote=%d",
        bootSnap.local_load_sim_frame,
        bootSnap.remote_load_sim_frame,
        bootSnap.local_baseline_sim_frame,
        bootSnap.remote_baseline_sim_frame);

    // Preserve hard startup alignment before deterministic intro runs.
    uint32_t preRestoreCRC = CalcCRC32(
        (const void*)ADDR_MATCH_BASE,
        (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
    NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
        "Pre-restore CRC=0x%08X (baseline=0x%08X match=%s)",
        preRestoreCRC,
        s_baselineCRC,
        (preRestoreCRC == s_baselineCRC) ? "YES" : "NO");

    if (Savestate_RestoreRollbackBaseline()) {
        WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, 0);
        WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, 0);

        uint32_t postRestoreCRC = CalcCRC32(
            (const void*)ADDR_MATCH_BASE,
            (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
        NetplayLog_Write("HANDOFF", 0,
            "Baseline RESTORED before intro: crc=0x%08X match=%s sim=0 writeIdx=0",
            postRestoreCRC,
            (postRestoreCRC == s_baselineCRC) ? "YES" : "NO");
    } else {
        NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
            "WARNING: Baseline restore FAILED before intro handoff");
        LOG_WARN("[OnlineWiring] Baseline restore failed before intro handoff");
    }

    // M3: no callback handoff — packet_router routes startup READY/ACK and
    // engine data here in every regime.
    ResetStartupBarrierState("interactive release armed");
    s_liveReleaseArmed = true;

    NetplayLog_Write("STARTUP", GetStartupLogFrame(),
        "Startup barrier armed: deterministic intro runs locally; "
        "rollback advance deferred until mutual post-intro interactive release");
    return true;
}

static bool TryStartRollbackSession() {
    if (s_rollbackActive) return true;  // Already active

    // Get locked config from pregame sync
    const Net::LockedMatchConfig* config = Net::PregameSync_GetLockedConfig();
    if (!config) {
        NetplayLog_Write("HANDOFF", -1, "ERROR: No locked config available for rollback start");
        LOG_ERROR("[OnlineWiring] No locked config for rollback session start");
        return false;
    }

    // Get bootstrap handoff facts for baseline info
    Net::PregameBootstrapInfo bootSnap{};
    Net::PregameSync_GetBootstrapInfo(&bootSnap);

    // Get delay policy values
    const int visibleDelay = Net::DelayPolicy_GetActiveDelay();
    const int effectiveDelay = Net::DelayPolicy_GetEffectiveLocalDelay();
    int rollbackBudget = Net::DelayPolicy_GetRollbackBudget();
    const int protectionWindow = Net::DelayPolicy_GetProtectionWindow();

    // Determine local/remote player via PlayerMapping module
    Net::SessionRole role = Net::Session_GetRole();
    bool isHost = (role == Net::SessionRole::Host);
    int localPlayer = Net::PlayerMapping_DeriveFromRole(config->host_side, isHost);
    int remotePlayer = Net::PlayerMapping_GetRemoteGameSlot();

    // Build rollback session config
    RollbackSessionConfig rbConfig{};
    rbConfig.local_player = localPlayer;
    rbConfig.remote_player = remotePlayer;
    rbConfig.initial_delay = effectiveDelay;
    rbConfig.rollback_budget = rollbackBudget;
    rbConfig.baseline_checksum = s_baselineCRC ? s_baselineCRC : bootSnap.local_baseline_crc;
    const int32_t bootstrapFrame = (int32_t)bootSnap.bootstrap_frame_abs;
    const int32_t interactiveFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    rbConfig.frame_origin_abs = interactiveFrame;

    // Config hash for logging
    s_configHash = Net::LockedMatchConfig_Hash(config);
    s_baselineCRC = rbConfig.baseline_checksum;
    s_handoffDelay = effectiveDelay;
    s_handoffBudget = rollbackBudget;
    s_frameOriginAbs = interactiveFrame;

    // --- LOG BEFORE/AFTER for live release ---
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "=== INTERACTIVE RELEASE -> ROLLBACK START ===");
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "Config: p1_char=%u p1_pal=%u p2_char=%u p2_pal=%u stage=%u rounds_raw=%u rounds_to_win=%d",
        config->p1_character, config->p1_palette,
        config->p2_character, config->p2_palette,
        config->stage_id,
        config->round_count,
        Net::GameSettingsSync_RoundsToWin(config->round_count));
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "RNG seed=0x%08X session_seed=0x%08X config_hash=0x%08X",
        config->rng_seed, config->session_seed, s_configHash);
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "Player assignment: local=P%d remote=P%d (role=%s host_side=%u)",
        localPlayer + 1, remotePlayer + 1,
        (role == Net::SessionRole::Host) ? "Host" : "Join",
        config->host_side);
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "Baseline CRC=0x%08X bootstrap_frame_abs=%d interactive_boundary_game_abs_frame=%d frame_origin_abs=%d rb_start_frame=0",
        s_baselineCRC, bootstrapFrame, interactiveFrame, rbConfig.frame_origin_abs);
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "Load barrier sim: local=%d remote=%d | baseline sim: local=%d remote=%d | start sim=%d",
        bootSnap.local_load_sim_frame,
        bootSnap.remote_load_sim_frame,
        bootSnap.local_baseline_sim_frame,
        bootSnap.remote_baseline_sim_frame,
        bootSnap.gameplay_start_host_game_abs_frame);
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "Frame origin delta from bootstrap baseline: %d frames",
        rbConfig.frame_origin_abs - bootstrapFrame);
    NetplayLog_Write("HANDOFF", interactiveFrame,
        "Local delay visible=%d effective=%d rollback_budget=%d remote_visible=%d remote_effective=%d protection_window=%d stall_threshold=%d",
        visibleDelay,
        effectiveDelay,
        rollbackBudget,
        Net::DelayPolicy_GetRemoteAnnouncedDelay(),
        Net::DelayPolicy_GetEffectiveRemoteDelay(),
        protectionWindow,
        Net::DelayPolicy_GetStallThreshold());
    Net::DelayPolicy_LogDelayMap("rollback handoff");

    // M3: no callback registration — packet_router is the permanent owner.

    // Start rollback session through GameplayBridge
    bool ok = Net::GameplayBridge_StartSession(rbConfig);
    if (!ok) {
        NetplayLog_Write("HANDOFF", interactiveFrame,
            "ERROR: GameplayBridge_StartSession FAILED");
        LOG_ERROR("[OnlineWiring] GameplayBridge_StartSession failed");
        return false;
    }

    s_rollbackStarted = true;
    s_rollbackActive = true;
    s_gameplayActive = Net::NetplayPhaseRuntime_IsInteractivePacingPhase(
        Net::NetplayPhaseRuntime_GetPhase());
    s_liveReleaseArmed = false;
    // Clear any stale startup hold pulse now that rollback owns gameplay.
    // If Gekko is still finishing its own pre-start sync, the dispatcher gate
    // will re-arm a fresh one-frame runtime freeze as needed.
    InputSyncHooks_SetTimesyncFreeze(false);

    // Mark delay as consumed by the live rollback session.
    Net::DelayPolicy_OnRollbackApplied(effectiveDelay);

    // Reset per-match digest history/desync flags so warnings don't leak across
    // rematch/new-session frame-number reuse windows.
    RollbackDebug_ResetSession();
    Net::SyncTrace_ResetSession("rollback start");

    // Enable state digest for desync detection (authoritative, settled-frame compare).
    RollbackDebug_SetDigestEnabled(true);
    if (Net::SyncTrace_ShouldArmIntegrityOnRollback()) {
        Net::SyncTrace_SetIntegrityActive(true, "rollback start");
    }

    if (!s_gameplayActive) {
        NetplayLog_Write("HANDOFF", interactiveFrame,
            "Rollback session owns pre-playable match frames; lifecycle=%s",
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
    }
    if (!s_startupReleased) {
        NetplayLog_Write("STARTUP", GetStartupLogFrame(),
            "WARNING: rollback session started before startup release completed");
    }

    NetplayLog_Write("HANDOFF", interactiveFrame,
        "=== ROLLBACK SESSION STARTED SUCCESSFULLY ===");
    LOG_INFO("[OnlineWiring] Rollback session started: P%d vs P%d, visible_delay=%d effective_delay=%d budget=%d remote_delay=%d stall_threshold=%d baseline=0x%08X",
        localPlayer + 1,
        remotePlayer + 1,
        visibleDelay,
        effectiveDelay,
        rollbackBudget,
        Net::DelayPolicy_GetRemoteAnnouncedDelay(),
        Net::DelayPolicy_GetStallThreshold(),
        s_baselineCRC);

    Net::SpectatorRuntime_OnRollbackStarted(rbConfig.frame_origin_abs);

    return true;
}

// ============================================================================
// Safe Teardown
// ============================================================================

static bool ShouldPreservePaletteRuntimeForPostMatch(const char* reason) {
    if (!reason) {
        return false;
    }

    return strstr(reason, "match ended") != nullptr ||
           strcmp(reason, "match end event") == 0;
}

static void StopRollbackSession(const char* reason) {
    if (!s_rollbackActive) return;

    int32_t frame = RollbackSession_GetCurrentFrame();

    // Snapshot before teardown
    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);

    NetplayLog_Write("TEARDOWN", frame,
        "=== ROLLBACK SESSION ENDING ===");
    NetplayLog_Write("TEARDOWN", frame,
        "Reason: %s", reason ? reason : "unknown");
    NetplayLog_Write("TEARDOWN", frame,
        "Final stats: rb_frame=%d game_abs_frame=%d origin_abs=%d rollbacks=%d maxdepth=%d",
        snap.rb_frame_current,
        snap.game_abs_frame_current,
        snap.frame_origin_abs,
        snap.rollback_count, snap.max_rollback_distance);
    NetplayLog_Write("TEARDOWN", frame,
        "IO: sent=%d recv=%d",
        snap.local_inputs_sent, snap.remote_inputs_received);

    // Check for desync before ending
    RollbackDebug_LogSessionSummary(reason ? reason : "rollback stop");
    if (RollbackDebug_IsDesyncDetected()) {
        int32_t desyncFrame = RollbackDebug_GetDesyncFrame();
        NetplayLog_Write("TEARDOWN", frame,
            "WARNING: Desync was detected at frame %d", desyncFrame);
    }

    // End session through GameplayBridge
    Net::GameplayBridge_EndSession();
    Net::NetplayPacing_ResetSession(reason ? reason : "rollback stop");
    Net::ChurnPause_ResetSession(reason ? reason : "rollback stop");
    Net::SyncTrace_ResetSession(reason ? reason : "rollback stop");
    Net::SyncTrace_SetIntegrityActive(false, reason ? reason : "rollback stop");
    RollbackDebug_SetDigestEnabled(false);
    InputSyncHooks_SetLoadBarrierFreeze(false);
    InputSyncHooks_SetTimesyncFreeze(false);

    s_rollbackActive = false;
    s_gameplayActive = false;
    s_liveReleaseArmed = false;
    ResetStartupBarrierState(reason ? reason : "rollback stop");
    Net::SpectatorRuntime_OnMatchEnd(reason ? reason : "rollback stop");
    if (ShouldPreservePaletteRuntimeForPostMatch(reason)) {
        NetplayLog_Write("PALETTE", frame,
            "Palette runtime retained for win-screen cleanup: reason=%s",
            reason ? reason : "rollback stop");
    } else {
        Net::NetplayPaletteRuntime_OnMatchEnd(reason ? reason : "rollback stop");
    }

    NetplayLog_Write("TEARDOWN", frame,
        "=== ROLLBACK SESSION ENDED ===");
    LOG_INFO("[OnlineWiring] Rollback session ended: %s", reason ? reason : "unknown");
}

static bool DisconnectNeedsBoundaryCleanup() {
    Net::MatchLifecycleSnapshot lifecycle{};
    Net::MatchLifecycle_GetSnapshot(&lifecycle);

    const Net::PregamePhase pregamePhase = Net::PregameSync_GetPhase();
    const uint32_t mode = GetGameMode();

    return s_rollbackStarted ||
           s_rollbackActive ||
           s_gameplayActive ||
           s_liveReleaseArmed ||
           s_rollbackBeginPending ||
           s_frameOriginAbs >= 0 ||
           s_baselineCRC != 0 ||
           s_configHash != 0 ||
           lifecycle.active ||
           pregamePhase != Net::PregamePhase::Idle ||
           mode == MODE_CHARSEL ||
           mode == MODE_PREMATCH_INTRO ||
           mode == MODE_MATCH ||
           mode == MODE_WINSCREEN;
}

// ============================================================================
// Phase Transition Logging
// ============================================================================

static void LogLifecycleTransition(Net::MatchLifecyclePhase from,
                                   Net::MatchLifecyclePhase to) {
    int32_t frame = s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1;

    NetplayLog_StateChange("LIFE", frame,
        "lifecycle_phase",
        Net::MatchLifecyclePhaseName(from),
        Net::MatchLifecyclePhaseName(to),
        "game state change");
}

static void LogDelayChange(int from, int to, const char* reason) {
    int32_t frame = s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1;
    NetplayLog_ValueChange("DELAY", frame, "effective_local_delay", from, to, reason);
}

static void LogBudgetChange(int from, int to, const char* reason) {
    int32_t frame = s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1;
    NetplayLog_ValueChange("DELAY", frame, "rollback_budget", from, to, reason);
}

// ============================================================================
// Per-Frame Update
// ============================================================================

// Mid-session local input-delay hotkeys `-`/`=` (plan §9 Q3, shipped with
// the first release; INV-23/B-7: a peer-local knob with no wire message —
// changing it here has no interaction with the peer whatsoever).
static void UpdateDelayHotkeys() {
    if (!s_rollbackActive || !s_gameplayActive) {
        return;
    }
    static bool s_minusHeld = false;
    static bool s_plusHeld = false;
    const bool minusDown = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
    const bool plusDown  = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;
    int step = 0;
    if (minusDown && !s_minusHeld) step -= 1;
    if (plusDown && !s_plusHeld) step += 1;
    s_minusHeld = minusDown;
    s_plusHeld = plusDown;
    if (step == 0) {
        return;
    }

    const int cur = RollbackSession_GetActiveDelay();
    const int target = cur + step;
    if (target < 0 || target > 15) {
        return;
    }
    if (RollbackSession_SetLocalDelay(target)) {
        Net::DelayPolicy_OnRollbackApplied(target);
        NetplayLog_Write("DELAY", RollbackSession_GetCurrentFrame(),
            "Local delay hotkey applied: %d -> %d (peer-local, no wire message)",
            cur, target);
        LOG_NETPLAY(LOG_INFO, "[OnlineWiring] Local input delay %d -> %d (hotkey)", cur, target);
    } else {
        NetplayLog_Write("DELAY", RollbackSession_GetCurrentFrame(),
            "Local delay hotkey deferred/refused by session: %d -> %d", cur, target);
    }
}

static void CheckPolicyChanges() {
    int curDelay = Net::DelayPolicy_GetEffectiveLocalDelay();
    if (s_lastActiveDelay >= 0 && curDelay != s_lastActiveDelay) {
        LogDelayChange(s_lastActiveDelay, curDelay, "delay policy update");

        if (s_rollbackActive) {
            const bool applied = RollbackSession_SetLocalDelay(curDelay);
            if (applied) {
                Net::DelayPolicy_OnRollbackApplied(curDelay);
                NetplayLog_Write("DELAY", RollbackSession_GetCurrentFrame(),
                    "Applied delay policy update to live rollback session: effective_delay=%d",
                    curDelay);
            } else {
                NetplayLog_Write("DELAY", RollbackSession_GetCurrentFrame(),
                    "WARNING: failed to apply live delay update to rollback session: effective_delay=%d",
                    curDelay);
            }
        }
    }
    s_lastActiveDelay = curDelay;

    int curBudget = Net::DelayPolicy_GetRollbackBudget();
    if (s_lastRollbackBudget >= 0 && curBudget != s_lastRollbackBudget) {
        LogBudgetChange(s_lastRollbackBudget, curBudget, "budget policy update");
    }
    s_lastRollbackBudget = curBudget;
}

static void CheckLifecyclePhase() {
    Net::MatchLifecyclePhase curPhase = Net::MatchLifecycle_GetPhase();

    if (curPhase != s_lastLifecyclePhase) {
        LogLifecycleTransition(s_lastLifecyclePhase, curPhase);

        // === Handle phase transitions ===

        // Entering PlayableGameplay — usually means interactive control began.
        // Rollback start is deferred to the mutual post-intro startup release.
        if (curPhase == Net::MatchLifecyclePhase::PlayableGameplay) {
            if (s_rollbackStarted) {
                const bool resumed = (s_lastLifecyclePhase == Net::MatchLifecyclePhase::PauseActive ||
                                      s_lastLifecyclePhase == Net::MatchLifecyclePhase::RoundTransition);
                const bool wasGameplayActive = s_gameplayActive;
                s_gameplayActive = true;

                if (!wasGameplayActive) {
                    NetplayLog_Write("LIFE",
                        s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1,
                        resumed
                            ? "Interactive gameplay resumed within the continuous rollback session"
                            : "Interactive gameplay began under continuous rollback ownership");
                }
            } else {
                if (!s_liveReleaseArmed) {
                    // Defensive fallback: if handoff wiring was missed, still arm the
                    // interactive startup barrier here so first-advance cannot race.
                    s_liveReleaseArmed = true;
                    ResetStartupBarrierState("lifecycle playable fallback");
                    NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                        "Fallback: startup barrier armed at PlayableGameplay (handoff missing)");
                }
                NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                    "Reached first interactive boundary; waiting for mutual startup release");
            }
        }

        // Leaving PlayableGameplay — handle based on where we're going
        if (s_lastLifecyclePhase == Net::MatchLifecyclePhase::PlayableGameplay) {
            if (curPhase == Net::MatchLifecyclePhase::PauseActive) {
                // Pause — keep the session alive, but interactive pacing stops.
                s_gameplayActive = false;
                Net::PauseHandler_OnPauseEnter();
                NetplayLog_Write("LIFE", RollbackSession_GetCurrentFrame(),
                    "Pause active — rollback session retained; interactive pacing paused");
            } else if (curPhase == Net::MatchLifecyclePhase::RoundTransition) {
                // Round end transition — keep the same session running into the next round.
                s_gameplayActive = false;
                NetplayLog_Write("LIFE", RollbackSession_GetCurrentFrame(),
                    "Round transition — rollback session retained; non-interactive in-match phase continues");
            } else if (curPhase == Net::MatchLifecyclePhase::MatchEnd) {
                // Match end — stop rollback safely
                StopRollbackSession("match ended");
            } else if (curPhase == Net::MatchLifecyclePhase::DisconnectRecovery) {
                StopRollbackSession("disconnect during gameplay");
            }
        }

        // Match end from any state
        if (curPhase == Net::MatchLifecyclePhase::MatchEnd &&
            s_lastLifecyclePhase != Net::MatchLifecyclePhase::MatchEnd) {
            // Record match result for set tracking
            Net::MatchLifecycleSnapshot lifeSnap{};
            Net::MatchLifecycle_GetSnapshot(&lifeSnap);
            Net::SetTracker_RecordResult(lifeSnap.winner);
        }

        if (curPhase == Net::MatchLifecyclePhase::MatchEnd && s_rollbackActive) {
            StopRollbackSession("match ended (non-gameplay)");
        }

        // Begin win-screen lockstep as soon as the match ends so both peers stay
        // aligned through the Mode 8 -> Mode 9 transition and fade-in substates.
        if (curPhase == Net::MatchLifecyclePhase::MatchEnd &&
            s_lastLifecyclePhase != Net::MatchLifecyclePhase::MatchEnd) {
            Net::WinScreenSync_Begin();
            NetplayLog_Write("LIFE", -1,
                "Match ended — win-screen lockstep armed before Mode 9 entry");
        }

        // Disconnect from any state
        if (curPhase == Net::MatchLifecyclePhase::DisconnectRecovery && s_rollbackActive) {
            StopRollbackSession("disconnect");
        }

        // Entering WinScreenActive — palette cleanup only (lockstep already armed at MatchEnd)
        if (curPhase == Net::MatchLifecyclePhase::WinScreenActive &&
            s_lastLifecyclePhase != Net::MatchLifecyclePhase::WinScreenActive) {
            Net::NetplayPaletteRuntime_OnWinScreenEnter();
            NetplayLog_Write("LIFE", -1,
                "Win screen entered — rollback gameplay session detached, post-match lockstep active");
        }

        // Leaving WinScreenActive — abort sync if still running
        if (s_lastLifecyclePhase == Net::MatchLifecyclePhase::WinScreenActive &&
            curPhase != Net::MatchLifecyclePhase::WinScreenActive) {
            Net::WinScreenSync_Abort();
        }

        // Disconnect kills win screen sync
        if (curPhase == Net::MatchLifecyclePhase::DisconnectRecovery) {
            Net::WinScreenSync_Abort();
        }

        // Inactive — full cleanup
        if (curPhase == Net::MatchLifecyclePhase::Inactive) {
            if (s_rollbackActive) {
                StopRollbackSession("lifecycle inactive");
            }
            // Reset per-match state
            s_rollbackStarted = false;
            s_rollbackBeginPending = false;
            s_liveReleaseArmed = false;
            s_frameOriginAbs = -1;
            s_remoteInputsReceived = 0;
            s_packetsDispatched = 0;
            s_backgroundPollCount = 0;
            s_backgroundPollFailures = 0;
            InputSyncHooks_SetLoadBarrierFreeze(false);
            InputSyncHooks_SetTimesyncFreeze(false);
            ResetStartupBarrierState("lifecycle inactive");
            NetplayLog_Write("LIFE", -1,
                "Rollback cleanup complete: lifecycle inactive, frame_origin_abs reset");
            // Do NOT reset SetTracker here — Inactive is reached on both rematch
            // (charsel return) and real session end. The session-end reset is in
            // OnlineWiring_OnDisconnect, which is the only correct reset point.
        }

        // MatchInit — potential round restart, re-enable gameplay flag
        if (curPhase == Net::MatchLifecyclePhase::MatchInit && s_rollbackStarted) {
            if (s_lastLifecyclePhase == Net::MatchLifecyclePhase::RoundTransition) {
                Net::NetplayPaletteRuntime_OnRoundRestart();
            }
            // New round starting — rollback session stays alive
            NetplayLog_Write("LIFE", -1,
                "New round init — same rollback session continues: frame_origin_abs=%d startup_released=%d pending_frame=%d",
                s_frameOriginAbs,
                s_startupReleased ? 1 : 0,
                RollbackSession_HasPendingFrame() ? 1 : 0);
        }

        // IntroActive → will reach PlayableGameplay soon
        if (curPhase == Net::MatchLifecyclePhase::IntroActive) {
            NetplayLog_Write("LIFE", -1,
                "Intro active — same rollback session continues: startup_barrier_active=%d pacing_active=0 pending_frame=%d release_armed=%d",
                (!s_startupReleased && (s_liveReleaseArmed || s_rollbackActive)) ? 1 : 0,
                RollbackSession_HasPendingFrame() ? 1 : 0,
                s_liveReleaseArmed ? 1 : 0);
        }

        s_lastLifecyclePhase = curPhase;
    }
}

// ============================================================================
// Public API
// ============================================================================

void OnlineWiring_Init() {
    s_initialized = true;
    s_rollbackStarted = false;
    s_rollbackActive = false;
    s_gameplayActive = false;
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
    s_frameOriginAbs = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    s_backgroundPollCount = 0;
    s_backgroundPollFailures = 0;
    s_lastLifecyclePhase = Net::MatchLifecyclePhase::Inactive;
    s_lastActiveDelay = -1;
    s_lastRollbackBudget = -1;
    ResetStartupBarrierState("init");

    Net::NetplayPacing_Init();
    Net::ChurnPause_Init();
    StressHooks_Init();
    Net::SetTracker_Init();
    Net::WinScreenSync_Init();
    Net::PauseHandler_Init();

    LOG_INFO("[OnlineWiring] Initialized");
    NetplayLog_Write("WIRING", -1, "OnlineWiring initialized");
    NetplayLog_Write("STARTUP", -1,
        "Vendor/runtime patch path: using integration-layer startup barrier; "
        "no GekkoNet internal FramesAhead override active");
}

void OnlineWiring_Shutdown() {
    if (s_rollbackActive) {
        StopRollbackSession("mod shutdown");
    }
    Net::NetplayPacing_Shutdown();
    Net::ChurnPause_Shutdown();
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
    s_frameOriginAbs = -1;
    ResetStartupBarrierState("shutdown");
    Net::WinScreenSync_Shutdown();
    Net::PauseHandler_Shutdown();
    StressHooks_Shutdown();
    s_initialized = false;
}

void OnlineWiring_FrameUpdate() {
    if (!s_initialized) return;

    // Track lifecycle phase transitions and trigger rollback start/stop
    CheckLifecyclePhase();

    // Track delay/budget policy changes (before/after logging)
    CheckPolicyChanges();

    s_gameplayActive = IsInteractiveRollbackPhase();

    // Mid-session delay hotkeys (M5; peer-local, INV-23).
    UpdateDelayHotkeys();

    // Keep Gekko session events/liveness flowing even when the input dispatcher
    // is stalled in lockstep/startup holds. This remains game-thread-owned
    // (no cross-thread Gekko mutation), but decouples poll cadence from the
    // dispatcher's ability to advance simulation.
    if (s_rollbackActive) {
        s_backgroundPollCount++;
        Net::ChurnPause_OnRollbackPoll(
            s_gameplayActive && s_rollbackActive,
            RollbackSession_GetCurrentFrame());
        const bool pollOk = RollbackSession_PollSession();
        // Session death gating (M3): tear down only when Gekko reports the
        // session broken (20s disconnect timeout fired) or the
        // ConnectionSupervisor has reached its Dead verdict. A supervisor
        // Interrupted state is a freeze-and-wait condition, never a teardown.
        const bool supervisorDead = Net::ConnectionSupervisor_IsDead();
        if (!pollOk || supervisorDead) {
            s_backgroundPollFailures++;
            NetplayLog_Write("GEKKO", RollbackSession_GetCurrentFrame(),
                "Background poll FAILED: count=%u failures=%u poll_ok=%d supervisor_dead=%d phase=%s",
                s_backgroundPollCount,
                s_backgroundPollFailures,
                pollOk ? 1 : 0,
                supervisorDead ? 1 : 0,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            if (Net::MatchLifecycle_GetPhase() != Net::MatchLifecyclePhase::DisconnectRecovery) {
                Net::MatchLifecycle_OnDisconnect(supervisorDead
                    ? "Connection supervisor declared peer dead"
                    : "Rollback session poll failure");
            }
        } else if (s_backgroundPollCount <= 5 || (s_backgroundPollCount % 300) == 0) {
            NetplayLog_Verbose("GEKKO", RollbackSession_GetCurrentFrame(),
                "Background poll ok: count=%u phase=%s gameplay=%d",
                s_backgroundPollCount,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                s_gameplayActive ? 1 : 0);
        }
    }

    const Net::MatchLifecyclePhase curPhase = Net::MatchLifecycle_GetPhase();
    if (s_rollbackActive &&
        RollbackSession_HasPendingFrame() &&
        IsContinuousInMatchRollbackPhase(curPhase) &&
        curPhase != Net::MatchLifecyclePhase::PlayableGameplay) {
        const bool drained = RollbackSession_DrainPendingNonAdvanceEvents();
        if (!drained) {
            NetplayLog_Write("GEKKO", RollbackSession_GetCurrentFrame(),
                "Pending rollback frame still requires dispatcher-owned advance: phase=%s",
                Net::MatchLifecyclePhaseName(curPhase));
        }
    }

    // Startup gameplay-entry barrier:
    // - Deterministic intro runs in vanilla/passive mode.
    // - Rollback-owned BeginFrame/Advance is held until BOTH peers report the
    //   first post-intro interactive boundary and exchange READY+ACK.
    if ((s_liveReleaseArmed || s_rollbackActive) && !s_startupReleased) {
        const bool sessionConnected = Net::Session_IsConnected();
        const Net::MatchRollbackPhase rollbackPhase = Net::NetplayPhaseRuntime_GetPhase();
        const bool inPlayableGameplay =
            Net::NetplayPhaseRuntime_IsInteractivePacingPhase(rollbackPhase);

        if (!s_startupBarrierEntered) {
            s_startupBarrierEntered = true;
            NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                "Local startup barrier entered: lifecycle=%s session_connected=%d rollback_active=%d",
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                sessionConnected ? 1 : 0,
                s_rollbackActive ? 1 : 0);
        }

        if (sessionConnected && !inPlayableGameplay) {
            s_introHoldLogCounter++;
            if (s_introHoldLogCounter <= 5 || (s_introHoldLogCounter % 120) == 0) {
                NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                    "Session connected but live advance still gated: intro not finished "
                    "(phase=%s)",
                    Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            }
        } else {
            s_introHoldLogCounter = 0;
        }

        if (sessionConnected && inPlayableGameplay && !s_startupProposed) {
            // TransitionBarrier GameplayStart (M5, replacing GekkoReady):
            // propose once at the interactive boundary; the barrier owns
            // resends (250 ms) and idempotent re-acks.
            Net::TransitionBarrier_Propose(
                Net::NetTransitionKind::GameplayStart, 0,
                Net::PregameSync_GetCurrentEpoch());
            s_startupProposed = true;
            NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                "Startup GameplayStart barrier proposed at interactive boundary: epoch=%u phase=%s",
                Net::PregameSync_GetCurrentEpoch(),
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
        }

        const bool releaseSatisfied =
            s_startupProposed &&
            Net::TransitionBarrier_IsCommitted(Net::NetTransitionKind::GameplayStart);

        if (releaseSatisfied) {
            Net::TransitionBarrier_ConsumeCommit(Net::NetTransitionKind::GameplayStart);
            s_startupReleased = true;
            NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                "Gameplay-entry release satisfied at interactive boundary "
                "(GameplayStart barrier committed): local_game_abs_frame=%d phase=%s",
                GetStartupLogFrame(),
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));

            if (!s_rollbackStarted) {
                s_rollbackBeginPending = true;
            }
        } else if (s_startupProposed) {
            s_startupBlockedLogCounter++;
            if (s_startupBlockedLogCounter <= 5 ||
                (s_startupBlockedLogCounter % 120) == 0) {
                NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                    "Startup release BLOCKED at interactive boundary: "
                    "remote_proposed=%d session_connected=%d local_game_abs_frame=%d phase=%s",
                    Net::TransitionBarrier_RemoteProposed(Net::NetTransitionKind::GameplayStart) ? 1 : 0,
                    sessionConnected ? 1 : 0,
                    GetStartupLogFrame(),
                    Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            }
        }
    }

    // Deferred rollback start (after startup barrier update)
    if (s_rollbackBeginPending) {
        if (TryStartRollbackSession()) {
            s_rollbackBeginPending = false;
        } else {
            NetplayLog_Write("HANDOFF", GetStartupLogFrame(),
                "Rollback start still pending (will retry): phase=%s released=%d",
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                s_startupReleased ? 1 : 0);
        }
    }

    // Drive win screen sync through match-end transition and win screen phase.
    if (curPhase == Net::MatchLifecyclePhase::MatchEnd ||
        curPhase == Net::MatchLifecyclePhase::WinScreenActive) {
        Net::WinScreenSync_FrameUpdate();
    }

    // Drive pause handler when session is owned
    if (Net::MatchLifecycle_IsMatchOwned()) {
        Net::PauseHandler_FrameUpdate();
    }

    Net::ChurnPause_FrameUpdate(
        s_gameplayActive && s_rollbackActive,
        s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1);

    // Drive rollback subsystems only when gameplay is active
    if (s_rollbackActive && s_gameplayActive) {
        // GameplayBridge_FrameUpdate (rollback session + delay consumption)
        // is called from mod_main.cpp after OnlineWiring_FrameUpdate.
        // We only do auxiliary diagnostic work here.

        // Periodic RTT/stats logging (every 5 seconds = 300 frames)
        int32_t frame = RollbackSession_GetCurrentFrame();
        if (frame > 0 && frame % 300 == 0) {
            Net::ConnectionStats stats{};
            Net::Session_GetStats(&stats);

            Net::DelayPolicySnapshot dpSnap{};
            Net::DelayPolicy_GetSnapshot(&dpSnap);

            RollbackSessionSnapshot rbSnap{};
            RollbackSession_GetSnapshot(&rbSnap);

            NetplayLog_Write("STATS", frame,
                "RTT=%.1fms jitter=%.1fms loss=%u/%u visible_delay=%d effective_delay=%d budget=%d stall=%d",
                rbSnap.link_avg_ping > 0.0f ? rbSnap.link_avg_ping : stats.rtt_ms,
                rbSnap.link_jitter,
                stats.packets_lost, stats.packets_sent,
                dpSnap.active_delay,
                dpSnap.effective_local_delay,
                dpSnap.rollback_budget,
                dpSnap.stall_threshold);

            NetplayLog_Write("STATS", frame,
                "Rollbacks=%d maxdepth=%d frames_ahead=%.1f",
                rbSnap.rollback_count,
                rbSnap.max_rollback_distance,
                rbSnap.frames_ahead);
        }
    }
}

void OnlineWiring_OnGameplayStart() {
    if (s_rollbackStarted || s_liveReleaseArmed) {
        return;
    }

    const Net::LockedMatchConfig* config = Net::PregameSync_GetLockedConfig();
    if (config) {
        Net::SpectatorRuntime_OnMatchBegin(config);
    }

    if (!PrepareBaselineForInteractiveRelease()) {
        NetplayLog_Write("HANDOFF", GetStartupLogFrame(),
            "ERROR: failed to prepare interactive-release startup handoff");
        return;
    }

    s_rollbackBeginPending = false;
    NetplayLog_Write("HANDOFF", GetStartupLogFrame(),
        "Rollback session start deferred: waiting for first post-intro interactive boundary");
}

void OnlineWiring_OnGameplayPause(const char* reason) {
    s_gameplayActive = false;
    NetplayLog_Write("LIFE", s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1,
        "Gameplay paused: %s", reason ? reason : "unknown");
}

void OnlineWiring_OnMatchEnd() {
    if (s_rollbackActive) {
        StopRollbackSession("match end event");
    } else {
        s_liveReleaseArmed = false;
        s_rollbackBeginPending = false;
        s_frameOriginAbs = -1;
        ResetStartupBarrierState("match end");
    }
}

void OnlineWiring_OnDisconnect(const char* reason) {
    const char* sessionErr = RollbackSession_GetErrorReason();
    const char* effectiveReason = reason;
    if (!effectiveReason || !effectiveReason[0]) {
        effectiveReason = (sessionErr && sessionErr[0]) ? sessionErr : "Disconnected.";
    } else if (sessionErr && sessionErr[0]) {
        NetplayLog_Write("DISCONNECT", s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1,
            "Rollback drop reason: %s (caller reason: %s)",
            sessionErr,
            reason);
    }

    Net::SetTrackerSnapshot setSnap{};
    Net::SetTracker_GetSnapshot(&setSnap);
    NetplayLog_Write("DISCONNECT", s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1,
        "=== DISCONNECT: %s === mode=%u pregame=%s lifecycle=%s frame_origin=%d local=%d remote=%d draws=%d matches=%d",
        effectiveReason,
        GetGameMode(),
        Net::PregamePhaseName(Net::PregameSync_GetPhase()),
        Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
        s_frameOriginAbs,
        setSnap.local_wins,
        setSnap.remote_wins,
        setSnap.draws,
        setSnap.total_matches);

    const bool boundaryCleanupNeeded = DisconnectNeedsBoundaryCleanup();

    if (s_rollbackActive) {
        StopRollbackSession(effectiveReason);
    }

    if (boundaryCleanupNeeded) {
        NetplayLog_Write("DISCONNECT", -1,
            "Running forced match-boundary cleanup for disconnect: mode=%u pregame=%s lifecycle=%s "
            "started=%d active=%d live_release=%d pending=%d baseline=0x%08X config=0x%08X",
            GetGameMode(),
            Net::PregamePhaseName(Net::PregameSync_GetPhase()),
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
            s_rollbackStarted ? 1 : 0,
            s_rollbackActive ? 1 : 0,
            s_liveReleaseArmed ? 1 : 0,
            s_rollbackBeginPending ? 1 : 0,
            s_baselineCRC,
            s_configHash);
        RematchCleanup_PrepareForNextMatch(effectiveReason);
    } else {
        NetplayLog_Write("DISCONNECT", -1,
            "Skipping forced match-boundary cleanup; disconnect occurred before match ownership");
    }

    if (Net::PregameSync_GetPhase() != Net::PregamePhase::Idle) {
        NetplayLog_Write("DISCONNECT", -1,
            "Aborting pregame/bootstrap state during disconnect cleanup: phase=%s",
            Net::PregamePhaseName(Net::PregameSync_GetPhase()));
        Net::PregameSync_Abort(effectiveReason);
    }

    s_rollbackStarted = false;
    s_rollbackActive = false;
    s_gameplayActive = false;
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
    s_frameOriginAbs = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    s_backgroundPollCount = 0;
    s_backgroundPollFailures = 0;
    s_lastLifecyclePhase = Net::MatchLifecyclePhase::Inactive;
    s_lastActiveDelay = -1;
    s_lastRollbackBudget = -1;
    Net::NetplayPacing_ResetSession("disconnect");
    Net::ChurnPause_ResetSession(reason ? reason : "disconnect");
    Net::SyncTrace_ResetSession(reason ? reason : "disconnect");
    Net::SyncTrace_SetIntegrityActive(false, reason ? reason : "disconnect");
    Net::WinScreenSync_Abort();
    Net::FrontendInputSync_AbortEpoch(reason ? reason : "disconnect");
    Net::SpectatorRuntime_OnDisconnect(reason ? reason : "disconnect");
    Net::NetplayPaletteRuntime_OnDisconnect(reason ? reason : "disconnect");
    Net::CharSelPaletteSelect_ResetNetplaySessionState(reason ? reason : "disconnect");

    // Reset set tracker on session end
    Net::SetTracker_Reset();
    ResetStartupBarrierState("disconnect");

    // Log final state
    Net::ConnectionStats stats{};
    Net::Session_GetStats(&stats);
    NetplayLog_Write("DISCONNECT", -1,
        "Final network stats: RTT=%.1fms sent=%u recv=%u lost=%u",
        stats.rtt_ms, stats.packets_sent, stats.packets_received, stats.packets_lost);

    NetplayLog_Flush();
}

void OnlineWiring_OnRematch() {
    NetplayLog_Write("POSTMATCH", -1,
        "=== REMATCH SELECTED — returning to CharSel ===");
    NetplayLog_Write("POSTMATCH", -1,
        "Clearing rollback state for next match");
    NetplayLog_Write("POSTMATCH", -1,
        "Pre-cleanup wiring state: started=%d active=%d gameplay=%d frame_origin_abs=%d baseline=0x%08X config=0x%08X recv=%d dispatched=%d",
        s_rollbackStarted ? 1 : 0,
        s_rollbackActive ? 1 : 0,
        s_gameplayActive ? 1 : 0,
        s_frameOriginAbs,
        s_baselineCRC,
        s_configHash,
        s_remoteInputsReceived,
        s_packetsDispatched);

    RematchCleanup_PrepareForNextMatch("post-match rematch");

    // Full reset for next match
    s_rollbackStarted = false;
    s_rollbackActive = false;
    s_gameplayActive = false;
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
    s_frameOriginAbs = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    s_backgroundPollCount = 0;
    s_backgroundPollFailures = 0;
    s_lastActiveDelay = -1;
    s_lastRollbackBudget = -1;
    Net::NetplayPacing_ResetSession("rematch");
    Net::ChurnPause_ResetSession("rematch");
    Net::SyncTrace_ResetSession("rematch");
    Net::SyncTrace_SetIntegrityActive(false, "rematch");
    ResetStartupBarrierState("rematch");
    Net::SpectatorRuntime_OnMatchEnd("rematch");
    Net::NetplayPaletteRuntime_OnMatchEnd("rematch");

    NetplayLog_Write("POSTMATCH", -1,
        "Post-cleanup wiring state: started=%d active=%d gameplay=%d frame_origin_abs=%d baseline=0x%08X config=0x%08X recv=%d dispatched=%d",
        s_rollbackStarted ? 1 : 0,
        s_rollbackActive ? 1 : 0,
        s_gameplayActive ? 1 : 0,
        s_frameOriginAbs,
        s_baselineCRC,
        s_configHash,
        s_remoteInputsReceived,
        s_packetsDispatched);
    NetplayLog_Write("POSTMATCH", -1,
        "New match reset complete after rematch selection");
}

void OnlineWiring_OnReturnToSession() {
    NetplayLog_Write("POSTMATCH", -1,
        "=== RETURN TO SESSION — exiting match flow ===");

    RematchCleanup_PrepareForNextMatch("post-match return to session");

    s_rollbackStarted = false;
    s_rollbackActive = false;
    s_gameplayActive = false;
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
    s_frameOriginAbs = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    s_backgroundPollCount = 0;
    s_backgroundPollFailures = 0;
    Net::NetplayPacing_ResetSession("return to session");
    Net::ChurnPause_ResetSession("return to session");
    Net::SyncTrace_ResetSession("return to session");
    Net::SyncTrace_SetIntegrityActive(false, "return to session");
    ResetStartupBarrierState("return to session");
    Net::SpectatorRuntime_OnMatchEnd("return to session");
    Net::NetplayPaletteRuntime_OnMatchEnd("return to session");
    NetplayLog_Write("POSTMATCH", -1,
        "Rollback cleanup complete for return-to-session route");
}

bool OnlineWiring_IsGameplayActive() {
    return IsInteractiveRollbackPhase();
}

bool OnlineWiring_IsStartupReleased() {
    return s_startupReleased;
}

bool OnlineWiring_IsGameplayEntryAdvanceBlocked() {
    const bool atInteractiveBoundary =
        Net::NetplayPhaseRuntime_IsInteractivePacingPhase(
            Net::NetplayPhaseRuntime_GetPhase());
    if (!atInteractiveBoundary) {
        return false;
    }

    if (!s_startupReleased) {
        return s_liveReleaseArmed || s_rollbackActive;
    }

    if (!s_rollbackActive && (s_liveReleaseArmed || s_rollbackBeginPending || !s_rollbackStarted)) {
        return true;
    }

    return false;
}

void OnlineWiring_GetSnapshot(OnlineWiringSnapshot* out) {
    if (!out) return;
    Net::NetplayPacingSnapshot pacingSnap{};
    Net::NetplayPacing_GetSnapshot(&pacingSnap);
    const Net::MatchRollbackPhase rollbackPhase = Net::NetplayPhaseRuntime_GetPhase();
    const bool sessionRunning = s_rollbackActive && RollbackSession_IsSessionRunning();
    const bool startupBarrierArmed = !s_startupReleased && (s_liveReleaseArmed || s_rollbackActive);
    const bool rollbackOwned = s_rollbackActive && Net::NetplayPhaseRuntime_IsRollbackOwnedPhase(rollbackPhase);
    out->rollback_started = s_rollbackStarted;
    out->rollback_active = s_rollbackActive;
    out->gameplay_active = s_rollbackActive &&
                           Net::NetplayPhaseRuntime_IsInteractivePacingPhase(rollbackPhase);
    out->session_running = sessionRunning;
    out->stepping_enabled = rollbackOwned && !OnlineWiring_IsGameplayEntryAdvanceBlocked();
    out->startup_barrier_armed = startupBarrierArmed;
    out->startup_barrier_released = s_startupReleased;
    out->lockstep_owner_active = Net::NetplayPhaseRuntime_IsLockstepPhase(rollbackPhase);
    out->phase = rollbackPhase;
    out->target_tick_scale = pacingSnap.target_scale;
    out->current_tick_scale = pacingSnap.current_scale;
    out->frame_origin_abs = s_frameOriginAbs;
    out->baseline_crc = s_baselineCRC;
    out->config_hash = s_configHash;
    out->handoff_delay = s_handoffDelay;
    out->handoff_budget = s_handoffBudget;
    out->remote_announced_delay = Net::DelayPolicy_GetRemoteAnnouncedDelay();
    out->stall_threshold = Net::DelayPolicy_GetStallThreshold();
    out->remote_inputs_received = s_remoteInputsReceived;
    out->packets_dispatched = s_packetsDispatched;
}

} // namespace Rollback
