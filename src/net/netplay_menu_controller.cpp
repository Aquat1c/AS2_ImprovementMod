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
#include "net/transition_barrier.h"
#include "rollback/stress_hooks.h"
#include "net/session_types.h"
#include "net/nat_traversal.h"
#include "net/netplay_menu_ui.h"
#include "net/charsel_sync.h"
#include "net/continue_flow.h"
#include "net/frontend_input_sync.h"
#include "net/pregame_sync.h"
#include "net/winscreen_sync.h"
#include "net/match_lifecycle.h"
#include "rollback/rollback_session.h"
#include "rollback/rollback_debug.h"
#include "net/sync_policy.h"
#include "net/set_tracker.h"
#include "net/delay_policy.h"
#include "net/spectator_runtime.h"
#include "net/spectator_manager.h"
#include "net/spectator_client.h"
#include "net/spectator_playback.h"
#include "net/netplay_palette_runtime.h"
#include "net/game_settings_sync.h"
#include "rollback/netplay_log.h"
#include "rollback/online_wiring.h"
#include "rollback/rematch_cleanup.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "core/mod_main.h"
#include "patches/tick_hooks.h"
#include "input/input_system.h"
#include "testing/autoconnect_harness.h"
#include "ui/log_window.h"
#include "ui/menu_utils.h"
#include "ui/netplay_hud_style.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal state
// ============================================================================

namespace {

using namespace NetMenu;

#define SPECTATE_MENU_LOG(level, tag, fmt, ...) \
    do { \
        LOG_NETPLAY(level, fmt, ##__VA_ARGS__); \
        Rollback::NetplayLog_WriteSpectator(tag, -1, fmt, ##__VA_ARGS__); \
    } while (0)

constexpr int kFadeFrames = 25;
// Counts down from kFadeFrames after the menu closes, fading the vanilla main
// menu back in from black (the mod parks in MODE_MENU and bypasses the vanilla
// mode-transition fade, so we recreate the fade-in ourselves on return).
static int s_mainMenuReturnFade = 0;

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
static char          s_status[128]       = "Choose an online option.";
static char          s_lastError[128]    = "";
static char          s_textEditBuffer[96] = "";
static int           s_textCursorPos      = 0;   // Cursor position within text edit buffer
static bool          s_textEditPrevKeyDown[256] = {};
static bool          s_textEditPrevKeyInitialized = false;
static bool          s_prevCopyAddressKeyDown = false;
static char          s_localNickname[64]  = "Player";
static uint16_t      s_listenPort         = 10700;
static char          s_remoteEndpoint[96] = "127.0.0.1:10700";
static char          s_spectatorEndpoint[96] = "127.0.0.1:10701";
static int           s_preferredDelay     = 0;
static int           s_rollbackBudget     = 7;  // Max rollback frames
static int           s_rollbackTolerance  = Net::ROLLBACK_TOLERANCE_DEFAULT;
static Net::GameplayDelayMode s_gameplayDelayMode = Net::GameplayDelayMode::AsymmetricExpert;
static Net::ConnectPreference s_connectPreference = Net::ConnectPreference::AutoDirectThenRelay;
static bool          s_upnpEnabled        = true;
static bool          s_stunEnabled        = true;
static bool          s_holePunchEnabled   = true;
static bool          s_pcpFallbackEnabled = true;
static bool          s_turnEnabled        = false;
static bool          s_allowIPv6Endpoint  = true;
static char          s_relayEndpoint[96]  = "";
static char          s_stunEndpoint[96]   = "stun.l.google.com:19302";
static char          s_turnEndpoint[96]   = "";
static char          s_turnUsername[64]   = "";
static char          s_turnPassword[64]   = "";
static uint32_t      s_natGatherTimeoutMs = 5000;
static uint32_t      s_natConnectTimeoutMs = 8000;
static uint32_t      s_natMappingTimeoutMs = 2000;
static uint8_t       s_natLogVerbosity = 1;
static bool          s_spectatorsEnabled = true;
static uint16_t      s_spectatorListenPort = 0;   // 0 = ephemeral
static bool          s_paletteSyncEnabled = true;
static bool          s_remotePalettePreviewEnabled = false;
static bool          s_continueScreenEnabled = true;
static bool          s_debugLoggingEnabled = false;
static bool          s_joinSpectatorProbeActive = false;
static char          s_joinSpectatorFailureReason[128] = "";
static char          s_joinSpectatorProbeEndpoint[96] = "";
static char          s_joinSpectatorFallbackEndpoint[96] = "";
static bool          s_joinSpectatorFallbackAttempted = false;
static uint32_t      s_selectedLanSpectatorIndex = 0;
static uint32_t      s_lastLanSpectatorResultCount = 0;
static bool          s_lastLanSpectatorDiscoveryActive = false;
static char          s_lastLanSpectatorEndpoint[96] = "";

enum class ActionPromptKind : uint8_t {
    None = 0,
    JoinAsSpectator,
    JoinInsteadOfWaiting,
};

static ActionPromptKind s_actionPromptKind = ActionPromptKind::None;
static uint32_t      s_actionPromptSelectedIndex = 0;
static uint32_t      s_actionPromptOptionCount = 0;
static char          s_actionPromptTitle[96] = "";
static char          s_actionPromptBody[160] = "";
static char          s_actionPromptOptions[3][32] = {};
static char          s_actionPromptJoinEndpoint[96] = "";
static bool          s_idleSpectatorPromptDeferred = false;

enum class ConnectionEntry : uint8_t {
    None = 0,
    Host,
    Join,
};

static ConnectionEntry s_connectionEntry = ConnectionEntry::None;
static MenuState       s_disconnectReturnState = MenuState::MenuRoot;

// Config file path (relative to game directory)
static const char*   kConfigFile          = "as2_netplay.cfg";
static const char*   kAutoConnectFile     = "as2_autoconnect.cfg";

// Early-cached autoconnect file content (read during ModInit before harness
// overwrites the file with the other role's config).
static char  s_cachedAutoConnectContent[4096] = {};
static bool  s_cachedAutoConnectValid = false;

static void TransitionTo(MenuState next, const char* why);
static MenuState ResolveDisconnectReturnMenu();
static bool BuildNatRuntimeConfig(Net::NatRuntimeConfig* outCfg);
static void ApplyNatSettingsToService(const char* reason);
static bool ParseEndpoint(const char* str, char* outHost, size_t hostCap,
                          uint16_t* outPort, bool allowIPv6);
static void ApplyDebugLoggingSetting(const char* reason);

static const char* FriendlyConnectPreferenceLabel(Net::ConnectPreference pref) {
    switch (pref) {
        case Net::ConnectPreference::AutoDirectThenRelay: return "Automatic";
        case Net::ConnectPreference::DirectOnly:          return "Direct Only";
        case Net::ConnectPreference::RelayOnly:           return "Relay N/A";
        default:                                          return "Unknown";
    }
}

static const char* EnabledStateLabel(bool enabled) {
    return enabled ? "Enabled" : "Disabled";
}

enum class AutoConnectState : uint8_t {
    Disabled = 0,
    WaitingForMenu,
    WaitingForConnection,
    WaitingForCharSel,
    SelectingCharacter,
    SelectingStage,
    WaitingForGameplay,
    InMatch,
    ConfirmingWinScreen,
    Failed,
};

struct AutoConnectConfig {
    bool     enabled;
    bool     valid;
    bool     isHost;
    // role=spectator: attach to a running match as a spectator instead of
    // hosting or joining. Needed to run the acceptance matrix with a spectator
    // attached, which previously required driving the menu by hand.
    bool     isSpectator;
    char     spectateTarget[128];
    char     nickname[64];
    uint16_t listenPort;
    char     targetIp[96];
    uint16_t targetPort;
    int      preferredDelay;
    int      characterGridIndex;
    int      palette;
    int      matchDurationSec;
    int      matchCount;
    int      continueNoEvery;   // M8 soak: answer NO on every Nth continue prompt (0 = always YES)
    // Deep-soak variation (2026-08-17 expanded acceptance): per-iteration
    // character/stage cycling so successive games use different matchups on
    // different stages. 0 = fixed (legacy behavior).
    int      charCycleStep;     // grid-index stride added per completed match
    int      stageCycleStep;    // stage-grid RIGHT taps added per completed match
};

static AutoConnectConfig s_autoConnect = {};
static AutoConnectState  s_autoConnectState = AutoConnectState::Disabled;
static int              s_autoConnectStateFrames = 0;
static int              s_autoConnectGlobalFrames = 0;
static uint32_t         s_autoConnectMatchFrame = 0;
static bool             s_autoConnectReleasePending = false;
static bool             s_autoConnectStageGridPressed = false;
static bool             s_autoConnectStageConfirmPressed = false;
// Character-select navigation and the mirror-palette escape (2026-08-17).
// The driver used to press ONLY confirm: characterGridIndex was parsed,
// logged, and never consumed, so both instances confirmed the default cell -
// a mirror match the configs were written specifically to avoid. Then the
// second slot to reach the palette stage was rejected forever by the
// same-vanilla-palette rule ("Palette lock rejected: ... other slot already
// locked same vanilla palette", every press, for 30 s, SOAK-FAIL) because
// nothing ever varied the selection after a rejection.
static int              s_autoConnectNavDownRemaining = 0;
static int              s_autoConnectNavRightRemaining = 0;
static int              s_autoConnectConfirmAttempts = 0;
static bool             s_autoConnectEscapeStepPending = false;
// The visible select grid is 3 columns, row-major (top-left is index 0).
static const int        kAutoConnectCharGridColumns = 3;
static bool             s_autoConnectWinScreenPressed = false;
static bool             s_autoConnectContinuePressed = false;
static bool             s_autoConnectContinueNoToggled = false;
static int              s_autoConnectContinueToggleFrame = 0;
// Deep-soak variation: stage-grid navigation plan (RIGHT taps before the
// grid confirm) and the safe cycling ranges. The char grid has 17 roster
// cells in 3 columns; cycling stays within the first 15 (5 full rows) so a
// navigation plan never walks into the partial last row. Stage cycling taps
// RIGHT n times on the stage grid — the cursor wraps, so any stride is safe.
static int              s_autoConnectStageNavRemaining = 0;
static const int        kAutoConnectCharCycleCells = 15;
static const int        kAutoConnectStageCycleSlots = 6;
static int              s_autoConnectCompletedMatches = 0;
static bool             s_autoRematchCleanupApplied = false;
static DWORD            s_autoRematchLastAttemptAt = 0;

static const char* AutoConnectStateName(AutoConnectState state) {
    switch (state) {
        case AutoConnectState::Disabled:            return "Disabled";
        case AutoConnectState::WaitingForMenu:      return "WaitingForMenu";
        case AutoConnectState::WaitingForConnection:return "WaitingForConnection";
        case AutoConnectState::WaitingForCharSel:   return "WaitingForCharSel";
        case AutoConnectState::SelectingCharacter:  return "SelectingCharacter";
        case AutoConnectState::SelectingStage:      return "SelectingStage";
        case AutoConnectState::WaitingForGameplay:  return "WaitingForGameplay";
        case AutoConnectState::InMatch:             return "InMatch";
        case AutoConnectState::ConfirmingWinScreen: return "ConfirmingWinScreen";
        case AutoConnectState::Failed:              return "Failed";
        default:                                    return "Unknown";
    }
}

// ============================================================================
// Settings persistence (INI-style text file)
// ============================================================================

static void SaveSettings() {
    s_debugLoggingEnabled = GetVerboseLogging();
    FILE* f = nullptr;
    if (fopen_s(&f, kConfigFile, "w") != 0 || !f) {
        LOG_NETPLAY(LOG_WARNING, "[NetMenu] Failed to save settings to %s", kConfigFile);
        return;
    }
    fprintf(f, "[netplay]\n");
    fprintf(f, "nickname=%s\n", s_localNickname);
    fprintf(f, "port=%u\n", s_listenPort);
    fprintf(f, "endpoint=%s\n", s_remoteEndpoint);
    fprintf(f, "spectator_endpoint=%s\n", s_spectatorEndpoint);
    fprintf(f, "delay=%d\n", s_preferredDelay);
    fprintf(f, "rollback=%d\n", s_rollbackBudget);
    fprintf(f, "rollback_tolerance=%d\n", s_rollbackTolerance);
    fprintf(f, "gameplay_delay_mode=%d\n", (int)s_gameplayDelayMode);
    fprintf(f, "connect_mode=%d\n", (int)s_connectPreference);
    fprintf(f, "upnp=%d\n", s_upnpEnabled ? 1 : 0);
    fprintf(f, "stun=%d\n", s_stunEnabled ? 1 : 0);
    fprintf(f, "hole_punch=%d\n", s_holePunchEnabled ? 1 : 0);
    fprintf(f, "pcp_fallback=%d\n", s_pcpFallbackEnabled ? 1 : 0);
    fprintf(f, "turn=%d\n", s_turnEnabled ? 1 : 0);
    fprintf(f, "allow_ipv6=%d\n", s_allowIPv6Endpoint ? 1 : 0);
    fprintf(f, "relay_endpoint=%s\n", s_relayEndpoint);
    fprintf(f, "stun_endpoint=%s\n", s_stunEndpoint);
    fprintf(f, "turn_endpoint=%s\n", s_turnEndpoint);
    fprintf(f, "turn_username=%s\n", s_turnUsername);
    fprintf(f, "turn_password=%s\n", s_turnPassword);
    fprintf(f, "nat_gather_timeout_ms=%u\n", s_natGatherTimeoutMs);
    fprintf(f, "nat_connect_timeout_ms=%u\n", s_natConnectTimeoutMs);
    fprintf(f, "nat_mapping_timeout_ms=%u\n", s_natMappingTimeoutMs);
    fprintf(f, "nat_log_verbosity=%u\n", (unsigned)s_natLogVerbosity);
    fprintf(f, "spectators=%d\n", s_spectatorsEnabled ? 1 : 0);
    fprintf(f, "spectator_port=%u\n", s_spectatorListenPort);
    fprintf(f, "palette_sync=%d\n", s_paletteSyncEnabled ? 1 : 0);
    fprintf(f, "remote_palette_preview=%d\n", s_remotePalettePreviewEnabled ? 1 : 0);
    fprintf(f, "continue_screen=%d\n", s_continueScreenEnabled ? 1 : 0);
    fprintf(f, "debug_logging=%d\n", s_debugLoggingEnabled ? 1 : 0);
    // Persist stress arming so the game's own settings rewrite can't drop it.
    if (Rollback::StressHooks_GetForcedRollbackDepth() > 0) {
        fprintf(f, "forced_rollback=%d\n", Rollback::StressHooks_GetForcedRollbackDepth());
    }
    NetplayHudStyle::Settings hudStyle{};
    NetplayHudStyle::GetLocal(&hudStyle);
    fprintf(f, "hud_trail_r=%u\n", hudStyle.trail_r);
    fprintf(f, "hud_trail_g=%u\n", hudStyle.trail_g);
    fprintf(f, "hud_trail_b=%u\n", hudStyle.trail_b);
    fprintf(f, "hud_text_r=%u\n", hudStyle.text_r);
    fprintf(f, "hud_text_g=%u\n", hudStyle.text_g);
    fprintf(f, "hud_text_b=%u\n", hudStyle.text_b);
    fprintf(f, "hud_score_r=%u\n", hudStyle.score_r);
    fprintf(f, "hud_score_g=%u\n", hudStyle.score_g);
    fprintf(f, "hud_score_b=%u\n", hudStyle.score_b);
    fprintf(f, "hud_trail_length=%u\n", hudStyle.trail_length_px);
    fprintf(f, "hud_vertical_position=%s\n",
        hudStyle.vertical_position == (uint8_t)NetplayHudStyle::HudVerticalPosition::Top ? "top" : "menu_safe");
    const char* fontSizeKey = "large";
    if (hudStyle.font_size == (uint8_t)NetplayHudStyle::HudFontSize::Small) {
        fontSizeKey = "small";
    } else if (hudStyle.font_size == (uint8_t)NetplayHudStyle::HudFontSize::Normal) {
        fontSizeKey = "normal";
    }
    fprintf(f, "hud_font_size=%s\n", fontSizeKey);
    fprintf(f, "hud_render_mode=%s\n",
        hudStyle.render_mode == (uint8_t)NetplayHudStyle::HudRenderMode::Vanilla ? "vanilla" : "overlay");
    fclose(f);
    LOG_NETPLAY(LOG_DEBUG, "[NetMenu] Settings saved to %s", kConfigFile);
}

static void LoadSettings() {
    FILE* f = nullptr;
    if (fopen_s(&f, kConfigFile, "r") != 0 || !f) {
        LOG_NETPLAY(LOG_DEBUG, "[NetMenu] No settings file %s — using defaults", kConfigFile);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Skip empty lines and section headers
        if (len == 0 || line[0] == '[' || line[0] == '#') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        if (_stricmp(key, "nickname") == 0 && val[0]) {
            strncpy_s(s_localNickname, sizeof(s_localNickname), val, _TRUNCATE);
        } else if (_stricmp(key, "port") == 0) {
            int p = atoi(val);
            if (p > 0 && p <= 65535) s_listenPort = (uint16_t)p;
        } else if (_stricmp(key, "endpoint") == 0 && val[0]) {
            strncpy_s(s_remoteEndpoint, sizeof(s_remoteEndpoint), val, _TRUNCATE);
        } else if (_stricmp(key, "spectator_endpoint") == 0 && val[0]) {
            strncpy_s(s_spectatorEndpoint, sizeof(s_spectatorEndpoint), val, _TRUNCATE);
        } else if (_stricmp(key, "delay") == 0) {
            int d = atoi(val);
            if (d >= 0 && d <= 15) s_preferredDelay = d;
        } else if (_stricmp(key, "rollback") == 0) {
            int r = atoi(val);
            if (r >= Net::ROLLBACK_BUDGET_MIN && r <= Net::ROLLBACK_BUDGET_MAX) s_rollbackBudget = r;
        } else if (_stricmp(key, "rollback_tolerance") == 0) {
            int rk = atoi(val);
            if (rk >= Net::ROLLBACK_TOLERANCE_MIN && rk <= Net::ROLLBACK_TOLERANCE_MAX) {
                s_rollbackTolerance = rk;
            }
        } else if (_stricmp(key, "gameplay_delay_mode") == 0) {
            int mode = atoi(val);
            if (Net::GameplayDelayMode_IsValid((uint8_t)mode)) {
                s_gameplayDelayMode = (Net::GameplayDelayMode)mode;
            }
        } else if (_stricmp(key, "connect_mode") == 0) {
            int mode = atoi(val);
            if (mode >= (int)Net::ConnectPreference::AutoDirectThenRelay &&
                mode <= (int)Net::ConnectPreference::RelayOnly) {
                s_connectPreference = (Net::ConnectPreference)mode;
            }
        } else if (_stricmp(key, "upnp") == 0) {
            s_upnpEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "stun") == 0) {
            s_stunEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "hole_punch") == 0) {
            s_holePunchEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "pcp_fallback") == 0) {
            s_pcpFallbackEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "turn") == 0) {
            s_turnEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "allow_ipv6") == 0) {
            s_allowIPv6Endpoint = (atoi(val) != 0);
        } else if (_stricmp(key, "relay_endpoint") == 0) {
            strncpy_s(s_relayEndpoint, sizeof(s_relayEndpoint), val, _TRUNCATE);
        } else if (_stricmp(key, "stun_endpoint") == 0) {
            strncpy_s(s_stunEndpoint, sizeof(s_stunEndpoint), val, _TRUNCATE);
        } else if (_stricmp(key, "turn_endpoint") == 0) {
            strncpy_s(s_turnEndpoint, sizeof(s_turnEndpoint), val, _TRUNCATE);
        } else if (_stricmp(key, "turn_username") == 0) {
            strncpy_s(s_turnUsername, sizeof(s_turnUsername), val, _TRUNCATE);
        } else if (_stricmp(key, "turn_password") == 0) {
            strncpy_s(s_turnPassword, sizeof(s_turnPassword), val, _TRUNCATE);
        } else if (_stricmp(key, "nat_gather_timeout_ms") == 0) {
            int t = atoi(val);
            if (t >= 1000 && t <= 60000) s_natGatherTimeoutMs = (uint32_t)t;
        } else if (_stricmp(key, "nat_connect_timeout_ms") == 0) {
            int t = atoi(val);
            if (t >= 1000 && t <= 60000) s_natConnectTimeoutMs = (uint32_t)t;
        } else if (_stricmp(key, "nat_mapping_timeout_ms") == 0) {
            int t = atoi(val);
            if (t >= 250 && t <= 30000) s_natMappingTimeoutMs = (uint32_t)t;
        } else if (_stricmp(key, "nat_log_verbosity") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 3) s_natLogVerbosity = (uint8_t)v;
        } else if (_stricmp(key, "spectators") == 0) {
            s_spectatorsEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "spectator_port") == 0) {
            int port = atoi(val);
            if (port > 0 && port <= 65535) s_spectatorListenPort = (uint16_t)port;
        } else if (_stricmp(key, "palette_sync") == 0) {
            s_paletteSyncEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "remote_palette_preview") == 0) {
            s_remotePalettePreviewEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "continue_screen") == 0) {
            s_continueScreenEnabled = (atoi(val) != 0);
        } else if (_stricmp(key, "forced_rollback") == 0) {
            // Stress mode from the settings file so EVERY launch gets it —
            // env-var-only arming silently missed user-launched sessions.
            const int depth = atoi(val);
            if (depth > 0) {
                Rollback::StressHooks_SetEnabled(true);
                Rollback::StressHooks_SetForcedRollbackDepth(depth);
                LOG_NETPLAY(LOG_INFO,
                    "[NetMenu] forced_rollback=%d armed from settings file", depth);
            }
        } else if (_stricmp(key, "debug_logging") == 0 ||
                   _stricmp(key, "verbose_logging") == 0) {
            s_debugLoggingEnabled = (atoi(val) != 0);
        }
    }

    fclose(f);
    NetplayHudStyle::Load();
    LOG_NETPLAY(LOG_INFO,
        "[NetMenu] Settings loaded: nick='%s' port=%u endpoint='%s' delay=%d rb=%d rb_tol=%d delay_mode=%s "
        "mode=%s upnp=%d pcp=%d stun=%d punch=%d turn=%d ipv6=%d relay='%s' stun_srv='%s' turn_srv='%s' "
        "nat_timeouts=[%u/%u/%u] nat_log=%u",
        s_localNickname,
        s_listenPort,
        s_remoteEndpoint,
        s_preferredDelay,
        s_rollbackBudget,
        s_rollbackTolerance,
        Net::GameplayDelayModeName(s_gameplayDelayMode),
        Net::ConnectPreferenceName(s_connectPreference),
        s_upnpEnabled ? 1 : 0,
        s_pcpFallbackEnabled ? 1 : 0,
        s_stunEnabled ? 1 : 0,
        s_holePunchEnabled ? 1 : 0,
        s_turnEnabled ? 1 : 0,
        s_allowIPv6Endpoint ? 1 : 0,
        s_relayEndpoint,
        s_stunEndpoint,
        s_turnEndpoint,
        s_natGatherTimeoutMs,
        s_natConnectTimeoutMs,
        s_natMappingTimeoutMs,
        (unsigned)s_natLogVerbosity);
}

static void ApplyDebugLoggingSetting(const char* reason) {
    SetVerboseLogging(s_debugLoggingEnabled);
    LOG_NETPLAY(LOG_INFO,
        "[NetMenu] Applied debug logging setting (%s): enabled=%d",
        reason ? reason : "unspecified",
        s_debugLoggingEnabled ? 1 : 0);
}

static void ApplyDelaySettingsToPolicy(const char* reason) {
    Net::DelayPolicy_SetConfiguredDelay(s_preferredDelay);
    Net::DelayPolicy_SetRollbackBudget(s_rollbackBudget);
    Net::DelayPolicy_SetRollbackToleranceK(s_rollbackTolerance);
    Net::DelayPolicy_SetGameplayDelayMode(s_gameplayDelayMode);

    LOG_NETPLAY(LOG_INFO,
        "[NetMenu] Applied delay policy settings (%s): my_delay=%d rb=%d tol=%d mode=%s active=%d",
        reason ? reason : "unspecified",
        s_preferredDelay,
        s_rollbackBudget,
        s_rollbackTolerance,
        Net::GameplayDelayModeName(s_gameplayDelayMode),
        Net::DelayPolicy_GetActiveDelay());
}

static void ApplySpectatorSettingsToRuntime(const char* reason) {
    Net::SpectatorRuntime_SetEnabled(s_spectatorsEnabled);
    Net::SpectatorRuntime_SetListenPort(s_spectatorListenPort);
    Net::SpectatorClient_SetRelayConfig(s_spectatorsEnabled, s_spectatorListenPort);

    char punchRelayHost[96] = "delthas.fr";
    uint16_t punchRelayPort = 14763;
    if (s_relayEndpoint[0]) {
        char parsedHost[96] = {};
        uint16_t parsedPort = 0;
        if (ParseEndpoint(s_relayEndpoint, parsedHost, sizeof(parsedHost), &parsedPort, false)) {
            strncpy_s(punchRelayHost, sizeof(punchRelayHost), parsedHost, _TRUNCATE);
            punchRelayPort = parsedPort;
        } else {
            SPECTATE_MENU_LOG(LOG_WARNING, "SMENU",
                "[NetMenu] Invalid punch relay endpoint for spectator runtime: %s",
                s_relayEndpoint);
        }
    }
    Net::SpectatorManager_SetAutopunchRelay(s_holePunchEnabled, punchRelayHost, punchRelayPort);
    Net::SpectatorClient_SetAutopunchRelay(s_holePunchEnabled, punchRelayHost, punchRelayPort);

    SPECTATE_MENU_LOG(LOG_INFO, "SMENU",
        "[NetMenu] Applied spectator settings (%s): enabled=%d port=%u punch=%d relay=%s:%u",
        reason ? reason : "unspecified",
        s_spectatorsEnabled ? 1 : 0,
        s_spectatorListenPort,
        s_holePunchEnabled ? 1 : 0,
        punchRelayHost,
        punchRelayPort);
}

