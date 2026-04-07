/**
 * Alice Senki 2 - Netplay Menu UI
 *
 * Renders the in-game netplay menu using the game's native render primitives.
 * Separate from the controller logic so rendering can be swapped or extended.
 */

#pragma once

#include "net/netplay_menu_state.h"

namespace NetMenuUI {

/// Render the in-game netplay menu overlay using game-native render functions.
/// Called from the menu controller's FrameUpdate when the menu is visible.
void Render(const NetMenu::MenuSnapshot* snap);

} // namespace NetMenuUI
