/**
 * Alice Senki 2 - Delay Policy
 *
 * Per-player delay policy for rollback gameplay, with an optional shared-max
 * compatibility mode for users who want both peers forced to the same delay.
 *
 * Each peer owns:
 *   - its local input delay
 *   - its max rollback budget
 *   - its rollback-tolerance preference for recommendations
 *
 * During config exchange, each peer announces its local delay, max rollback,
 * and delay mode. The default keeps local/remote visible delays asymmetric.
 * Shared-safe mode resolves both peers to the larger visible delay.
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

constexpr float FRAME_TIME_MS          = 1000.0f / 60.0f;
constexpr int kHiddenGameplayDelayFloor = 1;

enum class GameplayDelayMode : uint8_t {
    SharedSafe = 0,
    AsymmetricExpert = 1,
};

inline const char* GameplayDelayModeName(GameplayDelayMode mode) {
    switch (mode) {
        case GameplayDelayMode::SharedSafe:       return "shared_safe";
        case GameplayDelayMode::AsymmetricExpert: return "asymmetric_expert";
        default:                                  return "unknown";
    }
}

inline bool GameplayDelayMode_IsValid(uint8_t mode) {
    return mode <= (uint8_t)GameplayDelayMode::AsymmetricExpert;
}

// ============================================================================
// Measurement / Exchange
// ============================================================================

struct NetworkMeasurement {
    float    avg_ping_ms;
    float    rtt_variance_ms;
    float    rtt_p90_ms;
    float    rtt_p95_ms;
    float    jitter_p95_ms;
    float    one_way_frames;
    float    jitter_frames;
    int      recommended_delay;
    int      recommended_max_rollback;
    bool     valid;
};

struct DelayNegotiationData {
    int      local_input_delay;
    int      max_rollback;
    GameplayDelayMode gameplay_delay_mode;
};

struct DelayPolicySnapshot {
    int      configured_delay;
    int      active_delay;
    int      resolved_visible_local_delay;
    int      resolved_visible_remote_delay;
    int      effective_local_delay;
    int      effective_remote_delay;
    int      protection_window;
    int      rollback_budget;
    int      rollback_tolerance;
    GameplayDelayMode gameplay_delay_mode;

    int      recommended_delay;
    int      recommended_max_rollback;

    int      remote_announced_delay;
    int      remote_announced_max_rollback;
    int      stall_threshold;

    float    measured_avg_ping_ms;
    float    measured_rtt_variance_ms;
    float    measured_rtt_p90_ms;
    float    measured_rtt_p95_ms;
    float    measured_jitter_p95_ms;
    float    measured_one_way_frames;
    float    measured_jitter_frames;
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
void DelayPolicy_UpdateNetworkMeasurement(float avg_ping_ms,
                                          float rtt_variance_ms,
                                          float rtt_p90_ms,
                                          float rtt_p95_ms,
                                          float jitter_p95_ms);
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

void DelayPolicy_SetGameplayDelayMode(GameplayDelayMode mode);
GameplayDelayMode DelayPolicy_GetGameplayDelayMode();

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
void DelayPolicy_LogDelayMap(const char* reason);

} // namespace Net
