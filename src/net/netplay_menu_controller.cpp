/**
 * Alice Senki 2 - Netplay Menu Controller
 *
 * Menu state machine, navigation input, session integration, and launch
 * handoff. The menu controller owns the high-level flow; ModeOwnership
 * owns the hook points and vanilla suppression.
 */

#include "net/netplay_menu_controller.h"
#include "net/mode_ownership.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/netplay_menu_ui.h"
#include "net/pregame_sync.h"
#include "net/match_lifecycle.h"
#include "net/sync_policy.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Internal state
// ============================================================================

namespace {

using namespace NetMenu;

constexpr int kFadeFrames = 25;

static bool          s_initialized       = false;
static MenuState     s_state             = MenuState::Inactive;
static MenuPhase     s_phase             = MenuPhase::Hidden;
static RootBranch    s_activeBranch      = RootBranch::DirectPlay;
static SettingsCategory s_settingsCategory = SettingsCategory::Identity;
static TextEditField s_textEditField     = TextEditField::None;
static uint32_t      s_selectedIndex     = 0;
static int           s_fadeFrames        = 0;
static bool          s_captureInput      = false;
static bool          s_waitForNeutral    = false;
static char          s_status[128]       = "Waiting for network menu selection.";
static char          s_lastError[128]    = "";
static char          s_textEditBuffer[64] = "";

// ============================================================================
// Memory helpers
// ============================================================================

static uint8_t  ReadU8(uintptr_t a, uint8_t d = 0)   { __try { return *(volatile uint8_t*)a;  } __except(EXCEPTION_EXECUTE_HANDLER) { return d; } }
static void WriteU8(uintptr_t a, uint8_t v)   { __try { *(volatile uint8_t*)a = v;  } __except(EXCEPTION_EXECUTE_HANDLER) {} }
static void WriteU32(uintptr_t a, uint32_t v)  { __try { *(volatile uint32_t*)a = v; } __except(EXCEPTION_EXECUTE_HANDLER) {} }

// ============================================================================
// String helpers
// ============================================================================

static void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy_s(dst, cap, src, _TRUNCATE);
}

template <typename... Args>
static void SetStatus(const char* fmt, Args... args) {
    _snprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, args...);
}

template <typename... Args>
static void SetError(const char* fmt, Args... args) {
    _snprintf_s(s_lastError, sizeof(s_lastError), _TRUNCATE, fmt, args...);
}

static void ClearError() { s_lastError[0] = '\0'; }

// ============================================================================
// Menu visibility
// ============================================================================

static bool MenuVisible() { return s_phase != MenuPhase::Hidden; }
static bool IsTextEditing() { return s_textEditField != TextEditField::None; }

// ============================================================================
// State transitions
// ============================================================================

static void TransitionTo(MenuState next, const char* why) {
    if (s_state == next) return;
    LOG_NETPLAY(LOG_INFO, "[NetMenu] State %s -> %s (%s)",
        MenuStateName(s_state), MenuStateName(next), why ? why : "?");
    s_state = next;
}

static void ClearTextEditState() {
    s_textEditField = TextEditField::None;
    s_textEditBuffer[0] = '\0';
}

static void ResetMenuInputState() {
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    ClearTextEditState();
    InputSystem_ResetRepeatState(0);
}

// ============================================================================
// Menu open / close
// ============================================================================

static void OpenMenu() {
    ModeOwnership::EnterCustomMenuContext();
    s_activeBranch = RootBranch::DirectPlay;
    s_settingsCategory = SettingsCategory::Identity;
    s_phase = MenuPhase::Opening;
    s_fadeFrames = 0;
    s_captureInput = true;
    ResetMenuInputState();
    ClearError();
    SetStatus("Opening custom netplay menu.");
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Opening custom netplay menu");
    TransitionTo(MenuState::MenuRoot, "Network selected");
}

static void FinishClose() {
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    ClearTextEditState();
    s_waitForNeutral = true;
    s_selectedIndex = 0;
    SetStatus("Waiting for network menu selection.");
    TransitionTo(MenuState::Inactive, "menu closed");
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Custom netplay menu closed");
    InputSystem_ResetRepeatState(0);
}

