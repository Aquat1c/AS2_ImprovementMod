/**
 * Alice Senki 2 - Barrier Protocol Semantics
 *
 * Standalone module that defines and documents the barrier/sync
 * semantics used during pre-game, gameplay, and post-match phases.
 *
 * This module does NOT replace pregame_sync or match_bootstrap.
 * It provides:
 *   1. Explicit documentation of each barrier type and its semantics
 *   2. Helpers for barrier state queries that other modules can use
 *   3. Packet reliability classification
 *
 * Barrier types in the lifecycle:
 *
 *   SYNC BARRIERS (pregame_sync.cpp):
 *     - SyncAnnounce/SyncConfirm: session identity agreement
 *     - CharSelInput/CharSelLock: character selection lockstep
 *     - StageSync: stage selection lockstep
 *     → All reliable, CHANNEL_CONTROL
 *
 *   BOOTSTRAP BARRIERS (match_bootstrap.cpp):
 *     - ConfigExchange/ConfigAck: match config agreement
 *     - LoadBarrier: both peers finished asset loading
 *     - BaselineReady/BaselineDigest: savestate baseline agreement
 *     - GameplayStart: coordinated gameplay kickoff
 *     → All reliable, CHANNEL_CONTROL
 *
 *   GAMEPLAY STREAM (rollback_session.cpp):
 *     - GameplayInput: rollback input sync
 *     → Unreliable sequenced, CHANNEL_GAMEPLAY
 *     → Redundant input batch for packet loss resilience
 *
 *   DIAGNOSTICS (rollback_debug.cpp):
 *     - StateDigest: desync detection CRC32
 *     - Ping/Pong: application-level RTT
 *     → Unreliable, CHANNEL_DEBUG
 *
 *   POST-MATCH BARRIERS (future, via pregame_sync):
 *     - Rematch request: both peers agree to rematch
 *     - Return-to-CharSel: both peers agree to return
 *     - Disconnect during barrier: graceful teardown
 *     → Reliable, CHANNEL_CONTROL
 */

#pragma once

#include "net/protocol.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// Packet Reliability Classification
// ============================================================================

/// Classify whether a packet type should be sent reliably.
/// Reliable packets use ENet ENET_PACKET_FLAG_RELIABLE on CHANNEL_CONTROL.
/// Unreliable gameplay packets use sequenced delivery on CHANNEL_GAMEPLAY.
/// Debug packets are unsequenced unreliable on CHANNEL_DEBUG.
inline bool BarrierProtocol_IsReliable(PacketType type) {
    switch (type) {
        // Session control — always reliable
        case PacketType::Hello:
        case PacketType::HelloAck:
        case PacketType::Ready:
        case PacketType::Disconnect:
        case PacketType::SessionMeta:

        // Sync barriers — always reliable
        case PacketType::SyncAnnounce:
        case PacketType::SyncConfirm:

        // NAT coordination — reliable
        case PacketType::NatInfo:

        // Pre-game barriers — always reliable
        case PacketType::CharSelInput:
        case PacketType::CharSelLock:
        case PacketType::StageSync:
        case PacketType::ConfigExchange:
        case PacketType::ConfigAck:
        case PacketType::LoadBarrier:
        case PacketType::BaselineReady:
        case PacketType::BaselineDigest:
        case PacketType::GameplayStart:

        // Startup gameplay-entry barrier — reliable
        case PacketType::GekkoReady:
            return true;

        // Gameplay stream — unreliable (redundancy handles loss)
        case PacketType::GameplayInput:
        case PacketType::CharSelFrameInput:
            return false;

        // Diagnostics — unreliable
        case PacketType::Ping:
        case PacketType::Pong:
        case PacketType::StateDigest:
            return false;

        default:
            return true;  // Unknown → safe default
    }
}

/// Get the correct ENet channel for a packet type.
inline uint8_t BarrierProtocol_GetChannel(PacketType type) {
    switch (type) {
        case PacketType::GameplayInput:
        case PacketType::CharSelFrameInput:
            return CHANNEL_GAMEPLAY;

        case PacketType::Ping:
        case PacketType::Pong:
        case PacketType::StateDigest:
            return CHANNEL_DEBUG;

        // Startup gameplay-entry barrier — control channel, same as GameplayStart
        case PacketType::GekkoReady:
            return CHANNEL_CONTROL;

        default:
            return CHANNEL_CONTROL;
    }
}

// ============================================================================
// Barrier State Types
// ============================================================================

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

// ============================================================================
// Barrier State Queries
// ============================================================================

BarrierPhase BarrierProtocol_GetCurrentPhase();
bool BarrierProtocol_IsBarrierActive();
bool BarrierProtocol_IsGameplayReady();

// ============================================================================
// Send Helper
// ============================================================================

/// Send a barrier/control packet with correct reliability and channel
/// classification based on packet type. All pre-game barrier sends
/// (pregame_sync, charsel_sync, match_bootstrap) route through this.
bool BarrierProtocol_SendPacket(PacketType type, const void* payload, size_t len);

} // namespace Net
