/**
 * Minimal wsock32.dll proxy — duplicate-instance bypass
 *
 * The game statically imports WSOCK32.dll, so this DLL loads at process start
 * (before DXLib_Init).  We use that timing for duplicate-instance bypass and
 * retain the old early hotkey/IME workarounds behind a feature flag.
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
static DWORD g_processStartTick = 0;
static HWND g_gameWindow = nullptr;

static void WsockLog(const char* fmt, ...);

static constexpr uintptr_t kAddrShellHotkeySuppressFlag = 0x009E5B74;
static constexpr uintptr_t kAddrShellHotkeyAuxHook = 0x009E5B78;
static constexpr uintptr_t kAddrShellHotkeyMsgHook = 0x009E5B7C;
static constexpr uintptr_t kAddrShellHotkeyTempDllPath = 0x009E5B84;
static constexpr uintptr_t kAddrShellHotkeyTempDllOwned = 0x009E5C88;
static constexpr uintptr_t kAddrShellHotkeyHookModule = 0x009E5C8C;
static constexpr bool kEnableEarlyInputWorkarounds = false;

static void LogShellHotkeySuppressionState(const char* reason) {
    int* suppressFlag = reinterpret_cast<int*>(kAddrShellHotkeySuppressFlag);
    HHOOK* auxHookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyAuxHook);
    HHOOK* hookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyMsgHook);
    char* tempDllPath = reinterpret_cast<char*>(kAddrShellHotkeyTempDllPath);
    int* tempDllOwned = reinterpret_cast<int*>(kAddrShellHotkeyTempDllOwned);
    HMODULE* hookModule = reinterpret_cast<HMODULE*>(kAddrShellHotkeyHookModule);

    WsockLog("[EARLYPATCH] Shell hotkey state (%s): suppress=%d aux=0x%p msg=0x%p module=0x%p owned=%d path='%s'",
             reason ? reason : "unknown",
             *suppressFlag,
             *auxHookHandle,
             *hookHandle,
             *hookModule,
             *tempDllOwned,
             tempDllPath);
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
    HHOOK* auxHookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyAuxHook);
    HHOOK* hookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyMsgHook);
    char* tempDllPath = reinterpret_cast<char*>(kAddrShellHotkeyTempDllPath);
    int* tempDllOwned = reinterpret_cast<int*>(kAddrShellHotkeyTempDllOwned);
    HMODULE* hookModule = reinterpret_cast<HMODULE*>(kAddrShellHotkeyHookModule);

    bool changed = false;

    if (*suppressFlag != 0) {
        *suppressFlag = 0;
        changed = true;
    }

    if (*auxHookHandle) {
        UnhookWindowsHookEx(*auxHookHandle);
        *auxHookHandle = nullptr;
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

    if (changed) {
        WsockLog("[EARLYPATCH] Cleared vanilla shell hotkey suppression state (%s)",
                 reason ? reason : "unknown");
    }

    LogShellHotkeySuppressionState(reason ? reason : "clear-state");

    return changed;
}

static bool ShouldSuppressLateFocusSteal(HWND hWnd, const char* apiName) {
    if (!hWnd || !g_gameWindow || hWnd != g_gameWindow) {
        return false;
    }

    const DWORD elapsedMs = GetTickCount() - g_processStartTick;
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

static FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    if (lpProcName && !IS_INTRESOURCE(lpProcName) && lstrcmpA(lpProcName, "SetMSGHookDll") == 0) {
        ClearVanillaShellHotkeySuppressionState("SetMSGHookDll lookup");

        if (!g_loggedSetMsgHookBlock) {
            WsockLog("[EARLYPATCH] Blocked SetMSGHookDll export lookup before helper hook install");
            g_loggedSetMsgHookBlock = true;
        }
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }

    return ::GetProcAddress(hModule, lpProcName);
}

static BOOL WINAPI Hooked_SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) {
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

    if (hWnd && !g_gameWindow) {
        g_gameWindow = hWnd;
        WsockLog("[EARLYPATCH] Learned game window hwnd=0x%p from WINNLSEnableIME", hWnd);
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

static void PatchEarlyHotkeyImports() {
    LogShellHotkeySuppressionState("process attach (before clear)");
    ClearVanillaShellHotkeySuppressionState("process attach");

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

    LogShellHotkeySuppressionState("after PatchEarlyHotkeyImports");
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

        WsockLog("wsock32 proxy loaded (PID %lu)", GetCurrentProcessId());

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
