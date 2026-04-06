#include "netplay_hooks.h"

#include "as2_rollback.h"
#include "netplay_menu_controller.h"
#include "session_manager.h"
#include "rollback_session.h"
#include "input_sync_hooks.h"
#include "stress_test.h"
#include "match_runner.h"
#include "charsel_sync.h"
#include "match_bootstrap.h"
#include "desync_diagnostics.h"
#include "spectator_manager.h"
#include "adaptive_delay.h"
#include "rng_hooks.h"
#include "audio_hooks.h"
#include "render_hooks.h"
#include "packet_codec.h"
#include "log_window.h"

#include <stdint.h>

namespace NetplayHooks {

// ---------------------------------------------------------------------------
// Build signature (strict per-build marker)
// ---------------------------------------------------------------------------

static uint32_t Fnv1a32_Runtime(const char* s) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        h ^= (uint32_t)(*p);
        h *= 16777619u;
    }
    return h;
}

uint32_t GetBuildSignature() {
    static uint32_t sig = Fnv1a32_Runtime("AS2_Mod|" __DATE__ "|" __TIME__);
    return sig;
}

// ---------------------------------------------------------------------------
// Settings / state
// ---------------------------------------------------------------------------

static bool s_auto_hooks_enabled = true;
static int s_delay_frames = 0;

void SetAutoHooksEnabled(bool enabled) {
    s_auto_hooks_enabled = enabled;
}

void SetNetplayFrameDelay(int delay) {
    if (delay < 0) delay = 0;
    if (delay > 6) delay = 6;
    s_delay_frames = delay;
}

int GetNetplayFrameDelay() {
    return s_delay_frames;
}

// ---------------------------------------------------------------------------
// Session / frame tracking
// ---------------------------------------------------------------------------

static uint32_t s_last_mode = 0;
static uint32_t s_last_substate = 0;
static uint32_t s_last_game_type = 0;
static bool     s_rollback_started = false;  // true while rollback session is active
static uint32_t s_last_detailed_log_ms = 0;
static SessionManager::State s_last_session_state = SessionManager::State::Idle;

// Load barrier: waiting for MatchBootstrap to complete before starting rollback
static bool     s_waiting_for_load_barrier = false;
static bool     s_load_barrier_signaled    = false;  // true once NotifyLocalLoaded called
static uint32_t s_load_barrier_start_ms    = 0;
static constexpr uint32_t kLoadBarrierTimeoutMs = 30000;  // 30 seconds

// ---------------------------------------------------------------------------
// Rollback lifecycle helpers
// ---------------------------------------------------------------------------

namespace {

static bool TryGetModSession(SessionManager::Snapshot* out) {
    SessionManager::Snapshot snap{};
    if (!SessionManager::GetSnapshot(&snap)) {
        return false;
    }
    if (out) {
        *out = snap;
    }
    return true;
}

static bool IsConnectedSessionState(SessionManager::State state) {
    return state == SessionManager::State::Connected ||
           state == SessionManager::State::CharSel ||
           state == SessionManager::State::Gameplay;
}

} // namespace

uint8_t GetConnectionRole() {
    SessionManager::Snapshot snap{};
    if (TryGetModSession(&snap) && (snap.active || snap.has_error)) {
        return SessionManager::IsHost() ? 1 : 0;
    }
    if (NetplayMenuController::IsMenuActive()) {
        return 0xFF;
    }
    return 0xFF;
}

bool IsConnected() {
    SessionManager::Snapshot snap{};
    return TryGetModSession(&snap) && snap.active && IsConnectedSessionState(snap.state);
}

bool HasModNetplayMarker() {
    SessionManager::Snapshot snap{};
    if (TryGetModSession(&snap) && (snap.active || snap.has_error)) {
        return true;
    }
    if (NetplayMenuController::IsMenuActive()) {
        return true;
    }
    if (CharSelSync::IsActive()) {
        return true;
    }
    if (MatchBootstrap::IsActive()) {
        return true;
    }
    if (InputSyncHooks::IsLoadBarrierFrozen()) {
        return true;
    }
    if (InputSyncHooks::IsRollbackActive()) {
        return true;
    }
    if (RollbackSession::IsActive()) {
        return true;
    }
    return false;
}

bool IsInNetworkMode() {
    uint32_t mode = GetGameMode();
    if (mode != MODE_MENU && mode != MODE_CHARSEL && mode != MODE_MATCH) {
        return false;
    }
    return HasModNetplayMarker();
}

bool IsInNetplayLobby() {
    return IsInLobbyMode() && HasModNetplayMarker();
}

