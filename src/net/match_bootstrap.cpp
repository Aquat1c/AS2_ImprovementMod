/**
 * Alice Senki 2 - Match Bootstrap
 *
 * Post-selection bootstrap: config exchange, load barrier, baseline
 * capture, baseline agreement, and gameplay start signal.
 */

#include "net/match_bootstrap.h"
#include "net/baseline_sync.h"
#include "net/barrier_protocol.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/locked_match_config.h"
#include "net/delay_policy.h"
#include "net/game_settings_sync.h"
#include "rollback/netplay_log.h"
#include "rollback/savestate.h"
#include "rollback/determinism_verify.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace {

using namespace Net;

// ============================================================================
// Bootstrap sub-phases
// ============================================================================

enum class BootPhase : uint8_t {
    Idle = 0,
    ConfigExchange,
    Loading,
    Baseline,
    Ready,
    Done,
    Error,
};

static const char* BootPhaseName(BootPhase phase) {
    switch (phase) {
        case BootPhase::Idle:           return "Idle";
        case BootPhase::ConfigExchange: return "ConfigExchange";
        case BootPhase::Loading:        return "Loading";
        case BootPhase::Baseline:       return "Baseline";
        case BootPhase::Ready:          return "Ready";
        case BootPhase::Done:           return "Done";
        case BootPhase::Error:          return "Error";
        default:                        return "Unknown";
    }
}

// ============================================================================
// Internal state
// ============================================================================

static bool            s_initialized        = false;
static BootPhase       s_phase              = BootPhase::Idle;
static LockedMatchConfig s_config           = {};
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
static uint8_t         s_localBaselineMode      = 0;
static uint8_t         s_localBaselineSubstate  = 0;
static int32_t         s_localBaselineSimFrame  = -1;
static uint8_t         s_remoteBaselineMode     = 0;
static uint8_t         s_remoteBaselineSubstate = 0;
static int32_t         s_remoteBaselineSimFrame = -1;

// Gameplay start
static bool            s_gameplayStart      = false;
static uint32_t        s_bootstrapFrameAbs  = 0;
static bool            s_gameplayStartSent  = false;
static int32_t         s_gameplayStartHostGameAbsFrame = -1;

// Timeout tracking
static DWORD           s_loadStartTime      = 0;
static DWORD           s_baselineStartTime  = 0;
constexpr DWORD        LOAD_BARRIER_TIMEOUT_MS = 30000;  // 30s
constexpr DWORD        BASELINE_TIMEOUT_MS    = 15000;  // 15s

// Error
static char            s_error[128]         = "";

// Periodic logging counter (for rate-limiting state dumps)
static uint32_t        s_logTickCounter     = 0;

// Remote delay negotiation data (received from peer)
static bool            s_remoteDelayReceived = false;
static DelayNegotiationData s_remoteDelayData = {};

// ============================================================================
// Helpers
// ============================================================================

static void SetError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_error, sizeof(s_error), _TRUNCATE, fmt, ap);
    va_end(ap);
    s_phase = BootPhase::Error;
}

static void ResetRemoteDelayData() {
    memset(&s_remoteDelayData, 0, sizeof(s_remoteDelayData));
    s_remoteDelayData.local_input_delay = DELAY_DEFAULT_PREF;
    s_remoteDelayData.max_rollback = ROLLBACK_BUDGET_DEFAULT;
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
    view.phase_name = BootPhaseName(s_phase);
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
    view.session_seed = s_config.session_seed;
    return view;
}

