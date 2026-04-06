/**
 * Alice Senki 2 - Audio Hooks Implementation
 *
 * Hooks SE_Play (sub_4C3C00) — the actual sound effect trigger.
 * Signature: char __cdecl SE_Play(int soundId)
 *
 * SE_Play only modifies audio-side state (SE_ChannelActive[], SE_ChannelIndex)
 * and calls Audio_Stop + Audio_Play_Wrapper. It has NO simulation side effects,
 * so it is safe to suppress during rollback resimulation.
 *
 * When suppressed: returns 0 early (no audio plays, no channel state changes).
 * When not suppressed: passes through to the original SE_Play.
 */

#include "audio_hooks.h"
#include "as2_constants.h"
#include "log_window.h"
#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace AudioHooks {

// ============================================================================
// Types
// ============================================================================

// sub_4C3C00 — SE_Play: char __cdecl SE_Play(int soundId)
// Checks SE_ChannelActive[soundId], calls Audio_Stop + Audio_Play_Wrapper,
// sets SE_ChannelActive[soundId]=1, rotates SE_ChannelIndex.
// Pure audio state — no simulation writes.
typedef char (__cdecl *SEPlay_t)(int soundId);

static SEPlay_t g_origSEPlay = nullptr;
static bool s_installed  = false;
static bool s_suppressed = false;
static Stats s_stats     = {};
static uint32_t s_callsSinceToggle = 0;

// ============================================================================
// Hook
// ============================================================================

static char __cdecl Hook_SEPlay(int soundId) {
    s_stats.total_calls++;
    s_callsSinceToggle++;
    
    if (s_suppressed) {
        s_stats.suppressed_calls++;
        return 0;  // Skip sound during rollback — no simulation side effects
    }
    
    s_stats.passed_calls++;
    return g_origSEPlay(soundId);
}

// ============================================================================
// Public API
// ============================================================================

bool Install() {
    if (s_installed) return true;
    
    LOG_INFO("[Audio] Installing SE_Play hook @ 0x%08X (ADDR_SE_PLAY)", ADDR_SE_PLAY);
    
    MH_STATUS status = MH_CreateHook(
        (void*)ADDR_SE_PLAY,
        (void*)&Hook_SEPlay,
        (void**)&g_origSEPlay
    );
    if (status != MH_OK) {
        LOG_ERROR("[Audio] Failed to create hook: %d", status);
        return false;
    }
    
    status = MH_EnableHook((void*)ADDR_SE_PLAY);
    if (status != MH_OK) {
        LOG_ERROR("[Audio] Failed to enable hook: %d", status);
        MH_RemoveHook((void*)ADDR_SE_PLAY);
        return false;
    }
    
    s_installed = true;
    LOG_INFO("[Audio] SE_Play hook installed — sound suppression during rollback is now functional");
    return true;
}

void Uninstall() {
    if (!s_installed) return;
    MH_DisableHook((void*)ADDR_SE_PLAY);
    MH_RemoveHook((void*)ADDR_SE_PLAY);
    s_installed = false;
    s_suppressed = false;
    LOG_INFO("[Audio] SE_Play hook uninstalled");
}

bool IsInstalled() { return s_installed; }

void SetSuppressed(bool suppressed) {
    if (s_suppressed != suppressed) {
        s_callsSinceToggle = 0;
        LOG_NET_INFO("[Audio] Suppression %s (total=%u passed=%u suppressed=%u)",
                     suppressed ? "ENABLED" : "DISABLED",
                     s_stats.total_calls, s_stats.passed_calls, s_stats.suppressed_calls);
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

} // namespace AudioHooks
