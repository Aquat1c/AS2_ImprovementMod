# SAVESTATE_AUDIT — mode-8 write-set vs GameSnapshot coverage (re0.7)

Date: 2026-08-17. Companion to `M4_AUDITS.md` (M4-6/M4-7) and
`RE07_MASTER_REBUILD_PLAN.md` §2.7.7 (INV-22).

> **Status 2026-08-17 (same-day fix pass): F1–F6 are all FIXED.** Per-finding
> mechanism is recorded inline (`FIXED:` blocks) and summarized in §8, with
> the implementation entry in `IMPLEMENTATION_LOG.md`.

Method: **work from writes.** Every memory range written during a mode-8
substate-3 sim tick was enumerated from the refactored decomp
(`d:\dev\alice_senki\alicesenki2_decomp_refactored.c`; all `L…` numbers below
are lines in that file) and cross-checked against the capture in
`mod/src/rollback/game_snapshot.cpp` / `include/rollback/game_snapshot.h`.
Mod-verified addresses (`as2_constants.h`, `game_state.h`) win over IDA labels
per the decomp-conventions note. Sweep coverage: an automated global-write
scan over decomp lines 1–118700 (addresses 0x401000–0x4CA8xx — the entire
character-handler + match-engine module), plus targeted scans of every
tick-called function living above that range (input dispatcher family
0x5623xx–0x5627xx, `Match_UpdateScoreStats` 0x55BCD0, replay-settings
0x59Bxxx, BGM/audio wrappers 0x5D33xx/0x62Cxxx). The write patterns matched:
direct global assignment, `++/--/+=`, `LOBYTE/BYTE1/…` partial writes,
`memset/qmemcpy` on globals. Constant-data declarations were filtered out.

The tick call graph audited (from `Game_Update_MatchLoop` / sub_4C9B50,
L118085–118325): `Match_ClearPerFrameTempData` (4C3BE0), round
timer/state (4A0D10 / `Match_UpdateRoundState` L107972), the full
`Match_Update*` input/timer chain (0x49Dxxx–0x49Fxxx), `Input_ProcessRawInput`
/ `Input_UpdateCommandStates`, `AI_RecordPattern` (4A05F0) ×2, `AI_Update`
(L108263), command-flag/history helpers, the `Entity_Update*` chain,
`Weather_UpdateScroll` (4C4230), collision resolvers (4A0200-area, 4A5D30 /
4A5EF0 / 4A6030 strike resolvers), `Entity_UpdateSummons` + `HitDef_*`,
`Effect_Update` (4A9330) and `Effect_Spawn` (4A92C0),
`Weather_UpdateParticles` (4A8A10, L114719), `Entity_UpdateAttachedVisuals`
(L116646), hit-detection family (L107159–107970), `Match_UpdateAnnouncer`
(4C3C70), `Entity_UpdateAudio` (4C38F0), `SE_Play` (4C3C00),
`Match_UpdateScoreStats` (55BCD0), `Input_TryGetNextFrame` (5625E0),
`Frame_AdvanceDisplay` (562750), `Frame_AdvanceSimulation` (562760), and the
post-loop render phase (`Weather_Draw`, sub_4C47C0, 4C6260, 4C7F30, 4C3680,
4C7360, 4C68D0, 4C6B60, 4C7810, 4AB0F0, 4C63A0, 4C05B0, 4C8E20, 4C1F90).

Headline result: **the character-handler bulk (0x401000–0x49C000, decomp
lines 1–99200) contains ZERO runtime global writes outside the snapshot
regions** — all entity/action/combat scripting stays inside
`main_state`. The holes are concentrated in the AI-learning subsystem, the
frame-counter aliases, the render phase, and the audio/voice system.

---

## 1. Capture inventory (verified)

