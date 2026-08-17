#pragma once

#include <cstdint>
#include "net/protocol.h"

namespace Net {

// ============================================================================
// TransitionBarrier — wire-acknowledged cross-peer phase transitions (M4).
//
// Model: the 0.6 startup-readiness 4-way barrier (the only transition in
// the 0.6 code that never failed in the field). A transition COMMITS only once both sides
// have proposed it and each has seen the other's proposal/ack. Proposals are
// resent until acked; stale/duplicate proposals are re-acked idempotently so
// packet ordering can never wedge or kill a session (INV-4/INV-6).
//
// One barrier of each kind can be in flight at a time; committing or
// resetting clears it. All packets ride the reliable control channel.
// ============================================================================

void TransitionBarrier_Init();
void TransitionBarrier_Shutdown();

// Drives resends; call once per frame.
void TransitionBarrier_FrameUpdate();

// Clear all in-flight barriers (session boundary / disconnect).
void TransitionBarrier_Reset(const char* reason);

// Clear one kind's slot (e.g. arming a fresh startup barrier for the next
// match without disturbing unrelated in-flight barriers).
void TransitionBarrier_Clear(NetTransitionKind kind, const char* reason);

// Propose a transition (idempotent — safe to call every frame while the local
// side wants the transition). `intent` carries PostMatchIntentWire for
// PostMatchDecision barriers, else 0.
void TransitionBarrier_Propose(NetTransitionKind kind, uint8_t intent, uint32_t sessionId);

// Propose an EpochAlign barrier (§4.5): payload carries {epoch, first_phase,
// native_mode}. Commit additionally requires both sides to have proposed the
// SAME {epoch, first_phase} (INV-10); native_mode is informational (each side
// completes its native exit before consuming the commit).
void TransitionBarrier_ProposeEpochAlign(uint32_t epoch, uint8_t firstPhase,
                                         uint8_t nativeMode, uint32_t sessionId);

// Remote EpochAlign proposal fields (valid while RemoteProposed(EpochAlign)).
bool TransitionBarrier_GetRemoteEpochAlign(uint32_t* epoch, uint8_t* firstPhase,
                                           uint8_t* nativeMode);

// True once BOTH sides proposed the kind and the local proposal was acked.
bool TransitionBarrier_IsCommitted(NetTransitionKind kind);

// Remote's proposal state for the kind (valid until Reset/commit-clear).
bool TransitionBarrier_RemoteProposed(NetTransitionKind kind);
uint8_t TransitionBarrier_GetRemoteIntent(NetTransitionKind kind);

// Consume a committed barrier: returns true once per commit, then clears it.
bool TransitionBarrier_ConsumeCommit(NetTransitionKind kind);

// Packet entry points — call from whichever session packet handler is active.
// Returns true if the packet type was consumed.
bool TransitionBarrier_OnPacket(PacketType type, const void* payload, size_t payloadLen);

}  // namespace Net
