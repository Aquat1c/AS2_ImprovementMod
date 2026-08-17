/**
 * Alice Senki 2 - Block64 state digest (re0.7 M4, master plan §2.7.7)
 *
 * 64-bit block digest for savestate/sync hashing. The reference backend
 * measured FNV-1a at 13-19% of the 16.5 ms frame budget on this class of
 * 32-bit target; the block digest (8 bytes per multiply) is ~19x faster and
 * is mandated by the plan for anything that hashes per frame.
 *
 * Not cryptographic — a determinism tripwire. Stability matters: both peers
 * run the same build (handshake-verified), so the digest only has to be
 * deterministic across the two processes, not across compilers.
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
constexpr uint64_t BLOCK64_PRIME = 0x100000001b3ull;       // FNV prime

/// Fold `len` bytes into `state` 8 bytes at a time (tail handled bytewise
/// through a zero-padded block so equal content always hashes equal).
inline uint64_t Block64_Update(uint64_t state, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len >= 8) {
        uint64_t block;
        memcpy(&block, p, 8);
        state = (state ^ block) * BLOCK64_PRIME;
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t block = 0;
        memcpy(&block, p, len);
        state = (state ^ block) * BLOCK64_PRIME;
        // Fold the tail length so "AB" + "\0" cannot alias "AB".
        state = (state ^ (uint64_t)len) * BLOCK64_PRIME;
    }
    return state;
}

inline uint64_t Block64(const void* data, size_t len,
                        uint64_t seed = BLOCK64_SEED) {
    return Block64_Update(seed, data, len);
}

} // namespace Rollback