static void BeginClose(const char* why) {
    if (!MenuVisible() || s_phase == MenuPhase::Closing) return;
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Closing custom menu (%s)", why ? why : "?");
    SetStatus("Closing custom netplay menu.");
    s_phase = MenuPhase::Closing;
    s_waitForNeutral = true;
    ClearTextEditState();
}

// ============================================================================
// Disconnect / error
// ============================================================================

static void OpenDisconnectError(const char* why) {
    uint32_t currentMode = GetGameMode();
    LOG_NETPLAY(LOG_WARNING, "[NetMenu] OpenDisconnectError: reason='%s' mode=%u", why ? why : "?", currentMode);

    // Notify match lifecycle layer of disconnect
    if (Net::MatchLifecycle_IsMatchOwned()) {
        Net::MatchLifecycle_OnDisconnect(why ? why : "Disconnected");
    }

    // Abort any in-progress pre-game sync
    Net::PregameSync_Abort(why ? why : "Disconnected");

    // Clean vanilla netplay flags
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ModeOwnership::ClearVanillaNetplayFlags();

    // Force back to menu if not already there
    if (currentMode != MODE_MENU) {
        LOG_NETPLAY(LOG_WARNING, "[NetMenu] Forcing return to menu from mode %u", currentMode);
        ModeOwnership::EnterCustomMenuContext();
    }

    Net::Session_Cancel();

    ClearTextEditState();
    SetError("%s", why ? why : "Disconnected.");
    SetStatus("%s", why ? why : "Disconnected.");
    s_phase = MenuPhase::Active;
    s_fadeFrames = kFadeFrames;
    s_captureInput = true;
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    TransitionTo(MenuState::DisconnectError, "disconnect");
}

// ============================================================================
// Session state sync
// ============================================================================

static void SyncSessionState() {
    // Always pump session - keeps connections alive during gameplay
    Net::Session_Update();

    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);

    // Map session state to menu state when menu is active
    if (!MenuVisible()) return;

    switch (snap.state) {
        case Net::SessionState::Idle:
            break;
        case Net::SessionState::Connecting:
            if (s_state != MenuState::Connecting && s_state != MenuState::Handshake) {
                TransitionTo(MenuState::Connecting, "session connecting");
                s_selectedIndex = 0;
            }
            break;
        case Net::SessionState::Handshaking:
            if (s_state != MenuState::Handshake) {
                TransitionTo(MenuState::Handshake, "session handshaking");
                s_selectedIndex = 0;
            }
            break;
        case Net::SessionState::Connected:
        case Net::SessionState::Ready:
            if (s_state != MenuState::ConnectedSession && s_state != MenuState::CharSelTransition) {
                s_activeBranch = RootBranch::DirectPlay;
                TransitionTo(MenuState::ConnectedSession, "session connected");
                s_selectedIndex = 0;
            }
            break;
        case Net::SessionState::Failed:
            if (s_state != MenuState::DisconnectError) {
                OpenDisconnectError(snap.error_text[0] ? snap.error_text : "Session error.");
            }
            break;
        default:
            break;
    }
}

// ============================================================================
// Launch helpers
// ============================================================================

static void HideMenuForLaunch(const char* why) {
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Hiding menu for launch (%s)", why ? why : "?");
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    ClearTextEditState();
    s_waitForNeutral = false;
    s_selectedIndex = 0;
    ClearError();
    SetStatus("Waiting for network menu selection.");
    TransitionTo(MenuState::Inactive, why ? why : "launch");
    InputSystem_ResetRepeatState(0);
}

static bool LaunchOfflineVsDebug() {
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Launching offline VS debug");

    Net::Session_Cancel();
    HideMenuForLaunch("offline VS debug");
    ModeOwnership::SetPendingMenuRestore(false);

    // Set up VS Human (2P local) mode
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ModeOwnership::ClearVanillaNetplayFlags();
    WriteU8(ADDR_P1_CPU_FLAG, 0);
    WriteU8(ADDR_P2_CPU_FLAG, 0);
    WriteU8(ADDR_CHARSEL_ENABLE, 1);

    int result = ModeOwnership::CallOriginalSetGameMode(MODE_CHARSEL, 1);
    ModeOwnership::ResetCharSelFields();

    LOG_NETPLAY(LOG_INFO, "[NetMenu] Offline VS debug launch: result=%d mode=%u sub=%u type=%u",
        result, GetGameMode(), GetSubstate(), GetGameType());
    return true;
}

