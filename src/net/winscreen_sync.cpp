/**
 * Alice Senki 2 - Win Screen Lockstep Synchronization
 *
 * Win screen now runs on the same frontend epoch and shared frontend delay as
 * the rest of the session-owned frontend timeline.
 */

#include "net/winscreen_sync.h"

#include "net/continue_flow.h"
#include "net/frontend_input_sync.h"
#include "net/match_lifecycle.h"
#include "net/session_manager.h"
#include "net/transition_barrier.h"
#include "core/game_state.h"
#include "input/input_system.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using namespace Net;

static bool s_initialized = false;
static bool s_active = false;
static bool s_isHost = false;
static bool s_loggedSkipPropagate = false;
static bool s_handoffPending = false;
static bool s_handoffComplete = false;
static DWORD s_activeSinceMs = 0;
static constexpr DWORD kWinScreenHandoffTimeoutMs = 45000;

constexpr uint16_t WINSCREEN_ADVANCE_MASK = (uint16_t)(INPUT_A | INPUT_C | INPUT_START);
constexpr int PENDING_WINSCREEN_FRAME_INPUTS = 64;

static WinScreenFrameInputPayload s_pendingRemoteFrames[PENDING_WINSCREEN_FRAME_INPUTS] = {};
static int s_pendingRemoteFrameCount = 0;
static uint32_t s_pendingRemoteFrameDropped = 0;

static bool IsAdvanceIntent(uint16_t packedInput) {
    return (packedInput & WINSCREEN_ADVANCE_MASK) != 0;
}

static bool IsWinScreenLockstepRoute() {
    const uint32_t mode = GetGameMode();
    if (mode == MODE_WINSCREEN) {
        return true;
    }
    return mode == MODE_MATCH &&
           GetSubstate() == MATCH_SUB_END &&
           MatchLifecycle_GetPhase() == MatchLifecyclePhase::MatchEnd;
}

static void ResetRuntimeState() {
    s_active = false;
    s_isHost = false;
    s_loggedSkipPropagate = false;
    s_handoffPending = false;
}

static void ClearPendingRemoteFrames() {
    s_pendingRemoteFrameCount = 0;
    s_pendingRemoteFrameDropped = 0;
}

static void ResetState() {
    ResetRuntimeState();
    ClearPendingRemoteFrames();
    s_handoffComplete = false;
    s_activeSinceMs = 0;
}

static void FinalizeLockstep(const char* reason) {
    if (!s_active) {
        return;
    }

    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "Finalizing win-screen lockstep: reason=%s consume=%u local_adv=%d remote_adv=%d",
        reason ? reason : "?",
        FrontendInputSync_GetConsumeFrame(),
        FrontendInputSync_LocalAdvanceObserved() ? 1 : 0,
        FrontendInputSync_RemoteAdvanceObserved() ? 1 : 0);

    FrontendInputSync_StopWinScreenInputPhase(reason ? reason : "win-screen finalize");
    FrontendInputSync_ClearPhaseBarrier();
    ResetState();
    s_handoffComplete = true;
    // Wire-acknowledged winscreen exit (M4): tells the peer we released the
    // winscreen. The commit doubles as a wire-driven rematch-restart
    // signature on the other side, replacing coincidence-based local ones.
    TransitionBarrier_Propose(NetTransitionKind::WinScreenExit, 0, 0);
    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Finalized (%s)", reason ? reason : "?");
}

