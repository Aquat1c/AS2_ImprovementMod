/**
 * Alice Senki 2 - FrameScheduler pure core (re0.7 M2, master plan §2.8)
 *
 * The clock-math half of the mod-owned frame scheduler, kept free of Win32 and
 * game includes so the T-SCHED unit tests exercise the exact objects the
 * runtime driver (src/patches/frame_scheduler.cpp) runs:
 *
 *   - DeadlineClock      — §2.8.2 absolute QPC deadlines with integer-exact
 *                          Bresenham cadence carry (zero cumulative drift by
 *                          construction) and the multi-frame lateness rebase.
 *   - PaceSlew           — §2.8.4 bounded one-sided pace slew (the ONLY
 *                          wall-clock regulator): deadband/hysteresis
 *                          admission, rise-rate limit, absolute cap, instant
 *                          release on stale samples.
 *   - CadenceDebtLedger  — §2.8.5 bounded hidden-catch-up ledger with the
 *                          typed-cause rebase gate (exactly one repayment
 *                          owner per missed frame, QOH99 lesson 3).
 *   - IntervalStats      — per-second present-interval percentile window for
 *                          the §2.10 STAT line.
 *
 * All time quantities are QPC ticks unless suffixed otherwise (units in
 * names, conversions at the caller — QOH99 lesson 8).
 */

#pragma once

#include <stdint.h>

#include <algorithm>

#include "rollback/run_state.h"

namespace Sched {

// ── Cadence profiles (§2.8.2) ───────────────────────────────────────────────
// Frame period = qpf * mul / div ticks, carried integer-exactly:
//   proper_60:  {1, 60}    → 60.000 Hz  (period = QPF/60,     r = QPF%60)
//   compat_58:  {17, 1000} → 58.82 Hz   (period = QPF*17/1000, r = (QPF*17)%1000)
// The profile is carried in the v2 handshake and must match on both peers
// (fail-closed, M3); locally it is selected by the shipping `proper_60fps` /
// frame_timing setting (§2.8.7).
struct CadenceProfile {
    uint32_t mul;
    uint32_t div;
    const char* name;
};

inline constexpr CadenceProfile kCadenceProper60{1u, 60u, "proper_60"};
inline constexpr CadenceProfile kCadenceCompat58{17u, 1000u, "compat_58"};

// ── DeadlineClock (§2.8.2) ──────────────────────────────────────────────────
class DeadlineClock {
public:
    void Configure(uint64_t qpf, const CadenceProfile& profile) {
        qpf_ = qpf;
        const uint64_t total = qpf * (uint64_t)profile.mul;
        period_ = total / profile.div;
        rem_ = total % profile.div;
        den_ = profile.div;
        // Sub-tick remainder restarts on profile change; the deadline itself
        // is never reset here (INV-17: rebases, never resets).
        frac_ = 0;
    }

    bool Configured() const { return period_ != 0; }
    bool Primed() const { return primed_; }
    uint64_t Qpf() const { return qpf_; }
    uint64_t PeriodTicks() const { return period_; }
    uint64_t Deadline() const { return deadline_; }

    void Prime(uint64_t now) {
        deadline_ = now;
        primed_ = true;
    }

    // Base step for the next frame with the Bresenham carry. Integer-exact:
    // any `div` consecutive steps sum to exactly qpf*mul ticks.
    uint64_t NextBaseStep() {
        uint64_t step = period_;
        frac_ += rem_;
        if (frac_ >= den_) {
            step += 1;
            frac_ -= den_;
        }
        return step;
    }

    struct AdvanceResult {
        uint64_t wait_until;      // 0 ⇒ run immediately (late)
        uint32_t rebased_frames;  // >0 only when rebase fired
        bool rebase;
    };

    // Advance the deadline by `effective_step` (base step already scaled by
    // manual speed / period adjust / slew at the caller) and classify:
    //   - on time            → wait_until = deadline
    //   - sub-frame late     → run immediately, deadline preserved (the next
    //                          wait shortens; long-term rate exact)
    //   - > 2 periods late   → rebase: deadline = now, report skipped frames.
    //                          Visible cadence is never compressed to repay
    //                          lateness (INV-21 corollary, QOH99 lesson 3).
    AdvanceResult Advance(uint64_t now, uint64_t effective_step) {
        AdvanceResult r{0, 0, false};
        if (!primed_) {
            Prime(now);
        }
        deadline_ += effective_step;
        if (now > deadline_) {
            const uint64_t late = now - deadline_;
            if (period_ != 0 && late > 2 * period_) {
                r.rebased_frames = (uint32_t)(late / period_);
                r.rebase = true;
                deadline_ = now;
            }
            r.wait_until = 0;
        } else {
            r.wait_until = deadline_;
        }
        return r;
    }

    // QPC regression / discontinuity: re-anchor spacing state without
    // touching cadence configuration. Rebase, never reset.
    void RebaseTo(uint64_t now) {
        deadline_ = now;
        primed_ = true;
    }

private:
    uint64_t qpf_ = 0;
    uint64_t period_ = 0;
    uint64_t rem_ = 0;
    uint64_t den_ = 1;
    uint64_t frac_ = 0;
    uint64_t deadline_ = 0;
    bool primed_ = false;
};

// ── PaceSlew (§2.8.4) ───────────────────────────────────────────────────────
// Only the lower-depth peer acts, by shortening its own period (running
// slightly fast so the peer predicts less). Never lengthens. Determinism is
// unaffected by construction: it changes how fast wall time is spent, never
// which frames run.
class PaceSlew {
public:
    static constexpr int32_t kDeadbandEnterFrames = 2;   // admit at lag ≥ 2
    static constexpr int32_t kReleaseBelowFrames = 1;    // release at lag < 1
    static constexpr int32_t kPpmPerDepthFrame = 2500;   // target correction
    static constexpr int32_t kMaxRisePpmPerStep = 8000;  // slew-rate rise cap
    static constexpr int32_t kMaxPpm = 25000;            // absolute cap (2.5%)

