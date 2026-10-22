/**
 * Alice Senki 2 - Unified Mod Menu Header
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct ImDrawList;

// Menu API
void ModMenu_Init();
void ModMenu_SetOpen(bool open);
void ModMenu_Toggle();
bool ModMenu_IsRequestedOpen();
bool ModMenu_IsOpen();
void ModMenu_Render();

// Draw list that mod overlays (nicknames, HUD, etc.) should render into so the mod menu always
// stays on top: ImGui composites in a fixed order (background -> windows -> foreground), so an
// overlay drawn to the foreground list would cover the menu windows. This returns the background
// draw list while the menu is open (overlay sits above the game but below the menu) and the
// foreground list otherwise (overlay crisp on top of the game). Never null after ImGui init.
ImDrawList* ModMenu_OverlayDrawList();