static void SendConfig() {
    ConfigExchangePayload payload{};
    payload.p1_character = s_config.p1_character;
    payload.p1_palette = s_config.p1_palette;
    payload.p2_character = s_config.p2_character;
    payload.p2_palette = s_config.p2_palette;
    payload.stage_id = s_config.stage_id;
    payload.host_side = s_config.host_side;
    payload.round_count = s_config.round_count;
    payload.time_limit = s_config.time_limit;
    payload.rng_seed = s_config.rng_seed;
    payload.session_seed = s_config.session_seed;

    // Announce only the local peer's gameplay delay and rollback budget.
    DelayNegotiationData delayData{};
    DelayPolicy_BuildNegotiationData(&delayData);
    payload.my_input_delay = (uint8_t)delayData.local_input_delay;
    payload.my_max_rollback = (uint8_t)delayData.max_rollback;

    const bool sent = BarrierProtocol_SendPacket(PacketType::ConfigExchange,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchBoot] Failed to send ConfigExchange");
        Rollback::NetplayLog_Write(
            "BARRIER", -1,
            "ERROR: ConfigExchange queue failed: hash=0x%08X my_delay=%d my_max_rb=%d",
            LockedMatchConfig_Hash(&s_config),
            delayData.local_input_delay,
            delayData.max_rollback);
        return;
    }

    s_configSent = true;
    LOG_NETPLAY(LOG_INFO,
        "[MatchBoot] Sent config (hash=0x%08X rounds_raw=%u rounds_to_win=%d my_delay=%d my_max_rb=%d)",
        LockedMatchConfig_Hash(&s_config),
        s_config.round_count,
        GameSettingsSync_RoundsToWin(s_config.round_count),
        delayData.local_input_delay, delayData.max_rollback);
}

static void SendConfigAck(uint32_t hash, bool accepted) {
    ConfigAckPayload payload{};
    payload.config_hash = hash;
    payload.accepted = accepted ? 1 : 0;

    // Announce only the local peer's gameplay delay and rollback budget.
    DelayNegotiationData delayData{};
    DelayPolicy_BuildNegotiationData(&delayData);
    payload.my_input_delay = (uint8_t)delayData.local_input_delay;
    payload.my_max_rollback = (uint8_t)delayData.max_rollback;

    const bool sent = BarrierProtocol_SendPacket(PacketType::ConfigAck,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchBoot] Failed to send ConfigAck");
        Rollback::NetplayLog_Write(
            "BARRIER", -1,
            "ERROR: ConfigAck queue failed: hash=0x%08X accepted=%d my_delay=%d my_max_rb=%d",
            hash,
            accepted ? 1 : 0,
            delayData.local_input_delay,
            delayData.max_rollback);
        return;
    }

    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent ConfigAck: hash=0x%08X accepted=%d my_delay=%d my_max_rb=%d",
        hash, accepted ? 1 : 0,
        delayData.local_input_delay, delayData.max_rollback);
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
        LOG_WARN("[MatchBoot] Failed to send LoadBarrier");
        Rollback::NetplayLog_Write(
            "BARRIER", s_localLoadSimFrame,
            "ERROR: LoadBarrier queue failed: phase=%s local=%u/%u/%d remote=%u/%u/%d",
            BootPhaseName(s_phase),
            s_localLoadMode,
            s_localLoadSubstate,
            s_localLoadSimFrame,
            s_remoteLoadMode,
            s_remoteLoadSubstate,
            s_remoteLoadSimFrame);
        return;
    }

    s_loadBarrierSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent LoadBarrier: mode=%u sub=%u simFrame=%d",
        payload.mode, payload.substate, s_localLoadSimFrame);
    Rollback::NetplayLog_Write(
        "BARRIER", s_localLoadSimFrame,
        "Local LoadBarrier sent: phase=%s local=%u/%u/%d remote=%u/%u/%d local_loaded=%d remote_loaded=%d",
        BootPhaseName(s_phase),
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
        LOG_WARN("[MatchBoot] Failed to send BaselineReady");
        Rollback::NetplayLog_Write(
            "BASELINE", s_localBaselineSimFrame,
            "ERROR: BaselineReady queue failed: local=%u/%u/%d remote_ready=%d phase=%s",
            s_localBaselineMode,
            s_localBaselineSubstate,
            s_localBaselineSimFrame,
            s_remoteBaselineReady ? 1 : 0,
            BootPhaseName(s_phase));
        return;
    }

    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent BaselineReady: mode=%u sub=%u simFrame=%d",
        payload.mode, payload.substate, s_localBaselineSimFrame);
}

static void SendBaselineDigest(uint32_t crc) {
    BaselineDigestPayload payload{};
    payload.crc32 = crc;

    const bool sent = BarrierProtocol_SendPacket(PacketType::BaselineDigest,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchBoot] Failed to send BaselineDigest");
        Rollback::NetplayLog_Write(
            "BASELINE", s_localBaselineSimFrame,
            "ERROR: BaselineDigest queue failed: digest=0x%08X local_main=0x%08X remote_main=0x%08X phase=%s",
            crc,
            s_localBaselineCRC,
            s_remoteBaselineCRC,
            BootPhaseName(s_phase));
        return;
    }

    s_baselineDigestSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent BaselineDigest: crc=0x%08X", crc);
}

