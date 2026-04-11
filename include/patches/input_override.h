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

// --- Hook functions (installed by hook_installer) ---
int __cdecl Hook_KeyboardState(int keyCode);
int __cdecl Hook_JoystickState(int playerID);
int __cdecl Hook_InputProcess(int gameState);
int __cdecl Hook_InputDispatcher(__int16* outputInputs);
int __cdecl Hook_DInputKBRefresh();
int __cdecl Hook_DInputJoyRefresh(int joyID);
BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState);

// --- Original function pointers (set by hook_installer) ---
extern KeyboardState_t g_origKeyboardState;
extern JoystickState_t g_origJoystickState;
extern InputProcess_t g_origInputProcess;
extern InputDispatcher_t g_origInputDispatcher;
extern DInputKBRefresh_t g_origDInputKBRefresh;
extern DInputJoyRefresh_t g_origDInputJoyRefresh;
extern GetKeyboardState_t g_origGetKeyboardState;

// --- Input conversion helpers ---
uint16_t ScanCodeToInputFlag(int scancode);
uint16_t ConvertToGameJoyFormat(uint16_t input);
uint8_t SDLScancodeToDIK(int sdlScancode);

// --- Direct input injection ---
void WriteKeyboardInput(uint8_t scancode, bool pressed);
void WriteMatchInput(int player, uint16_t input);
void WritePlayerInput(int player, uint16_t input);
uint16_t ReadPlayerInput(int player);

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
