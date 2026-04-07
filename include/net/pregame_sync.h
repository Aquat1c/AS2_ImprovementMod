/**
 * Alice Senki 2 - Pre-Game Synchronization Layer
 *
 * State machine that bridges the session/menu layer and the rollback
 * gameplay core. Manages:
 *   1. Initial session sync (metadata exchange after connect)
 *   2. Front-end sync handoff (CharSel/StageSel lockstep delegation)
 *   3. Match startup bootstrap (load barrier, baseline, gameplay handoff)
 *
 * Lifecycle:
 *   PregameSync_Init() at mod startup
 *   PregameSync_FrameUpdate() every frame from ModOnFrame
 *   PregameSync_Begin() when session is connected and launch is requested
 *   PregameSync_Abort() on disconnect or error
 *   PregameSync_Shutdown() at mod shutdown
 *
 * The state machine registers itself as the Session_SetPacketCallback
 * handler for pre-game packet types (11-19).
 */

#pragma once

#include "net/locked_match_config.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// Pre-Game Sync Phase
// ============================================================================

enum class PregamePhase : uint8_t {
    Idle = 0,                   // No pre-game sync active

    // Initial session sync (after connect, before charsel interaction)
    SyncAnnounce,               // Sending/receiving session announce
    SyncExchange,               // Both announced, validating + confirming
    SyncConfirmed,              // Both confirmed, transitioning to frontend

    // Front-end sync
    FrontendCharSel,            // CharSel lockstep active
    FrontendStageSel,           // StageSel lockstep active
    FrontendLocked,             // Both CharSel+StageSel confirmed

    // Match config exchange
    ConfigExchange,             // Host sends config, join validates
    ConfigAgreed,               // Both peers agreed on config

    // Match bootstrap
    BootstrapLoading,           // Waiting for both peers to finish loading
    BootstrapBaseline,          // Baseline savestate capture + agreement
    BootstrapReady,             // Both peers ready to begin gameplay

    // Terminal states
    GameplayHandoff,            // Gameplay can begin — pre-game complete
    Error,                      // Unrecoverable error
};

inline const char* PregamePhaseName(PregamePhase phase) {
    switch (phase) {
        case PregamePhase::Idle:              return "Idle";
        case PregamePhase::SyncAnnounce:      return "SyncAnnounce";
        case PregamePhase::SyncExchange:      return "SyncExchange";
        case PregamePhase::SyncConfirmed:     return "SyncConfirmed";
        case PregamePhase::FrontendCharSel:   return "FrontendCharSel";
        case PregamePhase::FrontendStageSel:  return "FrontendStageSel";
        case PregamePhase::FrontendLocked:    return "FrontendLocked";
        case PregamePhase::ConfigExchange:    return "ConfigExchange";
        case PregamePhase::ConfigAgreed:      return "ConfigAgreed";
        case PregamePhase::BootstrapLoading:  return "BootstrapLoading";
        case PregamePhase::BootstrapBaseline: return "BootstrapBaseline";
        case PregamePhase::BootstrapReady:    return "BootstrapReady";
        case PregamePhase::GameplayHandoff:   return "GameplayHandoff";
        case PregamePhase::Error:             return "Error";
        default:                              return "Unknown";
    }
}

// ============================================================================
// Pre-Game Sync Snapshot (read-only for UI)
// ============================================================================

struct PregameSnapshot {
    bool          active;
    PregamePhase  phase;

    // Session sync
    uint32_t      session_id;
    bool          sync_confirmed;
    uint8_t       assigned_side;

    // CharSel/StageSel
    bool          local_charsel_locked;
    bool          remote_charsel_locked;
    bool          local_stage_locked;
    bool          remote_stage_locked;

    // Loading
    bool          local_loaded;
    bool          remote_loaded;

    // Baseline
    bool          local_baseline_ready;
    bool          remote_baseline_ready;

    // Config
    bool          config_agreed;
    uint32_t      config_hash;

    // Status
    char          status_text[128];
    char          error_text[128];
};

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize the pre-game sync system. Call once at mod startup.
void PregameSync_Init();

/// Shut down. Call once at mod shutdown.
void PregameSync_Shutdown();

// ============================================================================
// Per-Frame
// ============================================================================

/// Drive the pre-game sync state machine. Call once per game frame.
void PregameSync_FrameUpdate();

// ============================================================================
// Control
// ============================================================================

/// Begin pre-game sync after session is connected.
/// Transitions from Idle to FrontendCharSel.
/// Returns false if session is not in a valid state.
bool PregameSync_Begin();

/// Abort pre-game sync (e.g. on disconnect or player quit).
/// Returns to Idle.
void PregameSync_Abort(const char* reason);

// ============================================================================
// Queries
// ============================================================================

/// Get current phase.
PregamePhase PregameSync_GetPhase();

/// Is a pre-game sync currently in progress?
bool PregameSync_IsActive();

/// Get read-only snapshot for UI/logging.
void PregameSync_GetSnapshot(PregameSnapshot* out);

/// Get the locked match config (valid only after ConfigAgreed).
const LockedMatchConfig* PregameSync_GetLockedConfig();

/// Has the pre-game sync completed successfully?
bool PregameSync_IsComplete();

} // namespace Net
