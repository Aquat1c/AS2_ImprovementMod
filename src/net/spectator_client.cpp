#include <enet/enet.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ws2tcpip.h>

#include "net/spectator_client.h"

#include "net/enet_transport.h"
#include "net/game_settings_sync.h"
#include "net/netplay_menu_controller.h"
#include "net/netplay_palette_runtime.h"
#include "net/spectator_protocol.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <algorithm>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace {

using namespace Net;

#define SCLIENT_LOG(level, frame, fmt, ...) \
    do { \
        LOG_NETPLAY(level, fmt, ##__VA_ARGS__); \
        Rollback::NetplayLog_WriteSpectator("SCLIENT", frame, fmt, ##__VA_ARGS__); \
    } while (0)

#define SCLIENT_TRACE(frame, fmt, ...) \
    Rollback::NetplayLog_WriteSpectator("SCLIENT", frame, fmt, ##__VA_ARGS__)

struct BufferedFrame {
    Spectator::FrameRecord record;
    bool valid;
};

struct BufferedPaletteState {
    bool     metadata_valid;
    bool     bank_valid;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  flags;
    uint32_t payload_crc;
    uint16_t payload_size;
    uint8_t  data[NETPLAY_PALETTE_BANK_SIZE];
};

struct RelayPeerState {
    ENetPeer* peer;
    bool      handshake_complete;
    bool      needs_full_sync;
    int32_t   next_rb_frame;
    int32_t   last_playback_rb_frame;
    bool      fast_forward_requested;
    bool      hard_sync_requested;
    DWORD     connected_at_ms;
    DWORD     last_status_at_ms;
    char      nickname[64];
};

constexpr int kFastForwardGapFrames = 30;
constexpr int kHardSyncGapFrames = 180;
constexpr int kMaxEventsPerFrame = 64;
constexpr int kMaxRelayEventsPerFrame = 64;
constexpr int kMaxRelaySpectators = 8;
constexpr int kMaxBufferedFramesRetained = 36000;
constexpr DWORD kStatusIntervalMs = 250;
constexpr DWORD kConnectTimeoutMs = 3000;
constexpr DWORD kHandshakeTimeoutMs = 3000;
constexpr DWORD kServerSilenceTimeoutMs = 5000;
constexpr DWORD kRelayHeartbeatIntervalMs = 500;
constexpr DWORD kLanDiscoveryTimeoutMs = 1500;
constexpr int kMaxLanDiscoveryEventsPerFrame = 16;
constexpr int kPortFallbackScanCount = 16;

static bool s_initialized = false;
static SpectatorClientState s_state = SpectatorClientState::Idle;
static ENetHost* s_clientHost = nullptr;
static ENetPeer* s_peer = nullptr;
static char s_endpoint[96] = "";
static char s_redirectEndpoint[96] = "";
static char s_status[128] = "Watch client idle.";
static char s_error[128] = "";
static bool s_matchActive = false;
static uint32_t s_matchId = 0;
static uint32_t s_matchOrdinal = 0;
static uint16_t s_sessionListenPort = 0;
static int32_t s_bufferBaseRbFrame = -1;
static int32_t s_bufferEndRbFrame = -1;
static std::vector<BufferedFrame> s_buffer;
static int32_t s_serverConfirmedRbFrame = -1;
static int32_t s_confirmedContiguousRbFrame = -1;
static int32_t s_serverLiveRbFrame = -1;
static int32_t s_playbackRbFrame = -1;
static LockedMatchConfig s_matchConfig{};
static uint32_t s_streamConfigCrc = 0;
static uint32_t s_streamSessionSeed = 0;
static char s_p1Name[64] = "P1";
static char s_p2Name[64] = "P2";
static bool s_haveMatchState = false;
// Pre-match state: received when selection is committed, before archive starts.
// Lets playback begin charsel bootstrap while players are on loading screen.
static bool s_havePreMatchState = false;
static uint32_t s_preMatchId = 0;
static uint32_t s_preMatchOrdinal = 0;
static LockedMatchConfig s_preMatchConfig{};
static uint32_t s_preMatchConfigCrc = 0;
static uint32_t s_preMatchSessionSeed = 0;
static char s_preMatchP1Name[64] = "P1";
static char s_preMatchP2Name[64] = "P2";
static uint16_t s_p1Wins = 0;
static uint16_t s_p2Wins = 0;
static uint16_t s_draws = 0;
static uint16_t s_completedMatches = 0;
static bool s_fastForwardEnabled = true;
static bool s_hardSyncEnabled = false;
static bool s_shouldFastForward = false;
static bool s_needsHardSync = false;
static uint32_t s_bufferedValidFrameCount = 0;
static bool s_archiveHasGap = false;
static DWORD s_lastStatusSentAt = 0;
static DWORD s_lastServerPacketAt = 0;
static DWORD s_stateEnteredAt = 0;
static bool s_deferredDestroyHost = false;
static char s_deferredDestroyReason[64] = "";
static uint32_t s_paletteEpoch = 0;
static BufferedPaletteState s_palette[2] = {};
static bool s_relayEnabled = false;
static uint16_t s_relayListenPort = 10701;
static uint16_t s_relayBoundListenPort = 0;
static ENetHost* s_relayServer = nullptr;
static bool s_autopunchEnabled = true;
static char s_autopunchRelayHost[96] = "delthas.fr";
static uint16_t s_autopunchRelayPort = 14763;
static std::unordered_map<ENetPeer*, RelayPeerState> s_relayPeers;
static DWORD s_lastRelayHeartbeatAt = 0;
static uint32_t s_lastRelayPaletteEpochSent = 0;
static SOCKET s_discoverySocket = INVALID_SOCKET;
static bool s_discoveryActive = false;
static DWORD s_discoveryStartedAt = 0;
static uint32_t s_discoveryNonce = 0;
static char s_discoveryStatus[128] = "LAN scan idle.";
static SpectatorDiscoveryEntry s_discoveryResults[SPECTATOR_DISCOVERY_MAX_RESULTS] = {};
static uint32_t s_discoveryResultCount = 0;

static void DestroyRelayServer(const char* reason);

static bool HasBufferedCustomPaletteData(const BufferedPaletteState& palette) {
    return palette.metadata_valid &&
           palette.payload_size == NETPLAY_PALETTE_BANK_SIZE;
}

static bool IsBufferedPaletteBankReady(const BufferedPaletteState& palette) {
    return HasBufferedCustomPaletteData(palette) && palette.bank_valid;
}

static const char* ClientStateName(SpectatorClientState state) {
    switch (state) {
        case SpectatorClientState::Idle: return "Idle";
        case SpectatorClientState::Connecting: return "Connecting";
        case SpectatorClientState::Handshaking: return "Handshaking";
        case SpectatorClientState::ConnectedNoActiveMatch: return "ConnectedNoActiveMatch";
        case SpectatorClientState::Streaming: return "Streaming";
        case SpectatorClientState::Redirected: return "Redirected";
        case SpectatorClientState::Failed: return "Failed";
        default: return "Unknown";
    }
}

static uint32_t ComputeConfigCrc(const LockedMatchConfig& config) {
    return LockedMatchConfig_Hash(&config);
}

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
    char previous[sizeof(s_status)] = {};
    strncpy_s(previous, sizeof(previous), s_status, _TRUNCATE);

    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (strcmp(previous, s_status) != 0) {
        SCLIENT_TRACE(
            s_playbackRbFrame,
            "[SCLIENT] status state=%s endpoint=%s match=0x%08X/%u text=%s",
            ClientStateName(s_state),
            s_endpoint[0] ? s_endpoint : "(unset)",
            s_matchId,
            s_matchOrdinal,
            s_status);
    }
}

static void SetError(const char* fmt, ...) {
    char previous[sizeof(s_error)] = {};
    strncpy_s(previous, sizeof(previous), s_error, _TRUNCATE);

    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_error, sizeof(s_error), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (strcmp(previous, s_error) != 0) {
        SCLIENT_TRACE(
            s_playbackRbFrame,
            "[SCLIENT] error state=%s endpoint=%s match=0x%08X/%u text=%s",
            ClientStateName(s_state),
            s_endpoint[0] ? s_endpoint : "(unset)",
            s_matchId,
            s_matchOrdinal,
            s_error[0] ? s_error : "(cleared)");
    }
}

static void SetDiscoveryStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_discoveryStatus, sizeof(s_discoveryStatus), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void SetClientState(SpectatorClientState nextState, const char* why) {
    if (nextState == s_state) {
        return;
    }

    SCLIENT_TRACE(
        s_playbackRbFrame,
        "[SCLIENT] state %s -> %s endpoint=%s match=0x%08X/%u why=%s",
        ClientStateName(s_state),
        ClientStateName(nextState),
        s_endpoint[0] ? s_endpoint : "(unset)",
        s_matchId,
        s_matchOrdinal,
        why && why[0] ? why : "unspecified");
    s_state = nextState;
}

static void FillSpectatorHelloNickname(char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) {
        return;
    }

    NetMenu::MenuSnapshot menu{};
    NetMenu::GetSnapshot(&menu);
    if (menu.local_nickname[0]) {
        CopyText(dst, dstSize, menu.local_nickname);
        return;
    }

    CopyText(dst, dstSize, "Watcher");
}

static void ResetDiscoveryResults() {
    memset(s_discoveryResults, 0, sizeof(s_discoveryResults));
    s_discoveryResultCount = 0;
}

static void CloseDiscoverySocket(const char* reason) {
    if (s_discoverySocket == INVALID_SOCKET) {
        return;
    }
    closesocket(s_discoverySocket);
    s_discoverySocket = INVALID_SOCKET;
    if (reason && reason[0]) {
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] lan_discovery_socket_closed reason=%s",
            reason);
    }
}

static bool EnsureDiscoverySocket() {
    if (s_discoverySocket != INVALID_SOCKET) {
        return true;
    }

    if (!Transport_GlobalInit()) {
        SetDiscoveryStatus("LAN scan transport init failed.");
        return false;
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        SetDiscoveryStatus("Couldn't open the LAN scan socket.");
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] lan_discovery_create_failed err=%d",
            (int)WSAGetLastError());
        return false;
    }

    BOOL reuseAddr = TRUE;
    setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    BOOL allowBroadcast = TRUE;
    if (setsockopt(socketHandle, SOL_SOCKET, SO_BROADCAST,
            reinterpret_cast<const char*>(&allowBroadcast), sizeof(allowBroadcast)) != 0) {
        SetDiscoveryStatus("Couldn't enable LAN scan broadcasting.");
        closesocket(socketHandle);
        return false;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) != 0) {
        SetDiscoveryStatus("Couldn't prepare the LAN scan socket.");
        closesocket(socketHandle);
        return false;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(0);
    if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        SetDiscoveryStatus("Couldn't bind the LAN scan socket.");
        closesocket(socketHandle);
        return false;
    }

    s_discoverySocket = socketHandle;
    return true;
}

