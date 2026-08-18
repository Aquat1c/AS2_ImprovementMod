#include "training/native_recovery.h"
#include "training/action_state_classifier.h"

#include <cstdio>

using namespace Training;

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

// A fighter mid-neutral on the ground: every normal route open, no handoff.
static NativeRecoverySample GroundNeutral() {
    NativeRecoverySample s{};
    s.currentAction = kTargetStandNeutral;
    s.airborne = 0;
    for (int r = 2; r <= 14; ++r) s.routes[r] = 1;
    return s;
}

static NativeRecoverySample AirNeutral() {
    NativeRecoverySample s{};
    s.currentAction = kTargetAirNeutral;
    s.airborne = 1;
    for (int r = 2; r <= 14; ++r) s.routes[r] = 1;
    return s;
}

// Committed to an attack: the action script has zeroed its normal routes.
static NativeRecoverySample Committed(uint32_t actionId, uint8_t airborne = 0) {
    NativeRecoverySample s{};
    s.currentAction = actionId;
    s.airborne = airborne;
    return s;
}

// --- Terminal handoff + open route = recovery ----------------------------

static void TestGroundFreeRecovery() {
    NativeRecoverySample s = Committed(90);          // still shows the old attack
    s.pendingAction1 = kTargetStandNeutral;          // script queued stand neutral
    s.routes[kRouteStandOrAirA] = 1;                 // stand A would be considered

    const NativeRecoveryResult r = EvaluateNativeRecovery(s);
    TEST_CHECK(r.freeRecovery, "handoff + open route = free");
    TEST_CHECK(r.kind == NativeRecoveryKind::FreeGround, "ground kind");
    TEST_CHECK(r.pendingTarget == kTargetStandNeutral, "pending target reported");

    // The current action still being the attack is exactly the case that made
    // an action-ID edge a tick late; it must not block the result.
    TEST_CHECK(s.currentAction == 90, "current action is still the old attack");

    NativeRecoverySample crouch = s;
    crouch.pendingAction1 = kTargetCrouchNeutral;
    crouch.routes[kRouteStandOrAirA] = 0;
    crouch.routes[kRouteCrouchA] = 1;
    TEST_CHECK(EvaluateNativeRecovery(crouch).freeRecovery,
               "crouch handoff with crouch route open = free");
}

static void TestAirFreeRecovery() {
    NativeRecoverySample s = Committed(93, 1);
    s.pendingAction2 = kTargetAirNeutral;            // slot 2 carries it
    s.routes[kRouteStandOrAirA] = 1;

    const NativeRecoveryResult r = EvaluateNativeRecovery(s);
    TEST_CHECK(r.freeRecovery, "air handoff + air A route = free");
    TEST_CHECK(r.kind == NativeRecoveryKind::FreeAir, "air kind");

    // Route 5 is the crouch branch and is meaningless while airborne.
    NativeRecoverySample crouchOnly = Committed(93, 1);
    crouchOnly.pendingAction2 = kTargetAirNeutral;
    crouchOnly.routes[kRouteCrouchA] = 1;
    TEST_CHECK(!EvaluateNativeRecovery(crouchOnly).freeRecovery,
               "airborne cannot recover through the crouch route");
}

// --- Cancel windows and buffered input are not recovery ------------------

static void TestCancelWindowIsNotRecovery() {
    // A cancellable attack: routes reopen while the move is still committed and
    // no terminal neutral handoff exists.
    NativeRecoverySample s = Committed(90);
    s.routes[kRouteStandOrAirA] = 1;
    s.routes[kRouteCrouchA] = 1;

    const NativeRecoveryResult r = EvaluateNativeRecovery(s);
    TEST_CHECK(!r.freeRecovery, "cancel window is not recovery");
    TEST_CHECK(r.routeOpenWithoutFreeHandoff, "cancel window is flagged as telemetry");
    TEST_CHECK(r.kind == NativeRecoveryKind::Locked, "cancel window stays locked");
}

static void TestBufferedInputIsNotRecovery() {
    // Route retention can leave one route nonzero during a locked state.
    NativeRecoverySample s = Committed(72);          // hitstun
    s.routes[kRouteStandOrAirA] = 3;                 // retained buffer

    const NativeRecoveryResult r = EvaluateNativeRecovery(s);
    TEST_CHECK(!r.freeRecovery, "buffered route during hitstun is not recovery");
    TEST_CHECK(r.routeOpenWithoutFreeHandoff, "buffered route is flagged");
}

static void TestHandoffWithoutRouteIsNotRecovery() {
    NativeRecoverySample s = Committed(90);
    s.pendingAction1 = kTargetStandNeutral;
    // No route granted yet.
    TEST_CHECK(!EvaluateNativeRecovery(s).freeRecovery,
               "pending handoff without an open route is not recovery");
}

// --- Landing stays unverified -------------------------------------------

static void TestLandingUnverified() {
    NativeRecoverySample s = Committed(23);
    s.pendingAction1 = kTargetLanding;
    for (int r = 2; r <= 14; ++r) s.routes[r] = 1;

    const NativeRecoveryResult r = EvaluateNativeRecovery(s);
    TEST_CHECK(!r.freeRecovery, "landing is not declared free");
    TEST_CHECK(r.kind == NativeRecoveryKind::LandingUnverified, "landing kind");
}

