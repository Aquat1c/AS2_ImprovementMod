# QOH99 netplay backend — architecture study (porting template for AS2 0.7)

**Source:** `D:\dev\alice_senki\references\qoh99_netplay` (same tree as `D:\dev\qoh_99\qoh99_netplay`), read 2026-08-17.
All `file:line` references below are into that tree at this checkout. The project's own warning applies
(`docs/plan/ROLLBACK_AS_BUILT.md:6-7`): GameHooks.cpp line numbers drift under active work — re-anchor by symbol
if the reference tree moves.

**Reading order in their docs** (current vs historical matters — several docs describe superseded designs):

| Doc | Status |
|---|---|
| `docs/plan/ROLLBACK_AS_BUILT.md` | **The** current-model document. Read first. |
| `docs/plan/ROLLBACK_REVAMP_PLAN.md` | The target plan ("Rev2/Rev3 architecture" the code cites by §). |
| `docs/design/done/ROLLBACK_SCHEDULER_DESIGN.md` | Scheduler/debt contract, reconciled corrections list. |
| `docs/design/done/FULL_SPEED_ROLLBACK_IMPLEMENTATION_PROMPT.md` | Authoritative scheduler spec. |
| `docs/design/done/INPUT_SYSTEM_DESIGN.md` | Input workers + committed session-event timeline. |
| `docs/design/done/ROLLBACK_AUDIO_DESIGN.md` | Audio journaling/reconciliation under rollback. |
| `docs/status/NETPLAY_IMPLEMENTATION_STATUS_20260727.md` | Production-stack survey (transport, spectator, hardening). |
| `NETPLAY_DESIGN.md` | **Historical** (CCCaster mapping, fake-DirectPlay plan that was abandoned — its §4.5 "reuse vanilla netplay engine" was NOT built; the shipped system drives the game in *local* mode). |

---

## 0. System shape in one page

Two processes per player: a console **caster** (`qoh99caster.exe`, `src/client/`) that does discovery/handshake/
launch/injection, and the injected **game DLL** (`qoh99_netplay.dll`, `src/GameHooks.cpp` + libraries) that owns
everything after GO. The game itself stays in its **local 2P mode** (`doNetplay` fails closed if the native
netplay flag is set, GameHooks.cpp:18866-18871); the DLL detours the scene frame dispatchers and injects both
players' canonical inputs every tick. There is **no ENet and no GGPO/GekkoNet** — the whole stack is bespoke:

- one UDP socket per link; **unreliable redundant-window input stream** + per-message-type
  **latest-wins/resend-until-acked reliability** inside `NetplayCore` (no generic reliable layer on the player link);
- a **socket-free rollback core** (`RollbackSession`) + a **socket-free wire state machine** (`NetplayCore`),
  both deterministic and unit/soak-tested off the game;
- a single **canonical tick executor** (`netRunOwnedTick`) shared by players, corrections, spectators, replay;
- the DLL is the **single pacing owner**: game limiter neutered, ddraw-wrapper limiter handed off, one
  absolute-deadline 60.60 Hz presentation clock;
- **one session = one canonical frame timeline** across any number of matches; match/character boundaries rotate
  an **epoch** instead of re-bootstrapping anything.

---

## 1. Transport

### 1.1 Socket layer

- `src/net/Sockets.h` / `.cpp`: minimal winsock wrappers. `UdpSock` (non-blocking, `SO_EXCLUSIVEADDRUSE`,
  `adopt()`/`release()` for cross-process socket handover, `drain()` for pre-launch hygiene — Sockets.h:135-170).
  `TcpConn`/`TcpListener` with `[u16 len]`-framed messages for the caster control path (Sockets.h:172-238).
  IPv4-only by policy, one policy function (`resolveFamilySupported`, Sockets.h:41-118). Monotonic µs clock
  `nowUs()` (QPC) at Sockets.h:241.
- The caster establishes the sockets, then hands the **proven** UDP data socket to the game process via
  `WSADuplicateSocket` over IPC (CCCaster model; NETPLAY_DESIGN.md:28-39, `UdpSock::adopt` Sockets.h:147).
  From then on an independent **UDP pump thread** in the DLL owns it; the game thread only drains queues.

### 1.2 Two protocols, two magics

- **Caster control protocol** `'Q99N'`, version 18: `src/net/Protocol.h:1-80` — version/identity/tuning exchange,
  route candidates, route commit, runtime barriers, spectate intents. Originally TCP; since v10 it runs over the
  **UDP data socket** through `src/net/ControlLink.h` (TcpControlLink preferred, UdpControlLink fallback carried by
  `net/Reliable.h` after hole-punch — ControlLink.h:1-58). Rationale: TCP cannot survive an unforwarded NAT, and a
  punched hole is a property of the exact 5-tuple the match will use, so control-on-data-socket *is* the path proof.
- **In-game player protocol** `'QNP5'`, version 14: `src/netplay/WireProtocol.h:19-54`. Explicit little-endian
  serialization, no struct memcpy, no sockaddr on the wire (WireProtocol.h:2-6). Every version bump is fail-closed
  at Hello (mixed builds cannot join).

### 1.3 Packet format (player link)

`WireProtocol.h:287-300`: 44-byte header = `{magic, version, type, flags, payloadLength, sessionId(u64),
SessionCookie(128-bit), sequence, crc32}`. Datagrams ≤1200 bytes (`kMaxDatagramSize`, WireProtocol.h:62).
Decode validation order is fixed and documented: size → magic → version → type/length/flags → **session** → CRC
(WireProtocol.h:416-423). Every packet is bound to session id + cookie + the exact route-committed peer endpoint;
`NetplayCore::onDatagram` rejects wrong-source datagrams (`IngressResult::WrongSource`, NetplayCore.h:59-86).

Message set (`WireProtocol.h:83-98`): Hello/HelloAck, Ping/Pong, **Inputs**, Ready, SyncHash/SyncHashAck,
Disconnect/DisconnectAck, RouteProbe/RouteProbeAck, SessionEvent, RematchChoice.

### 1.4 Reliability model — three tiers, no generic reliable transport on the player link

1. **Inputs: unreliable + redundant window.** Every Inputs packet carries up to 30 frames
   (`kInputWindowSize`, WireProtocol.h:56) as `{newestFrame, validMask, inputs[30], ackThrough, simFrontier,
   PressureReport}` (`InputsPayload`, WireProtocol.h:186-193). The window is anchored at `peerAckThrough + 1`
   (NetplayCore.cpp:1173-1181) — i.e. it retransmits **exactly the un-acked suffix**, which the as-built doc calls
   strictly better than a fixed window: a hole is refilled without any NACK message
   (ROLLBACK_AS_BUILT.md:349-355). Loss needs no retransmit; the next packet re-covers. Sent when dirty, plus a
   50 ms resend while unacked (`inputResendMs`, NetplayCore.h:49; resend condition NetplayCore.cpp:1160-1163).
2. **Per-type "Ready-style" reliability inside NetplayCore.** Ready, SyncHash (+acks), SessionEvent, Disconnect
   each have one outbound slot retransmitted on its own cadence until acknowledged/superseded
   (`readyResendMs`/`hashResendMs`/`disconnectResendMs` = 100 ms, NetplayCore.h:50-53; session-event slot
   NetplayCore.h:335-340). Idempotent under duplication/reordering by design (SessionEvent ladder comment,
   WireProtocol.h:319-333). RematchChoice is deliberately unreliable latest-wins; the *commit* travels on the
   reliable SessionEvent ladder (WireProtocol.h:353-370).
