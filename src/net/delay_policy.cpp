/**
 * Alice Senki 2 - Delay Policy Implementation
 *
 * RTT-based delay recommendation, explicit negotiation, multi-stage change
 * state machine, safe boundary detection, and delay sync enforcement.
 * The gameplay bridge calls IsRollbackSynced/OnRollbackApplied to mark
 * delay as consumed. See delay_policy.h for details.
 */

#include "net/delay_policy.h"
#include "net/session_manager.h"
#include "net/sync_policy.h"
#include "net/match_lifecycle.h"
#include "core/game_state.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <algorithm>
#include <math.h>

namespace Net {

// ============================================================================
// Jitter Tracker (ring buffer for short-term RTT variance)
// ============================================================================

static constexpr int JITTER_WINDOW = 32;

struct JitterTracker {
    float    samples[JITTER_WINDOW];
    int      write_idx;
    int      count;
    float    smoothed_rtt;     // EWMA-smoothed RTT
    float    smoothed_jitter;  // EWMA-smoothed jitter (abs deviation from smoothed RTT)

    void Reset() {
        for (int i = 0; i < JITTER_WINDOW; i++) samples[i] = 0.0f;
        write_idx = 0;
        count = 0;
        smoothed_rtt = 0.0f;
        smoothed_jitter = 0.0f;
    }

    void Feed(float rtt_ms) {
        // EWMA smoothing factor — 0.1 = slow adaptation, good for stable baseline
        constexpr float ALPHA_RTT    = 0.1f;
        constexpr float ALPHA_JITTER = 0.1f;

        if (count == 0) {
            smoothed_rtt = rtt_ms;
            smoothed_jitter = 0.0f;
        } else {
            float deviation = fabsf(rtt_ms - smoothed_rtt);
            smoothed_jitter = ALPHA_JITTER * deviation + (1.0f - ALPHA_JITTER) * smoothed_jitter;
            smoothed_rtt = ALPHA_RTT * rtt_ms + (1.0f - ALPHA_RTT) * smoothed_rtt;
        }

        samples[write_idx] = rtt_ms;
        write_idx = (write_idx + 1) % JITTER_WINDOW;
        if (count < JITTER_WINDOW) count++;
    }

