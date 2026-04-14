# Alice Senki 2 Improvement Mod

A comprehensive runtime modification for Alice Senki 2, a doujin 2D fighting game. This mod adds rollback-based online netplay, a modern SDL3 input system, an ImGui debug overlay, training mode tools, a spectator system, custom palette support, NAT traversal, and an extensible mod loader. Built entirely as a DLL proxy stack that requires no modifications to the original game executable.

---

## Table of Contents

- [Runtime Architecture](#runtime-architecture)
  - [DLL Proxy Stack](#dll-proxy-stack)
  - [Boot Sequence](#boot-sequence)
  - [Lifecycle](#lifecycle)
- [Implemented Systems](#implemented-systems)
- [Networking and Netplay](#networking-and-netplay)
  - [Session Management](#session-management)
  - [Protocol](#protocol)
  - [Pre-Game Synchronization](#pre-game-synchronization)
  - [Character Select Lockstep](#character-select-lockstep)
  - [Stage Select Sync](#stage-select-sync)
  - [Match Bootstrap](#match-bootstrap)
  - [Delay Policy and Negotiation](#delay-policy-and-negotiation)
  - [Sync Policy](#sync-policy)
  - [Win Screen Sync](#win-screen-sync)
  - [Netplay Pacing](#netplay-pacing)
  - [NAT Traversal](#nat-traversal)
  - [Netplay Menu](#netplay-menu)
  - [Spectator System](#spectator-system)
  - [Custom Palette Sync](#custom-palette-sync)
  - [Network Thread](#network-thread)
  - [Pause Handling](#pause-handling)
  - [Set Tracker](#set-tracker)
- [Rollback Engine](#rollback-engine)
  - [Rollback Session](#rollback-session)
  - [Savestate System](#savestate-system)
  - [Resimulation](#resimulation)
  - [Input Timeline](#input-timeline)
  - [Prediction](#prediction)
  - [Frame Lineage](#frame-lineage)
  - [Determinism Verification](#determinism-verification)
  - [Rollback Debug and Diagnostics](#rollback-debug-and-diagnostics)
  - [Desync Dump](#desync-dump)
  - [Stress Testing](#stress-testing)
- [Game Patches](#game-patches)
  - [Hook Installer](#hook-installer)
  - [Input Override](#input-override)
  - [Input Sync Hooks](#input-sync-hooks)
  - [Tick Hooks](#tick-hooks)
  - [Filesystem Patch](#filesystem-patch)
  - [Locale Patch](#locale-patch)
  - [Unlock Patch](#unlock-patch)
  - [Palette Asset Hook](#palette-asset-hook)
  - [Character Select Palette Selection](#character-select-palette-selection)
  - [Memory Utilities](#memory-utilities)
- [Input System](#input-system)
  - [SDL3 Integration](#sdl3-integration)
  - [Binding System](#binding-system)
  - [SOCD Cleaning](#socd-cleaning)
  - [Override and Netplay Input](#override-and-netplay-input)
- [User Interface](#user-interface)
  - [Mod Menu](#mod-menu)
  - [Netplay HUD](#netplay-hud)
  - [Hitbox Viewer](#hitbox-viewer)
  - [Log Window](#log-window)
  - [Palette Editor](#palette-editor)
- [Replay System](#replay-system)
  - [Replay Runtime](#replay-runtime)
  - [Replay Takeover](#replay-takeover)
- [Training Mode](#training-mode)
- [Testing Infrastructure](#testing-infrastructure)
  - [Scripted Input Runner](#scripted-input-runner)
  - [Test Scenarios](#test-scenarios)
  - [Autoconnect Harness](#autoconnect-harness)
  - [Unit Tests](#unit-tests)
  - [Test Harness Launcher](#test-harness-launcher)
- [D3D9 Graphics Proxy](#d3d9-graphics-proxy)
  - [Letterbox Scaler](#letterbox-scaler)
- [Winsock Proxy](#winsock-proxy)
- [Mod Loader](#mod-loader)
  - [Bootstrap](#bootstrap)
  - [Priority Rules](#priority-rules)
  - [Folder Rules](#folder-rules)
  - [Optional DLL Mods](#optional-dll-mods)
  - [Example Layout](#example-layout)
- [Game State Reference](#game-state-reference)
  - [Game Modes](#game-modes)
  - [Match Substates](#match-substates)
  - [Character Select Substates](#character-select-substates)
  - [Entity Memory Layout](#entity-memory-layout)
  - [Hitbox System](#hitbox-system)
- [Building](#building)
  - [Prerequisites](#prerequisites)
  - [Dependencies](#dependencies)
  - [Configure](#configure)
  - [Build](#build)
  - [Install or Deploy](#install-or-deploy)
- [Required Runtime Files](#required-runtime-files)
- [Hotkeys](#hotkeys)
- [Configuration Files](#configuration-files)
- [Test Harness](#test-harness)
- [Project Map](#project-map)
- [Troubleshooting](#troubleshooting)
- [License and Disclaimer](#license-and-disclaimer)

---

## Runtime Architecture

### DLL Proxy Stack

The mod operates as a chain of proxy DLLs that intercept the game's normal library loading. No bytes of the original executable are modified on disk.

| DLL | Role | Loaded By |
|-----|------|-----------|
| `wsock32.dll` | Early-start proxy. Bypasses the duplicate-instance gate, suppresses shell hotkey and IME interference, traces focus-steal attempts. Forwards all 16 winsock functions to the real system `wsock32.dll`. | Game executable (static import) |
| `d3d9.dll` | Graphics proxy. Wraps the real system D3D9, owns the ImGui overlay, manages the letterbox scaler for aspect-ratio-correct upscaling, loads SDL3, loads `as2_rollback.dll`, then loads user DLL mods from `mods/`. | Game executable (static import via `Direct3DCreate9`) |
| `as2_rollback.dll` | Main mod runtime. Installs 25+ MinHook-based hooks covering input, timing, filesystem, locale, palette asset loading, and vanilla netplay suppression. Contains all gameplay, networking, rollback, training, and UI systems. | `d3d9.dll` (dynamic load) |
| `SDL3.dll` | SDL3 runtime for modern input handling. | `d3d9.dll` (dynamic load) |

### Boot Sequence

1. **Process start** -- Windows loads `wsock32.dll` (static import). The proxy patches `FindWindowA` in the game's IAT to always return `NULL`, enabling multiple simultaneous instances. It sets the DXLib duplicate-instance flag (`dword_9DB884 = 1`), applies shell hotkey suppression bypasses, and begins IME call tracing.
2. **D3D9 initialization** -- The game calls `Direct3DCreate9`. The proxy `d3d9.dll` intercepts this, loads the real system `d3d9.dll`, creates a wrapped D3D9 device, initializes ImGui, loads `SDL3.dll`, loads `as2_rollback.dll`, and finally loads user mod DLLs.
3. **Mod initialization** -- `as2_rollback.dll`'s `ModInit()` caches the game module handle and reads autoconnect configuration. Actual subsystem initialization is deferred.
4. **First frame** -- On the first call to `ModOnFrame()`, `DeferredInit()` runs. This initializes all subsystems: input system, game console capture, hook installation, unlock patch, filesystem/locale patches, rollback session, networking, practice tools, determinism verification, savestate, spectator, palette runtime, and more.
5. **Steady state** -- Every frame: `ModOnFrame()` updates all subsystems. `ModOnPresent()` renders the ImGui overlay.
6. **Shutdown** -- `ModShutdown()` tears down all subsystems in reverse order. `ModOnGameExit()` logs exit conditions.

### Lifecycle

```
ModInit(gameModule)       -- DLL load, cache config
  |
  v
DeferredInit()            -- First OnFrame, all subsystems start
  |
  v
ModOnFrame()              -- Per-frame update loop (input, net, rollback, UI)
ModOnPresent()            -- Per-frame render (ImGui overlay, HUD)
  |
  v
ModShutdown()             -- Cleanup all subsystems
ModOnGameExit()           -- Log exit
```

---

## Implemented Systems

| System | Status | Description |
|--------|--------|-------------|
| Proxy DLL stack | Complete | wsock32 + d3d9 proxy chain with early patches |
| SDL3 input | Complete | Keyboard + gamepad with unified binding, SOCD cleaning, background input |
| ImGui overlay | Complete | Tabbed menu with input config, debug tools, logs, savestates, netplay HUD |
| Filesystem patches | Complete | CP932/Shift-JIS filename support, mod file override system |
| Locale patches | Complete | Force Japanese codepage (CP932) on non-Japanese Windows |
| Unlock patch | Complete | All characters, boss, gallery, stage select unlocked |
| Palette system | Complete | Custom palette loading, asset hook injection, catalog sync, live preview |
| Session management | Complete | ENet transport, handshake, connection stats, graceful disconnect |
| Pre-game sync | Complete | Character select lockstep, stage select merge, config exchange, baseline agreement |
| Match bootstrap | Complete | Config negotiation, load barrier, baseline CRC verification, gameplay handoff |
| Rollback engine | Complete | GekkoNet integration, savestate ring buffer, resimulation, prediction |
| Determinism tools | Complete | RNG hooking, FPU capture, A/B comparison, per-frame checksum, desync dump |
| Delay negotiation | Complete | RTT-based recommendation, configurable visible delay, hidden delay floor |
| Adaptive pacing | Complete | Frame-rate scaling to smooth rollback jitter |
| NAT traversal | Complete | UPnP, STUN, PCP/NAT-PMP, TURN relay, UDP hole punch, IPv6 |
| Spectator system | Complete | Server + client + local playback, frame archive, palette propagation, relay/LAN discovery, fast-forward/hard-sync |
| Netplay menu | Complete | Full in-game menu for hosting, joining, spectating, settings |
| Replay tools | Complete | Replay browser controls, pause/step/rewind, HUD, takeover |
| Practice tools | Complete | Pause, frame-step, control swap, hitbox viewer |
| Hitbox viewer | Complete | Collision, attack, hurt, throw, projectile box rendering |
| Mod loader | Complete | Ordered file override and DLL mod loading |
| Test harness | Complete | Two-instance autoconnect, network simulation, fighting AI, shared memory dashboard |
| Letterbox scaler | Complete | Aspect-ratio-correct upscaling with offscreen render target |

---

## Networking and Netplay

### Session Management

The session manager (`session_manager.cpp`, ~550 lines) implements a connection state machine:

```
Idle -> Connecting -> Handshaking -> Connected -> Ready -> Disconnecting -> Idle
                                                       \-> Failed (terminal)
```

**Roles:** Host or Join (client).

**Handshake:** Uses `Hello`/`HelloAck` packets carrying protocol version, an exact local mod build fingerprint, and nickname (24 bytes max). Protocol or build mismatches trigger immediate disconnect before gameplay setup.

**Deferred packets:** Up to 64 control packets are buffered before the packet callback is registered, then flushed on registration.

**Connection stats** track RTT, RTT variance, packets sent/received/lost, and bytes sent/received.

### Protocol

Defined in `protocol.h` (457 lines). Protocol version 6 with 40+ packet types.

**Wire format:**
- Max packet size: 1200 bytes (MTU-friendly)
- Max payload: 1198 bytes
- All payloads are `#pragma pack(push, 1)` for wire compatibility

**ENet channels:**

| Channel | Name | Mode | Purpose |
|---------|------|------|---------|
| 0 | CHANNEL_CONTROL | Reliable ordered | Handshake, session control, config exchange |
| 1 | CHANNEL_GAMEPLAY | Unreliable sequenced | Gameplay input (rollback/lockstep) |
| 2 | CHANNEL_DEBUG | Unreliable | Diagnostics, state digests, frame sync |

**Packet types (grouped):**

- **Session control:** Hello, HelloAck, Ready, Disconnect, SessionMeta
- **Initial sync:** SyncAnnounce, SyncConfirm
- **NAT:** NatInfo, NatTraversalSignal
- **Pre-game:** CharSelInput, CharSelLock, StageSync, ConfigExchange, ConfigAck, LoadBarrier, BaselineReady, BaselineDigest, BaselineBreakdown, GameplayStart, GekkoReady
- **Lockstep:** CharSelFrameInput, WinScreenFrameInput
- **Palette:** PaletteConfig, PaletteData, PaletteAck
- **Debug:** Ping, Pong, StateDigest, FrameSyncStatus

**Key payloads:**
- `HelloPayload`: Protocol version, exact local mod build fingerprint (uint32), nickname[64], listen port
- `ConfigExchangePayload`: Character IDs, palettes, stage, round count, time limit, RNG seed, session seed, delay/rollback configuration
- `BaselineDigestPayload`: CRC32 of baseline savestate for agreement verification
- `BaselineBreakdownPayload`: Per-region CRCs (main, header, context, effects, summons, entities, input buffers) for mismatch diagnosis
- `CharSelFrameInputPayload`: Frame number, ack frame, 8-slot input history with redundancy for packet loss recovery

### Pre-Game Synchronization

The pre-game sync (`pregame_sync.cpp`) manages the entire flow from connection to gameplay start through a 13-phase state machine:

```
Idle -> SyncAnnounce -> SyncExchange -> SyncConfirmed
     -> FrontendCharSel -> FrontendStageSel -> FrontendLocked
     -> ConfigExchange -> ConfigAgreed
     -> BootstrapLoading -> BootstrapBaseline -> BootstrapReady
     -> GameplayHandoff
```

**Phase timeouts:** 10s for sync/config, 30s for loading, 15s for baseline.

Each phase dispatches incoming packets through `OnPregamePacket()`, which routes to the appropriate handler (SyncAnnounce, SyncConfirm, CharSelInput, CharSelLock, StageSync, ConfigExchange, ConfigAck, LoadBarrier, BaselineReady, BaselineDigest, BaselineBreakdown, GameplayStart, GekkoReady).

### Character Select Lockstep

Character select (`charsel_sync.cpp`, 765 lines) uses non-blocking lockstep synchronization:

- **Ring buffer:** 512-frame circular buffer for local and remote inputs
- **Input redundancy:** Each packet carries 8 previous frames for packet loss recovery
- **Input delay:** 2-10 frames, computed from RTT on first capture (clamped)
- **Timeout:** 10 seconds without new remote input
- **Resend interval:** 100ms periodic retransmission while waiting

The lockstep model:
- Three frame counters: `consumeFrame` (both sides have data), `localInputFrame` (latest local), `remoteLatestFrame` (latest remote)
- Host maps to P1, client maps to P2
- Send-ahead cap prevents buffering more than half the ring size ahead of consume
- Character locks and stage selections are sent as separate reliable packets

### Stage Select Sync

Stage select (`stagesel_sync.cpp`, 152 lines) uses a pure merge function with no internal state:

1. OR all inputs from P1 and P2
2. Cancel opposing directions (Left+Right -> 0, Up+Down -> 0)
3. Strip SELECT button (prevents unsynchronized roulette triggered by `rand()`)
4. Record in audit ring (128 entries) for diagnostics

Identical inputs always produce identical outputs -- deterministic by design.

### Match Bootstrap

The match bootstrap (`match_bootstrap.cpp`) manages the transition from pre-game to gameplay through 6 phases:

```
Idle -> ConfigExchange -> Loading -> Baseline -> Ready -> Done
```

**Config exchange:** Both peers exchange `ConfigExchangePayload` containing character IDs, palettes, stage, round count, time limit, RNG seed, and delay/rollback settings. Agreement is verified by CRC32 hash.

**Load barrier:** Both peers report when they've reached Mode 8 (match mode). A 30-second timeout prevents indefinite waiting. The game loop continues running (for rendering) but frame advancement is frozen.

**Baseline agreement:** After loading, both peers capture a savestate and exchange per-region CRC32 digests. If digests match, gameplay starts. If they don't, a detailed breakdown is exchanged and a mismatch dump is written to disk.

**Gameplay start:** The host sends a `GameplayStart` packet with the bootstrap frame number, and both peers synchronize their frame counters.

### Delay Policy and Negotiation

The delay policy (`delay_policy.cpp`, 416 lines) manages input delay and rollback budget:

**Constants:**
- Default visible delay: 0 frames (user-configurable 0-15)
- Hidden gameplay delay floor: 2 frames (always-on, invisible to the user)
- Default rollback budget: 7 frames
- Default rollback tolerance (K): 1

**Recommendation model:**
```
L = avg_ping_ms / 2 / 16.667       (one-way latency in frames)
delay = max(0, floor(L) - K)       (recommended visible delay)
max_rb = max(4, ceil(L) - delay + 2)  (recommended max rollback)
```

**Effective delay** = configured visible delay + hidden floor (2). Both peers exchange their delay configuration during the ConfigExchange phase.

**Derived thresholds:**
- Connection protection window = effective local delay + rollback budget
- Stall threshold = effective remote delay + rollback budget

### Sync Policy

The sync policy (`sync_policy.cpp`, 309 lines) classifies the current game state to determine which synchronization mode to use:

| Sync Mode | When | Description |
|-----------|------|-------------|
| Lockstep | CharSel sub 2/4, StageSel sub 7/8, Mode 9 (winscreen), Pause | Both peers step together frame-by-frame |
| Rollback | Mode 8 sub 3 (playable gameplay) | Full rollback with prediction and resimulation |
| Passive | Mode 7 (pre-match intro), Mode 8 subs 0-2/5, transitions | No sync needed, local progression only |

**Confirm arm logic** prevents double-confirm from held buttons:
1. Neutral frame -> arm
2. Armed + rising edge -> consume (fire)
3. Held = ignore

**Stage select enforcement:** Forces `ADDR_STAGESEL_ENABLE = 1` during netplay to ensure stage select is always available.

### Win Screen Sync

Win screen sync (`winscreen_sync.cpp`, 447 lines) mirrors the character select lockstep model for Mode 9:

- Ring buffer: 512 frames, 1-6 frame delay, 8-frame redundancy
- Advance detection: Checks A, C, or START button for win screen confirmation
- Both-confirmed tracking: Both peers must confirm before proceeding
- Timeout: 10 seconds, resend interval: 100ms

### Netplay Pacing

Adaptive frame-pacing (`netplay_pacing.cpp`, 313 lines) smooths out rollback jitter by dynamically adjusting the game's tick rate:

**Controller parameters:**
- Base frame time: 16.6667ms (60fps)
- Deadband: 0.08 frames (no correction within this range)
- Gain: 0.55ms per frame-ahead
- Max adjustment: 0.90ms
- EMA alpha: 0.12 (smoothing factor)
- Scale bounds: 0.94x - 1.06x

**Stall detection:** When the gap between local and remote frames exceeds the stall threshold, the pacing controller returns a `StallHold` action to freeze the local simulation until the remote catches up.

### NAT Traversal

Full NAT traversal support (`nat_traversal.cpp`, 250+ lines) with multiple backends:

| Method | Library | Description |
|--------|---------|-------------|
| UPnP | miniupnpc | Port mapping via router UPnP |
| PCP / NAT-PMP | libpcpnatpmp | Port mapping fallback |
| STUN | libjuice | Server-reflexive address discovery |
| TURN | libjuice | Relay-based connectivity |
| Hole punch | Built-in | UDP hole punch burst via ENet |

**Features:**
- IPv6 endpoint support with bracket notation
- Proton/Wine detection for compatibility
- Async worker thread for NAT operations
- Configurable timeouts for gather, connect, and mapping operations
- Signal queue for ICE candidate/SDP exchange

### Netplay Menu

The netplay menu system spans two files:

**Controller** (`netplay_menu_controller.cpp`): 16-state menu state machine with INI-based configuration persistence.

```
MenuRoot -> DirectConnect (Host/Join) -> Session -> CharSel -> Match -> PostMatch
         -> Settings (Identity/Endpoint/Session/Diagnostics)
         -> Spectate (Connect/Watch)
```

**Configurable settings** (persisted to `as2_netplay.cfg`):
- Identity: nickname, listen port
- Endpoint: remote endpoint, preferred delay, rollback budget/tolerance, connection mode
- NAT: UPnP, STUN, hole punch, PCP, TURN, IPv6 toggles, server endpoints, credentials, timeouts
- Spectator: enabled, listen port
- Palette: transport enabled, remote preview enabled

**Auto-connect:** Reads `as2_autoconnect.cfg` for automated testing workflows, progressing through: WaitingForMenu -> WaitingForConnection -> WaitingForCharSel -> SelectingCharacter -> SelectingStage -> WaitingForGameplay -> InMatch -> ConfirmingWinScreen.

**UI** (`netplay_menu_ui.cpp`): DXLib-based rendering using the game's own sprite and text functions. Panel layout (56-584 x 46-438), row highlighting, text edit fields with cursor, alpha fade transitions.

### Spectator System

Five-part spectator architecture:

**Server side:**
- `spectator_manager.cpp`: Manages up to 8 spectator clients. Handles handshake with protocol version check, per-peer state tracking, frame batch streaming, palette state/data propagation, heartbeat at 500ms intervals, redirect endpoint support, and keeps already-synced spectators attached across rematch gaps.
- `spectator_runtime.cpp`: Archives confirmed rollback frames for spectators, manages match metadata (ID, config, player names, set score), tracks palette epochs for sync, and prunes retained frames to the oldest still-requested point.

**Client side:**
- `spectator_client.cpp`: Connects to the host spectator server, performs LAN discovery, supports derived/fallback spectator endpoints plus downstream relay fanout, buffers frames with configurable thresholds (30-frame fast-forward, 180-frame hard-sync), and tracks palette state for remote preview.
- `spectator_protocol.cpp`: Defines spectator packet types: Hello, HelloAck, Redirect, MatchState, FrameBatch, PaletteState, PaletteData, Heartbeat, Disconnect, ClientStatus.
- `spectator_playback.cpp`: Owns local watch playback. It launches a local watch frontend once a full archive is available, force-locks character/palette/stage setup, feeds confirmed P1/P2 inputs through the input dispatcher, and uses a discrete catch-up budget plus tick scaling rather than rollback.

**Playback model:**
- Spectators are not live rollback peers. They consume a confirmed archived input stream through a dedicated sidecar path.
- Automatic watch startup currently requires a full archive that begins at rollback frame 0. Mid-match attach waits for the next fully archived match instead of attempting an unsafe partial reconstruction.
- Once local gameplay ownership has started, playback keeps dispatching through intro and round-transition phases so the game-side phase timer can advance naturally between rounds.

### Custom Palette Sync

**Runtime** (`netplay_palette_runtime.cpp`): Tracks character/palette selections for both players. Manages vanilla, live (loaded), and custom (user-created) palette banks. Supports live reload and remote preview. Can suppress visual overrides on win screen.

**Storage** (`netplay_palette_storage.cpp`): File I/O for custom palette banks at `custom_palettes/char_XXX_base_N.bin`. CRC32 validation on load. 12 palette banks per character, 1024 bytes per bank.

### Network Thread

Dedicated worker thread (`network_thread.cpp`, 612 lines) decouples network I/O from the game thread:

**Command queue** (game -> worker): StartHost, StartJoin, SendPacket, RequestDisconnect, RequestDestroyHost, ClearQueues. Max 4096 entries.

**Event queue** (worker -> game): Connected, PacketReceived, Disconnected, WorkerError. Max 4096 entries.

The worker thread waits on a condition variable with 2ms timeout for ENet servicing. Drops oldest entries on queue full and increments drop counters for diagnostics.

**Transport layer** (`enet_transport.cpp`, 357 lines): ENet host management, connection establishment, hole punch burst sending, typed packet transmission with channel/reliability routing.

### Pause Handling

The pause handler (`pause_handler.cpp`, 171 lines) detects pause-menu quits and ensures clean session termination. Tracks pause enter/exit, detects quit vs. resume, sends/receives `PauseQuit` packets to inform the remote peer.

### Set Tracker

Tracks match wins/losses per session (`set_tracker.cpp`, 113 lines). Records results using the player side mapping to correctly attribute wins. Provides P1/P2 win counts for HUD display.

---

## Rollback Engine

### Rollback Session

The rollback session (`rollback_session.cpp`, ~850 lines) manages the GekkoNet rollback library integration:

**Configuration:**
- Local/remote player assignment
- Initial input delay and rollback budget
- Baseline checksum for verification
- Frame origin for absolute-to-relative conversion

**Two-phase frame processing:**
1. `RollbackSession_BeginFrame(localInput)` -- Feed local input to GekkoNet
2. `RollbackSession_ProcessNextEvent()` -- Iterate GekkoNet events (Advance, Done, Error)

**GekkoNet bridge:**
- State capture/restore callbacks for savestate integration
- Transport adapter bridging ENet packets to GekkoNet's internal format
- Serialized game state (`GekkoState`) with explicit frame metadata and CRC
- FPU state capture (x87 control word + MXCSR) for floating-point determinism

**Telemetry:** `RollbackSessionSnapshot` provides complete session telemetry including frame state, rollback stats, and network metrics. Lightweight `RollbackTimesyncTelemetry` for pacing decisions avoids expensive state CRC computation.

### Savestate System

Manual savestate system (`savestate.cpp`, ~400 lines) for offline testing and debugging:

**Hotkeys:** F5 save, F6 load. Only active during gameplay (Mode 8, substate 3).

**State regions captured (~260KB total):**
- Main contiguous region: `0x76C5F8..0x7AB880` (259KB) -- match base through P2 entity end
- Pre-match gap (12 bytes): effect index, audio channel, render state
- Scattered globals: RNG seed, frame counters, mode/substate, input write indices
- Input buffers: 208 bytes per player (held/previous/just-pressed state)
- Metadata: frame number, checksum, RNG seed, game mode, substate

FPU state is captured for diagnostics but not restored by default.

### Resimulation

The resimulation engine (`resimulation.cpp`, ~1100 lines) executes rollback:

**State history:** Ring buffer of 32 slots, each ~254KB (~4MB total heap allocation). Captures full game state with metadata.

**Rollback flow:**
1. `StateHistory_LoadFrame(rollback_frame)` -- Restore the earliest mispredicted frame
2. `Resim_Execute(rollback_frame, target_frame)` -- Resimulate forward calling the game's Mode 8 handler per frame
3. Side-effect suppression flag (`Resim_IsResimulating()`) checked by audio and debug systems during resim

### Input Timeline

Per-frame input history (`input_timeline.cpp`, ~290 lines) with 256-frame circular buffer:

- `InputTimeline_SetLocalInput(frame, input)` -- Record confirmed local input
- `InputTimeline_SetRemoteInput(frame, input)` -- Record confirmed remote input, detect mispredictions
- `InputTimeline_PredictRemoteInput(frame, input)` -- Fill gap with predicted input
- `InputTimeline_FindFirstMisprediction(start_frame)` -- Scan for rollback trigger frame

### Prediction

Centralized prediction policy (`prediction.cpp`, ~110 lines):

| Strategy | Behavior |
|----------|----------|
| RepeatLast | Repeat last confirmed remote input (default) |
| Neutral | Predict neutral (no buttons) |

Minimal design for easy extension. State maintained per prediction module instance with `Prediction_OnRemoteConfirmed()` updates.

### Frame Lineage

Simple frame mapping (`frame_lineage.cpp`, ~15 lines):
- `FrameLineage_GameAbsFromRb(origin, rb_frame)` -- Convert rollback-relative frame to game absolute
- `FrameLineage_GameAbsFromCheckpoint(origin, rb_frame)` -- Handle bootstrap checkpoint normalization

### Determinism Verification

Comprehensive determinism tooling (`determinism_verify.cpp`, ~730 lines):

**RNG hooking:**
- Hooks `rand()` at `0x71459D` and `srand()` at `0x714590`
- Reads/writes TLS-based MSVC LCG seed via `_getptd() + 0x14`
- Captures up to 8 rand() callers per frame

**Per-frame trace:**
- RNG seed before and after frame
- x87 control word and MXCSR state
- Game state checksum (P1 entity + P2 entity + pre-match gap + RNG)
- Optional FPU enforcement (forces CW=0x027F, MXCSR=0x1F80)

**A/B comparison:** Two capture windows for replay verification with detailed mismatch reporting. Ring buffer stores last 1024 traces.

### Rollback Debug and Diagnostics

Session-level diagnostics (`rollback_debug.cpp`, ~750 lines):

- Per-frame gameplay state digest (P1/P2 entity + pre-match gap + RNG seed CRC)
- Digest exchange at configurable interval (default: every 60 frames)
- Settlement lag calculation (minimum 6 frames + rollback budget + delay + margin) ensures only confirmed frames are compared
- Checksum history ring (512 entries) for quick lookup
- Immediate desync detection with logging and dump trigger on first mismatch
- Sends `StateDigest` and `FrameSyncStatus` packets on the debug channel

### Desync Dump

Comprehensive post-mortem dump (`desync_dump.cpp`, ~800 lines) triggered on desync detection:

**Dump contents:**
- Header: timestamp, PID, frame, local/remote CRCs
- Rollback session snapshot: all frame counters, confirmed states, rollback history
- Global game state: mode, substate, game type, frame counters, RNG seed
- FPU state: x87 CW, MXCSR values
- Determinism trace: RNG transitions, rand caller addresses
- P1/P2 entity details: HP, meter, position, action, animation, hitstun, blockstun, combo state
- Per-entity sub-region CRC breakdown (11 regions)
- Raw hex dump of entity input buffers
- Decoded current inputs with button names
- Checksum history for nearby frames

**Cooldown:** 10-second real-time throttle to prevent dump spam.

### Stress Testing

Network stress hooks (`stress_hooks.cpp`, ~180 lines) for testing under adverse conditions:

| Parameter | Range | Description |
|-----------|-------|-------------|
| Added latency | 0-500ms | Fixed delay on outgoing packets |
| Jitter | 0-200ms | Random variation on top of latency |
| Packet drop | 0-100% | RNG-based packet drop probability |
| Input delay | 0-30 frames | Delayed input delivery |
| Forced misprediction | Count | XOR corruption of predicted inputs |

Provides lifetime statistics snapshot: packets dropped, delayed, mismatches forced.

---

## Game Patches

### Hook Installer

The hook installer (`hook_installer.cpp`, 268 lines) initializes MinHook and installs all 25+ game hooks in a specific order:

1. **Input hooks:** KeyboardState, JoystickState, InputProcess, InputDispatcher
2. **DInput hooks:** Keyboard and joystick buffer refresh, cooperative level sanitization
3. **Win32 hooks:** GetKeyboardState, ClipCursor, GetProcAddress, SystemParametersInfoA, WINNLSEnableIME
4. **Locale hooks:** GetOEMCP, GetACP, MultiByteToWideChar
5. **Filesystem hooks:** CreateFileA, DeleteFileA, FindFirstFileA, GetFileAttributesA
6. **Tick hook:** GetTick (timeGetTime wrapper)
7. **Command history hook:** Practice mode player swap
8. **Palette asset hook:** Character palette injection
9. **Input sync hooks:** Vanilla netplay suppression (installed after all others are enabled)

### Input Override

The input override system (`input_override.cpp`, 500+ lines) intercepts all game input paths:

**Hooked functions:**
- `Hook_KeyboardState()` / `Hook_JoystickState()` -- Query hooks returning SDL input
- `Hook_InputProcess()` -- Just-pressed flag computation
- `Hook_InputDispatcher()` -- Per-frame dispatch with rollback integration
- `Hook_DInputKBRefresh()` / `Hook_DInputJoyRefresh()` -- DInput buffer refresh
- `Hook_GetKeyboardState()` -- Win32 keyboard state with reserved key filtering
- `Hook_ClipCursor()` -- Prevents mouse trapping
- `Hook_GetProcAddress()` -- Blocks vanilla `SetMSGHookDll` shell hotkey suppression
- `Hook_SystemParametersInfoA()` -- Blocks legacy shell hotkey suppression
- `Hook_WINNLSEnableIME()` -- Prevents IME disable
- `Hook_DInputKeyboardSetCooperativeLevel()` -- Forces nonexclusive + foreground (removes EXCLUSIVE + NOWINKEY flags)

**Blocked keys:** LWin, RWin, Apps, LAlt, RAlt, Shift (both DirectInput scancodes and virtual keys).

### Input Sync Hooks

Vanilla netplay suppression (`input_sync_hooks.cpp`, 304 lines) intercepts 5 game functions:

| Hook | Address | Purpose |
|------|---------|---------|
| SendInputPacket | 0x562450 | Suppress when mod owns sync |
| RecvInputPacket | 0x5623D0 | Suppress when mod owns sync |
| GetSyncInput | 0x5624E0 | Return zeroed inputs when mod owns sync |
| AdvanceFrame | 0x562760 | Freeze for charsel lockstep, load barrier, practice pause |
| MatchSyncInit | 0x562550 | Suppress blocking sync wait, replicate state reset |

**Load barrier freeze:** Pauses gameplay at the Mode 8 boundary while keeping the game loop alive for rendering.

**Runtime timesync freeze:** One-frame pulse consumed by AdvanceFrame suppression for frame-pacing holds.

**MatchSyncInit replication:** When suppressing the vanilla blocking sync, the hook replicates the exact state reset: frame counters, input history, flags, timeout counters.

### Tick Hooks

Time scaling (`tick_hooks.cpp`, 136 lines) wraps `timeGetTime()`:

- Maintains virtual time with scale factor
- Smooth slew toward target scale (max 0.35% change per frame)
- Supports manual scale (0.1x - 32x) and netplay pacing scale simultaneously
- Effective scale = manual * netplay
- State struct: `NetplayTickState` (initialized, last_real_tick_ms, virtual_tick_ms, current_scale, target_scale, pacing_active)

### Filesystem Patch

Shift-JIS filename support and mod loader (`filesystem_patch.cpp`, 500+ lines):

**Hooked Win32 functions:** CreateFileA, DeleteFileA, FindFirstFileA, GetFileAttributesA. All redirected through CP932 -> Wide path conversion.

**Mod file override:** Searches mod directories in priority order (top-to-bottom). First match wins.

**Security checks:** Rejects absolute paths, parent traversal (`..`), wildcards, and invalid folder names (spaces, path separators, special characters).

**Config template:** Auto-generated `mods/mods.ini` on first launch if missing.

### Locale Patch

Force Japanese locale (`locale_patch.cpp`, 36 lines):
- `Hook_GetOEMCP()` -> returns 932
- `Hook_GetACP()` -> returns 932
- `Hook_MultiByteToWideChar()` -> redirects CP_ACP (0) and CP_OEMCP (1) to CP932

Allows the game to run correctly on non-Japanese Windows systems.

### Unlock Patch

Full content unlock (`unlock_patch.cpp`, 28 lines):
- Sets 80 unlock flag bytes to 1 (characters, boss, arcade/story clear, gallery)
- Enables `STAGESEL_ENABLE` flag at `0x8E93EE`
- Applied after config.dat loads (version == 258)

### Palette Asset Hook

Custom palette injection (`palette_asset_hook.cpp`, 400+ lines):

**Hooked asset pipeline functions:**
| Address | Function | Purpose |
|---------|----------|---------|
| 0x14A460 | AssetLoadAllFromArchive | Archive decode loop |
| 0x23A9B0 | ImageCreateFromDecodedBmp | BMP -> handle bridge |
| 0x23AB70 | ImageCreateFromFormat | Format -> handle creation |
| 0x220930 | ImageRegisterHandle | Handle allocation |
| 0x2132E0 | HandleRenderBind | Upload/bind to GPU |
| 0x2460C0 | ImageUploadToHandle | Late blit/upload |

Tracks 4 stages of the asset pipeline (decode -> create -> bind -> upload) to inject custom palette data at the correct point.

**Supported characters** (22 archive stems): ran, hat, pat, see, ray, ari, mar, shi, fan, mik, men, han, sat, tig, esc, mak, ali, nal, dem, lit, tad, fna.

**Palette format:** Max 12 banks per character, 1024 bytes per bank, patch buffer 0x3200 bytes.

### Character Select Palette Selection

Palette selection UI flow (`charsel_palette_select.cpp`, 400+ lines):

- 8 vanilla palettes + up to 8 custom palettes per character (16 options max)
- Active during CharSel substates 2 (select) and 4 (confirm)
- Netplay: exchanges catalog availability via `BarrierProtocol_SendPacket`
- Offline: both P1/P2 use local catalog
- Tracks per-slot phase: None -> Selecting -> LockedFinal

### Memory Utilities

SEH-protected memory access (`memory_utils.cpp`, 95 lines):
- `ReadMemory<T>(addr)` -- Reads any type from address, returns default on exception
- `WriteMemory<T>(addr, value)` -- Writes with VirtualProtect, returns success
- `CopyMemorySafe(dst, src, size)` -- SEH-protected memcpy
- `WriteMemoryBlockSafe(addr, data, size)` -- VirtualProtect + SEH write
- `CalcCRC32(data, size)` -- Standard CRC32 (polynomial 0xEDB88320)

All functions use `__try/__except` structured exception handling.

---

## Input System

### SDL3 Integration

The input system (`input_system.cpp`, ~1400 lines) replaces the game's native DirectInput with SDL3:

**Input bitmask (16-bit):**

| Bit | Name | Value |
|-----|------|-------|
| 0 | UP | 0x0001 |
| 1 | DOWN | 0x0002 |
| 2 | LEFT | 0x0004 |
| 3 | RIGHT | 0x0008 |
| 4 | A (light) | 0x0010 |
| 5 | B (medium) | 0x0020 |
| 6 | C (heavy) | 0x0040 |
| 7 | D (special) | 0x0080 |
| 8 | START | 0x0100 |
| 9 | SELECT | 0x0200 |
| 10 | L1 | 0x0400 |
| 11 | R1 | 0x0800 |
| 12 | L2 | 0x1000 |
| 13 | R2 | 0x2000 |

**Per-player state:** current, previous, just-pressed, just-released masks.

### Binding System

Each player has 14 bindable actions. Each binding supports keyboard (SDL scancode) and gamepad (button + axis with direction), OR'd together.

**Default P1 bindings:**
- Directions: Arrow keys
- A/B/C/D: Z, X, C, V
- L1/R1: A, S
- Start/Select: Enter, Backspace
- L2/R2: Gamepad triggers

**Default P2 bindings:**
- Directions: Numpad arrows
- Buttons: Numpad digits and operators
- System: Numpad Enter, Numpad Decimal

**Features:**
- Unified capture mode for keyboard and gamepad binding (single modal)
- Auto-save to `as2_input.cfg`
- Live input display with color-coded button indicators
- Per-player gamepad tracking (up to 4 gamepads, auto-matched)
- Gamepad hotplug support via SDL3 events
- Binding cooldown (30 frames after binding completes)
- Analog stick deadzone: 8000

### SOCD Cleaning

Automatic Simultaneous Opposing Cardinal Direction cleaning:
- UP + DOWN: both cancelled
- LEFT + RIGHT: both cancelled

### Override and Netplay Input

- `InputSystem_SetOverride(player, input)` -- Inject input for rollback/testing
- Separate netplay input storage for rollback-driven frames
- Control swap: P1 <-> P2 physical-to-logical remapping
- Pause blocking: suppresses START during netplay to prevent pause menu
- Background input: reads input even when window is unfocused
- Menu button repeat: 5-frame delay for UI navigation

---

## User Interface

### Mod Menu

Unified ImGui-based mod menu (`mod_menu.cpp`, ~650 lines) with tabbed interface:

| Tab | Contents |
|-----|----------|
| Input Config | Per-player key/gamepad binding tables, background input toggle, auto-save |
| Advanced Debug | Game state display, timing scale control, input debug, game console |
| Log | Scrollable log window with category filtering |
| Savestate | F5/F6 management, slot info display |
| Netplay HUD | Connection stats overlay configuration |
| Determinism | RNG hooking toggle, FPU enforcement, A/B capture comparison |
| Hitbox Viewer | Toggle, color settings, fill alpha, per-frame logging |
| Training | Pause/step controls, controller swap status |

Toggle with F1. Resolution-aware scaling for HiDPI displays.

### Netplay HUD

In-game overlay (`netplay_hud.cpp`, ~90 lines):
- **Top bar:** P1/P2 nickname pills aligned to the current display edges with side-colored backgrounds and shadowed text for readability
- **Bottom bar:** PING (ms), delay frames, rollback frames, or spectator playback status with a compact status bar
- Layout adapts to the current display size rather than assuming a fixed 640x480 screen space

### Hitbox Viewer

Debug visualization (`hitbox_viewer.cpp`, ~900 lines) derived from game decompilation:

**Box types rendered:**

| Type | Offset | Color | Description |
|------|--------|-------|-------------|
| Pushbox | +0 | White | Body collision / clash target |
| Attack hitbox | +8 | Red | Melee + throw attacker (4 per frame) |
| Hurtbox | +40 | Green | Universal vulnerability (4 per frame) |
| Extended hurtbox | +72 | Blue | Melee-only + tech throw (4 per frame) |
| Projectile hitbox | HitDef array | Yellow | Summon hitboxes (100 slots, 272 bytes each) |

**Coordinate math:**
- World positions: int16 at x10 fixed-point scale
- Box center: `entityPos/10 + 2*offset*facing`
- Box extent: `2*halfW` (game uses half-extents)
- World-to-screen: subtract camera scroll, scale by display ratio

**Controls:** Toggle individual box types, fill alpha slider, per-frame logging mode.

### Log Window

Central logging system (`log_window.cpp`, ~800 lines):

**Features:**
- 11 log categories: General, State, Handshake, ReadySync, Netplay, Input, Rollback, Savestate, Timing, Packet, Pump
- 4 log levels: DEBUG, INFO, WARNING, ERROR
- Duplicate message suppression with repeat count
- 1000-entry ring buffer with oldest-entry eviction
- Thread-safe mutex-protected writes
- Timestamp prefixes (HH:MM:SS format)
- ANSI color support for console output
- 256KB buffered file I/O with 50-line flush threshold

**Output files:**
| File | Purpose |
|------|---------|
| `as2_rollback_<PID>.log` | Main mod log |
| `as2_packets_<PID>.log` | Packet log (opt-in via AS2_PACKET_LOG env var) |
| `as2_netcode_<PID>.log` | Network diagnostics |
| `as2_gekko_<PID>.log` | GekkoNet transport internals |

### Palette Editor

In-game palette editing interface for character appearance testing and custom palette creation.

---

## Replay System

### Replay Runtime

The replay runtime (`replay_runtime.cpp`) extends both replay browsing and replay playback.

**Core features:**
- Replay browser hooks for save post-process and replay-select draw integration
- Pause, frame-step, rewind, and speed adjustment during replay playback
- Replay HUD with transport state, playback speed, and takeover state
- Checkpoint/snapshot-backed seeking so replay navigation and replay branching stay deterministic

**Replay controls:**

| Key | Action |
|-----|--------|
| Bksl | Pause / unpause replay playback |
| ] | Step forward |
| [ | Step backward |
| Shift+[ | Rewind |
| + / - | Change replay speed |
| Insert | Toggle replay HUD |

### Replay Takeover

Replay takeover lets live player input replace recorded replay input from a chosen replay frame.

**Current user flow:**
- Press `1` to arm P1 takeover or `2` to arm P2 takeover during an active replay match.
- The runtime captures a full snapshot at the current replay frame and starts a 60-frame countdown.
- When the countdown expires, live SDL input overrides the recorded replay input for the selected side.
- Press the same takeover key again to restore the captured snapshot and restart the takeover from the same source frame.
- Press `0` to exit takeover and restore the base replay timeline.

**Implementation notes:**
- The runtime tracks takeover state, countdown, and the captured origin snapshot internally.
- Future replay overrides and checkpoints are invalidated from the takeover start forward so the replay branch stays consistent.
- The underlying code supports takeover modes for P1, P2, and Both; the current public hotkeys arm the single-side P1/P2 flows directly.

---

## Training Mode

Practice mode tools (`practice_tools.cpp`, ~450 lines) exclusively for training mode (GAMETYPE_TRAINING in MODE_MATCH):

**Hotkeys:**

| Key | Action |
|-----|--------|
| F4 | Toggle hitbox viewer |
| F7 | Pause / unpause |
| F8 | Single-frame step (one frame per press while paused) |
| F9 | Controller swap (P1 <-> P2 with CPU flag management) |

**Controller swap:** Toggles `ADDR_P1_CPU_FLAG` and `ADDR_P2_CPU_FLAG`. Default: P1 human, P2 CPU dummy. Swapped: P1 CPU, P2 human.

**Toast notification system:** Up to 4 simultaneous toasts, 1.5s duration with fade-out at 1.0s, color-coded by action type.

**Integration:** `PracticeTools_ShouldFreezeFrame()` is queried by input_sync_hooks to suppress frame advancement during pause.

---

## Testing Infrastructure

### Scripted Input Runner

Lightweight offline scripted playback (`scripted_input_runner.cpp`, ~450 lines):

**Run modes:**

| Mode | P1 | P2 |
|------|----|----|
| LOCAL_VS | Scripted | Idle or scripted |
| VS_CPU | Scripted | Game CPU |
| DUAL_SCRIPT | Scripted | Scripted independently |

**Features:**
- Frame-accurate input injection via `InputSystem_SetOverride()`
- Multiple overlapping entries OR'd together
- Scenario looping support
- Max 512 entries per scenario, 16 scenarios in registry
- No dependency on netplay, rollback, or session systems

### Test Scenarios

Six built-in deterministic test scenarios (`test_scenarios.cpp`, ~295 lines):

| Name | Frames | Purpose |
|------|--------|---------|
| Idle300 | 300 | No input -- determinism baseline |
| WalkJumpLight | 240 | Walk, jump, light attack -- basic movement |
| ProjectileLoop | 300 | Repeated QCF+A motion -- special move loop |
| MashDirections | 300 | Alternating contradictory inputs -- SOCD/stress test |
| WakeupDPLoop | 360 | Repeated DP+A motion -- wakeup reversal test |
| CpuStressStarter | 600 | Rapid varied inputs in 6 phases -- CPU stress test |

### Autoconnect Harness

Automated netplay testing (`autoconnect_harness.cpp`, ~600 lines):

**Shared memory writer:** Creates PID-specific shared memory segments (`HARNESS_SHM_NAME_HOST` / `HARNESS_SHM_NAME_CLIENT`) containing:
- Session info: state, RTT, packets, nickname, status/error text
- Game state: mode, substate, game type, frame counters, RNG seed, checksum
- Entity state: P1/P2 HP, meter, position, action, input
- Rollback info: frames, state, advance count, depth, resim count
- Log ring buffer: 64 lines x 256 chars

**Fighting AI:** Simple state machine with LCG PRNG for automated match testing:
- Actions: Idle, Approach, Retreat, Jump, AirAttack, QCF, DP, HCF, Dash, Poke, Combo, Throw, Block, Wakeup
- Directional awareness (FWD/BACK relative to player side)
- Weighted random selection (~15% approach, 10% retreat, 15% jump, etc.)

### Unit Tests

Three test suites (`tests/`):

**standalone_rollback_tests.cpp:**
- FPU state preservation (x87 CW, MXCSR, exception masks, precision/rounding modes)
- Rapid 1000-cycle stability tests
- Ring buffer modulo correctness (`-1 % 16 -> 15`)
- MSVC LCG RNG determinism, seed save/restore, visual/simulation independence
- SEH-based crash validation (proves old fsave/frstor caused ACCESS_VIOLATION)

**test_packet_codec.cpp:**
- Round-trip encode/decode
- Truncation and corruption detection
- CRC determinism and differentiation
- Max/oversized payload handling
- All packet type coverage

**test_runner_main.cpp:** Orchestrator linking all tests with pass/fail reporting.

### Test Harness Launcher

Standalone executable (`tools/test_harness_launcher.cpp`, ~21KB):
- ImGui + D3D9 dashboard for two-instance autoconnect testing
- Configurable network conditions (latency, jitter, loss, duplication)
- Log collection and analysis on exit
- Command-line support for headless/scripted testing

---

## D3D9 Graphics Proxy

The D3D9 proxy (`d3d9_proxy.cpp`, ~69KB) intercepts `Direct3DCreate9` and wraps the real D3D9 device:

- Hooks `IDirect3DDevice9::EndScene` via MinHook for ImGui overlay rendering
- Manages ImGui context lifecycle and font scaling
- Loads SDL3.dll, as2_rollback.dll, and user DLL mods from `mods/`
- Intercepts WndProc for input capture and window state handling
- Integrates the letterbox scaler for display management

### Letterbox Scaler

Aspect-ratio-correct upscaling (`letterbox_scaler.cpp`):

1. Creates offscreen render target at native game resolution (640x480)
2. Redirects all game rendering to offscreen surface via `BeginFrame()`
3. At `EndFrame()`, copies offscreen to backbuffer with StretchRect scaling
4. Calculates letterbox/pillarbox destination rectangle to center content
5. Uses triangle-strip quad with bilinear filtering for smooth upscaling

---

## Winsock Proxy

The winsock proxy (`wsock32_proxy.cpp`) loads before all other DLLs and applies critical early patches:

1. **Duplicate-instance bypass:** Patches FindWindowA in game IAT to return NULL; sets DXLib flag to allow multiple copies
2. **Shell hotkey suppression bypass:** Clears vanilla game's shell hotkey message hooks
3. **IME suppression tracing:** Hooks ImmSetOpenStatus, ImmNotifyIME, ImmSetCompositionStringA for debugging
4. **Focus-steal suppression:** Blocks late SetForegroundWindow/BringWindowToTop/SetActiveWindow calls (>3s after start)
5. **FPU exception mask preservation:** Saves/restores FPU state during all operations (critical for determinism)
6. **Winsock forwarding:** Exports all 16 winsock functions, forwarding to real system wsock32.dll

Logging to `logs/wsock32_proxy_<PID>.log`.

---

## Mod Loader

The runtime includes an ordered mod loader built on the filesystem patch layer.

### Bootstrap

- Config file: `mods/mods.ini`
- On first launch, the runtime creates `mods/` and a commented `mods.ini` template if they do not exist.
- The `[Mods]` section is parsed manually in file order.

### Priority Rules

- Priority is top-to-bottom.
- Earlier entries win file conflicts.
- DLL mods load in the same top-to-bottom order.
- DLL mods unload in reverse order during shutdown.

### Folder Rules

- Each enabled mod lives at `mods/<ModName>/`.
- Folder names must not contain spaces, path separators, or special characters.
- A file override mirrors the game-relative path under the mod folder.
- Example: `data/tit.bin` becomes `mods/ExampleMod/data/tit.bin`.

### Optional DLL Mods

- If present, the loader looks for `mods/<ModName>/<ModName>.dll`.
- User DLL mods are loaded by `d3d9.dll` after `SDL3.dll` and `as2_rollback.dll` are active.
- Optional exports:
  - `ModSetLogDir(const char* dir)`
  - `ModInit(HMODULE gameModule)`
  - `ModShutdown()`

### Example Layout

```text
GameRoot/
  mods/
    mods.ini
    ExampleMod/
      ExampleMod.dll
      data/
        tit.bin
```

Example `mods.ini`:
```ini
; Auto-generated on first launch if missing.
; Uncomment entries and change the value to 1 to enable them.
; Earlier entries have higher priority than later ones.
[Mods]
;ExampleMod=1
```

---

## Game State Reference

### Game Modes

Address: `0x81638C` (`dword_81638C`)

| Value | Name | Description |
|-------|------|-------------|
| 0 | MODE_BOOT | Initial boot |
| 2 | MODE_TITLE | Title screen |
| 3 | MODE_MENU | Main menu |
| 4 | MODE_LOBBY | Network lobby (vanilla) |
| 5 | MODE_REPLAY_SELECT | Replay selection |
| 6 | MODE_CHARSEL | Character select |
| 7 | MODE_PREMATCH_INTRO | Pre-match intro |
| 8 | MODE_MATCH | Active match |
| 9 | MODE_WINSCREEN | Win screen |
| 10 | MODE_END | End screen |
| 11 | MODE_GALLERY | Gallery viewer |
| 12 | MODE_OPTIONS | Options menu |
| 13 | MODE_PALETTE | House of Alice mode (internally still routed through palette-select code) |

Game type address: `0x816410`

| Value | Name |
|-------|------|
| 0 | GAMETYPE_ARCADE |
| 1 | GAMETYPE_VS_CPU |
| 2 | GAMETYPE_VS_HUMAN |
| 3 | GAMETYPE_NETPLAY |
| 4 | GAMETYPE_TRAINING |
| 5 | GAMETYPE_REPLAY |
| 10 | GAMETYPE_DEMO |

### Match Substates

Address: `0x816390`

| Value | Name | Notes |
|-------|------|-------|
| 0 | MATCH_SUB_LOAD_ASSETS | Loading character/stage data |
| 1 | MATCH_SUB_SETUP | Match setup |
| 2 | MATCH_SUB_INIT | Match initialization |
| 3 | MATCH_SUB_GAMEPLAY | Primary rollback target (includes intro lock AND round-end transition) |
| 4 | MATCH_SUB_PAUSE | Pause menu |
| 5 | MATCH_SUB_END | Match end |

### Character Select Substates

| Value | Name | Notes |
|-------|------|-------|
| 0 | CHARSEL_SUB_INIT | Initialization |
| 1 | CHARSEL_SUB_FADEIN | Calls MatchSyncInit at frame 159 |
| 2 | CHARSEL_SUB_SELECT | Active selection (lockstep target) |
| 3 | CHARSEL_SUB_CANCEL | Cancel |
| 4 | CHARSEL_SUB_CONFIRM | Confirmed (lockstep target) |
| 5-8 | CHARSEL_SUB_STAGESEL_* | Stage select phases (slide, zoom, grid, confirm) |
| 9-10 | CHARSEL_SUB_FADE_* | Fade transitions |
| 11 | CHARSEL_SUB_MATCHUP_COMMIT | Matchup committed |
| 12-13 | CHARSEL_SUB_BACK_* | Return to menu/lobby |
| 14 | CHARSEL_SUB_TO_MATCH | Transition to match |

### Entity Memory Layout

Base addresses: P1 `0x776668`, P2 `0x790F74`. Entity size: `0x1A90C` (108,812 bytes).

| Offset | Size | Field |
|--------|------|-------|
| 0x00B0 | 4 | HP |
| 0x00B4 | 4 | Meter |
| 0x00B8 | 2 | X Position |
| 0x00BA | 2 | Y Position |
| 0x00BE | 2 | X Velocity |
| 0x00C0 | 2 | Y Velocity |
| 0x00C2 | 2 | X Acceleration |
| 0x00C4 | 2 | Y Acceleration |
| 0x0470 | 4 | Animation index |

Additional tracked fields: combo counter, hitstun, blockstun, invincibility state.

**Other critical addresses:**
- Match base: `0x76C5F8` (7456 bytes / 0x1D20)
- Simulation frame counter: `0x816490`
- Camera scroll: X `0x76CD3C`, Y `0x76CD3E`
- RNG seed: TLS-based MSVC LCG via `_getptd() + 0x14`
- Input history: P1 `0x8164A0`, P2 `0x87FC24` (216,000 frame capacity, 20-frame window)
- Effect array: `0x76E328` (200 slots, 32 bytes each)
- Summon array: `0x76FC28`

### Hitbox System

**Animation box layout** (8 bytes per box entry: int16 x, y, halfW, halfH):
- Collision box: frame offset +0
- Attack hitboxes: frame offset +8 (4 entries)
- Hurtboxes: frame offset +40 (4 entries)
- Extended hurtboxes: frame offset +72 (4 entries)

**HitDef structure:** 272 bytes per slot, 100 active slots. Fields include: owner, ID, type, active flag, damage, attack level, blockstun, hitstun, knockback.

---

## Building

### Prerequisites

- Visual Studio 2019 or 2022 with the C++ workload
- CMake 3.15 or newer
- A Win32 (x86) build target -- the game is 32-bit and must not be built as x64

### Dependencies

Run `setup_deps.ps1` to automatically download and set up dependencies, or configure manually:

| Library | Path | Required | Notes |
|---------|------|----------|-------|
| MinHook v1.3.3 | `lib/minhook_src` | Yes | Built from source as static library |
| SDL3 | `lib/SDL3` | Yes | Dynamic (DLL) |
| ImGui v1.91.9 | `lib/imgui` | Yes | Built as static library |
| ENet | `lib/enet` | Yes | Built as static library |
| GekkoNet | `lib/GekkoNet` | Yes | Rollback library |
| libjuice | `third_party/libjuice` | No | STUN/TURN NAT traversal |
| miniupnpc | `third_party/miniupnp_suite/miniupnpc` | No | UPnP NAT traversal |
| libpcpnatpmp | `third_party/libpcpnatpmp/lib` | No | PCP/NAT-PMP fallback |

NAT traversal libraries are optional; CMake warns and disables those paths when unavailable.

### Configure

```powershell
cmake -S mod -B mod/build -A Win32 `
  -DGAME_DIR="D:/Path/To/Alice Senki 2" `
  -DTEST_GAME_DIR="D:/Path/To/Second/GameCopy"
```

- `GAME_DIR`: Deployment directory for post-build copy rules and `cmake --install`.
- `TEST_GAME_DIR`: Optional, used by the two-instance harness workflow.

### Build

```powershell
# Debug build
cmake --build mod/build --config Debug --target as2_rollback d3d9 wsock32 as2_test_harness

# Release build (generates PDB and MAP files for crash symbolization)
cmake --build mod/build --config Release --target as2_rollback d3d9 wsock32 as2_test_harness
```

The build uses static MSVC runtime (/MT) to eliminate VC++ redistributable dependencies and enables /EHa for SEH exception handling.

### Install or Deploy

```powershell
cmake --install mod/build --config Release
```

This installs:
- `d3d9.dll`
- `as2_rollback.dll`
- `wsock32.dll`
- `as2_test_harness.exe`
- `SDL3.dll` (when `lib/SDL3/lib/x86/SDL3.dll` exists)

Post-build copy rules automatically push `d3d9.dll`, `as2_rollback.dll`, `SDL3.dll`, and `as2_test_harness.exe` into `GAME_DIR` when configured. `wsock32.dll` requires `cmake --install` or manual copy.

---

## Required Runtime Files

The game directory needs these files:

| File | Purpose |
|------|---------|
| `d3d9.dll` | Graphics proxy + overlay |
| `as2_rollback.dll` | Main mod runtime |
| `wsock32.dll` | Early-start proxy |
| `SDL3.dll` | Input runtime |

Optional:
| File | Purpose |
|------|---------|
| `as2_test_harness.exe` | Two-instance testing dashboard |

If the game directory already contains a third-party `d3d9.dll`, back it up before deploying.

---

## Hotkeys

| Key | Action | Context |
|-----|--------|---------|
| F1 | Toggle ImGui menu | Always |
| F4 | Toggle hitbox viewer | Training mode |
| F5 | Save manual savestate | During gameplay |
| F6 | Load manual savestate | During gameplay |
| F7 | Pause / unpause | Training mode |
| F8 | Single-frame advance (one frame per press) | Training mode, while paused |
| F9 | Swap P1/P2 controls | Training mode |
| Bksl | Pause / unpause replay playback | Replay |
| `[` | Step backward one frame | Replay, while paused |
| `]` | Step forward one frame | Replay, while paused |
| Shift+`[` | Rewind replay | Replay |
| + / - | Change replay speed | Replay |
| Insert | Toggle replay HUD | Replay |
| 1 | Start or restart P1 replay takeover | Replay |
| 2 | Start or restart P2 replay takeover | Replay |
| 0 | Exit replay takeover and restore base replay | Replay takeover |

---

## Configuration Files

| File | Purpose | Auto-created |
|------|---------|--------------|
| `as2_input.cfg` | Input bindings (keyboard + gamepad) | Yes, on first save |
| `as2_netplay.cfg` | Netplay settings (nickname, ports, delay, NAT, spectator, palette) | Yes, on first use |
| `as2_autoconnect.cfg` | Automated testing configuration | No, manual or harness |
| `mods/mods.ini` | Mod loader configuration | Yes, on first launch |
| `custom_palettes/char_XXX_base_N.bin` | Custom palette data | No, user-created |

---

## Test Harness

`as2_test_harness.exe` launches two game instances, writes autoconnect configs, opens a D3D9+ImGui dashboard, and summarizes logs on exit.

```powershell
.\as2_test_harness.exe --game-dir "D:\Path\To\Game" --delay 2 --latency 40 --jitter 5 --matches 3
```

| Flag | Description |
|------|-------------|
| `--game-dir <path>` | Path to game directory |
| `--delay <frames>` | Input delay |
| `--latency <ms>` | Simulated network latency |
| `--jitter <ms>` | Simulated jitter |
| `--loss <pct>` | Packet loss percentage |
| `--dup <pct>` | Packet duplication percentage |
| `--matches <n>` | Number of matches to play |
| `--duration <sec>` | Maximum test duration |
| `--timesync <float>` | Timesync scaling factor |
| `--stage-mash` | Random stage selection |
| `--no-launch` | Don't launch game instances |
| `--no-gui` | Headless mode (no dashboard) |

---

## Project Map

```
mod/
  d3d9_proxy/                  Graphics proxy, ImGui overlay, letterbox scaler, mod DLL loading
    d3d9_proxy.cpp               Main proxy (~69KB)
    d3d9_proxy.def               Export definition (Direct3DCreate9)
    letterbox_scaler.cpp         Aspect-ratio upscaling implementation
    letterbox_scaler.h           Scaler class definition

  winsock_proxy/                Early-start proxy for duplicate-instance bypass
    wsock32_proxy.cpp             Proxy implementation with 5 patch systems
    wsock32_proxy.def             16 winsock function exports

  src/
    core/                       Mod lifecycle and shared state
      mod_main.cpp                Main lifecycle, game queries, HUD data (734 lines)
      game_console.cpp            Game Log_Write hook capture (195 lines)

    input/                      SDL3 input runtime
      input_system.cpp            Unified keyboard+gamepad system (~1400 lines)

    patches/                    Game hooks and compatibility patches
      hook_installer.cpp          MinHook setup, 25+ hook installation (268 lines)
      tick_hooks.cpp              Time scaling with netplay pacing (136 lines)
      input_override.cpp          Input interception and SDL injection (500+ lines)
      input_sync_hooks.cpp        Vanilla netplay suppression (304 lines)
      filesystem_patch.cpp        CP932 filenames + mod file overrides (500+ lines)
      locale_patch.cpp            Force Japanese codepage (36 lines)
      memory_utils.cpp            SEH-protected memory access (95 lines)
      unlock_patch.cpp            Full content unlock (28 lines)
      charsel_palette_select.cpp  Palette selection UI flow (400+ lines)
      palette_asset_hook.cpp      Asset pipeline palette injection (400+ lines)

    net/                        Networking and netplay
      session_manager.cpp         Connection state machine (~550 lines)
      network_thread.cpp          Dedicated network I/O thread (612 lines)
      enet_transport.cpp          ENet host and peer management (357 lines)
      barrier_protocol.cpp        Phase-based packet routing (87 lines)
      pregame_sync.cpp            13-phase pre-game state machine
      charsel_sync.cpp            Character select lockstep (765 lines)
      stagesel_sync.cpp           Stage select input merge (152 lines)
      match_bootstrap.cpp         Config/load/baseline negotiation (300+ lines)
      match_lifecycle.cpp         14-phase match state machine (649 lines)
      mode_ownership.cpp          Vanilla menu/socket interception (495 lines)
      delay_policy.cpp            RTT-based delay negotiation (416 lines)
      sync_policy.cpp             Sync mode classification (309 lines)
      gameplay_bridge.cpp         GekkoNet rollback bridge (186 lines)
      netplay_pacing.cpp          Adaptive frame-rate controller (313 lines)
      nat_traversal.cpp           UPnP/STUN/PCP/TURN/hole-punch (250+ lines)
      netplay_menu_controller.cpp Menu state machine + config persistence
      netplay_menu_ui.cpp         DXLib-based menu rendering
      netplay_phase_runtime.cpp   Phase classification union (171 lines)
      netplay_palette_runtime.cpp Palette sync runtime
      netplay_palette_storage.cpp Palette file I/O
      pause_handler.cpp           Pause-quit detection (171 lines)
      player_side_mapping.cpp     Network role to game slot mapping (101 lines)
      winscreen_sync.cpp          Win screen lockstep (447 lines)
      set_tracker.cpp             Win/loss tracking (113 lines)
      locked_match_config.cpp     Config hash/compare (40 lines)
      baseline_sync.cpp           Baseline CRC verification (558 lines)
      spectator_manager.cpp       Server-side spectator management
      spectator_client.cpp        Client-side spectator viewing
      spectator_protocol.cpp      Spectator packet definitions
      spectator_runtime.cpp       Frame archival for spectators
      spectator_playback.cpp      Local spectator playback + catch-up

    replay/                     Replay browser, playback, and takeover
      replay_runtime.cpp          Replay runtime, HUD, seek, takeover

    rollback/                   Rollback engine and diagnostics
      rollback_session.cpp        GekkoNet session management (~850 lines)
      savestate.cpp               Manual F5/F6 savestates (~400 lines)
      resimulation.cpp            State history + rollback execution (~1100 lines)
      input_timeline.cpp          Per-frame input tracking (~290 lines)
      prediction.cpp              Remote input prediction (~110 lines)
      frame_lineage.cpp           Frame number conversion (~15 lines)
      determinism_verify.cpp      RNG/FPU/checksum verification (~730 lines)
      rollback_debug.cpp          Digest comparison + desync detection (~750 lines)
      desync_dump.cpp             Post-mortem state dump (~800 lines)
      online_wiring.cpp           Net packet serialization (~1200+ lines)
      netplay_log.cpp             Structured rollback logging (~220 lines)
      rematch_cleanup.cpp         Between-match reset (~145 lines)
      stress_hooks.cpp            Network stress testing (~180 lines)

    training/                   Practice mode tools
      practice_tools.cpp          Pause/step/swap/hitbox hotkeys (~450 lines)

    testing/                    Automated testing
      autoconnect_harness.cpp     SHM + fighting AI (~600 lines)
      scripted_input_runner.cpp   Offline input playback (~450 lines)
      test_scenarios.cpp          6 built-in test scenarios (~295 lines)

    ui/                         User interface
      mod_menu.cpp                Tabbed ImGui menu (~650 lines)
      netplay_hud.cpp             Connection stats overlay (~90 lines)
      hitbox_viewer.cpp           Debug box visualization (~900 lines)
      hitbox_display.cpp          Legacy hitbox reference (~450 lines)
      log_window.cpp              Logging system (~800 lines)
      menu_utils.cpp              Public IP, clipboard, helpers (~250 lines)
      palette_editor.cpp          Palette editing UI

  include/                      Header files (mirrors src/ structure)
    core/                       mod_main.h, game_console.h, game_state.h, as2_constants.h
    input/                      input_system.h
    patches/                    All patch headers
    net/                        All net headers + protocol.h, session_types.h, netplay_menu_state.h
    replay/                     replay_runtime.h
    rollback/                   All rollback headers
    training/                   practice_tools.h
    testing/                    autoconnect_harness.h, harness_shared_memory.h, scripted_input_runner.h, test_scenarios.h
    ui/                         All UI headers

  tests/                        Unit tests
    standalone_rollback_tests.cpp FPU, RNG, modulo tests
    test_packet_codec.cpp         Packet encode/decode tests
    test_runner_main.cpp          Test orchestrator

  tools/
    test_harness_launcher.cpp     Two-instance dashboard executable

  docs/
    MASTER_DOCUMENT.md          Primary long-form technical reference

  CMakeLists.txt                Build configuration (4 targets, 7+ libraries)
  build.bat                     Automated build script
  setup_deps.ps1                Dependency downloader
  diagnose_dlls.ps1             DLL troubleshooting script
  alicesenki2_decomp_refactored.c  Decompiled game reference
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Game does not load the mod | Verify every runtime DLL is x86 (32-bit) |
| Proxy stack only partially loads | Run `diagnose_dlls.ps1` from the game folder |
| Mod file override not applying | Confirm path under `mods/<ModName>/` exactly matches game-relative path, and mod is enabled with `=1` |
| User mod DLL not loading | Confirm DLL filename exactly matches the folder name |
| Japanese filenames fail | Confirm proxy stack loaded before game opens assets |
| Architecture mismatch (error 193) | Rebuild all targets with `-A Win32`, verify with `diagnose_dlls.ps1` |
| Missing dependency (error 126) | Check for VCRUNTIME140.dll and Universal CRT; run `diagnose_dlls.ps1` |
| Missing redistributable (error 14001) | Install VC++ 2015-2022 x86 redistributable |
| Access denied (error 5) | Check file permissions and Zone.Identifier blocking |
| Multiple instances won't launch | Verify `wsock32.dll` is present and loading correctly |
| Desync during netplay | Check desync dump files, compare determinism traces, verify both peers have matching builds |

---

## License and Disclaimer

This mod is provided for educational and preservation purposes. Use at your own risk.
Alice Senki 2 is property of its respective copyright holders.