static void UpsertDiscoveryResult(const char* endpoint,
                                  const Spectator::DiscoveryResponsePayload* response) {
    if (!endpoint || !endpoint[0] || !response) {
        return;
    }

    uint32_t slotIndex = s_discoveryResultCount;
    for (uint32_t index = 0; index < s_discoveryResultCount; index++) {
        if (_stricmp(s_discoveryResults[index].endpoint, endpoint) == 0) {
            slotIndex = index;
            break;
        }
    }

    if (slotIndex >= SPECTATOR_DISCOVERY_MAX_RESULTS) {
        return;
    }

    if (slotIndex == s_discoveryResultCount) {
        s_discoveryResultCount++;
    }

    SpectatorDiscoveryEntry& entry = s_discoveryResults[slotIndex];
    CopyText(entry.endpoint, sizeof(entry.endpoint), endpoint);
    CopyText(entry.host_nickname, sizeof(entry.host_nickname), response->host_nickname);
    CopyText(entry.p1_name, sizeof(entry.p1_name), response->p1_name);
    CopyText(entry.p2_name, sizeof(entry.p2_name), response->p2_name);
    entry.match_id = response->match_id;
    entry.match_active = response->match_state == Spectator::MATCH_STATE_ACTIVE;
    entry.connected_spectators = response->connected_spectators;
}

static void PollLanDiscoverySocket() {
    if (!s_discoveryActive || s_discoverySocket == INVALID_SOCKET) {
        return;
    }

    for (int attempt = 0; attempt < kMaxLanDiscoveryEventsPerFrame; attempt++) {
        Spectator::DiscoveryResponsePayload response{};
        sockaddr_in fromAddr{};
        int fromAddrLen = sizeof(fromAddr);
        const int recvResult = recvfrom(
            s_discoverySocket,
            reinterpret_cast<char*>(&response),
            sizeof(response),
            0,
            reinterpret_cast<sockaddr*>(&fromAddr),
            &fromAddrLen);
        if (recvResult == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                SetDiscoveryStatus("LAN scan receive failed.");
                SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
                    "[SCLIENT] lan_discovery_recv_failed err=%d",
                    error);
                s_discoveryActive = false;
                CloseDiscoverySocket("recv_failed");
            }
            break;
        }

        if (recvResult < (int)sizeof(response) ||
            response.magic != Spectator::LAN_DISCOVERY_MAGIC ||
            response.version != Spectator::LAN_DISCOVERY_VERSION ||
            response.nonce != s_discoveryNonce) {
            continue;
        }

        char hostAddress[64] = {};
        if (!InetNtopA(AF_INET, &fromAddr.sin_addr, hostAddress, sizeof(hostAddress))) {
            const char* legacyAddress = inet_ntoa(fromAddr.sin_addr);
            if (!legacyAddress || !legacyAddress[0]) {
                continue;
            }
            strncpy_s(hostAddress, sizeof(hostAddress), legacyAddress, _TRUNCATE);
        }

        char endpoint[96] = {};
        _snprintf_s(endpoint, sizeof(endpoint), _TRUNCATE,
            "%s:%u",
            hostAddress,
            response.spectator_port);

        UpsertDiscoveryResult(endpoint, &response);
        SetDiscoveryStatus("Found %u nearby host%s.",
            s_discoveryResultCount,
            s_discoveryResultCount == 1 ? "" : "s");
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] lan_discovery_result endpoint=%s host=%s match_state=%u spectators=%u",
            endpoint,
            response.host_nickname[0] ? response.host_nickname : "?",
            response.match_state,
            response.connected_spectators);
    }
}

static void UpdateLanDiscoveryLifetime() {
    if (!s_discoveryActive) {
        return;
    }

    const DWORD now = GetTickCount();
    if ((now - s_discoveryStartedAt) < kLanDiscoveryTimeoutMs) {
        return;
    }

    s_discoveryActive = false;
    CloseDiscoverySocket("timeout");
    if (s_discoveryResultCount <= 0) {
        SetDiscoveryStatus("No watch hosts found nearby.");
    } else {
        SetDiscoveryStatus("Scan done: found %u host%s.",
            s_discoveryResultCount,
            s_discoveryResultCount == 1 ? "" : "s");
    }
}

static void FailConnection(const char* message) {
    if (message && message[0]) {
        SetError("%s", message);
        SetStatus("%s", message);
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] fail reason=%s endpoint=%s match=0x%08X/%u playback=%d buffered=%u confirmed=%d",
            message,
            s_endpoint[0] ? s_endpoint : "(unset)",
            s_matchId,
            s_matchOrdinal,
            s_playbackRbFrame,
            (unsigned)s_bufferedValidFrameCount,
            s_confirmedContiguousRbFrame);
    }
    if (s_relayServer) {
        DestroyRelayServer(message && message[0] ? message : "upstream failure");
    }
    s_matchActive = false;
    SetClientState(SpectatorClientState::Failed, message && message[0] ? message : "fail_connection");
    s_stateEnteredAt = 0;
}

static void EnterConnectedNoActiveMatch(const char* source, const char* message) {
    const bool stateChanged = s_state != SpectatorClientState::ConnectedNoActiveMatch;
    s_matchActive = false;
    SetClientState(SpectatorClientState::ConnectedNoActiveMatch,
        source && source[0] ? source : "connected_no_active_match");
    s_stateEnteredAt = GetTickCount();
    s_error[0] = '\0';
    SetStatus("%s",
        (message && message[0])
            ? message
            : "Connected to the watch server. Waiting for a live match.");

    if (stateChanged) {
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] connected_waiting_no_match source=%s endpoint=%s",
            source && source[0] ? source : "unknown",
            s_endpoint[0] ? s_endpoint : "(unset)");
    }
}

static void ResetBuffer() {
    s_buffer.clear();
    s_bufferBaseRbFrame = -1;
    s_bufferEndRbFrame = -1;
    s_serverConfirmedRbFrame = -1;
    s_confirmedContiguousRbFrame = -1;
    s_serverLiveRbFrame = -1;
    s_playbackRbFrame = -1;
    s_matchId = 0;
    s_matchOrdinal = 0;
    s_sessionListenPort = 0;
    s_matchActive = false;
    LockedMatchConfig_Clear(&s_matchConfig);
    s_streamConfigCrc = 0;
    s_streamSessionSeed = 0;
    CopyText(s_p1Name, sizeof(s_p1Name), "P1");
    CopyText(s_p2Name, sizeof(s_p2Name), "P2");
    s_haveMatchState = false;
    s_havePreMatchState = false;
    s_preMatchId = 0;
    s_preMatchOrdinal = 0;
    LockedMatchConfig_Clear(&s_preMatchConfig);
    s_preMatchConfigCrc = 0;
    s_preMatchSessionSeed = 0;
    CopyText(s_preMatchP1Name, sizeof(s_preMatchP1Name), "P1");
    CopyText(s_preMatchP2Name, sizeof(s_preMatchP2Name), "P2");
    s_p1Wins = 0;
    s_p2Wins = 0;
    s_draws = 0;
    s_completedMatches = 0;
    s_shouldFastForward = false;
    s_needsHardSync = false;
    s_bufferedValidFrameCount = 0;
    s_archiveHasGap = false;
    s_lastStatusSentAt = 0;
    s_lastServerPacketAt = 0;
    s_paletteEpoch = 0;
    s_lastRelayHeartbeatAt = 0;
    s_lastRelayPaletteEpochSent = 0;
    memset(s_palette, 0, sizeof(s_palette));
}

static void DestroyClientHostNow(const char* reason) {
    const bool hadResources = (s_peer != nullptr) || (s_clientHost != nullptr);
    if (s_peer) {
        enet_peer_disconnect_now(s_peer, 0);
        s_peer = nullptr;
    }
    if (s_clientHost) {
        Transport_AutopunchStopForHost(s_clientHost, reason && reason[0] ? reason : "watch client destroyed");
        enet_host_destroy(s_clientHost);
        s_clientHost = nullptr;
    }
    s_deferredDestroyHost = false;
    s_deferredDestroyReason[0] = '\0';
    if (hadResources) {
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] cleanup_complete reason=%s endpoint=%s",
            reason && reason[0] ? reason : "unspecified",
            s_endpoint[0] ? s_endpoint : "(unset)");
    }
}

static void RequestDeferredDestroy(const char* reason) {
    if (!s_clientHost) {
        return;
    }

    CopyText(s_deferredDestroyReason,
        sizeof(s_deferredDestroyReason),
        reason && reason[0] ? reason : "deferred");
    if (!s_deferredDestroyHost) {
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] defer_teardown reason=%s endpoint=%s",
            s_deferredDestroyReason,
            s_endpoint[0] ? s_endpoint : "(unset)");
    }
    s_deferredDestroyHost = true;
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

static ENetHost* TryCreateRelayServerHost(uint16_t port) {
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    return enet_host_create(&address, kMaxRelaySpectators, Spectator::NUM_CHANNELS, 0, 0);
}

static ENetHost* CreateRelayServerWithFallback(uint16_t requestedPort,
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

    ENetHost* host = TryCreateRelayServerHost(requestedPort);
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

        host = TryCreateRelayServerHost((uint16_t)candidatePort);
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

    host = TryCreateRelayServerHost(0);
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

static bool HasBufferedStreamIdentity() {
    return s_matchId != 0 ||
           s_matchOrdinal != 0 ||
           s_streamConfigCrc != 0 ||
           s_streamSessionSeed != 0 ||
           s_haveMatchState ||
           s_bufferBaseRbFrame >= 0 ||
           s_bufferEndRbFrame >= 0 ||
           s_paletteEpoch != 0;
}

static void AdoptIncomingStreamIdentity(uint32_t matchId,
                                        uint32_t matchOrdinal,
                                        const char* source) {
    if (HasBufferedStreamIdentity() &&
        (s_matchId != matchId || s_matchOrdinal != matchOrdinal)) {
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] stream_identity_change source=%s old_match=0x%08X/%u new_match=0x%08X/%u reset=1",
            source && source[0] ? source : "unknown",
            s_matchId,
            s_matchOrdinal,
            matchId,
            matchOrdinal);
        ResetBuffer();
    }

    s_matchId = matchId;
    s_matchOrdinal = matchOrdinal;
}

static bool AdoptIncomingConfigIdentity(uint32_t configCrc,
                                        uint32_t sessionSeed,
                                        const char* source) {
    if (configCrc == 0 && sessionSeed == 0) {
        return true;
    }

    if ((s_streamConfigCrc != 0 || s_streamSessionSeed != 0) &&
        (s_streamConfigCrc != configCrc || s_streamSessionSeed != sessionSeed)) {
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] stream_identity_mismatch source=%s match=0x%08X/%u old_crc=0x%08X old_seed=0x%08X new_crc=0x%08X new_seed=0x%08X",
            source && source[0] ? source : "unknown",
            s_matchId,
            s_matchOrdinal,
            s_streamConfigCrc,
            s_streamSessionSeed,
            configCrc,
            sessionSeed);
        return false;
    }

    s_streamConfigCrc = configCrc;
    s_streamSessionSeed = sessionSeed;
    return true;
}

