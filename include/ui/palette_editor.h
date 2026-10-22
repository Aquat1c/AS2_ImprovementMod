/**
 * Alice Senki 2 - Palette Editor UI
 */

#pragma once

void PaletteEditor_Init();
void PaletteEditor_Render();
#include <stdint.h>
bool PaletteEditor_OpenNative();
bool PaletteEditor_UpdateNative(uint16_t pressed);
void PaletteEditor_RenderNative();
void PaletteEditor_CloseNative();
