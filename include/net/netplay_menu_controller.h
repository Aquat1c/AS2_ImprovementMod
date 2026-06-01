/**
 * Alice Senki 2 - Netplay Menu Controller
 *
 * Owns the custom netplay menu state machine that replaces vanilla MODE_LOBBY.
 * Handles navigation, session control, launch handoff, and disconnect recovery.
 * Sits on top of the Net::Session_* layer and ModeOwnership hooks.
 */

#pragma once

#include "net/netplay_menu_state.h"

namespace NetMenu {

// ============================================================================
// Lifecycle
// ============================================================================

/// Cache the raw autoconnect config file contents from disk immediately.
/// Call this as early as possible (e.g. ModInit) before the harness overwrites
/// the config file with the other role's settings.
void CacheAutoConnectFile();

/// Initialize the menu controller. Call after ModeOwnership::Install().
void Init();

/// Shut down the menu controller.
void Shutdown();

// ============================================================================
// Per-Frame Update
// ============================================================================

/// Called every frame. Pumps session, handles input, manages state transitions.
void FrameUpdate();

// ============================================================================
// Callbacks from ModeOwnership hooks
// ============================================================================

/// Called when the user selects "Network" from the main menu.
void HandleNetworkSelected();

/// Called when a disconnection or error forces return to the menu.
void HandleDisconnection(const char* reason);

/// Called when a match ends and the game returns to post-match state.
void HandlePostMatchReturn();

// ============================================================================
// Queries
// ============================================================================

/// Is the custom menu currently visible and active?
bool IsMenuActive();

/// Does the menu currently consume all game input?
bool ConsumesGameInput();

/// Hide the menu immediately because an external launch path is taking over.
void HideForLaunch(const char* reason);

/// Reopen the custom menu after an external launch path unwinds.
void ShowMenuAfterExternalLaunch(const char* reason);

/// Get a read-only snapshot for UI rendering and diagnostics.
void GetSnapshot(MenuSnapshot* out);

/// Render the menu in the game's render pass (called from Hook_MainMenuStateMachine).
void RenderFrame();

/// After the menu closes, fade the vanilla main menu back in (drawn over the
/// vanilla menu in the game's render pass). No-op when no return fade is pending.
/// Called from Hook_MainMenuStateMachine's vanilla (menu-inactive) path.
void RenderMainMenuReturnFade();

} // namespace NetMenu