3. **Go-Back-N channel (`src/net/Reliable.h`)** — CCCaster's GoBackN analog (50 ms resend, in-order-only receiver,
   keep-alive 1 s / timeout 10 s, Reliable.h:29-31) — is used by the **caster control link and relay/spectator
   paths**, not by the in-game player link.

**Send priority is explicit:** established-session `pollOutbound` emits **player input first**, before pong, ping,
session events, rematch, ready, hash-ack drain, hash (NetplayCore.cpp:1147-1160; Rev2 §6.7; landed 2026-08-05,
ROLLBACK_AS_BUILT.md:170).

### 1.5 Channels/messages worth copying verbatim

- **`PressureReport`** appended to every Inputs packet (QNP13; WireProtocol.h:144-178):
  `{inputProducedThrough, confirmedFrontier, predictionDepth, correctionDebt, runState, holdGeneration}`.
  Contractually **advisory** — may steer only peer-local wall-clock pacing, never simulation
  (NetplayCore.h:151-156). Exists to distinguish "peer stopped producing" from "peer's packets aren't arriving":
  `inFlight = peerProducedThrough - ourContiguousPrefix` (ROLLBACK_AS_BUILT.md:233-239).
- **`RouteProbe`/`RouteProbeAck`** (WireProtocol.h:124-142): authenticated RTT probe over the *selected* route,
  echoing an opaque monotonic µs stamp; the ack carries **responder dwell µs** so the prober subtracts service
  time. Generation-gated so a stale reply can't repopulate a reset estimator. Feeds `net/RouteRttEstimator.h`
  (dwell-corrected rolling p95, local-stall rejection). Lesson encoded here: **never derive tuning from the TCP
  lobby RTT or a ms-quantized clock** (FrontendDelayPolicy.h:20-25, ROLLBACK_SCHEDULER_DESIGN.md:15-22).
- **`SyncHashTag {layoutGeneration, epoch, frame}`** (WireProtocol.h:220-233) — every state proof is
  epoch-qualified; a reserved tag `kLayoutFingerprintFrame = 0xFFFFFFFE` implements the reliable **post-load layout
  rendezvous** that precedes frame zero of each epoch (WireProtocol.h:58-61).
- **`DisconnectPayload`** carries code + detail + a bounded 128-byte human reason + stable `reasonId` hash
  (WireProtocol.h:253-285) — terminal failures are self-describing on the wire.

### 1.6 Keepalive & liveness

- Ping/Pong every 1 s (`pingIntervalMs`, NetplayCore.h:53) → coarse RTT; the *good* RTT comes from RouteProbe.
- Any valid datagram refreshes liveness; timeout 10 s (`livenessTimeoutMs`, NetplayCore.h:54), evaluated in
  `pollOutbound` against `_lastValidReceiveMs`, with the pre-establishment budget anchored to the **first pump
  poll**, not process time zero (NetplayCore.h:278-283, NetplayCore.cpp:1050-1068) — closes a false-instant-timeout
  bug.
- Critical ordering fix: the game thread **drains accepted packets before judging liveness**
  (doNetplay, GameHooks.cpp:18996-19027) — a blocked game thread must not time out a peer that has been talking
  to its pump the whole time. Peer-sent Terminal still outranks everything.

### 1.7 Connection lifecycle (player link)

1. Caster phase: control handshake (version/identity/tuning/route candidates), route selection (below), both
   launchers start the game suspended, inject the DLL, exchange runtime barriers, resume (Protocol.h:22-27).
2. DLL phase: client sends `Hello` every 200 ms (`helloResendMs`); host validates
   compat-identity + learns the client's UDP port if configured (`learnPeerPort`, NetplayCore.h:42-46), replies
   `HelloAck` → `_established` (NetplayCore.cpp:1110-1141).
3. `Ready` exchange (reliable, repeated until mutually acked — `kReadyAcknowledgesPeer` flag, WireProtocol.h:63):
   carries RNG seed triple, `bootstrapHash` (deterministic frontend fingerprint), and the four match-rule words
   (WireProtocol.h:206-218). P1's tuple is applied on both peers (doNetplay GO block, GameHooks.cpp:18948-18958).
4. GO → canonical frame 0. Session runs until a terminal: Disconnect (repeated 100 ms until acked), liveness
   timeout (10 s), progress deadline (§3.6), desync, or fail-closed internal error. All terminals are sticky and
   published to the peer (`setLocalTerminalError`, NetplayCore.h:205-209).

### 1.8 NAT traversal / route selection (caster layer)

- **Autopunch** (`src/net/Autopunch.h`) — delthas' public relay protocol (the same one AS2 itself already uses!),
  4 packet shapes totaling 19 bytes, used only to learn external mappings; hardcoded public server address
  (Autopunch.h:1-50).
- **Route candidates**: private/LAN + observed public + hairpin labeling, raced with bounded validation, then a
  **two-peer route commit**; exactly one validated endpoint reaches the game process
  (`net/RouteCandidates.h`, `RouteSelection.h`; status doc `NETPLAY_IMPLEMENTATION_STATUS_20260727.md:200-215`).
- **Own relay** (`src/relay/`, `net/RelayProtocol.h`) as final fallback; rooms/tokens/lanes; direct tried 1.5 s
  first.

---

## 2. The rollback engine

### 2.1 Canonical frame counter

One `uint32` canonical frame, **half-open frontiers everywhere**, wrap-safe RFC-1982-style comparison
(`net/FrameArithmetic.h:15-44`: `frameAfter`, `forwardDistance`, `signedLead`; RollbackSession has identical
private copies, RollbackSession.h:409-414). **The frame counter never resets for the life of the session** —
not per match, not per round ("the global input frame never resets", status doc:137). There is no CCCaster
`IndexedFrame {index, frame}`; the epoch (§4.2) qualifies *state identity*, not time.

### 2.2 The socket-free core: `RollbackSession` (`src/netplay/RollbackSession.h/.cpp`, 434+999 lines)

Owns *no* game memory, rendering, audio, or sockets (RollbackSession.h:2-8) — it decides which canonical inputs a
frame consumes and when the caller must stall or restore/replay. The game adapter and the deterministic soak
harness (`src/netplay/tests/soak/`) exercise the same policy. Config (`RollbackConfig`, RollbackSession.h:19-27):
`{localPlayer, inputDelayFrames, maxRollbackFrames, neutralInput, firstFrame, maxRemoteFutureFrames=120,
historyCapacity=4096}`.

**Input pipeline (capture → delay → exchange → confirm):**

1. **Capture** — `captureLocalInput(sourceFrame, value)` (RollbackSession.cpp:365): exactly one physical sample per
   visible source frame, keyed to the sim frontier; schedules the word at `sourceFrame + delay`; repeated calls
   while stalled adopt, never resample (`adoptExistingCapture`, .cpp:401). **Assigned slots are immutable, ever** —
   the single invariant everything else leans on.
2. **Delay** — pure relabeling; a neutral prefix of `delay` frames is queued at reset (RollbackSession.h:266-268).
3. **Exchange** — `popLocalSubmission` → `NetplayCore::submitLocalInput` → redundant-window Inputs
   (GameHooks.cpp `netFlushLocalSubmissions`:13406); receive: pump → `netDrainRemoteInputs`
   (GameHooks.cpp:13692-13721) → `receiveRemoteInput(frame, value)` (RollbackSession.cpp:468) with typed results —
   `Conflict` (different value for an already-actual frame) and `InvalidValue` (bits outside the canonical 0x0F0F
   fighter mask, RollbackSession.h:48-50) are **protocol violations that end the session**, never masked.
4. **Predict** — `predictRemote` (RollbackSession.cpp:552): hold-last-actual. A late actual that differs marks the
   **earliest mismatch** (`markMismatch`); tracked as `_pendingMismatch`.
