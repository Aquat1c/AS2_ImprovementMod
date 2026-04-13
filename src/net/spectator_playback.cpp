#include "net/spectator_playback.h"

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "core/mod_main.h"
#include "input/input_system.h"
#include "net/mode_ownership.h"
#include "net/netplay_menu_controller.h"
#include "net/spectator_client.h"
#include "patches/charsel_palette_select.h"
#include "patches/input_sync_hooks.h"
#include "patches/memory_utils.h"
#include "patches/tick_hooks.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

constexpr uintptr_t ADDR_STAGE_AUX = 0x816028;
constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_CURSOR = 0x81602A;
constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_ACTION = 0x81602B;

constexpr int32_t kBootstrapStartBufferFrames = 60;
constexpr int32_t kCatchupEnter125Gap = 31;
constexpr int32_t kCatchupExit125Gap = 20;
constexpr int32_t kCatchupEnter150Gap = 91;
constexpr int32_t kCatchupExit150Gap = 75;
constexpr int32_t kCatchupEnter200Gap = 181;
constexpr int32_t kCatchupExit200Gap = 150;

static bool s_initialized = false;
static SpectatorPlaybackState s_state = SpectatorPlaybackState::Disconnected;
static uint32_t s_matchId = 0;
static uint32_t s_matchOrdinal = 0;
static uint32_t s_configCrc = 0;
static uint32_t s_sessionSeed = 0;
static bool s_launchIssued = false;
static int32_t s_localFrameOriginAbs = -1;
static int32_t s_localPlaybackRbFrame = -1;
static int32_t s_confirmedEdgeRbFrame = -1;
static int32_t s_liveEdgeRbFrame = -1;
static float s_targetCatchupScale = 1.0f;
static char s_status[128] = "Spectator playback idle.";

static void CopyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    strncpy_s(dst, dstSize, src, _TRUNCATE);
}

static void FormatStatus(char* out, size_t outSize, const char* fmt, va_list ap) {
    if (!out || outSize == 0) {
        return;
    }

    _vsnprintf_s(out, outSize, _TRUNCATE, fmt, ap);
}

