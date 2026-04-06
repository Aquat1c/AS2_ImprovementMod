#include "ack_tracker.h"
#include <stdio.h>
#include <string.h>

namespace AckTrackerTests {

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

static void TestBasicSequence() {
    printf("[AckTracker] Basic sequence... ");
    AckTracker::Init();
    uint16_t channel = 0;

    uint32_t seq1 = AckTracker::NextSendSeq(channel);
    uint32_t seq2 = AckTracker::NextSendSeq(channel);
    
    TEST_ASSERT(seq1 == 1, "First seq should be 1");
    TEST_ASSERT(seq2 == 2, "Second seq should be 2");

    printf("done\n");
}

static void TestRecvAndAckBits() {
    printf("[AckTracker] Recv sequence & Ack Bits... ");
    AckTracker::Init();
    uint16_t channel = 1;

    // Receive packets in order 1, 2, 3
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 1) == true, "Accept 1");
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 2) == true, "Accept 2");
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 3) == true, "Accept 3");

    // Check what we will tell the sender we received
    uint32_t ack = 0, ackBits = 0;
    AckTracker::GetAckState(channel, &ack, &ackBits);
    
    TEST_ASSERT(ack == 3, "Highest contiguous is 3");
    // bit 0 -> seq 2, bit 1 -> seq 1. Both should be 1.
    TEST_ASSERT(ackBits == 3, "Bits for 2 and 1 are set (3)");

    printf("done\n");
}

static void TestRecvOutofOrder() {
    printf("[AckTracker] Recv Out of Order & Dupes... ");
    AckTracker::Init();
    uint16_t channel = 2;

    // Receive 1, then 3 (miss 2)
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 1) == true, "Accept 1");
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 3) == true, "Accept 3");

    uint32_t ack = 0, ackBits = 0;
    AckTracker::GetAckState(channel, &ack, &ackBits);
    
    TEST_ASSERT(ack == 3, "Highest is 3");
    // bit 0 -> seq 2 (missed, 0), bit 1 -> seq 1 (received, 1) -> value 2
    TEST_ASSERT(ackBits == 2, "Bit for 2 is 0, for 1 is 1");

    // Receive 2 (late arrival)
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 2) == true, "Accept late 2");
    AckTracker::GetAckState(channel, &ack, &ackBits);
    TEST_ASSERT(ack == 3, "Highest is still 3");
    TEST_ASSERT(ackBits == 3, "Bit for 2 now filled in (3)");

    // Receive duplicate
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 1) == false, "Reject dupe 1");
    TEST_ASSERT(AckTracker::OnRecvSeq(channel, 2) == false, "Reject dupe 2");

    printf("done\n");
}

static void TestRemoteAckTracking() {
    printf("[AckTracker] Remote Ack Tracking... ");
    AckTracker::Init();
    uint16_t channel = 3;

    // We send packets 1, 2, 3, 4
    AckTracker::NextSendSeq(channel); // 1
    AckTracker::NextSendSeq(channel); // 2
    AckTracker::NextSendSeq(channel); // 3
    AckTracker::NextSendSeq(channel); // 4

    // Remote says: I got up to 4, but missed 3.
    // So ack = 4, ackBits (for 3, 2, 1) = 011 in binary = 3
    AckTracker::OnRecvAck(channel, 4, 6);

    // Verify IsAcked
    TEST_ASSERT(AckTracker::IsAcked(channel, 4) == true, "4 is acked");
    TEST_ASSERT(AckTracker::IsAcked(channel, 3) == false, "3 is NOT acked");
    TEST_ASSERT(AckTracker::IsAcked(channel, 2) == true, "2 is acked");
    TEST_ASSERT(AckTracker::IsAcked(channel, 1) == true, "1 is acked");

    printf("done\n");
}

int RunAll() {
    g_pass = 0;
    g_fail = 0;

    printf("=== AckTracker Edge Case Tests ===\n");

    TestBasicSequence();
    TestRecvAndAckBits();
    TestRecvOutofOrder();
    TestRemoteAckTracking();

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail;
}

}