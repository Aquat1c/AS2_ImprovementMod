/**
 * Alice Senki 2 - Netplay Palette Runtime
 *
 * Control-plane runtime for custom palette metadata exchange. This does not
 * apply palette assets yet; it only transports and tracks metadata.
 */

#pragma once

#include "net/locked_match_config.h"
#include "net/protocol.h"

#include <stddef.h>
#include <stdint.h>

namespace Net {

struct NetplayPalettePlayerState {
    bool     valid;
    uint8_t  game_slot;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  flags;
    uint16_t payload_size;
    uint32_t payload_crc;
    uint8_t  payload[NETPLAY_PALETTE_MAX_PAYLOAD];
};

struct NetplayPaletteRuntimeSnapshot {
    bool     initialized;
    bool     enabled;
    bool     remote_preview_enabled;
    bool     match_active;
    uint32_t epoch;
    uint32_t config_hash;
    bool     local_sent;
    bool     remote_acknowledged;
    NetplayPalettePlayerState player[2];
    char     status[128];
};

void NetplayPaletteRuntime_Init();
void NetplayPaletteRuntime_Shutdown();
void NetplayPaletteRuntime_FrameUpdate();

void NetplayPaletteRuntime_SetSyncEnabled(bool enabled);
void NetplayPaletteRuntime_SetRemotePreviewEnabled(bool enabled);
bool NetplayPaletteRuntime_GetSyncEnabled();
bool NetplayPaletteRuntime_GetRemotePreviewEnabled();

void NetplayPaletteRuntime_SetLocalOpaquePayload(const void* data, size_t len);
void NetplayPaletteRuntime_OnLockedMatchConfig(const LockedMatchConfig* config);
void NetplayPaletteRuntime_OnMatchEnd(const char* reason);
void NetplayPaletteRuntime_OnDisconnect(const char* reason);

void NetplayPaletteRuntime_OnRemoteConfig(const PaletteConfigPayload* payload);
void NetplayPaletteRuntime_OnRemoteAck(const PaletteAckPayload* payload);

void NetplayPaletteRuntime_GetSnapshot(NetplayPaletteRuntimeSnapshot* out);

} // namespace Net