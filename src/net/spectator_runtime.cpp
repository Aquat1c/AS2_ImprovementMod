#include <winsock2.h>
#include "net/spectator_runtime.h"

#include "net/netplay_palette_runtime.h"
#include "net/enet_transport.h"
#include "net/game_settings_sync.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/set_tracker.h"
#include "net/spectator_manager.h"
#include "net/spectator_protocol.h"
#include "rollback/netplay_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

using namespace Net;

#define SPECTATE_LOG(frame, fmt, ...) \
    Rollback::NetplayLog_WriteSpectator("SPECTATE", frame, fmt, ##__VA_ARGS__)

struct ArchivedFrame {
    Spectator::FrameRecord record;
    bool valid;
};

constexpr int kLiveEdgeSafetyFrames = 2;
constexpr DWORD kHeartbeatIntervalMs = 500;
constexpr int kMaxPeers = 8;
constexpr int kMaxArchiveFramesWithoutSpectators = 36000;

static bool s_initialized = false;
static bool s_enabled = true;
static bool s_matchActive = false;
static uint16_t s_listenPort = 10701;
static uint32_t s_matchId = 0;
static uint32_t s_matchOrdinal = 0;
static LockedMatchConfig s_config{};
static char s_p1Name[64] = "P1";
static char s_p2Name[64] = "P2";
static char s_status[128] = "Watch server idle.";
static uint16_t s_p1Wins = 0;
static uint16_t s_p2Wins = 0;
static uint16_t s_draws = 0;
static uint16_t s_completedMatches = 0;
static std::vector<ArchivedFrame> s_archive;
static int32_t s_archiveBaseRbFrame = 0;
static int32_t s_confirmedRbFrame = -1;
static int32_t s_liveRbFrame = -1;
static DWORD s_lastHeartbeatAt = 0;
static uint32_t s_lastBroadcastPaletteEpoch = 0;
static bool s_lastListenState = false;
static bool s_lastServeState = false;
static SOCKET s_discoverySocket = INVALID_SOCKET;
static uint32_t CurrentConfigCrc() {
    return LockedMatchConfig_Hash(&s_config);
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
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static uint16_t ClampCounterToU16(int value) {
    if (value <= 0) {
        return 0;
    }
    return value >= 65535 ? 65535 : (uint16_t)value;
}

static void RefreshSetScoreSnapshot() {
    SetTrackerSnapshot tracker{};
    SetTracker_GetSnapshot(&tracker);

    int p1Wins = 0;
    int p2Wins = 0;
    SetTracker_GetGameSideWins(&p1Wins, &p2Wins);

    s_p1Wins = ClampCounterToU16(p1Wins);
    s_p2Wins = ClampCounterToU16(p2Wins);
    s_draws = ClampCounterToU16(tracker.draws);
    s_completedMatches = ClampCounterToU16(tracker.total_matches);
}

static bool ShouldListenForSpectators() {
    return s_enabled &&
           Session_IsConnected() &&
           Session_GetRole() == SessionRole::Host;
}

static bool ShouldServeSpectators() {
    return ShouldListenForSpectators() && s_matchActive;
}

static uint16_t ResolveSessionListenPort() {
    SessionSnapshot session{};
    Session_GetSnapshot(&session);
    return session.local_listen_port;
}

static void CloseLanDiscoverySocket() {
    if (s_discoverySocket == INVALID_SOCKET) {
        return;
    }
    closesocket(s_discoverySocket);
    s_discoverySocket = INVALID_SOCKET;
}

static bool EnsureLanDiscoverySocket() {
    if (s_discoverySocket != INVALID_SOCKET) {
        return true;
    }

    if (!Transport_GlobalInit()) {
        return false;
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        SPECTATE_LOG(-1,
            "LAN discovery responder socket create failed: err=%d",
            (int)WSAGetLastError());
        return false;
    }

    BOOL reuseAddr = TRUE;
    setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    u_long nonBlocking = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) != 0) {
        SPECTATE_LOG(-1,
            "LAN discovery responder nonblocking failed: err=%d",
            (int)WSAGetLastError());
        closesocket(socketHandle);
        return false;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(Spectator::LAN_DISCOVERY_PORT);
    if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        SPECTATE_LOG(-1,
            "LAN discovery responder bind failed: port=%u err=%d",
            Spectator::LAN_DISCOVERY_PORT,
            (int)WSAGetLastError());
        closesocket(socketHandle);
        return false;
    }

    s_discoverySocket = socketHandle;
    SPECTATE_LOG(-1,
        "LAN discovery responder listening: port=%u",
        Spectator::LAN_DISCOVERY_PORT);
    return true;
}

