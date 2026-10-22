# How It Works

This document walks through the mod's runtime architecture with citations into
the source tree. Every cited symbol can be found by searching the named file.
For the long-form reference (per-feature notes, hotkeys, game-state tables,
troubleshooting), see [MASTER_DOCUMENT.md](MASTER_DOCUMENT.md).

## Repository map

| Path | Contents |
|------|----------|
| `d3d9_proxy/` | Source of the proxy `d3d9.dll`: bootstrap, ImGui overlay, scaling swap chain |
| `winsock_proxy/` | Source of the proxy `wsock32.dll`: duplicate-instance bypass, early shell-hotkey patches |
| `src/core/` | `mod_main.cpp` — the `Mod*` export surface the proxies drive |
| `src/input/` | SDL3 input system and gamepad worker |
| `src/net/` | Session, transport, pregame sync, spectator, palette sync, netplay menu |
| `src/rollback/` | Rollback engine (`engine2.cpp`), adapter, savestates, determinism diagnostics |
| `src/replay/` | Replay record format, playback, rewind, takeover |
| `src/training/` | Practice-mode tooling (auto-block, frame advantage, macros, hotkeys) |
| `src/ui/` | Mod menu, netplay HUD, palette editor, hitbox viewer, settings menus |
| `src/patches/` | Game hooks and byte patches (input override, scheduler, palettes, locale) |
| `src/testing/` | Autoconnect harness and soak runners |
| `include/` | Headers, mirroring `src/` |
| `tests/` | The twelve ctest suites |
| `tools/` | Two-instance test harness, Linux launcher, analysis scripts |
| `lib/`, `third_party/` | Vendored and submodule dependencies (see `.gitmodules` and the README table) |
| `assets/fonts/` | ImGui fonts (Noto Sans CJK JP, Shippori Mincho) |
| `cmake/` | Dist packaging scripts |

## The three-DLL proxy stack

The mod ships three DLLs next to the game executable; none of them modify the
executable on disk.

**`wsock32.dll` loads first** (the game imports it statically, before its own
init). Its `DllMain` in `winsock_proxy/wsock32_proxy.cpp` performs the
duplicate-instance bypass (`SetDXLibDuplicateFlag`, `PatchFindWindowA`), applies
the early shell-hotkey/IME IAT patches and fixed-RVA byte patches
(`PatchEarlyHotkeyImports`, `PatchVanillaWndprocSwallowBranches`), loads the
real System32 `wsock32.dll` (`LoadRealWsock32`), forwards its exports (see
`wsock32_proxy.def`), and starts a startup heartbeat thread
(`StartupHeartbeatThread`). The shell-hotkey work follows the policy in
[docs/SHELL_HOTKEY_POLICY.md](SHELL_HOTKEY_POLICY.md).

**`d3d9.dll` defers everything off the loader lock.** Its `DllMain` in
`d3d9_proxy/d3d9_proxy.cpp` installs a crash handler and resolves the real
system d3d9 (`LoadRealD3D9`), but the mod itself only boots on the first
`Direct3DCreate9` call: `Proxy_Direct3DCreate9` runs `RunModBootstrapOnce`
(guarded by `As2Loader::BootstrapGate` in `d3d9_proxy/module_loader.cpp`),
which calls `LoadCoreModDLL` — that loads `SDL3.dll` first, then
`as2_rollback.dll` (via `As2Loader::LoadPackageModule`), resolves the `Mod*`
export table, and calls `ModInit`. The proxy returns a `WrappedDirect3D9`
whose `CreateDevice` vtable-hooks `EndScene`/`Present`/`Reset` for the ImGui
overlay, and reports readiness back to wsock32 through `ReportStartupToWsock`
and the `AS2Wsock_ReportStartup` export. Because DXLib caches the 640x480
backbuffer, the proxy never resizes it — it keeps a second scaling swap chain
at window size and stretches into it with letterbox math
(`letterbox_scaler.cpp`).

**`as2_rollback.dll` is the mod core.** Its entry points are the `Mod*`
exports in `src/core/mod_main.cpp`: `ModInit` does the filesystem patches and
defers the rest; `DeferredInit` (which calls `InstallHooks`) runs on the first
`ModOnFrame`, driven from the proxy's frame hooks. `ModOnGameExit` and
`ModShutdown` tear down.

## Hook installation