static bool ParseEndpointText(const char* text, char* outHost, size_t outHostSize, uint16_t* outPort) {
    if (!text || !text[0] || !outHost || outHostSize == 0 || !outPort) {
        return false;
    }

    outHost[0] = '\0';
    *outPort = 0;

    if (text[0] == '[') {
        const char* close = strchr(text, ']');
        if (!close || close[1] != ':') {
            return false;
        }
        const size_t hostLen = (size_t)(close - (text + 1));
        if (hostLen == 0 || hostLen >= outHostSize) {
            return false;
        }
        memcpy(outHost, text + 1, hostLen);
        outHost[hostLen] = '\0';
        const int port = atoi(close + 2);
        if (port <= 0 || port > 65535) {
            return false;
        }
        *outPort = (uint16_t)port;
        return true;
    }

    const char* colon = strrchr(text, ':');
    if (!colon || colon == text) {
        return false;
    }
    const size_t hostLen = (size_t)(colon - text);
    if (hostLen == 0 || hostLen >= outHostSize) {
        return false;
    }
    memcpy(outHost, text, hostLen);
    outHost[hostLen] = '\0';
    const int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        return false;
    }
    *outPort = (uint16_t)port;
    return true;
}

static bool SendTyped(uint8_t channel,
                      Spectator::PacketType type,
                      const void* payload,
                      size_t payloadLen,
                      bool reliable) {
    if (!s_peer) {
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

    ENetPacket* packet = enet_packet_create(buffer,
        sizeof(type) + payloadLen,
        reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet) {
        return false;
    }
    if (enet_peer_send(s_peer, channel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

static int CountRelayHandshakenPeers() {
    int count = 0;
    for (const auto& entry : s_relayPeers) {
        if (entry.second.handshake_complete) {
            count++;
        }
    }
    return count;
}

static RelayPeerState* FindRelayPeer(ENetPeer* peer) {
    auto it = s_relayPeers.find(peer);
    return (it != s_relayPeers.end()) ? &it->second : nullptr;
}

static bool RelaySendTyped(ENetPeer* peer,
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

    ENetPacket* packet = enet_packet_create(buffer,
        sizeof(type) + payloadLen,
        reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet) {
        return false;
    }
    if (enet_peer_send(peer, channel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

static bool ShouldRunRelayServer() {
    return s_relayEnabled &&
           s_relayListenPort != 0 &&
           (s_state == SpectatorClientState::Connecting ||
            s_state == SpectatorClientState::Handshaking ||
            s_state == SpectatorClientState::ConnectedNoActiveMatch ||
            s_state == SpectatorClientState::Streaming);
}

static void DestroyRelayServer(const char* reason) {
    if (!s_relayServer) {
        s_relayPeers.clear();
        s_relayBoundListenPort = 0;
        s_lastRelayHeartbeatAt = 0;
        s_lastRelayPaletteEpochSent = 0;
        return;
    }

    Spectator::DisconnectPayload disconnect{};
    disconnect.reason_code = 0;
    CopyText(disconnect.message, sizeof(disconnect.message),
        reason && reason[0] ? reason : "relay offline");

    for (size_t index = 0; index < s_relayServer->peerCount; index++) {
        ENetPeer* peer = &s_relayServer->peers[index];
        if (peer->state == ENET_PEER_STATE_CONNECTED ||
            peer->state == ENET_PEER_STATE_CONNECTING) {
            RelaySendTyped(peer,
                Spectator::CHANNEL_CONTROL,
                Spectator::PacketType::Disconnect,
                &disconnect,
                sizeof(disconnect),
                true);
            enet_peer_disconnect_now(peer, 0);
        }
    }

    enet_host_flush(s_relayServer);

    Transport_AutopunchStopForHost(s_relayServer, reason && reason[0] ? reason : "watch relay destroyed");
    enet_host_destroy(s_relayServer);
    s_relayServer = nullptr;
    s_relayPeers.clear();
    s_relayBoundListenPort = 0;
    s_lastRelayHeartbeatAt = 0;
    s_lastRelayPaletteEpochSent = 0;

    SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
        "[SCLIENT] relay_server_destroyed reason=%s",
        reason && reason[0] ? reason : "unspecified");
}

static bool EnsureRelayServer() {
    if (s_relayServer) {
        if (ShouldRunRelayServer()) {
            return true;
        }
        DestroyRelayServer("relay_disabled");
        return false;
    }

    if (!ShouldRunRelayServer()) {
        return false;
    }

    bool usedFallback = false;
    bool usedEphemeral = false;
    s_relayServer = CreateRelayServerWithFallback(
        s_relayListenPort,
        &s_relayBoundListenPort,
        &usedFallback,
        &usedEphemeral);
    if (!s_relayServer) {
        s_relayBoundListenPort = 0;
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] relay_server_bind_failed port=%u",
            s_relayListenPort);
        return false;
    }

    SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
        "[SCLIENT] relay_server_listening requested_port=%u bound_port=%u fallback=%d ephemeral=%d",
        s_relayListenPort,
        s_relayBoundListenPort,
        usedFallback ? 1 : 0,
        usedEphemeral ? 1 : 0);
    if (s_autopunchEnabled) {
        Transport_AutopunchStartForHost(
            s_relayServer,
            "SPECTATE_REBROADCAST",
            s_autopunchRelayHost,
            s_autopunchRelayPort,
            s_relayBoundListenPort,
            nullptr,
            0);
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] relay_autopunch_start relay=%s:%u local_port=%u",
            s_autopunchRelayHost,
            s_autopunchRelayPort,
            s_relayBoundListenPort);
    } else {
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] relay_autopunch_disabled local_port=%u",
            s_relayBoundListenPort);
    }
    return true;
}

static BufferedFrame* EnsureBufferSlot(int32_t rb_frame) {
    if (rb_frame < 0) {
        return nullptr;
    }
    if (s_buffer.empty()) {
        s_bufferBaseRbFrame = rb_frame;
        s_buffer.resize(1);
        s_bufferEndRbFrame = rb_frame;
        return &s_buffer[0];
    }
    if (rb_frame < s_bufferBaseRbFrame) {
        return nullptr;
    }
    const size_t index = (size_t)(rb_frame - s_bufferBaseRbFrame);
    if (index >= s_buffer.size()) {
        s_buffer.resize(index + 1);
    }
    s_bufferEndRbFrame = (std::max)(s_bufferEndRbFrame, rb_frame);
    return &s_buffer[index];
}

static bool IsConfirmedBufferedFrame(const BufferedFrame& frame) {
    return frame.valid &&
        (((frame.record.flags & Spectator::FRAME_FLAG_CONFIRMED) != 0) ||
         (s_serverConfirmedRbFrame >= 0 && frame.record.rb_frame <= s_serverConfirmedRbFrame));
}

static void RecomputeBufferedArchiveStats() {
    const uint32_t previousValidFrameCount = s_bufferedValidFrameCount;
    const bool previousArchiveHasGap = s_archiveHasGap;
    const int32_t previousBufferEnd = s_bufferEndRbFrame;
    const int32_t previousConfirmedEdge = s_confirmedContiguousRbFrame;

    s_bufferedValidFrameCount = 0;
    s_archiveHasGap = false;
    s_bufferEndRbFrame = -1;
    s_confirmedContiguousRbFrame = -1;

    if (s_buffer.empty() || s_bufferBaseRbFrame < 0) {
        return;
    }

    for (size_t index = 0; index < s_buffer.size(); index++) {
        if (!s_buffer[index].valid) {
            continue;
        }

        s_bufferedValidFrameCount++;
        s_bufferEndRbFrame = s_bufferBaseRbFrame + (int32_t)index;
    }

    size_t contiguousCount = 0;
    while (contiguousCount < s_buffer.size() &&
           IsConfirmedBufferedFrame(s_buffer[contiguousCount])) {
        contiguousCount++;
    }

    if (contiguousCount > 0) {
        s_confirmedContiguousRbFrame = s_bufferBaseRbFrame + (int32_t)contiguousCount - 1;
    }

    s_archiveHasGap = s_bufferedValidFrameCount > contiguousCount;

    if (previousValidFrameCount != s_bufferedValidFrameCount ||
        previousArchiveHasGap != s_archiveHasGap ||
        previousBufferEnd != s_bufferEndRbFrame ||
        previousConfirmedEdge != s_confirmedContiguousRbFrame) {
        SCLIENT_TRACE(
            s_playbackRbFrame,
            "[SCLIENT] archive_stats start=%d end=%d confirmed_edge=%d valid=%u gap=%d server_confirmed=%d live=%d",
            s_bufferBaseRbFrame,
            s_bufferEndRbFrame,
            s_confirmedContiguousRbFrame,
            s_bufferedValidFrameCount,
            s_archiveHasGap ? 1 : 0,
            s_serverConfirmedRbFrame,
            s_serverLiveRbFrame);
    }
}

static const BufferedFrame* GetBufferSlot(int32_t rb_frame) {
    if (rb_frame < s_bufferBaseRbFrame) {
        return nullptr;
    }
    const size_t index = (size_t)(rb_frame - s_bufferBaseRbFrame);
    if (index >= s_buffer.size()) {
        return nullptr;
    }
    return &s_buffer[index];
}

static void PruneBufferedFrames() {
    if (s_buffer.empty()) {
        return;
    }

    int32_t keepFromRbFrame = s_bufferBaseRbFrame;
    if (CountRelayHandshakenPeers() > 0) {
        int32_t oldestRequestedRbFrame = -1;
        for (const auto& entry : s_relayPeers) {
            if (!entry.second.handshake_complete) {
                continue;
            }
            if (oldestRequestedRbFrame < 0) {
                oldestRequestedRbFrame = entry.second.next_rb_frame;
            } else {
                oldestRequestedRbFrame = (std::min)(oldestRequestedRbFrame, entry.second.next_rb_frame);
            }
        }
        if (oldestRequestedRbFrame >= 0) {
            keepFromRbFrame = (std::max)(keepFromRbFrame, oldestRequestedRbFrame);
        }
    } else if ((int)s_buffer.size() > kMaxBufferedFramesRetained) {
        const int32_t bufferEndRbFrame = s_bufferBaseRbFrame + (int32_t)s_buffer.size() - 1;
        keepFromRbFrame = bufferEndRbFrame - kMaxBufferedFramesRetained + 1;
    }

    const int32_t bufferEndRbFrame = s_bufferBaseRbFrame + (int32_t)s_buffer.size() - 1;
    keepFromRbFrame = (std::min)(keepFromRbFrame, bufferEndRbFrame);
    if (keepFromRbFrame <= s_bufferBaseRbFrame) {
        return;
    }

    const size_t trimCount = (size_t)(keepFromRbFrame - s_bufferBaseRbFrame);
    const int32_t previousBase = s_bufferBaseRbFrame;
    s_buffer.erase(s_buffer.begin(), s_buffer.begin() + trimCount);
    s_bufferBaseRbFrame = keepFromRbFrame;

    if (s_playbackRbFrame >= 0 && s_playbackRbFrame < s_bufferBaseRbFrame) {
        s_playbackRbFrame = s_bufferBaseRbFrame;
    }
    for (auto& entry : s_relayPeers) {
        if (entry.second.next_rb_frame < s_bufferBaseRbFrame) {
            entry.second.next_rb_frame = s_bufferBaseRbFrame;
        }
    }

    RecomputeBufferedArchiveStats();
    SCLIENT_TRACE(
        s_playbackRbFrame,
        "[SCLIENT] archive_prune old_start=%d new_start=%d trim=%zu relay_peers=%d",
        previousBase,
        s_bufferBaseRbFrame,
        trimCount,
        CountRelayHandshakenPeers());
}

