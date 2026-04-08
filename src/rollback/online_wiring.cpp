/**
 * Alice Senki 2 - Online Rollback Wiring Implementation
 *
 * This is the integration glue that connects all subsystems:
 *   - Bootstrap → RollbackSession handoff
 *   - Packet routing for GameplayInput + StateDigest
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
#include "net/gameplay_bridge.h"
#include "net/match_lifecycle.h"
#include "net/set_tracker.h"
#include "net/pregame_sync.h"
#include "net/match_bootstrap.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/sync_policy.h"
#include "net/delay_policy.h"
#include "net/locked_match_config.h"
#include "net/enet_transport.h"
#include "net/player_side_mapping.h"
#include "net/winscreen_sync.h"
#include "net/pause_handler.h"
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
static bool     s_gameplayActive         = false;   // Currently in playable gameplay
static int32_t  s_handoffFrame           = -1;
static uint32_t s_baselineCRC            = 0;
static uint32_t s_configHash             = 0;
static int      s_handoffDelay           = 0;
static int      s_handoffBudget          = 0;
static int      s_remoteInputsReceived   = 0;
static int      s_packetsDispatched      = 0;

// Phase tracking for before/after logging
static Net::MatchLifecyclePhase s_lastLifecyclePhase = Net::MatchLifecyclePhase::Inactive;
static int      s_lastActiveDelay        = -1;
static int      s_lastRollbackBudget     = -1;

// Rollback start guard — prevent double-starting
static bool     s_rollbackBeginPending   = false;

// ============================================================================
// Packet Dispatch for Gameplay Phase
// ============================================================================

/// Packet callback that handles both pregame AND gameplay packets.
/// During gameplay, GameplayInput/StateDigest are dispatched here.
/// All other packets are forwarded to the pregame handler.
static void OnGameplayPacket(Net::PacketType type, const void* payload, size_t payloadLen) {
    switch (type) {
        case Net::PacketType::GekkoData: {
            // GekkoNet internal protocol data — buffer for GekkoNet to drain
            if (payloadLen == 0 || !payload) break;

            RollbackSession_BufferGekkoPacket(payload, payloadLen);
            s_packetsDispatched++;
            s_remoteInputsReceived++;

            NetplayLog_Verbose("GEKKO", RollbackSession_GetCurrentFrame(),
                "Buffered GekkoData packet (%zu bytes)", payloadLen);
            break;
        }

        case Net::PacketType::StateDigest: {
            if (payloadLen < sizeof(Net::StateDigestPayload)) break;
            auto* p = static_cast<const Net::StateDigestPayload*>(payload);
            RollbackDebug_OnRemoteDigest((int32_t)p->frame_number, p->crc32);

            NetplayLog_Verbose("DESYNC", (int32_t)p->frame_number,
                "Remote digest: crc=0x%08X", p->crc32);
            break;
        }

        case Net::PacketType::FrameSyncStatus: {
            if (payloadLen < sizeof(Net::FrameSyncStatusPayload)) break;
            auto* p = static_cast<const Net::FrameSyncStatusPayload*>(payload);
            RollbackDebug_OnRemoteFrameSyncStatus(
                p->current_frame,
                p->game_frame,
                p->remote_view_frame,
                p->confirmed_frame,
                p->predicted_frames,
                p->checksum);
            break;
        }

        default:
            // Check for win screen / pause packets
            if (type == Net::PacketType::WinScreenConfirm) {
                Net::WinScreenSync_OnRemoteConfirm();
                break;
            }
            if (type == Net::PacketType::PauseQuit) {
                Net::PauseHandler_OnRemotePauseQuit();
                break;
            }
            // Not a gameplay packet; unhandled at this level.
            // During gameplay, the pregame handler is not active,
            // so non-gameplay packets are just logged.
            NetplayLog_Verbose("PACKET", -1,
                "Unhandled packet type %u during gameplay", (unsigned)type);
            break;
    }
}

// ============================================================================
// Bootstrap → Rollback Handoff
// ============================================================================

static bool TryStartRollbackSession() {
    if (s_rollbackActive) return true;  // Already active

    // Get locked config from pregame sync
    const Net::LockedMatchConfig* config = Net::PregameSync_GetLockedConfig();
    if (!config) {
        NetplayLog_Write("HANDOFF", -1, "ERROR: No locked config available for rollback start");
        LOG_ERROR("[OnlineWiring] No locked config for rollback session start");
        return false;
    }

    // Get bootstrap snapshot for baseline info
    Net::MatchBootstrapSnapshot bootSnap{};
    Net::MatchBootstrap_GetSnapshot(&bootSnap);

    // Get delay policy values
    int activeDelay = Net::DelayPolicy_GetActiveDelay();
    int rollbackBudget = Net::DelayPolicy_GetAgreedRollbackBudget();

    // Determine local/remote player via PlayerMapping module
    Net::SessionRole role = Net::Session_GetRole();
    bool isHost = (role == Net::SessionRole::Host);
    int localPlayer = Net::PlayerMapping_DeriveFromRole(config->host_side, isHost);
    int remotePlayer = Net::PlayerMapping_GetRemoteGameSlot();

    // Build rollback session config
    RollbackSessionConfig rbConfig{};
    rbConfig.local_player = localPlayer;
    rbConfig.remote_player = remotePlayer;
    rbConfig.initial_delay = activeDelay;
    rbConfig.rollback_budget = rollbackBudget;
    rbConfig.baseline_checksum = bootSnap.local_baseline_crc;
    const int32_t bootstrapFrame = (int32_t)bootSnap.start_frame;
    const int32_t gameplayFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    rbConfig.start_frame = gameplayFrame;

    // Config hash for logging
    s_configHash = Net::LockedMatchConfig_Hash(config);
    s_baselineCRC = bootSnap.local_baseline_crc;
    s_handoffDelay = activeDelay;
    s_handoffBudget = rollbackBudget;
    s_handoffFrame = gameplayFrame;

    // --- LOG BEFORE/AFTER for handoff ---
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "=== BOOTSTRAP -> ROLLBACK HANDOFF ===");
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Config: p1_char=%u p1_pal=%u p2_char=%u p2_pal=%u stage=%u",
        config->p1_character, config->p1_palette,
        config->p2_character, config->p2_palette,
        config->stage_id);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "RNG seed=0x%08X session_seed=0x%08X config_hash=0x%08X",
        config->rng_seed, config->session_seed, s_configHash);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Player assignment: local=P%d remote=P%d (role=%s host_side=%u)",
        localPlayer + 1, remotePlayer + 1,
        (role == Net::SessionRole::Host) ? "Host" : "Join",
        config->host_side);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Baseline CRC=0x%08X bootstrap_frame=%d rollback_start_frame=%d",
        s_baselineCRC, bootstrapFrame, rbConfig.start_frame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Load barrier sim: local=%d remote=%d | baseline sim: local=%d remote=%d | start sim=%d",
        bootSnap.local_load_sim_frame,
        bootSnap.remote_load_sim_frame,
        bootSnap.local_baseline_sim_frame,
        bootSnap.remote_baseline_sim_frame,
        bootSnap.gameplay_start_sim_frame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Gameplay ownership delta: %d frames from bootstrap baseline",
        rbConfig.start_frame - bootstrapFrame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Active delay=%d rollback_budget=%d",
        activeDelay, rollbackBudget);

    Net::MatchLifecyclePhase phase = Net::MatchLifecycle_GetPhase();
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Lifecycle phase at handoff: %s",
        Net::MatchLifecyclePhaseName(phase));

    // ── Restore baseline savestate ──────────────────────────────────
    // Between the baseline capture and this handoff point, the game ran
    // uncontrolled frames through the vanilla dispatcher path (Match_ClearPerFrameTempData,
    // Frame_AdvanceSimulation, and potentially render-side state writes all modify
    // the main blob). The number of these frames differs between HOST and CLIENT
    // due to timing, causing the main state to diverge BEFORE GekkoNet even starts.
    // Restoring the agreed-upon baseline forces both sides to identical state.
    {
        uint32_t preRestoreCRC = CalcCRC32((const void*)ADDR_MATCH_BASE,
            (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
        NetplayLog_Write("HANDOFF", s_handoffFrame,
            "Pre-restore CRC=0x%08X (baseline was 0x%08X, delta=%s)",
            preRestoreCRC, s_baselineCRC,
            (preRestoreCRC == s_baselineCRC) ? "none" : "DIVERGED");

        if (Savestate_Load()) {
            // Force sim_frame=0 and writeIdx=0 for clean GekkoNet start
            WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, 0);
            WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, 0);
            rbConfig.start_frame = 0;
            s_handoffFrame = 0;

            uint32_t postRestoreCRC = CalcCRC32((const void*)ADDR_MATCH_BASE,
                (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
            NetplayLog_Write("HANDOFF", 0,
                "Baseline RESTORED: CRC=0x%08X (match=%s) sim=0 writeIdx=0",
                postRestoreCRC,
                (postRestoreCRC == s_baselineCRC) ? "YES" : "NO");
        } else {
            NetplayLog_Write("HANDOFF", s_handoffFrame,
                "WARNING: Baseline restore FAILED — starting from diverged state");
            LOG_WARN("[OnlineWiring] Baseline restore failed at handoff");
        }
    }

    // Register gameplay packet callback
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Registering gameplay packet callback");
    Net::Session_SetPacketCallback(OnGameplayPacket);

    // Start rollback session through GameplayBridge
    bool ok = Net::GameplayBridge_StartSession(rbConfig);
    if (!ok) {
        NetplayLog_Write("HANDOFF", s_handoffFrame,
            "ERROR: GameplayBridge_StartSession FAILED");
        LOG_ERROR("[OnlineWiring] GameplayBridge_StartSession failed");
        return false;
    }

    s_rollbackStarted = true;
    s_rollbackActive = true;
    s_gameplayActive = true;

    // Enable state digest for desync detection
    RollbackDebug_SetDigestEnabled(true);

    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "=== ROLLBACK SESSION STARTED SUCCESSFULLY ===");
    LOG_INFO("[OnlineWiring] Rollback session started: P%d vs P%d, delay=%d budget=%d baseline=0x%08X",
        localPlayer + 1, remotePlayer + 1, activeDelay, rollbackBudget, s_baselineCRC);

    return true;
}

// ============================================================================
// Safe Teardown
// ============================================================================

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
        "Final stats: frames=%d rollbacks=%d maxdepth=%d",
        snap.current_frame,
        snap.rollback_count, snap.max_rollback_distance);
    NetplayLog_Write("TEARDOWN", frame,
        "IO: sent=%d recv=%d",
        snap.local_inputs_sent, snap.remote_inputs_received);

    // Check for desync before ending
    if (RollbackDebug_IsDesyncDetected()) {
        int32_t desyncFrame = RollbackDebug_GetDesyncFrame();
        NetplayLog_Write("TEARDOWN", frame,
            "WARNING: Desync was detected at frame %d", desyncFrame);
    }

    // End session through GameplayBridge
    Net::GameplayBridge_EndSession();
    RollbackDebug_SetDigestEnabled(false);

    s_rollbackActive = false;
    s_gameplayActive = false;

    NetplayLog_Write("TEARDOWN", frame,
        "=== ROLLBACK SESSION ENDED ===");
    LOG_INFO("[OnlineWiring] Rollback session ended: %s", reason ? reason : "unknown");
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
    NetplayLog_ValueChange("DELAY", frame, "active_delay", from, to, reason);
}

static void LogBudgetChange(int from, int to, const char* reason) {
    int32_t frame = s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1;
    NetplayLog_ValueChange("DELAY", frame, "rollback_budget", from, to, reason);
}

// ============================================================================
// Per-Frame Update
// ============================================================================

static void CheckPolicyChanges() {
    int curDelay = Net::DelayPolicy_GetActiveDelay();
    if (s_lastActiveDelay >= 0 && curDelay != s_lastActiveDelay) {
        LogDelayChange(s_lastActiveDelay, curDelay, "delay policy update");
    }
    s_lastActiveDelay = curDelay;

    int curBudget = Net::DelayPolicy_GetAgreedRollbackBudget();
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
        // The normal rollback start now happens at GameplayStart handoff; this
        // path remains as a fallback if bootstrap wiring failed to start it.
        if (curPhase == Net::MatchLifecyclePhase::PlayableGameplay) {
            if (!s_rollbackActive && s_rollbackStarted) {
                // Resuming from pause/transition — session still alive
                s_gameplayActive = true;
                NetplayLog_Write("LIFE", -1, "Gameplay resumed (rollback session already started)");
            } else if (!s_rollbackStarted) {
                // Fallback: start rollback on first gameplay if handoff missed it.
                s_rollbackBeginPending = true;
            }
        }

        // Leaving PlayableGameplay — handle based on where we're going
        if (s_lastLifecyclePhase == Net::MatchLifecyclePhase::PlayableGameplay) {
            if (curPhase == Net::MatchLifecyclePhase::PauseActive) {
                // Pause — keep session alive but stop advancing
                s_gameplayActive = false;
                Net::PauseHandler_OnPauseEnter();
                NetplayLog_Write("LIFE", RollbackSession_GetCurrentFrame(),
                    "Gameplay paused — rollback session suspended");
            } else if (curPhase == Net::MatchLifecyclePhase::RoundTransition) {
                // Round end transition — keep session for next round
                s_gameplayActive = false;
                NetplayLog_Write("LIFE", RollbackSession_GetCurrentFrame(),
                    "Round transition — rollback session suspended");
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

        // Disconnect from any state
        if (curPhase == Net::MatchLifecyclePhase::DisconnectRecovery && s_rollbackActive) {
            StopRollbackSession("disconnect");
        }

        // Entering WinScreenActive — begin win screen sync
        if (curPhase == Net::MatchLifecyclePhase::WinScreenActive &&
            s_lastLifecyclePhase != Net::MatchLifecyclePhase::WinScreenActive) {
            Net::WinScreenSync_Begin();
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
            s_handoffFrame = -1;
            s_remoteInputsReceived = 0;
            s_packetsDispatched = 0;

            // Reset set tracker when session fully ends
            Net::SetTracker_Reset();
        }

        // MatchInit — potential round restart, re-enable gameplay flag
        if (curPhase == Net::MatchLifecyclePhase::MatchInit && s_rollbackStarted) {
            // New round starting — rollback session stays alive
            NetplayLog_Write("LIFE", -1, "New round init — rollback session persists");
        }

        // IntroActive → will reach PlayableGameplay soon
        if (curPhase == Net::MatchLifecyclePhase::IntroActive) {
            NetplayLog_Write("LIFE", -1, "Intro active — waiting for playable gameplay");
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
    s_rollbackBeginPending = false;
    s_handoffFrame = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    s_lastLifecyclePhase = Net::MatchLifecyclePhase::Inactive;
    s_lastActiveDelay = -1;
    s_lastRollbackBudget = -1;

    StressHooks_Init();
    Net::SetTracker_Init();
    Net::WinScreenSync_Init();
    Net::PauseHandler_Init();

    LOG_INFO("[OnlineWiring] Initialized");
    NetplayLog_Write("WIRING", -1, "OnlineWiring initialized");
}

void OnlineWiring_Shutdown() {
    if (s_rollbackActive) {
        StopRollbackSession("mod shutdown");
    }
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

    // Deferred rollback start (after phase tracking)
    if (s_rollbackBeginPending) {
        s_rollbackBeginPending = false;
        TryStartRollbackSession();
    }

    // Drive win screen sync when in win screen phase
    Net::MatchLifecyclePhase curPhase = Net::MatchLifecycle_GetPhase();
    if (curPhase == Net::MatchLifecyclePhase::WinScreenActive) {
        Net::WinScreenSync_FrameUpdate();
    }

    // Drive pause handler when session is owned
    if (Net::MatchLifecycle_IsMatchOwned()) {
        Net::PauseHandler_FrameUpdate();
    }

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
                "RTT=%.1fms jitter=%.1fms loss=%u/%u delay=%d budget=%d",
                stats.rtt_ms, stats.rtt_variance_ms,
                stats.packets_lost, stats.packets_sent,
                dpSnap.active_delay, dpSnap.rollback_budget);

            NetplayLog_Write("STATS", frame,
                "Rollbacks=%d maxdepth=%d frames_ahead=%.1f",
                rbSnap.rollback_count,
                rbSnap.max_rollback_distance,
                rbSnap.frames_ahead);
        }
    }
}

void OnlineWiring_OnGameplayStart() {
    if (!s_rollbackStarted) {
        TryStartRollbackSession();
    }
}

void OnlineWiring_OnGameplayPause(const char* reason) {
    s_gameplayActive = false;
    NetplayLog_Write("LIFE", s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1,
        "Gameplay paused: %s", reason ? reason : "unknown");
}

void OnlineWiring_OnMatchEnd() {
    StopRollbackSession("match end event");
}

void OnlineWiring_OnDisconnect(const char* reason) {
    NetplayLog_Write("DISCONNECT", s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1,
        "=== DISCONNECT: %s ===", reason ? reason : "unknown");

    if (s_rollbackActive) {
        StopRollbackSession(reason ? reason : "disconnect");
    }

    // Reset set tracker on session end
    Net::SetTracker_Reset();

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
        "Pre-cleanup wiring state: started=%d active=%d gameplay=%d handoff=%d baseline=0x%08X config=0x%08X recv=%d dispatched=%d",
        s_rollbackStarted ? 1 : 0,
        s_rollbackActive ? 1 : 0,
        s_gameplayActive ? 1 : 0,
        s_handoffFrame,
        s_baselineCRC,
        s_configHash,
        s_remoteInputsReceived,
        s_packetsDispatched);

    RematchCleanup_PrepareForNextMatch("post-match rematch");

    // Full reset for next match
    s_rollbackStarted = false;
    s_rollbackActive = false;
    s_gameplayActive = false;
    s_rollbackBeginPending = false;
    s_handoffFrame = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    s_lastActiveDelay = -1;
    s_lastRollbackBudget = -1;

    NetplayLog_Write("POSTMATCH", -1,
        "Post-cleanup wiring state: started=%d active=%d gameplay=%d handoff=%d baseline=0x%08X config=0x%08X recv=%d dispatched=%d",
        s_rollbackStarted ? 1 : 0,
        s_rollbackActive ? 1 : 0,
        s_gameplayActive ? 1 : 0,
        s_handoffFrame,
        s_baselineCRC,
        s_configHash,
        s_remoteInputsReceived,
        s_packetsDispatched);
}

void OnlineWiring_OnReturnToSession() {
    NetplayLog_Write("POSTMATCH", -1,
        "=== RETURN TO SESSION — exiting match flow ===");

    RematchCleanup_PrepareForNextMatch("post-match return to session");

    s_rollbackStarted = false;
    s_rollbackActive = false;
    s_gameplayActive = false;
    s_rollbackBeginPending = false;
    s_handoffFrame = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
}

bool OnlineWiring_IsGameplayActive() {
    return s_rollbackActive && s_gameplayActive;
}

void OnlineWiring_GetSnapshot(OnlineWiringSnapshot* out) {
    if (!out) return;
    out->rollback_started = s_rollbackStarted;
    out->rollback_active = s_rollbackActive;
    out->gameplay_active = s_gameplayActive;
    out->handoff_frame = s_handoffFrame;
    out->baseline_crc = s_baselineCRC;
    out->config_hash = s_configHash;
    out->handoff_delay = s_handoffDelay;
    out->handoff_budget = s_handoffBudget;
    out->remote_inputs_received = s_remoteInputsReceived;
    out->packets_dispatched = s_packetsDispatched;
}

} // namespace Rollback
