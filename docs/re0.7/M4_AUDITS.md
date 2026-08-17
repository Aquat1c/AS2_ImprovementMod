# M4 audits — hash membership (M4-6) and exact-input windows (M4-7)

Companion to `RE07_MASTER_REBUILD_PLAN.md` §2.7.6/§2.7.7 (INV-22, INV-25).
Code anchors: `include/rollback/game_snapshot.h` (per-member role comments,
`GameSnapshot_HashGameplay`), `include/rollback/lifecycle_window.h` (predicate
+ evidence header). Decomp references are line numbers in
`decompilation/アリス戦記２.exe - Copy(refactored).c` (the refactored IDA dump;
per the decomp conventions note, field labels can lie — the classifications
below are grounded in the mod's empirically-verified constants
(`as2_constants.h`, `game_state.h`) plus targeted decomp reads, not in IDA
naming).

## M4-6 — snapshot region role table (INV-22)

Rule: the gameplay digest (SyncHash `gameplay_hash`, confirm-pipeline
`pre_state_hash`) hashes **sim-affecting state only**. Everything else is
still saved/restored (rollback must reproduce it locally) but can never fail
a cross-peer comparison. Palettes never appear anywhere below: the in-memory
palette pipeline bakes them at upload time into render-side banks outside
every snapshot region (see `inmem_palette` memory note) — render-only by
construction.