| Region | Range | Size | Verification |
|---|---|---|---|
| `main_state` | 0x76C5F8 – 0x7AB880 | 0x3F288 = 259,720 B | Arithmetic from mod constants: header 16 B + context 0x1D20 (0x76C608–0x76E328) + effect array 200×32 (–0x76FC28) + summons 100×272 (–0x776668) + P1/P2 entities 2×0x1A90C (–0x7AB880). All strides confirmed contiguous. **Doc nit: M4_AUDITS.md says "~243 KB"; actual is ~253.6 KB (259,720 B, as savestate.cpp states).** |
| `effect_index` | 0x76C5E8 | 4 B | Spawn cursor, wraps at 200. Covered. |
| `pre_match_gap` | 0x76C5EC–0x76C5F7 | 12 B | Data decls L22723–22727: `byte_76C5EC` (fade/blend), `SE_ChannelIndex` @0x76C5F0, 4 B pad. Covered. |
| per-frame temp | 0x76CCF8, 0x44 B | 68 B | **Exact identity established:** this block IS `SE_ChannelActive[68]` (decl L22935; `SE_Handles[204]` @0x76C9C8 ends at 0x76C9C8+0x330 = 0x76CCF8). One dedup latch byte per SE id (68 SFX loaded by sub_4C3B90, L114192). Cleared by `Match_ClearPerFrameTempData` `memset(match+1792, 0, 0x44)` (L114224) — **memset size matches the array bound exactly; the restore-path re-clear in `GameSnapshot_Restore` uses the correct window.** Next byte 0x76CD3C is scrollX — correctly not cleared. |
| `input_p1/p2` | 0x8E9E62 / 0x8E9F32, 208 B each | 416 B | Stride P2−P1 = 0xD0 = 208 ✓. `Input_ShiftHistoryBuffer` (sub_562350, L203566) touches words 0–70 only — 208 B is a safe superset. |
| scalars | 0x816490 (sim/read idx), 0x816498 (write idx), 0x81635C, 0x81638C/90/94, 0x816410, 0x816370 | — | Confirmed live: `Frame_Simulation`=dword_816490, `Frame_Inputs`=dword_816498 (L203864–203953). |
| RNG seed | TLS `_getptd()+0x14` | 4 B | Single statically-linked CRT LCG. |
| FPU CW / MXCSR | — | — | See §4.6. |

Decomp naming trap confirmed: `Game_Mode_Current` in the decomp is actually
**dword_816410 (game TYPE)** (trailing comment L203924), and `Frame_Display`
is **dword_816494**, not 0x81635C. IDA labels lie; mod constants are right.

---

## 2. MISSING regions (desync-relevant)

### F1 — CRITICAL: AI pattern-learning statics `dword_76C5E0` / `dword_76C5E4` (and CPU-side `dword_76C5D8` / `dword_76C5DC`)

* Address: 0x76C5D8–0x76C5E7, four dwords sitting **immediately below the
  capture start** (capture begins at `effect_index` 0x76C5E8). Not captured,
  not hashed, not reset by round/match setup (`sub_4C92B0` L117658 clears
  only command-history bytes).
* Writers/readers:
  * `AI_RecordPattern` (sub_4A05F0, L101854–102064) — called **twice per sim
    tick, every tick** (L118124–118125), and runs for **human-controlled
    players** (gate at L101890: proceeds when CPU flag `selstruct+172 != 1`).
    Master gate: `AI_PatternModeEnabled` = **byte_8E940D** (the config.dat
    "CPU learning" option, loaded L200417) AND a non-null matchup learning
    block (see F1b).
  * Cross-frame read-back: default case L102018–102037 — `v1 == dword_76C5E4`
    / `v22 != dword_76C5E4` comparisons decide whether **`rand()` is
    consumed** (L102025) and which table slot is written using the possibly
    **stale `dword_76C5E0`** (L102028–102032).
  * `AI_ExecutePattern` (sub_4A0200, L101691–101845): same pattern with
    `dword_76C5D8`/`dword_76C5DC` (L101787–101790: `rand()%100` consumed only
    when the opponent's action id differs from the static) — CPU-controlled
    players only (gate L101723), so netplay-inert but live in VS_CPU.
* Desync test: **FAILS — worst class in the audit.** The statics gate CRT
  `rand()` consumption. The RNG seed IS restored on rollback, but these
  statics are NOT, so a resimulated tick can consume a different number of
  rand() values than the original tick did → the RNG stream diverges from
  the peer's → every downstream consumer (weather particles every frame,
  effect velocities, character logic) diverges → hash mismatch. Worse: the
  statics carry over **across matches within a process**, so two peers enter
  a netplay match with different values and can diverge on the very first
  ticks **without any rollback happening at all**.
* Condition: only when `byte_8E940D != 0` (AI learning enabled). Verified the
  mod nowhere touches 0x8E940D / these statics (source grep: zero hits).

**FIXED (2026-08-17)** — two mechanisms, per the fix-list recommendation:

