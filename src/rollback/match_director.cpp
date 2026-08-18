/**
 * Alice Senki 2 - Match Director (re0.7 M6, plan §2.6)
 *
 * Implements the PRESERVED `OnlineWiring_*` facade (inventory §2.3) as the
 * match director. Replaces src/rollback/online_wiring.cpp at the M6 cutover.
 *
 * Responsibilities (§2.6, thin by design — ordering, not policy):
 *   - Startup: GameplayStart commit → engine arm (first match of the
 *     session) or ROTATE (rematch: RollbackSession_Begin under a higher
 *     epoch rotates the armed engine — no engine teardown between matches,
 *     the canonical frame counter never resets, INV-15/G2).
 *   - Match end: suspend-between-matches (engine survives through the
 *     winscreen / continue prompt); full RollbackSession_End only on the
 *     director-ordered SESSION teardown (§2.6.4).
 *   - INV-9 match-end ladder: WinScreenExit → PostMatchDecision →
 *     EpochAlign commits are consumed strictly in order; a later-step commit
 *     is held (the barrier keeps re-acking it) until the earlier step is
 *     committed locally.
 *   - match_exit_pending: derived from MatchLifecycle and mirrored into the
 *     engine's exact-input window (§2.7.6, INV-25) — replaces the M4
 *     conservative stand-in.
 *   - fx/spectator/palette/lifecycle event fan-out in the fixed order.
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
#include "patches/frame_scheduler.h"
#include "net/packet_router.h"
#include "net/match_lifecycle.h"
#include "net/netplay_menu_controller.h"
#include "net/session2.h"
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
#include "net/churn_pause.h"
#include "net/game_settings_sync.h"
#include "net/player_side_mapping.h"
#include "net/charsel_sync.h"
#include "net/winscreen_sync.h"
#include "net/pause_handler.h"
#include "net/session_exit.h"
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
// GameplayStart). Each peer proposes on reaching the FIRST interactive
// post-intro boundary; the commit (both proposed + acked) is the release.
static bool     s_startupBarrierEntered    = false;
static bool     s_startupProposed          = false;
static bool     s_startupReleased          = false;
static uint32_t s_startupBlockedLogCounter = 0;
static uint32_t s_introHoldLogCounter      = 0;

// Phase tracking for before/after logging
static Net::MatchLifecyclePhase s_lastLifecyclePhase = Net::MatchLifecyclePhase::Inactive;
static int      s_lastActiveDelay        = -1;
static int      s_lastRollbackBudget     = -1;

// Rollback start guard — prevent double-starting
static bool     s_rollbackBeginPending   = false;

// match_exit_pending mirror (M6, §2.7.6): last value pushed to the engine.
static bool     s_matchExitPendingMirror = false;

// ── INV-9 match-end ladder (M6, §4.4) ───────────────────────────────────────
// Armed when a match ends; disarmed when the ladder completes (EpochAlign
// consumed), on disconnect, or by the fail-open watchdog (a wedged gate must
// degrade into the §4.6 recovery ladder, never wedge a session — INV-12).
static bool     s_ladderArmed        = false;
static bool     s_ladderWseConsumed  = false;
static bool     s_ladderPmdConsumed  = false;
static uint32_t s_ladderHeldTicks    = 0;
static uint32_t s_ladderHeldLogCount = 0;
constexpr uint32_t kLadderFailOpenTicks = 600;   // ≈10 s at 60 Hz

// ── M7 (F-7): lockstep-derived post-match intent expectation ────────────────
// Registered by continue_flow at resolution (Rematch | CharselRestart);
// compared against the remote PostMatchDecision proposal while the boundary
// is live. Divergence between the two LOCKSTEP-DERIVED values = divergent
// lockstep streams = fail-closed terminal (worse than a desync — playing a
// rematch on divergent streams is a guaranteed desync). User-action intents
// are exempt (§ online_wiring.h note).
static uint8_t  s_expectedPmdIntent  = 0;   // PostMatchIntentWire, 0 = none
static bool     s_pmdContradictionFired = false;

static void ClearExpectedPostMatchIntent() {
    s_expectedPmdIntent = 0;
    s_pmdContradictionFired = false;
}

static void ArmMatchEndLadder(const char* reason) {
    // Open a fresh boundary FIRST: bumps the generation and clears the
    // boundary-scoped barriers unconditionally, so this boundary can never
    // inherit a stale proposal from the previous one (which happened whenever
    // the previous boundary exited by any route other than ladder-complete).
    Net::TransitionBarrier_BeginBoundary(reason);
    if (s_ladderArmed) return;
    s_ladderArmed = true;
    s_ladderWseConsumed = false;
    s_ladderPmdConsumed = false;
    s_ladderHeldTicks = 0;
    s_ladderHeldLogCount = 0;
    ClearExpectedPostMatchIntent();
    NetplayLog_Write("LADDER", -1,
        "Match-end ladder ARMED (%s): WinScreenExit -> PostMatchDecision -> EpochAlign",
        reason ? reason : "?");
}

static void DisarmMatchEndLadder(const char* reason) {
    if (!s_ladderArmed) return;
    s_ladderArmed = false;
    s_ladderHeldTicks = 0;
    ClearExpectedPostMatchIntent();
    NetplayLog_Write("LADDER", -1,
        "Match-end ladder disarmed (%s): wse=%d pmd=%d",
        reason ? reason : "?",
        s_ladderWseConsumed ? 1 : 0,
        s_ladderPmdConsumed ? 1 : 0);
}

static bool IsInteractiveRollbackPhase() {
    return s_rollbackActive &&
           Net::NetplayPhaseRuntime_IsInteractivePacingPhase(
               Net::NetplayPhaseRuntime_GetPhase());
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
    // never satisfy the next match's release.
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
    if (payloadLen == 0 || !payload) {
        LogGameplayPacketAnomaly("Empty InputStream", Net::PacketType::InputStream, payloadLen, 1);
        return;
    }

    if (!s_rollbackActive) {
        static uint32_t s_preLiveStreamDrops = 0;
        s_preLiveStreamDrops++;
        if (s_preLiveStreamDrops <= 5 || (s_preLiveStreamDrops % 120) == 0) {
            NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                "Dropping pre-live/suspended InputStream while rollback dispatch is inactive: "
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
// AI-learning session guard (SAVESTATE_AUDIT F1, P0)
// ============================================================================
// The "CPU learning" config byte (AI_PatternModeEnabled, 0x8E940D) gates
// AI_RecordPattern — which runs for HUMAN players too, twice per sim tick,
// and consumes CRT rand() data-dependently off four cross-frame statics
// (0x76C5D8..0x76C5E7) and per-matchup learning blocks loaded from each
// peer's LOCAL <A>vs<B> files. Divergent local files/statics therefore
// diverge the shared RNG stream — desync class F1, the worst in the audit.
//
// Fix: force the byte to 0 for the DURATION of the netplay session.
//
// Why the guard lives in the match director and arms at the startup handoff
// (not at RollbackSession_Begin): the deterministic intro runs sim ticks
// BEFORE the engine is armed (PrepareBaselineForInteractiveRelease →
// intro → TryStartRollbackSession), and AI_RecordPattern already runs during
// those ticks. Arming at Begin would leave the intro exposed and the peers
// could diverge before frame 0. The handoff is the last director-owned point
// before any mod-owned sim tick.
//
// The guard also zeroes the four statics: they are hashed by GameSnapshot
// now (they gate rand(), so INV-22 makes them SIM), and each peer carries
// stale values from its own offline matches — without normalization the very
// first SyncHash would false-alarm. Zeroing runs AFTER the baseline restore
// (the baseline slot may hold pre-guard values). With the byte forced to 0
// nothing reads or writes the statics for the whole session, so both peers
// hold zeros throughout.
//
// Restore happens on SESSION teardown only (StopRollbackSession(teardown),
// disconnect, shutdown) — never at match boundaries, so rematch substate-0
// learning-block loads are killed at their single gate too. Known accepted
// residuals (documented in SAVESTATE_AUDIT §8): (a) the FIRST match's
// substate-0 block load precedes the handoff and may leave inert blocks
// loaded — every reader/writer is behind the forced-0 gate, and the blocks
// are recovered by the next offline match-end save/free after restore;
// (b) opening the vanilla options menu mid-session and saving would re-write
// the byte — not reachable while a netplay session owns the frontend.
static bool    s_aiLearnForced = false;
static uint8_t s_aiLearnSavedByte = 0;

static void AiLearnGuard_Force(const char* where) {
    if (!s_aiLearnForced) {
        s_aiLearnSavedByte = ReadMemory<uint8_t>(ADDR_AI_PATTERN_MODE);
        s_aiLearnForced = true;
        WriteMemory<uint8_t>(ADDR_AI_PATTERN_MODE, 0);
        NetplayLog_Write("HANDOFF", -1,
            "AI-learning guard ARMED (%s): byte_8E940D %u -> 0 for the session (F1)",
            where ? where : "?", (unsigned)s_aiLearnSavedByte);
    }
    // Always (re-)zero the statics: a baseline restore may have re-written
    // them from a pre-guard capture, and the first hashed capture must see
    // the normalized values on both peers.
    static const uint8_t zeros[AI_LEARN_STATICS_SIZE] = {};
    WriteMemoryBlockSafe((void*)ADDR_AI_LEARN_STATICS, zeros, sizeof(zeros));
}

static void AiLearnGuard_Restore(const char* why) {
    if (!s_aiLearnForced) {
        return;
    }
    s_aiLearnForced = false;
    WriteMemory<uint8_t>(ADDR_AI_PATTERN_MODE, s_aiLearnSavedByte);
    NetplayLog_Write("TEARDOWN", -1,
        "AI-learning guard RESTORED (%s): byte_8E940D -> %u",
        why ? why : "?", (unsigned)s_aiLearnSavedByte);
}

// ============================================================================
// Bootstrap → Rollback Handoff
// ============================================================================

// Mirrors savestate.cpp ComputeMainChecksum(): CRC of {main-region CRC,
// effect_index}. The 2026-08-17 run compared a RAW region CRC against the
// baseline's FOLDED checksum here and logged "match=NO" for a restore the
// savestate layer itself had verified byte-exact ("RESTORED OK ... match") —
// apples to oranges, not a failed restore. Comparisons against
// bootSnap.local_baseline_crc must use this folded form.
static uint32_t ComputeBaselineComparableCRC() {
    struct ChecksumParts {
        uint32_t main_crc;
        uint32_t effect_index;
    } parts{};
    parts.main_crc = CalcCRC32(
        (const void*)ADDR_MATCH_BASE,
        (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
    parts.effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
    return CalcCRC32(&parts, sizeof(parts));
}

static bool PrepareBaselineForInteractiveRelease() {
    if (s_liveReleaseArmed) {
        return true;
    }

    const Net::LockedMatchConfig* config = Net::PregameSync_GetLockedConfig();
    if (!config) {
        NetplayLog_Write("HANDOFF", -1, "ERROR: No locked config available for startup handoff");
        LOG_ERROR("[MatchDirector] No locked config for startup handoff");
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
        "=== BOOTSTRAP -> INTRO HANDOFF === epoch=%u",
        Net::PregameSync_GetCurrentEpoch());
    NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
        "Config hash=0x%08X baseline_crc=0x%08X bootstrap_frame_abs=%u gameplay_start_host_game_abs_frame=%d",
        s_configHash,
        s_baselineCRC,
        bootSnap.bootstrap_frame_abs,
        bootSnap.gameplay_start_host_game_abs_frame);

    // Preserve hard startup alignment before deterministic intro runs.
    // Folded form (main CRC + effect_index) — the like-for-like comparand of
    // s_baselineCRC (see ComputeBaselineComparableCRC above).
    uint32_t preRestoreCRC = ComputeBaselineComparableCRC();
    NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
        "Pre-restore CRC=0x%08X (baseline=0x%08X match=%s)",
        preRestoreCRC,
        s_baselineCRC,
        (preRestoreCRC == s_baselineCRC) ? "YES" : "NO");

    if (Savestate_RestoreRollbackBaseline()) {
        WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, 0);
        WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, 0);

        uint32_t postRestoreCRC = ComputeBaselineComparableCRC();
        NetplayLog_Write("HANDOFF", 0,
            "Baseline RESTORED before intro: crc=0x%08X match=%s sim=0 writeIdx=0",
            postRestoreCRC,
            (postRestoreCRC == s_baselineCRC) ? "YES" : "NO");
    } else {
        NetplayLog_Write("HANDOFF", preIntroGameAbsFrame,
            "WARNING: Baseline restore FAILED before intro handoff");
        LOG_WARN("[MatchDirector] Baseline restore failed before intro handoff");
    }

    // F1 guard: after the baseline restore (which may re-write the statics
    // from a pre-guard capture), before the deterministic intro's first tick.
    AiLearnGuard_Force("startup handoff");

    ResetStartupBarrierState("interactive release armed");
    s_liveReleaseArmed = true;

    NetplayLog_Write("STARTUP", GetStartupLogFrame(),
        "Startup barrier armed: deterministic intro runs locally; "
        "rollback advance deferred until mutual post-intro interactive release");
    return true;
}

static bool TryStartRollbackSession() {
    if (s_rollbackActive) return true;  // Already active

    const Net::LockedMatchConfig* config = Net::PregameSync_GetLockedConfig();
    if (!config) {
        NetplayLog_Write("HANDOFF", -1, "ERROR: No locked config available for rollback start");
        LOG_ERROR("[MatchDirector] No locked config for rollback session start");
        return false;
    }

    Net::PregameBootstrapInfo bootSnap{};
    Net::PregameSync_GetBootstrapInfo(&bootSnap);

    const int visibleDelay = Net::DelayPolicy_GetActiveDelay();
    const int effectiveDelay = Net::DelayPolicy_GetEffectiveLocalDelay();
    int rollbackBudget = Net::DelayPolicy_GetRollbackBudget();

    Net::SessionRole role = Net::Session_GetRole();
    bool isHost = (role == Net::SessionRole::Host);
    int localPlayer = Net::PlayerMapping_DeriveFromRole(config->host_side, isHost);

    // Player mapping was owned by the retired gameplay_bridge; the director
    // sets and verifies it directly now (M6, gameplay_bridge deleted).
    Net::PlayerMapping_SetAssignment(localPlayer);
    int remotePlayer = Net::PlayerMapping_GetRemoteGameSlot();

    RollbackSessionConfig rbConfig{};
    rbConfig.local_player = localPlayer;
    rbConfig.remote_player = remotePlayer;
    rbConfig.initial_delay = effectiveDelay;
    rbConfig.rollback_budget = rollbackBudget;
    rbConfig.baseline_checksum = s_baselineCRC ? s_baselineCRC : bootSnap.local_baseline_crc;
    const int32_t bootstrapFrame = (int32_t)bootSnap.bootstrap_frame_abs;
    const int32_t interactiveFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    rbConfig.frame_origin_abs = interactiveFrame;

    s_configHash = Net::LockedMatchConfig_Hash(config);
    s_baselineCRC = rbConfig.baseline_checksum;
    s_handoffDelay = effectiveDelay;
    s_handoffBudget = rollbackBudget;
    s_frameOriginAbs = interactiveFrame;

    NetplayLog_Write("HANDOFF", interactiveFrame,
        "=== INTERACTIVE RELEASE -> ROLLBACK START === epoch=%u",
        Net::PregameSync_GetCurrentEpoch());
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
        "Local delay visible=%d effective=%d rollback_budget=%d remote_visible=%d remote_effective=%d",
        visibleDelay,
        effectiveDelay,
        rollbackBudget,
        Net::DelayPolicy_GetRemoteAnnouncedDelay(),
        Net::DelayPolicy_GetEffectiveRemoteDelay());
    Net::DelayPolicy_LogDelayMap("rollback handoff");

    // Arm (first match) or rotate (rematch — the engine2 adapter rotates the
    // armed engine when Begin arrives under a higher epoch, §2.6.5).
    bool ok = RollbackSession_Begin(rbConfig);
    if (!ok) {
        NetplayLog_Write("HANDOFF", interactiveFrame,
            "ERROR: RollbackSession_Begin FAILED");
        LOG_ERROR("[MatchDirector] RollbackSession_Begin failed");
        Net::PlayerMapping_Clear();
        return false;
    }

    s_rollbackStarted = true;
    s_rollbackActive = true;
    s_gameplayActive = Net::NetplayPhaseRuntime_IsInteractivePacingPhase(
        Net::NetplayPhaseRuntime_GetPhase());
    s_liveReleaseArmed = false;
    s_matchExitPendingMirror = false;
    RollbackSession_SetMatchExitPending(false);
    // Clear any stale startup hold pulse now that rollback owns gameplay.
    InputSyncHooks_SetTimesyncFreeze(false);

    Net::DelayPolicy_OnRollbackApplied(effectiveDelay);

    // Reset per-match digest history/desync flags so warnings don't leak
    // across rematch/new-session frame-number reuse windows.
    RollbackDebug_ResetSession();
    Net::SyncTrace_ResetSession("rollback start");

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
    LOG_INFO("[MatchDirector] Rollback session started: P%d vs P%d, visible_delay=%d effective_delay=%d budget=%d remote_delay=%d baseline=0x%08X",
        localPlayer + 1,
        remotePlayer + 1,
        visibleDelay,
        effectiveDelay,
        rollbackBudget,
        Net::DelayPolicy_GetRemoteAnnouncedDelay(),
        s_baselineCRC);

    Net::SpectatorRuntime_OnRollbackStarted(rbConfig.frame_origin_abs);

    return true;
}

// ============================================================================
// Suspension / teardown
// ============================================================================

static bool ShouldPreservePaletteRuntimeForPostMatch(const char* reason) {
    if (!reason) {
        return false;
    }

    return strstr(reason, "match ended") != nullptr ||
           strcmp(reason, "match end event") == 0;
}

// Match-boundary or session-end stop. `sessionTeardown=false` = the match
// ended but the session lives on (winscreen → rematch): the engine2 backend
// keeps its engine armed so the next Begin rotates the epoch (§2.6.3/§2.6.5).
// `sessionTeardown=true` = director-ordered SESSION teardown → full engine
// end (§2.6.4).
static void StopRollbackSession(const char* reason, bool sessionTeardown) {
    if (!s_rollbackActive) return;

    int32_t frame = RollbackSession_GetCurrentFrame();

    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);

    NetplayLog_Write("TEARDOWN", frame,
        "=== ROLLBACK SESSION %s ===",
        sessionTeardown ? "ENDING (session teardown)" : "SUSPENDING (match boundary)");
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

    RollbackDebug_LogSessionSummary(reason ? reason : "rollback stop");
    if (RollbackDebug_IsDesyncDetected()) {
        int32_t desyncFrame = RollbackDebug_GetDesyncFrame();
        NetplayLog_Write("TEARDOWN", frame,
            "WARNING: Desync was detected at frame %d", desyncFrame);
    }

    RollbackSession_SetMatchExitPending(false);
    s_matchExitPendingMirror = false;

    if (sessionTeardown) {
        RollbackSession_End();
        Net::PlayerMapping_Clear();
        AiLearnGuard_Restore(reason ? reason : "session teardown");
    } else {
        // Match boundary: the session lives on — the F1 guard stays armed so
        // the rematch's substate-0 learning-block load is killed at its gate.
        RollbackSession_SuspendBetweenMatches(reason ? reason : "match boundary");
    }
    FrameScheduler_OnSessionReset(reason ? reason : "rollback stop");
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
        "=== ROLLBACK SESSION %s ===",
        sessionTeardown ? "ENDED" : "SUSPENDED");
    LOG_INFO("[MatchDirector] Rollback session %s: %s",
        sessionTeardown ? "ended" : "suspended",
        reason ? reason : "unknown");
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

// Mid-session local input-delay hotkeys `-`/`=` (plan §9 Q3; INV-23/B-7:
// peer-local knob, no wire message).
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
        LOG_NETPLAY(LOG_INFO, "[MatchDirector] Local input delay %d -> %d (hotkey)", cur, target);
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

// M6 (§2.7.6/INV-25): derive match_exit_pending from MatchLifecycle and
// mirror it into the engine's exact-input window. The exit route can fire
// from MatchEnd (Mode 8 Sub 5) — any tick that can execute the mode-8 exit
// router (Game_ChangeMode → Handle_ReleaseAll) must be exact-input.
static void UpdateMatchExitPendingMirror() {
    if (!s_rollbackActive) {
        return;
    }
    Net::MatchLifecycleSnapshot lifeSnap{};
    Net::MatchLifecycle_GetSnapshot(&lifeSnap);
    // Route-byte sentinel fix (2026-08-17, deep-rollback cell R12/DD10):
    // the vanilla route byte (match+10) idles at 0xFF during NORMAL
    // gameplay in this build — `route != 0` read that as "exit armed" and
    // held the engine in exact-input (no-prediction) mode for the ENTIRE
    // match. Invisible on same-frame loopback (remote actuals present at
    // the frontier anyway), but the moment inputs arrived late the pair
    // degraded to full lockstep: zero rollbacks, sim ~30 fps,
    // Stall(LifecycleBoundary) spam. Only a REAL route value arms the
    // exact window; 0 and 0xFF are "no route".
    const uint8_t route = lifeSnap.match_end_route;
    const bool exitPending =
        lifeSnap.phase == Net::MatchLifecyclePhase::MatchEnd ||
        lifeSnap.phase == Net::MatchLifecyclePhase::PostMatchRoute ||
        (route != 0 && route != 0xFF);
    if (exitPending != s_matchExitPendingMirror) {
        s_matchExitPendingMirror = exitPending;
        RollbackSession_SetMatchExitPending(exitPending);
        NetplayLog_Write("LIFE", RollbackSession_GetCurrentFrame(),
            "match_exit_pending -> %d (phase=%s route=%u)",
            exitPending ? 1 : 0,
            Net::MatchLifecyclePhaseName(lifeSnap.phase),
            lifeSnap.match_end_route);
    }
}

static void CheckLifecyclePhase() {
    Net::MatchLifecyclePhase curPhase = Net::MatchLifecycle_GetPhase();

    if (curPhase != s_lastLifecyclePhase) {
        LogLifecycleTransition(s_lastLifecyclePhase, curPhase);

        // === Handle phase transitions ===

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
                s_gameplayActive = false;
                Net::PauseHandler_OnPauseEnter();
                NetplayLog_Write("LIFE", RollbackSession_GetCurrentFrame(),
                    "Pause active — rollback session retained; interactive pacing paused");
            } else if (curPhase == Net::MatchLifecyclePhase::RoundTransition) {
                s_gameplayActive = false;
                NetplayLog_Write("LIFE", RollbackSession_GetCurrentFrame(),
                    "Round transition — rollback session retained; non-interactive in-match phase continues");
            } else if (curPhase == Net::MatchLifecyclePhase::MatchEnd) {
                // Match end — suspend at the boundary; the SESSION (and on
                // engine2 the engine itself) survives into the winscreen.
                StopRollbackSession("match ended", /*sessionTeardown=*/false);
            } else if (curPhase == Net::MatchLifecyclePhase::DisconnectRecovery) {
                StopRollbackSession("disconnect during gameplay", /*sessionTeardown=*/true);
            }
        }

        // Match end from any state
        if (curPhase == Net::MatchLifecyclePhase::MatchEnd &&
            s_lastLifecyclePhase != Net::MatchLifecyclePhase::MatchEnd) {
            Net::MatchLifecycleSnapshot lifeSnap{};
            Net::MatchLifecycle_GetSnapshot(&lifeSnap);
            Net::SetTracker_RecordResult(lifeSnap.winner);
            // Arm the INV-9 match-end ladder for this boundary.
            ArmMatchEndLadder("match end");
        }

        if (curPhase == Net::MatchLifecyclePhase::MatchEnd && s_rollbackActive) {
            StopRollbackSession("match ended (non-gameplay)", /*sessionTeardown=*/false);
        }

        // Begin win-screen lockstep as soon as the match ends so both peers
        // stay aligned through the Mode 8 -> Mode 9 transition.
        if (curPhase == Net::MatchLifecyclePhase::MatchEnd &&
            s_lastLifecyclePhase != Net::MatchLifecyclePhase::MatchEnd) {
            Net::WinScreenSync_Begin();
            NetplayLog_Write("LIFE", -1,
                "Match ended — win-screen lockstep armed before Mode 9 entry");
        }

        // Disconnect from any state
        if (curPhase == Net::MatchLifecyclePhase::DisconnectRecovery && s_rollbackActive) {
            StopRollbackSession("disconnect", /*sessionTeardown=*/true);
        }

        // Entering WinScreenActive — palette cleanup only
        if (curPhase == Net::MatchLifecyclePhase::WinScreenActive &&
            s_lastLifecyclePhase != Net::MatchLifecyclePhase::WinScreenActive) {
            Net::NetplayPaletteRuntime_OnWinScreenEnter();
            NetplayLog_Write("LIFE", -1,
                "Win screen entered — rollback gameplay suspended, post-match lockstep active");
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
                StopRollbackSession("lifecycle inactive", /*sessionTeardown=*/false);
            }
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
            // Do NOT reset SetTracker here — Inactive is reached on both
            // rematch (charsel return) and real session end; the session-end
            // reset lives in OnlineWiring_OnDisconnect.
        }

        // MatchInit — potential round restart, re-enable gameplay flag
        if (curPhase == Net::MatchLifecyclePhase::MatchInit && s_rollbackStarted) {
            if (s_lastLifecyclePhase == Net::MatchLifecyclePhase::RoundTransition) {
                Net::NetplayPaletteRuntime_OnRoundRestart();
            }
            NetplayLog_Write("LIFE", -1,
                "New round init — same rollback session continues: frame_origin_abs=%d startup_released=%d pending_frame=%d",
                s_frameOriginAbs,
                s_startupReleased ? 1 : 0,
                RollbackSession_HasPendingFrame() ? 1 : 0);
        }

        if (curPhase == Net::MatchLifecyclePhase::IntroActive) {
            NetplayLog_Write("LIFE", -1,
                "Intro active — same rollback session continues: startup_barrier_active=%d pending_frame=%d release_armed=%d",
                (!s_startupReleased && (s_liveReleaseArmed || s_rollbackActive)) ? 1 : 0,
                RollbackSession_HasPendingFrame() ? 1 : 0,
                s_liveReleaseArmed ? 1 : 0);
        }

        s_lastLifecyclePhase = curPhase;
    }
}

