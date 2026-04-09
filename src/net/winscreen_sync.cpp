/**
 * Alice Senki 2 - Win Screen Lockstep Synchronization
 *
 * Mode 9 is a full input-driven state machine (multiple interactive substates).
 * This module exchanges per-frame inputs with redundancy and exposes a lockstep
 * consume interface for Hook_InputDispatcher.
 */

#include "net/winscreen_sync.h"
#include "net/barrier_protocol.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <string.h>

namespace {

using namespace Net;

static const int RING_SIZE = 512;
static const int RING_MASK = RING_SIZE - 1;
static const int INPUT_REDUNDANCY =
    (int)(sizeof(((WinScreenFrameInputPayload*)0)->inputs) / sizeof(uint16_t));
static const int MIN_INPUT_DELAY = 1;
static const int MAX_INPUT_DELAY = 6;
static const int DEFAULT_INPUT_DELAY = 2;
static const int SEND_HEAD_BUFFER = RING_SIZE / 2;
static const DWORD LOCKSTEP_TIMEOUT_MS = 10000;
static const DWORD RESEND_INTERVAL_MS = 100;
static const DWORD TARGETED_RESEND_INTERVAL_MS = 100;

static_assert(INPUT_REDUNDANCY > 0, "WinScreenFrameInputPayload requires input slots");

static bool     s_initialized = false;
static bool     s_active = false;
static bool     s_isHost = false;
static bool     s_timedOut = false;

static uint32_t s_consumeFrame = 0;
static uint32_t s_localInputFrame = 0;
static uint32_t s_remoteLatestFrame = 0;

static uint16_t s_localInputs[RING_SIZE];
static uint16_t s_remoteInputs[RING_SIZE];
static bool     s_hasLocalInput[RING_SIZE];
static bool     s_hasRemoteInput[RING_SIZE];

static int      s_inputDelay = DEFAULT_INPUT_DELAY;
static bool     s_delayLocked = false;
static DWORD    s_lastRemoteInputTime = 0;
static DWORD    s_lastResendTime = 0;
static DWORD    s_lastTargetedResendTime = 0;
static uint32_t s_waitLogCounter = 0;

// Legacy diagnostics used by existing cleanup logs/UI.
static bool     s_localAdvanceObserved = false;
static bool     s_remoteAdvanceObserved = false;

static bool IsAdvanceIntent(uint16_t packedInput) {
    return (packedInput & (INPUT_A | INPUT_C | INPUT_START)) != 0;
}

static void SendFrameInputPacket(uint32_t frame, const char* reason) {
    WinScreenFrameInputPayload payload{};
    payload.frame = frame;
    payload.ack_frame = s_consumeFrame;

    int count = 0;
    for (int i = 0; i < INPUT_REDUNDANCY; i++) {
        int f = (int)frame - i;
        if (f < 0) break;
        const int idx = f & RING_MASK;
        if (!s_hasLocalInput[idx]) break;
        payload.inputs[i] = s_localInputs[idx];
        count++;
    }
    payload.input_count = (uint16_t)count;

    const bool sent = BarrierProtocol_SendPacket(
        PacketType::WinScreenFrameInput,
        &payload,
        sizeof(payload));

    if (!sent) {
        Rollback::NetplayLog_Write("WINLOCK", -1,
            "SendFrameInputPacket FAILED: frame=%u ack=%u count=%u reason=%s active=%d",
            payload.frame,
            payload.ack_frame,
            payload.input_count,
            reason ? reason : "?",
            s_active ? 1 : 0);
        return;
    }

    Rollback::NetplayLog_Verbose("WINLOCK", -1,
        "SendFrameInputPacket: frame=%u ack=%u count=%u reason=%s consume=%u local=%u remote=%u",
        payload.frame,
        payload.ack_frame,
        payload.input_count,
        reason ? reason : "?",
        s_consumeFrame,
        s_localInputFrame,
        s_remoteLatestFrame);
}

static int LockInputDelayFromRTT() {
    ConnectionStats stats{};
    Session_GetStats(&stats);
    int delay = (int)ceilf(stats.rtt_ms * 60.0f / 1000.0f / 2.0f) + 1;
    if (delay < MIN_INPUT_DELAY) delay = MIN_INPUT_DELAY;
    if (delay > MAX_INPUT_DELAY) delay = MAX_INPUT_DELAY;
    return delay;
}

static void MaybeResendWhileWaiting() {
    if (!s_active || !Session_IsConnected() || s_localInputFrame == 0) return;

    const DWORD now = GetTickCount();
    if ((now - s_lastResendTime) >= RESEND_INTERVAL_MS) {
        SendFrameInputPacket(s_localInputFrame - 1, "periodic latest resend");
        s_lastResendTime = now;
    }

    if ((now - s_lastTargetedResendTime) >= TARGETED_RESEND_INTERVAL_MS &&
        s_consumeFrame < s_localInputFrame) {
        const int targetIdx = (int)(s_consumeFrame & RING_MASK);
        if (s_hasLocalInput[targetIdx]) {
            SendFrameInputPacket(s_consumeFrame, "targeted resend for peer ack");
        }
        s_lastTargetedResendTime = now;
    }
}

static void ResetState() {
    s_active = false;
    s_isHost = false;
    s_timedOut = false;
    s_consumeFrame = 0;
    s_localInputFrame = 0;
    s_remoteLatestFrame = 0;
    s_inputDelay = DEFAULT_INPUT_DELAY;
    s_delayLocked = false;
    s_lastRemoteInputTime = 0;
    s_lastResendTime = 0;
    s_lastTargetedResendTime = 0;
    s_waitLogCounter = 0;
    s_localAdvanceObserved = false;
    s_remoteAdvanceObserved = false;
    memset(s_localInputs, 0, sizeof(s_localInputs));
    memset(s_remoteInputs, 0, sizeof(s_remoteInputs));
    memset(s_hasLocalInput, 0, sizeof(s_hasLocalInput));
    memset(s_hasRemoteInput, 0, sizeof(s_hasRemoteInput));
}

} // anonymous namespace

