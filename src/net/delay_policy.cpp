/**
 * Alice Senki 2 - Delay Policy Implementation
 *
 * Rollback-first recommendation model with a jitter guard:
 *   guarded = (avg_ping_ms / 2 + rtt_variance_ms) / 16.667
 *   delay   = max(0, ceil(guarded) - target_prediction_frames)
 *   max_rb  = max(4, ceil(guarded) - delay + 2)
 *
 * Shared-safe mode is the default: both peers apply the larger visible delay.
 * Asymmetric expert mode preserves the older local-only behavior.
 */

#include "net/delay_policy.h"

#include "net/session_manager.h"
#include "rollback/rollback_session.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <algorithm>
#include <math.h>
#include <string.h>

namespace Net {

namespace {

static int   s_configuredDelay          = DELAY_DEFAULT_PREF;
static int   s_rollbackBudget           = ROLLBACK_BUDGET_DEFAULT;
static int   s_rollbackToleranceK       = ROLLBACK_TOLERANCE_DEFAULT;

static int   s_remoteAnnouncedDelay     = 0;
static int   s_remoteAnnouncedMaxRb     = ROLLBACK_BUDGET_DEFAULT;
static GameplayDelayMode s_gameplayDelayMode = GameplayDelayMode::SharedSafe;
static int   s_resolvedVisibleLocalDelay = DELAY_DEFAULT_PREF;
static int   s_resolvedVisibleRemoteDelay = DELAY_DEFAULT_PREF;
static int   s_effectiveLocalDelay      = DELAY_DEFAULT_PREF + kHiddenGameplayDelayFloor;
static int   s_effectiveRemoteDelay     = kHiddenGameplayDelayFloor;
static int   s_connectionProtectionWindow =
    DELAY_DEFAULT_PREF + kHiddenGameplayDelayFloor + ROLLBACK_BUDGET_DEFAULT;
static int   s_stallThreshold           = ROLLBACK_BUDGET_DEFAULT;

static float s_avgPingMs                = 0.0f;
static float s_lastVarianceMs           = 0.0f;
static float s_oneWayFrames             = 0.0f;
static bool  s_measurementValid         = false;
static int   s_recommendedDelay         = 0;
static int   s_recommendedMaxRollback   = ROLLBACK_BUDGET_MIN;

static int   s_activeDelay              = DELAY_DEFAULT_PREF;
static int   s_rollbackCurrentDelay     = -1;
static bool  s_rollbackSynced           = false;
static bool  s_initialized              = false;

static int ClampDelay(int delay) {
    return (std::max)(DELAY_MIN, (std::min)(DELAY_MAX, delay));
}

static int ClampRollback(int frames) {
    return (std::max)(ROLLBACK_BUDGET_MIN, (std::min)(ROLLBACK_BUDGET_MAX, frames));
}

static int ClampTolerance(int value) {
    return (std::max)(ROLLBACK_TOLERANCE_MIN,
                      (std::min)(ROLLBACK_TOLERANCE_MAX, value));
}

static void RecomputeRecommendations() {
    const float guardedOneWayFrames =
        ((s_avgPingMs * 0.5f) + (std::max)(0.0f, s_lastVarianceMs)) / FRAME_TIME_MS;
    s_oneWayFrames = guardedOneWayFrames;

    constexpr int targetPredictionFrames = 2;
    const int ceilFrames = (int)ceilf(guardedOneWayFrames);
    const int delay = (std::max)(0, ceilFrames - targetPredictionFrames);
    const int maxRollback = (std::max)(ROLLBACK_BUDGET_MIN, ceilFrames - delay + 2);

    s_recommendedDelay = ClampDelay(delay);
    s_recommendedMaxRollback = ClampRollback(maxRollback);
}

static void RecomputeDerivedLocalState() {
    const int localConfigured = ClampDelay(s_configuredDelay);
    const int remoteAnnounced = ClampDelay(s_remoteAnnouncedDelay);

    if (s_gameplayDelayMode == GameplayDelayMode::SharedSafe) {
        const int shared = ClampDelay((std::max)(localConfigured, remoteAnnounced));
        s_activeDelay = shared;
        s_resolvedVisibleLocalDelay = shared;
        s_resolvedVisibleRemoteDelay = shared;
    } else {
        s_activeDelay = localConfigured;
        s_resolvedVisibleLocalDelay = localConfigured;
        s_resolvedVisibleRemoteDelay = remoteAnnounced;
    }

    s_effectiveLocalDelay = s_resolvedVisibleLocalDelay + kHiddenGameplayDelayFloor;
    s_effectiveRemoteDelay = s_resolvedVisibleRemoteDelay + kHiddenGameplayDelayFloor;
    s_connectionProtectionWindow = s_effectiveLocalDelay + s_rollbackBudget;
    s_stallThreshold = s_effectiveRemoteDelay + s_rollbackBudget;
}

static void LogGameplayDelayState(const char* reason) {
    const float localPredictsRemote =
        (std::max)(0.0f, s_oneWayFrames - (float)s_resolvedVisibleRemoteDelay);
    const float remotePredictsLocal =
        (std::max)(0.0f, s_oneWayFrames - (float)s_resolvedVisibleLocalDelay);
    const bool expert = s_gameplayDelayMode == GameplayDelayMode::AsymmetricExpert;

    LOG_INFO(
        "[DelayPolicy] %s: policy=%s local_cfg=%d remote_cfg=%d resolved_local=%d resolved_remote=%d local_eff=%d remote_eff=%d max_rb=%d protection_window=%d stall_threshold=%d%s",
        reason ? reason : "delay state",
        GameplayDelayModeName(s_gameplayDelayMode),
        s_configuredDelay,
        s_remoteAnnouncedDelay,
        s_resolvedVisibleLocalDelay,
        s_resolvedVisibleRemoteDelay,
        s_effectiveLocalDelay,
        s_effectiveRemoteDelay,
        s_rollbackBudget,
        s_connectionProtectionWindow,
        s_stallThreshold,
        expert ? " WARNING=asymmetric_delay" : "");

    Rollback::NetplayLog_Write(
        "DELAY", -1,
        "DELAYMAP policy=%s local_cfg=%d remote_cfg=%d resolved_local=%d resolved_remote=%d local_eff=%d remote_eff=%d rtt_avg=%.1f rtt_var=%.1f one_way_guarded=%.2f recommend=%d max_rb=%d protection_window=%d stall_threshold=%d local_predicts_remote=%.1f remote_predicts_local=%.1f%s reason=%s",
        GameplayDelayModeName(s_gameplayDelayMode),
        s_configuredDelay,
        s_remoteAnnouncedDelay,
        s_resolvedVisibleLocalDelay,
        s_resolvedVisibleRemoteDelay,
        s_effectiveLocalDelay,
        s_effectiveRemoteDelay,
        s_avgPingMs,
        s_lastVarianceMs,
        s_oneWayFrames,
        s_recommendedDelay,
        s_recommendedMaxRollback,
        s_connectionProtectionWindow,
        s_stallThreshold,
        localPredictsRemote,
        remotePredictsLocal,
        expert ? " WARNING=asymmetric_delay" : "",
        reason ? reason : "delay state");
}

static void LogRecommendationUpdate(int prevDelay,
                                    int prevRollback,
                                    float prevPingMs) {
    if (prevDelay == s_recommendedDelay &&
        prevRollback == s_recommendedMaxRollback &&
        prevPingMs == s_avgPingMs) {
        return;
    }

    LOG_INFO(
        "[DelayPolicy] Recommendations: ping=%.1fms variance=%.1fms jitter=%.2ff guarded_one_way=%.2ff target_pred=2 delay=%d max_rb=%d",
        s_avgPingMs,
        s_lastVarianceMs,
        s_lastVarianceMs / FRAME_TIME_MS,
        s_oneWayFrames,
        s_recommendedDelay,
        s_recommendedMaxRollback);

    Rollback::NetplayLog_Write(
        "DELAY", -1,
        "Recommendations updated: ping=%.1fms variance=%.1fms jitter=%.2ff guarded_one_way=%.2ff target_pred=2 delay=%d max_rb=%d",
        s_avgPingMs,
        s_lastVarianceMs,
        s_lastVarianceMs / FRAME_TIME_MS,
        s_oneWayFrames,
        s_recommendedDelay,
        s_recommendedMaxRollback);
}

} // namespace

void DelayPolicy_Init() {
    s_avgPingMs = 0.0f;
    s_lastVarianceMs = 0.0f;
    s_oneWayFrames = 0.0f;
    s_measurementValid = false;
    s_remoteAnnouncedDelay = 0;
    s_remoteAnnouncedMaxRb = ROLLBACK_BUDGET_DEFAULT;
    s_gameplayDelayMode = GameplayDelayMode::SharedSafe;
    s_rollbackCurrentDelay = -1;
    s_rollbackSynced = false;
    RecomputeRecommendations();
    RecomputeDerivedLocalState();
    s_initialized = true;

    LOG_INFO(
        "[DelayPolicy] Initialized: delay=%d effective=%d max_rb=%d tolerance=%d protection_window=%d stall_threshold=%d",
        s_configuredDelay,
        s_effectiveLocalDelay,
        s_rollbackBudget,
        s_rollbackToleranceK,
        s_connectionProtectionWindow,
        s_stallThreshold);
}

void DelayPolicy_Shutdown() {
    s_initialized = false;
    LOG_INFO("[DelayPolicy] Shutdown");
}

void DelayPolicy_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    if (Rollback::RollbackSession_IsActive()) {
        Rollback::RollbackTimesyncTelemetry telemetry{};
        Rollback::RollbackSession_GetTimesyncTelemetry(&telemetry);
        if (telemetry.gekko_avg_ping > 0.0f) {
            DelayPolicy_UpdateFromStats(telemetry.gekko_avg_ping);
            return;
        }
    }

