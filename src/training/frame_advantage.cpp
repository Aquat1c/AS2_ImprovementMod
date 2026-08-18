#include "training/frame_advantage.h"
#include "training/native_recovery.h"

#include "training/action_state_classifier.h"
#include "training/practice_tools.h"
#include "core/game_state.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "log_window.h"
#include "imgui.h"

#include <array>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kFrameUnset = UINT32_MAX;
constexpr uint32_t kPendingTimeoutFrames = 180;
constexpr uint32_t kInteractionTimeoutFrames = 300;
constexpr uint32_t kTradeWindowFrames = 1;
constexpr uint32_t kOverlayDisplayFrames = 180;
constexpr uint32_t kGapDisplayFrames = 30;
constexpr uint32_t kGapMaxFrames = 60;
constexpr size_t kHistoryCapacity = 20;

constexpr float kOverlayPadding = 6.0f;
// Overlay anchor offset from bottom of screen (above meter bars).
// Slot 0 = bottom-most (PAUSED), slot 1 = FA, slot 2 = macro.
constexpr float kOverlayBottomOffset = 62.0f;

constexpr ImU32 kOverlayBg = IM_COL32(0, 0, 0, 180);
constexpr ImU32 kNeutralColor = IM_COL32(230, 230, 230, 255);
constexpr ImU32 kPlusColor = IM_COL32(120, 255, 140, 255);
constexpr ImU32 kMinusColor = IM_COL32(255, 150, 120, 255);
constexpr ImU32 kGapColor = IM_COL32(255, 225, 110, 255);

enum class InteractionResult : uint8_t {
    Blocked,
    Hit,
    Trade,
};

using EntitySample = Training::ActionStateSample;

struct PlayerState {
    EntitySample prev{};
    EntitySample curr{};
    bool hasPrev = false;
};

struct PendingAttack {
    bool active = false;
    uint32_t simFrame_attackStart = kFrameUnset;
    uint32_t simFrame_A_recover = kFrameUnset;
    uint32_t attacker_actionId = 0;
    uint32_t simFrame_lastUpdate = kFrameUnset;
    uint32_t simFrame_lastHitActive = kFrameUnset;
    bool hitActiveSeen = false;
};

struct Interaction {
    bool active = false;
    uint8_t attacker = 0;
    uint8_t defender = 1;
    uint32_t simFrame_contact = kFrameUnset;
    uint32_t simFrame_A_recover = kFrameUnset;
    uint32_t simFrame_D_recover = kFrameUnset;
    uint32_t attacker_actionId = 0;
    uint32_t defender_actionAtContact = 0;
    InteractionResult result = InteractionResult::Blocked;
    int32_t frameAdvantage = 0;
    uint32_t lastProgressFrame = kFrameUnset;
    bool defenderLaunched = false;
    bool defenderWasAirLocked = false;
};

struct HistoryEntry {
    uint8_t attacker = 0;
    uint32_t attacker_actionId = 0;
    uint32_t defender_actionAtContact = 0;
    uint32_t simFrame_contact = 0;
    InteractionResult result = InteractionResult::Blocked;
    int32_t frameAdvantage = 0;
};

struct GapDisplay {
    bool active = false;
    uint32_t untilFrame = kFrameUnset;
    uint32_t gapFrames = 0;
    uint8_t attacker = 0;
    uint8_t defender = 1;
};

bool s_initialized = false;
bool s_enabled = true;
bool s_roundResetApplied = false;
bool s_debugLogging = false;
bool s_actionabilityAuditLogging = false;

// Actionability source used for attacker/defender recovery detection. Default to
// the engine-derived state-class rule (every movement/neutral recovery state
// counts, not just the hand-enumerated legacy list); switchable to LegacyActionId
// at runtime for A/B comparison. The audit path always uses Legacy as its
// baseline regardless of this setting.
Training::ActionabilitySource s_actionabilitySource = Training::ActionabilitySource::StateClass;

// Throttle per-frame sample logging (only log on change)
uint32_t s_lastLoggedActionId[2] = {};
uint8_t  s_lastLoggedAttackState[2] = {};
uint8_t  s_lastLoggedHitActive[2] = {};
// Native pre-command recovery, the production source. A tick is recorded the
// first time the engine reports a terminal neutral handoff plus an open
// ordinary route; cancel-window route openings never touch it.
uint32_t s_nativeFreeFrame[2] = { kFrameUnset, kFrameUnset };
uint32_t s_nativeFreeGeneration[2] = {};
Training::NativeRecoveryKind s_nativeKind[2] = {};
bool s_nativeCpuUnsupported[2] = {};
uint32_t s_lastNativeLogFrame[2] = {};
const char* s_lastNativeReason[2] = { "", "" };

uint32_t s_lastAuditActionId[2] = {};
uint8_t  s_lastAuditNative[2] = {};
bool     s_lastAuditMismatch[2] = {};
bool     s_hasLastAudit[2] = {};
PlayerState s_players[2]{};
PendingAttack s_pending[2]{};
Interaction s_active[2]{};
GapDisplay s_gapDisplay{};
uint32_t s_resultDisplayUntilFrame = kFrameUnset;
uint32_t s_lastSimFrame = kFrameUnset;
uint32_t s_lastDefenderFreeFrame[2] = {kFrameUnset, kFrameUnset};
std::array<HistoryEntry, kHistoryCapacity> s_history{};
size_t s_historyHead = 0;
size_t s_historyCount = 0;

constexpr uintptr_t kEntityBases[2] = {
    ADDR_P1_ENTITY_BASE,
    ADDR_P2_ENTITY_BASE,
};

// Always-legacy evaluation — used only as the audit comparison baseline.
Training::ActionabilityResult EvaluateLegacyActionability(const EntitySample& sample,
                                                          Training::ActionableContext context,
                                                          bool defenderWasAirLocked = false) {
    return Training::EvaluateActionability(
        sample,
        context,
        defenderWasAirLocked,
        Training::ActionabilitySource::LegacyActionId);
}

// Recovery/actionability test used by the live FA state machine. Honors the
// runtime-selected source (StateClass by default, Legacy for A/B comparison).
bool IsFree(const EntitySample& sample,
            Training::ActionableContext context,
            bool defenderWasAirLocked = false) {
    return Training::EvaluateActionability(
        sample,
        context,
        defenderWasAirLocked,
        s_actionabilitySource).actionable;
}

