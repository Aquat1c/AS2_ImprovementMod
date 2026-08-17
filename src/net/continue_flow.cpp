/**
 * Alice Senki 2 - Netplay Continue-Screen Rematch Flow
 *
 * Deterministic state machine over the vanilla Mode 9 continue screen:
 *
 *   Idle -> Armed (mode 9 entered, netplay match owned)
 *        -> Prompt (forced sub=4; per-player cursor/lock)
 *        -> RematchPending | DeclineCooldown (resolved)
 *        -> Idle (reset / latch consumed)
 *
 * Determinism rules (absolute):
 *   - Prompt decisions (toggles, locks, timeout) come ONLY from consumed
 *     lockstep frames fed by WinScreenSync_ConsumeCurrentFrame. Both peers see
 *     the identical stream, so both resolve identically.
 *   - Edge state (s_prevInputs) lives here and is tracked continuously from
 *     the first consumed frame of the winscreen phase; it is never re-derived.
 *     Mode 9 is lockstep-only (no rollback), so this is safe — and it doubles
 *     as the entry carry gate: the confirm that skipped the win pose is still
 *     held at prompt entry and therefore is not a rising edge.
 *   - No wall clocks anywhere: the 3600-frame timeout counts consumed frames.
 *
 * Decomp facts used (see plan doc):
 *   - Continue = mode 9 sub 4, handler sub_601BB0 @ 0x601BB0. Cursor byte
 *     0x8EA3B0 (0=YES, 1=NO), LEFT/RIGHT toggles, A/C confirms, no countdown.
 *   - The VS_HUMAN/NETPLAY skip gate lives in sub_6019F0 (sub 3): forcing the
 *     screen = write sub=4, subTimer=0, cursor=0 when sub 3 would advance.
 *   - Vanilla YES: sub 4 -> 5 (25f fade on 0x816370) -> 36 -> mode change,
 *     re-reading the live charsel globals (untouched on this path).
 *   - Vanilla NO under gametype 2 would run the 1920-frame GAME OVER slide
 *     (sub 6); we route sub=8 instead (plain 25f fade -> 36 -> charsel).
 */

#include "net/continue_flow.h"

#include "net/locked_match_config.h"
#include "net/match_lifecycle.h"
#include "net/player_side_mapping.h"
#include "net/pregame_sync.h"
#include "net/session_manager.h"
#include "net/transition_barrier.h"
#include "net/winscreen_sync.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "patches/memory_utils.h"
#include "rollback/online_wiring.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using namespace Net;

enum class FlowState : uint8_t {
    Idle = 0,
    Armed,           // mode 9 entered under an owned netplay match
    Prompt,          // continue screen forced; collecting per-player choices
    RematchPending,  // both YES — waiting for mode_ownership to consume latch
    DeclineCooldown, // routed to charsel — A/C carry gate until release
};

static const char* FlowStateName(FlowState s) {
    switch (s) {
        case FlowState::Idle:            return "Idle";
        case FlowState::Armed:           return "Armed";
        case FlowState::Prompt:          return "Prompt";
        case FlowState::RematchPending:  return "RematchPending";
        case FlowState::DeclineCooldown: return "DeclineCooldown";
    }
    return "?";
}

static bool      s_initialized = false;
static bool      s_enabled     = true;   // config-gated, ON by default
static FlowState s_state       = FlowState::Idle;

// Deterministic prompt state — mutated ONLY from consumed lockstep frames.
static uint16_t s_prevInputs[2] = {0, 0};
static uint8_t  s_cursor[2]     = {0, 0};   // 0 = YES, 1 = NO (vanilla cursor values)
static bool     s_locked[2]     = {false, false};
static uint8_t  s_choice[2]     = {0, 0};
static uint32_t s_promptFrames  = 0;
// Armed-state gate: only a flow that actually consumed frames at the win pose
// (sub 3) may treat a later sub as "skipped past the continue gate". A phase
// re-armed during the post-decline sub 8/36 fade must never re-force sub 4.
static bool     s_sawWinPose    = false;

// Resolution bookkeeping (per-machine, display/routing only).
static bool s_rematchLatched    = false;
static bool s_declineCarryClear = false;

// Timeout counts consumed lockstep frames (~60 s), never wall time.
constexpr uint32_t kPromptTimeoutFrames = 3600;

constexpr uint16_t kPromptSuppressMask =
    (uint16_t)(INPUT_LEFT | INPUT_RIGHT | INPUT_A | INPUT_C);
