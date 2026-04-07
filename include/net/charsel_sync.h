/**
 * Alice Senki 2 - Character Select / Stage Select Sync
 *
 * Lockstep synchronization for the CharSel and StageSel front-end phases.
 * Reads local cursor/confirm state from game memory, sends it to the remote
 * peer, and applies remote state to the P2 slot (or P1 if we are client-side P2).
 *
 * This module does NOT modify the game's CharSel state machine — it only
 * relays input state between peers so both sides see identical selections.
 * The actual game CharSel runs as GAMETYPE_VS_HUMAN (local 2P), and we
 * puppet the remote player's cursor/confirm from network data.
 */

#pragma once

#include "net/protocol.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// CharSel Sync Snapshot
// ============================================================================

struct CharSelSyncSnapshot {
    bool     active;
    bool     in_stage_phase;          // true if in StageSel phase

    // Character selection state
    uint8_t  local_cursor;
    uint8_t  local_confirmed;
    uint8_t  remote_cursor;
    uint8_t  remote_confirmed;
    bool     both_characters_locked;

    // Resolved character IDs (after grid lookup)
    uint8_t  p1_character;
    uint8_t  p1_palette;
    uint8_t  p2_character;
    uint8_t  p2_palette;

    // Stage selection state
    uint8_t  local_stage;
    bool     local_stage_confirmed;
    uint8_t  remote_stage;
    bool     remote_stage_confirmed;
    bool     both_stage_locked;
    uint8_t  stage_id;
};

// ============================================================================
// Lifecycle
// ============================================================================

void CharSelSync_Init();
void CharSelSync_Shutdown();

// ============================================================================
// Control
// ============================================================================

/// Begin CharSel sync. Call when entering CharSel.
void CharSelSync_Begin();

/// Transition to stage select phase.
void CharSelSync_BeginStagePhase();

/// Abort CharSel sync (disconnect/quit).
void CharSelSync_Abort();

// ============================================================================
// Per-Frame
// ============================================================================

/// Drive charsel/stage sync. Call every frame during frontend sync.
void CharSelSync_FrameUpdate();

// ============================================================================
// Packet Reception (called by PregameSync packet handler)
// ============================================================================

void CharSelSync_OnRemoteInput(const CharSelInputPayload* p);
void CharSelSync_OnRemoteLock(const CharSelLockPayload* p);
void CharSelSync_OnRemoteStage(const StageSyncPayload* p);

// ============================================================================
// Queries
// ============================================================================

void CharSelSync_GetSnapshot(CharSelSyncSnapshot* out);

} // namespace Net
