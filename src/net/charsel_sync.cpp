/**
 * Alice Senki 2 - Character Select / Stage Select Sync
 *
 * Frontend-core-backed lockstep for character select and stage select.
 * Character select and stage select now share one negotiated frontend delay
 * and one epoch-scoped timeline. StageSync remains a watchdog packet only;
 * the shared stage cursor is still driven by deterministic merged inputs.
 */

#include "net/charsel_sync.h"

#include "net/barrier_protocol.h"
#include "net/frontend_input_sync.h"
#include "net/session_manager.h"
#include "net/stage_watchdog_tracker.h"
#include "net/stagesel_sync.h"
#include "core/as2_constants.h"
#include "input_system.h"
#include "patches/charsel_select_actions.h"
#include "patches/charsel_palette_select.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <string.h>

namespace {

using namespace Net;

constexpr uintptr_t ADDR_STAGE_AUX = 0x816028;
constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_CURSOR = 0x81602A;
constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_ACTION = 0x81602B;
constexpr DWORD STAGE_WATCHDOG_RESEND_MS = 250;

struct StageWatchdogState {
    uint8_t stage_id;
    uint8_t stage_cursor;
    uint8_t stage_confirmed;
    uint8_t stage_counter;
    uint8_t stage_aux;
    uint8_t stage_cancel;
    uint8_t confirm_menu_cursor;
    uint8_t confirm_menu_action;
    uint8_t committed_stage_id;
    uint8_t substate;
};

static bool     s_initialized = false;
static bool     s_active = false;
static bool     s_isHost = false;
static bool     s_inStagePhase = false;

static bool     s_bothCharsLocked = false;
static bool     s_bothStageLocked = false;
static bool     s_localCharConfirmed = false;
static bool     s_remoteCharLocked = false;
static bool     s_localStageLocked = false;
static bool     s_remoteStageLocked = false;
static bool     s_localLockSent = false;

static bool     s_charBoundarySent = false;
static bool     s_stageBoundarySent = false;
static bool     s_charDigestSent = false;
static bool     s_stageDigestSent = false;

static uint8_t  s_localChar = 0;
static uint8_t  s_localPalette = 0;
static bool     s_localPaletteCustom = false;
static uint8_t  s_remoteChar = 0;
static uint8_t  s_remotePalette = 0;
static bool     s_remotePaletteCustom = false;
static uint8_t  s_localStage = 0;
static uint8_t  s_remoteStage = 0;
static uint8_t  s_finalStageId = 0;

static StageWatchdogState s_localStageState = {};
static StageWatchdogState s_lastSentStageState = {};
static StageWatchdogState s_remoteStageState = {};
static StageWatchdogState s_stageBoundaryState = {};
static bool     s_stageBoundaryStateValid = false;
static DWORD    s_lastStageWatchdogSendTime = 0;
static StageWatchdogTrackerState s_remoteStageTracker = {};

static bool IsStageSelCommittedSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_MATCHUP_COMMIT ||
           substate == CHARSEL_SUB_TO_MATCH;
}

static uint8_t LookupCharId(uint8_t gridIndex) {
    if (gridIndex > 20) {
        return 0;
    }
    return (uint8_t)(ReadMemory<uint32_t>(ADDR_CHARSEL_GRID_TABLE + gridIndex * 4) & 0xFF);
}

static bool StageWatchdogEquals(const StageWatchdogState& a, const StageWatchdogState& b) {
    return memcmp(&a, &b, sizeof(StageWatchdogState)) == 0;
}

static uint8_t ResolveFinalStageId(const StageWatchdogState& state) {
    if (state.committed_stage_id != 0) {
        return state.committed_stage_id;
    }
    if (state.stage_id != 0) {
        return state.stage_id;
    }
    return state.stage_cursor;
}

static StageWatchdogState MakeStageBoundaryState(const StageWatchdogState& state) {
    StageWatchdogState boundaryState = state;
    const uint8_t finalStageId = ResolveFinalStageId(state);
    boundaryState.stage_id = finalStageId;
    boundaryState.committed_stage_id = finalStageId;
    boundaryState.stage_confirmed = 1;
    return boundaryState;
}

static void ResetState() {
    s_active = false;
    s_isHost = false;
    s_inStagePhase = false;
    s_bothCharsLocked = false;
    s_bothStageLocked = false;
    s_localCharConfirmed = false;
    s_remoteCharLocked = false;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_localLockSent = false;
    s_charBoundarySent = false;
    s_stageBoundarySent = false;
    s_charDigestSent = false;
    s_stageDigestSent = false;
    s_localChar = 0;
    s_localPalette = 0;
    s_localPaletteCustom = false;
    s_remoteChar = 0;
    s_remotePalette = 0;
    s_remotePaletteCustom = false;
    s_localStage = 0;
    s_remoteStage = 0;
    s_finalStageId = 0;
    memset(&s_localStageState, 0, sizeof(s_localStageState));
    memset(&s_lastSentStageState, 0, sizeof(s_lastSentStageState));
    memset(&s_remoteStageState, 0, sizeof(s_remoteStageState));
    memset(&s_stageBoundaryState, 0, sizeof(s_stageBoundaryState));
    s_stageBoundaryStateValid = false;
    s_lastStageWatchdogSendTime = 0;
    StageWatchdogTracker_Reset(&s_remoteStageTracker);
    CharSelSelectActions_ResetStageRandomScroll();
}

