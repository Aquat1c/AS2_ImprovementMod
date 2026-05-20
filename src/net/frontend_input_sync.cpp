#include "net/frontend_input_sync.h"

#include "net/barrier_protocol.h"
#include "net/delay_policy.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

constexpr int FRONTEND_RING_SIZE = 512;
constexpr int FRONTEND_RING_MASK = FRONTEND_RING_SIZE - 1;
constexpr int FRONTEND_INPUT_REDUNDANCY =
    (int)(sizeof(((CharSelFrameInputPayload*)0)->inputs) / sizeof(uint16_t));
constexpr DWORD FRONTEND_TIMEOUT_MS = 10000;
constexpr DWORD FRONTEND_RESEND_INTERVAL_MS = 100;
constexpr DWORD FRONTEND_TARGETED_RESEND_INTERVAL_MS = 100;
constexpr DWORD FRONTEND_DELAY_BUMP_INTERVAL_MS = 1500;
constexpr DWORD FRONTEND_PRESSURE_SAMPLE_INTERVAL_MS = 250;
constexpr DWORD FRONTEND_STARVATION_PRESSURE_MS = 900;
constexpr uint8_t FRONTEND_JITTER_PRESSURE_SAMPLE_THRESHOLD = 3;
constexpr uint8_t FRONTEND_STARVATION_PRESSURE_SAMPLE_THRESHOLD = 2;
constexpr float FRONTEND_JITTER_IGNORE_MS = 4.0f;
constexpr float FRONTEND_JITTER_BUMP_FRAME_MS = FRAME_TIME_MS * 0.75f;
constexpr uint32_t FRONTEND_SEND_HEAD_BUFFER = FRONTEND_RING_SIZE / 2;

static_assert((FRONTEND_RING_SIZE & (FRONTEND_RING_SIZE - 1)) == 0,
    "Frontend input ring size must be a power of two");
static_assert(FRONTEND_INPUT_REDUNDANCY > 0,
    "Frontend packets must carry redundant input history");

static bool              s_initialized = false;
static bool              s_epochActive = false;
static SessionRole       s_role = SessionRole::Host;
static uint32_t          s_epochId = 0;
static FrontendSyncPhase s_phase = FrontendSyncPhase::None;
static PacketType        s_packetType = PacketType::CharSelFrameInput;
static bool              s_inputPhaseActive = false;

static uint32_t          s_consumeFrame = 0;
static uint32_t          s_localInputFrame = 0;
static uint32_t          s_remoteLatestFrame = 0;
static uint32_t          s_remoteAckFrame = 0;
static uint16_t          s_localInputs[FRONTEND_RING_SIZE] = {};
static uint16_t          s_remoteInputs[FRONTEND_RING_SIZE] = {};
static uint32_t          s_localInputIds[FRONTEND_RING_SIZE] = {};
static uint32_t          s_remoteInputIds[FRONTEND_RING_SIZE] = {};
static bool              s_hasLocalInput[FRONTEND_RING_SIZE] = {};
static bool              s_hasRemoteInput[FRONTEND_RING_SIZE] = {};

static uint16_t          s_localDelayProposal = FRONTEND_DELAY_MIN;
static uint16_t          s_remoteDelayProposal = 0;
static bool              s_remoteDelayProposalSeen = false;
static uint16_t          s_sharedDelay = FRONTEND_DELAY_MIN;
static bool              s_delayNegotiated = false;
static bool              s_remoteSharedDelaySeen = false;

static DWORD             s_lastRemoteInputTime = 0;
static DWORD             s_lastResendTime = 0;
static DWORD             s_lastTargetedResendTime = 0;
static DWORD             s_lastRingWindowPressureLogTime = 0;
static DWORD             s_lastRemoteFrameTraceLogTime = 0;
static bool              s_remoteFrameTraceLogTimeValid = false;
static DWORD             s_lastDelayBumpRequestTime = 0;
static DWORD             s_waitingForCurrentFrameSince = 0;
static DWORD             s_lastPressureSampleTime = 0;
static uint8_t           s_jitterPressureSamples = 0;
static uint8_t           s_starvationPressureSamples = 0;
static bool              s_receivedRemoteInputThisPhase = false;
static bool              s_timedOut = false;

static bool              s_pendingDelayBump = false;
static uint16_t          s_pendingDelay = 0;
static uint32_t          s_pendingApplyFrom = 0;
static FrontendDelayBumpReason s_pendingDelayReason = FrontendDelayBumpReason::None;

static bool              s_waitingForDelayAck = false;
static uint16_t          s_requestedDelay = 0;
static uint32_t          s_requestedApplyFrom = 0;
static FrontendDelayBumpReason s_requestedDelayReason = FrontendDelayBumpReason::None;

static bool              s_localPhaseBarrierSent = false;
static bool              s_remotePhaseBarrierSeen = false;
static FrontendSyncPhase s_barrierNextPhase = FrontendSyncPhase::None;
static uint32_t          s_localBarrierFrame = 0;
static uint32_t          s_remoteBarrierFrame = 0;

static bool              s_localDigestSent = false;
static bool              s_remoteDigestSeen = false;
static bool              s_digestMatch = false;
static bool              s_digestMismatch = false;
static FrontendDigestKind s_digestKind = FrontendDigestKind::None;
static uint32_t          s_localDigest = 0;
static uint32_t          s_remoteDigest = 0;

static bool              s_localAdvanceObserved = false;
static bool              s_remoteAdvanceObserved = false;

static char              s_recoveryReason[128] = {};

#if defined(AS2_FRONTEND_SYNC_TESTING)
static bool              s_testClockOverrideActive = false;
static DWORD             s_testClockMs = 0;
#endif

struct FrontendDelayProposalDetails {
    uint16_t configured_delay;
    uint16_t configured_floor;
    uint16_t base_delay;
    uint16_t jitter_bump;
    uint16_t recommended_delay;
    float    avg_ping_ms;
    float    rtt_variance_ms;
    float    one_way_frames;
    float    jitter_frames;
    bool     measurement_valid;
};

static uint16_t ClampFrontendDelay(uint16_t delay);

static DWORD NowMs() {
#if defined(AS2_FRONTEND_SYNC_TESTING)
    if (s_testClockOverrideActive) {
        return s_testClockMs;
    }
#endif
    return GetTickCount();
}

static bool ShouldLogRemoteFrameTrace(uint32_t frame, DWORD now) {
    if (frame < 5 || (frame % 120u) == 0u) {
        if (!s_remoteFrameTraceLogTimeValid ||
            (now - s_lastRemoteFrameTraceLogTime) >= 250) {
            s_lastRemoteFrameTraceLogTime = now;
            s_remoteFrameTraceLogTimeValid = true;
            return true;
        }
    }
    return false;
}

