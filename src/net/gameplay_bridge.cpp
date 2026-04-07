/**
 * Alice Senki 2 - Gameplay Bridge Implementation (GekkoNet-driven)
 *
 * Simplified bridge: GekkoNet owns all rollback logic. This bridge
 * handles session lifecycle and diagnostics only. The per-frame
 * simulation loop is driven by the input dispatcher hook.
 */

#include "net/gameplay_bridge.h"
#include "net/player_side_mapping.h"
#include "net/session_manager.h"
#include "rollback/rollback_session.h"
#include "rollback/netplay_log.h"
#include "patches/input_sync_hooks.h"
#include "ui/log_window.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

namespace Net {

// ============================================================================
// Internal State
// ============================================================================

static bool s_initialized    = false;
static bool s_sessionActive  = false;
static char s_status[128]    = "GameplayBridge: not initialized.";

// Per-session counters for diagnostics
static int  s_framesUpdated  = 0;

static void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, args);
    va_end(args);
}

// ============================================================================
// Lifecycle
// ============================================================================

bool GameplayBridge_Init() {
    if (s_initialized) return true;

    s_initialized = true;
    s_sessionActive = false;
    SetStatus("GameplayBridge ready (GekkoNet rollback).");
    LOG_INFO("[GameplayBridge] Initialized (GekkoNet rollback mode)");

    return true;
}

void GameplayBridge_Shutdown() {
    if (!s_initialized) return;

    if (s_sessionActive) {
        GameplayBridge_EndSession();
    }

    s_initialized = false;
    SetStatus("GameplayBridge shut down.");
    LOG_INFO("[GameplayBridge] Shut down");
}

// ============================================================================
// Bridge Status
// ============================================================================

void GameplayBridge_GetSnapshot(GameplayBridgeSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->initialized = s_initialized;
    out->session_active = s_sessionActive;
    out->local_game_slot = PlayerMapping_GetLocalGameSlot();
    out->remote_game_slot = PlayerMapping_GetRemoteGameSlot();
    out->active_delay = Rollback::RollbackSession_GetActiveDelay();
    out->rollback_budget = Rollback::RollbackSession_GetRollbackBudget();
    out->current_frame = Rollback::RollbackSession_GetCurrentFrame();
    out->frames_ahead = Rollback::RollbackSession_FramesAhead();

    // GekkoNet network stats from rollback session
    Rollback::RollbackSessionSnapshot rbSnap{};
    Rollback::RollbackSession_GetSnapshot(&rbSnap);
    out->gekko_avg_ping = rbSnap.gekko_avg_ping;
    out->gekko_jitter = rbSnap.gekko_jitter;

    strncpy_s(out->status, sizeof(out->status), s_status, _TRUNCATE);
}

// ============================================================================
// Gameplay Session Control
// ============================================================================

bool GameplayBridge_StartSession(const Rollback::RollbackSessionConfig& config) {
    if (s_sessionActive) {
        LOG_WARN("[GameplayBridge] Session already active — ending previous");
        GameplayBridge_EndSession();
    }

    // Reset per-session counters
    s_framesUpdated = 0;

    // Set player mapping from the config
    PlayerMapping_SetAssignment(config.local_player);

    // Verify player mapping
    int verifyLocal = PlayerMapping_GetLocalGameSlot();
    int verifyRemote = PlayerMapping_GetRemoteGameSlot();
    if (verifyLocal != config.local_player || verifyRemote != config.remote_player) {
        LOG_ERROR("[GameplayBridge] Player mapping mismatch! expected local=P%d remote=P%d, got local=P%d remote=P%d",
            config.local_player + 1, config.remote_player + 1,
            verifyLocal + 1, verifyRemote + 1);
        PlayerMapping_Clear();
        SetStatus("Player mapping verification failed.");
        return false;
    }

    // Start the GekkoNet rollback session
    bool ok = Rollback::RollbackSession_Begin(config);
    if (!ok) {
        LOG_ERROR("[GameplayBridge] RollbackSession_Begin failed");
        Rollback::NetplayLog_Write("BRIDGE", -1,
            "ERROR: RollbackSession_Begin FAILED");
        PlayerMapping_Clear();
        SetStatus("Failed to start gameplay session.");
        return false;
    }

    s_sessionActive = true;
    SetStatus("Session active (GekkoNet rollback, local=P%d).",
        config.local_player + 1);

    LOG_INFO("[GameplayBridge] Session started: local=P%d remote=P%d delay=%d budget=%d",
        config.local_player + 1, config.remote_player + 1,
        config.initial_delay, config.rollback_budget);
    Rollback::NetplayLog_Write("BRIDGE", config.start_frame,
        "Session started: local=P%d remote=P%d delay=%d budget=%d baseline=0x%08X",
        config.local_player + 1, config.remote_player + 1,
        config.initial_delay, config.rollback_budget, config.baseline_checksum);

    return true;
}

void GameplayBridge_EndSession() {
    if (!s_sessionActive) return;

    int32_t frame = Rollback::RollbackSession_GetCurrentFrame();
    Rollback::NetplayLog_Write("BRIDGE", frame,
        "Session ending: frames_updated=%d", s_framesUpdated);

    Rollback::RollbackSession_End();
    PlayerMapping_Clear();

    s_sessionActive = false;
    SetStatus("Session ended.");
    LOG_INFO("[GameplayBridge] Session ended (frames=%d)", s_framesUpdated);
}

bool GameplayBridge_IsSessionActive() {
    return s_sessionActive;
}

// ============================================================================
// Per-Frame (lightweight diagnostic update only)
// ============================================================================

void GameplayBridge_FrameUpdate() {
    if (!s_sessionActive) return;
    s_framesUpdated++;
}

// ============================================================================
// Input Routing
// ============================================================================

uint16_t GameplayBridge_ReadLocalInput() {
    return PlayerMapping_ReadLocalInput();
}

} // namespace Net
