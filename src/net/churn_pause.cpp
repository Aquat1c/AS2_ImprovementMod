/**
 * Alice Senki 2 - Device Churn Pause Mitigation (Implementation)
 */

#include "net/churn_pause.h"

#include "input/gamepad_worker.h"
#include "net/netplay_phase_runtime.h"
#include "net/protocol.h"
#include "net/session_manager.h"
#include "rollback/netplay_log.h"
#include "rollback/online_wiring.h"
#include "rollback/rollback_session.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace Net {

namespace {

constexpr uint32_t kDefaultLocalGraceMs = 900;
constexpr uint32_t kDefaultRemoteGraceMs = 1500;
constexpr uint32_t kMainThreadGraceMs = 600;
constexpr uint32_t kMainThreadBlockingGraceMs = 1500;
constexpr uint32_t kResendIntervalMs = 100;
constexpr uint32_t kClearResendIntervalMs = 50;
constexpr uint32_t kPendingAttachGraceMs = 500;
constexpr uint32_t kPendingCommandGraceMs = 900;
constexpr uint32_t kPendingCloseRetryGraceMs = 1000;
constexpr uint32_t kMaxRemoteGraceMs = 5000;
constexpr uint32_t kMaxStalePacketAgeMs = 750;
constexpr uint32_t kMaxForceHoldMs = 12000;
constexpr uint32_t kForceHoldWatchdogLogMs = 2000;

static bool     s_initialized = false;
static bool     s_mitigationEnabled = true;
static bool     s_gameplayActive = false;
static bool     s_localChurnWasActive = false;
static bool     s_forceHoldWatchdogReleased = false;

static uint16_t s_sessionEpoch = 1;
static uint32_t s_localGraceUntilMs = 0;
static uint32_t s_remoteGraceUntilMs = 0;
static ChurnPauseReason s_localReason = ChurnPauseReason::None;
static ChurnPauseReason s_remoteReason = ChurnPauseReason::None;
static int32_t  s_remoteRbFrame = -1;
static int32_t  s_lastRbFrame = -1;
static uint32_t s_lastSendMs = 0;
static uint32_t s_lastClearSendMs = 0;
static uint32_t s_forceHoldSinceMs = 0;
static uint32_t s_forceHoldWatchdogLogMs = 0;
static uint32_t s_packetsSent = 0;
static uint32_t s_packetsReceived = 0;
static uint32_t s_packetsDroppedStale = 0;
static uint32_t s_packetsDroppedEpoch = 0;
static uint32_t s_forceHoldLogTick = 0;

static uint32_t NowMs() {
    return GetTickCount();
}

static uint32_t ElapsedMs(uint32_t fromMs, uint32_t toMs) {
    return toMs - fromMs;
}

static uint32_t SafeAddMs(uint32_t baseMs, uint32_t deltaMs) {
    const uint64_t sum = static_cast<uint64_t>(baseMs) + deltaMs;
    return sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
}

static bool IsGraceActive(uint32_t nowMs, uint32_t untilMs) {
    if (untilMs == 0) {
        return false;
    }
    return static_cast<int32_t>(untilMs - nowMs) >= 0;
}

static bool IsValidChurnReason(uint8_t reason) {
    return reason <= static_cast<uint8_t>(ChurnPauseReason::GamepadIo);
}

static void ExtendLocalGrace(uint32_t graceMs, ChurnPauseReason reason) {
    if (graceMs == 0) {
        graceMs = kDefaultLocalGraceMs;
    }

    const uint32_t now = NowMs();
    const uint32_t until = SafeAddMs(now, graceMs);
    if (static_cast<int32_t>(until - s_localGraceUntilMs) > 0) {
        s_localGraceUntilMs = until;
    }
    if (reason != ChurnPauseReason::None) {
        s_localReason = reason;
    }
}

static void ClearRemoteGrace() {
    s_remoteGraceUntilMs = 0;
    s_remoteReason = ChurnPauseReason::None;
    s_remoteRbFrame = -1;
}

static bool IsLocalGraceActive(uint32_t now) {
    return IsGraceActive(now, s_localGraceUntilMs);
}

static bool IsRemoteGraceActive(uint32_t now) {
    return IsGraceActive(now, s_remoteGraceUntilMs);
}

static void RefreshLocalGraceFromWorker(uint32_t now) {
    (void)now;

    if (Input::GamepadWorker_IsChurnActive()) {
        ExtendLocalGrace(kDefaultLocalGraceMs, ChurnPauseReason::GamepadIo);
    }

    Input::GamepadWorkerStats workerStats{};
    Input::GamepadWorker_GetStats(&workerStats);
    if (workerStats.result_queue_depth > 0) {
        ExtendLocalGrace(kPendingAttachGraceMs, ChurnPauseReason::GamepadConnect);
    }
    if (workerStats.command_queue_depth > 0) {
        ExtendLocalGrace(kPendingCommandGraceMs, ChurnPauseReason::GamepadIo);
    }
}

static bool SendChurnPacket(uint8_t flags,
                            ChurnPauseReason reason,
                            int32_t rbFrame,
                            uint32_t graceMs,
                            uint32_t now) {
    if (!Session_IsConnected()) {
        return false;
    }

    ChurnPausePayload payload{};
    payload.flags = flags;
    payload.reason = static_cast<uint8_t>(reason);
    payload.session_epoch = s_sessionEpoch;
    payload.rb_frame = rbFrame;
    payload.sender_ms = now;
    payload.grace_ms = graceMs;

    return Session_SendPacket(
        CHANNEL_DEBUG,
        PacketType::ChurnPause,
        &payload,
        sizeof(payload),
        false);
}

static void MaybeSendLocalPausePacket(int32_t rbFrame, uint32_t now) {
    if (!s_mitigationEnabled || !s_gameplayActive) {
        return;
    }
    if (!Rollback::OnlineWiring_IsGameplayActive()) {
        return;
    }
    if (!IsLocalGraceActive(now)) {
        return;
    }
    if (s_lastSendMs != 0 && ElapsedMs(s_lastSendMs, now) < kResendIntervalMs) {
        return;
    }

    if (!SendChurnPacket(
            CHURN_PAUSE_FLAG_ACTIVE,
            s_localReason != ChurnPauseReason::None ? s_localReason : ChurnPauseReason::GamepadIo,
            rbFrame,
            kDefaultRemoteGraceMs,
            now)) {
        return;
    }

    s_lastSendMs = now;
    s_packetsSent++;
}

static void MaybeSendLocalClearPacket(int32_t rbFrame, uint32_t now) {
    if (!s_mitigationEnabled || !s_gameplayActive) {
        return;
    }
    if (!Rollback::OnlineWiring_IsGameplayActive()) {
        return;
    }
    if (!Session_IsConnected()) {
        return;
    }
    if (s_lastClearSendMs != 0 && ElapsedMs(s_lastClearSendMs, now) < kClearResendIntervalMs) {
        return;
    }

    if (!SendChurnPacket(
            CHURN_PAUSE_FLAG_CLEAR,
            ChurnPauseReason::None,
            rbFrame,
            0,
            now)) {
        return;
    }

    s_lastClearSendMs = now;
    s_packetsSent++;
}

static void UpdateLocalGraceTransition(int32_t rbFrame, uint32_t now) {
    const bool localActive = IsLocalGraceActive(now);
    if (s_localChurnWasActive && !localActive) {
        MaybeSendLocalClearPacket(rbFrame, now);
        s_localReason = ChurnPauseReason::None;
    }
    s_localChurnWasActive = localActive;
}

static void TickChurnPause(bool gameplay_active, int32_t rb_frame_current) {
    s_gameplayActive = gameplay_active;
    s_lastRbFrame = rb_frame_current;

    const uint32_t now = NowMs();
    RefreshLocalGraceFromWorker(now);
    UpdateLocalGraceTransition(rb_frame_current, now);
    MaybeSendLocalPausePacket(rb_frame_current, now);
}

static void LogForceHoldIfNeeded(uint32_t now,
                                 const Rollback::RollbackTimesyncTelemetry& telemetry,
                                 bool localActive,
                                 bool remoteActive) {
    if (IsGraceActive(now, s_forceHoldLogTick)) {
        return;
    }
    s_forceHoldLogTick = SafeAddMs(now, 1000);

    Rollback::NetplayLog_Write(
        "CHURN",
        telemetry.rb_frame_current,
        "force_hold local=%d remote=%d local_reason=%u remote_reason=%u rb=%d remote_rb=%d gap=%d watchdog=%d",
        localActive ? 1 : 0,
        remoteActive ? 1 : 0,
        (unsigned)s_localReason,
        (unsigned)s_remoteReason,
        telemetry.rb_frame_current,
        telemetry.rb_frame_last_remote_received,
        telemetry.rb_frame_last_remote_received >= 0
            ? (telemetry.rb_frame_current - telemetry.rb_frame_last_remote_received)
            : -1,
        s_forceHoldWatchdogReleased ? 1 : 0);
}

static bool ShouldReleaseForceHoldWatchdog(uint32_t now) {
    if (s_forceHoldSinceMs == 0) {
        return false;
    }
    if (ElapsedMs(s_forceHoldSinceMs, now) <= kMaxForceHoldMs) {
        return false;
    }

    if (!s_forceHoldWatchdogReleased) {
        s_forceHoldWatchdogReleased = true;
        s_forceHoldWatchdogLogMs = now;
        Rollback::NetplayLog_Write(
            "CHURN",
            s_lastRbFrame,
            "force_hold watchdog released after %lums; falling back to normal stall policy",
            (unsigned long)kMaxForceHoldMs);
    } else if (s_forceHoldWatchdogLogMs != 0 &&
               ElapsedMs(s_forceHoldWatchdogLogMs, now) >= kForceHoldWatchdogLogMs) {
        s_forceHoldWatchdogLogMs = SafeAddMs(now, kForceHoldWatchdogLogMs);
        Rollback::NetplayLog_Write(
            "CHURN",
            s_lastRbFrame,
            "force_hold watchdog still active locally=%d remote=%d",
            IsLocalGraceActive(now) ? 1 : 0,
            IsRemoteGraceActive(now) ? 1 : 0);
    }

    return true;
}

} // namespace

