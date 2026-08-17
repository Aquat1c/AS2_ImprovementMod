// re0.7 M1 — unit tests for net/frame_arithmetic.h (wrap-safe half-open
// helpers, ported 1:1 from QOH99). Every frontier comparison the engine2
// core will make routes through these four functions, so the wrap behavior
// is pinned here explicitly.

#include "net/frame_arithmetic.h"

#include <cstdint>
#include <cstdio>

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

constexpr uint32_t kMax = 0xFFFFFFFFu;

void TestFrameAfterBasics() {
    TEST_CHECK(!Net::frameAfter(0, 0), "a frame is not after itself");
    TEST_CHECK(Net::frameAfter(1, 0), "1 is after 0");
    TEST_CHECK(!Net::frameAfter(0, 1), "0 is not after 1");
    TEST_CHECK(Net::frameAfter(1000000, 999999), "adjacent large frames order correctly");
    TEST_CHECK(!Net::frameAfter(999999, 1000000), "reverse of adjacent large frames");
}

void TestFrameAfterWrap() {
    TEST_CHECK(Net::frameAfter(0, kMax), "0 is after 0xFFFFFFFF across the wrap");
    TEST_CHECK(!Net::frameAfter(kMax, 0), "0xFFFFFFFF is not after 0");
    TEST_CHECK(Net::frameAfter(5, kMax - 5), "small frame is after pre-wrap frame");
    TEST_CHECK(!Net::frameAfter(kMax - 5, 5), "pre-wrap frame is not after post-wrap frame");
}

void TestFrameAfterHalfSpaceBoundary() {
    // Distance of exactly half the space is NOT "after" (tie region).
    TEST_CHECK(!Net::frameAfter(Net::kHalfSerialSpace, 0),
        "exactly half-space distance is not after");
    TEST_CHECK(Net::frameAfter(Net::kHalfSerialSpace - 1, 0),
        "half-space minus one is after");
    // From the other side, half-space distance is also not after.
    TEST_CHECK(!Net::frameAfter(0, Net::kHalfSerialSpace),
        "half-space distance is not after in either direction");
}

void TestFrameBeforeAndAtOrAfter() {
    TEST_CHECK(Net::frameBefore(0, 1), "0 is before 1");
    TEST_CHECK(!Net::frameBefore(1, 0), "1 is not before 0");
    TEST_CHECK(Net::frameBefore(kMax, 0), "0xFFFFFFFF is before 0 across the wrap");

    TEST_CHECK(Net::frameAtOrAfter(7, 7), "a frame is at-or-after itself");
    TEST_CHECK(Net::frameAtOrAfter(8, 7), "later frame is at-or-after");
    TEST_CHECK(!Net::frameAtOrAfter(6, 7), "earlier frame is not at-or-after");
    TEST_CHECK(Net::frameAtOrAfter(0, kMax), "wrap successor is at-or-after");
}

void TestForwardDistance() {
    TEST_CHECK(Net::forwardDistance(0, 0) == 0, "zero distance");
    TEST_CHECK(Net::forwardDistance(10, 25) == 15, "simple forward distance");
    TEST_CHECK(Net::forwardDistance(kMax, 0) == 1, "forward distance across the wrap");
    TEST_CHECK(Net::forwardDistance(kMax - 2, 3) == 6, "forward distance spanning the wrap");
}

void TestSignedLead() {
    TEST_CHECK(Net::signedLead(0, 0) == 0, "equal frames have zero lead");
    TEST_CHECK(Net::signedLead(10, 4) == 6, "positive lead");
    TEST_CHECK(Net::signedLead(4, 10) == -6, "negative lead");
    TEST_CHECK(Net::signedLead(0, kMax) == 1, "positive lead across the wrap");
    TEST_CHECK(Net::signedLead(kMax, 0) == -1, "negative lead across the wrap");
    TEST_CHECK(Net::signedLead(2, kMax - 2) == 5, "positive lead spanning the wrap");
    TEST_CHECK(Net::signedLead(kMax - 2, 2) == -5, "negative lead spanning the wrap");
}

void TestConstexprUsable() {
    // The helpers must stay constexpr — engine2 uses them in constant contexts.
    static_assert(Net::frameAfter(1, 0), "frameAfter constexpr");
    static_assert(Net::frameBefore(0, 1), "frameBefore constexpr");
    static_assert(Net::frameAtOrAfter(0, 0), "frameAtOrAfter constexpr");
    static_assert(Net::forwardDistance(0xFFFFFFFFu, 0) == 1, "forwardDistance constexpr");
    static_assert(Net::signedLead(0, 0xFFFFFFFFu) == 1, "signedLead constexpr");
    TEST_CHECK(true, "constexpr context compiles");
}

} // namespace

int main() {
    std::printf("Running frame arithmetic tests...\n");

    TestFrameAfterBasics();
    TestFrameAfterWrap();
    TestFrameAfterHalfSpaceBoundary();
    TestFrameBeforeAndAtOrAfter();
    TestForwardDistance();
    TestSignedLead();
    TestConstexprUsable();

    std::printf("Frame arithmetic tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