    if (Session_IsConnected()) {
        ConnectionStats stats{};
        Session_GetStats(&stats);
        DelayPolicy_UpdateMeasurement(stats.rtt_ms, stats.rtt_variance_ms);
    }
}

void DelayPolicy_UpdateMeasurement(float rtt_ms, float rtt_variance_ms) {
    s_lastVarianceMs = rtt_variance_ms;
    DelayPolicy_UpdateFromStats(rtt_ms);
}

void DelayPolicy_UpdateFromStats(float avg_ping_ms) {
    if (!s_initialized) {
        return;
    }

    if (avg_ping_ms < 0.0f) {
        avg_ping_ms = 0.0f;
    }

    const int prevDelay = s_recommendedDelay;
    const int prevRollback = s_recommendedMaxRollback;
    const float prevPing = s_avgPingMs;

    s_avgPingMs = avg_ping_ms;
    s_measurementValid = true;
    RecomputeRecommendations();
    LogRecommendationUpdate(prevDelay, prevRollback, prevPing);
}

int DelayPolicy_ComputeRecommendedDelay() {
    return s_recommendedDelay;
}

int DelayPolicy_ComputeRecommendedMaxRollback() {
    return s_recommendedMaxRollback;
}

void DelayPolicy_GetMeasurement(NetworkMeasurement* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->avg_ping_ms = s_avgPingMs;
    out->rtt_variance_ms = s_lastVarianceMs;
    out->one_way_frames = s_oneWayFrames;
    out->jitter_frames = s_lastVarianceMs / FRAME_TIME_MS;
    out->recommended_delay = s_recommendedDelay;
    out->recommended_max_rollback = s_recommendedMaxRollback;
    out->valid = s_measurementValid;
}

