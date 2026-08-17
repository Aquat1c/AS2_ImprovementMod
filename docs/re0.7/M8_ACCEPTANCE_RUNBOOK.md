# M8 Acceptance Runbook — re0.7 field acceptance runs

Authority: `RE07_MASTER_REBUILD_PLAN.md` §7 (test plan), §7.5 (acceptance
criteria), §8.3 (fallback criteria). This document is self-contained: it lists
every run, its exact configuration, the expected outcome, and the judge
command. The code/tooling side of M8 is complete (see IMPLEMENTATION_LOG.md
M8 entry); everything below is a FIELD RUN the operator performs on real
builds. GekkoNet removal and the 0.7-fallback decision are both gated on the
outcomes recorded here (§6/§7 below).

---

## 1. Prerequisites

### 1.1 Builds

- **Primary config:** default CMake (`AS2_WITH_GEKKO=OFF`) = engine2 backend.
  All acceptance runs use this config.
- **Fallback config sanity:** `-DAS2_WITH_GEKKO=ON` must still configure and
  compile (it is the §8.3 partial-fallback ship vehicle). No acceptance runs
  use it; one smoke build is enough.
- Unit suite green first: `frame_arithmetic_tests`, `frame_scheduler_tests`,
  `engine2_tests` (includes the 100k-frame socket-free soak + microbench),
  `frontend_sync_tests`, `transition_barrier_tests`, `async_log_tests`, and
  `tools/check_killpaths.ps1` (CI kill-path gate, INV-12).

### 1.2 Machines

- **min-spec** = the 0.7 field-log machines (plan §9 Q4 "pragmatic"). All
  §7.5 numbers are judged on min-spec AND spot-checked on one modern machine.
- Two-instance local runs need **separate game folders** (file-lock hazard,
  plan §7.2). The harness launcher (`as2_test_harness.exe`) already does this;
  manual runs must copy the game directory.

### 1.3 Instruments

| Instrument | What it is |
|---|---|
| **STAT line** | Per-second frozen-format telemetry in `logs/<...>/as2_netplay_fullpath_<pid>.log`, tag `[STAT    ]`. Emitted only while the FrameScheduler detour is installed — a log with no STAT lines means the R-1 fallback fired (check for the loud install-failure line; that run is invalid for pacing acceptance). |
| **`tools/analyze_stat.py`** | The judge. Parses STAT + incident lines, prints PASS/FAIL per §7.5 criterion. `python tools/analyze_stat.py --profile <p> <log> [log2]`. Run it on BOTH instances' logs. Exit code 0 = pass. |
| **Rematch soak** | `src/testing/rematch_soak.cpp` verdict layer over the autoconnect harness. M8 additions: per-frame epoch-regression + canonical-counter (INV-15) assertions, per-iteration epoch-rotation assertion, fastpath/charsel route labeling. `SOAK` tag; final `SUMMARY ... verdict=SOAK-PASS`. |
| **Autoconnect driver** | `as2_autoconnect.cfg` `[autoconnect]`. M8 addition: `continue_no_every = K` answers NO on every Kth continue prompt (cursor RIGHT, then A) so soaks cover the any-NO charsel route. Both YES,YES fast path and any-NO were already handled by the driver's `ConfirmingWinScreen` state. |
| **Harness launcher** | `as2_test_harness.exe` — SHM dashboards, save/load/advance µs profiling, final-state summary, log triage. |
| **Key log tags** | `PACE` (scheduler install/rebase/dead-clock), `STAT`, `PREGAME` (match_setup phases/epochs), `LADDER` (match-end barrier ladder + `F-7 TERMINAL`), `FRONTRESYNC` (INV-11 interrogation), `CONTINUE` (prompt cursor/lock/resolution), `SOAK`, `PALETTE`, `CONNSUP` (supervisor bands/progress deadline), `INJECT` (fault injection), `SPLAY` (spectator playback), `REPLAY` (replay record/verify), `STRESS`. |

### 1.4 Network shaping — what tool for what

- **`AS2_NET_INJECT_*` env vars** (outbound app-packet drop/blackout, read
  once at startup — see `docs/RESILIENCE_TESTING.md` §1 for full semantics):
  - `AS2_NET_INJECT_DROP_PCT` (0–100), `AS2_NET_INJECT_BLACKOUT_MS` +
    `AS2_NET_INJECT_BLACKOUT_PERIOD_MS`.
  - **INV-14 semantic change (important, supersedes the old doc):** the
    supervisor now measures ENet **protocol-level** silence. App-egress
    injection does NOT silence ENet acks/pings, so it does NOT move the
    supervisor bands anymore. It starves the InputStream/gameplay streams →
    engine-side loss behavior (PredictionLimit holds, redundant-window
    refill), which is exactly what the loss cells need. For supervisor-band
    runs (bursts, W-2..W-6) use external shaping or a cable pull.
  - Set on **both** instances for bidirectional loss.