static void BuildBufferedMatchStatePayload(Spectator::MatchStatePayload* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->match_id = s_matchId;
    out->match_ordinal = s_matchOrdinal;
    out->config_crc = s_streamConfigCrc != 0
        ? s_streamConfigCrc
        : (s_haveMatchState ? ComputeConfigCrc(s_matchConfig) : 0);
    out->match_state = s_matchActive
        ? Spectator::MATCH_STATE_ACTIVE
        : (s_matchId != 0 ? Spectator::MATCH_STATE_ENDED : Spectator::MATCH_STATE_IDLE);
    out->archive_start_rb_frame = s_bufferBaseRbFrame;
    out->confirmed_rb_frame = s_serverConfirmedRbFrame;
    out->live_rb_frame = s_serverLiveRbFrame;
    out->p1_wins = s_p1Wins;
    out->p2_wins = s_p2Wins;
    out->draws = s_draws;
    out->completed_matches = s_completedMatches;
    out->session_listen_port = s_sessionListenPort;
    out->config = s_matchConfig;
    CopyText(out->p1_name, sizeof(out->p1_name), s_p1Name);
    CopyText(out->p2_name, sizeof(out->p2_name), s_p2Name);
}

static void BuildBufferedPaletteStatePayload(Spectator::PaletteStatePayload* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->match_id = s_matchId;
    out->match_ordinal = s_matchOrdinal;
    out->config_crc = s_streamConfigCrc != 0
        ? s_streamConfigCrc
        : (s_haveMatchState ? ComputeConfigCrc(s_matchConfig) : 0);
    out->session_seed = s_streamSessionSeed != 0
        ? s_streamSessionSeed
        : (s_haveMatchState ? s_matchConfig.session_seed : 0);
    out->palette_epoch = s_paletteEpoch;

    for (int index = 0; index < 2; index++) {
        if (!s_palette[index].metadata_valid) {
            continue;
        }
        out->player[index].character_id = s_palette[index].character_id;
        out->player[index].base_palette = s_palette[index].base_palette;
        out->player[index].flags = s_palette[index].flags;
        out->player[index].has_custom_data = s_palette[index].bank_valid ? 1 : 0;
        out->player[index].payload_size = s_palette[index].payload_size;
        out->player[index].payload_crc = s_palette[index].payload_crc;
    }
}

static bool SendBufferedPaletteSyncForPeer(ENetPeer* peer) {
    if (!peer) {
        return false;
    }

    Spectator::PaletteStatePayload state{};
    BuildBufferedPaletteStatePayload(&state);
    if (!RelaySendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::PaletteState,
            &state,
            sizeof(state),
            true)) {
        return false;
    }

    for (int index = 0; index < 2; index++) {
        if (!s_palette[index].bank_valid ||
            s_palette[index].payload_size != NETPLAY_PALETTE_BANK_SIZE) {
            continue;
        }

        Spectator::PaletteDataPayload data{};
        data.match_id = s_matchId;
        data.match_ordinal = s_matchOrdinal;
        data.config_crc = state.config_crc;
        data.session_seed = state.session_seed;
        data.palette_epoch = s_paletteEpoch;
        data.game_slot = (uint8_t)index;
        data.character_id = s_palette[index].character_id;
        data.base_palette = s_palette[index].base_palette;
        data.payload_crc = s_palette[index].payload_crc;
        data.payload_size = s_palette[index].payload_size;
        memcpy(data.payload, s_palette[index].data, NETPLAY_PALETTE_BANK_SIZE);

        RelaySendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::PaletteData,
            &data,
            sizeof(data),
            true);
    }

    return true;
}

static void SendClientStatusIfNeeded() {
    if (!s_peer || s_state != SpectatorClientState::Streaming) {
        return;
    }

    const DWORD now = GetTickCount();
    if ((now - s_lastStatusSentAt) < kStatusIntervalMs) {
        return;
    }

    Spectator::ClientStatusPayload payload{};
    payload.match_id = s_matchId;
    payload.match_ordinal = s_matchOrdinal;
    payload.playback_rb_frame = s_playbackRbFrame;
    payload.buffered_frame_count = (s_playbackRbFrame >= 0 && s_bufferEndRbFrame >= s_playbackRbFrame)
        ? (s_bufferEndRbFrame - s_playbackRbFrame)
        : 0;
    if (s_shouldFastForward) {
        payload.flags |= Spectator::CLIENT_STATUS_FLAG_FAST_FORWARD;
    }
    if (s_needsHardSync) {
        payload.flags |= Spectator::CLIENT_STATUS_FLAG_HARD_SYNC;
    }

    if (SendTyped(Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::ClientStatus,
            &payload,
            sizeof(payload),
            true)) {
        s_lastStatusSentAt = now;
        SCLIENT_TRACE(
            payload.playback_rb_frame,
            "[SCLIENT] send_client_status match=0x%08X/%u playback=%d buffered=%u fast_forward=%d hard_sync=%d",
            payload.match_id,
            payload.match_ordinal,
            payload.playback_rb_frame,
            payload.buffered_frame_count,
            (payload.flags & Spectator::CLIENT_STATUS_FLAG_FAST_FORWARD) != 0 ? 1 : 0,
            (payload.flags & Spectator::CLIENT_STATUS_FLAG_HARD_SYNC) != 0 ? 1 : 0);
    }
}

static void HandleRelayHello(ENetPeer* peer, const Spectator::HelloPayload* payload) {
    if (!peer || !payload) {
        return;
    }

    auto it = s_relayPeers.find(peer);
    if (it == s_relayPeers.end()) {
        return;
    }

    if (payload->protocol_version != Spectator::PROTOCOL_VERSION) {
        Spectator::DisconnectPayload disconnect{};
        disconnect.reason_code = 1;
        CopyText(disconnect.message, sizeof(disconnect.message), "watch client version mismatch");
        RelaySendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &disconnect,
            sizeof(disconnect),
            true);
        enet_peer_disconnect_later(peer, 0);
        SCLIENT_TRACE(-1,
            "[SCLIENT] relay_reject peer=0x%p reason=protocol_mismatch",
            peer);
        return;
    }

    if (payload->requested_match_id != 0 && payload->requested_match_id != s_matchId) {
        Spectator::DisconnectPayload disconnect{};
        disconnect.reason_code = 2;
        CopyText(disconnect.message, sizeof(disconnect.message), "No active match is available.");
        RelaySendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &disconnect,
            sizeof(disconnect),
            true);
        enet_peer_disconnect_later(peer, 0);
        SCLIENT_TRACE(-1,
            "[SCLIENT] relay_reject peer=0x%p reason=requested_match_not_active requested=0x%08X active=0x%08X",
            peer,
            payload->requested_match_id,
            s_matchId);
        return;
    }

    if (CountRelayHandshakenPeers() >= kMaxRelaySpectators) {
        Spectator::DisconnectPayload disconnect{};
        disconnect.reason_code = 3;
        CopyText(disconnect.message, sizeof(disconnect.message), "The watch room is full.");
        RelaySendTyped(peer,
            Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &disconnect,
            sizeof(disconnect),
            true);
        enet_peer_disconnect_later(peer, 0);
        SCLIENT_TRACE(-1,
            "[SCLIENT] relay_reject peer=0x%p reason=capacity_reached count=%d",
            peer,
            CountRelayHandshakenPeers());
        return;
    }

    RelayPeerState& state = it->second;
    state.handshake_complete = true;
    state.needs_full_sync = true;
    state.next_rb_frame = (s_bufferBaseRbFrame >= 0) ? s_bufferBaseRbFrame : 0;
    state.last_playback_rb_frame = -1;
    state.fast_forward_requested = false;
    state.hard_sync_requested = false;
    CopyText(state.nickname, sizeof(state.nickname), payload->nickname);

    Spectator::HelloAckPayload ack{};
    ack.protocol_version = Spectator::PROTOCOL_VERSION;
    ack.server_listen_port = s_relayBoundListenPort != 0
        ? s_relayBoundListenPort
        : s_relayListenPort;
    ack.session_listen_port = s_sessionListenPort;
    ack.match_id = s_matchId;
    ack.match_ordinal = s_matchOrdinal;
    ack.match_state = s_matchActive
        ? Spectator::MATCH_STATE_ACTIVE
        : Spectator::MATCH_STATE_IDLE;
    RelaySendTyped(peer,
        Spectator::CHANNEL_CONTROL,
        Spectator::PacketType::HelloAck,
        &ack,
        sizeof(ack),
        true);
    SCLIENT_TRACE(-1,
        "[SCLIENT] relay_admit peer=0x%p nick='%s' match_id=0x%08X ordinal=%u state=%u listen_port=%u",
        peer,
        state.nickname[0] ? state.nickname : "?",
        ack.match_id,
        ack.match_ordinal,
        ack.match_state,
        ack.server_listen_port);
}

static void HandleRelayClientStatus(ENetPeer* peer, const Spectator::ClientStatusPayload* payload) {
    RelayPeerState* state = FindRelayPeer(peer);
    if (!state || !payload) {
        return;
    }

    if (payload->match_id != 0 &&
        (payload->match_id != s_matchId || payload->match_ordinal != s_matchOrdinal)) {
        return;
    }

    state->last_playback_rb_frame = payload->playback_rb_frame;
    state->fast_forward_requested = (payload->flags & Spectator::CLIENT_STATUS_FLAG_FAST_FORWARD) != 0;
    state->hard_sync_requested = (payload->flags & Spectator::CLIENT_STATUS_FLAG_HARD_SYNC) != 0;
    state->last_status_at_ms = GetTickCount();
    SCLIENT_TRACE(
        state->last_playback_rb_frame,
        "[SCLIENT] relay_client_status peer=0x%p nick='%s' playback=%d buffered=%u fast_forward=%d hard_sync=%d",
        peer,
        state->nickname[0] ? state->nickname : "?",
        state->last_playback_rb_frame,
        payload->buffered_frame_count,
        state->fast_forward_requested ? 1 : 0,
        state->hard_sync_requested ? 1 : 0);
}

static void HandleRelayDisconnect(ENetPeer* peer) {
    if (!peer) {
        return;
    }
    SCLIENT_TRACE(-1,
        "[SCLIENT] relay_disconnect peer=0x%p",
        peer);
    s_relayPeers.erase(peer);
    enet_peer_disconnect_now(peer, 0);
}

