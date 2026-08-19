#include "training/action_state_classifier.h"
#include "training/frame_advantage_math.h"

#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;

#define TEST_CHECK(expr, msg) \
    do { \
        ++g_checks; \
        if (!(expr)) { \
            ++g_failures; \
            std::printf("FAIL: %s\n", msg); \
        } \
    } while (0)

static Training::ActionStateSample Sample(uint32_t actionId, uint8_t nativeFlag = 0) {
    Training::ActionStateSample s{};
    s.actionId = actionId;
    s.nativeActionableCandidate = nativeFlag;
    return s;
}

static bool Free(uint32_t actionId,
                 Training::ActionableContext context = Training::ActionableContext::AttackerRecovery,
                 bool defenderWasAirLocked = false) {
    return Training::EvaluateActionability(
        Sample(actionId),
        context,
        defenderWasAirLocked,
        Training::ActionabilitySource::LegacyActionId).actionable;
}

static void TestLegacyFreeActions() {
    const uint32_t freeActions[] = {2, 3, 4, 5, 6, 7, 8, 22, 106};
    for (uint32_t action : freeActions) {
        TEST_CHECK(Free(action), "expected legacy free action");
    }
}

static void TestLandingPolicy() {
    TEST_CHECK(!Free(23, Training::ActionableContext::DefenderRecovery, true),
        "landing must not be defender-free after air lock");
    TEST_CHECK(!Free(23, Training::ActionableContext::AttackerRecovery, false),
        "landing is not canonical attacker recovery in legacy FA mode");
    TEST_CHECK(Free(23, Training::ActionableContext::ThreatWindowEnd, false),
        "landing remains allowed for threat-window cleanup");
}

static void TestProximityGuardPolicy() {
    const uint32_t prox[] = {63, 66, 69};
    for (uint32_t action : prox) {
        TEST_CHECK(Free(action), "prox guard should be soft-free for legacy recovery");
        TEST_CHECK(!Training::IsContactStartState(action),
            "prox guard must not start contact");
    }
}

static void TestForcedLocksAndContactStates() {
    const uint32_t forced[] = {
        64, 65, 67, 68, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82
    };
    for (uint32_t action : forced) {
        TEST_CHECK(!Free(action), "forced lock must not be free");
        TEST_CHECK(Training::IsForcedDefenderLock(action),
            "forced lock classifier should include block/hit/knockdown/tech states");
    }

    const uint32_t contact[] = {64, 65, 67, 68, 70, 71, 72, 73};
    for (uint32_t action : contact) {
        TEST_CHECK(Training::IsContactStartState(action),
            "blockstun/hitstun should start contact");
    }
    const uint32_t nonContact[] = {74, 75, 76, 77, 78, 79, 80, 81, 82};
    for (uint32_t action : nonContact) {
        TEST_CHECK(!Training::IsContactStartState(action),
            "wakeup/tech/knockdown should not start new contact");
    }
}

static void TestNativeCandidateAuditOnly() {
    const Training::ActionabilityResult legacy = Training::EvaluateActionability(
        Sample(89, 1),
        Training::ActionableContext::AttackerRecovery,
        false,
        Training::ActionabilitySource::LegacyActionId);
    TEST_CHECK(!legacy.actionable, "legacy mode must not trust native candidate");
    TEST_CHECK(legacy.nativeCandidate, "native candidate should be reported for audit");

    const Training::ActionabilityResult native = Training::EvaluateActionability(
        Sample(89, 1),
        Training::ActionableContext::AttackerRecovery,
        false,
        Training::ActionabilitySource::CandidateNativeFlag);
    TEST_CHECK(native.actionable, "candidate mode should expose native flag behavior");

    const Training::ActionabilityResult forcedNative = Training::EvaluateActionability(
        Sample(72, 1),
        Training::ActionableContext::DefenderRecovery,
        false,
        Training::ActionabilitySource::CandidateNativeFlag);
    TEST_CHECK(!forcedNative.actionable, "forced locks override native candidate");
}

// The +124 readings: a projectile's pending attack keeps the attacker's own
// recovery frame, then a contact 66 frames later subtracts it.
void TestAttackerRecoveryClamp() {
    using namespace Training;

    TEST_CHECK(EffectiveAttackerRecovery(8149, 8215) == 8215,
               "recovery predating contact clamps to contact");
    TEST_CHECK(ComputeFrameAdvantage(8149, 8273, 8215) == 58,
               "clamped advantage measures from contact, not from flight start");

    TEST_CHECK(EffectiveAttackerRecovery(120, 100) == 120,
               "an ordinary recovery after contact is left alone");
    TEST_CHECK(ComputeFrameAdvantage(120, 125, 100) == 5,
               "ordinary blockstring advantage is unchanged");
    TEST_CHECK(ComputeFrameAdvantage(130, 125, 100) == -5,
               "negative advantage is unchanged");

    TEST_CHECK(EffectiveAttackerRecovery(100, 100) == 100,
               "recovery on the contact frame is not clamped away");
    TEST_CHECK(EffectiveAttackerRecovery(kFrameAdvantageUnset, 100) == kFrameAdvantageUnset,
               "unset recovery stays unset");
    TEST_CHECK(EffectiveAttackerRecovery(80, kFrameAdvantageUnset) == 80,
               "unset contact cannot clamp");
}

void TestGapBeforeContact() {
    using namespace Training;

    TEST_CHECK(GapBeforeContact(100, 103, 60) == 3, "three actionable frames is a 3f gap");
    TEST_CHECK(GapBeforeContact(100, 101, 60) == 1, "a one-frame gap is reportable");
    TEST_CHECK(GapBeforeContact(100, 100, 60) == 0, "contact on the free frame is a true blockstring");
    TEST_CHECK(GapBeforeContact(100, 160, 60) == 60, "a gap exactly at the limit still counts");
    TEST_CHECK(GapBeforeContact(100, 161, 60) == 0, "past the limit is a new engagement, not a gap");
    TEST_CHECK(GapBeforeContact(kFrameAdvantageUnset, 120, 60) == 0, "no recorded free frame means no gap");
    TEST_CHECK(GapBeforeContact(120, 100, 60) == 0, "contact before the free frame is not a gap");
}

// The ordering rule. A gap must never appear after, beside, or appended to an
// advantage number: if one is on screen it is dropped outright, not queued.
void TestGapOrderingRule() {
    using namespace Training;

    TEST_CHECK(GapShouldPublish(3, false), "a real gap takes a free slot");
    TEST_CHECK(!GapShouldPublish(3, true), "a gap is dropped while a number is on screen");
    TEST_CHECK(!GapShouldPublish(0, false), "a zero gap is a blockstring and is never published");
    TEST_CHECK(!GapShouldPublish(0, true), "a zero gap stays unpublished either way");

    // Suppression, not deferral: the same gap does not become publishable once
    // the number expires, because it was discarded at the contact.
    TEST_CHECK(GapShouldPublish(3, false) && !GapShouldPublish(3, true),
               "publishability is decided at the contact, from the slot state then");
}

int main() {
    TestLegacyFreeActions();
    TestLandingPolicy();
    TestProximityGuardPolicy();
    TestForcedLocksAndContactStates();
    TestNativeCandidateAuditOnly();
    TestAttackerRecoveryClamp();
    TestGapBeforeContact();
    TestGapOrderingRule();

    if (g_failures) {
        std::printf("frame_advantage_classifier_tests: %d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }

    std::printf("frame_advantage_classifier_tests: passed (%d checks)\n", g_checks);
    return 0;
}
