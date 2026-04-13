/**
 * Alice Senki 2 - Spectator Playback
 *
 * Owns local confirmed-input playback for spectator streams.
 */

#pragma once

#include <stdint.h>

namespace Net {

struct NetplayPaletteBank;

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

enum class SpectatorDispatchAction : uint8_t {
    Unhandled = 0,
    BreakLoop,
    ProduceFrame,
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
    float    manual_catchup_scale;
    char     state_label[32];
    char     status[128];
};

void SpectatorPlayback_Init();
void SpectatorPlayback_Shutdown();
void SpectatorPlayback_FrameUpdate();
SpectatorDispatchAction SpectatorPlayback_GetDispatcherFrame(uint16_t* outP1,
                                                            uint16_t* outP2,
                                                            int32_t* outRbFrame);
bool SpectatorPlayback_CopyPaletteOverrideBank(uint8_t game_slot,
                                               uint8_t character_id,
                                               uint8_t base_palette,
                                               NetplayPaletteBank* out);

void SpectatorPlayback_GetSnapshot(SpectatorPlaybackSnapshot* out);

} // namespace Net
