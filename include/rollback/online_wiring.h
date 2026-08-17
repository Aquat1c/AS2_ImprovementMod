/**
 * Alice Senki 2 - Match Director (re0.7 M6, plan §2.6)
 *
 * The match director behind the PRESERVED `OnlineWiring_*` facade
 * (inventory §2.3 — all 13 functions keep their signatures; implementation
 * lives in src/rollback/match_director.cpp since M6, online_wiring.cpp is
 * deleted). Thin by design; owns ordering, not policy:
 *
 *   1. GameplayStart commit → engine arm (first match) / RotateEpoch
 *      (rematch — the engine survives match boundaries, §2.6.5)
 *   2. InputStream packets → RollbackSession_OnInputStreamPacket
 *   3. Lifecycle transitions → suspend-between-matches / director teardown
 *   4. INV-9 match-end ladder gate: WinScreenExit → PostMatchDecision →
 *      EpochAlign consume strictly in order (held + re-acked, never skipped)
 *   5. match_exit_pending → engine exact-input window (§2.7.6, INV-25)
 *   6. Disconnect → the single ordered teardown fan-out (§2.6.4)
 */

#pragma once

#include <stdint.h>

#include "net/netplay_phase_runtime.h"
#include "net/protocol.h"

namespace Rollback {

// ============================================================================
// Lifecycle
// ============================================================================

void OnlineWiring_Init();
void OnlineWiring_Shutdown();

// ============================================================================
// Per-Frame (called from ModOnFrame after lifecycle/policy updates)
// ============================================================================

void OnlineWiring_FrameUpdate();

/// Returns true if the rollback session is active and match runtime should advance.
bool OnlineWiring_IsGameplayActive();

/// Returns true once startup gameplay-entry barrier release is complete
/// (both peers reached the first post-intro interactive boundary and
/// mutually acknowledged readiness).
bool OnlineWiring_IsStartupReleased();

/// Returns true when first interactive gameplay advancement must still be held
/// by the startup gameplay-entry barrier (mutual ready+ack or rollback start
/// handoff still incomplete).
bool OnlineWiring_IsGameplayEntryAdvanceBlocked();

// ============================================================================
// Events
// ============================================================================

/// Called when pregame bootstrap hands the match to the lifecycle layer.
/// Starts the rollback session from the agreed baseline if not already active.
void OnlineWiring_OnGameplayStart();

/// Called when MatchLifecycle leaves PlayableGameplay.
/// Pauses/suspends rollback (does NOT destroy session for pause/transition).
void OnlineWiring_OnGameplayPause(const char* reason);

/// Called when match ends (MatchEnd phase reached).
void OnlineWiring_OnMatchEnd();

/// Called on disconnect from any state.
void OnlineWiring_OnDisconnect(const char* reason);

/// Called for post-match rematch (returning to CharSel).
void OnlineWiring_OnRematch();

/// Called for post-match return to session menu.
void OnlineWiring_OnReturnToSession();

// ============================================================================
// Packet sinks (called by Net::PacketRouter_OnPacket — M0 extraction, M3 promotion)
// ============================================================================

/// Engine input stream packet (InputStream, id 23 — raw Gekko data under
/// AS2_WITH_GEKKO=ON, v2 InputStreamPayload under engine2). Feeds the
/// rollback session when active. (The startup gameplay-entry barrier rides
/// TransitionBarrier kind GameplayStart since M5 — GekkoReady is retired.)
void OnlineWiring_HandleEngineDataPacket(const void* payload, size_t payloadLen);

// ============================================================================
// Match-end barrier ladder (M6, INV-9 / §4.4)
// ============================================================================

/// Strict ladder gate: consuming a later-step commit is refused until the
/// earlier steps have committed/consumed locally. Kinds outside the ladder
/// (or an unarmed ladder — e.g. session-start EpochAlign) always pass.
/// The barrier primitive keeps re-acking a held commit, so refusal = held,
/// never skipped (the exact inversion that killed the field session).
bool OnlineWiring_MatchEndLadderAllows(Net::NetTransitionKind kind);

/// The consumer of a ladder-step commit reports it so later steps unlock.
/// EpochAlign consumption completes the ladder (leftover WinScreenExit /
/// PostMatchDecision slots are retired so they can never satisfy the next
/// boundary).
void OnlineWiring_MatchEndLadderNotifyConsumed(Net::NetTransitionKind kind);

/// M7 (F-7): continue_flow registers the LOCKSTEP-DERIVED post-match intent
/// (PostMatchIntentWire: Rematch for YES,YES, CharselRestart for any NO) at
/// resolution. Both peers compute the resolution from the same consumed
/// lockstep stream, so a remote PostMatchDecision proposal carrying the
/// OTHER lockstep-derived value can only mean divergent streams — the
/// director fails closed (ProtocolViolation terminal). User-action intents
/// (ReturnToSession / Disconnect from the post-match menu) are exempt: they
/// are choices, not derivations. The expectation is cleared when the ladder
/// disarms.
void OnlineWiring_SetExpectedPostMatchIntent(uint8_t intentWire);

// ============================================================================
// Diagnostics
// ============================================================================

struct OnlineWiringSnapshot {
    bool     rollback_started;       // Has rollback session ever started this match
    bool     rollback_active;        // Is rollback session currently active
    bool     gameplay_active;        // Are we in playable gameplay right now
    bool     session_running;
    bool     stepping_enabled;
    bool     startup_barrier_armed;
    bool     startup_barrier_released;
    bool     lockstep_owner_active;
    Net::MatchRollbackPhase phase;
    float    target_tick_scale;
    float    current_tick_scale;
    int32_t  frame_origin_abs;       // Absolute engine frame where rollback rb_frame 0 begins
    uint32_t baseline_crc;           // Baseline CRC from bootstrap
    uint32_t config_hash;            // Config hash from pregame
    int      handoff_delay;          // Active delay at gameplay handoff
    int      handoff_budget;         // Rollback budget at handoff
    int      remote_announced_delay;
    int      stall_threshold;
    int      remote_inputs_received; // Total remote inputs processed
    int      packets_dispatched;     // Total gameplay packets dispatched
};

void OnlineWiring_GetSnapshot(OnlineWiringSnapshot* out);

} // namespace Rollback
