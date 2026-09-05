#include "training/auto_block.h"
#include "training/motion_script.h"

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
    TEST_CHECK(PolicyWantsBlock(BlockPolicy::StanceOnly, seq, 3),
               "Stance Only still wants to block; the lane check is what refuses");

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

    // The gap between two hits of a blockstring is quiet frames with the
    // defender already out of blockstun, so the grace is what decides whether
    // the next hit is "the same pressure" or a fresh sequence. A short grace
    // made First Hit block the second hit of anything with a 2f gap as if it
    // were the first.
    for (uint32_t i = 1; i < kSequenceQuietGrace; ++i) {
        TEST_CHECK(!SequenceTickQuiet(seq, false, false, kSequenceQuietGrace),
                   "a gap inside the grace keeps the sequence alive");
    }
    TEST_CHECK(SequenceTickQuiet(seq, false, false, kSequenceQuietGrace), "grace expiry ends it");
    TEST_CHECK(kSequenceQuietGrace >= 20u,
               "the blockstring window has to outlast a real gap, not a 1f one");

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

// --- Just-parry re-arm ---------------------------------------------------

static void TestParryRearm() {
    ParryInputState st{};

    // No threat: never drop guard.
    TEST_CHECK(!ParryWantsRelease(st, 0xFF, false), "idle window but no threat -> hold");

    // Threat armed and the window is idle: owe a fresh edge, so release once.
    TEST_CHECK(ParryWantsRelease(st, 0xFF, true), "idle window + threat -> release");
    // Never twice running, or the dummy would simply stop guarding.
    TEST_CHECK(!ParryWantsRelease(st, 0xFF, true), "release never repeats back to back");
    TEST_CHECK(ParryWantsRelease(st, 0xFF, true), "and alternates so the edge keeps coming");

    // A live window needs BACK held, not released - that is what keeps +1974 set.
    ParryInputState live{};
    for (uint8_t frames = 1; frames <= 7; ++frames) {
        TEST_CHECK(!ParryWantsRelease(live, frames, true),
                   "a counting window is never interrupted");
    }

    // Losing the threat resets the cycle so the next string starts clean.
    ParryInputState reset{};
    ParryWantsRelease(reset, 0xFF, true);
    TEST_CHECK(!ParryWantsRelease(reset, 0xFF, false), "no threat -> hold");
    TEST_CHECK(ParryWantsRelease(reset, 0xFF, true), "cycle restarts on the next threat");
}

