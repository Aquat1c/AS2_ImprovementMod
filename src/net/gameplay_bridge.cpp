/**
 * Alice Senki 2 - Gameplay Bridge Implementation
 *
 * The gameplay-runtime integration layer for the mod-owned rollback path.
 * This bridge is the single entry point for all gameplay session operations:
 *
 *   - Session start/stop (owns lifecycle contract)
 *   - Per-frame update (drives RollbackSession_FrameUpdate + delay consumption)
 *   - Remote input submission (routes to rollback session input timeline)
 *   - Player side mapping (sets/verifies at session start, clears at end)
 *   - Delay policy consumption (marks active delay as applied)
 *
 * GekkoNet library availability is probed at init for future integration
 * readiness. GekkoNet does NOT currently participate in gameplay runtime.
 */

#include "net/gameplay_bridge.h"
#include "net/player_side_mapping.h"
#include "net/delay_policy.h"
#include "net/session_manager.h"
#include "rollback/rollback_session.h"
#include "rollback/resimulation.h"
#include "rollback/netplay_log.h"
#include "patches/input_sync_hooks.h"
#include "patches/tick_hooks.h"
#include "ui/log_window.h"

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// GekkoNet future-integration readiness probe
// ============================================================================
// GekkoNet library is NOT used for gameplay. These declarations exist only
// to detect whether the library is linkable, for future migration planning.
#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC
#endif

struct GekkoSession;
enum GekkoSessionType : int;

extern "C" {
    __declspec(selectany) bool (*gekko_create_ptr)(GekkoSession**, int) = nullptr;
    __declspec(selectany) bool (*gekko_destroy_ptr)(GekkoSession**) = nullptr;
}

namespace Net {

// ============================================================================
// Internal State
// ============================================================================

static bool s_initialized       = false;
static bool s_gekkoNetAvailable  = false;   // Future integration readiness only
static bool s_sessionActive     = false;
static char s_status[128]       = "GameplayBridge: not initialized.";
static char s_gekkoNetVersion[48] = "v20260316133147-7f1f19e";

// Per-session counters for runtime verification
static int  s_framesUpdated         = 0;
static int  s_remoteInputsSubmitted = 0;
static int  s_delayPolicyConsumed   = 0;
static int  s_lastConsumedDelay     = -1;
static int  s_timesyncHardWaitFrames = 0;
static int  s_timesyncSoftFrames     = 0;
static int  s_timesyncPredictedFrames = 0;
static float s_runtimeTickScale      = 1.0f;

enum class TimesyncMode : uint8_t {
    Normal = 0,
    SoftSlow,
    HardFreeze,
};

struct TimesyncDecision {
    TimesyncMode mode;
    float tick_scale;
    int predicted_frames;
    int soft_threshold;
    int hard_threshold;
    float rtt_ms;
    float jitter_ms;
    int recommended_delay;
    bool measurement_valid;
};

static TimesyncMode s_timesyncMode = TimesyncMode::Normal;

static void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, args);
    va_end(args);
}

static const char* TimesyncModeName(TimesyncMode mode) {
    switch (mode) {
        case TimesyncMode::Normal:     return "Normal";
        case TimesyncMode::SoftSlow:   return "SoftSlow";
        case TimesyncMode::HardFreeze: return "HardFreeze";
        default:                       return "Unknown";
    }
}

static void ResetRuntimePacing() {
    InputSyncHooks_SetTimesyncFreeze(false);
    SetNetplayTickScale(1.0f);

    s_timesyncMode = TimesyncMode::Normal;
    s_runtimeTickScale = 1.0f;
    s_timesyncPredictedFrames = 0;
    s_timesyncHardWaitFrames = 0;
    s_timesyncSoftFrames = 0;
}

