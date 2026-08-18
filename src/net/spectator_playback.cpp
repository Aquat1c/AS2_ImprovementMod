#include "net/spectator_playback.h"

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "core/mod_main.h"
#include "input/input_system.h"
#include "net/game_settings_sync.h"
#include "net/mode_ownership.h"
#include "net/netplay_menu_controller.h"
#include "net/spectator_client.h"
#include "net/spectator_playback_policy.h"
#include "patches/charsel_palette_select.h"
#include "patches/input_sync_hooks.h"
#include "net/netplay_palette_runtime.h"
#include "patches/memory_utils.h"
#include "patches/frame_scheduler.h"
#include "rollback/determinism_verify.h"
#include "rollback/game_snapshot.h"
#include "rollback/netplay_log.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

#define SPLAY_LOG(frame, fmt, ...) \
    Rollback::NetplayLog_WriteSpectator("SPLAY", frame, fmt, ##__VA_ARGS__)

constexpr uintptr_t ADDR_STAGE_AUX = 0x816028;
constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_CURSOR = 0x81602A;
constexpr uintptr_t ADDR_STAGE_CONFIRM_MENU_ACTION = 0x81602B;

// Minimum confirmed frames before starting bootstrap on the normal (non-early)
// path. The early bootstrap (PreMatchState) bypasses this entirely.
// 10 frames is enough — frames continue arriving while bootstrap runs.
constexpr int32_t kBootstrapStartBufferFrames = 10;
// How many frames to wait before retrying a stalled stage-grid or stage-confirm
// write. 120 (2s) was a worst-case latency hedge; 30 (0.5s) is responsive enough
// while still giving the game state machine time to settle between retries.
constexpr uint32_t kBootstrapRetryFrames = 30;
constexpr uint32_t kBootstrapStateLogFrames = 120;
constexpr float kManualCatchupScaleSteps[] = {
    0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f
};
constexpr int kHotkeySpectatorSpeedSlower = VK_OEM_MINUS;
constexpr int kHotkeySpectatorSpeedFaster = VK_OEM_PLUS;

static bool s_initialized = false;
static SpectatorPlaybackState s_state = SpectatorPlaybackState::Disconnected;
static uint32_t s_matchId = 0;
static uint32_t s_matchOrdinal = 0;
static uint32_t s_configCrc = 0;
static uint32_t s_sessionSeed = 0;
static bool s_launchIssued = false;
// Survives ResetLocalSimulationState, unlike s_launchIssued. It answers "is
// there a local game context to re-enter", which is NOT the same question as
// "is a launch currently in flight" -- s_launchIssued is cleared on the
// WaitingNextMatch -> ConnectedWaitingMetadata transition, so using it as the
// not-the-first-match test silently disabled the re-entry for every match.
static bool s_localMatchEverLaunched = false;
static int32_t s_localFrameOriginAbs = -1;
static int32_t s_localPlaybackRbFrame = -1;
static int32_t s_nextDispatchRbFrame = -1;
static int32_t s_dispatchFrameBudget = 0;
static int32_t s_dispatchFramesProducedThisLoop = 0;
static uint16_t s_dispatchPrevP1 = 0;
static uint16_t s_dispatchPrevP2 = 0;
static int32_t s_confirmedEdgeRbFrame = -1;
static int32_t s_liveEdgeRbFrame = -1;
static float s_targetCatchupScale = 1.0f;
static int s_manualCatchupScaleIndex = 0;
static char s_status[128] = "Watch playback idle.";
static bool s_speedSlowerKeyWasDown = false;
static bool s_speedFasterKeyWasDown = false;
static bool s_matchSettingsApplied = false;
static uint32_t s_lastBootstrapMode = 0xFFFFFFFFu;
static uint32_t s_lastBootstrapSub = 0xFFFFFFFFu;
static uint32_t s_bootstrapSubFrames = 0;

// ── M7 §2.8.8 elastic pacing latches ────────────────────────────────────────
static bool s_playbackPrimed = false;       // startup prime reached once
static bool s_rebuffering = false;          // underrun → resume at 120, not 240
static bool s_catchingUp = false;           // catch-up hysteresis latch
static uint32_t s_elasticCredit = 0;        // Bresenham cadence credit
static uint32_t s_elasticHoldCount = 0;
static int64_t s_sliceStartQpc = 0;         // 12 ms catch-up wall slice anchor
static int64_t s_qpcFrequency = 0;

// ── M7 S-4 record hash verification latches ─────────────────────────────────
static bool s_hvDisabled = false;
static bool s_hvAcquired = false;
static uint32_t s_hvEntryMismatches = 0;
static uint32_t s_hvChecks = 0;
static bool s_hvFailLatch = false;          // consumed by FrameUpdate → leave
static char s_hvFailReason[128] = "";

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

static uint8_t ConfigCharacterForSlot(const SpectatorClientSnapshot& client, uint8_t gameSlot) {
    return gameSlot == 0 ? client.config.p1_character : client.config.p2_character;
}

static uint8_t ConfigBasePaletteForSlot(const SpectatorClientSnapshot& client, uint8_t gameSlot) {
    return gameSlot == 0 ? client.config.p1_palette : client.config.p2_palette;
}

static void ClearSpectatorPaletteHints() {
    CharSelPaletteSelect_ClearExternalCustomHints();
}

static float GetManualCatchupScale() {
    return kManualCatchupScaleSteps[s_manualCatchupScaleIndex];
}

static bool HasManualCatchupOverride() {
    return GetManualCatchupScale() > 0.0f;
}

static bool RawKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool IsGameWindowFocused() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }

    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return foregroundPid == GetCurrentProcessId();
}

static bool ConsumeHotkeyEdge(int vk, bool* wasDown) {
    const bool down = RawKeyDown(vk);
    const bool pressed = IsGameWindowFocused() && down && !*wasDown;
    *wasDown = down;
    return pressed;
}

static void ResetSpectatorHotkeyEdges() {
    s_speedSlowerKeyWasDown = RawKeyDown(kHotkeySpectatorSpeedSlower);
    s_speedFasterKeyWasDown = RawKeyDown(kHotkeySpectatorSpeedFaster);
}

static void ResetBootstrapDriveState() {
    s_lastBootstrapMode = 0xFFFFFFFFu;
    s_lastBootstrapSub = 0xFFFFFFFFu;
    s_bootstrapSubFrames = 0;
}

static void SetLastCompletedPlaybackFrame(int32_t rbFrame) {
    s_localPlaybackRbFrame = rbFrame;
    SpectatorClient_SetPlaybackFrame(rbFrame);
}

static void ResetDispatchState() {
    s_nextDispatchRbFrame = -1;
    s_dispatchFrameBudget = 0;
    s_dispatchFramesProducedThisLoop = 0;
    s_dispatchPrevP1 = 0;
    s_dispatchPrevP2 = 0;
    SpectatorClient_SetPlaybackFrame(-1);
    SpectatorClient_SetFastForwardEnabled(false);
    SpectatorClient_SetHardSyncEnabled(false);
}

static int32_t GetObservedNextRbFrame() {
    if (s_localFrameOriginAbs < 0) {
        return -1;
    }

    const int32_t observedNextRbFrame =
        (int32_t)AS2_GetFrameNumber() - s_localFrameOriginAbs;
    return observedNextRbFrame >= 0 ? observedNextRbFrame : -1;
}

