#include "patches/input_override.h"
#include "patches/input_sync_hooks.h"
#include "input_system.h"
#include "patches/memory_utils.h"
#include "as2_constants.h"
#include "log_window.h"
#include "net/netplay_menu_controller.h"
#include "net/netplay_pacing.h"
#include "net/netplay_phase_runtime.h"
#include "net/session_manager.h"
#include "net/charsel_sync.h"
#include "net/delay_policy.h"
#include "net/frontend_input_sync.h"
#include "net/match_lifecycle.h"
#include "net/pregame_sync.h"
#include "net/spectator_playback.h"
#include "net/stagesel_sync.h"
#include "net/winscreen_sync.h"
#include "net/player_side_mapping.h"
#include "patches/charsel_palette_select.h"
#include "patches/charsel_select_actions.h"
#include "core/game_state.h"
#include "replay/replay_runtime.h"
#include "rollback/rollback_session.h"
#include "rollback/online_wiring.h"
#include "rollback/netplay_log.h"
#include "training/practice_tools.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <intrin.h>

// ============================================================================
// Original function pointer storage (populated by hook_installer)
// ============================================================================

KeyboardState_t g_origKeyboardState = nullptr;
JoystickState_t g_origJoystickState = nullptr;
InputProcess_t g_origInputProcess = nullptr;
InputDispatcher_t g_origInputDispatcher = nullptr;
DInputKBRefresh_t g_origDInputKBRefresh = nullptr;
DInputJoyRefresh_t g_origDInputJoyRefresh = nullptr;
GetKeyboardState_t g_origGetKeyboardState = nullptr;
ClipCursor_t g_origClipCursor = nullptr;
GetProcAddress_t g_origGetProcAddress = nullptr;
SystemParametersInfoA_t g_origSystemParametersInfoA = nullptr;
WINNLSEnableIME_t g_origWINNLSEnableIME = nullptr;
DInputSetCooperativeLevel_t g_origDInputKeyboardSetCooperativeLevel = nullptr;

// ============================================================================
// Module config access
// ============================================================================

// Defined in mod_main.cpp
extern bool ModConfig_UseSDLInput();
extern bool ModConfig_VerboseLogging();

static bool s_inputGuardSettingsLoaded = false;
static bool s_inputGuardSettingsPathResolved = false;
static wchar_t s_inputGuardSettingsPathW[MAX_PATH] = {};
static char s_inputGuardSettingsPathUtf8[MAX_PATH * 3] = {};
static bool s_enableShellHotkeyImeWorkarounds = true;
// System-key stripping (Win/Apps) stays disabled by shell hotkey policy; the
// trace/diagnostic toggles below are no longer INI-configurable and remain off.
static bool s_enableSystemKeyWorkarounds = false;
static bool s_enableSwallowTrace = false;
static bool s_enableHotkeyTraceLog = false;
static uint32_t s_inputGuardDiagIntervalSec = 15;
static uint32_t s_swallowTraceStripLogCount = 0;
static uint32_t s_swallowTraceKeyboardPollCount = 0;
static DWORD s_swallowTraceLastStripLogMs = 0;
static constexpr uint32_t kSwallowTraceBurstMax = 64;
static constexpr DWORD kSwallowTraceThrottleMs = 400;
static DWORD s_lastInputGuardDiagMs = 0;
static uint32_t s_shellSuppressClearCount = 0;
static uint32_t s_dinputRepairAttemptCount = 0;
static uint32_t s_dinputRepairSuccessCount = 0;
static uint32_t s_dinputRepairSkipNoDevice = 0;
static uint32_t s_dinputRepairSkipNoHwnd = 0;
static uint32_t s_dinputRepairSkipNotFocused = 0;
static uint32_t s_shellStateProvenanceLogCount = 0;
static int32_t s_lastProvSuppressFlag = -1;
static intptr_t s_lastProvMsgHook = -1;
static int32_t s_lastProvLoadedFlag = -1;
static intptr_t s_lastProvHookModule = -1;
static int32_t s_lastProvTempOwned = -1;
static intptr_t s_lastProvCustomProc = -1;
static intptr_t s_lastProvMsgCallback = -1;
static int32_t s_lastProvCustomGate = -1;
static int32_t s_lastProvGameMode = -1;
static int32_t s_lastProvSubstate = -1;

static void ResolveInputGuardSettingsPath() {
    if (s_inputGuardSettingsPathResolved) {
        return;
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        wcscpy_s(s_inputGuardSettingsPathW, L"as2_rollback_settings.ini");
    } else {
        wchar_t* slash = wcsrchr(path, L'\\');
        wchar_t* fwdSlash = wcsrchr(path, L'/');
        if (!slash || (fwdSlash && fwdSlash > slash)) {
            slash = fwdSlash;
        }
        if (slash) {
            slash[1] = L'\0';
        } else {
            path[0] = L'\0';
        }
        swprintf_s(s_inputGuardSettingsPathW, L"%lsas2_rollback_settings.ini", path);
    }

    s_inputGuardSettingsPathUtf8[0] = '\0';
    WideCharToMultiByte(CP_UTF8,
                        0,
                        s_inputGuardSettingsPathW,
                        -1,
                        s_inputGuardSettingsPathUtf8,
                        sizeof(s_inputGuardSettingsPathUtf8),
                        nullptr,
                        nullptr);
    s_inputGuardSettingsPathResolved = true;
}

static bool ReadInputGuardIniBool(const wchar_t* key, bool fallback, bool* found) {
    wchar_t value[64] = {};
    ResolveInputGuardSettingsPath();
    GetPrivateProfileStringW(L"ModSettings", key, L"", value,
        (DWORD)(sizeof(value) / sizeof(value[0])), s_inputGuardSettingsPathW);
    if (value[0] == L'\0') {
        if (found) {
            *found = false;
        }
        return fallback;
    }

    if (found) {
        *found = true;
    }
    return _wcsicmp(value, L"1") == 0 ||
           _wcsicmp(value, L"true") == 0 ||
           _wcsicmp(value, L"yes") == 0 ||
           _wcsicmp(value, L"on") == 0;
}

static void WriteInputGuardDefaultBool(const wchar_t* key, bool enabled) {
    ResolveInputGuardSettingsPath();
    WritePrivateProfileStringW(L"ModSettings",
                               key,
                               enabled ? L"1" : L"0",
                               s_inputGuardSettingsPathW);
}

static void SyncInputGuardIniKeys() {
    WriteInputGuardDefaultBool(L"input_guard_shell_hotkeys_ime", s_enableShellHotkeyImeWorkarounds);
}

void InputOverride_GetIniSnapshot(InputGuardIniSnapshot* out) {
    if (!out) {
        return;
    }
    InputOverride_LoadSettings();
    out->shell_hotkeys_ime = s_enableShellHotkeyImeWorkarounds;
}

void InputOverride_SyncIniKeys() {
    InputOverride_LoadSettings();
    SyncInputGuardIniKeys();
}

void InputOverride_LoadSettings() {
    if (s_inputGuardSettingsLoaded) {
        return;
    }

    bool foundShell = false;

    s_enableShellHotkeyImeWorkarounds =
        ReadInputGuardIniBool(L"input_guard_shell_hotkeys_ime", true, &foundShell);

    (void)foundShell;
    SyncInputGuardIniKeys();

    s_inputGuardSettingsLoaded = true;
    LOG_INFO("[InputGuard] Settings: shell_hotkeys_ime=%d (%s) game_wndproc=ModCallGameWndProc path=%s",
        s_enableShellHotkeyImeWorkarounds ? 1 : 0,
        foundShell ? "ini" : "written",
        s_inputGuardSettingsPathUtf8[0] ? s_inputGuardSettingsPathUtf8 : "as2_rollback_settings.ini");
}

bool InputOverride_AreShellHotkeyImeWorkaroundsEnabled() {
    if (!s_inputGuardSettingsLoaded) {
        InputOverride_LoadSettings();
    }
    return s_enableShellHotkeyImeWorkarounds;
}

bool InputOverride_AreSystemKeyWorkaroundsEnabled() {
    if (!s_inputGuardSettingsLoaded) {
        InputOverride_LoadSettings();
    }
    return s_enableSystemKeyWorkarounds;
}

// ============================================================================
// Internal state
// ============================================================================

// Per-session startup tracking — reset when a new rollback session begins.
// Must be declared before AbortRollbackDispatcher which references it.
static bool s_rollbackSessionWasActive = false;

static int AbortRollbackDispatcher(const char* fallbackReason) {
    const char* reason = Rollback::RollbackSession_GetErrorReason();
    if (!reason || !reason[0]) {
        reason = fallbackReason ? fallbackReason : "Rollback session failure";
    }

    Rollback::NetplayLog_Write("DISCONNECT",
        Rollback::RollbackSession_GetCurrentFrame(),
        "Dispatcher abort: %s", reason);

    InputSyncHooks_SetTimesyncFreeze(false);
    s_rollbackSessionWasActive = false;  // force re-init on next session

    // End the GekkoNet session immediately so subsequent dispatcher calls
    // cannot re-enter this path through RollbackSession_IsActive().
    Rollback::RollbackSession_End();

    // Route to the netplay menu disconnect error screen. HandleDisconnection
    // forces the game back to MODE_MENU, cancels the ENet session, and shows
    // the DisconnectError overlay with the reason string.
    // Background rollback polling can move the lifecycle into
    // DisconnectRecovery before the dispatcher reaches this abort path, but
    // that phase transition alone does not mean Session_Cancel() already ran.
    // Keep routing through HandleDisconnection while the transport session is
    // still alive so the peer is actively torn down instead of waiting for a
    // later ENet timeout.
    const Net::SessionState sessionState = Net::Session_GetState();
    const bool sessionNeedsTeardown =
        sessionState != Net::SessionState::Idle &&
        sessionState != Net::SessionState::Failed &&
        sessionState != Net::SessionState::Disconnecting;
    if (sessionNeedsTeardown ||
        Net::MatchLifecycle_GetPhase() != Net::MatchLifecyclePhase::DisconnectRecovery) {
        NetMenu::HandleDisconnection(reason);
    }

    return -1;
}

static int g_hookCallCount = 0;
static int g_lastInputUpdateFrame = -1;
static bool s_shellHotkeyPatchLogged = false;
static bool s_shellHotkeyBlockLogged = false;
static bool s_legacyShellHotkeyBlockLogged = false;
static bool s_imeDisableBlockLogged = false;
static bool s_shellHotkeyStateLogged = false;
static bool s_mouseClipBlockLogged = false;
static bool s_imeRepairLogged = false;
static HWND  s_lastImeRepairHwnd = nullptr;
static bool s_cursorRepairLogged = false;
static bool s_initialClipReleased = false;
static bool s_cursorVisibilityOk = false;
static bool s_dinputKeyboardWindowMissingLogged = false;
static void* s_lastDInputKeyboardDevice = nullptr;
static HWND s_lastDInputKeyboardWindow = nullptr;
static bool s_dinputKeyboardCoopApplied = false;
static uint32_t s_dinputKeyboardCoopHookLogCount = 0;
static uint32_t s_dinputKeyboardCoopRepairLogCount = 0;
static DWORD s_lastDInputKeyboardCoopRepairMs = 0;
static uint32_t s_dinputJoyRefreshLogCount = 0;
static uint32_t s_joystickStateLogCount = 0;
static uint32_t s_joystickUnknownLogCount = 0;

InputDebugInfo g_inputDebug = {};

static inline bool IsCharSelDispatcherLockstepSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_SELECT ||
           substate == CHARSEL_SUB_CONFIRM;
}

static inline bool IsStageSelRawLockstepSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_STAGESEL_GRID ||
           substate == CHARSEL_SUB_STAGESEL_CONFIRM;
}

static inline bool ShouldHoldCharSelUntilLockstep(uint32_t mode, uint32_t substate) {
    return mode == MODE_CHARSEL &&
           IsCharSelDispatcherLockstepSubstate(substate) &&
           Net::PregameSync_IsActive() &&
           !Net::CharSelSync_IsLockstepActive();
}

static constexpr DWORD kDInputCoopExclusive = 0x00000001u;
static constexpr DWORD kDInputCoopNonexclusive = 0x00000002u;

static bool ClearVanillaDInputJoyState(int joyIndex, const char* reason) {
    if (joyIndex < 0 || joyIndex >= DINPUT_JOY_MAX) {
        return false;
    }

    static const uint8_t kZeroJoyState[0x50] = {};
    const uintptr_t joyBase = ADDR_DINPUT_JOYSTICK + (joyIndex * DINPUT_JOY_STRUCT_SIZE);
    const bool ok = WriteMemoryBlockSafe(reinterpret_cast<void*>(joyBase),
                                         kZeroJoyState,
                                         sizeof(kZeroJoyState));

    const bool verboseInputLog = ModConfig_VerboseLogging() || Rollback::NetplayLog_IsVerbose();
    if (((verboseInputLog && s_dinputJoyRefreshLogCount < 16) ||
         s_dinputJoyRefreshLogCount < 2 ||
         !ok) && reason) {
        LOG_INFO("[InputHook] Cleared vanilla DInput joy state idx=%d base=0x%08X bytes=%u buttonsOff=0x%X ok=%d reason=%s",
                 joyIndex,
                 (unsigned)joyBase,
                 (unsigned)sizeof(kZeroJoyState),
                 DINPUT_JOY_BTN_OFFSET,
                 ok ? 1 : 0,
                 reason);
        s_dinputJoyRefreshLogCount++;
    }

    return ok;
}
static constexpr DWORD kDInputCoopForeground = 0x00000004u;
static constexpr DWORD kDInputCoopBackground = 0x00000008u;
static constexpr DWORD kDInputCoopNoWinKey = 0x00000010u;  // DISCL_NOWINKEY
static constexpr DWORD kDInputCoopRepairIntervalMs = 2000;

struct DInputKeyboardDeviceVTable {
    void* QueryInterface;
    void* AddRef;
    void* Release;
    void* GetCapabilities;
    void* EnumObjects;
    void* GetProperty;
    void* SetProperty;
    HRESULT (STDMETHODCALLTYPE *Acquire)(void* device);
    HRESULT (STDMETHODCALLTYPE *Unacquire)(void* device);
    void* GetDeviceState;
    void* GetDeviceData;
    void* SetDataFormat;
    void* SetEventNotification;
    HRESULT (STDMETHODCALLTYPE *SetCooperativeLevel)(void* device, HWND hWnd, DWORD dwFlags);
};

static void* GetDInputKeyboardDevice() {
    return *reinterpret_cast<void**>(ADDR_DINPUT_KB_DEVICE);
}

static DInputKeyboardDeviceVTable* GetDInputKeyboardDeviceVTable(void* device) {
    return device ? *reinterpret_cast<DInputKeyboardDeviceVTable**>(device) : nullptr;
}

static HWND GetGameWindowHandle() {
    auto getWindowHandle = reinterpret_cast<GetWindowHandle_t>(ADDR_WINDOW_GET_HANDLE);
    return getWindowHandle ? reinterpret_cast<HWND>(getWindowHandle()) : nullptr;
}

static bool IsGameWindowForeground(HWND gameWindow) {
    if (!gameWindow) {
        return false;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground) {
        if (foreground == gameWindow) {
            return true;
        }

        if (GetAncestor(foreground, GA_ROOT) == gameWindow) {
            return true;
        }

        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(foreground, &foregroundPid);
        if (foregroundPid == GetCurrentProcessId()) {
            HWND active = GetActiveWindow();
            if (active == gameWindow || GetAncestor(active, GA_ROOT) == gameWindow) {
                return true;
            }
        }
    }

    HWND active = GetActiveWindow();
    return active == gameWindow || GetAncestor(active, GA_ROOT) == gameWindow;
}

static DWORD SanitizeDInputKeyboardCooperativeFlags(DWORD flags) {
    DWORD sanitized = flags & ~(kDInputCoopExclusive | kDInputCoopNoWinKey);
    sanitized |= kDInputCoopNonexclusive;
    if ((sanitized & (kDInputCoopForeground | kDInputCoopBackground)) == 0) {
        sanitized |= kDInputCoopForeground;
    }
    return sanitized;
}

static inline const void* CaptureCallerAddress();
static void LogShellStateProvenance(const char* reason, const void* caller, bool forceLog);

static bool ShouldRepairDInputKeyboardCooperativeLevel() {
    // Disabled: the DInput unacquire experiment proved the keyboard's background
    // cooperative level is unrelated to the Win key / Alt+Shift / middle-click issues
    // (the real fix is dropping the per-frame keybd_event(VK 0x07) phantom key in
    // wsock32_proxy). Forcing FOREGROUND coop + periodic Unacquire/Acquire only fought
    // vanilla and added needless per-interval work, so this repair is now inert. The
    // SetCooperativeLevel hook stays installed as a harmless pass-through.
    return false;
}

void* InputOverride_GetDInputKeyboardSetCooperativeLevelTarget() {
    void* device = GetDInputKeyboardDevice();
    DInputKeyboardDeviceVTable* vtable = GetDInputKeyboardDeviceVTable(device);
    return (vtable && vtable->SetCooperativeLevel)
        ? reinterpret_cast<void*>(vtable->SetCooperativeLevel)
        : nullptr;
}

HRESULT STDMETHODCALLTYPE Hook_DInputKeyboardSetCooperativeLevel(void* device, HWND hWnd, DWORD dwFlags) {
    LogShellStateProvenance("DInputKeyboardSetCooperativeLevel-enter", CaptureCallerAddress(), false);
    if (!g_origDInputKeyboardSetCooperativeLevel) {
        return E_FAIL;
    }

    if (!ShouldRepairDInputKeyboardCooperativeLevel()) {
        return g_origDInputKeyboardSetCooperativeLevel(device, hWnd, dwFlags);
    }

    const DWORD sanitizedFlags = SanitizeDInputKeyboardCooperativeFlags(dwFlags);
    HWND targetWindow = hWnd ? hWnd : GetGameWindowHandle();

    if (s_dinputKeyboardCoopHookLogCount < 8
            && (sanitizedFlags != dwFlags || targetWindow != hWnd)) {
        ++s_dinputKeyboardCoopHookLogCount;
        LOG_INFO("[Input] Sanitized DInput keyboard SetCooperativeLevel: hwnd=0x%p -> 0x%p flags=0x%08lX -> 0x%08lX",
                 hWnd,
                 targetWindow,
                 static_cast<unsigned long>(dwFlags),
                 static_cast<unsigned long>(sanitizedFlags));
    }
    return g_origDInputKeyboardSetCooperativeLevel(device, targetWindow, sanitizedFlags);
}

void InputOverride_EnsureDInputKeyboardCooperativeLevel(const char* reason) {
    if (!ShouldRepairDInputKeyboardCooperativeLevel()) {
        return;
    }

    void* device = GetDInputKeyboardDevice();
    DInputKeyboardDeviceVTable* vtable = GetDInputKeyboardDeviceVTable(device);
    HWND gameWindow = GetGameWindowHandle();

    if (!device || !vtable || !vtable->Acquire || !vtable->Unacquire || !vtable->SetCooperativeLevel) {
        ++s_dinputRepairSkipNoDevice;
        return;
    }

    if (!gameWindow) {
        ++s_dinputRepairSkipNoHwnd;
        if (!s_dinputKeyboardWindowMissingLogged) {
            LOG_WARN("[InputGuard] Could not resolve game HWND for DInput keyboard cooperative-level fix");
            s_dinputKeyboardWindowMissingLogged = true;
        }
        return;
    }

    if (!IsGameWindowForeground(gameWindow)) {
        ++s_dinputRepairSkipNotFocused;
        return;
    }

    const DWORD now = GetTickCount();
    if (device == s_lastDInputKeyboardDevice
            && gameWindow == s_lastDInputKeyboardWindow
            && s_dinputKeyboardCoopApplied
            && s_lastDInputKeyboardCoopRepairMs != 0
            && (DWORD)(now - s_lastDInputKeyboardCoopRepairMs) < kDInputCoopRepairIntervalMs) {
        return;
    }

    ++s_dinputRepairAttemptCount;

    const DWORD desiredFlags = kDInputCoopNonexclusive | kDInputCoopForeground;
    const HRESULT hrUnacquire = vtable->Unacquire(device);
    const HRESULT hrSet = vtable->SetCooperativeLevel(device, gameWindow, desiredFlags);
    const HRESULT hrAcquire = vtable->Acquire(device);

    if (s_dinputKeyboardCoopRepairLogCount < 8 || FAILED(hrSet)) {
        ++s_dinputKeyboardCoopRepairLogCount;
        LOG_INFO("[Input] DInput keyboard cooperative-level fix (%s): device=0x%p hwnd=0x%p flags=0x%08lX unacquire=0x%08lX set=0x%08lX acquire=0x%08lX",
                 reason ? reason : "unknown",
                 device,
                 gameWindow,
                 static_cast<unsigned long>(desiredFlags),
                 static_cast<unsigned long>(hrUnacquire),
                 static_cast<unsigned long>(hrSet),
                 static_cast<unsigned long>(hrAcquire));
    }

    s_lastDInputKeyboardDevice = device;
    s_lastDInputKeyboardWindow = gameWindow;
    s_dinputKeyboardCoopApplied = SUCCEEDED(hrSet);
    if (SUCCEEDED(hrSet)) {
        ++s_dinputRepairSuccessCount;
    }
    s_lastDInputKeyboardCoopRepairMs = now;
    s_dinputKeyboardWindowMissingLogged = false;
}

