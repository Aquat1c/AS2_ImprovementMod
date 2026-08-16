/**
 * Alice Senki 2 - Dedicated Network Service Thread
 *
 * Owns ENet host/service/send/receive on a background worker thread.
 * Game-thread systems enqueue outbound packets and drain inbound events.
 */

#pragma once

#include "net/protocol.h"

#include <stdint.h>
#include <stddef.h>
#include <windows.h>

namespace Net {

enum class NetworkThreadEventType : uint8_t {
    Connected,
    Disconnected,
    PacketReceived,
    WorkerError,
};

struct NetworkThreadEvent {
    NetworkThreadEventType type;
    uint32_t               session_token;
    uintptr_t              peer_token;
    DWORD                  transport_tick_ms;

    // PacketReceived
    uint8_t                channel_id;
    size_t                 packet_len;
    uint8_t                packet_data[MAX_PACKET_SIZE];

    // Disconnected
    uint32_t               disconnect_data;

    // WorkerError
    char                   error_text[96];
};

struct NetworkThreadStats {
    bool     worker_running;
    bool     host_active;
    bool     peer_connected;
    float    rtt_ms;
    float    rtt_variance_ms;
    uint32_t packets_sent;
    uint32_t packets_lost;
    uint32_t inbound_queue_depth;
    uint32_t outbound_queue_depth;
    uint32_t inbound_drop_count;
    uint32_t outbound_drop_count;
    DWORD    last_inbound_packet_tick_ms;
    DWORD    last_outbound_packet_tick_ms;
    DWORD    last_service_tick_ms;
    // ENet-protocol-level inbound silence (acks/pings count, not just app
    // packets). This is the true liveness signal: an idle-but-healthy link
    // shows ~0 here while app-level silence grows. 0xFFFFFFFF = no peer.
    DWORD    enet_silence_ms;
};

bool NetworkThread_Init();
void NetworkThread_Shutdown();

bool NetworkThread_StartHost(uint32_t session_token, uint16_t listen_port,
                             bool enable_autopunch,
                             const char* punch_relay_host,
                             uint16_t punch_relay_port);
bool NetworkThread_StartJoin(uint32_t session_token, uint16_t listen_port,
                             const char* target_host, uint16_t target_port,
                             bool send_hole_punch,
                             const char* punch_relay_host,
                             uint16_t punch_relay_port);

bool NetworkThread_SendPacket(uint32_t session_token, uint8_t channel,
                              PacketType type, const void* payload,
                              size_t payload_len, bool reliable);

void NetworkThread_RequestDisconnect(uint32_t session_token, uint32_t data, bool force);
void NetworkThread_RequestDestroyHost(uint32_t session_token);
void NetworkThread_ClearQueues(uint32_t session_token);

bool NetworkThread_TryPopEvent(NetworkThreadEvent* out);
void NetworkThread_GetStats(NetworkThreadStats* out);

} // namespace Net
