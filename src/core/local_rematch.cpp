#include "core/local_rematch.h"

#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "net/session_manager.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>

namespace LocalRematch {
namespace {

// ============================================================================
// Tuning - mirrors ContinueFlow so the two screens feel identical
// ============================================================================

// Vanilla's own idle before sub 3 self-advances (sub_6019F0: subTimer == 640).
constexpr uint32_t kIdlePromptFrames = 640;
constexpr uint32_t kPromptTimeoutFrames = 3600;

// The win pose is parked below its self-advance point so the native timer
// cannot reach the transition before the prompt does.
constexpr uint32_t kWinPoseHoldCeiling = 600;
constexpr uint32_t kWinPoseHoldFloor = 300;

constexpr uint16_t kAdvanceMask = (uint16_t)(INPUT_A | INPUT_C | INPUT_START);
// Native mode-9 substates, named as ContinueFlow names them.
constexpr uint32_t kSubWinPose = STORY_SUB_DIALOGUE_ADV;   // 3
constexpr uint32_t kSubContinue = STORY_SUB_DIALOGUE_END;  // 4
constexpr uint32_t kSubRematchFade = STORY_SUB_EVENT_SETUP;
constexpr uint32_t kSubDeclineFade = STORY_SUB_PREMATCH;

// Where sub_601BB0 sends the two answers itself. From the decomp: the cursor
// byte false branch writes substate 5 (the rematch fade this flow wants), the
// true branch writes substate 6 and starts BGM 73 (the GAME OVER slide this
// flow avoids by routing to 8 instead).
constexpr uint32_t kSubNativeYes = STORY_SUB_EVENT_SETUP;  // 5
constexpr uint32_t kSubNativeNo  = STORY_SUB_EVENT;        // 6

enum class State : uint8_t {
    Idle,
    Armed,
    Prompt,
    Resolved,
};

const char* StateName(State s) {
    switch (s) {
        case State::Armed:    return "Armed";
        case State::Prompt:   return "Prompt";
        case State::Resolved: return "Resolved";
        case State::Idle:
        default:              return "Idle";
    }
}

// ----------------------------------------------------------------------------
// Persistence
//
// Runtime-only was wrong: this changes what happens at the end of every local
// match, so a player turns it on once and expects it to stay on. Losing it at
// every restart reads as the feature not working at all.
//
// Same file and section the input guard uses. SavePersistentSettings rewrites
// that file wholesale, so it emits this key too - writing it only from here
// would let the next settings save drop it.
// ----------------------------------------------------------------------------

const wchar_t kIniSection[] = L"ModSettings";
const wchar_t kIniKey[] = L"local_rematch";

const wchar_t* IniPath() {
    static wchar_t path[MAX_PATH] = {};
    static bool resolved = false;
    if (resolved) {
        return path;
    }
    resolved = true;

    wchar_t exe[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        wcscpy_s(path, L"as2_rollback_settings.ini");
        return path;
    }

    wchar_t* slash = wcsrchr(exe, L'\\');
    wchar_t* fwd = wcsrchr(exe, L'/');
    if (!slash || (fwd && fwd > slash)) {
        slash = fwd;
    }
    if (slash) {
        slash[1] = L'\0';
    } else {
        exe[0] = L'\0';
    }
    swprintf_s(path, L"%lsas2_rollback_settings.ini", exe);
    return path;
}

// Default on: this was asked for, and off-by-default made the feature look
// absent rather than optional. An explicit 0 in the ini still turns it off.
constexpr bool kDefaultEnabled = true;

bool ReadEnabledFromIni() {
    wchar_t value[32] = {};
    GetPrivateProfileStringW(kIniSection, kIniKey, L"", value,
                             (DWORD)(sizeof(value) / sizeof(value[0])), IniPath());
    if (value[0] == L'\0') {
        return kDefaultEnabled;
    }
    return _wcsicmp(value, L"1") == 0 ||
           _wcsicmp(value, L"true") == 0 ||
           _wcsicmp(value, L"yes") == 0 ||
           _wcsicmp(value, L"on") == 0;
}

void WriteEnabledToIni(bool enabled) {
    WritePrivateProfileStringW(kIniSection, kIniKey, enabled ? L"1" : L"0", IniPath());
}

bool s_initialized = false;
bool s_enabled = false;
State s_state = State::Idle;

uint16_t s_prevInputs[2] = {0, 0};

uint32_t s_armedFrames = 0;
uint32_t s_promptFrames = 0;
bool s_sawWinPose = false;
bool s_rematchLatched = false;

// ============================================================================
// Game state
// ============================================================================

uint32_t GameMode() { return GetGameMode(); }
uint32_t Substate() { return GetSubstate(); }
uint32_t SubstateTimer() { return ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER); }
void WriteSubstate(uint32_t v) { WriteMemory<uint32_t>(ADDR_SUB_STATE, v); }
void WriteSubstateTimer(uint32_t v) { WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, v); }
void WriteMatchPhaseTimer(uint32_t v) { WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, v); }
void WriteContinueCursor(uint8_t v) { WriteMemory<uint8_t>(ADDR_CONTINUE_CURSOR, v); }

