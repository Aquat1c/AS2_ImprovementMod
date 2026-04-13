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

/// Force flush the log file (call on errors or shutdown).
void NetplayLog_Flush();

} // namespace Rollback
