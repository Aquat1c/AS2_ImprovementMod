# 0.7 Netplay Failure — Log Evidence

Analysis of `D:\dev\alice_senki\latest logs\0.7 tests\2` (primary, host PID 18840 / client PID 18420)
and `D:\dev\alice_senki\latest logs\0.7 tests\1` (earlier pair, host PID 23160 / client PID 18772).
Build on both sides: `0.7-beta, Aug 17 2026 01:31/01:32`. Line numbers cite the named files.

Clock note: host timestamps are `01:xx` (Aug 17 local), client timestamps `17:xx` (Aug 16 local); they are the
same instants, offset exactly 8 h. Below, times are given in client clock unless prefixed `host`.

File shorthand used in citations:

| Short | File |
|---|---|
| C-RB | `2\client\as2_rollback_18420.log` |
| H-RB | `2\host\as2_rollback_18840.log` |
| C-FP | `2\client\as2_netplay_fullpath_18420.log` |
| H-FP | `2\host\as2_netplay_fullpath_18840.log` |
| C-GK | `2\client\as2_gekko_18420.log` |

---

## 1. Timeline of session "2"

### Connect and configuration

- **17:44:10** — client connects to host `91.214.211.2:10800`, Hello/HelloAck, ver=18, hash `0x3676CE0D` on both sides (C-RB:276–291).
- **17:44:10** — `DelayPolicy` measures ping=135–136 ms, variance 41–52 ms and recommends **delay=4, max_rb=4** (C-FP:73, 76). The session instead runs the users' expert config: **delay=0, rb=8, delay_mode=asymmetric_expert** (H-RB:157 `Settings loaded: … delay=0 rb=8 … delay_mode=asymmetric_expert`).
- **17:44:13–17:44:21** — while both users sit in the config screen, client `ConnSup` flaps Healthy→Degraded (1000 ms silence)→Interrupted (3000 ms)→Healthy (C-RB:293–297). This is the same supervisor that killed test 1 (§5); here both users clicked Ready before the 20 s Dead threshold.
- **17:44:40** — config exchange: `my_delay=0 my_max_rb=8`, `stall_threshold=9`, `frame_timing=proper_60` both ways (C-RB:360–362, H-RB:361–363).
- **17:44:43** — baseline agreed, digest `0x6B12996B` (C-RB:427).

### Three matches, all completed, host wins 3-0

| Match | Gameplay window (client clock) | Sim frames | Display frames | Wall time | Effective sim rate | Rollbacks (client/host) | Max depth |
|---|---|---|---|---|---|---|---|
| 1 | 17:44:44 – 17:47:04 | 7 173 | 8 371 | ~140 s | **51.2 fps** | 1 073 / 701 | 6 / 6 |
| 2 | 17:47:32 – 17:50:56 | 10 483 | 12 230 | ~204 s | **51.4 fps** | 1 591 / 1 049 | 6 / 7 |
| 3 | 17:51:19 – 17:54:33 | 10 182 | 11 581 | ~194 s | **52.5 fps** | 1 571 / 1 000 | 6 / 6 |

Sources: `RollbackSession BEGIN/END` C-RB:455/679/890/1237/1438/1616, H-RB:453/658/865/1158/1362/1566; display frames from `GameplayBridge Session ended (frames=…)` C-RB:680/1238/1617. Session params every match: `visible_delay=0 effective_delay=1 max_rollback=8 protection_window=9 stall_threshold=9` (C-RB:455).

The display-vs-sim gap is the jitter: **1 198 + 1 747 + 1 399 = 4 344 rendered frames (12–14 % of all frames) where the sim was frozen**. The game effectively ran at ~86 % speed with a freeze roughly every 7th frame.

Set result: host `SetTracker Reset (was 3-0 …)` (H-RB tail), client `(was 0-3 …)` (C-RB tail).

### The jitter, quantified (gameplay phases)

