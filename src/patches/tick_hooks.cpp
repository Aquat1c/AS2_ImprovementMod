#include "patches/tick_hooks.h"
#include "log_window.h"

#include <math.h>
#include <string.h>

GetTick_t g_origGetTick = nullptr;

static volatile float g_manualTickScale = 1.0f;
static NetplayTickState g_netplayTickState{};
static float g_lastLoggedEffectiveScale = 1.0f;

static float ClampTickScale(float scale) {
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 32.0f) scale = 32.0f;
    return scale;
}

static float SlewScale(float current, float target) {
    static constexpr float kMaxScaleStepPerFrame = 0.0035f;
    const float delta = target - current;
    if (delta > kMaxScaleStepPerFrame) {
        return current + kMaxScaleStepPerFrame;
    }
    if (delta < -kMaxScaleStepPerFrame) {
        return current - kMaxScaleStepPerFrame;
    }
    return target;
}

static float DesiredNetplayScale() {
    return g_netplayTickState.pacing_active
        ? ClampTickScale(g_netplayTickState.target_scale)
        : 1.0f;
}

static float ComputeEffectiveScale() {
    return ClampTickScale((float)(g_manualTickScale * g_netplayTickState.current_scale));
}

DWORD __cdecl Hook_GetTick() {
    DWORD real = g_origGetTick ? g_origGetTick() : (GetTickCount() & 0x7FFFFFFF);
    real &= 0x7FFFFFFF;

    if (!g_netplayTickState.initialized) {
        g_netplayTickState.initialized = true;
        g_netplayTickState.last_real_tick_ms = real;
        g_netplayTickState.virtual_tick_ms = (double)real;
        g_netplayTickState.current_scale = ClampTickScale(g_netplayTickState.current_scale);
        if (g_netplayTickState.current_scale <= 0.0f) {
            g_netplayTickState.current_scale = 1.0f;
        }
        g_netplayTickState.target_scale = ClampTickScale(g_netplayTickState.target_scale);
        if (g_netplayTickState.target_scale <= 0.0f) {
            g_netplayTickState.target_scale = 1.0f;
        }
    }

    uint32_t realDeltaMs = 0;
    if (real >= g_netplayTickState.last_real_tick_ms) {
        realDeltaMs = real - g_netplayTickState.last_real_tick_ms;
    }
    if (realDeltaMs > 100) {
        realDeltaMs = 100;
    }
    g_netplayTickState.last_real_tick_ms = real;

    g_netplayTickState.current_scale = SlewScale(
        ClampTickScale(g_netplayTickState.current_scale),
        DesiredNetplayScale());

    const float effectiveScale = ComputeEffectiveScale();
    if (fabsf(g_lastLoggedEffectiveScale - effectiveScale) > 0.005f) {
        LOG_INFO(
            "[TickHooks] Effective tick scale changed: %.3fx (manual=%.3fx target=%.3fx current=%.3fx active=%d)",
            effectiveScale,
            g_manualTickScale,
            g_netplayTickState.target_scale,
            g_netplayTickState.current_scale,
            g_netplayTickState.pacing_active ? 1 : 0);
        g_lastLoggedEffectiveScale = effectiveScale;
    }

    g_netplayTickState.virtual_tick_ms += (double)realDeltaMs * (double)effectiveScale;
    return (DWORD)(((uint64_t)floor(g_netplayTickState.virtual_tick_ms)) & 0x7FFFFFFF);
}

void SetGlobalTickScale(float scale) {
    g_manualTickScale = ClampTickScale(scale);
}

float GetGlobalTickScale() {
    return g_manualTickScale;
}

void SetNetplayTickScale(float scale) {
    const float clamped = ClampTickScale(scale);
    g_netplayTickState.current_scale = clamped;
    g_netplayTickState.target_scale = clamped;
    g_netplayTickState.pacing_active = fabsf(clamped - 1.0f) > 0.001f;
}

void SetNetplayTickScaleTarget(float scale) {
    g_netplayTickState.target_scale = ClampTickScale(scale);
}

void SetNetplayPacingActive(bool active) {
    g_netplayTickState.pacing_active = active;
    if (!active) {
        g_netplayTickState.target_scale = 1.0f;
    }
}

void ResetNetplayTickScaleState() {
    memset(&g_netplayTickState, 0, sizeof(g_netplayTickState));
    g_netplayTickState.current_scale = 1.0f;
    g_netplayTickState.target_scale = 1.0f;
    g_lastLoggedEffectiveScale = 1.0f;
}

void GetNetplayTickState(NetplayTickState* out) {
    if (!out) {
        return;
    }

    *out = g_netplayTickState;
}

float GetNetplayTickScale() {
    return g_netplayTickState.current_scale;
}

float GetEffectiveTickScale() {
    return ComputeEffectiveScale();
}
