/**
 * Alice Senki 2 - ENet Transport Layer (Implementation)
 */

#include <enet/enet.h>   // Must be before anything that pulls in windows.h
#include <windows.h>
#include <ws2tcpip.h>

#include "net/enet_transport.h"
#include "log_window.h"
#include "rollback/netplay_log.h"

#include <mutex>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace Net {

// ============================================================================
// Static state
// ============================================================================

static bool       s_globalInit = false;
static ENetHost*  s_enetHost       = nullptr;

namespace {

constexpr uint32_t AUTOPUNCH_REGISTER_INTERVAL_MS = 500;
constexpr uint32_t AUTOPUNCH_LOOKUP_INTERVAL_MS   = 500;
constexpr uint32_t AUTOPUNCH_DIRECT_INTERVAL_MS   = 125;
constexpr uint32_t AUTOPUNCH_ACTIVE_WINDOW_MS     = 10000;

struct AutopunchState {
    ENetHost*   host;
    bool        enabled;
    bool        relay_resolved;
    bool        target_resolved;
    bool        connected_logged;
    ENetAddress relay;
    ENetAddress target;
    char        label[32];
    char        relay_host[96];
    char        target_host[96];
    uint16_t    relay_port;
    uint16_t    target_port;
    uint16_t    local_port;
    uint32_t    started_ms;
    uint32_t    last_register_ms;
    uint32_t    last_lookup_ms;
    uint32_t    last_direct_ms;
    uint32_t    relay_register_sent;
    uint32_t    relay_lookup_sent;
    uint32_t    direct_punch_sent;
    uint32_t    relay_mappings_received;
};

static std::mutex s_autopunchMutex;
static std::vector<AutopunchState> s_autopunchStates;

static const char* FormatEnetAddress(const ENetAddress& address, char* out, size_t outCap) {
    if (!out || outCap == 0) {
        return "";
    }

    char ip[64] = {};
    if (enet_address_get_host_ip(&address, ip, sizeof(ip)) != 0 || !ip[0]) {
        _snprintf_s(ip, sizeof(ip), _TRUNCATE, "0x%08X", (unsigned)address.host);
    }
    _snprintf_s(out, outCap, _TRUNCATE, "%s:%u", ip, (unsigned)address.port);
    return out;
}

static bool ResolveEnetAddress(const char* host, uint16_t port,
                               ENetAddress* out, const char* context) {
    if (!host || !host[0] || port == 0 || !out) {
        return false;
    }

    ENetAddress address{};
    address.port = port;
    if (enet_address_set_host(&address, host) < 0) {
        Rollback::NetplayLog_Write("ENET", -1,
            "%s resolve failed: host=%s port=%u",
            context ? context : "Endpoint",
            host,
            (unsigned)port);
        return false;
    }

    *out = address;
    char formatted[96] = {};
    Rollback::NetplayLog_Write("ENET", -1,
        "%s resolved: %s -> %s",
        context ? context : "Endpoint",
        host,
        FormatEnetAddress(address, formatted, sizeof(formatted)));
    return true;
}

static const char* AutopunchLabel(const AutopunchState& state) {
    return state.label[0] ? state.label : "ENET";
}

static AutopunchState* FindAutopunchStateLocked(ENetHost* host) {
    if (!host) {
        return nullptr;
    }
    for (auto& state : s_autopunchStates) {
        if (state.host == host) {
            return &state;
        }
    }
    return nullptr;
}

static AutopunchState* EnsureAutopunchStateLocked(ENetHost* host) {
    if (!host) {
        return nullptr;
    }
    if (AutopunchState* existing = FindAutopunchStateLocked(host)) {
        return existing;
    }
    AutopunchState state{};
    state.host = host;
    s_autopunchStates.push_back(state);
    return &s_autopunchStates.back();
}

static int RawSendToAddress(AutopunchState& state,
                            const ENetAddress& address,
                            const void* data, size_t length,
                            const char* context) {
    if (!state.host || !data || length == 0) {
        return -1;
    }

    ENetBuffer buffer{};
    buffer.data = const_cast<void*>(data);
    buffer.dataLength = length;

    const int sent = enet_socket_send(state.host->socket, &address, &buffer, 1);
    if (sent < 0) {
        char formatted[96] = {};
        Rollback::NetplayLog_Write("ENET", -1,
            "%s %s raw send failed: target=%s len=%zu wsa=%d",
            AutopunchLabel(state),
            context ? context : "UDP",
            FormatEnetAddress(address, formatted, sizeof(formatted)),
            length,
            WSAGetLastError());
    }
    return sent;
}

static bool AutopunchResolveRelay(AutopunchState& state) {
    if (!state.enabled) {
        return false;
    }
    if (state.relay_resolved) {
        return true;
    }

    state.relay_resolved = ResolveEnetAddress(
        state.relay_host,
        state.relay_port,
        &state.relay,
        "Autopunch relay");
    return state.relay_resolved;
}

static bool AutopunchResolveTarget(AutopunchState& state) {
    if (!state.enabled || !state.target_host[0] || state.target_port == 0) {
        return false;
    }
    if (state.target_resolved) {
        return true;
    }

    state.target_resolved = ResolveEnetAddress(
        state.target_host,
        state.target_port,
        &state.target,
        "Autopunch target");
    return state.target_resolved;
}

static bool AutopunchSendRegister(AutopunchState& state) {
    if (!AutopunchResolveRelay(state) || state.local_port == 0) {
        return false;
    }

    uint8_t payload[2] = {
        (uint8_t)(state.local_port >> 8),
        (uint8_t)(state.local_port & 0xFF)
    };

    const int sent = RawSendToAddress(
        state,
        state.relay,
        payload,
        sizeof(payload),
        "Autopunch register");
    if (sent == (int)sizeof(payload)) {
        state.relay_register_sent++;
        Rollback::NetplayLog_Verbose("ENET", -1,
            "%s Autopunch register sent: local_port=%u count=%u",
            AutopunchLabel(state),
            (unsigned)state.local_port,
            state.relay_register_sent);
        return true;
    }
    return false;
}

static bool AutopunchSendLookup(AutopunchState& state) {
    if (!AutopunchResolveRelay(state) || !AutopunchResolveTarget(state) || state.local_port == 0) {
        return false;
    }

    uint8_t payload[8] = {
        (uint8_t)(state.local_port >> 8),
        (uint8_t)(state.local_port & 0xFF),
        0, 0, 0, 0,
        (uint8_t)(state.target_port >> 8),
        (uint8_t)(state.target_port & 0xFF)
    };
    memcpy(payload + 2, &state.target.host, sizeof(state.target.host));

    const int sent = RawSendToAddress(
        state,
        state.relay,
        payload,
        sizeof(payload),
        "Autopunch lookup");
    if (sent == (int)sizeof(payload)) {
        state.relay_lookup_sent++;
        char target[96] = {};
        Rollback::NetplayLog_Verbose("ENET", -1,
            "%s Autopunch lookup sent: target=%s advertised_port=%u count=%u",
            AutopunchLabel(state),
            FormatEnetAddress(state.target, target, sizeof(target)),
            (unsigned)state.target_port,
            state.relay_lookup_sent);
        return true;
    }
    return false;
}

static int AutopunchSendDirectPings(AutopunchState& state,
                                    const ENetAddress& target,
                                    int count,
                                    const char* context) {
    static const uint8_t kPunchPayload[1] = {0};
    int sentCount = 0;
    for (int i = 0; i < count; i++) {
        const int sent = RawSendToAddress(
            state,
            target,
            kPunchPayload,
            sizeof(kPunchPayload),
            context ? context : "Autopunch direct");
        if (sent == (int)sizeof(kPunchPayload)) {
            sentCount++;
        }
    }
    state.direct_punch_sent += (uint32_t)sentCount;
    return sentCount;
}

static bool IsPeerAddressRewriteCandidate(const AutopunchState& state,
                                          const ENetPeer* peer,
                                          enet_uint32 host,
                                          uint16_t internalPort) {
    if (!peer || peer->state == ENET_PEER_STATE_DISCONNECTED) {
        return false;
    }
    if (peer->address.host != host) {
        return false;
    }
    return peer->address.port == internalPort ||
           (state.target_resolved &&
            peer->address.port == state.target_port);
}

static void AutopunchApplyRelayMapping(AutopunchState& state,
                                       uint16_t internalPort,
                                       uint16_t natPort,
                                       enet_uint32 host) {
    ENetAddress mapped{};
    mapped.host = host;
    mapped.port = natPort;

    state.relay_mappings_received++;
    char mappedText[96] = {};
    Rollback::NetplayLog_Write("ENET", -1,
        "%s Autopunch relay mapping: internal_port=%u nat=%s mappings=%u",
        AutopunchLabel(state),
        (unsigned)internalPort,
        FormatEnetAddress(mapped, mappedText, sizeof(mappedText)),
        state.relay_mappings_received);

    const int punchSent = AutopunchSendDirectPings(state, mapped, 4, "Autopunch mapped direct");
    Rollback::NetplayLog_Write("ENET", -1,
        "%s Autopunch mapped direct burst: target=%s sent=%d",
        AutopunchLabel(state),
        FormatEnetAddress(mapped, mappedText, sizeof(mappedText)),
        punchSent);

    if (state.target_resolved &&
        state.target.host == host &&
        internalPort == state.target_port) {
        const uint16_t oldPort = state.target.port;
        state.target.port = natPort;
        if (oldPort != natPort) {
            Rollback::NetplayLog_Write("ENET", -1,
                "%s Autopunch target NAT port learned: %u -> %u",
                AutopunchLabel(state),
                (unsigned)oldPort,
                (unsigned)natPort);
        }
    }

    if (!state.host) {
        return;
    }

    for (size_t i = 0; i < state.host->peerCount; i++) {
        ENetPeer* peer = &state.host->peers[i];
        if (!IsPeerAddressRewriteCandidate(state, peer, host, internalPort)) {
            continue;
        }

        const uint16_t oldPort = peer->address.port;
        peer->address.port = natPort;
        Rollback::NetplayLog_Write("ENET", -1,
            "%s Autopunch rewrote ENet peer endpoint: peer=%p port=%u->%u state=%d",
            AutopunchLabel(state),
            peer,
            (unsigned)oldPort,
            (unsigned)natPort,
            (int)peer->state);
    }
}

static int ENET_CALLBACK AutopunchIntercept(ENetHost* host, ENetEvent*) {
    if (!host || !host->receivedData || host->receivedDataLength == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(s_autopunchMutex);
    AutopunchState* state = FindAutopunchStateLocked(host);
    if (!state || !state->enabled) {
        return 0;
    }

    const uint8_t* data = host->receivedData;
    const size_t len = host->receivedDataLength;
    const ENetAddress from = host->receivedAddress;

    if (len == 1 && data[0] == 0) {
        char fromText[96] = {};
        Rollback::NetplayLog_Verbose("ENET", -1,
            "%s Autopunch consumed direct punch from %s",
            AutopunchLabel(*state),
            FormatEnetAddress(from, fromText, sizeof(fromText)));
        return 1;
    }

    const bool fromRelay =
        state->relay_resolved &&
        from.host == state->relay.host &&
        from.port == state->relay.port;

    if (!fromRelay) {
        return 0;
    }

    if (len == 8) {
        uint32_t mappedHost = 0;
        memcpy(&mappedHost, data + 4, sizeof(mappedHost));
        const uint16_t internalPort = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
        const uint16_t natPort = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
        if (internalPort != 0 && natPort != 0 && mappedHost != 0) {
            AutopunchApplyRelayMapping(*state, internalPort, natPort, mappedHost);
        } else {
            Rollback::NetplayLog_Write("ENET", -1,
                "%s Autopunch ignored invalid relay mapping: internal=%u nat=%u host=0x%08X",
                AutopunchLabel(*state),
                (unsigned)internalPort,
                (unsigned)natPort,
                (unsigned)mappedHost);
        }
        return 1;
    }

    Rollback::NetplayLog_Write("ENET", -1,
        "%s Autopunch ignored relay payload with unexpected size: len=%zu",
        AutopunchLabel(*state),
        len);
    return 1;
}

} // namespace

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
    s_enetHost->intercept = AutopunchIntercept;

