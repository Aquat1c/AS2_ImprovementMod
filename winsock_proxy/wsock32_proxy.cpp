/**
 * Minimal wsock32.dll proxy — duplicate-instance bypass
 *
 * The game statically imports WSOCK32.dll, so this DLL loads at process start
 * (before DXLib_Init).  We use that timing for duplicate-instance bypass and
 * early hotkey/IME workarounds (see mod/docs/SHELL_HOTKEY_POLICY.md — layer A).
 * Optional layer-B .exe NOPs belong here once bytes are verified on the shipping binary.
 *
 * All 16 wsock32 functions the game actually uses are forwarded to the real
 * wsock32.dll via GetProcAddress.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <imm.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// ============================================================================
// Logging (minimal — just a file, no console)
// ============================================================================

static FILE* g_logFile = nullptr;
static bool g_loggedSetMsgHookBlock = false;
static bool g_loggedSpi61Block = false;
static bool g_loggedImeDisableBlock = false;
static unsigned g_loggedImmSetOpenStatusCalls = 0;
static unsigned g_loggedImmNotifyCalls = 0;
static unsigned g_loggedImmSetCompositionCalls = 0;
static unsigned g_loggedSetWindowsHookExBlocks = 0;
static DWORD g_processStartTick = 0;
static HWND g_gameWindow = nullptr;
static DWORD g_shellChordLastSeenTick = 0;
static uint32_t g_startupTraceCount = 0;
static int32_t g_lastStartupMode = -1;
static int32_t g_lastStartupSubstate = -1;
static int32_t g_lastStartupExitCode = -1;
static int32_t g_lastStartupSuppress = -1;
static int32_t g_lastStartupLoaded = -1;
static intptr_t g_lastStartupMsgHook = -1;
static intptr_t g_lastStartupHookModule = -1;
static intptr_t g_lastStartupCustomProc = -1;
static intptr_t g_lastStartupMsgCallback = -1;
static bool g_startupTitleReached = false;

static void WsockLog(const char* fmt, ...);
static HHOOK WINAPI Hooked_SetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
static HHOOK WINAPI Hooked_SetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);

static constexpr uintptr_t kAddrShellHotkeySuppressFlag = 0x009E5B74;
// 0x9E5B78 was previously labeled "AuxHook" but is unverified by the decomp
// and does not appear as a named variable — do not call UnhookWindowsHookEx on it.
static constexpr uintptr_t kAddrShellHotkeyMsgHook = 0x009E5B7C;
static constexpr uintptr_t kAddrShellHotkeyLoadedFlag = 0x009E5B80;
static constexpr uintptr_t kAddrShellHotkeyTempDllPath = 0x009E5B84;
static constexpr uintptr_t kAddrShellHotkeyTempDllOwned = 0x009E5C88;
static constexpr uintptr_t kAddrShellHotkeyHookModule = 0x009E5C8C;
static constexpr uintptr_t kAddrGameWndprocCustomProc = 0x009DB668;
static constexpr uintptr_t kAddrGameWndprocMsgCallback = 0x009E5CB8;
static constexpr uintptr_t kAddrGameMode = 0x0081638C;
static constexpr uintptr_t kAddrGameSubstate = 0x00816390;
static constexpr uintptr_t kAddrGameExitCode = 0x00816358;
static constexpr bool kEnableEarlyInputWorkarounds = true;

template <typename T>
static bool ReadMemorySafe(uintptr_t address, T* outValue) {
    if (!outValue) {
        return false;
    }
    __try {
        *outValue = *reinterpret_cast<T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool IsStartupModeValuePlausible(int32_t mode) {
    return mode >= 0 && mode <= 20;
}

static void LogStartupProgressState(const char* reason, bool forceLog) {
    int32_t mode = -1;
    int32_t substate = -1;
    int32_t exitCode = -1;
    int32_t suppress = -1;
    int32_t loaded = -1;
    intptr_t msgHook = -1;
    intptr_t hookModule = -1;
    intptr_t customProc = -1;
    intptr_t msgCallback = -1;

    const bool hasMode = ReadMemorySafe<int32_t>(kAddrGameMode, &mode);
    const bool hasSubstate = ReadMemorySafe<int32_t>(kAddrGameSubstate, &substate);
    const bool hasExit = ReadMemorySafe<int32_t>(kAddrGameExitCode, &exitCode);
    const bool hasSuppress = ReadMemorySafe<int32_t>(kAddrShellHotkeySuppressFlag, &suppress);
    const bool hasLoaded = ReadMemorySafe<int32_t>(kAddrShellHotkeyLoadedFlag, &loaded);
    const bool hasMsgHook = ReadMemorySafe<intptr_t>(kAddrShellHotkeyMsgHook, &msgHook);
    const bool hasHookModule = ReadMemorySafe<intptr_t>(kAddrShellHotkeyHookModule, &hookModule);
    const bool hasCustom = ReadMemorySafe<intptr_t>(kAddrGameWndprocCustomProc, &customProc);
    const bool hasCallback = ReadMemorySafe<intptr_t>(kAddrGameWndprocMsgCallback, &msgCallback);

    const bool changed =
        mode != g_lastStartupMode ||
        substate != g_lastStartupSubstate ||
        exitCode != g_lastStartupExitCode ||
        suppress != g_lastStartupSuppress ||
        loaded != g_lastStartupLoaded ||
        msgHook != g_lastStartupMsgHook ||
        hookModule != g_lastStartupHookModule ||
        customProc != g_lastStartupCustomProc ||
        msgCallback != g_lastStartupMsgCallback;

    const bool reachedTitleNow = IsStartupModeValuePlausible(mode) && mode == 3 && substate >= 0;
    const bool logNow = forceLog || changed || (reachedTitleNow && !g_startupTitleReached);
    if (!logNow || g_startupTraceCount >= 512) {
        return;
    }

    ++g_startupTraceCount;
    WsockLog("[STARTUPTRACE][wsock32] #%u +%lums reason=%s mode=%d(%d) sub=%d(%d) exit=%d(%d) suppress=%d(%d) loaded=%d(%d) msg=0x%p(%d) module=0x%p(%d) custom=0x%p(%d) cb=0x%p(%d) hwnd=0x%p",
             g_startupTraceCount,
             GetTickCount() - g_processStartTick,
             reason ? reason : "unknown",
             mode,
             hasMode ? 1 : 0,
             substate,
             hasSubstate ? 1 : 0,
             exitCode,
             hasExit ? 1 : 0,
             suppress,
             hasSuppress ? 1 : 0,
             loaded,
             hasLoaded ? 1 : 0,
             reinterpret_cast<void*>(msgHook),
             hasMsgHook ? 1 : 0,
             reinterpret_cast<void*>(hookModule),
             hasHookModule ? 1 : 0,
             reinterpret_cast<void*>(customProc),
             hasCustom ? 1 : 0,
             reinterpret_cast<void*>(msgCallback),
             hasCallback ? 1 : 0,
             g_gameWindow);

    if (reachedTitleNow && !g_startupTitleReached) {
        g_startupTitleReached = true;
        WsockLog("[STARTUPTRACE][wsock32] reached mode=3 marker at +%lums (reason=%s)",
                 GetTickCount() - g_processStartTick,
                 reason ? reason : "unknown");
    }

    g_lastStartupMode = mode;
    g_lastStartupSubstate = substate;
    g_lastStartupExitCode = exitCode;
    g_lastStartupSuppress = suppress;
    g_lastStartupLoaded = loaded;
    g_lastStartupMsgHook = msgHook;
    g_lastStartupHookModule = hookModule;
    g_lastStartupCustomProc = customProc;
    g_lastStartupMsgCallback = msgCallback;
}

static void LogShellHotkeySuppressionState(const char* reason) {
    int* suppressFlag = reinterpret_cast<int*>(kAddrShellHotkeySuppressFlag);
    HHOOK* hookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyMsgHook);
    char* tempDllPath = reinterpret_cast<char*>(kAddrShellHotkeyTempDllPath);
    int* tempDllOwned = reinterpret_cast<int*>(kAddrShellHotkeyTempDllOwned);
    HMODULE* hookModule = reinterpret_cast<HMODULE*>(kAddrShellHotkeyHookModule);
    intptr_t* customProc = reinterpret_cast<intptr_t*>(kAddrGameWndprocCustomProc);
    intptr_t* msgCallback = reinterpret_cast<intptr_t*>(kAddrGameWndprocMsgCallback);

    WsockLog("[EARLYPATCH] Shell hotkey state (%s): suppress=%d msg=0x%p module=0x%p owned=%d custom=0x%p cb=0x%p path='%s'",
             reason ? reason : "unknown",
             *suppressFlag,
             *hookHandle,
             *hookModule,
             *tempDllOwned,
             (void*)(*customProc),
             (void*)(*msgCallback),
             tempDllPath);
    LogStartupProgressState(reason ? reason : "shell-state", false);
}

static const char* DescribeSystemParametersAction(UINT uiAction) {
    switch (uiAction) {
    case 0x11u:
        return "SPI_SETSCREENSAVEACTIVE";
    case 0x61u:
        return "SPI_SETFASTTASKSWITCH";
    case 0x2000u:
        return "SPI_GETFOREGROUNDLOCKTIMEOUT";
    case 0x2001u:
        return "SPI_SETFOREGROUNDLOCKTIMEOUT";
    default:
        return "UNKNOWN";
    }
}

static void LogSystemParametersCall(const char* stage,
                                    UINT uiAction,
                                    UINT uiParam,
                                    PVOID pvParam,
                                    UINT fWinIni,
                                    BOOL result) {
    DWORD value = 0;
    if (pvParam) {
        value = *reinterpret_cast<DWORD*>(pvParam);
    }

    WsockLog("[EARLYPATCH] %s SystemParametersInfoA action=0x%X (%s) uiParam=%u pvParam=0x%p value=%lu fWinIni=0x%X result=%d",
             stage ? stage : "Observed",
             uiAction,
             DescribeSystemParametersAction(uiAction),
             uiParam,
             pvParam,
             value,
             fWinIni,
             result ? 1 : 0);
}

static void LogFocusCallState(const char* stage, const char* apiName, HWND hWnd, LONG_PTR resultValue) {
    WsockLog("[EARLYPATCH] %s %s target=0x%p game=0x%p fg=0x%p active=0x%p focus=0x%p result=0x%p elapsed=%lu ms",
             stage ? stage : "Observed",
             apiName ? apiName : "FocusApi",
             hWnd,
             g_gameWindow,
             GetForegroundWindow(),
             GetActiveWindow(),
             GetFocus(),
             (void*)resultValue,
             GetTickCount() - g_processStartTick);
    LogStartupProgressState(apiName ? apiName : "focus-api", false);
}

static void WsockLog(const char* fmt, ...) {
    if (!g_logFile) {
        char dir[MAX_PATH];
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char* slash = strrchr(dir, '\\');
        if (slash) *slash = '\0';

        char logsBase[MAX_PATH];
        snprintf(logsBase, MAX_PATH, "%s\\logs", dir);
        CreateDirectoryA(logsBase, nullptr);

        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\wsock32_proxy_%lu.log",
                 logsBase, GetCurrentProcessId());
        g_logFile = fopen(path, "w");
        if (!g_logFile) return;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

static bool ClearVanillaShellHotkeySuppressionState(const char* reason) {
    int* suppressFlag = reinterpret_cast<int*>(kAddrShellHotkeySuppressFlag);
    HHOOK* hookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyMsgHook);
    char* tempDllPath = reinterpret_cast<char*>(kAddrShellHotkeyTempDllPath);
    int* tempDllOwned = reinterpret_cast<int*>(kAddrShellHotkeyTempDllOwned);
    HMODULE* hookModule = reinterpret_cast<HMODULE*>(kAddrShellHotkeyHookModule);
    intptr_t* customProc = reinterpret_cast<intptr_t*>(kAddrGameWndprocCustomProc);
    intptr_t* msgCallback = reinterpret_cast<intptr_t*>(kAddrGameWndprocMsgCallback);

    bool changed = false;

    if (*suppressFlag != 0) {
        *suppressFlag = 0;
        changed = true;
    }

    if (*hookHandle) {
        UnhookWindowsHookEx(*hookHandle);
        *hookHandle = nullptr;
        changed = true;
    }

    if (*hookModule) {
        FreeLibrary(*hookModule);
        *hookModule = nullptr;
        changed = true;
    }

    if (*tempDllOwned) {
        if (tempDllPath[0]) {
            DeleteFileA(tempDllPath);
            tempDllPath[0] = '\0';
        }
        *tempDllOwned = 0;
        changed = true;
    }

    if (*customProc) {
        *customProc = 0;
        changed = true;
    }

    if (*msgCallback) {
        *msgCallback = 0;
        changed = true;
    }

    if (changed) {
        WsockLog("[EARLYPATCH] Cleared vanilla shell hotkey suppression state (%s)",
                 reason ? reason : "unknown");
    }

    LogShellHotkeySuppressionState(reason ? reason : "clear-state");

    return changed;
}

static HWND GetRootWindowSafe(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) {
        return nullptr;
    }
    return GetAncestor(hWnd, GA_ROOT);
}

static bool IsGameRelatedWindow(HWND hWnd) {
    if (!hWnd || !g_gameWindow || !IsWindow(hWnd) || !IsWindow(g_gameWindow)) {
        return false;
    }
    if (hWnd == g_gameWindow) {
        return true;
    }

    const HWND gameRoot = GetRootWindowSafe(g_gameWindow);
    const HWND targetRoot = GetRootWindowSafe(hWnd);
    return gameRoot && targetRoot && gameRoot == targetRoot;
}

static bool ShouldSuppressLateFocusSteal(HWND hWnd, const char* apiName) {
    if (!IsGameRelatedWindow(hWnd)) {
        return false;
    }

    const DWORD elapsedMs = GetTickCount() - g_processStartTick;
    const bool winDown =
        (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    const bool altDown =
        (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
    const bool shiftDown =
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    const bool shellChordActive = winDown || (altDown && shiftDown);
    if (shellChordActive) {
        g_shellChordLastSeenTick = GetTickCount();
    }
    const bool shellChordRecent =
        g_shellChordLastSeenTick != 0 &&
        (GetTickCount() - g_shellChordLastSeenTick) <= 1200;

    // If shell/layout chords are active, never let the game re-assert foreground.
    if (shellChordActive || shellChordRecent) {
        WsockLog("[EARLYPATCH] Suppressed %s focus steal during shell chord/recent window (win=%d alt=%d shift=%d recent=%d elapsed=%lu ms target=0x%p game=0x%p)",
                 apiName ? apiName : "FocusApi",
                 winDown ? 1 : 0,
                 altDown ? 1 : 0,
                 shiftDown ? 1 : 0,
                 shellChordRecent ? 1 : 0,
                 elapsedMs,
                 hWnd,
                 g_gameWindow);
        return true;
    }

    if (elapsedMs <= 3000) {
        return false;
    }

    if (GetForegroundWindow() == g_gameWindow) {
        return false;
    }

    WsockLog("[EARLYPATCH] Suppressed late %s focus steal for hwnd=0x%p (elapsed=%lu ms)",
             apiName, hWnd, elapsedMs);
    return true;
}

// ============================================================================
// Generic IAT patch helper
// ============================================================================

static int PatchImportByAddress(uintptr_t realFn, uintptr_t hookFn, const char* label) {
    HMODULE game = GetModuleHandleA(nullptr);
    if (!game) return 0;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(game);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(game) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress) return 0;

    auto base = reinterpret_cast<uint8_t*>(game);
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);

    int patchedCount = 0;
    for (; desc->Name; ++desc) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            if (static_cast<uintptr_t>(thunk->u1.Function) != realFn) {
                continue;
            }

            DWORD oldProt = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), PAGE_READWRITE, &oldProt)) {
                WsockLog("[EARLYPATCH] %s VirtualProtect failed (err %lu)", label, GetLastError());
                continue;
            }

            thunk->u1.Function = hookFn;
            VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), oldProt, &oldProt);
            ++patchedCount;
        }
    }

    if (patchedCount > 0) {
        WsockLog("[EARLYPATCH] %s patched %d IAT entr%s", label, patchedCount, patchedCount == 1 ? "y" : "ies");
    } else {
        WsockLog("[EARLYPATCH] %s not found in game IAT", label);
    }

    return patchedCount;
}

// ============================================================================
// FindWindowA IAT hook — always returns NULL so DXLib sees no duplicate
// ============================================================================

static HWND WINAPI Hooked_FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName) {
    WsockLog("[DUPBYPASS] FindWindowA intercepted: class='%s' -> returning NULL",
             lpClassName ? lpClassName : "(null)");
    return NULL;
}

static void PatchFindWindowA() {
    auto realFn = reinterpret_cast<uintptr_t>(::FindWindowA);
    PatchImportByAddress(realFn, reinterpret_cast<uintptr_t>(&Hooked_FindWindowA), "FindWindowA duplicate bypass");
}

// ============================================================================
// Early hotkey / IME patches
// ============================================================================

typedef BOOL (WINAPI *WINNLSEnableIME_t)(HWND hWnd, BOOL fEnable);
typedef BOOL (WINAPI *ImmSetOpenStatus_t)(HIMC hIMC, BOOL fOpen);
typedef BOOL (WINAPI *ImmNotifyIME_t)(HIMC hIMC, DWORD dwAction, DWORD dwIndex, DWORD dwValue);
typedef BOOL (WINAPI *ImmSetCompositionStringA_t)(HIMC hIMC,
                                                  DWORD dwIndex,
                                                  LPCVOID lpComp,
                                                  DWORD dwCompLen,
                                                  LPCVOID lpRead,
                                                  DWORD dwReadLen);

static HMODULE GetImm32Module() {
    HMODULE imm32 = GetModuleHandleA("imm32.dll");
    if (!imm32) {
        imm32 = LoadLibraryA("imm32.dll");
    }
    return imm32;
}

static bool IsProcNameEqualsA(LPCSTR procName, const char* expected) {
    return procName && !IS_INTRESOURCE(procName) && lstrcmpA(procName, expected) == 0;
}

static void WINAPI Hooked_keybd_event(BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR dwExtraInfo);

static FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    if (IsProcNameEqualsA(lpProcName, "SetMSGHookDll")) {
        ClearVanillaShellHotkeySuppressionState("SetMSGHookDll lookup");

        if (!g_loggedSetMsgHookBlock) {
            WsockLog("[EARLYPATCH] Blocked SetMSGHookDll export lookup before helper hook install");
            g_loggedSetMsgHookBlock = true;
        }
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }

    if (IsProcNameEqualsA(lpProcName, "SetWindowsHookExA")) {
        WsockLog("[EARLYPATCH] Redirected GetProcAddress(SetWindowsHookExA) -> guarded hook");
        return reinterpret_cast<FARPROC>(&Hooked_SetWindowsHookExA);
    }

    if (IsProcNameEqualsA(lpProcName, "SetWindowsHookExW")) {
        WsockLog("[EARLYPATCH] Redirected GetProcAddress(SetWindowsHookExW) -> guarded hook");
        return reinterpret_cast<FARPROC>(&Hooked_SetWindowsHookExW);
    }

    if (IsProcNameEqualsA(lpProcName, "keybd_event")) {
        WsockLog("[EARLYPATCH] Redirected GetProcAddress(keybd_event) -> phantom-VK7 filter");
        return reinterpret_cast<FARPROC>(&Hooked_keybd_event);
    }

    return ::GetProcAddress(hModule, lpProcName);
}

static BOOL WINAPI Hooked_SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) {
    LogStartupProgressState("SystemParametersInfoA-enter", false);
    if (uiAction == 0x61u) {
        ClearVanillaShellHotkeySuppressionState("SystemParametersInfoA(0x61)");

        if (!g_loggedSpi61Block) {
            WsockLog("[EARLYPATCH] Blocked legacy SystemParametersInfoA(0x61) shell hotkey suppression");
            g_loggedSpi61Block = true;
        }
        return TRUE;
    }

    if (uiAction == 0x2000u || uiAction == 0x2001u || uiAction == 0x11u) {
        const BOOL result = ::SystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni);
        LogSystemParametersCall("Pass-through", uiAction, uiParam, pvParam, fWinIni, result);
        return result;
    }

    return ::SystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni);
}

static BOOL WINAPI Hooked_WINNLSEnableIME(HWND hWnd, BOOL fEnable) {
    WsockLog("[EARLYPATCH] WINNLSEnableIME(hwnd=0x%p, enable=%d)", hWnd, fEnable ? 1 : 0);
    LogStartupProgressState("WINNLSEnableIME-enter", false);

    if (hWnd && IsWindow(hWnd)) {
        const HWND learnedRoot = GetRootWindowSafe(hWnd);
        const HWND learned = learnedRoot ? learnedRoot : hWnd;
        if (!g_gameWindow || g_gameWindow != learned) {
            g_gameWindow = learned;
            WsockLog("[EARLYPATCH] Learned game window hwnd=0x%p (raw=0x%p) from WINNLSEnableIME", learned, hWnd);
        }
    }

    if (!fEnable) {
        ClearVanillaShellHotkeySuppressionState("WINNLSEnableIME(FALSE)");

        if (!g_loggedImeDisableBlock) {
            WsockLog("[EARLYPATCH] Prevented vanilla WINNLSEnableIME(FALSE) for hwnd=0x%p", hWnd);
            g_loggedImeDisableBlock = true;
        }
        return TRUE;
    }

    HMODULE user32 = GetModuleHandleA("user32.dll");
    auto realFn = reinterpret_cast<WINNLSEnableIME_t>(
        user32 ? ::GetProcAddress(user32, "WINNLSEnableIME") : nullptr);
    if (!realFn) {
        return TRUE;
    }

    return realFn(hWnd, fEnable);
}

static BOOL WINAPI Hooked_ImmSetOpenStatus(HIMC hIMC, BOOL fOpen) {
    if (g_loggedImmSetOpenStatusCalls < 32) {
        ++g_loggedImmSetOpenStatusCalls;
        WsockLog("[EARLYPATCH] ImmSetOpenStatus(himc=0x%p, open=%d) game=0x%p fg=0x%p active=0x%p focus=0x%p elapsed=%lu ms",
                 hIMC,
                 fOpen ? 1 : 0,
                 g_gameWindow,
                 GetForegroundWindow(),
                 GetActiveWindow(),
                 GetFocus(),
                 GetTickCount() - g_processStartTick);
    }

    auto realFn = reinterpret_cast<ImmSetOpenStatus_t>(
        GetImm32Module() ? ::GetProcAddress(GetImm32Module(), "ImmSetOpenStatus") : nullptr);
    return realFn ? realFn(hIMC, fOpen) : FALSE;
}

static BOOL WINAPI Hooked_ImmNotifyIME(HIMC hIMC, DWORD dwAction, DWORD dwIndex, DWORD dwValue) {
    if (g_loggedImmNotifyCalls < 32) {
        ++g_loggedImmNotifyCalls;
        WsockLog("[EARLYPATCH] ImmNotifyIME(himc=0x%p, action=0x%08lX, index=0x%08lX, value=0x%08lX) game=0x%p fg=0x%p active=0x%p focus=0x%p elapsed=%lu ms",
                 hIMC,
                 dwAction,
                 dwIndex,
                 dwValue,
                 g_gameWindow,
                 GetForegroundWindow(),
                 GetActiveWindow(),
                 GetFocus(),
                 GetTickCount() - g_processStartTick);
    }

    auto realFn = reinterpret_cast<ImmNotifyIME_t>(
        GetImm32Module() ? ::GetProcAddress(GetImm32Module(), "ImmNotifyIME") : nullptr);
    return realFn ? realFn(hIMC, dwAction, dwIndex, dwValue) : FALSE;
}

static BOOL WINAPI Hooked_ImmSetCompositionStringA(HIMC hIMC,
                                                   DWORD dwIndex,
                                                   LPCVOID lpComp,
                                                   DWORD dwCompLen,
                                                   LPCVOID lpRead,
                                                   DWORD dwReadLen) {
    if (g_loggedImmSetCompositionCalls < 32) {
        ++g_loggedImmSetCompositionCalls;
        WsockLog("[EARLYPATCH] ImmSetCompositionStringA(himc=0x%p, index=0x%08lX, compLen=%lu, readLen=%lu) game=0x%p fg=0x%p active=0x%p focus=0x%p elapsed=%lu ms",
                 hIMC,
                 dwIndex,
                 dwCompLen,
                 dwReadLen,
                 g_gameWindow,
                 GetForegroundWindow(),
                 GetActiveWindow(),
                 GetFocus(),
                 GetTickCount() - g_processStartTick);
    }

    auto realFn = reinterpret_cast<ImmSetCompositionStringA_t>(
        GetImm32Module() ? ::GetProcAddress(GetImm32Module(), "ImmSetCompositionStringA") : nullptr);
    return realFn ? realFn(hIMC, dwIndex, lpComp, dwCompLen, lpRead, dwReadLen) : FALSE;
}

// Game_MainLoop (sub_5D2AC0 @ 0x005D2AC0) injects keybd_event(7, 0, KEYEVENTF_KEYUP, 0)
// on EVERY frame. VK 0x07 is a reserved/undefined virtual key; injecting a phantom key-up
// continuously is the classic hook-free "disable the Windows key" trick: the shell only opens
// the Start menu on a clean Win-down -> Win-up with no intervening key event, and the
// Alt+Shift layout-switch hotkey likewise needs a clean chord. The constant phantom injection
// guarantees an intervening event, so Start never opens and Alt+Shift never switches layout.
// Alt+Space still works because it is handled directly by DefWindowProc on the real keydown.
// Dropping exactly the VK 0x07 injection restores both shell hotkeys with no other side effects.
static DWORD g_phantomKeyDropCount = 0;

static void WINAPI Hooked_keybd_event(BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR dwExtraInfo) {
    if (bVk == 0x07) {
        const DWORD n = ++g_phantomKeyDropCount;
        if (n <= 5 || (n % 1000) == 0) {
            WsockLog("[EARLYPATCH] Dropped phantom keybd_event(vk=0x07 flags=0x%X) #%lu "
                     "(Win/Alt+Shift shell-hotkey fix)", dwFlags, n);
        }
        return;
    }
    ::keybd_event(bVk, bScan, dwFlags, dwExtraInfo);
}

static BOOL WINAPI Hooked_SetForegroundWindow(HWND hWnd) {
    LogFocusCallState("Before", "SetForegroundWindow", hWnd, 0);

    if (ShouldSuppressLateFocusSteal(hWnd, "SetForegroundWindow")) {
        LogFocusCallState("Suppressed", "SetForegroundWindow", hWnd, TRUE);
        return TRUE;
    }

    const BOOL result = ::SetForegroundWindow(hWnd);
    LogFocusCallState("After", "SetForegroundWindow", hWnd, result);
    return result;
}

static BOOL WINAPI Hooked_BringWindowToTop(HWND hWnd) {
    LogFocusCallState("Before", "BringWindowToTop", hWnd, 0);

    if (ShouldSuppressLateFocusSteal(hWnd, "BringWindowToTop")) {
        LogFocusCallState("Suppressed", "BringWindowToTop", hWnd, TRUE);
        return TRUE;
    }

    const BOOL result = ::BringWindowToTop(hWnd);
    LogFocusCallState("After", "BringWindowToTop", hWnd, result);
    return result;
}

static HWND WINAPI Hooked_SetActiveWindow(HWND hWnd) {
    LogFocusCallState("Before", "SetActiveWindow", hWnd, 0);

    if (ShouldSuppressLateFocusSteal(hWnd, "SetActiveWindow")) {
        HWND result = GetActiveWindow();
        LogFocusCallState("Suppressed", "SetActiveWindow", hWnd, (LONG_PTR)result);
        return result;
    }

    HWND result = ::SetActiveWindow(hWnd);
    LogFocusCallState("After", "SetActiveWindow", hWnd, (LONG_PTR)result);
    return result;
}

static bool ShouldBlockKeyboardHookInstall(int idHook) {
    return idHook == WH_KEYBOARD || idHook == WH_KEYBOARD_LL;
}

static HHOOK WINAPI Hooked_SetWindowsHookExA(int idHook,
                                             HOOKPROC lpfn,
                                             HINSTANCE hmod,
                                             DWORD dwThreadId) {
    if (ShouldBlockKeyboardHookInstall(idHook)) {
        ClearVanillaShellHotkeySuppressionState("SetWindowsHookExA keyboard hook");
        if (g_loggedSetWindowsHookExBlocks < 32) {
            ++g_loggedSetWindowsHookExBlocks;
            WsockLog("[EARLYPATCH] Blocked SetWindowsHookExA idHook=%d proc=0x%p hmod=0x%p thread=%lu",
                     idHook,
                     lpfn,
                     hmod,
                     dwThreadId);
        }
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    return ::SetWindowsHookExA(idHook, lpfn, hmod, dwThreadId);
}

static HHOOK WINAPI Hooked_SetWindowsHookExW(int idHook,
                                             HOOKPROC lpfn,
                                             HINSTANCE hmod,
                                             DWORD dwThreadId) {
    if (ShouldBlockKeyboardHookInstall(idHook)) {
        ClearVanillaShellHotkeySuppressionState("SetWindowsHookExW keyboard hook");
        if (g_loggedSetWindowsHookExBlocks < 32) {
            ++g_loggedSetWindowsHookExBlocks;
            WsockLog("[EARLYPATCH] Blocked SetWindowsHookExW idHook=%d proc=0x%p hmod=0x%p thread=%lu",
                     idHook,
                     lpfn,
                     hmod,
                     dwThreadId);
        }
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    return ::SetWindowsHookExW(idHook, lpfn, hmod, dwThreadId);
}

// Layer B (mod/docs/SHELL_HOTKEY_POLICY.md): NOP swallow branches in sub_633490 once bytes
// are verified on the shipping .exe (SC_KEYMENU return 0, middle-mouse LABEL_129).
static bool ReadBytesAtRva(uintptr_t rva, uint8_t* out, size_t size) {
    if (!out || size == 0) {
        return false;
    }

    HMODULE game = GetModuleHandleA(nullptr);
    if (!game) {
        return false;
    }

    const uint8_t* src = reinterpret_cast<const uint8_t*>(game) + rva;
    __try {
        memcpy(out, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LogBytesAtRva(const char* tag, uintptr_t rva, size_t size) {
    uint8_t bytes[64] = {};
    if (size > sizeof(bytes)) {
        size = sizeof(bytes);
    }
    if (!ReadBytesAtRva(rva, bytes, size)) {
        WsockLog("[EARLYPATCH] %s: failed to read RVA 0x%lX", tag ? tag : "bytes", (unsigned long)rva);
        return;
    }

    char line[3 * 64 + 1] = {};
    size_t pos = 0;
    for (size_t i = 0; i < size && pos + 3 < sizeof(line); ++i) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X%s", bytes[i], (i + 1 < size) ? " " : "");
    }
    WsockLog("[EARLYPATCH] %s RVA 0x%lX: %s", tag ? tag : "bytes", (unsigned long)rva, line);
}

static bool PatchRvaIfMatches(uintptr_t rva,
                              const uint8_t* expected,
                              size_t expectedSize,
                              size_t patchOffset,
                              const uint8_t* patch,
                              size_t patchSize,
                              const char* label) {
    if (!expected || !patch || expectedSize == 0 || patchSize == 0 || patchOffset + patchSize > expectedSize) {
        return false;
    }

    HMODULE game = GetModuleHandleA(nullptr);
    if (!game) {
        WsockLog("[EARLYPATCH] %s: GetModuleHandleA(nullptr) failed", label ? label : "patch");
        return false;
    }

    uint8_t* site = reinterpret_cast<uint8_t*>(game) + rva;
    uint8_t actual[96] = {};
    if (expectedSize > sizeof(actual) || !ReadBytesAtRva(rva, actual, expectedSize)) {
        WsockLog("[EARLYPATCH] %s: failed to read expected bytes at RVA 0x%lX", label ? label : "patch", (unsigned long)rva);
        return false;
    }

    if (memcmp(actual, expected, expectedSize) != 0) {
        WsockLog("[EARLYPATCH] %s: signature mismatch at RVA 0x%lX (patch skipped)", label ? label : "patch", (unsigned long)rva);
        LogBytesAtRva("actual", rva, expectedSize);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(site + patchOffset, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        WsockLog("[EARLYPATCH] %s: VirtualProtect failed at RVA 0x%lX (err %lu)",
                 label ? label : "patch",
                 (unsigned long)(rva + patchOffset),
                 GetLastError());
        return false;
    }

    memcpy(site + patchOffset, patch, patchSize);
    VirtualProtect(site + patchOffset, patchSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site + patchOffset, patchSize);

    WsockLog("[EARLYPATCH] %s: patched RVA 0x%lX (+0x%lX)", label ? label : "patch", (unsigned long)rva, (unsigned long)patchOffset);
    LogBytesAtRva("post-patch", rva, expectedSize);
    return true;
}

static bool LocatePatternNearRva(uintptr_t preferredRva,
                                 intptr_t searchRadius,
                                 const uint8_t* expected,
                                 size_t expectedSize,
                                 uintptr_t* outLocatedRva) {
    if (!expected || expectedSize == 0 || !outLocatedRva) {
        return false;
    }

    HMODULE game = GetModuleHandleA(nullptr);
    if (!game) {
        return false;
    }

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(game) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize < expectedSize) {
        return false;
    }

    const intptr_t minRvaSigned = (preferredRva > static_cast<uintptr_t>(searchRadius))
                                      ? static_cast<intptr_t>(preferredRva - searchRadius)
                                      : 0;
    const intptr_t maxRvaSigned = static_cast<intptr_t>(preferredRva + searchRadius);
    size_t startRva = static_cast<size_t>(minRvaSigned < 0 ? 0 : minRvaSigned);
    size_t endRva = static_cast<size_t>(maxRvaSigned < 0 ? 0 : maxRvaSigned);
    if (endRva + expectedSize > imageSize) {
        endRva = imageSize - expectedSize;
    }
    if (startRva > endRva) {
        return false;
    }

    for (size_t rva = startRva; rva <= endRva; ++rva) {
        if (memcmp(reinterpret_cast<const uint8_t*>(game) + rva, expected, expectedSize) == 0) {
            *outLocatedRva = static_cast<uintptr_t>(rva);
            return true;
        }
    }

    return false;
}

static bool LocatePatternNearRvaMasked(uintptr_t preferredRva,
                                       intptr_t searchRadius,
                                       const uint8_t* pattern,
                                       const char* mask,
                                       size_t patternSize,
                                       uintptr_t* outLocatedRva) {
    if (!pattern || !mask || patternSize == 0 || !outLocatedRva) {
        return false;
    }

    HMODULE game = GetModuleHandleA(nullptr);
    if (!game) {
        return false;
    }

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(game) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize < patternSize) {
        return false;
    }

    const intptr_t minRvaSigned = (preferredRva > static_cast<uintptr_t>(searchRadius))
                                      ? static_cast<intptr_t>(preferredRva - searchRadius)
                                      : 0;
    const intptr_t maxRvaSigned = static_cast<intptr_t>(preferredRva + searchRadius);
    size_t startRva = static_cast<size_t>(minRvaSigned < 0 ? 0 : minRvaSigned);
    size_t endRva = static_cast<size_t>(maxRvaSigned < 0 ? 0 : maxRvaSigned);
    if (endRva + patternSize > imageSize) {
        endRva = imageSize - patternSize;
    }
    if (startRva > endRva) {
        return false;
    }

    for (size_t rva = startRva; rva <= endRva; ++rva) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(game) + rva;
        bool match = true;
        for (size_t i = 0; i < patternSize; ++i) {
            if (mask[i] == 'x' && ptr[i] != pattern[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            *outLocatedRva = static_cast<uintptr_t>(rva);
            return true;
        }
    }

    return false;
}

static bool PatchBySignatureNearRva(uintptr_t preferredRva,
                                    intptr_t searchRadius,
                                    const uint8_t* expected,
                                    size_t expectedSize,
                                    size_t patchOffset,
                                    const uint8_t* patch,
                                    size_t patchSize,
                                    const char* label) {
    uintptr_t locatedRva = 0;
    if (!LocatePatternNearRva(preferredRva, searchRadius, expected, expectedSize, &locatedRva)) {
        WsockLog("[EARLYPATCH] %s: signature not found near RVA 0x%lX (+/-%ld)",
                 label ? label : "patch",
                 (unsigned long)preferredRva,
                 (long)searchRadius);
        LogBytesAtRva("actual-near", preferredRva, expectedSize);
        return false;
    }

    if (locatedRva != preferredRva) {
        WsockLog("[EARLYPATCH] %s: relocated signature RVA 0x%lX -> 0x%lX",
                 label ? label : "patch",
                 (unsigned long)preferredRva,
                 (unsigned long)locatedRva);
    }

    return PatchRvaIfMatches(locatedRva,
                             expected,
                             expectedSize,
                             patchOffset,
                             patch,
                             patchSize,
                             label);
}

static bool PatchByMaskedSignatureNearRva(uintptr_t preferredRva,
                                          intptr_t searchRadius,
                                          const uint8_t* pattern,
                                          const char* mask,
                                          size_t patternSize,
                                          size_t patchOffset,
                                          const uint8_t* patch,
                                          size_t patchSize,
                                          const char* label) {
    uintptr_t locatedRva = 0;
    if (!LocatePatternNearRvaMasked(preferredRva, searchRadius, pattern, mask, patternSize, &locatedRva)) {
        WsockLog("[EARLYPATCH] %s: masked signature not found near RVA 0x%lX (+/-%ld)",
                 label ? label : "patch",
                 (unsigned long)preferredRva,
                 (long)searchRadius);
        return false;
    }

    if (locatedRva != preferredRva) {
        WsockLog("[EARLYPATCH] %s: relocated masked signature RVA 0x%lX -> 0x%lX",
                 label ? label : "patch",
                 (unsigned long)preferredRva,
                 (unsigned long)locatedRva);
    }

    HMODULE game = GetModuleHandleA(nullptr);
    if (!game) {
        return false;
    }

    uint8_t* site = reinterpret_cast<uint8_t*>(game) + locatedRva + patchOffset;
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        WsockLog("[EARLYPATCH] %s: VirtualProtect failed at RVA 0x%lX (err %lu)",
                 label ? label : "patch",
                 (unsigned long)(locatedRva + patchOffset),
                 GetLastError());
        return false;
    }

    memcpy(site, patch, patchSize);
    VirtualProtect(site, patchSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, patchSize);
    WsockLog("[EARLYPATCH] %s: patched RVA 0x%lX (+0x%lX)",
             label ? label : "patch",
             (unsigned long)locatedRva,
             (unsigned long)patchOffset);
    return true;
}

static void PatchVanillaWndprocSwallowBranches() {
    // Fixed-RVA patch plan verified against D:\Alice in wonderland 2\as2.exe.
    // We intentionally avoid runtime pattern relocation/scanning for these sites.
    static const uintptr_t kRvaScKeymenuBlock = 0x233DFE;
    static const uintptr_t kRvaCursorHideBlock = 0x234107;
    static const uintptr_t kRvaWndprocDefWindowGate = 0x233F25;
    static const uintptr_t kRvaDInputKbCoopArg1 = 0x22F1A6;
    static const uintptr_t kRvaDInputKbCoopArg2 = 0x22F2F1;
    static const uintptr_t kRvaWndprocCustomCallGuard = 0x2334B5;
    static const uintptr_t kRvaShellHelperGuard = 0x233979;
    static const uintptr_t kRvaScTasklistBlock = 0x233E21;
    static const uintptr_t kRvaScScreensaveBlock = 0x233E38;

    static const uint8_t kExpectedScKeymenu[] = {
        0x81, 0xFB, 0x00, 0xF1, 0x00, 0x00, 0x75, 0x1B,
        0x39, 0x15, 0x74, 0x5B, 0x9E, 0x00, 0x0F, 0x85,
        0x13, 0x01, 0x00, 0x00, 0x5F, 0x5E, 0x5D, 0x33,
        0xC0, 0x5B, 0x81, 0xC4, 0xB8, 0x00, 0x00, 0x00,
        0xC2, 0x10, 0x00
    };
    // Convert "JNE +0x113" (0F 85 13 01 00 00) to "JMP +0x114; NOP".
    // E9 uses a 5-byte instruction (vs 6-byte 0F 85), so rel32 must be +1.
    static const uint8_t kPatchScKeymenu[] = {
        0xE9, 0x14, 0x01, 0x00, 0x00, 0x90
    };
    // WM_SYSCOMMAND SC_TASKLIST (0xF170): vanilla currently returns 1 (swallow).
    // Convert JNE to unconditional JMP so even equal case skips the "return 1" block.
    static const uint8_t kExpectedScTasklist[] = {
        0x81, 0xFB, 0x70, 0xF1, 0x00, 0x00, 0x75, 0x0F,
        0x5F, 0x5E, 0x5D, 0x8B, 0xC2, 0x5B, 0x81, 0xC4,
        0xB8, 0x00, 0x00, 0x00, 0xC2, 0x10, 0x00
    };
    static const uint8_t kPatchScTasklistJmp[] = {
        0xEB
    };
    // WM_SYSCOMMAND SC_SCREENSAVE (0xF140): same swallow behavior via "return 1".
    // Convert JNE rel32 to unconditional JMP rel32 (+1) and pad with NOP.
    static const uint8_t kExpectedScScreensave[] = {
        0x81, 0xFB, 0x40, 0xF1, 0x00, 0x00, 0x0F, 0x85,
        0xE1, 0x00, 0x00, 0x00, 0x5F, 0x5E, 0x5D, 0x8B,
        0xC2, 0x5B, 0x81, 0xC4, 0xB8, 0x00, 0x00, 0x00,
        0xC2, 0x10, 0x00
    };
    static const uint8_t kPatchScScreensaveJmp[] = {
        0xE9, 0xE2, 0x00, 0x00, 0x00, 0x90
    };

    static const uint8_t kExpectedCursorHide[] = {
        0x39, 0x3D, 0xD4, 0xB6, 0x9D, 0x00, 0x0F, 0x85,
        0x12, 0xFE, 0xFF, 0xFF, 0x57, 0xFF, 0x15, 0x9C,
        0x22, 0x72, 0x00, 0x8B, 0x35, 0xE0, 0x22, 0x72,
        0x00, 0x57, 0xFF, 0xD6, 0x83, 0xF8, 0xFF
    };
    // Convert "JNE -0x1EE" (0F 85 12 FE FF FF) to unconditional "JMP -0x1ED; NOP".
    // E9 uses a 5-byte instruction (vs 6-byte 0F 85), so rel32 must be +1.
    static const uint8_t kPatchCursorHideBypass[] = {
        0xE9, 0x13, 0xFE, 0xFF, 0xFF, 0x90
    };
    // DefWindowProc gate tail in sub_633490:
    // if (dword_9DB660 == 1) return v45; else DefWindowProcA(...)
    // Convert JNE to unconditional JMP so we always take DefWindowProc path.
    static const uint8_t kExpectedDefWindowGate[] = {
        0x83, 0x3D, 0x60, 0xB6, 0x9D, 0x00, 0x01, 0x0F,
        0x85, 0xEC, 0x02, 0x00, 0x00, 0x8B, 0x44, 0x24,
        0x30, 0x5F, 0x5E, 0x5D, 0x5B, 0x81, 0xC4, 0xB8,
        0x00, 0x00, 0x00, 0xC2, 0x10, 0x00
    };
    // 0F 85 EC 02 00 00 -> E9 ED 02 00 00 90
    static const uint8_t kPatchDefWindowGate[] = {
        0xE9, 0xED, 0x02, 0x00, 0x00, 0x90
    };
    // sub_62EE80: SetCooperativeLevel(..., 0x0A) -> force 0x06.
    static const uint8_t kExpectedDInputCoopCall1[] = {
        0x6A, 0x0A, 0x8B, 0x1A, 0xE8, 0x41, 0x58, 0x00, 0x00
    };
    static const uint8_t kExpectedDInputCoopCall2[] = {
        0x6A, 0x0A, 0x8B, 0x38, 0xE8, 0xF6, 0x56, 0x00, 0x00
    };
    static const uint8_t kPatchDInputCoopArg[] = {
        0x06
    };
    // sub_633490 early custom-proc call guard:
    // cmp gate,1; jne skip; mov eax,[9DB668]; cmp eax,0; je skip; ... call eax
    // Change JE to JMP so this block is always skipped.
    static const uint8_t kExpectedCustomProcGuard[] = {
        0x3B, 0xCA, 0x75, 0x2E, 0xA1, 0x68, 0xB6, 0x9D, 0x00, 0x3B, 0xC7, 0x74, 0x25
    };
    static const uint8_t kPatchCustomProcGuardBypass[] = {
        0xEB
    };
    // if (dword_9E5B74 != 1) goto LABEL_193;
    // Convert JNE to JMP to always skip shell-helper arming block.
    static const uint8_t kExpectedShellHelperGuard[] = {
        0x74, 0x5B, 0x9E, 0x00, 0xBE, 0x01, 0x00, 0x00, 0x00, 0x3B,
        0xC6, 0x0F, 0x85, 0x94, 0x05, 0x00, 0x00, 0x81, 0x3D, 0x9C,
        0x5C, 0x9E, 0x00, 0x04, 0x01, 0x00, 0x00, 0x7D
    };
    static const uint8_t kPatchShellHelperGuardJmp[] = {
        0xE9, 0x95, 0x05, 0x00, 0x00, 0x90
    };

    PatchRvaIfMatches(kRvaScKeymenuBlock,
                      kExpectedScKeymenu,
                      sizeof(kExpectedScKeymenu),
                      14,
                      kPatchScKeymenu,
                      sizeof(kPatchScKeymenu),
                      "sub_633490 SC_KEYMENU swallow bypass");

    PatchRvaIfMatches(kRvaScTasklistBlock,
                      kExpectedScTasklist,
                      sizeof(kExpectedScTasklist),
                      6,
                      kPatchScTasklistJmp,
                      sizeof(kPatchScTasklistJmp),
                      "sub_633490 SC_TASKLIST swallow bypass");

    PatchRvaIfMatches(kRvaScScreensaveBlock,
                      kExpectedScScreensave,
                      sizeof(kExpectedScScreensave),
                      6,
                      kPatchScScreensaveJmp,
                      sizeof(kPatchScScreensaveJmp),
                      "sub_633490 SC_SCREENSAVE swallow bypass");

    PatchRvaIfMatches(kRvaCursorHideBlock,
                      kExpectedCursorHide,
                      sizeof(kExpectedCursorHide),
                      6,
                      kPatchCursorHideBypass,
                      sizeof(kPatchCursorHideBypass),
                      "sub_633490 cursor-hide loop bypass (hover/middle fix)");

    PatchRvaIfMatches(kRvaWndprocDefWindowGate,
                      kExpectedDefWindowGate,
                      sizeof(kExpectedDefWindowGate),
                      7,
                      kPatchDefWindowGate,
                      sizeof(kPatchDefWindowGate),
                      "sub_633490 DefWindowProc gate bypass (ignore 0x9DB660)");

    PatchRvaIfMatches(kRvaDInputKbCoopArg1,
                      kExpectedDInputCoopCall1,
                      sizeof(kExpectedDInputCoopCall1),
                      1,
                      kPatchDInputCoopArg,
                      sizeof(kPatchDInputCoopArg),
                      "sub_62EE80 keyboard coop arg #1 (0x0A->0x06)");

    PatchRvaIfMatches(kRvaDInputKbCoopArg2,
                      kExpectedDInputCoopCall2,
                      sizeof(kExpectedDInputCoopCall2),
                      1,
                      kPatchDInputCoopArg,
                      sizeof(kPatchDInputCoopArg),
                      "sub_62EE80 keyboard coop arg #2 (0x0A->0x06)");

    PatchRvaIfMatches(kRvaWndprocCustomCallGuard,
                      kExpectedCustomProcGuard,
                      sizeof(kExpectedCustomProcGuard),
                      11,
                      kPatchCustomProcGuardBypass,
                      sizeof(kPatchCustomProcGuardBypass),
                      "sub_633490 custom-proc guard bypass (ignore 0x9DB668)");

    PatchRvaIfMatches(kRvaShellHelperGuard,
                      kExpectedShellHelperGuard,
                      sizeof(kExpectedShellHelperGuard),
                      11,
                      kPatchShellHelperGuardJmp,
                      sizeof(kPatchShellHelperGuardJmp),
                      "sub_633490 shell-helper arming guard bypass");
}

static void PatchEarlyHotkeyImports() {
    LogStartupProgressState("PatchEarlyHotkeyImports-begin", true);
    LogShellHotkeySuppressionState("process attach (before clear)");
    ClearVanillaShellHotkeySuppressionState("process attach");
    PatchVanillaWndprocSwallowBranches();

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::GetProcAddress),
        reinterpret_cast<uintptr_t>(&Hooked_GetProcAddress),
        "GetProcAddress hotkey helper bypass");

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::SystemParametersInfoA),
        reinterpret_cast<uintptr_t>(&Hooked_SystemParametersInfoA),
        "SystemParametersInfoA hotkey bypass");

    HMODULE user32 = GetModuleHandleA("user32.dll");
    auto winNlsEnableIme = reinterpret_cast<uintptr_t>(
        user32 ? ::GetProcAddress(user32, "WINNLSEnableIME") : nullptr);
    if (!winNlsEnableIme) {
        WsockLog("[EARLYPATCH] WINNLSEnableIME export not found in user32.dll");
    } else {
        PatchImportByAddress(
            winNlsEnableIme,
            reinterpret_cast<uintptr_t>(&Hooked_WINNLSEnableIME),
            "WINNLSEnableIME hotkey bypass");
    }

    HMODULE imm32 = GetImm32Module();
    auto immSetOpenStatus = reinterpret_cast<uintptr_t>(
        imm32 ? ::GetProcAddress(imm32, "ImmSetOpenStatus") : nullptr);
    if (immSetOpenStatus) {
        PatchImportByAddress(
            immSetOpenStatus,
            reinterpret_cast<uintptr_t>(&Hooked_ImmSetOpenStatus),
            "ImmSetOpenStatus IME trace");
    }

    auto immNotifyIme = reinterpret_cast<uintptr_t>(
        imm32 ? ::GetProcAddress(imm32, "ImmNotifyIME") : nullptr);
    if (immNotifyIme) {
        PatchImportByAddress(
            immNotifyIme,
            reinterpret_cast<uintptr_t>(&Hooked_ImmNotifyIME),
            "ImmNotifyIME IME trace");
    }

    auto immSetCompositionStringA = reinterpret_cast<uintptr_t>(
        imm32 ? ::GetProcAddress(imm32, "ImmSetCompositionStringA") : nullptr);
    if (immSetCompositionStringA) {
        PatchImportByAddress(
            immSetCompositionStringA,
            reinterpret_cast<uintptr_t>(&Hooked_ImmSetCompositionStringA),
            "ImmSetCompositionStringA IME trace");
    }

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::keybd_event),
        reinterpret_cast<uintptr_t>(&Hooked_keybd_event),
        "keybd_event phantom-VK7 Win/Alt+Shift bypass");

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::SetForegroundWindow),
        reinterpret_cast<uintptr_t>(&Hooked_SetForegroundWindow),
        "SetForegroundWindow focus-steal bypass");

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::BringWindowToTop),
        reinterpret_cast<uintptr_t>(&Hooked_BringWindowToTop),
        "BringWindowToTop focus-steal bypass");

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::SetActiveWindow),
        reinterpret_cast<uintptr_t>(&Hooked_SetActiveWindow),
        "SetActiveWindow focus-steal bypass");

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::SetWindowsHookExA),
        reinterpret_cast<uintptr_t>(&Hooked_SetWindowsHookExA),
        "SetWindowsHookExA keyboard-hook bypass");

    PatchImportByAddress(
        reinterpret_cast<uintptr_t>(::SetWindowsHookExW),
        reinterpret_cast<uintptr_t>(&Hooked_SetWindowsHookExW),
        "SetWindowsHookExW keyboard-hook bypass");

    LogShellHotkeySuppressionState("after PatchEarlyHotkeyImports");
    LogStartupProgressState("PatchEarlyHotkeyImports-end", true);
}

// ============================================================================
// Set DXLib duplicate flag directly in memory
// ============================================================================

static void SetDXLibDuplicateFlag() {
    // 0x9DB884 = dword_9DB884 in sub_630C80.  When non-zero the duplicate
    // check branch is skipped even if FindWindowA returns a handle.
    volatile uint32_t* flag = reinterpret_cast<volatile uint32_t*>(0x9DB884);
    *flag = 1;
    WsockLog("[DUPBYPASS] Set dword_9DB884 = 1");
}

// ============================================================================
// Forward real wsock32 exports
// ============================================================================

static HMODULE g_hRealWsock32 = nullptr;

#define DECL_FWD(name) static decltype(&::name) g_real_##name = nullptr;
DECL_FWD(socket)
DECL_FWD(htons)
DECL_FWD(WSAStartup)
DECL_FWD(bind)
DECL_FWD(ioctlsocket)
DECL_FWD(WSAGetLastError)
DECL_FWD(recvfrom)
DECL_FWD(sendto)
DECL_FWD(WSACleanup)
DECL_FWD(inet_addr)
DECL_FWD(closesocket)
DECL_FWD(htonl)
DECL_FWD(WSAAsyncSelect)
DECL_FWD(accept)
DECL_FWD(recv)
DECL_FWD(send)
#undef DECL_FWD

static bool LoadRealWsock32() {
    // Load the real wsock32.dll from System32
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\wsock32.dll", sysDir);

    g_hRealWsock32 = LoadLibraryA(path);
    if (!g_hRealWsock32) {
        WsockLog("ERROR: Cannot load real wsock32.dll from %s (err %lu)",
                 path, GetLastError());
        return false;
    }
    WsockLog("Real wsock32.dll loaded from %s", path);

#define RESOLVE(name) \
    g_real_##name = reinterpret_cast<decltype(g_real_##name)>( \
        GetProcAddress(g_hRealWsock32, #name)); \
    if (!g_real_##name) WsockLog("WARN: " #name " not found in real wsock32");

    RESOLVE(socket)
    RESOLVE(htons)
    RESOLVE(WSAStartup)
    RESOLVE(bind)
    RESOLVE(ioctlsocket)
    RESOLVE(WSAGetLastError)
    RESOLVE(recvfrom)
    RESOLVE(sendto)
    RESOLVE(WSACleanup)
    RESOLVE(inet_addr)
    RESOLVE(closesocket)
    RESOLVE(htonl)
    RESOLVE(WSAAsyncSelect)
    RESOLVE(accept)
    RESOLVE(recv)
    RESOLVE(send)
#undef RESOLVE

    return true;
}

// ============================================================================
// Exported winsock functions (forwarded to real wsock32.dll)
// ============================================================================

extern "C" {

__declspec(dllexport) SOCKET PASCAL proxy_socket(int af, int type, int protocol) {
    return g_real_socket ? g_real_socket(af, type, protocol) : INVALID_SOCKET;
}

__declspec(dllexport) u_short PASCAL proxy_htons(u_short hostshort) {
    return g_real_htons ? g_real_htons(hostshort) : 0;
}

__declspec(dllexport) int PASCAL proxy_WSAStartup(WORD wVersionRequired, LPWSADATA lpWSAData) {
    return g_real_WSAStartup ? g_real_WSAStartup(wVersionRequired, lpWSAData) : WSASYSNOTREADY;
}

__declspec(dllexport) int PASCAL proxy_bind(SOCKET s, const struct sockaddr* addr, int namelen) {
    return g_real_bind ? g_real_bind(s, addr, namelen) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_ioctlsocket(SOCKET s, long cmd, u_long* argp) {
    return g_real_ioctlsocket ? g_real_ioctlsocket(s, cmd, argp) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_WSAGetLastError(void) {
    return g_real_WSAGetLastError ? g_real_WSAGetLastError() : 0;
}

__declspec(dllexport) int PASCAL proxy_recvfrom(SOCKET s, char* buf, int len, int flags,
                                                 struct sockaddr* from, int* fromlen) {
    return g_real_recvfrom ? g_real_recvfrom(s, buf, len, flags, from, fromlen) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_sendto(SOCKET s, const char* buf, int len, int flags,
                                               const struct sockaddr* to, int tolen) {
    return g_real_sendto ? g_real_sendto(s, buf, len, flags, to, tolen) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_WSACleanup(void) {
    return g_real_WSACleanup ? g_real_WSACleanup() : SOCKET_ERROR;
}

__declspec(dllexport) unsigned long PASCAL proxy_inet_addr(const char* cp) {
    return g_real_inet_addr ? g_real_inet_addr(cp) : INADDR_NONE;
}

__declspec(dllexport) int PASCAL proxy_closesocket(SOCKET s) {
    return g_real_closesocket ? g_real_closesocket(s) : SOCKET_ERROR;
}

__declspec(dllexport) u_long PASCAL proxy_htonl(u_long hostlong) {
    return g_real_htonl ? g_real_htonl(hostlong) : 0;
}

__declspec(dllexport) int PASCAL proxy_WSAAsyncSelect(SOCKET s, HWND hWnd, u_int wMsg, long lEvent) {
    return g_real_WSAAsyncSelect ? g_real_WSAAsyncSelect(s, hWnd, wMsg, lEvent) : SOCKET_ERROR;
}

__declspec(dllexport) SOCKET PASCAL proxy_accept(SOCKET s, struct sockaddr* addr, int* addrlen) {
    return g_real_accept ? g_real_accept(s, addr, addrlen) : INVALID_SOCKET;
}

__declspec(dllexport) int PASCAL proxy_recv(SOCKET s, char* buf, int len, int flags) {
    return g_real_recv ? g_real_recv(s, buf, len, flags) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_send(SOCKET s, const char* buf, int len, int flags) {
    return g_real_send ? g_real_send(s, buf, len, flags) : SOCKET_ERROR;
}

} // extern "C"

// ============================================================================
// DLL Entry Point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_processStartTick = GetTickCount();
        g_startupTraceCount = 0;
        g_lastStartupMode = -1;
        g_lastStartupSubstate = -1;
        g_lastStartupExitCode = -1;
        g_lastStartupSuppress = -1;
        g_lastStartupLoaded = -1;
        g_lastStartupMsgHook = -1;
        g_lastStartupHookModule = -1;
        g_lastStartupCustomProc = -1;
        g_lastStartupMsgCallback = -1;
        g_startupTitleReached = false;

        WsockLog("wsock32 proxy loaded (PID %lu)", GetCurrentProcessId());
        LogStartupProgressState("DllMain-process-attach", true);

        // 1. Set the DXLib duplicate-allow flag
        SetDXLibDuplicateFlag();

        // 2. Patch FindWindowA in the game's IAT
        PatchFindWindowA();

        // 3. Optionally patch the game's early hotkey/IME suppression APIs
        // before the window activation path runs.
        if (kEnableEarlyInputWorkarounds) {
            PatchEarlyHotkeyImports();
        } else {
            WsockLog("[EARLYPATCH] Startup input hotkey/IME workarounds disabled; leaving duplicate-instance bypass only");
        }

        // 4. Load real wsock32.dll and resolve function pointers
        if (!LoadRealWsock32()) {
            WsockLog("FATAL: Could not load real wsock32.dll — aborting");
            return FALSE;
        }

        WsockLog("wsock32 proxy init complete");
        LogStartupProgressState("wsock32-init-complete", true);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_hRealWsock32) {
            FreeLibrary(g_hRealWsock32);
            g_hRealWsock32 = nullptr;
        }
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }
    return TRUE;
}