static void SyncObservedDispatchFrame() {
    const int32_t observedNextRbFrame = GetObservedNextRbFrame();
    if (observedNextRbFrame < 0) {
        return;
    }

    if (s_nextDispatchRbFrame < 0) {
        s_nextDispatchRbFrame = observedNextRbFrame;
        SetLastCompletedPlaybackFrame(observedNextRbFrame - 1);
        return;
    }

    if (observedNextRbFrame > s_nextDispatchRbFrame) {
        SPLAY_LOG(
            s_localPlaybackRbFrame,
            "Observed playback drift expected_next=%d observed_next=%d last_playback=%d",
            s_nextDispatchRbFrame,
            observedNextRbFrame,
            s_localPlaybackRbFrame);
        s_nextDispatchRbFrame = observedNextRbFrame;
        SetLastCompletedPlaybackFrame(observedNextRbFrame - 1);
        s_dispatchFramesProducedThisLoop = 0;
    }
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
    char previousStatus[sizeof(s_status)] = {};
    strncpy_s(previousStatus, sizeof(previousStatus), s_status, _TRUNCATE);

    va_list ap;
    va_start(ap, fmt);
    FormatStatus(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);

    const bool stateChanged = nextState != s_state;
    const bool statusChanged = strcmp(previousStatus, s_status) != 0;

    if (stateChanged) {
        SPLAY_LOG(
            s_localPlaybackRbFrame,
            "State %s -> %s match=0x%08X/%u cfg=0x%08X seed=0x%08X status=%s",
            StateNameInternal(s_state),
            StateNameInternal(nextState),
            s_matchId,
            s_matchOrdinal,
            s_configCrc,
            s_sessionSeed,
            s_status);
    } else if (statusChanged) {
        SPLAY_LOG(
            s_localPlaybackRbFrame,
            "State %s status update match=0x%08X/%u cfg=0x%08X seed=0x%08X status=%s",
            StateNameInternal(s_state),
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

static void ChangeManualCatchupScale(int delta) {
    const int scaleCount = (int)(sizeof(kManualCatchupScaleSteps) / sizeof(kManualCatchupScaleSteps[0]));
    int nextIndex = s_manualCatchupScaleIndex + delta;
    if (nextIndex < 0) {
        nextIndex = 0;
    } else if (nextIndex >= scaleCount) {
        nextIndex = scaleCount - 1;
    }

    if (nextIndex == s_manualCatchupScaleIndex) {
        return;
    }

    s_manualCatchupScaleIndex = nextIndex;
    if (HasManualCatchupOverride()) {
        SPLAY_LOG(
            s_localPlaybackRbFrame,
            "Manual spectator catch-up budget cap set to %.0fx",
            GetManualCatchupScale());
    } else {
        SPLAY_LOG(
            s_localPlaybackRbFrame,
            "Manual spectator catch-up budget cap reset to auto");
    }
}

static void HandleSpectatorHotkeys() {
    if (ConsumeHotkeyEdge(kHotkeySpectatorSpeedFaster, &s_speedFasterKeyWasDown)) {
        ChangeManualCatchupScale(+1);
    }

    if (ConsumeHotkeyEdge(kHotkeySpectatorSpeedSlower, &s_speedSlowerKeyWasDown)) {
        ChangeManualCatchupScale(-1);
    }
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
    // The netplay tick-scale writers were deleted at M2 (INV-5: one speed
    // authority). Spectator playback only ever wrote neutral values here;
    // clearing the scheduler's period adjust keeps that contract.
    s_targetCatchupScale = 1.0f;
    FrameScheduler_SetPeriodAdjustUs(0.0f, "spectator_playback_reset");
}

static void ClearPlaybackOverrides() {
    InputSystem_ClearOverride(0);
    InputSystem_ClearOverride(1);
}

static void ApplySpectatorMatchSettings(const LockedMatchConfig& config, const char* reason) {
    if (!s_matchSettingsApplied) {
        GameSettingsSync_BeginNetplaySession(
            reason ? reason : "spectator match settings");
        s_matchSettingsApplied = true;
    }

    GameSettingsSync_ApplyLockedConfig(
        &config,
        reason ? reason : "spectator match settings");
    SPLAY_LOG(
        s_localPlaybackRbFrame,
        "Applied spectator match settings: rounds_raw=%u rounds_to_win=%d reason=%s",
        config.round_count,
        GameSettingsSync_RoundsToWin(config.round_count),
        reason ? reason : "?");
}

static void RestoreSpectatorMatchSettings(const char* reason) {
    if (!s_matchSettingsApplied) {
        return;
    }

    GameSettingsSync_RestoreLocalSession(
        reason ? reason : "spectator playback reset");
    s_matchSettingsApplied = false;
    SPLAY_LOG(
        s_localPlaybackRbFrame,
        "Restored local settings after spectator playback: reason=%s",
        reason ? reason : "?");
}

static void ClearLocalMatchHistory(const char* why) {
    if (s_localMatchEverLaunched) {
        SPLAY_LOG(-1, "Local match history cleared (%s): next launch is a full bootstrap",
                  why ? why : "?");
    }
    s_localMatchEverLaunched = false;
}

static void ResetLocalSimulationState() {
    ClearPlaybackOverrides();
    RestoreSpectatorMatchSettings("spectator local simulation reset");
    InputSyncHooks_SetTimesyncFreeze(false);
    ResetTickPacing();
    s_launchIssued = false;
    s_localFrameOriginAbs = -1;
    s_localPlaybackRbFrame = -1;
    ResetDispatchState();
    ResetBootstrapDriveState();
    // M7 pacing + verification latches restart with the local simulation.
    s_playbackPrimed = false;
    s_rebuffering = false;
    s_catchingUp = false;
    s_elasticCredit = 0;
    s_elasticHoldCount = 0;
    s_hvDisabled = false;
    s_hvAcquired = false;
    s_hvEntryMismatches = 0;
    s_hvChecks = 0;
    s_hvFailLatch = false;
    s_hvFailReason[0] = '\0';
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
        SPLAY_LOG(
            s_localPlaybackRbFrame,
            "Returning spectator playback to menu mode=%u sub=%u",
            GetGameMode(),
            GetSubstate());
        NetMenu::ShowMenuAfterExternalLaunch("Returning to custom netplay menu.");
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
    SPLAY_LOG(
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

static bool TryResolveBufferedSpectatorPalette(const SpectatorClientSnapshot& client,
                                              uint8_t gameSlot,
                                              bool* outUseCustom,
                                              const char** outWaitReason) {
    if (outUseCustom) {
        *outUseCustom = false;
    }
    if (outWaitReason) {
        *outWaitReason = "palette metadata";
    }

    SpectatorBufferedPaletteSlot palette{};
    if (!SpectatorClient_GetBufferedPaletteSlot(gameSlot, &palette) || !palette.metadata_valid) {
        return false;
    }

    const uint8_t expectedCharacter = ConfigCharacterForSlot(client, gameSlot);
    const uint8_t expectedBasePalette = ConfigBasePaletteForSlot(client, gameSlot);
    if (palette.character_id != expectedCharacter ||
        palette.base_palette != expectedBasePalette) {
        if (outWaitReason) {
            *outWaitReason = "final palette selection";
        }
        return false;
    }

    if (!palette.has_custom_data) {
        if (outWaitReason) {
            *outWaitReason = nullptr;
        }
        return true;
    }

    if (!palette.bank_valid) {
        if (outWaitReason) {
            *outWaitReason = "custom palette data";
        }
        return false;
    }

    if (outUseCustom) {
        *outUseCustom = true;
    }
    if (outWaitReason) {
        *outWaitReason = nullptr;
    }
    return true;
}

static void UpdateSpectatorPaletteHints(const SpectatorClientSnapshot& client) {
    for (uint8_t gameSlot = 0; gameSlot < 2; ++gameSlot) {
        bool useCustom = false;
        if (TryResolveBufferedSpectatorPalette(client, gameSlot, &useCustom, nullptr) && useCustom) {
            CharSelPaletteSelect_SetExternalCustomHint(gameSlot,
                ConfigCharacterForSlot(client, gameSlot),
                ConfigBasePaletteForSlot(client, gameSlot),
                true);
        } else {
            CharSelPaletteSelect_SetExternalCustomHint(gameSlot,
                ConfigCharacterForSlot(client, gameSlot),
                ConfigBasePaletteForSlot(client, gameSlot),
                false);
        }
    }
}

static int64_t NowQpcMicros() {
    if (s_qpcFrequency == 0) {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        s_qpcFrequency = freq.QuadPart != 0 ? freq.QuadPart : 1;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart * 1000000LL) / s_qpcFrequency);
}

static void ApplyCatchupScale(float targetScale, int32_t gap) {
    if (targetScale != s_targetCatchupScale) {
        SPLAY_LOG(
            s_localPlaybackRbFrame,
            "Catch-up budget %.0fx -> %.0fx gap=%d confirmed_edge=%d live_edge=%d",
            s_targetCatchupScale,
            targetScale,
            gap,
            s_confirmedEdgeRbFrame,
            s_liveEdgeRbFrame);
    }

    s_targetCatchupScale = targetScale;
    char catchupReason[96];
    _snprintf_s(
        catchupReason,
        sizeof(catchupReason),
        _TRUNCATE,
        "spectator_catchup target=%.0fx gap=%d",
        targetScale,
        gap);
    // Catch-up is realized through batched dispatcher ticks, never through the
    // clock; the scheduler period stays neutral (writers deleted at M2).
    FrameScheduler_SetPeriodAdjustUs(0.0f, catchupReason);
}

static bool BootstrapReady(const SpectatorClientSnapshot& client,
                          bool* outWaitNextMatch,
                          uint8_t* outPaletteWaitSlot,
                          const char** outPaletteWaitReason) {
    if (outWaitNextMatch) {
        *outWaitNextMatch = false;
    }
    if (outPaletteWaitSlot) {
        *outPaletteWaitSlot = 0xFF;
    }
    if (outPaletteWaitReason) {
        *outPaletteWaitReason = nullptr;
    }

    // Early bootstrap path: PreMatchState arrived before MatchState/archive.
    // The selection is committed and the players are now on the loading screen.
    // Start charsel bootstrap immediately — it runs in parallel with loading so
    // the spectator finishes setup by the time the archive frames arrive,
    // eliminating the forced speed-up at gameplay start.
    if (client.have_pre_match_state &&
        !client.have_match_state &&
        client.pre_match_id != 0 &&
        client.pre_match_config_crc != 0) {
        // No palette data yet — use base palette (custom will arrive with MatchState)
        SPLAY_LOG(-1,
            "Early bootstrap from PreMatchState: pre_match_id=0x%08X ordinal=%u chars=(%u,%u) stage=%u",
            client.pre_match_id,
            client.pre_match_ordinal,
            (unsigned)client.pre_match_config.p1_character,
            (unsigned)client.pre_match_config.p2_character,
            (unsigned)client.pre_match_config.stage_id);
        return true;
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

    for (uint8_t gameSlot = 0; gameSlot < 2; ++gameSlot) {
        bool useCustom = false;
        const char* waitReason = nullptr;
        if (!TryResolveBufferedSpectatorPalette(client, gameSlot, &useCustom, &waitReason)) {
            if (outPaletteWaitSlot) {
                *outPaletteWaitSlot = gameSlot;
            }
            if (outPaletteWaitReason) {
                *outPaletteWaitReason = waitReason;
            }
            return false;
        }
    }

    return client.confirmed_contiguous_rb_frame >= (kBootstrapStartBufferFrames - 1);
}

static const LockedMatchConfig& ResolveBootstrapConfig(const SpectatorClientSnapshot& client) {
    // For early bootstrap (PreMatchState received before MatchState), use the
    // pre-match config. Once MatchState arrives the two should be identical.
    return client.have_match_state ? client.config : client.pre_match_config;
}

static uint32_t ResolveBootstrapOrdinal(const SpectatorClientSnapshot& client) {
    return client.have_match_state
        ? (client.match_ordinal != 0 ? client.match_ordinal : 1)
        : (client.pre_match_ordinal != 0 ? client.pre_match_ordinal : 1);
}

// Re-enter the next match the way a player REMATCH does, instead of driving
// the character- and stage-select UI again.
//
// A rematch skips both selection screens because MODE_PREMATCH_INTRO (7)
// re-reads the LIVE charsel globals rather than the selection screen
// (continue_flow.cpp: "Mode 7 re-reads the live charsel globals"). So the
// spectator injects the next match's selection into those globals and enters
// mode 7 directly. ForceSelectionLocked and ForceBootstrapStageGridConfirm --
// the whole forced-UI drive -- never run again after the first match.
//
// This deliberately does NOT route through the win screen. Measured from run
// 2026-08-18_11-44, the spectator's local game never enters MODE_WINSCREEN at
// all (mode histogram: 6 x720, 8 x597, 7 x44, 3 x6, 0 x5 -- no 9), because it
// drives its own local game rather than replaying the players' frontend. A
// win-screen-gated re-entry is therefore dead code, which is exactly what the
// first attempt turned out to be.
static bool TryWinScreenRematchReentry(const LockedMatchConfig& cfg,
                                       const SpectatorClientSnapshot& client,
                                       const char* reason) {
    // Never inject an unpopulated config: bootstrap can legitimately reach
    // ReadyToBootstrap with cfg=0x00000000 while the identity is still in
    // flight, and injecting that would select character 0 on both sides.
    const bool cfgUsable =
        (client.have_match_state && client.config_crc != 0) ||
        (client.have_pre_match_state && client.pre_match_config_crc != 0);
    if (!cfgUsable) return false;

    const uint32_t mode = GetGameMode();
    const bool inLiveGame = (mode == MODE_MATCH ||
                             mode == MODE_PREMATCH_INTRO ||
                             mode == MODE_CHARSEL);

    // NOT IN A LIVE GAME -- the first match of the session, and also every
    // later boundary where the spectator has dipped through the menu for a few
    // frames before the next match's config arrives (measured: 3 samples of
    // mode 3 per boundary). Nothing here needs character select either: mode 7
    // reads the live charsel globals, so the only thing
    // BeginLocalSpectatorLaunch actually accomplished by dropping into
    // MODE_CHARSEL was to give DriveBootstrap somewhere to force the grids.
    // Do its menu-exit work, then enter mode 7 the same way a rematch does.
    // ResetCharSelFields runs BEFORE the injection below -- it clears the very
    // globals we are about to write, and the full-launch path only gets away
    // with the opposite order because the grid drive rewrites them afterwards.
    if (!inLiveGame) {
        NetMenu::HideForLaunch("spectator playback");
        ModeOwnership::SetPendingMenuRestore(false);
        WriteMemory<uint8_t>(ADDR_STAGESEL_ENABLE, 1);
        ModeOwnership::ResetCharSelFields();
    }

    // Re-assert what BeginLocalSpectatorLaunch would have set. On the re-entry
    // path these almost certainly still hold from the previous match, but
    // "almost certainly" is not a basis for the mode the next match runs in --
    // and they are idempotent writes.
    WriteMemory<uint32_t>(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ModeOwnership::ClearVanillaNetplayFlags();
    WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 0);
    WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 0);

    WriteMemory<uint32_t>(ADDR_CHARSEL_P1_CHAR_ID, (uint32_t)cfg.p1_character);
    WriteMemory<uint8_t>(ADDR_CHARSEL_P1_PALETTE,  cfg.p1_palette);
    WriteMemory<uint32_t>(ADDR_CHARSEL_P2_CHAR_ID, (uint32_t)cfg.p2_character);
    WriteMemory<uint8_t>(ADDR_CHARSEL_P2_PALETTE,  cfg.p2_palette);
    WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID,    cfg.stage_id);

    ApplySpectatorMatchSettings(cfg, reason ? reason : "spectator winscreen re-entry");
    DetVer_SetRngSeed(cfg.session_seed);

    // Straight into the match intro, the same mode a rematch lands in.
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, 0u);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, 0u);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0u);
    ModeOwnership::CallOriginalSetGameMode(MODE_PREMATCH_INTRO, 1);
    s_launchIssued = true;
    s_localMatchEverLaunched = true;

    SPLAY_LOG(-1,
        "Direct match entry (%s) from mode %u%s: injected chars=(%u,%u) palettes=(%u,%u) "
        "stage=%u — entering mode 7 directly, skipping charsel/stagesel",
        reason ? reason : "?", mode, inLiveGame ? "" : " [entered from menu]",
        (unsigned)cfg.p1_character, (unsigned)cfg.p2_character,
        (unsigned)cfg.p1_palette,  (unsigned)cfg.p2_palette,
        (unsigned)cfg.stage_id);
    return true;
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

    const LockedMatchConfig& cfg = ResolveBootstrapConfig(client);
    DetVer_SetRngSeed(cfg.session_seed);
    ApplySpectatorMatchSettings(cfg, "spectator playback launch");

    s_launchIssued = true;
    s_localMatchEverLaunched = true;
    const bool earlyStart = client.have_pre_match_state && !client.have_match_state;
    SetStatus("%sStarting watch playback for game %u.",
        earlyStart ? "Early " : "",
        ResolveBootstrapOrdinal(client));
    if (earlyStart) {
        SPLAY_LOG(-1,
            "Early bootstrap launch: pre_match_id=0x%08X ordinal=%u (archive not yet received)",
            client.pre_match_id,
            client.pre_match_ordinal);
    }
}

