/**
 * Alice Senki 2 - Adaptive Delay Implementation
 *
 * Tracks rollback frequency and negotiates mid-match delay changes
 * between both peers using a two-phase protocol:
 *   1. DelayChangeRequest(requested_delay, effective_frame, seq)
 *   2. DelayChangeAck(accepted_delay, effective_frame, seq)
 *   3. At effective_frame, both sides call RollbackSession::SetDelay()
 */

#include "adaptive_delay.h"
#include "rollback_session.h"
#include "session_manager.h"
#include "log_window.h"
#include "netplay_hooks.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

namespace AdaptiveDelay {

// ============================================================================
// Defaults
// ============================================================================

static constexpr int   kDefaultMinDelay       = 1;  // floor: delay=0 is never safe for online
static constexpr int   kDefaultMaxDelay       = 6;
static constexpr int   kDefaultWindowSize     = 180;   // 3 seconds at 60fps
static constexpr float kDefaultThresholdUp    = 0.30f;
static constexpr float kDefaultThresholdDown  = 0.03f; // 3% — only decrease when rollbacks are very rare
static constexpr int   kDefaultCooldownFrames = 300;   // 5 seconds (increase direction)
static constexpr int   kDefaultCooldownFramesDown = 600; // 10 seconds (decrease direction — conservative)
static constexpr int   kDefaultLeadFrames     = 30;

// Request retransmit timeout
static constexpr int   kRequestTimeoutFrames  = 120;   // 2 seconds — abort if no ack

// ============================================================================
// Internal State
// ============================================================================

namespace {

static Config           s_config         = {};
static bool             s_initialized    = false;
static int              s_currentDelay   = 0;

// Rolling window for rollback tracking
static bool*            s_rollbackWindow = nullptr;  // circular buffer
static int              s_windowWriteIdx = 0;
static int              s_windowFrameCount = 0;      // frames written (up to window_size)
static int              s_rollbacksInWindow = 0;     // count of true entries

// Negotiation state machine
static NegotiationState s_negotiationState = NegotiationState::Idle;
static int              s_pendingDelay    = 0;
static int              s_effectiveFrame  = 0;
static uint32_t         s_requestSeq      = 0;       // monotonic per session
static uint32_t         s_lastRequestSeq  = 0;       // last seq we sent
static int              s_requestSentFrame = 0;       // frame when we sent the request

// Cooldown
static int              s_cooldownRemaining = 0;

// Dynamic RTT-based floor (updated every pong)
static int              s_rttFloor          = 1;

// Stats
static uint32_t         s_changesApplied = 0;

// Network context
static uint64_t         s_sessionId      = 0;
static uint32_t         s_connectionId   = 0;

// ============================================================================
// Helpers
// ============================================================================

static void SendDelayChangeRequest(int requestedDelay, int effectiveFrame) {
    PacketCodec::DelayChangeRequestPayload payload = {};
    payload.requested_delay = (uint32_t)requestedDelay;
    payload.effective_frame = (uint32_t)effectiveFrame;
    payload.request_seq     = s_requestSeq;

    SessionManager::SendToPeer(PacketCodec::PacketType::DelayChangeRequest,
                               &payload, sizeof(payload), true);

    LOG_NET_INFO("[AdaptiveDelay] Sent DelayChangeRequest: delay=%d effective_frame=%d seq=%u",
                 requestedDelay, effectiveFrame, s_requestSeq);
}

static void SendDelayChangeAck(int acceptedDelay, int effectiveFrame, uint32_t reqSeq) {
    PacketCodec::DelayChangeAckPayload payload = {};
    payload.accepted_delay  = (uint32_t)acceptedDelay;
    payload.effective_frame = (uint32_t)effectiveFrame;
    payload.request_seq     = reqSeq;

    SessionManager::SendToPeer(PacketCodec::PacketType::DelayChangeAck,
                               &payload, sizeof(payload), true);

    LOG_NET_INFO("[AdaptiveDelay] Sent DelayChangeAck: delay=%d effective_frame=%d seq=%u",
                 acceptedDelay, effectiveFrame, reqSeq);
}

static int ClampDelay(int delay) {
    if (delay < s_config.min_delay) return s_config.min_delay;
    if (delay > s_config.max_delay) return s_config.max_delay;
    return delay;
}

static void ApplyDelayChange() {
    int clamped = ClampDelay(s_pendingDelay);
    int oldDelay = s_currentDelay;
    s_currentDelay = clamped;

    // Apply to GekkoNet (safe now that InputBuffer::SetDelay is fixed)
    RollbackSession::SetDelay(clamped);

    // Update the preference so it persists across rounds
    NetplayHooks::SetNetplayFrameDelay(clamped);

    s_changesApplied++;
    // Asymmetric cooldown: conservative for decrease, responsive for increase
    s_cooldownRemaining = (clamped < oldDelay) ? kDefaultCooldownFramesDown : s_config.cooldown_frames;
    s_negotiationState = NegotiationState::Idle;

    LOG_NET_INFO("[AdaptiveDelay] APPLIED delay change: %d -> %d at frame %d (total changes=%u)",
                 oldDelay, clamped, s_effectiveFrame, s_changesApplied);
}

static float ComputeRollbackRate() {
    if (s_windowFrameCount == 0) return 0.0f;
    return (float)s_rollbacksInWindow / (float)s_windowFrameCount;
}

static void RecordFrame(bool hadRollback) {
    if (!s_rollbackWindow) return;

    int windowSize = s_config.window_size;
    int idx = s_windowWriteIdx % windowSize;

    // If the window is full, subtract the oldest entry
    if (s_windowFrameCount >= windowSize) {
        if (s_rollbackWindow[idx]) {
            s_rollbacksInWindow--;
        }
    } else {
        s_windowFrameCount++;
    }

    // Write the new entry
    s_rollbackWindow[idx] = hadRollback;
    if (hadRollback) {
        s_rollbacksInWindow++;
    }
    s_windowWriteIdx++;
}

static void EvaluateHeuristic(int currentFrame) {
    if (!s_config.enabled) return;
    if (s_negotiationState != NegotiationState::Idle) return;
    if (s_cooldownRemaining > 0) return;
    if (s_windowFrameCount < s_config.window_size / 2) return;  // Need enough data

    float rate = ComputeRollbackRate();

    // Check if we should increase delay (too many rollbacks)
    if (rate >= s_config.threshold_up && s_currentDelay < s_config.max_delay) {
        int newDelay = ClampDelay(s_currentDelay + 1);
        if (newDelay != s_currentDelay) {
            s_requestSeq++;
            s_lastRequestSeq = s_requestSeq;
            s_pendingDelay = newDelay;
            s_effectiveFrame = currentFrame + s_config.lead_frames;
            s_requestSentFrame = currentFrame;
            s_negotiationState = NegotiationState::RequestSent;

            SendDelayChangeRequest(newDelay, s_effectiveFrame);
            LOG_NET_INFO("[AdaptiveDelay] Heuristic: rollback rate %.1f%% > %.1f%% — requesting delay %d->%d at frame %d",
                         rate * 100.0f, s_config.threshold_up * 100.0f,
                         s_currentDelay, newDelay, s_effectiveFrame);
        }
        return;
    }

    // Check if we should decrease delay (very few rollbacks)
    // Use strict < (rate AT threshold should not trigger decrease)
    int effectiveMin = (s_rttFloor > s_config.min_delay) ? s_rttFloor : s_config.min_delay;
    if (rate < s_config.threshold_down && s_currentDelay > effectiveMin) {
        int newDelay = ClampDelay(s_currentDelay - 1);
        if (newDelay != s_currentDelay) {
            s_requestSeq++;
            s_lastRequestSeq = s_requestSeq;
            s_pendingDelay = newDelay;
            s_effectiveFrame = currentFrame + s_config.lead_frames;
            s_requestSentFrame = currentFrame;
            s_negotiationState = NegotiationState::RequestSent;

            SendDelayChangeRequest(newDelay, s_effectiveFrame);
            LOG_NET_INFO("[AdaptiveDelay] Heuristic: rollback rate %.1f%% < %.1f%% — requesting delay %d->%d at frame %d",
                         rate * 100.0f, s_config.threshold_down * 100.0f,
                         s_currentDelay, newDelay, s_effectiveFrame);
        }
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

void Init(const Config* config, int initialDelay) {
    if (config) {
        memcpy(&s_config, config, sizeof(Config));
    } else {
        // Use defaults
        s_config.min_delay       = kDefaultMinDelay;
        s_config.max_delay       = kDefaultMaxDelay;
        s_config.window_size     = kDefaultWindowSize;
        s_config.threshold_up    = kDefaultThresholdUp;
        s_config.threshold_down  = kDefaultThresholdDown;
        s_config.cooldown_frames = kDefaultCooldownFrames;
        s_config.lead_frames     = kDefaultLeadFrames;
        s_config.enabled         = true;
    }

    // Validate
    if (s_config.window_size < 10) s_config.window_size = 10;
    if (s_config.window_size > 600) s_config.window_size = 600;
    if (s_config.lead_frames < 10) s_config.lead_frames = 10;
    if (s_config.cooldown_frames < 60) s_config.cooldown_frames = 60;

    // Allocate rolling window
    if (s_rollbackWindow) {
        free(s_rollbackWindow);
    }
    s_rollbackWindow = (bool*)calloc(s_config.window_size, sizeof(bool));
    s_windowWriteIdx = 0;
    s_windowFrameCount = 0;
    s_rollbacksInWindow = 0;

    s_currentDelay = ClampDelay(initialDelay);
    s_negotiationState = NegotiationState::Idle;
    s_pendingDelay = 0;
    s_effectiveFrame = 0;
    s_requestSeq = 0;
    s_lastRequestSeq = 0;
    s_requestSentFrame = 0;
    s_cooldownRemaining = 0;
    s_changesApplied = 0;
    s_initialized = true;

    LOG_NET_INFO("[AdaptiveDelay] Initialized: delay=%d min=%d max=%d window=%d "
                 "up_thresh=%.0f%% down_thresh=%.0f%% cooldown=%d lead=%d enabled=%d",
                 s_currentDelay, s_config.min_delay, s_config.max_delay,
                 s_config.window_size,
                 s_config.threshold_up * 100.0f, s_config.threshold_down * 100.0f,
                 s_config.cooldown_frames, s_config.lead_frames,
                 s_config.enabled ? 1 : 0);
}

void Shutdown() {
    if (s_rollbackWindow) {
        free(s_rollbackWindow);
        s_rollbackWindow = nullptr;
    }
    s_initialized = false;
    s_negotiationState = NegotiationState::Idle;
    LOG_NET_INFO("[AdaptiveDelay] Shutdown (applied %u changes this session)", s_changesApplied);
}

void OnFrame(int currentFrame, bool wasRollback) {
    if (!s_initialized) return;

    // Track rollback history
    RecordFrame(wasRollback);

    // Tick cooldown
    if (s_cooldownRemaining > 0) {
        s_cooldownRemaining--;
    }

    // State machine
    switch (s_negotiationState) {
    case NegotiationState::Idle:
        // Auto-evaluate heuristic
        EvaluateHeuristic(currentFrame);
        break;

    case NegotiationState::RequestSent:
        // Timeout: if no ack received within kRequestTimeoutFrames, abort
        if (currentFrame - s_requestSentFrame > kRequestTimeoutFrames) {
            LOG_NET_WARN("[AdaptiveDelay] Request timeout (seq=%u), reverting to Idle",
                         s_lastRequestSeq);
            s_negotiationState = NegotiationState::Idle;
            s_cooldownRemaining = s_config.cooldown_frames / 2;  // shorter cooldown after timeout
        }
        break;

    case NegotiationState::RequestReceived:
    case NegotiationState::Agreed:
        // Check if we've reached the effective frame
        if (currentFrame >= s_effectiveFrame) {
            ApplyDelayChange();
        }
        break;
    }
}

bool RequestManualChange(int direction) {
    if (!s_initialized || !s_config.enabled) return false;
    if (s_negotiationState != NegotiationState::Idle) return false;

    int newDelay = ClampDelay(s_currentDelay + direction);
    if (newDelay == s_currentDelay) return false;

    int currentFrame = RollbackSession::GetCurrentFrame();
    if (currentFrame < 0) return false;

    s_requestSeq++;
    s_lastRequestSeq = s_requestSeq;
    s_pendingDelay = newDelay;
    s_effectiveFrame = currentFrame + s_config.lead_frames;
    s_requestSentFrame = currentFrame;
    s_negotiationState = NegotiationState::RequestSent;

    SendDelayChangeRequest(newDelay, s_effectiveFrame);
    LOG_NET_INFO("[AdaptiveDelay] Manual change requested: %d->%d at frame %d (hotkey %s)",
                 s_currentDelay, newDelay, s_effectiveFrame,
                 direction > 0 ? "increase" : "decrease");
    return true;
}

void OnReceiveDelayChangeRequest(const PacketCodec::DelayChangeRequestPayload* payload) {
    if (!s_initialized || !payload) return;

    int requestedDelay = (int)payload->requested_delay;
    int effectiveFrame = (int)payload->effective_frame;
    uint32_t reqSeq = payload->request_seq;

    LOG_NET_INFO("[AdaptiveDelay] Received DelayChangeRequest: delay=%d frame=%d seq=%u (our state=%d)",
                 requestedDelay, effectiveFrame, reqSeq, (int)s_negotiationState);

    // If we have a pending request ourselves, the lower seq wins (tie to host)
    if (s_negotiationState == NegotiationState::RequestSent) {
        if (reqSeq < s_lastRequestSeq) {
            // Their request is older — reject ours, accept theirs
            LOG_NET_INFO("[AdaptiveDelay] Conflict resolution: their seq %u < our seq %u — accepting theirs",
                         reqSeq, s_lastRequestSeq);
        } else if (reqSeq > s_lastRequestSeq) {
            // Our request is older — ignore theirs, keep waiting for our ack
            LOG_NET_INFO("[AdaptiveDelay] Conflict resolution: their seq %u > our seq %u — keeping ours",
                         reqSeq, s_lastRequestSeq);
            return;
        } else {
            // Same seq (shouldn't happen) — host wins
            if (SessionManager::IsHost()) {
                LOG_NET_INFO("[AdaptiveDelay] Conflict tie seq=%u — we're host, keeping ours", reqSeq);
                return;
            }
            LOG_NET_INFO("[AdaptiveDelay] Conflict tie seq=%u — we're client, accepting theirs", reqSeq);
        }
    }

    // Clamp the requested delay to our bounds
    int clamped = ClampDelay(requestedDelay);

    // Accept the request
    s_pendingDelay = clamped;
    s_effectiveFrame = effectiveFrame;
    s_negotiationState = NegotiationState::RequestReceived;

    SendDelayChangeAck(clamped, effectiveFrame, reqSeq);
}

void OnReceiveDelayChangeAck(const PacketCodec::DelayChangeAckPayload* payload) {
    if (!s_initialized || !payload) return;

    int acceptedDelay = (int)payload->accepted_delay;
    int effectiveFrame = (int)payload->effective_frame;
    uint32_t reqSeq = payload->request_seq;

    LOG_NET_INFO("[AdaptiveDelay] Received DelayChangeAck: delay=%d frame=%d seq=%u (our state=%d, expected seq=%u)",
                 acceptedDelay, effectiveFrame, reqSeq, (int)s_negotiationState, s_lastRequestSeq);

    // Only accept if we're waiting for an ack and the seq matches
    if (s_negotiationState != NegotiationState::RequestSent) {
        LOG_NET_WARN("[AdaptiveDelay] Ignoring unexpected ack (state=%d)", (int)s_negotiationState);
        return;
    }
    if (reqSeq != s_lastRequestSeq) {
        LOG_NET_WARN("[AdaptiveDelay] Ignoring ack with mismatched seq (got %u, expected %u)",
                     reqSeq, s_lastRequestSeq);
        return;
    }

    // Accept the agreed values (peer may have clamped our request)
    s_pendingDelay = ClampDelay(acceptedDelay);
    s_effectiveFrame = effectiveFrame;
    s_negotiationState = NegotiationState::Agreed;

    LOG_NET_INFO("[AdaptiveDelay] Negotiation agreed: delay=%d at frame %d", s_pendingDelay, s_effectiveFrame);
}

void SetNetworkContext(uint64_t sessionId, uint32_t connectionId) {
    s_sessionId = sessionId;
    s_connectionId = connectionId;
}

bool IsActive() {
    return s_initialized;
}

bool GetSnapshot(Snapshot* out) {
    if (!out) return false;

    out->negotiation_state = s_negotiationState;
    out->current_delay = s_currentDelay;
    out->pending_delay = s_pendingDelay;
    out->effective_frame = s_effectiveFrame;
    out->rollback_rate = ComputeRollbackRate();
    out->cooldown_remaining = s_cooldownRemaining;
    out->changes_applied = s_changesApplied;
    out->enabled = s_config.enabled;

    return true;
}

int GetCurrentDelay() {
    return s_currentDelay;
}

float GetRollbackRate() {
    return ComputeRollbackRate();
}

uint32_t GetChangesApplied() {
    return s_changesApplied;
}

void UpdateRttFloor(float rttMs) {
    if (!s_initialized || rttMs <= 0.0f) return;
    // One-way trip in frames: ceil(rtt_ms * 60 / 1000 / 2), minimum 1
    int floor = (int)ceilf(rttMs * 60.0f / 1000.0f / 2.0f);
    if (floor < 1) floor = 1;
    if (floor != s_rttFloor) {
        LOG_NET_DEBUG("[AdaptiveDelay] RTT floor updated: %d -> %d (rtt=%.1fms)", s_rttFloor, floor, rttMs);
        s_rttFloor = floor;
    }
}

} // namespace AdaptiveDelay
