/**
 * Alice Senki 2 - Online Rollback Wiring
 *
 * Connects the bootstrap/lifecycle layer to the rollback gameplay core.
 * This is the integration glue — NOT a new architecture — just wiring:
 *
 *   1. Bootstrap completion → RollbackSession_Begin
 *   2. GekkoData packets → RollbackSession_BufferGekkoPacket
 *   3. Lifecycle transitions → RollbackSession_End / state safety
 *   4. Disconnect/failure → safe teardown 
 *   5. Post-match/rematch → clean handoff
 *   6. Full-path logging throughout
 *   7. Live diagnostics + stress hook integration
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// ============================================================================
// Lifecycle
// ============================================================================

void OnlineWiring_Init();
void OnlineWiring_Shutdown();

// ============================================================================
// Per-Frame (called from ModOnFrame after lifecycle/policy updates)
// ============================================================================

void OnlineWiring_FrameUpdate();

/// Returns true if the rollback session is active and match runtime should advance.
bool OnlineWiring_IsGameplayActive();

// ============================================================================
// Events
// ============================================================================

/// Called when pregame bootstrap hands the match to the lifecycle layer.
/// Starts the rollback session from the agreed baseline if not already active.
void OnlineWiring_OnGameplayStart();

/// Called when MatchLifecycle leaves PlayableGameplay.
/// Pauses/suspends rollback (does NOT destroy session for pause/transition).
void OnlineWiring_OnGameplayPause(const char* reason);

/// Called when match ends (MatchEnd phase reached).
void OnlineWiring_OnMatchEnd();

/// Called on disconnect from any state.
void OnlineWiring_OnDisconnect(const char* reason);

/// Called for post-match rematch (returning to CharSel).
void OnlineWiring_OnRematch();

/// Called for post-match return to session menu.
void OnlineWiring_OnReturnToSession();

// ============================================================================
// Diagnostics
// ============================================================================

struct OnlineWiringSnapshot {
    bool     rollback_started;       // Has rollback session ever started this match
    bool     rollback_active;        // Is rollback session currently active
    bool     gameplay_active;        // Are we in playable gameplay right now
    int32_t  handoff_frame;          // Frame at which bootstrap → gameplay occurred
    uint32_t baseline_crc;           // Baseline CRC from bootstrap
    uint32_t config_hash;            // Config hash from pregame
    int      handoff_delay;          // Active delay at gameplay handoff
    int      handoff_budget;         // Rollback budget at handoff
    int      remote_inputs_received; // Total remote inputs processed
    int      packets_dispatched;     // Total gameplay packets dispatched
};

void OnlineWiring_GetSnapshot(OnlineWiringSnapshot* out);

} // namespace Rollback
