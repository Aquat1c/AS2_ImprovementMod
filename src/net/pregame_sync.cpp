/**
 * Alice Senki 2 - Pre-Game Synchronization Layer
 *
 * State machine driving the pre-game sync path:
 *   FrontendCharSel → FrontendStageSel → FrontendLocked →
 *   ConfigExchange → ConfigAgreed →
 *   BootstrapLoading → BootstrapBaseline → BootstrapReady → GameplayHandoff
 */

#include "net/pregame_sync.h"
#include "net/charsel_sync.h"
#include "net/match_bootstrap.h"
#include "net/match_lifecycle.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/mode_ownership.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "rollback/netplay_log.h"
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
// Internal state
// ============================================================================

static bool            s_initialized       = false;
static PregamePhase    s_phase             = PregamePhase::Idle;
static LockedMatchConfig s_lockedConfig    = {};
static bool            s_configAgreed      = false;
static uint32_t        s_configHash        = 0;
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
static uint8_t         s_localCapabilities   = 0x01; // Bit 0: savestate baseline
static uint8_t         s_remoteCapabilities  = 0;
static bool            s_syncAnnounceSent    = false;
static bool            s_remoteSyncAnnounced = false;
static bool            s_syncConfirmSent     = false;
static bool            s_remoteSyncConfirmed = false;

// Phase timeout tracking
static DWORD           s_phaseStartTime      = 0;
constexpr DWORD        SYNC_TIMEOUT_MS       = 10000;  // 10s for sync phases
constexpr DWORD        CONFIG_TIMEOUT_MS     = 10000;  // 10s for config exchange
constexpr DWORD        LOAD_TIMEOUT_MS       = 30000;  // 30s for load barrier
constexpr DWORD        BASELINE_TIMEOUT_MS   = 15000;  // 15s for baseline

// ============================================================================
// Helpers
// ============================================================================

static void SetPhase(PregamePhase next, const char* why) {
    if (s_phase == next) return;
    const char* oldName = PregamePhaseName(s_phase);
    const char* newName = PregamePhaseName(next);
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Phase %s -> %s (%s)",
        oldName, newName, why ? why : "?");
    Rollback::NetplayLog_StateChange("PREGAME", -1,
        "PregamePhase", oldName, newName, why ? why : "?");
    s_phase = next;
    s_phaseStartTime = GetTickCount();
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

// ============================================================================
// Session sync packet handlers (called before OnPregamePacket dispatch)
// ============================================================================

static void HandleSyncAnnounce(const SyncAnnouncePayload* p) {
    if (!p || s_remoteSyncAnnounced) return;

    s_remoteSessionId = p->session_id;
    s_remoteCapabilities = p->capability_flags;
    s_remoteSyncAnnounced = true;

    LOG_NETPLAY(LOG_INFO, "[PregameSync] Remote SyncAnnounce: session=0x%08X caps=0x%02X",
        p->session_id, p->capability_flags);
}

static void HandleSyncConfirm(const SyncConfirmPayload* p) {
    if (!p || s_remoteSyncConfirmed) return;

    if (!p->confirmed) {
        SetErrorFmt("Remote peer rejected session sync");
        SetPhase(PregamePhase::Error, "sync rejected by remote");
        return;
    }

    s_remoteSyncConfirmed = true;

    // Join adopts host's session ID and side assignment
    SessionRole role = Session_GetRole();
    if (role == SessionRole::Join) {
        s_sessionId = p->session_id;
        s_assignedSide = p->assigned_side;
    }

    LOG_NETPLAY(LOG_INFO, "[PregameSync] Remote SyncConfirm: session=0x%08X side=%u confirmed=%u",
        p->session_id, p->assigned_side, p->confirmed);
}

// ============================================================================
// Packet handler (registered with Session_SetPacketCallback)
// ============================================================================

