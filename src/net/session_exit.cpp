#include "net/session_exit.h"

#include "net/session_manager.h"
#include "net/spectator_client.h"
#include "net/spectator_runtime.h"
#include "net/session_types.h"
#include "net/session2.h"
#include "net/protocol.h"
#include "net/netplay_menu_controller.h"
#include "net/spectator_protocol.h"   // CHANNEL_CONTROL
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "patches/memory_utils.h"
#include "rollback/rollback_session.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using namespace Net;

// Long enough that it cannot be hit by a stray tap during a match, short
// enough to feel deliberate rather than punitive.
constexpr DWORD kHoldMs = 1000;

// The MATCH abort is counted in FRAMES off the confirmed input stream, not in
// milliseconds off one machine's clock. 60 confirmed frames == 1s at 60Hz.
// Wall-clock cannot work here: two peers' clocks disagree, so a ms-based
// trigger fires on different simulation frames, and a spectator replaying the
// stream has no clock of its own at all.
constexpr int kHoldFrames = 60;

int  s_holdFramesP1 = 0;
int  s_holdFramesP2 = 0;
bool s_abortFired   = false;

bool  s_initialized   = false;
bool  s_holding       = false;
DWORD s_holdStart     = 0;
bool  s_firedThisHold = false;   // one action per physical press
bool  s_pauseBlockOwned = false; // did WE turn the pause block on?

// Raw, real-time, and deliberately NOT part of the rollback input stream: a
// gesture that ended the match must never be predictable, rollback-able, or
// captured into a snapshot.
bool MenuKeyHeld() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        return false;  // alt-tabbed; the key belongs to whatever has focus
    }
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) return true;

    // ...or the player's bound menu key (SELECT). Read raw so this stays out
    // of the rollback input stream like ESC does.
    if (const PlayerBindings_t* b = InputSystem_GetBindings(0)) {
        const int vk = InputSystem_ScancodeToVirtualKey(b->select.keyboard_key);
        if (vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0) return true;
    }
    return false;
}

bool SessionLive() {
    return Session_IsConnected();
}

// A spectator has no player session, so SessionLive() is false for it.
bool SpectatingLive() {
    SpectatorClientSnapshot snap{};
    SpectatorClient_GetSnapshot(&snap);
    return snap.active;
}

// EXACTLY the vanilla pause menu's "return to character select" branch
// (sub_4CA120, decomp:118367-118376):
//     *(_BYTE *)(a2 + 10) = (dword_816410 == 5) ? 3 : 1;   // route
//     *(_BYTE *)(a2 + 1)  = 1;                             // transition flag
//     a1[14] = 3;  a1[15] = 0;                             // substate, timer
// a1 is dword_816358, and 0x816358 + 14*4 == 0x816390 == ADDR_SUB_STATE, so
// a1[14]/a1[15] are exactly ADDR_SUB_STATE / ADDR_SUB_STATE_TIMER. Reusing the
// game's own route is the point: substate 3 with the transition flag set hands
// off to the ordinary match-end path (sub_4CA210), which is already proven by
// every normal match ending.
void RouteToCharselLikePauseMenu() {
    const uint8_t route = (GetGameType() == 5) ? 3u : 1u;
    WriteMemory<uint8_t>(ADDR_MATCH_HEADER + 10, route);
    WriteMemory<uint8_t>(ADDR_MATCH_HEADER + MATCH_HEADER_TRANSITION_OFFSET, 1);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, 3u);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0u);
}

void AbortToCharsel(const char* who) {
    // ORDER IS LOAD-BEARING. match+1 and match+10 are inside the hashed
    // snapshot region [0x76C5F8, 0x7AB880). Writing them while the engine is
    // still capturing and hashing is precisely the divergence the vanilla
    // pause-quit produced. End the session's rollback first, so these writes
    // can no longer reach a digest, then route.
    Rollback::RollbackSession_End();
    // RollbackSession_End alone does NOT tell spectators. That notification
    // lives in the director's StopRollbackSession, which this path bypasses --
    // without it the archive just stops growing and every spectator waits for
    // frames that will never arrive.
    Net::SpectatorRuntime_OnMatchEnd("players left to character select");
    RouteToCharselLikePauseMenu();
    s_holdFramesP1 = s_holdFramesP2 = 0;

    Rollback::NetplayLog_Write("EXIT", -1,
        "Return-to-charsel (%s): rollback ended, routed via the pause-menu path "
        "(route=%u transition=1 sub=3)",
        who, (unsigned)((GetGameType() == 5) ? 3u : 1u));
    LOG_NETPLAY(LOG_INFO,
        "[SessionExit] %s requested character select — both peers returning",
        who);
}

