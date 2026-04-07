/**
 * Alice Senki 2 - Determinism Verification System
 *
 * Uses real TLS-based RNG state via _getptd()+0x14.
 * Hooks rand/srand at verified addresses (binary-proven).
 * Captures FPU (x87 CW + MXCSR) and game state checksum per frame.
 *
 * All RNG state reads/writes go through _getptd(). No shadow/fake RNG.
 */

#include "rollback/determinism_verify.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "log_window.h"
#include "imgui.h"
#include "MinHook.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>

// ============================================================================
// _getptd access — TLS-based RNG state
// ============================================================================

typedef void* (__cdecl *GetPtdFn)();
static GetPtdFn g_getptd = reinterpret_cast<GetPtdFn>(ADDR_GETPTD);

uint32_t DetVer_GetRngSeed() {
    void* ptd = g_getptd();
    if (!ptd) return 0xDEADBEEF;
    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ptd) + RNG_SEED_OFFSET);
}

void DetVer_SetRngSeed(uint32_t seed) {
    void* ptd = g_getptd();
    if (!ptd) return;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ptd) + RNG_SEED_OFFSET) = seed;
}

// ============================================================================
// RNG Hook Types & Trampolines
// ============================================================================

typedef void (__cdecl *srand_t)(unsigned int seed);
typedef int  (__cdecl *rand_t)(void);

static srand_t g_origSrand = nullptr;
static rand_t  g_origRand  = nullptr;
static bool    g_hooksInstalled = false;

// ============================================================================
// RNG Hook Stats
// ============================================================================

static uint32_t g_srandHitCount    = 0;
static uint32_t g_randHitCount     = 0;
static uint32_t g_lastSrandCaller  = 0;
static uint32_t g_lastSrandSeed    = 0;
static uint32_t g_lastRandCaller   = 0;
static int      g_lastRandResult   = 0;

// ============================================================================
// System State
// ============================================================================

static bool g_enabled       = false;
static bool g_fpuEnforce    = false;

// Per-frame rand call counter (reset each frame)
static int  g_frameRandCalls   = 0;
static int  g_frameCallerIdx   = 0;

// Ring buffer
static DeterminismFrameTrace g_ring[DETVER_RING_SIZE];
static int  g_ringHead  = 0;
static int  g_ringCount = 0;

// Per-frame accumulator
static DeterminismFrameTrace g_current;
static bool g_frameActive = false;

// Capture windows
static DeterminismFrameTrace g_captureA[DETVER_CAPTURE_MAX];
static int g_captureACount = 0;
static DeterminismFrameTrace g_captureB[DETVER_CAPTURE_MAX];
static int g_captureBCount = 0;

typedef enum { CAPTURE_NONE = 0, CAPTURE_A = 1, CAPTURE_B = 2 } CaptureTarget;
static CaptureTarget g_captureTarget = CAPTURE_NONE;

// File logging
static char  g_logDir[MAX_PATH] = {0};
static FILE* g_logFile = nullptr;
static int   g_logFlushCounter = 0;
#define LOG_FLUSH_INTERVAL 60

// ============================================================================
// FPU Capture
// ============================================================================

static inline uint16_t CaptureX87CW() {
    uint16_t cw = 0;
    __asm { fnstcw word ptr [cw] }
    return cw;
}

static inline uint32_t CaptureMXCSR() {
    return _mm_getcsr();
}

static inline void EnforceFpuState() {
    uint16_t cw = 0x027F;
    __asm { fldcw word ptr [cw] }
    _mm_setcsr(0x1F80);
}

// ============================================================================
// Game State Checksum
// ============================================================================

#define DETVER_STATE_START  ADDR_MATCH_BASE
#define DETVER_STATE_SIZE   ((ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE)

