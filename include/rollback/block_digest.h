/**
 * Alice Senki 2 - Block64 state digest (re0.7 M4, master plan §2.7.7)
 *
 * 64-bit block digest for savestate/sync hashing. Not cryptographic — a
 * determinism tripwire. Stability matters: both peers run the same build
 * (handshake-verified), so the digest only has to be deterministic across the
 * two processes, not across compilers.
 *
 * WHY IT LOOKS LIKE THIS (2026-08-17). The first version folded 8 bytes per
 * step with `state = (state ^ block) * PRIME` — one 64-bit multiply per block,
 * every step waiting on the previous one. That is a pure latency chain, and
 * this is a 32-bit x86 build where each 64-bit multiply is synthesised from
 * three 32-bit multiplies plus adds. Under per-frame forced depth-30 rollback
 * the adapter hashes ~1800 x 253 KB per second, and hashing — not simulating —
 * became the dominant cost (sim held ~53 fps instead of 60).
 *
 * QOH99 hit exactly this and documents the fix in StateProofDigest.h: four
 * INDEPENDENT 32-bit lanes, so four multiplies are in flight at once and none
 * of them is a synthesised 64-bit operation. Same structure here, adapted to
 * the incremental Update() this codebase needs for digest-mask segmentation.
 *
 * DETERMINISM RULES (from the same reference, and they are not optional):
 *   - unsigned integer arithmetic only (wrap is defined; signed overflow is not)
 *   - memcpy for unaligned loads, never a pointer cast
 *   - no floating point, no CPU feature detection, no intrinsics
 * x86 little-endian is the only target, so loads need no byte swap.
 *
 * SEGMENTATION IS PART OF THE VALUE. Update() folds each call's length and
 * finalises its lanes into the running state, so hashing [A][B] separately is
 * deliberately NOT the same as hashing [AB]. Every path that must agree
 * (GameSnapshot_HashGameplay vs _HashGameplayLive) therefore has to walk the
 * identical mask table — which is already an invariant of those two functions.
 *
 * Pure, header-only: usable from the engine core, the game adapter, and the
 * offline test/microbench targets alike.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace Rollback {

constexpr uint64_t BLOCK64_SEED = 0xcbf29ce484222325ull;   // FNV offset basis
constexpr uint64_t BLOCK64_PRIME = 0x100000001b3ull;       // FNV prime (tail/mix)

// 32-bit lane constants (xxHash32's primes: well-mixed, all odd).
constexpr uint32_t BLOCK64_P1 = 2654435761u;
constexpr uint32_t BLOCK64_P2 = 2246822519u;
constexpr uint32_t BLOCK64_P3 = 3266489917u;
constexpr uint32_t BLOCK64_P4 = 668265263u;
constexpr uint32_t BLOCK64_P5 = 374761393u;

inline uint32_t Block64_Rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t Block64_Read32(const uint8_t* p) {
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

/// Fold `len` bytes into `state`. Four independent 32-bit lanes consume 16
/// bytes per iteration; the tail and the length are mixed in so a run of
/// zeroes and a longer run of zeroes stay distinct.
inline uint64_t Block64_Update(uint64_t state, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    const size_t n = len;

    // Seed the lanes from the incoming state so the result still depends on
    // everything folded before this call.
    const uint32_t s_lo = (uint32_t)state;
    const uint32_t s_hi = (uint32_t)(state >> 32);
    uint32_t a = s_lo + BLOCK64_P1 + BLOCK64_P2;
    uint32_t b = s_hi + BLOCK64_P2;
    uint32_t c = s_lo ^ s_hi;
    uint32_t d = s_hi - BLOCK64_P1;

    while (len >= 16) {
        // Four chains, no data dependency between them: the CPU keeps all
        // four multiplies in flight instead of serialising on one.
        a = Block64_Rotl32(a + Block64_Read32(p +  0) * BLOCK64_P2, 13) * BLOCK64_P1;
        b = Block64_Rotl32(b + Block64_Read32(p +  4) * BLOCK64_P2, 13) * BLOCK64_P1;
        c = Block64_Rotl32(c + Block64_Read32(p +  8) * BLOCK64_P2, 13) * BLOCK64_P1;
        d = Block64_Rotl32(d + Block64_Read32(p + 12) * BLOCK64_P2, 13) * BLOCK64_P1;
        p += 16;
        len -= 16;
    }

    uint32_t h = Block64_Rotl32(a, 1) + Block64_Rotl32(b, 7) +
                 Block64_Rotl32(c, 12) + Block64_Rotl32(d, 18);

    while (len >= 4) {
        h += Block64_Read32(p) * BLOCK64_P3;
        h = Block64_Rotl32(h, 17) * BLOCK64_P4;
        p += 4;
        len -= 4;
    }
    while (len > 0) {
        h += (uint32_t)(*p) * BLOCK64_P5;
        h = Block64_Rotl32(h, 11) * BLOCK64_P1;
        ++p;
        --len;
    }

    // Mix the length in, then avalanche, then widen back to 64 bits carrying
    // the prior state so Update() stays a fold rather than a restart.
    h += (uint32_t)n;
    h ^= h >> 15;
    h *= BLOCK64_P2;
    h ^= h >> 13;
    h *= BLOCK64_P3;
    h ^= h >> 16;

    const uint64_t widened = ((uint64_t)h << 32) | (uint64_t)(h * BLOCK64_P1);
    return (state ^ widened) * BLOCK64_PRIME;
}

inline uint64_t Block64(const void* data, size_t len,
                        uint64_t seed = BLOCK64_SEED) {
    return Block64_Update(seed, data, len);
}

/// Fold a 64-bit digest to 32 bits (both halves contribute).
inline uint32_t Block64_Fold32(uint64_t h) {
    return (uint32_t)(h ^ (h >> 32));
}

/// 32-bit state fingerprint for the per-frame paths that used to run a
/// byte-at-a-time table CRC32 over the whole state. The CRC is a serial
/// dependency chain — one byte, one table lookup, each step waiting on the
/// last — which measured ~400 us per 253 KB pass and ran on EVERY snapshot
/// capture and every visible frame. Same reason the digest above is four-lane;
/// see qoh99's StateProofDigest.h, which hit and fixed the identical problem.
///
/// Not interchangeable with CalcCRC32 values: anything comparing fingerprints
/// must produce them with the same function (both peers run the same build,
/// enforced by the handshake's build hash).
inline uint32_t StateFingerprint32(const void* data, size_t len) {
    return Block64_Fold32(Block64(data, len));
}

} // namespace Rollback
