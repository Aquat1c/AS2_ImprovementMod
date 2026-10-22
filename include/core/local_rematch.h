/**
 * Alice Senki 2 - Local VS Continue-Screen Rematch
 *
 * The offline twin of Net::ContinueFlow. Same vanilla screen (Mode 9 sub 4,
 * handler sub_601BB0), same forcing, same YES/NO routing - but driven straight
 * from the two local pads instead of a lockstep stream.
 *
 * Nothing here is reachable while a session is connected, and nothing in
 * ContinueFlow calls into this, so no offline-only branch can enter the
 * netplay determinism rules.
 *
 * The mod's netplay already runs as GAMETYPE_VS_HUMAN, so every game-side
 * fact ContinueFlow established - the sub_6019F0 gametype 2/3 skip gate, the
 * sub=8 decline route that dodges the 1920-frame GAME OVER slide, the mode-7
 * rematch route - applies under the exact gametype a local versus match uses.
 */

#pragma once

#include <stdint.h>

namespace LocalRematch {

void Init();
void Shutdown();
void Reset(const char* reason);

// On unless the ini says otherwise; the choice persists to
// as2_rollback_settings.ini ([ModSettings] local_rematch).
bool IsEnabled();
void SetEnabled(bool enabled);

/// One step per frame, from ModOnFrame. Reads both pads directly.
void FrameUpdate();

/// True while the prompt owns Mode 9 sub 4.
bool IsPromptActive();

/// Rematch resolved - mode_ownership redirects WinScreen->CharSel to
/// MODE_PREMATCH_INTRO, which re-reads the live charsel globals.
bool IsRematchLatched();
void ConsumeRematchLatch();

} // namespace LocalRematch