static bool ClearVanillaShellHotkeySuppression() {
    int* suppressFlag = reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG);
    HHOOK* hookHandle = reinterpret_cast<HHOOK*>(ADDR_SHELL_HOTKEY_MSG_HOOK);
    int* loadedFlag = reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_LOADED_FLAG);
    HMODULE* hookModule = reinterpret_cast<HMODULE*>(ADDR_SHELL_HOTKEY_HOOK_MODULE);
    int* tempDllOwned = reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_TEMP_DLL_OWNED);
    char* tempDllPath = reinterpret_cast<char*>(ADDR_SHELL_HOTKEY_TEMP_DLL_PATH);
    intptr_t* customProc = reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR);
    intptr_t* msgCallback = reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_MSG_CALLBACK);

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

    if (*loadedFlag != 0) {
        *loadedFlag = 0;
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

    return changed;
}

static void LogSwallowTracePollSnapshot(const char* reason);
static void PollShellKeyAsyncEdges();
static bool IsShellKeyTraceEnabled();

struct ShellKeyAsyncSample {
    bool lWin = false;
    bool rWin = false;
    bool apps = false;
    bool alt = false;
    bool shift = false;
    bool initialized = false;
};

static ShellKeyAsyncSample s_shellKeyAsyncSample;

static void FormatWindowBrief(HWND hwnd, char* out, size_t outSize);
static int ReadShellSuppressFlagSafe();
static void MaybeLogShellTraceStartup();
static inline const void* CaptureCallerAddress() {
#if defined(_MSC_VER)
    return _ReturnAddress();
#else
    return nullptr;
#endif
}

static void LogShellStateProvenance(const char* reason, const void* caller, bool forceLog) {
    int suppressFlag = -1;
    intptr_t msgHook = 0;
    int loadedFlag = -1;
    intptr_t hookModule = 0;
    int tempDllOwned = -1;
    intptr_t customProc = 0;
    intptr_t msgCallback = 0;
    int customGate = -1;

    __try {
        suppressFlag = *reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG);
        msgHook = reinterpret_cast<intptr_t>(*reinterpret_cast<HHOOK*>(ADDR_SHELL_HOTKEY_MSG_HOOK));
        loadedFlag = *reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_LOADED_FLAG);
        hookModule = reinterpret_cast<intptr_t>(*reinterpret_cast<HMODULE*>(ADDR_SHELL_HOTKEY_HOOK_MODULE));
        tempDllOwned = *reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_TEMP_DLL_OWNED);
        customProc = *reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR);
        msgCallback = *reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_MSG_CALLBACK);
        customGate = *reinterpret_cast<int*>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    const int32_t gameMode = static_cast<int32_t>(GetGameMode());
    const int32_t substate = static_cast<int32_t>(GetSubstate());
    const bool changed =
        suppressFlag != s_lastProvSuppressFlag ||
        msgHook != s_lastProvMsgHook ||
        loadedFlag != s_lastProvLoadedFlag ||
        hookModule != s_lastProvHookModule ||
        tempDllOwned != s_lastProvTempOwned ||
        customProc != s_lastProvCustomProc ||
        msgCallback != s_lastProvMsgCallback ||
        customGate != s_lastProvCustomGate ||
        gameMode != s_lastProvGameMode ||
        substate != s_lastProvSubstate;

    if (!(forceLog || changed) || s_shellStateProvenanceLogCount >= 512) {
        return;
    }

    ++s_shellStateProvenanceLogCount;
    LOG_INFO("[STARTUPTRACE][Provenance] #%u reason=%s caller=0x%p mode=%d sub=%d suppress=%d msg=0x%p loaded=%d module=0x%p owned=%d gate=%d custom=0x%p cb=0x%p",
             s_shellStateProvenanceLogCount,
             reason ? reason : "unknown",
             caller,
             gameMode,
             substate,
             suppressFlag,
             reinterpret_cast<void*>(msgHook),
             loadedFlag,
             reinterpret_cast<void*>(hookModule),
             tempDllOwned,
             customGate,
             reinterpret_cast<void*>(customProc),
             reinterpret_cast<void*>(msgCallback));

    s_lastProvSuppressFlag = suppressFlag;
    s_lastProvMsgHook = msgHook;
    s_lastProvLoadedFlag = loadedFlag;
    s_lastProvHookModule = hookModule;
    s_lastProvTempOwned = tempDllOwned;
    s_lastProvCustomProc = customProc;
    s_lastProvMsgCallback = msgCallback;
    s_lastProvCustomGate = customGate;
    s_lastProvGameMode = gameMode;
    s_lastProvSubstate = substate;
}

static void LogVanillaShellHotkeyState(const char* reason) {
    int* suppressFlag = reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG);
    HHOOK* hookHandle = reinterpret_cast<HHOOK*>(ADDR_SHELL_HOTKEY_MSG_HOOK);
    int* loadedFlag = reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_LOADED_FLAG);
    HMODULE* hookModule = reinterpret_cast<HMODULE*>(ADDR_SHELL_HOTKEY_HOOK_MODULE);
    int* tempDllOwned = reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_TEMP_DLL_OWNED);
    char* tempDllPath = reinterpret_cast<char*>(ADDR_SHELL_HOTKEY_TEMP_DLL_PATH);
    intptr_t* customProc = reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR);
    intptr_t* msgCallback = reinterpret_cast<intptr_t*>(ADDR_GAME_WNDPROC_MSG_CALLBACK);

    LOG_INFO("[Input] Shell hotkey state (%s): suppress=%d msg=0x%p loaded=%d module=0x%p owned=%d custom=0x%p cb=0x%p path='%s' fg=0x%p active=0x%p focus=0x%p",
             reason ? reason : "unknown",
             *suppressFlag,
             *hookHandle,
             *loadedFlag,
             *hookModule,
             *tempDllOwned,
             (void*)(*customProc),
             (void*)(*msgCallback),
             tempDllPath,
             GetForegroundWindow(),
             GetActiveWindow(),
             GetFocus());
    LogShellStateProvenance(reason ? reason : "shell-state", CaptureCallerAddress(), false);
}

InputOverrideGameWndProcShellGate::InputOverrideGameWndProcShellGate() : saved(0), active(false) {
    if (!s_enableShellHotkeyImeWorkarounds) {
        return;
    }

    __try {
        int* gate = reinterpret_cast<int*>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER);
        saved = *gate;
        if (saved != 0) {
            *gate = 0;
            active = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        active = false;
    }
}

InputOverrideGameWndProcShellGate::~InputOverrideGameWndProcShellGate() {
    if (!active) {
        return;
    }

    __try {
        *reinterpret_cast<int*>(ADDR_GAME_WNDPROC_CUSTOM_HANDLER) = saved;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void EnsureVanillaShellHotkeysEnabled() {
    if (!s_enableShellHotkeyImeWorkarounds) {
        return;
    }

    LogShellStateProvenance("EnsureVanillaShellHotkeysEnabled-before", CaptureCallerAddress(), false);
    const bool changed = ClearVanillaShellHotkeySuppression();

    if (changed) {
        ++s_shellSuppressClearCount;
        if (!s_shellHotkeyPatchLogged) {
            LOG_INFO("[InputGuard] Disabled vanilla system hotkey suppression (Win key / Alt+Shift fix)");
            s_shellHotkeyPatchLogged = true;
        } else {
            LOG_INFO("[InputGuard] Re-cleared vanilla shell hotkey suppression (count=%u)",
                s_shellSuppressClearCount);
        }
    }

    if (changed || !s_shellHotkeyStateLogged) {
        LogVanillaShellHotkeyState(changed ? "EnsureVanillaShellHotkeysEnabled changed" : "EnsureVanillaShellHotkeysEnabled initial");
        s_shellHotkeyStateLogged = true;
    }
    LogShellStateProvenance("EnsureVanillaShellHotkeysEnabled-after", CaptureCallerAddress(), false);
}

static void LogInputGuardDiagnostics(const char* reason) {
    const HWND gameWindow = GetGameWindowHandle();
    const HWND foreground = GetForegroundWindow();
    const HWND active = GetActiveWindow();
    const HWND focus = GetFocus();
    void* device = GetDInputKeyboardDevice();
    const bool focused = IsGameWindowForeground(gameWindow);

    int suppressFlag = 0;
    HHOOK msgHook = nullptr;
    int loadedFlag = 0;
    __try {
        suppressFlag = *reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG);
        msgHook = *reinterpret_cast<HHOOK*>(ADDR_SHELL_HOTKEY_MSG_HOOK);
        loadedFlag = *reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_LOADED_FLAG);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    char fgBrief[256] = {};
    FormatWindowBrief(foreground, fgBrief, sizeof(fgBrief));
    WNDPROC gameWndProc = nullptr;
    if (gameWindow) {
        gameWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC));
    }

    LOG_INFO("[InputGuard] DIAG (%s): shell=%d system=%d hotkey_trace=%d swallow_trace=%d "
             "suppress=%d hook=0x%p loaded=%d game_hwnd=0x%p wndproc=0x%p fg=%s "
             "active=0x%p focus=0x%p focused=%d dinput_dev=0x%p dinput_applied=%d "
             "suppress_clears=%u dinput_try=%u ok=%u skip{dev=%u hwnd=%u focus=%u} "
             "hooks{gpa=%d spi=%d ime=%d coop=%d}",
        reason ? reason : "heartbeat",
        s_enableShellHotkeyImeWorkarounds ? 1 : 0,
        s_enableSystemKeyWorkarounds ? 1 : 0,
        s_enableHotkeyTraceLog ? 1 : 0,
        s_enableSwallowTrace ? 1 : 0,
        suppressFlag,
        msgHook,
        loadedFlag,
        gameWindow,
        gameWndProc,
        fgBrief,
        active,
        focus,
        focused ? 1 : 0,
        device,
        s_dinputKeyboardCoopApplied ? 1 : 0,
        s_shellSuppressClearCount,
        s_dinputRepairAttemptCount,
        s_dinputRepairSuccessCount,
        s_dinputRepairSkipNoDevice,
        s_dinputRepairSkipNoHwnd,
        s_dinputRepairSkipNotFocused,
        g_origGetProcAddress ? 1 : 0,
        g_origSystemParametersInfoA ? 1 : 0,
        g_origWINNLSEnableIME ? 1 : 0,
        g_origDInputKeyboardSetCooperativeLevel ? 1 : 0);

    Rollback::NetplayLog_Write("INPUTGUARD", -1,
        "DIAG %s suppress=%d hook=0x%p game=0x%p fg=0x%p focused=%d dinput=0x%p coop_ok=%d clears=%u try=%u ok=%u skip=%u/%u/%u",
        reason ? reason : "heartbeat",
        suppressFlag,
        msgHook,
        gameWindow,
        foreground,
        focused ? 1 : 0,
        device,
        s_dinputKeyboardCoopApplied ? 1 : 0,
        s_shellSuppressClearCount,
        s_dinputRepairAttemptCount,
        s_dinputRepairSuccessCount,
        s_dinputRepairSkipNoDevice,
        s_dinputRepairSkipNoHwnd,
        s_dinputRepairSkipNotFocused);

    if (IsShellKeyTraceEnabled()) {
        LogSwallowTracePollSnapshot(reason);
    }
}

static void MaybeLogInputGuardDiagnostics(const char* reason) {
    if (!s_enableShellHotkeyImeWorkarounds && !s_enableSystemKeyWorkarounds && !IsShellKeyTraceEnabled()) {
        return;
    }

    uint32_t intervalSec = s_inputGuardDiagIntervalSec;
    if (IsShellKeyTraceEnabled() && (intervalSec == 0 || intervalSec > 5)) {
        intervalSec = 5;
    }
    if (ModConfig_VerboseLogging() || Rollback::NetplayLog_IsVerbose()) {
        if (intervalSec == 0 || intervalSec > 5) {
            intervalSec = 5;
        }
    } else if (intervalSec == 0) {
        return;
    }

    const DWORD now = GetTickCount();
    if (s_lastInputGuardDiagMs != 0 &&
        (DWORD)(now - s_lastInputGuardDiagMs) < intervalSec * 1000u) {
        return;
    }

    s_lastInputGuardDiagMs = now;
    LogInputGuardDiagnostics(reason);
}

static WINNLSEnableIME_t ResolveWinNlsEnableIme() {
    if (g_origWINNLSEnableIME) {
        return g_origWINNLSEnableIME;
    }

    HMODULE user32 = GetModuleHandleA("user32.dll");
    return reinterpret_cast<WINNLSEnableIME_t>(
        user32 ? ::GetProcAddress(user32, "WINNLSEnableIME") : nullptr);
}

static void EnsureImeEnabledForGameWindow() {
    if (!s_enableShellHotkeyImeWorkarounds) {
        return;
    }

    HWND gameWindow = GetGameWindowHandle();
    if (!IsGameWindowForeground(gameWindow)) {
        return;
    }

    // Only re-enable IME when the HWND changes or on the first call.
    // WINNLSEnableIME is an OS call — calling it every frame is wasteful.
    if (gameWindow == s_lastImeRepairHwnd) {
        return;
    }

    WINNLSEnableIME_t winNlsEnableIme = ResolveWinNlsEnableIme();
    if (!winNlsEnableIme) {
        return;
    }

    const BOOL result = winNlsEnableIme(gameWindow, TRUE);
    s_lastImeRepairHwnd = gameWindow;
    if (!s_imeRepairLogged) {
        LOG_INFO("[InputGuard] Re-enabled IME for game window hwnd=0x%p result=%d",
            gameWindow,
            result ? 1 : 0);
        s_imeRepairLogged = true;
    }
}

static bool IsCursorHidden() {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) {
        return false;
    }

    return (info.flags & CURSOR_SHOWING) == 0;
}

static void EnsureCursorReleasedAndVisible() {
    if (!s_enableShellHotkeyImeWorkarounds && !s_enableSystemKeyWorkarounds) {
        return;
    }

    // Hook_ClipCursor already blocks all non-null clip calls, so calling
    // ClipCursor(nullptr) every frame is redundant. Only call it once at
    // startup (before the hook has had a chance to fire), or skip it entirely
    // since the hook guarantees the cursor is never clipped by the game.
    if (!s_initialClipReleased) {
        if (g_origClipCursor) {
            g_origClipCursor(nullptr);
        } else {
            ::ClipCursor(nullptr);
        }
        s_initialClipReleased = true;
    }

    // The game intentionally hides the cursor while focused. Fighting ShowCursor every
    // frame corrupts the global display counter and breaks desktop mouse behavior.
    HWND gameWindow = GetGameWindowHandle();
    if (gameWindow && IsGameWindowForeground(gameWindow)) {
        return;
    }

    // Only check cursor visibility when it might actually be hidden.
    // GetCursorInfo is an OS call; skip it every frame once visibility
    // is confirmed intact.
    if (s_cursorVisibilityOk) {
        return;
    }

    if (!IsCursorHidden()) {
        s_cursorVisibilityOk = true;
        return;
    }

    int finalCount = 0;
    int repairCalls = 0;
    while (repairCalls < 16) {
        finalCount = ::ShowCursor(TRUE);
        ++repairCalls;
        if (finalCount >= 0) {
            break;
        }
    }
    s_cursorVisibilityOk = (finalCount >= 0);

    if (!s_cursorRepairLogged) {
        LOG_INFO("[InputGuard] Repaired hidden cursor display counter calls=%d final_count=%d",
            repairCalls,
            finalCount);
        s_cursorRepairLogged = true;
    }
}

static void EnsureSystemInputGuardState(const char* reason) {
    EnsureVanillaShellHotkeysEnabled();
    EnsureImeEnabledForGameWindow();
    EnsureCursorReleasedAndVisible();
    InputOverride_EnsureDInputKeyboardCooperativeLevel(reason);
    // Diagnostics are not tied to per-input-hook traffic; log from frame counter instead.
}

static void MaybeLogInputGuardDiagnosticsOnFrame() {
    if (!s_enableShellHotkeyImeWorkarounds && !s_enableSystemKeyWorkarounds) {
        return;
    }

    static int s_diagFrameCounter = 0;
    if (++s_diagFrameCounter < 3600) {
        return;
    }
    s_diagFrameCounter = 0;
    MaybeLogInputGuardDiagnostics("frame heartbeat");
}

static bool IsReservedSystemScanCode(int keyCode) {
    if (!s_enableSystemKeyWorkarounds) {
        return false;
    }
    return keyCode == 0xDD;   // DIK_APPS only (Win keys must pass through)
}

static bool ShouldLogSwallowTraceBurst() {
    if (!IsShellKeyTraceEnabled()) {
        return false;
    }

    const DWORD now = GetTickCount();
    if (s_swallowTraceStripLogCount < kSwallowTraceBurstMax) {
        return true;
    }

    if (s_swallowTraceLastStripLogMs != 0 &&
        (DWORD)(now - s_swallowTraceLastStripLogMs) < kSwallowTraceThrottleMs) {
        return false;
    }

    s_swallowTraceLastStripLogMs = now;
    return true;
}

static void LogSwallowTraceStrip(const char* source, const char* detail) {
    if (!ShouldLogSwallowTraceBurst()) {
        return;
    }

    ++s_swallowTraceStripLogCount;
    LOG_INFO("[SWALLOW-TRACE][poll] %s %s (strips=%u)",
             source ? source : "unknown",
             detail ? detail : "",
             s_swallowTraceStripLogCount);
}

static void FilterReservedSystemVirtualKeys(PBYTE keyState) {
    if (!s_enableSystemKeyWorkarounds || !keyState) {
        return;
    }

    const int blockedKeys[] = {
        VK_APPS,
    };

    for (int vk : blockedKeys) {
        if (IsShellKeyTraceEnabled() && (keyState[vk] & 0x80)) {
            char detail[64] = {};
            sprintf_s(detail, "stripped GetKeyboardState vk=0x%02X", vk);
            LogSwallowTraceStrip("GetKeyboardState", detail);
        }
        keyState[vk] = 0;
    }
}

static void FilterReservedSystemDirectInputKeys(uint8_t* keyBuffer) {
    if (!s_enableSystemKeyWorkarounds || !keyBuffer) {
        return;
    }

    const uint8_t blockedScancodes[] = {
        0xDD,
    };

    for (uint8_t scancode : blockedScancodes) {
        if (IsShellKeyTraceEnabled() && keyBuffer[scancode]) {
            char detail[64] = {};
            sprintf_s(detail,
                      "stripped byte_9D09CC[%u]=0x%02X",
                      static_cast<unsigned>(scancode),
                      keyBuffer[scancode]);
            LogSwallowTraceStrip("DInputKBRefresh", detail);
        }
        keyBuffer[scancode] = 0;
    }
}

static bool IsShellKeyTraceEnabled() {
    return s_enableSwallowTrace || s_enableHotkeyTraceLog;
}

static void FormatWindowBrief(HWND hwnd, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    if (!hwnd) {
        snprintf(out, outSize, "(null)");
        return;
    }

    wchar_t className[64] = {};
    wchar_t title[96] = {};
    GetClassNameW(hwnd, className, (int)(sizeof(className) / sizeof(className[0])));
    GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));

    char classUtf8[96] = {};
    char titleUtf8[160] = {};
    WideCharToMultiByte(CP_UTF8, 0, className, -1, classUtf8, (int)sizeof(classUtf8), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, titleUtf8, (int)sizeof(titleUtf8), nullptr, nullptr);
    snprintf(out, outSize, "0x%p cls=%s title=\"%.72s\"", hwnd, classUtf8, titleUtf8);
}

static int ReadShellSuppressFlagSafe() {
    __try {
        return *reinterpret_cast<int*>(ADDR_SHELL_HOTKEY_SUPPRESS_FLAG);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void MaybeLogShellTraceStartup() {
    static bool logged = false;
    if (logged || !IsShellKeyTraceEnabled()) {
        return;
    }
    logged = true;

    const HWND gameWindow = GetGameWindowHandle();
    WNDPROC gameWndProc = nullptr;
    if (gameWindow) {
        gameWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC));
    }

    char fgBrief[256] = {};
    FormatWindowBrief(GetForegroundWindow(), fgBrief, sizeof(fgBrief));

    LOG_INFO("[SWALLOW-TRACE] startup hotkey_trace=%d swallow_trace=%d suppress=%d game_hwnd=0x%p wndproc=0x%p fg=%s",
             s_enableHotkeyTraceLog ? 1 : 0,
             s_enableSwallowTrace ? 1 : 0,
             ReadShellSuppressFlagSafe(),
             gameWindow,
             gameWndProc,
             fgBrief);
}

