/**
 * Netplay HUD — ImGui-based overlay for online match stats.
 *
 * Renders player nicknames (top), connection stats (bottom)
 * using ImGui foreground draw list on the 640x480 backbuffer.
 *
 * Call NetplayHud_Render() from ModOnPresent every frame.
 */
#pragma once

void NetplayHud_Render();
bool NetplayHud_HasVisibleHud();

// Hides everything the mod paints over a netplay match — nicknames, the
// connection stats bar, the status line, and the vanilla-font name draw. The
// same hotkey also hides the replay playback overlay, which is why it is named
// for both. The game's own HUD is untouched. Runtime only, survives matches.
void NetplayHud_SetHidden(bool hidden);
bool NetplayHud_IsHidden();
void NetplayHud_ToggleHidden();
