#pragma once

#include <stdint.h>

namespace Net {

enum class MatchRollbackPhase : uint8_t {
    None = 0,
    Lockstep_CharSelect,
    Lockstep_StageSelect,
    Bootstrap_Loading,
    Bootstrap_Baseline,
    Bootstrap_Handoff,
    Match_Intro,
    Match_Playable,
    Match_Pause,
    Match_RoundTransition,
    Match_RoundInit,
    Match_Winscreen,
    Match_Exited,
};

const char* MatchRollbackPhaseName(MatchRollbackPhase phase);

MatchRollbackPhase NetplayPhaseRuntime_GetPhase();

bool NetplayPhaseRuntime_IsRollbackOwnedPhase(MatchRollbackPhase phase);
bool NetplayPhaseRuntime_IsLockstepPhase(MatchRollbackPhase phase);
bool NetplayPhaseRuntime_IsStartupPhase(MatchRollbackPhase phase);
bool NetplayPhaseRuntime_IsInteractivePacingPhase(MatchRollbackPhase phase);
bool NetplayPhaseRuntime_IsContinuousMatchPhase(MatchRollbackPhase phase);
bool NetplayPhaseRuntime_IsExitedPhase(MatchRollbackPhase phase);

struct NetplayPhaseRuntimeSnapshot {
    MatchRollbackPhase phase;
    bool rollback_owned;
    bool lockstep_owned;
    bool startup_owned;
    bool interactive_pacing;
};

void NetplayPhaseRuntime_GetSnapshot(NetplayPhaseRuntimeSnapshot* out);

} // namespace Net