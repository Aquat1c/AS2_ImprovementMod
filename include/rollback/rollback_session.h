/**
 * Alice Senki 2 - Rollback Session (GekkoNet-driven)
 *
 * GekkoNet event-driven rollback session. GekkoNet owns all rollback
 * logic: prediction, misprediction detection, savestate management,
 * resimulation scheduling, and timesync.
 *
 * The mod provides:
 *   - Transport adapter (ENet ↔ GekkoNet)
 *   - State capture/restore callbacks
 *   - Input injection into game buffers
 *   - Frame advance via game's match handler
 *
 * Two-phase API for the input dispatcher hook:
 *   1. BeginFrame(localInput) — feed local input to GekkoNet, update session
 *   2. ProcessNextEvent() — iterate GekkoNet events (save/load/advance)
 *      Returns Advance → dispatcher writes inputs, returns 0 (game steps)
 *      Returns Done → dispatcher returns -1 (break game's loop)
 *
 * Lifecycle:
 *   1. RollbackSession_Init() at mod startup
 *   2. RollbackSession_Begin() at bootstrap handoff
 *   3. BeginFrame/ProcessNextEvent called by input dispatcher each frame
 *   4. RollbackSession_End() on match exit or disconnect
 *   5. RollbackSession_Shutdown() at mod shutdown
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// ============================================================================
// Session Configuration (provided at Begin)
// ============================================================================

struct RollbackSessionConfig {
    int      local_player;       // 0 = P1, 1 = P2
    int      remote_player;      // 0 = P1, 1 = P2
    int      initial_delay;      // Input delay for local player
    int      rollback_budget;    // Max prediction window (GekkoNet input_prediction_window)
    uint32_t baseline_checksum;  // CRC32 of baseline state (for verification)
    int32_t  start_frame;        // Native gameplay frame where rollback ownership begins
};

// ============================================================================
// Event Result (returned by ProcessNextEvent)
// ============================================================================

enum class EventResult {
    Advance,     // GekkoNet wants one frame advanced — inputs written to game buffers
    Done,        // No more events this update — break game's dispatcher loop
    Error        // Session error — caller should end session
};

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize rollback subsystems (state history, GekkoNet). Called once at mod init.
void RollbackSession_Init();

/// Shutdown rollback subsystems.
void RollbackSession_Shutdown();

/// Begin a GekkoNet rollback session after bootstrap handoff.
bool RollbackSession_Begin(const RollbackSessionConfig& config);

/// End the current session. Destroys GekkoNet session and cleans up.
void RollbackSession_End();

/// Is a rollback session currently active?
bool RollbackSession_IsActive();

// ============================================================================
// Two-Phase Frame Processing (called from input dispatcher hook)
// ============================================================================

/// Phase 1: Feed local input to GekkoNet, trigger session update.
/// Call ONCE per game loop iteration before ProcessNextEvent.
void RollbackSession_BeginFrame(uint16_t localInput);

/// Phase 2: Process the next GekkoNet event.
/// Call repeatedly until it returns Done or Error.
///   Advance → inputs written to game buffers; dispatcher should return 0
///   Done    → no more events; dispatcher should return -1
///   Error   → session broken; dispatcher should return -1
EventResult RollbackSession_ProcessNextEvent();

/// Get the P1/P2 inputs from the last Advance event.
/// Only valid after ProcessNextEvent returns Advance.
void RollbackSession_GetAdvanceInputs(uint16_t* p1, uint16_t* p2);

// ============================================================================
// GekkoNet Packet Ingestion
// ============================================================================

/// Buffer a received GekkoData packet payload for GekkoNet to drain.
/// Called by the packet callback when a GekkoData packet arrives.
void RollbackSession_BufferGekkoPacket(const void* data, size_t len);

// ============================================================================
// Queries
// ============================================================================

/// Current simulation frame (from last advance event).
int32_t RollbackSession_GetCurrentFrame();

/// Whether the current advance event is a rollback resimulation frame.
bool RollbackSession_IsRollingBack();

/// GekkoNet's frame advantage metric for timesync decisions.
float RollbackSession_FramesAhead();

/// Current active delay.
int RollbackSession_GetActiveDelay();

/// Current rollback budget.
int RollbackSession_GetRollbackBudget();

/// Whether side effects should be suppressed (during rollback resim).
bool RollbackSession_ShouldSuppressSideEffects();

// ============================================================================
// Local Input Injection (for testing/verification)
// ============================================================================

/// Override local input for the next BeginFrame call (test harness).
void RollbackSession_InjectLocalInput(uint16_t input);

// ============================================================================
// Diagnostics
// ============================================================================

struct RollbackSessionSnapshot {
    bool     active;
    int      local_player;
    int      remote_player;

    // Frame state
    int32_t  current_frame;
    int32_t  last_confirmed_frame;
    int32_t  last_remote_received_frame;
    int32_t  last_saved_state_frame;

    // Rollback stats (from resim subsystem)
    int32_t  rollback_count;
    int32_t  last_rollback_start_frame;
    int32_t  last_rollback_replay_length;
    int32_t  max_rollback_distance;
    int32_t  predicted_frames_outstanding;

    // GekkoNet state
    bool     is_rolling_back;
    bool     side_effects_suppressed;
    float    frames_ahead;

    // Policy
    int      active_delay;
    int      rollback_budget;

    // State integrity
    uint32_t current_checksum;
    uint32_t baseline_checksum;

    // Input stats
    int32_t  total_predictions;
    int32_t  total_mispredictions;
    int32_t  total_correct_predictions;

    // IO counts
    int32_t  local_inputs_sent;
    int32_t  remote_inputs_received;

    // GekkoNet network stats
    float    gekko_avg_ping;
    float    gekko_jitter;
};

void RollbackSession_GetSnapshot(RollbackSessionSnapshot* out);

} // namespace Rollback