static bool IsPendingCandidate(const WinScreenFrameInputPayload* p) {
    if (!p || s_active || !s_initialized) {
        return false;
    }
    if (!IsWinScreenLockstepRoute()) {
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
            "Buffered early win-screen frame input: epoch=%u phase_id=%u frame=%u count=%u buffered=%d dropped=%u",
            p->epoch_id,
            p->phase_id,
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

    // §3.4 acceptance: buffered frames replay iff they carry the current
    // (epoch, phase_id) identity — no per-side serials exist (INV-7).
    int replayed = 0;
    int discarded = 0;
    const uint32_t epoch = FrontendInputSync_GetEpochId();
    for (int i = 0; i < s_pendingRemoteFrameCount; ++i) {
        const WinScreenFrameInputPayload& p = s_pendingRemoteFrames[i];
        if (p.epoch_id == epoch &&
            p.phase_id == (uint8_t)FrontendPhaseId::WinScreen &&
            p.phase == (uint16_t)FrontendSyncPhase::WinScreen) {
            FrontendInputSync_OnRemoteWinScreenFrameInput(&p);
            replayed++;
        } else {
            discarded++;
        }
    }

    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "Drained early win-screen frame inputs: replayed=%d discarded=%d dropped=%u epoch=%u",
        replayed,
        discarded,
        s_pendingRemoteFrameDropped,
        epoch);
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
    if (s_active) {
        return;
    }
    if (!FrontendInputSync_IsEpochActive()) {
        // Transient window during cross-phase rematch adoption: the adopt
        // reset tears the epoch down one tick before the EpochAlign commit
        // re-establishes it. Latching a recovery request here killed healthy
        // rematches (observed 4x on 2026-08-16); instead defer — callers
        // retry Begin() every frame while the winscreen route is active, and
        // it succeeds as soon as the epoch lands. (M5: the delay-negotiation
        // gate is gone — the frontend delay is derived locally, INV-23.)
        static DWORD s_lastDeferLogTick = 0;
        const DWORD now = GetTickCount();
        if (s_lastDeferLogTick == 0 || (now - s_lastDeferLogTick) >= 1000) {
            s_lastDeferLogTick = now;
            LOG_NETPLAY(LOG_WARNING,
                "[WinScreenSync] Begin deferred: no active frontend epoch yet");
        }
        return;
    }

    ResetRuntimeState();
    s_active = true;
    s_isHost = (Session_GetRole() == SessionRole::Host);
    s_handoffComplete = false;
    s_handoffPending = false;
    s_activeSinceMs = GetTickCount();
    ContinueFlow_Reset("winscreen begin");
    FrontendInputSync_BeginInputPhase(
        FrontendSyncPhase::WinScreen,
        PacketType::WinScreenFrameInput,
        "winscreen begin");
    DrainPendingRemoteFrames();

    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "=== WINSCREEN BEGIN: epoch=%u frontend_delay=%u role=%s ===",
        FrontendInputSync_GetEpochId(),
        FrontendInputSync_GetFrontendDelay(),
        s_isHost ? "Host" : "Join");
    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Begin (frontend delay=%u)",
        FrontendInputSync_GetFrontendDelay());
}

void WinScreenSync_RestartLockstep(const char* reason) {
    if (!s_initialized) {
        return;
    }
    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "Restart lockstep under SAME epoch (%s): was_active=%d consume=%u",
        reason ? reason : "?",
        s_active ? 1 : 0,
        FrontendInputSync_GetConsumeFrame());
    if (s_active) {
        FrontendInputSync_StopWinScreenInputPhase("winscreen realign restart");
        FrontendInputSync_ClearPhaseBarrier();
    }
    ResetState();
    WinScreenSync_Begin();
}

void WinScreenSync_Abort() {
    if (!s_active && !s_handoffPending) {
        return;
    }

    const bool abandonedLockstep = FrontendInputSync_HasRecoveryRequest();

    Rollback::NetplayLog_Write(
        "WINLOCK", -1,
        "Abort: consume=%u local=%u remote=%u local_adv=%d remote_adv=%d recovery=%s",
        FrontendInputSync_GetConsumeFrame(),
        FrontendInputSync_GetLocalInputFrame(),
        FrontendInputSync_GetRemoteLatestFrame(),
        FrontendInputSync_LocalAdvanceObserved() ? 1 : 0,
        FrontendInputSync_RemoteAdvanceObserved() ? 1 : 0,
        FrontendInputSync_HasRecoveryRequest() ? FrontendInputSync_GetRecoveryReason() : "none");

    FrontendInputSync_StopWinScreenInputPhase("winscreen abort");
    FrontendInputSync_ClearPhaseBarrier();
    ResetState();
    // A live abort mid-prompt means the continue decision can no longer be
    // driven from consumed frames — fail closed. (The rematch path never gets
    // here: its finalize already deactivated the sync, so Abort early-outs
    // above and the rematch latch survives for mode_ownership.)
    ContinueFlow_Reset("winscreen abort");
    if (abandonedLockstep) {
        // Timeout/recovery abort — allow post-match routing without blocking rematch.
        s_handoffComplete = true;
    }
    // We are leaving the winscreen either way — let the peer know (M4 exit
    // barrier), so its rematch restart doesn't depend on local heuristics.
    TransitionBarrier_Propose(NetTransitionKind::WinScreenExit, 0, 0);
    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Abort");
}

