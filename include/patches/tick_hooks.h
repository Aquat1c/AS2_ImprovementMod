#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

// ============================================================================
// Tick/Timing Hooks Module
// ============================================================================
// Hooks the game's tick source (sub_635F80) to allow time scaling.
// Provides global tick scale control for speed adjustment.
// Rollback-specific tick warp paths have been removed.

typedef DWORD (__cdecl* GetTick_t)();

extern GetTick_t g_origGetTick;

DWORD __cdecl Hook_GetTick();

// Tick scale control
void SetGlobalTickScale(float scale);
float GetGlobalTickScale();
void SetNetplayTickScale(float scale);
float GetNetplayTickScale();
float GetEffectiveTickScale();