bool IsBlockstun(uint32_t actionId) {
    return Training::IsBlockstun(actionId);
}

bool IsDefenderLocked(uint32_t actionId) {
    return Training::IsForcedDefenderLock(actionId);
}

bool IsKnockdownOrLaunch(uint32_t actionId) {
    return Training::IsKnockdownOrLaunch(actionId);
}

bool IsAirLockedAction(uint32_t actionId) {
    return actionId == 70 ||
           actionId == 71 ||
           actionId == 73 ||
           Training::IsKnockdownOrLaunch(actionId);
}

bool HasAttackPayload(const EntitySample& sample) {
    return sample.hitActive != 0xFF ||
           sample.blockstun != 0xFF ||
           sample.hitstunDuration != 0xFF ||
           sample.knockbackTimer != 0xFFFF;
}

bool HasLiveHitWindow(const EntitySample& sample) {
    return sample.hitActive != 0 && sample.hitActive != 0xFF;
}

bool IsAttackActionCandidate(const EntitySample& sample) {
    if (Training::IsForcedDefenderLock(sample.actionId)) {
        return false;
    }

    if (sample.attackState == 1) {
        return true;
    }

    return HasAttackPayload(sample);
}

const char* SideLabel(uint8_t side) {
    return side == 0 ? "P1" : "P2";
}

const char* ActionCategory(uint32_t actionId) {
    return Training::ActionCategory(actionId);
}

const char* ResultLabel(InteractionResult r) {
    switch (r) {
        case InteractionResult::Blocked: return "Blocked";
        case InteractionResult::Hit: return "Hit";
        case InteractionResult::Trade: return "Trade";
    }
    return "?";
}

const char* ActionabilitySourceName(Training::ActionabilitySource src) {
    switch (src) {
        case Training::ActionabilitySource::StateClass:          return "state_class";
        case Training::ActionabilitySource::LegacyActionId:      return "legacy";
        case Training::ActionabilitySource::CandidateNativeFlag: return "native";
        case Training::ActionabilitySource::HybridValidated:     return "hybrid";
    }
    return "?";
}

ImU32 AdvantageColor(int32_t adv) {
    if (adv > 0) return kPlusColor;
    if (adv < 0) return kMinusColor;
    return kNeutralColor;
}

uint32_t FrameDiff(uint32_t a, uint32_t b) {
    return (a >= b) ? (a - b) : (b - a);
}

EntitySample ReadEntitySample(uintptr_t entityBase) {
    EntitySample sample{};
    sample.actionId = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_ACTION_ID);
    sample.actionPhase = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_ACTION_PHASE);
    sample.actionFrame = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_ACTION_FRAME);
    // Route 2 kept under its old field name for the audit log only; it is one
    // route timer, never a scalar "actionable" bit.
    sample.nativeActionableCandidate =
        ReadMemory<uint8_t>(entityBase + ENTITY_OFF_COMMAND_ROUTE_TIMERS + ENTITY_ROUTE_A_STAND_OR_AIR);
    for (int i = 0; i < ENTITY_COMMAND_ROUTE_COUNT; ++i) {
        sample.routeOrBoxFlags[i] =
            ReadMemory<uint8_t>(entityBase + ENTITY_OFF_COMMAND_ROUTE_TIMERS + i);
    }
    sample.attackState = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_ATTACK_STATE);
    sample.hitActive = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_HIT_ACTIVE);
    sample.blockstun = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_BLOCKSTUN);
    sample.hitstunDuration = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_HITSTUN_DURATION);
    sample.knockbackTimer = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_KNOCKBACK_TIMER);
    return sample;
}

void FormatRouteFlags(const EntitySample& sample, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    size_t offset = 0;
    out[0] = '\0';
    for (int i = 0; i < 24 && offset + 3 < outSize; ++i) {
        const int written = snprintf(
            out + offset,
            outSize - offset,
            "%02X%s",
            (unsigned int)sample.routeOrBoxFlags[i],
            (i == 23) ? "" : " ");
        if (written <= 0) {
            break;
        }
        offset += (size_t)written;
    }
}

void AuditActionability(uint32_t simFrame, int playerIndex, const EntitySample& sample) {
    if (!s_actionabilityAuditLogging) {
        return;
    }

    const Training::ActionabilityResult result = EvaluateLegacyActionability(
        sample,
        Training::ActionableContext::AttackerRecovery,
        false);
    const bool mismatch = result.legacyActionable != result.nativeCandidate;
    const bool changed = !s_hasLastAudit[playerIndex] ||
                         s_lastAuditActionId[playerIndex] != sample.actionId ||
                         s_lastAuditNative[playerIndex] != sample.nativeActionableCandidate ||
                         s_lastAuditMismatch[playerIndex] != mismatch;
    if (!changed) {
        return;
    }

    char flags[96];
    FormatRouteFlags(sample, flags, sizeof(flags));
    LOG_INFO("[FAACT] frame=%u %s act=%u(%s) phase=%u aframe=%u legacy=%d native676=%d forced=%d contact=%d atk=%u hit=%u bs=%u hs=%u kb=%u reason=%s flags=%s",
             simFrame,
             SideLabel((uint8_t)playerIndex),
             sample.actionId,
             ActionCategory(sample.actionId),
             (unsigned int)sample.actionPhase,
             (unsigned int)sample.actionFrame,
             result.legacyActionable ? 1 : 0,
             result.nativeCandidate ? 1 : 0,
             result.forcedLocked ? 1 : 0,
             Training::IsContactStartState(sample.actionId) ? 1 : 0,
             (unsigned int)sample.attackState,
             (unsigned int)sample.hitActive,
             (unsigned int)sample.blockstun,
             (unsigned int)sample.hitstunDuration,
             (unsigned int)sample.knockbackTimer,
             mismatch ? "mismatch" : result.reason,
             flags);

    s_lastAuditActionId[playerIndex] = sample.actionId;
    s_lastAuditNative[playerIndex] = sample.nativeActionableCandidate;
    s_lastAuditMismatch[playerIndex] = mismatch;
    s_hasLastAudit[playerIndex] = true;
}

// A fighter's recovery tick is consumed once by whichever tracker needs it, so
// a later interaction cannot reuse a stale timestamp from an earlier one.
uint32_t TakeNativeFreeFrame(int player, uint32_t sinceFrame) {
    if (player < 0 || player > 1) {
        return kFrameUnset;
    }
    const uint32_t frame = s_nativeFreeFrame[player];
    if (frame == kFrameUnset) {
        return kFrameUnset;
    }
    if (sinceFrame != kFrameUnset && frame < sinceFrame) {
        return kFrameUnset;
    }
    return frame;
}

