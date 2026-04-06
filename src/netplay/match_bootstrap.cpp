/**
 * Alice Senki 2 - Match Bootstrap Manager Implementation
 *
 * Drives the full match bootstrap sequence: config apply → load barrier →
 * baseline sync → ready sync → gameplay start (§7).
 */

#include "match_bootstrap.h"
#include "session_manager.h"
#include "as2_constants.h"
#include "as2_rollback.h"
#include "netplay_hooks.h"
#include "rng_hooks.h"
#include "log_window.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

namespace MatchBootstrap {

// ============================================================================
// Internal State
// ============================================================================

static bool       s_active = false;
static bool       s_isHost = false;
static uint64_t   s_sessionId    = 0;
static uint32_t   s_connectionId = 0;

static Phase      s_phase = Phase::Idle;
static LoadState  s_loadState  = LoadState::Idle;
static ReadyState s_readyState = ReadyState::Idle;

static CharSelSync::LockedMatchConfig s_config = {};
static uint32_t s_configHash = 0;

// Load barrier tracking
static bool     s_localLoaded  = false;
static bool     s_remoteLoaded = false;
static uint32_t s_localAssetHash  = 0;
static uint32_t s_remoteAssetHash = 0;
static CompactSaveState_t s_localLoadSnapshot = {};  // kept for mismatch diagnostics

// Baseline sync tracking
static bool     s_localBaselineReady  = false;
static bool     s_remoteBaselineReady = false;
static uint32_t s_localBaselineChecksum  = 0;
static uint32_t s_remoteBaselineChecksum = 0;
static uint32_t s_baselineFrame = 0;

// Ready sync / delay
static bool     s_localReady  = false;
static bool     s_remoteReady = false;
static uint8_t  s_negotiatedDelay = 2;  // Default delay
static float    s_rttMs = 0.0f;

// Timing
static uint32_t s_phaseStartMs = 0;
static uint32_t s_startDeadlineMs = 0;
static constexpr uint32_t TIMEOUT_MS = 15000;  // 15 seconds (generous for bad connections)
static uint32_t s_lastHeartbeatMs = 0;

static constexpr uint8_t kMinGameplayDelay = 0;
static constexpr uint8_t kMaxGameplayDelay = 6;

// Reliable send tracking — send once, ENet handles retransmits.
static uint32_t s_loadBarrierSeq   = 0;   // 0 = not sent
static uint32_t s_baselineReadySeq = 0;
static uint32_t s_startGameplaySeq = 0;
static uint32_t s_startGameplayAckSeq = 0;

// Error state
static char s_errorMsg[128] = {};

// ============================================================================
// Helpers
// ============================================================================

static const char* PhaseToString(Phase p) {
    switch (p) {
        case Phase::Idle:         return "Idle";
        case Phase::ApplyConfig:  return "ApplyConfig";
        case Phase::Loading:      return "Loading";
        case Phase::BaselineSync: return "BaselineSync";
        case Phase::ReadySync:    return "ReadySync";
        case Phase::Starting:     return "Starting";
        case Phase::Complete:     return "Complete";
        case Phase::Error:        return "Error";
        default:                  return "Unknown";
    }
}

static const char* LoadStateToString(LoadState s) {
    switch (s) {
        case LoadState::Idle:      return "Idle";
        case LoadState::Waiting:   return "Waiting";
        case LoadState::Ready:     return "Ready";
        case LoadState::Confirmed: return "Confirmed";
        case LoadState::Mismatch:  return "Mismatch";
        case LoadState::Timeout:   return "Timeout";
        default:                   return "Unknown";
    }
}

static const char* ReadyStateToString(ReadyState s) {
    switch (s) {
        case ReadyState::Idle:     return "Idle";
        case ReadyState::Announce: return "Announce";
        case ReadyState::Confirm:  return "Confirm";
        case ReadyState::Done:     return "Done";
        default:                   return "Unknown";
    }
}

static uint32_t Now() {
    return PacketCodec::GetTimestampMs();
}

static void LogSnapshotSectionCrcs(const CompactSaveState_t& ss, const char* prefix) {
    const uint32_t crc_fpu = PacketCodec::Crc32(ss.fpu_state, sizeof(ss.fpu_state));
    const uint32_t crc_global = PacketCodec::Crc32(&ss.global, sizeof(ss.global));
    const uint32_t crc_p1 = PacketCodec::Crc32(ss.p1_entity, sizeof(ss.p1_entity));
    const uint32_t crc_p2 = PacketCodec::Crc32(ss.p2_entity, sizeof(ss.p2_entity));
    const uint32_t crc_matchctx = PacketCodec::Crc32(ss.match_context, sizeof(ss.match_context));
    const uint32_t crc_match = PacketCodec::Crc32(&ss.match, sizeof(ss.match));
    const uint32_t crc_fx = PacketCodec::Crc32(&ss.effects, sizeof(ss.effects));
    const uint32_t crc_input = PacketCodec::Crc32(&ss.input, sizeof(ss.input));
    const uint32_t crc_summons = PacketCodec::Crc32(&ss.summons, sizeof(ss.summons));

    LOG_NET_INFO(
        "[Bootstrap] %s section CRCs: fpu=0x%08X global=0x%08X p1=0x%08X p2=0x%08X "
        "matchctx=0x%08X match=0x%08X fx=0x%08X input=0x%08X summons=0x%08X",
        prefix ? prefix : "snapshot",
        crc_fpu,
        crc_global,
        crc_p1,
        crc_p2,
        crc_matchctx,
        crc_match,
        crc_fx,
        crc_input,
        crc_summons);
}

static void LogInputSnapshotCrcs(const CompactSaveState_t& ss, const char* prefix) {
    const uint32_t crc_input_idx = PacketCodec::Crc32(&ss.input.read_idx, sizeof(ss.input.read_idx) * 4);
    const uint32_t crc_p1_buf = PacketCodec::Crc32(ss.input.p1_buffer, sizeof(ss.input.p1_buffer));
    const uint32_t crc_p2_buf = PacketCodec::Crc32(ss.input.p2_buffer, sizeof(ss.input.p2_buffer));
    const uint32_t crc_p1_state = PacketCodec::Crc32(ss.input.p1_state, sizeof(ss.input.p1_state));
    const uint32_t crc_p2_state = PacketCodec::Crc32(ss.input.p2_state, sizeof(ss.input.p2_state));
    const uint32_t crc_p1_hist = PacketCodec::Crc32(ss.input.p1_history, sizeof(ss.input.p1_history));
    const uint32_t crc_p2_hist = PacketCodec::Crc32(ss.input.p2_history, sizeof(ss.input.p2_history));

    LOG_NET_INFO(
        "[Bootstrap] %s input CRCs: idx=0x%08X p1_buf=0x%08X p2_buf=0x%08X "
        "p1_state=0x%08X p2_state=0x%08X p1_hist=0x%08X p2_hist=0x%08X "
        "cur=0x%04X/0x%04X",
        prefix ? prefix : "snapshot",
        crc_input_idx,
        crc_p1_buf,
        crc_p2_buf,
        crc_p1_state,
        crc_p2_state,
        crc_p1_hist,
        crc_p2_hist,
        ss.input.p1_input,
        ss.input.p2_input);
}

static void LogLoadToBaselineDelta(const CompactSaveState_t& baseline) {
    if (!s_localLoaded) {
        return;
    }

    const auto crcOf = [](const void* data, size_t size) {
        return PacketCodec::Crc32(data, (int)size);
    };

    const uint32_t load_fpu = crcOf(s_localLoadSnapshot.fpu_state, sizeof(s_localLoadSnapshot.fpu_state));
    const uint32_t base_fpu = crcOf(baseline.fpu_state, sizeof(baseline.fpu_state));
    const uint32_t load_global = crcOf(&s_localLoadSnapshot.global, sizeof(s_localLoadSnapshot.global));
    const uint32_t base_global = crcOf(&baseline.global, sizeof(baseline.global));
    const uint32_t load_p1 = crcOf(s_localLoadSnapshot.p1_entity, sizeof(s_localLoadSnapshot.p1_entity));
    const uint32_t base_p1 = crcOf(baseline.p1_entity, sizeof(baseline.p1_entity));
    const uint32_t load_p2 = crcOf(s_localLoadSnapshot.p2_entity, sizeof(s_localLoadSnapshot.p2_entity));
    const uint32_t base_p2 = crcOf(baseline.p2_entity, sizeof(baseline.p2_entity));
    const uint32_t load_matchctx = crcOf(s_localLoadSnapshot.match_context, sizeof(s_localLoadSnapshot.match_context));
    const uint32_t base_matchctx = crcOf(baseline.match_context, sizeof(baseline.match_context));
    const uint32_t load_match = crcOf(&s_localLoadSnapshot.match, sizeof(s_localLoadSnapshot.match));
    const uint32_t base_match = crcOf(&baseline.match, sizeof(baseline.match));
    const uint32_t load_fx = crcOf(&s_localLoadSnapshot.effects, sizeof(s_localLoadSnapshot.effects));
    const uint32_t base_fx = crcOf(&baseline.effects, sizeof(baseline.effects));
    const uint32_t load_input = crcOf(&s_localLoadSnapshot.input, sizeof(s_localLoadSnapshot.input));
    const uint32_t base_input = crcOf(&baseline.input, sizeof(baseline.input));
    const uint32_t load_summons = crcOf(&s_localLoadSnapshot.summons, sizeof(s_localLoadSnapshot.summons));
    const uint32_t base_summons = crcOf(&baseline.summons, sizeof(baseline.summons));

    LOG_NET_INFO(
        "[Bootstrap] load->baseline delta: fpu=%c global=%c p1=%c p2=%c matchctx=%c match=%c fx=%c input=%c summons=%c",
        load_fpu == base_fpu ? '=' : '!',
        load_global == base_global ? '=' : '!',
        load_p1 == base_p1 ? '=' : '!',
        load_p2 == base_p2 ? '=' : '!',
        load_matchctx == base_matchctx ? '=' : '!',
        load_match == base_match ? '=' : '!',
        load_fx == base_fx ? '=' : '!',
        load_input == base_input ? '=' : '!',
        load_summons == base_summons ? '=' : '!');
}

static void LogHeartbeat(bool force, const char* reason) {
    if (!s_active) return;

    const uint32_t now = Now();
    if (!force && (now - s_lastHeartbeatMs) < 1000) {
        return;
    }
    s_lastHeartbeatMs = now;

    int32_t startInMs = 0;
    if (s_startDeadlineMs != 0) {
        startInMs = (int32_t)(s_startDeadlineMs - now);
    }

    LOG_NET_INFO(
        "[Bootstrap] Heartbeat(%s): phase=%s load=%s ready=%s loaded[L=%d R=%d] "
        "asset[L=0x%08X R=0x%08X] baseline[L=%d R=%d frame=%u sumL=0x%08X sumR=0x%08X] "
        "ready_flags[L=%d R=%d] delay=%u rtt=%.1fms start_in=%dms elapsed=%ums",
        reason ? reason : "tick",
        PhaseToString(s_phase),
        LoadStateToString(s_loadState),
        ReadyStateToString(s_readyState),
        s_localLoaded ? 1 : 0,
        s_remoteLoaded ? 1 : 0,
        s_localAssetHash,
        s_remoteAssetHash,
        s_localBaselineReady ? 1 : 0,
        s_remoteBaselineReady ? 1 : 0,
        s_baselineFrame,
        s_localBaselineChecksum,
        s_remoteBaselineChecksum,
        s_localReady ? 1 : 0,
        s_remoteReady ? 1 : 0,
        s_negotiatedDelay,
        s_rttMs,
        startInMs,
        now - s_phaseStartMs);
}

// With rollback, the prediction window absorbs most latency, but some
// minimum input delay based on RTT is still beneficial for reducing
// rollback depth and jitter.  Formula: ceil(RTT_ms * 60 / 2000) gives
// the number of frames it takes a one-way packet to arrive, halved
// because rollback covers the other half.  Clamped to [kMin, kMax].
static uint8_t ComputeSuggestedGameplayDelay(float rttMs) {
    if (rttMs <= 0.0f) return kMinGameplayDelay;
    int delay = (int)ceilf(rttMs * 60.0f / 2000.0f);
    if (delay < (int)kMinGameplayDelay) delay = (int)kMinGameplayDelay;
    if (delay > (int)kMaxGameplayDelay) delay = (int)kMaxGameplayDelay;
    return (uint8_t)delay;
}

static void SetPhase(Phase newPhase) {
    Phase old = s_phase;
    s_phase = newPhase;
    s_phaseStartMs = Now();
    // Reset reliable send tracking for the new phase
    s_loadBarrierSeq = 0;
    s_baselineReadySeq = 0;
    s_startGameplaySeq = 0;
    s_startGameplayAckSeq = 0;
    LOG_NET_INFO("[Bootstrap] Phase: %s -> %s", PhaseToString(old), PhaseToString(newPhase));
    LogHeartbeat(true, "phase");
}

static void SetError(const char* msg) {
    snprintf(s_errorMsg, sizeof(s_errorMsg), "%s", msg);
    SetPhase(Phase::Error);
    LOG_NET_ERROR("[Bootstrap] ERROR: %s", msg);
}

// ============================================================================
// Config Application (§7 + §14.2)
// ============================================================================

static void ApplyConfigToGameMemory() {
    // Read what the game currently has in memory.
    uint32_t gameP1 = *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID);
    uint32_t gameP2 = *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID);
    uint8_t  gamePal1 = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P1_PALETTE);
    uint8_t  gamePal2 = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P2_PALETTE);
    uint8_t  gameStage = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_STAGE_ID);
    
    LOG_NET_INFO("[Bootstrap] MatchConfig: P1=char%u/pal%u P2=char%u/pal%u stage=%u seed=%u",
                 s_config.p1_character_id, s_config.p1_palette_id,
                 s_config.p2_character_id, s_config.p2_palette_id,
                 s_config.stage_id, s_config.session_seed);
    LOG_NET_INFO("[Bootstrap] Game memory (before): P1=char%u/pal%u P2=char%u/pal%u stage=%u",
                 gameP1, gamePal1, gameP2, gamePal2, gameStage);
    
    // Force-write authoritative MatchConfig values into game memory.
    // The game blows through CHARSEL_SUB_STAGE (11) in zero lockstep frames,
    // so stage auto-selects locally and can diverge between host and client.
    // Characters/palettes SHOULD converge via charsel lockstep, but we write
    // them too as a safety net against any future relay bugs.
    *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID) = s_config.p1_character_id;
    *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID) = s_config.p2_character_id;
    *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P1_PALETTE)  = s_config.p1_palette_id;
    *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P2_PALETTE)  = s_config.p2_palette_id;
    *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_STAGE_ID)    = s_config.stage_id;

    if (gameP1 != s_config.p1_character_id || gameP2 != s_config.p2_character_id ||
        gamePal1 != s_config.p1_palette_id || gamePal2 != s_config.p2_palette_id) {
        LOG_NET_ERROR("[Bootstrap] CHARSEL DIVERGENCE: character/palette differs from MatchConfig! "
                      "Input relay bug — both sides should have converged naturally.");
    }
    if (gameStage != s_config.stage_id) {
        LOG_NET_WARN("[Bootstrap] Stage divergence: game had %u, MatchConfig says %u — corrected.",
                     gameStage, s_config.stage_id);
    }

    LOG_NET_INFO("[Bootstrap] Game memory (after): P1=char%u/pal%u P2=char%u/pal%u stage=%u",
                 s_config.p1_character_id, s_config.p1_palette_id,
                 s_config.p2_character_id, s_config.p2_palette_id,
                 s_config.stage_id);

    // Normalize the game's wall-clock baseline before Match mode setup runs.
    // Peers reach Mode 8 at different real times, and some pre-frame systems
    // consult the tick source during the handoff.
    AS2_SyncTickBaseline(0);

    LOG_NET_INFO("[Bootstrap] Tick baseline synchronized; session seed %u reserved for pre-frame handoff",
                 s_config.session_seed);
}

