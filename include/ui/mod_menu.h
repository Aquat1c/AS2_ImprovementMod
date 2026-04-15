/**
 * Alice Senki 2 - Unified Mod Menu Header
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Menu API
void ModMenu_Init();
void ModMenu_SetOpen(bool open);
void ModMenu_Toggle();
bool ModMenu_IsRequestedOpen();
bool ModMenu_IsOpen();
void ModMenu_Render();
