/**
 * Alice Senki 2 - Ack Tracker Implementation
 *
 * Per-channel sequence tracking, duplicate detection, and ack management.
 */

#include "ack_tracker.h"
#include "log_window.h"
#include <string.h>

namespace AckTracker {

static ChannelState s_channels[MAX_CHANNELS];
static bool s_initialized = false;

static void EnsureInitialized() {
    if (!s_initialized) {
        Init();
    }
}

void Init() {
    memset(s_channels, 0, sizeof(s_channels));
    // Start seq at 1 so 0 means "no packets sent/received yet"
    for (int i = 0; i < MAX_CHANNELS; i++) {
        s_channels[i].next_send_seq = 1;
        s_channels[i].highest_recv_seq = 0;
        s_channels[i].recv_bits = 0;
        s_channels[i].remote_ack = 0;
        s_channels[i].remote_ack_bits = 0;
    }
    s_initialized = true;
    LOG_NET_INFO("[AckTracker] Initialized %d channels", MAX_CHANNELS);
}

uint32_t NextSendSeq(uint16_t channel) {
    EnsureInitialized();
    if (channel >= MAX_CHANNELS) return 0;
    return s_channels[channel].next_send_seq++;
}

void GetAckState(uint16_t channel, uint32_t* outAck, uint32_t* outAckBits) {
    EnsureInitialized();
    if (channel >= MAX_CHANNELS) {
        if (outAck) *outAck = 0;
        if (outAckBits) *outAckBits = 0;
        return;
    }
    if (outAck) *outAck = s_channels[channel].highest_recv_seq;
    if (outAckBits) *outAckBits = s_channels[channel].recv_bits;
}

bool OnRecvSeq(uint16_t channel, uint32_t seq) {
    EnsureInitialized();
    if (channel >= MAX_CHANNELS) return false;
    if (seq == 0) return false;  // Invalid seq
    
    ChannelState& ch = s_channels[channel];
    
    if (ch.highest_recv_seq == 0) {
        // First packet received on this channel
        ch.highest_recv_seq = seq;
        ch.recv_bits = 0;
        LOG_NET_DEBUG("[AckTracker] ch%u: first recv seq=%u", channel, seq);
        return true;
    }
    
    if (seq == ch.highest_recv_seq) {
        // Exact duplicate
        LOG_NET_DEBUG("[AckTracker] ch%u: dup seq=%u", channel, seq);
        return false;
    }
    
    if (seq > ch.highest_recv_seq) {
        // New highest sequence — shift the bitfield
        uint32_t diff = seq - ch.highest_recv_seq;
        if (diff <= ACK_BITFIELD_SIZE) {
            // Shift existing bits and mark the old highest as received
            ch.recv_bits = (ch.recv_bits << diff) | (1u << (diff - 1));
        } else {
            // Gap too large — old bits are lost
            LOG_NET_WARN("[AckTracker] ch%u: gap %u > %d, bitfield reset (seq %u -> %u)",
                     channel, diff, ACK_BITFIELD_SIZE, ch.highest_recv_seq, seq);
            ch.recv_bits = 0;
        }
        ch.highest_recv_seq = seq;
        return true;
    }
    
    // seq < highest_recv_seq — check if it's in the bitfield window
    uint32_t diff = ch.highest_recv_seq - seq;
    if (diff > ACK_BITFIELD_SIZE) {
        // Too old — treat as duplicate (we can't track it)
        LOG_NET_DEBUG("[AckTracker] ch%u: seq=%u too old (highest=%u, diff=%u)",
                  channel, seq, ch.highest_recv_seq, diff);
        return false;
    }
    
    uint32_t bit = 1u << (diff - 1);
    if (ch.recv_bits & bit) {
        // Already received
        LOG_NET_DEBUG("[AckTracker] ch%u: late dup seq=%u (highest=%u)",
                  channel, seq, ch.highest_recv_seq);
        return false;
    }
    
    // Mark as received (out-of-order)
    ch.recv_bits |= bit;
    LOG_NET_DEBUG("[AckTracker] ch%u: out-of-order seq=%u accepted (highest=%u, diff=%u)",
              channel, seq, ch.highest_recv_seq, diff);
    return true;
}

void OnRecvAck(uint16_t channel, uint32_t ack, uint32_t ackBits) {
    EnsureInitialized();
    if (channel >= MAX_CHANNELS) return;
    ChannelState& ch = s_channels[channel];
    
    // Skip no-op updates (nothing changed)
    if (ack == ch.remote_ack && ackBits == ch.remote_ack_bits) return;
    
    // Only update if the remote ack is newer
    if (ack > ch.remote_ack || (ch.remote_ack == 0 && ack == 0 && ackBits != 0)) {
        LOG_NET_DEBUG("[AckTracker] ch%u: remote ack updated %u -> %u (bits=0x%08X)",
                  channel, ch.remote_ack, ack, ackBits);
        ch.remote_ack = ack;
        ch.remote_ack_bits = ackBits;
    }
}

bool IsAcked(uint16_t channel, uint32_t seq) {
    EnsureInitialized();
    if (channel >= MAX_CHANNELS) return false;
    if (seq == 0) return false;
    
    const ChannelState& ch = s_channels[channel];
    
    if (seq == ch.remote_ack) return true;
    if (ch.remote_ack == 0) return false;
    
    if (seq > ch.remote_ack) return false;  // Not yet acked
    
    uint32_t diff = ch.remote_ack - seq;
    if (diff > ACK_BITFIELD_SIZE) return false;  // Too old to tell
    
    return (ch.remote_ack_bits & (1u << (diff - 1))) != 0;
}

const ChannelState* GetChannelState(uint16_t channel) {
    EnsureInitialized();
    if (channel >= MAX_CHANNELS) return nullptr;
    return &s_channels[channel];
}

void ResetChannel(uint16_t channel) {
    EnsureInitialized();
    if (channel >= MAX_CHANNELS) return;
    s_channels[channel].next_send_seq = 1;
    s_channels[channel].highest_recv_seq = 0;
    s_channels[channel].recv_bits = 0;
    s_channels[channel].remote_ack = 0;
    s_channels[channel].remote_ack_bits = 0;
    LOG_NET_INFO("[AckTracker] Reset channel %u", channel);
}

} // namespace AckTracker
