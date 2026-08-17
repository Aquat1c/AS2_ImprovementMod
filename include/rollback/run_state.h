/**
 * Alice Senki 2 - Typed netplay run state + hold-episode ledger (re0.7 M0)
 *
 * Ported from qoh99_netplay `game/NetplayRunState.h` (the reference backend's
 * biggest observability lesson: label every hold with its true cause from day
 * one). Master plan §2.10.
 *
 * This does NOT decide to hold — the scheduler/engine decides from the same
 * facts; this classifier is deliberately downstream and observational only.
 * Native hit-freeze must never count as a hold: a native freeze does not stop
 * canonical frames, so it cannot produce a hold, so it cannot open an episode
 * (`native_freeze_active` is carried only so a test can pin that property).
 *
 * Pure and total: no I/O, no game calls, no clock of its own — the caller
 * supplies timestamps.
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// The closed hold-cause set (INV-2). Correction lateness is NOT a hold — it is
// absorbed by scheduler deadline rebase. No soft/hard/persistent-debt causes
// may ever be added here.
enum class HoldCause : uint8_t {
    None = 0,
    PredictionLimit,     // speculativeFrames >= R_local — the ONLY network hold
    LifecycleBoundary,   // exact-input window needs the remote actual
    LocalInputMissing,   // our own capture is late; not the network's fault
    ExternalSuspension,  // focus loss / device churn / OS discontinuity
};

constexpr const char* HoldCauseName(HoldCause c) {
    switch (c) {
        case HoldCause::None:              return "none";
        case HoldCause::PredictionLimit:   return "prediction-limit";
        case HoldCause::LifecycleBoundary: return "lifecycle-boundary";
        case HoldCause::LocalInputMissing: return "local-input-missing";
        case HoldCause::ExternalSuspension:return "external-suspension";
    }
    return "?";
}

// What the scheduler planned for this opportunity.
enum class WorkKind : uint8_t {
    Advance = 0,   // run one (or more) canonical frames
    Correct,       // spend the opportunity on a mismatch correction replay
    Hold,          // intentional 0-sim pass
};

// Master plan §2.10 run-state vocabulary. Classified once per pass from the
// plan; carried in PressureReport (`run_state` byte) once the v2 wire is live.
enum class RunState : uint8_t {
    Running = 0,           // canonical time advances normally
    CorrectionCatchUp,     // a mismatch correction is queued/running
    PredictionPressure,    // near the rollback ceiling (or local input late)
    ExactLifecycleBarrier, // exact-input window: may not be crossed on prediction
    ExternalSuspension,    // focus loss / process suspend / OS discontinuity
    EmergencyHold,         // at the ceiling with no actual remote input
    Terminated,            // session is over
};

constexpr const char* RunStateName(RunState s) {
    switch (s) {
        case RunState::Running:               return "running";
        case RunState::CorrectionCatchUp:     return "correction-catchup";
        case RunState::PredictionPressure:    return "prediction-pressure";
        case RunState::ExactLifecycleBarrier: return "lifecycle-barrier";
        case RunState::ExternalSuspension:    return "external-suspension";
        case RunState::EmergencyHold:         return "emergency-hold";
        case RunState::Terminated:            return "terminated";
    }
    return "?";
}

// True for the states in which canonical time is stopped. PredictionPressure
// is NOT stopped — that is the whole point of separating it from the hold —
// and CorrectionCatchUp spends an opportunity on replay, which is work.
constexpr bool RunStateStopsCanonicalTime(RunState s) {
    return s == RunState::EmergencyHold
        || s == RunState::ExactLifecycleBarrier
        || s == RunState::ExternalSuspension
        || s == RunState::Terminated;
}

// Exactly ONE state is a network emergency, so a routine lifecycle barrier or
// a user alt-tab can never inflate the emergency counters.
constexpr bool RunStateIsNetworkEmergency(RunState s) {
    return s == RunState::EmergencyHold;
}

struct RunStateInputs {
    // Speculation depth: nextCanonicalFrame - remoteActualThrough. Compared
    // against local_max_rollback (R_local).
    uint32_t missing_remote_depth = 0;
    uint32_t local_max_rollback = 0;
    // Pressure band: reaching (R_local - warning_margin) is the warning;
    // R_local itself is the hold. Margin 0 collapses the band (legal).
    uint32_t warning_margin = 2;
    // Owed hidden simulation frames (CadenceDebt) after a recoverable wait.
    uint32_t recoverable_debt_frames = 0;

    WorkKind  kind = WorkKind::Advance;
    HoldCause hold = HoldCause::None;

    bool session_terminated = false;
    bool external_suspension = false;
    // Carried ONLY so a test can prove the classifier never consults it.
    bool native_freeze_active = false;
};

// Total and pure. Precedence, most-authoritative first: session end outranks
// everything; an external suspension is not the network's fault and must not
// be counted as one; a lifecycle barrier is a design invariant; then the
// network states.
constexpr RunState ClassifyRunState(const RunStateInputs& in) {
    if (in.session_terminated)  return RunState::Terminated;
    if (in.external_suspension) return RunState::ExternalSuspension;

    if (in.kind == WorkKind::Hold) {
        switch (in.hold) {
            case HoldCause::LifecycleBoundary:
                return RunState::ExactLifecycleBarrier;
            case HoldCause::PredictionLimit:
                return RunState::EmergencyHold;
            case HoldCause::LocalInputMissing:
                // Our own input is late. Not a network emergency, but canonical
                // time IS stopped, so it must not read as Running. Pressure is
                // the honest bucket: still recoverable, locally.
                return RunState::PredictionPressure;
            case HoldCause::ExternalSuspension:
                return RunState::ExternalSuspension;
            case HoldCause::None:
                break;  // a retry poll says nothing about the session
        }
    }
    if (in.kind == WorkKind::Correct) return RunState::CorrectionCatchUp;

    // Advancing. Pressure outranks debt recovery: a peer repaying debt AND
    // approaching the ceiling is in the more urgent of the two.
    if (in.local_max_rollback != 0 &&
        in.missing_remote_depth + in.warning_margin >= in.local_max_rollback) {
        return RunState::PredictionPressure;
    }
    return RunState::Running;
}

// ── Hold-episode ledger ─────────────────────────────────────────────────────
//
// An episode is one contiguous run of network-emergency opportunities. Fixed
// size: aggregates plus current and worst episode — a session may sit in hold
// indefinitely, so a per-episode vector would be an unbounded allocation
// driven by the network.
class HoldEpisodeLedger {
public:
    struct Episode {
        uint32_t generation = 0;
        uint64_t start_us = 0;
        uint64_t end_us = 0;
        uint32_t first_missing_frame = 0;  // frame we were unable to run
        uint32_t local_max_rollback = 0;   // R_local at the time
        uint32_t opportunities_held = 0;
        uint32_t recovery_depth = 0;       // correction depth after resume

        uint64_t DurationUs() const {
            return end_us > start_us ? end_us - start_us : 0u;
        }
    };

    // One call per cadence opportunity, after classification. first_missing is
    // meaningful only in an emergency state; ignored otherwise.
    void Observe(RunState state, uint64_t now_us,
                 uint32_t first_missing, uint32_t max_rollback) {
        const bool emergency = RunStateIsNetworkEmergency(state);
        if (emergency) {
            if (!active_) {
                active_ = true;
                current_ = Episode{};
                current_.generation = ++generation_;
                current_.start_us = now_us;
                current_.first_missing_frame = first_missing;
                current_.local_max_rollback = max_rollback;
            }
            current_.end_us = now_us;
            ++current_.opportunities_held;
            ++held_opportunities_;
        } else if (active_) {
            // The episode ended between the last emergency sample and NOW;
            // observations are cadence-gated, so neither bound is exact. Take
            // the later one: over-reporting by up to one frame period errs
            // toward showing network cost rather than hiding it.
            active_ = false;
            current_.end_us = now_us;
            ++episodes_;
            total_held_us_ += current_.DurationUs();
            if (current_.DurationUs() > worst_.DurationUs()) worst_ = current_;
            awaiting_recovery_ = true;
        }
    }

    // Only the FIRST correction after an episode closes is recorded as that
    // episode's recovery depth (attributing every rollback to the last hold
    // would over-count wildly — mispredictions happen constantly).
    void ObserveRecovery(uint32_t rollback_depth) {
        if (!awaiting_recovery_) return;
        awaiting_recovery_ = false;
        current_.recovery_depth = rollback_depth;
        if (worst_.generation == current_.generation)
            worst_.recovery_depth = rollback_depth;
        if (rollback_depth > deepest_recovery_) deepest_recovery_ = rollback_depth;
    }

    void Reset() { *this = HoldEpisodeLedger{}; }

    bool Active() const { return active_; }
    uint32_t Episodes() const { return episodes_ + (active_ ? 1u : 0u); }
    uint32_t Generation() const { return generation_; }
    uint64_t TotalHeldUs() const {
        return total_held_us_ + (active_ ? current_.DurationUs() : 0u);
    }
    uint64_t HeldOpportunities() const { return held_opportunities_; }
    uint32_t DeepestRecovery() const { return deepest_recovery_; }
    const Episode& Current() const { return current_; }
    const Episode& Worst() const { return worst_; }

private:
    Episode current_{};
    Episode worst_{};
    uint32_t generation_ = 0;
    uint32_t episodes_ = 0;
    uint64_t total_held_us_ = 0;
    uint64_t held_opportunities_ = 0;
    uint32_t deepest_recovery_ = 0;
    bool active_ = false;
    bool awaiting_recovery_ = false;
};

} // namespace Rollback
