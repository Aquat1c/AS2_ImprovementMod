/**
 * Alice Senki 2 - Character/Stage Select Frontend Actions
 *
 * Patch-owned frontend controls for selection screens:
 *   - D random character/stage selection
 *   - B stage cancel back to character select
 *
 * Netplay callers pass lockstep-confirmed inputs. Offline callers pass the
 * raw game input arrays after Input_Process has populated them.
 */

#pragma once

#include <stdint.h>

namespace Net {

bool CharSelSelectActions_Install();

uint8_t CharSelSelectActions_HandleNetplayCharacterRandomInput(uint16_t p1_just,
                                                               uint16_t p2_just,
                                                               uint16_t p1_input,
                                                               uint16_t p2_input);

bool CharSelSelectActions_HandleNetplayStageCancelInput(uint16_t merged_just);
bool CharSelSelectActions_HandleOfflineStageCancelInput(uint16_t merged_just);

bool CharSelSelectActions_HandleNetplayStageRandomInput(uint16_t merged_just,
                                                       uint16_t merged_input);
bool CharSelSelectActions_HandleOfflineStageRandomInput(uint16_t merged_just,
                                                       uint16_t merged_input);

bool CharSelSelectActions_AdvanceStageRandomScroll();
bool CharSelSelectActions_IsStageRandomScrollActive();
void CharSelSelectActions_ResetStageRandomScroll();

} // namespace Net
