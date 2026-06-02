#include "training/action_state_classifier.h"

namespace Training {
namespace {

static bool IsLegacyFreeAction(uint32_t actionId, ActionableContext context) {
    switch (actionId) {
        case 2:   // standing neutral
        case 3:   // turnaround / facing correction
        case 4:   // walk forward
        case 5:   // walk back
        case 6:   // stand -> crouch transition
        case 7:   // crouch neutral
        case 8:   // crouch -> stand transition
        case 22:  // air neutral
        case 106: // healing stance cancel
            return true;

        case 23:  // landing; audit before treating as canonical recovery
            return context == ActionableContext::ThreatWindowEnd;

        case 63:  // stand prox guard
        case 66:  // crouch prox guard
        case 69:  // air prox guard
            return true;

        default:
            return false;
    }
}

} // namespace

bool IsProximityGuard(uint32_t actionId) {
    return actionId == 63 || actionId == 66 || actionId == 69;
}

bool IsBlockstun(uint32_t actionId) {
    return (actionId == 64 || actionId == 65) ||
           (actionId == 67 || actionId == 68) ||
           (actionId == 70 || actionId == 71);
}

bool IsHitstun(uint32_t actionId) {
    return actionId == 72 || actionId == 73;
}

bool IsWakeupNoTech(uint32_t actionId) {
    return actionId == 74;
}

bool IsAirTech(uint32_t actionId) {
    return actionId == 78;
}

bool IsGroundTech(uint32_t actionId) {
    return actionId >= 79 && actionId <= 81;
}

bool IsPostTech(uint32_t actionId) {
    return actionId == 82;
}

bool IsTechOrPostTech(uint32_t actionId) {
    return actionId >= 78 && actionId <= 82;
}

bool IsKnockdownOrLaunch(uint32_t actionId) {
    return actionId >= 74 && actionId <= 82;
}

bool IsForcedDefenderLock(uint32_t actionId) {
    return IsBlockstun(actionId) ||
           IsHitstun(actionId) ||
           IsKnockdownOrLaunch(actionId);
}

bool IsContactStartState(uint32_t actionId) {
    return IsBlockstun(actionId) || IsHitstun(actionId);
}

bool IsAttackMoveState(uint32_t actionId) {
    // Engine boundary (decomp: throw/state handlers branch on actionId < 85):
    // 0..84 are reaction/movement/neutral states, 85+ are the CharAction_*
    // attack-move handlers.
    return actionId >= 85;
}

bool IsStateClassActionable(uint32_t actionId, bool landingExcluded) {
    // Performing an attack move — not actionable until it returns to a state < 85.
    if (IsAttackMoveState(actionId)) {
        return false;
    }
    // Forced locks: blockstun/hitstun (64,65,67,68,70,71,72,73) and
    // knockdown/launch/tech (74..82).
    if (IsForcedDefenderLock(actionId)) {
        return false;
    }
    // Extended throw-receive / post-knockdown recovery states (83,84) observed
    // being assigned to the opponent during throw/special interactions. Treated
    // as locked until proven actionable by audit.
    if (actionId == 83 || actionId == 84) {
        return false;
    }
    // Landing (23) per context (excluded for defender recovery after air states).
    if (landingExcluded) {
        return false;
    }
    // Everything else < 85 is a movement/neutral state the character can act out
    // of: stand/crouch/walk/jump/air, proximity guard (63/66/69), etc. — including
    // states not present in the hand-enumerated legacy free list.
    return true;
}

const char* ActionCategory(uint32_t actionId) {
    if (IsProximityGuard(actionId)) return "ProxGuard";
    if (IsBlockstun(actionId)) return "Blockstun";
    if (IsHitstun(actionId)) return "Hitstun";
    if (IsWakeupNoTech(actionId)) return "WakeupNoTech";
    if (IsAirTech(actionId)) return "AirTech";
    if (IsGroundTech(actionId)) return "GroundTech";
    if (IsPostTech(actionId)) return "PostTech";
    if (actionId == 105 || actionId == 106) return "Healing";
    if (IsLegacyFreeAction(actionId, ActionableContext::DebugOnly)) return "Actionable";
    return "Other";
}

ActionabilityResult EvaluateActionability(const ActionStateSample& sample,
                                           ActionableContext context,
                                           bool defenderWasAirLocked,
                                           ActionabilitySource source) {
    ActionabilityResult out{};
    out.legacyActionable = IsLegacyFreeAction(sample.actionId, context);
    out.nativeCandidate = sample.nativeActionableCandidate != 0;
    out.forcedLocked = IsForcedDefenderLock(sample.actionId);

    if (sample.actionId == 23 &&
        (context != ActionableContext::ThreatWindowEnd ||
         (context == ActionableContext::DefenderRecovery && defenderWasAirLocked))) {
        out.landingExcluded = true;
    }

    if (out.forcedLocked) {
        out.actionable = false;
        out.reason = "forced_lock";
        return out;
    }

    if (out.landingExcluded) {
        out.actionable = false;
        out.reason = "landing_excluded";
        return out;
    }

    switch (source) {
        case ActionabilitySource::CandidateNativeFlag:
            out.actionable = out.nativeCandidate;
            out.reason = out.actionable ? "native_candidate" : "native_candidate_clear";
            break;

        case ActionabilitySource::HybridValidated:
            out.actionable = out.legacyActionable || out.nativeCandidate;
            out.reason = out.actionable ? "hybrid" : "hybrid_locked";
            break;

        case ActionabilitySource::StateClass:
            // Forced locks and landing exclusion were already resolved above; this
            // recomputes from the engine's state-class boundaries so any
            // movement/neutral recovery state counts, not just the legacy list.
            out.actionable = IsStateClassActionable(sample.actionId, out.landingExcluded);
            out.reason = out.actionable ? "state_class" : "state_class_locked";
            break;

        case ActionabilitySource::LegacyActionId:
        default:
            out.actionable = out.legacyActionable;
            out.reason = out.actionable ? "legacy_action" : "legacy_locked";
            break;
    }

    return out;
}

} // namespace Training
