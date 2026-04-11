/**
 * Alice Senki 2 - Rollback Session Implementation (GekkoNet-driven)
 *
 * GekkoNet owns all rollback logic. This module provides:
 *   - ENet ↔ GekkoNet transport adapter
 *   - State capture/restore for save/load events
 *   - Input injection for advance events
 *   - GekkoNet session lifecycle
 *   - Packet buffering for GekkoNet's receive adapter
 */

#include "rollback/rollback_session.h"
#include "rollback/frame_lineage.h"
#include "rollback/resimulation.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "net/session_manager.h"
#include "net/match_lifecycle.h"
#include "net/delay_policy.h"
#include "net/protocol.h"
#include "net/player_side_mapping.h"
#include "input/input_system.h"
#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"

#include <gekkonet.h>
#include <gekko_types.h>

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <algorithm>
#include <math.h>
#include <windows.h>
#include <xmmintrin.h>  // _mm_getcsr / _mm_setcsr for FPU state capture

namespace Rollback {

// ============================================================================
// Serialized Game State (written into GekkoNet's state buffer)
// ============================================================================

#define GS_MAIN_START     ADDR_MATCH_BASE
#define GS_MAIN_SIZE      ((ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE)
#define GS_PRE_MATCH_START ADDR_PRE_MATCH_GAP
#define GS_PRE_MATCH_SIZE  PRE_MATCH_GAP_SIZE
#define GS_INPUT_P1_START  ADDR_P1_INPUT_BUFFER
#define GS_INPUT_P2_START  ADDR_P2_INPUT_BUFFER
#define GS_INPUT_SIZE      INPUT_BUFFER_SIZE

#pragma pack(push, 1)
struct GekkoState {
    // Explicit frame-domain metadata for rollback integration debugging.
    uint32_t rb_frame;             // Rollback-session-relative frame from Gekko
    uint32_t frame_origin_abs;     // Absolute engine frame where rb_frame 0 begins

    // Scattered globals (all outside main blob, verified against decompilation)
    uint32_t rng_seed;            // TLS _getptd()+0x14 — C runtime rand() state
    uint32_t sim_frame;           // 0x816490 — absolute engine simulation frame (= input_read_idx)
    uint32_t display_frame;       // 0x81635C — absolute engine display frame
    uint32_t game_mode;           // 0x81638C — current mode handler
    uint32_t substate;            // 0x816390 — sub-state within mode
    uint32_t substate_timer;      // 0x816394 — timer/counter for substates
    uint32_t game_type;           // 0x816410 — gameplay type (arcade/vs/netplay)
    uint32_t match_phase_timer;   // 0x816370 — intro fade countdown
    uint32_t input_write_idx;     // 0x816498 — input write index

    // FPU control state — critical for floating-point determinism
    uint16_t fpu_cw;              // x87 floating-point control word
    uint32_t mxcsr;               // SSE/MXCSR control/status register
    uint16_t _pad0;               // alignment padding

    // Main game state blob: 0x76C5F8..end of P2 entity
    // Contains: match header, effects, camera/scroll, weather particles,
    //           round timer, P1 entity, P2 entity, summons, hitboxes
    uint8_t  main_state[GS_MAIN_SIZE];

    // Pre-match gap: 12 bytes at 0x76C5EC (effect index, render state)
    uint8_t  pre_match_gap[GS_PRE_MATCH_SIZE];

    // Per-player input buffers: 208 bytes each (held, previous, just-pressed, etc.)
    uint8_t  input_p1[GS_INPUT_SIZE];  // 0x8E9E62
    uint8_t  input_p2[GS_INPUT_SIZE];  // 0x8E9F32
};
#pragma pack(pop)

// ============================================================================
// GekkoNet Packet Receive Buffer
// ============================================================================

static const int MAX_PENDING_RECV = 64;

struct BufferedPacket {
    void*  data;
    size_t len;
};

static BufferedPacket s_recvBuffer[MAX_PENDING_RECV];
static int            s_recvCount = 0;

// Pre-allocated GekkoNetResult array for adapter
static GekkoNetResult* s_recvResults[MAX_PENDING_RECV];
static int             s_recvResultCount = 0;

// Dummy peer address (we only have one peer)
static uint8_t s_peerAddrData[4] = { 1, 0, 0, 0 };

// ============================================================================
// GekkoNet Transport Adapter (ENet ↔ GekkoNet)
// ============================================================================

/// Called by GekkoNet to send data to the remote peer.
/// We wrap it in a GekkoData packet and send via ENet.
static void AdapterSendData(GekkoNetAddress* /*addr*/, const char* data, int length) {
    if (!Net::Session_IsConnected()) return;
    if (length <= 0 || !data) return;

    Net::Session_SendPacket(
        Net::CHANNEL_GAMEPLAY,
        Net::PacketType::GekkoData,
        data, (size_t)length,
        false  // Unreliable — GekkoNet handles its own reliability
    );
}

/// Called by GekkoNet to drain received packets.
/// Returns array of GekkoNetResult pointers. GekkoNet will free them via AdapterFreeData.
static GekkoNetResult** AdapterReceiveData(int* length) {
    *length = 0;

    if (s_recvCount == 0) return nullptr;

    // Convert buffered packets to GekkoNetResult array
    s_recvResultCount = 0;
    for (int i = 0; i < s_recvCount && s_recvResultCount < MAX_PENDING_RECV; i++) {
        GekkoNetResult* result = (GekkoNetResult*)malloc(sizeof(GekkoNetResult));
        if (!result) continue;

        result->addr.data = malloc(4);
        if (result->addr.data) {
            memcpy(result->addr.data, s_peerAddrData, 4);
        }
        result->addr.size = 4;
        result->data = s_recvBuffer[i].data;  // Transfer ownership
        result->data_len = (unsigned int)s_recvBuffer[i].len;

        s_recvResults[s_recvResultCount++] = result;
    }

    // Clear the receive buffer (ownership transferred to results)
    s_recvCount = 0;

    *length = s_recvResultCount;
    return s_recvResultCount > 0 ? s_recvResults : nullptr;
}

/// Called by GekkoNet to free memory we allocated.
static void AdapterFreeData(void* data_ptr) {
    free(data_ptr);
}

static void RefreshCachedNetworkTelemetry(bool includeFramesAhead);

// ============================================================================
// Internal State
// ============================================================================

static bool           s_initialized    = false;
static bool           s_active         = false;
static GekkoSession*  s_session        = nullptr;
static int            s_localPlayer    = 0;
static int            s_remotePlayer   = 1;
static int            s_localHandle    = -1;
static int            s_remoteHandle   = -1;
static int            s_activeDelay    = 0;
static int            s_rollbackBudget = 7;
static uint32_t       s_baselineChecksum = 0;
static int32_t        s_frameOriginAbs = 0;
static int32_t        s_currentRbFrame = 0;
static int32_t        s_lastSavedRbFrame = -1;
static bool           s_rollingBack    = false;
static bool           s_sessionRunning = false;  // true after GekkoSessionStarted event
static float          s_cachedFramesAhead = 0.0f;
static int32_t        s_cachedCurrentRbFrame = 0;
static int32_t        s_cachedConfirmedRbFrame = -1;
static int32_t        s_cachedLastRemoteReceivedRbFrame = -1;
static float          s_cachedAvgPing = 0.0f;
static float          s_cachedJitter = 0.0f;
static bool           s_loggedFirstSaveEvent = false;
static bool           s_loggedFirstLoadEvent = false;
static bool           s_loggedFirstAdvanceEvent = false;
static bool           s_loggedFirstNonZeroFramesAhead = false;
static bool           s_waitingForAdvance = false;
static int32_t        s_waitingAdvanceRbFrame = -1;
static uint32_t       s_waitingForAdvanceCount = 0;

static int32_t RbFrameToGameAbsFrame(int32_t rbFrame) {
    return FrameLineage_GameAbsFromRb(s_frameOriginAbs, rbFrame);
}

// Gekko's bootstrap checkpoint is saved at rb_frame=-1, but at the first
// interactive boundary the engine still reports the same absolute sim counter
// for that bootstrap state and for Advance(0). Do not shift the whole
// save/load domain by +1; only normalize the bootstrap checkpoint.
static int32_t RbCheckpointFrameToGameAbsFrame(int32_t rbFrame) {
    return FrameLineage_GameAbsFromCheckpoint(s_frameOriginAbs, rbFrame);
}

static int32_t CurrentGameAbsFrame() {
    return RbFrameToGameAbsFrame(s_currentRbFrame);
}

static void LogFrameDomainMismatch(const char* label,
                                   int32_t rbFrame,
                                   int32_t expectedGameAbsFrame,
                                   uint32_t capturedGameAbsFrame,
                                   uint32_t inputWriteIdx,
                                   uint32_t displayFrame) {
    NetplayLog_Write("GEKKO", rbFrame,
        "FRAME DOMAIN %s MISMATCH: rb_frame=%d expected_game_abs_frame=%d captured_game_abs_frame=%u "
        "origin_abs=%d mode=%u sub=%u/%u phase=%s rolling_back=%d write_idx=%u display_frame=%u",
        label ? label : "UNKNOWN",
        rbFrame,
        expectedGameAbsFrame,
        capturedGameAbsFrame,
        s_frameOriginAbs,
        ReadMemory<uint32_t>(ADDR_GAME_MODE),
        ReadMemory<uint32_t>(ADDR_SUB_STATE),
        ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER),
        Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
        s_rollingBack ? 1 : 0,
        inputWriteIdx,
        displayFrame);
}

