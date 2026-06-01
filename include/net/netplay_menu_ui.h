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

/// Draw a full-screen black overlay at the given alpha (0 = clear, 255 = opaque).
/// Used for the main-menu return fade-in, drawn over the vanilla menu after close.
void RenderFullscreenFade(uint8_t blackAlpha);

} // namespace NetMenuUI
