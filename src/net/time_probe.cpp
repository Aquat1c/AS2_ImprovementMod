/**
 * Alice Senki 2 - time_probe implementation (re0.7 M6, plan §2.9.3)
 */

#include "net/time_probe.h"

#include "net/protocol.h"
#include "net/session_manager.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <algorithm>
#include <math.h>
#include <string.h>
#include <windows.h>

namespace Net {

namespace {

bool     s_initialized = false;
uint32_t s_generation = 1;
DWORD    s_lastSendTickMs = 0;

uint32_t s_samplesUs[kTimeProbeWindow] = {};
uint32_t s_sampleCount = 0;
uint32_t s_sampleHead = 0;

// Latch state (§2.9.3).
int      s_latchedDelay = -1;
uint64_t s_lastRiseUs = 0;
uint64_t s_lastDecayUs = 0;

uint64_t QpcUs() {
    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ll) / s_freq.QuadPart);
}

void PushSample(uint32_t rtt_us) {
    s_samplesUs[s_sampleHead] = rtt_us;
    s_sampleHead = (s_sampleHead + 1) % kTimeProbeWindow;
    if (s_sampleCount < kTimeProbeWindow) {
        s_sampleCount++;
    }
}

uint32_t Percentile(uint32_t permille) {
    if (s_sampleCount == 0) {
        return 0;
    }
    uint32_t sorted[kTimeProbeWindow];
    memcpy(sorted, s_samplesUs, sizeof(uint32_t) * s_sampleCount);
    std::sort(sorted, sorted + s_sampleCount);
    uint32_t idx = (uint32_t)(((uint64_t)(s_sampleCount - 1) * permille) / 1000);
    return sorted[idx];
}

} // namespace

void TimeProbe_Init() {
    s_initialized = true;
    TimeProbe_ResetSession("init");
}

void TimeProbe_Shutdown() {
    s_initialized = false;
}

void TimeProbe_ResetSession(const char* reason) {
    s_generation++;
    s_sampleCount = 0;
    s_sampleHead = 0;
    s_lastSendTickMs = 0;
    s_latchedDelay = -1;
    s_lastRiseUs = 0;
    s_lastDecayUs = 0;
    Rollback::NetplayLog_Verbose("PROBE", -1,
        "time_probe reset: generation=%u reason=%s",
        s_generation, reason ? reason : "?");
}

void TimeProbe_FrameUpdate() {
    if (!s_initialized || !Session_IsConnected()) {
        return;
    }
    const DWORD now = GetTickCount();
    if (s_lastSendTickMs != 0 && (DWORD)(now - s_lastSendTickMs) < kTimeProbeSendIntervalMs) {
        return;
    }
    s_lastSendTickMs = now;

    TimeProbePayload p{};
    p.generation = s_generation;
    p.stamp_us = QpcUs();
    Session_SendPacket(CHANNEL_GAMEPLAY, PacketType::TimeProbe,
                       &p, sizeof(p), false);
}

void TimeProbe_OnProbe(const void* payload, size_t len) {
    if (!s_initialized || !payload || len < sizeof(TimeProbePayload)) {
        return;
    }
    TimeProbePayload req{};
    memcpy(&req, payload, sizeof(req));

    const uint64_t arrival = QpcUs();
    TimeProbeAckPayload ack{};
    ack.generation = req.generation;    // requester's generation, echoed
    ack.stamp_us = req.stamp_us;        // echoed verbatim
    // Responder dwell: time between arrival and the ack build. Send happens
    // immediately after, so the residual dwell is sub-µs.
    ack.dwell_us = (uint32_t)(QpcUs() - arrival);
    Session_SendPacket(CHANNEL_GAMEPLAY, PacketType::TimeProbeAck,
                       &ack, sizeof(ack), false);
}

void TimeProbe_OnProbeAck(const void* payload, size_t len) {
    if (!s_initialized || !payload || len < sizeof(TimeProbeAckPayload)) {
        return;
    }
    TimeProbeAckPayload ack{};
    memcpy(&ack, payload, sizeof(ack));

    if (ack.generation != s_generation) {
        return;   // generation gate: stale reply cannot repopulate (§2.9.3)
    }
    const uint64_t now = QpcUs();
    if (ack.stamp_us > now) {
        return;   // clock anomaly — drop
    }
    uint64_t rtt = now - ack.stamp_us;
    if (ack.dwell_us < rtt) {
        rtt -= ack.dwell_us;
    }
    if (rtt > 2000000ull) {
        return;   // >2 s: not a link property
    }

    // Local-stall rejection: an RTT far above the rolling median is a local
    // scheduler stall (load screen, debugger), not a link change.
    if (s_sampleCount >= 4) {
        const uint32_t median = Percentile(500);
        if (median > 0 && rtt > (uint64_t)median * 4ull && rtt > 50000ull) {
            Rollback::NetplayLog_Verbose("PROBE", -1,
                "time_probe sample rejected as local stall: rtt_us=%llu median_us=%u",
                (unsigned long long)rtt, median);
            return;
        }
    }

    PushSample((uint32_t)rtt);
}

bool TimeProbe_HasMeasurement() {
    return s_sampleCount >= kTimeProbeMinSamples;
}

uint32_t TimeProbe_GetSampleCount() {
    return s_sampleCount;
}

uint32_t TimeProbe_GetRttP50Us() {
    return Percentile(500);
}

uint32_t TimeProbe_GetRttP95Us() {
    return Percentile(950);
}

uint32_t TimeProbe_GetJitterP95Us() {
    const uint32_t p95 = Percentile(950);
    const uint32_t p50 = Percentile(500);
    return p95 > p50 ? p95 - p50 : 0;
}

float TimeProbe_GetOneWayFramesP95(uint32_t frame_period_us) {
    if (s_sampleCount == 0 || frame_period_us == 0) {
        return 0.0f;
    }
    const float oneway_us = (float)Percentile(950) * 0.5f;
    return oneway_us / (float)frame_period_us;
}

int TimeProbe_GetLatchedFrontendDelay(int configured_floor, int fallback) {
    if (configured_floor < 0) configured_floor = 0;

    if (!TimeProbe_HasMeasurement()) {
        // Hold the baseline until >= 12 samples exist (§2.9.3).
        return s_latchedDelay >= 0 ? s_latchedDelay
                                   : (fallback > configured_floor ? fallback : configured_floor);
    }

    const float oneway = TimeProbe_GetOneWayFramesP95(16667u);
    int target = (int)ceilf(oneway) + 1;
    if (oneway >= 6.0f) {
        target += 1;   // +2 total once oneway >= 6 frames
    }
    if (target < configured_floor) target = configured_floor;
    if (target > 15) target = 15;

    const uint64_t now = QpcUs();
    if (s_latchedDelay < 0) {
        s_latchedDelay = target;
        s_lastRiseUs = now;
        s_lastDecayUs = now;
        return s_latchedDelay;
    }

    if (target > s_latchedDelay) {
        if (now - s_lastRiseUs >= 150000ull) {     // 1 frame per 0.15 s
            s_latchedDelay++;
            s_lastRiseUs = now;
            s_lastDecayUs = now;
        }
    } else if (target < s_latchedDelay) {
        if (now - s_lastDecayUs >= 300000ull) {    // 1 frame per 0.30 s
            s_latchedDelay--;
            s_lastDecayUs = now;
        }
    } else {
        s_lastRiseUs = now;
        s_lastDecayUs = now;
    }

    if (s_latchedDelay < configured_floor) s_latchedDelay = configured_floor;
    if (s_latchedDelay > 15) s_latchedDelay = 15;
    return s_latchedDelay;
}

} // namespace Net
