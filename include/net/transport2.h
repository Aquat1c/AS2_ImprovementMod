/**
 * Alice Senki 2 - transport2: ENet worker (re0.7 M3, master plan §2.2)
 *
 * Replaces network_thread as the owner of the ENet host/service loop for the
 * single-peer gameplay session. One worker thread owns the ENet host and
 * services it at a 1-2 ms cadence; the game thread enqueues outbound sends
 * and drains inbound events from Session_Update(). The game thread never
 * touches ENet objects.
 *
 * New over network_thread:
 *   - protocol_silence_ms (INV-14): time since ANY valid datagram from the
 *     peer — ENet commands (acks/pings/fragments) AND authenticated autopunch
 *     keepalives accepted by the raw-socket intercept. This is the sole
 *     liveness input for the connection supervisor.
 *   - Liveness anchor (QOH99 lesson 10): the pre-establishment silence budget
 *     is anchored at the first worker poll after StartHost/StartJoin, never
 *     process start.
 *   - Busy refusal (edge C-5/C-6): an inbound ENet connect while a peer is
 *     already active (or while our own outbound connect is pending) is
 *     force-refused with DisconnectReason::Busy; the existing peer/attempt is
 *     unaffected.
 *
 * NAT layers (hole punch, autopunch keepalive/rebind healing, fault
 * injection) stay in enet_transport helpers and are driven from the worker
 * exactly as before. Spectator sidecar keeps the Transport_*ForHost helpers.
 */

#pragma once

#include "net/protocol.h"

#include <stdint.h>
#include <stddef.h>
#include <windows.h>

namespace Net {

enum class Transport2EventType : uint8_t {
    Connected,
    Disconnected,
    PacketReceived,
    WorkerError,
};

struct Transport2Event {
    Transport2EventType type;
    uint32_t            session_token;
    uintptr_t           peer_token;
    DWORD               transport_tick_ms;

    // PacketReceived
    uint8_t             channel_id;
    size_t              packet_len;
    uint8_t             packet_data[MAX_PACKET_SIZE];

    // Disconnected
    uint32_t            disconnect_data;

    // WorkerError
    char                error_text[96];
};

struct Transport2Stats {
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
    // Protocol-level inbound silence (INV-14): ms since any valid datagram
    // from the peer — ENet commands (acks/pings included) or authenticated
    // autopunch keepalives. While no peer is connected, measured from the
    // liveness anchor (first worker poll after StartHost/StartJoin) once
    // anything was ever received; 0xFFFFFFFF = nothing ever received.
    uint32_t protocol_silence_ms;
};

bool Transport2_Init();
void Transport2_Shutdown();

bool Transport2_StartHost(uint32_t session_token, uint16_t listen_port,
                          bool enable_autopunch,
                          const char* punch_relay_host,
                          uint16_t punch_relay_port);
bool Transport2_StartJoin(uint32_t session_token, uint16_t listen_port,
                          const char* target_host, uint16_t target_port,
                          bool send_hole_punch,
                          const char* punch_relay_host,
                          uint16_t punch_relay_port);

bool Transport2_SendPacket(uint32_t session_token, uint8_t channel,
                           PacketType type, const void* payload,
                           size_t payload_len, bool reliable);

void Transport2_RequestDisconnect(uint32_t session_token, uint32_t data, bool force);
void Transport2_RequestDestroyHost(uint32_t session_token);
void Transport2_ClearQueues(uint32_t session_token);

bool Transport2_TryPopEvent(Transport2Event* out);
void Transport2_GetStats(Transport2Stats* out);

} // namespace Net