static bool LaunchNetplayCharSel() {
    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);
    bool isHost = (snap.role == Net::SessionRole::Host);

    LOG_NETPLAY(LOG_INFO, "[NetMenu] Launching netplay CharSel (role=%s)", isHost ? "Host" : "Client");

    // Do NOT cancel session — keep it alive. Only hide the menu UI.
    HideMenuForLaunch("netplay charsel");

    // Use offline VS Human mode so vanilla netplay sync never activates.
    // The mod relays remote input via its own hooks.
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ModeOwnership::ClearVanillaNetplayFlags();
    WriteU8(ADDR_P1_CPU_FLAG, 0);
    WriteU8(ADDR_P2_CPU_FLAG, 0);
    WriteU8(ADDR_CHARSEL_ENABLE, 1);
    WriteU8(ADDR_STAGESEL_ENABLE, 1);

    // Enforce Stage Select via sync policy (will persist across mode changes)
    Net::SyncPolicy_EnforceStageSelectForNetplay();
    Net::SyncPolicy_ResetConfirmArm();

    int result = ModeOwnership::CallOriginalSetGameMode(MODE_CHARSEL, 1);
    ModeOwnership::ResetCharSelFields();
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);

    LOG_NETPLAY(LOG_INFO, "[NetMenu] Netplay CharSel launch: result=%d mode=%u sub=%u type=%u",
        result, GetGameMode(), GetSubstate(), GetGameType());

    // Start pre-game sync (CharSel lockstep → bootstrap → gameplay)
    if (!Net::PregameSync_Begin()) {
        LOG_NETPLAY(LOG_WARNING, "[NetMenu] Failed to start pre-game sync");
    }

    return true;
}

// ============================================================================
// Item counts
// ============================================================================

static int ItemCount(MenuState st) {
    switch (st) {
        case MenuState::MenuRoot:            return 3; // Direct Play, Settings, Close
        case MenuState::DirectConnectEntry:  return 3; // Host, Join, Back
        case MenuState::HostEntry:           return 3; // Host, Listen Port, Back
        case MenuState::JoinEntry:           return 3; // Join, Remote Endpoint, Back
        case MenuState::SettingsEntry:       return 3; // Delay, Verbose, Back
        case MenuState::Connecting:          return 1; // Cancel
        case MenuState::Handshake:           return 1; // Cancel
        case MenuState::ConnectedSession:    return 2; // Launch CharSel, Disconnect
        case MenuState::CharSelTransition:   return 2; // Launch, Back
        case MenuState::PostMatch:           return 3; // Rematch, Return, Disconnect
        case MenuState::DisconnectError:     return 2; // OK, Close Menu
        default: return 0;
    }
}

static void MoveSelection(int delta) {
    int count = ItemCount(s_state);
    if (count <= 0) { s_selectedIndex = 0; return; }
    int next = (int)s_selectedIndex + delta;
    while (next < 0) next += count;
    while (next >= count) next -= count;
    s_selectedIndex = (uint32_t)next;
}

// ============================================================================
// Input helpers
// ============================================================================

static bool MenuJustPressed(uint16_t button) { return InputSystem_JustPressed(0, button); }
static bool ConfirmPressed() { return MenuJustPressed(INPUT_A) || MenuJustPressed(INPUT_START); }
static bool BackPressed()    { return MenuJustPressed(INPUT_B) || MenuJustPressed(INPUT_SELECT); }

// ============================================================================
// Text editing (keyboard input for nickname/endpoint/port)
// ============================================================================

static void BeginTextEdit(TextEditField field, const char* initial, const char* status) {
    s_textEditField = field;
    CopyText(s_textEditBuffer, sizeof(s_textEditBuffer), initial ? initial : "");
    s_waitForNeutral = true;
    if (status && status[0]) SetStatus("%s", status);
}