static void PollLanDiscoveryRequests(bool shouldListen) {
    if (!shouldListen) {
        CloseLanDiscoverySocket();
        return;
    }

    if (!EnsureLanDiscoverySocket()) {
        return;
    }

    for (int attempt = 0; attempt < 8; attempt++) {
        Spectator::DiscoveryQueryPayload query{};
        sockaddr_in fromAddr{};
        int fromAddrLen = sizeof(fromAddr);
        const int recvResult = recvfrom(
            s_discoverySocket,
            reinterpret_cast<char*>(&query),
            sizeof(query),
            0,
            reinterpret_cast<sockaddr*>(&fromAddr),
            &fromAddrLen);
        if (recvResult == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                SPECTATE_LOG(-1,
                    "LAN discovery responder recv failed: err=%d",
                    error);
                CloseLanDiscoverySocket();
            }
            break;
        }

        if (recvResult < (int)sizeof(query) ||
            query.magic != Spectator::LAN_DISCOVERY_MAGIC ||
            query.version != Spectator::LAN_DISCOVERY_VERSION) {
            continue;
        }

        SessionSnapshot session{};
        Session_GetSnapshot(&session);
        SpectatorManagerSnapshot manager{};
        SpectatorManager_GetSnapshot(&manager);
        const uint16_t listenPort = manager.server_active && manager.listen_port != 0
            ? manager.listen_port
            : s_listenPort;

        Spectator::DiscoveryResponsePayload response{};
        response.magic = Spectator::LAN_DISCOVERY_MAGIC;
        response.version = Spectator::LAN_DISCOVERY_VERSION;
        response.spectator_port = listenPort;
        response.session_listen_port = ResolveSessionListenPort();
        response.nonce = query.nonce;
        response.match_id = s_matchId;
        response.match_state = s_matchActive
            ? Spectator::MATCH_STATE_ACTIVE
            : (s_matchId != 0 ? Spectator::MATCH_STATE_ENDED : Spectator::MATCH_STATE_IDLE);
        response.connected_spectators = (uint8_t)(manager.connected_spectators > 255 ? 255 : manager.connected_spectators);
        CopyText(response.host_nickname, sizeof(response.host_nickname),
            session.local_nickname[0] ? session.local_nickname : "Host");
        CopyText(response.p1_name, sizeof(response.p1_name), s_p1Name);
        CopyText(response.p2_name, sizeof(response.p2_name), s_p2Name);

        char hostAddress[64] = {};
        if (!InetNtopA(AF_INET, &fromAddr.sin_addr, hostAddress, sizeof(hostAddress))) {
            const char* legacyAddress = inet_ntoa(fromAddr.sin_addr);
            if (legacyAddress && legacyAddress[0]) {
                strncpy_s(hostAddress, sizeof(hostAddress), legacyAddress, _TRUNCATE);
            }
        }

        sendto(
            s_discoverySocket,
            reinterpret_cast<const char*>(&response),
            sizeof(response),
            0,
            reinterpret_cast<const sockaddr*>(&fromAddr),
            fromAddrLen);

        SPECTATE_LOG(-1,
            "LAN discovery query served: from=%s nonce=0x%08X port=%u match_id=0x%08X match_state=%u spectators=%u",
            hostAddress[0] ? hostAddress : "?",
            query.nonce,
            listenPort,
            response.match_id,
            response.match_state,
            response.connected_spectators);
    }
}

static void ResetArchive() {
    s_archive.clear();
    s_archiveBaseRbFrame = 0;
    s_confirmedRbFrame = -1;
    s_liveRbFrame = -1;
}

