/**
 * Alice Senki 2 - Mode Ownership Layer
 *
 * Implements hooks into game mode transitions and vanilla netplay socket
 * lifecycle. Prevents the vanilla netplay path from interfering with
 * mod-owned networking.
 *
 * Hook points:
 *   MainMenuStateMachine (0x5FB200) - intercept vanilla "Network" option
 *   SetGameMode          (0x5D2EB0) - intercept MODE_LOBBY transitions
 *   NetInitHost          (0x5FB9E0) - block vanilla host socket init
 *   NetInitClient        (0x5FBBC0) - block vanilla client socket init
 *   NetCloseHost         (0x5FBBA0) - block vanilla host socket close
 *   NetCloseClient       (0x5FBCE0) - block vanilla client socket close
 */

#include "net/mode_ownership.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "ui/log_window.h"
#include "net/session_manager.h"
#include "net/match_lifecycle.h"
#include "net/netplay_menu_state.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <MinHook.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Forward declarations (circular dependency with menu controller)
// ============================================================================

namespace NetMenu {
    bool IsMenuActive();
    void HandleNetworkSelected();
    void HandleDisconnection(const char* reason);
    void HandlePostMatchReturn();
    void RenderFrame();
    void RenderMainMenuReturnFade();
}

// ============================================================================
// Constants
// ============================================================================

namespace {

constexpr uintptr_t ADDR_MAIN_MENU_STATE_MACHINE = 0x5FB200;
constexpr uintptr_t ADDR_SET_GAME_MODE           = 0x5D2EB0;

constexpr uintptr_t ADDR_TITLE_MENU_SELECTION = 0x8EA002;
constexpr uintptr_t ADDR_TITLE_MENU_BG_ACTIVE = 0x8EA00C;
constexpr uintptr_t ADDR_FADE_TIMER_LOCAL     = 0x816370;

} // anonymous namespace

// ============================================================================
// Function pointer types
// ============================================================================

typedef int  (__cdecl *SetGameMode_t)(int mode, char fade);
typedef char (__cdecl *MainMenuStateMachine_t)();
typedef int  (__cdecl *NetInitHost_t)(unsigned short listenPortHostOrder);
typedef int  (__cdecl *NetInitClient_t)(unsigned short port, char* ipAddress);
typedef int  (__cdecl *NetCloseSocket_t)();

// ============================================================================
// Static state
// ============================================================================

