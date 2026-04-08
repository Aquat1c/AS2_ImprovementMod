/**
 * Alice Senki 2 - Session Manager (Implementation)
 */

#include <winsock2.h>     // Must be before windows.h
#include <windows.h>

#include "net/session_manager.h"
#include "net/network_thread.h"
#include "log_window.h"
#include "rollback/netplay_log.h"

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
static uintptr_t      s_peerToken = 0;
static char           s_statusText[128] = "";
static char           s_errorText[128]  = "";
static PacketCallback s_packetCallback  = nullptr;
static bool           s_localReady      = false;
static bool           s_remoteReady     = false;
static DWORD          s_stateEnteredAt  = 0;  // GetTickCount when state entered
static uint32_t       s_activeSessionToken = 0;

static uint32_t       s_lastInboundDropCount  = 0;
static uint32_t       s_lastOutboundDropCount = 0;
static DWORD          s_lastQueueSpikeLogAt   = 0;
static DWORD          s_lastDrainLagLogAt     = 0;

constexpr int MAX_DEFERRED_CONTROL_PACKETS = 64;

struct DeferredControlPacket {
    PacketType type;
    uint8_t    channel_id;
    size_t     payload_len;
    uint8_t    payload[MAX_PAYLOAD_SIZE];
};

static DeferredControlPacket s_deferredControlPackets[MAX_DEFERRED_CONTROL_PACKETS];
static int                  s_deferredControlCount = 0;

// ============================================================================
// Helpers
// ============================================================================

static void SetState(SessionState newState) {
    if (s_state == newState) return;
    LOG_INFO("[Session] State: %s -> %s", SessionStateName(s_state), SessionStateName(newState));
    Rollback::NetplayLog_StateChange(
        "SESSION", -1,
        "state",
        SessionStateName(s_state),
        SessionStateName(newState),
        "session manager transition"
    );
    s_state = newState;
    s_stateEnteredAt = GetTickCount();
}

static void SetError(const char* msg) {
    snprintf(s_errorText, sizeof(s_errorText), "%s", msg);
    LOG_ERROR("[Session] Error: %s", msg);
    Rollback::NetplayLog_Write("SESSION", -1, "ERROR: %s", msg);
    SetState(SessionState::Failed);
}

static void ResetState() {
    s_state = SessionState::Idle;
    s_role  = SessionRole::None;
    memset(&s_remotePeer, 0, sizeof(s_remotePeer));
    memset(&s_stats, 0, sizeof(s_stats));
    s_peerToken = 0;
    s_statusText[0] = '\0';
    s_errorText[0]  = '\0';
    s_localReady  = false;
    s_remoteReady = false;
    s_stateEnteredAt = 0;
    s_deferredControlCount = 0;
    s_lastInboundDropCount = 0;
    s_lastOutboundDropCount = 0;
    s_lastQueueSpikeLogAt = 0;
    s_lastDrainLagLogAt = 0;
}

static bool DeferControlPacket(uint8_t channelID, PacketType type,
                               const void* payload, size_t payloadLen) {
    if (channelID != CHANNEL_CONTROL) {
        return false;
    }
    if (payloadLen > MAX_PAYLOAD_SIZE) {
        return false;
    }
    if (s_deferredControlCount >= MAX_DEFERRED_CONTROL_PACKETS) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Deferred control packet queue full: type=%s ch=%u payload=%zu state=%s role=%s",
            PacketTypeName(type),
            channelID,
            payloadLen,
            SessionStateName(s_state),
            SessionRoleName(s_role));
        Rollback::NetplayLog_Flush();
        return false;
    }

    DeferredControlPacket* slot = &s_deferredControlPackets[s_deferredControlCount++];
    slot->type = type;
    slot->channel_id = channelID;
    slot->payload_len = payloadLen;
    if (payloadLen > 0 && payload) {
        memcpy(slot->payload, payload, payloadLen);
    }

    Rollback::NetplayLog_Write("SESSION", -1,
        "Deferred control packet awaiting callback: type=%s ch=%u payload=%zu queued=%d state=%s role=%s",
        PacketTypeName(type),
        channelID,
        payloadLen,
        s_deferredControlCount,
        SessionStateName(s_state),
        SessionRoleName(s_role));
    Rollback::NetplayLog_Flush();
    return true;
}

