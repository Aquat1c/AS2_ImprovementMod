/**
 * Alice Senki 2 — deterministic session exit (netplay)
 *
 * Replaces the vanilla pause menu during an online session.
 *
 * WHY THE PAUSE MENU CANNOT STAY. Pause is a purely LOCAL mode-8 substate
 * change. A peer that opens it stops feeding the simulation while the other
 * keeps running, and when it leaves via "return to character select" the
 * handler writes match+10 (route) and match+1 (transition flag) — both INSIDE
 * the hashed snapshot region — on one side only. Observed live in run
 * 2026-08-18_10-34-13: the opponent paused 15.5 s, quit to charsel, and the
 * session died at frame 46469 with identical HP (13000,10000 on both) and
 * DIVERGENT RNG (a2b94193 vs 54da1691) — the signature of one peer running
 * code the other did not. It also risks the 20 s progress watchdog: that pause
 * was 15.5 s.
 *
 * WHAT REPLACES IT. Holding the menu/ESC key:
 *   - in a match      -> both peers return to character select, session intact
 *   - at character select -> the session is quit gracefully
 * The hold is read from the RAW keyboard on the game thread, deliberately
 * outside the rollback input stream, so it can never enter a snapshot, be
 * predicted, or be rolled back. The peer is told over a reliable packet and
 * both sides run the same routing.
 */
#pragma once

#include <cstdint>

namespace Net {

void SessionExit_Init();
void SessionExit_Shutdown();

/// Per-frame: enforces the pause block and services the hold-to-exit gesture.
void SessionExit_FrameUpdate();

/// Remote asked to return to character select (reliable).
void SessionExit_OnRemoteAbortToCharsel();

/// Remote quit the session from character select (reliable).
void SessionExit_OnRemoteQuit();

/// The local menu-key state as an input bit, OR-ed into the outgoing input
/// word each frame. Zero when no session or the key is up.
uint16_t SessionExit_LocalMenuBit();

/// Deterministic trigger. Fed the CONFIRMED inputs for a frame -- by the
/// rollback engine on a player, and by playback on a spectator -- so the hold
/// is counted in FRAMES off the agreed stream rather than in wall-clock
/// milliseconds off one machine's keyboard. Every observer fires on the same
/// frame, which a reliable side-channel packet can never guarantee.
void SessionExit_NoteConfirmedInputs(int32_t matchRelFrame, uint16_t p1, uint16_t p2);

/// Drop hold accumulation across a match/epoch boundary.
void SessionExit_ResetHoldTracking(const char* why);

/// 0..1 progress of the current hold, for HUD feedback. False when idle.
bool SessionExit_HoldProgress(float* out01);

}  // namespace Net
