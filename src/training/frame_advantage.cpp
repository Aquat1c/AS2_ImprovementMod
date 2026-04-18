#include "training/frame_advantage.h"

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
constexpr size_t kHistoryCapacity = 20;

constexpr float kOverlayPadding = 6.0f;
constexpr float kOverlayY = 54.0f; // Below pause indicator

constexpr ImU32 kOverlayBg = IM_COL32(0, 0, 0, 180);
constexpr ImU32 kNeutralColor = IM_COL32(230, 230, 230, 255);
constexpr ImU32 kPlusColor = IM_COL32(120, 255, 140, 255);
constexpr ImU32 kMinusColor = IM_COL32(255, 150, 120, 255);

enum class InteractionResult : uint8_t {
    Blocked,
    Hit,
    Trade,
};

struct EntitySample {
    uint32_t actionId = 0;
    uint8_t attackState = 0;
    uint8_t hitActive = 0;
    uint8_t blockstun = 0;
    uint8_t hitstunDuration = 0;
    uint16_t knockbackTimer = 0;
};

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
};

struct HistoryEntry {
    uint8_t attacker = 0;
    uint32_t attacker_actionId = 0;
    uint32_t defender_actionAtContact = 0;
    uint32_t simFrame_contact = 0;
    InteractionResult result = InteractionResult::Blocked;
    int32_t frameAdvantage = 0;
};

bool s_initialized = false;
bool s_enabled = true;
bool s_roundResetApplied = false;
bool s_debugLogging = false;

// Throttle per-frame sample logging (only log on change)
uint32_t s_lastLoggedActionId[2] = {};
uint8_t  s_lastLoggedAttackState[2] = {};
uint8_t  s_lastLoggedHitActive[2] = {};
PlayerState s_players[2]{};
PendingAttack s_pending[2]{};
Interaction s_active[2]{};
std::array<HistoryEntry, kHistoryCapacity> s_history{};
size_t s_historyHead = 0;
size_t s_historyCount = 0;

constexpr uintptr_t kEntityBases[2] = {
    ADDR_P1_ENTITY_BASE,
    ADDR_P2_ENTITY_BASE,
};

bool IsActionable(uint32_t actionId) {
    return actionId == 2 || actionId == 4 || actionId == 5 || actionId == 7 || actionId == 22;
}

bool IsBlockstun(uint32_t actionId) {
    return actionId >= 63 && actionId <= 71;
}

bool IsHitstun(uint32_t actionId) {
    return actionId == 72 || actionId == 73 || actionId == 74;
}

bool IsTech(uint32_t actionId) {
    return actionId >= 78 && actionId <= 82;
}

bool IsStunned(uint32_t actionId) {
    return IsBlockstun(actionId) || IsHitstun(actionId);
}

bool IsDefenderLocked(uint32_t actionId) {
    return IsStunned(actionId) || IsTech(actionId) || actionId == 23;
}

// Knockdown/launch states: tech recovery (78-82) or action IDs between
// hitstun and tech that represent knockdown/falling (75-77).
bool IsKnockdownOrLaunch(uint32_t actionId) {
    return (actionId >= 75 && actionId <= 82);
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
    return !IsActionable(sample.actionId) &&
           !IsDefenderLocked(sample.actionId) &&
           HasAttackPayload(sample);
}

const char* SideLabel(uint8_t side) {
    return side == 0 ? "P1" : "P2";
}

const char* ActionCategory(uint32_t actionId) {
    if (IsActionable(actionId)) return "Actionable";
    if (IsBlockstun(actionId)) return "Blockstun";
    if (IsHitstun(actionId)) return "Hitstun";
    if (IsTech(actionId)) return "Tech";
    if (IsKnockdownOrLaunch(actionId)) return "Knockdown";
    return "Other";
}

const char* ResultLabel(InteractionResult r) {
    switch (r) {
        case InteractionResult::Blocked: return "Blocked";
        case InteractionResult::Hit: return "Hit";
        case InteractionResult::Trade: return "Trade";
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
    sample.attackState = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_ATTACK_STATE);
    sample.hitActive = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_HIT_ACTIVE);
    sample.blockstun = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_BLOCKSTUN);
    sample.hitstunDuration = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_HITSTUN_DURATION);
    sample.knockbackTimer = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_KNOCKBACK_TIMER);
    return sample;
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

