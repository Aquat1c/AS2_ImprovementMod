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

---

## 2026-08-17 — M5 (match_setup + frontend phase-identity cutover; ships with M6)

### M5-1: `net/match_setup` — PregameSync_* facade re-implementation; pregame_sync/match_bootstrap internals deleted

- **Files:** `src/net/match_setup.cpp` (new, ~1660 lines),
  `include/net/pregame_sync.h` (facade preserved; additive
  `PregameSync_GetCurrentEpoch()` + `PregameBootstrapInfo`/
  `PregameSync_GetBootstrapInfo()`), `src/net/pregame_sync.cpp` (DELETED),
  `src/net/match_bootstrap.cpp` (DELETED), `include/net/match_bootstrap.h`
  (DELETED), `CMakeLists.txt`.
- **Done:**
  - One phase machine behind the verbatim 12-function `PregameSync_*` facade
    (`PregamePhase` kept as the reporting vocabulary). The former
    match_bootstrap internals (config exchange, load barrier, baseline
    rendezvous, GameplayStart GO) are absorbed as statics in the same TU;
    `MatchBootstrap_*` is retired tree-wide (online_wiring's two snapshot
    reads now use the additive `PregameSync_GetBootstrapInfo`).
  - **Epoch authority (§2.5):** host-minted u32 generations, strictly
    increasing, session-scoped (counter resets when `Session2_GetSessionId()`
    changes; `NextEpoch()` also absorbs the deterministic charsel-cancel
    frontend rebinds so mints can never collide). Join adopts via EpochAlign.
  - **EpochAlign barrier (INV-8/10):** rides the SyncConfirmed reporting
    phase. Host mints + proposes `{epoch, first_phase(CharSel|None),
    native_mode}`; join waits, adopts the host payload verbatim, echoes its
    proposal; commit requires identical `{epoch, first_phase}` (enforced in
    transition_barrier). Commit consumption is gated on completing any native
    mode-9 exit (§4.5 step 2); the commit cancels every prior frontend
    machine (`FrontendInputSync_AbortEpoch` + fresh `BeginEpoch(epoch)`)
    and routes to CharSel or (fast path) straight to ConfigExchange.
  - **Rematch fast path preserved:** `BeginRematch` contract unchanged;
    implemented as rotate → EpochAlign(first_phase=None) → ConfigExchange →
    Load → Baseline → GO; freeze-coverage extension carried over.
  - **Recovery ladder (§4.6, INV-11/12):** ALL recoverable failures now route
    through `RestartPregame(reason)` — abort machines, fresh epoch, live
    connection, both roles (a mid-run foreign SyncAnnounce is recognized as
    the peer's restart signal and converges both sides). This covers: sync/
    align/config timeouts (with the M1 45 s liveness-cap semantics kept),
    load timeout (F-11), baseline timeout, baseline capture failure, and all
    frontend recovery requests (which previously went `Error` →
    `HandleDisconnection`). `SetPhase(Error)` is now reserved for genuine
    fail-closed terminals: config validation/hash rejection (C-3 class) and
    the second baseline mismatch, which fires
    `Session2_Terminate(ConfirmedDesync)` first (`BaselineMismatch`, F-12).
  - **Interrogation escalations consumed here:** `FrontendInputSync_
    ConsumeResyncEscalation()` polled every FrameUpdate (including
    Idle/GameplayHandoff so the winscreen stream is covered): `Realign` in a
    charsel-family phase → EpochAlign re-run under the SAME epoch (barrier
    slot cleared, re-proposed, frontend phase restarted at re-commit);
    anything else → pregame restart under a fresh epoch. Never a teardown.
  - **INV-11 transport gate injected:** `FrontendInputSync_
    SetTransportHealthyGate` registered with supervisor
    `!Interrupted && !Dead` (interrogate only on a live link; the silence
    ladder owns the rest — INV-14).
  - Baseline mismatch retry (§2.5 "retry load once"): first mismatch dumps
    both breakdowns and re-runs the Baseline phase (recapture + re-exchange)
    once under the same epoch; second mismatch is the terminal.
  - Callback rewiring per inventory §4/§6 all preserved at their new homes:
    `NetplayPaletteRuntime_OnLockedMatchConfig` at config agreement,
    `SpectatorRuntime_OnSelectionCommitted` at ConfigAgreed,
    `MatchLifecycle_OnMatchEnter` + `OnlineWiring_OnGameplayStart` at
    handoff, palette `OnDisconnect` at begin/abort/restart resets.
- **Deviations (logged per the plan rule):**
  1. **Config/load/baseline stay on their shipping packet flows** (14-19/25)
     rather than TransitionBarrier kinds. The §2.5 wording makes load/GO
     "TransitionBarrier commits"; the shipping LoadBarrier/BaselineReady/
     GameplayStart packets already implement the same reliable both-or-neither
     rendezvous AND carry payloads (mode/substate/sim-frame diagnostics,
     bootstrap frame facts) a bare intent barrier cannot. EpochAlign and the
     startup gameplay-entry release (M5-4) are TransitionBarrier kinds as
     specified. M6 may consolidate if the director wants one primitive.
  2. **Baseline "retry load once" is a recapture-and-re-exchange**, not a
     full asset reload (the game is frozen at the sub-3 boundary; a real
     reload needs mode rewinding the director doesn't own until M6). A
     deterministic content divergence therefore terminates one retry later,
     as specified; a transient capture divergence is genuinely healed.
  3. **Winscreen-context Realign escalates to Restart** (fresh epoch,
     charsel) instead of re-running EpochAlign for the winscreen epoch — the
     native mode-9 exit routing needed for a same-epoch winscreen re-align
     belongs to the M6 director. Ladder level 3 instead of level 2; still
     never a teardown.
  4. `PregameSnapshot.session_id` keeps reporting the 32-bit pregame run id
     (seed scope), not the epoch — UI meaning unchanged; the epoch has its
     own query.

### M5-2: Frontend phase-identity migration (§3.4) — serial allocator deleted; delay negotiation retired (INV-23)

- **Files:** `include/net/frontend_input_sync.h` (rewritten),
  `src/net/frontend_input_sync.cpp` (rewritten), `include/net/protocol.h`,
  `src/net/charsel_sync.cpp`, `src/net/winscreen_sync.cpp`,
  `src/patches/input_override.cpp` (1 rename),
  `include/net/barrier_protocol.h`.
- **Done:**
  - **Acceptance rule cutover:** frame-input packets accept iff
    `(epoch, phase_id)` matches (§3.4); `phase_id` is filled from the fixed
    `FrontendPhaseId` mapping (`FrontendSyncPhaseToPhaseId`, new inline in
    the header). A coherent-but-unexpected `phase_id` increments a counter;
    at 60 packets it triggers the INV-11 interrogation instead of silent
    dropping. Control-plane packets (CharSelLock/StageSync/phase barrier/
    boundary digest) accept on `(epoch, phase)` — both halves fixed enums,
    nothing allocated at runtime (INV-7).
  - **Serial allocator deleted:** `AllocatePhaseSerial`, `s_phaseSerial`,
    `GetPhaseSerial`, `IsCurrentEpochPhaseSerial` gone; every wire
    `phase_serial` field renamed `_retired_serial` (sent 0, never read;
    wire sizes/pins unchanged: 56/56/20/36/16/24). winscreen_sync's
    early-frame buffer drains on `(epoch, phase_id)`.
  - **Delay negotiation retired (INV-23, INV-13):** the frontend delay is a
    locally derived per-epoch value (`GetFrontendDelay`, renamed from
    GetSharedDelay). Deleted: `OnRemoteSyncAnnounce/OnRemoteSyncConfirm`
    (delay carriers), `FinalizeDelayNegotiation`, `IsDelayNegotiated`,
    `GetLocal/RemoteDelayProposal`, the whole DelayChangeReq/Ack wire flow
    (packets 21/22 + payloads + routes + classification). Live bumps are
    increase-only LOCAL scheduled applies at `consume+delay+2` (the pressure
    sampling machinery is kept as the trigger); the `SyncAnnounce`/
    `SyncConfirm` delay fields (incl. the `shared=` field that shipped
    wrong) became `_retired*` pads, sizes 8/12 stable.
  - `WinScreenSync_Begin`'s defer gate is now "no active epoch"
    (`FrontendInputSync_IsEpochActive`, new) instead of "delay not
    negotiated"; INV-8 fail-log added to `BeginInputPhase` (a second Begin
    while a machine is active replaces it loudly).
  - Snapshot struct reshaped (only consumer was the test suite):
    `frontend_delay`, `phase_id`, `starved_frames`, `resync_cycles` added;
    serial/negotiation fields gone.
- **Deviation:** §3.2 says the serial field is "removed"; the BYTES stay as
  `_retired_serial` padding because the M1 wire-size pins
  (`static_assert` 56 B etc.) are load-bearing across the tree. Semantics
  (allocator, acceptance key, all reads) are fully removed.

### M5-3: ResyncRequest/Reply interrogation (INV-11) + session_id gate

- **Files:** `src/net/frontend_input_sync.cpp` (+header),
  `src/net/packet_router.cpp`, `include/net/packet_router.h`.
- **Done:**
  - Starvation clock counted in lockstep ticks (FrameUpdate cadence, never
    wall time): 120 ticks with zero accepted remote frames while the
    injected transport gate reports Healthy/Degraded → send
    `ResyncRequest{epoch, phase_id, native_mode, local_frame}`; every
    request is a cycle; accepted remote traffic resets counter+cycles.
    Requests are ALWAYS answered with the responder's identity tuple.
  - Reply identity mismatch → `FrontendResyncEscalation::Realign`; three
    cycles (~6 s) → `Restart`. The latch is consumed by match_setup (M5-1)
    — this module never tears anything down (INV-12).
  - Router routes `ResyncRequest`/`ResyncReply` (reliable ch0 via the
    barrier classification) and gates the v2-only `SyncHash` on
    `Session2_GetSessionId()` (§3.1, drop-before-mutation). InputStream
    CANNOT be gated at the router while AS2_WITH_GEKKO=ON ships raw Gekko
    bytes under id 23 — the gate lives at the engine2 adapter's typed
    ingest (documented in the router).
  - `GetGameMode()` reads are fenced behind `AS2_FRONTEND_SYNC_TESTING`
    (the standalone test binary has no game memory).

### M5-4: GekkoReady retired — startup release rides TransitionBarrier GameplayStart

- **Files:** `src/rollback/online_wiring.cpp`,
  `include/rollback/online_wiring.h`, `include/net/protocol.h`,
  `include/net/barrier_protocol.h`, `src/net/packet_router.cpp`,
  `include/net/transition_barrier.h`, `src/net/transition_barrier.cpp`.
- **Done:**
  - The READY/ACK flag exchange (~180 lines: SendGekkoReadyPacket,
    OnlineWiring_HandleStartupBarrierPacket, 4 latches + frames) is replaced
    by one `TransitionBarrier_Propose(GameplayStart)` at the interactive
    boundary; commit = mutual release (the barrier owns 250 ms resends and
    idempotent re-acks — it was modeled on this exact exchange).
    `PacketType::GekkoReady` (24), its payload and flags are deleted from
    the wire vocabulary.
  - New `TransitionBarrier_Clear(kind)` clears one slot;
    `ResetStartupBarrierState` clears the GameplayStart slot so a stale
    proposal from a previous match can never satisfy the next match's
    release (the structural replacement for the old "ignore remote READY
    before interactive boundary" guard). Ordering closes the race: the
    arm-time clear happens at the pregame handoff, seconds before either
    peer's post-intro proposal can exist.
  - transition_barrier gains the EpochAlign payload plumbing (M5-1):
    per-slot `{epoch, first_phase, native_mode}` for both sides, echoed
    verbatim in acks (INV-13 discipline), commit refused on
    `{epoch, first_phase}` mismatch with a rate-limited surface log
    (INV-6/INV-10), `ProposeEpochAlign`/`GetRemoteEpochAlign` API.
- **Deviation:** the pregame `GameplayStart` packet (19) is NOT folded into
  the barrier (see M5-1 deviation 1) — the plan's single-name "GameplayStart
  barrier" is realized as: pregame GO = packet 19 (host-authoritative,
  carries frame facts), startup release = TransitionBarrier kind 4.

### M5-5: Engine adapter obligations closed (epoch authority, producer fence, facade renames, SyncHash ingest, delay hotkeys)

- **Files:** `src/rollback/rollback_session_engine2.cpp`,
  `src/rollback/rollback_session.cpp`, `include/rollback/rollback_session.h`,
  `src/rollback/online_wiring.cpp`, `src/net/gameplay_bridge.cpp`,
  `include/net/gameplay_bridge.h`, `src/rollback/rollback_debug.cpp`.
- **Done:**
  - **Epoch authority into the adapter (M4 obligation):** engine2 Begin arms
    with `PregameSync_GetCurrentEpoch()` (falls back to 1 only when no
    pregame ran — harness use); StateHistory tag context follows. The M4
    "pins epoch 1" stub is gone.
  - **Producer fencing (M4 obligation):** `SetProducerFenced(frontend input
    phase active || pregame active)` refreshed every `PollSession` — the
    stalled-producer never feeds the gameplay stream while a frontend
    lockstep phase owns the exchange (§2.7.3-P). The continue prompt is
    covered by construction (mode 9 rides the winscreen input phase).
  - **Facade renames (inventory §11):** `RollbackSession_BufferGekkoPacket`
    → `RollbackSession_OnInputStreamPacket` (both adapters + the
    online_wiring caller); new `RollbackSession_OnSyncHashPacket` (engine2:
    session-gated `ReceiveSyncHash` + terminal pump; Gekko: documented
    no-op); `RollbackSessionSnapshot.gekko_avg_ping/gekko_jitter` →
    `link_avg_ping/link_jitter` (fill sites in both adapters; readers in
    online_wiring STATS, rollback_debug ImGui, gameplay_bridge — whose own
    doomed snapshot fields were renamed too).
  - **Delay hotkeys `-`/`=` (plan §9 Q3 SHIP):** edge-detected
    `VK_OEM_MINUS/PLUS` in `OnlineWiring_FrameUpdate` while gameplay is
    active → `RollbackSession_SetLocalDelay(active±1)` +
    `DelayPolicy_OnRollbackApplied` — peer-local, zero wire traffic (B-7);
    works on both adapters (`SetLocalDelay` exists on the Gekko path too).
- **Deviations/notes:** hotkeys use `GetAsyncKeyState` (global, not
  focus-gated) — matches the mod's existing hotkey idiom; refine with the M6
  HUD pass if focus-gating is wanted. Cross-match `RotateEpoch` (engine
  surviving the match end) remains the M6 director's job — at M5 the engine
  still re-arms per match, now under the true epoch.

### M5-6: Tests + gates + docs

- **Files:** `tests/frontend_sync_tests.cpp`, `tools/check_killpaths.ps1`,
  `docs/re0.7/API_FREEZE.md`, `CMakeLists.txt`.
- **Done:**
  - frontend_sync_tests reworked to the M5 API: local-delay derivation test
    (replaces shared-negotiation), local scheduled-bump tests (replace the
    DelayChangeReq wire assertions), `(epoch, phase_id)` acceptance test
    (stale epoch + foreign phase_id, replaces the phase-serial test), and a
    new interrogation-ladder test (120-tick starvation → ResyncRequest with
    the right identity tuple; mismatch reply → Realign, consumed-once; 3
    cycles → Restart; requests always answered). Frame builders fill
    phase_id; snapshot field renames followed.
  - Kill-path gate: `pregame_sync.cpp` entry replaced by
    `match_setup.cpp = 2` (terminal Error funnel + session-lost guard) with
    the INV-12 justification; match_lifecycle/input_override entries
    retagged M6. Counts otherwise unchanged.
  - API_FREEZE §4/§6 updated: facade renames done; M5 wire deletions
    (DelayChangeReq/Ack, GekkoReady, announce/confirm delay fields),
    §3.4 cutover state, live ResyncRequest/Reply + EpochAlign payloads +
    SyncHash-with-gate documented.
- **Deviation:** the M5 exit gate's "EpochAlign" unit coverage is not in
  frontend_sync_tests — transition_barrier isn't linked there (the suite
  stubs `TransitionBarrier_Propose`), and linking it would collide with the
  stubs. The commit rule is exercised by the in-game frontend-only soak
  (the gate's second half); a dedicated barrier unit target is an M6
  obligation.

### Build-system summary (M5)

- CMake `NET_SOURCES`: -`pregame_sync.cpp`, -`match_bootstrap.cpp`;
  +`match_setup.cpp`. `NET_HEADERS`: -`match_bootstrap.h`
  (`pregame_sync.h` stays — preserved facade).
- Deleted files: `src/net/pregame_sync.cpp`, `src/net/match_bootstrap.cpp`,
  `include/net/match_bootstrap.h`.
- Both `AS2_WITH_GEKKO` configurations chase the same edits: the Gekko
  adapter implements the renamed ingest + no-op SyncHash entry; GekkoNet
  lib/ untouched; no test-target source lists changed.

### Obligations for next milestones

- **M6 (director/cutover):** cross-match `RotateEpoch` +
  `StateHistory_SetTagContext` at the rematch GameplayStart commit (engine
  stays armed across matches — the M5 adapter still re-arms per match);
  strict INV-9 match-end ladder gating (WinScreenExit → PostMatchDecision →
  EpochAlign held-and-re-acked in order) — at M5 the three barriers exist
  and fire in order by construction but no director refuses out-of-order
  commits; winscreen-epoch same-epoch Realign (M5 escalates to Restart);
  consider folding PeerIdentity round/timing into config exchange (M3 note,
  still open); time_probe + frontend-delay latch (§2.9.3) replaces the
  ms-RTT `ComputeDelayProposal` source; frontend-phase STAT hold producer
  (M2 note, still open); consolidate load/GO onto TransitionBarrier if the
  director wants one primitive; delete `DelayPolicy_GetStallThreshold` with
  the input_override rewrite; dedicated transition_barrier unit target
  (EpochAlign commit rule, Clear semantics).
- **M6 kill-path burn-down:** match_lifecycle session-lost guard +
  input_override abort site; match_setup's Error funnel shrinks further
  once engine terminals own BaselineMismatch surfacing.
- **M8:** LE-1/§7.3 cells exercise the interrogation ladder under loss
  (ResyncRequest must NOT fire on plain packet loss — the 120-tick window
  plus redundant-window refill makes that structurally unlikely; verify).

### Compile risks to check first (M5 build session)

1. `match_setup.cpp` is a new ~1660-line TU aggregating the pregame +
   bootstrap include sets plus session2/supervisor/transition_barrier —
   first place an include-order or namespace slip surfaces. It defines
   `PregameSync_*` in `namespace Net` with statics in an anonymous
   namespace; `PregameSync_Begin` is called from inside the anonymous
   namespace (`RestartPregame`) via the header declaration.
2. `frontend_input_sync.cpp` was rewritten wholesale — the
   frontend_sync_tests target compiles it standalone under
   `AS2_FRONTEND_SYNC_TESTING`; the new `CurrentNativeModeByte()` fence is
   what keeps `GetGameMode()` out of that binary. If the tests crash at
   runtime, look for any other raw-memory read that slipped in.
3. `PhaseTransitionPayload` echo in `SendAck` now passes the whole payload
   struct by const ref — signature changed inside transition_barrier.cpp
   only; no external callers.
4. Renamed/deleted symbol stragglers — grep-verified zero references at edit
   time for: `MatchBootstrap_*`, `PacketType::GekkoReady`,
   `PacketType::DelayChangeReq/Ack`, `GekkoReadyPayload`,
   `GEKKO_READY_FLAG_*`, `DelayChangeReq/AckPayload`,
   `FrontendInputSync_GetPhaseSerial`, `IsCurrentEpochPhaseSerial`,
   `GetSharedDelay`, `IsDelayNegotiated`, `FinalizeDelayNegotiation`,
   `OnRemoteSyncAnnounce/Confirm`, `OnRemoteDelayChange*`,
   `RollbackSession_BufferGekkoPacket`, `.gekko_avg_ping/.gekko_jitter`,
   live `phase_serial` fields, `frontend_delay_proposal/shared`.
5. `RollbackSession_OnSyncHashPacket` must exist in BOTH adapter TUs —
   Gekko no-op is in rollback_session.cpp; the AS2_WITH_GEKKO=OFF config
   uses the engine2 implementation. The router calls it unconditionally.
6. `frontend_sync_tests` exercises the new interrogation path — the
   `FrontendInputSync_FrameUpdate` loop tests assume one starvation tick
   per call while the current remote frame is missing; if the counter
   semantics change, the 120/360 constants in the test are the mirror.
7. MSVC unused-variable warnings possible in match_setup
   (`s_loadBarrierSent`, `s_remoteCapabilities` are write-mostly — same as
   the deleted TUs).

---

## 2026-08-17 — M6 (match_director + engine2 CUTOVER; ships with M5)

**Cutover state: `AS2_WITH_GEKKO` default flipped to OFF — engine2 is the
live gameplay path.** The Gekko configuration remains the legacy/fallback
config (§8.3): it must keep COMPILING (both configs chased through every M6
edit) but is no longer default; `lib/GekkoNet` untouched. Deviation from the
plan-M6 letter ("Remove GekkoNet: lib, flag, branch"): the flag/branch/lib
survive as the fallback build until M8 field acceptance — deleting them
before the cutover gate has actually RUN (builds are forbidden in this
session) would destroy the §8.3 partial-fallback option.

### M6-1: `src/rollback/match_director.cpp` — online_wiring.cpp DELETED

- **Files:** `src/rollback/match_director.cpp` (new, ~1130 lines),
  `src/rollback/online_wiring.cpp` (DELETED), `include/rollback/online_wiring.h`
  (facade preserved; additive ladder API), `CMakeLists.txt`.
- **Done:** all 13 `OnlineWiring_*` facade functions re-implemented as the
  §2.6 director. Changes vs the ported wiring:
  - **Engine survival (§2.6.3/.5, G2):** match end now calls
    `RollbackSession_SuspendBetweenMatches` (new facade fn) instead of
    `RollbackSession_End`; full End happens ONLY on director-ordered session
    teardown (disconnect/shutdown). `StopRollbackSession(reason, sessionTeardown)`
    is the single ordered stop.
  - **INV-9 ladder:** `OnlineWiring_MatchEndLadderAllows/NotifyConsumed`
    (additive facade): armed at MatchEnd; EpochAlign consumption refused
    (held; barrier keeps re-acking) until WinScreenExit AND PostMatchDecision
    are committed/consumed locally; ladder completion retires the earlier
    slots (stale-commit leak across boundaries closed). Fail-open watchdog:
    600 held ticks (~10 s) releases the gate into the §4.6 recovery ladder
    (INV-12 — the gate itself must never wedge a session).
  - **match_exit_pending (M4 obligation closed):** derived per frame from
    MatchLifecycle (phase MatchEnd/PostMatchRoute or nonzero exit-route
    byte) and mirrored via `RollbackSession_SetMatchExitPending`.
  - gameplay_bridge is DELETED (inventory §11): the director sets/verifies
    PlayerMapping and calls `RollbackSession_Begin` directly.
    `frame_lineage` deleted too (trivial mapping inlined in the Gekko
    adapter). mod_main's bridge init/frame/shutdown/HUD uses replaced with
    facade queries.
  - netplay_pacing calls replaced: FrameScheduler_OnSessionReset at session
    resets; snapshot tick scales report `FrameScheduler_GetSpeedScale()`;
    `stall_threshold` snapshot field pinned 0.
- **Deviation (F-7):** the PostMatchDecision-vs-lockstep contradiction
  TERMINAL is NOT enabled — today's wire intents are inconsistent by design
  (continue_flow's decline proposes ReturnToSession while the charsel
  auto-rematch path proposes Rematch for the same "any NO" route), so a
  strict compare would kill healthy sessions. Ladder ordering is enforced;
  the intent unification + terminal is an M7 obligation.

### M6-2: dispatcher rewired onto engine2 typed stalls; netplay_pacing DELETED

- **Files:** `src/patches/input_override.cpp`, `src/net/netplay_pacing.cpp`
  + `include/net/netplay_pacing.h` (DELETED), `include/net/delay_policy.h`,
  `src/net/delay_policy.cpp`, `src/rollback/rollback_session.cpp` (log trims),
  `tools/check_killpaths.ps1`, `src/net/match_lifecycle.cpp`.
- **Done:**
  - The dead `#if 0` Gekko frames-ahead throttle block (~690 lines) deleted.
  - Netplay branch (§2.8.3): BeginFrame → ProcessNextEvent loop preserved;
    the ONLY pre-BeginFrame hold left is ChurnPause (ExternalSuspension).
    A pass whose event stream contains no Advance is an N=0 engine hold:
    freeze pulse + vanilla timeout clears + Session_Update/PollSession pump
    (typed HoldCause was already reported by the adapter from NextAction).
    Catch-up headroom now uses `predicted_frames_outstanding` (the legacy
    `prediction_debt` is 0 on engine2 by INV-1).
  - `DelayPolicy_GetStallThreshold`/`GetProtectionWindow` deleted (§2.8.7);
    snapshot fields report 0 (menu UI shows nothing at 0); DELAYMAP log
    keeps its columns as diagnostics.
  - `GetTimesyncDebugInfo` re-fed from FrameScheduler snapshot + telemetry.
  - **Kill-path burn-down:** AbortRollbackDispatcher routes any residual
    live session through `Session2_Terminate(TransportFailed)` BEFORE the UI
    funnel; match_lifecycle's session-lost guard no longer calls
    HandleDisconnection (unwinds match ownership via its own OnDisconnect);
    allowlist updated (match_lifecycle entry removed).
  - **Frontend-phase STAT producer (M2/M5 note closed):** charsel lockstep
    wait + winscreen lockstep wait now report
    `FrameScheduler_NotifyHold(LifecycleBoundary, false)`.

### M6-3: engine2 adapter obligations closed (rotation, exact window, stress, diagnostics)

- **Files:** `src/rollback/rollback_session_engine2.cpp`,
  `include/rollback/rollback_session.h` (additive: SuspendBetweenMatches,
  SetMatchExitPending, snapshot peer_* advisory fields),
  `include/rollback/engine2.h` + `src/rollback/engine2.cpp` (PredictionTap).
- **Done:**
  - **Cross-match RotateEpoch (M5 obligation closed):** a suspended armed
    engine rotating under a higher epoch continues the canonical counter
    (INV-15); rotation executes §2.6.5: StateHistory_Reset +
    SetTagContext(new epoch) + DesyncDump_Reset + delay re-request +
    per-boundary adapter state clear. Falls back to full re-arm when R or
    the player slot changed, the epoch did not increase, or the engine
    faulted. game-abs mapping is now epoch-relative
    (`rb - RbEpochOrigin()`), so fx/HUD abs frames stay per-match.
  - **§2.8.6(d) replay mode-bail:** a committed replay tick that left
    mode-8/sub-3 truncates via `FinishRollbackAtBoundary` instead of
    replaying the stale speculative suffix.
  - **SyncHash rng/hp diagnostics:** `DetVer_GetRngSeed` + P1/P2 HP words
    captured pre-tick into the confirm seam / SyncHash payloads.
  - **Stress ingest hooks:** delivery-delay queue (16-slot, frame-released)
    ahead of `IngestInputStream`; forced mismatch via a new engine
    `PredictionTap` (corrupts PREDICTIONS through
    `StressHooks_MaybeCorruptPrediction`, never actuals — INV-19 intact);
    tap refreshed per PollSession so the mod-menu toggle works live.
  - Link stats (`link_avg_ping/link_jitter`) and telemetry now fed from
    time_probe (p50 RTT / p95-p50 jitter, ms display of µs source).
- **Gekko adapter:** `SuspendBetweenMatches` = alias to End (per-match
  engine lifetime); `SetMatchExitPending` = no-op; snapshot peer_* fields
  memset-zero.

### M6-4: time_probe + frontend-delay latch + DelayPolicy re-feed + coverage

- **Files:** `include/net/time_probe.h` + `src/net/time_probe.cpp` (new),
  `src/net/packet_router.cpp` (routes 77/78), `src/net/session2.cpp`
  (4 Hz drive from Session_Update while Connected/Ready; reset in
  ResetState), `src/net/frontend_input_sync.cpp`, `src/net/delay_policy.cpp`,
  `include/net/delay_policy.h`.
- **Done:** §2.9.3 verbatim: QPC µs stamps, dwell subtraction,
  generation gate, 32-sample rolling window, 4x-median local-stall
  rejection; latched frontend delay (ceil(oneway)+1, +2 at >=6 frames,
  1/0.15 s rise, 1/0.30 s decay, >=12-sample warmup) consumed by
  `ComputeDelayProposalDetails` (ms-derived value stays the pre-window
  fallback — harness behavior unchanged). DelayPolicy_FrameUpdate prefers
  the probe (QOH99 lesson 7); `DelayPolicy_ClassifyLocalCoverage` implements
  the §2.9.1 directional math (FullSpeed/Marginal/Underbuffered,
  margin = max(2, oneway/3)).
- **Deviation:** oneway→frames conversion uses the fixed 16667 µs proper_60
  period (compat_58 differs by 2% — negligible for a delay estimate; noted
  for the M8 review).

### M6-5: winscreen same-epoch Realign (M5 deviation 3 closed)

- **Files:** `src/net/match_setup.cpp`, `src/net/winscreen_sync.cpp`,
  `include/net/winscreen_sync.h`.
- **Done:** a Realign escalation latched while the winscreen stream owns the
  exchange (pregame Idle/GameplayHandoff, mode 9) re-runs EpochAlign for the
  CURRENT epoch with `first_phase=WinScreen`; the peer echoes on seeing the
  proposal; commit → both sides `WinScreenSync_RestartLockstep` (new: stop +
  re-begin the input phase under the unchanged epoch, NO WinScreenExit
  proposal, frame index space restarts identically on both sides; the
  continue prompt re-runs from scratch via the existing
  `ContinueFlow_Reset` in Begin). 5 s commit timeout falls back to the
  fresh-epoch pregame restart. The same-epoch re-run is deliberately EXEMPT
  from the INV-9 ladder (the ladder gates the NEXT epoch's alignment).

### M6-6: sticky Disconnect resend-until-acked (M3/M4 deviation closed)

- **Files:** `src/net/session2.cpp`, `include/net/transport2.h`,
  `src/net/transport2.cpp`.
- **Done:** new `Transport2Stats.peer_reliable_in_transit` (worker stamps
  ENet `peer->reliableDataInTransit` per service pass). Fault terminals now
  linger up to 2 s, resending the reasoned Disconnect every 100 ms and
  exiting EARLY once the reliable queue drains (acked) or the peer object
  detaches. UserCancel/GameExit keep their existing bounded goodbyes.

### M6-7: HUD + diagnostics surface

- **Files:** `src/ui/netplay_hud.cpp`, `src/patches/input_override.cpp`
  (GetTimesyncDebugInfo), `src/core/mod_main.cpp`.
- **Done:** progress-stall banner ("Opponent's game stopped responding
  (Ns)", from `ConnectionSupervisor_IsProgressStallWarned/GetProgressStallMs`
  — M3 obligation closed); coverage badge ([Underbuffered]/[Marginal],
  INV-6 — FullSpeed draws nothing); hold-cause line (run_state name +
  peer prediction depth from PressureReport) replacing the NETCLASS/debt
  vocabulary. Disconnect UI already surfaces `Session2TerminalReasonName`
  via Session2's error text (verified — no extra wiring needed).

### M6-8: EpochAlign unit coverage (M5 gate deviation closed)

- **Files:** `tests/transition_barrier_tests.cpp` (new), `CMakeLists.txt`
  (new target `transition_barrier_tests` linking the real module with
  stubbed transport/logging), `tests/frontend_sync_tests.cpp` (+1
  Session_SendPacket stub; target gains `src/net/time_probe.cpp`).
- **Done:** T-TB-1..7 pin plain commit, the INV-10 {epoch, first_phase}
  commit rule + mismatch refusal + higher-epoch re-proposal, echo-verbatim
  acks (INV-13), idempotent re-ack + stale-ack rejection, Clear semantics
  (stale remote proposal can never re-commit), consume-once, and the
  barrier half of T-LADDER (all 6 arrival orders of the 3 match-end kinds
  commit). The director's consume-ORDER gate is not linkable standalone
  (game-memory TU) — exercised in-game via the LADDER log lines.

### Build-system summary (M6)

- `AS2_WITH_GEKKO` default ON → **OFF** (cutover). ON must still configure/
  compile (fallback config; GekkoNet subdir/link only in that config).
- Deleted files: `src/rollback/online_wiring.cpp`, `src/net/netplay_pacing.cpp`,
  `include/net/netplay_pacing.h`, `src/net/gameplay_bridge.cpp`,
  `include/net/gameplay_bridge.h`, `src/rollback/frame_lineage.cpp`,
  `include/rollback/frame_lineage.h`.
- Added: `src/rollback/match_director.cpp` (ROLLBACK_SOURCES),
  `src/net/time_probe.cpp` + `include/net/time_probe.h` (NET lists),
  `tests/transition_barrier_tests.cpp` (+target/add_test).
- frontend_sync_tests source list: +`src/net/time_probe.cpp`.

### Deferred (with reasons) → M7/M8 obligations

- **GekkoNet full removal** (flag, Gekko adapter TU, vendored lib): only
  after the cutover gate (§7 suite + LE-1) actually PASSES on a build —
  M8 exit review item. Until then AS2_WITH_GEKKO=ON is the §8.3 fallback.
- **F-7 contradiction terminal:** unify PostMatchDecision wire intents
  (continue_flow decline vs auto-rematch propose different intents today),
  then enable the fail-closed compare in the director ladder (M7).
- **PeerIdentity round/timing folding into config exchange** (M3 note):
  wire churn with no functional gain this milestone — reconsider at M7
  alongside the spectator re-hookup packet review.
- **Load/GO onto TransitionBarrier consolidation** (M5-1 deviation 1):
  the director consumes the shipping packet flows fine; consolidation
  remains optional (M7+, only if a director-driven reload lands for the
  baseline retry path).
- **Baseline retry with a real asset reload** (M5-1 deviation 2): still a
  recapture-only retry; a mode-rewinding reload needs new director
  machinery (M7 candidate).
- **Frontend-phase per-phase STAT rollup:** the M6 producer labels the
  dispatcher wait sites (LifecycleBoundary); a phase-aware per-phase
  breakdown remains open (M7, low priority).
- **M8:** in-game STAT acceptance runs (§7.5) incl. the 1.5 ms savestate
  p99 on min-spec; LE-1 replication; rematch soak 100 cycles now exercising
  RotateEpoch (assert: epoch strictly increasing AND canonical counter
  monotonic across the whole session log); compat_58 oneway-frames review.

### Compile risks to check first (M6 build session)

1. **Both configs must build:** `-DAS2_WITH_GEKKO=OFF` (new default) and
   `-DAS2_WITH_GEKKO=ON` (fallback). The ON config compiles
   rollback_session.cpp with the two new facade fns appended near
   `RollbackSession_IsActive` — first place a namespace slip surfaces.
2. `match_director.cpp` is a new ~1130-line TU with online_wiring's include
   set minus gameplay_bridge/netplay_pacing plus frame_scheduler.h — check
   for now-unused includes (determinism_verify, resimulation) warnings and
   the `Net::PlayerMapping_*` signatures.
3. `rollback_session_engine2.cpp`: two `__try` blocks landed in functions
   with C++ locals — MSVC C2712 fires if any local needs unwinding in the
   same function (ReadSyncHashDiagnostics and the replay mode-bail block in
   ProcessNextEvent are POD-only by construction; if C2712 appears, hoist
   the `__try` into a helper).
4. `time_probe.cpp` under WIN32_LEAN_AND_MEAN in the frontend_sync_tests
   target (no winsock dependency — pure QPC/GetTickCount); the tests' new
   `Session_SendPacket` stub must match the header signature exactly
   (uint8_t, PacketType, const void*, size_t, bool).
5. `transition_barrier_tests` stubs only 4 LogWindow functions — if
   LOG_NETPLAY expands to more than LogWindow_LogCat on this toolchain,
   add the missing stubs (frontend_sync_tests has the full list to copy).
6. `netplay_hud.cpp` now includes frame_scheduler.h/run_state.h — RunState
   is `Rollback::RunState`; check for LogLevel/LOG_* macro collisions with
   imgui headers in that TU.
7. Grep-verified zero remaining references at edit time:
   `NetplayPacing_*`, `GameplayBridge_*`, `FrameLineage_*`,
   `DelayPolicy_GetStallThreshold`, `DelayPolicy_GetProtectionWindow`
   (comments only). `OnlineWiringSnapshot.stall_threshold` and
   `DelayPolicySnapshot.stall_threshold/protection_window` fields survive
   (report 0) for rollback_debug/menu_ui shape stability.
8. The engine2 rotation path calls `StateHistory_SetTagContext` with
   MODE_MATCH from core/as2_constants.h — already in the include set.
9. `s_engine.SetPeerAdvisory` remains caller-less (IngestInputStream applies
   the advisory internally) — MSVC may warn on the unused decl only, no
   action needed.

---

## 2026-08-17 — M7 (spectator/replay re-hookup onto the confirm seam)

### M7-1: S-6 — engine2 → SpectatorRuntime confirmed-only push

- **Files:** `src/rollback/rollback_session_engine2.cpp`,
  `include/net/spectator_runtime.h`, `src/net/spectator_runtime.cpp`,
  `include/net/spectator_protocol.h`.
- **Done:**
  - `DrainConfirmSeam` (the §2.7.3-F single immutable seam) now pushes every
    popped `ConfirmedFrame` to the sidecar via a new additive
    `SpectatorRuntime_OnConfirmedFrame(rb, game_abs, p1, p2, pre_state_hash)`
    — confirmed-only by construction: no rewrite flags, watermark == frame,
    each archive slot written exactly once. The PRESERVED
    `SpectatorRuntime_OnGameplayFrame` signature is untouched (the Gekko
    fallback adapter still calls it).
  - **Frame numbering:** the seam pushes MATCH-RELATIVE rb frames
    (`RbFrame − RbEpochOrigin`, 0-based per epoch) + the matching
    `game_abs` — identical semantics to the per-match Gekko engine, so the
    sidecar protocol/clients see no change across the cutover (inventory §6
    match_id formula + payloads untouched). Records from a pre-rotation
    epoch that race the drain are dropped by a `matchRel >= 0` guard.
  - **Boundary flush:** `RollbackSession_SuspendBetweenMatches` and `_End`
    drain the seam BEFORE deactivating (the director fires
    `SpectatorRuntime_OnMatchEnd` right after Suspend returns, and rotation
    re-bases the epoch origin — un-flushed frames would be unmappable).
  - `FRAME_FLAG_ROLLBACK_REWRITE` retired on the engine2 path (comment marks
    it legacy; still set by rollback_session.cpp in the ON config, still
    honored by clients — wire-compatible both directions).

### M7-2: S-4 — record hash verification (host stamp + client verify)

- **Files:** `include/net/spectator_protocol.h`, `src/net/spectator_runtime.cpp`,
  `include/net/spectator_client.h`, `src/net/spectator_client.cpp`,
  `include/rollback/game_snapshot.h`, `src/rollback/game_snapshot.cpp`,
  `src/net/spectator_playback.cpp`, `include/net/spectator_playback_policy.h`.
- **Done:**
  - `FrameRecord`'s 3 pad bytes now carry a 24-bit fold of the confirmed
    pre-state Block64 digest under new flag `FRAME_FLAG_HAS_HASH` — wire
    size pinned at 16 B (static_assert added), sidecar protocol stays v8,
    old/new hosts and clients interoperate (absent flag = no verification).
  - New `GameSnapshot_HashGameplayLive()`: the SIM-membership digest
    computed directly over live game memory, mirror-exact to
    `GameSnapshot_HashGameplay` (same SimHeader order, same region order,
    same Block64 chaining) — one read pass, no capture memcpy, __try
    guarded. This is the client-side comparison source.
  - Playback verifies each record's hash BEFORE ticking it (which also
    checks the previous tick's post-state). **Sync-acquire model:** strict
    mode arms on the first agreeing record; a stream that never agrees
    within 600 records logs loudly and degrades to unverified (the viewer
    enters via the native charsel/loader path, not the players' baseline
    rendezvous — a systematic entry offset must not kill every session).
    After acquisition a mismatch is a genuine divergence: fail-closed
    latch → consumed by `SpectatorPlayback_FrameUpdate` →
    `SpectatorClient_Disconnect` + PlaybackError (leave the session; the
    player link is untouched — publication is observational, per S-4).
  - New additive `SpectatorClient_GetFrameHash(rb, *hash24, *has)`.

### M7-3: §2.8.8 — elastic spectator pacing (policy port)

- **Files:** `include/net/spectator_playback_policy.h` (new, pure header),
  `src/net/spectator_playback.cpp`, `CMakeLists.txt`.
- **Done:**
  - Policy ported from qoh99_netplay `game/SpectatorPlaybackPolicy.h` with
    the plan's watermarks: live target/startup prime 240 (~4 s), rebuffer
    resume 120, catch-up enter >300 with hysteresis release at ≤240,
    elastic slow-motion 950/850/700 permille at 180/120/60 records
    (Bresenham credit — distributed single-slot holds, never invents or
    skips a record), deep-backlog tick ladder 2/4/8/16/24 by rung, 12 ms
    wall slice, sealed-stream override (a finite tail never waits for a
    threshold it cannot reach). All pure/constexpr, unit-testable.
  - `UpdateLivePlayback` rebuilt on the policy: startup prime & rebuffer
    gates (freeze until cushion), elastic holds under low water (freeze
    pulse for exactly the held slot), catch-up budget from the ladder
    (replaces the old `1 + gap/30, cap 9` heuristic), manual hotkey
    override retained (caps the budget while set), sealed-tail drain for
    match-end/transport-failure playback preserved.
  - `SpectatorPlayback_GetDispatcherFrame` enforces the 12 ms wall slice
    across hidden catch-up ticks (QPC-anchored at the first tick of each
    presentation slot; executed ticks keep their credit — INV-21 analog);
    yields at phase boundaries exactly as before (mode gate + budget).
- **Deviation (deliberate):** the viewer now intentionally runs ~4 s behind
  the confirmed edge (QOH99 model, S-2 spec). The old code chased gap→0 and
  froze on every jitter bubble. The prime largely overlaps the pre-match
  intro, but a mid-intro join will visibly hold up to ~4 s once before
  playback starts — that is the design (latency-biased observational view).

### M7-4: S-5 — replay recorder onto the confirmed pipeline; epoch chapters; additive format

- **Files:** `include/replay/replay_runtime.h`, `src/replay/replay_runtime.cpp`,
  `src/rollback/rollback_session_engine2.cpp`.
- **Done:**
  - New `ReplayRuntime_OnConfirmedFrame(epoch, game_abs, p1, p2, pre_hash)`
    fed from `DrainConfirmSeam`: the recorder buffers exactly one epoch's
    confirmed input stream (u16 canonical words) + a pre-state digest every
    30 frames (§2.7.7 cadence). **Epoch boundary = chapter boundary**: an
    epoch change resets the buffer; the native one-file-per-match
    `ReplaySave` consumes it — chapters and files are 1:1 by construction.
    Contiguity is asserted (seam pops in order; breaks are counted and
    logged, defensive only). Recorder is netplay-context-gated at save
    (same gate as the netplay rename) so a later local save can never
    inherit a stale netplay stream.
  - `Hook_ReplaySave` appends a new self-identifying trailer chunk
    `AS2RCFM1` v1: `{version, epoch, first_game_abs, record_count,
    hash_count, payload_crc}` + records + hash points. Appended after the
    palette trailer; CRC-protected.
  - **Trailer loader reworked into a chunk scanner** (`AS2RPAL1` /
    `AS2RCFM1`, any order, unknown chunk stops the scan non-fatally). The
    palette chunk's parse semantics are byte-identical for old files —
    playback of 0.6 files unaffected (the S-5 requirement); the "v-bump" is
    realized as the new chunk's own magic+version rather than mutating the
    native 0x48 header (which the game itself parses).
  - **Playback verification:** during replay playback the dispatcher-fed
    inputs are compared per frame against the confirmed stream (under the
    native tape's 8-button mask — the tape stores bytes) and the live
    digest (`GameSnapshot_HashGameplayLive`) against the recorded cadence
    hashes, with the same sync-acquire model as the spectator (20 cadence
    points). Replays are offline artifacts, so divergence fail-DEGRADES
    with loud `REPLAY DESYNC` logs + counters (nothing to tear down);
    takeover mode disables verification (it diverges by design). This is
    the instrument for the M8 "plays back hash-clean" exit-gate run.
- **Note:** the mapping anchor is `first_game_abs` — the game-abs domain ==
  the native tape index domain (both are the game's per-match input
  write-index timeline; verified against `ReadObservedFrame`/
  `WriteReplayHistoryFrame`).

### M7-5: F-7 — PostMatchDecision intent unification + contradiction terminal (M6 obligation closed)

- **Files:** `include/net/protocol.h`, `src/net/continue_flow.cpp`,
  `src/net/netplay_menu_controller.cpp`, `include/rollback/online_wiring.h`,
  `src/rollback/match_director.cpp`, `tools/check_killpaths.ps1`.
- **Done:**
  - **Unification:** `PostMatchIntentWire::CharselRestart` (4, additive —
    payload byte/size unchanged). The lockstep-derived intents are now
    disjoint from the user-action intents: `Rematch` = YES,YES fast path
    only; `CharselRestart` = any-NO charsel route (continue_flow decline
    AND the menu controller's auto-rematch announce — previously
    `ReturnToSession` vs `Rematch` for the same route, which is why M6
    could not enable the compare). `wireSuggestsRematch` accepts either
    Rematch or CharselRestart from the peer.
  - **Terminal:** continue_flow registers the lockstep-derived answer via
    new `OnlineWiring_SetExpectedPostMatchIntent()` at resolution; the
    director compares it against the remote PostMatchDecision proposal
    every FrameUpdate while the boundary is live. A contradiction between
    the two LOCKSTEP-DERIVED values (Rematch vs CharselRestart) = divergent
    lockstep streams → `Session2_Terminate(ProtocolViolation)` →
    `MatchLifecycle_OnDisconnect` → `NetMenu::HandleDisconnection` (typed-
    first, same order as the supervisor terminals). User-action intents
    (ReturnToSession/Disconnect from the post-match menu) are EXEMPT —
    they are choices, not derivations; a strict compare there would kill
    healthy sessions. Expectation clears on ladder arm/disarm.
  - Kill-path gate: `match_director.cpp = 1` allowlisted with the INV-12
    justification (protocol violation is an allowlisted teardown cause,
    §4.6 row 4).

### M7-6: Deferred-obligation decisions (logged per the plan rule)

- **PeerIdentity round/timing folding (M3/M6 note): REVIEWED, RETAINED.**
  The spectator re-hookup confirmed the packet is load-bearing as-is (the
  sidecar's name/round data flows from SessionSnapshot, which PeerIdentity
  fills); folding round/timing into config exchange would churn three
  frozen wire pins for zero functional gain. Obligation closed as
  "reviewed at M7, keep".
- **Load/GO → TransitionBarrier consolidation (M5-1 dev. 1): STILL DEFERRED.**
  Plan condition ("only if a director-driven reload lands") not met — no
  reload machinery was built at M7.
- **Baseline retry with real asset reload (M5-1 dev. 2): DEFERRED to M8+.**
  Needs mode-rewinding director machinery; the shipped recapture retry
  satisfies §2.5's one-retry semantics for transient divergences, and a
  deterministic divergence still terminates one retry later as specified.
- **Frontend per-phase STAT rollup (M2/M6 note): DEFERRED (low priority).**
  The STAT line is the frozen §7 acceptance instrument (M0: "do not
  change"); frontend waits are already labeled LifecycleBoundary since M6,
  and the hold-episode ledger logs give per-phase visibility. A separate
  PSTAT line remains an option if M8 field analysis wants it.

### Build-system summary (M7)

- CMake: `include/net/spectator_playback_policy.h` added to `NET_HEADERS`.
  No sources added/deleted; no test-target changes.
- **Both configs chased:** ON (Gekko fallback) compiles everything M7 touched
  — spectator_runtime/client/playback, replay_runtime, protocol.h,
  continue_flow, menu controller, match_director, game_snapshot are all
  config-independent TUs; the ON config simply has no
  `OnConfirmedFrame` producers (Gekko adapter untouched, still pushes
  `OnGameplayFrame` with rewrite flags; recorder stays empty; records carry
  no hashes → clients skip verification). `lib/GekkoNet` untouched.
- API_FREEZE §6 updated (M7 wire-state paragraph).

### Obligations for M8

- **Exit-gate runs (need builds):** spectate a full 3-match rematch session
  incl. late join (deep-backlog ladder) + starve test (elastic holds, no
  freeze-then-sprint) — watch `SPLAY` logs for prime/rebuffer/elastic
  transitions and the S-4 ACQUIRED line; replay the same session and check
  zero `REPLAY DESYNC` lines (hash-clean gate).
- **S-4 acquire tuning:** if field spectates show the 600-record acquire
  window never arming (systematic baseline offset between the charsel-boot
  path and the players' baseline), the hash membership of the spectator
  comparison needs the M4-6 audit treatment before strict mode can be
  meaningful; the fail-degrade path keeps sessions alive meanwhile.
- **Elastic prime UX:** if the ~4 s prime reads as a hang to users, surface
  the existing Buffering status ("Priming the playback cushion (n/240)")
  in the spectator HUD prominently (it is already in the status string).
- **F-7 in the rematch soak:** extend `rematch_soak` assertions to include
  zero `F-7 TERMINAL` lines across 100 healthy cycles (the guard must
  never fire on a healthy session — both YES,YES and any-NO cycles).
- **Gekko full removal** (flag, adapter TU, vendored lib) remains gated on
  the §7 suite + LE-1 passing on real builds (M6 note stands).

### Compile risks to check first (M7 build session)

1. **Both configs:** `-DAS2_WITH_GEKKO=OFF` (default) and `ON` (fallback).
   The OFF config's `rollback_session_engine2.cpp` now includes
   `net/spectator_runtime.h` + `replay/replay_runtime.h` — first place an
   include-order slip surfaces (replay_runtime.h forward-declares
   `Net::NetplayPaletteBank`; no heavy includes).
2. `spectator_playback.cpp` gained an explicit `#include <windows.h>` (QPC)
   after the project headers — if a winsock/winsock2 clash appears in that
   TU, the fix is WIN32_LEAN_AND_MEAN ordering, not removing the include
   (the TU previously reached windows.h transitively).
3. `GameSnapshot_HashGameplayLive` uses `__try` with POD-only locals —
   MSVC C2712 fires if anyone adds an unwindable local later (same
   discipline as the adapter's existing SEH blocks).
4. `replay_runtime.cpp`: new `Rollback::NetplayLog_Write` calls needed
   `rollback/netplay_log.h` (added). `ConfirmedInputRecord` is memcpy'd —
   4-byte static_assert added; if MSVC pads it the assert is the truth.
5. `spectator_protocol.h` `FrameRecord` static_assert (16 B) — the struct
   is pack(1); if it fires something else changed.
6. `frontend_sync_tests` does NOT link continue_flow.cpp — the new
   `OnlineWiring_SetExpectedPostMatchIntent` call sites need no stub;
   verified against the target source list.
7. Policy header constexpr functions with multiple statements/static_asserts
   — C++20 targets only (all test targets already set CXX_STANDARD 20).
8. `kCatchupBudgetStepGap`/`kCatchupBudgetMax`/`ComputeAutoCatchupScale`
   deleted from spectator_playback.cpp — grep-verified zero remaining
   references at edit time.

---

## 2026-08-17 — M8 (soak & field acceptance: code/tooling side — FINAL milestone)

Scope split per the M8 brief: the in-game acceptance gates (two-instance
soaks, LE-1 field-log replication, perf p99, hash-clean replay, min-spec
benchmarks) are FIELD RUNS the user performs; this session implemented the
tooling, assertions, and documentation that judge them. **GekkoNet stays**
until those runs pass (§8 criteria recorded in the runbook, not executed).

### M8-1: Rematch soak upgraded for the re0.7 backend (M6/M7 obligations)

- **Files:** `src/testing/rematch_soak.cpp`, `include/testing/rematch_soak.h`.
- **Done:**
  - **Epoch assertions (M6 obligation):** per-frame monitor of
    `PregameSync_GetCurrentEpoch()` — any regression = FAIL; every rematch
    `GameplayHandoff` entry must carry a STRICTLY higher epoch than the
    previous handoff (§2.5 rotation) or the iteration FAILs.
  - **Canonical-counter assertion (INV-15, M6 obligation):**
    `RollbackSession_GetCurrentFrame()` must be monotonic. On engine2 the
    high-water persists across match boundaries (the engine stays armed
    through suspend/rotate); on the `AS2_WITH_GEKKO` fallback config the
    check is `#if`-relaxed to continuous-activity windows (per-match engine
    lifetime — the target-wide `AS2_WITH_GEKKO=1` define selects the branch).
  - **Route labeling:** iterations are tagged `path=fastpath` (no frontend
    phase seen — YES,YES EpochAlign(None) route) vs `path=charsel` (any-NO);
    PASS lines carry epoch/path/canonical; SUMMARY gains
    `fastpath= charsel= epoch_max= canonical_max=` fields. The PASS edge
    itself (GameplayHandoff re-entry) was verified still correct against
    match_setup's phase machine — both rematch routes leave and re-enter
    GameplayHandoff (match_setup.cpp lines 1361/1844/2161-2201).
  - **F-7 (M7 obligation):** no dedicated probe needed — the contradiction
    terminal tears the session down (ProtocolViolation), which the existing
    disconnect-shaped FAIL detection catches; the explicit
    zero-`F-7 TERMINAL` grep is a gated criterion in `analyze_stat.py` and
    the runbook checklist. Documented in the soak header.

### M8-2: Autoconnect driver — continue-prompt NO driving

- **Files:** `src/net/netplay_menu_controller.cpp`.
- **Done:** new `[autoconnect]` key **`continue_no_every = K`** — every Kth
  continue prompt is answered NO (one INPUT_RIGHT tap toggles the cursor,
  then INPUT_A locks >= 20 frames later; both taps are rising-edge
  injections matching ContinueFlow's lock semantics). Deterministic across
  peers (the completed-match counter advances in lockstep on both drivers);
  a single side answering NO also routes both to charsel. YES remains the
  default (unchanged logic). Config log line extended with
  `continue_no_every=`. This closes the §7.4 "100 YES cycles + mixed NO
  cycles" driving gap — the driver already handled the fast path
  (`MODE_PREMATCH_INTRO`/`MODE_MATCH` in ConfirmingWinScreen) and charsel
  re-entry.

### M8-3: `tools/analyze_stat.py` — the §7.5 field-run judge (new)

- **Files:** `tools/analyze_stat.py` (new, Python 3 stdlib only).
- **Done:** parses the frozen STAT format (regex mirrors
  `NetplayLog_Stat`/async_log prefix exactly; format untouched) and outputs
  PASS/FAIL verdicts per §7.5: hold-cause breakdown + zero-holds gates
  (gameplay holds never excusable; `--allow-lifecycle N` documented escape
  for human-paced screens only), present p99 vs 17.2 ms (offline) / 18.0 ms
  (online), sim rate vs cadence +/-0.02 (proper_60/compat_58,
  auto-detected), rollback/rb_max gates per profile (offline=0; LE-1
  rb_max <= 8), slew bounds (|ppm| <= 25000 cap; 0 offline), debt/silence
  report, STAT-gap (log-loss/stall) detection, and an incident scan
  (`F-7 TERMINAL`, INV-8 `BeginInputPhase while phase`,
  `Sent ResyncRequest` — hard-zero in clean profiles per the M5 note —
  `ConfirmedDesync`, `REPLAY DESYNC`, `PacingClockDead`, soak verdict
  echo). Profiles: `offline`, `online-clean`, `le1`, `lossy`, `degraded`,
  `report`; `--json` for machines; exit codes 0/1/2. Verified against
  synthetic logs (PASS, FAIL via injected F-7 line, NO-DATA path).

### M8-4: `docs/re0.7/M8_ACCEPTANCE_RUNBOOK.md` (new)

- Self-contained field-run manual: prerequisites (both build configs, unit
  suite, min-spec definition, separate-folder rule), instrument reference,
  shaping-tool guidance — including the **INV-14 injection caveat**:
  app-egress `AS2_NET_INJECT_*` no longer moves supervisor bands (ENet
  acks/pings keep flowing), so burst/liveness rows need external shaping —
  the full §7 run matrix with run IDs, exact configs, expected outcomes and
  judge commands (R-OFF, R-CLEAN-*, **R-LE1**, R-POL, R-LOSS-*, R-DEG,
  R-UNDER with the INV-4 field pin, R-BURST-*, R-ONEWAY, R-WEDGE,
  R-SOAK-100, R-DET, R-SPEC, R-REPLAY, R-BENCH-* incl. the deferred 1.5 ms
  savestate-p99 min-spec judgment and depth-8 <= 10 ms), the §7.5 release
  checklist, a log-grep checklist (palette re-arm per epoch, LADDER order,
  SetTracker continuity, deferred-Begin), the §5 edge-matrix -> coverage
  map (the §7.4 "test id column", realized here), **§6 GekkoNet-removal
  criteria + removal task list**, and **§7 0.7-fallback criteria** (§8.3
  verbatim incl. the partial-fallback option).
- `docs/RESILIENCE_TESTING.md`: prepended an M8 update banner (supervisor
  threshold changes, the INV-14 injection semantic change that invalidates
  its §2 supervisor expectations, backend default change, soak/driver
  additions) pointing at the runbook as the M8 authority. **Deviation from
  §7.4's letter** ("matrix table gains a test id column in
  RESILIENCE_TESTING.md"): that doc never contained the §5 matrix; the
  coverage map lives in the runbook to keep one authoritative M8 document,
  with the pointer note in RESILIENCE_TESTING.md.

### M8-5: T-ENG-9 grep half closed (M4-8 deviation)

- **Files:** `docs/re0.7/M4_AUDITS.md` (new section + M6-closure note on the
  old open item).
- **Done:** full-tree budget-reader sweep documented: the single gating
  comparison (`engine2.cpp` NextAction, marked INV-4) plus five audited
  legitimate reader classes (config validation/logging; §2.8.3 catch-up
  headroom; observational run-state/telemetry; INV-6/INV-23 policy/UI/
  advisory surface incl. PressureReport `adv_rollback`; fx journal sizing).
  Verdict: INV-4 holds. Ride-along finding: root-level `temp_session.cpp`
  is a dead scratch file (not in any CMake target, references the old Gekko
  config) — flagged for deletion with the GekkoNet-removal commit.

### M8-6: Harness/launcher retired-concept sweep

- **Files:** `tools/test_harness_launcher.cpp`,
  `include/testing/harness_shared_memory.h` (comment).
- **Done:** launcher log triage now recognizes the engine2 session-begin
  line (`engine2 session begin`) alongside the legacy Gekko strings (kept —
  the ON fallback config still emits them); status/summary labels
  `gekko[...]` -> `rb[...]`. SHM struct comment updated. The harness SHM
  layout/version is untouched (rbSnap fields it reads all survived the M5
  facade renames; epoch/canonical visibility comes from the soak log lines,
  not a breaking SHM change). `autoconnect_harness.cpp` itself needed no
  changes (already facade-clean; grep-verified zero retired symbols in
  `src/testing/`).

### M8-7: Deferred-obligation decisions (logged per the plan rule)

- **Per-phase STAT rollup (M2/M6/M7 note): REMAINS DEFERRED, by decision.**
  The STAT format is the frozen §7 instrument (M0: "do not change");
  frontend waits are labeled LifecycleBoundary since M6 and the
  hold-episode ledger gives per-phase visibility. A separate PSTAT line
  stays an option IF the M8 field analysis shows a need — no field data
  exists yet, so adding it now would be speculative surface. Recorded in
  the runbook caveats.
- **compat_58 oneway-frames review (M6-4 deviation): REVIEWED, RETAINED.**
  The fixed 16 667 us conversion under compat_58 overestimates required
  coverage by <= 2% — errs toward MORE delay/rollback margin (the safe
  direction, QOH99 lesson 15). Analyzer takes `--cadence 58.8` for the
  sim-rate target; no code change.
- **GekkoNet removal: NOT EXECUTED (per brief).** Both `AS2_WITH_GEKKO`
  configs still compile; removal criteria + one-commit task list are §6 of
  the runbook, gated on the field runs.
- **Baseline retry with real asset reload (M5-1 dev. 2): remains deferred**
  (M7 decision stands; no director reload machinery exists and the
  recapture retry satisfies §2.5's semantics).

### Build-system summary (M8)

- **No CMakeLists.txt changes.** No sources added/removed; edits touch
  existing TUs (`rematch_soak.cpp`, `netplay_menu_controller.cpp`,
  `test_harness_launcher.cpp`) plus a standalone Python tool and docs.
  Both `AS2_WITH_GEKKO` configs chased: the soak's INV-15 branch is the
  only config-sensitive edit (`#if defined(AS2_WITH_GEKKO)`, define
  already target-wide on the ON config); everything else is
  config-independent.

### Compile risks to check first (M8 build session)

1. `rematch_soak.cpp` now includes `rollback/rollback_session.h` — check
   for LOG_*/macro collisions with `ui/log_window.h` in that TU (the same
   pairing already exists in autoconnect_harness.cpp, so low risk).
2. The soak's new `#if defined(AS2_WITH_GEKKO)` branches: the OFF config
   must not reference the define; the ON config picks the relaxed branch —
   build BOTH configs once.
3. `netplay_menu_controller.cpp`: the ConfirmingWinScreen sub==4 block was
   restructured (single `if (sub == 4)` with YES/NO arms) — verify no
   dangling reference to the old one-liner, and that the two new statics
   (`s_autoConnectContinueNoToggled/ToggleFrame`) reset in
   `AutoConnectTransition`.
4. `test_harness_launcher.cpp` printf label edits changed no argument
   lists (labels only) — format-string/arg mismatch risk is nil, but the
   tool target should still be compiled once.
5. `analyze_stat.py` is not in the build; run it against any real field
   log if the STAT format is ever suspected to have drifted — the regex is
   intentionally strict.

---

## REBUILD COMPLETION SUMMARY (M0-M8, 2026-08-17)

All eight milestones of `RE07_MASTER_REBUILD_PLAN.md` are **code-complete**
on branch `re0.7`. What exists now:

- **Backend:** `transport2` (ENet worker, protocol-silence metric, INV-14)
  -> `session2` (5-step nonce handshake, teardown funnel, preserved
  `Session_*` facade) -> `packet_router` (single dispatch owner, both
  regimes) -> `match_setup` (host-minted epochs, EpochAlign, recovery
  ladder, preserved `PregameSync_*`) -> `match_director` (INV-9 match-end
  ladder, engine suspend/rotate across matches, preserved `OnlineWiring_*`)
  -> **`engine2`** (custom rollback: capture-once, delay-as-relabel,
  hold-last prediction, earliest-mismatch correction, confirmed-frame seam,
  SyncHash, fail-closed terminals; sole R reader = the INV-4 comparison) ->
  `FrameScheduler` (absolute-deadline pacing, limiter detour, pinned tick,
  one-sided pace slew, CadenceDebt, frozen STAT instrument).
- **Survivors intact (G6):** frontend/menu, frontend lockstep under
  `(epoch, phase_id)` identity, continue_flow rematch (fast path + any-NO),
  palettes, spectator (confirmed-only + elastic pacing + hash verify),
  replay (confirmed pipeline + AS2RCFM1 chapters + playback verification),
  training, NAT stack, supervisor (+ProgressDeadline) + barriers +
  kill-path CI gate.
- **Deleted:** GekkoNet from the default config (fallback compiles behind
  `AS2_WITH_GEKKO=ON`), netplay_pacing controller/classifier/debt,
  network_thread, session_manager/pregame_sync/match_bootstrap/
  online_wiring internals, gameplay_bridge, frame_lineage, delay
  negotiation, the serial allocator, legacy Hello/HelloAck.
- **Wire:** protocol v20 (v18/19 refused at handshake with named reason).
- **Verification in-tree:** unit suites T-ENG-1..12 (+wrap/epoch/cadence),
  T-SCHED-1..4, T-TB-1..7 (+T-LADDER barrier half), frontend identity +
  interrogation tests, 100k-frame socket-free lossy soak, microbench,
  kill-path gate; field-side: the upgraded rematch soak (epoch/INV-15
  asserts, NO-cycle driving), the STAT analyzer, and the M8 acceptance
  runbook.

**What remains is not code:** execute `docs/re0.7/M8_ACCEPTANCE_RUNBOOK.md`
on real builds (the first actual compile of the tree included — builds were
forbidden throughout; every milestone entry carries its compile-risk list,
M0 first). Ship/merge decision = runbook §3 checklist; GekkoNet removal =
runbook §6; fallback to `0.7` = runbook §7. The product bar remains plan
§1 G1: a 155 ms / 0% loss link at a flat 60.00 fps with zero holds —
judged by `python tools/analyze_stat.py --profile le1`.

---

## 2026-08-17 — Post-M8: GekkoNet removal + determinism suite (one session)

The M8-7 field-gate deferral was superseded by an explicit user
instruction (2026-08-17, during the rebuild session): "Verify that the
plan is fully done (Gekko deprecation as well, it's not needed
anymore)". On that instruction the runbook §6 one-commit removal task
list is EXECUTED, and the offline determinism suite (R-DET class)
lands alongside it.

### R-1: GekkoNet removal (runbook §6 task list, one commit)

- **Deleted files:** `src/rollback/rollback_session.cpp` (the Gekko
  adapter), `tests/gekko_input_tests.cpp`, `temp_session.cpp` (flagged-dead
  untracked root file), and the three dead legacy test sources the disabled
  `standalone_rollback_tests` block still referenced
  (`tests/standalone_rollback_tests.cpp`, `tests/test_runner_main.cpp`,
  `tests/test_packet_codec.cpp` — their support code moved to old_files/
  pre-M0; the commented-out CMake block is gone too).
- **CMakeLists.txt:** `AS2_WITH_GEKKO` option deleted; `GEKKONET_DIR` cache
  var deleted; GekkoNet subdirectory/link/define blocks deleted; the
  adapter selection is unconditional (`rollback_session_engine2.cpp` is the
  sole facade provider); `gekko_input_tests` target deleted; dependency
  list + status messages swept.
- **`#if AS2_WITH_GEKKO` sweep:** `src/testing/rematch_soak.cpp` keeps only
  the engine2 strict INV-15 branch (whole-session canonical monotonicity;
  the per-match relaxation is gone). No other TU had live branches.
- **API renames (grep-clean requirement):** the dead-after-removal sidecar
  hooks `RollbackAudio_OnGekkoLoad/OnGekkoBatchEnd`,
  `RollbackComboFx_OnGekkoSave/OnGekkoLoad/OnGekkoBatchEnd`,
  `RollbackStatusFx_OnGekkoLoad/OnGekkoBatchEnd` → `*_OnEngine*` (their
  only callers lived in the deleted Gekko adapter; definitions retained as
  dormant API for future engine2 wiring).
- **Gekko log channel deleted:** `LogWindow_LogGekko/LogGekkoV`, the
  `LOG_GEKKO_*` macros, and the `as2_gekko_<pid>.log` file plumbing removed
  from `ui/log_window.{h,cpp}` (zero live callers remained).
- **Comment/doc-string sweep (engine2-only wording, no Gekko mentions in
  compiled code):** `rollback_session.h` (facade header rewritten),
  `rollback_session_engine2.cpp`, `packet_router.{h,cpp}`, `protocol.h`,
  `online_wiring.h`, `spectator_protocol.h`, `spectator_runtime.h`,
  `transition_barrier.h`, `replay_runtime.{h,cpp}`, `churn_pause.h`,
  `input_timeline.h`, `prediction.h`, `desync_dump.h`,
  `rollback_debug.{h,cpp}`, `resimulation.cpp`, `connection_supervisor.cpp`,
  `input_override.cpp`, `mod_main.cpp`, `harness_shared_memory.h`,
  `rematch_soak.h`.
- **Kept by explicit allowance:** `tools/test_harness_launcher.cpp` still
  greps `"GekkoNet session started"` and `as2_gekko` log filenames — those
  read OLD field logs (triage tool), comment updated to say so.
- **`lib/GekkoNet/` retained on disk** (history only, unwired);
  `AS2_PATCHES.md` carries the retirement note. `M8_ACCEPTANCE_RUNBOOK.md`
  §6 marked EXECUTED (override noted), §1.1 fallback-build wording updated.
- **Post-condition:** `grep -r "Gekko" src/ include/ tests/ CMakeLists.txt`
  → zero hits; `tools/` hits are the old-log triage strings only.

### R-2: Forced-rollback mode (engine2 + stress hooks)

- **Files:** `include/rollback/engine2.h`, `src/rollback/engine2.cpp`,
  `include/rollback/stress_hooks.h`, `src/rollback/stress_hooks.cpp`,
  `src/rollback/rollback_session_engine2.cpp`.
- **Done:** `RollbackEngine::SetForcedRollback(depth)` — the offline analog
  of QOH99's selftest (save → tick → restore → replay K → compare): while
  depth > 0, every pass whose sim frontier advanced synthesizes a depth-N
  correction through the normal BeginRollback contract (a synthetic
  pending-mismatch), clamped to the epoch frame origin (§2.6.5), yielding
  to real mismatches (earliest-frame rule), one transaction per advanced
  frontier (marker-gated). Deterministic: no clock, no RNG (INV-16); depth
  0 = production default. Exposed live via
  `StressHooks_Set/GetForcedRollbackDepth` (0..15, snapshot field added);
  the adapter applies it wherever the prediction tap was applied (new
  `ApplyStressHooks()` at arm/rotate/poll; End clears to 0).

### R-3: Determinism suite (`tests/determinism_tests.cpp`, new CTest target)

- **Files:** `tests/determinism_tests.cpp` (new), `CMakeLists.txt`
  (`determinism_tests` target: engine2.cpp + the test TU, `add_test` wired).
- **Harness:** two full peers in the engine2_tests soak style (socket-free,
  clock-free, seeded xorshift only); link extended with duplication;
  per-epoch state-blob generator (fresh Block64 seed + epoch-salted tick =
  different characters/config per match); 3 epochs per cell via
  `RotateEpoch` at fully-confirmed boundaries (battle → park-at-boundary →
  align → rotate, the EpochAlign analog).
- **Matrix:** 17 cells — stable-low (~20 ms), stable-high (~150 ms),
  jittery-low (20±15), jittery-high (150±60), loss 3%/10%, loss+jitter,
  reorder+duplication; each profile at budgets 8 AND 12;
  forced-rollback-every-frame in 9/17 cells (depths 1/2/3/8, incl. depth-8
  under budget 12).
- **Assertions per cell:** zero terminals; SyncHash chains identical (and
  provably exercised); per-frame confirmed streams byte-identical
  (frame/epoch/inputs/pre-hash); canonical counter monotonic + epochs
  non-decreasing across all rotations (INV-15); forced cells prove one
  transaction per advanced frame at the requested max depth; full-speed
  invariant — `DeriveHoldBound` derives the PredictionLimit-hold budget
  from (D + R) vs worst transit: ZERO asserted in the stable cells,
  loss-proportional bounds elsewhere. Plus `TestForcedRollbackUnit` pins:
  once-per-frame gating, real-mismatch precedence, epoch-origin clamp,
  depth-0 disable.

### R-4: Frontend determinism (`tests/frontend_sync_tests.cpp` extension)

- **Files:** `tests/frontend_sync_tests.cpp`, `src/net/continue_flow.cpp`,
  `include/net/continue_flow.h`, `CMakeLists.txt` (frontend_sync_tests now
  links the real `continue_flow.cpp`).
- **Simultaneous navigation:** charsel + stagesel + winscreen phases, two
  peers streaming interleaved nav inputs concurrently over a jittery/
  reordering/duplicating simulated link (existing transport stubs record
  the outbound stream). Asserts: consumed remote stream byte-identical to
  the peer script; outbound redundant windows internally consistent and
  byte-identical to the consumed local stream (= the peer's consumed view);
  zero ResyncRequest / zero interrogation cycles in the clean cells.
- **Rematch handoff cycles:** the REAL continue_flow state machine driven
  at >= 3 winscreen → EpochAlign → next-phase handoffs with mixed YES/NO
  outcomes, fed exclusively through `WinScreenSync_ConsumeCurrentFrame` →
  `ContinueFlow_OnConsumedFrame`. Pins: entry carry gate (held confirm
  never locks), YES fast path (latch, sub-5 route, BeginRematch once,
  WinScreenExit before PostMatchDecision(Rematch), latch consumes once),
  NO route (cursor toggle → LockedNo, sub-8 route, A/C carry gate,
  finalize hold, PostMatchDecision(CharselRestart), abort-on-mode-exit
  proposes WinScreenExit), epoch rebind adoption every cycle.
- **Test seam:** continue_flow.cpp gained the `AS2_FRONTEND_SYNC_TESTING`
  game-memory shim (mode/sub/timers/cursor in shim variables; BGM stop
  swallowed) — same pattern as frontend_input_sync's test clock; live
  builds compile the direct-address branch unchanged.
  `ContinueFlow_Test_SetGameState/GetSubState/GetCursor` added under the
  same guard. The test TU's ContinueFlow stubs were replaced by
  director-side stubs (locked config, BeginRematch, MatchLifecycle
  owned/rematch, PlayerMapping slot, OnlineWiring intent hooks, recording
  TransitionBarrier_Propose).

### Deferred obligations

- **None code-side.** Field-only: rerun the runbook §2 acceptance matrix on
  the post-removal build (the removal is compile-clean by construction but
  M8's rule stands — no builds were run this session); R-DET now has an
  offline twin (`determinism_tests`) that CI runs unconditionally.

### Compile risks to check first (this session)

1. `frontend_sync_tests` now links `continue_flow.cpp`: verify the
   `AS2_FRONTEND_SYNC_TESTING` shim covers every game-memory touch (a
   missed `WriteMemory`/`GetGameMode` in that TU would AV the test, not
   fail to compile) and that no stub/definition duplicate symbols remain.
2. `determinism_tests.cpp`: MSVC Win32 — check the `CELL_CHECK` printf
   format args (`%zu` on size_t) and the aggregate `CellSpec` table braces.
3. The forced-rollback insert in `RollbackEngine::NextAction` runs before
   the stall checks — re-run the FULL engine2_tests suite to prove depth-0
   behavior is byte-identical (all existing tests must stay green).
4. `stress_hooks.h` snapshot gained `forced_rollback_depth` — any
   out-of-tree initializer lists of `StressHooksSnapshot` (mod menu panel)
   compile-check.
5. Deleted `LogWindow_LogGekko`: grep proved zero live callers, but the
   mod-menu/log-window ImGui panel should be compiled to confirm no
   stragglers behind macros.

## 2026-08-17 — Post-M8: desync divergence diagnostics (audit + upgrade)

### Question answered

**Could both sides localize a ConfirmedDesync from what they recorded?**
No. Audit findings:

- The detector's dump (`ReportEngineTerminal` → `DesyncDump_TryDump`) carried
  the peer's failing SyncHash values only inside the 160-char terminal-detail
  string (`remote_crc` was passed as 0); no structured local-vs-peer pair.
- No record existed of the confirmed-frame hash chain the SyncHash exchange
  actually compares: `DesyncDump_StoreChecksum` was a no-op stub, the dump's
  "Checksum History" is rollback_debug's live-frame CRC32 (a different
  digest), and its "Input History" is the game's raw ring — indices, not
  canonical confirmed frames, not guaranteed final values.
- The engine's cadence ring held only every 30th confirmed frame and had no
  external accessor.
- **The surviving side dumped nothing**: the `Disconnect` goodbye carries
  `reason_id` (fnv1a32 of the reason name) + human text, but the receive
  path in session2.cpp went straight to `SetError` — one-sided evidence,
  no way to diff.
- The per-region CRC breakdown existed only in human format, duplicated
  inline, with no machine counterpart to diff across peers.

### What was added (diagnostics only; capture membership untouched; wire pins intact — zero new/changed packets)

- **`include/rollback/desync_diag.h` + `src/rollback/desync_diag.cpp`** (new,
  pure: no Win32/game memory/clock): `DesyncDiagRing` (64 confirmed frames of
  `{frame, epoch, gameplay_hash, rng, hp0, hp1, p1_input, p2_input}` — spans
  two SyncHash cadence intervals), `DesyncEvidence` (the failing pair, all
  fields both sides), `DesyncEvidence_FirstDivergentField` (hash > rng >
  hp0 > hp1, mirroring `ReceiveSyncHash` comparison order), and deterministic
  `RING`/`EVIDENCE`/`FIRSTDIVERGENT` line formatters (timestamps live only in
  dump file headers).
- **engine2** (additive, still clock-free): `ReceiveSyncHash` captures the
  full failing pair into `desync_evidence_` when setting the ConfirmedDesync
  terminal; `GetDesyncEvidence()` accessor; cleared at `Arm`.
- **Adapter** (`rollback_session_engine2.cpp`): `s_diagRing` fed from the
  confirm seam in `DrainConfirmSeam` (always on during netplay, reset on
  Begin/rotation). ConfirmedDesync path now logs the localization line
  (first divergent field + frame) and writes
  `DesyncDump_TryDumpWithDiagnostics` (machine section first: EVIDENCE +
  FIRSTDIVERGENT + 64-entry RING + per-region REGION CRCs, then the full
  human dump). New `RollbackSession_NotifyPeerDesyncGoodbye()` dumps the
  surviving side's ring + regions.
- **session2.cpp**: fnv1a32 helper factored out of `Session2_Terminate`; the
  `Disconnect`-receive path calls the goodbye hook when
  `reason_id == fnv1a32("ConfirmedDesync")` — bounded single file write
  before the unchanged teardown (no new kill path).
- **desync_dump**: region table factored into one shared
  `DesyncDump_GetRegionTable` (11 regions, token names) used by both the
  human breakdown and the new machine `REGION` writer.
- **`tools/compare_desync_dumps.py`** (new): takes both sides' dumps, prints
  first divergent confirmed frame, divergent fields, region-CRC diff table,
  and the ±8-frame input context (flags canonical-input disagreement, which
  would mean the input stream itself broke).
- **engine2_tests**: `TestDesyncDiagnostics()` pins ring capacity/order/wrap,
  both line formats byte-exact, first-divergent priority, engine evidence
  capture on a real mismatch terminal, and Arm-clears-evidence.
- **M8_ACCEPTANCE_RUNBOOK.md**: R-DET row + D-1..D-5 row now reference the
  two-sided dumps and the comparator.

### Files

- new: `include/rollback/desync_diag.h`, `src/rollback/desync_diag.cpp`,
  `tools/compare_desync_dumps.py`
- modified: `include/rollback/engine2.h`, `src/rollback/engine2.cpp`,
  `include/rollback/desync_dump.h`, `src/rollback/desync_dump.cpp`,
  `src/rollback/rollback_session_engine2.cpp`,
  `include/rollback/rollback_session.h`, `src/net/session2.cpp`,
  `tests/engine2_tests.cpp`, `CMakeLists.txt`,
  `docs/re0.7/M8_ACCEPTANCE_RUNBOOK.md`

### Verification

Release build clean. Suites all green: frontend_sync 495/0,
frame_arithmetic 31/0, frame_scheduler 44/0, engine2 293/0 (includes the new
TestDesyncDiagnostics pins), transition_barrier PASS, determinism 608/0.
Comparator smoke-tested against synthetic two-sided dumps (divergence at a
known frame localized correctly; region diff and input context render).

## 2026-08-17 — Post-M8: SAVESTATE_AUDIT fix pass (F1–F6 + doc nits)

Implements the full prioritized fix list from `SAVESTATE_AUDIT.md` (the
mode-8 write-set vs GameSnapshot coverage audit). All six findings closed;
mechanisms below, `FIXED:` blocks inline in the audit doc, verified
constraints recorded in its new §8.

### F1 (P0) — AI-learning statics 0x76C5D8–0x76C5E7 / byte_8E940D

- **Session guard** (`match_director.cpp` `AiLearnGuard_Force/Restore`):
  `byte_8E940D` (AI_PatternModeEnabled) saved + forced to 0 at the startup
  handoff, restored only on SESSION teardown (every path that reaches
  `RollbackSession_End`: StopRollbackSession(teardown), disconnect,
  shutdown). **Home rationale**: NOT `RollbackSession_Begin` — the
  deterministic intro runs sim ticks (and therefore `AI_RecordPattern`,
  which runs for human players twice per tick) BEFORE the engine arms;
  `PrepareBaselineForInteractiveRelease` is the last director-owned point
  before any mod-owned sim tick. Staying forced across match boundaries
  kills the rematch substate-0 learning-block load at its single gate. The
  guard also zeroes the four statics AFTER the baseline restore so both
  peers hash identical values from frame 0 (each peer otherwise carries
  stale statics from its own offline matches → first-frame false desync).
- **Belt-and-braces capture** (offline forced-rollback determinism runs
  exercise savestates too): new `GameSnapshot.ai_learn[16]` SIM region
  (`ADDR_AI_LEARN_STATICS`), captured/restored/hashed. A dedicated region,
  not a `pre_match_gap` extension — that region is digest-excluded RENDER,
  and these bytes gate rand() so INV-22 makes them SIM.

### F2 (P0) — render-phase rand() in sub_4C47C0 (super backgrounds)

Two independent violations, two mechanisms:

- **RNG stream corruption → render-phase RNG isolation.** Reasoning against
  the DECOMP_TIMING_STUDY loop structure: `Frame_AdvanceSimulation` (already
  hooked, `Hook_AdvanceFrame`) runs exactly once per outer pass, after the
  sim while-loop and immediately before the `*(match+11)==1` render gate;
  the next sim-side code to execute is the next pass's first
  `Input_TryGetNextFrame` dispatch (all sim work lives inside the
  while-loop). So: capture the CRT seed at `Hook_AdvanceFrame` (post-sim
  value; also on 0-sim hold passes, which still render), restore it at the
  next dispatcher entry (`InputSyncHooks_RestoreRenderRngIfPending`, called
  at the top of `Hook_InputDispatcher`). Every rand() consumed between the
  two points — all 15 sub_4C47C0 sites, present, frontend — is erased from
  the sim stream. **Proof of the invariant the audit demands** (rolled-back
  peer vs straight-through peer): the sim RNG stream becomes exactly "as if
  render never called rand()" on BOTH peers, for ANY render:sim cadence
  (N-frame resim → 1 render vs N renders) and independent of the local
  background-off option, because the erased window covers the entire
  non-sim portion of every pass. Equivalent to the audit's private cosmetic
  PRNG (renderer consumes throwaway continuations of the sim stream) with
  no new PRNG, no sub_4C47C0 hook, no binary patch. Netplay-gated
  (`RollbackSession_IsActive() && MODE_MATCH`); the session-inactive path
  drops a stale capture instead of rewinding the offline stream.
- **Render-cadence writes into hashed entity scratch → digest masks.**
  entity+1244..+1851 (gate dword +1244 through last mutated field +1850,
  rounded to the dword boundary), both entities, masked from the digest.

### F3 (P1) — Frame_Display (0x816494)

New `GameSnapshot.frame_display` (SIM): captured, restored (resim
re-increments from the rewound value instead of drifting by rollback depth),
hashed in the SimHeader of both digest paths. Naming trap vs 0x81635C
annotated at `ADDR_FRAME_COUNTER`/`ADDR_FRAME_DISPLAY` in as2_constants.h.

### F4/F5 (P1/P2) — voice bookkeeping + tint timers: digest masks

`kMainDigestMasks` (game_snapshot.cpp): per entity, entity+440..+447 (tint
state/timer, F5), entity+1244..+1851 (F2), entity+107084..+107095 (voice
bookkeeping, F4) — captured+restored (they live inside `main_state`), but
skipped by BOTH `GameSnapshot_HashGameplay` and
`GameSnapshot_HashGameplayLive` through the shared `HashMainMasked` walker
(Block64's tail fold makes segmentation part of the value, so both paths
MUST use the same table — pinned by a constexpr sorted/disjoint/in-bounds
static_assert). Follows the existing display_frame captured-but-not-hashed
pattern. F5's possible sim read-back of +440 stays an open audit item
(audit §8 residual).

### F6 (P2) — Match_UpdateScoreStats double-apply on resim

Hooked 0x55BCD0 (`Hook_MatchUpdateScoreStats`, input_sync_hooks.cpp,
MinHook entry alongside the vanilla-netplay suppression hooks) — skipped
while `RollbackSession_IsRollingBack()`: the dispatcher/adapter already
exposes rollback-vs-normal advance (`s_rollingBack` holds through the game's
execution of each replay tick), so the speculative first pass applies the
`+=` once and the resim replay of the same commit tick is dropped. Residuals
accepted per the audit: outcome-changing mispredictions inside the replay
window leave superseded stats applied (persistence noise, no sim reader);
offline manual F6-load replays can still re-apply.

### Doc nits (P3)

M4_AUDITS main_state size "~243 KB" → 0x3F288 = 259,720 B (~253.6 KB), plus
mask/f3/f1 rows and the rng_seed cosmetic-consumer correction;
SAVESTATE_AUDIT gained per-finding FIXED blocks, a Status column in §7, and
§8 "verified constraints" (Block-table adjacency warning: any main-region
end extension past 0x7AB880 would swallow the live AI-block heap pointers
at 0x7AB762–0x7AC160; the still-owed sub_4AB0F0 one-off disassembly check).

### Engine cleanliness

engine2 untouched — stays socket/clock/game-memory-free. All new game-memory
and wall-clock-adjacent logic lives in the adapter ring (director, patches,
game_snapshot). GameSnapshot grew 20 B (frame_display + ai_learn); layout
pinned by a new compositional static_assert in game_snapshot.h (68-byte
scalar prefix + four regions). The engine2 microbench sizes itself off
sizeof(GameSnapshot) and needed no change.

### Files

- modified: `include/core/as2_constants.h`,
  `include/rollback/game_snapshot.h`, `src/rollback/game_snapshot.cpp`,
  `src/rollback/match_director.cpp`, `include/patches/input_sync_hooks.h`,
  `src/patches/input_sync_hooks.cpp`, `src/patches/input_override.cpp`,
  `include/rollback/savestate.h`, `docs/re0.7/SAVESTATE_AUDIT.md`,
  `docs/re0.7/M4_AUDITS.md`

### Verification

Release build clean. Suites all green: frontend_sync 495/0,
frame_arithmetic 31/0, frame_scheduler 44/0, engine2 293/0 (microbench
snapshot=253 KB, save p99 4 µs / restore p99 3 µs / hash p99 99 µs),
transition_barrier PASS, determinism 608/0 (includes the forced-rollback
and multi-epoch savestate-path cells).
