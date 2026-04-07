/**
 * Alice Senki 2 - Character Select / Stage Select Sync
 *
 * Input-driven lockstep synchronization for CharSel and StageSel phases.
 *
 * Both peers exchange raw per-frame inputs.  The input dispatcher hook
 * (Hook_InputDispatcher on sub_5625E0) injects lockstep inputs so the
 * game's native charsel handler processes them identically on both sides.
 *
 * Side assignment:
 *   Host  = P1 (left cursor),  local input → outputInputs[0]
 *   Join  = P2 (right cursor), local input → outputInputs[1]
 *
 * The game runs as GAMETYPE_VS_HUMAN.  Both cursors are driven by
 * lockstep data, not local hardware.
 */

#pragma once

#include "net/protocol.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// CharSel Sync Snapshot (queried by pregame_sync)
// ============================================================================

struct CharSelSyncSnapshot {
    bool     active;
    bool     in_stage_phase;

    // Character selection state (from game memory polling)
    uint8_t  local_cursor;
    uint8_t  local_confirmed;
    uint8_t  remote_cursor;
    uint8_t  remote_confirmed;
    bool     both_characters_locked;

    // Resolved character IDs
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

    // Lockstep diagnostics
    uint32_t lockstep_frame;
    uint32_t local_input_frame;
    int      input_delay;
};

// ============================================================================
// Lifecycle
// ============================================================================

void CharSelSync_Init();
void CharSelSync_Shutdown();

// ============================================================================
// Control
// ============================================================================

void CharSelSync_Begin();
void CharSelSync_BeginStagePhase();
void CharSelSync_Abort();

// ============================================================================
// Per-Frame
// ============================================================================

/// Drive charsel sync. Called every frame from pregame_sync.
void CharSelSync_FrameUpdate();

// ============================================================================
// Lockstep Input Interface (called by Hook_InputDispatcher)
// ============================================================================

/// Capture local player's raw input for the current lockstep frame.
/// Called once per game frame from Hook_InputProcess, before the charsel
/// handler runs.  Input is in game-packed format (from ReadPlayerInput).
void CharSelSync_CaptureLocalInput(uint16_t packedInput);

/// Are both local and remote inputs available for the current consume frame?
bool CharSelSync_HasInputsForCurrentFrame();

/// Consume the current frame's inputs and return P1/P2 in game-packed format.
/// Advances the consume frame counter.  Host→P1, Join→P2.
bool CharSelSync_ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2);

/// Is the lockstep system actively running?
bool CharSelSync_IsLockstepActive();

// ============================================================================
// Packet Reception
// ============================================================================

void CharSelSync_OnRemoteFrameInput(const CharSelFrameInputPayload* p);
void CharSelSync_OnRemoteLock(const CharSelLockPayload* p);
void CharSelSync_OnRemoteStage(const StageSyncPayload* p);

// ============================================================================
// Queries
// ============================================================================

void CharSelSync_GetSnapshot(CharSelSyncSnapshot* out);

} // namespace Net
