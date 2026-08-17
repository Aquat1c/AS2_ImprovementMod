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
| `main_state` | 0x76C5F8 .. P2 entity end (`GAME_SNAPSHOT_MAIN_SIZE` = 0x3F288 = 259,720 B, ~253.6 KB) | SIM | YES (minus masks) | Match header, per-frame temp block, match context, camera scroll words, weather particles, effects pool, summons, P1/P2 entities incl. full combat state. The entire deterministic battle state lives here; camera scroll is engine-read (screen-edge push affects movement) so it stays SIM. SAVESTATE_AUDIT F2/F4/F5: three per-entity windows inside this region are render-cadence/wall-clock written and are digest-MASKED (captured+restored, skipped by both hash paths): +440..+447 tint, +1244..+1851 super-bg scratch, +107084..+107095 voice bookkeeping (`kMainDigestMasks`, game_snapshot.cpp). |
| `rng_seed` | TLS `_getptd()+0x14` | SIM | YES | MSVC LCG state; drives gameplay rand(). AS2 has one physical RNG stream, but SAVESTATE_AUDIT F2 found the cosmetic consumer this row said didn't exist: the render-phase super-background renderer (sub_4C47C0, 15 rand() sites). Fixed per this row's own rule ("must get its own seed, not an exclusion") via render-phase RNG isolation: the CRT seed is captured post-sim (Frame_AdvanceSimulation hook) and restored at the next dispatcher entry, so render consumption never reaches the sim stream (input_sync_hooks.cpp). |
| `sim_frame` / `input_read_idx` | 0x816490 (aliased) | SIM | YES | Canonical sim/input-read counter; feeds input history indexing. |
| `input_write_idx` | 0x816498 | SIM | YES | Input ring write cursor. |
| `game_mode` / `substate` / `substate_timer` | 0x81638C / 0x816390 / 0x816394 | SIM | YES | Mode-handler dispatch + in-mode flow control. |
| `game_type` | 0x816410 | SIM | YES | Session-constant (config-locked at pregame) but sim-read (netplay branches); including it is free and catches config divergence instantly. |
| `match_phase_timer` | 0x816370 | SIM | YES | Intro lock countdown gates interactivity. |
| `effect_index` | 0x76C5E8 | SIM | YES | Effect_Enqueue write cursor (wraps at 200); already part of the legacy live checksum. |
| `input_p1` / `input_p2` | 0x8E9E62 / 0x8E9F32, 208 B each | SIM | YES | Held/previous/just-pressed per-player input state — derived from canonical inputs, but restoring/hashing it protects against edge-trigger divergence (TrialNetplay Avoid-4 class). |
| `frame_display` | 0x816494 | SIM | YES | SAVESTATE_AUDIT F3: `Frame_Display` — ++ once per SIM tick (Frame_AdvanceDisplay), sim-read every tick (215900 forced-draw check, replay-end check). NOT the render-loop counter 0x81635C below — see the naming-trap note in as2_constants.h. |
| `ai_learn` | 0x76C5D8 .. 0x76C5E7 (16 B) | SIM | YES | SAVESTATE_AUDIT F1: AI pattern-learning cross-frame statics — they gate rand() consumption, so they are SIM by INV-22. Zeroed at the netplay startup handoff (director AiLearnGuard) so peers hash identical values; the guard also forces the master gate byte_8E940D to 0 for the session. |
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
*(Closed at M6-1/M6-3: the director now derives `match_exit_pending` from
MatchLifecycle and mirrors it via `RollbackSession_SetMatchExitPending`.)*

## T-ENG-9 grep half — INV-4 "no other code path reads R" audit (M8)

The executable half of T-ENG-9 (exactly R speculative frames before the first
`Stall(PredictionLimit)`) is pinned in `tests/engine2_tests.cpp`. This section
is the deferred grep/review half (journal M4-8 deviation): a full-tree sweep
of every reader of the rollback budget (`max_rollback` / `rollback_budget` /
`R_local` / `RollbackSession_GetRollbackBudget`), classified. Date:
2026-08-17 (M8), engine2 default config.

**The single gating comparison** — `src/rollback/engine2.cpp` `NextAction()`:

```cpp
if (SpeculativeFrames() >= config_.max_rollback)   // marked INV-4 in-source
```

No other comparison, scaling, or clamp against the budget exists anywhere in
a sim/hold decision. Every other reader falls into one of five audited-and-
legitimate classes:

| Class | Readers | Why legal under INV-4 |
|---|---|---|
| 1. Config validation / arm logging | `engine2.cpp` Arm (range check 1..15, arm log), adapter clamps in `rollback_session_engine2.cpp` / `rollback_session.cpp` (facade config → EngineConfig) | Input validation before the engine owns the value; never re-read after arm. |
| 2. Hidden catch-up headroom (§2.8.3, plan-mandated) | `input_override.cpp` (`telemetry.rollback_budget − depth − 1` fed to `FrameScheduler_TryTakeCatchupFrame`; `frame_scheduler.h` documents the caller-computed contract) | The plan's own formula `k = min(debt, 2, R − depth − 1)`. Bounds HIDDEN extra work; cannot cause a hold or shrink the budget available before `Stall` (the −1 reserves the visible frame, INV-21). |
| 3. Observational run-state / telemetry | `engine2.cpp` (`ClassifyRunState` input `local_max_rollback`; `Stats.max_rollback_depth`), `run_state.h` warning band, snapshot fills (`rollback_budget`, `max_rollback_distance`) | Classification and HUD/STAT reporting only; §2.10 says observational, no decision consumes RunState. |
| 4. Policy / UI / advisory (INV-6/INV-23 surface) | `delay_policy.cpp` (directional coverage `D_peer + R_local`, recommendation, remote-advisory `max_rollback`), `netplay_menu_controller.cpp` / `netplay_menu_state.h` (config screen), `rollback_debug.cpp` (ImGui + debug skew thresholds), `desync_dump.cpp` (evidence print), PressureReport `adv_rollback` (engine2.cpp egress — advisory to the PEER, never applied locally) | The coverage/recommendation math is exactly what INV-6 requires the UI to show; nothing here feeds `NextAction`. |
| 5. Fx journal sizing | `RollbackAudio/ComboFx/StatusFx_OnSessionBegin(rollback_budget)` (director fan-out) | Buffer sizing for suppression journals; §2.6.1 contract. |

Also checked: zero remaining references to the retired budget-shrinking
vocabulary (`NETCLASS`, `stall_threshold` / `protection_window` consumers —
snapshot fields survive pinned to 0 for shape stability, no reader acts on
them), and no test/harness code reads the budget into a pacing decision.
`temp_session.cpp` (repo root) references a Gekko config but is not in any
CMake target — dead scratch file, flagged for deletion with the Gekko
removal commit.

**Verdict: INV-4 holds.** The engine uses all R frames before holding, and
nothing outside the single marked comparison can shrink or scale the
effective budget.