void DelayPolicy_SetConfiguredDelay(int delay) {
    const int clamped = ClampDelay(delay);
    if (s_configuredDelay == clamped) {
        return;
    }

    const int prev = s_configuredDelay;
    s_configuredDelay = clamped;
    RecomputeDerivedLocalState();
    s_rollbackSynced = false;

    LOG_INFO("[DelayPolicy] Input delay: %d -> %d", prev, s_configuredDelay);
    LogGameplayDelayState("local delay changed");
}

int DelayPolicy_GetConfiguredDelay() {
    return s_configuredDelay;
}

void DelayPolicy_SetRollbackBudget(int frames) {
    const int clamped = ClampRollback(frames);
    if (s_rollbackBudget == clamped) {
        return;
    }

    const int prev = s_rollbackBudget;
    s_rollbackBudget = clamped;
    RecomputeDerivedLocalState();

    LOG_INFO(
        "[DelayPolicy] Max rollback: %d -> %d (effective=%d protection_window=%d stall_threshold=%d)",
        prev,
        s_rollbackBudget,
        s_effectiveLocalDelay,
        s_connectionProtectionWindow,
        s_stallThreshold);
    LogGameplayDelayState("rollback budget changed");
}

int DelayPolicy_GetRollbackBudget() {
    return s_rollbackBudget;
}

