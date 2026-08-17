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
