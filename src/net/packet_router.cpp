/**
 * Alice Senki 2 - packet_router (Implementation, re0.7 M3)
 *
 * Promotion of the M0 gameplay_packet_router: one regime-independent routing
 * table, registered once by session2 and never handed off (§2.3). The
 * per-type handlers are the same module entry points both legacy regimes
 * routed to; the pregame-machine-owned set goes through
 * PregameSync_OnSessionPacket (which also maintains the pregame lock
 * latches for CharSelLock/StageSync).
 */

#include "net/packet_router.h"

#include "net/charsel_sync.h"
#include "net/churn_pause.h"
#include "net/frontend_input_sync.h"
#include "net/match_lifecycle.h"
#include "net/netplay_palette_runtime.h"
#include "net/pause_handler.h"
#include "net/pregame_sync.h"
#include "net/sync_trace.h"
#include "net/transition_barrier.h"
#include "net/winscreen_sync.h"
#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "rollback/online_wiring.h"
#include "rollback/rollback_debug.h"
#include "rollback/rollback_session.h"

namespace Net {

namespace {

int32_t RouterLogFrame() {
    if (Rollback::RollbackSession_IsActive()) {
        return Rollback::RollbackSession_GetCurrentFrame();
    }
    return (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

void LogRouterPacketAnomaly(const char* reason,
                            PacketType type,
                            size_t payloadLen,
                            size_t expectedLen) {
    Rollback::NetplayLog_Write(
        "PACKET", RouterLogFrame(),
        "%s: type=%s payload=%zu expected=%zu rollback_active=%d",
        reason ? reason : "packet anomaly",
        PacketTypeName(type),
        payloadLen,
        expectedLen,
        Rollback::RollbackSession_IsActive() ? 1 : 0);
}

uint32_t s_unknownPacketCount = 0;

} // anonymous namespace

void PacketRouter_OnPacket(PacketType type, const void* payload, size_t payloadLen) {
    // Wire-acknowledged transition barriers must be reachable regardless of
    // regime — winscreen-exit proposals arrive during gameplay, cancel/restart
    // proposals during pregame.
    if (TransitionBarrier_OnPacket(type, payload, payloadLen)) {
        return;
    }
    switch (type) {
        // --- Pregame-machine-owned set (state lives in pregame_sync /
        // match_bootstrap; CharSelLock/StageSync also set pregame latches) ---
        case PacketType::SyncAnnounce:
        case PacketType::SyncConfirm:
        case PacketType::CharSelInput:
        case PacketType::CharSelLock:
        case PacketType::StageSync:
        case PacketType::ConfigExchange:
        case PacketType::ConfigAck:
        case PacketType::LoadBarrier:
        case PacketType::BaselineReady:
        case PacketType::BaselineDigest:
        case PacketType::BaselineBreakdown:
        case PacketType::GameplayStart:
            PregameSync_OnSessionPacket(type, payload, payloadLen);
            break;

        // --- Engine sinks (Gekko-era, until the engine2/match_setup cutover;
        // both handlers self-guard against pre-live arrival) ---
        case PacketType::InputStream: {
            Rollback::OnlineWiring_HandleEngineDataPacket(payload, payloadLen);
            break;
        }

        case PacketType::GekkoReady: {
            Rollback::OnlineWiring_HandleStartupBarrierPacket(payload, payloadLen);
            break;
        }

        // --- Diagnostics ---
        case PacketType::StateDigest: {
            if (payloadLen < sizeof(StateDigestPayload)) {
                LogRouterPacketAnomaly("Short StateDigest", type, payloadLen, sizeof(StateDigestPayload));
                break;
            }
            auto* p = static_cast<const StateDigestPayload*>(payload);
            Rollback::RollbackDebug_OnRemoteDigest((int32_t)p->frame_number, p->crc32);

            Rollback::NetplayLog_Verbose("DIGEST", (int32_t)p->frame_number,
                "Remote digest received: crc=0x%08X", p->crc32);
            break;
        }

        case PacketType::FrameSyncStatus: {
            if (payloadLen < sizeof(FrameSyncStatusPayload)) {
                LogRouterPacketAnomaly("Short FrameSyncStatus", type, payloadLen, sizeof(FrameSyncStatusPayload));
                break;
            }
            auto* p = static_cast<const FrameSyncStatusPayload*>(payload);
            Rollback::RollbackDebug_OnRemoteFrameSyncStatus(
                p->rb_frame_current,
                p->game_abs_frame_current,
                p->frame_origin_abs,
                p->rb_frame_last_remote_received,
                p->rb_frame_confirmed,
                p->predicted_frames,
                p->checksum);
            break;
        }

        case PacketType::SyncTrace: {
            if (payloadLen < sizeof(SyncTracePayload)) {
                LogRouterPacketAnomaly("Short SyncTrace", type, payloadLen, sizeof(SyncTracePayload));
                break;
            }
            SyncTrace_OnRemoteTrace(static_cast<const SyncTracePayload*>(payload));
            break;
        }

        case PacketType::ChurnPause: {
            if (payloadLen < sizeof(ChurnPausePayload)) {
                LogRouterPacketAnomaly("Short ChurnPause", type, payloadLen, sizeof(ChurnPausePayload));
                break;
            }
            ChurnPause_OnRemotePacket(static_cast<const ChurnPausePayload*>(payload));
            break;
        }

        // --- Frontend lockstep ---
        case PacketType::CharSelFrameInput: {
            if (payloadLen < sizeof(CharSelFrameInputPayload)) {
                LogRouterPacketAnomaly("Short CharSelFrameInput", type, payloadLen,
                                       sizeof(CharSelFrameInputPayload));
                break;
            }
            CharSelSync_OnRemoteFrameInput(
                static_cast<const CharSelFrameInputPayload*>(payload));
            break;
        }

        case PacketType::WinScreenFrameInput: {
            if (payloadLen < sizeof(WinScreenFrameInputPayload)) {
                LogRouterPacketAnomaly("Short WinScreenFrameInput", type, payloadLen,
                                       sizeof(WinScreenFrameInputPayload));
                break;
            }
            WinScreenSync_OnRemoteFrameInput(
                static_cast<const WinScreenFrameInputPayload*>(payload));
            break;
        }

        case PacketType::FrontendPhaseBarrier: {
            if (payloadLen < sizeof(FrontendPhaseBarrierPayload)) {
                LogRouterPacketAnomaly("Short FrontendPhaseBarrier", type, payloadLen,
                                       sizeof(FrontendPhaseBarrierPayload));
                break;
            }
            FrontendInputSync_OnRemotePhaseBarrier(
                static_cast<const FrontendPhaseBarrierPayload*>(payload));
            break;
        }

        case PacketType::FrontendBoundaryDigest: {
            if (payloadLen < sizeof(FrontendBoundaryDigestPayload)) {
                LogRouterPacketAnomaly("Short FrontendBoundaryDigest", type, payloadLen,
                                       sizeof(FrontendBoundaryDigestPayload));
                break;
            }
            FrontendInputSync_OnRemoteBoundaryDigest(
                static_cast<const FrontendBoundaryDigestPayload*>(payload));
            break;
        }

        case PacketType::DelayChangeReq: {
            if (payloadLen < sizeof(DelayChangeReqPayload)) {
                LogRouterPacketAnomaly("Short DelayChangeReq", type, payloadLen,
                                       sizeof(DelayChangeReqPayload));
                break;
            }
            FrontendInputSync_OnRemoteDelayChangeReq(
                static_cast<const DelayChangeReqPayload*>(payload));
            break;
        }

        case PacketType::DelayChangeAck: {
            if (payloadLen < sizeof(DelayChangeAckPayload)) {
                LogRouterPacketAnomaly("Short DelayChangeAck", type, payloadLen,
                                       sizeof(DelayChangeAckPayload));
                break;
            }
            FrontendInputSync_OnRemoteDelayChangeAck(
                static_cast<const DelayChangeAckPayload*>(payload));
            break;
        }

        // --- Palette control-plane (50-52): single owner in every regime ---
        case PacketType::PaletteConfig: {
            if (payloadLen < sizeof(PaletteConfigPayload)) {
                LogRouterPacketAnomaly("Short PaletteConfig", type, payloadLen,
                                       sizeof(PaletteConfigPayload));
                break;
            }
            NetplayPaletteRuntime_OnRemoteConfig(
                static_cast<const PaletteConfigPayload*>(payload));
            break;
        }

        case PacketType::PaletteData: {
            if (payloadLen < sizeof(PaletteDataPayload)) {
                LogRouterPacketAnomaly("Short PaletteData", type, payloadLen,
                                       sizeof(PaletteDataPayload));
                break;
            }
            NetplayPaletteRuntime_OnRemoteData(
                static_cast<const PaletteDataPayload*>(payload));
            break;
        }

        case PacketType::PaletteAck: {
            if (payloadLen < sizeof(PaletteAckPayload)) {
                LogRouterPacketAnomaly("Short PaletteAck", type, payloadLen,
                                       sizeof(PaletteAckPayload));
                break;
            }
            NetplayPaletteRuntime_OnRemoteAck(
                static_cast<const PaletteAckPayload*>(payload));
            break;
        }

        case PacketType::PauseQuit:
            PauseHandler_OnRemotePauseQuit();
            break;

        default:
            // Unknown/unrouted type: log + count, never terminal (§2.3 —
            // forward compat within a protocol version).
            s_unknownPacketCount++;
            Rollback::NetplayLog_Verbose("PACKET", RouterLogFrame(),
                "Unrouted packet: type=%s(%u) payload=%zu total_unrouted=%u pregame=%s lifecycle=%s",
                PacketTypeName(type),
                (unsigned)type,
                payloadLen,
                s_unknownPacketCount,
                PregamePhaseName(PregameSync_GetPhase()),
                MatchLifecyclePhaseName(MatchLifecycle_GetPhase()));
            break;
    }
}

} // namespace Net
