#include "net/netplay_pacing.h"

#include "patches/tick_hooks.h"
#include "rollback/netplay_log.h"

#include <algorithm>
#include <math.h>
#include <string.h>

namespace {

struct NetplayPacingController {
    static constexpr float kBaseFrameMs = 16.6667f;
    static constexpr float kDeadbandFrames = 0.08f;
    static constexpr float kGainMsPerFrame = 0.55f;
    static constexpr float kMaxAdjustMs = 0.90f;
    static constexpr float kAdjustEmaAlpha = 0.12f;
    static constexpr float kScaleMin = 0.94f;
    static constexpr float kScaleMax = 1.06f;

    float filtered_adjust_ms = 0.0f;
    float last_frames_ahead = 0.0f;
    bool active = false;
};

static NetplayPacingController s_controller{};
static Net::MatchRollbackPhase s_phase = Net::MatchRollbackPhase::None;
static bool s_initialized = false;
static bool s_localModeLogged = false;
static bool s_stallActive = false;
static int  s_stallFrameCount = 0;
static int  s_stallGap = 0;
static int  s_stallThreshold = 0;

static bool IsInteractivePacingEnabled(Net::MatchRollbackPhase phase,
                                       bool startupBarrierReleased) {
    return startupBarrierReleased && Net::NetplayPhaseRuntime_IsInteractivePacingPhase(phase);
}

static void CopyTickState(Net::NetplayPacingSnapshot* out) {
    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    out->target_scale = tickState.target_scale;
    out->current_scale = tickState.current_scale;
}

static void DeactivatePacing(Net::MatchRollbackPhase phase,
                             const char* reason,
                             bool rollbackContinues) {
    const bool hadPacing = s_controller.active;
    const float priorAdjust = s_controller.filtered_adjust_ms;

    SetNetplayPacingActive(false);
    SetNetplayTickScaleTarget(1.0f);

    s_controller.filtered_adjust_ms = 0.0f;
    s_controller.last_frames_ahead = 0.0f;
    s_controller.active = false;
    s_stallGap = 0;
    s_stallThreshold = 0;
    s_stallFrameCount = 0;
    s_stallActive = false;

    if (!hadPacing && fabsf(priorAdjust) <= 0.001f) {
        s_phase = phase;
        return;
    }

    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    Rollback::NetplayLog_Write(
        "PACE",
        -1,
        "disable phase=%s reason=%s rollback_continues=%d target_scale=%.3f current_scale=%.3f",
        Net::MatchRollbackPhaseName(phase),
        reason ? reason : "unspecified",
        rollbackContinues ? 1 : 0,
        tickState.target_scale,
        tickState.current_scale);

    s_phase = phase;
}

static void LogEnable(const Rollback::RollbackTimesyncTelemetry& telemetry) {
    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    Rollback::NetplayLog_Write(
        "PACE",
        telemetry.rb_frame_current,
        "enable phase=%s rb=%d frames_ahead=%.2f adjust_ms=%.2f target_scale=%.3f current_scale=%.3f",
        Net::MatchRollbackPhaseName(s_phase),
        telemetry.rb_frame_current,
        telemetry.frames_ahead,
        s_controller.filtered_adjust_ms,
        tickState.target_scale,
        tickState.current_scale);
}

static void LogUpdate(const Rollback::RollbackTimesyncTelemetry& telemetry) {
    if (telemetry.rb_frame_current <= 0) {
        return;
    }

    const bool coarseInterval = (telemetry.rb_frame_current % 120) == 0;
    const bool meaningfulSkew = fabsf(telemetry.frames_ahead) >= 0.40f;
    if (!coarseInterval && !meaningfulSkew) {
        return;
    }

    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    Rollback::NetplayLog_Write(
        "PACE",
        telemetry.rb_frame_current,
        "update rb=%d frames_ahead=%.2f adjust_ms=%.2f target_scale=%.3f current_scale=%.3f",
        telemetry.rb_frame_current,
        telemetry.frames_ahead,
        s_controller.filtered_adjust_ms,
        tickState.target_scale,
        tickState.current_scale);
}

} // anonymous namespace