    uint16_t boundPort = 0;
    Transport_GetBoundPort(&boundPort);
    Rollback::NetplayLog_Write("ENET", -1,
        "Host created: requested_port=%u bound_port=%u max_peers=%u channels=%u intercept=autopunch",
        port,
        (unsigned)boundPort,
        maxPeers,
        NUM_CHANNELS);
    return true;
}

void Transport_DestroyHost() {
    if (!s_enetHost) return;

    Transport_AutopunchStop("host destroyed");

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

ENetPeer* Transport_Connect(const char* host, uint16_t port) {
    if (!s_enetHost) {
        LOG_ERROR("[Net] Transport_Connect: No host active");
        Rollback::NetplayLog_Write("ENET", -1, "Connect failed: no active host");
        return nullptr;
    }
    if (!host || !host[0]) {
        LOG_ERROR("[Net] Transport_Connect: empty host");
        Rollback::NetplayLog_Write("ENET", -1, "Connect failed: empty host");
        return nullptr;
    }

    ENetAddress address{};
    address.port = port;
    if (enet_address_set_host(&address, host) < 0) {
        LOG_ERROR("[Net] Failed to resolve endpoint '%s:%u' (ENet IPv4 transport)", host, port);
        Rollback::NetplayLog_Write("ENET", -1,
            "Connect resolve failed: host=%s port=%u", host, port);
        return nullptr;
    }

    ENetPeer* peer = enet_host_connect(s_enetHost, &address, NUM_CHANNELS, 0);
    if (!peer) {
        LOG_ERROR("[Net] enet_host_connect failed (%s:%u)", host, port);
        Rollback::NetplayLog_Write("ENET", -1,
            "Connect failed: host=%s port=%u", host, port);
        return nullptr;
    }

    LOG_INFO("[Net] Connecting to %s:%u...", host, port);
    Rollback::NetplayLog_Write("ENET", -1,
        "Connecting: peer=%p target=%s:%u channels=%u",
        peer,
        host,
        port,
        NUM_CHANNELS);
    return peer;
}

bool Transport_SendHolePunchBurst(const char* host, uint16_t port,
                                  int burstCount, uint32_t intervalMs) {
    return Transport_SendHolePunchBurstForHost(
        s_enetHost,
        host,
        port,
        burstCount,
        intervalMs);
}

bool Transport_SendHolePunchBurstForHost(ENetHost* enetHost,
                                         const char* host, uint16_t port,
                                         int burstCount, uint32_t intervalMs) {
    if (!host || !host[0] || port == 0 || burstCount <= 0) {
        return false;
    }
    if (!enetHost) {
        Rollback::NetplayLog_Write("ENET", -1,
            "Hole punch skipped: no active ENet host target=%s:%u",
            host,
            (unsigned)port);
        return false;
    }

    ENetAddress address{};
    if (!ResolveEnetAddress(host, port, &address, "Hole punch target")) {
        return false;
    }

    static const uint8_t kPunchPayload[1] = {0};
    int sentCount = 0;

    AutopunchState temp{};
    temp.host = enetHost;
    strncpy_s(temp.label, sizeof(temp.label), "BURST", _TRUNCATE);
    for (int i = 0; i < burstCount; i++) {
        const int sent = RawSendToAddress(
            temp,
            address,
            kPunchPayload,
            sizeof(kPunchPayload),
            "Hole punch direct");
        if (sent == (int)sizeof(kPunchPayload)) {
            sentCount++;
        }
        if (intervalMs > 0 && i + 1 < burstCount) {
            Sleep(intervalMs);
        }
    }

    uint16_t boundPort = 0;
    Transport_GetHostBoundPort(enetHost, &boundPort);

    char formatted[96] = {};
    Rollback::NetplayLog_Write("ENET", -1,
        "Hole punch burst sent from ENet socket: local_port=%u target=%s bursts=%d sent=%d",
        (unsigned)boundPort,
        FormatEnetAddress(address, formatted, sizeof(formatted)),
        burstCount,
        sentCount);

    return sentCount > 0;
}

bool Transport_GetBoundPort(uint16_t* outPort) {
    return Transport_GetHostBoundPort(s_enetHost, outPort);
}

bool Transport_GetHostBoundPort(ENetHost* host, uint16_t* outPort) {
    if (!outPort) {
        return false;
    }
    *outPort = 0;

    if (!host) {
        return false;
    }

    ENetAddress bound{};
    if (enet_socket_get_address(host->socket, &bound) == 0 && bound.port != 0) {
        *outPort = bound.port;
        return true;
    }

    if (host->address.port != 0) {
        *outPort = host->address.port;
        return true;
    }

    return false;
}

void Transport_AutopunchStart(const char* relayHost, uint16_t relayPort,
                              uint16_t localPort,
                              const char* targetHost, uint16_t targetPort) {
    Transport_AutopunchStartForHost(
        s_enetHost,
        "SESSION",
        relayHost,
        relayPort,
        localPort,
        targetHost,
        targetPort);
}

void Transport_AutopunchStartForHost(ENetHost* enetHost,
                                     const char* logLabel,
                                     const char* relayHost,
                                     uint16_t relayPort,
                                     uint16_t localPort,
                                     const char* targetHost,
                                     uint16_t targetPort) {
    if (!enetHost || !relayHost || !relayHost[0] || relayPort == 0 || localPort == 0) {
        Rollback::NetplayLog_Write("ENET", -1,
            "Autopunch start skipped: host_active=%d relay=%s:%u local_port=%u",
            enetHost ? 1 : 0,
            relayHost && relayHost[0] ? relayHost : "(none)",
            (unsigned)relayPort,
            (unsigned)localPort);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s_autopunchMutex);
        AutopunchState* state = EnsureAutopunchStateLocked(enetHost);
        if (!state) {
            return;
        }
        memset(state, 0, sizeof(*state));
        state->host = enetHost;
        state->enabled = true;
        state->relay_port = relayPort;
        state->local_port = localPort;
        state->started_ms = GetTickCount();
        strncpy_s(state->label, sizeof(state->label), logLabel && logLabel[0] ? logLabel : "ENET", _TRUNCATE);
        strncpy_s(state->relay_host, sizeof(state->relay_host), relayHost, _TRUNCATE);

        if (targetHost && targetHost[0] && targetPort != 0) {
            strncpy_s(state->target_host, sizeof(state->target_host), targetHost, _TRUNCATE);
            state->target_port = targetPort;
        }

        enetHost->intercept = AutopunchIntercept;

        Rollback::NetplayLog_Write("ENET", -1,
            "%s Autopunch start: local_port=%u relay=%s:%u target=%s:%u",
            AutopunchLabel(*state),
            (unsigned)state->local_port,
            state->relay_host,
            (unsigned)state->relay_port,
            state->target_host[0] ? state->target_host : "(listen-only)",
            (unsigned)state->target_port);
    }

    Transport_AutopunchServiceForHost(enetHost, GetTickCount(), false);
}