- **External shaping (clumsy / netem router / real WAN):** required for every
  cell with an RTT figure — there is **no in-mod latency injection**
  (`AS2_NET_INJECT_DELAY_MS` is unimplemented by design). LE-1 and the burst
  rows are most faithful over a real WAN link or clumsy with
  lag+jitter+drop on UDP both directions.
- **StressHooks (mod menu → stress panel):** engine-level knobs wired on
  engine2 — egress packet drop %, input delivery-delay (frames, 16-slot
  queue ahead of ingest), forced prediction mismatches (corrupts PREDICTIONS
  only, INV-19-safe). Use for deterministic depth/rollback forcing, not for
  wire realism.

### 1.5 Session knobs per run

- Delay/rollback are peer-local (INV-23): set each side's D/R in the netplay
  config screen (or `delay_frames` in the cfg + in-game `-`/`=` hotkeys).
- Cadence: `proper_60` is the netplay default and is handshake-enforced equal
  on both peers. All acceptance numbers below assume proper_60 (16 667 µs
  period; `--cadence 60`).

---

## 2. Run matrix

Every run: 3-match session (2 rematch boundaries) unless stated. Judge BOTH
peers' logs. `analyze` = `python tools/analyze_stat.py`. Use `--skip 30` (or
more) to trim menu/connect warmup so the window starts near first gameplay;
keep the window covering the rematch boundaries — §7.5#2 is end-to-end.

### 2.1 Pacing / §7.5 performance runs

| Run | Setup | Shaping | D/R | Expected | Judge |
|---|---|---|---|---|---|
| **R-OFF** | Single instance, offline VS/arcade ≥3 min, min-spec | none | — | §7.5#1: p99 ≤ 17.2 ms, zero holds/rollbacks, sim 60.00±0.02, slew 0, zero `PACE` rebase lines, CPU in wait ≤5% of one core (check Task Manager / the `PACE` install line) | `analyze --profile offline` |
| **R-CLEAN-LAN** | Two instances, LAN/loopback (separate folders) | none (≈0 ms) | D0/R2 both | 60.00 flat, zero holds, depth ≤1 | `analyze --profile online-clean` both logs |
| **R-CLEAN-80** | Two instances | 80 ms RTT ±5 ms, 0% loss | D2/R4 | zero holds, steady shallow rollbacks (≤3) | `analyze --profile online-clean` |
| **R-LE1** | **The acid test** — field-log replication (§7.3 LE-1; the scenario 0.7 failed at 51 fps / 8 freezes/s) | 155 ms RTT ±20 ms, 0% loss (real WAN preferred) | **D0/R8** | **zero holds of any cause**, continuous rollbacks depth ≈5, rb_max ≤8, sim 60.00±0.02, p99 ≤18 ms, across all 3 matches + both rematch boundaries | `analyze --profile le1` |
| **R-POL** | Same link as R-LE1 | 155 ms ±20 ms, 0% | D4/R4 (policy-recommended) | zero holds, shallow rollbacks (≤2) | `analyze --profile online-clean` |

### 2.2 Loss / degradation runs (§7.3)

| Run | Shaping | D/R | Expected | Judge |
|---|---|---|---|---|
| **R-LOSS-3** | 40 ms ±15 ms, 3% loss | coverage = required+1 | <2 holds/min, frequent shallow rollbacks, p99 ≤18 ms, no visible stutter | `analyze --profile lossy` |
| **R-LOSS-1** | 155 ms, 1% loss | coverage = required+1 | <2 holds/min, continuous rollbacks, fluid | `analyze --profile lossy` |
| **R-LOSS-5** | 155 ms, 5% loss | coverage = required+1 | occasional 1-frame holds, playable, HUD shows cause honestly | `analyze --profile lossy` |
| **R-DEG** | 10% loss + 100 ms jitter | any | degraded but stable; **zero teardowns**; instant recovery when shim clears | `analyze --profile degraded` + §4 grep: no terminal lines |
| **R-UNDER** | 155 ms, 0% loss | **D0/R3** deliberately underbuffered | honest slideshow: continuous PredictionLimit holds, **rb_max = 3 exactly, never a hold before depth 3** (INV-4 field pin), HUD `[Underbuffered]` badge | `analyze --profile report`; assert rb_max=3 in summary; ZERO `FRONTRESYNC` lines |
| **ResyncRequest guard** (rides every loss run) | — | — | `Sent ResyncRequest` count 0 on plain loss (M5 obligation: the 120-tick window + redundant refill must make interrogation structurally unlikely) | `incident_resync_request` in every lossy verdict — investigate ANY occurrence |

