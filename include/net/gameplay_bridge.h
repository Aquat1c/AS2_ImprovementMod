/**
 * Alice Senki 2 - Gameplay Bridge (GekkoNet-driven)
 *
 * Integration layer between session management and GekkoNet rollback.
 * Owns:
 *   - Session lifecycle (start/stop)
 *   - Player side mapping
 *   - Diagnostics snapshot
 *
 * GekkoNet is now the authoritative rollback engine. The per-frame
 * simulation loop is driven by the input dispatcher hook via
 * RollbackSession_BeginFrame/ProcessNextEvent, NOT by this bridge.
 *
 * The bridge provides FrameUpdate for diagnostic updates only.
 */

#pragma once

#include "rollback/rollback_session.h"
#include "net/player_side_mapping.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize the gameplay bridge. Call once at mod startup.
bool GameplayBridge_Init();

/// Shut down the gameplay bridge.
void GameplayBridge_Shutdown();

// ============================================================================
// Bridge Status
// ============================================================================

struct GameplayBridgeSnapshot {
    bool     initialized;
    bool     session_active;
    int      local_game_slot;
    int      remote_game_slot;
    int      active_delay;
    int      rollback_budget;
    int32_t  rb_frame_current;
    float    frames_ahead;
    float    link_avg_ping;
    float    link_jitter;
    char     status[128];
};

void GameplayBridge_GetSnapshot(GameplayBridgeSnapshot* out);

// ============================================================================
// Gameplay Session Control
// ============================================================================

/// Start a GekkoNet rollback session. Configures player mapping and
/// initializes the GekkoNet session with the given config.
bool GameplayBridge_StartSession(const Rollback::RollbackSessionConfig& config);

/// End the gameplay session. Destroys GekkoNet session, clears mapping.
void GameplayBridge_EndSession();

/// Is a gameplay session currently active?
bool GameplayBridge_IsSessionActive();

// ============================================================================
// Per-Frame (diagnostic/lightweight updates only)
// ============================================================================

/// Lightweight per-frame update for diagnostics.
/// The actual simulation is driven by the input dispatcher hook.
void GameplayBridge_FrameUpdate();

// ============================================================================
// Input Routing
// ============================================================================

/// Read the local player's input using P1 SDL bindings.
uint16_t GameplayBridge_ReadLocalInput();

} // namespace Net
