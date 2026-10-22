#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

// Tick/Timing Hooks Module
// Hooks the game's tick source (sub_635F80). With the FrameScheduler limiter
// detour installed (the normal case), this hook is a PASSTHROUGH pinned to
// scale 1.0: it returns real time to all ~20 consumers
// (movie gate, joystick poll, sound stamps, FPS counter), preserving only the
// 100 ms delta clamp. Speed control (manual slider, replay fast-forward,
// netplay pacing) is owned exclusively by the FrameScheduler period.
//
// If the scheduler failed to install or was disabled via
// `frame_scheduler=0`, the legacy virtual-clock limiter correction
// (17ms → 16.667ms scale) re-engages as the fallback, with reset-is-rebase
// and main-thread confinement applied.
//
// The scheduler is the one speed authority. The 60fps limiter-preference
// getters select the scheduler's cadence profile (proper_60 vs compat_58).

typedef DWORD (__cdecl* GetTick_t)();

extern GetTick_t g_origGetTick;

DWORD __cdecl Hook_GetTick();

struct NetplayTickState {
	bool     initialized;
	uint32_t last_real_tick_ms;
	double   virtual_tick_ms;
	float    current_scale;   // pinned 1.0 (kept for snapshot consumers)
	float    target_scale;    // pinned 1.0 (kept for snapshot consumers)
    bool     pacing_active;   // always false
	char     scale_reason[96];
};

// Manual speed control (applied through the FrameScheduler period when the
// scheduler is installed; through the fallback virtual clock otherwise).
void SetGlobalTickScale(float scale, const char* reason = nullptr);
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
// Rebase-never-reset: preserves virtual_tick_ms continuity and only
// re-anchors last_real_tick_ms — the backward-snap class is
// structurally closed even in the pinned shim.
void ResetNetplayTickScaleState(const char* reason = nullptr);
void GetNetplayTickState(NetplayTickState* out);
float GetNetplayTickScale();
float GetEffectiveTickScale();
