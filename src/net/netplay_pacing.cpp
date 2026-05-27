#include "net/netplay_pacing.h"

#include "net/delay_policy.h"
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

struct PacingProfile {
    int target_debt;
    int soft_threshold;
    int hard_threshold;
    int persistence_required_count;
    int persistence_window;
    int min_soft_hold_gap_rb_frames;
    float pressure_add_scale;
    float pressure_decay;
    float hold_cost;
    bool allow_soft_hold;
};

static NetplayPacingController s_controller{};
static Net::MatchRollbackPhase s_phase = Net::MatchRollbackPhase::None;
static bool s_initialized = false;
static bool s_localModeLogged = false;
static bool s_stallActive = false;
static bool s_softHoldActive = false;
static bool s_hardHoldActive = false;
static int  s_stallFrameCount = 0;
static int  s_softHoldCount = 0;
static int  s_hardHoldCount = 0;
static int  s_stallGap = 0;
static int  s_stallThreshold = 0;
static int  s_rawRemoteGap = 0;
static int  s_predictionDebt = 0;
static int  s_effectiveRemoteDelay = 0;
static int  s_rollbackBudget = 0;
static int  s_softThreshold = 0;
static int  s_hardThreshold = 0;
static int  s_lastSoftHoldRb = -1000000;
static int  s_lastHoldRb = -1000000;
static int  s_lastNetClassLogRb = -1000000;
static int  s_lastDebtLogRb = -1000000;
static int  s_lastDecisionLogRb = -1000000;
static int  s_lastAsymDelayLogRb = -1000000;
static float s_pressure = 0.0f;
static Net::NetQuality s_quality = Net::NetQuality::Unknown;
static Net::NetplayPacingAction s_lastAction = Net::NetplayPacingAction::None;
static bool s_profileSourceAvgOnly = true;

static bool s_debtHistory[8] = {};
static int  s_debtHistoryPos = 0;
static int  s_debtHistoryCount = 0;

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

static PacingProfile ProfileFor(Net::NetQuality quality) {
    switch (quality) {
        case Net::NetQuality::StableLowPing:
            return {1, 3, 6, 3, 6, 12, 1.0f, 0.75f, 4.0f, true};
        case Net::NetQuality::StableHighPing:
            return {2, 4, 6, 2, 5, 10, 1.0f, 0.55f, 5.0f, true};
        case Net::NetQuality::JitteryLowPing:
            return {2, 3, 5, 3, 5, 8, 1.2f, 0.45f, 4.0f, true};
        case Net::NetQuality::JitteryHighPing:
            return {2, 4, 6, 2, 5, 6, 1.3f, 0.35f, 4.0f, true};
        case Net::NetQuality::Lossy:
            return {2, 3, 5, 1, 4, 6, 1.5f, 0.25f, 3.0f, true};
        case Net::NetQuality::Severe:
            return {0, 1000000, 4, 1000000, 1, 1000000, 0.0f, 1.0f, 1000000.0f, false};
        case Net::NetQuality::Unknown:
        default:
            return {1, 3, 6, 3, 6, 12, 0.9f, 0.65f, 5.0f, true};
    }
}

