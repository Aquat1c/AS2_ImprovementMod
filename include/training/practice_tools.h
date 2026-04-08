/**
 * Alice Senki 2 - Practice Mode Tools
 *
 * Pause/unpause, single-frame advance, controller swap with CPU flag
 * management.  All features are gated to training mode only
 * (GAMETYPE_TRAINING in MODE_MATCH).
 *
 * Integration points:
 *   - input_sync_hooks.cpp: ShouldSuppressAdvanceFrame checks PracticeTools_ShouldFreezeFrame
 *   - input_override.cpp:   Hook_InputDispatcher returns -1 when practice freeze active
 *   - mod_main.cpp:         ModOnFrame calls PracticeTools_FrameUpdate
 *   - mod_menu.cpp:         Practice tab calls PracticeTools_RenderImGui
 */

#pragma once

#include <stdbool.h>

// Lifecycle
void PracticeTools_Init();
void PracticeTools_Shutdown();

// Called once per frame from ModOnFrame (before input).
// Handles hotkey edge detection, auto-cleanup on mode exit.
void PracticeTools_FrameUpdate();

// ImGui tab contents — drawn inside the mod menu Practice tab.
void PracticeTools_RenderImGui();

// Freeze query — checked by input_sync_hooks to suppress frame advancement.
bool PracticeTools_ShouldFreezeFrame();

// State queries
bool PracticeTools_IsPracticeModeActive();
bool PracticeTools_IsPaused();
bool PracticeTools_IsControlSwapped();
