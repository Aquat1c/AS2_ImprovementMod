/**
 * Alice Senki 2 - Resimulation Engine Implementation
 *
 * Contains:
 *   1. State history ring buffer (reuses savestate capture/restore pattern)
 *   2. Resimulation execution (load + replay + restore)
 */

#include "rollback/resimulation.h"
#include "rollback/input_timeline.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "input/input_system.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"

#include <string.h>
#include <new>
#include <windows.h>

namespace Rollback {

// ============================================================================
// Match Handler — direct call for resimulation
// ============================================================================

// sub_4C8F60: the game's Mode 8 handler. Takes dword_816358 as parameter.
// We call it directly during resim to advance one match frame without
// going through the full main loop (which would render, frame-limit, etc).
typedef unsigned short (__cdecl *MatchHandler_t)(uint32_t* a1);
static MatchHandler_t g_matchHandler = (MatchHandler_t)ADDR_MATCH_MODE;

// ============================================================================
// State History Slot
// ============================================================================

// Reuse the same state regions as the manual savestate system.
// These must stay in sync with savestate.cpp definitions.
#define SH_MAIN_START     ADDR_MATCH_BASE
#define SH_MAIN_SIZE      ((ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE)
#define SH_PRE_MATCH_START ADDR_PRE_MATCH_GAP
#define SH_PRE_MATCH_SIZE  PRE_MATCH_GAP_SIZE
#define SH_INPUT_P1_START  ADDR_P1_INPUT_BUFFER
#define SH_INPUT_P2_START  ADDR_P2_INPUT_BUFFER
#define SH_INPUT_SIZE      INPUT_BUFFER_SIZE

struct StateSlot {
    bool     valid;
    int32_t  frame;
    uint32_t checksum;
    uint32_t rng_seed;

    // Scattered globals
    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t game_mode;
    uint32_t substate;
    uint32_t substate_timer;
    uint32_t game_type;
    uint32_t match_phase_timer;
    uint32_t input_read_idx;
    uint32_t input_write_idx;

