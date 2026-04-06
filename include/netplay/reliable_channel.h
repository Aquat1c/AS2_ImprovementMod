/**
 * Alice Senki 2 - Reliable Channel
 *
 * Reliable ordered delivery over UDP (§12).
 * Maintains a send queue with retransmission for control/session packets.
 * Deduplicates incoming packets using sequence tracking.
 *
 * Timing (§12.3):
 *   - Retransmit timeout: 100ms initial, backs off to 200ms
 *   - Handshake no-progress timeout: 5s
 *   - Max retries: 30 (~3s at 100ms intervals)
 */

#pragma once

#include "packet_codec.h"
#include <stdint.h>
#include <winsock2.h>

namespace ReliableChannel {

constexpr int MAX_PENDING_SENDS = 128;
constexpr int MAX_RECV_DEDUP    = 128;
constexpr int RETRANSMIT_MS_MIN = 20;     // Floor: never retransmit faster than 20ms (LAN optimized)
constexpr int RETRANSMIT_MS_DEFAULT = 100; // Default without RTT info
constexpr int RETRANSMIT_MS_MAX = 500;    // Ceiling for backed-off retransmits
constexpr int MAX_RETRIES       = 50;     // ~5-10s depending on RTT

struct PendingSend {
    bool     active;
    uint32_t seq;
    uint32_t first_send_ms;
    uint32_t last_send_ms;
    int      retry_count;
    uint16_t channel;
    uint8_t  packet_data[PacketCodec::MAX_PACKET_SIZE];
    int      packet_len;
};

struct Stats {
    uint32_t packets_sent;
    uint32_t packets_resent;
    uint32_t packets_acked;
    uint32_t packets_dropped;    // exceeded max retries
    uint32_t duplicates_filtered;
    int      pending_count;
};

// Initialize the reliable channel system
void Init();

// Enqueue a reliable packet for sending. The packet is sent immediately
// and re-sent periodically until acked. Returns the assigned seq, or 0 on error.
uint32_t Send(SOCKET sock, const sockaddr_in* peer,
              PacketCodec::PacketType type, uint16_t channel,
              const void* payload, int payloadLen,
              uint64_t sessionId, uint32_t connectionId);

// Process ack information from a received header (call on every received packet).
// Removes acked packets from the resend queue.
void ProcessAcks(uint16_t channel, uint32_t ack, uint32_t ackBits);

// Pump retransmissions. Call once per frame (~16ms).
// Returns number of retransmitted packets.
int PumpRetransmits(SOCKET sock, const sockaddr_in* peer);

// Check if a received seq on a channel is a duplicate. Returns true if new.
bool AcceptIncoming(uint16_t channel, uint32_t seq);

// Get current stats
void GetStats(Stats* out);

// Update the RTT estimate used for retransmit timing.
// Called by SessionManager whenever a new RTT sample is computed.
void SetRttMs(float rttMs);

// Reset all state
void Reset();

// Get number of pending unacked packets
int GetPendingCount();

} // namespace ReliableChannel