static void SendBaselineBreakdown(const BaselineBreakdownPayload& payload) {
    const bool sent = BarrierProtocol_SendPacket(PacketType::BaselineBreakdown,
                                                 &payload, sizeof(payload));
    if (!sent) {
        LOG_WARN("[MatchBoot] Failed to send BaselineBreakdown");
        Rollback::NetplayLog_Write(
            "BASELINE", (int32_t)payload.sim_frame,
            "ERROR: BaselineBreakdown queue failed: main=0x%08X digest=0x%08X phase=%s",
            payload.main_crc,
            BaselineSync_ComputeAgreementDigest(payload),
            BootPhaseName(s_phase));
        return;
    }
    LOG_NETPLAY(LOG_INFO,
        "[MatchBoot] Sent BaselineBreakdown: main=0x%08X header=0x%08X context=0x%08X rng=0x%08X sim=%u display=%u",
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
        LOG_WARN("[MatchBoot] Failed to send GameplayStart");
        Rollback::NetplayLog_Write(
            "HANDOFF", s_gameplayStartHostGameAbsFrame,
            "ERROR: GameplayStart queue failed: bootstrap_frame_abs=%u host_game_abs_frame=%d phase=%s baseline_agreed=%d",
            frame,
            s_gameplayStartHostGameAbsFrame,
            BootPhaseName(s_phase),
            s_baselineAgreed ? 1 : 0);
        return false;
    }

    s_gameplayStartSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent GameplayStart: bootstrap_frame_abs=%u host_game_abs_frame=%d",
        frame, s_gameplayStartHostGameAbsFrame);
    return true;
}

// ============================================================================
// Phase update functions
// ============================================================================

static void UpdateConfigExchange() {
    // Host sends config, then waits for ack
    // Join waits for config, then sends ack
    if (s_isHost && !s_configSent) {
        SendConfig();
    }

    // Join: if we already received config before entering this phase
    // (race condition), agree + ack now.
    if (!s_isHost && s_configReceived && !s_configAgreed) {
        uint32_t hash = LockedMatchConfig_Hash(&s_config);
        s_configAgreed = true;
        SendConfigAck(hash, true);
        if (s_remoteDelayReceived) {
            DelayPolicy_NegotiateSession(&s_remoteDelayData);
        }
        LOG_NETPLAY(LOG_INFO,
            "[MatchBoot] Join accepted early config (hash=0x%08X rounds_raw=%u rounds_to_win=%d remote_delay=%d stall_threshold=%d)",
            hash,
            s_config.round_count,
            GameSettingsSync_RoundsToWin(s_config.round_count),
            DelayPolicy_GetRemoteAnnouncedDelay(),
            DelayPolicy_GetStallThreshold());
    }
}

static void UpdateLoading() {
    // Monitor game state to detect when assets are loaded
    // In charsel flow: substate progresses through LOADING(9) to PREMATCH(10)
    uint32_t mode = GetGameMode();
    uint32_t sub = GetSubstate();

    // Periodic state dump every ~1s (60 frames) while waiting
    if ((s_logTickCounter++ % 60) == 0) {
        DWORD elapsed = s_loadStartTime ? (GetTickCount() - s_loadStartTime) : 0;
        LOG_NETPLAY(LOG_DEBUG, "[MatchBoot] Loading state: mode=%u sub=%u local=%s remote=%s elapsed=%lums",
            mode, sub,
            s_localLoaded ? "yes" : "no",
            s_remoteLoaded ? "yes" : "no",
            elapsed);
    }

    // Consider loaded when we reach MODE_MATCH with substate >= MATCH_SETUP(1)
    // or when charsel leaves loading substate
    bool justLoaded = false;

    if (mode == MODE_MATCH && sub >= 1) {
        justLoaded = true;
    } else if (mode == MODE_PREMATCH_INTRO) {
        // VS_HUMAN flow goes CharSel → Mode 7 (prematch intro/loading) → Mode 8.
        // Mode 7 means we've left CharSel and are running the VS cinematic.
        // Baseline capture has its own MODE_MATCH sub>=2 gate, so this is safe.
        justLoaded = true;
    } else if (mode == MODE_CHARSEL && sub >= CHARSEL_SUB_FADE_BACK) {
        justLoaded = true;
    }

    if (justLoaded && !s_localLoaded) {
        s_localLoaded = true;
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Local loading complete (mode=%u sub=%u)", mode, sub);

        // Defensive RNG seeding: ensure the CRT rand state is set to the
        // agreed session seed as early as possible.  The authoritative seed
        // is also applied right before baseline capture, but seeding here
        // prevents any stray rand() calls between loading and baseline from
        // diverging the two peers.
        DetVer_SetRngSeed(s_config.session_seed);

        SendLoadBarrier();
    }

    // Timeout check
    if (s_loadStartTime && (GetTickCount() - s_loadStartTime) >= LOAD_BARRIER_TIMEOUT_MS) {
        SetError("Load barrier timed out (30s)");
    }
}

