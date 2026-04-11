#pragma once

#include <stdint.h>

#include "net/netplay_phase_runtime.h"
#include "rollback/rollback_session.h"

namespace Net {

enum class NetplayPacingAction : uint8_t {
    None = 0,
    StallHold,
};

struct NetplayPacingSnapshot {
    MatchRollbackPhase phase;
    bool pacing_active;
    bool local_mode_bypassed;
    bool stall_active;
    float frames_ahead;
    float filtered_adjust_ms;
    float target_scale;
    float current_scale;
    int   stall_frame_count;
    int   stall_gap;
    int   stall_threshold;
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

void NetplayPacing_GetSnapshot(NetplayPacingSnapshot* out);

} // namespace Net