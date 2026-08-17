/**
 * Alice Senki 2 - match_setup (re0.7 M5, master plan §2.5)
 *
 * One phase machine behind the preserved 12-function `PregameSync_*` facade,
 * replacing the pregame_sync + match_bootstrap pair:
 *
 *   SyncAnnounce → SyncExchange → SyncConfirmed (EpochAlign barrier wait)
 *     → FrontendCharSel → FrontendStageSel → FrontendLocked
 *     → ConfigExchange → ConfigAgreed
 *     → BootstrapLoading → BootstrapBaseline → BootstrapReady
 *     → GameplayHandoff
 *
 * M5 semantics owned here:
 *   - Epoch authority (§2.5): epochs are session-scoped u32 generations
 *     minted by the HOST, strictly increasing, starting at 1. The join side
 *     adopts them from the EpochAlign barrier — never a game-local counter,
 *     never derived from timers.
 *   - EpochAlign (INV-8/INV-10): a TransitionBarrier kind carrying
 *     {epoch, first_phase, native_mode}. Frontend input exchange for an
 *     epoch begins only after its commit; commit requires both sides to
 *     propose the same {epoch, first_phase}; adoption cancels every prior
 *     frontend machine.
 *   - Recovery ladder (§4.6, INV-11/INV-12): frontend starvation
 *     interrogation escalations and phase timeouts recover by EpochAlign
 *     re-run or a pregame restart under a fresh epoch ON THE LIVE
 *     CONNECTION. The only terminal exits are genuine incompatibilities
 *     (config validation, second baseline mismatch → ConfirmedDesync).
 *   - Rematch fast path (continue plan): EpochAlign(first_phase=None) →
 *     ConfigExchange, freeze coverage extended over the fast-path window.
 */

#include "net/pregame_sync.h"
#include "net/baseline_sync.h"
#include "net/barrier_protocol.h"
#include "net/charsel_sync.h"
#include "net/connection_supervisor.h"
#include "net/delay_policy.h"
#include "net/frontend_input_sync.h"
#include "net/game_settings_sync.h"
#include "net/locked_match_config.h"
#include "net/match_lifecycle.h"
#include "net/netplay_menu_controller.h"
#include "net/netplay_palette_runtime.h"
#include "net/protocol.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/session2.h"
#include "net/spectator_runtime.h"
#include "net/transition_barrier.h"
#include "net/continue_flow.h"
#include "net/winscreen_sync.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "patches/charsel_palette_select.h"
#include "patches/input_sync_hooks.h"
#include "patches/memory_utils.h"
#include "patches/tick_hooks.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "rollback/online_wiring.h"
#include "rollback/owner_diagnostics.h"
#include "rollback/savestate.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

// ============================================================================
// Internal state — phase machine
// ============================================================================

static bool            s_initialized       = false;
static PregamePhase    s_phase             = PregamePhase::Idle;
static LockedMatchConfig s_lockedConfig    = {};
static bool            s_configAgreedTop   = false;
static uint32_t        s_configHashTop     = 0;
static char            s_statusText[128]   = "";
static char            s_errorText[128]    = "";

// CharSel/StageSel tracking
static bool            s_localCharSelLocked  = false;
static bool            s_remoteCharSelLocked = false;
static bool            s_localStageLocked    = false;
static bool            s_remoteStageLocked   = false;

// Initial session sync state
static uint32_t        s_sessionId           = 0;
static uint32_t        s_remoteSessionId     = 0;
static uint8_t         s_assignedSide        = 0;   // 0 = host is P1
static uint8_t         s_localCapabilities   = 0x03; // Bit 0: savestate baseline, Bit 1: frontend digests
static uint8_t         s_remoteCapabilities  = 0;
static bool            s_syncAnnounceSent    = false;
static bool            s_remoteSyncAnnounced = false;
static bool            s_syncConfirmSent     = false;
static bool            s_remoteSyncConfirmed = false;
static uint8_t         s_syncRoundOption     = 0;
static bool            s_haveSyncRoundOption = false;

// Epoch authority (§2.5): host-minted, strictly increasing, session-scoped.
static uint32_t        s_epoch               = 0;    // current epoch (0 = none)
static uint32_t        s_epochCounter        = 0;    // high-water mark this session
static uint64_t        s_epochSessionId      = 0;    // Session2 id the counter belongs to
static bool            s_alignProposed       = false;
static bool            s_alignEpochMinted    = false;
static uint8_t         s_alignFirstPhase     = (uint8_t)FrontendPhaseId::CharSel;

// Continue-screen rematch fast path: skips FrontendCharSel/StageSel entirely
// and rebuilds the locked config from the previous match's snapshot.
static bool              s_rematchFastPath  = false;
static LockedMatchConfig s_rematchSnapshot  = {};

// Phase timeout tracking
static DWORD           s_phaseStartTime      = 0;
constexpr DWORD        SYNC_TIMEOUT_MS       = 10000;  // 10s for sync/align phases
constexpr DWORD        CONFIG_TIMEOUT_MS     = 10000;  // 10s for config exchange
constexpr DWORD        LOAD_TIMEOUT_MS       = 30000;  // 30s for load barrier
constexpr DWORD        BASELINE_TIMEOUT_MS   = 15000;  // 15s for baseline

// Periodic logging counter
static uint32_t        s_logTickCounter      = 0;
static uint32_t        s_charselLogCounter   = 0;
static uint32_t        s_restartCount        = 0;

// ============================================================================
// Internal state — absorbed bootstrap (former match_bootstrap)
// ============================================================================

static bool            s_isHost             = false;

// Config exchange
static bool            s_configSent         = false;
static bool            s_configReceived     = false;
static bool            s_configAgreed       = false;

// Load barrier
static bool            s_localLoaded        = false;
static bool            s_remoteLoaded       = false;
static bool            s_loadBarrierSent    = false;
static uint8_t         s_localLoadMode      = 0;
static uint8_t         s_localLoadSubstate  = 0;
static int32_t         s_localLoadSimFrame  = -1;
static uint8_t         s_remoteLoadMode     = 0;
static uint8_t         s_remoteLoadSubstate = 0;
static int32_t         s_remoteLoadSimFrame = -1;

// Baseline
static bool            s_localBaselineReady = false;
static bool            s_remoteBaselineReady= false;
static bool            s_baselineAgreed     = false;
static uint32_t        s_localBaselineCRC   = 0;
static uint32_t        s_remoteBaselineCRC  = 0;
static uint32_t        s_localBaselineDigest = 0;
static uint32_t        s_remoteBaselineDigest = 0;
static bool            s_baselineDigestSent = false;
static bool            s_baselineRetryUsed  = false;   // §2.5: one retry, then terminal
static uint8_t         s_localBaselineMode      = 0;
static uint8_t         s_localBaselineSubstate  = 0;
static int32_t         s_localBaselineSimFrame  = -1;
static uint8_t         s_remoteBaselineMode     = 0;
static uint8_t         s_remoteBaselineSubstate = 0;
static int32_t         s_remoteBaselineSimFrame = -1;

// Gameplay start (host-authoritative GO — packet 19; carries the frame facts
// a bare TransitionBarrier commit cannot)
static bool            s_gameplayStart      = false;
static uint32_t        s_bootstrapFrameAbs  = 0;
static bool            s_gameplayStartSent  = false;
static int32_t         s_gameplayStartHostGameAbsFrame = -1;

// Remote delay/rollback announcement (advisory — INV-23; the compatibility
// checks on mode/timing are fail-closed, the values are never applied
// locally beyond DelayPolicy's advisory bookkeeping)
static bool            s_remoteDelayReceived = false;
static DelayNegotiationData s_remoteDelayData = {};

// ============================================================================
// Helpers
// ============================================================================

static void BootstrapAbortInternal();
static void RestartPregame(const char* reason);
static void BeginConfigExchangeInternal(const LockedMatchConfig* config);
static void BeginLoadingInternal();
static void BeginBaselineInternal();

static void SetPhase(PregamePhase next, const char* why) {
    if (s_phase == next) return;
    const char* oldName = PregamePhaseName(s_phase);
    const char* newName = PregamePhaseName(next);

    uint32_t mode = GetGameMode();
    uint32_t sub  = GetSubstate();
    uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Phase %s -> %s (%s) | mode=%u sub=%u simFrame=%u epoch=%u",
        oldName, newName, why ? why : "?", mode, sub, simFrame, s_epoch);
    Rollback::NetplayLog_StateChange("PREGAME", (int32_t)simFrame,
        "PregamePhase", oldName, newName, why ? why : "?");
    s_phase = next;
    s_phaseStartTime = GetTickCount();

    // Error is a TERMINAL exit reserved for genuine incompatibilities
    // (config validation failure, confirmed baseline divergence). Everything
    // recoverable routes through RestartPregame instead (INV-12): a
    // recoverable frontend timeout never destroys the session.
    if (next == PregamePhase::Error) {
        InputSyncHooks_SetLoadBarrierFreeze(false);
        NetMenu::HandleDisconnection(s_errorText[0] ? s_errorText : why);
    }
}

static void SetStatusFmt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_statusText, sizeof(s_statusText), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void SetErrorFmt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_errorText, sizeof(s_errorText), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static bool IsSessionValid() {
    SessionSnapshot snap{};
    Session_GetSnapshot(&snap);
    return snap.active && snap.state != SessionState::Failed && snap.state != SessionState::Idle;
}

static bool PhaseTimedOut(DWORD timeoutMs) {
    if (s_phaseStartTime == 0 || timeoutMs == 0) return false;
    return (GetTickCount() - s_phaseStartTime) >= timeoutMs;
}

// Hard cap for a handshake whose peer is alive but not progressing (M1
// semantics, §2.5): phase timeouts only fire after the base timeout AND ≥2 s
// of protocol silence; while the peer provably transmits, extend to the 45 s
// hard cap. The cap routes to a pregame restart, never a teardown (INV-12).
constexpr DWORD HANDSHAKE_LIVENESS_CAP_MS = 45000;

static bool HandshakeTimedOut(DWORD baseTimeoutMs, const char* what) {
    if (!PhaseTimedOut(baseTimeoutMs)) return false;
    const uint32_t inboundSilenceMs = Session_GetMsSinceLastInbound();
    if (inboundSilenceMs < 2000 && !PhaseTimedOut(HANDSHAKE_LIVENESS_CAP_MS)) {
        static DWORD s_lastAliveWaitLogTick = 0;
        const DWORD now = GetTickCount();
        if (s_lastAliveWaitLogTick == 0 || (now - s_lastAliveWaitLogTick) >= 2000) {
            s_lastAliveWaitLogTick = now;
            LOG_NETPLAY(LOG_WARNING,
                "[MatchSetup] %s exceeded %lums but peer is alive (inbound %ums ago) — extending up to %lums",
                what ? what : "handshake",
                (unsigned long)baseTimeoutMs,
                inboundSilenceMs,
                (unsigned long)HANDSHAKE_LIVENESS_CAP_MS);
        }
        return false;
    }
    return true;
}

// §2.5 epoch authority: host-minted, strictly increasing, session-scoped.
// The counter also absorbs the deterministic local charsel-cancel rebinds
// (both peers bump the frontend epoch identically from the lockstep stream),
// so a freshly minted epoch can never collide with a bumped one.
static uint32_t NextEpoch() {
    const uint64_t sid = Session2_GetSessionId();
    if (sid != s_epochSessionId) {
        s_epochSessionId = sid;
        s_epochCounter = 0;
        Rollback::NetplayLog_Write("PREGAME", -1,
            "Epoch counter reset for new session (session_id=%016llX)",
            (unsigned long long)sid);
    }
    uint32_t base = s_epochCounter;
    const uint32_t frontendEpoch = FrontendInputSync_GetEpochId();
    if (frontendEpoch > base) {
        base = frontendEpoch;
    }
    s_epochCounter = base + 1;
    if (s_epochCounter == 0) {
        s_epochCounter = 1;   // 0 reserved = "no epoch"
    }
    return s_epochCounter;
}

static void AdoptEpoch(uint32_t epoch) {
    const uint64_t sid = Session2_GetSessionId();
    if (sid != s_epochSessionId) {
        s_epochSessionId = sid;
        s_epochCounter = 0;
    }
    if (epoch > s_epochCounter) {
        s_epochCounter = epoch;
    }
    s_epoch = epoch;
}

static uint8_t GetOutgoingSyncRoundOption(const char* reason) {
    if (Session_GetRole() == SessionRole::Host) {
        if (!s_haveSyncRoundOption) {
            s_syncRoundOption = GameSettingsSync_BuildHostRoundOption(
                reason ? reason : "pregame sync");
            s_haveSyncRoundOption = true;
        }
        GameSettingsSync_ApplyRoundOption(
            s_syncRoundOption,
            reason ? reason : "pregame sync");
        return s_syncRoundOption;
    }

    if (s_haveSyncRoundOption) {
        return s_syncRoundOption;
    }
    return GameSettingsSync_ReadRoundOption();
}

