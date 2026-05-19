#include <enet/enet.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "net/spectator_manager.h"
#include "net/enet_transport.h"
#include "net/session_manager.h"
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

#define SMGR_LOG(level, frame, fmt, ...) \
    do { \
        LOG_NETPLAY(level, fmt, ##__VA_ARGS__); \
        Rollback::NetplayLog_WriteSpectator("SMGR", frame, fmt, ##__VA_ARGS__); \
    } while (0)

#define SMGR_TRACE(frame, fmt, ...) \
    Rollback::NetplayLog_WriteSpectator("SMGR", frame, fmt, ##__VA_ARGS__)

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
    char nickname[64];
};

static bool s_initialized = false;
static bool s_enabled = false;
static ENetHost* s_server = nullptr;
static uint16_t s_listenPort = 10701;
static uint16_t s_boundListenPort = 0;
static uint32_t s_activeMatchId = 0;
static uint32_t s_activeMatchOrdinal = 0;
static char s_redirectEndpoint[96] = "";
static bool s_autopunchEnabled = true;
static char s_autopunchRelayHost[96] = "delthas.fr";
static uint16_t s_autopunchRelayPort = 14763;
static char s_status[128] = "Spectator server disabled.";
static std::unordered_map<ENetPeer*, PeerState> s_peers;

constexpr int kMaxEventsPerFrame = 64;
constexpr int kMaxHandshakenSpectators = 8;
constexpr DWORD kRelayPeerFreshStatusMs = 2000;
constexpr int kPortFallbackScanCount = 16;

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

static bool BuildPeerAdvertisedEndpoint(const PeerState& state, char* outEndpoint, size_t cap) {
    if (!outEndpoint || cap == 0) {
        return false;
    }

    outEndpoint[0] = '\0';
    if (!state.peer || state.advertised_listen_port == 0) {
        return false;
    }

    char host[96] = {};
    if (enet_address_get_host_ip(&state.peer->address, host, sizeof(host)) != 0 || !host[0]) {
        return false;
    }

    if (strchr(host, ':')) {
        _snprintf_s(outEndpoint, cap, _TRUNCATE, "[%s]:%u", host, state.advertised_listen_port);
    } else {
        _snprintf_s(outEndpoint, cap, _TRUNCATE, "%s:%u", host, state.advertised_listen_port);
    }
    return outEndpoint[0] != '\0';
}

static bool FindRelayRedirectEndpoint(char* outEndpoint, size_t cap) {
    if (!outEndpoint || cap == 0) {
        return false;
    }

    outEndpoint[0] = '\0';
    const DWORD now = GetTickCount();
    const PeerState* best = nullptr;

    for (const auto& entry : s_peers) {
        const PeerState& state = entry.second;
        if (!state.handshake_complete || !state.peer) {
            continue;
        }
        if (state.advertised_listen_port == 0 || state.last_status_at_ms == 0) {
            continue;
        }
        if ((now - state.last_status_at_ms) > kRelayPeerFreshStatusMs) {
            continue;
        }
        if (state.last_playback_rb_frame < 0) {
            continue;
        }

        if (!best || state.connected_at_ms < best->connected_at_ms) {
            best = &state;
        }
    }

    if (!best) {
        return false;
    }

    return BuildPeerAdvertisedEndpoint(*best, outEndpoint, cap);
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

static uint16_t ResolveBoundPort(ENetHost* host, uint16_t fallbackPort) {
    if (!host) {
        return fallbackPort;
    }

    ENetAddress boundAddress{};
    if (enet_socket_get_address(host->socket, &boundAddress) == 0 &&
        boundAddress.port != 0) {
        return boundAddress.port;
    }

    return fallbackPort;
}

static ENetHost* TryCreateServerHost(uint16_t port) {
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    return enet_host_create(&address, 8, Spectator::NUM_CHANNELS, 0, 0);
}

static ENetHost* CreateServerWithFallback(uint16_t requestedPort,
                                          uint16_t* outBoundPort,
                                          bool* outUsedFallback,
                                          bool* outUsedEphemeral) {
    if (outBoundPort) {
        *outBoundPort = requestedPort;
    }
    if (outUsedFallback) {
        *outUsedFallback = false;
    }
    if (outUsedEphemeral) {
        *outUsedEphemeral = false;
    }

    ENetHost* host = TryCreateServerHost(requestedPort);
    if (host) {
        if (outBoundPort) {
            *outBoundPort = ResolveBoundPort(host, requestedPort);
        }
        return host;
    }

    for (int delta = 1; delta <= kPortFallbackScanCount; delta++) {
        const unsigned candidatePort = (unsigned)requestedPort + (unsigned)delta;
        if (candidatePort > UINT16_MAX) {
            break;
        }

        host = TryCreateServerHost((uint16_t)candidatePort);
        if (!host) {
            continue;
        }

        if (outBoundPort) {
            *outBoundPort = ResolveBoundPort(host, (uint16_t)candidatePort);
        }
        if (outUsedFallback) {
            *outUsedFallback = true;
        }
        return host;
    }

    host = TryCreateServerHost(0);
    if (host) {
        if (outBoundPort) {
            *outBoundPort = ResolveBoundPort(host, 0);
        }
        if (outUsedFallback) {
            *outUsedFallback = true;
        }
        if (outUsedEphemeral) {
            *outUsedEphemeral = true;
        }
    }

    return host;
}

static void DestroyServer(const char* reason) {
    if (!s_server) {
        s_boundListenPort = 0;
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

    Transport_AutopunchStopForHost(s_server, reason && reason[0] ? reason : "spectator host destroyed");
    enet_host_destroy(s_server);
    s_server = nullptr;
    s_boundListenPort = 0;
    s_peers.clear();
    SetStatus("Spectator server offline%s%s",
        reason ? ": " : "",
        reason ? reason : "");
    SMGR_LOG(LOG_INFO, -1,
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

    bool usedFallback = false;
    bool usedEphemeral = false;
    s_server = CreateServerWithFallback(
        s_listenPort,
        &s_boundListenPort,
        &usedFallback,
        &usedEphemeral);
    if (!s_server) {
        s_boundListenPort = 0;
        SetStatus("Failed to bind spectator port %u", s_listenPort);
        SMGR_LOG(LOG_WARNING, -1,
            "[SpectatorMgr] Failed to create spectator host on port %u",
            s_listenPort);
        return false;
    }

    if (usedFallback) {
        SetStatus("Spectator server listening on %u (requested %u)",
            s_boundListenPort,
            s_listenPort);
    } else {
        SetStatus("Spectator server listening on %u", s_boundListenPort);
    }
    SMGR_LOG(LOG_INFO, -1,
        "[SpectatorMgr] Spectator server listening: requested_port=%u bound_port=%u fallback=%d ephemeral=%d",
        s_listenPort,
        s_boundListenPort,
        usedFallback ? 1 : 0,
        usedEphemeral ? 1 : 0);
    if (s_autopunchEnabled) {
        Transport_AutopunchStartForHost(
            s_server,
            "SPECTATE_HOST",
            s_autopunchRelayHost,
            s_autopunchRelayPort,
            s_boundListenPort,
            nullptr,
            0);
        SMGR_LOG(LOG_INFO, -1,
            "[SpectatorMgr] Autopunch relay active: relay=%s:%u local_port=%u",
            s_autopunchRelayHost,
            s_autopunchRelayPort,
            s_boundListenPort);
    } else {
        SMGR_LOG(LOG_INFO, -1,
            "[SpectatorMgr] Autopunch relay disabled for spectator host");
    }
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
        SMGR_LOG(LOG_WARNING, -1,
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
        SMGR_LOG(LOG_INFO, -1,
            "[SpectatorMgr] Reject peer=0x%p reason=requested_match_not_active requested=0x%08X active=0x%08X",
            peer,
            payload->requested_match_id,
            s_activeMatchId);
        return;
    }

    const int handshakenPeers = CountHandshakenPeers();
    char redirectEndpoint[96] = {};
    bool shouldRedirect = false;
    if (handshakenPeers >= kMaxHandshakenSpectators) {
        shouldRedirect = FindRelayRedirectEndpoint(redirectEndpoint, sizeof(redirectEndpoint));
    }
    if (!shouldRedirect && s_redirectEndpoint[0]) {
        CopyText(redirectEndpoint, sizeof(redirectEndpoint), s_redirectEndpoint);
        shouldRedirect = handshakenPeers >= kMaxHandshakenSpectators;
    }

    if (shouldRedirect) {
        Spectator::RedirectPayload redirect{};
        CopyText(redirect.endpoint, sizeof(redirect.endpoint), redirectEndpoint);
        SendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Redirect,
            &redirect,
            sizeof(redirect),
            true);
        SMGR_LOG(LOG_INFO, -1,
            "[SpectatorMgr] Redirect peer=0x%p endpoint=%s handshaken=%d",
            peer,
            redirectEndpoint,
            handshakenPeers);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    if (handshakenPeers >= kMaxHandshakenSpectators) {
        Spectator::DisconnectPayload disconnect{};
        disconnect.reason_code = 3;
        CopyText(disconnect.message, sizeof(disconnect.message), "spectator capacity reached");
        SendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &disconnect,
            sizeof(disconnect),
            true);
        SMGR_LOG(LOG_INFO, -1,
            "[SpectatorMgr] Reject peer=0x%p reason=capacity_reached",
            peer);
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
    SessionSnapshot session{};
    Session_GetSnapshot(&session);
    ack.protocol_version = Spectator::PROTOCOL_VERSION;
    ack.server_listen_port = s_boundListenPort != 0 ? s_boundListenPort : s_listenPort;
    ack.session_listen_port = session.local_listen_port;
    ack.match_id = s_activeMatchId;
    ack.match_ordinal = s_activeMatchOrdinal;
    ack.match_state = (s_activeMatchId != 0)
        ? Spectator::MATCH_STATE_ACTIVE
        : Spectator::MATCH_STATE_IDLE;

    SendTyped(peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::HelloAck,
        &ack,
        sizeof(ack),
        true);

    SMGR_LOG(LOG_INFO, -1,
        "[SpectatorMgr] Spectator admitted: peer=0x%p nick='%s' listen_port=%u match_id=0x%08X match_state=%u",
        peer,
        state.nickname[0] ? state.nickname : "?",
        state.advertised_listen_port,
        ack.match_id,
        ack.match_state);
}

static void HandleClientStatus(ENetPeer* peer, const Spectator::ClientStatusPayload* payload) {
    if (!peer || !payload) {
        return;
    }

    if (payload->match_id != 0 &&
        (payload->match_id != s_activeMatchId || payload->match_ordinal != s_activeMatchOrdinal)) {
        return;
    }

    auto it = s_peers.find(peer);
    if (it == s_peers.end()) {
        return;
    }

    PeerState& state = it->second;
    const int32_t previousPlayback = state.last_playback_rb_frame;
    const bool previousFastForward = state.fast_forward_requested;
    const bool previousHardSync = state.hard_sync_requested;
    state.last_playback_rb_frame = payload->playback_rb_frame;
    state.fast_forward_requested = (payload->flags & Spectator::CLIENT_STATUS_FLAG_FAST_FORWARD) != 0;
    state.hard_sync_requested = (payload->flags & Spectator::CLIENT_STATUS_FLAG_HARD_SYNC) != 0;
    state.last_status_at_ms = GetTickCount();

    if (previousPlayback != state.last_playback_rb_frame ||
        previousFastForward != state.fast_forward_requested ||
        previousHardSync != state.hard_sync_requested) {
        SMGR_TRACE(
            state.last_playback_rb_frame,
            "[SpectatorMgr] ClientStatus peer=0x%p nick='%s' playback=%d buffered=%u fast_forward=%d hard_sync=%d",
            peer,
            state.nickname[0] ? state.nickname : "?",
            state.last_playback_rb_frame,
            payload->buffered_frame_count,
            state.fast_forward_requested ? 1 : 0,
            state.hard_sync_requested ? 1 : 0);
    }
}

static void HandleClientDisconnect(ENetPeer* peer, const Spectator::DisconnectPayload* payload) {
    if (!peer) {
        return;
    }

    auto it = s_peers.find(peer);
    if (it == s_peers.end()) {
        return;
    }

    const char* message = (payload && payload->message[0])
        ? payload->message
        : "client disconnect";
    SMGR_LOG(LOG_INFO, -1,
        "[SpectatorMgr] Peer requested disconnect peer=0x%p nick='%s' reason_code=%u message=%s",
        peer,
        it->second.nickname[0] ? it->second.nickname : "?",
        payload ? payload->reason_code : 0,
        message);
    s_peers.erase(it);
    enet_peer_disconnect_now(peer, 0);
}

} // namespace

namespace Net {

void SpectatorManager_Init() {
    if (s_initialized) {
        return;
    }
    s_enabled = false;
    s_server = nullptr;
    s_boundListenPort = 0;
    s_activeMatchId = 0;
    s_activeMatchOrdinal = 0;
    s_status[0] = '\0';
    s_autopunchEnabled = true;
    CopyText(s_autopunchRelayHost, sizeof(s_autopunchRelayHost), "delthas.fr");
    s_autopunchRelayPort = 14763;
    SetStatus("Spectator server disabled.");
    s_initialized = true;
    SMGR_TRACE(-1,
        "[SpectatorMgr] Init listen_port=%u enabled=%d",
        s_listenPort,
        s_enabled ? 1 : 0);
}

void SpectatorManager_Shutdown() {
    if (!s_initialized) {
        return;
    }
    SMGR_TRACE(-1, "[SpectatorMgr] Shutdown");
    DestroyServer("shutdown");
    s_initialized = false;
}

void SpectatorManager_SetEnabled(bool enabled) {
    if (s_enabled != enabled) {
        SMGR_TRACE(-1,
            "[SpectatorMgr] Enabled %d -> %d",
            s_enabled ? 1 : 0,
            enabled ? 1 : 0);
    }
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

    SMGR_TRACE(-1,
        "[SpectatorMgr] Listen port %u -> %u",
        s_listenPort,
        port);
    s_listenPort = port;
    if (s_server) {
        DestroyServer("rebind");
    }
    return EnsureServer();
}

void SpectatorManager_SetRedirectEndpoint(const char* endpoint) {
    CopyText(s_redirectEndpoint, sizeof(s_redirectEndpoint), endpoint);
    SMGR_TRACE(-1,
        "[SpectatorMgr] Redirect endpoint set to %s",
        s_redirectEndpoint[0] ? s_redirectEndpoint : "(unset)");
}

void SpectatorManager_SetAutopunchRelay(bool enabled, const char* relayHost, uint16_t relayPort) {
    const bool changed =
        s_autopunchEnabled != enabled ||
        s_autopunchRelayPort != relayPort ||
        _stricmp(s_autopunchRelayHost, relayHost && relayHost[0] ? relayHost : "") != 0;

    s_autopunchEnabled = enabled;
    if (relayHost && relayHost[0] && relayPort != 0) {
        CopyText(s_autopunchRelayHost, sizeof(s_autopunchRelayHost), relayHost);
        s_autopunchRelayPort = relayPort;
    } else {
        CopyText(s_autopunchRelayHost, sizeof(s_autopunchRelayHost), "delthas.fr");
        s_autopunchRelayPort = 14763;
    }

    if (changed) {
        SMGR_LOG(LOG_INFO, -1,
            "[SpectatorMgr] Autopunch relay config enabled=%d relay=%s:%u",
            s_autopunchEnabled ? 1 : 0,
            s_autopunchRelayHost,
            s_autopunchRelayPort);
        if (s_server) {
            Transport_AutopunchStopForHost(s_server, "spectator autopunch reconfigure");
            if (s_autopunchEnabled && s_boundListenPort != 0) {
                Transport_AutopunchStartForHost(
                    s_server,
                    "SPECTATE_HOST",
                    s_autopunchRelayHost,
                    s_autopunchRelayPort,
                    s_boundListenPort,
                    nullptr,
                    0);
            }
        }
    }
}

void SpectatorManager_BeginMatch(uint32_t match_id, uint32_t match_ordinal) {
    s_activeMatchId = match_id;
    s_activeMatchOrdinal = match_ordinal;
    for (auto& entry : s_peers) {
        entry.second.needs_full_sync = true;
        entry.second.next_rb_frame = 0;
    }
    SMGR_LOG(LOG_INFO, -1,
        "[SpectatorMgr] Active match begin: match_id=0x%08X ordinal=%u peers=%d",
        match_id,
        match_ordinal,
        CountHandshakenPeers());
}

void SpectatorManager_EndMatch(const char* reason) {
    s_activeMatchId = 0;
    s_activeMatchOrdinal = 0;
    if (reason && reason[0]) {
        SetStatus("Spectator server idle: %s", reason);
    } else {
        SetStatus("Spectator server idle.");
    }
    SMGR_LOG(LOG_INFO, -1,
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
    int processedEvents = 0;
    while (processedEvents < kMaxEventsPerFrame && enet_host_service(s_server, &event, 0) > 0) {
        processedEvents++;
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
                SMGR_LOG(LOG_INFO, -1,
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
                    SMGR_LOG(LOG_WARNING, -1,
                        "[SpectatorMgr] Dropped invalid packet peer=0x%p bytes=%zu",
                        event.peer,
                        (size_t)event.packet->dataLength);
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
                        } else {
                            SMGR_LOG(LOG_WARNING, -1,
                                "[SpectatorMgr] Short ClientStatus packet peer=0x%p bytes=%zu",
                                event.peer,
                                payloadLen);
                        }
                        break;

                    case Spectator::PacketType::Disconnect:
                        if (payloadLen >= sizeof(Spectator::DisconnectPayload)) {
                            HandleClientDisconnect(event.peer, static_cast<const Spectator::DisconnectPayload*>(payload));
                        } else {
                            HandleClientDisconnect(event.peer, nullptr);
                        }
                        break;

                    default:
                        SMGR_TRACE(-1,
                            "[SpectatorMgr] Ignored packet peer=0x%p type=%u bytes=%zu",
                            event.peer,
                            (unsigned)type,
                            payloadLen);
                        break;
                }

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                SMGR_LOG(LOG_INFO, -1,
                    "[SpectatorMgr] Peer disconnected peer=0x%p",
                    event.peer);
                s_peers.erase(event.peer);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    if (s_autopunchEnabled) {
        Transport_AutopunchServiceForHost(s_server, GetTickCount(), false);
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
    int32_t oldest = -1;
    for (const auto& entry : s_peers) {
        if (!entry.second.handshake_complete) {
            continue;
        }
        if (oldest < 0) {
            oldest = entry.second.next_rb_frame;
        } else {
            oldest = (std::min)(oldest, entry.second.next_rb_frame);
        }
    }
    return oldest;
}

bool SpectatorManager_ClearPeerFullSync(uintptr_t peer_id) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer) {
        return false;
    }
    peer->needs_full_sync = false;
    SMGR_TRACE(-1,
        "[SpectatorMgr] Full sync cleared peer=0x%p next_rb_frame=%d",
        reinterpret_cast<void*>(peer_id),
        peer->next_rb_frame);
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
    const bool sent = SendTyped(peer->peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::MatchState,
        payload,
        sizeof(*payload),
        true);
    if (!sent) {
        SMGR_LOG(LOG_WARNING, -1,
            "[SpectatorMgr] SendMatchState failed peer=0x%p match_id=0x%08X ordinal=%u",
            reinterpret_cast<void*>(peer_id),
            payload->match_id,
            payload->match_ordinal);
    }
    return sent;
}

bool SpectatorManager_SendPaletteState(uintptr_t peer_id, const Spectator::PaletteStatePayload* payload) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer || !payload) {
        return false;
    }
    const bool sent = SendTyped(peer->peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::PaletteState,
        payload,
        sizeof(*payload),
        true);
    if (!sent) {
        SMGR_LOG(LOG_WARNING, -1,
            "[SpectatorMgr] SendPaletteState failed peer=0x%p match_id=0x%08X ordinal=%u epoch=%u",
            reinterpret_cast<void*>(peer_id),
            payload->match_id,
            payload->match_ordinal,
            payload->palette_epoch);
    }
    return sent;
}

bool SpectatorManager_SendPaletteData(uintptr_t peer_id, const Spectator::PaletteDataPayload* payload) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer || !payload) {
        return false;
    }
    const bool sent = SendTyped(peer->peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::PaletteData,
        payload,
        sizeof(*payload),
        true);
    if (!sent) {
        SMGR_LOG(LOG_WARNING, -1,
            "[SpectatorMgr] SendPaletteData failed peer=0x%p match_id=0x%08X ordinal=%u slot=%u epoch=%u",
            reinterpret_cast<void*>(peer_id),
            payload->match_id,
            payload->match_ordinal,
            payload->game_slot,
            payload->palette_epoch);
    }
    return sent;
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

    const bool sent = SendTyped(peer->peer,
        Spectator::CHANNEL_STREAM,
        Spectator::PacketType::FrameBatch,
        payload,
        payloadSize,
        true);
    if (!sent) {
        SMGR_LOG(LOG_WARNING, -1,
            "[SpectatorMgr] SendFrameBatch failed peer=0x%p match_id=0x%08X ordinal=%u records=%u start=%d confirmed=%d live=%d",
            reinterpret_cast<void*>(peer_id),
            payload->match_id,
            payload->match_ordinal,
            payload->record_count,
            payload->archive_start_rb_frame,
            payload->confirmed_rb_frame,
            payload->live_rb_frame);
    }
    return sent;
}

bool SpectatorManager_SendHeartbeat(uintptr_t peer_id, const Spectator::HeartbeatPayload* payload) {
    PeerState* peer = FindPeer(peer_id);
    if (!peer || !payload) {
        return false;
    }

    const bool sent = SendTyped(peer->peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::Heartbeat,
        payload,
        sizeof(*payload),
        true);
    if (!sent) {
        SMGR_LOG(LOG_WARNING, -1,
            "[SpectatorMgr] SendHeartbeat failed peer=0x%p match_id=0x%08X ordinal=%u confirmed=%d live=%d state=%u",
            reinterpret_cast<void*>(peer_id),
            payload->match_id,
            payload->match_ordinal,
            payload->confirmed_rb_frame,
            payload->live_rb_frame,
            payload->match_state);
    }
    return sent;
}

void SpectatorManager_BroadcastHeartbeat(const Spectator::HeartbeatPayload* payload) {
    if (!payload) {
        return;
    }
    int sentCount = 0;
    for (const auto& entry : s_peers) {
        if (!entry.second.handshake_complete) {
            continue;
        }
        if (SendTyped(entry.first,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Heartbeat,
            payload,
            sizeof(*payload),
            true)) {
            sentCount++;
        }
    }
    SMGR_TRACE(payload->live_rb_frame,
        "[SpectatorMgr] BroadcastHeartbeat peers=%d match_id=0x%08X ordinal=%u confirmed=%d live=%d state=%u",
        sentCount,
        payload->match_id,
        payload->match_ordinal,
        payload->confirmed_rb_frame,
        payload->live_rb_frame,
        payload->match_state);
}

void SpectatorManager_BroadcastDisconnect(const Spectator::DisconnectPayload* payload) {
    if (!payload) {
        return;
    }
    int sentCount = 0;
    for (const auto& entry : s_peers) {
        if (!entry.second.handshake_complete) {
            continue;
        }
        if (SendTyped(entry.first,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            payload,
            sizeof(*payload),
            true)) {
            sentCount++;
        }
        enet_peer_disconnect_later(entry.first, 0);
    }
    SMGR_TRACE(-1,
        "[SpectatorMgr] BroadcastDisconnect peers=%d reason_code=%u message=%s",
        sentCount,
        payload->reason_code,
        payload->message[0] ? payload->message : "?");
}

void SpectatorManager_GetSnapshot(SpectatorManagerSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->enabled = s_enabled;
    out->server_active = s_server != nullptr;
    out->listen_port = (s_server && s_boundListenPort != 0)
        ? s_boundListenPort
        : s_listenPort;
    out->active_match_id = s_activeMatchId;
    out->connected_spectators = CountHandshakenPeers();
    out->oldest_requested_rb_frame = SpectatorManager_GetOldestRequestedFrame();
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net
