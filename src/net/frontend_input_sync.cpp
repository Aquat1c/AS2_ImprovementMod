#include "net/frontend_input_sync.h"

#include "net/barrier_protocol.h"
#include "net/delay_policy.h"
#include "net/winscreen_sync.h"
#include "core/game_state.h"
#if !defined(AS2_FRONTEND_SYNC_TESTING)
#include "net/sync_trace.h"
#endif
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
constexpr DWORD FRONTEND_RESEND_INTERVAL_MS = 50;
constexpr DWORD FRONTEND_TARGETED_RESEND_INTERVAL_MS = 50;
constexpr DWORD FRONTEND_DELAY_BUMP_INTERVAL_MS = 1500;
constexpr DWORD FRONTEND_PRESSURE_SAMPLE_INTERVAL_MS = 250;
constexpr DWORD FRONTEND_STARVATION_PRESSURE_MS = 900;
constexpr uint8_t FRONTEND_JITTER_PRESSURE_SAMPLE_THRESHOLD = 3;
constexpr uint8_t FRONTEND_STARVATION_PRESSURE_SAMPLE_THRESHOLD = 2;
constexpr uint32_t FRONTEND_LEAD_EXTRA_DEFAULT = 2;

// INV-11 interrogation thresholds (§4.6): 120 lockstep ticks (~2 s) of zero
// accepted remote frames, or 60 coherent-but-unexpected phase_id packets,
// whichever first. Three failed cycles escalate to a pregame restart.
constexpr uint32_t FRONTEND_STARVATION_INTERROGATE_FRAMES = 120;
constexpr uint32_t FRONTEND_PHASE_ID_MISMATCH_THRESHOLD = 60;
constexpr uint8_t  FRONTEND_RESYNC_MAX_CYCLES = 3;

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
// Injected winscreen send gate (M0 dependency inversion — replaces the upward
// PregameSync_GetPhase() include). Null = sends allowed.
static FrontendWinScreenSendGate s_winScreenSendGate = nullptr;
// Injected transport-health gate (INV-11: interrogate only while the
// supervisor reports the transport alive). Null = healthy.
static FrontendTransportHealthyGate s_transportHealthyGate = nullptr;

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
static uint16_t          s_lastPhysicalInput = 0;
static uint16_t          s_latchedPressedInput = 0;

// Frontend delay: locally derived per epoch (peer-local, INV-23). Live bumps
// are increase-only local decisions applied at a scheduled consume frame so
// the input pipeline never rewinds; no wire negotiation exists.
static uint16_t          s_frontendDelay = FRONTEND_DELAY_MIN;

static DWORD             s_lastRemoteInputTime = 0;
static DWORD             s_lastResendTime = 0;
static DWORD             s_lastTargetedResendTime = 0;
static DWORD             s_lastRingWindowPressureLogTime = 0;
static DWORD             s_lastRemoteFrameTraceLogTime = 0;
static bool              s_remoteFrameTraceLogTimeValid = false;
static DWORD             s_lastDelayBumpTime = 0;
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

