/**
 * Alice Senki 2 - Gameplay Packet Router Implementation (re0.7 M0)
 *
 * Body moved verbatim from online_wiring.cpp OnGameplayPacket; the two
 * engine-coupled cases (engine input stream, startup barrier) call back into
 * online_wiring, which still owns that state.
 */

#include "net/gameplay_packet_router.h"

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
        reason ? reason : "gameplay packet anomaly",
        PacketTypeName(type),
        payloadLen,
        expectedLen,
        Rollback::RollbackSession_IsActive() ? 1 : 0);
}

} // anonymous namespace

void GameplayPacketRouter_OnPacket(PacketType type, const void* payload, size_t payloadLen) {
    // Wire-acknowledged transition barriers must be reachable regardless of
    // which callback owns the slot — winscreen-exit proposals arrive exactly
    // while this handler is installed.
    if (TransitionBarrier_OnPacket(type, payload, payloadLen)) {
        return;
    }
    switch (type) {
        case PacketType::InputStream: {
            // Engine input stream (Gekko internal data until the M5 cutover) —
            // owned by online_wiring/rollback_session.
            Rollback::OnlineWiring_HandleEngineDataPacket(payload, payloadLen);
            break;
        }

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

        case PacketType::GekkoReady: {
            // Startup gameplay-entry barrier — owned by online_wiring.
            Rollback::OnlineWiring_HandleStartupBarrierPacket(payload, payloadLen);
            break;
        }

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

        case PacketType::CharSelLock: {
            if (payloadLen < sizeof(CharSelLockPayload)) {
                LogRouterPacketAnomaly("Short CharSelLock", type, payloadLen,
                                       sizeof(CharSelLockPayload));
                break;
            }
            CharSelSync_OnRemoteLock(
                static_cast<const CharSelLockPayload*>(payload));
            break;
        }

        case PacketType::StageSync: {
            if (payloadLen < sizeof(StageSyncPayload)) {
                LogRouterPacketAnomaly("Short StageSync", type, payloadLen,
                                       sizeof(StageSyncPayload));
                break;
            }
            CharSelSync_OnRemoteStage(
                static_cast<const StageSyncPayload*>(payload));
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

        case PacketType::SyncAnnounce:
        case PacketType::SyncConfirm:
            if (PregameSync_HandleCrossPhaseSessionPacket(type, payload, payloadLen)) {
                break;
            }
            Rollback::NetplayLog_Write("HANDOFF", RouterLogFrame(),
                "Ignored cross-phase session sync packet: type=%s pregame=%s lifecycle=%s",
                PacketTypeName(type),
                PregamePhaseName(PregameSync_GetPhase()),
                MatchLifecyclePhaseName(MatchLifecycle_GetPhase()));
            break;

        default:
            if (type == PacketType::PauseQuit) {
                PauseHandler_OnRemotePauseQuit();
                break;
            }
            // Not a gameplay packet; unhandled at this level.
            // During gameplay, the pregame handler is not active,
            // so non-gameplay packets are just logged.
            Rollback::NetplayLog_Verbose("PACKET", -1,
                "Unhandled packet during gameplay: type=%s(%u) payload=%zu",
                PacketTypeName(type),
                (unsigned)type,
                payloadLen);
            break;
    }
}

} // namespace Net