static void FlushDeferredControlPackets() {
    if (!s_packetCallback || s_deferredControlCount <= 0) {
        return;
    }

    const int queued = s_deferredControlCount;
    Rollback::NetplayLog_Write("SESSION", -1,
        "Flushing %d deferred control packets to callback 0x%llX",
        queued,
        (unsigned long long)(uintptr_t)s_packetCallback);
    Rollback::NetplayLog_Flush();

    s_deferredControlCount = 0;
    for (int i = 0; i < queued; i++) {
        const DeferredControlPacket* packet = &s_deferredControlPackets[i];
        Rollback::NetplayLog_Write("SESSION", -1,
            "Dispatching deferred packet to callback: cb=0x%llX type=%s ch=%u payload=%zu state=%s role=%s",
            (unsigned long long)(uintptr_t)s_packetCallback,
            PacketTypeName(packet->type),
            packet->channel_id,
            packet->payload_len,
            SessionStateName(s_state),
            SessionRoleName(s_role));
        Rollback::NetplayLog_Flush();
        s_packetCallback(packet->type, packet->payload, packet->payload_len);
        Rollback::NetplayLog_Write("SESSION", -1,
            "Deferred packet callback returned: cb=0x%llX type=%s ch=%u",
            (unsigned long long)(uintptr_t)s_packetCallback,
            PacketTypeName(packet->type),
            packet->channel_id);
        Rollback::NetplayLog_Flush();
    }
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

static void NotePacketSent(uint8_t channel, PacketType type,
                           size_t payloadLen, bool reliable,
                           const char* context) {
    s_stats.bytes_sent += sizeof(PacketType) + payloadLen;
    Rollback::NetplayLog_Verbose("SESSION", -1,
        "SEND %s ch=%u type=%s payload=%zu total=%zu reliable=%d state=%s",
        context ? context : "packet",
        channel,
        PacketTypeName(type),
        payloadLen,
        sizeof(PacketType) + payloadLen,
        reliable ? 1 : 0,
        SessionStateName(s_state));
}

static void NotePacketReceived(uint8_t channel, PacketType type, size_t payloadLen) {
    s_stats.packets_received++;
    s_stats.bytes_received += sizeof(PacketType) + payloadLen;
    Rollback::NetplayLog_Verbose("SESSION", -1,
        "RECV ch=%u type=%s payload=%zu total=%zu state=%s",
        channel,
        PacketTypeName(type),
        payloadLen,
        sizeof(PacketType) + payloadLen,
        SessionStateName(s_state));
}

static uint32_t NextSessionToken() {
    s_activeSessionToken++;
    if (s_activeSessionToken == 0) {
        s_activeSessionToken = 1;
    }
    return s_activeSessionToken;
}

static bool QueueTypedPacket(uint8_t channel, PacketType type,
                             const void* payload, size_t payloadLen,
                             bool reliable, const char* context) {
    if (s_activeSessionToken == 0) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "DROP send (no active session token): ch=%u type=%s payload=%zu reliable=%d state=%s",
            channel,
            PacketTypeName(type),
            payloadLen,
            reliable ? 1 : 0,
            SessionStateName(s_state));
        return false;
    }

    const bool queued = NetworkThread_SendPacket(
        s_activeSessionToken,
        channel,
        type,
        payload,
        payloadLen,
        reliable);
    if (queued) {
        NotePacketSent(channel, type, payloadLen, reliable, context);
    } else {
        Rollback::NetplayLog_Write("SESSION", -1,
            "ERROR: outbound queue rejected send ch=%u type=%s payload=%zu reliable=%d token=%u",
            channel,
            PacketTypeName(type),
            payloadLen,
            reliable ? 1 : 0,
            s_activeSessionToken);
    }
    return queued;
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

    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::Hello,
                          &hello, sizeof(hello), true, "hello")) {
        SetError("Failed to send Hello");
        return;
    }
    LOG_INFO("[Session] Sent Hello (nick=%s, ver=%u, hash=0x%08X)",
             hello.nickname, hello.protocol_version, hello.build_hash);
    Rollback::NetplayLog_Write("SESSION", -1,
        "Sent Hello: nick=%s ver=%u hash=0x%08X listen_port=%u",
        hello.nickname, hello.protocol_version, hello.build_hash, hello.listen_port);
}

