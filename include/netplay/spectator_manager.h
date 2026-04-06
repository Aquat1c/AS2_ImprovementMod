/**
 * Alice Senki 2 - Spectator Manager
 *
 * Skeleton implementation of spectator support (§20).
 * MVP: 2 spectators, non-authoritative, join-in-progress via keyframes.
 *
 * Spectators do NOT participate in:
 *   - Synchronization barriers
 *   - Desync detection
 *   - Match flow decisions
 *
 * Spectator disconnect is a silent no-op for match flow.
 */

#pragma once

#include "packet_codec.h"
#include <stdint.h>
#include <winsock2.h>

namespace SpectatorManager {

// ============================================================================
// Configuration
// ============================================================================

constexpr int MAX_SPECTATORS = 2;
constexpr int SNAPSHOT_INTERVAL_FRAMES = 60;    // 1 snapshot per second
constexpr int SNAPSHOT_RING_SIZE = 3;           // Rolling buffer of last 3 snapshots

// ============================================================================
// Spectator Info
// ============================================================================

struct SpectatorInfo {
    bool        connected;
    sockaddr_in addr;
    uint64_t    nonce;
    uint32_t    last_keyframe_sent;
    uint32_t    connected_at_ms;
    char        nickname[24];
};

// ============================================================================
// Snapshot (rolling buffer entry)
// ============================================================================

struct StoredSnapshot {
    bool     valid;
    uint32_t frame;
    uint32_t checksum;
    uint8_t  state_data[256 * 1024];  // 256KB max savestate
    uint32_t state_size;
};

// ============================================================================
// Stats
// ============================================================================

struct Stats {
    uint32_t spectators_connected;
    uint32_t spectators_rejected;
    uint32_t keyframes_sent;
    uint32_t inputs_forwarded;
    int      active_count;
};

// ============================================================================
// Lifecycle
// ============================================================================

// Initialize the spectator system
void Init();

// Reset all spectator state
void Reset();

// Is spectator hosting enabled?
bool IsEnabled();
void SetEnabled(bool enabled);

// ============================================================================
// Per-Frame Update
// ============================================================================

// Call once per gameplay frame.
// Manages snapshot capture and input forwarding.
void FrameUpdate(uint32_t currentFrame, SOCKET sock);

// ============================================================================
// Admission
// ============================================================================

// Process a SpectatorHello from a potential spectator.
void OnReceiveSpectatorHello(const PacketCodec::ModNetHeader* header,
                             const void* payload, int payloadLen,
                             const sockaddr_in* from, SOCKET sock);

// ============================================================================
// Queries
// ============================================================================

int GetActiveCount();
void GetStats(Stats* out);
void GetSpectatorInfo(int index, SpectatorInfo* out);

} // namespace SpectatorManager