void DelayPolicy_SetRollbackToleranceK(int tolerance_k) {
    const int clamped = ClampTolerance(tolerance_k);
    if (s_rollbackToleranceK == clamped) {
        return;
    }

    const int prev = s_rollbackToleranceK;
    s_rollbackToleranceK = clamped;

    const int prevDelay = s_recommendedDelay;
    const int prevRollback = s_recommendedMaxRollback;
    RecomputeRecommendations();

    LOG_INFO("[DelayPolicy] RB bias (rollback_tolerance K): %d -> %d (delay=%d max_rb=%d)",
             prev, s_rollbackToleranceK, s_recommendedDelay, s_recommendedMaxRollback);
    LogRecommendationUpdate(prevDelay, prevRollback, s_avgPingMs);
}

int DelayPolicy_GetRollbackToleranceK() {
    return s_rollbackToleranceK;
}

void DelayPolicy_SetGameplayDelayMode(GameplayDelayMode mode) {
    if (mode != GameplayDelayMode::SharedSafe &&
        mode != GameplayDelayMode::AsymmetricExpert) {
        mode = GameplayDelayMode::SharedSafe;
    }
    if (s_gameplayDelayMode == mode) {
        return;
    }

    const GameplayDelayMode prev = s_gameplayDelayMode;
    s_gameplayDelayMode = mode;
    RecomputeDerivedLocalState();
    s_rollbackSynced = false;

    LOG_INFO("[DelayPolicy] Gameplay delay mode: %s -> %s",
             GameplayDelayModeName(prev),
             GameplayDelayModeName(s_gameplayDelayMode));
    LogGameplayDelayState("gameplay delay mode changed");
}

GameplayDelayMode DelayPolicy_GetGameplayDelayMode() {
    return s_gameplayDelayMode;
}

void DelayPolicy_BuildNegotiationData(DelayNegotiationData* out) {
    if (!out) {
        return;
    }

    out->local_input_delay = s_configuredDelay;
    out->max_rollback = s_rollbackBudget;
    out->gameplay_delay_mode = s_gameplayDelayMode;
}

void DelayPolicy_NegotiateSession(const DelayNegotiationData* remote) {
    if (!remote) {
        return;
    }

    s_remoteAnnouncedDelay = ClampDelay(remote->local_input_delay);
    s_remoteAnnouncedMaxRb = ClampRollback(remote->max_rollback);
    if (remote->gameplay_delay_mode == GameplayDelayMode::SharedSafe ||
        remote->gameplay_delay_mode == GameplayDelayMode::AsymmetricExpert) {
        s_gameplayDelayMode = remote->gameplay_delay_mode;
    }
    RecomputeDerivedLocalState();

    LOG_INFO(
        "[DelayPolicy] Remote config received: policy=%s remote_delay=%d remote_max_rb=%d local_delay=%d resolved_delay=%d effective_delay=%d local_max_rb=%d protection_window=%d stall_threshold=%d",
        GameplayDelayModeName(s_gameplayDelayMode),
        s_remoteAnnouncedDelay,
        s_remoteAnnouncedMaxRb,
        s_configuredDelay,
        s_resolvedVisibleLocalDelay,
        s_effectiveLocalDelay,
        s_rollbackBudget,
        s_connectionProtectionWindow,
        s_stallThreshold);

    LogGameplayDelayState("Startup config");
}