static bool IsAsyncKeyDown(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

static void PollShellKeyAsyncEdges() {
    LogShellStateProvenance("PollShellKeyAsyncEdges", CaptureCallerAddress(), false);
    if (!IsShellKeyTraceEnabled()) {
        return;
    }
    MaybeLogShellTraceStartup();

    const ShellKeyAsyncSample sample = {
        IsAsyncKeyDown(VK_LWIN),
        IsAsyncKeyDown(VK_RWIN),
        IsAsyncKeyDown(VK_APPS),
        IsAsyncKeyDown(VK_MENU) || IsAsyncKeyDown(VK_LMENU) || IsAsyncKeyDown(VK_RMENU),
        IsAsyncKeyDown(VK_SHIFT) || IsAsyncKeyDown(VK_LSHIFT) || IsAsyncKeyDown(VK_RSHIFT),
        true,
    };

    if (!s_shellKeyAsyncSample.initialized) {
        s_shellKeyAsyncSample = sample;
        s_shellKeyAsyncSample.initialized = true;
        return;
    }

    const HWND gameWindow = GetGameWindowHandle();
    const HWND foreground = GetForegroundWindow();

    auto logEdge = [&](const char* keyName, bool wasDown, bool isDown) {
        if (wasDown == isDown) {
            return;
        }

        uint8_t dinputByte = 0;
        uint8_t scancode = 0;
        if (strcmp(keyName, "LWIN") == 0) {
            scancode = 0xDB;
        } else if (strcmp(keyName, "RWIN") == 0) {
            scancode = 0xDC;
        } else if (strcmp(keyName, "APPS") == 0) {
            scancode = 0xDD;
        }
        if (scancode != 0) {
            __try {
                dinputByte = reinterpret_cast<const uint8_t*>(ADDR_DINPUT_KEYBOARD)[scancode];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        char fgBrief[256] = {};
        FormatWindowBrief(foreground, fgBrief, sizeof(fgBrief));
        WNDPROC gameWndProc = nullptr;
        if (gameWindow) {
            gameWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC));
        }

        LOG_INFO("[SWALLOW-TRACE][async] %s %s game=0x%p wndproc=0x%p fgIsGame=%d suppress=%d "
                 "dinput[0x%02X]=0x%02X fg=%s",
                 keyName,
                 isDown ? "DOWN" : "UP",
                 gameWindow,
                 gameWndProc,
                 (foreground == gameWindow || (gameWindow && GetAncestor(foreground, GA_ROOT) == gameWindow))
                     ? 1
                     : 0,
                 ReadShellSuppressFlagSafe(),
                 scancode,
                 dinputByte,
                 fgBrief);
    };

    if (sample.lWin && !s_shellKeyAsyncSample.lWin) {
        EnsureVanillaShellHotkeysEnabled();
    }

    logEdge("LWIN", s_shellKeyAsyncSample.lWin, sample.lWin);
    logEdge("RWIN", s_shellKeyAsyncSample.rWin, sample.rWin);
    logEdge("APPS", s_shellKeyAsyncSample.apps, sample.apps);
    logEdge("ALT", s_shellKeyAsyncSample.alt, sample.alt);
    logEdge("SHIFT", s_shellKeyAsyncSample.shift, sample.shift);
    s_shellKeyAsyncSample = sample;
}

static void LogSwallowTracePollSnapshot(const char* reason) {
    if (!IsShellKeyTraceEnabled()) {
        return;
    }

    uint8_t dinputWin = 0;
    uint8_t dinputRWin = 0;
    uint8_t dinputApps = 0;
    __try {
        const uint8_t* keyBuffer = reinterpret_cast<const uint8_t*>(ADDR_DINPUT_KEYBOARD);
        dinputWin = keyBuffer[0xDB];
        dinputRWin = keyBuffer[0xDC];
        dinputApps = keyBuffer[0xDD];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    BYTE keyState[256] = {};
    const BOOL gksOk = GetKeyboardState(keyState);

    char fgBrief[256] = {};
    FormatWindowBrief(GetForegroundWindow(), fgBrief, sizeof(fgBrief));

    LOG_INFO("[SWALLOW-TRACE][poll] %s gks_ok=%d win{L=%d R=%d Apps=%d} alt=%d shift=%d "
             "async{L=%d R=%d Apps=%d} dinput9D09CC{L=0x%02X R=0x%02X Apps=0x%02X} "
             "strips=%u kb_poll_blocks=%u suppress=%d fg=%s",
             reason ? reason : "snapshot",
             gksOk ? 1 : 0,
             (keyState[VK_LWIN] & 0x80) ? 1 : 0,
             (keyState[VK_RWIN] & 0x80) ? 1 : 0,
             (keyState[VK_APPS] & 0x80) ? 1 : 0,
             (keyState[VK_MENU] & 0x80) ? 1 : 0,
             (keyState[VK_SHIFT] & 0x80) ? 1 : 0,
             (GetAsyncKeyState(VK_LWIN) & 0x8000) ? 1 : 0,
             (GetAsyncKeyState(VK_RWIN) & 0x8000) ? 1 : 0,
             (GetAsyncKeyState(VK_APPS) & 0x8000) ? 1 : 0,
             dinputWin,
             dinputRWin,
             dinputApps,
             s_swallowTraceStripLogCount,
             s_swallowTraceKeyboardPollCount,
             ReadShellSuppressFlagSafe(),
             fgBrief);
}

static bool IsSetMSGHookDllLookup(LPCSTR procName) {
    return procName && !IS_INTRESOURCE(procName) && std::strcmp(procName, "SetMSGHookDll") == 0;
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    LogShellStateProvenance("Hook_GetProcAddress-enter", CaptureCallerAddress(), false);
    if (!s_enableShellHotkeyImeWorkarounds) {
        return g_origGetProcAddress ? g_origGetProcAddress(hModule, lpProcName)
                                    : ::GetProcAddress(hModule, lpProcName);
    }

    if (IsSetMSGHookDllLookup(lpProcName)) {
        ClearVanillaShellHotkeySuppression();
        LogShellStateProvenance("Hook_GetProcAddress-blocked", CaptureCallerAddress(), true);
        LogVanillaShellHotkeyState("Hook_GetProcAddress blocked SetMSGHookDll");

        if (!s_shellHotkeyBlockLogged) {
            LOG_INFO("[Input] Blocked vanilla SetMSGHookDll export lookup before helper hook install");
            s_shellHotkeyBlockLogged = true;
        }
        if (IsShellKeyTraceEnabled()) {
            LOG_INFO("[SWALLOW-TRACE][hook] blocked GetProcAddress(SetMSGHookDll)");
        }

        return nullptr;
    }

    return g_origGetProcAddress(hModule, lpProcName);
}

BOOL WINAPI Hook_SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) {
    LogShellStateProvenance("Hook_SystemParametersInfoA-enter", CaptureCallerAddress(), false);
    if (!s_enableShellHotkeyImeWorkarounds) {
        return g_origSystemParametersInfoA ? g_origSystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni)
                                           : ::SystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni);
    }

    if (uiAction == 0x61u) {
        ClearVanillaShellHotkeySuppression();
        LogShellStateProvenance("Hook_SystemParametersInfoA-blocked-0x61", CaptureCallerAddress(), true);
        LogVanillaShellHotkeyState("Hook_SystemParametersInfoA blocked 0x61");

        if (!s_legacyShellHotkeyBlockLogged) {
            LOG_INFO("[Input] Blocked legacy SystemParametersInfoA(0x61) shell hotkey suppression");
            s_legacyShellHotkeyBlockLogged = true;
        }
        if (IsShellKeyTraceEnabled()) {
            LOG_INFO("[SWALLOW-TRACE][hook] blocked SystemParametersInfoA(0x61 SPI_SETFASTTASKSWITCH)");
        }

        return TRUE;
    }

    if (uiAction == 0x2000u || uiAction == 0x2001u || uiAction == 0x11u) {
        const BOOL result = g_origSystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni);
        DWORD value = pvParam ? *reinterpret_cast<DWORD*>(pvParam) : 0;
        LOG_INFO("[Input] Observed SystemParametersInfoA action=0x%X uiParam=%u pvParam=0x%p value=%lu fWinIni=0x%X result=%d",
                 uiAction,
                 uiParam,
                 pvParam,
                 value,
                 fWinIni,
                 result ? 1 : 0);
        LogVanillaShellHotkeyState("Hook_SystemParametersInfoA pass-through");
        return result;
    }

    return g_origSystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni);
}

BOOL WINAPI Hook_WINNLSEnableIME(HWND hWnd, BOOL fEnable) {
    LogShellStateProvenance("Hook_WINNLSEnableIME-enter", CaptureCallerAddress(), false);
    if (!s_enableShellHotkeyImeWorkarounds) {
        if (g_origWINNLSEnableIME) {
            return g_origWINNLSEnableIME(hWnd, fEnable);
        }

        HMODULE user32 = GetModuleHandleA("user32.dll");
        auto winNlsEnableIme = reinterpret_cast<WINNLSEnableIME_t>(
            user32 ? ::GetProcAddress(user32, "WINNLSEnableIME") : nullptr);
        return winNlsEnableIme ? winNlsEnableIme(hWnd, fEnable) : FALSE;
    }

    if (!fEnable) {
        ClearVanillaShellHotkeySuppression();
        LogShellStateProvenance("Hook_WINNLSEnableIME-blocked-false", CaptureCallerAddress(), true);
        LogVanillaShellHotkeyState("Hook_WINNLSEnableIME blocked FALSE");

        if (!s_imeDisableBlockLogged) {
            LOG_INFO("[Input] Prevented vanilla WINNLSEnableIME(FALSE) for the game window");
            s_imeDisableBlockLogged = true;
        }
        if (IsShellKeyTraceEnabled()) {
            LOG_INFO("[SWALLOW-TRACE][hook] blocked WINNLSEnableIME(FALSE) hwnd=0x%p", hWnd);
        }

        return TRUE;
    }

    return g_origWINNLSEnableIME(hWnd, fEnable);
}

BOOL WINAPI Hook_ClipCursor(const RECT* lpRect) {
    if (!lpRect) {
        return g_origClipCursor ? g_origClipCursor(nullptr) : ::ClipCursor(nullptr);
    }

    if (g_origClipCursor) {
        g_origClipCursor(nullptr);
    } else {
        ::ClipCursor(nullptr);
    }

    if (!s_mouseClipBlockLogged) {
        LOG_INFO("[Input] Blocked game cursor clipping to keep the mouse free for the rest of the desktop");
        s_mouseClipBlockLogged = true;
    }

    return TRUE;
}

// ============================================================================
// Frame-based input update tracking
// ============================================================================

static void EnsureInputUpdated() {
    EnsureSystemInputGuardState("input update");
    PollShellKeyAsyncEdges();

    int currentFrame = ReadMemory<int>(ADDR_SIM_FRAME_COUNTER);
    if (currentFrame != g_lastInputUpdateFrame) {
        InputSystem_Update();
        g_lastInputUpdateFrame = currentFrame;
        MaybeLogInputGuardDiagnosticsOnFrame();
    }
}

void InputOverride_Shutdown() {
    if (s_enableShellHotkeyImeWorkarounds && ClearVanillaShellHotkeySuppression()) {
        LOG_INFO("[Input] Exit cleanup removed vanilla shell hotkey suppression state");
    }

    g_hookCallCount = 0;
    g_lastInputUpdateFrame = -1;
    s_shellHotkeyPatchLogged = false;
    s_shellHotkeyBlockLogged = false;
    s_legacyShellHotkeyBlockLogged = false;
    s_imeDisableBlockLogged = false;
    s_shellHotkeyStateLogged = false;
    s_mouseClipBlockLogged = false;
    s_imeRepairLogged = false;
    s_lastImeRepairHwnd = nullptr;
    s_cursorRepairLogged = false;
    s_initialClipReleased = false;
    s_cursorVisibilityOk = false;
    s_dinputKeyboardWindowMissingLogged = false;
    s_lastDInputKeyboardDevice = nullptr;
    s_lastDInputKeyboardWindow = nullptr;
    s_dinputKeyboardCoopApplied = false;
    s_dinputKeyboardCoopHookLogCount = 0;
    s_dinputKeyboardCoopRepairLogCount = 0;
    s_lastDInputKeyboardCoopRepairMs = 0;
    s_lastInputGuardDiagMs = 0;
    s_shellSuppressClearCount = 0;
    s_dinputRepairAttemptCount = 0;
    s_dinputRepairSuccessCount = 0;
    s_dinputRepairSkipNoDevice = 0;
    s_dinputRepairSkipNoHwnd = 0;
    s_dinputRepairSkipNotFocused = 0;
    s_rollbackSessionWasActive = false;
    if (g_origClipCursor) {
        g_origClipCursor(nullptr);
    } else {
        ::ClipCursor(nullptr);
    }
    ZeroMemory(&g_inputDebug, sizeof(g_inputDebug));
}

static bool  s_rollbackFrameStarted = false;
static bool  s_loggedFirstBeginAfterRelease = false;
static bool  s_loggedFirstAdvanceAfterRelease = false;
static uint32_t s_startupGateLogCount = 0;

static void ResetVanillaTimeoutCounters() {
    *reinterpret_cast<volatile uint32_t*>(ADDR_HOST_TIMEOUT_CTR) = 0;
    *reinterpret_cast<volatile uint32_t*>(ADDR_CLIENT_TIMEOUT_CTR) = 0;
}

static void CommitRollbackAdvance(__int16* outputInputs, uint16_t p1, uint16_t p2) {
    if (outputInputs) {
        outputInputs[0] = (__int16)p1;
        outputInputs[1] = (__int16)p2;
    }

    volatile int32_t* pFrameWrite = reinterpret_cast<volatile int32_t*>(ADDR_INPUT_WRITE_IDX);
    const uint32_t writeIdx = (uint32_t)*pFrameWrite;
    if (writeIdx < INPUT_HISTORY_MAX) {
        *reinterpret_cast<volatile uint16_t*>(ADDR_P1_INPUT_HISTORY + (writeIdx * sizeof(uint16_t))) = p1;
        *reinterpret_cast<volatile uint16_t*>(ADDR_P2_INPUT_HISTORY + (writeIdx * sizeof(uint16_t))) = p2;
    }
    *pFrameWrite = (int32_t)(writeIdx + 1);
}

// ============================================================================
// Scancode / conversion helpers
// ============================================================================

uint16_t ScanCodeToInputFlag(int scancode) {
    switch (scancode) {
        case 0xC8: return INPUT_UP;
        case 0xD0: return INPUT_DOWN;
        case 0xCB: return INPUT_LEFT;
        case 0xCD: return INPUT_RIGHT;
        case 72:   return INPUT_UP;
        case 80:   return INPUT_DOWN;
        case 75:   return INPUT_LEFT;
        case 77:   return INPUT_RIGHT;
        case 0x2C: return INPUT_A;
        case 0x2D: return INPUT_B;
        case 0x2E: return INPUT_C;
        case 0x1E: return INPUT_D;
        case 0x1F: return INPUT_L1;
        case 0x20: return INPUT_R1;
        case 0x10: return INPUT_L2;
        case 0x11: return INPUT_R2;
        case 0x01: return INPUT_SELECT;
        case 0x1C: return INPUT_START;
        case 0x39: return INPUT_SELECT;
        case 0x0E: return INPUT_SELECT;
        default:   return 0;
    }
}

uint16_t ConvertToGameJoyFormat(uint16_t input) {
    uint16_t result = 0;
    if (input & INPUT_UP)    result |= 0x0008;
    if (input & INPUT_DOWN)  result |= 0x0001;
    if (input & INPUT_LEFT)  result |= 0x0002;
    if (input & INPUT_RIGHT) result |= 0x0004;
    result |= (input & 0xFFF0);
    return result;
}

uint8_t SDLScancodeToDIK(int sdlScancode) {
    switch (sdlScancode) {
        case 4:  return 0x1E;  case 5:  return 0x30;  case 6:  return 0x2E;
        case 7:  return 0x20;  case 8:  return 0x12;  case 9:  return 0x21;
        case 10: return 0x22;  case 11: return 0x23;  case 12: return 0x17;
        case 13: return 0x24;  case 14: return 0x25;  case 15: return 0x26;
        case 16: return 0x32;  case 17: return 0x31;  case 18: return 0x18;
        case 19: return 0x19;  case 20: return 0x10;  case 21: return 0x13;
        case 22: return 0x1F;  case 23: return 0x14;  case 24: return 0x16;
        case 25: return 0x2F;  case 26: return 0x11;  case 27: return 0x2D;
        case 28: return 0x15;  case 29: return 0x2C;
        case 30: return 0x02;  case 31: return 0x03;  case 32: return 0x04;
        case 33: return 0x05;  case 34: return 0x06;  case 35: return 0x07;
        case 36: return 0x08;  case 37: return 0x09;  case 38: return 0x0A;
        case 39: return 0x0B;
        case 40: return 0x1C;  case 41: return 0x01;  case 42: return 0x0E;
        case 43: return 0x0F;  case 44: return 0x39;
        case 79: return 0xCD;  case 80: return 0xCB;  case 81: return 0xD0;
        case 82: return 0xC8;
        case 89: return 0x4F;  case 90: return 0x50;  case 91: return 0x51;
        case 92: return 0x4B;  case 93: return 0x4C;  case 94: return 0x4D;
        case 95: return 0x47;  case 96: return 0x48;  case 97: return 0x49;
        case 98: return 0x52;  case 88: return 0x9C;
        default: return 0;
    }
}

// ============================================================================
// Keyboard input injection (SDL → DInput buffer)
// ============================================================================

static void InjectKeyboardInput() {
    if (!ModConfig_UseSDLInput()) return;
    
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    const PlayerBindings_t* bindings = InputSystem_GetBindings(0);
    if (!bindings) return;
    
    uint16_t input = InputSystem_GetInput(0);
    
    auto injectKey = [&](const KeyBinding_t* binding, uint16_t flag) {
        if (binding->keyboard_key > 0 && (input & flag)) {
            uint8_t dik = SDLScancodeToDIK(binding->keyboard_key);
            if (dik > 0) {
                keyBuffer[dik] |= 0x80;
            }
        }
    };
    
    injectKey(&bindings->up,    INPUT_UP);
    injectKey(&bindings->down,  INPUT_DOWN);
    injectKey(&bindings->left,  INPUT_LEFT);
    injectKey(&bindings->right, INPUT_RIGHT);
    injectKey(&bindings->a,     INPUT_A);
    injectKey(&bindings->b,     INPUT_B);
    injectKey(&bindings->c,     INPUT_C);
    injectKey(&bindings->d,     INPUT_D);
    injectKey(&bindings->start, INPUT_START);
    injectKey(&bindings->select,INPUT_SELECT);
    injectKey(&bindings->l1,    INPUT_L1);
    injectKey(&bindings->r1,    INPUT_R1);
}

// ============================================================================
// Window focus helper
// ============================================================================

static bool IsGameWindowFocused() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return (foregroundPid == GetCurrentProcessId());
}

// ============================================================================
// Hook: Win32 GetKeyboardState
// ============================================================================

BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    if ((InputSystem_IsBindingActive() || NetMenu::ConsumesGameInput()) && lpKeyState) {
        memset(lpKeyState, 0, 256);
        return TRUE;
    }

    if (ModConfig_UseSDLInput() && lpKeyState) {
        if (g_origGetKeyboardState) {
            g_origGetKeyboardState(lpKeyState);
        } else {
            memset(lpKeyState, 0, 256);
        }

        EnsureInputUpdated();

        const uint16_t input = InputSystem_GetInput(0);
        auto setPressed = [&](int vk, bool pressed) {
            if (vk < 0 || vk >= 256) return;
            lpKeyState[vk] = pressed ? 0x80 : 0x00;
        };

        // Preserve the real system modifier state (Win/Alt/Shift/Ctrl, input
        // language toggles, etc.) and only rewrite the keys the game uses for
        // its default keyboard action map.
        setPressed(VK_UP,    (input & INPUT_UP) != 0);
        setPressed(VK_DOWN,  (input & INPUT_DOWN) != 0);
        setPressed(VK_LEFT,  (input & INPUT_LEFT) != 0);
        setPressed(VK_RIGHT, (input & INPUT_RIGHT) != 0);
        setPressed('Z', (input & INPUT_A) != 0);
        setPressed('X', (input & INPUT_B) != 0);
        setPressed('C', (input & INPUT_C) != 0);
        setPressed('A', (input & INPUT_D) != 0);
        setPressed('S', (input & INPUT_L1) != 0);
        setPressed('D', (input & INPUT_R1) != 0);
        setPressed('Q', (input & INPUT_L2) != 0);
        setPressed('W', (input & INPUT_R2) != 0);
        setPressed(VK_RETURN, (input & INPUT_START) != 0);
        setPressed(VK_ESCAPE, (input & INPUT_SELECT) != 0);
        setPressed(VK_SPACE,  (input & INPUT_SELECT) != 0);
        setPressed(VK_BACK,   (input & INPUT_SELECT) != 0);

        FilterReservedSystemVirtualKeys(lpKeyState);

        return TRUE;
    }

    const BOOL result = g_origGetKeyboardState(lpKeyState);
    FilterReservedSystemVirtualKeys(lpKeyState);
    return result;
}

// ============================================================================
// Hook: DInput keyboard buffer refresh (sub_630130)
// ============================================================================

int __cdecl Hook_DInputKBRefresh() {
    EnsureInputUpdated();

    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    
    if (ModConfig_UseSDLInput()) {
        memset(keyBuffer, 0, 256);
        FilterReservedSystemDirectInputKeys(keyBuffer);
        return 0;
    }

    const int result = g_origDInputKBRefresh();
    FilterReservedSystemDirectInputKeys(keyBuffer);
    return result;
}

// ============================================================================
// Hook: DInput joystick buffer refresh (sub_6302F0)
// ============================================================================