// ============================================================================
// Load Barrier (§7.1)
// ============================================================================

// Helper: check if a reliable send is still pending.
// With ENet reliable delivery, once we send, delivery is guaranteed.
// We use a simple "sent once" flag instead of polling transport acks.
static bool IsReliablePending(uint32_t seq) {
    // With ENet, reliable packets are guaranteed to arrive or the connection drops.
    // Return false to allow re-sends (the state machine relies on application-level
    // acks anyway, and double-sends of identical state are harmless).
    return false;
}

static uint32_t ComputeLoadStateHash(CompactSaveState_t* outSnapshot = nullptr) {
    CompactSaveState_t snapshot = {};
    if (AS2_SaveStateToBuffer(&snapshot)) {
        if (outSnapshot) {
            *outSnapshot = snapshot;
        }
        LOG_NET_DEBUG("[Bootstrap] LoadStateHash via savestate: crc=0x%08X", snapshot.checksum);
        return snapshot.checksum;
    }

    LOG_NET_WARN("[Bootstrap] Savestate capture failed — using fallback CRC");
    if (outSnapshot) {
        memset(outSnapshot, 0, sizeof(*outSnapshot));
    }

    struct {
        uint32_t config_hash;
        uint16_t quick_checksum;
        uint16_t reserved;
        uint32_t game_mode;
        uint32_t sub_state;
    } fallback = {};
    fallback.config_hash = s_configHash;
    fallback.quick_checksum = AS2_GetQuickChecksum();
    fallback.game_mode = *reinterpret_cast<volatile uint32_t*>(ADDR_GAME_MODE);
    fallback.sub_state = *reinterpret_cast<volatile uint32_t*>(ADDR_SUB_STATE);
    uint32_t hash = PacketCodec::Crc32(&fallback, sizeof(fallback));
    LOG_NET_DEBUG("[Bootstrap] Fallback hash: crc=0x%08X mode=%u sub=%u qchk=0x%04X",
                 hash, fallback.game_mode, fallback.sub_state, fallback.quick_checksum);
    return hash;
}

