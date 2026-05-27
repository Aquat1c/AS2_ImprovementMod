#include "patches/tick_hooks.h"
#include "log_window.h"

#include <math.h>
#include <string.h>
#include <wchar.h>

GetTick_t g_origGetTick = nullptr;

static volatile float g_manualTickScale = 1.0f;
static volatile bool g_frameLimiter60FpsPreferenceEnabled = true;
static volatile bool g_frameLimiter60FpsSessionOverrideActive = false;
static volatile bool g_frameLimiter60FpsSessionEnabled = true;
static volatile float g_preSessionManualTickScale = 1.0f;
static NetplayTickState g_netplayTickState{};
static float g_lastLoggedEffectiveScale = 1.0f;
static constexpr bool kEnableEffectiveTickScaleLogs = false;
static constexpr float kNativeLimiterFrameMs = 17.0f;
static constexpr float kTarget60FrameMs = 1000.0f / 60.0f;
static constexpr float kFrameLimiter60FpsScale = kNativeLimiterFrameMs / kTarget60FrameMs;
static bool g_tickSettingsLoaded = false;
static bool g_tickSettingsPathResolved = false;
static wchar_t g_tickSettingsPathW[MAX_PATH] = {};
static char g_tickSettingsPathUtf8[MAX_PATH * 3] = {};

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

static bool EffectiveFrameLimiter60FpsEnabled() {
    return g_frameLimiter60FpsSessionOverrideActive
        ? g_frameLimiter60FpsSessionEnabled
        : g_frameLimiter60FpsPreferenceEnabled;
}

static float ComputeEffectiveScale() {
    float scale = (float)(g_manualTickScale * g_netplayTickState.current_scale);
    if (EffectiveFrameLimiter60FpsEnabled()) {
        scale *= kFrameLimiter60FpsScale;
    }
    return ClampTickScale(scale);
}

static void ResolveTickSettingsPath() {
    if (g_tickSettingsPathResolved) {
        return;
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        wcscpy_s(g_tickSettingsPathW, L"as2_rollback_settings.ini");
    } else {
        wchar_t* slash = wcsrchr(path, L'\\');
        wchar_t* fwdSlash = wcsrchr(path, L'/');
        if (!slash || (fwdSlash && fwdSlash > slash)) {
            slash = fwdSlash;
        }
        if (slash) {
            slash[1] = L'\0';
        } else {
            path[0] = L'\0';
        }
        swprintf_s(g_tickSettingsPathW, L"%lsas2_rollback_settings.ini", path);
    }

    g_tickSettingsPathUtf8[0] = '\0';
    WideCharToMultiByte(CP_UTF8,
                        0,
                        g_tickSettingsPathW,
                        -1,
                        g_tickSettingsPathUtf8,
                        sizeof(g_tickSettingsPathUtf8),
                        nullptr,
                        nullptr);
    g_tickSettingsPathResolved = true;
}

static bool ReadIniBool(const wchar_t* section, const wchar_t* key, bool fallback, bool* found) {
    wchar_t value[64] = {};
    ResolveTickSettingsPath();
    GetPrivateProfileStringW(section, key, L"", value, (DWORD)(sizeof(value) / sizeof(value[0])), g_tickSettingsPathW);
    if (value[0] == L'\0') {
        if (found) {
            *found = false;
        }
        return fallback;
    }

    if (found) {
        *found = true;
    }
    return _wcsicmp(value, L"1") == 0 ||
           _wcsicmp(value, L"true") == 0 ||
           _wcsicmp(value, L"yes") == 0 ||
           _wcsicmp(value, L"on") == 0;
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
        if (kEnableEffectiveTickScaleLogs) {
            LOG_INFO(
                "[TickHooks] Effective tick scale changed: %.3fx (manual=%.3fx target=%.3fx current=%.3fx active=%d)",
                effectiveScale,
                g_manualTickScale,
                g_netplayTickState.target_scale,
                g_netplayTickState.current_scale,
                g_netplayTickState.pacing_active ? 1 : 0);
        }
        g_lastLoggedEffectiveScale = effectiveScale;
    }

    g_netplayTickState.virtual_tick_ms += (double)realDeltaMs * (double)effectiveScale;
    return (DWORD)(((uint64_t)floor(g_netplayTickState.virtual_tick_ms)) & 0x7FFFFFFF);
}

void SetGlobalTickScale(float scale) {
    if (g_frameLimiter60FpsSessionOverrideActive) {
        LOG_WARN(
            "[TickHooks] Ignoring manual tick scale change during locked netplay timing: requested=%.3f active=%.3f",
            scale,
            g_manualTickScale);
        return;
    }
    g_manualTickScale = ClampTickScale(scale);
}

float GetGlobalTickScale() {
    return g_manualTickScale;
}

