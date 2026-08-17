/**
 * Alice Senki 2 - Rollback Timesync Telemetry (type-only header)
 *
 * Extracted from rollback_session.h (re0.7 M0) so type-only consumers
 * (netplay_pacing, churn_pause) do not depend on the engine facade header.
 * Link-stat fields are engine-neutral: `link_avg_ping`/`link_jitter` describe
 * the transport link, whatever backend produces them.
 */

#pragma once

#include <stdint.h>

namespace Rollback {

struct RollbackTimesyncTelemetry {
    int32_t  rb_frame_current;
    int32_t  rb_frame_last_confirmed;
    int32_t  rb_frame_last_remote_received;
    int32_t  rb_frame_remote_contiguous;
    int32_t  raw_remote_gap;
    int32_t  effective_remote_delay;
    int32_t  prediction_debt;
    int32_t  rollback_budget;
    int32_t  game_abs_frame_current;
    int32_t  frame_origin_abs;
    int32_t  rollback_count;
    int32_t  last_rollback_replay_length;
    int32_t  max_rollback_distance;
    int32_t  predicted_frames_outstanding;
    float    frames_ahead;
    float    link_avg_ping;
    float    link_jitter;
    float    rtt_last_ms;
    float    rtt_avg_ms;
    float    rtt_p90_ms;
    float    rtt_p95_ms;
    float    jitter_avg_ms;
    float    jitter_p95_ms;
    float    packet_loss_ewma;
    int32_t  loss_burst_max;
};

} // namespace Rollback
