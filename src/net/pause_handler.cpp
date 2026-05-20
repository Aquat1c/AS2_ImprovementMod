/**
 * Alice Senki 2 - Pause Handler for Netplay
 *
 * Detects when the user quits from the pause menu during an online match
 * and cleanly tears down the session so we don't leave a dangling connection.
 *
 * Source-of-truth: reverse-engineering notes for sub_4CA120 (Mode 8 Sub 4 = pause handler)
 *   - Calls sub_4C8250 for pause menu logic
 *   - Return 0 = resume → substate back to 3 (gameplay)
 *   - Return 1 = quit match → substate 5 (match end) with route
 *   - Return 2 = quit to menu → substate 5 (match end) with route
 *
 * After sub_4CA120 processes a quit, the game transitions through
 * Mode 8 Sub 5 (sub_4CA210) which reads match_header+10 for the route
 * and calls Game_ChangeMode accordingly.
 *
 * Strategy:
 *   - When entering PauseActive phase: start tracking
 *   - When leaving PauseActive with quit: send PauseQuit, cancel session
 *   - When receiving PauseQuit from remote: cancel session, let game
 *     follow its natural mode transition
 */

#include "net/pause_handler.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/match_lifecycle.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using namespace Net;

// ============================================================================
// Internal state
// ============================================================================

static bool  s_initialized     = false;
static bool  s_pauseTracked    = false;   // Currently in a pause we're tracking
static bool  s_wasQuit         = false;   // Did the pause exit via quit?
static bool  s_remoteQuit      = false;   // Did remote send PauseQuit?
static DWORD s_pauseEnterTime  = 0;

// ============================================================================
// Packet sending
// ============================================================================

static void SendPauseQuitPacket() {
    // PauseQuit is a zero-payload reliable packet
    bool ok = Session_SendPacket(CHANNEL_CONTROL, PacketType::PauseQuit,
                                 nullptr, 0, true);
    if (ok) {
        LOG_NETPLAY(LOG_INFO, "[PauseHandler] Sent PauseQuit to remote");
    } else {
        LOG_NETPLAY(LOG_WARNING, "[PauseHandler] Failed to send PauseQuit");
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void PauseHandler_Init() {
    if (s_initialized) return;
    s_pauseTracked = false;
    s_wasQuit = false;
    s_remoteQuit = false;
    s_initialized = true;
    LOG_NETPLAY(LOG_DEBUG, "[PauseHandler] Initialized");
}

void PauseHandler_Shutdown() {
    if (!s_initialized) return;
    s_pauseTracked = false;
    s_initialized = false;
    LOG_NETPLAY(LOG_DEBUG, "[PauseHandler] Shutdown");
}

void PauseHandler_FrameUpdate() {
    if (!s_initialized) return;

    // Handle remote quit signal at any time
    if (s_remoteQuit) {
        s_remoteQuit = false;
        s_wasQuit = true;

        LOG_NETPLAY(LOG_INFO, "[PauseHandler] Remote peer quit — cancelling session");
        Session_Cancel();
        return;
    }

    if (!s_pauseTracked) return;

    // We're in a tracked pause — check if the game left pause state
    MatchLifecyclePhase phase = MatchLifecycle_GetPhase();
    if (phase != MatchLifecyclePhase::PauseActive) {
        // Pause ended — determine if it was a quit or resume
        // If we transitioned to MatchEnd, it was a quit
        // If we transitioned to PlayableGameplay, it was a resume
        bool quit = (phase == MatchLifecyclePhase::MatchEnd ||
                     phase == MatchLifecyclePhase::PostMatchRoute ||
                     phase == MatchLifecyclePhase::ReturningToCharSel ||
                     phase == MatchLifecyclePhase::ReturningToMenu);

        PauseHandler_OnPauseExit(quit);
    }
}

void PauseHandler_OnPauseEnter() {
    if (!s_initialized) return;

    s_pauseTracked = true;
    s_wasQuit = false;
    s_pauseEnterTime = GetTickCount();

    LOG_NETPLAY(LOG_INFO, "[PauseHandler] Pause entered — tracking for quit");
}

void PauseHandler_OnPauseExit(bool wasQuit) {
    if (!s_pauseTracked) return;

    DWORD elapsed = GetTickCount() - s_pauseEnterTime;
    s_pauseTracked = false;
    s_wasQuit = wasQuit;

    if (wasQuit) {
        LOG_NETPLAY(LOG_INFO, "[PauseHandler] Pause quit detected after %ums — sending PauseQuit and cancelling session",
            elapsed);

        // Notify remote peer
        if (Session_IsConnected()) {
            SendPauseQuitPacket();
        }

        // Cancel the session — this will trigger disconnect recovery
        // via match lifecycle
        Session_Cancel();
    } else {
        LOG_NETPLAY(LOG_INFO, "[PauseHandler] Pause resumed after %ums", elapsed);
    }
}

void PauseHandler_OnRemotePauseQuit() {
    LOG_NETPLAY(LOG_INFO, "[PauseHandler] Remote PauseQuit received");
    s_remoteQuit = true;
}

bool PauseHandler_IsPauseTracked() {
    return s_pauseTracked;
}

bool PauseHandler_WasQuit() {
    return s_wasQuit;
}

} // namespace Net
