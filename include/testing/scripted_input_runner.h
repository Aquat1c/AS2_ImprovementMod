/**
 * Alice Senki 2 - Scripted Input Runner
 *
 * Lightweight offline-first scripted input playback for deterministic
 * testing. Drives P1/P2 inputs frame-by-frame via the active input
 * override path (InputSystem_SetOverride). No dependency on netplay,
 * session manager, or old rollback architecture.
 *
 * Integration:
 *   - Call SIR_Init()      from DeferredInit
 *   - Call SIR_OnFrame()   from ModOnFrame (every frame)
 *   - Call SIR_Shutdown()  from ModShutdown
 *   - Call SIR_RenderImGui() from a menu tab
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Run modes
// ============================================================================

typedef enum {
    SIR_MODE_LOCAL_VS,      // P1 scripted, P2 idle or scripted
    SIR_MODE_VS_CPU,        // P1 scripted, P2 game-owned (CPU)
    SIR_MODE_DUAL_SCRIPT    // P1 scripted, P2 scripted (independent)
} SIR_RunMode;

// ============================================================================
// Scripted input entry — one frame-range of input for one player
// ============================================================================

typedef struct {
    int      frameStart;    // First frame (inclusive, 0-based in scenario)
    int      frameEnd;      // Last frame (inclusive)
    int      player;        // 0 = P1, 1 = P2
    uint16_t input;         // Bitmask (INPUT_UP | INPUT_A etc.)
} SIR_InputEntry;

// ============================================================================
// Scenario definition — named list of input entries
// ============================================================================

#define SIR_MAX_NAME      32
#define SIR_MAX_ENTRIES  512

typedef struct {
    char           name[SIR_MAX_NAME];
    int            totalFrames;     // Total scenario length
    bool           hasP2;           // True if scenario provides P2 inputs
    int            entryCount;
    SIR_InputEntry entries[SIR_MAX_ENTRIES];
} SIR_Scenario;

// ============================================================================
// Runner state (read-only query)
// ============================================================================

typedef struct {
    bool        active;
    bool        looping;
    SIR_RunMode mode;
    int         currentFrame;       // Frame within scenario (0-based)
    int         totalFrames;
    uint16_t    lastP1Input;
    uint16_t    lastP2Input;
    const char* scenarioName;
} SIR_Status;

// ============================================================================
// Lifecycle
// ============================================================================

void SIR_Init(void);
void SIR_Shutdown(void);

// Called every frame from ModOnFrame — handles playback and input injection
void SIR_OnFrame(void);

// ============================================================================
// Control API
// ============================================================================

// Select a scenario by index into the built-in scenario list
void SIR_SelectScenario(int index);

// Start / stop / restart
bool SIR_Start(void);      // Returns false if no scenario selected
void SIR_Stop(void);
void SIR_Restart(void);    // Stop + reset frame to 0 + start

// Run mode
void SIR_SetMode(SIR_RunMode mode);
SIR_RunMode SIR_GetMode(void);

// Looping
void SIR_SetLooping(bool loop);
bool SIR_IsLooping(void);

// ============================================================================
// Scenario registry
// ============================================================================

int              SIR_GetScenarioCount(void);
const SIR_Scenario* SIR_GetScenario(int index);
int              SIR_GetSelectedIndex(void);

// ============================================================================
// Status query
// ============================================================================

SIR_Status SIR_GetStatus(void);
bool       SIR_IsActive(void);

// ============================================================================
// ImGui
// ============================================================================

void SIR_RenderImGui(void);

#ifdef __cplusplus
}
#endif
