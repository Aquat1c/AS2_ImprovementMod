#pragma once

#include <stdint.h>

#include "net/netplay_phase_runtime.h"
#include "rollback/rollback_telemetry.h"

namespace Net {

enum class NetplayPacingAction : uint8_t {
    None = 0,
    SoftHold,
    HardHold,
    StallHold,
};

enum class NetQuality : uint8_t {
    Unknown = 0,
    StableLowPing,
    StableHighPing,
    JitteryLowPing,
    JitteryHighPing,
    Lossy,
    Severe,
};

const char* NetQualityName(NetQuality quality);
const char* NetplayPacingActionName(NetplayPacingAction action);

struct NetplayPacingSnapshot {
    MatchRollbackPhase phase;
    bool pacing_active;
    bool local_mode_bypassed;
    bool stall_active;
    bool soft_hold_active;
    bool hard_hold_active;
    float frames_ahead;
    float filtered_adjust_ms;
    float target_scale;
    float current_scale;
    float pressure;
    int   stall_frame_count;
    int   soft_hold_count;
    int   hard_hold_count;
    int   stall_gap;
    int   stall_threshold;
    int   raw_remote_gap;
    int   prediction_debt;
    int   effective_remote_delay;
    int   rollback_budget;
    int   soft_threshold;
    int   hard_threshold;
    NetQuality quality;
    NetplayPacingAction last_action;
};

void NetplayPacing_Init();
void NetplayPacing_Shutdown();
void NetplayPacing_ResetSession(const char* reason);
void NetplayPacing_NotifyLocalMode();

NetplayPacingAction NetplayPacing_BeginFrame(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startup_barrier_released,
    int stall_threshold);

void NetplayPacing_OnSessionSample(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startup_barrier_released,
    bool rollback_continues);

void NetplayPacing_OnHoldSample(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startup_barrier_released,
    NetplayPacingAction hold_kind);

void NetplayPacing_GetSnapshot(NetplayPacingSnapshot* out);

} // namespace Net