static void FinishTextEdit(bool commit) {
    TextEditField field = s_textEditField;
    if (field == TextEditField::None) return;

    if (!commit) {
        ClearTextEditState();
        s_waitForNeutral = true;
        InputSystem_ResetRepeatState(0);
        return;
    }

    // TODO: Apply edits to config when config system is wired up
    ClearTextEditState();
    s_waitForNeutral = true;
    InputSystem_ResetRepeatState(0);
}

static void HandleTextEditing() {
    bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    static bool prevDown[256] = {};

    for (int vk = 0; vk < 256; ++vk) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !prevDown[vk]) {
            size_t len = strlen(s_textEditBuffer);
            if (vk == VK_RETURN) {
                FinishTextEdit(true);
            } else if (vk == VK_ESCAPE) {
                FinishTextEdit(false);
            } else if ((vk == VK_BACK || vk == VK_DELETE) && len > 0) {
                s_textEditBuffer[len - 1] = '\0';
            } else if (vk >= '0' && vk <= '9' && len + 1 < sizeof(s_textEditBuffer)) {
                s_textEditBuffer[len] = (char)vk;
                s_textEditBuffer[len + 1] = '\0';
            } else if (vk >= 'A' && vk <= 'Z' && len + 1 < sizeof(s_textEditBuffer)) {
                s_textEditBuffer[len] = (char)(shiftDown ? vk : (vk + 32));
                s_textEditBuffer[len + 1] = '\0';
            } else if ((vk == VK_OEM_PERIOD || vk == VK_DECIMAL) && len + 1 < sizeof(s_textEditBuffer)) {
                s_textEditBuffer[len] = '.';
                s_textEditBuffer[len + 1] = '\0';
            } else if (vk == VK_OEM_1 && len + 1 < sizeof(s_textEditBuffer)) {
                s_textEditBuffer[len] = ':';
                s_textEditBuffer[len + 1] = '\0';
            }
        }
        prevDown[vk] = down;
    }
}

// ============================================================================
// Navigation input (when not text editing)
// ============================================================================

static void ActivateCurrentSelection();
static void HandleBackNavigation();

static void HandleNavigationInput() {
    // Repeat-aware Up/Down
    if (InputSystem_JustPressed(0, INPUT_UP))   MoveSelection(-1);
    if (InputSystem_JustPressed(0, INPUT_DOWN))  MoveSelection(1);

    if (ConfirmPressed()) {
        ActivateCurrentSelection();
        return;
    }
    if (BackPressed()) {
        HandleBackNavigation();
        return;
    }
}