void TickHooks_LoadSettings() {
    if (g_tickSettingsLoaded) {
        return;
    }

    bool found = false;
    g_frameLimiter60FpsPreferenceEnabled =
        ReadIniBool(L"ModSettings", L"proper_60fps", true, &found);
    g_tickSettingsLoaded = true;

    LOG_INFO(
        "[TickHooks] 60fps limiter correction preference: enabled=%d effective=%d scale=%.5f source=%s path=%s",
        g_frameLimiter60FpsPreferenceEnabled ? 1 : 0,
        EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
        kFrameLimiter60FpsScale,
        found ? "ini" : "default",
        g_tickSettingsPathUtf8[0] ? g_tickSettingsPathUtf8 : "as2_rollback_settings.ini");

    if (!found) {
        TickHooks_SaveSettings();
    }
}

void TickHooks_SaveSettings() {
    ResolveTickSettingsPath();
    const wchar_t* value = g_frameLimiter60FpsPreferenceEnabled ? L"1" : L"0";
    if (!WritePrivateProfileStringW(L"ModSettings", L"proper_60fps", value, g_tickSettingsPathW)) {
        LOG_WARN(
            "[TickHooks] Failed to save 60fps limiter setting to %s",
            g_tickSettingsPathUtf8[0] ? g_tickSettingsPathUtf8 : "as2_rollback_settings.ini");
        return;
    }
    LOG_INFO(
        "[TickHooks] Saved 60fps limiter correction preference: enabled=%d effective=%d override=%d path=%s",
        g_frameLimiter60FpsPreferenceEnabled ? 1 : 0,
        EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
        g_frameLimiter60FpsSessionOverrideActive ? 1 : 0,
        g_tickSettingsPathUtf8[0] ? g_tickSettingsPathUtf8 : "as2_rollback_settings.ini");
}

void SetFrameLimiter60FpsPatchEnabled(bool enabled) {
    TickHooks_SetFrameLimiter60FpsPreferenceEnabled(enabled);
}

bool IsFrameLimiter60FpsPatchEnabled() {
    return EffectiveFrameLimiter60FpsEnabled();
}

void TickHooks_SetFrameLimiter60FpsPreferenceEnabled(bool enabled) {
    if (g_frameLimiter60FpsPreferenceEnabled == enabled) {
        return;
    }
    g_frameLimiter60FpsPreferenceEnabled = enabled;
    LOG_INFO(
        "[TickHooks] 60fps limiter correction preference %s (effective=%d override=%d scale=%.5f effective_tick_scale=%.5f)",
        enabled ? "enabled" : "disabled",
        EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
        g_frameLimiter60FpsSessionOverrideActive ? 1 : 0,
        kFrameLimiter60FpsScale,
        ComputeEffectiveScale());
}

bool TickHooks_GetFrameLimiter60FpsPreferenceEnabled() {
    return g_frameLimiter60FpsPreferenceEnabled;
}

void TickHooks_SetFrameLimiter60FpsSessionOverride(bool enabled, const char* reason) {
    const bool hadOverride = g_frameLimiter60FpsSessionOverrideActive;
    const bool previousEffective = EffectiveFrameLimiter60FpsEnabled();
    if (!hadOverride) {
        g_preSessionManualTickScale = g_manualTickScale;
        g_manualTickScale = 1.0f;
    }
    g_frameLimiter60FpsSessionOverrideActive = true;
    g_frameLimiter60FpsSessionEnabled = enabled;

    const bool effective = EffectiveFrameLimiter60FpsEnabled();
    if (!hadOverride || previousEffective != effective) {
        LOG_INFO(
            "[TickHooks] Session FPS timing override: %s enabled=%d previous_effective=%d preference=%d manual_scale_forced=1.000 previous_manual=%.3f scale=%.5f effective_tick_scale=%.5f reason=%s",
            TickHooks_FrameLimiter60FpsLabel(enabled),
            enabled ? 1 : 0,
            previousEffective ? 1 : 0,
            g_frameLimiter60FpsPreferenceEnabled ? 1 : 0,
            g_preSessionManualTickScale,
            kFrameLimiter60FpsScale,
            ComputeEffectiveScale(),
            reason ? reason : "?");
    }
}

void TickHooks_ClearFrameLimiter60FpsSessionOverride(const char* reason) {
    if (!g_frameLimiter60FpsSessionOverrideActive) {
        return;
    }

    const bool previousEffective = EffectiveFrameLimiter60FpsEnabled();
    g_frameLimiter60FpsSessionOverrideActive = false;
    g_frameLimiter60FpsSessionEnabled = g_frameLimiter60FpsPreferenceEnabled;
    g_manualTickScale = ClampTickScale(g_preSessionManualTickScale);
    LOG_INFO(
        "[TickHooks] Cleared session FPS timing override: previous_effective=%d restored=%d preference=%d restored_manual=%.3f effective_tick_scale=%.5f reason=%s",
        previousEffective ? 1 : 0,
        EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
        g_frameLimiter60FpsPreferenceEnabled ? 1 : 0,
        g_manualTickScale,
        ComputeEffectiveScale(),
        reason ? reason : "?");
}

bool TickHooks_IsFrameLimiter60FpsSessionOverrideActive() {
    return g_frameLimiter60FpsSessionOverrideActive;
}

const char* TickHooks_FrameLimiter60FpsLabel(bool enabled) {
    return enabled ? "60.0 FPS" : "58.8 FPS";
}

float GetFrameLimiter60FpsCorrectionScale() {
    return kFrameLimiter60FpsScale;
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
