/**
 * Alice Senki 2 - Rollback Debug / Diagnostics
 *
 * Aggregates diagnostic data from all rollback subsystems into a single
 * unified view. Provides logging, ImGui rendering, and desync detection.
 *
 * Also handles:
 *   - Per-frame checksum logging for desync diagnosis
 *   - Network state digest exchange
 *   - Integration with determinism verification system
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// ============================================================================
// Lifecycle
// ============================================================================

void RollbackDebug_Init();
void RollbackDebug_Shutdown();

/// Reset per-match digest/checksum/desync state while keeping the module initialized.
void RollbackDebug_ResetSession();

// ============================================================================
// Per-Frame
// ============================================================================

/// Call each frame during rollback gameplay. Logs diagnostics, sends
/// state digests, checks for desync indicators.
void RollbackDebug_FrameUpdate();

// ============================================================================
// Desync Detection
// ============================================================================

/// Feed a remote state digest for comparison.
void RollbackDebug_OnRemoteDigest(int32_t frame, uint32_t remote_crc);

/// Feed remote frame-progress telemetry for live frame skew diagnosis.
void RollbackDebug_OnRemoteFrameSyncStatus(int32_t remote_rb_frame,
                                           int32_t remote_game_abs_frame,
                                           int32_t remote_frame_origin_abs,
                                           int32_t remote_rb_frame_last_received,
                                           int32_t remote_rb_frame_confirmed,
                                           int32_t remote_predicted_frames,
                                           uint32_t remote_checksum);

/// Check if a desync has been detected.
bool RollbackDebug_IsDesyncDetected();

/// Get the frame at which desync was first detected (-1 if none).
int32_t RollbackDebug_GetDesyncFrame();

// ============================================================================
// Configuration
// ============================================================================

/// Enable/disable per-frame state digest exchange.
void RollbackDebug_SetDigestEnabled(bool enabled);
bool RollbackDebug_IsDigestEnabled();

/// Set how often (in frames) to send/check state digests.
/// Lower = more CPU but faster desync detection. Default = 60.
void RollbackDebug_SetDigestInterval(int frames);

// ============================================================================
// Diagnostics Snapshot
// ============================================================================

struct RollbackDebugSnapshot {
    // Desync state
    bool     desync_detected;
    int32_t  desync_frame;
    uint32_t desync_local_crc;
    uint32_t desync_remote_crc;

    // Frame checksums
    uint32_t current_frame_checksum;
    int32_t  last_digest_frame;

    // Digest config
    bool     digest_enabled;
    int      digest_interval;
    int32_t  digests_sent;
    int32_t  digests_received;
    int32_t  digests_matched;
    int32_t  digests_mismatched;
};

void RollbackDebug_GetSnapshot(RollbackDebugSnapshot* out);

/// Render ImGui debug window for rollback state.
void RollbackDebug_RenderImGui(bool* p_open);

} // namespace Rollback