| Region / field | Address / size | Role | In digest? | Rationale |
|---|---|---|---|---|
| `main_state` | 0x76C5F8 .. P2 entity end (`GAME_SNAPSHOT_MAIN_SIZE`, ~243 KB) | SIM | YES | Match header, per-frame temp block, match context, camera scroll words, weather particles, effects pool, summons, P1/P2 entities incl. full combat state. The entire deterministic battle state lives here; camera scroll is engine-read (screen-edge push affects movement) so it stays SIM. |
| `rng_seed` | TLS `_getptd()+0x14` | SIM | YES | MSVC LCG state; drives gameplay rand(). AS2 has a single RNG stream — there is no separate cosmetic RNG to exclude (QOH99's cosmetic-RNG carve-out has no AS2 analog today; if a cosmetic consumer is ever split off, it must get its own seed, not an exclusion). |
| `sim_frame` / `input_read_idx` | 0x816490 (aliased) | SIM | YES | Canonical sim/input-read counter; feeds input history indexing. |
| `input_write_idx` | 0x816498 | SIM | YES | Input ring write cursor. |
| `game_mode` / `substate` / `substate_timer` | 0x81638C / 0x816390 / 0x816394 | SIM | YES | Mode-handler dispatch + in-mode flow control. |
| `game_type` | 0x816410 | SIM | YES | Session-constant (config-locked at pregame) but sim-read (netplay branches); including it is free and catches config divergence instantly. |
| `match_phase_timer` | 0x816370 | SIM | YES | Intro lock countdown gates interactivity. |
| `effect_index` | 0x76C5E8 | SIM | YES | Effect_Enqueue write cursor (wraps at 200); already part of the legacy live checksum. |
| `input_p1` / `input_p2` | 0x8E9E62 / 0x8E9F32, 208 B each | SIM | YES | Held/previous/just-pressed per-player input state — derived from canonical inputs, but restoring/hashing it protects against edge-trigger divergence (TrialNetplay Avoid-4 class). |
| `display_frame` | 0x81635C | TIMING | **NO** | Display/presentation counter; advances with renders, not sim truth. The DECOMP timing study places it on the presentation side; hashing it would couple the digest to pacing (INV-22 explicitly excludes timing state). |
| `pre_match_gap` | 0x76C5EC .. 0x76C5F7 (12 B) | RENDER | **NO** | Render state between the effect cursor and the match base (savestate.h note: capture-only so restores don't glitch the presenter). Saved+restored, never compared. |
| `fpu_cw` / `mxcsr` | x87 CW + MXCSR | CONTROL | **NO** | Captured/restored per slot (float determinism, §2.7.7); machine configuration, not game state — hashing it would turn an environment quirk into a "desync". |
| legacy `checksum` / `frame` / `valid` | snapshot metadata | META | NO | Bookkeeping about the snapshot, not state. |

Aux-region sweep note (risk R-3): state OUTSIDE all snapshot regions that the
sim reads is by definition a determinism hole, not a hash-membership question.
The known candidates from the 0.6 desync work (audio channel bookkeeping,
handle tables) are deliberately not snapshot members; the debug resim
self-check (§2.7.7, D-5) is the tripwire that finds any new one on a single
machine. Nothing was added to the digest at M4 that the snapshot does not
already capture.

## M4-7 — exact-input window audit (INV-25)

Question: which gameplay ticks may NOT run on a predicted remote input
because they cross an irreversible/lifetime boundary? Method per QOH99 §4.4 /
TrialNetplay §4: find every resource-release path reachable from the mode-8
match handler; predict everything else.

Findings (refactored decomp):

1. **`Game_ChangeMode` (0x5D2EB0, L266664–266692) is the only mode-transition
   path and it releases resources**: sets mode/substate/timer, calls
   `Handle_ReleaseAll()` (asset handle release; definition L303277), optional
   BGM stop. Irreversible: a snapshot restore cannot resurrect released
   handles.
2. **The mode-8 exit router `sub_4CA210` (0x4CA210, L118402+)** reads the
   match exit-route byte (match+10) and calls `Game_ChangeMode(9,1)` (win
   screen, L118430) or (6,1)/(3,1)/(5,1)/(2,1). Any tick that can execute
   this router is the match-end handoff — **exact window (a)** of §2.7.6.
3. **`Handle_ReleaseAll` call-site census**: `Game_ChangeMode` (L266689), a
   title/menu handler (L201044, `dword_7AC318` menu context), and the system
   shutdown path `sub_611EB0` (L301960). **Zero call sites inside the mode-8
   handler tree** (`sub_4C8F60`, L117463+).
4. **Round-to-round state is pure match-region data**:
   `Match_UpdateRoundState` (decl L856, def L107972–108011) mutates only the
   round-result byte (match+4), the result timer word (match+6) and the
   per-player win counters (match+41254) — all inside `main_state`. The
   stay-in-mode-8 round flow resets substate via plain global writes
   (`dword_816390 = 3; dword_816394 = 0;` L118391–118394). Nothing is freed
   or reloaded at a round boundary.

**Verdict (pins §2.7.6's initial set):**

- Round boundaries inside a match: **predicted normally** — the §2.7.6 "(b)
  round-result destructive commit tick" candidate does NOT exist in AS2
  (matches TrialNetplay's finding for this engine family). No exact window.
- Match-end handoff (any tick that can run the mode-8 exit router):
  **exact-input** — `Handle_ReleaseAll` via `Game_ChangeMode` is a lifetime
  boundary.
- Everything outside mode 8 substate 3: exact by architecture (frontend
  lockstep owns the stream; the predicate fails safe if consulted).

Predicate: `LifecycleWindow_IsExactInputNext(game_mode, substate,
match_exit_pending)` in `include/rollback/lifecycle_window.h`; the engine
veto is `RollbackEngine::SetLifecycleExactNext` → `Stall(LifecycleBoundary)`
(unit-pinned in `tests/engine2_tests.cpp::TestLifecycleWindow`).

Open item for M6 (director wiring): `match_exit_pending` must be derived
from the match exit-route state / MatchLifecycle winscreen handoff, not from
the M4 conservative stand-in in `rollback_session_engine2.cpp`
(`RefreshLifecycleWindow`), which treats only non-(mode 8, substate 3) ticks
as exact and therefore never predicts across ANY mode change — safe but one
frame more conservative than the audited minimum at match end.