namespace {

static MainMenuStateMachine_t s_origMainMenuStateMachine = nullptr;
static SetGameMode_t          s_origSetGameMode          = nullptr;
static NetInitHost_t          s_origNetInitHost           = nullptr;
static NetInitClient_t        s_origNetInitClient         = nullptr;
static NetCloseSocket_t       s_origNetCloseHost          = nullptr;
static NetCloseSocket_t       s_origNetCloseClient        = nullptr;

static bool     s_installed        = false;
static bool     s_interceptEnabled = true;
static bool     s_pendingMenuRestore = false;
static uint32_t s_lastStableGameType = GAMETYPE_ARCADE;

// Pre-open main-menu fade-out: when Network is selected we ramp the game's
// global darkness (dword_816370, applied by sub_488DC0) over the still-drawn
// vanilla menu before opening the net menu, so the menu fades out instead of
// cutting straight to black. 0 = inactive, 1..kOpenFadeOutFrames = ramping.
static constexpr int kOpenFadeOutFrames = 25;
static int s_pendingOpenFade = 0;

// ============================================================================
// Helpers
// ============================================================================

static uint8_t  ReadU8(uintptr_t a, uint8_t d = 0)  { __try { return *(volatile uint8_t*)a;  } __except(EXCEPTION_EXECUTE_HANDLER) { return d; } }
static uint32_t ReadU32(uintptr_t a, uint32_t d = 0) { __try { return *(volatile uint32_t*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return d; } }
static void WriteU8(uintptr_t a, uint8_t v)   { __try { *(volatile uint8_t*)a = v;  } __except(EXCEPTION_EXECUTE_HANDLER) {} }
static void WriteU32(uintptr_t a, uint32_t v)  { __try { *(volatile uint32_t*)a = v; } __except(EXCEPTION_EXECUTE_HANDLER) {} }

static bool ConfirmPressed() {
    return InputSystem_JustPressed(0, INPUT_A) || InputSystem_JustPressed(0, INPUT_START);
}

// ============================================================================
// Hook: MainMenuStateMachine (0x5FB200)
// ============================================================================

static char __cdecl Hook_MainMenuStateMachine() {
    // Pending menu restore: game needs to run sub=0 to load background sprites
    // before we can safely jump to sub=3 where our overlay draws.
    if (s_pendingMenuRestore) {
        const uint32_t sub = GetSubstate();
        if (sub < 1) {
            const char result = s_origMainMenuStateMachine ? s_origMainMenuStateMachine() : 0;
            if (GetSubstate() >= 1) {
                LOG_NETPLAY(LOG_INFO, "[ModeOwn] Pending restore: assets loaded (sub=%u), forcing sub=3", GetSubstate());
                WriteU32(ADDR_SUB_STATE, 3);
                WriteU32(ADDR_SUB_STATE_TIMER, 0);
                WriteU32(ADDR_FADE_TIMER_LOCAL, 0);
                s_pendingMenuRestore = false;
            }
            return (char)ReadU32(ADDR_SUB_STATE, sub);
        } else {
            LOG_NETPLAY(LOG_INFO, "[ModeOwn] Pending restore: assets ready (sub=%u), forcing sub=3", sub);
            WriteU32(ADDR_SUB_STATE, 3);
            WriteU32(ADDR_SUB_STATE_TIMER, 0);
            WriteU32(ADDR_FADE_TIMER_LOCAL, 0);
            s_pendingMenuRestore = false;
        }
    }

    const bool menuActive = NetMenu::IsMenuActive();

    if (!menuActive) {
        // Pre-open fade-out in progress: keep drawing the vanilla menu (pinned at
        // sub=3 so it can't advance) while ramping the global darkness, then open
        // the net menu once the screen is fully black.
        if (s_pendingOpenFade > 0) {
            WriteU32(ADDR_SUB_STATE, 3);
            if (s_origMainMenuStateMachine) s_origMainMenuStateMachine();
            WriteU32(ADDR_SUB_STATE, 3);
            WriteU32(ADDR_SUB_STATE_TIMER, 0);

            if (s_pendingOpenFade > kOpenFadeOutFrames) {
                s_pendingOpenFade = 0;
                LOG_NETPLAY(LOG_INFO, "[ModeOwn] Main-menu fade-out complete — opening net menu");
                NetMenu::HandleNetworkSelected();
                ModeOwnership::SanitizeOwnedGameType(MODE_MENU, false, "open-after-fadeout");
                NetMenu::RenderFrame();
                return (char)ReadU32(ADDR_SUB_STATE, 3);
            }

            WriteU32(ADDR_FADE_TIMER_LOCAL, (uint32_t)s_pendingOpenFade);
            ++s_pendingOpenFade;
            return (char)3;
        }

        // Not in custom menu — detect vanilla "Network" selection (option index 3)
        const uint32_t mode = GetGameMode();
        const uint32_t sub  = GetSubstate();
        const uint8_t  sel  = ReadU8(ADDR_TITLE_MENU_SELECTION, 0xFF);

        if (s_interceptEnabled && mode == MODE_MENU && sub == 3 && sel == 3 && ConfirmPressed()) {
            LOG_NETPLAY(LOG_INFO, "[ModeOwn] Intercepted Network selection — starting main-menu fade-out");
            // Begin the native fade-out: let vanilla draw (and play its confirm
            // SFX) this frame, but pin sub=3 and reset darkness to fully visible.
            WriteU32(ADDR_SUB_STATE, 3);
            if (s_origMainMenuStateMachine) s_origMainMenuStateMachine();
            WriteU32(ADDR_SUB_STATE, 3);
            WriteU32(ADDR_SUB_STATE_TIMER, 0);
            WriteU32(ADDR_FADE_TIMER_LOCAL, 0);
            s_pendingOpenFade = 1;
            return (char)3;
        }

        // Let vanilla handler run normally
        const char result = s_origMainMenuStateMachine ? s_origMainMenuStateMachine() : 0;

        // If the menu opened via SetGameMode intercept (the vanilla handler
        // saw a MODE_LOBBY transition), re-sanitize immediately and render.
        if (NetMenu::IsMenuActive()) {
            ModeOwnership::SanitizeOwnedGameType(MODE_MENU, false, "main-menu-post-open");
            NetMenu::RenderFrame();
            return (char)ReadU32(ADDR_SUB_STATE, 3);
        }

        // Just closed the custom menu: fade the vanilla main menu back in over
        // the black the net menu faded out to (no-op when no fade is pending).
        NetMenu::RenderMainMenuReturnFade();
        return result;
    }

    // Custom menu is active: keep game parked at sub=3, don't run vanilla logic
    WriteU32(ADDR_SUB_STATE, 3);
    WriteU32(ADDR_SUB_STATE_TIMER, 0);
    ModeOwnership::SanitizeOwnedGameType(MODE_MENU, false, "main-menu-visible");

    // Render the menu overlay inside the game's render pass
    NetMenu::RenderFrame();

    return (char)ReadU32(ADDR_SUB_STATE, 3);
}

// ============================================================================
// Hook: SetGameMode (0x5D2EB0)
// ============================================================================

static int __cdecl Hook_SetGameMode(int mode, char fade) {
    uint32_t sourceMode = GetGameMode();

    // Check if a session is active
    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);
    bool hasSession = snap.active && snap.state != Net::SessionState::Failed;

    // Intercept ALL transitions to MODE_LOBBY
    if (s_interceptEnabled && mode == MODE_LOBBY) {
        if (sourceMode == MODE_MENU) {
            LOG_NETPLAY(LOG_INFO, "[ModeOwn] Intercepted MODE_MENU -> MODE_LOBBY");
            NetMenu::HandleNetworkSelected();
            return 0;
        } else {
            LOG_NETPLAY(LOG_WARNING, "[ModeOwn] Intercepted mode %u -> MODE_LOBBY (vanilla fallback)", sourceMode);
            NetMenu::HandleDisconnection("Connection lost (vanilla lobby redirect intercepted)");
            return 0;
        }
    }

    // Graceful quit from CharSel with active session
    if (s_interceptEnabled && mode == MODE_MENU && sourceMode == MODE_CHARSEL && hasSession) {
        LOG_NETPLAY(LOG_INFO, "[ModeOwn] Intercepted CharSel -> MODE_MENU (player quit)");
        NetMenu::HandleDisconnection("Player quit from character select.");
        return 0;
    }

    // Match end route interception: when Mode 8 ends with active session,
    // intercept the vanilla route and go through PostMatch flow instead.
    if (s_interceptEnabled && sourceMode == MODE_MATCH && hasSession &&
        Net::MatchLifecycle_IsMatchOwned()) {
        if (mode == MODE_CHARSEL || mode == MODE_MENU) {
            LOG_NETPLAY(LOG_INFO, "[ModeOwn] Intercepted match end route (mode %u -> %u) — entering PostMatch",
                sourceMode, mode);
            // Let the actual mode transition happen so the game cleans up Mode 8,
            // but force the menu controller into PostMatch state.
            int result = s_origSetGameMode ? s_origSetGameMode(mode, fade) : 0;
            // Sanitize immediately after transition
            WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
            ModeOwnership::ClearVanillaNetplayFlags();
            // Signal PostMatch to menu controller
            NetMenu::HandlePostMatchReturn();
            return result;
        }
    }

    // Character swap is NOT needed with input-driven lockstep.
    // During charsel lockstep, Host controls P1 and Join controls P2 on BOTH
    // machines, so character assignments are already consistent.
    // The old state-driven charsel sync needed this swap because both peers
    // played as P1 locally — lockstep eliminates that.
    if (s_interceptEnabled && sourceMode == MODE_CHARSEL && hasSession &&
        (mode == MODE_PREMATCH_INTRO || mode == MODE_MATCH)) {
        Net::SessionRole role = Net::Session_GetRole();
        LOG_NETPLAY(LOG_DEBUG, "[ModeOwn] CharSel->Mode%u (role=%s) — lockstep, no swap needed",
            mode, (role == Net::SessionRole::Host) ? "Host" : "Join");
    }

    LOG_NETPLAY(LOG_DEBUG, "[ModeOwn] SetGameMode(%u, fade=%d) from mode=%u — passthrough", mode, fade ? 1 : 0, sourceMode);
    return s_origSetGameMode ? s_origSetGameMode(mode, fade) : 0;
}

// ============================================================================
// Hook: Socket lifecycle (block vanilla netplay)
// ============================================================================

static int __cdecl Hook_NetInitHost(unsigned short port) {
    if (ModeOwnership::ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING, "[ModeOwn] Blocked vanilla host socket init on port %u", (unsigned)port);
        return -1;
    }
    return s_origNetInitHost ? s_origNetInitHost(port) : -1;
}