static Net::NetQuality ClassifyQuality(const Rollback::RollbackTimesyncTelemetry& telemetry,
                                       bool* avgOnlyOut) {
    const float rttAvg = telemetry.rtt_avg_ms > 0.0f
        ? telemetry.rtt_avg_ms
        : telemetry.gekko_avg_ping;
    const float jitterAvg = telemetry.jitter_avg_ms > 0.0f
        ? telemetry.jitter_avg_ms
        : telemetry.gekko_jitter;
    const bool havePercentiles = telemetry.rtt_p90_ms > 0.0f || telemetry.rtt_p95_ms > 0.0f;
    const float rttP90 = telemetry.rtt_p90_ms > 0.0f ? telemetry.rtt_p90_ms : rttAvg;
    const float rttP95 = telemetry.rtt_p95_ms > 0.0f ? telemetry.rtt_p95_ms : (rttAvg + jitterAvg);
    const float jitterP95 = telemetry.jitter_p95_ms > 0.0f
        ? telemetry.jitter_p95_ms
        : jitterAvg;

    if (avgOnlyOut) {
        *avgOnlyOut = !havePercentiles;
    }

    const bool highPing = rttP90 >= 120.0f;
    const bool highJitter = jitterP95 >= 18.0f || (rttP95 - rttAvg) >= 25.0f;
    const bool lossy = telemetry.packet_loss_ewma >= 0.005f || telemetry.loss_burst_max >= 2;
    const bool severe =
        telemetry.packet_loss_ewma >= 0.020f ||
        telemetry.loss_burst_max >= 3 ||
        rttP95 >= 280.0f;

    if (severe) return Net::NetQuality::Severe;
    if (lossy) return Net::NetQuality::Lossy;
    if (highPing && highJitter) return Net::NetQuality::JitteryHighPing;
    if (highPing) return Net::NetQuality::StableHighPing;
    if (highJitter) return Net::NetQuality::JitteryLowPing;
    return Net::NetQuality::StableLowPing;
}

static void PushDebtHistory(bool overThreshold) {
    s_debtHistory[s_debtHistoryPos] = overThreshold;
    s_debtHistoryPos = (s_debtHistoryPos + 1) % (int)(sizeof(s_debtHistory) / sizeof(s_debtHistory[0]));
    if (s_debtHistoryCount < (int)(sizeof(s_debtHistory) / sizeof(s_debtHistory[0]))) {
        s_debtHistoryCount++;
    }
}

static int CountRecentDebtSamples(int window) {
    const int cappedWindow = (std::min)(window, s_debtHistoryCount);
    int count = 0;
    for (int i = 0; i < cappedWindow; i++) {
        const int idx = (s_debtHistoryPos - 1 - i + (int)(sizeof(s_debtHistory) / sizeof(s_debtHistory[0]))) %
                        (int)(sizeof(s_debtHistory) / sizeof(s_debtHistory[0]));
        if (s_debtHistory[idx]) {
            count++;
        }
    }
    return count;
}

static void UpdateDebtState(const Rollback::RollbackTimesyncTelemetry& telemetry,
                            int stallThreshold) {
    s_stallThreshold = stallThreshold;
    s_rawRemoteGap = telemetry.raw_remote_gap;
    if (s_rawRemoteGap <= 0 && telemetry.rb_frame_last_remote_received >= 0) {
        s_rawRemoteGap = (std::max)(0, telemetry.rb_frame_current - telemetry.rb_frame_last_remote_received);
    }
    s_stallGap = s_rawRemoteGap;
    s_effectiveRemoteDelay = telemetry.effective_remote_delay > 0
        ? telemetry.effective_remote_delay
        : Net::DelayPolicy_GetEffectiveRemoteDelay();
    s_rollbackBudget = telemetry.rollback_budget > 0
        ? telemetry.rollback_budget
        : Net::DelayPolicy_GetRollbackBudget();
    s_predictionDebt = telemetry.prediction_debt;
    if (s_predictionDebt <= 0) {
        s_predictionDebt = (std::max)(0, s_rawRemoteGap - s_effectiveRemoteDelay);
    }
}

static void LogNetClassIfNeeded(const Rollback::RollbackTimesyncTelemetry& telemetry,
                                Net::NetQuality prevQuality) {
    const bool qualityChanged = prevQuality != s_quality;
    const bool interval = telemetry.rb_frame_current - s_lastNetClassLogRb >= 300;
    if (!qualityChanged && !interval) {
        return;
    }

    s_lastNetClassLogRb = telemetry.rb_frame_current;
    Rollback::NetplayLog_Write(
        "NETCLASS",
        telemetry.rb_frame_current,
        "rb=%d class=%s source=%s rtt_avg=%.1f rtt_p90=%.1f rtt_p95=%.1f jitter95=%.1f loss=%.3f burst=%d",
        telemetry.rb_frame_current,
        Net::NetQualityName(s_quality),
        s_profileSourceAvgOnly ? "avg_only" : "p95",
        telemetry.rtt_avg_ms > 0.0f ? telemetry.rtt_avg_ms : telemetry.gekko_avg_ping,
        telemetry.rtt_p90_ms,
        telemetry.rtt_p95_ms,
        telemetry.jitter_p95_ms > 0.0f ? telemetry.jitter_p95_ms : telemetry.gekko_jitter,
        telemetry.packet_loss_ewma,
        telemetry.loss_burst_max);
}

