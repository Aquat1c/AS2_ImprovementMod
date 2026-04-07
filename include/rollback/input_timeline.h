/**
 * Alice Senki 2 - Input Timeline
 *
 * Frame-indexed input history for rollback gameplay.
 * Stores both local and remote input per frame, tracks confirmation
 * and prediction status.
 *
 * Input format: 16-bit bitmask matching the game's native format
 * (INPUT_UP/DOWN/LEFT/RIGHT/A/B/C/D/START/SELECT/L1/R1/L2/R2).
 *
 * The timeline is a fixed-size ring buffer. Frames older than the
 * buffer capacity are discarded. The buffer must be large enough
 * to hold MAX_ROLLBACK + input_delay + safety margin frames.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace Rollback {

// ============================================================================
// Constants
// ============================================================================

/// Maximum number of frames stored in the timeline ring buffer.
/// Must exceed max rollback + delay + margin. 256 is generous.
constexpr int TIMELINE_CAPACITY = 256;

/// Neutral input (no buttons pressed).
constexpr uint16_t INPUT_NEUTRAL = 0x0000;

// ============================================================================
// Per-Frame Input Entry
// ============================================================================

struct FrameInput {
    uint16_t local;               // Local player's input
    uint16_t remote;              // Remote player's input (real or predicted)
    bool     local_confirmed;     // Local input is committed
    bool     remote_confirmed;    // Remote input is real (not predicted)
    bool     remote_predicted;    // Remote input was filled by prediction
    bool     prediction_wrong;    // Real remote arrived and differs from predicted
};

// ============================================================================
// Lifecycle
// ============================================================================

void InputTimeline_Init();
void InputTimeline_Shutdown();

/// Clear all history and reset to frame 0.
void InputTimeline_Reset();

// ============================================================================
// Writing Input
// ============================================================================

/// Record the local player's input for the given frame.
/// Called once per frame during forward simulation.
void InputTimeline_SetLocalInput(int32_t frame, uint16_t input);

/// Record real remote input for the given frame.
/// If this frame was previously predicted, sets prediction_wrong if different.
/// Returns true if this input contradicts a previous prediction (triggers rollback).
bool InputTimeline_SetRemoteInput(int32_t frame, uint16_t input);

/// Fill remote input for a frame using prediction (called when real input
/// hasn't arrived yet). Marks the frame as predicted.
void InputTimeline_PredictRemoteInput(int32_t frame, uint16_t predicted_input);

// ============================================================================
// Reading Input
// ============================================================================

/// Get the input entry for a specific frame.
/// Returns nullptr if the frame is outside the buffer range.
const FrameInput* InputTimeline_GetFrame(int32_t frame);

/// Get the local input for a specific frame. Returns INPUT_NEUTRAL if unavailable.
uint16_t InputTimeline_GetLocalInput(int32_t frame);

/// Get the remote input for a specific frame (real or predicted).
/// Returns INPUT_NEUTRAL if unavailable.
uint16_t InputTimeline_GetRemoteInput(int32_t frame);

/// Check if remote input is confirmed for a given frame.
bool InputTimeline_IsRemoteConfirmed(int32_t frame);

// ============================================================================
// Timeline Queries
// ============================================================================

/// The earliest frame still in the buffer.
int32_t InputTimeline_GetEarliestFrame();

/// The latest frame that has local input.
int32_t InputTimeline_GetLatestLocalFrame();

/// The latest frame with confirmed remote input.
int32_t InputTimeline_GetLatestConfirmedRemoteFrame();

/// Find the first frame >= start_frame where remote was predicted and is
/// now known to be wrong. Returns -1 if no mismatch found.
int32_t InputTimeline_FindFirstMisprediction(int32_t start_frame);

/// Number of frames currently predicted (remote not yet confirmed).
int32_t InputTimeline_GetPredictedFrameCount();

// ============================================================================
// Diagnostics
// ============================================================================

struct InputTimelineSnapshot {
    int32_t earliest_frame;
    int32_t latest_local_frame;
    int32_t latest_confirmed_remote_frame;
    int32_t predicted_frame_count;
    int32_t total_predictions;       // Lifetime count
    int32_t total_mispredictions;    // Lifetime count
    int32_t total_correct_predictions;
};

void InputTimeline_GetSnapshot(InputTimelineSnapshot* out);

} // namespace Rollback