int DelayPolicy_GetActiveDelay() {
    return s_activeDelay;
}

int DelayPolicy_GetEffectiveLocalDelay() {
    return s_effectiveLocalDelay;
}

int DelayPolicy_GetEffectiveRemoteDelay() {
    return s_effectiveRemoteDelay;
}

int DelayPolicy_GetRemoteAnnouncedDelay() {
    return s_remoteAnnouncedDelay;
}

int DelayPolicy_GetRemoteAnnouncedMaxRollback() {
    return s_remoteAnnouncedMaxRb;
}

int DelayPolicy_GetProtectionWindow() {
    return s_connectionProtectionWindow;
}

int DelayPolicy_GetStallThreshold() {
    return s_stallThreshold;
}

bool DelayPolicy_IsRollbackSynced() {
    return s_rollbackSynced;
}

void DelayPolicy_OnRollbackApplied(int delay_value) {
    s_rollbackCurrentDelay = delay_value;
    s_rollbackSynced = (delay_value == s_effectiveLocalDelay);
}

void DelayPolicy_ResetSession() {
    s_avgPingMs = 0.0f;
    s_lastVarianceMs = 0.0f;
    s_oneWayFrames = 0.0f;
    s_measurementValid = false;
    s_remoteAnnouncedDelay = 0;
    s_remoteAnnouncedMaxRb = ROLLBACK_BUDGET_DEFAULT;
    s_resolvedVisibleLocalDelay = ClampDelay(s_configuredDelay);
    s_resolvedVisibleRemoteDelay = DELAY_DEFAULT_PREF;
    s_rollbackCurrentDelay = -1;
    s_rollbackSynced = false;
    RecomputeRecommendations();
    RecomputeDerivedLocalState();

    LOG_INFO(
        "[DelayPolicy] Session reset: delay=%d effective=%d max_rb=%d tolerance=%d protection_window=%d stall_threshold=%d",
        s_configuredDelay,
        s_effectiveLocalDelay,
        s_rollbackBudget,
        s_rollbackToleranceK,
        s_connectionProtectionWindow,
        s_stallThreshold);
}

void DelayPolicy_GetSnapshot(DelayPolicySnapshot* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->configured_delay = s_configuredDelay;
    out->active_delay = s_activeDelay;
    out->resolved_visible_local_delay = s_resolvedVisibleLocalDelay;
    out->resolved_visible_remote_delay = s_resolvedVisibleRemoteDelay;
    out->effective_local_delay = s_effectiveLocalDelay;
    out->effective_remote_delay = s_effectiveRemoteDelay;
    out->protection_window = s_connectionProtectionWindow;
    out->rollback_budget = s_rollbackBudget;
    out->rollback_tolerance = s_rollbackToleranceK;
    out->gameplay_delay_mode = s_gameplayDelayMode;
    out->recommended_delay = s_recommendedDelay;
    out->recommended_max_rollback = s_recommendedMaxRollback;
    out->remote_announced_delay = s_remoteAnnouncedDelay;
    out->remote_announced_max_rollback = s_remoteAnnouncedMaxRb;
    out->stall_threshold = s_stallThreshold;
    out->measured_avg_ping_ms = s_avgPingMs;
    out->measured_rtt_variance_ms = s_lastVarianceMs;
    out->measured_one_way_frames = s_oneWayFrames;
    out->measured_jitter_frames = s_lastVarianceMs / FRAME_TIME_MS;
    out->measurement_valid = s_measurementValid;
    out->rollback_synced = s_rollbackSynced;
    out->rollback_current_delay = s_rollbackCurrentDelay;
}

void DelayPolicy_LogDelayMap(const char* reason) {
    LogGameplayDelayState(reason);
}

} // namespace Net