static void SetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    FormatStatus(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

static const char* StateNameInternal(SpectatorPlaybackState state) {
    switch (state) {
        case SpectatorPlaybackState::Disconnected: return "Disconnected";
        case SpectatorPlaybackState::Connecting: return "Connecting";
        case SpectatorPlaybackState::ConnectedWaitingMetadata: return "ConnectedWaitingMetadata";
        case SpectatorPlaybackState::ConnectedNoActiveMatch: return "ConnectedNoActiveMatch";
        case SpectatorPlaybackState::WaitingFullArchive: return "WaitingFullArchive";
        case SpectatorPlaybackState::ReadyToBootstrap: return "ReadyToBootstrap";
        case SpectatorPlaybackState::BootstrappingFrontend: return "BootstrappingFrontend";
        case SpectatorPlaybackState::WaitingInteractiveStart: return "WaitingInteractiveStart";
        case SpectatorPlaybackState::Buffering: return "Buffering";
        case SpectatorPlaybackState::CatchingUp: return "CatchingUp";
        case SpectatorPlaybackState::Live: return "Live";
        case SpectatorPlaybackState::EndOfMatch: return "EndOfMatch";
        case SpectatorPlaybackState::WaitingNextMatch: return "WaitingNextMatch";
        case SpectatorPlaybackState::PlaybackError: return "PlaybackError";
        default: return "Unknown";
    }
}

static void TransitionState(SpectatorPlaybackState nextState, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    FormatStatus(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);

    if (nextState != s_state) {
        Rollback::NetplayLog_Write(
            "SPLAY",
            s_localPlaybackRbFrame,
            "State %s -> %s match=0x%08X/%u cfg=0x%08X seed=0x%08X status=%s",
            StateNameInternal(s_state),
            StateNameInternal(nextState),
            s_matchId,
            s_matchOrdinal,
            s_configCrc,
            s_sessionSeed,
            s_status);
    }

    s_state = nextState;
}

static bool IsBootstrappingState(SpectatorPlaybackState state) {
    return state == SpectatorPlaybackState::ReadyToBootstrap ||
           state == SpectatorPlaybackState::BootstrappingFrontend ||
           state == SpectatorPlaybackState::WaitingInteractiveStart;
}

static bool OwnsLocalSimulation() {
    return s_launchIssued ||
           s_localFrameOriginAbs >= 0 ||
           IsBootstrappingState(s_state) ||
           s_state == SpectatorPlaybackState::Buffering ||
           s_state == SpectatorPlaybackState::CatchingUp ||
           s_state == SpectatorPlaybackState::Live ||
           s_state == SpectatorPlaybackState::EndOfMatch;
}

static void ResetTickPacing() {
    s_targetCatchupScale = 1.0f;
    SetNetplayTickScale(1.0f);
    SetNetplayTickScaleTarget(1.0f);
    SetNetplayPacingActive(false);
}

static void ClearPlaybackOverrides() {
    InputSystem_ClearOverride(0);
    InputSystem_ClearOverride(1);
}

static void ResetLocalSimulationState() {
    ClearPlaybackOverrides();
    InputSyncHooks_SetTimesyncFreeze(false);
    ResetTickPacing();
    s_launchIssued = false;
    s_localFrameOriginAbs = -1;
    s_localPlaybackRbFrame = -1;
}

static void ClearTrackedIdentity() {
    s_matchId = 0;
    s_matchOrdinal = 0;
    s_configCrc = 0;
    s_sessionSeed = 0;
    s_confirmedEdgeRbFrame = -1;
    s_liveEdgeRbFrame = -1;
}

static void EnterSafeMenuIfNeeded() {
    if (OwnsLocalSimulation() && GetGameMode() != MODE_MENU) {
        ModeOwnership::EnterCustomMenuContext();
    }
}

static bool HasTrackedIdentity() {
    return s_matchId != 0 ||
           s_matchOrdinal != 0 ||
           s_configCrc != 0 ||
           s_sessionSeed != 0;
}

static void SyncStreamEdges(const SpectatorClientSnapshot& client) {
    s_confirmedEdgeRbFrame = client.confirmed_contiguous_rb_frame;
    s_liveEdgeRbFrame = client.server_live_rb_frame;
}

static void AdoptTrackedIdentity(const SpectatorClientSnapshot& client) {
    s_matchId = client.match_id;
    s_matchOrdinal = client.match_ordinal;
    s_configCrc = client.config_crc;
    s_sessionSeed = client.session_seed;
    SyncStreamEdges(client);
    Rollback::NetplayLog_Write(
        "SPLAY",
        -1,
        "Adopt stream match=0x%08X/%u cfg=0x%08X seed=0x%08X buffered_start=%d confirmed_edge=%d",
        s_matchId,
        s_matchOrdinal,
        s_configCrc,
        s_sessionSeed,
        client.buffered_start_rb_frame,
        client.confirmed_contiguous_rb_frame);
}

static bool StreamIdentityChanged(const SpectatorClientSnapshot& client) {
    if (!HasTrackedIdentity()) {
        return false;
    }

    return client.match_id != s_matchId ||
           client.match_ordinal != s_matchOrdinal ||
           client.config_crc != s_configCrc ||
           client.session_seed != s_sessionSeed;
}

static float ComputeCatchupScale(int32_t gap) {
    if (s_targetCatchupScale >= 2.0f - 0.01f) {
        if (gap >= kCatchupExit200Gap) {
            return 2.0f;
        }
    }

    if (s_targetCatchupScale >= 1.5f - 0.01f) {
        if (gap >= kCatchupEnter200Gap) {
            return 2.0f;
        }
        if (gap >= kCatchupExit150Gap) {
            return 1.5f;
        }
    }

    if (s_targetCatchupScale >= 1.25f - 0.01f) {
        if (gap >= kCatchupEnter200Gap) {
            return 2.0f;
        }
        if (gap >= kCatchupEnter150Gap) {
            return 1.5f;
        }
        if (gap >= kCatchupExit125Gap) {
            return 1.25f;
        }
    }

    if (gap >= kCatchupEnter200Gap) {
        return 2.0f;
    }
    if (gap >= kCatchupEnter150Gap) {
        return 1.5f;
    }
    if (gap >= kCatchupEnter125Gap) {
        return 1.25f;
    }
    return 1.0f;
}

static void ApplyCatchupScale(float targetScale, int32_t gap) {
    if (targetScale != s_targetCatchupScale) {
        Rollback::NetplayLog_Write(
            "SPLAY",
            s_localPlaybackRbFrame,
            "Catch-up target %.2fx -> %.2fx gap=%d confirmed_edge=%d live_edge=%d",
            s_targetCatchupScale,
            targetScale,
            gap,
            s_confirmedEdgeRbFrame,
            s_liveEdgeRbFrame);
    }

    s_targetCatchupScale = targetScale;
    SetNetplayTickScaleTarget(targetScale);
    SetNetplayPacingActive(targetScale > 1.0f);
}

static bool BootstrapReady(const SpectatorClientSnapshot& client, bool* outWaitNextMatch) {
    if (outWaitNextMatch) {
        *outWaitNextMatch = false;
    }

    if (!client.have_match_state || client.match_id == 0 || client.config_crc == 0) {
        return false;
    }

    if (client.buffered_start_rb_frame > 0) {
        if (outWaitNextMatch) {
            *outWaitNextMatch = true;
        }
        return false;
    }

    return client.confirmed_contiguous_rb_frame >= (kBootstrapStartBufferFrames - 1);
}

static void BeginLocalSpectatorLaunch(const SpectatorClientSnapshot& client) {
    NetMenu::HideForLaunch("spectator playback");
    ModeOwnership::SetPendingMenuRestore(false);

    WriteMemory<uint32_t>(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ModeOwnership::ClearVanillaNetplayFlags();
    WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 0);
    WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 0);
    WriteMemory<uint8_t>(ADDR_STAGESEL_ENABLE, 1);
    ModeOwnership::CallOriginalSetGameMode(MODE_CHARSEL, 1);
    ModeOwnership::ResetCharSelFields();
    DetVer_SetRngSeed(client.config.session_seed);

    s_launchIssued = true;
    SetStatus("Launching spectator playback for G%u.",
        client.match_ordinal != 0 ? client.match_ordinal : 1);
}