constexpr uint16_t kCarrySuppressMask = (uint16_t)(INPUT_A | INPUT_C);
// Same mask WinScreenSync uses for advance intent (winscreen_sync.cpp).
constexpr uint16_t kAdvanceMask = (uint16_t)(INPUT_A | INPUT_C | INPUT_START);

// BGM_PlayTrack(track): 255 stops — vanilla NO branch parity (the game's own
// NO branch never runs because the prompt suppresses its inputs).
using BgmPlayTrack_t = int(__cdecl*)(int track);
static BgmPlayTrack_t s_bgmPlay = reinterpret_cast<BgmPlayTrack_t>(ADDR_BGM_PLAY_TRACK);

// Local game side, same idiom as the HUD (mod_main.cpp): bootstrap-assigned
// player slot with session-role fallback (host = P1). Display-only — the
// decision logic is side-symmetric, so this never affects determinism.
static int ResolveLocalSide() {
    int slot = PlayerMapping_GetLocalGameSlot();
    if (slot != 0 && slot != 1) {
        slot = (Session_GetRole() == SessionRole::Host) ? 0 : 1;
    }
    return slot;
}

static void SetState(FlowState next, const char* why) {
    if (s_state == next) return;
    Rollback::NetplayLog_StateChange("CONTINUE", -1,
        "ContinueFlow", FlowStateName(s_state), FlowStateName(next),
        why ? why : "?");
    LOG_NETPLAY(LOG_INFO, "[ContinueFlow] %s -> %s (%s)",
        FlowStateName(s_state), FlowStateName(next), why ? why : "?");
    s_state = next;
}

// Decomp sub_6019F0 (mode 9 sub 3): the gametype 2/3 gate skips the continue
// screen. Forcing it = sub(0x816390)=4, subTimer(0x816394)=0, cursor
// (0x8EA3B0)=0 at the point sub 3 would otherwise advance. Assets/SEs are
// loaded unconditionally by sub_5FBEE0 for every mode-9 entry — no asset work.
static void ForceContinueScreen(const char* why) {
    WriteMemory<uint32_t>(ADDR_SUB_STATE, (uint32_t)STORY_SUB_DIALOGUE_END);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0u);
    WriteMemory<uint8_t>(ADDR_CONTINUE_CURSOR, 0u);
    Rollback::NetplayLog_Write("CONTINUE", -1,
        "Forced continue screen (sub=4): %s consume=%u",
        why ? why : "?", WinScreenSync_GetConsumeFrame());
}

static void BeginPrompt(const char* why) {
    // NOTE: s_prevInputs is intentionally NOT reset — continuity from before
    // entry is the carry gate against the still-held confirm (QOH99 #10).
    s_cursor[0] = s_cursor[1] = 0;
    s_locked[0] = s_locked[1] = false;
    s_choice[0] = s_choice[1] = 0;
    s_promptFrames = 0;
    s_rematchLatched = false;
    s_declineCarryClear = false;

    ForceContinueScreen(why);
    SetState(FlowState::Prompt, why);
}

static void ResolveDecline(const char* why) {
    Rollback::NetplayLog_Write("CONTINUE", -1,
        "=== CONTINUE RESOLVED: DECLINE (%s) frames=%u p1=%s p2=%s ===",
        why ? why : "?", s_promptFrames,
        s_locked[0] ? (s_choice[0] == 0 ? "YES" : "NO") : "undecided",
        s_locked[1] ? (s_choice[1] == 0 ? "YES" : "NO") : "undecided");

    // Skip the 1920-frame GAME OVER slide (sub 6): sub 8 is the plain
    // 25-frame fade -> sub 36 -> charsel (today's post-match flow).
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, 0u);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, (uint32_t)STORY_SUB_PREMATCH);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0u);

    // Vanilla NO stops the BGM in sub_601BB0; that branch never ran under
    // input suppression, so issue the stop ourselves for parity.
    if (s_bgmPlay) {
        s_bgmPlay(255);
    }

    s_declineCarryClear = false;
    SetState(FlowState::DeclineCooldown, why);

    // Telemetry/wire-signature only — fired AFTER resolution, decides nothing.
    // M7 (F-7): the "any NO" route is the lockstep-derived charsel restart —
    // the SAME intent the auto-rematch path announces, so both sides of a
    // healthy session always propose the identical value. The director is
    // told what the shared lockstep stream resolved to; a remote proposal
    // contradicting it can only mean divergent streams (fail-closed).
    Rollback::OnlineWiring_SetExpectedPostMatchIntent(
        (uint8_t)PostMatchIntentWire::CharselRestart);
    TransitionBarrier_Propose(NetTransitionKind::PostMatchDecision,
                              (uint8_t)PostMatchIntentWire::CharselRestart, 0);
}

