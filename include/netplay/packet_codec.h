/**
 * Alice Senki 2 - Packet Codec
 *
 * Wire protocol header, payload structs, and serialization.
 * Matches PLAN.md Sections 10.3–11.4.
 *
 * All multi-byte fields are little-endian (x86 native).
 */

#pragma once

#include <stdint.h>

namespace PacketCodec {

// ============================================================================
// Protocol Constants
// ============================================================================

constexpr uint32_t PROTOCOL_MAGIC   = 0x55325341;  // "AS2U" in little-endian
constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr uint16_t HEADER_SIZE      = 52;           // sizeof(ModNetHeader)
constexpr int      MAX_PACKET_SIZE  = 1200;         // Stay under typical MTU
constexpr int      MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE;

// ============================================================================
// Logical Channels (§9.3)
// ============================================================================

constexpr uint16_t CHANNEL_CONTROL_RELIABLE = 0;
constexpr uint16_t CHANNEL_CONTROL_FAST     = 1;
constexpr uint16_t CHANNEL_GAMEPLAY         = 2;
constexpr uint16_t CHANNEL_SPECTATOR        = 3;

// ============================================================================
// Packet Types
// ============================================================================

enum class PacketType : uint16_t {
    // ── Active (ENet transport) ──
    Hello             = 1,   // AppHandshake exchange after ENet connect
    HelloAck          = 2,   // [LEGACY] Pre-ENet handshake ack — unused
    Ready             = 3,   // Host→Joiner CharSel start signal
    Ping              = 4,   // [LEGACY] Pre-ENet RTT probe — ENet provides RTT natively
    Pong              = 5,   // [LEGACY] Pre-ENet RTT reply — unused
    StateDigest       = 6,   // Periodic desync detection
    GekkoData         = 7,   // Opaque GekkoNet wire data (rollback session)

    // ── CharSel + Bootstrap ──
    CharSelInput      = 8,   // Raw input relay during CharSel
    CharSelEvent      = 9,   // [LEGACY] Event-based CharSel sync — replaced by CharSelInput
    MatchConfig       = 10,
    MatchConfigAck    = 11,
    LoadBarrierReady  = 12,
    BaselineReady     = 13,
    StartGameplay     = 14,
    StartGameplayAck  = 15,

    // ── Spectator (skeleton — not yet implemented) ──
    SpectatorHello    = 16,
    SpectatorAccept   = 17,
    SpectatorReject   = 18,
    SpectatorKeyframe = 19,

    // ── Post-Match / Control ──
    Disconnect        = 20,  // [LEGACY] Pre-ENet disconnect — ENet handles disconnection
    Rematch           = 21,
    ErrorNotice       = 22,

