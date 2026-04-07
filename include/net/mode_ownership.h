/**
 * Alice Senki 2 - Mode Ownership Layer
 *
 * Hooks into game mode transitions and vanilla netplay socket lifecycle to
 * ensure the mod owns all networking flow. Prevents vanilla netplay from
 * interfering during mod-owned sessions.
 *
 * Hook points:
 *   - MainMenuStateMachine (0x5FB200): intercept vanilla "Network" selection
 *   - SetGameMode (0x5D2EB0): intercept MODE_LOBBY transitions
 *   - NetInitHost (0x5FB9E0): block vanilla host socket init
 *   - NetInitClient (0x5FBBC0): block vanilla client socket init
 *   - NetCloseHost (0x5FBBA0): block vanilla host socket close
 *   - NetCloseClient (0x5FBCE0): block vanilla client socket close
 */

#pragma once

#include <stdint.h>

namespace ModeOwnership {

// ============================================================================
// Lifecycle
// ============================================================================

/// Install all mode/socket ownership hooks. Call once during init.
bool Install();

/// Remove all hooks. Call during shutdown.
void Remove();

/// Per-frame update: sanitize game type, handle pending menu restore.
void FrameUpdate();

// ============================================================================
// Vanilla Netplay Suppression
// ============================================================================

/// Clear vanilla netplay role/connected flags.
void ClearVanillaNetplayFlags();

/// Check if vanilla socket lifecycle should be blocked.
bool ShouldBlockVanillaSocketLifecycle();

/// Sanitize game type to prevent vanilla netplay from taking over.
/// Called during frame update and after mode transitions.
void SanitizeOwnedGameType(uint32_t mode, bool sessionActive, const char* reason);

// ============================================================================
// Menu Restore
// ============================================================================

/// Safely transition the game to MODE_MENU, waiting for asset load.
/// If already in MODE_MENU, forces substate 3 directly.
void EnterCustomMenuContext();

/// Direct substate/type restoration when already in MODE_MENU.
void RestoreMainMenuContext();

/// Whether a menu restore is pending (waiting for sub=0 to load assets).
bool IsPendingMenuRestore();

/// Set the pending menu restore flag.
void SetPendingMenuRestore(bool pending);

// ============================================================================
// SetGameMode passthrough
// ============================================================================

/// Call the original (or vanilla) SetGameMode function.
int CallOriginalSetGameMode(int mode, char fade);

// ============================================================================
// Game Type Policy
// ============================================================================

/// Get a safe (non-netplay) game type for the menu context.
uint32_t GetSafeMenuGameType();

/// Track vanilla game type changes for restore purposes.
void TrackGameType(uint32_t type);

/// Get the last stable (non-netplay) game type.
uint32_t GetLastStableGameType();

// ============================================================================
// CharSel Field Reset
// ============================================================================

/// Reset all CharSel-related fields to safe defaults.
void ResetCharSelFields();

} // namespace ModeOwnership