static void ResolveRematch() {
    // Snapshot FIRST: the config is wiped by PregameSync_Begin's reset, and
    // it can be null if the previous run never reached ConfigAgreed.
    const LockedMatchConfig* cfg = PregameSync_GetLockedConfig();
    if (!cfg) {
        LOG_NETPLAY(LOG_WARNING,
            "[ContinueFlow] Rematch resolved but locked config unavailable — falling back to decline routing");
        Rollback::NetplayLog_Write("CONTINUE", -1,
            "Rematch resolution without locked config — declining");
        ResolveDecline("no locked config for rematch");
        return;
    }
    const LockedMatchConfig snapshot = *cfg;

    Rollback::NetplayLog_Write("CONTINUE", -1,
        "=== CONTINUE RESOLVED: REMATCH frames=%u p1=%u/%u p2=%u/%u stage=%u ===",
        s_promptFrames,
        snapshot.p1_character, snapshot.p1_palette,
        snapshot.p2_character, snapshot.p2_palette,
        snapshot.stage_id);

    MatchLifecycle_OnRematch();
    Rollback::OnlineWiring_OnRematch();

    // Release the winscreen lockstep before PregameSync_BeginRematch rebinds
    // the frontend epoch. Both peers resolve on the same consumed frame, so
    // no exit barrier handshake is needed here.
    WinScreenSync_FinalizeFromContinueFlow("continue rematch resolved");

    // Vanilla YES path: sub 5 (25f fade on 0x816370) -> 36 -> mode change.
    // Mode 7 re-reads the live charsel globals — nothing on this path may
    // touch 0x8E9F10/14, 0x8E9FE0/E4, 0x816470/71.
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, 0u);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, (uint32_t)STORY_SUB_EVENT_SETUP);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0u);

    s_rematchLatched = true;
    SetState(FlowState::RematchPending, "both locked YES");

    // Network side proceeds while the game fades: the session is still
    // Connected here (winscreen never tears it down), which is Begin()'s
    // precondition.
    if (!PregameSync_BeginRematch(&snapshot)) {
        LOG_NETPLAY(LOG_WARNING,
            "[ContinueFlow] PregameSync_BeginRematch failed — dropping latch, routing to charsel");
        Rollback::NetplayLog_Write("CONTINUE", -1,
            "BeginRematch failed after rematch resolution — decline routing");
        // Undo the YES routing: without a pregame fast path the mode-7 route
        // would start an unsynchronized match. Fall back to the charsel flow.
        s_rematchLatched = false;
        WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, 0u);
        WriteMemory<uint32_t>(ADDR_SUB_STATE, (uint32_t)STORY_SUB_PREMATCH);
        WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0u);
        SetState(FlowState::DeclineCooldown, "rematch begin failed");
        return;
    }

    // Telemetry/wire-signature only — fired AFTER resolution, decides nothing.
    // M7 (F-7): register the lockstep-derived answer with the director.
    Rollback::OnlineWiring_SetExpectedPostMatchIntent(
        (uint8_t)PostMatchIntentWire::Rematch);
    TransitionBarrier_Propose(NetTransitionKind::PostMatchDecision,
                              (uint8_t)PostMatchIntentWire::Rematch, 0);
}