int __cdecl Hook_DInputJoyRefresh(int joyID) {
    EnsureInputUpdated();
    
    if (ModConfig_UseSDLInput()) {
        const int result = g_origDInputJoyRefresh ? g_origDInputJoyRefresh(joyID) : 0;

        if (joyID >= 0 && joyID < DINPUT_JOY_MAX) {
            ClearVanillaDInputJoyState(joyID, "SDL owns game input");
        } else if (s_dinputJoyRefreshLogCount < 24) {
            LOG_INFO("[InputHook] DInputJoyRefresh received unexpected joyID=%d under SDL input; result=%d",
                     joyID,
                     result);
            s_dinputJoyRefreshLogCount++;
        }

        return result;
    }
    
    int result = g_origDInputJoyRefresh(joyID);
    
    if (joyID & 0x1000) {
        return result;
    }
    
    int joyIndex = (joyID & 0xFFF) - 1;
    if (joyIndex < 0 || joyIndex >= DINPUT_JOY_MAX) {
        return result;
    }
    
    static const uintptr_t P1_JOY_ID_ADDR = 0x816358 + 864440;
    static const uintptr_t P2_JOY_ID_ADDR = 0x816358 + 864484;
    int p1JoyID = ReadMemory<int>(P1_JOY_ID_ADDR);
    int p2JoyID = ReadMemory<int>(P2_JOY_ID_ADDR);
    
    int playerIndex = -1;
    if (joyID == p1JoyID) playerIndex = 0;
    else if (joyID == p2JoyID) playerIndex = 1;
    
    if (playerIndex < 0) {
        return result;
    }
    
    return result;
}

// ============================================================================
// Hook: Keyboard state (sub_62FD00)
// ============================================================================

int __cdecl Hook_KeyboardState(int keyCode) {
    g_hookCallCount++;
    g_inputDebug.keyboardHookCalls++;
    g_inputDebug.lastKeyCode = keyCode;
    
    EnsureInputUpdated();

    if (IsReservedSystemScanCode(keyCode)) {
        if (IsShellKeyTraceEnabled() && ShouldLogSwallowTraceBurst()) {
            ++s_swallowTraceKeyboardPollCount;
            LOG_INFO("[SWALLOW-TRACE][poll] sub_62FD00 blocked scancode=0x%02X (win/apps) blocks=%u",
                     keyCode,
                     s_swallowTraceKeyboardPollCount);
        }
        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = 0;
        return 0;
    }

    if (InputSystem_IsBindingActive() || NetMenu::ConsumesGameInput()) {
        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = 0;
        return 0;
    }
    
    if (ModConfig_UseSDLInput()) {
        const uint16_t flag = ScanCodeToInputFlag(keyCode);
        const uint16_t input = InputSystem_GetInput(0);
        const int pressed = (flag != 0 && (input & flag) != 0) ? 1 : 0;

        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = pressed;

        if (pressed) {
            g_inputDebug.keyboardInjectedCount++;
            g_inputDebug.lastInjectedKeyInput = flag;
        }
        return pressed;
    }
    
    int origResult = g_origKeyboardState(keyCode);
    g_inputDebug.lastOrigResult = origResult;
    g_inputDebug.lastFinalResult = origResult;
    return origResult;
}

// ============================================================================
// Hook: Joystick state (sub_62FF50)
// ============================================================================

int __cdecl Hook_JoystickState(int playerID) {
    g_hookCallCount++;
    g_inputDebug.joystickHookCalls++;
    g_inputDebug.lastPlayerID = playerID;
    
    EnsureInputUpdated();

    if (InputSystem_IsBindingActive()) {
        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = 0;
        return 0;
    }
    
    if (ModConfig_UseSDLInput()) {
        g_inputDebug.lastOrigResult = 0;
        
        static const uintptr_t GAME_CONFIG_BASE = 0x816358;
        static const uintptr_t P1_JOY_ID_ADDR = GAME_CONFIG_BASE + 864440;
        static const uintptr_t P2_JOY_ID_ADDR = GAME_CONFIG_BASE + 864484;
        
        int p1JoyID = ReadMemory<int>(P1_JOY_ID_ADDR);
        int p2JoyID = ReadMemory<int>(P2_JOY_ID_ADDR);
        int baseJoyID = playerID & ~0x1000;
        g_inputDebug.p1JoyID = p1JoyID;
        g_inputDebug.p2JoyID = p2JoyID;
        
        int playerIndex = -1;
        if (baseJoyID == p1JoyID) {
            playerIndex = 0;
        } else if (baseJoyID == p2JoyID) {
            playerIndex = 1;
        } else if ((p1JoyID <= 0 || p1JoyID > DINPUT_JOY_MAX) && baseJoyID == 1) {
            playerIndex = 0;
        } else if ((p2JoyID <= 0 || p2JoyID > DINPUT_JOY_MAX) && baseJoyID == 2) {
            playerIndex = 1;
        }
        g_inputDebug.lastMappedPlayer = playerIndex;

        if (baseJoyID >= 1 && baseJoyID <= DINPUT_JOY_MAX) {
            ClearVanillaDInputJoyState(baseJoyID - 1, "JoystickState SDL override");
        }

        if (playerIndex < 0) {
            g_inputDebug.lastFinalResult = 0;
            const bool verboseInputLog = ModConfig_VerboseLogging() || Rollback::NetplayLog_IsVerbose();
            if ((verboseInputLog && s_joystickUnknownLogCount < 16) ||
                s_joystickUnknownLogCount < 4) {
                LOG_INFO("[InputHook] JoystickState SDL ignored unmapped playerID=%d baseJoyID=%d p1JoyID=%d p2JoyID=%d",
                         playerID,
                         baseJoyID,
                         p1JoyID,
                         p2JoyID);
                s_joystickUnknownLogCount++;
            }
            return 0;
        }
        
        // During netplay: suppress local P2 hardware input entirely.
        // P2 is controlled by the remote peer (charsel_sync / rollback input).
        if (playerIndex == 1 && Net::Session_IsConnected()) {
            g_inputDebug.sdlInputP2 = 0;
            g_inputDebug.gameInputP2 = 0;
            g_inputDebug.lastFinalResult = 0;
            return 0;
        }

        uint16_t sdlInput = InputSystem_GetInput(playerIndex);
        uint16_t gameInput = ConvertToGameJoyFormat(sdlInput);
        
        if (playerIndex == 0) {
            g_inputDebug.sdlInputP1 = sdlInput;
            g_inputDebug.gameInputP1 = gameInput;
        } else {
            g_inputDebug.sdlInputP2 = sdlInput;
            g_inputDebug.gameInputP2 = gameInput;
        }
        
        if (gameInput != 0) {
            g_inputDebug.joystickInjectedCount++;
            g_inputDebug.lastInjectedJoyInput = gameInput;
        }
        
        const bool verboseInputLog = ModConfig_VerboseLogging() || Rollback::NetplayLog_IsVerbose();
        if ((verboseInputLog && s_joystickStateLogCount < 24) ||
            s_joystickStateLogCount < 4) {
            LOG_INFO("[InputHook] JoystickState SDL playerID=%d baseJoyID=%d p1JoyID=%d p2JoyID=%d mappedP%d sdl=0x%04X game=0x%04X",
                     playerID,
                     baseJoyID,
                     p1JoyID,
                     p2JoyID,
                     playerIndex + 1,
                     sdlInput,
                     gameInput);
            s_joystickStateLogCount++;
        }

        g_inputDebug.lastFinalResult = (int)gameInput;
        return (int)gameInput;
    }
    
    int origResult = g_origJoystickState(playerID);
    g_inputDebug.lastOrigResult = origResult;
    g_inputDebug.lastFinalResult = origResult;
    return origResult;
}

// ============================================================================
// Input Dispatcher Hook (sub_5625E0 / Input_TryGetNextFrame)
//
// During charsel lockstep: replaces vanilla input dispatch with lockstep data.
// Both peers see identical P1/P2 inputs → identical charsel navigation.
// ============================================================================

// Shared constant: just-pressed starts at word offset 28 in the input buffer.
#define JUST_PRESSED_OFFSET_WORDS 28

// Edge detection state for raw array overwrite
static uint16_t s_dispPrevP1 = 0;
static uint16_t s_dispPrevP2 = 0;
static uint16_t s_stageCancelPrevMerged = 0;

// Logging throttle
static uint32_t s_dispatchCount = 0;
static uint32_t s_dispatchWaitCount = 0;
static bool     s_dispatchFirstLog = false;

// Charsel: prevent producing more than one input per game-loop iteration.
// The game calls the dispatcher in a while-loop; we return 0 once (produce
// a frame) then -1 to break out. This flag resets when -1 is returned.
static bool s_charsel_produced_this_loop = false;

// WinScreen input is driven from Hook_InputProcess because Mode 9 does not
// enter the dispatcher loop in the reproduced rematch path.
static uint16_t s_winscreenPrevP1 = 0;
static uint16_t s_winscreenPrevP2 = 0;
static uint32_t s_winscreenDispatchCount = 0;
static uint32_t s_winscreenWaitCount = 0;
static bool s_winscreenFirstLog = false;
static bool s_winscreenFrameProduced = false;
static uint16_t s_winscreenFrameP1 = 0;
static uint16_t s_winscreenFrameP2 = 0;
static uint16_t s_winscreenFrameJustP1 = 0;
static uint16_t s_winscreenFrameJustP2 = 0;
static uint32_t s_winscreenLastProcessFrame = 0xFFFFFFFFu;
static bool s_spectatorDispatchActive = false;
static uint16_t s_spectatorPrevP1 = 0;
static uint16_t s_spectatorPrevP2 = 0;
static uint32_t s_spectatorDispatchCount = 0;

int __cdecl Hook_InputDispatcher(__int16* outputInputs) {
    if (!outputInputs) return -1;

    const uint32_t gameMode = GetGameMode();
    const uint32_t subState = GetSubstate();

    // ── Load barrier freeze ─────────────────────────────────────────
    // Game reached match loading but bootstrap hasn't completed.
    // Freeze gameplay (return -1) while keeping game loop alive for
    // SessionManager updates, packet exchange, and ImGui rendering.
    if (InputSyncHooks_IsGameplayFreezeActive()) {
        const bool practiceFreeze = PracticeTools_ShouldFreezeFrame();
        const bool replayFreeze = Replay::ReplayRuntime_ShouldFreezeFrame();
        const bool charselLockstep = Net::CharSelSync_IsLockstepActive();
        const bool loadBarrierFreeze = InputSyncHooks_IsLoadBarrierFrozen();
        const bool timesyncFreeze = InputSyncHooks_IsTimesyncFrozen();
        const bool rollbackOwnsGameplay = Rollback::RollbackSession_IsActive();
        const bool replayOwnsGameplay = Replay::ReplayRuntime_IsReplayMatchActive();
        const bool startupBarrierOwnsGameplay =
            Rollback::OnlineWiring_IsGameplayEntryAdvanceBlocked();
        const bool pregameOwnsLoadBarrier = Net::PregameSync_IsActive();
        const bool legitimateFreeze =
            practiceFreeze ||
            (replayFreeze && replayOwnsGameplay) ||
            charselLockstep ||
            (loadBarrierFreeze && pregameOwnsLoadBarrier) ||
            (timesyncFreeze && (rollbackOwnsGameplay || startupBarrierOwnsGameplay));

        if (!legitimateFreeze) {
            Rollback::NetplayLog_Write("SYNC", -1,
                "Clearing stale gameplay freeze in non-owned path: mode=%u sub=%u "
                "practice=%d charsel=%d load=%d timesync=%d pregame=%d "
                "replay=%d session_connected=%d rollback_active=%d startup_blocked=%d",
                gameMode,
                subState,
                practiceFreeze ? 1 : 0,
                charselLockstep ? 1 : 0,
                loadBarrierFreeze ? 1 : 0,
                timesyncFreeze ? 1 : 0,
                pregameOwnsLoadBarrier ? 1 : 0,
                replayFreeze ? 1 : 0,
                Net::Session_IsConnected() ? 1 : 0,
                rollbackOwnsGameplay ? 1 : 0,
                startupBarrierOwnsGameplay ? 1 : 0);
            InputSyncHooks_SetLoadBarrierFreeze(false);
            InputSyncHooks_SetTimesyncFreeze(false);
        } else {
            return -1;
        }
    }

    // LaunchNetplayCharSel enters Mode 6 before the announce/confirm
    // handshake has promoted pregame into frontend lockstep. Hold the live
    // selection substates inert so an early-arriving peer cannot move or
    // confirm locally before CharSelSync takes ownership.
    if (ShouldHoldCharSelUntilLockstep(gameMode, subState)) {
        static uint32_t s_charselHoldCount = 0;
        s_charselHoldCount++;
        if (s_charselHoldCount <= 5 || (s_charselHoldCount % 120) == 0) {
            Rollback::NetplayLog_Write("INPUT", -1,
                "CharSel HOLD (#%u): phase=%s mode=%u sub=%u",
                s_charselHoldCount,
                Net::PregamePhaseName(Net::PregameSync_GetPhase()),
                gameMode,
                subState);
            Rollback::NetplayLog_Flush();
        }
        return -1;
    }

    // Diagnostic: log every dispatcher call when charsel lockstep is active
    if (Net::CharSelSync_IsLockstepActive()) {
        static uint32_t s_diagCount = 0;
        s_diagCount++;
        if (s_diagCount <= 5) {
            Rollback::NetplayLog_Write("INPUT", -1,
                "Dispatcher ENTRY (lockstep active): call#%u mode=%u sub=%u produced=%d",
                s_diagCount, GetGameMode(), GetSubstate(), (int)s_charsel_produced_this_loop);
            Rollback::NetplayLog_Flush();
        }
    }

    // ── CharSel lockstep ────────────────────────────────────────────
    // Replaces vanilla dispatch with deterministic lockstep.
    // Both sides exchange inputs frame-by-frame. Game only advances
    // when BOTH local and remote inputs are available.
    if (Net::CharSelSync_IsLockstepActive()) {
        // Only run lockstep during active selection substates
        if (gameMode != MODE_CHARSEL || !IsCharSelDispatcherLockstepSubstate(subState)) {
            // Not in a lockstep substate — freeze and suppress vanilla
            Rollback::NetplayLog_Write("INPUT", -1,
                "Lockstep FREEZE (non-lockstep substate): mode=%u sub=%u", gameMode, subState);
            Rollback::NetplayLog_Flush();
            return -1;
        }

        // Log first intercept
        if (!s_dispatchFirstLog) {
            s_dispatchFirstLog = true;
            s_dispatchCount = 0;
            s_dispatchWaitCount = 0;
            s_charsel_produced_this_loop = false;

            // Reset edge detection so first frame doesn't ghost-press everything
            s_dispPrevP1 = 0;
            s_dispPrevP2 = 0;

            // Snapshot all frame counters for diagnostics
            int32_t frameSim   = *(volatile int32_t*)ADDR_FRAME_SIMULATION;
            int32_t frameDisp  = *(volatile int32_t*)ADDR_FRAME_DISPLAY;
            int32_t frameWrite = *(volatile int32_t*)ADDR_INPUT_WRITE_IDX;
            int32_t frameNet   = *(volatile int32_t*)ADDR_FRAME_NET_IDX;
            uint32_t gameType  = GetGameType();

            LOG_NETPLAY(LOG_INFO, "[InputDispatch] Lockstep intercept ACTIVE "
                "(mode=%u sub=%u type=%u frameSim=%d frameDisp=%d frameWrite=%d frameNet=%d)",
                gameMode, subState, gameType, frameSim, frameDisp, frameWrite, frameNet);
            Rollback::NetplayLog_Write("INPUT", -1,
                "Lockstep intercept ACTIVE: mode=%u sub=%u type=%u "
                "frameSim=%d frameDisp=%d frameWrite=%d frameNet=%d",
                gameMode, subState, gameType, frameSim, frameDisp, frameWrite, frameNet);
            Rollback::NetplayLog_Flush();
        }

        // Frame gate: only produce one frame per game-loop iteration.
        // The while-loop calls us repeatedly; after producing one frame,
        // return -1 to break out. Reset when we're called again next iteration.
        if (s_charsel_produced_this_loop) {
            s_charsel_produced_this_loop = false;  // Reset for next iteration
            Rollback::NetplayLog_Write("INPUT", -1,
                "Frame gate: produced_this_loop reset, returning -1 (frame#%u)", s_dispatchCount);
            Rollback::NetplayLog_Flush();
            return -1;
        }

        if (!Net::CharSelPaletteSelect_IsCatalogReady()) {
            Rollback::NetplayLog_Write("INPUT", -1,
                "Step 0: Waiting for palette catalog sync before charsel lockstep advances");
            Rollback::NetplayLog_Flush();
            return -1;
        }

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 1: SDL poll + CaptureLocalInput (frame#%u mode=%u sub=%u)",
            s_dispatchCount, gameMode, subState);
        Rollback::NetplayLog_Flush();

        // Poll SDL input and get local packed input
        InputSystem_Update();
        uint16_t localInput = Net::PlayerMapping_ReadLocalInput();

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 2: localInput=0x%04X, calling CaptureLocalInput", localInput);
        Rollback::NetplayLog_Flush();

        // Buffer locally and send to peer (with redundant history)
        Net::CharSelSync_CaptureLocalInput(localInput);

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 3: CaptureLocalInput done, checking HasInputsForCurrentFrame");
        Rollback::NetplayLog_Flush();

        // Lockstep gate: only advance when both inputs are available
        if (!Net::CharSelSync_HasInputsForCurrentFrame()) {
            s_dispatchWaitCount++;
            Rollback::NetplayLog_Write("INPUT", -1,
                "Step 3a: Waiting for remote input (wait#%u)", s_dispatchWaitCount);
            Rollback::NetplayLog_Flush();
            return -1;  // Freeze — wait for remote input
        }

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 4: Both inputs ready, calling ConsumeCurrentFrame");
        Rollback::NetplayLog_Flush();

        // Consume confirmed inputs for this frame
        uint16_t p1 = 0, p2 = 0;
        if (!Net::CharSelSync_ConsumeCurrentFrame(&p1, &p2)) {
            LOG_NETPLAY(LOG_WARNING, "[InputDispatch] Lockstep inconsistency: inputs ready but consume failed");
            Rollback::NetplayLog_Write("INPUT", -1,
                "Step 4a: CONSUME FAILED");
            Rollback::NetplayLog_Flush();
            return -1;
        }

        s_dispatchCount++;
        s_dispatchWaitCount = 0;

        {
            int32_t frameSim   = *(volatile int32_t*)ADDR_FRAME_SIMULATION;
            int32_t frameDisp  = *(volatile int32_t*)ADDR_FRAME_DISPLAY;
            int32_t frameWrite = *(volatile int32_t*)ADDR_INPUT_WRITE_IDX;
            Rollback::NetplayLog_Write("INPUT", -1,
                "Step 5: Consumed frame#%u P1=0x%04X P2=0x%04X sub=%u "
                "frameSim=%d frameDisp=%d frameWrite=%d",
                s_dispatchCount, p1, p2, subState, frameSim, frameDisp, frameWrite);
            Rollback::NetplayLog_Flush();
        }

        // Edge detection for raw array just-pressed
        const uint16_t rawP1 = p1;
        const uint16_t rawP2 = p2;
        uint16_t justP1 = rawP1 & ~s_dispPrevP1;
        uint16_t justP2 = rawP2 & ~s_dispPrevP2;
        s_dispPrevP1 = rawP1;
        s_dispPrevP2 = rawP2;

        const uint8_t canceledSlots = Net::CharSelSync_HandleCharacterCancelInput(justP1, justP2);
        const uint8_t randomSlots = Net::CharSelSelectActions_HandleNetplayCharacterRandomInput(
            (canceledSlots & 0x01) ? (uint16_t)0 : justP1,
            (canceledSlots & 0x02) ? (uint16_t)0 : justP2,
            rawP1,
            rawP2);
        const bool p1RandomActive = Net::CharSelPaletteSelect_IsRandomCharacterActive(0);
        const bool p2RandomActive = Net::CharSelPaletteSelect_IsRandomCharacterActive(1);
        if ((canceledSlots & 0x01) != 0) {
            p1 = 0;
            justP1 = 0;
        } else if ((randomSlots & 0x01) != 0 || p1RandomActive) {
            p1 = 0;
            justP1 = 0;
        } else {
            p1 &= (uint16_t)~INPUT_B;
            justP1 &= (uint16_t)~INPUT_B;
            p1 &= (uint16_t)~INPUT_D;
            justP1 &= (uint16_t)~INPUT_D;
        }
        if ((canceledSlots & 0x02) != 0) {
            p2 = 0;
            justP2 = 0;
        } else if ((randomSlots & 0x02) != 0 || p2RandomActive) {
            p2 = 0;
            justP2 = 0;
        } else {
            p2 &= (uint16_t)~INPUT_B;
            justP2 &= (uint16_t)~INPUT_B;
            p2 &= (uint16_t)~INPUT_D;
            justP2 &= (uint16_t)~INPUT_D;
        }

        // Write to output array after applying frontend-only B handling.
        outputInputs[0] = (__int16)p1;
        outputInputs[1] = (__int16)p2;

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 6: Wrote outputInputs[0]=0x%04X [1]=0x%04X canceled_slots=0x%02X random_slots=0x%02X random_active=%u/%u",
            (uint16_t)outputInputs[0],
            (uint16_t)outputInputs[1],
            canceledSlots,
            randomSlots,
            p1RandomActive ? 1 : 0,
            p2RandomActive ? 1 : 0);
        Rollback::NetplayLog_Flush();

        // Advance Frame_Inputs (vanilla dispatcher does this)
        volatile int32_t* pFrameWrite = reinterpret_cast<volatile int32_t*>(ADDR_INPUT_WRITE_IDX);
        (*pFrameWrite)++;

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 7: Frame_Inputs incremented to %d", *pFrameWrite);
        Rollback::NetplayLog_Flush();

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 8: Edge detect: rawP1=0x%04X rawP2=0x%04X justP1=0x%04X justP2=0x%04X prevP1=0x%04X prevP2=0x%04X",
            rawP1, rawP2, justP1, justP2, s_dispPrevP1, s_dispPrevP2);
        Rollback::NetplayLog_Flush();

        // Overwrite P1/P2 raw input arrays (held + just-pressed).
        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 9: Writing raw input buffers P1@0x%08X P2@0x%08X",
            ADDR_P1_INPUT_BUFFER, ADDR_P2_INPUT_BUFFER);
        Rollback::NetplayLog_Flush();

        __try {
            for (int i = 0; i < 10; i++) {
                uint16_t mask = (uint16_t)(1 << i);
                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (i * 2),
                    (uint16_t)((p1 & mask) ? 1 : 0));
                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                    (uint16_t)((justP1 & mask) ? 1 : 0));
                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (i * 2),
                    (uint16_t)((p2 & mask) ? 1 : 0));
                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                    (uint16_t)((justP2 & mask) ? 1 : 0));
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LOG_NETPLAY(LOG_ERROR, "[InputDispatch] EXCEPTION writing raw input buffers! "
                "P1=0x%04X P2=0x%04X frame#%u sub=%u",
                p1, p2, s_dispatchCount, subState);
            Rollback::NetplayLog_Write("INPUT", -1,
                "EXCEPTION writing raw input buffers! P1=0x%04X P2=0x%04X frame#%u",
                p1, p2, s_dispatchCount);
            Rollback::NetplayLog_Flush();
        }

        Rollback::NetplayLog_Write("INPUT", -1,
            "Step 10: Raw buffers written OK, setting produced=true, returning 0");
        Rollback::NetplayLog_Flush();

        s_charsel_produced_this_loop = true;
        return 0;
    }

    {
        uint16_t p1 = 0;
        uint16_t p2 = 0;
        int32_t rbFrame = -1;
        const Net::SpectatorDispatchAction spectatorAction =
            Net::SpectatorPlayback_GetDispatcherFrame(&p1, &p2, &rbFrame);
        if (spectatorAction == Net::SpectatorDispatchAction::ProduceFrame) {
            s_spectatorDispatchActive = true;
            s_spectatorDispatchCount++;

            outputInputs[0] = (__int16)p1;
            outputInputs[1] = (__int16)p2;

            volatile int32_t* pFrameWrite = reinterpret_cast<volatile int32_t*>(ADDR_INPUT_WRITE_IDX);
            const uint32_t writeIdx = (uint32_t)*pFrameWrite;
            if (writeIdx < INPUT_HISTORY_MAX) {
                *reinterpret_cast<volatile uint16_t*>(ADDR_P1_INPUT_HISTORY + (writeIdx * sizeof(uint16_t))) = p1;
                *reinterpret_cast<volatile uint16_t*>(ADDR_P2_INPUT_HISTORY + (writeIdx * sizeof(uint16_t))) = p2;
            }
            *pFrameWrite = (int32_t)(writeIdx + 1);

            const uint16_t justP1 = p1 & (uint16_t)~s_spectatorPrevP1;
            const uint16_t justP2 = p2 & (uint16_t)~s_spectatorPrevP2;
            s_spectatorPrevP1 = p1;
            s_spectatorPrevP2 = p2;

            __try {
                for (int i = 0; i < 10; i++) {
                    const uint16_t mask = (uint16_t)(1 << i);
                    WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (i * 2),
                        (uint16_t)((p1 & mask) ? 1 : 0));
                    WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                        (uint16_t)((justP1 & mask) ? 1 : 0));
                    WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (i * 2),
                        (uint16_t)((p2 & mask) ? 1 : 0));
                    WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                        (uint16_t)((justP2 & mask) ? 1 : 0));
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                LOG_NETPLAY(LOG_ERROR,
                    "[InputDispatch] EXCEPTION writing spectator raw input buffers rb=%d P1=0x%04X P2=0x%04X",
                    rbFrame,
                    p1,
                    p2);
                return -1;
            }

            if (s_spectatorDispatchCount <= 5 || (s_spectatorDispatchCount % 120) == 0) {
                Rollback::NetplayLog_Write("SPLAY", rbFrame,
                    "Dispatcher advance rb=%d P1=0x%04X P2=0x%04X writeIdx=%u->%u",
                    rbFrame,
                    p1,
                    p2,
                    writeIdx,
                    writeIdx + 1);
            }
            return 0;
        }

        if (spectatorAction == Net::SpectatorDispatchAction::BreakLoop) {
            return -1;
        }

        if (s_spectatorDispatchActive) {
            s_spectatorDispatchActive = false;
            s_spectatorPrevP1 = 0;
            s_spectatorPrevP2 = 0;
            s_spectatorDispatchCount = 0;
        }
    }

    // Pre-live interactive boundary gate:
    // Hold the first post-intro interactive frame until startup release and
    // rollback session activation are both complete. Keep session pumping alive.
    {
        static uint32_t s_preLiveStartupGateLogCount = 0;
        if (!Rollback::RollbackSession_IsActive() &&
            Rollback::OnlineWiring_IsGameplayEntryAdvanceBlocked()) {
            s_preLiveStartupGateLogCount++;
            if (s_preLiveStartupGateLogCount <= 5 ||
                (s_preLiveStartupGateLogCount % 120) == 0) {
                Rollback::NetplayLog_Write("STARTUP",
                    (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER),
                    "Session connected but live gameplay advance is GATED at first interactive boundary "
                    "(rollback not active yet): phase=%s released=%d",
                     Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                     Rollback::OnlineWiring_IsStartupReleased() ? 1 : 0);
            }

            InputSyncHooks_SetTimesyncFreeze(true);
            Net::Session_Update();
            if (!Net::Session_IsConnected()) {
                InputSyncHooks_SetTimesyncFreeze(false);
                return AbortRollbackDispatcher("Peer disconnected during startup interactive barrier");
            }
            return -1;
        }
        s_preLiveStartupGateLogCount = 0;
        InputSyncHooks_SetTimesyncFreeze(false);
    }

    // ── GekkoNet rollback session ───────────────────────────────────
    // Two-phase event processing: BeginFrame once, then ProcessNextEvent
    // until all events are consumed. Each AdvanceEvent = one game frame.