static void OnPregamePacket(PacketType type, const void* payload, size_t payloadLen) {
    switch (type) {
        case PacketType::SyncAnnounce:
            if (payloadLen >= sizeof(SyncAnnouncePayload)) {
                HandleSyncAnnounce(static_cast<const SyncAnnouncePayload*>(payload));
            }
            break;

        case PacketType::SyncConfirm:
            if (payloadLen >= sizeof(SyncConfirmPayload)) {
                HandleSyncConfirm(static_cast<const SyncConfirmPayload*>(payload));
            }
            break;

        case PacketType::CharSelInput:
            if (payloadLen >= sizeof(CharSelInputPayload)) {
                CharSelSync_OnRemoteInput(static_cast<const CharSelInputPayload*>(payload));
            }
            break;

        case PacketType::CharSelLock:
            if (payloadLen >= sizeof(CharSelLockPayload)) {
                CharSelSync_OnRemoteLock(static_cast<const CharSelLockPayload*>(payload));
                s_remoteCharSelLocked = true;
            }
            break;

        case PacketType::StageSync:
            if (payloadLen >= sizeof(StageSyncPayload)) {
                CharSelSync_OnRemoteStage(static_cast<const StageSyncPayload*>(payload));
                auto* sp = static_cast<const StageSyncPayload*>(payload);
                if (sp->confirmed) s_remoteStageLocked = true;
            }
            break;

        case PacketType::ConfigExchange:
            if (payloadLen >= sizeof(ConfigExchangePayload)) {
                MatchBootstrap_OnConfigExchange(static_cast<const ConfigExchangePayload*>(payload));
            }
            break;

        case PacketType::ConfigAck:
            if (payloadLen >= sizeof(ConfigAckPayload)) {
                MatchBootstrap_OnConfigAck(static_cast<const ConfigAckPayload*>(payload));
            }
            break;

        case PacketType::LoadBarrier:
            if (payloadLen >= sizeof(LoadBarrierPayload)) {
                MatchBootstrap_OnLoadBarrier(static_cast<const LoadBarrierPayload*>(payload));
            }
            break;

        case PacketType::BaselineReady:
            if (payloadLen >= sizeof(BaselineReadyPayload)) {
                MatchBootstrap_OnBaselineReady(static_cast<const BaselineReadyPayload*>(payload));
            }
            break;

        case PacketType::BaselineDigest:
            if (payloadLen >= sizeof(BaselineDigestPayload)) {
                MatchBootstrap_OnBaselineDigest(static_cast<const BaselineDigestPayload*>(payload));
            }
            break;

        case PacketType::GameplayStart:
            if (payloadLen >= sizeof(GameplayStartPayload)) {
                MatchBootstrap_OnGameplayStart(static_cast<const GameplayStartPayload*>(payload));
            }
            break;

        default:
            LOG_NETPLAY(LOG_WARNING, "[PregameSync] Unhandled packet type %u", (unsigned)type);
            break;
    }
}

// ============================================================================
// Phase update: Initial Session Sync
// ============================================================================

static void SendSyncAnnounce() {
    SyncAnnouncePayload payload{};
    payload.session_id = s_sessionId;
    payload.capability_flags = s_localCapabilities;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::SyncAnnounce,
                       &payload, sizeof(payload), true);
    s_syncAnnounceSent = true;
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Sent SyncAnnounce: session=0x%08X caps=0x%02X",
        s_sessionId, s_localCapabilities);
}

static void SendSyncConfirm() {
    SyncConfirmPayload payload{};
    payload.session_id = s_sessionId;
    payload.confirmed = 1;
    payload.assigned_side = s_assignedSide;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::SyncConfirm,
                       &payload, sizeof(payload), true);
    s_syncConfirmSent = true;
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Sent SyncConfirm: session=0x%08X side=%u",
        s_sessionId, s_assignedSide);
}

static void UpdateSyncAnnounce() {
    // Send our announce if not yet sent
    if (!s_syncAnnounceSent) {
        SendSyncAnnounce();
    }

    // Wait for remote announce
    if (s_remoteSyncAnnounced) {
        SetStatusFmt("Session announced. Confirming...");
        SetPhase(PregamePhase::SyncExchange, "remote announced");
    }

    if (PhaseTimedOut(SYNC_TIMEOUT_MS)) {
        SetErrorFmt("Session sync timed out waiting for announce");
        SetPhase(PregamePhase::Error, "sync announce timeout");
    }
}

static void UpdateSyncExchange() {
    // Both have announced — send our confirm
    if (!s_syncConfirmSent) {
        // Host decides side assignment
        SessionRole role = Session_GetRole();
        if (role == SessionRole::Host) {
            s_assignedSide = 0;  // Host = P1 by default
        }

        SendSyncConfirm();
    }

    // Wait for remote confirm
    if (s_remoteSyncConfirmed) {
        SetStatusFmt("Session sync confirmed.");
        SetPhase(PregamePhase::SyncConfirmed, "both confirmed");
    }

    if (PhaseTimedOut(SYNC_TIMEOUT_MS)) {
        SetErrorFmt("Session sync timed out waiting for confirm");
        SetPhase(PregamePhase::Error, "sync confirm timeout");
    }
}

static void UpdateSyncConfirmed() {
    // Transition immediately to CharSel lockstep
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Session sync complete: session=0x%08X side=%u",
        s_sessionId, s_assignedSide);

    CharSelSync_Begin();
    SetStatusFmt("Character select...");
    SetPhase(PregamePhase::FrontendCharSel, "sync confirmed -> charsel");
}

// ============================================================================
// Phase update: Front-End Sync
// ============================================================================

