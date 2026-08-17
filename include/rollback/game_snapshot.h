#pragma once

#include "as2_constants.h"

#include <stddef.h>
#include <stdint.h>

namespace Rollback {

static constexpr size_t GAME_SNAPSHOT_MAIN_SIZE =
    (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE;
static constexpr size_t GAME_SNAPSHOT_PRE_MATCH_SIZE = PRE_MATCH_GAP_SIZE;
static constexpr size_t GAME_SNAPSHOT_INPUT_SIZE = INPUT_BUFFER_SIZE;

// Region roles (re0.7 M4-6 hash-membership audit, plan §2.7.7 / INV-22).
// Every member is classified SIM (enters the gameplay digest) or
// RENDER/TIMING/CONTROL (saved+restored, but excluded from every digest).
// The full per-region table with rationale lives in docs/re0.7/M4_AUDITS.md.
struct GameSnapshot {
    bool     valid;
    int32_t  frame;
    uint32_t checksum;        // legacy CRC32 (main+effect_index) — diagnostics
    uint32_t rng_seed;        // SIM — MSVC LCG state (TLS _getptd()+0x14)

    uint32_t sim_frame;       // SIM — 0x816490 sim/input-read counter
    uint32_t display_frame;   // TIMING — 0x81635C display counter (EXCLUDED)
    uint32_t game_mode;       // SIM — mode handler dispatch
    uint32_t substate;        // SIM — sub-state within mode
    uint32_t substate_timer;  // SIM
    uint32_t game_type;       // SIM (session-constant, config-locked)
    uint32_t match_phase_timer; // SIM — intro lock countdown
    uint32_t input_read_idx;  // SIM (aliases sim_frame at 0x816490)
    uint32_t input_write_idx; // SIM
    uint32_t effect_index;    // SIM — Effect_Enqueue write cursor

    // CONTROL — x87/MXCSR control words, captured/restored per slot for
    // float determinism (plan §2.7.7); never hashed (config, not state).
    uint16_t fpu_cw;
    uint16_t _pad_fpu;
    uint32_t mxcsr;

    uint8_t main_state[GAME_SNAPSHOT_MAIN_SIZE];        // SIM — match+entities
    uint8_t pre_match_gap[GAME_SNAPSHOT_PRE_MATCH_SIZE];// RENDER (EXCLUDED)
    uint8_t input_p1[GAME_SNAPSHOT_INPUT_SIZE];         // SIM — input history
    uint8_t input_p2[GAME_SNAPSHOT_INPUT_SIZE];         // SIM
};

void GameSnapshot_Clear(GameSnapshot* snapshot);
bool GameSnapshot_Capture(GameSnapshot* snapshot, int32_t frame);
bool GameSnapshot_Restore(const GameSnapshot* snapshot);

/// Block64 digest over the SIM-affecting members only (INV-22): palettes are
/// render-only by pipeline design and live outside these regions entirely;
/// display_frame, the pre-match render gap, and the FPU control words are
/// excluded per the M4-6 audit. This is the `gameplay_hash` fed to the
/// SyncHash exchange and the confirm pipeline.
uint64_t GameSnapshot_HashGameplay(const GameSnapshot* snapshot);

/// M7 (S-4/S-5): the same SIM-membership digest computed directly over the
/// live game memory — no capture copy. Byte-for-byte identical to
/// `GameSnapshot_HashGameplay(capture)` at the same instant, so spectator
/// playback and replay verification can compare against the host/recorder
/// confirmed pre-state hashes cheaply (one read pass, no memcpy). Returns
/// false (and *outHash=0) if the regions fault.
bool GameSnapshot_HashGameplayLive(uint64_t* outHash);

} // namespace Rollback
