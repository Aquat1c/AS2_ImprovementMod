/**
 * Alice Senki 2 - NAT Traversal (Implementation)
 */

#include "net/nat_traversal.h"

#include "ui/log_window.h"
#include "rollback/netplay_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(AS2_HAVE_MINIUPNPC)
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>
#endif

#if defined(AS2_HAVE_PCPNATPMP)
#include <pcpnatpmp.h>
#endif

#if defined(AS2_HAVE_LIBJUICE)
#include <juice/juice.h>
#endif

namespace Net {

namespace {

constexpr size_t kMaxQueuedSignals = 256;

constexpr bool kSupportsUpnp =
#if defined(AS2_HAVE_MINIUPNPC)
    true;
#else
    false;
#endif

constexpr bool kSupportsPcp =
#if defined(AS2_HAVE_PCPNATPMP)
    true;
#else
    false;
#endif

constexpr bool kSupportsLibjuice =
#if defined(AS2_HAVE_LIBJUICE)
    true;
#else
    false;
#endif

constexpr bool kSupportsStun = kSupportsLibjuice;
constexpr bool kSupportsHolePunch = true;
constexpr bool kSupportsTurn = kSupportsLibjuice;

static std::mutex              s_mutex;
static std::condition_variable s_cv;
static NatRuntimeConfig        s_config{};
static NatSnapshot             s_snapshot{};
static uint16_t                s_internalPort = 0;
static std::string             s_remoteHintHost;
static uint16_t                s_remoteHintPort = 0;
static std::deque<NatSignalMessage> s_outboundSignals;
static std::deque<NatSignalMessage> s_inboundSignals;
static std::thread             s_worker;
static bool                    s_runtimeInitialized = false;

// Per-worker control block. Workers can sit multiple seconds inside blocking
// discovery calls (upnpDiscover/IGD HTTP, pcp_wait) that cannot be interrupted,
// so stopping must never join from the game thread while one is busy: the
// worker is signaled via `stop`, parked on the retired list, and reaped once
// it flags `done` on its own.
struct WorkerControl {
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
};
static std::shared_ptr<WorkerControl> s_workerCtl;  // guarded by s_mutex

struct RetiredWorkerList {
    // guarded by s_mutex
    std::vector<std::pair<std::thread, std::shared_ptr<WorkerControl>>> entries;
    ~RetiredWorkerList() {
        // Process teardown: detach anything still winding down so a joinable
        // std::thread destructor cannot std::terminate() the exit path.
        for (auto& entry : entries) {
            if (entry.first.joinable()) {
                entry.first.detach();
            }
        }
    }
};
static RetiredWorkerList       s_retiredWorkers;

#if defined(AS2_HAVE_MINIUPNPC)
static UPNPUrls                s_upnpUrls{};
static IGDdatas                s_upnpData{};
static bool                    s_upnpValid = false;
static char                    s_upnpLanAddr[64] = {};
#endif

#if defined(AS2_HAVE_LIBJUICE)
static std::atomic<int>        s_lastJuiceState{JUICE_STATE_DISCONNECTED};
static std::atomic<bool>       s_localGatherDone{false};
static std::atomic<bool>       s_remoteDescriptionSeen{false};
static std::atomic<bool>       s_remoteGatherDone{false};
static std::atomic<bool>       s_juiceLogConfigured{false};
#endif

static void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, cap, "%s", src);
    dst[cap - 1] = '\0';
}

static uint16_t ParsePortToken(const char* token) {
    if (!token || !token[0]) return 0;
    char* end = nullptr;
    long value = std::strtol(token, &end, 10);
    if (!end || *end != '\0' || value <= 0 || value > 65535) {
        return 0;
    }
    return static_cast<uint16_t>(value);
}

static bool ParseCandidateByType(const char* sdp, const char* type, char* outIp, size_t outIpCap, uint16_t* outPort) {
    if (!sdp || !type || !outIp || outIpCap == 0 || !outPort) return false;
    outIp[0] = '\0';
    *outPort = 0;

    std::vector<std::string> tokens;
    tokens.reserve(20);

    const char* cur = sdp;
    while (*cur) {
        while (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n') ++cur;
        if (!*cur) break;
        const char* start = cur;
        while (*cur && *cur != ' ' && *cur != '\t' && *cur != '\r' && *cur != '\n') ++cur;
        tokens.emplace_back(start, (size_t)(cur - start));
    }

    if (tokens.size() < 8) {
        return false;
    }

    size_t typIndex = SIZE_MAX;
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i] == "typ") {
            typIndex = i;
            break;
        }
    }
    if (typIndex == SIZE_MAX || typIndex + 1 >= tokens.size()) {
        return false;
    }
#if defined(_WIN32)
    if (_stricmp(tokens[typIndex + 1].c_str(), type) != 0) {
#else
    if (strcasecmp(tokens[typIndex + 1].c_str(), type) != 0) {
#endif
        return false;
    }
    if (tokens.size() < 6) {
        return false;
    }

    const std::string& ipTok = tokens[4];
    const uint16_t port = ParsePortToken(tokens[5].c_str());
    if (!port) {
        return false;
    }

    CopyText(outIp, outIpCap, ipTok.c_str());
    *outPort = port;
    return true;
}

static void FormatHostPort(char* out, size_t cap, const char* host, uint16_t port) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!host || !host[0] || port == 0) return;
    if (std::strchr(host, ':')) {
        std::snprintf(out, cap, "[%s]:%u", host, (unsigned)port);
    } else {
        std::snprintf(out, cap, "%s:%u", host, (unsigned)port);
    }
    out[cap - 1] = '\0';
}

static bool IsEnvSet(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0];
}

static bool DetectProtonRuntimeHint() {
    return IsEnvSet("STEAM_COMPAT_DATA_PATH") ||
           IsEnvSet("STEAM_COMPAT_CLIENT_INSTALL_PATH") ||
           IsEnvSet("PROTON_LOG") ||
           IsEnvSet("PROTON_DUMP_DEBUG_COMMANDS");
}

static bool DetectWineRuntimeHint() {
#if defined(_WIN32)
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return false;
    return GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return IsEnvSet("WINEPREFIX") || IsEnvSet("WINEDLLPATH");
#endif
}

