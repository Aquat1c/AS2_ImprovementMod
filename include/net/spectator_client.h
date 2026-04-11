/**
 * Alice Senki 2 - Spectator Sidecar Client
 *
 * Client-side archived-input buffer and synthetic playback cursor used for
 * spectator-mode scaffolding.
 */

#pragma once

#include <stdint.h>

namespace Net {

enum class SpectatorClientState : uint8_t {
    Idle = 0,
    Connecting,
    Handshaking,
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
    uint32_t match_id;
    bool     match_active;
    int32_t  buffered_start_rb_frame;
    int32_t  buffered_end_rb_frame;
    int32_t  server_confirmed_rb_frame;
    int32_t  server_live_rb_frame;
    int32_t  playback_rb_frame;
    uint32_t buffered_frame_count;
    bool     should_fast_forward;
    bool     needs_hard_sync;
    char     status[128];
    char     error[128];
};

void SpectatorClient_Init();
void SpectatorClient_Shutdown();
void SpectatorClient_FrameUpdate();

bool SpectatorClient_StartConnect(const char* endpoint);
void SpectatorClient_Disconnect(const char* reason);

void SpectatorClient_SetFastForwardEnabled(bool enabled);
void SpectatorClient_SetHardSyncEnabled(bool enabled);

SpectatorClientState SpectatorClient_GetState();
void SpectatorClient_GetSnapshot(SpectatorClientSnapshot* out);

} // namespace Net