static void LogBootstrapState(const SpectatorClientSnapshot& client, const char* reason) {
    const uint32_t mode = GetGameMode();
    const uint32_t sub = GetSubstate();
    const uint8_t stageCursor = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
    const uint8_t stageConfirmed = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 1);
    const uint8_t stageCounter = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 2);
    const uint8_t stageAux = ReadMemory<uint8_t>(ADDR_STAGE_AUX);
    const uint8_t stageCancel = ReadMemory<uint8_t>(ADDR_CHARSEL_CANCEL);
    const uint8_t confirmCursor = ReadMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR);
    const uint8_t confirmAction = ReadMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION);
    const uint8_t committedStage = ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID);

    SPLAY_LOG(
        s_localPlaybackRbFrame,
        "Bootstrap %s mode=%u sub=%u target_stage=%u cursor=%u confirmed=%u counter=%u aux=%u cancel=%u menu_cursor=%u menu_action=%u committed=%u p1_locked=%d p2_locked=%d launch=%d",
        reason ? reason : "state",
        mode,
        sub,
        (unsigned)ResolveBootstrapConfig(client).stage_id,
        (unsigned)stageCursor,
        (unsigned)stageConfirmed,
        (unsigned)stageCounter,
        (unsigned)stageAux,
        (unsigned)stageCancel,
        (unsigned)confirmCursor,
        (unsigned)confirmAction,
        (unsigned)committedStage,
        CharSelPaletteSelect_IsSelectionLocked(0) ? 1 : 0,
        CharSelPaletteSelect_IsSelectionLocked(1) ? 1 : 0,
        s_launchIssued ? 1 : 0);
}