static ArchivedFrame* EnsureFrameSlot(int32_t rb_frame) {
    if (rb_frame < 0) {
        return nullptr;
    }
    if (s_archive.empty()) {
        s_archiveBaseRbFrame = rb_frame;
        s_archive.resize(1);
        return &s_archive[0];
    }
    if (rb_frame < s_archiveBaseRbFrame) {
        return nullptr;
    }
    const size_t index = (size_t)(rb_frame - s_archiveBaseRbFrame);
    if (index >= s_archive.size()) {
        s_archive.resize(index + 1);
    }
    return &s_archive[index];
}

static const ArchivedFrame* GetFrameSlot(int32_t rb_frame) {
    if (rb_frame < s_archiveBaseRbFrame) {
        return nullptr;
    }
    const size_t index = (size_t)(rb_frame - s_archiveBaseRbFrame);
    if (index >= s_archive.size()) {
        return nullptr;
    }
    return &s_archive[index];
}

static void PruneArchive(int peerCount) {
    if (s_archive.empty()) {
        return;
    }

    int32_t keepFromRbFrame = s_archiveBaseRbFrame;
    if (peerCount > 0) {
        const int32_t oldestRequestedRbFrame = SpectatorManager_GetOldestRequestedFrame();
        if (oldestRequestedRbFrame >= 0) {
            keepFromRbFrame = (std::max)(keepFromRbFrame, oldestRequestedRbFrame);
        }
    } else if ((int)s_archive.size() > kMaxArchiveFramesWithoutSpectators) {
        const int32_t archiveEndRbFrame = s_archiveBaseRbFrame + (int32_t)s_archive.size() - 1;
        keepFromRbFrame = archiveEndRbFrame - kMaxArchiveFramesWithoutSpectators + 1;
    }

    const int32_t archiveEndRbFrame = s_archiveBaseRbFrame + (int32_t)s_archive.size() - 1;
    keepFromRbFrame = (std::min)(keepFromRbFrame, archiveEndRbFrame);
    if (keepFromRbFrame <= s_archiveBaseRbFrame) {
        return;
    }

    const size_t trimCount = (size_t)(keepFromRbFrame - s_archiveBaseRbFrame);
    s_archive.erase(s_archive.begin(), s_archive.begin() + trimCount);
    s_archiveBaseRbFrame = keepFromRbFrame;
}

static void RefreshNamesFromSession() {
    SessionSnapshot session{};
    Session_GetSnapshot(&session);

    const bool hostIsP1 = (s_config.host_side == 0);
    const char* localNick = session.local_nickname[0] ? session.local_nickname : "Local";
    const char* remoteNick = session.remote_peer.nickname[0] ? session.remote_peer.nickname : "Remote";

    if (Session_GetRole() == SessionRole::Host) {
        CopyText(hostIsP1 ? s_p1Name : s_p2Name, sizeof(s_p1Name), localNick);
        CopyText(hostIsP1 ? s_p2Name : s_p1Name, sizeof(s_p2Name), remoteNick);
    } else {
        CopyText(hostIsP1 ? s_p1Name : s_p2Name, sizeof(s_p1Name), remoteNick);
        CopyText(hostIsP1 ? s_p2Name : s_p1Name, sizeof(s_p2Name), localNick);
    }
}

static void BuildMatchStatePayload(Spectator::MatchStatePayload* out) {
    if (!out) {
        return;
    }

    RefreshSetScoreSnapshot();
    memset(out, 0, sizeof(*out));
    out->match_id = s_matchId;
    out->match_ordinal = s_matchOrdinal;
    out->config_crc = CurrentConfigCrc();
    out->match_state = s_matchActive
        ? Spectator::MATCH_STATE_ACTIVE
        : Spectator::MATCH_STATE_ENDED;
    out->archive_start_rb_frame = s_archiveBaseRbFrame;
    out->confirmed_rb_frame = s_confirmedRbFrame;
    out->live_rb_frame = s_liveRbFrame;
    out->p1_wins = s_p1Wins;
    out->p2_wins = s_p2Wins;
    out->draws = s_draws;
    out->completed_matches = s_completedMatches;
    out->session_listen_port = ResolveSessionListenPort();
    out->config = s_config;
    CopyText(out->p1_name, sizeof(out->p1_name), s_p1Name);
    CopyText(out->p2_name, sizeof(out->p2_name), s_p2Name);
}

