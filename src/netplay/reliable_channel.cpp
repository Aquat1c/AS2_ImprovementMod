/**
 * Alice Senki 2 - Reliable Channel Implementation
 *
 * Reliable ordered delivery with retransmission over UDP.
 */

#include "reliable_channel.h"
#include "ack_tracker.h"
#include "log_window.h"
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ReliableChannel {

static PendingSend s_sendQueue[MAX_PENDING_SENDS];
static Stats s_stats;
static bool s_initialized = false;

// RTT estimate from SessionManager, used for adaptive retransmit timing
static float s_rttMs = 0.0f;

// Dedup window per channel
static uint32_t s_recvHighest[4] = {};
static uint32_t s_recvBits[4] = {};

// Compute retransmit interval based on RTT and retry count
static uint32_t ComputeRetransmitInterval(int retryCount) {
    // Base: 1.5x RTT or default minimum, whichever is larger
    uint32_t baseMs;
    if (s_rttMs > 0.0f && s_rttMs < 10.0f) {
        // LAN condition: very fast retransmits
        baseMs = 20;
    } else {
        baseMs = (s_rttMs > 0.0f) ? (uint32_t)(s_rttMs * 1.5f) : RETRANSMIT_MS_DEFAULT;
        if (baseMs < RETRANSMIT_MS_MIN) baseMs = RETRANSMIT_MS_MIN;
    }

    // Exponential backoff: double every 4 retries, capped at max
    uint32_t factor = 1u << (retryCount / 4);
    uint32_t interval = baseMs * factor;
    if (interval > RETRANSMIT_MS_MAX) interval = RETRANSMIT_MS_MAX;
    return interval;
}

void Init() {
    memset(s_sendQueue, 0, sizeof(s_sendQueue));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_recvHighest, 0, sizeof(s_recvHighest));
    memset(s_recvBits, 0, sizeof(s_recvBits));
    s_rttMs = 0.0f;
    s_initialized = true;
    LOG_NET_INFO("[ReliableChannel] Initialized (queue=%d, max_retries=%d)",
             MAX_PENDING_SENDS, MAX_RETRIES);
}

static int FindFreeSlot() {
    for (int i = 0; i < MAX_PENDING_SENDS; i++) {
        if (!s_sendQueue[i].active) return i;
    }
    return -1;
}

static bool RawSend(SOCKET sock, const sockaddr_in* peer, const void* data, int len) {
    if (sock == INVALID_SOCKET || !peer || !data || len <= 0) return false;
    int sent = sendto(sock, (const char*)data, len, 0, (const sockaddr*)peer, sizeof(sockaddr_in));
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return false; // Queue full, retry soon
        }
        LOG_NET_WARN("[ReliableChannel] sendto failed: WSA error %d", err);
        return false;
    }
    return true;
}

uint32_t Send(SOCKET sock, const sockaddr_in* peer,
              PacketCodec::PacketType type, uint16_t channel,
              const void* payload, int payloadLen,
              uint64_t sessionId, uint32_t connectionId) {
    if (!s_initialized) Init();
    
    int slot = FindFreeSlot();
    if (slot < 0) {
        LOG_NET_ERROR("[ReliableChannel] Send queue full (%d slots), dropping %s packet",
                  MAX_PENDING_SENDS, PacketCodec::GetPacketTypeName(type));
        s_stats.packets_dropped++;
        return 0;
    }
    
    // Build the packet
    PacketCodec::ModNetHeader hdr;
    PacketCodec::InitHeader(&hdr, type, channel,
                            PacketCodec::FLAG_RELIABLE | PacketCodec::FLAG_ORDERED);
    hdr.session_id = sessionId;
    hdr.connection_id = connectionId;
    hdr.seq = AckTracker::NextSendSeq(channel);
    
    uint32_t ack, ackBits;
    AckTracker::GetAckState(channel, &ack, &ackBits);
    hdr.ack = ack;
    hdr.ack_bits = ackBits;
    
    PendingSend& ps = s_sendQueue[slot];
    ps.packet_len = PacketCodec::Encode(&hdr, payload, payloadLen,
                                         ps.packet_data, sizeof(ps.packet_data));
    if (ps.packet_len <= 0) {
        LOG_NET_ERROR("[ReliableChannel] Failed to encode %s packet", PacketCodec::GetPacketTypeName(type));
        return 0;
    }
    
    ps.active = true;
    ps.seq = hdr.seq;
    ps.first_send_ms = GetTickCount();
    ps.last_send_ms = ps.first_send_ms;
    ps.retry_count = 0;
    ps.channel = channel;
    
    // Send immediately
    if (!RawSend(sock, peer, ps.packet_data, ps.packet_len)) {
        ps.last_send_ms = 0; // Failed (likely WOULDBLOCK), force retry next pump
    }
    s_stats.packets_sent++;
    s_stats.pending_count++;
    
    LOG_NET_DEBUG("[ReliableChannel] Sent %s seq=%u ch=%u (%d bytes)",
             PacketCodec::GetPacketTypeName(type), hdr.seq, channel, ps.packet_len);
    
    return hdr.seq;
}

