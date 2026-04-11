#include "net/spectator_runtime.h"

#include "net/netplay_palette_runtime.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/spectator_manager.h"
#include "net/spectator_protocol.h"
#include "rollback/netplay_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

using namespace Net;

struct ArchivedFrame {
    Spectator::FrameRecord record;
    bool valid;
};

constexpr int kLiveEdgeSafetyFrames = 2;
constexpr DWORD kHeartbeatIntervalMs = 500;
constexpr int kMaxPeers = 8;

static bool s_initialized = false;
static bool s_enabled = true;
static bool s_matchActive = false;
static uint16_t s_listenPort = 10701;
static uint32_t s_matchId = 0;
static LockedMatchConfig s_config{};
static char s_p1Name[24] = "P1";
static char s_p2Name[24] = "P2";
static char s_status[128] = "Spectator runtime idle.";
static std::vector<ArchivedFrame> s_archive;
static int32_t s_archiveBaseRbFrame = 0;
static int32_t s_confirmedRbFrame = -1;
static int32_t s_liveRbFrame = -1;
static DWORD s_lastHeartbeatAt = 0;
static uint32_t s_lastBroadcastPaletteEpoch = 0;
static bool s_lastListenState = false;
static bool s_lastServeState = false;

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

static bool ShouldListenForSpectators() {
    return s_enabled &&
           Session_IsConnected() &&
           Session_GetRole() == SessionRole::Host;
}

static bool ShouldServeSpectators() {
    return ShouldListenForSpectators() && s_matchActive;
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
    memset(out, 0, sizeof(*out));
    out->match_id = s_matchId;
    out->match_state = s_matchActive
        ? Spectator::MATCH_STATE_ACTIVE
        : Spectator::MATCH_STATE_ENDED;
    out->archive_start_rb_frame = s_archiveBaseRbFrame;
    out->confirmed_rb_frame = s_confirmedRbFrame;
    out->live_rb_frame = s_liveRbFrame;
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

} // namespace

