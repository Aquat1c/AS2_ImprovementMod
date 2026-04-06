/**
 * Alice Senki 2 - Rollback Session
 *
 * Wraps a GekkoNet GameSession to drive rollback netplay:
 *   - Implements GekkoNetAdapter to bridge GekkoNet ↔ UdpSocket
 *   - Processes GekkoGameEvents: Advance → inject inputs, Save → capture, Load → restore
 *   - Manages savestate ring buffer for GekkoNet's limited_saving
 *
 * Lifecycle:
 *   1. SessionManager reaches Connected state
 *   2. Both sides send Ready packets
 *   3. RollbackSession::Create() sets up GekkoNet session + adapter
 *   4. Each game frame: call Update() which feeds local input and processes events
 *   5. On disconnect/error: call Destroy()
 */

#pragma once

#include <stdint.h>
namespace RollbackSession {

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    bool     is_host;              // true = P1 (host), false = P2 (join)
    uint8_t  input_delay;          // Local input delay frames (0-15)
    uint8_t  max_rollback;         // Max rollback window (default 8)
    bool     desync_detection;     // Enable checksum comparison
};

// ============================================================================
// Status
// ============================================================================

enum class State : uint32_t {
    Inactive = 0,    // Not created
    Syncing,         // Waiting for both players to sync
    Running,         // Active rollback gameplay
    Error,           // Fatal error
};

struct Snapshot {
    State    state;
    int      local_frame;         // Current local frame
    int      remote_frame;        // Last confirmed remote frame
    int      rollback_frames;     // Frames currently being resimulated
    float    frames_ahead;        // How far ahead of opponent (from GekkoNet)
    uint32_t save_count;          // Total savestates captured
    uint32_t load_count;          // Total savestates restored
    uint32_t advance_count;       // Total frames advanced
    uint32_t rollback_count;      // Total rollbacks triggered
    uint32_t adapter_send_count;  // Packets handed to the transport adapter
    uint32_t adapter_recv_count;  // Packets received from the transport adapter
    uint32_t pending_event_count; // Gekko events fetched this visual frame
    bool     rolling_back;        // True while replaying history
    uint32_t timesync_skips;      // Frames skipped to stay in sync with opponent
    char     status[128];
};

// ============================================================================
// Lifecycle
// ============================================================================

// Create and start a GekkoNet game session.
// Session sends/receives through SessionManager::SendToPeer() / BufferGekkoPacket().
// Returns true on success.
bool Create(const Config* config);

// Destroy the session and free all resources.
void Destroy();

// ============================================================================
// Per-Frame Update (legacy single-call — prefer two-phase API below)
// ============================================================================

// Feed local input and process GekkoNet events.
// Call once per game frame while in gameplay.
// localInput: 16-bit input bitmask from SDL/input system
void Update(uint16_t localInput);

// Poll only the GekkoNet transport/session handshake without generating
// gameplay events. Used while the match is still frozen on the load barrier.
void NetworkPoll();

// ============================================================================
// Two-Phase Frame Stepping API (for game loop integration)
// ============================================================================
//
// The game loop calls Hook_InputDispatcher repeatedly:
//   while (!Hook_InputDispatcher(out)) { /* simulate one frame */ }
//
// Phase 1 (once per visual frame):
//   BeginFrame(localInput) — feeds input to GekkoNet and fetches events.
//
// Phase 2 (called repeatedly by the game loop):
//   ProcessNextEvent() — processes Save/Load events internally,
//   returns FrameAdvance when the game should simulate one frame,
//   or NoMoreEvents when the game loop should break.

enum class EventResult {
    FrameAdvance,    // Inputs set via override — game should simulate one frame
    NoMoreEvents,    // All events consumed — break the game loop
    Error,           // Fatal error
};

// Phase 1: Feed local input and fetch new events from GekkoNet.
// Call exactly once per visual frame, before the first ProcessNextEvent().
void BeginFrame(uint16_t localInput);

// Phase 2: Process events until the next FrameAdvance or end.
// Save/Load events are handled internally (transparent to caller).
EventResult ProcessNextEvent();

// Returns true while the current BeginFrame batch still has unprocessed events.
bool HasPendingEvents();

// ============================================================================
// Queries
// ============================================================================

bool IsActive();
State GetState();
bool GetSnapshot(Snapshot* out);

// Was the last Update() call a rollback frame? (for skipping rendering, audio, etc.)
bool IsRollingBack();

// Current GekkoNet frame
int GetCurrentFrame();

// How far ahead local is relative to remote (positive = we're ahead).
// Used for timesync: caller should skip advancing when this is too positive.
float GetFramesAhead();

// Record a timesync skip (called by input_sync_hooks when throttling).
void RecordTimesyncSkip();

// Inputs from the most recent GekkoAdvanceEvent.
// Returns false until the first advance event provides a valid payload.
bool GetCurrentInputs(uint16_t* outP1, uint16_t* outP2);

// Change the local input delay on a live session.
// Returns false if no session is active.  Clamps delay to [0, 15].
bool SetDelay(int newDelay);

} // namespace RollbackSession
