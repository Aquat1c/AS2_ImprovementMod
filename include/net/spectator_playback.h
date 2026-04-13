/**
 * Alice Senki 2 - Spectator Playback
 *
 * Owns local confirmed-input playback for spectator streams.
 */

#pragma once

#include <stdint.h>

namespace Net {

enum class SpectatorPlaybackState : uint8_t {
    Disconnected = 0,
    Connecting,
    ConnectedWaitingMetadata,
    ConnectedNoActiveMatch,
    WaitingFullArchive,
    ReadyToBootstrap,
    BootstrappingFrontend,
    WaitingInteractiveStart,
    Buffering,
    CatchingUp,
    Live,
    EndOfMatch,
    WaitingNextMatch,
    PlaybackError,
};

const char* SpectatorPlaybackStateName(SpectatorPlaybackState state);

struct SpectatorPlaybackSnapshot {
    bool     active;
    bool     gameplay_owned;
    bool     bootstrapping;
    bool     playing;
    bool     waiting_for_frame;
    bool     catchup_active;
    bool     error;
    SpectatorPlaybackState state;
    uint32_t match_id;
    uint32_t match_ordinal;
    uint32_t config_crc;
    uint32_t session_seed;
    int32_t  local_playback_rb_frame;
    int32_t  confirmed_edge_rb_frame;
    int32_t  live_edge_rb_frame;
    float    tick_scale_target;
    char     state_label[32];
    char     status[128];
};

void SpectatorPlayback_Init();
void SpectatorPlayback_Shutdown();
void SpectatorPlayback_FrameUpdate();

void SpectatorPlayback_GetSnapshot(SpectatorPlaybackSnapshot* out);

} // namespace Net
