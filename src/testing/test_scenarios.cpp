/**
 * Alice Senki 2 - Test Scenarios (Implementation)
 *
 * Built-in deterministic input scenarios. Each builds a SIR_Scenario struct
 * with frame-indexed input entries.
 *
 * Input constants (from input_system.h / as2_constants.h):
 *   INPUT_UP=0x0001  INPUT_DOWN=0x0002  INPUT_LEFT=0x0004  INPUT_RIGHT=0x0008
 *   INPUT_A=0x0010   INPUT_B=0x0020     INPUT_C=0x0040     INPUT_D=0x0080
 *   INPUT_START=0x0100  INPUT_SELECT=0x0200
 *
 * Patterns inspired by old harness AI/mash logic and standard fighting-game
 * test sequences. No dependency on old harness modules.
 */

#include "testing/test_scenarios.h"
#include "input_system.h"
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static void ClearScenario(SIR_Scenario* s) {
    memset(s, 0, sizeof(*s));
}

static void AddEntry(SIR_Scenario* s, int start, int end, int player, uint16_t input) {
    if (s->entryCount >= SIR_MAX_ENTRIES) return;
    SIR_InputEntry* e = &s->entries[s->entryCount++];
    e->frameStart = start;
    e->frameEnd   = end;
    e->player     = player;
    e->input      = input;
}

// ============================================================================
// Scenario: Idle300 — 300 frames of no input
// ============================================================================

void Scenario_Idle300(SIR_Scenario* out) {
    ClearScenario(out);
    strncpy(out->name, "Idle300", SIR_MAX_NAME - 1);
    out->totalFrames = 300;
    out->hasP2 = false;
    // No entries — all frames produce 0 input
}

// ============================================================================
// Scenario: WalkJumpLight — basic movement + attack
// Pattern (P1 only, 240 frames):
//   0-29:   walk right
//   30-39:  jump (up)
//   40-44:  light attack (A) in air
//   45-59:  idle (landing)
//   60-89:  walk left
//   90-99:  jump
//  100-104: air A
//  105-119: idle
//  120-149: walk right
//  150-159: crouch (down)
//  160-164: crouching A
//  165-179: idle
//  180-209: walk right
//  210-219: standing A
//  220-239: idle
// ============================================================================

void Scenario_WalkJumpLight(SIR_Scenario* out) {
    ClearScenario(out);
    strncpy(out->name, "WalkJumpLight", SIR_MAX_NAME - 1);
    out->totalFrames = 240;
    out->hasP2 = false;

    AddEntry(out,   0,  29, 0, INPUT_RIGHT);
    AddEntry(out,  30,  39, 0, INPUT_UP);
    AddEntry(out,  40,  44, 0, INPUT_A);
    // 45-59: idle
    AddEntry(out,  60,  89, 0, INPUT_LEFT);
    AddEntry(out,  90,  99, 0, INPUT_UP);
    AddEntry(out, 100, 104, 0, INPUT_A);
    // 105-119: idle
    AddEntry(out, 120, 149, 0, INPUT_RIGHT);
    AddEntry(out, 150, 159, 0, INPUT_DOWN);
    AddEntry(out, 160, 164, 0, INPUT_DOWN | INPUT_A);
    // 165-179: idle
    AddEntry(out, 180, 209, 0, INPUT_RIGHT);
    AddEntry(out, 210, 219, 0, INPUT_A);
    // 220-239: idle
}

// ============================================================================
// Scenario: ProjectileLoop — repeated 236A (QCF+A) motion
// Pattern (P1, 300 frames, repeating ~50-frame cycle):
//   Cycle: down(6f) -> down-right(6f) -> right(6f) -> right+A(4f) -> idle(28f)
// Loops 6 times
// ============================================================================

void Scenario_ProjectileLoop(SIR_Scenario* out) {
    ClearScenario(out);
    strncpy(out->name, "ProjectileLoop", SIR_MAX_NAME - 1);
    out->totalFrames = 300;
    out->hasP2 = false;

    int cycle = 50;
    for (int i = 0; i < 6; i++) {
        int base = i * cycle;
        AddEntry(out, base +  0, base +  5, 0, INPUT_DOWN);
        AddEntry(out, base +  6, base + 11, 0, INPUT_DOWN | INPUT_RIGHT);
        AddEntry(out, base + 12, base + 17, 0, INPUT_RIGHT);
        AddEntry(out, base + 18, base + 21, 0, INPUT_RIGHT | INPUT_A);
        // base+22 to base+49: idle (recovery + wait)
    }
}

