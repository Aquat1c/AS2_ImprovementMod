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
static int32_t  s_digestsSkippedNotSettled = 0;
static int32_t  s_digestsSkippedNoHistory = 0;
static int32_t  s_digestsSkippedRemoteUnsettled = 0;
static int32_t  s_lastDigestFrame   = -1;
static int32_t  s_statusSent        = 0;
static int32_t  s_statusRecv        = 0;
static int32_t  s_statusInterval    = 15;
static DWORD    s_lastRemoteStatusRecvMs = 0;

static constexpr int kChecksumHistorySize = 4096;
static constexpr int kChecksumHistoryMask = kChecksumHistorySize - 1;
struct ChecksumHistorySlot {
    int32_t  frame;
    uint32_t crc;
};
static ChecksumHistorySlot s_checksumHistory[kChecksumHistorySize] = {};

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
static void ResetChecksumHistory();

uint32_t RollbackDebug_ComputeAuthoritativeChecksum() {
    return RollbackSession_ComputeLiveStateChecksum();
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
    s_digestsSkippedNotSettled = 0;
    s_digestsSkippedNoHistory = 0;
    s_digestsSkippedRemoteUnsettled = 0;
    s_lastDigestFrame = -1;
    s_statusSent = 0;
    s_statusRecv = 0;
    s_lastRemoteStatusRecvMs = 0;
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
        s_checksumHistory[i].frame = -1;
        s_checksumHistory[i].crc = 0;
    }
}

static void StoreChecksumForFrame(int32_t frame, uint32_t checksum) {
    if (frame < 0) {
        return;
    }

    int index = frame & kChecksumHistoryMask;
    for (int probe = 0; probe < kChecksumHistorySize; ++probe) {
        ChecksumHistorySlot* slot = &s_checksumHistory[(index + probe) & kChecksumHistoryMask];
        if (slot->frame < 0 || slot->frame == frame) {
            slot->frame = frame;
            slot->crc = checksum;
            return;
        }
    }
}

bool RollbackDebug_TryGetChecksumForFrame(int32_t frame, uint32_t* checksum) {
    if (!checksum || frame < 0) {
        return false;
    }

    int index = frame & kChecksumHistoryMask;
    for (int probe = 0; probe < kChecksumHistorySize; ++probe) {
        const ChecksumHistorySlot* slot = &s_checksumHistory[(index + probe) & kChecksumHistoryMask];
        if (slot->frame == frame) {
            *checksum = slot->crc;
            return true;
        }
        if (slot->frame < 0) {
            return false;
        }
    }
    return false;
}

static int32_t GetSettleLagFrames() {
    constexpr int kMinimumSettleLagFrames = 6;
    return (std::max)(
        kMinimumSettleLagFrames,
        RollbackSession_GetRollbackBudget() + RollbackSession_GetActiveDelay() + 2);
}

static int32_t GetRemoteSettledFrameEstimate(const RollbackSessionSnapshot& snap) {
    const int32_t settleLagFrames = GetSettleLagFrames();
    const int32_t localEstimate = snap.rb_frame_current - settleLagFrames;

    const DWORD now = GetTickCount();
    const bool statusFresh =
        s_lastRemoteStatusRecvMs > 0 &&
        now >= s_lastRemoteStatusRecvMs &&
        (now - s_lastRemoteStatusRecvMs) <= 5000;

    if (statusFresh && s_remoteStatusRbFrameConfirmed >= 0) {
        return (std::min)(s_remoteStatusRbFrameConfirmed, localEstimate);
    }
    return localEstimate;
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

    // Compute authoritative gameplay checksum (Gekko save/load equivalent).
    s_currentChecksum = RollbackDebug_ComputeAuthoritativeChecksum();

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
            true
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

void RollbackDebug_ReportDrift(const char* source,
                               int32_t frame,
                               uint32_t local_crc,
                               uint32_t remote_crc,
                               const char* detail) {
    if (!source) {
        source = "unknown";
    }

    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);

    if (!s_desyncDetected) {
        s_desyncDetected = true;
        s_desyncFrame = frame;
        s_desyncLocalCrc = local_crc;
        s_desyncRemoteCrc = remote_crc;

        LOG_ERROR("[RollbackDebug] SIM DRIFT at rb_frame %d via %s! local=0x%08X remote=0x%08X",
            frame, source, local_crc, remote_crc);

        NetplayLog_Write("DESYNC", frame,
            "!!! SIM DRIFT DETECTED !!! source=%s local=0x%08X remote=0x%08X "
            "rb=%d abs=%d origin=%d confirmed=%d remote_recv=%d rollbacks=%d max_rb=%d pred=%d %s",
            source,
            local_crc,
            remote_crc,
            snap.rb_frame_current,
            snap.game_abs_frame_current,
            snap.frame_origin_abs,
            snap.rb_frame_last_confirmed,
            snap.rb_frame_last_remote_received,
            snap.rollback_count,
            snap.max_rollback_distance,
            snap.predicted_frames_outstanding,
            detail ? detail : "");
        NetplayLog_Flush();
    } else {
        NetplayLog_Write("DESYNC", frame,
            "Additional drift via %s at rb=%d local=0x%08X remote=0x%08X (first=%d) %s",
            source,
            frame,
            local_crc,
            remote_crc,
            s_desyncFrame,
            detail ? detail : "");
    }

    DesyncDump_TryDump(frame, local_crc, remote_crc, source, detail);
}

