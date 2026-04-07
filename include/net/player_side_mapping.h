/**
 * Alice Senki 2 - Player Side Mapping
 *
 * Owns the mapping between network role (host/client) and game-side
 * player slots (P1/P2), plus the routing of local primary SDL input
 * to the correct game slot.
 *
 * Netplay semantics:
 *   - Host controls game P1 using local P1 SDL controls
 *   - Client controls game P2 using local P1 SDL controls
 *
 * Both players always use their local P1 SDL bindings. The mapping
 * layer routes that to the correct game-side slot.
 *
 * Consumers:
 *   - rollback_session.cpp (input collection)
 *   - online_wiring.cpp (session start)
 *   - pregame_sync.cpp (side assignment)
 *   - match_bootstrap.cpp (config exchange)
 *   - netplay_menu_controller.cpp (launch setup)
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Side Assignment
// ============================================================================

/// Establish the local/remote game-side mapping.
/// local_game_slot: 0 = this machine controls game P1, 1 = game P2.
/// Called once per match when side assignment is locked.
void PlayerMapping_SetAssignment(int local_game_slot);

/// Clear the current assignment (between matches).
void PlayerMapping_Clear();

// ============================================================================
// Queries
// ============================================================================

/// Which game slot does this machine control? (0 = P1, 1 = P2)
/// Returns -1 if no assignment is active.
int PlayerMapping_GetLocalGameSlot();

/// Which game slot does the remote peer control? (0 = P1, 1 = P2)
/// Returns -1 if no assignment is active.
int PlayerMapping_GetRemoteGameSlot();

/// Is the local machine the host (game P1)?
bool PlayerMapping_IsHost();

/// Is there an active assignment?
bool PlayerMapping_IsAssigned();

// ============================================================================
// Input Routing
// ============================================================================

/// Read the local player's input using P1 SDL bindings, regardless of
/// which game slot they are assigned to. This is the correct function
/// to call for netplay input collection.
///
/// Both host and client always use local P1 SDL controls — this function
/// handles the mapping so the caller doesn't need to care.
uint16_t PlayerMapping_ReadLocalInput();

// ============================================================================
// Role Helpers
// ============================================================================

/// Derive local/remote game slots from host_side and network role.
/// host_side: 0 = host is P1, 1 = host is P2.
/// is_host: true for host, false for client.
/// Returns the local game slot (0 or 1).
int PlayerMapping_DeriveFromRole(uint8_t host_side, bool is_host);

} // namespace Net
