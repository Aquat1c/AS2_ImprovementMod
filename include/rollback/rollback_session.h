/**
 * Alice Senki 2 - Rollback Session (GekkoNet-driven)
 *
 * GekkoNet event-driven rollback session. GekkoNet owns all rollback
 * logic: prediction, misprediction detection, savestate management,
 * resimulation scheduling, and timesync.
 *
 * The mod provides:
 *   - Transport adapter (ENet ↔ GekkoNet)
 *   - State capture/restore callbacks
 *   - Input injection into game buffers
 *   - Frame advance via game's match handler
 *
 * Two-phase API for the input dispatcher hook:
 *   1. BeginFrame(localInput) — feed local input to GekkoNet, update session
 *   2. ProcessNextEvent() — iterate GekkoNet events (save/load/advance)
 *      Returns Advance → dispatcher writes inputs, returns 0 (game steps)
 *      Returns Done → dispatcher returns -1 (break game's loop)
 *
 * Lifecycle:
 *   1. RollbackSession_Init() at mod startup
 *   2. RollbackSession_Begin() at bootstrap handoff
 *   3. BeginFrame/ProcessNextEvent called by input dispatcher each frame
 *   4. RollbackSession_End() on match exit or disconnect
 *   5. RollbackSession_Shutdown() at mod shutdown
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "rollback/rollback_telemetry.h"

namespace Rollback {

// ============================================================================
// Session Configuration (provided at Begin)
// ============================================================================

struct RollbackSessionConfig {
    int      local_player;       // 0 = P1, 1 = P2
    int      remote_player;      // 0 = P1, 1 = P2
    int      initial_delay;      // Input delay for local player
    int      rollback_budget;    // Max prediction window (GekkoNet input_prediction_window)
    uint32_t baseline_checksum;  // CRC32 of baseline state (for verification)
    int32_t  frame_origin_abs;   // Absolute engine frame where rollback rb_frame 0 begins
};

// ============================================================================
// Event Result (returned by ProcessNextEvent)
// ============================================================================

enum class EventResult {
    Advance,     // GekkoNet wants one frame advanced — inputs written to game buffers
    Done,        // No more events this update — break game's dispatcher loop
    Error        // Session error — caller should end session
};

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize rollback subsystems (state history, GekkoNet). Called once at mod init.
void RollbackSession_Init();

/// Shutdown rollback subsystems.
void RollbackSession_Shutdown();

/// Begin a GekkoNet rollback session after bootstrap handoff.
bool RollbackSession_Begin(const RollbackSessionConfig& config);

/// End the current session. Destroys GekkoNet session and cleans up.
void RollbackSession_End();

/// Is a rollback session currently active?
bool RollbackSession_IsActive();

// ============================================================================
// Two-Phase Frame Processing (called from input dispatcher hook)
// ============================================================================

/// Phase 1: Feed local input to GekkoNet, trigger session update.
/// Call ONCE per game loop iteration before ProcessNextEvent.
void RollbackSession_BeginFrame(uint16_t localInput);

/// Poll GekkoNet network/session state without feeding local input or
/// generating gameplay events. Used while gameplay is intentionally frozen so
/// disconnects and session state changes still propagate.
bool RollbackSession_PollSession();

/// Phase 2: Process the next GekkoNet event.
/// Call repeatedly until it returns Done or Error.
///   Advance → inputs written to game buffers; dispatcher should return 0
///   Done    → no more events; dispatcher should return -1
///   Error   → session broken; dispatcher should return -1
EventResult RollbackSession_ProcessNextEvent();

/// True while a BeginFrame/UpdateSession batch still has pending events.
bool RollbackSession_HasPendingFrame();

/// Drain only non-advance pending events (Save/Load/Done) outside the dispatcher.
/// Returns false if the pending stream still contains an Advance or the session is broken.
bool RollbackSession_DrainPendingNonAdvanceEvents();

/// Get the P1/P2 inputs from the last Advance event.
/// Only valid after ProcessNextEvent returns Advance.
void RollbackSession_GetAdvanceInputs(uint16_t* p1, uint16_t* p2);

// ============================================================================
// Wire packet ingestion (routed by net/packet_router)
// ============================================================================

/// Ingest a received InputStream (23) payload. Under AS2_WITH_GEKKO=ON the
/// payload is raw GekkoNet bytes buffered for the Gekko session to drain;
/// under the engine2 adapter it is the v2 InputStreamPayload (§3.2), with
/// the session_id gate applied before any state mutation (§3.1).
/// (M5: renamed from RollbackSession_BufferGekkoPacket — inventory §11.)
void RollbackSession_OnInputStreamPacket(const void* data, size_t len);

/// Ingest a received SyncHash (75) payload (§2.7.7 confirmed-frame
/// verification). No-op on the Gekko adapter (its desync detection rides
/// StateDigest); the engine2 adapter feeds the engine's hash queue.
void RollbackSession_OnSyncHashPacket(const void* data, size_t len);

// ============================================================================
// Queries
// ============================================================================

/// Current rollback-session-relative frame (rb_frame).
int32_t RollbackSession_GetCurrentFrame();

/// Absolute engine frame origin latched when rollback started.
int32_t RollbackSession_GetFrameOriginAbs();

/// Current engine frame derived from the active rollback frame origin.
int32_t RollbackSession_GetCurrentGameAbsFrame();

/// Convert a rollback-session-relative frame into the absolute engine frame domain.
int32_t RollbackSession_RbFrameToGameAbs(int32_t rb_frame);

/// Whether the current advance event is a rollback resimulation frame.
bool RollbackSession_IsRollingBack();

/// Whether GekkoNet has completed its initial sync and is producing game events.
bool RollbackSession_IsSessionRunning();

/// True while GekkoNet reports the remote peer as interrupted (inbound silence
/// past the interrupt timeout, but before the disconnect timeout). Non-fatal:
/// gameplay should freeze-and-wait; cleared automatically on PlayerResumed.
bool RollbackSession_IsPeerInterrupted();

/// GekkoNet's frame advantage metric for timesync decisions.
float RollbackSession_FramesAhead();

/// Current active delay.
int RollbackSession_GetActiveDelay();

/// Apply a new local input delay to the active Gekko session.
/// Returns true if accepted by the active session.
bool RollbackSession_SetLocalDelay(int delay);

/// Current rollback budget.
int RollbackSession_GetRollbackBudget();

/// Whether side effects should be suppressed (during rollback resim).
bool RollbackSession_ShouldSuppressSideEffects();

/// Fatal session error text, or an empty string when healthy.
const char* RollbackSession_GetErrorReason();

/// Copy the pending fatal error into out and clear the live session error.
/// Returns true when a non-empty reason was captured.
bool RollbackSession_TakeErrorReason(char* out, size_t outSize);

// RollbackTimesyncTelemetry now lives in rollback/rollback_telemetry.h (M0
// extraction; gekko_avg_ping/gekko_jitter renamed link_avg_ping/link_jitter).

/// Lightweight runtime telemetry for pacing/timesync logic.
/// Unlike RollbackSession_GetSnapshot, this does NOT compute large-state CRCs.
void RollbackSession_GetTimesyncTelemetry(RollbackTimesyncTelemetry* out);

// ============================================================================
// Local Input Injection (for testing/verification)
// ============================================================================

/// Override local input for the next BeginFrame call (test harness).
void RollbackSession_InjectLocalInput(uint16_t input);

// ============================================================================
// Diagnostics
// ============================================================================

struct RollbackSessionSnapshot {
    bool     active;
    int      local_player;
    int      remote_player;

    // Frame state
    int32_t  frame_origin_abs;
    int32_t  game_abs_frame_current;
    int32_t  rb_frame_current;
    int32_t  rb_frame_last_confirmed;
    int32_t  rb_frame_last_remote_received;
    int32_t  rb_frame_last_saved_state;

    // Rollback stats (from resim subsystem)
    int32_t  rollback_count;
    int32_t  rb_last_rollback_start_frame;
    int32_t  last_rollback_replay_length;
    int32_t  max_rollback_distance;
    int32_t  predicted_frames_outstanding;

    // GekkoNet state
    bool     is_rolling_back;
    bool     side_effects_suppressed;
    float    frames_ahead;

    // Policy
    int      active_delay;      // Visible user-facing delay; hidden floor stays internal.
    int      rollback_budget;

    // State integrity
    uint32_t current_checksum;
    uint32_t baseline_checksum;

    // Input stats
    int32_t  total_predictions;
    int32_t  total_mispredictions;
    int32_t  total_correct_predictions;

    // IO counts
    int32_t  local_inputs_sent;
    int32_t  remote_inputs_received;

    // Link stats (M5 rename per inventory §11: gekko_* → link_*; fed by the
    // backend's own RTT estimate — time_probe from M6 on the engine2 path)
    float    link_avg_ping;
    float    link_jitter;
};

void RollbackSession_GetSnapshot(RollbackSessionSnapshot* out);

/// Authoritative gameplay checksum (Gekko save/load equivalent: main match region + effect index).
uint32_t RollbackSession_ComputeLiveStateChecksum();

} // namespace Rollback
