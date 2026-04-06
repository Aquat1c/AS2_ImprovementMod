#include "reliable_channel.h"
#include <stdio.h>
#include <string.h>

namespace ReliableChannelTests {

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

static void TestQueueAndAck() {
    printf("[ReliableChannel] Queue & Ack removal... ");
    ReliableChannel::Init();
    
    uint16_t channel = 1;
    char payload[] = "Hello World";
    
    // Pass INVALID_SOCKET so we don't actually trigger WinSock sendto
    uint32_t seq1 = ReliableChannel::Send(INVALID_SOCKET, nullptr, 
                                          PacketCodec::PacketType::Hello, channel,
                                          payload, sizeof(payload),
                                          12345, 0);

    uint32_t seq2 = ReliableChannel::Send(INVALID_SOCKET, nullptr, 
                                          PacketCodec::PacketType::Ready, channel,
                                          payload, sizeof(payload),
                                          12345, 0);

    TEST_ASSERT(ReliableChannel::GetPendingCount() == 2, "2 packets pending");

    // Remote ACKs seq1 only
    ReliableChannel::ProcessAcks(channel, seq1, 0);
    TEST_ASSERT(ReliableChannel::GetPendingCount() == 1, "1 packet pending after ack");

    // Remote ACKs seq2 via bitfield mapping (highest is seq2, bits mask off seq1 as dup)
    ReliableChannel::ProcessAcks(channel, seq2, 1);
    TEST_ASSERT(ReliableChannel::GetPendingCount() == 0, "0 packets pending after bulk ack");

    printf("done\n");
}

static void TestDeduplication() {
    printf("[ReliableChannel] Deduplication... ");
    ReliableChannel::Init();
    
    uint16_t channel = 2;

    // Incoming 1
    TEST_ASSERT(ReliableChannel::AcceptIncoming(channel, 1) == true, "Accept 1");
    // Duplicate 1
    TEST_ASSERT(ReliableChannel::AcceptIncoming(channel, 1) == false, "Reject dup 1");
    
    // Incoming 2
    TEST_ASSERT(ReliableChannel::AcceptIncoming(channel, 2) == true, "Accept 2");
    // Duplicate 2
    TEST_ASSERT(ReliableChannel::AcceptIncoming(channel, 2) == false, "Reject dup 2");

    printf("done\n");
}

int RunAll() {
    g_pass = 0;
    g_fail = 0;

    printf("=== ReliableChannel Edge Case Tests ===\n");
    TestQueueAndAck();
    TestDeduplication();

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail;
}

}