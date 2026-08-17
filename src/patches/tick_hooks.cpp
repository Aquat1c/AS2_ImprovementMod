#include "patches/tick_hooks.h"
#include "patches/frame_scheduler.h"
#include "log_window.h"
#include "rollback/netplay_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

GetTick_t g_origGetTick = nullptr;

static volatile float g_manualTickScale = 1.0f;
static volatile bool g_frameLimiter60FpsPreferenceEnabled = true;
static volatile bool g_frameLimiter60FpsSessionOverrideActive = false;
static volatile bool g_frameLimiter60FpsSessionEnabled = true;
static volatile float g_preSessionManualTickScale = 1.0f;
static NetplayTickState g_netplayTickState{};
static char g_scaleReason[96] = "initial";
static float g_lastLoggedEffectiveScale = 1.0f;
static bool g_slowSpeedWarned = false;
static DWORD g_lastSlowSpeedLogMs = 0;
static constexpr bool kEnableEffectiveTickScaleLogs = false;
static constexpr float kSlowSpeedThreshold = 0.5f;
static constexpr float kSlowSpeedRecoverThreshold = 0.52f;
static constexpr DWORD kSlowSpeedRepeatLogMs = 5000;
static constexpr float kNativeLimiterFrameMs = 17.0f;
static constexpr float kTarget60FrameMs = 1000.0f / 60.0f;
static constexpr float kFrameLimiter60FpsScale = kNativeLimiterFrameMs / kTarget60FrameMs;
static bool g_tickSettingsLoaded = false;
static bool g_tickSettingsPathResolved = false;
static wchar_t g_tickSettingsPathW[MAX_PATH] = {};
static char g_tickSettingsPathUtf8[MAX_PATH * 3] = {};

// M2 thread confinement (DECOMP §5(c) mandatory fix #2): the accumulator math
// runs only on the game main thread; other callers (the AVI streaming worker,
// DECOMP §2.2) get the last published virtual value — race-free, monotonic,
// and at most one main-loop pass stale.
static volatile DWORD g_tickMainThreadId = 0;
static volatile DWORD g_lastPublishedTick = 0;

static float ClampTickScale(float scale) {
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 32.0f) scale = 32.0f;
    return scale;
}

static bool EffectiveFrameLimiter60FpsEnabled() {
    return g_frameLimiter60FpsSessionOverrideActive
        ? g_frameLimiter60FpsSessionEnabled
        : g_frameLimiter60FpsPreferenceEnabled;
}

// One speed authority (INV-5): with the FrameScheduler limiter detour
// installed, the tick hook is pinned to 1.0 — manual speed and the cadence
// correction are both realized in the scheduler's period instead. Only the
// R-1 fallback (scheduler not installed) keeps the legacy virtual-clock
// scaling alive.
static float ComputeEffectiveScale() {
    if (FrameScheduler_IsInstalled()) {
        return 1.0f;
    }
    float scale = g_manualTickScale;
    if (EffectiveFrameLimiter60FpsEnabled()) {
        scale *= kFrameLimiter60FpsScale;
    }
    return ClampTickScale(scale);
}

static void SetScaleReason(const char* reason) {
    if (!reason || !reason[0]) {
        return;
    }
    strncpy_s(g_scaleReason, reason, _TRUNCATE);
}

static void MaybeWarnSlowEffectiveSpeed(float effectiveScale, DWORD realTickMs) {
    if (effectiveScale < kSlowSpeedThreshold) {
        const bool repeatDue =
            g_slowSpeedWarned &&
            (realTickMs - g_lastSlowSpeedLogMs) >= kSlowSpeedRepeatLogMs;
        if (!g_slowSpeedWarned || repeatDue) {
            LOG_WARN(
                "[TickHooks] Effective game speed below half: effective=%.3fx manual=%.3fx "
                "scheduler_installed=%d limiter_60fps=%d reason=%s",
                effectiveScale,
                g_manualTickScale,
                FrameScheduler_IsInstalled() ? 1 : 0,
                EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
                g_scaleReason);
            Rollback::NetplayLog_Write(
                "SPEED",
                -1,
                "WARN effective speed below half: effective=%.3f manual=%.3f "
                "scheduler_installed=%d limiter_60fps=%d reason=%s",
                effectiveScale,
                g_manualTickScale,
                FrameScheduler_IsInstalled() ? 1 : 0,
                EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
                g_scaleReason);
            g_slowSpeedWarned = true;
            g_lastSlowSpeedLogMs = realTickMs;
        }
        return;
    }

    if (g_slowSpeedWarned && effectiveScale >= kSlowSpeedRecoverThreshold) {
        LOG_INFO(
            "[TickHooks] Effective game speed recovered: effective=%.3fx manual=%.3fx reason=%s",
            effectiveScale,
            g_manualTickScale,
            g_scaleReason);
        Rollback::NetplayLog_Write(
            "SPEED",
            -1,
            "Recovered from slow speed: effective=%.3f manual=%.3f reason=%s",
            effectiveScale,
            g_manualTickScale,
            g_scaleReason);
        g_slowSpeedWarned = false;
    }
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

    // Thread confinement: the accumulator is single-owner. The first caller
    // is the game main thread (DXLib init calls sub_635F80 long before the
    // AVI worker exists, DECOMP §2.2).
    const DWORD tid = GetCurrentThreadId();
    DWORD mainTid = g_tickMainThreadId;
    if (mainTid == 0) {
        g_tickMainThreadId = tid;
        mainTid = tid;
    }
    if (tid != mainTid) {
        const DWORD last = g_lastPublishedTick;
        return last != 0 ? last : real;
    }

    if (!g_netplayTickState.initialized) {
        g_netplayTickState.initialized = true;
        g_netplayTickState.last_real_tick_ms = real;
        g_netplayTickState.virtual_tick_ms = (double)real;
        g_netplayTickState.current_scale = 1.0f;
        g_netplayTickState.target_scale = 1.0f;
        g_netplayTickState.pacing_active = false;
    }

    uint32_t realDeltaMs = 0;
    if (real >= g_netplayTickState.last_real_tick_ms) {
        realDeltaMs = real - g_netplayTickState.last_real_tick_ms;
    }
    if (realDeltaMs > 100) {
        realDeltaMs = 100;
    }
    g_netplayTickState.last_real_tick_ms = real;

    const float effectiveScale = ComputeEffectiveScale();
    MaybeWarnSlowEffectiveSpeed(effectiveScale, real);
    if (fabsf(g_lastLoggedEffectiveScale - effectiveScale) > 0.005f) {
        if (kEnableEffectiveTickScaleLogs) {
            LOG_INFO(
                "[TickHooks] Effective tick scale changed: %.3fx (manual=%.3fx scheduler_installed=%d)",
                effectiveScale,
                g_manualTickScale,
                FrameScheduler_IsInstalled() ? 1 : 0);
        }
        g_lastLoggedEffectiveScale = effectiveScale;
    }

    g_netplayTickState.virtual_tick_ms += (double)realDeltaMs * (double)effectiveScale;
    const DWORD result = (DWORD)(((uint64_t)floor(g_netplayTickState.virtual_tick_ms)) & 0x7FFFFFFF);
    g_lastPublishedTick = result;
    return result;
}

