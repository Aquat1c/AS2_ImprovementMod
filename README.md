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

Boss Characters can be enabled or disabled in General Settings. Online play enables
them only when both players opted in before connecting; otherwise they stay off
for the entire set. Each player's preference is restored when the session ends.
Spectators follow the players' agreement and recover their own preference afterward.
Players and spectators must use the updated build.

On the Join screen, select Host Address and press C to paste a copied host:port.
Press D to watch that host directly, without attempting to join as a player.
While editing text, use Ctrl+V to paste.

## Build From Source

```powershell
git submodule update --init
cmake -S . -B build -A Win32 -DGAME_DIR="D:\Path\To\Game"
cmake --build build --config Release --target as2_rollback d3d9 wsock32
cmake --install build --config Release
```

Build Win32 only. The original game is 32-bit. The submodules provide ENet, MinHook, libjuice, miniupnpc, and libpcpnatpmp. `-DGAME_DIR` points at your game folder; post-build steps copy the freshly built DLLs there and `cmake --install` deploys them. Without it the DLLs are built into `build/Release` and nothing is deployed.

### Package a release

```powershell
cmake --build build --config Release --target dist
```

This writes `dist/アリス戦記２/` (a clean game folder plus the freshly built `d3d9.dll`, `as2_rollback.dll`, `wsock32.dll`, `SDL3.dll`, `run_as2_linux.sh`, the player `README.txt` (from `README_PLAYER.txt`), `BUILD_INFO.txt` and a `MANIFEST.sha256`) and `dist/as2im_<version>.zip` with those contents at the archive root. The clean game folder defaults to `build/Release/アリス戦記２`; point `-DAS2_DIST_BASE=<folder>` elsewhere if needed. The zip name takes the version from `AS2_MOD_VERSION` at the top of `CMakeLists.txt`. Logs, per-user configs, replays and test binaries in the base folder are never packaged.

On Linux, run `./run_as2_linux.sh` from the game folder; it sets the Wine DLL overrides itself.

## Third-Party Projects

The mod is built on a small set of vendored or workspace-pinned projects:

| Project | Location | Used for |
|---------|----------|----------|
| [ENet](https://github.com/lsalzman/enet) | `lib/enet` | UDP transport, channels, reliability, RTT/loss stats |
| [SDL3](https://github.com/libsdl-org/SDL) | `lib/SDL3` | Keyboard/gamepad input runtime (`SDL3.dll` ships next to the game) |
| [Dear ImGui](https://github.com/ocornut/imgui) | `lib/imgui` | In-game overlay, debug UI, test harness UI |
| [MinHook](https://github.com/TsudaKageyu/minhook) | `lib/minhook_src` | Runtime API and game-function hooks |
| [libjuice](https://github.com/paullouisageneau/libjuice) | `third_party/libjuice` | STUN/TURN/ICE-style NAT traversal support |
| [miniupnpc](https://github.com/miniupnp/miniupnp) | `third_party/miniupnp_suite/miniupnpc` | UPnP router port mapping |
| [libpcpnatpmp](https://github.com/libpcp/pcp) | `third_party/libpcpnatpmp` | PCP/NAT-PMP router mapping fallback |
| [autopunch](https://github.com/delthas/autopunch) | local implementation | UDP rendezvous and hole-punch behavior compatible with the public autopunch relay protocol |

Autopunch is not linked as an external library; the mod implements the relevant client-side rendezvous behavior directly and logs the configured relay endpoint for diagnostics.

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

- `Esc`: Exit replay playback or the replay browser and return to the netplay menu
- `Bksl`: Pause or unpause replay playback
- `[` / `]`: Step backward or forward while paused
- `Shift+[`: Rewind replay
- `+` / `-`: Change replay speed
- `Insert`: Toggle the replay HUD
- `1` / `2`: Start or restart replay takeover for P1 or P2
- `0`: Exit replay takeover and restore the base replay timeline

## Documentation

- [How it works](docs/HOW_IT_WORKS.md) — the runtime architecture: proxy stack, boot chain, hooks, rollback engine, netplay, spectator, replays, training tools, and palettes, with source citations.
- The full architecture, implementation notes, hotkeys, game-state reference, and troubleshooting guide live in [docs/MASTER_DOCUMENT.md](docs/MASTER_DOCUMENT.md).

## Notes

- `wsock32.dll` is required for the duplicate-instance bypass used by local testing and the harness.
- Build `as2_test_harness` separately when you need the two-instance test dashboard.

### Training palette editor

Open **Display → Palette Editor** in the training pause menu. The preview starts on the current fighter frame. Choose P1/P2, then select a colour group or an individual shade. Confirm opens the saturation/value picker; confirm again selects hue. Back returns to the swatches. **All colours** exposes individual entries within the used palette range; unused trailing entries stay hidden. Mouse controls and the configured gamepad/keyboard controls work in the same editor; C undoes and D redoes.

Changes preview live. **Save** opens a compact 1-8 palette list and asks before replacing an occupied slot; leaving or switching players with unsaved edits offers Save, Discard, or Keep editing. Transparency is preserved. Shade groups are inferred from adjacent colours; use individual shades when a group does not match the part you want to edit.

Fullscreen defaults to F11 and can be rebound or cleared in the hotkey settings.

In the training palette editor, drag the sprite to pan; use the mouse wheel over the preview or the -/+ buttons to zoom from 25% to 400%. Right-click **Frame** to toggle the current fighter frame. Click the HEX value to edit six RGB digits (type, or select digits with Left/Right and change them with Up/Down). **Original** toggles a temporary comparison without changing the edits.