static void SendStartGameplayEcho() {
    PacketCodec::StartGameplayPayload payload = {};
    payload.config_hash = s_configHash;
    payload.baseline_frame = s_baselineFrame;
    payload.start_epoch_ms = s_startDeadlineMs;
    payload.input_delay_p1 = s_negotiatedDelay;
    payload.input_delay_p2 = s_negotiatedDelay;

    SessionManager::SendToPeer(PacketCodec::PacketType::StartGameplay,
                               &payload, sizeof(payload), true);
    LOG_NET_INFO("[Bootstrap] Host sent final StartGameplay echo");
}

static void MarkReadyAndStart() {
    s_localReady = true;
    s_remoteReady = true;
    s_readyState = ReadyState::Done;
    SetPhase(Phase::Starting);
    LOG_NET_INFO("[Bootstrap] Both sides ready — negotiated delay=%u", s_negotiatedDelay);
}

static void SendLoadBarrierReady() {
    LOG_NET_DEBUG("[Bootstrap] >>> SendLoadBarrierReady ENTRY (seq=%u phase=%d localLoaded=%d)",
                 s_loadBarrierSeq, (int)s_phase, s_localLoaded ? 1 : 0);
    // With ENet reliable, just send once — ENet handles retransmit
    if (s_loadBarrierSeq != 0) {
        LOG_NET_DEBUG("[Bootstrap] SendLoadBarrierReady: skipped (already sent)");
        return;
    }
    
    PacketCodec::LoadBarrierReadyPayload payload = {};
    payload.config_hash    = s_configHash;
    payload.load_flags     = 0;
    payload.local_asset_hash = s_localAssetHash;
    
    SessionManager::SendToPeer(PacketCodec::PacketType::LoadBarrierReady,
                               &payload, sizeof(payload), true);
    s_loadBarrierSeq = 1;  // Mark as sent
    
    LOG_NET_INFO("[Bootstrap] Sent LoadBarrierReady (asset_hash=0x%08X)", s_localAssetHash);
}

