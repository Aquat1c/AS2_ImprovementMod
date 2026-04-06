/**
 * Alice Senki 2 - Local Stress Test
 *
 * Uses GekkoNet's GekkoStressSession to validate savestate determinism
 * locally (both "players" run in the same process). This catches:
 *   - Save/restore correctness
 *   - Rollback resimulation producing identical state
 *   - Checksum mismatches (desync bugs)
 *
 * The stress test is local-only and requires the game to be in
 * Mode 8 Substate 3 (gameplay). It replaces live input with
 * random/constant test inputs and runs rollback events.
 */

#pragma once

#include <stdint.h>

namespace StressTest {

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    int      duration_frames;       // How many frames to run (0 = infinite until stopped)
    int      input_delay;           // Simulated input delay
    int      max_rollback;          // Max rollback window
    bool     random_inputs;         // true = random inputs, false = neutral
    uint32_t random_seed;           // Seed for random input generation
    bool     desync_detection;      // Enable checksum comparison
    int      check_distance;        // Checksum comparison frequency
};

// ============================================================================
// Results
// ============================================================================

struct Results {
    bool     running;
    bool     completed;
    bool     passed;                // true = no desyncs detected

    int      frames_simulated;
    int      save_count;
    int      load_count;
    int      advance_count;
    int      rollback_count;
    int      max_rollback_depth;
    int      desync_count;
    int      last_checked_frame;

    uint32_t last_save_checksum;

    // Last desync info
    int      desync_frame;
    uint32_t desync_local_checksum;
    uint32_t desync_remote_checksum;

    char     status[128];
};

// ============================================================================
// Lifecycle
// ============================================================================

// Start a stress test. Requires Mode 8 Substate 3.
bool Start(const Config* config);

// Stop a running stress test.
void Stop();

// Called each frame while stress test is running.
// Returns true if a frame should be simulated (FrameAdvance event).
bool FrameUpdate();

// ============================================================================
// Queries
// ============================================================================

bool IsRunning();
bool GetResults(Results* out);

// Default config with sensible values
Config GetDefaultConfig();

} // namespace StressTest
