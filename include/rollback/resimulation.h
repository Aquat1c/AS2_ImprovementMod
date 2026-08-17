/**
 * Alice Senki 2 - Resimulation Engine
 *
 * Handles the core rollback operation:
 *   1. Detect first mispredicted frame
 *   2. Load the saved state for that frame (via state history)
 *   3. Resimulate frame-by-frame with corrected inputs
 *   4. Return to live gameplay at the current frame
 *
 * The resimulation engine is stateless between rollback events.
 * It operates on the savestate history and input timeline owned
 * by the rollback session.
 *
 * Side-effect suppression:
 *   During resimulation, the `IsResimulating()` flag is set.
 *   Other systems (audio, debug HUD) should check this flag
 *   and suppress non-deterministic side effects.
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// ============================================================================
// Resimulation State
// ============================================================================

/// True while the engine is replaying frames (after load, before catching up).
bool Resim_IsResimulating();

/// Current frame being resimulated (-1 if not resimulating).
int32_t Resim_GetCurrentFrame();

/// How many frames the current resimulation will replay.
int32_t Resim_GetReplayLength();

// ============================================================================
// State History (Ring Buffer of Savestates)
// ============================================================================

/// Maximum number of saved states in the ring buffer (re0.7 M4, plan §2.7.7):
/// 64 slots (R_max 15 + catch-up headroom + safety), direct-mapped
/// slot = frame % 64.
constexpr int STATE_HISTORY_CAPACITY = 64;

/// Initialize the state history ring buffer.
void StateHistory_Init();
void StateHistory_Shutdown();
void StateHistory_Reset();

/// Set the identity tag stamped onto every subsequent capture (re0.7 M4,
/// plan §2.7.7): every slot is tagged {epoch, frame, phase} and restores
/// validate the tag fail-closed. Epoch 0 = untagged/offline (legacy callers:
/// replay stepping, training, manual savestate baseline).
void StateHistory_SetTagContext(uint32_t epoch, uint32_t phase);

/// Capture the current game state and associate it with the given frame.
/// Returns true on success.
bool StateHistory_CaptureFrame(int32_t frame);

/// Capture and also report the Block64 gameplay digest of the captured
/// sim-affecting bytes (the SyncHash / confirm-pipeline `pre_state_hash`).
bool StateHistory_CaptureFrameHashed(int32_t frame, uint64_t* out_gameplay_hash);

/// Load a previously captured state for the given frame.
/// Returns true on success, false if the frame isn't in history.
bool StateHistory_LoadFrame(int32_t frame);

/// Tag-validated restore (fail-closed): refuses a slot whose {epoch, frame}
/// tag does not match. A stale-epoch slot can NEVER be restored (§2.7.5).
bool StateHistory_LoadFrameTagged(int32_t frame, uint32_t epoch);

/// Check if a specific frame is available in history.
bool StateHistory_HasFrame(int32_t frame);

/// Discard any saved states newer than the given frame.
/// Preserves older snapshots so reverse stepping can continue across restores.
void StateHistory_DiscardFramesAfter(int32_t frame);

/// Get the oldest frame available in history.
int32_t StateHistory_GetOldestFrame();

/// Get the newest frame available in history.
int32_t StateHistory_GetNewestFrame();

/// Get the number of states currently stored.
int32_t StateHistory_GetCount();

// ============================================================================
// Resimulation Execution
// ============================================================================

/// Perform a rollback: load state at rollback_frame, resimulate forward
/// until target_frame using corrected inputs from the timeline.
///
/// Parameters:
///   rollback_frame  — the frame to rewind to (must exist in state history)
///   target_frame    — the frame to catch up to (current simulation frame)
///   local_player    — which player slot is local (0 or 1)
///
/// Returns the number of frames resimulated, or -1 on failure.
///
/// During resimulation:
///   - IsResimulating() returns true
///   - Each frame reads inputs from the input timeline
///   - Each frame writes inputs to game buffers
///   - Each frame advances the game simulation by one tick
///   - Side effects should be suppressed by external systems
int32_t Resim_Execute(int32_t rollback_frame, int32_t target_frame, int local_player);

// ============================================================================
// Diagnostics
// ============================================================================

struct ResimSnapshot {
    bool     is_resimulating;
    int32_t  current_resim_frame;
    int32_t  replay_length;

    // Lifetime stats
    int32_t  total_rollbacks;
    int32_t  total_frames_resimulated;
    int32_t  max_rollback_depth;
    int32_t  last_rollback_frame;
    int32_t  last_rollback_length;

    // State history
    int32_t  history_count;
    int32_t  history_oldest_frame;
    int32_t  history_newest_frame;
};

void Resim_GetSnapshot(ResimSnapshot* out);

} // namespace Rollback