#if 0
    if (Rollback::RollbackSession_IsActive()) {
        // GekkoNet frames_ahead semantics:
        //   positive => local is AHEAD (prediction pressure increasing)
        //   negative => local is BEHIND (local can catch up)
        static int s_catchupCooldown = 0;
        static int s_aheadThrottleCooldown = 0;
        static bool s_doDoubleTick = false;
        static bool s_doAheadThrottleHold = false;
        static bool s_loggedTimesyncEnabled = false;
        static bool s_liveTimesyncWasEnabled = false;
        static bool s_timesyncCatchupArmed = false;
        static bool s_lastDispatcherCallAdvanced = false;
        static bool s_lastAdvanceWasRollback = false;
        static uint32_t s_postReleaseNormalAdvanceCount = 0;
        static DWORD s_liveTimesyncEnabledAtMs = 0;
        static bool s_loggedFirstBeginAfterRelease = false;
        static bool s_loggedFirstAdvanceAfterRelease = false;
        static bool s_startupBiasActive = false;
        static float s_startupBias = 0.0f;
        static int32_t s_lastCatchupDecisionFrame = -1000000;
        static int32_t s_lastAheadDecisionFrame = -1000000;
        static int32_t s_lastPacingDecisionFrame = -1000000;
        static bool s_aheadThrottleActive = false;
        static uint32_t s_aheadOverEnterCount = 0;
        static uint32_t s_aheadUnderExitCount = 0;
        static uint32_t s_behindPressureCount = 0;
        static uint32_t s_startupGateLogCount = 0;
        static uint32_t s_timesyncAheadPressureLogCount = 0;
        static uint32_t s_timesyncSettleSuppressedLogCount = 0;
        static uint32_t s_catchupRollbackSuppressedLogCount = 0;
        static uint32_t s_catchupCooldownSuppressedLogCount = 0;
        static uint32_t s_aheadThrottleSuppressedLogCount = 0;

        // Detect new session: reset per-session state
        if (!s_rollbackSessionWasActive) {
            s_rollbackSessionWasActive = true;
            s_loggedTimesyncEnabled = false;
            s_liveTimesyncWasEnabled = false;
            s_timesyncCatchupArmed = false;
            s_catchupCooldown = 0;
            s_aheadThrottleCooldown = 0;
            s_doDoubleTick = false;
            s_doAheadThrottleHold = false;
            s_lastDispatcherCallAdvanced = false;
            s_lastAdvanceWasRollback = false;
            s_postReleaseNormalAdvanceCount = 0;
            s_liveTimesyncEnabledAtMs = 0;
            s_loggedFirstBeginAfterRelease = false;
            s_loggedFirstAdvanceAfterRelease = false;
            s_startupBiasActive = false;
            s_startupBias = 0.0f;
            s_lastCatchupDecisionFrame = -1000000;
            s_lastAheadDecisionFrame = -1000000;
            s_lastPacingDecisionFrame = -1000000;
            s_aheadThrottleActive = false;
            s_aheadOverEnterCount = 0;
            s_aheadUnderExitCount = 0;
            s_behindPressureCount = 0;
            s_startupGateLogCount = 0;
            s_timesyncAheadPressureLogCount = 0;
            s_timesyncSettleSuppressedLogCount = 0;
            s_catchupRollbackSuppressedLogCount = 0;
            s_catchupCooldownSuppressedLogCount = 0;
            s_aheadThrottleSuppressedLogCount = 0;
            // Clear any stale global freeze state from prior sessions. Runtime
            // drift control below is dispatcher-local and does not use hard
            // frame suppression hooks.
            InputSyncHooks_SetTimesyncFreeze(false);
            Rollback::NetplayLog_Write("TIMESYNC", -1,
                "Session started: timesync=startup phase=%s "
                "semantics=framesAhead>0 local_ahead, framesAhead<0 local_behind",
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
        }

        // Gameplay-entry barrier: hold local BeginFrame until OnlineWiring
        // confirms the mutual startup release at the interactive boundary.
        // Packet/session pumping must continue while held.
        if (Rollback::OnlineWiring_IsGameplayEntryAdvanceBlocked()) {
            s_lastDispatcherCallAdvanced = false;
            s_startupGateLogCount++;
            if (s_startupGateLogCount <= 5 || (s_startupGateLogCount % 120) == 0) {
                Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                    "Session running but gameplay advance GATED at interactive boundary "
                    "(release/session pending): phase=%s session_running=%d",
                    Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                    Rollback::RollbackSession_IsSessionRunning() ? 1 : 0);
            }
            Net::Session_Update();
            if (!Rollback::RollbackSession_PollSession()) {
                return AbortRollbackDispatcher("Peer disconnected during startup barrier");
            }
            return -1;
        }
        s_startupGateLogCount = 0;

        float framesAhead = Rollback::RollbackSession_FramesAhead();
        const float freezeEnter = GetTimesyncFreezeEnterThreshold();
        const int rollbackBudget = Rollback::RollbackSession_GetRollbackBudget();

        // Timesync drift control belongs only to live gameplay after the
        // startup release barrier has completed.
        const bool inPlayableGameplay = IsInPlayableMatchGameplay();
        const bool startupReleased = Rollback::OnlineWiring_IsStartupReleased();
        const bool liveGameplayTimesync = inPlayableGameplay && startupReleased;
        const int32_t timesyncFrame = Rollback::RollbackSession_GetCurrentFrame();
        const DWORD nowMs = GetTickCount();

        constexpr DWORD TIMESYNC_SETTLE_MS = 180;
        constexpr uint32_t TIMESYNC_SETTLE_NORMAL_ADVANCES = 2;
        constexpr float STARTUP_BIAS_CAPTURE_THRESHOLD = 1.25f;
        constexpr float STARTUP_BIAS_DECAY_PER_NORMAL_ADV = 0.15f;
        constexpr float AHEAD_THROTTLE_EXIT_HYSTERESIS = 0.75f;
        constexpr uint32_t AHEAD_THROTTLE_ENTER_FRAMES = 3;
        constexpr uint32_t AHEAD_THROTTLE_EXIT_FRAMES = 5;
        constexpr uint32_t CATCHUP_CONFIRM_FRAMES = 2;

        if (liveGameplayTimesync && !s_liveTimesyncWasEnabled) {
            s_liveTimesyncWasEnabled = true;
            s_liveTimesyncEnabledAtMs = nowMs;
            s_postReleaseNormalAdvanceCount = 0;
            s_timesyncCatchupArmed = false;
            s_catchupCooldown = 0;
            s_aheadThrottleCooldown = 0;
            s_doDoubleTick = false;
            s_doAheadThrottleHold = false;
            s_lastCatchupDecisionFrame = -1000000;
            s_lastAheadDecisionFrame = -1000000;
            s_lastPacingDecisionFrame = -1000000;
            s_aheadThrottleActive = false;
            s_aheadOverEnterCount = 0;
            s_aheadUnderExitCount = 0;
            s_behindPressureCount = 0;
            s_timesyncSettleSuppressedLogCount = 0;
            s_catchupRollbackSuppressedLogCount = 0;
            s_catchupCooldownSuppressedLogCount = 0;
            s_aheadThrottleSuppressedLogCount = 0;

            if (std::fabs(framesAhead) >= STARTUP_BIAS_CAPTURE_THRESHOLD) {
                s_startupBiasActive = true;
                s_startupBias = framesAhead;
                Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                    "Startup bias capture ACTIVE at live enable: raw=%.2f bias=%.2f threshold=%.2f",
                    framesAhead, s_startupBias, STARTUP_BIAS_CAPTURE_THRESHOLD);
            } else {
                s_startupBiasActive = false;
                s_startupBias = 0.0f;
                Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                    "Startup bias capture SKIPPED at live enable: raw=%.2f threshold=%.2f",
                    framesAhead, STARTUP_BIAS_CAPTURE_THRESHOLD);
            }
        } else if (!liveGameplayTimesync && s_liveTimesyncWasEnabled) {
            s_liveTimesyncWasEnabled = false;
            s_timesyncCatchupArmed = false;
            s_doDoubleTick = false;
            s_doAheadThrottleHold = false;
            s_aheadThrottleActive = false;
            s_aheadOverEnterCount = 0;
            s_aheadUnderExitCount = 0;
            Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                "Gameplay timesync DISABLED: phase=%s startupReleased=%d",
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                startupReleased ? 1 : 0);
        }

        float effectiveFramesAhead = framesAhead;
        if (s_startupBiasActive) {
            effectiveFramesAhead -= s_startupBias;
        }
        const float aheadThresholdEnter = (std::max)(1.25f, freezeEnter * 0.6f);

        if (!s_loggedTimesyncEnabled && liveGameplayTimesync) {
            s_loggedTimesyncEnabled = true;
            Rollback::NetplayLog_Write("TIMESYNC", -1,
                "Gameplay timesync ENABLED (PlayableGameplay + startup released): "
                "raw=%.2f effective=%.2f bias=%.2f bias_active=%d ahead_enter=%.2f budget=%d",
                framesAhead, effectiveFramesAhead, s_startupBias,
                s_startupBiasActive ? 1 : 0, aheadThresholdEnter, rollbackBudget);
        }

        if (liveGameplayTimesync) {
            const bool settleElapsed =
                s_liveTimesyncEnabledAtMs != 0 &&
                (nowMs - s_liveTimesyncEnabledAtMs) >= TIMESYNC_SETTLE_MS;
            const bool settleNormalAdvance =
                s_postReleaseNormalAdvanceCount >= TIMESYNC_SETTLE_NORMAL_ADVANCES;

            if (!s_timesyncCatchupArmed) {
                if (settleElapsed && settleNormalAdvance) {
                    s_timesyncCatchupArmed = true;
                    Rollback::NetplayLog_Write("TIMESYNC", -1,
                        "Gameplay timesync CATCH-UP armed after settle window: "
                        "elapsed_ms=%lu normal_advances=%u raw=%.2f effective=%.2f",
                        (unsigned long)(nowMs - s_liveTimesyncEnabledAtMs),
                        s_postReleaseNormalAdvanceCount,
                        framesAhead,
                        effectiveFramesAhead);
                } else {
                    s_timesyncSettleSuppressedLogCount++;
                    if (s_timesyncSettleSuppressedLogCount <= 5 ||
                        (s_timesyncSettleSuppressedLogCount % 120) == 0) {
                        Rollback::NetplayLog_Write("TIMESYNC", -1,
                            "Gameplay timesync catch-up SUPPRESSED by settle window: "
                            "elapsed_ms=%lu/%lu normal_advances=%u/%u raw=%.2f effective=%.2f",
                            (unsigned long)(nowMs - s_liveTimesyncEnabledAtMs),
                            (unsigned long)TIMESYNC_SETTLE_MS,
                            s_postReleaseNormalAdvanceCount,
                            TIMESYNC_SETTLE_NORMAL_ADVANCES,
                            framesAhead,
                            effectiveFramesAhead);
                    }
                }
            }

            // Use a lower ahead-entry threshold than the legacy freezeEnter so
            // ahead-side correction can actually engage in live windows.
            const float aheadEnter = aheadThresholdEnter;
            const float aheadExit = (std::max)(0.5f, aheadEnter - AHEAD_THROTTLE_EXIT_HYSTERESIS);

            if (effectiveFramesAhead >= aheadEnter) {
                s_timesyncAheadPressureLogCount++;
                if (s_timesyncAheadPressureLogCount <= 5 ||
                    (s_timesyncAheadPressureLogCount % 120) == 0) {
                    Rollback::NetplayLog_Write("TIMESYNC", -1,
                        "Local ahead pressure observed: effective=%.2f raw=%.2f "
                        "bias=%.2f enter=%.2f exit=%.2f budget=%d",
                        effectiveFramesAhead, framesAhead, s_startupBias,
                        aheadEnter, aheadExit, rollbackBudget);
                }
            } else {
                s_timesyncAheadPressureLogCount = 0;
            }

            if (!s_aheadThrottleActive) {
                if (effectiveFramesAhead >= aheadEnter) {
                    s_aheadOverEnterCount++;
                    if (s_aheadOverEnterCount >= AHEAD_THROTTLE_ENTER_FRAMES) {
                        s_aheadThrottleActive = true;
                        s_aheadOverEnterCount = 0;
                        s_aheadUnderExitCount = 0;
                        Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                            "Ahead-side SOFT THROTTLE ENTER: raw=%.2f effective=%.2f "
                            "enter=%.2f exit=%.2f",
                            framesAhead, effectiveFramesAhead, aheadEnter, aheadExit);
                    }
                } else {
                    s_aheadOverEnterCount = 0;
                }
            } else {
                if (effectiveFramesAhead <= aheadExit) {
                    s_aheadUnderExitCount++;
                    if (s_aheadUnderExitCount >= AHEAD_THROTTLE_EXIT_FRAMES) {
                        s_aheadThrottleActive = false;
                        s_doAheadThrottleHold = false;
                        s_aheadUnderExitCount = 0;
                        Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                            "Ahead-side SOFT THROTTLE EXIT: raw=%.2f effective=%.2f exit=%.2f",
                            framesAhead, effectiveFramesAhead, aheadExit);
                    }
                } else {
                    s_aheadUnderExitCount = 0;
                }
            }
        } else {
            s_timesyncAheadPressureLogCount = 0;
            s_timesyncSettleSuppressedLogCount = 0;
            s_timesyncCatchupArmed = false;
            s_postReleaseNormalAdvanceCount = 0;
            s_doDoubleTick = false;
            s_doAheadThrottleHold = false;
            s_aheadThrottleActive = false;
            s_aheadOverEnterCount = 0;
            s_aheadUnderExitCount = 0;
            s_behindPressureCount = 0;
            s_lastPacingDecisionFrame = -1000000;
            s_catchupRollbackSuppressedLogCount = 0;
            s_catchupCooldownSuppressedLogCount = 0;
            s_aheadThrottleSuppressedLogCount = 0;
            if (inPlayableGameplay && !startupReleased) {
                Rollback::NetplayLog_Write("TIMESYNC", -1,
                    "Gameplay timesync SUPPRESSED by startup barrier: phase=%s "
                    "released=%d",
                    Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            }
        }

        if (s_catchupCooldown > 0) {
            s_catchupCooldown--;
        }
        if (s_aheadThrottleCooldown > 0) {
            s_aheadThrottleCooldown--;
        }

        const bool rollingBackNow = Rollback::RollbackSession_IsRollingBack();
        const bool catchupBehind = effectiveFramesAhead <= -1.0f;
        const bool stableNormalAdvance = s_lastDispatcherCallAdvanced && !s_lastAdvanceWasRollback;
        if (liveGameplayTimesync && s_timesyncCatchupArmed && catchupBehind && stableNormalAdvance && !rollingBackNow) {
            if (s_behindPressureCount < 255u) {
                s_behindPressureCount++;
            }
        } else if (!catchupBehind) {
            s_behindPressureCount = 0;
        }

        // If the local peer is behind, schedule an extra tick to catch up.
        // Only applies during live gameplay — not during startup/intro where
        // GekkoNet drives deterministic progression on its own.
        if (liveGameplayTimesync &&
            s_timesyncCatchupArmed &&
            stableNormalAdvance &&
            catchupBehind &&
            !s_doDoubleTick) {
            const bool blockedByRollback = rollingBackNow || s_lastAdvanceWasRollback;
            const bool blockedByCooldown = s_catchupCooldown > 0;
            const bool blockedByDecisionWindow = timesyncFrame == s_lastCatchupDecisionFrame;
            const bool blockedByPacingWindow = timesyncFrame == s_lastPacingDecisionFrame;
            const bool blockedByConfirm = s_behindPressureCount < CATCHUP_CONFIRM_FRAMES;

            if (!blockedByRollback &&
                !blockedByCooldown &&
                !blockedByDecisionWindow &&
                !blockedByPacingWindow &&
                !blockedByConfirm) {
                const float framesBehind = -effectiveFramesAhead;
                const uint32_t pressureCount = s_behindPressureCount;
                s_doDoubleTick = true;
                s_catchupCooldown = GetCatchupCooldown(framesBehind);
                s_lastCatchupDecisionFrame = timesyncFrame;
                s_lastPacingDecisionFrame = timesyncFrame;
                s_behindPressureCount = 0;
                Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                    "Scheduling catch-up tick: behind=%.2f raw=%.2f effective=%.2f "
                    "bias=%.2f cooldown=%d rb=%d pressure=%u",
                    framesBehind,
                    framesAhead,
                    effectiveFramesAhead,
                    s_startupBias,
                    s_catchupCooldown,
                    rollingBackNow ? 1 : 0,
                    pressureCount);
            } else if (blockedByRollback) {
                s_catchupRollbackSuppressedLogCount++;
                if (s_catchupRollbackSuppressedLogCount <= 5 ||
                    (s_catchupRollbackSuppressedLogCount % 120) == 0) {
                    Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                        "Catch-up SUPPRESSED: rollback active (rolling_back=%d last_advance_rb=%d) "
                        "raw=%.2f effective=%.2f",
                        rollingBackNow ? 1 : 0,
                        s_lastAdvanceWasRollback ? 1 : 0,
                        framesAhead,
                        effectiveFramesAhead);
                }
            } else if (blockedByCooldown) {
                s_catchupCooldownSuppressedLogCount++;
                if (s_catchupCooldownSuppressedLogCount <= 5 ||
                    (s_catchupCooldownSuppressedLogCount % 120) == 0) {
                    Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                        "Catch-up SUPPRESSED: cooldown active (%d) raw=%.2f effective=%.2f",
                        s_catchupCooldown,
                        framesAhead,
                        effectiveFramesAhead);
                }
            } else if (blockedByConfirm) {
                Rollback::NetplayLog_Verbose("TIMESYNC", timesyncFrame,
                    "Catch-up waiting for confirm streak: pressure=%u/%u raw=%.2f effective=%.2f",
                    s_behindPressureCount,
                    CATCHUP_CONFIRM_FRAMES,
                    framesAhead,
                    effectiveFramesAhead);
            } else if (blockedByPacingWindow || blockedByDecisionWindow) {
                Rollback::NetplayLog_Verbose("TIMESYNC", timesyncFrame,
                    "Catch-up skipped by pacing arbitration: pacing_window=%d decision_window=%d "
                    "raw=%.2f effective=%.2f",
                    blockedByPacingWindow ? 1 : 0,
                    blockedByDecisionWindow ? 1 : 0,
                    framesAhead,
                    effectiveFramesAhead);
            }
        }

        // Local-ahead correction: apply bounded, hysteretic, soft pacing hold.
        // This is intentionally a light throttle, not a hard freeze.
        if (liveGameplayTimesync &&
            s_aheadThrottleActive &&
            stableNormalAdvance &&
            !s_doDoubleTick &&
            !s_doAheadThrottleHold &&
            effectiveFramesAhead >= aheadThresholdEnter) {
            const bool blockedByRollback = rollingBackNow || s_lastAdvanceWasRollback;
            const bool blockedByCooldown = s_aheadThrottleCooldown > 0;
            const bool blockedByDecisionWindow = timesyncFrame == s_lastAheadDecisionFrame;
            const bool blockedByPacingWindow = timesyncFrame == s_lastPacingDecisionFrame;

            if (!blockedByRollback &&
                !blockedByCooldown &&
                !blockedByDecisionWindow &&
                !blockedByPacingWindow) {
                s_doAheadThrottleHold = true;
                s_aheadThrottleCooldown = GetAheadThrottleCooldown(effectiveFramesAhead);
                s_lastAheadDecisionFrame = timesyncFrame;
                s_lastPacingDecisionFrame = timesyncFrame;
                Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                    "Ahead-side SOFT THROTTLE scheduled: raw=%.2f effective=%.2f "
                    "cooldown=%d rb=%d",
                    framesAhead,
                    effectiveFramesAhead,
                    s_aheadThrottleCooldown,
                    rollingBackNow ? 1 : 0);
            } else {
                s_aheadThrottleSuppressedLogCount++;
                if (s_aheadThrottleSuppressedLogCount <= 5 ||
                    (s_aheadThrottleSuppressedLogCount % 120) == 0) {
                    Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                        "Ahead-side SOFT THROTTLE suppressed: rollback=%d last_rb=%d cooldown=%d "
                        "pacing_window=%d decision_window=%d",
                        rollingBackNow ? 1 : 0,
                        s_lastAdvanceWasRollback ? 1 : 0,
                        s_aheadThrottleCooldown,
                        blockedByPacingWindow ? 1 : 0,
                        blockedByDecisionWindow ? 1 : 0);
                }
            }
        }

        // State: track whether we've started this frame's event batch
        static bool s_gekkoFrameStarted = false;

        if (!s_gekkoFrameStarted) {
            if (s_doAheadThrottleHold) {
                s_doAheadThrottleHold = false;
                s_lastDispatcherCallAdvanced = false;
                Rollback::NetplayLog_Write("TIMESYNC", timesyncFrame,
                    "Ahead-side SOFT THROTTLE EXECUTED: skipping one local advance window "
                    "raw=%.2f effective=%.2f bias=%.2f cooldown=%d",
                    framesAhead,
                    effectiveFramesAhead,
                    s_startupBias,
                    s_aheadThrottleCooldown);
                Net::Session_Update();
                if (!Rollback::RollbackSession_PollSession()) {
                    return AbortRollbackDispatcher("Peer disconnected during ahead-side throttle hold");
                }
                return -1;
            }

            // Phase 1: Collect local input and feed to GekkoNet
            InputSystem_Update();
            uint16_t localInput = Net::PlayerMapping_ReadLocalInput();

            if (!s_loggedFirstBeginAfterRelease &&
                Rollback::OnlineWiring_IsStartupReleased() &&
                inPlayableGameplay) {
                s_loggedFirstBeginAfterRelease = true;
                Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                    "First BeginFrame allowed after startup release");
            }

            const char* timesyncMode = "startup";
            if (liveGameplayTimesync) {
                if (s_doAheadThrottleHold) {
                    timesyncMode = "live-ahead-throttle";
                } else {
                    timesyncMode = s_timesyncCatchupArmed ? "live" : "release-handoff";
                }
            } else if (inPlayableGameplay && !startupReleased) {
                timesyncMode = "startup-gated";
            }

            Rollback::NetplayLog_Write("INPUT", -1,
                "Dispatcher: starting new frame, local_input=0x%04X "
                "framesAhead(raw=%.2f effective=%.2f bias=%.2f) phase=%s tsync=%s",
                localInput, framesAhead, effectiveFramesAhead, s_startupBias,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                timesyncMode);

            Rollback::RollbackSession_BeginFrame(localInput);
            s_gekkoFrameStarted = true;
        }

        // Phase 2: Process next GekkoNet event
        Rollback::EventResult result = Rollback::RollbackSession_ProcessNextEvent();

        if (result == Rollback::EventResult::Error) {
            s_gekkoFrameStarted = false;
            s_lastDispatcherCallAdvanced = false;
            return AbortRollbackDispatcher("Rollback session failed");
        }

        if (result == Rollback::EventResult::Advance) {
            // GekkoNet wants one frame advanced (normal or rollback).
            // Inputs already set via InputSystem_SetNetplayInput by HandleAdvanceEvent.
            // Write to outputInputs for the game's dispatcher contract.
            uint16_t p1 = 0, p2 = 0;
            Rollback::RollbackSession_GetAdvanceInputs(&p1, &p2);
            outputInputs[0] = (__int16)p1;
            outputInputs[1] = (__int16)p2;

            // Write inputs to history buffers (game's match handler reads from these)
            volatile int32_t* pFrameWrite = reinterpret_cast<volatile int32_t*>(ADDR_INPUT_WRITE_IDX);
            const uint32_t writeIdx = (uint32_t)*pFrameWrite;
            if (writeIdx < INPUT_HISTORY_MAX) {
                *reinterpret_cast<volatile uint16_t*>(ADDR_P1_INPUT_HISTORY + (writeIdx * sizeof(uint16_t))) = p1;
                *reinterpret_cast<volatile uint16_t*>(ADDR_P2_INPUT_HISTORY + (writeIdx * sizeof(uint16_t))) = p2;
            }
            *pFrameWrite = (int32_t)(writeIdx + 1);

            Rollback::NetplayLog_Write("INPUT", -1,
                "Dispatcher: Advance → P1=0x%04X P2=0x%04X writeIdx=%u->%u rb=%d",
                p1, p2, writeIdx, writeIdx + 1, Rollback::RollbackSession_IsRollingBack() ? 1 : 0);

            s_lastDispatcherCallAdvanced = true;
            const bool advanceWasRollback = Rollback::RollbackSession_IsRollingBack();
            s_lastAdvanceWasRollback = advanceWasRollback;
            if (!startupReleased) {
                Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                    "ERROR: Advance produced before mutual post-intro startup release");
            }
            if (startupReleased && inPlayableGameplay && !advanceWasRollback) {
                s_postReleaseNormalAdvanceCount++;
                if (!s_loggedFirstAdvanceAfterRelease) {
                    s_loggedFirstAdvanceAfterRelease = true;
                    Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                        "First post-release NORMAL Advance observed");
                }
            } else if (startupReleased && inPlayableGameplay && advanceWasRollback) {
                Rollback::NetplayLog_Write("TIMESYNC", Rollback::RollbackSession_GetCurrentFrame(),
                    "Advance during rollback observed post-release (catch-up remains blocked): "
                    "raw=%.2f effective=%.2f",
                    framesAhead,
                    effectiveFramesAhead);
            }

            if (liveGameplayTimesync && !advanceWasRollback && s_startupBiasActive) {
                const float prevBias = s_startupBias;
                if (s_startupBias > 0.0f) {
                    s_startupBias = (std::max)(0.0f, s_startupBias - STARTUP_BIAS_DECAY_PER_NORMAL_ADV);
                } else if (s_startupBias < 0.0f) {
                    s_startupBias = (std::min)(0.0f, s_startupBias + STARTUP_BIAS_DECAY_PER_NORMAL_ADV);
                }

                if (prevBias != s_startupBias &&
                    (s_postReleaseNormalAdvanceCount <= 5 ||
                     (s_postReleaseNormalAdvanceCount % 120) == 0)) {
                    Rollback::NetplayLog_Write("TIMESYNC", Rollback::RollbackSession_GetCurrentFrame(),
                        "Startup bias decay: prev=%.2f next=%.2f raw=%.2f effective=%.2f normal_adv=%u",
                        prevBias,
                        s_startupBias,
                        framesAhead,
                        framesAhead - s_startupBias,
                        s_postReleaseNormalAdvanceCount);
                }

                const float postDecayEffective = framesAhead - s_startupBias;
                if (std::fabs(s_startupBias) < 0.25f || std::fabs(postDecayEffective) < 0.35f) {
                    s_startupBiasActive = false;
                    Rollback::NetplayLog_Write("TIMESYNC", Rollback::RollbackSession_GetCurrentFrame(),
                        "Startup bias neutralization COMPLETE: residual_bias=%.2f raw=%.2f effective=%.2f",
                        s_startupBias,
                        framesAhead,
                        postDecayEffective);
                    s_startupBias = 0.0f;
                }
            }

            return 0;  // Game's loop runs one full tick (InputProcess + matchHandler)
        } else {
            // No more events (Done) — frame batch complete
            s_gekkoFrameStarted = false;
            s_lastDispatcherCallAdvanced = false;

            // Double-tick: if scheduled, start another BeginFrame cycle
            // so the game runs a second full tick through the match handler
            if (s_doDoubleTick) {
                s_doDoubleTick = false;

                InputSystem_Update();
                uint16_t localInput2 = Net::PlayerMapping_ReadLocalInput();

                Rollback::NetplayLog_Write("TIMESYNC", -1,
                    "Catch-up tick: starting second BeginFrame, local_input=0x%04X", localInput2);

                Rollback::RollbackSession_BeginFrame(localInput2);
                s_gekkoFrameStarted = true;

                // Continue processing — the game's while loop will call us
                // again for each Advance event from GekkoNet
                Rollback::EventResult result2 = Rollback::RollbackSession_ProcessNextEvent();
                if (result2 == Rollback::EventResult::Error) {
                    s_gekkoFrameStarted = false;
                    s_lastDispatcherCallAdvanced = false;
                    return AbortRollbackDispatcher("Rollback session failed during double-tick");
                }

                if (result2 == Rollback::EventResult::Advance) {
                    uint16_t p1 = 0, p2 = 0;
                    Rollback::RollbackSession_GetAdvanceInputs(&p1, &p2);
                    outputInputs[0] = (__int16)p1;
                    outputInputs[1] = (__int16)p2;

                    volatile int32_t* pFrameWrite2 = reinterpret_cast<volatile int32_t*>(ADDR_INPUT_WRITE_IDX);
                    const uint32_t writeIdx2 = (uint32_t)*pFrameWrite2;
                    if (writeIdx2 < INPUT_HISTORY_MAX) {
                        *reinterpret_cast<volatile uint16_t*>(ADDR_P1_INPUT_HISTORY + (writeIdx2 * sizeof(uint16_t))) = p1;
                        *reinterpret_cast<volatile uint16_t*>(ADDR_P2_INPUT_HISTORY + (writeIdx2 * sizeof(uint16_t))) = p2;
                    }
                    *pFrameWrite2 = (int32_t)(writeIdx2 + 1);

                    Rollback::NetplayLog_Write("TIMESYNC", -1,
                        "Catch-up Advance → P1=0x%04X P2=0x%04X writeIdx=%u->%u rb=%d",
                        p1, p2, writeIdx2, writeIdx2 + 1,
                        Rollback::RollbackSession_IsRollingBack() ? 1 : 0);

                    s_lastDispatcherCallAdvanced = true;
                    const bool advanceWasRollback2 = Rollback::RollbackSession_IsRollingBack();
                    s_lastAdvanceWasRollback = advanceWasRollback2;
                    if (!startupReleased) {
                        Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                            "ERROR: Catch-up Advance produced before mutual post-intro startup release");
                    }
                    if (startupReleased && inPlayableGameplay && !advanceWasRollback2) {
                        s_postReleaseNormalAdvanceCount++;
                        if (!s_loggedFirstAdvanceAfterRelease) {
                            s_loggedFirstAdvanceAfterRelease = true;
                            Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                                "First post-release NORMAL Advance observed");
                        }
                    } else if (startupReleased && inPlayableGameplay && advanceWasRollback2) {
                        Rollback::NetplayLog_Write("TIMESYNC", Rollback::RollbackSession_GetCurrentFrame(),
                            "Catch-up Advance ran during rollback (no additional catch-up scheduling): "
                            "raw=%.2f effective=%.2f",
                            framesAhead,
                            effectiveFramesAhead);
                    }

                    if (liveGameplayTimesync && !advanceWasRollback2 && s_startupBiasActive) {
                        const float prevBias = s_startupBias;
                        if (s_startupBias > 0.0f) {
                            s_startupBias = (std::max)(0.0f, s_startupBias - STARTUP_BIAS_DECAY_PER_NORMAL_ADV);
                        } else if (s_startupBias < 0.0f) {
                            s_startupBias = (std::min)(0.0f, s_startupBias + STARTUP_BIAS_DECAY_PER_NORMAL_ADV);
                        }

                        const float postDecayEffective2 = framesAhead - s_startupBias;
                        if (prevBias != s_startupBias &&
                            (s_postReleaseNormalAdvanceCount <= 5 ||
                             (s_postReleaseNormalAdvanceCount % 120) == 0)) {
                            Rollback::NetplayLog_Write("TIMESYNC", Rollback::RollbackSession_GetCurrentFrame(),
                                "Startup bias decay (catch-up advance path): prev=%.2f next=%.2f raw=%.2f effective=%.2f",
                                prevBias,
                                s_startupBias,
                                framesAhead,
                                postDecayEffective2);
                        }

                        if (std::fabs(s_startupBias) < 0.25f || std::fabs(postDecayEffective2) < 0.35f) {
                            s_startupBiasActive = false;
                            Rollback::NetplayLog_Write("TIMESYNC", Rollback::RollbackSession_GetCurrentFrame(),
                                "Startup bias neutralization COMPLETE: residual_bias=%.2f raw=%.2f effective=%.2f",
                                s_startupBias,
                                framesAhead,
                                postDecayEffective2);
                            s_startupBias = 0.0f;
                        }
                    }

                    return 0;  // Game processes this tick normally
                }
                // If Done immediately (no advance needed), fall through
                s_gekkoFrameStarted = false;
                s_lastDispatcherCallAdvanced = false;
            }

            Rollback::NetplayLog_Write("INPUT", -1,
                "Dispatcher: Done, breaking dispatcher loop");

            return -1;  // Break game's dispatcher loop
        }
    }