static void UpdateStatusTextLocked() {
    std::snprintf(s_snapshot.status_text, sizeof(s_snapshot.status_text),
                  "State=%s UPnP=%s PCP=%s STUN=%s",
                  NatTraversalStateName(s_snapshot.traversal_state),
                  NatStatusName(s_snapshot.upnp_status),
                  NatStatusName(s_snapshot.pcp_status),
                  StunStatusName(s_snapshot.stun_status));
    s_snapshot.status_text[sizeof(s_snapshot.status_text) - 1] = '\0';
}

static void ResetRuntimeStateLocked() {
    std::memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.traversal_state = NatTraversalState::Idle;
    s_snapshot.upnp_status = NatStatus::Idle;
    s_snapshot.pcp_status = NatStatus::Idle;
    s_snapshot.stun_status = StunStatus::Idle;
    s_snapshot.upnp_enabled = s_config.enable_upnp && kSupportsUpnp;
    s_snapshot.pcp_enabled = s_config.enable_pcp_fallback && kSupportsPcp;
    s_snapshot.stun_enabled = s_config.enable_stun && kSupportsStun;
    s_snapshot.hole_punch_enabled = s_config.enable_hole_punch && kSupportsHolePunch;
    s_snapshot.turn_enabled = s_config.enable_turn && kSupportsTurn;
    s_snapshot.allow_ipv6_endpoint = s_config.allow_ipv6_endpoint;
    s_snapshot.prefer_portforwarded_direct = s_config.prefer_portforwarded_direct;
    s_snapshot.running_under_wine = DetectWineRuntimeHint();
    s_snapshot.running_under_proton = DetectProtonRuntimeHint();

    CopyText(s_snapshot.stun_server, sizeof(s_snapshot.stun_server), s_config.stun_host);
    if (s_config.turn_host[0]) {
        FormatHostPort(s_snapshot.turn_server, sizeof(s_snapshot.turn_server), s_config.turn_host, s_config.turn_port);
    }
    if (!s_remoteHintHost.empty() && s_remoteHintPort > 0) {
        FormatHostPort(s_snapshot.remote_hint, sizeof(s_snapshot.remote_hint), s_remoteHintHost.c_str(), s_remoteHintPort);
    }
    UpdateStatusTextLocked();
}

static void EnsureRuntimeDefaultsLocked() {
    if (s_runtimeInitialized) return;
    NatRuntimeConfig_SetDefaults(&s_config);
    s_runtimeInitialized = true;
    ResetRuntimeStateLocked();
}

static void QueueOutboundSignalLocked(NatSignalType type, const char* text) {
    if (s_outboundSignals.size() >= kMaxQueuedSignals) {
        s_outboundSignals.pop_front();
        Rollback::NetplayLog_Write("NAT", -1,
            "Outbound NAT signal queue overflow, dropping oldest (type=%s)",
            NatSignalTypeName(type));
    }
    NatSignalMessage msg{};
    msg.type = type;
    CopyText(msg.text, sizeof(msg.text), text);
    s_outboundSignals.push_back(msg);
}

static bool PopInboundSignalLocked(NatSignalMessage* out) {
    if (!out || s_inboundSignals.empty()) return false;
    *out = s_inboundSignals.front();
    s_inboundSignals.pop_front();
    return true;
}

#if defined(AS2_HAVE_MINIUPNPC)
static void CleanupUpnpLocked() {
    if (s_upnpValid) {
        FreeUPNPUrls(&s_upnpUrls);
        std::memset(&s_upnpUrls, 0, sizeof(s_upnpUrls));
        std::memset(&s_upnpData, 0, sizeof(s_upnpData));
        s_upnpValid = false;
    }
    s_upnpLanAddr[0] = '\0';
}

static void RemoveUpnpMappingLocked() {
    if (!s_upnpValid || s_snapshot.mapped_port == 0) {
        return;
    }

    char portStr[16] = {};
    std::snprintf(portStr, sizeof(portStr), "%u", (unsigned)s_snapshot.mapped_port);

    const int rc = UPNP_DeletePortMapping(
        s_upnpUrls.controlURL,
        s_upnpData.first.servicetype,
        portStr,
        "UDP",
        nullptr);

    if (rc == UPNPCOMMAND_SUCCESS) {
        Rollback::NetplayLog_Write("NAT", -1,
            "UPnP mapping removed: port=%u",
            (unsigned)s_snapshot.mapped_port);
    } else {
        Rollback::NetplayLog_Write("NAT", -1,
            "UPnP mapping removal failed: port=%u rc=%d (%s)",
            (unsigned)s_snapshot.mapped_port,
            rc,
            strupnperror(rc));
    }

    s_snapshot.mapped_port = 0;
}

static void TryUpnpMapping(uint16_t port) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.upnp_status = NatStatus::Discovering;
        UpdateStatusTextLocked();
    }

    int error = 0;
    UPNPDev* devList = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
    if (!devList) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.upnp_status = NatStatus::Unavailable;
        UpdateStatusTextLocked();
        Rollback::NetplayLog_Write("NAT", -1,
            "UPnP discover failed: error=%d",
            error);
        return;
    }

    int igd = UPNP_GetValidIGD(
        devList,
        &s_upnpUrls,
        &s_upnpData,
        s_upnpLanAddr,
        sizeof(s_upnpLanAddr),
        nullptr,
        0);

    freeUPNPDevlist(devList);

    if (igd == 0) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.upnp_status = NatStatus::Unavailable;
        UpdateStatusTextLocked();
        Rollback::NetplayLog_Write("NAT", -1, "UPnP IGD not found");
        return;
    }

    s_upnpValid = true;

    char extIp[64] = {};
    const int extRc = UPNP_GetExternalIPAddress(
        s_upnpUrls.controlURL,
        s_upnpData.first.servicetype,
        extIp);

    char portStr[16] = {};
    std::snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);
    const int addRc = UPNP_AddPortMapping(
        s_upnpUrls.controlURL,
        s_upnpData.first.servicetype,
        portStr,
        portStr,
        s_upnpLanAddr,
        "AS2 Improvement Mod",
        "UDP",
        nullptr,
        "0");

    std::lock_guard<std::mutex> lock(s_mutex);
    if (addRc == UPNPCOMMAND_SUCCESS) {
        s_snapshot.upnp_status = NatStatus::Mapped;
        s_snapshot.mapped_port = port;
        if (extRc == UPNPCOMMAND_SUCCESS && extIp[0]) {
            CopyText(s_snapshot.external_ip, sizeof(s_snapshot.external_ip), extIp);
        }
        Rollback::NetplayLog_Write("NAT", -1,
            "UPnP mapping success: lan=%s external=%s port=%u",
            s_upnpLanAddr,
            s_snapshot.external_ip[0] ? s_snapshot.external_ip : "?",
            (unsigned)port);
    } else {
        s_snapshot.upnp_status = NatStatus::Error;
        Rollback::NetplayLog_Write("NAT", -1,
            "UPnP AddPortMapping failed: rc=%d (%s)",
            addRc,
            strupnperror(addRc));
    }
    UpdateStatusTextLocked();
}
#endif