static void SendHelloAck() {
    HelloAckPayload ack;
    ack.protocol_version = PROTOCOL_VERSION;
    ack.build_hash = s_config.build_hash;
    ack.listen_port = s_config.listen_port;
    memset(ack.nickname, 0, sizeof(ack.nickname));
    strncpy(ack.nickname, s_config.nickname, sizeof(ack.nickname) - 1);

    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::HelloAck,
                          &ack, sizeof(ack), true, "hello-ack")) {
        SetError("Failed to send HelloAck");
        return;
    }
    LOG_INFO("[Session] Sent HelloAck");
    Rollback::NetplayLog_Write("SESSION", -1,
        "Sent HelloAck: nick=%s ver=%u hash=0x%08X listen_port=%u",
        ack.nickname, ack.protocol_version, ack.build_hash, ack.listen_port);
}

static bool ProcessHelloPayload(const void* payload, size_t len) {
    if (len < sizeof(HelloPayload)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Hello payload too small: got=%zu expected=%zu state=%s role=%s",
            len, sizeof(HelloPayload), SessionStateName(s_state), SessionRoleName(s_role));
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
    Rollback::NetplayLog_Write("SESSION", -1,
        "Remote Hello accepted: nick=%s ver=%u hash=0x%08X listen_port=%u",
        s_remotePeer.nickname,
        s_remotePeer.protocol_version,
        s_remotePeer.build_hash,
        s_remotePeer.listen_port);
    return true;
}

static bool ProcessHelloAckPayload(const void* payload, size_t len) {
    if (len < sizeof(HelloAckPayload)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "HelloAck payload too small: got=%zu expected=%zu state=%s role=%s",
            len, sizeof(HelloAckPayload), SessionStateName(s_state), SessionRoleName(s_role));
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

static void OnTransportConnected(uintptr_t peerToken) {
    s_peerToken = peerToken;
    LOG_INFO("[Session] ENet connected (peer 0x%llX)", (unsigned long long)peerToken);
    Rollback::NetplayLog_Write("SESSION", -1,
        "ENet connected: peer=0x%llX role=%s token=%u",
        (unsigned long long)peerToken,
        SessionRoleName(s_role),
        s_activeSessionToken);

    if (s_state == SessionState::Connecting) {
        SetState(SessionState::Handshaking);
        // Joiner sends Hello first; Host waits
        if (s_role == SessionRole::Join) {
            SendHello();
        }
    }
}

static void OnTransportDisconnect(uintptr_t peerToken, uint32_t data, DWORD transportTickMs) {
    const DWORD now = GetTickCount();
    const DWORD consumeLagMs = now - transportTickMs;
    LOG_INFO("[Session] ENet disconnected (peer 0x%llX, data=%u, lag=%lums)",
             (unsigned long long)peerToken, data, (unsigned long)consumeLagMs);
    Rollback::NetplayLog_Write("SESSION", -1,
        "ENet disconnected: peer=0x%llX data=%u state=%s token=%u consume_lag=%lums",
        (unsigned long long)peerToken,
        data,
        SessionStateName(s_state),
        s_activeSessionToken,
        (unsigned long)consumeLagMs);
    s_peerToken = 0;

    if (s_state == SessionState::Disconnecting) {
        ResetState();
    } else if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
        SetError("Peer disconnected unexpectedly");
    }
}

