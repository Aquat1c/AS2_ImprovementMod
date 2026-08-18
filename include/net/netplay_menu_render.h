#pragma once

#include <stdint.h>

// Drawing primitives shared with the game settings menu. These wrap the game's
// own renderer, so a screen built on them matches the native look.
namespace NetMenu {

void RenderMenuRow(int y, const char* label, const char* value,
                   bool selected, bool enabled, uint8_t alpha);
void RenderMenuInfoLine(int y, const char* label, const char* value, uint8_t alpha);
int  MenuRowStep();
// Text is drawn through the overlay, which does not see the game's blend state,
// so screens that fade must publish their alpha here.
void MenuSetTextAlpha(uint8_t alpha);

void MenuSetBlend(int mode, uint8_t alpha);
void MenuFillRect(int l, int t, int r, int b, uint8_t cr, uint8_t cg, uint8_t cb);
void MenuDrawText(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* text);
// size 0 keeps the default; the settings screens use larger rows than netplay.
void MenuDrawTextSized(int x, int y, uint8_t r, uint8_t g, uint8_t b, float size, const char* text);

} // namespace NetMenu