static void ResetStageSelectionRuntime() {
    s_inStagePhase = false;
    s_bothStageLocked = false;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_stageBoundarySent = false;
    s_stageDigestSent = false;
    s_localStage = 0;
    s_remoteStage = 0;
    s_finalStageId = 0;
    memset(&s_localStageState, 0, sizeof(s_localStageState));
    memset(&s_lastSentStageState, 0, sizeof(s_lastSentStageState));
    memset(&s_remoteStageState, 0, sizeof(s_remoteStageState));
    memset(&s_stageBoundaryState, 0, sizeof(s_stageBoundaryState));
    s_stageBoundaryStateValid = false;
    s_lastStageWatchdogSendTime = 0;
    StageWatchdogTracker_Reset(&s_remoteStageTracker);
    CharSelSelectActions_ResetStageRandomScroll();
}

static void ClearNativeStageSelectionState() {
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 1, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 2, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_AUX, 0);
    WriteMemory<uint8_t>(ADDR_CHARSEL_CANCEL, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION, 0);
    WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, 0);
}

static void RefreshLocalCharacterSelectionFromMemory() {
    const uintptr_t cursorAddr = s_isHost ? ADDR_CHARSEL_P1_CURSOR : ADDR_CHARSEL_P2_CURSOR;
    const uintptr_t paletteAddr = s_isHost ? ADDR_CHARSEL_P1_PALETTE : ADDR_CHARSEL_P2_PALETTE;
    const uint8_t localGameSlot = s_isHost ? 0 : 1;

    s_localChar = LookupCharId(ReadMemory<uint8_t>(cursorAddr));
    s_localPalette = ReadMemory<uint8_t>(paletteAddr);
    s_localPaletteCustom = CharSelPaletteSelect_ShouldUseCustomBank(
        localGameSlot,
        s_localChar,
        s_localPalette);
}

static void RestartCharacterInputPhase(const char* reason) {
    const uint32_t nextEpoch = FrontendInputSync_GetEpochId() + 1u;
    FrontendInputSync_RebindEpoch(nextEpoch, reason);
    FrontendInputSync_BeginInputPhase(
        FrontendSyncPhase::CharSel,
        PacketType::CharSelFrameInput,
        reason);
}

static void ResetCharacterBoundaryAfterCancel(const char* reason) {
    const uint8_t localGameSlot = s_isHost ? 0 : 1;

    s_bothCharsLocked = false;
    s_charBoundarySent = false;
    s_charDigestSent = false;
    s_localLockSent = false;
    s_remoteCharLocked = false;
    s_remoteChar = 0;
    s_remotePalette = 0;
    s_remotePaletteCustom = false;

    s_localCharConfirmed = CharSelPaletteSelect_IsSelectionLocked(localGameSlot);
    if (s_localCharConfirmed) {
        RefreshLocalCharacterSelectionFromMemory();
    } else {
        s_localChar = 0;
        s_localPalette = 0;
        s_localPaletteCustom = false;
    }

    ResetStageSelectionRuntime();
    FrontendInputSync_ClearPhaseBarrier();
    RestartCharacterInputPhase(reason);
}

static bool CancelCharacterSlot(uint8_t gameSlot, const char* reason) {
    if (!CharSelPaletteSelect_CanCancelSelection(gameSlot)) {
        Rollback::NetplayLog_Write(
            "CHARSEL", -1,
            "Character cancel ignored: slot=P%d reason=no cancelable selection context=%s",
            gameSlot + 1,
            reason ? reason : "?");
        return false;
    }
    if (!CharSelPaletteSelect_CancelSelection(gameSlot)) {
        Rollback::NetplayLog_Write(
            "CHARSEL", -1,
            "Character cancel ignored: slot=P%d reason=palette frontend refused context=%s",
            gameSlot + 1,
            reason ? reason : "?");
        return false;
    }

    Rollback::NetplayLog_Write(
        "CHARSEL", -1,
        "Character selection canceled: slot=P%d reason=%s",
        gameSlot + 1,
        reason ? reason : "?");
    return true;
}

static void ResetCharacterSelectionRuntimeForStageCancel() {
    s_bothCharsLocked = false;
    s_localCharConfirmed = false;
    s_remoteCharLocked = false;
    s_localLockSent = false;
    s_charBoundarySent = false;
    s_charDigestSent = false;
    s_localChar = 0;
    s_localPalette = 0;
    s_localPaletteCustom = false;
    s_remoteChar = 0;
    s_remotePalette = 0;
    s_remotePaletteCustom = false;
}