namespace Net {

void WinScreenSync_Init() {
    if (s_initialized) return;
    ResetState();
    s_initialized = true;
    LOG_NETPLAY(LOG_DEBUG, "[WinScreenSync] Initialized (lockstep)");
}

void WinScreenSync_Shutdown() {
    if (!s_initialized) return;
    ResetState();
    s_initialized = false;
    LOG_NETPLAY(LOG_DEBUG, "[WinScreenSync] Shutdown");
}

void WinScreenSync_Begin() {
    if (!s_initialized) return;

    ResetState();
    s_active = true;
    s_isHost = (Session_GetRole() == SessionRole::Host);
    s_lastRemoteInputTime = GetTickCount();

    Rollback::NetplayLog_Write("WINLOCK", -1,
        "=== WINSCREEN LOCKSTEP BEGIN (role=%s mode=%u sub=%u) ===",
        s_isHost ? "Host" : "Join",
        GetGameMode(),
        GetSubstate());
    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Begin lockstep (role=%s)",
        s_isHost ? "Host" : "Join");
}

void WinScreenSync_Abort() {
    if (!s_active) return;

    Rollback::NetplayLog_Write("WINLOCK", -1,
        "Abort: consume=%u local=%u remote=%u local_adv=%d remote_adv=%d timeout=%d",
        s_consumeFrame,
        s_localInputFrame,
        s_remoteLatestFrame,
        s_localAdvanceObserved ? 1 : 0,
        s_remoteAdvanceObserved ? 1 : 0,
        s_timedOut ? 1 : 0);
    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Abort");
    ResetState();
}

