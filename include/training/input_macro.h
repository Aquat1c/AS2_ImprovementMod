#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Constants
// ============================================================================

#define MACRO_MAX_SLOTS     8
#define MACRO_MAX_FRAMES    3600    // 60 seconds at 60fps

// ============================================================================
// State
// ============================================================================

enum MacroState : uint8_t {
    MACRO_IDLE = 0,
    MACRO_PRE_RECORD,
    MACRO_RECORDING,
    MACRO_REPLAYING,
};

// ============================================================================
// Public API
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

void InputMacro_Init(void);
void InputMacro_Shutdown(void);

// Called once per game frame (after simulation advances).
// Handles recording capture and playback injection.
void InputMacro_Tick(void);

// State machine transitions (called from hotkey handlers).
void InputMacro_ToggleRecord(void);
void InputMacro_TogglePlay(void);
void InputMacro_NextSlot(void);

// Stop any active recording or playback.
void InputMacro_Stop(void);

// Query
MacroState InputMacro_GetState(void);
int        InputMacro_GetCurrentSlot(void);
int        InputMacro_GetSlotFrameCount(int slot);
bool       InputMacro_SlotHasData(int slot);
int        InputMacro_GetPlaybackFrame(void);

// Savestate integration: cancel any active operation on load.
void InputMacro_OnSavestateLoad(void);

// HUD overlay and ImGui panel.
void InputMacro_RenderOverlay(void);
void InputMacro_RenderImGui(void);

#ifdef __cplusplus
}
#endif
