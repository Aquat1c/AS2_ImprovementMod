/**
 * Alice Senki 2 - Rollback Debug Implementation
 */

#include "rollback/rollback_debug.h"
#include "rollback/rollback_session.h"
#include "rollback/resimulation.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "rollback/desync_dump.h"
#include "rollback/online_wiring.h"
#include "net/delay_policy.h"
#include "net/sync_policy.h"
#include "net/session_manager.h"
#include "net/protocol.h"
#include "as2_constants.h"
#include "patches/input_override.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"
#include "imgui.h"

#include <algorithm>
#include <string.h>
#include <windows.h>

namespace Rollback {

// ============================================================================
// Internal State
// ============================================================================

static bool     s_initialized       = false;
static bool     s_digestEnabled     = false;
static int      s_digestInterval    = 60;    // Every N frames

// Desync tracking
static bool     s_desyncDetected    = false;
static int32_t  s_desyncFrame       = -1;
static uint32_t s_desyncLocalCrc    = 0;
static uint32_t s_desyncRemoteCrc   = 0;

// Counters
static int32_t  s_digestsSent       = 0;
static int32_t  s_digestsRecv       = 0;
static int32_t  s_digestsMatched    = 0;
static int32_t  s_digestsMismatched = 0;
static int32_t  s_lastDigestFrame   = -1;
static int32_t  s_statusSent        = 0;
static int32_t  s_statusRecv        = 0;
static int32_t  s_statusInterval    = 15;

static constexpr int kChecksumHistorySize = 512;
static int32_t  s_checksumHistoryFrame[kChecksumHistorySize] = {};
static uint32_t s_checksumHistoryCrc[kChecksumHistorySize] = {};

// Last remote frame-status telemetry
static int32_t  s_remoteStatusRbFrameCurrent          = -1;
static int32_t  s_remoteStatusGameAbsFrameCurrent     = -1;
static int32_t  s_remoteStatusFrameOriginAbs          = -1;
static int32_t  s_remoteStatusRbFrameLastReceived     = -1;
static int32_t  s_remoteStatusRbFrameConfirmed        = -1;
static int32_t  s_remoteStatusPredicted               = 0;
static uint32_t s_remoteStatusChecksum                = 0;

// Current frame checksum (cached)
static uint32_t s_currentChecksum   = 0;

// Forward declarations
static bool TryGetChecksumForFrame(int32_t frame, uint32_t* checksum);
static void ResetChecksumHistory();

static uint32_t Rotl32(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

static uint32_t ComputeGameplayDigestChecksum() {
    uint32_t p1Entity = 0;
    uint32_t p2Entity = 0;
    uint32_t preMatchGap = 0;
    uint32_t effectIndex = 0;
    uint32_t rngSeed = 0;

    __try {
        p1Entity = CalcCRC32((const void*)ADDR_P1_ENTITY_BASE, ENTITY_SIZE);
        p2Entity = CalcCRC32((const void*)ADDR_P2_ENTITY_BASE, ENTITY_SIZE);
        preMatchGap = CalcCRC32((const void*)ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE);
        effectIndex = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
        rngSeed = DetVer_GetRngSeed();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADDEAD;
    }

    // Use gameplay-critical deterministic state only (both entities + RNG +
    // effect cursor + pre-match gap).
    // This avoids false warnings from non-authoritative visual/transient memory regions.
    return p1Entity ^
           Rotl32(p2Entity, 5) ^
           Rotl32(preMatchGap, 11) ^
           Rotl32(effectIndex, 17) ^
           rngSeed;
}

static void ResetSessionState() {
    s_desyncDetected = false;
    s_desyncFrame = -1;
    s_desyncLocalCrc = 0;
    s_desyncRemoteCrc = 0;
    s_digestsSent = 0;
    s_digestsRecv = 0;
    s_digestsMatched = 0;
    s_digestsMismatched = 0;
    s_lastDigestFrame = -1;
    s_statusSent = 0;
    s_statusRecv = 0;
    s_remoteStatusRbFrameCurrent = -1;
    s_remoteStatusGameAbsFrameCurrent = -1;
    s_remoteStatusFrameOriginAbs = -1;
    s_remoteStatusRbFrameLastReceived = -1;
    s_remoteStatusRbFrameConfirmed = -1;
    s_remoteStatusPredicted = 0;
    s_remoteStatusChecksum = 0;
    s_currentChecksum = 0;
    ResetChecksumHistory();
    DesyncDump_Reset();
}

static void ResetChecksumHistory() {
    for (int i = 0; i < kChecksumHistorySize; ++i) {
        s_checksumHistoryFrame[i] = -1;
        s_checksumHistoryCrc[i] = 0;
    }
}

static void StoreChecksumForFrame(int32_t frame, uint32_t checksum) {
    if (frame < 0) {
        return;
    }

    const int index = frame % kChecksumHistorySize;
    s_checksumHistoryFrame[index] = frame;
    s_checksumHistoryCrc[index] = checksum;
}

static bool TryGetChecksumForFrame(int32_t frame, uint32_t* checksum) {
    if (!checksum || frame < 0) {
        return false;
    }

    const int index = frame % kChecksumHistorySize;
    if (s_checksumHistoryFrame[index] != frame) {
        return false;
    }

    *checksum = s_checksumHistoryCrc[index];
    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

void RollbackDebug_Init() {
    ResetSessionState();
    s_initialized = true;
    LOG_INFO("[RollbackDebug] Initialized");
}

void RollbackDebug_Shutdown() {
    s_initialized = false;
}

void RollbackDebug_ResetSession() {
    if (!s_initialized) {
        return;
    }
    ResetSessionState();
    LOG_INFO("[RollbackDebug] Session diagnostics reset");
}

// ============================================================================
// Per-Frame
// ============================================================================

void RollbackDebug_FrameUpdate() {
    if (!s_initialized) return;
    if (!RollbackSession_IsActive()) return;
    if (RollbackSession_IsRollingBack()) return;  // Don't log during resim

    const int32_t rbFrame = RollbackSession_GetCurrentFrame();
    const int32_t gameAbsFrame = RollbackSession_GetCurrentGameAbsFrame();
    const int32_t frameOriginAbs = RollbackSession_GetFrameOriginAbs();

    // Compute deterministic gameplay-core checksum.
    s_currentChecksum = ComputeGameplayDigestChecksum();

    StoreChecksumForFrame(rbFrame, s_currentChecksum);
    DesyncDump_StoreChecksum(rbFrame, s_currentChecksum);

    // Send state digest at configured interval
    if (s_digestEnabled && s_digestInterval > 0 &&
        (rbFrame % s_digestInterval == 0) && rbFrame > 0 &&
        Net::Session_IsConnected()) {

        Net::StateDigestPayload digest;
        digest.frame_number = (uint32_t)rbFrame;
        digest.crc32 = s_currentChecksum;

        Net::Session_SendPacket(
            Net::CHANNEL_DEBUG,
            Net::PacketType::StateDigest,
            &digest, sizeof(digest),
            true  // Reliable to avoid debug transport artifacts
        );

        s_digestsSent++;
        s_lastDigestFrame = rbFrame;

        NetplayLog_Verbose("DIGEST", rbFrame,
            "Sent: rb_frame=%d game_abs_frame=%d origin_abs=%d crc=0x%08X",
            rbFrame,
            gameAbsFrame,
            frameOriginAbs,
            s_currentChecksum);
    }

    if (s_digestEnabled && s_statusInterval > 0 &&
        (rbFrame % s_statusInterval == 0) && Net::Session_IsConnected()) {

        Net::FrameSyncStatusPayload status{};
        RollbackSessionSnapshot rbSnap{};
        RollbackSession_GetSnapshot(&rbSnap);
        status.rb_frame_current = rbFrame;
        status.game_abs_frame_current = gameAbsFrame;
        status.frame_origin_abs = frameOriginAbs;
        status.rb_frame_last_remote_received = rbSnap.rb_frame_last_remote_received;
        status.rb_frame_confirmed = rbSnap.rb_frame_last_confirmed;
        status.predicted_frames = rbSnap.predicted_frames_outstanding;
        status.checksum = s_currentChecksum;

        Net::Session_SendPacket(
            Net::CHANNEL_DEBUG,
            Net::PacketType::FrameSyncStatus,
            &status, sizeof(status),
            false
        );

        s_statusSent++;

        NetplayLog_Verbose("FSYNC", rbFrame,
            "Sent status: rb_frame=%d game_abs_frame=%d origin_abs=%d remote_received_rb=%d confirmed_rb=%d predicted=%d crc=0x%08X",
            status.rb_frame_current,
            status.game_abs_frame_current,
            status.frame_origin_abs,
            status.rb_frame_last_remote_received,
            status.rb_frame_confirmed,
            status.predicted_frames,
            status.checksum);
    }

    // Rate-limited diagnostics log (every 300 frames = ~5 seconds)
    if (rbFrame > 0 && rbFrame % 300 == 0) {
        RollbackSessionSnapshot snap;
        RollbackSession_GetSnapshot(&snap);

        LOG_INFO("[RollbackDebug] rb=%d game_abs=%d origin=%d conf=%d remote=%d "
                 "rb=%d maxrb=%d pred=%d misp=%d "
                 "delay=%d budget=%d crc=0x%08X",
            snap.rb_frame_current, snap.game_abs_frame_current, snap.frame_origin_abs,
            snap.rb_frame_last_confirmed,
            snap.rb_frame_last_remote_received,
            snap.rollback_count, snap.max_rollback_distance,
            snap.predicted_frames_outstanding, snap.total_mispredictions,
            snap.active_delay, snap.rollback_budget,
            s_currentChecksum);
    }
}

// ============================================================================
// Desync Detection
// ============================================================================

void RollbackDebug_OnRemoteDigest(int32_t frame, uint32_t remote_crc) {
    s_digestsRecv++;

    // Only compare fully settled frames on BOTH peers.
    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);

    constexpr int kMinimumSettleLagFrames = 6;
    const int settleLagFrames = (std::max)(
        kMinimumSettleLagFrames,
        RollbackSession_GetRollbackBudget() + RollbackSession_GetActiveDelay() + 2);
    const int32_t localSettledFrame = snap.rb_frame_current - settleLagFrames;

    if (frame > snap.rb_frame_last_confirmed || frame > localSettledFrame) {
        NetplayLog_Verbose("DIGEST", frame,
            "Skip compare: frame not locally settled (rb_frame=%d confirmed_rb=%d settled_rb=%d current_rb=%d)",
            frame,
            snap.rb_frame_last_confirmed,
            localSettledFrame,
            snap.rb_frame_current);
        return;
    }

    if (s_remoteStatusRbFrameConfirmed >= 0 && frame > s_remoteStatusRbFrameConfirmed) {
        NetplayLog_Verbose("DIGEST", frame,
            "Skip compare: frame not remotely confirmed yet (rb_frame=%d remote_confirmed_rb=%d)",
            frame,
            s_remoteStatusRbFrameConfirmed);
        return;
    }

    uint32_t local_crc = 0;
    if (!TryGetChecksumForFrame(frame, &local_crc)) {
        NetplayLog_Verbose("DIGEST", frame,
            "Skip compare: local checksum not retained for frame");
        return;
    }

    if (local_crc == remote_crc) {
        s_digestsMatched++;
        NetplayLog_Verbose("DIGEST", frame,
            "Match: local=0x%08X remote=0x%08X", local_crc, remote_crc);
    } else {
        s_digestsMismatched++;
        if (!s_desyncDetected) {
            s_desyncDetected = true;
            s_desyncFrame = frame;
            s_desyncLocalCrc = local_crc;
            s_desyncRemoteCrc = remote_crc;

            LOG_ERROR("[RollbackDebug] DESYNC DETECTED at frame %d! "
                      "local=0x%08X remote=0x%08X",
                frame, local_crc, remote_crc);

            NetplayLog_Write("DESYNC", frame,
                "!!! DESYNC DETECTED !!! local=0x%08X remote=0x%08X "
                "matched=%d mismatched=%d",
                local_crc, remote_crc,
                s_digestsMatched, s_digestsMismatched);
            NetplayLog_Flush();
        }

        // Dump full state on desync (first occurrence + 10s cooldown)
        DesyncDump_TryDump(frame, local_crc, remote_crc);
    }
}

void RollbackDebug_OnRemoteFrameSyncStatus(int32_t remote_rb_frame,
                                           int32_t remote_game_abs_frame,
                                           int32_t remote_frame_origin_abs,
                                           int32_t remote_rb_frame_last_received,
                                           int32_t remote_rb_frame_confirmed,
                                           int32_t remote_predicted_frames,
                                           uint32_t remote_checksum) {
    s_statusRecv++;
    s_remoteStatusRbFrameCurrent = remote_rb_frame;
    s_remoteStatusGameAbsFrameCurrent = remote_game_abs_frame;
    s_remoteStatusFrameOriginAbs = remote_frame_origin_abs;
    s_remoteStatusRbFrameLastReceived = remote_rb_frame_last_received;
    s_remoteStatusRbFrameConfirmed = remote_rb_frame_confirmed;
    s_remoteStatusPredicted = remote_predicted_frames;
    s_remoteStatusChecksum = remote_checksum;

    if (!RollbackSession_IsActive()) {
        return;
    }

    const int32_t localRbFrame = RollbackSession_GetCurrentFrame();
    const int32_t localGameAbsFrame = RollbackSession_GetCurrentGameAbsFrame();
    const int32_t localFrameOriginAbs = RollbackSession_GetFrameOriginAbs();
    const int32_t rbFrameDelta = localRbFrame - remote_rb_frame;
    const int32_t gameAbsDelta = localGameAbsFrame - remote_game_abs_frame;
    const int32_t originDelta = localFrameOriginAbs - remote_frame_origin_abs;
    const bool remoteReceivedValid = (remote_rb_frame_last_received >= 0);
    const int32_t remoteReceivedDelta =
        remoteReceivedValid ? (localRbFrame - remote_rb_frame_last_received) : 0;

    NetplayLog_Verbose("FSYNC", localRbFrame,
        "Remote status: remote_rb=%d remote_game_abs=%d remote_origin_abs=%d remote_received_rb=%d confirmed_rb=%d predicted=%d "
        "rb_delta=%d game_abs_delta=%d origin_delta=%d remote_received_delta=%d crc=0x%08X",
        remote_rb_frame,
        remote_game_abs_frame,
        remote_frame_origin_abs,
        remote_rb_frame_last_received,
        remote_rb_frame_confirmed,
        remote_predicted_frames,
        rbFrameDelta,
        gameAbsDelta,
        originDelta,
        remoteReceivedDelta,
        remote_checksum);

    const int rbFrameSkewThreshold = (std::max)(RollbackSession_GetRollbackBudget() + 2, 6);
    const int gameAbsSkewThreshold = 30;
    const int remoteReceivedSkewThreshold = (std::max)(RollbackSession_GetRollbackBudget() + 4, 8);

    const bool rbFrameSkew =
        (rbFrameDelta > rbFrameSkewThreshold || rbFrameDelta < -rbFrameSkewThreshold);
    const bool gameAbsSkew =
        (gameAbsDelta > gameAbsSkewThreshold || gameAbsDelta < -gameAbsSkewThreshold);
    const bool originSkew = (originDelta != 0);
    const bool remoteReceivedSkew =
        remoteReceivedValid &&
        (remoteReceivedDelta > remoteReceivedSkewThreshold ||
         remoteReceivedDelta < -remoteReceivedSkewThreshold);

    if (rbFrameSkew || gameAbsSkew || originSkew || remoteReceivedSkew) {
        NetplayLog_Write("FSYNC", localRbFrame,
            "SKEW: local_rb=%d remote_rb=%d local_game_abs=%d remote_game_abs=%d local_origin_abs=%d remote_origin_abs=%d "
            "remote_received_rb=%d confirmed_rb=%d predicted=%d local_crc=0x%08X remote_crc=0x%08X",
            localRbFrame,
            remote_rb_frame,
            localGameAbsFrame,
            remote_game_abs_frame,
            localFrameOriginAbs,
            remote_frame_origin_abs,
            remote_rb_frame_last_received,
            remote_rb_frame_confirmed,
            remote_predicted_frames,
            s_currentChecksum,
            remote_checksum);
    }
}

bool RollbackDebug_IsDesyncDetected() {
    return s_desyncDetected;
}

int32_t RollbackDebug_GetDesyncFrame() {
    return s_desyncFrame;
}

// ============================================================================
// Configuration
// ============================================================================

void RollbackDebug_SetDigestEnabled(bool enabled) {
    s_digestEnabled = enabled;
    LOG_INFO("[RollbackDebug] Digest %s", enabled ? "enabled" : "disabled");
}

bool RollbackDebug_IsDigestEnabled() {
    return s_digestEnabled;
}

void RollbackDebug_SetDigestInterval(int frames) {
    if (frames < 1) frames = 1;
    if (frames > 3600) frames = 3600;
    s_digestInterval = frames;
}

// ============================================================================
// Diagnostics
// ============================================================================

void RollbackDebug_GetSnapshot(RollbackDebugSnapshot* out) {
    if (!out) return;
    out->desync_detected = s_desyncDetected;
    out->desync_frame = s_desyncFrame;
    out->desync_local_crc = s_desyncLocalCrc;
    out->desync_remote_crc = s_desyncRemoteCrc;
    out->current_frame_checksum = s_currentChecksum;
    out->last_digest_frame = s_lastDigestFrame;
    out->digest_enabled = s_digestEnabled;
    out->digest_interval = s_digestInterval;
    out->digests_sent = s_digestsSent;
    out->digests_received = s_digestsRecv;
    out->digests_matched = s_digestsMatched;
    out->digests_mismatched = s_digestsMismatched;
}

// ============================================================================
// ImGui Debug Window
// ============================================================================

void RollbackDebug_RenderImGui(bool* p_open) {
    if (!ImGui::Begin("Rollback Debug", p_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // Session state
    RollbackSessionSnapshot snap;
    RollbackSession_GetSnapshot(&snap);

    ImGui::TextColored(snap.active ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
        "Session: %s", snap.active ? "ACTIVE" : "INACTIVE");

    if (snap.active) {
        ImGui::Separator();
        ImGui::Text("Player: P%d (local) vs P%d (remote)",
            snap.local_player + 1, snap.remote_player + 1);

        // Frame info
        ImGui::Separator();
        ImGui::Text("Frame Origin Abs: %d", snap.frame_origin_abs);
        ImGui::Text("Game Abs Frame:   %d", snap.game_abs_frame_current);
        ImGui::Text("RB Current:       %d", snap.rb_frame_current);
        ImGui::Text("RB Confirmed:     %d", snap.rb_frame_last_confirmed);
        ImGui::Text("RB Remote Recv:   %d", snap.rb_frame_last_remote_received);
        ImGui::Text("RB Last Saved:    %d", snap.rb_frame_last_saved_state);

        // Policy
        ImGui::Separator();
        ImGui::Text("Visible Delay:    %d", snap.active_delay);
        ImGui::Text("Rollback Budget:  %d", snap.rollback_budget);

        // Timesync / delay diagnostics
        Net::DelayPolicySnapshot delaySnap{};
        Net::DelayPolicy_GetSnapshot(&delaySnap);
        OnlineWiringSnapshot wiringSnap{};
        OnlineWiring_GetSnapshot(&wiringSnap);
        TimesyncDebugInfo tsDebug{};
        GetTimesyncDebugInfo(&tsDebug);

        ImGui::Separator();
        ImGui::Text("Timesync & Delay");
        ImGui::Text("Rollback Phase:    %s", Net::MatchRollbackPhaseName(wiringSnap.phase));
        ImGui::Text("Session Running:   %s", wiringSnap.session_running ? "yes" : "no");
        ImGui::Text("Stepping Enabled:  %s", wiringSnap.stepping_enabled ? "yes" : "no");
        ImGui::Text("Startup Armed:     %s", wiringSnap.startup_barrier_armed ? "yes" : "no");
        ImGui::Text("Startup Released:  %s", wiringSnap.startup_barrier_released ? "yes" : "no");
        ImGui::Text("Frames Ahead:     %.2f", tsDebug.frames_ahead);
        ImGui::Text("Filtered Adjust:  %.2f ms", tsDebug.rate_adjust_ms);
        ImGui::Text("Tick Target:      %.3f", wiringSnap.target_tick_scale);
        ImGui::Text("Tick Current:     %.3f", wiringSnap.current_tick_scale);
        ImGui::Text("Stall Frames:     %d", tsDebug.stall_frame_count);
        ImGui::Text("Stalled:          %s", tsDebug.stalled ? "yes" : "no");
        ImGui::Text("Max rollback:     %d", delaySnap.rollback_budget);
        ImGui::Text("Remote Delay:     %d", delaySnap.remote_announced_delay);
        ImGui::Text("Protection Win:   %d", delaySnap.protection_window);
        ImGui::Text("Stall Threshold:  %d", wiringSnap.stall_threshold);
        ImGui::Text("Avg Ping:         %.1f ms", snap.gekko_avg_ping);
        ImGui::Text("Jitter:           %.1f ms", snap.gekko_jitter);
        ImGui::Text("Rec Delay:        %d", delaySnap.recommended_delay);
        ImGui::Text("Rec Max RB:       %d", delaySnap.recommended_max_rollback);

        // Rollback stats
        ImGui::Separator();
        ImGui::Text("Rollback Count:   %d", snap.rollback_count);
        ImGui::Text("Max Depth:        %d", snap.max_rollback_distance);
        ImGui::Text("Last RB Start:    %d", snap.rb_last_rollback_start_frame);
        ImGui::Text("Last RB Length:   %d", snap.last_rollback_replay_length);
        ImGui::Text("Predicted Outstanding: %d", snap.predicted_frames_outstanding);

        // Prediction stats
        ImGui::Separator();
        ImGui::Text("Predictions:      %d", snap.total_predictions);
        ImGui::Text("Mispredictions:   %d", snap.total_mispredictions);
        ImGui::Text("Correct:          %d", snap.total_correct_predictions);
        if (snap.total_predictions > 0) {
            float accuracy = 100.0f * (float)snap.total_correct_predictions /
                (float)(snap.total_correct_predictions + snap.total_mispredictions);
            ImGui::Text("Accuracy:         %.1f%%", accuracy);
        }

        // IO
        ImGui::Separator();
        ImGui::Text("Inputs Sent:      %d", snap.local_inputs_sent);
        ImGui::Text("Inputs Received:  %d", snap.remote_inputs_received);

        // Resim state
        if (snap.is_rolling_back) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,1,0,1), "ROLLING BACK");
        }

        // Checksums
        ImGui::Separator();
        ImGui::Text("Baseline CRC:     0x%08X", snap.baseline_checksum);
        ImGui::Text("Current CRC:      0x%08X", snap.current_checksum);

        // Desync
        RollbackDebugSnapshot dbg;
        RollbackDebug_GetSnapshot(&dbg);
        if (dbg.desync_detected) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0,0,1), "DESYNC at frame %d!", dbg.desync_frame);
            ImGui::Text("Local:  0x%08X", dbg.desync_local_crc);
            ImGui::Text("Remote: 0x%08X", dbg.desync_remote_crc);
        }

        // Digest stats
        if (dbg.digest_enabled) {
            ImGui::Separator();
            ImGui::Text("Digests: sent=%d recv=%d match=%d mismatch=%d",
                dbg.digests_sent, dbg.digests_received,
                dbg.digests_matched, dbg.digests_mismatched);
        }
    }

    ImGui::End();
}

} // namespace Rollback
