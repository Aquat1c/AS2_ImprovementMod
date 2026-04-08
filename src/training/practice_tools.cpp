/**
 * Alice Senki 2 - Practice Mode Tools
 *
 * Pause/unpause, single-frame advance, controller swap with CPU flag
 * management.  All features are gated to training mode only
 * (GAMETYPE_TRAINING in MODE_MATCH).
 *
 * Hotkeys:
 *   F7  = Toggle pause / unpause
 *   F8  = Single-frame advance (while paused)
 *   F9  = Toggle controller swap (P1 <-> P2)
 */

#include "training/practice_tools.h"
#include "core/game_state.h"
#include "patches/memory_utils.h"
#include "input_system.h"
#include "as2_constants.h"
#include "log_window.h"
#include "imgui.h"

#include <windows.h>
#include <stdint.h>

// ============================================================================
// Internal state
// ============================================================================

static bool s_initialized = false;
static bool s_paused = false;
static bool s_stepRequested = false;   // Allow exactly one frame through
static bool s_wasActive = false;       // Previous frame's practice-mode status

// Edge detection for hotkeys (GetAsyncKeyState)
static bool s_f7WasDown = false;
static bool s_f8WasDown = false;
static bool s_f9WasDown = false;

// ============================================================================
// Practice-mode detection
// ============================================================================

// Training mode = GAMETYPE_TRAINING (4) in MODE_MATCH.
// The 5th main-menu option ("Training") sets dword_816410 = 4.
static bool IsPracticeModeNow() {
    return GetGameType() == GAMETYPE_TRAINING
        && GetGameMode() == MODE_MATCH;
}

// ============================================================================
// Controller swap helpers
// ============================================================================

static void ApplyControlSwap(bool swapped) {
    InputSystem_SetControlSwap(swapped);

    if (swapped) {
        // Player controls P2 side: P1 becomes AI, P2 becomes human
        WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 1);
        WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 0);
    } else {
        // Default: P1 human, P2 AI
        WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 0);
        WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 1);
    }
}

// ============================================================================
// Cleanup — reset all practice state
// ============================================================================

static void ResetPracticeState() {
    if (s_paused || InputSystem_GetControlSwap()) {
        LOG_INFO("[Practice] Cleaning up practice state");
    }

    s_paused = false;
    s_stepRequested = false;

    // Restore default control mapping if swapped
    if (InputSystem_GetControlSwap()) {
        ApplyControlSwap(false);
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void PracticeTools_Init() {
    s_initialized = true;
    s_paused = false;
    s_stepRequested = false;
    s_wasActive = false;
    s_f7WasDown = false;
    s_f8WasDown = false;
    s_f9WasDown = false;
    LOG_INFO("[Practice] Practice tools initialized");
}

void PracticeTools_Shutdown() {
    ResetPracticeState();
    s_initialized = false;
}

// ============================================================================
// Per-frame update
// ============================================================================

void PracticeTools_FrameUpdate() {
    if (!s_initialized) return;

    bool active = IsPracticeModeNow();

    // Cleanup when leaving practice mode
    if (s_wasActive && !active) {
        ResetPracticeState();
    }
    s_wasActive = active;

    if (!active) return;

    // --- Hotkey edge detection ---
    bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

    // F7: Toggle pause
    if (f7Down && !s_f7WasDown) {
        s_paused = !s_paused;
        s_stepRequested = false;
        LOG_INFO("[Practice] %s", s_paused ? "PAUSED" : "UNPAUSED");
    }

    // F8: Single-frame step (only while paused)
    if (f8Down && !s_f8WasDown && s_paused) {
        s_stepRequested = true;
    }

    // F9: Toggle controller swap
    if (f9Down && !s_f9WasDown) {
        bool newSwap = !InputSystem_GetControlSwap();
        ApplyControlSwap(newSwap);
        LOG_INFO("[Practice] Controller swap %s (controlling %s)",
                 newSwap ? "ON" : "OFF",
                 newSwap ? "P2" : "P1");
    }

    s_f7WasDown = f7Down;
    s_f8WasDown = f8Down;
    s_f9WasDown = f9Down;
}

// ============================================================================
// Freeze query (called by input_sync_hooks)
// ============================================================================

bool PracticeTools_ShouldFreezeFrame() {
    if (!s_initialized || !s_paused) return false;
    if (!IsPracticeModeNow()) return false;

    // If a single-frame step was requested, allow one frame through
    if (s_stepRequested) {
        s_stepRequested = false;
        return false;
    }

    return true;
}

// ============================================================================
// State queries
// ============================================================================

bool PracticeTools_IsPracticeModeActive() {
    return s_initialized && IsPracticeModeNow();
}

bool PracticeTools_IsPaused() {
    return s_paused;
}

bool PracticeTools_IsControlSwapped() {
    return InputSystem_GetControlSwap();
}

// ============================================================================
// ImGui tab contents
// ============================================================================

void PracticeTools_RenderImGui() {
    bool active = PracticeTools_IsPracticeModeActive();

    if (!active) {
        ImGui::TextDisabled("Practice tools are only available in Training mode.");
        ImGui::TextDisabled("Select Training (5th option) from the main menu.");
        return;
    }

    ImGui::Text("Practice Mode Active");
    ImGui::Separator();

    // --- Pause / Step ---
    ImGui::Text("Pause / Frame Step");

    bool paused = s_paused;
    if (ImGui::Checkbox("Paused (F7)", &paused)) {
        s_paused = paused;
        s_stepRequested = false;
    }

    ImGui::SameLine();
    if (ImGui::Button("Step (F8)")) {
        if (s_paused) {
            s_stepRequested = true;
        }
    }
    if (!s_paused) {
        ImGui::SameLine();
        ImGui::TextDisabled("(pause first to step)");
    }

    ImGui::Separator();

    // --- Controller Swap ---
    ImGui::Text("Controller Swap");

    bool swapped = InputSystem_GetControlSwap();
    if (ImGui::Checkbox("Swap P1/P2 Controls (F9)", &swapped)) {
        ApplyControlSwap(swapped);
    }
    ImGui::SameLine();
    ImGui::Text("Controlling: %s", swapped ? "P2" : "P1");

    ImGui::Separator();

    // --- Status ---
    ImGui::Text("Status");
    uint8_t p1Cpu = ReadMemory<uint8_t>(ADDR_P1_CPU_FLAG);
    uint8_t p2Cpu = ReadMemory<uint8_t>(ADDR_P2_CPU_FLAG);
    ImGui::Text("P1 CPU flag: %d  P2 CPU flag: %d", p1Cpu, p2Cpu);
    ImGui::Text("Game Type: %d  Mode: %d  Sub: %d",
                GetGameType(), GetGameMode(), GetSubstate());
}