static uint16_t ComputeJitterBumpFrames(float varianceMs) {
    if (varianceMs <= FRONTEND_JITTER_IGNORE_MS) {
        return 0;
    }

    const float effectiveVarianceMs = varianceMs - FRONTEND_JITTER_IGNORE_MS;
    int bump = (int)ceilf(effectiveVarianceMs / FRONTEND_JITTER_BUMP_FRAME_MS);
    if (bump < 0) {
        bump = 0;
    }
    if (bump > (FRONTEND_DELAY_MAX - FRONTEND_DELAY_MIN)) {
        bump = FRONTEND_DELAY_MAX - FRONTEND_DELAY_MIN;
    }
    return (uint16_t)bump;
}

static FrontendDelayProposalDetails ComputeDelayProposalDetails() {
    FrontendDelayProposalDetails details{};
    NetworkMeasurement measurement{};
    DelayPolicy_GetMeasurement(&measurement);

    details.measurement_valid = measurement.valid;
    details.avg_ping_ms = measurement.avg_ping_ms;
    details.rtt_variance_ms = measurement.rtt_variance_ms;
    details.one_way_frames = measurement.one_way_frames;
    details.jitter_frames = measurement.jitter_frames;
    details.configured_delay = (uint16_t)DelayPolicy_GetConfiguredDelay();
    details.configured_floor = ClampFrontendDelay((uint16_t)(details.configured_delay + 2));
    details.base_delay = ClampFrontendDelay((uint16_t)(DelayPolicy_ComputeRecommendedDelay() + 2));
    if (details.base_delay < details.configured_floor) {
        details.base_delay = details.configured_floor;
    }
    details.jitter_bump = measurement.valid
        ? ComputeJitterBumpFrames(measurement.rtt_variance_ms)
        : 0;
    details.recommended_delay = ClampFrontendDelay((uint16_t)(details.base_delay + details.jitter_bump));
    return details;
}

static FrontendDelayBumpReason SanitizeDelayReason(uint8_t reasonCode) {
    switch ((FrontendDelayBumpReason)reasonCode) {
        case FrontendDelayBumpReason::None:
        case FrontendDelayBumpReason::Starvation:
        case FrontendDelayBumpReason::RemoteRequest:
        case FrontendDelayBumpReason::HostAdjust:
        case FrontendDelayBumpReason::JitterPressure:
            return (FrontendDelayBumpReason)reasonCode;
        default:
            return FrontendDelayBumpReason::None;
    }
}

static void ResetDelayPressureTracking() {
    s_waitingForCurrentFrameSince = 0;
    s_lastPressureSampleTime = 0;
    s_jitterPressureSamples = 0;
    s_starvationPressureSamples = 0;
}

static uint16_t ClampFrontendDelay(uint16_t delay) {
    if (delay < FRONTEND_DELAY_MIN) return FRONTEND_DELAY_MIN;
    if (delay > FRONTEND_DELAY_MAX) return FRONTEND_DELAY_MAX;
    return delay;
}

static uint16_t GetEffectiveDelayFloor() {
    uint16_t floor = s_sharedDelay;
    if (s_pendingDelayBump && s_pendingDelay > floor) {
        floor = s_pendingDelay;
    }
    if (s_waitingForDelayAck && s_requestedDelay > floor) {
        floor = s_requestedDelay;
    }
    return floor;
}

static bool HasLocalInputFrame(uint32_t frame) {
    const int idx = (int)(frame & FRONTEND_RING_MASK);
    return s_hasLocalInput[idx] && s_localInputIds[idx] == frame;
}

static bool HasRemoteInputFrame(uint32_t frame) {
    const int idx = (int)(frame & FRONTEND_RING_MASK);
    return s_hasRemoteInput[idx] && s_remoteInputIds[idx] == frame;
}

static void StoreLocalInputFrame(uint32_t frame, uint16_t input) {
    const int idx = (int)(frame & FRONTEND_RING_MASK);
    s_localInputs[idx] = input;
    s_localInputIds[idx] = frame;
    s_hasLocalInput[idx] = true;
}

static void StoreRemoteInputFrame(uint32_t frame, uint16_t input) {
    const int idx = (int)(frame & FRONTEND_RING_MASK);
    s_remoteInputs[idx] = input;
    s_remoteInputIds[idx] = frame;
    s_hasRemoteInput[idx] = true;
}

static uint32_t GetLocalSendHeadLimit() {
    uint32_t limit = s_consumeFrame + FRONTEND_SEND_HEAD_BUFFER;
    const uint32_t ackLimited = s_remoteAckFrame + (uint32_t)FRONTEND_RING_SIZE - 1u;
    if (ackLimited < limit) {
        limit = ackLimited;
    }
    return limit;
}

static void ClearPhaseInputState() {
    s_consumeFrame = 0;
    s_localInputFrame = 0;
    s_remoteLatestFrame = 0;
    s_remoteAckFrame = 0;
    s_lastRemoteInputTime = NowMs();
    s_receivedRemoteInputThisPhase = false;
    s_lastResendTime = 0;
    s_lastTargetedResendTime = 0;
    s_lastRingWindowPressureLogTime = 0;
    s_lastRemoteFrameTraceLogTime = 0;
    s_remoteFrameTraceLogTimeValid = false;
    s_lastDelayBumpRequestTime = 0;
    s_timedOut = false;
    memset(s_localInputs, 0, sizeof(s_localInputs));
    memset(s_remoteInputs, 0, sizeof(s_remoteInputs));
    memset(s_localInputIds, 0, sizeof(s_localInputIds));
    memset(s_remoteInputIds, 0, sizeof(s_remoteInputIds));
    memset(s_hasLocalInput, 0, sizeof(s_hasLocalInput));
    memset(s_hasRemoteInput, 0, sizeof(s_hasRemoteInput));
    s_pendingDelayBump = false;
    s_pendingDelay = 0;
    s_pendingApplyFrom = 0;
    s_pendingDelayReason = FrontendDelayBumpReason::None;
    s_waitingForDelayAck = false;
    s_requestedDelay = 0;
    s_requestedApplyFrom = 0;
    s_requestedDelayReason = FrontendDelayBumpReason::None;
    s_localPhaseBarrierSent = false;
    s_remotePhaseBarrierSeen = false;
    s_barrierNextPhase = FrontendSyncPhase::None;
    s_localBarrierFrame = 0;
    s_remoteBarrierFrame = 0;
    s_localDigestSent = false;
    s_remoteDigestSeen = false;
    s_digestMatch = false;
    s_digestMismatch = false;
    s_digestKind = FrontendDigestKind::None;
    s_localDigest = 0;
    s_remoteDigest = 0;
    s_localAdvanceObserved = false;
    s_remoteAdvanceObserved = false;
    ResetDelayPressureTracking();
}

static void ClearEpochState() {
    s_epochActive = false;
    s_role = SessionRole::Host;
    s_epochId = 0;
    s_phase = FrontendSyncPhase::None;
    s_packetType = PacketType::CharSelFrameInput;
    s_inputPhaseActive = false;
    s_localDelayProposal = FRONTEND_DELAY_MIN;
    s_remoteDelayProposal = 0;
    s_remoteDelayProposalSeen = false;
    s_sharedDelay = FRONTEND_DELAY_MIN;
    s_delayNegotiated = false;
    s_remoteSharedDelaySeen = false;
    s_recoveryReason[0] = '\0';
    ClearPhaseInputState();
}

