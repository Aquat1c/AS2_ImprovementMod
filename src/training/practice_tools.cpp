/**
 * Alice Senki 2 - Practice Mode Tools
 *
 * Pause/unpause, single-frame advance, controller swap with CPU flag
 * management, hitbox toggle, in-game HUD overlay.
 * All features are gated to training mode only
 * (GAMETYPE_TRAINING in MODE_MATCH).
 *
 * Hotkeys:
 *   F4  = Toggle hitbox viewer
 *   F7  = Toggle pause / unpause
 *   F8  = Single-frame advance (while paused; each press = 1 frame)
 *   F9  = Toggle controller swap (P1 <-> P2)
 */

#include "training/practice_tools.h"
#include "core/game_state.h"
#include "patches/memory_utils.h"
#include "ui/hitbox_viewer.h"
#include "input_system.h"
#include "as2_constants.h"
#include "log_window.h"
#include "rollback/rollback_session.h"
#include "imgui.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

// ============================================================================
// Internal state
// ============================================================================

static bool s_initialized = false;
static bool s_paused = false;
static bool s_stepRequested = false;   // One-shot flag: let exactly one game frame advance then re-freeze
static int  s_stepCounter = 0;         // Cumulative frames stepped (increments per F8, always displayed)
static bool s_wasActive = false;       // Previous frame's practice-mode status

// Edge detection for hotkeys (GetAsyncKeyState)
static bool s_f4WasDown = false;
static bool s_f7WasDown = false;
static bool s_f8WasDown = false;
static bool s_f9WasDown = false;

// ============================================================================
// Toast notification system
// ============================================================================

static const int    TOAST_MAX = 4;
static const float  TOAST_DURATION = 1.5f;   // seconds
static const float  TOAST_FADE_START = 1.0f; // start fading at this remaining time

struct Toast {
    char   text[64];
    float  remaining;  // seconds remaining
    ImU32  color;
};

static Toast s_toasts[TOAST_MAX] = {};
static int   s_toastCount = 0;

static void PushToast(const char* text, ImU32 color = IM_COL32(255, 255, 255, 255)) {
    // Shift existing toasts down if full
    if (s_toastCount >= TOAST_MAX) {
        for (int i = 0; i < TOAST_MAX - 1; i++)
            s_toasts[i] = s_toasts[i + 1];
        s_toastCount = TOAST_MAX - 1;
    }
    Toast& t = s_toasts[s_toastCount++];
    snprintf(t.text, sizeof(t.text), "%s", text);
    t.remaining = TOAST_DURATION;
    t.color = color;
}

static void UpdateToasts(float dt) {
    int write = 0;
    for (int i = 0; i < s_toastCount; i++) {
        s_toasts[i].remaining -= dt;
        if (s_toasts[i].remaining > 0.0f) {
            if (write != i) s_toasts[write] = s_toasts[i];
            write++;
        }
    }
    s_toastCount = write;
}

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

    // Swap CPU flags so AI runs for the uncontrolled side.
    // Vanilla training mode: P1=human(0), P2=CPU(1).
    // AI_Update checks entity+172 to decide whether to run AI for each player.
    if (swapped) {
        // Human controls P2: P1 needs AI (dummy), P2 is human
        WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 1);
        WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 0);
    } else {
        // Default: P1 human, P2 gets AI (dummy)
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
    s_stepCounter = 0;

    // Restore default control mapping if swapped
    if (InputSystem_GetControlSwap()) {
        ApplyControlSwap(false);
    }
}