5. **Confirm** — a frame is confirmed only when every preceding remote input is actual **and** any mismatch replay
   has completed (`ConfirmedFrame`, RollbackSession.h:100-107; `updateStableConfirmed`). Confirmed frames pop out
   with their `preStateHash` and feed hashing/spectator/replay (§2.5).

**Per-opportunity decision** — `nextAction()` (RollbackSession.cpp:644-707) returns `Stall(reason)` /
`Advance(frame, inputs)` / `Rollback(rollbackFrom, replayUntilExclusive)`. The prediction ceiling is a hard
capacity check: `speculativeFrames() >= maxRollbackFrames` ⇒ `Stall(PredictionLimit)` — never a 9th speculative
frame at r8 (NetplaySchedulePolicy.h:17-19).

**Rollback is an explicit transaction** (RollbackSession.h:289-306): `beginRollback` → caller restores the
pre-frame snapshot at `rollbackFrom` → `commitReplayFrame` per replayed frame (with re-hash) → `finishRollback`,
or the two boundary-truncation variants: `finishRollbackAtBoundary` (corrected replay crossed a native layout
boundary earlier than the speculative timeline did — discard the speculative suffix) and
`finishRollbackBeforeBoundary` (an *uncommitted* replay tick would cross a boundary without full actual prefix —
stop before it). No local capture is legal during the transaction.

**Rev2 §6.1 producer** — `produceLocalInputAhead(value, maxLeadFrames)` (RollbackSession.h:229-259, .cpp:205):
while the sim is stalled, keep sealing one input record per canonical frame period so the peer never starves
(breaks the bilateral stall cascade: we stall → we stop feeding → peer hits its ceiling → peer stalls → we stay
starved). Bounded by the *peer's* consumption capacity `peerRollback + peerDelay + 2`, hard cap 32
(`netProducerMaxLeadFrames`, GameHooks.cpp:1160-1168); one record per 16.5 ms wall period, never per 1 ms poll
(GameHooks.cpp:13654-13664); prefers a fresh device-worker sample so presses during a stall survive
(GameHooks.cpp:13666-13679). Fenced OFF during lifecycle barriers/rematch prompt/frontend
(`inputProductionPermitted`, NetplayLifecyclePolicy.h:130-140) because the rematch decision travels *in* the
input stream and immutability would silently swallow the press.

### 2.3 Savestates (`src/RollbackManager.h/.cpp`, `src/GameHooks.cpp`)

- **Ring**: `kMaxWindow = 30`, `kPoolSlots = 31`, modulo slot = `frame % 31` (GameHooks.cpp:129-130,
  netSavePreFrame:14148-14166). **Full-copy, pre-tick, every rollback-eligible gameplay frame**: save →
  simulate → commit. Save/load wall time is telemetered (`g_netSaveWorkUs`, 14150-14152).
- **Contents**: fixed region list from the RE model + pointer-child regions (fighter frame-data at `fighter+24`,
  command-history child at `fighter+164`, AI script heaps as external-owner children) — RollbackManager.h:1-9,
  519-539. Snapshot = concatenated fixed regions + per-child `[u32 present][bytes]`. Atomic: any unreadable
  region fails the whole save; rollback never continues from a partial snapshot (RollbackManager.h:406-409).
- **Tagging**: every slot tagged `{layoutGeneration, epoch, frame, phase}` (SlotTag, RollbackManager.h:423-437);
  a modulo slot from an older character/layout epoch can never be restored. FPU/MXCSR captured per slot alongside
  (`netCaptureFpu`/`netRestoreFpu`, GameHooks.cpp:14158/14173).
- **Restore**: `netRestorePreFrame` (GameHooks.cpp:14168-14181) = tagged load + FPU restore, fail-closed.
- **Hashing**: `hashLive`/`hashSlot` over the *same bytes* the savestate saves, profile-filtered
  (Gameplay vs RenderCosmetic vs All — RollbackManager.h:444-504). Hash primitive is versioned
  (`Fnv1a64` vs `Block64`; the FNV pass cost 13-19% of the 16.5 ms frame budget on this i386 target, the block
  digest is ~19× faster — RollbackManager.h:457-483). Process-local pointers are **canonicalized for hashing
  only** (never for save/restore): actor interior pointers → pool-relative `0xA5xxxxxx` identities, AI script
  pointers → `(bank, offset)`, loader descriptors → `{family, count, byteCount, contentHash}` tuples
  (RollbackManager.h:313-397). Peer-local config bytes stay raw in snapshots but are replaced by a canonical
  `QohMatchConfigProof` view in the cross-peer hash (RollbackManager.h:20-73, 208-249).
- **Depth control**: the rollback ceiling **R is peer-local capacity, not negotiated** (§6). The offline
  determinism selftest ramps its verify depth K 1→2→4→8→16→30 after 180 consecutive green frames
  (`kGrowAfter`, GameHooks.cpp:131, 22218-22222; env `QOH99_NP_K` pins) — that ramp is a *test* mechanism, not a
  live netplay control.

### 2.4 Correction (rollback execution) and render-loop integration

`netReplayCorrection` (GameHooks.cpp:17257-17462): restore tagged pre-state at `rollbackFrom` → re-run each frame
through the **same** owned-tick executor with `PresentationDisposition::SuppressHistorical` (simulate + frame Blt;
the ddraw wrapper elides output) → re-hash and `commitReplayFrame` each → finish. Audio runs inside a staged
correction transaction that commits a reconcile plan exactly once (`AudioCorrectionScope`, GameHooks.cpp:17183-17213).
Corrections run **first** on the opportunity that finds them queued; the driver then re-reads frontiers and may
still advance on the same opportunity (NetplaySchedulePolicy.h:122-127). Correction wall time accrues to
`g_netPacingCorrectionWorkUs` (GameHooks.cpp:19384-19391). A correction that cannot commit a boundary is a HOLD,
paced via `netPaceSemanticHold` + `Sleep(1)` (GameHooks.cpp:19399-19404). Discarded speculative timelines
invalidate their meta/slots/FPU entries (`netInvalidateDiscardedTimeline`, GameHooks.cpp:17215-17234).

**Presentation dispositions** (GameHooks.h; taxonomy at GameHooks.cpp:14289-14316):
`Normal` (visible, owns the pacing slot) / `SuppressHistorical` (rollback replay: not drawn) /
`HiddenNewForward` (debt-repayment catch-up: not drawn, native FPS accounting preserved, audio journaled like a
forward tick) / `CorrectedFinal` (ABI-retained) / `VisibleFastForward` (spectator backlog burn: **drawn** but
consumes no pacing slot). The wrapper queries disposition per Blt (`queryPresentationDisposition`,
GameHooks.cpp:22935) and calls back into the DLL's slot protocol.

### 2.5 The single executor — one canonical tick for every role

`netRunOwnedTick(orig, frame, inputs, replay, result, notifyPeer, presentation)` (GameHooks.cpp:14524-14720) is the
only way a native frame runs in any network mode. It:

- advances deterministic DirectBoot if that owns the frame (14529-14538);
- writes both players' canonical inputs into `g_netInject[2]` — the input-read hook serves both slots
  (14539-14542);
- steps the **canonical rematch prompt** and the winscreen carry gate as pure functions of (native state,
  canonical inputs) — so players, corrections, spectators, replay all reproduce them (14543-14583, §4.3);
- sets the audio disposition/journal transaction (14589-14631);
- engages the deterministic clock (`netClockBegin`, §3.2), marks tick ownership (`g_netGateOwnsTick`), bumps the
  owned-tick serial, and calls the original dispatcher under SEH with a crash snapshot (14639-14660);
- consumes the presentation slot post-hoc if a legal non-rendering tick emitted no Blt (14677-14687).