#endif
    if (Rollback::RollbackSession_IsActive()) {
        if (!s_rollbackSessionWasActive) {
            s_rollbackSessionWasActive = true;
            s_rollbackFrameStarted = false;
            s_loggedFirstBeginAfterRelease = false;
            s_loggedFirstAdvanceAfterRelease = false;
            s_startupGateLogCount = 0;
            Net::NetplayPacing_ResetSession("dispatcher session start");
            InputSyncHooks_SetTimesyncFreeze(false);
            Rollback::NetplayLog_Write("TIMESYNC", -1,
                "Session started: controller=tick-slew phase=%s",
                Net::MatchRollbackPhaseName(Net::NetplayPhaseRuntime_GetPhase()));
        }

        const bool sessionFramePending = Rollback::RollbackSession_HasPendingFrame();
        if (s_rollbackFrameStarted != sessionFramePending) {
            Rollback::NetplayLog_Write("INPUT", Rollback::RollbackSession_GetCurrentFrame(),
                "Dispatcher/session frame-start sync: local=%d session=%d phase=%s",
                s_rollbackFrameStarted ? 1 : 0,
                sessionFramePending ? 1 : 0,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            s_rollbackFrameStarted = sessionFramePending;
        }

        const Net::MatchRollbackPhase rollbackPhase = Net::NetplayPhaseRuntime_GetPhase();
        const bool inPlayableGameplay =
            Net::NetplayPhaseRuntime_IsInteractivePacingPhase(rollbackPhase);
        const bool startupReleased = Rollback::OnlineWiring_IsStartupReleased();
        const bool liveGameplayPacing = startupReleased && inPlayableGameplay;

        if (Rollback::OnlineWiring_IsGameplayEntryAdvanceBlocked()) {
            s_startupGateLogCount++;
            if (s_startupGateLogCount <= 5 || (s_startupGateLogCount % 120) == 0) {
                Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                    "Session running but gameplay advance GATED at interactive boundary "
                    "(release/session pending): phase=%s session_running=%d",
                    Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                    Rollback::RollbackSession_IsSessionRunning() ? 1 : 0);
            }
            InputSyncHooks_SetTimesyncFreeze(true);
            Net::Session_Update();
            if (!Rollback::RollbackSession_PollSession()) {
                InputSyncHooks_SetTimesyncFreeze(false);
                return AbortRollbackDispatcher("Peer disconnected during startup barrier");
            }
            ResetVanillaTimeoutCounters();
            return -1;
        }
        s_startupGateLogCount = 0;
        InputSyncHooks_SetTimesyncFreeze(false);

        if (!s_rollbackFrameStarted) {
            Rollback::RollbackTimesyncTelemetry preTelemetry{};
            Rollback::RollbackSession_GetTimesyncTelemetry(&preTelemetry);

            const int stallThreshold = Net::DelayPolicy_GetStallThreshold();
            const int32_t currentFrame = preTelemetry.rb_frame_current;

            const Net::NetplayPacingAction pacingAction = Net::NetplayPacing_BeginFrame(
                preTelemetry,
                rollbackPhase,
                startupReleased,
                stallThreshold);
            if (pacingAction != Net::NetplayPacingAction::None) {
                Net::NetplayPacing_OnHoldSample(
                    preTelemetry,
                    rollbackPhase,
                    startupReleased,
                    pacingAction);
                Net::NetplayPacingSnapshot pacingSnap{};
                Net::NetplayPacing_GetSnapshot(&pacingSnap);
                const int holdCount =
                    pacingAction == Net::NetplayPacingAction::SoftHold
                        ? pacingSnap.soft_hold_count
                        : (pacingAction == Net::NetplayPacingAction::HardHold
                            ? pacingSnap.hard_hold_count
                            : pacingSnap.stall_frame_count);
                const int holdThreshold =
                    pacingAction == Net::NetplayPacingAction::SoftHold
                        ? pacingSnap.soft_threshold
                        : (pacingAction == Net::NetplayPacingAction::HardHold
                            ? pacingSnap.hard_threshold
                            : pacingSnap.stall_threshold);
                if (holdCount <= 5 || (holdCount % 120) == 0) {
                    Rollback::NetplayLog_Write(
                        "STALL", currentFrame,
                        "Holding gameplay: rb_current=%d rb_remote=%d action=%s raw_gap=%d debt=%d remote_eff=%d threshold=%d phase=%s count=%d class=%s pressure=%.2f",
                        currentFrame,
                        preTelemetry.rb_frame_last_remote_received,
                        Net::NetplayPacingActionName(pacingAction),
                        pacingSnap.raw_remote_gap,
                        pacingSnap.prediction_debt,
                        pacingSnap.effective_remote_delay,
                        holdThreshold,
                        Net::MatchRollbackPhaseName(rollbackPhase),
                        holdCount,
                        Net::NetQualityName(pacingSnap.quality),
                        pacingSnap.pressure);
                }
                InputSyncHooks_SetTimesyncFreeze(true);
                Net::Session_Update();
                if (!Rollback::RollbackSession_PollSession()) {
                    InputSyncHooks_SetTimesyncFreeze(false);
                    return AbortRollbackDispatcher("Peer disconnected during stall hold");
                }
                ResetVanillaTimeoutCounters();
                return -1;
            }

            InputSyncHooks_SetTimesyncFreeze(false);
            InputSystem_Update();
            const uint16_t localInput = Net::PlayerMapping_ReadLocalInput();

            if (!s_loggedFirstBeginAfterRelease && startupReleased && inPlayableGameplay) {
                s_loggedFirstBeginAfterRelease = true;
                Rollback::NetplayLog_Write("STARTUP", currentFrame,
                    "First BeginFrame allowed after startup release");
            }

            Rollback::NetplayLog_Write("INPUT", currentFrame,
                "Dispatcher frame start: local_input=0x%04X phase=%s stall_threshold=%d",
                localInput,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                stallThreshold);

            Rollback::RollbackSession_BeginFrame(localInput);

            Rollback::RollbackTimesyncTelemetry postTelemetry{};
            Rollback::RollbackSession_GetTimesyncTelemetry(&postTelemetry);
            Net::NetplayPacing_OnSessionSample(
                postTelemetry,
                rollbackPhase,
                startupReleased,
                Rollback::RollbackSession_IsSessionRunning());
            s_rollbackFrameStarted = true;

            Net::NetplayPacingSnapshot pacingSnap{};
            Net::NetplayPacing_GetSnapshot(&pacingSnap);

            Rollback::NetplayLog_Verbose("TIMESYNC", postTelemetry.rb_frame_current,
                "UpdateSession complete: rb_current=%d game_abs=%d origin_abs=%d frames_ahead=%.2f adjust_ms=%.2f tick_target=%.3f tick_current=%.3f remote_rb=%d confirmed_rb=%d ping=%.1f jitter=%.1f phase=%s",
                postTelemetry.rb_frame_current,
                postTelemetry.game_abs_frame_current,
                postTelemetry.frame_origin_abs,
                postTelemetry.frames_ahead,
                pacingSnap.filtered_adjust_ms,
                pacingSnap.target_scale,
                pacingSnap.current_scale,
                postTelemetry.rb_frame_last_remote_received,
                postTelemetry.rb_frame_last_confirmed,
                postTelemetry.gekko_avg_ping,
                postTelemetry.gekko_jitter,
                Net::MatchRollbackPhaseName(rollbackPhase));
        }

        Rollback::EventResult result = Rollback::RollbackSession_ProcessNextEvent();

        if (result == Rollback::EventResult::Error) {
            s_rollbackFrameStarted = false;
            return AbortRollbackDispatcher("Rollback session failed");
        }

        if (result == Rollback::EventResult::Advance) {
            uint16_t p1 = 0;
            uint16_t p2 = 0;
            Rollback::RollbackSession_GetAdvanceInputs(&p1, &p2);
            CommitRollbackAdvance(outputInputs, p1, p2);

            const bool hadRollback = Rollback::RollbackSession_IsRollingBack();
            Rollback::NetplayLog_Write("INPUT", Rollback::RollbackSession_GetCurrentFrame(),
                "Dispatcher: Advance -> P1=0x%04X P2=0x%04X rollback=%d",
                p1, p2, hadRollback ? 1 : 0);

            if (!startupReleased) {
                Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                    "ERROR: Advance produced before mutual post-intro startup release");
            } else if (inPlayableGameplay && !hadRollback && !s_loggedFirstAdvanceAfterRelease) {
                s_loggedFirstAdvanceAfterRelease = true;
                Rollback::NetplayLog_Write("STARTUP", Rollback::RollbackSession_GetCurrentFrame(),
                    "First post-release NORMAL Advance observed");
            }

            ResetVanillaTimeoutCounters();
            return 0;
        }

        s_rollbackFrameStarted = false;
        ResetVanillaTimeoutCounters();

        Rollback::NetplayLog_Write("INPUT", Rollback::RollbackSession_GetCurrentFrame(),
            "Dispatcher: Done, breaking dispatcher loop");

        return -1;
    }

    // ── Vanilla passthrough (offline/local play only) ────────────────
    // Reset logging state when not intercepting
    if (s_dispatchFirstLog) {
        s_dispatchFirstLog = false;
        s_dispatchCount = 0;
        s_dispatchWaitCount = 0;
    }
    // Reset per-session startup tracking so it re-fires on the next session
    s_rollbackSessionWasActive = false;
    s_rollbackFrameStarted = false;
    s_loggedFirstBeginAfterRelease = false;
    s_loggedFirstAdvanceAfterRelease = false;
    s_startupGateLogCount = 0;
    Net::NetplayPacing_NotifyLocalMode();
    InputSyncHooks_SetTimesyncFreeze(false);
    const int result = g_origInputDispatcher(outputInputs);
    if (result == 0 && Replay::ReplayRuntime_IsReplayMatchActive()) {
        Replay::ReplayRuntime_OnDispatcherAdvance(reinterpret_cast<int16_t*>(outputInputs));
    }
    return result;
}

