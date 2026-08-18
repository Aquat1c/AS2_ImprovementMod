#include "training/auto_block.h"

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

// --- 22.1 Ground mask decoder -------------------------------------------

static void TestGroundMaskDecoder() {
    TEST_CHECK(DecodeGroundGuardClass(0) == GroundGuardClass::None, "mask 0 -> None");
    TEST_CHECK(DecodeGroundGuardClass(1) == GroundGuardClass::StandOnly, "mask 1 -> StandOnly");
    TEST_CHECK(DecodeGroundGuardClass(2) == GroundGuardClass::CrouchOnly, "mask 2 -> CrouchOnly");
    TEST_CHECK(DecodeGroundGuardClass(3) == GroundGuardClass::Either, "mask 3 -> Either");
    TEST_CHECK(DecodeGroundGuardClass(5) == GroundGuardClass::StandOnly, "mask 5 -> StandOnly + air");
    TEST_CHECK(DecodeGroundGuardClass(6) == GroundGuardClass::CrouchOnly, "mask 6 -> CrouchOnly + air");
    TEST_CHECK(DecodeGroundGuardClass(7) == GroundGuardClass::Either, "mask 7 -> Either + air");
    TEST_CHECK(DecodeGroundGuardClass(0x2001) == GroundGuardClass::SpecialGuardRequired,
               "mask 0x2001 -> SpecialGuardRequired");

    // Bit 0 is the STAND lane, so a stand-only attack is the overhead. The old
    // ATTACK_FLAG_LOW_HIT reading had this exactly backwards.
    TEST_CHECK(DecodeGroundGuardClass(kGuardLaneStand) != GroundGuardClass::CrouchOnly,
               "bit 0 must not classify as low");
}

// --- 22.2 Ground-lane intersection --------------------------------------

static GuardLane PlanFor(const uint32_t* masks, int count, bool airborne = false,
                         bool preferCrouch = true) {
    FrameGuardPlan plan{};
    ResetPlan(plan, 100, airborne);
    for (int i = 0; i < count; ++i) {
        PlanAccumulateContact(plan, masks[i]);
    }
    PlanFinalize(plan, preferCrouch);
    return plan.lane;
}

static void TestLaneIntersection() {
    const uint32_t standStand[] = {1, 1};
    const uint32_t crouchCrouch[] = {2, 2};
    const uint32_t eitherStand[] = {3, 1};
    const uint32_t eitherCrouch[] = {3, 2};
    const uint32_t standCrouch[] = {1, 2};
    const uint32_t noneAndStand[] = {0, 1};

    TEST_CHECK(PlanFor(standStand, 2) == GuardLane::Stand, "stand + stand -> stand");
    TEST_CHECK(PlanFor(crouchCrouch, 2) == GuardLane::Crouch, "crouch + crouch -> crouch");
    TEST_CHECK(PlanFor(eitherStand, 2) == GuardLane::Stand, "either + stand -> stand");
    TEST_CHECK(PlanFor(eitherCrouch, 2) == GuardLane::Crouch, "either + crouch -> crouch");
    TEST_CHECK(PlanFor(standCrouch, 2) == GuardLane::Conflict, "stand + crouch -> conflict");
    TEST_CHECK(PlanFor(noneAndStand, 2) == GuardLane::None, "no-lane attack -> no ordinary guard");

    const uint32_t either[] = {3};
    TEST_CHECK(PlanFor(either, 1, false, true) == GuardLane::Crouch, "either prefers crouch");
    TEST_CHECK(PlanFor(either, 1, false, false) == GuardLane::Stand, "either honours stand preference");

    // A special-guard attack poisons the whole frame regardless of ordering.
    const uint32_t specialThenStand[] = {0x2003, 1};
    const uint32_t standThenSpecial[] = {1, 0x2003};
    TEST_CHECK(PlanFor(specialThenStand, 2) == GuardLane::None, "special guard first -> none");
    TEST_CHECK(PlanFor(standThenSpecial, 2) == GuardLane::None, "special guard second -> none");
}