static uint32_t ComputeGameStateChecksum() {
    __try {
        return CalcCRC32((const void*)DETVER_STATE_START, DETVER_STATE_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADDEAD;
    }
}

// ============================================================================
// File Logging
// ============================================================================

static void EnsureLogFile() {
    if (g_logFile) return;

    char path[MAX_PATH];
    if (g_logDir[0]) {
        CreateDirectoryA(g_logDir, NULL);
        snprintf(path, sizeof(path), "%s\\determinism_log.txt", g_logDir);
    } else {
        CreateDirectoryA("logs", NULL);
        snprintf(path, sizeof(path), "logs\\determinism_log.txt");
    }

    g_logFile = fopen(path, "w");
    if (g_logFile) {
        fprintf(g_logFile, "# Alice Senki 2 - Determinism Verification Log\n");
        fprintf(g_logFile, "# Build: %s %s\n", __DATE__, __TIME__);
        fprintf(g_logFile, "# RNG: rand=0x%08X srand=0x%08X _getptd=0x%08X seed_offset=+0x%X\n",
                ADDR_STATIC_RAND, ADDR_STATIC_SRAND, ADDR_GETPTD, RNG_SEED_OFFSET);
        fprintf(g_logFile, "# State region: 0x%08X size=%u\n",
                DETVER_STATE_START, (unsigned)DETVER_STATE_SIZE);
        fprintf(g_logFile, "#\n");
        fflush(g_logFile);
        LOG_INFO("[DetVer] Log file opened: %s", path);
    }
}

static void LogToFile(const char* fmt, ...) {
    EnsureLogFile();
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    g_logFlushCounter++;
    if (g_logFlushCounter >= LOG_FLUSH_INTERVAL) {
        fflush(g_logFile);
        g_logFlushCounter = 0;
    }
}

static void WriteTraceToFile(const DeterminismFrameTrace* t) {
    if (!g_logFile) return;
    fprintf(g_logFile,
        "Frame: %d\n"
        "RNG: begin=0x%08X end=0x%08X calls=%d\n"
        "FPU: cw=0x%04X->0x%04X mxcsr=0x%08X->0x%08X\n"
        "CHK: 0x%08X\n",
        t->frame,
        t->rng_begin, t->rng_end, t->rand_calls,
        t->fpu_cw_begin, t->fpu_cw_end, t->mxcsr_begin, t->mxcsr_end,
        t->checksum);
    if (t->rand_callers[0]) {
        fprintf(g_logFile, "CALLERS:");
        for (int i = 0; i < DETVER_MAX_CALLERS && t->rand_callers[i]; i++)
            fprintf(g_logFile, " 0x%08X", t->rand_callers[i]);
        fprintf(g_logFile, "\n");
    }
    fprintf(g_logFile, "\n");
    g_logFlushCounter++;
    if (g_logFlushCounter >= LOG_FLUSH_INTERVAL) {
        fflush(g_logFile);
        g_logFlushCounter = 0;
    }
}

// ============================================================================
// Ring Buffer
// ============================================================================

static void RingPush(const DeterminismFrameTrace* t) {
    g_ring[g_ringHead] = *t;
    g_ringHead = (g_ringHead + 1) % DETVER_RING_SIZE;
    if (g_ringCount < DETVER_RING_SIZE) g_ringCount++;
}

static const DeterminismFrameTrace* RingGet(int ago) {
    if (ago < 0 || ago >= g_ringCount) return nullptr;
    int idx = (g_ringHead - 1 - ago + DETVER_RING_SIZE) % DETVER_RING_SIZE;
    return &g_ring[idx];
}

// ============================================================================
// RNG Hook Callbacks
// ============================================================================

static void __cdecl Hook_srand(unsigned int seed) {
    g_srandHitCount++;
    g_lastSrandCaller = (uint32_t)(uintptr_t)_ReturnAddress();
    g_lastSrandSeed = seed;

    if (g_enabled) {
        LogToFile("EVENT: srand(%u) caller=0x%08X\n", seed, g_lastSrandCaller);
    }

    // Forward to original — it writes to _getptd()->holdrand
    if (g_origSrand) g_origSrand(seed);
}

static int __cdecl Hook_rand() {
    // Always call original — it reads/writes _getptd()->holdrand
    int result = g_origRand ? g_origRand() : 0;

    g_randHitCount++;
    g_lastRandCaller = (uint32_t)(uintptr_t)_ReturnAddress();
    g_lastRandResult = result;

    if (g_enabled && g_frameActive) {
        g_frameRandCalls++;
        if (g_frameCallerIdx < DETVER_MAX_CALLERS)
            g_current.rand_callers[g_frameCallerIdx++] = g_lastRandCaller;
    }

    return result;
}

// ============================================================================
// Hook Installation / Removal
// ============================================================================

bool DetVer_InstallHooks() {
    if (g_hooksInstalled) {
        LOG_WARN("[DetVer] Hooks already installed");
        return true;
    }

    MH_STATUS st;

    // Hook srand
    st = MH_CreateHook(
        reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)),
        reinterpret_cast<void*>(&Hook_srand),
        reinterpret_cast<void**>(&g_origSrand));
    if (st != MH_OK) {
        LOG_ERROR("[DetVer] MH_CreateHook srand failed at 0x%08X: status=%d",
                  ADDR_STATIC_SRAND, st);
        return false;
    }

    // Hook rand
    st = MH_CreateHook(
        reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_RAND)),
        reinterpret_cast<void*>(&Hook_rand),
        reinterpret_cast<void**>(&g_origRand));
    if (st != MH_OK) {
        LOG_ERROR("[DetVer] MH_CreateHook rand failed at 0x%08X: status=%d",
                  ADDR_STATIC_RAND, st);
        MH_RemoveHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)));
        g_origSrand = nullptr;
        return false;
    }

    // Enable both
    st = MH_EnableHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)));
    if (st != MH_OK) {
        LOG_ERROR("[DetVer] MH_EnableHook srand failed: status=%d", st);
        MH_RemoveHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)));
        MH_RemoveHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_RAND)));
        g_origSrand = nullptr;
        g_origRand  = nullptr;
        return false;
    }

    st = MH_EnableHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_RAND)));
    if (st != MH_OK) {
        LOG_ERROR("[DetVer] MH_EnableHook rand failed: status=%d", st);
        MH_DisableHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)));
        MH_RemoveHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)));
        MH_RemoveHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_RAND)));
        g_origSrand = nullptr;
        g_origRand  = nullptr;
        return false;
    }

    g_hooksInstalled = true;
    LOG_INFO("[DetVer] Hooks installed: srand=0x%08X rand=0x%08X", ADDR_STATIC_SRAND, ADDR_STATIC_RAND);
    LogToFile("# HOOKS INSTALLED: srand=0x%08X rand=0x%08X\n\n", ADDR_STATIC_SRAND, ADDR_STATIC_RAND);

    return true;
}