static void UpdateFrontendCharSel() {
    CharSelSync_FrameUpdate();

    CharSelSyncSnapshot csSnap{};
    CharSelSync_GetSnapshot(&csSnap);

    s_localCharSelLocked = csSnap.local_confirmed;
    s_remoteCharSelLocked = csSnap.remote_confirmed;

    if (csSnap.both_characters_locked) {
        SetStatusFmt("Characters locked. Stage select...");
        SetPhase(PregamePhase::FrontendStageSel, "both characters confirmed");
        CharSelSync_BeginStagePhase();
    }
}

static void UpdateFrontendStageSel() {
    CharSelSync_FrameUpdate();

    CharSelSyncSnapshot csSnap{};
    CharSelSync_GetSnapshot(&csSnap);

    s_localStageLocked = csSnap.local_stage_confirmed;
    s_remoteStageLocked = csSnap.remote_stage_confirmed;

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

    // Use side assignment from session sync
    s_lockedConfig.host_side = s_assignedSide;

    // Host determines seeds
    SessionRole role = Session_GetRole();
    if (role == SessionRole::Host) {
        s_lockedConfig.rng_seed = GetTickCount() ^ 0xDEADBEEF;
        s_lockedConfig.session_seed = s_sessionId;
        s_lockedConfig.round_count = 2;
        s_lockedConfig.time_limit = 0;
    }

    SetPhase(PregamePhase::ConfigExchange, "config ready");
    MatchBootstrap_BeginConfigExchange(&s_lockedConfig);
}

static void UpdateConfigExchange() {
    MatchBootstrap_FrameUpdate();

    MatchBootstrapSnapshot bSnap{};
    MatchBootstrap_GetSnapshot(&bSnap);

    if (bSnap.config_agreed) {
        // Get the final agreed config from bootstrap
        const LockedMatchConfig* agreed = MatchBootstrap_GetAgreedConfig();
        if (agreed) {
            memcpy(&s_lockedConfig, agreed, sizeof(LockedMatchConfig));
        }
        s_configHash = LockedMatchConfig_Hash(&s_lockedConfig);
        s_configAgreed = true;
        SetStatusFmt("Config agreed (hash=0x%08X). Loading...", s_configHash);
        SetPhase(PregamePhase::ConfigAgreed, "config agreed");
    }

    if (bSnap.error[0]) {
        SetErrorFmt("Config error: %s", bSnap.error);
        SetPhase(PregamePhase::Error, "config exchange failed");
    }

    if (PhaseTimedOut(CONFIG_TIMEOUT_MS)) {
        SetErrorFmt("Config exchange timed out");
        SetPhase(PregamePhase::Error, "config timeout");
    }
}

static void UpdateConfigAgreed() {
    // Transition immediately to loading phase
    SetStatusFmt("Waiting for assets to load...");
    SetPhase(PregamePhase::BootstrapLoading, "begin loading");
    MatchBootstrap_BeginLoading();
}

static void UpdateBootstrapLoading() {
    MatchBootstrap_FrameUpdate();

    MatchBootstrapSnapshot bSnap{};
    MatchBootstrap_GetSnapshot(&bSnap);

    if (bSnap.both_loaded) {
        SetStatusFmt("Assets loaded. Capturing baseline...");
        SetPhase(PregamePhase::BootstrapBaseline, "both loaded");
        MatchBootstrap_BeginBaseline();
    }

    if (bSnap.error[0]) {
        SetErrorFmt("Load error: %s", bSnap.error);
        SetPhase(PregamePhase::Error, "load barrier failed");
    }

    if (PhaseTimedOut(LOAD_TIMEOUT_MS)) {
        SetErrorFmt("Load barrier timed out (30s)");
        SetPhase(PregamePhase::Error, "load timeout");
    }
}

static void UpdateBootstrapBaseline() {
    MatchBootstrap_FrameUpdate();

    MatchBootstrapSnapshot bSnap{};
    MatchBootstrap_GetSnapshot(&bSnap);

    if (bSnap.baseline_agreed) {
        SetStatusFmt("Baseline agreed. Ready to play!");
        SetPhase(PregamePhase::BootstrapReady, "baseline agreed");
    }

    if (bSnap.error[0]) {
        SetErrorFmt("Baseline error: %s", bSnap.error);
        SetPhase(PregamePhase::Error, "baseline agreement failed");
    }

    if (PhaseTimedOut(BASELINE_TIMEOUT_MS)) {
        SetErrorFmt("Baseline agreement timed out (15s)");
        SetPhase(PregamePhase::Error, "baseline timeout");
    }
}