static void TestDefensiveResponses() {
    // The category map, cross-checked against the guide's own defence table.
    TEST_CHECK(ResponseForCategory(kDefenseCategoryJustParry) == DefensiveResponse::JustParry,
               "category 2 -> just parry");
    TEST_CHECK(ResponseForCategory(kDefenseCategoryDodge) == DefensiveResponse::Dodge,
               "category 6 -> dodge");
    // Push-away is guard + D, not automatic: Entity_CheckAirTech gates both of
    // its arming branches on D being held.
    TEST_CHECK(ResponseForCategory(kDefenseCategoryPushAway) == DefensiveResponse::PushAwayPerfect,
               "category 4 -> push-away");
    TEST_CHECK(ResponseSupportedByCategory(DefensiveResponse::PushAwayPerfect,
                                           kDefenseCategoryPushAway),
               "push-away is offered on category 4");
    TEST_CHECK(!ResponseSupportedByCategory(DefensiveResponse::PushAwayPerfect,
                                            kDefenseCategoryDodge),
               "push-away is refused on a dodge character");
    // Absolute defence has no input - +823 is a static capability flag, proved
    // by scanning as2.exe: nine reads of [base+337h], zero writes. It still
    // gets its own response, because it is reached only from blockstun, so
    // selecting it is what guarantees the dummy is blocking at all.
    TEST_CHECK(ResponseForCategory(kDefenseCategoryAbsolute) ==
                   DefensiveResponse::AbsoluteDefence,
               "category 5 -> absolute defence");
    {
        // The category-3 parry is 6 against high and mid but 2 against low, and
        // Entity_CheckGuardState arms a different reaction for each. Driving
        // only the forward one left a low unparryable.
        DefenseDriveState st{};
        DefenseDriveSample sm{};
        sm.threatArmed = true;
        sm.reactionState = kReactionIdle;

        auto tapFor = [&](GroundGuardClass cls) {
            st = DefenseDriveState{};
            sm.threatClass = cls;
            // The first frame is always the neutral one that makes the next
            // press a fresh edge; the second carries the direction.
            EvaluateDefenseInput(DefensiveResponse::Repel, sm, st);
            return EvaluateDefenseInput(DefensiveResponse::Repel, sm, st);
        };

        TEST_CHECK(tapFor(GroundGuardClass::CrouchOnly) == DefenseInputKind::DownTap,
                   "a low is parried with 2");
        TEST_CHECK(tapFor(GroundGuardClass::StandOnly) == DefenseInputKind::ForwardTap,
                   "an overhead is parried with 6");
        TEST_CHECK(tapFor(GroundGuardClass::Either) == DefenseInputKind::ForwardTap,
                   "a mid is parried with 6");
        TEST_CHECK(tapFor(GroundGuardClass::None) == DefenseInputKind::ForwardTap,
                   "an undecoded threat falls back to 6");

        // Either way the neutral frame still comes first, or the press is not
        // an edge and Entity_CheckGuardState ignores it.
        st = DefenseDriveState{};
        sm.threatClass = GroundGuardClass::CrouchOnly;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, sm, st) ==
                       DefenseInputKind::ReleaseGuard,
                   "the low parry still releases first");
    }

    {
        // Both push-block branches are selectable, and they differ only in
        // whether the D press is an edge. The timed one refuses to press
        // outside blockstun so the edge lands there; the untimed one holds D,
        // which is what makes +78 read 0 and takes the paid arm.
        DefenseDriveState st{};
        DefenseDriveSample sm{};
        sm.threatArmed = true;
        sm.actionId = 22;
        sm.meter = 999;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayMetered, sm, st) ==
                       DefenseInputKind::DodgePress,
                   "the metered variant presses outside blockstun");
        st = DefenseDriveState{};
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sm, st) ==
                       DefenseInputKind::None,
                   "the timed variant does not");

        // Held, not alternating - that is the whole point of the paid branch.
        st = DefenseDriveState{};
        sm.actionId = 67;
        for (int i = 0; i < 3; ++i) {
            TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayMetered, sm, st) ==
                           DefenseInputKind::DodgePress,
                       "the metered variant holds D rather than edging");
        }

        sm.meter = 0;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayMetered, sm, st) ==
                       DefenseInputKind::DodgePress,
                   "in blockstun it still presses with no meter");
        sm.actionId = 22;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayMetered, sm, st) ==
                       DefenseInputKind::None,
                   "but not outside it once the bar is empty");

        // Both belong to category 4 and nothing else.
        TEST_CHECK(ResponseSupportedByCategory(DefensiveResponse::PushAwayMetered,
                                               kDefenseCategoryPushAway),
                   "the metered variant is offered on category 4");
        TEST_CHECK(!ResponseSupportedByCategory(DefensiveResponse::PushAwayMetered,
                                                kDefenseCategoryRepel),
                   "and nowhere else");
        TEST_CHECK(ResponseForCategory(kDefenseCategoryPushAway) ==
                       DefensiveResponse::PushAwayPerfect,
                   "Native picks the free one");
    }

    // Absolute defence follows the engine's own route rather than being armed
    // at the resolver: the +1940 & 0x100 its resolver wants belongs to action
    // 49/52, so the only honest way in is stock plus blockstun plus the
    // guard-cancel offer. That means it implies guarding, and presses nothing.
    TEST_CHECK(ResponseRequiresBlockstun(DefensiveResponse::AbsoluteDefence),
               "the cancel is only offered from blockstun, so it implies guarding");
    TEST_CHECK(ResponseUsesBlockstunCancel(DefensiveResponse::AbsoluteDefence),
               "and its cancel lives there");
    TEST_CHECK(!ResponseArmsBeforeContact(DefensiveResponse::AbsoluteDefence),
               "there is no window to open ahead of the hit");
    {
        DefenseDriveState st{};
        DefenseDriveSample sm{};
        sm.actionable = true;
        sm.counterGuardStock = true;

        sm.actionId = 67;   // crouch blockstun, where the offer runs
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::AbsoluteDefence, sm, st) ==
                       DefenseInputKind::None,
                   "absolute defence presses nothing in blockstun");
        TEST_CHECK(st.armStep < 0, "and runs no motion");

        sm.actionId = 2;    // standing neutral
        sm.recordsToAttack = 4;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::AbsoluteDefence, sm, st) ==
                       DefenseInputKind::None,
                   "nor outside it - the charge is what the engine tests, not an input");
    }

    // The other entry. sub_522F30 queues the same action 49, but only when
    // command 26 has matched - so this one performs the 214D, before the first
    // hit rather than as a cancel out of blocking it. Both share the +823 gate.
    TEST_CHECK(ResponseSupportedByCategory(DefensiveResponse::AbsoluteDefenceFirst,
                                           kDefenseCategoryAbsolute),
               "the preemptive entry is category 5 too");
    TEST_CHECK(!ResponseSupportedByCategory(DefensiveResponse::AbsoluteDefenceFirst, 3),
               "and nowhere else");
    TEST_CHECK(ResponseArmsBeforeContact(DefensiveResponse::AbsoluteDefenceFirst),
               "the 57-frame state has to be up before the hit it absorbs");
    TEST_CHECK(!ResponseRequiresBlockstun(DefensiveResponse::AbsoluteDefenceFirst),
               "taking it first means NOT blocking first");
    TEST_CHECK(ResponseArmedByNativeHook(DefensiveResponse::AbsoluteDefenceFirst),
               "the counter-guard route byte IS a window the mod can write");
    {
        DefenseDriveState st{};
        DefenseDriveSample sm{};
        sm.actionable = true;
        sm.actionId = 2;
        sm.recordsToAttack = 3;   // an attack on its way, no box yet

        // Loops: the route consumes the match whether or not a hit came.
        int8_t seen[kGuardStateMotionFrames] = {};
        for (int i = 0; i < kGuardStateMotionFrames * 3; ++i) {
            TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::AbsoluteDefenceFirst, sm, st) ==
                           DefenseInputKind::ArmGuardState,
                       "214D keeps being fed while an attack is coming");
            TEST_CHECK(st.armStep >= 0 && st.armStep < kGuardStateMotionFrames,
                       "and the step stays inside the motion");
            seen[st.armStep] = 1;
        }
        for (int i = 0; i < kGuardStateMotionFrames; ++i) {
            TEST_CHECK(seen[i] != 0, "every frame of the motion gets fed");
        }

        // With the route arm installed there is no motion at all: feeding
        // 2-1-4 repeatedly is what let the command reader see 2...2 and give
        // the dummy a 22D instead of the counter guard.
        DefenseDriveSample hooked = sm;
        hooked.nativeArmActive = true;
        DefenseDriveState hookedSt{};
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::AbsoluteDefenceFirst,
                                        hooked, hookedSt) == DefenseInputKind::None,
                   "the route arm replaces the motion entirely");

        // Nothing coming: the motion would only walk the dummy backwards.
        DefenseDriveState idleSt{};
        DefenseDriveSample idle{};
        idle.actionId = 2;
        idle.actionable = true;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::AbsoluteDefenceFirst, idle, idleSt) ==
                       DefenseInputKind::None,
                   "nothing coming, nothing fed");

        // Blockstun still drives it, so a dummy that did end up blocking can
        // still take the cancel.
        DefenseDriveState blockSt{};
        DefenseDriveSample block{};
        block.actionId = 70;   // air blockstun
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::AbsoluteDefenceFirst, block, blockSt) ==
                       DefenseInputKind::ArmGuardState,
                   "and blockstun drives it too");
    }

    // The dodge's condition reads the DERIVED D word, so the press has to be a
    // fresh edge. Holding it means one chance and no retry.
    {
        DefenseDriveState st{};
        DefenseDriveSample sm{};
        sm.actionId = 67;
        sm.meter = 500;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, sm, st) ==
                       DefenseInputKind::DodgePress, "the dodge presses D");
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, sm, st) ==
                       DefenseInputKind::None, "releases so the next press is an edge");
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, sm, st) ==
                       DefenseInputKind::DodgePress, "and presses again");

        sm.actionId = 2;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, sm, st) ==
                       DefenseInputKind::None, "nothing outside blockstun");
        TEST_CHECK(!st.dodgePhase, "and the alternation resets with the window");
    }

    // The category-1 counter is a move with startup, so it has to be started
    // before the hit lands - the same rule as the two windows.
    TEST_CHECK(ResponseArmsBeforeContact(DefensiveResponse::GuardCounter),
               "the guard counter has to be under way before contact");
    {
        DefenseDriveState st{};
        DefenseDriveSample sm{};
        sm.actionable = true;
        sm.recordsToAttack = 4;   // startup only; no box exists yet
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::GuardCounter, sm, st) ==
                       DefenseInputKind::CounterForward,
                   "an attack still in startup is enough to commit the counter");
    }

    // The accepting action set is wider than the dodge's and the meter gate is
    // 100 rather than 500.
    TEST_CHECK(PushAwayWindowOpen(64, 100), "stand blockstun opens push-away");
    TEST_CHECK(PushAwayWindowOpen(67, 100), "crouch blockstun opens it");
    TEST_CHECK(PushAwayWindowOpen(70, 100), "air blockstun opens it");
    TEST_CHECK(PushAwayWindowOpen(44, 100), "guard action 44 opens it");
    TEST_CHECK(PushAwayWindowOpen(48, 999), "guard action 48, spare meter");
    // The two arming branches are an either/or: inside an accepting action the
    // free branch covers it, and outside one 100 meter does.
    TEST_CHECK(PushAwayWindowOpen(64, 0), "blockstun arms push-away with no meter");
    TEST_CHECK(PushAwayWindowOpen(22, 100), "100 meter arms it outside blockstun");
    TEST_CHECK(!PushAwayWindowOpen(22, 99), "neutral with 99 meter arms neither branch");

    // Entity_UpdateAction_Attacks offers the dodge from all six blockstun
    // actions; leaving out standing blockstun meant it never tried from there.
    TEST_CHECK(DodgeWindowOpen(64, 500), "stand blockstun opens the dodge");
    TEST_CHECK(DodgeWindowOpen(65, 500), "stand blockstun, second state");
    TEST_CHECK(!DodgeWindowOpen(22, 999), "neutral is not a dodge window");

    // Which mechanics can only happen out of blockstun, and therefore imply
    // that the dummy has to be blocking at all.
    TEST_CHECK(ResponseRequiresBlockstun(DefensiveResponse::Dodge),
               "dodge is a guard cancel");
    TEST_CHECK(ResponseRequiresBlockstun(DefensiveResponse::PushAwayPerfect),
               "push-away is a guard cancel");
    TEST_CHECK(!ResponseRequiresBlockstun(DefensiveResponse::JustParry),
               "parry arms from any state");
    TEST_CHECK(!ResponseRequiresBlockstun(DefensiveResponse::Repel),
               "repel arms from neutral, so blocking would prevent it");
    TEST_CHECK(!ResponseRequiresBlockstun(DefensiveResponse::GuardCounter),
               "the category-1 counter has no action gate");

    {
        // The free push block needs a fresh D edge taken inside blockstun. That
        // only happens if D was released beforehand, so the driver must not
        // press outside an accepting action however much meter is available.
        DefenseDriveState st{};
        DefenseDriveSample sm{};
        sm.threatArmed = true;
        sm.actionId = 22;          // neutral, closing in
        sm.meter = 999;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sm, st) ==
                       DefenseInputKind::None,
                   "no D outside blockstun, however much meter there is");

        // First blockstun frame: D goes down, and because it was up a moment
        // ago the engine reads +78 as just-pressed and takes the free branch.
        sm.actionId = 67;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sm, st) ==
                       DefenseInputKind::DodgePress,
                   "D lands on the first blockstun frame");
        // Then it releases, so a press that did not take can edge again.
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sm, st) ==
                       DefenseInputKind::None,
                   "D releases so the next press is an edge too");
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sm, st) ==
                       DefenseInputKind::DodgePress,
                   "and presses again");

        // Leaving blockstun rearms the sequence for the next contact.
        sm.actionId = 22;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sm, st) ==
                       DefenseInputKind::None,
                   "leaving blockstun stops the press");
        TEST_CHECK(!st.pushPhase, "and resets, so the next one starts with a press");
        sm.actionId = 64;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sm, st) ==
                       DefenseInputKind::DodgePress,
                   "stand blockstun arms it as well");
    }

    {
        // Push-away emits the same physical press as the dodge, because the
        // engine routes one input by category rather than by button.
        DefenseDriveState state{};
        DefenseDriveSample sample{};
        sample.threatArmed = true;
        sample.actionId = 67;
        sample.meter = 100;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sample, state) ==
                       DefenseInputKind::DodgePress,
                   "push-away presses guard + D");
        // Meter is irrelevant to the driver: it goes for the free branch, which
        // is gated on the action rather than on the bar.
        state = DefenseDriveState{};
        sample.meter = 0;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sample, state) ==
                       DefenseInputKind::DodgePress,
                   "blockstun arms push-away with no meter at all");
        state = DefenseDriveState{};
        sample.actionId = 22;
        sample.meter = 999;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sample, state) ==
                       DefenseInputKind::None,
                   "and a full bar does not make it press outside blockstun");
    }

    {
        // A guard cancel fires from blockstun with no attacker still active.
        // Requiring a live threat meant dodge only came out mid-blockstring,
        // where the next hit happened to be armed while the previous one still
        // held the dummy; on a single hit the threat was gone by the frame
        // blockstun began.
        DefenseDriveState state{};
        DefenseDriveSample sample{};
        sample.threatArmed = false;
        sample.actionId = 67;
        sample.meter = 500;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, sample, state) ==
                       DefenseInputKind::DodgePress,
                   "dodge fires from blockstun with no threat armed");

        sample.meter = 100;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::PushAwayPerfect, sample, state) ==
                       DefenseInputKind::DodgePress,
                   "push-away likewise");

        // Neutral with no threat is still nothing: the window, not the absence
        // of a check, is what gates them.
        sample.actionId = 22;
        sample.meter = 999;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, sample, state) ==
                       DefenseInputKind::None,
                   "no window, no dodge");

        // Parry and repel arm before the hit, so they still need to know one is
        // coming.
        sample.threatArmed = false;
        sample.parryWindow = 0xFF;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::JustParry, sample, state) ==
                       DefenseInputKind::None,
                   "parry still waits for a threat");
        sample.reactionState = kReactionIdle;
        TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, sample, state) ==
                       DefenseInputKind::None,
                   "repel still waits for a threat");
    }

    TEST_CHECK(ResponseSupportedByCategory(DefensiveResponse::JustParry, 2), "parry on cat 2");
    TEST_CHECK(!ResponseSupportedByCategory(DefensiveResponse::JustParry, 6), "no parry on cat 6");
    TEST_CHECK(!ResponseSupportedByCategory(DefensiveResponse::Dodge, 2), "no dodge on cat 2");
    TEST_CHECK(ResponseSupportedByCategory(DefensiveResponse::NormalGuard, 0),
               "guard-only always applies");
}