static void CheckLoadBarrier() {
    if (s_phase != Phase::Loading) return;  // Only transition from Loading
    if (!s_localLoaded || !s_remoteLoaded) return;

    if (s_localAssetHash != s_remoteAssetHash) {
        LOG_NET_ERROR("[Bootstrap] LOAD BARRIER MISMATCH! local=0x%08X remote=0x%08X",
                  s_localAssetHash, s_remoteAssetHash);

        // Dump per-section CRCs from the local snapshot so the log alone shows
        // which state region is most likely divergent between peers.
        LogSnapshotSectionCrcs(s_localLoadSnapshot, "mismatch-local");
        LogInputSnapshotCrcs(s_localLoadSnapshot, "mismatch-local");

        s_loadState = LoadState::Mismatch;
        SetError("Load barrier asset mismatch — peers loaded different state");
        return;
    }

    s_loadState = LoadState::Confirmed;
    LOG_NET_INFO("[Bootstrap] Load barrier CONFIRMED — both sides loaded");
    SetPhase(Phase::BaselineSync);
}

// ============================================================================
// Baseline Sync (§7.2)
// ============================================================================

static void CaptureBaseline() {
    if (s_localBaselineReady) return;
    
    LOG_NET_INFO("[Bootstrap] Capturing baseline savestate...");
    
    s_baselineFrame = 0;

    // Re-assert the authoritative match seed immediately before frame 0.
    RngHooks::ResetForMatch(s_config.session_seed);
    
    // Capture current game state and compute CRC32 checksum
    CompactSaveState_t baseline = {};
    if (AS2_SaveStateToBuffer(&baseline)) {
        s_localBaselineChecksum = baseline.checksum;
        char summary[512] = {};
        const char* summaryText = AS2_FormatCompactStateSummary(&baseline, summary, sizeof(summary))
            ? summary
            : "summary-unavailable";
        LOG_NET_INFO("[Bootstrap] Baseline summary: %s", summaryText);
        LogSnapshotSectionCrcs(baseline, "baseline");
        LogInputSnapshotCrcs(baseline, "baseline");
        LogLoadToBaselineDelta(baseline);
    } else {
        LOG_NET_WARN("[Bootstrap] Failed to capture baseline — using fallback checksum 0");
        s_localBaselineChecksum = 0;
    }
    s_localBaselineReady = true;
    
    LOG_NET_INFO("[Bootstrap] Baseline captured at frame %u (checksum=0x%08X)",
             s_baselineFrame, s_localBaselineChecksum);
}

