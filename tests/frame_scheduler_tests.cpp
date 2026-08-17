// re0.7 M2 — unit tests for patches/frame_scheduler_core.h (T-SCHED-1..4).
// The pure core is exactly what the runtime driver runs, so these pins are
// the offline-verifiable half of the M2 exit gate:
//   T-SCHED-1: deadline exactness — zero cumulative drift over a simulated
//              long run, both cadence profiles.
//   T-SCHED-2: rebase threshold — sub-frame lateness preserves the schedule;
//              >2-period lateness rebases and reports skipped frames; visible
//              cadence is never compressed.
//   T-SCHED-3: semanticHold neutrality — holds without debt leave the ledger
//              untouched; the typed-cause rebase gate converts lateness into
//              debt ONLY after a debt-creating PredictionLimit hold; external
//              suspension discards.
//   T-SCHED-4: pace slew — deadband admission, rise-rate limit, absolute
//              cap, hysteretic release, instant release on stale samples.

#include "patches/frame_scheduler_core.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_checks = 0;
int g_failures = 0;

#define TEST_CHECK(cond, msg)                                             \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
        }                                                                 \
    } while (0)

constexpr uint64_t kQpf = 10000000ull;  // typical Windows QPC frequency

// ── T-SCHED-1: deadline exactness ───────────────────────────────────────────

void TestDeadlineExactnessProper60() {
    Sched::DeadlineClock clock;
    clock.Configure(kQpf, Sched::kCadenceProper60);
    TEST_CHECK(clock.PeriodTicks() == kQpf / 60, "proper_60 period = QPF/60");

    // 600,000 frames (10,000 whole seconds worth): the Bresenham carry must
    // make the total EXACTLY 10,000 * QPF ticks — zero cumulative drift.
    uint64_t total = 0;
    for (int i = 0; i < 600000; ++i) {
        total += clock.NextBaseStep();
    }
    TEST_CHECK(total == kQpf * 10000ull,
        "600k proper_60 steps sum to exactly 10000 s of ticks (drift = 0)");
}

void TestDeadlineExactnessCompat58() {
    Sched::DeadlineClock clock;
    clock.Configure(kQpf, Sched::kCadenceCompat58);
    TEST_CHECK(clock.PeriodTicks() == kQpf * 17 / 1000,
        "compat_58 period = QPF*17/1000");

    // 1,000,000 frames: total must be exactly QPF * 17 * 1000 ticks.
    uint64_t total = 0;
    for (int i = 0; i < 1000000; ++i) {
        total += clock.NextBaseStep();
    }
    TEST_CHECK(total == kQpf * 17ull * 1000ull,
        "1M compat_58 steps sum exactly (drift = 0)");
}

void TestDeadlineAdvanceOnTime() {
    Sched::DeadlineClock clock;
    clock.Configure(kQpf, Sched::kCadenceProper60);
    clock.Prime(1000);

    // Always arriving exactly at the deadline: wait_until must march at the
    // exact cadence with no rebase ever.
    uint64_t now = 1000;
    uint64_t expected = 1000;
    uint64_t frac = 0;
    bool ok = true;
    for (int i = 0; i < 60000; ++i) {
        uint64_t step = kQpf / 60;
        frac += kQpf % 60;
        if (frac >= 60) { step += 1; frac -= 60; }
        expected += step;

        const auto r = clock.Advance(now, step);
        if (r.rebase || r.wait_until != expected) { ok = false; break; }
        now = r.wait_until;
    }
    TEST_CHECK(ok, "on-time passes march at the exact cadence, zero rebases");
    TEST_CHECK(now == 1000 + kQpf * 1000ull,
        "60k on-time frames land exactly 1000 s after priming");
}

// ── T-SCHED-2: rebase threshold ─────────────────────────────────────────────

