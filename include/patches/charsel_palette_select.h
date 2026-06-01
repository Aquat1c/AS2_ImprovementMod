/**
 * Alice Senki 2 - Character Select Palette Flow
 *
 * Replaces the vanilla direct button-to-palette confirm path with a
 * two-step flow:
 *   1. pick a character
 *   2. scroll palettes left/right and confirm with any attack button
 *
 * The module is shared across offline modes and the netplay frontend.
 */

#pragma once

#include "net/protocol.h"

#include <stdint.h>

namespace Net {

bool CharSelPaletteSelect_Install();
void CharSelPaletteSelect_FrameUpdate();

void CharSelPaletteSelect_OnCharSelBegin(bool netplay, uint8_t local_game_slot);
void CharSelPaletteSelect_EndFrontend();
/// Clear per-character palette cursor cache when the netplay connection ends.
/// Rematch on the same connection keeps local cache; a new opponent/session does not.
/// Does not remove custom banks from disk.
void CharSelPaletteSelect_ResetNetplaySessionState(const char* reason);
void CharSelPaletteSelect_OnRemoteCatalog(const CharSelInputPayload* payload);
void CharSelPaletteSelect_OnLocalCatalogChanged();
void CharSelPaletteSelect_ClearExternalCustomHints();
void CharSelPaletteSelect_SetExternalCustomHint(uint8_t game_slot,
                                                uint8_t character_id,
                                                uint8_t base_palette,
                                                bool available);

bool CharSelPaletteSelect_IsCatalogReady();
bool CharSelPaletteSelect_IsSelectionLocked(uint8_t game_slot);
bool CharSelPaletteSelect_CanCancelSelection(uint8_t game_slot);
bool CharSelPaletteSelect_CancelSelection(uint8_t game_slot);
bool CharSelPaletteSelect_RequestRandomCharacter(uint8_t game_slot,
                                                uint8_t target_grid_index,
                                                const char* context);
bool CharSelPaletteSelect_IsRandomCharacterActive(uint8_t game_slot);
bool CharSelPaletteSelect_ForceSelectionLocked(uint8_t game_slot,
                                              uint8_t character_id,
                                              uint8_t base_palette,
                                              bool use_custom);
bool CharSelPaletteSelect_ShouldPreviewCustomBank(uint8_t game_slot,
                                                 uint8_t character_id,
                                                 uint8_t base_palette);
bool CharSelPaletteSelect_ShouldUseCustomBank(uint8_t game_slot,
                                              uint8_t character_id,
                                              uint8_t base_palette);

} // namespace Net