void ProcessAcks(uint16_t channel, uint32_t ack, uint32_t ackBits) {
    AckTracker::OnRecvAck(channel, ack, ackBits);
    
    // Remove acked packets from the queue
    for (int i = 0; i < MAX_PENDING_SENDS; i++) {
        PendingSend& ps = s_sendQueue[i];
        if (!ps.active || ps.channel != channel) continue;
        
        if (AckTracker::IsAcked(channel, ps.seq)) {
            LOG_NET_DEBUG("[ReliableChannel] Acked seq=%u ch=%u (retries=%d)",
                      ps.seq, channel, ps.retry_count);
            ps.active = false;
            s_stats.packets_acked++;
            s_stats.pending_count--;
        }
    }
}

int PumpRetransmits(SOCKET sock, const sockaddr_in* peer) {
    if (!s_initialized) return 0;
    if (sock == INVALID_SOCKET || !peer) return 0;
    
    uint32_t now = GetTickCount();
    int retransmitted = 0;
    
    for (int i = 0; i < MAX_PENDING_SENDS; i++) {
        PendingSend& ps = s_sendQueue[i];
        if (!ps.active) continue;
        
        // Check if acked (in case ProcessAcks wasn't called)
        if (AckTracker::IsAcked(ps.channel, ps.seq)) {
            ps.active = false;
            s_stats.packets_acked++;
            s_stats.pending_count--;
            continue;
        }
        
        // Check max retries
        if (ps.retry_count >= MAX_RETRIES) {
            LOG_NET_WARN("[ReliableChannel] Packet seq=%u ch=%u exceeded max retries (%d), dropping",
                     ps.seq, ps.channel, MAX_RETRIES);
            ps.active = false;
            s_stats.packets_dropped++;
            s_stats.pending_count--;
            continue;
        }
        
        // Check retransmit interval (RTT-adaptive with exponential backoff)
        uint32_t interval = ComputeRetransmitInterval(ps.retry_count);
        uint32_t elapsed = now - ps.last_send_ms;
        if (elapsed >= interval) {
            // Update ack/ack_bits in the packet before resending
            // (The header is at the start of packet_data)
            PacketCodec::ModNetHeader* pHdr = (PacketCodec::ModNetHeader*)ps.packet_data;
            uint32_t newAck, newAckBits;
            AckTracker::GetAckState(ps.channel, &newAck, &newAckBits);
            pHdr->ack = newAck;
            pHdr->ack_bits = newAckBits;
            pHdr->send_timestamp_ms = PacketCodec::GetTimestampMs();
            // Recompute header CRC
            pHdr->header_crc16 = 0;
            pHdr->header_crc16 = PacketCodec::Crc16(pHdr, sizeof(PacketCodec::ModNetHeader));

            if (RawSend(sock, peer, ps.packet_data, ps.packet_len)) {
                ps.last_send_ms = now;
                ps.retry_count++;
                s_stats.packets_resent++;
                retransmitted++;
                LOG_NET_DEBUG("[ReliableChannel] Retransmit seq=%u ch=%u retry=%d/%d (elapsed=%ums)",
                          ps.seq, ps.channel, ps.retry_count, MAX_RETRIES, elapsed);
            }
        }
    }

    return retransmitted;
}

bool AcceptIncoming(uint16_t channel, uint32_t seq) {
    if (channel >= 4 || seq == 0) return false;
    
    if (s_recvHighest[channel] == 0) {
        s_recvHighest[channel] = seq;
        s_recvBits[channel] = 0;
        return true;
    }
    
    if (seq == s_recvHighest[channel]) {
        s_stats.duplicates_filtered++;
        LOG_NET_DEBUG("[ReliableChannel] Dedup ch=%u seq=%u (exact match)", channel, seq);
        return false;
    }
    
    if (seq > s_recvHighest[channel]) {
        uint32_t diff = seq - s_recvHighest[channel];
        if (diff > 1) {
            LOG_NET_DEBUG("[ReliableChannel] Accept ch=%u seq=%u (prev=%u gap=%u)",
                         channel, seq, s_recvHighest[channel], diff);
        }
        if (diff <= 32) {
            s_recvBits[channel] = (s_recvBits[channel] << diff) | (1u << (diff - 1));
        } else {
            s_recvBits[channel] = 0;
        }
        s_recvHighest[channel] = seq;
        return true;
    }
    
    uint32_t diff = s_recvHighest[channel] - seq;
    if (diff > 32) {
        s_stats.duplicates_filtered++;
        return false;
    }
    
    uint32_t bit = 1u << (diff - 1);
    if (s_recvBits[channel] & bit) {
        s_stats.duplicates_filtered++;
        return false;
    }
    
    s_recvBits[channel] |= bit;
    return true;
}

void GetStats(Stats* out) {
    if (out) *out = s_stats;
}

void Reset() {
    memset(s_sendQueue, 0, sizeof(s_sendQueue));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_recvHighest, 0, sizeof(s_recvHighest));
    memset(s_recvBits, 0, sizeof(s_recvBits));
    s_rttMs = 0.0f;
    LOG_NET_INFO("[ReliableChannel] Reset");
}

int GetPendingCount() {
    return s_stats.pending_count;
}

void SetRttMs(float rttMs) {
    s_rttMs = (rttMs > 0.0f) ? rttMs : 0.0f;
}

} // namespace ReliableChannel