bool IsInNetplayCharSel() {
    return IsInCharSelMode() && HasModNetplayMarker();
}

bool IsInCharSelInputSync() {
    if (!IsInNetplayCharSel()) return false;
    uint32_t sub = GetSubstate();
    return sub == CHARSEL_SUB_SELECT || sub == CHARSEL_SUB_CONFIRM;
}

bool IsInNetplayMatch() {
    return IsInMatchMode() && HasModNetplayMarker();
}

bool IsInNetplayGameplay() {
    return IsInNetplayMatch() && GetSubstate() == MATCH_SUB_GAMEPLAY;
}

static bool CreateRollbackSession() {
    if (RollbackSession::IsActive()) return true;

    // Transition SessionManager to Gameplay state
    SessionManager::EnterGameplay();

    // Initialize gameplay-only diagnostics for this match.
    DesyncDiag::Init();

    // Configure rollback session
    RollbackSession::Config cfg = {};
    cfg.is_host           = SessionManager::IsHost();
    cfg.input_delay       = MatchBootstrap::GetNegotiatedDelay();
    cfg.max_rollback      = 8;
    cfg.desync_detection  = true;
    s_delay_frames        = cfg.input_delay;

    if (!RollbackSession::Create(&cfg)) {
        LOG_NETPLAY(LOG_ERROR, "[NetplayHooks] Failed to create RollbackSession");
        return false;
    }

    // Install audio/render suppression hooks if not already installed
    if (!AudioHooks::IsInstalled()) AudioHooks::Install();
    if (!RenderHooks::IsInstalled()) RenderHooks::Install();
    if (!RngHooks::IsInstalled()) RngHooks::Install();

    // Reset vanilla frame counters — these accumulate during Mode 8 loading
    // substates (0-2) and must not pollute the rollback session's frame 0.
    *reinterpret_cast<uint32_t*>(ADDR_FRAME_SIMULATION) = 0;
    *reinterpret_cast<uint32_t*>(ADDR_FRAME_DISPLAY)    = 0;
    *reinterpret_cast<uint32_t*>(ADDR_FRAME_WRITE_IDX)  = 0;
    *reinterpret_cast<uint32_t*>(ADDR_FRAME_NET_IDX)    = 0;
    *reinterpret_cast<uint32_t*>(ADDR_REMOTE_FRAME)     = 0;
    AS2_LogFrameCounterState(0, "RollbackSessionStart");

    LOG_NETPLAY(LOG_INFO, "[NetplayHooks] Rollback session CREATED (host=%s delay=%d) — waiting for Gekko sync",
                cfg.is_host ? "yes" : "no", cfg.input_delay);
    return true;
}

static void ActivateRollbackSession() {
    if (s_rollback_started || !RollbackSession::IsActive()) return;

    // Activate rollback-driven frame stepping in input hooks
    InputSyncHooks::SetRollbackActive(true);
    InputSyncHooks::ResetForNewMatch();
    InputSyncHooks::ResetStats();

    // Initialize adaptive delay with default config
    AdaptiveDelay::Init(nullptr, s_delay_frames);
    AdaptiveDelay::SetNetworkContext(
        SessionManager::GetSessionId(),
        SessionManager::GetConnectionId());

    s_rollback_started = true;
    LOG_NETPLAY(LOG_INFO, "[NetplayHooks] Rollback session STARTED (host=%s delay=%d)",
                SessionManager::IsHost() ? "yes" : "no", s_delay_frames);
}

static void StopRollbackSession() {
    if (!s_rollback_started && !RollbackSession::IsActive()) return;

    // Deactivate rollback in input hooks (restore vanilla passthrough)
    if (s_rollback_started) {
        InputSyncHooks::SetRollbackActive(false);
    }

    // Shutdown adaptive delay
    AdaptiveDelay::Shutdown();

    // Clear load barrier state if still pending
    if (s_waiting_for_load_barrier) {
        s_waiting_for_load_barrier = false;
        s_load_barrier_signaled = false;
        InputSyncHooks::SetLoadBarrierFreeze(false);
    }

    // Ensure audio/render suppression is disabled
    AudioHooks::SetSuppressed(false);
    RenderHooks::SetSuppressed(false);

    // Destroy the GekkoNet session
    if (RollbackSession::IsActive()) {
        RollbackSession::Destroy();
    }

    // End CharSel sync if still active
    if (CharSelSync::IsActive()) {
        CharSelSync::End();
    }
    
    // Abort bootstrap if still active  
    if (MatchBootstrap::IsActive()) {
        MatchBootstrap::Abort("Rollback session stopped");
    }

    // Return SessionManager to Connected state
    SessionManager::ReturnToSession();

    s_rollback_started = false;
    LOG_NETPLAY(LOG_INFO, "[NetplayHooks] Rollback session STOPPED");
}

