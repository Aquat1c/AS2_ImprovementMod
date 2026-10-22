#include "training/native_recovery.h"

namespace Training {
namespace {

bool RouteOpen(const NativeRecoverySample& s, int route) {
    if (route < 0 || route >= kCommandRouteCount) {
        return false;
    }
    return s.routes[route] != 0;
}

} // namespace

uint32_t PendingTarget(const NativeRecoverySample& sample) {
    if (sample.pendingAction1 != 0) {
        return sample.pendingAction1;
    }
    return sample.pendingAction2;
}

bool BothPendingSlotsSet(const NativeRecoverySample& sample) {
    return sample.pendingAction1 != 0 && sample.pendingAction2 != 0;
}

bool IsGroundFreeTarget(uint32_t target) {
    return target == kTargetStandNeutral || target == kTargetCrouchNeutral;
}

bool IsAirFreeTarget(uint32_t target) {
    return target == kTargetAirNeutral;
}

bool IsTerminalNeutralTarget(uint32_t target) {
    return IsGroundFreeTarget(target) || IsAirFreeTarget(target);
}

bool NeutralRouteOpen(const NativeRecoverySample& sample) {
    if (sample.airborne == 1) {
        // Airborne normals only ever come from the 2/3/4 branch.
        return RouteOpen(sample, kRouteStandOrAirA);
    }
    // Grounded: route 2 is the standing branch, route 5 the crouching one.
    // Either being open means an ordinary attack input would be considered.
    return RouteOpen(sample, kRouteStandOrAirA) || RouteOpen(sample, kRouteCrouchA);
}

NativeRecoveryResult EvaluateNativeRecovery(const NativeRecoverySample& s) {
    NativeRecoveryResult out{};
    out.pendingTarget = PendingTarget(s);

    const bool airborne = s.airborne == 1;
    out.standOrAirA = RouteOpen(s, kRouteStandOrAirA);
    out.standOrAirB = RouteOpen(s, kRouteStandOrAirB);
    out.standOrAirC = RouteOpen(s, kRouteStandOrAirC);
    out.crouchA = !airborne && RouteOpen(s, kRouteCrouchA);
    out.crouchB = !airborne && RouteOpen(s, kRouteCrouchB);
    out.crouchC = !airborne && RouteOpen(s, kRouteCrouchC);

    if (s.cpuControlled != 0) {
        // The same routes select AI actions instead of player ones, so a human
        // actionability number would be a fiction here.
        out.kind = NativeRecoveryKind::CpuControlledUnsupported;
        out.reason = "cpu_controlled";
        return out;
    }

    if (!airborne && IsGroundFreeTarget(out.pendingTarget) && NeutralRouteOpen(s)) {
        out.kind = NativeRecoveryKind::FreeGround;
        out.freeRecovery = true;
        out.reason = "free_ground";
        return out;
    }

    if (airborne && IsAirFreeTarget(out.pendingTarget) && NeutralRouteOpen(s)) {
        out.kind = NativeRecoveryKind::FreeAir;
        out.freeRecovery = true;
        out.reason = "free_air";
        return out;
    }

    if (out.pendingTarget == kTargetLanding) {
        // Not automatically free and not automatically locked; needs an
        // injected-input acceptance test before it can be promoted.
        out.kind = NativeRecoveryKind::LandingUnverified;
        out.reason = "landing_unverified";
        return out;
    }

    if (NeutralRouteOpen(s)) {
        out.routeOpenWithoutFreeHandoff = true;
        out.reason = "route_open_without_terminal_neutral_handoff";

        // Already sitting in neutral with the full profile granted. Reported so
        // telemetry can tell "idle in neutral" apart from "cancel window", but
        // it is a STATE, not a recovery event, so freeRecovery stays false and
        // it never produces a timestamp.
        if (IsStableFreeFallback(s)) {
            out.kind = NativeRecoveryKind::StableFreeFallback;
            out.reason = "stable_free";
        }
    }

    return out;
}

bool IsStableFreeFallback(const NativeRecoverySample& s) {
    if (s.cpuControlled != 0) {
        return false;
    }

    if (s.airborne != 1 &&
        (s.currentAction == kTargetStandNeutral || s.currentAction == kTargetCrouchNeutral) &&
        RouteOpen(s, kRouteStandOrAirA) &&
        RouteOpen(s, kRouteStandOrAirB) &&
        RouteOpen(s, kRouteCrouchA)) {
        return true;
    }

    if (s.airborne == 1 &&
        s.currentAction == kTargetAirNeutral &&
        RouteOpen(s, kRouteStandOrAirA) &&
        RouteOpen(s, kRouteStandOrAirB)) {
        return true;
    }

    return false;
}

const char* NativeRecoveryKindLabel(NativeRecoveryKind kind) {
    switch (kind) {
        case NativeRecoveryKind::FreeGround:               return "free_ground";
        case NativeRecoveryKind::FreeAir:                  return "free_air";
        case NativeRecoveryKind::LandingUnverified:        return "landing_unverified";
        case NativeRecoveryKind::StableFreeFallback:       return "stable_free_fallback";
        case NativeRecoveryKind::CpuControlledUnsupported: return "cpu_controlled";
        case NativeRecoveryKind::Locked:
        default:                                           return "locked";
    }
}

} // namespace Training