static void StepPrompt(uint16_t p1, uint16_t p2) {
    // Fail-closed sanity: the prompt owns mode 9 sub 4 (input suppression
    // keeps sub_601BB0 from self-transitioning). Anything else means we lost
    // the screen — route to the safe decline path.
    const uint32_t mode = GetGameMode();
    const uint32_t sub = GetSubstate();
    if (mode != MODE_WINSCREEN || sub != STORY_SUB_DIALOGUE_END) {
        LOG_NETPLAY(LOG_WARNING,
            "[ContinueFlow] Prompt sanity failed (mode=%u sub=%u) — declining",
            mode, sub);
        ResolveDecline("prompt sanity failure");
        return;
    }

    const uint16_t inputs[2] = { p1, p2 };
    for (int side = 0; side < 2; ++side) {
        if (s_locked[side]) {
            continue;
        }
        const uint16_t rising =
            (uint16_t)(inputs[side] & (uint16_t)~s_prevInputs[side]);
        if (rising & (INPUT_LEFT | INPUT_RIGHT)) {
            s_cursor[side] ^= 1;
            Rollback::NetplayLog_Write("CONTINUE", -1,
                "P%d cursor -> %s (frame=%u)",
                side + 1, s_cursor[side] == 0 ? "YES" : "NO", s_promptFrames);
        }
        if (rising & (INPUT_A | INPUT_C)) {
            s_locked[side] = true;
            s_choice[side] = s_cursor[side];
            Rollback::NetplayLog_Write("CONTINUE", -1,
                "P%d locked %s (frame=%u)",
                side + 1, s_choice[side] == 0 ? "YES" : "NO", s_promptFrames);
        }
    }

    // Per-machine display of our own cursor only; game-state effects come
    // exclusively from resolution.
    WriteMemory<uint8_t>(ADDR_CONTINUE_CURSOR, s_cursor[ResolveLocalSide()]);

    s_promptFrames++;

    if (s_locked[0] && s_locked[1]) {
        if (s_choice[0] == 0 && s_choice[1] == 0) {
            ResolveRematch();
        } else {
            ResolveDecline("a player declined");
        }
        return;
    }

    if (s_promptFrames >= kPromptTimeoutFrames) {
        // Undecided sides count as NO; both peers hit this on the same
        // consumed frame.
        ResolveDecline("prompt timeout");
    }
}

static void ResetAll(const char* reason) {
    if (s_state != FlowState::Idle) {
        SetState(FlowState::Idle, reason ? reason : "reset");
    }
    s_prevInputs[0] = s_prevInputs[1] = 0;
    s_cursor[0] = s_cursor[1] = 0;
    s_locked[0] = s_locked[1] = false;
    s_choice[0] = s_choice[1] = 0;
    s_promptFrames = 0;
    s_sawWinPose = false;
    s_rematchLatched = false;
    s_declineCarryClear = false;
}

static ContinueChoiceState SideChoiceState(int side) {
    if (side != 0 && side != 1) {
        return ContinueChoiceState::Deciding;
    }
    if (s_state != FlowState::Prompt &&
        s_state != FlowState::RematchPending &&
        s_state != FlowState::DeclineCooldown) {
        return ContinueChoiceState::Deciding;
    }
    if (!s_locked[side]) {
        return ContinueChoiceState::Deciding;
    }
    return s_choice[side] == 0 ? ContinueChoiceState::LockedYes
                               : ContinueChoiceState::LockedNo;
}

} // anonymous namespace

