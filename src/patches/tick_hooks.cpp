#include "patches/tick_hooks.h"
#include "log_window.h"

#include <math.h>

// Original function pointer (set by hook_installer)
GetTick_t g_origGetTick = nullptr;

// Tick scaling state
static volatile float g_manualTickScale = 1.0f;
static volatile float g_netplayTickScale = 1.0f;
static DWORD g_timeWarpBaseReal = 0;
static double g_timeWarpBaseFake = 0.0;
static float g_timeWarpLastScale = 1.0f;
static bool g_timeWarpInitialized = false;
static float g_lastLoggedEffectiveScale = 1.0f;

extern bool GetVerboseLogging();

static float ClampTickScale(float scale) {
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 32.0f) scale = 32.0f;
    return scale;
}

static float ComputeEffectiveScale() {
    return ClampTickScale((float)(g_manualTickScale * g_netplayTickScale));
}

DWORD __cdecl Hook_GetTick() {
    DWORD real = g_origGetTick ? g_origGetTick() : (GetTickCount() & 0x7FFFFFFF);
    real &= 0x7FFFFFFF;

    const float effectiveScale = ComputeEffectiveScale();
    const bool scaleIsNeutral = fabsf(effectiveScale - 1.0f) <= 0.001f;
    if (fabsf(g_lastLoggedEffectiveScale - effectiveScale) > 0.001f) {
        LOG_INFO("[TickHooks] Effective tick scale changed: %.3fx (manual=%.3fx netplay=%.3fx)",
            effectiveScale, g_manualTickScale, g_netplayTickScale);
        g_lastLoggedEffectiveScale = effectiveScale;
    }

    // Fast path: default gameplay should match the game's original tick source exactly.
    // Do not run through time-warp math unless a non-1.0 scale is actively requested.
    if (scaleIsNeutral) {
        g_timeWarpInitialized = false;
        g_timeWarpLastScale = 1.0f;
        return real;
    }

    if (!g_timeWarpInitialized) {
        g_timeWarpBaseReal = real;
        g_timeWarpBaseFake = (double)real;
        g_timeWarpLastScale = effectiveScale;
        g_timeWarpInitialized = true;
        return real;
    }

    if (fabsf(g_timeWarpLastScale - effectiveScale) > 0.001f) {
        const DWORD delta = (real - g_timeWarpBaseReal) & 0x7FFFFFFF;
        g_timeWarpBaseFake += (double)delta * (double)g_timeWarpLastScale;
        g_timeWarpBaseReal = real;
        g_timeWarpBaseFake = floor(g_timeWarpBaseFake + 0.5);
        g_timeWarpLastScale = effectiveScale;
        return (DWORD)(((uint64_t)(g_timeWarpBaseFake + 0.5)) & 0x7FFFFFFF);
    }

    const DWORD baseReal = (g_timeWarpBaseReal & 0x7FFFFFFF);
    const DWORD delta = (real - baseReal) & 0x7FFFFFFF;

    const double scaledDelta = (double)delta * (double)effectiveScale;
    const uint64_t fake64 = (uint64_t)(g_timeWarpBaseFake + scaledDelta + 0.5);
    return (DWORD)(fake64 & 0x7FFFFFFF);
}

void SetGlobalTickScale(float scale) {
    g_manualTickScale = ClampTickScale(scale);
}

float GetGlobalTickScale() {
    return g_manualTickScale;
}

void SetNetplayTickScale(float scale) {
    g_netplayTickScale = ClampTickScale(scale);
}

float GetNetplayTickScale() {
    return g_netplayTickScale;
}

float GetEffectiveTickScale() {
    return ComputeEffectiveScale();
}
