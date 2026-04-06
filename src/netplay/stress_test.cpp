/**
 * Alice Senki 2 - Local Stress Test Implementation
 *
 * Runs GekkoNet StressSession locally (both players in one process).
 * Save/Load/Advance events exercise the savestate system and detect desyncs.
 * Driven from the Netplay menu tab; does NOT require a network connection.
 */

#include "stress_test.h"
#include "as2_rollback.h"
#include "log_window.h"

#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC
#endif
#include <gekkonet.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace StressTest {

// ============================================================================
// Constants
// ============================================================================

static constexpr int kMaxSavestateSlots = 16;

// ============================================================================
// Internal State
// ============================================================================

namespace {

static GekkoSession*     s_session       = nullptr;
static GekkoConfig       s_gekkoConfig   = {};
static Config            s_config        = {};
static Results           s_results       = {};
static bool              s_running       = false;

// Savestate ring buffer
static CompactSaveState_t* s_savestates  = nullptr;
static int               s_savestateCount = 0;

// Player handles
static int               s_p1Handle      = -1;
static int               s_p2Handle      = -1;

// RNG for random inputs
static uint32_t          s_inputRng      = 0;

// Maximum rollback depth tracker
static int               s_currentRollbackDepth = 0;

// ============================================================================
// Helpers
// ============================================================================

static void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(s_results.status, sizeof(s_results.status), _TRUNCATE, fmt, args);
    va_end(args);
}

static uint32_t NextRandom() {
    // Simple LCG (same as MSVC CRT)
    s_inputRng = s_inputRng * 214013u + 2531011u;
    return (s_inputRng >> 16) & 0x7FFF;
}

static uint16_t GenerateInput() {
    if (!s_config.random_inputs) {
        return 0; // Neutral input
    }
    // Generate a plausible fighting-game input:
    // Directions (bits 0-3) + buttons (bits 4-7)
    uint32_t r = NextRandom();
    uint16_t input = 0;

    // ~30% chance of a direction
    if ((r & 0xFF) < 77) {
        input |= (1 << (r & 3)); // One direction
    }

    // ~20% chance of a button
    r = NextRandom();
    if ((r & 0xFF) < 51) {
        input |= (1 << (4 + (r & 3))); // One button
    }

    return input;
}

// ============================================================================
// Event Handlers
// ============================================================================

static void HandleSaveEvent(const GekkoGameEvent* event) {
    int frame = event->data.save.frame;
    int slotIdx = frame % kMaxSavestateSlots;
    CompactSaveState_t* slot = &s_savestates[slotIdx];

    AS2_SaveStateToBuffer(slot);

    // Use the full deterministic savestate checksum rather than the narrow
    // quick checksum so the stress session validates rollback drift against the
    // same digest the rollback engine saves in each CompactSaveState_t.
    uint32_t checksum = slot->checksum;
    s_results.last_save_checksum = checksum;
    s_results.last_checked_frame = frame;

    if (event->data.save.checksum) {
        *(event->data.save.checksum) = checksum;
    }
    if (event->data.save.state_len) {
        *(event->data.save.state_len) = sizeof(CompactSaveState_t);
    }
    if (event->data.save.state) {
        memcpy(event->data.save.state, slot, sizeof(CompactSaveState_t));
    }

    s_results.save_count++;
}

static void HandleLoadEvent(const GekkoGameEvent* event) {
    int frame = event->data.load.frame;

    if (event->data.load.state && event->data.load.state_len >= sizeof(CompactSaveState_t)) {
        const CompactSaveState_t* state = (const CompactSaveState_t*)event->data.load.state;
        AS2_LoadStateFromBuffer(state);
    } else {
        int slotIdx = frame % kMaxSavestateSlots;
        AS2_LoadStateFromBuffer(&s_savestates[slotIdx]);
    }

    s_results.load_count++;
}

static void HandleAdvanceEvent(const GekkoGameEvent* event) {
    bool rolling_back = event->data.adv.rolling_back;
    const uint8_t* inputs = event->data.adv.inputs;
    unsigned int input_len = event->data.adv.input_len;

    if (rolling_back) {
        s_currentRollbackDepth++;
        if (s_currentRollbackDepth > s_results.max_rollback_depth) {
            s_results.max_rollback_depth = s_currentRollbackDepth;
        }
    } else {
        if (s_currentRollbackDepth > 0) {
            s_results.rollback_count++;
        }
        s_currentRollbackDepth = 0;
    }

    // Apply inputs
    if (input_len >= 4 && inputs) {
        uint16_t p1Input = *(const uint16_t*)(inputs);
        uint16_t p2Input = *(const uint16_t*)(inputs + 2);
        AS2_SetSynchronizedInputOverride(p1Input, p2Input);
    }

    s_results.advance_count++;
    s_results.frames_simulated = event->data.adv.frame;
}