static void TestAirPlan() {
    const uint32_t airMask[] = {0x7};
    TEST_CHECK(PlanFor(airMask, 1, true) == GuardLane::Air, "airborne + air-blockable -> air");

    const uint32_t groundOnly[] = {0x3};
    TEST_CHECK(PlanFor(groundOnly, 1, true) == GuardLane::None,
               "airborne vs non-air-blockable -> no ordinary guard");

    // The native predicate: both ground lanes on the defender AND 0x4 from
    // either side. Auto-block supplies 0x3 only, never 0x4.
    TEST_CHECK(NativeAirGuardPredicate(0x4, 0x3), "attack air bit satisfies predicate");
    TEST_CHECK(NativeAirGuardPredicate(0x0, 0x7), "defender air capability satisfies predicate");
    TEST_CHECK(!NativeAirGuardPredicate(0x0, 0x3), "no air bit anywhere -> refused");
    TEST_CHECK(!NativeAirGuardPredicate(0x4, 0x1), "defender missing a ground lane -> refused");
}

// --- 22.3 Per-frame latch ------------------------------------------------

static void TestPerFrameLatch() {
    FrameGuardPlan plan{};
    ResetPlan(plan, 500, false);
    PlanLatchFromContact(plan, kGuardLaneStand, 0, true);
    TEST_CHECK(plan.lane == GuardLane::Stand, "first contact latches stand");

    TEST_CHECK(PlanAcceptsAttack(plan, 0x3, 0), "later stand-compatible contact still accepted");
    TEST_CHECK(!PlanAcceptsAttack(plan, kGuardLaneCrouch, 0),
               "later crouch-only contact cannot switch the frame");

    // A second latch attempt must not move the lane.
    PlanLatchFromContact(plan, kGuardLaneCrouch, 0, true);
    TEST_CHECK(plan.lane == GuardLane::Stand, "lane is latched for the whole frame");

    // Next simulation frame is free to choose the other lane.
    ResetPlan(plan, 501, false);
    PlanLatchFromContact(plan, kGuardLaneCrouch, 0, true);
    TEST_CHECK(plan.lane == GuardLane::Crouch, "next frame may latch crouch");
}

static void TestFallbackRefusals() {
    FrameGuardPlan plan{};
    ResetPlan(plan, 10, false);
    TEST_CHECK(!PlanLatchFromContact(plan, 0x2000 | 0x3, 0, true),
               "special-guard attack refused without defender capability");
    TEST_CHECK(plan.lane == GuardLane::None, "special-guard attack yields no lane");

    ResetPlan(plan, 11, false);
    TEST_CHECK(PlanLatchFromContact(plan, 0x2000 | 0x3, kDefenderSpecialGuard, true),
               "special-guard attack accepted when the defender naturally has it");

    ResetPlan(plan, 12, false);
    TEST_CHECK(!PlanLatchFromContact(plan, 0, 0, true), "no-lane attack refused");
    TEST_CHECK(!PlanAcceptsAttack(plan, 0x3, 0), "a refused frame accepts nothing");
}

// --- 22.4 Policy ---------------------------------------------------------

static void TestPolicy() {
    AutoBlockSequenceState seq{};
    SequenceBegin(seq, 1, 0, 50);

    TEST_CHECK(PolicyWantsBlock(BlockPolicy::Off, seq, 0) == false, "Off never blocks");
    TEST_CHECK(PolicyWantsBlock(BlockPolicy::All, seq, 0), "All blocks ordinal 0");
    TEST_CHECK(PolicyWantsBlock(BlockPolicy::All, seq, 7), "All blocks later ordinals");
    TEST_CHECK(PolicyWantsBlock(BlockPolicy::Adaptive, seq, 3), "Adaptive always wants block");

    TEST_CHECK(PolicyWantsBlock(BlockPolicy::FirstHit, seq, 0), "First Hit blocks ordinal 0");
    TEST_CHECK(!PolicyWantsBlock(BlockPolicy::FirstHit, seq, 1), "First Hit skips ordinal 1");

    TEST_CHECK(!PolicyWantsBlock(BlockPolicy::AfterFirstHit, seq, 0), "After First skips ordinal 0");
    TEST_CHECK(PolicyWantsBlock(BlockPolicy::AfterFirstHit, seq, 1), "After First blocks ordinal 1");
    TEST_CHECK(PolicyWantsBlock(BlockPolicy::AfterFirstHit, seq, 5), "After First blocks ordinal 5");
}

