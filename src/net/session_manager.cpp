/**
 * Alice Senki 2 - Session Manager (Implementation)
 */

#include <winsock2.h>     // Must be before windows.h
#include <windows.h>

#include "net/session_manager.h"
#include "net/enet_transport.h"
#include "log_window.h"

#include <enet/enet.h>
#include <string.h>
#include <stdio.h>

namespace Net {

// ============================================================================
// Internal state
// ============================================================================

static SessionState   s_state = SessionState::Idle;
static SessionRole    s_role  = SessionRole::None;
static SessionConfig  s_config;
static PeerInfo       s_remotePeer;
static ConnectionStats s_stats;
static ENetPeer*      s_peer = nullptr;
static char           s_statusText[128] = "";
static char           s_errorText[128]  = "";
static PacketCallback s_packetCallback  = nullptr;
static bool           s_localReady      = false;
static bool           s_remoteReady     = false;
static DWORD          s_stateEnteredAt  = 0;  // GetTickCount when state entered

// ============================================================================
// Helpers
// ============================================================================

static void SetState(SessionState newState) {
    if (s_state == newState) return;
    LOG_INFO("[Session] State: %s -> %s", SessionStateName(s_state), SessionStateName(newState));
    s_state = newState;
    s_stateEnteredAt = GetTickCount();
}

static void SetError(const char* msg) {
    snprintf(s_errorText, sizeof(s_errorText), "%s", msg);
    LOG_ERROR("[Session] Error: %s", msg);
    SetState(SessionState::Failed);
}

static void ResetState() {
    s_state = SessionState::Idle;
    s_role  = SessionRole::None;
    memset(&s_remotePeer, 0, sizeof(s_remotePeer));
    memset(&s_stats, 0, sizeof(s_stats));
    s_peer = nullptr;
    s_statusText[0] = '\0';
    s_errorText[0]  = '\0';
    s_localReady  = false;
    s_remoteReady = false;
    s_stateEnteredAt = 0;
}

static void UpdateStatusText() {
    switch (s_state) {
        case SessionState::Idle:
            snprintf(s_statusText, sizeof(s_statusText), "No session");
            break;
        case SessionState::Connecting:
            snprintf(s_statusText, sizeof(s_statusText), "%s: Connecting...",
                     SessionRoleName(s_role));
            break;
        case SessionState::Handshaking:
            snprintf(s_statusText, sizeof(s_statusText), "%s: Handshaking...",
                     SessionRoleName(s_role));
            break;
        case SessionState::Connected:
            snprintf(s_statusText, sizeof(s_statusText), "Connected to %s (RTT: %.0fms)",
                     s_remotePeer.nickname, s_stats.rtt_ms);
            break;
        case SessionState::Ready:
            snprintf(s_statusText, sizeof(s_statusText), "Ready (both peers)");
            break;
        case SessionState::Disconnecting:
            snprintf(s_statusText, sizeof(s_statusText), "Disconnecting...");
            break;
        case SessionState::Failed:
            snprintf(s_statusText, sizeof(s_statusText), "Error: %s", s_errorText);
            break;
    }
}

// ============================================================================
// Handshake
// ============================================================================

static void SendHello() {
    HelloPayload hello;
    hello.protocol_version = PROTOCOL_VERSION;
    hello.build_hash = s_config.build_hash;
    hello.listen_port = s_config.listen_port;
    memset(hello.nickname, 0, sizeof(hello.nickname));
    strncpy(hello.nickname, s_config.nickname, sizeof(hello.nickname) - 1);

    Transport_SendTyped(s_peer, CHANNEL_CONTROL, PacketType::Hello,
                        &hello, sizeof(hello), true);
    LOG_INFO("[Session] Sent Hello (nick=%s, ver=%u, hash=0x%08X)",
             hello.nickname, hello.protocol_version, hello.build_hash);
}

static void SendHelloAck() {
    HelloAckPayload ack;
    ack.protocol_version = PROTOCOL_VERSION;
    ack.build_hash = s_config.build_hash;
    ack.listen_port = s_config.listen_port;
    memset(ack.nickname, 0, sizeof(ack.nickname));
    strncpy(ack.nickname, s_config.nickname, sizeof(ack.nickname) - 1);

    Transport_SendTyped(s_peer, CHANNEL_CONTROL, PacketType::HelloAck,
                        &ack, sizeof(ack), true);
    LOG_INFO("[Session] Sent HelloAck");
}

static bool ProcessHelloPayload(const void* payload, size_t len) {
    if (len < sizeof(HelloPayload)) {
        SetError("Hello payload too small");
        return false;
    }

    const HelloPayload* hello = static_cast<const HelloPayload*>(payload);

    if (hello->protocol_version != PROTOCOL_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Protocol version mismatch: local=%u remote=%u",
                 PROTOCOL_VERSION, hello->protocol_version);
        SetError(msg);
        return false;
    }