static void SendBaselineReady() {
    // With ENet reliable, just send once
    if (s_baselineReadySeq != 0) {
        return;
    }
    
    PacketCodec::BaselineReadyPayload payload = {};
    payload.config_hash        = s_configHash;
    payload.baseline_frame     = s_baselineFrame;
    payload.baseline_checksum  = s_localBaselineChecksum;
    
    SessionManager::SendToPeer(PacketCodec::PacketType::BaselineReady,
                               &payload, sizeof(payload), true);
    s_baselineReadySeq = 1;  // Mark as sent
    
    LOG_NET_INFO("[Bootstrap] Sent BaselineReady (frame=%u checksum=0x%08X)",
             s_baselineFrame, s_localBaselineChecksum);
}

static void CheckBaselineAgreement() {
    if (s_phase != Phase::BaselineSync) return;  // Only transition from BaselineSync
    if (!s_localBaselineReady || !s_remoteBaselineReady) return;
    
    if (s_localBaselineChecksum != s_remoteBaselineChecksum) {
        LOG_NET_ERROR("[Bootstrap] BASELINE MISMATCH! local=0x%08X remote=0x%08X",
                  s_localBaselineChecksum, s_remoteBaselineChecksum);
        SetError("Baseline checksum mismatch — game state differs");
        return;
    }
    
    LOG_NET_INFO("[Bootstrap] Baseline AGREED (checksum=0x%08X)", s_localBaselineChecksum);
    SetPhase(Phase::ReadySync);
}

// ============================================================================
// Ready Sync (§7.2 READY_SYNC)
// ============================================================================

