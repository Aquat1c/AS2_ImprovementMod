/**
 * Alice Senki 2 - ENet Transport Layer
 *
 * Thin wrapper around ENet host/peer management.
 * Owns ENet initialization, host creation, connect/disconnect,
 * send, and event polling. Does NOT own session state—that belongs
 * to session_manager.
 *
 * Thread safety: all calls must be made from the game thread.
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

/// Initiate an outbound connection to the target. Returns the ENet peer or nullptr.
/// The actual connection completes asynchronously via ENET_EVENT_TYPE_CONNECT.
ENetPeer* Transport_Connect(uint32_t ipv4, uint16_t port);

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
