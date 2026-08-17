/**
 * Alice Senki 2 - transport2: ENet worker (Implementation, re0.7 M3)
 *
 * Body descends from network_thread.cpp (deleted at M3); differences are the
 * §2.2 additions: protocol_silence_ms, the pre-establishment liveness anchor,
 * and the Busy refusal of surplus inbound connects (edges C-5/C-6).
 */

#include <winsock2.h>     // Must be before windows.h
#include <windows.h>
#include <enet/enet.h>

#include "net/transport2.h"
#include "net/link_emulator.h"
#include "net/enet_transport.h"
#include "net/session_types.h"
#include "rollback/netplay_log.h"

#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <string.h>

namespace Net {

namespace {

enum class WorkerCommandType : uint8_t {
    StartHost,
    StartJoin,
    SendPacket,
    RequestDisconnect,
    RequestDestroyHost,
    ClearQueues,
};

struct WorkerCommand {
    WorkerCommandType type;
    uint32_t          session_token;
    uint16_t          listen_port;
    char              target_host[96];
    uint16_t          target_port;
    bool              send_hole_punch;
    char              punch_relay_host[96];
    uint16_t          punch_relay_port;
    uint8_t           channel;
    PacketType        packet_type;
    bool              reliable;
    size_t            payload_len;
    uint8_t           payload[MAX_PAYLOAD_SIZE];
    uint32_t          disconnect_data;
    bool              force_disconnect;
};

constexpr size_t MAX_PENDING_COMMANDS = 4096;
constexpr size_t MAX_PENDING_EVENTS   = 4096;
constexpr DWORD  TRANSPORT_SERVICE_WAIT_MS = 2;

static std::thread              s_worker;
static std::mutex               s_commandMutex;
static std::condition_variable  s_commandCv;
static std::deque<WorkerCommand> s_commands;

static std::mutex               s_eventMutex;
static std::deque<Transport2Event> s_events;

static std::mutex               s_statsMutex;
static Transport2Stats          s_stats{};
// Liveness anchor (QOH99 lesson 10): stamped on the first worker poll after a
// StartHost/StartJoin command is processed. Pre-establishment silence is
// measured from here, never from process start. Guarded by s_statsMutex.
static DWORD                    s_livenessAnchorTickMs = 0;
// Last valid ENet-protocol inbound tick, derived from the peer's
// lastReceiveTime each service pass; survives peer teardown so silence keeps
// growing (instead of resetting) after a detach. Guarded by s_statsMutex.
static DWORD                    s_lastProtocolInboundTickMs = 0;
static bool                     s_anyInboundSeen = false;

static std::atomic<bool>        s_initialized{false};
static std::atomic<bool>        s_stopRequested{false};

static void UpdateInboundDepthStatsLocked() {
    s_stats.inbound_queue_depth = (uint32_t)s_events.size();
}

static void UpdateOutboundDepthStatsLocked() {
    s_stats.outbound_queue_depth = (uint32_t)s_commands.size();
}

static void PushWorkerErrorEvent(uint32_t sessionToken, const char* msg) {
    Transport2Event ev{};
    ev.type = Transport2EventType::WorkerError;
    ev.session_token = sessionToken;
    ev.transport_tick_ms = GetTickCount();
    strncpy_s(ev.error_text, sizeof(ev.error_text), msg ? msg : "transport worker error", _TRUNCATE);

    std::lock_guard<std::mutex> eventLock(s_eventMutex);
    std::lock_guard<std::mutex> statsLock(s_statsMutex);
    if (s_events.size() >= MAX_PENDING_EVENTS) {
        s_stats.inbound_drop_count++;
        return;
    }
    s_events.push_back(ev);
    UpdateInboundDepthStatsLocked();
}

static void PushTransportEvent(const Transport2Event& ev) {
    std::lock_guard<std::mutex> eventLock(s_eventMutex);
    std::lock_guard<std::mutex> statsLock(s_statsMutex);
    if (ev.type == Transport2EventType::PacketReceived ||
        ev.type == Transport2EventType::Disconnected) {
        s_stats.last_inbound_packet_tick_ms = ev.transport_tick_ms;
    }
    if (s_events.size() >= MAX_PENDING_EVENTS) {
        s_stats.inbound_drop_count++;
        return;
    }
    s_events.push_back(ev);
    UpdateInboundDepthStatsLocked();
}

static bool EnqueueCommand(const WorkerCommand& cmd) {
    if (!s_initialized.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> commandLock(s_commandMutex);
        if (s_commands.size() >= MAX_PENDING_COMMANDS) {
            std::lock_guard<std::mutex> statsLock(s_statsMutex);
            s_stats.outbound_drop_count++;
            UpdateOutboundDepthStatsLocked();
            return false;
        }
        s_commands.push_back(cmd);
        std::lock_guard<std::mutex> statsLock(s_statsMutex);
        UpdateOutboundDepthStatsLocked();
    }

    s_commandCv.notify_one();
    return true;
}

static void CopyCommands(std::deque<WorkerCommand>* out) {
    if (!out) return;

    std::lock_guard<std::mutex> lock(s_commandMutex);
    if (s_commands.empty()) {
        return;
    }
    out->insert(out->end(), s_commands.begin(), s_commands.end());
    s_commands.clear();

    std::lock_guard<std::mutex> statsLock(s_statsMutex);
    UpdateOutboundDepthStatsLocked();
}

static void ResetLivenessAnchorLocked() {
    s_livenessAnchorTickMs = 0;   // re-stamped on the next worker poll
    s_lastProtocolInboundTickMs = 0;
    s_anyInboundSeen = false;
}

static void UpdateStats(bool hostActive, ENetPeer* peer) {
    std::lock_guard<std::mutex> lock(s_statsMutex);
    const DWORD now = GetTickCount();
    s_stats.host_active = hostActive;
    s_stats.peer_connected = (peer != nullptr);
    s_stats.last_service_tick_ms = now;

    if (s_livenessAnchorTickMs == 0) {
        // First poll after Init/Start*: anchor the liveness budget here.
        s_livenessAnchorTickMs = now;
    }

    if (peer) {
        s_stats.rtt_ms = Transport_GetPeerRTT(peer);
        s_stats.rtt_variance_ms = (float)peer->roundTripTimeVariance;
        s_stats.packets_sent = peer->packetsSent;
        s_stats.packets_lost = peer->packetsLost;
        s_stats.peer_reliable_in_transit = peer->reliableDataInTransit;

        // ENet-protocol liveness: lastReceiveTime advances on ANY inbound
        // command (acks of our pings included), in host serviceTime units.
        ENetHost* host = Transport_GetHost();
        uint32_t enetSilenceMs = 0;
        if (host && host->serviceTime >= peer->lastReceiveTime) {
            enetSilenceMs = host->serviceTime - peer->lastReceiveTime;
        }
        if (now >= enetSilenceMs) {
            s_lastProtocolInboundTickMs = now - enetSilenceMs;
        } else {
            s_lastProtocolInboundTickMs = now;
        }
        s_anyInboundSeen = true;

        // Authenticated autopunch keepalives never reach ENet (raw-socket
        // intercept) but are genuine peer traffic — fold them in (INV-14).
        const uint32_t punchInbound = Transport_AutopunchLastInboundTickMs(host);
        if (punchInbound != 0 &&
            (int32_t)(punchInbound - s_lastProtocolInboundTickMs) > 0) {
            s_lastProtocolInboundTickMs = punchInbound;
        }

        s_stats.protocol_silence_ms =
            (now >= s_lastProtocolInboundTickMs)
                ? (uint32_t)(now - s_lastProtocolInboundTickMs)
                : 0;
    } else {
        s_stats.rtt_ms = 0.0f;
        s_stats.rtt_variance_ms = 0.0f;
        s_stats.packets_sent = 0;
        s_stats.packets_lost = 0;
        s_stats.peer_reliable_in_transit = 0;
        if (s_anyInboundSeen && s_lastProtocolInboundTickMs != 0) {
            // Peer object gone but silence keeps growing from the last
            // genuine inbound — a detached peer must not look "fresh".
            s_stats.protocol_silence_ms =
                (now >= s_lastProtocolInboundTickMs)
                    ? (uint32_t)(now - s_lastProtocolInboundTickMs)
                    : 0;
        } else {
            s_stats.protocol_silence_ms = 0xFFFFFFFFu;
        }
    }
}

static void WorkerThreadMain() {
    ENetPeer* activePeer = nullptr;
    uint32_t activeSessionToken = 0;
    bool transportConnected = false;
    // Set when our own outbound connect is pending (join role): any OTHER
    // inbound connect during that window is refused Busy (edge C-5).
    ENetPeer* pendingOutboundPeer = nullptr;

    {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        memset(&s_stats, 0, sizeof(s_stats));
        s_stats.worker_running = true;
    }

    while (!s_stopRequested.load()) {
        // Wait briefly for new commands, but keep servicing ENet even if no
        // game-thread work is happening.
        {
            std::unique_lock<std::mutex> lock(s_commandMutex);
            s_commandCv.wait_for(lock, std::chrono::milliseconds(TRANSPORT_SERVICE_WAIT_MS),
                                 [] { return s_stopRequested.load() || !s_commands.empty(); });
        }

        std::deque<WorkerCommand> localCommands;
        CopyCommands(&localCommands);

        for (const WorkerCommand& cmd : localCommands) {
            switch (cmd.type) {
                case WorkerCommandType::StartHost: {
                    activeSessionToken = cmd.session_token;
                    activePeer = nullptr;
                    pendingOutboundPeer = nullptr;
                    transportConnected = false;
                    Transport_DestroyHost();
                    {
                        std::lock_guard<std::mutex> statsLock(s_statsMutex);
                        s_stats.last_inbound_packet_tick_ms = 0;
                        s_stats.last_outbound_packet_tick_ms = 0;
                        ResetLivenessAnchorLocked();
                    }
                    if (!Transport_CreateHost(cmd.listen_port)) {
                        PushWorkerErrorEvent(cmd.session_token, "Transport worker failed to create host");
                    } else {
                        uint16_t boundPort = cmd.listen_port;
                        Transport_GetBoundPort(&boundPort);
                        if (cmd.send_hole_punch) {
                            Transport_AutopunchStart(
                                cmd.punch_relay_host,
                                cmd.punch_relay_port,
                                boundPort,
                                nullptr,
                                0);
                        }
                        Rollback::NetplayLog_Write("NTHREAD", -1,
                            "Worker host created: token=%u listen_port=%u bound_port=%u autopunch=%d relay=%s:%u",
                            cmd.session_token,
                            cmd.listen_port,
                            (unsigned)boundPort,
                            cmd.send_hole_punch ? 1 : 0,
                            cmd.punch_relay_host,
                            (unsigned)cmd.punch_relay_port);
                    }
                    break;
                }

                case WorkerCommandType::StartJoin: {
                    activeSessionToken = cmd.session_token;
                    activePeer = nullptr;
                    pendingOutboundPeer = nullptr;
                    transportConnected = false;
                    Transport_DestroyHost();
                    {
                        std::lock_guard<std::mutex> statsLock(s_statsMutex);
                        s_stats.last_inbound_packet_tick_ms = 0;
                        s_stats.last_outbound_packet_tick_ms = 0;
                        ResetLivenessAnchorLocked();
                    }
                    bool hostReady = false;
                    if (cmd.listen_port > 0) {
                        hostReady = Transport_CreateHost(cmd.listen_port);
                        if (!hostReady) {
                            Rollback::NetplayLog_Write("NTHREAD", -1,
                                "Join bind on configured port failed, falling back to ephemeral: token=%u listen_port=%u",
                                cmd.session_token,
                                cmd.listen_port);
                        }
                    }
                    if (!hostReady) {
                        hostReady = Transport_CreateHost(0);
                    }
                    if (!hostReady) {
                        PushWorkerErrorEvent(cmd.session_token, "Transport worker failed to create join host");
                        break;
                    }
                    uint16_t boundPort = cmd.listen_port;
                    Transport_GetBoundPort(&boundPort);
                    if (cmd.send_hole_punch) {
                        Transport_AutopunchStart(
                            cmd.punch_relay_host,
                            cmd.punch_relay_port,
                            boundPort,
                            cmd.target_host,
                            cmd.target_port);
                        const bool burstOk = Transport_SendHolePunchBurst(
                            cmd.target_host,
                            cmd.target_port,
                            8,
                            5);
                        Rollback::NetplayLog_Write("NTHREAD", -1,
                            "Join hole-punch assist requested: token=%u local_port=%u target=%s:%u relay=%s:%u burst_ok=%d",
                            cmd.session_token,
                            (unsigned)boundPort,
                            cmd.target_host,
                            cmd.target_port,
                            cmd.punch_relay_host,
                            (unsigned)cmd.punch_relay_port,
                            burstOk ? 1 : 0);
                    }
                    ENetPeer* peer = Transport_Connect(cmd.target_host, cmd.target_port);
                    if (!peer) {
                        PushWorkerErrorEvent(cmd.session_token, "Transport worker failed to initiate connect");
                        Transport_DestroyHost();
                        activeSessionToken = 0;
                        break;
                    }
                    activePeer = peer;
                    pendingOutboundPeer = peer;
                    Rollback::NetplayLog_Write("NTHREAD", -1,
                        "Worker join connect initiated: token=%u target=%s:%u hole_punch=%d",
                        cmd.session_token,
                        cmd.target_host,
                        cmd.target_port,
                        cmd.send_hole_punch ? 1 : 0);
                    break;
                }

                case WorkerCommandType::SendPacket: {
                    if (cmd.session_token != activeSessionToken || !activePeer) {
                        break;
                    }
                    if (!Transport_SendTyped(activePeer,
                                             cmd.channel,
                                             cmd.packet_type,
                                             cmd.payload_len > 0 ? cmd.payload : nullptr,
                                             cmd.payload_len,
                                             cmd.reliable)) {
                        PushWorkerErrorEvent(cmd.session_token, "Transport worker failed to send packet");
                    } else {
                        {
                            std::lock_guard<std::mutex> statsLock(s_statsMutex);
                            s_stats.last_outbound_packet_tick_ms = GetTickCount();
                        }
                    }
                    break;
                }

                case WorkerCommandType::RequestDisconnect: {
                    if (cmd.session_token != activeSessionToken || !activePeer) {
                        break;
                    }
                    if (cmd.force_disconnect) {
                        Transport_ForceDisconnectPeer(activePeer);
                    } else {
                        Transport_DisconnectPeer(activePeer, cmd.disconnect_data);
                        Transport_Flush();
                    }
                    break;
                }

                case WorkerCommandType::RequestDestroyHost: {
                    if (cmd.session_token != 0 && cmd.session_token != activeSessionToken) {
                        break;
                    }
                    if (activePeer) {
                        Transport_ForceDisconnectPeer(activePeer);
                        activePeer = nullptr;
                    }
                    pendingOutboundPeer = nullptr;
                    transportConnected = false;
                    Transport_DestroyHost();
                    Rollback::NetplayLog_Write("NTHREAD", -1,
                        "Worker host destroyed: token=%u",
                        activeSessionToken);
                    activeSessionToken = 0;
                    {
                        std::lock_guard<std::mutex> statsLock(s_statsMutex);
                        s_stats.last_inbound_packet_tick_ms = 0;
                        s_stats.last_outbound_packet_tick_ms = 0;
                        ResetLivenessAnchorLocked();
                    }
                    break;
                }

                case WorkerCommandType::ClearQueues: {
                    if (cmd.session_token != 0 && cmd.session_token != activeSessionToken) {
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> eventLock(s_eventMutex);
                        s_events.clear();
                    }
                    {
                        std::lock_guard<std::mutex> statsLock(s_statsMutex);
                        UpdateInboundDepthStatsLocked();
                    }
                    break;
                }
            }
        }

        // Release link-emulated packets whose delay has elapsed. Runs before
        // servicing so a due packet is never held an extra iteration.
        if (LinkEmulator_IsActive()) {
            const uint32_t nowMs = GetTickCount();
            Transport2Event due{};
            while (LinkEmulator_PopDue(&due, sizeof(due), nowMs)) {
                PushTransportEvent(due);
            }
            // Engagement evidence, once a second. An emulator that armed but
            // never saw a packet would otherwise look identical to one that
            // is shaping the link — which is exactly how a "high RTT" run
            // gets reported as passing without ever being high RTT.
            static DWORD s_lastEmuLogMs = 0;
            if (s_lastEmuLogMs == 0 || (DWORD)(nowMs - s_lastEmuLogMs) >= 1000) {
                s_lastEmuLogMs = nowMs;
                LinkEmulatorStats st{};
                LinkEmulator_GetStats(&st);
                Rollback::NetplayLog_Write("LINKEMU", -1,
                    "held=%u peak=%u delayed=%u released=%u dropped=%u overflow=%u",
                    st.queued, st.peak_queued, st.total_delayed,
                    st.total_released, st.total_dropped, st.overflow_forced);
            }
        }

        ENetEvent ev{};
        while (!s_stopRequested.load() && Transport_Service(0, &ev) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    // Busy refusal (C-5/C-6): exactly one peer per session.
                    // A second inbound connect while a peer is live, or any
                    // inbound connect that is not our own pending outbound
                    // while joining, is refused without touching the
                    // existing peer/attempt.
                    const bool surplus =
                        (transportConnected && ev.peer != activePeer) ||
                        (pendingOutboundPeer != nullptr && ev.peer != pendingOutboundPeer);
                    if (surplus) {
                        Rollback::NetplayLog_Write("NTHREAD", -1,
                            "Refusing surplus inbound connect with Busy: peer=0x%llX connected=%d pending_outbound=%d token=%u",
                            (unsigned long long)(uintptr_t)ev.peer,
                            transportConnected ? 1 : 0,
                            pendingOutboundPeer ? 1 : 0,
                            activeSessionToken);
                        Transport_DisconnectPeer(
                            ev.peer,
                            (uint32_t)DisconnectReason::Busy);
                        break;
                    }
                    activePeer = ev.peer;
                    pendingOutboundPeer = nullptr;
                    transportConnected = true;
                    Transport_ConfigurePeerResilience(ev.peer);
                    Transport_AutopunchService(GetTickCount(), true);
                    Transport2Event out{};
                    out.type = Transport2EventType::Connected;
                    out.session_token = activeSessionToken;
                    out.peer_token = (uintptr_t)ev.peer;
                    out.transport_tick_ms = GetTickCount();
                    Rollback::NetplayLog_Write("NTHREAD", -1,
                        "Inbound connect on worker: token=%u peer=0x%llX tick=%lu",
                        activeSessionToken,
                        (unsigned long long)out.peer_token,
                        (unsigned long)out.transport_tick_ms);
                    PushTransportEvent(out);
                    break;
                }

                case ENET_EVENT_TYPE_RECEIVE: {
                    if (!ev.packet) {
                        break;
                    }

                    if (ev.packet->dataLength > MAX_PACKET_SIZE) {
                        std::lock_guard<std::mutex> statsLock(s_statsMutex);
                        s_stats.inbound_drop_count++;
                        enet_packet_destroy(ev.packet);
                        break;
                    }

                    Transport2Event out{};
                    out.type = Transport2EventType::PacketReceived;
                    out.session_token = activeSessionToken;
                    out.peer_token = (uintptr_t)ev.peer;
                    out.transport_tick_ms = GetTickCount();
                    out.channel_id = ev.channelID;
                    out.packet_len = ev.packet->dataLength;
                    if (out.packet_len > 0 && ev.packet->data) {
                        memcpy(out.packet_data, ev.packet->data, out.packet_len);
                    }

                    // Test link emulation (off unless as2_stress.cfg arms it).
                    // Everything inbound goes through here — session, pregame,
                    // frontend lockstep, gameplay, SyncHash, TimeProbe — so a
                    // simulated link is the same link for every subsystem, and
                    // TimeProbe measures it exactly as it measures a real one.
                    if (LinkEmulator_IsActive()) {
                        const bool reliable =
                            (ev.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0;
                        LinkEmulator_Submit(&out, sizeof(out), reliable,
                                            out.transport_tick_ms);
                    } else {
                        PushTransportEvent(out);
                    }

                    enet_packet_destroy(ev.packet);
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT: {
                    if (ev.peer == activePeer) {
                        activePeer = nullptr;
                        transportConnected = false;
                    }
                    if (ev.peer == pendingOutboundPeer) {
                        pendingOutboundPeer = nullptr;
                    }
                    Transport2Event out{};
                    out.type = Transport2EventType::Disconnected;
                    out.session_token = activeSessionToken;
                    out.peer_token = (uintptr_t)ev.peer;
                    out.transport_tick_ms = GetTickCount();
                    out.disconnect_data = ev.data;
                    Rollback::NetplayLog_Write("NTHREAD", -1,
                        "Inbound disconnect on worker: token=%u peer=0x%llX data=%u tick=%lu",
                        activeSessionToken,
                        (unsigned long long)out.peer_token,
                        out.disconnect_data,
                        (unsigned long)out.transport_tick_ms);
                    PushTransportEvent(out);
                    break;
                }

                case ENET_EVENT_TYPE_NONE:
                    break;
            }
        }

        Transport_AutopunchService(GetTickCount(), transportConnected);
        UpdateStats(Transport_IsHostActive(), transportConnected ? activePeer : nullptr);
    }

    if (activePeer) {
        Transport_ForceDisconnectPeer(activePeer);
        activePeer = nullptr;
    }
    transportConnected = false;
    Transport_DestroyHost();

    {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        s_stats.worker_running = false;
        s_stats.host_active = false;
        s_stats.peer_connected = false;
        s_stats.rtt_ms = 0.0f;
        s_stats.rtt_variance_ms = 0.0f;
        s_stats.packets_sent = 0;
        s_stats.packets_lost = 0;
        s_stats.last_service_tick_ms = GetTickCount();
    }
}

} // anonymous namespace

bool Transport2_Init() {
    if (s_initialized.load()) {
        return true;
    }

    {
        std::lock_guard<std::mutex> commandLock(s_commandMutex);
        s_commands.clear();
    }
    {
        std::lock_guard<std::mutex> eventLock(s_eventMutex);
        s_events.clear();
    }
    {
        std::lock_guard<std::mutex> statsLock(s_statsMutex);
        memset(&s_stats, 0, sizeof(s_stats));
        ResetLivenessAnchorLocked();
    }

    s_stopRequested.store(false);
    s_initialized.store(true);

    s_worker = std::thread(WorkerThreadMain);

    Rollback::NetplayLog_Write("NTHREAD", -1, "transport2 worker thread started");
    return true;
}

void Transport2_Shutdown() {
    if (!s_initialized.load()) {
        return;
    }

    s_stopRequested.store(true);
    s_commandCv.notify_all();

    if (s_worker.joinable()) {
        s_worker.join();
    }

    {
        std::lock_guard<std::mutex> commandLock(s_commandMutex);
        s_commands.clear();
    }
    {
        std::lock_guard<std::mutex> eventLock(s_eventMutex);
        s_events.clear();
    }
    {
        std::lock_guard<std::mutex> statsLock(s_statsMutex);
        s_stats.inbound_queue_depth = 0;
        s_stats.outbound_queue_depth = 0;
    }

    s_initialized.store(false);
    Rollback::NetplayLog_Write("NTHREAD", -1, "transport2 worker thread stopped");
}

bool Transport2_StartHost(uint32_t session_token, uint16_t listen_port,
                          bool enable_autopunch,
                          const char* punch_relay_host,
                          uint16_t punch_relay_port) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::StartHost;
    cmd.session_token = session_token;
    cmd.listen_port = listen_port;
    cmd.send_hole_punch = enable_autopunch;
    if (punch_relay_host && punch_relay_host[0]) {
        strncpy_s(cmd.punch_relay_host, sizeof(cmd.punch_relay_host), punch_relay_host, _TRUNCATE);
    }
    cmd.punch_relay_port = punch_relay_port;
    const bool ok = EnqueueCommand(cmd);
    if (ok) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued StartHost: token=%u listen_port=%u autopunch=%d relay=%s:%u",
            session_token,
            listen_port,
            enable_autopunch ? 1 : 0,
            cmd.punch_relay_host,
            (unsigned)cmd.punch_relay_port);
    }
    return ok;
}

