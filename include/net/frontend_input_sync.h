/**
 * Alice Senki 2 - Unified Frontend Input Sync
 *
 * Owns the shared epoch/phase/frame timeline for lockstep frontend phases:
 * character select, stage select, and win screen.
 *
 * The core provides:
 *   - one epoch-scoped frontend timeline
 *   - one shared negotiated frontend delay per epoch
 *   - phase-scoped input rings and resend/timeout handling
 *   - explicit phase barriers and boundary digests
 *   - increase-only live delay bump coordination
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
    RemoteRequest = 2,
    HostAdjust   = 3,
    JitterPressure = 4,
};

inline const char* FrontendDelayBumpReasonName(FrontendDelayBumpReason reason) {
    switch (reason) {
        case FrontendDelayBumpReason::None:          return "None";
        case FrontendDelayBumpReason::Starvation:    return "Starvation";
        case FrontendDelayBumpReason::RemoteRequest: return "RemoteRequest";
        case FrontendDelayBumpReason::HostAdjust:    return "HostAdjust";
        case FrontendDelayBumpReason::JitterPressure:return "JitterPressure";
        default:                                     return "Unknown";
    }
}

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
    uint32_t          phase_serial;
    FrontendFrameId   consume_id;
    uint32_t          local_input_frame;
    uint32_t          remote_latest_frame;
    uint32_t          remote_contiguous_frame_exclusive;
    uint32_t          max_local_lead;
    uint16_t          local_delay_proposal;
    uint16_t          remote_delay_proposal;
    uint16_t          shared_delay;
    bool              delay_negotiated;
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
    char              recovery_reason[128];
};

void FrontendInputSync_Init();
void FrontendInputSync_Shutdown();

int  FrontendInputSync_ComputeDelayProposal();
void FrontendInputSync_BeginEpoch(SessionRole role, uint32_t epochId, uint16_t localDelayProposal, const char* reason);
void FrontendInputSync_RebindEpoch(uint32_t epochId, const char* reason);
void FrontendInputSync_AbortEpoch(const char* reason);

/// Stop win-screen input emission immediately (rematch / cross-phase handoff).
void FrontendInputSync_StopWinScreenInputPhase(const char* reason);
void FrontendInputSync_OnRemoteSyncAnnounce(uint16_t remoteDelayProposal, const char* reason);
void FrontendInputSync_OnRemoteSyncConfirm(uint16_t remoteDelayProposal, uint16_t remoteSharedDelay, const char* reason);
bool FrontendInputSync_FinalizeDelayNegotiation(const char* reason);

bool FrontendInputSync_IsDelayNegotiated();
uint16_t FrontendInputSync_GetSharedDelay();
uint16_t FrontendInputSync_GetLocalDelayProposal();
uint16_t FrontendInputSync_GetRemoteDelayProposal();
uint32_t FrontendInputSync_GetEpochId();

void FrontendInputSync_BeginInputPhase(FrontendSyncPhase phase, PacketType packetType, const char* reason);
void FrontendInputSync_BeginPassivePhase(FrontendSyncPhase phase, const char* reason);
void FrontendInputSync_EndPhase(const char* reason);

FrontendSyncPhase FrontendInputSync_GetPhase();
bool FrontendInputSync_IsInputPhaseActive();
uint32_t FrontendInputSync_GetConsumeFrame();
uint32_t FrontendInputSync_GetLocalInputFrame();
uint32_t FrontendInputSync_GetRemoteLatestFrame();
uint32_t FrontendInputSync_GetRemoteAckFrame();
uint32_t FrontendInputSync_GetRemoteContiguousFrameExclusive();
uint32_t FrontendInputSync_GetPhaseSerial();

void FrontendInputSync_FrameUpdate();
void FrontendInputSync_CaptureLocalInput(uint16_t packedInput);
bool FrontendInputSync_HasInputsForCurrentFrame();
bool FrontendInputSync_ConsumeCurrentFrame(uint16_t* outLocal, uint16_t* outRemote, FrontendFrameId* outFrameId);

void FrontendInputSync_OnRemoteCharSelFrameInput(const CharSelFrameInputPayload* p);
void FrontendInputSync_OnRemoteWinScreenFrameInput(const WinScreenFrameInputPayload* p);

bool FrontendInputSync_IsCurrentEpochPhase(uint32_t epochId, uint16_t phase, PacketType type, const char* context);
bool FrontendInputSync_IsCurrentEpochPhaseSerial(uint32_t epochId, uint16_t phase, uint32_t phaseSerial, PacketType type, const char* context);
void FrontendInputSync_SendPhaseBarrier(FrontendSyncPhase nextPhase, uint8_t reasonCode, const char* reason);
void FrontendInputSync_OnRemotePhaseBarrier(const FrontendPhaseBarrierPayload* p);
bool FrontendInputSync_IsPhaseBarrierSatisfied(FrontendSyncPhase nextPhase);
void FrontendInputSync_ClearPhaseBarrier();

void FrontendInputSync_SendBoundaryDigest(const FrontendBoundaryDigestPayload* payload, const char* reason);
void FrontendInputSync_OnRemoteBoundaryDigest(const FrontendBoundaryDigestPayload* p);
bool FrontendInputSync_IsDigestMatched(FrontendDigestKind kind);
bool FrontendInputSync_HasDigestMismatch();

void FrontendInputSync_OnRemoteDelayChangeReq(const DelayChangeReqPayload* p);
void FrontendInputSync_OnRemoteDelayChangeAck(const DelayChangeAckPayload* p);

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
