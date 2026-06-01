/**
 * Alice Senki 2 - Win Screen Lockstep Synchronization
 *
 * Win screen now runs on the same frontend epoch and shared frontend delay as
 * the rest of the session-owned frontend timeline.
 */

#include "net/winscreen_sync.h"

#include "net/frontend_input_sync.h"
#include "net/session_manager.h"
#include "core/game_state.h"
#include "input/input_system.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

namespace {

using namespace Net;

static bool s_initialized = false;
static bool s_active = false;
static bool s_isHost = false;
static bool s_advanceGateReleased = false;

constexpr uint16_t WINSCREEN_ADVANCE_MASK = (uint16_t)(INPUT_A | INPUT_C | INPUT_START);
constexpr int PENDING_WINSCREEN_FRAME_INPUTS = 64;

static WinScreenFrameInputPayload s_pendingRemoteFrames[PENDING_WINSCREEN_FRAME_INPUTS] = {};
static int s_pendingRemoteFrameCount = 0;
static uint32_t s_pendingRemoteFrameDropped = 0;

static bool IsAdvanceIntent(uint16_t packedInput) {
    return (packedInput & WINSCREEN_ADVANCE_MASK) != 0;
}

static void ResetRuntimeState() {
    s_active = false;
    s_isHost = false;
    s_advanceGateReleased = false;
}

static void ClearPendingRemoteFrames() {
    s_pendingRemoteFrameCount = 0;
    s_pendingRemoteFrameDropped = 0;
}

static void ResetState() {
    ResetRuntimeState();
    ClearPendingRemoteFrames();
}

static bool IsPendingCandidate(const WinScreenFrameInputPayload* p) {
    if (!p || s_active || !s_initialized) {
        return false;
    }
    if (GetGameMode() != MODE_WINSCREEN) {
        return false;
    }
    if (p->phase != (uint16_t)FrontendSyncPhase::WinScreen) {
        return false;
    }
    return true;
}

static void StorePendingRemoteFrame(const WinScreenFrameInputPayload* p) {
    if (!p) {
        return;
    }
    if (s_pendingRemoteFrameCount >= PENDING_WINSCREEN_FRAME_INPUTS) {
        for (int i = 1; i < PENDING_WINSCREEN_FRAME_INPUTS; ++i) {
            s_pendingRemoteFrames[i - 1] = s_pendingRemoteFrames[i];
        }
        s_pendingRemoteFrameCount = PENDING_WINSCREEN_FRAME_INPUTS - 1;
        s_pendingRemoteFrameDropped++;
    }

    s_pendingRemoteFrames[s_pendingRemoteFrameCount++] = *p;
    if (s_pendingRemoteFrameCount <= 4 || (s_pendingRemoteFrameCount % 16) == 0) {
        Rollback::NetplayLog_Write(
            "WINLOCK", -1,
            "Buffered early win-screen frame input: epoch=%u serial=%u frame=%u count=%u buffered=%d dropped=%u",
            p->epoch_id,
            p->phase_serial,
            p->frame,
            p->input_count,
            s_pendingRemoteFrameCount,
            s_pendingRemoteFrameDropped);
    }
}

static void DrainPendingRemoteFrames() {
    if (s_pendingRemoteFrameCount <= 0) {
        return;
    }

    int replayed = 0;
    int discarded = 0;
    const uint32_t epoch = FrontendInputSync_GetEpochId();
    const uint32_t serial = FrontendInputSync_GetPhaseSerial();
    for (int i = 0; i < s_pendingRemoteFrameCount; ++i) {
        const WinScreenFrameInputPayload& p = s_pendingRemoteFrames[i];
        if (p.epoch_id == epoch &&
            p.phase_serial == serial &&
            p.phase == (uint16_t)FrontendSyncPhase::WinScreen) {
            FrontendInputSync_OnRemoteWinScreenFrameInput(&p);
            replayed++;
        } else {
            discarded++;
        }
    }

    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "Drained early win-screen frame inputs: replayed=%d discarded=%d dropped=%u epoch=%u serial=%u",
        replayed,
        discarded,
        s_pendingRemoteFrameDropped,
        epoch,
        serial);
    ClearPendingRemoteFrames();
}

} // anonymous namespace

namespace Net {

void WinScreenSync_Init() {
    if (s_initialized) {
        return;
    }
    ResetState();
    s_initialized = true;
}

void WinScreenSync_Shutdown() {
    if (!s_initialized) {
        return;
    }
    ResetState();
    s_initialized = false;
}

void WinScreenSync_Begin() {
    if (!s_initialized) {
        return;
    }
    if (!FrontendInputSync_IsDelayNegotiated()) {
        FrontendInputSync_RequestRecovery("winscreen began without a negotiated frontend delay");
        LOG_NETPLAY(LOG_WARNING, "[WinScreenSync] Begin rejected: no negotiated frontend delay");
        return;
    }

    ResetRuntimeState();
    s_active = true;
    s_isHost = (Session_GetRole() == SessionRole::Host);
    FrontendInputSync_BeginInputPhase(
        FrontendSyncPhase::WinScreen,
        PacketType::WinScreenFrameInput,
        "winscreen begin");
    DrainPendingRemoteFrames();

    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "=== WINSCREEN BEGIN: epoch=%u shared_delay=%u role=%s ===",
        FrontendInputSync_GetEpochId(),
        FrontendInputSync_GetSharedDelay(),
        s_isHost ? "Host" : "Join");
    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Begin (shared frontend delay=%u)",
        FrontendInputSync_GetSharedDelay());
}

