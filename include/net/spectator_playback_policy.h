/**
 * Alice Senki 2 - Spectator playback pacing policy (re0.7 M7, plan §2.8.8)
 *
 * Pure, header-only elastic buffering policy for the spectator playback
 * driver — ported from qoh99_netplay `game/SpectatorPlaybackPolicy.h` with
 * the plan's watermarks. No Win32, no game memory: the driver feeds it
 * buffered-record counts and retains the small latches it returns.
 *
 * Model (S-1/S-2):
 *   - A live viewer deliberately runs ~4 s behind the confirmed edge
 *     (240 records). That cushion absorbs jitter and repair attempts
 *     without turning network variance into visible freezes.
 *   - Under low water the cadence stretches (950/850/700 permille at
 *     180/120/60 records) with Bresenham-distributed holds — smooth slight
 *     slow motion instead of drain-to-zero → freeze → 2-3x sprint.
 *   - A true underrun (0 records) freezes; playback resumes at 120
 *     records (rebuffer), not at the full 240 prime.
 *   - Material excess above the intentional delay (>300 records) enters
 *     hidden catch-up, budgeted by backlog rung (2/4/8/16/24 ticks per
 *     presentation slot) with hysteresis back to the 240 target and a
 *     12 ms wall slice so a slow machine degrades convergence speed, not
 *     visible cadence. Catch-up ticks never own the cadence slot.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Net::Spectator {

// Steady live target ≈4 s of confirmed records; the startup prime uses the
// same value so a normal prime starts LIVE, not in an automatic fast-forward.
constexpr size_t kPlaybackLiveTargetRecords = 240u;
constexpr size_t kPlaybackStartupBufferRecords = 240u;

// A true post-start underrun is already visible; resume at two seconds
// rather than staring at a frozen frame for the full four-second prime.
constexpr size_t kPlaybackRebufferRecords = 120u;

// Automatic catch-up engages only on a material excess above the intended
// delay; ordinary jitter is retained as safety margin.
constexpr size_t kPlaybackCatchupEnterRecords = 300u;
static_assert(kPlaybackRebufferRecords < kPlaybackLiveTargetRecords,
              "rebuffer recovery must resume below the steady target");
static_assert(kPlaybackLiveTargetRecords < kPlaybackCatchupEnterRecords,
              "catch-up hysteresis needs a non-empty band");

// Elastic slow-motion watermarks (records) and cadences (permille).
constexpr size_t kPlaybackElasticNearLowRecords = 180u;
constexpr size_t kPlaybackElasticLowRecords = 120u;
constexpr size_t kPlaybackElasticCriticalRecords = 60u;
constexpr uint32_t kPlaybackElasticNearLowPermille = 950u;
constexpr uint32_t kPlaybackElasticLowPermille = 850u;
constexpr uint32_t kPlaybackElasticCriticalPermille = 700u;
static_assert(kPlaybackElasticCriticalRecords < kPlaybackElasticLowRecords &&
              kPlaybackElasticLowRecords < kPlaybackElasticNearLowRecords &&
              kPlaybackElasticNearLowRecords < kPlaybackLiveTargetRecords,
              "elastic watermarks must rise toward the live target");
static_assert(kPlaybackElasticCriticalPermille < kPlaybackElasticLowPermille &&
              kPlaybackElasticLowPermille < kPlaybackElasticNearLowPermille &&
              kPlaybackElasticNearLowPermille < 1000u,
              "elastic cadence must recover monotonically toward 1x");

// Wall-clock ceiling for one presentation slot's hidden catch-up work. The
// tick ladder is an upper bound; the caller stops early when this elapses.
constexpr uint64_t kPlaybackCatchupSliceMicros = 12000u;

// Startup/rebuffer priming. A SEALED stream (match ended / transport gone,
// no more records will ever arrive) must never wait for a threshold it can
// no longer reach — terminal receipt alone unblocks a sub-threshold tail.
constexpr bool PlaybackPrimed(size_t bufferedRecords,
                              bool streamSealed,
                              bool rebuffering) {
    if (bufferedRecords == 0u) return false;
    if (streamSealed) return true;
    return bufferedRecords >= (rebuffering
        ? kPlaybackRebufferRecords
        : kPlaybackStartupBufferRecords);
}

constexpr uint32_t PlaybackElasticSpeedPermille(size_t bufferedRecords) {
    if (bufferedRecords >= kPlaybackElasticNearLowRecords) return 1000u;
    if (bufferedRecords >= kPlaybackElasticLowRecords)
        return kPlaybackElasticNearLowPermille;
    if (bufferedRecords >= kPlaybackElasticCriticalRecords)
        return kPlaybackElasticLowPermille;
    return bufferedRecords != 0u ? kPlaybackElasticCriticalPermille : 0u;
}

struct PlaybackElasticPlan {
    bool advance = true;
    uint32_t nextCredit = 0u;
    uint32_t speedPermille = 1000u;
};

// Bresenham-style fractional cadence: 850 permille distributes 85 record
// advances over 100 presentation slots instead of clustering fifteen holds.
// A held slot repeats the last presented frame; it never invents or skips a
// canonical record.
constexpr PlaybackElasticPlan PlaybackElasticPlan_Next(size_t bufferedRecords,
                                                       uint32_t priorCredit) {
    PlaybackElasticPlan plan{};
    plan.speedPermille = PlaybackElasticSpeedPermille(bufferedRecords);
    if (plan.speedPermille >= 1000u) return plan;
    if (plan.speedPermille == 0u) {
        plan.advance = false;
        return plan;
    }
    const uint32_t credit = (priorCredit % 1000u) + plan.speedPermille;
    plan.advance = credit >= 1000u;
    plan.nextCredit = plan.advance ? credit - 1000u : credit;
    return plan;
}

// One presentation slot may own several hidden archived ticks. The caller
// must still yield at any dispatcher return or phase/scene boundary, and
// must stop early when the wall slice elapses.
//
// `wasCatchingUp` is the caller's retained hysteresis latch: engage above
// the high-water mark, disengage only at/below the live target. The budget
// is strictly greater than one whenever the backlog exceeds the target —
// a live source produces ~one record per slot, so a budget of one has no
// closing rate.
struct PlaybackCatchupPlan {
    size_t tickBudget = 0u;
    bool catchingUp = false;
};

constexpr PlaybackCatchupPlan PlaybackCatchupPlan_Next(size_t bufferedRecords,
                                                       bool wasCatchingUp) {
    PlaybackCatchupPlan plan{};
    if (bufferedRecords == 0u) {
        return plan;
    }
    plan.catchingUp = wasCatchingUp
        ? (bufferedRecords > kPlaybackLiveTargetRecords)
        : (bufferedRecords > kPlaybackCatchupEnterRecords);
    if (!plan.catchingUp) {
        plan.tickBudget = 1u;
        return plan;
    }
    // Deep-backlog rungs (S-1 late join): scale the ladder with how far
    // behind the viewer actually is so the wait grows far more slowly than
    // the backlog does.
    if (bufferedRecords > 1920u) plan.tickBudget = 24u;
    else if (bufferedRecords > 960u) plan.tickBudget = 16u;
    else if (bufferedRecords > 480u) plan.tickBudget = 8u;
    else if (bufferedRecords > 360u) plan.tickBudget = 4u;
    else plan.tickBudget = 2u;
    return plan;
}

// ── S-4 record hash verification ────────────────────────────────────────────
// Records carry a 24-bit truncation of the host's confirmed pre-state digest.
// The client verifies its own live digest against record N's hash before
// ticking N (which also checks the post-state of N−1). Because the spectator
// reaches gameplay through the native charsel/loader path — not the players'
// baseline rendezvous — verification arms only after the first agreeing
// record ("sync acquire"); a stream that NEVER agrees within the window
// degrades to unverified playback with a loud log (observational viewer,
// fail-degrade) instead of killing every session on a systematic entry
// mismatch. After acquisition a mismatch is a genuine divergence and the
// client fails closed (leaves the session; the player link is unaffected).
constexpr uint32_t kPlaybackHashAcquireWindowRecords = 600u;   // ~10 s

enum class PlaybackHashVerdict : uint8_t {
    NotApplicable = 0,   // record has no hash / verification disabled
    Acquired,            // first agreement — strict mode arms
    Ok,                  // agreement under strict mode
    EntryMismatch,       // pre-acquire disagreement (retry window open)
    GiveUp,              // acquire window exhausted — disable, fail-degrade
    Diverged,            // post-acquire disagreement — fail closed (S-4)
};

constexpr PlaybackHashVerdict PlaybackVerifyRecordHash(bool recordHasHash,
                                                       bool verifyDisabled,
                                                       bool acquired,
                                                       uint32_t entryMismatches,
                                                       uint32_t recordHash24,
                                                       uint32_t localHash24) {
    if (!recordHasHash || verifyDisabled) return PlaybackHashVerdict::NotApplicable;
    if ((recordHash24 & 0xFFFFFFu) == (localHash24 & 0xFFFFFFu)) {
        return acquired ? PlaybackHashVerdict::Ok : PlaybackHashVerdict::Acquired;
    }
    if (acquired) return PlaybackHashVerdict::Diverged;
    return entryMismatches + 1u >= kPlaybackHashAcquireWindowRecords
        ? PlaybackHashVerdict::GiveUp
        : PlaybackHashVerdict::EntryMismatch;
}

constexpr uint32_t PlaybackHash24FromDigest(uint64_t digest) {
    // Fold the full 64-bit digest so all bytes contribute to the 24 bits.
    const uint64_t folded = digest ^ (digest >> 24) ^ (digest >> 48);
    return (uint32_t)(folded & 0xFFFFFFu);
}

} // namespace Net::Spectator
