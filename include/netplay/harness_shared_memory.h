/**
 * Alice Senki 2 - Test Harness Shared Memory
 *
 * IPC layout shared between the mod DLL (writer) and the test harness
 * launcher exe (reader).  Each game instance maps one slot; the launcher
 * opens both slots to show a dual-panel dashboard.
 *
 * Named objects (Local\ namespace):
 *   "Local\\AS2_Harness_Host"    – slot for the host instance
 *   "Local\\AS2_Harness_Client"  – slot for the client instance
 *
 * This header is self-contained – no external includes required.
 */

#pragma once

#include <stdint.h>

// ============================================================================
// Constants
// ============================================================================

#define HARNESS_SHM_MAGIC       0x48524E53  // "HRNS"
#define HARNESS_SHM_VERSION     5
#define HARNESS_SHM_NAME_HOST   "Local\\AS2_Harness_Host"
#define HARNESS_SHM_NAME_CLIENT "Local\\AS2_Harness_Client"

#define HARNESS_LOG_LINES       128
#define HARNESS_LOG_LINE_LEN    256

// ============================================================================
// Shared data layout  (writer: mod DLL  /  reader: launcher exe)
// ============================================================================

struct HarnessSharedData {
    // ---- Header (validated on open) ----
    uint32_t magic;          // HARNESS_SHM_MAGIC
    uint32_t version;        // HARNESS_SHM_VERSION
    volatile uint32_t write_seq; // Incremented after every coherent write

    // ---- Identity ----
    uint8_t  active;
    uint8_t  is_host;
    uint8_t  _pad0[2];
    char     nickname[24];

    // ---- Phase / State Machine ----
    uint32_t phase;          // Phase enum ordinal
    uint32_t frame_counter;
    char     phase_name[32];

    // ---- Session / Connection ----
    uint32_t session_state;  // SessionManager::State ordinal
    float    conn_rtt_ms;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t desync_count;
    char     peer_nickname[24];
    char     status_text[128];
    char     error_text[128];

    // ---- Game State (from latest snapshot) ----
    uint32_t game_mode;
    uint32_t sub_state;
    uint32_t stage_id;
    uint32_t game_type;
    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t write_frame;
    uint32_t net_frame;
    uint8_t  vanilla_role;
    uint8_t  vanilla_connected;
    uint8_t  _pad1[2];
    uint32_t rng_seed;
    uint32_t checksum;

    // ---- Entity State ----
    int16_t  p1_hp;
    int16_t  p2_hp;
    uint16_t p1_meter;
    uint16_t p2_meter;
    int16_t  p1_x, p1_y;
    int16_t  p2_x, p2_y;
    uint32_t p1_action;
    uint32_t p2_action;
    uint16_t p1_input;
    uint16_t p2_input;

    // ---- Accumulated Profile Stats ----
    uint32_t total_frames;
    uint32_t total_rollbacks;
    uint32_t max_rollback_depth;
    uint32_t total_saves;
    uint32_t total_loads;
    uint32_t desync_warnings;

    float    avg_save_us;
    float    peak_save_us;
    float    avg_load_us;
    float    peak_load_us;
    float    avg_advance_us;
    float    peak_advance_us;

    float    avg_rtt_ms;
    float    peak_rtt_ms;

    uint32_t vanilla_flag_violations;
    uint32_t rng_jumps;
    uint32_t frame_counter_anomalies;

    // ---- GekkoNet Rollback State ----
    int32_t  rb_local_frame;      // GekkoNet local frame (this side's perspective)
    int32_t  rb_remote_frame;     // GekkoNet remote frame estimate
    float    rb_frames_ahead;     // How far ahead of opponent
    uint32_t rb_state;            // RollbackSession::State ordinal
    uint32_t rb_advance_count;    // Total frames advanced by GekkoNet
    uint32_t rb_adapter_send;     // Packets sent via GekkoNet adapter
    uint32_t rb_adapter_recv;     // Packets received via GekkoNet adapter
    uint32_t rb_timesync_skips;   // Frames skipped by timesync throttle

    // ---- Network Simulation Config ----
    int32_t  sim_latency_ms;
    int32_t  sim_jitter_ms;
    float    sim_loss_pct;
    float    sim_dup_pct;

    // ---- Session Log Directory ----
    char     session_log_dir[260]; // Full path to session log directory (e.g. logs/2026-04-05_18-42-06/)
    char     rollback_trace_log[260];
    char     rollback_trace_bin[260];

    // ---- Log Ring Buffer ----
    uint32_t log_head;   // Next write position (mod increments)
    uint32_t log_count;  // Total lines ever written
    char     log_lines[HARNESS_LOG_LINES][HARNESS_LOG_LINE_LEN];
};

// Sanity: keep total shared memory under 64 KB
static_assert(sizeof(HarnessSharedData) < 65536,
              "HarnessSharedData exceeds 64 KB budget");