void ChurnPause_Init() {
    if (s_initialized) {
        return;
    }

    s_initialized = true;
    s_mitigationEnabled = true;
    ChurnPause_ResetSession("init");

    Rollback::NetplayLog_Write("CHURN", -1, "ChurnPause init: mitigation_enabled=1");
}

void ChurnPause_Shutdown() {
    if (!s_initialized) {
        return;
    }

    ChurnPause_ResetSession("shutdown");
    s_initialized = false;
    Rollback::NetplayLog_Write("CHURN", -1, "ChurnPause shutdown");
}

void ChurnPause_ResetSession(const char* reason) {
    s_gameplayActive = false;
    s_localGraceUntilMs = 0;
    s_remoteGraceUntilMs = 0;
    s_localReason = ChurnPauseReason::None;
    s_remoteReason = ChurnPauseReason::None;
    s_remoteRbFrame = -1;
    s_lastRbFrame = -1;
    s_lastSendMs = 0;
    s_lastClearSendMs = 0;
    s_forceHoldSinceMs = 0;
    s_forceHoldLogTick = 0;
    s_forceHoldWatchdogLogMs = 0;
    s_localChurnWasActive = false;
    s_forceHoldWatchdogReleased = false;
    s_packetsSent = 0;
    s_packetsReceived = 0;
    s_packetsDroppedStale = 0;
    s_packetsDroppedEpoch = 0;

    if (s_sessionEpoch == 0) {
        s_sessionEpoch = 1;
    } else {
        s_sessionEpoch++;
        if (s_sessionEpoch == 0) {
            s_sessionEpoch = 1;
        }
    }

    Rollback::NetplayLog_Write(
        "CHURN",
        -1,
        "ChurnPause session reset: %s epoch=%u",
        reason ? reason : "?",
        (unsigned)s_sessionEpoch);
}