static void ForceBootstrapStageGridConfirm(uint8_t targetStage, const char* reason) {
    const uint8_t cursor = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR);
    const uint8_t confirmed = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 1);
    const uint8_t counter = ReadMemory<uint8_t>(ADDR_STAGE_CURSOR + 2);

    if (cursor != targetStage) {
        WriteMemory<uint8_t>(ADDR_STAGE_CURSOR, targetStage);
        WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 1, 0);
        WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 2, 0);
    } else {
        WriteMemory<uint8_t>(ADDR_STAGE_CURSOR + 1, 1);
    }

    WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, targetStage);

    if (!reason || !reason[0]) {
        return;
    }

    SPLAY_LOG(
        s_localPlaybackRbFrame,
        "Bootstrap stage grid drive reason=%s mode=%u sub=%u target_stage=%u cursor=%u confirmed=%u counter=%u committed=%u",
        reason ? reason : "confirm",
        GetGameMode(),
        GetSubstate(),
        (unsigned)targetStage,
        (unsigned)cursor,
        (unsigned)confirmed,
        (unsigned)counter,
        (unsigned)ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID));
}

static void ForceBootstrapStageConfirmAccept(uint8_t targetStage, const char* reason) {
    const uint8_t menuCursor = ReadMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR);
    const uint8_t menuAction = ReadMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION);
    const uint8_t cancel = ReadMemory<uint8_t>(ADDR_CHARSEL_CANCEL);

    WriteMemory<uint8_t>(ADDR_STAGE_CURSOR, targetStage);
    WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, targetStage);
    WriteMemory<uint8_t>(ADDR_CHARSEL_CANCEL, 0);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_CURSOR, 1);
    WriteMemory<uint8_t>(ADDR_STAGE_CONFIRM_MENU_ACTION, 0);

    if (!reason || !reason[0]) {
        return;
    }

    SPLAY_LOG(
        s_localPlaybackRbFrame,
        "Bootstrap stage confirm drive reason=%s mode=%u sub=%u target_stage=%u menu_cursor=%u menu_action=%u cancel=%u committed=%u",
        reason,
        GetGameMode(),
        GetSubstate(),
        (unsigned)targetStage,
        (unsigned)menuCursor,
        (unsigned)menuAction,
        (unsigned)cancel,
        (unsigned)ReadMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID));
}

