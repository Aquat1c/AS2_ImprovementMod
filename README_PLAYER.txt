Alice Senki 2 Improvement Mod
==============================

This folder contains the original game plus the improvement mod. The mod adds
rollback netplay, spectator support, replay tools, training utilities, custom
palettes, and a mod loader. The original executable is not modified on disk.

Quick start
-----------

1. Start the game as usual (アリス戦記２.exe). The mod loads automatically.
2. Press F1 in-game to open the mod menu.
3. Enter a nickname in the Netplay tab before hosting or joining a match.

Boss Characters is available in General Settings. Both players must enable it
before connecting to use the extra characters online. If either player leaves
it off, bosses are temporarily disabled for the entire set without disconnecting.
The original preferences return when the session ends. Spectators follow the
players' agreement, regardless of their own setting.

On Join, highlight Host Address and press C to paste host:port. Press D to
watch that host directly without joining as a player. Use Ctrl+V while editing.

Included files
--------------

- d3d9.dll        - mod loader proxy (part of the mod, not the system file)
- as2_rollback.dll - the mod itself
- wsock32.dll     - proxy for the duplicate-instance bypass
- SDL3.dll        - input runtime used by the mod
- README.txt      - this file

On first launch the mod creates:

- as2_input.cfg    - keyboard and gamepad bindings
- as2_netplay.cfg  - nickname, ports, delay, spectator settings

Hotkeys
-------

- F1              toggle the mod menu
- F4              hitbox viewer (training)
- F5 / F6         save / load manual savestate
- F7 / F8         pause / frame advance (training)
- F9              swap P1/P2 controls (training)
- Esc             exit replay playback or browser
- Backslash       pause/unpause replay
- [ / ]           step replay backward/forward
- Shift+[         rewind replay
- + / -           replay speed
- Insert          toggle replay HUD
- 1 / 2 / 0       replay takeover P1 / P2 / exit takeover

Linux (Wine)
------------

Run ./run_as2_linux.sh from this folder; it sets the required DLL overrides.

Uninstall
---------

Delete d3d9.dll, as2_rollback.dll, wsock32.dll, and SDL3.dll from this folder.
The game runs without them. Your settings and replays remain in the cfg and
replay files and can be deleted separately.

Training palette editor: pause > Display > Palette Editor.
The preview starts on the current fighter frame. Select a colour group or shade, then confirm for the saturation/value picker and again for hue. Back returns to the swatches. Use the mouse or configured controls. Unused trailing colours stay hidden. C: undo. D: redo. Save opens a palette-slot chooser and confirms overwrites; leaving with unsaved changes offers Save / Discard / Keep editing. Transparency stays unchanged.

Fullscreen: F11 by default; rebind or clear it in the hotkey settings.

Palette preview: drag to pan, wheel or -/+ to zoom, right-click Frame toggles the current frame, click HEX to edit RGB. Original toggles comparison without changing edits.