static void ServiceRelayServer() {
    if (!ShouldRunRelayServer()) {
        if (s_relayServer) {
            DestroyRelayServer("relay_inactive");
        }
        return;
    }

    if (!EnsureRelayServer()) {
        return;
    }

    ENetEvent event{};
    int processedEvents = 0;
    while (processedEvents < kMaxRelayEventsPerFrame &&
           enet_host_service(s_relayServer, &event, 0) > 0) {
        processedEvents++;
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                RelayPeerState state{};
                state.peer = event.peer;
                state.handshake_complete = false;
                state.needs_full_sync = false;
                state.next_rb_frame = 0;
                state.last_playback_rb_frame = -1;
                state.fast_forward_requested = false;
                state.hard_sync_requested = false;
                state.connected_at_ms = GetTickCount();
                state.last_status_at_ms = 0;
                state.nickname[0] = '\0';
                s_relayPeers[event.peer] = state;
                SCLIENT_TRACE(-1,
                    "[SCLIENT] relay_peer_connected peer=0x%p raw_count=%zu",
                    event.peer,
                    s_relayPeers.size());
                break;
            }

            case ENET_EVENT_TYPE_RECEIVE: {
                if (!event.packet) {
                    break;
                }

                if (!Spectator::ValidatePacketSize(event.packet->data, event.packet->dataLength)) {
                    SCLIENT_TRACE(-1,
                        "[SCLIENT] relay_drop_invalid_packet peer=0x%p bytes=%zu",
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
                            HandleRelayHello(event.peer, static_cast<const Spectator::HelloPayload*>(payload));
                        }
                        break;

                    case Spectator::PacketType::ClientStatus:
                        if (payloadLen >= sizeof(Spectator::ClientStatusPayload)) {
                            HandleRelayClientStatus(event.peer, static_cast<const Spectator::ClientStatusPayload*>(payload));
                        } else {
                            SCLIENT_TRACE(-1,
                                "[SCLIENT] relay_short_client_status peer=0x%p bytes=%zu",
                                event.peer,
                                payloadLen);
                        }
                        break;

                    case Spectator::PacketType::Disconnect:
                        HandleRelayDisconnect(event.peer);
                        break;

                    default:
                        SCLIENT_TRACE(-1,
                            "[SCLIENT] relay_ignore_packet peer=0x%p type=%u bytes=%zu",
                            event.peer,
                            (unsigned)type,
                            payloadLen);
                        break;
                }

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                SCLIENT_TRACE(-1,
                    "[SCLIENT] relay_peer_disconnected peer=0x%p",
                    event.peer);
                s_relayPeers.erase(event.peer);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    if (s_autopunchEnabled) {
        Transport_AutopunchServiceForHost(s_relayServer, GetTickCount(), false);
    }

    if (!s_matchActive || !s_haveMatchState) {
        enet_host_flush(s_relayServer);
        return;
    }

    Spectator::MatchStatePayload matchPayload{};
    BuildBufferedMatchStatePayload(&matchPayload);

    for (auto& entry : s_relayPeers) {
        RelayPeerState& peer = entry.second;
        if (!peer.handshake_complete) {
            continue;
        }

        if (peer.needs_full_sync) {
            RelaySendTyped(peer.peer,
                Spectator::CHANNEL_CONTROL,
                Spectator::PacketType::MatchState,
                &matchPayload,
                sizeof(matchPayload),
                true);
            SendBufferedPaletteSyncForPeer(peer.peer);
            peer.next_rb_frame = (s_bufferBaseRbFrame >= 0) ? s_bufferBaseRbFrame : 0;
            peer.needs_full_sync = false;
        }

        const int32_t safeLiveFrame = s_serverConfirmedRbFrame - 2;
        if (safeLiveFrame < peer.next_rb_frame) {
            continue;
        }

        Spectator::FrameBatchPayload batch{};
        batch.match_id = s_matchId;
        batch.match_ordinal = s_matchOrdinal;
        batch.config_crc = s_streamConfigCrc != 0
            ? s_streamConfigCrc
            : ComputeConfigCrc(s_matchConfig);
        batch.session_seed = s_streamSessionSeed != 0
            ? s_streamSessionSeed
            : s_matchConfig.session_seed;
        batch.archive_start_rb_frame = s_bufferBaseRbFrame;
        batch.confirmed_rb_frame = s_serverConfirmedRbFrame;
        batch.live_rb_frame = s_serverLiveRbFrame;

        int32_t nextFrame = peer.next_rb_frame;
        for (; batch.record_count < Spectator::MAX_FRAME_BATCH && nextFrame <= safeLiveFrame; nextFrame++) {
            const BufferedFrame* slot = GetBufferSlot(nextFrame);
            if (!slot || !slot->valid) {
                break;
            }
            batch.records[batch.record_count++] = slot->record;
        }

        if (batch.record_count > 0) {
            size_t payloadSize = sizeof(batch);
            if (batch.record_count < Spectator::MAX_FRAME_BATCH) {
                payloadSize -= sizeof(batch.records) -
                    (sizeof(batch.records[0]) * batch.record_count);
            }

            if (RelaySendTyped(peer.peer,
                    Spectator::CHANNEL_STREAM,
                    Spectator::PacketType::FrameBatch,
                    &batch,
                    payloadSize,
                    true)) {
                peer.next_rb_frame = batch.records[batch.record_count - 1].rb_frame + 1;
            }
        }
    }

    if (s_paletteEpoch != s_lastRelayPaletteEpochSent) {
        s_lastRelayPaletteEpochSent = s_paletteEpoch;
        for (auto& entry : s_relayPeers) {
            if (!entry.second.handshake_complete) {
                continue;
            }
            SendBufferedPaletteSyncForPeer(entry.second.peer);
        }
    }

    const DWORD now = GetTickCount();
    if ((now - s_lastRelayHeartbeatAt) >= kRelayHeartbeatIntervalMs) {
        Spectator::HeartbeatPayload heartbeat{};
        heartbeat.match_id = s_matchId;
        heartbeat.match_ordinal = s_matchOrdinal;
        heartbeat.config_crc = s_streamConfigCrc != 0
            ? s_streamConfigCrc
            : ComputeConfigCrc(s_matchConfig);
        heartbeat.session_seed = s_streamSessionSeed != 0
            ? s_streamSessionSeed
            : s_matchConfig.session_seed;
        heartbeat.confirmed_rb_frame = s_serverConfirmedRbFrame;
        heartbeat.live_rb_frame = s_serverLiveRbFrame;
        heartbeat.session_listen_port = s_sessionListenPort;
        heartbeat.match_state = s_matchActive
            ? Spectator::MATCH_STATE_ACTIVE
            : Spectator::MATCH_STATE_ENDED;
        for (auto& entry : s_relayPeers) {
            if (!entry.second.handshake_complete) {
                continue;
            }
            RelaySendTyped(entry.second.peer,
                Spectator::CHANNEL_CONTROL,
                Spectator::PacketType::Heartbeat,
                &heartbeat,
                sizeof(heartbeat),
                true);
        }
        s_lastRelayHeartbeatAt = now;
    }

    PruneBufferedFrames();
    enet_host_flush(s_relayServer);
}

} // namespace

namespace Net {

void SpectatorClient_Init() {
    if (s_initialized) {
        return;
    }
    s_state = SpectatorClientState::Idle;
    s_endpoint[0] = '\0';
    s_redirectEndpoint[0] = '\0';
    s_status[0] = '\0';
    s_error[0] = '\0';
    s_matchActive = false;
    s_matchId = 0;
    ResetBuffer();
    SetStatus("Watch client idle.");
    SetDiscoveryStatus("LAN scan idle.");
    ResetDiscoveryResults();
    s_stateEnteredAt = 0;
    s_lastServerPacketAt = 0;
    s_deferredDestroyHost = false;
    s_deferredDestroyReason[0] = '\0';
    s_relayEnabled = false;
    s_relayListenPort = 10701;
    s_relayBoundListenPort = 0;
    s_autopunchEnabled = true;
    CopyText(s_autopunchRelayHost, sizeof(s_autopunchRelayHost), "delthas.fr");
    s_autopunchRelayPort = 14763;
    LockedMatchConfig_Clear(&s_matchConfig);
    CopyText(s_p1Name, sizeof(s_p1Name), "P1");
    CopyText(s_p2Name, sizeof(s_p2Name), "P2");
    s_initialized = true;
    SCLIENT_TRACE(-1,
        "[SCLIENT] init relay_enabled=%d relay_port=%u",
        s_relayEnabled ? 1 : 0,
        s_relayListenPort);
}

void SpectatorClient_Shutdown() {
    if (!s_initialized) {
        return;
    }
    SpectatorClient_Disconnect("shutdown");
    DestroyRelayServer("shutdown");
    s_discoveryActive = false;
    CloseDiscoverySocket("shutdown");
    ResetDiscoveryResults();
    s_initialized = false;
    SCLIENT_TRACE(-1, "[SCLIENT] shutdown");
}

void SpectatorClient_SetRelayConfig(bool enabled, uint16_t listenPort) {
    const uint16_t previousPort = s_relayListenPort;
    const bool previousEnabled = s_relayEnabled;
    s_relayEnabled = enabled;
    if (listenPort != 0) {
        s_relayListenPort = listenPort;
    }

    if (previousEnabled != s_relayEnabled || previousPort != s_relayListenPort) {
        SCLIENT_TRACE(-1,
            "[SCLIENT] relay_config enabled=%d->%d port=%u->%u",
            previousEnabled ? 1 : 0,
            s_relayEnabled ? 1 : 0,
            previousPort,
            s_relayListenPort);
    }

    if (!s_initialized) {
        return;
    }

    if (s_relayServer && (!enabled || s_relayListenPort != previousPort)) {
        DestroyRelayServer(enabled ? "relay_rebind" : "relay_disabled");
    }
    if (enabled) {
        EnsureRelayServer();
    }
}

void SpectatorClient_SetAutopunchRelay(bool enabled, const char* relayHost, uint16_t relayPort) {
    const char* nextHost = (relayHost && relayHost[0]) ? relayHost : "delthas.fr";
    const uint16_t nextPort = relayPort != 0 ? relayPort : 14763;
    const bool changed =
        s_autopunchEnabled != enabled ||
        s_autopunchRelayPort != nextPort ||
        _stricmp(s_autopunchRelayHost, nextHost) != 0;

    s_autopunchEnabled = enabled;
    CopyText(s_autopunchRelayHost, sizeof(s_autopunchRelayHost), nextHost);
    s_autopunchRelayPort = nextPort;

    if (!changed) {
        return;
    }

    SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
        "[SCLIENT] autopunch_config enabled=%d relay=%s:%u",
        s_autopunchEnabled ? 1 : 0,
        s_autopunchRelayHost,
        s_autopunchRelayPort);

    if (s_clientHost) {
        Transport_AutopunchStopForHost(s_clientHost, "watch client autopunch reconfigure");
        if (s_autopunchEnabled && s_endpoint[0]) {
            char host[96] = {};
            uint16_t port = 0;
            uint16_t localPort = 0;
            if (ParseEndpointText(s_endpoint, host, sizeof(host), &port) &&
                Transport_GetHostBoundPort(s_clientHost, &localPort) &&
                localPort != 0) {
                Transport_AutopunchStartForHost(
                    s_clientHost,
                    "SPECTATE_CLIENT",
                    s_autopunchRelayHost,
                    s_autopunchRelayPort,
                    localPort,
                    host,
                    port);
            }
        }
    }

    if (s_relayServer) {
        Transport_AutopunchStopForHost(s_relayServer, "watch relay autopunch reconfigure");
        if (s_autopunchEnabled && s_relayBoundListenPort != 0) {
            Transport_AutopunchStartForHost(
                s_relayServer,
                "SPECTATE_REBROADCAST",
                s_autopunchRelayHost,
                s_autopunchRelayPort,
                s_relayBoundListenPort,
                nullptr,
                0);
        }
    }
}