static void UpdateLivePlayback(const SpectatorClientSnapshot& client) {
    if (GetGameMode() != MODE_MATCH || s_localFrameOriginAbs < 0) {
        ResetLocalSimulationState();
        TransitionState(SpectatorPlaybackState::ReadyToBootstrap,
            "Local playback left the match. Restarting watch playback.");
        return;
    }

    ClearPlaybackOverrides();
    SyncStreamEdges(client);
    SyncObservedDispatchFrame();

    // S-4 fail latch: a verified divergence stops consumption immediately;
    // FrameUpdate performs the actual leave (never inside the dispatcher).
    if (s_hvFailLatch) {
        s_dispatchFrameBudget = 0;
        s_dispatchFramesProducedThisLoop = 0;
        SpectatorClient_SetFastForwardEnabled(false);
        SpectatorClient_SetHardSyncEnabled(false);
        InputSyncHooks_SetTimesyncFreeze(true);
        return;
    }

    const int32_t nextRbFrame = s_nextDispatchRbFrame >= 0 ? s_nextDispatchRbFrame : 0;
    const int32_t availableFrames = client.confirmed_contiguous_rb_frame - nextRbFrame + 1;
    const size_t buffered = availableFrames > 0 ? (size_t)availableFrames : 0u;

    // A sealed stream will never grow again — priming/rebuffer thresholds
    // must not deadlock a finite tail (policy PlaybackPrimed, S-2).
    const bool streamSealed = !client.match_active ||
        client.state == SpectatorClientState::Failed;

    uint16_t p1Input = 0;
    uint16_t p2Input = 0;
    const bool nextFrameReady = buffered > 0 &&
        SpectatorClient_GetFrameInputs(nextRbFrame, &p1Input, &p2Input);

    if (!nextFrameReady) {
        // True underrun (or archive hole): freeze; the elastic cadence above
        // this point exists precisely to make this rare. Resume at the
        // rebuffer mark (120), not the full startup prime (240).
        if (s_playbackPrimed) {
            s_rebuffering = true;
        }
        s_dispatchFrameBudget = 0;
        s_dispatchFramesProducedThisLoop = 0;
        SpectatorClient_SetFastForwardEnabled(false);
        SpectatorClient_SetHardSyncEnabled(false);
        InputSyncHooks_SetTimesyncFreeze(true);
        ApplyCatchupScale(1.0f, 0);
        TransitionState(SpectatorPlaybackState::Buffering,
            buffered > 0 ? "Waiting for the next confirmed frame."
                         : "Buffering confirmed match data.");
        return;
    }

    // §2.8.8 startup prime / rebuffer gate: build the ~4 s live cushion
    // before consuming (2 s after an underrun); a sealed tail plays out.
    if (!Spectator::PlaybackPrimed(buffered, streamSealed, s_rebuffering)) {
        s_dispatchFrameBudget = 0;
        s_dispatchFramesProducedThisLoop = 0;
        SpectatorClient_SetFastForwardEnabled(false);
        SpectatorClient_SetHardSyncEnabled(false);
        InputSyncHooks_SetTimesyncFreeze(true);
        ApplyCatchupScale(1.0f, 0);
        TransitionState(SpectatorPlaybackState::Buffering,
            "%s the playback cushion (%u/%u).",
            s_rebuffering ? "Rebuilding" : "Priming",
            (unsigned)buffered,
            (unsigned)(s_rebuffering ? Spectator::kPlaybackRebufferRecords
                                     : Spectator::kPlaybackStartupBufferRecords));
        return;
    }
    s_playbackPrimed = true;
    s_rebuffering = false;

    // Catch-up plan with hysteresis (enter >300, release at ≤240 target).
    const Spectator::PlaybackCatchupPlan catchPlan =
        Spectator::PlaybackCatchupPlan_Next(buffered, s_catchingUp);
    s_catchingUp = catchPlan.catchingUp;
    int32_t dispatchBudget = (int32_t)catchPlan.tickBudget;

    if (!s_catchingUp) {
        // Elastic slow-motion under low water (S-2): Bresenham-distributed
        // single-slot holds — smooth slight slow motion instead of
        // freeze-then-sprint. Never invents or skips a canonical record.
        const Spectator::PlaybackElasticPlan elastic =
            Spectator::PlaybackElasticPlan_Next(buffered, s_elasticCredit);
        s_elasticCredit = elastic.nextCredit;
        if (!elastic.advance) {
            ++s_elasticHoldCount;
            s_dispatchFrameBudget = 0;
            s_dispatchFramesProducedThisLoop = 0;
            SpectatorClient_SetFastForwardEnabled(false);
            SpectatorClient_SetHardSyncEnabled(false);
            InputSyncHooks_SetTimesyncFreeze(true);
            ApplyCatchupScale(1.0f, (int32_t)buffered - 1);
            TransitionState(SpectatorPlaybackState::Live,
                "Playing live (elastic %u permille, %u buffered).",
                elastic.speedPermille,
                (unsigned)buffered);
            return;
        }
    }

    // Manual override caps the automatic budget while active (hotkeys).
    if (HasManualCatchupOverride() && buffered > 1) {
        dispatchBudget = (int32_t)GetManualCatchupScale();
    }
    if (dispatchBudget < 1) {
        dispatchBudget = 1;
    }
    if (dispatchBudget > availableFrames) {
        dispatchBudget = availableFrames;
    }

    InputSyncHooks_SetTimesyncFreeze(false);
    s_dispatchFrameBudget = dispatchBudget;
    s_dispatchFramesProducedThisLoop = 0;
    SpectatorClient_SetFastForwardEnabled(dispatchBudget > 1);
    SpectatorClient_SetHardSyncEnabled(false);
    ApplyCatchupScale((float)dispatchBudget, (int32_t)buffered - 1);

    if (dispatchBudget > 1) {
        TransitionState(SpectatorPlaybackState::CatchingUp,
            "Catching up toward the live cushion (%u buffered).",
            (unsigned)buffered);
    } else {
        TransitionState(SpectatorPlaybackState::Live,
            "Playing live.");
    }
}

static bool HasConfirmedTailToDrain(const SpectatorClientSnapshot& client) {
    if (!OwnsLocalSimulation() ||
        GetGameMode() != MODE_MATCH ||
        s_localFrameOriginAbs < 0) {
        return false;
    }

    SyncStreamEdges(client);
    SyncObservedDispatchFrame();

    const int32_t nextRbFrame = s_nextDispatchRbFrame >= 0 ? s_nextDispatchRbFrame : 0;
    const int32_t availableFrames = client.confirmed_contiguous_rb_frame - nextRbFrame + 1;
    if (availableFrames <= 0) {
        return false;
    }

    uint16_t p1Input = 0;
    uint16_t p2Input = 0;
    return SpectatorClient_GetFrameInputs(nextRbFrame, &p1Input, &p2Input);
}