namespace Net {

void NetplayPacing_Init() {
    s_initialized = true;
    s_phase = MatchRollbackPhase::None;
    s_localModeLogged = false;
    s_stallActive = false;
    s_stallFrameCount = 0;
    s_stallGap = 0;
    s_stallThreshold = 0;
    s_controller = NetplayPacingController{};
    SetNetplayPacingActive(false);
    SetNetplayTickScaleTarget(1.0f);
}

void NetplayPacing_Shutdown() {
    if (!s_initialized) {
        return;
    }

    NetplayPacing_ResetSession("shutdown");
    s_initialized = false;
}

void NetplayPacing_ResetSession(const char* reason) {
    if (!s_initialized) {
        return;
    }

    DeactivatePacing(s_phase, reason ? reason : "session reset", false);
    ResetNetplayTickScaleState();
    s_localModeLogged = false;
}

void NetplayPacing_NotifyLocalMode() {
    if (!s_initialized) {
        return;
    }

    if (s_localModeLogged) {
        return;
    }

    NetplayPacing_ResetSession("local mode");

    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    Rollback::NetplayLog_Write(
        "PACE",
        -1,
        "local_mode rollback_bypassed=1 target_scale=%.3f current_scale=%.3f",
        tickState.target_scale,
        tickState.current_scale);
    s_localModeLogged = true;
}

NetplayPacingAction NetplayPacing_BeginFrame(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startupBarrierReleased,
    int stallThreshold) {
    if (!s_initialized) {
        return NetplayPacingAction::None;
    }

    s_localModeLogged = false;
    s_phase = phase;

    const bool interactive = IsInteractivePacingEnabled(phase, startupBarrierReleased);
    if (!interactive) {
        const char* reason = startupBarrierReleased ? "non_interactive" : "startup_or_lockstep";
        DeactivatePacing(phase, reason, NetplayPhaseRuntime_IsRollbackOwnedPhase(phase));
        return NetplayPacingAction::None;
    }

    s_stallThreshold = stallThreshold;
    s_stallGap = telemetry.rb_frame_last_remote_received >= 0
        ? telemetry.rb_frame_current - telemetry.rb_frame_last_remote_received
        : 0;

    if (stallThreshold >= 0 &&
        telemetry.rb_frame_last_remote_received >= 0 &&
        s_stallGap > stallThreshold) {
        s_stallFrameCount++;
        s_stallActive = true;
        if (s_stallFrameCount <= 5 || (s_stallFrameCount % 120) == 0) {
            Rollback::NetplayLog_Write(
                "PACE",
                telemetry.rb_frame_current,
                "stall_hold phase=%s rb=%d remote_rb=%d gap=%d threshold=%d frames_ahead=%.2f",
                MatchRollbackPhaseName(phase),
                telemetry.rb_frame_current,
                telemetry.rb_frame_last_remote_received,
                s_stallGap,
                stallThreshold,
                telemetry.frames_ahead);
        }
        return NetplayPacingAction::StallHold;
    }

    if (s_stallActive) {
        Rollback::NetplayLog_Write(
            "PACE",
            telemetry.rb_frame_current,
            "stall_recovered phase=%s rb=%d held_frames=%d gap=%d threshold=%d",
            MatchRollbackPhaseName(phase),
            telemetry.rb_frame_current,
            s_stallFrameCount,
            s_stallGap,
            stallThreshold);
        s_stallActive = false;
        s_stallFrameCount = 0;
    }

    return NetplayPacingAction::None;
}

void NetplayPacing_OnSessionSample(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startupBarrierReleased,
    bool rollbackContinues) {
    if (!s_initialized) {
        return;
    }

    s_phase = phase;
    const bool interactive = IsInteractivePacingEnabled(phase, startupBarrierReleased);
    if (!interactive) {
        const char* reason = startupBarrierReleased ? "non_interactive" : "startup_or_lockstep";
        DeactivatePacing(phase, reason, rollbackContinues);
        return;
    }

    const float frames = telemetry.frames_ahead;
    float targetAdjustMs = 0.0f;
    if (fabsf(frames) >= NetplayPacingController::kDeadbandFrames) {
        targetAdjustMs = (std::clamp)(
            frames * NetplayPacingController::kGainMsPerFrame,
            -NetplayPacingController::kMaxAdjustMs,
            +NetplayPacingController::kMaxAdjustMs);
    }

    s_controller.filtered_adjust_ms +=
        (targetAdjustMs - s_controller.filtered_adjust_ms) * NetplayPacingController::kAdjustEmaAlpha;

    float targetFrameMs = NetplayPacingController::kBaseFrameMs + s_controller.filtered_adjust_ms;
    if (targetFrameMs < 1.0f) {
        targetFrameMs = 1.0f;
    }

    float targetScale = NetplayPacingController::kBaseFrameMs / targetFrameMs;
    targetScale = (std::clamp)(
        targetScale,
        NetplayPacingController::kScaleMin,
        NetplayPacingController::kScaleMax);

    SetNetplayPacingActive(true);
    SetNetplayTickScaleTarget(targetScale);

    const bool firstEnable = !s_controller.active;
    s_controller.active = true;
    s_controller.last_frames_ahead = frames;

    if (firstEnable) {
        LogEnable(telemetry);
    }
    LogUpdate(telemetry);
}

void NetplayPacing_GetSnapshot(NetplayPacingSnapshot* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->phase = s_phase;
    out->pacing_active = s_controller.active;
    out->local_mode_bypassed = s_localModeLogged;
    out->stall_active = s_stallActive;
    out->frames_ahead = s_controller.last_frames_ahead;
    out->filtered_adjust_ms = s_controller.filtered_adjust_ms;
    out->stall_frame_count = s_stallFrameCount;
    out->stall_gap = s_stallGap;
    out->stall_threshold = s_stallThreshold;
    CopyTickState(out);
}

} // namespace Net