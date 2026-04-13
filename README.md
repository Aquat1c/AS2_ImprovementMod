# Alice Senki 2 Improvement Mod

Alice Senki 2 Improvement Mod is a DLL-proxy runtime mod for Alice Senki 2. It adds rollback netplay, spectator support, replay tools, training utilities, custom palettes, and a lightweight mod loader without modifying the original game executable on disk.

## Highlights

- Rollback netplay with stage select sync, palette sync, exact-build validation, and NAT traversal
- Spectator support with frame archival, local watch playback, relay-aware connect, and LAN discovery
- Replay tools with pause, step, rewind, HUD controls, and player takeover
- SDL3 input, training hotkeys, manual savestates, hitbox viewer, and ImGui diagnostics
- Full unlock and Japanese locale compatibility patches

## Install

For a normal install, copy these files into the same folder as `アリス戦記２.exe`:

- `d3d9.dll`
- `as2_rollback.dll`
- `wsock32.dll`
- `SDL3.dll`

If the game directory already has another `d3d9.dll`, back it up first.

On first launch, the mod creates its config files automatically. The main user-facing files are:

- `as2_input.cfg` for keyboard and gamepad bindings
- `as2_netplay.cfg` for nickname, ports, delay, spectator, and related netplay settings

## Build From Source

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --target as2_rollback d3d9 wsock32
cmake --install build --config Release
```

Build Win32 only. The original game is 32-bit. `cmake --install` deploys the runtime DLLs into the configured game directory.

## Key Binds

Core:

- `F1`: Toggle the in-game mod menu
- `F5` / `F6`: Save or load a manual savestate during gameplay

Training mode:

- `F4`: Toggle the hitbox viewer
- `F7`: Pause or unpause
- `F8`: Advance one frame while paused
- `F9`: Swap P1/P2 controls

Replay mode:

- `Bksl`: Pause or unpause replay playback
- `[` / `]`: Step backward or forward while paused
- `Shift+[`: Rewind replay
- `+` / `-`: Change replay speed
- `Insert`: Toggle the replay HUD
- `1` / `2`: Start or restart replay takeover for P1 or P2
- `0`: Exit replay takeover and restore the base replay timeline

## Documentation

The full architecture, implementation notes, hotkeys, game-state reference, and troubleshooting guide live in [docs/MASTER_DOCUMENT.md](docs/MASTER_DOCUMENT.md).

## Notes

- `wsock32.dll` is required for the duplicate-instance bypass used by local testing and the harness.
- Build `as2_test_harness` separately when you need the two-instance test dashboard.