static void ForceStageCancelBackToCharacterSelect(bool netplay, const char* reason) {
    ClearNativeStageSelectionState();
    WriteMemory<uint32_t>(ADDR_SUB_STATE, CHARSEL_SUB_SELECT);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0);
    CharSelPaletteSelect_OnCharSelBegin(netplay, netplay ? (s_isHost ? 0 : 1) : 0);

    Rollback::NetplayLog_Write(
        "STAGESEL", -1,
        "Stage selection canceled back to character select: mode=%u type=%u sub=%u netplay=%u reason=%s",
        GetGameMode(),
        GetGameType(),
        GetSubstate(),
        netplay ? 1 : 0,
        reason ? reason : "?");
}

static void SendCharSelLock() {
    const uintptr_t cursorAddr = s_isHost ? ADDR_CHARSEL_P1_CURSOR : ADDR_CHARSEL_P2_CURSOR;
    const uintptr_t paletteAddr = s_isHost ? ADDR_CHARSEL_P1_PALETTE : ADDR_CHARSEL_P2_PALETTE;
    const uint8_t localGameSlot = s_isHost ? 0 : 1;

    s_localChar = LookupCharId(ReadMemory<uint8_t>(cursorAddr));
    s_localPalette = ReadMemory<uint8_t>(paletteAddr);
    s_localPaletteCustom = CharSelPaletteSelect_ShouldUseCustomBank(
        localGameSlot,
        s_localChar,
        s_localPalette);

    CharSelLockPayload payload{};
    payload.epoch_id = FrontendInputSync_GetEpochId();
    payload.phase = (uint16_t)FrontendSyncPhase::CharSel;
    payload.character_id = s_localChar;
    payload.palette = s_localPalette;
    payload.flags = s_localPaletteCustom ? CHARSEL_LOCK_FLAG_CUSTOM_PALETTE : 0;

    BarrierProtocol_SendPacket(PacketType::CharSelLock, &payload, sizeof(payload));

    Rollback::NetplayLog_Write(
        "CHARSEL", -1,
        "Sent CharSelLock: epoch=%u char=%u palette=%u custom=%u",
        payload.epoch_id,
        payload.character_id,
        payload.palette,
        s_localPaletteCustom ? 1 : 0);
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Sent CharSelLock: char=%u pal=%u custom=%u",
        payload.character_id,
        payload.palette,
        s_localPaletteCustom ? 1 : 0);
}

static StageWatchdogState ReadLocalStageWatchdogState() {
    StageWatchdogState state{};
    state.stage_cursor = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
    state.stage_confirmed = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 1);
    state.stage_counter = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 2);
    state.stage_aux = ReadMemory<uint8_t>(ADDR_STAGE_AUX);
    state.stage_cancel = ReadMemory<uint8_t>(ADDR_CHARSEL_CANCEL);
    state.confirm_menu_cursor = ReadMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR);
    state.confirm_menu_action = ReadMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION);
    state.committed_stage_id = ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID);
    state.substate = (uint8_t)GetSubstate();
    state.stage_id = state.committed_stage_id != 0 ? state.committed_stage_id : state.stage_cursor;
    return state;
}

static void ApplyAuthoritativeStageStateToLocalMenu(const StageWatchdogState& state,
                                                    bool preserveLocalStageCounter) {
    const uint8_t localStageCounter = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 2);
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR, state.stage_cursor);
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 1, state.stage_confirmed);
    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 2,
        preserveLocalStageCounter ? localStageCounter : state.stage_counter);
    WriteMemory<uint8_t>(ADDR_STAGE_AUX, state.stage_aux);
    WriteMemory<uint8_t>(ADDR_CHARSEL_CANCEL, state.stage_cancel);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR, state.confirm_menu_cursor);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION, state.confirm_menu_action);
    WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, state.committed_stage_id);
}

static bool ShouldPreserveLocalStageCounter(const StageWatchdogState& state) {
    if (s_isHost || !s_inStagePhase) {
        return false;
    }

    const uint8_t localSubstate = (uint8_t)GetSubstate();
    return localSubstate == CHARSEL_SUB_STAGESEL_GRID &&
           state.substate != CHARSEL_SUB_STAGESEL_GRID;
}

static void MirrorHostStageStateOnClient(const StageWatchdogState& state, const char* reason) {
    if (s_isHost || !s_inStagePhase) {
        return;
    }
    if (CharSelSelectActions_IsStageRandomScrollActive() &&
        !state.stage_confirmed &&
        state.committed_stage_id == 0) {
        Rollback::NetplayLog_Verbose(
            "STAGESEL", -1,
            "Skipped host stage watchdog mirror during deterministic random scroll: remote_frame_sub=%u cursor=%u local_cursor=%u reason=%s",
            state.substate,
            state.stage_cursor,
            ReadMemory<uint8_t>(ADDR_STAGE_CURSOR),
            reason ? reason : "?");
        return;
    }

    const bool preserveLocalCounter = ShouldPreserveLocalStageCounter(state);
    ApplyAuthoritativeStageStateToLocalMenu(state, preserveLocalCounter);
    s_localStageState = ReadLocalStageWatchdogState();
    s_localStageState.stage_id = ResolveFinalStageId(state);
    s_localStage = ResolveFinalStageId(s_localStageState);

    if (preserveLocalCounter) {
        Rollback::NetplayLog_Write(
            "STAGESEL", -1,
            "Mirrored host stage transition while preserving local counter: local_sub=%u remote_sub=%u stage=%u cursor=%u confirmed=%u local_counter=%u remote_counter=%u reason=%s",
            (unsigned)GetSubstate(),
            state.substate,
            ResolveFinalStageId(state),
            state.stage_cursor,
            state.stage_confirmed,
            s_localStageState.stage_counter,
            state.stage_counter,
            reason ? reason : "?");
    }

    if (state.stage_confirmed ||
        state.committed_stage_id != 0 ||
        IsStageSelCommittedSubstate(state.substate)) {
        s_localStageLocked = true;
        s_stageBoundaryState = MakeStageBoundaryState(state);
        s_stageBoundaryStateValid = true;
    }

    Rollback::NetplayLog_Verbose(
        "STAGESEL", -1,
        "Client mirrored host stage state: stage=%u cursor=%u confirmed=%u menu_cursor=%u menu_action=%u committed=%u reason=%s",
        ResolveFinalStageId(state),
        state.stage_cursor,
        state.stage_confirmed,
        state.confirm_menu_cursor,
        state.confirm_menu_action,
        state.committed_stage_id,
        reason ? reason : "?");
}