static const char* GameModeToString(uint32_t mode) {
    switch (mode) {
        case MODE_BOOT:     return "Boot";
        case MODE_TITLE:    return "Title";
        case MODE_MENU:     return "Menu";
        case MODE_LOBBY:    return "Lobby";
        case MODE_VS_SELECT:return "VSSelect";
        case MODE_CHARSEL:  return "CharSel";
        case MODE_STAGESEL: return "StageSel";
        case MODE_MATCH:    return "Match";
        case MODE_STORY:    return "Story";
        case MODE_END:      return "End";
        case MODE_GALLERY:  return "Gallery";
        case MODE_OPTIONS:  return "Options";
        case MODE_PALETTE:  return "Palette";
        default:            return "Unknown";
    }
}

static const char* GameTypeToString(uint32_t type) {
    switch (type) {
        case GAMETYPE_ARCADE:   return "Arcade";
        case GAMETYPE_VS_CPU:   return "VsCPU";
        case GAMETYPE_VS_HUMAN: return "VsHuman";
        case GAMETYPE_NETPLAY:  return "Netplay";
        case GAMETYPE_WATCH:    return "Watch";
        case GAMETYPE_REPLAY:   return "Replay";
        case GAMETYPE_DEMO:     return "Demo";
        default:                return "Unknown";
    }
}

static const char* RollbackStateToString(RollbackSession::State state) {
    switch (state) {
        case RollbackSession::State::Inactive: return "Inactive";
        case RollbackSession::State::Syncing:  return "Syncing";
        case RollbackSession::State::Running:  return "Running";
        case RollbackSession::State::Error:    return "Error";
        default:                               return "Unknown";
    }
}

static const char* MatchSubstateToString(uint32_t substate) {
    switch (substate) {
        case MATCH_SUB_LOAD_ASSETS: return "LoadAssets";
        case MATCH_SUB_SETUP:       return "Setup";
        case MATCH_SUB_INIT:        return "IntroInit";
        case MATCH_SUB_GAMEPLAY:    return "Gameplay";
        case MATCH_SUB_PAUSE:       return "Pause";
        case MATCH_SUB_END:         return "End";
        default:                    return "Unknown";
    }
}

static const char* CharSelSubstateToString(uint32_t substate) {
    switch (substate) {
        case CHARSEL_SUB_INIT:        return "Init";
        case CHARSEL_SUB_FADEIN:      return "FadeIn";
        case CHARSEL_SUB_SELECT:      return "Select";
        case CHARSEL_SUB_CANCEL:      return "Cancel";
        case CHARSEL_SUB_CONFIRM:     return "Confirm";
        case CHARSEL_SUB_TRANS5:      return "Trans5";
        case CHARSEL_SUB_TRANS6:      return "Trans6";
        case CHARSEL_SUB_PREVIEW:     return "Preview";
        case CHARSEL_SUB_STAGE_INTRO: return "StageIntro";
        case CHARSEL_SUB_LOADING:     return "Loading";
        case CHARSEL_SUB_PREMATCH:    return "PreMatch";
        case CHARSEL_SUB_STAGE:       return "Stage";
        case CHARSEL_SUB_BACK_MENU:   return "BackMenu";
        case CHARSEL_SUB_BACK_LOBBY:  return "BackLobby";
        case CHARSEL_SUB_TO_MATCH:    return "ToMatch";
        default:                      return "Unknown";
    }
}

static const char* SubstateToString(uint32_t mode, uint32_t substate) {
    if (mode == MODE_MATCH) return MatchSubstateToString(substate);
    if (mode == MODE_CHARSEL) return CharSelSubstateToString(substate);
    return "n/a";
}