bool WinScreenSync_FrameUpdate() {
    if (!s_initialized) {
        return true;
    }
    if (!s_active) {
        // Inactive before Begin, after successful handoff, or after Abort — not an error.
        return true;
    }

    if (!IsWinScreenLockstepRoute()) {
        WinScreenSync_Abort();
        return true;
    }

    FrontendInputSync_FrameUpdate();

    if (FrontendInputSync_BothAdvanceObserved() && !s_handoffPending && !s_handoffComplete) {
        if (ContinueFlow_ShouldHoldWinScreenFinalize()) {
            // Continue prompt owns the winscreen exit: force the continue
            // screen instead of finalizing, and keep the lockstep phase alive
            // through subs 4/5/8. Finalize happens on decline (once the carry
            // gate clears, via this barrier), on rematch (directly from the
            // resolution), or through the existing abort paths.
            ContinueFlow_OnWinScreenAdvance();
        } else {
            s_handoffPending = true;
            FrontendInputSync_SendPhaseBarrier(
                FrontendSyncPhase::None,
                1,
                "win screen both confirmed");
        }
    }

    if (s_handoffPending &&
        FrontendInputSync_IsPhaseBarrierSatisfied(FrontendSyncPhase::None)) {
        FinalizeLockstep("both peers confirmed win-screen handoff");
        return true;
    }

    if (s_activeSinceMs != 0) {
        const DWORD elapsed = GetTickCount() - s_activeSinceMs;
        if (elapsed >= kWinScreenHandoffTimeoutMs &&
            !FrontendInputSync_BothAdvanceObserved()) {
            Rollback::NetplayLog_Write(
                "WINLOCK", -1,
                "Win-screen handoff timeout after %lums; resetting frontend phase",
                (unsigned long)elapsed);
            FrontendInputSync_RequestRecovery("win screen handoff timeout");
        }
    }

    if (FrontendInputSync_HasRecoveryRequest()) {
        Rollback::NetplayLog_Write(
            "WINLOCK", -1,
            "Win-screen frontend timeout; aborting lockstep: %s",
            FrontendInputSync_GetRecoveryReason());
        WinScreenSync_Abort();
        return true;
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

    // Offline either player can skip; mirror that in netplay by propagating one
    // side's skip intent to both synchronized input streams. Disabled while the
    // continue prompt is active — propagating there would lock the peer's
    // choice with our confirm (it stays for the sub-3 win-pose skip).
    //
    // Held as a LEVEL, not an edge: once either side's advance intent has been
    // observed, the confirm stays asserted on every subsequent consumed frame
    // until native Mode 9 exits (or the continue prompt takes ownership). The
    // native win screen samples the button per substate frame; an edge-only
    // pulse released the skip on exactly one frame and stranded the peers in
    // later substates (pre-existing test failure: winscreen confirm-hold).
    const bool advanceGateOpen =
        FrontendInputSync_LocalAdvanceObserved() ||
        FrontendInputSync_RemoteAdvanceObserved();
    if (advanceGateOpen && !ContinueFlow_IsPromptActive()) {
        if (!s_loggedSkipPropagate) {
            s_loggedSkipPropagate = true;
            Rollback::NetplayLog_Write(
                "WINLOCK", -1,
                "Win-screen skip propagated to both sides: local_adv=%d remote_adv=%d requester=%s",
                localAdvance ? 1 : 0,
                remoteAdvance ? 1 : 0,
                localAdvance ? "local" : "remote");
        }
        localInput = (uint16_t)(localInput | WINSCREEN_ADVANCE_MASK);
        remoteInput = (uint16_t)(remoteInput | WINSCREEN_ADVANCE_MASK);
    }

    if (s_isHost) {
        *outP1 = localInput;
        *outP2 = remoteInput;
    } else {
        *outP1 = remoteInput;
        *outP2 = localInput;
    }

    // Continue flow steps exactly once per consumed lockstep frame — the only
    // place its edge state may come from (determinism: confirmed inputs only).
    // Runs before the caller injects the words, so a suppression mask decided
    // here already applies to this same frame.
    ContinueFlow_OnConsumedFrame(*outP1, *outP2);
    return true;
}

void WinScreenSync_NotifyLocalRawAdvance(uint16_t rawInput) {
    if (!s_active) {
        return;
    }
    if (IsAdvanceIntent(rawInput)) {
        FrontendInputSync_ReportLocalAdvanceIntent(rawInput);
    }
}

bool WinScreenSync_IsActive() {
    return s_active;
}

bool WinScreenSync_IsHandoffComplete() {
    return s_handoffComplete;
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

void WinScreenSync_FinalizeFromContinueFlow(const char* reason) {
    // Continue-flow resolution point: both peers call this on the same
    // consumed frame, so the phase-barrier handshake is unnecessary here.
    FinalizeLockstep(reason ? reason : "continue flow finalize");
}

} // namespace Net