static void UpdateBaseline() {
    // Periodic state dump every ~1s (60 frames) while waiting
    if ((s_logTickCounter % 60) == 0) {
        uint32_t m = GetGameMode();
        uint32_t s = GetSubstate();
        DWORD elapsed = s_baselineStartTime ? (GetTickCount() - s_baselineStartTime) : 0;
        LOG_NETPLAY(LOG_DEBUG,
            "[MatchBoot] Baseline state: mode=%u sub=%u localReady=%s remoteReady=%s "
            "localMain=0x%08X remoteMain=0x%08X localDigest=0x%08X remoteDigest=0x%08X "
            "digestSent=%s agreed=%s elapsed=%lums",
            m, s,
            s_localBaselineReady ? "yes" : "no",
            s_remoteBaselineReady ? "yes" : "no",
            s_localBaselineCRC, s_remoteBaselineCRC,
            s_localBaselineDigest, s_remoteBaselineDigest,
            s_baselineDigestSent ? "yes" : "no",
            s_baselineAgreed ? "yes" : "no",
            elapsed);
    }

    // Capture baseline savestate at match gameplay start (sub 3).
    // The load-barrier freeze holds the game at sub 3 while we wait,
    // so BOTH peers capture at the exact same stable point.
    // Previous code targeted sub==2 but that substate is too transient
    // (sub 0→1→2→3 all advance within a few frames).
    if (!s_localBaselineReady) {
        uint32_t mode = GetGameMode();
        uint32_t sub = GetSubstate();

        if (mode == MODE_MATCH && sub == MATCH_SUB_GAMEPLAY) {
            uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
            LOG_NETPLAY(LOG_INFO, "[MatchBoot] Capturing baseline at mode=%u sub=%u simFrame=%u",
                mode, sub, simFrame);

            s_localBaselineMode = (uint8_t)mode;
            s_localBaselineSubstate = (uint8_t)sub;
            s_localBaselineSimFrame = (int32_t)simFrame;

            // Re-assert the authoritative match seed immediately before
            // baseline capture.  Both peers must have identical CRT rand
            // state for the captured savestate CRCs to match.
            DetVer_SetRngSeed(s_config.session_seed);
            LOG_NETPLAY(LOG_INFO, "[MatchBoot] RNG seed asserted: 0x%08X before baseline capture",
                s_config.session_seed);

            // Capture bootstrap baseline into the dedicated netplay slot.
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
                        "[MatchBoot] Baseline captured: main_crc=0x%08X digest=0x%08X frame=%u rng=0x%08X",
                        s_localBaselineCRC,
                        s_localBaselineDigest,
                        info->frame,
                        info->rng_seed);
                } else {
                    SetError("Failed to capture baseline savestate");
                }
            }
        }
    }

    // Check baseline agreement when both have exchanged
    if (s_localBaselineReady &&
        s_remoteBaselineReady &&
        s_baselineDigestSent &&
        s_localBaselineDigest != 0 &&
        s_remoteBaselineDigest != 0 &&
        !s_baselineAgreed) {
        if (s_localBaselineDigest == s_remoteBaselineDigest) {
            s_baselineAgreed = true;
            s_phase = BootPhase::Ready;
            LOG_NETPLAY(LOG_INFO,
                "[MatchBoot] Baseline agreed: digest=0x%08X main_local=0x%08X main_remote=0x%08X -> Ready phase",
                s_localBaselineDigest,
                s_localBaselineCRC,
                s_remoteBaselineCRC);
        } else {
            Rollback::NetplayLog_Write(
                "BASELINE", s_localBaselineSimFrame,
                "Baseline mismatch: local_digest=0x%08X remote_digest=0x%08X "
                "local_main=0x%08X remote_main=0x%08X local=%u/%u/%d remote=%u/%u/%d phase=%s",
                s_localBaselineDigest,
                s_remoteBaselineDigest,
                s_localBaselineCRC,
                s_remoteBaselineCRC,
                s_localBaselineMode,
                s_localBaselineSubstate,
                s_localBaselineSimFrame,
                s_remoteBaselineMode,
                s_remoteBaselineSubstate,
                s_remoteBaselineSimFrame,
                BootPhaseName(s_phase));
            BaselineSync_LogMismatchAndDump(BuildBaselineStateView());
            SetError("Baseline digest mismatch: local=0x%08X remote=0x%08X (main local=0x%08X remote=0x%08X)",
                s_localBaselineDigest,
                s_remoteBaselineDigest,
                s_localBaselineCRC,
                s_remoteBaselineCRC);
        }
    }

    // Timeout check
    if (s_baselineStartTime && (GetTickCount() - s_baselineStartTime) >= BASELINE_TIMEOUT_MS) {
        SetError("Baseline agreement timed out (15s)");
    }
}