Players call it from `netAdvanceOneCanonicalFrame` (GameHooks.cpp:17619-17955: fences → pre-state save/hash →
owned tick → post-state hash → `commitAdvance` → confirmed publish → audio flush). Corrections call it with
`replay=true`. **Spectators call the identical function** from their catch-up loop (doSpectator,
GameHooks.cpp:20397-20399), consuming archived confirmed records instead of a live session. Replay playback does
the same via `game/replay/ReplayRuntime`. This is the "canonical tick" property the AS2 rebuild wants: *the
executor does not know which role is running it; only the input source differs.*

### 2.6 Confirmed-frame pipeline, sync hashing, desync detection

`netPublishAndVerifyConfirmed` (GameHooks.cpp:15494-15820) drains `popConfirmedFrame` and, per confirmed frame:

- validates stored meta (frame + preHash must match — internal fail-closed otherwise, 15523-15530);
- feeds the **replay recorder** (15534-15563) and the **spectator archive/sidecar** (15581-15692) from the same
  immutable seam — "a predicted value can never enter the file" (15565-15568);
- every **30th confirmed frame**, submits a full canonical pre-state proof
  `{layoutGeneration, epoch, frame, rngGameplay, rngCosmetic, rngSelector, hp0, hp1, gameplayHash}`
  (15701-15743). No zero/wildcard hashes are ever sent; stale-epoch proofs are suppressed rather than sent
  backward (the transport rejects backward tag order).
- Peer hashes queue (bounded 128, 15765-15769) and compare only when local confirmation reaches the frame; the
  comparison is **exact across all fields** (15791-15803). Mismatch ⇒ `netWriteConfirmedDesync` evidence dump +
  `kNetConfirmedDesync` fail-closed (15804-15809). Epoch relations (Current/Stale/Future/Inconsistent) are
  serial-compared (15496-15521); stale is ignored, future waits, inconsistent is a protocol violation.
- RNG/HP fields are diagnostics; `gameplayHash` is authoritative (WireProtocol.h:236-245).
- Additionally, **before frame zero of every epoch**, peers reliably exchange a typed layout/content digest and
  gameplay opens only after both acknowledge the same one (`netCurrentLayoutDigest`, GameHooks.cpp:15862-15931;
  `netLayoutBarrierReady` gate at 17765-17772). Hard-won lesson embedded there: audio manifest and mixer state
  were **removed** from the digest after they killed matches whose gameplay states were identical
  (15895-15913).

---

## 3. Tick driving and pacing

### 3.1 Drive model: detour the dispatcher, hold with a benign return

The DLL detours QOH's scene frame dispatchers (match/menu/charselect); in netplay mode every entry routes to
`doNetplay(orig)` (dispatch switch GameHooks.cpp:22581, 22610, 22638). `doNetplay` runs the whole per-opportunity
pipeline (§3.4) and, when it must wait, returns `kNetHoldFrameRet = 1` — the native "nothing happened, keep
looping" value (return-value semantics documented at GameHooks.cpp:864-872; returning 0 exits the game). Holds are
`Sleep(1)` retry polls (~1 kHz), explicitly **not** cadence opportunities.

### 3.2 Timing sources — three clocks, strictly separated

1. **Real QPC** (`netUnhookedQpcNow`, GameHooks.cpp:10025-10032; original import pointers) — drives pacing,
   telemetry, deadlines. `timeBeginPeriod(1)` while pacing is owned (netPaceInit, GameHooks.cpp:2023-2027).
2. **Synthetic deterministic game clock**: QOH's imports of `timeGetTime`/`GetTickCount`/`QueryPerformanceCounter`
   are **IAT-patched** (not MinHooked — WP-003 note, GameHooks.cpp:10115-10162); during an owned tick, on the
   dispatch thread, outside the pacing bracket, they return frame-derived time `frame*10000/606 ms`
   (`netSyntheticMs`, GameHooks.cpp:10033-10041, hooks 10074-10113). Game logic that reads the clock is thereby a
   pure function of the canonical frame — identical on both peers and under re-simulation.
3. **Pacing-bracket exclusion**: the native limiter busy-waits on the clock ~600×/frame; bracket hooks around
   `sub_4704E0`/`sub_4704A0` set `g_inPacing` so those reads pass through to real time and never enter the
   record/replay rings (GameHooks.cpp:10061-10073, 10164-10182).

### 3.3 Single pacing owner

- The game's own limiter gate is hooked; when netplay owns the tick it answers "advance" without touching timing
  globals (`g_netGateOwnsTick` freeze semantics, GameHooks.cpp:10174-10182; without this the sim ran ~2100 fps —
  comment at 874-878).
- The ddraw wrapper's limiter is handed off at arm: pacing ABI check + `EFZDDRAW_SetFrameLimit(0)`
  (`netAcquireWrapperPacingOwnership`, GameHooks.cpp:1944-1975); teardown restores 60 (1977-1988).
- The wrapper calls the DLL back at the completed-frame Blt: `tryAcquirePresentationSlot`
  (GameHooks.cpp:22872-22933) validates thread + tick ownership + disposition, then blocks in
  `netConsumePresentationSlot` (GameHooks.cpp:2074-2128) until `NetplayPacingDeadline::poll()` grants the slot.
  So **the 60.60 Hz gate is charged per *presented* frame, inside the native tick, at the present boundary** —
  native execution time is part of the interval, not jitter on top of it.

### 3.4 The visible clock: `NetplayPacingDeadline` (`src/game/NetplayPacingDeadline.h`)

- Target period: `g_paceTicks = QPF*10/606` (60.60 Hz, GameHooks.cpp:1992-1997; the rational is also pinned in
  PacingBudget.h:35-39).
- **Absolute ideal deadline** (no cumulative Sleep drift, preserves long-term game speed) + a second
  **earliest-visible spacing floor** (7/8 frame from grant; ≥1/2 frame from *observed presentation completion*)
  so a late frame is not followed by a visible burst (NetplayPacingDeadline.h:50-134).
- Sub-frame lateness: remainder preserved (next wait shortens slightly). **Multi-frame lateness is never repaid
  by compressing visible cadence** — the deadline *rebases* and reports discarded whole frames
  (`rebasedDebtFrames`), and the driver decides their fate (§3.5) (poll(), NetplayPacingDeadline.h:136-193).
  `kTrueStallFrames = 1`.
- `semanticHold(now)` marks intentional holds without mutating deadlines (avoids both permanent slow-motion and
  burst-on-resume, .h:94-104).
- QPC regression fails visibly-smooth (reset, .h:140-148); a dead presentation clock latches a fault and the
  session fails closed (netConsumePresentationSlot 500 ms watchdog, GameHooks.cpp:2116-2121).

### 3.5 The scheduler: plan → debt → hidden catch-up (`src/game/NetplaySchedulePolicy.h`)

Pure, unit-tested `planSchedule(FrameDomains, OpportunityInputs, cfg)` (.h:110-195), decision order:

```
!cadenceOpportunity                        -> Hold(NotCadence)         (1 ms poll: zero debt motion)
pendingCorrection                          -> Correct                  (first; no forward this call)
!localInputReady                           -> Hold(LocalInputMissing)  (not a network wait)
lifecycleExactBoundary && !remoteActual    -> Hold(LifecycleBoundary)  (no debt)
depth >= R_local && !remoteActual          -> Hold(PredictionLimit)    (debt CreateOne)
else                                       -> Advance(1 + extra hidden catch-up)
```