static void ApplyPaletteSettingsToRuntime(const char* reason) {
    Net::NetplayPaletteRuntime_SetSyncEnabled(s_paletteSyncEnabled);
    Net::NetplayPaletteRuntime_SetRemotePreviewEnabled(s_remotePalettePreviewEnabled);

    LOG_NETPLAY(LOG_INFO,
        "[NetMenu] Applied palette settings (%s): sync=%d remote_preview=%d",
        reason ? reason : "unspecified",
        s_paletteSyncEnabled ? 1 : 0,
        s_remotePalettePreviewEnabled ? 1 : 0);
}

static void ApplyContinueScreenSettingToRuntime(const char* reason) {
    Net::ContinueFlow_SetEnabled(s_continueScreenEnabled);
    LOG_NETPLAY(LOG_INFO,
        "[NetMenu] Applied continue screen setting (%s): enabled=%d",
        reason ? reason : "unspecified",
        s_continueScreenEnabled ? 1 : 0);
}

// ============================================================================
// Memory helpers
// ============================================================================

static uint8_t  ReadU8(uintptr_t a, uint8_t d = 0)   { __try { return *(volatile uint8_t*)a;  } __except(EXCEPTION_EXECUTE_HANDLER) { return d; } }
static uint32_t ReadU32(uintptr_t a, uint32_t d = 0) { __try { return *(volatile uint32_t*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return d; } }
static void WriteU8(uintptr_t a, uint8_t v)   { __try { *(volatile uint8_t*)a = v;  } __except(EXCEPTION_EXECUTE_HANDLER) {} }
static void WriteU32(uintptr_t a, uint32_t v)  { __try { *(volatile uint32_t*)a = v; } __except(EXCEPTION_EXECUTE_HANDLER) {} }

// ============================================================================
// Menu presentation (vanilla net.bin background + wave\net.bin SFX + BGM 74)
//
// The mod never enters vanilla game mode 4 (Network_MenuStateMachine) because
// that starts the vanilla lockstep/socket machinery. Instead we call only the
// standalone resource loaders directly and reuse the (otherwise unused) mode-4
// asset slots, so the custom menu looks and sounds like the vanilla net menu
// while staying parked in MODE_MENU substate 3.
// ============================================================================

using AssetLoadAllFromArchive_t = int  (__cdecl*)(int* dst, const char* binPath, const char* palPath, int arg4);
using MenuSfxLoad3_t            = void (__cdecl*)(int* dst3, const char* binPath);
using BgmPlayTrack_t            = int  (__cdecl*)(int track);
using AudioPlayWrapper_t        = int  (__cdecl*)(int handle);
using AudioSetVolumeLevel_t     = int  (__cdecl*)(int handle, int level);
using AudioPlay_t              = int  (__cdecl*)(int handle, int mode, int immediate);
using AudioStop_t              = int  (__cdecl*)(int handle);

static AssetLoadAllFromArchive_t s_assetLoadAll = reinterpret_cast<AssetLoadAllFromArchive_t>(ADDR_ASSET_LOAD_ALL_FROM_ARCHIVE);
static MenuSfxLoad3_t            s_menuSfxLoad3  = reinterpret_cast<MenuSfxLoad3_t>(ADDR_MENU_SFX_LOAD3);
static BgmPlayTrack_t            s_bgmPlay       = reinterpret_cast<BgmPlayTrack_t>(ADDR_BGM_PLAY_TRACK);
static AudioPlayWrapper_t        s_audioPlay     = reinterpret_cast<AudioPlayWrapper_t>(ADDR_AUDIO_PLAY_HANDLE);
static AudioSetVolumeLevel_t     s_audioSetVol   = reinterpret_cast<AudioSetVolumeLevel_t>(ADDR_AUDIO_SET_VOLUME_LEVEL);
static AudioPlay_t               s_audioPlayRaw  = reinterpret_cast<AudioPlay_t>(ADDR_AUDIO_PLAY);
static AudioStop_t               s_audioStopRaw  = reinterpret_cast<AudioStop_t>(ADDR_AUDIO_STOP);

// BGM handle reuse: BGM_PlayTrack() reloads+decrypts the song from bgm.bin every
// call (~0.5s, the menu open/close lag). Audio_Stop only pauses — the song buffer
// stays resident — so once a track is loaded we can switch to it instantly with
// Audio_Stop/Audio_Play. We preload track 74 once (cached) and remember the
// main-menu track-0 handle to resume on close, both reset on a real mode change.
static constexpr int kBgmHandleNone = -1;
static int s_netBgmHandle      = kBgmHandleNone;  // cached track 74 handle
static int s_savedMainBgmHandle = kBgmHandleNone; // main-menu BGM (track 0) saved on open

// s_assetsLoaded tracks the cached net.bin / wave\net.bin archive handles. They
// are freed by the engine's Handle_ReleaseAll only on a real game-mode change,
// so while parked in MODE_MENU they survive menu open/close cycles. Caching them
// is what keeps reopening the menu instant instead of reloading the archive
// (a multi-ms hitch) mid-fade every time. s_bgmStarted is a per-open latch so
// BGM 74 restarts each time the menu opens (the main menu reclaims track 0 on
// close).
static bool s_assetsLoaded       = false;
static bool s_bgmStarted         = false;
static bool s_sfxCursorPlayed    = false;
static bool s_sfxConfirmPlayed   = false;
static bool s_sfxCancelPlayed    = false;

static void ResetMenuSfxFrameGuards() {
    s_sfxCursorPlayed  = false;
    s_sfxConfirmPlayed = false;
    s_sfxCancelPlayed  = false;
}

static double NowMs() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void StartNetMenuBgm();

// Load the net.bin background + wave\net.bin SFX bank (cached) and start the
// menu BGM (per open). Called once the menu is actually drawable (after any
// pending mode restore), so the slots are safe to use.
static void EnsureNetMenuPresentation() {
    if (!s_assetsLoaded) {
        s_assetsLoaded = true;  // latch up-front: never retry the heavy load every frame

        const double t0 = NowMs();
        s_assetLoadAll(reinterpret_cast<int*>(ADDR_NET_MENU_BG_HANDLE), "data\\net.bin", "data\\net.pal", 0);
        const double t1 = NowMs();
        s_menuSfxLoad3(reinterpret_cast<int*>(ADDR_NET_MENU_SFX_CURSOR), "wave\\net.bin");
        const double t2 = NowMs();

        const int vol = (int)ReadU8(ADDR_MENU_SFX_VOLUME_BYTE, 0);
        s_audioSetVol((int)ReadU32(ADDR_NET_MENU_SFX_CURSOR,  0), vol);
        s_audioSetVol((int)ReadU32(ADDR_NET_MENU_SFX_CONFIRM, 0), vol);
        s_audioSetVol((int)ReadU32(ADDR_NET_MENU_SFX_CANCEL,  0), vol);

        LOG_NETPLAY(LOG_INFO, "[NetMenu][TIMING] Loaded net assets: bg=0x%X vol=%d | bgLoad=%.1fms sfxLoad=%.1fms",
            ReadU32(ADDR_NET_MENU_BG_HANDLE, 0), vol, t1 - t0, t2 - t1);
    }

    if (!s_bgmStarted) {
        s_bgmStarted = true;
        StartNetMenuBgm();
    }
}

// Switch to the network-menu BGM (track 74). First time per menu session we let
// the game load+volume-init it (one ~0.5s hit, cached); afterwards we just stop
// the current track and replay the resident track-74 buffer — instant.
static void StartNetMenuBgm() {
    if (ReadU32(ADDR_BGM_ENABLED, 0) != 1) {
        return;  // BGM disabled in options — nothing to switch
    }

    // Remember whatever is playing now (main-menu track 0) so close can resume it.
    s_savedMainBgmHandle = (int)ReadU32(ADDR_BGM_CURRENT_HANDLE, (uint32_t)kBgmHandleNone);

    if (s_netBgmHandle == kBgmHandleNone) {
        const double t0 = NowMs();
        s_bgmPlay(NET_MENU_BGM_TRACK);  // loads + plays + sets volume (one-time)
        const double t1 = NowMs();
        s_netBgmHandle = (int)ReadU32(ADDR_BGM_CURRENT_HANDLE, (uint32_t)kBgmHandleNone);
        LOG_NETPLAY(LOG_INFO, "[NetMenu][TIMING] BGM_PlayTrack(%d) first load = %.1fms (handle=0x%X, cached)",
            NET_MENU_BGM_TRACK, t1 - t0, s_netBgmHandle);
        return;
    }

    // Cached: replay the resident buffer (loop mode 3) without reloading.
    const double t0 = NowMs();
    if (s_savedMainBgmHandle != kBgmHandleNone) {
        s_audioStopRaw(s_savedMainBgmHandle);
    }
    s_audioPlayRaw(s_netBgmHandle, 3, 1);
    WriteU32(ADDR_BGM_CURRENT_HANDLE, (uint32_t)s_netBgmHandle);
    const double t1 = NowMs();
    LOG_NETPLAY(LOG_INFO, "[NetMenu][TIMING] BGM switch to %d (cached handle) = %.1fms", NET_MENU_BGM_TRACK, t1 - t0);
}

// Drop the cached handles when the game leaves MODE_MENU. Handle_ReleaseAll frees
// the sprite handles and the audio system recycles BGM handles on a real mode
// change (e.g. match launch), so they must be reloaded on the next open. While we
// stay in MODE_MENU everything remains valid and cached.
static void InvalidateNetMenuPresentationOnModeLeave() {
    if ((s_assetsLoaded || s_netBgmHandle != kBgmHandleNone) && GetGameMode() != MODE_MENU) {
        s_assetsLoaded = false;
        s_netBgmHandle = kBgmHandleNone;
        s_savedMainBgmHandle = kBgmHandleNone;
        s_mainMenuReturnFade = 0;
    }
}

// Restore the main-menu BGM on close. If we have the saved resident track-0
// handle, resume it instantly; otherwise fall back to a (slow) reload.
static void RestoreMainMenuBgm() {
    if (ReadU32(ADDR_BGM_ENABLED, 0) != 1) {
        return;
    }
    if (s_savedMainBgmHandle != kBgmHandleNone) {
        const double t0 = NowMs();
        if (s_netBgmHandle != kBgmHandleNone) {
            s_audioStopRaw(s_netBgmHandle);
        }
        s_audioPlayRaw(s_savedMainBgmHandle, 3, 1);
        WriteU32(ADDR_BGM_CURRENT_HANDLE, (uint32_t)s_savedMainBgmHandle);
        const double t1 = NowMs();
        LOG_NETPLAY(LOG_INFO, "[NetMenu][TIMING] BGM resume main (cached handle) = %.1fms", t1 - t0);
    } else {
        const double t0 = NowMs();
        s_bgmPlay(MAIN_MENU_BGM_TRACK);
        const double t1 = NowMs();
        LOG_NETPLAY(LOG_INFO, "[NetMenu][TIMING] BGM_PlayTrack(%d) reload (no cache) = %.1fms", MAIN_MENU_BGM_TRACK, t1 - t0);
    }
}

static void PlayNetMenuSfx(uintptr_t handleAddr, bool* frameGuard) {
    if (*frameGuard) return;
    *frameGuard = true;
    if (!s_assetsLoaded) return;
    const uint32_t handle = ReadU32(handleAddr, 0);
    if (handle != 0 && s_audioPlay) {
        s_audioPlay((int)handle);
    }
}

static void PlayMenuCursorSfx()  { PlayNetMenuSfx(ADDR_NET_MENU_SFX_CURSOR,  &s_sfxCursorPlayed);  }
static void PlayMenuConfirmSfx() { PlayNetMenuSfx(ADDR_NET_MENU_SFX_CONFIRM, &s_sfxConfirmPlayed); }
static void PlayMenuCancelSfx()  { PlayNetMenuSfx(ADDR_NET_MENU_SFX_CANCEL,  &s_sfxCancelPlayed);  }

// ============================================================================
// String helpers
// ============================================================================

static void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy_s(dst, cap, src, _TRUNCATE);
}

static void TrimWhitespace(char* s) {
    if (!s) return;

    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }

    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') {
        ++start;
    }

    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

static bool IsUtf8ContinuationByte(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

static void AlignCursorToUtf8Boundary(const char* text, int* cursorPos) {
    if (!text || !cursorPos) {
        return;
    }

    const int len = (int)strlen(text);
    if (*cursorPos < 0) {
        *cursorPos = 0;
    }
    if (*cursorPos > len) {
        *cursorPos = len;
    }

    while (*cursorPos > 0 && *cursorPos < len &&
           IsUtf8ContinuationByte((unsigned char)text[*cursorPos])) {
        --(*cursorPos);
    }
}

static int PreviousUtf8Boundary(const char* text, int cursorPos) {
    AlignCursorToUtf8Boundary(text, &cursorPos);
    if (!text || cursorPos <= 0) {
        return 0;
    }

    --cursorPos;
    while (cursorPos > 0 && IsUtf8ContinuationByte((unsigned char)text[cursorPos])) {
        --cursorPos;
    }
    return cursorPos;
}

static int NextUtf8Boundary(const char* text, int cursorPos) {
    AlignCursorToUtf8Boundary(text, &cursorPos);
    if (!text) {
        return 0;
    }

    const int len = (int)strlen(text);
    if (cursorPos >= len) {
        return len;
    }

    ++cursorPos;
    while (cursorPos < len && IsUtf8ContinuationByte((unsigned char)text[cursorPos])) {
        ++cursorPos;
    }
    return cursorPos;
}

static bool IsEndpointField(TextEditField field) {
    return field == TextEditField::RemoteEndpoint ||
           field == TextEditField::SpectatorEndpoint ||
           field == TextEditField::RelayEndpoint ||
           field == TextEditField::StunEndpoint;
}

static bool IsWordSeparatorByte(unsigned char ch) {
    return ch == ' ' || ch == '.' || ch == ':' || ch == '/' || ch == '\\' ||
           ch == '[' || ch == ']' || ch == '-';
}

static int PreviousWordBoundary(const char* text, int cursorPos) {
    AlignCursorToUtf8Boundary(text, &cursorPos);
    while (cursorPos > 0) {
        const int prev = PreviousUtf8Boundary(text, cursorPos);
        const unsigned char ch = (unsigned char)text[prev];
        cursorPos = prev;
        if (!IsWordSeparatorByte(ch)) {
            break;
        }
    }

    while (cursorPos > 0) {
        const int prev = PreviousUtf8Boundary(text, cursorPos);
        const unsigned char ch = (unsigned char)text[prev];
        if (IsWordSeparatorByte(ch)) {
            break;
        }
        cursorPos = prev;
    }
    return cursorPos;
}

static int NextWordBoundary(const char* text, int cursorPos) {
    AlignCursorToUtf8Boundary(text, &cursorPos);
    const int len = (int)strlen(text);

    while (cursorPos < len) {
        const unsigned char ch = (unsigned char)text[cursorPos];
        if (!IsWordSeparatorByte(ch)) {
            break;
        }
        cursorPos = NextUtf8Boundary(text, cursorPos);
    }

    while (cursorPos < len) {
        const unsigned char ch = (unsigned char)text[cursorPos];
        if (IsWordSeparatorByte(ch)) {
            break;
        }
        cursorPos = NextUtf8Boundary(text, cursorPos);
    }

    while (cursorPos < len) {
        const unsigned char ch = (unsigned char)text[cursorPos];
        if (!IsWordSeparatorByte(ch)) {
            break;
        }
        cursorPos = NextUtf8Boundary(text, cursorPos);
    }

    return cursorPos;
}

static bool IsAllowedEndpointWideChar(wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') ||
           (ch >= L'a' && ch <= L'z') ||
           (ch >= L'A' && ch <= L'Z') ||
           ch == L'.' || ch == L':' || ch == L'-' || ch == L'_' ||
           ch == L'[' || ch == L']' || ch == L'/';
}

static bool NormalizeWideCharForField(TextEditField field, wchar_t* ch) {
    if (!ch) {
        return false;
    }

    if (*ch >= 0xD800 && *ch <= 0xDFFF) {
        return false;
    }

    if (field == TextEditField::ListenPort || field == TextEditField::SpectatorPort) {
        return *ch >= L'0' && *ch <= L'9';
    }

    if (IsEndpointField(field)) {
        return IsAllowedEndpointWideChar(*ch);
    }

    if (*ch == L'\r' || *ch == L'\n' || *ch == L'\t') {
        *ch = L' ';
    }

    return *ch >= 0x20 && *ch != 0x7F;
}

static bool IsProcessForegroundWindow() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }

    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return foregroundPid == GetCurrentProcessId();
}

static bool IsTextEditWindowFocused() {
    return IsProcessForegroundWindow();
}

static bool IsCopyAddressKeyDown() {
    return (GetAsyncKeyState('C') & 0x8000) != 0;
}

static void BuildAsyncKeyboardStateSnapshot(BYTE* keyState, size_t keyStateCount) {
    if (!keyState || keyStateCount < 256) {
        return;
    }

    memset(keyState, 0, keyStateCount);
    for (int vk = 0; vk < 256; ++vk) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            keyState[vk] |= 0x80;
        }
    }

    if (GetKeyState(VK_CAPITAL) & 0x0001) {
        keyState[VK_CAPITAL] |= 0x01;
    }
    if (GetKeyState(VK_NUMLOCK) & 0x0001) {
        keyState[VK_NUMLOCK] |= 0x01;
    }
    if (GetKeyState(VK_SCROLL) & 0x0001) {
        keyState[VK_SCROLL] |= 0x01;
    }
}

static int TranslateVirtualKeyToUnicode(int vk, wchar_t* outChars, int outCharCount) {
    BYTE keyState[256] = {};
    if (!outChars || outCharCount <= 0) {
        return 0;
    }

    BuildAsyncKeyboardStateSnapshot(keyState, sizeof(keyState));

    const UINT scanCode = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    int translatedCount = ToUnicode(vk, scanCode, keyState, outChars, outCharCount, 0);
    if (translatedCount < 0) {
        wchar_t deadKeyBuffer[8] = {};
        ToUnicode(vk, scanCode, keyState, deadKeyBuffer, (int)_countof(deadKeyBuffer), 0);
        return 0;
    }

    return translatedCount;
}

static bool InsertUtf8AtCursor(char* buffer,
                               size_t bufferCap,
                               size_t maxEditLen,
                               int* cursorPos,
                               const char* utf8Text,
                               size_t utf8Bytes) {
    if (!buffer || bufferCap == 0 || !cursorPos || !utf8Text || utf8Bytes == 0) {
        return false;
    }

    AlignCursorToUtf8Boundary(buffer, cursorPos);
    const size_t len = strlen(buffer);
    if (len + utf8Bytes > maxEditLen || len + utf8Bytes + 1 > bufferCap) {
        return false;
    }

    memmove(buffer + *cursorPos + utf8Bytes,
            buffer + *cursorPos,
            len - (size_t)(*cursorPos) + 1);
    memcpy(buffer + *cursorPos, utf8Text, utf8Bytes);
    *cursorPos += (int)utf8Bytes;
    return true;
}

static bool InsertWideTextAtCursor(TextEditField field,
                                   size_t maxEditLen,
                                   const wchar_t* wideText,
                                   int wideCharCount) {
    if (!wideText || wideCharCount <= 0) {
        return false;
    }

    bool insertedAny = false;
    for (int i = 0; i < wideCharCount; ++i) {
        wchar_t ch = wideText[i];
        if (!NormalizeWideCharForField(field, &ch)) {
            continue;
        }

        char utf8[8] = {};
        const int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, (int)sizeof(utf8), nullptr, nullptr);
        if (utf8Bytes <= 0) {
            continue;
        }

        if (!InsertUtf8AtCursor(s_textEditBuffer,
                                sizeof(s_textEditBuffer),
                                maxEditLen,
                                &s_textCursorPos,
                                utf8,
                                (size_t)utf8Bytes)) {
            break;
        }

        insertedAny = true;
    }

    return insertedAny;
}

static bool InsertClipboardTextAtCursor(TextEditField field, size_t maxEditLen, const char* utf8Text) {
    if (!utf8Text || !utf8Text[0]) {
        return false;
    }

    wchar_t wideBuffer[256] = {};
    int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text, -1, wideBuffer, (int)_countof(wideBuffer));
    if (wideCount <= 1) {
        wideCount = MultiByteToWideChar(CP_ACP, 0, utf8Text, -1, wideBuffer, (int)_countof(wideBuffer));
    }
    if (wideCount <= 1) {
        return false;
    }

    return InsertWideTextAtCursor(field, maxEditLen, wideBuffer, wideCount - 1);
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
// Endpoint parsing
// ============================================================================

/// Parse endpoint in one of:
///   ipv4:port
///   hostname:port
///   [ipv6]:port
static bool ParseEndpoint(const char* str, char* outHost, size_t hostCap,
                          uint16_t* outPort, bool allowIPv6) {
    if (!str || !outHost || hostCap == 0 || !outPort) return false;

    outHost[0] = '\0';
    *outPort = 0;

    char buf[128] = {};
    strncpy_s(buf, sizeof(buf), str, _TRUNCATE);
    TrimWhitespace(buf);
    if (!buf[0]) return false;

    const char* hostStart = buf;
    const char* hostEnd = nullptr;
    const char* portStart = nullptr;

    if (buf[0] == '[') {
        hostStart = buf + 1;
        hostEnd = strchr(hostStart, ']');
        if (!hostEnd || hostEnd == hostStart) return false;
        if (hostEnd[1] != ':') return false;
        portStart = hostEnd + 2;
        if (!allowIPv6) return false;
    } else {
        const char* lastColon = strrchr(buf, ':');
        if (!lastColon || lastColon == buf) return false;
        // Unbracketed IPv6 contains multiple ':' and is ambiguous.
        if (strchr(buf, ':') != lastColon) return false;
        hostEnd = lastColon;
        portStart = lastColon + 1;
    }

    if (!portStart || !portStart[0]) return false;
    int port = atoi(portStart);
    if (port <= 0 || port > 65535) return false;

    const size_t hostLen = (size_t)(hostEnd - hostStart);
    if (hostLen == 0 || hostLen >= hostCap) return false;
    memcpy(outHost, hostStart, hostLen);
    outHost[hostLen] = '\0';
    TrimWhitespace(outHost);
    if (!outHost[0]) return false;

    if (!allowIPv6 && strchr(outHost, ':')) {
        return false;
    }

    *outPort = (uint16_t)port;
    return true;
}

static bool FormatEndpointText(const char* host,
                               uint16_t port,
                               char* outEndpoint,
                               size_t outCap) {
    if (!host || !host[0] || port == 0 || !outEndpoint || outCap == 0) {
        return false;
    }

    if (strchr(host, ':')) {
        _snprintf_s(outEndpoint, outCap, _TRUNCATE, "[%s]:%u", host, port);
    } else {
        _snprintf_s(outEndpoint, outCap, _TRUNCATE, "%s:%u", host, port);
    }
    return outEndpoint[0] != '\0';
}

static bool ContainsInsensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !haystack[0] || !needle[0]) {
        return false;
    }

    const size_t needleLen = strlen(needle);
    for (const char* cur = haystack; *cur; ++cur) {
        if (_strnicmp(cur, needle, needleLen) == 0) {
            return true;
        }
    }
    return false;
}

static bool IsSpectatorNoActiveMatchError(const char* errorText) {
    return ContainsInsensitive(errorText, "No active match") ||
           ContainsInsensitive(errorText, "requested match not active");
}

static bool IsSpectatorEndpointTimeoutError(const char* errorText) {
    return ContainsInsensitive(errorText, "timed out") ||
           ContainsInsensitive(errorText, "unavailable");
}

static void CopyDisplayedSpectatorEndpoint(char* outEndpoint,
                                           size_t outCap,
                                           const Net::SpectatorClientSnapshot* spectator) {
    const char* source = s_spectatorEndpoint;

    if (s_joinSpectatorProbeActive && s_joinSpectatorProbeEndpoint[0]) {
        source = s_joinSpectatorProbeEndpoint;
    }

    if (spectator) {
        if (spectator->endpoint[0]) {
            source = spectator->endpoint;
        }
        if (spectator->state == Net::SpectatorClientState::Redirected &&
            spectator->redirect_endpoint[0]) {
            source = spectator->redirect_endpoint;
        }
    }

    CopyText(outEndpoint, outCap, source);
}

static void ClearJoinSpectatorProbe(const char* reason) {
    if (s_joinSpectatorProbeActive ||
        s_joinSpectatorFailureReason[0] ||
        s_joinSpectatorProbeEndpoint[0] ||
        s_joinSpectatorFallbackEndpoint[0]) {
        SPECTATE_MENU_LOG(LOG_INFO, "SPROBE",
            "[MENU] clear_join_spectator_probe reason=%s endpoint=%s",
            reason && reason[0] ? reason : "unspecified",
            s_joinSpectatorProbeEndpoint[0] ? s_joinSpectatorProbeEndpoint : "(unset)");
    }
    s_joinSpectatorProbeActive = false;
    s_joinSpectatorFailureReason[0] = '\0';
    s_joinSpectatorProbeEndpoint[0] = '\0';
    s_joinSpectatorFallbackEndpoint[0] = '\0';
    s_joinSpectatorFallbackAttempted = false;
}

