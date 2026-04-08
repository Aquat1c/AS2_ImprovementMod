/**
 * Alice Senki 2 - Dedicated Network Service Thread (Implementation)
 */

#include <winsock2.h>     // Must be before windows.h
#include <windows.h>
#include <enet/enet.h>

#include "net/network_thread.h"
#include "net/enet_transport.h"
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
    uint32_t          target_ip;
    uint16_t          target_port;
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
constexpr DWORD  NETWORK_SERVICE_WAIT_MS = 2;

static std::thread              s_worker;
static std::mutex               s_commandMutex;
static std::condition_variable  s_commandCv;
static std::deque<WorkerCommand> s_commands;

static std::mutex               s_eventMutex;
static std::deque<NetworkThreadEvent> s_events;

static std::mutex               s_statsMutex;
static NetworkThreadStats       s_stats{};

static std::atomic<bool>        s_initialized{false};
static std::atomic<bool>        s_stopRequested{false};

static void UpdateInboundDepthStatsLocked() {
    s_stats.inbound_queue_depth = (uint32_t)s_events.size();
}

static void UpdateOutboundDepthStatsLocked() {
    s_stats.outbound_queue_depth = (uint32_t)s_commands.size();
}

static void PushWorkerErrorEvent(uint32_t sessionToken, const char* msg) {
    NetworkThreadEvent ev{};
    ev.type = NetworkThreadEventType::WorkerError;
    ev.session_token = sessionToken;
    ev.transport_tick_ms = GetTickCount();
    strncpy_s(ev.error_text, sizeof(ev.error_text), msg ? msg : "network worker error", _TRUNCATE);

    std::lock_guard<std::mutex> eventLock(s_eventMutex);
    std::lock_guard<std::mutex> statsLock(s_statsMutex);
    if (s_events.size() >= MAX_PENDING_EVENTS) {
        s_stats.inbound_drop_count++;
        return;
    }
    s_events.push_back(ev);
    UpdateInboundDepthStatsLocked();
}

static void PushNetworkEvent(const NetworkThreadEvent& ev) {
    std::lock_guard<std::mutex> eventLock(s_eventMutex);
    std::lock_guard<std::mutex> statsLock(s_statsMutex);
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

static void UpdateStats(bool hostActive, ENetPeer* peer) {
    std::lock_guard<std::mutex> lock(s_statsMutex);
    s_stats.host_active = hostActive;
    s_stats.peer_connected = (peer != nullptr);
    s_stats.last_service_tick_ms = GetTickCount();
    if (peer) {
        s_stats.rtt_ms = Transport_GetPeerRTT(peer);
        s_stats.rtt_variance_ms = (float)peer->roundTripTimeVariance;
        s_stats.packets_sent = peer->packetsSent;
        s_stats.packets_lost = peer->packetsLost;
    } else {
        s_stats.rtt_ms = 0.0f;
        s_stats.rtt_variance_ms = 0.0f;
        s_stats.packets_sent = 0;
        s_stats.packets_lost = 0;
    }
}

static void WorkerThreadMain() {
    ENetPeer* activePeer = nullptr;
    uint32_t activeSessionToken = 0;

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
            s_commandCv.wait_for(lock, std::chrono::milliseconds(NETWORK_SERVICE_WAIT_MS),
                                 [] { return s_stopRequested.load() || !s_commands.empty(); });
        }

        std::deque<WorkerCommand> localCommands;
        CopyCommands(&localCommands);

        for (const WorkerCommand& cmd : localCommands) {
            switch (cmd.type) {
                case WorkerCommandType::StartHost: {
                    activeSessionToken = cmd.session_token;
                    activePeer = nullptr;
                    Transport_DestroyHost();
                    if (!Transport_CreateHost(cmd.listen_port)) {
                        PushWorkerErrorEvent(cmd.session_token, "Network thread failed to create host");
                    } else {
                        Rollback::NetplayLog_Write("NTHREAD", -1,
                            "Worker host created: token=%u listen_port=%u",
                            cmd.session_token,
                            cmd.listen_port);
                    }
                    break;
                }

                case WorkerCommandType::StartJoin: {
                    activeSessionToken = cmd.session_token;
                    activePeer = nullptr;
                    Transport_DestroyHost();
                    // Keep existing behavior: join side binds ephemeral local port.
                    if (!Transport_CreateHost(0)) {
                        PushWorkerErrorEvent(cmd.session_token, "Network thread failed to create join host");
                        break;
                    }
                    ENetPeer* peer = Transport_Connect(cmd.target_ip, cmd.target_port);
                    if (!peer) {
                        PushWorkerErrorEvent(cmd.session_token, "Network thread failed to initiate connect");
                        Transport_DestroyHost();
                        activeSessionToken = 0;
                        break;
                    }
                    activePeer = peer;
                    Rollback::NetplayLog_Write("NTHREAD", -1,
                        "Worker join connect initiated: token=%u target=%u.%u.%u.%u:%u",
                        cmd.session_token,
                        (cmd.target_ip) & 0xFF,
                        (cmd.target_ip >> 8) & 0xFF,
                        (cmd.target_ip >> 16) & 0xFF,
                        (cmd.target_ip >> 24) & 0xFF,
                        cmd.target_port);
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
                        PushWorkerErrorEvent(cmd.session_token, "Network thread failed to send packet");
                    } else {
                        Rollback::NetplayLog_Verbose("NTHREAD", -1,
                            "Outbound packet sent on worker: token=%u ch=%u type=%s payload=%zu reliable=%d",
                            cmd.session_token,
                            cmd.channel,
                            PacketTypeName(cmd.packet_type),
                            cmd.payload_len,
                            cmd.reliable ? 1 : 0);
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
                    Transport_DestroyHost();
                    Rollback::NetplayLog_Write("NTHREAD", -1,
                        "Worker host destroyed: token=%u",
                        activeSessionToken);
                    activeSessionToken = 0;
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

        ENetEvent ev{};
        while (!s_stopRequested.load() && Transport_Service(0, &ev) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    activePeer = ev.peer;
                    NetworkThreadEvent out{};
                    out.type = NetworkThreadEventType::Connected;
                    out.session_token = activeSessionToken;
                    out.peer_token = (uintptr_t)ev.peer;
                    out.transport_tick_ms = GetTickCount();
                    Rollback::NetplayLog_Write("NTHREAD", -1,
                        "Inbound connect on worker: token=%u peer=0x%llX tick=%lu",
                        activeSessionToken,
                        (unsigned long long)out.peer_token,
                        (unsigned long)out.transport_tick_ms);
                    PushNetworkEvent(out);
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

                    NetworkThreadEvent out{};
                    out.type = NetworkThreadEventType::PacketReceived;
                    out.session_token = activeSessionToken;
                    out.peer_token = (uintptr_t)ev.peer;
                    out.transport_tick_ms = GetTickCount();
                    out.channel_id = ev.channelID;
                    out.packet_len = ev.packet->dataLength;
                    if (out.packet_len > 0 && ev.packet->data) {
                        memcpy(out.packet_data, ev.packet->data, out.packet_len);
                    }
                    Rollback::NetplayLog_Verbose("NTHREAD", -1,
                        "Inbound packet received on worker: token=%u peer=0x%llX ch=%u len=%zu tick=%lu",
                        activeSessionToken,
                        (unsigned long long)out.peer_token,
                        out.channel_id,
                        out.packet_len,
                        (unsigned long)out.transport_tick_ms);
                    PushNetworkEvent(out);

                    enet_packet_destroy(ev.packet);
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT: {
                    if (ev.peer == activePeer) {
                        activePeer = nullptr;
                    }
                    NetworkThreadEvent out{};
                    out.type = NetworkThreadEventType::Disconnected;
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
                    PushNetworkEvent(out);
                    break;
                }

                case ENET_EVENT_TYPE_NONE:
                    break;
            }
        }

        UpdateStats(Transport_IsHostActive(), activePeer);
    }

    if (activePeer) {
        Transport_ForceDisconnectPeer(activePeer);
        activePeer = nullptr;
    }
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

bool NetworkThread_Init() {
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
        s_stats.inbound_queue_depth = 0;
        s_stats.outbound_queue_depth = 0;
    }

    s_stopRequested.store(false);
    s_initialized.store(true);

    s_worker = std::thread(WorkerThreadMain);

    Rollback::NetplayLog_Write("NTHREAD", -1, "Network worker thread started");
    return true;
}

void NetworkThread_Shutdown() {
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
    Rollback::NetplayLog_Write("NTHREAD", -1, "Network worker thread stopped");
}

bool NetworkThread_StartHost(uint32_t session_token, uint16_t listen_port) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::StartHost;
    cmd.session_token = session_token;
    cmd.listen_port = listen_port;
    const bool ok = EnqueueCommand(cmd);
    if (ok) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued StartHost: token=%u listen_port=%u",
            session_token,
            listen_port);
    }
    return ok;
}

