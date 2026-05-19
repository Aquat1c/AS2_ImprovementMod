/**
 * Alice Senki 2 - ENet Transport Layer
 *
 * Thin wrapper around ENet host/peer management.
 * Owns ENet initialization, host creation, connect/disconnect,
 * send, and event polling. Does NOT own session state—that belongs
 * to session_manager.
 *
 * Thread safety: calls must be serialized on the owning transport thread.
 * Current architecture: Session's dedicated network worker thread owns these.
 */

#pragma once

#include "net/protocol.h"
#include <stdint.h>

// Forward-declare ENet types to avoid leaking enet.h into every consumer
struct _ENetHost;
struct _ENetPeer;
struct _ENetEvent;
typedef struct _ENetHost  ENetHost;
typedef struct _ENetPeer  ENetPeer;
typedef struct _ENetEvent ENetEvent;

namespace Net {

// ============================================================================
// ENet global init / deinit (call once at mod startup / shutdown)
// ============================================================================

bool Transport_GlobalInit();
void Transport_GlobalDeinit();

// ============================================================================
// Host lifecycle
// ============================================================================

/// Create a host listening on the given port. Returns true on success.
/// For a joining peer, pass port = 0 to let the OS pick an ephemeral port.
bool Transport_CreateHost(uint16_t port, uint32_t maxPeers = 1);

/// Destroy the host and disconnect all peers.
void Transport_DestroyHost();

/// Returns true if a host is currently active.
bool Transport_IsHostActive();

// ============================================================================
// Connection
// ============================================================================

/// Initiate an outbound connection to the target host/IP. Returns ENet peer or nullptr.
/// The actual connection completes asynchronously via ENET_EVENT_TYPE_CONNECT.
ENetPeer* Transport_Connect(const char* host, uint16_t port);

/// Gracefully disconnect a peer. ENet will flush and then emit DISCONNECT event.
void Transport_DisconnectPeer(ENetPeer* peer, uint32_t data = 0);

/// Force-disconnect a peer immediately (no flush).
void Transport_ForceDisconnectPeer(ENetPeer* peer);

// ============================================================================
// Sending
// ============================================================================

/// Send a packet on a specific channel.
/// If reliable is true, ENet guarantees delivery (channel 0 control).
/// If reliable is false, ENet sends unreliable (sequenced on the channel).
/// The caller provides the PacketType + payload; this function prepends nothing.
///
/// Returns true if the packet was queued successfully.
bool Transport_Send(ENetPeer* peer, uint8_t channel, const void* data, size_t length, bool reliable);

/// Convenience: build a [PacketType | payload] buffer and send.
bool Transport_SendTyped(ENetPeer* peer, uint8_t channel, PacketType type,
                         const void* payload, size_t payloadLen, bool reliable);

/// Legacy hook for explicit hole-punch bursts.
/// Sends a small UDP burst to the target endpoint to assist NAT pinhole setup.
bool Transport_SendHolePunchBurst(const char* host, uint16_t port,
                                  int burstCount, uint32_t intervalMs);

/// Return the actual UDP port bound by the current ENet host.
bool Transport_GetBoundPort(uint16_t* outPort);
bool Transport_GetHostBoundPort(ENetHost* host, uint16_t* outPort);

/// Start/stop the autopunch-compatible UDP rendezvous helper.
/// Calls must stay on the transport owner thread.
void Transport_AutopunchStart(const char* relayHost, uint16_t relayPort,
                              uint16_t localPort,
                              const char* targetHost, uint16_t targetPort);
void Transport_AutopunchStop(const char* reason);
void Transport_AutopunchService(uint32_t nowMs, bool peerConnected);

bool Transport_SendHolePunchBurstForHost(ENetHost* enetHost,
                                         const char* host, uint16_t port,
                                         int burstCount, uint32_t intervalMs);
void Transport_AutopunchStartForHost(ENetHost* enetHost,
                                     const char* logLabel,
                                     const char* relayHost,
                                     uint16_t relayPort,
                                     uint16_t localPort,
                                     const char* targetHost,
                                     uint16_t targetPort);
void Transport_AutopunchStopForHost(ENetHost* enetHost, const char* reason);
void Transport_AutopunchServiceForHost(ENetHost* enetHost,
                                       uint32_t nowMs,
                                       bool peerConnected);

// ============================================================================
// Polling
// ============================================================================

/// Service ENet for up to timeoutMs milliseconds. Fills outEvent if an event
/// occurred. Returns > 0 if an event was dispatched, 0 if no event, < 0 on error.
int Transport_Service(uint32_t timeoutMs, ENetEvent* outEvent);

/// Flush any queued outgoing packets immediately without waiting for service.
void Transport_Flush();

// ============================================================================
// Queries
// ============================================================================

/// Get the ENet host pointer (for advanced use). Returns nullptr if not active.
ENetHost* Transport_GetHost();

/// Get the first connected peer, or nullptr. For single-peer sessions.
ENetPeer* Transport_GetPeer();

/// Get RTT for a peer in milliseconds (from ENet's built-in measurement).
float Transport_GetPeerRTT(ENetPeer* peer);

/// Get packet loss percentage for a peer.
float Transport_GetPeerLoss(ENetPeer* peer);

} // namespace Net