- `predictionDepth = simFrontier - remoteActualPrefix` (half-open, wrap-safe, .h:98-102).
- **There is deliberately no "we lead the peer" hold** (removed 2026-08-05; the deletion rationale — the estimate
  was transit-stale, fired 43×/match, needed an anti-ping-pong latch — is preserved in-code at .h:150-169;
  Rev2 §11.2: canonical time is never slowed to chase a peer; the prediction ceiling regulates from **local**
  facts). The *trailing* side instead runs hidden catch-up: `extra = min(owedDebt or behind-by-beyond-deadband,
  maxCatchupExtraPerOpportunity, safeForwardHeadroom)` (.h:171-194).
- **CadenceDebt** (.h:200-231): bounded ledger, max 8. `CreateOne` on PredictionLimit holds; `Consume` by hidden
  frames actually executed; `discardExternal()` on focus loss/suspend. `applyRebasedFrames` (.h:240-248) is the
  single entry converting deadline-rebase lateness into debt — and only when the last hold was `PredictionLimit`
  (slews/external stalls stay discarded; exactly one repayment path per missed frame).
- Live config `kNetSchedulePolicy{3, 3}` (GameHooks.cpp:961; raised from {1,2} after predicted catch-up landed —
  note ROLLBACK_AS_BUILT.md §2 still says {1,3}/deadband 2: trust code).
- `safeForwardHeadroom` = remaining prediction capacity `R_local - depth - 1` — hidden frames run on **predicted**
  input (Rev2 §7.5); the −1 reserves the visible frame's own prediction slot (the bug where hidden frames spent
  it produced "62 fps simulated, one frame drawn every 80 s" — GameHooks.cpp:19526-19556, 19629-19643).
- Batch execution (doNetplay, GameHooks.cpp:19644-19744): per-iteration re-`nextAction()` (session re-checks
  every step), wall-clock budget `kNetCatchupBatchBudgetUs = 6000` per opportunity (GameHooks.cpp:1004) measured
  against **hidden** work only; on overrun the batch *collapses* so the current iteration becomes the visible
  frame (never "break and draw nothing"); partial batches still credit the hidden frames they ran
  (19728-19740 — un-crediting was a self-sustaining trap). Catch-up repeats the already-sampled local word —
  hardware polled at most once per opportunity, a repeated held word cannot mint an edge (19656-19663).

### 3.6 Anti-jitter / anti-slowdown mechanisms (and deliberate absences)

- **No GGPO-style frames-ahead/time-sync**: no sleep-to-rebalance handshake exists. The regulators are
  (a) the prediction ceiling (local facts), (b) hidden catch-up for the trailing side, and
  (c) **rollback-balance pace slew** — the only wall-clock control: if a fresh peer `PressureReport` shows this
  side has materially lower prediction depth, the *low-depth* peer temporarily shortens its own frame period
  (never below native for the other side), bounded PI-ish controller: ±2500 ppm/frame lag, rise ≤8000 ppm/step,
  cap 25000 ppm (2.5%), deadband + hysteresis + instant release on any untrusted sample
  (`planTrailingPaceCorrection`, NetplayPacingDeadline.h:229-279; admission `classifyRollbackBalance`,
  RollbackBalancePolicy.h:106-133; wiring `netRegulatePaceSlew`, GameHooks.cpp:17484-17617 — note it runs on
  every turn *including stalls*, because the stalled side is exactly who needs releasing). Determinism unaffected
  by construction: it changes how fast wall time is spent, never which frames run (NetplayPacingDeadline.h:76-78).
- **Tick-scale manipulation is otherwise absent on purpose** — no speeding/slowing of canonical ticks to chase a
  peer (Rev2 §11.2 quoted at NetplaySchedulePolicy.h:150-157).
- **Progress deadline** (distinct from transport liveness): consecutive wall time with zero canonical progress —
  warn at 8 s, terminate at 20 s ("the other player's game stopped responding") — because a wedged game still
  answers keepalives (doNetplay, GameHooks.cpp:19408-19450; constants 1012-1016).
- **Frame-advantage catch-up throttle**: estimate-triggered (no-debt) catch-up arms a cooldown so transit-stale
  peer-frontier noise cannot chain spurious batches; peer-frontier freshness window is 4 frames
  (`kNetPeerFrontierFreshMs = 64`, GameHooks.cpp:950; transit/age compensation at 19497-19509).
- **Typed run state + hold-episode ledger** (`game/NetplayRunState.h`): Running / CorrectionCatchUp /
  PredictionPressure / HoldNegotiating(unreachable) / EmergencyNetworkHold / RecoveryCatchUp /
  ExactLifecycleBarrier / ExternalSuspension / Terminated, classified once per opportunity from the plan —
  observational only, but it is what finally made "slow because network" vs "slow because lifecycle wait"
  distinguishable (ROLLBACK_AS_BUILT.md §9a). Native hit-freeze explicitly must never count as a hold
  (NetplayRunState.h:36-42).

### 3.7 Spectator pacing (`src/game/SpectatorPlaybackPolicy.h`)

Deliberately latency-biased: startup buffer 240 records (~4 s) = live target; rebuffer resumes at 120; automatic
catch-up engages above 300 with hysteresis back to 240 (.h:19-38). **Elastic slow-motion under low water** instead
of freeze-then-sprint: 950/850/700 permille at 180/120/60 records, Bresenham-distributed holds
(spectatorElasticPlan, .h:40-163). Deep-backlog catch-up ladder: budgets 2/4/8/16/24 ticks per presentation slot
by backlog rung, bounded by a 12 ms wall slice (.h:173-207, 63-77); catch-up ticks are hidden or drawn
(`VisibleFastForward`) but never own the cadence slot; the loop yields at any dispatcher/phase/scene boundary
(.h:242-252; loop GameHooks.cpp:20370-20489). Every record is verified: pre-state hash must match the archive
before the tick, post-state after; mismatch is a spectator-side desync fail-closed (20322-20356, 20427-20443).

---

## 4. Session lifecycle

### 4.1 Startup: deterministic bootstrap → Ready barrier → GO

- Network **DirectBoot** walks the game deterministically to a fixed main-menu barrier as *simulation state*
  (advanced only inside owned ticks, inputs neutral — netRunOwnedTick 14529-14538; barrier gate in doNetplay
  18903-18907).
- Ready snapshot: RNG seed triple read from the game (0x4A9938/0x4A993C/0x4ABB24), `bootstrapHash`
  (canonical frontend/audio-manifest fingerprint), active match config validated and packed
  (doNetplay 18908-18929). Reliable Ready exchange; **bootstrapHash mismatch fails closed before frame 0**
  (18938-18947); P1's config tuple + seeds applied on both sides (18948-18958); rollback session initialized;
  recorder + spectator archive capture the same GO descriptor from the same seam (18959-18968); `g_netRunning=true`.
- Post-DirectBoot input hygiene: `NetworkDirectBootInputGate` requires one physically-neutral sample before
  accepting production input, so a held confirm cannot surface as a fresh edge frames later
  (NetplayLifecyclePolicy.h:166-203; wiring with SOCD subtlety at doNetplay 19235-19282).

### 4.2 Match boundaries WITHOUT per-match bootstrap — epoch rotation

**This is the centerpiece for AS2.** The session, transport link, frame counter, input history, confirmed
pipeline, spectator archive all continue across matches. What changes at a character/frame-data lifetime boundary
is the **epoch**:

- `reregisterFrameData()` (GameHooks.cpp:11865-11906): drop all pointer-child registrations, invalidate every
  savestate slot, `g_rbReady=false` (forces `ensureRollback()` to re-register + re-`init()` the pool with the new
  layout), `++g_netEpoch` (skipping 0), invalidate FPU slots, rotate the audio epoch.