#if defined(AS2_HAVE_PCPNATPMP)
static bool In6ToString(const in6_addr* addr, char* out, size_t outCap) {
    if (!addr || !out || outCap == 0) return false;
    out[0] = '\0';
#if defined(_WIN32)
    return InetNtopA(AF_INET6, (PVOID)addr, out, (DWORD)outCap) != nullptr;
#else
    return inet_ntop(AF_INET6, addr, out, outCap) != nullptr;
#endif
}

static bool TryPcpMapping(const NatRuntimeConfig& cfg, uint16_t internalPort, uint16_t* outExtPort, char* outExtIp, size_t outExtIpCap) {
    if (!outExtPort || !outExtIp || outExtIpCap == 0) return false;
    *outExtPort = 0;
    outExtIp[0] = '\0';

    pcp_ctx_t* ctx = pcp_init(ENABLE_AUTODISCOVERY, nullptr);
    if (!ctx) {
        Rollback::NetplayLog_Write("NAT", -1, "PCP init failed");
        return false;
    }

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = htons(internalPort);
    src.sin_addr.s_addr = htonl(INADDR_ANY);

    pcp_flow_t* flow = pcp_new_flow(
        ctx,
        reinterpret_cast<sockaddr*>(&src),
        nullptr,
        nullptr,
        IPPROTO_UDP,
        1800,
        nullptr);

    if (!flow) {
        Rollback::NetplayLog_Write("NAT", -1, "PCP flow creation failed");
        pcp_terminate(ctx, 0);
        return false;
    }

    const int timeoutMs = (cfg.mapping_timeout_ms > 0) ? (int)cfg.mapping_timeout_ms : 2000;
    const pcp_fstate_e waitState = pcp_wait(flow, timeoutMs, 1);
    bool success = false;

    if (waitState == pcp_state_succeeded || waitState == pcp_state_partial_result) {
        size_t infoCount = 0;
        pcp_flow_info_t* infos = pcp_flow_get_info(flow, &infoCount);
        if (infos) {
            for (size_t i = 0; i < infoCount; i++) {
                const pcp_flow_info_t& fi = infos[i];
                if (fi.ext_port == 0) {
                    continue;
                }
                const uint16_t port = ntohs(fi.ext_port);
                if (port == 0) {
                    continue;
                }
                char ipBuf[64] = {};
                if (!In6ToString(&fi.ext_ip, ipBuf, sizeof(ipBuf))) {
                    continue;
                }
                *outExtPort = port;
                CopyText(outExtIp, outExtIpCap, ipBuf);
                success = true;
                break;
            }
            std::free(infos);
        }
    }

    pcp_delete_flow(flow);
    pcp_terminate(ctx, 0);

    return success;
}
#endif

#if defined(AS2_HAVE_LIBJUICE)
static void JuiceLogHandler(juice_log_level_t level, const char* message) {
    if (!message) return;
    switch (level) {
        case JUICE_LOG_LEVEL_FATAL:
        case JUICE_LOG_LEVEL_ERROR:
            Rollback::NetplayLog_Write("NAT", -1, "libjuice[ERR] %s", message);
            break;
        case JUICE_LOG_LEVEL_WARN:
            Rollback::NetplayLog_Write("NAT", -1, "libjuice[WRN] %s", message);
            break;
        case JUICE_LOG_LEVEL_INFO:
            Rollback::NetplayLog_Write("NAT", -1, "libjuice[INF] %s", message);
            break;
        case JUICE_LOG_LEVEL_DEBUG:
            Rollback::NetplayLog_Write("NAT", -1, "libjuice[DBG] %s", message);
            break;
        case JUICE_LOG_LEVEL_VERBOSE:
            Rollback::NetplayLog_Verbose("NAT", -1, "libjuice[VRB] %s", message);
            break;
        default:
            break;
    }
}

static void ConfigureJuiceLogLevel(uint8_t verbosity) {
    if (!s_juiceLogConfigured.exchange(true)) {
        juice_set_log_handler(JuiceLogHandler);
    }

    juice_log_level_t level = JUICE_LOG_LEVEL_WARN;
    switch (verbosity) {
        case 0: level = JUICE_LOG_LEVEL_ERROR; break;
        case 1: level = JUICE_LOG_LEVEL_INFO; break;
        case 2: level = JUICE_LOG_LEVEL_DEBUG; break;
        default: level = JUICE_LOG_LEVEL_VERBOSE; break;
    }
    juice_set_log_level(level);
}

static void QueueLocalDescription(juice_agent_t* agent) {
    if (!agent) return;
    char desc[JUICE_MAX_SDP_STRING_LEN] = {};
    if (juice_get_local_description(agent, desc, sizeof(desc)) != JUICE_ERR_SUCCESS || !desc[0]) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    CopyText(s_snapshot.local_description, sizeof(s_snapshot.local_description), desc);
    if (std::strlen(desc) >= NAT_SIGNAL_TEXT_MAX) {
        CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason),
                 "Local ICE description exceeded signal payload size");
        Rollback::NetplayLog_Write("NAT", -1,
            "Local ICE description too large for signal payload: len=%zu max=%zu",
            std::strlen(desc),
            NAT_SIGNAL_TEXT_MAX - 1);
    } else {
        QueueOutboundSignalLocked(NatSignalType::LocalDescription, desc);
    }
    UpdateStatusTextLocked();
}