static int __cdecl Hook_NetInitClient(unsigned short port, char* ip) {
    if (ModeOwnership::ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING, "[ModeOwn] Blocked vanilla client socket init to %s:%u", ip ? ip : "(null)", (unsigned)port);
        return -1;
    }
    return s_origNetInitClient ? s_origNetInitClient(port, ip) : -1;
}

static int __cdecl Hook_NetCloseHost() {
    if (ModeOwnership::ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING, "[ModeOwn] Blocked vanilla host socket close");
        return 0;
    }
    return s_origNetCloseHost ? s_origNetCloseHost() : -1;
}

static int __cdecl Hook_NetCloseClient() {
    if (ModeOwnership::ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING, "[ModeOwn] Blocked vanilla client socket close");
        return 0;
    }
    return s_origNetCloseClient ? s_origNetCloseClient() : -1;
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace ModeOwnership {

bool Install() {
    if (s_installed) return true;

    s_lastStableGameType = (GetGameType() == GAMETYPE_NETPLAY) ? GAMETYPE_VS_HUMAN : GetGameType();

    struct HookEntry {
        uintptr_t target;
        void*     detour;
        void**    original;
        const char* name;
    };

    HookEntry hooks[] = {
        { ADDR_MAIN_MENU_STATE_MACHINE, (void*)&Hook_MainMenuStateMachine, (void**)&s_origMainMenuStateMachine, "MainMenuStateMachine" },
        { ADDR_SET_GAME_MODE,           (void*)&Hook_SetGameMode,          (void**)&s_origSetGameMode,          "SetGameMode"          },
        { ADDR_NET_INIT_HOST,           (void*)&Hook_NetInitHost,          (void**)&s_origNetInitHost,          "NetInitHost"          },
        { ADDR_NET_INIT_CLIENT,         (void*)&Hook_NetInitClient,        (void**)&s_origNetInitClient,        "NetInitClient"        },
        { ADDR_NET_CLOSE_HOST,          (void*)&Hook_NetCloseHost,         (void**)&s_origNetCloseHost,         "NetCloseHost"         },
        { ADDR_NET_CLOSE_CLIENT,        (void*)&Hook_NetCloseClient,       (void**)&s_origNetCloseClient,       "NetCloseClient"       },
    };

    constexpr int hookCount = sizeof(hooks) / sizeof(hooks[0]);

    for (int i = 0; i < hookCount; ++i) {
        MH_STATUS st = MH_CreateHook((LPVOID)hooks[i].target, hooks[i].detour, hooks[i].original);
        if (st != MH_OK) {
            LOG_NETPLAY(LOG_ERROR, "[ModeOwn] Failed to create hook %s: %d", hooks[i].name, st);
            // Rollback installed hooks
            for (int j = i - 1; j >= 0; --j) {
                MH_DisableHook((LPVOID)hooks[j].target);
                MH_RemoveHook((LPVOID)hooks[j].target);
            }
            return false;
        }
        st = MH_EnableHook((LPVOID)hooks[i].target);
        if (st != MH_OK) {
            LOG_NETPLAY(LOG_ERROR, "[ModeOwn] Failed to enable hook %s: %d", hooks[i].name, st);
            MH_RemoveHook((LPVOID)hooks[i].target);
            for (int j = i - 1; j >= 0; --j) {
                MH_DisableHook((LPVOID)hooks[j].target);
                MH_RemoveHook((LPVOID)hooks[j].target);
            }
            return false;
        }
    }

    s_installed = true;
    LOG_NETPLAY(LOG_INFO, "[ModeOwn] Installed %d mode/socket ownership hooks", hookCount);
    return true;
}

void Remove() {
    if (!s_installed) return;

    uintptr_t targets[] = {
        ADDR_NET_CLOSE_CLIENT,
        ADDR_NET_CLOSE_HOST,
        ADDR_NET_INIT_CLIENT,
        ADDR_NET_INIT_HOST,
        ADDR_SET_GAME_MODE,
        ADDR_MAIN_MENU_STATE_MACHINE,
    };
    for (auto t : targets) {
        MH_DisableHook((LPVOID)t);
        MH_RemoveHook((LPVOID)t);
    }

    s_installed = false;
    s_pendingMenuRestore = false;
    LOG_NETPLAY(LOG_INFO, "[ModeOwn] Removed all mode/socket ownership hooks");
}

void FrameUpdate() {
    uint32_t type = GetGameType();
    if (type != GAMETYPE_NETPLAY) {
        s_lastStableGameType = type;
    }

    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);
    bool sessionActive = snap.active && snap.state != Net::SessionState::Failed;

    SanitizeOwnedGameType(GetGameMode(), sessionActive, "frame-update");

    // Handle pending menu restore
    if (s_pendingMenuRestore && GetGameMode() == MODE_MENU) {
        uint32_t sub = GetSubstate();
        if (sub >= 1) {
            LOG_NETPLAY(LOG_INFO, "[ModeOwn] Pending menu restore complete: sub=%u -> forcing sub=3", sub);
            WriteU32(ADDR_SUB_STATE, 3);
            WriteU32(ADDR_SUB_STATE_TIMER, 0);
            WriteU32(ADDR_FADE_TIMER_LOCAL, 0);
            s_pendingMenuRestore = false;
        }
    }
}

