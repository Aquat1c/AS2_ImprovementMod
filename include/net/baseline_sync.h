/**
 * Alice Senki 2 - Baseline Sync Diagnostics
 *
 * Isolates baseline CRC breakdown capture/compare/dump logic from
 * match_bootstrap so baseline mismatch analysis is easier to evolve.
 */

#pragma once

#include "net/protocol.h"
#include <stdint.h>

namespace Net {

struct BaselineSyncStateView {
    const char* phase_name;

    bool local_loaded;
    bool remote_loaded;
    bool local_ready;
    bool remote_ready;
    bool digest_sent;
    bool baseline_agreed;

    uint8_t  local_mode;
    uint8_t  local_substate;
    int32_t  local_sim_frame;
    uint8_t  remote_mode;
    uint8_t  remote_substate;
    int32_t  remote_sim_frame;

    uint32_t local_crc;
    uint32_t remote_crc;
    uint32_t session_seed;
};

void BaselineSync_Reset();
void BaselineSync_LogBegin(const BaselineSyncStateView& state);

void BaselineSync_CaptureLocalBreakdown(BaselineBreakdownPayload* out);
void BaselineSync_RecordLocalBreakdown(const BaselineSyncStateView& state,
                                       const BaselineBreakdownPayload& payload,
                                       uint32_t reference_crc);
void BaselineSync_RecordRemoteBreakdown(const BaselineSyncStateView& state,
                                        const BaselineBreakdownPayload& payload);
void BaselineSync_LogRemoteDigest(const BaselineSyncStateView& state,
                                  uint32_t remote_crc);
void BaselineSync_LogMismatchAndDump(const BaselineSyncStateView& state);

bool BaselineSync_GetLocalBreakdown(BaselineBreakdownPayload* out);
bool BaselineSync_GetRemoteBreakdown(BaselineBreakdownPayload* out);

} // namespace Net