### 2.3 Burst / liveness runs (W-2..W-6) — external shaping or cable pull only

| Run | Outage | Expected | Evidence |
|---|---|---|---|
| **R-BURST-2** | 2 s full block | freeze at prediction ceiling, supervisor **Degraded**, resume ≤3 s from a single InputStream packet each way, catch-up ≤2 hidden frames/pass, no rebase burst | `CONNSUP` band lines; STAT hold_pred window; no terminal |
| **R-BURST-5** | 5 s | as above + supervisor **Interrupted** (5–20 s band), HUD banner, engine does NOT abort | same |
| **R-BURST-15** | 15 s | frozen ~15 s, resumes; Dead never fires early | same |
| **R-BURST-25** (negative control) | 25 s | exactly ONE teardown at 20 s: supervisor Dead, self-describing reason both sides | `CONNSUP` Dead + Disconnect reason |
| **R-ONEWAY** | 30 s one-direction block | teardown at 20 s by whichever supervisor starves; reason names the direction; HUD distinguishes "peer stopped producing" vs "packets not arriving" (PressureReport) | `CONNSUP` + HUD |
| **R-WEDGE** (P-8) | suspend the peer process (debugger) with the link alive | 8 s → HUD "opponent's game stopped responding", 20 s → `ProgressDeadline` teardown (distinct reason) | `CONNSUP` progress lines |

### 2.4 Lifecycle / soak runs (§7.4)

