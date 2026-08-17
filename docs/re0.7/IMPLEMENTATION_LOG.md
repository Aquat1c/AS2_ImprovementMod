# re0.7 Implementation Log (append-only)

Handoff journal for the milestone implementation agents. Per task: files
touched, what was done, deviations from the plan text, and anything the next
milestone must know. Plan references are to `RE07_MASTER_REBUILD_PLAN.md`.

---

## 2026-08-17 — M0 + M1 (this entry: both milestones, implemented together)

### M0-1: Extract `RollbackTimesyncTelemetry` → `include/rollback/rollback_telemetry.h`

- **Files:** `include/rollback/rollback_telemetry.h` (new),
  `include/rollback/rollback_session.h`, `src/rollback/rollback_session.cpp`,
  `include/net/churn_pause.h`, `include/net/netplay_pacing.h`,
  `src/net/churn_pause.cpp`, `src/net/netplay_pacing.cpp`,
  `src/net/delay_policy.cpp`, `src/patches/input_override.cpp`, `CMakeLists.txt`.
- **Done:** struct moved to the new type-only header; `gekko_avg_ping/gekko_jitter`
  → `link_avg_ping/link_jitter`; all field consumers renamed (netplay_pacing ×5,
  delay_policy ×3, input_override ×2, rollback_session fill site).
  `churn_pause.h`/`netplay_pacing.h` now include only `rollback_telemetry.h`
  (fully decoupled from the facade header); `churn_pause.cpp`'s direct
  `rollback_session.h` include deleted (it used no symbol from it).
  `rollback_session.h` includes the new header so its 23 existing includers
  keep compiling unchanged.