static DefenseDriveSample Drive(bool threat = true) {
    DefenseDriveSample d{};
    d.threatArmed = threat;
    d.actionable = true;
    d.parryWindow = 0xFF;
    d.reactionState = kReactionIdle;
    return d;
}

static void TestRepelDrive() {
    DefenseDriveState st{};
    DefenseDriveSample d = Drive();

    // Entity_CheckGuardState only arms from neutral, so the tap has to be
    // preceded by a frame with no direction at all.
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, d, st) ==
               DefenseInputKind::ReleaseGuard, "repel clears the direction first");
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, d, st) ==
               DefenseInputKind::ForwardTap, "then taps forward");
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, d, st) ==
               DefenseInputKind::ReleaseGuard, "and returns to neutral to re-arm");

    // A reaction is already armed for 24 frames; tapping again achieves nothing.
    d.reactionState = 5;
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, d, st) ==
               DefenseInputKind::None, "armed reaction is left alone");

    // No threat at all resets the cycle.
    DefenseDriveSample idle = Drive(false);
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, idle, st) ==
               DefenseInputKind::None, "no threat, no input");

    // Prediction, not observation. A hitbox that does not exist yet is exactly
    // the case the input driver has to cover, because the repel window opens
    // from a tap that must PRECEDE the hit.
    DefenseDriveSample coming = Drive(false);
    coming.recordsToAttack = 3;
    DefenseDriveState pred{};
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, coming, pred) ==
               DefenseInputKind::ReleaseGuard, "an attack still in startup arms the repel");

    DefenseDriveSample committed = Drive(false);
    committed.attackerCommitted = true;
    DefenseDriveState commit{};
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, committed, commit) ==
               DefenseInputKind::ReleaseGuard, "a committed attacker arms it too");
}

