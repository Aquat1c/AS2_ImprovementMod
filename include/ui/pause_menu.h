#pragma once

/**
 * Alice Senki 2 - Mod-owned in-match pause menu.
 *
 * Replaces the vanilla training pause menu (MODE_MATCH substate 4) by hooking
 * only its input handler (sub_4C8250) and its renderer (sub_4C8870). The native
 * substate handler sub_4CA120 keeps doing the scene redraw and the substate
 * plumbing, so every game type we do not own behaves exactly as before.
 *
 * The return protocol is the vanilla one:
 *   0   resume        1   character select        2   exit match        255 stay
 *
 * Layout follows the vanilla geometry exactly - full-screen dim, no panel, rows
 * at y = 64 + 32*i, label column x 64 - because the text is drawn at the vanilla
 * glyph size (25 px caps). Only the value column is widened, into space the
 * vanilla menu left empty.
 */

#include <stdbool.h>
#include <stdint.h>

bool PauseMenu_Install();
void PauseMenu_Uninstall();
bool PauseMenu_IsInstalled();
const char* PauseMenu_GetInstallError();

// True while the mod is driving the pause menu this frame. The practice runtime
// uses it to release its input overrides, which would otherwise drive the menu
// cursor through the dummy's own automation.
bool PauseMenu_IsActive();

void PauseMenu_Reset();
