/**
 * Alice Senki 2 - Practice Mode Tools
 *
 * Pause/unpause, single-frame advance, controller swap with CPU flag
 * management, hitbox toggle, macro recording, in-game HUD overlay.
 * All features are gated to training mode only
 * (GAMETYPE_TRAINING in MODE_MATCH).
 *
 * Hotkeys are configurable via HotkeyConfig.
 */

#include "training/practice_tools.h"
#include "training/frame_advantage.h"
#include "training/hotkey_config.h"
#include "training/input_macro.h"
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

    InputMacro_Stop();
    FrameAdvantage_ResetState();
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

void PracticeTools_CaptureRuntimeState(PracticeToolsRuntimeState* out) {
    if (!out) {
        return;
    }

    out->paused = s_paused;
    out->stepRequested = s_stepRequested;
    out->stepCounter = (uint32_t)s_stepCounter;
}

void PracticeTools_RestoreRuntimeState(const PracticeToolsRuntimeState* state) {
    if (!state) {
        return;
    }

    s_paused = state->paused;
    s_stepRequested = state->stepRequested;
    s_stepCounter = (int)state->stepCounter;
}

// ============================================================================
// Lifecycle
// ============================================================================

void PracticeTools_Init() {
    s_initialized = true;
    HotkeyConfig_Init();
    FrameAdvantage_Init();
    InputMacro_Init();
    s_paused = false;
    s_stepRequested = false;
    s_stepCounter = 0;
    s_wasActive = false;
    s_toastCount = 0;
    LOG_INFO("[Practice] Practice tools initialized");
}

void PracticeTools_Shutdown() {
    ResetPracticeState();
    InputMacro_Shutdown();
    FrameAdvantage_Shutdown();
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

    PracticeTools_SyncControlSwapState();

    // During macro replay, ensure CPU flags are set BEFORE the frame so
    // the game sees P2 as human during simulation (AI won't overwrite input).
    if (InputMacro_GetState() == MACRO_REPLAYING) {
        WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 0);
        WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 0);
    }

    // --- Advance toast timers ---
    // Use a fixed dt since game runs at 60fps
    UpdateToasts(1.0f / 60.0f);

    // --- Hotkey edge detection (via configurable HotkeyConfig) ---
    HotkeyConfig_Update();

    // Hitbox toggle (disabled during netplay)
    if (HotkeyConfig_JustPressed(HOTKEY_HITBOX_TOGGLE) && !Rollback::RollbackSession_IsActive()) {
        HitboxViewer_ToggleEnabled();
        PushToast(HitboxViewer_IsEnabled() ? "Hitboxes ON" : "Hitboxes OFF",
                  HitboxViewer_IsEnabled() ? IM_COL32(100, 255, 100, 255) : IM_COL32(255, 100, 100, 255));
        LOG_INFO("[Practice] Hitbox viewer %s", HitboxViewer_IsEnabled() ? "ON" : "OFF");
    }

    // Pause toggle
    if (HotkeyConfig_JustPressed(HOTKEY_PAUSE_TOGGLE)) {
        PracticeTools_SetPaused(!s_paused);
    }

    // Frame step (each key-down edge = request exactly 1 game frame advance)
    if (HotkeyConfig_JustPressed(HOTKEY_FRAME_STEP) && s_paused && !s_stepRequested) {
        s_stepRequested = true;
        s_stepCounter++;
    }

    // Controller swap
    if (HotkeyConfig_JustPressed(HOTKEY_CONTROL_SWAP)) {
        bool newSwap = !InputSystem_GetControlSwap();
        ApplyControlSwap(newSwap);
        PushToast(newSwap ? "Controls Swapped (P2)" : "Controls Normal (P1)",
                  IM_COL32(100, 200, 255, 255));
        LOG_INFO("[Practice] Controller swap %s (controlling %s)",
                 newSwap ? "ON" : "OFF",
                 newSwap ? "P2" : "P1");
    }

    // Macro recording
    if (HotkeyConfig_JustPressed(HOTKEY_MACRO_RECORD)) {
        InputMacro_ToggleRecord();
    }

    // Macro playback
    if (HotkeyConfig_JustPressed(HOTKEY_MACRO_PLAY)) {
        InputMacro_TogglePlay();
    }

    // Macro slot cycle
    if (HotkeyConfig_JustPressed(HOTKEY_MACRO_SLOT_NEXT)) {
        InputMacro_NextSlot();
    }
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
    FrameAdvantage_OnFrameAdvanced(ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
    InputMacro_Tick();

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

    // --- Display Options ---
    ImGui::Text("Display Options");

    bool hitbox = HitboxViewer_IsEnabled();
    if (ImGui::Checkbox("Hitbox Viewer (F4)", &hitbox)) {
        HitboxViewer_SetEnabled(hitbox);
    }

    FrameAdvantage_RenderImGui();

    ImGui::Separator();

    // --- Macro Recording ---
    if (ImGui::CollapsingHeader("Macro Recording", ImGuiTreeNodeFlags_DefaultOpen)) {
        InputMacro_RenderImGui();
    }

    ImGui::Separator();

    // --- Hotkey Config ---
    if (ImGui::CollapsingHeader("Hotkey Config")) {
        HotkeyConfig_RenderImGui();
    }

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

    FrameAdvantage_RenderOverlay();
    InputMacro_RenderOverlay();

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

void PracticeTools_ApplyControlSwapState(bool swapped) {
    InputSystem_SetControlSwap(swapped);

    if (IsPracticeModeNow()) {
        ApplyControlSwap(swapped);
    }
}

void PracticeTools_SyncControlSwapState() {
    if (!IsPracticeModeNow()) {
        return;
    }

    // During macro replay, the macro system owns CPU flags.
    // Don't fight it — the macro sets P2 to human so our override input
    // reaches the game instead of the AI overwriting it.
    if (InputMacro_GetState() == MACRO_REPLAYING) {
        return;
    }

    const bool swapped = InputSystem_GetControlSwap();
    const uint8_t expectedP1Cpu = swapped ? 1 : 0;
    const uint8_t expectedP2Cpu = swapped ? 0 : 1;
    const uint8_t p1Cpu = ReadMemory<uint8_t>(ADDR_P1_CPU_FLAG);
    const uint8_t p2Cpu = ReadMemory<uint8_t>(ADDR_P2_CPU_FLAG);

    if (p1Cpu == expectedP1Cpu && p2Cpu == expectedP2Cpu) {
        return;
    }

    LOG_INFO("[Practice] Re-syncing control swap state after game-state change (swap=%d, p1_cpu=%u->%u, p2_cpu=%u->%u)",
             swapped ? 1 : 0,
             (unsigned int)p1Cpu,
             (unsigned int)expectedP1Cpu,
             (unsigned int)p2Cpu,
             (unsigned int)expectedP2Cpu);
    ApplyControlSwap(swapped);
}

bool PracticeTools_HasVisibleHud() {
    return s_initialized &&
           IsPracticeModeNow() &&
           (s_paused || s_toastCount > 0 || FrameAdvantage_HasVisibleOverlay());
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