static void AdoptHostRoundOption(uint8_t roundOption, const char* reason) {
    if (Session_GetRole() != SessionRole::Join) {
        return;
    }

    s_syncRoundOption = GameSettingsSync_NormalizeRoundOption(
        roundOption,
        reason ? reason : "host pregame sync");
    s_haveSyncRoundOption = true;
    GameSettingsSync_ApplyRoundOption(
        s_syncRoundOption,
        reason ? reason : "host pregame sync");
}

static bool EnsureRoundOptionBeforeFrontend(const char* reason) {
    if (Session_GetRole() == SessionRole::Host) {
        GetOutgoingSyncRoundOption(reason ? reason : "sync confirmed");
        return true;
    }

    if (s_haveSyncRoundOption) {
        return true;
    }

    Rollback::NetplayLog_Write(
        "PREGAME", -1,
        "Missing host round count before frontend: reason=%s phase=%s session=0x%08X remoteSession=0x%08X",
        reason ? reason : "?",
        PregamePhaseName(s_phase),
        s_sessionId,
        s_remoteSessionId);
    Rollback::NetplayLog_Flush();
    return false;
}

static void UpdateBootstrapFreezeForBoundary() {
    const bool inBootstrap =
        s_phase == PregamePhase::BootstrapLoading ||
        s_phase == PregamePhase::BootstrapBaseline ||
        s_phase == PregamePhase::BootstrapReady;

    // Rematch fast path: with no charsel to absorb the handshake time, the
    // game can reach the Mode 8 gameplay boundary while the pregame handshake
    // is still in the Sync*/Config* phases — extend the freeze coverage there
    // (continue plan "freeze coverage gap", carried over per §2.5).
    const bool fastPathPreBootstrap =
        s_rematchFastPath &&
        (s_phase == PregamePhase::SyncAnnounce ||
         s_phase == PregamePhase::SyncExchange ||
         s_phase == PregamePhase::SyncConfirmed ||
         s_phase == PregamePhase::ConfigExchange ||
         s_phase == PregamePhase::ConfigAgreed);

    if (!inBootstrap && !fastPathPreBootstrap) {
        InputSyncHooks_SetLoadBarrierFreeze(false);
        return;
    }

    const uint32_t mode = GetGameMode();
    const uint32_t sub = GetSubstate();

    // Let Mode 8 loading/setup/init progress naturally; only freeze once the
    // game reaches the gameplay boundary before bootstrap has finished.
    const bool shouldFreeze = (mode == MODE_MATCH && sub == MATCH_SUB_GAMEPLAY);
    InputSyncHooks_SetLoadBarrierFreeze(shouldFreeze);
}

// INV-11 transport gate for the frontend interrogation: interrogate only
// while the supervisor reports the link alive (Healthy/Degraded). Interrupted
// and Dead belong to the silence ladder (INV-14).
static bool TransportHealthyGate() {
    return !ConnectionSupervisor_IsInterrupted() && !ConnectionSupervisor_IsDead();
}

// Injected into frontend_input_sync (M0 dependency inversion): win-screen
// frame-input sends are allowed only while pregame is inactive or already in
// gameplay handoff.
static bool PregameWinScreenSendGate() {
    return s_phase == PregamePhase::Idle || s_phase == PregamePhase::GameplayHandoff;
}

// ============================================================================
// Bootstrap send helpers (former match_bootstrap)
// ============================================================================

static FrameTimingMode LocalFrameTimingMode() {
    return IsFrameLimiter60FpsPatchEnabled()
        ? FrameTimingMode::Proper60
        : FrameTimingMode::Vanilla58_8;
}

static bool FrameTimingModeEnabled(FrameTimingMode mode) {
    return mode == FrameTimingMode::Proper60;
}

static void ResetRemoteDelayData() {
    memset(&s_remoteDelayData, 0, sizeof(s_remoteDelayData));
    s_remoteDelayData.local_input_delay = DELAY_DEFAULT_PREF;
    s_remoteDelayData.max_rollback = ROLLBACK_BUDGET_DEFAULT;
    s_remoteDelayData.gameplay_delay_mode = GameplayDelayMode::AsymmetricExpert;
}

static void CaptureGameplayContext(uint8_t* mode, uint8_t* substate, int32_t* simFrame) {
    if (mode) {
        *mode = (uint8_t)GetGameMode();
    }
    if (substate) {
        *substate = (uint8_t)GetSubstate();
    }
    if (simFrame) {
        *simFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    }
}

static BaselineSyncStateView BuildBaselineStateView() {
    BaselineSyncStateView view{};
    view.phase_name = PregamePhaseName(s_phase);
    view.local_loaded = s_localLoaded;
    view.remote_loaded = s_remoteLoaded;
    view.local_ready = s_localBaselineReady;
    view.remote_ready = s_remoteBaselineReady;
    view.digest_sent = s_baselineDigestSent;
    view.baseline_agreed = s_baselineAgreed;
    view.local_mode = s_localBaselineMode;
    view.local_substate = s_localBaselineSubstate;
    view.local_sim_frame = s_localBaselineSimFrame;
    view.remote_mode = s_remoteBaselineMode;
    view.remote_substate = s_remoteBaselineSubstate;
    view.remote_sim_frame = s_remoteBaselineSimFrame;
    view.local_crc = s_localBaselineDigest;
    view.remote_crc = s_remoteBaselineDigest;
    view.session_seed = s_lockedConfig.session_seed;
    return view;
}

static void SendConfig() {
    ConfigExchangePayload payload{};
    payload.p1_character = s_lockedConfig.p1_character;
    payload.p1_palette = s_lockedConfig.p1_palette;
    payload.p2_character = s_lockedConfig.p2_character;
    payload.p2_palette = s_lockedConfig.p2_palette;
    payload.stage_id = s_lockedConfig.stage_id;
    payload.host_side = s_lockedConfig.host_side;
    payload.round_count = s_lockedConfig.round_count;
    payload.time_limit = s_lockedConfig.time_limit;
    payload.rng_seed = s_lockedConfig.rng_seed;
    payload.session_seed = s_lockedConfig.session_seed;

    // Announce only the local peer's gameplay delay and rollback budget
    // (advisory, INV-23).
    DelayNegotiationData delayData{};
    DelayPolicy_BuildNegotiationData(&delayData);
    payload.my_input_delay = (uint8_t)delayData.local_input_delay;
    payload.my_max_rollback = (uint8_t)delayData.max_rollback;
    payload.gameplay_delay_mode = (uint8_t)delayData.gameplay_delay_mode;
    payload.frame_timing_mode = (uint8_t)LocalFrameTimingMode();

    const bool sent = BarrierProtocol_SendPacket(PacketType::ConfigExchange,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchSetup] Failed to send ConfigExchange");
        Rollback::NetplayLog_Write(
            "BARRIER", -1,
            "ERROR: ConfigExchange queue failed: hash=0x%08X my_delay=%d my_max_rb=%d",
            LockedMatchConfig_Hash(&s_lockedConfig),
            delayData.local_input_delay,
            delayData.max_rollback);
        return;
    }

    s_configSent = true;
    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Sent config (hash=0x%08X epoch=%u rounds_raw=%u rounds_to_win=%d my_delay=%d my_max_rb=%d delay_mode=%s frame_timing=%s)",
        LockedMatchConfig_Hash(&s_lockedConfig),
        s_epoch,
        s_lockedConfig.round_count,
        GameSettingsSync_RoundsToWin(s_lockedConfig.round_count),
        delayData.local_input_delay,
        delayData.max_rollback,
        GameplayDelayModeName(delayData.gameplay_delay_mode),
        FrameTimingModeName((FrameTimingMode)payload.frame_timing_mode));
}

static void SendConfigAck(uint32_t hash, bool accepted) {
    ConfigAckPayload payload{};
    payload.config_hash = hash;
    payload.accepted = accepted ? 1 : 0;

    DelayNegotiationData delayData{};
    DelayPolicy_BuildNegotiationData(&delayData);
    payload.my_input_delay = (uint8_t)delayData.local_input_delay;
    payload.my_max_rollback = (uint8_t)delayData.max_rollback;
    payload.gameplay_delay_mode = (uint8_t)delayData.gameplay_delay_mode;
    payload.frame_timing_mode = (uint8_t)LocalFrameTimingMode();

    const bool sent = BarrierProtocol_SendPacket(PacketType::ConfigAck,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchSetup] Failed to send ConfigAck");
        return;
    }

    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Sent ConfigAck: hash=0x%08X accepted=%d my_delay=%d my_max_rb=%d delay_mode=%s frame_timing=%s",
        hash, accepted ? 1 : 0,
        delayData.local_input_delay,
        delayData.max_rollback,
        GameplayDelayModeName(delayData.gameplay_delay_mode),
        FrameTimingModeName((FrameTimingMode)payload.frame_timing_mode));
}

static void SendLoadBarrier() {
    LoadBarrierPayload payload{};
    int32_t simFrame = -1;
    CaptureGameplayContext(&payload.mode, &payload.substate, &simFrame);
    payload.loaded = 1;
    payload.sim_frame = (uint32_t)simFrame;

    s_localLoadMode = payload.mode;
    s_localLoadSubstate = payload.substate;
    s_localLoadSimFrame = (int32_t)payload.sim_frame;

    const bool sent = BarrierProtocol_SendPacket(PacketType::LoadBarrier,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchSetup] Failed to send LoadBarrier");
        return;
    }

    s_loadBarrierSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Sent LoadBarrier: mode=%u sub=%u simFrame=%d",
        payload.mode, payload.substate, s_localLoadSimFrame);
    Rollback::NetplayLog_Write(
        "BARRIER", s_localLoadSimFrame,
        "Local LoadBarrier sent: phase=%s local=%u/%u/%d remote=%u/%u/%d local_loaded=%d remote_loaded=%d",
        PregamePhaseName(s_phase),
        s_localLoadMode,
        s_localLoadSubstate,
        s_localLoadSimFrame,
        s_remoteLoadMode,
        s_remoteLoadSubstate,
        s_remoteLoadSimFrame,
        s_localLoaded ? 1 : 0,
        s_remoteLoaded ? 1 : 0);
}

static void SendBaselineReady() {
    BaselineReadyPayload payload{};
    int32_t simFrame = -1;
    CaptureGameplayContext(&payload.mode, &payload.substate, &simFrame);
    payload.captured = 1;
    payload.sim_frame = (uint32_t)simFrame;

    s_localBaselineMode = payload.mode;
    s_localBaselineSubstate = payload.substate;
    s_localBaselineSimFrame = (int32_t)payload.sim_frame;

    const bool sent = BarrierProtocol_SendPacket(PacketType::BaselineReady,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchSetup] Failed to send BaselineReady");
        return;
    }

    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Sent BaselineReady: mode=%u sub=%u simFrame=%d",
        payload.mode, payload.substate, s_localBaselineSimFrame);
}

static void SendBaselineDigest(uint32_t crc) {
    BaselineDigestPayload payload{};
    payload.crc32 = crc;

    const bool sent = BarrierProtocol_SendPacket(PacketType::BaselineDigest,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchSetup] Failed to send BaselineDigest");
        return;
    }

    s_baselineDigestSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Sent BaselineDigest: crc=0x%08X", crc);
}

static void SendBaselineBreakdown(const BaselineBreakdownPayload& payload) {
    const bool sent = BarrierProtocol_SendPacket(PacketType::BaselineBreakdown,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchSetup] Failed to send BaselineBreakdown");
        return;
    }
    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Sent BaselineBreakdown: main=0x%08X header=0x%08X context=0x%08X rng=0x%08X sim=%u display=%u",
        payload.main_crc,
        payload.header_crc,
        payload.context_crc,
        payload.rng_seed,
        payload.sim_frame,
        payload.display_frame);
}

static bool SendGameplayStart(uint32_t frame) {
    GameplayStartPayload payload{};
    payload.bootstrap_frame_abs = frame;
    payload.host_game_abs_frame = (uint32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    s_gameplayStartHostGameAbsFrame = (int32_t)payload.host_game_abs_frame;

    const bool sent = BarrierProtocol_SendPacket(PacketType::GameplayStart,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchSetup] Failed to send GameplayStart");
        return false;
    }

    s_gameplayStartSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Sent GameplayStart: bootstrap_frame_abs=%u host_game_abs_frame=%d",
        frame, s_gameplayStartHostGameAbsFrame);
    return true;
}

// ============================================================================
// Bootstrap phase begin/abort (former MatchBootstrap_Begin*/Abort)
// ============================================================================

static void BeginConfigExchangeInternal(const LockedMatchConfig* config) {
    if (!config) return;

    BaselineSync_Reset();
    s_logTickCounter = 0;
    s_isHost = (Session_GetRole() == SessionRole::Host);

    // Host always uses its own config. Join: only overwrite if we haven't
    // already received the host's config via an early ConfigExchange packet.
    if (s_isHost || !s_configReceived) {
        if (&s_lockedConfig != config) {
            memcpy(&s_lockedConfig, config, sizeof(LockedMatchConfig));
        }
    }
    if (s_isHost || s_configReceived) {
        GameSettingsSync_ApplyLockedConfig(&s_lockedConfig,
            s_isHost ? "host begin config exchange" : "join begin config exchange with early host config");
    }

    s_configSent = false;
    // NOTE: Do NOT reset s_configReceived or s_configAgreed here — the
    // host's ConfigExchange packet may have arrived early.

    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Begin config exchange (role=%s epoch=%u config_already=%s agreed_already=%s rounds_raw=%u)",
        s_isHost ? "Host" : "Join",
        s_epoch,
        s_configReceived ? "yes" : "no",
        s_configAgreed ? "yes" : "no",
        s_lockedConfig.round_count);
}