static void LogDebtIfNeeded(const Rollback::RollbackTimesyncTelemetry& telemetry) {
    if (s_predictionDebt <= 0 && telemetry.rb_frame_current - s_lastDebtLogRb < 120) {
        return;
    }
    if (s_predictionDebt > 0 && telemetry.rb_frame_current - s_lastDebtLogRb < 30) {
        return;
    }

    s_lastDebtLogRb = telemetry.rb_frame_current;
    Rollback::NetplayLog_Write(
        "DEBT",
        telemetry.rb_frame_current,
        "rb=%d remote_contig=%d raw_gap=%d remote_eff=%d debt=%d frames_ahead=%.2f rb_budget=%d",
        telemetry.rb_frame_current,
        telemetry.rb_frame_remote_contiguous,
        s_rawRemoteGap,
        s_effectiveRemoteDelay,
        s_predictionDebt,
        telemetry.frames_ahead,
        s_rollbackBudget);
}

static void LogAsymDelayIfNeeded(const Rollback::RollbackTimesyncTelemetry& telemetry) {
    Net::DelayPolicySnapshot snap{};
    Net::DelayPolicy_GetSnapshot(&snap);
    if (snap.gameplay_delay_mode != Net::GameplayDelayMode::AsymmetricExpert) {
        return;
    }
    if (telemetry.rb_frame_current - s_lastAsymDelayLogRb < 600) {
        return;
    }

    const float rttForOneWay = telemetry.rtt_p90_ms > 0.0f
        ? telemetry.rtt_p90_ms
        : (telemetry.rtt_avg_ms > 0.0f ? telemetry.rtt_avg_ms : telemetry.gekko_avg_ping);
    const float oneWayP90Frames = (rttForOneWay * 0.5f) / Net::FRAME_TIME_MS;
    const float localPredictsRemote =
        (std::max)(0.0f, oneWayP90Frames - (float)snap.effective_remote_delay);
    const float remotePredictsLocal =
        (std::max)(0.0f, oneWayP90Frames - (float)snap.effective_local_delay);

    s_lastAsymDelayLogRb = telemetry.rb_frame_current;
    Rollback::NetplayLog_Write(
        "ASYMDELAY",
        telemetry.rb_frame_current,
        "rb=%d local_visible=%d local_eff=%d remote_visible=%d remote_eff=%d ow_p90=%.2f local_predicts_remote=%.2f remote_predicts_local=%.2f",
        telemetry.rb_frame_current,
        snap.resolved_visible_local_delay,
        snap.effective_local_delay,
        snap.resolved_visible_remote_delay,
        snap.effective_remote_delay,
        oneWayP90Frames,
        localPredictsRemote,
        remotePredictsLocal);
}

static void LogDecision(const Rollback::RollbackTimesyncTelemetry& telemetry,
                        Net::NetplayPacingAction action,
                        const char* reason) {
    const bool hold = action != Net::NetplayPacingAction::None;
    const bool interval = telemetry.rb_frame_current - s_lastDecisionLogRb >= 120;
    if (!hold && s_predictionDebt <= 0 && !interval) {
        return;
    }
    if (!hold && s_predictionDebt > 0 && telemetry.rb_frame_current - s_lastDecisionLogRb < 30) {
        return;
    }

    s_lastDecisionLogRb = telemetry.rb_frame_current;
    Rollback::NetplayLog_Write(
        "PACEDECIDE",
        telemetry.rb_frame_current,
        "rb=%d decision=%s debt=%d pressure=%.2f soft_threshold=%d hard_threshold=%d reason=%s class=%s frames_ahead=%.2f",
        telemetry.rb_frame_current,
        Net::NetplayPacingActionName(action),
        s_predictionDebt,
        s_pressure,
        s_softThreshold,
        s_hardThreshold,
        reason ? reason : "advance",
        Net::NetQualityName(s_quality),
        telemetry.frames_ahead);
}