bool NativeRecoveryAvailable(int player) {
    return player >= 0 && player <= 1 && !s_nativeCpuUnsupported[player];
}

void ClearPendingAttack(PendingAttack* pending) {
    if (!pending) {
        return;
    }
    *pending = PendingAttack{};
}

void ClearInteraction(Interaction* interaction) {
    if (!interaction) {
        return;
    }
    *interaction = Interaction{};
}

void ClearGapDisplay() {
    s_gapDisplay = GapDisplay{};
}

void ClearResultDisplay() {
    s_resultDisplayUntilFrame = kFrameUnset;
}

void ClearVisibleOverlayState() {
    ClearGapDisplay();
    ClearResultDisplay();
}

void PublishGapDisplay(uint32_t simFrame, int attackerIndex, int defenderIndex) {
    // Always clear a stale gap — new contact invalidates the previous one.
    // Do NOT clear the FA result here: if the gap is too large or absent, the
    // previous FA result should remain visible until the new interaction completes.
    ClearGapDisplay();

    if (defenderIndex < 0 || defenderIndex >= 2) {
        return;
    }

    const uint32_t defenderFreeFrame = s_lastDefenderFreeFrame[defenderIndex];
    s_lastDefenderFreeFrame[defenderIndex] = kFrameUnset;

    if (defenderFreeFrame == kFrameUnset || simFrame <= defenderFreeFrame) {
        return;
    }

    const uint32_t gapFrames = simFrame - defenderFreeFrame;
    if (gapFrames == 0 || gapFrames > kGapMaxFrames) {
        if (s_debugLogging && gapFrames > 0) {
            LOG_INFO("[FA] GAP ignored: %s->%s gap=%u (max=%u free=%u contact=%u)",
                     SideLabel((uint8_t)attackerIndex),
                     SideLabel((uint8_t)defenderIndex),
                     gapFrames,
                     kGapMaxFrames,
                     defenderFreeFrame,
                     simFrame);
        }
        return;
    }

    // A real gap is about to be shown — clear the FA result so the gap takes the slot.
    ClearResultDisplay();

    s_gapDisplay.active = true;
    s_gapDisplay.untilFrame = simFrame + kGapDisplayFrames;
    s_gapDisplay.gapFrames = gapFrames;
    s_gapDisplay.attacker = (uint8_t)attackerIndex;
    s_gapDisplay.defender = (uint8_t)defenderIndex;

    LOG_INFO("[FA] GAP: %s->%s gap=%u (free=%u contact=%u)",
             SideLabel((uint8_t)attackerIndex),
             SideLabel((uint8_t)defenderIndex),
             gapFrames,
             defenderFreeFrame,
             simFrame);
}

void ClearTrackingRuntime(bool clearHistory) {
    for (PlayerState& player : s_players) {
        player = PlayerState{};
    }
    for (PendingAttack& pending : s_pending) {
        pending = PendingAttack{};
    }
    for (Interaction& interaction : s_active) {
        interaction = Interaction{};
    }
    ClearVisibleOverlayState();
    s_lastSimFrame = kFrameUnset;
    s_lastDefenderFreeFrame[0] = kFrameUnset;
    s_lastDefenderFreeFrame[1] = kFrameUnset;
    memset(s_lastAuditActionId, 0, sizeof(s_lastAuditActionId));
    memset(s_lastAuditNative, 0, sizeof(s_lastAuditNative));
    memset(s_lastAuditMismatch, 0, sizeof(s_lastAuditMismatch));
    memset(s_hasLastAudit, 0, sizeof(s_hasLastAudit));
    if (clearHistory) {
        s_historyHead = 0;
        s_historyCount = 0;
        s_history.fill(HistoryEntry{});
    }
}

void PushHistory(const Interaction& interaction) {
    HistoryEntry& slot = s_history[s_historyHead];
    slot.attacker = interaction.attacker;
    slot.attacker_actionId = interaction.attacker_actionId;
    slot.defender_actionAtContact = interaction.defender_actionAtContact;
    slot.simFrame_contact = interaction.simFrame_contact;
    slot.result = interaction.result;
    slot.frameAdvantage = interaction.frameAdvantage;

    s_historyHead = (s_historyHead + 1) % kHistoryCapacity;
    if (s_historyCount < kHistoryCapacity) {
        s_historyCount++;
    }
}

const HistoryEntry* GetHistoryEntryNewest(size_t newestIndex) {
    if (newestIndex >= s_historyCount) {
        return nullptr;
    }
    const size_t base = (s_historyHead + kHistoryCapacity - 1 - newestIndex) % kHistoryCapacity;
    return &s_history[base];
}

bool HasActiveTracking() {
    for (const PendingAttack& pending : s_pending) {
        if (pending.active) {
            return true;
        }
    }
    for (const Interaction& interaction : s_active) {
        if (interaction.active) {
            return true;
        }
    }
    return false;
}

void UpdatePlayerSamples(uint32_t simFrame) {
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex) {
        PlayerState& player = s_players[playerIndex];
        player.curr = ReadEntitySample(kEntityBases[playerIndex]);
        AuditActionability(simFrame, playerIndex, player.curr);

        // Log on significant state changes (throttled)
        if (s_debugLogging) {
            bool changed = (player.curr.actionId != s_lastLoggedActionId[playerIndex]) ||
                           (player.curr.attackState != s_lastLoggedAttackState[playerIndex]) ||
                           (player.curr.hitActive != s_lastLoggedHitActive[playerIndex]);
            if (changed) {
                LOG_INFO("[FA] %s sample: act=%u(%s) atk=%u hit=%u bs=%u hs=%u kb=%u",
                         SideLabel((uint8_t)playerIndex),
                         player.curr.actionId, ActionCategory(player.curr.actionId),
                         player.curr.attackState, player.curr.hitActive,
                         player.curr.blockstun, player.curr.hitstunDuration,
                         player.curr.knockbackTimer);
                s_lastLoggedActionId[playerIndex] = player.curr.actionId;
                s_lastLoggedAttackState[playerIndex] = player.curr.attackState;
                s_lastLoggedHitActive[playerIndex] = player.curr.hitActive;
            }
        }
    }
}