void ClearVanillaNetplayFlags() {
    WriteU8(ADDR_NETPLAY_ROLE, 0);
    WriteU8(ADDR_NETPLAY_CONNECTED, 0);
}

bool ShouldBlockVanillaSocketLifecycle() {
    if (NetMenu::IsMenuActive()) return true;

    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);
    return snap.active || snap.state == Net::SessionState::Failed;
}

void SanitizeOwnedGameType(uint32_t mode, bool sessionActive, const char* reason) {
    uint32_t desiredType = GetGameType();
    bool shouldSanitize = false;

    if (NetMenu::IsMenuActive() && mode == MODE_MENU) {
        desiredType = GetSafeMenuGameType();
        shouldSanitize = true;
    } else if (sessionActive && (mode == MODE_CHARSEL || mode == MODE_MATCH || mode == MODE_MENU)) {
        desiredType = GAMETYPE_VS_HUMAN;
        shouldSanitize = true;
    }

    if (!shouldSanitize) return;

    uint32_t currentType = GetGameType();
    if (currentType != desiredType) {
        WriteU32(ADDR_GAME_TYPE, desiredType);
        LOG_NETPLAY(LOG_WARNING,
            "[ModeOwn] Sanitized game type %u -> %u (%s, mode=%u)",
            currentType, desiredType, reason ? reason : "?", mode);
    }
    ClearVanillaNetplayFlags();
}