static void HandleSessionEvents() {
    if (!s_session) return;

    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(s_session, &count);
    if (!events || count <= 0) return;

    for (int i = 0; i < count; i++) {
        GekkoSessionEvent* ev = events[i];
        if (!ev) continue;

        switch (ev->type) {
        case GekkoSessionStarted:
            LOG_NETPLAY(LOG_INFO, "[StressTest] GekkoNet session started");
            SetStatus("Running stress test...");
            break;

        case GekkoDesyncDetected:
            s_results.desync_count++;
            s_results.desync_frame = ev->data.desynced.frame;
            s_results.desync_local_checksum = ev->data.desynced.local_checksum;
            s_results.desync_remote_checksum = ev->data.desynced.remote_checksum;
            LOG_NETPLAY(LOG_ERROR,
                "[StressTest] DESYNC at frame %d! local=0x%08X remote=0x%08X",
                ev->data.desynced.frame,
                ev->data.desynced.local_checksum,
                ev->data.desynced.remote_checksum);
            SetStatus("DESYNC at frame %d!", ev->data.desynced.frame);
            break;

        case GekkoPlayerSyncing:
            SetStatus("Syncing... (%u/%u)",
                      ev->data.syncing.current, ev->data.syncing.max);
            break;

        default:
            break;
        }
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

Config GetDefaultConfig() {
    Config cfg = {};
    cfg.duration_frames   = 600;    // 10 seconds
    cfg.input_delay       = 0;
    cfg.max_rollback      = 8;
    cfg.random_inputs     = true;
    cfg.random_seed       = 12345;
    cfg.desync_detection  = true;
    cfg.check_distance    = 10;
    return cfg;
}

bool Start(const Config* config) {
    if (s_running) {
        LOG_NETPLAY(LOG_WARNING, "[StressTest] Already running");
        return false;
    }

    if (!config) {
        LOG_NETPLAY(LOG_ERROR, "[StressTest] Null config");
        return false;
    }

    // Require the interactive fighting window, not just the broad Substate 3 loop.
    if (!AS2_IsInPlayableGameplay()) {
        LOG_NETPLAY(LOG_ERROR, "[StressTest] Must be in interactive gameplay");
        SetStatus("ERROR: Not in interactive gameplay");
        return false;
    }

    // Store config
    memcpy(&s_config, config, sizeof(Config));

    // Reset results
    memset(&s_results, 0, sizeof(Results));
    s_results.running = true;
    s_currentRollbackDepth = 0;
    s_inputRng = config->random_seed;

    // Allocate savestate ring buffer
    s_savestates = (CompactSaveState_t*)calloc(kMaxSavestateSlots, sizeof(CompactSaveState_t));
    if (!s_savestates) {
        LOG_NETPLAY(LOG_ERROR, "[StressTest] Failed to allocate savestate buffer");
        SetStatus("ERROR: Memory allocation failed");
        s_results.running = false;
        return false;
    }
    s_savestateCount = kMaxSavestateSlots;

    // Create GekkoNet stress session
    if (!gekko_create(&s_session, GekkoStressSession)) {
        LOG_NETPLAY(LOG_ERROR, "[StressTest] gekko_create(GekkoStressSession) failed");
        SetStatus("ERROR: Failed to create stress session");
        free(s_savestates);
        s_savestates = nullptr;
        s_results.running = false;
        return false;
    }

    // Configure
    memset(&s_gekkoConfig, 0, sizeof(s_gekkoConfig));
    s_gekkoConfig.num_players = 2;
    s_gekkoConfig.max_spectators = 0;
    s_gekkoConfig.input_prediction_window = config->max_rollback > 0 ? config->max_rollback : 8;
    s_gekkoConfig.spectator_delay = 0;
    s_gekkoConfig.input_size = sizeof(uint16_t);
    s_gekkoConfig.state_size = sizeof(CompactSaveState_t);
    s_gekkoConfig.limited_saving = false;
    s_gekkoConfig.desync_detection = config->desync_detection;
    s_gekkoConfig.check_distance = config->check_distance > 0 ? config->check_distance : 10;

    gekko_start(s_session, &s_gekkoConfig);

    // Add both players as local (stress test runs both sides)
    GekkoNetAddress dummyAddr = {};
    s_p1Handle = gekko_add_actor(s_session, GekkoLocalPlayer, &dummyAddr);
    s_p2Handle = gekko_add_actor(s_session, GekkoLocalPlayer, &dummyAddr);

    if (s_p1Handle < 0 || s_p2Handle < 0) {
        LOG_NETPLAY(LOG_ERROR, "[StressTest] Failed to add players (p1=%d, p2=%d)",
                    s_p1Handle, s_p2Handle);
        gekko_destroy(&s_session);
        free(s_savestates);
        s_savestates = nullptr;
        s_results.running = false;
        return false;
    }

    // Set input delay for both players
    int delay = config->input_delay;
    if (delay < 0) delay = 0;
    if (delay > 15) delay = 15;
    gekko_set_local_delay(s_session, s_p1Handle, (unsigned char)delay);
    gekko_set_local_delay(s_session, s_p2Handle, (unsigned char)delay);

    s_running = true;
    SetStatus("Drift test started (frames=%d, check=%d, random=%s)",
              config->duration_frames,
              s_gekkoConfig.check_distance,
              config->random_inputs ? "yes" : "no");

    LOG_NETPLAY(LOG_INFO,
        "[StressTest] Started: frames=%d delay=%d maxRB=%d check=%u random=%s seed=%u",
        config->duration_frames, delay, config->max_rollback,
        s_gekkoConfig.check_distance,
        config->random_inputs ? "yes" : "no", config->random_seed);

    return true;
}

void Stop() {
    if (!s_running) return;

    if (s_session) {
        gekko_destroy(&s_session);
        s_session = nullptr;
    }

    if (s_savestates) {
        free(s_savestates);
        s_savestates = nullptr;
    }
    s_savestateCount = 0;

    AS2_ClearSynchronizedInputOverride();

    s_running = false;
    s_results.running = false;
    s_results.completed = true;
    s_results.passed = (s_results.desync_count == 0);

    s_p1Handle = -1;
    s_p2Handle = -1;

    if (s_results.passed) {
        SetStatus("PASSED: %d frames, %d saves, %d loads, %d rollbacks",
                  s_results.frames_simulated, s_results.save_count,
                  s_results.load_count, s_results.rollback_count);
    } else {
        SetStatus("FAILED: %d desyncs detected (last at frame %d)",
                  s_results.desync_count, s_results.desync_frame);
    }

    LOG_NETPLAY(LOG_INFO,
        "[StressTest] Stopped. result=%s frames=%d saves=%d loads=%d rollbacks=%d desyncs=%d maxDepth=%d",
        s_results.passed ? "PASS" : "FAIL",
        s_results.frames_simulated, s_results.save_count, s_results.load_count,
        s_results.rollback_count, s_results.desync_count, s_results.max_rollback_depth);
}

bool FrameUpdate() {
    if (!s_running || !s_session) return false;

    // Check duration limit
    if (s_config.duration_frames > 0 &&
        s_results.frames_simulated >= s_config.duration_frames) {
        Stop();
        return false;
    }

    // Generate and feed inputs for both local players
    uint16_t p1Input = GenerateInput();
    uint16_t p2Input = GenerateInput();

    gekko_add_local_input(s_session, s_p1Handle, &p1Input);
    gekko_add_local_input(s_session, s_p2Handle, &p2Input);

    // Update session — returns game events
    int eventCount = 0;
    GekkoGameEvent** events = gekko_update_session(s_session, &eventCount);

    // Process session-level events (desync, connected, etc.)
    HandleSessionEvents();

    // Process game events
    bool hadAdvance = false;
    if (events) {
        for (int i = 0; i < eventCount; i++) {
            GekkoGameEvent* ev = events[i];
            if (!ev) continue;

            switch (ev->type) {
            case GekkoSaveEvent:
                HandleSaveEvent(ev);
                break;
            case GekkoLoadEvent:
                HandleLoadEvent(ev);
                break;
            case GekkoAdvanceEvent:
                HandleAdvanceEvent(ev);
                hadAdvance = true;
                break;
            default:
                break;
            }
        }
    }

    return hadAdvance;
}

bool IsRunning() {
    return s_running;
}

bool GetResults(Results* out) {
    if (!out) return false;
    memcpy(out, &s_results, sizeof(Results));
    return true;
}

} // namespace StressTest