**The wire was fine.** `STATS` (client): `RTT=155.6ms jitter=17.2ms` at f300 through `RTT=154.2ms jitter=14.5ms loss=15/1696` at f9900 of match 3 (C-FP, `[STATS]` first/last). Host: `RTT=154.7ms jitter=8.9ms loss=11/1710` (H-FP, f9900). `NETCLASS` RTT samples across all three matches: rtt_avg 148.9–159.4 ms, p95 166–185 ms, jitter95 mostly 22–38 ms, **`loss=0.000` on every single NETCLASS line** (H-FP samples 01:45:16–01:54:05). Gekko-level loss ≈ 1 % (`loss=16/1354`). Two jitter95 spikes (114/143 ms) appear only in the last minutes (H-FP 01:53:43, 01:54:05).

**The pacer held the game almost every time it made a decision:**

| Metric | Client | Host |
|---|---|---|
| `PACEDECIDE` events | 4 295 | 4 027 |
| … `decision=hard_hold` (`reason=near_rollback_budget`) | 2 624 | 2 429 |
| … `decision=soft_hold` (`reason=persistent_debt`) | 1 616 | 1 550 |
| … `decision=advance` | 55 | 48 |
| `TSYNC` "Runtime freeze ENABLED" pulses | 4 269 | 4 011 |
| `NETCLASS` class-change events | 4 186 | 4 191 |

98.7 % of logged pacing decisions were holds. Freeze pulses ran at **~450–500/min for the entire session** (client per-minute counts: 169, 491, 491, 280, 489, 474, 501, 303, 459, 367, 245 for 17:44–17:54), i.e. ~8 freezes/sec, sampled per-second rate 3–22/s (C-FP 17:45:30–45). First hold fired **0.7 s into match 1**: `f14 decision=hard_hold … reason=near_rollback_budget class=Severe` (C-FP:6266, STALL at C-FP:6268). Cumulative `STALL … count=` reached 480 within match 1 on both sides (C-FP: 17:46:10 `f4350 … count=480`). The suppressed-advance counter hit #5400 by 17:54:21 (C-FP:377126).

**Debt/frames_ahead behavior.** `DEBT` lines show `raw_gap` steady at 3–7 with `remote_eff=1`, so `debt`=2–6 permanently against `rb_budget=8` (C-FP:6394, 6623, 6899, 383211…). This gap is not congestion — it is the physics of delay=0 at 155 ms RTT: one-way ≈ 78 ms ≈ **4.7 frames permanently in flight**. frames_ahead itself was healthy: client distribution 10 284 samples in [-0.5,0), 11 978 in [0,0.5), only 137 below -1.5 and 146 above +1 (host skews mirror-positive: 15 293 in [0,0.5), 5 882 in [0.5,1), 3 024 >1). Clock sync between peers was never the problem.

**Class churn.** NETCLASS logs on change and it changed 4 186 times in ~540 s of gameplay (~7.8 changes/sec). Distribution (client): Lossy 2 040, JitteryHighPing 1 523, Severe 585, StableHighPing 35, StableLowPing 3 — with **loss=0.000 throughout**. Class is driven by `burst` (client distribution: burst=2 ×2 040, burst=1 ×1 310, burst≥3 ×585), which is just the arrival pattern of coalesced input packets at high RTT. It even classifies `Severe` from burst alone with zero measurements: `f3 class=Severe source=avg_only rtt_avg=0.0 … loss=0.000 burst=3` (C-FP:6184, and again at every match start, C-FP:107017).

**Class → thresholds → holds.** Each class rewrites the hold thresholds (from PACEDECIDE lines): StableLowPing soft=3/hard=6, Lossy soft=3/hard=5, JitteryHighPing soft=4/hard=6, **Severe soft=1000000/hard=4**. With debt permanently 2–6, the thresholds sit *below the floor imposed by the network*, so the controller holds forever; and under Severe, `hard_hold reason=near_rollback_budget` fires at debt=4 — **half the configured 8-frame budget**. Observed max rollback depth was only 6–7; the 8-frame budget was never exhausted. The budget didn't fail — the pacer refused to use it.

