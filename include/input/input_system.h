/**
 * Alice Senki 2 - SDL3 Input System
 * Replaces game's native input with SDL3 for better control and netplay support.
 * Gamepad support via SDL3 Gamepad API (XInput, DirectInput, DualSense, etc.)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Input Button Definitions (matches game's internal format)
// ============================================================================

// Direction bits (directly usable)
#define INPUT_UP        0x0001
#define INPUT_DOWN      0x0002
#define INPUT_LEFT      0x0004
#define INPUT_RIGHT     0x0008

// Action buttons
#define INPUT_A         0x0010  // Light attack
#define INPUT_B         0x0020  // Medium attack
#define INPUT_C         0x0040  // Heavy attack
#define INPUT_D         0x0080  // Special

// System buttons
#define INPUT_START     0x0100
#define INPUT_SELECT    0x0200
#define INPUT_L1        0x0400
#define INPUT_R1        0x0800
#define INPUT_L2        0x1000
#define INPUT_R2        0x2000

// Combined inputs for convenience
#define INPUT_ANY_DIR   (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT)
#define INPUT_ANY_BTN   (INPUT_A | INPUT_B | INPUT_C | INPUT_D)

// Number of bindable actions per player
#define INPUT_ACTION_COUNT 14

// ============================================================================
// Key Binding Structure
// ============================================================================

typedef struct {
    int keyboard_key;      // SDL_SCANCODE_* (0 = unbound)
    int gamepad_button;    // SDL_GAMEPAD_BUTTON_* (-1 = unbound)
    int gamepad_axis;      // SDL_GAMEPAD_AXIS_* (-1 = not axis-bound)
    int axis_direction;    // 1 for positive, -1 for negative
} KeyBinding_t;

typedef struct {
    KeyBinding_t up;
    KeyBinding_t down;
    KeyBinding_t left;
    KeyBinding_t right;
    KeyBinding_t a;
    KeyBinding_t b;
    KeyBinding_t c;
    KeyBinding_t d;
    KeyBinding_t start;
    KeyBinding_t select;
    KeyBinding_t l1;
    KeyBinding_t r1;
    KeyBinding_t l2;
    KeyBinding_t r2;
} PlayerBindings_t;

// ============================================================================
// Input State
// ============================================================================

typedef struct {
    uint16_t current;      // Current frame input
    uint16_t previous;     // Previous frame input
    uint16_t pressed;      // Just pressed this frame (current & ~previous)
    uint16_t released;     // Just released this frame (~current & previous)
} InputState_t;

// ============================================================================
// Public API
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Initialize input system (SDL3 for keyboard + gamepads)
bool InputSystem_Init(void);

// Shutdown input system  
void InputSystem_Shutdown(void);

// Poll input and update state (call once per frame)
void InputSystem_Update(void);

// Get current input for player (0 = P1, 1 = P2)
uint16_t InputSystem_GetInput(int player);

// Get full input state for player
const InputState_t* InputSystem_GetState(int player);

// Check if a specific button is pressed
bool InputSystem_IsPressed(int player, uint16_t button);

// Check if a specific button was just pressed this frame
bool InputSystem_JustPressed(int player, uint16_t button);

// Check if button is ready (either just pressed, or held with repeat timer expired)
bool InputSystem_InputReady(int player, uint16_t button);

// Reset input repeat state (call when entering a new menu/state)
void InputSystem_ResetRepeatState(int player);

// Get/Set key bindings
const PlayerBindings_t* InputSystem_GetBindings(int player);
void InputSystem_SetBindings(int player, const PlayerBindings_t* bindings);

// Load/Save bindings to file
bool InputSystem_LoadConfig(const char* filename);
bool InputSystem_SaveConfig(const char* filename);

// Reset to default bindings
void InputSystem_ResetDefaults(int player);

// Input override for netplay (forces specific input regardless of local state)
void InputSystem_SetOverride(int player, uint16_t input);
void InputSystem_ClearOverride(int player);

// Background input mode - allows reading input even when window is unfocused
void InputSystem_SetBackgroundInputEnabled(bool enabled);
bool InputSystem_IsBackgroundInputEnabled(void);

// Netplay input storage
void InputSystem_SetNetplayInput(int player, uint16_t input);
uint16_t InputSystem_GetNetplayInput(int player);
void InputSystem_ClearNetplayInput(int player);
bool InputSystem_IsNetplayInputActive(int player);

// Check if a gamepad is connected for player (SDL3 Gamepad)
bool InputSystem_HasGamepad(int player);

// Backward compat alias
bool InputSystem_HasXInput(int player);

// Get gamepad name (SDL3 reports actual device name)
const char* InputSystem_GetGamepadName(int player);

// ============================================================================
// Direct Game Buffer Write
// ============================================================================

void InputSystem_WriteToGameBuffers(int player);
void InputSystem_WriteToGameBuffersBothPlayers(void);

// ============================================================================
// Control Swap (P1 <-> P2)
// ============================================================================

void InputSystem_SetControlSwap(bool enabled);
bool InputSystem_GetControlSwap(void);
void InputSystem_ToggleControlSwap(void);

// ============================================================================
// Pause Menu Suppression
// ============================================================================
// During netplay matches, suppress Start button from opening pause menu.
// The mod sets this when rollback is active; the input system strips
// INPUT_START from game buffer writes (just-pressed only) while active.

void InputSystem_SetPauseBlocked(bool blocked);
bool InputSystem_IsPauseBlocked(void);

// ============================================================================
// Unified Binding Capture (for ImGui UI)
// ============================================================================
// Start listening for ANY input (keyboard or gamepad). The next key/button/
// axis pressed is captured. While active, game input is suppressed.

void InputSystem_StartBinding(int player, int buttonIndex);
bool InputSystem_IsBindingActive(void);
// Returns true when a binding was captured. outBinding receives the result.
// outSource: 0=keyboard, 1=gamepad button, 2=gamepad axis
bool InputSystem_FinishBinding(KeyBinding_t* outBinding, int* outSource);
void InputSystem_CancelBinding(void);

// Get binding by index (0-13) from a PlayerBindings_t
KeyBinding_t* InputSystem_GetBindingByIndex(PlayerBindings_t* bindings, int index);
const KeyBinding_t* InputSystem_GetBindingByIndexConst(const PlayerBindings_t* bindings, int index);

// ============================================================================
// Display Name Helpers
// ============================================================================

const char* InputSystem_GetKeyName(int sdlScancode);
const char* InputSystem_GetGamepadButtonName(int sdlGamepadButton);
const char* InputSystem_GetGamepadAxisName(int sdlGamepadAxis, int direction);

#ifdef __cplusplus
}
#endif