static void UpdateReady() {
    // Host sends GameplayStart when baseline is agreed
    if (s_isHost && !s_gameplayStartSent) {
        s_bootstrapFrameAbs = 0;
        if (SendGameplayStart(s_bootstrapFrameAbs)) {
            s_gameplayStart = true;
            s_phase = BootPhase::Done;
        }
    }
    // Join: GameplayStart was already received (stored by OnGameplayStart)
    if (!s_isHost && s_gameplayStart) {
        s_phase = BootPhase::Done;
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void MatchBootstrap_Init() {
    if (s_initialized) return;
    s_phase = BootPhase::Idle;
    memset(&s_config, 0, sizeof(s_config));
    s_error[0] = '\0';
    BaselineSync_Reset();
    s_initialized = true;
    LOG_NETPLAY(LOG_DEBUG, "[MatchBoot] Initialized");
}

void MatchBootstrap_Shutdown() {
    if (!s_initialized) return;
    s_phase = BootPhase::Idle;
    BaselineSync_Reset();
    s_initialized = false;
    LOG_NETPLAY(LOG_DEBUG, "[MatchBoot] Shutdown");
}

void MatchBootstrap_BeginConfigExchange(const LockedMatchConfig* config) {
    if (!config) return;

    BaselineSync_Reset();
    s_isHost = (Session_GetRole() == SessionRole::Host);

    // Host always uses its own config.
    // Join: only overwrite if we haven't already received the host's config
    // via an early ConfigExchange packet (race condition where the host
    // sends config before the client enters this phase).
    if (s_isHost || !s_configReceived) {
        memcpy(&s_config, config, sizeof(LockedMatchConfig));
    }
    if (s_isHost || s_configReceived) {
        GameSettingsSync_ApplyLockedConfig(&s_config,
            s_isHost ? "host begin config exchange" : "join begin config exchange with early host config");
    }

    s_configSent = false;
    // NOTE: Do NOT reset s_configReceived or s_configAgreed here.
    // The host's ConfigExchange packet may have arrived while we were
    // still in FrontendLocked. Resetting would discard that early packet
    // and cause config exchange to time out.
    s_error[0] = '\0';

    s_phase = BootPhase::ConfigExchange;
    LOG_NETPLAY(LOG_INFO,
        "[MatchBoot] Begin config exchange (role=%s config_already=%s agreed_already=%s rounds_raw=%u rounds_to_win=%d)",
        s_isHost ? "Host" : "Join",
        s_configReceived ? "yes" : "no",
        s_configAgreed ? "yes" : "no",
        s_config.round_count,
        GameSettingsSync_RoundsToWin(s_config.round_count));
}

void MatchBootstrap_BeginLoading() {
    s_localLoaded = false;
    s_localLoadMode = 0;
    s_localLoadSubstate = 0;
    s_localLoadSimFrame = -1;
    // NOTE: Do NOT reset s_remoteLoaded here.
    // The remote peer's LoadBarrier packet may have arrived
    // while we were still in ConfigExchange/ConfigAgreed.
    // Resetting it would discard that early notification and
    // cause the loading barrier to time out.
    s_loadBarrierSent = false;

    s_loadStartTime = GetTickCount();
    s_phase = BootPhase::Loading;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Begin loading barrier (remote_already=%s)",
        s_remoteLoaded ? "yes" : "no");
    Rollback::NetplayLog_Write(
        "BARRIER", -1,
        "Begin loading barrier: phase=%s remote_already=%d remote_mode=%u remote_sub=%u remote_sim=%d",
        BootPhaseName(s_phase),
        s_remoteLoaded ? 1 : 0,
        s_remoteLoadMode,
        s_remoteLoadSubstate,
        s_remoteLoadSimFrame);
}

void MatchBootstrap_BeginBaseline() {
    Savestate_ClearRollbackBaseline("begin baseline capture");
    s_localBaselineReady = false;
    s_localBaselineMode = 0;
    s_localBaselineSubstate = 0;
    s_localBaselineSimFrame = -1;
    // NOTE: Do NOT reset s_remoteBaselineReady or remote digest/main here.
    // The remote peer's BaselineReady/Digest packets may have arrived
    // while we were still in the Loading phase. Resetting them would
    // discard that early notification and cause baseline agreement to
    // time out.
    s_baselineAgreed = false;
    s_localBaselineCRC = 0;
    s_localBaselineDigest = 0;
    s_baselineDigestSent = false;
    s_gameplayStart = false;
    s_gameplayStartSent = false;
    s_bootstrapFrameAbs = 0;
    s_gameplayStartHostGameAbsFrame = -1;

    s_baselineStartTime = GetTickCount();
    s_phase = BootPhase::Baseline;
    LOG_NETPLAY(LOG_INFO,
        "[MatchBoot] Begin baseline capture (remote_baseline_already=%s digest=0x%08X main=0x%08X)",
        s_remoteBaselineReady ? "yes" : "no",
        s_remoteBaselineDigest,
        s_remoteBaselineCRC);
    BaselineSync_LogBegin(BuildBaselineStateView());
}

void MatchBootstrap_Abort() {
    Savestate_ClearRollbackBaseline("bootstrap abort");
    s_phase = BootPhase::Idle;
    s_error[0] = '\0';
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
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Aborted (all state reset)");
}

void MatchBootstrap_FrameUpdate() {
    if (!s_initialized) return;

    switch (s_phase) {
        case BootPhase::ConfigExchange: UpdateConfigExchange(); break;
        case BootPhase::Loading:        UpdateLoading();        break;
        case BootPhase::Baseline:       UpdateBaseline();       break;
        case BootPhase::Ready:          UpdateReady();          break;
        default: break;
    }
}

// ============================================================================
// Packet handlers
// ============================================================================

void MatchBootstrap_OnConfigExchange(const ConfigExchangePayload* p) {
    if (!p) return;

    // Build a LockedMatchConfig from the payload
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
        // Host received config from join (shouldn't happen normally)
        LOG_NETPLAY(LOG_WARNING, "[MatchBoot] Host received ConfigExchange from join");
    } else {
        // Join: adopt host's config
        // NOTE: Do NOT gate on s_phase == ConfigExchange. The host may
        // send ConfigExchange before the client enters that phase (the
        // client may still be in FrontendLocked building its own config).
        // Store the config unconditionally; the ack is sent once the
        // client actually enters ConfigExchange and sees s_configReceived.
        memcpy(&s_config, &received, sizeof(LockedMatchConfig));
        GameSettingsSync_ApplyLockedConfig(&s_config, "join received host config");
        LOG_NETPLAY(LOG_INFO,
            "[MatchBoot] Join received config (hash=0x%08X phase=%u rounds_raw=%u rounds_to_win=%d)",
            receivedHash,
            (unsigned)s_phase,
            s_config.round_count,
            GameSettingsSync_RoundsToWin(s_config.round_count));

        // Store host's delay negotiation data
        s_remoteDelayData.local_input_delay = p->my_input_delay;
        s_remoteDelayData.max_rollback = p->my_max_rollback;
        s_remoteDelayReceived = true;
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Join received host config: remote_delay=%d remote_max_rb=%d",
            p->my_input_delay, p->my_max_rollback);

        // Only agree+ack if we are already in ConfigExchange phase.
        // If we haven't entered yet, UpdateConfigExchange will handle it.
        if (s_phase == BootPhase::ConfigExchange) {
            s_configAgreed = true;
            SendConfigAck(receivedHash, true);
            DelayPolicy_NegotiateSession(&s_remoteDelayData);
            LOG_NETPLAY(LOG_INFO, "[MatchBoot] Join accepted config (hash=0x%08X remote_delay=%d stall_threshold=%d)",
                receivedHash,
                DelayPolicy_GetRemoteAnnouncedDelay(),
                DelayPolicy_GetStallThreshold());
        }
    }
}

