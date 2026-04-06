/**
 * Alice Senki 2 - Audio Hooks
 *
 * Hooks SE_Play (sub_4C3C00 at ADDR_SE_PLAY = 0x4C3C00).
 * This function plays sound effects by toggling SE_ChannelActive[],
 * rotating SE_ChannelIndex, and calling Audio_Stop + Audio_Play_Wrapper.
 * These are purely audio-side state — no simulation writes.
 *
 * During rollback resimulation, the hook suppresses SE_Play calls entirely
 * (returns 0 early) so re-played frames don't produce duplicate sound
 * effects. On normal frames, all calls pass through to the original.
 *
 * NOTE: The old ADDR_SOUND_TRIGGER (0x4C3ED0) is actually Effect_SetParams1,
 * an entity state writer that MUST NOT be suppressed. That function is no
 * longer hooked.
 *
 * Hook target: sub_4C3C00 (ADDR_SE_PLAY = 0x4C3C00)
 */

#pragma once

#include <stdint.h>

namespace AudioHooks {

// Install/Uninstall MinHook on ADDR_SOUND_TRIGGER
bool Install();
void Uninstall();
bool IsInstalled();

// Set suppression state. When suppressed, sound triggers are silently dropped.
void SetSuppressed(bool suppressed);
bool IsSuppressed();

struct Stats {
    uint32_t total_calls;
    uint32_t suppressed_calls;
    uint32_t passed_calls;
};

void GetStats(Stats* out);
void ResetStats();

} // namespace AudioHooks