static void LogFrontendCore(const char* message) {
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "%s: epoch=%u phase=%s input_active=%d consume=%u local=%u remote=%u shared_delay=%u recovery=%s",
        message ? message : "state",
        s_epochId,
        FrontendSyncPhaseName(s_phase),
        s_inputPhaseActive ? 1 : 0,
        s_consumeFrame,
        s_localInputFrame,
        s_remoteLatestFrame,
        s_sharedDelay,
        s_recoveryReason[0] ? s_recoveryReason : "none");
}

static bool SendInputPacket(uint32_t frame, const char* reason) {
    if (!s_epochActive || !s_inputPhaseActive) {
        return false;
    }

    if (s_packetType == PacketType::CharSelFrameInput) {
        CharSelFrameInputPayload payload{};
        payload.epoch_id = s_epochId;
        payload.phase = (uint16_t)s_phase;
        payload.frame = frame;
        payload.ack_frame = s_consumeFrame;

        int count = 0;
        for (int i = 0; i < FRONTEND_INPUT_REDUNDANCY; i++) {
            const int f = (int)frame - i;
            if (f < 0) break;
            const int idx = f & FRONTEND_RING_MASK;
            if (!HasLocalInputFrame((uint32_t)f)) break;
            payload.inputs[i] = s_localInputs[idx];
            count++;
        }
        payload.input_count = (uint16_t)count;

        const bool sent = BarrierProtocol_SendPacket(
            PacketType::CharSelFrameInput,
            &payload,
            sizeof(payload));
        if (!sent) {
            Rollback::NetplayLog_Write(
                "FRONTEND", -1,
                "Send CharSelFrameInput failed: frame=%u ack=%u count=%u reason=%s",
                payload.frame,
                payload.ack_frame,
                payload.input_count,
                reason ? reason : "?");
        }
        return sent;
    }

    WinScreenFrameInputPayload payload{};
    payload.epoch_id = s_epochId;
    payload.phase = (uint16_t)s_phase;
    payload.frame = frame;
    payload.ack_frame = s_consumeFrame;

    int count = 0;
    for (int i = 0; i < FRONTEND_INPUT_REDUNDANCY; i++) {
        const int f = (int)frame - i;
        if (f < 0) break;
        const int idx = f & FRONTEND_RING_MASK;
        if (!HasLocalInputFrame((uint32_t)f)) break;
        payload.inputs[i] = s_localInputs[idx];
        count++;
    }
    payload.input_count = (uint16_t)count;

    const bool sent = BarrierProtocol_SendPacket(
        PacketType::WinScreenFrameInput,
        &payload,
        sizeof(payload));
    if (!sent) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Send WinScreenFrameInput failed: frame=%u ack=%u count=%u reason=%s",
            payload.frame,
            payload.ack_frame,
            payload.input_count,
            reason ? reason : "?");
    }
    return sent;
}

static void EnsureLocalLead(const char* reason) {
    if (!s_inputPhaseActive) {
        return;
    }

    const uint32_t targetLead = (uint32_t)(s_sharedDelay > 0 ? s_sharedDelay : FRONTEND_DELAY_MIN);
    const uint32_t maxSendFrame = GetLocalSendHeadLimit();
    while (s_localInputFrame < (s_consumeFrame + targetLead) && s_localInputFrame <= maxSendFrame) {
        StoreLocalInputFrame(s_localInputFrame, 0);
        SendInputPacket(s_localInputFrame, reason ? reason : "lead fill");
        s_localInputFrame++;
    }
}

static void MaybeApplyPendingDelay() {
    if (!s_pendingDelayBump) {
        return;
    }
    if (s_consumeFrame < s_pendingApplyFrom) {
        return;
    }

    const uint16_t oldDelay = s_sharedDelay;
    const FrontendDelayBumpReason appliedReason = s_pendingDelayReason;
    s_sharedDelay = ClampFrontendDelay(s_pendingDelay);
    s_pendingDelayBump = false;
    s_pendingDelay = 0;
    s_pendingApplyFrom = 0;
    s_pendingDelayReason = FrontendDelayBumpReason::None;
    s_waitingForDelayAck = false;
    s_requestedDelay = 0;
    s_requestedApplyFrom = 0;
    s_requestedDelayReason = FrontendDelayBumpReason::None;
    ResetDelayPressureTracking();

    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Applied shared frontend delay bump: old=%u new=%u phase=%s frame=%u reason=%s",
        oldDelay,
        s_sharedDelay,
        FrontendSyncPhaseName(s_phase),
        s_consumeFrame,
        FrontendDelayBumpReasonName(appliedReason));
}

static void SendDelayChangeAck(uint16_t ackedDelay,
                               uint32_t applyFromFrame,
                               bool accepted,
                               FrontendDelayBumpReason reason) {
    DelayChangeAckPayload ack{};
    ack.epoch_id = s_epochId;
    ack.phase = (uint16_t)s_phase;
    ack.acked_delay = ackedDelay;
    ack.apply_from_frame = applyFromFrame;
    ack.accepted = accepted ? 1 : 0;
    ack.reason_code = (uint8_t)reason;
    BarrierProtocol_SendPacket(PacketType::DelayChangeAck, &ack, sizeof(ack));
}

static void RequestDelayIncrease(uint16_t requestedDelay,
                                 FrontendDelayBumpReason reason,
                                 const char* context) {
    if (!s_epochActive || !s_inputPhaseActive || s_waitingForDelayAck) {
        return;
    }

    requestedDelay = ClampFrontendDelay(requestedDelay);
    if (requestedDelay <= GetEffectiveDelayFloor()) {
        return;
    }

    DelayChangeReqPayload req{};
    req.epoch_id = s_epochId;
    req.phase = (uint16_t)s_phase;
    req.new_delay = requestedDelay;
    req.apply_from_frame = s_consumeFrame + requestedDelay + 2;
    req.reason_code = (uint8_t)reason;

    if (!BarrierProtocol_SendPacket(PacketType::DelayChangeReq, &req, sizeof(req))) {
        return;
    }

    s_waitingForDelayAck = true;
    s_requestedDelay = requestedDelay;
    s_requestedApplyFrom = req.apply_from_frame;
    s_requestedDelayReason = reason;
    s_lastDelayBumpRequestTime = NowMs();

    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Requested frontend delay bump: phase=%s current=%u requested=%u apply_from=%u reason=%s context=%s",
        FrontendSyncPhaseName(s_phase),
        s_sharedDelay,
        requestedDelay,
        req.apply_from_frame,
        FrontendDelayBumpReasonName(reason),
        context ? context : "?");
}