static void ActivateCurrentSelection() {
    switch (s_state) {
        case MenuState::MenuRoot:
            if (s_selectedIndex == 0) {
                s_activeBranch = RootBranch::DirectPlay;
                s_selectedIndex = 0;
                SetStatus("Direct Play ready.");
                TransitionTo(MenuState::DirectConnectEntry, "open direct connect");
            } else if (s_selectedIndex == 1) {
                s_activeBranch = RootBranch::Settings;
                s_selectedIndex = 0;
                SetStatus("Settings opened.");
                TransitionTo(MenuState::SettingsEntry, "open settings");
            } else {
                BeginClose("close from root");
            }
            break;

        case MenuState::DirectConnectEntry:
            if (s_selectedIndex == 0) {
                // Host
                s_selectedIndex = 0;
                SetStatus("Configure host settings.");
                TransitionTo(MenuState::HostEntry, "open host config");
            } else if (s_selectedIndex == 1) {
                // Join
                s_selectedIndex = 0;
                SetStatus("Configure join settings.");
                TransitionTo(MenuState::JoinEntry, "open join config");
            } else {
                // Back
                s_selectedIndex = 0;
                TransitionTo(MenuState::MenuRoot, "back from direct connect");
            }
            break;

        case MenuState::HostEntry:
            if (s_selectedIndex == 0) {
                // Start hosting
                Net::SessionConfig cfg{};
                Net::SessionConfig_SetDefaults(&cfg);
                cfg.listen_port = 10700; // TODO: from config
                strncpy_s(cfg.nickname, sizeof(cfg.nickname), "Host", _TRUNCATE);
                if (Net::Session_StartHost(&cfg)) {
                    SetStatus("Waiting for peer...");
                    TransitionTo(MenuState::Connecting, "host started");
                } else {
                    SetStatus("Failed to start host.");
                }
            } else if (s_selectedIndex == 1) {
                // Edit port
                BeginTextEdit(TextEditField::ListenPort, "10700", "Type port number (1-65535).");
            } else {
                s_selectedIndex = 0;
                TransitionTo(MenuState::DirectConnectEntry, "back from host");
            }
            break;

        case MenuState::JoinEntry:
            if (s_selectedIndex == 0) {
                // Join
                Net::SessionConfig cfg{};
                Net::SessionConfig_SetDefaults(&cfg);
                cfg.listen_port = 10700;
                cfg.target_ip = 0x0100007F; // 127.0.0.1 in network byte order
                cfg.target_port = 10700; // TODO: from config
                strncpy_s(cfg.nickname, sizeof(cfg.nickname), "Client", _TRUNCATE);
                if (Net::Session_StartJoin(&cfg)) {
                    SetStatus("Connecting to host...");
                    TransitionTo(MenuState::Connecting, "join started");
                } else {
                    SetStatus("Failed to start join.");
                }
            } else if (s_selectedIndex == 1) {
                // Edit endpoint
                BeginTextEdit(TextEditField::RemoteEndpoint, "127.0.0.1:10700", "Type ip:port.");
            } else {
                s_selectedIndex = 0;
                TransitionTo(MenuState::DirectConnectEntry, "back from join");
            }
            break;

        case MenuState::SettingsEntry:
            if (s_selectedIndex == 2) {
                // Back
                s_selectedIndex = 0;
                TransitionTo(MenuState::MenuRoot, "back from settings");
            }
            // Other settings items: adjust inline via left/right
            break;

        case MenuState::ConnectedSession:
            if (s_selectedIndex == 0) {
                // Launch CharSel
                LaunchNetplayCharSel();
            } else {
                // Disconnect
                Net::Session_Cancel();
                OpenDisconnectError("Session cancelled by user.");
            }
            break;

        case MenuState::CharSelTransition:
            if (s_selectedIndex == 0) {
                LaunchNetplayCharSel();
            } else {
                s_selectedIndex = 0;
                TransitionTo(MenuState::ConnectedSession, "back from charsel staging");
            }
            break;

        case MenuState::Connecting:
        case MenuState::Handshake:
            // Cancel
            Net::Session_Cancel();
            s_selectedIndex = 0;
            SetStatus("Back to direct connect.");
            TransitionTo(MenuState::DirectConnectEntry, "back from connection");
            break;

        case MenuState::PostMatch:
            if (s_selectedIndex == 0) {
                // Rematch
                Net::MatchLifecycle_OnRematch();
                LaunchNetplayCharSel();
            } else if (s_selectedIndex == 1) {
                // Return to menu
                Net::MatchLifecycle_OnReturnToSession();
                s_selectedIndex = 0;
                TransitionTo(MenuState::ConnectedSession, "post-match return");
            } else {
                // Disconnect
                Net::MatchLifecycle_OnDisconnect("Disconnected after match.");
                Net::Session_Cancel();
                OpenDisconnectError("Disconnected after match.");
            }
            break;

        case MenuState::DisconnectError:
            if (s_selectedIndex == 0) {
                // OK - return to root
                ClearError();
                s_selectedIndex = 0;
                TransitionTo(MenuState::MenuRoot, "ack disconnect");
            } else {
                BeginClose("close from disconnect");
            }
            break;

        default:
            break;
    }
}

