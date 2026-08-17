/**
 * Alice Senki 2 - time_probe (re0.7 M6, plan §2.9.3)
 *
 * µs-resolution RTT estimator over TimeProbe/TimeProbeAck (77/78, unreliable
 * channel 1, 4 Hz). QOH99 lesson 7: tuning must never be fed from
 * ms-quantized transport clocks or menu/lobby measurements — this module is
 * the ONLY sanctioned source for the frontend-delay latch and the coverage
 * math (§2.9.1).
 *
 *   - Opaque QPC µs stamp echoed with responder dwell subtracted.
 *   - Generation-gated: a stale reply cannot repopulate a reset estimator.
 *   - Rolling p95 over 32 samples with local-stall rejection (a sample whose
 *     RTT exceeds 4x the rolling median is attributed to a local scheduler
 *     stall and dropped).
 *   - Unit-suffixed names (`*_us`), conversions at the caller (lesson 8).
 *
 * Driven from session2's Session_Update (send cadence + reset on session
 * boundaries); packets routed by packet_router.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace Net {

void TimeProbe_Init();
void TimeProbe_Shutdown();

/// Session boundary: bumps the generation (stale replies inert) and clears
/// the sample window.
void TimeProbe_ResetSession(const char* reason);

/// Per-frame driver (game thread, while a peer is connected): sends the 4 Hz
/// probe when due.
void TimeProbe_FrameUpdate();

/// Packet entry points (routed by packet_router).
void TimeProbe_OnProbe(const void* payload, size_t len);     // responder side
void TimeProbe_OnProbeAck(const void* payload, size_t len);  // estimator side

// ── Queries ─────────────────────────────────────────────────────────────────

/// True once >= kTimeProbeMinSamples samples exist in the window.
bool TimeProbe_HasMeasurement();
uint32_t TimeProbe_GetSampleCount();

/// Rolling statistics over the 32-sample window (0 when empty).
uint32_t TimeProbe_GetRttP50Us();
uint32_t TimeProbe_GetRttP95Us();
uint32_t TimeProbe_GetJitterP95Us();   // p95 − p50 spread

/// One-way p95 in frames at the given period (§2.9.1):
/// ceil(p95_oneway_us / period_us). 0.0f when no measurement exists.
float TimeProbe_GetOneWayFramesP95(uint32_t frame_period_us);

/// §2.9.3 latch: frontend_delay = ceil(oneway_frames) + 1 (+2 once oneway
/// >= 6 frames), floored at `configured_floor`, capped 15. Holds `fallback`
/// until >= 12 samples exist; rises max 1 frame per 0.15 s; decays max
/// 1 frame per 0.30 s (asymmetric toward the cheap mistake, lesson 9).
int TimeProbe_GetLatchedFrontendDelay(int configured_floor, int fallback);

constexpr uint32_t kTimeProbeWindow = 32;
constexpr uint32_t kTimeProbeMinSamples = 12;
constexpr uint32_t kTimeProbeSendIntervalMs = 250;   // 4 Hz

} // namespace Net
