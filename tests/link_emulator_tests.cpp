// Link emulator tests.
//
// The emulator exists to make high-RTT claims testable, so it has to be
// trustworthy itself: if it silently reordered, dropped reliable traffic, or
// released early, every "works at 150 ms" result built on it would be
// worthless. These pin the properties the live matrix depends on.

#include "net/link_emulator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdarg>

// Logging stubs: the emulator logs what it armed, which the offline harness
// does not need (LoadConfig itself is exercised live, not here).
namespace Rollback {
void NetplayLog_Write(const char*, int, const char*, ...) {}
}
extern "C" void LogWindow_Log(int, const char*, ...) {}

using namespace Net;

namespace {

int g_checks = 0;
int g_failures = 0;

#define TEST_CHECK(cond, msg)                                             \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
        }                                                                 \
    } while (0)

struct Payload {
    uint32_t id;
    uint8_t  filler[64];
};

LinkEmulatorConfig MakeCfg(uint32_t latency, uint32_t jitter, uint32_t loss) {
    LinkEmulatorConfig c{};
    c.one_way_latency_ms = latency;
    c.jitter_ms = jitter;
    c.loss_percent = loss;
    c.seed = 0x1234567u;
    return c;
}

// A packet must not be released before its latency has elapsed, and must be
// released once it has. "Delivered late" and "delivered instantly" are both
// failures — the second is how a fake simulator passes every test.
void TestLatencyIsActuallyApplied() {
    LinkEmulator_Configure(MakeCfg(75, 0, 0));
    TEST_CHECK(LinkEmulator_IsActive(), "a configured emulator reports active");

    Payload in{42, {}};
    TEST_CHECK(LinkEmulator_Submit(&in, sizeof(in), true, 1000), "submit accepted");

    Payload out{};
    TEST_CHECK(!LinkEmulator_PopDue(&out, sizeof(out), 1000),
               "nothing is due at submit time");
    TEST_CHECK(!LinkEmulator_PopDue(&out, sizeof(out), 1074),
               "nothing is due one millisecond early");
    TEST_CHECK(LinkEmulator_PopDue(&out, sizeof(out), 1075),
               "released exactly when the one-way latency has elapsed");
    TEST_CHECK(out.id == 42, "payload survives the queue intact");
    TEST_CHECK(!LinkEmulator_PopDue(&out, sizeof(out), 2000),
               "released exactly once");
}

// Jitter may add delay. It may never reorder: we sit above ENet's reliability
// layer, and the session layer is entitled to assume a reliable channel
// arrives in order.
void TestJitterNeverReorders() {
    LinkEmulator_Configure(MakeCfg(50, 40, 0));

    for (uint32_t i = 0; i < 64; ++i) {
        Payload p{i, {}};
        TEST_CHECK(LinkEmulator_Submit(&p, sizeof(p), true, 1000 + i), "submit");
    }

    uint32_t expected = 0;
    bool ordered = true;
    uint32_t lastRelease = 0;
    for (uint32_t t = 1000; t <= 1400 && expected < 64; ++t) {
        Payload out{};
        while (LinkEmulator_PopDue(&out, sizeof(out), t)) {
            if (out.id != expected) ordered = false;
            ++expected;
            lastRelease = t;
        }
    }
    TEST_CHECK(ordered, "jittered packets are released in submit order");
    TEST_CHECK(expected == 64, "every jittered packet is eventually released");
    TEST_CHECK(lastRelease >= 1050, "release respects the base latency");
}

// Dropping a reliable packet after ENet has acknowledged it to the sender is
// permanent — nothing upstream will retransmit. Loss must apply to unreliable
// traffic only.
void TestLossSparesReliableTraffic() {
    LinkEmulator_Configure(MakeCfg(10, 0, 100));   // drop everything droppable

    Payload p{7, {}};
    TEST_CHECK(LinkEmulator_Submit(&p, sizeof(p), true, 500),
               "reliable packets are never dropped, even at 100% loss");
    TEST_CHECK(!LinkEmulator_Submit(&p, sizeof(p), false, 500),
               "unreliable packets are dropped at 100% loss");

    LinkEmulatorStats st{};
    LinkEmulator_GetStats(&st);
    TEST_CHECK(st.total_dropped == 1, "exactly the unreliable packet was dropped");

    // And at 0% nothing is dropped at all.
    LinkEmulator_Configure(MakeCfg(10, 0, 0));
    int accepted = 0;
    for (int i = 0; i < 200; ++i) {
        if (LinkEmulator_Submit(&p, sizeof(p), false, 500)) ++accepted;
    }
    TEST_CHECK(accepted == 200, "zero loss drops nothing");
}

// An unconfigured emulator must be completely inert: production must not pay
// for it, and must never have packets held.
void TestDisabledIsInert() {
    LinkEmulator_Configure(MakeCfg(0, 0, 0));
    TEST_CHECK(!LinkEmulator_IsActive(),
               "an unconfigured emulator is inactive and bypassed entirely");
}

// A pathological configuration must degrade by releasing early and SAYING SO,
// never by growing without bound or silently discarding.
void TestOverflowIsCountedNotHidden() {
    LinkEmulator_Configure(MakeCfg(5000, 0, 0));
    Payload p{1, {}};
    for (int i = 0; i < 400; ++i) {
        LinkEmulator_Submit(&p, sizeof(p), true, 1000);
    }
    LinkEmulatorStats st{};
    LinkEmulator_GetStats(&st);
    TEST_CHECK(st.overflow_forced > 0,
               "queue overflow is reported, so an overflowing run is never read as clean");
    TEST_CHECK(st.peak_queued <= 256, "the queue stays bounded");
}

// Round trip: both peers emulate their own inbound direction, so a shared
// setting of N yields ~2N of round trip. This is the arithmetic the live
// matrix relies on when it compares configured latency against measured RTT.
void TestRoundTripArithmetic() {
    LinkEmulator_Configure(MakeCfg(75, 0, 0));

    // Peer A -> B costs 75 ms inbound at B; B's reply costs 75 ms at A.
    const uint32_t sendAt = 1000;
    Payload probe{1, {}};
    LinkEmulator_Submit(&probe, sizeof(probe), true, sendAt);
    uint32_t arrivedAtB = 0;
    Payload out{};
    for (uint32_t t = sendAt; t <= sendAt + 200; ++t) {
        if (LinkEmulator_PopDue(&out, sizeof(out), t)) { arrivedAtB = t; break; }
    }
    TEST_CHECK(arrivedAtB == sendAt + 75, "one-way is the configured latency");

    Payload reply{2, {}};
    LinkEmulator_Submit(&reply, sizeof(reply), true, arrivedAtB);
    uint32_t arrivedBackAtA = 0;
    for (uint32_t t = arrivedAtB; t <= arrivedAtB + 200; ++t) {
        if (LinkEmulator_PopDue(&out, sizeof(out), t)) { arrivedBackAtA = t; break; }
    }
    TEST_CHECK(arrivedBackAtA - sendAt == 150,
               "round trip is twice the configured one-way latency");
}

} // namespace

int main() {
    std::printf("Running link emulator tests...\n");

    TestLatencyIsActuallyApplied();
    TestJitterNeverReorders();
    TestLossSparesReliableTraffic();
    TestDisabledIsInert();
    TestOverflowIsCountedNotHidden();
    TestRoundTripArithmetic();

    std::printf("link_emulator_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
