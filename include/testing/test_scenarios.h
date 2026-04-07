/**
 * Alice Senki 2 - Test Scenarios
 *
 * Built-in deterministic input scenarios for offline testing.
 * Each scenario is a named sequence of frame-indexed input entries.
 *
 * Scenario design patterns derived from fighting-game testing:
 *   - idle (neutral baseline)
 *   - walk / jump / attack patterns
 *   - projectile / special move loops
 *   - contradictory directional mash (SOCD stress)
 *   - wakeup reversal loops
 *   - CPU stress starters
 */

#pragma once

#include "testing/scripted_input_runner.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Built-in scenario constructors
// ============================================================================

// Idle 300 frames — no input (determinism baseline)
void Scenario_Idle300(SIR_Scenario* out);

// Walk forward, jump, light attack — basic movement test
void Scenario_WalkJumpLight(SIR_Scenario* out);

// Repeated projectile motion input (236A) — special move loop
void Scenario_ProjectileLoop(SIR_Scenario* out);

// Contradictory directional mash — SOCD / stress test
void Scenario_MashDirections(SIR_Scenario* out);

// Wakeup DP (623A) loop — reversal timing test
void Scenario_WakeupDPLoop(SIR_Scenario* out);

// CPU stress — rapid varied inputs for VS CPU determinism testing
void Scenario_CpuStressStarter(SIR_Scenario* out);

// ============================================================================
// Registry — populates the runner's scenario list
// ============================================================================

// Returns the number of built-in scenarios
int Scenarios_GetBuiltinCount(void);

// Fill scenario at index into *out. Returns false if index out of range.
bool Scenarios_GetBuiltin(int index, SIR_Scenario* out);

// Get name of built-in scenario by index (for UI display)
const char* Scenarios_GetBuiltinName(int index);

#ifdef __cplusplus
}
#endif