void QuitSession(const char* who, const char* msg) {
    Rollback::NetplayLog_Write("EXIT", -1, "Graceful session quit (%s)", who);
    LOG_NETPLAY(LOG_INFO, "[SessionExit] %s quit the session", who);
    // NOT Session2_Terminate: a terminal reason during the pregame phase
    // surfaces as "Session lost during pre-game sync", which is an ERROR box
    // for something the player deliberately did.
    NetMenu::HandleGracefulSessionQuit(msg);
}

void SendControl(PacketType type) {
    const bool ok = Session_SendPacket(CHANNEL_CONTROL, type, nullptr, 0, true);
    Rollback::NetplayLog_Write("EXIT", -1, "Sent %s: ok=%d",
                               PacketTypeName(type), ok ? 1 : 0);
}

}  // namespace

namespace Net {

void SessionExit_Init() {
    s_initialized = true;
    s_holding = false;
    s_firedThisHold = false;
    s_pauseBlockOwned = false;
}

void SessionExit_Shutdown() {
    if (s_pauseBlockOwned) {
        InputSystem_SetPauseBlocked(false);
        s_pauseBlockOwned = false;
    }
    s_initialized = false;
}

void SessionExit_FrameUpdate() {
    // Self-initialising: this runs from ModOnFrame, which can fire before
    // match_director's init, and a missed init would silently re-enable the
    // pause menu -- the exact failure this module exists to prevent.
    if (!s_initialized) SessionExit_Init();

    const bool live = SessionLive();

    // 0. Keep the display awake for the whole session. Windows' monitor
    //    timeout put the screen to sleep mid-match on 2026-08-18, D3D9 lost
    //    the device, recovery failed repeatedly with 0x8876086C
    //    (D3DERR_INVALIDCALL), all three windows stayed black and the game
    //    thread stalled ~1s per frame until the session died. A netplay match
    //    is "activity" even though the player may not touch the keyboard for a
    //    whole round, so the OS has to be told.
    if (live) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    } else if (s_pauseBlockOwned) {
        SetThreadExecutionState(ES_CONTINUOUS);
    }

    // 1. The pause menu is disabled for the WHOLE session, not just gameplay:
    //    character select is a lockstep phase too, and a local pause there
    //    stalls the peer exactly the same way.
    if (live && !s_pauseBlockOwned) {
        InputSystem_SetPauseBlocked(true);
        s_pauseBlockOwned = true;
        Rollback::NetplayLog_Write("EXIT", -1, "Pause menu blocked for session");
    } else if (!live && s_pauseBlockOwned) {
        InputSystem_SetPauseBlocked(false);
        s_pauseBlockOwned = false;
        Rollback::NetplayLog_Write("EXIT", -1, "Pause menu unblocked (no session)");
    }

    // SPECTATOR: the gesture ends the watch session from ANY mode -- there is
    // no "back to character select" for a spectator, only stop watching.
    if (!live && SpectatingLive()) {
        const bool downSpec = MenuKeyHeld();
        if (!downSpec) {
            s_holding = false;
            s_firedThisHold = false;
            return;
        }
        if (!s_holding) {
            s_holding = true;
            s_holdStart = GetTickCount();
            return;
        }
        if (!s_firedThisHold && GetTickCount() - s_holdStart >= kHoldMs) {
            s_firedThisHold = true;
            Rollback::NetplayLog_Write("EXIT", -1,
                "Menu hold while spectating (mode=%u) — leaving the watch session",
                (unsigned)GetGameMode());
            LOG_NETPLAY(LOG_INFO, "[SessionExit] Stopped watching (menu hold)");
            SpectatorClient_Disconnect("stopped watching (menu hold)");
            NetMenu::HandleGracefulSessionQuit("You stopped watching.");
        }
        return;
    }

    if (!live) {
        s_holding = false;
        s_firedThisHold = false;
        return;
    }

    // 2. Hold-to-exit.
    const bool down = MenuKeyHeld();
    if (!down) {
        s_holding = false;
        s_firedThisHold = false;   // require a fresh press before firing again
        return;
    }

    if (!s_holding) {
        s_holding = true;
        s_holdStart = GetTickCount();
        return;
    }
    if (s_firedThisHold) return;
    if (GetTickCount() - s_holdStart < kHoldMs) return;