static void DriveBootstrap(const SpectatorClientSnapshot& client) {
    if (!s_launchIssued) {
        BeginLocalSpectatorLaunch(client);
    }

    const uint32_t mode = GetGameMode();
    const uint32_t sub = GetSubstate();

    if (mode != s_lastBootstrapMode || sub != s_lastBootstrapSub) {
        s_lastBootstrapMode = mode;
        s_lastBootstrapSub = sub;
        s_bootstrapSubFrames = 0;
        LogBootstrapState(client, "mode/sub change");
    } else {
        ++s_bootstrapSubFrames;
        if ((s_bootstrapSubFrames % kBootstrapStateLogFrames) == 0) {
            LogBootstrapState(client, "still waiting");
        }
    }

    const LockedMatchConfig& cfg = ResolveBootstrapConfig(client);
    DetVer_SetRngSeed(cfg.session_seed);
    // Only apply palette hints once we have the full match state (palette data
    // is not sent with PreMatchState — it arrives with MatchState).
    if (client.have_match_state) {
        UpdateSpectatorPaletteHints(client);
    }

    if (mode == MODE_MENU) {
        TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
            "Starting the local watch match.");
        return;
    }

    if (mode == MODE_CHARSEL) {
        // For early bootstrap, custom palette data isn't available yet — use base.
        // Palette hints will be applied once MatchState arrives.
        bool p1UseCustom = false;
        bool p2UseCustom = false;
        if (client.have_match_state) {
            (void)TryResolveBufferedSpectatorPalette(client, 0, &p1UseCustom, nullptr);
            (void)TryResolveBufferedSpectatorPalette(client, 1, &p2UseCustom, nullptr);
        }

        const bool p1Locked = CharSelPaletteSelect_ForceSelectionLocked(0,
            cfg.p1_character,
            cfg.p1_palette,
            p1UseCustom);
        const bool p2Locked = CharSelPaletteSelect_ForceSelectionLocked(1,
            cfg.p2_character,
            cfg.p2_palette,
            p2UseCustom);

        if (!p1Locked || !p2Locked) {
            SPLAY_LOG(
                s_localPlaybackRbFrame,
                "Bootstrap selection lock failed p1=%d p2=%d mode=%u sub=%u target=(%u,%u,%d)-(%u,%u,%d)",
                p1Locked ? 1 : 0,
                p2Locked ? 1 : 0,
                mode,
                sub,
                (unsigned)cfg.p1_character,
                (unsigned)cfg.p1_palette,
                p1UseCustom ? 1 : 0,
                (unsigned)cfg.p2_character,
                (unsigned)cfg.p2_palette,
                p2UseCustom ? 1 : 0);
        }

        WriteMemory<uint8_t>(ADDR_CHARSEL_STAGE_ID, cfg.stage_id);

        if (sub == CHARSEL_SUB_STAGESEL_GRID) {
            ForceBootstrapStageGridConfirm(
                cfg.stage_id,
                s_bootstrapSubFrames == 0 || (s_bootstrapSubFrames % kBootstrapRetryFrames) == 0
                    ? (s_bootstrapSubFrames == 0 ? "enter stage grid" : "retry stage grid drive")
                    : nullptr);
            TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
                "Advancing stage selection.");
            return;
        }

        if (sub == CHARSEL_SUB_STAGESEL_CONFIRM) {
            ForceBootstrapStageConfirmAccept(
                cfg.stage_id,
                s_bootstrapSubFrames == 0 || (s_bootstrapSubFrames % kBootstrapRetryFrames) == 0
                    ? (s_bootstrapSubFrames == 0 ? "enter stage confirm fallback" : "retry stage confirm fallback")
                    : nullptr);
            TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
                "Confirming the stage selection.");
            return;
        }

        ClearPlaybackOverrides();
        TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
            "Locking in the match setup.");
        return;
    }

    ClearPlaybackOverrides();

    if (mode == MODE_PREMATCH_INTRO || (mode == MODE_MATCH && !AS2_IsInPlayableGameplay())) {
        if (s_localFrameOriginAbs >= 0 && mode == MODE_MATCH) {
            // Gameplay was already started (round transition, not initial
            // bootstrap).  Keep dispatching so the game engine can advance
            // its intro/phase timer — otherwise the timer stalls and the
            // spectator deadlocks waiting for the intro to end.
            UpdateLivePlayback(client);
            return;
        }
        TransitionState(SpectatorPlaybackState::WaitingInteractiveStart,
            "Waiting for the round to start.");
        return;
    }

    if (mode == MODE_MATCH && AS2_IsInPlayableGameplay()) {
        if (s_localFrameOriginAbs < 0) {
            s_localFrameOriginAbs = (int32_t)AS2_GetFrameNumber();
            SetLastCompletedPlaybackFrame(-1);
            s_nextDispatchRbFrame = 0;
            s_dispatchFrameBudget = 0;
            s_dispatchFramesProducedThisLoop = 0;
            s_dispatchPrevP1 = 0;
            s_dispatchPrevP2 = 0;
            SPLAY_LOG(
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
        "Waiting for the match to finish loading.");
}

static void ResetPlayback(const char* reason) {
    if (s_matchId != 0 || s_localPlaybackRbFrame >= 0) {
        SPLAY_LOG(s_localPlaybackRbFrame,
            "Playback reset: reason=%s match=0x%08X/%u last_frame=%d cfg=0x%08X",
            reason && reason[0] ? reason : "(none)",
            s_matchId,
            s_matchOrdinal,
            s_localPlaybackRbFrame,
            s_configCrc);
    }
    ResetLocalSimulationState();
    ClearTrackedIdentity();
    ClearSpectatorPaletteHints();
    s_manualCatchupScaleIndex = 0;
    ResetSpectatorHotkeyEdges();
    TransitionState(SpectatorPlaybackState::Disconnected,
        "%s",
        reason && reason[0] ? reason : "Watch playback idle.");
}

static void DisconnectAfterBufferedPlayback(const char* reason) {
    const char* message =
        (reason && reason[0]) ? reason : "The live match ended after local playback finished.";

    SPLAY_LOG(s_localPlaybackRbFrame,
        "Buffered playback complete — disconnecting: reason=%s last_frame=%d match=0x%08X/%u",
        message,
        s_localPlaybackRbFrame,
        s_matchId,
        s_matchOrdinal);
    EnterSafeMenuIfNeeded();
    SpectatorClient_Disconnect(message);
    ResetPlayback(message);
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
    ResetSpectatorHotkeyEdges();
    ResetPlayback("Watch playback idle.");
}

void SpectatorPlayback_Shutdown() {
    if (!s_initialized) {
        return;
    }

    ResetPlayback("Watch playback shut down.");
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
            ResetPlayback("Watch playback idle.");
        }
        return;
    }

    // S-4 fail-closed consumption: a verified divergence leaves the session
    // (the player link is unaffected). Copy the reason first — the reset
    // below clears the latch state.
    if (s_hvFailLatch) {
        char reason[sizeof(s_hvFailReason)] = {};
        strncpy_s(reason, sizeof(reason),
            s_hvFailReason[0] ? s_hvFailReason
                              : "Watch playback diverged from the match.",
            _TRUNCATE);
        SPLAY_LOG(s_localPlaybackRbFrame,
            "Consuming S-4 divergence latch — disconnecting spectator session: %s",
            reason);
        EnterSafeMenuIfNeeded();
        SpectatorClient_Disconnect(reason);
        ResetLocalSimulationState();
        ClearTrackedIdentity();
        ClearSpectatorPaletteHints();
        TransitionState(SpectatorPlaybackState::PlaybackError, "%s", reason);
        return;
    }

    if (client.state == SpectatorClientState::Failed) {
        if (HasConfirmedTailToDrain(client)) {
            UpdateLivePlayback(client);
            return;
        }

        if (OwnsLocalSimulation()) {
            SPLAY_LOG(
                s_localPlaybackRbFrame,
                "Buffered spectator archive exhausted after transport failure; disconnecting local session.");
            DisconnectAfterBufferedPlayback(
                client.error[0] ? client.error : "The live match disconnected.");
            return;
        }

        EnterSafeMenuIfNeeded();
        ResetLocalSimulationState();
        ClearLocalMatchHistory("playback failed");
        ClearTrackedIdentity();
        ClearSpectatorPaletteHints();
        TransitionState(SpectatorPlaybackState::PlaybackError,
            "%s",
            client.error[0] ? client.error : "Watch playback failed.");
        return;
    }

    if (client.state == SpectatorClientState::Connecting ||
        client.state == SpectatorClientState::Handshaking ||
        client.state == SpectatorClientState::Redirected) {
        EnterSafeMenuIfNeeded();
        ResetLocalSimulationState();
        ClearLocalMatchHistory("reconnecting");
        ClearTrackedIdentity();
        ClearSpectatorPaletteHints();
        TransitionState(SpectatorPlaybackState::Connecting,
            "%s",
            client.status[0] ? client.status : "Connecting to the watch server.");
        return;
    }

    if (client.state == SpectatorClientState::ConnectedNoActiveMatch) {
        // If PreMatchState arrived, the match is starting — fall through to the
        // bootstrap logic so charsel setup begins immediately. The MatchState
        // (with archive) will arrive by the time gameplay starts.
        if (!client.have_pre_match_state) {
            EnterSafeMenuIfNeeded();
            ResetLocalSimulationState();
            ClearTrackedIdentity();
            ClearSpectatorPaletteHints();
            TransitionState(SpectatorPlaybackState::ConnectedNoActiveMatch,
                "%s",
                client.status[0] ? client.status : "Connected. Waiting for a live match.");
            return;
        }
        // Fall through: have_pre_match_state lets us proceed to bootstrap below.
        SPLAY_LOG(-1,
            "Early bootstrap: ConnectedNoActiveMatch -> bootstrap (pre_match_id=0x%08X ordinal=%u chars=(%u,%u) stage=%u)",
            client.pre_match_id,
            client.pre_match_ordinal,
            (unsigned)client.pre_match_config.p1_character,
            (unsigned)client.pre_match_config.p2_character,
            (unsigned)client.pre_match_config.stage_id);
    }

    HandleSpectatorHotkeys();

    // Allow the early bootstrap path through: have_pre_match_state is enough to
    // start and drive charsel. Once MatchState arrives, adopt identity normally.
    const bool haveUsableState = client.have_match_state ||
        (client.have_pre_match_state && client.pre_match_id != 0 && client.pre_match_config_crc != 0);

    if (!haveUsableState) {
        if (client.have_pre_match_state) {
            // Pre-match state is present but invalid — log which field is bad.
            SPLAY_LOG(-1,
                "Early bootstrap blocked: pre_match_id=0x%08X ordinal=%u cfg_crc=0x%08X seed=0x%08X "
                "(id_zero=%d crc_zero=%d)",
                client.pre_match_id,
                client.pre_match_ordinal,
                client.pre_match_config_crc,
                client.pre_match_session_seed,
                client.pre_match_id == 0 ? 1 : 0,
                client.pre_match_config_crc == 0 ? 1 : 0);
        }
        ResetLocalSimulationState();
        ClearTrackedIdentity();
        ClearSpectatorPaletteHints();
        TransitionState(SpectatorPlaybackState::ConnectedWaitingMetadata,
            "Connected. Waiting for match details.");
        return;
    }

    // If MatchState just arrived while we are already bootstrapping via an early
    // PreMatchState, adopt the real identity but do NOT reset the simulation —
    // the bootstrap is already running and the config must match.
    if (client.have_match_state && !HasTrackedIdentity()) {
        AdoptTrackedIdentity(client);
        if (IsBootstrappingState(s_state)) {
            // Already bootstrapping from PreMatchState — apply palette hints now
            // that we have the full config, and continue without restarting.
            SPLAY_LOG(s_localPlaybackRbFrame,
                "MatchState arrived mid-bootstrap: adopting identity 0x%08X/%u, continuing bootstrap",
                client.match_id,
                client.match_ordinal);
            UpdateSpectatorPaletteHints(client);
            ApplySpectatorMatchSettings(ResolveBootstrapConfig(client),
                "match state arrived mid early bootstrap");
        } else {
            const bool unsupportedMidMatch = client.buffered_start_rb_frame > 0;
            SPLAY_LOG(-1,
                "MatchState arrived before bootstrap: match=0x%08X/%u buffered_start=%d mid_match=%d",
                client.match_id,
                client.match_ordinal,
                client.buffered_start_rb_frame,
                unsupportedMidMatch ? 1 : 0);
            TransitionState(
                unsupportedMidMatch
                    ? SpectatorPlaybackState::WaitingNextMatch
                    : SpectatorPlaybackState::WaitingFullArchive,
                unsupportedMidMatch
                    ? "This match started before you connected. Waiting for the next full match."
                    : "Waiting for confirmed match data from the start.",
                client.buffered_start_rb_frame);
            return;
        }
    }

    if (!HasTrackedIdentity() && client.have_pre_match_state) {
        // Using pre-match identity — don't call AdoptTrackedIdentity yet since
        // match_id/ordinal are not final. Continue bootstrapping.
    }

    if (StreamIdentityChanged(client)) {
        SPLAY_LOG(
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
        ClearSpectatorPaletteHints();
        AdoptTrackedIdentity(client);
        const bool unsupportedMidMatch = client.buffered_start_rb_frame > 0;
        TransitionState(
            unsupportedMidMatch
                ? SpectatorPlaybackState::WaitingNextMatch
                : SpectatorPlaybackState::WaitingFullArchive,
            unsupportedMidMatch
                ? "Switched to a match that started before you connected. Waiting for the next full match."
                : "Switched matches. Rebuilding from the start.",
            client.buffered_start_rb_frame);
        return;
    }

    // Treat early-bootstrap (pre-match state only) as implicitly active — the
    // MatchState hasn't arrived yet but we know a match is starting.
    const bool effectivelyActive = client.match_active ||
        (client.have_pre_match_state && !client.have_match_state);

    if (!effectivelyActive) {
        // If bootstrap is running and match_active just went false because
        // MatchState arrived with MATCH_STATE_ENDED (match ended before we could
        // watch it), stop the bootstrap. Don't continue driving charsel into a
        // match that is already over.
        if (IsBootstrappingState(s_state)) {
            SPLAY_LOG(s_localPlaybackRbFrame,
                "Aborting bootstrap: match ended before playback reached gameplay "
                "(state=%s have_pre=%d have_match=%d match_active=%d)",
                StateNameInternal(s_state),
                client.have_pre_match_state ? 1 : 0,
                client.have_match_state ? 1 : 0,
                client.match_active ? 1 : 0);
            EnterSafeMenuIfNeeded();
            ResetLocalSimulationState();
            ClearSpectatorPaletteHints();
            TransitionState(SpectatorPlaybackState::WaitingNextMatch,
                "The match ended before local playback could start. Waiting for the next match.");
            return;
        }

        if (HasConfirmedTailToDrain(client)) {
            UpdateLivePlayback(client);
            return;
        }

        ClearPlaybackOverrides();
        InputSyncHooks_SetTimesyncFreeze(false);
        ApplyCatchupScale(1.0f, 0);
        // Hold on the WIN SCREEN between matches. Bouncing out to the online
        // menu tore down the whole local match context, which is why the next
        // match had to rebuild it by driving the character and stage grids.
        // Staying here keeps that context alive so the next match re-enters
        // through the rematch route (TryWinScreenRematchReentry).
        //
        // This MUST be tested before the EndOfMatch early-return below: that
        // return fires for every mode except MENU and CHARSEL, so the win
        // screen exits there and any winscreen test placed after it is
        // unreachable. (It was, and the spectator silently kept taking the old
        // charsel-drive path -- caught by run 2026-08-18_11-44 logging
        // "Starting watch playback for game 2" instead of the re-entry.)
        if (OwnsLocalSimulation() && GetGameMode() != MODE_MENU) {
            SpectatorClient_ArmForNextMatch("match ended, holding in-game for re-entry");
            ClearSpectatorPaletteHints();
            TransitionState(SpectatorPlaybackState::WaitingNextMatch,
                "Match over. Waiting for the next game.");
            return;
        }

        if (OwnsLocalSimulation() &&
            GetGameMode() != MODE_MENU &&
            GetGameMode() != MODE_CHARSEL) {
            TransitionState(SpectatorPlaybackState::EndOfMatch,
                "Match ended. Waiting for the menu to catch up.");
            return;
        }

        if (OwnsLocalSimulation()) {
            NetMenu::ShowMenuAfterExternalLaunch("Finished watching the match.");
        }

        ResetLocalSimulationState();
        ClearSpectatorPaletteHints();
        SpectatorClient_ArmForNextMatch("match ended");
        TransitionState(SpectatorPlaybackState::WaitingNextMatch,
            "Waiting for the next match.");
        return;
    }

    // Handle ReadyToBootstrap explicitly so BeginLocalSpectatorLaunch and the
    // mode transition happen before DriveBootstrap is called (next frame).
    if (s_state == SpectatorPlaybackState::ReadyToBootstrap) {
        // If we held on the win screen from the previous match, take the
        // rematch route with the next match's selection injected, rather than
        // dropping to MODE_CHARSEL and driving the character and stage grids
        // again. Only valid from the win screen; the first match of a session
        // still goes through the full launch below.
        const LockedMatchConfig& reentryCfg = ResolveBootstrapConfig(client);
        if (TryWinScreenRematchReentry(reentryCfg, client, "next match")) {
            s_launchIssued = true;
            s_localMatchEverLaunched = true;
            UpdateSpectatorPaletteHints(client);
            TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
                "Starting game %u.",
                ResolveBootstrapOrdinal(client));
            return;
        }

        BeginLocalSpectatorLaunch(client);
        TransitionState(SpectatorPlaybackState::BootstrappingFrontend,
            "Starting watch playback for game %u.",
            ResolveBootstrapOrdinal(client));
        return;
    }

    // Once bootstrap is in progress (BootstrappingFrontend / WaitingInteractiveStart),
    // skip BootstrapReady — it already passed once. Re-checking it here would kill
    // the bootstrap when MatchState arrives mid-early-bootstrap but before the palette
    // data packets (which arrive shortly after in the same burst) are buffered.
    if (s_state == SpectatorPlaybackState::BootstrappingFrontend ||
        s_state == SpectatorPlaybackState::WaitingInteractiveStart) {
        DriveBootstrap(client);
        return;
    }

    bool waitNextMatch = false;
    uint8_t paletteWaitSlot = 0xFF;
    const char* paletteWaitReason = nullptr;
    if (!BootstrapReady(client, &waitNextMatch, &paletteWaitSlot, &paletteWaitReason)) {
        ResetLocalSimulationState();
        ClearSpectatorPaletteHints();
        if (waitNextMatch) {
            TransitionState(SpectatorPlaybackState::WaitingNextMatch,
                "This match started before you connected. Waiting for the next full match.",
                client.buffered_start_rb_frame);
        } else if (paletteWaitSlot <= 1 && paletteWaitReason && paletteWaitReason[0]) {
            TransitionState(SpectatorPlaybackState::WaitingFullArchive,
                "Waiting for player %u %s.",
                (unsigned)(paletteWaitSlot + 1),
                paletteWaitReason);
        } else {
            TransitionState(SpectatorPlaybackState::WaitingFullArchive,
                "Waiting for enough confirmed match data to start.");
        }
        return;
    }

    if (!OwnsLocalSimulation()) {
        TransitionState(SpectatorPlaybackState::ReadyToBootstrap,
            "Ready to start local playback.");
        return;
    }
    // Reached for Live / CatchingUp / Buffering / EndOfMatch — all states where
    // OwnsLocalSimulation() is true but we're not in a bootstrap-specific state.
    // DriveBootstrap handles these via UpdateLivePlayback internally.
    DriveBootstrap(client);
}

