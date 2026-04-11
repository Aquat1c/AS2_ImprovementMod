#include <enet/enet.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "net/spectator_manager.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <algorithm>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

namespace {

using namespace Net;

struct PeerState {
    ENetPeer* peer;
    bool handshake_complete;
    bool needs_full_sync;
    int32_t next_rb_frame;
    int32_t last_playback_rb_frame;
    bool fast_forward_requested;
    bool hard_sync_requested;
    uint16_t advertised_listen_port;
    DWORD connected_at_ms;
    DWORD last_status_at_ms;
    char nickname[24];
};

static bool s_initialized = false;
static bool s_enabled = false;
static ENetHost* s_server = nullptr;
static uint16_t s_listenPort = 10701;
static uint32_t s_activeMatchId = 0;
static char s_redirectEndpoint[96] = "";
static char s_status[128] = "Spectator server disabled.";
static std::unordered_map<ENetPeer*, PeerState> s_peers;

static void CopyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, dstSize, src, _TRUNCATE);
}

static void SetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static int CountHandshakenPeers() {
    int count = 0;
    for (const auto& entry : s_peers) {
        if (entry.second.handshake_complete) {
            count++;
        }
    }
    return count;
}

static PeerState* FindPeer(uintptr_t peer_id) {
    for (auto& entry : s_peers) {
        if ((uintptr_t)entry.first == peer_id) {
            return &entry.second;
        }
    }
    return nullptr;
}

