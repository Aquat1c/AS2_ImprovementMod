/**
 * Alice Senki 2 - Win Screen Synchronization
 *
 * Handles the A/C confirm gate at Mode 9 (WIN_SCREEN) substate 3 for
 * VS Human netplay.  Both peers must confirm before the game advances
 * back to character select.
 *
 * Flow:
 *   Mode 8 → Mode 9 (win screen) → sub 0→1→2→3 (auto-advance)
 *   Sub 3: waits for A/C just-pressed from EITHER player
 *   For netplay: local player presses A/C → send WinScreenConfirm
 *                receive remote WinScreenConfirm → inject A/C
 *                both confirmed → allow game to advance
 *   Sub 3→8→36(0x24) → Game_ChangeMode(6,1) = MODE_CHARSEL
 *
 * Design:
 *   Simple one-shot signal exchange.  No lockstep ring buffer needed.
 *   The game only needs a single A/C just-pressed frame to advance.
 *   We suppress local A/C until both peers have confirmed, then inject
 *   it on the same frame for both sides.
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Lifecycle
// ============================================================================

void WinScreenSync_Init();
void WinScreenSync_Shutdown();

// ============================================================================
// Control
// ============================================================================

/// Begin win screen sync.  Called when entering Mode 9.
void WinScreenSync_Begin();

/// Abort / reset.  Called on disconnect or unexpected mode change.
void WinScreenSync_Abort();

// ============================================================================
// Per-Frame (called from match lifecycle or online wiring)
// ============================================================================

/// Drive win screen sync each frame.
/// Returns true if the sync is actively suppressing/injecting input.
bool WinScreenSync_FrameUpdate();

// ============================================================================
// Queries
// ============================================================================

/// Is win screen sync actively running?
bool WinScreenSync_IsActive();

/// Has the local player confirmed (pressed A/C)?
bool WinScreenSync_LocalConfirmed();

/// Has the remote player confirmed?
bool WinScreenSync_RemoteConfirmed();

/// Are both players confirmed and ready to advance?
bool WinScreenSync_BothConfirmed();

// ============================================================================
// Packet Handler
// ============================================================================

/// Handle incoming WinScreenConfirm packet.
void WinScreenSync_OnRemoteConfirm();

} // namespace Net
