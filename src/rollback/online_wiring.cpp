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
#include "rollback/netplay_log.h"
#include "rollback/stress_hooks.h"
#include "rollback/input_timeline.h"
#include "rollback/prediction.h"
#include "rollback/resimulation.h"
#include "rollback/determinism_verify.h"
#include "net/match_lifecycle.h"
#include "net/pregame_sync.h"
#include "net/match_bootstrap.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/sync_policy.h"
#include "net/delay_policy.h"
#include "net/locked_match_config.h"
#include "net/enet_transport.h"
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
        case Net::PacketType::GameplayInput: {
            // Deserialize the GameplayInputPayload
            // (defined identically in rollback_session.cpp — binary-compatible)
            #pragma pack(push, 1)
            struct GameplayInputPayload {
                int32_t  frame;
                uint16_t input;
                int32_t  start_frame;
                uint8_t  input_count;
                uint16_t inputs[8];
            };
            #pragma pack(pop)

            if (payloadLen < sizeof(int32_t) + sizeof(uint16_t)) {
                NetplayLog_Write("INPUT", -1,
                    "WARN: GameplayInput packet too small (%zu bytes)", payloadLen);
                break;
            }

            auto* p = static_cast<const GameplayInputPayload*>(payload);

            // Validate frame range
            int32_t currentFrame = RollbackSession_GetCurrentFrame();
            if (p->frame < 0 || p->frame > currentFrame + 300) {
                NetplayLog_Write("INPUT", currentFrame,
                    "WARN: Remote input frame %d out of range (current=%d)",
                    p->frame, currentFrame);
                break;
            }

            s_packetsDispatched++;

            // Submit primary input
            RollbackSession_SubmitRemoteInput(p->frame, p->input);
            s_remoteInputsReceived++;

            // Submit redundant batch (for packet loss recovery)
            if (p->input_count > 1 && payloadLen >= sizeof(GameplayInputPayload)) {
                // Batch starts at start_frame, inputs[0] = newest (frame), etc.
                for (int i = 1; i < p->input_count && i < 8; i++) {
                    int32_t batchFrame = p->frame - i;
                    if (batchFrame >= 0) {
                        RollbackSession_SubmitRemoteInput(batchFrame, p->inputs[i]);
                    }
                }
            }

            // Verbose logging: per-input trace
            NetplayLog_Verbose("INPUT", p->frame,
                "Remote: frame=%d input=0x%04X batch=%d",
                p->frame, p->input, p->input_count);

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

        default:
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

    // Determine local/remote player from side assignment
    Net::PregameSnapshot pregameSnap{};
    Net::PregameSync_GetSnapshot(&pregameSnap);

    int localPlayer, remotePlayer;
    Net::SessionRole role = Net::Session_GetRole();

    if (role == Net::SessionRole::Host) {
        // Host is P1 if host_side == 0
        localPlayer  = (config->host_side == 0) ? 0 : 1;
        remotePlayer = (config->host_side == 0) ? 1 : 0;
    } else {
        // Join is the opposite side
        localPlayer  = (config->host_side == 0) ? 1 : 0;
        remotePlayer = (config->host_side == 0) ? 0 : 1;
    }

    // Build rollback session config
    RollbackSessionConfig rbConfig{};
    rbConfig.local_player = localPlayer;
    rbConfig.remote_player = remotePlayer;
    rbConfig.initial_delay = activeDelay;
    rbConfig.rollback_budget = rollbackBudget;
    rbConfig.baseline_checksum = bootSnap.local_baseline_crc;
    rbConfig.start_frame = (int32_t)bootSnap.start_frame;

    // Config hash for logging
    s_configHash = Net::LockedMatchConfig_Hash(config);
    s_baselineCRC = bootSnap.local_baseline_crc;
    s_handoffDelay = activeDelay;
    s_handoffBudget = rollbackBudget;
    s_handoffFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

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
        "Baseline CRC=0x%08X start_frame=%d",
        s_baselineCRC, rbConfig.start_frame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Active delay=%d rollback_budget=%d",
        activeDelay, rollbackBudget);

    Net::MatchLifecyclePhase phase = Net::MatchLifecycle_GetPhase();
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Lifecycle phase at handoff: %s",
        Net::MatchLifecyclePhaseName(phase));

    // Register gameplay packet callback
    Net::Session_SetPacketCallback(OnGameplayPacket);

    // Start rollback session
    bool ok = RollbackSession_Begin(rbConfig);
    if (!ok) {
        NetplayLog_Write("HANDOFF", s_handoffFrame,
            "ERROR: RollbackSession_Begin FAILED");
        LOG_ERROR("[OnlineWiring] RollbackSession_Begin failed");
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
        "Final stats: frames=%d confirmed=%d rollbacks=%d maxdepth=%d",
        snap.current_frame, snap.last_confirmed_frame,
        snap.rollback_count, snap.max_rollback_distance);
    NetplayLog_Write("TEARDOWN", frame,
        "Predictions: total=%d mispredict=%d correct=%d",
        snap.total_predictions, snap.total_mispredictions,
        snap.total_correct_predictions);
    NetplayLog_Write("TEARDOWN", frame,
        "IO: sent=%d recv=%d",
        snap.local_inputs_sent, snap.remote_inputs_received);

    // Check for desync before ending
    if (RollbackDebug_IsDesyncDetected()) {
        int32_t desyncFrame = RollbackDebug_GetDesyncFrame();
        NetplayLog_Write("TEARDOWN", frame,
            "WARNING: Desync was detected at frame %d", desyncFrame);
    }

    RollbackSession_End();
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

        // Entering PlayableGameplay — start rollback if not active
        if (curPhase == Net::MatchLifecyclePhase::PlayableGameplay) {
            if (!s_rollbackActive && s_rollbackStarted) {
                // Resuming from pause/transition — session still alive
                s_gameplayActive = true;
                NetplayLog_Write("LIFE", -1, "Gameplay resumed (rollback session already started)");
            } else if (!s_rollbackStarted) {
                // First time reaching gameplay — start rollback session
                s_rollbackBeginPending = true;
            }
        }

        // Leaving PlayableGameplay — handle based on where we're going
        if (s_lastLifecyclePhase == Net::MatchLifecyclePhase::PlayableGameplay) {
            if (curPhase == Net::MatchLifecyclePhase::PauseActive) {
                // Pause — keep session alive but stop advancing
                s_gameplayActive = false;
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
        if (curPhase == Net::MatchLifecyclePhase::MatchEnd && s_rollbackActive) {
            StopRollbackSession("match ended (non-gameplay)");
        }

        // Disconnect from any state
        if (curPhase == Net::MatchLifecyclePhase::DisconnectRecovery && s_rollbackActive) {
            StopRollbackSession("disconnect");
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

    LOG_INFO("[OnlineWiring] Initialized");
    NetplayLog_Write("WIRING", -1, "OnlineWiring initialized");
}

void OnlineWiring_Shutdown() {
    if (s_rollbackActive) {
        StopRollbackSession("mod shutdown");
    }
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

    // Drive rollback subsystems only when gameplay is active
    if (s_rollbackActive && s_gameplayActive) {
        // The actual RollbackSession_FrameUpdate is called from mod_main.cpp
        // We just do auxiliary work here.

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
                "Confirmed=%d predicted=%d rollbacks=%d maxdepth=%d",
                rbSnap.last_confirmed_frame,
                rbSnap.predicted_frames_outstanding,
                rbSnap.rollback_count,
                rbSnap.max_rollback_distance);
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

    // Full reset for next match
    s_rollbackStarted = false;
    s_rollbackBeginPending = false;
    s_handoffFrame = -1;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    s_lastActiveDelay = -1;
    s_lastRollbackBudget = -1;
}

void OnlineWiring_OnReturnToSession() {
    NetplayLog_Write("POSTMATCH", -1,
        "=== RETURN TO SESSION — exiting match flow ===");

    s_rollbackStarted = false;
    s_rollbackBeginPending = false;
    s_handoffFrame = -1;
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