// ============================================================================
// INV-9 match-end ladder gate (M6, §4.4)
// ============================================================================

static bool LadderStepSatisfied(Net::NetTransitionKind kind, bool consumedFlag) {
    return consumedFlag || Net::TransitionBarrier_IsCommitted(kind);
}

bool OnlineWiring_MatchEndLadderAllows(Net::NetTransitionKind kind) {
    if (!s_ladderArmed) {
        return true;   // no boundary pending (e.g. session-start EpochAlign)
    }
    switch (kind) {
        case Net::NetTransitionKind::WinScreenExit:
            return true;
        case Net::NetTransitionKind::PostMatchDecision:
            return LadderStepSatisfied(Net::NetTransitionKind::WinScreenExit,
                                       s_ladderWseConsumed);
        case Net::NetTransitionKind::EpochAlign: {
            const bool ok =
                LadderStepSatisfied(Net::NetTransitionKind::WinScreenExit,
                                    s_ladderWseConsumed) &&
                LadderStepSatisfied(Net::NetTransitionKind::PostMatchDecision,
                                    s_ladderPmdConsumed);
            if (!ok) {
                s_ladderHeldLogCount++;
                if (s_ladderHeldLogCount <= 5 || (s_ladderHeldLogCount % 120) == 0) {
                    NetplayLog_Write("LADDER", -1,
                        "EpochAlign commit HELD by match-end ladder: wse=%d/%d pmd=%d/%d held_ticks=%u",
                        s_ladderWseConsumed ? 1 : 0,
                        Net::TransitionBarrier_IsCommitted(Net::NetTransitionKind::WinScreenExit) ? 1 : 0,
                        s_ladderPmdConsumed ? 1 : 0,
                        Net::TransitionBarrier_IsCommitted(Net::NetTransitionKind::PostMatchDecision) ? 1 : 0,
                        s_ladderHeldTicks);
                }
            }
            return ok;
        }
        default:
            return true;   // kinds outside the ladder are unaffected
    }
}

