/**
 * Alice Senki 2 - Input Synchronization Hooks
 * 
 * High-level hooks on the game's input sync functions for rollback integration.
 *
 * When rollback is active (SetRollbackActive(true)):
 *   - Hook_InputDispatcher drives the game loop via RollbackSession events
 *   - Vanilla send/recv/sync functions are suppressed
 *   - Frame advancement is controlled by GekkoNet
 *
 * When rollback is inactive (default):
 *   - All hooks pass through to the original game functions
 */

#pragma once

#include <stdint.h>

namespace InputSyncHooks {

// ============================================================================
// Lifecycle
// ============================================================================

bool Install();
void Uninstall();
bool IsInstalled();

// ============================================================================
// Rollback Mode Toggle
// ============================================================================

// Enable/disable rollback-driven frame stepping.
// When active: vanilla sync is suppressed, RollbackSession drives frames.
// When inactive: all hooks pass through to original functions.
void SetRollbackActive(bool active);
bool IsRollbackActive();

// ============================================================================
// Load Barrier Freeze
// ============================================================================

// Freeze game frame processing during load barrier synchronization.
// While frozen: InputDispatcher returns -1, vanilla send/recv suppressed.
// Game loop continues (D3D9 Present, ImGui, SessionManager updates) but
// no gameplay frames are processed.
void SetLoadBarrierFreeze(bool freeze);
bool IsLoadBarrierFrozen();

// ============================================================================
// Match Lifecycle
// ============================================================================

void ResetForNewMatch();
void ResetInjectionState();

// ============================================================================
// Stats
// ============================================================================

void GetStats(uint32_t* framesProcessed, uint32_t* vanillaFrames);
void ResetStats();

struct TimesyncSnapshot {
    float    nudge_us;          // Current nudge amount in microseconds
    uint32_t nudge_count;       // Total frames where nudge was applied
    float    last_fresh_ahead;  // Raw GekkoNet frames_ahead at last evaluation
    int      skip_budget;       // Current hard-skip budget remaining
    float    smoothed_ahead;    // EMA-filtered frames_ahead
    float    jitter;            // EMA of |raw - smoothed| deviation
    float    effective_dz;      // Current effective deadzone (base + jitter contribution)
    bool     skip_armed;        // Whether hard skip hysteresis is armed
    int      pathological_ctr;  // Consecutive frames above pathological threshold
};

void GetTimesyncSnapshot(TimesyncSnapshot* out);

} // namespace InputSyncHooks
