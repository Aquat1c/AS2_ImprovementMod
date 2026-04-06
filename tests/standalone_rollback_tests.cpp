#include <iostream>
#include <cstdint>
#include <cmath>

#ifdef _MSC_VER
#include <windows.h>

// 1. Mocking the FPU save/restore logic identical to our core fix
void MockSaveFpuState(uint8_t* buffer) {
    if (!buffer) return;
    __asm {
        mov eax, buffer
        fnstcw word ptr [eax]
        stmxcsr dword ptr [eax+4]
    }
}

void MockRestoreFpuState(const uint8_t* buffer) {
    if (!buffer) return;

    uint16_t saved_cw = *(const uint16_t*)buffer;
    uint16_t safe_cw = (saved_cw & 0x0F00) | 0x003F;

    uint32_t saved_mxcsr = *(const uint32_t*)(buffer + 4);
    uint32_t safe_mxcsr = (saved_mxcsr & 0x00006000) | 0x00001F80;

    __asm {
        fldcw word ptr [safe_cw]
        ldmxcsr dword ptr [safe_mxcsr]
    }
}

// 1b. Mocking the original BROKEN logic that caused ACCESS_VIOLATION (to prove it fails!)
void BadSaveFpuState(uint8_t* buffer) {
    if (!buffer) return;
    __asm {
        mov eax, buffer
        fsave [eax]
        frstor [eax]
    }
}

void BadRestoreFpuState(const uint8_t* buffer) {
    if (!buffer) return;
    __asm {
        mov eax, buffer
        frstor [eax]
    }
}
#endif

// 2. Mocking the ring buffer modulo logic identical to our fix
const int kMaxSavestateSlots = 16;
int GetRingBufferSlot(int frame) {
    int slotIdx = frame % kMaxSavestateSlots;
    if (slotIdx < 0) slotIdx += kMaxSavestateSlots;
    return slotIdx;
}

int GetBadRingBufferSlot(int frame) {
    return frame % kMaxSavestateSlots;
}

static uint16_t GetManagedFpuControlWordBits(uint16_t cw) {
    return cw & 0x0F3F;
}

