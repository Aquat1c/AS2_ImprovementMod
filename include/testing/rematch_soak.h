/**
 * Alice Senki 2 - Rematch Soak Monitor (M6 verification)
 *
 * Passive PASS/FAIL verdict layer over the autoconnect harness rematch loop.
 *
 * The DRIVER of the loop is the existing autoconnect state machine in
 * netplay_menu_controller.cpp: with `match_count` > 1 it plays a match with
 * the fighting AI, confirms the win screen, relaunches charsel, and repeats.
 * This module only OBSERVES and judges:
 *
 *   PASS  - a rematch iteration completes a fresh pregame sync, i.e.
 *           Net::PregameSync_GetPhase() reaches GameplayHandoff again after
 *           having left it (session stayed alive across the whole cycle).
 *           Works for BOTH rematch routes of the re0.7 backend: the YES,YES
 *           continue fast path (EpochAlign(None) -> ConfigExchange, no
 *           frontend phase; logged path=fastpath) and the any-NO charsel
 *           route (path=charsel).
 *   FAIL  - the netplay menu enters DisconnectError, the session reaches
 *           SessionState::Failed, the session ends (drops back to Idle)
 *           mid-soak, PregameSync hits its Error phase, or the autoconnect
 *           driver aborts to its Failed state with an iteration in flight.
 *
 * re0.7 M8 additions (plan §7.4 + the M6/M7 journal obligations):
 *   - Epoch assertions: PregameSync_GetCurrentEpoch() must never regress and
 *     must be strictly higher at every rematch GameplayHandoff than at the
 *     previous one (§2.5 rotation). Violation = FAIL.
 *   - Canonical-counter assertion: RollbackSession_GetCurrentFrame() must be
 *     monotonic for the whole session on the engine2 backend (the engine
 *     stays armed across matches, INV-15).
 *   - PASS lines carry epoch/path/canonical; SUMMARY carries fastpath vs
 *     charsel counts. The F-7 contradiction terminal needs no dedicated
 *     probe here: it tears the session down (ProtocolViolation), which the
 *     existing FAIL detection catches — additionally grep the netplay log
 *     for "F-7 TERMINAL" (must be zero across healthy cycles; see
 *     docs/re0.7/M8_ACCEPTANCE_RUNBOOK.md).
 *
 * Enable via as2_autoconnect.cfg:
 *   [autoconnect]
 *   soak_rematches = N     ; number of REMATCH iterations to verify (N >= 1)
 *   match_count    = N+1   ; driver must cover first match + N rematches
 * or via env var AS2_SOAK_REMATCHES=N (overrides the config key).
 * To mix any-NO cycles into the run, set `continue_no_every = K` in the same
 * section (the autoconnect driver answers NO on every Kth continue prompt;
 * see netplay_menu_controller.cpp).
 *
 * All verdicts go to the mod log ("[Soak] ...") and the netplay log
 * (tag "SOAK"), one line per iteration plus a final summary.
 * See docs/RESILIENCE_TESTING.md.
 */

#pragma once

#include <stdint.h>

/// Read soak config (as2_autoconnect.cfg [autoconnect] soak_rematches, env
/// override AS2_SOAK_REMATCHES) and arm the monitor if N > 0.
/// Called from AutoConnectHarness_Init.
void RematchSoak_Init(bool isHost);

/// Per-frame observation. Called from AutoConnectHarness_Update (i.e. every
/// frame while the autoconnect driver is active, in every phase).
/// @param autoconnectStateName  current autoconnect driver state name
/// @param frameCounter          global autoconnect frame counter
void RematchSoak_FrameUpdate(const char* autoconnectStateName, uint32_t frameCounter);

/// Final flush: fail any in-flight iteration and emit the summary once.
/// Called from AutoConnectHarness_Shutdown. Safe to call when disabled.
void RematchSoak_Finish(const char* reason);

/// True when soak monitoring is armed for this run.
bool RematchSoak_IsEnabled();
