# Shell hotkey / IME policy (Alice Senki 2 mod)

**Read this before changing Win key, Alt+Shift layout switch, middle-click scroll, or IME behavior.**

## ROOT CAUSE of Win key + Alt+Shift (confirmed 2026-06-01)

`Game_MainLoop` (`sub_5D2AC0` @ `0x005D2AC0`) calls **`keybd_event(7, 0, KEYEVENTF_KEYUP, 0)` on every frame**
(decompiled call at line 266424). `VK 0x07` is a reserved/undefined virtual key. Continuously
injecting a phantom key-up is the classic hook-free "disable the Windows key" technique:

- The shell opens **Start** only on a clean `Win`-down → `Win`-up with no intervening key event.
- The **Alt+Shift** layout-switch hotkey likewise needs a clean chord.
- The per-frame phantom event always lands between the real down/up, so the shell treats it as a
  chord and suppresses both behaviors. **Alt+Space still works** because it is handled directly by
  `DefWindowProc` on the real keydown, not by clean-sequence shell detection.

This is **ungated** (runs in vanilla, windowed and fullscreen) — which is why the issue reproduced
without the mod. It is unrelated to DInput: a diagnostic that fully unacquired the DInput keyboard
(and confirmed the mouse was never DInput-acquired) changed nothing.

**Fix (current):** `wsock32_proxy.cpp` IAT-hooks `keybd_event` (`Hooked_keybd_event`) and drops
exactly the `bVk == 0x07` injections, passing all other keystrokes through. Also redirected via the
`GetProcAddress` hook in case of dynamic resolution. The dead `0x9E5B74` suppression feature
(`SPI_SETSCREENSAVERRUNNING` + `SetMSGHookDll`) is never armed in this build and is NOT the cause.

> NOTE: `dword_9E5B74` is declared weak and **never assigned** anywhere in the binary — the entire
> in-game shell-suppression path is dead code. Do not waste time patching its branches.

## Problem we are solving

The game uses **DXLib**, which stores a **magic `WNDPROC` handle** (`0xFFFFxxxx`) on the game `HWND`, not a pointer to `sub_633490` (`0x00633490`). `d3d9_proxy` subclasses that window:

```
DispatchMessage
  → HookedWndProc (ImGui)
    → ProxyWndProc (d3d9)
      → CallWindowProcW(0xFFFFxxxx)   ← DXLib stub; does NOT call sub_633490
```

A MinHook on `sub_633490` alone **does nothing** for messages that never reach that function. Logs must show `ModCallGameWndProc` / `hooked-sub_633490`, not only `ProxyWndProc-exit original=0xFFFFxxxx`.

Vanilla swallowing (when messages *do* reach `sub_633490`):

| Address / symbol | Role |
|------------------|------|
| `0x9E5B74` | Shell suppress flag; enables MSG hook + `SC_KEYMENU` return 0 |
| `0x9DB660` | Custom wndproc gate; skip `DefWindowProcA` epilogue when set |
| `sub_633490` case `0xF100` (`SC_KEYMENU`) | Returns 0 when suppress on |
| `sub_633490` LABEL_129 | Hides cursor on mouse down (breaks middle-click scroll) |

## Layered fix model (use all that apply)

| Layer | When | What |
|-------|------|------|
| **A. wsock32 @ `DLL_PROCESS_ATTACH`** | Before DXLib window setup | Block APIs that *arm* suppression; clear hook DLL state; pin globals |
| **B. Verified `.exe` NOPs** | Same attach (wsock) or rollback init | Remove swallow branches inside `sub_633490` even if flags flip |
| **C. `ModCallGameWndProc`** | After d3d subclasses HWND | Route past DXLib `0xFFFFxxxx` stub into hooked `sub_633490` |

**Do not expect layer A or B alone to fix Win/Start** if messages never reach `sub_633490` (layer C is still required).

### Layer A — already in `wsock32_proxy.cpp`

On process attach (`PatchEarlyHotkeyImports`):

- Clears `0x9E5B74`, unhooks `0x9E5B7C`, frees hook module — suppression never arms from existing state
- IAT: block `GetProcAddress("SetMSGHookDll")`, block `SystemParametersInfoA(0x61)`, block `WINNLSEnableIME(FALSE)`
- Same pattern as `input_override` hooks, but **earlier** (before `as2_rollback` loads)

This is the right place to **prevent** the game from installing swallow machinery. It is **not** a substitute for routing (layer C) or wndproc code NOPs (layer B).

### Layer B — startup NOPs (implemented with byte verification on shipping `as2.exe`)

Patch in wsock attach (or shared early patch helper), only when expected bytes match:

