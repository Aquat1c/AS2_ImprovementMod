/**
 * Alice Senki 2 - Manual Savestate System
 *
 * Conservative broad-capture approach:
 *   - Full contiguous region from ADDR_MATCH_BASE through P2 entity end
 *   - Scattered globals: RNG seed, frame counter, mode/substate, input buffers,
 *     per-frame temp scratch, match phase timer, pre-match gap
 *
 * This is NOT a rollback savestate yet. Manual F5/F6 only.
 *
 * Verified state region (from determinism system):
 *   Start: ADDR_MATCH_BASE       = 0x76C5F8
 *   End:   ADDR_P2_ENTITY_BASE + ENTITY_SIZE = 0x7AB880
 *   Size:  0x3F288 = 259,720 bytes (~254 KB)
 *
 * This region contains:
 *   - Match header (16 bytes)
 *   - Match context: camera scroll, screen shake, weather particles (7456 bytes)
 *   - Effect array: 200 slots × 32 bytes (6400 bytes)
 *   - Summon array: 100 slots × 272 bytes (27200 bytes)
 *   - P1 entity: 108,812 bytes (HP, meter, position, action, animation,
 *     combo, hitstun, knockdown, recovery, guard, state blocks A/B, combat)
 *   - P2 entity: 108,812 bytes (same)
 */

#include "rollback/savestate.h"
#include "rollback/determinism_verify.h"
#include "training/practice_tools.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "log_window.h"
#include "imgui.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>

// ============================================================================
// State Region Definitions
// ============================================================================

// Main contiguous state: match base through end of P2 entity
#define SS_MAIN_START       ADDR_MATCH_BASE
#define SS_MAIN_SIZE        ((ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE)

// Pre-match gap (effect index, audio channel index, render blend)
// 12 bytes at 0x76C5EC immediately before match base
#define SS_PRE_MATCH_START  ADDR_PRE_MATCH_GAP
#define SS_PRE_MATCH_SIZE   PRE_MATCH_GAP_SIZE

// Per-frame temp scratch (collision/hit temp data at match+0x700)
// Already inside the main region, but we explicitly clear it on load
// to prevent stale one-frame transient data from bleeding.

// Input buffers (P1 and P2, 208 bytes each)
#define SS_INPUT_P1_START   ADDR_P1_INPUT_BUFFER
#define SS_INPUT_P2_START   ADDR_P2_INPUT_BUFFER
#define SS_INPUT_SIZE       INPUT_BUFFER_SIZE

// ============================================================================
// Savestate Storage
// ============================================================================

struct SavestateSlot {
    // Metadata
    SavestateInfo info;

    // Main contiguous state blob
    uint8_t main_state[SS_MAIN_SIZE];

    // Scattered globals
    uint8_t pre_match_gap[SS_PRE_MATCH_SIZE];
    uint32_t rng_seed;
    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t game_mode;
    uint32_t substate;
    uint32_t substate_timer;
    uint32_t game_type;
    uint32_t match_phase_timer;

    // Input buffers
    uint8_t input_p1[SS_INPUT_SIZE];
    uint8_t input_p2[SS_INPUT_SIZE];

    // Input read/write indices
    uint32_t input_read_idx;
    uint32_t input_write_idx;

    // FPU state — captured for diagnostics but NOT restored by default.
    // Uncomment the restore lines in Savestate_Load if desync evidence
    // points to FPU drift being the cause.
    uint16_t fpu_cw;
    uint32_t fpu_mxcsr;
};

static SavestateSlot g_slot;

// ============================================================================
// File Logging (integrates with determinism log directory)
// ============================================================================

static char  g_logDir[MAX_PATH] = {0};
static FILE* g_logFile = nullptr;

void Savestate_SetLogDir(const char* dir) {
    if (dir) {
        strncpy(g_logDir, dir, MAX_PATH - 1);
        g_logDir[MAX_PATH - 1] = '\0';
    }
}