static void BuildPalettePayload(Spectator::PaletteStatePayload* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    NetplayPaletteRuntimeSnapshot palette{};
    NetplayPaletteRuntime_GetSnapshot(&palette);
    out->match_id = s_matchId;
    out->match_ordinal = s_matchOrdinal;
    out->config_crc = CurrentConfigCrc();
    out->session_seed = s_config.session_seed;
    out->palette_epoch = palette.epoch;

    for (int index = 0; index < 2; index++) {
        out->player[index].character_id = palette.player[index].character_id;
        out->player[index].base_palette = palette.player[index].base_palette;
        out->player[index].flags = palette.player[index].flags;
        out->player[index].has_custom_data = palette.player[index].has_custom_bank ? 1 : 0;
        out->player[index].payload_size = palette.player[index].payload_size;
        out->player[index].payload_crc = palette.player[index].payload_crc;
    }
}

static bool SendPaletteSyncForPeer(uintptr_t peerId,
                                   const Spectator::PaletteStatePayload* statePayload) {
    if (!statePayload || !SpectatorManager_SendPaletteState(peerId, statePayload)) {
        return false;
    }

    for (int index = 0; index < 2; index++) {
        if (!statePayload->player[index].has_custom_data ||
            statePayload->player[index].payload_size != NETPLAY_PALETTE_BANK_SIZE) {
            continue;
        }

        NetplayPaletteBank bank{};
        if (!NetplayPaletteRuntime_CopySpectatorBank((uint8_t)index, &bank)) {
            continue;
        }

        Spectator::PaletteDataPayload data{};
        data.match_id = statePayload->match_id;
        data.match_ordinal = statePayload->match_ordinal;
        data.config_crc = statePayload->config_crc;
        data.session_seed = statePayload->session_seed;
        data.palette_epoch = statePayload->palette_epoch;
        data.game_slot = (uint8_t)index;
        data.character_id = bank.character_id;
        data.base_palette = bank.base_palette;
        data.payload_crc = bank.crc32;
        data.payload_size = NETPLAY_PALETTE_BANK_SIZE;
        memcpy(data.payload, bank.data, NETPLAY_PALETTE_BANK_SIZE);
        SpectatorManager_SendPaletteData(peerId, &data);
    }

    return true;
}

static void SendInactiveHeartbeatIfNeeded(DWORD now) {
    if (s_matchActive || s_matchId == 0 || (now - s_lastHeartbeatAt) < kHeartbeatIntervalMs) {
        return;
    }

    SpectatorPeerSnapshot peers[kMaxPeers] = {};
    const int peerCount = SpectatorManager_GetPeerSnapshots(peers, kMaxPeers);
    if (peerCount <= 0) {
        return;
    }

    Spectator::HeartbeatPayload heartbeat{};
    heartbeat.match_id = s_matchId;
    heartbeat.match_ordinal = s_matchOrdinal;
    heartbeat.config_crc = CurrentConfigCrc();
    heartbeat.session_seed = s_config.session_seed;
    heartbeat.confirmed_rb_frame = s_confirmedRbFrame;
    heartbeat.live_rb_frame = s_liveRbFrame;
    heartbeat.session_listen_port = ResolveSessionListenPort();
    heartbeat.match_state = Spectator::MATCH_STATE_ENDED;

    int sentCount = 0;
    for (int peerIndex = 0; peerIndex < peerCount; peerIndex++) {
        const SpectatorPeerSnapshot& peer = peers[peerIndex];
        if (!peer.handshake_complete || peer.needs_full_sync) {
            continue;
        }

        if (SpectatorManager_SendHeartbeat(peer.peer_id, &heartbeat)) {
            sentCount++;
        }
    }

    if (sentCount <= 0) {
        return;
    }

    s_lastHeartbeatAt = now;
    SPECTATE_LOG(-1,
        "Inactive spectator heartbeat peers=%d match_id=0x%08X ordinal=%u confirmed=%d live=%d",
        sentCount,
        s_matchId,
        s_matchOrdinal,
        s_confirmedRbFrame,
        s_liveRbFrame);
}

} // namespace

