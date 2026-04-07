/**
 * Alice Senki 2 - Sync Policy Implementation
 *
 * State classification, confirm arming, and Stage Select enforcement.
 */

#include "net/sync_policy.h"
#include "net/match_lifecycle.h"
#include "net/session_manager.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "core/mod_main.h"
#include "ui/log_window.h"

namespace Net {

// ============================================================================
// Memory Helpers (local, SEH-safe)
// ============================================================================

static void WriteU8(uintptr_t a, uint8_t v) {
    __try { *(volatile uint8_t*)a = v; } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

template<typename T>
static T ReadMemory(uintptr_t a) {
    __try { return *(volatile T*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return T{}; }
}

// ============================================================================
// Internal State
// ============================================================================

static SyncMode         s_currentMode       = SyncMode::None;
static LockstepContext  s_lockstepContext    = LockstepContext::None;
static bool             s_confirmArmed      = false;
static bool             s_lastConfirmState  = false;  // Previous frame's confirm press
static bool             s_stageSelectForced = false;
static bool             s_initialized       = false;

// ============================================================================
// Classification Logic
// ============================================================================

/// Determine sync mode + lockstep context from current game state.
static void ClassifyCurrentState(SyncMode* outMode, LockstepContext* outContext) {
    // Default: no sync
    *outMode = SyncMode::None;
    *outContext = LockstepContext::None;

    // Must have an active session
    SessionSnapshot snap;
    Session_GetSnapshot(&snap);
    if (snap.state != SessionState::Connected) {
        return;
    }

    uint32_t mode = GetGameMode();
    uint32_t sub  = GetSubstate();

    // -- CharSel lockstep --
    if (mode == MODE_CHARSEL) {
        // Substates 2 (SELECT) and 4 (CONFIRM) are interactive lockstep
        if (sub == CHARSEL_SUB_SELECT || sub == CHARSEL_SUB_CONFIRM) {
            *outMode = SyncMode::Lockstep;
            *outContext = LockstepContext::CharSelect;
            return;
        }
        // Substate 7 (STAGESEL_GRID) is stage selection within charsel
        if (sub == CHARSEL_SUB_STAGESEL_GRID) {
            *outMode = SyncMode::Lockstep;
            *outContext = LockstepContext::StageSelect;
            return;
        }
        // Other charsel substates (loading, transitions) are passive
        *outMode = SyncMode::Passive;
        return;
    }

    // -- Mode 7 is NOT stage select, it's a non-interactive pre-match intro --
    // No lockstep or rollback needed. If session is alive, stay passive.
    if (mode == MODE_PREMATCH_INTRO) {
        if (Net::MatchLifecycle_SessionShouldBeAlive()) {
            *outMode = SyncMode::Passive;
        }
        return;
    }

    // -- Match states --
    if (mode == MODE_MATCH) {
        // Check lifecycle phase for authoritative classification
        MatchLifecycleSnapshot lcSnap;
        lcSnap.active = false;
        // Use the lifecycle queries directly
        if (!MatchLifecycle_IsMatchOwned()) {
            return;  // Not our match
        }

        switch (sub) {
            case MATCH_SUB_LOAD_ASSETS:
            case MATCH_SUB_SETUP:
            case MATCH_SUB_INIT:
                // Pre-gameplay: passive (session alive, no input sync)
                *outMode = SyncMode::Passive;
                return;

            case MATCH_SUB_GAMEPLAY: {
                // Sub 3 is overloaded: intro, playable, round transition
                if (IsMatchIntroActive()) {
                    *outMode = SyncMode::Passive;
                    return;
                }
                if (IsMatchTransitionActive()) {
                    *outMode = SyncMode::Passive;
                    return;
                }
                // Playable gameplay — rollback
                if (MatchLifecycle_IsGameplayPlayable()) {
                    *outMode = SyncMode::Rollback;
                    return;
                }
                // Fallback passive
                *outMode = SyncMode::Passive;
                return;
            }

            case MATCH_SUB_PAUSE:
                *outMode = SyncMode::Lockstep;
                *outContext = LockstepContext::Pause;
                return;

            case MATCH_SUB_END:
                *outMode = SyncMode::Passive;
                return;

            default:
                *outMode = SyncMode::Passive;
                return;
        }
    }

    // -- WinScreen lockstep --
    if (mode == MODE_WINSCREEN) {
        if (MatchLifecycle_IsMatchOwned() || MatchLifecycle_IsPostMatchRouting()) {
            *outMode = SyncMode::Lockstep;
            *outContext = LockstepContext::WinScreen;
            return;
        }
    }

    // -- Post-match route decision --
    if (MatchLifecycle_IsPostMatchRouting()) {
        *outMode = SyncMode::Lockstep;
        *outContext = LockstepContext::PostMatch;
        return;
    }

    // If lifecycle says session should be alive but we're not in a classified state,
    // stay passive to keep the network pump running
    if (Net::MatchLifecycle_SessionShouldBeAlive()) {
        *outMode = SyncMode::Passive;
        return;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void SyncPolicy_Init() {
    s_currentMode       = SyncMode::None;
    s_lockstepContext    = LockstepContext::None;
    s_confirmArmed      = false;
    s_lastConfirmState  = false;
    s_stageSelectForced = false;
    s_initialized       = true;
    LOG_INFO("[SyncPolicy] Initialized");
}

void SyncPolicy_Shutdown() {
    s_currentMode       = SyncMode::None;
    s_lockstepContext    = LockstepContext::None;
    s_confirmArmed      = false;
    s_lastConfirmState  = false;
    s_stageSelectForced = false;
    s_initialized       = false;
    LOG_INFO("[SyncPolicy] Shutdown");
}

// ============================================================================
// Per-Frame Update
// ============================================================================

void SyncPolicy_FrameUpdate() {
    if (!s_initialized) return;

    SyncMode prevMode = s_currentMode;
    LockstepContext prevCtx = s_lockstepContext;

    ClassifyCurrentState(&s_currentMode, &s_lockstepContext);

    // On mode transition, reset confirm arm to prevent stale confirms
    if (s_currentMode != prevMode || s_lockstepContext != prevCtx) {
        s_confirmArmed = false;
        s_lastConfirmState = false;

        if (ModConfig_VerboseLogging()) {
            LOG_INFO("[SyncPolicy] Mode transition: %s/%s -> %s/%s",
                SyncModeName(prevMode), LockstepContextName(prevCtx),
                SyncModeName(s_currentMode), LockstepContextName(s_lockstepContext));
        }
    }

    // Enforce Stage Select stays ON if forced
    if (s_stageSelectForced && s_currentMode != SyncMode::None) {
        uint8_t current = ReadMemory<uint8_t>(ADDR_STAGESEL_ENABLE);
        if (current != 1) {
            WriteU8(ADDR_STAGESEL_ENABLE, 1);
        }
    }
}

// ============================================================================
// Classification Queries
// ============================================================================

SyncMode SyncPolicy_GetCurrentMode() {
    return s_currentMode;
}

LockstepContext SyncPolicy_GetLockstepContext() {
    return s_lockstepContext;
}

bool SyncPolicy_IsLockstep() {
    return s_currentMode == SyncMode::Lockstep;
}

bool SyncPolicy_IsRollbackActive() {
    return s_currentMode == SyncMode::Rollback;
}

bool SyncPolicy_IsPassive() {
    return s_currentMode == SyncMode::Passive;
}

bool SyncPolicy_IsSessionSynced() {
    return s_currentMode != SyncMode::None;
}

// ============================================================================
// Confirm Input Arming
// ============================================================================

bool SyncPolicy_IsConfirmArmed() {
    return s_confirmArmed;
}

bool SyncPolicy_FeedConfirmInput(bool confirm_pressed) {
    bool rising_edge = false;

    if (!confirm_pressed) {
        // Neutral frame — arm the confirm for next press
        s_confirmArmed = true;
    } else if (confirm_pressed && s_confirmArmed && !s_lastConfirmState) {
        // Rising edge with armed state — valid confirm
        rising_edge = true;
        s_confirmArmed = false;  // Consumed — must release again to re-arm
    }
    // If confirm_pressed but not armed, or held from previous frame: ignored

    s_lastConfirmState = confirm_pressed;
    return rising_edge;
}

void SyncPolicy_ResetConfirmArm() {
    s_confirmArmed = false;
    s_lastConfirmState = false;
}

// ============================================================================
// Stage Select Enforcement
// ============================================================================

void SyncPolicy_EnforceStageSelectForNetplay() {
    WriteU8(ADDR_STAGESEL_ENABLE, 1);
    s_stageSelectForced = true;
    LOG_INFO("[SyncPolicy] Stage Select forced ON for netplay");
}

bool SyncPolicy_IsStageSelectForced() {
    return s_stageSelectForced;
}

// ============================================================================
// Diagnostics
// ============================================================================

void SyncPolicy_GetSnapshot(SyncPolicySnapshot* out) {
    if (!out) return;
    out->current_mode      = s_currentMode;
    out->lockstep_context  = s_lockstepContext;
    out->session_active    = (s_currentMode != SyncMode::None);
    out->confirm_armed     = s_confirmArmed;
    out->stage_select_forced = s_stageSelectForced;
}

} // namespace Net