static void SendStageWatchdog(const StageWatchdogState& state, bool confirmed, const char* reason) {
    StageSyncPayload payload{};
    payload.epoch_id = FrontendInputSync_GetEpochId();
    payload.phase = (uint16_t)FrontendSyncPhase::StageSel;
    payload.frame = (uint16_t)FrontendInputSync_GetConsumeFrame();
    payload.stage_id = state.stage_id;
    payload.confirmed = confirmed ? 1 : 0;
    payload.stage_cursor = state.stage_cursor;
    payload.stage_counter = state.stage_counter;
    payload.stage_aux = state.stage_aux;
    payload.stage_cancel = state.stage_cancel;
    payload.confirm_menu_cursor = state.confirm_menu_cursor;
    payload.confirm_menu_action = state.confirm_menu_action;
    payload.committed_stage_id = state.committed_stage_id;
    payload.substate = state.substate;
    BarrierProtocol_SendPacket(PacketType::StageSync, &payload, sizeof(payload));

    Rollback::NetplayLog_Verbose(
        "STAGESEL", -1,
        "Sent StageSync watchdog: epoch=%u frame=%u stage=%u confirmed=%u cursor=%u counter=%u sub=%u reason=%s",
        payload.epoch_id,
        payload.frame,
        payload.stage_id,
        payload.confirmed,
        payload.stage_cursor,
        payload.stage_counter,
        payload.substate,
        reason ? reason : "?");
}

static uint32_t BuildCharBoundaryDigest() {
    struct DigestData {
        uint8_t p1_character;
        uint8_t p1_palette;
        uint8_t p2_character;
        uint8_t p2_palette;
        uint8_t p1_palette_custom;
        uint8_t p2_palette_custom;
        uint8_t local_confirmed;
        uint8_t remote_confirmed;
        uint8_t _pad[2];
    } data{};

    if (s_isHost) {
        data.p1_character = s_localChar;
        data.p1_palette = s_localPalette;
        data.p1_palette_custom = s_localPaletteCustom ? 1 : 0;
        data.p2_character = s_remoteChar;
        data.p2_palette = s_remotePalette;
        data.p2_palette_custom = s_remotePaletteCustom ? 1 : 0;
    } else {
        data.p1_character = s_remoteChar;
        data.p1_palette = s_remotePalette;
        data.p1_palette_custom = s_remotePaletteCustom ? 1 : 0;
        data.p2_character = s_localChar;
        data.p2_palette = s_localPalette;
        data.p2_palette_custom = s_localPaletteCustom ? 1 : 0;
    }
    data.local_confirmed = s_localCharConfirmed ? 1 : 0;
    data.remote_confirmed = s_remoteCharLocked ? 1 : 0;
    return CalcCRC32(&data, sizeof(data));
}

static void SendCharBoundaryDigest() {
    FrontendBoundaryDigestPayload payload{};
    payload.epoch_id = FrontendInputSync_GetEpochId();
    payload.phase = (uint16_t)FrontendSyncPhase::CharSel;
    payload.digest_kind = (uint8_t)FrontendDigestKind::CharSelLock;
    payload.substate = (uint8_t)GetSubstate();
    payload.frame = FrontendInputSync_GetConsumeFrame();

    if (s_isHost) {
        payload.p1_character = s_localChar;
        payload.p1_palette = s_localPalette;
        payload.p1_palette_custom = s_localPaletteCustom ? 1 : 0;
        payload.p2_character = s_remoteChar;
        payload.p2_palette = s_remotePalette;
        payload.p2_palette_custom = s_remotePaletteCustom ? 1 : 0;
    } else {
        payload.p1_character = s_remoteChar;
        payload.p1_palette = s_remotePalette;
        payload.p1_palette_custom = s_remotePaletteCustom ? 1 : 0;
        payload.p2_character = s_localChar;
        payload.p2_palette = s_localPalette;
        payload.p2_palette_custom = s_localPaletteCustom ? 1 : 0;
    }
    payload.digest = BuildCharBoundaryDigest();
    FrontendInputSync_SendBoundaryDigest(&payload, "character lock boundary");
    s_charDigestSent = true;
}

