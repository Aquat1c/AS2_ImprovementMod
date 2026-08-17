# RE0.7 Master Netcode Rebuild Plan

**Branch:** `re0.7` (fallback: `0.7`). **Date:** 2026-08-17. **Status:** implementation-ready.

This plan is the single authority for the 0.7 netcode rebuild. It is written to be implemented
directly, without re-deriving decisions. Source evidence and studies (all in this directory unless
noted):

| Doc | Role in this plan |
|---|---|
| `LOG_EVIDENCE.md` | Field failures the rebuild must fix; §5 must-not-do list → §1 invariants |
| `QOH99_BACKEND_STUDY.md` | The porting template (architecture, scheduler, epoch model, lessons §7) |
| `TRIALNETPLAY_STUDY.md` | Second reference (handshake, timeline generations, pitfalls; caster parts excluded) |
| `DECOMP_TIMING_STUDY.md` | Engine loop truth; pacing recommendation §5(b)+(a) adopted verbatim |
| `FRONTEND_BACKEND_INVENTORY.md` | KEEP/REBUILD boundary, API contract (§2), rebuild sequence (§10) |
| `../AS2_0_7_RESILIENCE_IMPLEMENTATION_NOTES.md` | Shipping features that must survive (supervisor, barriers, kill-path gate, NAT healing) |
| `../AS2_Continue_Screen_Netplay_Rematch_Plan.md` | Shipping continue-screen rematch that must survive |

**Fixed decisions (user, not revisitable inside this plan):**

1. Reuse the existing in-game frontend/menu. No caster-style external UI, no second process.
2. Rebuild the backend modeled on qoh99_netplay (single canonical timeline, epoch rotation).
3. Custom rollback engine. GekkoNet removed. ENet kept per inventory §8 verdict.
4. Winscreen continue-screen rematch (continue_flow) is reused, not redesigned.
5. Hard product requirement: **no fps issues, jitter, slowdown, or frame drops without a genuine
   cause** (packet loss or weak hardware). A clean 155 ms RTT / 0% loss link must play at a flat
   60.00 fps with zero holds when coverage is adequate — the exact scenario the 0.7 field logs
   failed (51–52 fps, 8 freezes/sec).
6. Work happens on `re0.7`; `0.7` is the fallback branch (criteria in §8.3).

---

## 1. Goals, non-goals, and design invariants

### 1.1 Goals

- **G1 — Flat frame pacing.** At 0% loss and adequate coverage: zero holds, zero freezes,
  present-interval p99 within 0.5 ms of target, for the whole session including rematch cycles.
- **G2 — One canonical timeline.** The session arms once; the mod-owned frame counter never resets;
  matches are epoch rotations, not session re-bootstraps. Per-match GekkoNet bootstrap is gone.
- **G3 — Custom rollback engine** behind the preserved `RollbackSession_*` facade: capture-once
  inputs, delay-as-relabel, hold-last prediction, earliest-mismatch correction, confirmed-frame
  pipeline, exact periodic state hashing, fail-closed desync handling.
- **G4 — Mod-owned pacing.** The mod's absolute-deadline scheduler replaces the game's 17 ms
  busy-spin; `Hook_GetTick` is pinned to 1.0; there is exactly one speed authority.
- **G5 — Teardown discipline.** The connection supervisor is the sole network-originated kill.
  Every other failure is recovered, retried, or surfaced — never escalated to session destruction.
- **G6 — Survivors keep working**: frontend/menu, frontend lockstep (charsel/stagesel/winscreen),
  continue_flow rematch, palette exchange, spectator sidecar, replay, training, NAT stack,
  supervisor + transition barriers + kill-path CI gate.

### 1.2 Non-goals

- No external caster/launcher, no second process, no session-ini contract (TrialNetplay §3 items
  1–3 explicitly excluded).
- No transport rewrite: ENet stays; only `network_thread`/`session_manager` internals are rebuilt.
- No GGPO-style timesync/frames-ahead rebalancing protocol. Deliberately absent (QOH99 §3.6).
- No hold-negotiation protocol between peers (QOH99 lesson 14: independent holds compose).
- No mid-session *negotiation* of delay/rollback (they become peer-local knobs, §2.9). The existing
  delay-change request/ack wire flow is retired.
- No new spectator topology (relay rooms etc.); the existing sidecar model is re-fed, not redesigned.
- Vanilla netplay (GAMETYPE 3) paths stay suppressed exactly as today; we do not reuse the vanilla
  wire loop.

### 1.3 Design invariants (numbered, enforced, traceable)

Every LOG_EVIDENCE §5 must-not-do maps to a numbered invariant with the concrete mechanism that
enforces it. INV-1..14 correspond 1:1 to LOG_EVIDENCE items 1–14; INV-15+ are derived from the
reference studies and the fixed decisions. **Any code change that violates an INV is a rejected
review, no exceptions.**

| # | Invariant | Enforcing mechanism | Plan § |
|---|---|---|---|
| INV-1 | **No frames-ahead/transit-gap debt pacing.** The steady-state in-flight input gap (≈ one-way RTT in frames − delay) is never treated as actionable error. | The debt controller is deleted. The only network-driven hold is `Stall(PredictionLimit)` when `speculativeFrames ≥ R_local` — a hard local capacity fact, not a transit estimate. The in-flight gap lives *inside* R by construction. There is no "debt = raw_gap − remote_eff" quantity anywhere in the new code. | §2.7.4, §2.8.5 |
| INV-2 | **No full-frame freezes as a routine actuator.** Freezes are for genuine capacity exhaustion only. | Hold causes are a closed enum: `PredictionLimit`, `LifecycleBoundary`, `LocalInputMissing`, `ExternalSuspension` (correction lateness is not a hold — it is absorbed by deadline rebase, §2.8.6). No `soft_hold`/`hard_hold`/`persistent_debt` decisions exist. Continuous rate error is absorbed by the absolute-deadline scheduler (sub-ms period accuracy) and the bounded one-sided pace slew. | §2.8 |
| INV-3 | **No heuristic network classifier rewriting thresholds tick-to-tick.** | The NetQuality classifier (`NETCLASS`) is deleted, not tuned. No class, no per-class thresholds, no burst heuristics. RTT statistics exist only as diagnostics and as input to the *recommendation* UI and the frontend-delay latch (which has explicit hysteresis: ≥12 samples, 1-frame-per-0.15/0.30 s slew). | §2.9.3 |
| INV-4 | **Nothing may shrink the effective rollback budget behind the user's back.** If the user grants R frames, the engine uses all R before holding. | `R_local` is read in exactly one comparison: `speculativeFrames() >= R_local ⇒ Stall`. No other code path reads or scales R. Unit test pins this (T-ENG-9). | §2.7.4 |
| INV-5 | **One speed authority.** No stacked actuators (tick scale × pacer scale × freeze pulses). | `FrameScheduler` is the only component that owns wall-clock timing. `Hook_GetTick` pinned to scale 1.0 permanently (kept installed only as insurance/compat shim). The game's busy-spin limiter is detoured out. Pace slew is applied *inside* the scheduler's period, nowhere else. `SetNetplayTickScaleTarget` callers are deleted. | §2.8.1 |
| INV-6 | **Delay-policy contradictions are surfaced, never covertly corrected.** | `DelayPolicy` computes directional coverage (QOH99 `PacingBudget` math) and classifies FullSpeed/Marginal/Underbuffered; the verdict is shown in the config screen and as a persistent HUD badge during play. The engine honors the user's numbers exactly; no pacing layer "compensates". | §2.9.2 |
| INV-7 | **Frame acceptance is never keyed on an implicitly allocated per-side stream counter.** | Frontend frame packets carry explicit `(epoch, phase_id, frame_index)`. The per-epoch `serial` allocator is deleted. A receiver seeing a coherent-but-unexpected `(epoch, phase_id)` sends `ResyncRequest` (interrogation), never silently drops into starvation. | §3.4, §4.5 |
| INV-8 | **One frontend sync machine per epoch.** | Epoch adoption (`EpochAlign` commit) cancels every deferred/pending frontend `Begin` from prior epochs; deferred Begins are tagged with their epoch and dropped on mismatch. `FrontendInputSync_Begin` asserts no machine is active for the epoch and fail-logs otherwise. | §4.5 |
| INV-9 | **Transition barriers commit in a fixed, both-or-neither order.** | Match-end ladder is a strict sequence gated by the match director: `WinScreenExit` → `PostMatchDecision` → `EpochAlign(new epoch)`. A commit for step N+1 is refused (held, re-acked) until step N is committed locally. TransitionBarrier's propose/ack (250 ms resend, idempotent) provides both-or-neither per step. | §4.4 |
| INV-10 | **No peer enters a synced frontend phase while the other is in a different mode.** | `EpochAlign` barrier payload carries `{epoch, first_phase, native_mode}`; frontend input exchange for an epoch is gated on its commit, and commit requires both sides to report the same `first_phase`. Cross-phase adoption routes through EpochAlign, never straight into input exchange. | §4.5 |
| INV-11 | **"Peer silent" is never diagnosed from a filtered stream.** Starvation with a healthy transport is a protocol fault → interrogate, don't time out. | If a frontend phase receives zero accepted remote frames for 2 s (120 lockstep ticks) while `ConnectionSupervisor` reports Healthy/Degraded, the side sends `ResyncRequest{epoch, phase_id, local_frame, mode}`; the peer replies with its own identity tuple; mismatch triggers a fresh `EpochAlign` re-run. Three failed cycles (≈6 s) → pregame restart under a fresh epoch (§4.6). Never a disconnect. | §4.6, §5 row F-3 |
| INV-12 | **A recoverable frontend timeout never destroys the session.** | The only network-originated teardown call sites are: supervisor Dead, confirmed desync, protocol violation (input conflict/invalid), user cancel/quit. `tools/check_killpaths.ps1` (shipping) is extended to allowlist exactly these; CI fails on any new `HandleDisconnection`/`OpenDisconnectError` site. Frontend recovery = pregame restart under fresh epoch on the *live* connection. | §4.6, §6 M5 |
| INV-13 | **Every negotiated value is echoed back verbatim and verified.** | Handshake (§4.2) step k+1 echoes every field of step k; mismatch fails closed *at the handshake* with a self-describing reason. The `shared=2` class of bug (computed-instead-of-echoed) is structurally impossible: confirm packets are built from the received bytes, not recomputed. | §4.2 |
| INV-14 | **Liveness = transport-level silence, never app-payload silence.** | Supervisor input is `transport2`'s `protocol_silence_ms` (time since *any* valid ENet datagram: acks, pings, fragments included). App-level quiet during user-paced screens cannot escalate. Thresholds: Healthy <1 s, Degraded 1–5 s, Interrupted 5–20 s, Dead ≥20 s — sane because ENet pings every 150 ms, so 1 s of protocol silence is genuinely abnormal. | §2.4 |
| INV-15 | **One canonical frame counter per session.** Never resets — not per match, not per round. Epochs qualify *state identity*, not time. | `RollbackEngine` owns a u32 canonical frame with wrap-safe half-open arithmetic (`frame_arithmetic.h`, RFC-1982 style). Game-side per-match counters are mapped via `epoch_frame_origin` (§2.7.2). No code may write the canonical counter except `advance/commitReplay`. | §2.7.2 |
| INV-16 | **No wall-clock in simulation decisions.** | All sim-affecting timeouts are frame-counted (continue prompt 3600 lockstep frames; barriers resend on wall clock but *commit* on both-acked state). Wall clocks drive pacing, telemetry, liveness only. Grep-gate in review: `timeGetTime|QueryPerformanceCounter|GetTickCount` forbidden in `rollback/engine2/*` and all sync decision code. | §2.7, §2.8.1 |
| INV-17 | **The pacer owns the clock.** Busy-spin limiter killed; `Hook_GetTick` pinned to 1.0; clock state rebases, never resets. | §2.8.1–2.8.3. The DECOMP §2.3 backward-snap class (virtual clock ahead of game timestamps → multi-second freeze) is eliminated because no virtual scaling exists; `ResetNetplayTickScaleState` is rewritten to rebase `last_real_tick_ms` and preserve `virtual_tick_ms` even in the pinned shim (belt and braces). | §2.8 |
| INV-18 | **Capture-once immutability.** Exactly one physical input sample per visible source frame; assigned slots are immutable forever. | `captureLocalInput(sourceFrame, value)` adopts (never resamples) on repeat calls during stalls; slot writes assert emptiness. This is the single invariant the whole engine leans on (QOH99 §2.2). | §2.7.3 |
| INV-19 | **Never fabricate inputs.** Predict-and-correct or stall; zero-padding a gap is forbidden; a conflicting actual for an already-actual frame is a fatal protocol violation, never masked. | `receiveRemoteInput` typed results: `Conflict` and `InvalidValue` end the session (fail-closed terminal with reason). Lockstep gaps hold; TrialNetplay's "guaranteed silent desync" lesson (§5-Adopt-2). | §2.7.3 |
| INV-20 | **Fail closed, loudly, with self-describing terminals.** | Every terminal carries `{code, reasonId hash, bounded human string}` on the wire (Disconnect payload), is sticky, and is resent 100 ms until acked. Desync writes evidence dumps on both machines before terminating. | §2.6.4, §5 row D-1 |
| INV-21 | **The visible frame is never optional.** All batch/budget bounds govern *hidden* work only. | Catch-up/resim budget overrun collapses the batch so the current iteration becomes the visible frame — never "break and draw nothing" (QOH99 lesson 5: two independent bugs each produced "sim 62 fps, one drawn frame per 80 s"). | §2.8.6 |
| INV-22 | **Hash membership is sim-affecting state only.** | Palettes (render-only per the in-memory palette pipeline notes), cosmetic RNG, timing/pacing state, audio bookkeeping are excluded from baseline digests and sync hashes. Region-role audit is a named migration task (M4-6). QOH99 lesson 12 + TrialNetplay pitfall 1. | §2.7.7 |
| INV-23 | **Delay and rollback are peer-local knobs, never negotiated.** Your rollback protects you; your delay protects the opponent. | Coverage is directional (§2.9.1); the peer's D/R are advisory HUD data carried in PressureReport-adjacent fields, never applied locally. The min-of-both-requests negotiation (TrialNetplay-style) is explicitly NOT adopted. | §2.9 |
| INV-24 | **A stall must not stop feeding the opponent.** | The stalled-producer (`produceLocalInputAhead`) keeps sealing one local input per frame period while the sim holds, bounded by the peer's advertised consumption capacity, so bilateral stall cascades cannot form (QOH99 Rev2 §6.1, lesson 11). | §2.7.3 step P |
| INV-25 | **Speculation never crosses an irreversible/lifetime boundary.** | Exact-input windows: match-end handoff into winscreen and the destructive commit tick(s) run lockstep; a predicted frame that would flip mode/substate or free resources is vetoed and held (TrialNetplay pitfall 3; QOH99 §4.4 narrowing method — start narrow, widen only with decomp evidence). | §2.7.6 |

