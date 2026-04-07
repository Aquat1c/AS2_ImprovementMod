/**
 * Alice Senki 2 - Match Bootstrap
 *
 * Post-selection bootstrap: config exchange, load barrier, baseline
 * capture, baseline agreement, and gameplay start signal.
 */

#include "net/match_bootstrap.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/locked_match_config.h"
#include "rollback/savestate.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
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

// Baseline
static bool            s_localBaselineReady = false;
static bool            s_remoteBaselineReady= false;
static bool            s_baselineAgreed     = false;
static uint32_t        s_localBaselineCRC   = 0;
static uint32_t        s_remoteBaselineCRC  = 0;
static bool            s_baselineDigestSent = false;

// Gameplay start
static bool            s_gameplayStart      = false;
static uint32_t        s_startFrame         = 0;
static bool            s_gameplayStartSent  = false;

// Timeout tracking
static DWORD           s_loadStartTime      = 0;
static DWORD           s_baselineStartTime  = 0;
constexpr DWORD        LOAD_BARRIER_TIMEOUT_MS = 30000;  // 30s
constexpr DWORD        BASELINE_TIMEOUT_MS    = 15000;  // 15s

// Error
static char            s_error[128]         = "";

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

    Session_SendPacket(CHANNEL_CONTROL, PacketType::ConfigExchange,
                       &payload, sizeof(payload), true);

    s_configSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent config (hash=0x%08X)",
        LockedMatchConfig_Hash(&s_config));
}

static void SendConfigAck(uint32_t hash, bool accepted) {
    ConfigAckPayload payload{};
    payload.config_hash = hash;
    payload.accepted = accepted ? 1 : 0;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::ConfigAck,
                       &payload, sizeof(payload), true);

    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent ConfigAck: hash=0x%08X accepted=%d",
        hash, accepted ? 1 : 0);
}

static void SendLoadBarrier() {
    LoadBarrierPayload payload{};
    payload.loaded = 1;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::LoadBarrier,
                       &payload, sizeof(payload), true);

    s_loadBarrierSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent LoadBarrier");
}

static void SendBaselineReady() {
    BaselineReadyPayload payload{};
    payload.captured = 1;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::BaselineReady,
                       &payload, sizeof(payload), true);

    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent BaselineReady");
}

static void SendBaselineDigest(uint32_t crc) {
    BaselineDigestPayload payload{};
    payload.crc32 = crc;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::BaselineDigest,
                       &payload, sizeof(payload), true);

    s_baselineDigestSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent BaselineDigest: crc=0x%08X", crc);
}

static void SendGameplayStart(uint32_t frame) {
    GameplayStartPayload payload{};
    payload.start_frame = frame;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::GameplayStart,
                       &payload, sizeof(payload), true);

    s_gameplayStartSent = true;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Sent GameplayStart: frame=%u", frame);
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
    // Otherwise wait for packet callbacks
}

static void UpdateLoading() {
    // Monitor game state to detect when assets are loaded
    // In charsel flow: substate progresses through LOADING(9) to PREMATCH(10)
    uint32_t mode = GetGameMode();
    uint32_t sub = GetSubstate();

    // Consider loaded when we reach MODE_MATCH with substate >= MATCH_SETUP(1)
    // or when charsel leaves loading substate
    bool justLoaded = false;

    if (mode == MODE_MATCH && sub >= 1) {
        justLoaded = true;
    } else if (mode == MODE_CHARSEL && sub >= CHARSEL_SUB_FADE_BACK) {
        justLoaded = true;
    }

    if (justLoaded && !s_localLoaded) {
        s_localLoaded = true;
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Local loading complete (mode=%u sub=%u)", mode, sub);
        SendLoadBarrier();
    }

    // Timeout check
    if (s_loadStartTime && (GetTickCount() - s_loadStartTime) >= LOAD_BARRIER_TIMEOUT_MS) {
        SetError("Load barrier timed out (30s)");
    }
}

static void UpdateBaseline() {
    // Capture baseline savestate at pre-frame-0
    if (!s_localBaselineReady) {
        uint32_t mode = GetGameMode();
        uint32_t sub = GetSubstate();

        // Capture baseline when match reaches INIT(2) or GAMEPLAY(3) substate
        if (mode == MODE_MATCH && sub >= 2) {
            // Capture savestate
            if (Savestate_Save()) {
                const SavestateInfo* info = Savestate_GetInfo();
                if (info && info->valid) {
                    s_localBaselineCRC = info->checksum;
                    s_localBaselineReady = true;
                    SendBaselineReady();
                    SendBaselineDigest(s_localBaselineCRC);
                    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Baseline captured: crc=0x%08X frame=%u",
                        s_localBaselineCRC, info->frame);
                } else {
                    SetError("Failed to capture baseline savestate");
                }
            }
        }
    }

    // Check baseline agreement when both have exchanged
    if (s_localBaselineReady && s_remoteBaselineReady && s_baselineDigestSent && !s_baselineAgreed) {
        if (s_localBaselineCRC == s_remoteBaselineCRC) {
            s_baselineAgreed = true;
            LOG_NETPLAY(LOG_INFO, "[MatchBoot] Baseline agreed: crc=0x%08X", s_localBaselineCRC);
        } else {
            SetError("Baseline CRC mismatch: local=0x%08X remote=0x%08X",
                s_localBaselineCRC, s_remoteBaselineCRC);
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
        s_startFrame = 0;
        SendGameplayStart(s_startFrame);
        s_gameplayStart = true;
        s_phase = BootPhase::Done;
    }
    // Join waits for GameplayStart from host
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
    s_initialized = true;
    LOG_NETPLAY(LOG_DEBUG, "[MatchBoot] Initialized");
}