static void MaybeRequestLiveDelayIncrease(DWORD now) {
    if (!s_epochActive ||
        !s_inputPhaseActive ||
        s_waitingForDelayAck ||
        s_pendingDelayBump ||
        s_sharedDelay >= FRONTEND_DELAY_MAX) {
        return;
    }

    if (s_lastDelayBumpRequestTime != 0 &&
        (now - s_lastDelayBumpRequestTime) < FRONTEND_DELAY_BUMP_INTERVAL_MS) {
        return;
    }

    if ((now - s_lastPressureSampleTime) < FRONTEND_PRESSURE_SAMPLE_INTERVAL_MS) {
        return;
    }

    s_lastPressureSampleTime = now;

    const FrontendDelayProposalDetails details = ComputeDelayProposalDetails();
    const uint16_t delayFloor = GetEffectiveDelayFloor();
    const DWORD remoteSilenceMs = s_lastRemoteInputTime != 0 && now >= s_lastRemoteInputTime
        ? (now - s_lastRemoteInputTime)
        : 0;
    const DWORD waitMs = s_waitingForCurrentFrameSince != 0 && now >= s_waitingForCurrentFrameSince
        ? (now - s_waitingForCurrentFrameSince)
        : 0;
    const bool jitterPressure = details.recommended_delay > delayFloor;
    const bool starvationPressure = s_receivedRemoteInputThisPhase &&
        (remoteSilenceMs >= FRONTEND_STARVATION_PRESSURE_MS ||
         waitMs >= FRONTEND_STARVATION_PRESSURE_MS);

    s_jitterPressureSamples = jitterPressure
        ? (uint8_t)(s_jitterPressureSamples < 0xFF ? s_jitterPressureSamples + 1 : 0xFF)
        : 0;
    s_starvationPressureSamples = starvationPressure
        ? (uint8_t)(s_starvationPressureSamples < 0xFF ? s_starvationPressureSamples + 1 : 0xFF)
        : 0;

    if (jitterPressure || starvationPressure) {
        Rollback::NetplayLog_Verbose(
            "FRONTEND", -1,
            "Delay pressure sample: phase=%s shared=%u floor=%u rec=%u base=%u jitter_bump=%u avg_ping=%.1f variance=%.1f silence_ms=%lu wait_ms=%lu jitter_samples=%u starvation_samples=%u",
            FrontendSyncPhaseName(s_phase),
            s_sharedDelay,
            delayFloor,
            details.recommended_delay,
            details.base_delay,
            details.jitter_bump,
            details.avg_ping_ms,
            details.rtt_variance_ms,
            (unsigned long)remoteSilenceMs,
                (unsigned long)waitMs,
            s_jitterPressureSamples,
            s_starvationPressureSamples);
    }

    if (s_starvationPressureSamples >= FRONTEND_STARVATION_PRESSURE_SAMPLE_THRESHOLD) {
        const uint16_t targetDelay = details.recommended_delay > delayFloor
            ? details.recommended_delay
            : ClampFrontendDelay((uint16_t)(delayFloor + 1));
        RequestDelayIncrease(targetDelay,
            FrontendDelayBumpReason::Starvation,
            "sustained starvation pressure");
        return;
    }

    if (s_jitterPressureSamples >= FRONTEND_JITTER_PRESSURE_SAMPLE_THRESHOLD) {
        RequestDelayIncrease(details.recommended_delay,
            FrontendDelayBumpReason::JitterPressure,
            "sustained jitter pressure");
    }
}

static void HandleRemoteFrameInput(uint32_t epochId,
                                   uint16_t phase,
                                   uint32_t frame,
                                   uint32_t ackFrame,
                                   const uint16_t* inputs,
                                   uint16_t inputCount,
                                   PacketType type) {
    if (!FrontendInputSync_IsCurrentEpochPhase(epochId, phase, type, "frame input")) {
        return;
    }
    if (type != s_packetType) {
        FrontendInputSync_RequestRecovery("frontend frame-input packet type mismatched active phase");
        return;
    }
    if (!s_inputPhaseActive) {
        return;
    }
    if (inputCount == 0 || inputCount > FRONTEND_INPUT_REDUNDANCY) {
        FrontendInputSync_RequestRecovery("frontend packet carried invalid input redundancy");
        return;
    }
    const DWORD now = NowMs();
    if (ackFrame > s_remoteAckFrame) {
        s_remoteAckFrame = ackFrame;
    }
    if (frame < s_consumeFrame) {
        if (ShouldLogRemoteFrameTrace(frame, now)) {
            Rollback::NetplayLog_Verbose(
                "FRONTEND", -1,
                "Ignored stale remote frame input older than consume point: type=%s frame=%u consume=%u ack=%u",
                PacketTypeName(type),
                frame,
                s_consumeFrame,
                ackFrame);
        }
        return;
    }

    const uint32_t previousRemoteLatest = s_remoteLatestFrame;
    bool acceptedAny = false;
    int newFramesApplied = 0;
    int futureFramesIgnored = 0;
    for (uint16_t i = 0; i < inputCount; i++) {
        const int f = (int)frame - (int)i;
        if (f < 0) {
            break;
        }
        if ((uint32_t)f < s_consumeFrame) {
            continue;
        }
        if (((uint32_t)f - s_consumeFrame) >= (uint32_t)FRONTEND_RING_SIZE) {
            futureFramesIgnored++;
            continue;
        }

        if (!HasRemoteInputFrame((uint32_t)f)) {
            StoreRemoteInputFrame((uint32_t)f, inputs[i]);
            acceptedAny = true;
            newFramesApplied++;
        }
    }

    if (frame > s_remoteLatestFrame) {
        s_remoteLatestFrame = frame;
    }
    if (acceptedAny || futureFramesIgnored > 0) {
        s_lastRemoteInputTime = now;
        s_receivedRemoteInputThisPhase = true;
        if (acceptedAny) {
            s_waitingForCurrentFrameSince = 0;
        }
        s_starvationPressureSamples = 0;
    }

    if (ackFrame < s_localInputFrame &&
        (now - s_lastTargetedResendTime) >= FRONTEND_TARGETED_RESEND_INTERVAL_MS) {
        uint32_t resendFrame = ackFrame;
        if ((s_localInputFrame - ackFrame) > (uint32_t)FRONTEND_INPUT_REDUNDANCY) {
            resendFrame = ackFrame + (uint32_t)FRONTEND_INPUT_REDUNDANCY - 1;
            if (resendFrame >= s_localInputFrame) {
                resendFrame = s_localInputFrame - 1;
            }
        }
        if (HasLocalInputFrame(resendFrame)) {
            SendInputPacket(resendFrame, "targeted resend");
            s_lastTargetedResendTime = NowMs();
        } else if (futureFramesIgnored > 0) {
            Rollback::NetplayLog_Write(
                "FRONTEND", -1,
                "Unable to satisfy targeted frontend resend: type=%s requested=%u local=%u consume=%u remote_ack=%u remote_frame=%u ignored_future=%d",
                PacketTypeName(type),
                resendFrame,
                s_localInputFrame,
                s_consumeFrame,
                s_remoteAckFrame,
                frame,
                futureFramesIgnored);
        }
    }

    if (futureFramesIgnored > 0 &&
        (s_lastRingWindowPressureLogTime == 0 ||
         (now - s_lastRingWindowPressureLogTime) >= 250)) {
        s_lastRingWindowPressureLogTime = now;
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Remote frontend input ahead of retain window: type=%s epoch=%u phase=%s frame=%u consume=%u ahead=%u ack=%u count=%u ignored_future=%d accepted_history=%d remoteLatest=%u",
            PacketTypeName(type),
            epochId,
            FrontendSyncPhaseName(s_phase),
            frame,
            s_consumeFrame,
            frame >= s_consumeFrame ? (frame - s_consumeFrame) : 0,
            ackFrame,
            inputCount,
            futureFramesIgnored,
            newFramesApplied,
            s_remoteLatestFrame);
    }

    if (futureFramesIgnored > 0) {
        RequestDelayIncrease(
            ClampFrontendDelay((uint16_t)(GetEffectiveDelayFloor() + 1)),
            FrontendDelayBumpReason::Starvation,
            "remote frontend input beyond retain window");
    }

    const bool sampleFrameTrace = ShouldLogRemoteFrameTrace(frame, now);
    if (!acceptedAny && frame <= previousRemoteLatest && sampleFrameTrace) {
        Rollback::NetplayLog_Verbose(
            "FRONTEND", -1,
            "Ignored duplicate/out-of-order remote frame input: type=%s epoch=%u phase=%s frame=%u ack=%u consume=%u remoteLatest=%u",
            PacketTypeName(type),
            epochId,
            FrontendSyncPhaseName(s_phase),
            frame,
            ackFrame,
            s_consumeFrame,
            previousRemoteLatest);
    } else if (acceptedAny && frame < previousRemoteLatest && sampleFrameTrace) {
        Rollback::NetplayLog_Verbose(
            "FRONTEND", -1,
            "Accepted out-of-order remote frame history fill: type=%s epoch=%u phase=%s frame=%u ack=%u new=%d consume=%u remoteLatest=%u",
            PacketTypeName(type),
            epochId,
            FrontendSyncPhaseName(s_phase),
            frame,
            ackFrame,
            newFramesApplied,
            s_consumeFrame,
            previousRemoteLatest);
    }

    if (sampleFrameTrace && (acceptedAny || futureFramesIgnored > 0)) {
        Rollback::NetplayLog_Verbose(
            "FRONTEND", -1,
            "Remote frame input sample: type=%s epoch=%u phase=%s frame=%u ack=%u count=%u new=%d consume=%u remoteLatest=%u ignored_future=%d",
            PacketTypeName(type),
            epochId,
            FrontendSyncPhaseName(s_phase),
            frame,
            ackFrame,
            inputCount,
            newFramesApplied,
            s_consumeFrame,
            s_remoteLatestFrame,
            futureFramesIgnored);
    }
}

} // anonymous namespace