static void BeginLoadingInternal() {
    s_localLoaded = false;
    s_localLoadMode = 0;
    s_localLoadSubstate = 0;
    s_localLoadSimFrame = -1;
    // NOTE: Do NOT reset s_remoteLoaded — the remote's LoadBarrier packet may
    // have arrived early.
    s_loadBarrierSent = false;

    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Begin loading barrier (remote_already=%s)",
        s_remoteLoaded ? "yes" : "no");
    Rollback::NetplayLog_Write(
        "BARRIER", -1,
        "Begin loading barrier: phase=%s remote_already=%d remote_mode=%u remote_sub=%u remote_sim=%d",
        PregamePhaseName(s_phase),
        s_remoteLoaded ? 1 : 0,
        s_remoteLoadMode,
        s_remoteLoadSubstate,
        s_remoteLoadSimFrame);
}

static void BeginBaselineInternal() {
    Savestate_ClearRollbackBaseline("begin baseline capture");
    s_localBaselineReady = false;
    s_localBaselineMode = 0;
    s_localBaselineSubstate = 0;
    s_localBaselineSimFrame = -1;
    // NOTE: Do NOT reset the remote baseline latches — they may have arrived
    // early (except on the explicit retry path, which resets both sides).
    s_baselineAgreed = false;
    s_localBaselineCRC = 0;
    s_localBaselineDigest = 0;
    s_baselineDigestSent = false;
    s_gameplayStart = false;
    s_gameplayStartSent = false;
    s_bootstrapFrameAbs = 0;
    s_gameplayStartHostGameAbsFrame = -1;

    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Begin baseline capture (remote_baseline_already=%s digest=0x%08X main=0x%08X retry_used=%d)",
        s_remoteBaselineReady ? "yes" : "no",
        s_remoteBaselineDigest,
        s_remoteBaselineCRC,
        s_baselineRetryUsed ? 1 : 0);
    BaselineSync_LogBegin(BuildBaselineStateView());
}

static void BootstrapAbortInternal() {
    Savestate_ClearRollbackBaseline("bootstrap abort");
    s_configSent = false;
    s_configReceived = false;
    s_configAgreed = false;
    s_remoteDelayReceived = false;
    ResetRemoteDelayData();
    s_localLoaded = false;
    s_remoteLoaded = false;
    s_loadBarrierSent = false;
    s_localLoadMode = 0;
    s_localLoadSubstate = 0;
    s_localLoadSimFrame = -1;
    s_remoteLoadMode = 0;
    s_remoteLoadSubstate = 0;
    s_remoteLoadSimFrame = -1;
    s_localBaselineReady = false;
    s_remoteBaselineReady = false;
    s_baselineAgreed = false;
    s_localBaselineCRC = 0;
    s_remoteBaselineCRC = 0;
    s_localBaselineDigest = 0;
    s_remoteBaselineDigest = 0;
    s_baselineDigestSent = false;
    s_baselineRetryUsed = false;
    s_localBaselineMode = 0;
    s_localBaselineSubstate = 0;
    s_localBaselineSimFrame = -1;
    s_remoteBaselineMode = 0;
    s_remoteBaselineSubstate = 0;
    s_remoteBaselineSimFrame = -1;
    s_gameplayStart = false;
    s_gameplayStartSent = false;
    s_bootstrapFrameAbs = 0;
    s_gameplayStartHostGameAbsFrame = -1;
    BaselineSync_Reset();
}

// ============================================================================
// Bootstrap packet handlers (former MatchBootstrap_On*)
// ============================================================================

static void OnConfigExchange(const ConfigExchangePayload* p) {
    if (!p) return;

    if (!GameplayDelayMode_IsValid(p->gameplay_delay_mode)) {
        SetErrorFmt("Unknown gameplay delay mode from host: %u", p->gameplay_delay_mode);
        SetPhase(PregamePhase::Error, "config validation failed");
        return;
    }
    if (!FrameTimingMode_IsValid(p->frame_timing_mode)) {
        SetErrorFmt("Unknown frame timing mode from host: %u", p->frame_timing_mode);
        SetPhase(PregamePhase::Error, "config validation failed");
        return;
    }
    const GameplayDelayMode remoteDelayMode = (GameplayDelayMode)p->gameplay_delay_mode;
    const FrameTimingMode remoteFrameTimingMode = (FrameTimingMode)p->frame_timing_mode;

    LockedMatchConfig received{};
    LockedMatchConfig_Clear(&received);
    received.p1_character = p->p1_character;
    received.p1_palette = p->p1_palette;
    received.p2_character = p->p2_character;
    received.p2_palette = p->p2_palette;
    received.stage_id = p->stage_id;
    received.host_side = p->host_side;
    received.round_count = p->round_count;
    received.time_limit = p->time_limit;
    received.rng_seed = p->rng_seed;
    received.session_seed = p->session_seed;

    uint32_t receivedHash = LockedMatchConfig_Hash(&received);
    s_configReceived = true;

    if (s_isHost) {
        LOG_NETPLAY(LOG_WARNING, "[MatchSetup] Host received ConfigExchange from join");
    } else {
        // Join: adopt host's config unconditionally (may arrive before we
        // enter ConfigExchange).
        memcpy(&s_lockedConfig, &received, sizeof(LockedMatchConfig));
        GameSettingsSync_ApplyLockedConfig(&s_lockedConfig, "join received host config");
        TickHooks_SetFrameLimiter60FpsSessionOverride(
            FrameTimingModeEnabled(remoteFrameTimingMode),
            "host config timing");
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] Join received config (hash=0x%08X phase=%s rounds_raw=%u frame_timing=%s)",
            receivedHash,
            PregamePhaseName(s_phase),
            s_lockedConfig.round_count,
            FrameTimingModeName(remoteFrameTimingMode));

        s_remoteDelayData.local_input_delay = p->my_input_delay;
        s_remoteDelayData.max_rollback = p->my_max_rollback;
        s_remoteDelayData.gameplay_delay_mode = remoteDelayMode;
        s_remoteDelayReceived = true;
        DelayPolicy_SetGameplayDelayMode(remoteDelayMode);

        if (s_phase == PregamePhase::ConfigExchange) {
            s_configAgreed = true;
            SendConfigAck(receivedHash, true);
            DelayPolicy_NegotiateSession(&s_remoteDelayData);
            LOG_NETPLAY(LOG_INFO, "[MatchSetup] Join accepted config (hash=0x%08X remote_delay=%d)",
                receivedHash,
                DelayPolicy_GetRemoteAnnouncedDelay());
        }
    }
}

static void OnConfigAck(const ConfigAckPayload* p) {
    if (!p) return;

    uint32_t localHash = LockedMatchConfig_Hash(&s_lockedConfig);

    if (!GameplayDelayMode_IsValid(p->gameplay_delay_mode)) {
        SetErrorFmt("Unknown gameplay delay mode from peer: %u", p->gameplay_delay_mode);
        SetPhase(PregamePhase::Error, "config validation failed");
        return;
    }
    if (!FrameTimingMode_IsValid(p->frame_timing_mode)) {
        SetErrorFmt("Unknown frame timing mode from peer: %u", p->frame_timing_mode);
        SetPhase(PregamePhase::Error, "config validation failed");
        return;
    }
    const GameplayDelayMode remoteDelayMode = (GameplayDelayMode)p->gameplay_delay_mode;
    const FrameTimingMode remoteFrameTimingMode = (FrameTimingMode)p->frame_timing_mode;
    if (remoteDelayMode != DelayPolicy_GetGameplayDelayMode()) {
        SetErrorFmt("Gameplay delay mode mismatch: local=%s remote=%s",
            GameplayDelayModeName(DelayPolicy_GetGameplayDelayMode()),
            GameplayDelayModeName(remoteDelayMode));
        SetPhase(PregamePhase::Error, "config validation failed");
        return;
    }
    if (remoteFrameTimingMode != LocalFrameTimingMode()) {
        SetErrorFmt("Frame timing mismatch: local=%s remote=%s",
            FrameTimingModeName(LocalFrameTimingMode()),
            FrameTimingModeName(remoteFrameTimingMode));
        SetPhase(PregamePhase::Error, "config validation failed");
        return;
    }

    if (p->accepted && p->config_hash == localHash) {
        s_remoteDelayData.local_input_delay = p->my_input_delay;
        s_remoteDelayData.max_rollback = p->my_max_rollback;
        s_remoteDelayData.gameplay_delay_mode = remoteDelayMode;
        s_remoteDelayReceived = true;

        s_configAgreed = true;
        DelayPolicy_NegotiateSession(&s_remoteDelayData);
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] Config agreed (hash=0x%08X epoch=%u local_delay=%d local_max_rb=%d remote_delay=%d remote_max_rb=%d)",
            localHash,
            s_epoch,
            DelayPolicy_GetConfiguredDelay(),
            DelayPolicy_GetRollbackBudget(),
            DelayPolicy_GetRemoteAnnouncedDelay(),
            DelayPolicy_GetRemoteAnnouncedMaxRollback());
    } else {
        SetErrorFmt("Config rejected by peer (local=0x%08X remote=0x%08X)",
            localHash, p->config_hash);
        SetPhase(PregamePhase::Error, "config rejected");
    }
}

static void OnLoadBarrier(const LoadBarrierPayload* p) {
    if (!p) return;

    if (p->loaded) {
        s_remoteLoaded = true;
        s_remoteLoadMode = p->mode;
        s_remoteLoadSubstate = p->substate;
        s_remoteLoadSimFrame = (int32_t)p->sim_frame;
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] Remote loading complete: mode=%u sub=%u simFrame=%d",
            s_remoteLoadMode, s_remoteLoadSubstate, s_remoteLoadSimFrame);
    }
}

static void OnBaselineReady(const BaselineReadyPayload* p) {
    if (!p) return;

    if (p->captured) {
        s_remoteBaselineReady = true;
        s_remoteBaselineMode = p->mode;
        s_remoteBaselineSubstate = p->substate;
        s_remoteBaselineSimFrame = (int32_t)p->sim_frame;
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] Remote baseline ready: mode=%u sub=%u simFrame=%d",
            s_remoteBaselineMode, s_remoteBaselineSubstate, s_remoteBaselineSimFrame);

        // Implicit load completion: BaselineReady means the peer is past
        // loading (avoids a deadlock when the explicit LoadBarrier was
        // processed before we entered Loading).
        if (s_phase == PregamePhase::BootstrapLoading && !s_remoteLoaded) {
            s_remoteLoaded = true;
            LOG_NETPLAY(LOG_INFO, "[MatchSetup] Implicit LoadBarrierReady from BaselineReady");
        }
    }
}

static void OnBaselineDigest(const BaselineDigestPayload* p) {
    if (!p) return;

    s_remoteBaselineDigest = p->crc32;
    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Remote baseline digest: digest=0x%08X", p->crc32);
    BaselineSync_LogRemoteDigest(BuildBaselineStateView(), p->crc32);
}

static void OnBaselineBreakdown(const BaselineBreakdownPayload* p) {
    if (!p) return;
    BaselineSync_RecordRemoteBreakdown(BuildBaselineStateView(), *p);
    s_remoteBaselineCRC = p->main_crc;

    const uint32_t expectedDigest = BaselineSync_ComputeAgreementDigest(*p);
    // Keep digest and breakdown consistent even if they arrive in different order.
    if (s_remoteBaselineDigest == 0) {
        s_remoteBaselineDigest = expectedDigest;
        Rollback::NetplayLog_Write(
            "BASELINE", (int32_t)p->sim_frame,
            "Remote BaselineDigest inferred from breakdown: digest=0x%08X main=0x%08X",
            s_remoteBaselineDigest,
            s_remoteBaselineCRC);
    } else if (s_remoteBaselineDigest != expectedDigest) {
        Rollback::NetplayLog_Write(
            "BASELINE", (int32_t)p->sim_frame,
            "WARNING: Remote baseline digest mismatch: digest=0x%08X breakdown_digest=0x%08X breakdown_main=0x%08X",
            s_remoteBaselineDigest,
            expectedDigest,
            s_remoteBaselineCRC);
    }
}

static void OnGameplayStart(const GameplayStartPayload* p) {
    if (!p) return;

    s_bootstrapFrameAbs = p->bootstrap_frame_abs;
    s_gameplayStart = true;
    // NOTE: no phase jump here — on the Join side this packet can arrive in
    // the same drain as BaselineReady/Digest, before the agreement check has
    // run. The phase transitions naturally through Baseline → Ready → Done.
    s_gameplayStartHostGameAbsFrame = (int32_t)p->host_game_abs_frame;
    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] GameplayStart received: bootstrap_frame_abs=%u host_game_abs_frame=%d local_game_abs_frame=%u",
        p->bootstrap_frame_abs,
        s_gameplayStartHostGameAbsFrame,
        ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
}