static void EnsureLogFile() {
    if (g_logFile) return;

    char path[MAX_PATH];
    if (g_logDir[0]) {
        CreateDirectoryA(g_logDir, NULL);
        snprintf(path, sizeof(path), "%s\\savestate_log.txt", g_logDir);
    } else {
        CreateDirectoryA("logs", NULL);
        snprintf(path, sizeof(path), "logs\\savestate_log.txt");
    }

    g_logFile = fopen(path, "w");
    if (g_logFile) {
        fprintf(g_logFile, "# Alice Senki 2 - Savestate Log\n");
        fprintf(g_logFile, "# Build: %s %s\n", __DATE__, __TIME__);
        fprintf(g_logFile, "# Main region: 0x%08X size=%u (%u KB)\n",
                SS_MAIN_START, (unsigned)SS_MAIN_SIZE, (unsigned)(SS_MAIN_SIZE / 1024));
        fprintf(g_logFile, "#\n");
        fflush(g_logFile);
    }
}

static void LogToFile(const char* fmt, ...) {
    EnsureLogFile();
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fflush(g_logFile);
}

// ============================================================================
// FPU Capture (diagnostic only)
// ============================================================================

static inline uint16_t CaptureX87CW() {
    uint16_t cw = 0;
    __asm { fnstcw word ptr [cw] }
    return cw;
}

static inline uint32_t CaptureMXCSR() {
    return _mm_getcsr();
}

// ============================================================================
// State Region Checksum
// ============================================================================

