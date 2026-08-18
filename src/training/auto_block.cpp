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
        case BlockPolicy::Adaptive:      return "adaptive";
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
    if (state.controlSwapActive) return false;
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

bool PolicyWantsBlock(BlockPolicy policy,
                      const AutoBlockSequenceState& sequence,
                      uint32_t contactOrdinal) {
    switch (policy) {
        case BlockPolicy::All:
        case BlockPolicy::Adaptive:
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