// ============================================================================
// Session sync packet handlers
// ============================================================================

static void HandleSyncAnnounce(const SyncAnnouncePayload* p) {
    if (!p || s_remoteSyncAnnounced) return;

    s_remoteSessionId = p->session_id;
    s_remoteCapabilities = p->capability_flags;
    s_remoteSyncAnnounced = true;
    AdoptHostRoundOption(p->round_count, "host sync announce");

    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Remote SyncAnnounce: session=0x%08X caps=0x%02X round_raw=%u round_wins=%d",
        p->session_id,
        p->capability_flags,
        p->round_count,
        GameSettingsSync_RoundsToWin(p->round_count));
}

static void HandleSyncConfirm(const SyncConfirmPayload* p) {
    if (!p || s_remoteSyncConfirmed) return;

    if (!p->confirmed) {
        SetErrorFmt("Remote peer rejected session sync");
        SetPhase(PregamePhase::Error, "sync rejected by remote");
        return;
    }

    s_remoteSyncConfirmed = true;
    AdoptHostRoundOption(p->round_count, "host sync confirm");

    // Receiving a SyncConfirm implies the remote already announced.
    if (!s_remoteSyncAnnounced) {
        s_remoteSyncAnnounced = true;
        s_remoteSessionId = p->session_id;
        LOG_NETPLAY(LOG_INFO, "[MatchSetup] Inferred remote announce from SyncConfirm");
    }

    // Join adopts host's session ID and side assignment. (The frontend epoch
    // is NOT bound here — it arrives with the EpochAlign barrier, §2.5.)
    SessionRole role = Session_GetRole();
    if (role == SessionRole::Join) {
        s_sessionId = p->session_id;
        s_assignedSide = p->assigned_side;
    }

    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Remote SyncConfirm: session=0x%08X side=%u confirmed=%u round_raw=%u",
        p->session_id,
        p->assigned_side,
        p->confirmed,
        p->round_count);
}

static void LogPregamePacketAnomaly(const char* reason,
                                    PacketType type,
                                    size_t payloadLen,
                                    size_t expectedLen) {
    Rollback::NetplayLog_Write(
        "PREGAME", -1,
        "%s: type=%s payload=%zu expected=%zu phase=%s session=0x%08X remoteSession=0x%08X role=%s mode=%u sub=%u",
        reason ? reason : "packet anomaly",
        PacketTypeName(type),
        payloadLen,
        expectedLen,
        PregamePhaseName(s_phase),
        s_sessionId,
        s_remoteSessionId,
        SessionRoleName(Session_GetRole()),
        GetGameMode(),
        GetSubstate());
    Rollback::NetplayLog_Flush();
}

// A SyncAnnounce with an unknown session id arriving while we are mid-run is
// the peer's restart signal (§4.6 step 3, both roles). Fresh announces from
// the current run are deduped by s_remoteSyncAnnounced/session-id equality.
static bool IsForeignRestartAnnounce(const SyncAnnouncePayload* p) {
    if (!p) return false;
    if (s_phase == PregamePhase::Idle ||
        s_phase == PregamePhase::GameplayHandoff ||
        s_phase == PregamePhase::SyncAnnounce ||
        s_phase == PregamePhase::SyncExchange) {
        return false;
    }
    return p->session_id != s_sessionId && p->session_id != s_remoteSessionId;
}