bool WinScreenSync_FrameUpdate() {
    if (!s_active || !s_initialized) return false;

    if (GetGameMode() != MODE_WINSCREEN) {
        WinScreenSync_Abort();
        return false;
    }

    if (s_timedOut) {
        s_waitLogCounter++;
        if (s_waitLogCounter <= 5 || (s_waitLogCounter % 120) == 0) {
            Rollback::NetplayLog_Write("WINLOCK", -1,
                "Timeout state: waiting for remote input consume=%u local=%u remote=%u connected=%d",
                s_consumeFrame,
                s_localInputFrame,
                s_remoteLatestFrame,
                Session_IsConnected() ? 1 : 0);
        }
    }

    MaybeResendWhileWaiting();
    return true;
}

void WinScreenSync_CaptureLocalInput(uint16_t packedInput) {
    if (!s_active) return;

    if (IsAdvanceIntent(packedInput)) {
        s_localAdvanceObserved = true;
    }

    if (!s_delayLocked) {
        s_delayLocked = true;
        s_inputDelay = LockInputDelayFromRTT();
        ConnectionStats stats{};
        Session_GetStats(&stats);

        for (int f = 0; f < s_inputDelay; f++) {
            const int idx = f & RING_MASK;
            s_localInputs[idx] = 0;
            s_hasLocalInput[idx] = true;
            SendFrameInputPacket((uint32_t)f, "delay prefill");
        }
        s_localInputFrame = (uint32_t)s_inputDelay;

        Rollback::NetplayLog_Write("WINLOCK", -1,
            "Input delay locked: delay=%d RTT=%.1fms prefill=%d consume=%u",
            s_inputDelay,
            stats.rtt_ms,
            s_inputDelay,
            s_consumeFrame);
    }

    const uint32_t maxSendFrame = s_consumeFrame + (uint32_t)SEND_HEAD_BUFFER;
    if (s_localInputFrame > maxSendFrame) {
        if (s_localInputFrame > 0) {
            SendFrameInputPacket(s_localInputFrame - 1, "send-ahead cap");
        }
        return;
    }

    const uint32_t targetFrame = s_localInputFrame;
    const int idx = (int)(targetFrame & RING_MASK);
    s_localInputs[idx] = packedInput;
    s_hasLocalInput[idx] = true;

    SendFrameInputPacket(targetFrame, "capture");
    s_localInputFrame = targetFrame + 1;
}

bool WinScreenSync_HasInputsForCurrentFrame() {
    if (!s_active) return false;

    const int idx = (int)(s_consumeFrame & RING_MASK);
    const bool ready = s_hasLocalInput[idx] && s_hasRemoteInput[idx];
    if (ready) return true;

    const DWORD now = GetTickCount();
    if (!s_timedOut &&
        s_lastRemoteInputTime != 0 &&
        (now - s_lastRemoteInputTime) > LOCKSTEP_TIMEOUT_MS) {
        s_timedOut = true;
        LOG_NETPLAY(LOG_WARNING,
            "[WinScreenSync] Timeout waiting for remote frame (consume=%u local=%u remote=%u)",
            s_consumeFrame, s_localInputFrame, s_remoteLatestFrame);
        Rollback::NetplayLog_Write("WINLOCK", -1,
            "TIMEOUT: no new remote winscreen frame for %ums consume=%u local=%u remote=%u",
            (unsigned)(now - s_lastRemoteInputTime),
            s_consumeFrame,
            s_localInputFrame,
            s_remoteLatestFrame);
    }

    MaybeResendWhileWaiting();
    return false;
}