void UpdatePlayerSamples() {
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex) {
        PlayerState& player = s_players[playerIndex];
        player.curr = ReadEntitySample(kEntityBases[playerIndex]);

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

    const bool attackStateEdge = attacker.prev.attackState == 0 && attacker.curr.attackState == 1;
    const bool attackActionEntered = attacker.curr.actionId != attacker.prev.actionId &&
        IsAttackActionCandidate(attacker.curr);
    const bool attackPayloadAppeared = !HasAttackPayload(attacker.prev) &&
        IsAttackActionCandidate(attacker.curr);
    const bool shouldStartPending = !s_active[attackerIndex].active &&
        (attackStateEdge || attackActionEntered || attackPayloadAppeared) &&
        (!pending.active || pending.attacker_actionId != attacker.curr.actionId);

    if (shouldStartPending) {
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

    if (pending.simFrame_A_recover == kFrameUnset &&
        !IsActionable(attacker.prev.actionId) &&
        IsActionable(attacker.curr.actionId)) {
        pending.simFrame_A_recover = simFrame;
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
        const bool enteredStun = !IsStunned(defender.prev.actionId) && IsStunned(defender.curr.actionId);
        if (!enteredStun) {
            continue;
        }

        if (s_debugLogging) {
            LOG_INFO("[FA] %s ENTERED STUN: frame=%u prev_act=%u(%s) curr_act=%u(%s)",
                     SideLabel((uint8_t)defenderIndex), simFrame,
                     defender.prev.actionId, ActionCategory(defender.prev.actionId),
                     defender.curr.actionId, ActionCategory(defender.curr.actionId));
        }

        const int attackerIndex = 1 - defenderIndex;
        Interaction& existing = s_active[attackerIndex];
        if (existing.active) {
            if (s_debugLogging) {
                LOG_INFO("[FA] %s re-stunned during active interaction (multi-hit), resetting D_recover",
                         SideLabel((uint8_t)defenderIndex));
            }
            existing.simFrame_D_recover = kFrameUnset;
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

    // Skip launches/knockdowns — frame advantage is meaningless.
    if (interaction->defenderLaunched) {
        if (s_debugLogging) {
            LOG_INFO("[FA] %s->%s LAUNCHED — discarding (no frame advantage for knockdown)",
                     SideLabel(interaction->attacker), SideLabel(interaction->defender));
        }
        ClearInteraction(interaction);
        return;
    }

    interaction->frameAdvantage = (int32_t)interaction->simFrame_D_recover - (int32_t)interaction->simFrame_A_recover;

    LOG_INFO("[FA] COMPLETE: %s->%s %s adv=%+d (A_recover=%u D_recover=%u contact=%u actionId=%u)",
             SideLabel(interaction->attacker), SideLabel(interaction->defender),
             ResultLabel(interaction->result),
             interaction->frameAdvantage,
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

    if (interaction.simFrame_A_recover == kFrameUnset &&
        !IsActionable(attacker.prev.actionId) &&
        IsActionable(attacker.curr.actionId)) {
        interaction.simFrame_A_recover = simFrame;
        interaction.lastProgressFrame = simFrame;
        if (s_debugLogging) {
            LOG_INFO("[FA] %s ATTACKER RECOVERED: frame=%u (act: %u->%u)",
                     SideLabel(interaction.attacker), simFrame,
                     attacker.prev.actionId, attacker.curr.actionId);
        }
    }

    // Detect launches: if the defender enters a knockdown/tech state after
    // being hit (not blocked), this is a launch — no meaningful frame advantage.
    if (interaction.result == InteractionResult::Hit &&
        !interaction.defenderLaunched &&
        IsKnockdownOrLaunch(defender.curr.actionId)) {
        interaction.defenderLaunched = true;
        if (s_debugLogging) {
            LOG_INFO("[FA] %s DEFENDER LAUNCHED: frame=%u def_act=%u",
                     SideLabel(interaction.defender), simFrame, defender.curr.actionId);
        }
    }

    if (interaction.simFrame_D_recover != kFrameUnset && IsDefenderLocked(defender.curr.actionId)) {
        interaction.simFrame_D_recover = kFrameUnset;
        interaction.lastProgressFrame = simFrame;
    }

    if (interaction.simFrame_D_recover == kFrameUnset &&
        !IsDefenderLocked(defender.curr.actionId) &&
        !IsActionable(defender.prev.actionId) &&
        IsActionable(defender.curr.actionId)) {
        interaction.simFrame_D_recover = simFrame;
        interaction.lastProgressFrame = simFrame;
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

void FrameAdvantage_ResetState(void) {
    ClearTrackingRuntime(true);
}

void FrameAdvantage_ClearDisplay(void) {
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
    return s_initialized && s_enabled && s_historyCount > 0;
}

void FrameAdvantage_OnFrameAdvanced(uint32_t simFrame) {
    if (!s_initialized || !s_enabled) {
        return;
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

    UpdatePlayerSamples();

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

    if (s_historyCount == 0) {
        return;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) {
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
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // Center horizontally, below the pause indicator
    const float x = (displaySize.x - textSize.x) * 0.5f;
    const float y = kOverlayY;

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
            }
        }
    }
}