static bool ShouldAttemptJoinSpectatorProbe(const char* errorText) {
    if (!errorText || !errorText[0]) {
        return true;
    }

    if (ContainsInsensitive(errorText, "protocol version mismatch") ||
        ContainsInsensitive(errorText, "version mismatch") ||
        ContainsInsensitive(errorText, "build hash mismatch") ||
        ContainsInsensitive(errorText, "build mismatch") ||
        ContainsInsensitive(errorText, "version/build mismatch")) {
        return false;
    }

    // "Connection timed out" is the pre-session ENet connect timeout: the host
    // never answered at all, so probing the spectator port on the same host
    // just burns another 3s per retry. Only probe when the gameplay connect
    // actually reached a peer (refusal, busy disconnect, handshake failure).
    if (ContainsInsensitive(errorText, "connection timed out")) {
        return false;
    }

    return true;
}

static bool BuildSpectatorEndpointFromBase(const char* baseEndpoint,
                                           char* outEndpoint,
                                           size_t outCap) {
    if (!baseEndpoint || !baseEndpoint[0] || !outEndpoint || outCap == 0) {
        return false;
    }

    char host[96] = {};
    uint16_t port = 0;
    if (!ParseEndpoint(baseEndpoint, host, sizeof(host), &port, true)) {
        return false;
    }

    int portDelta = (int)s_spectatorListenPort - (int)s_listenPort;
    if (portDelta == 0) {
        portDelta = 1;
    }

    const int spectatorPort = (int)port + portDelta;
    if (spectatorPort <= 0 || spectatorPort > 65535) {
        return false;
    }

    return FormatEndpointText(host, (uint16_t)spectatorPort, outEndpoint, outCap);
}

static bool BuildDerivedSpectatorEndpoint(char* outEndpoint, size_t outCap) {
    if (!outEndpoint || outCap == 0) {
        return false;
    }

    if (s_connectPreference == Net::ConnectPreference::RelayOnly &&
        BuildSpectatorEndpointFromBase(s_relayEndpoint, outEndpoint, outCap)) {
        return true;
    }

    if (BuildSpectatorEndpointFromBase(s_remoteEndpoint, outEndpoint, outCap)) {
        return true;
    }

    if (BuildSpectatorEndpointFromBase(s_relayEndpoint, outEndpoint, outCap)) {
        return true;
    }

    return false;
}

static bool BuildAlternateDerivedSpectatorEndpoint(const char* primaryEndpoint,
                                                   char* outEndpoint,
                                                   size_t outCap) {
    if (!outEndpoint || outCap == 0) {
        return false;
    }

    outEndpoint[0] = '\0';

    char derivedRemote[96] = {};
    char derivedRelay[96] = {};
    const bool haveRemote = BuildSpectatorEndpointFromBase(s_remoteEndpoint,
        derivedRemote,
        sizeof(derivedRemote));
    const bool haveRelay = BuildSpectatorEndpointFromBase(s_relayEndpoint,
        derivedRelay,
        sizeof(derivedRelay));

    if (haveRemote && haveRelay && _stricmp(derivedRemote, derivedRelay) != 0) {
        if (primaryEndpoint && _stricmp(primaryEndpoint, derivedRemote) == 0) {
            CopyText(outEndpoint, outCap, derivedRelay);
            return true;
        }
        if (primaryEndpoint && _stricmp(primaryEndpoint, derivedRelay) == 0) {
            CopyText(outEndpoint, outCap, derivedRemote);
            return true;
        }
        CopyText(outEndpoint, outCap, derivedRelay);
        return true;
    }

    return false;
}

static bool BuildSessionEndpointFromSpectator(const Net::SpectatorClientSnapshot& spectator,
                                             char* outEndpoint,
                                             size_t outCap) {
    if (!outEndpoint || outCap == 0 || spectator.session_listen_port == 0) {
        return false;
    }

    char sourceEndpoint[96] = {};
    CopyDisplayedSpectatorEndpoint(sourceEndpoint, sizeof(sourceEndpoint), &spectator);
    if (!sourceEndpoint[0]) {
        return false;
    }

    char host[96] = {};
    uint16_t ignoredPort = 0;
    if (!ParseEndpoint(sourceEndpoint, host, sizeof(host), &ignoredPort, true)) {
        return false;
    }

    return FormatEndpointText(host,
        spectator.session_listen_port,
        outEndpoint,
        outCap);
}

static bool IsActionPromptOpen() {
    return s_actionPromptKind != ActionPromptKind::None;
}

static void ClearActionPrompt(const char* reason) {
    if (!IsActionPromptOpen()) {
        return;
    }

    SPECTATE_MENU_LOG(LOG_INFO, "SMENU",
        "[MENU] clear_action_prompt kind=%u reason=%s",
        (unsigned)s_actionPromptKind,
        reason && reason[0] ? reason : "unspecified");
    s_actionPromptKind = ActionPromptKind::None;
    s_actionPromptSelectedIndex = 0;
    s_actionPromptOptionCount = 0;
    s_actionPromptTitle[0] = '\0';
    s_actionPromptBody[0] = '\0';
    memset(s_actionPromptOptions, 0, sizeof(s_actionPromptOptions));
    s_actionPromptJoinEndpoint[0] = '\0';
}

static void OpenActionPrompt(ActionPromptKind kind,
                             const char* title,
                             const char* body,
                             const char* option0,
                             const char* option1,
                             const char* option2,
                             uint32_t optionCount,
                             const char* joinEndpoint,
                             const char* statusText) {
    s_actionPromptKind = kind;
    s_actionPromptSelectedIndex = 0;
    s_actionPromptOptionCount = (std::min)(optionCount, 3u);
    CopyText(s_actionPromptTitle, sizeof(s_actionPromptTitle), title);
    CopyText(s_actionPromptBody, sizeof(s_actionPromptBody), body);
    memset(s_actionPromptOptions, 0, sizeof(s_actionPromptOptions));
    CopyText(s_actionPromptOptions[0], sizeof(s_actionPromptOptions[0]), option0 ? option0 : "");
    CopyText(s_actionPromptOptions[1], sizeof(s_actionPromptOptions[1]), option1 ? option1 : "");
    CopyText(s_actionPromptOptions[2], sizeof(s_actionPromptOptions[2]), option2 ? option2 : "");
    CopyText(s_actionPromptJoinEndpoint, sizeof(s_actionPromptJoinEndpoint), joinEndpoint ? joinEndpoint : "");
    if (statusText && statusText[0]) {
        SetStatus("%s", statusText);
    }
    s_waitForNeutral = true;
    InputSystem_ResetRepeatState(0);

    SPECTATE_MENU_LOG(LOG_INFO, "SMENU",
        "[MENU] open_action_prompt kind=%u title=%s options=%u join_endpoint=%s",
        (unsigned)kind,
        s_actionPromptTitle[0] ? s_actionPromptTitle : "(untitled)",
        (unsigned)s_actionPromptOptionCount,
        s_actionPromptJoinEndpoint[0] ? s_actionPromptJoinEndpoint : "(none)");
}

static void OpenJoinAsSpectatorPrompt(const Net::SpectatorClientSnapshot& spectator,
                                      const char* activeEndpoint) {
    char body[160] = {};
    if (spectator.p1_name[0] || spectator.p2_name[0]) {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
            "%s vs %s is already underway. Watch this match instead?",
            spectator.p1_name[0] ? spectator.p1_name : "P1",
            spectator.p2_name[0] ? spectator.p2_name : "P2");
    } else if (activeEndpoint && activeEndpoint[0]) {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
            "A match is already underway on %s. Watch it instead?",
            activeEndpoint);
    } else {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
            "A match is already underway. Watch it instead?");
    }

    OpenActionPrompt(
        ActionPromptKind::JoinAsSpectator,
        "A match is already in progress.",
        body,
        "Watch Match",
        "Back",
        nullptr,
        2,
        nullptr,
        "The host is already playing. Choose whether to watch or go back.");
}

static void OpenIdleSpectatorPrompt(const Net::SpectatorClientSnapshot& spectator,
                                    const char* activeEndpoint) {
    char joinEndpoint[96] = {};
    BuildSessionEndpointFromSpectator(spectator, joinEndpoint, sizeof(joinEndpoint));

    char body[160] = {};
    if (joinEndpoint[0]) {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
            "You can join the room now at %s, keep waiting here for a match to start, or cancel.",
            joinEndpoint);
    } else if (activeEndpoint && activeEndpoint[0]) {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
            "%s is idle right now. You can join now, keep waiting here, or cancel.",
            activeEndpoint);
    } else {
        _snprintf_s(body, sizeof(body), _TRUNCATE,
            "You can join now, keep waiting here, or cancel.");
    }

    OpenActionPrompt(
        ActionPromptKind::JoinInsteadOfWaiting,
        "The host is not in a match yet.",
        body,
        "Join Match",
        "Keep Waiting",
        "Cancel",
        3,
        joinEndpoint,
        "The watch server is idle. Choose whether to join, keep waiting, or cancel.");
}

static void MoveActionPromptSelection(int delta) {
    if (!IsActionPromptOpen() || s_actionPromptOptionCount == 0) {
        s_actionPromptSelectedIndex = 0;
        return;
    }

    int next = (int)s_actionPromptSelectedIndex + delta;
    while (next < 0) {
        next += (int)s_actionPromptOptionCount;
    }
    while (next >= (int)s_actionPromptOptionCount) {
        next -= (int)s_actionPromptOptionCount;
    }
    s_actionPromptSelectedIndex = (uint32_t)next;
}

static uint32_t GetActionPromptCancelIndex() {
    switch (s_actionPromptKind) {
        case ActionPromptKind::JoinAsSpectator:
            return 1;
        case ActionPromptKind::JoinInsteadOfWaiting:
            return 2;
        default:
            return 0;
    }
}

static bool TryJoinSpectatorProbeFallback(const char* failureReason) {
    if (!s_joinSpectatorProbeActive ||
        s_joinSpectatorFallbackAttempted ||
        !s_joinSpectatorFallbackEndpoint[0]) {
        return false;
    }

    s_joinSpectatorFallbackAttempted = true;
    Net::SpectatorClient_Disconnect("spectator probe fallback");
    if (!Net::SpectatorClient_StartConnect(s_joinSpectatorFallbackEndpoint)) {
        return false;
    }

    CopyText(s_joinSpectatorProbeEndpoint,
        sizeof(s_joinSpectatorProbeEndpoint),
        s_joinSpectatorFallbackEndpoint);
    SetStatus("The main watch address did not respond. Trying the relay watch address...");
    SPECTATE_MENU_LOG(LOG_WARNING, "SPROBE",
        "[SPROBE] fallback endpoint=%s reason=%s",
        s_joinSpectatorFallbackEndpoint,
        failureReason && failureReason[0] ? failureReason : "unspecified");
    return true;
}

static void ResolveJoinSpectatorProbeFailureMessage(const Net::SpectatorClientSnapshot& spectator,
                                                    char* outMessage,
                                                    size_t outCap) {
    if (!outMessage || outCap == 0) {
        return;
    }

    if (IsSpectatorNoActiveMatchError(spectator.error)) {
        strncpy_s(outMessage,
            outCap,
            "Target is not currently in an active match.",
            _TRUNCATE);
        return;
    }

    if (spectator.error[0]) {
        strncpy_s(outMessage, outCap, spectator.error, _TRUNCATE);
        return;
    }

    if (s_joinSpectatorFailureReason[0]) {
        strncpy_s(outMessage, outCap, s_joinSpectatorFailureReason, _TRUNCATE);
        return;
    }

    strncpy_s(outMessage, outCap, "No watch feed was available for that room.", _TRUNCATE);
}

static bool StartJoinSessionToEndpoint(const char* endpoint,
                                       const char* statusText,
                                       const char* transitionWhy) {
    char targetHost[96] = {};
    uint16_t targetPort = 0;
    if (!ParseEndpoint(endpoint, targetHost, sizeof(targetHost), &targetPort, s_allowIPv6Endpoint)) {
        SetStatus("Enter the host address as host:port or [ipv6]:port.");
        return false;
    }

    Net::SessionConfig cfg{};
    Net::SessionConfig_SetDefaults(&cfg);
    cfg.listen_port = s_listenPort;
    strncpy_s(cfg.target_host, sizeof(cfg.target_host), targetHost, _TRUNCATE);
    cfg.target_port = targetPort;
    cfg.connect_preference = s_connectPreference;
    strncpy_s(cfg.nickname, sizeof(cfg.nickname), s_localNickname, _TRUNCATE);

    Net::NatRuntimeConfig natCfg{};
    if (!BuildNatRuntimeConfig(&natCfg)) {
        SetStatus("Connection settings are invalid.");
        return false;
    }

    cfg.nat.enable_upnp = natCfg.enable_upnp;
    cfg.nat.enable_stun = natCfg.enable_stun;
    cfg.nat.enable_hole_punch = natCfg.enable_hole_punch;
    cfg.nat.enable_turn = natCfg.enable_turn;
    cfg.nat.enable_pcp_fallback = natCfg.enable_pcp_fallback;
    cfg.nat.allow_ipv6_endpoint = natCfg.allow_ipv6_endpoint;
    cfg.nat.prefer_portforwarded_direct = natCfg.prefer_portforwarded_direct;
    strncpy_s(cfg.nat.stun_host, sizeof(cfg.nat.stun_host), natCfg.stun_host, _TRUNCATE);
    cfg.nat.stun_port = natCfg.stun_port;
    strncpy_s(cfg.nat.turn_host, sizeof(cfg.nat.turn_host), natCfg.turn_host, _TRUNCATE);
    cfg.nat.turn_port = natCfg.turn_port;
    strncpy_s(cfg.nat.turn_username, sizeof(cfg.nat.turn_username), natCfg.turn_username, _TRUNCATE);
    strncpy_s(cfg.nat.turn_password, sizeof(cfg.nat.turn_password), natCfg.turn_password, _TRUNCATE);
    cfg.nat.gather_timeout_ms = natCfg.gather_timeout_ms;
    cfg.nat.connect_timeout_ms = natCfg.connect_timeout_ms;
    cfg.nat.mapping_timeout_ms = natCfg.mapping_timeout_ms;
    cfg.nat.traversal_log_verbosity = natCfg.traversal_log_verbosity;

    if (s_relayEndpoint[0]) {
        char relayHost[96] = {};
        uint16_t relayPort = 0;
        if (!ParseEndpoint(s_relayEndpoint, relayHost, sizeof(relayHost), &relayPort, true)) {
            SetStatus("The punch relay address is invalid.");
            return false;
        }
        strncpy_s(cfg.nat.relay_host, sizeof(cfg.nat.relay_host), relayHost, _TRUNCATE);
        cfg.nat.relay_port = relayPort;
    }

    ApplyDelaySettingsToPolicy("join start");
    ApplyNatSettingsToService("join start");
    CopyText(s_remoteEndpoint, sizeof(s_remoteEndpoint), endpoint);

    if (!Net::Session_StartJoin(&cfg)) {
        SetStatus("Couldn't start joining the room.");
        return false;
    }

    s_connectionEntry = ConnectionEntry::Join;
    s_activeBranch = RootBranch::DirectPlay;
    s_selectedIndex = 0;
    ClearError();
    SetStatus("%s", statusText && statusText[0] ? statusText : "Connecting to host...");
    TransitionTo(MenuState::Connecting, transitionWhy ? transitionWhy : "join started");
    return true;
}

static void CancelSpectatorConnectionAndReturn(const char* disconnectReason,
                                               const char* statusText,
                                               const char* transitionWhy) {
    const bool fromJoinProbe = s_joinSpectatorProbeActive;
    ClearActionPrompt("cancel_spectator_connection");
    ClearJoinSpectatorProbe("user_cancel");
    s_idleSpectatorPromptDeferred = false;
    Net::SpectatorClient_Disconnect(disconnectReason ? disconnectReason : "spectator canceled");
    s_selectedIndex = 0;
    if (fromJoinProbe) {
        s_activeBranch = RootBranch::DirectPlay;
        s_connectionEntry = ConnectionEntry::None;
        SetStatus(statusText && statusText[0] ? statusText : "Returned to Join a Match.");
        TransitionTo(MenuState::JoinEntry, transitionWhy ? transitionWhy : "cancel join spectator probe");
        return;
    }
    if (statusText && statusText[0]) {
        SetStatus("%s", statusText);
    }
    TransitionTo(MenuState::SpectateEntry, transitionWhy ? transitionWhy : "spectator cancel");
}

static bool BeginJoinSpectatorProbe(const char* sessionError) {
    char derivedEndpoint[96] = {};
    if (!BuildDerivedSpectatorEndpoint(derivedEndpoint, sizeof(derivedEndpoint))) {
        return false;
    }

    SPECTATE_MENU_LOG(LOG_WARNING, "SPROBE",
        "[JOIN] gameplay_join_failed_pre_session target=%s reason=%s",
        s_remoteEndpoint,
        sessionError && sessionError[0] ? sessionError : "busy_or_pre_session_fail");
    SPECTATE_MENU_LOG(LOG_INFO, "SPROBE",
        "[JOIN] probing_spectator_endpoint derived=%s",
        derivedEndpoint);

    Net::Session_Cancel();
    Net::SpectatorClient_Disconnect("restart spectator probe");

    if (s_joinSpectatorProbeActive) {
        ClearJoinSpectatorProbe("restart");
    }

    CopyText(s_joinSpectatorProbeEndpoint,
        sizeof(s_joinSpectatorProbeEndpoint),
        derivedEndpoint);
    BuildAlternateDerivedSpectatorEndpoint(
        derivedEndpoint,
        s_joinSpectatorFallbackEndpoint,
        sizeof(s_joinSpectatorFallbackEndpoint));
    s_joinSpectatorFallbackAttempted = false;
    CopyText(s_joinSpectatorFailureReason,
        sizeof(s_joinSpectatorFailureReason),
        sessionError && sessionError[0] ? sessionError : "Session error.");
    s_joinSpectatorProbeActive = true;

    if (!Net::SpectatorClient_StartConnect(derivedEndpoint)) {
        ClearJoinSpectatorProbe("start_failed");
        return false;
    }

    s_activeBranch = RootBranch::DirectPlay;
    s_connectionEntry = ConnectionEntry::Join;
    s_selectedIndex = 0;
    ClearError();
    SetStatus("The host may already be playing. Checking whether a live watch feed is available...");
    SPECTATE_MENU_LOG(LOG_INFO, "SPROBE",
        "[MENU] begin_join_spectator_probe endpoint=%s",
        s_joinSpectatorProbeEndpoint);
    TransitionTo(MenuState::SpectatorConnecting, "join spectator probe");
    return true;
}

static bool BuildNatRuntimeConfig(Net::NatRuntimeConfig* outCfg) {
    if (!outCfg) return false;

    Net::NatRuntimeConfig cfg{};
    Net::NatRuntimeConfig_SetDefaults(&cfg);
    cfg.enable_upnp = s_upnpEnabled;
    cfg.enable_stun = s_stunEnabled;
    cfg.enable_hole_punch = s_holePunchEnabled;
    cfg.enable_turn = s_turnEnabled;
    cfg.enable_pcp_fallback = s_pcpFallbackEnabled;
    cfg.allow_ipv6_endpoint = s_allowIPv6Endpoint;
    cfg.prefer_portforwarded_direct = true;
    cfg.gather_timeout_ms = s_natGatherTimeoutMs;
    cfg.connect_timeout_ms = s_natConnectTimeoutMs;
    cfg.mapping_timeout_ms = s_natMappingTimeoutMs;
    cfg.traversal_log_verbosity = s_natLogVerbosity;

    if (s_stunEndpoint[0]) {
        char stunHost[96] = {};
        uint16_t stunPort = 0;
        if (!ParseEndpoint(s_stunEndpoint, stunHost, sizeof(stunHost), &stunPort, true)) {
            SetStatus("The STUN server address is invalid.");
            return false;
        }
        strncpy_s(cfg.stun_host, sizeof(cfg.stun_host), stunHost, _TRUNCATE);
        cfg.stun_port = stunPort;
    } else {
        strncpy_s(cfg.stun_host, sizeof(cfg.stun_host), "stun.l.google.com", _TRUNCATE);
        cfg.stun_port = 19302;
    }

    if (s_turnEnabled && s_turnEndpoint[0]) {
        char turnHost[96] = {};
        uint16_t turnPort = 0;
        if (!ParseEndpoint(s_turnEndpoint, turnHost, sizeof(turnHost), &turnPort, true)) {
            SetStatus("The TURN server address is invalid.");
            return false;
        }
        strncpy_s(cfg.turn_host, sizeof(cfg.turn_host), turnHost, _TRUNCATE);
        cfg.turn_port = turnPort;
    } else {
        cfg.turn_host[0] = '\0';
        cfg.turn_port = 3478;
    }
    strncpy_s(cfg.turn_username, sizeof(cfg.turn_username), s_turnUsername, _TRUNCATE);
    strncpy_s(cfg.turn_password, sizeof(cfg.turn_password), s_turnPassword, _TRUNCATE);

    *outCfg = cfg;
    return true;
}

static void ApplyNatSettingsToService(const char* reason) {
    Net::NatRuntimeConfig cfg{};
    if (!BuildNatRuntimeConfig(&cfg)) {
        LOG_NETPLAY(LOG_WARNING, "[NetMenu] NAT settings invalid (%s)", reason ? reason : "?");
        return;
    }
    Net::Nat_ApplyRuntimeConfig(&cfg);
    LOG_NETPLAY(LOG_INFO,
        "[NetMenu] Applied NAT settings (%s): upnp=%d pcp=%d stun=%d punch=%d turn=%d ipv6=%d "
        "stun=%s:%u turn=%s:%u relay=%s mode=%s timeout[g=%u c=%u m=%u] logv=%u",
        reason ? reason : "unspecified",
        cfg.enable_upnp ? 1 : 0,
        cfg.enable_pcp_fallback ? 1 : 0,
        cfg.enable_stun ? 1 : 0,
        cfg.enable_hole_punch ? 1 : 0,
        cfg.enable_turn ? 1 : 0,
        cfg.allow_ipv6_endpoint ? 1 : 0,
        cfg.stun_host,
        cfg.stun_port,
        cfg.turn_host,
        cfg.turn_port,
        s_relayEndpoint,
        Net::ConnectPreferenceName(s_connectPreference),
        cfg.gather_timeout_ms,
        cfg.connect_timeout_ms,
        cfg.mapping_timeout_ms,
        (unsigned)cfg.traversal_log_verbosity);
}

static bool LaunchNetplayCharSel();
static void TryAutoRestartPregameFromPostMatchCharSel();

static void ClearAutoConnectOverride() {
    if (!s_autoConnectReleasePending) return;
    InputSystem_ClearOverride(0);
    s_autoConnectReleasePending = false;
}

static void AutoConnectTransition(AutoConnectState next, const char* why) {
    if (s_autoConnectState == next) return;

    LOG_NETPLAY(LOG_INFO, "[AutoConnect] State %s -> %s (%s)",
        AutoConnectStateName(s_autoConnectState),
        AutoConnectStateName(next),
        why ? why : "?");

    if (s_autoConnectState == AutoConnectState::InMatch && next != AutoConnectState::InMatch) {
        InputSystem_ClearOverride(0);
    }

    if (next == AutoConnectState::Disabled || next == AutoConnectState::Failed || next == AutoConnectState::InMatch) {
        ClearAutoConnectOverride();
    }

    if (next == AutoConnectState::SelectingStage) {
        s_autoConnectStageGridPressed = false;
        s_autoConnectStageConfirmPressed = false;
        // Deep-soak stage variation: tap RIGHT n times on the stage grid
        // before confirming. Both peers compute the same plan (the match
        // counter advances in lockstep), and only the stage owner's cursor
        // is authoritative — the other side's identical taps are harmless.
        s_autoConnectStageNavRemaining = s_autoConnect.stageCycleStep > 0
            ? (s_autoConnectCompletedMatches * s_autoConnect.stageCycleStep)
                  % kAutoConnectStageCycleSlots
            : 0;
        if (s_autoConnectStageNavRemaining > 0) {
            LOG_NETPLAY(LOG_INFO,
                "[AutoConnect] Stage navigation plan: match=%d -> right_taps=%d",
                s_autoConnectCompletedMatches + 1, s_autoConnectStageNavRemaining);
        }
    }

    if (next == AutoConnectState::SelectingCharacter) {
        // Walk plan for the CONFIGURED character: row-major 3-column grid,
        // so index -> (index/3) DOWN taps + (index%3) RIGHT taps from the
        // default top-left cursor. Every rematch charsel re-enters this
        // state, so the plan resets with it.
        // Deep-soak character variation: add a per-iteration stride so each
        // game (via the NO->charsel route) is a different matchup. Cycling
        // stays within the 15 full grid cells.
        int grid = s_autoConnect.characterGridIndex > 0
            ? s_autoConnect.characterGridIndex : 0;
        if (s_autoConnect.charCycleStep > 0) {
            grid = (grid + s_autoConnectCompletedMatches * s_autoConnect.charCycleStep)
                       % kAutoConnectCharCycleCells;
            LOG_NETPLAY(LOG_INFO,
                "[AutoConnect] CharSel cycle: match=%d base=%d step=%d -> grid=%d",
                s_autoConnectCompletedMatches + 1,
                s_autoConnect.characterGridIndex,
                s_autoConnect.charCycleStep, grid);
        }
        s_autoConnectNavDownRemaining = grid / kAutoConnectCharGridColumns;
        s_autoConnectNavRightRemaining = grid % kAutoConnectCharGridColumns;
        s_autoConnectConfirmAttempts = 0;
        s_autoConnectEscapeStepPending = false;
        if (grid > 0) {
            LOG_NETPLAY(LOG_INFO,
                "[AutoConnect] CharSel navigation plan: grid=%d -> down=%d right=%d",
                grid, s_autoConnectNavDownRemaining,
                s_autoConnectNavRightRemaining);
        }
    }

    if (next == AutoConnectState::ConfirmingWinScreen) {
        s_autoConnectWinScreenPressed = false;
        s_autoConnectContinuePressed = false;
        s_autoConnectContinueNoToggled = false;
        s_autoConnectContinueToggleFrame = 0;
    }

    if (next == AutoConnectState::InMatch) {
        s_autoConnectMatchFrame = 0;
        LOG_NETPLAY(LOG_INFO,
            "[AutoConnect] Entering match %d/%d",
            s_autoConnectCompletedMatches + 1,
            s_autoConnect.matchCount > 0 ? s_autoConnect.matchCount : 1);
    }

    s_autoConnectState = next;
    s_autoConnectStateFrames = 0;
}

