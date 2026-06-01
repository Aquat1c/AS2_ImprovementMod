#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

// ============================================================================
// Input Override Module
// ============================================================================
// Hooks for redirecting game input through the SDL input system.
// Handles DInput refresh bypass, keyboard/joystick state hooks,
// input processing (just-pressed flags), and direct buffer injection.

// --- Function typedefs for game hooks ---
typedef int (*KeyboardState_t)(int keyCode);
typedef int (*JoystickState_t)(int playerID);
typedef int (__cdecl *InputProcess_t)(int gameState);
typedef int (__cdecl *InputDispatcher_t)(__int16* outputInputs);
typedef int (__cdecl *DInputKBRefresh_t)();
typedef int (__cdecl *DInputJoyRefresh_t)(int joyID);
typedef BOOL (WINAPI *GetKeyboardState_t)(PBYTE lpKeyState);
typedef BOOL (WINAPI *ClipCursor_t)(const RECT* lpRect);
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE hModule, LPCSTR lpProcName);
typedef BOOL (WINAPI *SystemParametersInfoA_t)(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni);
typedef BOOL (WINAPI *WINNLSEnableIME_t)(HWND hWnd, BOOL fEnable);
typedef LPVOID (__cdecl *GetWindowHandle_t)();
typedef HRESULT (STDMETHODCALLTYPE *DInputSetCooperativeLevel_t)(void* device, HWND hWnd, DWORD dwFlags);

// --- Hook functions (installed by hook_installer) ---
int __cdecl Hook_KeyboardState(int keyCode);
int __cdecl Hook_JoystickState(int playerID);
int __cdecl Hook_InputProcess(int gameState);
int __cdecl Hook_InputDispatcher(__int16* outputInputs);
int __cdecl Hook_DInputKBRefresh();
int __cdecl Hook_DInputJoyRefresh(int joyID);
BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState);
BOOL WINAPI Hook_ClipCursor(const RECT* lpRect);
FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL WINAPI Hook_SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni);
BOOL WINAPI Hook_WINNLSEnableIME(HWND hWnd, BOOL fEnable);
HRESULT STDMETHODCALLTYPE Hook_DInputKeyboardSetCooperativeLevel(void* device, HWND hWnd, DWORD dwFlags);

// --- Original function pointers (set by hook_installer) ---
extern KeyboardState_t g_origKeyboardState;
extern JoystickState_t g_origJoystickState;
extern InputProcess_t g_origInputProcess;
extern InputDispatcher_t g_origInputDispatcher;
extern DInputKBRefresh_t g_origDInputKBRefresh;
extern DInputJoyRefresh_t g_origDInputJoyRefresh;
extern GetKeyboardState_t g_origGetKeyboardState;
extern ClipCursor_t g_origClipCursor;
extern GetProcAddress_t g_origGetProcAddress;
extern SystemParametersInfoA_t g_origSystemParametersInfoA;
extern WINNLSEnableIME_t g_origWINNLSEnableIME;
extern DInputSetCooperativeLevel_t g_origDInputKeyboardSetCooperativeLevel;

// --- Input conversion helpers ---
uint16_t ScanCodeToInputFlag(int scancode);
uint16_t ConvertToGameJoyFormat(uint16_t input);
uint8_t SDLScancodeToDIK(int sdlScancode);

// --- Direct input injection ---
void WriteKeyboardInput(uint8_t scancode, bool pressed);
void WriteMatchInput(int player, uint16_t input);
void WritePlayerInput(int player, uint16_t input);
uint16_t ReadPlayerInput(int player);
void InputOverride_LoadSettings();
void InputOverride_Shutdown();

struct InputGuardIniSnapshot {
    bool shell_hotkeys_ime;
};

void InputOverride_GetIniSnapshot(InputGuardIniSnapshot* out);
void InputOverride_SyncIniKeys();
bool InputOverride_AreShellHotkeyImeWorkaroundsEnabled();
bool InputOverride_AreSystemKeyWorkaroundsEnabled();
void* InputOverride_GetDInputKeyboardSetCooperativeLevelTarget();
void InputOverride_EnsureDInputKeyboardCooperativeLevel(const char* reason);

// RAII: clear game wndproc custom-handler gate so shell keys reach DefWindowProc inside the game.
struct InputOverrideGameWndProcShellGate {
    int saved;
    bool active;
    InputOverrideGameWndProcShellGate();
    ~InputOverrideGameWndProcShellGate();
};

// --- Debug ---
struct InputDebugInfo {
    int keyboardHookCalls;
    int joystickHookCalls;
    int lastKeyCode;
    int lastPlayerID;
    int lastOrigResult;
    int lastFinalResult;
    int keyboardInjectedCount;
    int joystickInjectedCount;
    uint16_t lastInjectedKeyInput;
    uint16_t lastInjectedJoyInput;
    int p1JoyID;
    int p2JoyID;
    int lastMappedPlayer;
    uint16_t sdlInputP1;
    uint16_t sdlInputP2;
    uint16_t sdlInputP1Raw;
    uint16_t sdlInputP2Raw;
    uint16_t gameInputP1;
    uint16_t gameInputP2;
    uint16_t gameBufferP1[10];
    uint16_t gameBufferP2[10];
    int32_t dinputJoyAxisX;
    int32_t dinputJoyAxisY;
    uint8_t dinputJoyButtons[8];
    uint8_t keyState_Up;
    uint8_t keyState_Down;
    uint8_t keyState_Left;
    uint8_t keyState_Right;
    uint8_t keyState_Z;
    uint8_t keyState_X;
    uint8_t keyState_Enter;
};

extern InputDebugInfo g_inputDebug;

struct TimesyncDebugInfo {
    float frames_ahead;
    float rate_adjust_ms;
    int   stall_frame_count;
    bool  stalled;
};

void UpdateInputDebugInfo();
void RenderInputDebugContent();
void GetTimesyncDebugInfo(TimesyncDebugInfo* out);