// The one gate that keeps this away from netplay. A mod-owned online match runs
// as GAMETYPE_VS_HUMAN too, so the game type alone is not enough - the absence
// of a session is what makes it local.
bool LocalVersusOwned() {
    if (GetGameType() != GAMETYPE_VS_HUMAN) {
        return false;
    }
    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);
    return !snap.active;
}

void SetState(State next, const char* why) {
    if (s_state == next) {
        return;
    }
    LOG_INFO("[LocalRematch] %s -> %s (%s)", StateName(s_state), StateName(next),
             why ? why : "?");
    s_state = next;
}

// sub_6019F0's gametype 2/3 gate skips the continue screen; forcing it is the
// same three writes ContinueFlow uses. Assets and SEs are loaded for every
// mode-9 entry by sub_5FBEE0, so there is no loading to arrange.
void ForceContinueScreen(const char* why) {
    WriteSubstate(kSubContinue);
    WriteSubstateTimer(0u);
    WriteContinueCursor(0u);
    LOG_INFO("[LocalRematch] Forced continue screen (sub=4): %s", why ? why : "?");
}

// Park the pose timer under sub_6019F0's `subTimer == 640` self-advance so the
// prompt decides when the screen moves on, not the native clock.
void HoldWinPose() {
    if (SubstateTimer() > kWinPoseHoldCeiling) {
        WriteSubstateTimer(kWinPoseHoldFloor);
    }
}

void BeginPrompt(const char* why) {
    s_promptFrames = 0;
    ForceContinueScreen(why);
    SetState(State::Prompt, why);
}

void ResolveRematch() {
    // Vanilla's own YES route: sub 5 (25f fade) -> 36 -> mode change. Mode 7
    // re-reads the live charsel globals, so nothing here may touch them.
    WriteMatchPhaseTimer(0u);
    WriteSubstate(kSubRematchFade);
    WriteSubstateTimer(0u);
    s_rematchLatched = true;
    SetState(State::Resolved, "native YES");
    LOG_INFO("[LocalRematch] Rematch latched - routing to prematch intro");
}

void ResolveDecline(const char* why) {
    // Not the vanilla NO: under gametype 2 that runs the 1920-frame GAME OVER
    // slide (sub 6). sub 8 is a plain 25f fade to charsel instead.
    WriteMatchPhaseTimer(0u);
    WriteSubstate(kSubDeclineFade);
    WriteSubstateTimer(0u);
    s_rematchLatched = false;
    SetState(State::Resolved, why);
    LOG_INFO("[LocalRematch] Declined (%s) - routing to character select", why ? why : "?");
}

// The native screen owns the choice.
//
// This used to mirror the cursor and the confirm itself, the way ContinueFlow
// does, and mask the inputs so sub_601BB0 could not act. Offline that was both
// unnecessary and actively wrong. Unnecessary because the lockstep mirroring
// exists to make two machines agree, and there is only one machine here.
// Wrong because the mask never reached the words the handler actually reads,
// so the handler resolved the choice first - and its substate write then
// tripped the "prompt sanity failed" branch, which overwrote a correct YES
// (substate 5) with a decline. Both answers came out the same because the
// answer was being thrown away, not because it was misread.
//
// So: force the screen open, then read the handler's own verdict.
void StepPrompt(uint16_t p1, uint16_t p2) {
    (void)p1;
    (void)p2;

    if (GameMode() != MODE_WINSCREEN) {
        ResolveDecline("left the win screen");
        return;
    }

    const uint32_t sub = Substate();

    if (sub == kSubContinue) {
        // Still on the screen with no answer yet.
        if (++s_promptFrames >= kPromptTimeoutFrames) {
            ResolveDecline("prompt timeout");
        }
        return;
    }

    if (sub == kSubNativeYes) {
        LOG_INFO("[LocalRematch] Native YES (sub=%u)", sub);
        ResolveRematch();
        return;
    }

    if (sub == kSubNativeNo) {
        LOG_INFO("[LocalRematch] Native NO (sub=%u)", sub);
        ResolveDecline("player chose NO");
        return;
    }

    LOG_WARN("[LocalRematch] Unexpected substate %u off the continue screen - declining", sub);
    ResolveDecline("unexpected substate");
}

}  // namespace

