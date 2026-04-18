/**
 * Alice Senki 2 - Practice Mode Tools
 *
 * Pause/unpause, single-frame advance, controller swap with CPU flag
 * management, hitbox toggle.  All features gated to training mode
 * (GAMETYPE_TRAINING in MODE_MATCH).
 *
 * Integration points:
 *   - input_sync_hooks.cpp: ShouldSuppressAdvanceFrame checks PracticeTools_ShouldFreezeFrame
 *   - input_override.cpp:   Hook_InputDispatcher returns -1 when practice freeze active
 *   - mod_main.cpp:         ModOnFrame calls PracticeTools_FrameUpdate
 *   - mod_main.cpp:         ModOnPresent calls PracticeTools_RenderHUD
 *   - mod_menu.cpp:         Practice tab calls PracticeTools_RenderImGui
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct PracticeToolsRuntimeState {
	bool paused;
	bool stepRequested;
	uint32_t stepCounter;
};

// Lifecycle
void PracticeTools_Init();
void PracticeTools_Shutdown();

// Called once per frame from ModOnFrame (before input).
// Handles hotkey edge detection, auto-cleanup on mode exit.
void PracticeTools_FrameUpdate();

// ImGui tab contents — drawn inside the mod menu Practice tab.
void PracticeTools_RenderImGui();

// In-game HUD overlay — drawn every present frame via ImGui foreground draw list.
// Shows frame step state, toast notifications, etc.
void PracticeTools_RenderHUD();
bool PracticeTools_HasVisibleHud();

// Freeze query — checked by input_sync_hooks to suppress frame advancement.
// IMPORTANT: This is a pure query — does NOT modify step state.
bool PracticeTools_ShouldFreezeFrame();

// Called by Hook_AdvanceFrame after g_origAdvanceFrame completes.
// Clears the one-shot step flag so the next frame re-freezes.
void PracticeTools_OnFrameAdvanced();

// State queries
bool PracticeTools_IsPracticeModeActive();
bool PracticeTools_IsPaused();
bool PracticeTools_IsControlSwapped();
void PracticeTools_ApplyControlSwapState(bool swapped);
void PracticeTools_SyncControlSwapState();
void PracticeTools_SetPaused(bool paused);
void PracticeTools_CaptureRuntimeState(PracticeToolsRuntimeState* out);
void PracticeTools_RestoreRuntimeState(const PracticeToolsRuntimeState* state);

// Push a toast notification to the in-game HUD.
// Only visible in practice mode. Short messages, auto-fading.
void PracticeTools_Toast(const char* text, unsigned int color);

// --- Command History Hook (redirects to P2 data when controls swapped) ---
typedef char (__cdecl *CmdHistoryUpdate_t)(int16_t* matchBase);
extern CmdHistoryUpdate_t g_origCmdHistoryUpdate;
char __cdecl Hook_CmdHistoryUpdate(int16_t* matchBase);