void OnlineWiring_MatchEndLadderNotifyConsumed(Net::NetTransitionKind kind) {
    if (!s_ladderArmed) {
        return;
    }
    switch (kind) {
        case Net::NetTransitionKind::WinScreenExit:
            s_ladderWseConsumed = true;
            break;
        case Net::NetTransitionKind::PostMatchDecision:
            s_ladderPmdConsumed = true;
            break;
        case Net::NetTransitionKind::EpochAlign: {
            // Ladder complete: retire this boundary's earlier-step slots so
            // stale commits can never satisfy the NEXT boundary. (F-7: the
            // intent-contradiction terminal is live since M7 — see
            // UpdatePostMatchIntentGuard; the wire intents were unified so
            // both lockstep-derived routes propose distinct values.)
            Net::TransitionBarrier_ConsumeCommit(Net::NetTransitionKind::WinScreenExit);
            Net::TransitionBarrier_ConsumeCommit(Net::NetTransitionKind::PostMatchDecision);
            Net::TransitionBarrier_Clear(Net::NetTransitionKind::WinScreenExit,
                                         "match-end ladder complete");
            Net::TransitionBarrier_Clear(Net::NetTransitionKind::PostMatchDecision,
                                         "match-end ladder complete");
            DisarmMatchEndLadder("ladder complete (EpochAlign consumed)");
            break;
        }
        default:
            break;
    }
}