static void DriveReadySync() {
    if (s_isHost) {
        // Host: calculate delay from RTT, send StartGameplay
        if (s_readyState == ReadyState::Idle) {
            uint8_t suggestedDelay = ComputeSuggestedGameplayDelay(s_rttMs);
            const int preferredDelay = NetplayHooks::GetNetplayFrameDelay();
            if (preferredDelay > (int)suggestedDelay) {
                suggestedDelay = (uint8_t)((preferredDelay > (int)kMaxGameplayDelay)
                    ? kMaxGameplayDelay
                    : preferredDelay);
            }
            s_negotiatedDelay = suggestedDelay;
            s_readyState = ReadyState::Announce;
            LOG_NET_INFO("[Bootstrap] Host calculated delay=%u (RTT=%.1fms preferred=%d)",
                         suggestedDelay,
                         s_rttMs,
                         preferredDelay);
        }

        if (s_readyState == ReadyState::Announce) {
            if (s_remoteReady) {
                MarkReadyAndStart();
                return;
            }

            // Send once via ENet reliable — it handles retransmits
            if (s_startGameplaySeq != 0) {
                return;  // Already sent
            }
            
            PacketCodec::StartGameplayPayload payload = {};
            payload.config_hash   = s_configHash;
            payload.baseline_frame = s_baselineFrame;
            payload.start_epoch_ms = Now() + 500;
            payload.input_delay_p1 = s_negotiatedDelay;
            payload.input_delay_p2 = s_negotiatedDelay;

            s_startDeadlineMs = payload.start_epoch_ms;
            
            SessionManager::SendToPeer(PacketCodec::PacketType::StartGameplay,
                                       &payload, sizeof(payload), true);
            s_startGameplaySeq = 1;  // Mark as sent
            
            LOG_NET_INFO("[Bootstrap] Host sent StartGameplay (delay=%u)", s_negotiatedDelay);
        }
    } else {
        // Client: wait for StartGameplay from host, then confirm it reliably.
        if (s_readyState == ReadyState::Idle) {
            s_readyState = ReadyState::Confirm;
            LOG_NET_INFO("[Bootstrap] Client waiting for StartGameplay from host...");
            return;
        }

        if (s_readyState == ReadyState::Confirm && s_remoteReady) {
            if (s_startGameplayAckSeq != 0) {
                return;  // Already sent
            }

            PacketCodec::StartGameplayAckPayload payload = {};
            payload.config_hash = s_configHash;
            payload.baseline_frame = s_baselineFrame;
            payload.accepted_delay = s_negotiatedDelay;

            SessionManager::SendToPeer(PacketCodec::PacketType::StartGameplayAck,
                                       &payload, sizeof(payload), true);
            s_startGameplayAckSeq = 1;  // Mark as sent

            LOG_NET_INFO("[Bootstrap] Client sent StartGameplayAck (delay=%u)",
                     s_negotiatedDelay);

            MarkReadyAndStart();
        }
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void Begin(const CharSelSync::LockedMatchConfig* config,
           bool isHost,
           uint64_t sessionId, uint32_t connectionId,
           float initialRttMs) {
    s_active = true;
    s_isHost = isHost;
    s_sessionId = sessionId;
    s_connectionId = connectionId;
    
    s_config = *config;
    s_configHash = config->config_hash;
    
    s_loadState  = LoadState::Waiting;
    s_readyState = ReadyState::Idle;
    
    s_localLoaded = false;
    s_remoteLoaded = false;
    s_localAssetHash = 0;
    s_remoteAssetHash = 0;
    memset(&s_localLoadSnapshot, 0, sizeof(s_localLoadSnapshot));
    
    s_localBaselineReady = false;
    s_remoteBaselineReady = false;
    s_localBaselineChecksum = 0;
    s_remoteBaselineChecksum = 0;
    s_baselineFrame = 0;
    
    s_localReady = false;
    s_remoteReady = false;
    s_negotiatedDelay = 2;
    s_rttMs = (initialRttMs > 0.0f) ? initialRttMs : 0.0f;
    
    s_phaseStartMs = Now();
    s_startDeadlineMs = 0;
    s_loadBarrierSeq = 0;
    s_baselineReadySeq = 0;
    s_startGameplaySeq = 0;
    s_startGameplayAckSeq = 0;
    s_errorMsg[0] = 0;
    s_lastHeartbeatMs = 0;
    
    SetPhase(Phase::ApplyConfig);
    
    LOG_NET_INFO("[Bootstrap] BEGIN (isHost=%d config_hash=0x%08X session=0x%llX)",
             isHost, s_configHash, sessionId);
}

void Abort(const char* reason) {
    if (!s_active) return;
    LOG_NET_WARN("[Bootstrap] ABORT: %s", reason);
    s_active = false;
    s_phase = Phase::Idle;
    s_lastHeartbeatMs = 0;
}

bool IsActive() { return s_active; }
bool IsComplete() { return s_phase == Phase::Complete; }

// ============================================================================
// Frame Update
// ============================================================================

void FrameUpdate() {
    if (!s_active) return;
    
    uint32_t now = Now();
    uint32_t elapsed = now - s_phaseStartMs;
    LogHeartbeat(false, "frame");
    
    // Phase-specific logic
    switch (s_phase) {
        case Phase::ApplyConfig:
            ApplyConfigToGameMemory();
            SetPhase(Phase::Loading);
            s_loadState = LoadState::Waiting;
            break;
            
        case Phase::Loading:
            // Check for timeout
            if (elapsed > TIMEOUT_MS) {
                s_loadState = LoadState::Timeout;
                SetError("Load barrier timeout — peer did not load");
                return;
            }
            
            // Send our load ready if loaded
            if (s_localLoaded) {
                s_loadState = LoadState::Ready;
                SendLoadBarrierReady();
            }
            
            CheckLoadBarrier();
            break;
            
        case Phase::BaselineSync:
            if (elapsed > TIMEOUT_MS) {
                SetError("Baseline sync timeout — peer did not respond");
                return;
            }
            
            CaptureBaseline();
            
            if (s_localBaselineReady) {
                SendBaselineReady();
            }
            
            CheckBaselineAgreement();
            break;
            
        case Phase::ReadySync:
            if (elapsed > TIMEOUT_MS) {
                SetError("Ready sync timeout — peer did not respond");
                return;
            }
            
            DriveReadySync();
            break;
            
        case Phase::Starting:
            // Wait until the negotiated start time if we have one.
            if (s_startDeadlineMs != 0 && (int32_t)(s_startDeadlineMs - now) > 0) {
                break;
            }
            if (s_startDeadlineMs != 0 || elapsed > 100) {
                SetPhase(Phase::Complete);
                LOG_NET_INFO("[Bootstrap] COMPLETE — ready for gameplay (delay=%u)", s_negotiatedDelay);
            }
            break;
            
        case Phase::Complete:
        case Phase::Error:
        case Phase::Idle:
            break;
    }
}

// ============================================================================
// External Events
// ============================================================================

void NotifyLocalLoaded() {
    if (!s_active) return;

    // CharSel -> Match setup can consume pre-frame random numbers asymmetrically.
    // Re-apply the authoritative session seed at the exact local-loaded handoff so
    // the load barrier and baseline start from the agreed match seed.
    RngHooks::ResetForMatch(s_config.session_seed);

    // NOTE: AS2_ScrubTransientMatchState() is NO LONGER called here.
    // It has been moved to the Mode→Match entry point in netplay_core.cpp
    // (UpdateAutoHooks) where it runs BEFORE the game initializes entities
    // and sets up the intro freeze flag in substates 0-2.  Running the scrub
    // at load barrier time (after substate 2) was interfering with the
    // round-start intro animation by resetting state that the game had
    // already set up.  The effects/summons/FPU/input-pipeline reset is now
    // done early enough that the game's own init builds on a clean slate.

    s_localLoaded = true;
    CompactSaveState_t snapshot = {};
    s_localAssetHash = ComputeLoadStateHash(&snapshot);
    s_localLoadSnapshot = snapshot;  // keep for mismatch diagnostics
    s_loadState = LoadState::Ready;

    char summary[512] = {};
    const char* summaryText = AS2_FormatCompactStateSummary(&snapshot, summary, sizeof(summary))
        ? summary
        : "summary-unavailable";
    LOG_NET_INFO("[Bootstrap] Local assets loaded (hash=0x%08X seed=%u summary=%s)",
                 s_localAssetHash,
                 s_config.session_seed,
                 summaryText);

    // Per-section CRC breakdown for desync debugging
    LogSnapshotSectionCrcs(snapshot, "local-loaded");
    LogInputSnapshotCrcs(snapshot, "local-loaded");

    LogHeartbeat(true, "local-loaded");

    // Send LoadBarrierReady IMMEDIATELY — don't wait for the next Update() tick.
    // If we defer to Update(), the peer's LoadBarrierReady may arrive first via
    // packet dispatch (OnReceiveLoadBarrierReady → CheckLoadBarrier), advance
    // the phase to BaselineSync, and the Loading case in Update() never runs.
    // The peer would never receive our LoadBarrierReady and would time out.
    if (s_phase == Phase::Loading) {
        SendLoadBarrierReady();
    }
}

void SetObservedRttMs(float rttMs) {
    if (!s_active || rttMs <= 0.0f) return;
    s_rttMs = rttMs;
}

void OnReceiveLoadBarrierReady(const PacketCodec::LoadBarrierReadyPayload* payload) {
    LOG_NET_DEBUG("[Bootstrap] >>> OnReceiveLoadBarrierReady ENTRY (active=%d phase=%d)",
                 s_active ? 1 : 0, (int)s_phase);
    if (!s_active) return;
    
    // Phase guard: only process during Loading phase.
    // Retransmitted messages from earlier phases must not regress the state machine.
    if (s_phase != Phase::Loading) {
        return;
    }
    
    LOG_NET_INFO("[Bootstrap] Received LoadBarrierReady (config_hash=0x%08X asset_hash=0x%08X)",
             payload->config_hash, payload->local_asset_hash);
    
    if (payload->config_hash != s_configHash) {
        LOG_NET_ERROR("[Bootstrap] Config hash mismatch in LoadBarrierReady! "
                  "expected=0x%08X got=0x%08X", s_configHash, payload->config_hash);
        SetError("Config hash mismatch in load barrier");
        return;
    }
    
    s_remoteLoaded = true;
    s_remoteAssetHash = payload->local_asset_hash;
    LogHeartbeat(true, "remote-loaded");
    CheckLoadBarrier();
}

void OnReceiveBaselineReady(const PacketCodec::BaselineReadyPayload* payload) {
    LOG_NET_DEBUG("[Bootstrap] >>> OnReceiveBaselineReady ENTRY (active=%d phase=%d)",
                 s_active ? 1 : 0, (int)s_phase);
    if (!s_active) return;
    
    // Defense-in-depth: if we receive BaselineReady while still in Loading,
    // the peer has implicitly loaded (they couldn't reach baseline otherwise).
    // Treat this as an implicit LoadBarrierReady to recover from the race where
    // the peer's LoadBarrierReady was lost but their BaselineReady arrived.
    if (s_phase == Phase::Loading) {
        LOG_NET_INFO("[Bootstrap] Received BaselineReady while still Loading — "
                     "implicitly satisfying load barrier for remote");
        s_remoteLoaded = true;
        s_remoteAssetHash = s_localAssetHash;  // Assume match (baseline proves it)
        CheckLoadBarrier();
        // After CheckLoadBarrier, phase may have advanced to BaselineSync.
        // Fall through to process the baseline payload.
    }
    
    // Phase guard: only process during BaselineSync (or ReadySync if we need the remote value).
    // Once we've moved past baseline agreement, retransmits must be ignored.
    if (s_phase != Phase::BaselineSync && s_phase != Phase::ReadySync) {
        return;
    }
    // If already in ReadySync, baseline was already agreed — ignore duplicate.
    if (s_phase == Phase::ReadySync && s_remoteBaselineReady) {
        return;
    }
    
    LOG_NET_INFO("[Bootstrap] Received BaselineReady (frame=%u checksum=0x%08X)",
             payload->baseline_frame, payload->baseline_checksum);
    
    if (payload->config_hash != s_configHash) {
        LOG_NET_ERROR("[Bootstrap] Config hash mismatch in BaselineReady!");
        SetError("Config hash mismatch in baseline sync");
        return;
    }
    
    s_remoteBaselineReady = true;
    s_remoteBaselineChecksum = payload->baseline_checksum;
    LogHeartbeat(true, "remote-baseline");
    CheckBaselineAgreement();
}

void OnReceiveStartGameplay(const PacketCodec::StartGameplayPayload* payload,
                            uint32_t senderTimestampMs) {
    if (!s_active) return;
    
    // Phase guard: only process during ReadySync.
    // Once Starting/Complete, further StartGameplay retransmits are ignored.
    if (s_phase != Phase::ReadySync) {
        return;
    }
    
    LOG_NET_INFO("[Bootstrap] Received StartGameplay (delay_p1=%u delay_p2=%u baseline=%u)",
             payload->input_delay_p1, payload->input_delay_p2, payload->baseline_frame);
    
    if (payload->config_hash != s_configHash) {
        LOG_NET_ERROR("[Bootstrap] Config hash mismatch in StartGameplay!");
        SetError("Config hash mismatch in start gameplay");
        return;
    }

    if (payload->baseline_frame != s_baselineFrame) {
        LOG_NET_ERROR("[Bootstrap] Baseline frame mismatch in StartGameplay! expected=%u got=%u",
                  s_baselineFrame, payload->baseline_frame);
        SetError("Baseline frame mismatch in start gameplay");
        return;
    }

    if (s_isHost) {
        LOG_NET_WARN("[Bootstrap] Host received unexpected StartGameplay packet");
        return;
    }

    // Lock delay to max of both sides as per §16
    s_negotiatedDelay = (uint8_t)payload->input_delay_p1;
    if (payload->input_delay_p2 > s_negotiatedDelay) {
        s_negotiatedDelay = (uint8_t)payload->input_delay_p2;
    }

    uint32_t relativeDelayMs = 0;
    if (payload->start_epoch_ms > senderTimestampMs) {
        relativeDelayMs = payload->start_epoch_ms - senderTimestampMs;
    }
    s_startDeadlineMs = Now() + relativeDelayMs;

    s_remoteReady = true;
    s_readyState = ReadyState::Confirm;
    LogHeartbeat(true, "start-gameplay");
}

void OnReceiveStartGameplayAck(const PacketCodec::StartGameplayAckPayload* payload) {
    if (!s_active) return;
    if (s_phase != Phase::ReadySync || !s_isHost) return;

    LOG_NET_INFO("[Bootstrap] Received StartGameplayAck (delay=%u baseline=%u)",
             payload->accepted_delay, payload->baseline_frame);

    if (payload->config_hash != s_configHash) {
        LOG_NET_ERROR("[Bootstrap] Config hash mismatch in StartGameplayAck!");
        SetError("Config hash mismatch in start gameplay ack");
        return;
    }

    if (payload->baseline_frame != s_baselineFrame) {
        LOG_NET_ERROR("[Bootstrap] Baseline frame mismatch in StartGameplayAck! expected=%u got=%u",
                  s_baselineFrame, payload->baseline_frame);
        SetError("Baseline frame mismatch in start gameplay ack");
        return;
    }

    uint8_t agreedDelay = (uint8_t)payload->accepted_delay;
    if (agreedDelay > s_negotiatedDelay) {
        s_negotiatedDelay = agreedDelay;
    }

    SendStartGameplayEcho();
    s_remoteReady = true;
    LogHeartbeat(true, "start-ack");
}

// ============================================================================
// Queries
// ============================================================================

Phase GetPhase() { return s_phase; }

void GetSnapshot(Snapshot* out) {
    if (!out) return;
    out->phase      = s_phase;
    out->load_state = s_loadState;
    out->ready_state = s_readyState;
    out->local_loaded = s_localLoaded;
    out->remote_loaded = s_remoteLoaded;
    out->local_ready = s_localReady;
    out->remote_ready = s_remoteReady;
    out->local_baseline_ready = s_localBaselineReady;
    out->remote_baseline_ready = s_remoteBaselineReady;
    out->local_asset_hash = s_localAssetHash;
    out->remote_asset_hash = s_remoteAssetHash;
    out->local_baseline_checksum  = s_localBaselineChecksum;
    out->remote_baseline_checksum = s_remoteBaselineChecksum;
    out->baseline_frame = s_baselineFrame;
    out->config_hash = s_configHash;
    out->negotiated_delay = s_negotiatedDelay;
    out->rtt_ms = s_rttMs;
    out->phase_elapsed_ms = Now() - s_phaseStartMs;
    out->start_deadline_ms = s_startDeadlineMs;
    
    snprintf(out->status, sizeof(out->status), "%s (load=%d, base L=%d R=%d)",
             PhaseToString(s_phase), (int)s_loadState,
             s_localBaselineReady, s_remoteBaselineReady);
    snprintf(out->error, sizeof(out->error), "%s", s_errorMsg);
}

uint8_t GetNegotiatedDelay()  { return s_negotiatedDelay; }
uint32_t GetSessionSeed()     { return s_config.session_seed; }

} // namespace MatchBootstrap