static void OnPacketReceived(uintptr_t peerToken, uint8_t channelID,
                             const void* data, size_t length,
                             DWORD transportTickMs) {
    if (!ValidatePacketSize(data, length)) {
        LOG_WARN("[Session] Received too-small packet (len=%zu)", length);
        Rollback::NetplayLog_Write("SESSION", -1,
            "DROP recv too-small: peer=0x%llX ch=%u len=%zu state=%s role=%s token=%u",
            (unsigned long long)peerToken,
            channelID,
            length,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken);
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD consumeLagMs = now - transportTickMs;
    if (consumeLagMs >= 50 && (now - s_lastDrainLagLogAt) >= 250) {
        s_lastDrainLagLogAt = now;
        Rollback::NetplayLog_Write("SESSION", -1,
            "Inbound packet consume lag: %lums peer=0x%llX ch=%u len=%zu token=%u",
            (unsigned long)consumeLagMs,
            (unsigned long long)peerToken,
            channelID,
            length,
            s_activeSessionToken);
    }

    PacketType type = ReadPacketType(data);
    const void* payload = GetPayloadPtr(data);
    size_t payloadLen = GetPayloadSize(length);

    NotePacketReceived(channelID, type, payloadLen);

    switch (type) {
        case PacketType::Hello:
            if (s_state == SessionState::Handshaking || s_state == SessionState::Connecting) {
                if (s_state == SessionState::Connecting) {
                    // Host received Hello before we noticed the connect event
                    s_peerToken = peerToken;
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
            Rollback::NetplayLog_Write("SESSION", -1,
                "Remote Ready received: local_ready=%d remote_ready=%d",
                s_localReady ? 1 : 0, s_remoteReady ? 1 : 0);
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
            Rollback::NetplayLog_Write("SESSION", -1,
                "Remote Disconnect received: %s", reason);
            if (s_activeSessionToken != 0) {
                NetworkThread_RequestDisconnect(s_activeSessionToken, 0, true);
                NetworkThread_RequestDestroyHost(s_activeSessionToken);
                NetworkThread_ClearQueues(s_activeSessionToken);
            }
            s_peerToken = 0;
            ResetState();
            break;
        }

        default:
            // Forward to external callback
            if (s_packetCallback) {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Dispatching packet to callback: cb=0x%llX type=%s ch=%u payload=%zu state=%s role=%s peer=0x%llX",
                    (unsigned long long)(uintptr_t)s_packetCallback,
                    PacketTypeName(type),
                    channelID,
                    payloadLen,
                    SessionStateName(s_state),
                    SessionRoleName(s_role),
                    (unsigned long long)peerToken);
                Rollback::NetplayLog_Flush();
                s_packetCallback(type, payload, payloadLen);
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Callback returned: cb=0x%llX type=%s ch=%u",
                    (unsigned long long)(uintptr_t)s_packetCallback,
                    PacketTypeName(type),
                    channelID);
                Rollback::NetplayLog_Flush();
            } else {
                if (!DeferControlPacket(channelID, type, payload, payloadLen)) {
                    Rollback::NetplayLog_Write("SESSION", -1,
                        "Unhandled packet with no callback: type=%s ch=%u payload=%zu state=%s role=%s peer=0x%llX",
                        PacketTypeName(type), channelID, payloadLen,
                        SessionStateName(s_state), SessionRoleName(s_role),
                        (unsigned long long)peerToken);
                    Rollback::NetplayLog_Flush();
                }
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
            // Host listens indefinitely — only joiner has a connect timeout
            if (s_role == SessionRole::Join && elapsed > s_config.connect_timeout_ms) {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Connect timeout after %lu ms", (unsigned long)elapsed);
                SetError("Connection timed out");
            }
            break;
        case SessionState::Handshaking:
            if (elapsed > s_config.handshake_timeout_ms) {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Handshake timeout after %lu ms", (unsigned long)elapsed);
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
    NetworkThreadStats netStats{};
    NetworkThread_GetStats(&netStats);

    s_stats.rtt_ms = netStats.rtt_ms;
    s_stats.rtt_variance_ms = netStats.rtt_variance_ms;
    s_stats.packets_sent = netStats.packets_sent;
    s_stats.packets_lost = netStats.packets_lost;

    const DWORD now = GetTickCount();
    if ((netStats.inbound_queue_depth >= 128 || netStats.outbound_queue_depth >= 128) &&
        (now - s_lastQueueSpikeLogAt) >= 250) {
        s_lastQueueSpikeLogAt = now;
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queue depth spike: inbound=%u outbound=%u state=%s role=%s token=%u",
            netStats.inbound_queue_depth,
            netStats.outbound_queue_depth,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken);
    }

    if (netStats.inbound_drop_count != s_lastInboundDropCount) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Inbound queue drops: old=%u new=%u state=%s token=%u",
            s_lastInboundDropCount,
            netStats.inbound_drop_count,
            SessionStateName(s_state),
            s_activeSessionToken);
        s_lastInboundDropCount = netStats.inbound_drop_count;
    }

    if (netStats.outbound_drop_count != s_lastOutboundDropCount) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Outbound queue drops: old=%u new=%u state=%s token=%u",
            s_lastOutboundDropCount,
            netStats.outbound_drop_count,
            SessionStateName(s_state),
            s_activeSessionToken);
        s_lastOutboundDropCount = netStats.outbound_drop_count;
    }
}