void MatchBootstrap_OnConfigAck(const ConfigAckPayload* p) {
    if (!p) return;

    uint32_t localHash = LockedMatchConfig_Hash(&s_config);

    if (p->accepted && p->config_hash == localHash) {
        // Store join's delay negotiation data
        s_remoteDelayData.local_input_delay = p->my_input_delay;
        s_remoteDelayData.max_rollback = p->my_max_rollback;
        s_remoteDelayReceived = true;
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Host received join config: remote_delay=%d remote_max_rb=%d",
            p->my_input_delay, p->my_max_rollback);

        s_configAgreed = true;
        DelayPolicy_NegotiateSession(&s_remoteDelayData);
        LOG_NETPLAY(LOG_INFO,
            "[MatchBoot] Config agreed (hash=0x%08X phase=%u rounds_raw=%u rounds_to_win=%d local_delay=%d local_max_rb=%d remote_delay=%d remote_max_rb=%d stall_threshold=%d)",
            localHash,
            (unsigned)s_phase,
            s_config.round_count,
            GameSettingsSync_RoundsToWin(s_config.round_count),
            DelayPolicy_GetConfiguredDelay(),
            DelayPolicy_GetRollbackBudget(),
            DelayPolicy_GetRemoteAnnouncedDelay(),
            DelayPolicy_GetRemoteAnnouncedMaxRollback(),
            DelayPolicy_GetStallThreshold());
    } else {
        SetError("Config rejected by peer (local=0x%08X remote=0x%08X)",
            localHash, p->config_hash);
    }
}

