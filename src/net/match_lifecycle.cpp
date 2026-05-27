/**
 * Alice Senki 2 - Match Lifecycle State Management
 *
 * Classifies the game's current state within Mode 8 and adjacent modes,
 * keeps session/network ownership across all match-adjacent states,
 * and handles transitions between intro/gameplay/pause/end/post-match.
 */

#include "net/match_lifecycle.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/mode_ownership.h"
#include "net/netplay_menu_controller.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "rollback/netplay_log.h"
#include "rollback/owner_diagnostics.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace {

using namespace Net;

// ============================================================================
// Match header field offsets (from ADDR_MATCH_HEADER = 0x76C5F8)
// ============================================================================

// Winner byte: written by sub_4CA210 from a2+4
constexpr uint32_t MATCH_HEADER_WINNER_OFFSET    = 4;
// Match end route byte: read by sub_4CA210 from a2+10
constexpr uint32_t MATCH_HEADER_END_ROUTE_OFFSET = 10;

// Match end route values (byte at match_header+10)
constexpr uint8_t ROUTE_STORY      = 0;   // → MODE_WINSCREEN (9) — arcade/story continue
constexpr uint8_t ROUTE_CHARSEL    = 1;   // → MODE_CHARSEL (6) — VS rematch
constexpr uint8_t ROUTE_MENU       = 2;   // → MODE_MENU (3)
constexpr uint8_t ROUTE_REPLAY_SEL = 3;   // → MODE_REPLAY_SELECT (5) — replay finished
constexpr uint8_t ROUTE_TITLE      = 4;   // → MODE_TITLE (2)
constexpr uint8_t ROUTE_LOBBY      = 5;   // → MODE_LOBBY (4) — vanilla netplay end

// Pause flag address
constexpr uintptr_t ADDR_PAUSE_FLAG = 0x816380;

// ============================================================================
// Internal state
// ============================================================================

static bool                s_initialized = false;
static MatchLifecyclePhase s_phase       = MatchLifecyclePhase::Inactive;
static bool                s_matchOwned  = false;
static char                s_error[128]  = "";

// Tracking for transitions
static uint32_t            s_lastMode    = 0;
static uint32_t            s_lastSub     = 0;
static DWORD               s_phaseStart  = 0;

// Post-match routing intent
enum class PostMatchIntent : uint8_t {
    None = 0,
    Rematch,
    ReturnToSession,
    Disconnect,
};
static PostMatchIntent     s_postMatchIntent = PostMatchIntent::None;

// ============================================================================
// Memory helpers
// ============================================================================

static uint8_t ReadU8(uintptr_t a, uint8_t d = 0) {
    __try { return *(volatile uint8_t*)a; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return d; }
}

