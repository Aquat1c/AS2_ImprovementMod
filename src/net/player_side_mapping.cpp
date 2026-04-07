/**
 * Alice Senki 2 - Player Side Mapping Implementation
 *
 * Maps network role (host/client) to game-side slots (P1/P2) and
 * routes local primary SDL input to the correct slot.
 *
 * Key invariant: both host and client always read from local SDL P1
 * bindings (InputSystem_GetInput(0)). The mapping layer translates
 * this to the correct game-side slot for rollback input injection.
 */

#include "net/player_side_mapping.h"
#include "input/input_system.h"
#include "ui/log_window.h"

namespace Net {

// ============================================================================
// Internal State
// ============================================================================

static bool s_assigned       = false;
static int  s_localGameSlot  = -1;   // 0 = P1, 1 = P2
static int  s_remoteGameSlot = -1;

// ============================================================================
// Assignment
// ============================================================================

void PlayerMapping_SetAssignment(int local_game_slot) {
    if (local_game_slot < 0 || local_game_slot > 1) {
        LOG_ERROR("[PlayerMapping] Invalid local_game_slot: %d", local_game_slot);
        return;
    }

    s_localGameSlot  = local_game_slot;
    s_remoteGameSlot = 1 - local_game_slot;
    s_assigned       = true;

    LOG_INFO("[PlayerMapping] Assignment: local=P%d remote=P%d",
        s_localGameSlot + 1, s_remoteGameSlot + 1);
}

void PlayerMapping_Clear() {
    s_assigned       = false;
    s_localGameSlot  = -1;
    s_remoteGameSlot = -1;
}

// ============================================================================
// Queries
// ============================================================================

int PlayerMapping_GetLocalGameSlot() {
    return s_localGameSlot;
}

int PlayerMapping_GetRemoteGameSlot() {
    return s_remoteGameSlot;
}

bool PlayerMapping_IsHost() {
    return s_assigned && s_localGameSlot == 0;
}

bool PlayerMapping_IsAssigned() {
    return s_assigned;
}

// ============================================================================
// Input Routing
// ============================================================================

uint16_t PlayerMapping_ReadLocalInput() {
    // ALWAYS read from SDL player 0 (local P1 bindings).
    // Both host and client use their local P1 controller.
    // The game slot assignment only affects where the input is
    // injected into the game state, not which physical device is read.
    return InputSystem_GetInput(0);
}

// ============================================================================
// Role Helpers
// ============================================================================

int PlayerMapping_DeriveFromRole(uint8_t host_side, bool is_host) {
    // host_side == 0: host is game P1, client is game P2
    // host_side == 1: host is game P2, client is game P1
    int local_slot;
    if (is_host) {
        local_slot = (host_side == 0) ? 0 : 1;
    } else {
        local_slot = (host_side == 0) ? 1 : 0;
    }

    PlayerMapping_SetAssignment(local_slot);
    return local_slot;
}

} // namespace Net
