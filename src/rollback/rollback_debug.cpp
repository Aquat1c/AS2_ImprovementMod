/**
 * Alice Senki 2 - Rollback Debug Implementation
 */

#include "rollback/rollback_debug.h"
#include "rollback/rollback_session.h"
#include "rollback/resimulation.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "net/delay_policy.h"
#include "net/sync_policy.h"
#include "net/session_manager.h"
#include "net/protocol.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"
#include "imgui.h"

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
static int32_t  s_remoteStatusFrame          = -1;
static int32_t  s_remoteStatusGameFrame      = -1;
static int32_t  s_remoteStatusViewFrame      = -1;
static int32_t  s_remoteStatusConfirmedFrame = -1;
static int32_t  s_remoteStatusPredicted      = 0;
static uint32_t s_remoteStatusChecksum       = 0;

// Current frame checksum (cached)
static uint32_t s_currentChecksum   = 0;

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
    s_desyncDetected = false;
    s_desyncFrame = -1;
    s_digestsSent = 0;
    s_digestsRecv = 0;
    s_digestsMatched = 0;
    s_digestsMismatched = 0;
    s_lastDigestFrame = -1;
    s_statusSent = 0;
    s_statusRecv = 0;
    s_remoteStatusFrame = -1;
    s_remoteStatusGameFrame = -1;
    s_remoteStatusViewFrame = -1;
    s_remoteStatusConfirmedFrame = -1;
    s_remoteStatusPredicted = 0;
    s_remoteStatusChecksum = 0;
    s_currentChecksum = 0;
    ResetChecksumHistory();
    s_initialized = true;
    LOG_INFO("[RollbackDebug] Initialized");
}

void RollbackDebug_Shutdown() {
    s_initialized = false;
}

// ============================================================================
// Per-Frame
// ============================================================================