bool Transport2_StartJoin(uint32_t session_token, uint16_t listen_port,
                          const char* target_host, uint16_t target_port,
                          bool send_hole_punch,
                          const char* punch_relay_host,
                          uint16_t punch_relay_port) {
    if (!target_host || !target_host[0]) {
        return false;
    }

    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::StartJoin;
    cmd.session_token = session_token;
    cmd.listen_port = listen_port;
    strncpy_s(cmd.target_host, sizeof(cmd.target_host), target_host, _TRUNCATE);
    cmd.target_port = target_port;
    cmd.send_hole_punch = send_hole_punch;
    if (punch_relay_host && punch_relay_host[0]) {
        strncpy_s(cmd.punch_relay_host, sizeof(cmd.punch_relay_host), punch_relay_host, _TRUNCATE);
    }
    cmd.punch_relay_port = punch_relay_port;
    const bool ok = EnqueueCommand(cmd);
    if (ok) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued StartJoin: token=%u local_listen=%u target=%s:%u hole_punch=%d relay=%s:%u",
            session_token,
            listen_port,
            target_host,
            target_port,
            send_hole_punch ? 1 : 0,
            cmd.punch_relay_host,
            (unsigned)cmd.punch_relay_port);
    }
    return ok;
}

bool Transport2_SendPacket(uint32_t session_token, uint8_t channel,
                           PacketType type, const void* payload,
                           size_t payload_len, bool reliable) {
    if (payload_len > MAX_PAYLOAD_SIZE) {
        return false;
    }

    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::SendPacket;
    cmd.session_token = session_token;
    cmd.channel = channel;
    cmd.packet_type = type;
    cmd.reliable = reliable;
    cmd.payload_len = payload_len;
    if (payload_len > 0 && payload) {
        memcpy(cmd.payload, payload, payload_len);
    }

    return EnqueueCommand(cmd);
}