// ============================================================================
// Scenario: MashDirections — contradictory SOCD stress test
// Pattern (P1, 300 frames):
//   Alternating contradictory directions + buttons every few frames.
//   Tests SOCD cleaning, input system robustness, and deterministic
//   behavior under pathological input patterns.
// ============================================================================

void Scenario_MashDirections(SIR_Scenario* out) {
    ClearScenario(out);
    strncpy(out->name, "MashDirections", SIR_MAX_NAME - 1);
    out->totalFrames = 300;
    out->hasP2 = false;

    // Mash contradictory patterns in 10-frame blocks
    for (int i = 0; i < 30; i++) {
        int base = i * 10;
        uint16_t pattern;
        switch (i % 6) {
            case 0: pattern = INPUT_LEFT | INPUT_RIGHT;           break; // LR
            case 1: pattern = INPUT_UP | INPUT_DOWN;              break; // UD
            case 2: pattern = INPUT_LEFT | INPUT_RIGHT | INPUT_A; break; // LR+A
            case 3: pattern = INPUT_UP | INPUT_DOWN | INPUT_B;    break; // UD+B
            case 4: pattern = INPUT_LEFT | INPUT_UP | INPUT_C;    break; // LU+C
            case 5: pattern = INPUT_RIGHT | INPUT_DOWN | INPUT_D; break; // RD+D
            default: pattern = 0; break;
        }
        AddEntry(out, base, base + 4, 0, pattern);
        // base+5 to base+9: idle (release)
    }
}

// ============================================================================
// Scenario: WakeupDPLoop — 623A (DP motion) loop
// Pattern (P1, 360 frames):
//   Cycle ~60 frames:
//     0-19: idle (simulate knockdown recovery)
//     20-23: right
//     24-27: down
//     28-31: down-right + A (DP input)
//     32-59: idle (recovery)
// Loops 6 times
// ============================================================================

void Scenario_WakeupDPLoop(SIR_Scenario* out) {
    ClearScenario(out);
    strncpy(out->name, "WakeupDPLoop", SIR_MAX_NAME - 1);
    out->totalFrames = 360;
    out->hasP2 = false;

    int cycle = 60;
    for (int i = 0; i < 6; i++) {
        int base = i * cycle;
        // 0-19: idle (knockdown)
        AddEntry(out, base + 20, base + 23, 0, INPUT_RIGHT);
        AddEntry(out, base + 24, base + 27, 0, INPUT_DOWN);
        AddEntry(out, base + 28, base + 31, 0, INPUT_DOWN | INPUT_RIGHT | INPUT_A);
        // 32-59: idle (recovery)
    }
}

// ============================================================================
// Scenario: CpuStressStarter — rapid varied inputs for VS CPU testing
// Pattern (P1, 600 frames):
//   Alternates between movement, attacks, specials, and idle windows.
//   Covers many input states to stress-test CPU opponent reactions and
//   verify determinism across runs.
// ============================================================================