bool SpectatorClient_BeginLanDiscovery() {
    if (!s_initialized) {
        return false;
    }

    if (s_state == SpectatorClientState::Connecting ||
        s_state == SpectatorClientState::Handshaking ||
        s_state == SpectatorClientState::ConnectedNoActiveMatch ||
        s_state == SpectatorClientState::Streaming ||
        s_state == SpectatorClientState::Redirected) {
        SetDiscoveryStatus("Stop watching before scanning.");
        return false;
    }

    s_discoveryActive = false;
    CloseDiscoverySocket("restart");
    ResetDiscoveryResults();

    if (!EnsureDiscoverySocket()) {
        return false;
    }

    Spectator::DiscoveryQueryPayload query{};
    query.magic = Spectator::LAN_DISCOVERY_MAGIC;
    query.version = Spectator::LAN_DISCOVERY_VERSION;
    query.nonce = (GetTickCount() ^ 0x51A2D04Fu);
    if (query.nonce == 0) {
        query.nonce = 1;
    }

    sockaddr_in targetAddr{};
    targetAddr.sin_family = AF_INET;
    targetAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    targetAddr.sin_port = htons(Spectator::LAN_DISCOVERY_PORT);

    const int sendResult = sendto(
        s_discoverySocket,
        reinterpret_cast<const char*>(&query),
        sizeof(query),
        0,
        reinterpret_cast<const sockaddr*>(&targetAddr),
        sizeof(targetAddr));
    if (sendResult != (int)sizeof(query)) {
        SetDiscoveryStatus("Couldn't broadcast the LAN scan.");
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] lan_discovery_send_failed err=%d",
            (int)WSAGetLastError());
        CloseDiscoverySocket("send_failed");
        return false;
    }

    s_discoveryNonce = query.nonce;
    s_discoveryStartedAt = GetTickCount();
    s_discoveryActive = true;
    SetDiscoveryStatus("Scanning local network...");
    SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
        "[SCLIENT] lan_discovery_start nonce=0x%08X port=%u",
        s_discoveryNonce,
        Spectator::LAN_DISCOVERY_PORT);
    return true;
}

bool SpectatorClient_StartConnect(const char* endpoint) {
    if (!s_initialized || !endpoint || !endpoint[0]) {
        return false;
    }

    s_discoveryActive = false;
    CloseDiscoverySocket("spectator_connect");

    char host[96] = {};
    uint16_t port = 0;
    if (!ParseEndpointText(endpoint, host, sizeof(host), &port)) {
        SetError("Invalid watch address.");
        SetStatus("Invalid watch address.");
        SetClientState(SpectatorClientState::Failed, "invalid_endpoint");
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] fail reason=invalid_endpoint endpoint=%s",
            endpoint ? endpoint : "(null)");
        return false;
    }

    DestroyRelayServer("start_connect_reset");
    DestroyClientHostNow("start_connect_reset");
    ResetBuffer();
    s_error[0] = '\0';
    s_redirectEndpoint[0] = '\0';
    s_deferredDestroyHost = false;
    s_deferredDestroyReason[0] = '\0';

    s_clientHost = enet_host_create(nullptr, 1, Spectator::NUM_CHANNELS, 0, 0);
    if (!s_clientHost) {
        SetError("Couldn't create the watch client.");
        SetStatus("Couldn't create the watch client.");
        SetClientState(SpectatorClientState::Failed, "create_host_failed");
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] fail reason=create_host endpoint=%s",
            endpoint);
        return false;
    }

    ENetAddress address{};
    address.port = port;
    if (enet_address_set_host(&address, host) < 0) {
        DestroyClientHostNow("resolve_failed");
        SetError("Couldn't resolve the watch address.");
        SetStatus("Couldn't resolve the watch address.");
        SetClientState(SpectatorClientState::Failed, "resolve_failed");
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] fail reason=resolve_failed endpoint=%s",
            endpoint);
        return false;
    }

    uint16_t localPort = 0;
    Transport_GetHostBoundPort(s_clientHost, &localPort);
    if (s_autopunchEnabled && localPort != 0) {
        Transport_AutopunchStartForHost(
            s_clientHost,
            "SPECTATE_CLIENT",
            s_autopunchRelayHost,
            s_autopunchRelayPort,
            localPort,
            host,
            port);
        Transport_SendHolePunchBurstForHost(
            s_clientHost,
            host,
            port,
            8,
            5);
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] client_autopunch_start local_port=%u target=%s:%u relay=%s:%u",
            localPort,
            host,
            port,
            s_autopunchRelayHost,
            s_autopunchRelayPort);
    } else {
        SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
            "[SCLIENT] client_autopunch_skipped enabled=%d local_port=%u target=%s:%u",
            s_autopunchEnabled ? 1 : 0,
            localPort,
            host,
            port);
    }

    s_peer = enet_host_connect(s_clientHost, &address, Spectator::NUM_CHANNELS, 0);
    if (!s_peer) {
        DestroyClientHostNow("connect_start_failed");
        SetError("Couldn't start the watch connection.");
        SetStatus("Couldn't start the watch connection.");
        SetClientState(SpectatorClientState::Failed, "connect_start_failed");
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] fail reason=connect_start_failed endpoint=%s",
            endpoint);
        return false;
    }

    CopyText(s_endpoint, sizeof(s_endpoint), endpoint);
    SetStatus("Connecting to watch address %s", endpoint);
    SetClientState(SpectatorClientState::Connecting, "start_connect");
    s_stateEnteredAt = GetTickCount();
    s_lastServerPacketAt = 0;
    EnsureRelayServer();
    SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
        "[SCLIENT] start_connect endpoint=%s",
        endpoint);
    return true;
}

void SpectatorClient_ArmForNextMatch(const char* reason) {
    if (!s_haveMatchState && !s_havePreMatchState) return;
    SCLIENT_LOG(LOG_INFO, -1, "[Spectator] Arming for next match (%s): dropping MatchState latch",
                reason ? reason : "?");
    s_haveMatchState = false;
    s_matchActive = false;
    s_matchId = 0;
    LockedMatchConfig_Clear(&s_matchConfig);
    s_streamConfigCrc = 0;
    s_havePreMatchState = false;
    s_preMatchId = 0;
    LockedMatchConfig_Clear(&s_preMatchConfig);
    s_preMatchConfigCrc = 0;
}

void SpectatorClient_Disconnect(const char* reason) {
    if (s_peer) {
        Spectator::DisconnectPayload payload{};
        payload.reason_code = 0;
        CopyText(payload.message, sizeof(payload.message), reason ? reason : "disconnect");
        SendTyped(Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &payload,
            sizeof(payload),
            true);
        if (s_clientHost) {
            enet_host_flush(s_clientHost);
        }
    }

    DestroyRelayServer(reason ? reason : "disconnect");
    DestroyClientHostNow(reason ? reason : "disconnect");
    ResetBuffer();
    s_matchActive = false;
    s_matchId = 0;
    s_error[0] = '\0';
    s_redirectEndpoint[0] = '\0';
    SetClientState(SpectatorClientState::Idle, reason ? reason : "disconnect");
    SetStatus("Watch client idle.");
    s_stateEnteredAt = 0;
    SCLIENT_TRACE(s_playbackRbFrame,
        "[SCLIENT] disconnect_complete reason=%s",
        reason ? reason : "disconnect");
}