void ChurnPause_SetMitigationEnabled(bool enabled) {
    s_mitigationEnabled = enabled;
}

bool ChurnPause_IsMitigationEnabled() {
    return s_initialized && s_mitigationEnabled;
}

void ChurnPause_FrameUpdate(bool gameplay_active, int32_t rb_frame_current) {
    if (!s_initialized) {
        return;
    }

    TickChurnPause(gameplay_active, rb_frame_current);
}

void ChurnPause_OnRollbackPoll(bool gameplay_active, int32_t rb_frame_current) {
    if (!s_initialized) {
        return;
    }

    TickChurnPause(gameplay_active, rb_frame_current);
}

void ChurnPause_NotifyLocalGamepadChurn(ChurnPauseReason reason, uint32_t grace_ms) {
    if (!s_initialized || reason == ChurnPauseReason::None) {
        return;
    }

    ExtendLocalGrace(grace_ms != 0 ? grace_ms : kMainThreadGraceMs, reason);
}

void ChurnPause_NotifyPendingCloseRetry() {
    if (!s_initialized) {
        return;
    }

    ExtendLocalGrace(kPendingCloseRetryGraceMs, ChurnPauseReason::GamepadDisconnect);
}

void ChurnPause_NotifyMainThreadBlockingIo() {
    if (!s_initialized) {
        return;
    }

    ExtendLocalGrace(kMainThreadBlockingGraceMs, ChurnPauseReason::GamepadIo);
}

bool ChurnPause_ShouldForceHold(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startup_barrier_released) {
    if (!ChurnPause_IsMitigationEnabled()) {
        return false;
    }
    if (!NetplayPhaseRuntime_IsInteractivePacingPhase(phase)) {
        return false;
    }
    if (!startup_barrier_released) {
        return false;
    }
    if (!Rollback::OnlineWiring_IsGameplayActive()) {
        return false;
    }

    const uint32_t now = NowMs();
    const bool localActive = IsLocalGraceActive(now);
    const bool remoteActive = IsRemoteGraceActive(now);
    if (!localActive && !remoteActive) {
        s_forceHoldSinceMs = 0;
        s_forceHoldWatchdogReleased = false;
        return false;
    }

    if (ShouldReleaseForceHoldWatchdog(now)) {
        return false;
    }

    if (s_forceHoldSinceMs == 0) {
        s_forceHoldSinceMs = now;
        s_forceHoldWatchdogReleased = false;
    }

    LogForceHoldIfNeeded(now, telemetry, localActive, remoteActive);
    return true;
}