static void LogDetailedGameHeartbeat(bool force, const char* reason) {
    SessionManager::Snapshot session = {};
    const bool haveSessionSnapshot = SessionManager::GetSnapshot(&session);
    const bool shouldLog =
        force ||
        (haveSessionSnapshot && (session.active || session.has_error)) ||
        s_waiting_for_load_barrier ||
        CharSelSync::IsActive() ||
        MatchBootstrap::IsActive() ||
        RollbackSession::IsActive();

    if (!shouldLog) return;

    const uint32_t now = PacketCodec::GetTimestampMs();
    if (!force && (now - s_last_detailed_log_ms) < 1000) {
        return;
    }
    s_last_detailed_log_ms = now;

    const uint32_t mode = GetGameMode();
    const uint32_t substate = GetSubstate();
    const uint32_t gameType = GetNetplayGameType();

    uint32_t subTimer = 0;
    uint32_t simFrame = 0;
    uint32_t renderFrame = 0;
    uint32_t displayFrame = 0;
    uint32_t writeFrame = 0;
    uint32_t netFrame = 0;
    int32_t roundTimer = 0;
    uint32_t quickChecksum = 0;
    uint32_t p1Char = 0;
    uint32_t p2Char = 0;
    uint8_t p1Pal = 0;
    uint8_t p2Pal = 0;
    uint8_t p1Conf = 0;
    uint8_t p2Conf = 0;
    uint8_t stageId = 0;

    __try {
        subTimer = *reinterpret_cast<volatile uint32_t*>(ADDR_SUB_STATE_TIMER);
        simFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_SIMULATION);
        renderFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_COUNTER);
        displayFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_DISPLAY);
        writeFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_WRITE_IDX);
        netFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_NET_IDX);
        roundTimer = *reinterpret_cast<volatile int32_t*>(ADDR_ROUND_TIMER);
        p1Char = *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID);
        p2Char = *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID);
        p1Pal = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P1_PALETTE);
        p2Pal = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P2_PALETTE);
        p1Conf = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P1_CONFIRM);
        p2Conf = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P2_CONFIRM);
        stageId = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_STAGE_ID);
        if (mode == MODE_MATCH) {
            quickChecksum = AS2_GetQuickChecksum();
        }
    } __except (1) {
    }

    RenderHooks::Stats renderStats = {};
    AudioHooks::Stats audioStats = {};
    RenderHooks::GetStats(&renderStats);
    AudioHooks::GetStats(&audioStats);

    LOG_NET_INFO(
        "[GameHeartbeat] %s mode=%s(%u) sub=%s(%u) timer=%u type=%s(%u) session=%s active=%d "
        "rollback_started=%d barrier=%d charsel=%d bootstrap=%d rollback=%s "
        "rtt=%.1fms pkts=%u/%u render_supp=%d audio_supp=%d",
        reason ? reason : "tick",
        GameModeToString(mode), mode,
        SubstateToString(mode, substate), substate,
        subTimer,
        GameTypeToString(gameType), gameType,
        haveSessionSnapshot ? SessionManager::GetStateName(session.state) : "None",
        (haveSessionSnapshot && session.active) ? 1 : 0,
        s_rollback_started ? 1 : 0,
        s_waiting_for_load_barrier ? 1 : 0,
        CharSelSync::IsActive() ? 1 : 0,
        MatchBootstrap::IsActive() ? 1 : 0,
        RollbackStateToString(RollbackSession::GetState()),
        haveSessionSnapshot ? session.rtt_ms : 0.0f,
        haveSessionSnapshot ? session.packets_sent : 0,
        haveSessionSnapshot ? session.packets_received : 0,
        RenderHooks::IsSuppressed() ? 1 : 0,
        AudioHooks::IsSuppressed() ? 1 : 0);

    LOG_NET_INFO(
        "[GameHeartbeat] frames sim=%u render=%u disp=%u write=%u net=%u match_visual=%u "
        "quick=0x%04X round_timer=%d sel[p1=%u pal=%u conf=%u p2=%u pal=%u conf=%u stage=%u] "
        "render[%u/%u/%u] audio[%u/%u/%u]",
        simFrame,
        renderFrame,
        displayFrame,
        writeFrame,
        netFrame,
        AS2_GetMatchVisualFrame(),
        quickChecksum,
        roundTimer,
        p1Char, p1Pal, p1Conf,
        p2Char, p2Pal, p2Conf,
        stageId,
        renderStats.total_calls, renderStats.rendered_calls, renderStats.suppressed_calls,
        audioStats.total_calls, audioStats.passed_calls, audioStats.suppressed_calls);

    if (CharSelSync::IsActive()) {
        CharSelSync::Snapshot charsel = {};
        CharSelSync::GetSnapshot(&charsel);
        LOG_NET_INFO(
            "[GameHeartbeat] charsel phase=%u frame=%d remoteLatest=%d peerAck=%d sendHead=%d delay=%d stalls=%u "
            "slots[L=P%u R=P%u] locked=%d hash=0x%08X status=%s",
            (uint32_t)charsel.phase,
            charsel.local_frame,
            charsel.remote_confirmed,
            charsel.peer_acked_local,
            charsel.local_send_head,
            charsel.input_delay,
            charsel.stalls,
            (uint32_t)charsel.local_slot,
            (uint32_t)charsel.remote_slot,
            charsel.config_locked ? 1 : 0,
            charsel.config_hash,
            charsel.status);
    }

    if (MatchBootstrap::IsActive()) {
        MatchBootstrap::Snapshot bootstrap = {};
        MatchBootstrap::GetSnapshot(&bootstrap);
        LOG_NET_INFO(
            "[GameHeartbeat] bootstrap phase=%u load=%u ready=%u loaded[L=%d R=%d] "
            "asset[L=0x%08X R=0x%08X] baseline[L=%d R=%d frame=%u sumL=0x%08X sumR=0x%08X] "
            "delay=%u rtt=%.1f elapsed=%ums start_deadline=%u status=%s",
            (uint32_t)bootstrap.phase,
            (uint32_t)bootstrap.load_state,
            (uint32_t)bootstrap.ready_state,
            bootstrap.local_loaded ? 1 : 0,
            bootstrap.remote_loaded ? 1 : 0,
            bootstrap.local_asset_hash,
            bootstrap.remote_asset_hash,
            bootstrap.local_baseline_ready ? 1 : 0,
            bootstrap.remote_baseline_ready ? 1 : 0,
            bootstrap.baseline_frame,
            bootstrap.local_baseline_checksum,
            bootstrap.remote_baseline_checksum,
            bootstrap.negotiated_delay,
            bootstrap.rtt_ms,
            bootstrap.phase_elapsed_ms,
            bootstrap.start_deadline_ms,
            bootstrap.status);
    }

    if (RollbackSession::IsActive()) {
        RollbackSession::Snapshot rollback = {};
        RollbackSession::GetSnapshot(&rollback);
        LOG_NET_INFO(
            "[GameHeartbeat] rollback state=%s local=%d remote=%d ahead=%.2f save=%u load=%u "
            "advance=%u rb=%u adapter[%u/%u] pending=%u rolling=%d status=%s",
            RollbackStateToString(rollback.state),
            rollback.local_frame,
            rollback.remote_frame,
            rollback.frames_ahead,
            rollback.save_count,
            rollback.load_count,
            rollback.advance_count,
            rollback.rollback_count,
            rollback.adapter_send_count,
            rollback.adapter_recv_count,
            rollback.pending_event_count,
            rollback.rolling_back ? 1 : 0,
            rollback.status);
    }
}