void MatchBootstrap_OnLoadBarrier(const LoadBarrierPayload* p) {
    if (!p) return;

    if (p->loaded) {
        s_remoteLoaded = true;
        s_remoteLoadMode = p->mode;
        s_remoteLoadSubstate = p->substate;
        s_remoteLoadSimFrame = (int32_t)p->sim_frame;
        LOG_NETPLAY(LOG_INFO,
            "[MatchBoot] Remote loading complete: mode=%u sub=%u simFrame=%d",
            s_remoteLoadMode, s_remoteLoadSubstate, s_remoteLoadSimFrame);
        if (s_phase != BootPhase::Loading) {
            Rollback::NetplayLog_Write(
                "BARRIER", s_remoteLoadSimFrame,
                "Remote LoadBarrier arrived out-of-phase: phase=%s local_loaded=%d remote_loaded=%d remote=%u/%u/%d",
                BootPhaseName(s_phase),
                s_localLoaded ? 1 : 0,
                s_remoteLoaded ? 1 : 0,
                s_remoteLoadMode,
                s_remoteLoadSubstate,
                s_remoteLoadSimFrame);
        }
    }
}

void MatchBootstrap_OnBaselineReady(const BaselineReadyPayload* p) {
    if (!p) return;

    if (p->captured) {
        s_remoteBaselineReady = true;
        s_remoteBaselineMode = p->mode;
        s_remoteBaselineSubstate = p->substate;
        s_remoteBaselineSimFrame = (int32_t)p->sim_frame;
        LOG_NETPLAY(LOG_INFO,
            "[MatchBoot] Remote baseline ready: mode=%u sub=%u simFrame=%d",
            s_remoteBaselineMode, s_remoteBaselineSubstate, s_remoteBaselineSimFrame);
        if (s_phase != BootPhase::Baseline) {
            Rollback::NetplayLog_Write(
                "BASELINE", s_remoteBaselineSimFrame,
                "Remote BaselineReady arrived out-of-phase: phase=%s local_ready=%d remote_ready=%d remote=%u/%u/%d",
                BootPhaseName(s_phase),
                s_localBaselineReady ? 1 : 0,
                s_remoteBaselineReady ? 1 : 0,
                s_remoteBaselineMode,
                s_remoteBaselineSubstate,
                s_remoteBaselineSimFrame);
        }

        // Implicit LoadBarrierReady: if we're still in Loading phase and
        // receive BaselineReady, the remote peer is obviously past loading.
        // Treat this as implicit load completion to avoid a deadlock where
        // the explicit LoadBarrier packet was processed before we entered
        // the Loading phase and was therefore discarded.
        if (s_phase == BootPhase::Loading && !s_remoteLoaded) {
            s_remoteLoaded = true;
            LOG_NETPLAY(LOG_INFO, "[MatchBoot] Implicit LoadBarrierReady from BaselineReady");
        }
    }
}

