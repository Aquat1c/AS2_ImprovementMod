#pragma once

#include "as2_constants.h"

#include <stddef.h>
#include <stdint.h>

namespace Rollback {

static constexpr size_t GAME_SNAPSHOT_MAIN_SIZE =
    (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE;
static constexpr size_t GAME_SNAPSHOT_PRE_MATCH_SIZE = PRE_MATCH_GAP_SIZE;
static constexpr size_t GAME_SNAPSHOT_AI_LEARN_SIZE = AI_LEARN_STATICS_SIZE;
static constexpr size_t GAME_SNAPSHOT_INPUT_SIZE = INPUT_BUFFER_SIZE;

// Region roles (re0.7 M4-6 hash-membership audit, plan §2.7.7 / INV-22;
// amended by docs/re0.7/SAVESTATE_AUDIT.md, 2026-08-17).
// Every member is classified SIM (enters the gameplay digest) or
// RENDER/TIMING/CONTROL (saved+restored, but excluded from every digest).
// The full per-region table with rationale lives in docs/re0.7/M4_AUDITS.md.
//
// Digest masks (SAVESTATE_AUDIT F2/F4/F5): three per-entity windows INSIDE
// main_state are render-cadence- or wall-clock-written and therefore hashed
// as gaps (captured+restored, skipped by both digest paths):
//   entity+440..+447        render tint state/timer (sub_4C6B60, F5)
//   entity+1244..+1851      super-background particle scratch (sub_4C47C0, F2)
//   entity+107084..+107095  voice bookkeeping (Audio_IsPlaying-gated, F4)
// The mask table lives in game_snapshot.cpp and is shared by
// GameSnapshot_HashGameplay and GameSnapshot_HashGameplayLive so the two
// stay byte-for-byte identical.
struct GameSnapshot {
    bool     valid;
    int32_t  frame;
    uint32_t checksum;        // legacy CRC32 (main+effect_index) — diagnostics
    uint32_t rng_seed;        // SIM — MSVC LCG state (TLS _getptd()+0x14)

    uint32_t sim_frame;       // SIM — 0x816490 sim/input-read counter
    uint32_t display_frame;   // TIMING — 0x81635C RENDER-LOOP counter (EXCLUDED)
    uint32_t game_mode;       // SIM — mode handler dispatch
    uint32_t substate;        // SIM — sub-state within mode
    uint32_t substate_timer;  // SIM
    uint32_t game_type;       // SIM (session-constant, config-locked)
    uint32_t match_phase_timer; // SIM — intro lock countdown
    uint32_t input_read_idx;  // SIM (aliases sim_frame at 0x816490)
    uint32_t input_write_idx; // SIM
    uint32_t effect_index;    // SIM — Effect_Enqueue write cursor
    // SIM — 0x816494 `Frame_Display` (ADDR_FRAME_DISPLAY): ++ once per SIM
    // tick by Frame_AdvanceDisplay, sim-read every tick (215900 forced-draw
    // check, replay-end check). NOT the render-loop counter 0x81635C held in
    // `display_frame` above — see the naming-trap note in as2_constants.h.
    // SAVESTATE_AUDIT F3.
    uint32_t frame_display;

    // CONTROL — x87/MXCSR control words, captured/restored per slot for
    // float determinism (plan §2.7.7); never hashed (config, not state).
    uint16_t fpu_cw;
    uint16_t _pad_fpu;
    uint32_t mxcsr;

    uint8_t main_state[GAME_SNAPSHOT_MAIN_SIZE];        // SIM — match+entities
                                                        // (minus digest masks)
    uint8_t pre_match_gap[GAME_SNAPSHOT_PRE_MATCH_SIZE];// RENDER (EXCLUDED)
    // SIM — 0x76C5D8..0x76C5E7: AI pattern-learning statics (SAVESTATE_AUDIT
    // F1). They gate rand() consumption, so they are hashed too; the netplay
    // startup handoff zeroes them so peers agree from frame 0.
    uint8_t ai_learn[GAME_SNAPSHOT_AI_LEARN_SIZE];
    uint8_t input_p1[GAME_SNAPSHOT_INPUT_SIZE];         // SIM — input history
    uint8_t input_p2[GAME_SNAPSHOT_INPUT_SIZE];         // SIM
};

// Layout pin: 68-byte scalar prefix (bool+pad, frame, checksum, rng_seed,
// 11 uint32 counters, fpu_cw+pad+mxcsr) followed by the four byte regions.
// Any member addition/removal must update this figure consciously — the
// StateHistory ring allocates 64 of these per session (§2.7.7) and the
// engine2 microbench sizes itself off sizeof(GameSnapshot).
static_assert(sizeof(GameSnapshot) ==
                  68 +
                  GAME_SNAPSHOT_MAIN_SIZE +
                  GAME_SNAPSHOT_PRE_MATCH_SIZE +
                  GAME_SNAPSHOT_AI_LEARN_SIZE +
                  2 * GAME_SNAPSHOT_INPUT_SIZE,
              "GameSnapshot layout changed — update the size pin and re-check "
              "capture/restore/hash membership (SAVESTATE_AUDIT)");

void GameSnapshot_Clear(GameSnapshot* snapshot);
bool GameSnapshot_Capture(GameSnapshot* snapshot, int32_t frame);
bool GameSnapshot_Restore(const GameSnapshot* snapshot);

/// Block64 digest over the SIM-affecting members only (INV-22): palettes are
/// render-only by pipeline design and live outside these regions entirely;
/// display_frame (0x81635C), the pre-match render gap, and the FPU control
/// words are excluded per the M4-6 audit, and the three per-entity digest
/// masks (F2/F4/F5, see struct comment) are skipped inside main_state.
/// This is the `gameplay_hash` fed to the SyncHash exchange and the confirm
/// pipeline.
uint64_t GameSnapshot_HashGameplay(const GameSnapshot* snapshot);

/// M7 (S-4/S-5): the same SIM-membership digest computed directly over the
/// live game memory — no capture copy. Byte-for-byte identical to
/// `GameSnapshot_HashGameplay(capture)` at the same instant, so spectator
/// playback and replay verification can compare against the host/recorder
/// confirmed pre-state hashes cheaply (one read pass, no memcpy). Returns
/// false (and *outHash=0) if the regions fault.
bool GameSnapshot_HashGameplayLive(uint64_t* outHash);

} // namespace Rollback
