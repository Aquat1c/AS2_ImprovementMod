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
    HOTKEY_STATE_SAVE,
    HOTKEY_STATE_LOAD,
    HOTKEY_POSITION_LOAD,
    HOTKEY_POSITION_SAVE,
    HOTKEY_MACRO_RECORD,
    HOTKEY_MACRO_PLAY,
    HOTKEY_MACRO_SLOT_NEXT,
    HOTKEY_OVERLAY_TOGGLE,
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

// Builds a UI label like "Step (F8)", or just "Step" when nothing is bound.
// Anywhere a key name is shown, use this rather than writing the default in.
void HotkeyConfig_FormatLabel(char* out, int outSize, const char* text,
                              HotkeyAction action);

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

// ----------------------------------------------------------------------------
// Rebinding, for UIs that are not ImGui (the in-game settings page).
// ----------------------------------------------------------------------------

// Starts listening. The next key or P1 pad input pressed becomes the binding.
void HotkeyConfig_BeginRebind(HotkeyAction action);

// Poll once per frame while rebinding: 1 captured, 0 still waiting, -1 stopped.
int  HotkeyConfig_PollRebind(void);

void HotkeyConfig_CancelRebind(void);
bool HotkeyConfig_IsRebinding(void);

// The action currently being rebound, or -1.
int  HotkeyConfig_RebindAction(void);

void HotkeyConfig_ClearBinding(HotkeyAction action);
void HotkeyConfig_ResetDefaults(void);

// Savestates work in arcade and versus too, so those actions are not gated on
// training; everything else is practice-only.
bool HotkeyConfig_ActionIsPracticeOnly(HotkeyAction action);

#ifdef __cplusplus
}
#endif