1. **Cursor-hide loop (`LABEL_129`)** @ RVA `0x234107`: bypass the `SetCursor(0)` / `ShowCursor(0)` loop with unconditional branch. This is intentionally broad (shared mouse path) to prevent hover/highlight flicker and middle-click autoscroll breakage.
2. **`SC_TASKLIST` (`0xF170`)** @ RVA `0x233E21` and **`SC_SCREENSAVE` (`0xF140`)** @ RVA `0x233E38`: convert the swallow `jne`/`return 1` to unconditional `jmp` so Ctrl+Esc / task-switch reach default handling.
3. **DefWindowProc epilogue gate** `if (dword_9DB660 != 1)` @ RVA `0x233F25` and **custom-proc guard** (`0x9DB668`) @ RVA `0x2334B5`: both touch live globals; kept.

> The former `SC_KEYMENU` (`0x233DFE`) and shell-helper arming (`0x233979`) patches were
> removed — both gated on `dword_9E5B74`, which is never assigned in this build (dead code).
> The DInput coop-arg patches (`0x22F1A6`/`0x22F2F1`) were also removed (DInput is irrelevant
> to shell hotkeys). The real Win/Alt+Shift fix lives in layer A (keybd_event VK 0x07 drop).

**Do not** NOP these from d3d9 with synthetic `SC_TASKLIST` / `ActivateKeyboardLayout` — that is forbidden.

### Layer C — DXLib stub bypass

`d3d9_proxy` **`CallGameWndProcChain`** → **`ModCallGameWndProc`** → MinHook `sub_633490`. Required because `CallWindowProcW(0xFFFFxxxx)` never calls game code.

Deterministic fallback rule for shell/layout/IME messages:

- Call game hook first.
- If game hook returns `0`, call prior wndproc.
- If both return `0`, call `DefWindowProcA/W` (ANSI/Unicode-aware) and return that result.

This is allowed because it preserves the full chain order and only uses OS default handling after both game paths decline the message.

## Allowed fixes (in priority order)

1. **Route wndproc through hooked game code** (layer C) — `ModCallGameWndProc` → MinHook trampoline → vanilla `sub_633490` → `DefWindowProcA` at `LABEL_194`.

2. **Neutralize vanilla suppression** (layer A + rollback) — clear `0x9E5B74`, block `SetMSGHookDll` / `SPI 0x61`; `InputOverrideGameWndProcShellGate` for `0x9DB660` during shell handling.

3. **Binary patches in game `.exe`** (layer B, offsets verified): bypass cursor-hide loop (middle-click), `SC_TASKLIST` / `SC_SCREENSAVE` swallow, and the live `DefWindowProc` / custom-proc gates.

4. **Mouse-only post-processing** in `Hook_GameWndProc`: restore cursor after middle-button handling in vanilla `LABEL_129` (game bug, not OS shell).

5. **Win key / Alt+Shift**: drop the per-frame `keybd_event(VK 0x07)` phantom key the game injects in `Game_MainLoop` (`sub_5D2AC0`). This is the actual fix — see root-cause section. DInput coop level is NOT involved.

## Forbidden — do NOT implement (user rejected repeatedly)

These are **proxy-layer mitigations**. They may appear to “work” in testing but are **not** acceptable fixes:

| Forbidden | Why |
|-----------|-----|
| `SendMessage` / `PostMessage` **`WM_SYSCOMMAND` + `SC_TASKLIST`** for Win | Synthetic Start menu; not vanilla game path |
| **`ActivateKeyboardLayout` / `LoadKeyboardLayout`** for Alt+Shift | Synthetic layout switch |
| **`SendInput` / `keybd_event`** for shell keys | Fake input |
| **`DefWindowProc` only in d3d9** before/instead of game chain | Bypasses `sub_633490`; proved insufficient (result 0, no OS UI). Fallback after game+prior both return 0 is allowed. |
| **d3d9 “shell passthrough”** that intercepts shell keys and never calls game wndproc | Same class of hack |
| **Clearing `0x9DB660` only in RAM** each frame without fixing call chain | Mitigation |

If `DefWindowProc` on the subclassed `HWND` returns 0 and Start still does not open, the next step is **game wndproc + verified patches**, not synthetic shell APIs.

## Verification (logs)

After a fix, with `input_guard_hotkey_trace=1`:

- `[CREATEDEVICE] Prior WndProc is DXLib magic stub` — expected.
- Shell keys should show **`[ShellHotkey] CallGameWndProc`** or **`hooked-sub_633490`**, not only `shell-dispatch` + `DefWindowProc`.
- Win tests: look for vanilla reaching **`LABEL_194` / `DefWindowProcA`** inside game code (or `WM_INPUTLANGCHANGEREQUEST` for layout), not `WM_SYSCOMMAND-SC_TASKLIST` from mod.

## Startup observability contract

Keep these trace families enabled and bounded:

