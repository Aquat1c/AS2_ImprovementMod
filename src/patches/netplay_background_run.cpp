/**
 * Netplay background-run override
 *
 * Vanilla DXLib main-loop idle (sub_634910) blocks the entire game thread when
 * the window is inactive (uiParam == 0):
 *
 *   while ( !uiParam && !sub_634550() ) { ; }
 *
 * That freeze is harmless offline but breaks netplay: rollback's worker thread
 * keeps receiving packets while the game thread stalls, and post-match lockstep
 * (win screen, charsel) cannot advance until both windows regain focus.
 *
 * While a netplay session is connected we force the same "run in background"
 * state the engine already supports (uiParam + g_bRunInBackground) immediately
 * before the idle pump runs. Rollback gameplay is unaffected — this only prevents
 * the inactive-window stall from touching session-owned phases.
 */

#include "patches/netplay_background_run.h"

#include "as2_constants.h"
#include "core/game_state.h"
#include "net/session_manager.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include "MinHook.h"

#include <stdint.h>
#include <windows.h>

namespace {

using MessagePumpIdle_t = int (__cdecl*)();

MessagePumpIdle_t g_origMessagePumpIdle = nullptr;

static bool s_overrideActive = false;
static bool s_wasSessionConnected = false;
static uint32_t s_savedRunInBackground = 0;
static bool s_loggedEngage = false;
static bool s_loggedRelease = false;

static uint32_t ReadU32(uintptr_t addr, uint32_t fallback = 0) {
    __try {
        return *(volatile uint32_t*)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static void WriteU32(uintptr_t addr, uint32_t value) {
    __try {
        *(volatile uint32_t*)addr = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void UpdateSessionOverrideState(bool sessionConnected) {
    if (sessionConnected && !s_wasSessionConnected) {
        s_savedRunInBackground = ReadU32(ADDR_GAME_RUN_IN_BACKGROUND, 0);
        s_overrideActive = true;
        s_loggedEngage = false;
        s_loggedRelease = false;
        Rollback::NetplayLog_Write(
            "BGRUN", -1,
            "Netplay background-run override engaged (saved run_in_background=%u mode=%u sub=%u)",
            s_savedRunInBackground,
            GetGameMode(),
            GetSubstate());
        LOG_NETPLAY(LOG_INFO,
            "[BgRun] Engaged inactive-window bypass for connected session (saved run_in_background=%u)",
            s_savedRunInBackground);
    } else if (!sessionConnected && s_wasSessionConnected) {
        if (s_overrideActive) {
            WriteU32(ADDR_GAME_RUN_IN_BACKGROUND, s_savedRunInBackground);
            s_overrideActive = false;
            if (!s_loggedRelease) {
                s_loggedRelease = true;
                Rollback::NetplayLog_Write(
                    "BGRUN", -1,
                    "Netplay background-run override released (restored run_in_background=%u)",
                    s_savedRunInBackground);
                LOG_NETPLAY(LOG_INFO,
                    "[BgRun] Released inactive-window bypass (restored run_in_background=%u)",
                    s_savedRunInBackground);
            }
        }
    }
    s_wasSessionConnected = sessionConnected;
}

static void ApplyBackgroundRunOverride() {
    if (!Net::Session_IsConnected()) {
        UpdateSessionOverrideState(false);
        return;
    }

    UpdateSessionOverrideState(true);

    const uint32_t prevActive = ReadU32(ADDR_GAME_WINDOW_ACTIVE, 0);
    WriteU32(ADDR_GAME_WINDOW_ACTIVE, 1);
    WriteU32(ADDR_GAME_RUN_IN_BACKGROUND, 1);

    if (!s_loggedEngage && prevActive == 0) {
        s_loggedEngage = true;
        Rollback::NetplayLog_Write(
            "BGRUN", -1,
            "Prevented inactive-window main-loop stall (uiParam %u -> 1, run_in_background=1 mode=%u sub=%u)",
            prevActive,
            GetGameMode(),
            GetSubstate());
    }
}

static int __cdecl Hook_MessagePumpIdle() {
    ApplyBackgroundRunOverride();
    return g_origMessagePumpIdle ? g_origMessagePumpIdle() : 0;
}

} // namespace

namespace NetplayBackgroundRun {

void Init() {
    // Hook installed from hook_installer.cpp after MinHook init.
}

void Shutdown() {
    if (s_overrideActive) {
        WriteU32(ADDR_GAME_RUN_IN_BACKGROUND, s_savedRunInBackground);
        s_overrideActive = false;
    }
    s_wasSessionConnected = false;
    g_origMessagePumpIdle = nullptr;
}

bool InstallHook() {
    const MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_GAME_MESSAGE_PUMP_IDLE),
        reinterpret_cast<void*>(&Hook_MessagePumpIdle),
        reinterpret_cast<void**>(&g_origMessagePumpIdle));
    if (status != MH_OK) {
        LOG_ERROR("[BgRun] Failed to hook sub_634910 (status=%d)", status);
        return false;
    }
    LOG_INFO("[BgRun] Hooked sub_634910 inactive-window idle pump bypass");
    return true;
}

} // namespace NetplayBackgroundRun
