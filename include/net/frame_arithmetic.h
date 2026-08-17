/* ============================================================================
 * frame_arithmetic.h  (re0.7 M1)
 *
 * Pure, header-only wrap-safe half-open frame arithmetic, ported 1:1 from
 * qoh99_netplay `net/FrameArithmetic.h` (namespace qnet → Net, INV-15).
 * All netplay frontiers are one-past (half-open); never use raw unsigned
 * subtraction to compare frames that may straddle the 32-bit wrap or when one
 * side may be ahead.
 * ==========================================================================*/
#pragma once

#include <stdint.h>

namespace Net {

constexpr uint32_t kHalfSerialSpace = 0x80000000u;

// left is strictly AFTER right (within half the serial space).
constexpr bool frameAfter(uint32_t left, uint32_t right) {
    const uint32_t distance = left - right;
    return distance != 0u && distance < kHalfSerialSpace;
}

constexpr bool frameBefore(uint32_t left, uint32_t right) {
    return frameAfter(right, left);
}

constexpr bool frameAtOrAfter(uint32_t left, uint32_t right) {
    return left == right || frameAfter(left, right);
}

// Number of forward steps from -> to (valid when `to` is at-or-after `from`
// within the half-space; caller guarantees ordering).
constexpr uint32_t forwardDistance(uint32_t from, uint32_t to) {
    return to - from;
}

// Signed lead of `ahead` over `behind`: positive when `ahead` is later, negative
// when earlier, 0 when equal. Wrap-safe within +/- half the serial space.
constexpr int32_t signedLead(uint32_t ahead, uint32_t behind) {
    const uint32_t forward = ahead - behind;
    return forward < kHalfSerialSpace
        ? static_cast<int32_t>(forward)
        : -static_cast<int32_t>(behind - ahead);
}

} // namespace Net
