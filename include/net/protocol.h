/**
 * Alice Senki 2 - Network Protocol Definitions
 *
 * Packet types, wire header, version constants, and channel assignments
 * for the mod-owned ENet transport layer.
 *
 * All multi-byte fields are little-endian (x86 native).
 * ENet packets carry: [PacketType (2 bytes) | payload]
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Protocol Constants
// ============================================================================

constexpr uint16_t PROTOCOL_VERSION = 2;
constexpr int      MAX_PACKET_SIZE  = 1200;     // Stay under typical MTU
constexpr int      MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - 2;  // minus PacketType

// ============================================================================
// ENet Channel Assignments
// ============================================================================

constexpr uint8_t CHANNEL_CONTROL   = 0;  // Reliable ordered: handshake, session control
constexpr uint8_t CHANNEL_GAMEPLAY  = 1;  // Unreliable sequenced: gameplay input (future)
constexpr uint8_t CHANNEL_DEBUG     = 2;  // Unreliable: diagnostics, state digests
constexpr uint8_t NUM_CHANNELS      = 3;

// ============================================================================
// Packet Types
// ============================================================================

enum class PacketType : uint16_t {
    // Session control (reliable, channel 0)
    Hello           = 1,    // Initiator sends after ENet connect
    HelloAck        = 2,    // Responder replies with own info
    Ready           = 3,    // Peer is ready for next phase
    Disconnect      = 4,    // Graceful disconnect with reason
    SessionMeta     = 5,    // Arbitrary session metadata exchange

    // Initial session sync (reliable, channel 0)
    SyncAnnounce    = 6,    // Session metadata announce for pre-game agreement
    SyncConfirm     = 7,    // Session sync confirmation / side assignment

    // NAT traversal coordination (reliable, channel 0)
    NatInfo         = 10,   // Exchange NAT/UPnP status

    // Pre-game sync (reliable, channel 0)
    CharSelInput    = 11,   // CharSel cursor/confirm state exchange
    CharSelLock     = 12,   // Both confirmed — lock character selections
    StageSync       = 13,   // Stage selection exchange/lock
    ConfigExchange  = 14,   // LockedMatchConfig proposed by host
    ConfigAck       = 15,   // Join acknowledges config (includes config hash)
    LoadBarrier     = 16,   // Peer finished loading assets
    BaselineReady   = 17,   // Peer captured baseline savestate
    BaselineDigest  = 18,   // CRC32 of baseline savestate for agreement
    GameplayStart   = 19,   // Both peers ready — begin gameplay

    // CharSel lockstep (unreliable, channel 1)
    CharSelFrameInput = 40, // Per-frame charsel input with redundancy

    // Win screen / pause (reliable, channel 0)
    WinScreenConfirm = 41,  // Win screen A/C confirm signal
    PauseQuit        = 42,  // Pause menu quit signal

    // Gameplay (unreliable, channel 1) — placeholders for rollback layer
    GameplayInput   = 20,   // Reserved for future rollback input sync

    // Mid-match delay changes (reliable, channel 0)
    DelayChangeReq  = 21,   // Request to change input delay
    DelayChangeAck  = 22,   // Acknowledge delay change request

    // Debug / diagnostics (unreliable, channel 2)
    Ping            = 30,   // Application-level ping (supplements ENet RTT)
    Pong            = 31,   // Application-level pong
    StateDigest     = 32,   // CRC32 state digest for desync detection
};

// ============================================================================
// Wire Payloads
// ============================================================================

#pragma pack(push, 1)

struct HelloPayload {
    uint16_t protocol_version;   // Must match PROTOCOL_VERSION
    uint32_t build_hash;         // Compile-time build identifier
    char     nickname[24];       // Null-terminated UTF-8 nickname
    uint16_t listen_port;        // Port this peer is listening on
};

struct HelloAckPayload {
    uint16_t protocol_version;
    uint32_t build_hash;
    char     nickname[24];
    uint16_t listen_port;
};

struct DisconnectPayload {
    uint16_t reason_code;        // 0 = normal, 1 = timeout, 2 = version mismatch, 3 = error
    char     message[64];        // Human-readable reason
};

struct PingPayload {
    uint32_t ping_id;            // Echoed back in Pong
    uint32_t send_time_ms;       // Local timestamp (GetTickCount)
};

struct PongPayload {
    uint32_t ping_id;
    uint32_t original_send_time_ms;
    uint32_t responder_time_ms;
};

struct StateDigestPayload {
    uint32_t frame_number;
    uint32_t crc32;
};

// Initial session sync payloads

struct SyncAnnouncePayload {
    uint32_t session_id;         // Random session identifier for this match
    uint8_t  capability_flags;   // Bit 0: savestate baseline, Bit 1: desync diagnostics
    uint8_t  _pad[3];
};

struct SyncConfirmPayload {
    uint32_t session_id;         // Agreed session ID (host's ID is authoritative)
    uint8_t  confirmed;          // 1 = all checks passed
    uint8_t  assigned_side;      // Host decides: 0 = host is P1, 1 = host is P2
    uint8_t  _pad[2];
};

// Pre-game sync payloads

struct CharSelInputPayload {
    uint8_t  cursor;             // Grid index (0-20)
    uint8_t  confirmed;          // 0 = browsing, 1 = confirmed
    uint8_t  palette;            // Palette selection (0-7)
    uint8_t  _pad;
};

struct CharSelLockPayload {
    uint8_t  character_id;       // Resolved character ID from grid table
    uint8_t  palette;            // Final palette
    uint8_t  _pad[2];
};

struct StageSyncPayload {
    uint8_t  stage_id;           // Selected stage ID
    uint8_t  confirmed;          // 0 = browsing, 1 = locked
    uint8_t  _pad[2];
};

struct ConfigExchangePayload {
    uint8_t  p1_character;
    uint8_t  p1_palette;
    uint8_t  p2_character;
    uint8_t  p2_palette;
    uint8_t  stage_id;
    uint8_t  host_side;          // 0 = host is P1, 1 = host is P2
    uint8_t  round_count;
    uint8_t  time_limit;
    uint32_t rng_seed;
    uint32_t session_seed;
    // Delay negotiation (sender's preferences)
    uint8_t  delay_configured;   // 0 = auto, 1-15 = manual preference
    uint8_t  delay_recommended;  // Auto-computed from RTT measurement
    uint8_t  delay_rollback;     // Rollback budget (frames)
    uint8_t  _delay_pad;
};

struct ConfigAckPayload {
    uint32_t config_hash;        // CRC32 of the LockedMatchConfig peer built
    uint8_t  accepted;           // 1 = matches, 0 = mismatch
    // Delay negotiation (join's preferences)
    uint8_t  delay_configured;   // 0 = auto, 1-15 = manual preference
    uint8_t  delay_recommended;  // Auto-computed from RTT measurement
    uint8_t  delay_rollback;     // Rollback budget (frames)
};

struct LoadBarrierPayload {
    uint8_t  loaded;             // 1 = assets loaded
    uint8_t  _pad[3];
};

struct BaselineReadyPayload {
    uint8_t  captured;           // 1 = baseline savestate captured
    uint8_t  _pad[3];
};

struct BaselineDigestPayload {
    uint32_t crc32;              // CRC32 of baseline savestate
};

struct GameplayStartPayload {
    uint32_t start_frame;        // Agreed frame to begin gameplay (typically 0)
};

struct CharSelFrameInputPayload {
    uint32_t frame;              // Lockstep frame number
    uint32_t ack_frame;          // Latest remote frame we received
    uint16_t inputs[4];          // Redundant history: [frame, frame-1, frame-2, frame-3]
    uint16_t input_count;        // Number of valid entries in inputs[] (1-4)
    uint16_t _pad;
};

struct DelayChangeReqPayload {
    uint8_t  new_delay;          // Requested input delay (1-15)
    uint8_t  _pad[3];
};

struct DelayChangeAckPayload {
    uint8_t  acked_delay;        // Acknowledged delay value
    uint8_t  accepted;           // 1 = accepted, 0 = counter-proposed
    uint8_t  _pad[2];
};

#pragma pack(pop)

// ============================================================================
// Helpers
// ============================================================================

inline const char* PacketTypeName(PacketType type) {
    switch (type) {
        case PacketType::Hello:          return "Hello";
        case PacketType::HelloAck:       return "HelloAck";
        case PacketType::Ready:          return "Ready";
        case PacketType::Disconnect:     return "Disconnect";
        case PacketType::SessionMeta:    return "SessionMeta";
        case PacketType::SyncAnnounce:   return "SyncAnnounce";
        case PacketType::SyncConfirm:    return "SyncConfirm";
        case PacketType::NatInfo:        return "NatInfo";
        case PacketType::CharSelInput:   return "CharSelInput";
        case PacketType::CharSelLock:    return "CharSelLock";
        case PacketType::StageSync:      return "StageSync";
        case PacketType::ConfigExchange: return "ConfigExchange";
        case PacketType::ConfigAck:      return "ConfigAck";
        case PacketType::LoadBarrier:    return "LoadBarrier";
        case PacketType::BaselineReady:  return "BaselineReady";
        case PacketType::BaselineDigest: return "BaselineDigest";
        case PacketType::GameplayStart:  return "GameplayStart";
        case PacketType::GameplayInput:      return "GameplayInput";
        case PacketType::CharSelFrameInput:  return "CharSelFrameInput";
        case PacketType::WinScreenConfirm:   return "WinScreenConfirm";
        case PacketType::PauseQuit:           return "PauseQuit";
        case PacketType::Ping:           return "Ping";
        case PacketType::Pong:           return "Pong";
        case PacketType::StateDigest:    return "StateDigest";
        default:                         return "Unknown";
    }
}

/// Validate that a received buffer is large enough for [PacketType + payload].
/// Returns false if the buffer is too small for the packet type header.
inline bool ValidatePacketSize(const void* data, size_t length) {
    return data != nullptr && length >= sizeof(PacketType);
}

/// Extract PacketType from the front of a raw buffer.
inline PacketType ReadPacketType(const void* data) {
    return *static_cast<const PacketType*>(data);
}

/// Get a pointer to the payload after the PacketType header.
inline const void* GetPayloadPtr(const void* data) {
    return static_cast<const uint8_t*>(data) + sizeof(PacketType);
}

/// Get payload size from total packet length.
inline size_t GetPayloadSize(size_t totalLength) {
    return totalLength > sizeof(PacketType) ? totalLength - sizeof(PacketType) : 0;
}

} // namespace Net