static void UpdateBootstrapReady() {
    MatchBootstrap_FrameUpdate();

    MatchBootstrapSnapshot bSnap{};
    MatchBootstrap_GetSnapshot(&bSnap);

    if (bSnap.gameplay_start) {
        SetStatusFmt("Gameplay starting!");
        SetPhase(PregamePhase::GameplayHandoff, "gameplay start");

        // Notify match lifecycle layer — it now owns the match flow
        MatchLifecycle_OnMatchEnter();
    }

    if (bSnap.error[0]) {
        SetErrorFmt("Ready error: %s", bSnap.error);
        SetPhase(PregamePhase::Error, "ready phase failed");
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void PregameSync_Init() {
    if (s_initialized) return;
    s_phase = PregamePhase::Idle;
    s_configAgreed = false;
    s_configHash = 0;
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
    s_phaseStartTime = 0;

    CharSelSync_Init();
    MatchBootstrap_Init();

    s_initialized = true;
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Initialized");
}

void PregameSync_Shutdown() {
    if (!s_initialized) return;

    MatchBootstrap_Shutdown();
    CharSelSync_Shutdown();

    s_phase = PregamePhase::Idle;
    s_initialized = false;
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Shutdown");
}

void PregameSync_FrameUpdate() {
    if (!s_initialized) return;
    if (s_phase == PregamePhase::Idle || s_phase == PregamePhase::GameplayHandoff) return;

    // Check session validity
    if (!IsSessionValid()) {
        if (s_phase != PregamePhase::Error) {
            PregameSync_Abort("Session lost during pre-game sync");
        }
        return;
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

bool PregameSync_Begin() {
    if (!s_initialized) return false;
    if (s_phase != PregamePhase::Idle) {
        LOG_NETPLAY(LOG_WARNING, "[PregameSync] Begin called but phase is %s",
            PregamePhaseName(s_phase));
        return false;
    }

    if (!IsSessionValid()) {
        LOG_NETPLAY(LOG_WARNING, "[PregameSync] Begin called but session not valid");
        return false;
    }

    // Register our packet handler
    Session_SetPacketCallback(OnPregamePacket);

    // Reset all tracking state
    s_configAgreed = false;
    s_configHash = 0;
    LockedMatchConfig_Clear(&s_lockedConfig);
    s_errorText[0] = '\0';
    s_localCharSelLocked = false;
    s_remoteCharSelLocked = false;
    s_localStageLocked = false;
    s_remoteStageLocked = false;

    // Initialize session sync state
    s_sessionId = GetTickCount() ^ (uint32_t)(uintptr_t)&s_phase;  // Unique per session
    s_remoteSessionId = 0;
    s_assignedSide = 0;
    s_remoteCapabilities = 0;
    s_syncAnnounceSent = false;
    s_remoteSyncAnnounced = false;
    s_syncConfirmSent = false;
    s_remoteSyncConfirmed = false;

    // Start with initial session sync (announce → exchange → confirmed → charsel)
    SetStatusFmt("Synchronizing session...");
    SetPhase(PregamePhase::SyncAnnounce, "begin");

    LOG_NETPLAY(LOG_INFO, "[PregameSync] Pre-game sync started (session=0x%08X)", s_sessionId);
    return true;
}

void PregameSync_Abort(const char* reason) {
    if (s_phase == PregamePhase::Idle) return;

    LOG_NETPLAY(LOG_WARNING, "[PregameSync] Abort: %s (was %s)",
        reason ? reason : "?", PregamePhaseName(s_phase));

    CharSelSync_Abort();
    MatchBootstrap_Abort();

    SetErrorFmt("%s", reason ? reason : "Pre-game sync aborted");
    SetPhase(PregamePhase::Idle, "aborted");

    s_configAgreed = false;
    s_configHash = 0;
    s_localCharSelLocked = false;
    s_remoteCharSelLocked = false;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_syncAnnounceSent = false;
    s_remoteSyncAnnounced = false;
    s_syncConfirmSent = false;
    s_remoteSyncConfirmed = false;
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
    MatchBootstrapSnapshot bSnap{};
    MatchBootstrap_GetSnapshot(&bSnap);
    out->local_loaded = bSnap.local_loaded;
    out->remote_loaded = bSnap.remote_loaded;
    out->local_baseline_ready = bSnap.local_baseline_ready;
    out->remote_baseline_ready = bSnap.remote_baseline_ready;

    // Config
    out->config_agreed = s_configAgreed;
    out->config_hash = s_configHash;

    // Status
    strncpy_s(out->status_text, sizeof(out->status_text), s_statusText, _TRUNCATE);
    strncpy_s(out->error_text, sizeof(out->error_text), s_errorText, _TRUNCATE);
}

const LockedMatchConfig* PregameSync_GetLockedConfig() {
    if (!s_configAgreed) return nullptr;
    return &s_lockedConfig;
}

bool PregameSync_IsComplete() {
    return s_phase == PregamePhase::GameplayHandoff;
}

} // namespace Net
