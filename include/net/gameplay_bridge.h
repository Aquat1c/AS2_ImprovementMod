/**
 * Alice Senki 2 - Gameplay Bridge
 *
 * The gameplay-runtime integration layer for the mod-owned rollback path.
 * This bridge centralizes the contract between online session management
 * and the rollback gameplay subsystems:
 *
 *   - Rollback session (rollback_session) — deterministic gameplay loop
 *   - Player side mapping (player_side_mapping) — local/remote → P1/P2
 *   - Delay policy (delay_policy) — input delay negotiation/consumption
 *
 * All gameplay session lifecycle operations (start, stop, per-frame update,
 * remote input submission, delay policy consumption) flow through this bridge.
 * The bridge owns the per-frame entry point: mod_main calls
 * GameplayBridge_FrameUpdate(), which internally drives the rollback session.
 *
 * GekkoNet library availability is checked at init as a future-integration
 * readiness probe. GekkoNet does NOT currently participate in gameplay runtime.
 * All rollback logic is mod-owned via RollbackSession.
 */

#pragma once

#include "rollback/rollback_session.h"
#include "net/player_side_mapping.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize the gameplay bridge. Checks GekkoNet library availability
/// (future integration readiness — not used for gameplay).
/// Call once at mod startup.
bool GameplayBridge_Init();

/// Shut down the gameplay bridge.
void GameplayBridge_Shutdown();

/// Is the GekkoNet library available? (future integration readiness only)
bool GameplayBridge_IsGekkoNetAvailable();

/// Get the pinned GekkoNet library version string.
const char* GameplayBridge_GetGekkoNetVersion();

// ============================================================================
// Bridge Status
// ============================================================================

struct GameplayBridgeSnapshot {
    bool     initialized;
    bool     gekkonet_available;    // Future integration readiness only
    bool     session_active;
    bool     timesync_hard_freeze;
    int      local_game_slot;       // From PlayerMapping
    int      remote_game_slot;
    int      active_delay;          // From DelayPolicy
    int      rollback_budget;
    int32_t  current_frame;         // From RollbackSession
    int      predicted_frames;
    int      hard_wait_frames;
    float    runtime_tick_scale;
    char     status[128];
    char     gekkonet_version[48];  // Pinned version for future use
};

void GameplayBridge_GetSnapshot(GameplayBridgeSnapshot* out);

// ============================================================================
// Gameplay Session Control
// ============================================================================

/// Start the gameplay session. Configures player mapping and
/// initializes the mod-owned rollback session with the given config.
/// Returns true on success.
bool GameplayBridge_StartSession(const Rollback::RollbackSessionConfig& config);

/// End the gameplay session. Tears down rollback session and clears
/// player mapping.
void GameplayBridge_EndSession();

/// Is a gameplay session currently active?
bool GameplayBridge_IsSessionActive();

// ============================================================================
// Per-Frame Entry Point
// ============================================================================

/// The single per-frame entry point for gameplay runtime.
/// Called from mod_main when session is active and gameplay is running.
/// Internally drives: RollbackSession_FrameUpdate + delay policy consumption.
void GameplayBridge_FrameUpdate();

// ============================================================================
// Input Routing
// ============================================================================

/// Read the local player's input using P1 SDL bindings.
uint16_t GameplayBridge_ReadLocalInput();

/// Submit remote input for a specific frame.
/// Routes through to the rollback session's input timeline.
void GameplayBridge_SubmitRemoteInput(int32_t frame, uint16_t input);

} // namespace Net