void PrimePreviousSamples() {
    for (PlayerState& player : s_players) {
        player.prev = player.curr;
        player.hasPrev = true;
    }
}

void FinalizeFrameSamples() {
    for (PlayerState& player : s_players) {
        player.prev = player.curr;
        player.hasPrev = true;
    }
}

void RefreshPendingAttack(uint32_t simFrame, int attackerIndex) {
    const PlayerState& attacker = s_players[attackerIndex];
    PendingAttack& pending = s_pending[attackerIndex];
    const Interaction& active = s_active[attackerIndex];

    const bool attackStateEdge = attacker.prev.attackState == 0 && attacker.curr.attackState == 1;
    const bool attackActionEntered = attacker.curr.actionId != attacker.prev.actionId &&
        IsAttackActionCandidate(attacker.curr);
    const bool attackPayloadAppeared = !HasAttackPayload(attacker.prev) &&
        IsAttackActionCandidate(attacker.curr);
    const bool canQueuePending = !active.active || active.attacker_actionId != attacker.curr.actionId;
    const bool shouldStartPending = canQueuePending &&
        (attackStateEdge || attackActionEntered || attackPayloadAppeared) &&
        (!pending.active || pending.attacker_actionId != attacker.curr.actionId);

    if (shouldStartPending) {
        ClearVisibleOverlayState();
        pending = PendingAttack{};
        pending.active = true;
        pending.simFrame_attackStart = simFrame;
        pending.simFrame_lastUpdate = simFrame;
        pending.attacker_actionId = attacker.curr.actionId;
        if (s_debugLogging) {
            const char* reason = attackActionEntered ? "action-enter" :
                                 (attackPayloadAppeared ? "payload-appear" : "attack-state");
            LOG_INFO("[FA] %s PENDING START: frame=%u actionId=%u reason=%s prev_act=%u prev_atk=%u curr_atk=%u",
                     SideLabel((uint8_t)attackerIndex), simFrame, attacker.curr.actionId,
                     reason, attacker.prev.actionId,
                     attacker.prev.attackState, attacker.curr.attackState);
        }
    }

    if (!pending.active) {
        return;
    }

    pending.simFrame_lastUpdate = simFrame;

    if (IsAttackActionCandidate(attacker.curr)) {
        pending.attacker_actionId = attacker.curr.actionId;
    }

    if (HasLiveHitWindow(attacker.curr)) {
        if (s_debugLogging && !pending.hitActiveSeen) {
            LOG_INFO("[FA] %s HIT_ACTIVE first seen: frame=%u actionId=%u",
                     SideLabel((uint8_t)attackerIndex), simFrame, attacker.curr.actionId);
        }
        pending.hitActiveSeen = true;
        pending.simFrame_lastHitActive = simFrame;
        pending.attacker_actionId = attacker.curr.actionId;
    }

    // Native recovery is authoritative. The legacy edge is only a fallback for
    // the CPU-controlled case, where the same routes select AI actions and a
    // human actionability number would be a fiction.
    const uint32_t nativeAttackerFree =
        NativeRecoveryAvailable(attackerIndex)
            ? TakeNativeFreeFrame(attackerIndex, pending.simFrame_attackStart)
            : kFrameUnset;
    const bool legacyEdge =
        !NativeRecoveryAvailable(attackerIndex) &&
        !IsFree(attacker.prev, Training::ActionableContext::AttackerRecovery) &&
        IsFree(attacker.curr, Training::ActionableContext::AttackerRecovery);

    if (pending.simFrame_A_recover == kFrameUnset &&
        (nativeAttackerFree != kFrameUnset || legacyEdge)) {
        pending.simFrame_A_recover =
            (nativeAttackerFree != kFrameUnset) ? nativeAttackerFree : simFrame;
        if (s_actionabilityAuditLogging) {
            LOG_INFO("[FAREC] frame=%u %s role=pending-attacker act=%u phase=%u aframe=%u source=legacy native676=%u A_recover=%u",
                     simFrame,
                     SideLabel((uint8_t)attackerIndex),
                     attacker.curr.actionId,
                     (unsigned int)attacker.curr.actionPhase,
                     (unsigned int)attacker.curr.actionFrame,
                     (unsigned int)attacker.curr.nativeActionableCandidate,
                     pending.simFrame_A_recover);
        }
        if (s_debugLogging) {
            LOG_INFO("[FA] %s PENDING attacker recovered: frame=%u (prev_act=%u curr_act=%u)",
                     SideLabel((uint8_t)attackerIndex), simFrame,
                     attacker.prev.actionId, attacker.curr.actionId);
        }
    }

    if (simFrame > pending.simFrame_attackStart &&
        (simFrame - pending.simFrame_attackStart) > kPendingTimeoutFrames) {
        if (s_debugLogging) {
            LOG_INFO("[FA] %s PENDING TIMEOUT: started=%u now=%u hitActiveSeen=%d",
                     SideLabel((uint8_t)attackerIndex),
                     pending.simFrame_attackStart, simFrame, pending.hitActiveSeen ? 1 : 0);
        }
        ClearPendingAttack(&pending);
    }
}

void PromotePendingAttackToInteraction(uint32_t simFrame, int attackerIndex, int defenderIndex) {
    PendingAttack& pending = s_pending[attackerIndex];
    Interaction& interaction = s_active[attackerIndex];
    const PlayerState& defender = s_players[defenderIndex];

    if (!pending.active || interaction.active) {
        if (s_debugLogging && pending.active) {
            LOG_INFO("[FA] Promote blocked: pending=%d hitSeen=%d interactionActive=%d",
                     pending.active ? 1 : 0, pending.hitActiveSeen ? 1 : 0,
                     interaction.active ? 1 : 0);
        }
        return;
    }

    interaction = Interaction{};
    interaction.active = true;
    interaction.attacker = (uint8_t)attackerIndex;
    interaction.defender = (uint8_t)defenderIndex;
    interaction.simFrame_contact = simFrame;
    interaction.simFrame_A_recover = pending.simFrame_A_recover;
    interaction.attacker_actionId = pending.attacker_actionId;
    interaction.defender_actionAtContact = defender.curr.actionId;
    interaction.result = IsBlockstun(defender.curr.actionId) ? InteractionResult::Blocked : InteractionResult::Hit;
    interaction.lastProgressFrame = simFrame;
    interaction.defenderWasAirLocked = IsAirLockedAction(defender.curr.actionId);

    if (s_debugLogging) {
        LOG_INFO("[FA] PROMOTED %s->%s: frame=%u actionId=%u def_act=%u result=%s A_recover=%s hitSeen=%d",
                 SideLabel((uint8_t)attackerIndex), SideLabel((uint8_t)defenderIndex),
                 simFrame, pending.attacker_actionId, defender.curr.actionId,
                 ResultLabel(interaction.result),
                 (pending.simFrame_A_recover == kFrameUnset) ? "unset" : "set",
                 pending.hitActiveSeen ? 1 : 0);
        if (!pending.hitActiveSeen) {
            LOG_INFO("[FA] %s->%s promoted from contact edge without hitActive; stun transition is treated as authoritative proof of contact",
                     SideLabel((uint8_t)attackerIndex), SideLabel((uint8_t)defenderIndex));
        }
    }

    ClearPendingAttack(&pending);
}

