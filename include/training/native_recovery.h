#pragma once

/**
 * Alice Senki 2 - Native free-recovery detection (pure logic).
 *
 * Answers one question: on which simulation tick did a fighter finish the
 * committed attack, hitstun, blockstun or landing and regain unrestricted
 * neutral control?
 *
 * A cancel window is NOT recovery. A fighter still committed to an attack that
 * happens to be cancellable has not recovered, so the predicate needs two
 * things together:
 *
 *   1. a terminal neutral handoff is pending (the action script finished and
 *      queued stand / crouch / air neutral), and
 *   2. the ordinary neutral route for the fighter's current posture is open.
 *
 * Route openings without a pending handoff are cancel or buffer evidence and
 * are reported as telemetry only.
 *
 * Field meanings verified against the decomp (see docs/re0.7/NATIVE_RECOVERY.md):
 *   +0x0674..+0x068B  24 command-route timers, scanned 23..0 by 0x4BEA20
 *   +0x06C4           airborne flag (1 = airborne)
 *   +0x000A           DOWN input word (crouch), NOT a ground/air lane
 *   +0x0444/+0x0448   pending action slots
 *   +0x00CF           Training Life/Spirit restore gate, audit only
 */

#include <stddef.h>
#include <stdint.h>

namespace Training {

constexpr int kCommandRouteCount = 24;

// Shared A/B/C handlers. 2/3/4 cover airborne and standing-grounded; 5/6/7
// cover crouching-grounded. Posture picks the branch, not the route index.
constexpr int kRouteStandOrAirA = 2;
constexpr int kRouteStandOrAirB = 3;
constexpr int kRouteStandOrAirC = 4;
constexpr int kRouteCrouchA = 5;
constexpr int kRouteCrouchB = 6;
constexpr int kRouteCrouchC = 7;

constexpr uint32_t kTargetStandNeutral = 2;
constexpr uint32_t kTargetCrouchNeutral = 7;
constexpr uint32_t kTargetAirNeutral = 22;
constexpr uint32_t kTargetLanding = 23;

enum class NativeRecoveryKind : uint8_t {
    Locked = 0,
    FreeGround,
    FreeAir,
    LandingUnverified,
    StableFreeFallback,
    CpuControlledUnsupported,
};

struct NativeRecoverySample {
    uint32_t currentAction = 0;
    uint32_t pendingAction1 = 0;
    uint32_t pendingAction2 = 0;

    uint8_t airborne = 0;            // +0x06C4: 1 = airborne
    uint16_t downHeld = 0;           // +0x000A: DOWN input word
    uint8_t cpuControlled = 0;       // charData + 172
    uint8_t trainingRestoreGate = 0; // +0x00CF, audit only

    uint8_t routes[kCommandRouteCount] = {};
};

struct NativeRecoveryResult {
    NativeRecoveryKind kind = NativeRecoveryKind::Locked;
    bool freeRecovery = false;
    bool routeOpenWithoutFreeHandoff = false;

    // Which shared branches the engine would consider this tick.
    bool standOrAirA = false;
    bool standOrAirB = false;
    bool standOrAirC = false;
    bool crouchA = false;
    bool crouchB = false;
    bool crouchC = false;

    uint32_t pendingTarget = 0;
    const char* reason = "locked";
};

// Slot 1 wins when both are set; the native setters normally clear the other.
uint32_t PendingTarget(const NativeRecoverySample& sample);
bool BothPendingSlotsSet(const NativeRecoverySample& sample);

bool IsGroundFreeTarget(uint32_t target);
bool IsAirFreeTarget(uint32_t target);
bool IsTerminalNeutralTarget(uint32_t target);

// The authoritative per-tick evaluation.
NativeRecoveryResult EvaluateNativeRecovery(const NativeRecoverySample& sample);

// Used only when an interaction begins after a fighter has already recovered,
// or after a state restore leaves no handoff to observe.
bool IsStableFreeFallback(const NativeRecoverySample& sample);

const char* NativeRecoveryKindLabel(NativeRecoveryKind kind);

// True when the engine would accept an ordinary neutral attack input right now
// for the fighter's current posture. This is the honest replacement for the
// action-ID "is this an attack move" test: attacks below action 85 exist, so
// the ID range cannot decide commitment, but a committed action script has
// zeroed its normal routes.
bool NeutralRouteOpen(const NativeRecoverySample& sample);

} // namespace Training