void PracticeTools_SetPaused(bool paused) {
    if (!s_initialized || s_paused == paused) {
        return;
    }

    s_paused = paused;
    s_stepRequested = false;
    s_stepCounter = 0;

    if (IsPracticeModeNow()) {
        PushToast(s_paused ? "PAUSED" : "UNPAUSED",
                  s_paused ? IM_COL32(255, 255, 100, 255) : IM_COL32(100, 255, 100, 255));
        LOG_INFO("[Practice] %s", s_paused ? "PAUSED" : "UNPAUSED");
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void PracticeTools_Init() {
    s_initialized = true;
    s_paused = false;
    s_stepRequested = false;
    s_stepCounter = 0;
    s_wasActive = false;
    s_f4WasDown = false;
    s_f7WasDown = false;
    s_f8WasDown = false;
    s_f9WasDown = false;
    s_toastCount = 0;
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

    // Log practice mode entry (no CPU flag overrides — the vanilla game handles
    // training dummy AI via BYTE2(dword_8E93B8) and AI_Update correctly)
    if (!s_wasActive && active) {
        LOG_INFO("[Practice] Entered practice mode");
    }

    s_wasActive = active;

    if (!active) return;

    // --- Advance toast timers ---
    // Use a fixed dt since game runs at 60fps
    UpdateToasts(1.0f / 60.0f);

    // --- Hotkey edge detection ---
    bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

    // F4: Toggle hitbox viewer (disabled during netplay)
    if (f4Down && !s_f4WasDown && !Rollback::RollbackSession_IsActive()) {
        HitboxViewer_ToggleEnabled();
        PushToast(HitboxViewer_IsEnabled() ? "Hitboxes ON" : "Hitboxes OFF",
                  HitboxViewer_IsEnabled() ? IM_COL32(100, 255, 100, 255) : IM_COL32(255, 100, 100, 255));
        LOG_INFO("[Practice] Hitbox viewer %s", HitboxViewer_IsEnabled() ? "ON" : "OFF");
    }

    // F7: Toggle pause
    if (f7Down && !s_f7WasDown) {
        PracticeTools_SetPaused(!s_paused);
    }

    // F8: Frame step (each key-down edge = request exactly 1 game frame advance)
    // s_stepRequested is a one-shot flag consumed by Hook_AdvanceFrame after the
    // game frame completes (via PracticeTools_OnFrameAdvanced).
    if (f8Down && !s_f8WasDown && s_paused && !s_stepRequested) {
        s_stepRequested = true;
        s_stepCounter++;
    }

    // F9: Toggle controller swap
    if (f9Down && !s_f9WasDown) {
        bool newSwap = !InputSystem_GetControlSwap();
        ApplyControlSwap(newSwap);
        PushToast(newSwap ? "Controls Swapped (P2)" : "Controls Normal (P1)",
                  IM_COL32(100, 200, 255, 255));
        LOG_INFO("[Practice] Controller swap %s (controlling %s)",
                 newSwap ? "ON" : "OFF",
                 newSwap ? "P2" : "P1");
    }

    s_f4WasDown = f4Down;
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

    // One-shot step: when s_stepRequested is set, allow exactly one game frame
    // through.  The flag is cleared by PracticeTools_OnFrameAdvanced() which is
    // called from Hook_AdvanceFrame after g_origAdvanceFrame completes.
    if (s_stepRequested) {
        return false;
    }

    return true;
}

void PracticeTools_OnFrameAdvanced() {
    // Called from Hook_AdvanceFrame after the game frame actually ran.
    // Clears the one-shot step flag so the next frame re-freezes.
    s_stepRequested = false;
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

void PracticeTools_Toast(const char* text, unsigned int color) {
    PushToast(text, (ImU32)color);
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
        PracticeTools_SetPaused(paused);
    }

    ImGui::SameLine();
    if (ImGui::Button("Step (F8)")) {
        if (s_paused && !s_stepRequested) {
            s_stepRequested = true;
            s_stepCounter++;
        }
    }
    if (s_paused && s_stepCounter > 0) {
        ImGui::SameLine();
        ImGui::Text("Step: %d", s_stepCounter);
    } else if (!s_paused) {
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

// ============================================================================
// In-game HUD overlay (rendered via ImGui foreground draw list)
// ============================================================================

void PracticeTools_RenderHUD() {
    if (!s_initialized) return;
    if (!IsPracticeModeNow()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // --- Pause / Frame Step indicator (top-center, below FPS area) ---
    if (s_paused) {
        char buf[64];
        if (s_stepCounter > 0) {
            snprintf(buf, sizeof(buf), "STEP %d", s_stepCounter);
        } else {
            snprintf(buf, sizeof(buf), "PAUSED");
        }

        ImVec2 textSize = ImGui::CalcTextSize(buf);
        float x = (displaySize.x - textSize.x) * 0.5f;
        float y = 32.0f;

        // Background box for readability
        dl->AddRectFilled(
            ImVec2(x - 6, y - 2),
            ImVec2(x + textSize.x + 6, y + textSize.y + 2),
            IM_COL32(0, 0, 0, 180));
        dl->AddText(ImVec2(x, y), IM_COL32(255, 255, 100, 255), buf);
    }

    // --- Toast notifications (bottom-center, stacked upward) ---
    if (s_toastCount > 0) {
        float baseY = displaySize.y - 60.0f;

        for (int i = s_toastCount - 1; i >= 0; i--) {
            const Toast& t = s_toasts[i];

            // Fade alpha in last 0.5s
            float alpha = 1.0f;
            if (t.remaining < (TOAST_DURATION - TOAST_FADE_START)) {
                alpha = t.remaining / (TOAST_DURATION - TOAST_FADE_START);
                if (alpha < 0.0f) alpha = 0.0f;
            }

            ImU32 col = t.color;
            // Apply fade to color alpha
            uint8_t a = (uint8_t)(((col >> 24) & 0xFF) * alpha);
            col = (col & 0x00FFFFFF) | ((ImU32)a << 24);
            ImU32 bgCol = IM_COL32(0, 0, 0, (uint8_t)(160 * alpha));

            ImVec2 textSize = ImGui::CalcTextSize(t.text);
            float x = (displaySize.x - textSize.x) * 0.5f;
            float y = baseY - (float)(s_toastCount - 1 - i) * (textSize.y + 6.0f);

            dl->AddRectFilled(
                ImVec2(x - 6, y - 2),
                ImVec2(x + textSize.x + 6, y + textSize.y + 2),
                bgCol);
            dl->AddText(ImVec2(x, y), col, t.text);
        }
    }
}

bool PracticeTools_HasVisibleHud() {
    return s_initialized &&
           IsPracticeModeNow() &&
           (s_paused || s_toastCount > 0);
}

// ============================================================================
// Command History Hook
// ============================================================================

CmdHistoryUpdate_t g_origCmdHistoryUpdate = nullptr;

char __cdecl Hook_CmdHistoryUpdate(int16_t* matchBase) {
    // When controls are swapped in practice mode, redirect command history
    // to read P2's input data instead of P1's.
    if (IsPracticeModeNow() && InputSystem_GetControlSwap()) {
        matchBase = reinterpret_cast<int16_t*>(
            reinterpret_cast<uint8_t*>(matchBase) + CHARACTER_STRUCT_SIZE);
    }
    return g_origCmdHistoryUpdate ? g_origCmdHistoryUpdate(matchBase) : 0;
}
