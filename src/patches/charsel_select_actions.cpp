#include "patches/charsel_select_actions.h"

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "input_system.h"
#include "net/charsel_sync.h"
#include "net/frontend_input_sync.h"
#include "net/stagesel_sync.h"
#include "patches/charsel_palette_select.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string.h>

namespace {

using namespace Net;

constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_ACTION = 0x81602B;
constexpr uintptr_t ADDR_STAGE_AUX = 0x816028;
constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_CURSOR = 0x81602A;
constexpr uint16_t STAGE_CONFIRM_MASK = INPUT_A | INPUT_C;
constexpr uint8_t RANDOM_CHARACTER_RANGE = 18;
constexpr uint8_t RANDOM_STAGE_RANGE = 24;

struct RandomCursorScrollState {
    bool    active;
    uint8_t target;
    uint8_t steps_remaining;
};

static bool s_installed = false;
static RandomCursorScrollState s_stageRandomScroll = {};

static bool IsStageSelCommittedSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_MATCHUP_COMMIT ||
           substate == CHARSEL_SUB_TO_MATCH;
}

static bool IsOfflineSupportedGameType(uint32_t gameType) {
    switch (gameType) {
        case GAMETYPE_ARCADE:
        case GAMETYPE_VS_CPU:
        case GAMETYPE_VS_HUMAN:
        case GAMETYPE_TRAINING:
            return true;
        default:
            return false;
    }
}

static uint32_t MixDeterministicRandom(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

static uint8_t PickNetplayRandomIndex(uint8_t range,
                                      uint8_t slotOrKind,
                                      uint8_t currentCursor,
                                      uint16_t inputMix) {
    const uint32_t frame = FrontendInputSync_GetConsumeFrame() > 0
        ? (FrontendInputSync_GetConsumeFrame() - 1)
        : 0;
    uint32_t seed = FrontendInputSync_GetEpochId() * 0x9E3779B9u;
    seed ^= (frame + 1u) * 0x85EBCA6Bu;
    seed ^= ((uint32_t)slotOrKind + 3u) * 0xC2B2AE35u;
    seed ^= ((uint32_t)currentCursor + 11u) * 0x27D4EB2Fu;
    seed ^= ((uint32_t)inputMix + 5u) * 0x165667B1u;
    return (uint8_t)(MixDeterministicRandom(seed) % range);
}

static uint8_t PickOfflineRandomIndex(uint8_t range,
                                      uint8_t slotOrKind,
                                      uint8_t currentCursor,
                                      uint16_t inputMix) {
    const uint32_t frame = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    uint32_t seed = frame ^ (GetTickCount() * 0x45D9F3Bu);
    seed ^= ((uint32_t)slotOrKind + 7u) * 0x9E3779B9u;
    seed ^= ((uint32_t)currentCursor + 13u) * 0x85EBCA6Bu;
    seed ^= ((uint32_t)inputMix + 19u) * 0xC2B2AE35u;
    return (uint8_t)(MixDeterministicRandom(seed) % range);
}

static bool CanApplyStageCancel(uint16_t mergedJust, const char* context) {
    if ((mergedJust & INPUT_B) == 0) {
        return false;
    }

    const uint32_t substate = GetSubstate();
    const uint8_t stageConfirmed = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 1);
    const uint8_t committedStage = ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID);
    const bool confirmSameFrame = (mergedJust & STAGE_CONFIRM_MASK) != 0;
    const bool confirmPending = StageSelSync_IsConfirmPending();
    const bool committedSubstate = IsStageSelCommittedSubstate(substate);

    const char* ignoredReason = nullptr;
    if (confirmSameFrame) {
        ignoredReason = "confirm same frame";
    } else if (confirmPending) {
        ignoredReason = "confirm gate pending";
    } else if (substate != CHARSEL_SUB_STAGESEL_GRID) {
        ignoredReason = "not stage grid";
    } else if (stageConfirmed != 0 || committedStage != 0 || committedSubstate) {
        ignoredReason = "stage already confirmed";
    }

    if (ignoredReason) {
        Rollback::NetplayLog_Write(
            "STAGESEL", -1,
            "Stage B cancel ignored: reason=%s context=%s sub=%u confirmed=%u committed=%u confirm_pending=%u merged_just=0x%04X",
            ignoredReason,
            context ? context : "?",
            substate,
            stageConfirmed,
            committedStage,
            confirmPending ? 1 : 0,
            mergedJust);
        return false;
    }

    return true;
}

