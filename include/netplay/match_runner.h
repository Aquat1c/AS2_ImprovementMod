/**
 * Alice Senki 2 - Local Match Runner
 *
 * Runs a full local match with two custom AI players through the
 * GekkoNet StressSession rollback pipeline.  Unlike the drift stress
 * test (which feeds random button presses), the match runner uses a
 * lightweight deterministic AI that reads entity state from game
 * memory and produces fighting-game inputs (approach, attack, block,
 * jump, tech).
 *
 * The custom AI intentionally avoids calling the game's built-in
 * AI functions because those call rand() internally, which would
 * corrupt the RNG during rollback resimulation and cause desyncs.
 * Instead, the AI uses its own LCG seeded per-match and reads only
 * from game memory that is captured in savestates.
 *
 * All per-frame state is logged to a dedicated file (as2_matchrunner.log).
 * Requires Mode 8 Substate 3 (gameplay).
 */

#pragma once

#include <stdint.h>

namespace MatchRunner {

// ============================================================================
// AI Style
// ============================================================================

enum AIStyle {
    AI_RANDOM    = 0,   // Pure random (same as stress test)
    AI_AGGRO     = 1,   // Rushdown — always approach + attack
    AI_DEFENSIVE = 2,   // Block-heavy, punish on recovery
    AI_BALANCED  = 3,   // Mix of approach/attack/block/retreat
    AI_MIRROR    = 4,   // Copy opponent's last input (interesting for determinism tests)
};

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    // Gekko rollback settings
    int      input_delay;           // Simulated input delay (0-6)
    int      max_rollback;          // Max rollback window (1-12)
    int      check_distance;        // Checksum comparison frequency
    bool     desync_detection;      // Enable checksum comparison

    // AI settings
    AIStyle  p1_ai;                 // P1 AI style
    AIStyle  p2_ai;                 // P2 AI style
    uint32_t random_seed;           // Seed for AI RNG

    // Duration
    int      max_frames;            // 0 = run until match ends naturally
    int      max_rounds;            // 0 = unlimited, >0 = stop after N round transitions

    // Logging
    bool     log_per_frame;         // Log every frame (verbose)
    bool     log_rollbacks;         // Log rollback events
    bool     log_state_on_desync;   // Dump full state on desync
};

// ============================================================================
// Per-Player Snapshot (read from game memory each frame)
// ============================================================================

struct PlayerState {
    int16_t  hp;
    int16_t  meter;
    int16_t  x_pos;
    int16_t  y_pos;
    int8_t   facing;        // +1 = right, -1 = left
    uint32_t action_id;     // Current action/animation state
    uint16_t last_input;    // Last input bitmask we generated
};

// ============================================================================
// Results
// ============================================================================

struct Results {
    bool     running;
    bool     completed;
    bool     passed;                // true = no desyncs detected

    int      frames_simulated;
    int      rounds_completed;
    int      save_count;
    int      load_count;
    int      advance_count;
    int      rollback_count;
    int      max_rollback_depth;
    int      desync_count;

    // Last desync info
    int      desync_frame;
    uint32_t desync_local_checksum;
    uint32_t desync_remote_checksum;

    // Current player states (updated each advance)
    PlayerState p1;
    PlayerState p2;

    // Last checksum
    uint32_t last_save_checksum;
    int      last_checked_frame;

    char     status[160];
};

// ============================================================================
// Lifecycle
// ============================================================================

// Start a match run. Requires Mode 8 Substate 3.
bool Start(const Config* config);

// Stop a running match.
void Stop();

// Called each frame while running.
// Returns true if a frame should be simulated (AdvanceEvent emitted).
bool FrameUpdate();

// ============================================================================
// Queries
// ============================================================================

bool IsRunning();
bool GetResults(Results* out);

// Default config with sensible values
Config GetDefaultConfig();

// AI style name for UI
const char* GetAIStyleName(AIStyle style);

} // namespace MatchRunner