// INV-11 interrogation state (per input phase).
static uint32_t          s_starvedFrames = 0;
static uint32_t          s_phaseIdMismatchCount = 0;
static uint8_t           s_resyncCycles = 0;
static bool              s_resyncAwaitingReply = false;
static FrontendResyncEscalation s_resyncEscalation = FrontendResyncEscalation::None;

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
    details.configured_floor = ClampFrontendDelay(details.configured_delay);
    if (measurement.valid) {
        int networkDelay = (int)ceilf(measurement.one_way_frames + measurement.jitter_frames);
        if (networkDelay < FRONTEND_DELAY_MIN) {
            networkDelay = FRONTEND_DELAY_MIN;
        }
        details.base_delay = ClampFrontendDelay((uint16_t)networkDelay);
        int jitterFrames = (int)ceilf(measurement.jitter_frames);
        if (jitterFrames < 0) {
            jitterFrames = 0;
        }
        details.jitter_bump = (uint16_t)jitterFrames;
    } else {
        details.base_delay = FRONTEND_DELAY_MIN;
        details.jitter_bump = 0;
    }
    if (details.base_delay < details.configured_floor) {
        details.base_delay = details.configured_floor;
    }
    details.recommended_delay = ClampFrontendDelay(details.base_delay);
    return details;
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
    uint16_t floor = s_frontendDelay;
    if (s_pendingDelayBump && s_pendingDelay > floor) {
        floor = s_pendingDelay;
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

static uint32_t GetRemoteContiguousFrameExclusive() {
    uint32_t frame = s_consumeFrame;
    const uint32_t stop = s_consumeFrame + (uint32_t)FRONTEND_RING_SIZE;
    while (frame < stop && HasRemoteInputFrame(frame)) {
        frame++;
    }
    return frame;
}

static uint32_t GetLocalLeadFrames() {
    return s_localInputFrame > s_consumeFrame
        ? (s_localInputFrame - s_consumeFrame)
        : 0;
}

static uint32_t GetFrontendMaxLocalLead() {
    return (uint32_t)GetEffectiveDelayFloor() + FRONTEND_LEAD_EXTRA_DEFAULT;
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

static uint32_t GetLocalCaptureHeadLimitExclusive() {
    uint32_t limit = s_consumeFrame + GetFrontendMaxLocalLead();
    const uint32_t ackLimited = s_remoteAckFrame + (uint32_t)FRONTEND_RING_SIZE - 1u;
    if (ackLimited < limit) {
        limit = ackLimited;
    }
    return limit;
}

static void UpdateFrontendInputLatch(uint16_t current) {
    const uint16_t pressed = (uint16_t)(current & ~s_lastPhysicalInput);
    s_latchedPressedInput = (uint16_t)(s_latchedPressedInput | pressed);
    s_lastPhysicalInput = current;
}

static uint16_t ConsumeLatchedFrontendInput(uint16_t current) {
    const uint16_t out = (uint16_t)(current | s_latchedPressedInput);
    s_latchedPressedInput = 0;
    return out;
}

static uint8_t CurrentPhaseIdByte() {
    return (uint8_t)FrontendSyncPhaseToPhaseId(s_phase);
}

// Native MODE_* byte for the resync identity tuple. The standalone test
// build has no game memory to read (GetGameMode dereferences 0x81638C).
static uint8_t CurrentNativeModeByte() {
#if defined(AS2_FRONTEND_SYNC_TESTING)
    return 0;
#else
    return (uint8_t)GetGameMode();
#endif
}

static void LogFrontendQueueState(const char* reason, DWORD now, bool force) {
    if (!force &&
        s_lastRingWindowPressureLogTime != 0 &&
        (now - s_lastRingWindowPressureLogTime) < FRONTEND_PRESSURE_SAMPLE_INTERVAL_MS) {
        return;
    }

    s_lastRingWindowPressureLogTime = now;
    const uint32_t remoteContig = GetRemoteContiguousFrameExclusive();
    const uint32_t remoteHoleSpan = s_receivedRemoteInputThisPhase && s_remoteLatestFrame >= remoteContig
        ? (s_remoteLatestFrame - remoteContig + 1u)
        : 0u;
    const DWORD waitMs = s_waitingForCurrentFrameSince != 0 && now >= s_waitingForCurrentFrameSince
        ? (now - s_waitingForCurrentFrameSince)
        : 0;
    Rollback::NetplayLog_Write(
        "FRONTQ", -1,
        "Queue state: reason=%s epoch=%u phase=%s phase_id=%u consume=%u local_head=%u local_lead=%u max_lead=%u capture_limit=%u remote_contig=%u remote_latest=%u remote_ack=%u missing_current=%u hole_span=%u wait_ms=%lu delay=%u pending_delay=%u starved=%u",
        reason ? reason : "?",
        s_epochId,
        FrontendSyncPhaseName(s_phase),
        CurrentPhaseIdByte(),
        s_consumeFrame,
        s_localInputFrame,
        GetLocalLeadFrames(),
        GetFrontendMaxLocalLead(),
        GetLocalCaptureHeadLimitExclusive(),
        remoteContig,
        s_remoteLatestFrame,
        s_remoteAckFrame,
        HasRemoteInputFrame(s_consumeFrame) ? 0u : 1u,
        remoteHoleSpan,
        (unsigned long)waitMs,
        s_frontendDelay,
        GetEffectiveDelayFloor(),
        s_starvedFrames);
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
    s_lastDelayBumpTime = 0;
    s_timedOut = false;
    memset(s_localInputs, 0, sizeof(s_localInputs));
    memset(s_remoteInputs, 0, sizeof(s_remoteInputs));
    memset(s_localInputIds, 0, sizeof(s_localInputIds));
    memset(s_remoteInputIds, 0, sizeof(s_remoteInputIds));
    memset(s_hasLocalInput, 0, sizeof(s_hasLocalInput));
    memset(s_hasRemoteInput, 0, sizeof(s_hasRemoteInput));
    s_lastPhysicalInput = 0;
    s_latchedPressedInput = 0;
    s_pendingDelayBump = false;
    s_pendingDelay = 0;
    s_pendingApplyFrom = 0;
    s_pendingDelayReason = FrontendDelayBumpReason::None;
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
    s_starvedFrames = 0;
    s_phaseIdMismatchCount = 0;
    s_resyncCycles = 0;
    s_resyncAwaitingReply = false;
    s_resyncEscalation = FrontendResyncEscalation::None;
    ResetDelayPressureTracking();
}

static void ClearEpochState() {
    s_epochActive = false;
    s_role = SessionRole::Host;
    s_epochId = 0;
    s_phase = FrontendSyncPhase::None;
    s_packetType = PacketType::CharSelFrameInput;
    s_inputPhaseActive = false;
    s_frontendDelay = FRONTEND_DELAY_MIN;
    s_recoveryReason[0] = '\0';
    ClearPhaseInputState();
}

static bool SendInputPacket(uint32_t frame, const char* reason) {
    if (!s_epochActive || !s_inputPhaseActive) {
        return false;
    }

    if (s_packetType == PacketType::WinScreenFrameInput) {
        if (s_phase != FrontendSyncPhase::WinScreen || !WinScreenSync_IsActive()) {
            return false;
        }
        // Injected predicate (M0 dependency inversion): match_setup registers
        // its phase-based gate; a null gate allows sends (test harness default).
        if (s_winScreenSendGate && !s_winScreenSendGate()) {
            return false;
        }
    }

    if (s_packetType == PacketType::CharSelFrameInput) {
        CharSelFrameInputPayload payload{};
        payload.epoch_id = s_epochId;
        payload.phase = (uint16_t)s_phase;
        payload.phase_id = CurrentPhaseIdByte();
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
                "Send CharSelFrameInput failed: phase_id=%u frame=%u ack=%u count=%u reason=%s",
                payload.phase_id,
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
    payload.phase_id = CurrentPhaseIdByte();
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
            "Send WinScreenFrameInput failed: phase_id=%u frame=%u ack=%u count=%u reason=%s",
            payload.phase_id,
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

    const uint32_t targetLead = (uint32_t)(s_frontendDelay > 0 ? s_frontendDelay : FRONTEND_DELAY_MIN);
    const uint32_t captureLimit = GetLocalCaptureHeadLimitExclusive();
    while (GetLocalLeadFrames() < targetLead && s_localInputFrame < captureLimit) {
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

    const uint16_t oldDelay = s_frontendDelay;
    const FrontendDelayBumpReason appliedReason = s_pendingDelayReason;
    s_frontendDelay = ClampFrontendDelay(s_pendingDelay);
    s_pendingDelayBump = false;
    s_pendingDelay = 0;
    s_pendingApplyFrom = 0;
    s_pendingDelayReason = FrontendDelayBumpReason::None;
    ResetDelayPressureTracking();

    Rollback::NetplayLog_Write(
        "FRONTDELAY", -1,
        "Applied local frontend delay bump: old=%u new=%u phase=%s frame=%u reason=%s",
        oldDelay,
        s_frontendDelay,
        FrontendSyncPhaseName(s_phase),
        s_consumeFrame,
        FrontendDelayBumpReasonName(appliedReason));
}

// Increase-only local frontend delay bump (INV-23: peer-local knob, no wire
// message — the peer simply sees our capture lead grow). Applied at a
// scheduled consume frame so the pipeline never rewinds.
static void ScheduleLocalDelayIncrease(uint16_t requestedDelay,
                                       FrontendDelayBumpReason reason,
                                       const char* context) {
    if (!s_epochActive || !s_inputPhaseActive) {
        return;
    }

    requestedDelay = ClampFrontendDelay(requestedDelay);
    if (requestedDelay <= GetEffectiveDelayFloor()) {
        return;
    }

    s_pendingDelayBump = true;
    s_pendingDelay = requestedDelay;
    s_pendingApplyFrom = s_consumeFrame + requestedDelay + 2;
    s_pendingDelayReason = reason;
    s_lastDelayBumpTime = NowMs();

    Rollback::NetplayLog_Write(
        "FRONTDELAY", -1,
        "Scheduled local frontend delay bump: phase=%s current=%u requested=%u apply_from=%u reason=%s context=%s",
        FrontendSyncPhaseName(s_phase),
        s_frontendDelay,
        requestedDelay,
        s_pendingApplyFrom,
        FrontendDelayBumpReasonName(reason),
        context ? context : "?");
}

static void MaybeRequestLiveDelayIncrease(DWORD now) {
    if (!s_epochActive ||
        !s_inputPhaseActive ||
        s_pendingDelayBump ||
        s_frontendDelay >= FRONTEND_DELAY_MAX) {
        return;
    }

    if (s_lastDelayBumpTime != 0 &&
        (now - s_lastDelayBumpTime) < FRONTEND_DELAY_BUMP_INTERVAL_MS) {
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
    const uint32_t remoteContig = GetRemoteContiguousFrameExclusive();
    const uint32_t remoteHoleSpan = s_receivedRemoteInputThisPhase && s_remoteLatestFrame >= remoteContig
        ? (s_remoteLatestFrame - remoteContig + 1u)
        : 0u;
    const bool missingCurrentRemote = !HasRemoteInputFrame(s_consumeFrame);
    const bool jitterPressure = details.recommended_delay > delayFloor;
    const bool queuePressure = s_receivedRemoteInputThisPhase &&
        ((missingCurrentRemote && waitMs >= 150) ||
         remoteHoleSpan > 0);
    const bool starvationPressure = s_receivedRemoteInputThisPhase &&
        (queuePressure ||
         remoteSilenceMs >= FRONTEND_STARVATION_PRESSURE_MS ||
         waitMs >= FRONTEND_STARVATION_PRESSURE_MS);

    s_jitterPressureSamples = jitterPressure
        ? (uint8_t)(s_jitterPressureSamples < 0xFF ? s_jitterPressureSamples + 1 : 0xFF)
        : 0;
    s_starvationPressureSamples = starvationPressure
        ? (uint8_t)(s_starvationPressureSamples < 0xFF ? s_starvationPressureSamples + 1 : 0xFF)
        : 0;

    if (jitterPressure || starvationPressure) {
        Rollback::NetplayLog_Verbose(
            "FRONTDELAY", -1,
            "Delay pressure sample: phase=%s delay=%u floor=%u rec=%u base=%u jitter_frames=%u avg_ping=%.1f variance=%.1f silence_ms=%lu wait_ms=%lu remote_contig=%u remote_latest=%u hole_span=%u missing_current=%u jitter_samples=%u starvation_samples=%u",
            FrontendSyncPhaseName(s_phase),
            s_frontendDelay,
            delayFloor,
            details.recommended_delay,
            details.base_delay,
            details.jitter_bump,
            details.avg_ping_ms,
            details.rtt_variance_ms,
            (unsigned long)remoteSilenceMs,
            (unsigned long)waitMs,
            remoteContig,
            s_remoteLatestFrame,
            remoteHoleSpan,
            missingCurrentRemote ? 1u : 0u,
            s_jitterPressureSamples,
            s_starvationPressureSamples);
    }

    if (queuePressure) {
        LogFrontendQueueState("delay pressure", now, false);
    }

    if (s_starvationPressureSamples >= FRONTEND_STARVATION_PRESSURE_SAMPLE_THRESHOLD) {
        const uint16_t targetDelay = details.recommended_delay > delayFloor
            ? details.recommended_delay
            : ClampFrontendDelay((uint16_t)(delayFloor + 1));
        ScheduleLocalDelayIncrease(targetDelay,
            FrontendDelayBumpReason::Starvation,
            "sustained starvation pressure");
        return;
    }

    if (s_jitterPressureSamples >= FRONTEND_JITTER_PRESSURE_SAMPLE_THRESHOLD) {
        ScheduleLocalDelayIncrease(details.recommended_delay,
            FrontendDelayBumpReason::JitterPressure,
            "sustained jitter pressure");
    }
}

// ============================================================================
// INV-11 interrogation (§4.6 ladder step 2)
// ============================================================================

static bool TransportHealthyForInterrogation() {
    return s_transportHealthyGate == nullptr || s_transportHealthyGate();
}

static void SendResyncRequest(const char* reason) {
    ResyncRequestPayload req{};
    req.epoch = s_epochId;
    req.phase_id = CurrentPhaseIdByte();
    req.native_mode = CurrentNativeModeByte();
    req.local_frame = s_consumeFrame;
    if (!BarrierProtocol_SendPacket(PacketType::ResyncRequest, &req, sizeof(req))) {
        return;
    }
    s_resyncAwaitingReply = true;
    s_resyncCycles = (uint8_t)(s_resyncCycles < 0xFF ? s_resyncCycles + 1 : 0xFF);
    Rollback::NetplayLog_Write(
        "FRONTRESYNC", -1,
        "Sent ResyncRequest: epoch=%u phase_id=%u mode=%u frame=%u cycle=%u reason=%s",
        req.epoch, req.phase_id, req.native_mode, req.local_frame,
        s_resyncCycles, reason ? reason : "?");
    if (s_resyncCycles >= FRONTEND_RESYNC_MAX_CYCLES &&
        s_resyncEscalation == FrontendResyncEscalation::None) {
        // Three failed cycles (~6 s) → pregame restart under a fresh epoch
        // (§4.6 step 3). NEVER a teardown (INV-12).
        s_resyncEscalation = FrontendResyncEscalation::Restart;
        Rollback::NetplayLog_Write(
            "FRONTRESYNC", -1,
            "Interrogation exhausted (%u cycles) — requesting pregame restart",
            s_resyncCycles);
    }
}

// Starvation clock: counted in lockstep ticks (FrameUpdate cadence), never
// wall time (INV-16 discipline for sync decisions; wall clocks pace resends
// only).
static void UpdateStarvationInterrogation() {
    if (!s_epochActive || !s_inputPhaseActive) {
        return;
    }
    if (HasRemoteInputFrame(s_consumeFrame)) {
        s_starvedFrames = 0;
        s_resyncAwaitingReply = false;
        return;
    }
    s_starvedFrames++;
    if (s_starvedFrames < FRONTEND_STARVATION_INTERROGATE_FRAMES) {
        return;
    }
    if (!TransportHealthyForInterrogation()) {
        // Transport itself is in trouble — the supervisor's silence ladder
        // owns this case (INV-14); do not interrogate over a dead link.
        return;
    }
    s_starvedFrames = 0;
    SendResyncRequest("starvation threshold");
}

static void NotePhaseIdMismatch(uint32_t epochId, uint8_t packetPhaseId, PacketType type) {
    s_phaseIdMismatchCount++;
    if ((s_phaseIdMismatchCount % 20u) == 1u) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Coherent-but-unexpected phase_id packet: type=%s epoch=%u packet_phase_id=%u local_phase_id=%u count=%u",
            PacketTypeName(type),
            epochId,
            packetPhaseId,
            CurrentPhaseIdByte(),
            s_phaseIdMismatchCount);
    }
    // §3.4: at threshold this triggers the INV-11 interrogation instead of
    // silent dropping forever.
    if (s_phaseIdMismatchCount >= FRONTEND_PHASE_ID_MISMATCH_THRESHOLD &&
        TransportHealthyForInterrogation()) {
        s_phaseIdMismatchCount = 0;
        SendResyncRequest("phase_id mismatch threshold");
    }
}

// §3.4 frame acceptance: (epoch, phase_id) with the mismatch counter.
static bool AcceptFramePacketIdentity(uint32_t epochId,
                                      uint8_t packetPhaseId,
                                      PacketType type) {
    if (!s_epochActive) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Ignored frame input with no active epoch: type=%s epoch=%u phase_id=%u",
            PacketTypeName(type), epochId, packetPhaseId);
        return false;
    }
    if (epochId != s_epochId) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Ignored stale-epoch frame input: type=%s packet_epoch=%u local_epoch=%u",
            PacketTypeName(type), epochId, s_epochId);
        return false;
    }
    if (packetPhaseId != CurrentPhaseIdByte()) {
        NotePhaseIdMismatch(epochId, packetPhaseId, type);
        return false;
    }
    return true;
}