static void JuiceStateChanged(juice_agent_t* agent, juice_state_t state, void*) {
    s_lastJuiceState.store((int)state);

    std::lock_guard<std::mutex> lock(s_mutex);
    switch (state) {
        case JUICE_STATE_GATHERING:
            s_snapshot.traversal_state = NatTraversalState::Gathering;
            Rollback::NetplayLog_Write("NAT", -1, "ICE gathering started");
            break;
        case JUICE_STATE_CONNECTING:
            s_snapshot.traversal_state = NatTraversalState::Connecting;
            Rollback::NetplayLog_Write("NAT", -1,
                "ICE connecting: local_candidates=%d remote_candidates=%d stun=%s",
                s_snapshot.local_candidate_count,
                s_snapshot.remote_candidate_count,
                StunStatusName(s_snapshot.stun_status));
            break;
        case JUICE_STATE_CONNECTED:
        case JUICE_STATE_COMPLETED:
            s_snapshot.traversal_state = NatTraversalState::Connected;
            break;
        case JUICE_STATE_FAILED:
            s_snapshot.traversal_state = NatTraversalState::Failed;
            CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason), "ICE connection failed");
            Rollback::NetplayLog_Write("NAT", -1,
                "ICE failed: local_candidates=%d remote_candidates=%d remote_desc_seen=%d",
                s_snapshot.local_candidate_count,
                s_snapshot.remote_candidate_count,
                s_remoteDescriptionSeen.load() ? 1 : 0);
            break;
        default:
            break;
    }

    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        char localCandidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
        char remoteCandidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
        if (juice_get_selected_candidates(agent,
                                          localCandidate, sizeof(localCandidate),
                                          remoteCandidate, sizeof(remoteCandidate)) == JUICE_ERR_SUCCESS) {
            CopyText(s_snapshot.selected_local_candidate, sizeof(s_snapshot.selected_local_candidate), localCandidate);
            CopyText(s_snapshot.selected_remote_candidate, sizeof(s_snapshot.selected_remote_candidate), remoteCandidate);
        }

        char localAddr[JUICE_MAX_ADDRESS_STRING_LEN] = {};
        char remoteAddr[JUICE_MAX_ADDRESS_STRING_LEN] = {};
        if (juice_get_selected_addresses(agent,
                                         localAddr, sizeof(localAddr),
                                         remoteAddr, sizeof(remoteAddr)) == JUICE_ERR_SUCCESS) {
            CopyText(s_snapshot.selected_local_address, sizeof(s_snapshot.selected_local_address), localAddr);
            CopyText(s_snapshot.selected_remote_address, sizeof(s_snapshot.selected_remote_address), remoteAddr);
        }
        Rollback::NetplayLog_Write("NAT", -1,
            "ICE connected: local_addr=%s remote_addr=%s local_cand=%s remote_cand=%s",
            s_snapshot.selected_local_address[0] ? s_snapshot.selected_local_address : "?",
            s_snapshot.selected_remote_address[0] ? s_snapshot.selected_remote_address : "?",
            s_snapshot.selected_local_candidate[0] ? s_snapshot.selected_local_candidate : "?",
            s_snapshot.selected_remote_candidate[0] ? s_snapshot.selected_remote_candidate : "?");
    }

    UpdateStatusTextLocked();
    s_cv.notify_all();
}

static void JuiceCandidate(juice_agent_t*, const char* sdp, void*) {
    if (!sdp || !sdp[0]) return;

    std::lock_guard<std::mutex> lock(s_mutex);
    s_snapshot.local_candidate_count++;
    if (std::strlen(sdp) < NAT_SIGNAL_TEXT_MAX) {
        QueueOutboundSignalLocked(NatSignalType::Candidate, sdp);
    } else {
        Rollback::NetplayLog_Write("NAT", -1,
            "Skipping oversized ICE candidate signal len=%zu max=%zu",
            std::strlen(sdp),
            NAT_SIGNAL_TEXT_MAX - 1);
    }

    char endpointIp[64] = {};
    uint16_t endpointPort = 0;
    if (ParseCandidateByType(sdp, "srflx", endpointIp, sizeof(endpointIp), &endpointPort)) {
        s_snapshot.stun_status = StunStatus::Available;
        s_snapshot.stun_external_port = endpointPort;
        CopyText(s_snapshot.external_ip, sizeof(s_snapshot.external_ip), endpointIp);
        FormatHostPort(s_snapshot.stun_endpoint, sizeof(s_snapshot.stun_endpoint), endpointIp, endpointPort);
        Rollback::NetplayLog_Write("NAT", -1,
            "ICE srflx candidate #%d: external=%s:%u (your public endpoint)",
            s_snapshot.local_candidate_count, endpointIp, (unsigned)endpointPort);
    } else if (s_snapshot.turn_enabled &&
               ParseCandidateByType(sdp, "relay", endpointIp, sizeof(endpointIp), &endpointPort)) {
        if (!s_snapshot.stun_endpoint[0]) {
            s_snapshot.stun_external_port = endpointPort;
            CopyText(s_snapshot.external_ip, sizeof(s_snapshot.external_ip), endpointIp);
            FormatHostPort(s_snapshot.stun_endpoint, sizeof(s_snapshot.stun_endpoint), endpointIp, endpointPort);
        }
        Rollback::NetplayLog_Write("NAT", -1,
            "ICE relay candidate #%d: %s:%u (TURN relay)",
            s_snapshot.local_candidate_count, endpointIp, (unsigned)endpointPort);
    } else {
        Rollback::NetplayLog_Verbose("NAT", -1,
            "ICE host candidate #%d", s_snapshot.local_candidate_count);
    }

    UpdateStatusTextLocked();
    s_cv.notify_all();
}

static void JuiceGatherDone(juice_agent_t* agent, void*) {
    s_localGatherDone.store(true);

    QueueLocalDescription(agent);

    std::lock_guard<std::mutex> lock(s_mutex);
    QueueOutboundSignalLocked(NatSignalType::GatheringDone, "");
    if (s_snapshot.stun_enabled && s_snapshot.stun_status == StunStatus::Probing) {
        s_snapshot.stun_status = StunStatus::Failed;
    }
    Rollback::NetplayLog_Write("NAT", -1,
        "ICE gather complete: local_candidates=%d stun=%s external=%s",
        s_snapshot.local_candidate_count,
        StunStatusName(s_snapshot.stun_status),
        s_snapshot.stun_endpoint[0] ? s_snapshot.stun_endpoint : "(none)");
    UpdateStatusTextLocked();
    s_cv.notify_all();
}
#endif