void DetVer_RemoveHooks() {
    if (!g_hooksInstalled) return;

    MH_DisableHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)));
    MH_DisableHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_RAND)));
    MH_RemoveHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_SRAND)));
    MH_RemoveHook(reinterpret_cast<void*>(static_cast<uintptr_t>(ADDR_STATIC_RAND)));

    g_origSrand = nullptr;
    g_origRand  = nullptr;
    g_hooksInstalled = false;

    LOG_INFO("[DetVer] RNG hooks removed");
    LogToFile("# HOOKS REMOVED\n\n");
}

bool DetVer_AreHooksInstalled() {
    return g_hooksInstalled;
}

// ============================================================================
// RNG Hook Stats
// ============================================================================

uint32_t DetVer_GetRandHitCount()  { return g_randHitCount; }
uint32_t DetVer_GetSrandHitCount() { return g_srandHitCount; }

// ============================================================================
// FPU Enforcement
// ============================================================================

void DetVer_SetFpuEnforce(bool enabled) {
    g_fpuEnforce = enabled;
    LOG_INFO("[DetVer] FPU enforcement: %s", enabled ? "ON" : "OFF");
    LogToFile("# FPU enforcement: %s\n\n", enabled ? "ON" : "OFF");
}

bool DetVer_GetFpuEnforce() { return g_fpuEnforce; }

