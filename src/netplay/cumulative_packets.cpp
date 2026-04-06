/**
 * Cumulative Input Packets - Implementation
 * 
 * Based on giuroll-hagb's approach: every packet contains ALL unconfirmed
 * inputs since the last frame the opponent confirmed. This provides automatic
 * packet loss recovery without explicit ACKs or retransmission.
 */

#include "cumulative_packets.h"
#include "log_window.h"
#include <windows.h>  // For GetTickCount()
#include <string.h>
#include <algorithm>

// ============================================================================
// INTERNAL STATE
// ============================================================================

static InputHistoryBuffer s_input_history = {};
static FrameConfirmState s_confirm_state = {};
static uint16_t s_session_id = 0;
static bool s_initialized = false;

// ============================================================================
// INITIALIZATION
// ============================================================================

void CumulativePackets_Init(void) {
    memset(&s_input_history, 0, sizeof(s_input_history));
    memset(&s_confirm_state, 0, sizeof(s_confirm_state));
    s_session_id = (uint16_t)(GetTickCount() & 0xFFFF);
    s_initialized = true;
    LOG_NET_INFO("[CumulativePackets] Initialized with session_id=%u", s_session_id);
}

void CumulativePackets_Reset(void) {
    memset(&s_input_history, 0, sizeof(s_input_history));
    memset(&s_confirm_state, 0, sizeof(s_confirm_state));
    // Generate new session ID for new match
    s_session_id = (uint16_t)(GetTickCount() & 0xFFFF);
    LOG_NET_INFO("[CumulativePackets] Reset for new match, session_id=%u", s_session_id);
}

// ============================================================================
// INPUT HISTORY MANAGEMENT
// ============================================================================

void CumulativePackets_RecordInput(uint32_t frame, uint16_t input) {
    if (!s_initialized) {
        CumulativePackets_Init();
    }
    
    // Write to circular buffer
    uint32_t idx = frame & (INPUT_HISTORY_SIZE - 1);  // Fast modulo for power of 2
    s_input_history.inputs[idx] = input;
    s_input_history.frame_ids[idx] = frame;
    
    // Update tracking
    if (frame > s_input_history.newest_frame || s_input_history.newest_frame == 0) {
        s_input_history.newest_frame = frame;
    }
    
    // Update oldest frame (when buffer wraps)
    if (s_input_history.oldest_frame == 0) {
        s_input_history.oldest_frame = frame;
    } else if (frame >= INPUT_HISTORY_SIZE) {
        s_input_history.oldest_frame = frame - INPUT_HISTORY_SIZE + 1;
    }
    
    s_confirm_state.last_local_input_frame = frame;
}

// Get input for a specific frame from history
static bool GetInputFromHistory(uint32_t frame, uint16_t* outInput) {
    if (frame < s_input_history.oldest_frame || frame > s_input_history.newest_frame) {
        return false;  // Frame not in buffer
    }
    
    uint32_t idx = frame & (INPUT_HISTORY_SIZE - 1);
    if (s_input_history.frame_ids[idx] != frame) {
        return false;  // Slot was overwritten
    }
    
    *outInput = s_input_history.inputs[idx];
    return true;
}

// ============================================================================
// PACKET BUILDING
// ============================================================================

int CumulativePackets_BuildGameplayPacket(
    uint8_t* outBuf,
    int bufSize,
    uint32_t currentFrame,
    uint16_t currentInput,
    uint8_t delay,
    uint8_t maxRollback,
    uint8_t desyncCheck,
    int32_t timingSync
) {
    if (!outBuf || bufSize < sizeof(AS2GameplayHeader)) {
        LOG_NET_ERROR("[CumulativePackets] BuildGameplayPacket: invalid buffer");
        return 0;
    }
    
    // Record current input
    CumulativePackets_RecordInput(currentFrame, currentInput);
    
    // Determine range of inputs to include: from (last_opponent_confirm + 1) to currentFrame
    // This is the KEY insight from giuroll-hagb: include ALL unconfirmed inputs
    uint32_t startFrame = s_confirm_state.last_opponent_confirm + 1;
    if (startFrame < 1) startFrame = 1;  // Frame 0 is special
    
    // Clamp to available history
    if (startFrame < s_input_history.oldest_frame) {
        startFrame = s_input_history.oldest_frame;
    }
    
    // Calculate how many inputs we're including
    int inputCount = 0;
    if (currentFrame >= startFrame) {
        inputCount = (int)(currentFrame - startFrame + 1);
    }
    
    // Cap at maximum
    if (inputCount > MAX_CUMULATIVE_INPUTS) {
        startFrame = currentFrame - MAX_CUMULATIVE_INPUTS + 1;
        inputCount = MAX_CUMULATIVE_INPUTS;
    }
    
    // Check buffer size
    int totalSize = sizeof(AS2GameplayHeader) + inputCount * sizeof(uint16_t);
    if (totalSize > bufSize) {
        LOG_NET_ERROR("[CumulativePackets] Buffer too small: need %d, have %d", totalSize, bufSize);
        return 0;
    }
    
    // Build header
    AS2GameplayHeader* hdr = (AS2GameplayHeader*)outBuf;
    hdr->hdr.magic = AS2_UNIFIED_MAGIC;
    hdr->hdr.phase = PHASE_GAMEPLAY;
    hdr->hdr.subtype = 0;  // Not used for gameplay
    hdr->hdr.session_id = s_session_id;
    hdr->frame_id = currentFrame;
    hdr->last_confirmed = s_confirm_state.last_received_frame;  // What we've received from opponent
    hdr->input_count = (uint8_t)inputCount;
    hdr->delay = delay;
    hdr->max_rollback = maxRollback;
    hdr->desync_check = desyncCheck;
    hdr->timing_sync = timingSync;
    
    // Write inputs (newest first, like giuroll-hagb)
    uint16_t* inputPtr = (uint16_t*)(outBuf + sizeof(AS2GameplayHeader));
    for (int i = 0; i < inputCount; i++) {
        uint32_t frame = currentFrame - i;
        uint16_t input = 0;
        if (!GetInputFromHistory(frame, &input)) {
            // Missing from history - use current input as fallback
            input = currentInput;
        }
        inputPtr[i] = input;
    }
    
    // Update state
    s_confirm_state.last_sent_frame = currentFrame;
    
    // Log periodically
    static int s_send_count = 0;
    if (++s_send_count <= 3 || s_send_count % 300 == 0) {
        LOG_NET_DEBUG("[CumulativePackets] Built packet: frame=%u, inputs=%d (from %u), lastConfirm=%u",
                  currentFrame, inputCount, startFrame, hdr->last_confirmed);
    }
    
    return totalSize;
}

