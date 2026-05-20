/**
 * Alice Senki 2 - Stage Select Sync
 *
 * Confirmed shared-input buffer for the stage select grid (charsel substate 7).
 *
 * Unlike character select where each player has their own cursor, stage select
 * has a SINGLE shared cursor on a 12x2 grid that BOTH players can move.
 *
 * Decompilation of sub_5C0B20 shows the game checks each direction/button as:
 *   if (P1_input == 1 || P2_input == 1) → act once
 *
 * We pre-merge P1 and P2 into a single confirmed shared input BEFORE writing
 * to the game.  Both machines compute the same merge from lockstep-confirmed
 * inputs, so the game sees exactly ONE identical input on both sides.
 *
 * Merge rules (STATELESS — pure function, no internal state):
 *   - OR all inputs:  combined = p1 | p2
 *   - Cancel opposing directions:  Left+Right → neither, Up+Down → neither
 *   - Buttons (A–D, Start, Select): simple OR
 *
 * The merged input is written to P1's slot; P2 is zeroed.
 * A confirmed-frame audit ring records each frame for desync diagnostics.
 *
 * Source-of-truth: reverse-engineering notes for sub_5C0B20 (stage select grid handler).
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Lifecycle
// ============================================================================

void StageSelSync_Init();
void StageSelSync_Shutdown();

/// Activate stage select sync. Called when charsel enters stage phase.
void StageSelSync_Begin();

/// Deactivate stage select sync.
void StageSelSync_Abort();

/// Is stage select sync currently active?
bool StageSelSync_IsActive();

/// Returns true once after StageSelSync_Begin(), then false.
/// Used by Hook_InputProcess to reset edge detection on phase entry,
/// preventing desync from unsynchronized physical input during animation subs 5-6.
bool StageSelSync_ConsumeEdgeReset();

/// True while a stage-grid confirm edge has entered the netplay confirm gate
/// and is waiting for synchronized release.
bool StageSelSync_IsConfirmPending();

// ============================================================================
// Shared Input Merge (called from Hook_InputDispatcher)
// ============================================================================

/// Merge P1 and P2 lockstep-confirmed inputs into a single shared input.
/// STATELESS: same (p1, p2) always produces the same result — no internal
/// state, no cooldown, no history. Records the confirmed frame in the audit
/// ring. The merge authority lives at raw input injection in Hook_InputProcess;
/// callers should not pre-merge elsewhere.
uint16_t StageSelSync_MergeConfirmed(uint32_t frame, uint16_t p1, uint16_t p2);

/// Gate the native stage-grid confirm edge until the peer has acknowledged
/// consuming the lockstep frame that carried the confirm intent. While pending,
/// stage-grid input is held neutral so the selected stage cannot drift before
/// the synchronized release.
void StageSelSync_ApplyConfirmAckGate(uint32_t frame,
                                      uint16_t* ioHeld,
                                      uint16_t* ioPressed,
                                      uint32_t remoteAckFrame,
                                      uint16_t sharedDelay,
                                      uint8_t stageCursor);


// ============================================================================
// Diagnostics
// ============================================================================

struct StageSelConfirmedFrame {
    uint32_t frame;
    uint16_t p1;
    uint16_t p2;
    uint16_t merged;
};

/// Get the most recent confirmed frame for diagnostic display.
bool StageSelSync_GetLastConfirmedFrame(StageSelConfirmedFrame* out);

/// Get total number of confirmed frames processed this session.
uint32_t StageSelSync_GetConfirmedFrameCount();

} // namespace Net