// ============================================================================
// Public
// ============================================================================

void Init() {
    s_initialized = true;
    s_enabled = ReadEnabledFromIni();
    Reset("init");
    LOG_INFO("[LocalRematch] Initialized (enabled=%d, from ini)", s_enabled ? 1 : 0);
}

void Shutdown() {
    Reset("shutdown");
    s_initialized = false;
}

void Reset(const char* reason) {
    if (s_state != State::Idle) {
        LOG_INFO("[LocalRematch] Reset (%s)", reason ? reason : "?");
    }
    s_state = State::Idle;
    s_prevInputs[0] = 0;
    s_prevInputs[1] = 0;
    s_armedFrames = 0;
    s_promptFrames = 0;
    s_sawWinPose = false;
    s_rematchLatched = false;
}

bool IsEnabled() { return s_enabled; }

void SetEnabled(bool enabled) {
    if (s_enabled == enabled) {
        return;
    }
    s_enabled = enabled;
    if (!enabled) {
        Reset("disabled");
    }
    // Written immediately rather than at shutdown: a crash or a kill from the
    // task bar would otherwise silently discard the choice.
    WriteEnabledToIni(enabled);
    LOG_INFO("[LocalRematch] %s (persisted)", enabled ? "Enabled" : "Disabled");
}

void FrameUpdate() {
    if (!s_initialized || !s_enabled) {
        return;
    }

    // Leaving mode 9 ends the flow regardless of where it got to. The latch is
    // deliberately kept - mode_ownership consumes it on the way out.
    if (GameMode() != MODE_WINSCREEN) {
        if (s_state != State::Idle) {
            const bool latched = s_rematchLatched;
            Reset("left the win screen");
            s_rematchLatched = latched;
        }
        return;
    }

    if (!LocalVersusOwned()) {
        // A netplay match reaches mode 9 as gametype 2 as well; ContinueFlow
        // owns that one and this must stay out of its way entirely.
        if (s_state != State::Idle) {
            Reset("not a local versus match");
        }
        return;
    }

    const uint16_t p1 = InputSystem_GetInput(0);
    const uint16_t p2 = InputSystem_GetInput(1);

    switch (s_state) {
        case State::Idle:
            s_armedFrames = 0;
            s_sawWinPose = false;
            SetState(State::Armed, "mode 9 entered (local versus)");
            break;

        case State::Armed: {
            const uint32_t sub = Substate();
            if (sub == kSubWinPose) {
                s_sawWinPose = true;
                HoldWinPose();
            }

            // Rising edge, not the raw word: a confirm still held from the
            // match's last hit would otherwise open the prompt on frame one and
            // skip the win pose entirely.
            const uint16_t rising = (uint16_t)(
                ((uint16_t)(p1 & (uint16_t)~s_prevInputs[0])) |
                ((uint16_t)(p2 & (uint16_t)~s_prevInputs[1])));

            if ((rising & kAdvanceMask) != 0) {
                BeginPrompt("advance pressed");
            } else if (++s_armedFrames >= kIdlePromptFrames) {
                BeginPrompt("idle threshold");
            } else if (sub > kSubContinue && s_sawWinPose) {
                // With the pose held and the confirm bits suppressed the native
                // handler cannot move the substate, so reaching here means
                // something else did.
                LOG_WARN("[LocalRematch] Substate escaped the held win pose (sub=%u)", sub);
                BeginPrompt("substate escaped the win pose");
            }
            break;
        }

        case State::Prompt:
            StepPrompt(p1, p2);
            break;

        case State::Resolved:
            break;
    }

    s_prevInputs[0] = p1;
    s_prevInputs[1] = p2;
}

bool IsPromptActive() {
    return s_initialized && s_enabled && s_state == State::Prompt;
}

bool IsRematchLatched() {
    return s_initialized && s_enabled && s_rematchLatched;
}

void ConsumeRematchLatch() {
    s_rematchLatched = false;
}

}  // namespace LocalRematch