void Transport2_RequestDisconnect(uint32_t session_token, uint32_t data, bool force) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::RequestDisconnect;
    cmd.session_token = session_token;
    cmd.disconnect_data = data;
    cmd.force_disconnect = force;
    if (EnqueueCommand(cmd)) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued disconnect request: token=%u data=%u force=%d",
            session_token,
            data,
            force ? 1 : 0);
    }
}

void Transport2_RequestDestroyHost(uint32_t session_token) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::RequestDestroyHost;
    cmd.session_token = session_token;
    if (EnqueueCommand(cmd)) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued host destroy: token=%u",
            session_token);
    }
}

void Transport2_ClearQueues(uint32_t session_token) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::ClearQueues;
    cmd.session_token = session_token;
    if (EnqueueCommand(cmd)) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued queue clear: token=%u",
            session_token);
    }
}

bool Transport2_TryPopEvent(Transport2Event* out) {
    if (!out) return false;

    std::lock_guard<std::mutex> eventLock(s_eventMutex);
    if (s_events.empty()) {
        return false;
    }

    *out = s_events.front();
    s_events.pop_front();

    std::lock_guard<std::mutex> statsLock(s_statsMutex);
    UpdateInboundDepthStatsLocked();
    return true;
}

void Transport2_GetStats(Transport2Stats* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(s_statsMutex);
    *out = s_stats;
}

} // namespace Net