static uint32_t BuildStageBoundaryDigest(const StageWatchdogState& state) {
    struct DigestData {
        uint8_t p1_character;
        uint8_t p1_palette;
        uint8_t p2_character;
        uint8_t p2_palette;
        uint8_t p1_palette_custom;
        uint8_t p2_palette_custom;
        uint8_t final_stage_id;
        uint8_t _pad;
    } data{};

    if (s_isHost) {
        data.p1_character = s_localChar;
        data.p1_palette = s_localPalette;
        data.p1_palette_custom = s_localPaletteCustom ? 1 : 0;
        data.p2_character = s_remoteChar;
        data.p2_palette = s_remotePalette;
        data.p2_palette_custom = s_remotePaletteCustom ? 1 : 0;
    } else {
        data.p1_character = s_remoteChar;
        data.p1_palette = s_remotePalette;
        data.p1_palette_custom = s_remotePaletteCustom ? 1 : 0;
        data.p2_character = s_localChar;
        data.p2_palette = s_localPalette;
        data.p2_palette_custom = s_localPaletteCustom ? 1 : 0;
    }
    // Only hash stable boundary state. Post-lock UI bytes like substate,
    // confirm-menu state, and transition counters can advance on different
    // frames before the reliable boundary exchange completes.
    data.final_stage_id = ResolveFinalStageId(state);
    return CalcCRC32(&data, sizeof(data));
}

static void SendStageBoundaryDigest() {
    const StageWatchdogState& boundaryState = s_stageBoundaryStateValid
        ? s_stageBoundaryState
        : s_localStageState;

    FrontendBoundaryDigestPayload payload{};
    payload.epoch_id = FrontendInputSync_GetEpochId();
    payload.phase = (uint16_t)FrontendSyncPhase::StageSel;
    payload.digest_kind = (uint8_t)FrontendDigestKind::StageSel;
    payload.substate = boundaryState.substate;
    payload.frame = FrontendInputSync_GetConsumeFrame();

    if (s_isHost) {
        payload.p1_character = s_localChar;
        payload.p1_palette = s_localPalette;
        payload.p1_palette_custom = s_localPaletteCustom ? 1 : 0;
        payload.p2_character = s_remoteChar;
        payload.p2_palette = s_remotePalette;
        payload.p2_palette_custom = s_remotePaletteCustom ? 1 : 0;
    } else {
        payload.p1_character = s_remoteChar;
        payload.p1_palette = s_remotePalette;
        payload.p1_palette_custom = s_remotePaletteCustom ? 1 : 0;
        payload.p2_character = s_localChar;
        payload.p2_palette = s_localPalette;
        payload.p2_palette_custom = s_localPaletteCustom ? 1 : 0;
    }

    payload.stage_cursor = boundaryState.stage_cursor;
    payload.stage_confirmed = boundaryState.stage_confirmed;
    payload.stage_counter = boundaryState.stage_counter;
    payload.stage_aux = boundaryState.stage_aux;
    payload.stage_cancel = boundaryState.stage_cancel;
    payload.confirm_menu_cursor = boundaryState.confirm_menu_cursor;
    payload.confirm_menu_action = boundaryState.confirm_menu_action;
    payload.committed_stage_id = boundaryState.committed_stage_id;
    payload.digest = BuildStageBoundaryDigest(boundaryState);
    FrontendInputSync_SendBoundaryDigest(&payload, "stage lock boundary");
    s_stageDigestSent = true;
}

static void MaybeFinalizeCharacterBoundary() {
    if (!s_localCharConfirmed || !s_remoteCharLocked || s_bothCharsLocked) {
        return;
    }

    if (!s_charDigestSent) {
        SendCharBoundaryDigest();
    }
    if (!s_charBoundarySent) {
        FrontendInputSync_SendPhaseBarrier(FrontendSyncPhase::StageSel, 1, "characters locked");
        s_charBoundarySent = true;
    }

    if (FrontendInputSync_IsDigestMatched(FrontendDigestKind::CharSelLock) &&
        FrontendInputSync_IsPhaseBarrierSatisfied(FrontendSyncPhase::StageSel)) {
        s_bothCharsLocked = true;
        Rollback::NetplayLog_Write(
            "CHARSEL", -1,
            "Both characters locked with barrier+digest agreement: p1=%u p2=%u",
            s_isHost ? s_localChar : s_remoteChar,
            s_isHost ? s_remoteChar : s_localChar);
        LOG_NETPLAY(LOG_INFO, "[CharSelSync] Character boundary agreed");
    }
}