void OnlineWiring_SetExpectedPostMatchIntent(uint8_t intentWire) {
    s_expectedPmdIntent = intentWire;
    s_pmdContradictionFired = false;
    NetplayLog_Write("LADDER", -1,
        "Lockstep-derived post-match intent registered: %s",
        Net::PostMatchIntentWireName((Net::PostMatchIntentWire)intentWire));
}

// M7 (F-7): fail-closed compare of the remote PostMatchDecision proposal
// against the lockstep-derived local answer. Only the two lockstep-derived
// values (Rematch / CharselRestart) participate: both peers compute the
// resolution from the SAME consumed lockstep stream (F-5), so a
// contradiction between them can only mean the streams diverged — playing
// on would guarantee a desynced rematch. User-action intents
// (ReturnToSession / Disconnect) come from the post-match menu, not from a
// derivation, and are exempt (a strict compare there would kill healthy
// sessions on legitimate asymmetric choices).
// Only these two are DERIVED FROM THE SHARED LOCKSTEP STREAM, so only these
// two may be compared fail-closed. RecoveryRestart is deliberately excluded:
// it is what every non-lockstep exit announces (ForceExitToCharsel,
// WinScreenSync_Abort, the auto-rematch heuristic, cross-phase restart).
// Before it existed those routes announced CharselRestart, and this guard
// killed healthy sessions whenever one peer merely RECOVERED while the other
// cleanly resolved Rematch — an asymmetric recovery is not evidence of
// divergent streams.
static bool IsLockstepDerivedIntent(uint8_t intent) {
    return intent == (uint8_t)Net::PostMatchIntentWire::Rematch ||
           intent == (uint8_t)Net::PostMatchIntentWire::CharselRestart;
}