// The three window mechanics are armed through the engine's own setters, so
// when the hooks are live the driver must produce NO input: a forward tap would
// drop the guard and walk the dummy forward for a window that is already open.
static void TestNativeArmSuppressesInput() {
    TEST_CHECK(ResponseArmedByNativeHook(DefensiveResponse::JustParry), "parry is hooked");
    TEST_CHECK(ResponseArmedByNativeHook(DefensiveResponse::Repel), "repel is hooked");
    TEST_CHECK(ResponseArmedByNativeHook(DefensiveResponse::PushAwayPerfect),
               "push away is hooked");
    TEST_CHECK(!ResponseArmedByNativeHook(DefensiveResponse::Dodge),
               "the dodge is a guard cancel, not a window");
    TEST_CHECK(!ResponseArmedByNativeHook(DefensiveResponse::GuardCounter),
               "the counter is a guard cancel too");
    TEST_CHECK(!ResponseArmedByNativeHook(DefensiveResponse::AbsoluteDefence),
               "absolute defence's bit belongs to action 49, not to the resolver");

    DefenseDriveSample d = Drive();
    d.nativeArmActive = true;
    DefenseDriveState st{};
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, d, st) ==
               DefenseInputKind::None, "hooked repel needs no input");
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::JustParry, d, st) ==
               DefenseInputKind::None, "hooked parry needs no input");


    // A guard cancel is not a window, so the hook changes nothing about it.
    DefenseDriveSample dodge = Drive();
    dodge.nativeArmActive = true;
    dodge.actionId = 67;
    dodge.meter = 500;
    DefenseDriveState ds{};
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, dodge, ds) ==
               DefenseInputKind::DodgePress, "the dodge still presses D");

    // And with the hooks unavailable the input driver has to come back.
    DefenseDriveSample fallback = Drive();
    fallback.nativeArmActive = false;
    DefenseDriveState fs{};
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, fallback, fs) ==
               DefenseInputKind::ReleaseGuard, "no hook, back to the input driver");
}