static void MaybeFinalizeStageBoundary() {
    if (!s_localStageLocked || !s_remoteStageLocked || s_bothStageLocked) {
        return;
    }

    if (!s_stageDigestSent) {
        SendStageBoundaryDigest();
    }
    if (!s_stageBoundarySent) {
        FrontendInputSync_SendPhaseBarrier(FrontendSyncPhase::Locked, 2, "stage locked");
        s_stageBoundarySent = true;
    }

    if (!FrontendInputSync_IsDigestMatched(FrontendDigestKind::StageSel) ||
        !FrontendInputSync_IsPhaseBarrierSatisfied(FrontendSyncPhase::Locked)) {
        return;
    }

    if (s_isHost) {
        s_finalStageId = s_localStageState.committed_stage_id != 0
            ? s_localStageState.committed_stage_id
            : s_localStage;
    } else {
        s_finalStageId = s_remoteStageState.committed_stage_id != 0
            ? s_remoteStageState.committed_stage_id
            : s_remoteStage;
    }

    s_bothStageLocked = true;
    s_inStagePhase = false;
    FrontendInputSync_BeginPassivePhase(FrontendSyncPhase::Locked, "stage boundary agreed");
    Rollback::NetplayLog_Write(
        "STAGESEL", -1,
        "Stage boundary agreed: local=%u remote=%u final=%u",
        s_localStage,
        s_remoteStage,
        s_finalStageId);
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Stage boundary agreed: final=%u", s_finalStageId);
}

} // anonymous namespace

namespace Net {

void CharSelSync_Init() {
    if (s_initialized) {
        return;
    }
    ResetState();
    StageSelSync_Init();
    s_initialized = true;
}

void CharSelSync_Shutdown() {
    if (!s_initialized) {
        return;
    }
    ResetState();
    StageSelSync_Shutdown();
    s_initialized = false;
}

void CharSelSync_Begin() {
    if (!s_initialized) {
        return;
    }

    ResetState();
    s_active = true;
    s_isHost = (Session_GetRole() == SessionRole::Host);
    FrontendInputSync_BeginInputPhase(
        FrontendSyncPhase::CharSel,
        PacketType::CharSelFrameInput,
        "charsel begin");

    CharSelPaletteSelect_OnCharSelBegin(true, s_isHost ? 0 : 1);
    Rollback::NetplayLog_Write(
        "CHARSEL", -1,
        "=== CHARSEL BEGIN: epoch=%u shared_delay=%u role=%s ===",
        FrontendInputSync_GetEpochId(),
        FrontendInputSync_GetSharedDelay(),
        s_isHost ? "Host" : "Join");
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Begin (shared frontend delay=%u)",
        FrontendInputSync_GetSharedDelay());
}

void CharSelSync_BeginStagePhase() {
    if (!s_active) {
        return;
    }
    s_inStagePhase = true;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_bothStageLocked = false;
    s_stageBoundarySent = false;
    s_stageDigestSent = false;
    memset(&s_localStageState, 0, sizeof(s_localStageState));
    memset(&s_lastSentStageState, 0, sizeof(s_lastSentStageState));
    memset(&s_remoteStageState, 0, sizeof(s_remoteStageState));
    memset(&s_stageBoundaryState, 0, sizeof(s_stageBoundaryState));
    s_stageBoundaryStateValid = false;
    s_lastStageWatchdogSendTime = 0;
    StageWatchdogTracker_Reset(&s_remoteStageTracker);

    CharSelPaletteSelect_EndFrontend();
    StageSelSync_Begin();
    FrontendInputSync_BeginInputPhase(
        FrontendSyncPhase::StageSel,
        PacketType::CharSelFrameInput,
        "stage select begin");

    Rollback::NetplayLog_Write(
        "STAGESEL", -1,
        "=== STAGE SELECT BEGIN: epoch=%u shared_delay=%u ===",
        FrontendInputSync_GetEpochId(),
        FrontendInputSync_GetSharedDelay());
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Stage phase begin (shared frontend delay preserved)");
}

void CharSelSync_Abort() {
    if (!s_active) {
        return;
    }
    s_active = false;
    s_inStagePhase = false;
    CharSelPaletteSelect_EndFrontend();
    StageSelSync_Abort();
    FrontendInputSync_EndPhase("charsel abort");
    ResetState();
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Abort");
}

void CharSelSync_CaptureLocalInput(uint16_t packedInput) {
    if (!s_active) {
        return;
    }
    FrontendInputSync_CaptureLocalInput(packedInput);
}

bool CharSelSync_IsLockstepActive() {
    return s_active;
}

bool CharSelSync_HasInputsForCurrentFrame() {
    return FrontendInputSync_HasInputsForCurrentFrame();
}

bool CharSelSync_ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2) {
    if (!s_active || !outP1 || !outP2) {
        return false;
    }

    uint16_t localInput = 0;
    uint16_t remoteInput = 0;
    if (!FrontendInputSync_ConsumeCurrentFrame(&localInput, &remoteInput, nullptr)) {
        return false;
    }

    uint16_t p1 = 0;
    uint16_t p2 = 0;
    if (s_isHost) {
        p1 = localInput;
        p2 = remoteInput;
    } else {
        p1 = remoteInput;
        p2 = localInput;
    }

    *outP1 = p1;
    *outP2 = p2;

    return true;
}

