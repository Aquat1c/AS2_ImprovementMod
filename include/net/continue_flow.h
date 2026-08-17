/**
 * Alice Senki 2 - Netplay Continue-Screen Rematch Flow
 *
 * Drives the vanilla Mode 9 continue screen (sub 4, handler sub_601BB0
 * @ 0x601BB0) as a per-player rematch prompt for mod-owned netplay matches.
 *
 * Contract (docs/AS2_Continue_Screen_Netplay_Rematch_Plan.md):
 *   - Each player owns their own choice; A/C locks it, LEFT/RIGHT toggles.
 *   - Both YES  -> instant rematch (same chars/stage/palettes, fresh seeds).
 *   - Any NO    -> both route to character select (existing flow).
 *   - Timeout (3600 consumed lockstep frames) -> NO.
 *   - Every decision is computed from confirmed lockstep inputs consumed by
 *     WinScreenSync — no wall clocks, no out-of-band messages decide anything.
 *
 * The module is driven by WinScreenSync's consume path (one step per consumed
 * lockstep frame); it has no FrameUpdate of its own.
 */

#pragma once

#include <stdint.h>

namespace Net {

enum class ContinueChoiceState : uint8_t {
    Deciding = 0,
    LockedYes,
    LockedNo,
};

// ============================================================================
// Lifecycle
// ============================================================================

void ContinueFlow_Init();
void ContinueFlow_Shutdown();

/// Reset to Idle. Called at winscreen lockstep begin and on abort paths.
void ContinueFlow_Reset(const char* reason);

// ============================================================================
// Feature gate (ON by default; persisted via the netplay menu settings file)
// ============================================================================

bool ContinueFlow_IsEnabled();
void ContinueFlow_SetEnabled(bool enabled);

// ============================================================================
// Drive (WinScreenSync only)
// ============================================================================

/// Called by WinScreenSync when both-advance is observed instead of finalizing
/// the winscreen lockstep — forces the continue screen (sub 4) when armed.
void ContinueFlow_OnWinScreenAdvance();

/// Step the state machine with one consumed lockstep frame. p1/p2 are the
/// game-side packed input words (host = P1 mapping, identical on both peers).
/// This is the ONLY input source for prompt decisions.
void ContinueFlow_OnConsumedFrame(uint16_t p1Inputs, uint16_t p2Inputs);

// ============================================================================
// Queries
// ============================================================================

/// True while the continue prompt owns Mode 9 sub 4.
bool ContinueFlow_IsPromptActive();

/// Input bits to strip from the injected held/just-pressed words this frame:
/// LEFT|RIGHT|A|C while the prompt is active (so sub_601BB0 never
/// self-transitions), A|C during the post-resolution carry-gate window.
uint16_t ContinueFlow_GetSuppressMask();

/// True while WinScreenSync must keep its lockstep phase alive instead of
/// finalizing at both-advance (prompt pending/active, or decline carry gate
/// still waiting for the held confirm to release).
bool ContinueFlow_ShouldHoldWinScreenFinalize();

/// Rematch resolved: mode_ownership redirects the vanilla
/// MODE_WINSCREEN -> MODE_CHARSEL route to MODE_PREMATCH_INTRO (7) instead.
bool ContinueFlow_IsRematchLatched();
void ContinueFlow_ConsumeRematchLatch();

/// Choice status for the HUD (single source of truth for both players).
ContinueChoiceState ContinueFlow_GetLocalChoiceState();
ContinueChoiceState ContinueFlow_GetRemoteChoiceState();

#if defined(AS2_FRONTEND_SYNC_TESTING)
// Test-only game-memory shim (frontend_sync_tests): the state machine runs
// against shim mode/sub/cursor variables instead of live game memory.
void ContinueFlow_Test_SetGameState(uint32_t mode, uint32_t sub);
uint32_t ContinueFlow_Test_GetSubState();
uint8_t ContinueFlow_Test_GetCursor();
#endif

} // namespace Net