bool WinScreenSync_ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2) {
    if (!s_active || !outP1 || !outP2) return false;

    const int idx = (int)(s_consumeFrame & RING_MASK);
    if (!s_hasLocalInput[idx] || !s_hasRemoteInput[idx]) {
        return false;
    }

    const uint16_t local = s_localInputs[idx];
    const uint16_t remote = s_remoteInputs[idx];
    if (IsAdvanceIntent(remote)) {
        s_remoteAdvanceObserved = true;
    }

    if (s_isHost) {
        *outP1 = local;
        *outP2 = remote;
    } else {
        *outP1 = remote;
        *outP2 = local;
    }

    s_hasLocalInput[idx] = false;
    s_hasRemoteInput[idx] = false;
    s_consumeFrame++;

    Rollback::NetplayLog_Verbose("WINLOCK", -1,
        "Consume frame=%u local=0x%04X remote=0x%04X outP1=0x%04X outP2=0x%04X",
        s_consumeFrame - 1,
        local,
        remote,
        *outP1,
        *outP2);
    return true;
}

bool WinScreenSync_IsActive() {
    return s_active;
}

bool WinScreenSync_LocalConfirmed() {
    return s_localAdvanceObserved;
}

bool WinScreenSync_RemoteConfirmed() {
    return s_remoteAdvanceObserved;
}

bool WinScreenSync_BothConfirmed() {
    return s_localAdvanceObserved && s_remoteAdvanceObserved;
}

uint32_t WinScreenSync_GetConsumeFrame() {
    return s_consumeFrame;
}

uint32_t WinScreenSync_GetRemoteLatestFrame() {
    return s_remoteLatestFrame;
}

void WinScreenSync_OnRemoteFrameInput(const WinScreenFrameInputPayload* p) {
    if (!p) return;
    if (!s_active) {
        Rollback::NetplayLog_Verbose("WINLOCK", -1,
            "Remote frame input ignored (not active): frame=%u ack=%u count=%u",
            p->frame, p->ack_frame, p->input_count);
        return;
    }

    if (p->input_count == 0 || p->input_count > INPUT_REDUNDANCY) {
        Rollback::NetplayLog_Write("WINLOCK", -1,
            "Invalid remote frame payload: frame=%u ack=%u count=%u",
            p->frame, p->ack_frame, p->input_count);
        return;
    }

    const uint32_t remoteFrame = p->frame;
    bool acceptedAny = false;
    for (uint16_t i = 0; i < p->input_count; i++) {
        const int f = (int)remoteFrame - (int)i;
        if (f < 0) break;

        const int idx = f & RING_MASK;
        s_remoteInputs[idx] = p->inputs[i];
        s_hasRemoteInput[idx] = true;
        acceptedAny = true;

        if (IsAdvanceIntent(p->inputs[i])) {
            s_remoteAdvanceObserved = true;
        }

        if ((uint32_t)f > s_remoteLatestFrame) {
            s_remoteLatestFrame = (uint32_t)f;
            s_lastRemoteInputTime = GetTickCount();
        }
    }

    // Fast targeted resend: remote ACK tells us exactly which frame they need.
    const uint32_t requested = p->ack_frame;
    if (requested < s_localInputFrame) {
        const int reqIdx = (int)(requested & RING_MASK);
        const DWORD now = GetTickCount();
        if (s_hasLocalInput[reqIdx] &&
            (now - s_lastTargetedResendTime) >= TARGETED_RESEND_INTERVAL_MS) {
            SendFrameInputPacket(requested, "remote ack request");
            s_lastTargetedResendTime = now;
        }
    }

    if (acceptedAny) {
        Rollback::NetplayLog_Verbose("WINLOCK", -1,
            "Remote frame recv: frame=%u ack=%u count=%u consume=%u remoteLatest=%u",
            p->frame,
            p->ack_frame,
            p->input_count,
            s_consumeFrame,
            s_remoteLatestFrame);
    }
}

void WinScreenSync_OnRemoteConfirm() {
    // Backward compatibility for older peers that still emit one-shot confirm.
    if (!s_active) {
        Rollback::NetplayLog_Verbose("WINLOCK", -1,
            "Legacy WinScreenConfirm ignored (not active)");
        return;
    }

    s_remoteAdvanceObserved = true;
    Rollback::NetplayLog_Write("WINLOCK", -1,
        "Legacy WinScreenConfirm observed while lockstep active");
}

} // namespace Net
