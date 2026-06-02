#pragma once

#include <stdint.h>

namespace Training {

enum class ActionableContext : uint8_t {
    AttackerRecovery,
    DefenderRecovery,
    PracticeTrigger,
    ThreatWindowEnd,
    DebugOnly,
};

enum class ActionabilitySource : uint8_t {
    LegacyActionId,
    CandidateNativeFlag,
    HybridValidated,
    // State-class rule derived from the engine's own action-ID boundaries:
    // actionId < 85 (not performing an attack move) and not a forced-lock /
    // extended-recovery state. Catches every movement/neutral recovery state
    // instead of a hand-enumerated subset. See action_state_classifier.cpp.
    StateClass,
};

// True when the action-state ID denotes the character is performing an attack
// move (the CharAction_* handler range). Confirmed engine boundary: states
// 0..84 are reaction/movement/neutral; 85+ are attack moves.
bool IsAttackMoveState(uint32_t actionId);

// State-class actionability: the character is in a movement/neutral state it can
// act out of — not in an attack move, not in a forced lock or extended recovery,
// and not in a context-excluded landing. This mirrors how the engine itself
// classifies states rather than enumerating individual "free" action IDs.
bool IsStateClassActionable(uint32_t actionId, bool landingExcluded);

struct ActionStateSample {
    uint32_t actionId = 0;
    uint16_t actionPhase = 0;
    uint16_t actionFrame = 0;
    uint8_t nativeActionableCandidate = 0;
    uint8_t routeOrBoxFlags[24] = {};
    uint8_t attackState = 0;
    uint8_t hitActive = 0;
    uint8_t blockstun = 0;
    uint8_t hitstunDuration = 0;
    uint16_t knockbackTimer = 0;
};

struct ActionabilityResult {
    bool actionable = false;
    bool legacyActionable = false;
    bool nativeCandidate = false;
    bool forcedLocked = false;
    bool landingExcluded = false;
    const char* reason = "";
};

bool IsProximityGuard(uint32_t actionId);
bool IsBlockstun(uint32_t actionId);
bool IsHitstun(uint32_t actionId);
bool IsWakeupNoTech(uint32_t actionId);
bool IsAirTech(uint32_t actionId);
bool IsGroundTech(uint32_t actionId);
bool IsPostTech(uint32_t actionId);
bool IsTechOrPostTech(uint32_t actionId);
bool IsKnockdownOrLaunch(uint32_t actionId);
bool IsForcedDefenderLock(uint32_t actionId);
bool IsContactStartState(uint32_t actionId);
const char* ActionCategory(uint32_t actionId);

ActionabilityResult EvaluateActionability(const ActionStateSample& sample,
                                           ActionableContext context,
                                           bool defenderWasAirLocked,
                                           ActionabilitySource source);

} // namespace Training