static void TestGuardCounterDrive() {
    DefenseDriveState st{};
    DefenseDriveSample d = Drive();

    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::GuardCounter, d, st) ==
               DefenseInputKind::CounterForward, "category 1 presses 6D");

    // 6D holds FORWARD, so repeating it would mean never guarding again: one
    // press per threat, then back to the ordinary guard.
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::GuardCounter, d, st) ==
               DefenseInputKind::None, "the counter does not repeat while held");

    // The guide says "air OK" and the engine has the airborne branch, so height
    // is the engine's business.
    DefenseDriveState air{};
    DefenseDriveSample da = Drive();
    da.airborne = true;
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::GuardCounter, da, air) ==
               DefenseInputKind::CounterForward, "airborne counter is allowed");

    // Pressing it mid-move is thrown away, so do not.
    DefenseDriveState busy{};
    DefenseDriveSample db = Drive();
    db.actionable = false;
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::GuardCounter, db, busy) ==
               DefenseInputKind::None, "not actionable, no press");

    // A new threat re-arms it.
    DefenseDriveState again{};
    EvaluateDefenseInput(DefensiveResponse::GuardCounter, d, again);
    EvaluateDefenseInput(DefensiveResponse::GuardCounter, Drive(false), again);
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::GuardCounter, d, again) ==
               DefenseInputKind::CounterForward, "next threat gets its own counter");
}

