/**
 * Alice Senki 2 - Match Lifecycle State Management
 *
 * Tracks the mod-owned lifecycle of an online match across all non-trivial
 * game states: intro, playable gameplay, pause, round transitions, match end,
 * winscreen, post-match routing, and return-to-CharSel / rematch flow.
 *
 * The purpose of this layer is to ensure that:
 *   - The session/network pump remains alive through all lifecycle states
 *   - Vanilla netplay ownership never re-asserts during any owned state
 *   - The rollback gameplay core starts only at the correct boundary
 *   - Pause, round transitions, and match end do not tear down the session
 *   - Disconnect recovery works from any lifecycle state
 *   - Post-match routing (rematch/return/disconnect) is handled cleanly
 *
 * Lifecycle:
 *   MatchLifecycle_Init()          — once at mod startup
 *   MatchLifecycle_Shutdown()      — once at mod shutdown
 *   MatchLifecycle_FrameUpdate()   — every frame from ModOnFrame
 *   MatchLifecycle_OnMatchEnter()  — when PregameSync reaches GameplayHandoff
 *   MatchLifecycle_OnMatchExit()   — when leaving owned match flow entirely
 *   MatchLifecycle_OnDisconnect()  — on session loss in any state
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Match Lifecycle Phase
// ============================================================================

enum class MatchLifecyclePhase : uint8_t {
    Inactive = 0,           // No owned match in progress

    // Match entry sequence (Mode 8, pre-interactive)
    BootstrapWait,          // Waiting for bootstrap handoff (GameplayHandoff reached)
    LoadingAssets,          // Mode 8 Sub 0: loading character/stage assets
    MatchSetup,             // Mode 8 Sub 1: entity init, state reset
    MatchInit,              // Mode 8 Sub 2: intro sequence, HP/meter/pos init
    IntroActive,            // Mode 8 Sub 3 with intro lock / phase timer active

    // Core gameplay
    PlayableGameplay,       // Mode 8 Sub 3, no intro lock, no transition — ROLLBACK HERE

    // Interruptions during gameplay
    PauseActive,            // Mode 8 Sub 4: pause menu open

    // Round / match end sequence
    RoundTransition,        // Mode 8 Sub 3 with transition byte set (round end countdown)
    MatchEnd,               // Mode 8 Sub 5: match end handler running

    // Post-match flow
    PostMatchRoute,         // Match end committed, routing decision pending
    ReturningToCharSel,     // Routing back to CharSel for rematch (session alive)
    ReturningToMenu,        // Routing back to custom menu (session alive, match done)

    // Terminal
    DisconnectRecovery,     // Session lost, unwinding to safe state
};

inline const char* MatchLifecyclePhaseName(MatchLifecyclePhase phase) {
    switch (phase) {
        case MatchLifecyclePhase::Inactive:           return "Inactive";
        case MatchLifecyclePhase::BootstrapWait:      return "BootstrapWait";
        case MatchLifecyclePhase::LoadingAssets:       return "LoadingAssets";
        case MatchLifecyclePhase::MatchSetup:          return "MatchSetup";
        case MatchLifecyclePhase::MatchInit:           return "MatchInit";
        case MatchLifecyclePhase::IntroActive:         return "IntroActive";
        case MatchLifecyclePhase::PlayableGameplay:    return "PlayableGameplay";
        case MatchLifecyclePhase::PauseActive:         return "PauseActive";
        case MatchLifecyclePhase::RoundTransition:     return "RoundTransition";
        case MatchLifecyclePhase::MatchEnd:            return "MatchEnd";
        case MatchLifecyclePhase::PostMatchRoute:      return "PostMatchRoute";
        case MatchLifecyclePhase::ReturningToCharSel:  return "ReturningToCharSel";
        case MatchLifecyclePhase::ReturningToMenu:     return "ReturningToMenu";
        case MatchLifecyclePhase::DisconnectRecovery:  return "DisconnectRecovery";
        default:                                       return "Unknown";
    }
}

// ============================================================================
// Match Lifecycle Snapshot (read-only for UI / diagnostics)
// ============================================================================

struct MatchLifecycleSnapshot {
    bool                 active;
    MatchLifecyclePhase  phase;

    // Classification flags
    bool                 session_should_be_alive;   // Network pump must continue
    bool                 gameplay_is_playable;       // Rollback core may run
    bool                 pause_is_active;            // Game is paused
    bool                 transition_in_progress;     // Round/match end transition
    bool                 post_match_routing;         // Post-match decision pending
    bool                 match_owned;                // Mod owns match flow

    // Match state
    uint32_t             game_mode;
    uint32_t             game_substate;
    uint32_t             game_type;

    // Match details (valid when in match)
    uint8_t              match_end_route;            // Vanilla route byte (0-5)
    uint8_t              winner;                     // Winner byte from match header

    // Error
    char                 error[128];
};

// ============================================================================
// Lifecycle
// ============================================================================

void MatchLifecycle_Init();
void MatchLifecycle_Shutdown();

// ============================================================================
// Per-Frame
// ============================================================================

void MatchLifecycle_FrameUpdate();

// ============================================================================
// Events (called by PregameSync / menu controller)
// ============================================================================

/// Called when PregameSync reaches GameplayHandoff — mod now owns the match.
void MatchLifecycle_OnMatchEnter();

/// Called when explicitly leaving the owned match flow (clean exit).
void MatchLifecycle_OnMatchExit();

/// Called on session loss — unwind from any lifecycle state.
void MatchLifecycle_OnDisconnect(const char* reason);

/// Called when post-match rematch is selected — route back to CharSel.
void MatchLifecycle_OnRematch();

/// Called when post-match return is selected — go back to connected session menu.
void MatchLifecycle_OnReturnToSession();

// ============================================================================
// Queries
// ============================================================================

MatchLifecyclePhase MatchLifecycle_GetPhase();
void MatchLifecycle_GetSnapshot(MatchLifecycleSnapshot* out);

/// True when in Mode 8 and the rollback core should be active.
bool MatchLifecycle_IsGameplayPlayable();

/// True when the session/network pump should remain alive.
bool MatchLifecycle_SessionShouldBeAlive();

/// True when the mod owns the current match flow.
bool MatchLifecycle_IsMatchOwned();

/// True when pause is active.
bool MatchLifecycle_IsPauseActive();

/// True when post-match routing is in progress.
bool MatchLifecycle_IsPostMatchRouting();

} // namespace Net