**Tick-scale churn on top.** A global limiter correction `effective_tick_scale=1.02` is active for the whole session (C-RB:~46 TickHooks, and restated at teardown). The pacer's `target_scale` meanwhile micro-oscillates across ≥15 values 0.994–1.008 (client: 7 566 of 30 868 PACE lines at 1.000, rest spread). A ±0.6 % speed nudge can never amortize a 4-frame transit gap, so the freeze hammer does all the actual work — a classic bang-bang controller: actuator #1 too weak, actuator #2 (full stop) too strong, firing 8×/s.

**Gekko itself was healthy.** `GEKKO_NETOUT` telemetry: ~90–102 input packets/s, ~8 redundant input frames per 26-byte packet, `cache_hit_resends` 0–4/s, acks flowing continuously; zero timeout/disconnect/desync events in either gekko log for the entire session (C-GK; the only "timeout" hits are the config constants `input_retry_interval_ms=50 interrupt_timeout_ms=3000 disconnect_timeout_ms=20000`, C-GK:5, 33948, 83308). Gekko's 50 ms resend cadence contributes to the bursty arrival pattern that the classifier misreads, but Gekko delivered every input and never escalated.

---

## 2. Root cause of the jitter (stable ping, 8-frame budget)

Causal chain, each link evidenced above:

1. **Delay 0 at 155 ms RTT ⇒ permanent 4–5 frame in-flight gap.** DelayPolicy said delay=4/max_rb=4 (C-FP:73); expert config said delay=0/rb=8. Nothing clamps or reconciles the two.
2. **The pacing controller treats that constant transit gap as "debt".** `debt = raw_gap − remote_eff` never goes below ~2–3 because it physically can't (DEBT lines, §1).
3. **The network classifier converts burst-arrival artifacts into scary classes.** loss=0.000 all session, yet Lossy/Severe/JitteryHighPing 99 % of the time, 4 186 class flips, Severe from `burst=3` alone with no RTT data (C-FP:6184).
4. **Classes set hold thresholds (3–6) at or below the debt floor**, and Severe drops the hard threshold to 4 = half the rollback budget.
5. **Result: hold decision on 98.7 % of evaluations → 4 269 one-frame freezes (client) ≈ 15 % of frames → 51–52 fps effective with a stutter every ~7 frames.** This is the user-visible "jitter". It began at f14 of match 1 and never stopped.
6. Contributing noise, not primary: `target_scale` micro-churn (0.994–1.008) fighting the global 1.02 limiter correction; Gekko's 50 ms retry cadence shaping arrival bursts. Explicitly exonerated: Gekko delivery/acks (clean), frames_ahead clock sync (±0.5), rollback budget overflow (max depth 6–7 of 8), packet loss (0.000), the wire itself (RTT flat at ~155 ms).

The user's intuition was right: 8 frames of rollback budget at 155 ms RTT with delay 0 *should* absorb the gap silently (5 in-flight frames < 8 budget). The pacing layer intervened thousands of times anyway.

---

## 3. The rematch-cleanup disconnect (after match 3)

### Exact reason string and kill path

Both sides independently fired the same local kill path 10 s after entering the rematch:

> `[FrontendSync] Recovery requested: frontend input timed out waiting for remote frame`
> → `[PregameSync] Phase FrontendCharSel -> Error (frontend charsel recovery)`
> → `[NetMenu] OpenDisconnectError: reason='Frontend charsel recovery requested: frontend input timed out waiting for remote frame'`
> → `[PregameSync] Abort: rematch cleanup reset` → `[NetMenu] Forcing return to menu` → `[Session] Canceling session (was Ready)` → `[Net] Disconnecting peer (data=4)` → `[Net] Force-disconnected peer` → `[Net] ENet host destroyed`

