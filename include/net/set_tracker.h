/**
 * Alice Senki 2 - Set Win Tracker
 *
 * Tracks wins per player across a set (multiple matches within one
 * session). Persists through rematch/return-to-CharSel. Resets when
 * the session ends or a new session begins.
 *
 * Uses game-side P1/P2 (winner byte at match_header+4) and maps to
 * local/remote via PlayerMapping.
 */

#pragma once

#include <stdint.h>

namespace Net {

struct SetTrackerSnapshot {
    int  local_wins;
    int  remote_wins;
    int  draws;
    int  total_matches;
};

/// Initialize. Safe to call multiple times.
void SetTracker_Init();

/// Record a match result from the game's winner byte.
/// winner: 0=P1 win, 1=P2 win, 2=draw, 0xFF=invalid.
/// Maps to local/remote via current PlayerMapping.
void SetTracker_RecordResult(uint8_t winner);

/// Reset all counts. Called when a new session begins.
void SetTracker_Reset();

/// Get current tracker state.
void SetTracker_GetSnapshot(SetTrackerSnapshot* out);

/// Get wins mapped to game-side P1/P2 slots.
void SetTracker_GetGameSideWins(int* p1_wins, int* p2_wins);

} // namespace Net
