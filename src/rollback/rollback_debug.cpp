/**
 * Alice Senki 2 - Rollback Debug Implementation
 */

#include "rollback/rollback_debug.h"
#include "rollback/rollback_session.h"
#include "rollback/resimulation.h"
#include "rollback/input_timeline.h"
#include "rollback/prediction.h"
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

// Current frame checksum (cached)
static uint32_t s_currentChecksum   = 0;

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
    s_currentChecksum = 0;
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
    if (RollbackSession_IsResimulating()) return;  // Don't log during resim

    int32_t frame = RollbackSession_GetCurrentFrame();

    // Compute current state checksum
    __try {
        s_currentChecksum = CalcCRC32(
            (const void*)ADDR_MATCH_BASE,
            (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_currentChecksum = 0xDEADDEAD;
    }

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
    // If we haven't reached that frame yet, or are still predicting, skip.
    int32_t confirmed = RollbackSession_GetLastConfirmedFrame();
    if (frame > confirmed) return;

    // Compute what our state was at that frame (we can only check current).
    // For true per-frame comparison, we'd need to store checksums per frame.
    // For now, if the remote digest frame matches our current frame, compare directly.
    int32_t current = RollbackSession_GetCurrentFrame();
    if (frame != current - 1) return;  // Only compare on matching frames

    uint32_t local_crc = s_currentChecksum;

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
        if (snap.is_resimulating) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,1,0,1), "RESIMULATING");
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
