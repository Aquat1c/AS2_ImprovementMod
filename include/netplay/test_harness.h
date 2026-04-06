/**
 * Alice Senki 2 - Automated Test Harness
 *
 * Reads a config file (as2_autoconnect.cfg) on startup and automatically:
 *   1. Starts a host or client session
 *   2. Navigates to character select
 *   3. Auto-selects characters
 *   4. Begins a match with full profiling and logging
 *
 * Deep game-state profiling monitors:
 *   - Frame counters (sim, display, write, net)
 *   - Entity state (HP, meter, position, action, facing)
 *   - RNG seed tracking
 *   - Input buffer integrity
 *   - Vanilla netplay flag monitoring (should be 0 during mod sessions)
 *   - Save/load/advance microsecond timing
 *   - Rollback depth and frequency
 *   - Per-frame state checksums (CRC32)
 *   - RTT and packet statistics
 *
 * Network condition simulation (latency, jitter, packet loss) is also
 * configurable via the same file.
 *
 * The companion launcher (as2_test_harness.exe) writes two config files
 * and spawns two game instances to run a fully automated test.
 */

#pragma once

#include "harness_shared_memory.h"
#include <stdint.h>

namespace TestHarness {

// ============================================================================
// Config read from as2_autoconnect.cfg
// ============================================================================

struct NetSimConfig {
    int     latency_ms;
    int     jitter_ms;
    float   packet_loss_pct;
    float   duplicate_pct;
    float   timesync_threshold;  // Override for kTimesyncThreshold (0 = use default 0.5)
};

struct ProfilingConfig {
    bool    enabled;
    char    log_file[260];
    bool    log_inputs;
    bool    log_checksums;
    bool    log_rollbacks;
    bool    log_frame_timing;
    bool    log_rtt;
    bool    log_game_state;     // Log full game state snapshots
    bool    log_entity_detail;  // Log entity HP/meter/pos/action per frame
    bool    log_rng;            // Log RNG seed changes
    bool    log_vanilla_flags;  // Log vanilla netplay flag state (should be 0)
    bool    log_stage_debug;    // Log detailed CharSel/stage-preview telemetry
};

struct AutoConnectConfig {
    bool    enabled;
    bool    is_host;
    char    nickname[21];
    uint16_t listen_port;
    char    target_ip[64];
    uint16_t target_port;
    int     character_id;
    int     palette;
    int     delay_frames;
    int     match_duration_sec;
    bool    stage_mash_test;
    int     stage_mash_frames;

    NetSimConfig    net_sim;
    ProfilingConfig profiling;
};

// ============================================================================
// Deep profiler snapshot (for ImGui display)
// ============================================================================

struct FrameSnapshot {
    uint32_t frame;
    uint16_t p1_input;
    uint16_t p2_input;
    int16_t  p1_hp;
    int16_t  p2_hp;
    uint16_t p1_meter;
    uint16_t p2_meter;
    int16_t  p1_x, p1_y;
    int16_t  p2_x, p2_y;
    uint32_t p1_action;
    uint32_t p2_action;
    uint32_t rng_seed;
    uint32_t checksum;
    uint32_t game_mode;
    uint32_t sub_state;
    uint32_t stage_id;
    uint32_t game_type;
    // Frame counters
    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t write_frame;
    uint32_t net_frame;
    // Vanilla flags (should be 0)
    uint8_t  vanilla_role;
    uint8_t  vanilla_connected;
};

struct ProfileStats {
    // Counters
    uint32_t total_frames;
    uint32_t total_rollbacks;
    uint32_t max_rollback_depth;
    uint32_t total_saves;
    uint32_t total_loads;
    uint32_t desync_warnings;

    // Timing (microseconds, running averages)
    float    avg_save_us;
    float    avg_load_us;
    float    avg_advance_us;
    float    peak_save_us;
    float    peak_load_us;
    float    peak_advance_us;

    // Network
    float    avg_rtt_ms;
    float    peak_rtt_ms;
    uint32_t packets_sent;
    uint32_t packets_received;

    // Game state anomalies
    uint32_t vanilla_flag_violations;  // Times vanilla flags were non-zero
    uint32_t rng_jumps;                // Unexpected RNG seed changes
    uint32_t frame_counter_anomalies;  // sim != display+1 etc
};

// ============================================================================
// Public API (called from as2_rollback.cpp)
// ============================================================================

bool Init();
void FrameUpdate();
void Shutdown();
bool IsActive();
bool InMatch();

// Net sim accessors
float GetTimesyncThreshold();  // Returns override or 0.0f if not set

// ============================================================================
// Profiling API (called from hooks to record data)
// ============================================================================

void RecordFrameTiming(uint32_t frame, int saveUs, int loadUs, int advanceUs);
void RecordRollback(uint32_t frame, int depth, int replayFrames);
void RecordInputs(uint32_t frame, uint16_t p1, uint16_t p2);
void RecordChecksum(uint32_t frame, uint32_t checksum);
void RecordRtt(uint32_t frame, float rttMs);
void RecordDesync(uint32_t frame, uint32_t localChecksum, uint32_t remoteChecksum, const char* source);
void RecordStageEvent(const char* source, uint32_t gameFrame, int32_t lockstepFrame,
                      uint32_t mode, uint32_t subState, uint8_t stageId,
                      uint16_t localInput, uint16_t remoteInput,
                      uint16_t p1Input, uint16_t p2Input,
                      uint16_t sharedInput,
                      uint16_t appliedP1, uint16_t appliedP2);
void FlushLog();

} // namespace TestHarness
