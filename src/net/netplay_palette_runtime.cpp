#include "net/netplay_palette_runtime.h"

#include "net/player_side_mapping.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

constexpr DWORD kPaletteResendIntervalMs = 500;

static bool s_initialized = false;
static bool s_enabled = true;
static bool s_remotePreviewEnabled = false;
static bool s_matchActive = false;
static uint32_t s_epoch = 0;
static uint32_t s_configHash = 0;
static bool s_localDirty = false;
static bool s_localSent = false;
static bool s_remoteAcknowledged = false;
static DWORD s_lastSendAt = 0;
static int s_localGameSlot = -1;
static NetplayPalettePlayerState s_player[2] = {};
static char s_status[128] = "Palette runtime idle.";

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

static void ResetMatchState(const char* reason) {
    s_matchActive = false;
    s_configHash = 0;
    s_localDirty = false;
    s_localSent = false;
    s_remoteAcknowledged = false;
    s_lastSendAt = 0;
    s_localGameSlot = -1;
    memset(s_player, 0, sizeof(s_player));
    SetStatus("Palette runtime idle%s%s",
        reason ? ": " : "",
        reason ? reason : "");
}

static void RefreshLocalFlags() {
    if (s_localGameSlot < 0 || s_localGameSlot > 1) {
        return;
    }
    NetplayPalettePlayerState& local = s_player[s_localGameSlot];
    local.flags = 0;
    if (s_enabled) {
        local.flags |= NETPLAY_PALETTE_FLAG_TRANSPORT_ENABLED;
    }
    if (s_remotePreviewEnabled) {
        local.flags |= NETPLAY_PALETTE_FLAG_REMOTE_PREVIEW_ENABLED;
    }
    if (local.payload_size > 0) {
        local.flags |= NETPLAY_PALETTE_FLAG_HAS_CUSTOM_DATA;
    }
    local.flags |= NETPLAY_PALETTE_FLAG_SPECTATOR_PROPAGATE;
}

static bool SendLocalConfig() {
    if (!s_matchActive || !Session_IsConnected() || s_localGameSlot < 0 || s_localGameSlot > 1) {
        return false;
    }

    RefreshLocalFlags();

    const NetplayPalettePlayerState& local = s_player[s_localGameSlot];
    PaletteConfigPayload payload{};
    payload.epoch = s_epoch;
    payload.config_hash = s_configHash;
    payload.game_slot = (uint8_t)s_localGameSlot;
    payload.character_id = local.character_id;
    payload.base_palette = local.base_palette;
    payload.flags = local.flags;
    payload.payload_crc = local.payload_crc;
    payload.payload_size = (uint16_t)local.payload_size;
    if (local.payload_size > 0) {
        memcpy(payload.payload, local.payload, local.payload_size);
    }

    const bool sent = Session_SendPacket(
        CHANNEL_CONTROL,
        PacketType::PaletteConfig,
        &payload,
        sizeof(payload),
        true);
    if (sent) {
        s_localSent = true;
        s_localDirty = false;
        s_lastSendAt = GetTickCount();
        SetStatus("Palette epoch %u sent for P%d", s_epoch, s_localGameSlot + 1);
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Local palette config sent: epoch=%u config=0x%08X slot=P%d char=%u base=%u flags=0x%02X size=%u crc=0x%08X",
            s_epoch,
            s_configHash,
            s_localGameSlot + 1,
            local.character_id,
            local.base_palette,
            local.flags,
            (unsigned)local.payload_size,
            local.payload_crc);
    }
    return sent;
}

} // namespace