---

## 2. Target architecture

### 2.1 Module diagram

```
                     ┌─────────────────────────────── KEEP (frontend / policy) ───────────────────────────────┐
                     │ netplay_menu_controller/ui · charsel/stagesel/winscreen_sync · frontend_input_sync      │
                     │ continue_flow · transition_barrier · connection_supervisor · barrier_protocol           │
                     │ match_lifecycle · sync_policy · delay_policy · palette runtime/storage · set_tracker    │
                     │ pause_handler · churn_pause · nat_traversal · game_settings_sync · HUD/UI · training    │
                     └───────────────▲───────────────────────────▲───────────────────────▲────────────────────┘
                        Session_* /  │        PregameSync_* API  │      OnlineWiring_* / │ RollbackSession_* facades
                        callbacks    │        (preserved)        │      (preserved)      │ (preserved)
┌────────────┐   ┌──────────────────┴───┐   ┌────────────────────┴──┐   ┌────────────────┴─────────────────────┐
│ transport2 │◄──┤ session2             │◄──┤ match_setup           │◄──┤ match_director                        │
│ (ENet wkr, │   │ (Session_* facade,   │   │ (PregameSync_* facade,│   │ (OnlineWiring_* facade, lifecycle     │
│ ev queue,  │   │ state machine,       │   │ 5-step handshake,     │   │ events, startup barrier, teardown,    │
│ silence    │   │ handshake transport, │   │ epoch machine,        │   │ spectator/replay push, palette hooks) │
│ metric,    │   │ packet_router owner) │   │ EpochAlign, config/   │   └────────────────┬─────────────────────┘
│ *ForHost)  │   └──────────┬───────────┘   │ load/baseline barriers│                    │ drives
└────────────┘              │               └───────────────────────┘   ┌────────────────▼─────────────────────┐
                            │ dispatch                                  │ RollbackEngine (rollback/engine2)     │
                 ┌──────────▼───────────┐                               │ input_timeline+prediction (resurrect) │
                 │ packet_router        │──── InputStream/SyncHash ────►│ StateHistory/game_snapshot (reuse)    │
                 │ (single owner, both  │                               │ capture-once → delay → predict →      │
                 │ regimes, deferred    │                               │ mismatch → restore/replay → confirm   │
                 │ flush)               │                               └────────────────┬─────────────────────┘
                 └──────────────────────┘                                                │ sims-per-pass plan
                                                                        ┌────────────────▼─────────────────────┐
                                                                        │ FrameScheduler (patches/frame_        │
                                                                        │ scheduler): QPC absolute deadlines,   │
                                                                        │ limiter detour, dispatcher N∈{0,1,1+k}│
                                                                        │ debt ledger, one-sided pace slew      │
                                                                        └──────────────────────────────────────┘
```

New modules (bold = new file): **`net/transport2`**, **`net/session2`**, **`net/packet_router`**,
**`net/match_setup`**, **`rollback/match_director`**, **`rollback/engine2`** (+ resurrected
`input_timeline`, `prediction`), **`patches/frame_scheduler`**, **`net/frame_arithmetic.h`**,
**`net/time_probe`** (µs RTT estimator), **`rollback/run_state.h`** (typed run state + hold ledger).

Deleted: `gameplay_bridge`, `frame_lineage`, GekkoNet (lib + CMake + `rollback_session.cpp`
internals), `netplay_pacing`'s controller (classifier/debt/holds — see deviation note §2.8.7),
`network_thread`, `session_manager` internals, `pregame_sync`/`match_bootstrap` internals (facades
preserved), the delay-change wire flow.

### 2.2 transport2 (ENet worker)

Replaces `network_thread` + the single-peer paths of `enet_transport`. Contract consumers:
session2 (events), connection_supervisor (stats), spectator sidecar (`Transport_*ForHost` helpers,
preserved verbatim).

- **Threading:** one worker thread owns the ENet host; `enet_host_service` at 1 ms cadence. SPSC
  queues both ways: outbound send queue (game thread → worker), inbound event queue (worker → game
  thread). The game thread never touches ENet objects. All packet callbacks fire on the game thread
  from `Session_Update()` drain — same as today, preserving every reentrancy assumption in KEEP code.
- **Channels (unchanged):** 0 = reliable-ordered control; 1 = unreliable-sequenced gameplay
  (InputStream, TimeProbe); 2 = unreliable debug. Note on ch1 sequencing: ENet drops late-arriving
  older unreliable-sequenced packets — harmless by design, because every InputStream packet carries
  the full redundant window, so any newer packet supersedes any older one (§3.2).