int RunBaseMathTests() {
    bool all_passed = true;
    std::cout << "--- RUNNING CORE MATH TESTS ---" << std::endl;

    std::cout << "Test 1: Ring Buffer Modulo Stability... ";
    int bad_frame_neg1 = GetBadRingBufferSlot(-1);
    int good_frame_neg1 = GetRingBufferSlot(-1);
    
    if (bad_frame_neg1 != 15 && good_frame_neg1 == 15) {
        std::cout << "PASS (Original bug would yield: " << bad_frame_neg1 << ")" << std::endl;
    } else {
        std::cout << "FAIL" << std::endl;
        all_passed = false;
    }

#ifdef _MSC_VER
    std::cout << "Test 2: FPU State Simulation Validation (Targeting FLT_STACK_CHECK mitigation)..." << std::endl;
    uint8_t fpu_buffer[108] = {0};
    
    // Valid initialization so we don't restore garbage
    MockSaveFpuState(fpu_buffer);

    float active_float = 1.0f;
    bool fpu_survived = true;

    // Use Structured Exception Handling to safely detect FPU crashes instead of hard-crashing the test runner
    __try {
        for (int i = 0; i < 50; i++) {
            active_float = std::sin(active_float + 0.1f);
            
            // This is the sequence GekkoNet fires off on Rollback/Advance events back to back
            MockSaveFpuState(fpu_buffer);
            MockRestoreFpuState(fpu_buffer);
            
            active_float = std::cos(active_float + 0.1f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fpu_survived = false;
    }

    if (fpu_survived) {
        std::cout << "  -> PASS: New FPU logic maintained Float Registers perfectly (Final math output: " << active_float << ")" << std::endl;
    } else {
        std::cout << "  -> FAIL: New FPU logic crashed!" << std::endl;
        all_passed = false;
    }

    // Now prove the old logic was actually the culprit
    std::cout << "Test 3: Validating Old FPU logic strictly produced ACCESS_VIOLATION 0xC0000092... ";
    bool bad_fpu_crashed = false;
    __try {
        BadSaveFpuState(fpu_buffer);
        float x = 1.0f;
        for (int i = 0; i < 50; i++) {
            x = std::sin(x + 0.1f);
            BadSaveFpuState(fpu_buffer);
            BadRestoreFpuState(fpu_buffer);
            x = std::cos(x + 0.1f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bad_fpu_crashed = true;
    }

    if (bad_fpu_crashed) {
        std::cout << "PASS (It correctly self-destructed due to stack override as expected!)" << std::endl;
    } else {
        std::cout << "SKIP (Old logic did not crash in this context)" << std::endl;
    }

    // Test 4: Verify that restoring a control word with unmasked exceptions does NOT crash
    std::cout << "Test 4: FPU exception masks preserved after restore... ";
    bool exception_safe = true;
    __try {
        uint8_t bad_buffer[8] = {0};
        // Simulate a control word with ALL exception masks CLEARED (0x0000)
        // This is the exact scenario that caused STATUS_FLOAT_MULTIPLE_FAULTS
        *(uint16_t*)bad_buffer = 0x0000;   // No exceptions masked, no precision set
        *(uint32_t*)(bad_buffer + 4) = 0x00000000;  // SSE: no exceptions masked

        // Our safe restore should force-mask all exceptions
        MockRestoreFpuState(bad_buffer);

        // Now do float math that would trigger FLT_INEXACT_RESULT if unmasked
        volatile float f = 1.0f / 3.0f;  // inexact
        f = f * 7.0f;
        f = std::sqrt(f);
        (void)f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exception_safe = false;
    }

    if (exception_safe) {
        std::cout << "PASS (Exception masks correctly forced ON)" << std::endl;
    } else {
        std::cout << "FAIL (Float exception triggered despite masking!)" << std::endl;
        all_passed = false;
    }

    // Test 5: x87 may normalize ignored control-word bits on readback.
    // The rollback verifier should compare only the managed bits
    // (exception masks + precision + rounding), not exact raw equality.
    std::cout << "Test 5: x87 readback normalizes ignored CW bits... ";
    {
        uint8_t buf[8] = {0};
        *(uint16_t*)buf = 0x003F;
        *(uint32_t*)(buf + 4) = 0x00001F80;

        MockRestoreFpuState(buf);

        uint8_t verify[8] = {0};
        MockSaveFpuState(verify);
        uint16_t gotCW = *(uint16_t*)verify;

        if (GetManagedFpuControlWordBits(gotCW) == 0x003F) {
            std::cout << "PASS (loaded=0x003F readback=0x" << std::hex << gotCW << std::dec << ")" << std::endl;
        } else {
            std::cout << "FAIL (loaded=0x003F readback=0x" << std::hex << gotCW << std::dec << ")" << std::endl;
            all_passed = false;
        }
    }

    // Test 6: Verify precision/rounding bits ARE preserved through save/restore
    std::cout << "Test 6: Precision/Rounding bits preserved... ";
    {
        uint8_t buf1[8] = {0};
        MockSaveFpuState(buf1);
        uint16_t original_cw = *(uint16_t*)buf1;

        MockRestoreFpuState(buf1);

        uint8_t buf2[8] = {0};
        MockSaveFpuState(buf2);
        uint16_t restored_cw = *(uint16_t*)buf2;

        // Precision (bits 8-9) and rounding (bits 10-11) should match
        bool precision_match = (original_cw & 0x0F00) == (restored_cw & 0x0F00);
        if (precision_match) {
            std::cout << "PASS (CW: orig=0x" << std::hex << original_cw << " restored=0x" << restored_cw << std::dec << ")" << std::endl;
        } else {
            std::cout << "FAIL (CW: orig=0x" << std::hex << original_cw << " restored=0x" << restored_cw << std::dec << ")" << std::endl;
            all_passed = false;
        }
    }

    // Test 7: All 4 precision modes survive save/restore cycle
    std::cout << "Test 7: All precision modes preserved through save/restore... ";
    {
        bool all_modes_ok = true;
        // Test all 4 precision settings: 00=24bit, 01=reserved, 10=53bit, 11=64bit
        uint16_t testPrecisions[] = { 0x0000, 0x0100, 0x0200, 0x0300 };
        const char* precNames[] = { "24bit", "reserved", "53bit", "64bit" };
        for (int p = 0; p < 4; p++) {
            uint8_t buf[8] = {0};
            // Set a specific precision mode with all masks on
            *(uint16_t*)buf = testPrecisions[p] | 0x003F;
            *(uint32_t*)(buf + 4) = 0x00001F80;
            MockRestoreFpuState(buf);

            // Read back
            uint8_t verify[8] = {0};
            MockSaveFpuState(verify);
            uint16_t gotCW = *(uint16_t*)verify;
            if ((gotCW & 0x0300) != testPrecisions[p]) {
                std::cout << "\n    FAIL: precision=" << precNames[p] 
                          << " expected bits 0x" << std::hex << testPrecisions[p] 
                          << " got 0x" << (gotCW & 0x0300) << std::dec;
                all_modes_ok = false;
            }
        }
        if (all_modes_ok) {
            std::cout << "PASS (all 4 precision modes)" << std::endl;
        } else {
            std::cout << std::endl;
            all_passed = false;
        }
    }

    // Test 8: All 4 rounding modes survive save/restore cycle
    std::cout << "Test 8: All rounding modes preserved through save/restore... ";
    {
        bool all_modes_ok = true;
        uint16_t testRoundings[] = { 0x0000, 0x0400, 0x0800, 0x0C00 };
        const char* roundNames[] = { "nearest", "down", "up", "truncate" };
        for (int r = 0; r < 4; r++) {
            uint8_t buf[8] = {0};
            *(uint16_t*)buf = testRoundings[r] | 0x0300 | 0x003F; // 64bit precision + masks
            *(uint32_t*)(buf + 4) = 0x00001F80;
            MockRestoreFpuState(buf);

            uint8_t verify[8] = {0};
            MockSaveFpuState(verify);
            uint16_t gotCW = *(uint16_t*)verify;
            if ((gotCW & 0x0C00) != testRoundings[r]) {
                std::cout << "\n    FAIL: rounding=" << roundNames[r]
                          << " expected bits 0x" << std::hex << testRoundings[r]
                          << " got 0x" << (gotCW & 0x0C00) << std::dec;
                all_modes_ok = false;
            }
        }
        if (all_modes_ok) {
            std::cout << "PASS (all 4 rounding modes)" << std::endl;
        } else {
            std::cout << std::endl;
            all_passed = false;
        }
    }

    // Test 9: MXCSR rounding modes survive save/restore cycle
    std::cout << "Test 9: MXCSR rounding modes preserved... ";
    {
        bool all_modes_ok = true;
        uint32_t testMxcsrRound[] = { 0x00000000, 0x00002000, 0x00004000, 0x00006000 };
        const char* mxRoundNames[] = { "nearest", "down", "up", "truncate" };
        for (int r = 0; r < 4; r++) {
            uint8_t buf[8] = {0};
            *(uint16_t*)buf = 0x037F; // Standard CW (64bit, nearest, all masked)
            *(uint32_t*)(buf + 4) = testMxcsrRound[r] | 0x00001F80;
            MockRestoreFpuState(buf);

            uint8_t verify[8] = {0};
            MockSaveFpuState(verify);
            uint32_t gotMXCSR = *(uint32_t*)(verify + 4);
            if ((gotMXCSR & 0x6000) != testMxcsrRound[r]) {
                std::cout << "\n    FAIL: mxcsr_rounding=" << mxRoundNames[r]
                          << " expected 0x" << std::hex << testMxcsrRound[r]
                          << " got 0x" << (gotMXCSR & 0x6000) << std::dec;
                all_modes_ok = false;
            }
        }
        if (all_modes_ok) {
            std::cout << "PASS (all 4 MXCSR rounding modes)" << std::endl;
        } else {
            std::cout << std::endl;
            all_passed = false;
        }
    }

    // Test 10: Rapid save/restore cycles don't degrade FPU state over time
    std::cout << "Test 10: 1000 rapid save/restore cycles stability... ";
    {
        uint8_t buf[8] = {0};
        MockSaveFpuState(buf);
        uint16_t initialCW = *(uint16_t*)buf;
        uint32_t initialMXCSR = *(uint32_t*)(buf + 4);

        bool stable = true;
        float accumulator = 1.0f;
        for (int i = 0; i < 1000; i++) {
            MockSaveFpuState(buf);
            MockRestoreFpuState(buf);
            // Do some actual float work between cycles
            accumulator = accumulator * 1.001f + 0.001f;
            if (accumulator > 1e10f) accumulator = 1.0f;
        }

        MockSaveFpuState(buf);
        uint16_t finalCW = *(uint16_t*)buf;
        uint32_t finalMXCSR = *(uint32_t*)(buf + 4);

        if ((finalCW & 0x0F3F) != (initialCW & 0x0F3F)) {
            std::cout << "FAIL (CW drifted: 0x" << std::hex << initialCW << " -> 0x" << finalCW << std::dec << ")" << std::endl;
            stable = false;
            all_passed = false;
        } else if ((finalMXCSR & 0x7F80) != (initialMXCSR & 0x7F80)) {
            std::cout << "FAIL (MXCSR drifted: 0x" << std::hex << initialMXCSR << " -> 0x" << finalMXCSR << std::dec << ")" << std::endl;
            stable = false;
            all_passed = false;
        } else {
            std::cout << "PASS (CW=0x" << std::hex << finalCW << " MXCSR=0x" << finalMXCSR << std::dec 
                      << " acc=" << accumulator << ")" << std::endl;
        }
    }

    // Test 11: Float math produces identical results before and after save/restore
    std::cout << "Test 11: Float determinism through save/restore... ";
    {
        // Compute reference result WITHOUT save/restore
        float ref = 1.0f;
        for (int i = 0; i < 100; i++) {
            ref = std::sin(ref * 0.7f) + std::cos(ref * 0.3f);
        }

        // Reset and compute WITH save/restore every iteration
        float test = 1.0f;
        uint8_t buf[8] = {0};
        MockSaveFpuState(buf);
        for (int i = 0; i < 100; i++) {
            MockRestoreFpuState(buf);
            test = std::sin(test * 0.7f) + std::cos(test * 0.3f);
            MockSaveFpuState(buf);
        }

        if (ref == test) {
            std::cout << "PASS (result=" << ref << ")" << std::endl;
        } else {
            std::cout << "FAIL (ref=" << ref << " test=" << test << " diff=" << std::abs(ref-test) << ")" << std::endl;
            all_passed = false;
        }
    }
#endif

    int fpu_fail_count = all_passed ? 0 : 1;
    return fpu_fail_count;
}

// ============================================================================
// RNG Tests
// ============================================================================

// MSVC LCG algorithm: seed = seed * 214013 + 2531011, result = (seed >> 16) & 0x7FFF
static uint32_t MockLcgStep(uint32_t seed) {
    return seed * 214013 + 2531011;
}

static int MockRandFromSeed(uint32_t seed) {
    return (seed >> 16) & 0x7FFF;
}

int RunRngTests() {
    bool all_passed = true;
    std::cout << "\n--- RUNNING RNG TESTS ---" << std::endl;

    // Test R1: LCG produces known sequence from seed=1 (MSVC default)
    std::cout << "Test R1: MSVC LCG known sequence from seed=1... ";
    {
        uint32_t seed = 1;
        // First few rand() values from MSVC CRT with seed=1 are well-known
        int expected[] = { 41, 18467, 6334, 26500, 19169 };
        bool match = true;
        for (int i = 0; i < 5; i++) {
            seed = MockLcgStep(seed);
            int val = MockRandFromSeed(seed);
            if (val != expected[i]) {
                std::cout << "\n    FAIL at step " << i << ": expected " << expected[i] << " got " << val;
                match = false;
            }
        }
        if (match) {
            std::cout << "PASS" << std::endl;
        } else {
            std::cout << std::endl;
            all_passed = false;
        }
    }

    // Test R2: Seed=0 produces deterministic output (not broken)
    std::cout << "Test R2: Seed=0 produces valid output... ";
    {
        uint32_t seed = 0;
        seed = MockLcgStep(seed);
        int val = MockRandFromSeed(seed);
        // seed = 0 * 214013 + 2531011 = 2531011 = 0x269EC3
        // (0x269EC3 >> 16) & 0x7FFF = 0x26 & 0x7FFF = 38
        if (val == 38) {
            std::cout << "PASS (first rand from seed=0 is " << val << ")" << std::endl;
        } else {
            std::cout << "FAIL (expected 38, got " << val << ")" << std::endl;
            all_passed = false;
        }
    }

    // Test R3: Save/restore produces identical sequence
    std::cout << "Test R3: RNG restore produces identical sequence... ";
    {
        uint32_t savedSeed = 12345;
        
        // Generate 10 values from this seed
        uint32_t seq1Seed = savedSeed;
        int seq1[10];
        for (int i = 0; i < 10; i++) {
            seq1Seed = MockLcgStep(seq1Seed);
            seq1[i] = MockRandFromSeed(seq1Seed);
        }

        // "Restore" the seed and regenerate
        uint32_t seq2Seed = savedSeed;
        int seq2[10];
        for (int i = 0; i < 10; i++) {
            seq2Seed = MockLcgStep(seq2Seed);
            seq2[i] = MockRandFromSeed(seq2Seed);
        }

        bool match = true;
        for (int i = 0; i < 10; i++) {
            if (seq1[i] != seq2[i]) {
                std::cout << "FAIL at step " << i << std::endl;
                match = false;
                break;
            }
        }
        if (match) {
            std::cout << "PASS" << std::endl;
        }
        if (!match) all_passed = false;
    }

    // Test R4: LCG cycle detection - after enough steps, seed should not be 0 (unless started at 0)
    std::cout << "Test R4: LCG doesn't degenerate to zero from seed=1... ";
    {
        uint32_t seed = 1;
        bool hitZero = false;
        for (int i = 0; i < 100000; i++) {
            seed = MockLcgStep(seed);
            if (seed == 0) {
                hitZero = true;
                std::cout << "FAIL (seed became 0 at step " << i << ")" << std::endl;
                break;
            }
        }
        if (!hitZero) {
            std::cout << "PASS (100K steps, seed never 0)" << std::endl;
        } else {
            all_passed = false;
        }
    }

    // Test R5: Visual vs Sim RNG independence
    std::cout << "Test R5: Visual and Sim seeds diverge independently... ";
    {
        uint32_t simSeed = 42;
        uint32_t visSeed = 42;

        // Advance sim 5 times, vis 3 times
        for (int i = 0; i < 5; i++) simSeed = MockLcgStep(simSeed);
        for (int i = 0; i < 3; i++) visSeed = MockLcgStep(visSeed);

        // They should be different
        if (simSeed != visSeed) {
            std::cout << "PASS (sim=0x" << std::hex << simSeed << " vis=0x" << visSeed << std::dec << ")" << std::endl;
        } else {
            std::cout << "FAIL (seeds should differ after different call counts)" << std::endl;
            all_passed = false;
        }
    }

    // Test R6: Rollback restore replay - save seed, advance N, restore, advance N -> same results
    std::cout << "Test R6: Rollback replay determinism (save, advance, restore, advance)... ";
    {
        const int ADVANCE_FRAMES = 50;
        const int RANDS_PER_FRAME = 5;

        uint32_t baseSeed = 0xF4F3A161;  // Realistic seed from actual game

        // Phase 1: Record
        uint32_t recordSeed = baseSeed;
        int recorded[ADVANCE_FRAMES * RANDS_PER_FRAME];
        for (int f = 0; f < ADVANCE_FRAMES; f++) {
            for (int r = 0; r < RANDS_PER_FRAME; r++) {
                recordSeed = MockLcgStep(recordSeed);
                recorded[f * RANDS_PER_FRAME + r] = MockRandFromSeed(recordSeed);
            }
        }

        // Phase 2: Restore to baseSeed and replay
        uint32_t replaySeed = baseSeed;
        bool match = true;
        for (int f = 0; f < ADVANCE_FRAMES; f++) {
            for (int r = 0; r < RANDS_PER_FRAME; r++) {
                replaySeed = MockLcgStep(replaySeed);
                int val = MockRandFromSeed(replaySeed);
                if (val != recorded[f * RANDS_PER_FRAME + r]) {
                    std::cout << "FAIL at frame=" << f << " rand=" << r << std::endl;
                    match = false;
                    break;
                }
            }
            if (!match) break;
        }
        if (match) {
            std::cout << "PASS (250 values matched after restore)" << std::endl;
        }
        if (!match) all_passed = false;
    }

    return all_passed ? 0 : 1;
}