Client at 17:54:49 (C-RB:1685 ff., `mode=9`), host at 01:54:49 (H-RB:1656–1674, `mode=6`). No Gekko or ENet failure preceded it — the transport was alive (see below). This was a **frontend lockstep starvation timeout, escalated to full session teardown.**

### What actually starved it: frontend stream serial mismatch

The frontend input queue stamps every packet with `(epoch, phase, serial)`. Per-epoch serial allocation in all three healthy pregame cycles was symmetric on both sides: CharSel=1, StageSel=2, WinScreen=4 (FRONTQ census, C-FP/H-FP: epochs 1818080467, 1817907325, 1817679785). In the fatal epoch **1817453462** (=0x6C542396):

- **Host:** `Begin input phase: epoch=1817453462 phase=CharSel serial=1` (H-FP:342661).
- **Client:** `Begin input phase: epoch=1817453462 phase=WinScreen serial=1` (C-FP:383767) — then, 0 ms later, `phase=CharSel serial=2` (C-FP:383772).

Host sends CharSel frames as serial 1; client sends CharSel frames as serial 2; each side's filter silently drops the other's stream. Both queues show the signature of a *filtered* (not lost) stream: `local_head=7` captured and capped, `remote_contig=0 remote_latest=0 remote_ack=0` for the entire 10 s — client to `wait_ms=9750` (C-FP:384986), host to `wait_ms=7500` (H-FP final FRONTQ block). Even the *acks* are zero both ways: neither side accepted a single packet of the other's frontend stream.

Transport proof: during the starvation window client ConnSup went Degraded at 1000 ms silence and **back to Healthy at 16 ms** (C-RB:1680–1682) — packets were flowing; Gekko was already shut down (match ended 17:54:33); ENet stayed connected until each side executed its own force-disconnect.

### Why the client burned serial 1 on WinScreen: a transition-arbiter race

Client-side sequence (C-RB:1616–1691):

1. **17:54:33** match 3 ends; both sides `WinScreenSync Begin` under the *old* epoch (serial 4) (C-RB:1621, H-RB:1571).
2. **17:54:38** client receives host's `PostMatchDecision seq=2 intent=1` (rematch), commits it, then proposes `WinScreenExit seq=3 intent=0` and aborts its WinScreenSync (C-RB:1650–1653).
3. Host, however, **never proposes or commits WinScreenExit seq=3** — it acked the proposal (client sees `Local WinScreenExit seq=3 acked`, C-RB:1670) but its own log ends at `Remote proposed WinScreenExit seq=3 intent=0` (H-RB:1636) with no COMMIT. Compare the healthy boundary at 17:47:11–12, where *both* sides proposed and COMMITTED WinScreenExit seq=1 before PostMatchDecision seq=2 (C-RB:721–747, H-RB:703–710). This time the commit order inverted (PostMatchDecision landed first) and the WinScreenExit barrier was simply skipped by the host: it routed `PostMatchRoute -> ReturningToCharSel -> Inactive` directly (H-RB, 01:54:39).
4. So the client is **still in mode 9 (WinScreenActive)** when host's rematch `SyncAnnounce` for session 0x6C542396 arrives. Client does a **cross-phase adopt** (`Phase GameplayHandoff -> Idle (cross-phase adopt) … mode=9 sub=3`, C-RB:1657 ff.).
5. Because it is still on the win screen, client logs `WinScreenSync Begin deferred: frontend delay not negotiated yet` (C-RB:1668), and once SyncConfirmed lands it executes the deferred `WinScreenSync Begin` **inside the new epoch**, allocating serial 1 (C-FP:383767), then `CharSelSync Begin` takes serial 2.
6. Host, on charsel with no win screen, allocates CharSel = serial 1. Streams never match; 10 s later both sides kill the session.
7. Terminal confirmation of the asymmetry: client dies with `[MatchLife] Disconnect during WinScreenActive` (C-RB:1687) — it never left the win screen; host dies in charsel (`mode=6 sub=2`).