void EnterCustomMenuContext() {
    uint32_t currentMode = GetGameMode();
    if (currentMode != MODE_MENU) {
        LOG_NETPLAY(LOG_INFO, "[ModeOwn] EnterCustomMenuContext: SetGameMode from mode %u", currentMode);
        CallOriginalSetGameMode(MODE_MENU, 0);
        s_pendingMenuRestore = true;
    } else {
        WriteU32(ADDR_SUB_STATE, 3);
        WriteU32(ADDR_SUB_STATE_TIMER, 0);
        WriteU32(ADDR_FADE_TIMER_LOCAL, 0);
        s_pendingMenuRestore = false;
    }

    WriteU32(ADDR_GAME_TYPE, GetSafeMenuGameType());
    WriteU8(ADDR_TITLE_MENU_SELECTION, 3);
    ClearVanillaNetplayFlags();
    WriteU8(ADDR_P1_CPU_FLAG, 0);
    WriteU8(ADDR_P2_CPU_FLAG, 0);
    ResetCharSelFields();
}

void RestoreMainMenuContext() {
    WriteU32(ADDR_GAME_MODE, MODE_MENU);
    WriteU32(ADDR_SUB_STATE, 3);
    WriteU32(ADDR_SUB_STATE_TIMER, 0);
    WriteU32(ADDR_FADE_TIMER_LOCAL, 0);
    WriteU32(ADDR_GAME_TYPE, GetSafeMenuGameType());
    WriteU8(ADDR_TITLE_MENU_SELECTION, 3);
    ClearVanillaNetplayFlags();
}

