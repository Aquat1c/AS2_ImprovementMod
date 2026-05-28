#include "training/action_state_classifier.h"

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

int main() {
    TestLegacyFreeActions();
    TestLandingPolicy();
    TestProximityGuardPolicy();
    TestForcedLocksAndContactStates();
    TestNativeCandidateAuditOnly();

    if (g_failures) {
        std::printf("frame_advantage_classifier_tests: %d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }

    std::printf("frame_advantage_classifier_tests: passed (%d checks)\n", g_checks);
    return 0;
}