static void RefreshCachedNetworkTelemetry(bool includeFramesAhead) {
    if (!s_session) {
        s_cachedFramesAhead = 0.0f;
        s_cachedCurrentRbFrame = s_currentRbFrame;
        s_cachedConfirmedRbFrame = -1;
        s_cachedLastRemoteReceivedRbFrame = -1;
        s_cachedAvgPing = 0.0f;
        s_cachedJitter = 0.0f;
        return;
    }

    if (includeFramesAhead) {
        s_cachedFramesAhead = gekko_frames_ahead(s_session);
    }
    s_cachedCurrentRbFrame = gekko_current_frame(s_session);
    s_cachedConfirmedRbFrame = gekko_min_received_frame(s_session);
    s_cachedLastRemoteReceivedRbFrame =
        (s_remoteHandle >= 0) ? gekko_last_received_frame(s_session, s_remoteHandle) : -1;

    if (s_remoteHandle >= 0) {
        GekkoNetworkStats stats{};
        gekko_network_stats(s_session, s_remoteHandle, &stats);
        s_cachedAvgPing = stats.avg_ping;
        s_cachedJitter = stats.jitter;
    } else {
        s_cachedAvgPing = 0.0f;
        s_cachedJitter = 0.0f;
    }
}

// Two-phase event processing state
static GekkoGameEvent** s_events       = nullptr;
static int              s_eventCount   = 0;
static int              s_eventIdx     = 0;
static bool             s_frameStarted = false;
static bool             s_sessionBroken = false;
static char             s_sessionError[128] = "";

// Current advance event inputs (valid after ProcessNextEvent returns Advance)
static uint16_t s_advP1 = 0;
static uint16_t s_advP2 = 0;

// Injected test input
static bool     s_hasInjectedInput = false;
static uint16_t s_injectedInput    = 0;

// Stats
static int32_t  s_totalRollbacks      = 0;
static int32_t  s_maxRollbackDepth    = 0;
static int32_t  s_lastRollbackFrame   = -1;
static int32_t  s_lastRollbackLength  = 0;
static int32_t  s_localInputsSent     = 0;
static int32_t  s_remoteInputsRecv    = 0;
static int32_t  s_saveEventCount      = 0;
static int32_t  s_loadEventCount      = 0;
static int32_t  s_advanceEventCount   = 0;
static bool     s_loggedInputBridge   = false;

// ============================================================================
// Match Handler — direct call during rollback resimulation
// ============================================================================

typedef unsigned short (__cdecl *MatchHandler_t)(uint32_t* a1);
static MatchHandler_t g_matchHandler = (MatchHandler_t)ADDR_MATCH_MODE;

// ============================================================================
// State Capture / Restore (into/from GekkoNet's buffer)
// ============================================================================