// ============================================================================
// Input Processing Hook (sub_562060)
// ============================================================================

static_assert(ADDR_P1_INPUT_BUFFER == 0x8E9E62,
              "Alt-buffer IS the main buffer — they must be the same address");
static_assert(ADDR_P2_INPUT_BUFFER == 0x8E9F32,
              "Alt-buffer IS the main buffer — they must be the same address");

static const uint16_t g_buttonMasks[10] = {
    0x0001, 0x0002, 0x0004, 0x0008, 0x0010,
    0x0020, 0x0040, 0x0080, 0x0100, 0x0200
};
static uint16_t ReadHeldMaskFromAltBuffer(uintptr_t altBufferAddr) {
    uint16_t heldMask = 0;
    __try {
        for (int i = 0; i < 10; i++) {
            const uint16_t heldVal = ReadMemory<uint16_t>(altBufferAddr + (i * 2));
            if (heldVal) {
                heldMask |= g_buttonMasks[i];
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return heldMask;
}

static uint16_t ReadJustPressedMaskFromAltBuffer(uintptr_t altBufferAddr) {
    uint16_t pressedMask = 0;
    __try {
        for (int i = 0; i < 10; i++) {
            const uintptr_t justAddr = altBufferAddr +
                (JUST_PRESSED_OFFSET_WORDS * 2) +
                (i * 2);
            const uint16_t pressedVal = ReadMemory<uint16_t>(justAddr);
            if (pressedVal) {
                pressedMask |= g_buttonMasks[i];
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return pressedMask;
}

int __cdecl Hook_InputProcess(int gameState) {
    const bool consumeForCustomMenu =
        InputSystem_IsBindingActive() ||
        NetMenu::ConsumesGameInput() ||
        Replay::ReplayRuntime_ShouldConsumeMenuInput();
    const uint32_t gameMode = GetGameMode();
    const uint32_t subState = GetSubstate();

    constexpr size_t FRONTEND_INPUT_BUFFER_CLEAR_SIZE =
        JUST_PRESSED_OFFSET_WORDS * sizeof(uint16_t);
    static_assert(ADDR_P1_INPUT_STATE - ADDR_P1_INPUT_BUFFER == FRONTEND_INPUT_BUFFER_CLEAR_SIZE,
        "P1 input state must immediately follow the live input buffer");
    static_assert(ADDR_P2_INPUT_STATE - ADDR_P2_INPUT_BUFFER == FRONTEND_INPUT_BUFFER_CLEAR_SIZE,
        "P2 input state must immediately follow the live input buffer");

    if (ModConfig_UseSDLInput()) {
        EnsureInputUpdated();
    }

    const uint16_t prevHeldP1 = ReadHeldMaskFromAltBuffer(ADDR_P1_INPUT_BUFFER);
    const uint16_t prevHeldP2 = ReadHeldMaskFromAltBuffer(ADDR_P2_INPUT_BUFFER);

    int result = g_origInputProcess(gameState);

    auto clearLiveInputBuffers = []() {
        // The frontend "input buffers" share the surrounding block with
        // charsel player data. Clearing the full 208-byte match-era span here
        // wipes the committed character IDs after selection.
        static const uint8_t zeroBuffer[FRONTEND_INPUT_BUFFER_CLEAR_SIZE] = {};
        static const uint8_t zeroState[INPUT_STATE_SIZE] = {};
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_BUFFER, zeroBuffer, sizeof(zeroBuffer));
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_BUFFER, zeroBuffer, sizeof(zeroBuffer));
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_STATE, zeroState, sizeof(zeroState));
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_STATE, zeroState, sizeof(zeroState));
    };

    auto writeLiveInputBuffers = [](uint16_t currentP1,
                                    uint16_t currentP2,
                                    uint16_t pressedP1,
                                    uint16_t pressedP2) {
        for (int i = 0; i < 10; i++) {
            const uint16_t mask = g_buttonMasks[i];

            {
                const uintptr_t heldAddr = ADDR_P1_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                WriteMemory<uint16_t>(heldAddr, (currentP1 & mask) ? 1 : 0);
                WriteMemory<uint16_t>(justPressedAddr, (pressedP1 & mask) ? 1 : 0);
            }

            {
                const uintptr_t heldAddr = ADDR_P2_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                WriteMemory<uint16_t>(heldAddr, (currentP2 & mask) ? 1 : 0);
                WriteMemory<uint16_t>(justPressedAddr, (pressedP2 & mask) ? 1 : 0);
            }
        }
    };

    auto readSdlFrontendInputs = [](uint16_t* outP1, uint16_t* outP2) {
        const uint16_t allowedMask = (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT |
                                      INPUT_A | INPUT_B | INPUT_C | INPUT_D |
                                      INPUT_START | INPUT_SELECT);

        const InputState_t* p1State = InputSystem_GetState(0);
        const InputState_t* p2State = InputSystem_GetState(1);
        uint16_t currentP1 = (uint16_t)((p1State ? p1State->current : 0) & allowedMask);
        uint16_t currentP2 = (uint16_t)((p2State ? p2State->current : 0) & allowedMask);

        // During netplay, P2 hardware is remote-owned. This mirrors the
        // generic SDL injection path below so offline stage handling does not
        // accidentally reintroduce a second local controller.
        if (Net::Session_IsConnected()) {
            currentP2 = 0;
        }

        if (InputSystem_GetControlSwap()) {
            const uint16_t tmp = currentP1;
            currentP1 = currentP2;
            currentP2 = tmp;
        }

        if (outP1) {
            *outP1 = currentP1;
        }
        if (outP2) {
            *outP2 = currentP2;
        }
    };

    auto resetWinScreenProcessState = []() {
        s_winscreenDispatchCount = 0;
        s_winscreenWaitCount = 0;
        s_winscreenPrevP1 = 0;
        s_winscreenPrevP2 = 0;
        s_winscreenFrameProduced = false;
        s_winscreenFrameP1 = 0;
        s_winscreenFrameP2 = 0;
        s_winscreenFrameJustP1 = 0;
        s_winscreenFrameJustP2 = 0;
        s_winscreenLastProcessFrame = 0xFFFFFFFFu;
        s_winscreenFirstLog = false;
    };

    if (consumeForCustomMenu) {
        if (Replay::ReplayRuntime_ShouldConsumeMenuInput()) {
            Replay::ReplayRuntime_OnFrontendInputsProcessed();
        }
        clearLiveInputBuffers();
        return result;
    }

    if (ShouldHoldCharSelUntilLockstep(gameMode, subState)) {
        clearLiveInputBuffers();
        return result;
    }

    if (gameMode == MODE_CHARSEL &&
        IsCharSelDispatcherLockstepSubstate(subState) &&
        Net::CharSelSync_IsLockstepActive() &&
        !Net::CharSelPaletteSelect_IsCatalogReady()) {
        clearLiveInputBuffers();
        return result;
    }

    if (!(gameMode == MODE_CHARSEL && IsStageSelRawLockstepSubstate(subState)) &&
        Net::CharSelSelectActions_IsStageRandomScrollActive()) {
        Net::CharSelSelectActions_ResetStageRandomScroll();
    }

    if (Net::CharSelSync_IsLockstepActive()) {
        if (gameMode == MODE_CHARSEL && IsStageSelRawLockstepSubstate(subState)) {
            // On phase entry, reset edge detection baseline.
            // During subs 5-6 (non-interactive animation), the SDL path wrote
            // each player's LOCAL physical input to the P1 buffer. That state
            // is unsynchronized — if host held A but client didn't, their
            // just-pressed computations diverge even though merged held is
            // identical. Forcing prevHeld=0 on the first stage frame ensures
            // both sides compute the same just-pressed from the same baseline.
            const bool edgeReset = Net::StageSelSync_ConsumeEdgeReset();

            InputSystem_Update();
            const uint16_t localInput = Net::PlayerMapping_ReadLocalInput();
            Net::CharSelSync_CaptureLocalInput(localInput);

            if (!Net::CharSelSync_HasInputsForCurrentFrame()) {
                clearLiveInputBuffers();
                return result;
            }

            uint16_t p1 = 0;
            uint16_t p2 = 0;
            if (!Net::CharSelSync_ConsumeCurrentFrame(&p1, &p2)) {
                clearLiveInputBuffers();
                return result;
            }

            const uint32_t mergedFrontendFrame = Net::FrontendInputSync_GetConsumeFrame() > 0
                ? (Net::FrontendInputSync_GetConsumeFrame() - 1)
                : 0;
            if (edgeReset) {
                Rollback::NetplayLog_Write(
                    "STAGESEL", -1,
                    "Stage merge authority=Hook_InputProcess shared_frame=%u policy=or_strip_select_cancel_axes",
                    mergedFrontendFrame);
            }

            const uint16_t merged = Net::StageSelSync_MergeConfirmed(
                mergedFrontendFrame, p1, p2);
            if (edgeReset) {
                s_stageCancelPrevMerged = merged;
            }
            const uint16_t stageCancelPressed =
                (uint16_t)(merged & (uint16_t)~s_stageCancelPrevMerged);
            s_stageCancelPrevMerged = merged;

            const uint16_t adjPrevHeld = edgeReset ? (uint16_t)0 : prevHeldP1;
            uint16_t gatedMerged = merged;
            uint16_t pressedMerged = (uint16_t)(merged & (uint16_t)~adjPrevHeld);
            if ((stageCancelPressed & INPUT_B) != 0 &&
                Net::CharSelSelectActions_HandleNetplayStageCancelInput(stageCancelPressed)) {
                clearLiveInputBuffers();
                s_dispPrevP1 = 0;
                s_dispPrevP2 = 0;
                s_stageCancelPrevMerged = 0;
                s_dispatchFirstLog = false;
                s_charsel_produced_this_loop = false;
                return result;
            }

            const bool stageRandomStarted =
                ((stageCancelPressed & INPUT_D) != 0) &&
                Net::CharSelSelectActions_HandleNetplayStageRandomInput(stageCancelPressed, merged);
            const bool stageRandomAdvanced = Net::CharSelSelectActions_AdvanceStageRandomScroll();

            gatedMerged &= (uint16_t)~INPUT_B;
            pressedMerged &= (uint16_t)~INPUT_B;
            gatedMerged &= (uint16_t)~INPUT_D;
            pressedMerged &= (uint16_t)~INPUT_D;
            if (stageRandomStarted ||
                stageRandomAdvanced ||
                Net::CharSelSelectActions_IsStageRandomScrollActive()) {
                gatedMerged = 0;
                pressedMerged = 0;
            }
            if (subState == CHARSEL_SUB_STAGESEL_GRID) {
                const uint8_t stageCursor = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
                Net::StageSelSync_ApplyConfirmAckGate(
                    mergedFrontendFrame,
                    &gatedMerged,
                    &pressedMerged,
                    Net::FrontendInputSync_GetRemoteAckFrame(),
                    Net::FrontendInputSync_GetSharedDelay(),
                    stageCursor);
            }

            // g_origInputProcess already populated the stage-select raw input
            // block from unsynchronized local hardware state. Clear the safe
            // frontend-owned span so the merged lockstep input becomes the only
            // remaining source for held/current/just-pressed stage UI reads.
            clearLiveInputBuffers();

            for (int i = 0; i < 10; i++) {
                const uint16_t mask = g_buttonMasks[i];

                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (i * 2),
                    (gatedMerged & mask) ? 1 : 0);
                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                    (pressedMerged & mask) ? 1 : 0);

                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (i * 2), 0);
                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2), 0);
            }

            return result;
        }
    }

    if (gameMode == MODE_CHARSEL &&
        IsStageSelRawLockstepSubstate(subState) &&
        !Net::CharSelSync_IsLockstepActive()) {
        uint16_t currentP1 = 0;
        uint16_t currentP2 = 0;
        uint16_t justP1 = 0;
        uint16_t justP2 = 0;
        const bool usingSdlInput = ModConfig_UseSDLInput();
        if (usingSdlInput) {
            readSdlFrontendInputs(&currentP1, &currentP2);
            justP1 = (uint16_t)(currentP1 & (uint16_t)~prevHeldP1);
            justP2 = (uint16_t)(currentP2 & (uint16_t)~prevHeldP2);
        } else {
            currentP1 = ReadHeldMaskFromAltBuffer(ADDR_P1_INPUT_BUFFER);
            currentP2 = ReadHeldMaskFromAltBuffer(ADDR_P2_INPUT_BUFFER);
            justP1 = ReadJustPressedMaskFromAltBuffer(ADDR_P1_INPUT_BUFFER);
            justP2 = ReadJustPressedMaskFromAltBuffer(ADDR_P2_INPUT_BUFFER);
        }

        const uint16_t pressedMerged = (uint16_t)(justP1 | justP2);
        const uint16_t heldMerged = (uint16_t)(currentP1 | currentP2);

        if ((pressedMerged & (INPUT_B | INPUT_D)) != 0 ||
            Net::CharSelSelectActions_IsStageRandomScrollActive()) {
            Rollback::NetplayLog_Write(
                "STAGESEL", -1,
                "Offline stage frontend input: source=%s sub=%u held=0x%04X just=0x%04X p1=0x%04X p2=0x%04X p1_just=0x%04X p2_just=0x%04X random_active=%u",
                usingSdlInput ? "sdl" : "raw",
                subState,
                heldMerged,
                pressedMerged,
                currentP1,
                currentP2,
                justP1,
                justP2,
                Net::CharSelSelectActions_IsStageRandomScrollActive() ? 1 : 0);
        }

        if ((pressedMerged & INPUT_B) != 0 &&
            Net::CharSelSelectActions_HandleOfflineStageCancelInput(pressedMerged)) {
            clearLiveInputBuffers();
            s_stageCancelPrevMerged = 0;
            return result;
        }
        const bool stageRandomStarted =
            ((pressedMerged & INPUT_D) != 0) &&
            Net::CharSelSelectActions_HandleOfflineStageRandomInput(pressedMerged,
                (uint16_t)(currentP1 | currentP2));
        const bool stageRandomAdvanced = Net::CharSelSelectActions_AdvanceStageRandomScroll();
        if (stageRandomStarted ||
            stageRandomAdvanced ||
            Net::CharSelSelectActions_IsStageRandomScrollActive()) {
            clearLiveInputBuffers();
            return result;
        }
        if ((heldMerged & (INPUT_B | INPUT_D)) != 0) {
            const uint16_t stageFrontendMask = (INPUT_B | INPUT_D);
            writeLiveInputBuffers(
                (uint16_t)(currentP1 & (uint16_t)~stageFrontendMask),
                (uint16_t)(currentP2 & (uint16_t)~stageFrontendMask),
                (uint16_t)(justP1 & (uint16_t)~stageFrontendMask),
                (uint16_t)(justP2 & (uint16_t)~stageFrontendMask));
            return result;
        }
    }

    // Sub 3 is the first interactive substate — subs 0-2 are non-interactive
    // animations (route detection, asset load, 25-frame fade-in). Lockstep still
    // consumes every frame during 0-2 so both peers stay aligned before input matters.
    static constexpr uint32_t WINSCREEN_INTERACTIVE_SUB = 3;

    const bool winScreenRoute =
        gameMode == MODE_WINSCREEN ||
        (gameMode == MODE_MATCH &&
         subState == MATCH_SUB_END &&
         Net::MatchLifecycle_GetPhase() == Net::MatchLifecyclePhase::MatchEnd);

    if (winScreenRoute &&
        !Net::WinScreenSync_IsActive() &&
        Net::MatchLifecycle_IsMatchOwned() &&
        Net::Session_IsConnected()) {
        Rollback::NetplayLog_Write("WINLOCK", -1,
            "InputProcess activating winscreen lockstep on-demand (mode=%u sub=%u phase=%s)",
            gameMode,
            subState,
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
        Net::WinScreenSync_Begin();
    }

    if (Net::WinScreenSync_IsActive()) {
        if (!winScreenRoute) {
            Rollback::NetplayLog_Write("WINLOCK", -1,
                "InputProcess lockstep abort request: sync active outside win-screen route (mode=%u sub=%u phase=%s)",
                gameMode,
                subState,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            Net::WinScreenSync_Abort();
            clearLiveInputBuffers();
            resetWinScreenProcessState();
            return result;
        }

        if (!s_winscreenFirstLog) {
            resetWinScreenProcessState();
            s_winscreenFirstLog = true;
            Rollback::NetplayLog_Write("WINLOCK", -1,
                "InputProcess lockstep ACTIVE: mode=%u sub=%u consume=%u remote_latest=%u",
                gameMode,
                subState,
                Net::WinScreenSync_GetConsumeFrame(),
                Net::WinScreenSync_GetRemoteLatestFrame());
        }

        const uint32_t absFrame = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
        if (absFrame == s_winscreenLastProcessFrame) {
            if (gameMode == MODE_WINSCREEN && subState < WINSCREEN_INTERACTIVE_SUB) {
                clearLiveInputBuffers();
            } else if (s_winscreenFrameProduced) {
                writeLiveInputBuffers(
                    s_winscreenFrameP1,
                    s_winscreenFrameP2,
                    s_winscreenFrameJustP1,
                    s_winscreenFrameJustP2);
            } else {
                clearLiveInputBuffers();
            }
            return result;
        }
        s_winscreenLastProcessFrame = absFrame;

        const uint16_t localInput = Net::PlayerMapping_ReadLocalInput();
        Net::WinScreenSync_CaptureLocalInput(localInput);

        const bool writeInputsToGame =
            gameMode == MODE_WINSCREEN && subState >= WINSCREEN_INTERACTIVE_SUB;
        if (writeInputsToGame) {
            Net::WinScreenSync_NotifyLocalRawAdvance(localInput);
        }

        if (!Net::WinScreenSync_HasInputsForCurrentFrame()) {
            s_winscreenFrameProduced = false;
            s_winscreenFrameP1 = 0;
            s_winscreenFrameP2 = 0;
            s_winscreenFrameJustP1 = 0;
            s_winscreenFrameJustP2 = 0;
            s_winscreenWaitCount++;
            if (s_winscreenWaitCount <= 5 || (s_winscreenWaitCount % 120) == 0) {
                Rollback::NetplayLog_Write("WINLOCK", -1,
                    "InputProcess waiting for remote frame: wait#%u consume=%u remote_latest=%u mode=%u sub=%u",
                    s_winscreenWaitCount,
                    Net::WinScreenSync_GetConsumeFrame(),
                    Net::WinScreenSync_GetRemoteLatestFrame(),
                    gameMode,
                    subState);
            }
            clearLiveInputBuffers();
            return result;
        }

        uint16_t p1 = 0;
        uint16_t p2 = 0;
        if (!Net::WinScreenSync_ConsumeCurrentFrame(&p1, &p2)) {
            s_winscreenFrameProduced = false;
            clearLiveInputBuffers();
            Rollback::NetplayLog_Write("WINLOCK", -1,
                "InputProcess consume failed despite ready frame: consume=%u remote_latest=%u",
                Net::WinScreenSync_GetConsumeFrame(),
                Net::WinScreenSync_GetRemoteLatestFrame());
            return result;
        }

        s_winscreenDispatchCount++;
        s_winscreenWaitCount = 0;

        if (!writeInputsToGame) {
            clearLiveInputBuffers();
            if (s_winscreenDispatchCount <= 5 || (s_winscreenDispatchCount % 120) == 0) {
                Rollback::NetplayLog_Write("WINLOCK", -1,
                    "InputProcess sync-only frame#%u mode=%u sub=%u abs=%u",
                    s_winscreenDispatchCount,
                    gameMode,
                    subState,
                    absFrame);
            }
            return result;
        }

        const uint16_t justP1 = p1 & ~s_winscreenPrevP1;
        const uint16_t justP2 = p2 & ~s_winscreenPrevP2;
        s_winscreenPrevP1 = p1;
        s_winscreenPrevP2 = p2;
        s_winscreenFrameProduced = true;
        s_winscreenFrameP1 = p1;
        s_winscreenFrameP2 = p2;
        s_winscreenFrameJustP1 = justP1;
        s_winscreenFrameJustP2 = justP2;

        writeLiveInputBuffers(p1, p2, justP1, justP2);

        if (s_winscreenDispatchCount <= 5 || (s_winscreenDispatchCount % 120) == 0) {
            Rollback::NetplayLog_Write("WINLOCK", -1,
                "InputProcess advance frame#%u sub=%u abs=%u P1=0x%04X P2=0x%04X",
                s_winscreenDispatchCount,
                subState,
                absFrame,
                p1,
                p2);
        }

        return result;
    }

    if (s_winscreenFirstLog) {
        Rollback::NetplayLog_Write("WINLOCK", -1,
            "InputProcess lockstep INACTIVE: mode=%u sub=%u consumed=%u",
            gameMode,
            subState,
            s_winscreenDispatchCount);
        resetWinScreenProcessState();
    }

    // Netplay override: when rollback session is active, inject rollback-controlled
    // inputs instead of SDL data. The netplay inputs were stored by
    // RollbackSession_FrameUpdate Step 7 via InputSystem_SetNetplayInput.
    if (InputSystem_IsNetplayInputActive(0) || InputSystem_IsNetplayInputActive(1)) {
        uint16_t currentP1 = InputSystem_IsNetplayInputActive(0)
                                 ? InputSystem_GetNetplayInput(0) : 0;
        uint16_t currentP2 = InputSystem_IsNetplayInputActive(1)
                                 ? InputSystem_GetNetplayInput(1) : 0;

        uint16_t pressedP1 = (uint16_t)(currentP1 & (uint16_t)~prevHeldP1);
        uint16_t pressedP2 = (uint16_t)(currentP2 & (uint16_t)~prevHeldP2);
        if (InputSystem_IsPauseBlocked()) {
            pressedP1 = (uint16_t)(pressedP1 & (uint16_t)~INPUT_START);
            pressedP2 = (uint16_t)(pressedP2 & (uint16_t)~INPUT_START);
        }

        for (int i = 0; i < 10; i++) {
            const uint16_t mask = g_buttonMasks[i];

            {
                const uintptr_t heldAddr = ADDR_P1_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                WriteMemory<uint16_t>(heldAddr, (currentP1 & mask) ? 1 : 0);
                WriteMemory<uint16_t>(justPressedAddr, (pressedP1 & mask) ? 1 : 0);
            }

            {
                const uintptr_t heldAddr = ADDR_P2_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                WriteMemory<uint16_t>(heldAddr, (currentP2 & mask) ? 1 : 0);
                WriteMemory<uint16_t>(justPressedAddr, (pressedP2 & mask) ? 1 : 0);
            }
        }

        return result;
    }

    if (ModConfig_UseSDLInput()) {
        uint16_t currentP1 = 0;
        uint16_t currentP2 = 0;
        readSdlFrontendInputs(&currentP1, &currentP2);

        uint16_t pressedP1 = (uint16_t)(currentP1 & (uint16_t)~prevHeldP1);
        uint16_t pressedP2 = (uint16_t)(currentP2 & (uint16_t)~prevHeldP2);
        if (InputSystem_IsPauseBlocked()) {
            pressedP1 = (uint16_t)(pressedP1 & (uint16_t)~INPUT_START);
            pressedP2 = (uint16_t)(pressedP2 & (uint16_t)~INPUT_START);
        }

        for (int i = 0; i < 10; i++) {
            const uint16_t mask = g_buttonMasks[i];

            {
                const uintptr_t heldAddr = ADDR_P1_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                const uint16_t heldVal = (currentP1 & mask) ? 1 : 0;
                const uint16_t pressedVal = (pressedP1 & mask) ? 1 : 0;
                WriteMemory<uint16_t>(heldAddr, heldVal);
                WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (i * 2), heldVal);
            }

            {
                const uintptr_t heldAddr = ADDR_P2_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                const uint16_t heldVal = (currentP2 & mask) ? 1 : 0;
                const uint16_t pressedVal = (pressedP2 & mask) ? 1 : 0;
                WriteMemory<uint16_t>(heldAddr, heldVal);
                WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (i * 2), heldVal);
            }
        }

        // During netplay: charsel lockstep capture is handled by Hook_InputDispatcher.
        // Do NOT capture here — it would cause unbounded send-head growth.
    }

    return result;
}

