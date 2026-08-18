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
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace Net {

// ============================================================================
// Static state
// ============================================================================

static bool       s_globalInit = false;
static ENetHost*  s_enetHost       = nullptr;

namespace {

// Fast cadence during the punch window; qoh99's steady-state values after it.
constexpr uint32_t AUTOPUNCH_REGISTER_INTERVAL_MS  = 500;
constexpr uint32_t AUTOPUNCH_LOOKUP_INTERVAL_MS    = 500;
constexpr uint32_t AUTOPUNCH_DIRECT_INTERVAL_MS    = 125;
constexpr uint32_t AUTOPUNCH_ACTIVE_WINDOW_MS      = 10000;
constexpr uint32_t AUTOPUNCH_KEEPALIVE_INTERVAL_MS = 2000;

// Idle cadence: 2000ms is well inside the shortest NAT mapping lifetimes.
constexpr uint32_t AUTOPUNCH_IDLE_REGISTER_INTERVAL_MS = 2000;
constexpr uint32_t AUTOPUNCH_IDLE_LOOKUP_INTERVAL_MS   = 1000;
constexpr uint32_t AUTOPUNCH_IDLE_DIRECT_INTERVAL_MS   = 250;

// Failed name resolution backs off; enet_address_set_host blocks the caller.
constexpr uint32_t AUTOPUNCH_RESOLVE_RETRY_BASE_MS = 1000;
constexpr uint32_t AUTOPUNCH_RESOLVE_RETRY_MAX_MS  = 30000;

// Keepalive wire format: [magic 4 | connectID 4 | reserved 4]. The leading
// 0xFF 0xFF parses on a pre-keepalive build as peerID 0xFFF with the
// COMPRESSED header flag set, which ENet discards silently when no compressor
// is installed — old builds ignore these packets without side effects.
constexpr uint8_t AUTOPUNCH_KEEPALIVE_MAGIC[4]    = {0xFF, 0xFF, 'A', 'K'};
constexpr size_t  AUTOPUNCH_KEEPALIVE_PACKET_SIZE = 12;

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
    uint32_t    last_keepalive_ms;
    uint32_t    relay_register_sent;
    uint32_t    relay_lookup_sent;
    uint32_t    direct_punch_sent;
    uint32_t    relay_mappings_received;
    uint32_t    keepalive_sent;
    uint32_t    keepalive_received;
    uint32_t    rebind_heals;
    bool        active_window_done;
    uint32_t    relay_resolve_fail_ms;      // last failed attempt
    uint32_t    relay_resolve_fails;
    uint32_t    target_resolve_fail_ms;
    uint32_t    target_resolve_fails;
    // GetTickCount of the last AUTHENTICATED keepalive accepted from the
    // connected peer (connectID verified). Feeds transport2's
    // protocol_silence_ms so autopunch keepalives count as liveness (INV-14).
    uint32_t    last_authenticated_inbound_ms;
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

// Exponential backoff after a failed resolve, capped.
static bool ResolveBackoffElapsed(uint32_t lastFailMs, uint32_t fails) {
    if (fails == 0 || lastFailMs == 0) return true;
    uint32_t wait = AUTOPUNCH_RESOLVE_RETRY_BASE_MS;
    for (uint32_t i = 1; i < fails && wait < AUTOPUNCH_RESOLVE_RETRY_MAX_MS; ++i) {
        wait *= 2;
    }
    if (wait > AUTOPUNCH_RESOLVE_RETRY_MAX_MS) wait = AUTOPUNCH_RESOLVE_RETRY_MAX_MS;
    return (GetTickCount() - lastFailMs) >= wait;
}

static bool AutopunchResolveRelay(AutopunchState& state) {
    if (!state.enabled) {
        return false;
    }
    if (state.relay_resolved) {
        return true;
    }
    if (!ResolveBackoffElapsed(state.relay_resolve_fail_ms, state.relay_resolve_fails)) {
        return false;
    }

    state.relay_resolved = ResolveEnetAddress(
        state.relay_host,
        state.relay_port,
        &state.relay,
        "Autopunch relay");
    if (!state.relay_resolved) {
        state.relay_resolve_fail_ms = GetTickCount();
        ++state.relay_resolve_fails;
    } else {
        state.relay_resolve_fails = 0;
    }
    return state.relay_resolved;
}

static bool AutopunchResolveTarget(AutopunchState& state) {
    if (!state.enabled || !state.target_host[0] || state.target_port == 0) {
        return false;
    }
    if (state.target_resolved) {
        return true;
    }
    if (!ResolveBackoffElapsed(state.target_resolve_fail_ms, state.target_resolve_fails)) {
        return false;
    }

    state.target_resolved = ResolveEnetAddress(
        state.target_host,
        state.target_port,
        &state.target,
        "Autopunch target");
    if (!state.target_resolved) {
        state.target_resolve_fail_ms = GetTickCount();
        ++state.target_resolve_fails;
    } else {
        state.target_resolve_fails = 0;
    }
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
        Rollback::NetplayLog_Write("ENET", -1,
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
        Rollback::NetplayLog_Write("ENET", -1,
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

static ENetPeer* FindSingleConnectedPeer(ENetHost* host) {
    if (!host) {
        return nullptr;
    }
    ENetPeer* found = nullptr;
    for (size_t i = 0; i < host->peerCount; i++) {
        ENetPeer* peer = &host->peers[i];
        if (peer->state != ENET_PEER_STATE_CONNECTED) {
            continue;
        }
        if (found) {
            return nullptr;  // Ambiguous: refuse rather than guess.
        }
        found = peer;
    }
    return found;
}

static bool AutopunchSendKeepalive(AutopunchState& state, ENetPeer* peer) {
    if (!peer || peer->connectID == 0) {
        return false;
    }

    uint8_t payload[AUTOPUNCH_KEEPALIVE_PACKET_SIZE] = {};
    memcpy(payload, AUTOPUNCH_KEEPALIVE_MAGIC, sizeof(AUTOPUNCH_KEEPALIVE_MAGIC));
    const uint32_t connectId = peer->connectID;
    memcpy(payload + sizeof(AUTOPUNCH_KEEPALIVE_MAGIC), &connectId, sizeof(connectId));

    const int sent = RawSendToAddress(
        state,
        peer->address,
        payload,
        sizeof(payload),
        "Autopunch keepalive");
    if (sent == (int)sizeof(payload)) {
        state.keepalive_sent++;
        char target[96] = {};
        Rollback::NetplayLog_Verbose("ENET", -1,
            "%s Autopunch keepalive sent: target=%s connect_id=0x%08X total=%u",
            AutopunchLabel(state),
            FormatEnetAddress(peer->address, target, sizeof(target)),
            (unsigned)connectId,
            state.keepalive_sent);
        return true;
    }
    return false;
}

static void AutopunchHandleKeepalive(AutopunchState& state,
                                     ENetHost* host,
                                     const ENetAddress& from,
                                     uint32_t connectId) {
    state.keepalive_received++;

    // Rebind healing is restricted to the gameplay host with exactly one
    // connected peer; spectator hosts multiplex peers and a wrong heal there
    // could hijack an unrelated client's slot.
    if (host != s_enetHost) {
        return;
    }

    ENetPeer* peer = FindSingleConnectedPeer(host);
    if (!peer || peer->connectID == 0 || peer->connectID != connectId) {
        return;
    }

    // connectID matched the live peer: this is genuine peer traffic.
    state.last_authenticated_inbound_ms = GetTickCount();

    if (from.host == peer->address.host && from.port == peer->address.port) {
        return;  // Endpoint unchanged: plain keepalive.
    }

    // connectID travels in cleartext, so it is not strong enough to accept a
    // full IP migration; heal only same-IP port rebinds.
    if (from.host != peer->address.host) {
        char fromText[96] = {};
        Rollback::NetplayLog_Write("ENET", -1,
            "%s Autopunch keepalive from foreign IP ignored: from=%s connect_id=0x%08X",
            AutopunchLabel(state),
            FormatEnetAddress(from, fromText, sizeof(fromText)),
            (unsigned)connectId);
        return;
    }

    const ENetAddress oldAddress = peer->address;
    peer->address = from;
    state.rebind_heals++;

    char oldText[96] = {};
    char newText[96] = {};
    Rollback::NetplayLog_Write("ENET", -1,
        "%s NAT rebind healed: peer=%p endpoint %s -> %s connect_id=0x%08X heals=%u",
        AutopunchLabel(state),
        peer,
        FormatEnetAddress(oldAddress, oldText, sizeof(oldText)),
        FormatEnetAddress(from, newText, sizeof(newText)),
        (unsigned)connectId,
        state.rebind_heals);
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
        Rollback::NetplayLog_Write("ENET", -1,
            "%s Autopunch consumed direct punch from %s",
            AutopunchLabel(*state),
            FormatEnetAddress(from, fromText, sizeof(fromText)));
        return 1;
    }

    if (len == AUTOPUNCH_KEEPALIVE_PACKET_SIZE &&
        memcmp(data, AUTOPUNCH_KEEPALIVE_MAGIC, sizeof(AUTOPUNCH_KEEPALIVE_MAGIC)) == 0) {
        uint32_t connectId = 0;
        memcpy(&connectId, data + sizeof(AUTOPUNCH_KEEPALIVE_MAGIC), sizeof(connectId));
        AutopunchHandleKeepalive(*state, host, from, connectId);
        return 1;  // Ours either way — never let ENet parse it.
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
// Fault injection (M6 verification) — OUTBOUND application packets only.
//
// Controlled by environment variables read ONCE at Transport_GlobalInit:
//   AS2_NET_INJECT_DROP_PCT            0-100, random per-packet egress drop
//   AS2_NET_INJECT_BLACKOUT_MS         blackout window length (ms)
//   AS2_NET_INJECT_BLACKOUT_PERIOD_MS  blackout repeat period (ms, > window)
//   AS2_NET_INJECT_DELAY_MS            parsed, NOT implemented (see below)
//
// The hook lives in Transport_Send only; Transport_SendTyped funnels through
// it, so both entry points are covered by one check. A dropped packet is
// swallowed BEFORE enet_packet_create and reported as success to the caller —
// simulated loss must be invisible to the sender, exactly like the real wire.
//
// Deliberately NOT affected by injection:
//   - Autopunch keepalives / punch bursts / relay traffic: those ride
//     RawSendToAddress → enet_socket_send, below this layer.
//   - ENet protocol internals (acks, pings, connection management): generated
//     inside enet_host_service, never pass through Transport_Send.
// This is intentional — the ConnectionSupervisor measures APPLICATION inbound
// silence (Session-level receive events), so app-egress blackout on the peer
// produces a genuine supervisor-visible outage here even though ENet's own
// low-level channel stays up.
//
// Caveat: RELIABLE packets dropped here are lost permanently (ENet never saw
// them, so it cannot retransmit). Real wire loss of a reliable packet heals
// via ENet retransmission. Blackout tests remain valid (a cable pull also
// stops retransmits from crossing), but sustained DROP_PCT is harsher than
// real loss for reliable control traffic. Documented in
// docs/RESILIENCE_TESTING.md.
//
// AS2_NET_INJECT_DELAY_MS is intentionally NOT implemented: Transport_Send
// executes on the network worker thread, so an inline wait would block
// servicing; a correct fixed delay needs a timed egress queue drained in
// Transport_Service with cross-frame ENetPeer lifetime handling (stale-peer
// hazard on host teardown), which exceeds this additive hook's budget. The
// variable is detected and a warning is logged so a configured-but-ignored
// delay is never silent.
//
// Each instance shapes only its own egress. Simulating bidirectional loss
// requires the env vars set on BOTH game instances. Blackout scheduling uses
// raw GetTickCount() modulo the period — GetTickCount is ms-since-boot, so
// two instances on the SAME machine share one clock and their blackout
// windows align automatically.
//
// Thread model: state is written by InjectInitFromEnv (Transport_GlobalInit)
// and afterwards touched only from Transport_Send, whose calls are serialized
// on the transport owner thread per this file's contract. No lock needed.
// Default: fully OFF — with no env vars set, the only added cost in
// Transport_Send is a single bool test.
// ============================================================================

namespace {

struct FaultInjectState {
    bool     enabled;               // any injection configured
    uint32_t drop_pct;              // 0-100 random egress drop
    uint32_t blackout_ms;           // blackout window length
    uint32_t blackout_period_ms;    // blackout repeat period
    bool     in_blackout;           // edge tracking for enter/exit logs
    uint32_t blackout_entered_ms;   // tick at window entry (for exit log)
    uint32_t blackout_entered_drop; // dropped_blackout at window entry
    uint32_t rng;                   // LCG state (MSVC rand constants)
    uint32_t dropped_random;        // lifetime counters
    uint32_t dropped_blackout;
};

static FaultInjectState s_inject = {};

static uint32_t InjectReadEnvU32(const char* name, bool* outPresent) {
    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (outPresent) {
        *outPresent = (n > 0 && n < sizeof(buf));
    }
    if (n == 0 || n >= sizeof(buf)) {
        return 0;
    }
    const long v = strtol(buf, nullptr, 10);
    return v > 0 ? (uint32_t)v : 0;
}

static void InjectInitFromEnv() {
    memset(&s_inject, 0, sizeof(s_inject));

    bool present = false;
    s_inject.drop_pct = InjectReadEnvU32("AS2_NET_INJECT_DROP_PCT", &present);
    if (s_inject.drop_pct > 100) {
        s_inject.drop_pct = 100;
    }
    s_inject.blackout_ms        = InjectReadEnvU32("AS2_NET_INJECT_BLACKOUT_MS", &present);
    s_inject.blackout_period_ms = InjectReadEnvU32("AS2_NET_INJECT_BLACKOUT_PERIOD_MS", &present);

    // Blackout needs both knobs and a period strictly larger than the window.
    if (s_inject.blackout_ms != 0 &&
        (s_inject.blackout_period_ms == 0 ||
         s_inject.blackout_period_ms <= s_inject.blackout_ms)) {
        LOG_WARN("[Net] Fault injection: invalid blackout config (window=%ums period=%ums) -- blackout disabled",
                 s_inject.blackout_ms, s_inject.blackout_period_ms);
        Rollback::NetplayLog_Write("INJECT", -1,
            "Blackout config invalid: window=%u period=%u (need period > window) -- blackout disabled",
            s_inject.blackout_ms, s_inject.blackout_period_ms);
        s_inject.blackout_ms = 0;
        s_inject.blackout_period_ms = 0;
    }

    bool delayPresent = false;
    const uint32_t delayMs = InjectReadEnvU32("AS2_NET_INJECT_DELAY_MS", &delayPresent);
    if (delayPresent && delayMs > 0) {
        LOG_WARN("[Net] Fault injection: AS2_NET_INJECT_DELAY_MS=%u is NOT implemented and will be ignored "
                 "(no non-blocking delay point on the transport worker)", delayMs);
        Rollback::NetplayLog_Write("INJECT", -1,
            "AS2_NET_INJECT_DELAY_MS=%u ignored: delay injection not implemented", delayMs);
    }

    s_inject.enabled = (s_inject.drop_pct > 0) ||
                       (s_inject.blackout_ms > 0 && s_inject.blackout_period_ms > 0);
    if (!s_inject.enabled) {
        return;  // Fully off: zero log noise, single bool check per send.
    }

    s_inject.rng = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ 0x5F3759DFu;

    LOG_WARN("[Net] FAULT INJECTION ACTIVE (outbound only): drop_pct=%u blackout=%ums/%ums",
             s_inject.drop_pct, s_inject.blackout_ms, s_inject.blackout_period_ms);
    Rollback::NetplayLog_Write("INJECT", -1,
        "Fault injection active: drop_pct=%u blackout_window_ms=%u blackout_period_ms=%u "
        "(outbound app packets only; autopunch keepalives and ENet acks/pings unaffected)",
        s_inject.drop_pct, s_inject.blackout_ms, s_inject.blackout_period_ms);
}

// Decide whether to swallow the current outbound packet. Called only when
// s_inject.enabled. Blackout enter/exit logging is edge-triggered here, so
// window boundaries are logged at the first send attempt inside/outside the
// window (not at the exact wall-clock boundary) — good enough for test logs.
static bool InjectShouldDropOutbound() {
    if (s_inject.blackout_ms != 0) {
        const uint32_t now = GetTickCount();
        const bool black = (now % s_inject.blackout_period_ms) < s_inject.blackout_ms;
        if (black != s_inject.in_blackout) {
            s_inject.in_blackout = black;
            if (black) {
                s_inject.blackout_entered_ms   = now;
                s_inject.blackout_entered_drop = s_inject.dropped_blackout;
                LOG_WARN("[Net] Injection blackout ENTER (window=%ums period=%ums)",
                         s_inject.blackout_ms, s_inject.blackout_period_ms);
                Rollback::NetplayLog_Write("INJECT", -1,
                    "Blackout ENTER: window=%ums period=%ums tick=%u",
                    s_inject.blackout_ms, s_inject.blackout_period_ms, now);
            } else {
                LOG_WARN("[Net] Injection blackout EXIT after ~%ums (%u packets swallowed)",
                         now - s_inject.blackout_entered_ms,
                         s_inject.dropped_blackout - s_inject.blackout_entered_drop);
                Rollback::NetplayLog_Write("INJECT", -1,
                    "Blackout EXIT: held ~%ums swallowed=%u total_blackout_drops=%u",
                    now - s_inject.blackout_entered_ms,
                    s_inject.dropped_blackout - s_inject.blackout_entered_drop,
                    s_inject.dropped_blackout);
            }
        }
        if (black) {
            s_inject.dropped_blackout++;
            return true;
        }
    }

    if (s_inject.drop_pct != 0) {
        s_inject.rng = s_inject.rng * 214013u + 2531011u;
        if (((s_inject.rng >> 16) % 100u) < s_inject.drop_pct) {
            s_inject.dropped_random++;
            if ((s_inject.dropped_random % 500u) == 1u) {
                Rollback::NetplayLog_Write("INJECT", -1,
                    "Random egress drops so far: %u (drop_pct=%u)",
                    s_inject.dropped_random, s_inject.drop_pct);
            }
            return true;
        }
    }
    return false;
}

} // namespace

bool Transport_FaultInjectionActive() {
    return s_inject.enabled;
}

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

    // M6 injection hook: read AS2_NET_INJECT_* env vars once. No-op (single
    // bool per send) when none are set.
    InjectInitFromEnv();
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
            "%s Autopunch stop: reason=%s registers=%u lookups=%u direct=%u mappings=%u keepalives=%u/%u heals=%u",
            AutopunchLabel(*it),
            reason && reason[0] ? reason : "?",
            it->relay_register_sent,
            it->relay_lookup_sent,
            it->direct_punch_sent,
            it->relay_mappings_received,
            it->keepalive_sent,
            it->keepalive_received,
            it->rebind_heals);
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

uint32_t Transport_AutopunchLastInboundTickMs(ENetHost* enetHost) {
    std::lock_guard<std::mutex> lock(s_autopunchMutex);
    const AutopunchState* state = FindAutopunchStateLocked(enetHost);
    return state ? state->last_authenticated_inbound_ms : 0;
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
                "%s Autopunch peer connected; relay helper quiet, keepalive every %ums",
                AutopunchLabel(*state),
                AUTOPUNCH_KEEPALIVE_INTERVAL_MS);
        }
        // Low-rate keepalive while connected: keeps the NAT mapping warm and
        // gives the remote intercept authenticated datagrams to detect a NAT
        // rebind from (ENet itself drops packets from a changed source).
        if (state->last_keepalive_ms == 0 ||
            nowMs - state->last_keepalive_ms >= AUTOPUNCH_KEEPALIVE_INTERVAL_MS) {
            state->last_keepalive_ms = nowMs;
            if (ENetPeer* peer = FindSingleConnectedPeer(state->host)) {
                AutopunchSendKeepalive(*state, peer);
            }
        }
        return;
    }

    state->last_keepalive_ms = 0;

    // Log the window elapsing once, then decay to the idle cadence. started_ms
    // is no longer restamped, so ageMs stays a true age.
    const uint32_t ageMs = nowMs - state->started_ms;
    if (!state->active_window_done && ageMs > AUTOPUNCH_ACTIVE_WINDOW_MS) {
        state->active_window_done = true;
        Rollback::NetplayLog_Write("ENET", -1,
            "%s Autopunch active window elapsed: age=%ums registers=%u lookups=%u direct=%u mappings=%u (idle cadence now %ums)",
            AutopunchLabel(*state),
            ageMs,
            state->relay_register_sent,
            state->relay_lookup_sent,
            state->direct_punch_sent,
            state->relay_mappings_received,
            AUTOPUNCH_IDLE_REGISTER_INTERVAL_MS);
    }

    const uint32_t registerInterval = state->active_window_done
        ? AUTOPUNCH_IDLE_REGISTER_INTERVAL_MS : AUTOPUNCH_REGISTER_INTERVAL_MS;
    const uint32_t lookupInterval = state->active_window_done
        ? AUTOPUNCH_IDLE_LOOKUP_INTERVAL_MS : AUTOPUNCH_LOOKUP_INTERVAL_MS;
    const uint32_t directInterval = state->active_window_done
        ? AUTOPUNCH_IDLE_DIRECT_INTERVAL_MS : AUTOPUNCH_DIRECT_INTERVAL_MS;

    // Registration never stops: a listen-only host depends on the relay
    // answering someone else's lookup. Only the rate decays.
    if (state->last_register_ms == 0 ||
        nowMs - state->last_register_ms >= registerInterval) {
        state->last_register_ms = nowMs;
        AutopunchSendRegister(*state);
    }

    if (state->target_host[0]) {
        if (state->last_lookup_ms == 0 ||
            nowMs - state->last_lookup_ms >= lookupInterval) {
            state->last_lookup_ms = nowMs;
            AutopunchSendLookup(*state);
        }

        if (AutopunchResolveTarget(*state) &&
            (state->last_direct_ms == 0 ||
             nowMs - state->last_direct_ms >= directInterval)) {
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

void Transport_ConfigurePeerResilience(ENetPeer* peer) {
    if (!peer) return;

    // ENet defaults were never tuned for netplay: the 5s timeout minimum
    // meant ~5s of sustained loss killed the link, and the 500ms ping
    // interval starts the death clock late. 10s minimum / 30s maximum with a
    // 150ms ping keeps ENet's own verdict well behind the mod's
    // ConnectionSupervisor, which owns liveness.
    enet_peer_timeout(peer, 0, 10000, 30000);
    enet_peer_ping_interval(peer, 150);

    // Disable ENet's unreliable-packet throttle. Under sustained RTT growth
    // it deliberately drops unreliable packets locally — for rollback input
    // traffic (redundant by design) that just manufactures loss during ping
    // spikes.
    enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, 0, 0);
    peer->packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
    peer->packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;

    LOG_INFO("[Net] Peer resilience configured: timeout=10000/30000ms ping=150ms throttle=off");
    Rollback::NetplayLog_Write("ENET", -1,
        "Peer resilience configured: peer=%p timeout_min=10000 timeout_max=30000 ping_interval=150 throttle=disabled",
        peer);
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

    // M6 injection hook: swallow the packet before ENet sees it and report
    // success — simulated loss must look like real wire loss to the caller.
    // Covers Transport_SendTyped too (it funnels through here). Autopunch
    // keepalives and ENet acks/pings do not pass through this path.
    if (s_inject.enabled && InjectShouldDropOutbound()) {
        return true;
    }

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
