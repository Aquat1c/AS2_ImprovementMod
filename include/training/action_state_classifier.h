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

// Where a recovery observation came from. Only NativePreCommand is a production
// source; the rest exist to compare against it in the logs.
enum class ActionabilitySource : uint8_t {
    LegacyActionId,
    CandidateNativeFlag,
    HybridValidated,
    // Action-ID range rule. Retained for audit comparison only: the engine has
    // attack handlers below action 85 (CharAction_0x2A..0x2E are 42..46), so an
    // ID range cannot decide whether a fighter is committed.
    StateClass,
    // Command-route vector observed at Entity_ProcessCommandMatches entry.
    NativePreCommand,
};

// True when the action ID is one of the SHARED normal/command/super actions the
// route handlers queue: 85..98 (stand / crouch / air normals and command
// normals) and 137..140 (supers).
//
// This is deliberately narrow. It is NOT "is this fighter attacking": character
// specials live in the 34..84 range alongside the reaction and movement states,
// so no ID range separates the two. Use the command-route vector
// (Training::NeutralRouteOpen) for commitment.
bool IsSharedAttackAction(uint32_t actionId);

// Audit-only heuristic: not a forced lock, not a shared attack action, not a
// context-excluded landing. It cannot see character specials below action 85,
// which is exactly why it is no longer a production recovery source.

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

bool IsStateClassActionable(uint32_t actionId, bool landingExcluded);

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
