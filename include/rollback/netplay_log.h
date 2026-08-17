/**
 * Alice Senki 2 - Dedicated Full-Path Rollback/Netplay Log
 *
 * A separate log file covering the ENTIRE mod-owned online path:
 *   - Session connect/disconnect
 *   - Handshake / initial sync
 *   - Character Select / Stage Select sync
 *   - Bootstrap (config, loading, baseline)
 *   - Lifecycle transitions (intro/round/pause/winscreen/match end)
 *   - Rollback gameplay events (prediction, misprediction, rollback, replay)
 *   - Delay/runtime policy changes (with before/after values)
 *   - Post-match / rematch / cleanup
 *   - Desync suspicion / checksum mismatch
 *
 * Uses a clear, searchable format with timestamps and frame numbers.
 * Lines are queued through Diagnostics::AsyncLog so gameplay/front-end hot
 * paths do not perform FILE I/O. Queue overflow drops log lines and emits
 * LOGSTATS summaries instead of blocking the game.
 * Supports normal and verbose modes.
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// ============================================================================
// Lifecycle
// ============================================================================

void NetplayLog_Init();
void NetplayLog_Shutdown();

/// Set the log directory (typically the session log dir).
void NetplayLog_SetLogDir(const char* dir);

// ============================================================================
// Verbosity
// ============================================================================

void NetplayLog_SetVerbose(bool verbose);
bool NetplayLog_IsVerbose();

// ============================================================================
// Logging API
// ============================================================================

/// Main log function. tag = subsystem tag (e.g., "BOOT", "LIFE", "INPUT", "ROLLBACK").
/// frame = game frame or -1 if not applicable.
void NetplayLog_Write(const char* tag, int32_t frame, const char* fmt, ...);

/// Spectator log function. Writes to the main full-path netplay log and the
/// dedicated spectator log file.
void NetplayLog_WriteSpectator(const char* tag, int32_t frame, const char* fmt, ...);

/// Verbose-only log (only written in verbose mode).
void NetplayLog_Verbose(const char* tag, int32_t frame, const char* fmt, ...);

/// Verbose-only spectator log. Writes to the main full-path netplay log and
/// the dedicated spectator log file when verbose mode is enabled.
void NetplayLog_VerboseSpectator(const char* tag, int32_t frame, const char* fmt, ...);

/// Log a before/after state change with reason.
void NetplayLog_StateChange(const char* tag, int32_t frame,
                            const char* field,
                            const char* before, const char* after,
                            const char* reason);

/// Log a before/after integer value change with reason.
void NetplayLog_ValueChange(const char* tag, int32_t frame,
                            const char* field,
                            int before, int after,
                            const char* reason);

/// Request an async flush. Shutdown performs a blocking drain.
void NetplayLog_Flush();

// ============================================================================
// STAT line (re0.7 §2.10) — structured per-second telemetry rollup
// ============================================================================

/// One second of pacing/engine telemetry. Holds are bucketed by the closed
/// HoldCause set (rollback/run_state.h); fields with no producer yet (present
/// percentiles, slew before the M2 scheduler lands) are reported as zero.
struct NetplayStatSample {
    float    sim_fps;             // canonical sim frames advanced this second
    uint32_t present_p50_us;      // present interval p50 (0 until FrameScheduler)
    uint32_t present_p99_us;      // present interval p99 (0 until FrameScheduler)
    uint32_t holds_prediction;    // HoldCause::PredictionLimit passes
    uint32_t holds_lifecycle;     // HoldCause::LifecycleBoundary passes
    uint32_t holds_local_input;   // HoldCause::LocalInputMissing passes
    uint32_t holds_external;      // HoldCause::ExternalSuspension passes
    uint32_t rollbacks;           // corrections applied this second
    uint32_t rollback_max_depth;  // deepest correction this second
    int32_t  slew_ppm;            // active pace slew (0 until FrameScheduler)
    int32_t  debt_frames;         // owed hidden frames (CadenceDebt / legacy debt)
    uint32_t silence_ms;          // supervisor inbound protocol silence
};

/// Emit the frozen-format STAT line. The acceptance harness parses exactly
/// this shape for the whole 0.7 cycle:
///   STAT sim_fps=<f2> present_p50_us=<u> present_p99_us=<u> hold_pred=<u>
///   hold_life=<u> hold_input=<u> hold_ext=<u> rollbacks=<u> rb_max=<u>
///   slew_ppm=<d> debt=<d> silence_ms=<u>
void NetplayLog_Stat(int32_t frame, const NetplayStatSample& sample);

} // namespace Rollback