1. **Session guard** (`match_director.cpp` `AiLearnGuard_Force/Restore`):
   `byte_8E940D` is saved and forced to 0 at the startup handoff
   (`PrepareBaselineForInteractiveRelease` — chosen over
   `RollbackSession_Begin` because the deterministic intro runs sim ticks,
   and therefore `AI_RecordPattern`, BEFORE the engine is armed), restored
   only on SESSION teardown (StopRollbackSession(teardown)/disconnect/
   shutdown — every path that reaches `RollbackSession_End`). Staying forced
   across match boundaries also kills the rematch substate-0 block load at
   its single gate. The guard additionally zeroes the four statics (after
   the baseline restore, which may rewrite them from a pre-guard capture)
   so both peers hash identical values from frame 0.
2. **Belt-and-braces capture+hash**: the 16 bytes at 0x76C5D8 are a new
   dedicated SIM region in `GameSnapshot` (`ai_learn[16]`,
   `ADDR_AI_LEARN_STATICS`) — captured, restored, and hashed by both
   `GameSnapshot_HashGameplay` and `GameSnapshot_HashGameplayLive` (a
   separate region rather than widening `pre_match_gap`, which is
   digest-excluded RENDER — these bytes gate rand(), so they must be SIM).
   Offline savestates/forced-rollback resim now rewind them too.

   Accepted residuals (documented): the FIRST match's substate-0 block load
   precedes the handoff and may leave inert learning blocks allocated (every
   reader/writer is behind the forced-0 gate; recovered by the next offline
   match-end save/free after restore). The options-menu save path
   (L200417-region) could rewrite the byte mid-session but is unreachable
   while netplay owns the frontend.