// ============================================================================
// PACKET PROCESSING
// ============================================================================

bool CumulativePackets_ProcessGameplayPacket(
    const uint8_t* data,
    int dataLen,
    uint16_t* outInputs,
    int* outInputCount,
    uint32_t* outFrameStart,
    uint32_t* outLastConfirm
) {
    if (!data || dataLen < (int)sizeof(AS2GameplayHeader)) {
        return false;
    }
    
    const AS2GameplayHeader* hdr = (const AS2GameplayHeader*)data;
    
    // Validate magic and phase
    if (hdr->hdr.magic != AS2_UNIFIED_MAGIC) {
        return false;  // Not our packet
    }
    if (hdr->hdr.phase != PHASE_GAMEPLAY) {
        return false;  // Not a gameplay packet
    }
    
    // Validate input count
    int expectedSize = sizeof(AS2GameplayHeader) + hdr->input_count * sizeof(uint16_t);
    if (dataLen < expectedSize) {
        LOG_NET_WARN("[CumulativePackets] Packet too short: expected %d, got %d", expectedSize, dataLen);
        return false;
    }
    if (hdr->input_count > MAX_CUMULATIVE_INPUTS) {
        LOG_NET_WARN("[CumulativePackets] Too many inputs: %d > %d", hdr->input_count, MAX_CUMULATIVE_INPUTS);
        return false;
    }
    
    // Extract inputs
    const uint16_t* inputPtr = (const uint16_t*)(data + sizeof(AS2GameplayHeader));
    
    // Inputs are stored newest-first, so frame_id is the newest
    uint32_t newestFrame = hdr->frame_id;
    uint32_t oldestFrame = newestFrame - hdr->input_count + 1;
    
    if (outInputs && outInputCount) {
        // Copy inputs (still newest-first)
        for (int i = 0; i < hdr->input_count; i++) {
            outInputs[i] = inputPtr[i];
        }
        *outInputCount = hdr->input_count;
    }
    
    if (outFrameStart) {
        *outFrameStart = oldestFrame;  // Starting frame of input range
    }
    
    if (outLastConfirm) {
        *outLastConfirm = hdr->last_confirmed;  // Opponent's confirmation
    }
    
    // Update our confirmation state based on opponent's packet
    // CRITICAL: The opponent's `last_confirmed` tells us what frames they've received from us
    // This is how we know we can stop retransmitting those frames
    if (hdr->last_confirmed > s_confirm_state.last_opponent_confirm) {
        s_confirm_state.last_opponent_confirm = hdr->last_confirmed;
    }
    
    // Track what we've received
    if (newestFrame > s_confirm_state.last_received_frame) {
        s_confirm_state.last_received_frame = newestFrame;
    }
    
    // Log periodically
    static int s_recv_count = 0;
    if (++s_recv_count <= 3 || s_recv_count % 300 == 0) {
        LOG_NET_DEBUG("[CumulativePackets] Processed packet: frame=%u, inputs=%d (from %u), theyConfirmed=%u",
                  newestFrame, hdr->input_count, oldestFrame, hdr->last_confirmed);
    }
    
    return true;
}

// ============================================================================
// STATE ACCESS
// ============================================================================

const FrameConfirmState* CumulativePackets_GetState(void) {
    return &s_confirm_state;
}

bool CumulativePackets_IsFrameConfirmed(uint32_t frame) {
    return frame <= s_confirm_state.last_opponent_confirm;
}