static TimesyncDecision ComputeTimesyncDecision() {
    TimesyncDecision out{};
    out.mode = TimesyncMode::Normal;
    out.tick_scale = 1.0f;

    Rollback::RollbackSessionSnapshot rbSnap{};
    Rollback::RollbackSession_GetSnapshot(&rbSnap);

    NetworkMeasurement measurement{};
    DelayPolicy_GetMeasurement(&measurement);

    out.predicted_frames = rbSnap.predicted_frames_outstanding;
    if (out.predicted_frames < 0) out.predicted_frames = 0;

    out.rtt_ms = measurement.rtt_ms;
    out.jitter_ms = measurement.jitter_ms;
    out.recommended_delay = measurement.recommended_delay;
    out.measurement_valid = measurement.valid;

    int jitterFrames = 0;
    if (measurement.valid) {
        jitterFrames = (int)ceilf((measurement.jitter_ms + measurement.rtt_variance_ms) / FRAME_TIME_MS);
    }
    if (jitterFrames < 0) jitterFrames = 0;

    out.soft_threshold = 1 + (std::min)(jitterFrames, 2);

    int hardThreshold = out.soft_threshold + 2;
    const int maxSafeThreshold = (std::max)(2, rbSnap.rollback_budget - 1);
    if (hardThreshold > maxSafeThreshold) hardThreshold = maxSafeThreshold;
    if (hardThreshold <= out.soft_threshold) hardThreshold = out.soft_threshold + 1;
    if (hardThreshold > rbSnap.rollback_budget) hardThreshold = rbSnap.rollback_budget;
    if (hardThreshold < 2) hardThreshold = 2;
    out.hard_threshold = hardThreshold;

    if (out.predicted_frames >= out.hard_threshold && rbSnap.current_frame >= out.hard_threshold) {
        out.mode = TimesyncMode::HardFreeze;
        return out;
    }

    if (out.predicted_frames > out.soft_threshold) {
        const int over = out.predicted_frames - out.soft_threshold;
        out.tick_scale = 1.0f - (0.10f * (float)over);
        if (out.tick_scale < 0.70f) out.tick_scale = 0.70f;
        out.mode = TimesyncMode::SoftSlow;
    }

    return out;
}

static void ApplyTimesyncDecision(const TimesyncDecision& decision, int32_t frame) {
    const TimesyncMode previousMode = s_timesyncMode;
    const float previousScale = s_runtimeTickScale;

    s_timesyncMode = decision.mode;
    s_runtimeTickScale = decision.tick_scale;
    s_timesyncPredictedFrames = decision.predicted_frames;

    SetNetplayTickScale(decision.tick_scale);
    InputSyncHooks_SetTimesyncFreeze(decision.mode == TimesyncMode::HardFreeze);

    if (previousMode != decision.mode) {
        Rollback::NetplayLog_StateChange(
            "TSYNC", frame,
            "mode",
            TimesyncModeName(previousMode),
            TimesyncModeName(decision.mode),
            "bridge pacing decision"
        );
    }

    if (fabsf(previousScale - decision.tick_scale) > 0.01f) {
        Rollback::NetplayLog_Write("TSYNC", frame,
            "tick_scale %.2f -> %.2f predicted=%d soft=%d hard=%d rtt=%.1f jitter=%.1f rec_delay=%d valid=%d",
            previousScale,
            decision.tick_scale,
            decision.predicted_frames,
            decision.soft_threshold,
            decision.hard_threshold,
            decision.rtt_ms,
            decision.jitter_ms,
            decision.recommended_delay,
            decision.measurement_valid ? 1 : 0);
    } else {
        Rollback::NetplayLog_Verbose("TSYNC", frame,
            "mode=%s predicted=%d soft=%d hard=%d tick_scale=%.2f rtt=%.1f jitter=%.1f rec_delay=%d valid=%d",
            TimesyncModeName(decision.mode),
            decision.predicted_frames,
            decision.soft_threshold,
            decision.hard_threshold,
            decision.tick_scale,
            decision.rtt_ms,
            decision.jitter_ms,
            decision.recommended_delay,
            decision.measurement_valid ? 1 : 0);
    }
}

// ============================================================================
// GekkoNet Availability Probe (future integration only)
// ============================================================================

/// Check if GekkoNet library is linkable. This does NOT affect gameplay —
/// all rollback is mod-owned regardless of this result.
static bool ProbeGekkoNetAvailability() {
#ifdef GEKKONET_AVAILABLE
    GekkoSession* session = nullptr;
    if (!gekko_create(&session, GekkoStressSession)) {
        LOG_WARN("[GameplayBridge] GekkoNet probe: gekko_create failed");
        return false;
    }
    if (!gekko_destroy(&session)) {
        LOG_WARN("[GameplayBridge] GekkoNet probe: gekko_destroy failed");
        return false;
    }
    return true;
#else
    return false;  // GekkoNet not linked in this build
#endif
}