void TestSubFrameLatenessPreservesSchedule() {
    Sched::DeadlineClock clock;
    clock.Configure(kQpf, Sched::kCadenceProper60);
    clock.Prime(0);
    const uint64_t period = clock.PeriodTicks();

    // One pass arrives half a period late: no rebase, the deadline is NOT
    // re-anchored, so the next on-time pass sees a shortened wait and the
    // long-term schedule is preserved.
    auto r1 = clock.Advance(period / 2, period);  // deadline = period, now < deadline
    TEST_CHECK(!r1.rebase && r1.wait_until == period, "on-time first pass");

    // Late by 0.5 period at the second pass (now = 2.5 periods, deadline = 2).
    auto r2 = clock.Advance(2 * period + period / 2, period);
    TEST_CHECK(!r2.rebase, "sub-frame lateness does not rebase");
    TEST_CHECK(r2.wait_until == 0, "late pass runs immediately");
    TEST_CHECK(clock.Deadline() == 2 * period,
        "deadline preserved (next wait shortens; remainder kept)");

    // Third pass on time relative to the preserved schedule.
    auto r3 = clock.Advance(2 * period + period / 2, period);
    TEST_CHECK(!r3.rebase && r3.wait_until == 3 * period,
        "schedule re-converges without compression");
}

void TestMultiFrameLatenessRebases() {
    Sched::DeadlineClock clock;
    clock.Configure(kQpf, Sched::kCadenceProper60);
    clock.Prime(0);
    const uint64_t period = clock.PeriodTicks();

    // now = 6 periods past the (advanced) deadline → rebase, deadline = now,
    // reported frames = late / period.
    const uint64_t now = period + 6 * period;
    auto r = clock.Advance(now, period);
    TEST_CHECK(r.rebase, ">2-period lateness rebases");
    TEST_CHECK(r.rebased_frames == 6, "rebase reports late/period frames");
    TEST_CHECK(clock.Deadline() == now, "rebase re-anchors deadline to now");
    TEST_CHECK(r.wait_until == 0, "rebased pass runs immediately");
}

// ── T-SCHED-3: semanticHold neutrality / debt gate ──────────────────────────

void TestSemanticHoldCreatesNoDebt() {
    Sched::CadenceDebtLedger debt;

    // The M2 legacy-pacing shim: PredictionLimit-labeled holds with
    // create_debt=false are pure semanticHolds.
    for (int i = 0; i < 100; ++i) {
        debt.OnHold(Rollback::HoldCause::PredictionLimit, false);
    }
    TEST_CHECK(debt.Owed() == 0, "semanticHold creates no debt");
    TEST_CHECK(debt.ApplyRebasedFrames(4) == 0,
        "rebase after a non-debt hold converts nothing");

    // Lifecycle holds never create debt regardless of the flag.
    debt.OnHold(Rollback::HoldCause::LifecycleBoundary, true);
    TEST_CHECK(debt.Owed() == 0, "lifecycle holds never create debt");
    TEST_CHECK(debt.ApplyRebasedFrames(4) == 0,
        "rebase after a lifecycle hold converts nothing (typed-cause gate)");
}

void TestPredictionDebtCreateConsumeAndRebaseGate() {
    Sched::CadenceDebtLedger debt;

    // Engine-style holds (M4+): CreateOne per PredictionLimit hold, bounded.
    for (int i = 0; i < 20; ++i) {
        debt.OnHold(Rollback::HoldCause::PredictionLimit, true);
    }
    TEST_CHECK(debt.Owed() == Sched::CadenceDebtLedger::kMaxOwed,
        "debt bounded at kMaxOwed");

    debt.Consume(3);
    TEST_CHECK(debt.Owed() == Sched::CadenceDebtLedger::kMaxOwed - 3,
        "catch-up consumes owed frames");

    // Rebase lateness converts into debt ONLY behind a debt-creating
    // PredictionLimit hold (exactly one repayment owner per missed frame).
    const uint32_t converted = debt.ApplyRebasedFrames(10);
    TEST_CHECK(debt.Owed() == Sched::CadenceDebtLedger::kMaxOwed,
        "converted rebase frames are clamped to the ledger bound");
    TEST_CHECK(converted == 3, "conversion reports only what fit");

    // External suspension discards everything.
    debt.OnHold(Rollback::HoldCause::ExternalSuspension, false);
    TEST_CHECK(debt.Owed() == 0, "external suspension discards owed debt");
    TEST_CHECK(debt.ApplyRebasedFrames(5) == 0,
        "no conversion after an external discard");
}

