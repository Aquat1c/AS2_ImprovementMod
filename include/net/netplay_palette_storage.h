/**
 * Alice Senki 2 - Netplay Palette Storage
 *
 * Persistent local storage for custom 1024-byte palette banks.
 */

#pragma once

#include "net/netplay_palette_runtime.h"

#include <stdint.h>

namespace Net {

void NetplayPaletteStorage_Init();
void NetplayPaletteStorage_Shutdown();

bool NetplayPaletteStorage_GetBank(uint8_t character_id,
                                   uint8_t base_palette,
                                   NetplayPaletteBank* out);
bool NetplayPaletteStorage_SaveBank(const NetplayPaletteBank* bank);
bool NetplayPaletteStorage_DeleteBank(uint8_t character_id, uint8_t base_palette);

} // namespace Net