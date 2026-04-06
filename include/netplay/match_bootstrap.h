/**
 * Alice Senki 2 - Match Bootstrap Manager
 *
 * Implements the match bootstrap sequence after CharSel lock (§7):
 *   1. Apply locked match config to game memory
 *   2. Load barrier (WAITING → READY → CONFIRMED → MISMATCH → TIMEOUT)
 *   3. Baseline sync (savestate + checksum exchange)
 *   4. Start gameplay (delay lock, frame-0 agreement)
 *
 * CRITICAL RULES (from §7):
 *   - PumpIncomingPackets can receive LOADED_SYNC before ResetForNewMatch
 *   - Always reset READY_SYNC on new match
 *   - Both sides start running on ready_done=true, NOT on first gameplay packet
 *   - Delay lock: max(local, remote), locked before frame 0
 */

#pragma once

#include "charsel_sync.h"
#include "packet_codec.h"
#include <stdint.h>

namespace MatchBootstrap {

// ============================================================================
// Load Barrier States (§7.1)
// ============================================================================

enum class LoadState : uint8_t {
    Idle      = 0,
    Waiting   = 1,   // Assets loading locally
    Ready     = 2,   // Snapshot taken, checksum computed
    Confirmed = 3,   // Both checksums match → proceed
    Mismatch  = 4,   // Checksums differ → abort
    Timeout   = 5,   // Peer did not respond → disconnect
};

// ============================================================================
// Ready Sync States (§7.2)
// ============================================================================

enum class ReadyState : uint8_t {
    Idle      = 0,
    Announce  = 1,   // Host sending RS_ANNOUNCE
    Confirm   = 2,   // Client sending RS_CONFIRM
    Done      = 3,   // Both sides agreed
};

// ============================================================================
// Overall Bootstrap Phase
// ============================================================================

enum class Phase : uint8_t {
    Idle          = 0,
    ApplyConfig   = 1,   // Writing locked config to game memory
    Loading       = 2,   // Load barrier active
    BaselineSync  = 3,   // Capturing baseline savestate + exchange
    ReadySync     = 4,   // RTT/delay negotiation
    Starting      = 5,   // Both sides agreed, about to start
    Complete      = 6,   // Gameplay can begin
    Error         = 7,   // Abort
};

// ============================================================================
// Snapshot
// ============================================================================

struct Snapshot {
    Phase      phase;
    LoadState  load_state;
    ReadyState ready_state;
    bool       local_loaded;
    bool       remote_loaded;
    bool       local_ready;
    bool       remote_ready;
    bool       local_baseline_ready;
    bool       remote_baseline_ready;
    uint32_t   local_asset_hash;
    uint32_t   remote_asset_hash;
    uint32_t   local_baseline_checksum;
    uint32_t   remote_baseline_checksum;
    uint32_t   baseline_frame;
    uint32_t   config_hash;
    uint8_t    negotiated_delay;
    float      rtt_ms;
    uint32_t   phase_elapsed_ms;
    uint32_t   start_deadline_ms;
    char       status[128];
    char       error[128];
};

// ============================================================================
// Lifecycle
// ============================================================================

// Begin match bootstrap with locked config from CharSel.
void Begin(const CharSelSync::LockedMatchConfig* config,
           bool isHost,
           uint64_t sessionId, uint32_t connectionId,
           float initialRttMs);

// Abort bootstrap and return to clean state.
void Abort(const char* reason);

// Is bootstrap currently active?
bool IsActive();

// Has bootstrap completed successfully? (ready for gameplay)
bool IsComplete();

// ============================================================================
// Per-Frame Update
// ============================================================================

// Call once per frame while bootstrap is active.
// Drives the state machine: apply config → load barrier → baseline → ready sync.
void FrameUpdate();

// ============================================================================
// External Events
// ============================================================================

// Notify that local assets have finished loading (load barrier can advance).
void NotifyLocalLoaded();
void SetObservedRttMs(float rttMs);

// Process incoming bootstrap packets (called from SessionManager router).
void OnReceiveLoadBarrierReady(const PacketCodec::LoadBarrierReadyPayload* payload);
void OnReceiveBaselineReady(const PacketCodec::BaselineReadyPayload* payload);
void OnReceiveStartGameplay(const PacketCodec::StartGameplayPayload* payload,
                            uint32_t senderTimestampMs);
void OnReceiveStartGameplayAck(const PacketCodec::StartGameplayAckPayload* payload);

// ============================================================================
// Queries
// ============================================================================

Phase GetPhase();
void GetSnapshot(Snapshot* out);
uint8_t GetNegotiatedDelay();
uint32_t GetSessionSeed();

} // namespace MatchBootstrap
