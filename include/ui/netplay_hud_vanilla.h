#pragma once

// Draw netplay nickname HUD through the game's native render API (sub_4C05B0 path).
// Installed via match HUD hook during the game's render pass.

bool NetplayHudVanilla_InstallHooks();
void NetplayHudVanilla_RenderMatch(int game, int match);
void NetplayHudVanilla_RenderCharSel();