static void DrainNetworkEvents() {
    NetworkThreadEvent ev{};
    int drained = 0;
    int staleDropped = 0;

    while (NetworkThread_TryPopEvent(&ev)) {
        drained++;

        if ((ev.session_token == 0 && s_activeSessionToken != 0) ||
            (ev.session_token != 0 && ev.session_token != s_activeSessionToken)) {
            staleDropped++;
            continue;
        }

        switch (ev.type) {
            case NetworkThreadEventType::Connected:
                OnTransportConnected(ev.peer_token);
                break;

            case NetworkThreadEventType::Disconnected:
                OnTransportDisconnect(ev.peer_token, ev.disconnect_data, ev.transport_tick_ms);
                break;

            case NetworkThreadEventType::PacketReceived:
                OnPacketReceived(ev.peer_token,
                                 ev.channel_id,
                                 ev.packet_data,
                                 ev.packet_len,
                                 ev.transport_tick_ms);
                break;

            case NetworkThreadEventType::WorkerError:
                Rollback::NetplayLog_Write("NTHREAD", -1,
                    "Worker error event: token=%u msg=%s state=%s role=%s",
                    ev.session_token,
                    ev.error_text,
                    SessionStateName(s_state),
                    SessionRoleName(s_role));
                if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
                    SetError(ev.error_text[0] ? ev.error_text : "Network worker error");
                }
                break;
        }
    }

    if (staleDropped > 0) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Dropped %d stale network events (active_token=%u)",
            staleDropped,
            s_activeSessionToken);
    }

    if (drained > 0) {
        Rollback::NetplayLog_Verbose("NTHREAD", -1,
            "Drained network events on game thread: count=%d state=%s role=%s token=%u",
            drained,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken);
    }
}

// ============================================================================
// Public API
// ============================================================================

void Session_Init() {
    ResetState();
    s_activeSessionToken = 0;
    if (!NetworkThread_Init()) {
        LOG_ERROR("[Session] Failed to initialize network service thread");
        Rollback::NetplayLog_Write("NTHREAD", -1, "ERROR: network service thread init failed");
    }
    LOG_INFO("[Session] Session manager initialized");
}

void Session_Shutdown() {
    if (s_state != SessionState::Idle) {
        Session_Cancel();
    }
    NetworkThread_Shutdown();
    s_activeSessionToken = 0;
    ResetState();
    LOG_INFO("[Session] Session manager shut down");
}