void SpectatorClient_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    PollLanDiscoverySocket();
    UpdateLanDiscoveryLifetime();
    ServiceRelayServer();

    if (!s_clientHost) {
        return;
    }

    if (s_autopunchEnabled) {
        const bool upstreamConnected =
            s_state == SpectatorClientState::Handshaking ||
            s_state == SpectatorClientState::ConnectedNoActiveMatch ||
            s_state == SpectatorClientState::Streaming;
        Transport_AutopunchServiceForHost(s_clientHost, GetTickCount(), upstreamConnected);
    }

    bool stopProcessing = false;
    ENetEvent event{};
    int processedEvents = 0;
    while (s_clientHost && !stopProcessing && processedEvents < kMaxEventsPerFrame &&
           enet_host_service(s_clientHost, &event, 0) > 0) {
        processedEvents++;
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (s_autopunchEnabled) {
                    Transport_AutopunchServiceForHost(s_clientHost, GetTickCount(), true);
                }
                Spectator::HelloPayload hello{};
                hello.protocol_version = Spectator::PROTOCOL_VERSION;
                hello.client_listen_port = s_relayServer ? s_relayBoundListenPort : 0;
                hello.flags = Spectator::HELLO_FLAG_ACCEPT_REDIRECT;
                FillSpectatorHelloNickname(hello.nickname, sizeof(hello.nickname));
                SendTyped(Spectator::CHANNEL_CONTROL,
                    Spectator::PacketType::Hello,
                    &hello,
                    sizeof(hello),
                    true);
                SetClientState(SpectatorClientState::Handshaking, "hello_sent");
                s_stateEnteredAt = GetTickCount();
                SetStatus("Connected to the watch server. Starting handshake.");
                SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                    "[SCLIENT] connected endpoint=%s -> handshake_start",
                    s_endpoint[0] ? s_endpoint : "(unset)");
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
                s_lastServerPacketAt = GetTickCount();

                switch (type) {
                    case Spectator::PacketType::HelloAck:
                        if (payloadLen >= sizeof(Spectator::HelloAckPayload)) {
                            const auto* ack = static_cast<const Spectator::HelloAckPayload*>(payload);
                            AdoptIncomingStreamIdentity(ack->match_id, ack->match_ordinal, "hello_ack");
                            s_sessionListenPort = ack->session_listen_port;
                            s_matchActive = ack->match_state == Spectator::MATCH_STATE_ACTIVE;
                            if (!s_matchActive) {
                                SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                    "[SCLIENT] hello_ack endpoint=%s match_id=0x%08X ordinal=%u active_match=0",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_matchId,
                                    s_matchOrdinal);
                                EnterConnectedNoActiveMatch(
                                    "hello_ack",
                                    "Connected to the watch server. Waiting for a live match.");
                            } else {
                                SetStatus("Connected to the watch server for game %u.",
                                    s_matchOrdinal != 0 ? s_matchOrdinal : 1);
                                SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                    "[SCLIENT] hello_ack endpoint=%s match_id=0x%08X ordinal=%u active_match=1",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_matchId,
                                    s_matchOrdinal);
                            }
                        }
                        break;

                    case Spectator::PacketType::Redirect:
                        if (payloadLen >= sizeof(Spectator::RedirectPayload)) {
                            const auto* redirect = static_cast<const Spectator::RedirectPayload*>(payload);
                            CopyText(s_redirectEndpoint, sizeof(s_redirectEndpoint), redirect->endpoint);
                            SetStatus("Switching to watch address %s", s_redirectEndpoint);
                            SetClientState(SpectatorClientState::Redirected, "server_redirect");
                            s_stateEnteredAt = 0;
                            SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                "[SCLIENT] fail reason=redirect endpoint=%s redirect=%s",
                                s_endpoint[0] ? s_endpoint : "(unset)",
                                s_redirectEndpoint[0] ? s_redirectEndpoint : "(unset)");
                            RequestDeferredDestroy("redirect_during_receive");
                            stopProcessing = true;
                        }
                        break;

                    case Spectator::PacketType::MatchState:
                        if (payloadLen >= sizeof(Spectator::MatchStatePayload)) {
                            const auto* match = static_cast<const Spectator::MatchStatePayload*>(payload);
                            const SpectatorClientState previousState = s_state;
                            AdoptIncomingStreamIdentity(match->match_id, match->match_ordinal, "match_state");
                            if (!AdoptIncomingConfigIdentity(
                                    match->config_crc != 0
                                        ? match->config_crc
                                        : ComputeConfigCrc(match->config),
                                    match->config.session_seed,
                                    "match_state")) {
                                FailConnection("The live match changed unexpectedly.");
                                RequestDeferredDestroy("identity_mismatch");
                                stopProcessing = true;
                                break;
                            }
                            s_matchActive = match->match_state == Spectator::MATCH_STATE_ACTIVE;
                            s_matchConfig = match->config;
                            s_p1Wins = match->p1_wins;
                            s_p2Wins = match->p2_wins;
                            s_draws = match->draws;
                            s_completedMatches = match->completed_matches;
                            s_sessionListenPort = match->session_listen_port;
                            CopyText(s_p1Name, sizeof(s_p1Name), match->p1_name);
                            CopyText(s_p2Name, sizeof(s_p2Name), match->p2_name);
                            s_haveMatchState = true;
                            s_serverConfirmedRbFrame = match->confirmed_rb_frame;
                            s_serverLiveRbFrame = match->live_rb_frame;
                            RecomputeBufferedArchiveStats();
                            if (s_matchActive) {
                                SetClientState(SpectatorClientState::Streaming, "match_state_active");
                                s_stateEnteredAt = 0;
                                SetStatus("Watching game %u live. Set score %u-%u.",
                                    s_matchOrdinal != 0 ? s_matchOrdinal : 1,
                                    s_p1Wins,
                                    s_p2Wins);
                                SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                    "[SCLIENT] stream_active endpoint=%s match_id=0x%08X ordinal=%u live=%d confirmed=%d rounds_raw=%u rounds_to_win=%d score=%u-%u draws=%u completed=%u",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_matchId,
                                    s_matchOrdinal,
                                    s_serverLiveRbFrame,
                                    s_serverConfirmedRbFrame,
                                    s_matchConfig.round_count,
                                    GameSettingsSync_RoundsToWin(s_matchConfig.round_count),
                                    s_p1Wins,
                                    s_p2Wins,
                                    s_draws,
                                    s_completedMatches);
                                if (previousState == SpectatorClientState::ConnectedNoActiveMatch) {
                                    SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                        "[SCLIENT] active_match_detected endpoint=%s match_id=0x%08X ordinal=%u",
                                        s_endpoint[0] ? s_endpoint : "(unset)",
                                        s_matchId,
                                        s_matchOrdinal);
                                }
                            } else {
                                SetClientState(SpectatorClientState::Streaming, "match_state_waiting_next_match");
                                s_stateEnteredAt = 0;
                                SetStatus("Connected to the watch server. Waiting for the next match.");
                            }
                        }
                        break;

                    case Spectator::PacketType::PreMatchState:
                        if (payloadLen >= sizeof(Spectator::PreMatchStatePayload)) {
                            const auto* pre = static_cast<const Spectator::PreMatchStatePayload*>(payload);
                            // Validate: only adopt if config CRC is plausible
                            const uint32_t verifiedCrc = pre->config_crc != 0
                                ? pre->config_crc
                                : ComputeConfigCrc(pre->config);
                            if (verifiedCrc != 0 && pre->session_seed != 0) {
                                s_havePreMatchState = true;
                                s_preMatchId = pre->pre_match_id;
                                s_preMatchOrdinal = pre->pre_match_ordinal;
                                s_preMatchConfig = pre->config;
                                s_preMatchConfigCrc = verifiedCrc;
                                s_preMatchSessionSeed = pre->session_seed;
                                CopyText(s_preMatchP1Name, sizeof(s_preMatchP1Name), pre->p1_name);
                                CopyText(s_preMatchP2Name, sizeof(s_preMatchP2Name), pre->p2_name);
                                SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                    "[SCLIENT] pre_match_state endpoint=%s pre_match_id=0x%08X ordinal=%u chars=(%u,%u) stage=%u crc=0x%08X seed=0x%08X",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_preMatchId,
                                    s_preMatchOrdinal,
                                    (unsigned)pre->config.p1_character,
                                    (unsigned)pre->config.p2_character,
                                    (unsigned)pre->config.stage_id,
                                    verifiedCrc,
                                    pre->session_seed);
                            } else {
                                SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
                                    "[SCLIENT] pre_match_state_rejected endpoint=%s reason=%s crc=0x%08X seed=0x%08X",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    verifiedCrc == 0 ? "config_crc_zero" : "session_seed_zero",
                                    verifiedCrc,
                                    pre->session_seed);
                            }
                        }
                        break;

                    case Spectator::PacketType::FrameBatch:
                        if (payloadLen >= offsetof(Spectator::FrameBatchPayload, records)) {
                            const auto* batch = static_cast<const Spectator::FrameBatchPayload*>(payload);
                            const uint16_t recordCount = (std::min)(batch->record_count,
                                (uint16_t)Spectator::MAX_FRAME_BATCH);
                            const SpectatorClientState previousState = s_state;
                            AdoptIncomingStreamIdentity(batch->match_id, batch->match_ordinal, "frame_batch");
                            if (!AdoptIncomingConfigIdentity(batch->config_crc,
                                    batch->session_seed,
                                    "frame_batch")) {
                                FailConnection("The live match changed unexpectedly.");
                                RequestDeferredDestroy("identity_mismatch");
                                stopProcessing = true;
                                break;
                            }
                            s_serverConfirmedRbFrame = batch->confirmed_rb_frame;
                            s_serverLiveRbFrame = batch->live_rb_frame;
                            for (uint16_t index = 0; index < recordCount; index++) {
                                BufferedFrame* slot = EnsureBufferSlot(batch->records[index].rb_frame);
                                if (!slot) {
                                    continue;
                                }
                                slot->record = batch->records[index];
                                slot->valid = true;
                            }
                            RecomputeBufferedArchiveStats();
                            SetClientState(SpectatorClientState::Streaming, "frame_batch");
                            s_stateEnteredAt = 0;
                            s_matchActive = true;
                            if (previousState == SpectatorClientState::ConnectedNoActiveMatch) {
                                SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                    "[SCLIENT] active_archive_detected endpoint=%s match_id=0x%08X ordinal=%u confirmed=%d",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_matchId,
                                    s_matchOrdinal,
                                    s_serverConfirmedRbFrame);
                            }
                        }
                        break;

                    case Spectator::PacketType::PaletteState:
                        if (payloadLen >= sizeof(Spectator::PaletteStatePayload)) {
                            const auto* palette = static_cast<const Spectator::PaletteStatePayload*>(payload);
                            AdoptIncomingStreamIdentity(palette->match_id, palette->match_ordinal, "palette_state");
                            if (!AdoptIncomingConfigIdentity(palette->config_crc,
                                    palette->session_seed,
                                    "palette_state")) {
                                FailConnection("The live match changed unexpectedly.");
                                RequestDeferredDestroy("identity_mismatch");
                                stopProcessing = true;
                                break;
                            }
                            s_paletteEpoch = palette->palette_epoch;
                            for (int index = 0; index < 2; index++) {
                                const bool expectsCustomData =
                                    palette->player[index].has_custom_data != 0 &&
                                    palette->player[index].payload_size == NETPLAY_PALETTE_BANK_SIZE;
                                const bool metadataChanged =
                                    !s_palette[index].metadata_valid ||
                                    s_palette[index].character_id != palette->player[index].character_id ||
                                    s_palette[index].base_palette != palette->player[index].base_palette ||
                                    s_palette[index].payload_crc != palette->player[index].payload_crc ||
                                    s_palette[index].payload_size != palette->player[index].payload_size;
                                s_palette[index].metadata_valid = true;
                                s_palette[index].character_id = palette->player[index].character_id;
                                s_palette[index].base_palette = palette->player[index].base_palette;
                                s_palette[index].flags = palette->player[index].flags;
                                s_palette[index].payload_crc = palette->player[index].payload_crc;
                                s_palette[index].payload_size = palette->player[index].payload_size;
                                if (!expectsCustomData || metadataChanged) {
                                    s_palette[index].bank_valid = false;
                                    memset(s_palette[index].data, 0, sizeof(s_palette[index].data));
                                }
                            }
                        }
                        break;

                    case Spectator::PacketType::PaletteData:
                        if (payloadLen >= sizeof(Spectator::PaletteDataPayload)) {
                            const auto* palette = static_cast<const Spectator::PaletteDataPayload*>(payload);
                            if (palette->game_slot < 2 &&
                                palette->payload_size == NETPLAY_PALETTE_BANK_SIZE) {
                                AdoptIncomingStreamIdentity(palette->match_id, palette->match_ordinal, "palette_data");
                                if (!AdoptIncomingConfigIdentity(palette->config_crc,
                                        palette->session_seed,
                                        "palette_data")) {
                                    FailConnection("The live match changed unexpectedly.");
                                    RequestDeferredDestroy("identity_mismatch");
                                    stopProcessing = true;
                                    break;
                                }
                                s_paletteEpoch = palette->palette_epoch;
                                BufferedPaletteState& slot = s_palette[palette->game_slot];
                                slot.metadata_valid = true;
                                slot.bank_valid = true;
                                slot.character_id = palette->character_id;
                                slot.base_palette = palette->base_palette;
                                slot.payload_crc = palette->payload_crc;
                                slot.payload_size = palette->payload_size;
                                memcpy(slot.data, palette->payload, NETPLAY_PALETTE_BANK_SIZE);
                            }
                        }
                        break;

                    case Spectator::PacketType::Heartbeat:
                        if (payloadLen >= sizeof(Spectator::HeartbeatPayload)) {
                            const auto* heartbeat = static_cast<const Spectator::HeartbeatPayload*>(payload);
                            const bool wasWaitingNoMatch = s_state == SpectatorClientState::ConnectedNoActiveMatch;
                            AdoptIncomingStreamIdentity(heartbeat->match_id, heartbeat->match_ordinal, "heartbeat");
                            if (!AdoptIncomingConfigIdentity(heartbeat->config_crc,
                                    heartbeat->session_seed,
                                    "heartbeat")) {
                                FailConnection("The live match changed unexpectedly.");
                                RequestDeferredDestroy("identity_mismatch");
                                stopProcessing = true;
                                break;
                            }
                            s_serverConfirmedRbFrame = heartbeat->confirmed_rb_frame;
                            s_serverLiveRbFrame = heartbeat->live_rb_frame;
                            s_sessionListenPort = heartbeat->session_listen_port;
                            s_matchActive = heartbeat->match_state == Spectator::MATCH_STATE_ACTIVE;
                            RecomputeBufferedArchiveStats();
                            if (s_matchActive && wasWaitingNoMatch) {
                                SetStatus("A match was found. Waiting for watch details.");
                                SCLIENT_LOG(LOG_INFO, s_playbackRbFrame,
                                    "[SCLIENT] active_match_heartbeat endpoint=%s match_id=0x%08X ordinal=%u",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_matchId,
                                    s_matchOrdinal);
                            }
                            if (!s_matchActive && s_state == SpectatorClientState::Streaming) {
                                SetStatus("Connected to the watch server. Waiting for the next match.");
                            }
                        }
                        break;

                    case Spectator::PacketType::Disconnect:
                        if (payloadLen >= sizeof(Spectator::DisconnectPayload)) {
                            const auto* disconnect = static_cast<const Spectator::DisconnectPayload*>(payload);
                            FailConnection(disconnect->message);
                        } else {
                            FailConnection("The watch server disconnected.");
                        }
                        RequestDeferredDestroy("fail_during_receive");
                        stopProcessing = true;
                        break;

                    case Spectator::PacketType::ClientStatus:
                    case Spectator::PacketType::Hello:
                        break;
                }

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                s_peer = nullptr;
                if (s_state != SpectatorClientState::Redirected) {
                    if (s_state == SpectatorClientState::Connecting ||
                        s_state == SpectatorClientState::Handshaking) {
                        FailConnection("The watch server is unavailable.");
                    } else {
                        FailConnection(s_error[0] ? s_error : "The watch connection closed.");
                    }
                    RequestDeferredDestroy("disconnect_event");
                    stopProcessing = true;
                }
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    if (s_deferredDestroyHost && s_clientHost) {
        DestroyClientHostNow(s_deferredDestroyReason[0] ? s_deferredDestroyReason : "deferred");
    }

    if (s_state == SpectatorClientState::Redirected ||
        s_state == SpectatorClientState::Failed) {
        return;
    }

    if (!s_clientHost) {
        return;
    }

    const DWORD now = GetTickCount();
    if (s_state == SpectatorClientState::Connecting &&
        s_stateEnteredAt != 0 &&
        (now - s_stateEnteredAt) >= kConnectTimeoutMs) {
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] connect_timeout endpoint=%s elapsed_ms=%lu have_pre_match=%d",
            s_endpoint[0] ? s_endpoint : "(unset)",
            (unsigned long)(now - s_stateEnteredAt),
            s_havePreMatchState ? 1 : 0);
        FailConnection("The watch connection timed out.");
        DestroyClientHostNow("connect_timeout");
        return;
    }

    if (s_state == SpectatorClientState::Handshaking &&
        s_stateEnteredAt != 0 &&
        (now - s_stateEnteredAt) >= kHandshakeTimeoutMs) {
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] handshake_timeout endpoint=%s elapsed_ms=%lu",
            s_endpoint[0] ? s_endpoint : "(unset)",
            (unsigned long)(now - s_stateEnteredAt));
        FailConnection("The watch handshake timed out.");
        DestroyClientHostNow("handshake_timeout");
        return;
    }

    if (s_state == SpectatorClientState::Streaming &&
        s_lastServerPacketAt != 0 &&
        (now - s_lastServerPacketAt) >= kServerSilenceTimeoutMs) {
        SCLIENT_LOG(LOG_WARNING, s_playbackRbFrame,
            "[SCLIENT] stream_timeout endpoint=%s last_packet_ms=%lu match=0x%08X/%u buffered=%u playback=%d confirmed=%d",
            s_endpoint[0] ? s_endpoint : "(unset)",
            (unsigned long)(now - s_lastServerPacketAt),
            s_matchId,
            s_matchOrdinal,
            (unsigned)s_bufferedValidFrameCount,
            s_playbackRbFrame,
            s_confirmedContiguousRbFrame);
        FailConnection("The live match timed out.");
        DestroyClientHostNow("stream_timeout");
        return;
    }

    SendClientStatusIfNeeded();
    enet_host_flush(s_clientHost);
}

