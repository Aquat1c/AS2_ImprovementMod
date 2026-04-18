#pragma once

#include "input_system.h"

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Hotkey Action IDs
// ============================================================================

enum HotkeyAction : int {
    HOTKEY_HITBOX_TOGGLE = 0,
    HOTKEY_PAUSE_TOGGLE,
    HOTKEY_FRAME_STEP,
    HOTKEY_CONTROL_SWAP,
    HOTKEY_POSITION_LOAD,
    HOTKEY_POSITION_SAVE,
    HOTKEY_MACRO_RECORD,
    HOTKEY_MACRO_PLAY,
    HOTKEY_MACRO_SLOT_NEXT,
    HOTKEY_COUNT
};

// ============================================================================
// Public API
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

void HotkeyConfig_Init(void);

// Returns the binding for an action.
const KeyBinding_t* HotkeyConfig_GetBinding(HotkeyAction action);

// Sets the full keyboard/controller binding for an action.
void HotkeyConfig_SetBinding(HotkeyAction action, const KeyBinding_t* binding);

// Formats a display name for an action's current binding.
void HotkeyConfig_GetBindingDisplayName(HotkeyAction action, char* out, int outSize);

// Returns a display name for a virtual key code (e.g. "F4", "A", "Numpad0").
const char* HotkeyConfig_KeyName(int vk);

// Returns the display label for an action (e.g. "Hitbox Toggle").
const char* HotkeyConfig_ActionName(HotkeyAction action);

// Returns true if training hotkeys should be ignored this frame.
bool HotkeyConfig_IsSuppressed(void);

// Edge-detected key press query. Call once per frame for each action.
// Returns true on the frame a key transitions from up to down.
bool HotkeyConfig_JustPressed(HotkeyAction action);

// Call once per frame to update edge detection state.
void HotkeyConfig_Update(void);

// ImGui widget for rebinding all hotkeys.
void HotkeyConfig_RenderImGui(void);

#ifdef __cplusplus
}
#endif