void ProcessContactEdges(uint32_t simFrame) {
    for (int defenderIndex = 0; defenderIndex < 2; ++defenderIndex) {
        const PlayerState& defender = s_players[defenderIndex];
        const bool enteredContact = !Training::IsContactStartState(defender.prev.actionId) &&
                                    Training::IsContactStartState(defender.curr.actionId);
        if (!enteredContact) {
            continue;
        }

        if (s_debugLogging) {
            LOG_INFO("[FA] %s ENTERED CONTACT: frame=%u prev_act=%u(%s) curr_act=%u(%s)",
                     SideLabel((uint8_t)defenderIndex), simFrame,
                     defender.prev.actionId, ActionCategory(defender.prev.actionId),
                     defender.curr.actionId, ActionCategory(defender.curr.actionId));
        }
        if (s_actionabilityAuditLogging) {
            LOG_INFO("[FAREC] frame=%u %s role=contact act=%u phase=%u aframe=%u source=contact-state native676=%u",
                     simFrame,
                     SideLabel((uint8_t)defenderIndex),
                     defender.curr.actionId,
                     (unsigned int)defender.curr.actionPhase,
                     (unsigned int)defender.curr.actionFrame,
                     (unsigned int)defender.curr.nativeActionableCandidate);
        }

        const int attackerIndex = 1 - defenderIndex;
        Interaction& existing = s_active[attackerIndex];
        PendingAttack& pending = s_pending[attackerIndex];

        // Fallback seed for the gap tracker: if this interaction is being replaced
        // mid-string and its D_recover was somehow not yet recorded (e.g. the
        // recovery frame was consumed by an earlier contact), recover it from the
        // ongoing interaction. The primary record now happens at D_recover
        // detection, so this only fires in the rare already-consumed case.
        if (existing.active &&
            existing.simFrame_D_recover != kFrameUnset &&
            s_lastDefenderFreeFrame[defenderIndex] == kFrameUnset) {
            s_lastDefenderFreeFrame[defenderIndex] = existing.simFrame_D_recover;
        }

        PublishGapDisplay(simFrame, attackerIndex, defenderIndex);

        if (existing.active) {
            if (pending.active && pending.attacker_actionId != existing.attacker_actionId) {
                if (s_debugLogging) {
                    LOG_INFO("[FA] %s->%s new move contact: replacing actionId=%u with actionId=%u at frame %u",
                             SideLabel((uint8_t)attackerIndex),
                             SideLabel((uint8_t)defenderIndex),
                             existing.attacker_actionId,
                             pending.attacker_actionId,
                             simFrame);
                }
                ClearInteraction(&existing);
                PromotePendingAttackToInteraction(simFrame, attackerIndex, defenderIndex);
                continue;
            }

            if (s_debugLogging) {
                LOG_INFO("[FA] %s re-contact during active interaction, refreshing contact frame to %u",
                         SideLabel((uint8_t)defenderIndex),
                         simFrame);
            }
            existing.simFrame_contact = simFrame;
            existing.simFrame_D_recover = kFrameUnset;
            existing.defender_actionAtContact = defender.curr.actionId;
            existing.result = IsBlockstun(defender.curr.actionId) ? InteractionResult::Blocked : InteractionResult::Hit;
            existing.defenderLaunched = false;
            existing.defenderWasAirLocked = existing.defenderWasAirLocked || IsAirLockedAction(defender.curr.actionId);
            existing.lastProgressFrame = simFrame;
            continue;
        }

        PromotePendingAttackToInteraction(simFrame, attackerIndex, defenderIndex);
    }

    if (s_active[0].active && s_active[1].active) {
        const uint32_t diff = FrameDiff(s_active[0].simFrame_contact, s_active[1].simFrame_contact);
        if (diff <= kTradeWindowFrames) {
            s_active[0].result = InteractionResult::Trade;
            s_active[1].result = InteractionResult::Trade;
        }
    }
}

void CompleteInteraction(Interaction* interaction) {
    if (!interaction || !interaction->active) {
        return;
    }

    interaction->frameAdvantage = (int32_t)interaction->simFrame_D_recover - (int32_t)interaction->simFrame_A_recover;
    s_resultDisplayUntilFrame = interaction->simFrame_D_recover + kOverlayDisplayFrames;

    if (interaction->defenderLaunched && s_debugLogging) {
        LOG_INFO("[FA] %s->%s completed after launch/knockdown recovery",
                 SideLabel(interaction->attacker),
                 SideLabel(interaction->defender));
    }

    LOG_INFO("[FA] COMPLETE: %s->%s %s adv=%+d src=%s (A_recover=%u D_recover=%u contact=%u actionId=%u)",
             SideLabel(interaction->attacker), SideLabel(interaction->defender),
             ResultLabel(interaction->result),
             interaction->frameAdvantage,
             ActionabilitySourceName(s_actionabilitySource),
             interaction->simFrame_A_recover, interaction->simFrame_D_recover,
             interaction->simFrame_contact, interaction->attacker_actionId);

    PushHistory(*interaction);
    ClearInteraction(interaction);
}