    // ── Adaptive Delay ──
    DelayChangeRequest = 23,
    DelayChangeAck     = 24,
};

// ============================================================================
// Flags
// ============================================================================

enum PacketFlags : uint16_t {
    FLAG_RELIABLE      = 1 << 0,
    FLAG_ORDERED       = 1 << 1,
    FLAG_HANDSHAKE     = 1 << 4,
    FLAG_DEBUG         = 1 << 5,
};

// ============================================================================
// Wire Header (52 bytes, packed) — LEGACY
// Pre-ENet wire format. Still used by spectator_manager skeleton code.
// Active ENet transport uses [1 byte PacketType | payload] instead.
// ============================================================================

#pragma pack(push, 1)
struct ModNetHeader {
    uint32_t magic;               // PROTOCOL_MAGIC
    uint16_t protocol_version;    // PROTOCOL_VERSION
    uint16_t header_size;         // sizeof(ModNetHeader) = 52
    uint64_t session_id;          // Session identity
    uint32_t connection_id;       // Distinguishes reconnects
    uint32_t seq;                 // Per-channel outgoing sequence
    uint32_t ack;                 // Highest contiguous remote seq received
    uint32_t ack_bits;            // Selective ack bitfield (prev 32)
    uint16_t channel;             // Logical channel
    uint16_t packet_type;         // PacketType enum
    uint16_t flags;               // PacketFlags
    uint16_t reserved;            // Must be 0
    uint16_t payload_size;        // Payload length after header
    uint16_t header_crc16;        // CRC16 of header (with this field = 0)
    uint32_t send_timestamp_ms;   // For RTT estimation
    uint32_t payload_crc32;       // CRC32 of payload bytes
};
#pragma pack(pop)

static_assert(sizeof(ModNetHeader) == 52, "ModNetHeader must be 52 bytes");

// ============================================================================
// Payload Structs
// ============================================================================

#pragma pack(push, 1)

struct HelloPayload {
    uint32_t build_hash;
    uint32_t gameplay_hash;
    uint32_t feature_flags;
    uint16_t requested_player_slot;  // 0 = no preference
    uint16_t listen_port;
    uint64_t client_nonce;
    uint16_t nickname_len;
    char     nickname[24];           // Fixed-size for simplicity (null-padded)
};

struct HelloAckPayload {
    uint32_t accepted_feature_flags;
    uint16_t assigned_player_slot;   // 1 = P1, 2 = P2
    uint16_t max_spectators;
    uint64_t server_nonce;
    uint32_t reject_reason;          // 0 = accepted, see REJECT_* constants
    uint32_t build_hash;             // Host's build hash for joiner verification
    uint16_t nickname_len;
    char     nickname[24];           // Host's nickname (null-padded)
};

// Reject reason codes for HelloAck
constexpr uint32_t REJECT_NONE              = 0;
constexpr uint32_t REJECT_BUILD_MISMATCH    = 1;  // build_hash doesn't match
constexpr uint32_t REJECT_PROTOCOL_MISMATCH = 2;  // protocol_version mismatch (caught earlier)
constexpr uint32_t REJECT_LOBBY_FULL        = 3;  // no available slots

struct PingPayload {
    uint32_t ping_id;
    uint32_t local_time_ms;
};

struct PongPayload {
    uint32_t ping_id;
    uint32_t echoed_time_ms;
};

struct DisconnectPayload {
    uint32_t reason_code;
};

// ── Ready / CharSel Start (§5.2) ──
// Sent by the host to tell the joiner "enter CharSel now".
// The joiner responds with the same packet to confirm.
struct ReadyPayload {
    uint32_t ready_flags;  // 0 = normal CharSel start
};

// State digest for desync detection (sent periodically)
struct StateDigestPayload {
    uint32_t frame_number;        // ADDR_SIM_FRAME_COUNTER
    uint32_t game_mode;           // ADDR_GAME_MODE
    uint32_t sub_state;           // ADDR_SUB_STATE
    uint32_t rng_seed;            // Current RNG seed
    // P1 state
    int16_t  p1_hp;
    int16_t  p1_x;
    int16_t  p1_y;
    uint16_t p1_meter;
    uint32_t p1_action_id;
    uint16_t p1_input;
    // P2 state
    int16_t  p2_hp;
    int16_t  p2_x;
    int16_t  p2_y;
    uint16_t p2_meter;
    uint32_t p2_action_id;
    uint16_t p2_input;
    // Match state
    int32_t  round_timer;
    uint16_t quick_checksum;      // AS2_GetQuickChecksum()
    uint32_t full_state_crc;      // CRC32 of critical state
};

// ── CharSel Input (lockstep input sync for CharSel) ──
// Each packet carries the sender's latest input plus redundant history
// so that a single lost packet doesn't cause permanent divergence.
static const int kCharSelInputRedundancy = 8;

struct CharSelInputPayload {
    uint32_t frame;           // Frame number of the LATEST input in this packet
    uint32_t ack_frame;       // Latest remote frame the sender has received
    uint16_t input_count;     // How many inputs in the history (1..kCharSelInputRedundancy)
    uint16_t reserved;
    // inputs[0] = input for `frame`, inputs[1] = input for `frame-1`, etc.
    uint16_t inputs[kCharSelInputRedundancy];
};

// ── CharSel Event (§11.5) ──
struct CharSelEventPayload {
    uint32_t event_seq;
    uint8_t  sender_slot;        // 1=P1, 2=P2
    uint8_t  phase;              // CharSelPhase enum
    uint16_t event_type;         // CharSelEventType enum
    int32_t  arg0;
    int32_t  arg1;
    uint32_t state_hash;
};

// ── Match Config (§11.6) ──
struct MatchConfigPayload {
    uint32_t config_hash;
    uint32_t p1_character;
    uint32_t p2_character;
    uint32_t p1_palette;
    uint32_t p2_palette;
    uint32_t stage_id;
    uint32_t round_count;
    uint32_t timer_setting;
    uint32_t gameplay_flags;
    uint32_t session_seed;
};

// ── Match Config Ack (§11.7) ──
struct MatchConfigAckPayload {
    uint32_t config_hash;
    uint32_t accepted;           // 1=accepted, 0=rejected
};

// ── Load Barrier Ready (§11.8) ──
struct LoadBarrierReadyPayload {
    uint32_t config_hash;
    uint32_t load_flags;
    uint32_t local_asset_hash;
};

// ── Baseline Ready (§11.9) ──
struct BaselineReadyPayload {
    uint32_t config_hash;
    uint32_t baseline_frame;
    uint32_t baseline_checksum;
};

// ── Start Gameplay (§11.10) ──
struct StartGameplayPayload {
    uint32_t config_hash;
    uint32_t baseline_frame;
    uint32_t start_epoch_ms;
    uint32_t input_delay_p1;
    uint32_t input_delay_p2;
};

struct StartGameplayAckPayload {
    uint32_t config_hash;
    uint32_t baseline_frame;
    uint32_t accepted_delay;
};

// ── Rematch (§11.13) ──
struct RematchPayload {
    uint32_t rematch_seq;
    uint32_t decision;           // 0=decline, 1=accept
};

// ── Error Notice ──
struct ErrorNoticePayload {
    uint32_t error_code;
    uint32_t related_packet_type;
};

// ── Delay Change Request (adaptive delay negotiation) ──
struct DelayChangeRequestPayload {
    uint32_t requested_delay;    // New delay value (frames)
    uint32_t effective_frame;    // Frame at which both sides apply the change
    uint32_t request_seq;        // Monotonic sequence to deduplicate
};

// ── Delay Change Ack ──
struct DelayChangeAckPayload {
    uint32_t accepted_delay;     // Echoed delay value (must match request)
    uint32_t effective_frame;    // Echoed frame (must match request)
    uint32_t request_seq;        // Echoed sequence
};

#pragma pack(pop)

// ============================================================================
// CRC Functions
// ============================================================================

uint16_t Crc16(const void* data, int len);
uint32_t Crc32(const void* data, int len);

// ============================================================================
// Encode / Decode
// ============================================================================

// Get current time in ms (for send_timestamp_ms)
uint32_t GetTimestampMs();

// Initialize header fields. Caller fills packet_type, channel, flags, etc.
void InitHeader(ModNetHeader* hdr, PacketType type, uint16_t channel = 0, uint16_t flags = 0);

// Encode a complete packet into a buffer.
// header.payload_size, header_crc16, and payload_crc32 are computed automatically.
// Returns total bytes written, or 0 on error.
int Encode(const ModNetHeader* hdr, const void* payload, int payloadLen,
           void* outBuf, int outBufLen);

// Decode a received buffer into header + payload pointer.
// Validates magic, version, CRCs.
// On success: fills hdr, sets *outPayload to point into buf, *outPayloadLen = payload size.
// Returns true on success.
bool Decode(const void* buf, int bufLen,
            ModNetHeader* hdr, const void** outPayload, int* outPayloadLen);

// Get human-readable name for a packet type
const char* GetPacketTypeName(PacketType type);

// Compute CRC32 hash of a MatchConfigPayload for agreement
uint32_t HashMatchConfig(const MatchConfigPayload* config);

} // namespace PacketCodec
