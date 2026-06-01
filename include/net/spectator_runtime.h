/**
 * Alice Senki 2 - Spectator Runtime
 *
 * High-level spectator archive and sidecar broadcast runtime.
 */

#pragma once

#include "net/locked_match_config.h"

#include <stdint.h>

namespace Net {

struct SpectatorRuntimeSnapshot {
    bool     initialized;
    bool     enabled;
    bool     match_active;
    bool     server_active;
    uint16_t listen_port;
    uint32_t match_id;
    int      connected_spectators;
    int32_t  archive_start_rb_frame;
    int32_t  confirmed_rb_frame;
    int32_t  live_rb_frame;
    int32_t  oldest_requested_rb_frame;
    char     status[128];
};

void SpectatorRuntime_Init();
void SpectatorRuntime_Shutdown();
void SpectatorRuntime_FrameUpdate();

void SpectatorRuntime_SetEnabled(bool enabled);
bool SpectatorRuntime_GetEnabled();
void SpectatorRuntime_SetListenPort(uint16_t port);
uint16_t SpectatorRuntime_GetListenPort();

/// Called when both players' selection is committed (chars + stage locked), before
/// loading starts. Sends PreMatchState to connected spectators so they can start
/// charsel bootstrap in parallel with the players' loading screen, eliminating
/// the forced speed-up at the beginning of each game.
void SpectatorRuntime_OnSelectionCommitted(const LockedMatchConfig* config);
void SpectatorRuntime_OnMatchBegin(const LockedMatchConfig* config);
void SpectatorRuntime_OnRollbackStarted(int32_t frame_origin_abs);
void SpectatorRuntime_OnGameplayFrame(int32_t rb_frame,
                                      int32_t game_abs_frame,
                                      uint16_t p1_input,
                                      uint16_t p2_input,
                                      bool rolling_back,
                                      int32_t confirmed_rb_frame);
void SpectatorRuntime_OnMatchEnd(const char* reason);
void SpectatorRuntime_OnDisconnect(const char* reason);

void SpectatorRuntime_GetSnapshot(SpectatorRuntimeSnapshot* out);

} // namespace Net