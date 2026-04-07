/**
 * Alice Senki 2 - Autoconnect Test Harness
 *
 * DLL-side module that creates shared memory for the test harness launcher
 * and provides a fighting game AI for the InMatch autoconnect phase.
 *
 * The launcher (as2_test_harness.exe) writes config files and reads SHM.
 * This module writes to SHM every frame so the launcher can display
 * real-time game state for both instances.
 */

#pragma once

#include <stdint.h>

// ============================================================================
// Public API — called from netplay_menu_controller.cpp
// ============================================================================

/// Initialize harness SHM + logging. Call once after autoconnect config is loaded.
/// @param isHost  true for host instance, false for client
/// @param nickname  player nickname for SHM identity
/// @param matchDurationSec  safety timeout (0 = no limit)
/// @return true if SHM was created successfully
bool AutoConnectHarness_Init(bool isHost, const char* nickname, int matchDurationSec);

/// Per-frame update: writes to SHM, runs profiling.
/// Call every frame while autoconnect is active (all phases).
/// @param phaseName  current autoconnect state name for SHM display
/// @param phaseOrdinal  current autoconnect state ordinal
/// @param frameCounter  global autoconnect frame counter
void AutoConnectHarness_Update(const char* phaseName, uint32_t phaseOrdinal, uint32_t frameCounter);

/// Per-frame InMatch AI: generates and injects fighting inputs.
/// Call from HandleAutoConnect InMatch phase only.
/// @param isHost  true if this is the host instance
/// @param matchFrame  frames since match started (0 on first call)
/// @return the input bitmask that was injected
uint16_t AutoConnectHarness_RunFightingAI(bool isHost, uint32_t matchFrame);

/// Clean up: flush final stats, close SHM and log file.
void AutoConnectHarness_Shutdown();

/// Check if SHM is active.
bool AutoConnectHarness_IsActive();