- `wsock32_proxy`: `[STARTUPTRACE][wsock32]` state snapshots from process attach to mode-3 marker.
- `as2_rollback`: `[STARTUPTRACE][Provenance]` transitions for `0x9E5B74/0x9E5B7C/0x9E5B80/0x9E5C8C/0x9DB660/0x9DB668/0x9E5CB8`.
- `as2_rollback`: `[STARTUPTRACE][CreateFileA]` and `[STARTUPTRACE][AssetLoad]` title marker (`data\\tit.bin`) timeline.
- `as2_rollback`: `[STARTUPTRACE][ShellWndProc]` per-message classification for shell/layout/IME path decisions.

## Auditable sensitive patch set

Layer-B sensitive RVAs currently applied (`PatchVanillaWndprocSwallowBranches`), each
verified byte-for-byte before write:

- `0x233E21` (`SC_TASKLIST` swallow bypass) — keeps Ctrl+Esc / task-switch working
- `0x233E38` (`SC_SCREENSAVE` swallow bypass)
- `0x233F25` (`DefWindowProc gate` bypass, live `0x9DB660`)
- `0x2334B5` (`custom-proc guard` bypass, live `0x9DB668`)
- `0x234107` (`cursor-hide loop` bypass) — the middle-click / hover fix

**Removed (2026-06-01) as dead-end / ineffective:**

- `0x233DFE` (`SC_KEYMENU` swallow bypass) — gated on `dword_9E5B74`, never assigned → no-op
- `0x233979` (`shell-helper arming guard` bypass) — same `dword_9E5B74` dead gate
- `0x22F1A6`, `0x22F2F1` (DInput keyboard coop `0x0A→0x06`) — DInput proven irrelevant to
  shell hotkeys by the unacquire experiment; reverted to vanilla. The runtime DInput coop
  "repair" (`ShouldRepairDInputKeyboardCooperativeLevel`) is likewise disabled.

The real Win key / Alt+Shift fix is the **`keybd_event(VK 0x07)` phantom-key drop** in
`PatchEarlyHotkeyImports` (see root-cause section at top).

**Deployment note (2026-08-17):** this whole EARLYPATCH layer lives in `wsock32.dll`.
It was never part of the CMake post-build deploy, so game directories silently ran
stale pre-fix copies of the proxy (the dev game dir carried an April build until
today). `wsock32` now has the same `POST_BUILD copy_if_different` as `d3d9` /
`as2_rollback` — if the hotkey/middle-click fixes look "missing", check the
`wsock32_proxy_<pid>.log` for `[EARLYPATCH]` lines first; a stale DLL logs none.

## Window-resize collateral of the `0x9DB660` gate (fixed 2026-08-17)

Forcing `0x9DB660 = 0` (`ForceDisableCustomWndProcPaths`, needed to kill the vanilla
swallow paths) has a side effect outside the wndproc: DXLib's windowed-size enforcer
`sub_63a110` (`0x63A110`) begins with `if (dword_9DB660 != 1)` and otherwise
compares the client rect against the engine's expected size, `MoveWindow`ing it back
on any mismatch. Retail users always run with the gate armed (=1), so retail never
executes this function and the window can be freely drag-resized. With the gate
forced to 0, the DXLib wndproc invoked the enforcer on `WM_SIZE` for every `wParam`
except `SIZE_MAXIMIZED` (plus once per frame from `ProcessMessage`), so every
edge/corner drag snapped back to 640x480 within a frame — "maximize works, nothing
else does".

Fix: `ShellHotkeyPatch_Install` also MinHooks `sub_63a110` to a `return 0` no-op
(`Hook_WindowSizeEnforcer`), which reproduces the retail gate==1 skip exactly. Window
sizing remains owned by the d3d9 proxy (WM_SIZING 4:3 keeper + scaling swap chain).
Do not "fix" resize by restoring the gate to 1 — the live gate also swallows
`DefWindowProc` for unhandled messages unless the wsock32 byte patches are present.

## Key files

| File | Responsibility |
|------|----------------|
| `mod/src/patches/shell_hotkey_patch.cpp` | MinHook `sub_633490`; `ShellHotkey_CallGameWndProc`; mouse cursor restore |
| `mod/d3d9_proxy/d3d9_proxy.cpp` | `CallGameWndProcChain` — never shell-intercept; bypass DXLib stub |
| `mod/src/patches/input_override.cpp` | Suppress hooks, DInput, optional Win strip in game reads only |
| `mod/winsock_proxy/wsock32_proxy.cpp` | Early suppress clear + IAT blocks |
| `mod/docs/STARTUP_INPUT_VALIDATION.md` | Evidence-backed pass/fail matrix for latest cold run |

## Agent checklist

- [ ] Did I avoid all items in **Forbidden**?
- [ ] Do shell messages reach **`sub_633490`** (hooked), not only `0xFFFFxxxx` stub?
- [ ] If adding d3d9 wndproc logic, is it **routing only** (no synthetic OS actions)?
- [ ] Did I update this doc if architecture changed?