static uint32_t ComputeMainChecksum() {
    __try {
        return CalcCRC32((const void*)SS_MAIN_START, SS_MAIN_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADDEAD;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void Savestate_Init() {
    memset(&g_slot, 0, sizeof(g_slot));
    g_slot.info.valid = false;
    LOG_INFO("[Savestate] Initialized (main region: 0x%08X, %u bytes / %u KB)",
             SS_MAIN_START, (unsigned)SS_MAIN_SIZE, (unsigned)(SS_MAIN_SIZE / 1024));
}

void Savestate_Shutdown() {
    if (g_logFile) {
        fprintf(g_logFile, "# Shutdown\n");
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

// ============================================================================
// Safety Check
// ============================================================================

bool Savestate_CanSaveLoad() {
    uint32_t mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    uint32_t sub  = ReadMemory<uint32_t>(ADDR_SUB_STATE);

    // Only allow during active gameplay (MODE_MATCH, substate GAMEPLAY)
    // Also allow during INIT (substate 2) for early-frame testing
    if (mode != MODE_MATCH) return false;
    if (sub != MATCH_SUB_GAMEPLAY && sub != MATCH_SUB_INIT) return false;

    return true;
}

// ============================================================================
// SAVE
// ============================================================================

bool Savestate_Save() {
    if (!Savestate_CanSaveLoad()) {
        LOG_WARN("[Savestate] Cannot save — not in active gameplay (mode=%d sub=%d)",
                 ReadMemory<uint32_t>(ADDR_GAME_MODE), ReadMemory<uint32_t>(ADDR_SUB_STATE));
        return false;
    }

    // Capture metadata
    g_slot.sim_frame       = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    g_slot.display_frame   = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    g_slot.game_mode       = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    g_slot.substate        = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    g_slot.substate_timer  = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    g_slot.game_type       = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    g_slot.match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);

    // Capture RNG seed via TLS
    g_slot.rng_seed = DetVer_GetRngSeed();

    // Capture FPU state (diagnostic only — NOT restored on load)
    g_slot.fpu_cw    = CaptureX87CW();
    g_slot.fpu_mxcsr = CaptureMXCSR();

    // Capture main contiguous region
    __try {
        memcpy(g_slot.main_state, (const void*)SS_MAIN_START, SS_MAIN_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[Savestate] FAILED — access violation reading main region at 0x%08X", SS_MAIN_START);
        return false;
    }

    // Capture pre-match gap
    __try {
        memcpy(g_slot.pre_match_gap, (const void*)SS_PRE_MATCH_START, SS_PRE_MATCH_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[Savestate] Failed to capture pre-match gap (non-critical)");
        memset(g_slot.pre_match_gap, 0, SS_PRE_MATCH_SIZE);
    }

    // Capture input buffers
    __try {
        memcpy(g_slot.input_p1, (const void*)SS_INPUT_P1_START, SS_INPUT_SIZE);
        memcpy(g_slot.input_p2, (const void*)SS_INPUT_P2_START, SS_INPUT_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[Savestate] Failed to capture input buffers (non-critical)");
    }

    // Capture input indices
    g_slot.input_read_idx  = ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX);
    g_slot.input_write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);

    // Compute checksum for verification
    uint32_t checksum = CalcCRC32(g_slot.main_state, SS_MAIN_SIZE);

    // Fill info
    g_slot.info.valid     = true;
    g_slot.info.frame     = g_slot.sim_frame;
    g_slot.info.checksum  = checksum;
    g_slot.info.rng_seed  = g_slot.rng_seed;
    g_slot.info.game_mode = g_slot.game_mode;
    g_slot.info.substate  = g_slot.substate;

    LOG_INFO("[Savestate] SAVED at frame %d — checksum=0x%08X rng=0x%08X mode=%d sub=%d",
             g_slot.info.frame, g_slot.info.checksum, g_slot.info.rng_seed,
             g_slot.info.game_mode, g_slot.info.substate);

    // Log to file for determinism analysis
    LogToFile("SAVE frame=%d checksum=0x%08X rng=0x%08X mode=%d sub=%d timer=%d fpu_cw=0x%04X mxcsr=0x%08X\n",
              g_slot.info.frame, g_slot.info.checksum, g_slot.info.rng_seed,
              g_slot.info.game_mode, g_slot.info.substate, g_slot.match_phase_timer,
              g_slot.fpu_cw, g_slot.fpu_mxcsr);

    return true;
}

// ============================================================================
// LOAD
// ============================================================================

bool Savestate_Load() {
    if (!g_slot.info.valid) {
        LOG_WARN("[Savestate] Cannot load — no savestate exists. Press F5 first.");
        return false;
    }

    if (!Savestate_CanSaveLoad()) {
        LOG_WARN("[Savestate] Cannot load — not in active gameplay (mode=%d sub=%d)",
                 ReadMemory<uint32_t>(ADDR_GAME_MODE), ReadMemory<uint32_t>(ADDR_SUB_STATE));
        return false;
    }

    // Log pre-load state
    uint32_t preFrame    = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    uint32_t preRng      = DetVer_GetRngSeed();
    uint32_t preChecksum = ComputeMainChecksum();

    LOG_INFO("[Savestate] LOADING — restoring frame %d (current frame %d)",
             g_slot.info.frame, preFrame);

    // Restore main contiguous region
    __try {
        memcpy((void*)SS_MAIN_START, g_slot.main_state, SS_MAIN_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[Savestate] FAILED — access violation writing main region at 0x%08X", SS_MAIN_START);
        return false;
    }

    // Restore pre-match gap
    __try {
        memcpy((void*)SS_PRE_MATCH_START, g_slot.pre_match_gap, SS_PRE_MATCH_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[Savestate] Failed to restore pre-match gap (non-critical)");
    }

    // Restore RNG seed
    DetVer_SetRngSeed(g_slot.rng_seed);

    // Restore frame counters
    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, g_slot.sim_frame);
    WriteMemory<uint32_t>(ADDR_FRAME_COUNTER, g_slot.display_frame);

    // Restore mode/substate (should be same, but be explicit)
    WriteMemory<uint32_t>(ADDR_GAME_MODE, g_slot.game_mode);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, g_slot.substate);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, g_slot.substate_timer);
    WriteMemory<uint32_t>(ADDR_GAME_TYPE, g_slot.game_type);
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, g_slot.match_phase_timer);

    // Restore input buffers
    __try {
        memcpy((void*)SS_INPUT_P1_START, g_slot.input_p1, SS_INPUT_SIZE);
        memcpy((void*)SS_INPUT_P2_START, g_slot.input_p2, SS_INPUT_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[Savestate] Failed to restore input buffers (non-critical)");
    }

    // Restore input indices
    WriteMemory<uint32_t>(ADDR_INPUT_READ_IDX, g_slot.input_read_idx);
    WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, g_slot.input_write_idx);

    // Post-load cleanup:
    // Clear per-frame temp scratch to prevent stale collision/hit data
    // from bleeding into the first frame after load.
    __try {
        memset((void*)ADDR_MATCH_PER_FRAME_TEMP, 0, MATCH_PER_FRAME_TEMP_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[Savestate] Failed to clear per-frame temp scratch");
    }

    // FPU state: NOT restored by default. If desync investigation reveals
    // that FPU drift is causing issues, uncomment these lines:
    // {
    //     uint16_t cw = g_slot.fpu_cw;
    //     __asm { fldcw word ptr [cw] }
    //     _mm_setcsr(g_slot.fpu_mxcsr);
    // }

    // Verify restoration
    uint32_t postChecksum = ComputeMainChecksum();
    uint32_t postRng      = DetVer_GetRngSeed();
    uint32_t postFrame    = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    bool checksumMatch = (postChecksum == g_slot.info.checksum);

    if (checksumMatch) {
        LOG_INFO("[Savestate] LOADED OK — frame %d->%d checksum=0x%08X (match) rng=0x%08X",
                 preFrame, postFrame, postChecksum, postRng);
    } else {
        LOG_ERROR("[Savestate] LOADED with CHECKSUM MISMATCH! expected=0x%08X got=0x%08X",
                  g_slot.info.checksum, postChecksum);
    }

    // Log to file
    LogToFile("LOAD pre_frame=%d pre_checksum=0x%08X pre_rng=0x%08X\n",
              preFrame, preChecksum, preRng);
    LogToFile("     restored_frame=%d post_checksum=0x%08X rng=0x%08X match=%s\n",
              postFrame, postChecksum, postRng, checksumMatch ? "YES" : "NO");

    return true;
}

// ============================================================================
// Queries
// ============================================================================

const SavestateInfo* Savestate_GetInfo() {
    return &g_slot.info;
}

// ============================================================================
// Hotkey Processing
// ============================================================================

void Savestate_ProcessHotkeys() {
    // Use GetAsyncKeyState for edge detection (key-down transition)
    static bool s_f5WasDown = false;
    static bool s_f6WasDown = false;

    bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;

    // F5: Save (on key-down edge)
    if (f5Down && !s_f5WasDown) {
        if (Savestate_Save()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "State Saved (F%d)", g_slot.info.frame);
            PracticeTools_Toast(buf, 0xFF64FF64);  // green
        } else {
            PracticeTools_Toast("Save Failed", 0xFF6464FF);  // red
        }
    }

    // F6: Load (on key-down edge)
    if (f6Down && !s_f6WasDown) {
        if (Savestate_Load()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "State Loaded (F%d)", g_slot.info.frame);
            PracticeTools_Toast(buf, 0xFF64C8FF);  // cyan
        } else {
            PracticeTools_Toast("Load Failed", 0xFF6464FF);  // red
        }
    }

    s_f5WasDown = f5Down;
    s_f6WasDown = f6Down;
}

// ============================================================================
// Debug UI
// ============================================================================

void Savestate_RenderImGui() {
    const SavestateInfo* info = &g_slot.info;

    ImGui::Text("Manual Savestate (F5 save / F6 load)");
    ImGui::Separator();

    if (info->valid) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Slot: VALID");
        ImGui::Text("Frame:    %d", info->frame);
        ImGui::Text("Checksum: 0x%08X", info->checksum);
        ImGui::Text("RNG Seed: 0x%08X", info->rng_seed);
        ImGui::Text("Mode:     %d  Sub: %d", info->game_mode, info->substate);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Slot: EMPTY");
        ImGui::TextDisabled("Press F5 during a match to save state");
    }

    ImGui::Separator();

    bool canSaveLoad = Savestate_CanSaveLoad();
    ImGui::Text("Can Save/Load: %s", canSaveLoad ? "Yes" : "No");

    if (canSaveLoad) {
        // Show current live state for comparison
        uint32_t curFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        uint32_t curRng   = DetVer_GetRngSeed();
        ImGui::Text("Current Frame: %d", curFrame);
        ImGui::Text("Current RNG:   0x%08X", curRng);

        if (info->valid) {
            int frameDiff = (int)curFrame - (int)info->frame;
            ImGui::Text("Frames since save: %d", frameDiff);
        }
    }

    ImGui::Separator();

    // Manual buttons (in addition to hotkeys)
    if (canSaveLoad) {
        if (ImGui::Button("Save State (F5)")) {
            Savestate_Save();
        }
        ImGui::SameLine();
        if (info->valid) {
            if (ImGui::Button("Load State (F6)")) {
                Savestate_Load();
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Load State (F6)");
            ImGui::EndDisabled();
        }
    } else {
        ImGui::TextDisabled("Start a match to enable save/load");
    }

    ImGui::Separator();
    ImGui::TextDisabled("State region: 0x%08X (%u KB)", SS_MAIN_START, (unsigned)(SS_MAIN_SIZE / 1024));
    ImGui::TextDisabled("FPU: diagnostic only (not restored)");
}