static void TestContactGroups() {
    AutoBlockSequenceState seq{};
    SequenceBegin(seq, 100, 0, 0);
    TEST_CHECK(seq.resolvedContactGroups == 0, "sequence starts at ordinal 0");

    // Several objects resolving on one frame are one temporal hit.
    TEST_CHECK(SequenceNoteContactGroup(seq, 100), "first contact on frame 100 counts");
    TEST_CHECK(!SequenceNoteContactGroup(seq, 100), "second object on frame 100 does not");
    TEST_CHECK(seq.resolvedContactGroups == 1, "one group after simultaneous contacts");

    TEST_CHECK(SequenceNoteContactGroup(seq, 101), "next frame counts a new group");
    TEST_CHECK(seq.resolvedContactGroups == 2, "two groups over two frames");
}

static void TestSequenceLifetime() {
    AutoBlockSequenceState seq{};
    SequenceBegin(seq, 200, 0, 0);

    // A one-frame inactive gap inside a multi-hit action must not end it.
    TEST_CHECK(!SequenceTickQuiet(seq, false, false, kSequenceQuietGrace), "one quiet frame keeps it alive");
    TEST_CHECK(SequenceTickQuiet(seq, false, false, kSequenceQuietGrace), "grace expiry ends it");

    SequenceBegin(seq, 300, 0, 0);
    TEST_CHECK(!SequenceTickQuiet(seq, true, false, kSequenceQuietGrace), "armed threat keeps it alive");
    TEST_CHECK(!SequenceTickQuiet(seq, false, true, kSequenceQuietGrace), "forced defender state keeps it alive");
    TEST_CHECK(seq.quietFrames == 0, "quiet counter resets while pressure continues");
}

static void TestDeterministicRandom() {
    ThreatKey key{};
    key.source = ThreatSource::HitDef;
    key.owner = 0;
    key.objectId = 142;

    const uint32_t a = DeterministicSequenceRoll(1234, 7, 3, 11, key);
    const uint32_t b = DeterministicSequenceRoll(1234, 7, 3, 11, key);
    TEST_CHECK(a == b, "roll is stable across repeated evaluation");

    const uint32_t c = DeterministicSequenceRoll(1235, 7, 3, 11, key);
    TEST_CHECK(a != c, "roll varies with the sequence start frame");

    // Slot must not participate: HitDef compaction moves entries.
    ThreatKey moved = key;
    moved.hitDefSlot = 42;
    TEST_CHECK(DeterministicSequenceRoll(1234, 7, 3, 11, moved) == a,
               "roll ignores the HitDef slot");

    AutoBlockSequenceState never{};
    SequenceBegin(never, 1234, a, 0);
    TEST_CHECK(!never.randomBlock, "0% never blocks");
    AutoBlockSequenceState always{};
    SequenceBegin(always, 1234, a, 100);
    TEST_CHECK(always.randomBlock, "100% always blocks");
}

// --- 22.5 Scoped low-bit mutation ---------------------------------------

static void TestScopedLaneBits() {
    const uint32_t high = 0x0DEA0000u | kDefenderGuardPoint;

    const uint32_t stand = ApplyGuardLaneBits(high | kGuardLaneCrouch, GuardLane::Stand);
    TEST_CHECK((stand & kGroundGuardMask) == kGuardLaneStand, "stand lane replaces the ground bits");
    TEST_CHECK((stand & ~kGroundGuardMask) == high, "high bits are untouched");
    TEST_CHECK((stand & kDefenderAirGuard) == 0, "air bit 0x4 is never fabricated");

    const uint32_t crouch = ApplyGuardLaneBits(high, GuardLane::Crouch);
    TEST_CHECK((crouch & kGroundGuardMask) == kGuardLaneCrouch, "crouch lane set");

    const uint32_t air = ApplyGuardLaneBits(high, GuardLane::Air);
    TEST_CHECK((air & kGroundGuardMask) == kGroundGuardMask, "air lane supplies both ground bits");
    TEST_CHECK((air & kDefenderAirGuard) == 0, "air lane still does not fabricate 0x4");

    TEST_CHECK(ApplyGuardLaneBits(high, GuardLane::Conflict) == high, "conflict changes nothing");
    TEST_CHECK(ApplyGuardLaneBits(high, GuardLane::None) == high, "no-lane changes nothing");

    // Restore must keep whatever native code changed above bit 1.
    const uint32_t oldFlags = high | kGuardLaneCrouch;
    const uint32_t nativeChanged = (stand | 0x00040000u) & ~kDefenderGuardPoint;
    const uint32_t restored = RestoreGuardLaneBits(nativeChanged, oldFlags);
    TEST_CHECK((restored & kGroundGuardMask) == kGuardLaneCrouch, "low bits restored");
    TEST_CHECK((restored & ~kGroundGuardMask) == (nativeChanged & ~kGroundGuardMask),
               "native high-bit changes survive the restore");
}