static void HandleSessionPacketInternal(PacketType type, const void* payload, size_t payloadLen) {
    Rollback::NetplayLog_Write("PREGAME", -1,
        "OnPregamePacket: type=%s payload=%zu phase=%s session=0x%08X remoteSession=0x%08X announced=%d confirmed=%d epoch=%u",
        PacketTypeName(type),
        payloadLen,
        PregamePhaseName(s_phase),
        s_sessionId,
        s_remoteSessionId,
        s_remoteSyncAnnounced ? 1 : 0,
        s_remoteSyncConfirmed ? 1 : 0,
        s_epoch);
    Rollback::NetplayLog_Flush();

    switch (type) {
        case PacketType::SyncAnnounce:
            if (payloadLen >= sizeof(SyncAnnouncePayload)) {
                // Terminal phases route through the cross-phase restart path.
                if ((s_phase == PregamePhase::Idle ||
                     s_phase == PregamePhase::GameplayHandoff) &&
                    PregameSync_HandleCrossPhaseSessionPacket(type, payload, payloadLen)) {
                    break;
                }
                // Mid-run foreign announce = the peer restarted its pregame
                // (recovery ladder) — restart ours on the live connection.
                const auto* announce = static_cast<const SyncAnnouncePayload*>(payload);
                if (IsForeignRestartAnnounce(announce)) {
                    Rollback::NetplayLog_Write("PREGAME", -1,
                        "Peer restart announce mid-run: peer_session=0x%08X local_session=0x%08X phase=%s — restarting pregame",
                        announce->session_id, s_sessionId, PregamePhaseName(s_phase));
                    RestartPregame("peer pregame restart announce");
                    // Feed the announce into the fresh run.
                    HandleSyncAnnounce(announce);
                    break;
                }
                HandleSyncAnnounce(announce);
            } else {
                LogPregamePacketAnomaly("Short SyncAnnounce", type, payloadLen, sizeof(SyncAnnouncePayload));
            }
            break;

        case PacketType::SyncConfirm:
            if (payloadLen >= sizeof(SyncConfirmPayload)) {
                if ((s_phase == PregamePhase::Idle ||
                     s_phase == PregamePhase::GameplayHandoff) &&
                    PregameSync_HandleCrossPhaseSessionPacket(type, payload, payloadLen)) {
                    break;
                }
                HandleSyncConfirm(static_cast<const SyncConfirmPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short SyncConfirm", type, payloadLen, sizeof(SyncConfirmPayload));
            }
            break;

        case PacketType::CharSelInput:
            if (payloadLen >= sizeof(CharSelInputPayload)) {
                CharSelPaletteSelect_OnRemoteCatalog(static_cast<const CharSelInputPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short CharSelInput", type, payloadLen, sizeof(CharSelInputPayload));
            }
            break;

        case PacketType::CharSelLock:
            if (payloadLen >= sizeof(CharSelLockPayload)) {
                CharSelSync_OnRemoteLock(static_cast<const CharSelLockPayload*>(payload));
                s_remoteCharSelLocked = true;
            } else {
                LogPregamePacketAnomaly("Short CharSelLock", type, payloadLen, sizeof(CharSelLockPayload));
            }
            break;

        case PacketType::StageSync:
            if (payloadLen >= sizeof(StageSyncPayload)) {
                CharSelSync_OnRemoteStage(static_cast<const StageSyncPayload*>(payload));
                auto* sp = static_cast<const StageSyncPayload*>(payload);
                if (sp->confirmed) s_remoteStageLocked = true;
            } else {
                LogPregamePacketAnomaly("Short StageSync", type, payloadLen, sizeof(StageSyncPayload));
            }
            break;

        case PacketType::ConfigExchange:
            if (payloadLen >= sizeof(ConfigExchangePayload)) {
                OnConfigExchange(static_cast<const ConfigExchangePayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short ConfigExchange", type, payloadLen, sizeof(ConfigExchangePayload));
            }
            break;

        case PacketType::ConfigAck:
            if (payloadLen >= sizeof(ConfigAckPayload)) {
                OnConfigAck(static_cast<const ConfigAckPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short ConfigAck", type, payloadLen, sizeof(ConfigAckPayload));
            }
            break;

        case PacketType::LoadBarrier:
            if (payloadLen >= sizeof(LoadBarrierPayload)) {
                OnLoadBarrier(static_cast<const LoadBarrierPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short LoadBarrier", type, payloadLen, sizeof(LoadBarrierPayload));
            }
            break;

        case PacketType::BaselineReady:
            if (payloadLen >= sizeof(BaselineReadyPayload)) {
                OnBaselineReady(static_cast<const BaselineReadyPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short BaselineReady", type, payloadLen, sizeof(BaselineReadyPayload));
            }
            break;

        case PacketType::BaselineDigest:
            if (payloadLen >= sizeof(BaselineDigestPayload)) {
                OnBaselineDigest(static_cast<const BaselineDigestPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short BaselineDigest", type, payloadLen, sizeof(BaselineDigestPayload));
            }
            break;

        case PacketType::BaselineBreakdown:
            if (payloadLen >= sizeof(BaselineBreakdownPayload)) {
                OnBaselineBreakdown(static_cast<const BaselineBreakdownPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short BaselineBreakdown", type, payloadLen, sizeof(BaselineBreakdownPayload));
            }
            break;

        case PacketType::GameplayStart:
            if (payloadLen >= sizeof(GameplayStartPayload)) {
                OnGameplayStart(static_cast<const GameplayStartPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short GameplayStart", type, payloadLen, sizeof(GameplayStartPayload));
            }
            break;

        default:
            LOG_NETPLAY(LOG_WARNING, "[MatchSetup] Unhandled packet type %u", (unsigned)type);
            Rollback::NetplayLog_Write(
                "PREGAME", -1,
                "Unhandled packet: type=%s(%u) payload=%zu phase=%s session=0x%08X remoteSession=0x%08X",
                PacketTypeName(type),
                (unsigned)type,
                payloadLen,
                PregamePhaseName(s_phase),
                s_sessionId,
                s_remoteSessionId);
            break;
    }
}

// ============================================================================
// Phase updates: session sync + EpochAlign
// ============================================================================

static void SendSyncAnnounce() {
    SyncAnnouncePayload payload{};
    payload.session_id = s_sessionId;
    payload.capability_flags = s_localCapabilities;
    payload.round_count = GetOutgoingSyncRoundOption("send sync announce");

    BarrierProtocol_SendPacket(PacketType::SyncAnnounce,
                              &payload, sizeof(payload));
    s_syncAnnounceSent = true;
    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Sent SyncAnnounce: session=0x%08X caps=0x%02X round_raw=%u",
        s_sessionId,
        s_localCapabilities,
        payload.round_count);
}

static void SendSyncConfirm() {
    SyncConfirmPayload payload{};
    payload.session_id = s_sessionId;
    payload.confirmed = 1;
    payload.assigned_side = s_assignedSide;
    payload.round_count = GetOutgoingSyncRoundOption("send sync confirm");

    BarrierProtocol_SendPacket(PacketType::SyncConfirm,
                              &payload, sizeof(payload));
    s_syncConfirmSent = true;
    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Sent SyncConfirm: session=0x%08X side=%u round_raw=%u",
        s_sessionId,
        s_assignedSide,
        payload.round_count);
}

static DWORD s_lastAnnounceSendTime = 0;
constexpr DWORD ANNOUNCE_RESEND_MS  = 500;

static void UpdateSyncAnnounce() {
    if (!s_syncAnnounceSent) {
        SendSyncAnnounce();
        s_lastAnnounceSendTime = GetTickCount();
    }

    if (s_syncAnnounceSent && !s_remoteSyncAnnounced) {
        DWORD now = GetTickCount();
        if (now - s_lastAnnounceSendTime >= ANNOUNCE_RESEND_MS) {
            LOG_NETPLAY(LOG_DEBUG, "[MatchSetup] Resending SyncAnnounce (no remote announce yet)");
            SendSyncAnnounce();
            s_lastAnnounceSendTime = now;
        }
    }

    if (s_remoteSyncConfirmed) {
        SetStatusFmt("Session confirmed (fast path). Exchanging...");
        SetPhase(PregamePhase::SyncExchange, "remote already confirmed");
        return;
    }

    if (s_remoteSyncAnnounced) {
        SetStatusFmt("Session announced. Confirming...");
        SetPhase(PregamePhase::SyncExchange, "remote announced");
        return;
    }

    if (HandshakeTimedOut(SYNC_TIMEOUT_MS, "sync announce")) {
        RestartPregame("sync announce timeout");
    }
}

static DWORD s_lastConfirmSendTime = 0;
constexpr DWORD CONFIRM_RESEND_MS  = 500;

static void UpdateSyncExchange() {
    if (!s_syncConfirmSent) {
        SessionRole role = Session_GetRole();
        if (role == SessionRole::Host) {
            s_assignedSide = 0;  // Host = P1 by default
        }
        SendSyncConfirm();
        s_lastConfirmSendTime = GetTickCount();
    }

    if (s_syncConfirmSent && !s_remoteSyncConfirmed) {
        DWORD now = GetTickCount();
        if (now - s_lastConfirmSendTime >= CONFIRM_RESEND_MS) {
            LOG_NETPLAY(LOG_DEBUG, "[MatchSetup] Resending SyncConfirm (no remote confirm yet)");
            SendSyncConfirm();
            s_lastConfirmSendTime = now;
        }
    }

    if (s_remoteSyncConfirmed) {
        SetStatusFmt("Session sync confirmed. Aligning epoch...");
        SetPhase(PregamePhase::SyncConfirmed, "both confirmed");
    }

    if (HandshakeTimedOut(SYNC_TIMEOUT_MS, "sync confirm")) {
        RestartPregame("sync confirm timeout");
    }
}

// Actions executed exactly once when the EpochAlign barrier commits: begin
// the frontend epoch under the aligned identity (INV-8: this cancels every
// prior machine by construction) and route to the epoch's first phase.
static void ExecuteEpochAlignCommit() {
    const SessionRole role = Session_GetRole();
    const FrontendPhaseId firstPhase = (FrontendPhaseId)s_alignFirstPhase;

    FrontendInputSync_AbortEpoch("epoch align commit");
    FrontendInputSync_BeginEpoch(
        role,
        s_epoch,
        (uint16_t)FrontendInputSync_ComputeDelayProposal(),
        "epoch align commit");
    Rollback::OwnerDiag_Log("epoch_align_commit");

    Rollback::NetplayLog_Write("PREGAME", -1,
        "EpochAlign COMMITTED: epoch=%u first_phase=%s role=%s fast_path=%d",
        s_epoch,
        FrontendPhaseIdName(firstPhase),
        SessionRoleName(role),
        s_rematchFastPath ? 1 : 0);

    if (firstPhase == FrontendPhaseId::None) {
        // Rematch fast path (§2.5): straight to config exchange; chars/
        // palettes/stage/host_side preserved, host mints fresh seeds.
        s_lockedConfig = s_rematchSnapshot;
        if (role == SessionRole::Host) {
            s_lockedConfig.rng_seed = GetTickCount() ^ 0xDEADBEEF;
            s_lockedConfig.session_seed = s_sessionId;
            s_lockedConfig.round_count = GetOutgoingSyncRoundOption("rematch fast path");
            s_lockedConfig.time_limit = 0;
            GameSettingsSync_ApplyLockedConfig(&s_lockedConfig, "rematch fast path host build");
        }

        Rollback::NetplayLog_Write("PREGAME", -1,
            "Rematch fast path config: p1=%u/%u p2=%u/%u stage=%u host_side=%u session=0x%08X epoch=%u",
            s_lockedConfig.p1_character, s_lockedConfig.p1_palette,
            s_lockedConfig.p2_character, s_lockedConfig.p2_palette,
            s_lockedConfig.stage_id, s_lockedConfig.host_side,
            s_sessionId, s_epoch);

        SetStatusFmt("Rematch! Exchanging match config...");
        SetPhase(PregamePhase::ConfigExchange, "rematch fast path");
        BeginConfigExchangeInternal(&s_lockedConfig);
        return;
    }

    CharSelSync_Begin();
    SetStatusFmt("Character select...");
    SetPhase(PregamePhase::FrontendCharSel, "epoch align commit -> charsel");
}

// SyncConfirmed doubles as the EpochAlign wait state (§2.5). The host mints
// and proposes; the join adopts the host's payload and echoes it; both sides
// consume the commit only once their native mode has completed any required
// exit (INV-10 — no peer enters a synced frontend phase while the other is
// in a different mode).
static void UpdateSyncConfirmed() {
    if (!EnsureRoundOptionBeforeFrontend("sync confirmed before charsel")) {
        RestartPregame("round sync missing before charsel");
        return;
    }

    const SessionRole role = Session_GetRole();

    if (role == SessionRole::Host) {
        if (!s_alignProposed) {
            if (!s_alignEpochMinted) {
                s_epoch = NextEpoch();
                s_alignEpochMinted = true;
            }
            s_alignFirstPhase = s_rematchFastPath
                ? (uint8_t)FrontendPhaseId::None
                : (uint8_t)FrontendPhaseId::CharSel;
            TransitionBarrier_ProposeEpochAlign(
                s_epoch, s_alignFirstPhase, (uint8_t)GetGameMode(), s_sessionId);
            s_alignProposed = true;
        }
    } else {
        // Join: adopt the host's proposal (INV-10: both sides must report the
        // same first_phase — the join echoes the host's payload verbatim).
        uint32_t remoteEpoch = 0;
        uint8_t remoteFirst = 0;
        uint8_t remoteMode = 0;
        if (TransitionBarrier_GetRemoteEpochAlign(&remoteEpoch, &remoteFirst, &remoteMode)) {
            if (remoteEpoch != 0 && (!s_alignProposed || s_epoch != remoteEpoch ||
                                     s_alignFirstPhase != remoteFirst)) {
                const bool expectedFast = s_rematchFastPath;
                const bool hostFast = remoteFirst == (uint8_t)FrontendPhaseId::None;
                if (expectedFast != hostFast) {
                    Rollback::NetplayLog_Write("PREGAME", -1,
                        "EpochAlign first_phase differs from local expectation (local_fast=%d host_first=%u) — adopting host",
                        expectedFast ? 1 : 0, remoteFirst);
                    s_rematchFastPath = hostFast;
                }
                AdoptEpoch(remoteEpoch);
                s_alignFirstPhase = remoteFirst;
                TransitionBarrier_ProposeEpochAlign(
                    s_epoch, s_alignFirstPhase, (uint8_t)GetGameMode(), s_sessionId);
                s_alignProposed = true;
                Rollback::NetplayLog_Write("PREGAME", -1,
                    "EpochAlign adopted from host: epoch=%u first_phase=%u host_native_mode=%u",
                    s_epoch, s_alignFirstPhase, remoteMode);
            }
        }
    }

    if (TransitionBarrier_IsCommitted(NetTransitionKind::EpochAlign)) {
        // Complete any required native exit before acting on the commit
        // (§4.5 adoption step 2): a peer still rendering the win screen
        // finishes its mode-9 exit first.
        if (GetGameMode() == MODE_WINSCREEN) {
            static DWORD s_lastExitWaitLogTick = 0;
            const DWORD now = GetTickCount();
            if (s_lastExitWaitLogTick == 0 || (now - s_lastExitWaitLogTick) >= 1000) {
                s_lastExitWaitLogTick = now;
                Rollback::NetplayLog_Write("PREGAME", -1,
                    "EpochAlign committed but native mode-9 exit pending (sub=%u) — holding commit consumption",
                    GetSubstate());
            }
        } else if (!Rollback::OnlineWiring_MatchEndLadderAllows(
                       NetTransitionKind::EpochAlign)) {
            // INV-9 (M6): the match-end ladder refuses this commit until
            // WinScreenExit and PostMatchDecision committed locally. The
            // barrier keeps re-acking the held proposal; nothing is skipped.
        } else if (TransitionBarrier_ConsumeCommit(NetTransitionKind::EpochAlign)) {
            Rollback::OnlineWiring_MatchEndLadderNotifyConsumed(
                NetTransitionKind::EpochAlign);
            ExecuteEpochAlignCommit();
            return;
        }
    }

    if (HandshakeTimedOut(SYNC_TIMEOUT_MS, "epoch align")) {
        RestartPregame("epoch align timeout");
    }
}

// ============================================================================
// Phase updates: frontend
// ============================================================================

// EpochAlign re-run for the current epoch (§4.6 step 2, idempotent: same
// epoch, re-commit). Frontend machines restart under the unchanged epoch.
static void RealignCurrentEpoch(const char* reason) {
    Rollback::NetplayLog_Write("PREGAME", -1,
        "EpochAlign re-run: epoch=%u phase=%s reason=%s",
        s_epoch, PregamePhaseName(s_phase), reason ? reason : "?");
    CharSelSync_Abort();
    FrontendInputSync_ClearRecoveryRequest();
    TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "epoch realign");
    s_alignProposed = false;   // keep s_alignEpochMinted: SAME epoch re-runs
    SetStatusFmt("Re-synchronizing...");
    SetPhase(PregamePhase::SyncConfirmed, "epoch realign");
}

static void UpdateFrontendCharSel() {
    if (FrontendInputSync_HasRecoveryRequest()) {
        // §4.6/INV-12: a recoverable frontend timeout restarts the pregame
        // under a fresh epoch on the live connection — never a teardown.
        char reason[160];
        _snprintf_s(reason, sizeof(reason), _TRUNCATE,
            "frontend charsel recovery: %s", FrontendInputSync_GetRecoveryReason());
        RestartPregame(reason);
        return;
    }

    CharSelSync_FrameUpdate();

    CharSelSyncSnapshot csSnap{};
    CharSelSync_GetSnapshot(&csSnap);

    s_localCharSelLocked = csSnap.local_confirmed;
    s_remoteCharSelLocked = csSnap.remote_confirmed;

    s_charselLogCounter++;
    if (s_charselLogCounter == 1 || (s_charselLogCounter % 120) == 0) {
        LOG_NETPLAY(LOG_DEBUG,
            "[MatchSetup] CharSel tick=%u lockstep_frame=%u local_input=%u delay=%d "
            "localConfirm=%u remoteConfirm=%u",
            s_charselLogCounter, csSnap.lockstep_frame, csSnap.local_input_frame,
            csSnap.input_delay, csSnap.local_confirmed, csSnap.remote_confirmed);
    }

    if (csSnap.both_characters_locked) {
        SetStatusFmt("Characters locked. Stage select...");
        SetPhase(PregamePhase::FrontendStageSel, "both characters confirmed");
        CharSelSync_BeginStagePhase();
    }
}

static void UpdateFrontendStageSel() {
    if (FrontendInputSync_HasRecoveryRequest()) {
        char reason[160];
        _snprintf_s(reason, sizeof(reason), _TRUNCATE,
            "frontend stagesel recovery: %s", FrontendInputSync_GetRecoveryReason());
        RestartPregame(reason);
        return;
    }

    CharSelSync_FrameUpdate();

    CharSelSyncSnapshot csSnap{};
    CharSelSync_GetSnapshot(&csSnap);

    s_localStageLocked = csSnap.local_stage_confirmed;
    s_remoteStageLocked = csSnap.remote_stage_confirmed;

    if (!csSnap.in_stage_phase && !csSnap.both_stage_locked) {
        s_localStageLocked = false;
        s_remoteStageLocked = false;
        SetStatusFmt("Stage canceled. Character select...");
        SetPhase(PregamePhase::FrontendCharSel, "stage cancel back to charsel");
        return;
    }

    if (csSnap.both_stage_locked) {
        SetStatusFmt("Stage locked. Building match config...");
        SetPhase(PregamePhase::FrontendLocked, "stage confirmed");
    }
}

static void UpdateFrontendLocked() {
    // Build the LockedMatchConfig from CharSelSync results
    CharSelSyncSnapshot csSnap{};
    CharSelSync_GetSnapshot(&csSnap);

    LockedMatchConfig_Clear(&s_lockedConfig);
    s_lockedConfig.p1_character = csSnap.p1_character;
    s_lockedConfig.p1_palette = csSnap.p1_palette;
    s_lockedConfig.p2_character = csSnap.p2_character;
    s_lockedConfig.p2_palette = csSnap.p2_palette;
    s_lockedConfig.stage_id = csSnap.stage_id;

    s_lockedConfig.host_side = s_assignedSide;

    SessionRole role = Session_GetRole();
    if (role == SessionRole::Host) {
        s_lockedConfig.rng_seed = GetTickCount() ^ 0xDEADBEEF;
        s_lockedConfig.session_seed = s_sessionId;
        s_lockedConfig.round_count = GetOutgoingSyncRoundOption("pregame frontend locked");
        s_lockedConfig.time_limit = 0;
        GameSettingsSync_ApplyLockedConfig(&s_lockedConfig, "host config build");
    }

    // Front-end selections are fully resolved — end CharSel lockstep before
    // the game transitions into Mode 7 / Mode 8.
    CharSelSync_Abort();

    SetPhase(PregamePhase::ConfigExchange, "config ready");
    BeginConfigExchangeInternal(&s_lockedConfig);
}

// ============================================================================
// Phase updates: config / load / baseline / handoff
// ============================================================================

static void UpdateConfigExchange() {
    // Host sends config, then waits for ack; Join waits for config, acks it.
    if (s_isHost && !s_configSent) {
        SendConfig();
    }
    if (!s_isHost && s_configReceived && !s_configAgreed) {
        uint32_t hash = LockedMatchConfig_Hash(&s_lockedConfig);
        s_configAgreed = true;
        SendConfigAck(hash, true);
        if (s_remoteDelayReceived) {
            DelayPolicy_NegotiateSession(&s_remoteDelayData);
        }
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] Join accepted early config (hash=0x%08X rounds_raw=%u)",
            hash,
            s_lockedConfig.round_count);
    }

    if ((s_logTickCounter++ % 120) == 0) {
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] ConfigExchange: sent=%s recv=%s agreed=%s elapsed=%lums",
            s_configSent ? "yes" : "no",
            s_configReceived ? "yes" : "no",
            s_configAgreed ? "yes" : "no",
            elapsed);
    }

    if (s_configAgreed) {
        GameSettingsSync_ApplyLockedConfig(&s_lockedConfig, "config agreed");

        // Palette transport is keyed by the final agreed config (§4.3).
        NetplayPaletteRuntime_OnLockedMatchConfig(&s_lockedConfig);

        s_configHashTop = LockedMatchConfig_Hash(&s_lockedConfig);
        s_configAgreedTop = true;
        SetStatusFmt("Config agreed (hash=0x%08X, first to %d). Loading...",
            s_configHashTop,
            GameSettingsSync_RoundsToWin(s_lockedConfig.round_count));
        SetPhase(PregamePhase::ConfigAgreed, "config agreed");
        return;
    }

    if (HandshakeTimedOut(CONFIG_TIMEOUT_MS, "config exchange")) {
        RestartPregame("config exchange timeout");
    }
}

static void UpdateConfigAgreed() {
    SetStatusFmt("Waiting for assets to load...");

    // Notify spectators that selection is committed so they can start charsel
    // bootstrap in parallel with the loading screen.
    if (s_configAgreedTop) {
        SpectatorRuntime_OnSelectionCommitted(&s_lockedConfig);
    }

    SetPhase(PregamePhase::BootstrapLoading, "begin loading");
    BeginLoadingInternal();
}

static void UpdateBootstrapLoading() {
    UpdateBootstrapFreezeForBoundary();

    uint32_t mode = GetGameMode();
    uint32_t sub = GetSubstate();

    if ((s_logTickCounter++ % 120) == 0) {
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] BootstrapLoading: mode=%u sub=%u local=%s remote=%s localFrame=%d remoteFrame=%d elapsed=%lums",
            mode, sub,
            s_localLoaded ? "yes" : "no",
            s_remoteLoaded ? "yes" : "no",
            s_localLoadSimFrame,
            s_remoteLoadSimFrame,
            elapsed);
    }

    // Detect local load completion (same predicates as the old bootstrap).
    bool justLoaded = false;
    if (mode == MODE_MATCH && sub >= 1) {
        justLoaded = true;
    } else if (mode == MODE_PREMATCH_INTRO) {
        justLoaded = true;
    } else if (mode == MODE_CHARSEL && sub >= CHARSEL_SUB_FADE_BACK) {
        justLoaded = true;
    }

    if (justLoaded && !s_localLoaded) {
        s_localLoaded = true;
        LOG_NETPLAY(LOG_INFO, "[MatchSetup] Local loading complete (mode=%u sub=%u)", mode, sub);

        // Defensive RNG seeding (authoritative seed re-asserted at baseline).
        DetVer_SetRngSeed(s_lockedConfig.session_seed);

        SendLoadBarrier();
    }

    if (s_localLoaded && s_remoteLoaded) {
        SetStatusFmt("Assets loaded. Capturing baseline...");
        SetPhase(PregamePhase::BootstrapBaseline, "both loaded");
        BeginBaselineInternal();
        return;
    }

    // F-11: load divergence/timeout → pregame restart with reason, not
    // teardown (asset failure itself surfaces locally).
    if (HandshakeTimedOut(LOAD_TIMEOUT_MS, "load barrier")) {
        RestartPregame("load barrier timeout");
    }
}