static void AutoConnectInjectPress(uint16_t input, const char* why) {
    InputSystem_SetOverride(0, input);
    s_autoConnectReleasePending = true;

    LOG_NETPLAY(LOG_INFO, "[AutoConnect] Inject input 0x%04X (%s) mode=%u sub=%u",
        input,
        why ? why : "?",
        GetGameMode(),
        GetSubstate());
}

// ============================================================================
// Early autoconnect config caching
// ============================================================================

static void CacheAutoConnectFileImpl() {
    FILE* f = nullptr;
    if (fopen_s(&f, kAutoConnectFile, "r") != 0 || !f) {
        s_cachedAutoConnectValid = false;
        return;
    }
    size_t n = fread(s_cachedAutoConnectContent, 1, sizeof(s_cachedAutoConnectContent) - 1, f);
    s_cachedAutoConnectContent[n] = '\0';
    fclose(f);
    s_cachedAutoConnectValid = true;
}

static void LoadAutoConnectConfig() {
    memset(&s_autoConnect, 0, sizeof(s_autoConnect));
    s_autoConnect.enabled = false;
    s_autoConnect.valid = false;
    s_autoConnect.isHost = true;
    s_autoConnect.listenPort = s_listenPort;
    strncpy_s(s_autoConnect.nickname, sizeof(s_autoConnect.nickname), s_localNickname, _TRUNCATE);
    strncpy_s(s_autoConnect.targetIp, sizeof(s_autoConnect.targetIp), "127.0.0.1", _TRUNCATE);
    s_autoConnect.targetPort = s_listenPort;
    s_autoConnect.preferredDelay = s_preferredDelay;
    s_autoConnect.characterGridIndex = 0;
    s_autoConnect.palette = 0;
    s_autoConnect.matchDurationSec = 0;
    s_autoConnect.matchCount = 1;
    s_autoConnectState = AutoConnectState::Disabled;
    s_autoConnectStateFrames = 0;
    s_autoConnectCompletedMatches = 0;
    s_autoConnectReleasePending = false;
    s_autoConnectStageGridPressed = false;
    s_autoConnectStageConfirmPressed = false;

    // Use early-cached content if available (avoids race with harness overwriting
    // the config file), otherwise read from disk.
    char localBuf[4096] = {};
    const char* parseSource = nullptr;

    if (s_cachedAutoConnectValid) {
        strncpy_s(localBuf, sizeof(localBuf), s_cachedAutoConnectContent, _TRUNCATE);
        parseSource = "cache";
    } else {
        FILE* f = nullptr;
        if (fopen_s(&f, kAutoConnectFile, "r") != 0 || !f) {
            return;
        }
        size_t n = fread(localBuf, 1, sizeof(localBuf) - 1, f);
        localBuf[n] = '\0';
        fclose(f);
        parseSource = "disk";
    }

    enum class Section : uint8_t {
        None = 0,
        AutoConnect,
    } section = Section::None;

    // Parse line-by-line from localBuf
    char* ctx = nullptr;
    char* linePtr = strtok_s(localBuf, "\n", &ctx);
    while (linePtr) {
        char line[256];
        strncpy_s(line, sizeof(line), linePtr, _TRUNCATE);
        linePtr = strtok_s(nullptr, "\n", &ctx);
        TrimWhitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            if (_stricmp(line, "[autoconnect]") == 0) {
                section = Section::AutoConnect;
            } else {
                section = Section::None;
            }
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char* key = line;
        char* val = eq + 1;
        TrimWhitespace(key);
        TrimWhitespace(val);

        if (section != Section::AutoConnect) continue;

        if (_stricmp(key, "enabled") == 0) {
            s_autoConnect.enabled = (atoi(val) != 0);
        } else if (_stricmp(key, "role") == 0) {
            s_autoConnect.isHost = (_stricmp(val, "host") == 0);
            s_autoConnect.isSpectator = (_stricmp(val, "spectator") == 0);
        } else if (_stricmp(key, "spectate_target") == 0 && val[0]) {
            strncpy_s(s_autoConnect.spectateTarget,
                      sizeof(s_autoConnect.spectateTarget), val, _TRUNCATE);
        } else if (_stricmp(key, "nickname") == 0 && val[0]) {
            strncpy_s(s_autoConnect.nickname, sizeof(s_autoConnect.nickname), val, _TRUNCATE);
        } else if (_stricmp(key, "port") == 0) {
            int port = atoi(val);
            if (port > 0 && port <= 65535) {
                s_autoConnect.listenPort = (uint16_t)port;
            }
        } else if (_stricmp(key, "target_ip") == 0 && val[0]) {
            strncpy_s(s_autoConnect.targetIp, sizeof(s_autoConnect.targetIp), val, _TRUNCATE);
        } else if (_stricmp(key, "target_endpoint") == 0 && val[0]) {
            char host[96] = {};
            uint16_t port = 0;
            if (ParseEndpoint(val, host, sizeof(host), &port, true)) {
                strncpy_s(s_autoConnect.targetIp, sizeof(s_autoConnect.targetIp), host, _TRUNCATE);
                s_autoConnect.targetPort = port;
            }
        } else if (_stricmp(key, "target_port") == 0) {
            int port = atoi(val);
            if (port > 0 && port <= 65535) {
                s_autoConnect.targetPort = (uint16_t)port;
            }
        } else if (_stricmp(key, "delay_frames") == 0) {
            int delay = atoi(val);
            if (delay >= 0 && delay <= 15) {
                s_autoConnect.preferredDelay = delay;
            }
        } else if (_stricmp(key, "character_id") == 0) {
            s_autoConnect.characterGridIndex = atoi(val);
        } else if (_stricmp(key, "char_cycle_step") == 0) {
            // Deep-soak variation: grid-index stride per completed match.
            int v = atoi(val);
            if (v >= 0 && v < kAutoConnectCharCycleCells) s_autoConnect.charCycleStep = v;
        } else if (_stricmp(key, "stage_cycle_step") == 0) {
            // Deep-soak variation: stage-grid RIGHT taps per completed match.
            int v = atoi(val);
            if (v >= 0 && v < 16) s_autoConnect.stageCycleStep = v;
        } else if (_stricmp(key, "palette") == 0) {
            s_autoConnect.palette = atoi(val);
        } else if (_stricmp(key, "match_duration_sec") == 0) {
            s_autoConnect.matchDurationSec = atoi(val);
        } else if (_stricmp(key, "match_count") == 0) {
            s_autoConnect.matchCount = atoi(val);
        } else if (_stricmp(key, "continue_no_every") == 0) {
            // M8 soak knob: answer NO on every Nth continue prompt so the
            // rematch soak exercises the any-NO charsel route as well as the
            // YES,YES fast path. Deterministic on both peers when both cfgs
            // carry the same value (the match counter advances in lockstep);
            // a single side answering NO also routes both to charsel.
            s_autoConnect.continueNoEvery = atoi(val);
        }
    }

    if (s_autoConnect.matchCount <= 0) {
        s_autoConnect.matchCount = 1;
    }

    if (!s_autoConnect.enabled) {
        return;
    }

    s_autoConnect.valid = true;
    strncpy_s(s_localNickname, sizeof(s_localNickname), s_autoConnect.nickname, _TRUNCATE);
    s_listenPort = s_autoConnect.listenPort;
    if (strchr(s_autoConnect.targetIp, ':')) {
        _snprintf_s(s_remoteEndpoint, sizeof(s_remoteEndpoint), _TRUNCATE,
            "[%s]:%u", s_autoConnect.targetIp, s_autoConnect.targetPort);
    } else {
        _snprintf_s(s_remoteEndpoint, sizeof(s_remoteEndpoint), _TRUNCATE,
            "%s:%u", s_autoConnect.targetIp, s_autoConnect.targetPort);
    }
    s_preferredDelay = s_autoConnect.preferredDelay;
    Net::DelayPolicy_SetConfiguredDelay(s_preferredDelay);

    LOG_NETPLAY(LOG_INFO,
        "[AutoConnect] Loaded %s (from %s): role=%s nick='%s' port=%u target=%s delay=%d char=%d pal=%d duration=%d matches=%d continue_no_every=%d",
        kAutoConnectFile,
        parseSource,
        s_autoConnect.isHost ? "Host" : "Join",
        s_localNickname,
        s_listenPort,
        s_remoteEndpoint,
        s_preferredDelay,
        s_autoConnect.characterGridIndex,
        s_autoConnect.palette,
        s_autoConnect.matchDurationSec,
        s_autoConnect.matchCount,
        s_autoConnect.continueNoEvery);

    // Initialize the test harness SHM so the launcher can see real-time state
    AutoConnectHarness_Init(s_autoConnect.isHost, s_autoConnect.nickname,
                            s_autoConnect.matchDurationSec);

    AutoConnectTransition(AutoConnectState::WaitingForMenu, "config loaded");
}

static bool IsAutoConnectSessionReady(Net::SessionState state) {
    return state == Net::SessionState::Connected || state == Net::SessionState::Ready;
}

static bool BeginAutoConnectSession() {
    Net::SessionConfig cfg{};
    Net::SessionConfig_SetDefaults(&cfg);
    strncpy_s(cfg.nickname, sizeof(cfg.nickname), s_localNickname, _TRUNCATE);
    cfg.listen_port = s_listenPort;
    cfg.connect_timeout_ms = 10000;
    cfg.handshake_timeout_ms = 5000;
    cfg.connect_preference = s_connectPreference;

    Net::NatRuntimeConfig natCfg{};
    if (!BuildNatRuntimeConfig(&natCfg)) {
        return false;
    }
    cfg.nat.enable_upnp = natCfg.enable_upnp;
    cfg.nat.enable_stun = natCfg.enable_stun;
    cfg.nat.enable_hole_punch = natCfg.enable_hole_punch;
    cfg.nat.enable_turn = natCfg.enable_turn;
    cfg.nat.enable_pcp_fallback = natCfg.enable_pcp_fallback;
    cfg.nat.allow_ipv6_endpoint = natCfg.allow_ipv6_endpoint;
    cfg.nat.prefer_portforwarded_direct = natCfg.prefer_portforwarded_direct;
    strncpy_s(cfg.nat.stun_host, sizeof(cfg.nat.stun_host), natCfg.stun_host, _TRUNCATE);
    cfg.nat.stun_port = natCfg.stun_port;
    strncpy_s(cfg.nat.turn_host, sizeof(cfg.nat.turn_host), natCfg.turn_host, _TRUNCATE);
    cfg.nat.turn_port = natCfg.turn_port;
    strncpy_s(cfg.nat.turn_username, sizeof(cfg.nat.turn_username), natCfg.turn_username, _TRUNCATE);
    strncpy_s(cfg.nat.turn_password, sizeof(cfg.nat.turn_password), natCfg.turn_password, _TRUNCATE);
    cfg.nat.gather_timeout_ms = natCfg.gather_timeout_ms;
    cfg.nat.connect_timeout_ms = natCfg.connect_timeout_ms;
    cfg.nat.mapping_timeout_ms = natCfg.mapping_timeout_ms;
    cfg.nat.traversal_log_verbosity = natCfg.traversal_log_verbosity;

    ApplyDelaySettingsToPolicy("autoconnect session begin");
    ApplyNatSettingsToService("autoconnect session begin");

    if (s_autoConnect.isHost) {
        LOG_NETPLAY(LOG_INFO, "[AutoConnect] Starting host session on port %u", cfg.listen_port);
        return Net::Session_StartHost(&cfg);
    }

    char targetHost[96] = {};
    uint16_t targetPort = 0;
    if (!ParseEndpoint(s_remoteEndpoint, targetHost, sizeof(targetHost), &targetPort, s_allowIPv6Endpoint)) {
        LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Invalid endpoint '%s'", s_remoteEndpoint);
        return false;
    }

    strncpy_s(cfg.target_host, sizeof(cfg.target_host), targetHost, _TRUNCATE);
    cfg.target_port = targetPort;

    if (s_relayEndpoint[0]) {
        char relayHost[96] = {};
        uint16_t relayPort = 0;
        if (!ParseEndpoint(s_relayEndpoint, relayHost, sizeof(relayHost), &relayPort, true)) {
            LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Invalid punch relay endpoint '%s'", s_relayEndpoint);
            return false;
        }
        strncpy_s(cfg.nat.relay_host, sizeof(cfg.nat.relay_host), relayHost, _TRUNCATE);
        cfg.nat.relay_port = relayPort;
    }

    LOG_NETPLAY(LOG_INFO, "[AutoConnect] Starting join session to %s", s_remoteEndpoint);
    return Net::Session_StartJoin(&cfg);
}

static void HandleAutoConnect() {
    if (s_autoConnectState == AutoConnectState::Disabled) return;

    ClearAutoConnectOverride();
    ++s_autoConnectStateFrames;
    ++s_autoConnectGlobalFrames;

    // Update harness SHM every frame (all phases)
    AutoConnectHarness_Update(
        AutoConnectStateName(s_autoConnectState),
        (uint32_t)s_autoConnectState,
        (uint32_t)s_autoConnectGlobalFrames);

    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);

    if (snap.state == Net::SessionState::Failed && s_autoConnectState != AutoConnectState::Failed) {
        LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Session failed: %s",
            snap.error_text[0] ? snap.error_text : "Unknown error");
        AutoConnectTransition(AutoConnectState::Failed, "session failed");
        return;
    }

    const uint32_t mode = GetGameMode();
    const uint32_t sub = GetSubstate();

    switch (s_autoConnectState) {
        case AutoConnectState::WaitingForMenu:
            // Spectator role: no session of our own — attach to the host's
            // spectator endpoint and then just watch.
            if (s_autoConnect.isSpectator) {
                if (mode == MODE_MENU && !ModeOwnership::IsPendingMenuRestore()) {
                    const char* target = s_autoConnect.spectateTarget[0]
                                             ? s_autoConnect.spectateTarget
                                             : s_remoteEndpoint;
                    LOG_NETPLAY(LOG_INFO,
                        "[AutoConnect] Spectator attaching to %s", target);
                    if (Net::SpectatorClient_StartConnect(target)) {
                        AutoConnectTransition(AutoConnectState::WaitingForConnection,
                                              "spectator connect started");
                    } else if (s_autoConnectStateFrames > 600) {
                        // Host may not be listening yet; retry rather than fail.
                        AutoConnectTransition(AutoConnectState::WaitingForMenu,
                                              "spectator connect retry");
                    }
                }
                break;
            }
            if (mode == MODE_MENU && !ModeOwnership::IsPendingMenuRestore() &&
                snap.state == Net::SessionState::Idle) {
                if (BeginAutoConnectSession()) {
                    AutoConnectTransition(AutoConnectState::WaitingForConnection, "session started");
                } else {
                    AutoConnectTransition(AutoConnectState::Failed, "session start failed");
                }
            }
            break;

        case AutoConnectState::WaitingForConnection:
            if (s_autoConnect.isSpectator) {
                // Spectator has no ready/charsel handshake; it simply follows.
                break;
            }
            if (snap.state == Net::SessionState::Connected && !snap.local_ready) {
                // Auto-accept the match
                Net::Session_SignalReady();
                AutoConnectTransition(AutoConnectState::WaitingForConnection, "auto-accepted, waiting for peer");
            } else if (snap.state == Net::SessionState::Ready) {
                // Both accepted — launch charsel
                if (LaunchNetplayCharSel()) {
                    AutoConnectTransition(AutoConnectState::WaitingForCharSel, "both accepted, launching charsel");
                } else {
                    AutoConnectTransition(AutoConnectState::Failed, "charsel launch failed");
                }
            } else if (s_autoConnectStateFrames > 3600) {
                LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Timed out waiting for connection");
                AutoConnectTransition(AutoConnectState::Failed, "connection timeout");
            }
            break;

        case AutoConnectState::WaitingForCharSel:
            if (mode == MODE_CHARSEL) {
                if (sub >= CHARSEL_SUB_STAGESEL_SLIDE) {
                    AutoConnectTransition(AutoConnectState::SelectingStage, "entered stage select");
                } else {
                    AutoConnectTransition(AutoConnectState::SelectingCharacter, "entered charsel");
                }
            } else if (mode == MODE_MATCH) {
                AutoConnectTransition(AutoConnectState::InMatch, "entered match early");
            } else if (s_autoConnectStateFrames > 900) {
                LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Timed out waiting for charsel");
                AutoConnectTransition(AutoConnectState::Failed, "charsel timeout");
            }
            break;

        case AutoConnectState::SelectingCharacter: {
            Net::CharSelSyncSnapshot csSnap{};
            Net::CharSelSync_GetSnapshot(&csSnap);

            if (mode == MODE_MATCH) {
                AutoConnectTransition(AutoConnectState::InMatch, "entered match");
                break;
            }

            if (mode == MODE_CHARSEL && (csSnap.both_characters_locked || sub >= CHARSEL_SUB_STAGESEL_SLIDE)) {
                AutoConnectTransition(AutoConnectState::SelectingStage, "characters locked");
                break;
            }

            if (mode == MODE_CHARSEL &&
                (sub == CHARSEL_SUB_SELECT || sub == CHARSEL_SUB_CONFIRM)) {
                // Navigation taps run on a fast cadence; confirm keeps the
                // original slow one. Taps are spaced (not every frame)
                // because each must be a fresh press edge.
                const bool navPending =
                    s_autoConnectNavDownRemaining > 0
                    || s_autoConnectNavRightRemaining > 0;
                const bool navTick = navPending
                    && s_autoConnectStateFrames >= 31
                    && (s_autoConnectStateFrames % 20) == 0;
                const bool confirmTick = !navPending
                    && (s_autoConnectStateFrames == 31
                        || (s_autoConnectStateFrames > 300
                            && (s_autoConnectStateFrames % 120) == 0));
                if (navTick && sub == CHARSEL_SUB_SELECT) {
                    if (s_autoConnectNavDownRemaining > 0) {
                        AutoConnectInjectPress(INPUT_DOWN, "navigate to configured character");
                        s_autoConnectNavDownRemaining--;
                    } else {
                        AutoConnectInjectPress(INPUT_RIGHT, "navigate to configured character");
                        s_autoConnectNavRightRemaining--;
                    }
                } else if (confirmTick) {
                    // ── THE MIRROR-PALETTE ESCAPE ─────────────────────────
                    // A confirm press that leaves local_confirmed at 0 was
                    // REJECTED - in a mirror match the same-vanilla-palette
                    // rule refuses the second slot's lock, and pressing the
                    // identical confirm forever is what hung the 2026-08-17
                    // soak on both peers. After two rejected confirms,
                    // interleave one RIGHT: at the palette stage it steps to
                    // the next color, at the grid it steps to the next
                    // character - either way the next confirm asks for
                    // something the other slot has not locked, so this
                    // converges regardless of which side locked first.
                    if (!csSnap.local_confirmed
                        && s_autoConnectConfirmAttempts >= 2
                        && !s_autoConnectEscapeStepPending) {
                        AutoConnectInjectPress(INPUT_RIGHT,
                            "step selection (confirm rejected; mirror palette conflict)");
                        s_autoConnectEscapeStepPending = true;
                    } else {
                        AutoConnectInjectPress(INPUT_A, "confirm character");
                        s_autoConnectConfirmAttempts++;
                        s_autoConnectEscapeStepPending = false;
                    }
                }
            }

            if (s_autoConnectStateFrames > 1800) {
                LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Timed out during character selection");
                AutoConnectTransition(AutoConnectState::Failed, "character select timeout");
            }
            break;
        }

        case AutoConnectState::SelectingStage: {
            Net::CharSelSyncSnapshot csSnap{};
            Net::CharSelSync_GetSnapshot(&csSnap);

            if (mode == MODE_MATCH) {
                AutoConnectTransition(AutoConnectState::InMatch, "entered match");
                break;
            }

            if (mode == MODE_PREMATCH_INTRO ||
                (mode == MODE_CHARSEL && (csSnap.both_stage_locked ||
                                          sub == CHARSEL_SUB_MATCHUP_COMMIT ||
                                          sub == CHARSEL_SUB_TO_MATCH))) {
                AutoConnectTransition(AutoConnectState::WaitingForGameplay, "stage locked");
                break;
            }

            if (mode == MODE_CHARSEL && sub == CHARSEL_SUB_STAGESEL_GRID &&
                s_autoConnectStateFrames >= 31 &&
                (s_autoConnectStateFrames % 20) == 0) {
                // Deep-soak stage variation: walk the cursor before confirm,
                // and never confirm a LOCKED stage — the grid handler
                // (sub_5C0B20) silently refuses the confirm when
                // byte_815FFF[cursor] != 1, which would strand the driver
                // in an A-retry loop until the stage timeout. The cursor is
                // the shared merged-input cursor, identical on both peers,
                // so both sides converge on the same available slot.
                const uint8_t stageCursor =
                    *(volatile uint8_t*)ADDR_STAGE_CURSOR;
                const uint8_t stageAvail = (stageCursor < 24)
                    ? *(volatile uint8_t*)(ADDR_STAGE_AVAIL_TABLE + stageCursor)
                    : (uint8_t)0;
                if (s_autoConnectStageNavRemaining > 0 || stageAvail != 1) {
                    AutoConnectInjectPress(INPUT_RIGHT,
                        s_autoConnectStageNavRemaining > 0
                            ? "navigate stage grid" : "skip locked stage");
                    if (s_autoConnectStageNavRemaining > 0) {
                        s_autoConnectStageNavRemaining--;
                    }
                } else if (!s_autoConnectStageGridPressed ||
                           (s_autoConnectStateFrames > 300 &&
                            (s_autoConnectStateFrames % 120) == 0)) {
                    AutoConnectInjectPress(INPUT_A, "open stage confirm");
                    s_autoConnectStageGridPressed = true;
                }
            } else if (mode == MODE_CHARSEL && sub == CHARSEL_SUB_STAGESEL_CONFIRM &&
                       (!s_autoConnectStageConfirmPressed ||
                        (s_autoConnectStateFrames > 300 && (s_autoConnectStateFrames % 120) == 0))) {
                AutoConnectInjectPress(INPUT_A, "confirm stage");
                s_autoConnectStageConfirmPressed = true;
            }

            if (s_autoConnectStateFrames > 2400) {
                LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Timed out during stage selection");
                AutoConnectTransition(AutoConnectState::Failed, "stage select timeout");
            }
            break;
        }

        case AutoConnectState::WaitingForGameplay:
            if (mode == MODE_MATCH) {
                AutoConnectTransition(AutoConnectState::InMatch, "entered match");
            } else if (s_autoConnectStateFrames > 2400) {
                LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Timed out waiting for gameplay");
                AutoConnectTransition(AutoConnectState::Failed, "gameplay timeout");
            }
            break;

        case AutoConnectState::InMatch: {
            // Run fighting AI — inject combat inputs every frame
            uint16_t input = AutoConnectHarness_RunFightingAI(
                s_autoConnect.isHost, s_autoConnectMatchFrame);
            s_autoConnectMatchFrame++;

            // Periodic log
            if (s_autoConnectMatchFrame % 300 == 0) {
                LOG_NETPLAY(LOG_INFO, "[AutoConnect] InMatch frame=%u input=0x%04X mode=%u sub=%u",
                    s_autoConnectMatchFrame, input, mode, sub);
            }

            if (mode == MODE_WINSCREEN) {
                // Per-game acceptance report. One machine-greppable line per
                // match carrying the health counters that matter, so an
                // unattended run can be audited game by game instead of by
                // scrolling raw logs.
                {
                    Rollback::RollbackSessionSnapshot rb{};
                    Rollback::RollbackSession_GetSnapshot(&rb);
                    Rollback::ForcedRollbackLiveStats fr{};
                    Rollback::RollbackSession_GetForcedStats(&fr);
                    Net::DelayPolicySnapshot dp{};
                    Net::DelayPolicy_GetSnapshot(&dp);
                    Rollback::NetplayLog_Write("GAMEREPORT", -1,
                        "match=%d/%d frames=%u rtt=%.1fms delay=%d budget=%d "
                        "rollbacks=%d maxdepth=%d mispredictions=%d "
                        "replay_verified=%u replay_bad=%u desync=%d",
                        s_autoConnectCompletedMatches + 1,
                        s_autoConnect.matchCount > 0 ? s_autoConnect.matchCount : 1,
                        s_autoConnectMatchFrame,
                        dp.measurement_valid ? dp.measured_avg_ping_ms : 0.0f,
                        Net::DelayPolicy_GetActiveDelay(),
                        rb.rollback_budget,
                        rb.rollback_count,
                        rb.max_rollback_distance,
                        rb.total_mispredictions,
                        fr.replay_verifications,
                        fr.replay_mismatches,
                        Rollback::RollbackDebug_IsDesyncDetected() ? 1 : 0);
                    Rollback::NetplayLog_Flush();
                }
                LOG_NETPLAY(LOG_INFO, "[AutoConnect] Match reached win screen after %u frames",
                    s_autoConnectMatchFrame);
                AutoConnectTransition(AutoConnectState::ConfirmingWinScreen, "entered win screen");
                break;
            }

            // Detect match end: mode changed away from match
            if (mode != MODE_MATCH) {
                LOG_NETPLAY(LOG_INFO, "[AutoConnect] Match ended (mode=%u) after %u frames",
                    mode, s_autoConnectMatchFrame);
                AutoConnectHarness_Shutdown();
                AutoConnectTransition(AutoConnectState::Failed, "match ended");
                break;
            }

            // Per-match safety timeout. It used to call
            // AutoConnectHarness_Shutdown() and go to Failed, which stops the
            // fighting AI PERMANENTLY — so on a multi-match soak the driver
            // died at the first timeout and every later "match" was two idle
            // characters. Live run 01-12-58 showed exactly that: rollbacks
            // frozen at 2703, peer_depth 0, sim still at 60 fps, for the rest
            // of the session.
            //
            // A stuck match must not end the soak. Stop DRIVING this match and
            // let the normal match-end path carry us to the next one; the
            // session, the harness and the AI all stay alive.
            if (s_autoConnect.matchDurationSec > 0) {
                int elapsedSec = (int)(s_autoConnectMatchFrame / 60);
                if (elapsedSec >= s_autoConnect.matchDurationSec) {
                    LOG_NETPLAY(LOG_WARNING,
                        "[AutoConnect] Match ran past %ds without ending — "
                        "releasing this match, harness stays live (match %d/%d)",
                        elapsedSec, s_autoConnectCompletedMatches + 1,
                        s_autoConnect.matchCount);
                    Rollback::NetplayLog_Write("AUTOCONN", -1,
                        "Match duration limit reached (%ds): released, driver kept alive",
                        elapsedSec);
                    s_autoConnectMatchFrame = 0;
                    AutoConnectTransition(AutoConnectState::ConfirmingWinScreen,
                                          "match duration limit");
                }
            }
            break;
        }

        case AutoConnectState::ConfirmingWinScreen:
            // MODE_PREMATCH_INTRO/MODE_MATCH: continue-screen rematch fast
            // path launched the next match directly (no charsel).
            // WaitingForCharSel already handles MODE_MATCH -> InMatch.
            if (mode == MODE_CHARSEL || mode == MODE_MENU ||
                mode == MODE_PREMATCH_INTRO || mode == MODE_MATCH) {
                const int targetMatches = s_autoConnect.matchCount > 0 ? s_autoConnect.matchCount : 1;
                s_autoConnectCompletedMatches++;
                LOG_NETPLAY(LOG_INFO,
                    "[AutoConnect] Win screen complete (mode=%u sub=%u) match %d/%d",
                    mode, sub, s_autoConnectCompletedMatches, targetMatches);

                if (s_autoConnectCompletedMatches >= targetMatches) {
                    if (AutoConnectHarness_IsActive()) {
                        AutoConnectHarness_Shutdown();
                    }
                    AutoConnectTransition(AutoConnectState::Disabled, "configured match count reached");
                    break;
                }

                if (mode == MODE_MENU) {
                    if (LaunchNetplayCharSel()) {
                        AutoConnectTransition(AutoConnectState::WaitingForCharSel, "next match launch from menu");
                    } else {
                        if (AutoConnectHarness_IsActive()) {
                            AutoConnectHarness_Shutdown();
                        }
                        AutoConnectTransition(AutoConnectState::Failed, "next match launch failed");
                    }
                    break;
                }

                AutoConnectTransition(AutoConnectState::WaitingForCharSel, "next match at charsel");
                break;
            }

            if (mode != MODE_WINSCREEN) {
                LOG_NETPLAY(LOG_WARNING, "[AutoConnect] Left win screen unexpectedly (mode=%u sub=%u)",
                    mode, sub);
                if (AutoConnectHarness_IsActive()) {
                    AutoConnectHarness_Shutdown();
                }
                AutoConnectTransition(AutoConnectState::Failed, "left winscreen unexpectedly");
                break;
            }

            if (sub == 3 && (!s_autoConnectWinScreenPressed ||
                             (s_autoConnectStateFrames > 300 && (s_autoConnectStateFrames % 120) == 0))) {
                // NO-route fix (2026-08-17, run 19-53-3x evidence): the
                // continue prompt consumes the winscreen LOCKSTEP stream, so
                // the sub-3 confirm tap arrives inside the prompt ~frame 2 as
                // a rising A and locks YES before this driver ever SEES
                // sub 4 (prompt lifetime was 3 frames — the sub-4 toggle
                // branch below can never win that race). ContinueFlow
                // processes cursor toggles BEFORE lock edges within a frame,
                // so injecting RIGHT+A as the confirm makes the same
                // stream-delayed word toggle the cursor to NO and lock NO
                // atomically. Both peers compute the same answer (match
                // counter is lockstep), and any-NO routes both to charsel.
                const bool answerNo = s_autoConnect.continueNoEvery > 0 &&
                    ((s_autoConnectCompletedMatches + 1) % s_autoConnect.continueNoEvery == 0);
                AutoConnectInjectPress(
                    answerNo ? (uint16_t)(INPUT_RIGHT | INPUT_A) : INPUT_A,
                    answerNo ? "confirm win screen (continue answer NO)"
                             : "confirm win screen");
                s_autoConnectWinScreenPressed = true;
            }

            // Continue prompt (sub 4, ContinueFlow): a fresh tap locks YES
            // (cursor defaults to YES). The prompt requires a release before
            // the lock edge, which the tap-style injection provides.
            // M8: with continue_no_every = K configured, every Kth prompt is
            // answered NO instead (LEFT/RIGHT toggles the cursor to NO, then
            // A locks it) so soak runs cover the any-NO charsel route too.
            if (sub == 4) {
                const bool answerNo = s_autoConnect.continueNoEvery > 0 &&
                    ((s_autoConnectCompletedMatches + 1) % s_autoConnect.continueNoEvery == 0);
                if (answerNo && !s_autoConnectContinueNoToggled) {
                    AutoConnectInjectPress(INPUT_RIGHT, "continue cursor -> NO");
                    s_autoConnectContinueNoToggled = true;
                    s_autoConnectContinueToggleFrame = s_autoConnectStateFrames;
                } else if ((!answerNo ||
                            s_autoConnectStateFrames >= s_autoConnectContinueToggleFrame + 20) &&
                           (!s_autoConnectContinuePressed ||
                            (s_autoConnectStateFrames > 300 && (s_autoConnectStateFrames % 120) == 0))) {
                    AutoConnectInjectPress(INPUT_A, answerNo ? "lock continue NO" : "lock continue YES");
                    s_autoConnectContinuePressed = true;
                }
            }

            if (s_autoConnectStateFrames > 1800) {
                LOG_NETPLAY(LOG_ERROR, "[AutoConnect] Timed out during win screen confirm");
                if (AutoConnectHarness_IsActive()) {
                    AutoConnectHarness_Shutdown();
                }
                AutoConnectTransition(AutoConnectState::Failed, "winscreen timeout");
            }
            break;

        case AutoConnectState::Failed:
            // Shut down harness if still active when we reach Failed
            if (AutoConnectHarness_IsActive()) {
                AutoConnectHarness_Shutdown();
            }
            break;

        case AutoConnectState::Disabled:
            break;
    }
}

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
    s_textCursorPos = 0;
}