namespace Net {

void FrontendInputSync_Init() {
    if (s_initialized) {
        return;
    }
    ClearEpochState();
    s_initialized = true;
}

void FrontendInputSync_Shutdown() {
    if (!s_initialized) {
        return;
    }
    ClearEpochState();
    s_initialized = false;
}

int FrontendInputSync_ComputeDelayProposal() {
    const FrontendDelayProposalDetails details = ComputeDelayProposalDetails();

    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Local frontend delay proposal: configured_delay=%u configured_floor=%u base=%u jitter_bump=%u final=%u avg_ping=%.1f variance=%.1f one_way=%.2f jitter_frames=%.2f valid=%d",
        details.configured_delay,
        details.configured_floor,
        details.base_delay,
        details.jitter_bump,
        details.recommended_delay,
        details.avg_ping_ms,
        details.rtt_variance_ms,
        details.one_way_frames,
        details.jitter_frames,
        details.measurement_valid ? 1 : 0);

    return details.recommended_delay;
}

void FrontendInputSync_BeginEpoch(SessionRole role,
                                  uint32_t epochId,
                                  uint16_t localDelayProposal,
                                  const char* reason) {
    ClearEpochState();
    s_epochActive = true;
    s_role = role;
    s_epochId = epochId;
    s_localDelayProposal = ClampFrontendDelay(localDelayProposal);
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Begin epoch: epoch=%u role=%s local_delay_proposal=%u reason=%s",
        s_epochId,
        SessionRoleName(role),
        s_localDelayProposal,
        reason ? reason : "?");
}

void FrontendInputSync_RebindEpoch(uint32_t epochId, const char* reason) {
    if (!s_epochActive || s_epochId == epochId) {
        return;
    }
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Rebind epoch: old=%u new=%u reason=%s",
        s_epochId,
        epochId,
        reason ? reason : "?");
    s_epochId = epochId;
}

void FrontendInputSync_AbortEpoch(const char* reason) {
    if (!s_epochActive) {
        return;
    }
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Abort epoch: epoch=%u phase=%s reason=%s",
        s_epochId,
        FrontendSyncPhaseName(s_phase),
        reason ? reason : "?");
    ClearEpochState();
}

void FrontendInputSync_OnRemoteSyncAnnounce(uint16_t remoteDelayProposal, const char* reason) {
    if (!s_epochActive) {
        return;
    }
    s_remoteDelayProposal = ClampFrontendDelay(remoteDelayProposal);
    s_remoteDelayProposalSeen = true;
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Remote SyncAnnounce delay proposal: epoch=%u remote=%u reason=%s",
        s_epochId,
        s_remoteDelayProposal,
        reason ? reason : "?");
}

void FrontendInputSync_OnRemoteSyncConfirm(uint16_t remoteDelayProposal,
                                           uint16_t remoteSharedDelay,
                                           const char* reason) {
    if (!s_epochActive) {
        return;
    }
    s_remoteDelayProposal = ClampFrontendDelay(remoteDelayProposal);
    s_remoteDelayProposalSeen = true;
    s_sharedDelay = ClampFrontendDelay(remoteSharedDelay);
    s_remoteSharedDelaySeen = true;
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Remote SyncConfirm delay: epoch=%u remote_proposal=%u shared=%u reason=%s",
        s_epochId,
        s_remoteDelayProposal,
        s_sharedDelay,
        reason ? reason : "?");
}

bool FrontendInputSync_FinalizeDelayNegotiation(const char* reason) {
    if (!s_epochActive || !s_remoteDelayProposalSeen) {
        return false;
    }

    const uint16_t computed = ClampFrontendDelay(
        s_localDelayProposal > s_remoteDelayProposal ? s_localDelayProposal : s_remoteDelayProposal);

    if (s_role == SessionRole::Host) {
        s_sharedDelay = computed;
        s_delayNegotiated = true;
    } else {
        if (!s_remoteSharedDelaySeen) {
            return false;
        }
        if (s_sharedDelay < computed) {
            FrontendInputSync_RequestRecovery("remote shared frontend delay was below local requirements");
            return false;
        }
        s_delayNegotiated = true;
    }

    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Frontend delay negotiated: epoch=%u local=%u remote=%u shared=%u reason=%s",
        s_epochId,
        s_localDelayProposal,
        s_remoteDelayProposal,
        s_sharedDelay,
        reason ? reason : "?");
    return true;
}

bool FrontendInputSync_IsDelayNegotiated() {
    return s_delayNegotiated;
}

uint16_t FrontendInputSync_GetSharedDelay() {
    return s_sharedDelay;
}

uint16_t FrontendInputSync_GetLocalDelayProposal() {
    return s_localDelayProposal;
}