void MatchBootstrap_Shutdown() {
    if (!s_initialized) return;
    s_phase = BootPhase::Idle;
    s_initialized = false;
    LOG_NETPLAY(LOG_DEBUG, "[MatchBoot] Shutdown");
}

void MatchBootstrap_BeginConfigExchange(const LockedMatchConfig* config) {
    if (!config) return;

    memcpy(&s_config, config, sizeof(LockedMatchConfig));
    s_isHost = (Session_GetRole() == SessionRole::Host);

    s_configSent = false;
    s_configReceived = false;
    s_configAgreed = false;
    s_error[0] = '\0';

    s_phase = BootPhase::ConfigExchange;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Begin config exchange (role=%s)",
        s_isHost ? "Host" : "Join");
}

void MatchBootstrap_BeginLoading() {
    s_localLoaded = false;
    s_remoteLoaded = false;
    s_loadBarrierSent = false;

    s_loadStartTime = GetTickCount();
    s_phase = BootPhase::Loading;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Begin loading barrier");
}

void MatchBootstrap_BeginBaseline() {
    s_localBaselineReady = false;
    s_remoteBaselineReady = false;
    s_baselineAgreed = false;
    s_localBaselineCRC = 0;
    s_remoteBaselineCRC = 0;
    s_baselineDigestSent = false;
    s_gameplayStart = false;
    s_gameplayStartSent = false;

    s_baselineStartTime = GetTickCount();
    s_phase = BootPhase::Baseline;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Begin baseline capture");
}

void MatchBootstrap_Abort() {
    s_phase = BootPhase::Idle;
    s_error[0] = '\0';
    s_configAgreed = false;
    s_gameplayStart = false;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Aborted");
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
    if (!p || s_phase != BootPhase::ConfigExchange) return;

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
        // Join: adopt host's config, send ack
        memcpy(&s_config, &received, sizeof(LockedMatchConfig));
        s_configAgreed = true;
        SendConfigAck(receivedHash, true);
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Join accepted config (hash=0x%08X)", receivedHash);
    }
}

void MatchBootstrap_OnConfigAck(const ConfigAckPayload* p) {
    if (!p || s_phase != BootPhase::ConfigExchange) return;

    uint32_t localHash = LockedMatchConfig_Hash(&s_config);

    if (p->accepted && p->config_hash == localHash) {
        s_configAgreed = true;
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Config agreed (hash=0x%08X)", localHash);
    } else {
        SetError("Config rejected by peer (local=0x%08X remote=0x%08X)",
            localHash, p->config_hash);
    }
}

void MatchBootstrap_OnLoadBarrier(const LoadBarrierPayload* p) {
    if (!p) return;

    if (p->loaded) {
        s_remoteLoaded = true;
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Remote loading complete");
    }
}

void MatchBootstrap_OnBaselineReady(const BaselineReadyPayload* p) {
    if (!p) return;

    if (p->captured) {
        s_remoteBaselineReady = true;
        LOG_NETPLAY(LOG_INFO, "[MatchBoot] Remote baseline ready");
    }
}

void MatchBootstrap_OnBaselineDigest(const BaselineDigestPayload* p) {
    if (!p) return;

    s_remoteBaselineCRC = p->crc32;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] Remote baseline digest: crc=0x%08X", p->crc32);
}

void MatchBootstrap_OnGameplayStart(const GameplayStartPayload* p) {
    if (!p) return;

    s_startFrame = p->start_frame;
    s_gameplayStart = true;
    s_phase = BootPhase::Done;
    LOG_NETPLAY(LOG_INFO, "[MatchBoot] GameplayStart received: frame=%u", p->start_frame);
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
    out->local_baseline_ready = s_localBaselineReady;
    out->remote_baseline_ready = s_remoteBaselineReady;
    out->baseline_agreed = s_baselineAgreed;
    out->local_baseline_crc = s_localBaselineCRC;
    out->remote_baseline_crc = s_remoteBaselineCRC;
    out->gameplay_start = s_gameplayStart;
    out->start_frame = s_startFrame;
    strncpy_s(out->error, sizeof(out->error), s_error, _TRUNCATE);
}

const LockedMatchConfig* MatchBootstrap_GetAgreedConfig() {
    if (!s_configAgreed) return nullptr;
    return &s_config;
}

} // namespace Net
