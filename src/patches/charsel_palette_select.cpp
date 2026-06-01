#include "patches/charsel_palette_select.h"

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "net/barrier_protocol.h"
#include "net/session_manager.h"
#include "net/netplay_palette_runtime.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include "ui/netplay_hud_style.h"
#include "ui/netplay_hud_vanilla.h"
#include "MinHook.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

constexpr uintptr_t ADDR_CHARSEL_SELECT_PLAYER = GAME_BASE + 0x1BECC0;
constexpr uintptr_t ADDR_CHARSEL_RENDER_HELPER = GAME_BASE + 0x1BF490;
constexpr uintptr_t ADDR_AUDIO_PLAY_WRAPPER = GAME_BASE + 0x1D3410;
constexpr uintptr_t ADDR_RENDER_CREATE_COLOR = GAME_BASE + 0x1D3150;
constexpr uintptr_t ADDR_DRAW_FORMAT_STRING = GAME_BASE + 0x229A20;
constexpr uintptr_t ADDR_CHARSEL_MOVE_SE = 0x816250;
constexpr uintptr_t ADDR_CHARSEL_CONFIRM_SE = 0x816254;
constexpr uintptr_t ADDR_CHARSEL_CANCEL_SE = 0x816258;
constexpr uintptr_t ADDR_CHARSEL_UNLOCK_TABLE = 0x815FE8;
constexpr uint8_t kVisibleVanillaPaletteCount = 8;
constexpr size_t kOptionCapacity = kVisibleVanillaPaletteCount * 2;
constexpr uint8_t kRandomCharacterMaxGridIndex = 17;
constexpr uint8_t kRandomCharacterRange = kRandomCharacterMaxGridIndex + 1;
constexpr int kRawHeldBIndex = 5;
constexpr int kRawHeldDIndex = 7;
constexpr int kRawJustPressedOffset = 28;
constexpr int kRawJustAIndex = kRawJustPressedOffset + 4;
constexpr int kRawJustBIndex = kRawJustPressedOffset + 5;
constexpr int kRawJustCIndex = kRawJustPressedOffset + 6;
constexpr int kRawJustDIndex = kRawJustPressedOffset + 7;

static_assert(kVisibleVanillaPaletteCount <= NETPLAY_PALETTE_BANK_COUNT,
    "Visible palette count must fit inside stored palette banks");

using CharSelSelectPlayer_t = char (__cdecl *)(uint16_t* raw_input,
                                               int local_selection,
                                               int other_selection,
                                               uint8_t* control);
using CharSelRenderHelper_t = int (__cdecl *)();
using RenderCreateColor_t = int (__cdecl *)(unsigned __int8 r,
                                            unsigned __int8 g,
                                            unsigned __int8 b);
using DrawFormatString_t = int (__cdecl *)(int x,
                                           int y,
                                           unsigned int color,
                                           char* fmt,
                                           ...);
using AudioPlayWrapper_t = int (__cdecl *)(int handle);

enum class SlotPhase : uint8_t {
    None = 0,
    Selecting = 1,
    LockedFinal = 2,
};

struct PaletteOption {
    uint8_t base_palette;
    bool    use_custom;
};

struct FrontendSlotState {
    SlotPhase phase;
    uint8_t   character_id;
    uint8_t   display_index;
};

struct MatchSelection {
    bool    valid;
    uint8_t character_id;
    uint8_t base_palette;
    bool    use_custom;
};

struct PaletteSlotCache {
    bool    valid;
    uint8_t base_palette;
    bool    use_custom;
};

struct ExternalCustomHint {
    bool    valid;
    uint8_t character_id;
    uint8_t base_palette;
};

struct RandomCharacterScrollState {
    bool    active;
    uint8_t target_grid_index;
    uint8_t steps_remaining;
};

static CharSelSelectPlayer_t s_originalSelectPlayer = nullptr;
static CharSelRenderHelper_t s_originalRenderHelper = nullptr;
static RenderCreateColor_t s_createColor = reinterpret_cast<RenderCreateColor_t>(ADDR_RENDER_CREATE_COLOR);
static DrawFormatString_t s_drawFormatString = reinterpret_cast<DrawFormatString_t>(ADDR_DRAW_FORMAT_STRING);
static AudioPlayWrapper_t s_audioPlay = reinterpret_cast<AudioPlayWrapper_t>(ADDR_AUDIO_PLAY_WRAPPER);

static bool s_frontendActive = false;
static bool s_frontendNetplay = false;
static bool s_blockOfflineFrontend = false;
static uint8_t s_localGameSlot = 0;
static bool s_catalogReceived[2] = {};
static uint16_t s_catalogMasks[2][256] = {};
static bool s_pendingCatalogReceived[2] = {};
static uint16_t s_pendingCatalogMasks[2][256] = {};
static FrontendSlotState s_slotState[2] = {};
static MatchSelection s_matchSelection[2] = {};
static PaletteSlotCache s_paletteSlotCache[2][256] = {};
static ExternalCustomHint s_externalCustomHint[2] = {};
static RandomCharacterScrollState s_randomScroll[2] = {};

static void RefreshSlotAfterCatalogChange(uint8_t gameSlot);
static void PlayCharSelSound(uintptr_t handleAddress);

