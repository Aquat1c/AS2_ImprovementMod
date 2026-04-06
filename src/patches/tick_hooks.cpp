#include "patches/tick_hooks.h"
#include "log_window.h"

// Original function pointer (set by hook_installer)
GetTick_t g_origGetTick = nullptr;

// Tick scaling state
static volatile float g_globalTickScale = 1.0f;
static DWORD g_timeWarpBaseReal = 0;
static DWORD g_timeWarpBaseFake = 0;
static float g_timeWarpLastScale = 1.0f;

extern bool GetVerboseLogging();

DWORD __cdecl Hook_GetTick() {
    DWORD real = g_origGetTick ? g_origGetTick() : (GetTickCount() & 0x7FFFFFFF);
    real &= 0x7FFFFFFF;

    float effectiveScale = g_globalTickScale;

    if (effectiveScale <= 1.0f) {
        g_timeWarpLastScale = 1.0f;
        return real;
    }

    if (g_timeWarpLastScale != effectiveScale) {
        g_timeWarpBaseReal = real;
        g_timeWarpBaseFake = real;
        g_timeWarpLastScale = effectiveScale;
        return real;
    }

    const DWORD baseReal = (g_timeWarpBaseReal & 0x7FFFFFFF);
    const DWORD baseFake = (g_timeWarpBaseFake & 0x7FFFFFFF);
    const DWORD delta = (real - baseReal) & 0x7FFFFFFF;

    const double scaledDelta = (double)delta * (double)effectiveScale;
    const uint64_t fake64 = (uint64_t)baseFake + (uint64_t)(scaledDelta + 0.5);
    return (DWORD)(fake64 & 0x7FFFFFFF);
}

void SetGlobalTickScale(float scale) {
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 32.0f) scale = 32.0f;
    g_globalTickScale = scale;
    g_timeWarpLastScale = 0.0f;
}

float GetGlobalTickScale() {
    return g_globalTickScale;
}