- **Deviation:** the plan's task table lists only 3 files; the field rename
  actually reaches `delay_policy.cpp` and `input_override.cpp` too (telemetry
  *field* consumers the inventory table didn't enumerate). Renamed there as well.
- **Next-milestone note:** `RollbackSessionSnapshot.gekko_avg_ping/gekko_jitter`
  (the *snapshot*, consumed by rollback_debug/online_wiring/gameplay_bridge) is
  deliberately NOT renamed at M0 — inventory §11 schedules that rename with the
  M5 facade cutover.

### M0-2: Extract `OnGameplayPacket` → `src/net/gameplay_packet_router.cpp`

- **Files:** `include/net/gameplay_packet_router.h` (new),
  `src/net/gameplay_packet_router.cpp` (new), `src/rollback/online_wiring.cpp`,
  `include/rollback/online_wiring.h`, `CMakeLists.txt`.
- **Done:** the ~300-line dispatch switch moved verbatim into
  `Net::GameplayPacketRouter_OnPacket` (seed of the §2.3 packet_router). The two
  engine-coupled cases stayed in online_wiring behind two new functions
  (`OnlineWiring_HandleEngineDataPacket` — InputStream buffering + pre-live drop
  logging; `OnlineWiring_HandleStartupBarrierPacket` — GekkoReady READY/ACK
  state), because they read/write online_wiring's startup-barrier statics.
  The 3 `Session_SetPacketCallback(OnGameplayPacket)` sites now register the
  router. The router carries its own anomaly-log helper (RollbackSession-based
  log frame instead of online_wiring's private counters — log content differs
  by the dropped `gameplay_active/dispatched/remote_inputs` fields only).
- **Next-milestone note (M3):** packet_router promotion should absorb the
  deferred-flush semantics from `Session_SetPacketCallback` and route the two
  engine sinks through the routing table instead of the OnlineWiring_Handle*
  indirection.

### M0-3: Delete dead includes

- **Files:** `src/net/connection_supervisor.cpp` (`net/network_thread.h`),
  `src/net/barrier_protocol.cpp` (`net/match_bootstrap.h`).
- **Done:** both deleted; no symbol use existed (inventory-verified).

### M0-4: Invert frontend_input_sync → pregame dependency

- **Files:** `include/net/frontend_input_sync.h`,
  `src/net/frontend_input_sync.cpp`, `src/net/pregame_sync.cpp`.
- **Done:** new `FrontendWinScreenSendGate` predicate +
  `FrontendInputSync_SetWinScreenSendGate()`. The winscreen-send gate in
  `SendInputPacket` now consults the injected predicate; `pregame_sync.h`
  include removed from frontend_input_sync.cpp. `PregameSync_Init` registers
  `PregameWinScreenSendGate` (phase == Idle || GameplayHandoff — the exact
  predicate previously computed inline); `PregameSync_Shutdown` unregisters.
  Null gate ⇒ sends allowed, which preserves the test harness behavior (the
  tests' `PregameSync_GetPhase` stub returned Idle ⇒ allowed; the stub is now
  unreferenced but left in place harmlessly).

### M0-5: Fix pre-existing failing test `frontend_sync_tests.cpp:418` (winscreen confirm-hold)

- **Files:** `src/net/winscreen_sync.cpp` (the module that was actually wrong).
- **Done:** skip propagation in `WinScreenSync_ConsumeCurrentFrame` changed from
  an edge (`localAdvance || remoteAdvance` on the current frame only) to a level
  (`FrontendInputSync_Local/RemoteAdvanceObserved()`): once the advance gate has
  released, the confirm mask stays asserted on every consumed frame until native
  Mode 9 exits or the continue prompt takes ownership
  (`ContinueFlow_IsPromptActive()` still disables propagation entirely, so the
  prompt's per-player choices are untouched). Rationale: the native win screen
  samples the buttons per substate frame; a one-frame pulse released the skip on
  exactly one frame — the test's documented expectation ("keep confirm held
  until native Mode 9 exits") is the intended behavior.
- **Behavior-change note:** this is the one M0 task that intentionally changes
  runtime behavior (it is a bug fix pinned by an existing test).

### M0-6: `rollback/run_state.h` + hold-episode ledger + §2.10 STAT line

- **Files:** `include/rollback/run_state.h` (new),
  `include/rollback/netplay_log.h`, `src/rollback/netplay_log.cpp`,
  `src/net/netplay_pacing.cpp`, `CMakeLists.txt`.
- **Done:**
  - `run_state.h`: closed `HoldCause` enum (INV-2 set: PredictionLimit /
    LifecycleBoundary / LocalInputMissing / ExternalSuspension), `WorkKind`,
    `RunState` (§2.10 vocabulary), pure/total `ClassifyRunState`, and
    `HoldEpisodeLedger` — ported from QOH99 `game/NetplayRunState.h` with the
    plan's trimmed enum (no HoldNegotiating — the hold-negotiation protocol is
    a §1.2 non-goal; no RecoveryCatchUp state — the §2.10 enum omits it, debt
    repayment is visible via the STAT `debt` field instead).
  - `NetplayLog_Stat(frame, NetplayStatSample)` — the frozen STAT line format
    (tag `STAT`): `sim_fps= present_p50_us= present_p99_us= hold_pred=
    hold_life= hold_input= hold_ext= rollbacks= rb_max= slew_ppm= debt=
    silence_ms=`. **This format is the §7 acceptance instrument — do not change
    it**; M2's scheduler and M4's engine must emit through this same call.
  - Per-second emission wired "behind existing telemetry" in netplay_pacing
    (`UpdateStatRollup`, fed from `OnSessionSample`/`OnHoldSample`, reset in
    `ResetSession`): sim_fps from rb-frame deltas, rollbacks/max-depth from
    telemetry counters, debt from the legacy debt value, silence from
    `ConnectionSupervisor_GetInboundSilenceMs()`.
- **Deviation / limitation (temporary by design):** the legacy pacing layer
  cannot label hold causes, so ALL its holds land in `hold_pred`;
  `present_p50/p99`, `slew_ppm`, and the life/input/ext buckets emit 0 until M2
  (scheduler) and M4 (typed `nextAction` plan) take over the same line.
  `run_state.h` has no runtime consumer yet — M2's scheduler classifies with it
  first. When netplay_pacing's controller is deleted at M2 (§2.8.7), the STAT
  emission must MOVE to FrameScheduler, not die with the module.

### M1-1: API freeze doc + struct-size pins

- **Files:** `docs/re0.7/API_FREEZE.md` (new), `include/net/session_manager.h`,
  `include/net/protocol.h`.
- **Done:** checklist of every preserved contract (inventory §2.1–2.5);
  static_asserts: `SessionSnapshot == 472`, `PeerInfo == 96`,
  `ConnectionStats == 40` (MSVC Win32 default packing — hand-computed, see
  compile-risk note below), palette payloads 20/1044/16, frontend payloads
  56/56/20/36, all new v2 payloads. `LockedMatchConfig == 20` already existed.

### M1-2: protocol.h v2

- **Files:** `include/net/protocol.h`, `include/net/barrier_protocol.h`,
  `src/net/transition_barrier.cpp`, `src/net/pregame_sync.cpp`,
  `src/net/winscreen_sync.cpp`, `include/net/winscreen_sync.h`,
  `src/rollback/rollback_session.cpp`, `src/net/gameplay_packet_router.cpp`.
- **Done:**
  - `PROTOCOL_VERSION` 18 → **20** (19 skipped per §3.1).
  - **Deleted** (dead): `SessionMeta` (5), `GameplayInput` (20),
    `WinScreenConfirm` (41) — plus their switch cases in barrier_protocol.h,
    pregame_sync.cpp, PacketTypeName, and the now-caller-less
    `WinScreenSync_OnRemoteConfirm()` (decl + def).
  - **Renamed:** `GekkoData` → `InputStream` (id 23 unchanged; old backend
    still transports raw Gekko bytes under it — pure rename, no behavior change).
  - **Added** §3.2: `SessionHello/Offer/Ack/Confirm/ConfirmAck` (70–74) with
    echo-verbatim payload nesting (INV-13 by construction), `InputStreamPayload`
    + `PressureReport` (fixed 32-slot window + count; senders will transmit the
    used prefix), `SyncHash` (75) / `SyncHashAck` (76), `TimeProbe` (77) /
    `TimeProbeAck` (78), `ResyncReply` (63) + a real `ResyncRequestPayload`
    (id 62 was reserved/declared-only), `FrontendPhaseId` fixed enum (§3.4),
    `NetTransitionKind::EpochAlign` (6) + `{epoch, first_phase, native_mode}`
    fields appended to `PhaseTransitionPayload` (zero for other kinds;
    `kMaxKinds` 6→7 in transition_barrier.cpp).
  - **`(epoch, phase_id)` on frontend payloads:** `phase_id` (u8,
    `FrontendPhaseId`) carved out of existing pad bytes in
    `CharSelFrameInputPayload`, `WinScreenFrameInputPayload`,
    `FrontendPhaseBarrierPayload`, `FrontendBoundaryDigestPayload` — **wire
    sizes unchanged**, field is zero until M5. (`epoch_id` already existed.)
  - barrier_protocol.h classification: `InputStream`/`TimeProbe`/`TimeProbeAck`
    → unreliable CHANNEL_GAMEPLAY; new session/hash types fall through to the
    reliable-control default (correct).
- **Deviations (intent-over-letter, per the M1 exit gate "old backend still
  running on the old packet set"):**
  1. §3.3 lists `Hello/HelloAck`, `DelayChangeReq/Ack`, `GekkoReady`, and the
     `SyncAnnounce/SyncConfirm` delay fields as retired — but all have LIVE
     send/receive sites in the shipping backend (session_manager handshake,
     frontend delay-bump flow, startup barrier, pregame announce). Deleting
     them at M1 would contradict the exit gate, so they are **kept and marked
     LEGACY** in the enum with their cutover milestone (M3 for the handshake,
     M5 for the rest). The M3/M5 agents must delete them and their flows.
  2. The per-side `phase_serial` field is NOT removed from the frontend
     payloads at M1 (plan §3.2 says "serial field removed") — it is the live
     acceptance key of the current frontend lockstep; removal happens with the
     §3.4 acceptance-rule cutover (M5). Marked "(retired at M5)" in the struct.
  3. `SessionConfirmPayload` name: the legacy pregame struct is
     `SyncConfirmPayload`, so no collision; the v2 handshake struct uses the
     plain name.
- **Next-milestone notes:**
  - M3 session2: use `SessionHelloPayload` etc.; `session_id` =
    `fnv1a64(client_nonce ‖ host_nonce ‖ host_seed)`; per-step 200 ms resend,
    10 s step / 30 s total timeouts (§4.2). Add `session_id u64` enforcement on
    gameplay-phase packets when the v2 stream goes live.
  - v18 peers: version mismatch now fails closed at the legacy Hello validator
    (it compares `PROTOCOL_VERSION`) — intended.

### M1-3: `net/frame_arithmetic.h` + unit tests

- **Files:** `include/net/frame_arithmetic.h` (new),
  `tests/frame_arithmetic_tests.cpp` (new), `CMakeLists.txt` (new test target
  `frame_arithmetic_tests` + `add_test`).
- **Done:** ported 1:1 from `d:\dev\qoh_99\qoh99_netplay\src\net\FrameArithmetic.h`
  (`kHalfSerialSpace`, `frameAfter`, `frameBefore`, `frameAtOrAfter`,
  `forwardDistance`, `signedLead`), namespace `qnet` → `Net` (only change).
  Tests pin: basics, wrap crossing, exact half-space boundary (not-after in
  both directions), forwardDistance across wrap, signedLead sign/wrap, and
  constexpr usability.

### Build-system summary (M0+M1)

- CMake: added `src/net/gameplay_packet_router.cpp` to `NET_SOURCES`; headers
  `gameplay_packet_router.h`, `frame_arithmetic.h`, `rollback_telemetry.h`,
  `run_state.h` to the header lists; new `frame_arithmetic_tests` target.
- No files deleted from the build in M0/M1 (deletions of `network_thread`,
  `session_manager` internals, GekkoNet, `netplay_pacing` etc. are M2/M3/M5
  tasks).

### Compile risks to check first (for the build session)

1. `static_assert(sizeof(SessionSnapshot) == 472)` (and PeerInfo 96 /
   ConnectionStats 40) in `session_manager.h` — hand-computed MSVC x86 layout
   (uint64_t alignment 8 inside `ConnectionStats`). If the assert fires, the
   compiler's number is the truth: update the constant, not the struct.
2. `frontend_sync_tests` links `winscreen_sync.cpp` — the removed
   `WinScreenSync_OnRemoteConfirm` and the new advance-level propagation are
   exercised there; expect the previously failing check at line 417/418 to pass.
3. `gameplay_packet_router.cpp` is a new TU aggregating many headers — first
   place an include or namespace slip would surface.
4. Anywhere in the tree that still spelled `PacketType::GekkoData/GameplayInput/
   SessionMeta/WinScreenConfirm` — swept clean (only comments and the dead
   `tests/test_packet_codec.cpp`, which belongs to the disabled legacy target
   and uses its own `PacketCodec::PacketType`).

---

## 2026-08-17 — M2 (FrameScheduler + clock pinning)

### M2-1: `frame_scheduler` — deadline math, cadence profiles, semanticHold, rebase reporting, slew hook, STAT

- **Files:** `include/patches/frame_scheduler_core.h` (new),
  `include/patches/frame_scheduler.h` (new),
  `src/patches/frame_scheduler.cpp` (new),
  `tests/frame_scheduler_tests.cpp` (new), `CMakeLists.txt`.
- **Done:**
  - Pure core (`Sched` namespace, zero Win32/game includes, unit-tested):
    `DeadlineClock` (§2.8.2 Bresenham carry — integer-exact, any `div`
    consecutive steps sum to exactly `qpf*mul` ticks; sub-frame lateness
    preserves the schedule; >2-period lateness rebases with frame count,
    never compresses), `PaceSlew` (§2.8.4: deadband ≥2 enter / <1 release,
    2500 ppm/frame target, 8000 ppm rise cap, 25000 ppm absolute cap,
    instant release on stale), `CadenceDebtLedger` (§2.8.5: max 8,
    CreateOne only for debt-declared PredictionLimit holds,
    `ApplyRebasedFrames` typed-cause gate, `DiscardExternal`),
    `IntervalStats` (per-second percentile window).
  - Driver (`FrameScheduler_*`): cadence profiles `proper_60` {1,60} /
    `compat_58` {17,1000} selected live from
    `IsFrameLimiter60FpsPatchEnabled()` (the shipping `proper_60fps` /
    session `frame_timing` override — §2.8.7 survival path); hybrid wait
    (Sleep(1) until 2 ms, `_mm_pause` tail); QPC-regression rebase;
    dead-clock latch (500 Sleep rounds with frozen QPC → loud
    `PacingClockDead` log + rebase; the fail-closed *terminal* needs
    session2 and lands at M3 — deviation, noted in the code); per-pass
    accounting with first-cause-wins hold labeling; `run_state.h`
    classification (its first runtime consumer, per the M0 note) +
    `HoldEpisodeLedger` observation; manual speed (mod-menu slider /
    replay fast-forward) realized by dividing the period — one speed
    authority (INV-5).
  - **STAT emission MOVED here from netplay_pacing** (M0 obligation): per
    second via `NetplayLog_Stat` — sim_fps from pass accounting,
    present_p50/p99 from wait-entry intervals, holds bucketed by typed
    HoldCause, rollbacks/rb_max from rollback telemetry deltas, slew_ppm,
    debt from the ledger, silence from the supervisor. Frozen format
    untouched.
  - Tests pin T-SCHED-1..4 (drift-zero over 600k/1M simulated frames both
    profiles, rebase threshold + schedule preservation, semanticHold/debt
    typed-cause gate, slew admission/rise/cap/hysteresis/stale-release)
    plus percentile sanity. New CMake target `frame_scheduler_tests`.
- **Deviations:**
  1. STAT emits only while the scheduler is installed (the emission point
     is the detour). In the R-1 fallback mode there is no STAT line — the
     install failure is logged loudly instead.
  2. Present intervals are measured wait-entry→wait-entry at the limiter
     site (includes Present blocking — which is what T-SCHED measures);
     the proxy is not instrumented.
  3. sim_fps counts a pass with no dispatcher notifications (offline /
     menus / frontend lockstep waits) as 1 sim — frontend lockstep waits
     are not labeled holds (STAT is the gameplay acceptance instrument;
     frontend holds would need a phase-aware producer that arrives at M5).

### M2-2: Limiter detour (byte-signature, install-or-fail-loud)

- **Files:** `src/patches/frame_scheduler.cpp` (scanner/patcher),
  `src/patches/hook_installer.cpp`, `include/core/as2_constants.h`.
- **Done:** signature scan over `[ADDR_GAME_MAINLOOP, +0x600)` for
  `E8 <rel32→0x635F80> / 2B 05 60 63 81 00 / 83 F8 11 / jl-backward`
  (short `7C` and near `0F 8C` forms accepted; call target verified by
  computed rel32; jl displacement must be negative). Exactly-once match
  required; the whole 16/20-byte cluster is replaced by
  `call FrameScheduler_WaitForNextFrame` + NOPs (vanilla `jl`
  structurally neutralized). The following re-stamp
  (`call sub_635F80; mov dword_816360, eax`) is untouched → FPS
  bookkeeping coherent. Original bytes restored in `RemoveHooks` →
  `FrameScheduler_Shutdown`. New constants
  `ADDR_FRAME_LIMITER_SCAN_BEGIN/SIZE`. Install called at the end of
  `InstallHooks`; failure logs loud and leaves the legacy path running
  (R-1). Config kill-switch `frame_scheduler=0` in
  `as2_rollback_settings.ini` `[ModSettings]` (the R-1 "config flag for
  one release"). `timeGetDevCaps` asserted at install (log-only).
- **Register-safety note:** the detour replaces a `call sub_635F80`
  (cdecl) at the same site, so caller-saved clobbers are identical by
  construction; cdecl preserves ebx/esi/edi/ebp.

### M2-3: `Hook_GetTick` pinned to 1.0; writers deleted; reset → rebase

- **Files:** `src/patches/tick_hooks.cpp`, `include/patches/tick_hooks.h`,
  `src/net/netplay_pacing.cpp`, `src/net/spectator_playback.cpp`.
- **Done:**
  - `SetNetplayTickScale`, `SetNetplayTickScaleTarget`,
    `SetNetplayPacingActive` deleted (decl + def). Netplay slew/pacing
    fields of `NetplayTickState` pinned to 1.0/false (struct kept for the
    snapshot API); `GetNetplayTickScale()` returns 1.0.
  - `Hook_GetTick` with scheduler installed = passthrough at scale 1.0
    (100 ms delta clamp preserved); without it (R-1 fallback) the legacy
    manual×1.02 virtual clock remains, now with BOTH DECOMP §5(c)
    mandatory fixes: (1) `ResetNetplayTickScaleState` rebases —
    `virtual_tick_ms` preserved, `last_real_tick_ms` re-anchored — the
    backward-snap class (§2.3 #1) is closed even in the pinned shim;
    (2) main-thread confinement — non-main callers (AVI worker) get the
    last published tick, race-free.
  - `netplay_pacing` sink swap (DECOMP §5 recommendation 3): the EMA
    adjust now feeds `FrameScheduler_SetPeriodAdjustUs(adjust_ms*1000)`
    (±1000 µs clamp in the scheduler); `DeactivatePacing`/Init/Reset
    clear it; `NetplayPacing_ResetSession` additionally calls
    `FrameScheduler_OnSessionReset` (adjust/slew/debt cleared, deadline
    continuous). Snapshot `target_scale/current_scale` now report the
    period-equivalent scale (`base/(base+adjust)`) so
    online_wiring/rollback_debug consumers keep meaning. STAT rollup
    block deleted (moved, M2-1); `connection_supervisor.h` include
    dropped.
  - `spectator_playback` only ever wrote neutral scales — calls replaced
    with `FrameScheduler_SetPeriodAdjustUs(0)`; `tick_hooks.h` include
    dropped.
- **Deviation:** the M2 task table says "netplay_pacing.cpp callers"; the
  writers also had call sites in `spectator_playback.cpp` (all neutral
  no-ops) — swept with the same change. Manual speed
  (`SetGlobalTickScale` — mod menu, replay fast-forward) survives and is
  applied through the scheduler period, not the clock, keeping replay
  speed control working under the pin.
- **Fallback caveat (documented):** in R-1 fallback mode, netplay runs on
  the fixed 1.02 correction with holds only — the continuous adjust
  no-ops (scheduler absent). Degraded but functional; loud at install.

### M2-4: d3d9 proxy `present_interval=immediate` config key

- **Files:** `d3d9_proxy/d3d9_proxy.cpp`.
- **Done:** the scaling swap chain's `PresentationInterval` (the single
  present-interval owner, DECOMP §4.3) now reads
  `[ModSettings] present_interval` from `as2_rollback_settings.ini`:
  `default` (≙ vsync ONE, unchanged default) or `immediate` — the
  documented escape for non-60 Hz-multiple displays / two-pacer beat
  (§2.8.1). Logged once via ProxyLog.

### M2-5: Dispatcher N∈{0,1,1+k} plumbing

- **Files:** `src/patches/input_override.cpp`,
  `src/patches/input_sync_hooks.cpp`.
- **Done:**
  - Hold passes (already −1 + AdvanceFrame-suppression pulse + vanilla
    timeout clears — DECOMP §6.1, all three halves, unchanged) now report
    their typed cause to the scheduler: legacy pacing holds →
    `PredictionLimit` (or `ExternalSuspension` when
    `ChurnPause_ShouldForceHold` is the trigger), startup/pre-live gates
    and the pregame load-barrier freeze → `LifecycleBoundary`. All with
    `create_debt=false` — **the M2 exit-gate semanticHold shim**: legacy
    holds consume their slot at the normal deadline and owe nothing.
  - Normal (non-rollback) Advance → `FrameScheduler_NotifySimFrame()`
    (rollback-replay advances are re-runs and are not counted).
  - Catch-up generalized to k at the Done boundary: when
    `FrameScheduler_TryTakeCatchupFrame(R−depth−1)` grants (debt owed,
    <2 extras this pass, headroom positive, <6000 µs wall budget), one
    more BeginFrame cycle runs inside the same pass with a FRESH device
    sample (DECOMP §6.2), crediting `NotifyCatchupFrame` per executed
    hidden frame (partial batches keep their credit). This is the 0.6
    double-tick machinery generalized — and **inert on the Gekko path**
    by construction, because nothing creates debt until engine2's typed
    PredictionLimit holds at M4 (protects the "0.6 Gekko netplay still
    works" exit gate).
  - `InputSyncHooks_SetLoadBarrierFreeze(true)` →
    `FrameScheduler_DiscardExternalDebt` (§2.8.5 load-stall discard).
- **Deviation:** §2.8.5 says "CreateOne on PredictionLimit holds"; at M2
  the legacy shim deliberately does NOT create debt (see exit gate:
  "netplay_pacing holds temporarily map to semanticHold"). The
  `create_debt` parameter is the switch engine2 flips at M4.

### Build-system summary (M2)

- CMake: `src/patches/frame_scheduler.cpp` added to `PATCH_SOURCES`;
  `frame_scheduler.h`/`frame_scheduler_core.h` added to `PATCH_HEADERS`;
  new test target `frame_scheduler_tests` (+`add_test`). No files removed
  from the build (netplay_pacing survives until M6; GekkoNet untouched).

### Obligations for next milestones

- **M3 (session2):** wire `PacingClockDead` and the 20 s progress
  deadline into the `Session2_Terminate` funnel; supervisor re-feed does
  not change the STAT `silence_ms` source name
  (`ConnectionSupervisor_GetInboundSilenceMs` is re-implemented over
  protocol silence per §2.2).
- **M4 (engine2):** typed `nextAction` holds must call
  `FrameScheduler_NotifyHold(cause, create_debt = cause==PredictionLimit)`
  and feed `FrameScheduler_SubmitPeerDepthSample` from PressureReport
  (the §2.8.4 input hook exists and is released-by-staleness until then);
  `LocalInputMissing` currently has no producer (the legacy path cannot
  detect it) — first emitter is engine2.
- **M5 (frontend identity):** frontend lockstep waits are unlabeled at M2
  (see M2-1 deviation 3); if frontend-phase STAT visibility is wanted,
  add a phase-aware hold producer with the §3.4 migration.
- **M6:** delete netplay_pacing controller + its shim sink call sites
  (`FrameScheduler_SetPeriodAdjustUs` loses its last legacy caller), HUD
  reads `FrameScheduler_GetSnapshot` (run_state/holds/slew/debt readouts
  replace the NETCLASS/debt readouts).

### Compile risks to check first (M2 build session)

1. `frame_scheduler.cpp` is a new TU pulling `mmsystem.h` under
   `WIN32_LEAN_AND_MEAN` (timeGetDevCaps/TIMECAPS) — winmm.lib is already
   linked; if TIMECAPS is missing, include order vs lean-and-mean is the
   suspect.
2. `input_override.cpp` now includes `net/churn_pause.h` — check for
   duplicate/ambiguous includes; `ChurnPause_ShouldForceHold` signature
   is `(telemetry, phase, startup_released)`.
3. `RollbackTimesyncTelemetry` field types are `int32_t`
   (`rollback_count`, `prediction_debt`, `rollback_budget`) — the
   catch-up headroom math in input_override and the STAT anchor math in
   frame_scheduler assume that.
4. `frame_scheduler_core.h` uses `<algorithm>` (std::sort) and includes
   `rollback/run_state.h` — both header-only/pure; the new
   `frame_scheduler_tests` target compiles it standalone.
5. Any straggler caller of the deleted
   `SetNetplayTickScale/Target/SetNetplayPacingActive` — grep-verified
   zero remaining references at edit time.

---

## 2026-08-17 — M3 (transport2 + session2 swap under the unchanged `Session_*` contract)

### M3-1: `net/transport2` — ENet worker with protocol_silence_ms; network_thread deleted

- **Files:** `include/net/transport2.h` (new), `src/net/transport2.cpp` (new),
  `src/net/enet_transport.cpp`, `include/net/enet_transport.h`,
  `src/net/network_thread.cpp` (DELETED), `include/net/network_thread.h`
  (DELETED), `CMakeLists.txt`.
- **Done:**
  - Worker thread/command-queue/event-queue model carried over from
    network_thread (1-2 ms `enet_host_service` cadence, game thread never
    touches ENet objects, callbacks fire from the `Session_Update` drain —
    every reentrancy assumption in KEEP code preserved).
  - **`protocol_silence_ms` (INV-14):** derived per service pass from ENet's
    `peer->lastReceiveTime` (any inbound command counts: acks, pings,
    fragments) folded with authenticated autopunch keepalive accepts. New
    `Transport_AutopunchLastInboundTickMs(host)` helper +
    `last_authenticated_inbound_ms` stamp in `AutopunchHandleKeepalive`
    (stamped only after the connectID check passes — a spoofable foreign-IP
    keepalive never counts as liveness). Silence keeps growing from the last
    genuine inbound after a peer detach (a detached peer must not look
    fresh); 0xFFFFFFFF = nothing ever received.
  - **Liveness anchor (QOH99 lesson 10):** anchor re-stamped on the first
    worker poll after every StartHost/StartJoin/DestroyHost — the
    pre-establishment budget can never reference process start.
  - **Busy refusal (C-5/C-6):** a surplus inbound ENet connect (second peer
    while one is live, or any inbound that is not our own pending outbound
    while joining) is refused with `DisconnectReason::Busy` (new enum value
    5 in session_types.h) without touching the existing peer/attempt.
    session2 surfaces the Busy data word as "Host is busy with another
    session" on the refused side.
  - Peer-resilience config, autopunch start/keepalive/rebind-heal service
    calls, hole-punch bursts, and fault injection all unchanged
    (enet_transport keeps them; `Transport_FaultInjectionActive` and the
    `*ForHost` spectator helper set preserved verbatim).
- **Deviations:**
  1. Plan says "shrink enet_transport to helpers" — enet_transport already
     IS the helper layer (host/peer/send/autopunch/fault-injection, no
     session logic), and transport2 drives its single-peer helpers from the
     worker exactly as network_thread did. Nothing was moved out of it;
     network_thread.* is the deletion. No dead code remains.
  2. Queues are mutex+deque (inherited), not literal SPSC rings — same
     threading contract (one producer, one consumer per direction), kept to
     avoid rewriting proven code; revisit only if profiling ever shows lock
     contention.

### M3-2: `net/session2` — Session_* facade re-implementation, 5-step handshake, teardown funnel

- **Files:** `include/net/session2.h` (new), `src/net/session2.cpp` (new),
  `src/net/session_manager.cpp` (DELETED; `session_manager.h` header stays
  as the preserved facade), `include/net/session_manager.h` (comments only),
  `include/net/protocol.h`, `include/net/session_types.h`,
  `include/net/barrier_protocol.h`, `src/patches/frame_scheduler.cpp`,
  `include/patches/frame_scheduler.h`, `src/ui/netplay_hud.cpp` (comment),
  `docs/re0.7/API_FREEZE.md`.
- **Done:**
  - All 17 `Session_*` facade functions re-implemented byte/shape-stable
    (SessionSnapshot/PeerInfo/ConnectionStats static_asserts untouched).
    NAT plumbing, relay fallback, build-fingerprint lock, stats/status
    machinery, worker-detach watchdog, queue-spike logging carried over.
  - **5-step nonce handshake (§4.2)** replaces Hello/HelloAck: joiner sends
    `SessionHello` on ENet connect; host validates proto/build/cadence
    fail-closed with the exact field named in a reliable Disconnect + ENet
    VersionMismatch data (C-3 — both sides show it); `SessionOffer` echoes
    the received Hello BYTES verbatim and `SessionAck` echoes the Offer
    BYTES (INV-13 by construction — memcmp against the sent struct, nothing
    recomputed); both compute
    `session_id = fnv1a64(client_nonce || host_nonce || host_seed)` and
    exchange it via `SessionConfirm`/`SessionConfirmAck`. 200 ms per-step
    resend, duplicate-step packets re-acked idempotently (incl. Confirm
    after the client is already Connected), 10 s step / 30 s total timeout
    -> `SetError` back to menu (C-4: no session existed, not a kill path).
    Nonces from std::random_device XOR QPC (fresh nonzero per attempt, C-7).
  - **Cadence enforcement:** `SessionHello` carries the §2.8.2 rational
    ({1,60} proper_60 / {17,1000} compat_58 from
    `IsFrameLimiter60FpsPatchEnabled()`); mismatch = fail-closed refusal
    naming the field. NOTE the deliberate behavior change vs the legacy
    handshake: the joiner no longer silently adopts host timing — the plan
    (§2.8.2, C-3) mandates handshake-enforced equality.
  - **`PeerIdentity` (new packet 79, reliable ch0):** one-shot exchange on
    entering Connected carrying full nickname[64], listen_port, round
    option, frame timing, HUD style — fills the preserved PeerInfo contract
    (mod_main HUD-style consumer, menu round/nickname display, replay
    naming). Joiner applies the host's round option here (was: HelloAck);
    handshake short nickname (16 B) seeds PeerInfo until it lands.
  - **Teardown funnel `Session2_Terminate(reason, detail)`** (session2.h):
    typed `Session2TerminalReason`, reasoned reliable Disconnect when a
    peer is reachable, transport disconnect/destroy/clear, Nat stop,
    settings restore; UserCancel -> Idle, GameExit -> bounded 150 ms goodbye
    (WM_CLOSE path, no teardown), fault reasons -> Failed with the detail as
    error text. Callers: Session_Cancel, Session_NotifyGameExit, handshake
    refusals, supervisor Dead/ProgressDeadline, PacingClockDead.
  - **PacingClockDead terminal (M2 obligation):** new
    `FrameScheduler_IsPacingClockDead()` query over the sticky M2 latch;
    session2 polls it each `Session_Update` while Connected/Ready, fires
    `Session2_Terminate(PacingClockDead)` exactly once, then
    `NetMenu::HandleDisconnection` for the UI/wiring teardown.
  - **`Session_GetMsSinceLastInbound`** re-implemented over
    `protocol_silence_ms` (header contract survives; 0xFFFFFFFF
    never-received semantics preserved).
  - **protocol.h:** `Hello` (1) / `HelloAck` (2) deleted outright (enum,
    payloads, names, size pins, barrier_protocol classification);
    `PeerIdentityPayload` added (84 B pin). API_FREEZE §6 updated.
- **Deviations + justification:**
  1. **`PeerIdentity` is not in the plan's §3.2 catalog.** The v2 handshake
     payloads are wire-frozen at M1 (34/62/62/8/8 B pins) and carry only
     the fail-closed identity; without a carrier, deleting Hello/HelloAck
     would break the preserved PeerInfo/HUD-style/round-option contracts
     (G6 survivors: mod_main HUD style sync, menu controller round display,
     replay metadata). Additive reliable packet, sent once, no INV touched.
     M5's match_setup may absorb round/timing into config exchange and slim
     this packet — flagged as an M5 note below.
  2. **`SessionConfig.handshake_timeout_ms` is no longer consulted** for
     the Handshaking state — the plan's fixed 10 s step / 30 s total caps
     replace it (config default was 3 s, far too tight for a 5-step
     exchange over a lossy punched path). Field kept for shape stability.
  3. **DisconnectPayload NOT enriched** to the §3.2
     `{code u8, reason_id u32, human[96]}` shape — the sticky 100 ms-resend
     terminal protocol belongs to the engine terminals (M4+); the existing
     `{reason_code u16, message[64]}` carries the reasoned strings fine for
     the M3 flows. Wire break is legal later (v20 is dev-only until M6).
  4. `Session2TerminalReason` values `ProtocolViolation`/`ConfirmedDesync`
     are declared but have no callers until engine2 (M4) — placed now so
     the funnel enum is complete per §2.4.

### M3-3: packet_router promotion — single dispatch owner, both regimes

- **Files:** `include/net/packet_router.h` (new), `src/net/packet_router.cpp`
  (new), `src/net/gameplay_packet_router.cpp` + `include/net/gameplay_packet_router.h`
  (DELETED), `src/net/pregame_sync.cpp`, `include/net/pregame_sync.h`,
  `src/rollback/online_wiring.cpp`, `include/rollback/online_wiring.h`
  (comment), `CMakeLists.txt`.
- **Done:**
  - `PacketRouter_OnPacket` merges the M0 gameplay router and pregame_sync's
    private callback into one regime-independent table: TransitionBarrier
    first; pregame-machine-owned set (SyncAnnounce/Confirm, CharSelInput,
    CharSelLock, StageSync, Config/Load/Baseline/GameplayStart) ->
    `PregameSync_OnSessionPacket` (the former static `OnPregamePacket`,
    now public — it keeps the pregame lock latches and the cross-phase
    Idle/GameplayHandoff restart routing); frontend lockstep, palette
    50-52, ChurnPause, PauseQuit, diagnostics -> module handlers; engine
    sinks InputStream/GekkoReady -> `OnlineWiring_Handle*` (self-guarding:
    pre-live InputStream drops with logging, startup barrier ignores
    pre-boundary READYs) — the M0 obligation "route the engine sinks
    through the routing table" done. Unknown types: log + count, never
    terminal (§2.3).
  - **Registered once by session2 at `Session_Init`, never handed off.**
    All 5 legacy `Session_SetPacketCallback` handoff sites deleted
    (pregame Begin, pregame cross-phase adopt, online_wiring x3).
    `Session_SetPacketCallback` survives per the header contract: non-null
    overrides the router (test harnesses), null restores it; deferred-flush
    queue kept in session2 with the §2.3 bounds (64->256 packets + 256 KB
    byte cap).
  - Palette packets 50-52 now trivially routed in every regime by
    construction (the F-9 race class is dead).
- **Deviations:**
  1. The deferred queue lives in session2 (the `Session_SetPacketCallback`
     API home, per §2.3's "kept behind the same API"), not in the router
     TU; with the router registered from init the defer window no longer
     occurs in practice.
  2. InputStream/GekkoReady arriving during pregame now reach the
     self-guarding online_wiring sinks instead of pregame's silent ignore
     list — behavioral no-op (drop+log vs silent ignore; the startup
     barrier resend re-asserts READY either way), but worth knowing when
     reading STARTUP logs.

### M3-4: connection_supervisor re-feed + ProgressDeadline

- **Files:** `src/net/connection_supervisor.cpp`,
  `include/net/connection_supervisor.h`.
- **Done:**
  - Verdict input is now protocol-level silence via the re-based
    `Session_GetMsSinceLastInbound` (INV-14); thresholds set to the §2.4
    bands: Healthy <1000, Degraded 1000-5000, **Interrupted 5000** (was
    3000) -20000, Dead >=20000. Heartbeat unchanged (reliable Ping every
    250 ms once silence >=500 ms). STAT `silence_ms` source name
    (`ConnectionSupervisor_GetInboundSilenceMs`) unchanged per the M2 note.
  - Supervisor Dead now routes through
    `Session2_Terminate(SupervisorDead)` before `NetMenu::HandleDisconnection`
    (typed terminal + reasoned Disconnect on the wire, then the existing UI
    funnel).
  - **ProgressDeadline (§2.4, new input):** counts only while a rollback
    session is running AND silence < Interrupted (R-9: the silence ladder
    owns a quiet transport) AND no churn-pause grace/force-hold is active
    (ExternalSuspension). Zero canonical-frame progress
    (`RollbackSession_GetCurrentFrame` frozen) for 8 s -> warn latch + log
    ("opponent's game stopped responding" — HUD readout is
    `ConnectionSupervisor_GetProgressStallMs`/`IsProgressStallWarned`,
    consumer arrives at M6); 20 s ->
    `Session2_Terminate(ProgressDeadline)` + `HandleDisconnection`, fired
    once per session.
- **Deviation:** the 8 s HUD warning is exposed as a query + log line only;
  netplay_hud wiring is scheduled with the M6 HUD update (the plan's HUD
  task list lives there).

### M3-5: kill-path gate allowlist (INV-12)

- **Files:** `tools/check_killpaths.ps1`.
- **Done:** `connection_supervisor.cpp` 1->2 (silence Dead +
  ProgressDeadline); `session2.cpp` = 1 added with justification
  (PacingClockDead — local fail-closed terminal INV-20, routed through
  `Session2_Terminate` first). Legacy heuristic entries retagged to their
  actual cutover milestones (pregame/lifecycle -> M5, input_override -> M4);
  counts unchanged. `session_manager.cpp` had no entry, so its deletion
  needs none removed.

### Build-system summary (M3)

- CMake `NET_HEADERS`: -`gameplay_packet_router.h`, -`network_thread.h`;
  +`packet_router.h`, +`transport2.h`, +`session2.h`
  (`session_manager.h` stays — preserved facade).
- CMake `NET_SOURCES`: -`gameplay_packet_router.cpp`, -`network_thread.cpp`,
  -`session_manager.cpp`; +`packet_router.cpp`, +`transport2.cpp`,
  +`session2.cpp`.
- Deleted files: `src/net/network_thread.cpp`, `include/net/network_thread.h`,
  `src/net/session_manager.cpp`, `src/net/gameplay_packet_router.cpp`,
  `include/net/gameplay_packet_router.h`.
- GekkoNet untouched in lib/ (the `AS2_WITH_GEKKO` gate is an M4 task). No
  test targets link the changed TUs (frontend_sync_tests stubs
  `Session_*`/`PregameSync_GetPhase` itself).

### Obligations for next milestones

- **M4 (engine2):** `Session2_Terminate` reasons `ProtocolViolation` and
  `ConfirmedDesync` are reserved for the ingest/verify terminals; feed them
  from engine2's typed results. `RollbackSession_IsSessionRunning` is what
  arms the ProgressDeadline — keep that meaning ("gameplay timeline
  advancing is expected") in the engine2 adapter.
- **M5 (match_setup + stream cutover):** enforce `session_id`
  (`Session2_GetSessionId()`) on gameplay-phase packets at the router/ingest
  (§3.1 — drop before any state mutation); retire `DelayChangeReq/Ack`,
  `GekkoReady`, the SyncAnnounce/SyncConfirm delay fields, and the
  `phase_serial` acceptance keys per the M1 notes; consider folding
  `PeerIdentity`'s round/timing fields into the config exchange and
  slimming the packet to nickname+HUD style.
- **M6 (HUD):** consume `ConnectionSupervisor_GetProgressStallMs`/
  `IsProgressStallWarned` for the 8 s "opponent's game stopped responding"
  banner; surface `Session2TerminalReasonName` in the disconnect UI.

### Compile risks to check first (M3 build session)

1. `session2.cpp` is a ~2.5k-line new TU aggregating winsock2/windows,
   `<random>`, and the netplay stack — first place an include-order or
   namespace slip surfaces. `FrameScheduler_IsPacingClockDead` is a
   global-namespace function called from `namespace Net` (ordinary lookup —
   same pattern as netplay_pacing's scheduler calls).
2. `SessionOffer/Ack` echo checks memcmp whole packed structs — if MSVC
   padded them the M1 static_asserts (34/62/62 B) fire first; trust the
   asserts.
3. `PeerIdentityPayload == 84` static_assert is hand-computed
   (64+2+1+1+1+12+3 under pack(1)); if it fires, the compiler's number is
   the truth — fix the constant.
4. `connection_supervisor.cpp` now includes `rollback/rollback_session.h` +
   `net/churn_pause.h` + `net/session2.h` — check for macro collisions with
   `ui/log_window.h` (LOG_* macros) in that TU.
5. transport2.cpp compares tick domains
   (`(int32_t)(punchInbound - s_lastProtocolInboundTickMs) > 0`) — both are
   GetTickCount-domain; ENet `serviceTime` is only ever used as a duration
   difference, never mixed absolutely.
6. Grep-verified zero remaining references: `NetworkThread_*`,
   `GameplayPacketRouter_OnPacket`, `PacketType::Hello/HelloAck`,
   `HelloPayload/HelloAckPayload`, `enet_silence_ms` (spectator's own
   `Spectator::PacketType::Hello` namespace is unrelated and untouched).

---

## 2026-08-17 — M4 (engine2 offline bring-up; Gekko still ships)

### M4-1: Resurrect + finish `input_timeline` + `prediction` (§2.7.3)

- **Files:** `include/rollback/input_timeline.h` (rewritten),
  `include/rollback/prediction.h` (rewritten),
  `src/rollback/input_timeline.cpp` / `src/rollback/prediction.cpp` (reduced
  to include-hygiene stub TUs).
- **Done:** both modules resurrected as PURE, instantiable, header-only
  classes (inventory confirmed both orphaned — zero includers — so the old
  global C API had no consumer to preserve):
  - `InputRing` — direct-mapped canonical u32 input ring (wrap-safe via
    frame_arithmetic, INV-15), typed `Set` results
    (`Stored/Occupied/Conflict/StaleSlot`) so the engine maps occupied-slot
    writes to adopt / DuplicateIdentical / Conflict without the ring
    guessing intent; slot immutability is the INV-18 substrate.
  - `HoldLastPredictor` — hold-last-actual with newest-frame-wins updates;
    earliest-mismatch tracking deliberately lives in the engine.
- **Deviation:** header-only instead of .cpp-backed (the pure engine core
  and the socket-free test target instantiate them without linking mod TUs);
  the stub .cpp files stay listed in CMake so the layout is unchanged.

### M4-2: `rollback/engine2` — the socket-free RollbackEngine core (§2.7)

- **Files:** `include/rollback/engine2.h` (new), `src/rollback/engine2.cpp`
  (new), `include/rollback/block_digest.h` (new — Block64 primitive, §2.7.7
  hash mandate, shared by core/adapter/bench).
- **Done:** full §2.7 spec as an instantiable `RollbackEngine` (zero
  Win32/game/socket/clock/log includes; protocol.h structs only):
  - §2.7.1 `EngineConfig` (validated at Arm; neutral 0xFFFF/out-of-mask
    rejected); §2.7.2 canonical u32 counter + epoch mapping
    (`RotateEpoch` strictly-increasing, origin remap, counter NEVER resets).
  - §2.7.3 C/D/X/P/R/F: capture-once with adopt-on-repeat (INV-18); delay =
    pure relabel (raise fills neutral future immediately; lower drains at a
    fully-confirmed boundary, frame-counted 300-opportunity timeout reverts
    — INV-16, no wall clock); typed ingest
    Applied/DuplicateIdentical/Conflict/InvalidValue(0xFFFF or ~0x3FFF)/
    TooFarFuture(+120)/Stale, with Conflict/Invalid as sticky fail-closed
    terminals (INV-19); stalled producer bounded
    `min(peerR+peerD+2, 30)` ahead of peer ack with the ≤32-suffix
    structural assert (INV-24), fenceable; hold-last predict + earliest
    mismatch (out-of-order actuals take the min); confirm = actual prefix +
    committed replay, single immutable pop seam (predicted values can never
    pop).
  - §2.7.4 `NextAction()` in exactly the plan's order; the PredictionLimit
    comparison is the ONLY reader of R_local in the engine (INV-1/INV-4,
    marked in-source).
  - §2.7.5 explicit transaction: `BeginRollback / NextReplayInputs /
    CommitReplayFrame / FinishRollback / FinishRollbackAtBoundary /
    FinishRollbackBeforeBoundary` (both truncation variants collapse the
    speculative suffix to the replay cursor and invalidate its exec
    records); capture inside the transaction = typed InternalInvariant
    terminal.
  - §2.7.7 SyncHash: cadence exactly every 30 confirmed frames, 128-record
    local cadence ring, bounded (128) peer queue with
    stale-ignore/future-queue/inconsistent-fatal epoch relations and
    exact-across-all-fields compare → `ConfirmedDesync` terminal.
  - §3.2 wire building: `BuildInputStream` (window anchored at
    peer_ack_through+1, clamped to 32/first-frame, ack + PressureReport on
    every packet), `IngestInputStream` (idempotent merge, monotonic ack
    re-anchor, advisory capture), `FillPressureReport`.
  - run_state.h classification per pass; per-engine `Stats`;
    `FramesAheadSigned` telemetry (explicitly no decision consumer).
- **Deviation:** internal invariant violations set a typed terminal instead
  of asserting (a release game process must fail closed, not crash); the
  unit tests pin the terminal. `HashVerify::InconsistentEpoch` doubles as
  the queue-overflow return (both are the ProtocolViolation class; the
  terminal code distinguishes them).

### M4-3: Facade adapter behind unchanged `rollback_session.h`

- **Files:** `src/rollback/rollback_session_engine2.cpp` (new),
  `CMakeLists.txt` (`AS2_WITH_GEKKO` option).
- **Done:** every symbol of the preserved facade implemented over the
  engine2 core (§2.7.8 mapping table honored): two-phase dispatcher contract
  (BeginFrame captures at the sim frontier + plans; ProcessNextEvent drains
  — corrections first via StateHistory tagged restore + replay-Advance
  events, then at most the visible advance), save→simulate→commit shape
  (pre-tick `StateHistory_CaptureFrameHashed` supplies the confirm
  pre-hash), InputStream egress (send-per-seal + 50 ms idle resend while
  un-acked, session_id stamped, StressHooks egress drop query preserved),
  `BufferGekkoPacket` reinterpreted as the v2 InputStreamPayload ingest
  (wrong session_id inert per §3.1), SyncHash send/receive, and:
  - **M2 obligations closed:** typed stalls call
    `FrameScheduler_NotifyHold(cause, create_debt = cause==PredictionLimit)`
    — engine2 is the first (and only) `LocalInputMissing` producer;
    PressureReport ingest feeds `FrameScheduler_SubmitPeerDepthSample`.
  - **M3 obligations closed:** engine terminals route through
    `Session2_Terminate(ProtocolViolation | ConfirmedDesync)` (desync path
    dumps evidence via `DesyncDump_TryDump` first — D-1); the UI abort stays
    with input_override's existing single kill-path site (reads
    `GetErrorReason`), so the kill-path allowlist is UNCHANGED.
  - `IsSessionRunning` keeps the ProgressDeadline meaning;
    `IsPeerInterrupted` is the supervisor passthrough;
    `FramesAhead` = signed produced-frontier lead (telemetry only).
- **Deviations:**
  1. Adapter is a separate TU selected by CMake
     (`AS2_WITH_GEKKO=ON` → rollback_session.cpp,
     `OFF` → rollback_session_engine2.cpp) instead of `#if` inside the live
     66 KB Gekko file — same flag semantics, zero risk to the shipping
     path. GekkoNet subdirectory/link/include/`GEKKONET_STATIC` and the
     `gekko_input_tests` target are all gated on the option; **default ON**
     (Gekko is still the live gameplay path per the M4 exit gate).
  2. Epoch authority is a local stub (epoch 1, no rotation caller) until
     M5 match_setup; `SetProducerFenced` and the delay hotkeys have no
     caller until M5/M6.
  3. `RefreshLifecycleWindow` uses the conservative stand-in
     (`match_exit_pending=false`, all non-(8,3) ticks exact) — the audited
     minimal window needs the M6 director signal (see M4_AUDITS.md).
  4. Stress ingest/prediction hook points (delivery delay, forced
     mismatch) are not yet wired (egress drop is); rewire with the M6
     dispatcher pass.
  5. SyncHash rng/hp diagnostics are 0 (gameplay_hash authoritative);
     side-channel lands with the M6 HUD/dump pass.

### M4-4: StateHistory 64-slot tagging + FPU + savestate guard adapter

- **Files:** `include/rollback/resimulation.h`,
  `src/rollback/resimulation.cpp`, `include/rollback/game_snapshot.h`,
  `src/rollback/game_snapshot.cpp`, `src/rollback/savestate.cpp`,
  `src/net/netplay_palette_runtime.cpp`.
- **Done:**
  - `STATE_HISTORY_CAPACITY` 32 → 64; storage is direct-mapped
    (`slot = frame % 64`) per §2.7.7; every slot tagged `{epoch, frame,
    phase}` via `StateHistory_SetTagContext`; new
    `StateHistory_LoadFrameTagged` refuses tag mismatches fail-closed
    (stale-epoch slots can never be restored); new
    `StateHistory_CaptureFrameHashed` returns the Block64 gameplay digest.
    Legacy `CaptureFrame/LoadFrame/HasFrame/DiscardFramesAfter` keep their
    signatures (replay reverse-stepping + training verified compatible:
    HasFrame-before-Load patterns, dense frame sequences).
  - `GameSnapshot` gains per-slot x87 CW + MXCSR capture/restore (same
    fnstcw/fldcw/_mm_getcsr mechanism the Gekko CaptureState already used —
    TrialNetplay: capture-and-restore, never assume 0x027F) and
    `GameSnapshot_HashGameplay` (sim-affecting members only, INV-22).
  - Guard adapter (inventory 3a): the redundant
    `GameplayBridge_IsSessionActive` check removed from savestate.cpp (the
    facade check was already present) and replaced with
    `Rollback::RollbackSession_IsActive()` at the three
    netplay_palette_runtime sites; both TUs no longer include
    gameplay_bridge.h. (mod_main/online_wiring bridge uses are the doomed
    set itself — untouched until the M6 deletion.)

### M4-6 / M4-7: audits

- **Files:** `docs/re0.7/M4_AUDITS.md` (new),
  `include/rollback/lifecycle_window.h` (new), region-role comments in
  `game_snapshot.h`.
- **Done:** M4-6 per-region role table (SIM vs RENDER/TIMING/CONTROL) with
  rationale; digest membership = main_state + rng/sim counters +
  mode/substate/timers + input buffers + effect cursor; EXCLUDED:
  display_frame (timing), pre_match_gap (render), FPU control words
  (machine config), palettes (outside all regions by pipeline design).
  M4-7 decomp audit with line citations: `Game_ChangeMode` (0x5D2EB0,
  L266664-92) calls `Handle_ReleaseAll` — the only release path reachable
  from gameplay, via the mode-8 exit router `sub_4CA210` (L118402+, mode 9
  at L118430); zero release sites inside the match handler; round state is
  pure match-region counters (`Match_UpdateRoundState` L107972+). Verdict:
  round boundaries predict normally; the match-end handoff is the single
  in-gameplay exact window. Pure predicate
  `LifecycleWindow_IsExactInputNext` is the §2.7.6 engine predicate
  (unit-pinned).

### M4-8: tests + soak + microbench

- **Files:** `tests/engine2_tests.cpp` (new), `CMakeLists.txt` (new target
  `engine2_tests` + `add_test`).
- **Done:** T-ENG-1..12 all implemented against the pure core (capture-once
  adopt/immutability; delay raise/lower-drain/timeout-revert; hold-last +
  earliest mismatch under out-of-order actuals + retained speculation;
  transaction begin/commit/finish + boundary truncation + in-transaction
  capture fail-closed; confirm ordering with pre-hash carry; full ingest
  taxonomy; window math incl. 30-cap producer suffix, single-packet hole
  refill, ack re-anchor, fully-acked degenerate window; producer bound +
  fencing; INV-4 pin "exactly R speculative frames, never R+1, released by
  one actual"; full-pipeline u32 wrap run at first_frame=0xFFFFFF00; epoch
  rotation + stale/future/inconsistent hash relations + origin remap;
  SyncHash 30-frame cadence + exact-all-fields compare + 128 queue bound)
  plus the lifecycle-window predicate pins, the §7.2-model soak (two full
  peers over a seeded lossy/jittery/reordering frame-clock link: 100k
  frames, 3% loss, 1+[0..2]-frame jitter ≈ 40±15 ms RTT; asserts zero
  terminals, hash chain clean, per-frame confirmed streams identical, both
  confirm frontiers >90% of the run), and the §7.5#4 microbench (real
  `sizeof(GameSnapshot)` volume: save/restore memcpy + Block64 p50/p99
  printout; hard-fails only at 10x budget so CI cannot flake — the 1.5 ms
  acceptance is judged on min-spec at M8).
- **Deviations:** T-ENG-9's "no other code path reads R" link-time/grep half
  is a review-gate item (single reader marked INV-4 in engine2.cpp), not an
  executable test. The microbench measures byte-volume cost offline, not
  in-game capture (game memory unavailable in a unit target); in-game
  numbers come from the M8 STAT instrument.

### Ride-along: DisconnectPayload §3.2 enrichment (M3 deviation closed)

- **Files:** `include/net/protocol.h`, `src/net/session2.cpp`,
  `docs/re0.7/API_FREEZE.md`.
- **Done:** `DisconnectPayload` → `{code u8, reason_id u32, human[96]}`
  (102 B pin); `Session2_Terminate` fills code from DisconnectReason,
  reason_id = fnv1a32 of the typed `Session2TerminalReason` name, and for
  fault terminals resends the packet every 100 ms across a bounded 400 ms
  goodbye window before transport destroy; receiver logs/surfaces
  code+reason_id+human. API_FREEZE §6 updated (wire break legal — v20 is
  dev-only until M6).
- **Deviation / M5-M6 obligation:** full sticky resend-UNTIL-ACKED needs a
  session object that outlives the terminal (today teardown is synchronous);
  the bounded resend window + ENet reliable-channel retransmission is the
  M4 approximation. Revisit when session2 gains a terminal-lingering state
  at the M5/M6 cutover.

### Build-system summary (M4)

- New CMake option `AS2_WITH_GEKKO` (default ON) selects
  `rollback_session.cpp` + GekkoNet lib/link/include/`GEKKONET_STATIC` +
  `gekko_input_tests` (ON) vs `rollback_session_engine2.cpp` with no
  GekkoNet anywhere (OFF). GekkoNet vendored lib itself untouched.
- `ROLLBACK_HEADERS` += `block_digest.h`, `engine2.h`,
  `lifecycle_window.h`; `ROLLBACK_SOURCES` += `engine2.cpp`
  (rollback_session*.cpp now appended conditionally).
- New test target `engine2_tests` (tests/engine2_tests.cpp +
  src/rollback/engine2.cpp; include dirs `include` + `include/core` for the
  as2_constants chain under game_snapshot.h).
- No kill-path allowlist changes (the engine2 adapter funnels through
  `Session2_Terminate` + the existing input_override abort site only).

### Obligations for next milestones

- **M5 (match_setup):** take epoch authority into the adapter
  (`RotateEpoch` + `StateHistory_SetTagContext` at GameplayStart commit;
  the M4 stub pins epoch 1); retire the `BufferGekkoPacket` facade name
  with the router-table cutover; fence the producer during frontend phases
  (`SetProducerFenced` — currently caller-less); enforce session_id at the
  router (adapter already drops mismatches at ingest).
- **M6 (director/cutover):** wire `match_exit_pending` from
  MatchLifecycle/exit-route state into `RefreshLifecycleWindow` (replace
  the conservative stand-in); adapter-side mode-change bail during replay
  (§2.8.6-d) once the dispatcher rewiring lands; delay hotkeys `-`/`=`;
  SyncHash rng/hp diagnostics; stress delivery-delay/forced-mismatch
  hooks; flip default `AS2_WITH_GEKKO=OFF` at the cutover gate and delete
  the Gekko branch per plan M6; session2 terminal-lingering state for true
  sticky Disconnect resend; input_override abort site burns down to the
  typed-terminal flow.
- **M8:** run the microbench equivalent in-game on min-spec (STAT
  instrument) against the 1.5 ms p99 acceptance.

### Compile risks to check first (M4 build session)

1. `engine2.h` is included by two new TUs (`engine2.cpp`,
   `engine2_tests.cpp`) and pulls `net/protocol.h` — first place a
   namespace/packing slip surfaces. `InputStreamPayload`/`SyncHashPayload`
   field names must match the M1 header exactly.
2. `static_assert(sizeof(DisconnectPayload) == 102)` — hand-computed under
   pack(1); if it fires, the compiler's number is the truth.
3. `GameSnapshot` grew (fpu_cw/_pad_fpu/mxcsr) — savestate/replay slots
   grow with it (heap-allocated everywhere; no wire/file format carries the
   struct; replay coarse checkpoints are process-local).
4. `__asm fnstcw/fldcw` in game_snapshot.cpp — x86-only MSVC inline asm,
   same mechanism already living in rollback_session.cpp; needs
   `<xmmintrin.h>` for `_mm_getcsr/_mm_setcsr` (added).
5. StateHistory internals rewrote the slot layout (`StateSlot{snap, epoch,
   phase, gameplay_hash}`) — grep-verified no external accessor of the old
   fields; `memset` over the slot array is legal (all-POD struct).
6. The `AS2_WITH_GEKKO=OFF` configuration compiles
   `rollback_session_engine2.cpp` — a new ~700-line TU aggregating
   session2/frame_scheduler/supervisor/delay_policy headers; build BOTH
   configurations once (`-DAS2_WITH_GEKKO=OFF` is the engine2 CI config;
   default ON is the shipping config).
7. `engine2_tests` runs a 100k-frame soak + a memcpy-heavy microbench —
   expect a few seconds of runtime; if CTest timeouts are tight, the soak
   constant is `kFrames` in `SoakRun`.