static void ResetMenuInputState() {
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    s_prevCopyAddressKeyDown = false;
    ClearTextEditState();
    InputSystem_ResetRepeatState(0);
}

// ============================================================================
// Menu open / close
// ============================================================================

static void OpenMenu() {
    ModeOwnership::EnterCustomMenuContext();
    ClearActionPrompt("open_menu");
    ClearJoinSpectatorProbe("open_menu");
    s_idleSpectatorPromptDeferred = false;
    s_activeBranch = RootBranch::DirectPlay;
    s_settingsCategory = SettingsCategory::Identity;
    s_phase = MenuPhase::Opening;
    s_fadeFrames = 0;
    s_mainMenuReturnFade = 0;  // cancel any in-progress return fade
    s_captureInput = true;
    ResetMenuInputState();
    ClearError();
    SetStatus("Opening the online menu.");
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Opening custom netplay menu");
    TransitionTo(MenuState::MenuRoot, "Network selected");
}

static void FinishClose() {
    if (s_joinSpectatorProbeActive) {
        Net::SpectatorClient_Disconnect("menu close cleared join spectator probe");
    }
    ClearActionPrompt("finish_close");
    ClearJoinSpectatorProbe("finish_close");
    s_idleSpectatorPromptDeferred = false;
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    ClearTextEditState();
    s_waitForNeutral = true;
    s_selectedIndex = 0;
    SetStatus("Choose an online option.");
    TransitionTo(MenuState::Inactive, "menu closed");
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Custom netplay menu closed");
    InputSystem_ResetRepeatState(0);
    // Restart BGM 74 on the next open (the main menu reclaims track 0 on close).
    // Keep the cached net.bin / wave\net.bin handles: while parked in MODE_MENU
    // they survive, so reopening the menu is instant. They are invalidated
    // separately once the game leaves MODE_MENU (handles freed by the engine).
    s_bgmStarted = false;
}

static void BeginClose(const char* why) {
    if (!MenuVisible() || s_phase == MenuPhase::Closing) return;
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Closing custom menu (%s)", why ? why : "?");
    SetStatus("Closing the online menu.");
    s_phase = MenuPhase::Closing;
    s_waitForNeutral = true;
    ClearTextEditState();
}

// ============================================================================
// Disconnect / error
// ============================================================================

static MenuState ResolveDisconnectReturnMenu() {
    if (s_connectionEntry == ConnectionEntry::Join || s_joinSpectatorProbeActive) {
        return MenuState::JoinEntry;
    }
    if (s_connectionEntry == ConnectionEntry::Host) {
        return MenuState::HostEntry;
    }
    if (s_activeBranch == RootBranch::Spectate) {
        return MenuState::SpectateEntry;
    }
    if (s_activeBranch == RootBranch::Settings) {
        return MenuState::SettingsCategoryMenu;
    }
    if (s_activeBranch == RootBranch::DirectPlay) {
        return MenuState::DirectConnectEntry;
    }
    return MenuState::MenuRoot;
}

// Player-facing wording. The raw text stays in the log; hashes, packet names
// and internal state never reach the screen.
static const char* FriendlyError(const char* raw) {
    if (!raw || !raw[0]) return "Disconnected.";
    struct Rule { const char* needle; const char* text; };
    static const Rule kRules[] = {
        { "build hash mismatch",   "The other player is running a different version of the mod." },
        { "build fingerprint",     "Couldn't verify this installation's mod files." },
        { "Baseline digest",       "Couldn't start the match: the two games disagreed on the starting state." },
        { "Config rejected",       "The other player's match settings didn't match yours." },
        { "confirmed-desync",      "The match went out of sync and had to stop." },
        { "sync hash mismatch",    "The match went out of sync and had to stop." },
        { "ProtocolViolation",     "The other player sent something this version didn't understand." },
        { "Failed to send Session","Lost contact with the other player while setting up." },
        { "network worker",        "Couldn't start networking on this machine." },
        { "IPv6",                  "IPv6 addresses aren't supported yet — use an IPv4 address." },
        { "Remote canceled",       "The other player left." },
        { "stopped responding",    "The other player stopped responding." },
        { "timed out",             "The connection timed out." },
    };
    for (const Rule& r : kRules) {
        if (ContainsInsensitive(raw, r.needle)) return r.text;
    }
    return raw;
}

static void OpenDisconnectError(const char* why) {
    const char* rollbackReason = Rollback::RollbackSession_GetErrorReason();
    const char* effectiveWhy = why;
    if (!effectiveWhy || !effectiveWhy[0]) {
        effectiveWhy = (rollbackReason && rollbackReason[0]) ? rollbackReason : "Disconnected.";
    }

    uint32_t currentMode = GetGameMode();
    LOG_NETPLAY(LOG_WARNING, "[NetMenu] OpenDisconnectError: reason='%s' mode=%u",
        effectiveWhy, currentMode);
    Net::SpectatorClient_Disconnect("disconnect error");
    ClearActionPrompt("disconnect_error");
    s_disconnectReturnState = ResolveDisconnectReturnMenu();
    ClearJoinSpectatorProbe("disconnect_error");
    s_idleSpectatorPromptDeferred = false;

    // Notify match lifecycle layer of disconnect
    if (Net::MatchLifecycle_IsMatchOwned()) {
        Net::MatchLifecycle_OnDisconnect(effectiveWhy);
    }
    Rollback::OnlineWiring_OnDisconnect(effectiveWhy);

    // Abort any in-progress pre-game sync
    Net::PregameSync_Abort(effectiveWhy);
    Net::FrontendInputSync_AbortEpoch(effectiveWhy);

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
    SetError("%s", FriendlyError(why));
    SetStatus("%s", FriendlyError(why));
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

static bool ShouldRecoverHiddenSessionLoss(const Net::SessionSnapshot& snap) {
    if (MenuVisible() || s_state == MenuState::DisconnectError) {
        return false;
    }

    const bool pregameActive = Net::PregameSync_GetPhase() != Net::PregamePhase::Idle;
    const bool matchOwned = Net::MatchLifecycle_IsMatchOwned();
    if (!pregameActive && !matchOwned) {
        return false;
    }

    return snap.state == Net::SessionState::Failed ||
           snap.state == Net::SessionState::Idle;
}

static const char* HiddenSessionLossReason(const Net::SessionSnapshot& snap) {
    if (snap.error_text[0]) {
        return snap.error_text;
    }
    if (Net::MatchLifecycle_IsMatchOwned()) {
        return "Session lost during online match.";
    }
    return "Session lost during pre-game sync.";
}

static void SyncSessionState() {
    // Always drain session events on the game thread.
    // The ENet transport itself is serviced independently on the network worker.
    Net::Session_Update();

    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);

    if (ShouldRecoverHiddenSessionLoss(snap)) {
        const char* reason = HiddenSessionLossReason(snap);
        LOG_NETPLAY(LOG_WARNING,
            "[NetMenu] Hidden netplay flow lost session; forcing recovery: state=%s reason='%s'",
            Net::SessionStateName(snap.state),
            reason ? reason : "?");
        OpenDisconnectError(reason);
        return;
    }

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
            // Show ConnectedSession config/accept screen
            ClearJoinSpectatorProbe("session_connected");
            if (s_state == MenuState::Connecting || s_state == MenuState::Handshake) {
                LOG_NETPLAY(LOG_INFO, "[NetMenu] Session connected — opening config screen");
                s_activeBranch = RootBranch::DirectPlay;
                TransitionTo(MenuState::ConnectedSession, "session connected");
                SetStatus("%s", snap.status_text[0] ? snap.status_text : "Session connected.");
                s_selectedIndex = 0;
            } else if (s_state != MenuState::ConnectedSession && s_state != MenuState::CharSelTransition) {
                s_activeBranch = RootBranch::DirectPlay;
                TransitionTo(MenuState::ConnectedSession, "session connected");
                SetStatus("%s", snap.status_text[0] ? snap.status_text : "Session connected.");
                s_selectedIndex = 0;
            }
            break;
        case Net::SessionState::Ready:
            // Both peers accepted — auto-launch character selection
            ClearJoinSpectatorProbe("session_ready");
            if (s_state == MenuState::ConnectedSession ||
                s_state == MenuState::Connecting ||
                s_state == MenuState::Handshake) {
                LOG_NETPLAY(LOG_INFO, "[NetMenu] Both peers accepted — launching charsel");
                LaunchNetplayCharSel();
            }
            break;
        case Net::SessionState::Failed:
            if (s_state != MenuState::DisconnectError) {
                // If we're sitting at an idle menu screen (not actively in a
                // connection flow), silently clear the stale failed session
                // instead of showing a disconnect error the user never asked for.
                const bool inConnectionFlow =
                    s_state == MenuState::Connecting ||
                    s_state == MenuState::Handshake ||
                    s_state == MenuState::ConnectedSession ||
                    s_state == MenuState::CharSelTransition ||
                    s_state == MenuState::PostMatch ||
                    s_state == MenuState::SpectatorConnecting ||
                    s_state == MenuState::SpectatorConnected;
                if (!inConnectionFlow) {
                    LOG_NETPLAY(LOG_INFO, "[NetMenu] Silently clearing stale failed session (menu state=%d, reason='%s')",
                        (int)s_state, snap.error_text[0] ? snap.error_text : "?");
                    Net::Session_Cancel();
                    break;
                }
                const bool isJoinConnectFailure =
                    snap.role == Net::SessionRole::Join &&
                    (s_state == MenuState::Connecting || s_state == MenuState::Handshake) &&
                    !s_joinSpectatorProbeActive;
                if (isJoinConnectFailure &&
                    ShouldAttemptJoinSpectatorProbe(snap.error_text) &&
                    BeginJoinSpectatorProbe(snap.error_text[0] ? snap.error_text : "Session error.")) {
                    break;
                }
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
    ClearActionPrompt("hide_for_launch");
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

    // A fresh netplay launch can follow practice/offline/previous online play
    // without process restart. Clear match-owned residue before CharSel so
    // baseline capture starts from the same canonical scratch state on both peers.
    Rollback::RematchCleanup_PrepareForNetplayLaunch("netplay charsel launch");

    // Do NOT cancel session — keep it alive. Only hide the menu UI.
    HideMenuForLaunch("netplay charsel");

    // Use offline VS Human mode so vanilla netplay sync never activates.
    // The mod relays remote input via its own hooks.
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ModeOwnership::ClearVanillaNetplayFlags();
    WriteU8(ADDR_P1_CPU_FLAG, 0);
    WriteU8(ADDR_P2_CPU_FLAG, 0);
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
        LOG_NETPLAY(LOG_WARNING, "[NetMenu] Failed to start pre-game sync — returning to menu");
        // Undo game mode change and restore menu
        ModeOwnership::CallOriginalSetGameMode(MODE_MENU, 1);
        s_phase = MenuPhase::Active;
        TransitionTo(MenuState::ConnectedSession, "pregame begin failed");
        return false;
    }

    return true;
}

