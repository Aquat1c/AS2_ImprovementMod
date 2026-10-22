#include "net/netplay_phase_runtime.h"

#include "net/match_lifecycle.h"
#include "net/pregame_sync.h"

#include <string.h>

namespace {

static Net::MatchRollbackPhase MapPregamePhase(Net::PregamePhase phase) {
    switch (phase) {
        case Net::PregamePhase::SyncAnnounce:
        case Net::PregamePhase::SyncExchange:
        case Net::PregamePhase::SyncConfirmed:
        case Net::PregamePhase::FrontendLocked:
        case Net::PregamePhase::ConfigExchange:
        case Net::PregamePhase::ConfigAgreed:
            return Net::MatchRollbackPhase::Bootstrap_Handoff;

        case Net::PregamePhase::FrontendCharSel:
            return Net::MatchRollbackPhase::Lockstep_CharSelect;

        case Net::PregamePhase::FrontendStageSel:
            return Net::MatchRollbackPhase::Lockstep_StageSelect;

        case Net::PregamePhase::BootstrapLoading:
            return Net::MatchRollbackPhase::Bootstrap_Loading;

        case Net::PregamePhase::BootstrapBaseline:
            return Net::MatchRollbackPhase::Bootstrap_Baseline;

        case Net::PregamePhase::BootstrapReady:
            return Net::MatchRollbackPhase::Bootstrap_Handoff;

        default:
            return Net::MatchRollbackPhase::None;
    }
}

static Net::MatchRollbackPhase MapLifecyclePhase(Net::MatchLifecyclePhase phase) {
    switch (phase) {
        case Net::MatchLifecyclePhase::BootstrapWait:
        case Net::MatchLifecyclePhase::LoadingAssets:
        case Net::MatchLifecyclePhase::MatchSetup:
            return Net::MatchRollbackPhase::Bootstrap_Handoff;

        case Net::MatchLifecyclePhase::IntroActive:
            return Net::MatchRollbackPhase::Match_Intro;

        case Net::MatchLifecyclePhase::PlayableGameplay:
            return Net::MatchRollbackPhase::Match_Playable;

        case Net::MatchLifecyclePhase::PauseActive:
            return Net::MatchRollbackPhase::Match_Pause;

        case Net::MatchLifecyclePhase::RoundTransition:
            return Net::MatchRollbackPhase::Match_RoundTransition;

        case Net::MatchLifecyclePhase::MatchInit:
            return Net::MatchRollbackPhase::Match_RoundInit;

        case Net::MatchLifecyclePhase::MatchEnd:
        case Net::MatchLifecyclePhase::WinScreenActive:
            return Net::MatchRollbackPhase::Match_Winscreen;

        case Net::MatchLifecyclePhase::PostMatchRoute:
        case Net::MatchLifecyclePhase::ReturningToCharSel:
        case Net::MatchLifecyclePhase::ReturningToMenu:
        case Net::MatchLifecyclePhase::DisconnectRecovery:
            return Net::MatchRollbackPhase::Match_Exited;

        default:
            return Net::MatchRollbackPhase::None;
    }
}

} // anonymous namespace

namespace Net {

const char* MatchRollbackPhaseName(MatchRollbackPhase phase) {
    switch (phase) {
        case MatchRollbackPhase::None:                  return "None";
        case MatchRollbackPhase::Lockstep_CharSelect:   return "Lockstep_CharSelect";
        case MatchRollbackPhase::Lockstep_StageSelect:  return "Lockstep_StageSelect";
        case MatchRollbackPhase::Bootstrap_Loading:     return "Bootstrap_Loading";
        case MatchRollbackPhase::Bootstrap_Baseline:    return "Bootstrap_Baseline";
        case MatchRollbackPhase::Bootstrap_Handoff:     return "Bootstrap_Handoff";
        case MatchRollbackPhase::Match_Intro:           return "Match_Intro";
        case MatchRollbackPhase::Match_Playable:        return "Match_Playable";
        case MatchRollbackPhase::Match_Pause:           return "Match_Pause";
        case MatchRollbackPhase::Match_RoundTransition: return "Match_RoundTransition";
        case MatchRollbackPhase::Match_RoundInit:       return "Match_RoundInit";
        case MatchRollbackPhase::Match_Winscreen:       return "Match_Winscreen";
        case MatchRollbackPhase::Match_Exited:          return "Match_Exited";
        default:                                        return "Unknown";
    }
}

MatchRollbackPhase NetplayPhaseRuntime_GetPhase() {
    if (PregameSync_IsActive()) {
        const MatchRollbackPhase pregamePhase = MapPregamePhase(PregameSync_GetPhase());
        if (pregamePhase != MatchRollbackPhase::None) {
            return pregamePhase;
        }
    }

    return MapLifecyclePhase(MatchLifecycle_GetPhase());
}

bool NetplayPhaseRuntime_IsRollbackOwnedPhase(MatchRollbackPhase phase) {
    switch (phase) {
        case MatchRollbackPhase::Match_Intro:
        case MatchRollbackPhase::Match_Playable:
        case MatchRollbackPhase::Match_Pause:
        case MatchRollbackPhase::Match_RoundTransition:
        case MatchRollbackPhase::Match_RoundInit:
            return true;
        default:
            return false;
    }
}

bool NetplayPhaseRuntime_IsLockstepPhase(MatchRollbackPhase phase) {
    switch (phase) {
        case MatchRollbackPhase::Lockstep_CharSelect:
        case MatchRollbackPhase::Lockstep_StageSelect:
        case MatchRollbackPhase::Match_Winscreen:
            return true;
        default:
            return false;
    }
}

bool NetplayPhaseRuntime_IsStartupPhase(MatchRollbackPhase phase) {
    switch (phase) {
        case MatchRollbackPhase::Bootstrap_Loading:
        case MatchRollbackPhase::Bootstrap_Baseline:
        case MatchRollbackPhase::Bootstrap_Handoff:
            return true;
        default:
            return false;
    }
}

bool NetplayPhaseRuntime_IsInteractivePacingPhase(MatchRollbackPhase phase) {
    return phase == MatchRollbackPhase::Match_Playable;
}

bool NetplayPhaseRuntime_IsContinuousMatchPhase(MatchRollbackPhase phase) {
    return NetplayPhaseRuntime_IsRollbackOwnedPhase(phase);
}

bool NetplayPhaseRuntime_IsExitedPhase(MatchRollbackPhase phase) {
    return phase == MatchRollbackPhase::Match_Exited ||
           phase == MatchRollbackPhase::None;
}

void NetplayPhaseRuntime_GetSnapshot(NetplayPhaseRuntimeSnapshot* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->phase = NetplayPhaseRuntime_GetPhase();
    out->rollback_owned = NetplayPhaseRuntime_IsRollbackOwnedPhase(out->phase);
    out->lockstep_owned = NetplayPhaseRuntime_IsLockstepPhase(out->phase);
    out->startup_owned = NetplayPhaseRuntime_IsStartupPhase(out->phase);
    out->interactive_pacing = NetplayPhaseRuntime_IsInteractivePacingPhase(out->phase);
}

} // namespace Net