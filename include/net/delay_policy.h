/**
 * Alice Senki 2 - Delay Policy
 *
 * Manages input delay computation, negotiation, and lifecycle.
 *
 * The mod owns ALL delay and rollback-budget decisions. The rollback session
 * (RollbackSession_FrameUpdate) consumes the active delay directly.
 *
 * The gameplay bridge (gameplay_bridge) calls IsRollbackSynced() and
 * OnRollbackApplied() to mark delay as consumed by the rollback session.
 *
 * === Delay value taxonomy ===
 *
 *   configured_delay   — user's persistent preference (from config/UI)
 *   recommended_delay  — computed at connect time from measured RTT + jitter
 *   agreed_delay       — negotiated startup value both peers accept
 *   active_delay       — the delay the rollback session is currently using
 *   pending_next_delay — committed for next match/round, not yet active
 *
 * === Delay change state machine ===
 *
 *   Requested → Pending → Committed → Applied
 *
 *   - Requested: one peer wants a change
 *   - Pending:   both peers acknowledge, waiting for safe boundary
 *   - Committed: safe boundary reached, ready to apply
 *   - Applied:   rollback session has consumed the new value
 *
 * === Safe boundaries (where commit/apply may occur) ===
 *
 *   - Round start (Mode 8 Sub 2 → Sub 3 edge)
 *   - Post-round transition (transition byte 0→1 edge)
 *   - Post-match / win screen (Mode 8 Sub 5, or Mode 9)
 *   - Pause (Mode 8 Sub 4, both peers halted)
 *   - CharSel/front-end lockstep (session alive, no rollback running)
 *
 *   In lockstep/front-end states, committed changes apply to the
 *   NEXT rollback match, not the current (non-existent) one.
 *
 * === Rollback budget ===
 *
 *   Rollback budget is an independent policy, NOT a derived formula.
 *   Both delay and rollback budget are exchanged and agreed explicitly.
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Constants
// ============================================================================

constexpr int DELAY_MIN             = 0;    // Absolute minimum delay
constexpr int DELAY_MAX             = 15;   // Absolute maximum delay
constexpr int DELAY_DEFAULT_PREF    = 0;    // Default user preference (auto)
constexpr int ROLLBACK_BUDGET_MIN   = 2;    // Never allow fewer than 2 rollback frames
constexpr int ROLLBACK_BUDGET_MAX   = 10;   // Hard cap on rollback frames
constexpr int ROLLBACK_BUDGET_DEFAULT = 7;  // Default rollback budget
constexpr float FRAME_TIME_MS       = 16.6667f;  // 60 FPS frame time in ms

// ============================================================================
// Delay Change State Machine
// ============================================================================

enum class DelayChangeState : uint8_t {
    Idle = 0,         // No change in progress
    Requested,        // Change requested by one peer
    Pending,          // Both peers acknowledged, awaiting safe boundary
    Committed,        // Safe boundary reached, ready to apply
    Applied,          // Rollback session has consumed this value
};

inline const char* DelayChangeStateName(DelayChangeState s) {
    switch (s) {
        case DelayChangeState::Idle:           return "Idle";
        case DelayChangeState::Requested:      return "Requested";
        case DelayChangeState::Pending:        return "Pending";
        case DelayChangeState::Committed:      return "Committed";
        case DelayChangeState::Applied:        return "Applied";
        default:                               return "Unknown";
    }
}

// ============================================================================
// Safe Boundary Classification
// ============================================================================

enum class DelaySafeBoundary : uint8_t {
    None = 0,
    RoundStart,         // Mode 8: Sub 2 → Sub 3 edge
    PostRound,          // Mode 8 Sub 3: transition byte 0→1 edge
    PostMatch,          // Mode 8 Sub 5 or Mode 9 (win screen)
    Pause,              // Mode 8 Sub 4
    FrontEndLockstep,   // CharSel/StageSel lockstep (session alive, no rollback)
};

inline const char* DelaySafeBoundaryName(DelaySafeBoundary b) {
    switch (b) {
        case DelaySafeBoundary::None:             return "None";
        case DelaySafeBoundary::RoundStart:       return "RoundStart";
        case DelaySafeBoundary::PostRound:        return "PostRound";
        case DelaySafeBoundary::PostMatch:        return "PostMatch";
        case DelaySafeBoundary::Pause:            return "Pause";
        case DelaySafeBoundary::FrontEndLockstep: return "FrontEndLockstep";
        default:                                  return "Unknown";
    }
}

// ============================================================================
// RTT Measurement Snapshot
// ============================================================================

struct NetworkMeasurement {
    float    rtt_ms;             // Smoothed RTT from ENet
    float    rtt_variance_ms;    // ENet EWMA variance
    float    jitter_ms;          // Short-term jitter (tracked locally)
    float    one_way_ms;         // Estimated one-way delay (rtt/2)
    int      recommended_delay;  // Computed from measurements
    bool     valid;              // True after first measurement
};

// ============================================================================
// Negotiation Exchange Payload (for session sync)
// ============================================================================

/// Sent during initial session sync so both peers agree on delay/rollback.
struct DelayNegotiationData {
    int      configured_delay;     // User's preference (0 = auto)
    int      recommended_delay;    // Computed from network measurement
    int      min_acceptable;       // Floor the peer will accept
    int      max_acceptable;       // Ceiling the peer will accept
    int      rollback_budget;      // Peer's rollback budget
    int      rollback_delay;       // Input pipeline delay (CCCaster-style: residual after rollback absorbs latency)
};

// ============================================================================
// Delay Policy Snapshot (read-only for UI / diagnostics)
// ============================================================================

struct DelayPolicySnapshot {
    // Value taxonomy
    int      configured_delay;      // User's persistent preference (0=auto)
    int      recommended_delay;     // Computed from RTT + jitter
    int      agreed_delay;          // Session startup negotiated value
    int      active_delay;          // Currently used by rollback session
    int      pending_next_delay;    // Committed for next match/boundary (0 = none)

    // Rollback parameters (CCCaster-style separation)
    int      rollback_budget;       // Max frames local can run ahead of confirmed remote
    int      agreed_rollback;       // Session-agreed rollback budget
    int      rollback_delay;        // Input pipeline delay when rollback is active

    // Change state machine
    DelayChangeState change_state;
    int      change_target_delay;   // Target delay of in-progress change

    // Delay sync tracking (consumed by rollback session via gameplay bridge)
    bool     rollback_synced;       // Rollback session is using active_delay
    int      rollback_current_delay;// Last value consumed by rollback session

    // Network measurement
    float    measured_rtt_ms;
    float    measured_jitter_ms;
    float    measured_one_way_ms;
    bool     measurement_valid;

    // Session stats
    uint32_t changes_applied;
    DelaySafeBoundary last_boundary;

    // Context
    bool     in_rollback_match;     // Currently in active rollback gameplay
    bool     safe_boundary_available; // A safe boundary is present right now
};

// ============================================================================
// Lifecycle
// ============================================================================

void DelayPolicy_Init();
void DelayPolicy_Shutdown();

// ============================================================================
// Per-Frame
// ============================================================================

/// Every frame: updates measurements, checks boundaries, applies commits.
void DelayPolicy_FrameUpdate();

// ============================================================================
// Network Measurement
// ============================================================================

/// Feed current RTT values (called with ENet stats each frame or on update).
void DelayPolicy_UpdateMeasurement(float rtt_ms, float rtt_variance_ms);

/// Compute recommended delay from current measurements. Returns frames.
int  DelayPolicy_ComputeRecommendedDelay();

/// Get the current measurement snapshot.
void DelayPolicy_GetMeasurement(NetworkMeasurement* out);

// ============================================================================
// User Configuration
// ============================================================================

/// Set user's preferred delay. 0 = auto (use recommended). Clamped to valid range.
void DelayPolicy_SetConfiguredDelay(int delay);
int  DelayPolicy_GetConfiguredDelay();

/// Set rollback budget preference (max frames ahead of confirmed remote).
void DelayPolicy_SetRollbackBudget(int frames);
int  DelayPolicy_GetRollbackBudget();

/// Set rollback delay (input pipeline delay when rollback is active).
/// CCCaster formula: max(0, recommended_delay - rollback_budget)
void DelayPolicy_SetRollbackDelay(int frames);
int  DelayPolicy_GetRollbackDelay();

/// Compute suggested rollback delay from current recommended delay and rollback budget.
int  DelayPolicy_ComputeSuggestedRollbackDelay();

// ============================================================================
// Session Negotiation
// ============================================================================

/// Build the negotiation data to send to the remote peer.
void DelayPolicy_BuildNegotiationData(DelayNegotiationData* out);

/// Process remote peer's negotiation data and produce agreed values.
/// Call once during initial session sync. Sets agreed_delay and starts the session.
void DelayPolicy_NegotiateSession(const DelayNegotiationData* remote);

/// Get the agreed startup delay. Valid after NegotiateSession.
int  DelayPolicy_GetAgreedDelay();

// ============================================================================
// Active Delay (rollback-session-facing)
// ============================================================================

/// The input pipeline delay the rollback session is currently running with.
/// In rollback mode, this is rollback_delay (residual after rollback absorbs latency).
/// NOT the same as agreed_delay (which is the full computed delay before rollback subtraction).
int  DelayPolicy_GetActiveDelay();

/// The agreed rollback budget (max frames ahead).
int  DelayPolicy_GetAgreedRollbackBudget();

/// The agreed rollback delay (input pipeline delay in rollback mode).
int  DelayPolicy_GetAgreedRollbackDelay();

/// True if the rollback session's delay matches the committed value.
bool DelayPolicy_IsRollbackSynced();

/// Called by the gameplay bridge AFTER the rollback session has consumed
/// the delay value. Completes the Requested→Pending→Committed→Applied cycle.
void DelayPolicy_OnRollbackApplied(int delay_value);

// ============================================================================
// Mid-Session Delay Changes
// ============================================================================

/// Request a delay change. Enters Requested state.
/// During rollback: change applies to current match at next safe boundary.
/// During lockstep/front-end: change applies to next rollback match.
void DelayPolicy_RequestChange(int new_delay);

/// Remote peer acknowledged our change request. Moves Requested→Pending.
void DelayPolicy_OnRemoteAck(int acked_delay);

/// Check current change state.
DelayChangeState DelayPolicy_GetChangeState();
int  DelayPolicy_GetChangeTarget();

/// Is a safe boundary available right now for applying a pending change?
bool DelayPolicy_IsSafeBoundaryAvailable();

/// What type of safe boundary is currently available?
DelaySafeBoundary DelayPolicy_GetCurrentSafeBoundary();

/// Get pending next-match delay (committed but not yet active).
int  DelayPolicy_GetPendingNextDelay();
bool DelayPolicy_HasPendingNextDelay();

// ============================================================================
// Session Reset
// ============================================================================

/// Reset all session state. Called on disconnect or new session.
void DelayPolicy_ResetSession();

// ============================================================================
// Diagnostics
// ============================================================================

void DelayPolicy_GetSnapshot(DelayPolicySnapshot* out);

} // namespace Net
