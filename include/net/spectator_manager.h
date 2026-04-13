/**
 * Alice Senki 2 - Spectator Sidecar Server Transport
 *
 * Owns the dedicated ENet host used for spectator clients. This is kept
 * separate from SessionManager because the gameplay session transport is
 * strictly single-peer.
 */

#pragma once

#include "net/spectator_protocol.h"

#include <stdint.h>

namespace Net {

struct SpectatorPeerSnapshot {
    uintptr_t peer_id;
    bool      connected;
    bool      handshake_complete;
    bool      needs_full_sync;
    int32_t   next_rb_frame;
    int32_t   last_playback_rb_frame;
    bool      fast_forward_requested;
    bool      hard_sync_requested;
    uint16_t  advertised_listen_port;
    char      nickname[64];
};

struct SpectatorManagerSnapshot {
    bool     initialized;
    bool     enabled;
    bool     server_active;
    uint16_t listen_port;
    uint32_t active_match_id;
    int      connected_spectators;
    int32_t  oldest_requested_rb_frame;
    char     status[128];
};

void SpectatorManager_Init();
void SpectatorManager_Shutdown();

void SpectatorManager_SetEnabled(bool enabled);
bool SpectatorManager_SetListenPort(uint16_t port);
void SpectatorManager_SetRedirectEndpoint(const char* endpoint);

void SpectatorManager_BeginMatch(uint32_t match_id, uint32_t match_ordinal);
void SpectatorManager_EndMatch(const char* reason);
void SpectatorManager_FrameUpdate();

bool SpectatorManager_IsServerActive();
int SpectatorManager_GetConnectedCount();
int SpectatorManager_GetPeerSnapshots(SpectatorPeerSnapshot* out, int maxPeers);
int32_t SpectatorManager_GetOldestRequestedFrame();

bool SpectatorManager_ClearPeerFullSync(uintptr_t peer_id);
bool SpectatorManager_SetPeerNextFrame(uintptr_t peer_id, int32_t next_rb_frame);

bool SpectatorManager_SendMatchState(uintptr_t peer_id, const Spectator::MatchStatePayload* payload);
bool SpectatorManager_SendPaletteState(uintptr_t peer_id, const Spectator::PaletteStatePayload* payload);
bool SpectatorManager_SendPaletteData(uintptr_t peer_id, const Spectator::PaletteDataPayload* payload);
bool SpectatorManager_SendFrameBatch(uintptr_t peer_id, const Spectator::FrameBatchPayload* payload);
bool SpectatorManager_SendHeartbeat(uintptr_t peer_id, const Spectator::HeartbeatPayload* payload);
void SpectatorManager_BroadcastHeartbeat(const Spectator::HeartbeatPayload* payload);
void SpectatorManager_BroadcastDisconnect(const Spectator::DisconnectPayload* payload);

void SpectatorManager_GetSnapshot(SpectatorManagerSnapshot* out);

} // namespace Net