void AdvanceInteraction(uint32_t simFrame, int attackerIndex) {
    Interaction& interaction = s_active[attackerIndex];
    if (!interaction.active) {
        return;
    }

    const PlayerState& attacker = s_players[interaction.attacker];
    const PlayerState& defender = s_players[interaction.defender];

    // Bounded by the contact frame: a recovery from BEFORE this interaction was
    // promoted has already been carried across in simFrame_A_recover, so
    // anything this tracker still needs to find must be at or after contact.
    const uint32_t nativeAttackerFree =
        NativeRecoveryAvailable(interaction.attacker)
            ? TakeNativeFreeFrame(interaction.attacker, interaction.simFrame_contact)
            : kFrameUnset;
    const bool legacyAttackerEdge =
        !NativeRecoveryAvailable(interaction.attacker) &&
        !IsFree(attacker.prev, Training::ActionableContext::AttackerRecovery) &&
        IsFree(attacker.curr, Training::ActionableContext::AttackerRecovery);

    if (interaction.simFrame_A_recover == kFrameUnset &&
        (nativeAttackerFree != kFrameUnset || legacyAttackerEdge)) {
        interaction.simFrame_A_recover =
            (nativeAttackerFree != kFrameUnset) ? nativeAttackerFree : simFrame;
        interaction.lastProgressFrame = simFrame;
        if (s_actionabilityAuditLogging) {
            LOG_INFO("[FAREC] frame=%u %s role=attacker act=%u phase=%u aframe=%u source=legacy native676=%u A_recover=%u",
                     simFrame,
                     SideLabel(interaction.attacker),
                     attacker.curr.actionId,
                     (unsigned int)attacker.curr.actionPhase,
                     (unsigned int)attacker.curr.actionFrame,
                     (unsigned int)attacker.curr.nativeActionableCandidate,
                     interaction.simFrame_A_recover);
        }
        if (s_debugLogging) {
            LOG_INFO("[FA] %s ATTACKER RECOVERED: frame=%u (act: %u->%u)",
                     SideLabel(interaction.attacker), simFrame,
                     attacker.prev.actionId, attacker.curr.actionId);
        }
    }

    // Track whether the exchange flowed through launch/knockdown recovery.
    // Unlike the old implementation, this no longer discards the result.
    if (interaction.result == InteractionResult::Hit &&
        !interaction.defenderLaunched &&
        IsKnockdownOrLaunch(defender.curr.actionId)) {
        interaction.defenderLaunched = true;
        interaction.defenderWasAirLocked = true;
        if (s_debugLogging) {
            LOG_INFO("[FA] %s DEFENDER ENTERED KNOCKDOWN/WAKEUP: frame=%u def_act=%u(%s) - waiting for true recovery",
                     SideLabel(interaction.defender), simFrame,
                     defender.curr.actionId, ActionCategory(defender.curr.actionId));
        }
    }

    if (interaction.simFrame_D_recover != kFrameUnset && IsDefenderLocked(defender.curr.actionId)) {
        interaction.simFrame_D_recover = kFrameUnset;
        interaction.lastProgressFrame = simFrame;
    }

    const uint32_t nativeDefenderFree =
        NativeRecoveryAvailable(interaction.defender)
            ? TakeNativeFreeFrame(interaction.defender, interaction.simFrame_contact)
            : kFrameUnset;
    const bool legacyDefenderEdge =
        !NativeRecoveryAvailable(interaction.defender) &&
        !IsFree(defender.prev, Training::ActionableContext::DefenderRecovery, interaction.defenderWasAirLocked) &&
        IsFree(defender.curr, Training::ActionableContext::DefenderRecovery, interaction.defenderWasAirLocked);

    if (interaction.simFrame_D_recover == kFrameUnset &&
        !IsDefenderLocked(defender.curr.actionId) &&
        (nativeDefenderFree != kFrameUnset || legacyDefenderEdge)) {
        interaction.simFrame_D_recover =
            (nativeDefenderFree != kFrameUnset) ? nativeDefenderFree : simFrame;
        interaction.lastProgressFrame = simFrame;
        // Record the defender's free frame for gap detection. This must happen on
        // every D_recover — not just mid-string — so the gap between two hits is
        // still measurable when the previous interaction completes (both sides
        // recover) before the next contact. The next contact's PublishGapDisplay
        // consumes this; kGapMaxFrames filters out stale (non-string) values.
        s_lastDefenderFreeFrame[interaction.defender] = simFrame;
        if (s_actionabilityAuditLogging) {
            LOG_INFO("[FAREC] frame=%u %s role=defender act=%u phase=%u aframe=%u source=legacy native676=%u D_recover=%u airLocked=%d",
                     simFrame,
                     SideLabel(interaction.defender),
                     defender.curr.actionId,
                     (unsigned int)defender.curr.actionPhase,
                     (unsigned int)defender.curr.actionFrame,
                     (unsigned int)defender.curr.nativeActionableCandidate,
                     interaction.simFrame_D_recover,
                     interaction.defenderWasAirLocked ? 1 : 0);
        }
        if (s_debugLogging) {
            LOG_INFO("[FA] %s DEFENDER RECOVERED: frame=%u (act: %u->%u)",
                     SideLabel(interaction.defender), simFrame,
                     defender.prev.actionId, defender.curr.actionId);
        }
    }

    if (interaction.simFrame_A_recover != kFrameUnset && interaction.simFrame_D_recover != kFrameUnset) {
        CompleteInteraction(&interaction);
        return;
    }

    if (interaction.lastProgressFrame != kFrameUnset &&
        simFrame > interaction.lastProgressFrame &&
        (simFrame - interaction.lastProgressFrame) > kInteractionTimeoutFrames) {
        if (s_debugLogging) {
            LOG_INFO("[FA] %s->%s INTERACTION TIMEOUT: frame=%u last_progress=%u A_recover=%s D_recover=%s launched=%d",
                     SideLabel(interaction.attacker), SideLabel(interaction.defender),
                     simFrame, interaction.lastProgressFrame,
                     (interaction.simFrame_A_recover == kFrameUnset) ? "unset" : "set",
                     (interaction.simFrame_D_recover == kFrameUnset) ? "unset" : "set",
                     interaction.defenderLaunched ? 1 : 0);
        }
        ClearInteraction(&interaction);
    }
}

} // namespace

void FrameAdvantage_Init(void) {
    s_initialized = true;
    s_enabled = true;
    s_roundResetApplied = false;
    ClearTrackingRuntime(true);
}

void FrameAdvantage_Shutdown(void) {
    ClearTrackingRuntime(true);
    s_roundResetApplied = false;
    s_initialized = false;
}