void WinScreenSync_Abort() {
    if (!s_active) {
        return;
    }

    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "Abort: consume=%u local=%u remote=%u local_adv=%d remote_adv=%d recovery=%s",
        FrontendInputSync_GetConsumeFrame(),
        FrontendInputSync_GetLocalInputFrame(),
        FrontendInputSync_GetRemoteLatestFrame(),
        FrontendInputSync_LocalAdvanceObserved() ? 1 : 0,
        FrontendInputSync_RemoteAdvanceObserved() ? 1 : 0,
        FrontendInputSync_HasRecoveryRequest() ? FrontendInputSync_GetRecoveryReason() : "none");

    FrontendInputSync_EndPhase("winscreen abort");
    ResetState();
    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Abort");
}

bool WinScreenSync_FrameUpdate() {
    if (!s_active || !s_initialized) {
        return false;
    }

    if (GetGameMode() != MODE_WINSCREEN) {
        WinScreenSync_Abort();
        return false;
    }

    FrontendInputSync_FrameUpdate();
    if (FrontendInputSync_HasRecoveryRequest()) {
        Rollback::NetplayLog_Write(
            "WINLOCK", -1,
            "Win-screen frontend timeout; aborting lockstep: %s",
            FrontendInputSync_GetRecoveryReason());
        WinScreenSync_Abort();  // clears recovery request via EndPhase
        return false;           // caller must disconnect
    }
    return true;
}

void WinScreenSync_CaptureLocalInput(uint16_t packedInput) {
    if (!s_active) {
        return;
    }
    FrontendInputSync_CaptureLocalInput(packedInput);
}

bool WinScreenSync_HasInputsForCurrentFrame() {
    return FrontendInputSync_HasInputsForCurrentFrame();
}

bool WinScreenSync_ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2) {
    if (!s_active || !outP1 || !outP2) {
        return false;
    }

    uint16_t localInput = 0;
    uint16_t remoteInput = 0;
    if (!FrontendInputSync_ConsumeCurrentFrame(&localInput, &remoteInput, nullptr)) {
        return false;
    }

    const bool localAdvance = IsAdvanceIntent(localInput);
    const bool remoteAdvance = IsAdvanceIntent(remoteInput);
    if (localAdvance) {
        FrontendInputSync_ReportLocalAdvanceIntent(1);
    }
    if (remoteAdvance) {
        FrontendInputSync_ReportRemoteAdvanceIntent(1);
    }

    const bool releaseRequested =
        s_advanceGateReleased ||
        localAdvance ||
        remoteAdvance ||
        FrontendInputSync_LocalAdvanceObserved() ||  // raw press already signaled this frame
        FrontendInputSync_RemoteAdvanceObserved();

    if (!releaseRequested) {
        if (localAdvance || remoteAdvance) {
            Rollback::NetplayLog_Verbose(
                "WINLOCK", -1,
                "Holding win-screen advance: local_adv=%d remote_adv=%d",
                FrontendInputSync_LocalAdvanceObserved() ? 1 : 0,
                FrontendInputSync_RemoteAdvanceObserved() ? 1 : 0);
        }
        localInput = (uint16_t)(localInput & (uint16_t)~WINSCREEN_ADVANCE_MASK);
        remoteInput = (uint16_t)(remoteInput & (uint16_t)~WINSCREEN_ADVANCE_MASK);
    } else {
        if (!s_advanceGateReleased) {
            s_advanceGateReleased = true;
            Rollback::NetplayLog_Write(
                "WINLOCK", -1,
                "Win-screen advance gate released: local_adv=%d remote_adv=%d requester=%s",
                FrontendInputSync_LocalAdvanceObserved() ? 1 : 0,
                FrontendInputSync_RemoteAdvanceObserved() ? 1 : 0,
                localAdvance ? "local" : (remoteAdvance ? "remote" : "legacy"));
        }
        localInput = (uint16_t)(localInput | INPUT_A);
        remoteInput = (uint16_t)(remoteInput | INPUT_A);
    }

    if (s_isHost) {
        *outP1 = localInput;
        *outP2 = remoteInput;
    } else {
        *outP1 = remoteInput;
        *outP2 = localInput;
    }
    return true;
}

void WinScreenSync_NotifyLocalRawAdvance(uint16_t rawInput) {
    if (!s_active || s_advanceGateReleased) {
        return;
    }
    if (IsAdvanceIntent(rawInput)) {
        FrontendInputSync_ReportLocalAdvanceIntent(rawInput);
        Rollback::NetplayLog_Write(
            "WINLOCK", -1,
            "Raw local advance intent: input=0x%04X gate not yet released",
            rawInput);
    }
}

bool WinScreenSync_IsActive() {
    return s_active;
}

bool WinScreenSync_LocalConfirmed() {
    return FrontendInputSync_LocalAdvanceObserved();
}

bool WinScreenSync_RemoteConfirmed() {
    return FrontendInputSync_RemoteAdvanceObserved();
}

bool WinScreenSync_BothConfirmed() {
    return FrontendInputSync_BothAdvanceObserved();
}

uint32_t WinScreenSync_GetConsumeFrame() {
    return FrontendInputSync_GetConsumeFrame();
}

uint32_t WinScreenSync_GetRemoteLatestFrame() {
    return FrontendInputSync_GetRemoteLatestFrame();
}

void WinScreenSync_OnRemoteFrameInput(const WinScreenFrameInputPayload* p) {
    if (IsPendingCandidate(p)) {
        StorePendingRemoteFrame(p);
        return;
    }
    FrontendInputSync_OnRemoteWinScreenFrameInput(p);
}

void WinScreenSync_OnRemoteConfirm() {
    if (!s_active) {
        return;
    }
    FrontendInputSync_ReportRemoteAdvanceIntent(1);
}

} // namespace Net
