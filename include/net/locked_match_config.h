/**
 * Alice Senki 2 - Locked Match Configuration
 *
 * Deterministic match config agreed upon by both peers before gameplay starts.
 * This struct represents the fully resolved selections from CharSel/StageSel.
 *
 * The hash is used during the baseline agreement step to verify both peers
 * will enter gameplay with identical configuration.
 */

#pragma once

#include <stdint.h>

namespace Net {

#pragma pack(push, 1)

struct LockedMatchConfig {
    // Character selections
    uint8_t  p1_character;       // Character ID for P1 (grid-table resolved)
    uint8_t  p1_palette;         // P1 palette (0-7)
    uint8_t  p2_character;       // Character ID for P2
    uint8_t  p2_palette;         // P2 palette (0-7)

    // Stage
    uint8_t  stage_id;           // Resolved stage ID
    uint8_t  _pad0;

    // Side assignment: which physical player maps to which network peer
    // host_side == 0 means host is P1, join is P2
    // host_side == 1 means host is P2, join is P1
    uint8_t  host_side;
    uint8_t  _pad1;

    // RNG seed for deterministic match start
    uint32_t rng_seed;

    // Session-level seed (for tiebreakers, side assignment derivation)
    uint32_t session_seed;

    // Match parameters. round_count uses the vanilla zero-based option:
    // 0,1,2 means first to 1,2,3 wins respectively.
    uint8_t  round_count;
    uint8_t  time_limit;         // 0 = infinite, else seconds/10
    uint8_t  _pad2[2];
};

#pragma pack(pop)

static_assert(sizeof(LockedMatchConfig) == 20, "LockedMatchConfig must be 20 bytes");

/// Compute a CRC32 hash of the locked match config for agreement verification.
uint32_t LockedMatchConfig_Hash(const LockedMatchConfig* cfg);

/// Compare two configs for exact equality.
bool LockedMatchConfig_Equal(const LockedMatchConfig* a, const LockedMatchConfig* b);

/// Zero-initialize a config.
void LockedMatchConfig_Clear(LockedMatchConfig* cfg);

} // namespace Net