static void HandleInboundSignal(
#if defined(AS2_HAVE_LIBJUICE)
    juice_agent_t* agent,
#else
    void*,
#endif
    const NatSignalMessage& msg) {
#if defined(AS2_HAVE_LIBJUICE)
    if (!agent) {
        return;
    }

    int rc = JUICE_ERR_INVALID;
    switch (msg.type) {
        case NatSignalType::LocalDescription:
            if (!msg.text[0]) {
                return;
            }
            rc = juice_set_remote_description(agent, msg.text);
            if (rc == JUICE_ERR_SUCCESS) {
                s_remoteDescriptionSeen.store(true);
                Rollback::NetplayLog_Write("NAT", -1, "Remote ICE description accepted");
            } else {
                Rollback::NetplayLog_Write("NAT", -1,
                    "Remote ICE description rejected rc=%d", rc);
                std::lock_guard<std::mutex> lock(s_mutex);
                s_snapshot.traversal_state = NatTraversalState::Failed;
                CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason),
                         "Remote ICE description rejected");
                UpdateStatusTextLocked();
            }
            break;

        case NatSignalType::Candidate:
            if (!msg.text[0]) {
                return;
            }
            rc = juice_add_remote_candidate(agent, msg.text);
            if (rc == JUICE_ERR_SUCCESS || rc == JUICE_ERR_IGNORED) {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_snapshot.remote_candidate_count++;
                UpdateStatusTextLocked();
            } else {
                Rollback::NetplayLog_Write("NAT", -1,
                    "Remote ICE candidate rejected rc=%d", rc);
            }
            break;

        case NatSignalType::GatheringDone:
            rc = juice_set_remote_gathering_done(agent);
            if (rc == JUICE_ERR_SUCCESS || rc == JUICE_ERR_IGNORED) {
                s_remoteGatherDone.store(true);
                std::lock_guard<std::mutex> lock(s_mutex);
                Rollback::NetplayLog_Write("NAT", -1,
                    "Remote gather done: remote_candidates=%d local_candidates=%d",
                    s_snapshot.remote_candidate_count,
                    s_snapshot.local_candidate_count);
            }
            break;
    }
#else
    (void)msg;
#endif
}

static bool LocalIPv6Available() {
    int fd = (int)::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
    return true;
}

static bool AnyRetiredWorkerBusyLocked() {
    for (const auto& entry : s_retiredWorkers.entries) {
        if (entry.second && !entry.second->done.load()) {
            return true;
        }
    }
    return false;
}

static void WorkerMain(std::shared_ptr<WorkerControl> ctl) {
    NatRuntimeConfig cfg{};
    uint16_t internalPort = 0;
    std::string remoteHintHost;
    bool runtimeWine = false;
    bool runtimeProton = false;

    // Retired predecessors may still be inside blocking discovery calls, and
    // the UPnP globals are written outside s_mutex (UPNP_GetValidIGD), so two
    // workers must never overlap. Wait here on the worker thread instead of
    // joining on the game thread.
    bool waitedForPredecessor = false;
    for (;;) {
        if (ctl->stop.load()) {
            ctl->done.store(true);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!AnyRetiredWorkerBusyLocked()) {
                break;
            }
        }
        waitedForPredecessor = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (waitedForPredecessor) {
            // The predecessor's wind-down may have overwritten the snapshot
            // Nat_StartServices just reset; rebuild it before publishing status.
            ResetRuntimeStateLocked();
        }
        cfg = s_config;
        internalPort = s_internalPort;
        remoteHintHost = s_remoteHintHost;

        s_snapshot.local_ipv6_available = LocalIPv6Available();
        if (!kSupportsLibjuice && (cfg.enable_stun || cfg.enable_turn)) {
            CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason),
                     "libjuice backend not linked");
        }
        if (!kSupportsUpnp && cfg.enable_upnp) {
            Rollback::NetplayLog_Write("NAT", -1, "UPnP requested but miniupnpc backend not linked");
        }
        if (!kSupportsPcp && cfg.enable_pcp_fallback) {
            Rollback::NetplayLog_Write("NAT", -1, "PCP/NAT-PMP requested but libpcpnatpmp backend not linked");
        }

        if (!cfg.enable_upnp && !cfg.enable_stun && !cfg.enable_hole_punch && !cfg.enable_turn) {
            s_snapshot.traversal_state = NatTraversalState::Disabled;
        } else {
            s_snapshot.traversal_state = NatTraversalState::Gathering;
        }
        s_snapshot.upnp_status = cfg.enable_upnp ? NatStatus::Discovering : NatStatus::Idle;
        s_snapshot.pcp_status = cfg.enable_pcp_fallback ? NatStatus::Discovering : NatStatus::Idle;
        s_snapshot.stun_status = (cfg.enable_stun || cfg.enable_turn) ? StunStatus::Probing : StunStatus::Idle;
        UpdateStatusTextLocked();
        runtimeWine = s_snapshot.running_under_wine;
        runtimeProton = s_snapshot.running_under_proton;
    }

    Rollback::NetplayLog_Write("NAT", -1,
        "Traversal worker start: port=%u upnp=%d pcp=%d stun=%d hole=%d turn=%d "
        "stun=%s:%u turn=%s:%u remote_hint=%s runtime[wine=%d proton=%d]",
        (unsigned)internalPort,
        cfg.enable_upnp ? 1 : 0,
        cfg.enable_pcp_fallback ? 1 : 0,
        cfg.enable_stun ? 1 : 0,
        cfg.enable_hole_punch ? 1 : 0,
        cfg.enable_turn ? 1 : 0,
        cfg.stun_host[0] ? cfg.stun_host : "stun.l.google.com",
        (unsigned)(cfg.stun_port ? cfg.stun_port : 19302),
        cfg.turn_host,
        (unsigned)(cfg.turn_port ? cfg.turn_port : 3478),
        remoteHintHost.empty() ? "(none)" : remoteHintHost.c_str(),
        runtimeWine ? 1 : 0,
        runtimeProton ? 1 : 0);

#if defined(AS2_HAVE_MINIUPNPC)
    // Each discovery phase below can block for seconds; re-check the stop flag
    // between phases so an abandoned worker winds down at the next boundary.
    if (cfg.enable_upnp && !ctl->stop.load()) {
        TryUpnpMapping(internalPort);
    } else {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.upnp_status = NatStatus::Idle;
        UpdateStatusTextLocked();
    }
