#pragma once

#include <stdbool.h>

// ============================================================================
// Hook Installer Module
// ============================================================================
// Installs only the non-rollback hooks needed for the clean active build:
//  - Input override hooks (keyboard, joystick, DInput refresh, input process)
//  - Win32 GetKeyboardState (Alt+Shift prevention)
//  - Locale/codepage patches (GetOEMCP, GetACP)
//  - Filesystem Shift-JIS path hooks (CreateFileA, etc.)
//  - MultiByteToWideChar CP redirect
//  - Tick/timing hook (GetTick)
//  - Character palette asset-load hook
//
// Does NOT install: RNG hooks, rollback hooks, savestate hooks, audio/render hooks.

bool InstallHooks();
void RemoveHooks();
