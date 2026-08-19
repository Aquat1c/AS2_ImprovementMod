/**
 * Frame-advantage arithmetic, split out from the state machine so the rules
 * that decide what a number means can be tested without game memory.
 *
 * The state machine's job is to find three timestamps — contact, attacker
 * recovery, defender recovery. What those timestamps are allowed to mean is
 * here.
 */

#pragma once

#include <stdint.h>

namespace Training {

constexpr uint32_t kFrameAdvantageUnset = UINT32_MAX;

/// The attacker's recovery frame as it applies to this exchange.
///
/// A timestamp that predates contact belongs to a lingering hitbox — a
/// projectile, or a pending attack promoted long after the move itself ended.
/// Subtracting it reports the projectile's whole flight time as advantage. The
/// attacker was already actionable when the hit landed, so contact is the
/// earliest frame their recovery can mean anything here.
inline uint32_t EffectiveAttackerRecovery(uint32_t attackerRecover, uint32_t contact) {
    if (attackerRecover == kFrameAdvantageUnset || contact == kFrameAdvantageUnset) {
        return attackerRecover;
    }
    return (attackerRecover < contact) ? contact : attackerRecover;
}

/// Positive = attacker recovers first.
inline int32_t ComputeFrameAdvantage(uint32_t attackerRecover,
                                     uint32_t defenderRecover,
                                     uint32_t contact) {
    const uint32_t effective = EffectiveAttackerRecovery(attackerRecover, contact);
    return (int32_t)defenderRecover - (int32_t)effective;
}

/// Frames the defender was actionable before this contact, or 0 for "no gap
/// worth reporting". Anything past maxFrames is a fresh engagement, not a hole
/// in a blockstring.
inline uint32_t GapBeforeContact(uint32_t defenderFreeFrame,
                                 uint32_t contact,
                                 uint32_t maxFrames) {
    if (defenderFreeFrame == kFrameAdvantageUnset ||
        contact == kFrameAdvantageUnset ||
        contact <= defenderFreeFrame) {
        return 0;
    }
    const uint32_t gap = contact - defenderFreeFrame;
    return (gap > maxFrames) ? 0 : gap;
}

/// The ordering rule: may a measured gap take the readout slot?
///
/// A suppression, not a deferral. A gap is always discovered after the previous
/// exchange's advantage has been published, so showing it then reads as a
/// correction to that number and appending it puts two measurements of
/// different things on one line. If a number is up, the gap is dropped — it is
/// only worth anything in the moment, and the moment is taken.
///
/// The case this leaves through is the one that matters: when no number is up,
/// the previous exchange never completed (the attacker had not recovered when
/// the defender did), so the gap is the only readout that describes it.
inline bool GapShouldPublish(uint32_t gapFrames, bool resultReadoutLive) {
    return gapFrames != 0 && !resultReadoutLive;
}

} // namespace Training