void Transport_AutopunchStop(const char* reason) {
    Transport_AutopunchStopForHost(s_enetHost, reason);
}

void Transport_AutopunchStopForHost(ENetHost* enetHost, const char* reason) {
    std::lock_guard<std::mutex> lock(s_autopunchMutex);
    for (auto it = s_autopunchStates.begin(); it != s_autopunchStates.end(); ++it) {
        if (it->host != enetHost) {
            continue;
        }

        Rollback::NetplayLog_Write("ENET", -1,
            "%s Autopunch stop: reason=%s registers=%u lookups=%u direct=%u mappings=%u",
            AutopunchLabel(*it),
            reason && reason[0] ? reason : "?",
            it->relay_register_sent,
            it->relay_lookup_sent,
            it->direct_punch_sent,
            it->relay_mappings_received);
        if (enetHost && enetHost->intercept == AutopunchIntercept) {
            enetHost->intercept = nullptr;
        }
        s_autopunchStates.erase(it);
        return;
    }
}

void Transport_AutopunchService(uint32_t nowMs, bool peerConnected) {
    Transport_AutopunchServiceForHost(s_enetHost, nowMs, peerConnected);
}

void Transport_AutopunchServiceForHost(ENetHost* enetHost,
                                       uint32_t nowMs,
                                       bool peerConnected) {
    std::lock_guard<std::mutex> lock(s_autopunchMutex);
    AutopunchState* state = FindAutopunchStateLocked(enetHost);
    if (!state || !state->enabled || !state->host) {
        return;
    }

    if (peerConnected) {
        if (!state->connected_logged) {
            state->connected_logged = true;
            Rollback::NetplayLog_Write("ENET", -1,
                "%s Autopunch peer connected; keeping relay helper quiet",
                AutopunchLabel(*state));
        }
        return;
    }

    const uint32_t ageMs = nowMs - state->started_ms;
    if (ageMs > AUTOPUNCH_ACTIVE_WINDOW_MS) {
        Rollback::NetplayLog_Write("ENET", -1,
            "%s Autopunch active window elapsed: age=%ums registers=%u lookups=%u direct=%u mappings=%u",
            AutopunchLabel(*state),
            ageMs,
            state->relay_register_sent,
            state->relay_lookup_sent,
            state->direct_punch_sent,
            state->relay_mappings_received);
        state->started_ms = nowMs;
    }

    if (state->last_register_ms == 0 ||
        nowMs - state->last_register_ms >= AUTOPUNCH_REGISTER_INTERVAL_MS) {
        state->last_register_ms = nowMs;
        AutopunchSendRegister(*state);
    }

    if (state->target_host[0]) {
        if (state->last_lookup_ms == 0 ||
            nowMs - state->last_lookup_ms >= AUTOPUNCH_LOOKUP_INTERVAL_MS) {
            state->last_lookup_ms = nowMs;
            AutopunchSendLookup(*state);
        }

        if (AutopunchResolveTarget(*state) &&
            (state->last_direct_ms == 0 ||
             nowMs - state->last_direct_ms >= AUTOPUNCH_DIRECT_INTERVAL_MS)) {
            state->last_direct_ms = nowMs;
            const int sent = AutopunchSendDirectPings(*state, state->target, 1, "Autopunch direct");
            if (sent > 0) {
                char target[96] = {};
                Rollback::NetplayLog_Verbose("ENET", -1,
                    "%s Autopunch direct ping sent: target=%s total=%u",
                    AutopunchLabel(*state),
                    FormatEnetAddress(state->target, target, sizeof(target)),
                    state->direct_punch_sent);
            }
        }
    }
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
