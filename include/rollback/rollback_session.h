/**
 * Alice Senki 2 - Rollback Session
 *
 * Central runtime owner of rollback gameplay state.
 * Orchestrates:
 *   - Input collection (local from SDL, remote from network)
 *   - Input prediction and misprediction detection
 *   - State capture into rolling history
 *   - Rollback trigger and resimulation
 *   - Frame advance contract
 *   - Delay policy consumption
 *   - Side-effect suppression flag
 *   - Diagnostics/snapshot
 *
 * Lifecycle:
 *   1. Pre-game sync completes, bootstrap captures baseline
 *   2. RollbackSession_Begin() starts the session
 *   3. RollbackSession_FrameUpdate() runs every frame during playable gameplay
 *   4. RollbackSession_End() cleans up on match exit or disconnect
 *
 * The rollback session does NOT own the match lifecycle or sync policy.
 * It consumes them via read-only queries.
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
    int      initial_delay;      // Agreed input delay at session start
    int      rollback_budget;    // Max rollback frames allowed
    uint32_t baseline_checksum;  // CRC32 of baseline state (for verification)
    int32_t  start_frame;        // Frame number to begin at (typically 0)
};

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize rollback subsystems. Called once at mod init.
void RollbackSession_Init();

/// Shutdown rollback subsystems.
void RollbackSession_Shutdown();

/// Begin a rollback gameplay session after bootstrap handoff.
/// Captures the initial baseline state.
bool RollbackSession_Begin(const RollbackSessionConfig& config);

/// End the current rollback session.
void RollbackSession_End();

/// Is a rollback session currently active?
bool RollbackSession_IsActive();

// ============================================================================
// Per-Frame (called from ModOnFrame during playable gameplay)
// ============================================================================

/// Main per-frame update. This is the rollback gameplay loop entry point.
/// Must be called every frame when SyncPolicy_IsRollbackActive() is true.
///
/// Performs:
///   1. Collect local input from SDL
///   2. Send local input to remote peer
///   3. Check for new remote inputs
///   4. Predict missing remote frames if needed
///   5. Check for mispredictions
///   6. If misprediction: rollback + resimulate
///   7. Save state for current frame
///   8. Write inputs to game buffers
///   9. (Game loop will advance the frame via normal Mode 8 handler)
void RollbackSession_FrameUpdate();

// ============================================================================
// Remote Input Ingestion
// ============================================================================

/// Submit a remote gameplay input received from the network.
/// Called by the session packet callback when a GameplayInput arrives.
void RollbackSession_SubmitRemoteInput(int32_t frame, uint16_t input);

/// Submit a batch of remote inputs (for redundant/cumulative packets).
void RollbackSession_SubmitRemoteInputBatch(int32_t start_frame, const uint16_t* inputs, int count);

// ============================================================================
// Local Input Injection (for testing/verification)
// ============================================================================

/// Override local input for a specific frame (for test harness / scripted input).
/// If not called, local input comes from SDL.
void RollbackSession_InjectLocalInput(int32_t frame, uint16_t input);

// ============================================================================
// Queries
// ============================================================================

/// Current simulation frame.
int32_t RollbackSession_GetCurrentFrame();

/// Last fully confirmed frame (both local and remote confirmed).
int32_t RollbackSession_GetLastConfirmedFrame();

/// Whether resimulation is currently in progress.
bool RollbackSession_IsResimulating();

/// Current active delay being used.
int RollbackSession_GetActiveDelay();

/// Current rollback budget being used.
int RollbackSession_GetRollbackBudget();

/// Whether side effects should be suppressed (during resim).
bool RollbackSession_ShouldSuppressSideEffects();

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

    // Rollback stats
    int32_t  rollback_count;
    int32_t  last_rollback_start_frame;
    int32_t  last_rollback_replay_length;
    int32_t  max_rollback_distance;
    int32_t  predicted_frames_outstanding;

    // Resimulation state
    bool     is_resimulating;
    bool     side_effects_suppressed;

    // Policy consumption
    int      active_delay;
    int      rollback_budget;

    // State integrity
    uint32_t current_checksum;
    uint32_t baseline_checksum;

    // Input stats
    int32_t  total_predictions;
    int32_t  total_mispredictions;
    int32_t  total_correct_predictions;

    // Frames sent/received
    int32_t  local_inputs_sent;
    int32_t  remote_inputs_received;
};

void RollbackSession_GetSnapshot(RollbackSessionSnapshot* out);

} // namespace Rollback