    // Blobs
    uint8_t main_state[SH_MAIN_SIZE];
    uint8_t pre_match_gap[SH_PRE_MATCH_SIZE];
    uint8_t input_p1[SH_INPUT_SIZE];
    uint8_t input_p2[SH_INPUT_SIZE];
};

// ============================================================================
// State History Ring Buffer
// ============================================================================

static StateSlot* s_slots         = nullptr;  // Heap-allocated (each slot is ~254KB)
static int        s_writeIdx      = 0;
static int        s_count         = 0;
static bool       s_historyInit   = false;

// ============================================================================
// Resimulation State
// ============================================================================

static bool    s_resimulating     = false;
static int32_t s_resimFrame       = -1;
static int32_t s_resimLength      = 0;

// Lifetime stats
static int32_t s_totalRollbacks        = 0;
static int32_t s_totalResimFrames      = 0;
static int32_t s_maxRollbackDepth      = 0;
static int32_t s_lastRollbackFrame     = -1;
static int32_t s_lastRollbackLength    = 0;

// ============================================================================
// State Capture / Restore (replicates savestate.cpp logic)
// ============================================================================

static bool CaptureToSlot(StateSlot* slot, int32_t frame) {
    slot->frame = frame;

    // Scattered globals
    slot->sim_frame       = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    slot->display_frame   = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    slot->game_mode       = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    slot->substate        = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    slot->substate_timer  = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    slot->game_type       = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    slot->match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
    slot->rng_seed        = DetVer_GetRngSeed();

    // Main region
    __try {
        memcpy(slot->main_state, (const void*)SH_MAIN_START, SH_MAIN_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[StateHistory] Capture AV at main region 0x%08X", SH_MAIN_START);
        return false;
    }

    // Pre-match gap
    __try {
        memcpy(slot->pre_match_gap, (const void*)SH_PRE_MATCH_START, SH_PRE_MATCH_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(slot->pre_match_gap, 0, SH_PRE_MATCH_SIZE);
    }

    // Input buffers
    __try {
        memcpy(slot->input_p1, (const void*)SH_INPUT_P1_START, SH_INPUT_SIZE);
        memcpy(slot->input_p2, (const void*)SH_INPUT_P2_START, SH_INPUT_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    slot->input_read_idx  = ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX);
    slot->input_write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);

    // Checksum for diagnostics
    slot->checksum = CalcCRC32(slot->main_state, SH_MAIN_SIZE);
    slot->valid = true;

    return true;
}

static bool RestoreFromSlot(const StateSlot* slot) {
    // Main region
    __try {
        memcpy((void*)SH_MAIN_START, slot->main_state, SH_MAIN_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[StateHistory] Restore AV at main region 0x%08X", SH_MAIN_START);
        return false;
    }

    // Pre-match gap
    __try {
        memcpy((void*)SH_PRE_MATCH_START, slot->pre_match_gap, SH_PRE_MATCH_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    // RNG
    DetVer_SetRngSeed(slot->rng_seed);

    // Scattered globals
    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, slot->sim_frame);
    WriteMemory<uint32_t>(ADDR_FRAME_COUNTER, slot->display_frame);
    WriteMemory<uint32_t>(ADDR_GAME_MODE, slot->game_mode);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, slot->substate);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, slot->substate_timer);
    WriteMemory<uint32_t>(ADDR_GAME_TYPE, slot->game_type);
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, slot->match_phase_timer);

    // Input buffers
    __try {
        memcpy((void*)SH_INPUT_P1_START, slot->input_p1, SH_INPUT_SIZE);
        memcpy((void*)SH_INPUT_P2_START, slot->input_p2, SH_INPUT_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    WriteMemory<uint32_t>(ADDR_INPUT_READ_IDX, slot->input_read_idx);
    WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, slot->input_write_idx);

    // Clear per-frame temp scratch to prevent stale collision data
    __try {
        memset((void*)ADDR_MATCH_PER_FRAME_TEMP, 0, MATCH_PER_FRAME_TEMP_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical
    }

    return true;
}

/// Write input pair (local + remote) into game buffers for the next frame advance.
/// local_player: 0 = P1 is local, 1 = P2 is local.
static void WriteInputsForFrame(int32_t frame, int local_player) {
    uint16_t local_input  = InputTimeline_GetLocalInput(frame);
    uint16_t remote_input = InputTimeline_GetRemoteInput(frame);

    uint16_t p1_input, p2_input;
    if (local_player == 0) {
        p1_input = local_input;
        p2_input = remote_input;
    } else {
        p1_input = remote_input;
        p2_input = local_input;
    }

    // Write through the SDL input system's netplay override path
    InputSystem_SetNetplayInput(0, p1_input);
    InputSystem_SetNetplayInput(1, p2_input);
    InputSystem_WriteToGameBuffersBothPlayers();
}

// ============================================================================
// State History API
// ============================================================================

void StateHistory_Init() {
    if (s_historyInit) return;

    // Heap-allocate the ring buffer — each slot is ~254KB, total ~4MB
    s_slots = new (std::nothrow) StateSlot[STATE_HISTORY_CAPACITY];
    if (!s_slots) {
        LOG_ERROR("[StateHistory] Failed to allocate %d slots (%zu bytes)",
            STATE_HISTORY_CAPACITY, sizeof(StateSlot) * STATE_HISTORY_CAPACITY);
        return;
    }

    memset(s_slots, 0, sizeof(StateSlot) * STATE_HISTORY_CAPACITY);
    s_writeIdx = 0;
    s_count = 0;
    s_historyInit = true;

    LOG_INFO("[StateHistory] Initialized (%d slots, %zu KB each, %zu KB total)",
        STATE_HISTORY_CAPACITY,
        sizeof(StateSlot) / 1024,
        (sizeof(StateSlot) * STATE_HISTORY_CAPACITY) / 1024);
    NetplayLog_Write("STATE", -1,
        "State history initialized: slots=%d slot_kb=%zu total_kb=%zu",
        STATE_HISTORY_CAPACITY,
        sizeof(StateSlot) / 1024,
        (sizeof(StateSlot) * STATE_HISTORY_CAPACITY) / 1024);
}

void StateHistory_Shutdown() {
    if (s_slots) {
        delete[] s_slots;
        s_slots = nullptr;
    }
    s_historyInit = false;
    s_writeIdx = 0;
    s_count = 0;
}

void StateHistory_Reset() {
    if (!s_historyInit || !s_slots) return;
    for (int i = 0; i < STATE_HISTORY_CAPACITY; i++) {
        s_slots[i].valid = false;
    }
    s_writeIdx = 0;
    s_count = 0;
    LOG_INFO("[StateHistory] Reset");
    NetplayLog_Write("STATE", -1, "State history reset");
}

bool StateHistory_CaptureFrame(int32_t frame) {
    if (!s_historyInit || !s_slots) return false;

    StateSlot* slot = &s_slots[s_writeIdx];
    if (!CaptureToSlot(slot, frame)) {
        return false;
    }

    s_writeIdx = (s_writeIdx + 1) % STATE_HISTORY_CAPACITY;
    if (s_count < STATE_HISTORY_CAPACITY) s_count++;

    NetplayLog_Verbose("STATE", frame,
        "Captured frame: checksum=0x%08X count=%d oldest=%d newest=%d",
        slot->checksum,
        s_count,
        StateHistory_GetOldestFrame(),
        StateHistory_GetNewestFrame());

    return true;
}

bool StateHistory_LoadFrame(int32_t frame) {
    if (!s_historyInit || !s_slots) return false;

    // Search for the requested frame in the ring buffer
    for (int i = 0; i < s_count; i++) {
        int idx = (s_writeIdx - 1 - i + STATE_HISTORY_CAPACITY) % STATE_HISTORY_CAPACITY;
        if (s_slots[idx].valid && s_slots[idx].frame == frame) {
            NetplayLog_Write("STATE", frame,
                "Loading state: checksum=0x%08X slot=%d",
                s_slots[idx].checksum,
                idx);
            return RestoreFromSlot(&s_slots[idx]);
        }
    }

    LOG_ERROR("[StateHistory] Frame %d not found in history (oldest=%d newest=%d count=%d)",
        frame, StateHistory_GetOldestFrame(), StateHistory_GetNewestFrame(), s_count);
    return false;
}

bool StateHistory_HasFrame(int32_t frame) {
    if (!s_historyInit || !s_slots) return false;
    for (int i = 0; i < s_count; i++) {
        int idx = (s_writeIdx - 1 - i + STATE_HISTORY_CAPACITY) % STATE_HISTORY_CAPACITY;
        if (s_slots[idx].valid && s_slots[idx].frame == frame) {
            return true;
        }
    }
    return false;
}

int32_t StateHistory_GetOldestFrame() {
    if (!s_historyInit || !s_slots || s_count == 0) return -1;
    int idx = (s_writeIdx - s_count + STATE_HISTORY_CAPACITY) % STATE_HISTORY_CAPACITY;
    return s_slots[idx].frame;
}

int32_t StateHistory_GetNewestFrame() {
    if (!s_historyInit || !s_slots || s_count == 0) return -1;
    int idx = (s_writeIdx - 1 + STATE_HISTORY_CAPACITY) % STATE_HISTORY_CAPACITY;
    return s_slots[idx].frame;
}

int32_t StateHistory_GetCount() {
    return s_count;
}

// ============================================================================
// Find best rollback frame
// ============================================================================

/// Find the latest saved frame that is <= target_frame.
/// Returns -1 if no suitable frame exists.
static int32_t FindBestRollbackFrame(int32_t target_frame) {
    int32_t best = -1;
    for (int i = 0; i < s_count; i++) {
        int idx = (s_writeIdx - 1 - i + STATE_HISTORY_CAPACITY) % STATE_HISTORY_CAPACITY;
        if (s_slots[idx].valid && s_slots[idx].frame <= target_frame) {
            if (best == -1 || s_slots[idx].frame > best) {
                best = s_slots[idx].frame;
            }
        }
    }
    return best;
}

// ============================================================================
// Resimulation
// ============================================================================

bool Resim_IsResimulating() {
    return s_resimulating;
}

int32_t Resim_GetCurrentFrame() {
    return s_resimFrame;
}

int32_t Resim_GetReplayLength() {
    return s_resimLength;
}

int32_t Resim_Execute(int32_t rollback_frame, int32_t target_frame, int local_player) {
    if (s_resimulating) {
        LOG_ERROR("[Resim] Already resimulating — recursive call blocked");
        return -1;
    }

    if (rollback_frame >= target_frame) {
        LOG_ERROR("[Resim] Invalid range: rollback=%d target=%d", rollback_frame, target_frame);
        return -1;
    }

    // Find the best available state at or before rollback_frame
    int32_t load_frame = FindBestRollbackFrame(rollback_frame);
    if (load_frame == -1) {
        LOG_ERROR("[Resim] No saved state at or before frame %d", rollback_frame);
        return -1;
    }

    int32_t replay_length = target_frame - load_frame;
    if (replay_length <= 0) {
        LOG_ERROR("[Resim] No frames to replay (load=%d target=%d)", load_frame, target_frame);
        return -1;
    }

    LOG_INFO("[Resim] BEGIN: load=%d rollback=%d target=%d replay=%d frames",
        load_frame, rollback_frame, target_frame, replay_length);
    NetplayLog_Write("RESIM", target_frame,
        "BEGIN load=%d rollback=%d target=%d replay=%d local=P%d",
        load_frame, rollback_frame, target_frame, replay_length, local_player + 1);

    // Load the saved state
    if (!StateHistory_LoadFrame(load_frame)) {
        LOG_ERROR("[Resim] Failed to load state at frame %d", load_frame);
        NetplayLog_Write("RESIM", target_frame,
            "ERROR: failed to load state at frame %d", load_frame);
        return -1;
    }

    // Enter resimulation mode
    s_resimulating = true;
    s_resimLength = replay_length;

    int32_t frames_replayed = 0;

    // Resimulate frame by frame
    for (int32_t f = load_frame; f < target_frame; f++) {
        s_resimFrame = f;

        const FrameInput* frameInput = InputTimeline_GetFrame(f);
        NetplayLog_Verbose("RESIM", f,
            "Replay frame: local=0x%04X remote=0x%04X local_ok=%d remote_ok=%d predicted=%d",
            frameInput ? frameInput->local : INPUT_NEUTRAL,
            frameInput ? frameInput->remote : INPUT_NEUTRAL,
            frameInput && frameInput->local_confirmed ? 1 : 0,
            frameInput && frameInput->remote_confirmed ? 1 : 0,
            frameInput && frameInput->remote_predicted ? 1 : 0);

        // Write the correct inputs for this frame
        WriteInputsForFrame(f, local_player);

        // Clear per-frame temp scratch (the game normally does this at
        // the top of Game_Update_MatchLoop, which we're bypassing)
        __try {
            memset((void*)ADDR_MATCH_PER_FRAME_TEMP, 0, MATCH_PER_FRAME_TEMP_SIZE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Continue resim even if this fails
        }

        // Advance the game's match handler by one frame
        __try {
            g_matchHandler((uint32_t*)ADDR_GAME_STATE_BASE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("[Resim] EXCEPTION during frame %d resimulation", f);
            NetplayLog_Write("RESIM", f,
                "ERROR: exception during resimulation");
            s_resimulating = false;
            s_resimFrame = -1;
            return -1;
        }

        frames_replayed++;
    }

    // Exit resimulation mode
    s_resimulating = false;
    s_resimFrame = -1;

    // Update stats
    s_totalRollbacks++;
    s_totalResimFrames += frames_replayed;
    s_lastRollbackFrame = load_frame;
    s_lastRollbackLength = frames_replayed;
    if (frames_replayed > s_maxRollbackDepth) {
        s_maxRollbackDepth = frames_replayed;
    }

    LOG_INFO("[Resim] DONE: replayed %d frames (total_rollbacks=%d max_depth=%d)",
        frames_replayed, s_totalRollbacks, s_maxRollbackDepth);
    NetplayLog_Write("RESIM", target_frame,
        "DONE replayed=%d total_rollbacks=%d max_depth=%d",
        frames_replayed, s_totalRollbacks, s_maxRollbackDepth);

    return frames_replayed;
}

// ============================================================================
// Diagnostics
// ============================================================================

void Resim_GetSnapshot(ResimSnapshot* out) {
    if (!out) return;

    out->is_resimulating = s_resimulating;
    out->current_resim_frame = s_resimFrame;
    out->replay_length = s_resimLength;

    out->total_rollbacks = s_totalRollbacks;
    out->total_frames_resimulated = s_totalResimFrames;
    out->max_rollback_depth = s_maxRollbackDepth;
    out->last_rollback_frame = s_lastRollbackFrame;
    out->last_rollback_length = s_lastRollbackLength;

    out->history_count = s_count;
    out->history_oldest_frame = StateHistory_GetOldestFrame();
    out->history_newest_frame = StateHistory_GetNewestFrame();
}

} // namespace Rollback