// ── T-SCHED-4: pace slew bounds / admission / instant release ───────────────

void TestSlewAdmissionAndDeadband() {
    Sched::PaceSlew slew;
    TEST_CHECK(slew.Update(true, 1, 0) == 0, "lag 1 < deadband: no engage");
    TEST_CHECK(slew.Update(true, 0, 5) == 0, "we are the deeper side: never engage");
    TEST_CHECK(slew.Update(true, 2, 0) > 0, "lag 2 admits");
}

void TestSlewRiseRateAndCap() {
    Sched::PaceSlew slew;
    // Huge lag: target = cap. Rise must be limited to 8000 ppm per step.
    int32_t p1 = slew.Update(true, 30, 0);
    TEST_CHECK(p1 == Sched::PaceSlew::kMaxRisePpmPerStep,
        "first step limited by rise rate");
    int32_t p2 = slew.Update(true, 30, 0);
    TEST_CHECK(p2 == 2 * Sched::PaceSlew::kMaxRisePpmPerStep,
        "second step limited by rise rate");
    int32_t p = p2;
    for (int i = 0; i < 10; ++i) {
        p = slew.Update(true, 30, 0);
    }
    TEST_CHECK(p == Sched::PaceSlew::kMaxPpm, "absolute cap 25000 ppm");
}

void TestSlewHystereticRelease() {
    Sched::PaceSlew slew;
    slew.Update(true, 3, 0);  // engage
    TEST_CHECK(slew.Engaged(), "engaged at lag 3");
    // Lag 1 is inside the hysteresis band (release only below 1): stays
    // engaged, target falls freely.
    int32_t p = slew.Update(true, 1, 0);
    TEST_CHECK(slew.Engaged(), "lag 1 keeps engagement (hysteresis)");
    TEST_CHECK(p == Sched::PaceSlew::kPpmPerDepthFrame,
        "target falls freely to lag*2500");
    // Lag 0 releases.
    p = slew.Update(true, 0, 0);
    TEST_CHECK(!slew.Engaged() && p == 0, "lag 0 releases to zero");
}

void TestSlewInstantReleaseOnStale() {
    Sched::PaceSlew slew;
    slew.Update(true, 10, 0);
    slew.Update(true, 10, 0);
    TEST_CHECK(slew.Ppm() > 0, "slew active before staleness");
    TEST_CHECK(slew.Update(false, 10, 0) == 0,
        "stale sample releases instantly to zero");
    TEST_CHECK(!slew.Engaged(), "stale sample drops engagement");
}

// ── IntervalStats sanity (STAT percentile window) ───────────────────────────

void TestIntervalStatsPercentiles() {
    Sched::IntervalStats stats;
    TEST_CHECK(stats.Percentile(50) == 0, "empty window reports 0");
    for (uint32_t i = 1; i <= 100; ++i) {
        stats.Add(i * 100);  // 100..10000 µs
    }
    TEST_CHECK(stats.Percentile(0) == 100, "p0 = min");
    TEST_CHECK(stats.Percentile(100) == 10000, "p100 = max");
    const uint32_t p50 = stats.Percentile(50);
    TEST_CHECK(p50 >= 4900 && p50 <= 5200, "p50 near the median");
    const uint32_t p99 = stats.Percentile(99);
    TEST_CHECK(p99 >= 9800 && p99 <= 10000, "p99 near the top");
    stats.Reset();
    TEST_CHECK(stats.Count() == 0 && stats.Percentile(50) == 0, "reset clears");
}

}  // namespace

int main() {
    TestDeadlineExactnessProper60();
    TestDeadlineExactnessCompat58();
    TestDeadlineAdvanceOnTime();
    TestSubFrameLatenessPreservesSchedule();
    TestMultiFrameLatenessRebases();
    TestSemanticHoldCreatesNoDebt();
    TestPredictionDebtCreateConsumeAndRebaseGate();
    TestSlewAdmissionAndDeadband();
    TestSlewRiseRateAndCap();
    TestSlewHystereticRelease();
    TestSlewInstantReleaseOnStale();
    TestIntervalStatsPercentiles();

    std::printf("frame_scheduler_tests: %d checks, %d failures\n",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