namespace Net {

void ContinueFlow_Init() {
    if (s_initialized) return;
    ResetAll("init");
    s_initialized = true;
    LOG_NETPLAY(LOG_INFO, "[ContinueFlow] Initialized (enabled=%d)", s_enabled ? 1 : 0);
}

void ContinueFlow_Shutdown() {
    if (!s_initialized) return;
    ResetAll("shutdown");
    s_initialized = false;
}

void ContinueFlow_Reset(const char* reason) {
    if (!s_initialized) return;
    if (s_state != FlowState::Idle) {
        Rollback::NetplayLog_Write("CONTINUE", -1,
            "Reset from %s: %s", FlowStateName(s_state), reason ? reason : "?");
    }
    ResetAll(reason);
}

bool ContinueFlow_IsEnabled() {
    return s_enabled;
}

void ContinueFlow_SetEnabled(bool enabled) {
    if (s_enabled == enabled) return;
    s_enabled = enabled;
    LOG_NETPLAY(LOG_INFO, "[ContinueFlow] %s", enabled ? "Enabled" : "Disabled");
    if (!enabled && s_initialized) {
        ResetAll("disabled");
    }
}

void ContinueFlow_OnWinScreenAdvance() {
    if (!s_initialized || !s_enabled) return;
    if (s_state != FlowState::Armed) return;
    if (GetGameMode() != MODE_WINSCREEN) return;
    // Both-advance is latched (level-triggered) in WinScreenSync; enter the
    // prompt as soon as the win pose (sub 3) is on screen. The consume path
    // usually beats this — it exists for advance intents latched before the
    // interactive sub (pre-skips during the mode-9 fade-in).
    if (GetSubstate() == STORY_SUB_DIALOGUE_ADV) {
        BeginPrompt("both-advance observed (winscreen sync)");
    }
}

void ContinueFlow_OnConsumedFrame(uint16_t p1Inputs, uint16_t p2Inputs) {
    if (!s_initialized || !s_enabled) return;

    switch (s_state) {
        case FlowState::Idle:
            if (GetGameMode() == MODE_WINSCREEN &&
                MatchLifecycle_IsMatchOwned() &&
                Session_IsConnected()) {
                SetState(FlowState::Armed, "mode 9 entered (owned netplay match)");
            }
            break;

        case FlowState::Armed: {
            if (GetGameMode() != MODE_WINSCREEN) {
                break;  // mode-8 sub-5 tail — keep tracking prev inputs only
            }
            const uint32_t sub = GetSubstate();
            if (sub == STORY_SUB_DIALOGUE_ADV) {
                s_sawWinPose = true;
                if (((uint16_t)(p1Inputs | p2Inputs) & kAdvanceMask) != 0) {
                    // This consumed frame carries the (skip-propagated)
                    // advance — without intervention sub_6019F0 skips the
                    // continue screen for gametype 2. Runs before the words
                    // are injected, so the suppression mask already applies
                    // to this same frame.
                    BeginPrompt("advance in consumed stream at sub 3");
                }
            } else if (sub > STORY_SUB_DIALOGUE_END && s_sawWinPose) {
                // Vanilla 640-frame idle timeout advanced sub 3 without any
                // lockstep input — pull the flow back to the prompt. Entry is
                // per-machine timed here; continuous prev-input tracking means
                // a divergent lock would need a release + re-press inside the
                // few-frame entry skew (not humanly reachable). The
                // s_sawWinPose gate keeps a phase re-armed during the
                // post-decline fade from ever re-forcing the prompt.
                BeginPrompt("sub self-advanced past continue gate");
            }
            break;
        }

        case FlowState::Prompt:
            StepPrompt(p1Inputs, p2Inputs);
            break;

        case FlowState::RematchPending:
            // Lockstep already finalized at resolution; nothing to consume.
            break;

        case FlowState::DeclineCooldown:
            if (!s_declineCarryClear &&
                ((uint16_t)(p1Inputs | p2Inputs) & kCarrySuppressMask) == 0) {
                s_declineCarryClear = true;
                Rollback::NetplayLog_Write("CONTINUE", -1,
                    "Decline carry gate cleared (consume=%u)",
                    WinScreenSync_GetConsumeFrame());
            }
            break;
    }

    // Edge state lives here, never re-derived (mode 9 is lockstep, no rollback).
    s_prevInputs[0] = p1Inputs;
    s_prevInputs[1] = p2Inputs;
}

bool ContinueFlow_IsPromptActive() {
    return s_initialized && s_enabled && s_state == FlowState::Prompt;
}

uint16_t ContinueFlow_GetSuppressMask() {
    if (!s_initialized || !s_enabled) return 0;
    switch (s_state) {
        case FlowState::Prompt:
            return kPromptSuppressMask;
        case FlowState::RematchPending:
            return kCarrySuppressMask;
        case FlowState::DeclineCooldown:
            return s_declineCarryClear ? (uint16_t)0 : kCarrySuppressMask;
        default:
            return 0;
    }
}

bool ContinueFlow_ShouldHoldWinScreenFinalize() {
    if (!s_initialized || !s_enabled) return false;
    switch (s_state) {
        case FlowState::Armed:
        case FlowState::Prompt:
            return true;
        case FlowState::DeclineCooldown:
            // Keep the (masked) injection alive until the held confirm shows
            // a release in both raw streams, then let the normal handoff
            // barrier finalize.
            return !s_declineCarryClear;
        default:
            return false;
    }
}

bool ContinueFlow_IsRematchLatched() {
    return s_initialized && s_rematchLatched;
}

void ContinueFlow_ConsumeRematchLatch() {
    if (!s_rematchLatched) return;
    s_rematchLatched = false;
    Rollback::NetplayLog_Write("CONTINUE", -1,
        "Rematch latch consumed (mode=%u sub=%u)", GetGameMode(), GetSubstate());
    ResetAll("rematch latch consumed");
}

ContinueChoiceState ContinueFlow_GetLocalChoiceState() {
    return SideChoiceState(ResolveLocalSide());
}

ContinueChoiceState ContinueFlow_GetRemoteChoiceState() {
    return SideChoiceState(1 - ResolveLocalSide());
}

} // namespace Net