void FrameAdvantage_OnPreCommandDispatch(int player,
                                         uint32_t simFrame,
                                         const Training::NativeRecoverySample& sample,
                                         const Training::NativeRecoveryResult& result) {
    if (player < 0 || player > 1 || !s_initialized) {
        return;
    }

    s_nativeKind[player] = result.kind;
    s_nativeCpuUnsupported[player] =
        result.kind == Training::NativeRecoveryKind::CpuControlledUnsupported;

    if (result.freeRecovery) {
        s_nativeFreeFrame[player] = simFrame;
        s_nativeFreeGeneration[player]++;
    }

    if (!s_actionabilityAuditLogging) {
        return;
    }

    const bool changed = s_lastNativeReason[player] != result.reason ||
                         s_lastNativeLogFrame[player] + 60 < simFrame;
    if (!changed) {
        return;
    }
    s_lastNativeReason[player] = result.reason;
    s_lastNativeLogFrame[player] = simFrame;

    LOG_INFO("[FAACT] f=%u %s act=%u p1=%u p2=%u target=%u air=%u down=%u cpu=%u restore=%u "
             "r2=%u r3=%u r4=%u r5=%u r6=%u r7=%u free=%d routeOnly=%d kind=%s",
             simFrame,
             SideLabel((uint8_t)player),
             sample.currentAction,
             sample.pendingAction1,
             sample.pendingAction2,
             result.pendingTarget,
             (unsigned int)sample.airborne,
             (unsigned int)sample.downHeld,
             (unsigned int)sample.cpuControlled,
             (unsigned int)sample.trainingRestoreGate,
             (unsigned int)sample.routes[2],
             (unsigned int)sample.routes[3],
             (unsigned int)sample.routes[4],
             (unsigned int)sample.routes[5],
             (unsigned int)sample.routes[6],
             (unsigned int)sample.routes[7],
             result.freeRecovery ? 1 : 0,
             result.routeOpenWithoutFreeHandoff ? 1 : 0,
             Training::NativeRecoveryKindLabel(result.kind));

    if (Training::BothPendingSlotsSet(sample)) {
        LOG_WARN("[FAACT] f=%u %s both pending slots set (p1=%u p2=%u)",
                 simFrame, SideLabel((uint8_t)player),
                 sample.pendingAction1, sample.pendingAction2);
    }
}

void FrameAdvantage_OnPostCommandDispatch(int player,
                                          uint32_t simFrame,
                                          uint32_t pending1,
                                          uint32_t pending2) {
    if (player < 0 || player > 1 || !s_initialized || !s_actionabilityAuditLogging) {
        return;
    }
    if (s_nativeFreeFrame[player] != simFrame) {
        return;
    }
    const uint32_t after = pending1 != 0 ? pending1 : pending2;
    if (!Training::IsTerminalNeutralTarget(after)) {
        LOG_INFO("[FAACT] f=%u %s real input replaced the neutral handoff (now %u)",
                 simFrame, SideLabel((uint8_t)player), after);
    }
}

void FrameAdvantage_ResetState(void) {
    for (int i = 0; i < 2; ++i) {
        s_nativeFreeFrame[i] = kFrameUnset;
        s_nativeFreeGeneration[i] = 0;
        s_nativeKind[i] = Training::NativeRecoveryKind::Locked;
        s_nativeCpuUnsupported[i] = false;
        s_lastNativeLogFrame[i] = 0;
        s_lastNativeReason[i] = "";
    }
    ClearTrackingRuntime(true);
}

void FrameAdvantage_ClearDisplay(void) {
    ClearVisibleOverlayState();
    s_historyHead = 0;
    s_historyCount = 0;
    s_history.fill(HistoryEntry{});
}

void FrameAdvantage_CancelCalculation(void) {
    for (PlayerState& player : s_players) {
        player.hasPrev = false;
    }
    for (PendingAttack& pending : s_pending) {
        pending = PendingAttack{};
    }
    for (Interaction& interaction : s_active) {
        interaction = Interaction{};
    }
    ClearGapDisplay();
    s_lastDefenderFreeFrame[0] = kFrameUnset;
    s_lastDefenderFreeFrame[1] = kFrameUnset;
}

void FrameAdvantage_SetEnabled(bool enabled) {
    s_enabled = enabled;
    if (!enabled) {
        FrameAdvantage_CancelCalculation();
        FrameAdvantage_ClearDisplay();
    }
}

bool FrameAdvantage_IsEnabled(void) {
    return s_enabled;
}

bool FrameAdvantage_HasVisibleOverlay(void) {
    if (!s_initialized || !s_enabled) {
        return false;
    }

    if (s_gapDisplay.active) {
        return true;
    }

    return s_historyCount > 0 &&
           s_resultDisplayUntilFrame != kFrameUnset &&
           s_lastSimFrame != kFrameUnset &&
           s_lastSimFrame < s_resultDisplayUntilFrame;
}

void FrameAdvantage_OnFrameAdvanced(uint32_t simFrame) {
    if (!s_initialized || !s_enabled) {
        return;
    }

    s_lastSimFrame = simFrame;

    if (s_resultDisplayUntilFrame != kFrameUnset && simFrame >= s_resultDisplayUntilFrame) {
        ClearResultDisplay();
    }

    if (s_gapDisplay.active &&
        s_gapDisplay.untilFrame != kFrameUnset &&
        simFrame >= s_gapDisplay.untilFrame) {
        ClearGapDisplay();
    }

    if (!PracticeTools_IsPracticeModeActive()) {
        FrameAdvantage_ResetState();
        s_roundResetApplied = false;
        return;
    }

    if (GetSubstate() == MATCH_SUB_INIT || IsMatchIntroActive()) {
        if (!s_roundResetApplied) {
            FrameAdvantage_ResetState();
            s_roundResetApplied = true;
        }
        return;
    }

    s_roundResetApplied = false;

    if (!IsInActiveGameplay() || IsMatchTransitionActive() || !IsInPlayableMatchGameplay()) {
        return;
    }

    UpdatePlayerSamples(simFrame);

    if (!s_players[0].hasPrev || !s_players[1].hasPrev) {
        PrimePreviousSamples();
        return;
    }

    RefreshPendingAttack(simFrame, 0);
    RefreshPendingAttack(simFrame, 1);
    ProcessContactEdges(simFrame);
    AdvanceInteraction(simFrame, 0);
    AdvanceInteraction(simFrame, 1);
    FinalizeFrameSamples();
}