uint16_t FrontendInputSync_GetRemoteDelayProposal() {
    return s_remoteDelayProposal;
}

uint32_t FrontendInputSync_GetEpochId() {
    return s_epochId;
}

void FrontendInputSync_BeginInputPhase(FrontendSyncPhase phase,
                                       PacketType packetType,
                                       const char* reason) {
    if (!s_epochActive) {
        return;
    }
    ClearPhaseInputState();
    s_phase = phase;
    s_packetType = packetType;
    s_inputPhaseActive = true;
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Begin input phase: epoch=%u phase=%s packet=%s shared_delay=%u reason=%s",
        s_epochId,
        FrontendSyncPhaseName(phase),
        PacketTypeName(packetType),
        s_sharedDelay,
        reason ? reason : "?");
}

void FrontendInputSync_BeginPassivePhase(FrontendSyncPhase phase, const char* reason) {
    if (!s_epochActive) {
        return;
    }
    ClearPhaseInputState();
    s_phase = phase;
    s_inputPhaseActive = false;
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Begin passive phase: epoch=%u phase=%s reason=%s",
        s_epochId,
        FrontendSyncPhaseName(phase),
        reason ? reason : "?");
}

void FrontendInputSync_EndPhase(const char* reason) {
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "End phase: epoch=%u phase=%s reason=%s",
        s_epochId,
        FrontendSyncPhaseName(s_phase),
        reason ? reason : "?");
    ClearPhaseInputState();
    s_phase = FrontendSyncPhase::None;
    s_inputPhaseActive = false;
}

FrontendSyncPhase FrontendInputSync_GetPhase() {
    return s_phase;
}

bool FrontendInputSync_IsInputPhaseActive() {
    return s_inputPhaseActive;
}

uint32_t FrontendInputSync_GetConsumeFrame() {
    return s_consumeFrame;
}

uint32_t FrontendInputSync_GetLocalInputFrame() {
    return s_localInputFrame;
}

uint32_t FrontendInputSync_GetRemoteLatestFrame() {
    return s_remoteLatestFrame;
}

uint32_t FrontendInputSync_GetRemoteAckFrame() {
    return s_remoteAckFrame;
}

void FrontendInputSync_FrameUpdate() {
    if (!s_epochActive) {
        return;
    }
    MaybeApplyPendingDelay();
    if (!s_inputPhaseActive) {
        return;
    }

    const DWORD now = NowMs();
    if (s_localInputFrame > 0 && (now - s_lastResendTime) >= FRONTEND_RESEND_INTERVAL_MS) {
        SendInputPacket(s_localInputFrame - 1, "periodic resend");
        s_lastResendTime = now;
    }
}

void FrontendInputSync_CaptureLocalInput(uint16_t packedInput) {
    if (!s_epochActive || !s_inputPhaseActive) {
        return;
    }

    EnsureLocalLead("delay lead fill");

    const uint32_t maxSendFrame = GetLocalSendHeadLimit();
    if (s_localInputFrame > maxSendFrame) {
        if (s_localInputFrame > 0) {
            SendInputPacket(s_localInputFrame - 1, "send ahead cap");
        }
        return;
    }

    StoreLocalInputFrame(s_localInputFrame, packedInput);
    SendInputPacket(s_localInputFrame, "capture");
    s_localInputFrame++;
}

bool FrontendInputSync_HasInputsForCurrentFrame() {
    if (!s_epochActive || !s_inputPhaseActive) {
        return false;
    }

    const bool ready =
        HasLocalInputFrame(s_consumeFrame) &&
        HasRemoteInputFrame(s_consumeFrame);
    if (ready) {
        ResetDelayPressureTracking();
        return true;
    }

    const DWORD now = NowMs();
    if (s_waitingForCurrentFrameSince == 0) {
        s_waitingForCurrentFrameSince = now;
    }

    MaybeRequestLiveDelayIncrease(now);

    if (!s_timedOut &&
        s_lastRemoteInputTime != 0 &&
        (now - s_lastRemoteInputTime) >= FRONTEND_TIMEOUT_MS) {
        s_timedOut = true;
        FrontendInputSync_RequestRecovery("frontend input timed out waiting for remote frame");
    }

    if (s_localInputFrame > 0 && (now - s_lastResendTime) >= FRONTEND_RESEND_INTERVAL_MS) {
        SendInputPacket(s_localInputFrame - 1, "wait resend");
        s_lastResendTime = now;
    }

    return false;
}

bool FrontendInputSync_ConsumeCurrentFrame(uint16_t* outLocal,
                                           uint16_t* outRemote,
                                           FrontendFrameId* outFrameId) {
    if (!s_epochActive || !s_inputPhaseActive || !outLocal || !outRemote) {
        return false;
    }

    if (!FrontendInputSync_HasInputsForCurrentFrame()) {
        return false;
    }

    MaybeApplyPendingDelay();

    const int idx = (int)(s_consumeFrame & FRONTEND_RING_MASK);
    *outLocal = s_localInputs[idx];
    *outRemote = s_remoteInputs[idx];
    if (outFrameId) {
        outFrameId->epoch_id = s_epochId;
        outFrameId->phase = (uint16_t)s_phase;
        outFrameId->frame = (uint16_t)s_consumeFrame;
    }

    s_hasRemoteInput[idx] = false;
    s_consumeFrame++;
    return true;
}

void FrontendInputSync_OnRemoteCharSelFrameInput(const CharSelFrameInputPayload* p) {
    if (!p) {
        return;
    }
    HandleRemoteFrameInput(p->epoch_id,
                           p->phase,
                           p->frame,
                           p->ack_frame,
                           p->inputs,
                           p->input_count,
                           PacketType::CharSelFrameInput);
}

void FrontendInputSync_OnRemoteWinScreenFrameInput(const WinScreenFrameInputPayload* p) {
    if (!p) {
        return;
    }
    HandleRemoteFrameInput(p->epoch_id,
                           p->phase,
                           p->frame,
                           p->ack_frame,
                           p->inputs,
                           p->input_count,
                           PacketType::WinScreenFrameInput);
}

bool FrontendInputSync_IsCurrentEpochPhase(uint32_t epochId,
                                           uint16_t phase,
                                           PacketType type,
                                           const char* context) {
    if (!s_epochActive) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Ignored %s packet with no active epoch: type=%s epoch=%u phase=%u",
            context ? context : "frontend",
            PacketTypeName(type),
            epochId,
            phase);
        return false;
    }
    if (epochId != s_epochId) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Ignored stale epoch packet: type=%s packet_epoch=%u local_epoch=%u context=%s",
            PacketTypeName(type),
            epochId,
            s_epochId,
            context ? context : "?");
        return false;
    }
    if (phase != (uint16_t)s_phase) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Ignored stale phase packet: type=%s packet_phase=%u local_phase=%u context=%s",
            PacketTypeName(type),
            phase,
            (uint16_t)s_phase,
            context ? context : "?");
        return false;
    }
    return true;
}