namespace Net {

void NetplayPaletteRuntime_Init() {
    if (s_initialized) {
        return;
    }
    ResetMatchState(nullptr);
    s_initialized = true;
}

void NetplayPaletteRuntime_Shutdown() {
    if (!s_initialized) {
        return;
    }
    ResetMatchState("shutdown");
    s_initialized = false;
}

void NetplayPaletteRuntime_FrameUpdate() {
    if (!s_initialized || !s_matchActive) {
        return;
    }

    if (s_localDirty) {
        SendLocalConfig();
        return;
    }

    if (!s_remoteAcknowledged && s_localSent) {
        const DWORD now = GetTickCount();
        if ((now - s_lastSendAt) >= kPaletteResendIntervalMs) {
            SendLocalConfig();
        }
    }
}

void NetplayPaletteRuntime_SetSyncEnabled(bool enabled) {
    if (s_enabled == enabled) {
        return;
    }
    s_enabled = enabled;
    RefreshLocalFlags();
    s_localDirty = true;
}

void NetplayPaletteRuntime_SetRemotePreviewEnabled(bool enabled) {
    if (s_remotePreviewEnabled == enabled) {
        return;
    }
    s_remotePreviewEnabled = enabled;
    RefreshLocalFlags();
    s_localDirty = true;
}

bool NetplayPaletteRuntime_GetSyncEnabled() {
    return s_enabled;
}

bool NetplayPaletteRuntime_GetRemotePreviewEnabled() {
    return s_remotePreviewEnabled;
}

void NetplayPaletteRuntime_SetLocalOpaquePayload(const void* data, size_t len) {
    if (s_localGameSlot < 0 || s_localGameSlot > 1) {
        return;
    }

    NetplayPalettePlayerState& local = s_player[s_localGameSlot];
    const size_t clamped = (len > NETPLAY_PALETTE_MAX_PAYLOAD)
        ? NETPLAY_PALETTE_MAX_PAYLOAD
        : len;
    local.payload_size = (uint16_t)clamped;
    if (clamped > 0 && data) {
        memcpy(local.payload, data, clamped);
        local.payload_crc = CalcCRC32(local.payload, clamped);
    } else {
        memset(local.payload, 0, sizeof(local.payload));
        local.payload_crc = 0;
    }
    RefreshLocalFlags();
    s_localDirty = true;
}

void NetplayPaletteRuntime_OnLockedMatchConfig(const LockedMatchConfig* config) {
    if (!s_initialized || !config) {
        return;
    }

    memset(s_player, 0, sizeof(s_player));
    s_epoch++;
    s_configHash = LockedMatchConfig_Hash(config);
    s_matchActive = true;
    s_localSent = false;
    s_remoteAcknowledged = false;
    s_lastSendAt = 0;

    s_player[0].valid = true;
    s_player[0].game_slot = 0;
    s_player[0].character_id = config->p1_character;
    s_player[0].base_palette = config->p1_palette;

    s_player[1].valid = true;
    s_player[1].game_slot = 1;
    s_player[1].character_id = config->p2_character;
    s_player[1].base_palette = config->p2_palette;

    const bool isHost = Session_GetRole() == SessionRole::Host;
    s_localGameSlot = PlayerMapping_DeriveFromRole(config->host_side, isHost);
    RefreshLocalFlags();
    s_localDirty = true;

    SetStatus("Palette runtime armed: epoch=%u config=0x%08X local=P%d",
        s_epoch,
        s_configHash,
        s_localGameSlot + 1);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Palette runtime armed: epoch=%u config=0x%08X local=P%d remote=P%d",
        s_epoch,
        s_configHash,
        s_localGameSlot + 1,
        s_localGameSlot == 0 ? 2 : 1);
}

void NetplayPaletteRuntime_OnMatchEnd(const char* reason) {
    ResetMatchState(reason ? reason : "match ended");
}

void NetplayPaletteRuntime_OnDisconnect(const char* reason) {
    ResetMatchState(reason ? reason : "disconnect");
}

void NetplayPaletteRuntime_OnRemoteConfig(const PaletteConfigPayload* payload) {
    if (!payload || payload->game_slot > 1) {
        return;
    }

    NetplayPalettePlayerState& remote = s_player[payload->game_slot];
    remote.valid = true;
    remote.game_slot = payload->game_slot;
    remote.character_id = payload->character_id;
    remote.base_palette = payload->base_palette;
    remote.flags = payload->flags;
    remote.payload_crc = payload->payload_crc;
    remote.payload_size = payload->payload_size > NETPLAY_PALETTE_MAX_PAYLOAD
        ? NETPLAY_PALETTE_MAX_PAYLOAD
        : payload->payload_size;
    if (remote.payload_size > 0) {
        memcpy(remote.payload, payload->payload, remote.payload_size);
    }

    PaletteAckPayload ack{};
    ack.epoch = payload->epoch;
    ack.config_hash = payload->config_hash;
    ack.game_slot = payload->game_slot;
    ack.accepted = (payload->config_hash == s_configHash) ? 1 : 0;
    ack.payload_size = remote.payload_size;
    ack.payload_crc = remote.payload_crc;
    Session_SendPacket(CHANNEL_CONTROL,
        PacketType::PaletteAck,
        &ack,
        sizeof(ack),
        true);

    SetStatus("Remote palette config received for P%d", payload->game_slot + 1);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Remote palette config: epoch=%u config=0x%08X slot=P%d char=%u base=%u flags=0x%02X size=%u crc=0x%08X",
        payload->epoch,
        payload->config_hash,
        payload->game_slot + 1,
        payload->character_id,
        payload->base_palette,
        payload->flags,
        (unsigned)remote.payload_size,
        remote.payload_crc);
}

void NetplayPaletteRuntime_OnRemoteAck(const PaletteAckPayload* payload) {
    if (!payload) {
        return;
    }

    if (payload->config_hash == s_configHash &&
        payload->epoch == s_epoch &&
        payload->accepted != 0) {
        s_remoteAcknowledged = true;
        SetStatus("Palette epoch %u acknowledged by remote", s_epoch);
    }
}

void NetplayPaletteRuntime_GetSnapshot(NetplayPaletteRuntimeSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->enabled = s_enabled;
    out->remote_preview_enabled = s_remotePreviewEnabled;
    out->match_active = s_matchActive;
    out->epoch = s_epoch;
    out->config_hash = s_configHash;
    out->local_sent = s_localSent;
    out->remote_acknowledged = s_remoteAcknowledged;
    out->player[0] = s_player[0];
    out->player[1] = s_player[1];
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net