static void HandleBaselineMismatch() {
    Rollback::NetplayLog_Write(
        "BASELINE", s_localBaselineSimFrame,
        "Baseline mismatch: local_digest=0x%08X remote_digest=0x%08X local_main=0x%08X remote_main=0x%08X retry_used=%d",
        s_localBaselineDigest,
        s_remoteBaselineDigest,
        s_localBaselineCRC,
        s_remoteBaselineCRC,
        s_baselineRetryUsed ? 1 : 0);
    BaselineSync_LogMismatchAndDump(BuildBaselineStateView());

    if (!s_baselineRetryUsed) {
        // §2.5: retry once under the same epoch. Both sides observe the same
        // mismatch (each holds both digests), so both retry deterministically:
        // clear both baseline sides and recapture/re-exchange at the frozen
        // gameplay boundary.
        s_baselineRetryUsed = true;
        s_remoteBaselineReady = false;
        s_remoteBaselineCRC = 0;
        s_remoteBaselineDigest = 0;
        s_remoteBaselineMode = 0;
        s_remoteBaselineSubstate = 0;
        s_remoteBaselineSimFrame = -1;
        Rollback::NetplayLog_Write("BASELINE", s_localBaselineSimFrame,
            "Baseline retry (once, same epoch %u) — recapturing", s_epoch);
        BeginBaselineInternal();
        s_phaseStartTime = GetTickCount();
        return;
    }

    // Second mismatch = genuine build/content divergence → fail-closed
    // terminal `BaselineMismatch` (ConfirmedDesync class, INV-20).
    SetErrorFmt("Baseline digest mismatch: local=0x%08X remote=0x%08X",
        s_localBaselineDigest,
        s_remoteBaselineDigest);
    Session2_Terminate(Session2TerminalReason::ConfirmedDesync, s_errorText);
    SetPhase(PregamePhase::Error, "baseline mismatch (after retry)");
}

// ── Baseline input-residue normalization (2026-08-17 f0 desync fix) ─────────
// The engine2 sync hash covers both 208-byte global input spans
// (ADDR_P1/P2_INPUT_BUFFER → GameSnapshot input_p1/p2), but the baseline
// AGREEMENT digest historically did not — per-side frontend residue inside
// those spans sailed through "baseline agreed" and guaranteed a confirmed
// desync at rb frame 0. Live evidence (logs 2026-08-17_17-25-3x, both dumps):
//   (a) held/just-pressed navigation words (span offsets +4/+6 and +32/+34)
//       held each side's LAST charsel key — differing by construction;
//   (b) the P2 span's final two bytes overlap the title-screen attract
//       counter LOWORD (dword_8EA000): 0x0072 host vs 0x0000 instB — pure
//       wall-time residue (nicknames verified NOT in these spans).
// Fix: at this synchronized frozen boundary (both peers halted by the load
// barrier immediately before capture) zero the frontend-SAFE input words,
// the 20-byte just-pressed states, and the 2 title-counter bytes on BOTH
// sides. The unsafe charsel tail (committed char IDs/palettes) is
// session-synced data and must NOT be cleared (rematch_cleanup.cpp tail
// guard); it is covered by the agreement digest instead (baseline_sync.cpp),
// so residual divergence there fails loud at baseline, not at f0.
static void ZeroFrontendInputResidueForBaseline() {
    constexpr size_t kSafeSpan = ADDR_P1_INPUT_STATE - ADDR_P1_INPUT_BUFFER;
    static_assert(kSafeSpan == 56,
        "frontend-safe input span changed; re-verify charsel tail layout");
    static const uint8_t zeroSafe[kSafeSpan] = {};
    static const uint8_t zeroState[INPUT_STATE_SIZE] = {};
    static const uint8_t zeroTitle[TITLE_STATE_IN_INPUT_SPAN_SIZE] = {};

    bool ok = true;
    ok &= WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_BUFFER, zeroSafe, sizeof(zeroSafe));
    ok &= WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_BUFFER, zeroSafe, sizeof(zeroSafe));
    ok &= WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_STATE, zeroState, sizeof(zeroState));
    ok &= WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_STATE, zeroState, sizeof(zeroState));
    ok &= WriteMemoryBlockSafe((void*)ADDR_TITLE_SCREEN_STATE, zeroTitle, sizeof(zeroTitle));

    Rollback::NetplayLog_Write("BASELINE", -1,
        "Pre-capture input-residue zero: safe=%u state=%u title=%u ok=%d "
        "(hashed spans must be cross-side identical by construction)",
        (unsigned)sizeof(zeroSafe),
        (unsigned)sizeof(zeroState),
        (unsigned)sizeof(zeroTitle),
        ok ? 1 : 0);
    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Zeroed frontend input residue before baseline capture (ok=%d)",
        ok ? 1 : 0);
}

static void UpdateBootstrapBaseline() {
    UpdateBootstrapFreezeForBoundary();

    if ((s_logTickCounter % 120) == 0) {
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        LOG_NETPLAY(LOG_DEBUG,
            "[MatchSetup] Baseline: localReady=%s remoteReady=%s localDigest=0x%08X remoteDigest=0x%08X agreed=%s elapsed=%lums",
            s_localBaselineReady ? "yes" : "no",
            s_remoteBaselineReady ? "yes" : "no",
            s_localBaselineDigest, s_remoteBaselineDigest,
            s_baselineAgreed ? "yes" : "no",
            elapsed);
    }

    // Capture baseline savestate at match gameplay start (sub 3): the
    // load-barrier freeze holds both peers at the exact same stable point.
    if (!s_localBaselineReady) {
        uint32_t mode = GetGameMode();
        uint32_t sub = GetSubstate();

        if (mode == MODE_MATCH && sub == MATCH_SUB_GAMEPLAY) {
            uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
            LOG_NETPLAY(LOG_INFO, "[MatchSetup] Capturing baseline at mode=%u sub=%u simFrame=%u",
                mode, sub, simFrame);

            s_localBaselineMode = (uint8_t)mode;
            s_localBaselineSubstate = (uint8_t)sub;
            s_localBaselineSimFrame = (int32_t)simFrame;

            // Both peers must have identical CRT rand state for the captured
            // savestate CRCs to match.
            DetVer_SetRngSeed(s_lockedConfig.session_seed);

            // Hashed input spans must be cross-side identical by
            // construction before the capture (f0 desync fix, 2026-08-17).
            ZeroFrontendInputResidueForBaseline();

            if (Savestate_CaptureRollbackBaseline()) {
                const SavestateInfo* info = Savestate_GetRollbackBaselineInfo();
                if (info && info->valid) {
                    s_localBaselineCRC = info->checksum;
                    BaselineBreakdownPayload localBreakdown{};
                    BaselineSync_CaptureLocalBreakdown(&localBreakdown);
                    s_localBaselineDigest = BaselineSync_ComputeAgreementDigest(localBreakdown);
                    s_localBaselineReady = true;
                    SendBaselineReady();
                    SendBaselineDigest(s_localBaselineDigest);

                    SendBaselineBreakdown(localBreakdown);
                    BaselineSync_RecordLocalBreakdown(
                        BuildBaselineStateView(),
                        localBreakdown,
                        s_localBaselineCRC);

                    LOG_NETPLAY(LOG_INFO,
                        "[MatchSetup] Baseline captured: main_crc=0x%08X digest=0x%08X frame=%u rng=0x%08X",
                        s_localBaselineCRC,
                        s_localBaselineDigest,
                        info->frame,
                        info->rng_seed);
                } else {
                    // Local capture failure — recoverable, retry via restart.
                    RestartPregame("failed to capture baseline savestate");
                    return;
                }
            }
        }
    }

    // Baseline rendezvous (§2.5): both sides exchange the typed digest and
    // gameplay opens only after both hold the same value.
    if (s_localBaselineReady &&
        s_remoteBaselineReady &&
        s_baselineDigestSent &&
        s_localBaselineDigest != 0 &&
        s_remoteBaselineDigest != 0 &&
        !s_baselineAgreed) {
        if (s_localBaselineDigest == s_remoteBaselineDigest) {
            s_baselineAgreed = true;
            SetStatusFmt("Baseline agreed. Ready to play!");
            SetPhase(PregamePhase::BootstrapReady, "baseline agreed");
            LOG_NETPLAY(LOG_INFO,
                "[MatchSetup] Baseline agreed: digest=0x%08X main_local=0x%08X main_remote=0x%08X",
                s_localBaselineDigest,
                s_localBaselineCRC,
                s_remoteBaselineCRC);
        } else {
            HandleBaselineMismatch();
        }
        return;
    }

    if (HandshakeTimedOut(BASELINE_TIMEOUT_MS, "baseline agreement")) {
        RestartPregame("baseline agreement timeout");
    }
}

static void UpdateBootstrapReady() {
    UpdateBootstrapFreezeForBoundary();

    // Host sends the authoritative GO once the baseline rendezvous holds.
    if (s_isHost && !s_gameplayStartSent) {
        s_bootstrapFrameAbs = 0;
        if (SendGameplayStart(s_bootstrapFrameAbs)) {
            s_gameplayStart = true;
        }
    }

    if ((s_logTickCounter % 120) == 0) {
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] BootstrapReady: gameplayStart=%s bootstrap_frame_abs=%u host_game_abs_frame=%d elapsed=%lums",
            s_gameplayStart ? "yes" : "no",
            s_bootstrapFrameAbs,
            s_gameplayStartHostGameAbsFrame,
            elapsed);
    }

    if (s_gameplayStart) {
        SetStatusFmt("Gameplay starting!");
        SetPhase(PregamePhase::GameplayHandoff, "gameplay start");

        // Disable load barrier freeze — the rollback session takes over
        InputSyncHooks_SetLoadBarrierFreeze(false);

        // Notify match lifecycle layer — it now owns the match flow
        MatchLifecycle_OnMatchEnter();

        // Arm the match director side (online_wiring until M6): restore the
        // agreed baseline, keep the intro deterministic, defer rollback-owned
        // advance until the mutual startup barrier releases.
        Rollback::OnlineWiring_OnGameplayStart();
        return;
    }

    if (HandshakeTimedOut(BASELINE_TIMEOUT_MS, "gameplay start")) {
        RestartPregame("gameplay start timeout");
    }
}

// ============================================================================
// Reset / restart plumbing
// ============================================================================

