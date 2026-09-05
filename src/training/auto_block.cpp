#include "training/auto_block.h"

#include "training/action_state_classifier.h"
#include "input/input_system.h"

namespace Training {
namespace {

// The frame-lane latch is written once per simulation frame and then only
// validated, so a conflict never rewrites it.
void LatchLane(FrameGuardPlan& plan, GuardLane lane, uint32_t commonLanes) {
    plan.lane = lane;
    plan.commonGroundLanes = commonLanes;
    plan.laneLatched = (lane == GuardLane::Stand ||
                        lane == GuardLane::Crouch ||
                        lane == GuardLane::Air);
}

uint32_t Mix(uint32_t h, uint32_t v) {
    h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
    return h;
}

} // namespace

bool SameThreatIdentity(const ThreatKey& a, const ThreatKey& b) {
    if (a.source != b.source || a.owner != b.owner) {
        return false;
    }
    // Slot is diagnostic only: HitDef compaction moves entries between frames,
    // so (owner, id) is the stable identity.
    return a.objectId == b.objectId;
}

uint32_t ThreatKeyHash(const ThreatKey& key) {
    uint32_t h = 0x811C9DC5u;
    h = Mix(h, static_cast<uint32_t>(key.source));
    h = Mix(h, key.owner);
    h = Mix(h, key.objectId);
    return h;
}

GroundGuardClass DecodeGroundGuardClass(uint32_t attackMask) {
    if ((attackMask & kAttackSpecialGuardRequired) != 0) {
        return GroundGuardClass::SpecialGuardRequired;
    }

    switch (attackMask & kGroundGuardMask) {
        case kGuardLaneStand:
            return GroundGuardClass::StandOnly;
        case kGuardLaneCrouch:
            return GroundGuardClass::CrouchOnly;
        case kGroundGuardMask:
            return GroundGuardClass::Either;
        default:
            return GroundGuardClass::None;
    }
}

const char* GroundGuardClassLabel(GroundGuardClass cls) {
    switch (cls) {
        case GroundGuardClass::StandOnly:            return "stand_only";
        case GroundGuardClass::CrouchOnly:           return "crouch_only";
        case GroundGuardClass::Either:               return "either";
        case GroundGuardClass::SpecialGuardRequired: return "special_guard";
        case GroundGuardClass::None:
        default:                                     return "no_lane";
    }
}

const char* GuardLaneLabel(GuardLane lane) {
    switch (lane) {
        case GuardLane::Stand:    return "stand";
        case GuardLane::Crouch:   return "crouch";
        case GuardLane::Air:      return "air";
        case GuardLane::None:     return "none";
        case GuardLane::Conflict: return "conflict";
        case GuardLane::Unset:
        default:                  return "unset";
    }
}

const char* ContactResolutionLabel(uint32_t result) {
    switch (result) {
        case 1:  return "none";
        case 3:  return "unique_defense";
        case 4:  return "just_parry";
        case 5:  return "repel";
        case 6:  return "push_away";
        case 7:  return "absolute_defense";
        case 8:  return "dodge";
        case 9:  return "guard_point";
        case 10: return "guard";
        case 11: return "hit";
        default: return "unknown";
    }
}

const char* BlockPolicyLabel(BlockPolicy policy) {
    switch (policy) {
        case BlockPolicy::All:           return "all";
        case BlockPolicy::StanceOnly:    return "stance_only";
        case BlockPolicy::FirstHit:      return "first_hit";
        case BlockPolicy::AfterFirstHit: return "after_first_hit";
        case BlockPolicy::Random:        return "random";
        case BlockPolicy::Off:
        default:                         return "off";
    }
}

bool AttackHasGroundGuardLane(uint32_t attackMask) {
    return (attackMask & kGroundGuardMask) != 0;
}

bool NativeAirGuardPredicate(uint32_t attackMask, uint32_t defenderFlags) {
    // sub_4A5EF0 air branch, reproduced exactly.
    if ((defenderFlags & kGroundGuardMask) != kGroundGuardMask) {
        return false;
    }
    return (attackMask & kGuardLaneAir) != 0 || (defenderFlags & kDefenderAirGuard) != 0;
}

bool SpecialGuardSatisfied(uint32_t attackMask, uint32_t defenderFlags) {
    if ((attackMask & kAttackSpecialGuardRequired) == 0) {
        return true;
    }
    return (defenderFlags & kDefenderSpecialGuard) != 0;
}

void ResetPlan(FrameGuardPlan& plan, uint32_t simFrame, bool defenderAirborne) {
    plan = FrameGuardPlan{};
    plan.simFrame = simFrame;
    plan.defenderAirborne = defenderAirborne;
    plan.commonGroundLanes = kGroundGuardMask;
}

void PlanAccumulateContact(FrameGuardPlan& plan, uint32_t attackMask) {
    plan.threatCount++;

    const GroundGuardClass cls = DecodeGroundGuardClass(attackMask);
    if (cls == GroundGuardClass::SpecialGuardRequired || cls == GroundGuardClass::None) {
        // Not ordinarily guardable: no lane can cover this frame.
        plan.guardBypassPresent = true;
        plan.commonGroundLanes = 0;
        return;
    }

    if (plan.guardBypassPresent) {
        return;
    }

    if (plan.defenderAirborne && (attackMask & kGuardLaneAir) == 0) {
        // Airborne dummy cannot ordinarily guard a non-air-blockable attack.
        plan.guardBypassPresent = true;
        plan.commonGroundLanes = 0;
        return;
    }

    plan.commonGroundLanes &= (attackMask & kGroundGuardMask);
}

void PlanFinalize(FrameGuardPlan& plan, bool preferCrouch) {
    plan.exactContactScan = true;

    if (plan.threatCount == 0) {
        LatchLane(plan, GuardLane::Unset, kGroundGuardMask);
        plan.valid = false;
        return;
    }

    plan.valid = true;

    if (plan.guardBypassPresent) {
        LatchLane(plan, GuardLane::None, 0);
        return;
    }

    if (plan.defenderAirborne) {
        LatchLane(plan, GuardLane::Air, kGroundGuardMask);
        return;
    }

    switch (plan.commonGroundLanes) {
        case kGuardLaneStand:
            LatchLane(plan, GuardLane::Stand, kGuardLaneStand);
            break;
        case kGuardLaneCrouch:
            LatchLane(plan, GuardLane::Crouch, kGuardLaneCrouch);
            break;
        case kGroundGuardMask:
            LatchLane(plan,
                      preferCrouch ? GuardLane::Crouch : GuardLane::Stand,
                      kGroundGuardMask);
            break;
        default:
            // Empty intersection with more than one threat is a real high/low
            // conflict; a human could not guard both.
            plan.conflictCount++;
            LatchLane(plan, GuardLane::Conflict, 0);
            break;
    }
}

bool PlanLatchFromContact(FrameGuardPlan& plan,
                          uint32_t attackMask,
                          uint32_t defenderFlags,
                          bool preferCrouch) {
    if (plan.laneLatched || plan.lane == GuardLane::Conflict || plan.lane == GuardLane::None) {
        return plan.laneLatched;
    }

    plan.valid = true;
    plan.exactContactScan = false;

    if (!SpecialGuardSatisfied(attackMask, defenderFlags)) {
        plan.guardBypassPresent = true;
        LatchLane(plan, GuardLane::None, 0);
        return false;
    }

    if (plan.defenderAirborne) {
        if (!NativeAirGuardPredicate(attackMask, defenderFlags | kGroundGuardMask)) {
            plan.guardBypassPresent = true;
            LatchLane(plan, GuardLane::None, 0);
            return false;
        }
        LatchLane(plan, GuardLane::Air, kGroundGuardMask);
        return true;
    }

    const uint32_t lanes = attackMask & kGroundGuardMask;
    if (lanes == kGuardLaneStand) {
        LatchLane(plan, GuardLane::Stand, kGuardLaneStand);
        return true;
    }
    if (lanes == kGuardLaneCrouch) {
        LatchLane(plan, GuardLane::Crouch, kGuardLaneCrouch);
        return true;
    }
    if (lanes == kGroundGuardMask) {
        LatchLane(plan, preferCrouch ? GuardLane::Crouch : GuardLane::Stand, kGroundGuardMask);
        return true;
    }

    plan.guardBypassPresent = true;
    LatchLane(plan, GuardLane::None, 0);
    return false;
}

bool PlanAcceptsAttack(const FrameGuardPlan& plan,
                       uint32_t attackMask,
                       uint32_t defenderFlags) {
    if (!plan.valid || !plan.laneLatched) {
        return false;
    }
    if (!SpecialGuardSatisfied(attackMask, defenderFlags)) {
        return false;
    }

    switch (plan.lane) {
        case GuardLane::Stand:
            return (attackMask & kGuardLaneStand) != 0;
        case GuardLane::Crouch:
            return (attackMask & kGuardLaneCrouch) != 0;
        case GuardLane::Air:
            // The lane supplies the ground bits only; 0x4 must come from the
            // attack or from a capability the defender already had.
            return NativeAirGuardPredicate(attackMask, defenderFlags | kGroundGuardMask);
        default:
            return false;
    }
}

uint32_t GuardLaneBits(GuardLane lane) {
    switch (lane) {
        case GuardLane::Stand:  return kGuardLaneStand;
        case GuardLane::Crouch: return kGuardLaneCrouch;
        case GuardLane::Air:    return kGroundGuardMask;
        default:                return 0;
    }
}

uint32_t ApplyGuardLaneBits(uint32_t oldFlags, GuardLane lane) {
    const uint32_t bits = GuardLaneBits(lane);
    if (bits == 0) {
        return oldFlags;
    }
    return (oldFlags & ~kTemporaryGuardBits) | bits;
}

uint32_t RestoreGuardLaneBits(uint32_t afterFlags, uint32_t oldFlags) {
    // Keep whatever native code changed in the high bits; put back only ours.
    return (afterFlags & ~kTemporaryGuardBits) | (oldFlags & kTemporaryGuardBits);
}

bool DefenderAlreadyGuards(uint32_t attackMask, uint32_t defenderFlags, bool airborne) {
    if (!SpecialGuardSatisfied(attackMask, defenderFlags)) {
        return false;
    }
    if (airborne) {
        return NativeAirGuardPredicate(attackMask, defenderFlags);
    }
    return ((attackMask & kGuardLaneStand) != 0 && (defenderFlags & kGuardLaneStand) != 0) ||
           ((attackMask & kGuardLaneCrouch) != 0 && (defenderFlags & kGuardLaneCrouch) != 0);
}

bool CanAutoGuardAtContact(const DefenderGuardState& state) {
    if (!state.practiceAdvancedModeActive) return false;
    if (state.macroOwnsDummyInput) return false;
    if (state.guardGauge == 0) return false;

    // Hitstun is deliberately rejected so After First Hit cannot escape a true
    // combo. Blockstun is deliberately allowed so the lane can change for the
    // next hit, exactly as a player could.
    if (IsHitstun(state.actionId)) return false;
    if (IsKnockdownOrLaunch(state.actionId)) return false;
    if (state.actionId == 83 || state.actionId == 84) return false;

    // Committed to an attack: augmenting here would turn a counter-hit into a
    // block. Both the shared normals AND character specials must be caught, and
    // only the route vector sees the latter.
    if (state.attackActive) return false;
    if (IsSharedAttackAction(state.actionId)) return false;

    // Blockstun and proximity guard are explicitly guardable even though their
    // routes are closed; anything else needs the engine to actually be willing
    // to take an ordinary input.
    return IsBlockstun(state.actionId) ||
           IsProximityGuard(state.actionId) ||
           state.neutralRouteOpen;
}

bool PolicyAllowsLane(BlockPolicy policy, uint32_t attackMask, bool preferCrouch) {
    if (policy != BlockPolicy::StanceOnly) {
        return true;
    }
    // The stance's own lane, and only that one. An attack that does not offer
    // it is a mixup the stance loses to, which is the whole point of the mode.
    const uint32_t lane = preferCrouch ? kGuardLaneCrouch : kGuardLaneStand;
    return (attackMask & lane) != 0;
}

bool PolicyWantsBlock(BlockPolicy policy,
                      const AutoBlockSequenceState& sequence,
                      uint32_t contactOrdinal) {
    switch (policy) {
        case BlockPolicy::All:
        case BlockPolicy::StanceOnly:
            return true;
        case BlockPolicy::FirstHit:
            return contactOrdinal == 0;
        case BlockPolicy::AfterFirstHit:
            return contactOrdinal >= 1;
        case BlockPolicy::Random:
            return sequence.randomBlock;
        case BlockPolicy::Off:
        default:
            return false;
    }
}

uint32_t DeterministicSequenceRoll(uint32_t startFrame,
                                   uint32_t generation,
                                   uint32_t attackerCharId,
                                   uint32_t defenderCharId,
                                   const ThreatKey& firstThreat) {
    uint32_t h = 0x2545F491u;
    h = Mix(h, startFrame);
    h = Mix(h, generation);
    h = Mix(h, attackerCharId);
    h = Mix(h, defenderCharId);
    h = Mix(h, ThreatKeyHash(firstThreat));
    h ^= h >> 16;
    return h;
}

void SequenceBegin(AutoBlockSequenceState& sequence,
                   uint32_t simFrame,
                   uint32_t roll,
                   int randomPercent) {
    sequence.active = true;
    sequence.generation++;
    sequence.startFrame = simFrame;
    sequence.lastThreatFrame = simFrame;
    sequence.lastContactFrame = 0xFFFFFFFFu;
    sequence.resolvedContactGroups = 0;
    sequence.quietFrames = 0;

    int percent = randomPercent;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    sequence.randomBlock = (roll % 100u) < static_cast<uint32_t>(percent);
}

void SequenceNoteThreat(AutoBlockSequenceState& sequence, uint32_t simFrame) {
    sequence.lastThreatFrame = simFrame;
    sequence.quietFrames = 0;
}

bool SequenceNoteContactGroup(AutoBlockSequenceState& sequence, uint32_t simFrame) {
    if (sequence.lastContactFrame == simFrame) {
        return false;
    }
    sequence.lastContactFrame = simFrame;
    sequence.resolvedContactGroups++;
    sequence.quietFrames = 0;
    return true;
}

bool SequenceTickQuiet(AutoBlockSequenceState& sequence,
                       bool threatPresent,
                       bool defenderInForcedState,
                       uint32_t quietGrace) {
    if (!sequence.active) {
        return false;
    }
    if (threatPresent || defenderInForcedState) {
        sequence.quietFrames = 0;
        return false;
    }
    sequence.quietFrames++;
    return sequence.quietFrames >= quietGrace;
}

void SequenceEnd(AutoBlockSequenceState& sequence) {
    sequence.active = false;
    sequence.quietFrames = 0;
    sequence.resolvedContactGroups = 0;
    sequence.lastContactFrame = 0xFFFFFFFFu;
    sequence.randomBlock = false;
}

DefensiveResponse ResponseForCategory(int category) {
    switch (category) {
        case kDefenseCategoryJustParry: return DefensiveResponse::JustParry;
        case kDefenseCategoryDodge:     return DefensiveResponse::Dodge;
        case kDefenseCategoryRepel:     return DefensiveResponse::Repel;
        case kDefenseCategoryUnique:    return DefensiveResponse::GuardCounter;
        case kDefenseCategoryPushAway:  return DefensiveResponse::PushAwayPerfect;
        case kDefenseCategoryAbsolute:  return DefensiveResponse::AbsoluteDefence;
        default:                        return DefensiveResponse::NormalGuard;
    }
}

bool ResponseArmsBeforeContact(DefensiveResponse response) {
    // Each of these has to be under way BEFORE the hit lands: parry and repel
    // open a window, and the category-1 counter is a move with startup whose
    // +1940 & 0x10 only exists while it runs. Driving any of them from a live
    // hitbox is asking for the input one frame after it could have mattered.
    return response == DefensiveResponse::JustParry ||
           response == DefensiveResponse::Repel ||
           response == DefensiveResponse::GuardCounter ||
           response == DefensiveResponse::GuardCounterBack ||
           // The motion has to have matched, and the 57-frame state has to be
           // up, before the hit it is meant to absorb.
           response == DefensiveResponse::AbsoluteDefenceFirst;
}

bool ResponseArmedByNativeHook(DefensiveResponse response) {
    switch (response) {
        case DefensiveResponse::JustParry:        // Entity_CheckHitState
        case DefensiveResponse::Repel:            // Entity_CheckGuardState
        case DefensiveResponse::PushAwayPerfect:  // Entity_CheckAirTech
        case DefensiveResponse::PushAwayMetered:
        // The preemptive entry is armed by writing the counter-guard route
        // byte, which is the engine's own record that command 26 matched -
        // after that every gate and every frame of the state is the engine's.
        case DefensiveResponse::AbsoluteDefenceFirst:
            return true;
        // The on-block entry is deliberately NOT here. Its gate is +1940 & 0x100,
        // which belongs to action 49/52 - lending it at the resolver would
        // absorb every hit from every state with no action entered and no stock
        // spent. The engine's own route is used instead: stock, blockstun,
        // cancel.
        default:
            return false;
    }
}

bool ResponseSupportedByCategory(DefensiveResponse response, int category) {
    switch (response) {
        case DefensiveResponse::JustParry: return category == kDefenseCategoryJustParry;
        case DefensiveResponse::Dodge:
        case DefensiveResponse::DodgeForward:
            return category == kDefenseCategoryDodge;
        case DefensiveResponse::GuardCounterBack:
            return category == kDefenseCategoryUnique;
        case DefensiveResponse::Repel:     return category == kDefenseCategoryRepel;
        case DefensiveResponse::GuardCounter: return category == kDefenseCategoryUnique;
        case DefensiveResponse::PushAwayPerfect:
        case DefensiveResponse::PushAwayMetered:
            return category == kDefenseCategoryPushAway;
        case DefensiveResponse::AbsoluteDefence:
        case DefensiveResponse::AbsoluteDefenceFirst:
            return category == kDefenseCategoryAbsolute;
        case DefensiveResponse::CharacterNative:
        case DefensiveResponse::NormalGuard:
        default:                           return true;
    }
}

const char* DefensiveResponseLabel(DefensiveResponse response) {
    switch (response) {
        case DefensiveResponse::CharacterNative: return "character_native";
        case DefensiveResponse::JustParry:       return "just_parry";
        case DefensiveResponse::Dodge:           return "dodge_back";
        case DefensiveResponse::DodgeForward:    return "dodge_forward";
        case DefensiveResponse::GuardCounterBack: return "guard_counter_back";
        case DefensiveResponse::Repel:           return "repel";
        case DefensiveResponse::GuardCounter:    return "guard_counter";
        case DefensiveResponse::PushAwayPerfect: return "push_away_perfect";
        case DefensiveResponse::PushAwayMetered: return "push_away_metered";
        case DefensiveResponse::AbsoluteDefence: return "absolute_defence_block";
        case DefensiveResponse::AbsoluteDefenceFirst: return "absolute_defence_first";
        case DefensiveResponse::NormalGuard:
        default:                                 return "guard_only";
    }
}

bool DodgeWindowOpen(uint32_t actionId, uint16_t meter) {
    if (meter < kDodgeMeterCost) {
        return false;
    }
    // Entity_UpdateAction_Attacks offers the dodge from *all six* blockstun
    // actions: stand 64/65 and crouch 67/68 both route to 59/60, air 70/71 to
    // 61/62. An earlier pass listed only the last four, so a dummy blocking
    // standing up never even tried.
    return actionId == 64 || actionId == 65 ||
           actionId == 67 || actionId == 68 ||
           actionId == 70 || actionId == 71;
}

bool ResponseUsesBlockstunCancel(DefensiveResponse response) {
    switch (response) {
        case DefensiveResponse::Dodge:
        case DefensiveResponse::DodgeForward:
        case DefensiveResponse::PushAwayPerfect:
        case DefensiveResponse::PushAwayMetered:
        case DefensiveResponse::AbsoluteDefence:
        case DefensiveResponse::AbsoluteDefenceFirst:
            return true;
        default:
            return false;
    }
}

bool ResponseRequiresBlockstun(DefensiveResponse response) {
    switch (response) {
        // Guard cancels: the action switch that offers them is keyed on the
        // blockstun states.
        case DefensiveResponse::Dodge:
        case DefensiveResponse::DodgeForward:
        case DefensiveResponse::PushAwayPerfect:
        case DefensiveResponse::PushAwayMetered:
            return true;
        // The cancel that produces action 49 is only offered from blockstun, so
        // a dummy that never guards never reaches it however much stock it has.
        case DefensiveResponse::AbsoluteDefence:
            return true;
        // Parry arms on a fresh BACK press from any state - the window is only
        // shorter out of blockstun. Repel arms from neutral, so blocking would
        // actively prevent it. The category-1 counter has no action gate at all.
        default:
            return false;
    }
}

bool PushAwayFreeWindow(uint32_t actionId) {
    switch (actionId) {
        case 64: case 65:
        case 67: case 68:
        case 70: case 71:
        case 44: case 46: case 48:
            return true;
        default:
            return false;
    }
}

bool PushAwayWindowOpen(uint32_t actionId, uint16_t meter) {
    // Entity_CheckAirTech arms on either branch, and they are an either/or, not
    // an and: a fresh press inside one of these actions is the free variant,
    // and 100 meter covers every other state. Requiring both was stricter than
    // the engine.
    switch (actionId) {
        case 64: case 65:
        case 67: case 68:
        case 70: case 71:
        case 44: case 46: case 48:
            return true;
        default:
            return meter >= kPushAwayMeterCost;
    }
}

const char* DefenseInputKindLabel(DefenseInputKind kind) {
    switch (kind) {
        case DefenseInputKind::ReleaseGuard:   return "release";
        case DefenseInputKind::ForwardTap:     return "fwd tap";
        case DefenseInputKind::DownTap:        return "down tap";
        case DefenseInputKind::DodgePress:     return "back+D";
        case DefenseInputKind::CounterForward: return "fwd+D";
        case DefenseInputKind::CounterBack:    return "back+D";
        case DefenseInputKind::DodgeForwardPress: return "D";
        case DefenseInputKind::ArmGuardState:  return "214D";
        case DefenseInputKind::None:
        default:                               return "hold guard";
    }
}

bool ParryWantsRelease(ParryInputState& state, uint8_t windowByte, bool threatArmed) {
    if (!threatArmed) {
        state.releasedLastFrame = false;
        return false;
    }

    // A window already counting down does not need re-arming; holding BACK
    // through it is what keeps +1974 set for the lane check.
    if (windowByte != 0xFFu) {
        state.releasedLastFrame = false;
        return false;
    }

    // Idle window: one frame without BACK, then the next press is the edge
    // Entity_CheckHitState is looking for. Releasing twice in a row would just
    // drop the guard, so alternate.
    if (state.releasedLastFrame) {
        state.releasedLastFrame = false;
        return false;
    }
    state.releasedLastFrame = true;
    return true;
}

bool ThreatIsIncoming(const DefenseDriveSample& sample) {
    return sample.threatArmed || sample.recordsToAttack >= 0 || sample.attackerCommitted;
}

DefenseInputKind EvaluateDefenseInput(DefensiveResponse response,
                                      const DefenseDriveSample& sample,
                                      DefenseDriveState& state) {
    // A guard cancel is driven by the dummy's own state, not by an incoming
    // attack: once blockstun is entered the window is open whether or not the
    // attacker is still active. Requiring a live threat is what made dodge and
    // push-away fire only inside a long blockstring - the one case where the
    // next hit happens to be armed while the previous one still holds the
    // dummy. On a single hit the threat is already gone by the frame blockstun
    // begins, so the window opened and closed with the driver switched off.
    //
    // Parry and repel do need to know one is coming - but "coming", not
    // "landing". They watch ThreatIsIncoming, which reads the attacker's own
    // animation records ahead of where it has got to, rather than threatArmed
    // (a box is live). Waiting for the box is what made repel miss: its window
    // opens from a neutral tap and lasts 24 frames, and by the time a box
    // exists the tap has to have happened already.
    //
    // None of that applies to a mechanic the native arming hook is opening
    // directly. Its window is already set by the time the dummy could have
    // pressed anything, and feeding the input as well would drop the guard for
    // a frame and walk the dummy forward for no gain.
    if (sample.nativeArmActive && ResponseArmedByNativeHook(response)) {
        state = DefenseDriveState{};
        return DefenseInputKind::None;
    }

    const bool blockstunDriven = ResponseRequiresBlockstun(response);
    const bool preArmed = ResponseArmsBeforeContact(response);
    const bool haveThreat = preArmed ? ThreatIsIncoming(sample) : sample.threatArmed;
    // Blockstun is a reason to keep driving on its own for anything with a
    // cancel there: the next hit of a string does not have to be detectable yet
    // for the cancel out of THIS one to be worth attempting.
    const bool inBlockstunCancel =
        IsBlockstun(sample.actionId) && ResponseUsesBlockstunCancel(response);
    if (!haveThreat && !blockstunDriven && !inBlockstunCancel) {
        state = DefenseDriveState{};
        return DefenseInputKind::None;
    }

    switch (response) {
        case DefensiveResponse::JustParry:
            return ParryWantsRelease(state.parry, sample.parryWindow, true)
                       ? DefenseInputKind::ReleaseGuard
                       : DefenseInputKind::None;

        case DefensiveResponse::Dodge:
        case DefensiveResponse::DodgeForward: {
            // The engine's condition is a FRESH D press (+78), so the press is
            // alternated rather than held: if the first one does not take -
            // the route not open yet on the frame blockstun began, say - a held
            // D reads 0 there forever and the cancel is simply never offered.
            if (!DodgeWindowOpen(sample.actionId, sample.meter)) {
                state.dodgePhase = false;
                return DefenseInputKind::None;
            }
            state.dodgePhase = !state.dodgePhase;
            if (!state.dodgePhase) {
                return DefenseInputKind::None;
            }
            // BACK held selects 60/62, released selects 59/61 - the roll's
            // direction is the only difference between the two responses.
            return response == DefensiveResponse::DodgeForward
                       ? DefenseInputKind::DodgeForwardPress
                       : DefenseInputKind::DodgePress;
        }

        case DefensiveResponse::PushAwayPerfect:
            // Physically the same press as the dodge - BACK + D - because the
            // engine routes one input by category rather than by button.
            //
            // Timing is the whole difference between the two variants. The free
            // one needs the D press taken as a *fresh edge* inside blockstun; a
            // D already held from before contact reads 0 at +78 there and drops
            // to the branch that charges 100 meter. So D is never pressed
            // outside an accepting action - which is what leaves it released
            // when blockstun begins - and it alternates while inside one, so a
            // press that did not take can edge again on the next frame.
            if (!PushAwayFreeWindow(sample.actionId)) {
                state.pushPhase = false;
                return DefenseInputKind::None;
            }
            state.pushPhase = !state.pushPhase;
            return state.pushPhase ? DefenseInputKind::DodgePress
                                   : DefenseInputKind::None;

        case DefensiveResponse::PushAwayMetered:
            // The untimed press: D simply held. Inside blockstun that makes
            // +78 read 0, which is exactly what sends Entity_CheckAirTech to
            // its "or 100 meter" arm, so this is the paid variant by
            // construction rather than by accident.
            return PushAwayWindowOpen(sample.actionId, sample.meter)
                       ? DefenseInputKind::DodgePress
                       : DefenseInputKind::None;

        case DefensiveResponse::Repel:
            // Entity_CheckGuardState only arms from neutral, so the tap has to
            // be preceded by a frame with no direction at all. Nothing to do
            // while a reaction is already armed - it lasts 24 frames.
            if (sample.reactionState != kReactionIdle) {
                state.tapPhase = false;
                return DefenseInputKind::None;
            }
            state.tapPhase = !state.tapPhase;
            if (state.tapPhase) {
                return DefenseInputKind::ReleaseGuard;
            }
            // Entity_CheckGuardState has both arms: a forward edge from neutral
            // sets reaction 5 (7 airborne), a down edge sets 6. A crouch-only
            // attack is a low, and only the down edge parries it.
            return sample.threatClass == GroundGuardClass::CrouchOnly
                       ? DefenseInputKind::DownTap
                       : DefenseInputKind::ForwardTap;

        case DefensiveResponse::GuardCounter:
        case DefensiveResponse::GuardCounterBack:
            // 6D moves the character forward, which is the opposite of holding
            // guard - so it has to be one press per threat rather than a held
            // direction, or the dummy would simply never block. The guide lists
            // it as "6D or 4D; air OK", and the engine has the airborne branch,
            // so height is its business, not ours.
            if (state.counterFired || !sample.actionable) {
                return DefenseInputKind::None;
            }
            state.counterFired = true;
            return response == DefensiveResponse::GuardCounterBack
                       ? DefenseInputKind::CounterBack
                       : DefenseInputKind::CounterForward;

        case DefensiveResponse::AbsoluteDefence:
            // Nothing to press. The blockstun handlers open route 23 themselves
            // (Input_UpdateMinValue_1675 sits in all three), so the guard-cancel
            // offer runs every blockstun frame on its own; the only term the
            // dummy is missing is the +823 charge, and that is supplied where
            // the engine reads it.
            state.armStep = -1;
            return DefenseInputKind::None;

        case DefensiveResponse::AbsoluteDefenceFirst:
            // The other entry: sub_522F30 wants command 26 to have matched, so
            // the motion is fed for real. It loops rather than running once -
            // the route consumes the match whether or not a hit came, and the
            // state it opens only lasts 57 frames. Outside a threat it would
            // just walk the dummy backwards, so it is gated on one coming.
            state.armStep = (state.armStep < 0 ||
                             state.armStep + 1 >= kGuardStateMotionFrames)
                                ? 0
                                : (int8_t)(state.armStep + 1);
            return DefenseInputKind::ArmGuardState;

        case DefensiveResponse::CharacterNative:
        case DefensiveResponse::NormalGuard:
        default:
            return DefenseInputKind::None;
    }
}

SemanticDirection SemanticGuardForLane(GuardLane lane, bool defenderAirborne) {
    if (defenderAirborne) {
        return lane == GuardLane::Air ? SemanticDirection::AirBack : SemanticDirection::Neutral;
    }
    switch (lane) {
        case GuardLane::Stand:  return SemanticDirection::Back;
        case GuardLane::Crouch: return SemanticDirection::DownBack;
        default:                return SemanticDirection::Neutral;
    }
}

uint16_t ResolveSemanticDirection(SemanticDirection direction, bool defenderFacingRight) {
    const uint16_t back = static_cast<uint16_t>(defenderFacingRight ? INPUT_LEFT : INPUT_RIGHT);
    switch (direction) {
        case SemanticDirection::Back:
        case SemanticDirection::AirBack:
            return back;
        case SemanticDirection::DownBack:
            return static_cast<uint16_t>(back | INPUT_DOWN);
        case SemanticDirection::Neutral:
        default:
            return 0;
    }
}

} // namespace Training