static void HandleRemoteFrameInput(uint32_t epochId,
                                   uint8_t packetPhaseId,
                                   uint16_t phase,
                                   uint32_t frame,
                                   uint32_t ackFrame,
                                   const uint16_t* inputs,
                                   uint16_t inputCount,
                                   PacketType type) {
    if (!AcceptFramePacketIdentity(epochId, packetPhaseId, type)) {
        return;
    }
    (void)phase;  // diagnostic only; acceptance is keyed on (epoch, phase_id)
    if (type != s_packetType) {
        // In-flight packet from the previous screen crossing a phase edge
        // (e.g. a CharSel frame arriving after StageSel began). Pure ordering
        // artifact on a healthy link — drop it; latching recovery here killed
        // live sessions.
        Rollback::NetplayLog_Verbose(
            "FRONTEND", -1,
            "Dropped frame input of mismatched packet type: got=%s active=%s phase=%s",
            PacketTypeName(type),
            PacketTypeName(s_packetType),
            FrontendSyncPhaseName(s_phase));
        return;
    }
    if (!s_inputPhaseActive) {
        return;
    }
    if (inputCount == 0 || inputCount > FRONTEND_INPUT_REDUNDANCY) {
        // Malformed or stale packet — drop it rather than killing the session.
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Dropped frame input with invalid redundancy: count=%u max=%u type=%s",
            inputCount,
            (unsigned)FRONTEND_INPUT_REDUNDANCY,
            PacketTypeName(type));
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
        if (HasRemoteInputFrame(s_consumeFrame)) {
            s_waitingForCurrentFrameSince = 0;
        }
        s_starvationPressureSamples = 0;
        // Accepted remote traffic ends the current interrogation cycle
        // (recovered — INV-11 ladder resets).
        s_starvedFrames = 0;
        s_resyncCycles = 0;
        s_resyncAwaitingReply = false;
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
                "FRONTNET", -1,
                "Unable to satisfy targeted frontend resend: type=%s epoch=%u phase=%s requested=%u local=%u consume=%u remote_ack=%u remote_frame=%u ignored_future=%d remote_contig=%u",
                PacketTypeName(type),
                epochId,
                FrontendSyncPhaseName(s_phase),
                resendFrame,
                s_localInputFrame,
                s_consumeFrame,
                s_remoteAckFrame,
                frame,
                futureFramesIgnored,
                GetRemoteContiguousFrameExclusive());
        }
    }

    if (futureFramesIgnored > 0 &&
        (s_lastRingWindowPressureLogTime == 0 ||
         (now - s_lastRingWindowPressureLogTime) >= 250)) {
        s_lastRingWindowPressureLogTime = now;
        Rollback::NetplayLog_Write(
            "FRONTNET", -1,
            "Remote frontend input ahead of retain window: type=%s epoch=%u phase=%s frame=%u consume=%u ahead=%u ack=%u count=%u ignored_future=%d accepted_history=%d remote_contig=%u remote_latest=%u",
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
            GetRemoteContiguousFrameExclusive(),
            s_remoteLatestFrame);
    }

    if (futureFramesIgnored > 0) {
        ScheduleLocalDelayIncrease(
            ClampFrontendDelay((uint16_t)(GetEffectiveDelayFloor() + 1)),
            FrontendDelayBumpReason::Starvation,
            "remote frontend input beyond retain window");
    }

    const bool sampleFrameTrace = ShouldLogRemoteFrameTrace(frame, now);
    if (!acceptedAny && frame <= previousRemoteLatest && sampleFrameTrace) {
        Rollback::NetplayLog_Verbose(
            "FRONTNET", -1,
            "Ignored duplicate/out-of-order remote frame input: type=%s epoch=%u phase=%s frame=%u ack=%u consume=%u remote_contig=%u remote_latest=%u",
            PacketTypeName(type),
            epochId,
            FrontendSyncPhaseName(s_phase),
            frame,
            ackFrame,
            s_consumeFrame,
            GetRemoteContiguousFrameExclusive(),
            previousRemoteLatest);
    } else if (acceptedAny && frame < previousRemoteLatest && sampleFrameTrace) {
        Rollback::NetplayLog_Verbose(
            "FRONTNET", -1,
            "Accepted out-of-order remote frame history fill: type=%s epoch=%u phase=%s frame=%u ack=%u new=%d consume=%u remote_contig=%u remote_latest=%u",
            PacketTypeName(type),
            epochId,
            FrontendSyncPhaseName(s_phase),
            frame,
            ackFrame,
            newFramesApplied,
            s_consumeFrame,
            GetRemoteContiguousFrameExclusive(),
            previousRemoteLatest);
    }

    if (sampleFrameTrace && (acceptedAny || futureFramesIgnored > 0)) {
        Rollback::NetplayLog_Verbose(
            "FRONTNET", -1,
            "Remote frame input sample: type=%s epoch=%u phase=%s frame=%u ack=%u count=%u new=%d consume=%u remote_contig=%u remote_latest=%u ignored_future=%d",
            PacketTypeName(type),
            epochId,
            FrontendSyncPhaseName(s_phase),
            frame,
            ackFrame,
            inputCount,
            newFramesApplied,
            s_consumeFrame,
            GetRemoteContiguousFrameExclusive(),
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

void FrontendInputSync_SetWinScreenSendGate(FrontendWinScreenSendGate gate) {
    s_winScreenSendGate = gate;
}

void FrontendInputSync_SetTransportHealthyGate(FrontendTransportHealthyGate gate) {
    s_transportHealthyGate = gate;
}

int FrontendInputSync_ComputeDelayProposal() {
    const FrontendDelayProposalDetails details = ComputeDelayProposalDetails();

    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Local frontend delay: configured_delay=%u configured_floor=%u base=%u jitter_bump=%u final=%u avg_ping=%.1f variance=%.1f one_way=%.2f jitter_frames=%.2f valid=%d",
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
                                  uint16_t frontendDelay,
                                  const char* reason) {
    // INV-8: one frontend sync machine per epoch — beginning an epoch cancels
    // every deferred/pending machine of any prior epoch by construction.
    ClearEpochState();
    s_epochActive = true;
    s_role = role;
    s_epochId = epochId;
    s_frontendDelay = ClampFrontendDelay(frontendDelay);
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Begin epoch: epoch=%u role=%s frontend_delay=%u reason=%s",
        s_epochId,
        SessionRoleName(role),
        s_frontendDelay,
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

bool FrontendInputSync_IsEpochActive() {
    return s_epochActive;
}

void FrontendInputSync_StopWinScreenInputPhase(const char* reason) {
    if (s_phase == FrontendSyncPhase::WinScreen && s_inputPhaseActive) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Stopping win-screen input phase: epoch=%u reason=%s",
            s_epochId,
            reason ? reason : "?");
        FrontendInputSync_EndPhase(reason ? reason : "win-screen stop");
    }
    if (s_packetType == PacketType::WinScreenFrameInput) {
        s_packetType = PacketType::CharSelFrameInput;
    }
}

uint16_t FrontendInputSync_GetFrontendDelay() {
    return s_frontendDelay;
}

uint32_t FrontendInputSync_GetEpochId() {
    return s_epochId;
}

void FrontendInputSync_BeginInputPhase(FrontendSyncPhase phase,
                                       PacketType packetType,
                                       const char* reason) {
    if (!s_epochActive) {
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "ERROR: BeginInputPhase without active epoch: phase=%s reason=%s",
            FrontendSyncPhaseName(phase),
            reason ? reason : "?");
        return;
    }
    if (s_inputPhaseActive) {
        // INV-8 fail-log: a second Begin while a machine is active means a
        // caller missed an EndPhase. The old machine is torn down (never two
        // machines for one epoch), loudly.
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "ERROR: BeginInputPhase while phase %s active (epoch=%u) — replacing (INV-8)",
            FrontendSyncPhaseName(s_phase),
            s_epochId);
    }
    ClearPhaseInputState();
    s_phase = phase;
    s_packetType = packetType;
    s_inputPhaseActive = true;
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Begin input phase: epoch=%u phase=%s phase_id=%u packet=%s frontend_delay=%u max_local_lead=%u reason=%s",
        s_epochId,
        FrontendSyncPhaseName(phase),
        CurrentPhaseIdByte(),
        PacketTypeName(packetType),
        s_frontendDelay,
        GetFrontendMaxLocalLead(),
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