static void UpdateAutoHooks() {
    if (!s_auto_hooks_enabled) return;

    const uint32_t currentMode = GetGameMode();
    const uint32_t currentSubstate = GetSubstate();
    const uint32_t currentGameType = GetNetplayGameType();

    const bool modeChanged = (currentMode != s_last_mode);
    const bool subChanged = (currentSubstate != s_last_substate);
    const bool typeChanged = (currentGameType != s_last_game_type);

    // Check if a netplay session is active (to decide whether to log to netcode file)
    SessionManager::Snapshot snap = {};
    bool sessionActive = SessionManager::GetSnapshot(&snap) && snap.active;

    // ── Pre-session arena zero + deterministic state reset ────────
    // When a session transitions to Connected, zero entity/match memory
    // to eliminate residual data from demo matches or prior sessions,
    // and immediately set RNG and FPU to deterministic defaults.
    // This runs once on connection, well before any match starts.
    if (sessionActive) {
        if (snap.state == SessionManager::State::Connected &&
            s_last_session_state != SessionManager::State::Connected) {
            LOG_NET_INFO("[GameState] Session connected — zeroing match arena + resetting RNG/FPU");
            AS2_ZeroMatchArena();

            // Reset FPU to deterministic defaults NOW, not at load barrier.
            // x87 CW  = 0x027F: PC=10 (double), RC=00 (nearest), all masks ON
            // MXCSR   = 0x1F80: RC=00, all masks ON, no flush-to-zero / DAZ
            {
                uint16_t defaultCW = 0x027F;
                uint32_t defaultMXCSR = 0x1F80;
                __asm {
                    fldcw word ptr [defaultCW]
                    ldmxcsr dword ptr [defaultMXCSR]
                }
            }

            // Set RNG to a known deterministic seed immediately.
            // This ensures any rand() calls during CharSel/StageSelect
            // (demo animations, weather, effects) produce identical results
            // on both peers.  The actual match session_seed will override
            // this later via RngHooks::ResetForMatch() at load barrier.
            AS2_SetRngSeed(0);
            AS2_SetVisualRngSeed(0);
            RngHooks::Install();  // Mark facade as installed

            LOG_NET_INFO("[GameState] Deterministic state reset: FPU=defaults RNG=0 visual_RNG=0");
        }
        s_last_session_state = snap.state;
    } else if (s_last_session_state != SessionManager::State::Idle) {
        s_last_session_state = SessionManager::State::Idle;
    }

    // Always log game type changes to netcode log (critical for debugging transitions)
    if (typeChanged) {
        LOG_NET_INFO("[GameState] GameType: %s(%u) -> %s(%u)",
            GameTypeToString(s_last_game_type), s_last_game_type,
            GameTypeToString(currentGameType), currentGameType);
        s_last_game_type = currentGameType;
        LogDetailedGameHeartbeat(true, "game-type-change");
    }

    if (modeChanged) {
        LOG_STATE(LOG_INFO, "Mode: %u -> %u", s_last_mode, currentMode);
        
        // Also log to netcode log when a session is active
        if (sessionActive) {
            LOG_NET_INFO("[GameState] Mode: %s(%u) -> %s(%u) [sub=%u type=%s session=%s]",
                GameModeToString(s_last_mode), s_last_mode,
                GameModeToString(currentMode), currentMode,
                currentSubstate,
                GameTypeToString(currentGameType),
                SessionManager::GetStateName(snap.state));
        }

        // When leaving Match mode, stop rollback and mark match end
        if (currentMode != MODE_MATCH && s_last_mode == MODE_MATCH) {
            // Record match result (winner byte) before cleanup
            if (sessionActive) {
                AS2_RecordMatchResult();
            }

            // Clean up load barrier if we exit during the freeze phase
            if (s_waiting_for_load_barrier) {
                s_waiting_for_load_barrier = false;
                s_load_barrier_signaled = false;
                InputSyncHooks::SetLoadBarrierFreeze(false);
                if (MatchBootstrap::IsActive()) {
                    MatchBootstrap::Abort("Left Match mode during load barrier");
                }
            }
            StopRollbackSession();
            AS2_EndMatch();
        }

        // When leaving CharSel mode during an active session, clean up sync systems.
        // IMPORTANT: Do NOT end CharSelSync when transitioning to MODE_STAGESEL (7).
        // CharSelSync::PollGameState() needs to detect MODE_STAGESEL to finalize config
        // lock (character + stage selections). CharSelSync will be ended naturally when
        // leaving StageSelect or when the config is locked and bootstrap takes over.
        if (currentMode != MODE_CHARSEL && s_last_mode == MODE_CHARSEL) {
            if (currentMode != MODE_STAGESEL) {
                if (CharSelSync::IsActive()) {
                    LOG_NET_INFO("[GameState] Left CharSel (to mode %u, not StageSelect) — ending CharSelSync", currentMode);
                    CharSelSync::End();
                }
            } else {
                LOG_NET_INFO("[GameState] CharSel -> StageSelect — keeping CharSelSync alive for config lock");
            }
            if (MatchBootstrap::IsActive() && currentMode != MODE_MATCH && currentMode != MODE_STAGESEL) {
                LOG_NET_INFO("[GameState] Left CharSel with active MatchBootstrap (not entering Match/StageSelect) — aborting");
                MatchBootstrap::Abort("Left CharSel mode unexpectedly");
            }
        }

        // When leaving StageSelect mode, end CharSelSync if it's still alive.
        // By this point PollGameState() should have detected MODE_STAGESEL and locked the config.
        if (currentMode != MODE_STAGESEL && s_last_mode == MODE_STAGESEL) {
            if (CharSelSync::IsActive()) {
                LOG_NET_INFO("[GameState] Left StageSelect — ending CharSelSync (locked=%d)", CharSelSync::IsConfigLocked() ? 1 : 0);
                CharSelSync::End();
            }
            if (MatchBootstrap::IsActive() && currentMode != MODE_MATCH) {
                LOG_NET_INFO("[GameState] Left StageSelect with active MatchBootstrap (not entering Match) — aborting");
                MatchBootstrap::Abort("Left StageSelect mode unexpectedly");
            }
        }

        // ── Pre-load arena zero + deterministic reset ────────────────
        // When entering Match mode for a mod-owned session, zero entity
        // blocks, match context, and match header BEFORE the game loads
        // character assets.  This eliminates residual data from any prior
        // match (including the title-screen demo) that the game's own
        // init does not overwrite, preventing load-barrier hash mismatches.
        // Also reset FPU/RNG to deterministic defaults so any rand() calls
        // during asset loading produce identical results on both peers.
        //
        // CRITICAL: The full scrub (effects, summons, FPU, input pipeline)
        // runs HERE — not at load barrier time.  Running it during the load
        // barrier (after substate 2 has set up the intro freeze flag, fade
        // timer, and entity init) interferes with the round-start intro
        // animation / "fight!" call.  At Mode→Match entry, none of that
        // has been set up yet, so the scrub is safe.
        if (currentMode == MODE_MATCH && s_last_mode != MODE_MATCH && sessionActive) {
            AS2_ZeroMatchArena();           // entities, context, header, gap
            AS2_ScrubTransientMatchState(); // effects, summons, FPU, input pipeline
            AS2_SetRngSeed(0);
            AS2_SetVisualRngSeed(0);
            LOG_NET_INFO("[GameState] Pre-load: full state scrub (arena+effects+summons+input+FPU+RNG) for match entry");
        }

        // ── Cursor sync on any charsel re-entry from a post-match mode ──
        // The game's charsel init (sub 0) reads char IDs from memory to
        // position cursors.  On the client, SessionManager may have already
        // called EnterCharSel() (which does its own cursor sync + Begin())
        // before the game mode actually changes, so CharSelSync may already
        // be active here.  We still need a second sync at mode-change time
        // because the game init may have consumed stale values by this point.
        if (currentMode == MODE_CHARSEL && sessionActive &&
            !MatchBootstrap::IsActive()) {
            CharSelSync::RestoreCursorsFromLastConfig();
        }

        // When entering CharSel mode during an active session that's in
        // Connected state (e.g. returning from win screen after a match),
        // re-activate CharSelSync so input relay works for the next match.
        // On the client this may already be active (Ready packet started it).
        if (currentMode == MODE_CHARSEL && sessionActive &&
            snap.state == SessionManager::State::Connected &&
            !CharSelSync::IsActive() && !MatchBootstrap::IsActive()) {
            LOG_NET_INFO("[GameState] Re-entering CharSel with active session — restarting CharSelSync");
            SessionManager::EnterCharSel();
        }

        // Safety net: if the game enters MODE_LOBBY during an active session,
        // this means the vanilla disconnect code fired. Redirect to our menu.
        if (currentMode == MODE_LOBBY && sessionActive) {
            LOG_NET_WARN("[GameState] Game entered MODE_LOBBY during active session — vanilla fallback detected!");
            HandleDisconnection("Connection lost (vanilla lobby fallback)");
        }

        s_last_mode = currentMode;
        LogDetailedGameHeartbeat(true, "mode-change");
    }

    if (subChanged) {
        if (currentMode == MODE_MATCH) {
            LOG_STATE(LOG_INFO, "Substate: %u -> %u", s_last_substate, currentSubstate);
            if (sessionActive) {
                LOG_NET_INFO("[GameState] Match substate: %u -> %u [type=%s session=%s]",
                    s_last_substate, currentSubstate,
                    GameTypeToString(currentGameType),
                    SessionManager::GetStateName(snap.state));
            }

            // Start load barrier when entering Gameplay substate for a mod-owned
            // online session. The game itself may still be in VS_HUMAN because we
            // no longer rely on the vanilla netplay game type for the handoff.
            if (currentSubstate == MATCH_SUB_GAMEPLAY) {
                if (snap.active && !snap.has_error && !s_rollback_started && !s_waiting_for_load_barrier) {
                    s_waiting_for_load_barrier = true;
                    s_load_barrier_signaled = false;
                    s_load_barrier_start_ms = PacketCodec::GetTimestampMs();
                    InputSyncHooks::SetLoadBarrierFreeze(true);
                    LOG_NET_INFO("[NetplayHooks] Match substate 3 — entering load barrier (waiting for peer)");
                }
            }

            // Handle leaving Gameplay substate
            if (s_last_substate == MATCH_SUB_GAMEPLAY && currentSubstate != MATCH_SUB_GAMEPLAY) {
                // Clean up load barrier if we leave substate 3 during the freeze
                if (s_waiting_for_load_barrier) {
                    s_waiting_for_load_barrier = false;
                    s_load_barrier_signaled = false;
                    InputSyncHooks::SetLoadBarrierFreeze(false);
                    if (MatchBootstrap::IsActive()) {
                        MatchBootstrap::Abort("Left Gameplay substate during load barrier");
                    }
                }
                // DO NOT destroy the rollback session here.
                // Between-round transitions (sub 3→2→3) are part of normal
                // gameplay within Match mode.  The session must survive so
                // both peers keep exchanging inputs through GekkoNet.
                // Session cleanup happens when leaving Match mode entirely
                // (see the modeChanged block above).
                if (s_rollback_started) {
                    LOG_NET_INFO("[NetplayHooks] Match substate %u -> %u — rollback session continues (round transition)",
                        s_last_substate, currentSubstate);
                }
            }
        } else if (currentMode == MODE_CHARSEL && sessionActive) {
            LOG_NET_INFO("[GameState] CharSel substate: %u -> %u [type=%s session=%s]",
                s_last_substate, currentSubstate,
                GameTypeToString(currentGameType),
                SessionManager::GetStateName(snap.state));
        } else {
            LOG_STATE(LOG_DEBUG, "Substate: %u -> %u", s_last_substate, currentSubstate);
        }
        s_last_substate = currentSubstate;
        LogDetailedGameHeartbeat(true, "substate-change");
    }
}