static void TryAutoRestartPregameFromPostMatchCharSel() {
    if (MenuVisible()) return;

    // Continue-screen rematch in flight: the fast path owns the restart (the
    // game never reaches CharSel), so the charsel auto-restart must not race
    // it. Once the fast path is running, PregameSync_IsActive() below covers
    // the remaining window (ConfigExchange onward is an active phase).
    if (Net::ContinueFlow_IsRematchLatched()) {
        return;
    }

    const uint32_t mode = GetGameMode();
    if (mode != MODE_CHARSEL) {
        s_autoRematchCleanupApplied = false;
        s_autoRematchLastAttemptAt = 0;
        return;
    }

    Net::SessionSnapshot session{};
    Net::Session_GetSnapshot(&session);
    if (!session.active ||
        (session.state != Net::SessionState::Connected &&
         session.state != Net::SessionState::Ready)) {
        return;
    }

    const Net::PregamePhase prePhase = Net::PregameSync_GetPhase();
    if (Net::PregameSync_IsActive() || Net::CharSelSync_IsLockstepActive()) {
        // Pregame lockstep already owns CharSel again.
        s_autoRematchCleanupApplied = true;
        return;
    }

    if (Net::WinScreenSync_IsActive()) {
        return;
    }

    const Net::MatchLifecyclePhase lifePhase = Net::MatchLifecycle_GetPhase();
    if (lifePhase == Net::MatchLifecyclePhase::MatchEnd ||
        lifePhase == Net::MatchLifecyclePhase::WinScreenActive) {
        if (!Net::WinScreenSync_IsHandoffComplete()) {
            return;
        }
    }

    // Primary rematch signature: previous pregame run ended in GameplayHandoff
    // and the game routed back to CharSel from win screen.
    const bool staleGameplayHandoff = (prePhase == Net::PregamePhase::GameplayHandoff);

    const bool lifecycleSuggestsPostMatch =
        Net::MatchLifecycle_IsPostMatchRouting() ||
        Net::MatchLifecycle_GetPhase() == Net::MatchLifecyclePhase::PostMatchRoute ||
        Net::MatchLifecycle_GetPhase() == Net::MatchLifecyclePhase::ReturningToCharSel;

    // Wire-driven signatures (M4): the local heuristics above have ~one-frame
    // lifetimes and can be destroyed by aborts; the peer's winscreen-exit
    // barrier and rematch intent arrive over the wire and cannot be missed.
    const uint8_t remotePmdIntent =
        Net::TransitionBarrier_RemoteProposed(Net::NetTransitionKind::PostMatchDecision)
            ? Net::TransitionBarrier_GetRemoteIntent(Net::NetTransitionKind::PostMatchDecision)
            : (uint8_t)Net::PostMatchIntentWire::None;
    const bool wireSuggestsRematch =
        Net::TransitionBarrier_IsCommitted(Net::NetTransitionKind::WinScreenExit) ||
        remotePmdIntent == (uint8_t)Net::PostMatchIntentWire::Rematch ||
        Net::PostMatchIntentIsRestart(remotePmdIntent);

    if (!staleGameplayHandoff && !lifecycleSuggestsPostMatch && !wireSuggestsRematch) {
        return;
    }

    // Announce our own restart intent so the peer's restart doesn't depend on
    // ITS local heuristics either. M7 (F-7 unification): this path IS the
    // lockstep-derived "any NO" charsel route, so it announces CharselRestart
    // — the same value continue_flow's decline proposed — never Rematch
    // (which now exclusively means the YES,YES fast path).
    // RECOVERY route, not a lockstep decision. Announcing CharselRestart here
    // made this indistinguishable from continue_flow's decline, and the
    // director's F-7 guard terminated the session as a protocol violation
    // whenever the peer had cleanly resolved Rematch instead.
    Net::TransitionBarrier_Propose(Net::NetTransitionKind::PostMatchDecision,
                                   (uint8_t)Net::PostMatchIntentWire::RecoveryRestart, 0);

    const DWORD now = GetTickCount();
    if ((now - s_autoRematchLastAttemptAt) < 250) {
        return;
    }
    s_autoRematchLastAttemptAt = now;

    if (!s_autoRematchCleanupApplied) {
        LOG_NETPLAY(LOG_INFO,
            "[NetMenu] Auto-rematch handoff detected (phase=%s lifecycle=%s) — preparing next match",
            Net::PregamePhaseName(prePhase),
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
        Rollback::OnlineWiring_OnRematch();
        s_autoRematchCleanupApplied = true;
    }

    if (Net::PregameSync_Begin()) {
        LOG_NETPLAY(LOG_INFO,
            "[NetMenu] Auto-rematch pregame restart started on CharSel");
        // Retire this boundary's barriers so the next match starts clean —
        // in ladder order (INV-9, M6), reporting each consume to the
        // director's match-end ladder gate.
        if (Net::TransitionBarrier_ConsumeCommit(Net::NetTransitionKind::WinScreenExit)) {
            Rollback::OnlineWiring_MatchEndLadderNotifyConsumed(
                Net::NetTransitionKind::WinScreenExit);
        }
        if (Rollback::OnlineWiring_MatchEndLadderAllows(
                Net::NetTransitionKind::PostMatchDecision) &&
            Net::TransitionBarrier_ConsumeCommit(Net::NetTransitionKind::PostMatchDecision)) {
            Rollback::OnlineWiring_MatchEndLadderNotifyConsumed(
                Net::NetTransitionKind::PostMatchDecision);
        }
    } else {
        LOG_NETPLAY(LOG_WARNING,
            "[NetMenu] Auto-rematch pregame restart deferred (phase=%s lifecycle=%s)",
            Net::PregamePhaseName(Net::PregameSync_GetPhase()),
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
    }
}

// ============================================================================
// Item counts
// ============================================================================

static int ItemCount(MenuState st) {
    switch (st) {
        case MenuState::MenuRoot:            return 4; // Host, Join, Settings, Close
        case MenuState::DirectConnectEntry:  return 3; // Host, Join, Back
        case MenuState::HostEntry:           return 3; // Host, Listen Port, Back
        case MenuState::JoinEntry:           return 3; // Join, Remote Endpoint, Back
        case MenuState::SpectateEntry:       return 4; // Connect, Discover LAN, Endpoint, Back
        case MenuState::SpectatorConnecting: return 1; // Cancel
        case MenuState::SpectatorConnected:  return 1; // Disconnect
        case MenuState::SettingsCategoryMenu: return 6; // Player, Appearance, Network, Watch, Diagnostics, Back
        case MenuState::SettingsEntry: {
            switch (s_settingsCategory) {
                case SettingsCategory::Identity:     return 4; // Name, Delay, Rollback, Back
                case SettingsCategory::Appearance:   return 8; // Trail, Text, Score, Length, Position, Font, Render, Back
                case SettingsCategory::Endpoint:     return 8; // Route, UPnP, STUN, Hole, IPv6, Relay, STUN srv, Back
                case SettingsCategory::SessionMatch: return 4; // Watchers, PalSync, PalPreview, Back
                case SettingsCategory::Diagnostics:  return 2; // Debug logging, Back
                default: return 5;
            }
        }
        case MenuState::Connecting:          return 1; // Cancel
        case MenuState::Handshake:           return 1; // Cancel
        case MenuState::ConnectedSession:    return 4; // Rollback Frames, Input Delay, Launch CharSel, Disconnect
        case MenuState::CharSelTransition:   return 2; // Launch, Back
        case MenuState::PostMatch:           return 3; // Rematch, Return, Disconnect
        case MenuState::DisconnectError:     return 2; // OK, Close Menu
        default: return 0;
    }
}

// Map (category, local index) -> original flat setting index (0-15)
// Returns -1 for "Back" items or invalid
static int SettingGlobalId() {
    if (s_state != MenuState::SettingsEntry) return -1;
    switch (s_settingsCategory) {
        case SettingsCategory::Identity:
            switch (s_selectedIndex) {
                case 0: return 0;   // Display Name
                case 1: return 1;   // Input Delay
                case 2: return 2;   // Max Rollback
                default: return -1; // Back
            }
        case SettingsCategory::Appearance:
            switch (s_selectedIndex) {
                case 0: return 17;  // Trail color
                case 1: return 18;  // Text color
                case 2: return 19;  // Score color
                case 3: return 20;  // Trail length
                case 4: return 21;  // Vertical position
                case 5: return 22;  // Font size
                case 6: return 23;  // Render mode
                default: return -1; // Back
            }
        case SettingsCategory::Endpoint:
            switch (s_selectedIndex) {
                case 0: return 4;   // Route
                case 1: return 5;   // UPnP
                case 2: return 6;   // STUN
                case 3: return 7;   // UDP Hole Punch
                case 4: return 8;   // Allow IPv6
                case 5: return 9;   // Punch Relay
                case 6: return 10;  // STUN Server
                default: return -1; // Back
            }
        case SettingsCategory::SessionMatch:
            switch (s_selectedIndex) {
                case 0: return 11;  // Watchers
                case 1: return 13;  // Sync Palettes
                case 2: return 14;  // Preview Remote
                default: return -1; // Back
            }
        case SettingsCategory::Diagnostics:
            switch (s_selectedIndex) {
                case 0: return 15;  // Debug Logging
                default: return -1; // Back
            }
        default: return -1;
    }
}

static void MoveSelection(int delta) {
    int count = ItemCount(s_state);
    if (count <= 0) { s_selectedIndex = 0; return; }
    int next = (int)s_selectedIndex + delta;
    while (next < 0) next += count;
    while (next >= count) next -= count;
    if ((uint32_t)next != s_selectedIndex) {
        PlayMenuCursorSfx();
    }
    s_selectedIndex = (uint32_t)next;
}

// ============================================================================
// Input helpers
// ============================================================================

static bool MenuJustPressed(uint16_t button) { return InputSystem_JustPressed(0, button); }
static bool ConfirmPressed() { return MenuJustPressed(INPUT_A) || MenuJustPressed(INPUT_START); }
static bool BackPressed()    { return MenuJustPressed(INPUT_B) || MenuJustPressed(INPUT_SELECT); }

static void ResetTextEditKeyState();

// ============================================================================
// Text editing (keyboard input for nickname/endpoint/port)
// ============================================================================

static void BeginTextEdit(TextEditField field, const char* initial, const char* status) {
    s_textEditField = field;
    CopyText(s_textEditBuffer, sizeof(s_textEditBuffer), initial ? initial : "");
    s_textCursorPos = (int)strlen(s_textEditBuffer);
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

    TrimWhitespace(s_textEditBuffer);

    // Apply committed edits
    if (field == TextEditField::Nickname && s_textEditBuffer[0]) {
        CopyText(s_localNickname, sizeof(s_localNickname), s_textEditBuffer);
        LOG_NETPLAY(LOG_INFO, "[NetMenu] Nickname set to: %s", s_localNickname);
        SetStatus("Display name: %s", s_localNickname);
    } else if (field == TextEditField::ListenPort && s_textEditBuffer[0]) {
        int port = atoi(s_textEditBuffer);
        if (port > 0 && port <= 65535) {
            s_listenPort = (uint16_t)port;
            LOG_NETPLAY(LOG_INFO, "[NetMenu] Listen port set to: %u", s_listenPort);
            SetStatus("Room port: %u", s_listenPort);
        } else {
            SetStatus("Enter a valid room port from 1 to 65535.");
        }
    } else if (field == TextEditField::RemoteEndpoint && s_textEditBuffer[0]) {
        char testHost[96] = {};
        uint16_t testPort = 0;
        if (ParseEndpoint(s_textEditBuffer, testHost, sizeof(testHost), &testPort, s_allowIPv6Endpoint)) {
            CopyText(s_remoteEndpoint, sizeof(s_remoteEndpoint), s_textEditBuffer);
            LOG_NETPLAY(LOG_INFO, "[NetMenu] Remote endpoint set to: %s", s_remoteEndpoint);
            SetStatus("Host address: %s", s_remoteEndpoint);
        } else {
            SetStatus("Enter an address as host:port or [ipv6]:port.");
        }
    } else if (field == TextEditField::SpectatorEndpoint && s_textEditBuffer[0]) {
        char testHost[96] = {};
        uint16_t testPort = 0;
        if (ParseEndpoint(s_textEditBuffer, testHost, sizeof(testHost), &testPort, true)) {
            CopyText(s_spectatorEndpoint, sizeof(s_spectatorEndpoint), s_textEditBuffer);
            SPECTATE_MENU_LOG(LOG_INFO, "SMENU", "[NetMenu] Spectator endpoint set to: %s", s_spectatorEndpoint);
            SetStatus("Watch address: %s", s_spectatorEndpoint);
        } else {
            SetStatus("Enter a valid watch address.");
        }
    } else if (field == TextEditField::SpectatorPort && s_textEditBuffer[0]) {
        int port = atoi(s_textEditBuffer);
        if (port > 0 && port <= 65535) {
            s_spectatorListenPort = (uint16_t)port;
            ApplySpectatorSettingsToRuntime("text edit commit");
            SPECTATE_MENU_LOG(LOG_INFO, "SMENU", "[NetMenu] Spectator listen port set to: %u", s_spectatorListenPort);
            SetStatus("Watch port: %u", s_spectatorListenPort);
        } else {
            SetStatus("Enter a valid watch port from 1 to 65535.");
        }
    } else if (field == TextEditField::RelayEndpoint) {
        if (!s_textEditBuffer[0]) {
            s_relayEndpoint[0] = '\0';
            SetStatus("Punch relay reset to the default server.");
        } else {
            char testHost[96] = {};
            uint16_t testPort = 0;
            if (ParseEndpoint(s_textEditBuffer, testHost, sizeof(testHost), &testPort, true)) {
                CopyText(s_relayEndpoint, sizeof(s_relayEndpoint), s_textEditBuffer);
                LOG_NETPLAY(LOG_INFO, "[NetMenu] Punch relay endpoint set to: %s", s_relayEndpoint);
                SetStatus("Punch relay set to custom server.");
            } else {
                SetStatus("Enter a valid punch relay address.");
            }
        }
    } else if (field == TextEditField::StunEndpoint) {
        if (!s_textEditBuffer[0]) {
            CopyText(s_stunEndpoint, sizeof(s_stunEndpoint), "stun.l.google.com:19302");
            SetStatus("STUN server reset to the default address.");
        } else {
            char testHost[96] = {};
            uint16_t testPort = 0;
            if (ParseEndpoint(s_textEditBuffer, testHost, sizeof(testHost), &testPort, true)) {
                CopyText(s_stunEndpoint, sizeof(s_stunEndpoint), s_textEditBuffer);
                LOG_NETPLAY(LOG_INFO, "[NetMenu] STUN endpoint set to: %s", s_stunEndpoint);
                SetStatus("STUN server: %s", s_stunEndpoint);
            } else {
                SetStatus("Enter a valid STUN server address.");
            }
        }
    }

    ClearTextEditState();
    s_waitForNeutral = true;
    InputSystem_ResetRepeatState(0);

    // Persist settings after any committed change
    if (commit) {
        ApplyNatSettingsToService("text edit commit");
        ApplySpectatorSettingsToRuntime("text edit commit");
        SaveSettings();
    }
}

static void HandleTextEditing() {
    if (!IsTextEditWindowFocused()) {
        ResetTextEditKeyState();
        return;
    }

    bool ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (!s_textEditPrevKeyInitialized) {
        for (int vk = 0; vk < 256; ++vk) {
            s_textEditPrevKeyDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
        s_textEditPrevKeyInitialized = true;
        return;
    }

    size_t maxEditLen = sizeof(s_textEditBuffer) - 1;
    if (s_textEditField == TextEditField::Nickname) {
        maxEditLen = sizeof(s_localNickname) - 1;
    } else if (s_textEditField == TextEditField::ListenPort ||
               s_textEditField == TextEditField::SpectatorPort) {
        maxEditLen = 5;
    } else if (IsEndpointField(s_textEditField)) {
        maxEditLen = sizeof(s_remoteEndpoint) - 1;
    }

    int len = (int)strlen(s_textEditBuffer);
    AlignCursorToUtf8Boundary(s_textEditBuffer, &s_textCursorPos);

    {
        bool vDown = (GetAsyncKeyState('V') & 0x8000) != 0;
        if (ctrlDown && vDown && !s_textEditPrevKeyDown['V']) {
            char pasteBuffer[128] = {};
            if (MenuUtils::PasteFromClipboard(pasteBuffer, sizeof(pasteBuffer))) {
                InsertClipboardTextAtCursor(s_textEditField, maxEditLen, pasteBuffer);
            }
            s_textEditPrevKeyDown['V'] = true;
            return;
        }
    }

    {
        bool aDown = (GetAsyncKeyState('A') & 0x8000) != 0;
        if (ctrlDown && aDown && !s_textEditPrevKeyDown['A']) {
            s_textEditBuffer[0] = '\0';
            s_textCursorPos = 0;
            s_textEditPrevKeyDown['A'] = true;
            return;
        }
    }

    for (int vk = 0; vk < 256; ++vk) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !s_textEditPrevKeyDown[vk]) {
            len = (int)strlen(s_textEditBuffer);

            if (vk == VK_RETURN) {
                ResetTextEditKeyState();
                FinishTextEdit(true);
                return;
            } else if (vk == VK_ESCAPE) {
                ResetTextEditKeyState();
                FinishTextEdit(false);
                return;
            }

            else if (vk == VK_LEFT) {
                s_textCursorPos = ctrlDown
                    ? PreviousWordBoundary(s_textEditBuffer, s_textCursorPos)
                    : PreviousUtf8Boundary(s_textEditBuffer, s_textCursorPos);
            }
            else if (vk == VK_RIGHT) {
                s_textCursorPos = ctrlDown
                    ? NextWordBoundary(s_textEditBuffer, s_textCursorPos)
                    : NextUtf8Boundary(s_textEditBuffer, s_textCursorPos);
            }

            else if (vk == VK_HOME) {
                s_textCursorPos = 0;
            }
            else if (vk == VK_END) {
                s_textCursorPos = len;
            }

            else if (vk == VK_BACK) {
                if (ctrlDown) {
                    memmove(s_textEditBuffer, s_textEditBuffer + s_textCursorPos, len - s_textCursorPos + 1);
                    s_textCursorPos = 0;
                } else if (s_textCursorPos > 0) {
                    const int eraseFrom = PreviousUtf8Boundary(s_textEditBuffer, s_textCursorPos);
                    memmove(s_textEditBuffer + eraseFrom,
                            s_textEditBuffer + s_textCursorPos,
                            len - s_textCursorPos + 1);
                    s_textCursorPos = eraseFrom;
                }
            }

            else if (vk == VK_DELETE) {
                if (ctrlDown) {
                    s_textEditBuffer[s_textCursorPos] = '\0';
                } else if (s_textCursorPos < len) {
                    const int eraseTo = NextUtf8Boundary(s_textEditBuffer, s_textCursorPos);
                    memmove(s_textEditBuffer + s_textCursorPos,
                            s_textEditBuffer + eraseTo,
                            len - eraseTo + 1);
                }
            }

            else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
                const wchar_t digit = (wchar_t)(L'0' + (vk - VK_NUMPAD0));
                InsertWideTextAtCursor(s_textEditField, maxEditLen, &digit, 1);
            }

            else if (!ctrlDown) {
                wchar_t translated[8] = {};
                const int translatedCount = TranslateVirtualKeyToUnicode(vk, translated, (int)_countof(translated));
                if (translatedCount > 0) {
                    InsertWideTextAtCursor(s_textEditField, maxEditLen, translated, translatedCount);
                }
            }
        }
        s_textEditPrevKeyDown[vk] = down;
    }
}

/// Reset the text editing key state tracker (call when entering/leaving text edit)
static void ResetTextEditKeyState() {
    memset(s_textEditPrevKeyDown, 0, sizeof(s_textEditPrevKeyDown));
    s_textEditPrevKeyInitialized = false;
}

// ============================================================================
// Navigation input (when not text editing)
// ============================================================================

static void ActivateCurrentSelection();
static void HandleBackNavigation();

static void ApplyDiscoveredSpectatorEndpoint(const Net::SpectatorDiscoveryEntry& entry,
                                             uint32_t index,
                                             uint32_t count,
                                             const char* reason) {
    if (!entry.endpoint[0]) {
        return;
    }

    CopyText(s_spectatorEndpoint, sizeof(s_spectatorEndpoint), entry.endpoint);
    SPECTATE_MENU_LOG(LOG_INFO, "SMENU",
        "[NetMenu] Spectator LAN endpoint selected (%s): index=%u/%u endpoint=%s host=%s match_active=%d",
        reason ? reason : "unspecified",
        (unsigned)(index + 1),
        (unsigned)count,
        entry.endpoint,
        entry.host_nickname[0] ? entry.host_nickname : "?",
        entry.match_active ? 1 : 0);

    if (entry.match_active) {
        SetStatus("LAN room %u/%u: %s (%s vs %s)",
            (unsigned)(index + 1),
            (unsigned)count,
            entry.endpoint,
            entry.p1_name[0] ? entry.p1_name : "P1",
            entry.p2_name[0] ? entry.p2_name : "P2");
    } else {
        SetStatus("LAN room %u/%u: %s (waiting for a match)",
            (unsigned)(index + 1),
            (unsigned)count,
            entry.endpoint);
    }
}

static void SyncSpectatorDiscoveryState() {
    if (!MenuVisible()) {
        return;
    }

    Net::SpectatorDiscoverySnapshot discovery{};
    Net::SpectatorClient_GetDiscoverySnapshot(&discovery);

    if (discovery.result_count > 0) {
        if (s_selectedLanSpectatorIndex >= discovery.result_count) {
            s_selectedLanSpectatorIndex = 0;
        }

        const Net::SpectatorDiscoveryEntry& selected =
            discovery.results[s_selectedLanSpectatorIndex];
        if (selected.endpoint[0] &&
            (strcmp(selected.endpoint, s_lastLanSpectatorEndpoint) != 0 ||
             discovery.result_count != s_lastLanSpectatorResultCount ||
             discovery.active != s_lastLanSpectatorDiscoveryActive)) {
            CopyText(s_lastLanSpectatorEndpoint,
                sizeof(s_lastLanSpectatorEndpoint),
                selected.endpoint);
            if (s_state == MenuState::SpectateEntry && !IsTextEditing()) {
                ApplyDiscoveredSpectatorEndpoint(
                    selected,
                    s_selectedLanSpectatorIndex,
                    discovery.result_count,
                    discovery.active ? "discovery update" : "discovery complete");
            }
        }
    } else if (!discovery.active &&
               s_lastLanSpectatorDiscoveryActive &&
               s_state == MenuState::SpectateEntry &&
               discovery.status[0]) {
        s_lastLanSpectatorEndpoint[0] = '\0';
        SetStatus("%s", discovery.status);
    } else if (discovery.active &&
               !s_lastLanSpectatorDiscoveryActive &&
               s_state == MenuState::SpectateEntry &&
               discovery.status[0]) {
        SetStatus("%s", discovery.status);
    }

    s_lastLanSpectatorResultCount = discovery.result_count;
    s_lastLanSpectatorDiscoveryActive = discovery.active;
}

static void SyncSpectatorClientState() {
    if (!MenuVisible()) {
        return;
    }

    Net::SpectatorClientSnapshot spectator{};
    Net::SpectatorClient_GetSnapshot(&spectator);
    char activeEndpoint[96] = {};
    CopyDisplayedSpectatorEndpoint(activeEndpoint, sizeof(activeEndpoint), &spectator);

    if (spectator.state == Net::SpectatorClientState::Redirected && spectator.redirect_endpoint[0]) {
        SetStatus("This watch address redirected you to %s.", spectator.redirect_endpoint);
        SPECTATE_MENU_LOG(LOG_INFO, "SPROBE",
            "[SPROBE] redirect endpoint=%s",
            spectator.redirect_endpoint);
        if (!Net::SpectatorClient_StartConnect(spectator.redirect_endpoint)) {
            Net::SpectatorClientSnapshot restartAttempt{};
            Net::SpectatorClient_GetSnapshot(&restartAttempt);
            const char* connectError = restartAttempt.error[0]
                ? restartAttempt.error
                : "Failed to follow spectator redirect.";
            if (s_joinSpectatorProbeActive) {
                ClearJoinSpectatorProbe("redirect_failure");
                OpenDisconnectError(connectError);
            } else {
                SetError("%s", connectError);
                SetStatus("%s", connectError);
                s_selectedIndex = 0;
                TransitionTo(MenuState::SpectateEntry, "spectator redirect failed");
            }
        }
        return;
    }

    if (s_state != MenuState::SpectateEntry &&
        s_state != MenuState::SpectatorConnecting &&
        s_state != MenuState::SpectatorConnected) {
        return;
    }

    switch (spectator.state) {
        case Net::SpectatorClientState::Connecting:
        case Net::SpectatorClientState::Handshaking:
        case Net::SpectatorClientState::Redirected:
            s_idleSpectatorPromptDeferred = false;
            if (s_state != MenuState::SpectatorConnecting) {
                if (!s_joinSpectatorProbeActive) {
                    s_activeBranch = RootBranch::Spectate;
                }
                s_selectedIndex = 0;
                TransitionTo(MenuState::SpectatorConnecting, "spectator connecting");
            }
            break;

        case Net::SpectatorClientState::ConnectedNoActiveMatch:
            if (s_joinSpectatorProbeActive) {
                ClearActionPrompt("join_probe_idle_host");
                ClearJoinSpectatorProbe("connected_waiting_for_match");
                OpenDisconnectError("The host is not currently in an active match.");
                return;
            }
            if (s_state != MenuState::SpectatorConnected) {
                s_activeBranch = RootBranch::Spectate;
                s_selectedIndex = 0;
                TransitionTo(MenuState::SpectatorConnected, "spectator waiting for active match");
            }
            if (!s_idleSpectatorPromptDeferred && !IsActionPromptOpen()) {
                OpenIdleSpectatorPrompt(spectator, activeEndpoint);
            }
            break;

        case Net::SpectatorClientState::Streaming:
            s_idleSpectatorPromptDeferred = false;
            if (s_joinSpectatorProbeActive) {
                SPECTATE_MENU_LOG(LOG_INFO, "SPROBE",
                    "[SPROBE] active_match_available=1 switching_to_spectate=1 endpoint=%s",
                    activeEndpoint[0] ? activeEndpoint : "(unset)");
            }
            if (s_state != MenuState::SpectatorConnected) {
                if (!s_joinSpectatorProbeActive) {
                    s_activeBranch = RootBranch::Spectate;
                }
                s_selectedIndex = 0;
                TransitionTo(MenuState::SpectatorConnected, "spectator streaming");
            }
            if (s_joinSpectatorProbeActive && s_actionPromptKind != ActionPromptKind::JoinAsSpectator) {
                OpenJoinAsSpectatorPrompt(spectator, activeEndpoint);
            } else if (IsActionPromptOpen() &&
                       s_actionPromptKind == ActionPromptKind::JoinInsteadOfWaiting) {
                ClearActionPrompt("active_match_started");
                SetStatus("A match just started. You're now watching.");
            }
            break;

        case Net::SpectatorClientState::Failed:
            s_idleSpectatorPromptDeferred = false;
            ClearActionPrompt("spectator_failed");
            if (s_joinSpectatorProbeActive) {
                char errorBuf[128] = {};
                ResolveJoinSpectatorProbeFailureMessage(spectator, errorBuf, sizeof(errorBuf));
                if (IsSpectatorNoActiveMatchError(spectator.error)) {
                    SPECTATE_MENU_LOG(LOG_INFO, "SPROBE",
                        "[SPROBE] active_match_available=0 reason=no_active_match endpoint=%s",
                        activeEndpoint[0] ? activeEndpoint : "(unset)");
                    ClearJoinSpectatorProbe("no_active_match");
                } else if (IsSpectatorEndpointTimeoutError(spectator.error)) {
                    if (TryJoinSpectatorProbeFallback(spectator.error)) {
                        return;
                    }
                    SPECTATE_MENU_LOG(LOG_WARNING, "SPROBE",
                        "[SPROBE] timeout endpoint=%s",
                        activeEndpoint[0] ? activeEndpoint : "(unset)");
                    ClearJoinSpectatorProbe("timeout");
                } else {
                    SPECTATE_MENU_LOG(LOG_WARNING, "SPROBE",
                        "[SPROBE] failure endpoint=%s reason=%s",
                        activeEndpoint[0] ? activeEndpoint : "(unset)",
                        errorBuf[0] ? errorBuf : "unknown");
                    ClearJoinSpectatorProbe("failure");
                }
                // A failed spectator PROBE is not a player-session loss —
                // routing it through OpenDisconnectError added a full
                // error-screen cycle to every join retry. Return to the join
                // screen with the reason shown instead.
                SetError("%s", errorBuf[0] ? errorBuf : "Could not reach the host.");
                SetStatus("%s", errorBuf[0] ? errorBuf : "Could not reach the host.");
                s_selectedIndex = 0;
                TransitionTo(MenuState::JoinEntry, "join spectator probe failed");
                return;
            }
            if (spectator.error[0]) {
                SetError("%s", spectator.error);
                SetStatus("%s", spectator.error);
            }
            s_selectedIndex = 0;
            TransitionTo(MenuState::SpectateEntry, "spectator failed");
            break;

        case Net::SpectatorClientState::Idle:
            s_idleSpectatorPromptDeferred = false;
            ClearActionPrompt("spectator_idle");
            if (s_state == MenuState::SpectatorConnecting ||
                s_state == MenuState::SpectatorConnected) {
                if (s_joinSpectatorProbeActive) {
                    char errorBuf[128] = {};
                    ResolveJoinSpectatorProbeFailureMessage(spectator, errorBuf, sizeof(errorBuf));
                    SPECTATE_MENU_LOG(LOG_WARNING, "SPROBE",
                        "[SPROBE] failure endpoint=%s reason=%s",
                        activeEndpoint[0] ? activeEndpoint : "(unset)",
                        errorBuf[0] ? errorBuf : "idle_without_result");
                    ClearJoinSpectatorProbe("idle_without_result");
                    SetError("%s", errorBuf[0] ? errorBuf : "Could not reach the host.");
                    SetStatus("%s", errorBuf[0] ? errorBuf : "Could not reach the host.");
                    s_selectedIndex = 0;
                    TransitionTo(MenuState::JoinEntry, "join spectator probe failed");
                    return;
                }
                if (spectator.error[0]) {
                    SetError("%s", spectator.error);
                }
                if (spectator.status[0]) {
                    SetStatus("%s", spectator.status);
                }
                s_selectedIndex = 0;
                TransitionTo(MenuState::SpectateEntry, "spectator idle");
            }
            break;
    }
}

