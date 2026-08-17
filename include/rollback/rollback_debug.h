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

/// Authoritative gameplay checksum (same algorithm as RollbackSession_ComputeLiveStateChecksum).
uint32_t RollbackDebug_ComputeAuthoritativeChecksum();

/// Lookup a retained per-rb-frame authoritative checksum (-1 if not retained).
bool RollbackDebug_TryGetChecksumForFrame(int32_t frame, uint32_t* checksum);

/// True when both peers should have a settled authoritative view of rb_frame.
bool RollbackDebug_IsRbFrameReadyToCompare(int32_t frame, int32_t remote_confirmed_rb);

/// Feed a remote state digest for comparison.
void RollbackDebug_OnRemoteDigest(int32_t frame, uint32_t remote_crc);

/// Report confirmed simulation drift from any detection path (StateDigest, SyncHash, SyncTrace).
void RollbackDebug_ReportDrift(const char* source,
                               int32_t frame,
                               uint32_t local_crc,
                               uint32_t remote_crc,
                               const char* detail);

/// Emit digest/integrity counters for the ending match.
void RollbackDebug_LogSessionSummary(const char* reason);

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
    int32_t  digests_skipped_not_settled;
    int32_t  digests_skipped_no_history;
    int32_t  digests_skipped_remote_unsettled;
};

void RollbackDebug_GetSnapshot(RollbackDebugSnapshot* out);

/// Render ImGui debug window for rollback state.
void RollbackDebug_RenderImGui(bool* p_open);

} // namespace Rollback
