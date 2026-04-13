/**
 * Alice Senki 2 - StageSync Watchdog Tracker
 *
 * Ordered, idempotent acceptance helper for StageSync watchdog packets.
 * StageSync is secondary/watchdog data only, so older or weaker packets
 * must never overwrite newer watchdog truth.
 */

#pragma once

#include "net/protocol.h"

namespace Net {

enum class StageWatchdogApplyResult : uint8_t {
    AcceptedNewer = 0,
    AcceptedSameFrameUpgrade,
    Duplicate,
    IgnoredOutOfOrder,
    IgnoredRegression,
};

inline const char* StageWatchdogApplyResultName(StageWatchdogApplyResult result) {
    switch (result) {
        case StageWatchdogApplyResult::AcceptedNewer: return "AcceptedNewer";
        case StageWatchdogApplyResult::AcceptedSameFrameUpgrade: return "AcceptedSameFrameUpgrade";
        case StageWatchdogApplyResult::Duplicate: return "Duplicate";
        case StageWatchdogApplyResult::IgnoredOutOfOrder: return "IgnoredOutOfOrder";
        case StageWatchdogApplyResult::IgnoredRegression: return "IgnoredRegression";
        default: return "Unknown";
    }
}

struct StageWatchdogTrackerState {
    bool             has_packet;
    StageSyncPayload latest;
};

void StageWatchdogTracker_Reset(StageWatchdogTrackerState* state);
StageWatchdogApplyResult StageWatchdogTracker_Apply(StageWatchdogTrackerState* state,
                                                    const StageSyncPayload* payload);

} // namespace Net