static void UpdatePostMatchIntentGuard() {
    if (s_pmdContradictionFired ||
        s_expectedPmdIntent == 0 ||
        !IsLockstepDerivedIntent(s_expectedPmdIntent)) {
        return;
    }
    if (!Net::TransitionBarrier_RemoteProposed(Net::NetTransitionKind::PostMatchDecision)) {
        return;
    }
    const uint8_t remote =
        Net::TransitionBarrier_GetRemoteIntent(Net::NetTransitionKind::PostMatchDecision);
    if (!IsLockstepDerivedIntent(remote) || remote == s_expectedPmdIntent) {
        return;
    }

    s_pmdContradictionFired = true;
    char reason[160];
    snprintf(reason, sizeof(reason),
        "Post-match decision contradiction: local lockstep resolved %s, peer proposed %s "
        "(divergent lockstep streams)",
        Net::PostMatchIntentWireName((Net::PostMatchIntentWire)s_expectedPmdIntent),
        Net::PostMatchIntentWireName((Net::PostMatchIntentWire)remote));
    NetplayLog_Write("LADDER", -1, "F-7 TERMINAL: %s", reason);
    NetplayLog_Flush();
    LOG_ERROR("[MatchDirector] %s", reason);

    // Fail closed (INV-20): typed terminal first (reasoned Disconnect on the
    // wire), then the match unwind and the single UI funnel — the same order
    // the supervisor terminals use.
    Net::Session2_Terminate(Net::Session2TerminalReason::ProtocolViolation, reason);
    if (Net::MatchLifecycle_GetPhase() != Net::MatchLifecyclePhase::DisconnectRecovery) {
        Net::MatchLifecycle_OnDisconnect(reason);
    }
    NetMenu::HandleDisconnection(reason);
}