#else
    if (cfg.enable_upnp) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.upnp_status = NatStatus::Unavailable;
        UpdateStatusTextLocked();
    }
#endif

#if defined(AS2_HAVE_PCPNATPMP)
    if (cfg.enable_pcp_fallback && !ctl->stop.load()) {
        bool shouldTryPcp = true;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_snapshot.upnp_status == NatStatus::Mapped) {
                shouldTryPcp = false;
                s_snapshot.pcp_status = NatStatus::Idle;
                UpdateStatusTextLocked();
            } else {
                s_snapshot.traversal_state = NatTraversalState::MappingFallback;
                UpdateStatusTextLocked();
            }
        }

        if (shouldTryPcp) {
            uint16_t extPort = 0;
            char extIp[64] = {};
            const bool ok = TryPcpMapping(cfg, internalPort, &extPort, extIp, sizeof(extIp));
            std::lock_guard<std::mutex> lock(s_mutex);
            if (ok) {
                s_snapshot.pcp_status = NatStatus::Mapped;
                s_snapshot.pcp_mapped_port = extPort;
                if (extIp[0]) {
                    CopyText(s_snapshot.pcp_external_ip, sizeof(s_snapshot.pcp_external_ip), extIp);
                    if (!s_snapshot.external_ip[0]) {
                        CopyText(s_snapshot.external_ip, sizeof(s_snapshot.external_ip), extIp);
                    }
                }
                Rollback::NetplayLog_Write("NAT", -1,
                    "PCP/NAT-PMP mapping success: external=%s port=%u",
                    s_snapshot.pcp_external_ip[0] ? s_snapshot.pcp_external_ip : "?",
                    (unsigned)extPort);
            } else {
                s_snapshot.pcp_status = NatStatus::Unavailable;
                Rollback::NetplayLog_Write("NAT", -1,
                    "PCP/NAT-PMP mapping unavailable");
            }
            UpdateStatusTextLocked();
        }
    } else {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.pcp_status = NatStatus::Idle;
        UpdateStatusTextLocked();
    }
#else
    if (cfg.enable_pcp_fallback) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.pcp_status = NatStatus::Unavailable;
        UpdateStatusTextLocked();
    }
#endif

#if defined(AS2_HAVE_LIBJUICE)
    juice_agent_t* agent = nullptr;
    if ((cfg.enable_stun || cfg.enable_turn) && !ctl->stop.load()) {
        ConfigureJuiceLogLevel(cfg.traversal_log_verbosity);

        juice_config_t jc{};
        jc.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
        jc.stun_server_host = cfg.stun_host[0] ? cfg.stun_host : "stun.l.google.com";
        jc.stun_server_port = cfg.stun_port ? cfg.stun_port : 19302;
        jc.local_port_range_begin = 0;
        jc.local_port_range_end = 0;
        jc.cb_state_changed = JuiceStateChanged;
        jc.cb_candidate = JuiceCandidate;
        jc.cb_gathering_done = JuiceGatherDone;
        jc.user_ptr = nullptr;

        Rollback::NetplayLog_Write("NAT", -1,
            "libjuice diagnostics use an ephemeral socket; ENet socket autopunch owns game-port punching (game_port=%u)",
            (unsigned)internalPort);

        juice_turn_server_t turnServer{};
        if (cfg.enable_turn && cfg.turn_host[0]) {
            turnServer.host = cfg.turn_host;
            turnServer.port = cfg.turn_port ? cfg.turn_port : 3478;
            turnServer.username = cfg.turn_username;
            turnServer.password = cfg.turn_password;
            jc.turn_servers = &turnServer;
            jc.turn_servers_count = 1;
        }

        agent = juice_create(&jc);
        if (!agent) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_snapshot.traversal_state = NatTraversalState::Failed;
            s_snapshot.stun_status = StunStatus::Failed;
            CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason), "libjuice agent create failed");
            UpdateStatusTextLocked();
            Rollback::NetplayLog_Write("NAT", -1, "libjuice create failed");
        } else {
            s_localGatherDone.store(false);
            s_remoteDescriptionSeen.store(false);
            s_remoteGatherDone.store(false);
            s_lastJuiceState.store(JUICE_STATE_DISCONNECTED);

            QueueLocalDescription(agent);

            const int gatherRc = juice_gather_candidates(agent);
            if (gatherRc != JUICE_ERR_SUCCESS) {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_snapshot.traversal_state = NatTraversalState::Failed;
                s_snapshot.stun_status = StunStatus::Failed;
                CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason), "libjuice gather failed");
                UpdateStatusTextLocked();
                Rollback::NetplayLog_Write("NAT", -1, "libjuice gather failed rc=%d", gatherRc);
            } else {
                Rollback::NetplayLog_Write("NAT", -1,
                    "libjuice agent created, gathering started: stun=%s:%u turn=%s gather_timeout=%ums",
                    cfg.stun_host[0] ? cfg.stun_host : "stun.l.google.com",
                    (unsigned)(cfg.stun_port ? cfg.stun_port : 19302),
                    (cfg.enable_turn && cfg.turn_host[0]) ? cfg.turn_host : "disabled",
                    cfg.gather_timeout_ms ? cfg.gather_timeout_ms : 5000);
            }
        }
    }

    const auto gatherStart = std::chrono::steady_clock::now();
    bool connectTimerStarted = false;
    auto connectStart = std::chrono::steady_clock::now();

    while (!ctl->stop.load()) {
        NatSignalMessage msg{};
        while (true) {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (!PopInboundSignalLocked(&msg)) {
                    break;
                }
            }
            HandleInboundSignal(agent, msg);
        }

        if (agent) {
            const juice_state_t state = static_cast<juice_state_t>(s_lastJuiceState.load());
            if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_snapshot.traversal_state = NatTraversalState::Connected;
                UpdateStatusTextLocked();
            }

            const auto now = std::chrono::steady_clock::now();
            if (!s_localGatherDone.load()) {
                const uint32_t gatherTimeout = (cfg.gather_timeout_ms > 0) ? cfg.gather_timeout_ms : 5000;
                const auto gatherElapsedMs = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - gatherStart).count();
                if (gatherElapsedMs > gatherTimeout) {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    if (s_snapshot.stun_enabled && s_snapshot.stun_status == StunStatus::Probing) {
                        s_snapshot.stun_status = StunStatus::Failed;
                    }
                    if (s_snapshot.traversal_state != NatTraversalState::Connected) {
                        s_snapshot.traversal_state = NatTraversalState::TimedOut;
                    }
                    CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason), "ICE gather timed out");
                    UpdateStatusTextLocked();
                    s_localGatherDone.store(true);
                    Rollback::NetplayLog_Write("NAT", -1,
                        "ICE gather timeout after %ums: local_candidates=%d stun=%s external=%s",
                        gatherElapsedMs,
                        s_snapshot.local_candidate_count,
                        StunStatusName(s_snapshot.stun_status),
                        s_snapshot.stun_endpoint[0] ? s_snapshot.stun_endpoint : "(none)");
                }
            }

            if (cfg.enable_hole_punch && s_remoteDescriptionSeen.load()) {
                if (!connectTimerStarted) {
                    connectTimerStarted = true;
                    connectStart = now;
                    const uint32_t connectTimeout = (cfg.connect_timeout_ms > 0) ? cfg.connect_timeout_ms : 8000;
                    int localCands = 0, remoteCands = 0;
                    {
                        std::lock_guard<std::mutex> lock(s_mutex);
                        localCands = s_snapshot.local_candidate_count;
                        remoteCands = s_snapshot.remote_candidate_count;
                    }
                    Rollback::NetplayLog_Write("NAT", -1,
                        "ICE connect timer started: timeout=%ums local_cands=%d remote_cands=%d",
                        connectTimeout, localCands, remoteCands);
                }
                const uint32_t connectTimeout = (cfg.connect_timeout_ms > 0) ? cfg.connect_timeout_ms : 8000;
                const auto connectElapsedMs = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - connectStart).count();
                if (connectElapsedMs > connectTimeout &&
                    state != JUICE_STATE_CONNECTED &&
                    state != JUICE_STATE_COMPLETED) {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    if (s_snapshot.traversal_state != NatTraversalState::Connected) {
                        s_snapshot.traversal_state = NatTraversalState::TimedOut;
                    }
                    CopyText(s_snapshot.failure_reason, sizeof(s_snapshot.failure_reason), "ICE connect timed out");
                    UpdateStatusTextLocked();
                    const char* juiceStateName = "?";
                    switch (state) {
                        case JUICE_STATE_DISCONNECTED: juiceStateName = "disconnected"; break;
                        case JUICE_STATE_GATHERING:    juiceStateName = "gathering"; break;
                        case JUICE_STATE_CONNECTING:   juiceStateName = "connecting"; break;
                        case JUICE_STATE_FAILED:       juiceStateName = "failed"; break;
                        default: break;
                    }
                    Rollback::NetplayLog_Write("NAT", -1,
                        "ICE connect timeout after %ums: ice_state=%s local_cands=%d remote_cands=%d remote_desc=%d",
                        connectElapsedMs, juiceStateName,
                        s_snapshot.local_candidate_count,
                        s_snapshot.remote_candidate_count,
                        s_remoteDescriptionSeen.load() ? 1 : 0);
                }
            }
        }

        std::unique_lock<std::mutex> lock(s_mutex);
        s_cv.wait_for(lock, std::chrono::milliseconds(20), [&ctl] {
            return ctl->stop.load() || !s_inboundSignals.empty();
        });
    }

    if (agent) {
        juice_destroy(agent);
        agent = nullptr;
    }