| Run | Setup | Expected | Judge |
|---|---|---|---|
| **R-SOAK-100** | Two instances, autoconnect: `match_count=101`, `soak_rematches=100`, `continue_no_every=4` (≈75 fast-path + ≈25 charsel cycles), `AS2_NET_INJECT_DROP_PCT=3` both sides | Both logs end `verdict=SOAK-PASS`. The soak itself now asserts: epoch strictly increasing per handoff, no epoch regression, canonical counter monotonic across the WHOLE session (INV-15), session never dies. | both `SOAK` summaries + §4 grep checklist |
| **R-SOAK-YES** (optional pure-fast-path) | as above, no `continue_no_every` | 100 fast-path cycles (`fastpath=100` in SUMMARY) | same |
| **R-DET** | Harness two-instance scripted run, 15k+ battle frames, StressHooks 3% drop + delivery-delay jitter | zero desyncs, hash chain clean (reference bar: QOH99's 13 230 frames / 0 mismatches at 40±15 ms + 3% loss); determinism_verify + debug resim self-check enabled | launcher final summary: `dsync=0`; no `ConfirmedDesync` |
| **R-SPEC** (M7 gate) | Spectate a full 3-match rematch session; include a late join mid-match and a starve test (block the sidecar feed briefly) | late join: deep-backlog catch-up ladder, joins cleanly; starve: elastic slow-motion (950/850/700‰), NEVER freeze-then-sprint; prime ≈4 s cushion is by design | `SPLAY` log: prime/rebuffer/elastic transitions + S-4 `ACQUIRED` line; zero spectator desync exits |
| **R-REPLAY** (M7 gate) | Replay the R-SPEC session's files | plays back hash-clean | zero `REPLAY DESYNC` lines (`incident_replay_desync` gate) |

### 2.5 Cost benchmarks (§7.5#4, M4 obligation)

| Run | How | Acceptance |
|---|---|---|
| **R-BENCH-UNIT** | `engine2_tests` microbench on **min-spec** (prints save/restore + Block64 p50/p99 at real `sizeof(GameSnapshot)` volume) | savestate p99 ≤ 1.5 ms |
| **R-BENCH-GAME** | During R-LE1 on min-spec: harness SHM profiling panel (`Save/Load/Advance avg/peak µs`) or the rollback_debug ImGui panel | in-game save/restore peak consistent with the bench; **depth-8 correction ≤ 10 ms** (peak load + 8×advance). If exceeded: document a lower R recommendation cap (plan R-2 pressure valve) — not a rebuild trigger. |

### 2.6 Handshake / edge spot checks (§5, manual)

Quick manual passes — each is one attempt, evidence is the on-screen reason
plus the log:

- **C-3:** mismatch a build (or force `compat_58` on one side): fail-closed
  refusal naming the exact field, both sides, no retry loop.
- **C-5/C-6:** cross-join race and third-peer connect → `Busy`, existing
  session unaffected.
- **F-10:** browse the config screen ≥5 min before Ready — session must stay
  Healthy indefinitely (protocol-silence liveness).
- **P-6:** alt-F4 mid-match → peer shows "Peer closed the game" (never
  "Connection lost").
- **B-7:** hammer `-`/`=` delay hotkeys both sides mid-match → no wire
  traffic, no interaction, HUD-only changes.

---

## 3. §7.5 acceptance checklist (release gate)

All measured by the STAT instrument on min-spec + one modern machine:

- [ ] **1. Offline:** R-OFF passes `--profile offline` (mean 16.667±0.01 ms,
      p99 ≤17.2 ms, zero rebases, ≤5% core).
- [ ] **2. Online clean:** R-CLEAN-LAN, R-CLEAN-80, R-POL pass
      `--profile online-clean` — zero holds of any cause end-to-end across
      3 matches + 2 rematch boundaries, 60.00±0.02, p99 ≤18 ms.
- [ ] **3. LE-1:** R-LE1 passes `--profile le1` exactly as tabled.
- [ ] **4. Costs:** R-BENCH-UNIT + R-BENCH-GAME within budget (or documented
      R cap).
- [ ] **5. Freeze pulses:** 0 at 0% loss (identical to the zero-holds check);
      NETCLASS class-change events: the metric no longer exists — its absence
      is the criterion (grep for `NETCLASS` finds nothing).

## 4. Log-grep checklist (runs alongside every soak/session)

PowerShell, from the game dir (`Select-String` over
`logs\*\as2_netplay_fullpath_*.log`); `analyze_stat.py` already counts the
starred ones:

- [ ] * `F-7 TERMINAL` — 0 occurrences across all healthy cycles.
- [ ] * `BeginInputPhase while phase` — 0 (INV-8 deferred-Begin fires).
- [ ] * `Sent ResyncRequest` — 0 on clean/plain-loss runs; any occurrence on
      a healthy link is a bug to file.
- [ ] * `REPLAY DESYNC` — 0 on R-REPLAY.
- [ ] `PACE` + `rebase` — 0 during R-OFF/clean runs (external stalls exempt).
- [ ] `PREGAME` epoch lines strictly increasing (also asserted live by the
      soak; grep is the two-instance cross-check).
- [ ] `PALETTE` re-arm present after EVERY epoch (no `override=none`
      leftovers into a rematch — §7.4).
- [ ] `LADDER` — every match end shows WinScreenExit → PostMatchDecision →
      EpochAlign committed in order; no `held` line older than ~10 s.
- [ ] SetTracker continuity: set score carries across all soak cycles (HUD
      or `[SetTracker]` mod-log lines; §7.4).
- [ ] `Deferred` / dead-clock / `PacingClockDead` — 0.

## 5. §5 edge-matrix coverage map (test-id column, plan §7.4)

Where each edge-matrix row is exercised. `unit` = already pinned by the CI
suites; `harness` = covered by a §2 run; `manual` = §2.6 spot check.

| Rows | Coverage |
|---|---|
| C-1..C-2 | manual (connect timeout/refusal — status text, no error cycle) |
| C-3..C-7 | unit-ish (session2 handshake logic) + manual §2.6 |
| W-1 | R-LOSS-3/1/5 |
| W-2..W-5 | R-BURST-2/5/15/25 |
| W-6 | R-ONEWAY |
| W-7..W-8 | R-LE1 variants (spike the shaping mid-run; expect clean holds only, badge on sustained) |
| W-9 | manual: mid-match NAT rebind with autopunch relay (shipping guards) |
| W-10 | long R-LE1 (≥30 min): slew absorbs crystal drift, STAT slew_ppm small and bounded |
| W-11 | unit (T-ENG-7/T-CODEC) + any loss run |
| P-1 | run min-spec vs modern machine pair; expect honest labeled holds on the fast side |
| P-2..P-4 | manual (unplug pad, minimize, suspend) |
| P-5..P-6 | manual §2.6 |
| P-7 | kill -9 one instance → peer Dead at 20 s |
| P-8 | R-WEDGE |
| F-1..F-2 | R-SOAK-100 (ladder ordering + mode-9 laggard alignment every cycle) |
| F-3 | unit (frontend_sync interrogation tests) + R-UNDER zero-resync guard |
| F-4 | manual charsel cancel storm (rapid cancel/re-enter both sides ×10) |
| F-5..F-7 | R-SOAK-100 with `continue_no_every` (simultaneous resolution, AFK timeout via one side idle, F-7 guard silent) |
| F-8 | R-SOAK-100 fast-path cycles (freeze-coverage window) |
| F-9..F-12 | R-SOAK-100 + F-10 manual; F-12 needs a deliberate content mismatch (edit a data file on one side → one retry then `BaselineMismatch` terminal) |
| D-1..D-5 | R-DET + StressHooks forced mismatches (D-1 evidence dumps both sides); D-5 debug build resim self-check |
| S-1..S-6 | R-SPEC + R-REPLAY |
| B-1..B-7 | R-SOAK-100 (B-1/B-4/B-5 arise naturally) + manual B-2/B-6/B-7 |

## 6. GekkoNet removal criteria (§8 / M6-M7 deferral)

GekkoNet (CMake `AS2_WITH_GEKKO` option, `rollback_session.cpp` Gekko
adapter, `lib/GekkoNet`, `gekko_input_tests`) is removed **only after ALL**:

1. §3 checklist items 1–5 all pass on real builds (the M6 cutover gate has
   actually RUN, not just compiled).
2. R-LE1 passes on min-spec — the field-log scenario is demonstrably fixed.
3. R-SOAK-100 passes on both instances (both routes exercised).
4. R-DET: desync rate ≤ 1/100 harness matches.
5. No §8.3 fallback trigger (below) is active.

Removal task list (one commit, after sign-off): delete the CMake option +
conditional blocks (GekkoNet subdir/link/defines, adapter selection,
`gekko_input_tests`), `src/rollback/rollback_session.cpp`, `lib/GekkoNet`,
the `AS2_WITH_GEKKO` guard in `src/testing/rematch_soak.cpp` (keep the
engine2 strict branch), and the fallback-config wording in this doc +
CMakeLists comments. Grep `AS2_WITH_GEKKO|Gekko` must then return docs only.

## 7. 0.7-fallback criteria (§8.3)

Fallback = ship branch `0.7` (GekkoNet + resilience M-work) while re0.7
continues in dev. Trigger if ANY:

1. The cutover gate (§3 above) is not green within 3 weeks of code-complete —
   desync rate >1/100 harness matches unresolved, or any INV structurally
   violated.
2. §7.5 criteria 2–3 unreachable on min-spec after R-2 mitigations (region
   pruning, R-cap).
3. **R-LE1 does not show the field scenario fixed** — any recurrence of
   hold-rate >1% of pace decisions at 0% loss.
4. A frontend regression class (charsel/rematch) with no drop+log downgrade
   available.

Partial fallback (pre-planned): M2 (scheduler) + M3 (transport/session/
supervisor) are individually shippable on top of GekkoNet
(`AS2_WITH_GEKKO=ON` config) — they fix INV-5/14/17 independently and are
strictly better than 0.7 alone.

## 8. Known analysis caveats

- **Lifecycle holds in zero-hold profiles:** frontend lockstep waits are
  labeled `LifecycleBoundary` (M6). In a fully-driven harness run they should
  be 0; in a HUMAN-paced session, winscreen/charsel waits reflect the humans,
  not the pacer. `--allow-lifecycle N` exists for that documented case only —
  gameplay holds (`pred/input/ext`) have no allowance ever.
- **STAT covers gameplay + menus alike** (a menu pass counts as one sim).
  Use `--skip` to window past connect/menus; the frozen format has no phase
  field by design (M7-6 decision — the hold-episode ledger gives per-phase
  visibility if needed).
- **compat_58:** the delay-recommendation oneway→frames conversion uses the
  fixed proper_60 period; under compat_58 it overestimates need by ≤2%
  (errs toward more coverage — safe). Reviewed at M8, retained. Pass
  `--cadence 58.8` to the analyzer for compat sessions.
- **S-4 spectator acquire:** if field spectates never log `ACQUIRED` within
  600 records, the spectator hash membership needs the M4-6 audit treatment;
  fail-degrade keeps sessions alive meanwhile (M7 note).
