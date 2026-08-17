/**
 * Alice Senki 2 - Exact-input lifecycle windows (re0.7 M4-7, plan §2.7.6, INV-25)
 *
 * Pure predicate deciding when the next simulated tick may NOT run on a
 * predicted remote input (the engine answers Stall(LifecycleBoundary) until
 * the remote actual arrives). Start narrow, widen only with decomp evidence
 * (QOH99 §4.4 narrowing method).
 *
 * ── M4-7 decomp audit (evidence, refactored decomp
 *    `decompilation/アリス戦記２.exe - Copy(refactored).c`) ──────────────────
 *
 * 1. Match-end handoff frees resources; round-end does not.
 *    - `Game_ChangeMode` (0x5D2EB0, decomp L266664-266692) is the ONLY mode
 *      transition path and calls `Handle_ReleaseAll()` (asset handle release)
 *      plus substate/timer reset — an irreversible lifetime boundary.
 *    - The mode-8 exit router `sub_4CA210` (0x4CA210, L118402+) reads the
 *      route byte at match+10 and calls `Game_ChangeMode(9,1)` (L118430,
 *      win screen) / (6,1) charsel / (3,1) menu etc. Any tick that can take
 *      this path must be exact-input.
 *    - `Handle_ReleaseAll` call-site census: definition L303277; callers are
 *      `Game_ChangeMode` (L266689), a title/menu handler (L201044) and the
 *      system shutdown path `sub_611EB0` (L301960). ZERO call sites inside
 *      the mode-8 match handler tree (sub_4C8F60, L117463+).
 *    - Round-to-round state: `Match_UpdateRoundState` (0x4C8??? via decl
 *      L856, def L107972-108011) mutates only bytes inside the match region
 *      (result byte match+4, timer word match+6, per-player win counters at
 *      match+41254) — all inside the snapshot main region; nothing is freed
 *      or reloaded. The round-result flow that stays in mode 8 writes
 *      `dword_816390 = 3; dword_816394 = 0;` (substate reset, L118391-118394)
 *      — plain globals, snapshot-covered.
 *    ⇒ Verdict (matches TrialNetplay's finding for this engine family):
 *      round boundaries inside a match are PREDICTED NORMALLY; the only
 *      exact windows inside gameplay are the tick(s) that can execute the
 *      mode-8 exit router (match-end handoff into mode 9 / any mode change).
 *
 * 2. Everything outside mode 8 substate 3 is frontend lockstep by
 *    architecture (§2.7.6 "exact always") — the rollback engine simply is
 *    not the input authority there, so the predicate treats it as exact to
 *    fail safe if it is ever consulted out of regime.
 *
 * The adapter derives `match_exit_pending` from the match region's exit
 * route state (match_lifecycle already tracks the winscreen handoff); the
 * unit tests drive the flag directly.
 *
 * Pure, header-only: no Win32, no game reads — the caller samples state.
 */

#pragma once

#include <stdint.h>

namespace Rollback {

constexpr uint32_t LIFECYCLE_MODE_MATCH = 8;      // MODE_MATCH
constexpr uint32_t LIFECYCLE_SUBSTATE_FIGHT = 3;  // in-match gameplay substate

/// True when the NEXT tick must consume only actual remote inputs (INV-25).
///   game_mode / substate    — sampled game globals (0x81638C / 0x816390)
///   match_exit_pending      — the mode-8 exit router is armed this tick
///                             (match result resolved; next tick can call
///                             Game_ChangeMode via sub_4CA210)
constexpr bool LifecycleWindow_IsExactInputNext(uint32_t game_mode,
                                                uint32_t substate,
                                                bool match_exit_pending) {
    // Outside rollback gameplay: exact by definition (frontend lockstep owns
    // the stream; fail safe if consulted).
    if (game_mode != LIFECYCLE_MODE_MATCH || substate != LIFECYCLE_SUBSTATE_FIGHT) {
        return true;
    }
    // Inside gameplay: only the match-end handoff (Handle_ReleaseAll via
    // Game_ChangeMode) is irreversible. Round boundaries predict normally
    // (M4-7 audit above).
    return match_exit_pending;
}

} // namespace Rollback
