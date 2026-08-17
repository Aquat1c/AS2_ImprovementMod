/**
 * Alice Senki 2 - Online Rollback Stress Hooks
 *
 * Developer-side test hooks for verification and debugging:
 *   - Artificial latency injection
 *   - Jitter simulation
 *   - Packet reordering
 *   - Packet drop simulation
 *   - Delayed remote input delivery
 *   - Forced rollback mismatch tests
 *
 * All hooks are OFF by default and log activation/parameter changes.
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// ============================================================================
// Lifecycle
// ============================================================================

void StressHooks_Init();
void StressHooks_Shutdown();

// ============================================================================
// Artificial Latency
// ============================================================================

/// Add artificial one-way latency to outgoing gameplay packets (ms).
void StressHooks_SetAddedLatencyMs(int ms);
int  StressHooks_GetAddedLatencyMs();

// ============================================================================
// Jitter
// ============================================================================

/// Add random jitter range (±ms) to outgoing gameplay packets.
void StressHooks_SetJitterMs(int ms);
int  StressHooks_GetJitterMs();

// ============================================================================
// Packet Drop
// ============================================================================

/// Set simulated packet drop percentage (0-100).
void StressHooks_SetDropPercent(int pct);
int  StressHooks_GetDropPercent();

// ============================================================================
// Forced Mismatch
// ============================================================================

/// Force a prediction mismatch on the next N remote input arrivals.
/// Injects a corrupted prediction to trigger rollback.
void StressHooks_ForceNextMismatches(int count);
int  StressHooks_GetRemainingForcedMismatches();

// ============================================================================
// Forced Rollback Every Frame (determinism verification)
// ============================================================================

/// Force a rollback transaction of depth N on EVERY advanced frame
/// (engine2 SetForcedRollback passthrough; QOH99 selftest model:
/// save → tick → restore → replay N → compare). 0 = off. Deterministic —
/// the replay re-consumes the same sealed inputs, so a clean sim reproduces
/// identical state; any divergence surfaces as a SyncHash desync.
void StressHooks_SetForcedRollbackDepth(int depth);
int  StressHooks_GetForcedRollbackDepth();

/// Path the arming came from, or nullptr when no as2_stress.cfg was found.
const char* StressHooks_GetConfigSource();

// ============================================================================
// Delayed Input Delivery
// ============================================================================

/// Hold all received remote inputs for N extra frames before delivering.
void StressHooks_SetInputDeliveryDelay(int frames);
int  StressHooks_GetInputDeliveryDelay();

// ============================================================================
// Query — should packet be dropped/delayed?
// ============================================================================

/// Call before sending a gameplay packet.
/// Returns true if the packet should be dropped (simulated loss).
bool StressHooks_ShouldDropPacket();

/// Returns the total delay to add to outgoing packets (latency + jitter), in ms.
int  StressHooks_GetOutgoingDelayMs();

/// Call when a remote input arrives. Returns the frame to actually deliver it
/// (current_frame + delivery_delay). If delivery_delay == 0, returns the input frame directly.
int32_t StressHooks_AdjustDeliveryFrame(int32_t input_frame, int32_t current_frame);

/// Call when predicting remote input: if forced mismatch is active,
/// returns a corrupted input. Otherwise returns the prediction unchanged.
uint16_t StressHooks_MaybeCorruptPrediction(uint16_t predicted);

/// master enable/disable
void StressHooks_SetEnabled(bool enabled);
bool StressHooks_IsEnabled();

// ============================================================================
// Diagnostics
// ============================================================================

struct StressHooksSnapshot {
    bool     enabled;
    int      added_latency_ms;
    int      jitter_ms;
    int      drop_percent;
    int      input_delivery_delay;
    int      forced_mismatches_remaining;
    int      forced_rollback_depth;
    int      total_packets_dropped;
    int      total_packets_delayed;
    int      total_mismatches_forced;
};

void StressHooks_GetSnapshot(StressHooksSnapshot* out);

} // namespace Rollback
