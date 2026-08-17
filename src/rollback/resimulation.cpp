/**
 * Alice Senki 2 - Resimulation Engine Implementation
 *
 * Contains:
 *   1. State history ring buffer (reuses savestate capture/restore pattern)
 *   2. Resimulation execution (load + replay + restore)
 */

#include "rollback/resimulation.h"
#include "rollback/determinism_verify.h"
#include "rollback/game_snapshot.h"
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

// Direct-mapped tagged slot (re0.7 M4, plan §2.7.7): slot = frame % 64,
// every slot tagged {epoch, frame, phase}; restores validate the tag
// fail-closed so a stale-epoch slot can never be restored.
struct StateSlot {
    GameSnapshot snap;
    uint32_t     epoch = 0;   // 0 = untagged/offline capture
    uint32_t     phase = 0;
    uint64_t     gameplay_hash = 0;
};

// ============================================================================
// State History Ring Buffer
// ============================================================================

static StateSlot* s_slots         = nullptr;  // Heap-allocated (each slot is ~254KB)
static bool       s_historyInit   = false;
static uint32_t   s_tagEpoch      = 0;        // stamped onto captures
static uint32_t   s_tagPhase      = 0;

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

/// Write input pair (local + remote) into game buffers for the next frame advance.
/// local_player: 0 = P1 is local, 1 = P2 is local.
static void WriteInputsForFrame(int32_t frame, int local_player) {
    // Dead code — the engine2 adapter drives resimulation through the
    // rollback transaction (BeginRollback/CommitReplayFrame), not this path.
    // Kept as a compilation stub for Resim_Execute (also dead code).
    (void)frame;
    (void)local_player;
}

// ============================================================================
// State History API
// ============================================================================

static int SlotIndexFor(int32_t frame) {
    return (int)((uint32_t)frame % (uint32_t)STATE_HISTORY_CAPACITY);
}

void StateHistory_Init() {
    if (s_historyInit) return;

    // Heap-allocate the ring buffer — each slot is ~254KB
    s_slots = new (std::nothrow) StateSlot[STATE_HISTORY_CAPACITY];
    if (!s_slots) {
        LOG_ERROR("[StateHistory] Failed to allocate %d slots (%zu bytes)",
            STATE_HISTORY_CAPACITY, sizeof(StateSlot) * STATE_HISTORY_CAPACITY);
        return;
    }

    memset(s_slots, 0, sizeof(StateSlot) * STATE_HISTORY_CAPACITY);
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
}

void StateHistory_Reset() {
    if (!s_historyInit || !s_slots) return;
    for (int i = 0; i < STATE_HISTORY_CAPACITY; i++) {
        GameSnapshot_Clear(&s_slots[i].snap);
        s_slots[i].epoch = 0;
        s_slots[i].phase = 0;
        s_slots[i].gameplay_hash = 0;
    }
    LOG_INFO("[StateHistory] Reset");
    NetplayLog_Write("STATE", -1, "State history reset");
}

void StateHistory_SetTagContext(uint32_t epoch, uint32_t phase) {
    s_tagEpoch = epoch;
    s_tagPhase = phase;
    NetplayLog_Write("STATE", -1,
        "State history tag context: epoch=%u phase=%u", epoch, phase);
}

static bool CaptureFrameInternal(int32_t frame, uint64_t* out_hash, bool want_hash) {
    if (!s_historyInit || !s_slots) return false;

    StateSlot* slot = &s_slots[SlotIndexFor(frame)];
    if (!GameSnapshot_Capture(&slot->snap, frame)) {
        return false;
    }
    slot->epoch = s_tagEpoch;
    slot->phase = s_tagPhase;
    slot->gameplay_hash = want_hash ? GameSnapshot_HashGameplay(&slot->snap) : 0;
    if (out_hash) *out_hash = slot->gameplay_hash;

    NetplayLog_Verbose("STATE", frame,
        "Captured frame: checksum=0x%08X epoch=%u slot=%d",
        slot->snap.checksum,
        slot->epoch,
        SlotIndexFor(frame));

    return true;
}

bool StateHistory_CaptureFrame(int32_t frame) {
    return CaptureFrameInternal(frame, nullptr, false);
}

bool StateHistory_CaptureFrameHashed(int32_t frame, uint64_t* out_gameplay_hash) {
    return CaptureFrameInternal(frame, out_gameplay_hash, true);
}