- Trigger points: entering gameplay with a pending refresh or changed layout config (doNetplay 19174-19184), and —
  the precise one — the **rematch's scene-2/matchSub -1 confirmed setup tick**, rotated *before* the native setup
  tick so setup and frame zero share one epoch (netAdvanceOneCanonicalFrame 17667-17681).
- Epoch qualification everywhere: savestate tags, sync-hash tags, FPU slots, audio journal, replay/spectator
  records. Stale-epoch peer hashes are *ignored*, future ones queued, inconsistent ones fatal (§2.6). The
  reliable **layout digest rendezvous** re-runs before each epoch's frame zero (§2.6).
- The frontend between matches runs **exact lockstep** (no prediction outside `rollbackEligible()` — scene 2 +
  matchSub 0; netAdvanceOneCanonicalFrame 17651-17666 refuses any predicted non-gameplay tick and counts it as an
  exact wait), so both sims cross every lifetime boundary on identical inputs at the same canonical frame.

### 4.3 Rematch — the v3 "canonical lockstep prompt" (three designs' graveyard, one survivor)

The final design (`netCanonicalRematchTick`, GameHooks.cpp:14318-14473): the prompt is **ordinary exact-lockstep
frontend frames**; everything about it is a pure function of (native state, canonical inputs), stepped inside the
one shared executor — no holds, no out-of-band advertisements, no ladder stages to stall, "there is nothing
out-of-band left to lose" (14318-14329). Both peers freeze at the same doorway frame by construction
(exact-input contract), a native "continue" theater is flipped deterministically, YES-YES performs an **instant
restart** that never leaves scene 2 (post-tick writes at 14453-14472), decline routes to character select. The
earlier out-of-band prompt machine (QNP11 RematchChoice + SessionEvent commit, netPumpSessionEvents
18000-18110+) survives as the agreement/commit channels; its freeze-frame variant is dead scaffolding
(18069-18074). Lesson chain: unilateral decline desynced (18185-18194), the two-party prompt broke test harnesses
that pressed only on the loser's side (ROLLBACK_AS_BUILT.md §9c).

### 4.4 Exact-input lifecycle windows (KO/round-result) — the narrowing saga

`lifecycleTransitionRequiresExactInput(roundResultState, roundCounter, stageTimer)`
(NetplayLifecyclePolicy.h:79-85): originally the whole KO outro forbade prediction; live capture proved nothing is
freed at a *round* end, the outro is savestate-complete, and the wide window cost 39 s of an outro that natively
takes 4.5 s at 264 ms RTT ("14% of a match's frames burning 60% of its wall clock") — so it narrowed to
(a) `roundCounter == 5` (match-end handoff into the winscreen, protecting `reregisterFrameData` at the rematch)
and (b) the ONE destructive stage-8 commit tick (`roundResultState == 8 && stageTimer <= 1`), kept as cheap
insurance because the self-test proves reproducibility but not commit-tick mispredictions (.h:9-78 carries the
whole evidence chain, including the decompiled stage-8 tick). **Read this file in full before designing AS2's
boundary predicate — it is the best single document on how to decide what must be exact.**

### 4.5 Committed session-event timeline (leave / F1 return-to-charselect / pause-class events)

QNP8 `SessionEvent` ladder (WireProtocol.h:324-348): Request → Quiesce (freeze capture + report frozen scheduler
facts) → Prepare(boundary B) → AtBoundary → Commit → CommitAck, host-sequenced, idempotent, latest-stage
retransmitted. The driver (`netPumpSessionEvents`, GameHooks.cpp:18000+) runs after ingress and before capture;
while draining toward B the scheduler is forced to single-frame opportunities so catch-up cannot execute B-1 and
B in one batch (`sessionEventDrain`, NetplaySchedulePolicy.h:80-84, doNetplay 19519-19523,
`nextQuiesceDrainAction` RollbackSession.h:277-280, `fillLocalNeutralThrough` .h:345-350). This is how "leave
match" and "return to character select" land on the **same canonical frame on both peers**.

### 4.6 Desync and disconnect policy

- Desync: exact full-state proof every 30 confirmed frames (§2.6); on mismatch both sides write region-level
  evidence dumps (`netWriteConfirmedDesync` / desync dump format doc `docs/runtime/qoh99_desync_dump_format.md`)
  and terminate with `ConfirmedDesync`. **Whoever detects, decides** — there is no arbitration; the terminal is
  repeated reliably to the peer.
- Disconnect: any local terminal (`netFailClosed`) halts simulation, latches a sticky reason, and the pump
  repeats `Disconnect` every 100 ms until `DisconnectAck` (NetplayCore.cpp:1074-1107). Remote terminals are
  applied before local liveness judgment (§1.6). Timeouts: 10 s transport liveness, 20 s zero-progress deadline.
  **There is deliberately no gameplay stall timeout that drops a session mid-hold** (§9.11 "no automatic drop",
  ROLLBACK_AS_BUILT.md:243-246) — the two timeouts above are the only reapers.

---

## 5. Delay / rollback: negotiation model and mid-session adjustment

**There is no negotiation.** The CCCaster peer-local model, taken further:

- Coverage is directional: `coverage(peer→us) = peerDelay + OUR rollback`; `coverage(us→peer) = OUR delay + peer
  rollback` (PacingBudget.h:17-27). Your rollback protects **you**; your delay protects the **opponent**. The
  opponent needs neither notice nor agreement for your changes (RollbackSession.h:204-215).
- Caster lobby: each side picks its own D/R (0-15) with measured route evidence; `PacingBudget.h` computes
  directional requirements from the selected-route p95 (`RouteRttMs` strong type so the TCP lobby RTT physically
  cannot leak in, .h:79-101), classifies FullSpeed/Marginal/Underbuffered, and **recommends one frame above the
  threshold** — the "solve for full speed, not the threshold" lesson, backed by the live capture where
  budget==required ran 41-59 fps with 1273 pure-starvation stalls (.h:189-273). Route-scaled safety margin:
  `max(2, oneWay/3)` (.h:50-76). Remote D/R are advertisements for HUD/diagnostics only; provenance is logged,
  never promoted to "peer applied" without attestation (armNetplay, GameHooks.cpp:23237-23266).
- **Mid-session delay change** (hotkeys `-`/`=`, RuntimeDelayControl.h): request → drain at a **fully confirmed
  boundary** (`tryChangeInputDelayAtConfirmedBoundary`, RollbackSession.h:192-201): global frame counter never
  resets; raising fills only missing future slots with neutral; lowering drains the longer pipeline without
  recapture. While draining, capture and simulation hold but transport keeps pumping and confirmed/spectator
  publication keeps flowing (doNetplay 19113-19131). Drain timeout 5 s → revert to the proven old value
  (`kRuntimeDelayDrainTimeoutMs`, RuntimeDelayControl.h:16-20, cancel at GameHooks.cpp:17094-17114).
- **Mid-session rollback change** (hotkeys `[`/`]`): pure capacity — raising is immediate; lowering below live
  speculative depth is **deferred and retried**, never forced (RollbackSession.h:204-217;
  `netRequestGameplayRollback` GameHooks.cpp:16794-16848). Route-derived floors stop players tuning below what
  the route needs — floor under *your delay* comes from *peer rollback* and vice versa (16714-16773).