Secondary anomaly captured on the way: client's `Sent SyncConfirm … local_frontend_delay=5 shared=2` (C-RB:1669) — the client reports shared=2 in **every** SyncConfirm all session (host receipts H-RB:315, 730, 1224, 1641) while both sides then use the host's 5. Harmless here (host is authoritative) but the field is computed wrong on the join side — a latent negotiation bug.

### Both sides' views, summarized

| | Host | Client |
|---|---|---|
| Mode at death | 6 (charsel), sub=2 | 9 (win screen), sub=3 |
| Frontend stream sent | CharSel serial=1 | WinScreen serial=1, then CharSel serial=2 |
| Remote frames accepted | 0 (`remote_contig=0`, 7.5 s logged) | 0 (`remote_contig=0`, 9.75 s logged) |
| Kill trigger | Local FrontendSync recovery timeout (01:54:49) | Local FrontendSync recovery timeout (17:54:49) |
| Reason string | `Frontend charsel recovery requested: frontend input timed out waiting for remote frame` | identical |
| Final act | `Force-disconnected peer (data=4)`, ENet host destroyed | identical |

---

## 4. Earlier pair (`0.7 tests\1`) — supervisor kill, and no jitter data

- Host hosts at 01:38:16; client connects 01:39:26; both land in the **config screen** (H-RB `…23160.log`:286 ff., C-RB `…18772.log`).
- Host ConnSup immediately flaps on 1 s silences, escalates: Degraded 01:39:27 → Interrupted 01:39:31 → **Dead 01:39:48 (`inbound silence 20000ms`)** → `Connection lost (no data from peer for 20 seconds)` → force-disconnect data=4 (host fullpath `…23160.log`:266–402; H-RB:292–298).
- Client's view: it had *received* host data fine (`Remote peer signaled Ready` 17:39:33) but sent nothing supervisor-countable while the user browsed config (client never signaled Ready), then got `ENet disconnected (data=4)` → `Remote canceled the session` (C-RB `…18772.log` tail). The supervisor counts app-payload silence, not transport liveness — ENet keepalives were flowing. Known bug, since fixed.
- **No jitter evidence exists in this pair**: gameplay was never reached — both gekko logs are header-only (3 lines), there is no `RollbackSession BEGIN`, no PACE/NETCLASS/TSYNC output at all. The session died 22 s after connect, in menus.

---

## 5. What the rebuilt backend must not do

Hard constraints, each derived from a specific failure above:

**Pacing / timesync (the jitter):**
1. **Must not treat the steady-state in-flight input gap as actionable debt.** Hold thresholds must be floored at `ceil(one_way_RTT_frames) − delay + margin`; at 155 ms RTT and delay 0 the debt floor is ~4–5 and any threshold ≤ that guarantees a permanent stall regime (4 240 holds / 4 295 decisions).
2. **Must not use full-frame freezes as the routine actuator.** frames_ahead stayed within ±0.5 essentially always; freezing 8×/s (15 % of frames, 51 fps effective) for sub-frame errors is bang-bang control. Freezes are for genuine emergencies (predicted depth ≥ budget−1), rate-scaling for everything else — and the rate actuator must be strong enough to matter or not exist.
3. **Must not let a heuristic "network class" rewrite hold thresholds tick-to-tick.** 4 186 class changes in 9 minutes, thresholds swinging 3↔4↔6 and soft↔disabled, is thrash. Any classifier needs hysteresis/dwell time and must never classify Severe/Lossy off `burst` alone while measured `loss=0.000` (C-FP:6184: Severe with zero RTT data at f3).
4. **Must not shrink the effective rollback budget behind the user's back.** `class=Severe` cut the hard threshold to 4 against a configured budget of 8; observed depth never exceeded 7. If the user grants 8 frames, the backend uses 8 frames before holding.
5. **Must not run multiple independent speed authorities.** effective_tick_scale=1.02 (limiter correction) × pacer target_scale (0.994–1.008) × freeze pulses = three actuators on one plant with no shared setpoint. One controller, one output.
6. **Must not ignore its own delay policy silently.** Policy computed delay=4/max_rb=4; config forced delay=0/rb=8; the pacer then punished the mismatch continuously. Clamp, renegotiate, or at minimum surface the contradiction — never let the pacing layer "correct" it covertly.

