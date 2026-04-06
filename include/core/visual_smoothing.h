/**
 * Alice Senki 2 - Visual Rollback Smoothing
 *
 * Prevents visual position snapping after rollback corrections by
 * temporarily writing interpolated positions to entity memory during
 * the render phase.
 *
 * Lifecycle:
 *   1. OnRollbackEnd() — flags that a rollback just completed
 *   2. ApplyBeforeRender() — from Hook_AdvanceFrame (between sim and draw):
 *      during playable gameplay only, reads sim positions, computes visual
 *      offset, writes smoothed positions
 *   3. RestoreAfterRender() — from Hook_RenderPresent (after draw):
 *      restores original sim positions to entity memory
 *
 * Entity positions are only modified during the brief render window.
 * Save/Load events always see correct simulation positions.
 */

#pragma once

#include <stdint.h>

namespace VisualSmoothing {

// Called at match start / session creation to reset state
void Reset();

// Signal that a rollback sequence just completed (IsRollingBack went true→false).
// Called from Hook_InputDispatcher's FrameAdvance path.
void OnRollbackEnd();

// Apply visual offsets to entity positions before game draws.
// Called from Hook_AdvanceFrame (after sim, before render) and self-gates to
// the playable Mode 8 gameplay window so intro / transition phases are not
// visually distorted by stale rollback offsets.
void ApplyBeforeRender();

// Restore simulation positions after game draws.
// Called from Hook_RenderPresent (after Present/draw).
void RestoreAfterRender();

struct Stats {
    uint32_t smoothing_frames;    // Frames where a non-zero offset was applied
    uint32_t rollback_events;     // Times a rollback-end offset was computed
    float    max_offset_seen;     // Largest offset magnitude (game coords)
    float    current_offset_p1;   // Current P1 offset magnitude
    float    current_offset_p2;   // Current P2 offset magnitude
};

void GetStats(Stats* out);

} // namespace VisualSmoothing