namespace Net {

void SpectatorRuntime_Init() {
    if (s_initialized) {
        return;
    }
    LockedMatchConfig_Clear(&s_config);
    ResetArchive();
    SetStatus("Watch server idle.");
    SpectatorManager_Init();
    s_lastListenState = false;
    s_lastServeState = false;
    s_initialized = true;
    SPECTATE_LOG(-1,
        "Spectator runtime init enabled=%d listen_port=%u",
        s_enabled ? 1 : 0,
        s_listenPort);
}

void SpectatorRuntime_Shutdown() {
    if (!s_initialized) {
        return;
    }
    SPECTATE_LOG(-1, "Spectator runtime shutdown");
    SpectatorRuntime_OnDisconnect("shutdown");
    CloseLanDiscoverySocket();
    SpectatorManager_Shutdown();
    s_initialized = false;
}

void SpectatorRuntime_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    const bool shouldListen = ShouldListenForSpectators();
    const bool shouldServe = shouldListen && s_matchActive;

    if (shouldListen != s_lastListenState || shouldServe != s_lastServeState) {
        SPECTATE_LOG(-1,
            "[SpectatorRuntime] listener=%d serve=%d role=%s session_connected=%d match_active=%d port=%u",
            shouldListen ? 1 : 0,
            shouldServe ? 1 : 0,
            SessionRoleName(Session_GetRole()),
            Session_IsConnected() ? 1 : 0,
            s_matchActive ? 1 : 0,
            s_listenPort);
        s_lastListenState = shouldListen;
        s_lastServeState = shouldServe;
    }

    SpectatorManager_SetListenPort(s_listenPort);
    SpectatorManager_SetEnabled(shouldListen);
    SpectatorManager_FrameUpdate();
    PollLanDiscoveryRequests(shouldListen);

    SpectatorManagerSnapshot manager{};
    SpectatorManager_GetSnapshot(&manager);
    const uint16_t effectiveListenPort = manager.server_active && manager.listen_port != 0
        ? manager.listen_port
        : s_listenPort;

    if (!shouldListen) {
        SetStatus("Watch server idle.");
        return;
    }

    if (!shouldServe) {
        SendInactiveHeartbeatIfNeeded(GetTickCount());
        if (effectiveListenPort != s_listenPort) {
            SetStatus("Watch server ready on %u. Waiting for a match (requested %u).",
                effectiveListenPort,
                s_listenPort);
        } else {
            SetStatus("Watch server ready on %u. Waiting for a match.", effectiveListenPort);
        }
        return;
    }

    RefreshNamesFromSession();

    SpectatorPeerSnapshot peers[kMaxPeers] = {};
    const int peerCount = SpectatorManager_GetPeerSnapshots(peers, kMaxPeers);
    if (peerCount <= 0) {
        PruneArchive(0);
        if (effectiveListenPort != s_listenPort) {
            SetStatus("Watch server on %u. Requested %u. Game %u, set score %u-%u. No viewers yet.",
                effectiveListenPort,
                s_listenPort,
                s_matchOrdinal,
                s_p1Wins,
                s_p2Wins);
        } else {
            SetStatus("Watch server on %u. Game %u, set score %u-%u. No viewers yet.",
                effectiveListenPort,
                s_matchOrdinal,
                s_p1Wins,
                s_p2Wins);
        }
        return;
    }

    Spectator::MatchStatePayload matchPayload{};
    BuildMatchStatePayload(&matchPayload);

    Spectator::PaletteStatePayload palettePayload{};
    BuildPalettePayload(&palettePayload);