#else
    while (!ctl->stop.load()) {
        std::unique_lock<std::mutex> lock(s_mutex);
        s_cv.wait_for(lock, std::chrono::milliseconds(50), [&ctl] { return ctl->stop.load(); });
    }
#endif

#if defined(AS2_HAVE_MINIUPNPC)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        RemoveUpnpMappingLocked();
        CleanupUpnpLocked();
    }
#endif

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        // Only settle final state while still the current worker; a parked
        // worker exiting late must not overwrite a snapshot the owner already
        // reset for the next session.
        if (s_workerCtl == ctl &&
            s_snapshot.traversal_state != NatTraversalState::Connected &&
            s_snapshot.traversal_state != NatTraversalState::TimedOut &&
            s_snapshot.traversal_state != NatTraversalState::Failed &&
            s_snapshot.traversal_state != NatTraversalState::Disabled) {
            const bool mapped = (s_snapshot.upnp_status == NatStatus::Mapped) ||
                                (s_snapshot.pcp_status == NatStatus::Mapped);
            const bool stunOk = (s_snapshot.stun_status == StunStatus::Available);
            s_snapshot.traversal_state = (mapped || stunOk) ? NatTraversalState::Connected : NatTraversalState::Idle;
        }
        if (s_workerCtl == ctl) {
            UpdateStatusTextLocked();
        }
    }

    Rollback::NetplayLog_Write("NAT", -1,
        "Traversal worker stop: state=%s upnp=%s pcp=%s stun=%s endpoint=%s",
        NatTraversalStateName(s_snapshot.traversal_state),
        NatStatusName(s_snapshot.upnp_status),
        NatStatusName(s_snapshot.pcp_status),
        StunStatusName(s_snapshot.stun_status),
        s_snapshot.stun_endpoint);

    // Must be the last shared-state touch: once `done` is visible the thread
    // may be joined or a successor may start using the UPnP globals.
    ctl->done.store(true);
}

static void ReapRetiredWorkersLocked() {
    for (size_t i = s_retiredWorkers.entries.size(); i > 0; --i) {
        auto& entry = s_retiredWorkers.entries[i - 1];
        if (entry.second && !entry.second->done.load()) {
            continue;
        }
        if (entry.first.joinable()) {
            entry.first.join();  // worker already flagged done; returns promptly
        }
        s_retiredWorkers.entries.erase(s_retiredWorkers.entries.begin() + (ptrdiff_t)(i - 1));
    }
}

