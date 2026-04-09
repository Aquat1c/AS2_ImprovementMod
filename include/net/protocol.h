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

constexpr uint16_t PROTOCOL_VERSION = 3;
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
    NatTraversalSignal = 34, // Exchange ICE description/candidates over session control

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
    BaselineBreakdown = 25, // Detailed per-region baseline CRCs for mismatch diagnosis

    // CharSel lockstep (unreliable, channel 1)
    CharSelFrameInput = 40, // Per-frame charsel input with redundancy

    // Win screen / pause
    WinScreenConfirm    = 41,  // Legacy win screen one-shot confirm signal
    PauseQuit           = 42,  // Pause menu quit signal
    WinScreenFrameInput = 43,  // Mode 9 lockstep frame input with redundancy

    // Gameplay (unreliable, channel 1)
    GameplayInput   = 20,   // Legacy mod-owned rollback input sync (DEAD — no send site)
    GekkoData       = 23,   // Raw GekkoNet internal protocol data

    // Startup gameplay-entry barrier (reliable, channel 0)
    // Sent when first post-intro interactive boundary is reached while held;
    // release requires mutual ready+ack.
    GekkoReady      = 24,   // Startup barrier control (ready/ack)

    // Mid-match delay changes (reliable, channel 0)
    DelayChangeReq  = 21,   // Request to change input delay
    DelayChangeAck  = 22,   // Acknowledge delay change request

    // Debug / diagnostics (unreliable, channel 2)
    Ping            = 30,   // Application-level ping (supplements ENet RTT)
    Pong            = 31,   // Application-level pong
    StateDigest     = 32,   // CRC32 state digest for desync detection
    FrameSyncStatus = 33,   // Lightweight frame-progress telemetry
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

struct FrameSyncStatusPayload {
    int32_t  current_frame;     // Sender's logical rollback frame
    int32_t  game_frame;        // Sender's native sim frame counter
    int32_t  remote_view_frame; // Sender's latest confirmed frame for the peer
    int32_t  confirmed_frame;   // Sender's fully confirmed frame
    int32_t  predicted_frames;  // Sender's outstanding predicted frames
    uint32_t checksum;          // Sender's current state checksum
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

struct NatInfoPayload {
    uint8_t  flags;              // NAT_INFO_FLAG_*
    uint8_t  upnp_status;        // Net::NatStatus
    uint8_t  stun_status;        // Net::StunStatus
    uint8_t  extra_flags;        // NAT_INFO_EX_FLAG_*
    uint16_t listen_port;        // Local listen port used by ENet host
    uint16_t external_port;      // External/stun-discovered UDP port
    char     external_ip[48];    // UPnP/STUN external IP string (v4/v6 text)
};

struct NatTraversalSignalPayload {
    uint8_t  signal_type;        // Net::NatSignalType
    uint8_t  _pad;
    uint16_t text_len;           // bytes used in text[]
    char     text[1024];         // ICE SDP/candidate payload (trickle)
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
    uint8_t  delay_rollback_delay; // Input pipeline delay for rollback mode
};

struct ConfigAckPayload {
    uint32_t config_hash;        // CRC32 of the LockedMatchConfig peer built
    uint8_t  accepted;           // 1 = matches, 0 = mismatch
    // Delay negotiation (join's preferences)
    uint8_t  delay_configured;   // 0 = auto, 1-15 = manual preference
    uint8_t  delay_recommended;  // Auto-computed from RTT measurement
    uint8_t  delay_rollback;     // Rollback budget (frames)
    uint8_t  delay_rollback_delay; // Input pipeline delay for rollback mode
};

struct LoadBarrierPayload {
    uint8_t  loaded;             // 1 = assets loaded
    uint8_t  mode;               // Local game mode when barrier was sent
    uint8_t  substate;           // Local substate when barrier was sent
    uint8_t  _pad;
    uint32_t sim_frame;          // Native sim frame at load completion
};

struct BaselineReadyPayload {
    uint8_t  captured;           // 1 = baseline savestate captured
    uint8_t  mode;               // Local game mode when baseline was captured
    uint8_t  substate;           // Local substate when baseline was captured
    uint8_t  _pad;
    uint32_t sim_frame;          // Native sim frame when baseline was captured
};

struct BaselineDigestPayload {
    uint32_t crc32;              // CRC32 of baseline savestate
};

struct BaselineBreakdownPayload {
    // Baseline CRC components
    uint32_t main_crc;
    uint32_t header_crc;
    uint32_t context_crc;
    uint32_t effect_crc;
    uint32_t summon_crc;
    uint32_t p1_entity_crc;
    uint32_t p2_entity_crc;

    // Adjacent/non-hash diagnostics
    uint32_t pre_match_gap_crc;
    uint32_t p1_input_crc;
    uint32_t p2_input_crc;
    uint32_t per_frame_temp_crc;