**Frontend/rematch handshake (the disconnect):**
7. **Must not key frame acceptance on an implicitly allocated per-side stream counter.** The `(epoch, serial)` discriminator diverged (host CharSel=1 vs client CharSel=2) purely because the two sides began different phase sequences; frames must carry the *phase identity* (or serials must be deterministically derived from negotiated phase), and a receiver seeing a coherent-but-unexpected serial must resynchronize, not silently drop.
8. **Must not allow two frontend sync machines in one epoch.** The client's deferred `WinScreenSync Begin` fired inside the new session epoch *after* charsel adoption, consuming serial 1 (C-FP:383767→383772, 0 ms apart). Session adoption must cancel any deferred/pending frontend Begins.
9. **Must not commit transition barriers in side-dependent order.** Healthy boundary: WinScreenExit committed on both sides, then PostMatchDecision. Fatal boundary: PostMatchDecision committed first, host skipped WinScreenExit entirely (acked, never committed — H-RB:1636), stranding the client in mode 9 while host reached mode 6. Barrier order must be fixed and both-or-neither.
10. **Must not let one peer enter a synced frontend phase while the other is in a different mode.** Cross-phase adopt from `mode=9` started CharSel lockstep on a client that was still rendering the win screen. Adoption must include a mode/phase alignment barrier before frontend input exchange begins.
11. **Must not diagnose "peer silent" from a filtered stream.** Both sides had `remote_contig=0 remote_ack=0` for 10 s while ConnSup measured 16 ms inbound silence. Starvation with a healthy transport is a *protocol* fault and must trigger an interrogation/resync exchange (e.g., "what phase/serial are you sending?"), not a timeout.
12. **Must not escalate a recoverable frontend timeout into session destruction.** The kill path was timeout → OpenDisconnectError → cancel session → force-disconnect → ENet host destroyed, on both sides, for a charsel that could have been re-begun under a fresh sync announce. Rematch failures should retry pregame sync; teardown is for dead transports.
13. **Must not ship asymmetric negotiation fields.** Join side sent `shared=2` in every SyncConfirm while the negotiated value was 5 (H-RB:315/730/1224/1641). Every negotiated value must be echoed back verbatim and verified.

**Supervision (test 1, and flaps in test 2):**
14. **Must not measure liveness by app-payload silence.** ENet-level keepalive was alive in both incidents; the supervisor killed a healthy connection (test 1) and flapped Degraded/Interrupted on every load barrier and config screen (6–7 flaps per session in test 2, C-RB:293–297 etc.). Liveness = transport heartbeats; app silence during user-paced screens (config, charsel, win screen) is normal and must not escalate — and 1000 ms is far too tight a Degraded threshold for screens that legitimately pause traffic.

---

## 6. Numbers at a glance

- RTT ~155 ms flat, jitter95 22–38 ms, loss 0.000 (NETCLASS) / ~1 % (Gekko STATS) — the network was fine.
- 27 838 sim frames over 3 matches; 4 344 additional display frames spent frozen (12–14 %); effective 51–52.5 fps.
- Client: 4 295 pace decisions → 55 advance; 4 269 freeze pulses; 4 235 rollbacks (15.2 % of frames), max depth 6.
- Host: 4 027 decisions → 48 advance; 4 011 freezes; 2 750 rollbacks (9.9 %), max depth 7.
- Budget 8 / protection window 9 / stall threshold 9 — never exceeded; hard holds fired from debt=4.
- Fatal rematch: 10.0 s of bidirectional `remote_contig=0` with healthy transport; serial 1 vs 2 on CharSel streams; identical reason string on both sides at ±0 s: `Frontend charsel recovery requested: frontend input timed out waiting for remote frame`.