Game functions are located by fixed RVAs from
`include/core/as2_constants.h` (`ADDR_INPUT_PROCESS`, `ADDR_INPUT_DISPATCHER`,
`ADDR_KEYBOARD_STATE`, `ADDR_JOYSTICK_STATE`, ...) and detoured with MinHook in
`src/patches/hook_installer.cpp` (`InstallHooks`, `RemoveHooks`,
`MH_EnableHook(MH_ALL_HOOKS)`). Besides the input and audio functions, it
detours selected Win32 APIs and then runs the subsystem installers:
`FilesystemPatch_InstallHooks`, `PaletteAssetHook_Install`,
`CharSelPaletteSelect_Install`, `ReplayRuntime_InstallHooks`,
`ShellHotkeyPatch_Install`, `NetplayBackgroundRun::InstallHook`,
`InputSyncHooks_Install`, `FrameScheduler_Install` (a byte-signature scan of
the game's frame limiter, not a fixed RVA — see `src/patches/frame_scheduler.cpp`),
`PracticeDefense_Install`, `PracticeRecovery_Install`, `PauseMenu_Install`.
The wsock32 proxy applies its own earlier IAT and signature-verified byte
patches (`PatchImportByAddress`, `PatchRvaIfMatches`) before MinHook exists.

## Input path

`InputSystem_Init` (`src/input/input_system.cpp`) sets per-player bindings and
loads `as2_input.cfg`. Keyboard is Win32; gamepads are SDL3
(`SDL_InitSubSystem(SDL_INIT_GAMEPAD)` is deliberately deferred past the
game's early setup) with a `GamepadWorker` thread for open/close/hotplug.
Each `InputSystem_Update` merges keyboard and pad words per player and applies
SOCD cleaning: opposing up+down and left+right are both stripped. Override and
netplay stores (`InputSystem_SetOverride`, `InputSystem_SetNetplayInput`,
`InputSystem_GetNetplayInput`) let replays, macros, and rollback replace a
player's word. Injection into the game goes through the detours in
`src/patches/input_override.cpp` (`Hook_KeyboardState`, `Hook_JoystickState`,
`Hook_InputProcess`, `Hook_InputDispatcher`). The dispatcher is the ownership
gate: during a rollback session `RollbackSession_IsActive()` owns gameplay,
during character/stage select `CharSelSync_IsLockstepActive()` owns frontend
lockstep.

## Rollback engine

`RollbackEngine` (`include/rollback/engine2.h`, `src/rollback/engine2.cpp`) is
a pure decision core: it owns no sockets, no clock, and no game memory — the
header states the contract, and the same object is exercised by
`tests/engine2_tests`. Per pass, `NextAction` decides in fixed order between
`Rollback`, `Stall` (lifecycle boundary, missing local input, prediction
limit), and `Advance`; the caller seals results with `CommitAdvance`. Remote
inputs arrive via `ReceiveRemoteInput`/`IngestInputStream` with a fail-closed
conflict taxonomy (`IngestResult`). Missing remote values come from the
`HoldLastPredictor` (`include/rollback/prediction.h`) and are corrected by the
rollback transaction `BeginRollback`/`NextReplayInputs`/`CommitReplayFrame`/
`FinishRollback`. Confirmed frames exit only through `PopConfirmedFrame`
(`ConfirmedFrame` — a predicted value can never appear there), feeding the
sync-hash exchange (`ReceiveSyncHash`), the spectator archive, and the replay
recorder.

The adapter `src/rollback/rollback_session_engine2.cpp` holds the single
`RollbackEngine s_engine` and owns everything the engine does not: savestates,
wire sends (`SendInputStreamIfDue`), ingest, and idle resend. The game-facing
facade is `include/rollback/rollback_session.h`
(`RollbackSession_BeginFrame` + `RollbackSession_ProcessNextEvent`).

Savestates are `GameSnapshot` structs (`src/rollback/game_snapshot.cpp`:
`GameSnapshot_Capture`, `GameSnapshot_Restore`) kept in a 64-slot direct-mapped
ring (`STATE_HISTORY_CAPACITY` in `include/rollback/resimulation.h`;
`StateHistory_CaptureFrameHashed`, `Resim_Execute`). State identity is hashed
with the four-lane 32-bit `Block64` digest (`include/rollback/block_digest.h`)
over a shared digest-mask table (`kMainDigestMasks`) that both
`GameSnapshot_HashGameplay` and `GameSnapshot_HashGameplayLive` must walk
identically — that shared-walk rule is what makes cross-peer hashes
comparable.

## Netplay session

The gameplay connection is a single-peer ENet session owned by a dedicated
worker thread (`include/net/transport2.h`: the game thread never touches ENet
objects; `src/net/transport2.cpp` runs the worker, `src/net/session2.cpp`
starts it via `Transport2_StartHost`/`Transport2_StartJoin`). Liveness is
`Transport2Stats.protocol_silence_ms`, consumed by the connection supervisor.
`src/net/enet_transport.cpp` provides the ENet helpers and the autopunch relay
client (`AutopunchSendRegister`, `AutopunchSendLookup`) for symmetric-NAT
punch assist.

Each peer owns a local input delay and rollback budget
(`include/net/delay_policy.h`), exchanged during config exchange
(`DelayPolicy_BuildNegotiationData`, `DelayPolicy_NegotiateSession`); latency
is measured by `TimeProbe` percentiles with ENet's average as fallback.
Pre-game (character select, stage select) runs epoch-scoped frontend lockstep:
`src/net/frontend_input_sync.cpp` (`FrontendInputSync_BeginEpoch`,
`ConsumeCurrentFrame`), `src/net/charsel_sync.cpp` (`CharSelSync_*`),
`src/net/stagesel_sync.cpp` (`StageSelSync_*`), with `src/net/match_setup.cpp`
driving the BootstrapLoading → BootstrapBaseline → BootstrapReady phases and
minting the `LockedMatchConfig`. Cross-peer phase transitions commit through
`src/net/transition_barrier.cpp` (`TransitionBarrier_Propose`,
`TransitionBarrier_IsCommitted`, `TransitionBarrier_ConsumeCommit`) —
wire-acknowledged, idempotently re-acked proposals. NAT traversal
(`src/net/nat_traversal.cpp`) stacks libjuice (ICE/STUN/TURN), miniupnpc
UPnP, and PCP/NAT-PMP mappings behind `Nat_StartServices` /
`Nat_AddPortMapping`.

## Spectator system

Spectators are not rollback peers: they consume archived inputs and match
metadata over a dedicated sidecar protocol
(`include/net/spectator_protocol.h`). The host archives confirmed frames
(`include/net/spectator_runtime.h`: `SpectatorRuntime_OnConfirmedFrame`) and a
separate ENet host serves them (`src/net/spectator_manager.cpp`) so the
strictly single-peer gameplay transport is untouched. The viewer side
(`src/net/spectator_client.cpp`: `SpectatorClient_StartConnect`,
`SpectatorClient_BeginLanDiscovery` — UDP broadcast on `LAN_DISCOVERY_PORT`)
buffers the archive, while `src/net/spectator_playback.cpp` owns the local
simulation and paces catch-up (`SpectatorPlayback_OwnsLocalSimulation`,
`SpectatorPlayback_FrameUpdate`). A spectator re-enters each following match
via `SpectatorClient_ArmForNextMatch`.

## Replay system

Replays use the game's native one-file-per-match format, extended by the mod:
a palette trailer (`kReplayPaletteTrailerMagic`) and, for netplay matches, a
confirmed-stream trailer (`kReplayConfirmedTrailerMagic`) with per-frame
confirmed inputs and periodic digests, recorded from the engine's confirm seam
(`ReplayRuntime_OnConfirmedFrame`) — predicted inputs never reach a replay
file. All of this lives in `src/replay/replay_runtime.cpp`, which hooks the
game's native save path (`Hook_ReplaySave`, installed by
`ReplayRuntime_InstallHooks`). Playback supports pause, step, rewind
(`RewindToFrame`, with coarse checkpoints), speed steps, and player takeover
(`BeginTakeover`/`ExitTakeover` around a `GameSnapshot` of the takeover
start).

## Training mode

Practice tooling sits behind the `PracticeTools_*` API
(`include/training/practice_tools.h`, implemented in
`src/training/practice_runtime.cpp`): dummy automation, position presets,
value editors, and the settings bridge. Auto-block
(`src/training/auto_block.cpp`) plans guard lanes from live attack masks and
arms defensive windows through the engine's own setters; its hooks
(`src/patches/practice_defense_hooks.cpp`) run at contact time. Frame
advantage is measured around command dispatch
(`FrameAdvantage_OnPreCommandDispatch`/`OnPostCommandDispatch`,
`src/training/frame_advantage.cpp`). The hitbox viewer
(`src/ui/hitbox_viewer.cpp`, `HitboxViewer_Render`) draws from entity memory
during offline play.

## Palette system

Custom palettes are edited in the ImGui editor (`src/ui/palette_editor.cpp`:
`PaletteEditor_Init`, `PaletteEditor_Render`) into fixed-size
`NetplayPaletteBank`s (`include/net/netplay_palette_runtime.h`), persisted by
`src/net/netplay_palette_storage.cpp` (`NetplayPaletteStorage_SaveBank`).
Injection hooks the game's image pipeline in
`src/patches/palette_asset_hook.cpp` (`PaletteAssetHook_Install` and the
`Hook_*` asset functions): custom palette bytes replace the vanilla sprite
palette bytes as character assets decode and upload. Netplay sync is a
control plane (`src/net/netplay_palette_runtime.cpp`) that transports and
acknowledges banks; spectators and replays receive the same banks over their
own channels.

## Config, logging, tests

User-facing files, created on first launch: `as2_input.cfg`
(`InputSystem_LoadConfig`/`InputSystem_SaveConfig`) and `as2_netplay.cfg`
(`kConfigFile` in `src/net/netplay_menu_controller.cpp`). The d3d9 proxy adds
`as2_rollback_settings.ini`; the link emulator reads `as2_stress.cfg`; the
test harness drives connections through `as2_autoconnect.cfg`. Logging goes
through the async netplay log (`include/rollback/netplay_log.h`) plus the
proxy-owned startup log with heartbeat.

The build (see `CMakeLists.txt`) produces three Win32 DLLs — `as2_rollback`,
`d3d9`, `wsock32` — the `as2_test_harness` dashboard
(`tools/test_harness_launcher.cpp`), and twelve ctest suites covering the
engine core, determinism, frontend sync, transition barriers, frame math and
scheduler, async logging, the module loader, the link emulator, auto-block,
native recovery, and training logic. The `dist` target assembles a clean game
folder overlay and the release zip via `cmake/make_dist.cmake`.
