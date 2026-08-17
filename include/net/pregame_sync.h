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
 * Since re0.7 M3 the state machine no longer owns the Session packet
 * callback: net/packet_router is the single registered dispatch owner and
 * routes the pregame packet set into PregameSync_OnSessionPacket.
 *
 * Since re0.7 M5 the implementation is net/match_setup.cpp (master plan
 * §2.5): one phase machine replacing pregame_sync + match_bootstrap behind
 * this preserved 12-function facade. It owns epoch authority (host-minted
 * u32 generations, strictly increasing, EpochAlign TransitionBarrier commit
 * before any frontend input exchange), the config/load/baseline/handoff
 * barriers, the rematch fast path, and the §4.6 recovery ladder (frontend
 * timeouts recover by pregame restart under a fresh epoch — never teardown,
 * INV-12).
 */

#pragma once

#include "net/locked_match_config.h"
#include "net/protocol.h"
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
    bool          round_count_valid;
    uint8_t       round_count;          // Vanilla option 0..2 (wins required = value + 1)
    int           rounds_to_win;
    char          rounds_label[32];

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

/// Begin the continue-screen rematch fast path: same reset as Begin(), but
/// the frontend charsel/stagesel phases are skipped — at SyncConfirmed the
/// locked config is rebuilt from `previousConfig` (chars/palettes/stage/
/// host_side preserved; host mints fresh seeds) and goes straight to
/// ConfigExchange. `previousConfig` is copied before the reset, so passing
/// PregameSync_GetLockedConfig() is safe.
bool PregameSync_BeginRematch(const LockedMatchConfig* previousConfig);

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

/// The current epoch (host-minted u32 generation, §2.5). Strictly increasing
/// for the life of the session; 0 = no epoch minted/adopted yet. Consumed by
/// the engine adapter (savestate tag context / engine arm) and diagnostics.
uint32_t PregameSync_GetCurrentEpoch();

/// Bootstrap handoff facts consumed by the match director side
/// (online_wiring until M6): baseline CRCs and barrier frame diagnostics.
struct PregameBootstrapInfo {
    uint32_t local_baseline_crc;
    uint32_t remote_baseline_crc;
    int32_t  local_load_sim_frame;
    int32_t  remote_load_sim_frame;
    int32_t  local_baseline_sim_frame;
    int32_t  remote_baseline_sim_frame;
    uint32_t bootstrap_frame_abs;
    int32_t  gameplay_start_host_game_abs_frame;
};
void PregameSync_GetBootstrapInfo(PregameBootstrapInfo* out);

/// Handle SyncAnnounce/SyncConfirm while the gameplay packet callback is still
/// active (rematch race). Returns true when the packet was consumed.
bool PregameSync_HandleCrossPhaseSessionPacket(PacketType type,
                                               const void* payload,
                                               size_t payloadLen);

/// Pregame-owned packet entry point, called by packet_router (§2.3) for the
/// pregame machine's packet set (SyncAnnounce/Confirm, CharSelInput/Lock,
/// StageSync, Config/Load/Baseline/GameplayStart). Since M3 the router is the
/// single registered Session callback; pregame_sync no longer registers or
/// hands off the callback itself.
void PregameSync_OnSessionPacket(PacketType type, const void* payload, size_t payloadLen);

} // namespace Net
