/**
 * Alice Senki 2 - Pause Handler for Netplay
 *
 * Handles the pause menu during online VS Human matches:
 *   - Allows pause (START) during netplay (removes the START suppression)
 *   - When user selects "Quit Match" from the pause menu:
 *     sends a PauseQuit signal to the remote peer and cancels the session
 *   - When remote sends PauseQuit: cleanly tears down the connection
 *
 * Pause menu (sub_4C8250):
 *   Return 0 = resume
 *   Return 1 = quit match (→ route CHARSEL)
 *   Return 2 = quit to menu (→ route MENU)
 *
 * For netplay, quit-to-menu should also cleanly cancel the session rather
 * than leaving a dangling connection.
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Lifecycle
// ============================================================================

void PauseHandler_Init();
void PauseHandler_Shutdown();

// ============================================================================
// Per-Frame (called from online_wiring or match lifecycle)
// ============================================================================

/// Drive pause handler each frame.
/// Detects pause-quit transitions and triggers session cleanup.
void PauseHandler_FrameUpdate();

// ============================================================================
// Events
// ============================================================================

/// Called when entering PauseActive lifecycle phase
void PauseHandler_OnPauseEnter();

/// Called when leaving PauseActive lifecycle phase
void PauseHandler_OnPauseExit(bool wasQuit);

// ============================================================================
// Packet Handler
// ============================================================================

/// Handle incoming PauseQuit packet from remote peer.
void PauseHandler_OnRemotePauseQuit();

// ============================================================================
// Queries
// ============================================================================

/// Is the pause handler currently tracking a pause?
bool PauseHandler_IsPauseTracked();

/// Did the user (or remote) quit from pause?
bool PauseHandler_WasQuit();

} // namespace Net