    const uint32_t mode = GetGameMode();
    if (mode == MODE_MATCH) {
        // Deliberately NOT fired here. In a match the gesture travels as
        // INPUT_MENU_HOLD inside the confirmed input word and fires from
        // SessionExit_NoteConfirmedInputs, so both players and every spectator
        // act on the SAME simulation frame. Firing from wall-clock here would
        // put the two peers on different frames.
    } else if (mode == MODE_CHARSEL) {
        s_firedThisHold = true;
        SendControl(PacketType::SessionQuitGraceful);
        QuitSession("local player", "You left the session.");
    }
    // Any other mode (loading, win screen): the gesture does nothing. Those
    // phases have their own barriers and an abrupt route here would race them.
}

void SessionExit_OnRemoteAbortToCharsel() {
    if (!s_initialized) return;
    if (GetGameMode() != MODE_MATCH) {
        // Already left gameplay by another path — nothing to abort.
        Rollback::NetplayLog_Write("EXIT", -1,
            "Remote abort ignored: not in a match (mode=%u)",
            (unsigned)GetGameMode());
        return;
    }
    AbortToCharsel("opponent");
}

void SessionExit_OnRemoteQuit() {
    if (!s_initialized) return;
    QuitSession("opponent", "Opponent left the session.");
}

uint16_t SessionExit_LocalMenuBit() {
    if (!s_initialized) SessionExit_Init();
    if (!SessionLive()) return 0;
    if (GetGameMode() != MODE_MATCH) return 0;
    const bool held = MenuKeyHeld();
    static bool s_lastHeld = false;
    if (held != s_lastHeld) {
        s_lastHeld = held;
        Rollback::NetplayLog_Write("EXIT", -1,
            "menu key %s (mode=%u)", held ? "DOWN" : "up",
            (unsigned)GetGameMode());
    }
    return held ? (uint16_t)INPUT_MENU_HOLD : (uint16_t)0;
}

void SessionExit_ResetHoldTracking(const char* why) {
    if (s_holdFramesP1 || s_holdFramesP2 || s_abortFired) {
        Rollback::NetplayLog_Write("EXIT", -1,
            "Hold tracking reset (%s)", why ? why : "?");
    }
    s_holdFramesP1 = 0;
    s_holdFramesP2 = 0;
    s_abortFired = false;
}

void SessionExit_NoteConfirmedInputs(int32_t matchRelFrame, uint16_t p1, uint16_t p2) {
    if (!s_initialized) SessionExit_Init();
    if (s_abortFired) return;

    s_holdFramesP1 = (p1 & INPUT_MENU_HOLD) ? (s_holdFramesP1 + 1) : 0;
    s_holdFramesP2 = (p2 & INPUT_MENU_HOLD) ? (s_holdFramesP2 + 1) : 0;

    if ((s_holdFramesP1 > 0 && s_holdFramesP1 % 15 == 0) ||
        (s_holdFramesP2 > 0 && s_holdFramesP2 % 15 == 0)) {
        Rollback::NetplayLog_Write("EXIT", matchRelFrame,
            "menu-hold confirmed frames p1=%d p2=%d (need %d) words=%04X/%04X",
            s_holdFramesP1, s_holdFramesP2, kHoldFrames, p1, p2);
    }

    const bool p1Fires = s_holdFramesP1 >= kHoldFrames;
    const bool p2Fires = s_holdFramesP2 >= kHoldFrames;
    if (!p1Fires && !p2Fires) return;

    s_abortFired = true;
    const char* who = p1Fires ? "P1" : "P2";
    Rollback::NetplayLog_Write("EXIT", matchRelFrame,
        "Menu-hold reached %d confirmed frames (%s) — returning to character select",
        kHoldFrames, who);

    // Players route themselves out. A spectator has no session of its own and
    // must not write the match route; it follows the stream.
    if (SessionLive()) {
        AbortToCharsel(who);
    }
}

bool SessionExit_HoldProgress(float* out01) {
    if (!s_initialized || !s_holding || s_firedThisHold) return false;
    if (!SessionLive()) return false;
    const uint32_t mode = GetGameMode();
    if (mode != MODE_MATCH && mode != MODE_CHARSEL) return false;
    if (mode == MODE_MATCH) {
        // Report the CONFIRMED progress, not the local key clock: what matters
        // to the player is how far the agreed stream has counted.
        const int f = s_holdFramesP1 > s_holdFramesP2 ? s_holdFramesP1 : s_holdFramesP2;
        if (out01) {
            float p = (float)f / (float)kHoldFrames;
            *out01 = p > 1.0f ? 1.0f : p;
        }
        return f > 0;
    }
    const DWORD held = GetTickCount() - s_holdStart;
    if (out01) {
        float p = (float)held / (float)kHoldMs;
        *out01 = p > 1.0f ? 1.0f : p;
    }
    return true;
}

}  // namespace Net
