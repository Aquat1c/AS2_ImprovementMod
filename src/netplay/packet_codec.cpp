/**
 * Alice Senki 2 - Packet Codec Implementation
 */

#include "packet_codec.h"
#include "log_window.h"
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace PacketCodec {

// ============================================================================
// CRC-16/CCITT (polynomial 0x1021, init 0xFFFF)
// ============================================================================

static const uint16_t s_crc16Table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x54A5,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x4864, 0x5845, 0x6826, 0x7807, 0x08E0, 0x18C1, 0x28A2, 0x3883,
    0xC96C, 0xD94D, 0xE92E, 0xF90F, 0x89E8, 0x99C9, 0xA9AA, 0xB98B,
    0x5A55, 0x4A74, 0x7A17, 0x6A36, 0x1AD1, 0x0AF0, 0x3A93, 0x2AB2,
    0xDB5D, 0xCB7C, 0xFB1F, 0xEB3E, 0x9BD9, 0x8BF8, 0xBB9B, 0xABBA,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x9798, 0xE77F, 0xF75E, 0xC73D, 0xD71C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD9EC, 0xC9CD, 0xF9AE, 0xE98F, 0x9968, 0x8949, 0xB92A, 0xA90B,
    0x58E4, 0x48C5, 0x78A6, 0x6887, 0x1860, 0x0841, 0x3822, 0x2803,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBF9A, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

uint16_t Crc16(const void* data, int len) {
    const uint8_t* p = (const uint8_t*)data;
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc = (uint16_t)((crc << 8) ^ s_crc16Table[((crc >> 8) ^ p[i]) & 0xFF]);
    }
    return crc;
}

// ============================================================================
// CRC-32 (standard Ethernet/ZIP polynomial 0xEDB88320)
// ============================================================================

static uint32_t s_crc32Table[256];
static bool s_crc32TableReady = false;

static void InitCrc32Table() {
    if (s_crc32TableReady) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        s_crc32Table[i] = c;
    }
    s_crc32TableReady = true;
}

uint32_t Crc32(const void* data, int len) {
    InitCrc32Table();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc = s_crc32Table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// Timestamp
// ============================================================================

uint32_t GetTimestampMs() {
    // Use QueryPerformanceCounter for sub-ms resolution.
    // GetTickCount() has ~15.625ms granularity which inflates RTT on LAN.
    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint32_t)(now.QuadPart * 1000 / s_freq.QuadPart);
}

// ============================================================================
// Header Init
// ============================================================================

void InitHeader(ModNetHeader* hdr, PacketType type, uint16_t channel, uint16_t flags) {
    if (!hdr) return;
    memset(hdr, 0, sizeof(ModNetHeader));
    hdr->magic = PROTOCOL_MAGIC;
    hdr->protocol_version = PROTOCOL_VERSION;
    hdr->header_size = HEADER_SIZE;
    hdr->packet_type = (uint16_t)type;
    hdr->channel = channel;
    hdr->flags = flags;
    hdr->send_timestamp_ms = GetTimestampMs();
}

// ============================================================================
// Encode
// ============================================================================

int Encode(const ModNetHeader* hdr, const void* payload, int payloadLen,
           void* outBuf, int outBufLen) {
    if (!hdr || !outBuf) return 0;
    if (payloadLen < 0) payloadLen = 0;

    int totalSize = (int)sizeof(ModNetHeader) + payloadLen;
    if (totalSize > outBufLen || totalSize > MAX_PACKET_SIZE) {
        LOG_NET_ERROR("[PacketCodec] Encode: packet too large (%d bytes)", totalSize);
        return 0;
    }

    // Copy header, set computed fields
    ModNetHeader finalHdr = *hdr;
    finalHdr.payload_size = (uint16_t)payloadLen;
    finalHdr.send_timestamp_ms = GetTimestampMs();

    // Compute payload CRC
    if (payload && payloadLen > 0) {
        finalHdr.payload_crc32 = Crc32(payload, payloadLen);
    } else {
        finalHdr.payload_crc32 = 0;
    }

    // Compute header CRC (with crc field zeroed)
    finalHdr.header_crc16 = 0;
    finalHdr.header_crc16 = Crc16(&finalHdr, sizeof(ModNetHeader));

    // Write to buffer
    memcpy(outBuf, &finalHdr, sizeof(ModNetHeader));
    if (payload && payloadLen > 0) {
        memcpy((uint8_t*)outBuf + sizeof(ModNetHeader), payload, payloadLen);
    }

    return totalSize;
}

// ============================================================================
// Decode
// ============================================================================