    float GetSmoothedRTT() const { return smoothed_rtt; }
    float GetJitter() const { return smoothed_jitter; }
    bool  IsValid() const { return count >= 4; }  // Need a few samples
};

// ============================================================================
// Internal State
// ============================================================================

// User configuration (persists across sessions)
static int s_configuredDelay     = DELAY_DEFAULT_PREF;  // 0 = auto
static int s_rollbackBudget      = ROLLBACK_BUDGET_DEFAULT;
static int s_rollbackDelay       = 0;  // Input pipeline delay (CCCaster-style)

// Network measurement
static JitterTracker s_jitter;
static float s_lastRttMs         = 0.0f;
static float s_lastVarianceMs    = 0.0f;
static int   s_recommendedDelay  = 0;
static bool  s_measurementValid  = false;

// Session negotiation
static int  s_agreedDelay        = 0;
static int  s_agreedRollback     = ROLLBACK_BUDGET_DEFAULT;
static int  s_agreedRollbackDelay = 0;  // Agreed input pipeline delay
static bool s_sessionNegotiated  = false;

// Active delay (consumed by rollback session via gameplay bridge)
static int  s_activeDelay        = 0;
static int  s_rollbackCurrentDelay  = -1;  // Last delay value consumed by rollback session
static bool s_rollbackSynced        = false; // true when rollback session has consumed active_delay

// Pending next-match delay (committed for next match, not yet active)
static int  s_pendingNextDelay   = 0;
static bool s_hasPendingNext     = false;

// Change state machine
static DelayChangeState s_changeState  = DelayChangeState::Idle;
static int              s_changeTarget = 0;

// Stats
static uint32_t         s_changesApplied = 0;
static DelaySafeBoundary s_lastBoundary  = DelaySafeBoundary::None;

// Previous-frame state for edge detection
static uint32_t s_prevSubstate    = 0xFFFFFFFF;
static bool     s_prevTransition  = false;

static bool s_initialized = false;

// ============================================================================
// Helpers
// ============================================================================

static int ClampDelay(int delay) {
    if (delay < DELAY_MIN) return DELAY_MIN;
    if (delay > DELAY_MAX) return DELAY_MAX;
    return delay;
}

static int ClampRollback(int frames) {
    if (frames < ROLLBACK_BUDGET_MIN) return ROLLBACK_BUDGET_MIN;
    if (frames > ROLLBACK_BUDGET_MAX) return ROLLBACK_BUDGET_MAX;
    return frames;
}

/// Compute recommended delay from one-way trip time + jitter + safety margin.
///
/// Model:
///   one_way = smoothed_rtt / 2
///   variance_margin = jitter * 1.5 (covers ~90th percentile spikes)
///   total_latency = one_way + variance_margin
///   recommended_frames = ceil(total_latency / frame_time) + 1 safety frame
///
/// The +1 safety frame ensures that even under modest jitter, inputs arrive
/// before the frame they're needed. Without it, borderline connections would
/// alternate between 0 and 1 rollback frames constantly.
static int ComputeRecommendedFromMeasurement(float smoothed_rtt, float jitter) {
    float one_way = smoothed_rtt * 0.5f;
    float variance_margin = jitter * 1.5f;
    float total_latency_ms = one_way + variance_margin;

    int frames = (int)ceilf(total_latency_ms / FRAME_TIME_MS);
    frames += 1;  // Safety margin

    return ClampDelay(frames);
}

/// Detect safe boundary from real mode+substate, using sync_policy context.
static DelaySafeBoundary DetectSafeBoundary() {
    uint32_t mode = GetGameMode();
    uint32_t sub  = GetSubstate();

    // Front-end lockstep states (CharSel, StageSel within CharSel, pause outside match)
    // Changes here apply to the NEXT rollback match.
    SyncMode syncMode = SyncPolicy_GetCurrentMode();
    if (syncMode == SyncMode::Lockstep) {
        LockstepContext ctx = SyncPolicy_GetLockstepContext();
        // During CharSel/StageSel lockstep, delay changes are safe because
        // no rollback session is running.
        if (ctx == LockstepContext::CharSelect ||
            ctx == LockstepContext::StageSelect ||
            ctx == LockstepContext::PostMatch ||
            ctx == LockstepContext::WinScreen) {
            return DelaySafeBoundary::FrontEndLockstep;
        }
        // Pause during gameplay — both peers halted
        if (ctx == LockstepContext::Pause) {
            return DelaySafeBoundary::Pause;
        }
    }

    // Match-internal boundaries
    if (mode == MODE_MATCH) {
        // Post-match (sub 5)
        if (sub == MATCH_SUB_END) {
            return DelaySafeBoundary::PostMatch;
        }

        // Round start edge: Sub 2 (INIT) → Sub 3 (GAMEPLAY)
        if (sub == MATCH_SUB_GAMEPLAY && s_prevSubstate == MATCH_SUB_INIT) {
            return DelaySafeBoundary::RoundStart;
        }

        // Post-round edge: transition byte went 0→1
        if (sub == MATCH_SUB_GAMEPLAY && IsMatchTransitionActive() && !s_prevTransition) {
            return DelaySafeBoundary::PostRound;
        }
    }

    // Win screen (Mode 9)
    if (mode == MODE_WINSCREEN) {
        return DelaySafeBoundary::PostMatch;
    }

    return DelaySafeBoundary::None;
}

/// Transition change state machine forward when a safe boundary is detected.
static void TryCommitAtBoundary(DelaySafeBoundary boundary) {
    if (s_changeState == DelayChangeState::Pending) {
        // Committed — ready to apply
        s_changeState = DelayChangeState::Committed;
        s_lastBoundary = boundary;

        bool inRollbackMatch = SyncPolicy_IsRollbackActive();

        if (inRollbackMatch) {
            // Active rollback: apply immediately (rollback session will pick it up)
            s_activeDelay = ClampDelay(s_changeTarget);
            s_rollbackSynced = false;  // Rollback session must re-sync
            s_changesApplied++;

            LOG_INFO("[DelayPolicy] Change committed to active match: delay=%d (boundary=%s, changes=%u)",
                s_activeDelay, DelaySafeBoundaryName(boundary), s_changesApplied);
        } else {
            // Lockstep/front-end: stage for next match
            s_pendingNextDelay = ClampDelay(s_changeTarget);
            s_hasPendingNext = true;
            s_changesApplied++;

            // Also update agreed delay since no rollback session is live
            s_agreedDelay = s_pendingNextDelay;

            LOG_INFO("[DelayPolicy] Change committed for next match: delay=%d (boundary=%s, changes=%u)",
                s_pendingNextDelay, DelaySafeBoundaryName(boundary), s_changesApplied);

            // In front-end, mark as applied since there's no rollback session to sync
            s_changeState = DelayChangeState::Applied;
        }
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void DelayPolicy_Init() {
    s_jitter.Reset();
    s_lastRttMs = 0.0f;
    s_lastVarianceMs = 0.0f;
    s_recommendedDelay = 0;
    s_measurementValid = false;

    s_agreedDelay = 0;
    s_agreedRollback = ROLLBACK_BUDGET_DEFAULT;
    s_agreedRollbackDelay = 0;
    s_sessionNegotiated = false;

    s_activeDelay = ClampDelay(s_rollbackDelay);
    s_rollbackCurrentDelay = -1;
    s_rollbackSynced = false;

    s_pendingNextDelay = 0;
    s_hasPendingNext = false;

    s_changeState = DelayChangeState::Idle;
    s_changeTarget = 0;

    s_changesApplied = 0;
    s_lastBoundary = DelaySafeBoundary::None;

    s_prevSubstate = 0xFFFFFFFF;
    s_prevTransition = false;

    s_initialized = true;
    LOG_INFO("[DelayPolicy] Initialized (configured=%d rollback_budget=%d rollback_delay=%d active=%d)",
        s_configuredDelay, s_rollbackBudget, s_rollbackDelay, s_activeDelay);
}

void DelayPolicy_Shutdown() {
    s_initialized = false;
    LOG_INFO("[DelayPolicy] Shutdown (changes=%u)", s_changesApplied);
}

// ============================================================================
// Per-Frame Update
// ============================================================================

void DelayPolicy_FrameUpdate() {
    if (!s_initialized) return;

    if (Session_IsConnected()) {
        ConnectionStats stats{};
        Session_GetStats(&stats);
        DelayPolicy_UpdateMeasurement(stats.rtt_ms, stats.rtt_variance_ms);
    }

    // Detect safe boundaries and try to advance change state machine
    DelaySafeBoundary boundary = DetectSafeBoundary();

    if (boundary != DelaySafeBoundary::None && s_changeState == DelayChangeState::Pending) {
        TryCommitAtBoundary(boundary);
    }

    // If active delay changed and rollback session is out of sync,
    // The gameplay bridge will detect this via IsRollbackSynced() and
    // call OnRollbackApplied(). We do NOT auto-apply here.

    // When transitioning from front-end to rollback match, apply pending next delay
    if (s_hasPendingNext && SyncPolicy_IsRollbackActive()) {
        s_activeDelay = s_pendingNextDelay;
        s_hasPendingNext = false;
        s_pendingNextDelay = 0;
        s_rollbackSynced = false;  // Rollback session must re-sync with new value

        LOG_INFO("[DelayPolicy] Applied pending next-match delay: %d", s_activeDelay);
    }

    // Edge detection state for next frame
    if (GetGameMode() == MODE_MATCH) {
        s_prevSubstate = GetSubstate();
        s_prevTransition = IsMatchTransitionActive();
    } else {
        s_prevSubstate = 0xFFFFFFFF;
        s_prevTransition = false;
    }
}

// ============================================================================
// Network Measurement
// ============================================================================

void DelayPolicy_UpdateMeasurement(float rtt_ms, float rtt_variance_ms) {
    if (!s_initialized) return;

    const bool prevValid = s_measurementValid;
    const int prevRecommended = s_recommendedDelay;

    s_lastRttMs = rtt_ms;
    s_lastVarianceMs = rtt_variance_ms;

    if (rtt_ms <= 0.0f) {
        return;
    }

    s_jitter.Feed(rtt_ms);
    s_measurementValid = s_jitter.IsValid();

    if (s_measurementValid) {
        s_recommendedDelay = ComputeRecommendedFromMeasurement(
            s_jitter.GetSmoothedRTT(),
            s_jitter.GetJitter()
        );
    }

    if (prevValid != s_measurementValid) {
        Rollback::NetplayLog_StateChange(
            "DELAY", -1,
            "measurement_valid",
            prevValid ? "true" : "false",
            s_measurementValid ? "true" : "false",
            "ENet RTT sample window"
        );
    }

    if (s_measurementValid && prevRecommended != s_recommendedDelay) {
        LOG_INFO("[DelayPolicy] Recommended delay updated: %d -> %d (RTT=%.1fms jitter=%.1fms)",
            prevRecommended, s_recommendedDelay,
            s_jitter.GetSmoothedRTT(), s_jitter.GetJitter());
        Rollback::NetplayLog_ValueChange(
            "DELAY", -1,
            "recommended_delay",
            prevRecommended,
            s_recommendedDelay,
            "RTT/jitter measurement update"
        );
    }

    // Rate-limit verbose measurement log to every ~2s (120 frames at 60fps)
    // to avoid flooding the log file during normal gameplay.
    static uint32_t s_measureLogCounter = 0;
    if ((s_measureLogCounter++ % 120) == 0) {
        Rollback::NetplayLog_Verbose("DELAY", -1,
            "Measurement: rtt=%.1fms smoothed=%.1fms variance=%.1fms jitter=%.1fms one_way=%.1fms recommended=%d valid=%d",
            rtt_ms,
            s_jitter.GetSmoothedRTT(),
            rtt_variance_ms,
            s_jitter.GetJitter(),
            s_jitter.GetSmoothedRTT() * 0.5f,
            s_recommendedDelay,
            s_measurementValid ? 1 : 0);
    }
}

int DelayPolicy_ComputeRecommendedDelay() {
    if (!s_measurementValid) return 2;  // Conservative default before measurement
    return s_recommendedDelay;
}

void DelayPolicy_GetMeasurement(NetworkMeasurement* out) {
    if (!out) return;
    out->rtt_ms = s_jitter.GetSmoothedRTT();
    out->rtt_variance_ms = s_lastVarianceMs;
    out->jitter_ms = s_jitter.GetJitter();
    out->one_way_ms = s_jitter.GetSmoothedRTT() * 0.5f;
    out->recommended_delay = s_recommendedDelay;
    out->valid = s_measurementValid;
}

// ============================================================================
// User Configuration
// ============================================================================

void DelayPolicy_SetConfiguredDelay(int delay) {
    s_configuredDelay = (delay == 0) ? 0 : ClampDelay(delay);
    LOG_INFO("[DelayPolicy] Configured delay set to %d (%s)",
        s_configuredDelay, s_configuredDelay == 0 ? "auto" : "manual");
}

int DelayPolicy_GetConfiguredDelay() {
    return s_configuredDelay;
}

void DelayPolicy_SetRollbackBudget(int frames) {
    s_rollbackBudget = ClampRollback(frames);
    LOG_INFO("[DelayPolicy] Rollback budget set to %d", s_rollbackBudget);
}

int DelayPolicy_GetRollbackBudget() {
    return s_rollbackBudget;
}

void DelayPolicy_SetRollbackDelay(int frames) {
    const int prevRollbackDelay = s_rollbackDelay;
    const int prevActiveDelay = s_activeDelay;

    s_rollbackDelay = ClampDelay(frames);

    // Rollback delay is the effective local input pipeline delay for rollback.
    // Apply immediately in policy state so startup negotiation and live session
    // updates consume the newest value.
    s_activeDelay = s_rollbackDelay;
    if (s_activeDelay != prevActiveDelay) {
        s_rollbackSynced = false;
    }

    LOG_INFO("[DelayPolicy] Rollback delay set: cfg=%d->%d active=%d->%d",
        prevRollbackDelay, s_rollbackDelay, prevActiveDelay, s_activeDelay);
}

int DelayPolicy_GetRollbackDelay() {
    return s_rollbackDelay;
}

int DelayPolicy_ComputeSuggestedRollbackDelay() {
    int rec = DelayPolicy_ComputeRecommendedDelay();
    int residual = rec - s_rollbackBudget;
    return (residual < 0) ? 0 : residual;
}

// ============================================================================
// Session Negotiation
// ============================================================================

void DelayPolicy_BuildNegotiationData(DelayNegotiationData* out) {
    if (!out) return;

    int effective_pref = s_configuredDelay;
    if (effective_pref == 0) {
        // Auto mode: use recommended from measurement
        effective_pref = DelayPolicy_ComputeRecommendedDelay();
    }

    out->configured_delay  = s_configuredDelay;
    out->recommended_delay = DelayPolicy_ComputeRecommendedDelay();
    out->min_acceptable    = DELAY_MIN;
    out->max_acceptable    = DELAY_MAX;
    out->rollback_budget   = s_rollbackBudget;
    out->rollback_delay    = s_rollbackDelay;
}

void DelayPolicy_NegotiateSession(const DelayNegotiationData* remote) {
    if (!remote) return;

    // Resolve local effective preference
    int local_effective = s_configuredDelay;
    if (local_effective == 0) {
        local_effective = DelayPolicy_ComputeRecommendedDelay();
    }

    int remote_effective = remote->configured_delay;
    if (remote_effective == 0) {
        remote_effective = remote->recommended_delay;
    }

    // Agreed delay: use the maximum of both peers' effective preferences.
    // This ensures both peers have enough time for their inputs to arrive.
    // Then clamp to the intersection of both peers' acceptable ranges.
    int agreed = (std::max)(local_effective, remote_effective);

    int remote_range_min = remote->min_acceptable;
    int remote_range_max = remote->max_acceptable;

    // Current bootstrap packets only carry configured/recommended/rollback.
    // If the remote range arrives as the zero-initialized default 0..0,
    // treat that as "unspecified" and fall back to the shared global range
    // instead of collapsing the negotiated delay to zero.
    if (remote_range_max <= remote_range_min) {
        remote_range_min = DELAY_MIN;
        remote_range_max = DELAY_MAX;
    }

    int range_min = (std::max)(DELAY_MIN, remote_range_min);
    int range_max = (std::min)(DELAY_MAX, remote_range_max);
    if (agreed < range_min) agreed = range_min;
    if (agreed > range_max) agreed = range_max;

    agreed = ClampDelay(agreed);

    // Rollback budget: use the minimum of both peers' budgets.
    // Both peers must be able to handle the agreed rollback depth.
    int agreed_rb = (std::min)(s_rollbackBudget, remote->rollback_budget);
    agreed_rb = ClampRollback(agreed_rb);

    s_agreedDelay = agreed;
    s_agreedRollback = agreed_rb;
    s_agreedRollbackDelay = s_rollbackDelay;  // Each peer uses its own rollback delay
    s_activeDelay = ClampDelay(s_rollbackDelay);  // Active delay = rollback delay (input pipeline)
    s_sessionNegotiated = true;
    s_rollbackSynced = false;  // Rollback session hasn't applied this yet

    LOG_INFO("[DelayPolicy] Session negotiated:"
        " local_eff=%d remote_eff=%d agreed=%d"
        " range=[%d,%d] remote_range=[%d,%d]"
        " local_rec=%d remote_rec=%d"
        " rollback=%d rollback_delay=%d active_delay=%d"
        " rtt=%.1fms jitter=%.1fms",
        local_effective, remote_effective, agreed,
        DELAY_MIN, DELAY_MAX, remote_range_min, remote_range_max,
        s_recommendedDelay, remote->recommended_delay,
        agreed_rb, s_rollbackDelay, s_activeDelay,
        s_jitter.GetSmoothedRTT(), s_jitter.GetJitter());
}

int DelayPolicy_GetAgreedDelay() {
    return s_agreedDelay;
}

// ============================================================================
// Active Delay (rollback-session-facing)
// ============================================================================

int DelayPolicy_GetActiveDelay() {
    return s_activeDelay;
}

int DelayPolicy_GetAgreedRollbackBudget() {
    return s_agreedRollback;
}

int DelayPolicy_GetAgreedRollbackDelay() {
    return s_agreedRollbackDelay;
}

bool DelayPolicy_IsRollbackSynced() {
    return s_rollbackSynced;
}

void DelayPolicy_OnRollbackApplied(int delay_value) {
    s_rollbackCurrentDelay = delay_value;
    s_rollbackSynced = (delay_value == s_activeDelay);

    if (s_changeState == DelayChangeState::Committed) {
        s_changeState = DelayChangeState::Applied;
        LOG_INFO("[DelayPolicy] Delay consumed by rollback session: delay=%d (synced=%s)",
            delay_value, s_rollbackSynced ? "yes" : "no");
    }
}

// ============================================================================
// Mid-Session Delay Changes
// ============================================================================

void DelayPolicy_RequestChange(int new_delay) {
    int clamped = ClampDelay(new_delay);
    if (clamped == s_activeDelay && !s_hasPendingNext) {
        return;  // No change needed
    }

    if (s_changeState != DelayChangeState::Idle &&
        s_changeState != DelayChangeState::Applied) {
        LOG_INFO("[DelayPolicy] Change request rejected: already in state %s",
            DelayChangeStateName(s_changeState));
        return;
    }

    s_changeTarget = clamped;
    s_changeState = DelayChangeState::Requested;

    LOG_INFO("[DelayPolicy] Change requested: %d -> %d (state=Requested, sync_mode=%s)",
        s_activeDelay, clamped, SyncModeName(SyncPolicy_GetCurrentMode()));
}

void DelayPolicy_OnRemoteAck(int acked_delay) {
    if (s_changeState != DelayChangeState::Requested) {
        LOG_INFO("[DelayPolicy] Remote ack ignored: not in Requested state (state=%s)",
            DelayChangeStateName(s_changeState));
        return;
    }

    if (acked_delay != s_changeTarget) {
        LOG_INFO("[DelayPolicy] Remote ack mismatch: expected=%d got=%d, renegotiating",
            s_changeTarget, acked_delay);
        // Take the max — conservative approach
        s_changeTarget = ClampDelay((std::max)(s_changeTarget, acked_delay));
    }

    s_changeState = DelayChangeState::Pending;
    LOG_INFO("[DelayPolicy] Change pending: target=%d (awaiting safe boundary)",
        s_changeTarget);
}

DelayChangeState DelayPolicy_GetChangeState() {
    return s_changeState;
}

int DelayPolicy_GetChangeTarget() {
    return s_changeTarget;
}

bool DelayPolicy_IsSafeBoundaryAvailable() {
    return DetectSafeBoundary() != DelaySafeBoundary::None;
}

DelaySafeBoundary DelayPolicy_GetCurrentSafeBoundary() {
    return DetectSafeBoundary();
}

int DelayPolicy_GetPendingNextDelay() {
    return s_hasPendingNext ? s_pendingNextDelay : s_activeDelay;
}

bool DelayPolicy_HasPendingNextDelay() {
    return s_hasPendingNext;
}

// ============================================================================
// Session Reset
// ============================================================================

void DelayPolicy_ResetSession() {
    // Keep user configuration (s_configuredDelay, s_rollbackBudget, s_rollbackDelay)
    // Reset everything else
    s_jitter.Reset();
    s_lastRttMs = 0.0f;
    s_lastVarianceMs = 0.0f;
    s_recommendedDelay = 0;
    s_measurementValid = false;

    s_agreedDelay = 0;
    s_agreedRollback = ROLLBACK_BUDGET_DEFAULT;
    s_agreedRollbackDelay = 0;
    s_sessionNegotiated = false;

    s_activeDelay = ClampDelay(s_rollbackDelay);
    s_rollbackCurrentDelay = -1;
    s_rollbackSynced = false;

    s_pendingNextDelay = 0;
    s_hasPendingNext = false;

    s_changeState = DelayChangeState::Idle;
    s_changeTarget = 0;

    s_changesApplied = 0;
    s_lastBoundary = DelaySafeBoundary::None;

    s_prevSubstate = 0xFFFFFFFF;
    s_prevTransition = false;

    LOG_INFO("[DelayPolicy] Session reset (rollback_delay=%d active_delay=%d)",
        s_rollbackDelay, s_activeDelay);
}

// ============================================================================
// Diagnostics
// ============================================================================

void DelayPolicy_GetSnapshot(DelayPolicySnapshot* out) {
    if (!out) return;

    out->configured_delay    = s_configuredDelay;
    out->recommended_delay   = s_recommendedDelay;
    out->agreed_delay        = s_agreedDelay;
    out->active_delay        = s_activeDelay;
    out->pending_next_delay  = s_hasPendingNext ? s_pendingNextDelay : 0;

    out->rollback_budget     = s_rollbackBudget;
    out->agreed_rollback     = s_agreedRollback;
    out->rollback_delay      = s_rollbackDelay;

    out->change_state        = s_changeState;
    out->change_target_delay = s_changeTarget;

    out->rollback_synced        = s_rollbackSynced;
    out->rollback_current_delay = s_rollbackCurrentDelay;

    out->measured_rtt_ms     = s_jitter.GetSmoothedRTT();
    out->measured_jitter_ms  = s_jitter.GetJitter();
    out->measured_one_way_ms = s_jitter.GetSmoothedRTT() * 0.5f;
    out->measurement_valid   = s_measurementValid;

    out->changes_applied     = s_changesApplied;
    out->last_boundary       = s_lastBoundary;

    out->in_rollback_match   = SyncPolicy_IsRollbackActive();
    out->safe_boundary_available = (DetectSafeBoundary() != DelaySafeBoundary::None);
}

} // namespace Net