void FrameAdvantage_RenderOverlay(void) {
    if (!s_initialized || !s_enabled || !PracticeTools_IsPracticeModeActive()) {
        return;
    }

    if (!s_gapDisplay.active &&
        (s_historyCount == 0 ||
         s_resultDisplayUntilFrame == kFrameUnset ||
         s_lastSimFrame == kFrameUnset ||
         s_lastSimFrame >= s_resultDisplayUntilFrame)) {
        return;
    }

    ImDrawList* dl = PracticeTools_ShouldRenderHudBehindMenu()
        ? ImGui::GetBackgroundDrawList()
        : ImGui::GetForegroundDrawList();
    if (!dl) {
        return;
    }

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    if (s_gapDisplay.active) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Gap %u", s_gapDisplay.gapFrames);

        const ImVec2 textSize = ImGui::CalcTextSize(buf);
        const float x = (displaySize.x - textSize.x) * 0.5f;
        const float y = displaySize.y - kOverlayBottomOffset;

        dl->AddRectFilled(
            ImVec2(x - kOverlayPadding, y - 2.0f),
            ImVec2(x + textSize.x + kOverlayPadding, y + textSize.y + 2.0f),
            kOverlayBg, 4.0f);
        dl->AddText(ImVec2(x, y), kGapColor, buf);
        return;
    }

    // Show only the most recent result as "Px +/-Y"
    const HistoryEntry* entry = GetHistoryEntryNewest(0);
    if (!entry) {
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%s %+d", SideLabel(entry->attacker), entry->frameAdvantage);

    const ImU32 textColor = AdvantageColor(entry->frameAdvantage);
    const ImVec2 textSize = ImGui::CalcTextSize(buf);

    // Center horizontally, above the meter bars at bottom of screen
    const float x = (displaySize.x - textSize.x) * 0.5f;
    const float y = displaySize.y - kOverlayBottomOffset;

    dl->AddRectFilled(
        ImVec2(x - kOverlayPadding, y - 2.0f),
        ImVec2(x + textSize.x + kOverlayPadding, y + textSize.y + 2.0f),
        kOverlayBg, 4.0f);
    dl->AddText(ImVec2(x, y), textColor, buf);
}

void FrameAdvantage_RenderImGui(void) {
    bool enabled = s_enabled;
    if (ImGui::Checkbox("Frame Advantage Display", &enabled)) {
        FrameAdvantage_SetEnabled(enabled);
    }

    if (s_enabled) {
        ImGui::SameLine();
        ImGui::Checkbox("Debug Log", &s_debugLogging);
        ImGui::SameLine();
        ImGui::Checkbox("Audit Actionability", &s_actionabilityAuditLogging);

        // Recovery detection source: StateClass (engine-derived, default) vs the
        // legacy hand-enumerated action-ID list. Switchable live for A/B testing.
        // Changing it abandons any in-flight calculation so the next interaction
        // is measured cleanly under the new rule.
        bool useStateClass =
            s_actionabilitySource == Training::ActionabilitySource::StateClass;
        if (ImGui::Checkbox("State-class actionability", &useStateClass)) {
            const Training::ActionabilitySource next = useStateClass
                ? Training::ActionabilitySource::StateClass
                : Training::ActionabilitySource::LegacyActionId;
            if (next != s_actionabilitySource) {
                s_actionabilitySource = next;
                FrameAdvantage_CancelCalculation();
                LOG_INFO("[FA] Actionability source -> %s",
                         useStateClass ? "StateClass" : "LegacyActionId");
            }
        }
    }

    if (s_enabled && s_historyCount > 0) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            FrameAdvantage_ClearDisplay();
        }

        const HistoryEntry* entry = GetHistoryEntryNewest(0);
        if (entry) {
            ImGui::Text("Last: %s %+d (%s)", SideLabel(entry->attacker),
                        entry->frameAdvantage, ResultLabel(entry->result));
        }
    }

    if (s_enabled && s_gapDisplay.active) {
        ImGui::Text("Gap: %u (%s->%s)",
                    s_gapDisplay.gapFrames,
                    SideLabel(s_gapDisplay.attacker),
                    SideLabel(s_gapDisplay.defender));
    }

    // Live tracking state display
    if (s_enabled && s_debugLogging) {
        ImGui::Separator();
        ImGui::TextDisabled("Live FA Tracking State:");

        for (int i = 0; i < 2; i++) {
            const PlayerState& ps = s_players[i];
            if (ps.hasPrev) {
                ImGui::Text("  %s: act=%u(%s) atk=%u hit=%u",
                            SideLabel((uint8_t)i),
                            ps.curr.actionId, ActionCategory(ps.curr.actionId),
                            ps.curr.attackState, ps.curr.hitActive);
            }
        }

        for (int i = 0; i < 2; i++) {
            const PendingAttack& p = s_pending[i];
            if (p.active) {
                ImGui::Text("  %s Pending: act=%u hitSeen=%d start=%u A_rec=%s",
                            SideLabel((uint8_t)i), p.attacker_actionId,
                            p.hitActiveSeen ? 1 : 0, p.simFrame_attackStart,
                            (p.simFrame_A_recover == kFrameUnset) ? "unset" : "set");
            }
        }

        for (int i = 0; i < 2; i++) {
            const Interaction& ia = s_active[i];
            if (ia.active) {
                ImGui::Text("  %s->%s Active: %s contact=%u A_rec=%s D_rec=%s launched=%d",
                            SideLabel(ia.attacker), SideLabel(ia.defender),
                            ResultLabel(ia.result),
                            ia.simFrame_contact,
                            (ia.simFrame_A_recover == kFrameUnset) ? "unset" : "set",
                            (ia.simFrame_D_recover == kFrameUnset) ? "unset" : "set",
                            ia.defenderLaunched ? 1 : 0);
                ImGui::Text("    airLocked=%d", ia.defenderWasAirLocked ? 1 : 0);
            }
        }

        for (int i = 0; i < 2; i++) {
            if (s_lastDefenderFreeFrame[i] != kFrameUnset) {
                ImGui::Text("  %s LastFree=%u", SideLabel((uint8_t)i), s_lastDefenderFreeFrame[i]);
            }
        }

        if (s_gapDisplay.active) {
            ImGui::Text("  ActiveGap: %s->%s gap=%u until=%u",
                        SideLabel(s_gapDisplay.attacker),
                        SideLabel(s_gapDisplay.defender),
                        s_gapDisplay.gapFrames,
                        s_gapDisplay.untilFrame);
        }
    }
}