static void WriteU8(uintptr_t a, uint8_t v) {
    __try { *(volatile uint8_t*)a = v; }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void WriteU32(uintptr_t a, uint32_t v) {
    __try { *(volatile uint32_t*)a = v; }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// ============================================================================
// Phase transition
// ============================================================================

static void SetPhase(MatchLifecyclePhase next, const char* why) {
    if (s_phase == next) return;
    const char* oldName = MatchLifecyclePhaseName(s_phase);
    const char* newName = MatchLifecyclePhaseName(next);
    LOG_NETPLAY(LOG_INFO, "[MatchLife] Phase %s -> %s (%s)",
        oldName, newName, why ? why : "?");
    Rollback::NetplayLog_StateChange("LIFECYCL", -1,
        "MatchPhase", oldName, newName, why ? why : "?");
    s_phase = next;
    s_phaseStart = GetTickCount();
}

static void SetError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_error, sizeof(s_error), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// ============================================================================
// Game state classification
// ============================================================================

static bool IsSessionAlive() {
    SessionSnapshot snap{};
    Session_GetSnapshot(&snap);
    return snap.active &&
           snap.state != SessionState::Failed &&
           snap.state != SessionState::Idle;
}

static uint8_t GetMatchEndRoute() {
    return ReadU8(ADDR_MATCH_HEADER + MATCH_HEADER_END_ROUTE_OFFSET, 0);
}

static uint8_t GetMatchWinner() {
    return ReadU8(ADDR_MATCH_HEADER + MATCH_HEADER_WINNER_OFFSET, 0);
}

// ============================================================================
// Vanilla suppression during owned lifecycle
// ============================================================================

/// Keep game type as VS_HUMAN and vanilla netplay flags clear.
/// Called every frame while the match is owned.
static void EnforceModOwnership() {
    uint32_t type = GetGameType();
    if (type == GAMETYPE_NETPLAY) {
        WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
        ModeOwnership::ClearVanillaNetplayFlags();
    }
}

/// Override the match end route byte to prevent vanilla lobby return.
/// For mod-owned netplay, route 5 (→ MODE_LOBBY) and route 1 (→ MODE_CHARSEL)
/// must be intercepted at the SetGameMode hook level, but we can also
/// pre-write the route to ensure VS_HUMAN path (route 1 = CharSel) is taken.
static void SanitizeMatchEndRoute() {
    uint8_t route = GetMatchEndRoute();
    if (route == ROUTE_LOBBY) {
        // Vanilla netplay end route → rewrite to VS rematch (CharSel)
        WriteU8(ADDR_MATCH_HEADER + MATCH_HEADER_END_ROUTE_OFFSET, ROUTE_CHARSEL);
        LOG_NETPLAY(LOG_INFO, "[MatchLife] Replaced vanilla lobby route with CharSel route");
    }
}

// ============================================================================
// Phase detection from game state (Mode 8 substates)
// ============================================================================

/// Classify the current Mode 8 substate into a lifecycle phase.
/// This is the primary state detection function.
static MatchLifecyclePhase ClassifyMatchState() {
    uint32_t mode = GetGameMode();
    uint32_t sub  = GetSubstate();

    // Not in Mode 8 — check adjacent modes
    if (mode != MODE_MATCH) {
        if (mode == MODE_WINSCREEN) {
            return MatchLifecyclePhase::WinScreenActive;
        }
        if (mode == MODE_CHARSEL) {
            return MatchLifecyclePhase::ReturningToCharSel;
        }
        if (mode == MODE_MENU) {
            return MatchLifecyclePhase::ReturningToMenu;
        }
        // Any other mode while match was owned = disconnect/error
        return MatchLifecyclePhase::Inactive;
    }

    // Mode 8 substates
    switch (sub) {
        case MATCH_SUB_LOAD_ASSETS:
            return MatchLifecyclePhase::LoadingAssets;

        case MATCH_SUB_SETUP:
            return MatchLifecyclePhase::MatchSetup;

        case MATCH_SUB_INIT:
            return MatchLifecyclePhase::MatchInit;

        case MATCH_SUB_GAMEPLAY: {
            // Substate 3 is broad — disambiguate
            if (IsMatchIntroActive()) {
                return MatchLifecyclePhase::IntroActive;
            }
            if (IsMatchTransitionActive()) {
                return MatchLifecyclePhase::RoundTransition;
            }
            return MatchLifecyclePhase::PlayableGameplay;
        }

        case MATCH_SUB_PAUSE:
            return MatchLifecyclePhase::PauseActive;

        case MATCH_SUB_END:
            return MatchLifecyclePhase::MatchEnd;

        default:
            return MatchLifecyclePhase::Inactive;
    }
}

// ============================================================================
// Phase update functions
// ============================================================================

static void UpdateBootstrapWait() {
    // Waiting for game to enter Mode 8 after PregameSync GameplayHandoff
    uint32_t mode = GetGameMode();
    if (mode == MODE_MATCH) {
        SetPhase(ClassifyMatchState(), "entered Mode 8");
    }
}

static void UpdateLoadingAssets() {
    EnforceModOwnership();
    // Nothing active to do — game is loading. Session pump continues via Session_Update.
    MatchLifecyclePhase detected = ClassifyMatchState();
    if (detected != MatchLifecyclePhase::LoadingAssets) {
        SetPhase(detected, "asset loading complete");
    }
}

static void UpdateMatchSetup() {
    EnforceModOwnership();
    MatchLifecyclePhase detected = ClassifyMatchState();
    if (detected != MatchLifecyclePhase::MatchSetup) {
        SetPhase(detected, "setup complete");
    }
}

static void UpdateMatchInit() {
    EnforceModOwnership();
    MatchLifecyclePhase detected = ClassifyMatchState();
    if (detected != MatchLifecyclePhase::MatchInit) {
        SetPhase(detected, "init complete");
    }
}

static void UpdateIntroActive() {
    EnforceModOwnership();
    MatchLifecyclePhase detected = ClassifyMatchState();
    if (detected == MatchLifecyclePhase::PlayableGameplay) {
        SetPhase(MatchLifecyclePhase::PlayableGameplay, "intro ended — gameplay playable");
    } else if (detected != MatchLifecyclePhase::IntroActive) {
        SetPhase(detected, "intro state changed");
    }
}

static void UpdatePlayableGameplay() {
    EnforceModOwnership();

    // Check for state changes
    MatchLifecyclePhase detected = ClassifyMatchState();
    if (detected == MatchLifecyclePhase::PauseActive) {
        SetPhase(MatchLifecyclePhase::PauseActive, "pause entered");
    } else if (detected == MatchLifecyclePhase::RoundTransition) {
        SetPhase(MatchLifecyclePhase::RoundTransition, "round ending");
    } else if (detected == MatchLifecyclePhase::MatchEnd) {
        SanitizeMatchEndRoute();
        SetPhase(MatchLifecyclePhase::MatchEnd, "match ended");
    } else if (detected != MatchLifecyclePhase::PlayableGameplay) {
        SetPhase(detected, "gameplay state changed");
    }
}

static void UpdatePauseActive() {
    EnforceModOwnership();

    // Session pump continues — do NOT tear down anything.
    // Detect resume (return to Sub 3) or quit (mode transition)
    MatchLifecyclePhase detected = ClassifyMatchState();
    if (detected == MatchLifecyclePhase::PlayableGameplay) {
        SetPhase(MatchLifecyclePhase::PlayableGameplay, "resumed from pause");
    } else if (detected == MatchLifecyclePhase::IntroActive) {
        // Can happen if round boundary coincides with pause resume
        SetPhase(MatchLifecyclePhase::IntroActive, "resumed to intro");
    } else if (detected == MatchLifecyclePhase::MatchEnd) {
        SanitizeMatchEndRoute();
        SetPhase(MatchLifecyclePhase::MatchEnd, "quit from pause → match end");
    } else if (detected != MatchLifecyclePhase::PauseActive) {
        // Mode changed away from Mode 8 — pause-quit routing
        SetPhase(detected, "pause exit → mode change");
    }
}

static void UpdateRoundTransition() {
    EnforceModOwnership();

    // The 25-frame round-end countdown is running inside Sub 3.
    // When it completes, the game either:
    //   - Moves to Sub 5 (match end) if the match is over
    //   - Moves to Sub 2 (match init) for the next round
    MatchLifecyclePhase detected = ClassifyMatchState();
    if (detected == MatchLifecyclePhase::MatchInit) {
        SetPhase(MatchLifecyclePhase::MatchInit, "next round starting");
    } else if (detected == MatchLifecyclePhase::MatchEnd) {
        SanitizeMatchEndRoute();
        SetPhase(MatchLifecyclePhase::MatchEnd, "match ended after round");
    } else if (detected == MatchLifecyclePhase::IntroActive) {
        SetPhase(MatchLifecyclePhase::IntroActive, "round transition → intro");
    } else if (detected != MatchLifecyclePhase::RoundTransition) {
        SetPhase(detected, "round transition state changed");
    }
}

static void UpdateMatchEnd() {
    EnforceModOwnership();
    SanitizeMatchEndRoute();

    // Sub 5 will call SetGameMode to transition away.
    // The SetGameMode hook will intercept MODE_LOBBY and MODE_CHARSEL transitions.
    // We wait for the mode to actually change.
    uint32_t mode = GetGameMode();
    if (mode != MODE_MATCH) {
        // Match ended — determine route
        if (mode == MODE_WINSCREEN) {
            SetPhase(MatchLifecyclePhase::WinScreenActive, "match end → win screen");
        } else if (mode == MODE_CHARSEL) {
            SetPhase(MatchLifecyclePhase::PostMatchRoute, "match end → charsel route");
        } else if (mode == MODE_MENU) {
            SetPhase(MatchLifecyclePhase::PostMatchRoute, "match end → menu route");
        } else {
            // Unexpected mode — likely vanilla fallback intercepted
            SetPhase(MatchLifecyclePhase::PostMatchRoute, "match end → unexpected route");
        }
    }
}

static void UpdateWinScreenActive() {
    EnforceModOwnership();

    // Mode 9 (win screen) is active. Session stays alive.
    // WinScreenSync handles full per-frame winscreen lockstep.
    // We wait for the mode to change away from Mode 9.
    uint32_t mode = GetGameMode();
    if (mode != MODE_WINSCREEN) {
        if (mode == MODE_CHARSEL) {
            SetPhase(MatchLifecyclePhase::PostMatchRoute, "win screen → charsel");
        } else if (mode == MODE_MENU) {
            SetPhase(MatchLifecyclePhase::PostMatchRoute, "win screen → menu");
        } else {
            SetPhase(MatchLifecyclePhase::PostMatchRoute, "win screen → other");
        }
    }
}

static void UpdatePostMatchRoute() {
    EnforceModOwnership();

    // Post-match: the game has transitioned away from Mode 8.
    // Prefer explicit intent from the menu controller when available.
    switch (s_postMatchIntent) {
        case PostMatchIntent::Rematch:
            SetPhase(MatchLifecyclePhase::ReturningToCharSel, "rematch selected");
            s_postMatchIntent = PostMatchIntent::None;
            break;
        case PostMatchIntent::ReturnToSession:
            SetPhase(MatchLifecyclePhase::ReturningToMenu, "return to session");
            s_postMatchIntent = PostMatchIntent::None;
            break;
        case PostMatchIntent::Disconnect:
            s_matchOwned = false;
            SetPhase(MatchLifecyclePhase::Inactive, "disconnect from post-match");
            s_postMatchIntent = PostMatchIntent::None;
            break;
        default:
            break;
    }

    // If no explicit intent was set, honor the actual route chosen by the
    // game state so we do not stall in PostMatchRoute indefinitely.
    if (s_postMatchIntent == PostMatchIntent::None) {
        const uint32_t mode = GetGameMode();
        if (mode == MODE_CHARSEL) {
            SetPhase(MatchLifecyclePhase::ReturningToCharSel,
                "post-match direct route to charsel");
        } else if (mode == MODE_MENU) {
            SetPhase(MatchLifecyclePhase::ReturningToMenu,
                "post-match direct route to menu");
        } else if (!IsSessionAlive()) {
            s_matchOwned = false;
            SetPhase(MatchLifecyclePhase::Inactive,
                "post-match route lost session");
        }
    }
}

static void UpdateReturningToCharSel() {
    EnforceModOwnership();

    // Returning to CharSel for rematch — PregameSync will manage the charsel flow.
    // Once we're in charsel, this lifecycle instance is complete.
    // A new MatchLifecycle_OnMatchEnter() will be called when the next match starts.
    uint32_t mode = GetGameMode();
    if (mode == MODE_CHARSEL) {
        s_matchOwned = false;
        SetPhase(MatchLifecyclePhase::Inactive, "arrived at CharSel for rematch");
    }
}

static void UpdateReturningToMenu() {
    EnforceModOwnership();

    // Returning to menu — session stays alive, menu controller takes over.
    uint32_t mode = GetGameMode();
    if (mode == MODE_MENU) {
        s_matchOwned = false;
        SetPhase(MatchLifecyclePhase::Inactive, "arrived at menu");
    }
}

static void UpdateDisconnectRecovery() {
    // Session is dead or dying, we're unwinding.
    // ModeOwnership and the menu controller handle the actual recovery.
    // We just wait until we're back at MODE_MENU.
    uint32_t mode = GetGameMode();
    if (mode == MODE_MENU) {
        s_matchOwned = false;
        SetPhase(MatchLifecyclePhase::Inactive, "disconnect recovery complete");
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void MatchLifecycle_Init() {
    if (s_initialized) return;
    s_phase = MatchLifecyclePhase::Inactive;
    s_matchOwned = false;
    s_error[0] = '\0';
    s_postMatchIntent = PostMatchIntent::None;
    s_lastMode = 0;
    s_lastSub = 0;
    s_phaseStart = 0;
    s_initialized = true;
    LOG_NETPLAY(LOG_DEBUG, "[MatchLife] Initialized");
}

void MatchLifecycle_Shutdown() {
    if (!s_initialized) return;
    s_phase = MatchLifecyclePhase::Inactive;
    s_matchOwned = false;
    s_initialized = false;
    LOG_NETPLAY(LOG_DEBUG, "[MatchLife] Shutdown");
}

void MatchLifecycle_FrameUpdate() {
    if (!s_initialized) return;
    if (s_phase == MatchLifecyclePhase::Inactive) return;

    // Check for session loss in any owned state
    if (s_matchOwned && !IsSessionAlive()) {
        if (s_phase != MatchLifecyclePhase::DisconnectRecovery &&
            s_phase != MatchLifecyclePhase::PostMatchRoute &&
            s_phase != MatchLifecyclePhase::ReturningToCharSel &&
            s_phase != MatchLifecyclePhase::ReturningToMenu) {
            LOG_NETPLAY(LOG_WARNING, "[MatchLife] Session lost during %s",
                MatchLifecyclePhaseName(s_phase));
            NetMenu::HandleDisconnection("Session lost during match");
            return;
        }
    }

    // Log mode/substate transitions for debugging
    uint32_t mode = GetGameMode();
    uint32_t sub  = GetSubstate();
    if (mode != s_lastMode || sub != s_lastSub) {
        LOG_NETPLAY(LOG_DEBUG, "[MatchLife] Game state: mode=%u sub=%u (phase=%s)",
            mode, sub, MatchLifecyclePhaseName(s_phase));
        s_lastMode = mode;
        s_lastSub = sub;
    }

    // Drive phase-specific updates
    switch (s_phase) {
        case MatchLifecyclePhase::BootstrapWait:      UpdateBootstrapWait();      break;
        case MatchLifecyclePhase::LoadingAssets:       UpdateLoadingAssets();       break;
        case MatchLifecyclePhase::MatchSetup:          UpdateMatchSetup();          break;
        case MatchLifecyclePhase::MatchInit:           UpdateMatchInit();           break;
        case MatchLifecyclePhase::IntroActive:         UpdateIntroActive();         break;
        case MatchLifecyclePhase::PlayableGameplay:    UpdatePlayableGameplay();    break;
        case MatchLifecyclePhase::PauseActive:         UpdatePauseActive();         break;
        case MatchLifecyclePhase::RoundTransition:     UpdateRoundTransition();     break;
        case MatchLifecyclePhase::MatchEnd:            UpdateMatchEnd();            break;
        case MatchLifecyclePhase::WinScreenActive:     UpdateWinScreenActive();     break;
        case MatchLifecyclePhase::PostMatchRoute:      UpdatePostMatchRoute();      break;
        case MatchLifecyclePhase::ReturningToCharSel:  UpdateReturningToCharSel();  break;
        case MatchLifecyclePhase::ReturningToMenu:     UpdateReturningToMenu();     break;
        case MatchLifecyclePhase::DisconnectRecovery:  UpdateDisconnectRecovery();  break;
        default: break;
    }
}

void MatchLifecycle_OnMatchEnter() {
    if (!s_initialized) return;

    LOG_NETPLAY(LOG_INFO, "[MatchLife] Match entered (PregameSync handoff)");
    Rollback::OwnerDiag_Log("match_start");
    s_matchOwned = true;
    s_error[0] = '\0';
    s_postMatchIntent = PostMatchIntent::None;
    s_lastMode = GetGameMode();
    s_lastSub = GetSubstate();

    // If we're already in Mode 8, classify immediately
    if (s_lastMode == MODE_MATCH) {
        SetPhase(ClassifyMatchState(), "match enter (already in Mode 8)");
    } else {
        SetPhase(MatchLifecyclePhase::BootstrapWait, "match enter (waiting for Mode 8)");
    }
}

void MatchLifecycle_OnMatchExit() {
    if (!s_initialized) return;

    LOG_NETPLAY(LOG_INFO, "[MatchLife] Match exited cleanly from %s",
        MatchLifecyclePhaseName(s_phase));
    s_matchOwned = false;
    s_postMatchIntent = PostMatchIntent::None;
    SetPhase(MatchLifecyclePhase::Inactive, "clean exit");
}

void MatchLifecycle_OnDisconnect(const char* reason) {
    if (!s_initialized) return;

    LOG_NETPLAY(LOG_WARNING, "[MatchLife] Disconnect during %s: %s",
        MatchLifecyclePhaseName(s_phase), reason ? reason : "unknown");

    SetError("%s", reason ? reason : "Disconnected");

    // Clear vanilla netplay flags immediately
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ModeOwnership::ClearVanillaNetplayFlags();

    SetPhase(MatchLifecyclePhase::DisconnectRecovery, reason ? reason : "disconnect");
}

void MatchLifecycle_OnRematch() {
    if (!s_initialized) return;
    if (s_phase == MatchLifecyclePhase::PostMatchRoute ||
        s_phase == MatchLifecyclePhase::MatchEnd) {
        s_postMatchIntent = PostMatchIntent::Rematch;
        LOG_NETPLAY(LOG_INFO, "[MatchLife] Post-match intent: Rematch");
    }
}

void MatchLifecycle_OnReturnToSession() {
    if (!s_initialized) return;
    if (s_phase == MatchLifecyclePhase::PostMatchRoute ||
        s_phase == MatchLifecyclePhase::MatchEnd) {
        s_postMatchIntent = PostMatchIntent::ReturnToSession;
        LOG_NETPLAY(LOG_INFO, "[MatchLife] Post-match intent: ReturnToSession");
    }
}

MatchLifecyclePhase MatchLifecycle_GetPhase() {
    return s_phase;
}

void MatchLifecycle_GetSnapshot(MatchLifecycleSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->active = (s_phase != MatchLifecyclePhase::Inactive);
    out->phase = s_phase;

    // Classification flags
    out->match_owned = s_matchOwned;

    out->session_should_be_alive =
        s_matchOwned &&
        s_phase != MatchLifecyclePhase::Inactive &&
        s_phase != MatchLifecyclePhase::DisconnectRecovery;

    out->gameplay_is_playable =
        s_phase == MatchLifecyclePhase::PlayableGameplay;

    out->pause_is_active =
        s_phase == MatchLifecyclePhase::PauseActive;

    out->transition_in_progress =
        s_phase == MatchLifecyclePhase::RoundTransition ||
        s_phase == MatchLifecyclePhase::MatchEnd;

    out->post_match_routing =
        s_phase == MatchLifecyclePhase::PostMatchRoute ||
        s_phase == MatchLifecyclePhase::ReturningToCharSel ||
        s_phase == MatchLifecyclePhase::ReturningToMenu;

    // Current game state
    out->game_mode = GetGameMode();
    out->game_substate = GetSubstate();
    out->game_type = GetGameType();

    // Match details
    out->match_end_route = GetMatchEndRoute();
    out->winner = GetMatchWinner();

    // Error
    strncpy_s(out->error, sizeof(out->error), s_error, _TRUNCATE);
}

bool MatchLifecycle_IsGameplayPlayable() {
    return s_phase == MatchLifecyclePhase::PlayableGameplay;
}

bool MatchLifecycle_SessionShouldBeAlive() {
    return s_matchOwned &&
           s_phase != MatchLifecyclePhase::Inactive &&
           s_phase != MatchLifecyclePhase::DisconnectRecovery;
}

bool MatchLifecycle_IsMatchOwned() {
    return s_matchOwned;
}

bool MatchLifecycle_IsPauseActive() {
    return s_phase == MatchLifecyclePhase::PauseActive;
}

bool MatchLifecycle_IsPostMatchRouting() {
    return s_phase == MatchLifecyclePhase::PostMatchRoute ||
           s_phase == MatchLifecyclePhase::ReturningToCharSel ||
           s_phase == MatchLifecyclePhase::ReturningToMenu;
}

} // namespace Net
