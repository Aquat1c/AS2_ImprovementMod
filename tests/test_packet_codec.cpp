/**
 * Edge-case tests for PacketCodec (encode/decode, CRC, truncation, corruption).
 *
 * Build:   cl /EHsc /I../include test_packet_codec.cpp ../src/netplay/packet_codec.cpp
 * Or just compile as part of the mod and call RunPacketCodecTests() from the menu.
 */

#include "packet_codec.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

namespace PacketCodecTests {

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, msg)                                         \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            g_fail++;                                                  \
        } else {                                                       \
            g_pass++;                                                  \
        }                                                              \
    } while (0)

// ============================================================================
// 1. Round-trip: Encode → Decode produces identical header + payload
// ============================================================================

static void TestRoundTrip_Hello() {
    printf("[PacketCodec] RoundTrip Hello... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::Hello, 0, PacketCodec::FLAG_HANDSHAKE);
    hdr.session_id = 0xDEADBEEF12345678ULL;
    hdr.connection_id = 42;
    hdr.seq = 1;

    PacketCodec::HelloPayload hello{};
    hello.build_hash = 0xAABBCCDD;
    hello.gameplay_hash = 0x11223344;
    hello.feature_flags = 1;
    hello.requested_player_slot = 1;
    hello.listen_port = 7500;
    hello.client_nonce = 999;
    hello.nickname_len = 4;
    memcpy(hello.nickname, "Test", 4);

    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int written = PacketCodec::Encode(&hdr, &hello, sizeof(hello), buf, sizeof(buf));
    TEST_ASSERT(written > 0, "Encode should succeed");
    TEST_ASSERT(written == PacketCodec::HEADER_SIZE + (int)sizeof(hello), "Written size");

    PacketCodec::ModNetHeader decoded{};
    const void* payload = nullptr;
    int payloadLen = 0;
    bool ok = PacketCodec::Decode(buf, written, &decoded, &payload, &payloadLen);
    TEST_ASSERT(ok, "Decode should succeed");
    TEST_ASSERT(decoded.magic == PacketCodec::PROTOCOL_MAGIC, "Magic match");
    TEST_ASSERT(decoded.packet_type == (uint16_t)PacketCodec::PacketType::Hello, "Type match");
    TEST_ASSERT(decoded.session_id == 0xDEADBEEF12345678ULL, "Session ID match");
    TEST_ASSERT(decoded.seq == 1, "Seq match");
    TEST_ASSERT(payloadLen == sizeof(hello), "Payload size match");

    const auto* decodedHello = (const PacketCodec::HelloPayload*)payload;
    TEST_ASSERT(decodedHello->build_hash == 0xAABBCCDD, "Build hash match");
    TEST_ASSERT(decodedHello->client_nonce == 999, "Nonce match");
    TEST_ASSERT(memcmp(decodedHello->nickname, "Test", 4) == 0, "Nickname match");

    printf("done\n");
}

// ============================================================================
// 2. Round-trip: Empty payload (Disconnect)
// ============================================================================

static void TestRoundTrip_EmptyPayload() {
    printf("[PacketCodec] RoundTrip EmptyPayload... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::Disconnect);

    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int written = PacketCodec::Encode(&hdr, nullptr, 0, buf, sizeof(buf));
    TEST_ASSERT(written == PacketCodec::HEADER_SIZE, "Header-only packet");

    PacketCodec::ModNetHeader decoded{};
    const void* payload = nullptr;
    int payloadLen = 0;
    bool ok = PacketCodec::Decode(buf, written, &decoded, &payload, &payloadLen);
    TEST_ASSERT(ok, "Decode header-only");
    TEST_ASSERT(payloadLen == 0, "No payload");

    printf("done\n");
}

// ============================================================================
// 3. Truncated packet (too short for header)
// ============================================================================

static void TestTruncated_TooShort() {
    printf("[PacketCodec] Truncated (too short)... ");

    uint8_t buf[10] = {};
    PacketCodec::ModNetHeader decoded{};
    const void* payload = nullptr;
    int payloadLen = 0;
    bool ok = PacketCodec::Decode(buf, 10, &decoded, &payload, &payloadLen);
    TEST_ASSERT(!ok, "Should reject buffer shorter than header");

    printf("done\n");
}

// ============================================================================
// 4. Truncated packet (header says payload but buffer is short)
// ============================================================================

static void TestTruncated_PayloadCutOff() {
    printf("[PacketCodec] Truncated (payload cut off)... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::Ping);

    PacketCodec::PingPayload ping{};
    ping.ping_id = 1;
    ping.local_time_ms = 12345;

    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int written = PacketCodec::Encode(&hdr, &ping, sizeof(ping), buf, sizeof(buf));
    TEST_ASSERT(written > 0, "Encode ping");

    // Truncate: only give header + half payload
    int truncated = PacketCodec::HEADER_SIZE + (int)sizeof(ping) / 2;
    PacketCodec::ModNetHeader decoded{};
    const void* payload = nullptr;
    int payloadLen = 0;
    bool ok = PacketCodec::Decode(buf, truncated, &decoded, &payload, &payloadLen);
    TEST_ASSERT(!ok, "Should reject truncated payload");

    printf("done\n");
}

// ============================================================================
// 5. Corrupted magic
// ============================================================================

static void TestCorrupted_Magic() {
    printf("[PacketCodec] Corrupted magic... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::Ping);

    PacketCodec::PingPayload ping{};
    ping.ping_id = 2;

    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int written = PacketCodec::Encode(&hdr, &ping, sizeof(ping), buf, sizeof(buf));
    TEST_ASSERT(written > 0, "Encode");

    // Corrupt magic bytes
    buf[0] ^= 0xFF;

    PacketCodec::ModNetHeader decoded{};
    const void* payload = nullptr;
    int payloadLen = 0;
    bool ok = PacketCodec::Decode(buf, written, &decoded, &payload, &payloadLen);
    TEST_ASSERT(!ok, "Should reject corrupted magic");

    printf("done\n");
}

// ============================================================================
// 6. Corrupted header CRC
// ============================================================================

static void TestCorrupted_HeaderCRC() {
    printf("[PacketCodec] Corrupted header CRC... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::Pong);

    PacketCodec::PongPayload pong{};
    pong.ping_id = 3;
    pong.echoed_time_ms = 999;

    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int written = PacketCodec::Encode(&hdr, &pong, sizeof(pong), buf, sizeof(buf));
    TEST_ASSERT(written > 0, "Encode pong");

    // Flip a bit in the seq field (inside header, after CRC was computed)
    buf[20] ^= 0x01;

    PacketCodec::ModNetHeader decoded{};
    const void* payload = nullptr;
    int payloadLen = 0;
    bool ok = PacketCodec::Decode(buf, written, &decoded, &payload, &payloadLen);
    TEST_ASSERT(!ok, "Should reject corrupted header CRC");

    printf("done\n");
}

// ============================================================================
// 7. Corrupted payload CRC
// ============================================================================

static void TestCorrupted_PayloadCRC() {
    printf("[PacketCodec] Corrupted payload CRC... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::Hello, 0, PacketCodec::FLAG_HANDSHAKE);

    PacketCodec::HelloPayload hello{};
    hello.build_hash = 0x12345678;

    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int written = PacketCodec::Encode(&hdr, &hello, sizeof(hello), buf, sizeof(buf));
    TEST_ASSERT(written > 0, "Encode");

    // Corrupt one byte deep in the payload
    buf[PacketCodec::HEADER_SIZE + 2] ^= 0xFF;

    PacketCodec::ModNetHeader decoded{};
    const void* payload = nullptr;
    int payloadLen = 0;
    bool ok = PacketCodec::Decode(buf, written, &decoded, &payload, &payloadLen);
    TEST_ASSERT(!ok, "Should reject corrupted payload CRC");

    printf("done\n");
}

// ============================================================================
// 8. Max-size payload (fill to MTU)
// ============================================================================

static void TestMaxPayload() {
    printf("[PacketCodec] Max payload size... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::GekkoData);

    uint8_t payload[PacketCodec::MAX_PAYLOAD_SIZE];
    for (int i = 0; i < PacketCodec::MAX_PAYLOAD_SIZE; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int written = PacketCodec::Encode(&hdr, payload, PacketCodec::MAX_PAYLOAD_SIZE,
                                       buf, sizeof(buf));
    TEST_ASSERT(written == PacketCodec::MAX_PACKET_SIZE, "Should fill exactly MTU");

    PacketCodec::ModNetHeader decoded{};
    const void* outPayload = nullptr;
    int outLen = 0;
    bool ok = PacketCodec::Decode(buf, written, &decoded, &outPayload, &outLen);
    TEST_ASSERT(ok, "Decode max payload");
    TEST_ASSERT(outLen == PacketCodec::MAX_PAYLOAD_SIZE, "Payload len match");
    TEST_ASSERT(memcmp(outPayload, payload, PacketCodec::MAX_PAYLOAD_SIZE) == 0,
                "Payload content match");

    printf("done\n");
}

// ============================================================================
// 9. Oversized payload (exceeds MTU)
// ============================================================================

static void TestOversizedPayload() {
    printf("[PacketCodec] Oversized payload... ");

    PacketCodec::ModNetHeader hdr{};
    PacketCodec::InitHeader(&hdr, PacketCodec::PacketType::GekkoData);

    uint8_t payload[PacketCodec::MAX_PAYLOAD_SIZE + 100];
    memset(payload, 0xAA, sizeof(payload));

    uint8_t buf[2000];
    int written = PacketCodec::Encode(&hdr, payload, sizeof(payload), buf, sizeof(buf));
    // Should either fail (return 0) or clamp — either way, decode shouldn't accept garbage
    TEST_ASSERT(written == 0 || written <= PacketCodec::MAX_PACKET_SIZE,
                "Should reject or clamp oversized payload");

    printf("done\n");
}

// ============================================================================
// 10. CRC determinism
// ============================================================================

static void TestCrcDeterminism() {
    printf("[PacketCodec] CRC determinism... ");

    const char* data = "Alice Senki 2 rollback test data";
    int len = (int)strlen(data);

    uint16_t c16a = PacketCodec::Crc16(data, len);
    uint16_t c16b = PacketCodec::Crc16(data, len);
    TEST_ASSERT(c16a == c16b, "CRC16 deterministic");
    TEST_ASSERT(c16a != 0, "CRC16 non-zero for non-empty data");

    uint32_t c32a = PacketCodec::Crc32(data, len);
    uint32_t c32b = PacketCodec::Crc32(data, len);
    TEST_ASSERT(c32a == c32b, "CRC32 deterministic");
    TEST_ASSERT(c32a != 0, "CRC32 non-zero for non-empty data");

    // Different data should produce different CRC
    const char* data2 = "Alice Senki 2 rollback test datb";
    uint32_t c32c = PacketCodec::Crc32(data2, len);
    TEST_ASSERT(c32a != c32c, "CRC32 differs for different data");

    printf("done\n");
}

// ============================================================================
// 11. All packet types round-trip
// ============================================================================

static void TestAllPacketTypes() {
    printf("[PacketCodec] All packet types... ");

    PacketCodec::PacketType types[] = {
        PacketCodec::PacketType::Hello,
        PacketCodec::PacketType::HelloAck,
        PacketCodec::PacketType::Ready,
        PacketCodec::PacketType::Ping,
        PacketCodec::PacketType::Pong,
        PacketCodec::PacketType::StateDigest,
        PacketCodec::PacketType::GekkoData,
        PacketCodec::PacketType::Disconnect,
        PacketCodec::PacketType::ErrorNotice,
    };

    for (auto type : types) {
        PacketCodec::ModNetHeader hdr{};
        PacketCodec::InitHeader(&hdr, type);

        uint8_t dummy[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
        int written = PacketCodec::Encode(&hdr, dummy, sizeof(dummy), buf, sizeof(buf));
        TEST_ASSERT(written > 0, "Encode type");

        PacketCodec::ModNetHeader decoded{};
        const void* payload = nullptr;
        int payloadLen = 0;
        bool ok = PacketCodec::Decode(buf, written, &decoded, &payload, &payloadLen);
        TEST_ASSERT(ok, "Decode type");
        TEST_ASSERT(decoded.packet_type == (uint16_t)type, "Type preserved");
    }

    printf("done\n");
}

// ============================================================================
// Public entry point
// ============================================================================

void RunAll() {
    g_pass = 0;
    g_fail = 0;
    printf("=== PacketCodec Edge Case Tests ===\n");

    TestRoundTrip_Hello();
    TestRoundTrip_EmptyPayload();
    TestTruncated_TooShort();
    TestTruncated_PayloadCutOff();
    TestCorrupted_Magic();
    TestCorrupted_HeaderCRC();
    TestCorrupted_PayloadCRC();
    TestMaxPayload();
    TestOversizedPayload();
    TestCrcDeterminism();
    TestAllPacketTypes();

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
}

} // namespace PacketCodecTests
