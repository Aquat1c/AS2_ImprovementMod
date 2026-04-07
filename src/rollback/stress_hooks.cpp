/**
 * Alice Senki 2 - Online Rollback Stress Hooks Implementation
 */

#include "rollback/stress_hooks.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <stdlib.h>
#include <string.h>

namespace Rollback {

// ============================================================================
// Internal State
// ============================================================================

static bool s_enabled              = false;
static int  s_addedLatencyMs       = 0;
static int  s_jitterMs             = 0;
static int  s_dropPercent          = 0;
static int  s_inputDeliveryDelay   = 0;
static int  s_forcedMismatches     = 0;

// Stats
static int  s_totalDropped         = 0;
static int  s_totalDelayed         = 0;
static int  s_totalMismatchesForced= 0;

// ============================================================================
// Lifecycle
// ============================================================================

void StressHooks_Init() {
    s_enabled = false;
    s_addedLatencyMs = 0;
    s_jitterMs = 0;
    s_dropPercent = 0;
    s_inputDeliveryDelay = 0;
    s_forcedMismatches = 0;
    s_totalDropped = 0;
    s_totalDelayed = 0;
    s_totalMismatchesForced = 0;
}

void StressHooks_Shutdown() {
    s_enabled = false;
}

// ============================================================================
// Configuration (all log changes)
// ============================================================================

void StressHooks_SetEnabled(bool enabled) {
    if (s_enabled != enabled) {
        NetplayLog_Write("STRESS", -1, "StressHooks %s", enabled ? "ENABLED" : "DISABLED");
        LOG_INFO("[StressHooks] %s", enabled ? "ENABLED" : "DISABLED");
    }
    s_enabled = enabled;
}

bool StressHooks_IsEnabled() {
    return s_enabled;
}

void StressHooks_SetAddedLatencyMs(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 500) ms = 500;
    if (ms != s_addedLatencyMs) {
        NetplayLog_ValueChange("STRESS", -1, "added_latency_ms", s_addedLatencyMs, ms, "user set");
        LOG_INFO("[StressHooks] Added latency: %d -> %d ms", s_addedLatencyMs, ms);
    }
    s_addedLatencyMs = ms;
}

int StressHooks_GetAddedLatencyMs() { return s_addedLatencyMs; }

void StressHooks_SetJitterMs(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 200) ms = 200;
    if (ms != s_jitterMs) {
        NetplayLog_ValueChange("STRESS", -1, "jitter_ms", s_jitterMs, ms, "user set");
        LOG_INFO("[StressHooks] Jitter: %d -> %d ms", s_jitterMs, ms);
    }
    s_jitterMs = ms;
}

int StressHooks_GetJitterMs() { return s_jitterMs; }

void StressHooks_SetDropPercent(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct != s_dropPercent) {
        NetplayLog_ValueChange("STRESS", -1, "drop_percent", s_dropPercent, pct, "user set");
        LOG_INFO("[StressHooks] Drop: %d -> %d%%", s_dropPercent, pct);
    }
    s_dropPercent = pct;
}

int StressHooks_GetDropPercent() { return s_dropPercent; }

void StressHooks_ForceNextMismatches(int count) {
    if (count < 0) count = 0;
    NetplayLog_Write("STRESS", -1, "Forcing %d mispredictions", count);
    LOG_INFO("[StressHooks] Forcing %d mispredictions", count);
    s_forcedMismatches = count;
}

int StressHooks_GetRemainingForcedMismatches() { return s_forcedMismatches; }

void StressHooks_SetInputDeliveryDelay(int frames) {
    if (frames < 0) frames = 0;
    if (frames > 30) frames = 30;
    if (frames != s_inputDeliveryDelay) {
        NetplayLog_ValueChange("STRESS", -1, "input_delivery_delay", s_inputDeliveryDelay, frames, "user set");
        LOG_INFO("[StressHooks] Input delivery delay: %d -> %d frames", s_inputDeliveryDelay, frames);
    }
    s_inputDeliveryDelay = frames;
}

int StressHooks_GetInputDeliveryDelay() { return s_inputDeliveryDelay; }

// ============================================================================
// Runtime Queries
// ============================================================================

bool StressHooks_ShouldDropPacket() {
    if (!s_enabled || s_dropPercent <= 0) return false;
    int roll = rand() % 100;
    if (roll < s_dropPercent) {
        s_totalDropped++;
        return true;
    }
    return false;
}

int StressHooks_GetOutgoingDelayMs() {
    if (!s_enabled) return 0;
    int delay = s_addedLatencyMs;
    if (s_jitterMs > 0) {
        int jitter = (rand() % (s_jitterMs * 2 + 1)) - s_jitterMs;
        delay += jitter;
    }
    if (delay < 0) delay = 0;
    if (delay > 0) s_totalDelayed++;
    return delay;
}

int32_t StressHooks_AdjustDeliveryFrame(int32_t input_frame, int32_t current_frame) {
    if (!s_enabled || s_inputDeliveryDelay <= 0) return input_frame;
    // Hold the input — return the adjusted delivery frame
    return input_frame;  // The caller handles the delay queue
}

uint16_t StressHooks_MaybeCorruptPrediction(uint16_t predicted) {
    if (!s_enabled || s_forcedMismatches <= 0) return predicted;
    s_forcedMismatches--;
    s_totalMismatchesForced++;
    // XOR with a non-zero value to guarantee corruption
    uint16_t corrupted = predicted ^ 0x00FF;
    NetplayLog_Write("STRESS", -1, "Corrupted prediction: 0x%04X -> 0x%04X (remaining=%d)",
        predicted, corrupted, s_forcedMismatches);
    return corrupted;
}

// ============================================================================
// Diagnostics
// ============================================================================

void StressHooks_GetSnapshot(StressHooksSnapshot* out) {
    if (!out) return;
    out->enabled = s_enabled;
    out->added_latency_ms = s_addedLatencyMs;
    out->jitter_ms = s_jitterMs;
    out->drop_percent = s_dropPercent;
    out->input_delivery_delay = s_inputDeliveryDelay;
    out->forced_mismatches_remaining = s_forcedMismatches;
    out->total_packets_dropped = s_totalDropped;
    out->total_packets_delayed = s_totalDelayed;
    out->total_mismatches_forced = s_totalMismatchesForced;
}

} // namespace Rollback