static void TestDriveRouting() {
    DefenseDriveState st{};
    DefenseDriveSample d = Drive();
    d.actionId = 67;
    d.meter = 900;

    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Dodge, d, st) ==
               DefenseInputKind::DodgePress, "dodge presses D from blockstun");
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::NormalGuard, d, st) ==
               DefenseInputKind::None, "guard only never adds an input");
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::JustParry, d, st) ==
               DefenseInputKind::ReleaseGuard, "parry drops the direction to re-arm");

    // Category mapping now covers every response the mod can drive.
    TEST_CHECK(ResponseForCategory(kDefenseCategoryRepel) == DefensiveResponse::Repel,
               "category 3 -> repel");
    TEST_CHECK(ResponseForCategory(kDefenseCategoryUnique) == DefensiveResponse::GuardCounter,
               "category 1 -> guard counter");
    TEST_CHECK(!ResponseSupportedByCategory(DefensiveResponse::Repel, 6), "no repel on cat 6");
    TEST_CHECK(!ResponseSupportedByCategory(DefensiveResponse::GuardCounter, 3),
               "no counter on cat 3");
}

static void TestDodgeWindow() {
    // Only the blockstun states route 22 actually offers the cancel from.
    TEST_CHECK(DodgeWindowOpen(67, 500), "crouch blockstun opens the dodge");
    TEST_CHECK(DodgeWindowOpen(68, 900), "crouch blockstun, spare meter");
    TEST_CHECK(DodgeWindowOpen(70, 500), "air blockstun opens it");
    TEST_CHECK(DodgeWindowOpen(71, 500), "air blockstun, second state");

    TEST_CHECK(DodgeWindowOpen(64, 900), "stand blockstun is routed to dodge too");
    TEST_CHECK(!DodgeWindowOpen(2, 900), "neutral is not a guard cancel");
    TEST_CHECK(!DodgeWindowOpen(67, 499), "one short of the cost is still no");
    TEST_CHECK(!DodgeWindowOpen(67, 0), "no meter, no dodge");
}