// --- CPU-controlled sides are refused, not guessed -----------------------

static void TestCpuControlled() {
    NativeRecoverySample s = GroundNeutral();
    s.pendingAction1 = kTargetStandNeutral;
    s.cpuControlled = 1;

    const NativeRecoveryResult r = EvaluateNativeRecovery(s);
    TEST_CHECK(!r.freeRecovery, "CPU side reports no recovery");
    TEST_CHECK(r.kind == NativeRecoveryKind::CpuControlledUnsupported, "cpu kind");
    TEST_CHECK(!IsStableFreeFallback(s), "CPU side has no stable fallback either");
}

// --- The training restore gate is not actionability ----------------------

static void TestRestoreGateIsNotActionability() {
    // Action 0x81 sets +207 at startup while zeroing every route for the body
    // of the move. The gate must not imply recovery.
    NativeRecoverySample s = Committed(0x81);
    s.trainingRestoreGate = 1;

    const NativeRecoveryResult r = EvaluateNativeRecovery(s);
    TEST_CHECK(!r.freeRecovery, "restore gate set but no routes = locked");
    TEST_CHECK(!r.routeOpenWithoutFreeHandoff, "no route open at all");
}

// --- Pending slot selection ---------------------------------------------

static void TestPendingSlots() {
    NativeRecoverySample s{};
    TEST_CHECK(PendingTarget(s) == 0, "no pending target");

    s.pendingAction2 = kTargetAirNeutral;
    TEST_CHECK(PendingTarget(s) == kTargetAirNeutral, "slot 2 used when slot 1 empty");

    s.pendingAction1 = kTargetStandNeutral;
    TEST_CHECK(PendingTarget(s) == kTargetStandNeutral, "slot 1 wins");
    TEST_CHECK(BothPendingSlotsSet(s), "both-slots case is detectable");
}

// --- Route-open helper used by auto-block eligibility --------------------

static void TestNeutralRouteOpen() {
    TEST_CHECK(NeutralRouteOpen(GroundNeutral()), "ground neutral has routes open");
    TEST_CHECK(NeutralRouteOpen(AirNeutral()), "air neutral has routes open");
    TEST_CHECK(!NeutralRouteOpen(Committed(90)), "committed attack has no route open");

    // Grounded: either the standing or the crouching branch counts.
    NativeRecoverySample crouchOnly = Committed(2);
    crouchOnly.routes[kRouteCrouchA] = 1;
    TEST_CHECK(NeutralRouteOpen(crouchOnly), "grounded crouch route counts");

    // Airborne only ever uses the 2/3/4 branch.
    NativeRecoverySample airCrouch = Committed(22, 1);
    airCrouch.routes[kRouteCrouchA] = 1;
    TEST_CHECK(!NeutralRouteOpen(airCrouch), "airborne ignores the crouch route");
}

static void TestStableFallback() {
    TEST_CHECK(IsStableFreeFallback(GroundNeutral()), "ground neutral is a stable fallback");
    TEST_CHECK(IsStableFreeFallback(AirNeutral()), "air neutral is a stable fallback");
    TEST_CHECK(!IsStableFreeFallback(Committed(90)), "committed attack is not stable free");

    // A neutral action ID with the routes still closed is not free.
    NativeRecoverySample halfway = Committed(kTargetStandNeutral);
    halfway.routes[kRouteStandOrAirA] = 1;
    TEST_CHECK(!IsStableFreeFallback(halfway), "partial routes are not a stable fallback");
}

// --- The action-ID range rule is no longer used for commitment -----------

static void TestSharedAttackActionRange() {
    TEST_CHECK(IsSharedAttackAction(85), "stand A is a shared attack action");
    TEST_CHECK(IsSharedAttackAction(92), "air A is a shared attack action");
    TEST_CHECK(IsSharedAttackAction(98), "command normal is a shared attack action");
    TEST_CHECK(IsSharedAttackAction(137), "super is a shared attack action");
    TEST_CHECK(!IsSharedAttackAction(2), "standing neutral is not");
    TEST_CHECK(!IsSharedAttackAction(72), "hitstun is not");

    // The old `>= 85` rule called these attacks; they are not shared actions.
    TEST_CHECK(!IsSharedAttackAction(105), "healing is not a shared attack action");
    TEST_CHECK(!IsSharedAttackAction(120), "action 120 is not a shared attack action");

    // And these ARE attacks the old rule missed entirely: CharAction_0x2A..0x2E
    // are attack handlers at 42..46, below the old boundary. The ID test cannot
    // see them, which is why commitment now comes from the routes.
    for (uint32_t id = 42; id <= 46; ++id) {
        NativeRecoverySample s = Committed(id);
        TEST_CHECK(!NeutralRouteOpen(s),
                   "sub-85 character special is caught by the route vector");
    }
}

int main() {
    TestGroundFreeRecovery();
    TestAirFreeRecovery();
    TestCancelWindowIsNotRecovery();
    TestBufferedInputIsNotRecovery();
    TestHandoffWithoutRouteIsNotRecovery();
    TestLandingUnverified();
    TestCpuControlled();
    TestRestoreGateIsNotActionability();
    TestPendingSlots();
    TestNeutralRouteOpen();
    TestStableFallback();
    TestSharedAttackActionRange();

    std::printf("native_recovery_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