// ============================================================================
// Direct Input Injection
// ============================================================================

static const uint8_t SCANCODE_UP    = 0xC8;
static const uint8_t SCANCODE_DOWN  = 0xD0;
static const uint8_t SCANCODE_LEFT  = 0xCB;
static const uint8_t SCANCODE_RIGHT = 0xCD;
static const uint8_t SCANCODE_Z     = 0x2C;
static const uint8_t SCANCODE_X     = 0x2D;
static const uint8_t SCANCODE_A     = 0x1E;
static const uint8_t SCANCODE_S     = 0x1F;
static const uint8_t SCANCODE_ENTER = 0x1C;
static const uint8_t SCANCODE_ESC   = 0x01;

void WriteKeyboardInput(uint8_t scancode, bool pressed) {
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    if (pressed) {
        keyBuffer[scancode] |= 0x80;
    } else {
        keyBuffer[scancode] &= 0x7F;
    }
}

static void WriteJoystickInputDirect(int joyIndex, uint16_t input) {
    if (joyIndex < 0 || joyIndex >= DINPUT_JOY_MAX) return;
    
    uintptr_t joyBase = ADDR_DINPUT_JOYSTICK + (joyIndex * DINPUT_JOY_STRUCT_SIZE);
    
    int32_t xAxis = 0;
    if (input & INPUT_LEFT)  xAxis = -1000;
    if (input & INPUT_RIGHT) xAxis = +1000;
    WriteMemory<int32_t>(joyBase + 0, xAxis);
    
    int32_t yAxis = 0;
    if (input & INPUT_UP)   yAxis = -1000;
    if (input & INPUT_DOWN) yAxis = +1000;
    WriteMemory<int32_t>(joyBase + 4, yAxis);
    
    uint8_t* buttons = reinterpret_cast<uint8_t*>(joyBase + DINPUT_JOY_BTN_OFFSET);
    for (int i = 0; i < 24; i++) {
        buttons[i] = 0;
    }
    
    if (input & INPUT_A)      buttons[0] = 0x80;
    if (input & INPUT_B)      buttons[1] = 0x80;
    if (input & INPUT_C)      buttons[2] = 0x80;
    if (input & INPUT_D)      buttons[3] = 0x80;
    if (input & INPUT_START)  buttons[7] = 0x80;
    if (input & INPUT_SELECT) buttons[6] = 0x80;
}

void WriteMatchInput(int player, uint16_t input) {
    uintptr_t bufferAddr = (player == 0) ? ADDR_P1_INPUT_BUFFER : ADDR_P2_INPUT_BUFFER;
    
    WriteMemory<uint16_t>(bufferAddr + 0,  (input & INPUT_DOWN)   ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 2,  (input & INPUT_UP)     ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 4,  (input & INPUT_LEFT)   ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 6,  (input & INPUT_RIGHT)  ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 8,  (input & INPUT_A)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 10, (input & INPUT_B)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 12, (input & INPUT_C)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 14, (input & INPUT_D)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 16, (input & INPUT_START)  ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 18, (input & INPUT_SELECT) ? 1 : 0);
}

void WritePlayerInput(int player, uint16_t input) {
    WriteMatchInput(player, input);
}

uint16_t ReadPlayerInput(int player) {
    uintptr_t bufferAddr = (player == 0) ? ADDR_P1_INPUT_BUFFER : ADDR_P2_INPUT_BUFFER;
    uint16_t result = 0;
    for (int i = 0; i < 10; i++) {
        if (ReadMemory<uint16_t>(bufferAddr + i * 2) == 1) {
            result |= (1 << i);
        }
    }
    return result;
}

// ============================================================================
// ImGui Debug Content
// ============================================================================

void UpdateInputDebugInfo() {
    g_inputDebug.sdlInputP1 = InputSystem_GetInput(0);
    g_inputDebug.sdlInputP2 = InputSystem_GetInput(1);

    uint16_t localBufferP1[10] = {};
    uint16_t localBufferP2[10] = {};
    if (CopyMemorySafe(localBufferP1, (const void*)ADDR_P1_INPUT_BUFFER, sizeof(localBufferP1))) {
        memcpy(g_inputDebug.gameBufferP1, localBufferP1, sizeof(localBufferP1));
    } else {
        for (int i = 0; i < 10; i++) {
            g_inputDebug.gameBufferP1[i] = ReadMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + i * 2);
        }
    }
    if (CopyMemorySafe(localBufferP2, (const void*)ADDR_P2_INPUT_BUFFER, sizeof(localBufferP2))) {
        memcpy(g_inputDebug.gameBufferP2, localBufferP2, sizeof(localBufferP2));
    } else {
        for (int i = 0; i < 10; i++) {
            g_inputDebug.gameBufferP2[i] = ReadMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + i * 2);
        }
    }

    uintptr_t joyBase = ADDR_DINPUT_JOYSTICK;
    uint8_t joyState[72] = {};
    if (CopyMemorySafe(joyState, (const void*)joyBase, sizeof(joyState))) {
        memcpy(&g_inputDebug.dinputJoyAxisX, joyState + 0, sizeof(g_inputDebug.dinputJoyAxisX));
        memcpy(&g_inputDebug.dinputJoyAxisY, joyState + 4, sizeof(g_inputDebug.dinputJoyAxisY));
        memcpy(g_inputDebug.dinputJoyButtons, joyState + DINPUT_JOY_BTN_OFFSET, sizeof(g_inputDebug.dinputJoyButtons));
    } else {
        g_inputDebug.dinputJoyAxisX = ReadMemory<int32_t>(joyBase + 0);
        g_inputDebug.dinputJoyAxisY = ReadMemory<int32_t>(joyBase + 4);
        for (int i = 0; i < 8; i++) {
            g_inputDebug.dinputJoyButtons[i] = ReadMemory<uint8_t>(joyBase + DINPUT_JOY_BTN_OFFSET + i);
        }
    }

    uint8_t keyBuffer[256] = {};
    if (CopyMemorySafe(keyBuffer, (const void*)ADDR_DINPUT_KEYBOARD, sizeof(keyBuffer))) {
        g_inputDebug.keyState_Up    = keyBuffer[SCANCODE_UP];
        g_inputDebug.keyState_Down  = keyBuffer[SCANCODE_DOWN];
        g_inputDebug.keyState_Left  = keyBuffer[SCANCODE_LEFT];
        g_inputDebug.keyState_Right = keyBuffer[SCANCODE_RIGHT];
        g_inputDebug.keyState_Z     = keyBuffer[SCANCODE_Z];
        g_inputDebug.keyState_X     = keyBuffer[SCANCODE_X];
        g_inputDebug.keyState_Enter = keyBuffer[SCANCODE_ENTER];
    } else {
        g_inputDebug.keyState_Up    = ReadMemory<uint8_t>(ADDR_DINPUT_KEYBOARD + SCANCODE_UP);
        g_inputDebug.keyState_Down  = ReadMemory<uint8_t>(ADDR_DINPUT_KEYBOARD + SCANCODE_DOWN);
        g_inputDebug.keyState_Left  = ReadMemory<uint8_t>(ADDR_DINPUT_KEYBOARD + SCANCODE_LEFT);
        g_inputDebug.keyState_Right = ReadMemory<uint8_t>(ADDR_DINPUT_KEYBOARD + SCANCODE_RIGHT);
        g_inputDebug.keyState_Z     = ReadMemory<uint8_t>(ADDR_DINPUT_KEYBOARD + SCANCODE_Z);
        g_inputDebug.keyState_X     = ReadMemory<uint8_t>(ADDR_DINPUT_KEYBOARD + SCANCODE_X);
        g_inputDebug.keyState_Enter = ReadMemory<uint8_t>(ADDR_DINPUT_KEYBOARD + SCANCODE_ENTER);
    }
}

void RenderInputDebugContent() {
    UpdateInputDebugInfo();
    
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "=== Hook Statistics ===");
    ImGui::Text("Total Hook Calls: %d", g_hookCallCount);
    ImGui::Text("Keyboard Hook: %d (injected: %d)", 
        g_inputDebug.keyboardHookCalls, g_inputDebug.keyboardInjectedCount);
    ImGui::Text("Joystick Hook: %d (injected: %d)", 
        g_inputDebug.joystickHookCalls, g_inputDebug.joystickInjectedCount);
    
    if (g_hookCallCount == 0) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "WARNING: Hooks not being called!");
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Check if hooks installed correctly.");
    }
    
    ImGui::Text("Last keyCode: 0x%02X | playerID: 0x%08X", 
        g_inputDebug.lastKeyCode, g_inputDebug.lastPlayerID);
    ImGui::Text("Last origResult: 0x%04X -> finalResult: 0x%04X", 
        g_inputDebug.lastOrigResult, g_inputDebug.lastFinalResult);
    ImGui::Text("Last KB inject: 0x%04X | Last Joy inject: 0x%04X",
        g_inputDebug.lastInjectedKeyInput, g_inputDebug.lastInjectedJoyInput);
    
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 1, 1), "Player Mapping:");
    ImGui::Text("P1 JoyID: 0x%X | P2 JoyID: 0x%X | MappedTo: P%d", 
        g_inputDebug.p1JoyID, g_inputDebug.p2JoyID, g_inputDebug.lastMappedPlayer + 1);
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(0, 1, 1, 1), "=== SDL Input ===");
    ImGui::Text("P1 SDL: 0x%04X -> Game: 0x%04X", g_inputDebug.sdlInputP1, g_inputDebug.gameInputP1);
    ImGui::Text("P2 SDL: 0x%04X -> Game: 0x%04X", g_inputDebug.sdlInputP2, g_inputDebug.gameInputP2);
    
    // Visual P1 buttons
    ImGui::Text("P1: ");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_UP ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "U");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_DOWN ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_LEFT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "L");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_RIGHT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "R");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_A ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "A");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_B ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "B");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_C ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "C");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_D ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_START ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "St");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_SELECT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "Se");
    
    // Visual P2 buttons
    ImGui::Text("P2: ");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_UP ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "U");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_DOWN ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_LEFT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "L");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_RIGHT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "R");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_A ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "A");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_B ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "B");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_C ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "C");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_D ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_START ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "St");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_SELECT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "Se");
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "=== Game Match Buffers ===");
    ImGui::Text("P1@%08X: D%d U%d L%d R%d A%d B%d C%d D%d St%d Se%d",
        ADDR_P1_INPUT_BUFFER,
        g_inputDebug.gameBufferP1[0], g_inputDebug.gameBufferP1[1],
        g_inputDebug.gameBufferP1[2], g_inputDebug.gameBufferP1[3],
        g_inputDebug.gameBufferP1[4], g_inputDebug.gameBufferP1[5],
        g_inputDebug.gameBufferP1[6], g_inputDebug.gameBufferP1[7],
        g_inputDebug.gameBufferP1[8], g_inputDebug.gameBufferP1[9]);
    ImGui::Text("P2@%08X: D%d U%d L%d R%d A%d B%d C%d D%d St%d Se%d",
        ADDR_P2_INPUT_BUFFER,
        g_inputDebug.gameBufferP2[0], g_inputDebug.gameBufferP2[1],
        g_inputDebug.gameBufferP2[2], g_inputDebug.gameBufferP2[3],
        g_inputDebug.gameBufferP2[4], g_inputDebug.gameBufferP2[5],
        g_inputDebug.gameBufferP2[6], g_inputDebug.gameBufferP2[7],
        g_inputDebug.gameBufferP2[8], g_inputDebug.gameBufferP2[9]);
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "=== DirectInput ===");
    ImGui::Text("Joy@%08X: X=%d Y=%d", 
        ADDR_DINPUT_JOYSTICK, g_inputDebug.dinputJoyAxisX, g_inputDebug.dinputJoyAxisY);
    ImGui::Text("Joy Btns: %02X %02X %02X %02X %02X %02X %02X %02X",
        g_inputDebug.dinputJoyButtons[0], g_inputDebug.dinputJoyButtons[1],
        g_inputDebug.dinputJoyButtons[2], g_inputDebug.dinputJoyButtons[3],
        g_inputDebug.dinputJoyButtons[4], g_inputDebug.dinputJoyButtons[5],
        g_inputDebug.dinputJoyButtons[6], g_inputDebug.dinputJoyButtons[7]);
    ImGui::Text("KB@%08X: U=%02X D=%02X L=%02X R=%02X Z=%02X X=%02X",
        ADDR_DINPUT_KEYBOARD,
        g_inputDebug.keyState_Up, g_inputDebug.keyState_Down,
        g_inputDebug.keyState_Left, g_inputDebug.keyState_Right,
        g_inputDebug.keyState_Z, g_inputDebug.keyState_X);
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Hook Status:");
    ImGui::Text("KB hook: %s (orig: %p)", 
        g_origKeyboardState ? "OK" : "FAIL", g_origKeyboardState);
    ImGui::Text("Joy hook: %s (orig: %p)", 
        g_origJoystickState ? "OK" : "FAIL", g_origJoystickState);
}

void GetTimesyncDebugInfo(TimesyncDebugInfo* out) {
    if (!out) {
        return;
    }

    Net::NetplayPacingSnapshot pacingSnap{};
    Net::NetplayPacing_GetSnapshot(&pacingSnap);
    out->frames_ahead = pacingSnap.frames_ahead;
    out->rate_adjust_ms = pacingSnap.filtered_adjust_ms;
    out->stall_frame_count =
        pacingSnap.stall_frame_count + pacingSnap.soft_hold_count + pacingSnap.hard_hold_count;
    out->stalled = pacingSnap.stall_active ||
                   pacingSnap.soft_hold_active ||
                   pacingSnap.hard_hold_active;
}