static void UpdateLivePlayback(const SpectatorClientSnapshot& client) {
    if (GetGameMode() != MODE_MATCH || s_localFrameOriginAbs < 0) {
        ResetLocalSimulationState();
        TransitionState(SpectatorPlaybackState::ReadyToBootstrap,
            "Local spectator simulation left gameplay; rebootstrap required.");
        return;
    }

    const int32_t currentAbs = (int32_t)AS2_GetFrameNumber();
    const int32_t targetRbFrame = currentAbs - s_localFrameOriginAbs;
    s_localPlaybackRbFrame = targetRbFrame;
    SyncStreamEdges(client);

    if (targetRbFrame > client.confirmed_contiguous_rb_frame) {
        ClearPlaybackOverrides();
        InputSyncHooks_SetTimesyncFreeze(true);
        ApplyCatchupScale(1.0f, 0);
        TransitionState(SpectatorPlaybackState::Buffering,
            "Buffering confirmed frame %d (edge=%d).",
            targetRbFrame,
            client.confirmed_contiguous_rb_frame);
        return;
    }

    uint16_t p1Input = 0;
    uint16_t p2Input = 0;
    if (!SpectatorClient_GetFrameInputs(targetRbFrame, &p1Input, &p2Input)) {
        ClearPlaybackOverrides();
        InputSyncHooks_SetTimesyncFreeze(true);
        ApplyCatchupScale(1.0f, 0);
        TransitionState(SpectatorPlaybackState::Buffering,
            "Waiting for contiguous confirmed frame %d.",
            targetRbFrame);
        return;
    }

    InputSyncHooks_SetTimesyncFreeze(false);
    InputSystem_SetOverride(0, p1Input);
    InputSystem_SetOverride(1, p2Input);

    const int32_t gap = client.confirmed_contiguous_rb_frame - targetRbFrame;
    const float targetScale = ComputeCatchupScale(gap);
    ApplyCatchupScale(targetScale, gap);

    if (targetScale > 1.0f) {
        TransitionState(SpectatorPlaybackState::CatchingUp,
            "Catch-up gap=%d edge=%d scale=%.2fx.",
            gap,
            client.confirmed_contiguous_rb_frame,
            targetScale);
    } else {
        TransitionState(SpectatorPlaybackState::Live,
            "Live playback local=%d edge=%d.",
            targetRbFrame,
            client.confirmed_contiguous_rb_frame);
    }
}

