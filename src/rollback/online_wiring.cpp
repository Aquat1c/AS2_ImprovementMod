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
#include "patches/input_sync_hooks.h"
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
static bool     s_liveReleaseArmed       = false;   // Waiting for post-intro interactive release
static int32_t  s_handoffFrame           = -1;
static uint32_t s_baselineCRC            = 0;
static uint32_t s_configHash             = 0;
static int      s_handoffDelay           = 0;
static int      s_handoffBudget          = 0;
static int      s_remoteInputsReceived   = 0;
static int      s_packetsDispatched      = 0;

// Startup gameplay-entry barrier state:
// - READY means this peer reached the FIRST interactive post-intro boundary and is held.
// - ACK means this peer has observed remote READY.
// Release requires both READY and ACK observed from both sides.
static bool     s_startupBarrierEntered    = false;
static bool     s_localGekkoReadySent      = false;  // local READY sent
static bool     s_localGekkoReadyAckSent   = false;  // local ACK sent
static bool     s_remoteGekkoReady         = false;  // remote READY observed
static bool     s_remoteGekkoReadyAck      = false;  // remote ACK observed
static bool     s_startupReleased          = false;  // gameplay-entry barrier released
static int32_t  s_localReadyFrame          = -1;
static int32_t  s_localAckFrame            = -1;
static int32_t  s_remoteReadyFrame         = -1;
static int32_t  s_remoteAckFrame           = -1;
static uint32_t s_startupBlockedLogCounter = 0;
static uint32_t s_introHoldLogCounter      = 0;
static DWORD    s_lastStartupReadySendAt   = 0;
static uint8_t  s_lastStartupReadyFlags    = 0;

// Phase tracking for before/after logging
static Net::MatchLifecyclePhase s_lastLifecyclePhase = Net::MatchLifecyclePhase::Inactive;
static int      s_lastActiveDelay        = -1;
static int      s_lastRollbackBudget     = -1;

// Rollback start guard — prevent double-starting
static bool     s_rollbackBeginPending   = false;