SpectatorDispatchAction SpectatorPlayback_GetDispatcherFrame(uint16_t* outP1,
                                                            uint16_t* outP2,
                                                            int32_t* outRbFrame) {
    if (!outP1 || !outP2 || !outRbFrame) {
        return SpectatorDispatchAction::BreakLoop;
    }

    const bool gameplayOwned =
        s_localFrameOriginAbs >= 0 &&
        (s_state == SpectatorPlaybackState::Buffering ||
         s_state == SpectatorPlaybackState::CatchingUp ||
         s_state == SpectatorPlaybackState::Live);
    if (!gameplayOwned || GetGameMode() != MODE_MATCH) {
        s_dispatchFramesProducedThisLoop = 0;
        // Unhandled hands the frame back to LOCAL input. That is right for a
        // player, and wrong for a spectator sitting in MODE_MATCH between
        // games: the match becomes locally playable. Hold those frames.
        //
        // ONLY the post-match hold, never the bootstrap states: during
        // BootstrappingFrontend/WaitingInteractiveStart the game is already in
        // MODE_MATCH playing its intro and MUST keep advancing to reach
        // playable gameplay, or the origin is never set and bootstrap
        // deadlocks.
        if (GetGameMode() == MODE_MATCH &&
            (s_state == SpectatorPlaybackState::WaitingNextMatch ||
             s_state == SpectatorPlaybackState::EndOfMatch)) {
            return SpectatorDispatchAction::BreakLoop;
        }
        return SpectatorDispatchAction::Unhandled;
    }

    if (s_dispatchFramesProducedThisLoop == 0) {
        SyncObservedDispatchFrame();
    }

    if (s_dispatchFrameBudget <= 0) {
        s_dispatchFramesProducedThisLoop = 0;
        return SpectatorDispatchAction::BreakLoop;
    }

    if (s_dispatchFramesProducedThisLoop >= s_dispatchFrameBudget) {
        s_dispatchFramesProducedThisLoop = 0;
        return SpectatorDispatchAction::BreakLoop;
    }

    // §2.8.8 wall slice: hidden catch-up ticks stop when the 12 ms budget
    // elapses — the tick ladder is an upper bound, the slice is the truth.
    // The batch keeps the ticks it already executed; a slow machine degrades
    // convergence speed, never visible cadence.
    if (s_dispatchFramesProducedThisLoop == 0) {
        s_sliceStartQpc = NowQpcMicros();
    } else if ((uint64_t)(NowQpcMicros() - s_sliceStartQpc) >=
               Spectator::kPlaybackCatchupSliceMicros) {
        s_dispatchFramesProducedThisLoop = 0;
        return SpectatorDispatchAction::BreakLoop;
    }

    const int32_t rbFrame = s_nextDispatchRbFrame;
    uint16_t p1 = 0;
    uint16_t p2 = 0;
    if (rbFrame < 0 || !SpectatorClient_GetFrameInputs(rbFrame, &p1, &p2)) {
        s_dispatchFrameBudget = 0;
        s_dispatchFramesProducedThisLoop = 0;
        SpectatorClient_SetFastForwardEnabled(false);
        SpectatorClient_SetHardSyncEnabled(false);
        return SpectatorDispatchAction::BreakLoop;
    }

    // S-4: verify the record's confirmed pre-state digest against our own
    // live state BEFORE ticking it (which also checks the post-state of the
    // previous tick). Strict mode arms on the first agreeing record; a
    // stream that never agrees within the window degrades to unverified
    // playback (loud log) — the viewer entered through the native loader,
    // not the players' baseline rendezvous. After acquisition a mismatch is
    // a genuine divergence: fail closed, leave the session (player link
    // unaffected — publication is observational).
    {
        uint32_t recordHash24 = 0;
        bool recordHasHash = false;
        SpectatorClient_GetFrameHash(rbFrame, &recordHash24, &recordHasHash);
        if (recordHasHash && !s_hvDisabled) {
            uint64_t liveDigest = 0;
            if (Rollback::GameSnapshot_HashGameplayLive(&liveDigest)) {
                ++s_hvChecks;
                const uint32_t localHash24 =
                    Spectator::PlaybackHash24FromDigest(liveDigest);
                switch (Spectator::PlaybackVerifyRecordHash(
                            recordHasHash, s_hvDisabled, s_hvAcquired,
                            s_hvEntryMismatches, recordHash24, localHash24)) {
                    case Spectator::PlaybackHashVerdict::Acquired:
                        s_hvAcquired = true;
                        SPLAY_LOG(rbFrame,
                            "Record hash sync ACQUIRED (checks=%u entry_misses=%u)",
                            s_hvChecks, s_hvEntryMismatches);
                        break;
                    case Spectator::PlaybackHashVerdict::EntryMismatch:
                        ++s_hvEntryMismatches;
                        break;
                    case Spectator::PlaybackHashVerdict::GiveUp:
                        s_hvDisabled = true;
                        SPLAY_LOG(rbFrame,
                            "Record hash verification GAVE UP after %u entry misses — "
                            "continuing unverified (fail-degrade)",
                            s_hvEntryMismatches + 1);
                        break;
                    case Spectator::PlaybackHashVerdict::Diverged:
                        s_hvFailLatch = true;
                        _snprintf_s(s_hvFailReason, sizeof(s_hvFailReason), _TRUNCATE,
                            "Watch playback diverged from the match at frame %d.",
                            rbFrame);
                        SPLAY_LOG(rbFrame,
                            "SPECTATOR DESYNC: record hash 0x%06X != local 0x%06X "
                            "(checks=%u) — leaving the session (S-4 fail-closed)",
                            recordHash24, localHash24, s_hvChecks);
                        s_dispatchFrameBudget = 0;
                        s_dispatchFramesProducedThisLoop = 0;
                        return SpectatorDispatchAction::BreakLoop;
                    default:
                        break;
                }
            }
        }
    }

    *outP1 = p1;
    *outP2 = p2;
    *outRbFrame = rbFrame;

    SetLastCompletedPlaybackFrame(rbFrame);
    s_nextDispatchRbFrame = rbFrame + 1;
    ++s_dispatchFramesProducedThisLoop;
    return SpectatorDispatchAction::ProduceFrame;
}

