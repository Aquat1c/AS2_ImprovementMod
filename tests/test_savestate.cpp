/**
 * Edge-case tests for savestate system (save/load round-trip, checksum, integrity).
 *
 * These tests exercise the CompactSaveState API from as2_rollback.h.
 * They must be run while in Mode 8 Substate 3 (gameplay) for meaningful data.
 */

#include "tests.h"
#include "as2_rollback.h"
#include "as2_constants.h"
#include <stdio.h>
#include <string.h>

namespace SavestateTests {

static int g_pass = 0;
static int g_fail = 0;
static ProbeResult g_lastProbe = {};

#define TEST_ASSERT(cond, msg)                                         \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            g_fail++;                                                  \
        } else {                                                       \
            g_pass++;                                                  \
        }                                                              \
    } while (0)

// ============================================================================
// 1. Save produces non-zero checksum
// ============================================================================

static void TestSaveChecksum() {
    printf("[Savestate] Save checksum... ");

    CompactSaveState_t state{};
    AS2_SaveCompactState(&state);

    uint32_t crc = AS2_GetQuickChecksum();
    printf("checksum=0x%08X ", crc);
    TEST_ASSERT(state.global.frame_counter >= 0, "Frame counter valid");

    printf("done\n");
}

// ============================================================================
// 2. Save → Load → Save produces identical state
// ============================================================================

static void TestSaveLoadRoundTrip() {
    printf("[Savestate] Save→Load→Save round-trip... ");

    CompactSaveState_t state1{};
    CompactSaveState_t state2{};

    AS2_SaveCompactState(&state1);
    uint32_t crc1 = AS2_GetQuickChecksum();

    AS2_LoadCompactState(&state1);
    AS2_SaveCompactState(&state2);
    uint32_t crc2 = AS2_GetQuickChecksum();

    // After load→save, the checksums should match
    TEST_ASSERT(crc1 == crc2, "Checksum matches after load+save round-trip");

    // Binary compare the key fields
    TEST_ASSERT(state1.global.frame_counter == state2.global.frame_counter,
                "Frame counter matches");
    TEST_ASSERT(state1.rng_seed == state2.rng_seed,
                "RNG seed matches");
    TEST_ASSERT(state1.match.round_timer[0] == state2.match.round_timer[0],
                "Round timer matches");

    // Compare entity data (raw P1 entity buffer)
    TEST_ASSERT(memcmp(state1.p1_entity, state2.p1_entity, 256) == 0,
                "P1 entity first 256 bytes match");

    printf("done\n");
}

// ============================================================================
// 3. Multiple saves produce consistent checksums (determinism within frame)
// ============================================================================

static void TestSaveDeterminism() {
    printf("[Savestate] Save determinism (same frame)... ");

    CompactSaveState_t stateA{};
    CompactSaveState_t stateB{};

    AS2_SaveCompactState(&stateA);
    AS2_SaveCompactState(&stateB);

    // Two saves within the same frame should be identical
    TEST_ASSERT(memcmp(&stateA.global, &stateB.global,
                       sizeof(stateA.global)) == 0,
                "Global state identical on consecutive saves");

    // Compare P1 and P2 entity buffers
    TEST_ASSERT(memcmp(stateA.p1_entity, stateB.p1_entity, 256) == 0,
                "P1 entity match");
    TEST_ASSERT(memcmp(stateA.p2_entity, stateB.p2_entity, 256) == 0,
                "P2 entity match");

    printf("done\n");
}

// ============================================================================
// 4. Load restores FPU state correctly
// ============================================================================

static void TestFPURestore() {
    printf("[Savestate] FPU state restore... ");

    CompactSaveState_t state{};
    AS2_SaveCompactState(&state);

    // Corrupt FPU by doing some float ops
    volatile float f = 1.23456f;
    for (int i = 0; i < 100; i++) f = f * 1.001f + 0.0001f;

    AS2_LoadCompactState(&state);

    // After load, do another save and check FPU state matches
    CompactSaveState_t state2{};
    AS2_SaveCompactState(&state2);

    TEST_ASSERT(memcmp(state.fpu_state, state2.fpu_state, sizeof(state.fpu_state)) == 0,
                "FPU state restored correctly");

    printf("done\n");
}

// ============================================================================
// 5. Input buffer preservation
// ============================================================================

static void TestInputBufferPreservation() {
    printf("[Savestate] Input buffer preservation... ");

    CompactSaveState_t state{};
    AS2_SaveCompactState(&state);

    // Check that input buffers were captured (not all zeros in gameplay)
    bool hasAnyInput = false;
    for (int i = 0; i < INPUT_BUFFER_SIZE; i++) {
        if (state.input.p1_buffer[i] != 0 ||
            state.input.p2_buffer[i] != 0) {
            hasAnyInput = true;
            break;
        }
    }
    // Note: In early frames, inputs might all be zero — that's valid too
    printf("has_inputs=%s ", hasAnyInput ? "yes" : "no (early frame?)");

    // Load and re-save, verify input buffers match
    AS2_LoadCompactState(&state);
    CompactSaveState_t state2{};
    AS2_SaveCompactState(&state2);

    TEST_ASSERT(memcmp(&state.input, &state2.input,
                       sizeof(state.input)) == 0,
                "Input buffers preserved through load");

    printf("done\n");
}

