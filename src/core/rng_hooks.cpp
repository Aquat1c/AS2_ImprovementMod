/**
 * Alice Senki 2 - RNG Hooks Implementation (Facade)
 *
 * Thin facade over the existing RNG hooks in as2_rollback.cpp.
 * The actual hooks on static CRT rand()/srand() are installed by
 * as2_rollback.cpp during game initialization.  This module provides
 * a clean namespace-based API for the netplay/rollback subsystems.
 *
 * DO NOT install separate MinHook hooks here — as2_rollback.cpp
 * already hooks ADDR_STATIC_SRAND / ADDR_STATIC_RAND.
 */

#include "rng_hooks.h"
#include "as2_rollback.h"
#include "log_window.h"

namespace RngHooks {

// ============================================================================
// State (visual seed is managed here; sim seed delegates to as2_rollback.cpp)
// ============================================================================

static uint32_t s_visualSeed = 0;
static bool     s_installed  = false;
static bool     s_visualSplit = false;
static Stats    s_stats      = {};

// ============================================================================
// Public API — delegates to AS2_GetRngSeed / AS2_SetRngSeed
// ============================================================================

bool Install() {
    // No-op: hooks are already installed by as2_rollback.cpp.
    // We just mark ourselves as "installed" for callers that check.
    if (s_installed) return true;
    s_installed = true;
    LOG_INFO("[RngHooks] Facade initialized (actual hooks in as2_rollback.cpp)");
    return true;
}

void Uninstall() {
    // No-op: we don't own the hooks.
    s_installed = false;
    LOG_INFO("[RngHooks] Facade uninstalled");
}

bool IsInstalled() { return s_installed; }

uint32_t GetSimSeed() {
    return AS2_GetRngSeed();
}

void SetSimSeed(uint32_t seed) {
    AS2_SetRngSeed(seed);
}

int SimRand() {
    // Use the existing hooked rand() path.
    // as2_rollback.cpp's Hook_rand() updates g_currentRngSeed.
    s_stats.sim_rand_calls++;
    return AS2_GetRngSeed();  // Caller shouldn't rely on this; use the game's rand().
}

uint32_t GetVisualSeed() { return s_visualSeed; }
void SetVisualSeed(uint32_t seed) { s_visualSeed = seed; }

void ResetForMatch(uint32_t sessionSeed) {
    AS2_SetRngSeed(sessionSeed);
    s_visualSeed = sessionSeed ^ 0xDEADBEEF;
    AS2_SetVisualRngSeed(s_visualSeed);  // Apply to actual visual RNG stream
    s_stats = {};
    LOG_INFO("[RngHooks] ResetForMatch: session_seed=%u visual=0x%08X",
             sessionSeed, s_visualSeed);
}

void SetVisualSplitEnabled(bool enabled) { s_visualSplit = enabled; }
bool IsVisualSplitEnabled() { return s_visualSplit; }

void GetStats(Stats* out) {
    if (out) *out = s_stats;
}

} // namespace RngHooks