static void HandleNavigationInput() {
    // Confirm / cancel SFX. Edge-triggered and latched once per frame so every
    // navigable substate gets the vanilla net-menu beep without per-site wiring.
    if (ConfirmPressed())   PlayMenuConfirmSfx();
    else if (BackPressed()) PlayMenuCancelSfx();

    // C key: copy your address to clipboard (in states where it's relevant)
    {
        bool cDown = IsCopyAddressKeyDown();
        if (cDown && !s_prevCopyAddressKeyDown) {
            const bool hostContext =
                s_state == MenuState::HostEntry ||
                ((s_state == MenuState::Connecting ||
                  s_state == MenuState::Handshake ||
                  s_state == MenuState::ConnectedSession) &&
                 s_connectionEntry == ConnectionEntry::Host);
            if (hostContext) {
                MenuUtils::UpdateYourAddress(s_listenPort);
                if (MenuUtils::CopyToClipboard(MenuUtils::GetYourAddress())) {
                    MenuUtils::FlashClipboardMessage("Copied!");
                }
            }
        }
        s_prevCopyAddressKeyDown = cDown;
    }

    if (IsActionPromptOpen()) {
        if (InputSystem_JustPressed(0, INPUT_UP) || InputSystem_JustPressed(0, INPUT_LEFT)) {
            MoveActionPromptSelection(-1);
        }
        if (InputSystem_JustPressed(0, INPUT_DOWN) || InputSystem_JustPressed(0, INPUT_RIGHT)) {
            MoveActionPromptSelection(1);
        }
        if (ConfirmPressed()) {
            Net::SpectatorClientSnapshot spectator{};
            Net::SpectatorClient_GetSnapshot(&spectator);
            const uint32_t selected = s_actionPromptSelectedIndex;
            if (s_actionPromptKind == ActionPromptKind::JoinAsSpectator) {
                if (selected == 0) {
                    ClearActionPrompt("accept_join_as_spectator");
                    ClearJoinSpectatorProbe("accept_join_as_spectator");
                    s_activeBranch = RootBranch::Spectate;
                    s_selectedIndex = 0;
                    SetStatus("Now watching the live match.");
                    TransitionTo(MenuState::SpectatorConnected, "accept join as spectator");
                } else {
                    ClearActionPrompt("decline_join_as_spectator");
                    ClearJoinSpectatorProbe("decline_join_as_spectator");
                    Net::SpectatorClient_Disconnect("declined spectator redirect");
                    s_activeBranch = RootBranch::DirectPlay;
                    s_selectedIndex = 0;
                    SetStatus("Returned to Join a Match.");
                    TransitionTo(MenuState::JoinEntry, "decline join as spectator");
                }
            } else if (s_actionPromptKind == ActionPromptKind::JoinInsteadOfWaiting) {
                if (selected == 0) {
                    char joinEndpoint[96] = {};
                    if (s_actionPromptJoinEndpoint[0]) {
                        CopyText(joinEndpoint, sizeof(joinEndpoint), s_actionPromptJoinEndpoint);
                    } else {
                        BuildSessionEndpointFromSpectator(spectator, joinEndpoint, sizeof(joinEndpoint));
                    }
                    if (!joinEndpoint[0]) {
                        SetStatus("This watch server did not advertise a room address to join.");
                        return;
                    }
                    if (StartJoinSessionToEndpoint(
                            joinEndpoint,
                            "Connecting to host...",
                            "join from idle spectator prompt")) {
                        ClearActionPrompt("join_from_idle_spectator");
                        s_idleSpectatorPromptDeferred = false;
                        Net::SpectatorClient_Disconnect("join from spectator prompt");
                    }
                } else if (selected == 1) {
                    ClearActionPrompt("wait_for_spectating");
                    s_idleSpectatorPromptDeferred = true;
                    s_activeBranch = RootBranch::Spectate;
                    s_selectedIndex = 0;
                    SetStatus("Waiting for the host to start a match.");
                    TransitionTo(MenuState::SpectatorConnected, "wait for active spectator match");
                } else {
                    ClearActionPrompt("cancel_idle_spectator_prompt");
                    s_idleSpectatorPromptDeferred = false;
                    CancelSpectatorConnectionAndReturn(
                        "spectator idle prompt canceled",
                        "Returned to Watch a Match.",
                        "cancel idle spectator prompt");
                }
            }
            return;
        }
        if (BackPressed()) {
            s_actionPromptSelectedIndex = GetActionPromptCancelIndex();
            Net::SpectatorClientSnapshot spectator{};
            Net::SpectatorClient_GetSnapshot(&spectator);
            (void)spectator;
            if (s_actionPromptKind == ActionPromptKind::JoinAsSpectator) {
                ClearActionPrompt("back_decline_join_as_spectator");
                ClearJoinSpectatorProbe("back_decline_join_as_spectator");
                Net::SpectatorClient_Disconnect("declined spectator redirect");
                s_activeBranch = RootBranch::DirectPlay;
                s_selectedIndex = 0;
                SetStatus("Returned to Join a Match.");
                TransitionTo(MenuState::JoinEntry, "back decline join as spectator");
            } else if (s_actionPromptKind == ActionPromptKind::JoinInsteadOfWaiting) {
                ClearActionPrompt("back_cancel_idle_spectator_prompt");
                s_idleSpectatorPromptDeferred = false;
                CancelSpectatorConnectionAndReturn(
                    "spectator idle prompt canceled",
                    "Returned to Watch a Match.",
                    "back cancel idle spectator prompt");
            }
            return;
        }
        return;
    }

    // Repeat-aware Up/Down
    if (InputSystem_JustPressed(0, INPUT_UP))   MoveSelection(-1);
    if (InputSystem_JustPressed(0, INPUT_DOWN))  MoveSelection(1);

    // Left/Right for inline settings adjustments
    if (s_state == MenuState::SettingsEntry) {
        const bool left  = InputSystem_JustPressed(0, INPUT_LEFT);
        const bool right = InputSystem_JustPressed(0, INPUT_RIGHT);
        bool changed = false;
        bool natChanged = false;
        bool spectatorChanged = false;
        bool paletteChanged = false;
        const int gid = SettingGlobalId();

        if (left || right) {
            if (gid == 1) {
                if (left && s_preferredDelay > 0) {
                    s_preferredDelay--;
                    changed = true;
                } else if (right && s_preferredDelay < 15) {
                    s_preferredDelay++;
                    changed = true;
                }
                if (changed) {
                    Net::DelayPolicy_SetConfiguredDelay(s_preferredDelay);
                    SetStatus("Input delay: %d frame%s", s_preferredDelay, s_preferredDelay == 1 ? "" : "s");
                }
            } else if (gid == 2) {
                if (left && s_rollbackBudget > Net::ROLLBACK_BUDGET_MIN) {
                    s_rollbackBudget--;
                    changed = true;
                } else if (right && s_rollbackBudget < Net::ROLLBACK_BUDGET_MAX) {
                    s_rollbackBudget++;
                    changed = true;
                }
                if (changed) {
                    Net::DelayPolicy_SetRollbackBudget(s_rollbackBudget);
                    SetStatus("Max rollback: %d frame%s", s_rollbackBudget, s_rollbackBudget == 1 ? "" : "s");
                }
            } else if (gid == 3) {
                if (left && s_rollbackTolerance > Net::ROLLBACK_TOLERANCE_MIN) {
                    s_rollbackTolerance--;
                    changed = true;
                } else if (right && s_rollbackTolerance < Net::ROLLBACK_TOLERANCE_MAX) {
                    s_rollbackTolerance++;
                    changed = true;
                }
                if (changed) {
                    Net::DelayPolicy_SetRollbackToleranceK(s_rollbackTolerance);
                    SetStatus("Stability bias: %d", s_rollbackTolerance);
                }
            } else if (gid == 4) {
                int mode = (int)s_connectPreference;
                if (left) {
                    mode--;
                    if (mode < (int)Net::ConnectPreference::AutoDirectThenRelay) {
                        mode = (int)Net::ConnectPreference::DirectOnly;
                    }
                    changed = true;
                } else if (right) {
                    mode++;
                    if (mode > (int)Net::ConnectPreference::DirectOnly) {
                        mode = (int)Net::ConnectPreference::AutoDirectThenRelay;
                    }
                    changed = true;
                }
                if (changed) {
                    s_connectPreference = (Net::ConnectPreference)mode;
                    SetStatus("Connection route: %s", FriendlyConnectPreferenceLabel(s_connectPreference));
                }
            } else if (gid == 5) {
                s_upnpEnabled = !s_upnpEnabled;
                changed = true;
                natChanged = true;
                SetStatus("UPnP: %s", EnabledStateLabel(s_upnpEnabled));
            } else if (gid == 6) {
                s_stunEnabled = !s_stunEnabled;
                changed = true;
                natChanged = true;
                SetStatus("STUN: %s", EnabledStateLabel(s_stunEnabled));
            } else if (gid == 7) {
                s_holePunchEnabled = !s_holePunchEnabled;
                changed = true;
                natChanged = true;
                SetStatus("UDP hole punch: %s", EnabledStateLabel(s_holePunchEnabled));
            } else if (gid == 8) {
                s_allowIPv6Endpoint = !s_allowIPv6Endpoint;
                changed = true;
                natChanged = true;
                SetStatus("IPv6 addresses: %s", EnabledStateLabel(s_allowIPv6Endpoint));
            } else if (gid == 11) {
                s_spectatorsEnabled = !s_spectatorsEnabled;
                changed = true;
                spectatorChanged = true;
                SetStatus("Allow watchers: %s", EnabledStateLabel(s_spectatorsEnabled));
            } else if (gid == 13) {
                s_paletteSyncEnabled = !s_paletteSyncEnabled;
                changed = true;
                paletteChanged = true;
                SetStatus("Palette sync: %s", EnabledStateLabel(s_paletteSyncEnabled));
            } else if (gid == 14) {
                s_remotePalettePreviewEnabled = !s_remotePalettePreviewEnabled;
                changed = true;
                paletteChanged = true;
                SetStatus("Remote palette preview: %s", EnabledStateLabel(s_remotePalettePreviewEnabled));
            } else if (gid == 15) {
                s_debugLoggingEnabled = GetVerboseLogging();
                s_debugLoggingEnabled = !s_debugLoggingEnabled;
                changed = true;
                SetStatus("Debug logging: %s", EnabledStateLabel(s_debugLoggingEnabled));
            } else if (gid == 16) {
                s_gameplayDelayMode =
                    s_gameplayDelayMode == Net::GameplayDelayMode::AsymmetricExpert
                        ? Net::GameplayDelayMode::SharedSafe
                        : Net::GameplayDelayMode::AsymmetricExpert;
                Net::DelayPolicy_SetGameplayDelayMode(s_gameplayDelayMode);
                changed = true;
                SetStatus("Delay mode: %s",
                    s_gameplayDelayMode == Net::GameplayDelayMode::SharedSafe
                        ? "Shared max"
                        : "Per-player");
            } else if (gid == 17) {
                NetplayHudStyle::CycleTrailPreset(left ? -1 : 1);
                changed = true;
                SetStatus("Bar color: %s", NetplayHudStyle::GetTrailPresetLabel());
            } else if (gid == 18) {
                NetplayHudStyle::CycleTextPreset(left ? -1 : 1);
                changed = true;
                SetStatus("Text color: %s", NetplayHudStyle::GetTextPresetLabel());
            } else if (gid == 19) {
                NetplayHudStyle::CycleScorePreset(left ? -1 : 1);
                changed = true;
                SetStatus("Score color: %s", NetplayHudStyle::GetScorePresetLabel());
            } else if (gid == 20) {
                NetplayHudStyle::AdjustTrailLength(left ? -16 : 16);
                changed = true;
                NetplayHudStyle::Settings hudStyle{};
                NetplayHudStyle::GetLocal(&hudStyle);
                SetStatus("Bar extend: %u px", hudStyle.trail_length_px);
            } else if (gid == 21) {
                NetplayHudStyle::CycleVerticalPosition(left ? -1 : 1);
                changed = true;
                SetStatus("Name position: %s", NetplayHudStyle::GetVerticalPositionLabel());
            } else if (gid == 22) {
                NetplayHudStyle::CycleFontSize(left ? -1 : 1);
                changed = true;
                SetStatus("Font size: %s", NetplayHudStyle::GetFontSizeLabel());
            } else if (gid == 23) {
                NetplayHudStyle::CycleRenderMode(left ? -1 : 1);
                changed = true;
                SetStatus("Render mode: %s", NetplayHudStyle::GetRenderModeLabel());
            }

            if (changed) {
                PlayMenuCursorSfx();
                if (natChanged) {
                    ApplyNatSettingsToService("settings navigation");
                    ApplySpectatorSettingsToRuntime("settings navigation");
                }
                if (spectatorChanged && !natChanged) {
                    ApplySpectatorSettingsToRuntime("settings navigation");
                }
                if (paletteChanged) {
                    ApplyPaletteSettingsToRuntime("settings navigation");
                }
                if (gid == 15) {
                    ApplyDebugLoggingSetting("settings navigation");
                }
                SaveSettings();
            }
        }
    }

    // Left/Right for ConnectedSession: Rollback Frames (index 0) and Input Delay (index 1)
    if (s_state == MenuState::ConnectedSession) {
        if (s_selectedIndex == 0) {
            // Max rollback
            if (InputSystem_JustPressed(0, INPUT_LEFT)) {
                int cur = s_rollbackBudget;
                if (cur > Net::ROLLBACK_BUDGET_MIN) {
                    s_rollbackBudget = cur - 1;
                    Net::DelayPolicy_SetRollbackBudget(s_rollbackBudget);
                    SetStatus("Max rollback: %d frame%s | suggested %d",
                        s_rollbackBudget,
                        s_rollbackBudget == 1 ? "" : "s",
                        Net::DelayPolicy_ComputeRecommendedMaxRollback());
                    SaveSettings();
                }
            }
            if (InputSystem_JustPressed(0, INPUT_RIGHT)) {
                int cur = s_rollbackBudget;
                if (cur < Net::ROLLBACK_BUDGET_MAX) {
                    s_rollbackBudget = cur + 1;
                    Net::DelayPolicy_SetRollbackBudget(s_rollbackBudget);
                    SetStatus("Max rollback: %d frame%s | suggested %d",
                        s_rollbackBudget,
                        s_rollbackBudget == 1 ? "" : "s",
                        Net::DelayPolicy_ComputeRecommendedMaxRollback());
                    SaveSettings();
                }
            }
        } else if (s_selectedIndex == 1) {
            // My local gameplay delay
            if (InputSystem_JustPressed(0, INPUT_LEFT)) {
                if (s_preferredDelay > Net::DELAY_MIN) {
                    s_preferredDelay--;
                    Net::DelayPolicy_SetConfiguredDelay(s_preferredDelay);
                    SetStatus("Input delay: %d frame%s", s_preferredDelay, s_preferredDelay == 1 ? "" : "s");
                    SaveSettings();
                }
            }
            if (InputSystem_JustPressed(0, INPUT_RIGHT)) {
                if (s_preferredDelay < Net::DELAY_MAX) {
                    s_preferredDelay++;
                    Net::DelayPolicy_SetConfiguredDelay(s_preferredDelay);
                    SetStatus("Input delay: %d frame%s", s_preferredDelay, s_preferredDelay == 1 ? "" : "s");
                    SaveSettings();
                }
            }
        }
    }

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
                SetStatus("Set up your room.");
                MenuUtils::BeginPublicIPFetch();
                TransitionTo(MenuState::HostEntry, "open host config");
            } else if (s_selectedIndex == 1) {
                // Join covers spectating: a busy room fails the gameplay
                // connect and the spectator probe offers to watch instead.
                s_activeBranch = RootBranch::DirectPlay;
                s_selectedIndex = 0;
                SetStatus("Enter the host address.");
                TransitionTo(MenuState::JoinEntry, "open join config");
            } else if (s_selectedIndex == 2) {
                s_activeBranch = RootBranch::Settings;
                s_selectedIndex = 0;
                SetStatus("Connection settings opened.");
                TransitionTo(MenuState::SettingsCategoryMenu, "open settings categories");
            } else {
                BeginClose("close from root");
            }
            break;

        case MenuState::DirectConnectEntry:
            if (s_selectedIndex == 0) {
                // Host
                s_selectedIndex = 0;
                SetStatus("Set up your room.");
                TransitionTo(MenuState::HostEntry, "open host config");
            } else if (s_selectedIndex == 1) {
                // Join
                s_selectedIndex = 0;
                SetStatus("Enter the host address.");
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
                ClearJoinSpectatorProbe("manual_host_start");
                Net::SessionConfig cfg{};
                Net::SessionConfig_SetDefaults(&cfg);
                cfg.listen_port = s_listenPort;
                strncpy_s(cfg.nickname, sizeof(cfg.nickname), s_localNickname, _TRUNCATE);
                cfg.connect_preference = s_connectPreference;
                Net::NatRuntimeConfig natCfg{};
                if (!BuildNatRuntimeConfig(&natCfg)) {
                    SetStatus("Connection settings are invalid.");
                    break;
                }
                cfg.nat.enable_upnp = natCfg.enable_upnp;
                cfg.nat.enable_stun = natCfg.enable_stun;
                cfg.nat.enable_hole_punch = natCfg.enable_hole_punch;
                cfg.nat.enable_turn = natCfg.enable_turn;
                cfg.nat.enable_pcp_fallback = natCfg.enable_pcp_fallback;
                cfg.nat.allow_ipv6_endpoint = natCfg.allow_ipv6_endpoint;
                cfg.nat.prefer_portforwarded_direct = natCfg.prefer_portforwarded_direct;
                strncpy_s(cfg.nat.stun_host, sizeof(cfg.nat.stun_host), natCfg.stun_host, _TRUNCATE);
                cfg.nat.stun_port = natCfg.stun_port;
                strncpy_s(cfg.nat.turn_host, sizeof(cfg.nat.turn_host), natCfg.turn_host, _TRUNCATE);
                cfg.nat.turn_port = natCfg.turn_port;
                strncpy_s(cfg.nat.turn_username, sizeof(cfg.nat.turn_username), natCfg.turn_username, _TRUNCATE);
                strncpy_s(cfg.nat.turn_password, sizeof(cfg.nat.turn_password), natCfg.turn_password, _TRUNCATE);
                cfg.nat.gather_timeout_ms = natCfg.gather_timeout_ms;
                cfg.nat.connect_timeout_ms = natCfg.connect_timeout_ms;
                cfg.nat.mapping_timeout_ms = natCfg.mapping_timeout_ms;
                cfg.nat.traversal_log_verbosity = natCfg.traversal_log_verbosity;
                if (s_relayEndpoint[0]) {
                    char relayHost[96] = {};
                    uint16_t relayPort = 0;
                    if (ParseEndpoint(s_relayEndpoint, relayHost, sizeof(relayHost), &relayPort, true)) {
                        strncpy_s(cfg.nat.relay_host, sizeof(cfg.nat.relay_host), relayHost, _TRUNCATE);
                        cfg.nat.relay_port = relayPort;
                    }
                }
                ApplyDelaySettingsToPolicy("manual host start");
                ApplyNatSettingsToService("manual host start");
                MenuUtils::BeginPublicIPFetch();
                if (Net::Session_StartHost(&cfg)) {
                    s_connectionEntry = ConnectionEntry::Host;
                    s_activeBranch = RootBranch::DirectPlay;
                    SetStatus("Room is open. Waiting for another player...");
                    TransitionTo(MenuState::Connecting, "host started");
                } else {
                    SetStatus("Couldn't open the room.");
                }
            } else if (s_selectedIndex == 1) {
                // Edit port
                char portBuf[8];
                _snprintf_s(portBuf, sizeof(portBuf), _TRUNCATE, "%u", s_listenPort);
                BeginTextEdit(TextEditField::ListenPort, portBuf, "Enter the room port (1-65535).");
            } else {
                s_selectedIndex = 0;
                TransitionTo(MenuState::MenuRoot, "back from host");
            }
            break;

        case MenuState::JoinEntry:
            if (s_selectedIndex == 0) {
                // Join
                ClearJoinSpectatorProbe("manual_join_start");
                StartJoinSessionToEndpoint(
                    s_remoteEndpoint,
                    "Connecting to host...",
                    "join started");
            } else if (s_selectedIndex == 1) {
                // Edit endpoint
                BeginTextEdit(TextEditField::RemoteEndpoint, s_remoteEndpoint, "Enter the host address as host:port or [ipv6]:port.");
            } else {
                s_selectedIndex = 0;
                TransitionTo(MenuState::MenuRoot, "back from join");
            }
            break;

        case MenuState::SpectateEntry:
            if (s_selectedIndex == 0) {
                if (!s_spectatorEndpoint[0]) {
                    SetStatus("Enter a watch address first.");
                    break;
                }
                ClearJoinSpectatorProbe("manual_spectate_start");
                s_idleSpectatorPromptDeferred = false;
                ClearError();
                if (Net::SpectatorClient_StartConnect(s_spectatorEndpoint)) {
                    SetStatus("Connecting to watch server...");
                    SPECTATE_MENU_LOG(LOG_INFO, "SMENU",
                        "[MENU] manual_spectate_connect endpoint=%s",
                        s_spectatorEndpoint);
                    TransitionTo(MenuState::SpectatorConnecting, "spectator connect");
                } else {
                    SetStatus("Couldn't start the watch connection.");
                }
            } else if (s_selectedIndex == 1) {
                Net::SpectatorDiscoverySnapshot discovery{};
                Net::SpectatorClient_GetDiscoverySnapshot(&discovery);
                if (discovery.active) {
                    SetStatus("%s", discovery.status[0] ? discovery.status : "Scanning the local network for watch hosts...");
                } else if (discovery.result_count > 1) {
                    s_selectedLanSpectatorIndex =
                        (s_selectedLanSpectatorIndex + 1) % discovery.result_count;
                    ApplyDiscoveredSpectatorEndpoint(
                        discovery.results[s_selectedLanSpectatorIndex],
                        s_selectedLanSpectatorIndex,
                        discovery.result_count,
                        "manual cycle");
                } else {
                    s_selectedLanSpectatorIndex = 0;
                    s_lastLanSpectatorResultCount = 0;
                    s_lastLanSpectatorEndpoint[0] = '\0';
                    if (Net::SpectatorClient_BeginLanDiscovery()) {
                        SetStatus("Scanning the local network for watch hosts...");
                    } else {
                        Net::SpectatorDiscoverySnapshot retry{};
                        Net::SpectatorClient_GetDiscoverySnapshot(&retry);
                        SetStatus("%s", retry.status[0] ? retry.status : "Couldn't start the local network scan.");
                    }
                }
            } else if (s_selectedIndex == 2) {
                BeginTextEdit(TextEditField::SpectatorEndpoint, s_spectatorEndpoint, "Enter the watch address as host:port or [ipv6]:port.");
            } else {
                s_selectedIndex = 0;
                TransitionTo(MenuState::MenuRoot, "back from spectate");
            }
            break;

        case MenuState::SpectatorConnecting:
            CancelSpectatorConnectionAndReturn(
                "spectator connect canceled",
                "Watch connection cancelled.",
                "cancel spectator connect");
            break;

        case MenuState::SpectatorConnected:
            CancelSpectatorConnectionAndReturn(
                "spectator disconnected by user",
                "Stopped watching the match.",
                "spectator disconnect");
            break;

        case MenuState::SettingsEntry: {
            const int gid = SettingGlobalId();
            if (gid == 0) {
                BeginTextEdit(TextEditField::Nickname, s_localNickname, "Enter your display name.");
            } else if (gid == 9) {
                BeginTextEdit(TextEditField::RelayEndpoint, s_relayEndpoint, "Enter the punch relay as host:port or [ipv6]:port.");
            } else if (gid == 10) {
                BeginTextEdit(TextEditField::StunEndpoint, s_stunEndpoint, "Enter the STUN server as host:port or [ipv6]:port.");
            } else if (gid == 12) {
                char portBuf[8];
                _snprintf_s(portBuf, sizeof(portBuf), _TRUNCATE, "%u", s_spectatorListenPort);
                BeginTextEdit(TextEditField::SpectatorPort, portBuf, "Enter the watch port (1-65535).");
            } else if (gid == -1) {
                // Back to category menu
                s_selectedIndex = 0;
                TransitionTo(MenuState::SettingsCategoryMenu, "back from settings page");
            }
            // Most settings are adjusted via left/right.
            break;
        }

        case MenuState::SettingsCategoryMenu:
            if (s_selectedIndex == 0) {
                s_settingsCategory = SettingsCategory::Identity;
                s_selectedIndex = 0;
                TransitionTo(MenuState::SettingsEntry, "open player settings");
            } else if (s_selectedIndex == 1) {
                s_settingsCategory = SettingsCategory::Appearance;
                s_selectedIndex = 0;
                TransitionTo(MenuState::SettingsEntry, "open appearance settings");
            } else if (s_selectedIndex == 2) {
                s_settingsCategory = SettingsCategory::Endpoint;
                s_selectedIndex = 0;
                TransitionTo(MenuState::SettingsEntry, "open network settings");
            } else if (s_selectedIndex == 3) {
                s_settingsCategory = SettingsCategory::SessionMatch;
                s_selectedIndex = 0;
                TransitionTo(MenuState::SettingsEntry, "open watch settings");
            } else if (s_selectedIndex == 4) {
                s_settingsCategory = SettingsCategory::Diagnostics;
                s_selectedIndex = 0;
                TransitionTo(MenuState::SettingsEntry, "open diagnostics settings");
            } else {
                // Back to main menu
                s_selectedIndex = 0;
                TransitionTo(MenuState::MenuRoot, "back from settings categories");
            }
            break;

        case MenuState::ConnectedSession:
            if (s_selectedIndex == 2) {
                // Accept Match — signal ready to peer
                Net::Session_SignalReady();
            } else if (s_selectedIndex == 3) {
                // Decline
                Net::Session_Cancel();
                OpenDisconnectError("Match declined.");
            }
            // Items 0 (Rollback) and 1 (Delay): adjusted via left/right, not confirm
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
            Net::Session_Cancel();
            Net::SpectatorClient_Disconnect("cancel connection flow");
            ClearJoinSpectatorProbe("cancel connection flow");
            {
                const MenuState target =
                    s_connectionEntry == ConnectionEntry::Join ? MenuState::JoinEntry :
                    s_connectionEntry == ConnectionEntry::Host ? MenuState::HostEntry :
                    MenuState::DirectConnectEntry;
                s_connectionEntry = ConnectionEntry::None;
                s_activeBranch = RootBranch::DirectPlay;
                s_selectedIndex = 0;
                SetStatus(target == MenuState::JoinEntry ? "Returned to Join a Match."
                        : target == MenuState::HostEntry ? "Returned to Host a Room."
                        : "Returned to Play Online.");
                TransitionTo(target, "cancel connection");
            }
            break;

        case MenuState::PostMatch:
            if (s_selectedIndex == 0) {
                // Rematch
                Net::TransitionBarrier_Propose(Net::NetTransitionKind::PostMatchDecision,
                                               (uint8_t)Net::PostMatchIntentWire::Rematch, 0);
                Net::MatchLifecycle_OnRematch();
                Rollback::OnlineWiring_OnRematch();
                LaunchNetplayCharSel();
            } else if (s_selectedIndex == 1) {
                // Return to menu
                Net::TransitionBarrier_Propose(Net::NetTransitionKind::PostMatchDecision,
                                               (uint8_t)Net::PostMatchIntentWire::ReturnToSession, 0);
                Net::MatchLifecycle_OnReturnToSession();
                Rollback::OnlineWiring_OnReturnToSession();
                s_selectedIndex = 0;
                TransitionTo(MenuState::ConnectedSession, "post-match return");
            } else {
                // Disconnect
                Net::TransitionBarrier_Propose(Net::NetTransitionKind::PostMatchDecision,
                                               (uint8_t)Net::PostMatchIntentWire::Disconnect, 0);
                Net::MatchLifecycle_OnDisconnect("Disconnected after match.");
                Net::Session_Cancel();
                OpenDisconnectError("Disconnected after match.");
            }
            break;

        case MenuState::DisconnectError:
            if (s_selectedIndex == 0) {
                ClearError();
                s_selectedIndex = 0;
                const MenuState target = s_disconnectReturnState;
                if (target == MenuState::JoinEntry ||
                    target == MenuState::HostEntry ||
                    target == MenuState::DirectConnectEntry) {
                    s_activeBranch = RootBranch::DirectPlay;
                } else if (target == MenuState::SpectateEntry) {
                    s_activeBranch = RootBranch::Spectate;
                }
                s_connectionEntry = ConnectionEntry::None;
                TransitionTo(target, "ack disconnect");
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
            TransitionTo(MenuState::MenuRoot, "back from host");
            break;
        case MenuState::JoinEntry:
            s_selectedIndex = 0;
            TransitionTo(MenuState::MenuRoot, "back from join");
            break;
        case MenuState::SpectateEntry:
            s_selectedIndex = 0;
            TransitionTo(MenuState::MenuRoot, "back from spectate");
            break;
        case MenuState::SpectatorConnecting:
            CancelSpectatorConnectionAndReturn(
                "spectator connect canceled",
                "Watch connection cancelled.",
                "cancel spectator connect");
            break;
        case MenuState::SpectatorConnected:
            CancelSpectatorConnectionAndReturn(
                "spectator disconnected by user",
                "Stopped watching the match.",
                "back from spectator stream");
            break;
        case MenuState::SettingsCategoryMenu:
            s_selectedIndex = 0;
            TransitionTo(MenuState::MenuRoot, "back from settings categories");
            break;
        case MenuState::SettingsEntry:
            s_selectedIndex = 0;
            TransitionTo(MenuState::SettingsCategoryMenu, "back from settings page");
            break;
        case MenuState::Connecting:
        case MenuState::Handshake:
            Net::Session_Cancel();
            Net::SpectatorClient_Disconnect("cancel connection flow");
            ClearJoinSpectatorProbe("cancel connection flow");
            {
                const MenuState target =
                    s_connectionEntry == ConnectionEntry::Join ? MenuState::JoinEntry :
                    s_connectionEntry == ConnectionEntry::Host ? MenuState::HostEntry :
                    MenuState::DirectConnectEntry;
                s_connectionEntry = ConnectionEntry::None;
                s_activeBranch = RootBranch::DirectPlay;
                s_selectedIndex = 0;
                SetStatus(target == MenuState::JoinEntry ? "Returned to Join a Match."
                        : target == MenuState::HostEntry ? "Returned to Host a Room."
                        : "Returned to Play Online.");
                TransitionTo(target, "cancel connection");
            }
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

void CacheAutoConnectFile() {
    CacheAutoConnectFileImpl();
}

void Init() {
    if (s_initialized) return;
    s_state = MenuState::Inactive;
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    ClearActionPrompt("init");
    s_idleSpectatorPromptDeferred = false;
    LoadSettings();
    ApplyDebugLoggingSetting("menu init");
    ApplyDelaySettingsToPolicy("menu init");
    ApplyNatSettingsToService("menu init");
    ApplySpectatorSettingsToRuntime("menu init");
    ApplyPaletteSettingsToRuntime("menu init");
    ApplyContinueScreenSettingToRuntime("menu init");
    LoadAutoConnectConfig();
    s_autoRematchCleanupApplied = false;
    s_autoRematchLastAttemptAt = 0;
    s_selectedLanSpectatorIndex = 0;
    s_lastLanSpectatorResultCount = 0;
    s_lastLanSpectatorDiscoveryActive = false;
    s_lastLanSpectatorEndpoint[0] = '\0';
    s_initialized = true;
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Initialized");
}

void Shutdown() {
    if (!s_initialized) return;
    ClearAutoConnectOverride();
    AutoConnectHarness_Shutdown();
    ClearActionPrompt("shutdown");
    s_autoConnectState = AutoConnectState::Disabled;
    s_autoConnectCompletedMatches = 0;
    s_idleSpectatorPromptDeferred = false;
    s_autoRematchCleanupApplied = false;
    s_autoRematchLastAttemptAt = 0;
    s_selectedLanSpectatorIndex = 0;
    s_lastLanSpectatorResultCount = 0;
    s_lastLanSpectatorDiscoveryActive = false;
    s_lastLanSpectatorEndpoint[0] = '\0';
    memset(&s_autoConnect, 0, sizeof(s_autoConnect));
    FinishClose();
    MenuUtils::Cleanup();
    s_initialized = false;
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Shutdown");
}

void FrameUpdate() {
    uint32_t mode = GetGameMode();

    // ALWAYS pump the session — even when the menu is hidden (CharSel/Match).
    // This drives ENet polling, keepalive, and timeout detection.
    const double tf0 = NowMs();
    SyncSessionState();
    const double tf1 = NowMs();

    // Re-read mode after session sync (disconnect may have forced mode change)
    mode = GetGameMode();

    // Drop cached net assets if the game has left MODE_MENU (engine freed them).
    // Runs even while the menu is hidden so a post-match reopen reloads cleanly.
    InvalidateNetMenuPresentationOnModeLeave();

    HandleAutoConnect();
    const double tf2 = NowMs();
    TryAutoRestartPregameFromPostMatchCharSel();
    SyncSpectatorDiscoveryState();
    const double tf3 = NowMs();
    SyncSpectatorClientState();
    const double tf4 = NowMs();

    // Surface any per-frame stall (network/spectator pumps) that could be felt
    // as menu lag. Only logs when something actually blocks (>20ms).
    if ((tf4 - tf0) > 20.0) {
        LOG_NETPLAY(LOG_WARNING, "[NetMenu][TIMING] FrameUpdate pump stall=%.1fms (session=%.1f autoconnect=%.1f spectatorDisc=%.1f spectatorClient=%.1f)",
            tf4 - tf0, tf1 - tf0, tf2 - tf1, tf3 - tf2, tf4 - tf3);
    }

    if (!MenuVisible()) return;

    // If we're no longer in MODE_MENU, close the menu
    if (mode != MODE_MENU) {
        LOG_NETPLAY(LOG_WARNING, "[NetMenu] Closing menu because mode changed to %u", mode);
        FinishClose();
        return;
    }

    // Wait for menu background assets to load before rendering.
    // EnterCustomMenuContext() sets pending restore when transitioning from
    // a non-menu mode — the game needs sub=0 to load sprites before sub=3
    // is safe to draw over. Without this guard the menu renders on black.
    if (ModeOwnership::IsPendingMenuRestore()) {
        return;
    }

    // Now that the menu is drawable, load the vanilla net.bin background +
    // wave\net.bin SFX and start BGM 74. Idempotent per open.
    EnsureNetMenuPresentation();
    ResetMenuSfxFrameGuards();

    // Fade transitions
    if (s_phase == MenuPhase::Opening) {
        if (++s_fadeFrames >= kFadeFrames) {
            s_fadeFrames = kFadeFrames;
            s_phase = MenuPhase::Active;
        }
    } else if (s_phase == MenuPhase::Closing) {
        if (--s_fadeFrames <= 0) {
            // Real close back to the title: restore the main-menu BGM before
            // handing the mode back to vanilla.
            RestoreMainMenuBgm();
            ModeOwnership::RestoreMainMenuContext();
            // Start a full-screen fade-in over the vanilla main menu so the
            // return doesn't hard-cut (the net menu has just faded to black).
            s_mainMenuReturnFade = kFadeFrames;
            FinishClose();
            return;
        }
    }

    if (!IsProcessForegroundWindow()) {
        s_waitForNeutral = true;
        s_prevCopyAddressKeyDown = false;
        ResetTextEditKeyState();
        InputSystem_ResetRepeatState(0);
        return;
    }

    // Wait for all inputs to be released before processing
    if (s_waitForNeutral) {
        uint16_t held = InputSystem_GetInput(0);
        if ((held & (INPUT_ANY_DIR | INPUT_A | INPUT_B | INPUT_START | INPUT_SELECT)) == 0 &&
            !IsCopyAddressKeyDown()) {
            s_waitForNeutral = false;
            s_prevCopyAddressKeyDown = false;
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
}

void HandleNetworkSelected() {
    OpenMenu();
}

// Set while a DELIBERATE quit is tearing the session down. The pregame
// watchdog (match_setup.cpp:2202) sees the session object vanish and reports
// "Session lost during pre-game sync" -- accurate, but it was us, and an error
// box for an intentional action is simply wrong. Window rather than a bool
// because several subsystems notice the teardown at their own pace.
static DWORD s_gracefulQuitUntilMs = 0;

void HandleDisconnection(const char* reason) {
    if (s_gracefulQuitUntilMs != 0 && GetTickCount() < s_gracefulQuitUntilMs) {
        LOG_NETPLAY(LOG_INFO,
            "[NetMenu] Disconnect notice suppressed (deliberate quit in progress): %s",
            reason ? reason : "?");
        return;
    }
    OpenDisconnectError(reason);
}

void HandleGracefulSessionQuit(const char* reason) {
    const char* msg = reason ? reason : "Left the session.";
    LOG_NETPLAY(LOG_INFO, "[NetMenu] Graceful session quit: %s", msg);
    s_gracefulQuitUntilMs = GetTickCount() + 5000;

    // Same teardown the error path does. Without aborting pregame sync its
    // watchdog keeps reporting "Session lost during pre-game sync" long after
    // the quit, and the suppression window above just delays the error box
    // instead of preventing it.
    Net::MatchLifecycle_OnDisconnect(msg);
    Rollback::OnlineWiring_OnDisconnect(msg);
    Net::PregameSync_Abort(msg);
    Net::FrontendInputSync_AbortEpoch(msg);
    ClearJoinSpectatorProbe("graceful_quit");
    ModeOwnership::ClearVanillaNetplayFlags();
    Net::Session_Cancel();

    ModeOwnership::EnterCustomMenuContext();
    ClearActionPrompt("graceful_quit");
    s_activeBranch = RootBranch::DirectPlay;
    s_phase = MenuPhase::Active;
    s_fadeFrames = kFadeFrames;
    s_captureInput = true;
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    ClearError();
    SetStatus(msg);
    TransitionTo(MenuState::MenuRoot, "graceful quit");
}

void HandlePostMatchDirectCharsel(const char* reason) {
    LOG_NETPLAY(LOG_INFO,
        "[NetMenu] Match ended (%s) — straight to character select",
        reason ? reason : "?");
    // RecoveryRestart, never Rematch: this is not a lockstep-derived decision.
    // Both peers reach it unconditionally, and match_director deliberately
    // excludes RecoveryRestart from the fail-closed intent comparison, so it
    // cannot trip the F-7 guard the way a Rematch/CharselRestart pair can.
    Net::TransitionBarrier_Propose(Net::NetTransitionKind::PostMatchDecision,
                                   (uint8_t)Net::PostMatchIntentWire::RecoveryRestart, 0);
    Net::MatchLifecycle_OnRematch();
    Rollback::OnlineWiring_OnRematch();
    LaunchNetplayCharSel();
}

void HandlePostMatchReturn() {
    LOG_NETPLAY(LOG_INFO, "[NetMenu] HandlePostMatchReturn — showing post-match menu");

    // Ensure we're in a menu-visible state for the PostMatch options
    ModeOwnership::EnterCustomMenuContext();

    // Open the menu at PostMatch state
    s_phase = MenuPhase::Active;
    s_fadeFrames = kFadeFrames;
    s_captureInput = true;
    ClearActionPrompt("post_match_return");
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    ClearError();
    SetStatus("Match complete. Choose what to do next.");
    TransitionTo(MenuState::PostMatch, "match ended");
}

bool IsMenuActive() {
    return MenuVisible();
}

bool ConsumesGameInput() {
    return s_captureInput;
}

void HideForLaunch(const char* reason) {
    HideMenuForLaunch(reason ? reason : "external launch");
}

void ShowMenuAfterExternalLaunch(const char* reason) {
    if (!MenuVisible()) {
        OpenMenu();
    } else {
        ModeOwnership::EnterCustomMenuContext();
        ClearActionPrompt("external_return_menu");
        ClearJoinSpectatorProbe("external_return_menu");
        s_idleSpectatorPromptDeferred = false;
        s_activeBranch = RootBranch::DirectPlay;
        s_settingsCategory = SettingsCategory::Identity;
        s_phase = MenuPhase::Active;
        s_fadeFrames = kFadeFrames;
        s_captureInput = true;
        s_selectedIndex = 0;
        s_waitForNeutral = true;
        ClearError();
        ClearTextEditState();
        InputSystem_ResetRepeatState(0);
        if (s_state == MenuState::Inactive) {
            TransitionTo(MenuState::MenuRoot, "external return");
        }
    }

    if (reason && reason[0]) {
        SetStatus("%s", reason);
    }
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

    // Address and clipboard flash
    MenuUtils::UpdateYourAddress(s_listenPort);
    CopyText(out->your_address, sizeof(out->your_address), MenuUtils::GetYourAddress());
    if (MenuUtils::HasClipboardFlash()) {
        CopyText(out->clipboard_flash, sizeof(out->clipboard_flash), MenuUtils::GetClipboardFlash());
    }

    // Connection config
    out->listen_port = s_listenPort;
    CopyText(out->remote_endpoint, sizeof(out->remote_endpoint), s_remoteEndpoint);
    out->preferred_delay = s_preferredDelay;
    out->connection_mode = (int)s_connectPreference;
    out->upnp_enabled = s_upnpEnabled;
    out->stun_enabled = s_stunEnabled;
    out->hole_punch_enabled = s_holePunchEnabled;
    out->allow_ipv6_endpoint = s_allowIPv6Endpoint;
    CopyText(out->relay_endpoint, sizeof(out->relay_endpoint), s_relayEndpoint);
    CopyText(out->stun_endpoint, sizeof(out->stun_endpoint), s_stunEndpoint);
    out->spectators_enabled = s_spectatorsEnabled;
    out->spectator_listen_port = s_spectatorListenPort;
    out->palette_sync_enabled = s_paletteSyncEnabled;
    out->remote_palette_preview_enabled = s_remotePalettePreviewEnabled;
    out->debug_logging_enabled = GetVerboseLogging();

    out->join_spectator_probe_active = s_joinSpectatorProbeActive;
    out->connecting_as_host = s_connectionEntry == ConnectionEntry::Host;

    Net::NatSnapshot natSnap{};
    Net::Nat_GetSnapshot(&natSnap);
    char endpointShort[40] = {};
    if (natSnap.stun_endpoint[0]) {
        _snprintf_s(endpointShort, sizeof(endpointShort), _TRUNCATE, "%.30s", natSnap.stun_endpoint);
    } else {
        _snprintf_s(endpointShort, sizeof(endpointShort), _TRUNCATE, "-");
    }
    char natStatus[128];
    _snprintf_s(natStatus, sizeof(natStatus), _TRUNCATE,
        "St=%s U=%s P=%s S=%s map=%u/%u ep=%s",
        Net::NatTraversalStateName(natSnap.traversal_state),
        Net::NatStatusName(natSnap.upnp_status),
        Net::NatStatusName(natSnap.pcp_status),
        Net::StunStatusName(natSnap.stun_status),
        natSnap.mapped_port,
        natSnap.pcp_mapped_port,
        endpointShort);
    CopyText(out->nat_status, sizeof(out->nat_status), natStatus);

    _snprintf_s(out->nat_route_status, sizeof(out->nat_route_status), _TRUNCATE,
        "%s, %s",
        FriendlyConnectPreferenceLabel(s_connectPreference),
        s_allowIPv6Endpoint ? "IPv6 text accepted" : "IPv4/DNS only");
    _snprintf_s(out->nat_mapping_status, sizeof(out->nat_mapping_status), _TRUNCATE,
        "UPnP %s, PCP %s, game port %u/%u",
        Net::NatStatusName(natSnap.upnp_status),
        Net::NatStatusName(natSnap.pcp_status),
        natSnap.mapped_port,
        natSnap.pcp_mapped_port);
    _snprintf_s(out->nat_punch_status, sizeof(out->nat_punch_status), _TRUNCATE,
        "%s%s",
        s_holePunchEnabled ? "On" : "Off",
        s_relayEndpoint[0] ? ", custom relay" : ", default relay");
    _snprintf_s(out->nat_stun_status, sizeof(out->nat_stun_status), _TRUNCATE,
        "%s", Net::StunStatusName(natSnap.stun_status));
    if (endpointShort[0] && endpointShort[0] != '-') {
        _snprintf_s(out->nat_stun_endpoint, sizeof(out->nat_stun_endpoint), _TRUNCATE,
            "%s", endpointShort);
    }
    _snprintf_s(out->spectator_punch_status, sizeof(out->spectator_punch_status), _TRUNCATE,
        "%s, watch port %u%s",
        s_holePunchEnabled ? "Punch on" : "Punch off",
        s_spectatorListenPort,
        s_relayEndpoint[0] ? ", custom relay" : ", default relay");

    Net::DelayPolicySnapshot delaySnap{};
    Net::DelayPolicy_GetSnapshot(&delaySnap);

    Net::SpectatorRuntimeSnapshot spectatorRuntime{};
    Net::SpectatorRuntime_GetSnapshot(&spectatorRuntime);
    out->connected_spectators = spectatorRuntime.connected_spectators;
    CopyText(out->spectator_status, sizeof(out->spectator_status), spectatorRuntime.status);

    Net::NetplayPaletteRuntimeSnapshot paletteSnap{};
    Net::NetplayPaletteRuntime_GetSnapshot(&paletteSnap);
    CopyText(out->palette_status, sizeof(out->palette_status), paletteSnap.status);

    Net::SpectatorClientSnapshot spectatorClient{};
    Net::SpectatorClient_GetSnapshot(&spectatorClient);
    CopyDisplayedSpectatorEndpoint(out->spectator_endpoint,
        sizeof(out->spectator_endpoint),
        &spectatorClient);
    out->spectator_client_active = spectatorClient.active;
    out->spectator_client_match_id = spectatorClient.match_id;
    out->spectator_client_match_ordinal = spectatorClient.match_ordinal;
    CopyText(out->spectator_p1_name, sizeof(out->spectator_p1_name), spectatorClient.p1_name);
    CopyText(out->spectator_p2_name, sizeof(out->spectator_p2_name), spectatorClient.p2_name);
    out->spectator_p1_wins = spectatorClient.p1_wins;
    out->spectator_p2_wins = spectatorClient.p2_wins;
    out->spectator_client_buffered_frames = spectatorClient.buffered_frame_count;
    out->spectator_client_buffer_start = spectatorClient.buffered_start_rb_frame;
    out->spectator_client_buffer_end = spectatorClient.buffered_end_rb_frame;
    out->spectator_client_confirmed_edge = spectatorClient.confirmed_contiguous_rb_frame;
    out->spectator_client_playback_frame = spectatorClient.playback_rb_frame;
    out->spectator_client_should_fast_forward = spectatorClient.should_fast_forward;
    out->spectator_client_needs_hard_sync = spectatorClient.needs_hard_sync;
    out->spectator_client_relay_active = spectatorClient.relay_server_active;
    out->spectator_client_relay_port = spectatorClient.relay_listen_port;
    out->spectator_client_relay_spectators = spectatorClient.relay_connected_spectators;
    CopyText(out->spectator_client_status,
        sizeof(out->spectator_client_status),
        spectatorClient.error[0] ? spectatorClient.error : spectatorClient.status);

    Net::SpectatorPlaybackSnapshot spectatorPlayback{};
    Net::SpectatorPlayback_GetSnapshot(&spectatorPlayback);
    out->spectator_playback_active = spectatorPlayback.active;
    out->spectator_playback_frame = spectatorPlayback.local_playback_rb_frame;
    CopyText(out->spectator_playback_status,
        sizeof(out->spectator_playback_status),
        spectatorPlayback.status);

    out->prompt_active = IsActionPromptOpen();
    out->prompt_selected_index = s_actionPromptSelectedIndex;
    out->prompt_option_count = s_actionPromptOptionCount;
    CopyText(out->prompt_title, sizeof(out->prompt_title), s_actionPromptTitle);
    CopyText(out->prompt_body, sizeof(out->prompt_body), s_actionPromptBody);
    CopyText(out->prompt_option_labels[0], sizeof(out->prompt_option_labels[0]), s_actionPromptOptions[0]);
    CopyText(out->prompt_option_labels[1], sizeof(out->prompt_option_labels[1]), s_actionPromptOptions[1]);
    CopyText(out->prompt_option_labels[2], sizeof(out->prompt_option_labels[2]), s_actionPromptOptions[2]);

    Net::SpectatorDiscoverySnapshot spectatorDiscovery{};
    Net::SpectatorClient_GetDiscoverySnapshot(&spectatorDiscovery);
    out->spectator_lan_discovery_active = spectatorDiscovery.active;
    out->spectator_lan_result_count = spectatorDiscovery.result_count;
    CopyText(out->spectator_lan_discovery_status,
        sizeof(out->spectator_lan_discovery_status),
        spectatorDiscovery.status);

    // Peer info from session
    Net::SessionSnapshot sessionSnap{};
    Net::Session_GetSnapshot(&sessionSnap);
    if (sessionSnap.active) {
        CopyText(out->peer_nickname, sizeof(out->peer_nickname), sessionSnap.remote_peer.nickname);
        out->rtt_ms = delaySnap.measurement_valid ? delaySnap.measured_avg_ping_ms
                                                   : sessionSnap.stats.rtt_ms;
        out->is_host = (sessionSnap.role == Net::SessionRole::Host);
        out->local_accepted  = sessionSnap.local_ready;
        out->remote_accepted = sessionSnap.remote_ready;
        if (sessionSnap.remote_peer.frame_timing_valid) {
            out->remote_frame_timing_valid = true;
            out->remote_frame_timing_mode = sessionSnap.remote_peer.frame_timing_mode;
        }
    }

    // Active delay from delay policy
    out->active_delay = delaySnap.active_delay;

    // Rollback config
    out->rollback_budget = s_rollbackBudget;
    out->rollback_tolerance = s_rollbackTolerance;
    out->gameplay_delay_mode = (int)s_gameplayDelayMode;
    out->recommended_delay = delaySnap.recommended_delay;
    out->recommended_max_rollback = delaySnap.recommended_max_rollback;
    out->stall_threshold = delaySnap.stall_threshold;
    out->local_frame_timing_mode = IsFrameLimiter60FpsPatchEnabled()
        ? (int)Net::FrameTimingMode::Proper60
        : (int)Net::FrameTimingMode::Vanilla58_8;
    out->frame_timing_session_locked = TickHooks_IsFrameLimiter60FpsSessionOverrideActive();

    const int expectedDepth =
        (std::max)(0,
            (int)ceilf(delaySnap.measured_one_way_frames) - delaySnap.remote_announced_delay);
    out->stall_warning = delaySnap.measurement_valid &&
                         (expectedDepth > s_rollbackBudget);

    Net::PregameSnapshot pregameSnap{};
    Net::PregameSync_GetSnapshot(&pregameSnap);
    if (pregameSnap.round_count_valid) {
        out->current_rounds_to_win = pregameSnap.rounds_to_win;
        CopyText(out->current_rounds_label,
            sizeof(out->current_rounds_label),
            pregameSnap.rounds_label);
    } else if (sessionSnap.active && sessionSnap.role == Net::SessionRole::Host) {
        Net::GameSettingsSyncSnapshot settingsSnap{};
        Net::GameSettingsSync_GetSnapshot(&settingsSnap);
        out->current_rounds_to_win = settingsSnap.current_rounds_to_win;
        Net::GameSettingsSync_FormatRoundLabel(settingsSnap.current_round_option,
            out->current_rounds_label,
            sizeof(out->current_rounds_label));
    } else if (sessionSnap.active &&
               sessionSnap.role == Net::SessionRole::Join &&
               sessionSnap.remote_peer.round_count_valid) {
        out->current_rounds_to_win =
            Net::GameSettingsSync_RoundsToWin(sessionSnap.remote_peer.round_count);
        Net::GameSettingsSync_FormatRoundLabel(sessionSnap.remote_peer.round_count,
            out->current_rounds_label,
            sizeof(out->current_rounds_label));
    }

    // Local nickname
    CopyText(out->local_nickname, sizeof(out->local_nickname), s_localNickname);

    // Text editing state
    out->is_text_editing = IsTextEditing();
    out->text_edit_field = s_textEditField;
    CopyText(out->text_edit_buffer, sizeof(out->text_edit_buffer), s_textEditBuffer);
    out->text_cursor_pos = s_textCursorPos;

    // Set tracker wins
    Net::SetTrackerSnapshot setSnap{};
    Net::SetTracker_GetSnapshot(&setSnap);
    out->local_wins = setSnap.local_wins;
    out->remote_wins = setSnap.remote_wins;

    CopyText(out->hud_trail_color_label, sizeof(out->hud_trail_color_label),
        NetplayHudStyle::GetTrailPresetLabel());
    CopyText(out->hud_text_color_label, sizeof(out->hud_text_color_label),
        NetplayHudStyle::GetTextPresetLabel());
    CopyText(out->hud_score_color_label, sizeof(out->hud_score_color_label),
        NetplayHudStyle::GetScorePresetLabel());
    CopyText(out->hud_vertical_position_label, sizeof(out->hud_vertical_position_label),
        NetplayHudStyle::GetVerticalPositionLabel());
    CopyText(out->hud_font_size_label, sizeof(out->hud_font_size_label),
        NetplayHudStyle::GetFontSizeLabel());
    CopyText(out->hud_render_mode_label, sizeof(out->hud_render_mode_label),
        NetplayHudStyle::GetRenderModeLabel());
    NetplayHudStyle::Settings hudStyle{};
    NetplayHudStyle::GetLocal(&hudStyle);
    out->hud_trail_length = (int)hudStyle.trail_length_px;
}

void RenderFrame() {
    if (!MenuVisible()) return;
    if (ModeOwnership::IsPendingMenuRestore()) return;
    MenuSnapshot snap{};
    GetSnapshot(&snap);
    NetMenuUI::Render(&snap);
}

void RenderMainMenuReturnFade() {
    if (s_mainMenuReturnFade <= 0) return;
    // alpha: kFadeFrames -> 255 (black), 0 -> clear. Draw over the vanilla menu,
    // then decrement so it fades in over kFadeFrames frames.
    const float norm = (float)s_mainMenuReturnFade / (float)kFadeFrames;
    uint8_t alpha = (uint8_t)(norm * 255.0f + 0.5f);
    NetMenuUI::RenderFullscreenFade(alpha);
    --s_mainMenuReturnFade;
}

} // namespace NetMenu
