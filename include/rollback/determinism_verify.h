/**
 * Alice Senki 2 - Determinism Verification System
 *
 * Correct RNG hooking via TLS-based _getptd()->holdrand (+0x14).
 * Per-frame capture of RNG state, FPU state, and game checksum.
 * A/B capture comparison for replay determinism proof.
 *
 * Verified addresses (binary proof — single LCG instance):
 *   rand   = 0x71459D   (1112 callers)
 *   srand  = 0x714590   (1 caller — init with time())
 *   _getptd= 0x716D29   (TLS getter, seed at +0x14)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define DETVER_MAX_CALLERS      8
#define DETVER_RING_SIZE        1024
#define DETVER_CAPTURE_MAX      512

// ============================================================================
// Frame Trace Record
// ============================================================================

struct DeterminismFrameTrace {
    int      frame;

    uint32_t rng_begin;
    uint32_t rng_end;
    int      rand_calls;

    uint16_t fpu_cw_begin;
    uint16_t fpu_cw_end;

    uint32_t mxcsr_begin;
    uint32_t mxcsr_end;

    uint32_t checksum;

    uint32_t rand_callers[DETVER_MAX_CALLERS];
};

// ============================================================================
// Lifecycle
// ============================================================================

void DetVer_Init(void);
void DetVer_Shutdown(void);

// ============================================================================
// Per-frame hooks (call from main loop)
// ============================================================================

void DetVer_BeginFrame(int frame);
void DetVer_EndFrame(int frame);

// ============================================================================
// Controls
// ============================================================================

void DetVer_SetEnabled(bool enabled);
bool DetVer_IsEnabled(void);
void DetVer_Reset(void);
void DetVer_FlushToFile(void);

// ============================================================================
// RNG Hook Management
// ============================================================================

bool DetVer_InstallHooks(void);
void DetVer_RemoveHooks(void);
bool DetVer_AreHooksInstalled(void);

// ============================================================================
// TLS-based RNG State Access
// ============================================================================

uint32_t DetVer_GetRngSeed(void);
void     DetVer_SetRngSeed(uint32_t seed);

// ============================================================================
// FPU Enforcement
// ============================================================================

void DetVer_SetFpuEnforce(bool enabled);
bool DetVer_GetFpuEnforce(void);

// ============================================================================
// Capture Windows (A/B comparison)
// ============================================================================

void DetVer_StartCaptureA(void);
void DetVer_StartCaptureB(void);
void DetVer_StopCapture(void);
int  DetVer_Compare(void);

// ============================================================================
// Status queries
// ============================================================================

bool DetVer_IsCaptureActive(void);
int  DetVer_GetCaptureACount(void);
int  DetVer_GetCaptureBCount(void);
int  DetVer_GetRingCount(void);
const struct DeterminismFrameTrace* DetVer_GetLatestTrace(void);

// RNG hook stats
uint32_t DetVer_GetRandHitCount(void);
uint32_t DetVer_GetSrandHitCount(void);

// ============================================================================
// ImGui / Logging
// ============================================================================

void DetVer_RenderImGui(void);
void DetVer_SetLogDir(const char* dir);

#ifdef __cplusplus
}
#endif
