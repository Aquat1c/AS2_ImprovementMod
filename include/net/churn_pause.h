/**
 * Alice Senki 2 - Device Churn Pause Mitigation
 *
 * During local gamepad/USB device I/O, coordinates a mutual gameplay hold so
 * rollback netplay stalls both sides instead of running ahead, disconnecting,
 * or desyncing. Reuses existing StallHold pacing — no changes to GekkoNet or
 * rollback input flow beyond an early hold gate.
 */

#pragma once

#include <stdint.h>

#include "net/netplay_phase_runtime.h"
#include "rollback/rollback_session.h"

namespace Net {

enum class ChurnPauseReason : uint8_t {
    None = 0,
    GamepadConnect = 1,
    GamepadDisconnect = 2,
    GamepadIo = 3,
};

struct ChurnPauseSnapshot {
    bool     mitigation_enabled;
    bool     local_grace_active;
    bool     remote_grace_active;
    bool     force_hold_active;
    bool     force_hold_watchdog_released;
    uint8_t  local_reason;
    uint8_t  remote_reason;
    uint16_t session_epoch;
    int32_t  remote_rb_frame;
    uint32_t local_grace_until_ms;
    uint32_t remote_grace_until_ms;
    uint32_t force_hold_elapsed_ms;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_dropped_stale;
    uint32_t packets_dropped_epoch;
};

void ChurnPause_Init();
void ChurnPause_Shutdown();
void ChurnPause_ResetSession(const char* reason);

void ChurnPause_SetMitigationEnabled(bool enabled);
bool ChurnPause_IsMitigationEnabled();

/// Per-frame housekeeping: refresh local grace from the gamepad worker and
/// emit peer-visible pause packets while local device I/O is active.
void ChurnPause_FrameUpdate(bool gameplay_active, int32_t rb_frame_current);

/// Secondary tick while rollback is active (e.g. during dispatcher holds) so
/// pause packets still flow if the main frame update path is skipped.
void ChurnPause_OnRollbackPoll(bool gameplay_active, int32_t rb_frame_current);

/// Main-thread hint that device churn is underway (e.g. disconnect detected).
void ChurnPause_NotifyLocalGamepadChurn(ChurnPauseReason reason, uint32_t grace_ms);

/// Close could not be queued on the worker and is pending or running on main.
void ChurnPause_NotifyPendingCloseRetry();
void ChurnPause_NotifyMainThreadBlockingIo();

/// Returns true when interactive rollback gameplay should enter StallHold early.
bool ChurnPause_ShouldForceHold(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startup_barrier_released);

void ChurnPause_OnRemotePacket(const struct ChurnPausePayload* payload);

void ChurnPause_GetSnapshot(ChurnPauseSnapshot* out);

} // namespace Net