// All must never degrade into the vanilla adaptive guard, which picks a POSTURE
// from a rule of thumb (sub_4A8D00 crouches unless mask & 3 == 1) and can
// therefore be opened up by the lane it guessed against. The mod's All takes
// the lane from the attack itself, so the stance cannot lose it a mixup.
static void TestAllBlocksBothLanesRegardlessOfStance() {
    // A stand-only attack is stand-guarded even with the crouch stance set...
    {
        FrameGuardPlan plan{};
        ResetPlan(plan, 1, false);
        PlanAccumulateContact(plan, kGuardLaneStand);
        PlanFinalize(plan, /*preferCrouch=*/true);
        TEST_CHECK(plan.lane == GuardLane::Stand,
                   "All stand-guards a high even while the stance prefers crouch");
    }
    // ...and a crouch-only attack is crouch-guarded even with the stand stance.
    {
        FrameGuardPlan plan{};
        ResetPlan(plan, 1, false);
        PlanAccumulateContact(plan, kGuardLaneCrouch);
        PlanFinalize(plan, /*preferCrouch=*/false);
        TEST_CHECK(plan.lane == GuardLane::Crouch,
                   "All crouch-guards a low even while the stance prefers stand");
    }
    // Only when the attack allows either does the stance get a say - and then
    // both lanes guard it, so the choice cannot lose.
    {
        FrameGuardPlan plan{};
        ResetPlan(plan, 1, false);
        PlanAccumulateContact(plan, kGroundGuardMask);
        PlanFinalize(plan, /*preferCrouch=*/true);
        TEST_CHECK(plan.lane == GuardLane::Crouch, "a mid follows the stance");
    }

    // And the policy layer never refuses a lane for All, whatever the stance.
    TEST_CHECK(PolicyAllowsLane(BlockPolicy::All, kGuardLaneCrouch, false),
               "All covers a low from a standing stance");
    TEST_CHECK(PolicyAllowsLane(BlockPolicy::All, kGuardLaneStand, true),
               "All covers a high from a crouching stance");
}

// The prediction must not answer a walk. attackerCommitted is set from the
// attacker's ATTACK PAYLOAD, which Entity_ResetHitData clears on entry to every
// ordinary action, so walking toward the dummy is not an incoming attack -
// reading the command-route vector instead treated it as one, and the dummy
// spent its counter guard on someone strolling over.
static void TestThreatPredictionIgnoresWalking() {
    DefenseDriveSample walking{};
    walking.actionable = true;
    walking.recordsToAttack = -1;   // no committed attack, so no lookahead
    walking.attackerCommitted = false;
    walking.threatArmed = false;
    TEST_CHECK(!ThreatIsIncoming(walking), "walking toward the dummy is not a threat");

    DefenseDriveSample startup = walking;
    startup.attackerCommitted = true;
    TEST_CHECK(ThreatIsIncoming(startup), "a committed attack is, before any box exists");

    DefenseDriveSample predicted = walking;
    predicted.recordsToAttack = 3;
    TEST_CHECK(ThreatIsIncoming(predicted), "and so is one the lookahead can see");

    DefenseDriveSample live = walking;
    live.threatArmed = true;
    TEST_CHECK(ThreatIsIncoming(live), "a live box obviously still counts");

    // Nothing that only reacts to a hit should be moved by the prediction.
    DefenseDriveState st{};
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::AbsoluteDefenceFirst, walking, st) ==
                   DefenseInputKind::None,
               "the preemptive counter guard stays put while the attacker walks");
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::Repel, walking, st) ==
                   DefenseInputKind::None,
               "and so does the repel arm");
    TEST_CHECK(EvaluateDefenseInput(DefensiveResponse::GuardCounter, walking, st) ==
                   DefenseInputKind::None,
               "and the counter");
}