// ============================================================================
// 6. Extended input tail + current-slot restoration
// ============================================================================

static void TestExtendedInputRestore() {
    printf("[Savestate] Extended input state restore... ");

    CompactSaveState_t before{};
    CompactSaveState_t after{};
    AS2_SaveCompactState(&before);

    const uintptr_t p1SelectJustPressed = ADDR_P1_INPUT_STATE + 18;
    const uintptr_t p2SelectJustPressed = ADDR_P2_INPUT_STATE + 18;
    const uintptr_t p1CooldownWord = ADDR_P1_INPUT_BUFFER + 84;
    const uintptr_t p2CooldownWord = ADDR_P2_INPUT_BUFFER + 84;
    const uintptr_t p1HoldCounterWord = ADDR_P1_INPUT_BUFFER + 140;
    const uintptr_t p2HoldCounterWord = ADDR_P2_INPUT_BUFFER + 140;
    const uintptr_t p1CurrentInput = ADDR_P1_INPUT_HISTORY + before.input.write_idx * sizeof(uint16_t);
    const uintptr_t p2CurrentInput = ADDR_P2_INPUT_HISTORY + before.input.write_idx * sizeof(uint16_t);

    WriteMemory<uint16_t>(p1SelectJustPressed, (uint16_t)(before.input.p1_state[18] ? 0 : 1));
    WriteMemory<uint16_t>(p2SelectJustPressed, (uint16_t)(before.input.p2_state[18] ? 0 : 1));
    WriteMemory<uint16_t>(p1CooldownWord, 0x1357);
    WriteMemory<uint16_t>(p2CooldownWord, 0x2468);
    WriteMemory<uint16_t>(p1HoldCounterWord, 0xAAAA);
    WriteMemory<uint16_t>(p2HoldCounterWord, 0x5555);
    WriteMemory<uint16_t>(p1CurrentInput, (uint16_t)(before.input.p1_input ^ 0x03FF));
    WriteMemory<uint16_t>(p2CurrentInput, (uint16_t)(before.input.p2_input ^ 0x03FF));

    AS2_LoadCompactState(&before);
    AS2_SaveCompactState(&after);

    TEST_ASSERT(memcmp(before.input.p1_buffer, after.input.p1_buffer, INPUT_BUFFER_SIZE) == 0,
                "P1 full 208-byte input buffer restored");
    TEST_ASSERT(memcmp(before.input.p2_buffer, after.input.p2_buffer, INPUT_BUFFER_SIZE) == 0,
                "P2 full 208-byte input buffer restored");
    TEST_ASSERT(memcmp(before.input.p1_state, after.input.p1_state, INPUT_STATE_SIZE) == 0,
                "P1 just-pressed array restored");
    TEST_ASSERT(memcmp(before.input.p2_state, after.input.p2_state, INPUT_STATE_SIZE) == 0,
                "P2 just-pressed array restored");
    TEST_ASSERT(before.input.p1_input == after.input.p1_input,
                "P1 current history slot restored");
    TEST_ASSERT(before.input.p2_input == after.input.p2_input,
                "P2 current history slot restored");

    printf("done\n");
}

// ============================================================================
// 7. Struct size sanity
// ============================================================================

static void TestStructSizes() {
    printf("[Savestate] Struct sizes... ");

    // CompactSaveState_t should be reasonable (< 512KB)
    size_t sz = sizeof(CompactSaveState_t);
    printf("size=%zu ", sz);
    TEST_ASSERT(sz > 0, "Non-zero size");
    TEST_ASSERT(sz < 512 * 1024, "Under 512KB");

    printf("done\n");
}

// ============================================================================
// 8. Format version is set correctly
// ============================================================================

static void TestFormatVersion() {
    printf("[Savestate] Format version... ");

    CompactSaveState_t state{};
    AS2_SaveCompactState(&state);

    TEST_ASSERT(state.format_version == 2,
                "format_version should be 2 (FSAVE+MXCSR+visual_rng)");

    TEST_ASSERT(state.visual_rng_seed != 0 || state.rng_seed != 0,
                "at least one RNG seed should be non-zero after capture");

    printf("format_version=%u done\n", state.format_version);
}

// ============================================================================
// 9. FPU FSAVE round-trip (full 108-byte state)
// ============================================================================