// ============================================================================
// Lifecycle
// ============================================================================

void DetVer_Init() {
    memset(g_ring, 0, sizeof(g_ring));
    g_ringHead = 0;
    g_ringCount = 0;
    g_captureACount = 0;
    g_captureBCount = 0;
    g_captureTarget = CAPTURE_NONE;
    g_enabled = false;
    g_fpuEnforce = false;
    g_frameActive = false;
    g_logFile = nullptr;
    g_logFlushCounter = 0;
    g_hooksInstalled = false;
    g_srandHitCount = 0;
    g_randHitCount = 0;
    g_frameRandCalls = 0;
    g_frameCallerIdx = 0;

    LOG_INFO("[DetVer] Determinism verification initialized");
    LOG_INFO("[DetVer] _getptd=0x%08X, seed offset=+0x%X", ADDR_GETPTD, RNG_SEED_OFFSET);

    // Read current RNG seed to verify _getptd works
    uint32_t seed = DetVer_GetRngSeed();
    LOG_INFO("[DetVer] Current RNG seed (via _getptd): 0x%08X", seed);
}

void DetVer_Shutdown() {
    DetVer_RemoveHooks();

    if (g_logFile) {
        fprintf(g_logFile, "# Session ended\n");
        fprintf(g_logFile, "# Final: srand_hits=%u rand_hits=%u\n",
                g_srandHitCount, g_randHitCount);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_enabled = false;
    LOG_INFO("[DetVer] Shut down (srand_hits=%u rand_hits=%u)", g_srandHitCount, g_randHitCount);
}

// ============================================================================
// Per-frame Hooks
// ============================================================================

void DetVer_BeginFrame(int frame) {
    if (!g_enabled) return;

    // Optional FPU enforcement
    if (g_fpuEnforce) {
        EnforceFpuState();
    }

    memset(&g_current, 0, sizeof(g_current));
    g_current.frame = frame;
    g_frameRandCalls = 0;
    g_frameCallerIdx = 0;

    // Read real RNG seed from TLS
    g_current.rng_begin    = DetVer_GetRngSeed();
    g_current.fpu_cw_begin = CaptureX87CW();
    g_current.mxcsr_begin  = CaptureMXCSR();

    g_frameActive = true;
}

void DetVer_EndFrame(int frame) {
    if (!g_enabled || !g_frameActive) return;
    g_frameActive = false;

    // Read real RNG seed from TLS
    g_current.rng_end    = DetVer_GetRngSeed();
    g_current.fpu_cw_end = CaptureX87CW();
    g_current.mxcsr_end  = CaptureMXCSR();
    g_current.rand_calls = g_frameRandCalls;
    g_current.checksum   = ComputeGameStateChecksum();

    RingPush(&g_current);

    if (g_captureTarget == CAPTURE_A && g_captureACount < DETVER_CAPTURE_MAX)
        g_captureA[g_captureACount++] = g_current;
    else if (g_captureTarget == CAPTURE_B && g_captureBCount < DETVER_CAPTURE_MAX)
        g_captureB[g_captureBCount++] = g_current;

    EnsureLogFile();
    WriteTraceToFile(&g_current);
}

// ============================================================================
// Controls
// ============================================================================

void DetVer_SetEnabled(bool enabled) {
    if (enabled && !g_enabled) {
        LOG_INFO("[DetVer] Verification ENABLED (hooks=%s, fpu_enforce=%s)",
                 g_hooksInstalled ? "YES" : "NO",
                 g_fpuEnforce ? "YES" : "NO");
        LogToFile("# === Verification ENABLED (hooks=%s fpu_enforce=%s) ===\n\n",
                  g_hooksInstalled ? "YES" : "NO",
                  g_fpuEnforce ? "YES" : "NO");

        // Log current RNG seed
        uint32_t seed = DetVer_GetRngSeed();
        LOG_INFO("[DetVer] RNG seed at enable: 0x%08X", seed);
        LogToFile("# RNG seed at enable: 0x%08X\n\n", seed);
    } else if (!enabled && g_enabled) {
        LOG_INFO("[DetVer] Verification DISABLED");
        LogToFile("# === Verification DISABLED ===\n\n");
    }
    g_enabled = enabled;
}

bool DetVer_IsEnabled() { return g_enabled; }

void DetVer_Reset() {
    g_ringHead = 0;
    g_ringCount = 0;
    g_captureACount = 0;
    g_captureBCount = 0;
    g_captureTarget = CAPTURE_NONE;
    g_frameActive = false;
    g_srandHitCount = 0;
    g_randHitCount = 0;
    g_frameRandCalls = 0;
    g_frameCallerIdx = 0;

    LOG_INFO("[DetVer] Reset (ring, captures, hit counters)");
    LogToFile("# === RESET ===\n\n");
}

void DetVer_FlushToFile() {
    EnsureLogFile();
    if (!g_logFile) return;
    fprintf(g_logFile, "# === FLUSH: %d entries in ring ===\n", g_ringCount);
    for (int i = g_ringCount - 1; i >= 0; i--) {
        const DeterminismFrameTrace* t = RingGet(i);
        if (t) WriteTraceToFile(t);
    }
    fprintf(g_logFile, "# === END FLUSH ===\n\n");
    fflush(g_logFile);
    LOG_INFO("[DetVer] Flushed %d entries to log", g_ringCount);
}

// ============================================================================
// Capture Windows
// ============================================================================

void DetVer_StartCaptureA() {
    g_captureACount = 0;
    g_captureTarget = CAPTURE_A;
    LOG_INFO("[DetVer] Capture A started");
    LogToFile("# === Capture A START ===\n\n");
}

void DetVer_StartCaptureB() {
    g_captureBCount = 0;
    g_captureTarget = CAPTURE_B;
    LOG_INFO("[DetVer] Capture B started");
    LogToFile("# === Capture B START ===\n\n");
}

void DetVer_StopCapture() {
    if (g_captureTarget == CAPTURE_A)
        LOG_INFO("[DetVer] Capture A stopped: %d frames", g_captureACount);
    else if (g_captureTarget == CAPTURE_B)
        LOG_INFO("[DetVer] Capture B stopped: %d frames", g_captureBCount);
    g_captureTarget = CAPTURE_NONE;
}

int DetVer_Compare() {
    int countA = g_captureACount;
    int countB = g_captureBCount;
    if (countA == 0 || countB == 0) {
        LOG_WARN("[DetVer] Cannot compare: A=%d B=%d frames", countA, countB);
        return -1;
    }

    int compareCount = (countA < countB) ? countA : countB;
    int mismatches = 0;
    int firstMismatch = -1;

    EnsureLogFile();
    if (g_logFile)
        fprintf(g_logFile, "# === COMPARISON: A(%d) vs B(%d) ===\n", countA, countB);

    for (int i = 0; i < compareCount; i++) {
        const DeterminismFrameTrace* a = &g_captureA[i];
        const DeterminismFrameTrace* b = &g_captureB[i];

        bool rngMatch  = (a->rng_begin == b->rng_begin) && (a->rng_end == b->rng_end);
        bool callMatch = (a->rand_calls == b->rand_calls);
        bool fpuMatch  = (a->fpu_cw_begin == b->fpu_cw_begin) &&
                         (a->fpu_cw_end == b->fpu_cw_end) &&
                         (a->mxcsr_begin == b->mxcsr_begin) &&
                         (a->mxcsr_end == b->mxcsr_end);
        bool chkMatch  = (a->checksum == b->checksum);

        if (!rngMatch || !callMatch || !fpuMatch || !chkMatch) {
            mismatches++;
            if (firstMismatch < 0) firstMismatch = i;
            if (g_logFile) {
                fprintf(g_logFile, "MISMATCH at index %d (A.frame=%d B.frame=%d):\n",
                        i, a->frame, b->frame);
                if (!rngMatch)
                    fprintf(g_logFile, "  RNG: A begin=0x%08X end=0x%08X | B begin=0x%08X end=0x%08X\n",
                            a->rng_begin, a->rng_end, b->rng_begin, b->rng_end);
                if (!callMatch)
                    fprintf(g_logFile, "  CALLS: A=%d B=%d\n", a->rand_calls, b->rand_calls);
                if (!fpuMatch)
                    fprintf(g_logFile, "  FPU: A cw=0x%04X->0x%04X mxcsr=0x%08X->0x%08X"
                            " | B cw=0x%04X->0x%04X mxcsr=0x%08X->0x%08X\n",
                            a->fpu_cw_begin, a->fpu_cw_end, a->mxcsr_begin, a->mxcsr_end,
                            b->fpu_cw_begin, b->fpu_cw_end, b->mxcsr_begin, b->mxcsr_end);
                if (!chkMatch)
                    fprintf(g_logFile, "  CHECKSUM: A=0x%08X B=0x%08X\n", a->checksum, b->checksum);
                fprintf(g_logFile, "\n");
            }
        }
    }

    if (g_logFile) {
        if (mismatches == 0)
            fprintf(g_logFile, "MATCH: %d frames, all identical\n\n", compareCount);
        else
            fprintf(g_logFile, "RESULT: %d mismatches in %d frames (first at %d)\n\n",
                    mismatches, compareCount, firstMismatch);
        fflush(g_logFile);
    }

    if (mismatches == 0)
        LOG_INFO("[DetVer] MATCH: %d frames, identical", compareCount);
    else
        LOG_WARN("[DetVer] MISMATCH: %d in %d frames (first at %d)",
                 mismatches, compareCount, firstMismatch);
    return mismatches;
}

// ============================================================================
// Status Queries
// ============================================================================

bool DetVer_IsCaptureActive() { return g_captureTarget != CAPTURE_NONE; }
int  DetVer_GetCaptureACount() { return g_captureACount; }
int  DetVer_GetCaptureBCount() { return g_captureBCount; }
int  DetVer_GetRingCount() { return g_ringCount; }
const DeterminismFrameTrace* DetVer_GetLatestTrace() { return RingGet(0); }

void DetVer_SetLogDir(const char* dir) {
    if (dir) {
        strncpy(g_logDir, dir, sizeof(g_logDir) - 1);
        g_logDir[sizeof(g_logDir) - 1] = '\0';
    } else {
        g_logDir[0] = '\0';
    }
}

// ============================================================================
// ImGui Debug Panel
// ============================================================================

void DetVer_RenderImGui() {
    // === RNG Status (always visible) ===
    if (ImGui::CollapsingHeader("RNG Status", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Show live RNG seed from TLS
        uint32_t currentSeed = DetVer_GetRngSeed();
        ImGui::Text("Live RNG Seed: 0x%08X", currentSeed);
        ImGui::Text("_getptd: 0x%08X  offset: +0x%X", ADDR_GETPTD, RNG_SEED_OFFSET);

        ImGui::Separator();

        // Hook controls
        if (!g_hooksInstalled) {
            if (ImGui::Button("Install RNG Hooks")) {
                DetVer_InstallHooks();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(rand=0x%08X srand=0x%08X)", ADDR_STATIC_RAND, ADDR_STATIC_SRAND);
        } else {
            ImGui::TextColored(ImVec4(0, 0.8f, 1, 1), "Hooks ACTIVE");
            ImGui::SameLine();
            if (ImGui::Button("Remove Hooks")) {
                DetVer_RemoveHooks();
            }
        }

        // Hook stats
        if (g_srandHitCount > 0 || g_randHitCount > 0) {
            ImGui::Text("srand hits: %u (last caller=0x%08X seed=%u)",
                        g_srandHitCount, g_lastSrandCaller, g_lastSrandSeed);
            ImGui::Text("rand  hits: %u (last caller=0x%08X result=%d)",
                        g_randHitCount, g_lastRandCaller, g_lastRandResult);
        }
    }

    ImGui::Separator();

    // === FPU ===
    if (ImGui::CollapsingHeader("FPU State")) {
        uint16_t cw = CaptureX87CW();
        uint32_t mxcsr = CaptureMXCSR();
        ImGui::Text("x87 CW:  0x%04X", cw);
        ImGui::Text("MXCSR:   0x%08X", mxcsr);

        bool enforce = g_fpuEnforce;
        if (ImGui::Checkbox("Force FPU State (CW=0x027F MXCSR=0x1F80)", &enforce))
            DetVer_SetFpuEnforce(enforce);
    }

    ImGui::Separator();

    // === Frame Tracing ===
    bool enabled = g_enabled;
    if (ImGui::Checkbox("Enable Frame Tracing", &enabled))
        DetVer_SetEnabled(enabled);

    if (!g_enabled) {
        ImGui::TextDisabled("(enable to start per-frame tracing)");
        return;
    }

    ImGui::Separator();

    // Ring buffer + latest trace
    ImGui::Text("Ring: %d / %d frames", g_ringCount, DETVER_RING_SIZE);

    const DeterminismFrameTrace* latest = DetVer_GetLatestTrace();
    if (latest) {
        ImGui::Text("Frame: %d", latest->frame);
        ImGui::Text("RNG: 0x%08X -> 0x%08X (%d calls)",
                    latest->rng_begin, latest->rng_end, latest->rand_calls);

        bool cwChanged    = latest->fpu_cw_begin != latest->fpu_cw_end;
        bool mxcsrChanged = latest->mxcsr_begin  != latest->mxcsr_end;
        if (cwChanged || mxcsrChanged)
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1),
                "FPU: cw=0x%04X->0x%04X mxcsr=0x%08X->0x%08X",
                latest->fpu_cw_begin, latest->fpu_cw_end,
                latest->mxcsr_begin, latest->mxcsr_end);
        else
            ImGui::Text("FPU: cw=0x%04X mxcsr=0x%08X (stable)",
                        latest->fpu_cw_begin, latest->mxcsr_begin);

        ImGui::Text("Checksum: 0x%08X", latest->checksum);
    } else {
        ImGui::TextDisabled("No trace data (enter gameplay)");
    }

    ImGui::Separator();

    // Controls
    if (ImGui::Button("Reset All")) DetVer_Reset();
    ImGui::SameLine();
    if (ImGui::Button("Flush to File")) DetVer_FlushToFile();

    ImGui::Separator();

    // Capture controls
    ImGui::Text("Captures: A=%d  B=%d", g_captureACount, g_captureBCount);

    if (g_captureTarget == CAPTURE_NONE) {
        if (ImGui::Button("Start Capture A")) DetVer_StartCaptureA();
        ImGui::SameLine();
        if (ImGui::Button("Start Capture B")) DetVer_StartCaptureB();
    } else {
        const char* label = (g_captureTarget == CAPTURE_A) ? "Recording A..." : "Recording B...";
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", label);
        ImGui::SameLine();
        if (ImGui::Button("Stop Capture")) DetVer_StopCapture();
    }

    if (g_captureACount > 0 && g_captureBCount > 0) {
        if (ImGui::Button("Compare A vs B"))
            DetVer_Compare();
    }
}
