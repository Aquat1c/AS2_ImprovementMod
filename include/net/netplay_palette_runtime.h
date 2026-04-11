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

enum class NetplayPaletteBankSource : uint8_t {
    LiveMemory = 0,
    VanillaSource = 1,
    SavedCustom = 2,
};

struct NetplayPaletteBank {
    bool     valid;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  _pad;
    uint32_t crc32;
    uint8_t  data[NETPLAY_PALETTE_BANK_SIZE];
};

struct NetplayPalettePlayerState {
    bool     valid;
    uint8_t  game_slot;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  flags;
    bool     has_custom_bank;
    bool     custom_bank_ready;
    bool     vanilla_bank_ready;
    bool     live_bank_ready;
    bool     asset_loaded;
    uint32_t payload_crc;
    uint16_t payload_size;
    uint16_t asset_count;
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

struct NetplayPaletteLocalContext {
    bool     available;
    bool     match_active;
    bool     transport_enabled;
    bool     remote_preview_enabled;
    bool     has_custom_bank;
    bool     has_vanilla_bank;
    bool     has_live_bank;
    bool     asset_loaded;
    uint8_t  game_slot;
    uint8_t  character_id;
    uint8_t  base_palette;
    char     status[128];
};

struct NetplayPaletteReloadRequest {
    uint8_t  game_slot;
};

void NetplayPaletteRuntime_Init();
void NetplayPaletteRuntime_Shutdown();
void NetplayPaletteRuntime_FrameUpdate();

void NetplayPaletteRuntime_SetSyncEnabled(bool enabled);
void NetplayPaletteRuntime_SetRemotePreviewEnabled(bool enabled);
bool NetplayPaletteRuntime_GetSyncEnabled();
bool NetplayPaletteRuntime_GetRemotePreviewEnabled();

void NetplayPaletteRuntime_OnLockedMatchConfig(const LockedMatchConfig* config);
void NetplayPaletteRuntime_OnMatchEnd(const char* reason);
void NetplayPaletteRuntime_OnDisconnect(const char* reason);

void NetplayPaletteRuntime_OnRemoteConfig(const PaletteConfigPayload* payload);
void NetplayPaletteRuntime_OnRemoteData(const PaletteDataPayload* payload);
void NetplayPaletteRuntime_OnRemoteAck(const PaletteAckPayload* payload);

bool NetplayPaletteRuntime_CopyEditableLocalBank(NetplayPaletteBank* out);
bool NetplayPaletteRuntime_CopyLocalBankForSource(NetplayPaletteBankSource source, NetplayPaletteBank* out);
bool NetplayPaletteRuntime_SetLocalCustomBank(const NetplayPaletteBank* bank, bool persist_to_disk);
bool NetplayPaletteRuntime_ClearLocalCustomBank(bool delete_from_disk);
bool NetplayPaletteRuntime_SetOfflineEditorGameSlot(uint8_t game_slot);
void NetplayPaletteRuntime_GetLocalContext(NetplayPaletteLocalContext* out);

bool NetplayPaletteRuntime_CopyAssetOverrideBank(uint8_t game_slot, NetplayPaletteBank* out);
bool NetplayPaletteRuntime_CopySpectatorBank(uint8_t game_slot, NetplayPaletteBank* out);

void NetplayPaletteRuntime_OnAssetBankCaptured(uint8_t game_slot,
                                              uint8_t base_palette,
                                              const void* data,
                                              size_t len,
                                              uint16_t asset_count,
                                              const char* archive_path,
                                              const char* patch_path);
void NetplayPaletteRuntime_OnLiveBankObserved(uint8_t game_slot,
                                             uint8_t base_palette,
                                             const void* data,
                                             size_t len,
                                             uint16_t asset_count,
                                             const char* archive_path,
                                             const char* patch_path);
bool NetplayPaletteRuntime_ConsumeLiveReloadRequest(NetplayPaletteReloadRequest* out);
void NetplayPaletteRuntime_OnLiveReloadComplete(uint8_t game_slot, bool success, const char* reason);

void NetplayPaletteRuntime_GetSnapshot(NetplayPaletteRuntimeSnapshot* out);

} // namespace Net