    // One call per pass, including stalls (the stalled side is exactly who
    // needs releasing). `sample_fresh` = peer PressureReport age within the
    // freshness bound (≤ 4 frames / 64 ms) — stale ⇒ instant release.
    int32_t Update(bool sample_fresh, int32_t peer_depth, int32_t local_depth) {
        if (!sample_fresh) {
            engaged_ = false;
            ppm_ = 0;
            return 0;
        }
        const int32_t lag = peer_depth - local_depth;
        if (!engaged_) {
            if (lag >= kDeadbandEnterFrames) {
                engaged_ = true;
            }
        } else if (lag < kReleaseBelowFrames) {
            engaged_ = false;
        }
        if (!engaged_) {
            ppm_ = 0;
            return 0;
        }
        int32_t target = lag * kPpmPerDepthFrame;
        if (target < 0) target = 0;
        if (target > kMaxPpm) target = kMaxPpm;
        if (target > ppm_) {
            const int32_t rise = target - ppm_;
            ppm_ += (rise > kMaxRisePpmPerStep) ? kMaxRisePpmPerStep : rise;
        } else {
            ppm_ = target;  // falls freely; only the rise is rate-limited
        }
        return ppm_;
    }

    void Reset() {
        engaged_ = false;
        ppm_ = 0;
    }

    int32_t Ppm() const { return ppm_; }
    bool Engaged() const { return engaged_; }

private:
    int32_t ppm_ = 0;
    bool engaged_ = false;
};

// ── CadenceDebtLedger (§2.8.5) ──────────────────────────────────────────────
class CadenceDebtLedger {
public:
    static constexpr uint32_t kMaxOwed = 8;
    static constexpr uint32_t kMaxCatchupExtraPerPass = 2;

    // Record the typed cause of the pass's hold. `create_debt` is the
    // caller's declaration that this hold owes a hidden frame (engine2
    // PredictionLimit holds at M4+; the M2 legacy-pacing shim passes false —
    // its holds are pure semanticHolds and the peer froze too, so there is
    // nothing to repay).
    void OnHold(Rollback::HoldCause cause, bool create_debt) {
        last_hold_cause_ = cause;
        last_hold_created_debt_ = create_debt;
        if (cause == Rollback::HoldCause::ExternalSuspension) {
            DiscardExternal();
            return;
        }
        if (create_debt && cause == Rollback::HoldCause::PredictionLimit &&
            owed_ < kMaxOwed) {
            ++owed_;
        }
    }

    void Consume(uint32_t n) { owed_ = owed_ > n ? owed_ - n : 0; }

    // Focus loss / device churn / churn-pause / load stalls: external causes
    // never leave repayment behind (QOH99 lesson 3).
    void DiscardExternal() {
        owed_ = 0;
        last_hold_cause_ = Rollback::HoldCause::None;
        last_hold_created_debt_ = false;
    }

    // The single entry converting deadline-rebase lateness into debt — and
    // only when the last hold cause was PredictionLimit (typed-cause gate:
    // external stalls stay discarded; exactly one repayment owner per missed
    // frame). Returns the frames actually converted.
    uint32_t ApplyRebasedFrames(uint32_t n) {
        if (last_hold_cause_ != Rollback::HoldCause::PredictionLimit ||
            !last_hold_created_debt_) {
            return 0;
        }
        uint32_t add = n;
        if (owed_ + add > kMaxOwed) add = kMaxOwed - owed_;
        owed_ += add;
        return add;
    }

    uint32_t Owed() const { return owed_; }
    Rollback::HoldCause LastHoldCause() const { return last_hold_cause_; }

    void Reset() {
        owed_ = 0;
        last_hold_cause_ = Rollback::HoldCause::None;
        last_hold_created_debt_ = false;
    }

private:
    uint32_t owed_ = 0;
    Rollback::HoldCause last_hold_cause_ = Rollback::HoldCause::None;
    bool last_hold_created_debt_ = false;
};

// ── IntervalStats — per-second percentile window for the STAT line ──────────
class IntervalStats {
public:
    static constexpr uint32_t kCapacity = 256;  // > 4 s of 60 Hz samples

    void Add(uint32_t interval_us) {
        if (count_ < kCapacity) {
            samples_[count_++] = interval_us;
        }
        // Window resets every emission second; overflow (impossible at frame
        // cadence) drops the tail rather than skewing the head.
    }

    uint32_t Count() const { return count_; }

    // p in [0,100]. Sorts a scratch copy — called once per second only.
    uint32_t Percentile(uint32_t p) const {
        if (count_ == 0) return 0;
        uint32_t scratch[kCapacity];
        for (uint32_t i = 0; i < count_; ++i) scratch[i] = samples_[i];
        std::sort(scratch, scratch + count_);
        uint32_t idx = (uint32_t)(((uint64_t)p * (count_ - 1) + 50) / 100);
        if (idx >= count_) idx = count_ - 1;
        return scratch[idx];
    }

    void Reset() { count_ = 0; }

private:
    uint32_t samples_[kCapacity] = {};
    uint32_t count_ = 0;
};

}  // namespace Sched