// Adaptive holds the stance's own lane instead of switching to whatever is
// incoming, so a mixup the stance loses to actually lands.
static void TestAdaptiveLane() {
    // Standing: covers the stand lane, loses to a crouch-only attack.
    TEST_CHECK(PolicyAllowsLane(BlockPolicy::StanceOnly, kGuardLaneStand, false),
               "standing covers a high");
    TEST_CHECK(PolicyAllowsLane(BlockPolicy::StanceOnly, kGroundGuardMask, false),
               "standing covers a mid");
    TEST_CHECK(!PolicyAllowsLane(BlockPolicy::StanceOnly, kGuardLaneCrouch, false),
               "standing loses to a low");

    // Crouching: the mirror image.
    TEST_CHECK(PolicyAllowsLane(BlockPolicy::StanceOnly, kGuardLaneCrouch, true),
               "crouching covers a low");
    TEST_CHECK(!PolicyAllowsLane(BlockPolicy::StanceOnly, kGuardLaneStand, true),
               "crouching loses to a high");

    // Every other policy switches lane per contact, so none of them refuse.
    const BlockPolicy switching[] = { BlockPolicy::All, BlockPolicy::FirstHit,
                                      BlockPolicy::AfterFirstHit, BlockPolicy::Random };
    for (size_t i = 0; i < sizeof(switching) / sizeof(switching[0]); ++i) {
        TEST_CHECK(PolicyAllowsLane(switching[i], kGuardLaneCrouch, false),
                   "only Adaptive holds one lane");
        TEST_CHECK(PolicyAllowsLane(switching[i], kGuardLaneStand, true),
                   "only Adaptive holds one lane");
    }
}

// The motion string syntax. A charge segment and an explicitly named button
// are the two things a plain "one digit per frame" reader could not express,
// and both are needed for a route like 21[4]D~6C.
static void TestMotionScript() {
    MotionFrame frames[80];

    // Plain motions are unchanged: one frame per digit, no buttons of their own.
    TEST_CHECK(MotionLength("236", 50) == 3, "a plain motion is one frame per digit");
    TEST_CHECK(!MotionNamesButtons("236"), "a plain motion names no button");
    {
        const int n = ExpandMotion("236", 50, frames, 80);
        TEST_CHECK(n == 3, "236 expands to three frames");
        TEST_CHECK(frames[0].dir == '2' && frames[1].dir == '3' && frames[2].dir == '6',
                   "and in order");
        TEST_CHECK(frames[2].buttonMask == 0, "the row's button is applied by the caller");
    }

    // A charge holds one direction for the charge length.
    TEST_CHECK(MotionLength("[4]6", 50) == 51, "a charge is held for its whole length");
    {
        const int n = ExpandMotion("[4]6", 50, frames, 80);
        TEST_CHECK(n == 51, "charge plus the release frame");
        TEST_CHECK(frames[0].dir == '4' && frames[49].dir == '4', "held throughout");
        TEST_CHECK(frames[50].dir == '6', "then released forward");
    }

    // The route the absolute-defence follow-up needs: 2, 1, hold 4, D on the
    // last charge frame, then 6 with the row's own button.
    TEST_CHECK(MotionNamesButtons("21[4]d6"), "the route names its own D");
    TEST_CHECK(MotionExplicitButtons("21[4]d6") == (1u << 3), "and only D");
    {
        const int n = ExpandMotion("21[4]d6", 50, frames, 80);
        TEST_CHECK(n == 53, "2, 1, fifty charge frames, then the 6");
        TEST_CHECK(frames[0].dir == '2' && frames[1].dir == '1', "the 21 leads");
        TEST_CHECK(frames[2].dir == '4' && frames[51].dir == '4', "the charge is the 4");
        TEST_CHECK(frames[51].buttonMask == (1u << 3),
                   "D presses on the last charge frame, still holding 4");
        TEST_CHECK(frames[50].buttonMask == 0, "and not before it");
        TEST_CHECK(frames[52].dir == '6' && frames[52].buttonMask == 0,
                   "the cancel is a bare 6 - its button comes from the row");
    }

    // Degenerate input must not invent frames.
    TEST_CHECK(ExpandMotion(nullptr, 50, frames, 80) == 0, "no string, no frames");
    TEST_CHECK(MotionLength("21[4", 50) == 2, "an unterminated charge stops the expansion");
    TEST_CHECK(ExpandMotion("236", 50, frames, 2) == 2, "capacity is respected");
    TEST_CHECK(MotionLength("d", 50) == 0, "a button with no frame to attach to is dropped");
}

int main() {
    TestParryRearm();
    TestDefensiveResponses();
    TestDodgeWindow();
    TestRepelDrive();
    TestNativeArmSuppressesInput();
    TestGuardCounterDrive();
    TestDriveRouting();
    TestGroundMaskDecoder();
    TestLaneIntersection();
    TestAirPlan();
    TestPerFrameLatch();
    TestFallbackRefusals();
    TestPolicy();
    TestContactGroups();
    TestMotionScript();
    TestAdaptiveLane();
    TestThreatPredictionIgnoresWalking();
    TestAllBlocksBothLanesRegardlessOfStance();
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
