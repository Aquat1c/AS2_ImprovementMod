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
#include "net/pause_handler.h"
#include "net/winscreen_sync.h"
#include "net/netplay_palette_runtime.h"
#include "net/barrier_protocol.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "net/mode_ownership.h"
#include "net/netplay_menu_controller.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "patches/input_sync_hooks.h"
#include "patches/memory_utils.h"
#include "rollback/online_wiring.h"
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

// Periodic logging counter
static uint32_t        s_logTickCounter      = 0;

// ============================================================================
// Helpers
// ============================================================================

static void SetPhase(PregamePhase next, const char* why) {
    if (s_phase == next) return;
    const char* oldName = PregamePhaseName(s_phase);
    const char* newName = PregamePhaseName(next);

    uint32_t mode = GetGameMode();
    uint32_t sub  = GetSubstate();
    uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    LOG_NETPLAY(LOG_INFO, "[PregameSync] Phase %s -> %s (%s) | mode=%u sub=%u simFrame=%u",
        oldName, newName, why ? why : "?", mode, sub, simFrame);
    Rollback::NetplayLog_StateChange("PREGAME", (int32_t)simFrame,
        "PregamePhase", oldName, newName, why ? why : "?");
    s_phase = next;
    s_phaseStartTime = GetTickCount();

    // Error phase → immediately trigger full disconnect.
    // This ensures the load barrier freeze is cleared, the session is
    // canceled, and the game returns to the menu with an error message.
    // Without this the Error phase was a dead-end: FrameUpdate() stopped
    // driving the state machine but nothing disconnected, so the game
    // fell through to local-only gameplay.
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

static void UpdateBootstrapFreezeForBoundary() {
    const bool inBootstrap =
        s_phase == PregamePhase::BootstrapLoading ||
        s_phase == PregamePhase::BootstrapBaseline ||
        s_phase == PregamePhase::BootstrapReady;

    if (!inBootstrap) {
        InputSyncHooks_SetLoadBarrierFreeze(false);
        return;
    }

    const uint32_t mode = GetGameMode();
    const uint32_t sub = GetSubstate();

    // Match the old working behavior: let Mode 8 loading/setup/init progress
    // naturally, and only freeze once the game reaches the gameplay boundary
    // before bootstrap has finished.
    const bool shouldFreeze = (mode == MODE_MATCH && sub == MATCH_SUB_GAMEPLAY);
    InputSyncHooks_SetLoadBarrierFreeze(shouldFreeze);
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

    // Receiving a SyncConfirm implies the remote already announced
    // (they went through Announce→Exchange→Confirm). If we missed their
    // SyncAnnounce due to callback registration timing, this recovers.
    if (!s_remoteSyncAnnounced) {
        s_remoteSyncAnnounced = true;
        s_remoteSessionId = p->session_id;
        LOG_NETPLAY(LOG_INFO, "[PregameSync] Inferred remote announce from SyncConfirm");
    }

    // Join adopts host's session ID and side assignment
    SessionRole role = Session_GetRole();
    if (role == SessionRole::Join) {
        s_sessionId = p->session_id;
        s_assignedSide = p->assigned_side;
    }

    LOG_NETPLAY(LOG_INFO, "[PregameSync] Remote SyncConfirm: session=0x%08X side=%u confirmed=%u",
        p->session_id, p->assigned_side, p->confirmed);
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

// ============================================================================
// Packet handler (registered with Session_SetPacketCallback)
// ============================================================================

static void OnPregamePacket(PacketType type, const void* payload, size_t payloadLen) {
    Rollback::NetplayLog_Write("PREGAME", -1,
        "OnPregamePacket: type=%s payload=%zu phase=%s session=0x%08X remoteSession=0x%08X announced=%d confirmed=%d",
        PacketTypeName(type),
        payloadLen,
        PregamePhaseName(s_phase),
        s_sessionId,
        s_remoteSessionId,
        s_remoteSyncAnnounced ? 1 : 0,
        s_remoteSyncConfirmed ? 1 : 0);
    Rollback::NetplayLog_Flush();

    switch (type) {
        case PacketType::SyncAnnounce:
            if (payloadLen >= sizeof(SyncAnnouncePayload)) {
                HandleSyncAnnounce(static_cast<const SyncAnnouncePayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short SyncAnnounce", type, payloadLen, sizeof(SyncAnnouncePayload));
            }
            break;

        case PacketType::SyncConfirm:
            if (payloadLen >= sizeof(SyncConfirmPayload)) {
                HandleSyncConfirm(static_cast<const SyncConfirmPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short SyncConfirm", type, payloadLen, sizeof(SyncConfirmPayload));
            }
            break;

        case PacketType::CharSelInput:
            // Legacy state-driven charsel sync — no longer used.
            // Input-driven lockstep uses CharSelFrameInput instead.
            break;

        case PacketType::CharSelFrameInput:
            if (payloadLen >= sizeof(CharSelFrameInputPayload)) {
                CharSelSync_OnRemoteFrameInput(static_cast<const CharSelFrameInputPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short CharSelFrameInput", type, payloadLen, sizeof(CharSelFrameInputPayload));
            }
            break;

        case PacketType::WinScreenFrameInput:
            if (payloadLen >= sizeof(WinScreenFrameInputPayload)) {
                WinScreenSync_OnRemoteFrameInput(static_cast<const WinScreenFrameInputPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short WinScreenFrameInput", type, payloadLen, sizeof(WinScreenFrameInputPayload));
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
                MatchBootstrap_OnConfigExchange(static_cast<const ConfigExchangePayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short ConfigExchange", type, payloadLen, sizeof(ConfigExchangePayload));
            }
            break;

        case PacketType::ConfigAck:
            if (payloadLen >= sizeof(ConfigAckPayload)) {
                MatchBootstrap_OnConfigAck(static_cast<const ConfigAckPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short ConfigAck", type, payloadLen, sizeof(ConfigAckPayload));
            }
            break;

        case PacketType::LoadBarrier:
            if (payloadLen >= sizeof(LoadBarrierPayload)) {
                MatchBootstrap_OnLoadBarrier(static_cast<const LoadBarrierPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short LoadBarrier", type, payloadLen, sizeof(LoadBarrierPayload));
            }
            break;

        case PacketType::BaselineReady:
            if (payloadLen >= sizeof(BaselineReadyPayload)) {
                MatchBootstrap_OnBaselineReady(static_cast<const BaselineReadyPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short BaselineReady", type, payloadLen, sizeof(BaselineReadyPayload));
            }
            break;

        case PacketType::BaselineDigest:
            if (payloadLen >= sizeof(BaselineDigestPayload)) {
                MatchBootstrap_OnBaselineDigest(static_cast<const BaselineDigestPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short BaselineDigest", type, payloadLen, sizeof(BaselineDigestPayload));
            }
            break;

        case PacketType::BaselineBreakdown:
            if (payloadLen >= sizeof(BaselineBreakdownPayload)) {
                MatchBootstrap_OnBaselineBreakdown(static_cast<const BaselineBreakdownPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short BaselineBreakdown", type, payloadLen, sizeof(BaselineBreakdownPayload));
            }
            break;

        case PacketType::GameplayStart:
            if (payloadLen >= sizeof(GameplayStartPayload)) {
                MatchBootstrap_OnGameplayStart(static_cast<const GameplayStartPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short GameplayStart", type, payloadLen, sizeof(GameplayStartPayload));
            }
            break;

        case PacketType::PaletteConfig:
            if (payloadLen >= sizeof(PaletteConfigPayload)) {
                NetplayPaletteRuntime_OnRemoteConfig(static_cast<const PaletteConfigPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short PaletteConfig", type, payloadLen, sizeof(PaletteConfigPayload));
            }
            break;

        case PacketType::PaletteAck:
            if (payloadLen >= sizeof(PaletteAckPayload)) {
                NetplayPaletteRuntime_OnRemoteAck(static_cast<const PaletteAckPayload*>(payload));
            } else {
                LogPregamePacketAnomaly("Short PaletteAck", type, payloadLen, sizeof(PaletteAckPayload));
            }
            break;

        default:
            // Handle cross-phase packets that can arrive at any time
            if (type == PacketType::PauseQuit) {
                PauseHandler_OnRemotePauseQuit();
                break;
            }
            if (type == PacketType::WinScreenConfirm) {
                WinScreenSync_OnRemoteConfirm();
                break;
            }
            // Gameplay/debug packets that can arrive during handoff transition
            // — silently ignore rather than spam warnings
            if (type == PacketType::GameplayInput ||
                type == PacketType::FrameSyncStatus ||
                type == PacketType::StateDigest ||
                type == PacketType::Ping ||
                type == PacketType::Pong ||
                // GekkoData / GekkoReady can race in on the same tick that
                // TryStartRollbackSession() switches the callback to OnGameplayPacket.
                // If OnPregamePacket still handles the packet, ignore silently;
                // OnlineWiring's startup barrier resend path will re-assert READY.
                type == PacketType::GekkoData ||
                type == PacketType::GekkoReady) {
                Rollback::NetplayLog_Verbose(
                    "PREGAME", -1,
                    "Ignoring cross-phase packet during pregame: type=%s payload=%zu phase=%s",
                    PacketTypeName(type),
                    payloadLen,
                    PregamePhaseName(s_phase));
                break;
            }
            LOG_NETPLAY(LOG_WARNING, "[PregameSync] Unhandled packet type %u", (unsigned)type);
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
// Phase update: Initial Session Sync
// ============================================================================

static void SendSyncAnnounce() {
    SyncAnnouncePayload payload{};
    payload.session_id = s_sessionId;
    payload.capability_flags = s_localCapabilities;

    BarrierProtocol_SendPacket(PacketType::SyncAnnounce,
                              &payload, sizeof(payload));
    s_syncAnnounceSent = true;
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Sent SyncAnnounce: session=0x%08X caps=0x%02X",
        s_sessionId, s_localCapabilities);
}

static void SendSyncConfirm() {
    SyncConfirmPayload payload{};
    payload.session_id = s_sessionId;
    payload.confirmed = 1;
    payload.assigned_side = s_assignedSide;

    BarrierProtocol_SendPacket(PacketType::SyncConfirm,
                              &payload, sizeof(payload));
    s_syncConfirmSent = true;
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Sent SyncConfirm: session=0x%08X side=%u",
        s_sessionId, s_assignedSide);
}

static DWORD s_lastAnnounceSendTime = 0;
constexpr DWORD ANNOUNCE_RESEND_MS  = 500;  // Resend every 500ms

static void UpdateSyncAnnounce() {
    // Send our announce if not yet sent
    if (!s_syncAnnounceSent) {
        SendSyncAnnounce();
        s_lastAnnounceSendTime = GetTickCount();
    }

    // Periodically resend announce in case the first was dropped
    // (remote may not have registered packet callback yet)
    if (s_syncAnnounceSent && !s_remoteSyncAnnounced) {
        DWORD now = GetTickCount();
        if (now - s_lastAnnounceSendTime >= ANNOUNCE_RESEND_MS) {
            LOG_NETPLAY(LOG_DEBUG, "[PregameSync] Resending SyncAnnounce (no remote announce yet)");
            SendSyncAnnounce();
            s_lastAnnounceSendTime = now;
        }
    }

    // If remote already confirmed (implies they announced + exchanged),
    // skip directly to SyncExchange.
    if (s_remoteSyncConfirmed) {
        SetStatusFmt("Session confirmed (fast path). Exchanging...");
        SetPhase(PregamePhase::SyncExchange, "remote already confirmed");
        return;
    }

    // Normal path: wait for remote announce
    if (s_remoteSyncAnnounced) {
        SetStatusFmt("Session announced. Confirming...");
        SetPhase(PregamePhase::SyncExchange, "remote announced");
        return;
    }

    if (PhaseTimedOut(SYNC_TIMEOUT_MS)) {
        SetErrorFmt("Session sync timed out waiting for announce");
        SetPhase(PregamePhase::Error, "sync announce timeout");
    }
}

static DWORD s_lastConfirmSendTime = 0;
constexpr DWORD CONFIRM_RESEND_MS  = 500;

static void UpdateSyncExchange() {
    // Both have announced — send our confirm
    if (!s_syncConfirmSent) {
        // Host decides side assignment
        SessionRole role = Session_GetRole();
        if (role == SessionRole::Host) {
            s_assignedSide = 0;  // Host = P1 by default
        }

        SendSyncConfirm();
        s_lastConfirmSendTime = GetTickCount();
    }

    // Periodically resend confirm in case it was missed
    if (s_syncConfirmSent && !s_remoteSyncConfirmed) {
        DWORD now = GetTickCount();
        if (now - s_lastConfirmSendTime >= CONFIRM_RESEND_MS) {
            LOG_NETPLAY(LOG_DEBUG, "[PregameSync] Resending SyncConfirm (no remote confirm yet)");
            SendSyncConfirm();
            s_lastConfirmSendTime = now;
        }
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
    SessionRole role = Session_GetRole();
    LOG_NETPLAY(LOG_INFO, "[PregameSync] Session sync complete: session=0x%08X side=%u role=%s",
        s_sessionId, s_assignedSide,
        role == SessionRole::Host ? "Host" : "Join");

    CharSelSync_Begin();
    SetStatusFmt("Character select...");
    SetPhase(PregamePhase::FrontendCharSel, "sync confirmed -> charsel");
}

// ============================================================================
// Phase update: Front-End Sync
// ============================================================================

static uint32_t s_charselLogCounter = 0;

static void UpdateFrontendCharSel() {
    CharSelSync_FrameUpdate();

    CharSelSyncSnapshot csSnap{};
    CharSelSync_GetSnapshot(&csSnap);

    s_localCharSelLocked = csSnap.local_confirmed;
    s_remoteCharSelLocked = csSnap.remote_confirmed;

    // Periodic charsel state logging (every 120 frames ~2s)
    s_charselLogCounter++;
    if (s_charselLogCounter == 1 || (s_charselLogCounter % 120) == 0) {
        LOG_NETPLAY(LOG_DEBUG,
            "[PregameSync] CharSel tick=%u lockstep_frame=%u local_input=%u delay=%d "
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

    // Front-end selections are fully resolved at this point.
    // End CharSel lockstep before the game transitions into Mode 7 / Mode 8,
    // otherwise Hook_InputDispatcher will keep treating later states as
    // frontend-owned and stall the bootstrap handoff.
    CharSelSync_Abort();

    NetplayPaletteRuntime_OnLockedMatchConfig(&s_lockedConfig);

    SetPhase(PregamePhase::ConfigExchange, "config ready");
    MatchBootstrap_BeginConfigExchange(&s_lockedConfig);
}

static void UpdateConfigExchange() {
    MatchBootstrap_FrameUpdate();

    MatchBootstrapSnapshot bSnap{};
    MatchBootstrap_GetSnapshot(&bSnap);

    // Periodic state context every ~2s (120 frames)
    if ((s_logTickCounter++ % 120) == 0) {
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        LOG_NETPLAY(LOG_INFO,
            "[PregameSync] ConfigExchange: sent=%s recv=%s agreed=%s elapsed=%lums",
            bSnap.config_sent ? "yes" : "no",
            bSnap.config_received ? "yes" : "no",
            bSnap.config_agreed ? "yes" : "no",
            elapsed);
    }

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
    // Transition immediately to loading phase.
    // Do not freeze here: Mode 7 / Mode 8 substates 0-2 must continue running
    // so both peers can actually finish loading and reach the baseline gate.
    SetStatusFmt("Waiting for assets to load...");
    SetPhase(PregamePhase::BootstrapLoading, "begin loading");
    MatchBootstrap_BeginLoading();
}

static void UpdateBootstrapLoading() {
    MatchBootstrap_FrameUpdate();

    MatchBootstrapSnapshot bSnap{};
    MatchBootstrap_GetSnapshot(&bSnap);

    UpdateBootstrapFreezeForBoundary();

    // Periodic state context every ~2s (120 frames)
    if ((s_logTickCounter++ % 120) == 0) {
        uint32_t mode = GetGameMode();
        uint32_t sub  = GetSubstate();
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        bool frozen = (mode == MODE_MATCH && sub == MATCH_SUB_GAMEPLAY);
        LOG_NETPLAY(LOG_INFO,
            "[PregameSync] BootstrapLoading: mode=%u sub=%u frozen=%s "
            "local=%s remote=%s localFrame=%d remoteFrame=%d elapsed=%lums",
            mode, sub, frozen ? "yes" : "no",
            bSnap.local_loaded ? "yes" : "no",
            bSnap.remote_loaded ? "yes" : "no",
            bSnap.local_load_sim_frame,
            bSnap.remote_load_sim_frame,
            elapsed);
    }

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

    UpdateBootstrapFreezeForBoundary();

    // Periodic state context every ~2s (120 frames)
    if ((s_logTickCounter % 120) == 0) {
        uint32_t mode = GetGameMode();
        uint32_t sub  = GetSubstate();
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        bool frozen = (mode == MODE_MATCH && sub == MATCH_SUB_GAMEPLAY);
        LOG_NETPLAY(LOG_INFO,
            "[PregameSync] BootstrapBaseline: mode=%u sub=%u frozen=%s "
            "localReady=%s remoteReady=%s localCRC=0x%08X remoteCRC=0x%08X "
            "localFrame=%d remoteFrame=%d digestSent=%s elapsed=%lums",
            mode, sub, frozen ? "yes" : "no",
            bSnap.local_baseline_ready ? "yes" : "no",
            bSnap.remote_baseline_ready ? "yes" : "no",
            bSnap.local_baseline_crc, bSnap.remote_baseline_crc,
            bSnap.local_baseline_sim_frame,
            bSnap.remote_baseline_sim_frame,
            bSnap.baseline_agreed ? "yes" : "no",
            elapsed);
    }

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

    UpdateBootstrapFreezeForBoundary();

    // Periodic state context every ~2s (120 frames)
    if ((s_logTickCounter % 120) == 0) {
        DWORD elapsed = s_phaseStartTime ? (GetTickCount() - s_phaseStartTime) : 0;
        LOG_NETPLAY(LOG_INFO,
            "[PregameSync] BootstrapReady: gameplayStart=%s bootstrap_frame_abs=%u host_game_abs_frame=%d elapsed=%lums",
            bSnap.gameplay_start ? "yes" : "no",
            bSnap.bootstrap_frame_abs,
            bSnap.gameplay_start_host_game_abs_frame,
            elapsed);
    }

    if (bSnap.gameplay_start) {
        SetStatusFmt("Gameplay starting!");
        SetPhase(PregamePhase::GameplayHandoff, "gameplay start");

        // Disable load barrier freeze — the rollback session will take over
        InputSyncHooks_SetLoadBarrierFreeze(false);

        // Notify match lifecycle layer — it now owns the match flow
        MatchLifecycle_OnMatchEnter();

        // Arm online wiring startup handoff now:
        // - restore agreed baseline/state before intro
        // - keep intro deterministic/passive
        // - defer rollback-owned BeginFrame/Advance until mutual first
        //   post-intro interactive release
        Rollback::OnlineWiring_OnGameplayStart();
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
        // Rematch flow reaches CharSel with PregameSync still in terminal
        // GameplayHandoff from the previous match. Allow an explicit restart.
        if (s_phase == PregamePhase::GameplayHandoff ||
            s_phase == PregamePhase::Error) {
            LOG_NETPLAY(LOG_INFO,
                "[PregameSync] Begin requested from terminal phase %s — resetting for restart",
                PregamePhaseName(s_phase));

            // Clear any stale phase-owned state from the prior match.
            InputSyncHooks_SetLoadBarrierFreeze(false);
            CharSelSync_Abort();
            MatchBootstrap_Abort();
            SetPhase(PregamePhase::Idle, "restart begin");
        } else {
            LOG_NETPLAY(LOG_WARNING, "[PregameSync] Begin called but phase is %s",
                PregamePhaseName(s_phase));
            return false;
        }
    }

    if (!IsSessionValid()) {
        LOG_NETPLAY(LOG_WARNING, "[PregameSync] Begin called but session not valid");
        return false;
    }

    // Register our packet handler
    Rollback::NetplayLog_Write("PREGAME", -1,
        "Registering pregame packet callback");
    Rollback::NetplayLog_Flush();
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
    s_lastAnnounceSendTime = 0;
    s_lastConfirmSendTime = 0;
    NetplayPaletteRuntime_OnDisconnect("pregame begin reset");

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

    // Always clear load barrier freeze on abort — ensure game isn't stuck
    InputSyncHooks_SetLoadBarrierFreeze(false);

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
    s_lastAnnounceSendTime = 0;
    s_lastConfirmSendTime = 0;
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