    for (int peerIndex = 0; peerIndex < peerCount; peerIndex++) {
        const SpectatorPeerSnapshot& peer = peers[peerIndex];
        if (peer.needs_full_sync) {
            SpectatorManager_SendMatchState(peer.peer_id, &matchPayload);
            SendPaletteSyncForPeer(peer.peer_id, &palettePayload);
            SpectatorManager_SetPeerNextFrame(peer.peer_id, s_archiveBaseRbFrame);
            SpectatorManager_ClearPeerFullSync(peer.peer_id);
            SPECTATE_LOG(-1,
                "Full sync sent peer=0x%p match_id=0x%08X ordinal=%u archive_start=%d confirmed=%d live=%d palette_epoch=%u",
                reinterpret_cast<void*>(peer.peer_id),
                s_matchId,
                s_matchOrdinal,
                s_archiveBaseRbFrame,
                s_confirmedRbFrame,
                s_liveRbFrame,
                palettePayload.palette_epoch);
        }

        const int32_t safeLiveFrame = s_confirmedRbFrame - kLiveEdgeSafetyFrames;
        if (safeLiveFrame < peer.next_rb_frame) {
            continue;
        }

        Spectator::FrameBatchPayload batch{};
        batch.match_id = s_matchId;
        batch.match_ordinal = s_matchOrdinal;
        batch.config_crc = CurrentConfigCrc();
        batch.session_seed = s_config.session_seed;
        batch.archive_start_rb_frame = s_archiveBaseRbFrame;
        batch.confirmed_rb_frame = s_confirmedRbFrame;
        batch.live_rb_frame = s_liveRbFrame;

        int32_t nextFrame = peer.next_rb_frame;
        for (; batch.record_count < Spectator::MAX_FRAME_BATCH && nextFrame <= safeLiveFrame; nextFrame++) {
            const ArchivedFrame* slot = GetFrameSlot(nextFrame);
            if (!slot || !slot->valid) {
                break;
            }

            Spectator::FrameRecord record = slot->record;
            if (record.rb_frame <= s_confirmedRbFrame) {
                record.flags |= Spectator::FRAME_FLAG_CONFIRMED;
            }
            batch.records[batch.record_count++] = record;
        }

        if (batch.record_count > 0 && SpectatorManager_SendFrameBatch(peer.peer_id, &batch)) {
            SpectatorManager_SetPeerNextFrame(peer.peer_id,
                batch.records[batch.record_count - 1].rb_frame + 1);
        }
    }

    PruneArchive(peerCount);

    NetplayPaletteRuntimeSnapshot paletteSnapshot{};
    NetplayPaletteRuntime_GetSnapshot(&paletteSnapshot);
    if (paletteSnapshot.epoch != s_lastBroadcastPaletteEpoch) {
        s_lastBroadcastPaletteEpoch = paletteSnapshot.epoch;
        SPECTATE_LOG(-1,
            "Broadcast palette epoch=%u peers=%d",
            paletteSnapshot.epoch,
            peerCount);
        for (int peerIndex = 0; peerIndex < peerCount; peerIndex++) {
            SendPaletteSyncForPeer(peers[peerIndex].peer_id, &palettePayload);
        }
    }

    const DWORD now = GetTickCount();
    if ((now - s_lastHeartbeatAt) >= kHeartbeatIntervalMs) {
        Spectator::HeartbeatPayload heartbeat{};
        heartbeat.match_id = s_matchId;
        heartbeat.match_ordinal = s_matchOrdinal;
        heartbeat.config_crc = CurrentConfigCrc();
        heartbeat.session_seed = s_config.session_seed;
        heartbeat.confirmed_rb_frame = s_confirmedRbFrame;
        heartbeat.live_rb_frame = s_liveRbFrame;
        heartbeat.session_listen_port = ResolveSessionListenPort();
        heartbeat.match_state = s_matchActive
            ? Spectator::MATCH_STATE_ACTIVE
            : Spectator::MATCH_STATE_ENDED;
        SpectatorManager_BroadcastHeartbeat(&heartbeat);
        s_lastHeartbeatAt = now;
    }

    if (effectiveListenPort != s_listenPort) {
        SetStatus("Watch server on %u. Requested %u. %d viewers. Game %u, set score %u-%u.",
            effectiveListenPort,
            s_listenPort,
            peerCount,
            s_matchOrdinal,
            s_p1Wins,
            s_p2Wins);
    } else {
        SetStatus("Watch server on %u. %d viewers. Game %u, set score %u-%u.",
            effectiveListenPort,
            peerCount,
            s_matchOrdinal,
            s_p1Wins,
            s_p2Wins);
    }
}

void SpectatorRuntime_SetEnabled(bool enabled) {
    s_enabled = enabled;
    SPECTATE_LOG(-1,
        "Spectator runtime enabled=%d",
        enabled ? 1 : 0);
    if (!enabled) {
        SpectatorManager_SetEnabled(false);
        SetStatus("Watch server disabled.");
    }
}

bool SpectatorRuntime_GetEnabled() {
    return s_enabled;
}

void SpectatorRuntime_SetListenPort(uint16_t port) {
    if (port != 0) {
        if (s_listenPort != port) {
            SPECTATE_LOG(-1,
                "Spectator runtime listen port %u -> %u",
                s_listenPort,
                port);
        }
        s_listenPort = port;
    }
}