- **Peer resilience config (keep from M2 resilience work):** `enet_peer_timeout(0, 10000, 30000)`,
  `enet_peer_ping_interval(150)`, unreliable throttle pinned (ENet must never manufacture loss and
  its own timeout verdict stays behind the supervisor's).
- **`protocol_silence_ms` metric (new, INV-14):** worker stamps `last_valid_datagram_qpc` on every
  ENet event *and* every raw-socket intercept accept (autopunch keepalive included); exposed via
  `Transport2_GetStats()` → consumed by connection_supervisor in place of
  `Session_GetMsSinceLastInbound` app-level counting (that function is re-implemented on top of the
  same metric so the header contract survives).
- **NAT layers (keep verbatim):** hole-punch bursts, autopunch relay keepalive (2 s, 12-byte
  authenticated), mid-match rebind healing (rewrite `peer->address` under the existing four guards),
  egress fault injection (`Transport_FaultInjectionActive` query preserved for `rematch_soak`).
- **Liveness anchor (QOH99 lesson 10):** the pre-establishment liveness budget is anchored at the
  first worker poll after `Session_StartHost/Join`, never process start — no false instant timeouts.
- **Drain-before-judge (QOH99 lesson 10):** `Session_Update()` drains the inbound queue *before*
  supervisor evaluation runs in the same frame (ordering fixed in `ModOnFrame`).

### 2.3 packet_router (single dispatch owner)

Extraction of `online_wiring`'s `OnGameplayPacket` (~300 lines, 22 cases) + `pregame_sync`'s
callback into one owner registered once at session start and never handed off. This kills the
callback-handoff race class outright (palette packets 50–52 today must be routed by BOTH regimes;
with one owner the requirement is trivially met).

- Routing table (packet type → handler), regime-independent. Handlers: engine ingest
  (`InputStream`, `SyncHash*`), match_setup (handshake, config, load, baseline), frontend lockstep
  (`CharSelFrameInput`, `WinScreenFrameInput`, `CharSelLock`, `StageSync`, `FrontendPhaseBarrier`,
  `FrontendBoundaryDigest`, `ResyncRequest/Reply`), transition_barrier (`PhaseTransitionProposal/Ack`),
  palette (50–52), supervisor (`Ping`), pause/churn, NAT signaling, spectator probe, debug (ch2).
- **Deferred flush preserved:** reliable packets arriving before a handler is installed are queued
  (bounded 256 packets / 256 KB) and flushed on install, never dropped — existing semantics of
  `Session_SetPacketCallback`, kept behind the same API.
- Unknown packet type: log + count, never terminal (forward compat within a protocol version).

### 2.4 session2 + connection_supervisor

`session2` re-implements `session_manager` behind the **verbatim preserved** `Session_*` header
(inventory §2.1, all 17 functions + types byte/shape-stable). Internal changes:

- Single-owner state machine on the game thread: `Idle → Connecting → HandshakeV2 → Connected →
  Ready → InSession → Closing → Idle/Failed`. All transitions logged with reasons.
- The 5-step nonce handshake (§4.2) runs here, before any `PregameSync` activity.
- **Teardown funnel:** exactly one function `Session2_Terminate(TerminalReason)` executes network
  teardown; its callers are the INV-12 allowlist. Graceful path keeps the shipping behavior:
  `Session_NotifyGameExit()` → reliable Disconnect (reason=UserCancel "Peer closed the game"),
  bounded 150 ms wait, then destroy.
- **connection_supervisor (KEEP, re-fed):** thresholds re-based on `protocol_silence_ms` (INV-14):
  Healthy <1000 ms, Degraded 1000–5000, Interrupted 5000–20000, Dead ≥20000. Heartbeat unchanged
  (reliable Ping every 250 ms once silence ≥500 ms). Dead fires the single teardown exactly once.
  Supervisor gains one new input: the **progress deadline** (QOH99 §3.6) — during active gameplay
  (rollback session running, not ExternalSuspension), zero canonical-frame progress for 8 s → HUD
  warning "opponent's game stopped responding", 20 s → Dead-equivalent teardown with distinct
  reason `ProgressDeadline`. This catches a wedged-but-pinging peer; it cannot fire on user-paced
  screens because frontend lockstep exchanges frames continuously and gameplay holds still make
  progress the moment inputs arrive.

### 2.5 match_setup (replaces pregame_sync + match_bootstrap)

One phase machine behind the **preserved 12-function `PregameSync_*` API** (inventory §2.2;
`PregamePhase` enum values kept as the reporting vocabulary, internal states may differ).
`MatchBootstrap_*` is retired (was internal to the doomed set).

Responsibilities:

- **Epoch authority.** Epochs are session-scoped u32 generations minted by the **host**, strictly
  increasing, starting at 1 (0 reserved = "no epoch"). Full-width transport-owned values — never a
  game-local counter, never derived from timers (TrialNetplay pitfall 5; the fatal 0x6C542396 epoch
  in the field logs was a timestamp-derived value). Epoch rotation points: session start (epoch 1),
  every `PregameSync_Begin` (charsel path), every `PregameSync_BeginRematch` (continue fast path),
  and mid-set config-affecting changes if ever added (palette change is render-only → NOT an epoch
  event, INV-22).
- **EpochAlign barrier** (new TransitionBarrier kind): payload `{epoch, first_phase, native_mode}`.
  Committed both-or-neither before any frontend input exchange in the epoch (INV-10). Commit
  cancels all deferred frontend Begins from prior epochs (INV-8).
- **Deterministic phase schedule.** Per epoch, the frontend phase sequence is a fixed table keyed
  by entry kind: `CharselEntry → [CharSel, StageSel]`, `RematchFastPath → []` (no frontend phases;
  straight to config), `PostMatch → [WinScreen]`. Both sides derive the same table from the
  committed EpochAlign `first_phase` — no implicit allocation (INV-7).
- **Config exchange:** `LockedMatchConfig` (20-byte, byte-stable, KEEP) exchange + hash agreement;
  host mints `session_seed`/`rng_seed`; `GameSettingsSync_ApplyLockedConfig`,
  `NetplayPaletteRuntime_OnLockedMatchConfig` fire at ConfigAgreed exactly as today (§4.3).
- **Load barrier + baseline rendezvous:** load-complete is a TransitionBarrier commit; then the
  **baseline digest rendezvous** (reusing `baseline_sync` CRC breakdown) — both sides exchange the
  typed digest of sim-affecting state and gameplay opens only after both ack the same value
  (QOH99 layout-rendezvous pattern; INV-22 governs membership). Digest mismatch → dump both sides
  (`baseline_sync` breakdown compare, shipping) → **retry load once** under the same epoch; second
  mismatch → fail-closed terminal `BaselineMismatch` (a genuine build/content divergence).
- **GameplayStart barrier:** final TransitionBarrier commit; on commit, hand
  `{epoch, epoch_frame_origin, LockedMatchConfig, seeds, baseline snapshot}` to match_director →
  engine arm/rotate. Frame origin = current canonical frame (the counter does NOT reset, INV-15).
- **Rematch fast path preserved:** `PregameSync_BeginRematch(prevConfig)` keeps its contract from
  the continue-screen plan (skip charsel; host mints fresh seeds; straight to ConfigExchange) —
  now implemented as: rotate epoch → EpochAlign(first_phase=None) → ConfigExchange → Load →
  Baseline → GameplayStart. The freeze-coverage extension from the continue plan (§ "Freeze
  coverage gap") carries over: load-barrier freeze applies whenever the fast path is active and the
  game reaches mode 8 sub 3 before GameplayStart commits.
- **Cross-phase announce recovery (keep from M1 resilience):** a `SyncAnnounce` arriving in
  Idle/GameplayHandoff restarts pregame on BOTH roles (host restart shipped in M1); under the new
  model "restart" = fresh epoch + EpochAlign, on the live connection.
- **Handshake-adjacent timeouts (keep M1 semantics):** phase timeouts only fire after base timeout
  AND ≥2 s of *protocol* silence; while the peer provably transmits, extend to the 45 s hard cap;
  on cap → pregame restart (fresh epoch), NOT teardown (INV-12).

### 2.6 match_director (replaces online_wiring)

Behind the **preserved `OnlineWiring_*` surface** (inventory §2.3, all 13 functions). Thin by
design; owns ordering, not policy.

1. **Startup:** consumes GameplayStart commit → `RollbackEngine_Arm(epoch, origin, config, seeds,
   baseline)` → `RollbackAudio/ComboFx/StatusFx_OnSessionBegin(R_local)` →
   `SpectatorRuntime_OnMatchBegin(config)` → `MatchLifecycle` events → release startup freeze
   (`InputSyncHooks_SetLoadBarrierFreeze(false)`).
2. **Per-frame pump order (in `ModOnFrame` / dispatcher hook):** drain transport → packet_router →
   supervisor evaluate → transition_barrier `FrameUpdate` → match_setup `FrameUpdate` → engine
   (via input_override dispatcher contract) → scheduler wait. Drain-before-judge is this ordering.
3. **Match end:** `OnMatchEnd` → engine keeps running the same timeline through winscreen exact
   lockstep (no engine teardown between matches — the whole point); spectator `OnMatchEnd`;
   `SetTracker` update; hand control to winscreen_sync + continue_flow (KEEP, unchanged).
4. **Teardown (session end only):** engine end → fx `OnSessionEnd(reason)` → palette
   `OnDisconnect(reason)` + `NetplayPaletteStorage_ClearCache()` → `MatchLifecycle_OnDisconnect` →
   spectator `OnDisconnect` → session2 terminate. One function, one order, logged.
5. **Epoch rotation execution** at the rematch boundary: rotate savestate tags, invalidate all
   StateHistory slots, re-baseline, rotate audio-journal epoch — the AS2 analog of
   `reregisterFrameData()`, executed at the GameplayStart commit of the new epoch so setup and
   frame zero share one epoch.
6. **Queries preserved:** `IsGameplayActive` (churn_pause), `IsStartupReleased`,
   `IsGameplayEntryAdvanceBlocked` (input_override), `GetSnapshot` (rollback_debug).

### 2.7 RollbackEngine (`rollback/engine2`) — full specification

Socket-free, clock-free, game-memory-free core (like QOH99 `RollbackSession`): it owns *no* ENet,
no QPC, no game addresses. It decides which canonical inputs a frame consumes and when the caller
must stall or restore/replay. The game adapter (input_override dispatcher branch + match_director)
and the unit/soak harness exercise the identical object. Exposed behind the **preserved
`RollbackSession_*` facade** (inventory §2.4, every listed function; `RollbackSession_BufferGekkoPacket`
deleted; telemetry fields renamed `gekko_*` → `link_*`).

#### 2.7.1 Configuration

```cpp
struct EngineConfig {
    uint8_t  local_player;        // 0/1 from PlayerMapping_DeriveFromRole
    uint8_t  input_delay;         // D_local, 0..15, peer-local (INV-23)
    uint8_t  max_rollback;        // R_local, 1..15, peer-local; sole use: the INV-4 comparison
    uint16_t neutral_input;       // 0x0000 (0xFFFF is unrepresentable: it is the game's
                                  // "no input yet" history marker — reject on receive, INV-19)
    uint32_t first_frame;         // canonical frame at arm (session start: 0; epochs never reset it)
    uint32_t max_remote_future;   // 120: remote input beyond frontier+120 is dropped (bound memory)
    uint32_t history_capacity;    // 4096 input records per side (ring)
};
```

#### 2.7.2 Canonical frame counter and epoch mapping (INV-15)

- One `uint32_t` canonical frame; all frontiers half-open; comparisons via `frame_arithmetic.h`
  (`frameAfter`, `forwardDistance`, `signedLead` — wrap-safe, ported 1:1 from QOH99
  `net/FrameArithmetic.h`). Never resets for session life.
- Per epoch, match_setup supplies `epoch_frame_origin`. Game-side mapping:
  `game_abs_frame = canonical_frame − epoch_frame_origin`. The game's own counters
  (0x816490/0x816494/0x816498) keep their per-match reset semantics via `Netplay_InitialSync` — the
  engine writes through them exactly as the current dispatcher does (write idx bump bounded by
  `INPUT_HISTORY_MAX` 216 000; a 61-minute match hits the vanilla round-timeout force-end at
  215 900 display frames first, so the cap is unreachable in practice).
- `RollbackSession_GetFrameOriginAbs()` / `GetCurrentGameAbsFrame()` keep their meaning through
  this mapping (fx journals and HUD depend on them).

#### 2.7.3 Input pipeline (capture → delay → exchange → predict → confirm)

- **C — Capture (INV-18).** `captureLocalInput(sourceFrame, value)`: exactly one physical SDL-layer
  sample per visible source frame (sampled at `BeginFrame`, immune to the vanilla latch-loss class,
  DECOMP §1.4). Schedules the word at `sourceFrame + D_local`. Repeat calls while stalled **adopt**
  the existing record; slots are immutable forever.
- **D — Delay = pure relabel.** A neutral prefix of `D_local` frames is queued at arm. Raising D
  mid-session fills only missing future slots with neutral; lowering drains the longer pipeline at
  a fully-confirmed boundary without recapture (QOH99 §5 model; local hotkeys `-`/`=`, no wire
  message — INV-23). Drain timeout 5 s → revert to proven old value.
- **X — Exchange.** Sealed local records → `InputStream` packets (§3.2): sent on every newly sealed
  input (≈60 pps during play) plus a 50 ms idle resend while the un-acked suffix is non-empty.
  Window anchored at `peer_ack_through + 1` — retransmits exactly the un-acked suffix; a hole
  refills with no NACK. Receive: `receiveRemoteInput(frame, value)` with typed results —
  `Applied / DuplicateIdentical (ignore) / Conflict (fatal, INV-19) / InvalidValue (fatal: 0xFFFF or
  bits outside the AS2 button mask) / TooFarFuture (drop, >max_remote_future) / Stale (ignore,
  ≤ contiguous prefix)`.
- **P — Producer while stalled (INV-24).** While the sim holds, keep sealing one local input record
  per canonical frame period (scheduler supplies the cadence tick; the engine itself stays
  clock-free). Bound: `min(peer_advertised_rollback + peer_advertised_delay + 2, 30)` frames ahead
  of `peer_ack_through` — 30 = `kInputWindowFrames − 2`, guaranteeing the un-acked suffix always
  fits one packet (structural invariant: `produced_through − peer_ack_through ≤ 32`, asserted).
  Prefers a fresh device sample so presses during a stall survive. **Fenced OFF** during lifecycle
  barriers and frontend phases (frontend lockstep owns its own stream) — but NOT during the
  continue prompt, whose decisions ride the winscreen lockstep stream (unchanged design).
- **R — Predict.** `predictRemote(frame)`: hold-last-actual. A late actual that differs marks the
  **earliest mismatch** (`pending_mismatch = min(...)`). Matching predictions cost nothing
  (retained speculation — no rollback occurs unless a prediction was actually wrong).
- **F — Confirm.** A frame is confirmed when every remote input ≤ frame is actual AND any mismatch
  replay covering it has committed. `popConfirmedFrame()` yields `{frame, inputs[2], pre_state_hash}`
  in order; this single immutable seam feeds (a) SyncHash exchange, (b) `SpectatorRuntime_On*`
  push, (c) the replay recorder. A predicted value can never enter a file or a hash.

#### 2.7.4 Per-opportunity decision — `nextAction()` (INV-1, INV-4)

Evaluated once per scheduler pass (and re-evaluated between batch iterations):

```
pending_mismatch                            -> Rollback{from = pending_mismatch,
                                                        replay_until = sim_frontier}   (runs FIRST)
lifecycle_exact_boundary && !remote_actual  -> Stall(LifecycleBoundary)                (no debt)
!local_input_ready                          -> Stall(LocalInputMissing)                (not a network wait)
speculativeFrames() >= R_local              -> Stall(PredictionLimit)                  (debt CreateOne)
else                                        -> Advance{frame, inputs[2]}
```

`speculativeFrames() = forwardDistance(remote_actual_prefix, sim_frontier)`. This is the ONLY
consumer of `R_local` and the ONLY network-driven hold in the whole system. There is deliberately
no "we lead the peer" hold (QOH99 removed theirs; rationale preserved in their code) and no
debt/gap/class inputs to this decision.

#### 2.7.5 Rollback as an explicit transaction

`beginRollback(from)` → adapter restores the StateHistory snapshot tagged
`{epoch, frame=from, phase}` (tag mismatch = fail-closed, never restore a stale-epoch slot) →
per replayed frame: adapter runs the game tick with canonical inputs under resim conditions
(§2.8.6) and calls `commitReplayFrame(frame, post_hash)` → `finishRollback()`. Boundary rule: if a
replayed frame flips mode/substate earlier than the speculative timeline did, `finishRollbackAtBoundary()`
discards the speculative suffix and truncates the frontier there; if an *uncommitted* replay tick
would cross a lifetime boundary without full actual prefix, stop before it
(`finishRollbackBeforeBoundary()`). No local capture is legal inside the transaction (asserted).
Discarded speculative timelines invalidate their StateHistory slots and audio-journal entries.

#### 2.7.6 Exact-input lifecycle windows (INV-25)

Start from the narrow set and widen only with decomp evidence (QOH99 §4.4 method — read
`NetplayLifecyclePolicy.h`'s evidence chain before touching this):

- **Exact (lockstep) always:** everything outside mode 8 sub 3 gameplay — charsel, stagesel,
  winscreen (mode 9 all subs, incl. the continue prompt), loading, pregame. These run on the
  frontend lockstep stream with the RTT-derived frontend delay (§2.9.3), same canonical timeline.
- **Exact inside gameplay (initial set):** (a) the match-end handoff tick(s) into mode 9 (protects
  the epoch-rotation/asset-lifetime boundary — AS2 analog of QOH99's roundCounter==5 window);
  (b) the round-result destructive commit tick if the decomp audit (M4-7) finds one that frees or
  reloads resources. Round-to-round boundaries within a match are **predicted normally** —
  TrialNetplay proved nothing is freed at round end in this engine family; M4-7 verifies for AS2
  before enabling.
- Predicted frames are vetoed (engine returns `Stall(LifecycleBoundary)`) whenever the *next* tick
  matches an exact-window predicate and the remote actual is missing.

#### 2.7.7 Savestates, hashing, desync (INV-22)

- **Ring:** reuse `StateHistory` (`resimulation.cpp`) over `game_snapshot` regions. Capacity 64
  slots (R_max 15 + catch-up headroom + safety; slot = `frame % 64`), full-copy pre-tick save every
  rollback-eligible frame: save → simulate → commit. Every slot tagged `{epoch, frame, phase}`;
  restore validates the tag (fail-closed). FPU/MXCSR control words captured per slot and restored
  (TrialNetplay pins 0x027F; AS2 must capture-and-restore rather than assume).
- **Save budget:** measured in M4 microbench; acceptance p99 save ≤ 1.5 ms, restore ≤ 1.5 ms on the
  min-spec machine (§7.5). If exceeded: prune regions (the game_snapshot fixed ranges are already
  minimal) before any design change.
- **Hash:** `hashConfirmedPreState()` over the same bytes the snapshot saves, **sim-affecting
  regions only** (INV-22: palettes/cosmetic RNG/timing state excluded — palette banks are
  render-only per the in-memory palette pipeline notes, so they never enter any digest; the QOH99
  audio-manifest lesson applies verbatim). Hash primitive: 64-bit block digest (Block64-style;
  FNV-1a cost 13–19% of frame budget on this class of target — do not use it per-frame).
- **SyncHash cadence:** every **30 confirmed frames**, submit
  `{epoch, frame, gameplay_hash u64, rng_state u32, hp0, hp1}` (rng/hp are diagnostics;
  `gameplay_hash` is authoritative). Peer hashes queue (bounded 128) and compare only when local
  confirmation reaches the frame; comparison exact across all fields. Epoch relations: stale →
  ignore, future → queue, inconsistent (same frame, different epoch lineage) → protocol-violation
  terminal. No zero/wildcard hashes are ever sent.
- **Mismatch:** `desync_dump` region-level evidence written on BOTH machines (whoever detects,
  decides; terminal repeated reliably) → fail-closed terminal `ConfirmedDesync` (INV-20). Plus the
  TrialNetplay **resim self-check** as a debug-build option: re-save the same frame after replay
  with identical inputs; differing hash = non-deterministic resim, caught on a single instance.

#### 2.7.8 Facade mapping (inventory §2.4 — every symbol accounted)

| Facade symbol | engine2 implementation |
|---|---|
| `RollbackSession_IsActive/IsSessionRunning` | armed && not terminal |
| `RollbackSession_BeginFrame(localInput)` | capture C + scheduler plan for this pass |
| `RollbackSession_ProcessNextEvent` → Advance/Done/Error | drains the pass plan: rollback transaction steps, then advances (two-phase dispatcher contract preserved for input_override) |
| `RollbackSession_GetAdvanceInputs(p1,p2)` | canonical inputs of the frame being advanced |
| `RollbackSession_HasPendingFrame/PollSession` | plan non-empty / pump + terminal check |
| `RollbackSession_End` | disarm (director-ordered teardown only) |
| `RollbackSession_FramesAhead` | `signedLead(local_produced, remote_produced)` — **telemetry only**, consumed by HUD; no decision reads it |
| `RollbackSession_GetCurrentFrame/GetFrameOriginAbs/GetCurrentGameAbsFrame` | §2.7.2 mapping |
| `RollbackSession_GetRollbackBudget` | R_local |
| `RollbackSession_GetTimesyncTelemetry` | `link_avg_ping/link_jitter` (from time_probe), depth, confirmed frontier, run state — feeds HUD/sync_trace; the pacing consumer is gone (§2.8.7) |
| `RollbackSession_IsRollingBack/ShouldSuppressSideEffects` | inside rollback transaction (fx journals) |
| `RollbackSession_ComputeLiveStateChecksum` | live-region hash (rollback_debug) |
| `RollbackSession_GetErrorReason/GetSnapshot` | terminal reason string / snapshot struct (renamed fields) |
| `RollbackSession_IsPeerInterrupted` (0.7 addition) | supervisor Interrupted verdict passthrough (keeps input_override's abort-gating sites compiling) |
| `StressHooks_*` query points | called at ingest/egress/prediction exactly as today (test methodology preserved) |
| `DelayPolicy_OnRollbackApplied/IsRollbackSynced` | fed from `finishRollback` |

### 2.8 FrameScheduler (`patches/frame_scheduler`) — the mod-owned clock

Implements DECOMP_TIMING_STUDY §5 recommendation **(b) + (a)** exactly: an absolute-deadline
scheduler at the limiter site for continuous rate control, with dispatcher-driven 0/1/1+k sim
passes for discrete holds and rollback catch-up.

#### 2.8.1 Ownership (INV-5, INV-17)

- **Limiter detour:** byte-signature the unique `call sub_635F80 / sub / cmp 17 / jl` cluster at
  the bottom of `Game_MainLoop` (0x5D2AC0, decomp L266504–266510) and detour to
  `FrameScheduler_WaitForNextFrame()`. Install-time assert: signature found exactly once, else
  fail loud and fall back (§8.1 risk R-1). The detour runs *after* the game stamps `dword_816360`
  so vanilla FPS bookkeeping stays coherent; the vanilla `jl` is neutralized (fed a satisfied
  condition).
- **`Hook_GetTick` pinned to 1.0:** stays installed as a passthrough (+100 ms delta clamp
  preserved), returning real time to all 20+ consumers in DECOMP §2.2. All
  `SetNetplayTickScaleTarget` / `SetNetplayPacingActive` write paths are deleted.
  `ResetNetplayTickScaleState` is rewritten to **rebase, never reset** (preserve `virtual_tick_ms`,
  re-anchor `last_real_tick_ms`) — even pinned, this closes the backward-snap class forever
  (DECOMP §2.3 #1).
- **Present interval:** proxy scaling chain stays `D3DPRESENT_INTERVAL_DEFAULT` (≙ONE) by default —
  windowed DWM present at 60 Hz + 16.667 ms scheduler is the lowest-jitter combination (DECOMP
  §4.4). New d3d9 proxy config key `present_interval=immediate` (one line, d3d9_proxy.cpp L2307
  site) as the documented escape for non-60 Hz-multiple displays / beat stutter. The scheduler's
  absolute deadlines already treat Present blocking as frame cost, so no handoff protocol is
  needed beyond this.
- `timeBeginPeriod(1)` is already process-lifetime active (DXLib init, DECOMP §2.1) — verified, no
  action needed; scheduler asserts it via `timeGetDevCaps` at install.

#### 2.8.2 Deadline math (concrete)

```
// Cadence profile {num, den} — integer-exact on both peers, carried in handshake, fail-closed
// on mismatch (QOH99 delta #8). Two profiles:
//   proper_60:  period = QPF / 60          (60.000 Hz)      remainder r = QPF % 60
//   compat_58:  period = QPF * 17 / 1000   (58.82 Hz)       remainder r = (QPF*17) % 1000
deadline += period; frac += r; if (frac >= den) { deadline += 1; frac -= den; }   // Bresenham carry
```

- **Wait:** `while (deadline − now > 2 ms QPC-equivalent) Sleep(1);` then spin (`_mm_pause`) to the
  deadline. Worst-case oversleep ≈1–2 ms absorbed by the spin tail. CPU: ~2% vs today's 100% core.
- **Sub-frame lateness:** remainder preserved — the next wait shortens; long-term rate is exact
  (zero cumulative drift by construction).
- **Multi-frame lateness (rebase):** if `now > deadline + 2·period`: `rebased = floor((now −
  deadline)/period)`, `deadline = now`, report `rebased` to the debt ledger (§2.8.5). Visible
  cadence is **never compressed** to repay lateness (INV-21 corollary; QOH99 lesson 3).
- **`semanticHold()`:** an intentional 0-sim pass consumes its pacing slot at the normal deadline
  without any deadline mutation — no permanent slow-motion, no burst-on-resume.
- **QPC regression / dead clock:** QPC going backward → reset spacing state, log, continue
  visibly-smooth; 500 ms with no deadline progress while a session runs → latch fault, terminal
  `PacingClockDead` (fail closed, INV-20).
- **Pace-slew application point:** `effective_period = period · (1 − slew_ppm/1e6)` — §2.8.4;
  applied to the *next* deadline increment only (continuous, no jumps).

#### 2.8.3 Sims-per-render dispatch (the DECOMP §6 contract)

Decision made once, **before** the pass, in the dispatcher hook (`Hook_InputDispatcher` /
`RollbackSession_BeginFrame`), N ∈ {0, 1, 1+k}:

- **N=0 (hold):** dispatcher returns −1 AND `Hook_AdvanceFrame` suppresses the budget grant AND
  vanilla timeout counters are cleared (0x8EA200/0x8EA3A8) — all three, every hold pass (DECOMP
  §6.1 rules; missing the second half causes engine self-catch-up bursts). Render re-presents the
  frozen state; packets keep pumping; local capture keeps running via the producer (INV-24).
- **N=1:** the normal case.
- **N=1+k (hidden catch-up):** `k = min(owed_debt, kMaxCatchupExtraPerPass=2, R_local − depth − 1)`.
  The `−1` reserves the visible frame's own prediction slot (QOH99's "62 fps sim, one drawn frame
  per 80 s" bug class). Hidden frames consume already-sealed local inputs (producer output — never
  resampled, INV-18) and predicted remote inputs (catch-up must work on predicted input, QOH99
  lesson 4). AS2 renders once per pass regardless of N (DECOMP §1.2), so hidden frames are
  structurally invisible; SE dedup within a pass is free (per-frame temp block, DECOMP §6.2).
- **Batch budget:** hidden work wall budget `6000 µs` per pass, re-checked between iterations; on
  overrun the batch **collapses** so the current iteration becomes the visible frame (INV-21);
  partial batches still credit the hidden frames they ran (un-crediting is a self-sustaining trap).

#### 2.8.4 Bounded one-sided pace slew (the ONLY wall-clock regulator)

Purpose: equalize prediction-depth imbalance (one peer consistently deeper than the other —
crystal drift, load imbalance, weak hardware) without any negotiated timesync.

- Input: fresh peer `PressureReport` (freshness ≤ 4 frames / 64 ms — stale ⇒ instant release).
- Admission: `peer_depth − local_depth ≥ 2` frames (deadband) with hysteresis (release at <1).
- Only the **lower-depth** peer acts, by *shortening* its own period (running slightly fast so the
  peer predicts less). Never lengthen, never slow below native for the other side.
- Bounds: target correction `2500 ppm × depth_lag`, slew-rate rise ≤ `8000 ppm` per step, absolute
  cap `25000 ppm` (2.5% ≈ 0.42 ms/frame). Instant release to 0 on stale/untrusted sample or
  admission loss.
- Determinism unaffected by construction: it changes how fast wall time is spent, never which
  frames run. Runs every pass **including stalls** (the stalled side is exactly who needs releasing).

#### 2.8.5 CadenceDebt ledger

Bounded ledger, max 8. `CreateOne` on `PredictionLimit` holds only. `Consume` by hidden frames
actually executed. `discardExternal()` on focus loss / device churn / churn-pause / load stalls.
`applyRebasedFrames(n)` is the single entry converting deadline-rebase lateness into debt — and
only when the last hold cause was `PredictionLimit` (external stalls stay discarded; **exactly one
repayment owner per missed frame**, QOH99 lesson 3; the Class-A defect — correction lateness
dropped because `lastHold == None` — is why the gate keys on the *typed* cause).

#### 2.8.6 Hidden/resim frame conditions

Rollback resim (correction) executes inside one pass, before the visible frame, via the shipping
`Resim_Execute` path (direct Mode-8 handler calls): per iteration it must (a) clear the per-frame
temp block itself (outer clear runs once per pass — DECOMP §6.2), (b) run under
`ShouldSuppressSideEffects()==true` so `rollback_audio`/`combo_fx`/`status_fx` journal-reconcile
instead of re-firing, (c) have its Presents dropped by the proxy (resim draws cost draw-call time
only), (d) bail at any mode/substate change (`*(game+56) != 3` class of checks) → §2.7.5 boundary
truncation. Corrections run to completion within the pass (atomic); worst case R=15 × ~2 ms ≈
30 ms once ≈ one late frame absorbed by rebase rules — and the default R recommendation is ≤10
(§2.9.2) so the common worst case fits the frame.

#### 2.8.7 Deviation from inventory: `netplay_pacing` is retired, not KA

`FRONTEND_BACKEND_INVENTORY.md` §1 marked `netplay_pacing` KA ("keep, re-fed"). That verdict
predates the fixed decision set: LOG_EVIDENCE proves the module's mechanism (debt controller +
NetQuality classifier + freeze actuator) IS the jitter (98.7% hold rate at 0% loss), and INV-1/2/3
forbid all three of its pillars. Decision: **delete the controller**; keep only its telemetry
naming where useful. Replacements: FrameScheduler (rate), engine `nextAction` (holds), pace slew
(imbalance), run_state ledger (observability). `TickHooks` survives (pinned); `tick_hooks`'s
limiter-preference getters stay for the menu/settings sync. Retired with it: the
`stall_threshold`/`protection_window` session knobs and `DelayPolicy_GetStallThreshold` (its only
consumer was the rewritten input_override netplay branch). `game_settings_sync`'s `frame_timing`
setting survives — it now selects the §2.8.2 cadence profile, which the handshake carries and
verifies (both peers must match, fail-closed).

#### 2.8.8 Spectator pacing (policy port, player path independent)

Spectator playback gets its own elastic policy (port of QOH99 `SpectatorPlaybackPolicy`, pure
header): live target 240 buffered confirmed records (~4 s), rebuffer resumes at 120, auto
catch-up above 300 with hysteresis back to 240. Under low water, **elastic slow-motion instead of
freeze-then-sprint**: 950/850/700 permille playback at 180/120/60 records, Bresenham-distributed
holds. Deep-backlog catch-up ladder: 2/4/8/16/24 ticks per presentation slot by backlog rung,
bounded by a 12 ms wall slice; catch-up ticks never own the cadence slot; the loop yields at any
phase/scene boundary. Every record is hash-verified before and after its tick (S-4).

### 2.9 Delay / rollback knobs — peer-local, never negotiated (INV-23)

#### 2.9.1 Coverage math (port QOH99 `PacingBudget.h`)

- `oneway_frames = ceil(p95_oneway_us / period_us)` from time_probe (§2.9.3), never from ENet ms
  RTT, never from any menu/lobby measurement (QOH99 lesson 7; strong-typed `RouteRttUs` so lobby
  values cannot compile into the formula).
- Directional coverage: `coverage(peer→us) = D_peer + R_local`; `coverage(us→peer) = D_local +
  R_peer`. Your rollback protects you; your delay protects the opponent.
- Requirement per direction: `required = oneway_frames + margin`, `margin = max(2, oneway_frames/3)`.
- Classification: `FullSpeed (coverage > required)` / `Marginal (==)` / `Underbuffered (<)`.
  **Recommend one frame above threshold** — budget==required measurably degrades into a stable
  sub-speed equilibrium (QOH99 lesson 15).

#### 2.9.2 UI/policy behavior (INV-6)

- Config screen shows: measured RTT (µs-derived), oneway frames, per-direction classification for
  the *current* local knobs given the peer's advertised knobs, and the recommendation. The field
  scenario (155 ms RTT, delay 0) renders as: "delay 0: ~5 frames always predicted; R ≥ 6 required,
  R ≥ 7 recommended". Expert values stay allowed — with a persistent `Underbuffered` HUD badge
  when selected. Nothing clamps, nothing silently corrects (INV-6), and nothing punishes: with
  delay 0 / R 8 at 155 ms the engine simply runs depth ≈5 rollbacks continuously at full speed
  (T-PERF-3 pins this exact case).
- Peer knob changes need no notice: raising R_local is immediate; lowering below live speculative
  depth is deferred-and-retried, never forced. D_local changes drain at a confirmed boundary
  (§2.7.3-D). Remote D/R arrive as advisory fields (HUD + coverage math input only).
- `delay_mode=asymmetric_expert` survives as-is — it is *already* the peer-local model.

#### 2.9.3 Frontend delay (exact lockstep regime)

Everything outside rollback gameplay uses an RTT-derived frontend delay (QOH99 §5 two-regime
model; regime keyed on scene: mode 8 sub 3 = gameplay regime, all else = frontend regime):

- Curve: `frontend_delay = ceil(oneway_frames) + 1` (+2 once oneway ≥ 6 frames), floored at
  `D_local`, capped 15.
- Latch: holds baseline until ≥12 time_probe samples exist; rises max 1 frame per 0.15 s; decays
  max 1 frame per 0.30 s (asymmetric toward the cheap mistake — QOH99 lesson 9). Unit-suffixed
  names (`*_us`) with conversions at the caller (lesson 8).
- **time_probe:** `TimeProbe/TimeProbeAck` (§3.2) at 4 Hz on channel 1: opaque QPC µs stamp echoed
  with responder dwell µs subtracted; generation-gated (a stale reply cannot repopulate a reset
  estimator); rolling p95 over 32 samples with local-stall rejection.

### 2.10 Telemetry and run state (build first, not last)

QOH99 lesson 2: label every hold with its true cause from day one. New `rollback/run_state.h`:

- Typed `RunState`: `Running / CorrectionCatchUp / PredictionPressure / ExactLifecycleBarrier /
  ExternalSuspension / EmergencyHold / Terminated` — classified once per pass from the plan,
  observational only, carried in PressureReport (`run_state` byte).
- **Hold-episode ledger:** every hold logs `{cause, start_frame, duration_frames, depth, R}`;
  per-minute rollup line. Native hit-freeze must never count as a hold.
- Structured per-second `STAT` line: sim rate, present p50/p99, holds by cause, rollbacks + max
  depth, slew ppm, debt, silence ms. This is the §7 acceptance instrument; the log format is
  specified in M2 so the harness parses one format all cycle.

---

## 3. Wire protocol v2

### 3.1 Framing, versioning, identity

- Transport framing stays ENet (peer/channel/reliability, CRC and endpoint binding come free).
  App header on every packet (existing `protocol.h` shape): `{type u8, PROTOCOL_VERSION}` plus
  **`session_id u64`** (new: derived from handshake nonces, §4.2) on every gameplay-phase packet;
  wrong session_id ⇒ drop before any state mutation (TrialNetplay §1 hygiene — makes stale,
  replayed, and port-reuse datagrams structurally inert).
- `PROTOCOL_VERSION`: 18 (field builds) → **20** (19 skipped to distinguish interim re0.7 dev
  builds). Version mismatch fails closed at handshake step 1 with a self-describing refusal.
- Existing byte-stable payloads keep their layout: `LockedMatchConfig` (20 B),
  `PaletteConfig/PaletteData/PaletteAck` (50–52; 1024 B bank fits one packet, static_assert
  pinned), `PhaseTransitionPayload` (60/61), NAT signaling, ChurnPause, Ping, Disconnect.

### 3.2 Packet catalog (new and changed)

| Packet | Ch/reliability | Payload (little-endian, packed) | Semantics |
|---|---|---|---|
| `SessionHello` (new) | 0 reliable | `{proto_ver u16, build_hash u32, cadence_num u16, cadence_den u16, client_nonce u64, nickname[16]}` | Handshake step 1; 200 ms resend |
| `SessionOffer` (new) | 0 reliable | `{echo of all Hello fields, host_nonce u64, host_seed u32, host_nickname[16]}` | Step 2; echo-verbatim verify (INV-13) |
| `SessionAck` (new) | 0 reliable | `{echo of Offer fields}` | Step 3; fixes the endpoint |
| `SessionConfirm` (new) | 0 reliable | `{session_id u64 = H(client_nonce, host_nonce, host_seed)}` | Step 4 |
| `SessionConfirmAck` (new) | 0 reliable | `{session_id u64}` | Step 5 → Connected |
| `InputStream` (replaces `GekkoData`) | 1 unreliable-seq | `{session_id u64, epoch u32, newest_frame u32, ack_through u32, count u8 (1..32), inputs[count] u16}` + PressureReport `{produced_through u32, confirmed_frontier u32, prediction_depth u8, run_state u8, adv_delay u8, adv_rollback u8}` (~90 B max) | Redundant window anchored at `peer_ack_through+1`; oldest included frame = `max(newest−31, peer_ack_through+1)`; idempotent merge; sent per sealed input + 50 ms idle resend while unacked suffix non-empty |
| `SyncHash` (new schema) | 0 reliable | `{session_id u64, epoch u32, frame u32, gameplay_hash u64, rng_state u32, hp0 u16, hp1 u16}` | Every 30 confirmed frames; §2.7.7 compare rules |
| `SyncHashAck` (new) | 0 reliable | `{epoch u32, frame u32}` | Bounds the sender's outstanding window |
| `TimeProbe`/`TimeProbeAck` (new) | 1 unreliable | `{generation u32, stamp_us u64}` / `{generation u32, stamp_us u64, dwell_us u32}` | 4 Hz µs RTT (§2.9.3) |
| `ResyncRequest` (id 62, reserved→implemented) | 0 reliable | `{epoch u32, phase_id u8, native_mode u8, local_frame u32}` | Frontend starvation interrogation (INV-11) |
| `ResyncReply` (new) | 0 reliable | same tuple, responder's view | Mismatch → EpochAlign re-run |
| `CharSelFrameInput` / `WinScreenFrameInput` (changed) | per barrier_protocol | + `{epoch u32, phase_id u8}` prepended; per-side serial field **removed** | Acceptance keyed on `(epoch, phase_id)` (INV-7); redundancy window unchanged |
| `FrontendPhaseBarrier` / `FrontendBoundaryDigest` (changed) | 0 reliable | + `{epoch, phase_id}` | Digest VALUE mismatch still requests recovery; KIND mismatch still drop+log (M1 semantics kept) |
| `PhaseTransitionProposal/Ack` (60/61, kept) | 0 reliable | existing payload; kinds gain `EpochAlign {epoch u32, first_phase u8, native_mode u8}` | 250 ms resend, idempotent, per-kind slots (shipping module) |
| `Disconnect` (kept, enriched) | 0 reliable | `{code u8, reason_id u32 hash, human[96]}` | Sticky terminal, 100 ms resend until acked (INV-20) |

### 3.3 Retired wire symbols

`GekkoData`, `GekkoReady` (+payload/flags — startup barrier becomes TransitionBarrier
`GameplayStart`), `GameplayInput` (dead, no send site), `SessionMeta` (dead), `WinScreenConfirm`
(receive-only legacy), `DelayChangeReq/Ack` (INV-23: no negotiation), the legacy `Hello/HelloAck`
handshake (superseded by the 5-step nonce exchange), and the pregame `SyncAnnounce`/`SyncConfirm`
*delay-negotiation fields* (frontend delay is now locally derived from time_probe; announce/confirm
survive as pregame wake-up/identity packets only — which also deletes the `shared=` field that
shipped wrong, INV-13). Config-side retirements that ride along: `stall_threshold`,
`protection_window`, and the frames-ahead exchange — all were inputs to the deleted debt
controller and have no consumer left. Deleted-symbol checklist from inventory §11 applies
verbatim.

### 3.4 Frontend stream acceptance rule (the serial-drift killer)

Receiver accepts a frontend frame packet iff `packet.session_id == session_id && packet.epoch ==
current_epoch && packet.phase_id == current_phase_id`; `frame_index` must be within the lockstep
window. A packet with correct session+epoch but unexpected `phase_id` increments a counter; at
threshold (60 packets or 2 s, whichever first) it triggers the INV-11 interrogation instead of
silent dropping. Phase IDs are a fixed enum: `CharSel=1, StageSel=2, WinScreen=3` (the continue prompt rides
WinScreen's stream under id 3, per the shipped continue_flow design), identical on both builds by
construction — there is nothing left to allocate at runtime.

---

## 4. Session lifecycle

### 4.1 Overview

```
Connect (ENet + NAT) → Handshake (5-step nonce) → Config screen (user-paced)
  → Ready barrier → EPOCH 1: EpochAlign → CharSel/StageSel lockstep → Config lock
  → Palette exchange → Load barrier → Baseline rendezvous → GameplayStart → GAMEPLAY (rollback)
  → WinScreen exact lockstep → Continue prompt (continue_flow)
       ├─ YES,YES → EPOCH k+1: rotate → EpochAlign(None) → ConfigExchange → … → GameplayStart
       ├─ any NO  → EPOCH k+1: rotate → EpochAlign(CharSel) → CharSel lockstep → …
       └─ (session, transport, canonical frame counter, supervisor: CONTINUOUS throughout)
  → Teardown: ONLY supervisor Dead / ProgressDeadline / desync / protocol violation / user quit
```

The engine arms once at the first GameplayStart and is *rotated* (never destroyed) at each
subsequent epoch. The canonical frame counter is the **gameplay** timeline and only advances while
the engine advances; frontend phases exchange per-phase `frame_index` values scoped by
`(epoch, phase_id)` (§3.4) — the two never mix, and neither ever resets mid-session (the canonical
counter spans all matches; a phase's index space dies with its phase).

### 4.2 Connect → handshake (5-step nonce, per TrialNetplay §1)

1. ENet connect (existing menu flow, NAT traversal candidates/hole-punch unchanged). Connect
   timeout 10 s per candidate; refusal/exhaustion → return to JoinEntry with reason as status
   (M1 behavior kept — no DisconnectError cycle per retry).
2. `SessionHello` (client, 200 ms resend): protocol version, build hash, cadence profile, fresh
   nonzero `client_nonce`, nickname. Host validates version+build+cadence **fail-closed** — a
   mismatch sends a refusal Disconnect with the exact field named, before any state exists.
3. `SessionOffer` (host): echoes every Hello field verbatim (INV-13), adds fresh `host_nonce`,
   `host_seed`, nickname. Client verifies its own bytes came back exact.
4. `SessionAck` (client): echoes the Offer verbatim; its arrival fixes the peer endpoint (the
   5-tuple the match will use — the punched hole is the path proof).
5. `SessionConfirm` (host) / `SessionConfirmAck` (client): both compute
   `session_id = fnv1a64(client_nonce ‖ host_nonce ‖ host_seed)` and exchange it; equality is the
   final echo check. Fresh nonces make prior-session replays structurally inert.
   Per-step timeout 10 s, whole-handshake cap 30 s → back to menu with reason (not a kill path —
   no session existed yet).

### 4.3 Ready → first epoch → GO

- Config screen is user-paced; supervisor runs on protocol silence so browsing forever is safe
  (INV-14 — the test-1 killer class). `Session_SignalReady` both ways → Ready barrier
  (TransitionBarrier kind, both-or-neither) → `PregameSync_Begin` → epoch 1 → EpochAlign(CharSel).
- CharSel/StageSel lockstep runs exactly as today (KEEP modules) with §3.4 acceptance and the
  frontend delay from §2.9.3. Palette catalog rides `CharSelInput` pre-lock; bank exchange
  (50–52) fires post-config-lock; both routed by packet_router in all regimes (§2.3).
- Load barrier → baseline rendezvous → GameplayStart (§2.5) → match_director arms the engine at
  `epoch_frame_origin` = current canonical frame → gameplay.

### 4.4 Match end → continue prompt → rematch epochs (the fixed ladder, INV-9)

1. Match end detected (MatchLifecycle) → winscreen entered; winscreen lockstep continues on the
   session timeline; `continue_flow` drives the prompt from **consumed lockstep frames only**
   (its shipped design: per-player cursors, both-lock resolution, 3600-frame timeout → NO, session
   never dies from this screen).
2. Resolution reached → the fixed barrier ladder, strictly ordered, each both-or-neither:
   **(a) `WinScreenExit`** — proposed by both on resolution; commits when both acked. The director
   refuses to act on any later-step commit until (a) is locally committed (a re-ordered arrival is
   held and re-acked, not skipped — the exact inversion that killed the field session is
   structurally impossible).
   **(b) `PostMatchDecision{intent}`** — carries Rematch/ReturnToCharsel; both sides already know
   the answer from lockstep (continue_flow), so this is confirmation + telemetry; mismatch with
   the lockstep-derived answer is a protocol-violation terminal (can only mean divergent lockstep
   streams — fail closed loudly rather than play a desynced rematch).
   **(c) `EpochAlign{epoch k+1, first_phase, native_mode}`** — host mints k+1; `first_phase` =
   None (YES,YES fast path) or CharSel (any NO). Commit requires both peers to report the same
   `native_mode`; a peer still in mode 9 completes its local exit (fade path per the continue
   plan) before acking — the alignment barrier IS the fix for "one peer in charsel while the
   other still renders the win screen" (INV-10). Commit cancels every deferred Begin (INV-8).
3. YES,YES: mode-ownership interception (`MODE_WINSCREEN→MODE_CHARSEL` ⇒ `(7,1)`) and the
   `PregameSync_BeginRematch` fast path run exactly per the continue plan; under the hood the
   fast path is §2.5's rematch flow. Characters/stage/palettes preserved; host mints fresh
   `session_seed`/`rng_seed`; SetTracker continuity untouched.
4. Any NO: both route to charsel under epoch k+1; existing auto-restart flow, now wire-driven by
   the committed ladder (the fragile local signatures from M4 resilience become secondary).

### 4.5 Epoch adoption rules (INV-7/8/10 mechanics)

- Adoption = processing an `EpochAlign` proposal for epoch > current. On adoption the receiver:
  (1) cancels all deferred/pending frontend `Begin`s and any active sync machine of an older epoch
  (both are epoch-tagged); (2) completes any local native-mode exit required to match
  `native_mode`; (3) acks. Input exchange for the epoch begins only after local commit.
- The fatal field sequence replays as: client still in mode 9 receives EpochAlign(CharSel, mode 6)
  → cancels its deferred WinScreenSync Begin (INV-8) → finishes its mode-9 exit → acks → both
  enter CharSel with `phase_id=CharSel` streams. No serials exist to diverge (INV-7).
- A deferred Begin firing is a bug by construction now: `FrontendInputSync_Begin(epoch, phase)`
  no-ops with an error log if `epoch != current_epoch` or a machine is already active.

### 4.6 Recovery ladder (never-teardown paths, INV-11/12)

Ordered, escalating, all on the live connection:

1. **Drop+log** (M1 downgrades, kept): packet-type mismatch, redundancy oddity, stale digest KIND,
   phase-barrier next-phase mismatch when locally committed.
2. **Interrogate** (2 s starved + supervisor not Dead): `ResyncRequest`/`Reply` identity exchange;
   on mismatch → EpochAlign re-run for the current epoch (idempotent: same epoch, re-commit).
3. **Pregame restart** (3 failed interrogation cycles ≈ 6 s, or handshake-phase 45 s cap): abort
   frontend machines, fresh epoch, cross-phase announce restart (M1 mechanism) — both roles.
4. **Teardown** — only: supervisor Dead (20 s protocol silence), ProgressDeadline (20 s zero
   canonical progress in gameplay), ConfirmedDesync, protocol violation (Conflict/InvalidValue/
   inconsistent epoch hash/lockstep-vs-barrier contradiction), user cancel/quit. Enforced by the
   kill-path CI gate (INV-12).

### 4.7 Graceful/dead teardown

- Graceful: `Session_NotifyGameExit` → reliable Disconnect (code, reason_id, human string),
  bounded 150 ms wait → destroy (shipping behavior kept). Remote side surfaces the reason
  verbatim ("Peer closed the game"), never "Connection lost".
- Dead: supervisor fires `NetMenu::HandleDisconnection` exactly once; director teardown order
  §2.6.4; ENet host destroyed last. Remote terminals are applied before local liveness judgment
  (drain-before-judge).
- Crash: peer sees 20 s silence → Dead. Local: SEH tick wrapper writes a crash snapshot; async
  log loses ≤1 s (periodic flush, shipping).

---

## 5. Edge-case matrix

Every row: the situation, the specified outcome, and the single component that decides it.
Nothing here is best-effort; each row is a test in §7.4.

### 5.1 Connection / handshake

| # | Case | Specified handling | Decider |
|---|---|---|---|
| C-1 | Connect timeout (host unreachable) | 10 s/candidate, exhaust candidates incl. autopunch, return to JoinEntry with status text; no spectator-port auto-probe after gameplay-connect timeout (M1) | session2 |
| C-2 | Connection refused / port closed | Same as C-1, distinct status string | session2 |
| C-3 | Protocol/build/cadence mismatch | Fail-closed refusal at Hello/Offer naming the exact field; both sides show it; no retry loop | session2 handshake validator |
| C-4 | Handshake step stalls (packet loss) | 200 ms per-step resend; 10 s step timeout; 30 s total cap → back to menu (no session existed → not a kill path) | session2 |
| C-5 | Join-vs-host race: both host, or A joins B while B joins A | Roles are explicit UI choices; both-host = both listen (no connect) → C-1 timeout at each. Cross-join: each ENet host gets an inbound connect while its own outbound is pending — session2 accepts the inbound only when in Hosting state, else refuses with `Busy`; the pair resolves to whichever direction completed ENet connect first, the other side's outbound is canceled on Connected | session2 state machine |
| C-6 | Second peer connects to a hosting session already in Handshake+ | Refuse with `Busy` disconnect data word; existing peer unaffected | transport2 |
| C-7 | Handshake replay from a previous session (stale datagrams, port reuse) | Fresh nonzero nonces per attempt; wrong/zero nonce or wrong session_id → drop before state mutation | session2 / packet_router |

### 5.2 Wire quality during gameplay

| # | Case | Specified handling | Decider |
|---|---|---|---|
| W-1 | Steady loss 1–5% | Redundant window refills holes with zero retransmit round-trips; rollback depth breathes ±1–2; **zero holds while coverage ≥ required**; no classifier exists to overreact (INV-3) | engine |
| W-2 | Loss burst ≈2 s (120 frames) | Both sides reach own `PredictionLimit` ceiling ≤ R frames in, hold (scheduler N=0, timeout counters cleared); producers keep sealing ≤30 ahead of ack (INV-24); supervisor → Degraded. On resume: one InputStream packet each way refills the whole suffix (window 32 ≥ producer bound 30); hidden catch-up burns owed debt ≤2/pass; no rebase burst (semanticHold) | engine + scheduler + supervisor |
| W-3 | Burst ≈5 s | As W-2; supervisor → Interrupted (5–20 s band): HUD banner, gameplay stays frozen at ceiling, engine does NOT abort (Interrupted ≠ broken, per M3 resilience semantics now owned by supervisor) | supervisor |
| W-4 | Burst ≈15 s | As W-3; resume before 20 s → recovery as W-2; supervisor Dead never fires early — no other component may count silence (INV-14) | supervisor |
| W-5 | Burst ≥20 s | Supervisor Dead → the single teardown; self-describing reason both sides (peer sees its own 20 s silence) | supervisor |
| W-6 | One-way loss (we send, nothing returns) | Our `protocol_silence_ms` grows → our supervisor escalates to Dead at 20 s. Peer receives our inputs but its acks die: our `peer_ack_through` freezes → producer stops at bound → we hold at ceiling; PressureReport lets each side's HUD distinguish "peer stopped producing" (produced_through frozen) from "packets not arriving" (silence). Teardown by whichever supervisor dies first; reason states direction | supervisor (+ HUD from PressureReport) |
| W-7 | Ping spike (RTT doubles for seconds) | No classifier reaction (deleted). Depth grows; if it hits R → clean PredictionLimit holds for the spike's duration, else nothing visible. time_probe p95 rises → frontend-delay latch may add ≤1 frame per 0.15 s; decays after | engine; frontend latch |
| W-8 | Sustained RTT above coverage (route change) | Continuous holds at ceiling = honest degradation (genuine cause); HUD shows Underbuffered badge + measured oneway frames; user raises R/D locally (INV-23, no negotiation needed) | engine + delay_policy UI |
| W-9 | NAT rebind mid-match | Autopunch 2 s authenticated keepalive heals: rewrite `peer->address` under the four shipping guards (host only, single peer, connectID match, same source IP). Non-autopunch sessions: ENet peer continues if the OS mapping survives; else W-2..W-5 ladder | transport2 |
| W-10 | Clock/crystal drift between machines | Both pin the identical integer-exact cadence rational (handshake-verified); residual ±50 ppm crystal drift builds depth imbalance over minutes → pace slew (cap 25 000 ppm) absorbs it invisibly; no wall-clock enters any sim decision (INV-16) | scheduler slew |
| W-11 | Reordered/duplicated datagrams | InputStream: idempotent merge, `DuplicateIdentical` ignored; ENet ch1 sequencing drops stale packets whose content is superseded anyway; control ch0 is reliable-ordered; barriers idempotent by (kind, seq) | engine ingest / transition_barrier |

### 5.3 Peer machine behavior

| # | Case | Specified handling | Decider |
|---|---|---|---|
| P-1 | Peer fps degradation (weak hardware, thermal throttle) | Slow peer's frontier lags; fast peer's depth → R → holds. Slew asks the *slow* (low-depth) side to run 2.5% fast — if hardware can't, its deadline rebases (external cause → debt discarded). Net: session runs at the slow machine's speed with clean holds on the fast side. This is the "genuine cause: weak hardware" carve-out — honest, labeled (`RunState=PredictionPressure`, HUD shows peer sim rate from PressureReport) | scheduler + engine |
| P-2 | Local device churn (controller unplug/replug) | churn_pause (KEEP): mutual pause hint packet; scheduler `ExternalSuspension` (debt discarded); resume via existing flow | churn_pause + scheduler |
| P-3 | Focus loss / minimize | netplay_background_run keeps simulating (KEEP); minimized present skipped (proxy Sleep(16) path) while sim paces on the scheduler — pacing no longer depends on Present at all | scheduler |
| P-4 | Windows sleep/resume, debugger stall, 100 ms+ freeze | QPC delta clamp (100 ms) in pinned tick shim; scheduler rebase (deadline=now, frames discarded as external); peer side sees W-2..W-5 ladder | scheduler |
| P-5 | Pause menu / F1-style mid-match exits | Synchronized lifecycle edge: queued and applied at a confirmed boundary via TransitionBarrier `SessionCancel`-family kinds; raw async keys never act directly on sim (TrialNetplay ESC lesson) | pause_handler + transition_barrier |
| P-6 | Alt-F4 / window close | `ModOnGameExit`: reliable Disconnect "Peer closed the game", 150 ms bounded wait, log flush (shipping). Peer routes to menu with honest reason — never "Connection lost" | session2 |
| P-7 | Hard crash mid-rollback | Local: SEH snapshot around the tick, async log ≤1 s loss. Peer: silence → supervisor ladder → Dead at 20 s. On next launch no stale state exists (session state is process-local; savestates in-memory only) | supervisor (peer side) |
| P-8 | Wedged game (pings alive, sim stuck) | ProgressDeadline: 8 s zero canonical progress → HUD warn, 20 s → teardown reason `ProgressDeadline` (distinct from silence Dead) | supervisor |

### 5.4 Frontend / rematch / epochs

| # | Case | Specified handling | Decider |
|---|---|---|---|
| F-1 | Winscreen/rematch commit-order race (the field killer) | Fixed ladder WinScreenExit → PostMatchDecision → EpochAlign; later-step commits held+re-acked until earlier step committed locally; skipping a step is impossible (INV-9) | match_director gate |
| F-2 | One peer still in mode 9 when new epoch announced | EpochAlign adoption: cancel deferred Begins, complete native exit to `native_mode`, then ack; input exchange gated on commit (INV-8/10) | match_setup |
| F-3 | Frontend starvation with healthy transport (serial-drift class) | Cannot start (serials gone, INV-7); if any (epoch,phase) mismatch persists: 2 s → interrogation, 6 s → pregame restart fresh epoch; never teardown (INV-11/12) | frontend_input_sync + match_setup |
| F-4 | Charsel cancel storm (rapid cancel/re-enter, both sides) | Every cancel/restart is a barriered transition with per-kind seq; stale proposals re-acked idempotently; cross-phase announce restarts pregame on both roles; each restart = fresh epoch so no stream aliasing | transition_barrier + match_setup |
| F-5 | Both sides resolve continue prompt simultaneously | Deterministic by construction: resolution is a pure function of the shared lockstep stream — both compute identical results on the same lockstep frame; ladder proposals cross on the wire and merge idempotently | continue_flow |
| F-6 | Continue prompt timeout with one AFK player | 3600 lockstep frames → NO on the AFK side's behalf per shipped contract; routes both to charsel; session lives | continue_flow |
| F-7 | PostMatchDecision barrier contradicts lockstep-derived answer | Protocol-violation terminal (divergent lockstep streams = worse than desync); dump + fail closed | match_director |
| F-8 | Rematch fast path reaches mode 8 sub 3 before GameplayStart commit | Load-barrier freeze extended over the fast-path window (continue plan "freeze coverage gap" carried over) | match_setup |
| F-9 | Palette bank arrives during regime transition | Single packet_router owner routes 50–52 in every regime; 500 ms resend until ack unchanged | packet_router |
| F-10 | Config screen browsed for minutes (test-1 class) | Protocol-silence liveness: ENet pings every 150 ms → supervisor stays Healthy indefinitely. Pregame phase timeouts fire only after base timeout AND ≥2 s protocol silence; while the peer provably transmits they extend to the 45 s cap, and the cap routes to pregame restart, not teardown (M1 semantics kept, INV-12) | supervisor (INV-14) + match_setup |
| F-11 | Stage watchdog / load divergence (one side fails asset load) | Load barrier never commits → 45 s cap → pregame restart with reason; asset failure itself surfaces locally | match_setup |
| F-12 | Baseline digest mismatch | Dump breakdown both sides → one retry (same epoch) → second mismatch = terminal `BaselineMismatch` (genuine content divergence) | match_setup |

### 5.5 Desync / verification

| # | Case | Specified handling | Decider |
|---|---|---|---|
| D-1 | SyncHash mismatch at confirmed frame | Evidence dump both machines (desync_dump region-level) → terminal `ConfirmedDesync`, reliable resend until ack; whoever detects, decides — no arbitration | engine confirm pipeline |
| D-2 | Hash from stale epoch | Ignore (rotation raced the pipe — normal at boundaries) | engine |
| D-3 | Hash from future epoch | Queue until local epoch catches up; bound 128 then protocol violation | engine |
| D-4 | Remote input Conflict / InvalidValue (0xFFFF or out-of-mask bits) | Fatal protocol violation, never masked (INV-19) | engine ingest |
| D-5 | Non-deterministic resim on one machine | Debug-build resim self-check catches it locally (same frame, same inputs, different hash) before it ever becomes a cross-peer desync | engine (debug) |

### 5.6 Spectator / replay

| # | Case | Specified handling | Decider |
|---|---|---|---|
| S-1 | Spectator joins mid-match | Sidecar serves PreMatchState (`match_id = session_seed ^ (config_hash<<1)`, stable formula) + confirmed-record backlog from current epoch origin; client runs the catch-up ladder: budgets 2/4/8/16/24 ticks per presentation slot by backlog rung, 12 ms wall slice, yields at any phase boundary | spectator_playback policy |
| S-2 | Spectator starved (upstream stall) | Elastic slow-motion under low water: 950/850/700 permille at 180/120/60 buffered records, Bresenham-distributed holds — never freeze-then-sprint; rebuffer resumes at 120, live target 240 (~4 s), auto catch-up above 300 with hysteresis to 240 | spectator_playback policy |
| S-3 | Spectator joins during frontend/rematch phases | Sidecar streams phase markers + LockedMatchConfig on epoch boundaries; spectator idles at its "waiting for match" screen until the next MatchState | spectator_manager |
| S-4 | Spectator desync vs archive | Records carry pre/post hashes; mismatch = spectator-side fail-closed (leave session), player link unaffected (publication is observational, fail-degrades) | spectator_client |
| S-5 | Replay recording across epochs | Recorder consumes the confirmed pipeline only (predicted values can never enter the file); records epoch-tagged; epoch boundary = replay chapter; format extension is additive (v-bump in replay header), playback of 0.6 files unaffected | replay_runtime |
| S-6 | Rollback rewrites vs sidecar | Gone by construction: engine2 pushes **confirmed-only** frames (`SpectatorRuntime_OnGameplayFrame` with `confirmed_rb_frame == rb_frame` always; `FRAME_FLAG_ROLLBACK_REWRITE` retired) — inventory §6's noted simplification | engine confirm pipeline |

### 5.7 Simultaneity sweep ("both sides at once")

| # | Both sides simultaneously… | Result | Decider |
|---|---|---|---|
| B-1 | …propose the same barrier kind | Idempotent merge on (kind, seq); host-sequenced tie-break; single commit | transition_barrier |
| B-2 | …cancel the session | Both run graceful teardown; crossing Disconnects ack each other; no error UI ("You canceled") | session2 |
| B-3 | …hit PredictionLimit (bilateral stall risk) | Producers keep feeding (INV-24) → each side's ceiling releases as the other's inputs land; the bilateral sub-speed equilibrium cannot form; if coverage is just-fit, HUD shows Marginal (lesson 15) | engine producer |
| B-4 | …request pregame restart (cross-phase announce) | Announce carries epoch; higher epoch wins adoption; equal epoch (true simultaneity) → host's announce wins (role tie-break) | match_setup |
| B-5 | …detect desync on different frames | Both dump, both send terminal; first-received reason displayed, both logged | engine |
| B-6 | …alt-F4 | Crossing Disconnects; both exit clean; logs flushed | session2 |
| B-7 | …change local delay (hotkeys) | No interaction whatsoever — peer-local knobs, no wire messages (INV-23) | engine (each side) |

---

## 6. Migration plan on `re0.7`

Expansion of inventory §10 into concrete milestones. Each milestone is independently buildable and
testable; its exit gate is listed. Gekko keeps shipping until M6's cutover gate passes.
Binary-compatibility ledger in §6.2.

### M0 — Zero-risk pre-refactors (no behavior change; each independently landable)

| Task | Files |
|---|---|
| Extract `RollbackTimesyncTelemetry` → `include/rollback/rollback_telemetry.h`; rename `gekko_avg_ping/gekko_jitter` → `link_avg_ping/link_jitter` | rollback_session.h, churn_pause.cpp, netplay_pacing.cpp (type-only includers decouple fully) |
| Extract `OnGameplayPacket` (~300 lines, 22 cases) → `src/net/gameplay_packet_router.cpp` (seed of packet_router) | online_wiring.cpp |
| Delete dead includes: `network_thread.h` from connection_supervisor.cpp; `match_bootstrap.h` from barrier_protocol.cpp | 2 files |
| Invert dependency: inject the `PregameSync_GetPhase()` predicate into frontend_input_sync instead of the upward include | frontend_input_sync.cpp/h, pregame_sync.cpp |
| Fix pre-existing failing test `frontend_sync_tests.cpp:418` (winscreen confirm-hold) | frontend_sync_tests.cpp (+ whichever module is actually wrong) |
| Land `rollback/run_state.h` + hold-episode ledger + the §2.10 `STAT` line format (behind existing telemetry) | new header, netplay_log |
| **Exit gate:** build green, all existing tests green, field-log format documented |

### M1 — Facade freeze + protocol v2 header (compile-only)

| Task | Files |
|---|---|
| Pin the preserved contracts (§ inventory 2.1–2.5) in a `docs/re0.7/API_FREEZE.md` checklist; add static_asserts on struct sizes (`SessionSnapshot`, `LockedMatchConfig` 20 B, palette payloads) | headers + new doc |
| protocol.h v2: add §3.2 packets (`SessionHello..ConfirmAck`, `InputStream`, `SyncHash/Ack`, `TimeProbe/Ack`, `ResyncReply`); add `(epoch, phase_id)` to frontend frame/barrier/digest payloads; add `EpochAlign` barrier kind; delete §3.3 retired symbols; `PROTOCOL_VERSION` → 20 | include/net/protocol.h, transition_barrier.h |
| `net/frame_arithmetic.h` ported 1:1 from QOH99 (wrap-safe half-open helpers) with unit tests | new header + tests |
| **Exit gate:** build green with old backend still running on the old packet set (new types defined, unsent) |

### M2 — FrameScheduler + clock pinning (offline-verifiable, biggest single de-risk)

| Task | Files |
|---|---|
| `src/patches/frame_scheduler.cpp/h`: deadline math §2.8.2, cadence profiles, semanticHold, rebase reporting, slew input hook, `STAT` instrumentation | new |
| Limiter detour: byte-signature scan of 0x5D2AC0 cluster, install-or-fail-loud, vanilla `jl` neutralization, `dword_816360`/FPS bookkeeping coherence | hook_installer.cpp, as2_constants.h |
| Pin `Hook_GetTick` to 1.0; delete `SetNetplayTickScaleTarget`/`SetNetplayPacingActive` writers; rewrite `ResetNetplayTickScaleState` to rebase-never-reset | tick_hooks.cpp, netplay_pacing.cpp callers |
| d3d9 proxy `present_interval=immediate` config key | d3d9_proxy.cpp (L2307 site + ini read) |
| Wire dispatcher N∈{0,1,1+k} plumbing (hold = −1 + AdvanceFrame suppression + timeout clears; catch-up via existing double-tick machinery generalized to k) | input_override.cpp, input_sync_hooks.cpp |
| **Exit gate (T-SCHED):** offline 3-min run: present-interval mean 16.667±0.01 ms, p99 ≤ 17.2 ms, zero rebases, CPU ≤5% of one core; 58.8-compat profile equally exact; the 0.6 GekkoNet netplay path still works on top (scheduler replaces limiter under it — netplay_pacing holds temporarily map to semanticHold via a shim) |

### M3 — transport2 + session2 swap (under unchanged `Session_*` contract)

| Task | Files |
|---|---|
| `src/net/transport2.cpp/h`: worker thread, SPSC queues, `protocol_silence_ms`, peer-resilience config, NAT/keepalive/fault-injection absorption, `*ForHost` helpers preserved for spectator | new; delete network_thread.*; shrink enet_transport to helpers |
| `src/net/session2.cpp`: state machine, 5-step handshake §4.2, teardown funnel `Session2_Terminate`, `Session_*` facade verbatim; `Session_GetMsSinceLastInbound` re-implemented over protocol silence | new; delete session_manager internals (header stays) |
| packet_router: single owner, routing table, deferred flush (256/256 KB bound) | promote gameplay_packet_router → src/net/packet_router.cpp |
| connection_supervisor: re-feed from protocol silence; add ProgressDeadline input | connection_supervisor.cpp |
| Extend `tools/check_killpaths.ps1` allowlist to the INV-12 set exactly | tools |
| **Exit gate:** menu connect/handshake/config/supervisor/NAT all green over the new stack with the OLD pregame+Gekko gameplay still riding it (old SyncAnnounce flow tunnels over session2); autoconnect_harness passes; T-SUP silence-ladder tests pass |

### M4 — engine2 offline bring-up (socket-free; Gekko still ships)

| Task | Files |
|---|---|
| Resurrect + finish `input_timeline` (canonical ring, capture-once, delay relabel, ack/confirm frontiers) and `prediction` (hold-last + earliest mismatch) per §2.7.3 | src/rollback/input_timeline.*, prediction.* |
| `src/rollback/engine2.cpp/h`: EngineConfig, nextAction, rollback transaction, confirm pipeline, producer, PressureReport, typed ingest results — **zero Win32/game includes** | new |
| Facade adapter: `rollback_session.cpp` becomes engine2 adapter behind unchanged header (Gekko branch kept behind `AS2_WITH_GEKKO` CMake flag until M6 gate) | rollback_session.cpp |
| StateHistory: 64-slot tagging `{epoch, frame, phase}`, FPU/MXCSR capture, tag-validated restore; savestate guard adapter (`GameplayBridge_IsSessionActive` → `RollbackSession_IsActive`, 2-line) | resimulation.cpp, savestate.cpp, netplay_palette_runtime.cpp |
| M4-6: **hash-membership audit** — walk game_snapshot regions, classify sim-affecting vs render/timing (INV-22), document per-region role table | new doc section + game_snapshot |
| M4-7: **exact-window decomp audit** — verify AS2 round-end frees nothing (TrialNetplay §4 method); pin the §2.7.6 initial window set with decomp line citations | doc + engine predicate |
| Unit tests T-ENG-1..12 (§7.1) + deterministic soak harness (scripted inputs, virtual clock, loss/jitter/reorder shims — no sockets) | tests/ |
| Microbench: snapshot save/restore p99 on min-spec | tests/ |
| **Exit gate:** all T-ENG green; 100k-frame socket-free soak at 3% loss + 40±15 ms jitter: zero conflicts, zero confirm stalls, hash-chain clean; save p99 ≤1.5 ms |

### M5 — match_setup (big-bang part 1; ships only with M6)

| Task | Files |
|---|---|
| `src/net/match_setup.cpp`: epoch authority, EpochAlign, deterministic phase schedule, config/load/baseline/GameplayStart barriers, rematch fast path, cross-phase restart, recovery ladder §4.6, `PregameSync_*` facade | new; delete pregame_sync/match_bootstrap internals |
| Frontend phase-identity migration: `(epoch, phase_id)` in frame/barrier/digest payloads; serial allocator deleted; deferred-Begin epoch tagging + cancellation | frontend_input_sync.cpp, charsel/stagesel/winscreen_sync.cpp |
| ResyncRequest/Reply implementation (INV-11) | frontend_input_sync.cpp, packet_router |
| Palette/spectator/lifecycle callback rewiring per inventory §4/§6 (all 6+ call sites listed there) | match_setup.cpp, match_director.cpp |
| continue_flow integration: ladder proposals fired from resolution; `PregameSync_BeginRematch` contract unchanged | continue_flow.cpp (minimal edits), match_setup |
| **Exit gate:** frontend_sync_tests extended for phase-identity + EpochAlign + interrogation, all green; two-instance charsel/rematch cycling under 5% loss without gameplay (frontend-only soak) |

### M6 — match_director + engine2 cutover (big-bang part 2; one release with M5)

| Task | Files |
|---|---|
| `src/rollback/match_director.cpp`: `OnlineWiring_*` facade, arm/rotate/teardown ordering §2.6, epoch rotation execution, fx/spectator/lifecycle event fan-out | new; delete online_wiring.cpp |
| input_override dispatcher branch rewired to engine2 two-phase contract; netplay_pacing controller deleted (§2.8.7); DelayPolicy re-fed (`OnRollbackApplied` from finishRollback; measurement from time_probe) | input_override.cpp, delete netplay_pacing.cpp, delay_policy.cpp |
| time_probe implementation + frontend-delay latch port | new src/net/time_probe.cpp, frontend_input_sync |
| Remove GekkoNet: CMake `GEKKONET_DIR`, link line, `AS2_WITH_GEKKO` flag + Gekko branch of rollback_session.cpp, vendored lib | CMakeLists, lib/GekkoNet |
| Rename cleanup per inventory §11 deleted-symbol checklist (grep-verified zero references) | tree-wide |
| HUD updates: coverage classification badge (Underbuffered/Marginal), peer sim rate + produced-frontier readout (from PressureReport), hold-cause line (run_state), removal of NETCLASS/debt readouts | netplay_hud*.cpp |
| **Exit gate (the cutover gate):** full §7 suite green incl. T-PERF-1..4 and the LE-1 replication; only then does `re0.7` merge forward |

### M7 — Spectator/replay re-hookup

| Task | Files |
|---|---|
| engine2 → `SpectatorRuntime_On*` confirmed-only push (S-6); match_id formula preserved | engine2, spectator_runtime |
| Replay recorder onto confirmed pipeline; epoch chapters; header v-bump | replay_runtime |
| **Exit gate:** spectate a full 3-match rematch session incl. late join + starve test; replay of same session plays back hash-clean |

### M8 — Soak and field acceptance

Rematch soak (100 cycles), full loss-injection matrix (§7.3), 2-instance determinism runs, manual
WAN sessions replicating the field-log scenario. Exit gate = §7.5 acceptance criteria + §8.3
fallback review.

### 6.1 Sequencing constraints

- M0→M1→M2 strictly ordered; M3 and M4 parallelizable after M2; M5+M6 ship together (pregame hands
  the baseline to the engine; barrier kinds span both); M7 after M6; M8 last.
- After M3, every field build carries the new transport (protocol v20 pre-release tag) — 0.6/0.7
  peers fail closed at handshake by design.

### 6.2 Binary-compatibility ledger

**Stays byte-stable:** `LockedMatchConfig` (20 B + hash), palette payloads 50–52 (+1024 B bank
static_asserts), `PhaseTransitionPayload`, savestate/game_snapshot memory layout, replay file
format (additive header bump only), spectator sidecar protocol (match_id formula, PreMatch/Match
packets), settings/ini keys, all preserved C API headers (§ inventory 2.1–2.5).
**Breaks by design:** wire protocol (v18/19 peers refused at Hello with a named reason),
GekkoNet-era packet types, delay-negotiation wire flow.

---

## 7. Test & verification plan

### 7.1 Engine-core unit tests (socket-free, clock-free — the QOH99 method)

| ID | Pins |
|---|---|
| T-ENG-1 | Capture-once: repeat capture during stall adopts, never resamples; slot immutability asserts (INV-18) |
| T-ENG-2 | Delay relabel: raise fills neutral future only; lower drains at confirmed boundary; 5 s drain revert |
| T-ENG-3 | Predict/mismatch: hold-last; earliest-mismatch selection under out-of-order actuals; matching prediction ⇒ no rollback |
| T-ENG-4 | Rollback transaction: begin/commit/finish; boundary truncation variants; capture-during-transaction asserts |
| T-ENG-5 | Confirm ordering: confirmed frames pop in order with correct pre-hash; predicted values can never pop |
| T-ENG-6 | Ingest taxonomy: Applied/Duplicate/Conflict/Invalid(0xFFFF, out-of-mask)/TooFarFuture/Stale each hit their exact result; Conflict+Invalid are terminal (INV-19) |
| T-ENG-7 | Window math: un-acked suffix always ≤32 (producer bound 30); ack-anchored re-anchor after loss; hole refill from a single packet |
| T-ENG-8 | Producer: seals 1/frame-period while stalled; bound `min(peerR+peerD+2, 30)`; fenced during frontend/barriers (INV-24) |
| T-ENG-9 | INV-4 pin: with R=8, exactly 8 speculative frames advance before the first Stall(PredictionLimit); no other code path reads R (link-time grep test) |
| T-ENG-10 | Wrap safety: full pipeline across the u32 boundary (first_frame = 0xFFFFFF00) |
| T-ENG-11 | Epoch rotation: stale-epoch restore refused; stale hash ignored/future queued/inconsistent fatal; origin remap correct |
| T-ENG-12 | SyncHash cadence: every 30th confirmed frame exactly; queue bound 128; exact-field compare |
| T-SCHED-1..4 | Deadline exactness (1M-frame simulated run, drift = 0 ticks); rebase threshold; semanticHold neutrality; slew bounds/admission/instant-release |
| T-LADDER | Barrier ladder: all 6 orderings of arrival for the 3 match-end steps converge to committed-in-order; step-skip impossible |
| T-CODEC | Packet codec round-trip + fuzz (truncated/oversized/garbage never crash, never mutate state) |

### 7.2 Deterministic two-instance harness

Extends the shipping loopback tooling (autoconnect_harness, scripted_input_runner,
harness_shared_memory; TrialNetplay §5-Adopt-9 model): two game instances, scripted inputs, egress
net shim (delay/jitter/loss/reorder/dup via stress_hooks + transport fault injection), separate
game folders (two-instance file-lock hazard — TrialNetplay pitfall 7), machine-checkable PASS/FAIL:
final SyncHash chain equality + zero terminal + per-frame confirmed-hash comparison. Target: 15k+
battle frames per scenario, the reference's "13,230 frames, 0 mismatches under 40±15 ms + 3% loss"
as the bar.

### 7.3 Loss-injection matrix vs expectations

Rows run in the harness AND (spot-checked) manual WAN. Delay/R per cell chosen so coverage =
required+1 unless stated. **Expected** columns are pass/fail criteria, not aspirations.

| Scenario (RTT, jitter, loss) | Expected holds | Expected rollbacks | Expected outcome |
|---|---|---|---|
| LAN 5 ms, 0%, D0/R2 | 0 | ≈0 | 60.00 fps flat, depth ≤1 |
| 80 ms, ±5 ms, 0%, D2/R4 | 0 | steady shallow (≤3) | 60.00 fps flat |
| **LE-1: 155 ms, ±20 ms, 0%, D0/R8** (field replication) | **0** | continuous depth ≈5, max ≤8 | **60.00 fps flat — the acid test the old stack failed at 51 fps / 8 freezes/s** |
| 155 ms, ±20 ms, 0%, D4/R4 (policy-recommended) | 0 | shallow (≤2) | 60.00 fps flat |
| 40 ms, ±15 ms, 3% | <2 holds/min | frequent shallow | no visible stutter; p99 present ≤18 ms |
| 155 ms, 1% | <2 holds/min | continuous | fluid; STATS confirm loss seen |
| 155 ms, 5% | occasional 1-frame holds | continuous deeper | playable; honest degradation, HUD shows cause |
| 10% + 100 ms jitter | frequent holds | deep | degraded but stable; zero teardowns; recovers instantly when shim clears |
| Burst 2 s / 5 s / 15 s (W-2..4) | frozen during burst | resume ≤3 s | supervisor band correct; no teardown; refill from single packet verified |
| One-way loss 30 s (W-6) | frozen | — | exactly one teardown at 20 s, correct direction in reason |
| Underbuffered on purpose: 155 ms D0/R3 | continuous PredictionLimit | max depth = 3 exactly | honest slideshow, HUD badge, INV-4 pin: never holds before depth 3 |

### 7.4 Scenario/lifecycle suites

- **Rematch soak:** 100 continue-screen YES cycles + mixed NO cycles under 3% loss (extends
  shipping `rematch_soak`); PASS edge: pregame handoff re-entry; asserts: epoch strictly
  increasing, zero deferred-Begin fires, zero teardowns, SetTracker continuity, palette re-arm
  each epoch (no `override=none` leftovers).
- **Edge-matrix execution:** every §5 row becomes a scripted scenario or a documented manual test;
  the matrix table gains a "test id" column in `docs/RESILIENCE_TESTING.md` at M8.
- **Kill-path gate:** `check_killpaths.ps1` in CI on every commit (INV-12).
- **Determinism regression:** determinism_verify + debug resim self-check on in every harness run.

### 7.5 Performance acceptance criteria (release gate)

Measured by the §2.10 STAT instrument on the defined min-spec machine (see §9 Q4) and one modern
machine, 3-match sessions:

1. Offline: present-interval mean 16.667±0.01 ms, p99 ≤17.2 ms, zero rebases, ≤5% of one core in
   the wait (vs 100% today).
2. Online 0% loss, coverage ≥ required+1 (any RTT ≤250 ms): **zero holds of any cause** end-to-end
   across 3 matches + 2 rematch boundaries; effective sim rate 60.00±0.02 fps; present p99
   ≤18.0 ms.
3. LE-1 cell exactly as tabled.
4. Rollback cost: depth-8 correction completes ≤10 ms on min-spec (else R recommendation cap
   documented lower); savestate p99 ≤1.5 ms.
5. Freeze-pulse count (the old `TSYNC` equivalent): 0 at 0% loss. Class-change events: metric no
   longer exists (classifier deleted) — its absence is the criterion.

---

## 8. Risk register + fallback

### 8.1 Risks and mitigations

| # | Risk | L×I | Mitigation / early warning |
|---|---|---|---|
| R-1 | Limiter-detour signature not found / game update shifts it | L×H | Signature asserted at install with loud failure; fallback path: pinned `Hook_GetTick` virtual clock with the two DECOMP §5(c) mandatory fixes (rebase-not-reset + thread confinement) is kept implemented behind a config flag for one release |
| R-2 | Savestate cost too high for per-frame save + deep resim on min-spec | M×H | M4 microbench gates before any wiring; region pruning first; Block64 hash mandated; R recommendation cap as pressure valve |
| R-3 | Determinism holes (out-of-region globals, edge-triggered flags) | H×M | Debug resim self-check (single-instance detection); aux-region sweep task in M4-6; desync dump diff loop is a shipped 10-minute workflow (TrialNetplay) |
| R-4 | Epoch rotation misses a match-scoped buffer (stale state leaks across matches) | M×H | Epoch-tag audit checklist in M6 review; tag-validated restore fails closed; rematch soak with checksum assertions is the net |
| R-5 | Big-bang M5+M6 breadth | M×H | Gekko path compiles behind `AS2_WITH_GEKKO` until the cutover gate; frontend-only soak (M5 gate) de-risks half; facade freeze keeps 23+36 including TUs untouched |
| R-6 | Frontend phase-identity migration breaks a charsel edge case | M×M | frontend_sync_tests expanded first (M5 gate); M1's drop+log downgrades remain the safety net |
| R-7 | Two-pacer beat (scheduler vs vsync ONE on 60 Hz displays) | L×M | Absolute deadlines absorb Present blocking by design; `present_interval=immediate` escape documented; T-SCHED measures on both display classes |
| R-8 | ENet ch1 sequencing interacts badly with window merge | L×L | Property test T-CODEC/T-ENG-7 with reorder shim; fallback: unsequenced flag on InputStream (one line) |
| R-9 | Supervisor progress-deadline false positive (long legit stall, e.g. 15 s burst + slow resume) | L×M | Progress deadline only counts while inputs are absent AND transport silent is false; W-3/W-4 tests pin it; 8 s HUD warn precedes |
| R-10 | Schedule slip pressure to ship M5 without M6 | M×H | Hard rule in this plan: they ship together (§6.1); interim field builds stay on 0.7 branch |

### 8.2 Explicitly accepted risks

Weak-hardware sessions run at the slow machine's pace (P-1) — by design, labeled honestly.
Symmetric-NAT pairs without forwarding still need the relay-less fallback that doesn't exist
(unchanged from 0.6/0.7 — out of scope). 61-minute single rounds hit the vanilla force-end
(pre-existing engine behavior).

### 8.3 Fallback criteria (revert to branch `0.7`)

Fallback = ship 0.7 (GekkoNet + resilience M-work) while re0.7 continues in dev. Trigger if ANY:

1. M6 cutover gate not green within 3 weeks of M5+M6 code-complete (desync rate >1/100 harness
   matches unresolved, or any INV violated structurally).
2. §7.5 criteria 2–3 unreachable on min-spec after R-2 mitigations.
3. Field A/B (M8) does not show the LE-1 scenario fixed (any recurrence of hold-rate >1% of pace
   decisions at 0% loss).
4. A frontend regression class (charsel/rematch) with no drop+log downgrade available.

Partial-fallback option (pre-planned): M2 (scheduler) + M3 (transport/session/supervisor) are
individually shippable on top of GekkoNet if the engine cutover slips — they fix INV-5/14/17
independently and are strictly better than 0.7 alone.

---

## 9. Open questions — RESOLVED (user, 2026-08-17)

1. **Default cadence: CONFIRMED** — exact 60.000 Hz (`proper_60`) netplay default, `compat_58`
   opt-in, handshake-enforced match.
2. **Spectator scope: SHIP** — M7 ships in the same release as M5/M6.
3. **Mid-session delay hotkeys: SHIP** — keep the hotkeys exposed in the first release.
4. **Min-spec: pragmatic** — "whatever works best": use the 0.7 field-log machines as the de-facto
   perf-gate reference; tighten later if a weaker field machine appears.
5. **Interim builds: none to testers before functional readiness** — development continues on
   re0.7; field testing begins only once the netcode is functionally ready (M6 cutover gate).
   No v20 dev builds distributed before then.

---

## Appendix A — Reference-lesson traceability

### A.1 QOH99 §7 lessons register → plan sections

| QOH99 lesson | Where enforced here |
|---|---|
| 1. Full speed + zero desyncs ≠ healthy lifecycle | Lifecycle-progress assertions: ProgressDeadline (§2.4), rematch-soak epoch/Begin asserts (§7.4) |
| 2. Label every hold with its true cause, day one | run_state.h + hold-episode ledger landed in M0 (§2.10) |
| 3. One repayment owner per missed frame | CadenceDebt: typed-cause gate, applyRebasedFrames single entry (§2.8.5) |
| 4. Catch-up must work on predicted input | Hidden k bounded by `R − depth − 1`, predicted remotes (§2.8.3) |
| 5. The visible frame is not optional | INV-21; batch collapse rule (§2.8.3, §2.8.6) |
| 6. Exact-input windows are the fps killer, not rollbacks | Narrow initial window set + decomp audit M4-7 (§2.7.6); frontend gets own RTT delay (§2.9.3) |
| 7. Never feed tuning from ms-quantized/lobby clocks | time_probe µs QPC + dwell + generation gate; strong-typed µs (§2.9.3) |
| 8. Units in names, conversion at caller | `*_us` naming rule (§2.9.3) |
| 9. Ramps over latches, asymmetric toward cheap mistake | Frontend-delay latch 0.15/0.30 s, ≥12 samples (§2.9.3) |
| 10. Drain before judging; anchor liveness to first pump poll | §2.2 (both bullets), §2.6.2 pump order |
| 11. A stall must not stop feeding the opponent | INV-24 producer + PressureReport (§2.7.3-P) |
| 12. Hash membership is a product decision | INV-22 + M4-6 region-role audit (§2.7.7) |
| 13. Peer-local everything | INV-23 (§2.9); remaining two-party agreements all reliable barriers at canonical boundaries |
| 14. No hold negotiation | Non-goal §1.2; independent holds compose (B-3) |
| 15. Solve for full speed, not the threshold | Recommend required+1 (§2.9.1); Marginal HUD state |
| 16. Fail closed, loudly, self-describing | INV-20; Disconnect payload §3.2; dumps D-1 |

### A.2 TrialNetplay adopt/avoid → plan sections

| Item | Where |
|---|---|
| Adopt 1: retained speculation, rollback only on real mismatch | §2.7.3-R, §2.7.4 |
| Adopt 2: never fabricate inputs; fail closed | INV-19 |
| Adopt 3: ack = contiguous count + trailing window every datagram | §3.2 InputStream (`ack_through` + window) |
| Adopt 4: desync detection as first-class subsystem | §2.7.7 (SyncHash + dumps + resim self-check) |
| Adopt 5: hash only confirmed frames | §2.7.3-F ("predicted value can never enter a hash") |
| Adopt 6: split RNG streams / FPU pinning | FPU/MXCSR per-slot (§2.7.7); cosmetic RNG excluded from hash (INV-22); AS2's DetVer seeding kept (§4.3) |
| Adopt 7: session-ID/generation tagging before any mutation | §3.1 session_id; epochs §2.5 |
| Adopt 8: mod-owned exact QPC fractional pacing | §2.8.2 Bresenham carry |
| Adopt 9: harness before WAN | §7.2 |
| Avoid 1: pacing/wall-clock state in snapshots | INV-22; M4-6 audit explicitly excludes timing state |
| Avoid 2: out-of-region sim state | R-3 + aux sweep task |
| Avoid 3: speculation across irreversible transitions | INV-25 (§2.7.6) |
| Avoid 4: raw async input polling as sync source | P-5 (queued confirmed-boundary lifecycle keys); SDL once-per-frame capture (§2.7.3-C) |
| Avoid 5: game-local counters as epochs / narrow epoch fields | §2.5 (u32 host-minted transport generations) |
| Avoid 6: hidden re-sim side effects | §2.8.6 conditions (a)–(d) |
| Avoid 7: two-instance same-folder hazards | §7.2 separate folders |
| 5-step nonce handshake, exact-size validation, host-authoritative GO | §4.2, §3.1, GameplayStart barrier (§2.5) |
| NOT adopted: min-of-both rollback negotiation, host-authoritative delay, external launcher, 9× duplicate control sends | INV-23; non-goals §1.2; (barriers use targeted resend instead of blind duplication) |

### A.3 Inventory §2 API accounting

| Contract | Provider in new architecture | Notes |
|---|---|---|
| `Session_*` (17 fns + types, §2.1) | session2 | Verbatim header; `GetMsSinceLastInbound` re-based on protocol silence |
| `PregameSync_*` (12 fns, §2.2) | match_setup | Verbatim; `PregamePhase` enum kept as reporting vocabulary |
| `OnlineWiring_*` (13 fns, §2.3) | match_director | Verbatim |
| `RollbackSession_*` (§2.4 full list) | engine2 adapter | §2.7.8 table; `BufferGekkoPacket` deleted; `gekko_*` fields renamed |
| `DelayPolicy_*` | kept module, re-fed | Measurement from time_probe; `OnRollbackApplied` from finishRollback; negotiation builders retired with the wire flow |
| `SyncPolicy_*`, `MatchLifecycle_*`, `NetplayPhaseRuntime_*` | kept modules | Re-derived from new backend states; director calls every `MatchLifecycle_On*` |
| `TickHooks` setters/getters | kept, writers deleted | Scale pinned; limiter-preference getters stay for menu/settings |
| `InputSyncHooks_Set*Freeze/IsModOwnedSync` | driven by scheduler + match_setup | `SetLoadBarrierFreeze` driven by match_setup (load/fast-path windows); `SetTimesyncFreeze` is absorbed by the scheduler's N=0 path (same underlying hooks: dispatcher −1 + AdvanceFrame suppression), kept as the primitive it drives |
| `TransitionBarrier_*` | kept module | Gains EpochAlign kind; routed by packet_router; FrameUpdate in pump |
| `ConnectionSupervisor_*` | kept module | Re-fed (INV-14) + ProgressDeadline; verdict consumed by session2/engine gates |
| `Transport_*ForHost` + fault-injection query | transport2 | Preserved for spectator sidecar + rematch_soak |
| `SpectatorRuntime_On*` push set | engine2/match_director | Confirmed-only stream (S-6) |
| `StateHistory_*` / `GameSnapshot_*` / `Savestate_*` | kept, engine2 consumes | Stays public for replay/training (never privatized) |
| fx journals `RollbackAudio/ComboFx/StatusFx_*` | director calls Begin/End; engine flags suppress | §2.6.1/§2.8.6 |