bool NetworkThread_StartJoin(uint32_t session_token, uint16_t listen_port,
                             uint32_t target_ip, uint16_t target_port) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::StartJoin;
    cmd.session_token = session_token;
    cmd.listen_port = listen_port;
    cmd.target_ip = target_ip;
    cmd.target_port = target_port;
    const bool ok = EnqueueCommand(cmd);
    if (ok) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued StartJoin: token=%u local_listen=%u target=%u.%u.%u.%u:%u",
            session_token,
            listen_port,
            (target_ip) & 0xFF,
            (target_ip >> 8) & 0xFF,
            (target_ip >> 16) & 0xFF,
            (target_ip >> 24) & 0xFF,
            target_port);
    }
    return ok;
}

bool NetworkThread_SendPacket(uint32_t session_token, uint8_t channel,
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

    const bool ok = EnqueueCommand(cmd);
    if (ok) {
        Rollback::NetplayLog_Verbose("NTHREAD", -1,
            "Queued outbound packet: token=%u ch=%u type=%s payload=%zu reliable=%d",
            session_token,
            channel,
            PacketTypeName(type),
            payload_len,
            reliable ? 1 : 0);
    }
    return ok;
}

void NetworkThread_RequestDisconnect(uint32_t session_token, uint32_t data, bool force) {
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

void NetworkThread_RequestDestroyHost(uint32_t session_token) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::RequestDestroyHost;
    cmd.session_token = session_token;
    if (EnqueueCommand(cmd)) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued host destroy: token=%u",
            session_token);
    }
}

void NetworkThread_ClearQueues(uint32_t session_token) {
    WorkerCommand cmd{};
    cmd.type = WorkerCommandType::ClearQueues;
    cmd.session_token = session_token;
    if (EnqueueCommand(cmd)) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queued queue clear: token=%u",
            session_token);
    }
}

bool NetworkThread_TryPopEvent(NetworkThreadEvent* out) {
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

void NetworkThread_GetStats(NetworkThreadStats* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(s_statsMutex);
    *out = s_stats;
}

} // namespace Net
