#pragma once

#include "net/protocol.h"
#include <stdint.h>

namespace Net {

// Packet Reliability Classification

/// Classify whether a packet type should be sent reliably.
/// Reliable packets use ENet ENET_PACKET_FLAG_RELIABLE on CHANNEL_CONTROL.
/// Unreliable gameplay packets use sequenced delivery on CHANNEL_GAMEPLAY.
/// Debug packets are unsequenced unreliable on CHANNEL_DEBUG.
inline bool BarrierProtocol_IsReliable(PacketType type) {
    switch (type) {
        case PacketType::Ready:
        case PacketType::Disconnect:
        case PacketType::PeerIdentity:

        case PacketType::SyncAnnounce:
        case PacketType::SyncConfirm:
        case PacketType::FrontendPhaseBarrier:
        case PacketType::FrontendBoundaryDigest:

        case PacketType::NatInfo:
        case PacketType::NatTraversalSignal:

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

        case PacketType::ResyncRequest:
        case PacketType::ResyncReply:
            return true;

        case PacketType::InputStream:
        case PacketType::CharSelFrameInput:
        case PacketType::WinScreenFrameInput:
        case PacketType::TimeProbe:
        case PacketType::TimeProbeAck:
            return false;

        case PacketType::Ping:
        case PacketType::Pong:
        case PacketType::StateDigest:
        case PacketType::SyncTrace:
        case PacketType::ChurnPause:
            return false;

        default:
            return true;  // Unknown → safe default
    }
}

/// Get the correct ENet channel for a packet type.
inline uint8_t BarrierProtocol_GetChannel(PacketType type) {
    switch (type) {
        case PacketType::InputStream:
        case PacketType::CharSelFrameInput:
        case PacketType::WinScreenFrameInput:
        case PacketType::TimeProbe:
        case PacketType::TimeProbeAck:
            return CHANNEL_GAMEPLAY;

        case PacketType::Ping:
        case PacketType::Pong:
        case PacketType::StateDigest:
        case PacketType::SyncTrace:
        case PacketType::ChurnPause:
            return CHANNEL_DEBUG;

        case PacketType::NatTraversalSignal:
            return CHANNEL_CONTROL;

        default:
            return CHANNEL_CONTROL;
    }
}

// Barrier State Types

/// Lifecycle-level barrier phases.
/// These are queries over the combined state of pregame_sync + match_bootstrap.
enum class BarrierPhase : uint8_t {
    None = 0,               // No barrier active
    SessionSync,            // SyncAnnounce/SyncConfirm exchange
    FrontendLockstep,       // CharSel/StageSel lockstep
    ConfigAgreement,        // Config exchange + ack
    LoadWait,               // Load barrier
    BaselineAgreement,      // Baseline capture + CRC agreement
    GameplayReady,          // Both ready, gameplay imminent
    PostMatchDecision,      // Rematch / return / disconnect
};

inline const char* BarrierPhaseName(BarrierPhase phase) {
    switch (phase) {
        case BarrierPhase::None:              return "None";
        case BarrierPhase::SessionSync:       return "SessionSync";
        case BarrierPhase::FrontendLockstep:  return "FrontendLockstep";
        case BarrierPhase::ConfigAgreement:   return "ConfigAgreement";
        case BarrierPhase::LoadWait:          return "LoadWait";
        case BarrierPhase::BaselineAgreement: return "BaselineAgreement";
        case BarrierPhase::GameplayReady:     return "GameplayReady";
        case BarrierPhase::PostMatchDecision: return "PostMatchDecision";
        default:                              return "Unknown";
    }
}

// Barrier State Queries

BarrierPhase BarrierProtocol_GetCurrentPhase();
bool BarrierProtocol_IsBarrierActive();
bool BarrierProtocol_IsGameplayReady();

// Send Helper

/// Send a barrier/control packet with correct reliability and channel
/// classification based on packet type. All pre-game barrier sends
/// (pregame_sync, charsel_sync, match_bootstrap) route through this.
bool BarrierProtocol_SendPacket(PacketType type, const void* payload, size_t len);

} // namespace Net
