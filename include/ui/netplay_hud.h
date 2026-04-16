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