static bool SendTyped(ENetPeer* peer,
                      uint8_t channel,
                      Spectator::PacketType type,
                      const void* payload,
                      size_t payloadLen,
                      bool reliable) {
    if (!peer) {
        return false;
    }
    if (sizeof(Spectator::PacketType) + payloadLen > (size_t)Spectator::MAX_PACKET_SIZE) {
        return false;
    }

    uint8_t buffer[Spectator::MAX_PACKET_SIZE] = {};
    memcpy(buffer, &type, sizeof(type));
    if (payload && payloadLen > 0) {
        memcpy(buffer + sizeof(type), payload, payloadLen);
    }

    const uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(buffer, sizeof(type) + payloadLen, flags);
    if (!packet) {
        return false;
    }
    if (enet_peer_send(peer, channel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

static void DestroyServer(const char* reason) {
    if (!s_server) {
        s_peers.clear();
        return;
    }

    for (size_t index = 0; index < s_server->peerCount; index++) {
        ENetPeer* peer = &s_server->peers[index];
        if (peer->state == ENET_PEER_STATE_CONNECTED ||
            peer->state == ENET_PEER_STATE_CONNECTING) {
            enet_peer_disconnect_now(peer, 0);
        }
    }

    enet_host_destroy(s_server);
    s_server = nullptr;
    s_peers.clear();
    SetStatus("Spectator server offline%s%s",
        reason ? ": " : "",
        reason ? reason : "");
    LOG_NETPLAY(LOG_INFO,
        "[SpectatorMgr] Server destroyed reason=%s",
        reason && reason[0] ? reason : "unspecified");
}

static bool EnsureServer() {
    if (!s_enabled) {
        DestroyServer("disabled");
        return false;
    }
    if (s_server) {
        return true;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = s_listenPort;

    s_server = enet_host_create(&address, 8, Spectator::NUM_CHANNELS, 0, 0);
    if (!s_server) {
        SetStatus("Failed to bind spectator port %u", s_listenPort);
        LOG_NETPLAY(LOG_WARNING,
            "[SpectatorMgr] Failed to create spectator host on port %u",
            s_listenPort);
        return false;
    }

    SetStatus("Spectator server listening on %u", s_listenPort);
    LOG_NETPLAY(LOG_INFO,
        "[SpectatorMgr] Spectator server listening on port %u",
        s_listenPort);
    return true;
}

static void HandleHello(ENetPeer* peer, const Spectator::HelloPayload* payload) {
    if (!peer || !payload) {
        return;
    }

    auto it = s_peers.find(peer);
    if (it == s_peers.end()) {
        return;
    }

    if (payload->protocol_version != Spectator::PROTOCOL_VERSION) {
        Spectator::DisconnectPayload disconnect{};
        disconnect.reason_code = 1;
        CopyText(disconnect.message, sizeof(disconnect.message), "spectator protocol mismatch");
        SendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &disconnect,
            sizeof(disconnect),
            true);
        enet_peer_disconnect_later(peer, 0);
        LOG_NETPLAY(LOG_WARNING,
            "[SpectatorMgr] Reject peer=0x%p reason=protocol_mismatch",
            peer);
        return;
    }

    if (payload->requested_match_id != 0 &&
        payload->requested_match_id != s_activeMatchId) {
        Spectator::DisconnectPayload disconnect{};
        disconnect.reason_code = 2;
        CopyText(disconnect.message, sizeof(disconnect.message), "requested match not active");
        SendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &disconnect,
            sizeof(disconnect),
            true);
        enet_peer_disconnect_later(peer, 0);
        LOG_NETPLAY(LOG_INFO,
            "[SpectatorMgr] Reject peer=0x%p reason=requested_match_not_active requested=0x%08X active=0x%08X",
            peer,
            payload->requested_match_id,
            s_activeMatchId);
        return;
    }

    if (CountHandshakenPeers() >= 8) {
        if (s_redirectEndpoint[0]) {
            Spectator::RedirectPayload redirect{};
            CopyText(redirect.endpoint, sizeof(redirect.endpoint), s_redirectEndpoint);
            SendTyped(peer,
                Spectator::CHANNEL_CONTROL,
                Spectator::PacketType::Redirect,
                &redirect,
                sizeof(redirect),
                true);
            LOG_NETPLAY(LOG_INFO,
                "[SpectatorMgr] Redirect peer=0x%p endpoint=%s",
                peer,
                s_redirectEndpoint);
        } else {
            Spectator::DisconnectPayload disconnect{};
            disconnect.reason_code = 3;
            CopyText(disconnect.message, sizeof(disconnect.message), "spectator capacity reached");
            SendTyped(peer,
                Spectator::CHANNEL_CONTROL,
                Spectator::PacketType::Disconnect,
                &disconnect,
                sizeof(disconnect),
                true);
            LOG_NETPLAY(LOG_INFO,
                "[SpectatorMgr] Reject peer=0x%p reason=capacity_reached",
                peer);
        }
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    PeerState& state = it->second;
    state.handshake_complete = true;
    state.needs_full_sync = true;
    state.next_rb_frame = 0;
    state.last_playback_rb_frame = -1;
    state.fast_forward_requested = false;
    state.hard_sync_requested = false;
    state.advertised_listen_port = payload->client_listen_port;
    CopyText(state.nickname, sizeof(state.nickname), payload->nickname);

    Spectator::HelloAckPayload ack{};
    ack.protocol_version = Spectator::PROTOCOL_VERSION;
    ack.server_listen_port = s_listenPort;
    ack.match_id = s_activeMatchId;
    ack.match_state = (s_activeMatchId != 0)
        ? Spectator::MATCH_STATE_ACTIVE
        : Spectator::MATCH_STATE_IDLE;

    SendTyped(peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::HelloAck,
        &ack,
        sizeof(ack),
        true);

    LOG_NETPLAY(LOG_INFO,
        "[SpectatorMgr] Spectator admitted: peer=0x%p nick='%s' listen_port=%u match_id=0x%08X match_state=%u",
        peer,
        state.nickname,
        state.advertised_listen_port,
        ack.match_id,
        ack.match_state);
}

static void HandleClientStatus(ENetPeer* peer, const Spectator::ClientStatusPayload* payload) {
    if (!peer || !payload) {
        return;
    }

    auto it = s_peers.find(peer);
    if (it == s_peers.end()) {
        return;
    }

    PeerState& state = it->second;
    state.last_playback_rb_frame = payload->playback_rb_frame;
    state.fast_forward_requested = (payload->flags & Spectator::CLIENT_STATUS_FLAG_FAST_FORWARD) != 0;
    state.hard_sync_requested = (payload->flags & Spectator::CLIENT_STATUS_FLAG_HARD_SYNC) != 0;
    state.last_status_at_ms = GetTickCount();
}

} // namespace

namespace Net {

void SpectatorManager_Init() {
    if (s_initialized) {
        return;
    }
    s_enabled = false;
    s_server = nullptr;
    s_activeMatchId = 0;
    s_status[0] = '\0';
    SetStatus("Spectator server disabled.");
    s_initialized = true;
}

void SpectatorManager_Shutdown() {
    if (!s_initialized) {
        return;
    }
    DestroyServer("shutdown");
    s_initialized = false;
}

void SpectatorManager_SetEnabled(bool enabled) {
    s_enabled = enabled;
    if (!enabled) {
        DestroyServer("disabled");
    }
}

bool SpectatorManager_SetListenPort(uint16_t port) {
    if (port == 0) {
        return false;
    }
    if (s_listenPort == port) {
        return true;
    }

    s_listenPort = port;
    if (s_server) {
        DestroyServer("rebind");
    }
    return EnsureServer();
}

void SpectatorManager_SetRedirectEndpoint(const char* endpoint) {
    CopyText(s_redirectEndpoint, sizeof(s_redirectEndpoint), endpoint);
}

void SpectatorManager_BeginMatch(uint32_t match_id) {
    s_activeMatchId = match_id;
    for (auto& entry : s_peers) {
        entry.second.needs_full_sync = true;
        entry.second.next_rb_frame = 0;
    }
    LOG_NETPLAY(LOG_INFO,
        "[SpectatorMgr] Active match begin: match_id=0x%08X peers=%d",
        match_id,
        CountHandshakenPeers());
}

void SpectatorManager_EndMatch(const char* reason) {
    s_activeMatchId = 0;
    if (reason && reason[0]) {
        SetStatus("Spectator server idle: %s", reason);
    } else {
        SetStatus("Spectator server idle.");
    }
    LOG_NETPLAY(LOG_INFO,
        "[SpectatorMgr] Active match end reason=%s",
        reason && reason[0] ? reason : "unspecified");
}

void SpectatorManager_FrameUpdate() {
    if (!s_initialized) {
        return;
    }
    if (!EnsureServer()) {
        return;
    }

    ENetEvent event{};
    while (enet_host_service(s_server, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                PeerState state{};
                state.peer = event.peer;
                state.handshake_complete = false;
                state.needs_full_sync = false;
                state.next_rb_frame = 0;
                state.last_playback_rb_frame = -1;
                state.fast_forward_requested = false;
                state.hard_sync_requested = false;
                state.advertised_listen_port = 0;
                state.connected_at_ms = GetTickCount();
                state.last_status_at_ms = 0;
                state.nickname[0] = '\0';
                s_peers[event.peer] = state;
                LOG_NETPLAY(LOG_INFO,
                    "[SpectatorMgr] Peer connected peer=0x%p raw_count=%zu",
                    event.peer,
                    s_peers.size());
                break;
            }

            case ENET_EVENT_TYPE_RECEIVE: {
                if (!event.packet) {
                    break;
                }

                if (!Spectator::ValidatePacketSize(event.packet->data, event.packet->dataLength)) {
                    enet_packet_destroy(event.packet);
                    break;
                }

                const Spectator::PacketType type = Spectator::ReadPacketType(event.packet->data);
                const void* payload = Spectator::GetPayloadPtr(event.packet->data);
                const size_t payloadLen = Spectator::GetPayloadSize(event.packet->dataLength);

                switch (type) {
                    case Spectator::PacketType::Hello:
                        if (payloadLen >= sizeof(Spectator::HelloPayload)) {
                            HandleHello(event.peer, static_cast<const Spectator::HelloPayload*>(payload));
                        }
                        break;

                    case Spectator::PacketType::ClientStatus:
                        if (payloadLen >= sizeof(Spectator::ClientStatusPayload)) {
                            HandleClientStatus(event.peer, static_cast<const Spectator::ClientStatusPayload*>(payload));
                        }
                        break;

                    default:
                        break;
                }

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                LOG_NETPLAY(LOG_INFO,
                    "[SpectatorMgr] Peer disconnected peer=0x%p",
                    event.peer);
                s_peers.erase(event.peer);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    enet_host_flush(s_server);
}

bool SpectatorManager_IsServerActive() {
    return s_server != nullptr;
}

int SpectatorManager_GetConnectedCount() {
    return CountHandshakenPeers();
}

int SpectatorManager_GetPeerSnapshots(SpectatorPeerSnapshot* out, int maxPeers) {
    if (!out || maxPeers <= 0) {
        return 0;
    }

    int count = 0;
    for (const auto& entry : s_peers) {
        if (!entry.second.handshake_complete) {
            continue;
        }
        if (count >= maxPeers) {
            break;
        }

        SpectatorPeerSnapshot& dst = out[count++];
        memset(&dst, 0, sizeof(dst));
        dst.peer_id = (uintptr_t)entry.first;
        dst.connected = true;
        dst.handshake_complete = entry.second.handshake_complete;
        dst.needs_full_sync = entry.second.needs_full_sync;
        dst.next_rb_frame = entry.second.next_rb_frame;
        dst.last_playback_rb_frame = entry.second.last_playback_rb_frame;
        dst.fast_forward_requested = entry.second.fast_forward_requested;
        dst.hard_sync_requested = entry.second.hard_sync_requested;
        dst.advertised_listen_port = entry.second.advertised_listen_port;
        CopyText(dst.nickname, sizeof(dst.nickname), entry.second.nickname);
    }
    return count;
}

int32_t SpectatorManager_GetOldestRequestedFrame() {
    int32_t oldest = INT_MAX;
    for (const auto& entry : s_peers) {
        if (!entry.second.handshake_complete) {
            continue;
        }
        oldest = (std::min)(oldest, entry.second.next_rb_frame);
    }
    return oldest;
}

bool SpectatorManager_ClearPeerFullSync(uintptr_t peer_id) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer) {
        return false;
    }
    peer->needs_full_sync = false;
    return true;
}

bool SpectatorManager_SetPeerNextFrame(uintptr_t peer_id, int32_t next_rb_frame) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer) {
        return false;
    }
    peer->next_rb_frame = next_rb_frame;
    return true;
}

bool SpectatorManager_SendMatchState(uintptr_t peer_id, const Spectator::MatchStatePayload* payload) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer || !payload) {
        return false;
    }
    return SendTyped(peer->peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::MatchState,
        payload,
        sizeof(*payload),
        true);
}