static bool CanApplyStageRandom(uint16_t mergedJust, const char* context) {
    if ((mergedJust & INPUT_D) == 0) {
        return false;
    }

    const uint32_t substate = GetSubstate();
    const uint8_t stageConfirmed = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 1);
    const uint8_t committedStage = ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID);
    const bool confirmSameFrame = (mergedJust & STAGE_CONFIRM_MASK) != 0;
    const bool cancelSameFrame = (mergedJust & INPUT_B) != 0;
    const bool confirmPending = StageSelSync_IsConfirmPending();
    const bool committedSubstate = IsStageSelCommittedSubstate(substate);

    const char* ignoredReason = nullptr;
    if (confirmSameFrame) {
        ignoredReason = "confirm same frame";
    } else if (cancelSameFrame) {
        ignoredReason = "cancel same frame";
    } else if (confirmPending) {
        ignoredReason = "confirm gate pending";
    } else if (substate != CHARSEL_SUB_STAGESEL_GRID) {
        ignoredReason = "not stage grid";
    } else if (stageConfirmed != 0 || committedStage != 0 || committedSubstate) {
        ignoredReason = "stage already confirmed";
    }

    if (ignoredReason) {
        Rollback::NetplayLog_Write(
            "STAGESEL", -1,
            "Stage D random ignored: reason=%s context=%s sub=%u confirmed=%u committed=%u confirm_pending=%u merged_just=0x%04X",
            ignoredReason,
            context ? context : "?",
            substate,
            stageConfirmed,
            committedStage,
            confirmPending ? 1 : 0,
            mergedJust);
        return false;
    }

    return true;
}

static bool BeginStageRandomScroll(uint8_t targetStage, const char* context) {
    targetStage = (uint8_t)(targetStage % RANDOM_STAGE_RANGE);
    uint8_t currentStage = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
    if (currentStage >= RANDOM_STAGE_RANGE) {
        currentStage = 0;
        WriteMemory<uint8_t>(ADDR_STAGE_CURSOR, currentStage);
    }

    uint8_t steps = (uint8_t)((targetStage + RANDOM_STAGE_RANGE - currentStage) %
        RANDOM_STAGE_RANGE);
    if (steps == 0) {
        steps = RANDOM_STAGE_RANGE;
    }

    s_stageRandomScroll.active = true;
    s_stageRandomScroll.target = targetStage;
    s_stageRandomScroll.steps_remaining = steps;

    Rollback::NetplayLog_Write(
        "STAGESEL", -1,
        "Stage random start: current=%u target=%u steps=%u context=%s epoch=%u frame=%u",
        currentStage,
        targetStage,
        steps,
        context ? context : "?",
        FrontendInputSync_GetEpochId(),
        FrontendInputSync_GetConsumeFrame() > 0 ? (FrontendInputSync_GetConsumeFrame() - 1) : 0);
    return true;
}

static void ForceOfflineStageCancelBackToCharacterSelect(const char* reason) {
    CharSelSelectActions_ResetStageRandomScroll();
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 1, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 2, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_AUX, 0);
    WriteMemory<uint8_t>(ADDR_CHARSEL_CANCEL, 0);
    WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION, 0);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, CHARSEL_SUB_SELECT);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0);
    CharSelPaletteSelect_OnCharSelBegin(false, 0);

    Rollback::NetplayLog_Write(
        "STAGESEL", -1,
        "Offline stage selection canceled back to character select: mode=%u type=%u sub=%u reason=%s",
        GetGameMode(),
        GetGameType(),
        GetSubstate(),
        reason ? reason : "?");
}

} // anonymous namespace

namespace Net {

bool CharSelSelectActions_Install() {
    if (s_installed) {
        return true;
    }
    s_installed = true;
    LOG_NETPLAY(LOG_INFO, "[CharSelSelectActions] Installed character/stage frontend action patch");
    Rollback::NetplayLog_Write(
        "CHARSEL", -1,
        "Character/stage frontend action patch installed");
    return true;
}

uint8_t CharSelSelectActions_HandleNetplayCharacterRandomInput(uint16_t p1Just,
                                                               uint16_t p2Just,
                                                               uint16_t p1Input,
                                                               uint16_t p2Input) {
    if (GetGameMode() != MODE_CHARSEL || !CharSelSync_IsLockstepActive()) {
        return 0;
    }

    uint8_t randomMask = 0;
    const uint16_t inputMix = (uint16_t)(p1Input ^ (uint16_t)(p2Input << 1));

    if ((p1Just & INPUT_D) != 0) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_CHARSEL_P1_CURSOR);
        const uint8_t target = PickNetplayRandomIndex(RANDOM_CHARACTER_RANGE, 0, current, inputMix);
        if (CharSelPaletteSelect_RequestRandomCharacter(0, target, "netplay P1 D")) {
            randomMask |= 0x01;
        }
    }
    if ((p2Just & INPUT_D) != 0) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_CHARSEL_P2_CURSOR);
        const uint8_t target = PickNetplayRandomIndex(RANDOM_CHARACTER_RANGE, 1, current, inputMix);
        if (CharSelPaletteSelect_RequestRandomCharacter(1, target, "netplay P2 D")) {
            randomMask |= 0x02;
        }
    }

    if (randomMask != 0) {
        Rollback::NetplayLog_Write(
            "CHARSEL", -1,
            "Character random applied: slots=0x%02X epoch=%u frame=%u p1=0x%04X p2=0x%04X",
            randomMask,
            FrontendInputSync_GetEpochId(),
            FrontendInputSync_GetConsumeFrame() > 0 ? (FrontendInputSync_GetConsumeFrame() - 1) : 0,
            p1Input,
            p2Input);
    }

    return randomMask;
}