bool StateHistory_LoadFrame(int32_t frame) {
    if (!s_historyInit || !s_slots) return false;

    StateSlot* slot = &s_slots[SlotIndexFor(frame)];
    if (slot->snap.valid && slot->snap.frame == frame) {
        NetplayLog_Write("STATE", frame,
            "Loading state: checksum=0x%08X epoch=%u slot=%d",
            slot->snap.checksum,
            slot->epoch,
            SlotIndexFor(frame));
        return GameSnapshot_Restore(&slot->snap);
    }

    LOG_ERROR("[StateHistory] Frame %d not found in history (oldest=%d newest=%d count=%d)",
        frame, StateHistory_GetOldestFrame(), StateHistory_GetNewestFrame(),
        StateHistory_GetCount());
    return false;
}

bool StateHistory_LoadFrameTagged(int32_t frame, uint32_t epoch) {
    if (!s_historyInit || !s_slots) return false;

    StateSlot* slot = &s_slots[SlotIndexFor(frame)];
    if (!slot->snap.valid || slot->snap.frame != frame || slot->epoch != epoch) {
        // Tag mismatch = fail-closed: never restore a stale-epoch slot
        // (§2.7.5).
        LOG_ERROR("[StateHistory] Tagged restore refused: frame=%d want_epoch=%u "
            "slot={valid=%d frame=%d epoch=%u}",
            frame, epoch,
            slot->snap.valid ? 1 : 0, slot->snap.frame, slot->epoch);
        NetplayLog_Write("STATE", frame,
            "Tagged restore REFUSED: want_epoch=%u slot_valid=%d slot_frame=%d slot_epoch=%u",
            epoch, slot->snap.valid ? 1 : 0, slot->snap.frame, slot->epoch);
        return false;
    }
    NetplayLog_Write("STATE", frame,
        "Loading tagged state: checksum=0x%08X epoch=%u slot=%d",
        slot->snap.checksum, slot->epoch, SlotIndexFor(frame));
    return GameSnapshot_Restore(&slot->snap);
}

bool StateHistory_HasFrame(int32_t frame) {
    if (!s_historyInit || !s_slots) return false;
    const StateSlot* slot = &s_slots[SlotIndexFor(frame)];
    return slot->snap.valid && slot->snap.frame == frame;
}

void StateHistory_DiscardFramesAfter(int32_t frame) {
    if (!s_historyInit || !s_slots) return;

    for (int i = 0; i < STATE_HISTORY_CAPACITY; i++) {
        if (s_slots[i].snap.valid && s_slots[i].snap.frame > frame) {
            GameSnapshot_Clear(&s_slots[i].snap);
            s_slots[i].epoch = 0;
            s_slots[i].phase = 0;
            s_slots[i].gameplay_hash = 0;
        }
    }

    NetplayLog_Write("STATE", frame,
        "Discarded future states after frame %d: count=%d oldest=%d newest=%d",
        frame,
        StateHistory_GetCount(),
        StateHistory_GetOldestFrame(),
        StateHistory_GetNewestFrame());
}

int32_t StateHistory_GetOldestFrame() {
    if (!s_historyInit || !s_slots) return -1;
    int32_t oldest = -1;
    for (int i = 0; i < STATE_HISTORY_CAPACITY; i++) {
        if (s_slots[i].snap.valid &&
            (oldest == -1 || s_slots[i].snap.frame < oldest)) {
            oldest = s_slots[i].snap.frame;
        }
    }
    return oldest;
}

int32_t StateHistory_GetNewestFrame() {
    if (!s_historyInit || !s_slots) return -1;
    int32_t newest = -1;
    for (int i = 0; i < STATE_HISTORY_CAPACITY; i++) {
        if (s_slots[i].snap.valid && s_slots[i].snap.frame > newest) {
            newest = s_slots[i].snap.frame;
        }
    }
    return newest;
}

int32_t StateHistory_GetCount() {
    if (!s_historyInit || !s_slots) return 0;
    int32_t count = 0;
    for (int i = 0; i < STATE_HISTORY_CAPACITY; i++) {
        if (s_slots[i].snap.valid) count++;
    }
    return count;
}

// ============================================================================
// Find best rollback frame
// ============================================================================

/// Find the latest saved frame that is <= target_frame.
/// Returns -1 if no suitable frame exists.
static int32_t FindBestRollbackFrame(int32_t target_frame) {
    int32_t best = -1;
    for (int i = 0; i < STATE_HISTORY_CAPACITY; i++) {
        if (s_slots[i].snap.valid && s_slots[i].snap.frame <= target_frame) {
            if (best == -1 || s_slots[i].snap.frame > best) {
                best = s_slots[i].snap.frame;
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

        NetplayLog_Verbose("RESIM", f,
            "Replay frame %d (dead path — the engine2 transaction owns rollback)", f);

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

    out->history_count = StateHistory_GetCount();
    out->history_oldest_frame = StateHistory_GetOldestFrame();
    out->history_newest_frame = StateHistory_GetNewestFrame();
}

} // namespace Rollback