bool SpectatorManager_SendPaletteState(uintptr_t peer_id, const Spectator::PaletteStatePayload* payload) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer || !payload) {
        return false;
    }
    return SendTyped(peer->peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::PaletteState,
        payload,
        sizeof(*payload),
        true);
}

bool SpectatorManager_SendFrameBatch(uintptr_t peer_id, const Spectator::FrameBatchPayload* payload) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer || !payload) {
        return false;
    }

    size_t payloadSize = sizeof(*payload);
    if (payload->record_count < Spectator::MAX_FRAME_BATCH) {
        payloadSize -= sizeof(payload->records) -
            (sizeof(payload->records[0]) * payload->record_count);
    }

    return SendTyped(peer->peer,
        Spectator::CHANNEL_STREAM,
        Spectator::PacketType::FrameBatch,
        payload,
        payloadSize,
        true);
}

void SpectatorManager_BroadcastHeartbeat(const Spectator::HeartbeatPayload* payload) {
    if (!payload) {
        return;
    }
    for (const auto& entry : s_peers) {
        if (!entry.second.handshake_complete) {
            continue;
        }
        SendTyped(entry.first,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Heartbeat,
            payload,
            sizeof(*payload),
            true);
    }
}

void SpectatorManager_BroadcastDisconnect(const Spectator::DisconnectPayload* payload) {
    if (!payload) {
        return;
    }
    for (const auto& entry : s_peers) {
        if (!entry.second.handshake_complete) {
            continue;
        }
        SendTyped(entry.first,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            payload,
            sizeof(*payload),
            true);
        enet_peer_disconnect_later(entry.first, 0);
    }
}

void SpectatorManager_GetSnapshot(SpectatorManagerSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->enabled = s_enabled;
    out->server_active = s_server != nullptr;
    out->listen_port = s_listenPort;
    out->active_match_id = s_activeMatchId;
    out->connected_spectators = CountHandshakenPeers();
    out->oldest_requested_rb_frame = SpectatorManager_GetOldestRequestedFrame();
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net