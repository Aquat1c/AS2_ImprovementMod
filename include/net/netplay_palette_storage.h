/**
 * Alice Senki 2 - Netplay Palette Storage
 *
 * Persistent local storage for custom 1024-byte palette banks.
 */

#pragma once

#include "net/netplay_palette_runtime.h"

#include <stdint.h>

namespace Net {

struct NetplayPalettePresetInfo {
    char name[64];
};

void NetplayPaletteStorage_Init();
void NetplayPaletteStorage_Shutdown();

bool NetplayPaletteStorage_GetBank(uint8_t character_id,
                                   uint8_t base_palette,
                                   NetplayPaletteBank* out);
bool NetplayPaletteStorage_SaveBank(const NetplayPaletteBank* bank);
bool NetplayPaletteStorage_DeleteBank(uint8_t character_id, uint8_t base_palette);

int  NetplayPaletteStorage_ListPresets(uint8_t character_id,
                                       uint8_t base_palette,
                                       NetplayPalettePresetInfo* out,
                                       int capacity);
bool NetplayPaletteStorage_HasPreset(uint8_t character_id,
                                     uint8_t base_palette,
                                     const char* preset_name);
bool NetplayPaletteStorage_LoadPreset(uint8_t character_id,
                                      uint8_t base_palette,
                                      const char* preset_name,
                                      NetplayPaletteBank* out);
bool NetplayPaletteStorage_SavePreset(const NetplayPaletteBank* bank, const char* preset_name);
bool NetplayPaletteStorage_DeletePreset(uint8_t character_id,
                                        uint8_t base_palette,
                                        const char* preset_name);

} // namespace Net