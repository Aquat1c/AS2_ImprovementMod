/**
 * Alice Senki 2 - Ack Tracker
 *
 * Tracks seq/ack/ack_bits per logical channel (§10.5, §12).
 * Independent sequence spaces per channel.
 *
 * Usage:
 *   - OnSend(): get next outgoing seq, stamp header with current ack/ack_bits
 *   - OnRecv(): update ack state from received packet, detect duplicates
 *   - GetPendingAcks(): check which sent packets have been acked by remote
 */

#pragma once

#include <stdint.h>

namespace AckTracker {

constexpr int MAX_CHANNELS = 4;
constexpr int ACK_BITFIELD_SIZE = 32;
constexpr int SEND_HISTORY_SIZE = 256;  // Ring buffer of sent seq numbers

struct ChannelState {
    // Outgoing
    uint32_t next_send_seq;       // Next seq to assign to outgoing packets
    
    // Incoming
    uint32_t highest_recv_seq;    // Highest contiguous received seq
    uint32_t recv_bits;           // Bitfield for previous 32 seqs (relative to highest_recv_seq)
    
    // Remote ack tracking (what the remote has acked of our sent packets)
    uint32_t remote_ack;          // Last ack value received from remote
    uint32_t remote_ack_bits;     // Last ack_bits received from remote
};

// Initialize all channel states
void Init();

// Get next outgoing sequence for a channel. Increments the counter.
uint32_t NextSendSeq(uint16_t channel);

// Get current ack state to stamp on outgoing headers for a channel.
void GetAckState(uint16_t channel, uint32_t* outAck, uint32_t* outAckBits);

// Process a received packet's seq number. Returns false if duplicate.
bool OnRecvSeq(uint16_t channel, uint32_t seq);

// Process ack/ack_bits from a received packet (updates our send-side tracking).
void OnRecvAck(uint16_t channel, uint32_t ack, uint32_t ackBits);

// Check if a specific seq number has been acked by the remote.
bool IsAcked(uint16_t channel, uint32_t seq);

// Get channel state (for diagnostics)
const ChannelState* GetChannelState(uint16_t channel);

// Reset a specific channel
void ResetChannel(uint16_t channel);

} // namespace AckTracker