static void ResetTrackingStateForNewRun() {
    s_logTickCounter = 0;
    s_charselLogCounter = 0;
    s_configAgreedTop = false;
    s_configHashTop = 0;
    LockedMatchConfig_Clear(&s_lockedConfig);
    // Cross-phase adoption is the charsel path — never the rematch fast path.
    s_rematchFastPath = false;
    s_errorText[0] = '\0';
    s_localCharSelLocked = false;
    s_remoteCharSelLocked = false;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_remoteSessionId = 0;
    s_assignedSide = 0;
    s_remoteCapabilities = 0;
    s_syncAnnounceSent = false;
    s_remoteSyncAnnounced = false;
    s_syncConfirmSent = false;
    s_remoteSyncConfirmed = false;
    s_syncRoundOption = 0;
    s_haveSyncRoundOption = false;
    s_lastAnnounceSendTime = 0;
    s_lastConfirmSendTime = 0;
    s_alignProposed = false;
    s_alignEpochMinted = false;
    s_alignFirstPhase = (uint8_t)FrontendPhaseId::CharSel;
    s_phaseStartTime = GetTickCount();
}

// §4.6 step 3: pregame restart on the LIVE connection under a fresh epoch —
// both roles. The restart announces; the peer either adopts (cross-phase
// path) or its own ladder converges on the same restart.
static void RestartPregame(const char* reason) {
    s_restartCount++;
    Rollback::NetplayLog_Write("PREGAME", -1,
        "=== PREGAME RESTART #%u (fresh epoch, live connection): %s | was phase=%s epoch=%u ===",
        s_restartCount,
        reason ? reason : "?",
        PregamePhaseName(s_phase),
        s_epoch);
    Rollback::NetplayLog_Flush();
    LOG_NETPLAY(LOG_WARNING, "[MatchSetup] Pregame restart: %s", reason ? reason : "?");

    InputSyncHooks_SetLoadBarrierFreeze(false);
    CharSelSync_Abort();
    BootstrapAbortInternal();
    FrontendInputSync_AbortEpoch(reason ? reason : "pregame restart");
    TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "pregame restart");
    NetplayPaletteRuntime_OnDisconnect(reason ? reason : "pregame restart");

    s_phase = PregamePhase::Idle;   // silent phase write; Begin() re-announces
    s_phaseStartTime = GetTickCount();

    if (!PregameSync_Begin()) {
        // Session genuinely unusable (Begin validates the session) — leave it
        // to the supervisor/session layer; nothing is torn down here.
        LOG_NETPLAY(LOG_WARNING,
            "[MatchSetup] Pregame restart Begin() failed — session layer owns recovery");
    }
}

// ── Winscreen same-epoch realign (M6, closes M5 deviation 3) ────────────────
// A Realign escalation latched while the winscreen stream owns the exchange
// (pregame Idle/GameplayHandoff, native mode 9) re-runs EpochAlign for the
// CURRENT epoch with first_phase=WinScreen; on commit both sides restart the
// winscreen lockstep under the unchanged epoch (§4.6 step 2). Timeout falls
// back to the fresh-epoch pregame restart (never a teardown, INV-12).
// Note: this same-epoch re-run is deliberately exempt from the director's
// INV-9 match-end ladder — the ladder gates the NEXT epoch's alignment.
static bool  s_winRealignActive = false;
static DWORD s_winRealignStartTick = 0;
constexpr DWORD kWinRealignTimeoutMs = 5000;

static bool WinRealignContextValid() {
    return (s_phase == PregamePhase::Idle ||
            s_phase == PregamePhase::GameplayHandoff) &&
           GetGameMode() == MODE_WINSCREEN;
}

static void StartWinScreenRealign(const char* reason) {
    Rollback::NetplayLog_Write("PREGAME", -1,
        "WinScreen EpochAlign re-run START: epoch=%u reason=%s",
        s_epoch, reason ? reason : "?");
    FrontendInputSync_ClearRecoveryRequest();
    TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "winscreen realign");
    TransitionBarrier_ProposeEpochAlign(
        s_epoch, (uint8_t)FrontendPhaseId::WinScreen,
        (uint8_t)GetGameMode(), s_sessionId);
    s_winRealignActive = true;
    s_winRealignStartTick = GetTickCount();
}

static void UpdateWinScreenRealign() {
    // Peer-initiated re-run: echo a remote EpochAlign(WinScreen) proposal for
    // the current epoch so the commit can form (both-or-neither, INV-10).
    if (!s_winRealignActive && WinRealignContextValid()) {
        uint32_t remoteEpoch = 0;
        uint8_t remoteFirst = 0;
        uint8_t remoteMode = 0;
        if (TransitionBarrier_RemoteProposed(NetTransitionKind::EpochAlign) &&
            TransitionBarrier_GetRemoteEpochAlign(&remoteEpoch, &remoteFirst, &remoteMode) &&
            remoteEpoch == s_epoch &&
            remoteFirst == (uint8_t)FrontendPhaseId::WinScreen) {
            Rollback::NetplayLog_Write("PREGAME", -1,
                "WinScreen EpochAlign re-run adopted from peer: epoch=%u", s_epoch);
            TransitionBarrier_ProposeEpochAlign(
                s_epoch, (uint8_t)FrontendPhaseId::WinScreen,
                (uint8_t)GetGameMode(), s_sessionId);
            s_winRealignActive = true;
            s_winRealignStartTick = GetTickCount();
        }
    }

    if (!s_winRealignActive) {
        return;
    }

    if (!WinRealignContextValid()) {
        // The winscreen ended underneath the re-run (exit route fired) — the
        // normal ladder owns convergence from here.
        s_winRealignActive = false;
        TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "winscreen realign aborted");
        return;
    }

    if (TransitionBarrier_IsCommitted(NetTransitionKind::EpochAlign) &&
        TransitionBarrier_ConsumeCommit(NetTransitionKind::EpochAlign)) {
        s_winRealignActive = false;
        Rollback::NetplayLog_Write("PREGAME", -1,
            "WinScreen EpochAlign re-run COMMITTED: epoch=%u — restarting winscreen lockstep",
            s_epoch);
        WinScreenSync_RestartLockstep("winscreen epoch realign");
        return;
    }

    if ((DWORD)(GetTickCount() - s_winRealignStartTick) >= kWinRealignTimeoutMs) {
        s_winRealignActive = false;
        TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "winscreen realign timeout");
        RestartPregame("winscreen realign timeout");
    }
}

// Winscreen-context escalation (pregame Idle/GameplayHandoff): a Realign on
// the winscreen stream re-runs EpochAlign under the SAME epoch (M6); charsel
// phases realign in place; everything else restarts under a fresh epoch.
static void HandleResyncEscalation(FrontendResyncEscalation esc) {
    if (esc == FrontendResyncEscalation::None) {
        return;
    }
    if (!IsSessionValid()) {
        return;
    }
    if (esc == FrontendResyncEscalation::Realign &&
        (s_phase == PregamePhase::FrontendCharSel ||
         s_phase == PregamePhase::FrontendStageSel)) {
        RealignCurrentEpoch("interrogation identity mismatch");
        return;
    }
    if (esc == FrontendResyncEscalation::Realign && WinRealignContextValid()) {
        StartWinScreenRealign("interrogation identity mismatch (winscreen stream)");
        return;
    }
    // Everything else (Restart, or Realign with no owning context): pregame
    // restart under a fresh epoch.
    RestartPregame(esc == FrontendResyncEscalation::Restart
        ? "interrogation exhausted"
        : "interrogation identity mismatch (non-charsel phase)");
}

} // anonymous namespace

// ============================================================================
// Public API (preserved PregameSync_* facade)
// ============================================================================

namespace Net {

void PregameSync_OnSessionPacket(PacketType type, const void* payload, size_t payloadLen) {
    HandleSessionPacketInternal(type, payload, payloadLen);
}

void PregameSync_Init() {
    if (s_initialized) return;
    s_phase = PregamePhase::Idle;
    s_configAgreedTop = false;
    s_configHashTop = 0;
    LockedMatchConfig_Clear(&s_lockedConfig);
    s_statusText[0] = '\0';
    s_errorText[0] = '\0';
    s_localCharSelLocked = false;
    s_remoteCharSelLocked = false;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_sessionId = 0;
    s_remoteSessionId = 0;
    s_assignedSide = 0;
    s_remoteCapabilities = 0;
    s_syncAnnounceSent = false;
    s_remoteSyncAnnounced = false;
    s_syncConfirmSent = false;
    s_remoteSyncConfirmed = false;
    s_syncRoundOption = 0;
    s_haveSyncRoundOption = false;
    s_phaseStartTime = 0;
    s_epoch = 0;
    s_epochCounter = 0;
    s_epochSessionId = 0;
    s_alignProposed = false;
    s_alignEpochMinted = false;
    s_restartCount = 0;
    ResetRemoteDelayData();
    BaselineSync_Reset();

    FrontendInputSync_Init();
    FrontendInputSync_SetWinScreenSendGate(&PregameWinScreenSendGate);
    FrontendInputSync_SetTransportHealthyGate(&TransportHealthyGate);
    CharSelSync_Init();

    s_initialized = true;
    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Initialized");
}

void PregameSync_Shutdown() {
    if (!s_initialized) return;

    BootstrapAbortInternal();
    CharSelSync_Shutdown();
    FrontendInputSync_SetWinScreenSendGate(nullptr);
    FrontendInputSync_SetTransportHealthyGate(nullptr);
    FrontendInputSync_Shutdown();

    s_phase = PregamePhase::Idle;
    s_initialized = false;
    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Shutdown");
}

void PregameSync_FrameUpdate() {
    if (!s_initialized) return;

    // The INV-11 ladder escalation can latch in any regime (the winscreen
    // stream interrogates while pregame is idle) — consume it first.
    HandleResyncEscalation(FrontendInputSync_ConsumeResyncEscalation());

    // Winscreen same-epoch realign driver (M6): runs in Idle/GameplayHandoff
    // where the winscreen stream lives.
    UpdateWinScreenRealign();

    if (s_phase == PregamePhase::Idle || s_phase == PregamePhase::GameplayHandoff) return;

    // Check session validity — a genuinely dead session routes to the UI
    // funnel (the supervisor is the network-originated authority; this is
    // the "session object already gone" guard).
    if (!IsSessionValid()) {
        if (s_phase != PregamePhase::Error) {
            NetMenu::HandleDisconnection("Session lost during pre-game sync");
        }
        return;
    }

    // Rematch fast path: freeze coverage while the game races toward the
    // Mode 8 gameplay boundary during Sync*/Config* phases.
    if (s_rematchFastPath) {
        UpdateBootstrapFreezeForBoundary();
    }

    switch (s_phase) {
        case PregamePhase::SyncAnnounce:      UpdateSyncAnnounce();      break;
        case PregamePhase::SyncExchange:      UpdateSyncExchange();      break;
        case PregamePhase::SyncConfirmed:     UpdateSyncConfirmed();     break;
        case PregamePhase::FrontendCharSel:   UpdateFrontendCharSel();   break;
        case PregamePhase::FrontendStageSel:  UpdateFrontendStageSel();  break;
        case PregamePhase::FrontendLocked:    UpdateFrontendLocked();    break;
        case PregamePhase::ConfigExchange:    UpdateConfigExchange();    break;
        case PregamePhase::ConfigAgreed:      UpdateConfigAgreed();      break;
        case PregamePhase::BootstrapLoading:  UpdateBootstrapLoading();  break;
        case PregamePhase::BootstrapBaseline: UpdateBootstrapBaseline(); break;
        case PregamePhase::BootstrapReady:    UpdateBootstrapReady();    break;
        case PregamePhase::Error:
            // Stay in error until Abort() is called
            break;
        default:
            break;
    }
}

static bool s_beginInProgress = false;

bool PregameSync_Begin() {
    if (!s_initialized) return false;
    if (s_beginInProgress) return false;
    if (s_phase != PregamePhase::Idle) {
        // Rematch flow reaches CharSel with PregameSync still in terminal
        // GameplayHandoff from the previous match. Allow an explicit restart.
        if (s_phase == PregamePhase::GameplayHandoff ||
            s_phase == PregamePhase::Error) {
            LOG_NETPLAY(LOG_INFO,
                "[MatchSetup] Begin requested from terminal phase %s — resetting for restart",
                PregamePhaseName(s_phase));

            InputSyncHooks_SetLoadBarrierFreeze(false);
            CharSelSync_Abort();
            BootstrapAbortInternal();
            FrontendInputSync_AbortEpoch("pregame restart begin");
            TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "pregame restart begin");
            SetPhase(PregamePhase::Idle, "restart begin");
        } else {
            LOG_NETPLAY(LOG_WARNING, "[MatchSetup] Begin called but phase is %s",
                PregamePhaseName(s_phase));
            return false;
        }
    }

    if (!IsSessionValid()) {
        LOG_NETPLAY(LOG_WARNING, "[MatchSetup] Begin called but session not valid");
        return false;
    }

    s_beginInProgress = true;

    ResetTrackingStateForNewRun();
    BootstrapAbortInternal();
    LockedMatchConfig_Clear(&s_rematchSnapshot);

