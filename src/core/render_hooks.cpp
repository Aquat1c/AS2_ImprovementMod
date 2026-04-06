/**
 * Alice Senki 2 - Render Hooks Implementation
 *
 * Hooks sub_620D90 (screen flip / D3D9 Present) to skip during rollback.
 */

#include "render_hooks.h"
#include "as2_constants.h"
#include "visual_smoothing.h"
#include "log_window.h"
#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Address of the render present function (sub_620D90)
#define ADDR_RENDER_PRESENT  (GAME_BASE + 0x220D90)

namespace RenderHooks {

// ============================================================================
// Types
// ============================================================================

// sub_620D90 signature: int __cdecl sub_620D90()
typedef int (__cdecl *RenderPresent_t)();

static RenderPresent_t g_origRenderPresent = nullptr;
static bool s_installed  = false;
static bool s_suppressed = false;
static Stats s_stats     = {};
static uint32_t s_callsSinceToggle = 0;

// ============================================================================
// Hook
// ============================================================================

static int __cdecl Hook_RenderPresent() {
    s_stats.total_calls++;
    s_callsSinceToggle++;
    
    if (s_suppressed) {
        s_stats.suppressed_calls++;
        if (s_callsSinceToggle <= 5 || (s_callsSinceToggle % 300) == 0) {
            LOG_NET_DEBUG("[Render] Present skipped #%u since toggle (total=%u rendered=%u suppressed=%u)",
                          s_callsSinceToggle, s_stats.total_calls, s_stats.rendered_calls, s_stats.suppressed_calls);
        }
        // DISABLED: Visual smoothing temporarily disabled.
        // VisualSmoothing::RestoreAfterRender();
        return 0;  // Skip Present entirely during rollback resim
    }
    
    s_stats.rendered_calls++;
    if (s_callsSinceToggle <= 5) {
        LOG_NET_DEBUG("[Render] Present rendered #%u since toggle (total=%u rendered=%u suppressed=%u)",
                      s_callsSinceToggle, s_stats.total_calls, s_stats.rendered_calls, s_stats.suppressed_calls);
    }
    int result = g_origRenderPresent();
    // DISABLED: Visual smoothing temporarily disabled.
    // VisualSmoothing::RestoreAfterRender();
    return result;
}

// ============================================================================
// Public API
// ============================================================================

bool Install() {
    if (s_installed) return true;
    
    LOG_INFO("[Render] Installing present hook @ 0x%08X", ADDR_RENDER_PRESENT);
    
    MH_STATUS status = MH_CreateHook(
        (void*)ADDR_RENDER_PRESENT,
        (void*)&Hook_RenderPresent,
        (void**)&g_origRenderPresent
    );
    if (status != MH_OK) {
        LOG_ERROR("[Render] Failed to create hook: %d", status);
        return false;
    }
    
    status = MH_EnableHook((void*)ADDR_RENDER_PRESENT);
    if (status != MH_OK) {
        LOG_ERROR("[Render] Failed to enable hook: %d", status);
        MH_RemoveHook((void*)ADDR_RENDER_PRESENT);
        return false;
    }
    
    s_installed = true;
    LOG_INFO("[Render] Present hook installed");
    return true;
}

void Uninstall() {
    if (!s_installed) return;
    MH_DisableHook((void*)ADDR_RENDER_PRESENT);
    MH_RemoveHook((void*)ADDR_RENDER_PRESENT);
    s_installed = false;
    s_suppressed = false;
    LOG_INFO("[Render] Present hook uninstalled");
}

bool IsInstalled() { return s_installed; }

void SetSuppressed(bool suppressed) {
    if (s_suppressed != suppressed) {
        s_callsSinceToggle = 0;
        LOG_NET_INFO("[Render] Suppression %s (total=%u rendered=%u suppressed=%u)",
                     suppressed ? "ENABLED" : "DISABLED",
                     s_stats.total_calls, s_stats.rendered_calls, s_stats.suppressed_calls);
    }
    s_suppressed = suppressed;
}

bool IsSuppressed() { return s_suppressed; }

void GetStats(Stats* out) {
    if (out) *out = s_stats;
}

void ResetStats() {
    s_stats = {};
    s_callsSinceToggle = 0;
}

} // namespace RenderHooks
