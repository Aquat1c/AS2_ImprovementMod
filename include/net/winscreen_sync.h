/**
 * Alice Senki 2 - Win Screen Lockstep Synchronization
 *
 * Mode 9 is input-driven across multiple substates. Lockstep begins at MatchEnd
 * (Mode 8 sub 5) so both peers stay aligned through the Mode 9 fade-in, and one
 * player's skip input propagates to both sides like offline behavior.
 */

#pragma once

#include "net/protocol.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// Lifecycle
// ============================================================================

void WinScreenSync_Init();
void WinScreenSync_Shutdown();

// ============================================================================
// Control
// ============================================================================

/// Begin win screen sync.  Called when entering Mode 9.
void WinScreenSync_Begin();

/// Same-epoch lockstep restart (M6, §4.6 ladder step 2 for the winscreen
/// stream): stop + re-begin the winscreen input phase under the UNCHANGED
/// epoch after a committed EpochAlign(first_phase=WinScreen) re-run. Unlike
/// Abort this proposes NO WinScreenExit and both sides restart their frame
/// index space identically. The continue prompt re-runs from scratch.
void WinScreenSync_RestartLockstep(const char* reason);

/// Abort / reset.  Called on disconnect or unexpected mode change.
void WinScreenSync_Abort();

// ============================================================================
// Per-Frame (called from match lifecycle or online wiring)
// ============================================================================

/// Drive lockstep maintenance (timeout/resend/mode guards) each frame.
bool WinScreenSync_FrameUpdate();

/// True once both peers confirmed advance and the handoff barrier completed.
bool WinScreenSync_IsHandoffComplete();

// ============================================================================
// Lockstep Input Interface (called by Hook_InputDispatcher)
// ============================================================================

/// Capture local player's raw packed input for the current lockstep frame.
void WinScreenSync_CaptureLocalInput(uint16_t packedInput);

/// Are both local and remote inputs available for the current consume frame?
bool WinScreenSync_HasInputsForCurrentFrame();

/// Consume the current frame's synchronized inputs and advance consume frame.
/// Host maps local→P1, join maps local→P2.
bool WinScreenSync_ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2);

/// Signal local advance intent from raw (non-delay-buffered) input so the
/// gate is released this frame instead of shared_delay frames later.
/// Safe to call every frame in the interactive sub — no-op if gate already open.
void WinScreenSync_NotifyLocalRawAdvance(uint16_t rawInput);

// ============================================================================
// Queries
// ============================================================================

/// Is winscreen lockstep currently active?
bool WinScreenSync_IsActive();

/// Legacy query: has local side produced any advance intent input (A/C/start)?
bool WinScreenSync_LocalConfirmed();

/// Legacy query: has remote side produced any advance intent input (A/C/start)?
bool WinScreenSync_RemoteConfirmed();

/// Legacy query: both local and remote advance intents observed.
bool WinScreenSync_BothConfirmed();

/// Current lockstep consume frame (for diagnostics).
uint32_t WinScreenSync_GetConsumeFrame();

/// Latest remote frame received (for diagnostics).
uint32_t WinScreenSync_GetRemoteLatestFrame();

// ============================================================================
// Packet Handler
// ============================================================================

/// Handle incoming winscreen frame-input packet.
void WinScreenSync_OnRemoteFrameInput(const WinScreenFrameInputPayload* p);

// ============================================================================
// Continue Flow Integration
// ============================================================================

/// Finalize the lockstep phase from a continue-flow rematch resolution (the
/// decline path finalizes through the normal handoff barrier instead). Both
/// peers resolve on the same consumed frame, so this skips the phase-barrier
/// handshake.
void WinScreenSync_FinalizeFromContinueFlow(const char* reason);

} // namespace Net