static void ResetDebtController() {
    s_pressure = 0.0f;
    s_softHoldActive = false;
    s_hardHoldActive = false;
    s_softHoldCount = 0;
    s_hardHoldCount = 0;
    s_lastSoftHoldRb = -1000000;
    s_lastHoldRb = -1000000;
    s_lastAction = Net::NetplayPacingAction::None;
    memset(s_debtHistory, 0, sizeof(s_debtHistory));
    s_debtHistoryPos = 0;
    s_debtHistoryCount = 0;
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
    s_rawRemoteGap = 0;
    s_predictionDebt = 0;
    s_effectiveRemoteDelay = 0;
    s_rollbackBudget = 0;
    s_softThreshold = 0;
    s_hardThreshold = 0;
    ResetDebtController();

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

static void ApplyTickSlew(const Rollback::RollbackTimesyncTelemetry& telemetry,
                          bool clampSpeedup) {
    const float frames = telemetry.frames_ahead;
    float targetAdjustMs = 0.0f;
    if (fabsf(frames) >= NetplayPacingController::kDeadbandFrames) {
        targetAdjustMs = (std::clamp)(
            frames * NetplayPacingController::kGainMsPerFrame,
            -NetplayPacingController::kMaxAdjustMs,
            +NetplayPacingController::kMaxAdjustMs);
    }

    if (clampSpeedup && targetAdjustMs < 0.0f) {
        targetAdjustMs = 0.0f;
    }

    s_controller.filtered_adjust_ms +=
        (targetAdjustMs - s_controller.filtered_adjust_ms) * NetplayPacingController::kAdjustEmaAlpha;

    if (clampSpeedup && s_controller.filtered_adjust_ms < 0.0f) {
        s_controller.filtered_adjust_ms = 0.0f;
    }

    float targetFrameMs = NetplayPacingController::kBaseFrameMs + s_controller.filtered_adjust_ms;
    if (targetFrameMs < 1.0f) {
        targetFrameMs = 1.0f;
    }

    float targetScale = NetplayPacingController::kBaseFrameMs / targetFrameMs;
    targetScale = (std::clamp)(
        targetScale,
        NetplayPacingController::kScaleMin,
        clampSpeedup ? 1.0f : NetplayPacingController::kScaleMax);

    SetNetplayPacingActive(true);
    SetNetplayTickScaleTarget(targetScale);

    s_controller.active = true;
    s_controller.last_frames_ahead = frames;
}

static void LogEnable(const Rollback::RollbackTimesyncTelemetry& telemetry) {
    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    Rollback::NetplayLog_Write(
        "PACE",
        telemetry.rb_frame_current,
        "enable phase=%s rb=%d frames_ahead=%.2f debt=%d adjust_ms=%.2f target_scale=%.3f current_scale=%.3f",
        Net::MatchRollbackPhaseName(s_phase),
        telemetry.rb_frame_current,
        telemetry.frames_ahead,
        s_predictionDebt,
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
    if (!coarseInterval && !meaningfulSkew && s_predictionDebt <= 0) {
        return;
    }

    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    Rollback::NetplayLog_Write(
        "PACE",
        telemetry.rb_frame_current,
        "update rb=%d frames_ahead=%.2f debt=%d pressure=%.2f adjust_ms=%.2f target_scale=%.3f current_scale=%.3f",
        telemetry.rb_frame_current,
        telemetry.frames_ahead,
        s_predictionDebt,
        s_pressure,
        s_controller.filtered_adjust_ms,
        tickState.target_scale,
        tickState.current_scale);
}

} // anonymous namespace

namespace Net {

const char* NetQualityName(NetQuality quality) {
    switch (quality) {
        case NetQuality::StableLowPing:  return "StableLowPing";
        case NetQuality::StableHighPing: return "StableHighPing";
        case NetQuality::JitteryLowPing: return "JitteryLowPing";
        case NetQuality::JitteryHighPing:return "JitteryHighPing";
        case NetQuality::Lossy:          return "Lossy";
        case NetQuality::Severe:         return "Severe";
        case NetQuality::Unknown:
        default:                         return "Unknown";
    }
}

const char* NetplayPacingActionName(NetplayPacingAction action) {
    switch (action) {
        case NetplayPacingAction::SoftHold:  return "soft_hold";
        case NetplayPacingAction::HardHold:  return "hard_hold";
        case NetplayPacingAction::StallHold: return "stall_hold";
        case NetplayPacingAction::None:
        default:                             return "advance";
    }
}

void NetplayPacing_Init() {
    s_initialized = true;
    s_phase = MatchRollbackPhase::None;
    s_localModeLogged = false;
    s_stallActive = false;
    s_stallFrameCount = 0;
    s_stallGap = 0;
    s_stallThreshold = 0;
    s_controller = NetplayPacingController{};
    s_quality = NetQuality::Unknown;
    s_profileSourceAvgOnly = true;
    ResetDebtController();
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
    s_quality = NetQuality::Unknown;
    s_lastNetClassLogRb = -1000000;
    s_lastDebtLogRb = -1000000;
    s_lastDecisionLogRb = -1000000;
    s_lastAsymDelayLogRb = -1000000;
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
    s_lastAction = NetplayPacingAction::None;
    s_softHoldActive = false;
    s_hardHoldActive = false;

    const bool interactive = IsInteractivePacingEnabled(phase, startupBarrierReleased);
    if (!interactive) {
        const char* reason = startupBarrierReleased ? "non_interactive" : "startup_or_lockstep";
        DeactivatePacing(phase, reason, NetplayPhaseRuntime_IsRollbackOwnedPhase(phase));
        return NetplayPacingAction::None;
    }

    UpdateDebtState(telemetry, stallThreshold);

    const NetQuality prevQuality = s_quality;
    s_quality = ClassifyQuality(telemetry, &s_profileSourceAvgOnly);
    const PacingProfile profile = ProfileFor(s_quality);
    s_softThreshold = profile.soft_threshold;
    s_hardThreshold = (std::min)(profile.hard_threshold, (std::max)(1, s_rollbackBudget - 1));

    LogNetClassIfNeeded(telemetry, prevQuality);
    LogDebtIfNeeded(telemetry);
    LogAsymDelayIfNeeded(telemetry);

    if (stallThreshold >= 0 &&
        telemetry.rb_frame_last_remote_received >= 0 &&
        s_rawRemoteGap > stallThreshold) {
        s_stallFrameCount++;
        s_stallActive = true;
        s_lastAction = NetplayPacingAction::StallHold;
        s_lastHoldRb = telemetry.rb_frame_current;
        LogDecision(telemetry, s_lastAction, "emergency_stall_threshold");
        if (s_stallFrameCount <= 5 || (s_stallFrameCount % 120) == 0) {
            Rollback::NetplayLog_Write(
                "PACE",
                telemetry.rb_frame_current,
                "stall_hold phase=%s rb=%d remote_rb=%d gap=%d threshold=%d debt=%d frames_ahead=%.2f",
                MatchRollbackPhaseName(phase),
                telemetry.rb_frame_current,
                telemetry.rb_frame_last_remote_received,
                s_rawRemoteGap,
                stallThreshold,
                s_predictionDebt,
                telemetry.frames_ahead);
        }
        return s_lastAction;
    }

    if (s_stallActive) {
        Rollback::NetplayLog_Write(
            "PACE",
            telemetry.rb_frame_current,
            "stall_recovered phase=%s rb=%d held_frames=%d gap=%d threshold=%d",
            MatchRollbackPhaseName(phase),
            telemetry.rb_frame_current,
            s_stallFrameCount,
            s_rawRemoteGap,
            stallThreshold);
        s_stallActive = false;
        s_stallFrameCount = 0;
    }

    PushDebtHistory(s_predictionDebt >= profile.soft_threshold);
    const int persistentCount = CountRecentDebtSamples(profile.persistence_window);
    const bool debtPersistent = persistentCount >= profile.persistence_required_count;

    if (s_predictionDebt >= s_hardThreshold) {
        s_hardHoldCount++;
        s_hardHoldActive = true;
        s_lastAction = NetplayPacingAction::HardHold;
        s_lastHoldRb = telemetry.rb_frame_current;
        LogDecision(telemetry, s_lastAction, "near_rollback_budget");
        return s_lastAction;
    }

    const bool enoughGapSinceSoftHold =
        (telemetry.rb_frame_current - s_lastSoftHoldRb) >= profile.min_soft_hold_gap_rb_frames;
    const bool sameFrameAlreadyHeld = s_lastHoldRb == telemetry.rb_frame_current;
    const bool softCandidate =
        profile.allow_soft_hold &&
        s_predictionDebt >= profile.soft_threshold &&
        debtPersistent &&
        enoughGapSinceSoftHold &&
        !sameFrameAlreadyHeld;

    if (softCandidate) {
        const float add = (std::max)(0.0f, (float)(s_predictionDebt - profile.soft_threshold + 1)) *
                          profile.pressure_add_scale;
        s_pressure += add;
    } else {
        s_pressure = (std::max)(0.0f, s_pressure - profile.pressure_decay);
    }

    if (softCandidate && s_pressure >= profile.hold_cost) {
        s_pressure = (std::max)(0.0f, s_pressure - profile.hold_cost);
        s_softHoldCount++;
        s_softHoldActive = true;
        s_lastSoftHoldRb = telemetry.rb_frame_current;
        s_lastHoldRb = telemetry.rb_frame_current;
        s_lastAction = NetplayPacingAction::SoftHold;
        LogDecision(telemetry, s_lastAction, "persistent_debt");
        return s_lastAction;
    }

    LogDecision(telemetry, NetplayPacingAction::None,
                s_predictionDebt > profile.target_debt ? "debt_observed" : "within_target");
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

    const bool firstEnable = !s_controller.active;
    ApplyTickSlew(telemetry, false);

    if (firstEnable) {
        LogEnable(telemetry);
    }
    LogUpdate(telemetry);
}

void NetplayPacing_OnHoldSample(
    const Rollback::RollbackTimesyncTelemetry& telemetry,
    MatchRollbackPhase phase,
    bool startupBarrierReleased,
    NetplayPacingAction holdKind) {
    if (!s_initialized || holdKind == NetplayPacingAction::None) {
        return;
    }

    s_phase = phase;
    if (!IsInteractivePacingEnabled(phase, startupBarrierReleased)) {
        return;
    }

    ApplyTickSlew(telemetry, true);

    NetplayTickState tickState{};
    GetNetplayTickState(&tickState);
    Rollback::NetplayLog_Write(
        "PACE",
        telemetry.rb_frame_current,
        "hold_sample rb=%d action=%s debt=%d frames_ahead=%.2f pressure=%.2f target_scale=%.3f current_scale=%.3f",
        telemetry.rb_frame_current,
        NetplayPacingActionName(holdKind),
        s_predictionDebt,
        telemetry.frames_ahead,
        s_pressure,
        tickState.target_scale,
        tickState.current_scale);
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
    out->soft_hold_active = s_softHoldActive;
    out->hard_hold_active = s_hardHoldActive;
    out->frames_ahead = s_controller.last_frames_ahead;
    out->filtered_adjust_ms = s_controller.filtered_adjust_ms;
    out->pressure = s_pressure;
    out->stall_frame_count = s_stallFrameCount;
    out->soft_hold_count = s_softHoldCount;
    out->hard_hold_count = s_hardHoldCount;
    out->stall_gap = s_stallGap;
    out->stall_threshold = s_stallThreshold;
    out->raw_remote_gap = s_rawRemoteGap;
    out->prediction_debt = s_predictionDebt;
    out->effective_remote_delay = s_effectiveRemoteDelay;
    out->rollback_budget = s_rollbackBudget;
    out->soft_threshold = s_softThreshold;
    out->hard_threshold = s_hardThreshold;
    out->quality = s_quality;
    out->last_action = s_lastAction;
    CopyTickState(out);
}

} // namespace Net