bool Decode(const void* buf, int bufLen,
            ModNetHeader* hdr, const void** outPayload, int* outPayloadLen) {
    if (!buf || !hdr) return false;

    // Need at least a header
    if (bufLen < (int)sizeof(ModNetHeader)) {
        return false;
    }

    // Read header
    memcpy(hdr, buf, sizeof(ModNetHeader));

    // Validate magic
    if (hdr->magic != PROTOCOL_MAGIC) {
        return false;  // Not our packet - silently ignore (could be vanilla)
    }

    // Validate protocol version
    if (hdr->protocol_version != PROTOCOL_VERSION) {
        LOG_NET_WARN("[PacketCodec] Protocol version mismatch: got %u, expected %u",
                 hdr->protocol_version, PROTOCOL_VERSION);
        return false;
    }

    // Validate header size
    if (hdr->header_size != HEADER_SIZE) {
        LOG_NET_WARN("[PacketCodec] Header size mismatch: got %u, expected %u",
                 hdr->header_size, HEADER_SIZE);
        return false;
    }

    // Validate header CRC
    uint16_t receivedCrc = hdr->header_crc16;
    ModNetHeader checkHdr = *hdr;
    checkHdr.header_crc16 = 0;
    uint16_t computedCrc = Crc16(&checkHdr, sizeof(ModNetHeader));
    if (receivedCrc != computedCrc) {
        LOG_NET_WARN("[PacketCodec] Header CRC mismatch: got 0x%04X, computed 0x%04X",
                 receivedCrc, computedCrc);
        return false;
    }

    // Validate payload size
    int payloadLen = hdr->payload_size;
    if (payloadLen < 0 || (int)sizeof(ModNetHeader) + payloadLen > bufLen) {
        LOG_NET_WARN("[PacketCodec] Payload size invalid: %d (buf=%d)", payloadLen, bufLen);
        return false;
    }

    // Validate payload CRC
    const void* payloadPtr = (const uint8_t*)buf + sizeof(ModNetHeader);
    if (payloadLen > 0) {
        uint32_t computedPayloadCrc = Crc32(payloadPtr, payloadLen);
        if (hdr->payload_crc32 != computedPayloadCrc) {
            LOG_NET_WARN("[PacketCodec] Payload CRC mismatch: got 0x%08X, computed 0x%08X",
                     hdr->payload_crc32, computedPayloadCrc);
            return false;
        }
    }

    if (outPayload) *outPayload = (payloadLen > 0) ? payloadPtr : nullptr;
    if (outPayloadLen) *outPayloadLen = payloadLen;
    return true;
}

// ============================================================================
// Helpers
// ============================================================================

const char* GetPacketTypeName(PacketType type) {
    switch (type) {
        case PacketType::Hello:             return "Hello";
        case PacketType::HelloAck:          return "HelloAck";
        case PacketType::Ready:             return "Ready";
        case PacketType::Ping:              return "Ping";
        case PacketType::Pong:              return "Pong";
        case PacketType::StateDigest:       return "StateDigest";
        case PacketType::GekkoData:         return "GekkoData";
        case PacketType::CharSelInput:      return "CharSelInput";
        case PacketType::CharSelEvent:      return "CharSelEvent";
        case PacketType::MatchConfig:       return "MatchConfig";
        case PacketType::MatchConfigAck:    return "MatchConfigAck";
        case PacketType::LoadBarrierReady:  return "LoadBarrierReady";
        case PacketType::BaselineReady:     return "BaselineReady";
        case PacketType::StartGameplay:     return "StartGameplay";
        case PacketType::StartGameplayAck:  return "StartGameplayAck";
        case PacketType::SpectatorHello:    return "SpectatorHello";
        case PacketType::SpectatorAccept:   return "SpectatorAccept";
        case PacketType::SpectatorReject:   return "SpectatorReject";
        case PacketType::SpectatorKeyframe: return "SpectatorKeyframe";
        case PacketType::Disconnect:        return "Disconnect";
        case PacketType::Rematch:           return "Rematch";
        case PacketType::ErrorNotice:       return "ErrorNotice";
        case PacketType::DelayChangeRequest: return "DelayChangeRequest";
        case PacketType::DelayChangeAck:    return "DelayChangeAck";
        default:                            return "Unknown";
    }
}

uint32_t HashMatchConfig(const MatchConfigPayload* config) {
    if (!config) return 0;
    // Hash everything except the config_hash field itself
    MatchConfigPayload tmp = *config;
    tmp.config_hash = 0;
    return Crc32(&tmp, sizeof(tmp));
}

} // namespace PacketCodec
