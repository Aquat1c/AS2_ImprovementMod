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
};

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
