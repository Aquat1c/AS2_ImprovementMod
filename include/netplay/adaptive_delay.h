/**
 * Alice Senki 2 - Adaptive Delay
 *
 * Mid-match delay adjustment via coordinated negotiation protocol.
 *
 * How it works:
 *   1. Tracks rollback rate over a rolling window
 *   2. When rate exceeds threshold, initiates a delay change request
 *   3. Both sides negotiate via DelayChangeRequest / DelayChangeAck
 *   4. At the agreed effective_frame, both sides apply the new delay
 *
 * The protocol ensures both sides change delay at the same frame,
 * preventing desync or input drops.
 */

#pragma once

#include "packet_codec.h"
#include <stdint.h>

namespace AdaptiveDelay {

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    int      min_delay;           // Minimum delay (default 0)
    int      max_delay;           // Maximum delay (default 6)
    int      window_size;         // Rolling window in frames (default 180 = 3s)
    float    threshold_up;        // Rollback rate to trigger increase (default 0.30)
    float    threshold_down;      // Rollback rate to trigger decrease (default 0.05)
    int      cooldown_frames;     // Frames between changes (default 300 = 5s)
    int      lead_frames;         // Frames ahead for effective_frame (default 30)
    bool     enabled;             // Master enable (default true)
};

// ============================================================================
// State
// ============================================================================

enum class NegotiationState : uint32_t {
    Idle = 0,             // No pending change
    RequestSent,          // We sent a request, waiting for ack
    RequestReceived,      // We received a request, sent ack, waiting for frame
    Agreed,               // Both sides agreed, waiting for effective_frame
};

struct Snapshot {
    NegotiationState negotiation_state;
    int      current_delay;
    int      pending_delay;       // Negotiated delay (valid when state != Idle)
    int      effective_frame;     // Frame at which change applies
    float    rollback_rate;       // Current rollback rate [0.0, 1.0]
    int      cooldown_remaining;  // Frames until next change allowed
    uint32_t changes_applied;     // Total delay changes this session
    bool     enabled;
};

// ============================================================================
// Lifecycle
// ============================================================================

void Init(const Config* config, int initialDelay);
void Shutdown();

// ============================================================================
// Per-Frame Update
// ============================================================================

// Call once per non-rollback frame from the rollback dispatch path.
// currentFrame: the GekkoNet local frame
// wasRollback: true if this frame was preceded by a rollback (load event)
void OnFrame(int currentFrame, bool wasRollback);

// ============================================================================
// Manual Trigger (from hotkeys)
// ============================================================================

// Request a delay change from hotkey press.
// direction: +1 to increase, -1 to decrease.
// Returns true if a request was initiated.
bool RequestManualChange(int direction);

// ============================================================================
// Packet Handlers (called from SessionManager dispatch)
// ============================================================================

void OnReceiveDelayChangeRequest(const PacketCodec::DelayChangeRequestPayload* payload);
void OnReceiveDelayChangeAck(const PacketCodec::DelayChangeAckPayload* payload);

// ============================================================================
// Network Context (must be set before use)
// ============================================================================

void SetNetworkContext(uint64_t sessionId, uint32_t connectionId);

// ============================================================================
// Queries
// ============================================================================

bool IsActive();
bool GetSnapshot(Snapshot* out);
int  GetCurrentDelay();
float GetRollbackRate();
uint32_t GetChangesApplied();

// Update the RTT-based delay floor.  Called whenever a new RTT sample arrives.
// Prevents delay from decreasing below the one-way trip in frames.
void UpdateRttFloor(float rttMs);

} // namespace AdaptiveDelay
