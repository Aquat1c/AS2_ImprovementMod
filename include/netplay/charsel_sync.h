/**
 * Alice Senki 2 - Character Select Sync Manager
 *
 * Implements mod-owned CharSel lockstep input sync (deterministic lockstep).
 * Both sides must have each other's input for frame N before frame N advances.
 * This guarantees identical cursor paths and character selections on both sides.
 *
 * Flow:
 *   1. SessionManager enters CharSel state
 *   2. CharSelSync::Begin() starts lockstep  
 *   3. Each frame: local input buffered, sent with redundancy, remote checked
 *   4. Frame only advances when both local+remote input available (lockstep)
 *   5. PollGameState() detects both-confirmed → TryLockConfig → MatchConfig exchange
 *   6. CharSelSync::GetLockedConfig() returns agreed config
 *   7. MatchBootstrapManager takes over
 */

#pragma once

#include "packet_codec.h"
#include <stdint.h>

namespace CharSelSync {

// ============================================================================
// Phases
// ============================================================================

enum class Phase : uint8_t {
    Enter       = 0,   // Just entered CharSel, waiting for first input
    Active      = 1,   // Lockstep running, processing frames
    Locked      = 2,   // Config locked, ready for match
};

// ============================================================================
// Locked Match Config
// ============================================================================

struct LockedMatchConfig {
    uint32_t version_hash;
    uint32_t p1_character_id;
    uint32_t p2_character_id;
    uint32_t p1_palette_id;
    uint32_t p2_palette_id;
    uint32_t stage_id;
    uint32_t round_count;
    uint32_t timer_setting;
    uint32_t gameplay_flags;
    uint32_t session_seed;
    uint32_t config_hash;     // Filled by HashMatchConfig
};

// ============================================================================
// Snapshot (for UI/debug)
// ============================================================================

struct Snapshot {
    Phase       phase;
    int32_t     local_frame;        // Current lockstep frame
    int32_t     remote_confirmed;   // Latest frame with confirmed remote input 
    int32_t     peer_acked_local;   // Latest local frame the peer reports having received
    int32_t     local_send_head;    // Latest local frame buffered for send
    int32_t     input_delay;        // Current input delay in frames
    uint32_t    stalls;             // Total frames we had to freeze
    uint8_t     local_slot;         // 1=P1, 2=P2
    uint8_t     remote_slot;        // 1=P1, 2=P2
    bool        config_locked;
    uint32_t    config_hash;
    char        status[128];
};

// ============================================================================
// Lifecycle
// ============================================================================

void Begin(bool isHost,
           int negotiatedDelay,
           uint64_t sessionId, uint32_t connectionId);
void End();
bool IsActive();

// ============================================================================
// Per-Frame Update (called by SessionManager::UpdateCharSel)
// ============================================================================

void FrameUpdate();

// ============================================================================
// Lockstep Input API (called by Hook_InputDispatcher)
// ============================================================================

// Set the observed RTT so lockstep can compute input delay.
void SetRttMs(float rttMs);

// Compute the recommended CharSel input delay for a measured RTT.
int SuggestInputDelay(float rttMs);

// Buffer local input for the next frame, send to peer.
// Called once per game frame from Hook_InputDispatcher.
void BufferAndSendLocalInput(uint16_t rawInput);

// Check if we have both local and remote input for the current lockstep frame.
bool HasInputsForCurrentFrame();

// Consume inputs for the current lockstep frame.
// Writes P1/P2 input based on host/client role. Advances local frame.
// Returns true on success.
bool ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2);

// Canonicalize the shared stage-grid input for a lockstep frame.
// Inputs must be in game-slot order where P1 is the host and P2 is the joiner.
// Non-conflicting inputs are preserved, but ambiguous same-frame conflicts are
// resolved deterministically so both peers drive the same shared cursor.
uint16_t CanonicalizeSharedStageInput(uint16_t p1Input, uint16_t p2Input);

// ============================================================================
// Receive (called from SessionManager packet router)
// ============================================================================

void OnReceiveInput(const PacketCodec::CharSelInputPayload* payload);
void OnReceiveMatchConfig(const PacketCodec::MatchConfigPayload* payload);
void OnReceiveMatchConfigAck(const PacketCodec::MatchConfigAckPayload* payload);

// ============================================================================
// Queries
// ============================================================================

Phase GetPhase();
bool IsConfigLocked();
bool GetLockedConfig(LockedMatchConfig* out);

// Returns the last config that was locked (survives End()/Begin() cycles).
// Used to sync cursor positions when re-entering charsel after a match.
bool GetLastConfirmedConfig(LockedMatchConfig* out);

// Write cursor positions + char IDs + stage to game memory from the last
// confirmed config.  Called before charsel-init to guarantee both peers
// see identical cursor placement.  Returns true if values were written.
bool RestoreCursorsFromLastConfig();

void GetSnapshot(Snapshot* out);

} // namespace CharSelSync