static uint8_t ReadU8(uintptr_t address, uint8_t fallback = 0) {
    __try {
        return *(volatile uint8_t*)address;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static uint32_t ReadU32(uintptr_t address, uint32_t fallback = 0) {
    __try {
        return *(volatile uint32_t*)address;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static void WriteU8(uintptr_t address, uint8_t value) {
    __try {
        *(volatile uint8_t*)address = value;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void WriteU32(uintptr_t address, uint32_t value) {
    __try {
        *(volatile uint32_t*)address = value;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
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

static bool IsPaletteFrontendSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_SELECT ||
           substate == CHARSEL_SUB_CONFIRM;
}

static bool IsModNetplayCharSelContext() {
    return GetGameMode() == MODE_CHARSEL && Session_IsConnected();
}

static int GetControlSlot(const uint8_t* control) {
    const uintptr_t controlAddress = reinterpret_cast<uintptr_t>(control);
    if (controlAddress == ADDR_CHARSEL_P1_ENABLE) {
        return 0;
    }
    if (controlAddress == ADDR_CHARSEL_P2_ENABLE) {
        return 1;
    }
    return -1;
}

static void ResetNativeSelectionState(uint8_t gameSlot) {
    const uintptr_t enableAddress = gameSlot == 0 ? ADDR_CHARSEL_P1_ENABLE : ADDR_CHARSEL_P2_ENABLE;
    if ((int8_t)ReadU8(enableAddress, 0xFF) == -1) {
        return;
    }

    WriteU8(enableAddress, 0);
    WriteU8(enableAddress + 2, 0);
    WriteU8(enableAddress + 3, 0);
    WriteU8(enableAddress + 4, 0);
    WriteU8(enableAddress + 5, 0);
    WriteU32(gameSlot == 0 ? ADDR_CHARSEL_P1_CHAR_ID : ADDR_CHARSEL_P2_CHAR_ID, 0xFFFFFFFFu);
    WriteU8(gameSlot == 0 ? ADDR_CHARSEL_P1_PALETTE : ADDR_CHARSEL_P2_PALETTE, 0xFF);
}

static uint8_t LookupCharId(uint8_t gridIndex) {
    if (gridIndex > 20) {
        return 0;
    }
    return (uint8_t)(ReadU32(ADDR_CHARSEL_GRID_TABLE + gridIndex * 4, 0) & 0xFF);
}

static bool IsSelectableCharacter(uint8_t characterId) {
    return ReadU8(ADDR_CHARSEL_UNLOCK_TABLE + characterId, 0) == 1;
}

static bool TryFindGridIndex(uint8_t characterId, uint8_t* outGridIndex) {
    if (!outGridIndex) {
        return false;
    }

    for (uint8_t gridIndex = 0; gridIndex <= 20; ++gridIndex) {
        if (LookupCharId(gridIndex) == characterId) {
            *outGridIndex = gridIndex;
            return true;
        }
    }

    return false;
}

static void ResetFrontendSlotState(FrontendSlotState* state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->phase = SlotPhase::None;
}

static void ResetRandomScroll(uint8_t gameSlot) {
    if (gameSlot < 2) {
        memset(&s_randomScroll[gameSlot], 0, sizeof(s_randomScroll[gameSlot]));
    }
}

static void ClearPaletteSlotCaches() {
    memset(s_paletteSlotCache, 0, sizeof(s_paletteSlotCache));
}

static void DeactivateFrontend() {
    if (s_frontendNetplay && GetGameMode() == MODE_CHARSEL) {
        // Netplay tears down the palette frontend before the game fully leaves
        // charsel. Hold off on offline re-entry until the mode tree changes.
        s_blockOfflineFrontend = true;
    }
    s_frontendActive = false;
    s_frontendNetplay = false;
    memset(s_catalogReceived, 0, sizeof(s_catalogReceived));
    memset(s_catalogMasks, 0, sizeof(s_catalogMasks));
    memset(s_pendingCatalogReceived, 0, sizeof(s_pendingCatalogReceived));
    memset(s_pendingCatalogMasks, 0, sizeof(s_pendingCatalogMasks));
    ResetFrontendSlotState(&s_slotState[0]);
    ResetFrontendSlotState(&s_slotState[1]);
    ResetRandomScroll(0);
    ResetRandomScroll(1);
    memset(s_externalCustomHint, 0, sizeof(s_externalCustomHint));
}

static void BuildLocalCatalog(uint16_t* outMasks) {
    if (!outMasks) {
        return;
    }

    memset(outMasks, 0, sizeof(uint16_t) * 256);
    for (int characterId = 0; characterId < 256; ++characterId) {
        uint16_t mask = 0;
        for (uint8_t basePalette = 0; basePalette < kVisibleVanillaPaletteCount; ++basePalette) {
            if (NetplayPaletteRuntime_HasLocalCustomBankFor((uint8_t)characterId, basePalette)) {
                mask |= (uint16_t)(1u << basePalette);
            }
        }
        outMasks[characterId] = mask;
    }
}

static void SendLocalCatalog() {
    CharSelInputPayload payload{};
    payload.game_slot = s_localGameSlot;
    memcpy(payload.custom_masks,
        s_catalogMasks[s_localGameSlot],
        sizeof(payload.custom_masks));

    BarrierProtocol_SendPacket(PacketType::CharSelInput, &payload, sizeof(payload));
    Rollback::NetplayLog_Write("CHARPAL", -1,
        "Sent palette catalog: slot=P%d",
        s_localGameSlot + 1);
}

static void BeginFrontend(bool netplay, uint8_t localGameSlot) {
    uint16_t localMasks[256] = {};
    BuildLocalCatalog(localMasks);

    s_blockOfflineFrontend = false;
    s_frontendActive = true;
    s_frontendNetplay = netplay;
    s_localGameSlot = localGameSlot;
    memset(s_matchSelection, 0, sizeof(s_matchSelection));
    ResetFrontendSlotState(&s_slotState[0]);
    ResetFrontendSlotState(&s_slotState[1]);
    ResetRandomScroll(0);
    ResetRandomScroll(1);
    memset(s_catalogReceived, 0, sizeof(s_catalogReceived));
    memset(s_catalogMasks, 0, sizeof(s_catalogMasks));
    ResetNativeSelectionState(0);
    ResetNativeSelectionState(1);

    if (netplay) {
        // Drop only the opponent-side cache each charsel entry. Local cache is kept
        // for rematch on the same connection (see ResetNetplaySessionState on disconnect).
        const uint8_t remoteGameSlot = localGameSlot == 0 ? 1 : 0;
        memset(s_paletteSlotCache[remoteGameSlot], 0,
               sizeof(s_paletteSlotCache[remoteGameSlot]));

        for (int gameSlot = 0; gameSlot < 2; ++gameSlot) {
            if (!s_pendingCatalogReceived[gameSlot]) {
                continue;
            }

            memcpy(s_catalogMasks[gameSlot],
                s_pendingCatalogMasks[gameSlot],
                sizeof(s_catalogMasks[gameSlot]));
            s_catalogReceived[gameSlot] = true;
        }

        memcpy(s_catalogMasks[localGameSlot], localMasks, sizeof(localMasks));
        s_catalogReceived[localGameSlot] = true;
        memset(s_pendingCatalogReceived, 0, sizeof(s_pendingCatalogReceived));
        memset(s_pendingCatalogMasks, 0, sizeof(s_pendingCatalogMasks));
        SendLocalCatalog();
        LOG_INFO("[CharSelPalette] Netplay frontend begin local=P%d", localGameSlot + 1);
        return;
    }

    memcpy(s_catalogMasks[0], localMasks, sizeof(localMasks));
    memcpy(s_catalogMasks[1], localMasks, sizeof(localMasks));
    s_catalogReceived[0] = true;
    s_catalogReceived[1] = true;
    LOG_INFO("[CharSelPalette] Offline frontend begin");
}

static void EnsureFrontendState() {
    const uint32_t mode = GetGameMode();
    const uint32_t gameType = GetGameType();
    const uint32_t substate = GetSubstate();

    if (mode != MODE_CHARSEL) {
        s_blockOfflineFrontend = false;
        if (s_frontendActive) {
            DeactivateFrontend();
        }
        return;
    }

    if (s_frontendNetplay) {
        s_frontendActive = true;
        return;
    }

    if (IsModNetplayCharSelContext()) {
        if (!s_frontendNetplay) {
            return;
        }
        s_frontendActive = true;
        return;
    }

    if (s_blockOfflineFrontend) {
        return;
    }

    if (!IsOfflineSupportedGameType(gameType)) {
        if (s_frontendActive && !s_frontendNetplay) {
            DeactivateFrontend();
        }
        return;
    }

    if (!IsPaletteFrontendSubstate(substate)) {
        if (s_frontendActive && !s_frontendNetplay) {
            s_frontendActive = false;
            ResetFrontendSlotState(&s_slotState[0]);
            ResetFrontendSlotState(&s_slotState[1]);
            ResetRandomScroll(0);
            ResetRandomScroll(1);
        }
        return;
    }

    if (!s_frontendActive || s_frontendNetplay) {
        BeginFrontend(false, 0);
    }
}

static int BuildOptions(uint8_t gameSlot,
                        uint8_t characterId,
                        PaletteOption* outOptions,
                        int capacity) {
    if (!outOptions || capacity <= 0) {
        return 0;
    }

    int count = 0;
    for (uint8_t basePalette = 0; basePalette < kVisibleVanillaPaletteCount && count < capacity; ++basePalette) {
        outOptions[count].base_palette = basePalette;
        outOptions[count].use_custom = false;
        ++count;
    }

    uint16_t customMask = s_catalogMasks[gameSlot][characterId];
    const ExternalCustomHint& externalHint = s_externalCustomHint[gameSlot];
    if (externalHint.valid &&
        externalHint.character_id == characterId &&
        externalHint.base_palette < kVisibleVanillaPaletteCount) {
        customMask |= (uint16_t)(1u << externalHint.base_palette);
    }
    for (uint8_t basePalette = 0; basePalette < kVisibleVanillaPaletteCount && count < capacity; ++basePalette) {
        if ((customMask & (uint16_t)(1u << basePalette)) == 0) {
            continue;
        }
        outOptions[count].base_palette = basePalette;
        outOptions[count].use_custom = true;
        ++count;
    }

    return count;
}

static bool TryGetDisplayedOption(uint8_t gameSlot,
                                  const FrontendSlotState& state,
                                  PaletteOption* outOption) {
    if (!outOption || state.phase == SlotPhase::None) {
        return false;
    }

    PaletteOption options[kOptionCapacity] = {};
    const int optionCount = BuildOptions(gameSlot, state.character_id, options, (int)kOptionCapacity);
    if (optionCount <= 0) {
        return false;
    }

    uint8_t optionIndex = state.display_index;
    if (optionIndex >= optionCount) {
        optionIndex = 0;
    }

    *outOption = options[optionIndex];
    return true;
}

static void MaybeRequestPreviewReload(uint8_t gameSlot,
                                      bool hadPrevious,
                                      const PaletteOption* previousOption,
                                      bool hadCurrent,
                                      const PaletteOption* currentOption) {
    if (hadPrevious && !hadCurrent) {
        if (previousOption && previousOption->use_custom) {
            NetplayPaletteRuntime_RequestFrontendReload(gameSlot);
        }
        return;
    }

    if (!hadPrevious && hadCurrent) {
        if (currentOption && currentOption->use_custom) {
            NetplayPaletteRuntime_RequestFrontendReload(gameSlot);
        }
        return;
    }

    if (!hadPrevious || !hadCurrent || !previousOption || !currentOption) {
        return;
    }

    if (previousOption->base_palette != currentOption->base_palette) {
        return;
    }

    if (previousOption->use_custom == currentOption->use_custom) {
        return;
    }

    NetplayPaletteRuntime_RequestFrontendReload(gameSlot);
}

static uint8_t FindOptionIndex(const PaletteOption* options,
                               int count,
                               uint8_t basePalette,
                               bool useCustom) {
    for (int index = 0; index < count; ++index) {
        if (options[index].base_palette == basePalette &&
            options[index].use_custom == useCustom) {
            return (uint8_t)index;
        }
    }
    return 0;
}

static bool TryFindOptionIndex(const PaletteOption* options,
                               int count,
                               uint8_t basePalette,
                               bool useCustom,
                               uint8_t* outIndex) {
    if (!options || !outIndex) {
        return false;
    }

    for (int index = 0; index < count; ++index) {
        if (options[index].base_palette == basePalette &&
            options[index].use_custom == useCustom) {
            *outIndex = (uint8_t)index;
            return true;
        }
    }

    return false;
}

static bool IsSameVanillaPaletteLockedByOtherSlot(uint8_t gameSlot,
                                                  uint8_t characterId,
                                                  const PaletteOption& option) {
    if (!s_frontendNetplay || gameSlot > 1 || option.use_custom) {
        return false;
    }

    const MatchSelection& other = s_matchSelection[gameSlot ^ 1u];
    return other.valid &&
           !other.use_custom &&
           other.character_id == characterId &&
           other.base_palette == option.base_palette;
}

static bool ShouldApplyPaletteCacheForInitialSelection(uint8_t gameSlot) {
    if (gameSlot > 1) {
        return false;
    }
    return !s_frontendNetplay || gameSlot == s_localGameSlot;
}

static void RememberPaletteSelection(uint8_t gameSlot,
                                     uint8_t characterId,
                                     const PaletteOption& option,
                                     const char* reason) {
    if (gameSlot > 1) {
        return;
    }

    PaletteSlotCache& cache = s_paletteSlotCache[gameSlot][characterId];
    cache.valid = true;
    cache.base_palette = option.base_palette;
    cache.use_custom = option.use_custom;

    Rollback::NetplayLog_Write("CHARPAL", -1,
        "Palette slot cache remember: slot=P%d char=%u base=%u custom=%d reason=%s",
        gameSlot + 1,
        characterId,
        option.base_palette,
        option.use_custom ? 1 : 0,
        reason ? reason : "?");
}

static uint8_t FindFirstAllowedOptionIndex(uint8_t gameSlot,
                                           uint8_t characterId,
                                           const PaletteOption* options,
                                           int count) {
    if (!options || count <= 0) {
        return 0;
    }

    for (int index = 0; index < count; ++index) {
        if (!IsSameVanillaPaletteLockedByOtherSlot(gameSlot,
                                                   characterId,
                                                   options[index])) {
            return (uint8_t)index;
        }
    }

    return 0;
}

static bool TryResolveCachedOptionIndex(uint8_t gameSlot,
                                        uint8_t characterId,
                                        const PaletteOption* options,
                                        int count,
                                        uint8_t* outIndex) {
    if (!ShouldApplyPaletteCacheForInitialSelection(gameSlot) || !options || !outIndex || count <= 0) {
        return false;
    }

    const PaletteSlotCache& cache = s_paletteSlotCache[gameSlot][characterId];
    if (!cache.valid) {
        return false;
    }

    uint8_t cachedIndex = 0;
    if (!TryFindOptionIndex(options, count, cache.base_palette, cache.use_custom, &cachedIndex) &&
        !TryFindOptionIndex(options, count, cache.base_palette, false, &cachedIndex)) {
        Rollback::NetplayLog_Write("CHARPAL", -1,
            "Palette slot cache skipped: slot=P%d char=%u base=%u custom=%d reason=option unavailable",
            gameSlot + 1,
            characterId,
            cache.base_palette,
            cache.use_custom ? 1 : 0);
        return false;
    }

    if (IsSameVanillaPaletteLockedByOtherSlot(gameSlot, characterId, options[cachedIndex])) {
        Rollback::NetplayLog_Write("CHARPAL", -1,
            "Palette slot cache skipped: slot=P%d char=%u base=%u custom=%d reason=netplay duplicate vanilla",
            gameSlot + 1,
            characterId,
            options[cachedIndex].base_palette,
            options[cachedIndex].use_custom ? 1 : 0);
        return false;
    }

    *outIndex = cachedIndex;
    Rollback::NetplayLog_Write("CHARPAL", -1,
        "Palette slot cache use: slot=P%d char=%u base=%u custom=%d",
        gameSlot + 1,
        characterId,
        options[cachedIndex].base_palette,
        options[cachedIndex].use_custom ? 1 : 0);
    return true;
}

static uint8_t ResolveInitialOptionIndex(uint8_t gameSlot,
                                         uint8_t characterId,
                                         const PaletteOption* options,
                                         int count) {
    const MatchSelection& selection = s_matchSelection[gameSlot];
    if (selection.valid && selection.character_id == characterId) {
        const uint8_t previousIndex =
            FindOptionIndex(options, count, selection.base_palette, selection.use_custom);
        if (!IsSameVanillaPaletteLockedByOtherSlot(gameSlot,
                                                   characterId,
                                                   options[previousIndex])) {
            return previousIndex;
        }
    }
    uint8_t cachedIndex = 0;
    if (TryResolveCachedOptionIndex(gameSlot, characterId, options, count, &cachedIndex)) {
        return cachedIndex;
    }
    const uint8_t defaultIndex = FindOptionIndex(options, count, 0, false);
    if (!IsSameVanillaPaletteLockedByOtherSlot(gameSlot,
                                               characterId,
                                               options[defaultIndex])) {
        return defaultIndex;
    }
    return FindFirstAllowedOptionIndex(gameSlot, characterId, options, count);
}

static void RefreshSlotAfterCatalogChange(uint8_t gameSlot) {
    if (gameSlot > 1) {
        return;
    }

    FrontendSlotState& slotState = s_slotState[gameSlot];
    if (slotState.phase == SlotPhase::None) {
        return;
    }

    PaletteOption previousOption{};
    const bool hadPreviousOption = TryGetDisplayedOption(gameSlot, slotState, &previousOption);

    PaletteOption options[kOptionCapacity] = {};
    const int optionCount = BuildOptions(gameSlot, slotState.character_id, options, (int)kOptionCapacity);
    if (optionCount <= 0) {
        if (slotState.phase == SlotPhase::LockedFinal) {
            memset(&s_matchSelection[gameSlot], 0, sizeof(s_matchSelection[gameSlot]));
        }
        ResetFrontendSlotState(&slotState);
        MaybeRequestPreviewReload(gameSlot,
            hadPreviousOption,
            hadPreviousOption ? &previousOption : nullptr,
            false,
            nullptr);
        return;
    }

    uint8_t newIndex = slotState.display_index;
    if (hadPreviousOption) {
        if (!TryFindOptionIndex(options,
                optionCount,
                previousOption.base_palette,
                previousOption.use_custom,
                &newIndex) &&
            !TryFindOptionIndex(options,
                optionCount,
                previousOption.base_palette,
                false,
                &newIndex)) {
            newIndex = 0;
        }
    } else if (newIndex >= optionCount) {
        newIndex = 0;
    }

    slotState.display_index = newIndex;
    const PaletteOption currentOption = options[newIndex];

    if (slotState.phase == SlotPhase::LockedFinal) {
        s_matchSelection[gameSlot].valid = true;
        s_matchSelection[gameSlot].character_id = slotState.character_id;
        s_matchSelection[gameSlot].base_palette = currentOption.base_palette;
        s_matchSelection[gameSlot].use_custom = currentOption.use_custom;
        RememberPaletteSelection(gameSlot, slotState.character_id, currentOption, "catalog refresh");
    }

    MaybeRequestPreviewReload(gameSlot,
        hadPreviousOption,
        hadPreviousOption ? &previousOption : nullptr,
        true,
        &currentOption);
}

static void WriteSelectionValues(int localSelection,
                                 uint8_t* control,
                                 uint8_t characterId,
                                 const PaletteOption& option,
                                 bool freezeAge) {
    *(uint32_t*)(localSelection + 176) = characterId;
    *(uint8_t*)(localSelection + 180) = option.base_palette;
    control[2] = 1;
    if (freezeAge) {
        control[3] = 0;
    }
    control[4] = 0;
    control[5] = 0;
}

static void CancelSelection(int localSelection, uint8_t* control, uint8_t gameSlot) {
    *(uint8_t*)(localSelection + 180) = 0xFF;
    control[2] = 0;
    control[3] = 0;
    control[4] = 0;
    control[5] = 0;
    ResetFrontendSlotState(&s_slotState[gameSlot]);
    ResetRandomScroll(gameSlot);
}

static bool RawBJustPressed(const uint16_t* rawInput) {
    return rawInput && rawInput[kRawJustBIndex] != 0;
}

static bool RawDJustPressed(const uint16_t* rawInput) {
    return rawInput && rawInput[kRawJustDIndex] != 0;
}

static bool CanRandomCharacter(uint8_t gameSlot) {
    if (gameSlot > 1 || !s_frontendActive) {
        return false;
    }
    if (s_slotState[gameSlot].phase != SlotPhase::None) {
        return false;
    }

    const uintptr_t enableAddress = gameSlot == 0 ? ADDR_CHARSEL_P1_ENABLE : ADDR_CHARSEL_P2_ENABLE;
    const uint8_t enableValue = ReadU8(enableAddress, 0xFF);
    if ((int8_t)enableValue == -1) {
        return false;
    }
    return ReadU8(enableAddress + 2, 0) == 0;
}

static uint32_t MixRandomTargetSeed(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

static uint8_t ChooseOfflineRandomCharacterTarget(uint8_t gameSlot, uint8_t currentCursor) {
    const uint32_t frame = ReadU32(ADDR_FRAME_COUNTER, 0);
    uint32_t seed = frame ^ (GetTickCount() * 0x45D9F3Bu);
    seed ^= ((uint32_t)gameSlot + 1u) * 0x9E3779B9u;
    seed ^= ((uint32_t)currentCursor + 17u) * 0x85EBCA6Bu;
    return (uint8_t)(MixRandomTargetSeed(seed) % kRandomCharacterRange);
}

static bool BeginRandomCharacterScroll(uint8_t gameSlot,
                                       uint8_t targetGridIndex,
                                       const char* context) {
    if (!CanRandomCharacter(gameSlot)) {
        Rollback::NetplayLog_Write("CHARPAL", -1,
            "Character random ignored: slot=P%d reason=not on character grid context=%s",
            gameSlot + 1,
            context ? context : "?");
        return false;
    }

    if (targetGridIndex > kRandomCharacterMaxGridIndex) {
        targetGridIndex = (uint8_t)(targetGridIndex % kRandomCharacterRange);
    }

    const uintptr_t cursorAddress = gameSlot == 0 ? ADDR_CHARSEL_P1_CURSOR : ADDR_CHARSEL_P2_CURSOR;
    uint8_t currentCursor = ReadU8(cursorAddress, 0);
    if (currentCursor > kRandomCharacterMaxGridIndex) {
        currentCursor = 0;
        WriteU8(cursorAddress, currentCursor);
    }

    uint8_t steps = (uint8_t)((targetGridIndex + kRandomCharacterRange - currentCursor) %
        kRandomCharacterRange);
    if (steps == 0) {
        steps = kRandomCharacterRange;
    }

    RandomCharacterScrollState& random = s_randomScroll[gameSlot];
    random.active = true;
    random.target_grid_index = targetGridIndex;
    random.steps_remaining = steps;

    Rollback::NetplayLog_Write("CHARPAL", -1,
        "Character random start: slot=P%d current=%u target=%u steps=%u context=%s",
        gameSlot + 1,
        currentCursor,
        targetGridIndex,
        steps,
        context ? context : "?");
    PlayCharSelSound(ADDR_CHARSEL_MOVE_SE);
    return true;
}

static bool AdvanceRandomCharacterScroll(uint8_t gameSlot, uint8_t* control) {
    if (gameSlot > 1 || !control) {
        return false;
    }

    RandomCharacterScrollState& random = s_randomScroll[gameSlot];
    if (!random.active) {
        return false;
    }

    if (!CanRandomCharacter(gameSlot)) {
        Rollback::NetplayLog_Write("CHARPAL", -1,
            "Character random aborted: slot=P%d reason=left character grid target=%u remaining=%u",
            gameSlot + 1,
            random.target_grid_index,
            random.steps_remaining);
        ResetRandomScroll(gameSlot);
        return false;
    }

    uint8_t currentCursor = control[1];
    if (currentCursor > kRandomCharacterMaxGridIndex) {
        currentCursor = 0;
    }
    const uint8_t nextCursor = (uint8_t)((currentCursor + 1) % kRandomCharacterRange);
    control[1] = nextCursor;
    if (random.steps_remaining > 0) {
        random.steps_remaining--;
    }

    Rollback::NetplayLog_Verbose("CHARPAL", -1,
        "Character random step: slot=P%d cursor=%u target=%u remaining=%u",
        gameSlot + 1,
        nextCursor,
        random.target_grid_index,
        random.steps_remaining);

    if (random.steps_remaining == 0) {
        Rollback::NetplayLog_Write("CHARPAL", -1,
            "Character random complete: slot=P%d target=%u",
            gameSlot + 1,
            random.target_grid_index);
        ResetRandomScroll(gameSlot);
    }

    return true;
}

static bool CancelSlotSelection(uint8_t gameSlot, const char* reason) {
    if (gameSlot > 1 || !s_frontendActive) {
        return false;
    }

    FrontendSlotState& slotState = s_slotState[gameSlot];
    if (slotState.phase == SlotPhase::None && !s_matchSelection[gameSlot].valid) {
        return false;
    }

    PaletteOption previousOption{};
    bool hadPreviousOption = TryGetDisplayedOption(gameSlot, slotState, &previousOption);
    if (!hadPreviousOption && s_matchSelection[gameSlot].valid) {
        previousOption.base_palette = s_matchSelection[gameSlot].base_palette;
        previousOption.use_custom = s_matchSelection[gameSlot].use_custom;
        hadPreviousOption = true;
    }

    memset(&s_matchSelection[gameSlot], 0, sizeof(s_matchSelection[gameSlot]));
    memset(&s_externalCustomHint[gameSlot], 0, sizeof(s_externalCustomHint[gameSlot]));
    ResetNativeSelectionState(gameSlot);
    ResetFrontendSlotState(&slotState);
    ResetRandomScroll(gameSlot);
    MaybeRequestPreviewReload(gameSlot,
        hadPreviousOption,
        hadPreviousOption ? &previousOption : nullptr,
        false,
        nullptr);

    Rollback::NetplayLog_Write("CHARPAL", -1,
        "Palette selection canceled: slot=P%d reason=%s",
        gameSlot + 1,
        reason ? reason : "?");
    return true;
}

static void PlayCharSelSound(uintptr_t handleAddress) {
    const uint32_t handle = ReadU32(handleAddress, 0);
    if (handle != 0) {
        s_audioPlay((int)handle);
    }
}

static bool AnyConfirmAttackJustPressed(const uint16_t* rawInput) {
    return rawInput[kRawJustAIndex] || rawInput[kRawJustCIndex];
}

static char CallOriginalWithFrontendButtonsStripped(CharSelSelectPlayer_t original,
                                                    uint16_t* rawInput,
                                                    int localSelection,
                                                    int otherSelection,
                                                    uint8_t* control) {
    if (!original || !rawInput) {
        return 0;
    }

    const uint16_t heldB = rawInput[kRawHeldBIndex];
    const uint16_t justB = rawInput[kRawJustBIndex];
    const uint16_t heldD = rawInput[kRawHeldDIndex];
    const uint16_t justD = rawInput[kRawJustDIndex];
    rawInput[kRawHeldBIndex] = 0;
    rawInput[kRawJustBIndex] = 0;
    rawInput[kRawHeldDIndex] = 0;
    rawInput[kRawJustDIndex] = 0;
    const char result = original(rawInput, localSelection, otherSelection, control);
    rawInput[kRawHeldBIndex] = heldB;
    rawInput[kRawJustBIndex] = justB;
    rawInput[kRawHeldDIndex] = heldD;
    rawInput[kRawJustDIndex] = justD;
    return result;
}

static void DrawTextShadowed(int x,
                             int y,
                             uint8_t r,
                             uint8_t g,
                             uint8_t b,
                             const char* text) {
    if (!text || !text[0]) {
        return;
    }

    const unsigned int shadowColor = (unsigned int)s_createColor(0, 0, 0);
    const unsigned int textColor = (unsigned int)s_createColor(r, g, b);
    s_drawFormatString(x + 1, y + 1, shadowColor, (char*)"%s", (char*)text);
    s_drawFormatString(x, y, textColor, (char*)"%s", (char*)text);
}

static void DrawSlotOverlay(uint8_t gameSlot) {
    const FrontendSlotState& state = s_slotState[gameSlot];
    if (state.phase == SlotPhase::None) {
        return;
    }

    const uint8_t enableValue = ReadU8(gameSlot == 0 ? ADDR_CHARSEL_P1_ENABLE : ADDR_CHARSEL_P2_ENABLE, 0xFF);
    if ((int8_t)enableValue == -1) {
        return;
    }

    PaletteOption options[kOptionCapacity] = {};
    const int optionCount = BuildOptions(gameSlot, state.character_id, options, (int)kOptionCapacity);
    if (optionCount <= 0) {
        return;
    }

    uint8_t optionIndex = state.display_index;
    if (optionIndex >= optionCount) {
        optionIndex = 0;
    }
    const PaletteOption& option = options[optionIndex];

    char label[64] = {};
    if (state.phase == SlotPhase::Selecting) {
        _snprintf_s(label,
            sizeof(label),
            _TRUNCATE,
            option.use_custom ? "< CUSTOM %u >" : "< COLOR %u >",
            (unsigned)(option.base_palette + 1));
    } else {
        _snprintf_s(label,
            sizeof(label),
            _TRUNCATE,
            option.use_custom ? "CUSTOM %u" : "COLOR %u",
            (unsigned)(option.base_palette + 1));
    }

    const int x = gameSlot == 0 ? 12 : 396;
    const int y = 410;
    const uint8_t r = option.use_custom ? 255 : 240;
    const uint8_t g = option.use_custom ? 220 : 240;
    const uint8_t b = option.use_custom ? 96 : 255;
    DrawTextShadowed(x, y, r, g, b, label);
}

static char __cdecl Hook_CharSelSelectPlayer(uint16_t* rawInput,
                                             int localSelection,
                                             int otherSelection,
                                             uint8_t* control) {
    EnsureFrontendState();

    const int gameSlot = GetControlSlot(control);
    if (!s_frontendActive || !rawInput || !control || gameSlot < 0) {
        return s_originalSelectPlayer(rawInput, localSelection, otherSelection, control);
    }

    if ((int8_t)control[0] == -1) {
        ResetFrontendSlotState(&s_slotState[gameSlot]);
        ResetRandomScroll((uint8_t)gameSlot);
        return s_originalSelectPlayer(rawInput, localSelection, otherSelection, control);
    }

    FrontendSlotState& slotState = s_slotState[gameSlot];

    if (AdvanceRandomCharacterScroll((uint8_t)gameSlot, control)) {
        return 0;
    }

    if (slotState.phase == SlotPhase::LockedFinal && control[2] == 0) {
        PaletteOption previousOption{};
        const bool hadPreviousOption = TryGetDisplayedOption((uint8_t)gameSlot, slotState, &previousOption);
        memset(&s_matchSelection[gameSlot], 0, sizeof(s_matchSelection[gameSlot]));
        ResetFrontendSlotState(&slotState);
        ResetRandomScroll((uint8_t)gameSlot);
        MaybeRequestPreviewReload((uint8_t)gameSlot,
            hadPreviousOption,
            &previousOption,
            false,
            nullptr);
    }

    if (slotState.phase == SlotPhase::LockedFinal) {
        if (RawBJustPressed(rawInput)) {
            if (CancelSlotSelection((uint8_t)gameSlot, "locked-final B")) {
                PlayCharSelSound(ADDR_CHARSEL_CANCEL_SE);
            }
            return 0;
        }
        if (RawDJustPressed(rawInput)) {
            Rollback::NetplayLog_Write("CHARPAL", -1,
                "D ignored during locked character selection: slot=P%d",
                gameSlot + 1);
            return 0;
        }

        PaletteOption options[kOptionCapacity] = {};
        const int optionCount = BuildOptions((uint8_t)gameSlot,
            slotState.character_id,
            options,
            (int)kOptionCapacity);
        if (optionCount <= 0) {
            return 0;
        }

        uint8_t optionIndex = slotState.display_index;
        if (optionIndex >= optionCount) {
            optionIndex = 0;
            slotState.display_index = 0;
        }

        WriteSelectionValues(localSelection,
            control,
            slotState.character_id,
            options[optionIndex],
            false);
        return 0;
    }

    if (slotState.phase == SlotPhase::Selecting) {
        PaletteOption options[kOptionCapacity] = {};
        const int optionCount = BuildOptions((uint8_t)gameSlot,
            slotState.character_id,
            options,
            (int)kOptionCapacity);
        if (optionCount <= 0) {
            CancelSelection(localSelection, control, (uint8_t)gameSlot);
            return 0;
        }

        if (slotState.display_index >= optionCount) {
            slotState.display_index = 0;
        }

        if (RawBJustPressed(rawInput)) {
            CancelSlotSelection((uint8_t)gameSlot, "palette-select B");
            PlayCharSelSound(ADDR_CHARSEL_CANCEL_SE);
            return 0;
        }
        if (RawDJustPressed(rawInput)) {
            Rollback::NetplayLog_Write("CHARPAL", -1,
                "D ignored during palette selection: slot=P%d",
                gameSlot + 1);
            return 0;
        }

        if (rawInput[30]) {
            const PaletteOption previousOption = options[slotState.display_index];
            slotState.display_index = (uint8_t)((slotState.display_index + optionCount - 1) % optionCount);
            const PaletteOption currentOption = options[slotState.display_index];
            MaybeRequestPreviewReload((uint8_t)gameSlot,
                true,
                &previousOption,
                true,
                &currentOption);
            PlayCharSelSound(ADDR_CHARSEL_MOVE_SE);
        } else if (rawInput[31]) {
            const PaletteOption previousOption = options[slotState.display_index];
            slotState.display_index = (uint8_t)((slotState.display_index + 1) % optionCount);
            const PaletteOption currentOption = options[slotState.display_index];
            MaybeRequestPreviewReload((uint8_t)gameSlot,
                true,
                &previousOption,
                true,
                &currentOption);
            PlayCharSelSound(ADDR_CHARSEL_MOVE_SE);
        } else if (AnyConfirmAttackJustPressed(rawInput)) {
            const PaletteOption& option = options[slotState.display_index];
            if (IsSameVanillaPaletteLockedByOtherSlot((uint8_t)gameSlot,
                                                      slotState.character_id,
                                                      option)) {
                PlayCharSelSound(ADDR_CHARSEL_CANCEL_SE);
                Rollback::NetplayLog_Write("CHARPAL", -1,
                    "Palette lock rejected: slot=P%d char=%u base=%u reason=other slot already locked same vanilla palette",
                    gameSlot + 1,
                    slotState.character_id,
                    option.base_palette);
                return 0;
            }
            WriteSelectionValues(localSelection,
                control,
                slotState.character_id,
                option,
                false);
            slotState.phase = SlotPhase::LockedFinal;
            s_matchSelection[gameSlot].valid = true;
            s_matchSelection[gameSlot].character_id = slotState.character_id;
            s_matchSelection[gameSlot].base_palette = option.base_palette;
            s_matchSelection[gameSlot].use_custom = option.use_custom;
            RememberPaletteSelection((uint8_t)gameSlot, slotState.character_id, option, "local lock");
            PlayCharSelSound(ADDR_CHARSEL_CONFIRM_SE);
            Rollback::NetplayLog_Write("CHARPAL", -1,
                "Palette locked: slot=P%d char=%u base=%u custom=%d",
                gameSlot + 1,
                slotState.character_id,
                option.base_palette,
                option.use_custom ? 1 : 0);
            return 0;
        }

        WriteSelectionValues(localSelection,
            control,
            slotState.character_id,
            options[slotState.display_index],
            true);
        return 0;
    }

    if (RawBJustPressed(rawInput)) {
        Rollback::NetplayLog_Write("CHARPAL", -1,
            "B ignored on character grid: slot=P%d reason=no character selected",
            gameSlot + 1);
        return CallOriginalWithFrontendButtonsStripped(s_originalSelectPlayer,
            rawInput,
            localSelection,
            otherSelection,
            control);
    }

    if (RawDJustPressed(rawInput)) {
        const uint8_t currentCursor = control[1] <= kRandomCharacterMaxGridIndex ? control[1] : 0;
        const uint8_t target = ChooseOfflineRandomCharacterTarget((uint8_t)gameSlot, currentCursor);
        BeginRandomCharacterScroll((uint8_t)gameSlot, target, "offline D");
        return 0;
    }

    if (!AnyConfirmAttackJustPressed(rawInput)) {
        return CallOriginalWithFrontendButtonsStripped(s_originalSelectPlayer,
            rawInput,
            localSelection,
            otherSelection,
            control);
    }

    const uint8_t characterId = LookupCharId(control[1]);
    if (!IsSelectableCharacter(characterId)) {
        return CallOriginalWithFrontendButtonsStripped(s_originalSelectPlayer,
            rawInput,
            localSelection,
            otherSelection,
            control);
    }

    PaletteOption options[kOptionCapacity] = {};
    const int optionCount = BuildOptions((uint8_t)gameSlot,
        characterId,
        options,
        (int)kOptionCapacity);
    if (optionCount <= 0) {
        return CallOriginalWithFrontendButtonsStripped(s_originalSelectPlayer,
            rawInput,
            localSelection,
            otherSelection,
            control);
    }

    slotState.phase = SlotPhase::Selecting;
    slotState.character_id = characterId;
    slotState.display_index = ResolveInitialOptionIndex((uint8_t)gameSlot,
        characterId,
        options,
        optionCount);
    const PaletteOption currentOption = options[slotState.display_index];

    WriteSelectionValues(localSelection,
        control,
        characterId,
        currentOption,
        true);
    MaybeRequestPreviewReload((uint8_t)gameSlot,
        false,
        nullptr,
        true,
        &currentOption);
    PlayCharSelSound(ADDR_CHARSEL_CONFIRM_SE);
    Rollback::NetplayLog_Write("CHARPAL", -1,
        "Palette phase enter: slot=P%d char=%u options=%d",
        gameSlot + 1,
        characterId,
        optionCount);
    return 0;
}

static int __cdecl Hook_CharSelRenderHelper() {
    const int result = s_originalRenderHelper();
    EnsureFrontendState();
    if (!s_frontendActive) {
        return result;
    }

    DrawSlotOverlay(0);
    DrawSlotOverlay(1);
    if (NetplayHudStyle::GetRenderMode() == NetplayHudStyle::HudRenderMode::Vanilla) {
        NetplayHudVanilla_RenderCharSel();
    }
    return result;
}

} // namespace

namespace Net {

bool CharSelPaletteSelect_Install() {
    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_CHARSEL_SELECT_PLAYER),
        reinterpret_cast<void*>(&Hook_CharSelSelectPlayer),
        reinterpret_cast<void**>(&s_originalSelectPlayer));
    if (status != MH_OK) {
        LOG_ERROR("[CharSelPalette] Failed to hook sub_5BECC0! Status: %d", status);
        return false;
    }

    status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_CHARSEL_RENDER_HELPER),
        reinterpret_cast<void*>(&Hook_CharSelRenderHelper),
        reinterpret_cast<void**>(&s_originalRenderHelper));
    if (status != MH_OK) {
        LOG_ERROR("[CharSelPalette] Failed to hook sub_5BF490! Status: %d", status);
        return false;
    }

    LOG_INFO("[CharSelPalette] Installed mode-6 palette hooks");
    return true;
}

void CharSelPaletteSelect_FrameUpdate() {
    if (GetGameMode() == MODE_CHARSEL) {
        return;
    }

    if (s_blockOfflineFrontend) {
        Rollback::NetplayLog_Write(
            "CHARPAL", -1,
            "Cleared offline frontend block after leaving CharSel: mode=%u",
            GetGameMode());
    }
    s_blockOfflineFrontend = false;

    if (s_frontendActive) {
        DeactivateFrontend();
    }
}

void CharSelPaletteSelect_OnCharSelBegin(bool netplay, uint8_t localGameSlot) {
    BeginFrontend(netplay, localGameSlot);
}

void CharSelPaletteSelect_EndFrontend() {
    DeactivateFrontend();
}

void CharSelPaletteSelect_ResetNetplaySessionState(const char* reason) {
    ClearPaletteSlotCaches();
    memset(s_matchSelection, 0, sizeof(s_matchSelection));
    memset(s_externalCustomHint, 0, sizeof(s_externalCustomHint));
    memset(s_pendingCatalogReceived, 0, sizeof(s_pendingCatalogReceived));
    memset(s_pendingCatalogMasks, 0, sizeof(s_pendingCatalogMasks));

    if (GetGameMode() == MODE_CHARSEL) {
        ResetNativeSelectionState(0);
        ResetNativeSelectionState(1);
    }

    Rollback::NetplayLog_Write("CHARPAL", -1,
        "Netplay connection palette reset (local+remote cache cleared): "
        "frontend_active=%d netplay=%d reason=%s",
        s_frontendActive ? 1 : 0,
        s_frontendNetplay ? 1 : 0,
        reason ? reason : "?");
}

void CharSelPaletteSelect_OnRemoteCatalog(const CharSelInputPayload* payload) {
    if (!payload || payload->game_slot > 1) {
        return;
    }

    const bool canApplyImmediately = s_frontendNetplay && s_frontendActive;
    const bool shouldBufferPending = IsModNetplayCharSelContext() &&
        GetSubstate() < CHARSEL_SUB_STAGESEL_SLIDE;

    if (!canApplyImmediately && !shouldBufferPending) {
        return;
    }

    uint16_t (*targetMasks)[256] = canApplyImmediately ? s_catalogMasks : s_pendingCatalogMasks;
    bool* targetReceived = canApplyImmediately ? s_catalogReceived : s_pendingCatalogReceived;

    memcpy(targetMasks[payload->game_slot],
        payload->custom_masks,
        sizeof(payload->custom_masks));
    targetReceived[payload->game_slot] = true;
    Rollback::NetplayLog_Write("CHARPAL", -1,
        canApplyImmediately ? "Received palette catalog: slot=P%d ready=%d/%d"
                            : "Buffered palette catalog: slot=P%d pending=%d/%d",
        payload->game_slot + 1,
        targetReceived[0] ? 1 : 0,
        targetReceived[1] ? 1 : 0);
}

void CharSelPaletteSelect_OnLocalCatalogChanged() {
    if (!s_frontendActive) {
        return;
    }

    uint16_t localMasks[256] = {};
    BuildLocalCatalog(localMasks);

    if (s_frontendNetplay) {
        memcpy(s_catalogMasks[s_localGameSlot], localMasks, sizeof(localMasks));
        s_catalogReceived[s_localGameSlot] = true;
        RefreshSlotAfterCatalogChange(s_localGameSlot);
        SendLocalCatalog();
        return;
    }

    memcpy(s_catalogMasks[0], localMasks, sizeof(localMasks));
    memcpy(s_catalogMasks[1], localMasks, sizeof(localMasks));
    s_catalogReceived[0] = true;
    s_catalogReceived[1] = true;
    RefreshSlotAfterCatalogChange(0);
    RefreshSlotAfterCatalogChange(1);
}

void CharSelPaletteSelect_ClearExternalCustomHints() {
    bool changed = false;
    for (uint8_t gameSlot = 0; gameSlot < 2; ++gameSlot) {
        if (!s_externalCustomHint[gameSlot].valid) {
            continue;
        }

        memset(&s_externalCustomHint[gameSlot], 0, sizeof(s_externalCustomHint[gameSlot]));
        changed = true;
        if (s_frontendActive) {
            RefreshSlotAfterCatalogChange(gameSlot);
        }
    }

    if (changed) {
        Rollback::NetplayLog_Write("CHARPAL", -1, "Cleared external custom palette hints");
    }
}

void CharSelPaletteSelect_SetExternalCustomHint(uint8_t gameSlot,
                                                uint8_t characterId,
                                                uint8_t basePalette,
                                                bool available) {
    if (gameSlot > 1) {
        return;
    }

    ExternalCustomHint next{};
    if (available && basePalette < kVisibleVanillaPaletteCount) {
        next.valid = true;
        next.character_id = characterId;
        next.base_palette = basePalette;
    }

    ExternalCustomHint& current = s_externalCustomHint[gameSlot];
    if (current.valid == next.valid &&
        current.character_id == next.character_id &&
        current.base_palette == next.base_palette) {
        return;
    }

    current = next;
    if (s_frontendActive) {
        RefreshSlotAfterCatalogChange(gameSlot);
    }
}

bool CharSelPaletteSelect_IsCatalogReady() {
    if (!s_frontendNetplay) {
        return true;
    }
    return s_catalogReceived[0] && s_catalogReceived[1];
}

bool CharSelPaletteSelect_IsSelectionLocked(uint8_t gameSlot) {
    return gameSlot < 2 && s_slotState[gameSlot].phase == SlotPhase::LockedFinal;
}

bool CharSelPaletteSelect_CanCancelSelection(uint8_t gameSlot) {
    return gameSlot < 2 &&
           s_frontendActive &&
           s_slotState[gameSlot].phase != SlotPhase::None;
}

bool CharSelPaletteSelect_CancelSelection(uint8_t gameSlot) {
    return CancelSlotSelection(gameSlot, "sync B");
}

bool CharSelPaletteSelect_RequestRandomCharacter(uint8_t gameSlot,
                                                 uint8_t targetGridIndex,
                                                 const char* context) {
    return BeginRandomCharacterScroll(gameSlot, targetGridIndex, context);
}

bool CharSelPaletteSelect_IsRandomCharacterActive(uint8_t gameSlot) {
    return gameSlot < 2 && s_randomScroll[gameSlot].active;
}

bool CharSelPaletteSelect_ForceSelectionLocked(uint8_t gameSlot,
                                               uint8_t characterId,
                                               uint8_t basePalette,
                                               bool useCustom) {
    if (gameSlot > 1 || !s_frontendActive || !IsSelectableCharacter(characterId)) {
        return false;
    }

    PaletteOption previousOption{};
    bool hadPreviousOption = TryGetDisplayedOption(gameSlot, s_slotState[gameSlot], &previousOption);
    if (!hadPreviousOption) {
        const MatchSelection& previousSelection = s_matchSelection[gameSlot];
        if (previousSelection.valid) {
            previousOption.base_palette = previousSelection.base_palette;
            previousOption.use_custom = previousSelection.use_custom;
            hadPreviousOption = true;
        }
    }

    PaletteOption options[kOptionCapacity] = {};
    const int optionCount = BuildOptions(gameSlot,
        characterId,
        options,
        (int)kOptionCapacity);
    if (optionCount <= 0) {
        return false;
    }

    uint8_t optionIndex = 0;
    if (!TryFindOptionIndex(options, optionCount, basePalette, useCustom, &optionIndex) &&
        !TryFindOptionIndex(options, optionCount, basePalette, false, &optionIndex)) {
        optionIndex = 0;
    }

    uint8_t gridIndex = 0;
    if (!TryFindGridIndex(characterId, &gridIndex)) {
        return false;
    }

    const int localSelection = (int)((gameSlot == 0 ? ADDR_CHARSEL_P1_CHAR_ID : ADDR_CHARSEL_P2_CHAR_ID) - 176);
    uint8_t* const control = reinterpret_cast<uint8_t*>(gameSlot == 0 ? ADDR_CHARSEL_P1_ENABLE : ADDR_CHARSEL_P2_ENABLE);
    const PaletteOption& option = options[optionIndex];

    ResetRandomScroll(gameSlot);
    control[0] = 0;
    control[1] = gridIndex;

    s_slotState[gameSlot].phase = SlotPhase::LockedFinal;
    s_slotState[gameSlot].character_id = characterId;
    s_slotState[gameSlot].display_index = optionIndex;

    WriteSelectionValues(localSelection,
        control,
        characterId,
        option,
        false);

    s_matchSelection[gameSlot].valid = true;
    s_matchSelection[gameSlot].character_id = characterId;
    s_matchSelection[gameSlot].base_palette = option.base_palette;
    s_matchSelection[gameSlot].use_custom = option.use_custom;
    RememberPaletteSelection(gameSlot, characterId, option, "force lock");
    MaybeRequestPreviewReload(gameSlot,
        hadPreviousOption,
        hadPreviousOption ? &previousOption : nullptr,
        true,
        &option);
    return true;
}

bool CharSelPaletteSelect_ShouldPreviewCustomBank(uint8_t gameSlot,
                                                  uint8_t characterId,
                                                  uint8_t basePalette) {
    if (gameSlot > 1) {
        return false;
    }

    PaletteOption option{};
    const FrontendSlotState& slotState = s_slotState[gameSlot];
    if (TryGetDisplayedOption(gameSlot, slotState, &option)) {
        return option.use_custom &&
               slotState.character_id == characterId &&
               option.base_palette == basePalette;
    }

    const MatchSelection& selection = s_matchSelection[gameSlot];
    return selection.valid &&
           selection.use_custom &&
           selection.character_id == characterId &&
           selection.base_palette == basePalette;
}

bool CharSelPaletteSelect_ShouldUseCustomBank(uint8_t gameSlot,
                                              uint8_t characterId,
                                              uint8_t basePalette) {
    if (gameSlot > 1) {
        return false;
    }

    const MatchSelection& selection = s_matchSelection[gameSlot];
    return selection.valid &&
           selection.use_custom &&
           selection.character_id == characterId &&
           selection.base_palette == basePalette;
}

} // namespace Net
