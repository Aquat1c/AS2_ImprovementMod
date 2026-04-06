/**
 * Alice Senki 2 - RNG Hooks
 *
 * Hooks the game's statically-linked rand()/srand() (§6).
 * Implements Simulation vs Visual RNG split:
 *   - Simulation RNG: Saved/restored with every savestate. Deterministic.
 *   - Visual RNG: Allowed to diverge. Cosmetic-only effects.
 *
 * CRITICAL: The game uses MSVC's static CRT rand/srand, NOT msvcrt.dll.
 *   srand() @ 0x714598 (ADDR_STATIC_SRAND)
 *   rand()  @ 0x7145A0 (ADDR_STATIC_RAND)
 *   Algorithm: seed = seed * 214013 + 2531011; return (seed >> 16) & 0x7FFF
 */

#pragma once

#include <stdint.h>

namespace RngHooks {

// Install hooks on static CRT rand/srand
bool Install();
void Uninstall();
bool IsInstalled();

// Simulation RNG — deterministic, saved/restored in savestates
uint32_t GetSimSeed();
void SetSimSeed(uint32_t seed);
int SimRand();

// Visual RNG — allowed to diverge
uint32_t GetVisualSeed();
void SetVisualSeed(uint32_t seed);

// Reset both RNG streams from a session seed (for match start)
void ResetForMatch(uint32_t sessionSeed);

// Enable/disable visual RNG split
// When disabled, all rand() calls use simulation RNG (default).
// When enabled, calls from known visual-only code paths use visual RNG.
void SetVisualSplitEnabled(bool enabled);
bool IsVisualSplitEnabled();

// Stats
struct Stats {
    uint32_t sim_rand_calls;
    uint32_t visual_rand_calls;
    uint32_t srand_calls;
};
void GetStats(Stats* out);

} // namespace RngHooks