static int32_t GetStartupLogFrame() {
    if (s_rollbackActive) {
        return RollbackSession_GetCurrentFrame();
    }
    return (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

static void ResetStartupBarrierState(const char* reason) {
    const bool hadState =
        s_startupBarrierEntered ||
        s_localGekkoReadySent ||
        s_localGekkoReadyAckSent ||
        s_remoteGekkoReady ||
        s_remoteGekkoReadyAck ||
        s_startupReleased;

    if (hadState) {
        NetplayLog_Write("STARTUP", GetStartupLogFrame(),
            "Startup barrier reset: reason=%s entered=%d local_ready=%d local_ack=%d "
            "remote_ready=%d remote_ack=%d released=%d "
            "local_ready_frame=%d local_ack_frame=%d remote_ready_frame=%d remote_ack_frame=%d",
            reason ? reason : "unspecified",
            s_startupBarrierEntered ? 1 : 0,
            s_localGekkoReadySent ? 1 : 0,
            s_localGekkoReadyAckSent ? 1 : 0,
            s_remoteGekkoReady ? 1 : 0,
            s_remoteGekkoReadyAck ? 1 : 0,
            s_startupReleased ? 1 : 0,
            s_localReadyFrame,
            s_localAckFrame,
            s_remoteReadyFrame,
            s_remoteAckFrame);
    }

    s_startupBarrierEntered = false;
    s_localGekkoReadySent = false;
    s_localGekkoReadyAckSent = false;
    s_remoteGekkoReady = false;
    s_remoteGekkoReadyAck = false;
    s_startupReleased = false;
    s_localReadyFrame = -1;
    s_localAckFrame = -1;
    s_remoteReadyFrame = -1;
    s_remoteAckFrame = -1;
    s_startupBlockedLogCounter = 0;
    s_introHoldLogCounter = 0;
    s_lastStartupReadySendAt = 0;
    s_lastStartupReadyFlags = 0;
}

static void SendGekkoReadyPacket(uint8_t flags, const char* reason) {
    Net::GekkoReadyPayload payload{};
    payload.flags = flags;
    payload.phase = (uint8_t)Net::MatchLifecycle_GetPhase();
    payload.rollback_frame = GetStartupLogFrame();
    if ((flags & Net::GEKKO_READY_FLAG_READY) != 0 && s_localReadyFrame < 0) {
        s_localReadyFrame = payload.rollback_frame;
    }
    if ((flags & Net::GEKKO_READY_FLAG_ACK) != 0 && s_localAckFrame < 0) {
        s_localAckFrame = payload.rollback_frame;
    }

    Net::Session_SendPacket(
        Net::CHANNEL_CONTROL,
        Net::PacketType::GekkoReady,
        &payload,
        sizeof(payload),
        true);
    s_lastStartupReadySendAt = GetTickCount();
    s_lastStartupReadyFlags = flags;

    NetplayLog_Write("STARTUP", GetStartupLogFrame(),
        "Local GekkoReady sent: flags=0x%02X (%s%s) reason=%s phase=%s",
        flags,
        (flags & Net::GEKKO_READY_FLAG_READY) ? "READY" : "",
        (flags & Net::GEKKO_READY_FLAG_ACK) ? "|ACK" : "",
        reason ? reason : "unspecified",
        Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
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
// Packet Dispatch for Gameplay Phase
// ============================================================================

/// Packet callback that handles both pregame AND gameplay packets.
/// During gameplay, GameplayInput/StateDigest are dispatched here.
/// All other packets are forwarded to the pregame handler.
static void OnGameplayPacket(Net::PacketType type, const void* payload, size_t payloadLen) {
    switch (type) {
        case Net::PacketType::GekkoData: {
            // GekkoNet internal protocol data — buffer for GekkoNet to drain
            if (payloadLen == 0 || !payload) {
                LogGameplayPacketAnomaly("Empty GekkoData", type, payloadLen, 1);
                break;
            }

            if (!s_rollbackActive) {
                static uint32_t s_preLiveGekkoDrops = 0;
                s_preLiveGekkoDrops++;
                if (s_preLiveGekkoDrops <= 5 || (s_preLiveGekkoDrops % 120) == 0) {
                    NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                        "Dropping pre-live GekkoData while rollback session is not active: "
                        "len=%zu phase=%s",
                        payloadLen,
                        Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
                }
                break;
            }

            RollbackSession_BufferGekkoPacket(payload, payloadLen);
            s_packetsDispatched++;
            s_remoteInputsReceived++;

            NetplayLog_Verbose("GEKKO", RollbackSession_GetCurrentFrame(),
                "Buffered GekkoData packet (%zu bytes)", payloadLen);
            break;
        }

        case Net::PacketType::StateDigest: {
            if (payloadLen < sizeof(Net::StateDigestPayload)) {
                LogGameplayPacketAnomaly("Short StateDigest", type, payloadLen, sizeof(Net::StateDigestPayload));
                break;
            }
            auto* p = static_cast<const Net::StateDigestPayload*>(payload);
            RollbackDebug_OnRemoteDigest((int32_t)p->frame_number, p->crc32);

            NetplayLog_Verbose("DESYNC", (int32_t)p->frame_number,
                "Remote digest: crc=0x%08X", p->crc32);
            break;
        }

        case Net::PacketType::FrameSyncStatus: {
            if (payloadLen < sizeof(Net::FrameSyncStatusPayload)) {
                LogGameplayPacketAnomaly("Short FrameSyncStatus", type, payloadLen, sizeof(Net::FrameSyncStatusPayload));
                break;
            }
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

        case Net::PacketType::GekkoReady: {
            uint8_t flags = Net::GEKKO_READY_FLAG_READY;  // Legacy fallback: empty payload => READY
            uint8_t phase = 0xFF;
            int32_t remoteFrame = -1;

            if (payloadLen >= sizeof(Net::GekkoReadyPayload) && payload) {
                const auto* p = static_cast<const Net::GekkoReadyPayload*>(payload);
                flags = p->flags;
                phase = p->phase;
                remoteFrame = p->rollback_frame;
            } else if (payloadLen >= 1 && payload) {
                flags = *static_cast<const uint8_t*>(payload);
            } else if (payloadLen != 0) {
                LogGameplayPacketAnomaly("Short GekkoReady", type, payloadLen, sizeof(uint8_t));
                break;
            }

            if ((flags & Net::GEKKO_READY_FLAG_READY) == 0 &&
                (flags & Net::GEKKO_READY_FLAG_ACK) == 0) {
                LogGameplayPacketAnomaly("Empty GekkoReady flags", type, payloadLen, sizeof(uint8_t));
                break;
            }
            if ((flags & Net::GEKKO_READY_FLAG_ACK) != 0) {
                flags |= Net::GEKKO_READY_FLAG_READY;
            }

            const bool legacyPhase = (phase == 0xFF);
            const bool remoteAtInteractiveBoundary =
                legacyPhase ||
                (phase == (uint8_t)Net::MatchLifecyclePhase::PlayableGameplay);
            if (!remoteAtInteractiveBoundary && !s_startupReleased) {
                NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                    "Ignoring remote GekkoReady before interactive boundary: flags=0x%02X "
                    "phase=%u remote_frame=%d local_phase=%s",
                    flags,
                    (unsigned)phase,
                    remoteFrame,
                    Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
                break;
            }

            if ((flags & Net::GEKKO_READY_FLAG_READY) != 0) {
                if (!s_remoteGekkoReady) {
                    s_remoteGekkoReady = true;
                    s_remoteReadyFrame = remoteFrame;
                    NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                        "Remote startup READY observed: flags=0x%02X phase=%u remote_frame=%d",
                        flags, (unsigned)phase, remoteFrame);
                } else {
                    NetplayLog_Verbose("STARTUP", GetStartupLogFrame(),
                        "Duplicate remote READY ignored: flags=0x%02X phase=%u remote_frame=%d",
                        flags, (unsigned)phase, remoteFrame);
                }
            }

            if ((flags & Net::GEKKO_READY_FLAG_ACK) != 0) {
                if (!s_remoteGekkoReadyAck) {
                    s_remoteGekkoReadyAck = true;
                    s_remoteAckFrame = remoteFrame;
                    NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                        "Remote startup ACK observed: flags=0x%02X phase=%u remote_frame=%d",
                        flags, (unsigned)phase, remoteFrame);
                } else {
                    NetplayLog_Verbose("STARTUP", GetStartupLogFrame(),
                        "Duplicate remote ACK ignored: flags=0x%02X phase=%u remote_frame=%d",
                        flags, (unsigned)phase, remoteFrame);
                }
            }
            break;
        }

        case Net::PacketType::WinScreenFrameInput: {
            if (payloadLen < sizeof(Net::WinScreenFrameInputPayload)) {
                LogGameplayPacketAnomaly("Short WinScreenFrameInput", type, payloadLen,
                                         sizeof(Net::WinScreenFrameInputPayload));
                break;
            }
            Net::WinScreenSync_OnRemoteFrameInput(
                static_cast<const Net::WinScreenFrameInputPayload*>(payload));
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
                "Unhandled packet during gameplay: type=%s(%u) payload=%zu",
                Net::PacketTypeName(type),
                (unsigned)type,
                payloadLen);
            break;
    }
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

    Net::MatchBootstrapSnapshot bootSnap{};
    Net::MatchBootstrap_GetSnapshot(&bootSnap);

    s_configHash = Net::LockedMatchConfig_Hash(config);
    s_baselineCRC = bootSnap.local_baseline_crc;
    s_handoffDelay = Net::DelayPolicy_GetActiveDelay();
    s_handoffBudget = Net::DelayPolicy_GetAgreedRollbackBudget();
    s_handoffFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "=== BOOTSTRAP -> INTRO HANDOFF ===");
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Config hash=0x%08X baseline_crc=0x%08X start_frame=%u host_start_sim=%d",
        s_configHash,
        s_baselineCRC,
        bootSnap.start_frame,
        bootSnap.gameplay_start_sim_frame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Load barrier sim: local=%d remote=%d | baseline sim: local=%d remote=%d",
        bootSnap.local_load_sim_frame,
        bootSnap.remote_load_sim_frame,
        bootSnap.local_baseline_sim_frame,
        bootSnap.remote_baseline_sim_frame);

    // Preserve hard startup alignment before deterministic intro runs.
    uint32_t preRestoreCRC = CalcCRC32(
        (const void*)ADDR_MATCH_BASE,
        (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Pre-restore CRC=0x%08X (baseline=0x%08X match=%s)",
        preRestoreCRC,
        s_baselineCRC,
        (preRestoreCRC == s_baselineCRC) ? "YES" : "NO");

    if (Savestate_Load()) {
        WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, 0);
        WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, 0);
        s_handoffFrame = 0;

        uint32_t postRestoreCRC = CalcCRC32(
            (const void*)ADDR_MATCH_BASE,
            (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
        NetplayLog_Write("HANDOFF", 0,
            "Baseline RESTORED before intro: crc=0x%08X match=%s sim=0 writeIdx=0",
            postRestoreCRC,
            (postRestoreCRC == s_baselineCRC) ? "YES" : "NO");
    } else {
        NetplayLog_Write("HANDOFF", s_handoffFrame,
            "WARNING: Baseline restore FAILED before intro handoff");
        LOG_WARN("[OnlineWiring] Baseline restore failed before intro handoff");
    }

    // Switch to gameplay-phase packet callback now so startup READY/ACK can be
    // exchanged during intro/passive startup before rollback session begin.
    Net::Session_SetPacketCallback(OnGameplayPacket);
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
    rbConfig.baseline_checksum = s_baselineCRC ? s_baselineCRC : bootSnap.local_baseline_crc;
    const int32_t bootstrapFrame = (int32_t)bootSnap.start_frame;
    const int32_t interactiveFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    rbConfig.start_frame = interactiveFrame;

    // Config hash for logging
    s_configHash = Net::LockedMatchConfig_Hash(config);
    s_baselineCRC = rbConfig.baseline_checksum;
    s_handoffDelay = activeDelay;
    s_handoffBudget = rollbackBudget;
    s_handoffFrame = interactiveFrame;

    // --- LOG BEFORE/AFTER for live release ---
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "=== INTERACTIVE RELEASE -> ROLLBACK START ===");
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
        "Baseline CRC=0x%08X bootstrap_frame=%d interactive_release_frame=%d rollback_start_frame=%d",
        s_baselineCRC, bootstrapFrame, interactiveFrame, rbConfig.start_frame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Load barrier sim: local=%d remote=%d | baseline sim: local=%d remote=%d | start sim=%d",
        bootSnap.local_load_sim_frame,
        bootSnap.remote_load_sim_frame,
        bootSnap.local_baseline_sim_frame,
        bootSnap.remote_baseline_sim_frame,
        bootSnap.gameplay_start_sim_frame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Rollback start delta from bootstrap baseline: %d frames",
        rbConfig.start_frame - bootstrapFrame);
    NetplayLog_Write("HANDOFF", s_handoffFrame,
        "Active delay=%d rollback_budget=%d",
        activeDelay, rollbackBudget);

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
    s_gameplayActive = Net::MatchLifecycle_IsGameplayPlayable();
    s_liveReleaseArmed = false;

    // Enable state digest for desync detection
    RollbackDebug_SetDigestEnabled(true);

    if (!s_gameplayActive) {
        NetplayLog_Write("HANDOFF", s_handoffFrame,
            "Rollback session owns pre-playable match frames; lifecycle=%s",
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
    }
    if (!s_startupReleased) {
        NetplayLog_Write("STARTUP", GetStartupLogFrame(),
            "WARNING: rollback session started before startup release completed");
    }

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
    InputSyncHooks_SetTimesyncFreeze(false);

    s_rollbackActive = false;
    s_gameplayActive = false;
    s_liveReleaseArmed = false;
    ResetStartupBarrierState(reason ? reason : "rollback stop");

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
                            ? "Gameplay resumed (rollback session already started)"
                            : "Interactive gameplay began under rollback ownership");
                }
            } else {
                if (!s_liveReleaseArmed) {
                    // Defensive fallback: if handoff wiring was missed, still arm the
                    // interactive startup barrier here so first-advance cannot race.
                    s_liveReleaseArmed = true;
                    ResetStartupBarrierState("lifecycle playable fallback");
                    Net::Session_SetPacketCallback(OnGameplayPacket);
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
            s_liveReleaseArmed = false;
            s_handoffFrame = -1;
            s_remoteInputsReceived = 0;
            s_packetsDispatched = 0;
            ResetStartupBarrierState("lifecycle inactive");

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
            NetplayLog_Write("LIFE", -1,
                "Intro active — waiting for first interactive boundary (release_armed=%d)",
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
    ResetStartupBarrierState("init");

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
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
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

    // Startup gameplay-entry barrier:
    // - Deterministic intro runs in vanilla/passive mode.
    // - Rollback-owned BeginFrame/Advance is held until BOTH peers report the
    //   first post-intro interactive boundary and exchange READY+ACK.
    if ((s_liveReleaseArmed || s_rollbackActive) && !s_startupReleased) {
        const bool sessionConnected = Net::Session_IsConnected();
        const bool inPlayableGameplay = IsInPlayableMatchGameplay();

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

        if (sessionConnected && inPlayableGameplay) {
            if (!s_localGekkoReadySent) {
                uint8_t flags = Net::GEKKO_READY_FLAG_READY;
                if (s_remoteGekkoReady) {
                    flags |= Net::GEKKO_READY_FLAG_ACK;
                }
                SendGekkoReadyPacket(flags, "local reached first interactive boundary");
                s_localGekkoReadySent = true;
                if ((flags & Net::GEKKO_READY_FLAG_ACK) != 0) {
                    s_localGekkoReadyAckSent = true;
                }
            } else if (s_remoteGekkoReady && !s_localGekkoReadyAckSent) {
                SendGekkoReadyPacket(
                    Net::GEKKO_READY_FLAG_READY | Net::GEKKO_READY_FLAG_ACK,
                    "remote interactive READY observed");
                s_localGekkoReadyAckSent = true;
            } else if (s_localGekkoReadySent) {
                uint8_t resendFlags = Net::GEKKO_READY_FLAG_READY;
                if (s_localGekkoReadyAckSent) {
                    resendFlags |= Net::GEKKO_READY_FLAG_ACK;
                }

                constexpr DWORD STARTUP_READY_RESEND_MS = 500;
                const DWORD now = GetTickCount();
                if ((now - s_lastStartupReadySendAt) >= STARTUP_READY_RESEND_MS ||
                    resendFlags != s_lastStartupReadyFlags) {
                    SendGekkoReadyPacket(resendFlags, "startup barrier resend");
                }
            }
        }

        const bool releaseSatisfied =
            s_localGekkoReadySent &&
            s_remoteGekkoReady &&
            s_localGekkoReadyAckSent &&
            s_remoteGekkoReadyAck;

        if (releaseSatisfied) {
            s_startupReleased = true;
            NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                "Gameplay-entry release satisfied at interactive boundary: "
                "local_ready=%d local_ack=%d remote_ready=%d remote_ack=%d "
                "local_frame=%d local_ready_frame=%d local_ack_frame=%d "
                "remote_ready_frame=%d remote_ack_frame=%d phase=%s",
                s_localGekkoReadySent ? 1 : 0,
                s_localGekkoReadyAckSent ? 1 : 0,
                s_remoteGekkoReady ? 1 : 0,
                s_remoteGekkoReadyAck ? 1 : 0,
                GetStartupLogFrame(),
                s_localReadyFrame,
                s_localAckFrame,
                s_remoteReadyFrame,
                s_remoteAckFrame,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));

            if (!s_rollbackStarted) {
                s_rollbackBeginPending = true;
            }
        } else if (s_localGekkoReadySent) {
            s_startupBlockedLogCounter++;
            if (s_startupBlockedLogCounter <= 5 ||
                (s_startupBlockedLogCounter % 120) == 0) {
                NetplayLog_Write("STARTUP", GetStartupLogFrame(),
                    "Startup release BLOCKED at interactive boundary: "
                    "waiting_remote_ready=%d waiting_remote_ack=%d "
                    "session_connected=%d local_frame=%d remote_ready_frame=%d remote_ack_frame=%d "
                    "phase=%s",
                    s_remoteGekkoReady ? 0 : 1,
                    s_remoteGekkoReadyAck ? 0 : 1,
                    sessionConnected ? 1 : 0,
                    GetStartupLogFrame(),
                    s_remoteReadyFrame,
                    s_remoteAckFrame,
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
    if (s_rollbackStarted || s_liveReleaseArmed) {
        return;
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
        ResetStartupBarrierState("match end");
    }
}

void OnlineWiring_OnDisconnect(const char* reason) {
    NetplayLog_Write("DISCONNECT", s_rollbackActive ? RollbackSession_GetCurrentFrame() : -1,
        "=== DISCONNECT: %s ===", reason ? reason : "unknown");

    if (s_rollbackActive) {
        StopRollbackSession(reason ? reason : "disconnect");
    }

    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;

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
    s_liveReleaseArmed = false;
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
    ResetStartupBarrierState("rematch");

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
    s_liveReleaseArmed = false;
    s_rollbackBeginPending = false;
    s_handoffFrame = -1;
    s_baselineCRC = 0;
    s_configHash = 0;
    s_handoffDelay = 0;
    s_handoffBudget = 0;
    s_remoteInputsReceived = 0;
    s_packetsDispatched = 0;
    ResetStartupBarrierState("return to session");
}

bool OnlineWiring_IsGameplayActive() {
    return s_rollbackActive && s_gameplayActive;
}

bool OnlineWiring_IsStartupReleased() {
    return s_startupReleased;
}

bool OnlineWiring_IsGameplayEntryAdvanceBlocked() {
    const bool atInteractiveBoundary = IsInPlayableMatchGameplay();
    if (!atInteractiveBoundary) {
        return false;
    }

    if (!s_startupReleased) {
        return s_liveReleaseArmed || s_rollbackActive;
    }

    if (!s_rollbackActive && (s_liveReleaseArmed || s_rollbackBeginPending || !s_rollbackStarted)) {
        return true;
    }

    if (s_rollbackActive && !RollbackSession_IsSessionRunning()) {
        return true;
    }

    return false;
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
