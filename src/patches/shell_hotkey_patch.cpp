/**
 * Vanilla shell-input patch — game wndproc (sub_633490) only.
 *
 * Policy: mod/docs/SHELL_HOTKEY_POLICY.md
 * DO NOT add SC_TASKLIST, ActivateKeyboardLayout, SendInput, or proxy DefWindowProc
 * shortcuts for Win / Alt+Shift. Fix the DXLib stub chain and vanilla swallow paths.
 */

#include "patches/shell_hotkey_patch.h"

#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"
#include "patches/input_override.h"
#include "patches/memory_utils.h"
#include "core/game_state.h"

#include <stdint.h>
#include <windows.h>

typedef LRESULT(CALLBACK* GameWndProc_t)(HWND, UINT, WPARAM, LPARAM);
typedef int(__cdecl* WindowSizeEnforcer_t)();

static GameWndProc_t g_origGameWndProc = nullptr;
static WindowSizeEnforcer_t g_origWindowSizeEnforcer = nullptr;
static bool s_installed = false;
static bool s_sizeEnforcerHooked = false;

// sub_63a110 no-op (window resize fix).
//
// Retail behavior: the vanilla msg-hook helper sets the 0x9DB660 gate to 1 at
// startup, and sub_63a110 starts with `if (gate != 1)` — so in an unmodded
// game this per-frame "snap the window back to the expected client size"
// enforcement NEVER runs and the user can freely drag-resize the window.
//
// This patch forces the gate to 0 (ForceDisableCustomWndProcPaths, to kill the
// vanilla shell-key swallow paths), which as a side effect re-armed the
// enforcer: the DXLib wndproc calls it on WM_SIZE for every wParam except
// SIZE_MAXIMIZED, and ProcessMessage calls it per frame, so every edge-drag
// resize snapped back to 640x480 within a frame (maximize alone survived).
//
// No-oping the function reproduces the retail (gate==1) skip exactly: it
// always returns 0 and no caller consumes its side effects in the gate==1
// world retail users run in. Window sizing stays owned by the d3d9 proxy
// (WM_SIZING aspect keeping + scaling swap chain).
static int __cdecl Hook_WindowSizeEnforcer() {
    return 0;
}

static bool IsMiddleMouseMessage(UINT msg) {
    return msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP || msg == WM_MBUTTONDBLCLK;
}

static bool IsWinKeyMessage(UINT msg, WPARAM wParam) {
    return (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
           (wParam == VK_LWIN || wParam == VK_RWIN);
}

static bool IsAltShiftLayoutKeyMessage(UINT msg, WPARAM wParam) {
    if (!(msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        return false;
    }
    if (!(wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
          wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU)) {
        return false;
    }

    const bool msgCarriesAlt =
        (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
        (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU);
    const bool msgCarriesShift =
        (wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT);

    const bool altDown = msgCarriesAlt ||
        ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0) ||
        ((::GetAsyncKeyState(VK_LMENU) & 0x8000) != 0) ||
        ((::GetAsyncKeyState(VK_RMENU) & 0x8000) != 0);
    const bool shiftDown = msgCarriesShift ||
        ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) ||
        ((::GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) ||
        ((::GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
    return altDown && shiftDown;
}

static bool IsLayoutOrSystemMenuMessage(UINT msg, WPARAM wParam) {
    if (msg == WM_INPUTLANGCHANGEREQUEST || msg == WM_INPUTLANGCHANGE) {
        return true;
    }
    return msg == WM_SYSCOMMAND && ((wParam & 0xFFF0u) == SC_KEYMENU);
}

static LRESULT CallDefaultWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool unicode = (::IsWindowUnicode(hwnd) != FALSE);
    if (unicode) {
        const LRESULT wideResult = ::DefWindowProcW(hwnd, msg, wParam, lParam);
        if (wideResult != 0) {
            return wideResult;
        }
        // Defensive fallback for mixed ANSI/Unicode subclass chains.
        return ::DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    const LRESULT ansiResult = ::DefWindowProcA(hwnd, msg, wParam, lParam);
    if (ansiResult != 0) {
        return ansiResult;
    }
    // Defensive fallback for mixed ANSI/Unicode subclass chains.
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ForceDisableCustomWndProcPaths() {
    WriteMemory<int32_t>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER, 0);
    WriteMemory<intptr_t>(ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR, 0);
    WriteMemory<intptr_t>(ADDR_GAME_WNDPROC_MSG_CALLBACK, 0);
}

static void RestoreCursorAfterMiddleButton(HWND hwnd) {
    ::ClipCursor(nullptr);

    while (::ShowCursor(TRUE) < 0) {
    }

    WriteMemory<int32_t>(ADDR_GAME_CURSOR_CURRENT_SHOWN, 1);

    if (hwnd) {
        ::PostMessageA(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd),
                       MAKELPARAM(HTCLIENT, WM_MBUTTONDOWN));
    }
}

static bool IsScKeymenu(UINT msg, WPARAM wParam) {
    return msg == WM_SYSCOMMAND && ((wParam & 0xFFF0u) == SC_KEYMENU);
}

static bool ShouldRetryForImeLang(UINT msg) {
    return msg == WM_INPUTLANGCHANGEREQUEST || msg == WM_INPUTLANGCHANGE;
}

static LRESULT RetryVanillaAfterSuppressClear(HWND hwnd,
                                              UINT msg,
                                              WPARAM wParam,
                                              LPARAM lParam,
                                              LRESULT initialResult) {
    if (initialResult != 0) {
        return initialResult;
    }

    const bool retryKeymenu = IsScKeymenu(msg, wParam);
    const bool retryImeLang = ShouldRetryForImeLang(msg);
    if (!retryKeymenu && !retryImeLang) {
        return initialResult;
    }

    int32_t suppressFlag = ReadMemory<int32_t>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG);
    int32_t customWndprocGate = ReadMemory<int32_t>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER);

    if (suppressFlag == 0 && customWndprocGate == 0) {
        return initialResult;
    }

    WriteMemory<int32_t>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG, 0);
    WriteMemory<int32_t>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER, 0);

    const LRESULT retriedResult = g_origGameWndProc(hwnd, msg, wParam, lParam);
    return retriedResult;
}

