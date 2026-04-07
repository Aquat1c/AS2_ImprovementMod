/**
 * Alice Senki 2 - Sync Policy
 *
 * Classifies every game state into one of three synchronization modes:
 *
 *   Lockstep  — Both peers must agree before advancing.
 *               Used for: CharSel, StageSel, Pause, WinScreen confirm.
 *               Input is edge-triggered (confirm requires neutral→press).
 *
 *   Rollback  — Speculative execution with rollback correction.
 *               Used for: Playable gameplay only (Mode 8 Sub 3, no intro/transition).
 *               Only active when match lifecycle is PlayableGameplay.
 *
 *   Passive   — No input sync. Game advances locally, session stays alive.
 *               Used for: Intros, round transitions, loading, match end animations.
 *
 * This module also:
 *   - Enforces Stage Select ON for all netplay matches
 *   - Implements confirm input arming (neutral-release gating)
 *   - Prevents stale/held confirm from triggering lockstep actions
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Sync Mode Classification
// ============================================================================

enum class SyncMode : uint8_t {
    None = 0,       // Not in any synced state (offline, menu, etc.)
    Lockstep,       // Deterministic lockstep — wait for peer agreement
    Rollback,       // Speculative rollback — GekkoNet drives frame advance
    Passive,        // Session alive, no input sync — visual/timer states
};

inline const char* SyncModeName(SyncMode mode) {
    switch (mode) {
        case SyncMode::None:     return "None";
        case SyncMode::Lockstep: return "Lockstep";
        case SyncMode::Rollback: return "Rollback";
        case SyncMode::Passive:  return "Passive";
        default:                 return "Unknown";
    }
}

// ============================================================================
// Lockstep Context (which lockstep state are we in)
// ============================================================================

enum class LockstepContext : uint8_t {
    None = 0,
    CharSelect,     // Character selection grid (MODE_CHARSEL sub 2/4)
    StageSelect,    // Stage selection grid (CharSel substates 5-9)
    Pause,          // In-match pause (MODE_MATCH sub 4)
    WinScreen,      // Win screen / post-match confirm (MODE_WINSCREEN)
    PostMatch,      // Post-match routing decision (lifecycle PostMatchRoute)
};

inline const char* LockstepContextName(LockstepContext ctx) {
    switch (ctx) {
        case LockstepContext::None:        return "None";
        case LockstepContext::CharSelect:  return "CharSelect";
        case LockstepContext::StageSelect: return "StageSelect";
        case LockstepContext::Pause:       return "Pause";
        case LockstepContext::WinScreen:   return "WinScreen";
        case LockstepContext::PostMatch:   return "PostMatch";
        default:                           return "Unknown";
    }
}

// ============================================================================
// Sync Policy Snapshot (read-only for UI / diagnostics)
// ============================================================================

struct SyncPolicySnapshot {
    SyncMode         current_mode;
    LockstepContext  lockstep_context;
    bool             session_active;        // Session should be alive
    bool             confirm_armed;         // Confirm input has been armed (neutral seen)
    bool             stage_select_forced;   // Stage Select was forced ON
};

// ============================================================================
// Lifecycle
// ============================================================================

void SyncPolicy_Init();
void SyncPolicy_Shutdown();

// ============================================================================
// Per-Frame
// ============================================================================

/// Call every frame from ModOnFrame, after MatchLifecycle_FrameUpdate.
void SyncPolicy_FrameUpdate();

// ============================================================================
// Classification Queries
// ============================================================================

/// Current sync mode based on game state + lifecycle phase.
SyncMode SyncPolicy_GetCurrentMode();

/// Current lockstep context (valid only when mode is Lockstep).
LockstepContext SyncPolicy_GetLockstepContext();

/// True when in a lockstep state that requires peer agreement.
bool SyncPolicy_IsLockstep();

/// True when rollback gameplay is active.
bool SyncPolicy_IsRollbackActive();

/// True when in a passive lifecycle state (session alive, no input sync).
bool SyncPolicy_IsPassive();

/// True when any sync mode is active (session should be alive).
bool SyncPolicy_IsSessionSynced();

// ============================================================================
// Confirm Input Arming (Edge-Triggered Lockstep Confirms)
// ============================================================================

/// Returns true if confirm input is currently armed (safe to act on press).
/// Must see a neutral (released) frame before the confirm press counts.
bool SyncPolicy_IsConfirmArmed();

/// Feed raw confirm button state each frame. Returns true on the
/// rising edge ONLY if the confirm was properly armed.
/// This prevents held-confirm from triggering lockstep actions.
bool SyncPolicy_FeedConfirmInput(bool confirm_pressed);

/// Force-reset the confirm arm state (e.g., on state transition).
void SyncPolicy_ResetConfirmArm();

// ============================================================================
// Stage Select Enforcement
// ============================================================================

/// Force Stage Select ON for netplay. Called during CharSel launch.
void SyncPolicy_EnforceStageSelectForNetplay();

/// Returns true if Stage Select was force-enabled by the mod.
bool SyncPolicy_IsStageSelectForced();

// ============================================================================
// Diagnostics
// ============================================================================

void SyncPolicy_GetSnapshot(SyncPolicySnapshot* out);

} // namespace Net
