/**
 * Alice Senki 2 - Set Win Tracker Implementation
 *
 * Adapted from old netplay win tracking (AS2_RecordMatchResult).
 * Maps game-side P1/P2 winner byte to local/remote via PlayerMapping.
 */

#include "net/set_tracker.h"
#include "net/player_side_mapping.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <string.h>

namespace Net {

// ============================================================================
// Internal State
// ============================================================================

static int  s_localWins   = 0;
static int  s_remoteWins  = 0;
static int  s_draws       = 0;
static int  s_totalMatches = 0;
static bool s_initialized = false;

// ============================================================================
// Public API
// ============================================================================

void SetTracker_Init() {
    s_localWins = 0;
    s_remoteWins = 0;
    s_draws = 0;
    s_totalMatches = 0;
    s_initialized = true;
}

void SetTracker_RecordResult(uint8_t winner) {
    int localSlot = PlayerMapping_GetLocalGameSlot();

    if (winner == 0 || winner == 1) {
        int winnerSlot = (int)winner;  // 0=P1, 1=P2
        if (winnerSlot == localSlot) {
            s_localWins++;
        } else {
            s_remoteWins++;
        }
        s_totalMatches++;

        Rollback::NetplayLog_Write("SET", -1,
            "Match result: P%d wins (local=%s) | Score: local=%d remote=%d (matches=%d)",
            winner + 1,
            (winnerSlot == localSlot) ? "LOCAL WIN" : "REMOTE WIN",
            s_localWins, s_remoteWins, s_totalMatches);
        LOG_INFO("[SetTracker] P%d wins → local=%d remote=%d (total=%d)",
            winner + 1, s_localWins, s_remoteWins, s_totalMatches);

    } else if (winner == 2) {
        s_draws++;
        s_totalMatches++;

        Rollback::NetplayLog_Write("SET", -1,
            "Match result: DRAW | Score: local=%d remote=%d draws=%d (matches=%d)",
            s_localWins, s_remoteWins, s_draws, s_totalMatches);
        LOG_INFO("[SetTracker] Draw → local=%d remote=%d draws=%d (total=%d)",
            s_localWins, s_remoteWins, s_draws, s_totalMatches);

    } else {
        // 0xFF or other = no valid result
        Rollback::NetplayLog_Write("SET", -1,
            "Match result: INVALID (winner=0x%02X) — not counted", winner);
        LOG_WARN("[SetTracker] Invalid winner byte 0x%02X — ignoring", winner);
    }
}

void SetTracker_Reset() {
    if (s_totalMatches > 0) {
        Rollback::NetplayLog_Write("SET", -1,
            "Set tracker RESET (was local=%d remote=%d draws=%d matches=%d)",
            s_localWins, s_remoteWins, s_draws, s_totalMatches);
        LOG_INFO("[SetTracker] Reset (was %d-%d, %d draws, %d matches)",
            s_localWins, s_remoteWins, s_draws, s_totalMatches);
    }
    s_localWins = 0;
    s_remoteWins = 0;
    s_draws = 0;
    s_totalMatches = 0;
}

void SetTracker_GetSnapshot(SetTrackerSnapshot* out) {
    if (!out) return;
    out->local_wins = s_localWins;
    out->remote_wins = s_remoteWins;
    out->draws = s_draws;
    out->total_matches = s_totalMatches;
}

void SetTracker_GetGameSideWins(int* p1_wins, int* p2_wins) {
    int localSlot = PlayerMapping_GetLocalGameSlot();
    // Before bootstrap assigns the slot, mirror the menu controller's fallback:
    // host is game P1, client is game P2.
    if (localSlot != 0 && localSlot != 1) {
        SessionSnapshot snap{};
        Session_GetSnapshot(&snap);
        localSlot = (snap.role == SessionRole::Host) ? 0 : 1;
    }
    if (localSlot == 0) {
        // Local is P1
        if (p1_wins) *p1_wins = s_localWins;
        if (p2_wins) *p2_wins = s_remoteWins;
    } else {
        // Local is P2
        if (p1_wins) *p1_wins = s_remoteWins;
        if (p2_wins) *p2_wins = s_localWins;
    }
}

} // namespace Net