static LRESULT CALLBACK Hook_GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ForceDisableCustomWndProcPaths();

    const bool middleMouse = IsMiddleMouseMessage(msg);
    const bool isSetCursor = (msg == WM_SETCURSOR);
    const bool isLayoutOrSystemMenu = IsLayoutOrSystemMenuMessage(msg, wParam);
    const bool isWinKey = IsWinKeyMessage(msg, wParam);
    const bool isAltShift = IsAltShiftLayoutKeyMessage(msg, wParam);

    // Keep vanilla side effects first, then force OS fallback if vanilla returns 0.
    if (isLayoutOrSystemMenu || isWinKey || isAltShift) {
        const LRESULT vanillaResult = g_origGameWndProc(hwnd, msg, wParam, lParam);
        ForceDisableCustomWndProcPaths();
        if (vanillaResult != 0) {
            return vanillaResult;
        }

        const LRESULT defResult = CallDefaultWindowProc(hwnd, msg, wParam, lParam);
        return RetryVanillaAfterSuppressClear(hwnd, msg, wParam, lParam, defResult);
    }

    // Cursor behavior fix remains direct default-proc.
    if (isSetCursor) {
        return CallDefaultWindowProc(hwnd, msg, wParam, lParam);
    }

    InputOverrideGameWndProcShellGate gate;
    const LRESULT result = g_origGameWndProc(hwnd, msg, wParam, lParam);
    ForceDisableCustomWndProcPaths();
    const LRESULT fixedResult = RetryVanillaAfterSuppressClear(hwnd, msg, wParam, lParam, result);

    if (middleMouse) {
        RestoreCursorAfterMiddleButton(hwnd);
    }

    return fixedResult;
}

bool ShellHotkey_IsHookInstalled() {
    return s_installed && g_origGameWndProc != nullptr;
}

bool ShellHotkey_CallGameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* outResult) {
    if (!outResult || !hwnd || !g_origGameWndProc) {
        return false;
    }

    *outResult = Hook_GameWndProc(hwnd, msg, wParam, lParam);
    return true;
}

bool ShellHotkeyPatch_Install() {
    if (s_installed) {
        return true;
    }

    const bool shellWorkaroundsEnabled = InputOverride_AreShellHotkeyImeWorkaroundsEnabled();

    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(ADDR_GAME_WNDPROC),
                                     reinterpret_cast<void*>(&Hook_GameWndProc),
                                     reinterpret_cast<void**>(&g_origGameWndProc));
    if (status != MH_OK) {
        LOG_ERROR("[ShellHotkey] Failed to hook game wndproc at 0x%08X (status=%d)",
                  ADDR_GAME_WNDPROC,
                  status);
        return false;
    }

    WriteMemory<int32_t>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER, 0);
    WriteMemory<intptr_t>(ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR, 0);
    WriteMemory<intptr_t>(ADDR_GAME_WNDPROC_MSG_CALLBACK, 0);
    WriteMemory<int32_t>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG, 0);

    // Forcing the gate to 0 above re-arms the DXLib windowed-size enforcer
    // (skipped in retail where the gate is 1) — no-op it so user window
    // resizing keeps working. See Hook_WindowSizeEnforcer.
    status = MH_CreateHook(reinterpret_cast<void*>(ADDR_GAME_WINDOW_SIZE_ENFORCER),
                           reinterpret_cast<void*>(&Hook_WindowSizeEnforcer),
                           reinterpret_cast<void**>(&g_origWindowSizeEnforcer));
    if (status == MH_OK) {
        s_sizeEnforcerHooked = true;
        LOG_INFO("[ShellHotkey] No-op'd DXLib windowed-size enforcer sub_63a110 at 0x%08X "
                 "(restores user window resize; retail skips it via the 0x9DB660 gate)",
                 ADDR_GAME_WINDOW_SIZE_ENFORCER);
    } else {
        LOG_WARN("[ShellHotkey] Failed to hook windowed-size enforcer at 0x%08X (status=%d) — "
                 "edge-drag window resize will snap back to 640x480",
                 ADDR_GAME_WINDOW_SIZE_ENFORCER,
                 status);
    }

    s_installed = true;
    LOG_INFO("[ShellHotkey] Hooked sub_633490 at 0x%08X (workarounds=%d) — use ModCallGameWndProc (see SHELL_HOTKEY_POLICY.md)",
             ADDR_GAME_WNDPROC,
             shellWorkaroundsEnabled ? 1 : 0);
    return true;
}

void ShellHotkeyPatch_Remove() {
    if (!s_installed) {
        return;
    }

    MH_DisableHook(reinterpret_cast<void*>(ADDR_GAME_WNDPROC));
    MH_RemoveHook(reinterpret_cast<void*>(ADDR_GAME_WNDPROC));
    if (s_sizeEnforcerHooked) {
        MH_DisableHook(reinterpret_cast<void*>(ADDR_GAME_WINDOW_SIZE_ENFORCER));
        MH_RemoveHook(reinterpret_cast<void*>(ADDR_GAME_WINDOW_SIZE_ENFORCER));
        s_sizeEnforcerHooked = false;
    }
    g_origWindowSizeEnforcer = nullptr;
    g_origGameWndProc = nullptr;
    s_installed = false;
}