void RollbackDebug_FrameUpdate() {
    if (!s_initialized) return;
    if (!RollbackSession_IsActive()) return;
    if (RollbackSession_IsRollingBack()) return;  // Don't log during resim

    int32_t frame = RollbackSession_GetCurrentFrame();

    // Compute current state checksum
    __try {
        s_currentChecksum = CalcCRC32(
            (const void*)ADDR_MATCH_BASE,
            (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_currentChecksum = 0xDEADDEAD;
    }

    StoreChecksumForFrame(frame, s_currentChecksum);

    // Send state digest at configured interval
    if (s_digestEnabled && s_digestInterval > 0 &&
        (frame % s_digestInterval == 0) && frame > 0 &&
        Net::Session_IsConnected()) {

        Net::StateDigestPayload digest;
        digest.frame_number = (uint32_t)frame;
        digest.crc32 = s_currentChecksum;

        Net::Session_SendPacket(
            Net::CHANNEL_DEBUG,
            Net::PacketType::StateDigest,
            &digest, sizeof(digest),
            false  // Unreliable
        );

        s_digestsSent++;
        s_lastDigestFrame = frame;

        NetplayLog_Verbose("DIGEST", frame,
            "Sent: crc=0x%08X", s_currentChecksum);
    }

    if (s_digestEnabled && s_statusInterval > 0 &&
        (frame % s_statusInterval == 0) && Net::Session_IsConnected()) {

        Net::FrameSyncStatusPayload status{};
        status.current_frame = frame;
        status.game_frame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

        RollbackSessionSnapshot rbSnap{};
        RollbackSession_GetSnapshot(&rbSnap);
        status.remote_view_frame = 0;  // GekkoNet manages this internally
        status.confirmed_frame = rbSnap.last_confirmed_frame;
        status.predicted_frames = rbSnap.predicted_frames_outstanding;
        status.checksum = s_currentChecksum;

        Net::Session_SendPacket(
            Net::CHANNEL_DEBUG,
            Net::PacketType::FrameSyncStatus,
            &status, sizeof(status),
            false
        );

        s_statusSent++;

        NetplayLog_Verbose("FSYNC", frame,
            "Sent status: game=%d remote_view=%d confirmed=%d predicted=%d crc=0x%08X",
            status.game_frame,
            status.remote_view_frame,
            status.confirmed_frame,
            status.predicted_frames,
            status.checksum);
    }

    // Rate-limited diagnostics log (every 300 frames = ~5 seconds)
    if (frame > 0 && frame % 300 == 0) {
        RollbackSessionSnapshot snap;
        RollbackSession_GetSnapshot(&snap);

        LOG_INFO("[RollbackDebug] f=%d conf=%d remote=%d "
                 "rb=%d maxrb=%d pred=%d misp=%d "
                 "delay=%d budget=%d crc=0x%08X",
            snap.current_frame, snap.last_confirmed_frame,
            snap.last_remote_received_frame,
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

    // Only compare if we have a confirmed (non-predicted) state for this frame.
    // With GekkoNet, use last_confirmed_frame from the snapshot.
    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);
    if (frame > snap.last_confirmed_frame) return;

    uint32_t local_crc = 0;
    if (!TryGetChecksumForFrame(frame, &local_crc)) {
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
    }
}

void RollbackDebug_OnRemoteFrameSyncStatus(int32_t remote_frame,
                                           int32_t remote_game_frame,
                                           int32_t remote_view_frame,
                                           int32_t remote_confirmed_frame,
                                           int32_t remote_predicted_frames,
                                           uint32_t remote_checksum) {
    s_statusRecv++;
    s_remoteStatusFrame = remote_frame;
    s_remoteStatusGameFrame = remote_game_frame;
    s_remoteStatusViewFrame = remote_view_frame;
    s_remoteStatusConfirmedFrame = remote_confirmed_frame;
    s_remoteStatusPredicted = remote_predicted_frames;
    s_remoteStatusChecksum = remote_checksum;

    if (!RollbackSession_IsActive()) {
        return;
    }

    const int32_t localFrame = RollbackSession_GetCurrentFrame();
    const int32_t localGameFrame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    const int32_t frameDelta = localFrame - remote_frame;
    const int32_t gameDelta = localGameFrame - remote_game_frame;
    const int32_t remoteViewDelta = localFrame - remote_view_frame;

    NetplayLog_Verbose("FSYNC", localFrame,
        "Remote status: remote_frame=%d remote_game=%d remote_view=%d confirmed=%d predicted=%d "
        "frame_delta=%d game_delta=%d remote_view_delta=%d crc=0x%08X",
        remote_frame,
        remote_game_frame,
        remote_view_frame,
        remote_confirmed_frame,
        remote_predicted_frames,
        frameDelta,
        gameDelta,
        remoteViewDelta,
        remote_checksum);

    if (frameDelta > 2 || frameDelta < -2 ||
        gameDelta > 2 || gameDelta < -2 ||
        remoteViewDelta > 4 || remoteViewDelta < -4) {
        NetplayLog_Write("FSYNC", localFrame,
            "SKEW: local_frame=%d remote_frame=%d local_game=%d remote_game=%d remote_view=%d "
            "confirmed=%d predicted=%d local_crc=0x%08X remote_crc=0x%08X",
            localFrame,
            remote_frame,
            localGameFrame,
            remote_game_frame,
            remote_view_frame,
            remote_confirmed_frame,
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
        ImGui::Text("Current Frame:    %d", snap.current_frame);
        ImGui::Text("Confirmed Frame:  %d", snap.last_confirmed_frame);
        ImGui::Text("Remote Received:  %d", snap.last_remote_received_frame);
        ImGui::Text("Last Saved State: %d", snap.last_saved_state_frame);

        // Policy
        ImGui::Separator();
        ImGui::Text("Active Delay:     %d", snap.active_delay);
        ImGui::Text("Rollback Budget:  %d", snap.rollback_budget);

        // Rollback stats
        ImGui::Separator();
        ImGui::Text("Rollback Count:   %d", snap.rollback_count);
        ImGui::Text("Max Depth:        %d", snap.max_rollback_distance);
        ImGui::Text("Last RB Frame:    %d", snap.last_rollback_start_frame);
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