// ============================================================================
// Lifecycle
// ============================================================================

bool GameplayBridge_Init() {
    if (s_initialized) return true;

    s_initialized = true;
    s_sessionActive = false;

    // Probe GekkoNet availability (informational only — does not affect gameplay)
    s_gekkoNetAvailable = ProbeGekkoNetAvailability();

    if (s_gekkoNetAvailable) {
        SetStatus("GameplayBridge ready. GekkoNet linkable (%s) but not used for gameplay.", s_gekkoNetVersion);
        LOG_INFO("[GameplayBridge] Initialized. GekkoNet available (%s) — not used for gameplay", s_gekkoNetVersion);
    } else {
        SetStatus("GameplayBridge ready. Mod-owned rollback (GekkoNet not linked).");
        LOG_INFO("[GameplayBridge] Initialized. Mod-owned rollback path (GekkoNet not linked)");
    }

    return true;
}

void GameplayBridge_Shutdown() {
    if (!s_initialized) return;

    if (s_sessionActive) {
        GameplayBridge_EndSession();
    }

    s_initialized = false;
    s_gekkoNetAvailable = false;
    SetStatus("GameplayBridge shut down.");
    LOG_INFO("[GameplayBridge] Shut down");
}

bool GameplayBridge_IsGekkoNetAvailable() {
    return s_gekkoNetAvailable;
}

const char* GameplayBridge_GetGekkoNetVersion() {
    return s_gekkoNetVersion;
}

// ============================================================================
// Bridge Status
// ============================================================================

void GameplayBridge_GetSnapshot(GameplayBridgeSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->initialized = s_initialized;
    out->gekkonet_available = s_gekkoNetAvailable;
    out->session_active = s_sessionActive;
    out->timesync_hard_freeze = (s_timesyncMode == TimesyncMode::HardFreeze);
    out->local_game_slot = PlayerMapping_GetLocalGameSlot();
    out->remote_game_slot = PlayerMapping_GetRemoteGameSlot();
    out->active_delay = DelayPolicy_GetActiveDelay();
    out->rollback_budget = DelayPolicy_GetAgreedRollbackBudget();
    out->current_frame = Rollback::RollbackSession_GetCurrentFrame();
    out->predicted_frames = s_timesyncPredictedFrames;
    out->hard_wait_frames = s_timesyncHardWaitFrames;
    out->runtime_tick_scale = GetEffectiveTickScale();

    strncpy_s(out->status, sizeof(out->status), s_status, _TRUNCATE);
    strncpy_s(out->gekkonet_version, sizeof(out->gekkonet_version),
              s_gekkoNetVersion, _TRUNCATE);
}

// ============================================================================
// Gameplay Session Control
// ============================================================================

bool GameplayBridge_StartSession(const Rollback::RollbackSessionConfig& config) {
    if (s_sessionActive) {
        LOG_WARN("[GameplayBridge] Session already active — ending previous");
        GameplayBridge_EndSession();
    }

    // Reset per-session counters
    s_framesUpdated = 0;
    s_remoteInputsSubmitted = 0;
    s_delayPolicyConsumed = 0;
    s_lastConsumedDelay = -1;
    ResetRuntimePacing();

    // Set player mapping from the config
    PlayerMapping_SetAssignment(config.local_player);

    // Verify player mapping was applied correctly
    int verifyLocal = PlayerMapping_GetLocalGameSlot();
    int verifyRemote = PlayerMapping_GetRemoteGameSlot();
    if (verifyLocal != config.local_player || verifyRemote != config.remote_player) {
        LOG_ERROR("[GameplayBridge] Player mapping mismatch! expected local=P%d remote=P%d, got local=P%d remote=P%d",
            config.local_player + 1, config.remote_player + 1,
            verifyLocal + 1, verifyRemote + 1);
        Rollback::NetplayLog_Write("BRIDGE", -1,
            "ERROR: Player mapping mismatch after SetAssignment");
        PlayerMapping_Clear();
        SetStatus("Player mapping verification failed.");
        return false;
    }

    // Start the mod-owned rollback session
    bool ok = Rollback::RollbackSession_Begin(config);
    if (!ok) {
        LOG_ERROR("[GameplayBridge] RollbackSession_Begin failed");
        Rollback::NetplayLog_Write("BRIDGE", -1,
            "ERROR: RollbackSession_Begin FAILED");
        PlayerMapping_Clear();
        SetStatus("Failed to start gameplay session.");
        return false;
    }

    s_sessionActive = true;
    SetStatus("Session active (mod-owned rollback, local=P%d).",
        config.local_player + 1);

    LOG_INFO("[GameplayBridge] Session started: local=P%d remote=P%d delay=%d budget=%d",
        config.local_player + 1, config.remote_player + 1,
        config.initial_delay, config.rollback_budget);
    Rollback::NetplayLog_Write("BRIDGE", config.start_frame,
        "Session started: local=P%d remote=P%d delay=%d budget=%d baseline=0x%08X",
        config.local_player + 1, config.remote_player + 1,
        config.initial_delay, config.rollback_budget, config.baseline_checksum);

    return true;
}