void NetplayFrameUpdate() {
    UpdateAutoHooks();
    NetplayMenuController::FrameUpdate();

    if (StressTest::IsRunning()) {
        StressTest::FrameUpdate();
    }

    if (MatchRunner::IsRunning()) {
        MatchRunner::FrameUpdate();
    }

    // ── Load barrier: drive bootstrap to completion ─────────────────
    if (s_waiting_for_load_barrier) {
        // Signal NotifyLocalLoaded once bootstrap is active
        // (MatchBootstrap::Begin may not have been called yet when we first freeze)
        if (!s_load_barrier_signaled && MatchBootstrap::IsActive()) {
            MatchBootstrap::NotifyLocalLoaded();
            s_load_barrier_signaled = true;
            LOG_NET_INFO("[NetplayHooks] Load barrier: signaled local loaded to bootstrap");
        }

        // Check for completion
        if (MatchBootstrap::IsComplete()) {
            if (!RollbackSession::IsActive()) {
                LOG_NET_INFO("[NetplayHooks] Load barrier COMPLETE — creating rollback session");
                if (!CreateRollbackSession()) {
                    s_waiting_for_load_barrier = false;
                    s_load_barrier_signaled = false;
                    InputSyncHooks::SetLoadBarrierFreeze(false);
                    HandleDisconnection("Failed to create rollback session");
                    return;
                }
            }

            if (!s_rollback_started) {
                RollbackSession::NetworkPoll();

                if (RollbackSession::GetState() == RollbackSession::State::Error) {
                    LOG_NET_ERROR("[NetplayHooks] Gekko sync FAILED during load barrier");
                    s_waiting_for_load_barrier = false;
                    s_load_barrier_signaled = false;
                    InputSyncHooks::SetLoadBarrierFreeze(false);
                    HandleDisconnection("Rollback sync failed");
                    return;
                }

                if (RollbackSession::GetState() == RollbackSession::State::Running) {
                    LOG_NET_INFO("[NetplayHooks] Gekko sync COMPLETE — enabling rollback gameplay");
                    s_waiting_for_load_barrier = false;
                    s_load_barrier_signaled = false;
                    InputSyncHooks::SetLoadBarrierFreeze(false);
                    ActivateRollbackSession();
                }
            }
        }
        // Check for error
        else if (MatchBootstrap::IsActive() && MatchBootstrap::GetPhase() == MatchBootstrap::Phase::Error) {
            LOG_NET_ERROR("[NetplayHooks] Load barrier FAILED — bootstrap error");
            s_waiting_for_load_barrier = false;
            s_load_barrier_signaled = false;
            InputSyncHooks::SetLoadBarrierFreeze(false);
            HandleDisconnection("Load barrier failed");
        }
        // Check for timeout (entire load barrier wait, including pre-bootstrap)
        else {
            uint32_t elapsed = PacketCodec::GetTimestampMs() - s_load_barrier_start_ms;
            if (elapsed > kLoadBarrierTimeoutMs) {
                LOG_NET_ERROR("[NetplayHooks] Load barrier TIMEOUT after %ums", elapsed);
                s_waiting_for_load_barrier = false;
                s_load_barrier_signaled = false;
                InputSyncHooks::SetLoadBarrierFreeze(false);
                if (MatchBootstrap::IsActive()) {
                    MatchBootstrap::Abort("Load barrier timeout");
                }
                HandleDisconnection("Load barrier timeout — peer did not respond");
            }
        }
    }

    LogDetailedGameHeartbeat(false, "tick");
}

void HandleDisconnection(const char* reason) {
    LOG_NETPLAY(LOG_INFO, "[NetplayHooks] Disconnection: %s", reason ? reason : "(no reason)");
    NetplayMenuController::HandleDisconnection(reason);
}

} // namespace NetplayHooks
