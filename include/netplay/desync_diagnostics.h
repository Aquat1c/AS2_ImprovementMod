/**
 * Alice Senki 2 - Desync Diagnostics
 *
 * Per-frame checksums piggybacked on gameplay packets (§21.8).
 * Compares local vs remote state for early desync detection.
 *
 * Checksum covers: HP, position, velocity, RNG seed, effects.
 * On mismatch: disconnect, log both checksums + entity state.
 */

#pragma once

#include <stdint.h>

namespace DesyncDiag {

// ============================================================================
// Per-Frame Checksum Record
// ============================================================================

struct FrameChecksum {
    uint32_t frame;
    uint32_t checksum;
    // Accompanying snapshot for diagnostics on mismatch
    int16_t  p1_hp, p1_x, p1_y;
    int16_t  p2_hp, p2_x, p2_y;
    uint32_t rng_seed;
    // Per-section CRCs for pinpointing desync source
    uint32_t crc_p1_entity;
    uint32_t crc_p2_entity;
    uint32_t crc_match_ctx;
    uint32_t crc_effects;
    uint32_t crc_summons;
    // Key divergence-prone values (entity timer at +0x1A7E8, meter at +0xB4)
    int16_t  p1_meter, p2_meter;
    uint32_t p1_entity_timer;
    uint32_t p2_entity_timer;
};

// ============================================================================
// Desync Dump
// ============================================================================

struct DesyncDump {
    uint32_t frame;
    uint32_t local_checksum;
    uint32_t remote_checksum;
    int16_t  local_p1_hp, local_p1_x, local_p1_y;
    int16_t  local_p2_hp, local_p2_x, local_p2_y;
    uint32_t local_rng;
    int16_t  remote_p1_hp, remote_p1_x, remote_p1_y;
    int16_t  remote_p2_hp, remote_p2_x, remote_p2_y;
    uint32_t remote_rng;
    // Per-section CRCs (local side — compare via log files)
    uint32_t local_crc_p1_entity;
    uint32_t local_crc_p2_entity;
    uint32_t local_crc_match_ctx;
    uint32_t local_crc_effects;
    uint32_t local_crc_summons;
    int16_t  local_p1_meter, local_p2_meter;
    uint32_t local_p1_timer, local_p2_timer;
};

// ============================================================================
// Configuration
// ============================================================================

// How many frames back the checksum piggyback covers (default N-8)
constexpr int CHECKSUM_DELAY_FRAMES = 8;

// Ring buffer size for stored checksums
constexpr int CHECKSUM_RING_SIZE = 128;

// ============================================================================
// API
// ============================================================================

// Initialize / reset the diagnostic system
void Init();
void Reset();

// Capture checksum for the current frame.
// Call once per simulated frame (after game state update).
void CaptureFrame(uint32_t frame);

// Get the checksum for a past frame (for piggybacking on packets).
// Returns false if frame is no longer in ring buffer.
bool GetChecksum(uint32_t frame, uint32_t* outChecksum);

// Get full checksum record for a frame.
bool GetFrameRecord(uint32_t frame, FrameChecksum* out);

// Report a received remote checksum for comparison.
// Returns true if match, false if desync detected.
bool CompareRemoteChecksum(uint32_t frame, uint32_t remoteChecksum,
                           const FrameChecksum* remoteSnap);

// Get the last desync dump (if any).
bool GetLastDesyncDump(DesyncDump* out);

// Is there an unresolved desync?
bool HasDesyncOccurred();

// ============================================================================
// Stats
// ============================================================================

struct Stats {
    uint32_t frames_captured;
    uint32_t comparisons_made;
    uint32_t mismatches_found;
    uint32_t last_desync_frame;
};

void GetStats(Stats* out);

} // namespace DesyncDiag