void GameplayBridge_EndSession() {
    if (!s_sessionActive) return;

    // Log session summary before teardown
    int32_t frame = Rollback::RollbackSession_GetCurrentFrame();
    Rollback::NetplayLog_Write("BRIDGE", frame,
        "Session ending: frames_updated=%d remote_inputs=%d delay_consumed=%d soft_frames=%d hard_waits=%d",
        s_framesUpdated, s_remoteInputsSubmitted, s_delayPolicyConsumed,
        s_timesyncSoftFrames, s_timesyncHardWaitFrames);

    ResetRuntimePacing();

    Rollback::RollbackSession_End();
    PlayerMapping_Clear();

    s_sessionActive = false;
    SetStatus("Session ended.");
    LOG_INFO("[GameplayBridge] Session ended (frames=%d inputs=%d)",
        s_framesUpdated, s_remoteInputsSubmitted);
}

bool GameplayBridge_IsSessionActive() {
    return s_sessionActive;
}

// ============================================================================
// Per-Frame Entry Point
// ============================================================================

void GameplayBridge_FrameUpdate() {
    if (!s_sessionActive) return;

    const int32_t frame = Rollback::RollbackSession_GetCurrentFrame();
    const TimesyncDecision timesync = ComputeTimesyncDecision();
    ApplyTimesyncDecision(timesync, frame);

    if (timesync.mode == TimesyncMode::HardFreeze) {
        s_timesyncHardWaitFrames++;
        return;
    }

    if (timesync.mode == TimesyncMode::SoftSlow) {
        s_timesyncSoftFrames++;
    }

    s_framesUpdated++;

    // Drive the mod-owned rollback session (the core gameplay loop).
    // This is the authoritative per-frame entry point — no other code
    // should call RollbackSession_FrameUpdate() directly.
    Rollback::RollbackSession_FrameUpdate();

    // Consume delay policy — complete the Requested→Applied cycle.
    // This marks the rollback session's active delay as consumed so
    // the delay change state machine can proceed.
    if (!DelayPolicy_IsRollbackSynced()) {
        int activeDelay = Rollback::RollbackSession_GetActiveDelay();
        DelayPolicy_OnRollbackApplied(activeDelay);
        s_delayPolicyConsumed++;

        if (activeDelay != s_lastConsumedDelay) {
            Rollback::NetplayLog_Write("BRIDGE",
                Rollback::RollbackSession_GetCurrentFrame(),
                "Delay policy consumed: %d -> %d",
                s_lastConsumedDelay, activeDelay);
            s_lastConsumedDelay = activeDelay;
        }
    }
}

// ============================================================================
// Input Routing
// ============================================================================

uint16_t GameplayBridge_ReadLocalInput() {
    return PlayerMapping_ReadLocalInput();
}

void GameplayBridge_SubmitRemoteInput(int32_t frame, uint16_t input) {
    Rollback::RollbackSession_SubmitRemoteInput(frame, input);
    s_remoteInputsSubmitted++;
}

} // namespace Net