bool CharSelSelectActions_HandleNetplayStageCancelInput(uint16_t mergedJust) {
    if (GetGameMode() != MODE_CHARSEL || !CharSelSync_IsLockstepActive()) {
        return false;
    }
    if (!CanApplyStageCancel(mergedJust, "netplay")) {
        return false;
    }

    CharSelSelectActions_ResetStageRandomScroll();
    return CharSelSync_CancelStageSelectionBackToCharacterSelect("netplay B");
}

bool CharSelSelectActions_HandleOfflineStageCancelInput(uint16_t mergedJust) {
    if (GetGameMode() != MODE_CHARSEL || CharSelSync_IsLockstepActive()) {
        return false;
    }
    if (!IsOfflineSupportedGameType(GetGameType())) {
        if ((mergedJust & INPUT_B) != 0) {
            Rollback::NetplayLog_Write(
                "STAGESEL", -1,
                "Offline stage B cancel ignored: unsupported game type=%u sub=%u",
                GetGameType(),
                GetSubstate());
        }
        return false;
    }
    if (!CanApplyStageCancel(mergedJust, "offline")) {
        return false;
    }

    ForceOfflineStageCancelBackToCharacterSelect("offline B");
    return true;
}

bool CharSelSelectActions_HandleNetplayStageRandomInput(uint16_t mergedJust,
                                                       uint16_t mergedInput) {
    if (GetGameMode() != MODE_CHARSEL || !CharSelSync_IsLockstepActive()) {
        return false;
    }
    if (!CanApplyStageRandom(mergedJust, "netplay")) {
        return false;
    }

    const uint8_t current = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
    const uint8_t target = PickNetplayRandomIndex(RANDOM_STAGE_RANGE, 2, current, mergedInput);
    return BeginStageRandomScroll(target, "netplay D");
}

bool CharSelSelectActions_HandleOfflineStageRandomInput(uint16_t mergedJust,
                                                       uint16_t mergedInput) {
    if (GetGameMode() != MODE_CHARSEL || CharSelSync_IsLockstepActive()) {
        return false;
    }
    if (!IsOfflineSupportedGameType(GetGameType())) {
        if ((mergedJust & INPUT_D) != 0) {
            Rollback::NetplayLog_Write(
                "STAGESEL", -1,
                "Offline stage D random ignored: unsupported game type=%u sub=%u",
                GetGameType(),
                GetSubstate());
        }
        return false;
    }
    if (!CanApplyStageRandom(mergedJust, "offline")) {
        return false;
    }

    const uint8_t current = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
    const uint8_t target = PickOfflineRandomIndex(RANDOM_STAGE_RANGE, 2, current, mergedInput);
    return BeginStageRandomScroll(target, "offline D");
}

bool CharSelSelectActions_AdvanceStageRandomScroll() {
    if (!s_stageRandomScroll.active) {
        return false;
    }

    const uint32_t substate = GetSubstate();
    const uint8_t stageConfirmed = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 1);
    if (GetGameMode() != MODE_CHARSEL ||
        substate != CHARSEL_SUB_STAGESEL_GRID ||
        stageConfirmed != 0 ||
        ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID) != 0) {
        Rollback::NetplayLog_Write(
            "STAGESEL", -1,
            "Stage random aborted: sub=%u confirmed=%u committed=%u target=%u remaining=%u",
            substate,
            stageConfirmed,
            ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID),
            s_stageRandomScroll.target,
            s_stageRandomScroll.steps_remaining);
        CharSelSelectActions_ResetStageRandomScroll();
        return false;
    }

    uint8_t current = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
    if (current >= RANDOM_STAGE_RANGE) {
        current = 0;
    }
    const uint8_t next = (uint8_t)((current + 1) % RANDOM_STAGE_RANGE);
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR, next);
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 1, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION, 0);

    if (s_stageRandomScroll.steps_remaining > 0) {
        s_stageRandomScroll.steps_remaining--;
    }

    Rollback::NetplayLog_Verbose(
        "STAGESEL", -1,
        "Stage random step: cursor=%u target=%u remaining=%u",
        next,
        s_stageRandomScroll.target,
        s_stageRandomScroll.steps_remaining);

    if (s_stageRandomScroll.steps_remaining == 0) {
        Rollback::NetplayLog_Write(
            "STAGESEL", -1,
            "Stage random complete: target=%u",
            s_stageRandomScroll.target);
        CharSelSelectActions_ResetStageRandomScroll();
    }

    return true;
}

bool CharSelSelectActions_IsStageRandomScrollActive() {
    return s_stageRandomScroll.active;
}

void CharSelSelectActions_ResetStageRandomScroll() {
    memset(&s_stageRandomScroll, 0, sizeof(s_stageRandomScroll));
}

} // namespace Net