// Runs on the game thread (join start, failure cleanup, session cancel), so it
// must never wait out a worker stuck in a multi-second blocking discovery
// call: after a short grace window the thread is parked on the retired list
// and reaped once it finishes on its own.
static void StopWorkerThread() {
    std::shared_ptr<WorkerControl> ctl;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        ctl = s_workerCtl;
        s_workerCtl.reset();
        ReapRetiredWorkersLocked();
    }

    if (!s_worker.joinable()) {
        return;
    }

    if (ctl) {
        ctl->stop.store(true);
    }
    s_cv.notify_all();

    constexpr std::chrono::milliseconds kStopGrace{50};
    const auto graceStart = std::chrono::steady_clock::now();
    while (ctl && !ctl->done.load() &&
           (std::chrono::steady_clock::now() - graceStart) < kStopGrace) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!ctl || ctl->done.load()) {
        s_worker.join();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_retiredWorkers.entries.emplace_back(std::move(s_worker), ctl);
    }
    Rollback::NetplayLog_Write("NAT", -1,
        "Traversal worker parked for async wind-down (busy in a blocking discovery call)");
}

} // namespace

void Nat_ApplyRuntimeConfig(const NatRuntimeConfig* config) {
    std::lock_guard<std::mutex> lock(s_mutex);
    EnsureRuntimeDefaultsLocked();

    NatRuntimeConfig defaults{};
    NatRuntimeConfig_SetDefaults(&defaults);
    s_config = defaults;

    if (config) {
        s_config = *config;
    }
    ResetRuntimeStateLocked();
}

void Nat_StartServices(uint16_t internalPort) {
    StopWorkerThread();

    std::shared_ptr<WorkerControl> ctl = std::make_shared<WorkerControl>();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        EnsureRuntimeDefaultsLocked();
        s_internalPort = internalPort;
        s_outboundSignals.clear();
        s_inboundSignals.clear();
        ResetRuntimeStateLocked();
        s_workerCtl = ctl;
    }

    // The new worker itself waits for any parked predecessor before touching
    // shared discovery state, so this never blocks the game thread.
    s_worker = std::thread(WorkerMain, std::move(ctl));
}

void Nat_StopServices() {
    StopWorkerThread();
    std::lock_guard<std::mutex> lock(s_mutex);
    s_outboundSignals.clear();
    s_inboundSignals.clear();
    ResetRuntimeStateLocked();
    s_snapshot.traversal_state = NatTraversalState::Idle;
    UpdateStatusTextLocked();
}

void Nat_GetSnapshot(NatSnapshot* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    *out = s_snapshot;
}

void Nat_SetRemoteHint(const char* host, uint16_t port) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_remoteHintHost = (host && host[0]) ? host : "";
    s_remoteHintPort = port;
    if (!s_remoteHintHost.empty() && s_remoteHintPort > 0) {
        FormatHostPort(s_snapshot.remote_hint, sizeof(s_snapshot.remote_hint),
                       s_remoteHintHost.c_str(), s_remoteHintPort);
    } else {
        s_snapshot.remote_hint[0] = '\0';
    }
    UpdateStatusTextLocked();
}

void Nat_ClearRemoteHint() {
    Nat_SetRemoteHint(nullptr, 0);
}

bool Nat_TryPopOutboundSignal(NatSignalMessage* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_outboundSignals.empty()) {
        return false;
    }
    *out = s_outboundSignals.front();
    s_outboundSignals.pop_front();
    return true;
}

void Nat_SubmitRemoteSignal(const NatSignalMessage* msg) {
    if (!msg) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_inboundSignals.size() >= kMaxQueuedSignals) {
        s_inboundSignals.pop_front();
        Rollback::NetplayLog_Write("NAT", -1,
            "Inbound NAT signal queue overflow, dropping oldest (type=%s)",
            NatSignalTypeName(msg->type));
    }
    s_inboundSignals.push_back(*msg);
    s_cv.notify_all();
}

bool Nat_ShouldPreferDirect() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_snapshot.prefer_portforwarded_direct) {
        return false;
    }
    if (s_snapshot.upnp_status == NatStatus::Mapped || s_snapshot.pcp_status == NatStatus::Mapped) {
        return true;
    }
    if (s_snapshot.stun_status == StunStatus::Available) {
        return true;
    }
    if (s_snapshot.traversal_state == NatTraversalState::Connected &&
        s_snapshot.selected_remote_address[0] != '\0') {
        return true;
    }
    return false;
}

bool Nat_HasMappedPort() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return (s_snapshot.upnp_status == NatStatus::Mapped && s_snapshot.mapped_port != 0) ||
           (s_snapshot.pcp_status == NatStatus::Mapped && s_snapshot.pcp_mapped_port != 0);
}

bool Nat_IsStunBackendAvailable() {
    return kSupportsStun;
}

bool Nat_IsHolePunchBackendAvailable() {
    return kSupportsHolePunch;
}

bool Nat_IsTurnBackendAvailable() {
    return kSupportsTurn;
}

bool Nat_IsUpnpBackendAvailable() {
    return kSupportsUpnp;
}

bool Nat_IsPcpBackendAvailable() {
    return kSupportsPcp;
}

void Nat_AddPortMapping(uint16_t internalPort) {
    NatRuntimeConfig cfg{};
    NatRuntimeConfig_SetDefaults(&cfg);
    cfg.enable_upnp = true;
    cfg.enable_stun = false;
    cfg.enable_hole_punch = false;
    cfg.enable_turn = false;
    cfg.enable_pcp_fallback = true;
    Nat_ApplyRuntimeConfig(&cfg);
    Nat_StartServices(internalPort);
}

void Nat_RemovePortMapping() {
    Nat_StopServices();
}

NatStatus Nat_GetStatus() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_snapshot.upnp_status;
}

const char* Nat_GetStatusText() {
    static thread_local char text[192];
    std::lock_guard<std::mutex> lock(s_mutex);
    CopyText(text, sizeof(text), s_snapshot.status_text);
    return text;
}

const char* Nat_GetExternalIP() {
    static thread_local char ip[64];
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_snapshot.external_ip[0]) {
        CopyText(ip, sizeof(ip), s_snapshot.external_ip);
    } else if (s_snapshot.pcp_external_ip[0]) {
        CopyText(ip, sizeof(ip), s_snapshot.pcp_external_ip);
    } else {
        ip[0] = '\0';
    }
    return ip;
}

const char* Nat_GetStunEndpoint() {
    static thread_local char endpoint[96];
    std::lock_guard<std::mutex> lock(s_mutex);
    CopyText(endpoint, sizeof(endpoint), s_snapshot.stun_endpoint);
    return endpoint;
}

} // namespace Net