namespace Net {

void SpectatorRuntime_Init() {
    if (s_initialized) {
        return;
    }
    LockedMatchConfig_Clear(&s_config);
    ResetArchive();
    SetStatus("Spectator runtime idle.");
    SpectatorManager_Init();
    s_lastListenState = false;
    s_lastServeState = false;
    s_initialized = true;
}

void SpectatorRuntime_Shutdown() {
    if (!s_initialized) {
        return;
    }
    SpectatorRuntime_OnDisconnect("shutdown");
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
        Rollback::NetplayLog_Write("SPECTATE", -1,
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

    if (!shouldListen) {
        SetStatus("Spectator runtime idle.");
        return;
    }

    if (!shouldServe) {
        SetStatus("Spectator server listening on %u (no active match)", s_listenPort);
        return;
    }

    RefreshNamesFromSession();

    SpectatorPeerSnapshot peers[kMaxPeers] = {};
    const int peerCount = SpectatorManager_GetPeerSnapshots(peers, kMaxPeers);
    if (peerCount <= 0) {
        SetStatus("Spectator server listening on %u (no spectators)", s_listenPort);
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
        }

        const int32_t safeLiveFrame = s_confirmedRbFrame - kLiveEdgeSafetyFrames;
        if (safeLiveFrame < peer.next_rb_frame) {
            continue;
        }

        Spectator::FrameBatchPayload batch{};
        batch.match_id = s_matchId;
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

    NetplayPaletteRuntimeSnapshot paletteSnapshot{};
    NetplayPaletteRuntime_GetSnapshot(&paletteSnapshot);
    if (paletteSnapshot.epoch != s_lastBroadcastPaletteEpoch) {
        s_lastBroadcastPaletteEpoch = paletteSnapshot.epoch;
        for (int peerIndex = 0; peerIndex < peerCount; peerIndex++) {
            SendPaletteSyncForPeer(peers[peerIndex].peer_id, &palettePayload);
        }
    }

    const DWORD now = GetTickCount();
    if ((now - s_lastHeartbeatAt) >= kHeartbeatIntervalMs) {
        Spectator::HeartbeatPayload heartbeat{};
        heartbeat.match_id = s_matchId;
        heartbeat.confirmed_rb_frame = s_confirmedRbFrame;
        heartbeat.live_rb_frame = s_liveRbFrame;
        heartbeat.match_state = s_matchActive
            ? Spectator::MATCH_STATE_ACTIVE
            : Spectator::MATCH_STATE_ENDED;
        SpectatorManager_BroadcastHeartbeat(&heartbeat);
        s_lastHeartbeatAt = now;
    }

    SetStatus("Spectator server listening on %u (%d spectators, live=%d confirmed=%d)",
        s_listenPort,
        peerCount,
        s_liveRbFrame,
        s_confirmedRbFrame);
}

void SpectatorRuntime_SetEnabled(bool enabled) {
    s_enabled = enabled;
    if (!enabled) {
        SpectatorManager_SetEnabled(false);
        SetStatus("Spectator runtime disabled.");
    }
}

bool SpectatorRuntime_GetEnabled() {
    return s_enabled;
}

void SpectatorRuntime_SetListenPort(uint16_t port) {
    if (port != 0) {
        s_listenPort = port;
    }
}

uint16_t SpectatorRuntime_GetListenPort() {
    return s_listenPort;
}

void SpectatorRuntime_OnMatchBegin(const LockedMatchConfig* config) {
    if (!s_initialized || !config) {
        return;
    }

    s_config = *config;
    s_matchId = config->session_seed ^ (LockedMatchConfig_Hash(config) << 1);
    if (s_matchId == 0) {
        s_matchId = 1;
    }
    s_matchActive = true;
    s_lastBroadcastPaletteEpoch = 0;
    ResetArchive();
    RefreshNamesFromSession();
    SpectatorManager_BeginMatch(s_matchId);
    SetStatus("Spectator match prepared: id=0x%08X", s_matchId);
    Rollback::NetplayLog_Write("SPECTATE", -1,
        "Spectator match begin: id=0x%08X role=%s enabled=%d host_side=%u",
        s_matchId,
        SessionRoleName(Session_GetRole()),
        s_enabled ? 1 : 0,
        config->host_side);
}

void SpectatorRuntime_OnRollbackStarted(int32_t frame_origin_abs) {
    if (!s_matchActive) {
        return;
    }
    Rollback::NetplayLog_Write("SPECTATE", frame_origin_abs,
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

    Spectator::DisconnectPayload disconnect{};
    disconnect.reason_code = 0;
    CopyText(disconnect.message, sizeof(disconnect.message), reason ? reason : "match ended");
    SpectatorManager_BroadcastDisconnect(&disconnect);
    SpectatorManager_EndMatch(reason ? reason : "match ended");
    s_matchActive = false;
    SetStatus("Spectator match ended: %s", reason ? reason : "match ended");
    Rollback::NetplayLog_Write("SPECTATE", -1,
        "Spectator match end: id=0x%08X reason=%s live=%d confirmed=%d",
        s_matchId,
        reason ? reason : "match ended",
        s_liveRbFrame,
        s_confirmedRbFrame);
}

void SpectatorRuntime_OnDisconnect(const char* reason) {
    SpectatorRuntime_OnMatchEnd(reason ? reason : "disconnect");
    ResetArchive();
    s_matchId = 0;
    SetStatus("Spectator runtime idle.");
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
    out->listen_port = s_listenPort;
    out->match_id = s_matchId;
    out->connected_spectators = manager.connected_spectators;
    out->archive_start_rb_frame = s_archiveBaseRbFrame;
    out->confirmed_rb_frame = s_confirmedRbFrame;
    out->live_rb_frame = s_liveRbFrame;
    out->oldest_requested_rb_frame = manager.oldest_requested_rb_frame;
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net