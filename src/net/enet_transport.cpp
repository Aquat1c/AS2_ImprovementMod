/**
 * Alice Senki 2 - ENet Transport Layer (Implementation)
 */

#include <enet/enet.h>   // Must be before anything that pulls in windows.h

#include "net/enet_transport.h"
#include "log_window.h"
#include "rollback/netplay_log.h"

#include <string.h>

namespace Net {

// ============================================================================
// Static state
// ============================================================================

static bool       s_globalInit = false;
static ENetHost*  s_enetHost       = nullptr;

// ============================================================================
// Global init / deinit
// ============================================================================

bool Transport_GlobalInit() {
    if (s_globalInit) return true;
    if (enet_initialize() != 0) {
        LOG_ERROR("[Net] enet_initialize() failed");
        Rollback::NetplayLog_Write("ENET", -1, "enet_initialize FAILED");
        return false;
    }
    s_globalInit = true;
    LOG_INFO("[Net] ENet %d.%d.%d initialized", ENET_VERSION_MAJOR, ENET_VERSION_MINOR, ENET_VERSION_PATCH);
    Rollback::NetplayLog_Write("ENET", -1,
        "ENet initialized: version=%d.%d.%d",
        ENET_VERSION_MAJOR, ENET_VERSION_MINOR, ENET_VERSION_PATCH);
    return true;
}

void Transport_GlobalDeinit() {
    if (!s_globalInit) return;
    Transport_DestroyHost();
    enet_deinitialize();
    s_globalInit = false;
    LOG_INFO("[Net] ENet deinitialized");
    Rollback::NetplayLog_Write("ENET", -1, "ENet deinitialized");
}

// ============================================================================
// Host lifecycle
// ============================================================================

bool Transport_CreateHost(uint16_t port, uint32_t maxPeers) {
    if (!s_globalInit) {
        LOG_ERROR("[Net] Transport_CreateHost: ENet not initialized");
        Rollback::NetplayLog_Write("ENET", -1,
            "CreateHost failed: ENet not initialized");
        return false;
    }
    if (s_enetHost) {
        LOG_WARN("[Net] Transport_CreateHost: Host already active, destroying first");
        Transport_DestroyHost();
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    s_enetHost = enet_host_create(
        port > 0 ? &address : nullptr,  // nullptr = ephemeral port for joiner
        maxPeers,
        NUM_CHANNELS,
        0,  // incoming bandwidth (0 = unlimited)
        0   // outgoing bandwidth (0 = unlimited)
    );

    if (!s_enetHost) {
        LOG_ERROR("[Net] Failed to create ENet host on port %u", port);
        Rollback::NetplayLog_Write("ENET", -1,
            "CreateHost failed: port=%u max_peers=%u channels=%u",
            port, maxPeers, NUM_CHANNELS);
        return false;
    }

    LOG_INFO("[Net] ENet host created on port %u (max peers: %u, channels: %u)",
             port, maxPeers, NUM_CHANNELS);
    Rollback::NetplayLog_Write("ENET", -1,
        "Host created: port=%u max_peers=%u channels=%u",
        port, maxPeers, NUM_CHANNELS);
    return true;
}

void Transport_DestroyHost() {
    if (!s_enetHost) return;

    // Disconnect all connected peers gracefully
    for (size_t i = 0; i < s_enetHost->peerCount; ++i) {
        ENetPeer* peer = &s_enetHost->peers[i];
        if (peer->state == ENET_PEER_STATE_CONNECTED ||
            peer->state == ENET_PEER_STATE_CONNECTING) {
            enet_peer_disconnect_now(peer, 0);
        }
    }

    enet_host_destroy(s_enetHost);
    s_enetHost = nullptr;
    LOG_INFO("[Net] ENet host destroyed");
    Rollback::NetplayLog_Write("ENET", -1, "Host destroyed");
}

bool Transport_IsHostActive() {
    return s_enetHost != nullptr;
}

// ============================================================================
// Connection
// ============================================================================

ENetPeer* Transport_Connect(uint32_t ipv4, uint16_t port) {
    if (!s_enetHost) {
        LOG_ERROR("[Net] Transport_Connect: No host active");
        Rollback::NetplayLog_Write("ENET", -1, "Connect failed: no active host");
        return nullptr;
    }

    ENetAddress address;
    address.host = ipv4;  // Already in network byte order
    address.port = port;

    ENetPeer* peer = enet_host_connect(s_enetHost, &address, NUM_CHANNELS, 0);
    if (!peer) {
        LOG_ERROR("[Net] enet_host_connect failed (port %u)", port);
        Rollback::NetplayLog_Write("ENET", -1,
            "Connect failed: %u.%u.%u.%u:%u",
            (ipv4) & 0xFF, (ipv4 >> 8) & 0xFF, (ipv4 >> 16) & 0xFF, (ipv4 >> 24) & 0xFF,
            port);
        return nullptr;
    }

    LOG_INFO("[Net] Connecting to %u.%u.%u.%u:%u...",
             (ipv4) & 0xFF, (ipv4 >> 8) & 0xFF, (ipv4 >> 16) & 0xFF, (ipv4 >> 24) & 0xFF,
             port);
    Rollback::NetplayLog_Write("ENET", -1,
        "Connecting: peer=%p target=%u.%u.%u.%u:%u channels=%u",
        peer,
        (ipv4) & 0xFF, (ipv4 >> 8) & 0xFF, (ipv4 >> 16) & 0xFF, (ipv4 >> 24) & 0xFF,
        port,
        NUM_CHANNELS);
    return peer;
}

void Transport_DisconnectPeer(ENetPeer* peer, uint32_t data) {
    if (!peer) return;
    enet_peer_disconnect(peer, data);
    LOG_INFO("[Net] Disconnecting peer (data=%u)", data);
    Rollback::NetplayLog_Write("ENET", -1,
        "Disconnect peer=%p data=%u", peer, data);
}

void Transport_ForceDisconnectPeer(ENetPeer* peer) {
    if (!peer) return;
    enet_peer_disconnect_now(peer, 0);
    LOG_INFO("[Net] Force-disconnected peer");
    Rollback::NetplayLog_Write("ENET", -1,
        "Force disconnect peer=%p", peer);
}

// ============================================================================
// Sending
// ============================================================================

bool Transport_Send(ENetPeer* peer, uint8_t channel, const void* data, size_t length, bool reliable) {
    if (!peer || !data || length == 0) return false;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    if (!reliable && channel == CHANNEL_GAMEPLAY) {
        flags = 0;  // Sequenced unreliable for gameplay
    }

    ENetPacket* packet = enet_packet_create(data, length, flags);
    if (!packet) {
        LOG_ERROR("[Net] Failed to create ENet packet (len=%zu)", length);
        Rollback::NetplayLog_Write("ENET", -1,
            "Packet create failed: ch=%u len=%zu reliable=%d",
            channel, length, reliable ? 1 : 0);
        return false;
    }

    if (enet_peer_send(peer, channel, packet) < 0) {
        LOG_ERROR("[Net] enet_peer_send failed (channel=%u, len=%zu)", channel, length);
        enet_packet_destroy(packet);
        Rollback::NetplayLog_Write("ENET", -1,
            "enet_peer_send failed: peer=%p ch=%u len=%zu reliable=%d",
            peer, channel, length, reliable ? 1 : 0);
        return false;
    }

    const char* packetName = "Raw";
    if (length >= sizeof(PacketType)) {
        packetName = PacketTypeName(ReadPacketType(data));
    }

    Rollback::NetplayLog_Verbose("ENET", -1,
        "Queued send: peer=%p ch=%u type=%s len=%zu reliable=%d flags=0x%X",
        peer,
        channel,
        packetName,
        length,
        reliable ? 1 : 0,
        flags);

    return true;
}

bool Transport_SendTyped(ENetPeer* peer, uint8_t channel, PacketType type,
                         const void* payload, size_t payloadLen, bool reliable) {
    size_t totalLen = sizeof(PacketType) + payloadLen;
    if (totalLen > MAX_PACKET_SIZE) {
        LOG_ERROR("[Net] Packet too large: %zu > %d", totalLen, MAX_PACKET_SIZE);
        return false;
    }

    uint8_t buf[MAX_PACKET_SIZE];
    memcpy(buf, &type, sizeof(PacketType));
    if (payload && payloadLen > 0) {
        memcpy(buf + sizeof(PacketType), payload, payloadLen);
    }

    return Transport_Send(peer, channel, buf, totalLen, reliable);
}

// ============================================================================
// Polling
// ============================================================================

int Transport_Service(uint32_t timeoutMs, ENetEvent* outEvent) {
    if (!s_enetHost || !outEvent) return -1;
    const int result = enet_host_service(s_enetHost, outEvent, timeoutMs);
    if (result < 0) {
        Rollback::NetplayLog_Write("ENET", -1,
            "enet_host_service failed (timeout=%u)", timeoutMs);
    }
    return result;
}

void Transport_Flush() {
    if (s_enetHost) {
        enet_host_flush(s_enetHost);
    }
}

// ============================================================================
// Queries
// ============================================================================

ENetHost* Transport_GetHost() {
    return s_enetHost;
}

ENetPeer* Transport_GetPeer() {
    if (!s_enetHost) return nullptr;
    for (size_t i = 0; i < s_enetHost->peerCount; ++i) {
        if (s_enetHost->peers[i].state == ENET_PEER_STATE_CONNECTED) {
            return &s_enetHost->peers[i];
        }
    }
    return nullptr;
}

float Transport_GetPeerRTT(ENetPeer* peer) {
    if (!peer) return 0.0f;
    return (float)peer->roundTripTime;
}

float Transport_GetPeerLoss(ENetPeer* peer) {
    if (!peer) return 0.0f;
    return (float)peer->packetLoss / (float)ENET_PEER_PACKET_LOSS_SCALE * 100.0f;
}

} // namespace Net