    // Fresh 32-bit session-run identifier (seeds/config scope; the epoch is
    // minted separately at EpochAlign). Join adopts the host's at confirm.
    s_sessionId = GetTickCount() ^ (uint32_t)(uintptr_t)&s_phase;
    TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "pregame begin");
    NetplayPaletteRuntime_OnDisconnect("pregame begin reset");
    Rollback::OwnerDiag_Log("pregame_begin");

    // Start with initial session sync (announce → exchange → epoch align).
    SetStatusFmt("Synchronizing session...");
    SetPhase(PregamePhase::SyncAnnounce, "begin");

    s_beginInProgress = false;
    LOG_NETPLAY(LOG_INFO, "[MatchSetup] Pre-game sync started (session=0x%08X)", s_sessionId);
    return true;
}

bool PregameSync_BeginRematch(const LockedMatchConfig* previousConfig) {
    if (!s_initialized || !previousConfig) {
        return false;
    }

    // Copy before Begin(): previousConfig routinely aliases s_lockedConfig,
    // which Begin's reset clears.
    const LockedMatchConfig snapshot = *previousConfig;

    if (!PregameSync_Begin()) {
        LOG_NETPLAY(LOG_WARNING,
            "[MatchSetup] BeginRematch: Begin() failed (phase=%s)",
            PregamePhaseName(s_phase));
        return false;
    }

    // Begin() cleared the flag — arm the fast path after the shared reset.
    s_rematchSnapshot = snapshot;
    s_rematchFastPath = true;
    SetStatusFmt("Rematch! Synchronizing session...");

    Rollback::NetplayLog_Write("PREGAME", -1,
        "Rematch fast path armed: p1=%u/%u p2=%u/%u stage=%u host_side=%u session=0x%08X",
        snapshot.p1_character, snapshot.p1_palette,
        snapshot.p2_character, snapshot.p2_palette,
        snapshot.stage_id, snapshot.host_side,
        s_sessionId);
    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Rematch fast path armed (frontend charsel skipped; EpochAlign first_phase=None)");
    return true;
}

void PregameSync_Abort(const char* reason) {
    if (s_phase == PregamePhase::Idle) return;

    LOG_NETPLAY(LOG_WARNING, "[MatchSetup] Abort: %s (was %s)",
        reason ? reason : "?", PregamePhaseName(s_phase));

    // Always clear load barrier freeze on abort — ensure game isn't stuck
    InputSyncHooks_SetLoadBarrierFreeze(false);

    CharSelSync_Abort();
    BootstrapAbortInternal();
    FrontendInputSync_AbortEpoch(reason ? reason : "pregame abort");
    TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "pregame abort");

    SetErrorFmt("%s", reason ? reason : "Pre-game sync aborted");
    SetPhase(PregamePhase::Idle, "aborted");

    s_rematchFastPath = false;
    s_configAgreedTop = false;
    s_configHashTop = 0;
    s_localCharSelLocked = false;
    s_remoteCharSelLocked = false;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_syncAnnounceSent = false;
    s_remoteSyncAnnounced = false;
    s_syncConfirmSent = false;
    s_remoteSyncConfirmed = false;
    s_syncRoundOption = 0;
    s_haveSyncRoundOption = false;
    s_lastAnnounceSendTime = 0;
    s_lastConfirmSendTime = 0;
    s_logTickCounter = 0;
    s_charselLogCounter = 0;
    s_alignProposed = false;
    s_alignEpochMinted = false;
    NetplayPaletteRuntime_OnDisconnect(reason ? reason : "pregame abort");
}

PregamePhase PregameSync_GetPhase() {
    return s_phase;
}

bool PregameSync_IsActive() {
    return s_phase != PregamePhase::Idle && s_phase != PregamePhase::GameplayHandoff;
}

void PregameSync_GetSnapshot(PregameSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = PregameSync_IsActive();
    out->phase = s_phase;

    // Session sync
    out->session_id = s_sessionId;
    out->sync_confirmed = s_remoteSyncConfirmed && s_syncConfirmSent;
    out->assigned_side = s_assignedSide;

    // CharSel/StageSel
    out->local_charsel_locked = s_localCharSelLocked;
    out->remote_charsel_locked = s_remoteCharSelLocked;
    out->local_stage_locked = s_localStageLocked;
    out->remote_stage_locked = s_remoteStageLocked;

    // Bootstrap
    out->local_loaded = s_localLoaded;
    out->remote_loaded = s_remoteLoaded;
    out->local_baseline_ready = s_localBaselineReady;
    out->remote_baseline_ready = s_remoteBaselineReady;

    // Config
    out->config_agreed = s_configAgreedTop;
    out->config_hash = s_configHashTop;
    if (s_configAgreedTop) {
        out->round_count_valid = true;
        out->round_count = s_lockedConfig.round_count;
    } else if (s_haveSyncRoundOption) {
        out->round_count_valid = true;
        out->round_count = s_syncRoundOption;
    }
    if (out->round_count_valid) {
        out->rounds_to_win = GameSettingsSync_RoundsToWin(out->round_count);
        GameSettingsSync_FormatRoundLabel(
            out->round_count,
            out->rounds_label,
            sizeof(out->rounds_label));
    }

    // Status
    strncpy_s(out->status_text, sizeof(out->status_text), s_statusText, _TRUNCATE);
    strncpy_s(out->error_text, sizeof(out->error_text), s_errorText, _TRUNCATE);
}

const LockedMatchConfig* PregameSync_GetLockedConfig() {
    if (!s_configAgreedTop) return nullptr;
    return &s_lockedConfig;
}

bool PregameSync_IsComplete() {
    return s_phase == PregamePhase::GameplayHandoff;
}

uint32_t PregameSync_GetCurrentEpoch() {
    return s_epoch;
}

void PregameSync_GetBootstrapInfo(PregameBootstrapInfo* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->local_baseline_crc = s_localBaselineCRC;
    out->remote_baseline_crc = s_remoteBaselineCRC;
    out->local_load_sim_frame = s_localLoadSimFrame;
    out->remote_load_sim_frame = s_remoteLoadSimFrame;
    out->local_baseline_sim_frame = s_localBaselineSimFrame;
    out->remote_baseline_sim_frame = s_remoteBaselineSimFrame;
    out->bootstrap_frame_abs = s_bootstrapFrameAbs;
    out->gameplay_start_host_game_abs_frame = s_gameplayStartHostGameAbsFrame;
}

bool PregameSync_HandleCrossPhaseSessionPacket(PacketType type,
                                               const void* payload,
                                               size_t payloadLen) {
    if (!s_initialized) {
        return false;
    }
    if (type != PacketType::SyncAnnounce && type != PacketType::SyncConfirm) {
        return false;
    }
    if (s_phase != PregamePhase::Idle && s_phase != PregamePhase::GameplayHandoff) {
        return false;
    }

    // ── Win-screen deferral (2026-08-17, run 21-58) ────────────────────────
    // The peer resolved the continue prompt a few consumed frames before us
    // and announced immediately (PregameSync_Begin / _BeginRematch both send
    // SyncAnnounce). Our own lockstep stream carries the IDENTICAL decision
    // and resolves within those frames — so this packet is not news, it is a
    // few frames early.
    //
    // Acting on it destroyed the match: the restart aborts the win-screen
    // phase, but nothing drives the game out of mode 9 sub 4, so the screen
    // sits there with the lockstep down and the fail-closed input guard holds
    // neutral forever (host had P2's YES latched, P1's YES was 4 consumed
    // frames away, peer had already resolved REMATCH and announced).
    //
    // The peer retransmits every ~250 ms while it waits for our announce, so
    // ignoring it costs nothing: the normal handler picks the retransmit up
    // the moment our own resolution lands. The wall clock below is a recovery
    // bound only — it gates nothing that either side's decision depends on.
    static DWORD s_deferStartTick = 0;
    static DWORD s_lastDeferLogTick = 0;
    if (WinScreenSync_IsActive() && ContinueFlow_ShouldHoldWinScreenFinalize()) {
        const DWORD now = GetTickCount();
        if (s_deferStartTick == 0) {
            s_deferStartTick = now;
        }
        constexpr DWORD kMaxDeferMs = 3000;
        if ((now - s_deferStartTick) < kMaxDeferMs) {
            if (s_lastDeferLogTick == 0 || (now - s_lastDeferLogTick) >= 500) {
                s_lastDeferLogTick = now;
                Rollback::NetplayLog_Write(
                    "PREGAME", -1,
                    "Cross-phase %s deferred: win-screen decision still in flight "
                    "(consume=%u remote_latest=%u waited=%ums)",
                    PacketTypeName(type),
                    WinScreenSync_GetConsumeFrame(),
                    WinScreenSync_GetRemoteLatestFrame(),
                    (unsigned)(now - s_deferStartTick));
            }
            return true;  // consumed: peer retransmits
        }
        // Deferral exhausted — our stream is genuinely gone (the peer left the
        // win screen and stopped producing frames). Take the restart, but
        // drive mode 9 off screen first so the recovery cannot strand the
        // game on an ownerless continue prompt.
        Rollback::NetplayLog_Write(
            "PREGAME", -1,
            "Cross-phase %s deferral exhausted after %ums — releasing win-screen route",
            PacketTypeName(type), (unsigned)(now - s_deferStartTick));
        ContinueFlow_ForceExitToCharsel("cross-phase restart, win-screen stream lost");
    }
    // One deferral budget per win-screen route, not per process.
    s_deferStartTick = 0;
    s_lastDeferLogTick = 0;

    const SessionRole role = Session_GetRole();
    if (role != SessionRole::Join) {
        // Host: the join peer is announcing a pregame (re)start. Restart our
        // own pregame sync: Begin() mints the authoritative session id and a
        // fresh epoch, and the join side adopts them.
        Rollback::NetplayLog_Write(
            "PREGAME", -1,
            "Cross-phase session sync as HOST — restarting pregame: type=%s phase=%s lifecycle=%s",
            PacketTypeName(type),
            PregamePhaseName(s_phase),
            MatchLifecyclePhaseName(MatchLifecycle_GetPhase()));
        Rollback::NetplayLog_Flush();

        Rollback::OnlineWiring_OnRematch();
        if (s_phase == PregamePhase::GameplayHandoff) {
            InputSyncHooks_SetLoadBarrierFreeze(false);
            CharSelSync_Abort();
            BootstrapAbortInternal();
            SetPhase(PregamePhase::Idle, "cross-phase host restart");
        }
        const bool restarted = PregameSync_Begin();
        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] Host pregame restart via cross-phase %s: %s",
            PacketTypeName(type),
            restarted ? "started" : "FAILED");
        return restarted;
    }

    Rollback::NetplayLog_Write(
        "PREGAME", -1,
        "Cross-phase session sync adopt (join): type=%s phase=%s winscreen=%d handoff=%d lifecycle=%s",
        PacketTypeName(type),
        PregamePhaseName(s_phase),
        WinScreenSync_IsActive() ? 1 : 0,
        WinScreenSync_IsHandoffComplete() ? 1 : 0,
        MatchLifecyclePhaseName(MatchLifecycle_GetPhase()));
    Rollback::NetplayLog_Flush();

    Rollback::OnlineWiring_OnRematch();

    if (s_phase == PregamePhase::GameplayHandoff) {
        InputSyncHooks_SetLoadBarrierFreeze(false);
        CharSelSync_Abort();
        BootstrapAbortInternal();
        SetPhase(PregamePhase::Idle, "cross-phase adopt");
    }

    ResetTrackingStateForNewRun();
    TransitionBarrier_Clear(NetTransitionKind::EpochAlign, "cross-phase adopt");
    FrontendInputSync_AbortEpoch("cross-phase adopt");
    NetplayPaletteRuntime_OnDisconnect("cross-phase pregame adopt");

    if (type == PacketType::SyncAnnounce) {
        if (payloadLen < sizeof(SyncAnnouncePayload)) {
            LogPregamePacketAnomaly("Short SyncAnnounce", type, payloadLen, sizeof(SyncAnnouncePayload));
            return true;
        }

        const SyncAnnouncePayload* announce =
            static_cast<const SyncAnnouncePayload*>(payload);
        s_sessionId = announce->session_id;
        Rollback::OwnerDiag_Log("cross_phase_pregame_adopt");

        SetStatusFmt("Synchronizing session...");
        SetPhase(PregamePhase::SyncAnnounce, "cross-phase announce received");
        HandleSyncAnnounce(announce);

        LOG_NETPLAY(LOG_INFO,
            "[MatchSetup] Adopted host rematch session via cross-phase SyncAnnounce: session=0x%08X",
            s_sessionId);
        return true;
    }

    if (payloadLen < sizeof(SyncConfirmPayload)) {
        LogPregamePacketAnomaly("Short SyncConfirm", type, payloadLen, sizeof(SyncConfirmPayload));
        return true;
    }

    const SyncConfirmPayload* confirm =
        static_cast<const SyncConfirmPayload*>(payload);
    s_sessionId = confirm->session_id;
    Rollback::OwnerDiag_Log("cross_phase_pregame_adopt_confirm");

    SetStatusFmt("Synchronizing session...");
    SetPhase(PregamePhase::SyncAnnounce, "cross-phase confirm fallback");
    HandleSyncConfirm(confirm);

    LOG_NETPLAY(LOG_INFO,
        "[MatchSetup] Adopted host rematch session via cross-phase SyncConfirm: session=0x%08X",
        s_sessionId);
    return true;
}

} // namespace Net