- **Two delay regimes** (`netApplyPhaseInputDelay`, GameHooks.cpp:17010-17170): rollback gameplay uses the
  selected gameplay delay; **everything else (exact frontend: menus, charselect, results/winscreen) uses an
  RTT-derived frontend delay** — `frontendDelayForRttMicros` = ceil(one-way frames from dwell-corrected µs p95)
  + 1 safety frame (2 once one-way ≥ 6 frames), floored at gameplay delay, capped 15
  (FrontendDelayPolicy.h:26-69). The latch that tracks it (`FrontendDelayLatch`, .h:94-137) rises AND decays one
  frame per interval (0.15 s up / 0.30 s down), holds baseline until the RTT window has ≥12 samples — each rule a
  scar from a named defect (unit mismatch: the "3 s" decay that ran at 300 ms; the one-poisoned-sample jump to 15
  frames; the empty-boot-window latch — GameHooks.cpp:16572-16634). Regime switch keys on **scene** (scene 2 +
  matchSub 0), NOT on the KO predicate — keying it on `rollbackGameplayPolicyActive()` inflated capture delay
  after every round (D1a's fix, 17012-17024).

---

## 6. What is portable vs game-specific — and the AS2 fit assessment

### 6.1 Directly portable (take the files, or transliterate 1:1)

| Piece | Files | Notes |
|---|---|---|
| Rollback/input core | `netplay/RollbackSession.{h,cpp}` | Zero Win32/game deps. The exact confirm/predict/correct/immutability semantics AS2 needs to replace GekkoNet. Includes the §6.1 producer and the delay/rollback change machinery. |
| Wire state machine | `netplay/NetplayCore.{h,cpp}`, `netplay/WireProtocol.{h,cpp}` | Socket-free, clock-injected, fully testable. Even if AS2 keeps ENet, the *state machine* (ready barrier, hash queues, pressure, terminals, ingress validation taxonomy) is the template. |
| Scheduler + debt | `game/NetplaySchedulePolicy.h` | Pure header. The decision order + CadenceDebt + applyRebasedFrames is the whole "no slow-motion, no burst" contract. |
| Pacing deadline | `game/NetplayPacingDeadline.h` | Pure header. Absolute ideal deadline + spacing floor + rebase reporting + bounded one-sided pace correction. |
| Frame arithmetic | `net/FrameArithmetic.h` | Trivial and load-bearing (every frontier is half-open + wrap-safe). |
| Coverage math | `net/PacingBudget.h` | Directional coverage, classification, recommend-above-threshold. Feed AS2's lobby UI with it. |
| Frontend delay | `game/FrontendDelayPolicy.h` | Curve + latch, µs-typed. AS2's frontend lockstep gets its delay from this. |
| Runtime tuning | `game/RuntimeDelayControl.h`, `game/RollbackBalancePolicy.h` | Hotkey edge/gating policy; balance-slew admission. |
| Run-state telemetry | `game/NetplayRunState.h` (+ HoldEpisodeLedger) | Build this in from day one — their biggest observability lesson. |
| RTT estimation | `net/RouteRttEstimator.h`, RouteProbe messages | Dwell-corrected, generation-gated, local-stall-rejecting p95. |
| Spectator pacing | `game/SpectatorPlaybackPolicy.h` | Pure policy: buffers, elastic slow-mo, catch-up ladder. |

### 6.2 Portable as *patterns* (contents must be rebuilt per game)

- **RollbackManager**: the tagged-slot ring, layout generations, atomic save, hash-canonicalization of
  process-local pointers, Gameplay/RenderCosmetic region roles, extra-region debug API, desync-dump
  introspection. AS2 already has a savestate story under GekkoNet; adopt the *tag* (`{layoutGen, epoch, frame,
  phase}`) and the fail-closed atomicity.
- **`netRunOwnedTick` single-executor**: input injection seam + disposition scoping + deterministic clock window +
  SEH + post-hoc slot fallback. AS2's equivalent is whatever wraps its frame dispatcher; the invariant to copy is
  *one executor, every role, presentation as a parameter*.
- **Confirmed-frame pipeline** (`netPublishAndVerifyConfirmed`): one immutable seam feeding hash exchange,
  recorder, spectator. AS2's replay/spectator features should hang off confirmed frames the same way.
- **Lifecycle predicates** (`NetplayLifecyclePolicy.h`): the *method* — decompile-verified narrow exact windows,
  epoch rotation at the confirmed lifetime boundary, gates as pure constexpr functions with the evidence in
  comments — is exactly what AS2 needs for its own scene machine; every address in it is QOH's.
- **Deterministic clock shim**: if AS2 sim code reads wall clocks, the IAT-patch + synthetic frame time + pacing
  bracket is the proven shape. (If AS2's determinism work under GekkoNet already fixed clock reads, skip.)

### 6.3 QOH-specific — do not port

Caster/launcher/injection/socket-handover (`src/client/`), DirectBoot walker (`game/DirectBoot.*`), rematch
theater flip (native Arcade-continue screen), the QOH address/region catalogs (`src/qoh99/`), audio journal
specifics (though `ROLLBACK_AUDIO_DESIGN.md`'s intent-journal/reconcile model is worth reading if AS2 replays
audio), ddraw-wrapper pacing ABI, DPlayTransport remnants, relay/spectator topology services (unless wanted
later).

### 6.4 Candid mismatches with AS2's current architecture

1. **Per-match GekkoNet bootstrap vs one-session canonical timeline.** This is the deepest change, and it's the
   point of the rebuild. qoh99 proves the target: session arms once; the frame counter never resets; matches are
   epoch rotations (`reregisterFrameData`) inside a continuous confirmed timeline; the frontend between matches
   is exact lockstep on the same timeline. AS2's existing "frontend lockstep with epochs" is conceptually
   *already* the qoh99 frontend — the rebuild extends that timeline through gameplay instead of handing off to a
   freshly-bootstrapped GekkoNet session per match. Consequence to plan for: AS2's match-entry code must become
   an *epoch boundary* (re-register state regions, rotate epoch, layout rendezvous) rather than a session
   constructor, and every match-scoped buffer (inputs, savestates, hashes) must become epoch-tagged instead of
   reallocated.
2. **ENet vs custom UDP.** qoh99's model does not require dropping ENet, but two of its properties must be
   preserved *through* ENet if kept: (a) inputs are **unreliable + redundant window + idempotent merge** — do NOT
   send inputs on a reliable-ordered channel (head-of-line blocking recreates exactly the stalls this design
   exists to kill); use an unsequenced/unreliable channel carrying the ackThrough-anchored window; (b) the
   reliable messages are **latest-wins slots with app-level resend**, not a stream — ENet reliable channels can
   carry them, but keep the app-level state machine (Ready barrier, hash tag ordering, SessionEvent ladder) from
   NetplayCore rather than trusting channel ordering as a protocol. Session cookie/CRC/endpoint binding comes for
   free with ENet; the ingress-validation taxonomy (NetplayCore.h:59-86) is still worth mirroring for payload
   semantics. Alternative worth costing honestly: port `NetplayCore`+`WireProtocol` wholesale and drop ENet —
   they are small, tested, and remove a dependency; the loss is ENet's connection management, which qoh99
   replaces with ~200 lines of Hello/liveness anyway.
3. **Custom palettes exchanged pre-match.** qoh99 has no user-content exchange; its closest analog is the Ready
   payload + bootstrapHash + compatibility identity (content that must match fails closed at Hello/Ready). AS2's
   palettes are the opposite — peer-*different* content that must be transferred. Keep the existing AS2 pre-match
   ENet exchange in the frontend phase, but move it **before the Ready barrier** in the new lifecycle and fold a
   palette digest into the AS2 equivalent of `bootstrapHash`/layout digest *only if* palettes can affect
   simulation; if they are render-only (per the in-memory palette pipeline notes), keep them out of every hash —
   qoh99's audio-manifest lesson (§2.6: identical gameplay killed over cosmetic bookkeeping) applies verbatim.
   Note the epoch model gives palettes a natural home: a mid-set palette change is an epoch-boundary event, not a
   session rebuild.
4. **Menu-driven session UI stays.** qoh99's caster owns discovery/handshake; AS2's in-game menu owns it. The
   seam to copy is `armNetplay(localPlayer, delay, rollback, remoteDelay, remoteRollback, provenance, ...)`
   (GameHooks.cpp:23148) — a single arm call with all tuning decided beforehand, plus `disarmNetplayState()`
   (22687) that restores every pacing/limiter/config side effect. AS2's menu becomes the caster: it runs the
   control conversation (over ENet), then arms.
5. **Pacing ownership.** qoh99 assumes it can neuter both the game limiter and the wrapper limiter and own a
   blocking present-boundary gate. AS2's mod owns the renderer path already (DLL proxy), so the equivalent exists,
   but the AS2 plan must decide the seam where the presentation slot blocks (their choice — inside the native
   tick at the present call — is what makes native execution time part of the pacing interval; copying only the
   deadline class without that placement reintroduces jitter).
6. **60.6 Hz rational**: QOH-specific. AS2 substitutes its own exact rational; keep it integer-exact on both
   peers (PacingBudget.h:35-39's reasoning) and carry it in the handshake (their delta-register item #8:
   versioned cadence profile, fail-closed on mismatch — ROLLBACK_AS_BUILT.md:156).
7. **Spectators/replay**: qoh99's confirmed-record archive + verified playback through the same executor is the
   model if AS2 wants spectating later; nothing in the player path depends on it (publication is observational,
   fail-degrades — GameHooks.cpp:15581-15599).

---

## 7. Lessons register (their docs/plan, distilled — highest-value inputs to the AS2 plan)

From `ROLLBACK_AS_BUILT.md` unless noted:

1. **"Full canonical speed, zero desyncs" is not evidence the lifecycle is healthy** (§9c end) — the rematch hang
   had both for 30 s at a time. AS2 needs lifecycle-progress assertions, not just sync/fps.
2. **Label every hold with its true cause, from day one** (§8, §9a; the D2/D3 defect): 608/817 holds logged as
   "frame-advantage" that were all exact-input waits; the starvation metric measured the wrong quantity until the
   typed run state existed. Telemetry blind spots cost them weeks of misdiagnosis.
3. **One repayment owner per missed frame** (§1, §3; scheduler doc §4): visible cadence never compresses;
   rebased lateness → typed debt → bounded hidden catch-up. Their Class-A defect (correction lateness dropped
   because `lastHold == None`) shows the gate must admit every *typed* cause deliberately.
4. **Catch-up must work on predicted input** (§9 delta #5): requiring actual-through-f+1 made repayment inert at
   WAN depth; debt pinned at 8 forever. The fix (Rev2 §7.5) bounds by remaining prediction capacity instead.
5. **The visible frame is not optional** (GameHooks.cpp:19630-19643): every batch bound governs *hidden* frames
   only. Two independent bugs each produced "sim 62 fps, one drawn frame per 80 s".
6. **Exact-input windows are the fps killer, not rollbacks** (PacingBudget.h:50-66, NetplayLifecyclePolicy.h):
   the measured WAN sessions lost 12% wall clock to prediction-limit stalls with **zero** rollbacks; the KO
   window at delay 0 was a slideshow. Narrow exact windows surgically (one commit tick, not a stage), and give
   the exact frontend its own RTT-derived delay.
7. **Never feed tuning from a ms-quantized or lobby-contaminated clock** (FrontendDelayPolicy.h:20-25,
   scheduler doc §1-2): GetTickCount RTT (0/16/31/47 ms) latched a LAN session 2 frames high; TCP lobby RTT
   inflated "250-280 ms" that was largely pump-queue artifacts. Type the domains (`RouteRttMs`) so it cannot
   compile.
8. **Units in names, conversion at the caller** (FrontendDelayPolicy.h:76-83): the "...Us" constant compared
   against raw QPC ticks ran the 3 s decay at 300 ms for its entire life.
9. **Ramps over latches, asymmetric toward the cheap mistake** (FrontendDelayLatch): one frame short = brief
   wait, self-corrects; ten frames long = permanent menu lag.
10. **Drain before judging the peer** (doNetplay 18996-19027) and **anchor liveness to the first pump poll**
    (NetplayCore.h:278-283): both false-timeout classes shipped and were fixed by ordering, not by tolerance.
11. **A stall must not stop feeding the opponent** (Rev2 §6.1/§6.2, §9d honestly reports the A/B): the producer +
    per-packet pressure report are the cure for the bilateral sub-speed equilibrium; the A/B also shows how to
    not fool yourself (kill switch as first diagnostic move, single-variable arms, "a metric that shows a
    difference where the mechanism provably did nothing is measuring the machine").
12. **Hash membership is a product decision** (§9b; GameHooks.cpp:15895-15913): two real freeze counters were
    silently outside the hash (invisible divergence), while audio bookkeeping inside it killed healthy matches.
    Audit region roles both directions; split regions rather than argue.
13. **Peer-local everything** (delay, rollback, pacing slew) removes an entire class of negotiation
    protocol/desync surface. The only two-party agreements left are: Ready barrier, layout rendezvous, session
    events, rematch — all reliable, all exactly-once, all at canonical boundaries.
14. **No hold negotiation was needed** (§9a "Not built, deliberately"): independent holds compose correctly by
    construction (each side stops at its own ceiling). Don't build the request/ack ladder until something needs it.
15. **Solve for full speed, not the threshold** (PacingBudget.h:189-208): recommend one frame above required;
    just-fit coverage "degrades cooperatively" into a stable sub-speed equilibrium.
16. **Fail closed, loudly, with self-describing terminals** (QNP9 Disconnect reasons; layout dumps written on
    both machines at mismatch, GameHooks.cpp:15947-15960): every abandoned session should explain itself to a
    stranger.

---

## 8. Quick symbol index (GameHooks.cpp, this checkout)

| Symbol | Line | Role |
|---|---|---|
| `doNetplay` | 18848 | Player driver (per dispatcher entry) |
| `doSpectator` | 19790 | Spectator driver (same executor, archived records) |
| `netAdvanceOneCanonicalFrame` | 17619 | One canonical frame: fences→save→tick→verify→commit→publish |
| `netRunOwnedTick` | 14524 | THE single executor (all roles) |
| `netReplayCorrection` | 17257 | Rollback transaction driver |
| `netSavePreFrame` / `netRestorePreFrame` | 14148 / 14168 | Tagged ring save/load + FPU |
| `netPublishAndVerifyConfirmed` | 15494 | Confirmed pipeline + hash exchange + desync |
| `netPumpSessionEvents` | 18000 | Session-event ladder + rematch prompt driver |
| `netCanonicalRematchTick` | 14331 | v3 canonical lockstep rematch |
| `reregisterFrameData` | 11865 | Epoch rotation / layout re-registration |
| `netApplyPhaseInputDelay` | 17010 | Gameplay-vs-frontend delay regime switch |
| `netRefreshFrontendDelay` | 16572 | RTT→frontend-delay latch |
| `netProduceLocalInputWhileStalled` | 13633 | Rev2 §6.1 producer |
| `netRegulatePaceSlew` | 17484 | Rollback-balance wall-clock slew |
| `netConsumePresentationSlot` | 2074 | Blocking 60.60 Hz presentation gate |
| `tryAcquirePresentationSlot` | 22872 | Wrapper→DLL commit-boundary callback |
| `netAcquireWrapperPacingOwnership` | 1944 | Limiter handoff (`EFZDDRAW_SetFrameLimit(0)`) |
| `netInstallQohClockImportPatches` | 10127 | Deterministic clock IAT shim |
| `armNetplay` / `disarmNetplayState` | 23148 / 22687 | Session arm/teardown |
| `ensureRollback` | 11910 | Lazy region registration + pool init |
