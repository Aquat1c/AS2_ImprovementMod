/**
 * Alice Senki 2 - Local VS Continue-Screen Rematch
 *
 * The offline twin of Net::ContinueFlow. Same vanilla screen (Mode 9 sub 4,
 * handler sub_601BB0), same forcing, same YES/NO routing - but driven straight
 * from the two local pads instead of a lockstep stream.
 *
 * Deliberately a separate module rather than a mode inside ContinueFlow. That
 * one's determinism rules exist because there are two machines that must resolve
 * identically; threading a local path through them would put offline-only
 * branches inside the code that keeps netplay from desyncing. Nothing here is
 * reachable while a session is connected, and nothing in ContinueFlow calls
 * into this.
 *
 * Why it works at all: the mod's netplay already runs as GAMETYPE_VS_HUMAN, so
 * every game-side fact ContinueFlow established - the sub_6019F0 gametype 2/3
 * skip gate, the sub=8 decline route that dodges the 1920-frame GAME OVER
 * slide, the mode-7 rematch route - was proven under the exact gametype a local
 * versus match uses.
 */

#pragma once

#include <stdint.h>

namespace LocalRematch {

void Init();
void Shutdown();
void Reset(const char* reason);

// On unless the ini says otherwise, and the choice persists to
// as2_rollback_settings.ini ([ModSettings] local_rematch). It was off by
// default and runtime-only at first; both of those made a feature that was
// explicitly asked for look like it simply did not work.
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