uint16_t SpectatorRuntime_GetListenPort() {
    return s_listenPort;
}

void SpectatorRuntime_OnSelectionCommitted(const LockedMatchConfig* config) {
    if (!s_initialized || !config) {
        return;
    }
    if (!s_enabled) {
        SPECTATE_LOG(-1, "Selection committed but spectator server is disabled — skipping PreMatchState");
        return;
    }
    if (!Session_IsConnected()) {
        SPECTATE_LOG(-1, "Selection committed but session is not connected — skipping PreMatchState");
        return;
    }
    if (Session_GetRole() != SessionRole::Host) {
        // Joiner doesn't run the spectator server; this is expected, not an error.
        return;
    }

    // Compute the provisional match identity — same formula OnMatchBegin will use,
    // so the spectator can match against the MatchState that arrives at gameplay start.
    // Same formula as OnMatchBegin so pre_match_id == match_id for this match.
    uint32_t preMatchId = config->session_seed ^ (LockedMatchConfig_Hash(config) << 1);
    if (preMatchId == 0) {
        preMatchId = 1;
    }
    const uint32_t preMatchOrdinal = s_matchOrdinal + 1;

    RefreshNamesFromSession();

    Spectator::PreMatchStatePayload payload{};
    payload.pre_match_id = preMatchId;
    payload.pre_match_ordinal = preMatchOrdinal;
    payload.config_crc = LockedMatchConfig_Hash(config);
    payload.session_seed = config->session_seed;
    payload.config = *config;
    CopyText(payload.p1_name, sizeof(payload.p1_name), s_p1Name);
    CopyText(payload.p2_name, sizeof(payload.p2_name), s_p2Name);

    SpectatorPeerSnapshot peers[kMaxPeers] = {};
    const int peerCount = SpectatorManager_GetPeerSnapshots(peers, kMaxPeers);
    int sent = 0;
    for (int i = 0; i < peerCount; i++) {
        if (SpectatorManager_SendPreMatchState(peers[i].peer_id, &payload)) {
            sent++;
        }
    }

    SPECTATE_LOG(-1,
        "Selection committed: pre_match_id=0x%08X ordinal=%u chars=(%u,%u) stage=%u spectators_notified=%d/%d",
        preMatchId,
        preMatchOrdinal,
        (unsigned)config->p1_character,
        (unsigned)config->p2_character,
        (unsigned)config->stage_id,
        sent,
        peerCount);
}

void SpectatorRuntime_OnMatchBegin(const LockedMatchConfig* config) {
    if (!s_initialized || !config) {
        return;
    }

    s_config = *config;
    s_matchOrdinal++;
    if (s_matchOrdinal == 0) {
        s_matchOrdinal = 1;
    }
    s_matchId = config->session_seed ^ (LockedMatchConfig_Hash(config) << 1);
    if (s_matchId == 0) {
        s_matchId = 1;
    }
    s_matchActive = true;
    s_lastBroadcastPaletteEpoch = 0;
    RefreshSetScoreSnapshot();
    ResetArchive();
    RefreshNamesFromSession();
    SpectatorManager_BeginMatch(s_matchId, s_matchOrdinal);
    SetStatus("Watch server ready for game %u. Set score %u-%u.",
        s_matchOrdinal,
        s_p1Wins,
        s_p2Wins);
    SPECTATE_LOG(-1,
        "Spectator match begin: id=0x%08X ordinal=%u role=%s enabled=%d host_side=%u rounds_raw=%u rounds_to_win=%d score=%u-%u draws=%u completed=%u",
        s_matchId,
        s_matchOrdinal,
        SessionRoleName(Session_GetRole()),
        s_enabled ? 1 : 0,
        config->host_side,
        config->round_count,
        GameSettingsSync_RoundsToWin(config->round_count),
        s_p1Wins,
        s_p2Wins,
        s_draws,
        s_completedMatches);
}

void SpectatorRuntime_OnRollbackStarted(int32_t frame_origin_abs) {
    if (!s_matchActive) {
        return;
    }
    SPECTATE_LOG(frame_origin_abs,
        "Spectator rollback stream armed: frame_origin_abs=%d match_id=0x%08X",
        frame_origin_abs,
        s_matchId);
}