static void DriveBootstrap(const SpectatorClientSnapshot& client) {
    if (!s_launchIssued) {
        BeginLocalSpectatorLaunch(client);
    }

    const uint32_t mode = GetGameMode();
    const uint32_t sub = GetSubstate();

    DetVer_SetRngSeed(client.config.session_seed);

    if (mode == MODE_MENU) {
        TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
            "Launching local spectator match.");
        return;
    }

    if (mode == MODE_CHARSEL) {
        CharSelPaletteSelect_ForceSelectionLocked(0,
            client.config.p1_character,
            client.config.p1_palette,
            false);
        CharSelPaletteSelect_ForceSelectionLocked(1,
            client.config.p2_character,
            client.config.p2_palette,
            false);

        WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, client.config.stage_id);

        if (sub == CHARSEL_SUB_STAGESEL_GRID) {
            WriteMemory<uint8_t>(ADDR_STAGE_CURSOR, client.config.stage_id);
            WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 1, 0);
            WriteMemory<uint8_t>(ADDR_STAGE_AUX, 0);
            InputSystem_SetOverride(0, INPUT_A);
            InputSystem_SetOverride(1, 0);
            TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
                "Selecting stage %u for spectator playback.",
                (unsigned)client.config.stage_id);
            return;
        }

        if (sub == CHARSEL_SUB_STAGESEL_CONFIRM) {
            WriteMemory<uint8_t>(ADDR_STAGE_CURSOR, client.config.stage_id);
            WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, client.config.stage_id);
            WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR, 0);
            WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION, 0);
            InputSystem_SetOverride(0, INPUT_A);
            InputSystem_SetOverride(1, 0);
            TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
                "Confirming spectator stage selection.");
            return;
        }

        ClearPlaybackOverrides();
        TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
            "Locking spectator selections.");
        return;
    }

    ClearPlaybackOverrides();

    if (mode == MODE_PREMATCH_INTRO || (mode == MODE_MATCH && !AS2_IsInPlayableGameplay())) {
        TransitionState(SpectatorPlaybackState::WaitingInteractiveStart,
            "Waiting for local interactive start.");
        return;
    }

    if (mode == MODE_MATCH && AS2_IsInPlayableGameplay()) {
        if (s_localFrameOriginAbs < 0) {
            s_localFrameOriginAbs = (int32_t)AS2_GetFrameNumber();
            s_localPlaybackRbFrame = 0;
            Rollback::NetplayLog_Write(
                "SPLAY",
                0,
                "Interactive boundary reached: abs_origin=%d match=0x%08X/%u",
                s_localFrameOriginAbs,
                s_matchId,
                s_matchOrdinal);
        }
        UpdateLivePlayback(client);
        return;
    }

    TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
        "Waiting for spectator launch path (mode=%u sub=%u).",
        mode,
        sub);
}