void RollbackDebug_LogSessionSummary(const char* reason) {
    if (!s_initialized) {
        return;
    }

    NetplayLog_Write("INTEGRITY", s_desyncFrame,
        "Session summary (%s): digest_enabled=%d sent=%d recv=%d matched=%d mismatched=%d "
        "skip_not_settled=%d skip_no_history=%d skip_remote_unsettled=%d desync=%d desync_frame=%d",
        reason ? reason : "unspecified",
        s_digestEnabled ? 1 : 0,
        s_digestsSent,
        s_digestsRecv,
        s_digestsMatched,
        s_digestsMismatched,
        s_digestsSkippedNotSettled,
        s_digestsSkippedNoHistory,
        s_digestsSkippedRemoteUnsettled,
        s_desyncDetected ? 1 : 0,
        s_desyncFrame);
    if (s_desyncDetected) {
        NetplayLog_Write("INTEGRITY", s_desyncFrame,
            "First drift: local=0x%08X remote=0x%08X",
            s_desyncLocalCrc,
            s_desyncRemoteCrc);
    }
}

bool RollbackDebug_IsRbFrameReadyToCompare(int32_t frame, int32_t remote_confirmed_rb) {
    if (frame < 0 || !RollbackSession_IsActive()) {
        return false;
    }

    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);

    const int32_t settleLagFrames = GetSettleLagFrames();
    const int32_t localSettledFrame = snap.rb_frame_current - settleLagFrames;

    if (frame > snap.rb_frame_last_confirmed || frame > localSettledFrame) {
        return false;
    }

    int32_t remoteSettledFrame = GetRemoteSettledFrameEstimate(snap);
    if (remote_confirmed_rb >= 0) {
        remoteSettledFrame = (std::min)(remoteSettledFrame, remote_confirmed_rb);
    }
    return frame <= remoteSettledFrame;
}

void RollbackDebug_OnRemoteDigest(int32_t frame, uint32_t remote_crc) {
    s_digestsRecv++;

    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);

    const int32_t settleLagFrames = GetSettleLagFrames();
    const int32_t localSettledFrame = snap.rb_frame_current - settleLagFrames;
    const int32_t remoteSettledFrame = GetRemoteSettledFrameEstimate(snap);

    if (frame > snap.rb_frame_last_confirmed || frame > localSettledFrame) {
        s_digestsSkippedNotSettled++;
        NetplayLog_Verbose("DIGEST", frame,
            "Skip compare: frame not locally settled (rb_frame=%d confirmed_rb=%d settled_rb=%d current_rb=%d)",
            frame,
            snap.rb_frame_last_confirmed,
            localSettledFrame,
            snap.rb_frame_current);
        return;
    }

    if (frame > remoteSettledFrame) {
        s_digestsSkippedRemoteUnsettled++;
        NetplayLog_Verbose("DIGEST", frame,
            "Skip compare: frame not remotely settled yet (rb_frame=%d remote_settled_rb=%d remote_confirmed_rb=%d)",
            frame,
            remoteSettledFrame,
            s_remoteStatusRbFrameConfirmed);
        return;
    }

    uint32_t local_crc = 0;
    if (!RollbackDebug_TryGetChecksumForFrame(frame, &local_crc)) {
        s_digestsSkippedNoHistory++;
        if (s_digestsSkippedNoHistory <= 5 || (s_digestsSkippedNoHistory % 60) == 0) {
            NetplayLog_Write("DIGEST", frame,
                "Skip compare: local checksum not retained for rb_frame=%d (skipped_total=%d)",
                frame,
                s_digestsSkippedNoHistory);
        }
        return;
    }

    if (local_crc == remote_crc) {
        s_digestsMatched++;
        NetplayLog_Verbose("DIGEST", frame,
            "Match: local=0x%08X remote=0x%08X", local_crc, remote_crc);
    } else {
        s_digestsMismatched++;
        RollbackDebug_ReportDrift(
            "StateDigest",
            frame,
            local_crc,
            remote_crc,
            "Authoritative match-region checksum mismatch on settled frame");
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
    s_lastRemoteStatusRecvMs = GetTickCount();
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
    out->digests_skipped_not_settled = s_digestsSkippedNotSettled;
    out->digests_skipped_no_history = s_digestsSkippedNoHistory;
    out->digests_skipped_remote_unsettled = s_digestsSkippedRemoteUnsettled;
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
        ImGui::Text("Avg Ping:         %.1f ms", snap.link_avg_ping);
        ImGui::Text("Jitter:           %.1f ms", snap.link_jitter);
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