static void TestNativeGuardDetection() {
    // Already guarding: no augmentation needed at all.
    TEST_CHECK(DefenderAlreadyGuards(kGuardLaneCrouch, kGuardLaneCrouch, false),
               "crouch attack vs crouching defender");
    TEST_CHECK(!DefenderAlreadyGuards(kGuardLaneCrouch, kGuardLaneStand, false),
               "low vs standing defender is not already guarded");
    TEST_CHECK(!DefenderAlreadyGuards(0x2000 | 0x3, 0x3, false),
               "special-guard attack is not already guarded without the capability");
    TEST_CHECK(DefenderAlreadyGuards(0x7, 0x7, true), "airborne with native air guard");
    TEST_CHECK(!DefenderAlreadyGuards(0x3, 0x3, true), "airborne vs non-air-blockable");
}

// --- 22.6 HitDef identity ------------------------------------------------

static void TestThreatIdentity() {
    ThreatKey a{};
    a.source = ThreatSource::HitDef;
    a.owner = 0;
    a.objectId = 142;
    a.hitDefSlot = 7;
    a.payloadAddress = 0x1000;

    ThreatKey compacted = a;
    compacted.hitDefSlot = 3;
    compacted.payloadAddress = 0x0800;
    TEST_CHECK(SameThreatIdentity(a, compacted), "compaction does not create a new threat");

    ThreatKey reused = a;
    reused.objectId = 143;
    TEST_CHECK(!SameThreatIdentity(a, reused), "a new id is a new threat");

    ThreatKey otherOwner = a;
    otherOwner.owner = 1;
    TEST_CHECK(!SameThreatIdentity(a, otherOwner), "owner participates in identity");

    ThreatKey direct = a;
    direct.source = ThreatSource::DirectPlayer;
    TEST_CHECK(!SameThreatIdentity(a, direct), "source participates in identity");
}

// --- Eligibility ---------------------------------------------------------

// Free states are the ones where the engine would accept an ordinary input, so
// the route vector is part of the state, not the action ID.
static DefenderGuardState Defender(uint32_t actionId, bool routeOpen = true) {
    DefenderGuardState d{};
    d.practiceAdvancedModeActive = true;
    d.guardGauge = 10000;
    d.actionId = actionId;
    d.neutralRouteOpen = routeOpen;
    return d;
}

static void TestEligibility() {
    TEST_CHECK(CanAutoGuardAtContact(Defender(2)), "standing neutral can guard");
    TEST_CHECK(CanAutoGuardAtContact(Defender(5)), "walking back can guard");
    TEST_CHECK(CanAutoGuardAtContact(Defender(63)), "proximity guard can guard");
    TEST_CHECK(CanAutoGuardAtContact(Defender(64)), "blockstun can change lane for the next hit");

    // Blockstun and proximity guard are guardable even with every route closed;
    // that is the whole point of allowing a lane change mid-string.
    TEST_CHECK(CanAutoGuardAtContact(Defender(64, false)),
               "blockstun guards even with routes closed");
    TEST_CHECK(CanAutoGuardAtContact(Defender(63, false)),
               "proximity guard guards even with routes closed");

    // A character special below action 85: the old ID-range rule called this
    // free and would have converted a counter-hit into a block. The closed
    // routes are the only thing that catches it.
    TEST_CHECK(!CanAutoGuardAtContact(Defender(44, false)),
               "committed sub-85 special cannot guard");
    TEST_CHECK(!CanAutoGuardAtContact(Defender(2, false)),
               "no open route means not actionable, whatever the action ID says");

    DefenderGuardState attacking = Defender(44);
    attacking.attackActive = true;
    TEST_CHECK(!CanAutoGuardAtContact(attacking), "live attack payload cannot guard");

    TEST_CHECK(!CanAutoGuardAtContact(Defender(72)), "hitstun cannot guard");
    TEST_CHECK(!CanAutoGuardAtContact(Defender(73)), "hitstun cannot guard");
    TEST_CHECK(!CanAutoGuardAtContact(Defender(74)), "knockdown cannot guard");
    TEST_CHECK(!CanAutoGuardAtContact(Defender(83)), "throw-receive state cannot guard");
    TEST_CHECK(!CanAutoGuardAtContact(Defender(85)), "shared attack action cannot guard");

    DefenderGuardState crushed = Defender(2);
    crushed.guardGauge = 0;
    TEST_CHECK(!CanAutoGuardAtContact(crushed), "guard crush cannot guard");

    DefenderGuardState swapped = Defender(2);
    swapped.controlSwapActive = true;
    TEST_CHECK(!CanAutoGuardAtContact(swapped), "control swap disables auto-block");

    DefenderGuardState macro = Defender(2);
    macro.macroOwnsDummyInput = true;
    TEST_CHECK(!CanAutoGuardAtContact(macro), "macro playback owns the dummy");

    DefenderGuardState off = Defender(2);
    off.practiceAdvancedModeActive = false;
    TEST_CHECK(!CanAutoGuardAtContact(off), "native dummy control disables auto-block");
}