static void ResetPlayback(const char* reason) {
    ResetLocalSimulationState();
    ClearTrackedIdentity();
    TransitionState(SpectatorPlaybackState::Disconnected,
        "%s",
        reason && reason[0] ? reason : "Spectator playback idle.");
}

} // namespace

namespace Net {

const char* SpectatorPlaybackStateName(SpectatorPlaybackState state) {
    return StateNameInternal(state);
}

void SpectatorPlayback_Init() {
    if (s_initialized) {
        return;
    }

    s_initialized = true;
    ResetPlayback("Spectator playback idle.");
}

void SpectatorPlayback_Shutdown() {
    if (!s_initialized) {
        return;
    }

    ResetPlayback("Spectator playback shutdown.");
    s_initialized = false;
}

void SpectatorPlayback_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    SpectatorClientSnapshot client{};
    SpectatorClient_GetSnapshot(&client);
    SyncStreamEdges(client);

    if (!client.active || client.state == SpectatorClientState::Idle) {
        if (s_state != SpectatorPlaybackState::Disconnected) {
            EnterSafeMenuIfNeeded();
            ResetPlayback("Spectator playback idle.");
        }
        return;
    }

    if (client.state == SpectatorClientState::Failed) {
        EnterSafeMenuIfNeeded();
        ResetLocalSimulationState();
        ClearTrackedIdentity();
        TransitionState(SpectatorPlaybackState::PlaybackError,
            "%s",
            client.error[0] ? client.error : "Spectator playback failed.");
        return;
    }

    if (client.state == SpectatorClientState::Connecting ||
        client.state == SpectatorClientState::Handshaking ||
        client.state == SpectatorClientState::Redirected) {
        EnterSafeMenuIfNeeded();
        ResetLocalSimulationState();
        ClearTrackedIdentity();
        TransitionState(SpectatorPlaybackState::Connecting,
            "%s",
            client.status[0] ? client.status : "Connecting to spectator stream.");
        return;
    }

    if (client.state == SpectatorClientState::ConnectedNoActiveMatch) {
        EnterSafeMenuIfNeeded();
        ResetLocalSimulationState();
        ClearTrackedIdentity();
        TransitionState(SpectatorPlaybackState::ConnectedNoActiveMatch,
            "%s",
            client.status[0] ? client.status : "Connected; waiting for active match.");
        return;
    }

    if (!client.have_match_state || client.match_id == 0 || client.config_crc == 0) {
        ResetLocalSimulationState();
        ClearTrackedIdentity();
        TransitionState(SpectatorPlaybackState::ConnectedWaitingMetadata,
            "Connected; waiting for stream metadata.");
        return;
    }

    if (!HasTrackedIdentity()) {
        AdoptTrackedIdentity(client);
        const bool unsupportedMidMatch = client.buffered_start_rb_frame > 0;
        TransitionState(
            unsupportedMidMatch
                ? SpectatorPlaybackState::WaitingNextMatch
                : SpectatorPlaybackState::WaitingFullArchive,
            unsupportedMidMatch
                ? "Current stream starts at rb frame %d; waiting for next full match."
                : "Waiting for confirmed archive from rb frame 0.",
            client.buffered_start_rb_frame);
        return;
    }

