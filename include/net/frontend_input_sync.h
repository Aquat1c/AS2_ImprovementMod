/**
 * Alice Senki 2 - Unified Frontend Input Sync
 *
 * Owns the shared epoch/phase/frame timeline for lockstep frontend phases:
 * character select, stage select, and win screen.
 *
 * The core provides:
 *   - one epoch-scoped frontend timeline (epoch minted by match_setup, §2.5)
 *   - a locally derived frontend delay per epoch (peer-local, INV-23 —
 *     the delay-negotiation wire flow is retired at M5)
 *   - phase-scoped input rings and resend/timeout handling
 *   - (epoch, phase_id) frame acceptance (§3.4 — the per-side phase_serial
 *     allocator is deleted, INV-7)
 *   - explicit phase barriers and boundary digests
 *   - the ResyncRequest/Reply starvation interrogation (INV-11)
 */

#pragma once

#include "net/protocol.h"
#include "net/session_types.h"

#include <stdint.h>

namespace Net {

constexpr int FRONTEND_DELAY_MIN = 2;
constexpr int FRONTEND_DELAY_MAX = 10;

enum class FrontendSyncPhase : uint16_t {
    None      = 0,
    CharSel   = 1,
    StageSel  = 2,
    Locked    = 3,
    WinScreen = 4,
};

inline const char* FrontendSyncPhaseName(FrontendSyncPhase phase) {
    switch (phase) {
        case FrontendSyncPhase::None:      return "None";
        case FrontendSyncPhase::CharSel:   return "CharSel";
        case FrontendSyncPhase::StageSel:  return "StageSel";
        case FrontendSyncPhase::Locked:    return "Locked";
        case FrontendSyncPhase::WinScreen: return "WinScreen";
        default:                           return "Unknown";
    }
}

/// Fixed §3.4 wire identity of a frontend phase (identical on both builds by
/// construction — nothing is allocated at runtime, INV-7).
inline FrontendPhaseId FrontendSyncPhaseToPhaseId(FrontendSyncPhase phase) {
    switch (phase) {
        case FrontendSyncPhase::CharSel:   return FrontendPhaseId::CharSel;
        case FrontendSyncPhase::StageSel:  return FrontendPhaseId::StageSel;
        case FrontendSyncPhase::WinScreen: return FrontendPhaseId::WinScreen;
        default:                           return FrontendPhaseId::None;
    }
}

enum class FrontendDigestKind : uint8_t {
    None        = 0,
    CharSelLock = 1,
    StageSel    = 2,
    PreConfig   = 3,
    WinScreen   = 4,
};

inline const char* FrontendDigestKindName(FrontendDigestKind kind) {
    switch (kind) {
        case FrontendDigestKind::None:        return "None";
        case FrontendDigestKind::CharSelLock: return "CharSelLock";
        case FrontendDigestKind::StageSel:    return "StageSel";
        case FrontendDigestKind::PreConfig:   return "PreConfig";
        case FrontendDigestKind::WinScreen:   return "WinScreen";
        default:                              return "Unknown";
    }
}

enum class FrontendDelayBumpReason : uint8_t {
    None         = 0,
    Starvation   = 1,
    JitterPressure = 2,
};

inline const char* FrontendDelayBumpReasonName(FrontendDelayBumpReason reason) {
    switch (reason) {
        case FrontendDelayBumpReason::None:          return "None";
        case FrontendDelayBumpReason::Starvation:    return "Starvation";
        case FrontendDelayBumpReason::JitterPressure:return "JitterPressure";
        default:                                     return "Unknown";
    }
}

/// Escalation requested by the INV-11 interrogation ladder (§4.6). Consumed
/// (and acted on) by match_setup; this module never tears anything down.
enum class FrontendResyncEscalation : uint8_t {
    None    = 0,
    Realign = 1,   // identity mismatch confirmed → EpochAlign re-run
    Restart = 2,   // 3 failed interrogation cycles → pregame restart, fresh epoch
};

struct FrontendFrameId {
    uint32_t epoch_id;
    uint16_t phase;
    uint16_t frame;
};

struct FrontendInputSyncSnapshot {
    bool              epoch_active;
    bool              input_phase_active;
    SessionRole       role;
    FrontendSyncPhase phase;
    uint8_t           phase_id;          // FrontendPhaseId of the active phase
    FrontendFrameId   consume_id;
    uint32_t          local_input_frame;
    uint32_t          remote_latest_frame;
    uint32_t          remote_contiguous_frame_exclusive;
    uint32_t          max_local_lead;
    uint16_t          frontend_delay;    // locally derived (INV-23)
    bool              local_phase_barrier_sent;
    bool              remote_phase_barrier_seen;
    bool              phase_barrier_satisfied;
    FrontendSyncPhase barrier_next_phase;
    bool              local_digest_sent;
    bool              remote_digest_seen;
    bool              digest_match;
    bool              digest_mismatch;
    FrontendDigestKind digest_kind;
    uint32_t          local_digest;
    uint32_t          remote_digest;
    bool              pending_delay_bump;
    uint16_t          pending_delay;
    uint32_t          pending_apply_from;
    bool              local_advance_observed;
    bool              remote_advance_observed;
    bool              timed_out;
    uint32_t          starved_frames;    // frames since last accepted remote input
    uint8_t           resync_cycles;     // interrogation cycles this phase
    char              recovery_reason[128];
};

void FrontendInputSync_Init();
void FrontendInputSync_Shutdown();

/// Predicate gating win-screen frame-input sends (true = sends allowed).
/// Injected by the pregame/session layer so this module never reaches upward
/// into pregame state (M0 dependency inversion). Null (default) allows sends.
typedef bool (*FrontendWinScreenSendGate)();
void FrontendInputSync_SetWinScreenSendGate(FrontendWinScreenSendGate gate);

/// Predicate reporting a healthy/degraded transport (true = healthy enough to
/// interrogate — INV-11: starvation with a live transport is a protocol
/// fault). Injected by match_setup (supervisor verdict); null = healthy.
typedef bool (*FrontendTransportHealthyGate)();
void FrontendInputSync_SetTransportHealthyGate(FrontendTransportHealthyGate gate);

int  FrontendInputSync_ComputeDelayProposal();
void FrontendInputSync_BeginEpoch(SessionRole role, uint32_t epochId, uint16_t frontendDelay, const char* reason);
void FrontendInputSync_RebindEpoch(uint32_t epochId, const char* reason);
void FrontendInputSync_AbortEpoch(const char* reason);
bool FrontendInputSync_IsEpochActive();

/// Stop win-screen input emission immediately (rematch / cross-phase handoff).
void FrontendInputSync_StopWinScreenInputPhase(const char* reason);

/// The locally derived frontend delay for this epoch (peer-local, INV-23).
uint16_t FrontendInputSync_GetFrontendDelay();
uint32_t FrontendInputSync_GetEpochId();

void FrontendInputSync_BeginInputPhase(FrontendSyncPhase phase, PacketType packetType, const char* reason);
void FrontendInputSync_BeginPassivePhase(FrontendSyncPhase phase, const char* reason);
void FrontendInputSync_EndPhase(const char* reason);

FrontendSyncPhase FrontendInputSync_GetPhase();
FrontendPhaseId FrontendInputSync_GetPhaseId();
bool FrontendInputSync_IsInputPhaseActive();
uint32_t FrontendInputSync_GetConsumeFrame();
uint32_t FrontendInputSync_GetLocalInputFrame();
uint32_t FrontendInputSync_GetRemoteLatestFrame();
uint32_t FrontendInputSync_GetRemoteAckFrame();
uint32_t FrontendInputSync_GetRemoteContiguousFrameExclusive();

void FrontendInputSync_FrameUpdate();
void FrontendInputSync_CaptureLocalInput(uint16_t packedInput);
bool FrontendInputSync_HasInputsForCurrentFrame();
bool FrontendInputSync_ConsumeCurrentFrame(uint16_t* outLocal, uint16_t* outRemote, FrontendFrameId* outFrameId);

void FrontendInputSync_OnRemoteCharSelFrameInput(const CharSelFrameInputPayload* p);
void FrontendInputSync_OnRemoteWinScreenFrameInput(const WinScreenFrameInputPayload* p);

/// (epoch, phase) acceptance for control-plane frontend packets (CharSelLock,
/// StageSync, phase barrier, boundary digest). Frame-input packets use the
/// §3.4 (epoch, phase_id) rule internally.
bool FrontendInputSync_IsCurrentEpochPhase(uint32_t epochId, uint16_t phase, PacketType type, const char* context);
void FrontendInputSync_SendPhaseBarrier(FrontendSyncPhase nextPhase, uint8_t reasonCode, const char* reason);
void FrontendInputSync_OnRemotePhaseBarrier(const FrontendPhaseBarrierPayload* p);
bool FrontendInputSync_IsPhaseBarrierSatisfied(FrontendSyncPhase nextPhase);
void FrontendInputSync_ClearPhaseBarrier();

void FrontendInputSync_SendBoundaryDigest(const FrontendBoundaryDigestPayload* payload, const char* reason);
void FrontendInputSync_OnRemoteBoundaryDigest(const FrontendBoundaryDigestPayload* p);
bool FrontendInputSync_IsDigestMatched(FrontendDigestKind kind);
bool FrontendInputSync_HasDigestMismatch();

// INV-11 interrogation (§4.6 ladder step 2; routed by packet_router).
void FrontendInputSync_OnRemoteResyncRequest(const ResyncRequestPayload* p);
void FrontendInputSync_OnRemoteResyncReply(const ResyncReplyPayload* p);
/// Consume the pending escalation verdict (returns None when nothing pending).
FrontendResyncEscalation FrontendInputSync_ConsumeResyncEscalation();

void FrontendInputSync_ReportLocalAdvanceIntent(uint16_t packedInput);
void FrontendInputSync_ReportRemoteAdvanceIntent(uint16_t packedInput);
bool FrontendInputSync_LocalAdvanceObserved();
bool FrontendInputSync_RemoteAdvanceObserved();
bool FrontendInputSync_BothAdvanceObserved();

void FrontendInputSync_RequestRecovery(const char* reason);
bool FrontendInputSync_HasRecoveryRequest();
const char* FrontendInputSync_GetRecoveryReason();
void FrontendInputSync_ClearRecoveryRequest();

#if defined(AS2_FRONTEND_SYNC_TESTING)
void FrontendInputSync_Test_SetClockMs(uint32_t nowMs);
void FrontendInputSync_Test_ClearClockOverride();
#endif

void FrontendInputSync_GetSnapshot(FrontendInputSyncSnapshot* out);

} // namespace Net