bool IsPendingMenuRestore() {
    return s_pendingMenuRestore;
}

void SetPendingMenuRestore(bool pending) {
    s_pendingMenuRestore = pending;
}

int CallOriginalSetGameMode(int mode, char fade) {
    if (s_origSetGameMode) return s_origSetGameMode(mode, fade);
    return ((SetGameMode_t)ADDR_SET_GAME_MODE)(mode, fade);
}

uint32_t GetSafeMenuGameType() {
    uint32_t type = s_lastStableGameType;
    if (type == GAMETYPE_NETPLAY) type = GAMETYPE_VS_HUMAN;
    return type;
}

void TrackGameType(uint32_t type) {
    if (type != GAMETYPE_NETPLAY) {
        s_lastStableGameType = type;
    }
}

uint32_t GetLastStableGameType() {
    return s_lastStableGameType;
}

void ResetCharSelFields() {
    // Reset charsel state addresses to safe defaults
    WriteU32(ADDR_CHARSEL_P1_CHAR_ID, 0);
    WriteU32(ADDR_CHARSEL_P2_CHAR_ID, 0);

    // Reset cursor positions: P1 at grid 0 (top-left), P2 at grid 2 (top-right)
    WriteU8(ADDR_CHARSEL_P1_CURSOR, 0);
    WriteU8(ADDR_CHARSEL_P1_CONFIRM, 0);
    WriteU8(ADDR_CHARSEL_P1_AGE, 0);
    WriteU8(ADDR_CHARSEL_P2_CURSOR, 2);
    WriteU8(ADDR_CHARSEL_P2_CONFIRM, 0);
    WriteU8(ADDR_CHARSEL_P2_AGE, 0);

    // Reset stage cursor to prevent desync from stale prior values
    WriteU8(ADDR_CHARSEL_STAGE_ID, 0);
}

} // namespace ModeOwnership