    if (StreamIdentityChanged(client)) {
        Rollback::NetplayLog_Write(
            "SPLAY",
            s_localPlaybackRbFrame,
            "Stream switch old=0x%08X/%u cfg=0x%08X seed=0x%08X new=0x%08X/%u cfg=0x%08X seed=0x%08X",
            s_matchId,
            s_matchOrdinal,
            s_configCrc,
            s_sessionSeed,
            client.match_id,
            client.match_ordinal,
            client.config_crc,
            client.session_seed);
        EnterSafeMenuIfNeeded();
        ResetLocalSimulationState();
        AdoptTrackedIdentity(client);
        const bool unsupportedMidMatch = client.buffered_start_rb_frame > 0;
        TransitionState(
            unsupportedMidMatch
                ? SpectatorPlaybackState::WaitingNextMatch
                : SpectatorPlaybackState::WaitingFullArchive,
            unsupportedMidMatch
                ? "Switched to stream starting at rb frame %d; waiting for next full match."
                : "Switched spectator stream; rebuilding from frame 0 archive.",
            client.buffered_start_rb_frame);
        return;
    }

    if (!client.match_active) {
        ClearPlaybackOverrides();
        InputSyncHooks_SetTimesyncFreeze(false);
        ApplyCatchupScale(1.0f, 0);
        if (OwnsLocalSimulation() &&
            GetGameMode() != MODE_MENU &&
            GetGameMode() != MODE_CHARSEL) {
            TransitionState(SpectatorPlaybackState::EndOfMatch,
                "Match ended; waiting for local frontend to unwind.");
            return;
        }

        ResetLocalSimulationState();
        TransitionState(SpectatorPlaybackState::WaitingNextMatch,
            "Waiting for next match archive.");
        return;
    }

    bool waitNextMatch = false;
    if (!BootstrapReady(client, &waitNextMatch)) {
        ResetLocalSimulationState();
        if (waitNextMatch) {
            TransitionState(SpectatorPlaybackState::WaitingNextMatch,
                "Current stream started at rb frame %d; waiting for next full match.",
                client.buffered_start_rb_frame);
        } else {
            TransitionState(SpectatorPlaybackState::WaitingFullArchive,
                "Waiting for bootstrap buffer: confirmed edge %d / need %d.",
                client.confirmed_contiguous_rb_frame,
                kBootstrapStartBufferFrames - 1);
        }
        return;
    }

    if (!OwnsLocalSimulation() &&
        s_state != SpectatorPlaybackState::ReadyToBootstrap) {
        TransitionState(SpectatorPlaybackState::ReadyToBootstrap,
            "Ready to bootstrap from confirmed edge %d.",
            client.confirmed_contiguous_rb_frame);
        return;
    }

    if (s_state == SpectatorPlaybackState::ReadyToBootstrap) {
        BeginLocalSpectatorLaunch(client);
        TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
            "Launching spectator playback for G%u.",
            client.match_ordinal != 0 ? client.match_ordinal : 1);
        return;
    }

    DriveBootstrap(client);
}

void SpectatorPlayback_GetSnapshot(SpectatorPlaybackSnapshot* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->active = s_state != SpectatorPlaybackState::Disconnected;
    out->gameplay_owned = OwnsLocalSimulation();
    out->bootstrapping = IsBootstrappingState(s_state);
    out->playing = s_state == SpectatorPlaybackState::Buffering ||
        s_state == SpectatorPlaybackState::CatchingUp ||
        s_state == SpectatorPlaybackState::Live;
    out->waiting_for_frame = s_state == SpectatorPlaybackState::Buffering;
    out->catchup_active = s_state == SpectatorPlaybackState::CatchingUp;
    out->error = s_state == SpectatorPlaybackState::PlaybackError;
    out->state = s_state;
    out->match_id = s_matchId;
    out->match_ordinal = s_matchOrdinal;
    out->config_crc = s_configCrc;
    out->session_seed = s_sessionSeed;
    out->local_playback_rb_frame = s_localPlaybackRbFrame;
    out->confirmed_edge_rb_frame = s_confirmedEdgeRbFrame;
    out->live_edge_rb_frame = s_liveEdgeRbFrame;
    out->tick_scale_target = s_targetCatchupScale;
    CopyText(out->state_label, sizeof(out->state_label), StateNameInternal(s_state));
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net