/**
 * Alice Senki 2 - Spectator Manager Implementation (Skeleton)
 *
 * MVP spectator support: admission, snapshot ring, input forwarding stubs.
 */

#include "spectator_manager.h"
#include "packet_codec.h"
#include "log_window.h"
#include <string.h>

namespace SpectatorManager {

// ============================================================================
// Internal State
// ============================================================================

static bool s_enabled = false;
static SpectatorInfo s_spectators[MAX_SPECTATORS] = {};
static StoredSnapshot s_snapshots[SNAPSHOT_RING_SIZE] = {};
static int s_snapshotHead = 0;
static uint32_t s_lastSnapshotFrame = 0;
static Stats s_stats = {};

// ============================================================================
// Lifecycle
// ============================================================================

void Init() {
    Reset();
    LOG_INFO("[Spectator] Initialized (max=%d, interval=%d frames, ring=%d)",
             MAX_SPECTATORS, SNAPSHOT_INTERVAL_FRAMES, SNAPSHOT_RING_SIZE);
}

void Reset() {
    memset(s_spectators, 0, sizeof(s_spectators));
    memset(s_snapshots, 0, sizeof(s_snapshots));
    s_snapshotHead = 0;
    s_lastSnapshotFrame = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    LOG_INFO("[Spectator] Reset");
}

bool IsEnabled() { return s_enabled; }
void SetEnabled(bool enabled) {
    if (s_enabled != enabled) {
        LOG_INFO("[Spectator] %s", enabled ? "Enabled" : "Disabled");
    }
    s_enabled = enabled;
}

// ============================================================================
// Frame Update
// ============================================================================

void FrameUpdate(uint32_t currentFrame, SOCKET sock) {
    if (!s_enabled) return;
    
    // Check if we should capture a snapshot
    if (currentFrame - s_lastSnapshotFrame >= SNAPSHOT_INTERVAL_FRAMES) {
        // Capture snapshot into ring buffer
        StoredSnapshot& snap = s_snapshots[s_snapshotHead];
        snap.valid    = true;
        snap.frame    = currentFrame;
        snap.checksum = 0;  // TODO: compute from actual savestate
        snap.state_size = 0;  // TODO: capture actual savestate data
        
        LOG_DEBUG("[Spectator] Snapshot captured: frame=%u slot=%d", currentFrame, s_snapshotHead);
        s_snapshotHead = (s_snapshotHead + 1) % SNAPSHOT_RING_SIZE;
        s_lastSnapshotFrame = currentFrame;
    }
    
    // Forward inputs to connected spectators
    int active = 0;
    for (int i = 0; i < MAX_SPECTATORS; i++) {
        if (s_spectators[i].connected) {
            active++;
            // TODO: send SpectatorInputs packet to spectators[i]
            s_stats.inputs_forwarded++;
        }
    }
    s_stats.active_count = active;
}

// ============================================================================
// Admission
// ============================================================================

void OnReceiveSpectatorHello(const PacketCodec::ModNetHeader* header,
                             const void* payload, int payloadLen,
                             const sockaddr_in* from, SOCKET sock) {
    if (!s_enabled) {
        LOG_INFO("[Spectator] Rejected (spectating disabled)");
        s_stats.spectators_rejected++;
        
        // Send SpectatorReject
        PacketCodec::ModNetHeader rejectHdr;
        PacketCodec::InitHeader(&rejectHdr, PacketCodec::PacketType::SpectatorReject,
                                PacketCodec::CHANNEL_SPECTATOR);
        uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
        int len = PacketCodec::Encode(&rejectHdr, nullptr, 0, buf, sizeof(buf));
        if (len > 0) {
            sendto(sock, (const char*)buf, len, 0,
                   (const sockaddr*)from, sizeof(*from));
        }
        return;
    }
    
    // Find free slot
    int freeSlot = -1;
    for (int i = 0; i < MAX_SPECTATORS; i++) {
        if (!s_spectators[i].connected) {
            freeSlot = i;
            break;
        }
    }
    
    if (freeSlot < 0) {
        LOG_INFO("[Spectator] Rejected (all slots full)");
        s_stats.spectators_rejected++;
        
        // Send SpectatorReject
        PacketCodec::ModNetHeader rejectHdr;
        PacketCodec::InitHeader(&rejectHdr, PacketCodec::PacketType::SpectatorReject,
                                PacketCodec::CHANNEL_SPECTATOR);
        uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
        int len = PacketCodec::Encode(&rejectHdr, nullptr, 0, buf, sizeof(buf));
        if (len > 0) {
            sendto(sock, (const char*)buf, len, 0,
                   (const sockaddr*)from, sizeof(*from));
        }
        return;
    }
    
    // Accept spectator
    SpectatorInfo& spec = s_spectators[freeSlot];
    spec.connected = true;
    spec.addr = *from;
    spec.connected_at_ms = PacketCodec::GetTimestampMs();
    spec.last_keyframe_sent = 0;
    // Parse nickname from payload if available
    spec.nickname[0] = 0;
    
    s_stats.spectators_connected++;
    LOG_INFO("[Spectator] Accepted spectator #%d from %s:%d",
             freeSlot, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    
    // Send SpectatorAccept
    PacketCodec::ModNetHeader acceptHdr;
    PacketCodec::InitHeader(&acceptHdr, PacketCodec::PacketType::SpectatorAccept,
                            PacketCodec::CHANNEL_SPECTATOR);
    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    int len = PacketCodec::Encode(&acceptHdr, nullptr, 0, buf, sizeof(buf));
    if (len > 0) {
        sendto(sock, (const char*)buf, len, 0,
               (const sockaddr*)from, sizeof(*from));
    }
    
    // Send most recent keyframe
    // TODO: Find latest valid snapshot in ring buffer and send it
    LOG_INFO("[Spectator] TODO: Send keyframe to new spectator");
}

// ============================================================================
// Queries
// ============================================================================

int GetActiveCount() {
    int count = 0;
    for (int i = 0; i < MAX_SPECTATORS; i++) {
        if (s_spectators[i].connected) count++;
    }
    return count;
}

void GetStats(Stats* out) {
    if (out) *out = s_stats;
}

void GetSpectatorInfo(int index, SpectatorInfo* out) {
    if (!out || index < 0 || index >= MAX_SPECTATORS) return;
    *out = s_spectators[index];
}

} // namespace SpectatorManager