void Scenario_CpuStressStarter(SIR_Scenario* out) {
    ClearScenario(out);
    strncpy(out->name, "CpuStressStarter", SIR_MAX_NAME - 1);
    out->totalFrames = 600;
    out->hasP2 = false;

    // Phase 1: approach (0-59)
    AddEntry(out,   0,  29, 0, INPUT_RIGHT);
    AddEntry(out,  30,  39, 0, INPUT_RIGHT | INPUT_A);
    AddEntry(out,  40,  59, 0, INPUT_RIGHT);

    // Phase 2: pressure (60-149)
    AddEntry(out,  60,  64, 0, INPUT_A);
    AddEntry(out,  70,  74, 0, INPUT_B);
    AddEntry(out,  80,  84, 0, INPUT_DOWN | INPUT_A);
    AddEntry(out,  90,  99, 0, INPUT_DOWN | INPUT_B);
    AddEntry(out, 100, 109, 0, INPUT_RIGHT);
    AddEntry(out, 110, 114, 0, INPUT_C);
    AddEntry(out, 120, 129, 0, INPUT_DOWN | INPUT_C);
    AddEntry(out, 130, 149, 0, 0); // idle

    // Phase 3: specials (150-269)
    // QCF+A
    AddEntry(out, 150, 155, 0, INPUT_DOWN);
    AddEntry(out, 156, 161, 0, INPUT_DOWN | INPUT_RIGHT);
    AddEntry(out, 162, 167, 0, INPUT_RIGHT);
    AddEntry(out, 168, 171, 0, INPUT_RIGHT | INPUT_A);
    AddEntry(out, 172, 199, 0, 0); // recovery idle
    // QCF+B
    AddEntry(out, 200, 205, 0, INPUT_DOWN);
    AddEntry(out, 206, 211, 0, INPUT_DOWN | INPUT_RIGHT);
    AddEntry(out, 212, 217, 0, INPUT_RIGHT);
    AddEntry(out, 218, 221, 0, INPUT_RIGHT | INPUT_B);
    AddEntry(out, 222, 249, 0, 0); // recovery idle
    // DP+A
    AddEntry(out, 250, 253, 0, INPUT_RIGHT);
    AddEntry(out, 254, 257, 0, INPUT_DOWN);
    AddEntry(out, 258, 261, 0, INPUT_DOWN | INPUT_RIGHT | INPUT_A);
    AddEntry(out, 262, 269, 0, 0);

    // Phase 4: movement stress (270-389)
    AddEntry(out, 270, 279, 0, INPUT_LEFT);
    AddEntry(out, 280, 289, 0, INPUT_RIGHT);
    AddEntry(out, 290, 299, 0, INPUT_UP);
    AddEntry(out, 300, 304, 0, INPUT_A); // air attack
    AddEntry(out, 305, 329, 0, 0); // land
    AddEntry(out, 330, 349, 0, INPUT_LEFT);
    AddEntry(out, 350, 359, 0, INPUT_UP | INPUT_LEFT);
    AddEntry(out, 360, 364, 0, INPUT_B); // air attack
    AddEntry(out, 365, 389, 0, 0); // land

    // Phase 5: heavy pressure (390-509)
    AddEntry(out, 390, 399, 0, INPUT_RIGHT);
    AddEntry(out, 400, 404, 0, INPUT_A);
    AddEntry(out, 408, 412, 0, INPUT_A);
    AddEntry(out, 416, 420, 0, INPUT_B);
    AddEntry(out, 424, 428, 0, INPUT_C);
    AddEntry(out, 430, 449, 0, INPUT_DOWN | INPUT_RIGHT);
    AddEntry(out, 450, 453, 0, INPUT_RIGHT | INPUT_D);
    AddEntry(out, 454, 509, 0, 0); // recovery

    // Phase 6: final burst (510-599)
    for (int i = 0; i < 9; i++) {
        int base = 510 + i * 10;
        uint16_t btn;
        switch (i % 4) {
            case 0: btn = INPUT_A; break;
            case 1: btn = INPUT_B; break;
            case 2: btn = INPUT_C; break;
            case 3: btn = INPUT_D; break;
            default: btn = INPUT_A; break;
        }
        AddEntry(out, base, base + 3, 0, btn);
        // base+4 to base+9: idle
    }
}

// ============================================================================
// Registry
// ============================================================================

typedef void (*ScenarioBuildFn)(SIR_Scenario*);

static const struct {
    const char*    name;
    ScenarioBuildFn build;
} g_builtins[] = {
    { "Idle300",          Scenario_Idle300 },
    { "WalkJumpLight",    Scenario_WalkJumpLight },
    { "ProjectileLoop",   Scenario_ProjectileLoop },
    { "MashDirections",   Scenario_MashDirections },
    { "WakeupDPLoop",     Scenario_WakeupDPLoop },
    { "CpuStressStarter", Scenario_CpuStressStarter },
};

static const int g_builtinCount = (int)(sizeof(g_builtins) / sizeof(g_builtins[0]));

int Scenarios_GetBuiltinCount(void) {
    return g_builtinCount;
}

bool Scenarios_GetBuiltin(int index, SIR_Scenario* out) {
    if (index < 0 || index >= g_builtinCount || !out) return false;
    g_builtins[index].build(out);
    return true;
}

const char* Scenarios_GetBuiltinName(int index) {
    if (index < 0 || index >= g_builtinCount) return "???";
    return g_builtins[index].name;
}
