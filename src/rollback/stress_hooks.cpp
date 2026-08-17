/**
 * Alice Senki 2 - Online Rollback Stress Hooks Implementation
 */

#include "rollback/stress_hooks.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
static int  s_forcedRollbackDepth  = 0;

// Stats
static int  s_totalDropped         = 0;
static int  s_totalDelayed         = 0;
static int  s_totalMismatchesForced= 0;

// ============================================================================
// Lifecycle
// ============================================================================

void StressHooks_Init() {
    // Settings-file arming (NetMenu::Init) runs BEFORE this init in the boot
    // sequence — zeroing unconditionally wiped forced_rollback= on every
    // launch (only env-var launches survived, because the env re-arms below).
    const int preArmedDepth = s_forcedRollbackDepth;
    const bool preArmedEnabled = s_enabled && preArmedDepth > 0;
    s_enabled = false;
    s_addedLatencyMs = 0;
    s_jitterMs = 0;
    s_dropPercent = 0;
    s_inputDeliveryDelay = 0;
    s_forcedMismatches = 0;
    s_forcedRollbackDepth = 0;
    if (preArmedEnabled) {
        s_enabled = true;
        s_forcedRollbackDepth = preArmedDepth;
    }
    s_totalDropped = 0;
    s_totalDelayed = 0;
    s_totalMismatchesForced = 0;

    // Deep-rollback acceptance cells (2026-08-17): AS2_STRESS_DELIVERY_DELAY=N
    // arms the hooks at launch with an N-frame input delivery delay so the
    // two-instance loopback pair sustains real prediction depth ~N (forcing
    // deep restore/replay under combat without a WAN shim). Menu toggles
    // still work on top.
    // Dedicated stress config: as2_stress.cfg in the game directory. The
    // settings ini is rewritten (and its tail corrupted) by the game's own
    // save path, and env vars never reach user-launched sessions — this file
    // is touched by nobody but the user/harness. Format: forced_rollback=N
    {
        FILE* sf = nullptr;
        if (fopen_s(&sf, "as2_stress.cfg", "r") == 0 && sf) {
            char line[128];
            while (fgets(line, sizeof(line), sf)) {
                int depth = 0;
                if (sscanf_s(line, "forced_rollback=%d", &depth) == 1 && depth > 0) {
                    s_enabled = true;
                    s_forcedRollbackDepth = depth > 48 ? 48 : depth;
                }
                int dd = 0;
                if (sscanf_s(line, "delivery_delay=%d", &dd) == 1 && dd > 0) {
                    s_enabled = true;
                    s_inputDeliveryDelay = dd;
                }
            }
            fclose(sf);
            if (s_forcedRollbackDepth > 0) {
                LOG_INFO("[StressHooks] as2_stress.cfg armed: forced_rollback=%d",
                         s_forcedRollbackDepth);
            }
        }
    }

    char env[16] = {};
    if (GetEnvironmentVariableA("AS2_STRESS_DELIVERY_DELAY", env, sizeof(env)) > 0) {
        const int frames = atoi(env);
        if (frames > 0) {
            s_enabled = true;
            s_inputDeliveryDelay = frames > 15 ? 15 : frames;
            LOG_INFO("[StressHooks] Env-armed: delivery_delay=%d frames (AS2_STRESS_DELIVERY_DELAY)",
                     s_inputDeliveryDelay);
        }
    }

    // AS2_STRESS_FORCED_ROLLBACK=N (alias: AS2_FORCE_ROLLBACK=N) arms
    // per-frame forced depth-N rollback transactions (engine
    // SetForcedRollback passthrough): every advanced frontier performs a
    // genuine depth-N restore/replay during live combat. The adapter emits
    // a per-second [FORCED] evidence line with the executed transaction
    // count and achieved depths, and logs "[FORCED_RB] depth=N ACTIVE" at
    // every session arm/rotate.
    char envFr[16] = {};
    if (GetEnvironmentVariableA("AS2_STRESS_FORCED_ROLLBACK", envFr, sizeof(envFr)) == 0) {
        GetEnvironmentVariableA("AS2_FORCE_ROLLBACK", envFr, sizeof(envFr));
    }
    if (envFr[0] != '\0') {
        const int depth = atoi(envFr);
        if (depth > 0) {
            s_enabled = true;
            s_forcedRollbackDepth = depth > 48 ? 48 : depth;
            LOG_INFO("[StressHooks] Env-armed: forced_rollback_depth=%d per frame (AS2_STRESS_FORCED_ROLLBACK/AS2_FORCE_ROLLBACK)",
                     s_forcedRollbackDepth);
        }
    }
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

void StressHooks_SetForcedRollbackDepth(int depth) {
    if (depth < 0) depth = 0;
    if (depth > 48) depth = 48;  // engine cap (SetForcedRollback, ring-bound)
    if (depth != s_forcedRollbackDepth) {
        NetplayLog_ValueChange("STRESS", -1, "forced_rollback_depth",
            s_forcedRollbackDepth, depth, "user set");
        LOG_INFO("[StressHooks] Forced rollback depth: %d -> %d (every frame)",
            s_forcedRollbackDepth, depth);
    }
    s_forcedRollbackDepth = depth;
}

int StressHooks_GetForcedRollbackDepth() { return s_forcedRollbackDepth; }

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
    out->forced_rollback_depth = s_forcedRollbackDepth;
    out->total_packets_dropped = s_totalDropped;
    out->total_packets_delayed = s_totalDelayed;
    out->total_mismatches_forced = s_totalMismatchesForced;
}

} // namespace Rollback