    if (hello->build_hash != 0 && s_config.build_hash != 0 &&
        hello->build_hash != s_config.build_hash) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Build hash mismatch: local=0x%08X remote=0x%08X",
                 s_config.build_hash, hello->build_hash);
        SetError(msg);
        return false;
    }

    s_remotePeer.valid = true;
    s_remotePeer.protocol_version = hello->protocol_version;
    s_remotePeer.build_hash = hello->build_hash;
    s_remotePeer.listen_port = hello->listen_port;
    memset(s_remotePeer.nickname, 0, sizeof(s_remotePeer.nickname));
    memcpy(s_remotePeer.nickname, hello->nickname, sizeof(hello->nickname));

    LOG_INFO("[Session] Remote peer: nick=%s, ver=%u, hash=0x%08X",
             s_remotePeer.nickname, s_remotePeer.protocol_version, s_remotePeer.build_hash);
    return true;
}

static bool ProcessHelloAckPayload(const void* payload, size_t len) {
    if (len < sizeof(HelloAckPayload)) {
        SetError("HelloAck payload too small");
        return false;
    }

    const HelloAckPayload* ack = static_cast<const HelloAckPayload*>(payload);

    HelloPayload asHello;
    asHello.protocol_version = ack->protocol_version;
    asHello.build_hash = ack->build_hash;
    asHello.listen_port = ack->listen_port;
    memcpy(asHello.nickname, ack->nickname, sizeof(asHello.nickname));

    return ProcessHelloPayload(&asHello, sizeof(asHello));
}

// ============================================================================
// Event handlers
// ============================================================================

static void OnENetConnect(ENetPeer* peer) {
    s_peer = peer;
    LOG_INFO("[Session] ENet connected (peer %p)", peer);

    if (s_state == SessionState::Connecting) {
        SetState(SessionState::Handshaking);
        // Joiner sends Hello first; Host waits
        if (s_role == SessionRole::Join) {
            SendHello();
        }
    }
}

static void OnENetDisconnect(ENetPeer* peer, uint32_t data) {
    LOG_INFO("[Session] ENet disconnected (peer %p, data=%u)", peer, data);
    s_peer = nullptr;

    if (s_state == SessionState::Disconnecting) {
        ResetState();
    } else if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
        SetError("Peer disconnected unexpectedly");
    }
}

static void OnPacketReceived(ENetPeer* peer, uint8_t channelID,
                             const void* data, size_t length) {
    if (!ValidatePacketSize(data, length)) {
        LOG_WARN("[Session] Received too-small packet (len=%zu)", length);
        return;
    }

    PacketType type = ReadPacketType(data);
    const void* payload = GetPayloadPtr(data);
    size_t payloadLen = GetPayloadSize(length);

    switch (type) {
        case PacketType::Hello:
            if (s_state == SessionState::Handshaking || s_state == SessionState::Connecting) {
                if (s_state == SessionState::Connecting) {
                    // Host received Hello before we noticed the connect event
                    s_peer = peer;
                    SetState(SessionState::Handshaking);
                }
                if (ProcessHelloPayload(payload, payloadLen)) {
                    SendHelloAck();
                    if (s_role == SessionRole::Host) {
                        SetState(SessionState::Connected);
                    }
                }
            }
            break;

        case PacketType::HelloAck:
            if (s_state == SessionState::Handshaking) {
                if (ProcessHelloAckPayload(payload, payloadLen)) {
                    SetState(SessionState::Connected);
                }
            }
            break;

        case PacketType::Ready:
            s_remoteReady = true;
            LOG_INFO("[Session] Remote peer signaled Ready");
            if (s_localReady && s_remoteReady && s_state == SessionState::Connected) {
                SetState(SessionState::Ready);
            }
            break;

        case PacketType::Disconnect: {
            const char* reason = "Remote disconnected";
            if (payloadLen >= sizeof(DisconnectPayload)) {
                const DisconnectPayload* dp = static_cast<const DisconnectPayload*>(payload);
                reason = dp->message;
            }
            LOG_INFO("[Session] Received Disconnect: %s", reason);
            if (s_peer) {
                Transport_ForceDisconnectPeer(s_peer);
                s_peer = nullptr;
            }
            ResetState();
            break;
        }

        default:
            // Forward to external callback
            if (s_packetCallback) {
                s_packetCallback(type, payload, payloadLen);
            }
            break;
    }
}

// ============================================================================
// Timeout checks
// ============================================================================

static void CheckTimeouts() {
    DWORD now = GetTickCount();
    DWORD elapsed = now - s_stateEnteredAt;

    switch (s_state) {
        case SessionState::Connecting:
            if (elapsed > s_config.connect_timeout_ms) {
                SetError("Connection timed out");
            }
            break;
        case SessionState::Handshaking:
            if (elapsed > s_config.handshake_timeout_ms) {
                SetError("Handshake timed out");
            }
            break;
        default:
            break;
    }
}

// ============================================================================
// Update peer stats
// ============================================================================

static void UpdateStats() {
    if (!s_peer) return;
    s_stats.rtt_ms = Transport_GetPeerRTT(s_peer);
    s_stats.rtt_variance_ms = (float)s_peer->roundTripTimeVariance;
    s_stats.packets_sent = s_peer->packetsSent;
    s_stats.packets_lost = s_peer->packetsLost;
}