void SpectatorClient_SetFastForwardEnabled(bool enabled) {
    if (s_shouldFastForward != enabled) {
        SCLIENT_TRACE(s_playbackRbFrame,
            "[SCLIENT] fast_forward_enabled %d -> %d",
            s_shouldFastForward ? 1 : 0,
            enabled ? 1 : 0);
    }
    s_fastForwardEnabled = enabled;
    s_shouldFastForward = enabled;
}

void SpectatorClient_SetHardSyncEnabled(bool enabled) {
    if (s_needsHardSync != enabled) {
        SCLIENT_TRACE(s_playbackRbFrame,
            "[SCLIENT] hard_sync_enabled %d -> %d",
            s_needsHardSync ? 1 : 0,
            enabled ? 1 : 0);
    }
    s_hardSyncEnabled = enabled;
    s_needsHardSync = enabled;
}

void SpectatorClient_SetPlaybackFrame(int32_t rbFrame) {
    s_playbackRbFrame = rbFrame;
}

SpectatorClientState SpectatorClient_GetState() {
    return s_state;
}

void SpectatorClient_GetSnapshot(SpectatorClientSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->active = s_state != SpectatorClientState::Idle;
    out->state = s_state;
    CopyText(out->endpoint, sizeof(out->endpoint), s_endpoint);
    CopyText(out->redirect_endpoint, sizeof(out->redirect_endpoint), s_redirectEndpoint);
    out->session_listen_port = s_sessionListenPort;
    out->match_id = s_matchId;
    out->match_ordinal = s_matchOrdinal;
    out->have_match_state = s_haveMatchState;
    out->match_active = s_matchActive;
    out->have_pre_match_state = s_havePreMatchState;
    out->pre_match_id = s_preMatchId;
    out->pre_match_ordinal = s_preMatchOrdinal;
    out->pre_match_config = s_preMatchConfig;
    out->pre_match_config_crc = s_preMatchConfigCrc;
    out->pre_match_session_seed = s_preMatchSessionSeed;
    CopyText(out->pre_match_p1_name, sizeof(out->pre_match_p1_name), s_preMatchP1Name);
    CopyText(out->pre_match_p2_name, sizeof(out->pre_match_p2_name), s_preMatchP2Name);
    out->config_crc = s_streamConfigCrc != 0
        ? s_streamConfigCrc
        : (s_haveMatchState ? ComputeConfigCrc(s_matchConfig) : 0);
    out->session_seed = s_streamSessionSeed != 0
        ? s_streamSessionSeed
        : (s_haveMatchState ? s_matchConfig.session_seed : 0);
    out->config = s_matchConfig;
    CopyText(out->p1_name, sizeof(out->p1_name), s_p1Name);
    CopyText(out->p2_name, sizeof(out->p2_name), s_p2Name);
    out->p1_wins = s_p1Wins;
    out->p2_wins = s_p2Wins;
    out->draws = s_draws;
    out->completed_matches = s_completedMatches;
    out->buffered_start_rb_frame = s_bufferBaseRbFrame;
    out->buffered_end_rb_frame = s_bufferEndRbFrame;
    out->server_confirmed_rb_frame = s_serverConfirmedRbFrame;
    out->confirmed_contiguous_rb_frame = s_confirmedContiguousRbFrame;
    out->server_live_rb_frame = s_serverLiveRbFrame;
    out->playback_rb_frame = s_playbackRbFrame;
    out->buffered_frame_count = s_bufferedValidFrameCount;
    out->archive_has_gap = s_archiveHasGap;
    out->should_fast_forward = s_shouldFastForward;
    out->needs_hard_sync = s_needsHardSync;
    out->relay_server_active = s_relayServer != nullptr;
    out->relay_listen_port = s_relayBoundListenPort != 0
        ? s_relayBoundListenPort
        : s_relayListenPort;
    out->relay_connected_spectators = (uint32_t)CountRelayHandshakenPeers();
    CopyText(out->status, sizeof(out->status), s_status);
    CopyText(out->error, sizeof(out->error), s_error);
}

void SpectatorClient_GetDiscoverySnapshot(SpectatorDiscoverySnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->active = s_discoveryActive;
    out->result_count = s_discoveryResultCount;
    CopyText(out->status, sizeof(out->status), s_discoveryStatus);
    for (uint32_t index = 0; index < s_discoveryResultCount && index < SPECTATOR_DISCOVERY_MAX_RESULTS; index++) {
        out->results[index] = s_discoveryResults[index];
    }
}

bool SpectatorClient_GetFrameInputs(int32_t rb_frame, uint16_t* outP1, uint16_t* outP2) {
    if (rb_frame < s_bufferBaseRbFrame ||
        (s_confirmedContiguousRbFrame >= 0 && rb_frame > s_confirmedContiguousRbFrame)) {
        return false;
    }

    const BufferedFrame* slot = GetBufferSlot(rb_frame);
    if (!slot || !slot->valid) {
        return false;
    }

    if (outP1) {
        *outP1 = slot->record.p1_input;
    }
    if (outP2) {
        *outP2 = slot->record.p2_input;
    }
    return true;
}

bool SpectatorClient_GetFrameHash(int32_t rb_frame, uint32_t* outHash24, bool* outHasHash) {
    if (outHash24) {
        *outHash24 = 0;
    }
    if (outHasHash) {
        *outHasHash = false;
    }
    if (rb_frame < s_bufferBaseRbFrame) {
        return false;
    }

    const BufferedFrame* slot = GetBufferSlot(rb_frame);
    if (!slot || !slot->valid) {
        return false;
    }

    if ((slot->record.flags & Spectator::FRAME_FLAG_HAS_HASH) != 0) {
        if (outHasHash) {
            *outHasHash = true;
        }
        if (outHash24) {
            *outHash24 = (uint32_t)slot->record.hash24[0] |
                         ((uint32_t)slot->record.hash24[1] << 8) |
                         ((uint32_t)slot->record.hash24[2] << 16);
        }
    }
    return true;
}

bool SpectatorClient_GetBufferedPaletteSlot(uint8_t gameSlot, SpectatorBufferedPaletteSlot* out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (gameSlot > 1) {
        return false;
    }

    const BufferedPaletteState& slot = s_palette[gameSlot];
    out->metadata_valid = slot.metadata_valid;
    out->has_custom_data = HasBufferedCustomPaletteData(slot);
    out->bank_valid = IsBufferedPaletteBankReady(slot);
    out->character_id = slot.character_id;
    out->base_palette = slot.base_palette;
    out->flags = slot.flags;
    out->payload_crc = slot.payload_crc;
    out->payload_size = slot.payload_size;
    return out->metadata_valid;
}

bool SpectatorClient_CopyBufferedPaletteBank(uint8_t gameSlot, NetplayPaletteBank* out) {
    if (!out || gameSlot > 1) {
        return false;
    }

    const BufferedPaletteState& slot = s_palette[gameSlot];
    if (!IsBufferedPaletteBankReady(slot)) {
        return false;
    }

    out->valid = true;
    out->character_id = slot.character_id;
    out->base_palette = slot.base_palette;
    out->_pad = 0;
    out->crc32 = slot.payload_crc;
    memcpy(out->data, slot.data, NETPLAY_PALETTE_BANK_SIZE);
    return true;
}

} // namespace Net
