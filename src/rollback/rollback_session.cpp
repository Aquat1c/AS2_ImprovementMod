/**
 * Alice Senki 2 - Rollback Session Implementation
 *
 * Central orchestrator for rollback gameplay.
 */

#include "rollback/rollback_session.h"
#include "rollback/input_timeline.h"
#include "rollback/prediction.h"
#include "rollback/resimulation.h"
#include "rollback/determinism_verify.h"
#include "rollback/stress_hooks.h"
#include "rollback/netplay_log.h"
#include "net/delay_policy.h"
#include "net/sync_policy.h"
#include "net/match_lifecycle.h"
#include "net/session_manager.h"
#include "net/protocol.h"
#include "net/player_side_mapping.h"
#include "input/input_system.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"

#include <string.h>
#include <algorithm>
#include <windows.h>

namespace Rollback {

// ============================================================================
// GameplayInput Wire Payload
// ============================================================================

#pragma pack(push, 1)
struct GameplayInputPayload {
    int32_t  frame;           // Frame this input is for
    uint16_t input;           // 16-bit input bitmask
    int32_t  start_frame;     // Start frame for redundant batch
    uint8_t  input_count;     // Number of inputs in batch (1-8)
    uint16_t inputs[8];       // Redundant input history (newest at [0])
};
#pragma pack(pop)

// ============================================================================
// Internal State
// ============================================================================

static bool     s_active            = false;
static int      s_localPlayer       = 0;
static int      s_remotePlayer      = 1;
static int32_t  s_currentFrame      = 0;
static int32_t  s_startFrame        = 0;
static uint32_t s_baselineChecksum  = 0;

// Policy consumption
static int      s_activeDelay       = 0;
static int      s_rollbackBudget    = 7;

// Rollback mitigations (CCCaster-style)
static const int MAX_ROLLBACK        = 15;   // Hard cap on rollback depth
static const int MIN_ROLLBACK_SPACING = 2;   // Min normal frames between rollbacks
static int32_t  s_framesSinceLastRollback = 0; // Cooldown counter

// Input tracking
static int32_t  s_lastSavedFrame    = -1;
static int32_t  s_localInputsSent   = 0;
static int32_t  s_remoteInputsRecv  = 0;

// Injected test input
static bool     s_hasInjectedInput  = false;
static int32_t  s_injectedFrame     = -1;
static uint16_t s_injectedInput     = 0;
static bool     s_loggedFirstFrameInput = false;

// Frame advance guard: prevents double-advancing when the game's natural
// loop will call Mode 8 handler after ModOnFrame returns.
static bool     s_frameAdvancePending = false;

static bool     s_initialized       = false;

// ============================================================================
// Network Send
// ============================================================================

/// Send local input to remote peer. Includes redundant recent history
/// for packet loss resilience.
static void SendLocalInput(int32_t frame, uint16_t input) {
    if (!Net::Session_IsConnected()) return;

    // Stress test: simulate packet drop
    if (StressHooks_ShouldDropPacket()) return;

    GameplayInputPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.frame = frame;
    payload.input = input;
    payload.start_frame = frame;

    // Include up to 8 recent inputs for redundancy
    int count = 0;
    for (int32_t f = frame; f >= s_startFrame && count < 8; f--, count++) {
        payload.inputs[count] = InputTimeline_GetLocalInput(f);
    }
    payload.input_count = (uint8_t)count;
    payload.start_frame = frame - count + 1;

    Net::Session_SendPacket(
        Net::CHANNEL_GAMEPLAY,
        Net::PacketType::GameplayInput,
        &payload, sizeof(payload),
        false  // Unreliable — speed > reliability, redundancy handles loss
    );

    NetplayLog_Verbose("RBINPUT", frame,
        "Send local input: input=0x%04X batch_start=%d count=%u",
        input,
        payload.start_frame,
        payload.input_count);

    s_localInputsSent++;
}

// ============================================================================
// Lifecycle
// ============================================================================

void RollbackSession_Init() {
    InputTimeline_Init();
    Prediction_Init();
    StateHistory_Init();

    s_active = false;
    s_initialized = true;
    LOG_INFO("[RollbackSession] Initialized");
}

void RollbackSession_Shutdown() {
    if (s_active) {
        RollbackSession_End();
    }

    StateHistory_Shutdown();
    Prediction_Shutdown();
    InputTimeline_Shutdown();

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

    // Reset subsystems
    InputTimeline_Reset();
    Prediction_Reset();
    StateHistory_Reset();

    // Store config
    s_localPlayer      = config.local_player;
    s_remotePlayer     = config.remote_player;
    s_startFrame       = config.start_frame;
    s_currentFrame     = config.start_frame;
    s_baselineChecksum = config.baseline_checksum;
    s_activeDelay      = config.initial_delay;
    s_rollbackBudget   = config.rollback_budget;

    s_lastSavedFrame   = -1;
    s_localInputsSent  = 0;
    s_remoteInputsRecv = 0;
    s_hasInjectedInput = false;
    s_loggedFirstFrameInput = false;
    s_frameAdvancePending = false;
    s_framesSinceLastRollback = MIN_ROLLBACK_SPACING; // Allow rollback immediately
    if (!StateHistory_CaptureFrame(s_startFrame)) {
        LOG_ERROR("[RollbackSession] Failed to capture baseline state");
        return false;
    }
    s_lastSavedFrame = s_startFrame;

    s_active = true;

    // Suppress pause during rollback gameplay
    InputSystem_SetPauseBlocked(true);

    LOG_INFO("[RollbackSession] BEGIN: local=P%d remote=P%d delay=%d rb_budget=%d "
             "start_frame=%d baseline_crc=0x%08X",
        s_localPlayer + 1, s_remotePlayer + 1,
        s_activeDelay, s_rollbackBudget,
        s_startFrame, s_baselineChecksum);

    NetplayLog_Write("RBSESS", s_startFrame,
        "BEGIN: local=P%d remote=P%d delay=%d rb_budget=%d baseline_crc=0x%08X",
        s_localPlayer + 1, s_remotePlayer + 1,
        s_activeDelay, s_rollbackBudget, s_baselineChecksum);

    NetplayLog_Write("RBSESS", s_startFrame,
        "State history capacity=%d slots",
        STATE_HISTORY_CAPACITY);

    return true;
}

void RollbackSession_End() {
    if (!s_active) return;

    NetplayLog_Write("RBSESS", s_currentFrame,
        "END: frame=%d sent=%d recv=%d",
        s_currentFrame, s_localInputsSent, s_remoteInputsRecv);

    s_active = false;
    s_hasInjectedInput = false;
    s_loggedFirstFrameInput = false;
    s_frameAdvancePending = false;

    // Reset subsystems so no stale state carries into next session
    InputTimeline_Reset();
    Prediction_Reset();
    StateHistory_Reset();

    // Clear netplay input overrides
    InputSystem_ClearNetplayInput(0);
    InputSystem_ClearNetplayInput(1);
    InputSystem_SetPauseBlocked(false);

    LOG_INFO("[RollbackSession] END: frame=%d sent=%d recv=%d",
        s_currentFrame, s_localInputsSent, s_remoteInputsRecv);
}

bool RollbackSession_IsActive() {
    return s_active;
}

// ============================================================================
// Remote Input Ingestion
// ============================================================================

void RollbackSession_SubmitRemoteInput(int32_t frame, uint16_t input) {
    if (!s_active) return;

    // Update the prediction system with confirmed remote input
    Prediction_OnRemoteConfirmed(frame, input);

    // Write to timeline — returns true if a prediction was wrong
    bool mismatch = InputTimeline_SetRemoteInput(frame, input);

    s_remoteInputsRecv++;

    NetplayLog_Verbose("RBINPUT", frame,
        "Remote input submitted: input=0x%04X recv_total=%d",
        input,
        s_remoteInputsRecv);

    if (mismatch) {
        LOG_INFO("[RollbackSession] Misprediction detected at frame %d "
                 "(remote_recv_total=%d)", frame, s_remoteInputsRecv);
        NetplayLog_Write("RBSESS", frame,
            "MISPREDICTION: remote_recv_total=%d", s_remoteInputsRecv);
    }
}

void RollbackSession_SubmitRemoteInputBatch(int32_t start_frame, const uint16_t* inputs, int count) {
    if (!s_active || !inputs) return;

    for (int i = 0; i < count; i++) {
        RollbackSession_SubmitRemoteInput(start_frame + i, inputs[i]);
    }
}

// ============================================================================
// Local Input Injection (testing)
// ============================================================================

void RollbackSession_InjectLocalInput(int32_t frame, uint16_t input) {
    s_hasInjectedInput = true;
    s_injectedFrame = frame;
    s_injectedInput = input;
}

// ============================================================================
// Per-Frame Update — The Core Rollback Loop
// ============================================================================

void RollbackSession_FrameUpdate() {
    if (!s_active) return;

    // --- REFRESH DELAY POLICY ---
    // Read the current active delay and rollback budget from the policy layer.
    // These may change mid-session via the delay change state machine.
    int policy_delay = Net::DelayPolicy_GetActiveDelay();
    int policy_budget = Net::DelayPolicy_GetAgreedRollbackBudget();

    if (policy_delay != s_activeDelay) {
        LOG_INFO("[RollbackSession] Delay updated: %d -> %d", s_activeDelay, policy_delay);
        NetplayLog_ValueChange("RBSESS", s_currentFrame,
            "active_delay", s_activeDelay, policy_delay, "DelayPolicy update");
        s_activeDelay = policy_delay;
    }
    if (policy_budget != s_rollbackBudget) {
        LOG_INFO("[RollbackSession] Rollback budget updated: %d -> %d",
            s_rollbackBudget, policy_budget);
        NetplayLog_ValueChange("RBSESS", s_currentFrame,
            "rollback_budget", s_rollbackBudget, policy_budget, "DelayPolicy update");
        s_rollbackBudget = policy_budget;
    }

    // --- STEP 1: COLLECT LOCAL INPUT ---
    uint16_t local_input;
    if (s_hasInjectedInput && s_injectedFrame == s_currentFrame) {
        local_input = s_injectedInput;
        s_hasInjectedInput = false;
    } else {
        // Always read from local P1 SDL bindings, regardless of which game
        // slot this machine controls. PlayerMapping handles the routing.
        local_input = Net::PlayerMapping_ReadLocalInput();
    }

    // The input for frame N goes into the timeline at frame N + delay.
    // This means the current frame's SDL input won't actually be used
    // until `delay` frames from now in the simulation.
    int32_t local_target_frame = s_currentFrame + s_activeDelay;

    InputTimeline_SetLocalInput(local_target_frame, local_input);

    NetplayLog_Verbose("RBSTEP", s_currentFrame,
        "Begin frame: local_input=0x%04X target_frame=%d active_delay=%d rb_budget=%d",
        local_input,
        local_target_frame,
        s_activeDelay,
        s_rollbackBudget);

    // --- STEP 2: SEND LOCAL INPUT TO REMOTE ---
    SendLocalInput(local_target_frame, local_input);

    // --- STEP 3: PREDICT MISSING REMOTE INPUTS ---
    // For any frame from the last confirmed remote up to the current frame,
    // if remote input hasn't arrived, predict it.
    int32_t last_confirmed = InputTimeline_GetLatestConfirmedRemoteFrame();
    for (int32_t f = last_confirmed + 1; f <= s_currentFrame; f++) {
        if (!InputTimeline_IsRemoteConfirmed(f)) {
            uint16_t predicted = Prediction_PredictRemote(f);

            // Stress hook: optionally corrupt prediction for testing
            predicted = StressHooks_MaybeCorruptPrediction(predicted);

            InputTimeline_PredictRemoteInput(f, predicted);
        }
    }

    NetplayLog_Verbose("RBSTEP", s_currentFrame,
        "Prediction window: last_confirmed=%d predicted_outstanding=%d",
        last_confirmed,
        InputTimeline_GetPredictedFrameCount());

    // --- STEP 3b: HARD CAP BLOCKING (CCCaster-style) ---
    // If we've exceeded MAX_ROLLBACK predicted (unconfirmed remote) frames,
    // block: do NOT advance the game frame. This prevents unbounded speculation
    // and forces us to wait for remote inputs to arrive.
    {
        int32_t predicted_count = InputTimeline_GetPredictedFrameCount();
        if (predicted_count >= MAX_ROLLBACK) {
            NetplayLog_Write("RBSESS", s_currentFrame,
                "BLOCKING: predicted_count=%d >= MAX_ROLLBACK=%d — waiting for remote",
                predicted_count, MAX_ROLLBACK);
            // Don't advance frame counter — the game will re-enter FrameUpdate
            // next frame and we'll check again. We already sent our input in Step 2
            // so remote has what it needs.
            return;
        }
    }

    // --- STEP 4: CHECK FOR MISPREDICTIONS ---
    // Find the first frame with a known-wrong prediction.
    int32_t mispredicted_frame = InputTimeline_FindFirstMisprediction(
        (std::max)(s_startFrame, last_confirmed - s_rollbackBudget));

    // --- STEP 5: ROLLBACK IF NEEDED ---
    // CCCaster-style mitigations:
    // (a) Spacing cooldown: don't rollback if we rolled back too recently
    // (b) Hard cap: clamp rollback depth to MAX_ROLLBACK
    if (mispredicted_frame >= 0 && mispredicted_frame < s_currentFrame) {
        // Spacing cooldown: skip this rollback if not enough normal frames have passed
        if (s_framesSinceLastRollback < MIN_ROLLBACK_SPACING) {
            NetplayLog_Verbose("RBSESS", s_currentFrame,
                "Rollback deferred: spacing cooldown (%d/%d frames since last)",
                s_framesSinceLastRollback, MIN_ROLLBACK_SPACING);
        } else {
            int32_t rollback_depth = s_currentFrame - mispredicted_frame;

            // Hard cap: clamp to MAX_ROLLBACK (CCCaster: 15)
            if (rollback_depth > MAX_ROLLBACK) {
                LOG_WARN("[RollbackSession] Rollback depth %d exceeds MAX_ROLLBACK %d — clamping",
                    rollback_depth, MAX_ROLLBACK);
                mispredicted_frame = s_currentFrame - MAX_ROLLBACK;
                rollback_depth = MAX_ROLLBACK;
            }

            // Also clamp to configured budget
            if (rollback_depth > s_rollbackBudget) {
                LOG_WARN("[RollbackSession] Rollback depth %d exceeds budget %d — clamping",
                    rollback_depth, s_rollbackBudget);
                mispredicted_frame = s_currentFrame - s_rollbackBudget;
            }

            // Verify the target frame is within state history before attempting rollback
            int32_t oldest = StateHistory_GetOldestFrame();
            if (oldest >= 0 && mispredicted_frame < oldest) {
                LOG_WARN("[RollbackSession] Rollback target %d is older than history (oldest=%d) — clamping",
                    mispredicted_frame, oldest);
                mispredicted_frame = oldest;
            }

            LOG_INFO("[RollbackSession] ROLLBACK: frame %d -> %d (depth=%d)",
                s_currentFrame, mispredicted_frame, s_currentFrame - mispredicted_frame);

            NetplayLog_Write("RBSESS", s_currentFrame,
                "ROLLBACK: %d -> %d (depth=%d spacing=%d)",
                s_currentFrame, mispredicted_frame,
                s_currentFrame - mispredicted_frame,
                s_framesSinceLastRollback);

            int32_t replayed = Resim_Execute(
                mispredicted_frame,
                s_currentFrame,
                s_localPlayer
            );

            if (replayed < 0) {
                LOG_ERROR("[RollbackSession] Resimulation failed — ending session");
                RollbackSession_End();
                return;
            }

            // Reset spacing cooldown after a rollback
            s_framesSinceLastRollback = 0;
        }
    }

    // Increment spacing cooldown counter
    s_framesSinceLastRollback++;

    // --- STEP 6: SAVE STATE FOR CURRENT FRAME ---
    // We save BEFORE the game advances this frame, so we can rollback
    // to this point if future input proves our predictions wrong.
    if (s_currentFrame > s_lastSavedFrame) {
        StateHistory_CaptureFrame(s_currentFrame);
        s_lastSavedFrame = s_currentFrame;
    }

    // --- STEP 7: WRITE INPUTS FOR CURRENT FRAME TO GAME BUFFERS ---
    // This is what the game will use when it processes this frame.
    {
        uint16_t p1_input, p2_input;
        uint16_t cur_local  = InputTimeline_GetLocalInput(s_currentFrame);
        uint16_t cur_remote = InputTimeline_GetRemoteInput(s_currentFrame);

        if (s_localPlayer == 0) {
            p1_input = cur_local;
            p2_input = cur_remote;
        } else {
            p1_input = cur_remote;
            p2_input = cur_local;
        }

        InputSystem_SetNetplayInput(0, p1_input);
        InputSystem_SetNetplayInput(1, p2_input);
        InputSystem_WriteToGameBuffersBothPlayers();

        if (!s_loggedFirstFrameInput) {
            NetplayLog_Write("RBSESS", s_currentFrame,
                "FIRST INPUT INJECTION: P1=0x%04X P2=0x%04X (local=P%d)",
                p1_input, p2_input, s_localPlayer + 1);
            s_loggedFirstFrameInput = true;
        }

        NetplayLog_Verbose("RBSTEP", s_currentFrame,
            "Inject inputs: P1=0x%04X P2=0x%04X local=P%d predicted=%d",
            p1_input,
            p2_input,
            s_localPlayer + 1,
            InputTimeline_GetPredictedFrameCount());
    }

    // --- STEP 8: ADVANCE FRAME COUNTER ---
    // The game's natural Mode 8 handler will execute after ModOnFrame returns.
    // We advance OUR frame counter here. The game's sim_frame_counter is
    // managed by the game itself — we just track our logical frame.
    s_currentFrame++;

    // NOTE: Delay policy consumption (marking active delay as applied) is
    // handled by the gameplay bridge after calling this function.
    // The bridge owns the delay policy lifecycle contract.

    NetplayLog_Verbose("RBSTEP", s_currentFrame,
        "End frame: next_frame=%d last_saved=%d local_sent=%d remote_recv=%d",
        s_currentFrame,
        s_lastSavedFrame,
        s_localInputsSent,
        s_remoteInputsRecv);
}

// ============================================================================
// Queries
// ============================================================================

int32_t RollbackSession_GetCurrentFrame() {
    return s_currentFrame;
}

int32_t RollbackSession_GetLastConfirmedFrame() {
    int32_t last_local = InputTimeline_GetLatestLocalFrame();
    int32_t last_remote = InputTimeline_GetLatestConfirmedRemoteFrame();
    return (std::min)(last_local, last_remote);
}

bool RollbackSession_IsResimulating() {
    return Resim_IsResimulating();
}

int RollbackSession_GetActiveDelay() {
    return s_activeDelay;
}

int RollbackSession_GetRollbackBudget() {
    return s_rollbackBudget;
}

bool RollbackSession_ShouldSuppressSideEffects() {
    return Resim_IsResimulating();
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
    out->current_frame = s_currentFrame;
    out->last_confirmed_frame = RollbackSession_GetLastConfirmedFrame();
    out->last_remote_received_frame = InputTimeline_GetLatestConfirmedRemoteFrame();
    out->last_saved_state_frame = s_lastSavedFrame;

    // Rollback stats from resim engine
    ResimSnapshot resim;
    Resim_GetSnapshot(&resim);
    out->rollback_count = resim.total_rollbacks;
    out->last_rollback_start_frame = resim.last_rollback_frame;
    out->last_rollback_replay_length = resim.last_rollback_length;
    out->max_rollback_distance = resim.max_rollback_depth;

    // Predicted frames
    out->predicted_frames_outstanding = InputTimeline_GetPredictedFrameCount();

    // Resimulation state
    out->is_resimulating = resim.is_resimulating;
    out->side_effects_suppressed = Resim_IsResimulating();

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

    // Input stats from timeline
    InputTimelineSnapshot tl;
    InputTimeline_GetSnapshot(&tl);
    out->total_predictions = tl.total_predictions;
    out->total_mispredictions = tl.total_mispredictions;
    out->total_correct_predictions = tl.total_correct_predictions;

    // IO counts
    out->local_inputs_sent = s_localInputsSent;
    out->remote_inputs_received = s_remoteInputsRecv;
}

} // namespace Rollback