// Fail-open watchdog: a wedged ladder degrades into the §4.6 recovery ladder
// instead of wedging the session (INV-12). Counted only while an EpochAlign
// commit is actually being held.
static void UpdateLadderWatchdog() {
    if (!s_ladderArmed) {
        return;
    }
    if (Net::TransitionBarrier_IsCommitted(Net::NetTransitionKind::EpochAlign) &&
        !OnlineWiring_MatchEndLadderAllows(Net::NetTransitionKind::EpochAlign)) {
        s_ladderHeldTicks++;
        if (s_ladderHeldTicks >= kLadderFailOpenTicks) {
            NetplayLog_Write("LADDER", -1,
                "Match-end ladder FAIL-OPEN after %u held ticks (earlier steps never "
                "committed) — releasing gate; recovery ladder owns convergence",
                s_ladderHeldTicks);
            DisarmMatchEndLadder("fail-open watchdog");
        }
    } else {
        s_ladderHeldTicks = 0;
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
    s_matchExitPendingMirror = false;
    s_ladderArmed = false;
    s_ladderWseConsumed = false;
    s_ladderPmdConsumed = false;
    ClearExpectedPostMatchIntent();
    ResetStartupBarrierState("init");

    Net::ChurnPause_Init();
    StressHooks_Init();
    Net::SetTracker_Init();
    Net::WinScreenSync_Init();
    Net::PauseHandler_Init();
    Net::SessionExit_Init();

    LOG_INFO("[MatchDirector] Initialized");
    NetplayLog_Write("WIRING", -1, "MatchDirector initialized (OnlineWiring facade)");
}

void OnlineWiring_Shutdown() {
    if (s_rollbackActive) {
        StopRollbackSession("mod shutdown", /*sessionTeardown=*/true);
    } else {
        // A suspended engine still holds resources — end it on shutdown.
        RollbackSession_End();
        AiLearnGuard_Restore("mod shutdown");
    }
    Net::ChurnPause_Shutdown();
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
    s_frameOriginAbs = -1;
    ResetStartupBarrierState("shutdown");
    DisarmMatchEndLadder("shutdown");
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

    // M6: exact-input window signal for the engine (§2.7.6, INV-25).
    UpdateMatchExitPendingMirror();

    // INV-9 ladder fail-open watchdog.
    UpdateLadderWatchdog();

    // F-7 lockstep-vs-barrier contradiction guard (M7).
    UpdatePostMatchIntentGuard();

    // Mid-session delay hotkeys (M5; peer-local, INV-23).
    UpdateDelayHotkeys();

    // Keep engine events/liveness flowing even when the input dispatcher is
    // stalled in lockstep/startup holds.
    if (s_rollbackActive) {
        s_backgroundPollCount++;
        Net::ChurnPause_OnRollbackPoll(
            s_gameplayActive && s_rollbackActive,
            RollbackSession_GetCurrentFrame());
        const bool pollOk = RollbackSession_PollSession();
        // Session death gating: tear down only when the session reports a
        // terminal or the ConnectionSupervisor reached its Dead verdict.
        // Interrupted is a freeze-and-wait condition, never a teardown.
        const bool supervisorDead = Net::ConnectionSupervisor_IsDead();
        if (!pollOk || supervisorDead) {
            s_backgroundPollFailures++;
            NetplayLog_Write("ENGINE", RollbackSession_GetCurrentFrame(),
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
            NetplayLog_Verbose("ENGINE", RollbackSession_GetCurrentFrame(),
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
            NetplayLog_Write("ENGINE", RollbackSession_GetCurrentFrame(),
                "Pending rollback frame still requires dispatcher-owned advance: phase=%s",
                Net::MatchLifecyclePhaseName(curPhase));
        }
    }

    // Startup gameplay-entry barrier: deterministic intro runs vanilla;
    // rollback-owned advance is held until BOTH peers commit GameplayStart.
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
                    "Session connected but live advance still gated: intro not finished (phase=%s)",
                    Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            }
        } else {
            s_introHoldLogCounter = 0;
        }

        if (sessionConnected && inPlayableGameplay && !s_startupProposed) {
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
    // SessionExit is deliberately NOT driven from here: MatchLifecycle_IsMatchOwned()
    // is MATCH-scoped, so this site never runs at character select -- where the
    // pause block and the quit gesture are both required. It runs from
    // ModOnFrame instead, every frame in every mode, and gates itself on
    // Session_IsConnected().

    Net::ChurnPause_FrameUpdate(
        s_gameplayActive && s_rollbackActive,
        s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1);

    // Periodic RTT/stats logging (every 5 seconds = 300 frames)
    if (s_rollbackActive && s_gameplayActive) {
        int32_t frame = RollbackSession_GetCurrentFrame();
        if (frame > 0 && frame % 300 == 0) {
            Net::ConnectionStats stats{};
            Net::Session_GetStats(&stats);

            Net::DelayPolicySnapshot dpSnap{};
            Net::DelayPolicy_GetSnapshot(&dpSnap);

            RollbackSessionSnapshot rbSnap{};
            RollbackSession_GetSnapshot(&rbSnap);

            FrameSchedulerSnapshot schedSnap{};
            FrameScheduler_GetSnapshot(&schedSnap);

            NetplayLog_Write("STATS", frame,
                "RTT=%.1fms jitter=%.1fms loss=%u/%u visible_delay=%d effective_delay=%d budget=%d slew_ppm=%d debt=%u",
                rbSnap.link_avg_ping > 0.0f ? rbSnap.link_avg_ping : stats.rtt_ms,
                rbSnap.link_jitter,
                stats.packets_lost, stats.packets_sent,
                dpSnap.active_delay,
                dpSnap.effective_local_delay,
                dpSnap.rollback_budget,
                schedSnap.slew_ppm,
                schedSnap.debt_frames);

            NetplayLog_Write("STATS", frame,
                "Rollbacks=%d maxdepth=%d frames_ahead=%.1f peer_depth=%u peer_produced=%u",
                rbSnap.rollback_count,
                rbSnap.max_rollback_distance,
                rbSnap.frames_ahead,
                rbSnap.peer_prediction_depth,
                rbSnap.peer_produced_frontier);
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
    ArmMatchEndLadder("match end event");
    if (s_rollbackActive) {
        StopRollbackSession("match end event", /*sessionTeardown=*/false);
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
        StopRollbackSession(effectiveReason, /*sessionTeardown=*/true);
    } else {
        // A suspended engine (between matches) still ends with the session.
        RollbackSession_End();
        Net::PlayerMapping_Clear();
        AiLearnGuard_Restore(effectiveReason);
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
    s_matchExitPendingMirror = false;
    DisarmMatchEndLadder("disconnect");
    FrameScheduler_OnSessionReset("disconnect");
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

    // Match-scoped reset for the next match. The engine itself stays armed
    // (suspended) on the engine2 backend — the next GameplayStart commit
    // rotates the epoch instead of re-arming (§2.6.5).
    if (s_rollbackActive) {
        StopRollbackSession("post-match rematch", /*sessionTeardown=*/false);
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
    s_lastActiveDelay = -1;
    s_lastRollbackBudget = -1;
    s_matchExitPendingMirror = false;
    FrameScheduler_OnSessionReset("rematch");
    Net::ChurnPause_ResetSession("rematch");
    Net::SyncTrace_ResetSession("rematch");
    Net::SyncTrace_SetIntegrityActive(false, "rematch");
    ResetStartupBarrierState("rematch");
    Net::SpectatorRuntime_OnMatchEnd("rematch");
    Net::NetplayPaletteRuntime_OnMatchEnd("rematch");

    NetplayLog_Write("POSTMATCH", -1,
        "New match reset complete after rematch selection");
}

void OnlineWiring_OnReturnToSession() {
    NetplayLog_Write("POSTMATCH", -1,
        "=== RETURN TO SESSION — exiting match flow ===");

    RematchCleanup_PrepareForNextMatch("post-match return to session");

    if (s_rollbackActive) {
        StopRollbackSession("return to session", /*sessionTeardown=*/false);
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
    s_matchExitPendingMirror = false;
    DisarmMatchEndLadder("return to session");
    FrameScheduler_OnSessionReset("return to session");
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
    FrameSchedulerSnapshot schedSnap{};
    FrameScheduler_GetSnapshot(&schedSnap);
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
    // INV-5: there is no tick-scale actuator anymore; report the scheduler's
    // effective speed in both fields (consumers show them as-is).
    out->target_tick_scale = schedSnap.speed_scale;
    out->current_tick_scale = schedSnap.speed_scale;
    out->frame_origin_abs = s_frameOriginAbs;
    out->baseline_crc = s_baselineCRC;
    out->config_hash = s_configHash;
    out->handoff_delay = s_handoffDelay;
    out->handoff_budget = s_handoffBudget;
    out->remote_announced_delay = Net::DelayPolicy_GetRemoteAnnouncedDelay();
    out->stall_threshold = 0;   // retired knob (§2.8.7); field kept for shape
    out->remote_inputs_received = s_remoteInputsReceived;
    out->packets_dispatched = s_packetsDispatched;
}

} // namespace Rollback