bool Session_StartHost(const SessionConfig* config) {
    if (!config) return false;
    if (s_state != SessionState::Idle) {
        LOG_WARN("[Session] Cannot start host: session already active (state=%s)",
                 SessionStateName(s_state));
        return false;
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_role = SessionRole::Host;

    Rollback::NetplayLog_Write("SESSION", -1,
        "StartHost: listen_port=%u nick=%s hash=0x%08X connect_timeout=%u handshake_timeout=%u",
        config->listen_port,
        config->nickname,
        config->build_hash,
        config->connect_timeout_ms,
        config->handshake_timeout_ms);

    if (!NetworkThread_Init()) {
        SetError("Failed to initialize network worker");
        return false;
    }

    NextSessionToken();
    NetworkThread_ClearQueues(0);
    if (!NetworkThread_StartHost(s_activeSessionToken, config->listen_port)) {
        SetError("Failed to start network host thread command");
        return false;
    }

    SetState(SessionState::Connecting);
    LOG_INFO("[Session] Hosting on port %u, waiting for peer... (token=%u)",
             config->listen_port, s_activeSessionToken);
    Rollback::NetplayLog_Write("NTHREAD", -1,
        "Session attached to network thread: role=Host token=%u",
        s_activeSessionToken);
    return true;
}

bool Session_StartJoin(const SessionConfig* config) {
    if (!config) return false;
    if (s_state != SessionState::Idle) {
        LOG_WARN("[Session] Cannot start join: session already active (state=%s)",
                 SessionStateName(s_state));
        return false;
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_role = SessionRole::Join;

    Rollback::NetplayLog_Write("SESSION", -1,
        "StartJoin: target=%u.%u.%u.%u:%u nick=%s hash=0x%08X connect_timeout=%u handshake_timeout=%u",
        (config->target_ip) & 0xFF,
        (config->target_ip >> 8) & 0xFF,
        (config->target_ip >> 16) & 0xFF,
        (config->target_ip >> 24) & 0xFF,
        config->target_port,
        config->nickname,
        config->build_hash,
        config->connect_timeout_ms,
        config->handshake_timeout_ms);

    if (!NetworkThread_Init()) {
        SetError("Failed to initialize network worker");
        return false;
    }

    NextSessionToken();
    NetworkThread_ClearQueues(0);
    if (!NetworkThread_StartJoin(s_activeSessionToken,
                                 config->listen_port,
                                 config->target_ip,
                                 config->target_port)) {
        SetError("Failed to start network join thread command");
        return false;
    }

    SetState(SessionState::Connecting);
    Rollback::NetplayLog_Write("NTHREAD", -1,
        "Session attached to network thread: role=Join token=%u",
        s_activeSessionToken);
    return true;
}

void Session_Cancel() {
    if (s_state == SessionState::Idle) return;

    LOG_INFO("[Session] Canceling session (was %s)", SessionStateName(s_state));
    Rollback::NetplayLog_Write("SESSION", -1,
        "Cancel requested from state=%s", SessionStateName(s_state));

    const uint32_t cancelToken = s_activeSessionToken;

    // Send a disconnect packet if we have a peer
    if (cancelToken != 0 &&
        (s_state == SessionState::Connected ||
         s_state == SessionState::Ready ||
         s_state == SessionState::Handshaking)) {
        DisconnectPayload dp;
        dp.reason_code = static_cast<uint16_t>(DisconnectReason::UserCancel);
        strncpy(dp.message, "Session canceled", sizeof(dp.message) - 1);
        dp.message[sizeof(dp.message) - 1] = '\0';
        QueueTypedPacket(CHANNEL_CONTROL, PacketType::Disconnect, &dp, sizeof(dp), true, "cancel");
        NetworkThread_RequestDisconnect(cancelToken, 0, false);
    }

    if (cancelToken != 0) {
        NetworkThread_RequestDestroyHost(cancelToken);
        NetworkThread_ClearQueues(cancelToken);
    } else {
        NetworkThread_RequestDestroyHost(0);
        NetworkThread_ClearQueues(0);
    }
    Rollback::NetplayLog_Write("NTHREAD", -1,
        "Session detached from network thread: token=%u",
        cancelToken);

    s_activeSessionToken = 0;
    ResetState();
}

void Session_SignalReady() {
    if (s_state != SessionState::Connected) {
        LOG_WARN("[Session] Cannot signal ready: not in Connected state");
        return;
    }

    s_localReady = true;
    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::Ready,
                          nullptr, 0, true, "ready")) {
        LOG_WARN("[Session] Failed to send Ready packet");
        Rollback::NetplayLog_Write("SESSION", -1,
            "ERROR: failed to send Ready packet");
        return;
    }
    LOG_INFO("[Session] Signaled Ready (local)");
    Rollback::NetplayLog_Write("SESSION", -1,
        "Local Ready sent: remote_ready=%d", s_remoteReady ? 1 : 0);

    if (s_localReady && s_remoteReady) {
        SetState(SessionState::Ready);
    }
}

void Session_Update() {
    // Game thread owns packet interpretation and callback dispatch. Network
    // thread only enqueues transport events.
    DrainNetworkEvents();

    if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
        CheckTimeouts();
    }

    UpdateStats();
    UpdateStatusText();
}

bool Session_SendPacket(uint8_t channel, PacketType type,
                        const void* payload, size_t payloadLen, bool reliable) {
    if (s_activeSessionToken == 0 ||
        s_peerToken == 0 ||
        (s_state != SessionState::Connected && s_state != SessionState::Ready)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "DROP send: ch=%u type=%s payload=%zu reliable=%d state=%s role=%s peer=0x%llX token=%u",
            channel,
            PacketTypeName(type),
            payloadLen,
            reliable ? 1 : 0,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            (unsigned long long)s_peerToken,
            s_activeSessionToken);
        return false;
    }

    return QueueTypedPacket(channel, type, payload, payloadLen, reliable, "session-send");
}

void Session_SetPacketCallback(PacketCallback cb) {
    Rollback::NetplayLog_Write("SESSION", -1,
        "Packet callback change: old=0x%llX new=0x%llX state=%s role=%s",
        (unsigned long long)(uintptr_t)s_packetCallback,
        (unsigned long long)(uintptr_t)cb,
        SessionStateName(s_state),
        SessionRoleName(s_role));
    Rollback::NetplayLog_Flush();
    s_packetCallback = cb;
    FlushDeferredControlPackets();
}

void Session_GetSnapshot(SessionSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = (s_state != SessionState::Idle);
    out->state  = s_state;
    out->role   = s_role;
    out->remote_peer = s_remotePeer;
    out->stats  = s_stats;
    out->local_ready  = s_localReady;
    out->remote_ready = s_remoteReady;
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
    return s_state == SessionState::Connected ||
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
