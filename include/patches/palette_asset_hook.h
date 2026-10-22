/**
 * Alice Senki 2 - Palette Asset Hook
 *
 * Hooks character asset loads so custom palette banks can be captured and
 * applied without modifying the original game data files.
 */

#pragma once

bool PaletteAssetHook_Install();
void PaletteAssetHook_FrameUpdate();
void PaletteAssetHook_Shutdown();
// Resolve on each draw: palette reloads replace the native handles.
bool PaletteAssetHook_GetPreview(int gameSlot, int index, int* handle, int* width, int* height, int* count);