uint8_t CharSelSync_HandleCharacterCancelInput(uint16_t p1Just, uint16_t p2Just) {
    if (!s_active || s_inStagePhase || GetGameMode() != MODE_CHARSEL) {
        return 0;
    }

    uint8_t canceledMask = 0;
    if ((p1Just & INPUT_B) != 0 &&
        CancelCharacterSlot(0, "P1 B")) {
        canceledMask |= 0x01;
    }
    if ((p2Just & INPUT_B) != 0 &&
        CancelCharacterSlot(1, "P2 B")) {
        canceledMask |= 0x02;
    }

    if (canceledMask == 0) {
        return 0;
    }

    ResetCharacterBoundaryAfterCancel("character B cancel");
    Rollback::NetplayLog_Write(
        "CHARSEL", -1,
        "Character cancel applied: slots=0x%02X epoch=%u local_locked=%u",
        canceledMask,
        FrontendInputSync_GetEpochId(),
        s_localCharConfirmed ? 1 : 0);
    return canceledMask;
}

bool CharSelSync_CancelStageSelectionBackToCharacterSelect(const char* reason) {
    if (!s_active ||
        !s_inStagePhase ||
        GetGameMode() != MODE_CHARSEL) {
        return false;
    }

    StageSelSync_Abort();
    ResetStageSelectionRuntime();
    ResetCharacterSelectionRuntimeForStageCancel();
    ForceStageCancelBackToCharacterSelect(true, reason ? reason : "netplay stage cancel");
    FrontendInputSync_ClearPhaseBarrier();
    RestartCharacterInputPhase("stage B cancel back to charsel");
    return true;
}

void CharSelSync_FrameUpdate() {
    if (!s_active) {
        return;
    }

    FrontendInputSync_FrameUpdate();

    const uint32_t mode = GetGameMode();
    if (mode == MODE_CHARSEL) {
        const uint8_t localGameSlot = s_isHost ? 0 : 1;
        const uintptr_t localCursorAddr = s_isHost ? ADDR_CHARSEL_P1_CURSOR : ADDR_CHARSEL_P2_CURSOR;

        if (CharSelPaletteSelect_IsSelectionLocked(localGameSlot) && !s_localCharConfirmed) {
            s_localCharConfirmed = true;
            s_localChar = LookupCharId(ReadMemory<uint8_t>(localCursorAddr));
            s_localPalette = ReadMemory<uint8_t>(s_isHost ? ADDR_CHARSEL_P1_PALETTE : ADDR_CHARSEL_P2_PALETTE);
            s_localPaletteCustom = CharSelPaletteSelect_ShouldUseCustomBank(
                localGameSlot,
                s_localChar,
                s_localPalette);
            Rollback::NetplayLog_Write(
                "CHARSEL", -1,
                "Local character finalized: char=%u pal=%u custom=%u slot=P%d",
                s_localChar,
                s_localPalette,
                s_localPaletteCustom ? 1 : 0,
                localGameSlot + 1);
        }

        if (s_localCharConfirmed && !s_localLockSent) {
            SendCharSelLock();
            s_localLockSent = true;
        }

        if (s_inStagePhase) {
            s_localStageState = ReadLocalStageWatchdogState();
            s_localStage = s_localStageState.stage_id;

            const DWORD now = GetTickCount();
            if (!StageWatchdogEquals(s_localStageState, s_lastSentStageState) ||
                (now - s_lastStageWatchdogSendTime) >= STAGE_WATCHDOG_RESEND_MS) {
                SendStageWatchdog(s_localStageState, s_localStageLocked, "state update");
                s_lastSentStageState = s_localStageState;
                s_lastStageWatchdogSendTime = now;
            }

            if (IsStageSelCommittedSubstate(GetSubstate()) && !s_localStageLocked) {
                s_localStageLocked = true;
                s_localStage = s_localStageState.committed_stage_id != 0
                    ? s_localStageState.committed_stage_id
                    : s_localStageState.stage_id;
                s_localStageState.stage_id = s_localStage;
                s_localStageState.committed_stage_id = s_localStage;
                s_localStageState.stage_confirmed = 1;
                s_stageBoundaryState = MakeStageBoundaryState(s_localStageState);
                s_stageBoundaryStateValid = true;
                SendStageWatchdog(s_localStageState, true, "local stage lock");
                Rollback::NetplayLog_Write(
                    "STAGESEL", -1,
                    "Local stage locked: stage=%u sub=%u",
                    s_localStage,
                    GetSubstate());
            }
        }
    }

    MaybeFinalizeCharacterBoundary();
    MaybeFinalizeStageBoundary();
}

void CharSelSync_OnRemoteFrameInput(const CharSelFrameInputPayload* p) {
    FrontendInputSync_OnRemoteCharSelFrameInput(p);
}