static void HandleBackNavigation() {
    switch (s_state) {
        case MenuState::MenuRoot:
            BeginClose("back from root");
            break;
        case MenuState::DirectConnectEntry:
            s_selectedIndex = 0;
            TransitionTo(MenuState::MenuRoot, "back from direct connect");
            break;
        case MenuState::HostEntry:
            s_selectedIndex = 0;
            TransitionTo(MenuState::DirectConnectEntry, "back from host");
            break;
        case MenuState::JoinEntry:
            s_selectedIndex = 0;
            TransitionTo(MenuState::DirectConnectEntry, "back from join");
            break;
        case MenuState::SettingsCategoryMenu:
        case MenuState::SettingsEntry:
            s_selectedIndex = 0;
            TransitionTo(MenuState::MenuRoot, "back from settings");
            break;
        case MenuState::Connecting:
        case MenuState::Handshake:
            Net::Session_Cancel();
            s_selectedIndex = 0;
            SetStatus("Back to direct connect.");
            TransitionTo(MenuState::DirectConnectEntry, "cancel connection");
            break;
        case MenuState::ConnectedSession:
            Net::Session_Cancel();
            OpenDisconnectError("Session cancelled by user.");
            break;
        case MenuState::CharSelTransition:
            s_selectedIndex = 0;
            TransitionTo(MenuState::ConnectedSession, "back from charsel staging");
            break;
        case MenuState::DisconnectError:
            BeginClose("back from disconnect");
            break;
        default:
            break;
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace NetMenu {

void Init() {
    if (s_initialized) return;
    s_state = MenuState::Inactive;
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    s_initialized = true;
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Initialized");
}

void Shutdown() {
    if (!s_initialized) return;
    FinishClose();
    s_initialized = false;
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Shutdown");
}

void FrameUpdate() {
    uint32_t mode = GetGameMode();

    // ALWAYS pump the session — even when the menu is hidden (CharSel/Match).
    // This drives ENet polling, keepalive, and timeout detection.
    SyncSessionState();

    // Re-read mode after session sync (disconnect may have forced mode change)
    mode = GetGameMode();

    if (!MenuVisible()) return;

    // If we're no longer in MODE_MENU, close the menu
    if (mode != MODE_MENU) {
        LOG_NETPLAY(LOG_WARNING, "[NetMenu] Closing menu because mode changed to %u", mode);
        FinishClose();
        return;
    }

    // Fade transitions
    if (s_phase == MenuPhase::Opening) {
        if (++s_fadeFrames >= kFadeFrames) {
            s_fadeFrames = kFadeFrames;
            s_phase = MenuPhase::Active;
        }
    } else if (s_phase == MenuPhase::Closing) {
        if (--s_fadeFrames <= 0) {
            ModeOwnership::RestoreMainMenuContext();
            FinishClose();
            return;
        }
    }

    // Wait for all inputs to be released before processing
    if (s_waitForNeutral) {
        uint16_t held = InputSystem_GetInput(0);
        if ((held & (INPUT_ANY_DIR | INPUT_A | INPUT_B | INPUT_START | INPUT_SELECT)) == 0) {
            s_waitForNeutral = false;
            InputSystem_ResetRepeatState(0);
        }
        return;
    }

    if (s_phase != MenuPhase::Active) return;

    // Handle input
    if (IsTextEditing()) {
        HandleTextEditing();
    } else {
        HandleNavigationInput();
    }

    // Render the in-game menu
    MenuSnapshot snap{};
    GetSnapshot(&snap);
    NetMenuUI::Render(&snap);
}

void HandleNetworkSelected() {
    OpenMenu();
}

void HandleDisconnection(const char* reason) {
    OpenDisconnectError(reason);
}

void HandlePostMatchReturn() {
    LOG_NETPLAY(LOG_INFO, "[NetMenu] HandlePostMatchReturn — showing post-match menu");

    // Ensure we're in a menu-visible state for the PostMatch options
    ModeOwnership::EnterCustomMenuContext();

    // Open the menu at PostMatch state
    s_phase = MenuPhase::Active;
    s_fadeFrames = kFadeFrames;
    s_captureInput = true;
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    ClearError();
    SetStatus("Match complete. Choose next action.");
    TransitionTo(MenuState::PostMatch, "match ended");
}

bool IsMenuActive() {
    return MenuVisible();
}

bool ConsumesGameInput() {
    return s_captureInput;
}

void GetSnapshot(MenuSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->menu_active = MenuVisible();
    out->captures_input = s_captureInput;
    out->state = s_state;
    out->phase = s_phase;
    out->root_branch = s_activeBranch;
    out->settings_category = s_settingsCategory;
    out->selected_index = s_selectedIndex;
    out->fade_frames = s_fadeFrames;
    CopyText(out->status, sizeof(out->status), s_status);
    CopyText(out->last_error, sizeof(out->last_error), s_lastError);
}

} // namespace NetMenu