FrontendPhaseId FrontendInputSync_GetPhaseId() {
    return FrontendSyncPhaseToPhaseId(s_phase);
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

uint32_t FrontendInputSync_GetRemoteContiguousFrameExclusive() {
    return GetRemoteContiguousFrameExclusive();
}

void FrontendInputSync_FrameUpdate() {
    if (!s_epochActive) {
        return;
    }
    MaybeApplyPendingDelay();
    if (!s_inputPhaseActive) {
        return;
    }

    UpdateStarvationInterrogation();

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

    UpdateFrontendInputLatch(packedInput);
    EnsureLocalLead("delay lead fill");

    const DWORD now = NowMs();
    const uint32_t captureLimit = GetLocalCaptureHeadLimitExclusive();
    const uint32_t maxLocalLead = GetFrontendMaxLocalLead();
    const bool currentRemoteMissing = !HasRemoteInputFrame(s_consumeFrame);
    const bool leadCapped = GetLocalLeadFrames() >= maxLocalLead;
    if (s_localInputFrame >= captureLimit || (currentRemoteMissing && leadCapped)) {
        if (s_localInputFrame > 0) {
            SendInputPacket(s_localInputFrame - 1, "capture cap resend");
        }
        LogFrontendQueueState("local capture capped", now, false);
        return;
    }

    StoreLocalInputFrame(s_localInputFrame, ConsumeLatchedFrontendInput(packedInput));
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

    LogFrontendQueueState("waiting for current frame", now, false);
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
    const uint32_t consumedFrame = s_consumeFrame;
    *outLocal = s_localInputs[idx];
    *outRemote = s_remoteInputs[idx];
    if (outFrameId) {
        outFrameId->epoch_id = s_epochId;
        outFrameId->phase = (uint16_t)s_phase;
        outFrameId->frame = (uint16_t)consumedFrame;
    }

#if !defined(AS2_FRONTEND_SYNC_TESTING)
    SyncTrace_OnFrontendFrameConsumed(
        s_epochId,
        s_phase,
        consumedFrame,
        *outLocal,
        *outRemote);
#endif

    s_hasRemoteInput[idx] = false;
    s_consumeFrame++;
    return true;
}

void FrontendInputSync_OnRemoteCharSelFrameInput(const CharSelFrameInputPayload* p) {
    if (!p) {
        return;
    }
    HandleRemoteFrameInput(p->epoch_id,
                           p->phase_id,
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
                           p->phase_id,
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
    payload.phase_id = CurrentPhaseIdByte();
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
    if (s_localPhaseBarrierSent &&
        s_barrierNextPhase != (FrontendSyncPhase)p->next_phase &&
        s_barrierNextPhase != FrontendSyncPhase::None) {
        // An in-flight barrier crossing a phase edge disagrees with ours.
        // Drop it and keep waiting — the interrogation ladder (INV-11)
        // backstops a genuine divergence. Latching recovery here killed live
        // sessions on pure packet ordering.
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Ignored phase barrier with mismatched next-phase: local=%s remote=%s epoch=%u",
            FrontendSyncPhaseName(s_barrierNextPhase),
            FrontendSyncPhaseName((FrontendSyncPhase)p->next_phase),
            s_epochId);
        return;
    }
    s_remotePhaseBarrierSeen = true;
    s_remoteBarrierFrame = p->last_completed_frame;
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
    FrontendBoundaryDigestPayload sendPayload = *payload;
    sendPayload._retired_serial = 0;
    sendPayload.phase_id = CurrentPhaseIdByte();
    if (!BarrierProtocol_SendPacket(PacketType::FrontendBoundaryDigest, &sendPayload, sizeof(sendPayload))) {
        return;
    }
    const FrontendDigestKind localKind = (FrontendDigestKind)sendPayload.digest_kind;
    if (s_remoteDigestSeen && s_digestKind != FrontendDigestKind::None && s_digestKind != localKind) {
        // The previously-received remote digest belongs to a different
        // boundary kind (in-flight packet crossing a phase edge). Discard the
        // stale remote digest and continue; killing the session here was a
        // pure ordering casualty.
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Discarding stale remote digest of different kind: had=%s now_sending=%s",
            FrontendDigestKindName(s_digestKind),
            FrontendDigestKindName(localKind));
        s_remoteDigestSeen = false;
        s_digestMatch = false;
        s_digestMismatch = false;
    }
    s_localDigestSent = true;
    s_digestKind = localKind;
    s_localDigest = sendPayload.digest;
    s_digestMatch = s_remoteDigestSeen && (s_localDigest == s_remoteDigest);
    s_digestMismatch = s_remoteDigestSeen && !s_digestMatch;
    if (s_digestMismatch) {
        Rollback::NetplayLog_Write(
            "FRONTDESYNC", -1,
            "Boundary digest mismatch after local send: epoch=%u phase=%s kind=%s frame=%u local=0x%08X remote=0x%08X action=recovery",
            sendPayload.epoch_id,
            FrontendSyncPhaseName((FrontendSyncPhase)sendPayload.phase),
            FrontendDigestKindName((FrontendDigestKind)sendPayload.digest_kind),
            sendPayload.frame,
            s_localDigest,
            s_remoteDigest);
        FrontendInputSync_RequestRecovery("frontend boundary digest mismatch");
    }
    Rollback::NetplayLog_Write(
        "FRONTEND", -1,
        "Sent boundary digest: epoch=%u phase=%s kind=%s frame=%u digest=0x%08X reason=%s",
        sendPayload.epoch_id,
        FrontendSyncPhaseName((FrontendSyncPhase)sendPayload.phase),
        FrontendDigestKindName((FrontendDigestKind)sendPayload.digest_kind),
        sendPayload.frame,
        sendPayload.digest,
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
    if (s_localDigestSent && s_digestKind != (FrontendDigestKind)p->digest_kind) {
        // In-flight digest for a different boundary kind — drop it before
        // recording anything; our own digest state stands.
        Rollback::NetplayLog_Write(
            "FRONTEND", -1,
            "Ignored remote digest of mismatched kind: local=%s remote=%s",
            FrontendDigestKindName(s_digestKind),
            FrontendDigestKindName((FrontendDigestKind)p->digest_kind));
        return;
    }
    s_remoteDigestSeen = true;
    s_remoteDigest = p->digest;
    s_digestKind = (FrontendDigestKind)p->digest_kind;
    s_digestMatch = s_localDigestSent && (s_localDigest == s_remoteDigest);
    s_digestMismatch = s_localDigestSent && !s_digestMatch;
    if (s_digestMismatch) {
        Rollback::NetplayLog_Write(
            "FRONTDESYNC", -1,
            "Boundary digest mismatch after remote digest: epoch=%u phase=%s kind=%s frame=%u local=0x%08X remote=0x%08X action=recovery",
            p->epoch_id,
            FrontendSyncPhaseName((FrontendSyncPhase)p->phase),
            FrontendDigestKindName((FrontendDigestKind)p->digest_kind),
            p->frame,
            s_localDigest,
            s_remoteDigest);
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

void FrontendInputSync_OnRemoteResyncRequest(const ResyncRequestPayload* p) {
    if (!p) {
        return;
    }
    // Always answer with our identity tuple (INV-11: interrogation is an
    // identity exchange, never a drop). Replying is legal in any state — a
    // mismatched reply is exactly what triggers the peer's EpochAlign re-run.
    ResyncReplyPayload reply{};
    reply.epoch = s_epochId;
    reply.phase_id = CurrentPhaseIdByte();
    reply.native_mode = CurrentNativeModeByte();
    reply.local_frame = s_consumeFrame;
    BarrierProtocol_SendPacket(PacketType::ResyncReply, &reply, sizeof(reply));
    Rollback::NetplayLog_Write(
        "FRONTRESYNC", -1,
        "Answered ResyncRequest: peer={epoch=%u phase_id=%u mode=%u frame=%u} local={epoch=%u phase_id=%u frame=%u}",
        p->epoch, p->phase_id, p->native_mode, p->local_frame,
        reply.epoch, reply.phase_id, reply.local_frame);
}

void FrontendInputSync_OnRemoteResyncReply(const ResyncReplyPayload* p) {
    if (!p) {
        return;
    }
    s_resyncAwaitingReply = false;
    const bool identityMatch =
        p->epoch == s_epochId && p->phase_id == CurrentPhaseIdByte();
    Rollback::NetplayLog_Write(
        "FRONTRESYNC", -1,
        "ResyncReply: peer={epoch=%u phase_id=%u mode=%u frame=%u} local={epoch=%u phase_id=%u frame=%u} match=%d cycles=%u",
        p->epoch, p->phase_id, p->native_mode, p->local_frame,
        s_epochId, CurrentPhaseIdByte(), s_consumeFrame,
        identityMatch ? 1 : 0, s_resyncCycles);
    if (!identityMatch && s_resyncEscalation == FrontendResyncEscalation::None) {
        // Confirmed identity mismatch on a live link → EpochAlign re-run
        // (§4.6 step 2). match_setup consumes and acts.
        s_resyncEscalation = s_resyncCycles >= FRONTEND_RESYNC_MAX_CYCLES
            ? FrontendResyncEscalation::Restart
            : FrontendResyncEscalation::Realign;
    }
}

FrontendResyncEscalation FrontendInputSync_ConsumeResyncEscalation() {
    const FrontendResyncEscalation out = s_resyncEscalation;
    s_resyncEscalation = FrontendResyncEscalation::None;
    return out;
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
    out->phase_id = CurrentPhaseIdByte();
    out->consume_id.epoch_id = s_epochId;
    out->consume_id.phase = (uint16_t)s_phase;
    out->consume_id.frame = (uint16_t)s_consumeFrame;
    out->local_input_frame = s_localInputFrame;
    out->remote_latest_frame = s_remoteLatestFrame;
    out->remote_contiguous_frame_exclusive = GetRemoteContiguousFrameExclusive();
    out->max_local_lead = GetFrontendMaxLocalLead();
    out->frontend_delay = s_frontendDelay;
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
    out->pending_delay_bump = s_pendingDelayBump;
    out->pending_delay = s_pendingDelay;
    out->pending_apply_from = s_pendingApplyFrom;
    out->local_advance_observed = s_localAdvanceObserved;
    out->remote_advance_observed = s_remoteAdvanceObserved;
    out->timed_out = s_timedOut;
    out->starved_frames = s_starvedFrames;
    out->resync_cycles = s_resyncCycles;
    _snprintf_s(out->recovery_reason,
                sizeof(out->recovery_reason),
                _TRUNCATE,
                "%s",
                s_recoveryReason);
}

} // namespace Net