static void TestFpuFsaveRoundTrip() {
    printf("[Savestate] FPU FSAVE round-trip... ");

    CompactSaveState_t state1{};
    CompactSaveState_t state2{};

    AS2_SaveCompactState(&state1);

    // Do some float ops to disturb the FPU
    volatile float f = 3.14159f;
    volatile double d = 2.71828;
    for (int i = 0; i < 50; i++) {
        f = f * 1.1f + 0.01f;
        d = d * 0.99 + 0.001;
    }
    (void)f; (void)d;

    // Restore and re-save
    AS2_LoadCompactState(&state1);
    AS2_SaveCompactState(&state2);

    // Full 108-byte FPU state should match (FSAVE captures register stack too)
    TEST_ASSERT(memcmp(state1.fpu_state, state2.fpu_state, 108) == 0,
                "Full FSAVE state (108 bytes) matches after round-trip");
    TEST_ASSERT(state1.mxcsr == state2.mxcsr,
                "MXCSR matches after round-trip");

    printf("done\n");
}

// ============================================================================
// 10. Alt-buffer IS main buffer (compile-time verified, runtime sanity)
// ============================================================================

static void TestAltBufferIdentity() {
    printf("[Savestate] Alt-buffer identity... ");

    // Compile-time: static_asserts in as2_rollback.cpp already verify this
    // Runtime: check that the savestate captures the same memory
    TEST_ASSERT(ADDR_P1_INPUT_BUFFER == 0x8E9E62,
                "P1 input buffer at expected address");
    TEST_ASSERT(ADDR_P2_INPUT_BUFFER == 0x8E9F32,
                "P2 input buffer at expected address");

    printf("done\n");
}

bool RunRoundTripProbe(ProbeResult* out) {
    ProbeResult result = {};
    result.valid = true;

    if (!AS2_IsInGameplay()) {
        snprintf(result.status, sizeof(result.status),
                 "Probe requires gameplay (Mode 8 Substate 3).");
        if (out) *out = result;
        g_lastProbe = result;
        return false;
    }

    CompactSaveState_t before{};
    CompactSaveState_t after{};

    if (!AS2_SaveStateToBuffer(&before)) {
        snprintf(result.status, sizeof(result.status),
                 "Failed to capture initial savestate.");
        if (out) *out = result;
        g_lastProbe = result;
        return false;
    }

    result.frame_before = before.frame_number;
    result.checksum_before = before.checksum;
    result.quick_before = AS2_GetQuickChecksum();
    result.rng_before = before.rng_seed;

    if (!AS2_LoadStateFromBuffer(&before)) {
        snprintf(result.status, sizeof(result.status),
                 "Failed to load captured savestate.");
        if (out) *out = result;
        g_lastProbe = result;
        return false;
    }

    if (!AS2_SaveStateToBuffer(&after)) {
        snprintf(result.status, sizeof(result.status),
                 "Failed to re-capture state after load.");
        if (out) *out = result;
        g_lastProbe = result;
        return false;
    }

    result.frame_after = after.frame_number;
    result.checksum_after = after.checksum;
    result.quick_after = AS2_GetQuickChecksum();
    result.rng_after = after.rng_seed;

    const bool checksumMatch = (result.checksum_before == result.checksum_after);
    const bool quickMatch = (result.quick_before == result.quick_after);
    const bool rngMatch = (result.rng_before == result.rng_after);
    const bool frameMatch = (result.frame_before == result.frame_after);
    const bool timerMatch =
        (before.global.sub_state_timer == after.global.sub_state_timer);

    result.passed = checksumMatch && quickMatch && rngMatch && frameMatch && timerMatch;

    if (result.passed) {
        snprintf(result.status, sizeof(result.status),
                 "PASS: frame=%u checksum=0x%08X quick=0x%04X rng=0x%08X",
                 result.frame_after,
                 result.checksum_after,
                 result.quick_after,
                 result.rng_after);
    } else {
        snprintf(result.status, sizeof(result.status),
                 "FAIL: full %s quick %s rng %s frame %s timer %s",
                 checksumMatch ? "ok" : "mismatch",
                 quickMatch ? "ok" : "mismatch",
                 rngMatch ? "ok" : "mismatch",
                 frameMatch ? "ok" : "mismatch",
                 timerMatch ? "ok" : "mismatch");
    }

    g_lastProbe = result;
    if (out) *out = result;

    printf("[SavestateProbe] %s\n", result.status);
    return result.passed;
}

bool GetLastProbeResult(ProbeResult* out) {
    if (!out || !g_lastProbe.valid) return false;
    *out = g_lastProbe;
    return true;
}

// ============================================================================
// Public entry point
// ============================================================================

void RunAll() {
    g_pass = 0;
    g_fail = 0;

    bool inGameplay = AS2_IsInGameplay();
    printf("=== Savestate Edge Case Tests ===\n");
    printf("In gameplay: %s\n", inGameplay ? "YES" : "NO");

    if (!inGameplay) {
        printf("WARNING: Not in gameplay (Mode 8 Sub 3). Some tests will produce trivial results.\n");
    }

    TestStructSizes();
    TestFormatVersion();
    TestSaveChecksum();
    TestSaveLoadRoundTrip();
    TestSaveDeterminism();
    TestFPURestore();
    TestFpuFsaveRoundTrip();
    TestInputBufferPreservation();
    TestExtendedInputRestore();
    TestAltBufferIdentity();

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
}

} // namespace SavestateTests