// --- Semantic direction --------------------------------------------------

static void TestSemanticDirection() {
    TEST_CHECK(SemanticGuardForLane(GuardLane::Stand, false) == SemanticDirection::Back,
               "stand lane holds back");
    TEST_CHECK(SemanticGuardForLane(GuardLane::Crouch, false) == SemanticDirection::DownBack,
               "crouch lane holds down-back");
    TEST_CHECK(SemanticGuardForLane(GuardLane::Air, true) == SemanticDirection::AirBack,
               "airborne holds air back");
    TEST_CHECK(SemanticGuardForLane(GuardLane::Conflict, false) == SemanticDirection::Neutral,
               "conflict holds nothing");

    // Resolved from live facing, so a cross-up cannot stale the direction.
    const uint16_t left = 0x0004;
    const uint16_t right = 0x0008;
    const uint16_t down = 0x0002;
    TEST_CHECK(ResolveSemanticDirection(SemanticDirection::Back, true) == left,
               "facing right -> back is left");
    TEST_CHECK(ResolveSemanticDirection(SemanticDirection::Back, false) == right,
               "facing left -> back is right");
    TEST_CHECK(ResolveSemanticDirection(SemanticDirection::DownBack, true) == (left | down),
               "down-back combines both bits");
    TEST_CHECK(ResolveSemanticDirection(SemanticDirection::Neutral, true) == 0,
               "neutral resolves to no input");
}

// --- Simultaneous-threat scenarios --------------------------------------

static void TestSimultaneousScenarios() {
    // An ordinary high plus a low on the same frame: crouch covers both.
    const uint32_t highAndLow[] = {0x3, 0x2};
    TEST_CHECK(PlanFor(highAndLow, 2) == GuardLane::Crouch, "high + low -> crouch blocks both");

    // A stand-only overhead plus a low: nothing covers both.
    const uint32_t overheadAndLow[] = {0x1, 0x2};
    FrameGuardPlan plan{};
    ResetPlan(plan, 900, false);
    PlanAccumulateContact(plan, overheadAndLow[0]);
    PlanAccumulateContact(plan, overheadAndLow[1]);
    PlanFinalize(plan, true);
    TEST_CHECK(plan.lane == GuardLane::Conflict, "overhead + low -> conflict");
    TEST_CHECK(plan.conflictCount == 1, "conflict is counted");
    TEST_CHECK(!PlanAcceptsAttack(plan, 0x1, 0), "conflict frame blocks nothing");
    TEST_CHECK(!PlanAcceptsAttack(plan, 0x2, 0), "conflict frame blocks nothing, either lane");
}

int main() {
    TestGroundMaskDecoder();
    TestLaneIntersection();
    TestAirPlan();
    TestPerFrameLatch();
    TestFallbackRefusals();
    TestPolicy();
    TestContactGroups();
    TestSequenceLifetime();
    TestDeterministicRandom();
    TestScopedLaneBits();
    TestNativeGuardDetection();
    TestThreatIdentity();
    TestEligibility();
    TestSemanticDirection();
    TestSimultaneousScenarios();

    std::printf("auto_block_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