void MatchBootstrap_OnBaselineDigest(const BaselineDigestPayload* p) {
    if (!p) return;

    s_remoteBaselineDigest = p->crc32;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Remote baseline digest: digest=0x%08X", p->crc32);
    BaselineSync_LogRemoteDigest(BuildBaselineStateView(), p->crc32);
}

void MatchBootstrap_OnBaselineBreakdown(const BaselineBreakdownPayload* p) {
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

void MatchBootstrap_OnGameplayStart(const GameplayStartPayload* p) {
    if (!p) return;

    s_bootstrapFrameAbs = p->bootstrap_frame_abs;
    s_gameplayStart = true;
    // NOTE: Do NOT set s_phase = BootPhase::Done here.
    // On the Join side, this packet can arrive in the same Session_Update()
    // poll as BaselineReady/BaselineDigest — before UpdateBaseline() has run
    // the agreement check.  Setting Done would cause FrameUpdate() to skip
    // UpdateBaseline(), so s_baselineAgreed never becomes true and
    // PregameSync stays stuck at BootstrapBaseline forever.
    // The phase transitions naturally: Baseline → Ready (via agreement) →
    // Done (host: UpdateReady sends GameplayStart; join: PregameSync reads
    // gameplay_start from the snapshot).
    s_gameplayStartHostGameAbsFrame = (int32_t)p->host_game_abs_frame;
    LOG_NETPLAY(LOG_INFO,
        "[MatchBoot] GameplayStart received: bootstrap_frame_abs=%u host_game_abs_frame=%d local_game_abs_frame=%u",
        p->bootstrap_frame_abs,
        s_gameplayStartHostGameAbsFrame,
        ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
}

// ============================================================================
// Queries
// ============================================================================

void MatchBootstrap_GetSnapshot(MatchBootstrapSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->active = (s_phase != BootPhase::Idle && s_phase != BootPhase::Done);
    out->config_sent = s_configSent;
    out->config_received = s_configReceived;
    out->config_agreed = s_configAgreed;
    out->local_loaded = s_localLoaded;
    out->remote_loaded = s_remoteLoaded;
    out->both_loaded = s_localLoaded && s_remoteLoaded;
    out->local_load_sim_frame = s_localLoadSimFrame;
    out->remote_load_sim_frame = s_remoteLoadSimFrame;
    out->local_baseline_ready = s_localBaselineReady;
    out->remote_baseline_ready = s_remoteBaselineReady;
    out->baseline_agreed = s_baselineAgreed;
    out->local_baseline_crc = s_localBaselineCRC;
    out->remote_baseline_crc = s_remoteBaselineCRC;
    out->local_baseline_sim_frame = s_localBaselineSimFrame;
    out->remote_baseline_sim_frame = s_remoteBaselineSimFrame;
    out->gameplay_start = s_gameplayStart;
    out->bootstrap_frame_abs = s_bootstrapFrameAbs;
    out->gameplay_start_host_game_abs_frame = s_gameplayStartHostGameAbsFrame;
    strncpy_s(out->error, sizeof(out->error), s_error, _TRUNCATE);
}

const LockedMatchConfig* MatchBootstrap_GetAgreedConfig() {
    if (!s_configAgreed) return nullptr;
    return &s_config;
}

} // namespace Net
