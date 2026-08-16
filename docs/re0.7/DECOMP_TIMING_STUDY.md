# AS2 Game-Loop Timing — Decompilation Study (re0.7)

Deep reverse-engineering study of Alice Senki 2's frame timing, written as the
foundation for the 0.7 netcode rebuild. Goal: a netcode that **never** causes
jitter, slowdown, or frame drops unless there is genuine packet loss.

Sources and citation conventions:

- `alicesenki2_decomp_refactored.c` (11 MB IDA export) — cited as `decomp L<line>`.
  Function start addresses come from the `//----- (00XXXXXX)` banner lines, which
  are authoritative. **Refactored names and "Old name" comments can lie** (examples
  called out below); addresses and raw globals are what was verified.
- Mod source (verified at runtime over the 0.6 cycle) — cited by file path.
- All raw addresses match `include/core/as2_constants.h` / `include/core/game_state.h`
  conventions (`ADDR_*`, no ASLR, image base 0x400000).

Contents:

1. [The complete main loop](#1-the-complete-main-loop)
2. [The sub_635F80 timebase and its full caller inventory](#2-the-timebase-sub_635f80--its-callers-the-virtual-clock-risk-surface)
3. [Simulation frame counters](#3-simulation-frame-counters)
4. [Present / vsync behavior](#4-present--vsync-behavior)
5. [How a rebuilt netcode should pace the game](#5-pacing-options-for-the-rebuilt-netcode)
6. [Skipping and doubling frames safely](#6-skipping-and-doubling-sim-frames-safely)
7. [Appendix: address quick reference](#7-appendix-address-quick-reference)

---

## 1. The complete main loop

### 1.1 `Game_MainLoop` — sub_5D2AC0 @ 0x5D2AC0 (decomp L266409–266531)

The entire game is one single-threaded loop. Reconstructed control flow, with
decomp line numbers:

```c
// decomp L266417
for ( exitCode = dword_816358; !dword_816358; exitCode = dword_816358 ) {
    if ( sub_62FD00(1) == 1 )  dword_816358 = 5;      // L266420 window-close check (DXLib key/window state poll)
    keybd_event(7, 0, KEYEVENTF_KEYUP, 0);            // L266424 phantom VK 0x07 every frame (shell-hotkey killer, see as2_constants.h ADDR_GAME_MAINLOOP note)
    ++dword_81635C;                                   // L266427 RENDER-LOOP frame counter (0x81635C)
    sub_561F50(&dword_816358);                        // L266430 input poll (ADDR_INPUT_POLL) — runs even while paused
    if ( sub_63BD10() == -1 )  dword_816358 = 2;      // L266436 DXLib window validity; also pumps messages / focus-idle gate

    if ( !dword_816380 || dword_816384 == 1 ) {       // L266440 pause gate (window-inactive pause)
        sub_61D430(-2);                               // L266443 clear render target
        sub_615670();                                 // L266446 audio system per-frame update
        Input_ProcessGameInput(&dword_816358);        // L266449 sub_562060 — builds word_8E9E62/word_8E9F32 held/edge state
        switch ( dword_81638C ) { ... }               // L266452 mode dispatch (0..13); Mode 8 -> sub_4C8F60
        sub_488DC0(&dword_816358);                    // L266498 post-mode update
        sub_620D90();                                 // L266501 ScreenFlip: flush + StretchRect + Present (section 4)
    }

    // === THE FRAME LIMITER ===  decomp L266504-266510
    while ( (int)(Sys_GetTimeMs() - dword_816360) < 17 )
        ;                                             // pure busy-spin. No Sleep. 100% CPU.
    dword_816360 = Sys_GetTimeMs();                   // last-frame timestamp (ADDR_LAST_FRAME_TIME 0x816360)

    // FPS counter — L266513-266526
    if ( (int)(Sys_GetTimeMs() - dword_81636C) > 1019 ) {
        dword_816364 = dword_81635C - dword_816368;   // FPS value (0x816364)
        dword_816368 = dword_81635C;
        dword_81636C = Sys_GetTimeMs();
    }
}
```

Verified facts about the limiter:

- **Timing API:** `Sys_GetTimeMs` = sub_635F80 = `timeGetTime() & 0x7FFFFFFF`
  (decomp L331545–331550). Not QPC, not GetTickCount. DXLib init calls
  `timeGetDevCaps` + `timeBeginPeriod(wPeriodMin)` (decomp L334190–334191), so
  timeGetTime has ~1 ms resolution for the whole process lifetime.
- **Threshold is integer 17 ms** → native cadence 1000/17 = **58.82 fps**, not 60.
  This is the entire reason the mod's `kFrameLimiter60FpsScale = 17/16.667 ≈ 1.02`
  exists (`src/patches/tick_hooks.cpp` L26–28).
- **Wait style: busy spin.** There is no Sleep and no drift-correcting accumulator.
  `dword_816360` is re-stamped *after* the wait, so time overshoot is **not**
  carried into the next frame: a frame that takes 25 ms just happens, and the next
  frame again gets a full ≥17 ms period. The engine therefore **only slows down**
  when late; it never fast-forwards, never drops sim frames, never drops renders
  on its own.
- Signed 31-bit subtraction makes the comparison wrap-safe across the
  `& 0x7FFFFFFF` mask boundary (24.85-day wrap).
- FPS window constant is 1019 ms (not 1000).

### 1.2 Rendering vs simulation position

Rendering happens **inside the mode handler**, before `sub_620D90()` presents:

- Mode 8 chain: main loop → `sub_4C8F60` @ 0x4C8F60 (decomp banner L117463) →
  substate 3 → `Game_Update_MatchLoop` @ 0x4C9B50 (decomp banner L118083; note the
  refactored file's "Old name: sub_4C4CB0" comment there is one of the label lies —
  the `(004C9B50)` banner address is correct and matches the mod's verified
  `ADDR_MATCH_MODE`/substate handler layout).
- `Game_Update_MatchLoop` structure (decomp L118100–118324) — **this is the
  load-bearing function for all loop control**:

```c
Match_ClearPerFrameTempData(match);                  // L118100 — clears 68B @ match+0x700, ONCE per outer call
if (netplay && timeout>1800) Netplay_IsConnected=0;  // L118101
while ( !Input_TryGetNextFrame(inputBuffer) ) {      // L118103 — SIM LOOP: 0..N iterations
    if ( *(game+56) != 3 ) break;                    // L118105 — bail if substate changed mid-pass
    ... ~60 sim update calls (timers, inputs, entities, collision, effects, audio triggers) ...
    ... round-end / timeout / disconnect checks ...
    *(match+11) = 1;                                 // L118302 — "has simulated once" flag (sticky for the whole match)
    Frame_AdvanceDisplay();                          // L118303 — ++dword_816494
}
Frame_AdvanceSimulation();                           // L118305 — ++dword_816490 (budget grant, ONCE per outer call)
if ( *(match+11) == 1 ) {                            // L118306
    // RENDER PHASE — runs ONCE regardless of how many sim iterations ran,
    // including ZERO iterations (flag is only cleared at match setup, L117731)
    Weather_Draw .. sub_4C47C0 .. sub_4C6B60 (players) .. sub_4C05B0 (HUD) .. sub_4C1F90;
}
```

  **Key structural fact:** sims-per-render is decided entirely by how many times
  `Input_TryGetNextFrame` (sub_5625E0) returns 0 before returning −1. The engine
  natively supports 0 sims + 1 render (flag at match+11 stays 1 → re-renders the
  frozen state) and N sims + 1 render (loop iterates N times). The render phase is
  *not* interpolated — sprites are drawn directly from sim state.

- The same `while(!Input_TryGetNextFrame) { ... Frame_AdvanceDisplay(); } Frame_AdvanceSimulation();`
  pattern exists in CharSel: substate 2 handler sub_5BE7B0 @ 0x5BE7B0 (decomp
  L255186–255272) and substate 4 handler (decomp L256075–256080). All other
  modes/substates read inputs directly and run exactly once per main-loop pass.

### 1.3 Message pump / focus behavior

- `sub_63BD10` → DXLib message pump. On focus loss the pump path stamps
  `dword_9DB6AC = Sys_GetTimeMs()` (decomp L330082, L330223) and
  `sub_634910` @ 0x634910 (decomp L330195–330230, = the mod's
  `ADDR_GAME_MESSAGE_PUMP_IDLE`) **spins until the window is active again**,
  freezing the whole loop. The mod's `netplay_background_run` patch +
  `ADDR_GAME_RUN_IN_BACKGROUND` (0x9E5CA0) bypasses this for netplay.
- Pause gate `dword_816380` (set on window deactivate) skips *logic and render*
  but the limiter still runs.

### 1.4 Input timing

- `sub_561F50` (poll) runs every loop pass even when paused; `Input_ProcessGameInput`
  (sub_562060) refreshes the packed button-word buffers `word_8E9E62` (P1) /
  `word_8E9F32` (P2) once per render pass. Sim frames *sample* those words inside
  `Input_TryGetNextFrame` → `Input_PackButtons`. Consequence: in vanilla code, if
  0 sim frames run for several passes, presses are still latched in the held-state
  words (edge detection happens later, sim-side, in `Input_ProcessRawInput`), but a
  press+release *entirely contained* within a frozen span is lost. The mod's SDL
  input capture (`InputSystem_Update` at `RollbackSession_BeginFrame`) samples once
  per rollback frame and is immune to this.

---

## 2. The timebase sub_635F80 & its callers (the virtual-clock risk surface)

### 2.1 The function itself

```c
//----- (00635F80) ----- decomp L331545
DWORD Sys_GetTimeMs() { return timeGetTime() & 0x7FFFFFFF; }
```

Immediately after it, `Sys_GetTimeMicroseconds` (sub_635F90, decomp L331552–331599)
exists with RDTSC → QPC → timeGetTime fallbacks, but **nothing in the frame path
uses it** — it backs DXLib's internal profiling only. The limiter is pure
timeGetTime.

The mod hooks sub_635F80 with `Hook_GetTick` (`src/patches/tick_hooks.cpp` L191–239,
installed in `src/patches/hook_installer.cpp` L305–314). **Every caller below
therefore sees the virtual clock**, not real time.

### 2.2 Complete caller inventory (grep of `Sys_GetTimeMs` over the decomp)

| decomp line(s) | Function / address | What it uses time for | Virtual-clock impact |
|---|---|---|---|
| L266380, 266506, 266510, 266513, 266525 | `Game_MainLoop` 0x5D2AC0 | **The 17 ms frame limiter** + FPS counter | The intended target. Scale s → real frame period 17/s ms |
| L147075/77/87 | sub_4FF330 0x4FF330 (boot Mode 0) | 100 ms calibration loops (counts rand()/draw iterations) | Boot-time only; ±2% harmless |
| L314329–314343 | sub_6212D0 0x6212D0 | Software-blit path pacer: min-interval wait + `IDirectDraw::WaitForVerticalBlank` (vtbl +68 GetVerticalBlankStatus, +88 WaitForVerticalBlank), `Sleep(1)` loop | **Not used** in the D3D9 hardware path we run (see §4); would double-pace if software mode |
| L322523 | DXLib sound play (0x62Bxxx) | stamps play-start time into sound handle (+632) | Cosmetic bookkeeping; not read by the mixer on the hot path |
| L323500 | DXLib sound play, streaming variant | same (slot[158]) | same |
| L327013, 327068 | sub_630760/sub_630800 0x630760/0x630800 | DirectInput joystick poll/re-acquire scheduling (per-device interval timestamps) | Poll cadence scales with clock; ±2% harmless, but a **backward clock snap stalls re-polling** until real time catches up |
| L327616, 327791 | DXLib system init 0x636D00-region | srand seed + 200 ms init wait; QPC-vs-timeGetTime tick-rate test | Init only (may predate hook install anyway) |
| L330082, 330223 | DXLib message pump / `sub_634910` focus-idle gate | stamps "went inactive" time `dword_9DB6AC` | Cosmetic |
| L331931, 331941 | sub_636610 0x636610 | socket close: up to 2×1000 ms flush wait | Teardown only |
| L333888, 334015 | 0x638xxx text-input draw | IME/typing cursor blink interval (`dword_9D767C`) | Cosmetic |
| L334258 | DXLib init 0x63Cxxx | 100 ms RDTSC frequency calibration spin | Init only |
| L351055, 351118 | movie/AVI streaming update, function @ 0x64D220 (banner L350945) | **17 ms frame-delivery gate** for video playback (struct+136 = last-delivery time); runs partly on a **worker thread** (WaitForSingleObject/ResetEvent around L351063/351119) | Videos (logos, title OPs) play 2% fast under the 1.02 scale — harmless. **But**: this is a second thread calling the hooked function |

### 2.3 Verified risk analysis of the current virtual clock

`Hook_GetTick` (tick_hooks.cpp L191–239): keeps `virtual_tick_ms` (double), adds
`realDelta × effectiveScale` per call, clamps realDelta to 100 ms, floors to DWORD.
`effectiveScale = manual × netplay_current × (1.02 if 60fps correction)` (L64–70).

Failure modes, in order of severity:

1. **Backward snap on reset (the known killer).** With the 60 fps correction on,
   the virtual clock runs permanently 2% *fast*, so it drifts **ahead of real time
   by ~1.2 s per minute of uptime**. `ResetNetplayTickScaleState`
   (tick_hooks.cpp L407–415) memsets the state → `initialized=false` → next call
   re-seeds `virtual_tick_ms = real` (L195–198), i.e. the clock **jumps backward
   by the entire accumulated drift**. But the game's `dword_816360` (and the FPS
   baseline, sound stamps, movie gate, joystick poll stamps) still hold *future*
   virtual timestamps, so the limiter spin `(virtual_now − dword_816360) < 17`
   stays true until real time re-covers the drift: **a hard freeze of ≈ the full
   drift** (10 min of uptime → ~12 s frozen). `ResetNetplayTickScaleState` is
   reachable from `NetplayPacing_ResetSession` (netplay_pacing.cpp L630–645) at
   session start/end/local-mode. Any rebuild that keeps a virtual clock must
   **rebase, never reset**: on state reset preserve `virtual_tick_ms` and only
   reset scales/`last_real_tick_ms`.
2. **Global coupling.** The scale multiplies *everything* in §2.2, not just the
   limiter. Mostly cosmetic today, but every future consumer of sub_635F80 is
   silently retimed. There is no per-callsite selectivity.
3. **Thread safety.** The AVI streaming path (0x64D220) calls the hooked function
   from a worker thread; `NetplayTickState` is unsynchronized doubles/uint32s.
   During movie playback (title screen / boot logos) `last_real_tick_ms`/`virtual_tick_ms`
   can race. Netplay never runs during movies, which is why this has not bitten,
   but a rebuilt scheduler should not inherit the hazard.
4. **Quantization beat.** The limiter compares *floored* virtual ms to integer 17.
   With scale 1.02 the real period alternates sample-to-sample (16/17 ms slices)
   averaging 16.667 ms — fine in aggregate, but combined with vsync (§4.3) it can
   produce a beat pattern.
5. **Slew is per-call, not per-frame.** `SlewScale` (L40–50, 0.0035/step) runs on
   *every* Hook_GetTick call — and the limiter spin calls it thousands of times per
   frame — so "slew" converges essentially instantly during the spin. The EMA that
   actually smooths pacing lives in `netplay_pacing.cpp` (`kAdjustEmaAlpha=0.12`),
   not here.
6. **CPU burn.** The spin was already 100% of one core in vanilla; the hook makes
   every spin iteration do FP math and (rarely) logging. Harmless but wasteful.

What the virtual clock does **right** (keep these properties in any replacement):
scale changes are continuous (no time jumps while scaled), delta clamp of 100 ms
absorbs debugger stalls, and 31-bit masking matches the game's own wrap handling.

---

## 3. Simulation frame counters

### 3.1 The counter set (all reset to 0 by `Netplay_InitialSync`, decomp L203824–203829; mirrored by the mod's `Hook_MatchSyncInit`, input_sync_hooks.cpp L182–219)

| Address | Mod name | Incremented by | Read by |
|---|---|---|---|
| 0x81635C | `ADDR_FRAME_COUNTER` | `Game_MainLoop` L266427, every render pass, never reset | FPS counter only (L266516) |
| 0x816490 | `ADDR_FRAME_SIMULATION` / `ADDR_SIM_FRAME_COUNTER` (aka `Frame_Simulation`) | **`Frame_AdvanceSimulation` sub_562760 @ 0x562760 only** (decomp L203940–203954), once per outer pass of a stepped-mode handler | `Input_TryGetNextFrame` budget check (L203873/203886/203909); netplay delay meters in match HUD sub_4C05B0 (L113043–113064) and charsel HUD sub_5BF490 (L255848–255903) |
| 0x816494 | `ADDR_FRAME_DISPLAY` (`Frame_Display`) | **`Frame_AdvanceDisplay` sub_562750 @ 0x562750 only** (L203932–203937), once per *sim iteration* | round timeout check `== 215900` (L118216); replay end check (L118233); vanilla send index (L203902); HUD meters |
| 0x816498 | `ADDR_INPUT_WRITE_IDX` (`Frame_Inputs`) | `Input_TryGetNextFrame` when it produces an input (L203897/203919) | budget comparisons; the mod writes it directly in the GekkoNet dispatcher (input_override.cpp L2910–2916) |
| 0x81649C | `ADDR_FRAME_NET_IDX` | `Netplay_SendGameplayPacket` (L203720) | vanilla only |
| 0x87FC20 | `ADDR_REMOTE_FRAME` | `Netplay_WaitForPacket` on packet receive (L203684) | vanilla only; HUD meter |

**Semantics** (offline / GAMETYPE 0,1,2,4): each outer pass, `Input_TryGetNextFrame`
produces exactly one input while `Frame_Simulation > Frame_Inputs − 1`, then
returns −1; after the loop `Frame_AdvanceSimulation` grants one more unit of
budget. So the three counters advance in lockstep 1/pass and **sims-per-pass = 
(Frame_Simulation + 1) − Frame_Inputs** at entry. Inflating 0x816490 (or letting it
advance during a freeze) produces a burst of catch-up sims on the next pass — this
is exactly why `Hook_AdvanceFrame` suppression exists (input_sync_hooks.cpp L135–175):
a freeze must suppress **both** the dispatcher (return −1) and the budget grant, or
the engine self-catches-up when released.

Vanilla netplay (GAMETYPE 3): `Frame_AdvanceSimulation` lets `Frame_Simulation` run
ahead of `Frame_Display` by up to 10 frames (input delay window), except when
`Frame_Display % 60 == 0` where it must equal it (hard resync point every 60
display frames) — decomp L203945–203952.

### 3.2 Wrap / overflow behavior

There is **no wraparound handling** — the counters are plain ints reset only by
`Netplay_InitialSync` (per match). The guard is the round timeout: at
`Frame_Display == 215900` the round force-ends (L118216–118220). That number is
not arbitrary: the input history buffers are exactly **216,000 frames**:
`memset(word_8164A0, 0xFF, 0x69780)` = 216,000×2 bytes (L203833), ending exactly at
0x87FC20 (`ADDR_REMOTE_FRAME`), and the P2 buffer 0x87FC24+0x69780 ends exactly at
0x8E93A4. (The IDA-guessed array sizes `[85489]`/`[78836]` at L203635–203636 are
label lies — trust the memset.) At 58.8 fps, 215,900 frames ≈ 61 minutes. Any
rebuilt input timeline that reuses these buffers inherits the 216,000-frame hard
cap per `Netplay_InitialSync` reset; index with `writeIdx < INPUT_HISTORY_MAX`
(216000) as the mod already does (input_override.cpp L2912).

`0xFFFF` in a history slot = "no input yet" — it doubles as the lockstep wait
marker (`Input_GetSyncedInputs` L203774/203779), so raw input value 0xFFFF is
unrepresentable.

---

## 4. Present / vsync behavior

### 4.1 Device creation — what the game asks for

DXLib creates the device in the function at decomp L300000-region (called after
`sub_60F970` @ 0x60F970 loads d3d9.dll and calls `Direct3DCreate9(32)`, L299958–299977):

- `D3DPRESENT_PARAMETERS` assembled at L300057–300105; **PresentationInterval:**
  `v59 = dword_A03088 != 0 ? 0x80000000 : 1;` (L300104) — i.e.
  `D3DPRESENT_INTERVAL_IMMEDIATE` if the "vsync off" flag is set, else
  `D3DPRESENT_INTERVAL_ONE`.
- `dword_A03088` is DXLib's SetWaitVSyncFlag storage: `sub_63B680(a1) { dword_A03088 = a1 == 0; }`
  (L336077–336082), getter sub_63B6A0. Call sites: DXLib init sets **vsync ON**
  (`sub_63B680(1)` at L327219); the only other calls (L329305/329319, L329797/329799)
  are temporary save/restore around internal redraws. **The game itself never turns
  vsync off → the shipping device runs `D3DPRESENT_INTERVAL_ONE`.**
- `CreateDevice` via vtbl+64 with those params at L300194–300201 → device
  `dword_95AB84`; then `GetBackBuffer` → `dword_95AB8C` (L300273) and
  `GetSwapChain(0)` → `dword_95AB88` (L300279).

### 4.2 The flip — sub_620D90 @ 0x620D90 (decomp L314039–314246)

Called once per main-loop pass (L266501). Hardware (D3D9) path, `dword_8FEA64 != 0`:

1. Flush DXLib's batched vertex buffer: `SetFVF` (vtbl+356) + `DrawPrimitiveUP`
   (vtbl+332) with the primitive-count switch at L314146–314181.
2. `StretchRect` (vtbl+136) from the internal render target `dword_95AB90` to the
   backbuffer `dword_95AB8C` (L314188–314198).
3. Present via **`IDirect3DSwapChain9::Present`** (vtbl+12 on `dword_95AB88`),
   windowed with src/dst rects, flags 0 (L314210–314237). *Not* device Present —
   which is why the proxy hooks `IDirect3DSwapChain9::Present` too
   (d3d9_proxy.cpp L5329, `HookedSwapChainPresent` L4584).
4. Software/GDI fallback path (not taken on our setups) uses `sub_6212D0`'s
   DirectDraw `WaitForVerticalBlank` + `StretchDIBits` (L314242–314291).

There is **no CPU-side vsync wait in the D3D path** — blocking happens inside
Present when the frame queue is full (interval ONE).

### 4.3 What the mod's d3d9 proxy actually presents

`d3d9_proxy/d3d9_proxy.cpp`:

- `CreateDevice` is passed through with the game's params but **forced
  `Windowed=TRUE`** for borderless (L5630–5645). PresentationInterval is *not*
  overridden — the game's `INTERVAL_ONE` survives (and both original and final
  params incl. `Interval=` are in the proxy log, L5570/L5653).
- In scaled mode, `HookedSwapChainPresent`/`HookedPresent` **skip the game's
  present entirely**: ColorFill bars → `StretchRect` 640×480 → scaling backbuffer →
  present the proxy-owned **scaling swap chain** (`g_pScalingSwapChain->Present(..., 0)`,
  L4533) created with `PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT`
  (L2307) — DEFAULT ≙ ONE. So in practice: **one vsynced windowed present per
  main-loop pass, on the proxy's chain; the proxy is the single owner of the
  present-interval decision** and can change it without touching the game.
- Minimized: present is skipped and replaced by `Sleep(16)` (L4380–4383, L4616–4619)
  — during netplay the sim keeps pacing off the limiter, not off Present.
- Device-lost: 50 ms throttle + recovery (L4371–4377).

### 4.4 Behavior when a frame is late

Combining §1.1 and §4.1–4.3, the engine's late-frame behavior is:

- Sim+render over 17 ms → the limiter spin evaluates true immediately; the frame
  simply ships late; **the next period restarts from "now"** (no debt carried).
  Game time falls behind real time 1:1 — pure slowdown, no skips, no catch-up.
- In windowed mode with a compositor (our case), a missed vblank costs up to one
  refresh of queue latency but does not stall the loop hard the way exclusive
  fullscreen interval-ONE would.
- On a 60 Hz display, the vsynced present (≈16.67 ms) is *tighter* than the 17 ms
  limiter, so **Present is the real pacer at native 58.8→60 fps settings**, and
  with the 1.02 tick correction the limiter target (16.667 ms) sits exactly on the
  vblank cadence — two pacers at the same nominal rate, which is where beat-pattern
  microstutter comes from when either one jitters. On >60 Hz displays the limiter
  is the only pacer.

---

## 5. Pacing options for the rebuilt netcode

What the netcode needs to modulate, per the debt controller in
`src/net/netplay_pacing.cpp`: continuous ±6% frame-period adjustment
(`kScaleMin/Max` 0.94–1.06, ~±1 ms via `kMaxAdjustMs=0.90`), plus discrete holds
(soft/hard/stall = 0-sim frames) — all without visible jitter.

### Option (a): run-N-ticks-per-render loop control

Drive speed purely by sims-per-pass (0/1/2) through the dispatcher hook, leaving
the 17 ms limiter untouched.

- **Engine fit:** perfect for *discrete* corrections — the `while(!Input_TryGetNextFrame)`
  loop is designed for it (§1.2), the mod already ships it (`s_doDoubleTick`
  catch-up in input_override.cpp L2883–2977; hold = return −1 + AdvanceFrame
  suppression). Substate-change bailout (L118105) makes mid-pass transitions safe.
- **Why it can't be the whole answer:** granularity is a full frame. A 2-sim pass
  presents 33 ms of motion in one refresh; a 0-sim pass shows a repeated frame.
  Used *continuously* for rate matching (e.g. one extra/skipped sim every N frames)
  it is textbook judder — this is precisely the jitter the rebuild must avoid.
  Reserve (a) for rollback resim and rare timesync corrections, rate-limited the
  way the current debt controller does (pressure/hold-cost, min gap between holds).

### Option (b): replace the limiter with a real frame scheduler (recommended)

Detour the busy-spin at the bottom of `Game_MainLoop` (locate by byte signature:
the only `call sub_635F80 / sub / cmp 17 / jl` cluster in 0x5D2AC0; the decomp
shows it as L266504–266510) and substitute a mod-owned `WaitForNextFrame()`:

```
period_us = 16667 (or 17000 for compat mode) + pacing_adjust_us   // from netplay_pacing EMA, clamped ±1000
deadline += period_us                                             // absolute accumulator (QPC)
if (now > deadline + 2*period) deadline = now                     // resnap after stall (load, debugger, device-lost)
sleep_until(deadline - 1.5ms) via Sleep(1) loop; spin the rest    // timeBeginPeriod(1) already active (§2.1)
```

- **Pros, against engine reality:**
  - Absolute deadlines mean **zero cumulative drift** and sub-ms period control —
    a 60.000 fps target needs no 1.02 hack, so the *entire* backward-snap failure
    class (§2.3 #1) disappears; `Hook_GetTick` can stay installed but pinned to
    scale 1.0 (or removed), returning real time to all §2.2 consumers.
  - Pacing becomes local to one callsite; audio stamps, movie gate, joystick poll,
    worker threads all untouched (§2.3 #2/#3 solved).
  - Late frames inherit the vanilla behavior (resnap = slow down, never burst) —
    matching the "no catch-up unless commanded" requirement.
  - CPU drops from 100% spin to ~2% (Sleep + short spin).
- **Cons / obligations:**
  - Must also neutralize the vanilla comparison (patch the `jl` or keep feeding it
    a satisfied condition) and keep `dword_816360`/FPS bookkeeping coherent —
    simplest is to run the detour *after* letting the game stamp `dword_816360`.
  - Two pacers problem (§4.4) remains: with vsync ONE on a 60 Hz display, Present
    blocks too. Resolution: the scheduler's deadline math must treat Present's
    block as part of frame cost (it already does, being absolute), and the proxy
    should expose a config to create the scaling chain with
    `D3DPRESENT_INTERVAL_IMMEDIATE` (change one line, d3d9_proxy.cpp L2307) for
    users whose refresh ≠ 60 Hz multiples or who see beat stutter. Default can
    stay DEFAULT/ONE: windowed DWM present at 60 Hz + 16.667 ms scheduler is the
    lowest-jitter combination available.
  - The sleep granularity risk is bounded by the hybrid sleep/spin; with
    timeBeginPeriod(1) worst-case oversleep ≈ 1–2 ms, absorbed by the spin tail.

### Option (c): keep the tick-scale virtual clock

Status quo (`Hook_GetTick` + `SetNetplayTickScaleTarget` slew).

- **Pros:** shipped, battle-tested through 0.6; zero binary patching of the loop;
  continuous scaling already integrates with the debt controller; the 17 ms integer
  limiter becomes a non-issue because the clock lies to it.
- **Cons (documented failure modes, §2.3):** permanent 2% drift → **backward snap
  freezes on every state reset** unless reset semantics are changed to rebase;
  global retiming of 20+ unrelated consumers; a cross-thread race during movie
  playback; scale quantization beat against vsync; per-call slew being effectively
  instantaneous; 100% CPU spin retained.
- If (c) is kept short-term, the two mandatory fixes are: (1) `ResetNetplayTickScaleState`
  must preserve `virtual_tick_ms` continuity (rebase `last_real_tick_ms = current real`,
  keep the virtual accumulator; never re-seed from real), and (2) add a mutex or
  confine the hook math to `GetCurrentThreadId()==main` with passthrough for other
  threads.

### Recommendation

**(b) + (a): a deadline scheduler at the limiter site for continuous rate control,
with dispatcher-driven 0/2-sim passes reserved for discrete holds and rollback
catch-up.** Evidence-based reasoning:

1. The engine has exactly one pacing point (the spin, §1.1) and exactly one
   sims-per-render decision point (the dispatcher loop, §1.2). Owning both natively
   is strictly more precise than lying to them through the clock.
2. Every observed jitter/freeze mechanism traces to the virtual clock's side
   effects (backward snap, quantization beat, global coupling) — not to the
   engine, whose own behavior when late is benign pure-slowdown (§4.4).
3. The ±6%/±0.9 ms adjustments the debt controller emits map 1:1 onto
   `period_us += filtered_adjust_ms*1000` with *no* clamp interactions, and
   "hard_hold" maps onto the existing freeze machinery unchanged
   (`NetplayPacing_*` API can be kept verbatim; only the sink changes from
   `SetNetplayTickScaleTarget` to `Scheduler_SetPeriodAdjust`).
4. Keep `Hook_GetTick` installed but neutral (scale pinned 1.0) during the
   transition so the 58.8-fps compat toggle (`proper_60fps=0`) can still be honored
   by the scheduler period instead (17000 µs vs 16667 µs).

---

## 6. Skipping and doubling sim frames safely

Everything here is grounded in the loop structure of §1.2 and verified mod
mechanisms.

### 6.1 Running 0 sims per render (hold / freeze)

**Mechanism:** dispatcher (sub_5625E0 hook) returns −1 before producing a frame
**and** `Frame_AdvanceSimulation` (sub_562760 hook) is suppressed for the same
pass. Both halves are mandatory — suppressing only the dispatcher lets 0x816490
run ahead and the engine bursts the deficit as multiple sims on release (§3.1).
This is exactly `Hook_AdvanceFrame` + `Hook_InputDispatcher` returning −1 today
(input_sync_hooks.cpp L135–175; input_override.cpp L1903–1941, L3030–3044).

**What keeps working:** render phase re-renders the frozen state every pass
(match+11 stays 1, §1.2); Present keeps firing (window stays responsive, overlay
alive); packet pumping continues (`Net::Session_Update` from the dispatcher);
vanilla timeout counters must keep being zeroed (`ResetVanillaTimeouts`,
0x8EA200/0x8EA3A8) or the 1800-frame disconnect at L118101 fires.

**What breaks / caveats:**
- *Audio:* nothing stops — BGM loops, already-triggered SEs finish. Only sim-driven
  triggers (Match_UpdateAnnouncer L118165, Entity_UpdateAudio L118166, SE_Play
  0x4C3C00 calls inside entity updates) pause with the sim. No repair needed.
- *Input latching:* vanilla path can drop a tap fully contained in the hold (§1.4).
  The rebuilt netcode must keep sampling local input **once per rollback frame at
  BeginFrame from the SDL layer**, not from the game's packed words, exactly as
  the GekkoNet path does now.
- *Round timer & timeout:* both are display-frame-driven (L118107, L118216), so
  they freeze with the sim — consistent, nothing drifts.
- *Do not hold via the pause flag `dword_816380`* — that skips render+audio update
  too and is owned by focus handling.

### 6.2 Running 2+ sims per render (catch-up)

**Mechanism (proven, shipping):** after the dispatcher's frame batch completes,
schedule a second `RollbackSession_BeginFrame` → the dispatcher returns 0 again →
the game's while-loop runs a second full sim iteration before the single render
(`s_doDoubleTick`, input_override.cpp L2883–2977). Rollback resim is the same
mechanism at higher N: each GekkoNet Advance event returns 0 for one more loop
iteration until Done (L2875, L2986). The offline resim engine
(`src/rollback/resimulation.cpp` L295–325) instead calls the Mode-8 handler
(0x4C8F60) N times directly — note it must clear the per-frame temp block itself
(L306–310) because `Match_ClearPerFrameTempData` runs only at the top of the
*outer* function, not between while-iterations (as2_constants.h L147–156).

**What breaks / caveats, per subsystem:**

- **Audio — SE duplication within a pass is naturally suppressed; across passes it
  is not.** The 68-byte per-frame temp block at match+0x700 doubles as the
  "SE already active this pass" byte array (rollback_audio.cpp `SoundActiveBytes()`
  = `ADDR_MATCH_PER_FRAME_TEMP`, `VanillaWouldPlayNow` checks byte==0). Because it
  is cleared once per outer pass, two sims in one pass fire each distinct SE id at
  most once — double-tick sounds correct *for free*. Rollback resim (which replays
  frames whose SEs already played in an earlier pass) needs the event-reconciliation
  layer that `rollback_audio.cpp` provides (committed/pending/corrected event lists,
  stand-in window). Any rebuilt catch-up must keep both behaviors: leave the temp
  block's once-per-pass clear for stacked ticks, route resim through the audio
  reconciler.
- **Input latching:** each sim iteration consumes one input frame. The catch-up
  tick must be fed a *fresh* sampled input (`InputSystem_Update` before the second
  BeginFrame, L2886), never a duplicate of the previous frame's — duplicating
  locally while the remote consumes real inputs is a desync.
- **Render interpolation: none exists.** The single render after a 2-sim pass
  shows a 2-frame position jump. One isolated double-tick is invisible; sustained
  doubling is 30 fps-equivalent judder. Rate-limit exactly as the debt controller
  does (soft-hold pressure cost, `min_soft_hold_gap_rb_frames`), and prefer the
  §5(b) scheduler's continuous period stretch for anything below ~1 frame of
  correction.
- **Round/mode transitions mid-burst are safe:** the loop re-checks
  `*(game+56) != 3` every iteration (L118105) and round-end paths set
  substate/mode via the same fields, so a burst that crosses a KO simply stops
  early. Do not continue a resim burst across a substate change — the remaining
  frames belong to a different handler.
- **Display-frame side effects scale with N:** round timer decrements N times,
  timeout counter approaches 215900 N times — all deterministic and mirrored on
  the remote, no correction needed.
- **Frame budget:** N sims + 1 render must fit in the (16.67 ms) period or the
  pass itself becomes the jitter. Empirically sim iterations are ~1–2 ms; cap
  bursts (GekkoNet rollback budget / `s_hardThreshold = min(profile, budget−1)`,
  netplay_pacing.cpp L697) so worst case rollback+resim stays under ~10 ms.
- **Skipping the render (fast-forward, spectator seek):** cannot be done from the
  dispatcher (render phase is outside the loop), but *can* be done by (1) calling
  the Mode-8 handler directly as resimulation.cpp does — its render phase draws to
  the DXLib target but costs only draw-call time — or (2) letting the proxy drop
  the Present. Never skip `sub_620D90`'s batch flush while skipping presents for
  long spans without bounding the batch, and never skip sim-side per-frame temp
  clears.

### 6.3 Rules of thumb for the rebuild

1. One decision point: choose sims-for-this-pass ∈ {0, 1, 2} **before** the pass
   (dispatcher BeginFrame), never mid-pass.
2. Every 0-sim pass = dispatcher −1 **and** AdvanceFrame suppressed **and**
   vanilla timeout counters cleared.
3. Every extra sim = fresh input sample + write-through to both history buffers +
   write-idx bump (bounded by 216,000, §3.2).
4. Continuous rate error < ±0.06 frames/frame → scheduler period adjust;
   discrete error ≥ 1 frame → hold/double with hysteresis; rollback → burst inside
   one pass, capped by budget.
5. Never reset a clock/scheduler accumulator to "now" while any game-side
   timestamp may be ahead of it (§2.3 #1) — rebase monotonically.

---

## 7. Appendix: address quick reference

| Thing | Address | Decomp anchor |
|---|---|---|
| Game_MainLoop | 0x5D2AC0 | L266409 |
| Frame limiter spin (17 ms, timeGetTime) | inside 0x5D2AC0 | L266504–266510 |
| Sys_GetTimeMs (`timeGetTime & 0x7FFFFFFF`) | 0x635F80 | L331545 |
| Sys_GetTimeMicroseconds (RDTSC/QPC, unused by loop) | 0x635F90 | L331552 |
| timeBeginPeriod(min) at init | 0x63Cxxx | L334190 |
| ScreenFlip (flush + StretchRect + SwapChain::Present) | 0x620D90 | L314039 |
| SW-path vsync wait (DDraw WaitForVerticalBlank) | 0x6212D0 | L314322 |
| SetWaitVSyncFlag storage (`dword_A03088`) | 0x63B680 setter | L336077; init ON at L327219 |
| CreateDevice w/ `PresentationInterval = A03088 ? IMMEDIATE : ONE` | 0x60Fxxx | L300104, L300194 |
| Mode-8 handler | 0x4C8F60 | banner L117463 |
| Game_Update_MatchLoop (sim loop + render phase) | 0x4C9B50 | banner L118083 |
| Input_TryGetNextFrame (dispatcher) | 0x5625E0 | L203859 |
| Frame_AdvanceDisplay / Frame_AdvanceSimulation | 0x562750 / 0x562760 | L203932 / L203940 |
| Netplay_InitialSync (counter+history reset) | 0x562550 | L203801 |
| CharSel stepped loops | 0x5BE7B0 / 0x5C0030 | L255198 / L256075 |
| Sim/display counters | 0x816490 / 0x816494 | §3 |
| Input write/net idx, remote frame | 0x816498 / 0x81649C / 0x87FC20 | §3 |
| Input histories (216,000 frames each) | 0x8164A0 / 0x87FC24 | L203833–203837 |
| Round-timeout constant | 215900 | L118216 |
| Focus-idle spin gate | 0x634910 | L330195 |
| Movie 17 ms delivery gate (worker thread!) | 0x64D220 | banner L350945, L351055 |
| Netplay delay HUD meters (read sim/display gap) | 0x4C05B0 / 0x5BF490 | L113043 / L255848 |

*Decomp line numbers refer to `d:\dev\alice_senki\alicesenki2_decomp_refactored.c`
as of 2026-08-17.*
