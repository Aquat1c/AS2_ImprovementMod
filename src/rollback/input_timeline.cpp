/**
 * Alice Senki 2 - Input Timeline Implementation
 */

#include "rollback/input_timeline.h"
#include "ui/log_window.h"

#include <string.h>

namespace Rollback {

// ============================================================================
// Internal State
// ============================================================================

static FrameInput s_buffer[TIMELINE_CAPACITY];
static int32_t    s_baseFrame         = 0;    // Frame number of buffer[0]
static int32_t    s_latestLocal       = -1;   // Latest frame with local input
static int32_t    s_latestRemoteConf  = -1;   // Latest frame with confirmed remote
static bool       s_initialized       = false;

// Lifetime stats
static int32_t    s_totalPredictions      = 0;
static int32_t    s_totalMispredictions   = 0;
static int32_t    s_totalCorrect          = 0;

// ============================================================================
// Helpers
// ============================================================================

static int BufIndex(int32_t frame) {
    int idx = (int)(frame - s_baseFrame);
    if (idx < 0 || idx >= TIMELINE_CAPACITY) return -1;
    return idx;
}

/// Advance the base frame so that `frame` fits in the buffer.
/// Discards old frames as needed.
static void EnsureFrameFits(int32_t frame) {
    if (frame < s_baseFrame) return;  // Already fits or is in the past

    int needed = (int)(frame - s_baseFrame);
    if (needed < TIMELINE_CAPACITY) return;  // Already fits

    // Need to shift the window forward
    int shift = needed - TIMELINE_CAPACITY + 1;
    if (shift > 0) {
        // Move remaining entries forward
        int remaining = TIMELINE_CAPACITY - shift;
        if (remaining > 0) {
            memmove(s_buffer, s_buffer + shift, remaining * sizeof(FrameInput));
            // Zero the newly exposed slots
            memset(s_buffer + remaining, 0, shift * sizeof(FrameInput));
        } else {
            // Entire buffer is invalidated
            memset(s_buffer, 0, sizeof(s_buffer));
        }
        s_baseFrame += shift;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void InputTimeline_Init() {
    memset(s_buffer, 0, sizeof(s_buffer));
    s_baseFrame = 0;
    s_latestLocal = -1;
    s_latestRemoteConf = -1;
    s_totalPredictions = 0;
    s_totalMispredictions = 0;
    s_totalCorrect = 0;
    s_initialized = true;
    LOG_INFO("[InputTimeline] Initialized (capacity=%d)", TIMELINE_CAPACITY);
}

void InputTimeline_Shutdown() {
    s_initialized = false;
}

void InputTimeline_Reset() {
    memset(s_buffer, 0, sizeof(s_buffer));
    s_baseFrame = 0;
    s_latestLocal = -1;
    s_latestRemoteConf = -1;
    // Keep lifetime stats
    LOG_INFO("[InputTimeline] Reset");
}

// ============================================================================
// Writing
// ============================================================================

void InputTimeline_SetLocalInput(int32_t frame, uint16_t input) {
    if (!s_initialized) return;

    EnsureFrameFits(frame);
    int idx = BufIndex(frame);
    if (idx < 0) return;

    s_buffer[idx].local = input;
    s_buffer[idx].local_confirmed = true;

    if (frame > s_latestLocal) {
        s_latestLocal = frame;
    }
}

bool InputTimeline_SetRemoteInput(int32_t frame, uint16_t input) {
    if (!s_initialized) return false;

    EnsureFrameFits(frame);
    int idx = BufIndex(frame);
    if (idx < 0) return false;

    bool was_predicted = s_buffer[idx].remote_predicted;
    bool mismatch = false;

    if (was_predicted) {
        // Compare with prediction
        if (s_buffer[idx].remote != input) {
            s_buffer[idx].prediction_wrong = true;
            s_totalMispredictions++;
            mismatch = true;
        } else {
            s_totalCorrect++;
        }
    }

    s_buffer[idx].remote = input;
    s_buffer[idx].remote_confirmed = true;
    s_buffer[idx].remote_predicted = false;

    if (frame > s_latestRemoteConf) {
        s_latestRemoteConf = frame;
    }

    return mismatch;
}

void InputTimeline_PredictRemoteInput(int32_t frame, uint16_t predicted_input) {
    if (!s_initialized) return;

    EnsureFrameFits(frame);
    int idx = BufIndex(frame);
    if (idx < 0) return;

    // Don't overwrite confirmed input
    if (s_buffer[idx].remote_confirmed) return;

    s_buffer[idx].remote = predicted_input;
    s_buffer[idx].remote_predicted = true;
    s_buffer[idx].prediction_wrong = false;
    s_totalPredictions++;
}

// ============================================================================
// Reading
// ============================================================================

const FrameInput* InputTimeline_GetFrame(int32_t frame) {
    int idx = BufIndex(frame);
    if (idx < 0) return nullptr;
    return &s_buffer[idx];
}

uint16_t InputTimeline_GetLocalInput(int32_t frame) {
    int idx = BufIndex(frame);
    if (idx < 0) return INPUT_NEUTRAL;
    return s_buffer[idx].local;
}

uint16_t InputTimeline_GetRemoteInput(int32_t frame) {
    int idx = BufIndex(frame);
    if (idx < 0) return INPUT_NEUTRAL;
    return s_buffer[idx].remote;
}

bool InputTimeline_IsRemoteConfirmed(int32_t frame) {
    int idx = BufIndex(frame);
    if (idx < 0) return false;
    return s_buffer[idx].remote_confirmed;
}

// ============================================================================
// Timeline Queries
// ============================================================================

int32_t InputTimeline_GetEarliestFrame() {
    return s_baseFrame;
}

int32_t InputTimeline_GetLatestLocalFrame() {
    return s_latestLocal;
}

int32_t InputTimeline_GetLatestConfirmedRemoteFrame() {
    return s_latestRemoteConf;
}

int32_t InputTimeline_FindFirstMisprediction(int32_t start_frame) {
    for (int32_t f = start_frame; f <= s_latestLocal; f++) {
        int idx = BufIndex(f);
        if (idx < 0) continue;
        if (s_buffer[idx].prediction_wrong) {
            return f;
        }
    }
    return -1;
}

int32_t InputTimeline_GetPredictedFrameCount() {
    int32_t count = 0;
    for (int32_t f = s_latestRemoteConf + 1; f <= s_latestLocal; f++) {
        int idx = BufIndex(f);
        if (idx < 0) continue;
        if (s_buffer[idx].remote_predicted && !s_buffer[idx].remote_confirmed) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// Diagnostics
// ============================================================================

void InputTimeline_GetSnapshot(InputTimelineSnapshot* out) {
    if (!out) return;
    out->earliest_frame = s_baseFrame;
    out->latest_local_frame = s_latestLocal;
    out->latest_confirmed_remote_frame = s_latestRemoteConf;
    out->predicted_frame_count = InputTimeline_GetPredictedFrameCount();
    out->total_predictions = s_totalPredictions;
    out->total_mispredictions = s_totalMispredictions;
    out->total_correct_predictions = s_totalCorrect;
}

} // namespace Rollback