**F1b — AI learning heap blocks.** `Block` pointer table (decomp symbol
`Block`, decl L23030; .bss immediately after the P2-entity end — bounded
(0x7AB762, 0x7AC160), 484 `void*` = 22×22 matchups) → per-matchup 0x61613-byte
(399,891 B) `calloc` blocks, loaded from `<A>vs<B>` learning files by
sub_49FE50 (L101469) during **substate 0 load** (called from sub_4C8FF0 at
L117625), written every tick by `AI_RecordPattern` for human players, read by
`AI_ExecutePattern` for CPU players, saved+freed at match end by sub_4A0A90
(L102070–102126, called from the exit router sub_4CA210 L118414). For
netplay determinism the block contents are write-only (no CPU reader) — the
desync vector is F1's rand() gating, not the block itself. For **offline
VS_CPU savestate/resim** the block is genuine uncaptured sim state (read by
the CPU's decisions) and cannot practically be snapshotted (400 KB × live
matchups); the correct offline mitigation is the same as online: neutralize
learning during resim. Note the only `calloc`/`free` in the whole gameplay
tree is this pair at match load/end — **allocator and handle state are
otherwise untouched during play** (zero `Handle_Alloc`/`malloc` call sites in
the tick range; consistent with M4-7's `Handle_ReleaseAll` census).

### F2 — CRITICAL: render-phase `rand()` in the super/stage background renderer (sub_4C47C0)

* sub_4C47C0 (L114841–115702) runs in the **render phase** (post-loop, once
  per display frame with ≥1 sim frame, gated by `*(match+11)==1`, L118310).
  It dispatches on a per-player state dword at entity+1244 (`i = match+42320
  + 108812·player`, L114934) and, for the animated background cases, mutates
  a per-player particle field block around entity+1248..+1850 **and calls
  `rand()`** — 15 call sites: L114989–114990, L115075–115076, L115180–115181,
  L115466, L115575–115576, L115595, L115602–115604, L115664–115665.
* Two independent violations:
  1. **RNG stream corruption** — same CRT LCG as the sim. Render cadence is
     not sim cadence: a rollback resim runs N sim frames inside one outer
     call → 1 render, while the peer that didn't roll back rendered N times.
     Whenever a super background is active across a rollback, the peers
     consume different rand() counts → permanent RNG divergence → sim desync
     (weather particles alone consume rand() every tick, L114745+). This
     directly refutes the M4-6 note that "QOH99's cosmetic-RNG carve-out has
     no AS2 analog" — sub_4C47C0 IS the cosmetic consumer sharing the stream.
  2. **Render-cadence writes into hashed state** — the particle scratch it
     mutates lives inside the P1/P2 entities, i.e. inside `main_state`, which
     is hashed by INV-22. Even absent rand(), the bytes differ between peers
     whose render:sim ratios differ → false-positive hash mismatch.
* Additional hazard: the whole pass is gated by `HIBYTE(dword_8E93EC)`
  (0x8E93EF, the local "background off" display option, L114931). Peers with
  different local settings consume rand() differently even at 1:1 cadence.
* The gate state entity+1244 is set by sim (super activation), so activation
  itself is deterministic — the damage starts the first time cadences differ
  while it is active.

**FIXED (2026-08-17)** — both violations, without a new PRNG or binary patch:

1. **RNG stream corruption → render-phase RNG isolation**
   (`input_sync_hooks.cpp` + dispatcher entry in `input_override.cpp`).
   Per DECOMP_TIMING_STUDY §1.2, `Frame_AdvanceSimulation` (already hooked:
   `Hook_AdvanceFrame`) runs exactly once per outer pass, after the sim
   while-loop and immediately before the render-phase gate; the next
   sim-side code is the next pass's first `Input_TryGetNextFrame` dispatch
   (also already hooked). During a netplay mode-8 session the CRT seed is
   captured at `Hook_AdvanceFrame` (post-sim value; runs on 0-sim hold
   passes too, which still render) and restored at the next dispatcher
   entry via `InputSyncHooks_RestoreRenderRngIfPending()`. Everything
   between the two — all 15 sub_4C47C0 rand() sites, plus any present/
   frontend consumption — is erased from the sim stream. The sim RNG stream
   on both peers becomes exactly "as if render never called rand()",
   independent of render:sim cadence AND of the local `HIBYTE(dword_8E93EC)`
   background-off option; the renderer still sees naturally varying
   (cosmetic-only, per-peer) values. This achieves the M4-6 "cosmetic
   consumer must get its own seed" rule with zero new state: the render
   stream is a throwaway continuation of the sim stream.
2. **Render-cadence writes into hashed state → digest masks**
   (`game_snapshot.cpp` `kMainDigestMasks`): entity+1244..+1851 (the +1244
   gate dword through the last mutated field +1850, rounded up to the dword
   boundary) is skipped by both digest paths for both entities. Still
   captured+restored (inside `main_state`).

### F3 — HIGH: `Frame_Display` (dword_816494) — sim-read, sim-incremented, uncaptured

* Written: `Frame_AdvanceDisplay` (0x562750, L203933–203937) — called at
  `LABEL_66` **inside the sim while-loop, once per sim tick** (L118303).
  Reset to 0 by `Netplay_InitialSync` (L203826) and by the mod's
  `Hook_MatchSyncInit` (`input_sync_hooks.cpp` L191).
* Read by sim logic every tick: the ~61-minute forced-draw check
  `if (dword_816494 == 215900)` (L118216) which sets the round-transition and
  exit-route bytes; the replay-end check (gametype 5, L118233); vanilla
  netplay pacing in `Frame_AdvanceSimulation` (L203947–203950, gametype 3
  only).
* Not in the snapshot. The snapshot's `display_frame` field is a **different
  counter** (0x81635C) — the two are easy to confuse (mod names them
  `ADDR_FRAME_DISPLAY` vs `ADDR_FRAME_COUNTER`).
* Desync test: read every tick → sim state. On every rollback the resim
  re-increments it without rewind, so it drifts ahead by the sum of rollback
  depths, differently per peer. Consequence is bounded: behavior only forks
  at the equality check, so the visible failure is a mistimed/asymmetric
  forced draw near the 215900 boundary (and replay-verification divergence in
  playback). Trivial to fix — capture/restore + hash it.

**FIXED (2026-08-17)**: new `GameSnapshot.frame_display` field (SIM) —
captured from 0x816494, restored on rollback (resim re-increments from the
rewound value), and hashed in the SimHeader of both digest paths. The
0x816494-vs-0x81635C naming trap is now annotated at `ADDR_FRAME_COUNTER` /
`ADDR_FRAME_DISPLAY` in `as2_constants.h` (fix-list item 7).

---

## 3. HASHED-BUT-NONDETERMINISTIC (INV-22 violations from the other side)

These are writes INTO `main_state` (captured and hashed) whose values depend
on non-sim inputs — they cannot desync gameplay, but they can fail the
cross-peer digest (false desync alarms) and pollute resim self-checks.

### F4 — Entity_UpdateAudio voice bookkeeping gated by wall-clock audio

* `Entity_UpdateAudio` (sub_4C38F0, L114090–114133; in-loop call L118166)
  drives character voice playback from a 3-dword block per entity at
  match+149180+108812·player → **entity+107084/107088/107092** ([0] requested
  voice id — sim-written; [1] priority latch; [2] last-played id). The
  lower-priority replacement branch writes [1]/[2] **only when
  `Audio_IsPlaying(handle)` returns false** (L114107–114116).
  `Audio_IsPlaying` (L323552) queries live DSound buffer status — wall-clock,
  peer-divergent.
* The written fields are inside `main_state` → hashed. Peers' [1]/[2] blocks
  drift apart whenever voice lines overlap → `gameplay_hash` mismatch with
  perfectly synced gameplay.
* Gates: `Audio_Enabled` (0x81637C) and per-character voice-enable
  `byte_8E93F1[charId]` — both local config.
* The mod journals `SE_Play` (hooked, `rollback_audio.cpp`) but **does not
  hook sub_4C38F0** — the voice path is unmanaged.
* Fix options: (a) mask the 2×12-byte voice blocks out of the digest (keep
  them captured/restored); (b) hook Entity_UpdateAudio in netplay and settle
  the IsPlaying question from a deterministic journal like SE_Play's.

**FIXED (2026-08-17)**: option (a) — entity+107084..+107095 (both entities,
`ENTITY_OFF_VOICE_BOOKKEEPING`) is digest-masked in
`game_snapshot.cpp::kMainDigestMasks`, following the same
captured-but-not-hashed pattern as `display_frame`. Restore still rewinds
the blocks (they sit inside `main_state`). Option (b) (deterministic voice
journal) remains available if voice presentation across rollbacks ever needs
correction — a presentation concern, not a digest one.

### F5 — Render tint timers inside the entity (sub_4C6B60)

* The player renderer sub_4C6B60 (render phase, L116286–116343) advances
  `*(entity+444)` (`ENTITY_OFF_RENDER_TINT_TIMER`) once per **render** frame
  while a tint mode 2–9 is active, and terminates the mode by writing
  `*(entity+440) = 1` when the timer expires (L116341). Both fields are
  inside `main_state` → hashed at render-cadence-dependent values.
* Additionally, the mode termination (+440 ← 1) is a **render-side decision
  written into sim-visible state**; if any character script branches on +440
  (unverified — none found in the swept handler bulk, but scripts read entity
  fields through dispatch), this graduates from hash noise to true desync.
  Treat +440/+444 as suspect pending a script audit.
* Fix: mask +440/+444 (and F2's entity+1244..+1850 super-bg scratch) from the
  digest, or freeze render-phase mutation during netplay.

**FIXED (2026-08-17)**: entity+440..+447 (both entities,
`ENTITY_OFF_RENDER_TINT_STATE`, both dwords) digest-masked in
`game_snapshot.cpp::kMainDigestMasks`; captured/restored unchanged. The
possible sim read-back of +440 by character scripts remains UNVERIFIED —
masking removes the false-alarm hash risk; if a script reader is ever found,
+440 graduates back to true desync and needs a render-side freeze during
netplay (tracked as the §8 residual, pending the script audit).

---

## 4. DELIBERATELY-EXCLUDED — verified sound reasons (and two corrections)

1. **Input history rings** `word_8164A0[85489]` (0x8164A0, 170,978 B) /
   `word_87FC24[78836]` (0x87FC24, 157,672 B) — decomp decls match mod sizes
   exactly (L203635–203636). Under the mod's netplay (gametype 2) the sim
   path only **writes** them (`Input_StoreInputToHistory` L203611; the
   read-back path `Input_GetSyncedInputs` is gametype-3-only, L203905).
   Because the write cursor `Frame_Inputs` (0x816498) IS restored, resim
   rewrites the same slots — self-healing. Sound exclusion. Caveat: replay
   save (sub_59B830, L235600+, called at match end L118426) serializes these
   rings; a mid-match crash of prediction inputs is overwritten by resim, so
   replays stay canonical as long as every misprediction is resimulated.
2. **Vanilla netplay bookkeeping** — 0x87FC20 (remote frame), 0x81649C (net
   idx), 0x8E93A4/0x8E93AE/0x8E93B2/0x8E93B6 (sync words), timeout counters
   0x8EA200/0x8EA3A8: every tick-reachable write/read is behind
   `dword_816410 == 3` (vanilla netplay) — dead under the mod's gametype 2.
3. **`display_frame` (0x81635C)** — presentation counter, TIMING exclusion
   from the digest stands. (But see F3: its near-namesake 0x816494 is NOT
   timing-only.)
4. **Training/pause state** — `Match_UpdateTrainingModeSettings` (L116869) is
   gated `dword_816410 == 4`; `Input_UpdateGlobalCommandHistory` (L117293)
   likewise (its display arrays byte_8E93C1/C2/D5/D6/E9 at 0x8E93C1–0x8E93E9
   are self-feeding but read by nothing in the sim; re-initialized by
   sub_4C92B0 each match). Pause repeat counter `byte_8E93EA` is written on
   pause entry (L118296) and used only by the substate-4 handler sub_4C8250
   (L116932+). None reachable in netplay gameplay ticks; offline savestates
   suffer at most one-frame command-display glitches. Sound.
5. **SE channel state** — `SE_ChannelIndex` (0x76C5F0, in the captured gap)
   and `SE_ChannelActive` (= per-frame temp, captured + policy-cleared).
   Feed nothing but audio channel rotation. Sound, with one requirement: the
   mod's clear-before-each-sim-frame policy must stay symmetric on both peers
   (it is — it's unconditional in the restore path), since vanilla only
   clears once per outer/display frame (L118100 is outside the while loop).
6. **FPU/MXCSR** — grep of the entire decomp: **zero** occurrences of
   `fldcw`/`_controlfp`/`_control87` in game code, i.e. the game itself never
   retunes float mode mid-play; the only mutation risk is external
   (D3D/DSound), which the per-slot capture/restore already covers. No other
   persistent float-mode state exists (x87 stack/status do not survive across
   the tick boundary; no `fesetround`, no SSE CSR writers). Verified sound.
7. **Palettes** — outside all regions by pipeline design (inmem palette
   note); no palette-side writes surfaced in the tick sweep. Stands.

---

## 5. Round-transition writes that persist (special item 7)

`Match_UpdateRoundState` (L107972–108011) writes only match+4, match+6 and
the per-player win counter (match + 108812·winner + 41254) — all inside
`main_state`, confirming M4-7 item 4. The stay-in-mode-8 round reset writes
substate globals (captured). **But the round-end commit tick (transition
timer == 25, L118182–118213) additionally calls:**

### F6 — MEDIUM: `Match_UpdateScoreStats` (0x55BCD0, L199960) — cumulative globals outside the snapshot

* Gate (L200004): human winner AND `dword_816410 <= 2` — **includes
  gametype 2, the mod's netplay type.** Runs on every non-draw round commit.
* Writes: `dword_8E9650[24·charId] += dword_790E50[…]` (playtime/score
  accumulation — a `+=`, L200008), rank byte `byte_8E9657[96·id]` (L200032),
  arcade-continuation bytes `byte_8E9E38[..]`/`byte_8E9E4D`–`byte_8E9E58`
  (L200066–200150). Also, VS_CPU only, the high-score byte at game+189 =
  0x816415 (L118189–118193).
* Desync test: **passes** — no mode-8 sim reader (readers are the records
  screen at L203060+ and mode-9 progression). Not a desync source in
  netplay.
* Real hazard: the round-commit tick sits inside the predicted window (M4-7
  deliberately has no round-boundary exact window), so a rollback across it
  **double-applies the `+=`** (and the mod's savestate F6-load in offline play
  can re-apply it arbitrarily). Corrupts persistent score/rank/continuation
  data that gets saved to config.dat. Fix: journal-suppress the call during
  resim (hook 0x55BCD0, skip when `rolling_back`), or accept and document.
* Match-end-only writes (winner byte at game+282 = 0x816472, AI save, replay
  save, `Game_ChangeMode`) sit behind the M4-7 **exact-input window** — never
  resimulated, correctly excluded.

**FIXED (2026-08-17)**: journal-suppress option — 0x55BCD0 is hooked
(`Hook_MatchUpdateScoreStats` in `input_sync_hooks.cpp`,
`ADDR_MATCH_SCORE_STATS`) and skipped while
`RollbackSession_IsRollingBack()`: the speculative first pass over the
commit tick applied the stats; the resim pass over the same tick is the
double-apply and is dropped, netting exactly one application. Post-rollback
frames beyond the old frontier are normal advances and apply fresh.
Accepted residuals (per the audit's accept-and-document clause): a
misprediction that changes the round outcome inside the replay window leaves
the superseded outcome's stats applied (persistence noise only — no mode-8
sim reader); offline manual savestate F6-load can still re-apply (practice
tooling, out of netplay scope).

---

## 6. Answers to the special-attention checklist

1. **Static locals in sim functions** — found: F1 statics (0x76C5D8–E4);
   score/continuation bytes (F6); command-history bytes (§4.4). The
   0x401000–0x49C000 handler bulk is clean (declaration-filtered sweep).
2. **Allocator/handle state** — no allocation in the tick tree; the AI-block
   calloc/load happens in substate 0, save/free at match end (F1b). Entities,
   effects (200×32 @0x76E328), summons/hitdefs (100×272 @0x76FC28) are fixed
   arrays inside `main_state`; HitDef ids live in the entries themselves
   (`HitDef_Clear` L109902 writes entry fields only — no global id counter).
3. **Audio state gating sim behavior** — yes: F4 (`Audio_IsPlaying` gates
   writes to hashed voice bookkeeping). SE path is already journaled;
   voice path is not.
4. **Camera/screen position** — scroll (match+0x744/0x746) is written by
   `Weather_UpdateScroll` **inside the sim loop** (L118147) and read by
   hit/boundary logic; screen-shake bytes match+1864/1865 decay in
   `Weather_UpdateParticles` (sim). All captured+hashed. Correct as-is.
5. **Per-frame temp window** — exact: 0x76CCF8 + 0x44 == `SE_ChannelActive[68]`
   == the game's own memset window (L114224). Boundary verified on both
   sides (SE_Handles below, scrollX above).
6. **FPU/MXCSR** — no other float-mode state; game code contains zero CW
   writers (§4.6).
7. **Round transitions** — §5; the only escapee is F6 (+ the VS_CPU
   high-score byte).

Also noted while auditing: sub_4AB0F0 (global effect-queue draw) failed to
decompile (`#error funcsize=0`, L109887) and could not be write-audited from
this file; it is however already hooked and slot-filtered by
`rollback_status_fx.cpp`, and the legacy checksum treated `effect_index` as
its only external cursor. Recommend a one-off disassembly pass to confirm it
writes nothing outside `main_state`.

---

## 7. Prioritized fix list

| # | Priority | Item | Suggested fix | Status |
|---|---|---|---|---|
| 1 | **P0** | F1: AI statics 0x76C5D8–0x76C5E7 gate rand(); persist across matches; unmanaged | Cheapest correct fix: **force `byte_8E940D = 0` for the duration of a netplay session** (kills RecordPattern *and* the substate-0 block load at its single gate), restore on session end. Belt-and-braces / offline-resim: extend GameSnapshot to capture+restore the four dwords (extend `pre_match_gap` start from 0x76C5EC to 0x76C5D8, +16 B, keep OUT of the digest is not enough here — they gate rand(), so hash them too) . | **FIXED** — director `AiLearnGuard` (armed pre-intro at the startup handoff, restored on session teardown) + dedicated hashed `ai_learn[16]` SIM region (see F1 block) |
| 2 | **P0** | F2: render-phase rand() in sub_4C47C0 + render writes into hashed entity scratch | Netplay: hook sub_4C47C0 and swap CRT rand for a private cosmetic PRNG (per the M4-6 "must get its own seed" rule) — fixes stream corruption for all stages/options at once; then mask entity+1244..+1850 (both players) plus F5's +440/+444 from the digest, or zero them at capture. | **FIXED** — render-phase RNG isolation (seed capture at `Hook_AdvanceFrame`, restore at next dispatcher entry) + digest mask +1244..+1851 both entities (see F2 block) |
| 3 | **P1** | F3: dword_816494 sim-read but uncaptured | Add to GameSnapshot (capture/restore + digest). One field. | **FIXED** — `GameSnapshot.frame_display`, captured/restored/hashed |
| 4 | **P1** | F4: voice bookkeeping (entity+107084..107095 ×2) hashed but Audio_IsPlaying-driven | Digest mask (12 B per entity), or hook sub_4C38F0 with a deterministic journal like SE_Play's. | **FIXED** — digest mask (captured/restored, never hashed) |
| 5 | **P2** | F5: tint timer/state entity+440/444 render-cadence writes into digest; possible sim read-back of +440 | Digest mask now; audit character scripts for +440 readers before trusting further. | **FIXED** — digest mask; +440 script-reader audit still open (§8 residual) |
| 6 | **P2** | F6: Match_UpdateScoreStats `+=` double-apply on resim across round commit | Suppress during resim (hook, skip when rolling back), or make the round-commit tick exact-input (costs one confirm-wait per round). | **FIXED** — hook 0x55BCD0, skip while `IsRollingBack` |
| 7 | **P3** | Doc fixes | M4_AUDITS size "~243 KB" → 259,720 B; annotate 0x816494 vs 0x81635C naming trap in as2_constants.h; record `SE_ChannelActive` identity of the per-frame temp block; record `Block` table adjacency to region end (any future region-end extension must not swallow live heap pointers). | **FIXED** — M4_AUDITS figure corrected; naming trap annotated in as2_constants.h; `SE_ChannelActive` identity recorded in §1 above; `Block` adjacency promoted to §8 verified constraints |

Residual risk after fixes 1–4: the render phase writes nothing else outside
covered regions (sweep of all 14 post-loop functions: sub_4C6260/4C3680/
4C7360/4C68D0/4C7810/4C63A0/4C05B0/4C8E20 write nothing; 4C7F30/4C1F90 write
stack locals only; Weather_Draw is pure), and the sim tick writes nothing
outside covered regions except the enumerated F-items.

---

## 8. Fix record (2026-08-17) + verified constraints

All F1–F6 findings are implemented; mechanisms are in the inline `FIXED:`
blocks above and in `IMPLEMENTATION_LOG.md` ("Savestate-audit fix pass").
Files touched: `game_snapshot.{h,cpp}`, `match_director.cpp`,
`input_sync_hooks.{h,cpp}`, `input_override.cpp`, `as2_constants.h`,
`savestate.h`, `M4_AUDITS.md`.

### Verified constraints (keep true; violating any re-opens an audit finding)

1. **`Block`-table boundary (F1b)**: the AI-learning heap-pointer table
   (decomp symbol `Block`, 484 `void*`, bounded 0x7AB762–0x7AC160) sits in
   .bss **immediately after the P2-entity end (0x7AB880)**. Any future
   extension of the `main_state` region end MUST NOT swallow these live heap
   pointers — capturing/restoring raw pointers to per-matchup calloc blocks
   would restore dangling pointers after a match-end free. The region end
   is pinned by `GAME_SNAPSHOT_MAIN_SIZE = (ADDR_P2_ENTITY_BASE +
   ENTITY_SIZE) - ADDR_MATCH_BASE`; extend only with an explicit re-audit.
2. **TODO — sub_4AB0F0 disassembly check**: the global effect-queue draw
   (0x4AB0F0) failed to decompile (`#error funcsize=0`, decomp L109887) and
   could not be write-audited from the C export. It is hooked and
   slot-filtered by `rollback_status_fx.cpp`, and the legacy checksum
   treated `effect_index` as its only external cursor — but a one-off
   disassembly pass confirming it writes nothing outside `main_state` is
   still owed before the write-set audit can be called exhaustive.
3. **Digest-mask segmentation is part of the hash value**: Block64's tail
   fold makes segment boundaries significant, so
   `GameSnapshot_HashGameplay` and `GameSnapshot_HashGameplayLive` MUST walk
   the same `kMainDigestMasks` table (they share `HashMainMasked`). Editing
   the mask table changes every digest — cross-build incompatible, like any
   hash-membership change.
4. **Render-RNG isolation depends on the loop shape** (DECOMP_TIMING_STUDY
   §1.2): capture point = `Frame_AdvanceSimulation` (once per outer pass,
   post-sim, pre-render); restore point = next `Input_TryGetNextFrame`
   dispatch (first sim-side call of the next pass). If either hook moves, or
   a future change makes sim code run between those two points, the
   isolation window must be re-derived.
5. **F5 open item**: character-script readers of entity+440 remain
   unaudited. The digest mask removes the false-alarm class; a confirmed
   script reader would make the render-side termination write (+440 ← 1) a
   true desync source requiring a render-phase freeze during netplay.
