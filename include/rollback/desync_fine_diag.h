/**
 * Alice Senki 2 - Fine-grained desync localization ring (re0.7, F7d hunt)
 *
 * SAVESTATE_AUDIT §9 F7d next-step diagnostics: the coarse per-region CRCs
 * in the desync dump are computed at the DETECTION instant (live memory,
 * post-divergence) and cannot localize where the digest first diverged.
 * This module records, AT THE CONFIRM SEAM (the same instant as the
 * pre-tick StateHistory capture that produces the exchanged gameplay
 * hash), for each of the last FINE_DIAG_RING_CAPACITY frames:
 *
 *   - the raw SimHeader scalar values (rng, sim_frame, mode, substate,
 *     substate_timer, game_type, match_phase_timer, read/write idx,
 *     effect_index, frame_display) — the exact values the digest folds,
 *   - a per-64-byte-window 32-bit digest over the whole main_state
 *     (4043 windows over 0x76C5F8..0x7AB880),
 *   - a RAW byte image of main[0, 0x3630) = match_header + match_context
 *     + effect_array (the F7d candidate ranges) for byte-exact diffing,
 *   - ai_learn and input-span stable-tail ([76,208)) digests.
 *
 * Dumped (machine-readable FINE* lines) into the desync dump on
 * ConfirmedDesync from BOTH sides; tools/compare_desync_dumps.py diffs the
 * two files and names the exact divergent windows/bytes at the FIRST
 * divergent confirmed frame — no more fingerprint inference.
 *
 * Storage: ~1.9 MB static ring. Cost per seam record: one 13.9 KB memcpy +
 * one Block64 pass over 253 KB (~100 µs — same order as the digest hash
 * itself). Enabled by default during netplay; AS2_FINE_DIAG=0 disables.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>

namespace Rollback {

/// Matches DESYNC_DIAG_RING_CAPACITY so the fine window always spans the
/// coarse ring's frames (and thus the failing exchange).
constexpr uint32_t FINE_DIAG_RING_CAPACITY = 64;
constexpr size_t   FINE_DIAG_WINDOW = 64;   // bytes per CRC window

void FineDiag_Reset();

/// Record the live pre-tick state for `canonical_frame`. Call at the
/// confirm seam immediately after the StateHistory capture for the frame
/// (replays re-record — the surviving entry is the confirmed capture).
void FineDiag_RecordPreTick(uint32_t canonical_frame);

/// Write the machine-readable FINE* lines (all valid ring entries,
/// ascending frame order). Deterministic given the ring contents.
void FineDiag_WriteDump(FILE* f);

} // namespace Rollback