    // Contextual counters
    uint32_t rng_seed;
    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t game_mode;
    uint32_t substate;
    uint32_t game_type;
    uint32_t match_phase_timer;
    uint32_t frame_simulation;
    uint32_t frame_display;
    uint32_t frame_write_idx;
    uint32_t frame_net_idx;
    uint32_t remote_frame_idx;

    // Raw critical bytes
    uint8_t  match_header_bytes[16];
    uint8_t  pre_match_gap_bytes[12];
};

struct GameplayStartPayload {
    uint32_t start_frame;        // Bootstrap baseline frame (typically 0)
    uint32_t host_sim_frame;     // Host native sim frame when start was sent
};

struct GekkoReadyPayload {
    uint8_t  flags;              // GEKKO_READY_FLAG_*
    uint8_t  phase;              // Sender MatchLifecyclePhase at send time
    uint16_t _pad;
    int32_t  rollback_frame;     // Sender rollback frame at send time
};

struct CharSelFrameInputPayload {
    uint32_t frame;              // Lockstep frame number
    uint32_t ack_frame;          // Sender's consumeFrame (frame they need from us)
    uint16_t inputs[8];          // Redundant history: [frame, frame-1, ..., frame-7]
    uint16_t input_count;        // Number of valid entries in inputs[] (1-8)
    uint16_t _pad;
};

struct WinScreenFrameInputPayload {
    uint32_t frame;              // Lockstep frame number
    uint32_t ack_frame;          // Sender's consumeFrame (frame they need from us)
    uint16_t inputs[8];          // Redundant history: [frame, frame-1, ..., frame-7]
    uint16_t input_count;        // Number of valid entries in inputs[] (1-8)
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

// GekkoReadyPayload flags
constexpr uint8_t GEKKO_READY_FLAG_READY = 1 << 0;  // Local reached post-intro interactive boundary
constexpr uint8_t GEKKO_READY_FLAG_ACK   = 1 << 1;  // Local has observed peer READY

// NatInfoPayload flags
constexpr uint8_t NAT_INFO_FLAG_UPNP_ENABLED      = 1 << 0;
constexpr uint8_t NAT_INFO_FLAG_UPNP_MAPPED       = 1 << 1;
constexpr uint8_t NAT_INFO_FLAG_STUN_ENABLED      = 1 << 2;
constexpr uint8_t NAT_INFO_FLAG_STUN_OK           = 1 << 3;
constexpr uint8_t NAT_INFO_FLAG_HOLE_PUNCH        = 1 << 4;
constexpr uint8_t NAT_INFO_FLAG_RELAY_FALLBACK    = 1 << 5;
constexpr uint8_t NAT_INFO_FLAG_PREFER_DIRECT     = 1 << 6;
constexpr uint8_t NAT_INFO_FLAG_IPV6_ENDPOINTS    = 1 << 7;
constexpr uint8_t NAT_INFO_EX_FLAG_TURN_ENABLED   = 1 << 0;
constexpr uint8_t NAT_INFO_EX_FLAG_PCP_ENABLED    = 1 << 1;

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
        case PacketType::NatTraversalSignal: return "NatTraversalSignal";
        case PacketType::CharSelInput:   return "CharSelInput";
        case PacketType::CharSelLock:    return "CharSelLock";
        case PacketType::StageSync:      return "StageSync";
        case PacketType::ConfigExchange: return "ConfigExchange";
        case PacketType::ConfigAck:      return "ConfigAck";
        case PacketType::LoadBarrier:    return "LoadBarrier";
        case PacketType::BaselineReady:  return "BaselineReady";
        case PacketType::BaselineDigest: return "BaselineDigest";
        case PacketType::BaselineBreakdown: return "BaselineBreakdown";
        case PacketType::GameplayStart:  return "GameplayStart";
        case PacketType::GekkoReady:     return "GekkoReady";
        case PacketType::GameplayInput:   return "GameplayInput";
        case PacketType::DelayChangeReq:  return "DelayChangeReq";
        case PacketType::DelayChangeAck:  return "DelayChangeAck";
        case PacketType::GekkoData:       return "GekkoData";
        case PacketType::CharSelFrameInput:   return "CharSelFrameInput";
        case PacketType::WinScreenConfirm:    return "WinScreenConfirm";
        case PacketType::PauseQuit:           return "PauseQuit";
        case PacketType::WinScreenFrameInput: return "WinScreenFrameInput";
        case PacketType::Ping:           return "Ping";
        case PacketType::Pong:           return "Pong";
        case PacketType::StateDigest:    return "StateDigest";
        case PacketType::FrameSyncStatus:return "FrameSyncStatus";
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
