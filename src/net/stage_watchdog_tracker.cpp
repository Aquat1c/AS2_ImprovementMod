#include "net/stage_watchdog_tracker.h"

#include <string.h>

namespace {

static bool HasCommittedStage(const Net::StageSyncPayload& payload) {
    return payload.committed_stage_id != 0;
}

static bool WouldRegress(const Net::StageSyncPayload& current,
                         const Net::StageSyncPayload& next) {
    if (current.confirmed && !next.confirmed) {
        return true;
    }
    if (HasCommittedStage(current) && !HasCommittedStage(next)) {
        return true;
    }
    return false;
}

static bool IsSameFrameUpgrade(const Net::StageSyncPayload& current,
                               const Net::StageSyncPayload& next) {
    if (next.confirmed != current.confirmed) {
        return next.confirmed > current.confirmed;
    }
    if (HasCommittedStage(next) != HasCommittedStage(current)) {
        return HasCommittedStage(next) && !HasCommittedStage(current);
    }
    return false;
}

} // namespace

namespace Net {

void StageWatchdogTracker_Reset(StageWatchdogTrackerState* state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

StageWatchdogApplyResult StageWatchdogTracker_Apply(StageWatchdogTrackerState* state,
                                                    const StageSyncPayload* payload) {
    if (!state || !payload) {
        return StageWatchdogApplyResult::IgnoredRegression;
    }

    if (!state->has_packet) {
        state->has_packet = true;
        state->latest = *payload;
        return StageWatchdogApplyResult::AcceptedNewer;
    }

    if (WouldRegress(state->latest, *payload)) {
        return StageWatchdogApplyResult::IgnoredRegression;
    }

    if (payload->frame < state->latest.frame) {
        return StageWatchdogApplyResult::IgnoredOutOfOrder;
    }

    if (payload->frame == state->latest.frame) {
        if (memcmp(&state->latest, payload, sizeof(*payload)) == 0) {
            return StageWatchdogApplyResult::Duplicate;
        }
        if (!IsSameFrameUpgrade(state->latest, *payload)) {
            return StageWatchdogApplyResult::IgnoredRegression;
        }

        state->latest = *payload;
        return StageWatchdogApplyResult::AcceptedSameFrameUpgrade;
    }

    state->latest = *payload;
    return StageWatchdogApplyResult::AcceptedNewer;
}

} // namespace Net