void CharSelSync_OnRemoteLock(const CharSelLockPayload* p) {
    if (!s_active || !p) {
        return;
    }
    if (!FrontendInputSync_IsCurrentEpochPhase(p->epoch_id,
                                               p->phase,
                                               PacketType::CharSelLock,
                                               "char lock")) {
        return;
    }

    s_remoteCharLocked = true;
    s_remoteChar = p->character_id;
    s_remotePalette = p->palette;
    s_remotePaletteCustom = (p->flags & CHARSEL_LOCK_FLAG_CUSTOM_PALETTE) != 0;

    const uint8_t remoteGameSlot = s_isHost ? 1 : 0;
    CharSelPaletteSelect_SetExternalCustomHint(
        remoteGameSlot,
        s_remoteChar,
        s_remotePalette,
        s_remotePaletteCustom);
    const bool forced = CharSelPaletteSelect_ForceSelectionLocked(
        remoteGameSlot,
        s_remoteChar,
        s_remotePalette,
        s_remotePaletteCustom);

    Rollback::NetplayLog_Write(
        "CHARSEL", -1,
        "Remote character locked: epoch=%u char=%u palette=%u custom=%u forced=%u",
        p->epoch_id,
        p->character_id,
        p->palette,
        s_remotePaletteCustom ? 1 : 0,
        forced ? 1 : 0);
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Remote character locked: char=%u palette=%u custom=%u",
        p->character_id,
        p->palette,
        s_remotePaletteCustom ? 1 : 0);
}

void CharSelSync_OnRemoteStage(const StageSyncPayload* p) {
    if (!s_active || !p) {
        return;
    }
    if (!FrontendInputSync_IsCurrentEpochPhase(p->epoch_id,
                                               p->phase,
                                               PacketType::StageSync,
                                               "stage watchdog")) {
        return;
    }

    const StageWatchdogApplyResult applyResult = StageWatchdogTracker_Apply(&s_remoteStageTracker, p);
    if (applyResult == StageWatchdogApplyResult::Duplicate ||
        applyResult == StageWatchdogApplyResult::IgnoredOutOfOrder ||
        applyResult == StageWatchdogApplyResult::IgnoredRegression) {
        Rollback::NetplayLog_Verbose(
            "STAGESEL", -1,
            "Ignored StageSync watchdog: epoch=%u frame=%u stage=%u confirmed=%u result=%s",
            p->epoch_id,
            p->frame,
            p->stage_id,
            p->confirmed,
            StageWatchdogApplyResultName(applyResult));
        return;
    }

    const StageSyncPayload& accepted = s_remoteStageTracker.latest;

    s_remoteStageState.stage_id = accepted.stage_id;
    s_remoteStageState.stage_cursor = accepted.stage_cursor;
    s_remoteStageState.stage_confirmed = accepted.confirmed;
    s_remoteStageState.stage_counter = accepted.stage_counter;
    s_remoteStageState.stage_aux = accepted.stage_aux;
    s_remoteStageState.stage_cancel = accepted.stage_cancel;
    s_remoteStageState.confirm_menu_cursor = accepted.confirm_menu_cursor;
    s_remoteStageState.confirm_menu_action = accepted.confirm_menu_action;
    s_remoteStageState.committed_stage_id = accepted.committed_stage_id;
    s_remoteStageState.substate = accepted.substate;
    s_remoteStage = accepted.stage_id;
    if (accepted.confirmed) {
        s_remoteStageLocked = true;
    }

    MirrorHostStageStateOnClient(s_remoteStageState, StageWatchdogApplyResultName(applyResult));

    Rollback::NetplayLog_Verbose(
        "STAGESEL", -1,
        "Remote stage watchdog: epoch=%u frame=%u stage=%u confirmed=%u cursor=%u sub=%u result=%s",
        accepted.epoch_id,
        accepted.frame,
        accepted.stage_id,
        accepted.confirmed,
        accepted.stage_cursor,
        accepted.substate,
        StageWatchdogApplyResultName(applyResult));
}

void CharSelSync_GetSnapshot(CharSelSyncSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->active = s_active;
    out->in_stage_phase = s_inStagePhase;

    const uintptr_t localCursorAddr = s_isHost ? ADDR_CHARSEL_P1_CURSOR : ADDR_CHARSEL_P2_CURSOR;
    const uintptr_t remoteCursorAddr = s_isHost ? ADDR_CHARSEL_P2_CURSOR : ADDR_CHARSEL_P1_CURSOR;
    out->local_cursor = ReadMemory<uint8_t>(localCursorAddr);
    out->local_confirmed = s_localCharConfirmed ? 1 : 0;
    out->remote_cursor = ReadMemory<uint8_t>(remoteCursorAddr);
    out->remote_confirmed = s_remoteCharLocked ? 1 : 0;
    out->both_characters_locked = s_bothCharsLocked;

    if (s_isHost) {
        out->p1_character = s_localChar;
        out->p1_palette = s_localPalette;
        out->p2_character = s_remoteChar;
        out->p2_palette = s_remotePalette;
    } else {
        out->p1_character = s_remoteChar;
        out->p1_palette = s_remotePalette;
        out->p2_character = s_localChar;
        out->p2_palette = s_localPalette;
    }

    out->local_stage = s_localStage;
    out->local_stage_confirmed = s_localStageLocked;
    out->remote_stage = s_remoteStage;
    out->remote_stage_confirmed = s_remoteStageLocked;
    out->both_stage_locked = s_bothStageLocked;
    out->stage_id = s_finalStageId;

    out->lockstep_frame = FrontendInputSync_GetConsumeFrame();
    out->local_input_frame = FrontendInputSync_GetLocalInputFrame();
    out->input_delay = (int)FrontendInputSync_GetSharedDelay();
}

} // namespace Net
