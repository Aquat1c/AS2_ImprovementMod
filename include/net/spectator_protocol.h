/**
 * Alice Senki 2 - Spectator Sidecar Protocol
 *
 * Dedicated protocol for spectator transport. Spectators are not rollback
 * peers; they consume archived inputs and sideband match metadata.
 */

#pragma once

#include "net/locked_match_config.h"
#include "net/protocol.h"

#include <stddef.h>
#include <stdint.h>

namespace Net::Spectator {

constexpr uint16_t PROTOCOL_VERSION = 6;
constexpr int MAX_PACKET_SIZE = 1200;
constexpr int MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - 2;
constexpr int MAX_FRAME_BATCH = 32;
constexpr uint16_t LAN_DISCOVERY_PORT = 10702;
constexpr uint32_t LAN_DISCOVERY_MAGIC = 0x44325341; // "AS2D"
constexpr uint16_t LAN_DISCOVERY_VERSION = 3;

constexpr uint8_t CHANNEL_CONTROL = 0;
constexpr uint8_t CHANNEL_STREAM = 1;
constexpr uint8_t NUM_CHANNELS = 2;

enum class PacketType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    Redirect = 3,
    MatchState = 4,
    FrameBatch = 5,
    PaletteState = 6,
    PaletteData = 7,
    Heartbeat = 8,
    Disconnect = 9,
    ClientStatus = 10,
};

enum MatchStateKind : uint8_t {
    MATCH_STATE_IDLE = 0,
    MATCH_STATE_ACTIVE = 1,
    MATCH_STATE_ENDED = 2,
};

constexpr uint8_t HELLO_FLAG_ACCEPT_REDIRECT = 1 << 0;

constexpr uint8_t FRAME_FLAG_CONFIRMED = 1 << 0;
constexpr uint8_t FRAME_FLAG_ROLLBACK_REWRITE = 1 << 1;

constexpr uint8_t CLIENT_STATUS_FLAG_FAST_FORWARD = 1 << 0;
constexpr uint8_t CLIENT_STATUS_FLAG_HARD_SYNC = 1 << 1;

#pragma pack(push, 1)

struct HelloPayload {
    uint16_t protocol_version;
    uint16_t client_listen_port;
    uint32_t requested_match_id;
    uint8_t flags;
    uint8_t _pad[3];
    char nickname[64];
};

struct HelloAckPayload {
    uint16_t protocol_version;
    uint16_t server_listen_port;
    uint16_t session_listen_port;
    uint16_t _pad0;
    uint32_t match_id;
    uint32_t match_ordinal;
    uint8_t match_state;
    uint8_t _pad[3];
};

struct RedirectPayload {
    char endpoint[96];
};

struct MatchStatePayload {
    uint32_t match_id;
    uint32_t match_ordinal;
    uint32_t config_crc;
    uint8_t match_state;
    uint8_t _pad0[3];
    int32_t archive_start_rb_frame;
    int32_t confirmed_rb_frame;
    int32_t live_rb_frame;
    uint16_t p1_wins;
    uint16_t p2_wins;
    uint16_t draws;
    uint16_t completed_matches;
    uint16_t session_listen_port;
    uint16_t _pad1;
    LockedMatchConfig config;
    char p1_name[64];
    char p2_name[64];
};

struct FrameRecord {
    int32_t rb_frame;
    int32_t game_abs_frame;
    uint16_t p1_input;
    uint16_t p2_input;
    uint8_t flags;
    uint8_t _pad[3];
};

struct FrameBatchPayload {
    uint32_t match_id;
    uint32_t match_ordinal;
    uint32_t config_crc;
    uint32_t session_seed;
    int32_t archive_start_rb_frame;
    int32_t confirmed_rb_frame;
    int32_t live_rb_frame;
    uint16_t record_count;
    uint16_t _pad;
    FrameRecord records[MAX_FRAME_BATCH];
};

struct PalettePlayerState {
    uint8_t character_id;
    uint8_t base_palette;
    uint8_t flags;
    uint8_t has_custom_data;
    uint16_t payload_size;
    uint16_t _pad;
    uint32_t payload_crc;
};

struct PaletteStatePayload {
    uint32_t match_id;
    uint32_t match_ordinal;
    uint32_t config_crc;
    uint32_t session_seed;
    uint32_t palette_epoch;
    PalettePlayerState player[2];
};

struct PaletteDataPayload {
    uint32_t match_id;
    uint32_t match_ordinal;
    uint32_t config_crc;
    uint32_t session_seed;
    uint32_t palette_epoch;
    uint8_t  game_slot;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  _pad0;
    uint32_t payload_crc;
    uint16_t payload_size;
    uint16_t _pad1;
    uint8_t  payload[NETPLAY_PALETTE_BANK_SIZE];
};

struct HeartbeatPayload {
    uint32_t match_id;
    uint32_t match_ordinal;
    uint32_t config_crc;
    uint32_t session_seed;
    int32_t confirmed_rb_frame;
    int32_t live_rb_frame;
    uint16_t session_listen_port;
    uint8_t match_state;
    uint8_t _pad;
};

struct ClientStatusPayload {
    uint32_t match_id;
    uint32_t match_ordinal;
    int32_t playback_rb_frame;
    int32_t buffered_frame_count;
    uint8_t flags;
    uint8_t _pad[3];
};

struct DisconnectPayload {
    uint16_t reason_code;
    char message[64];
};

struct DiscoveryQueryPayload {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad;
    uint32_t nonce;
};

struct DiscoveryResponsePayload {
    uint32_t magic;
    uint16_t version;
    uint16_t spectator_port;
    uint16_t session_listen_port;
    uint32_t nonce;
    uint32_t match_id;
    uint8_t match_state;
    uint8_t connected_spectators;
    uint8_t _pad[2];
    char host_nickname[64];
    char p1_name[64];
    char p2_name[64];
};

#pragma pack(pop)

const char* PacketTypeName(PacketType type);

static_assert(sizeof(PacketType) + sizeof(PaletteDataPayload) <= MAX_PACKET_SIZE,
    "Spectator PaletteDataPayload must fit inside one packet");

inline bool ValidatePacketSize(const void* data, size_t length) {
    return data != nullptr && length >= sizeof(PacketType);
}

inline PacketType ReadPacketType(const void* data) {
    return *static_cast<const PacketType*>(data);
}

inline const void* GetPayloadPtr(const void* data) {
    return static_cast<const uint8_t*>(data) + sizeof(PacketType);
}

inline size_t GetPayloadSize(size_t totalLength) {
    return totalLength > sizeof(PacketType) ? totalLength - sizeof(PacketType) : 0;
}

} // namespace Net::Spectator