void SpectatorRuntime_OnGameplayFrame(int32_t rb_frame,
                                      int32_t game_abs_frame,
                                      uint16_t p1_input,
                                      uint16_t p2_input,
                                      bool rolling_back,
                                      int32_t confirmed_rb_frame) {
    if (!s_matchActive || !ShouldServeSpectators()) {
        return;
    }

    ArchivedFrame* slot = EnsureFrameSlot(rb_frame);
    if (!slot) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->record.rb_frame = rb_frame;
    slot->record.game_abs_frame = game_abs_frame;
    slot->record.p1_input = p1_input;
    slot->record.p2_input = p2_input;
    slot->record.flags = rolling_back ? Spectator::FRAME_FLAG_ROLLBACK_REWRITE : 0;

    s_liveRbFrame = (std::max)(s_liveRbFrame, rb_frame);
    s_confirmedRbFrame = (std::max)(s_confirmedRbFrame, confirmed_rb_frame);
}

void SpectatorRuntime_OnMatchEnd(const char* reason) {
    if (!s_matchActive) {
        return;
    }

    RefreshNamesFromSession();
    RefreshSetScoreSnapshot();

    Spectator::MatchStatePayload matchPayload{};
    BuildMatchStatePayload(&matchPayload);
    matchPayload.match_state = Spectator::MATCH_STATE_ENDED;

    SpectatorPeerSnapshot peers[kMaxPeers] = {};
    const int peerCount = SpectatorManager_GetPeerSnapshots(peers, kMaxPeers);
    for (int peerIndex = 0; peerIndex < peerCount; peerIndex++) {
        SpectatorManager_SendMatchState(peers[peerIndex].peer_id, &matchPayload);
    }

    Spectator::HeartbeatPayload heartbeat{};
    heartbeat.match_id = s_matchId;
    heartbeat.match_ordinal = s_matchOrdinal;
    heartbeat.config_crc = CurrentConfigCrc();
    heartbeat.session_seed = s_config.session_seed;
    heartbeat.confirmed_rb_frame = s_confirmedRbFrame;
    heartbeat.live_rb_frame = s_liveRbFrame;
    heartbeat.session_listen_port = ResolveSessionListenPort();
    heartbeat.match_state = Spectator::MATCH_STATE_ENDED;
    SpectatorManager_BroadcastHeartbeat(&heartbeat);

    SpectatorManager_EndMatch(reason ? reason : "match ended");
    s_matchActive = false;
    SetStatus("The watch server finished the match.");
    SPECTATE_LOG(-1,
        "Spectator match end: id=0x%08X reason=%s live=%d confirmed=%d",
        s_matchId,
        reason ? reason : "match ended",
        s_liveRbFrame,
        s_confirmedRbFrame);
}

void SpectatorRuntime_OnDisconnect(const char* reason) {
    SPECTATE_LOG(-1,
        "Spectator runtime disconnect reason=%s",
        reason ? reason : "disconnect");
    SpectatorRuntime_OnMatchEnd(reason ? reason : "disconnect");
    ResetArchive();
    s_matchId = 0;
    s_matchOrdinal = 0;
    s_p1Wins = 0;
    s_p2Wins = 0;
    s_draws = 0;
    s_completedMatches = 0;
    SetStatus("Watch server idle.");
    s_lastServeState = false;
}

void SpectatorRuntime_GetSnapshot(SpectatorRuntimeSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    SpectatorManagerSnapshot manager{};
    SpectatorManager_GetSnapshot(&manager);

    out->initialized = s_initialized;
    out->enabled = s_enabled;
    out->match_active = s_matchActive;
    out->server_active = manager.server_active;
    out->listen_port = manager.server_active && manager.listen_port != 0
        ? manager.listen_port
        : s_listenPort;
    out->match_id = s_matchId;
    out->connected_spectators = manager.connected_spectators;
    out->archive_start_rb_frame = s_archiveBaseRbFrame;
    out->confirmed_rb_frame = s_confirmedRbFrame;
    out->live_rb_frame = s_liveRbFrame;
    out->oldest_requested_rb_frame = manager.oldest_requested_rb_frame;
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net
