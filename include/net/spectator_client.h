/**
 * Alice Senki 2 - Spectator Sidecar Client
 *
 * Client-side archived-input buffer and synthetic playback cursor used for
 * spectator-mode scaffolding.
 */

#pragma once

#include "net/locked_match_config.h"

#include <stdint.h>

namespace Net {

struct NetplayPaletteBank;

constexpr int SPECTATOR_DISCOVERY_MAX_RESULTS = 8;

enum class SpectatorClientState : uint8_t {
    Idle = 0,
    Connecting,
    Handshaking,
    ConnectedNoActiveMatch,
    Streaming,
    Redirected,
    Failed,
};

struct SpectatorClientSnapshot {
    bool     initialized;
    bool     active;
    SpectatorClientState state;
    char     endpoint[96];
    char     redirect_endpoint[96];
    uint16_t session_listen_port;
    uint32_t match_id;
    uint32_t match_ordinal;
    bool     have_match_state;
    bool     match_active;
    uint32_t config_crc;
    uint32_t session_seed;
    LockedMatchConfig config;
    // Set when a PreMatchState packet arrives (before MatchState/archive).
    // Lets playback start charsel bootstrap early, in parallel with loading.
    bool     have_pre_match_state;
    uint32_t pre_match_id;
    uint32_t pre_match_ordinal;
    uint32_t pre_match_config_crc;
    uint32_t pre_match_session_seed;
    LockedMatchConfig pre_match_config;
    char     pre_match_p1_name[64];
    char     pre_match_p2_name[64];
    char     p1_name[64];
    char     p2_name[64];
    uint16_t p1_wins;
    uint16_t p2_wins;
    uint16_t draws;
    uint16_t completed_matches;
    int32_t  buffered_start_rb_frame;
    int32_t  buffered_end_rb_frame;
    int32_t  server_confirmed_rb_frame;
    int32_t  confirmed_contiguous_rb_frame;
    int32_t  server_live_rb_frame;
    int32_t  playback_rb_frame;
    uint32_t buffered_frame_count;
    bool     archive_has_gap;
    bool     should_fast_forward;
    bool     needs_hard_sync;
    bool     relay_server_active;
    uint16_t relay_listen_port;
    uint32_t relay_connected_spectators;
    char     status[128];
    char     error[128];
};

struct SpectatorDiscoveryEntry {
    char     endpoint[96];
    char     host_nickname[64];
    char     p1_name[64];
    char     p2_name[64];
    uint32_t match_id;
    bool     match_active;
    uint8_t  connected_spectators;
};

struct SpectatorDiscoverySnapshot {
    bool     active;
    uint32_t result_count;
    char     status[128];
    SpectatorDiscoveryEntry results[SPECTATOR_DISCOVERY_MAX_RESULTS];
};

struct SpectatorBufferedPaletteSlot {
    bool     metadata_valid;
    bool     has_custom_data;
    bool     bank_valid;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  flags;
    uint32_t payload_crc;
    uint16_t payload_size;
};

void SpectatorClient_Init();
void SpectatorClient_Shutdown();
void SpectatorClient_FrameUpdate();

bool SpectatorClient_BeginLanDiscovery();
bool SpectatorClient_StartConnect(const char* endpoint);
void SpectatorClient_Disconnect(const char* reason);
void SpectatorClient_SetRelayConfig(bool enabled, uint16_t listenPort);
void SpectatorClient_SetAutopunchRelay(bool enabled, const char* relayHost, uint16_t relayPort);

void SpectatorClient_SetFastForwardEnabled(bool enabled);
void SpectatorClient_SetHardSyncEnabled(bool enabled);
void SpectatorClient_SetPlaybackFrame(int32_t rb_frame);

SpectatorClientState SpectatorClient_GetState();
void SpectatorClient_GetSnapshot(SpectatorClientSnapshot* out);
void SpectatorClient_GetDiscoverySnapshot(SpectatorDiscoverySnapshot* out);
bool SpectatorClient_GetFrameInputs(int32_t rb_frame, uint16_t* outP1, uint16_t* outP2);
bool SpectatorClient_GetBufferedPaletteSlot(uint8_t game_slot, SpectatorBufferedPaletteSlot* out);
bool SpectatorClient_CopyBufferedPaletteBank(uint8_t game_slot, NetplayPaletteBank* out);

} // namespace Net
