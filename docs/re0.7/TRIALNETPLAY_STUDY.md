# TrialNetplay Study — rollback netplay mod for the EFZ 2001 trial

Reference studied: `D:\dev\alice_senki\references\TrialNetplay` (same tree at
`D:\dev\qoh_99\references\TrialNetplay`). All `file:line` citations below are relative to
that root. Line numbers are from the working tree as of 2026-08-17.

## What it is

**Not a game and not a classic external caster.** It is a two-component **rollback netplay
mod for the 2001-04-08 trial (demo) build of Eternal Fighter Zero** (`EFZ_trial/EFZ.EXE`, a
DEBUG-CRT build) — a sister project to the AS2 mod, targeting the same era of engine:

- **`netplay.dll`** — injected DLL, source `trial_hitbox_viewer/src/` (CMake target
  `netplay`, ~39k lines). Owns transport, lockstep, rollback, spectator, determinism,
  crash guards (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:35-70`).
- **`TrialLauncher.exe`** — external ImGui launcher, source `trial_caster/src/`
  (`caster_ui.cpp` alone is 3305 lines). UI, session negotiation, writes a session ini the
  DLL reads, then injects a suspended `EFZ.EXE` via `launch_smoke`
  (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:44-56`).
- **`ddraw.dll` wrapper** (`ddraw_wrapper/`) — DirectDraw→D3D9 wrapper with an
  `EFZDDRAW_SetFrameLimit` export used for the pacing handoff
  (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:850-851`).

Hooking is MinHook on the game's screen-tick handlers, input poll, and render/SFX paths
(`docs/TRIAL_ARCHITECTURE_AUDIT.md:63-74`). Vendored `lib/GekkoNet`, `lib/enet`,
`lib/miniupnpc` and `third_party/libjuice` exist but are **not wired into any build**
(`docs/TRIAL_ARCHITECTURE_AUDIT.md:33-34`) — the transport and the rollback engine are
**fully custom**. `References/CCCaster-master` is present purely as study material. The
consolidated engineering doc is `docs/TRIAL_NETPLAY_DEVELOPMENT.md` (876 lines); the deep
engine audit is `docs/TRIAL_ROLLBACK_AUDIT_2026-07.md`.

Bonus: `docs/alice_senki_reuse_alignment.md` is *their* pass over our AS2 mod — it maps
AS2's `mod/src/net/*` sync files onto their EFZ counterparts and calls out AS2's
three-counter frontend invariant as the thing worth preserving
(`docs/alice_senki_reuse_alignment.md:1-50`).

---

## 1. Netcode architecture

### Transport
- One non-blocking **UDP socket per process** — `OpenSocket`
  (`trial_hitbox_viewer/src/caster/netplay_sync.cpp:2434`, `socket(AF_INET, SOCK_DGRAM,
  IPPROTO_UDP)` at `:2437`), host binds the configured port, client binds ephemeral
  (`BindLocalPort` `:2356`).
- **Loss resilience by brute duplication**: critical control packets are sent as
  `kUdpOverlapSends = 9` overlapped duplicates (`netplay_sync.cpp:61`, `SendToPeer`
  `:414`). Input packets rely instead on the trailing window (below).
- **NAT traversal**: direct IPv4 first, then fallback to the **AutoPunch rendezvous
  coordinator at `delthas.fr:14763`** — the same service AS2 uses
  (`shared/autopunch_protocol.h:13,17`; `ServiceAutopunch` at `netplay_sync.cpp:2537`).
  No TURN-style relay; symmetric NATs still need port forwarding
  (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:800-806`).

### Sync model — split by screen class
- **Frontend (menus / char select / loading): delayed lockstep.**
  `trial_hitbox_viewer/src/caster/netplay_lockstep.cpp` owns local/remote history vectors;
  the delay is **ping half-period only** (`ceil(ping/2 / frameMs)`, comment at
  `netplay_lockstep.cpp:472`), not the battle input delay. Char select merges the delayed
  local + remote wire input into the DirectInput view the game reads, so both cursors are
  wire-driven and picks are inherently synced (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:395-409`).
- **Battle: unified delay + rollback ("retained speculation").**
  Host-authoritative input delay 0..20 and MaxRollback 0..20 as *independent* settings —
  0/0 delayed lockstep through 20/20 all valid
  (`docs/TRIAL_ROLLBACK_INTEGRATION.md:17-34`). Remote inputs beyond the confirmed
  frontier are predicted **hold-last** (explicit GGPO comparison in the source:
  `rollback_engine.cpp:805-806`). The live speculative state is retained; a rollback
  happens **only on an actual prediction mismatch** — matching predictions cost nothing
  (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:451-457`).

### Wire protocol (`trial_hitbox_viewer/src/core/trial_protocol.h`)
All packets `#pragma pack(1)`, dispatched by control word **plus exact packed size**
(`docs/TRIAL_NETPLAY_DEVELOPMENT.md:254-271`). The workhorse `SyncDataPacket` (159 bytes,
`trial_protocol.h:97-116`) carries:
- `timeline` (u16) — input-history **generation**, bumped at every committed battle entry;
  mismatched timeline ⇒ datagram fully ignored (kills the "stale packet repopulates
  trimmed history under a reused frame number" desync class).
- `currentFrame` — sender's input frontier; `ackFrame` — **count of the receiver's inputs
  contiguously received** (the real flow-control ack; the opposite frontier is *not* an
  ack under one-way loss — `docs/TRIAL_NETPLAY_DEVELOPMENT.md:280-282`).
- `confirmedHashFrame`/`confirmedHashChain` — cumulative desync-check hash chain riding
  **every** input packet, so loss can't hide a divergence.
- `actionData[64]` — a **trailing 64-record window** of inputs on every datagram; this is
  the retransmission scheme (sized so every delay/rollback combo up to 20/20 is
  recoverable under asymmetric loss while the sender is capped by the peer's ack).
- `sessionId` (u64) — runtime tag derived from handshake nonces+seed, checked **before**
  liveness or any state mutation (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:360-366`).

### Session flow (runtime, post-injection)
1. Reliable 5-message, exact-16-byte **nonce-bound RNG handshake**: `ClientHello →
   HostOffer → ClientAck → HostConfirm → ClientConfirmAck` (`BeginSeedExchange`
   `netplay_sync.cpp:2640`; walkthrough `docs/TRIAL_NETPLAY_DEVELOPMENT.md:319-334`).
   Fresh nonzero nonces reject prior-session replays; the exact ClientAck fixes the
   endpoint; the 64-bit session ID is derived from `{clientNonce, hostNonce, seed}`.
2. **Rollback negotiation**: tagged `RollbackReadyPacket` exchange; negotiated
   `maxRollback` = min of both requests; `targetFps` must be exactly 60 or 64 and match on
   both peers or the session aborts (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:335-340`).
3. **LaunchGo** (`SendLaunchGo` `netplay_sync.cpp:4339`, 500 ms retransmit): host's
   `inputDelay` + `targetFps` become authoritative on both peers.
4. Frontend lockstep → battle-entry barrier (§4 below).

### Liveness — fail-closed by design
`CheckPeerLiveness` (`netplay_sync.cpp:3245`): handshake-phase absolute deadline
`clamp(15000 + ping·8, 30000, 120000)` ms (`:3264`); active-phase two-tier deadline
`clamp(4000 + ping·8, 8000, 60000)` ms where, once a frontier is behind, only *actual
input/ack progress* refreshes it — duplicates don't (`:3312`,
`docs/TRIAL_NETPLAY_DEVELOPMENT.md:373-385`). Every failure funnels into `AbortNetplay`
(`netplay_sync.cpp:2842`): send `QuitPacket`, **close the game process**. Continuing on
fabricated state is never allowed.

---

## 2. Tick / timing / pacing

### How the game is driven
- The game's own `WinMain` loop dispatches one screen handler per frame off a mode byte
  (`docs/TRIAL_ARCHITECTURE_AUDIT.md:44-60`). The mod MinHooks **every screen handler**
  (`trial_hitbox_viewer/src/hooks/framestep.cpp`, install log at `:588`); during an active
  session the native handler runs **only inside the engine's controlled `AdvanceOne`** —
  the engine drives the game, not vice-versa (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:89-92`).
- A separate **mod thread** polls every 16 ms (`kPollIntervalMs = 16`,
  `trial_hitbox_viewer/src/core/dllmain.cpp:58`; `ModThread` `:727`) for input capture,
  transport pump, and heartbeat; all hook callbacks fire on the game thread.

### Frame limiting — session-owned exact 60/64 FPS
`trial_hitbox_viewer/src/hooks/game_pacing.cpp` replaces the native `timeGetTime`
busy-wait with a **QPC deadline accumulator with fractional-remainder carry** so 60 and 64
Hz are exact with zero drift (`AdvanceDeadline` `:148-161`: `deadline += freq/fps`,
carry `freq%fps`). The wait is `Sleep(remaining-1)` then spin/`Sleep(0)` to the deadline
(`WaitForQpcDeadline` `:165-196`). The rate is **host-authoritative and immutable for the
session** — only 60 or 64 accepted (`ConfigureSessionTargetFps` `:431`; guard `:208-211`).
The native limiter is bypassed for owned frames by back-dating its tick anchor
(`BypassNativeLimiterForOwnedFrame` `:69-82`), and the game's FPS HUD latch is normalized
so a true 64 Hz session still displays sanely (`ServiceOwnedNativeFpsHud` `:86-135`).

### Rollback catch-up inside the frame budget
`BatchRender` (`rollback_engine.cpp:2845`) runs the batch of native re-sim ticks with
hidden (non-final) frames getting: present suppression (no Flip —
`hooks/render_gate.cpp:59-61`), no SFX retrigger (`:72-76`), visual-RNG isolation, a
`BypassNextNativeFrameWait`, and pacing-anchor preservation — so replay CPU time lands
inside the single visible frame's ~16 ms slot and only the final visible tick is paced
(`docs/TRIAL_NETPLAY_DEVELOPMENT.md:497-505`, `docs/TRIAL_NETPLAY_DEVELOPMENT.md:771-773`).

### Peer-to-peer time regulation — *no GGPO-style timesync*
There is no frame-advantage exchange and no "sleep N frames to rebalance" (GGPO's
`timesync`). Instead:
- Both peers run the **same exact host-set FPS** (drift-free QPC pacing above), so rate
  divergence is structurally absent.
- **Flow control**: the sender may never run past `peerAck + 64` (kMaxQueueSlot); the
  prediction budget in `AdjustPrediction` (`rollback_engine.cpp:2449`) is the negotiated
  rollback window clamped by that ack headroom (comments `:2533-2546`).
- If input arrives beyond the combined delay+rollback budget, the game **holds at the safe
  frontier** (stall, not fabricate) until history arrives
  (`docs/TRIAL_ROLLBACK_INTEGRATION.md:31-34`; `WaitRemote` `rollback_engine.cpp:2331`).
- Frontend lockstep stalls are taken *inside* the hooked handler while still consuming a
  pacing slot so wall-clock stays right (`ShouldStallLockstepFrame` gate used at
  `framestep.cpp:194` and `:451`).

### Anti-jitter / latency knobs
- Launcher measures RTT with ping/reply over its own UDP channel
  (`trial_caster/src/net_session.cpp:853-884`) and recommends an initial delay:
  `ceil(RTT / (2·frameMs))`-based (`trial_caster/src/delay_recommendation.h:12-33`).
- **Live in-battle delay adjustment** (numpad hotkeys, local-only, never on the wire):
  `AdjustLiveInputDelay` clamps decreases to `MinimumLiveInputDelay =
  clamp(ceil(ping/2 frames) − activeRollback, 0, 20)` — you can trade delay against
  rollback but not below what the link supports
  (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:528-531`; implementation around
  `rollback_engine.cpp:232-288`).

---

## 3. Connection lifecycle / UI model — **caster-shaped: design differently for AS2**

The whole pre-game phase is an **external-launcher model** (CCCaster lineage):

1. `TrialLauncher.exe` (ImGui, `trial_caster/src/caster_ui.cpp`) — host enters a port,
   client enters `host:port` (`README.txt`), controls are configured in the launcher.
2. Launcher↔launcher negotiation over its own protocol (`trial_caster/src/protocol.h`):
   version/compat check, ping measurement, delay recommendation, practice-key mutual
   opt-in, then a **9-byte launcher `LaunchGoPacket` with a fixed 2000 ms lead**
   (`kLaunchSyncLeadMs`, `protocol.h:173-179`) so both machines start the game
   near-simultaneously.
3. The launcher writes an **all-or-nothing session ini** (`trial_caster_session.ini`, or
   `TRIAL_CASTER_SESSION_INI` env; `ready=1` published only after full write+readback) with
   role, endpoints, delay, rollback, target fps, bindings
   (`docs/TRIAL_ROLLBACK_INTEGRATION.md:38-58`), then **injects the DLL into a suspended
   `EFZ.EXE`** (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:47-50`).
4. Post-injection, the DLL re-handshakes on the gameplay socket (the §1 five-step nonce
   exchange), auto-navigates the game's menus to VS-human
   (`trial_hitbox_viewer/src/caster/netplay_navigation.cpp`), and runs the session.
   Launcher-domain and runtime packets are structurally separated by exact size (8/9/5
   bytes vs 16/17/16) so delayed launcher traffic can never enter a runtime path
   (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:368-371`).

**For AS2 (in-game menu, single process): drop items 1–3 wholesale.** The two-process
split, the session-ini contract, ready-flag handoff, injection sequencing, and the
2000 ms synchronized process launch exist *only because* the UI lives outside the game.
With AS2's in-game menu the equivalents collapse into: an in-menu host/join screen feeding
the transport directly, an in-memory (not ini) session descriptor, and no launch-time
synchronization at all. **What transfers directly**: the runtime five-step nonce
handshake, session-ID tagging, exact-size+control-word packet validation, the
host-authoritative parameter commit (delay/rollback/fps as an explicit `LaunchGo`-style
contract before gameplay), AutoPunch fallback, and the fail-closed liveness policy.
(Their own docs flag the launcher/DLL boundary as the source of several race classes —
stale ini, duplicate instances, deploy-time file locks:
`docs/TRIAL_NETPLAY_DEVELOPMENT.md:852-855`.)

---

## 4. Match / rematch boundaries & state reset discipline

This is the strongest part of the reference — worth adopting nearly verbatim:

- **Input-history timeline**: a u16 generation bumped at every committed battle entry
  (`AdvanceSyncTimeline` `netplay_sync.cpp:4925`); packets from the previous
  screen/battle are ignored entirely, so a reused frame number can never alias
  (`trial_protocol.h:100-103`).
- **Battle-entry barrier** (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:427-449`):
  1. Loading runs lockstep to an **aligned frontier**; the two frontiers may legitimately
     differ by one, so both adopt `min(local, peer)` (`TrimForBattleEntry`) — otherwise the
     desync detector compares misaligned frames forever.
  2. `BattleEntryPacket{ready, epoch, frame, sessionId}` broadcast; the epoch is a
     **full-width transport generation** (`GetSyncGeneration`), *not* the game's own
     peer-local match counter (which can skew across peers) — and 32-bit so a stale advert
     can never alias a later round (an earlier 8-bit epoch wrapped and bit them).
  3. **Commit** (`CommitBattleEntry` `netplay_sync.cpp:4470`): cross the barrier, bump the
     timeline, then **retransmit the committed entry until SyncData bearing the new
     timeline arrives** — the peer's first same-timeline input packet *is* the ack; no
     separate ack packet.
- **Generation-keyed RNG reseed** at each entry: `seed ^ 0x9E3779B9·(generation+1)`
  (`ReseedMatchRng` `rollback_engine.cpp:457-482`).
- **Round vs match boundaries**: battle → round-post → next-round intro share **one
  prediction epoch** — explicitly *no* ring/generation/RNG reset at round boundaries; a
  new match (rematch) starts a new generation and reseeds
  (`rollback_engine.cpp:1808-1896`, comment "no cursor, generation, RNG, recorder, or ring
  reset is allowed here" `:1865`).
- **ESC / KO discipline**: ESC is a synchronized lifecycle edge queued and applied at a
  confirmed boundary (raw DI polling frequency differs between a batching peer and a
  paused one → would desync); the gameplay-ESC fade releases unrestorable input/audio
  resources so every fade tick runs confirmed lockstep
  (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:518-524`). A **predicted** frame that would flip
  round/screen state is vetoed and the match holds until the remote input for that frame
  actually exists — a KO happens only on confirmed data
  (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:507-511`).

---

## 5. Techniques to adopt / pitfalls to avoid for a custom AS2 rollback backend

### Adopt
1. **Retained speculation** (vs. GGPO's rewind-oriented flow): store the predicted remote
   input *in the frame's snapshot slot*; on arrival compare (`PushRemoteInput`
   `rollback_engine.cpp:355`, earliest-mismatch min at `:430-432`); roll back only on real
   disagreement (`ServicePendingRollback` `:2076`, `ReloadSavedState` `:1979`). Correct
   predictions are free.
2. **Never fabricate inputs.** Zero-padding missing history was removed as "a guaranteed
   silent desync"; unfillable gaps are rejected and abort loudly
   (`netplay_lockstep.cpp:371-386`). Corollary: the whole **fail-closed** policy — a
   confirmed hash mismatch or resim divergence terminates the session
   (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:461-463`).
3. **Ack = contiguous-receipt count + trailing input window on every datagram** — implicit
   retransmission with no reliable layer (`trial_protocol.h:97-116`). GGPO does the same
   windowing; the explicit-ack-as-flow-control framing here is cleaner.
4. **Desync detection as a first-class subsystem** (GGPO has essentially none in
   production mode): per-confirmed-frame region CRCs + a cumulative hash chain riding
   every input packet + a **re-sim self-check** that catches incomplete snapshots on a
   *single instance* (re-saving the same frame with identical inputs but different hash ⇒
   non-deterministic resim) (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:575-591`;
   `src/rollback/desync_check.cpp`). Plus the one-shot byte-dump → diff → symbol loop that
   turned each divergence into a ~10-minute fix (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:759-764`).
5. **Hash only corrected confirmed frames, never speculative ones** — predicted-frame
   hashes were their false-positive burst class (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:715-719`).
6. **Split RNG streams**: sim shadow LCG + a *separate, also-snapshotted* visual stream for
   draw-path rand, so hidden re-draws never drain the sim sequence
   (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:569-573`; `src/util/rng.cpp`). FPU control word
   pinned (0x027F) and canonicalized before hashing (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:563-568`).
7. **Timeline/generation scoping + session-ID tagging checked before any mutation** —
   makes stale/replayed/port-reuse datagrams structurally inert
   (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:360-366`).
8. **Exact QPC fractional pacing owned by the mod** (`game_pacing.cpp:148-196`) — for AS2
   this maps onto our existing pacing hooks; the key idea is that netplay owns the frame
   clock and the native limiter is bypassed for owned frames.
9. **Harness before WAN**: two-instance loopback runner with scripted inputs and an
   outgoing net shim (delay/jitter/loss) emitting a machine-checkable PASS/FAIL verdict
   (`scripts/run_harness_session.ps1`; `docs/TRIAL_NETPLAY_DEVELOPMENT.md:766+`). Their
   first fully clean validation: 13,230 battle frames, 0 mismatches, ~8.7k rollbacks under
   40 ms ±15 ms jitter + 3% loss (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:735-738`).

### Avoid / watch for
1. **Snapshotting pacing/wall-clock state**: one pacing byte wrongly inside a snapshot
   region caused an every-frame false desync, and the resulting logging was itself the fps
   collapse (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:707-713`). Region boundaries must exclude
   timing state.
2. **Out-of-region sim state**: edge-triggered flags living outside the obvious player
   structs (their back-charge timer flags) desync only on re-sim — budget for an
   aux-capture list plus a systematic sweep of "excluded window" globals
   (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:720-727`, §9 aux bytes at `:551-556`).
3. **Speculative frames crossing irreversible transitions** (KO fades, resource frees):
   veto and hold rather than predict through them (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:507-511`,
   `:518-524`). AS2's screen-flow equivalent: never let a predicted frame trigger a
   screen-state change that releases assets.
4. **Raw async input polling as a sync source** (their ESC lesson): anything that samples
   input at native-poll frequency diverges between a batching peer and a stalled peer —
   route every lifecycle-affecting key through queued, confirmed-boundary application.
5. **Using game-local counters as network epochs** (their matchId lesson) and **narrow
   epoch fields** (8-bit wrapped in real sessions) — use transport-owned full-width
   generations.
6. **Hidden re-sim side effects**: without present/SFX gates, corrections re-fire hit
   sounds and re-flip; their sound path also had a use-after-free only reachable from
   re-sim timing (`render_gate.cpp:105-152`).
7. **Two-instance/same-folder hazards** (loopback testing): exclusive file opens colliding
   during wire-synced loads produced their worst crash
   (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:685-697`) — relevant to any AS2 loopback harness.

### Contrast with a GGPO-style model (summary)
- Same core lineage: hold-last prediction, snapshot/rewind/replay, input delay + max
  prediction window (the source itself cites GGPO at `rollback_engine.cpp:805-806`).
- **No timesync**: GGPO measures frame advantage and asks the ahead peer to sleep;
  TrialNetplay instead pins both peers to one drift-free host-set FPS and bounds skew via
  ack-window flow control (`peerAck + 64`) plus stall-at-frontier. Simpler, and viable
  because both sides run identical exact pacing — a custom AS2 backend controlling both
  clients can make the same choice.
- **Library boundary inverted**: GGPO is a callback library (save/load/advance) with the
  game as driver; here the engine *is* the driver and the native tick runs inside its
  `AdvanceOne`/`BatchRender` (`docs/TRIAL_NETPLAY_DEVELOPMENT.md:89-92`) — the natural
  shape for retrofitting an unmodifiable binary, and closer to what AS2 needs.
- **Beyond GGPO**: built-in desync detection + fail-closed policy, session-ID/timeline
  anti-replay hygiene, explicit rematch/round generation discipline, live delay
  adjustment, and a spectator system (~4000 lines, `src/caster/spectator_*.{h,cpp}`,
  `docs/TRIAL_NETPLAY_DEVELOPMENT.md:593+`) fed atomically from confirmed frames only.
