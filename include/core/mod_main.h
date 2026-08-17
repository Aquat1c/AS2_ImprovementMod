/**
 * Alice Senki 2 - Mod Main Header
 *
 * Slim public API for the mod lifecycle and game state queries.
 * Replaces as2_rollback.h for the active (non-rollback) build.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include "as2_constants.h"

// ============================================================================
// Mod Lifecycle Exports (called by d3d9_proxy)
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

__declspec(dllexport) void ModSetImGuiContext(void* ctx);
__declspec(dllexport) void ModSetLogDir(const char* dir);
__declspec(dllexport) void ModInit(HMODULE gameModule);
__declspec(dllexport) void ModShutdown();
__declspec(dllexport) void ModOnGameExit(int exitCode, const char* reason);
__declspec(dllexport) void ModOnFrame();
__declspec(dllexport) void ModOnPresent(void* pDevice);
__declspec(dllexport) bool ModWantsExclusiveOverlay();
__declspec(dllexport) void ModToggleMenu();
__declspec(dllexport) bool ModIsMenuRequestedOpen();
__declspec(dllexport) bool ModShouldRenderImGui();
__declspec(dllexport) bool ModGetNetplayHudText(char* out, int cap);

struct MatchHudData {
    bool     active;
    char     p1_name[64];
    char     p2_name[64];
    int      p1_wins;
    int      p2_wins;
    uint8_t  p1_trail_r;
    uint8_t  p1_trail_g;
    uint8_t  p1_trail_b;
    uint8_t  p1_text_r;
    uint8_t  p1_text_g;
    uint8_t  p1_text_b;
    uint8_t  p2_trail_r;
    uint8_t  p2_trail_g;
    uint8_t  p2_trail_b;
    uint8_t  p2_text_r;
    uint8_t  p2_text_g;
    uint8_t  p2_text_b;
    uint8_t  p1_score_r;
    uint8_t  p1_score_g;
    uint8_t  p1_score_b;
    uint8_t  p2_score_r;
    uint8_t  p2_score_g;
    uint8_t  p2_score_b;
    uint16_t p1_trail_length_px;
    uint16_t p2_trail_length_px;
    uint8_t  p1_vertical_position;
    uint8_t  p2_vertical_position;
    uint8_t  p1_font_size;
    uint8_t  p2_font_size;
    float    ping_ms;
    int      delay_frames;
    int      rollback_frames;      // configured rollback budget (R_local)
    int      rollback_depth_now;   // last rollback transaction's replay length
                                   // (per-frame achieved depth in forced mode)
    int      local_frame;
    int      remote_frame;
    bool     is_host;
    bool     spectator_mode;
    bool     show_connection_stats;
    char     status_text[64];
};
__declspec(dllexport) bool ModGetMatchHudData(MatchHudData* out);

__declspec(dllexport) uint32_t GetCurrentFrame();
__declspec(dllexport) uint16_t GetPlayerHP(int player);
__declspec(dllexport) bool* GetForceBorderlessPtr();

#ifdef __cplusplus
}
#endif

// ============================================================================
// Game State Queries
// ============================================================================

uint32_t AS2_GetFrameNumber();
bool     AS2_IsInMatch();
bool     AS2_IsInGameplay();
bool     AS2_IsInPlayableGameplay();
bool     AS2_IsInPauseMenu();
bool     AS2_IsInMenu();

// Entity access
uintptr_t GetEntityBase(int playerIndex);
uint16_t  GetP1HP();
uint16_t  GetP2HP();

// Utility
uint32_t  AS2_GetQuickChecksum();
void      AS2_ClearVanillaNetplayBuffers();

// Shared UI scaling helpers. These compensate for proxy upscaling so the
// mod's ImGui does not become oversized when the window is enlarged.
float     ModUI_GetScale();
float     ModUI_Scale(float value);

// Verbose logging control
void SetVerboseLogging(bool enabled);
bool GetVerboseLogging();

// Config accessors for other modules
bool ModConfig_UseSDLInput();
bool ModConfig_VerboseLogging();