bool SpectatorPlayback_CopyPaletteOverrideBank(uint8_t gameSlot,
                                               uint8_t characterId,
                                               uint8_t basePalette,
                                               NetplayPaletteBank* out) {
    if (!out || gameSlot > 1) {
        return false;
    }

    SpectatorClientSnapshot client{};
    SpectatorClient_GetSnapshot(&client);
    if (!client.active ||
        !client.have_match_state ||
        client.match_id != s_matchId ||
        client.match_ordinal != s_matchOrdinal ||
        client.config_crc != s_configCrc ||
        client.session_seed != s_sessionSeed) {
        return false;
    }

    if (characterId != ConfigCharacterForSlot(client, gameSlot) ||
        basePalette != ConfigBasePaletteForSlot(client, gameSlot)) {
        return false;
    }

    bool useCustom = false;
    if (!TryResolveBufferedSpectatorPalette(client, gameSlot, &useCustom, nullptr) || !useCustom) {
        return false;
    }

    NetplayPaletteBank bank{};
    if (!SpectatorClient_CopyBufferedPaletteBank(gameSlot, &bank) ||
        !bank.valid ||
        bank.character_id != characterId ||
        bank.base_palette != basePalette) {
        return false;
    }

    *out = bank;
    return true;
}

bool SpectatorPlayback_OwnsLocalSimulation() {
    return OwnsLocalSimulation();
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
    out->manual_catchup_scale = GetManualCatchupScale();
    CopyText(out->state_label, sizeof(out->state_label), StateNameInternal(s_state));
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net
