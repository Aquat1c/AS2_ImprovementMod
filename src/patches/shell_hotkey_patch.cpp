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

static GameWndProc_t g_origGameWndProc = nullptr;
static bool s_installed = false;
static uint32_t s_callGameWndprocLogCount = 0;
static uint32_t s_scKeymenuRetryLogCount = 0;
static uint32_t s_inputLangRetryLogCount = 0;
static uint32_t s_fastPathLogCount = 0;
static uint32_t s_stateTraceLogCount = 0;

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

static void LogShellPathState(const char* reason, UINT msg, WPARAM wParam, LRESULT vanillaResult, LRESULT fallbackResult) {
    if (s_stateTraceLogCount >= 128) {
        return;
    }
    ++s_stateTraceLogCount;
    int32_t suppress = 0;
    int32_t gate = 0;
    intptr_t custom = 0;
    intptr_t cb = 0;
    __try {
        suppress = *reinterpret_cast<int32_t*>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG);
        gate = *reinterpret_cast<int32_t*>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER);
        custom = *reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR);
        cb = *reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_MSG_CALLBACK);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    LOG_INFO("[STARTUPTRACE][ShellWndProc] %s mode=%u sub=%u msg=0x%03X vk=0x%02X vanilla=0x%p fallback=0x%p suppress=%d gate=%d custom=0x%p cb=0x%p",
             reason ? reason : "unknown",
             static_cast<unsigned>(GetGameMode()),
             static_cast<unsigned>(GetSubstate()),
             msg,
             static_cast<unsigned>(wParam & 0xFFu),
             reinterpret_cast<void*>(vanillaResult),
             reinterpret_cast<void*>(fallbackResult),
             suppress,
             gate,
             reinterpret_cast<void*>(custom),
             reinterpret_cast<void*>(cb));
}

static void ForceDisableCustomWndProcPaths() {
    WriteMemory<int32_t>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER, 0);
    WriteMemory<intptr_t>(ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR, 0);
    WriteMemory<intptr_t>(ADDR_GAME_WNDPROC_MSG_CALLBACK, 0);
}

static void LogCallGameWndprocOnce(UINT msg, WPARAM wParam, LRESULT result) {
    if (s_callGameWndprocLogCount >= 48) {
        return;
    }

    ++s_callGameWndprocLogCount;
    LOG_INFO("[ShellHotkey] CallGameWndProc msg=0x%03X vk=0x%02X result=0x%p trampoline=0x%p",
             msg,
             static_cast<unsigned>(wParam & 0xFFu),
             reinterpret_cast<void*>(result),
             reinterpret_cast<void*>(g_origGameWndProc));
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
    if (retryKeymenu && s_scKeymenuRetryLogCount < 16) {
        ++s_scKeymenuRetryLogCount;
        LOG_INFO("[ShellHotkey] Retried SC_KEYMENU after clearing suppress=%d gate=%d initial=0x%p retried=0x%p",
                 suppressFlag,
                 customWndprocGate,
                 reinterpret_cast<void*>(initialResult),
                 reinterpret_cast<void*>(retriedResult));
    } else if (retryImeLang && s_inputLangRetryLogCount < 16) {
        ++s_inputLangRetryLogCount;
        LOG_INFO("[ShellHotkey] Retried input-language msg=0x%03X after clearing suppress=%d gate=%d initial=0x%p retried=0x%p",
                 msg,
                 suppressFlag,
                 customWndprocGate,
                 reinterpret_cast<void*>(initialResult),
                 reinterpret_cast<void*>(retriedResult));
    }

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
            if (s_fastPathLogCount < 64) {
                ++s_fastPathLogCount;
                LOG_INFO("[ShellHotkey] Shell fallback kept vanilla unicode=%d msg=0x%03X wp=0x%p lp=0x%p vanilla=0x%p",
                         ::IsWindowUnicode(hwnd) ? 1 : 0,
                         msg,
                         reinterpret_cast<void*>(wParam),
                         reinterpret_cast<void*>(lParam),
                         reinterpret_cast<void*>(vanillaResult));
            }
            LogShellPathState("keep-vanilla", msg, wParam, vanillaResult, vanillaResult);
            return vanillaResult;
        }

        const LRESULT defResult = CallDefaultWindowProc(hwnd, msg, wParam, lParam);
        if (s_fastPathLogCount < 64) {
            ++s_fastPathLogCount;
            LOG_INFO("[ShellHotkey] Shell fallback used default-proc unicode=%d msg=0x%03X wp=0x%p lp=0x%p vanilla=0x%p default=0x%p",
                     ::IsWindowUnicode(hwnd) ? 1 : 0,
                     msg,
                     reinterpret_cast<void*>(wParam),
                     reinterpret_cast<void*>(lParam),
                     reinterpret_cast<void*>(vanillaResult),
                     reinterpret_cast<void*>(defResult));
        }
        LogShellPathState("default-fallback", msg, wParam, vanillaResult, defResult);
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

    const bool traceKey =
        msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
        msg == WM_INPUTLANGCHANGEREQUEST || msg == WM_INPUTLANGCHANGE ||
        (msg == WM_SYSCOMMAND && ((wParam & 0xFFF0u) == SC_KEYMENU));

    if (traceKey) {
        LogCallGameWndprocOnce(msg, wParam, *outResult);
    }

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
    g_origGameWndProc = nullptr;
    s_installed = false;
}