void SetGlobalTickScale(float scale, const char* reason) {
    if (g_frameLimiter60FpsSessionOverrideActive) {
        LOG_WARN(
            "[TickHooks] Ignoring manual tick scale change during locked netplay timing: requested=%.3f active=%.3f reason=%s",
            scale,
            g_manualTickScale,
            reason ? reason : "?");
        return;
    }
    g_manualTickScale = ClampTickScale(scale);
    SetScaleReason(reason ? reason : "manual_global_scale");
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
        "[TickHooks] 60fps cadence preference: enabled=%d effective=%d fallback_scale=%.5f source=%s path=%s",
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
        "[TickHooks] Saved 60fps cadence preference: enabled=%d effective=%d override=%d path=%s",
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
        "[TickHooks] 60fps cadence preference %s (effective=%d override=%d scheduler_installed=%d)",
        enabled ? "enabled" : "disabled",
        EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
        g_frameLimiter60FpsSessionOverrideActive ? 1 : 0,
        FrameScheduler_IsInstalled() ? 1 : 0);
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
    SetScaleReason(reason ? reason : "session_fps_override");

    const bool effective = EffectiveFrameLimiter60FpsEnabled();
    if (!hadOverride || previousEffective != effective) {
        LOG_INFO(
            "[TickHooks] Session FPS timing override: %s enabled=%d previous_effective=%d preference=%d manual_scale_forced=1.000 previous_manual=%.3f scheduler_installed=%d reason=%s",
            TickHooks_FrameLimiter60FpsLabel(enabled),
            enabled ? 1 : 0,
            previousEffective ? 1 : 0,
            g_frameLimiter60FpsPreferenceEnabled ? 1 : 0,
            g_preSessionManualTickScale,
            FrameScheduler_IsInstalled() ? 1 : 0,
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
    SetScaleReason(reason ? reason : "session_fps_override_clear");
    LOG_INFO(
        "[TickHooks] Cleared session FPS timing override: previous_effective=%d restored=%d preference=%d restored_manual=%.3f reason=%s",
        previousEffective ? 1 : 0,
        EffectiveFrameLimiter60FpsEnabled() ? 1 : 0,
        g_frameLimiter60FpsPreferenceEnabled ? 1 : 0,
        g_manualTickScale,
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

// Rebase, never reset (INV-17; DECOMP §2.3 #1 — the backward-snap freeze
// class). `virtual_tick_ms` continuity is preserved: game-side timestamps
// (dword_816360, sound stamps, movie gate, joystick poll stamps) can never
// end up ahead of the returned clock. Only the real-time anchor re-arms so a
// long stall between sessions is not charged as one giant delta (the 100 ms
// clamp already bounds that; this keeps the reason string honest).
void ResetNetplayTickScaleState(const char* reason) {
    if (g_netplayTickState.initialized) {
        const DWORD real = g_origGetTick
            ? (g_origGetTick() & 0x7FFFFFFF)
            : (GetTickCount() & 0x7FFFFFFF);
        g_netplayTickState.last_real_tick_ms = real;
        // virtual_tick_ms deliberately preserved.
    }
    g_netplayTickState.current_scale = 1.0f;
    g_netplayTickState.target_scale = 1.0f;
    g_netplayTickState.pacing_active = false;
    g_lastLoggedEffectiveScale = 1.0f;
    g_slowSpeedWarned = false;
    g_lastSlowSpeedLogMs = 0;
    SetScaleReason(reason ? reason : "netplay_scale_rebase");
}

void GetNetplayTickState(NetplayTickState* out) {
    if (!out) {
        return;
    }

    *out = g_netplayTickState;
    strncpy_s(out->scale_reason, g_scaleReason, _TRUNCATE);
}

float GetNetplayTickScale() {
    // Netplay tick scaling was deleted at M2 (INV-5): the scheduler owns pace.
    return 1.0f;
}

float GetEffectiveTickScale() {
    if (FrameScheduler_IsInstalled()) {
        return FrameScheduler_GetSpeedScale();
    }
    return ComputeEffectiveScale();
}
