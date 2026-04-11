/**
 * Alice Senki 2 - Delay Policy
 *
 * Asymmetric local-delay policy for rollback gameplay.
 *
 * Each peer owns:
 *   - its local input delay
 *   - its max rollback budget
 *   - its rollback-tolerance preference for recommendations
 *
 * During config exchange, each peer announces only its local delay and
 * max rollback. The remote values are stored locally for diagnostics and
 * stall-threshold computation; gameplay no longer negotiates a shared delay.
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Constants
// ============================================================================

constexpr int DELAY_MIN                = 0;
constexpr int DELAY_MAX                = 6;
constexpr int DELAY_DEFAULT_PREF       = 0;

constexpr int ROLLBACK_BUDGET_MIN      = 4;
constexpr int ROLLBACK_BUDGET_MAX      = 10;
constexpr int ROLLBACK_BUDGET_DEFAULT  = 7;

constexpr int ROLLBACK_TOLERANCE_MIN   = 0;
constexpr int ROLLBACK_TOLERANCE_MAX   = 4;
constexpr int ROLLBACK_TOLERANCE_DEFAULT = 2;

constexpr float FRAME_TIME_MS          = 16.667f;
constexpr int kHiddenGameplayDelayFloor = 1;

// ============================================================================
// Measurement / Exchange
// ============================================================================

struct NetworkMeasurement {
    float    avg_ping_ms;
    float    one_way_frames;
    int      recommended_delay;
    int      recommended_max_rollback;
    bool     valid;
};

struct DelayNegotiationData {
    int      local_input_delay;
    int      max_rollback;
};

struct DelayPolicySnapshot {
    int      configured_delay;
    int      active_delay;
    int      effective_local_delay;
    int      effective_remote_delay;
    int      protection_window;
    int      rollback_budget;
    int      rollback_tolerance;

    int      recommended_delay;
    int      recommended_max_rollback;

    int      remote_announced_delay;
    int      remote_announced_max_rollback;
    int      stall_threshold;

    float    measured_avg_ping_ms;
    float    measured_one_way_frames;
    bool     measurement_valid;

    bool     rollback_synced;
    int      rollback_current_delay;
};

// ============================================================================
// Lifecycle
// ============================================================================

void DelayPolicy_Init();
void DelayPolicy_Shutdown();
void DelayPolicy_FrameUpdate();
void DelayPolicy_ResetSession();

// ============================================================================
// Measurement / Recommendations
// ============================================================================

void DelayPolicy_UpdateMeasurement(float rtt_ms, float rtt_variance_ms);
void DelayPolicy_UpdateFromStats(float avg_ping_ms);
int  DelayPolicy_ComputeRecommendedDelay();
int  DelayPolicy_ComputeRecommendedMaxRollback();
void DelayPolicy_GetMeasurement(NetworkMeasurement* out);

// ============================================================================
// Local Settings
// ============================================================================

void DelayPolicy_SetConfiguredDelay(int delay);
int  DelayPolicy_GetConfiguredDelay();

void DelayPolicy_SetRollbackBudget(int frames);
int  DelayPolicy_GetRollbackBudget();

void DelayPolicy_SetRollbackToleranceK(int tolerance_k);
int  DelayPolicy_GetRollbackToleranceK();

// ============================================================================
// Config Exchange / Runtime State
// ============================================================================

void DelayPolicy_BuildNegotiationData(DelayNegotiationData* out);
void DelayPolicy_NegotiateSession(const DelayNegotiationData* remote);

int  DelayPolicy_GetActiveDelay();
int  DelayPolicy_GetEffectiveLocalDelay();
int  DelayPolicy_GetEffectiveRemoteDelay();
int  DelayPolicy_GetRemoteAnnouncedDelay();
int  DelayPolicy_GetRemoteAnnouncedMaxRollback();
int  DelayPolicy_GetProtectionWindow();
int  DelayPolicy_GetStallThreshold();

bool DelayPolicy_IsRollbackSynced();
void DelayPolicy_OnRollbackApplied(int delay_value);

// ============================================================================
// Diagnostics
// ============================================================================

void DelayPolicy_GetSnapshot(DelayPolicySnapshot* out);

} // namespace Net