void FrontendInputSync_SendPhaseBarrier(FrontendSyncPhase nextPhase,
                                        uint8_t reasonCode,
                                        const char* reason) {
    if (!s_epochActive) {
        return;
    }
    FrontendPhaseBarrierPayload payload{};
    payload.epoch_id = s_epochId;
    payload.phase = (uint16_t)s_phase;
    payload.next_phase = (uint16_t)nextPhase;
    payload.last_completed_frame = s_consumeFrame > 0 ? (s_consumeFrame - 1) : 0;
    payload.reason_code = reasonCode;
    if (BarrierProtocol_SendPacket(PacketType::FrontendPhaseBarrier, &payload, sizeof(payload))) {
        s_localPhaseBarrierSent = true;
        s_barrierNextPhase = nextPhase;
        s_localBarrierFrame = payload.last_completed_frame;
    }
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Sent phase barrier: epoch=%u phase=%s next=%s last_frame=%u reason_code=%u reason=%s",
        s_epochId,
        FrontendSyncPhaseName(s_phase),
        FrontendSyncPhaseName(nextPhase),
        payload.last_completed_frame,
        reasonCode,
        reason ? reason : "?");
}

void FrontendInputSync_OnRemotePhaseBarrier(const FrontendPhaseBarrierPayload* p) {
    if (!p) {
        return;
    }
    if (!FrontendInputSync_IsCurrentEpochPhase(p->epoch_id,
                                               p->phase,
                                               PacketType::FrontendPhaseBarrier,
                                               "phase barrier")) {
        return;
    }
    s_remotePhaseBarrierSeen = true;
    s_remoteBarrierFrame = p->last_completed_frame;
    if (s_localPhaseBarrierSent && s_barrierNextPhase != (FrontendSyncPhase)p->next_phase) {
        FrontendInputSync_RequestRecovery("frontend phase barrier next-phase mismatch");
    }
    s_barrierNextPhase = (FrontendSyncPhase)p->next_phase;
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Remote phase barrier: epoch=%u phase=%s next=%s last_frame=%u reason_code=%u",
        s_epochId,
        FrontendSyncPhaseName(s_phase),
        FrontendSyncPhaseName((FrontendSyncPhase)p->next_phase),
        p->last_completed_frame,
        p->reason_code);
}

bool FrontendInputSync_IsPhaseBarrierSatisfied(FrontendSyncPhase nextPhase) {
    return s_localPhaseBarrierSent &&
           s_remotePhaseBarrierSeen &&
           s_barrierNextPhase == nextPhase;
}

void FrontendInputSync_ClearPhaseBarrier() {
    s_localPhaseBarrierSent = false;
    s_remotePhaseBarrierSeen = false;
    s_barrierNextPhase = FrontendSyncPhase::None;
    s_localBarrierFrame = 0;
    s_remoteBarrierFrame = 0;
}

void FrontendInputSync_SendBoundaryDigest(const FrontendBoundaryDigestPayload* payload,
                                          const char* reason) {
    if (!payload || !s_epochActive) {
        return;
    }
    if (!BarrierProtocol_SendPacket(PacketType::FrontendBoundaryDigest, payload, sizeof(*payload))) {
        return;
    }
    const FrontendDigestKind localKind = (FrontendDigestKind)payload->digest_kind;
    if (s_remoteDigestSeen && s_digestKind != FrontendDigestKind::None && s_digestKind != localKind) {
        FrontendInputSync_RequestRecovery("frontend digest kind mismatch");
        return;
    }
    s_localDigestSent = true;
    s_digestKind = localKind;
    s_localDigest = payload->digest;
    s_digestMatch = s_remoteDigestSeen && (s_localDigest == s_remoteDigest);
    s_digestMismatch = s_remoteDigestSeen && !s_digestMatch;
    if (s_digestMismatch) {
        FrontendInputSync_RequestRecovery("frontend boundary digest mismatch");
    }
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Sent boundary digest: epoch=%u phase=%s kind=%s frame=%u digest=0x%08X reason=%s",
        payload->epoch_id,
        FrontendSyncPhaseName((FrontendSyncPhase)payload->phase),
        FrontendDigestKindName((FrontendDigestKind)payload->digest_kind),
        payload->frame,
        payload->digest,
        reason ? reason : "?");
}

void FrontendInputSync_OnRemoteBoundaryDigest(const FrontendBoundaryDigestPayload* p) {
    if (!p) {
        return;
    }
    if (!FrontendInputSync_IsCurrentEpochPhase(p->epoch_id,
                                               p->phase,
                                               PacketType::FrontendBoundaryDigest,
                                               "boundary digest")) {
        return;
    }
    s_remoteDigestSeen = true;
    s_remoteDigest = p->digest;
    if (s_localDigestSent && s_digestKind != (FrontendDigestKind)p->digest_kind) {
        FrontendInputSync_RequestRecovery("frontend digest kind mismatch");
        return;
    }
    s_digestKind = (FrontendDigestKind)p->digest_kind;
    s_digestMatch = s_localDigestSent && (s_localDigest == s_remoteDigest);
    s_digestMismatch = s_localDigestSent && !s_digestMatch;
    if (s_digestMismatch) {
        FrontendInputSync_RequestRecovery("frontend boundary digest mismatch");
    }
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Remote boundary digest: epoch=%u phase=%s kind=%s frame=%u digest=0x%08X match=%d",
        p->epoch_id,
        FrontendSyncPhaseName((FrontendSyncPhase)p->phase),
        FrontendDigestKindName((FrontendDigestKind)p->digest_kind),
        p->frame,
        p->digest,
        s_digestMatch ? 1 : 0);
}

bool FrontendInputSync_IsDigestMatched(FrontendDigestKind kind) {
    return s_localDigestSent && s_remoteDigestSeen && s_digestMatch && s_digestKind == kind;
}

bool FrontendInputSync_HasDigestMismatch() {
    return s_digestMismatch;
}

void FrontendInputSync_OnRemoteDelayChangeReq(const DelayChangeReqPayload* p) {
    if (!p) {
        return;
    }
    if (!FrontendInputSync_IsCurrentEpochPhase(p->epoch_id,
                                               p->phase,
                                               PacketType::DelayChangeReq,
                                               "delay change req")) {
        return;
    }

    const uint16_t requested = ClampFrontendDelay(p->new_delay);
    const FrontendDelayBumpReason requestedReason = SanitizeDelayReason(p->reason_code);
    const uint32_t requestedApplyFrom = p->apply_from_frame > (s_consumeFrame + 1)
        ? p->apply_from_frame
        : (s_consumeFrame + 1);
    const uint16_t delayFloor = GetEffectiveDelayFloor();
    if (requested < delayFloor) {
        SendDelayChangeAck(delayFloor,
                           s_consumeFrame + 1,
                           false,
                           requestedReason);
        return;
    }

    if (s_pendingDelayBump &&
        s_pendingDelay == requested &&
        s_pendingApplyFrom == requestedApplyFrom) {
        SendDelayChangeAck(requested,
                           requestedApplyFrom,
                           true,
                           requestedReason);
        Rollback::NetplayLog_Verbose(
            "FRONTEND", -1,
            "Ignored duplicate remote delay bump request: phase=%s requested=%u apply_from=%u reason=%s",
            FrontendSyncPhaseName(s_phase),
            requested,
            requestedApplyFrom,
            FrontendDelayBumpReasonName(requestedReason));
        return;
    }

    s_pendingDelayBump = true;
    s_pendingDelay = requested;
    s_pendingApplyFrom = requestedApplyFrom;
    s_pendingDelayReason = requestedReason;
    SendDelayChangeAck(requested,
                       s_pendingApplyFrom,
                       true,
                       requestedReason);

    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Accepted remote delay bump: phase=%s current=%u requested=%u apply_from=%u floor=%u reason=%s",
        FrontendSyncPhaseName(s_phase),
        s_sharedDelay,
        requested,
        s_pendingApplyFrom,
        delayFloor,
        FrontendDelayBumpReasonName(requestedReason));
}

