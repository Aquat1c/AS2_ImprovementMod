/**
 * Alice Senki 2 - Render Hooks
 *
 * Suppresses D3D9 Present during rollback resimulation (§17A.4).
 * Allows the final frame (post-resimulation) to render normally.
 *
 * Hook target: sub_620D90 (screen flip / render present)
 */

#pragma once

#include <stdint.h>

namespace RenderHooks {

// Install/Uninstall MinHook on sub_620D90
bool Install();
void Uninstall();
bool IsInstalled();

// Set suppression state. When suppressed, Present is skipped.
void SetSuppressed(bool suppressed);
bool IsSuppressed();

struct Stats {
    uint32_t total_calls;
    uint32_t suppressed_calls;
    uint32_t rendered_calls;
};

void GetStats(Stats* out);
void ResetStats();

} // namespace RenderHooks