// ============================================================================
// Public API
// ============================================================================

void Session_Init() {
    ResetState();
    LOG_INFO("[Session] Session manager initialized");
}

void Session_Shutdown() {
    if (s_state != SessionState::Idle) {
        Session_Cancel();
    }
    LOG_INFO("[Session] Session manager shut down");
}

bool Session_StartHost(const SessionConfig* config) {
    if (s_state != SessionState::Idle) {
        LOG_WARN("[Session] Cannot start host: session already active (state=%s)",
                 SessionStateName(s_state));
        return false;
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_role = SessionRole::Host;

    if (!Transport_CreateHost(config->listen_port)) {
        SetError("Failed to create host");
        return false;
    }

    SetState(SessionState::Connecting);
    LOG_INFO("[Session] Hosting on port %u, waiting for peer...", config->listen_port);
    return true;
}

bool Session_StartJoin(const SessionConfig* config) {
    if (s_state != SessionState::Idle) {
        LOG_WARN("[Session] Cannot start join: session already active (state=%s)",
                 SessionStateName(s_state));
        return false;
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_role = SessionRole::Join;

    // Joiner creates a host on an ephemeral port, then connects out
    if (!Transport_CreateHost(0)) {
        SetError("Failed to create host for joining");
        return false;
    }

    ENetPeer* peer = Transport_Connect(config->target_ip, config->target_port);
    if (!peer) {
        SetError("Failed to initiate connection");
        Transport_DestroyHost();
        return false;
    }

    SetState(SessionState::Connecting);
    return true;
}

void Session_Cancel() {
    if (s_state == SessionState::Idle) return;

    LOG_INFO("[Session] Canceling session (was %s)", SessionStateName(s_state));

    // Send a disconnect packet if we have a peer
    if (s_peer && (s_state == SessionState::Connected ||
                   s_state == SessionState::Ready ||
                   s_state == SessionState::Handshaking)) {
        DisconnectPayload dp;
        dp.reason_code = static_cast<uint16_t>(DisconnectReason::UserCancel);
        strncpy(dp.message, "Session canceled", sizeof(dp.message) - 1);
        dp.message[sizeof(dp.message) - 1] = '\0';
        Transport_SendTyped(s_peer, CHANNEL_CONTROL, PacketType::Disconnect,
                            &dp, sizeof(dp), true);
        Transport_Flush();
        Transport_DisconnectPeer(s_peer);
    }

    Transport_DestroyHost();
    ResetState();
}

void Session_SignalReady() {
    if (s_state != SessionState::Connected) {
        LOG_WARN("[Session] Cannot signal ready: not in Connected state");
        return;
    }

    s_localReady = true;
    Transport_SendTyped(s_peer, CHANNEL_CONTROL, PacketType::Ready,
                        nullptr, 0, true);
    LOG_INFO("[Session] Signaled Ready (local)");

    if (s_localReady && s_remoteReady) {
        SetState(SessionState::Ready);
    }
}

void Session_Update() {
    if (s_state == SessionState::Idle || s_state == SessionState::Failed) {
        UpdateStatusText();
        return;
    }

    // Poll ENet (non-blocking)
    ENetEvent event;
    while (Transport_Service(0, &event) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                OnENetConnect(event.peer);
                // Host receives connect -> transition to Handshaking, wait for Hello
                if (s_role == SessionRole::Host && s_state == SessionState::Connecting) {
                    s_peer = event.peer;
                    SetState(SessionState::Handshaking);
                }
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                OnPacketReceived(event.peer, event.channelID,
                                 event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                OnENetDisconnect(event.peer, event.data);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    CheckTimeouts();
    UpdateStats();
    UpdateStatusText();
}

bool Session_SendPacket(uint8_t channel, PacketType type,
                        const void* payload, size_t payloadLen, bool reliable) {
    if (!s_peer || (s_state != SessionState::Connected && s_state != SessionState::Ready)) {
        return false;
    }
    return Transport_SendTyped(s_peer, channel, type, payload, payloadLen, reliable);
}

void Session_SetPacketCallback(PacketCallback cb) {
    s_packetCallback = cb;
}

void Session_GetSnapshot(SessionSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = (s_state != SessionState::Idle);
    out->state  = s_state;
    out->role   = s_role;
    out->remote_peer = s_remotePeer;
    out->stats  = s_stats;
    memcpy(out->status_text, s_statusText, sizeof(out->status_text));
    memcpy(out->error_text, s_errorText, sizeof(out->error_text));
}

SessionState Session_GetState() {
    return s_state;
}

SessionRole Session_GetRole() {
    return s_role;
}

bool Session_IsConnected() {
    return s_state == SessionState::Handshaking ||
           s_state == SessionState::Connected ||
           s_state == SessionState::Ready;
}

const PeerInfo* Session_GetRemotePeer() {
    return s_remotePeer.valid ? &s_remotePeer : nullptr;
}

void Session_GetStats(ConnectionStats* out) {
    if (out) {
        memcpy(out, &s_stats, sizeof(*out));
    }
}

} // namespace Net
