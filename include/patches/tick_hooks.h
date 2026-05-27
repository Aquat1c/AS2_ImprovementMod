#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

// ============================================================================
// Tick/Timing Hooks Module
// ============================================================================
// Hooks the game's tick source (sub_635F80) to allow time scaling and the
// optional 17ms -> true-60fps limiter correction.
// Provides global tick scale control for speed adjustment.
// Rollback-specific tick warp paths have been removed.

typedef DWORD (__cdecl* GetTick_t)();

extern GetTick_t g_origGetTick;

DWORD __cdecl Hook_GetTick();

struct NetplayTickState {
	bool     initialized;
	uint32_t last_real_tick_ms;
	double   virtual_tick_ms;
	float    current_scale;
	float    target_scale;
	bool     pacing_active;
};

// Tick scale control
void SetGlobalTickScale(float scale);
float GetGlobalTickScale();
void TickHooks_LoadSettings();
void TickHooks_SaveSettings();
void SetFrameLimiter60FpsPatchEnabled(bool enabled);
bool IsFrameLimiter60FpsPatchEnabled();
void TickHooks_SetFrameLimiter60FpsPreferenceEnabled(bool enabled);
bool TickHooks_GetFrameLimiter60FpsPreferenceEnabled();
void TickHooks_SetFrameLimiter60FpsSessionOverride(bool enabled, const char* reason);
void TickHooks_ClearFrameLimiter60FpsSessionOverride(const char* reason);
bool TickHooks_IsFrameLimiter60FpsSessionOverrideActive();
const char* TickHooks_FrameLimiter60FpsLabel(bool enabled);
float GetFrameLimiter60FpsCorrectionScale();
void SetNetplayTickScale(float scale);
void SetNetplayTickScaleTarget(float scale);
void SetNetplayPacingActive(bool active);
void ResetNetplayTickScaleState();
void GetNetplayTickState(NetplayTickState* out);
float GetNetplayTickScale();
float GetEffectiveTickScale();
