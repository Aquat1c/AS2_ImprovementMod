/**
 * Alice Senki 2 - Manual Savestate System
 *
 * Captures and restores the full authoritative match state for
 * manual save/load verification (F5 save, F6 load).
 *
 * State captured:
 *   1. Main contiguous region: ADDR_MATCH_BASE through end of P2 entity
 *      (match header + context/camera/weather + effects + summons + P1 + P2)
 *   2. Pre-match gap (effect index, audio channel, render state)
 *   3. RNG seed via _getptd()+0x14 (TLS-based MSVC LCG)
 *   4. Simulation frame counter (ADDR_SIM_FRAME_COUNTER)
 *   5. Game mode / substate / game type
 *   6. Per-frame temp scratch region (match+0x700, 68 bytes)
 *   7. Input buffers (P1 + P2 current input state, 208 bytes each)
 *   8. Match phase timer (intro lock countdown)
 *
 * FPU state (x87 CW, MXCSR) is NOT saved/restored — diagnostic only.
 * A comment marks where it could be added if desync evidence points there.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Savestate Info (read-only metadata for UI / logging)
// ============================================================================

struct SavestateInfo {
    bool     valid;            // Is this slot populated?
    uint32_t frame;            // Frame number at time of save
    uint32_t checksum;         // CRC32 of main state region at save
    uint32_t rng_seed;         // RNG seed at save
    uint32_t game_mode;        // Game mode at save
    uint32_t substate;         // Substate at save
};

// ============================================================================
// Lifecycle
// ============================================================================

void Savestate_Init(void);
void Savestate_Shutdown(void);

// ============================================================================
// Save / Load
// ============================================================================

// Save current game state into the single slot.
// Returns true if save succeeded.
// Only valid during active gameplay (MODE_MATCH, substate 3).
bool Savestate_Save(void);

// Load saved state back into the game.
// Returns true if load succeeded.
// Only valid if a savestate exists and game is in a safe state to restore.
bool Savestate_Load(void);

// ============================================================================
// Queries
// ============================================================================

// Get info about the current savestate slot (for UI display).
const SavestateInfo* Savestate_GetInfo(void);

// Returns true if we are in a state where save/load is allowed.
bool Savestate_CanSaveLoad(void);

// ============================================================================
// Hotkey Processing
// ============================================================================

// Call once per frame from ModOnFrame. Checks F5/F6 and triggers save/load.
void Savestate_ProcessHotkeys(void);

// ============================================================================
// Debug UI
// ============================================================================

// Render ImGui section (for mod menu Savestate tab).
void Savestate_RenderImGui(void);

// ============================================================================
// Log Integration
// ============================================================================

// Set log directory (for determinism log file integration).
void Savestate_SetLogDir(const char* dir);

#ifdef __cplusplus
}
#endif