void ChurnPause_OnRemotePacket(const ChurnPausePayload* payload) {
    if (!s_initialized || !payload) {
        return;
    }
    if (!Rollback::OnlineWiring_IsGameplayActive()) {
        return;
    }

    if (payload->session_epoch != s_sessionEpoch) {
        s_packetsDroppedEpoch++;
        if (s_packetsDroppedEpoch <= 3 || (s_packetsDroppedEpoch % 30) == 0) {
            Rollback::NetplayLog_Write(
                "CHURN",
                payload->rb_frame,
                "dropped remote_pause stale epoch: rx=%u expected=%u",
                (unsigned)payload->session_epoch,
                (unsigned)s_sessionEpoch);
        }
        return;
    }

    const uint32_t now = NowMs();
    if (payload->sender_ms != 0) {
        const uint32_t ageMs = ElapsedMs(payload->sender_ms, now);
        if (ageMs > kMaxStalePacketAgeMs) {
            s_packetsDroppedStale++;
            if (s_packetsDroppedStale <= 3 || (s_packetsDroppedStale % 30) == 0) {
                Rollback::NetplayLog_Write(
                    "CHURN",
                    payload->rb_frame,
                    "dropped remote_pause stale age: age_ms=%lu reason=%u",
                    (unsigned long)ageMs,
                    (unsigned)payload->reason);
            }
            return;
        }
    }

    if ((payload->flags & CHURN_PAUSE_FLAG_CLEAR) != 0) {
        ClearRemoteGrace();
        s_packetsReceived++;
        Rollback::NetplayLog_Write(
            "CHURN",
            payload->rb_frame,
            "remote_pause clear rx=%u",
            s_packetsReceived);
        return;
    }

    if ((payload->flags & CHURN_PAUSE_FLAG_ACTIVE) == 0) {
        return;
    }

    if (!IsValidChurnReason(payload->reason)) {
        return;
    }

    const uint32_t graceMs = (std::min)(
        payload->grace_ms > 0 ? payload->grace_ms : kDefaultRemoteGraceMs,
        kMaxRemoteGraceMs);
    const uint32_t until = SafeAddMs(now, graceMs);
    if (static_cast<int32_t>(until - s_remoteGraceUntilMs) > 0) {
        s_remoteGraceUntilMs = until;
    }

    s_remoteRbFrame = payload->rb_frame;
    s_remoteReason = static_cast<ChurnPauseReason>(payload->reason);

    s_packetsReceived++;
    if (s_packetsReceived <= 3 || (s_packetsReceived % 60) == 0) {
        Rollback::NetplayLog_Write(
            "CHURN",
            payload->rb_frame,
            "remote_pause rx=%u reason=%u grace_ms=%u remote_rb=%d",
            s_packetsReceived,
            (unsigned)payload->reason,
            graceMs,
            payload->rb_frame);
    }
}

void ChurnPause_GetSnapshot(ChurnPauseSnapshot* out) {
    if (!out) {
        return;
    }

    const uint32_t now = NowMs();
    const bool localActive = IsLocalGraceActive(now);
    const bool remoteActive = IsRemoteGraceActive(now);

    out->mitigation_enabled = ChurnPause_IsMitigationEnabled();
    out->local_grace_active = localActive;
    out->remote_grace_active = remoteActive;
    out->force_hold_watchdog_released = s_forceHoldWatchdogReleased;
    out->force_hold_active =
        out->mitigation_enabled &&
        Rollback::OnlineWiring_IsGameplayActive() &&
        (localActive || remoteActive) &&
        !s_forceHoldWatchdogReleased;
    out->local_reason = static_cast<uint8_t>(s_localReason);
    out->remote_reason = static_cast<uint8_t>(s_remoteReason);
    out->session_epoch = s_sessionEpoch;
    out->remote_rb_frame = s_remoteRbFrame;
    out->local_grace_until_ms = s_localGraceUntilMs;
    out->remote_grace_until_ms = s_remoteGraceUntilMs;
    out->force_hold_elapsed_ms =
        s_forceHoldSinceMs != 0 ? ElapsedMs(s_forceHoldSinceMs, now) : 0;
    out->packets_sent = s_packetsSent;
    out->packets_received = s_packetsReceived;
    out->packets_dropped_stale = s_packetsDroppedStale;
    out->packets_dropped_epoch = s_packetsDroppedEpoch;
}

} // namespace Net