static bool CaptureState(GekkoState* state) {
    state->rb_frame          = (uint32_t)s_currentRbFrame;
    state->frame_origin_abs  = (uint32_t)s_frameOriginAbs;

    // Scattered globals
    state->rng_seed          = DetVer_GetRngSeed();
    state->sim_frame         = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    state->display_frame     = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    state->game_mode         = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    state->substate          = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    state->substate_timer    = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    state->game_type         = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    state->match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
    state->input_write_idx   = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);

    // FPU state — capture x87 control word and MXCSR for float determinism
    state->fpu_cw = 0;
    state->mxcsr  = 0;
    __try {
        unsigned short cw;
        __asm { fnstcw cw }
        state->fpu_cw = cw;
        state->mxcsr = _mm_getcsr();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // FPU capture failed — non-fatal, zeros are safe defaults
    }

    // Main blob: match state + entities + effects + camera + weather
    __try {
        memcpy(state->main_state, (const void*)GS_MAIN_START, GS_MAIN_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[RollbackSession] Capture AV at main region 0x%08X", GS_MAIN_START);
        return false;
    }

    // Pre-match gap: effect index + render state
    __try {
        memcpy(state->pre_match_gap, (const void*)GS_PRE_MATCH_START, GS_PRE_MATCH_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(state->pre_match_gap, 0, GS_PRE_MATCH_SIZE);
    }

    // Input buffers: per-player held/previous/just-pressed state
    __try {
        memcpy(state->input_p1, (const void*)GS_INPUT_P1_START, GS_INPUT_SIZE);
        memcpy(state->input_p2, (const void*)GS_INPUT_P2_START, GS_INPUT_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    return true;
}

static bool RestoreState(const GekkoState* state) {
    // Main blob
    __try {
        memcpy((void*)GS_MAIN_START, state->main_state, GS_MAIN_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[RollbackSession] Restore AV at main region 0x%08X", GS_MAIN_START);
        return false;
    }

    // Pre-match gap
    __try {
        memcpy((void*)GS_PRE_MATCH_START, state->pre_match_gap, GS_PRE_MATCH_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    // Input buffers
    __try {
        memcpy((void*)GS_INPUT_P1_START, state->input_p1, GS_INPUT_SIZE);
        memcpy((void*)GS_INPUT_P2_START, state->input_p2, GS_INPUT_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    // Scattered globals
    DetVer_SetRngSeed(state->rng_seed);
    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, state->sim_frame);
    WriteMemory<uint32_t>(ADDR_FRAME_COUNTER, state->display_frame);
    WriteMemory<uint32_t>(ADDR_GAME_MODE, state->game_mode);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, state->substate);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, state->substate_timer);
    WriteMemory<uint32_t>(ADDR_GAME_TYPE, state->game_type);
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, state->match_phase_timer);
    // Note: ADDR_INPUT_READ_IDX == ADDR_SIM_FRAME_COUNTER (0x816490).
    // Already restored via sim_frame above. Only write_idx is separate.
    WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, state->input_write_idx);

    // Restore FPU control state
    __try {
        if (state->fpu_cw != 0) {
            unsigned short cw = state->fpu_cw;
            __asm { fldcw cw }
        }
        if (state->mxcsr != 0) {
            _mm_setcsr(state->mxcsr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // FPU restore failed — non-fatal
    }

    // Clear per-frame temp scratch (sub_4C3BE0 equivalent)
    __try {
        memset((void*)ADDR_MATCH_PER_FRAME_TEMP, 0, MATCH_PER_FRAME_TEMP_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    return true;
}

// ============================================================================
// GekkoNet Event Handlers
// ============================================================================

static void HandleSaveEvent(GekkoGameEvent* ev) {
    const int32_t rbFrame = ev->data.save.frame;
    const int32_t gameAbsFrame = RbCheckpointFrameToGameAbsFrame(rbFrame);
    GekkoState* state = (GekkoState*)ev->data.save.state;

    if (!CaptureState(state)) {
        LOG_ERROR("[RollbackSession] Save event FAILED at rb_frame %d", rbFrame);
        return;
    }
    state->rb_frame = (uint32_t)rbFrame;
    state->frame_origin_abs = (uint32_t)s_frameOriginAbs;

#ifndef NDEBUG
    assert(rbFrame >= -1);
    assert(gameAbsFrame >= s_frameOriginAbs);
#endif
    if ((int32_t)state->frame_origin_abs != s_frameOriginAbs) {
        NetplayLog_Write("GEKKO", rbFrame,
            "WARN: Save metadata origin mismatch: rb_frame=%d state_origin_abs=%u local_origin_abs=%d phase=%s",
            rbFrame,
            state->frame_origin_abs,
            s_frameOriginAbs,
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
    }
    if ((int32_t)state->sim_frame != gameAbsFrame) {
        LogFrameDomainMismatch(
            "SAVE",
            rbFrame,
            gameAbsFrame,
            state->sim_frame,
            state->input_write_idx,
            state->display_frame);
    }

    *ev->data.save.state_len = sizeof(GekkoState);

    // Compute checksums for desync detection — full state for GekkoNet,
    // and per-section hashes for diagnostic logging
    uint32_t fullChecksum = CalcCRC32(state->main_state, GS_MAIN_SIZE);
    *ev->data.save.checksum = fullChecksum;

    s_lastSavedRbFrame = rbFrame;
    s_saveEventCount++;

    // Per-section hashes for debugging desyncs
    uint32_t inputHash = CalcCRC32(state->input_p1, GS_INPUT_SIZE)
                       ^ CalcCRC32(state->input_p2, GS_INPUT_SIZE);
    NetplayLog_Write("GEKKO", rbFrame,
        "SAVE: rb_frame=%d game_abs_frame=%d origin_abs=%d full=0x%08X inp=0x%08X rng=0x%08X mode=%u sub=%u/%u fpu=0x%04X/%08X",
        rbFrame, gameAbsFrame, s_frameOriginAbs, fullChecksum, inputHash, state->rng_seed,
        state->game_mode, state->substate, state->substate_timer,
        state->fpu_cw, state->mxcsr);
    if (!s_loggedFirstSaveEvent) {
        s_loggedFirstSaveEvent = true;
        NetplayLog_Write("GEKKO", rbFrame,
            "FIRST SAVE EVENT: rb_frame=%d game_abs_frame=%d engine_sim_after_capture=%u origin_abs=%d",
            rbFrame,
            gameAbsFrame,
            state->sim_frame,
            s_frameOriginAbs);
    }
}

static void HandleLoadEvent(GekkoGameEvent* ev) {
    const int32_t rbFrame = ev->data.load.frame;
    const int32_t expectedGameAbsFrame = RbCheckpointFrameToGameAbsFrame(rbFrame);
    const GekkoState* state = (const GekkoState*)ev->data.load.state;

    if (!RestoreState(state)) {
        LOG_ERROR("[RollbackSession] Load event FAILED at rb_frame %d", rbFrame);
        return;
    }

    // Gekko load events are rb_frame-domain, but the restored engine state is the
    // post-frame checkpoint for that rb_frame. Keep the engine counter absolute.
#ifndef NDEBUG
    assert(rbFrame >= -1);
    assert(expectedGameAbsFrame >= s_frameOriginAbs);
#endif
    if ((int32_t)state->frame_origin_abs != s_frameOriginAbs) {
        NetplayLog_Write("GEKKO", rbFrame,
            "WARN: Load metadata origin mismatch: rb_frame=%d state_origin_abs=%u local_origin_abs=%d",
            rbFrame,
            state->frame_origin_abs,
            s_frameOriginAbs);
    }
    if ((int32_t)state->sim_frame != expectedGameAbsFrame) {
        LogFrameDomainMismatch(
            "LOAD",
            rbFrame,
            expectedGameAbsFrame,
            state->sim_frame,
            state->input_write_idx,
            state->display_frame);
    }
    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, (uint32_t)expectedGameAbsFrame);

    s_loadEventCount++;

    uint32_t mainHash = CalcCRC32(state->main_state, GS_MAIN_SIZE);
    uint32_t inputHash = CalcCRC32(state->input_p1, GS_INPUT_SIZE)
                       ^ CalcCRC32(state->input_p2, GS_INPUT_SIZE);
    NetplayLog_Write("GEKKO", rbFrame,
        "LOAD: rb_frame=%d game_abs_frame=%d origin_abs=%d full=0x%08X inp=0x%08X rng=0x%08X mode=%u sub=%u/%u fpu=0x%04X/%08X",
        rbFrame, expectedGameAbsFrame, s_frameOriginAbs, mainHash, inputHash, state->rng_seed,
        state->game_mode, state->substate, state->substate_timer,
        state->fpu_cw, state->mxcsr);
    if (!s_loggedFirstLoadEvent) {
        s_loggedFirstLoadEvent = true;
        NetplayLog_Write("GEKKO", rbFrame,
            "FIRST LOAD EVENT: rb_frame=%d game_abs_frame=%d engine_sim_after_write=%u origin_abs=%d",
            rbFrame,
            expectedGameAbsFrame,
            ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER),
            s_frameOriginAbs);
    }
}

// Rollback sequence tracking (shared across calls)
static int32_t s_rbSequenceStart = -1;

static void HandleAdvanceEvent(GekkoGameEvent* ev) {
    const int32_t rbFrame = ev->data.adv.frame;
    const int32_t gameAbsFrame = RbFrameToGameAbsFrame(rbFrame);
    bool rolling_back = ev->data.adv.rolling_back;
    const uint8_t* inputs = ev->data.adv.inputs;
    unsigned int input_len = ev->data.adv.input_len;

    s_currentRbFrame = rbFrame;
    s_rollingBack = rolling_back;

    // GekkoNet provides inputs as [P1_input (2 bytes) | P2_input (2 bytes)]
    // ordered by player handle. input_size=2 per player, num_players=2.
    uint16_t raw_p1 = 0, raw_p2 = 0;
    if (inputs && input_len >= 4) {
        memcpy(&raw_p1, inputs, 2);
        memcpy(&raw_p2, inputs + 2, 2);
    } else {
        NetplayLog_Write("GEKKO", rbFrame,
            "WARN: Advance has insufficient input data: rb_frame=%d game_abs_frame=%d len=%u rolling_back=%d",
            rbFrame, gameAbsFrame, input_len, rolling_back);
    }

    // Gekko advance events are rb_frame-domain; engine memory must stay absolute.
    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, (uint32_t)gameAbsFrame);

    // Store for GetAdvanceInputs (read by input_override.cpp for normal frames)
    s_advP1 = raw_p1;
    s_advP2 = raw_p2;

    // Game-slot mapping diagnostics:
    // - raw_p1/raw_p2 always represent game P1/P2 inputs.
    // - local/remote attribution depends on local side assignment.
    const uint16_t localByGameSlot = (s_localPlayer == 0) ? raw_p1 : raw_p2;
    const uint16_t remoteByGameSlot = (s_localPlayer == 0) ? raw_p2 : raw_p1;
    if (!s_loggedInputBridge) {
        s_loggedInputBridge = true;
        NetplayLog_Write("GEKKO", rbFrame,
            "SDL->Rollback bridge ACTIVE: local_slot=P%d remote_slot=P%d "
            "local_input_sourced_from=PlayerMapping_ReadLocalInput "
            "remote_input_sourced_from=GekkoData stream",
            s_localPlayer + 1,
            s_remotePlayer + 1);
    }
    NetplayLog_Verbose("GEKKO", rbFrame,
        "Advance map: rb_frame=%d game_abs_frame=%d game[P1=0x%04X P2=0x%04X] local[P%d=0x%04X] remote[P%d=0x%04X] rolling_back=%d",
        rbFrame,
        gameAbsFrame,
        raw_p1,
        raw_p2,
        s_localPlayer + 1,
        localByGameSlot,
        s_remotePlayer + 1,
        remoteByGameSlot,
        rolling_back ? 1 : 0);

    // Write inputs to game buffers via InputSystem
    InputSystem_SetNetplayInput(0, raw_p1);
    InputSystem_SetNetplayInput(1, raw_p2);
    InputSystem_WriteToGameBuffersBothPlayers();

    s_advanceEventCount++;
    if (!s_loggedFirstAdvanceEvent) {
        s_loggedFirstAdvanceEvent = true;
        NetplayLog_Write("GEKKO", rbFrame,
            "FIRST ADVANCE EVENT: rb_frame=%d game_abs_frame=%d engine_sim_after_write=%u origin_abs=%d rolling_back=%d",
            rbFrame,
            gameAbsFrame,
            ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER),
            s_frameOriginAbs,
            rolling_back ? 1 : 0);
    }
    if (s_advanceEventCount <= 10) {
        NetplayLog_Write("GEKKO", rbFrame,
            "ADVANCE DETAIL #%d: rb_frame=%d game_abs_frame=%d confirmed_rb=%d remote_rb=%d rolling_back=%d",
            s_advanceEventCount,
            rbFrame,
            gameAbsFrame,
            s_cachedConfirmedRbFrame,
            s_cachedLastRemoteReceivedRbFrame,
            rolling_back ? 1 : 0);
    }

    if (rolling_back) {
        // Track rollback sequence start
        if (s_rbSequenceStart < 0) {
            s_rbSequenceStart = rbFrame;
            s_totalRollbacks++;
            NetplayLog_Write("GEKKO", rbFrame,
                "ROLLBACK START: rb_frame=%d game_abs_frame=%d rng=0x%08X mode=%u sub=%u",
                rbFrame, gameAbsFrame, DetVer_GetRngSeed(),
                ReadMemory<uint32_t>(ADDR_GAME_MODE),
                ReadMemory<uint32_t>(ADDR_SUB_STATE));
        }
        s_lastRollbackFrame = s_rbSequenceStart;

        // Rollback frames: inputs are set above via InputSystem_SetNetplayInput.
        // The game's own loop (called by the dispatcher returning 0) will:
        //   1. Call Hook_InputProcess which reads our netplay inputs
        //   2. Call g_matchHandler to simulate the frame
        //   3. Handle all per-frame bookkeeping
        // We do NOT call g_matchHandler directly — that skips critical game logic.

        NetplayLog_Write("GEKKO", rbFrame,
            "ADVANCE(rollback): rb_frame=%d game_abs_frame=%d P1=0x%04X P2=0x%04X rng=0x%08X",
            rbFrame, gameAbsFrame, raw_p1, raw_p2, DetVer_GetRngSeed());
    } else {
        // Normal advance — will be processed by game's own match handler
        // after the input dispatcher returns 0.
        if (s_rbSequenceStart >= 0) {
            // End of rollback sequence
            int depth = rbFrame - s_rbSequenceStart;
            s_lastRollbackLength = depth;
            if (depth > s_maxRollbackDepth) s_maxRollbackDepth = depth;
            NetplayLog_Write("GEKKO", rbFrame,
                "ROLLBACK COMPLETE: start_rb=%d end_rb=%d game_abs_frame=%d depth=%d total=%d max=%d rng=0x%08X",
                s_rbSequenceStart,
                rbFrame,
                gameAbsFrame,
                depth,
                s_totalRollbacks,
                s_maxRollbackDepth,
                DetVer_GetRngSeed());
            s_rbSequenceStart = -1;
        }

        NetplayLog_Write("GEKKO", rbFrame,
            "ADVANCE(normal): rb_frame=%d game_abs_frame=%d P1=0x%04X P2=0x%04X rng=0x%08X writeIdx=%u",
            rbFrame, gameAbsFrame, raw_p1, raw_p2, DetVer_GetRngSeed(),
            ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX));
    }
}

static void HandleSessionEvents() {
    if (!s_session) return;

    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(s_session, &count);

    for (int i = 0; i < count; i++) {
        GekkoSessionEvent* ev = events[i];
        switch (ev->type) {
            case GekkoPlayerSyncing:
                NetplayLog_Write("GEKKO", -1,
                    "SESSION: Player %d syncing (%u/%u)",
                    ev->data.syncing.handle,
                    ev->data.syncing.current,
                    ev->data.syncing.max);
                break;

            case GekkoPlayerConnected:
                NetplayLog_Write("GEKKO", -1,
                    "SESSION: Player %d connected",
                    ev->data.connected.handle);
                LOG_INFO("[RollbackSession] GekkoNet: Player %d connected",
                    ev->data.connected.handle);
                break;

            case GekkoPlayerDisconnected:
                NetplayLog_Write("GEKKO", -1,
                    "SESSION: Player %d disconnected",
                    ev->data.disconnected.handle);
                LOG_WARN("[RollbackSession] GekkoNet: Player %d disconnected",
                    ev->data.disconnected.handle);
                if (!s_sessionBroken) {
                    s_sessionBroken = true;
                    _snprintf_s(s_sessionError, sizeof(s_sessionError), _TRUNCATE,
                        "Rollback peer disconnected (handle=%d)",
                        ev->data.disconnected.handle);
                }
                break;

            case GekkoSessionStarted:
                s_sessionRunning = true;
                NetplayLog_Write("GEKKO", -1, "SESSION: Started");
                LOG_INFO("[RollbackSession] GekkoNet session started");
                break;

            case GekkoDesyncDetected:
                NetplayLog_Write("GEKKO", ev->data.desynced.frame,
                    "DESYNC DETECTED: frame=%d local_crc=0x%08X remote_crc=0x%08X remote_handle=%d",
                    ev->data.desynced.frame,
                    ev->data.desynced.local_checksum,
                    ev->data.desynced.remote_checksum,
                    ev->data.desynced.remote_handle);
                LOG_ERROR("[RollbackSession] DESYNC at frame %d! local=0x%08X remote=0x%08X",
                    ev->data.desynced.frame,
                    ev->data.desynced.local_checksum,
                    ev->data.desynced.remote_checksum);
                break;

            default:
                break;
        }
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void RollbackSession_Init() {
    // State history still needed for diagnostics / state integrity checks
    StateHistory_Init();

    s_active = false;
    s_session = nullptr;
    s_initialized = true;
    LOG_INFO("[RollbackSession] Initialized (GekkoNet mode, local StateHistory is diagnostic-only)");
}

void RollbackSession_Shutdown() {
    if (s_active) {
        RollbackSession_End();
    }

    StateHistory_Shutdown();
    s_initialized = false;
    LOG_INFO("[RollbackSession] Shutdown");
}

bool RollbackSession_Begin(const RollbackSessionConfig& config) {
    if (!s_initialized) {
        LOG_ERROR("[RollbackSession] Not initialized");
        return false;
    }

    if (s_active) {
        LOG_WARN("[RollbackSession] Already active — ending previous session");
        RollbackSession_End();
    }

    // Create GekkoNet session
    if (!gekko_create(&s_session, GekkoGameSession)) {
        LOG_ERROR("[RollbackSession] gekko_create failed");
        return false;
    }

    // Configure GekkoNet
    GekkoConfig gkConfig{};
    gkConfig.num_players = 2;
    gkConfig.max_spectators = 0;
    gkConfig.input_size = sizeof(uint16_t);  // 2 bytes per player
    gkConfig.state_size = sizeof(GekkoState);
    gkConfig.input_prediction_window = (unsigned char)(std::min)(config.rollback_budget, 255);
    gkConfig.limited_saving = false;
    gkConfig.desync_detection = true;
    gkConfig.check_distance = 60;  // Check every 60 frames

#ifndef NDEBUG
    assert((int)gkConfig.input_prediction_window == config.rollback_budget);
    assert(config.frame_origin_abs >= 0);
#endif

    gekko_start(s_session, &gkConfig);

    // Set transport adapter (must be static — GekkoNet holds the pointer)
    static GekkoNetAdapter s_adapter{};
    s_adapter.send_data = AdapterSendData;
    s_adapter.receive_data = AdapterReceiveData;
    s_adapter.free_data = AdapterFreeData;
    gekko_net_adapter_set(s_session, &s_adapter);

    // Add players
    GekkoNetAddress localAddr{};
    localAddr.data = nullptr;
    localAddr.size = 0;

    GekkoNetAddress remoteAddr{};
    remoteAddr.data = s_peerAddrData;
    remoteAddr.size = sizeof(s_peerAddrData);

    // GekkoNet player handles: add local player first, then remote
    // The order determines input layout in advance events
    if (config.local_player == 0) {
        // Local is P1, Remote is P2
        s_localHandle = gekko_add_actor(s_session, GekkoLocalPlayer, &localAddr);
        s_remoteHandle = gekko_add_actor(s_session, GekkoRemotePlayer, &remoteAddr);
    } else {
        // Remote is P1 (added first), Local is P2
        s_remoteHandle = gekko_add_actor(s_session, GekkoRemotePlayer, &remoteAddr);
        s_localHandle = gekko_add_actor(s_session, GekkoLocalPlayer, &localAddr);
    }

    if (s_localHandle < 0 || s_remoteHandle < 0) {
        LOG_ERROR("[RollbackSession] gekko_add_actor failed: local=%d remote=%d",
            s_localHandle, s_remoteHandle);
        gekko_destroy(&s_session);
        return false;
    }

    // Set input delay for the LOCAL handle only.
#ifndef NDEBUG
    assert(s_localHandle >= 0);
    assert(s_localHandle != s_remoteHandle);
#endif
    gekko_set_local_delay(s_session, s_localHandle, (unsigned char)config.initial_delay);

    // Store config
    s_localPlayer      = config.local_player;
    s_remotePlayer     = config.remote_player;
    s_activeDelay      = config.initial_delay;
    s_rollbackBudget   = config.rollback_budget;
    s_baselineChecksum = config.baseline_checksum;
    s_frameOriginAbs   = config.frame_origin_abs;
    s_currentRbFrame   = 0;
    s_lastSavedRbFrame = -1;
    s_rollingBack      = false;
    s_sessionRunning   = false;
    s_hasInjectedInput = false;
    s_cachedFramesAhead = 0.0f;
    s_cachedCurrentRbFrame = 0;
    s_cachedConfirmedRbFrame = -1;
    s_cachedLastRemoteReceivedRbFrame = -1;
    s_cachedAvgPing = 0.0f;
    s_cachedJitter = 0.0f;
    s_loggedFirstSaveEvent = false;
    s_loggedFirstLoadEvent = false;
    s_loggedFirstAdvanceEvent = false;
    s_loggedFirstNonZeroFramesAhead = false;
    s_waitingForAdvance = false;
    s_waitingAdvanceRbFrame = -1;
    s_waitingForAdvanceCount = 0;

    // Reset event processing state
    s_events       = nullptr;
    s_eventCount   = 0;
    s_eventIdx     = 0;
    s_frameStarted = false;
    s_sessionBroken = false;
    s_sessionError[0] = '\0';

    // Reset stats
    s_totalRollbacks    = 0;
    s_maxRollbackDepth  = 0;
    s_lastRollbackFrame = -1;
    s_lastRollbackLength = 0;
    s_localInputsSent   = 0;
    s_remoteInputsRecv  = 0;
    s_saveEventCount    = 0;
    s_loadEventCount    = 0;
    s_advanceEventCount = 0;
    s_loggedInputBridge = false;
    s_rbSequenceStart   = -1;

    StateHistory_Reset();

    // Clear receive buffer
    for (int i = 0; i < s_recvCount; i++) {
        free(s_recvBuffer[i].data);
    }
    s_recvCount = 0;
    s_recvResultCount = 0;

    s_active = true;

    // Suppress pause during rollback gameplay
    InputSystem_SetPauseBlocked(true);

    const char* localRole = (s_localPlayer == 0) ? "P1" : "P2";
    const int visibleDelay = Net::DelayPolicy_GetActiveDelay();
    const int effectiveDelay = Net::DelayPolicy_GetEffectiveLocalDelay();
    const int protectionWindow = Net::DelayPolicy_GetProtectionWindow();
    LOG_INFO("[RollbackSession] BEGIN: local=%s(h%d) visible_delay=%u effective_delay=%u max_rollback=%u protection_window=%u stall_threshold=%u frame_origin_abs=%d rb_start=0 state_size=%u",
        localRole,
        s_localHandle,
        (unsigned)visibleDelay,
        (unsigned)effectiveDelay,
        (unsigned)config.rollback_budget,
        (unsigned)protectionWindow,
        (unsigned)Net::DelayPolicy_GetStallThreshold(),
        s_frameOriginAbs,
        (unsigned)sizeof(GekkoState));

    NetplayLog_Write("GEKKO", 0,
        "BEGIN: rb_start=0 frame_origin_abs=%d local=P%d(h%d) remote=P%d(h%d) visible_delay=%d effective_delay=%d window=%d stall_threshold=%d state_kb=%zu baseline=0x%08X",
        s_frameOriginAbs,
        s_localPlayer + 1, s_localHandle,
        s_remotePlayer + 1, s_remoteHandle,
        visibleDelay,
        effectiveDelay,
        protectionWindow,
        Net::DelayPolicy_GetStallThreshold(),
        sizeof(GekkoState) / 1024, config.baseline_checksum);

    NetplayLog_Write("GEKKO", 0,
        "Timesync metric: framesAhead>0 means local ahead, framesAhead<0 means local behind");
    NetplayLog_Write("GEKKO", 0,
        "Frame domains: rb_frame starts at 0, game_abs_frame = frame_origin_abs + rb_frame");

    return true;
}

void RollbackSession_End() {
    if (!s_active) return;

    const int32_t finalRbFrame = s_currentRbFrame;
    const int32_t finalGameAbsFrame = CurrentGameAbsFrame();

    NetplayLog_Write("GEKKO", s_currentRbFrame,
        "END: rb_frame=%d game_abs_frame=%d origin_abs=%d saves=%d loads=%d advances=%d rollbacks=%d max_depth=%d sent=%d recv=%d",
        s_currentRbFrame,
        CurrentGameAbsFrame(),
        s_frameOriginAbs,
        s_saveEventCount, s_loadEventCount, s_advanceEventCount,
        s_totalRollbacks, s_maxRollbackDepth, s_localInputsSent, s_remoteInputsRecv);

    // Destroy GekkoNet session
    if (s_session) {
        gekko_destroy(&s_session);
        s_session = nullptr;
    }

    // Clear receive buffer
    for (int i = 0; i < s_recvCount; i++) {
        free(s_recvBuffer[i].data);
    }
    s_recvCount = 0;
    s_recvResultCount = 0;

    // Clear state
    s_active = false;
    s_rollingBack = false;
    s_sessionRunning = false;
    s_frameStarted = false;
    s_events = nullptr;
    s_eventCount = 0;
    s_eventIdx = 0;
    s_hasInjectedInput = false;
    s_sessionBroken = false;
    s_sessionError[0] = '\0';
    s_cachedFramesAhead = 0.0f;
    s_cachedCurrentRbFrame = 0;
    s_cachedConfirmedRbFrame = -1;
    s_cachedLastRemoteReceivedRbFrame = -1;
    s_cachedAvgPing = 0.0f;
    s_cachedJitter = 0.0f;
    s_frameOriginAbs = 0;
    s_currentRbFrame = 0;
    s_lastSavedRbFrame = -1;
    s_rbSequenceStart = -1;
    s_loggedFirstSaveEvent = false;
    s_loggedFirstLoadEvent = false;
    s_loggedFirstAdvanceEvent = false;
    s_loggedFirstNonZeroFramesAhead = false;
    s_waitingForAdvance = false;
    s_waitingAdvanceRbFrame = -1;
    s_waitingForAdvanceCount = 0;

    StateHistory_Reset();

    // Clear netplay input overrides
    InputSystem_ClearNetplayInput(0);
    InputSystem_ClearNetplayInput(1);
    InputSystem_SetPauseBlocked(false);

    LOG_INFO("[RollbackSession] END: rb_frame=%d game_abs_frame=%d rollbacks=%d max_depth=%d",
        finalRbFrame, finalGameAbsFrame, s_totalRollbacks, s_maxRollbackDepth);
}

bool RollbackSession_IsActive() {
    return s_active;
}

// ============================================================================
// Two-Phase Frame Processing
// ============================================================================

void RollbackSession_BeginFrame(uint16_t localInput) {
    if (!s_active || !s_session) return;

    // Handle injected test input
    if (s_hasInjectedInput) {
        localInput = s_injectedInput;
        s_hasInjectedInput = false;
    }

    // Step 1: Poll network/session state before feeding new local input.
    if (!RollbackSession_PollSession()) {
        s_events = nullptr;
        s_eventCount = 0;
        s_eventIdx = 0;
        s_frameStarted = false;
        return;
    }

    const int32_t currentRbBeforeUpdate = gekko_current_frame(s_session);
    const bool continuingWaitingFrame =
        s_waitingForAdvance && currentRbBeforeUpdate == s_waitingAdvanceRbFrame;

    // Step 2: Feed local input to GekkoNet
#ifndef NDEBUG
    assert(!s_frameStarted);
#endif
    if (!continuingWaitingFrame) {
        gekko_add_local_input(s_session, s_localHandle, &localInput);
        s_localInputsSent++;
    } else if (s_waitingForAdvanceCount <= 5 || (s_waitingForAdvanceCount % 120) == 0) {
        NetplayLog_Write("GEKKO", currentRbBeforeUpdate,
            "BeginFrame retry without re-submitting local input: rb_frame=%d game_abs_frame=%d "
            "remote_rb=%d confirmed_rb=%d phase=%s",
            currentRbBeforeUpdate,
            RbFrameToGameAbsFrame(currentRbBeforeUpdate),
            s_cachedLastRemoteReceivedRbFrame,
            s_cachedConfirmedRbFrame,
            Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
    }

    // Step 3: Update GekkoNet session — this produces game events
    s_events = gekko_update_session(s_session, &s_eventCount);
    RefreshCachedNetworkTelemetry(true);
    s_eventIdx = 0;
    s_frameStarted = true;

    if (s_eventCount == 0 && s_cachedCurrentRbFrame == currentRbBeforeUpdate) {
        s_waitingForAdvance = true;
        s_waitingAdvanceRbFrame = currentRbBeforeUpdate;
        s_waitingForAdvanceCount++;

        if (s_waitingForAdvanceCount <= 5 || (s_waitingForAdvanceCount % 120) == 0) {
            NetplayLog_Write("GEKKO", currentRbBeforeUpdate,
                "No gameplay events produced: rb_frame=%d game_abs_frame=%d remote_rb=%d confirmed_rb=%d "
                "frames_ahead=%.2f phase=%s wait_count=%u",
                currentRbBeforeUpdate,
                RbFrameToGameAbsFrame(currentRbBeforeUpdate),
                s_cachedLastRemoteReceivedRbFrame,
                s_cachedConfirmedRbFrame,
                s_cachedFramesAhead,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()),
                s_waitingForAdvanceCount);
        }
    } else if (s_waitingForAdvance) {
        NetplayLog_Write("GEKKO", s_cachedCurrentRbFrame,
            "Recovered from no-event wait: previous_rb_frame=%d current_rb_frame=%d current_game_abs_frame=%d "
            "remote_rb=%d confirmed_rb=%d wait_count=%u",
            s_waitingAdvanceRbFrame,
            s_cachedCurrentRbFrame,
            RbFrameToGameAbsFrame(s_cachedCurrentRbFrame),
            s_cachedLastRemoteReceivedRbFrame,
            s_cachedConfirmedRbFrame,
            s_waitingForAdvanceCount);
        s_waitingForAdvance = false;
        s_waitingAdvanceRbFrame = -1;
        s_waitingForAdvanceCount = 0;
    }

    if (!s_loggedFirstNonZeroFramesAhead &&
        fabsf(s_cachedFramesAhead) > 0.01f) {
        s_loggedFirstNonZeroFramesAhead = true;
        NetplayLog_Write("TIMESYNC", s_cachedCurrentRbFrame,
            "FIRST NONZERO FRAMES_AHEAD: rb_frame=%d game_abs_frame=%d frames_ahead=%.2f origin_abs=%d",
            s_cachedCurrentRbFrame,
            RbFrameToGameAbsFrame(s_cachedCurrentRbFrame),
            s_cachedFramesAhead,
            s_frameOriginAbs);
    }

    NetplayLog_Write("GEKKO", s_cachedCurrentRbFrame,
        "BeginFrame: rb_frame=%d game_abs_frame=%d input=0x%04X events=%d recvBuf=%d",
        s_cachedCurrentRbFrame,
        RbFrameToGameAbsFrame(s_cachedCurrentRbFrame),
        localInput,
        s_eventCount,
        s_recvCount);
}

bool RollbackSession_PollSession() {
    if (!s_active || !s_session) return true;

    gekko_network_poll(s_session);
    RefreshCachedNetworkTelemetry(false);
    HandleSessionEvents();
    return !s_sessionBroken;
}

EventResult RollbackSession_ProcessNextEvent() {
    if (s_sessionBroken) {
        s_frameStarted = false;
        s_events = nullptr;
        s_eventCount = 0;
        s_eventIdx = 0;
        s_waitingForAdvance = false;
        s_waitingAdvanceRbFrame = -1;
        s_waitingForAdvanceCount = 0;
        return EventResult::Error;
    }

    if (!s_active || !s_session || !s_frameStarted) {
        return EventResult::Done;
    }

    // Process events one at a time. For every advance event (normal OR rollback),
    // return Advance so the game's own loop runs one full simulation tick.
    // This matches the old working architecture: the dispatcher returns 0 for
    // each advance, and the game calls Hook_InputProcess → g_matchHandler.
    while (s_eventIdx < s_eventCount) {
        GekkoGameEvent* ev = s_events[s_eventIdx];
        s_eventIdx++;

        if (!ev || ev->type == GekkoEmptyGameEvent) continue;

        switch (ev->type) {
            case GekkoSaveEvent:
                HandleSaveEvent(ev);
                continue;

            case GekkoLoadEvent:
                HandleLoadEvent(ev);
                continue;

            case GekkoAdvanceEvent:
                HandleAdvanceEvent(ev);
                s_waitingForAdvance = false;
                s_waitingAdvanceRbFrame = -1;
                s_waitingForAdvanceCount = 0;
                // Return Advance for EVERY advance event (both normal and rollback).
                // The game's own loop will call its match handler + input processing.
                return EventResult::Advance;

            default:
                NetplayLog_Write("GEKKO", s_currentRbFrame,
                    "ProcessNextEvent: unknown event type=%d at idx=%d/%d rb_frame=%d game_abs_frame=%d",
                    (int)ev->type,
                    s_eventIdx - 1,
                    s_eventCount,
                    s_currentRbFrame,
                    CurrentGameAbsFrame());
                continue;
        }
    }

    // No more events
    const bool hadEvents = s_eventCount > 0;
    const int processedEventCount = s_eventCount;
    s_frameStarted = false;
    s_rollingBack = false;
    s_events = nullptr;
    s_eventCount = 0;
    s_eventIdx = 0;
    if (hadEvents) {
        s_waitingForAdvance = false;
        s_waitingAdvanceRbFrame = -1;
        s_waitingForAdvanceCount = 0;
    }
    NetplayLog_Write("GEKKO", s_currentRbFrame,
        "ProcessNextEvent: Done rb_frame=%d game_abs_frame=%d (processed %d events, saves=%d loads=%d advances=%d)",
        s_currentRbFrame,
        CurrentGameAbsFrame(),
        processedEventCount,
        s_saveEventCount,
        s_loadEventCount,
        s_advanceEventCount);
    return EventResult::Done;
}

bool RollbackSession_HasPendingFrame() {
    return s_frameStarted;
}

bool RollbackSession_DrainPendingNonAdvanceEvents() {
    if (s_sessionBroken) {
        return false;
    }

    if (!s_active || !s_session || !s_frameStarted) {
        return true;
    }

    const int remainingBefore = s_eventCount - s_eventIdx;
    if (remainingBefore <= 0) {
        s_frameStarted = false;
        s_rollingBack = false;
        s_events = nullptr;
        s_eventCount = 0;
        s_eventIdx = 0;
        s_waitingForAdvance = false;
        s_waitingAdvanceRbFrame = -1;
        s_waitingForAdvanceCount = 0;
        return true;
    }

    NetplayLog_Write("GEKKO", s_currentRbFrame,
        "Draining pending non-advance events outside dispatcher: remaining=%d phase=%s",
        remainingBefore,
        Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));

    while (s_eventIdx < s_eventCount) {
        GekkoGameEvent* ev = s_events[s_eventIdx];
        if (!ev || ev->type == GekkoEmptyGameEvent) {
            s_eventIdx++;
            continue;
        }

        if (ev->type == GekkoAdvanceEvent) {
            NetplayLog_Write("GEKKO", s_currentRbFrame,
                "Pending advance left undrained outside dispatcher: idx=%d/%d rb_frame=%d phase=%s",
                s_eventIdx,
                s_eventCount,
                s_currentRbFrame,
                Net::MatchLifecyclePhaseName(Net::MatchLifecycle_GetPhase()));
            return false;
        }

        s_eventIdx++;
        if (ev->type == GekkoSaveEvent) {
            HandleSaveEvent(ev);
        } else if (ev->type == GekkoLoadEvent) {
            HandleLoadEvent(ev);
        } else {
            NetplayLog_Write("GEKKO", s_currentRbFrame,
                "Skipping unexpected non-advance event while draining: type=%d idx=%d/%d",
                (int)ev->type,
                s_eventIdx - 1,
                s_eventCount);
        }
    }

    s_frameStarted = false;
    s_rollingBack = false;
    s_events = nullptr;
    s_eventCount = 0;
    s_eventIdx = 0;
    s_waitingForAdvance = false;
    s_waitingAdvanceRbFrame = -1;
    s_waitingForAdvanceCount = 0;

    NetplayLog_Write("GEKKO", s_currentRbFrame,
        "Pending non-advance drain COMPLETE: rb_frame=%d game_abs_frame=%d",
        s_currentRbFrame,
        CurrentGameAbsFrame());
    return true;
}

void RollbackSession_GetAdvanceInputs(uint16_t* p1, uint16_t* p2) {
    if (p1) *p1 = s_advP1;
    if (p2) *p2 = s_advP2;
}

// ============================================================================
// GekkoNet Packet Ingestion
// ============================================================================

void RollbackSession_BufferGekkoPacket(const void* data, size_t len) {
    if (!data || len == 0) return;

    if (s_recvCount >= MAX_PENDING_RECV) {
        NetplayLog_Write("GEKKO", -1,
            "WARN: Receive buffer full (%d packets), dropping len=%zu queued=%d rb_frame=%d remote_recv=%d",
            MAX_PENDING_RECV,
            len,
            s_recvCount,
            s_currentRbFrame,
            s_remoteInputsRecv);
        return;
    }

    void* copy = malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);

    s_recvBuffer[s_recvCount].data = copy;
    s_recvBuffer[s_recvCount].len = len;
    s_recvCount++;
    s_remoteInputsRecv++;
}

// ============================================================================
// Queries
// ============================================================================

int32_t RollbackSession_GetCurrentFrame() {
    return s_currentRbFrame;
}

int32_t RollbackSession_GetFrameOriginAbs() {
    return s_frameOriginAbs;
}

int32_t RollbackSession_GetCurrentGameAbsFrame() {
    return CurrentGameAbsFrame();
}

int32_t RollbackSession_RbFrameToGameAbs(int32_t rb_frame) {
    return RbFrameToGameAbsFrame(rb_frame);
}

bool RollbackSession_IsRollingBack() {
    return s_rollingBack;
}

bool RollbackSession_IsSessionRunning() {
    return s_active && s_sessionRunning;
}

float RollbackSession_FramesAhead() {
    if (!s_active || !s_session) return 0.0f;
    return s_cachedFramesAhead;
}

int RollbackSession_GetActiveDelay() {
    return s_activeDelay;
}

bool RollbackSession_SetLocalDelay(int delay) {
    if (!s_active || !s_session || s_localHandle < 0) {
        return false;
    }

    if (delay < 0) {
        delay = 0;
    } else if (delay > 255) {
        delay = 255;
    }

    if (s_activeDelay == delay) {
        return true;
    }

    const int prevDelay = s_activeDelay;
    gekko_set_local_delay(s_session, s_localHandle, (unsigned char)delay);
    s_activeDelay = delay;

    NetplayLog_Write("GEKKO", s_currentRbFrame,
        "Local effective delay updated: rb_frame=%d game_abs_frame=%d %d -> %d (handle=%d)",
        s_currentRbFrame,
        CurrentGameAbsFrame(),
        prevDelay,
        s_activeDelay,
        s_localHandle);
    return true;
}

int RollbackSession_GetRollbackBudget() {
    return s_rollbackBudget;
}

bool RollbackSession_ShouldSuppressSideEffects() {
    return s_rollingBack;
}

const char* RollbackSession_GetErrorReason() {
    return s_sessionError;
}

void RollbackSession_GetTimesyncTelemetry(RollbackTimesyncTelemetry* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    const int32_t currentRbFrame = s_cachedCurrentRbFrame;

    out->rb_frame_current = currentRbFrame;
    out->rb_frame_last_confirmed = s_cachedConfirmedRbFrame;
    out->rb_frame_last_remote_received = s_cachedLastRemoteReceivedRbFrame;
    out->game_abs_frame_current = RbFrameToGameAbsFrame(currentRbFrame);
    out->frame_origin_abs = s_frameOriginAbs;
    out->rollback_count = s_totalRollbacks;
    out->last_rollback_replay_length = s_lastRollbackLength;
    out->max_rollback_distance = s_maxRollbackDepth;

    const float framesAhead = s_cachedFramesAhead;
    int predictedOutstanding = 0;
    if (framesAhead > 0.0f) {
        predictedOutstanding = (int)ceilf(framesAhead);
    }
    if (predictedOutstanding < 0) {
        predictedOutstanding = 0;
    }
    if (predictedOutstanding > s_rollbackBudget) {
        predictedOutstanding = s_rollbackBudget;
    }
    out->predicted_frames_outstanding = predictedOutstanding;
    out->frames_ahead = framesAhead;
    out->gekko_avg_ping = s_cachedAvgPing;
    out->gekko_jitter = s_cachedJitter;
}

// ============================================================================
// Local Input Injection (testing)
// ============================================================================

void RollbackSession_InjectLocalInput(uint16_t input) {
    s_hasInjectedInput = true;
    s_injectedInput = input;
}

// ============================================================================
// Diagnostics
// ============================================================================

void RollbackSession_GetSnapshot(RollbackSessionSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->active = s_active;
    out->local_player = s_localPlayer;
    out->remote_player = s_remotePlayer;

    // Frame state
    out->frame_origin_abs = s_frameOriginAbs;
    out->rb_frame_current = s_cachedCurrentRbFrame;
    out->game_abs_frame_current = RbFrameToGameAbsFrame(out->rb_frame_current);
    const float framesAhead = s_cachedFramesAhead;
    int predictedOutstanding = 0;
    if (framesAhead > 0.0f) {
        predictedOutstanding = (int)ceilf(framesAhead);
    }
    if (predictedOutstanding < 0) {
        predictedOutstanding = 0;
    }
    if (predictedOutstanding > s_rollbackBudget) {
        predictedOutstanding = s_rollbackBudget;
    }

    out->rb_frame_last_confirmed = s_cachedConfirmedRbFrame;
    out->rb_frame_last_remote_received = s_cachedLastRemoteReceivedRbFrame;
    out->rb_frame_last_saved_state = s_lastSavedRbFrame;

    // Rollback stats
    out->rollback_count = s_totalRollbacks;
    out->rb_last_rollback_start_frame = s_lastRollbackFrame;
    out->last_rollback_replay_length = s_lastRollbackLength;
    out->max_rollback_distance = s_maxRollbackDepth;
    out->predicted_frames_outstanding = predictedOutstanding;

    // GekkoNet state
    out->is_rolling_back = s_rollingBack;
    out->side_effects_suppressed = s_rollingBack;
    out->frames_ahead = framesAhead;

    // Policy
    out->active_delay = s_activeDelay;
    out->rollback_budget = s_rollbackBudget;

    // Checksums
    out->baseline_checksum = s_baselineChecksum;
    __try {
        out->current_checksum = CalcCRC32(
            (const void*)ADDR_MATCH_BASE,
            (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->current_checksum = 0xDEADDEAD;
    }

    // IO counts
    out->local_inputs_sent = s_localInputsSent;
    out->remote_inputs_received = s_remoteInputsRecv;

    // GekkoNet network stats
    out->gekko_avg_ping = s_cachedAvgPing;
    out->gekko_jitter = s_cachedJitter;
}

} // namespace Rollback