void FrontendInputSync_OnRemoteDelayChangeAck(const DelayChangeAckPayload* p) {
    if (!p) {
        return;
    }
    if (!FrontendInputSync_IsCurrentEpochPhase(p->epoch_id,
                                               p->phase,
                                               PacketType::DelayChangeAck,
                                               "delay change ack")) {
        return;
    }
    if (!s_waitingForDelayAck) {
        if (s_pendingDelayBump &&
            p->accepted &&
            p->acked_delay == s_pendingDelay &&
            p->apply_from_frame == s_pendingApplyFrom) {
            Rollback::NetplayLog_Verbose(
                "FRONTEND", -1,
                "Ignored duplicate frontend delay bump ack: acked=%u apply_from=%u reason=%s",
                p->acked_delay,
                p->apply_from_frame,
                FrontendDelayBumpReasonName(SanitizeDelayReason(p->reason_code)));
        }
        return;
    }

    s_waitingForDelayAck = false;
    if (!p->accepted) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Remote rejected frontend delay bump: requested=%u current=%u",
            s_requestedDelay,
            s_sharedDelay);
        return;
    }
    if (p->acked_delay < s_requestedDelay) {
        FrontendInputSync_RequestRecovery("frontend delay bump acked a smaller delay than requested");
        return;
    }

    const uint16_t ackedDelay = s_pendingDelayBump && s_pendingDelay > p->acked_delay
        ? s_pendingDelay
        : p->acked_delay;
    const uint32_t applyFrom = s_pendingDelayBump && s_pendingApplyFrom > p->apply_from_frame
        ? s_pendingApplyFrom
        : p->apply_from_frame;

    s_pendingDelayBump = true;
    s_pendingDelay = ackedDelay;
    s_pendingApplyFrom = applyFrom > s_requestedApplyFrom
        ? applyFrom
        : s_requestedApplyFrom;
    s_pendingDelayReason = s_requestedDelayReason;
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Remote acked frontend delay bump: requested=%u acked=%u effective=%u apply_from=%u reason=%s",
        s_requestedDelay,
        p->acked_delay,
        s_pendingDelay,
        s_pendingApplyFrom,
        FrontendDelayBumpReasonName(s_pendingDelayReason));
}

void FrontendInputSync_ReportLocalAdvanceIntent(uint16_t packedInput) {
    if (packedInput != 0) {
        s_localAdvanceObserved = true;
    }
}

void FrontendInputSync_ReportRemoteAdvanceIntent(uint16_t packedInput) {
    if (packedInput != 0) {
        s_remoteAdvanceObserved = true;
    }
}

bool FrontendInputSync_LocalAdvanceObserved() {
    return s_localAdvanceObserved;
}

bool FrontendInputSync_RemoteAdvanceObserved() {
    return s_remoteAdvanceObserved;
}

bool FrontendInputSync_BothAdvanceObserved() {
    return s_localAdvanceObserved && s_remoteAdvanceObserved;
}

void FrontendInputSync_RequestRecovery(const char* reason) {
    if (s_recoveryReason[0] != '\0') {
        return;
    }
    _snprintf_s(s_recoveryReason,
                sizeof(s_recoveryReason),
                _TRUNCATE,
                "%s",
                reason ? reason : "frontend recovery requested");
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Recovery requested: epoch=%u phase=%s reason=%s",
        s_epochId,
        FrontendSyncPhaseName(s_phase),
        s_recoveryReason);
    LOG_NETPLAY(LOG_WARNING, "[FrontendSync] Recovery requested: %s", s_recoveryReason);
}

bool FrontendInputSync_HasRecoveryRequest() {
    return s_recoveryReason[0] != '\0';
}

const char* FrontendInputSync_GetRecoveryReason() {
    return s_recoveryReason;
}

void FrontendInputSync_ClearRecoveryRequest() {
    s_recoveryReason[0] = '\0';
}

#if defined(AS2_FRONTEND_SYNC_TESTING)
void FrontendInputSync_Test_SetClockMs(uint32_t nowMs) {
    s_testClockOverrideActive = true;
    s_testClockMs = nowMs;
}

void FrontendInputSync_Test_ClearClockOverride() {
    s_testClockOverrideActive = false;
    s_testClockMs = 0;
}
#endif

void FrontendInputSync_GetSnapshot(FrontendInputSyncSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->epoch_active = s_epochActive;
    out->input_phase_active = s_inputPhaseActive;
    out->role = s_role;
    out->phase = s_phase;
    out->consume_id.epoch_id = s_epochId;
    out->consume_id.phase = (uint16_t)s_phase;
    out->consume_id.frame = (uint16_t)s_consumeFrame;
    out->local_input_frame = s_localInputFrame;
    out->remote_latest_frame = s_remoteLatestFrame;
    out->local_delay_proposal = s_localDelayProposal;
    out->remote_delay_proposal = s_remoteDelayProposal;
    out->shared_delay = s_sharedDelay;
    out->delay_negotiated = s_delayNegotiated;
    out->local_phase_barrier_sent = s_localPhaseBarrierSent;
    out->remote_phase_barrier_seen = s_remotePhaseBarrierSeen;
    out->phase_barrier_satisfied = s_localPhaseBarrierSent && s_remotePhaseBarrierSeen;
    out->barrier_next_phase = s_barrierNextPhase;
    out->local_digest_sent = s_localDigestSent;
    out->remote_digest_seen = s_remoteDigestSeen;
    out->digest_match = s_digestMatch;
    out->digest_mismatch = s_digestMismatch;
    out->digest_kind = s_digestKind;
    out->local_digest = s_localDigest;
    out->remote_digest = s_remoteDigest;
    out->pending_delay_bump = s_pendingDelayBump || s_waitingForDelayAck;
    out->pending_delay = s_pendingDelayBump ? s_pendingDelay : s_requestedDelay;
    out->pending_apply_from = s_pendingDelayBump ? s_pendingApplyFrom : s_requestedApplyFrom;
    out->local_advance_observed = s_localAdvanceObserved;
    out->remote_advance_observed = s_remoteAdvanceObserved;
    out->timed_out = s_timedOut;
    _snprintf_s(out->recovery_reason,
                sizeof(out->recovery_reason),
                _TRUNCATE,
                "%s",
                s_recoveryReason);
}

} // namespace Net
