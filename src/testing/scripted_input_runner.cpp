/**
 * Alice Senki 2 - Scripted Input Runner (Implementation)
 *
 * Drives deterministic input scenarios via InputSystem_SetOverride().
 * Completely offline — no netplay, session, or rollback dependency.
 *
 * Integration with determinism verification:
 *   - Start scenario -> run -> capture trace A
 *   - Restart scenario -> rerun -> capture trace B
 *   - Compare A vs B for desync detection
 *
 * Input injection path:
 *   InputSystem_SetOverride(player, input)  writes to the override
 *   slot that the joystick hook consults on the next poll, which
 *   then flows into the game's per-player input buffer.
 *
 * Later reuse / launcher compatibility:
 *   The scenario names and runner state are queryable via SIR_GetStatus()
 *   and SIR_GetScenario(). An external launcher/dashboard can consume
 *   these through shared memory or log export in a future pass.
 */

#include "testing/scripted_input_runner.h"
#include "testing/test_scenarios.h"
#include "input_system.h"
#include "mod_main.h"
#include "log_window.h"
#include "imgui.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// Internal state
// ============================================================================

static bool         g_sirInitialized = false;

// Scenario registry (loaded from built-in constructors at init)
#define SIR_MAX_SCENARIOS 16
static SIR_Scenario g_scenarios[SIR_MAX_SCENARIOS];
static int          g_scenarioCount = 0;
static int          g_selectedIndex = -1;

// Runner state
static bool         g_active   = false;
static bool         g_looping  = false;
static SIR_RunMode  g_mode     = SIR_MODE_LOCAL_VS;
static int          g_frame    = 0;        // Current frame within scenario
static uint16_t     g_lastP1   = 0;
static uint16_t     g_lastP2   = 0;

// ============================================================================
// Helpers
// ============================================================================

static const SIR_Scenario* ActiveScenario(void) {
    if (g_selectedIndex < 0 || g_selectedIndex >= g_scenarioCount)
        return nullptr;
    return &g_scenarios[g_selectedIndex];
}

// Compute the combined input mask for a given player at a given scenario frame.
// Multiple entries can overlap; their masks are OR'd together.
static uint16_t ComputeInput(const SIR_Scenario* sc, int frame, int player) {
    uint16_t result = 0;
    for (int i = 0; i < sc->entryCount; i++) {
        const SIR_InputEntry* e = &sc->entries[i];
        if (e->player == player && frame >= e->frameStart && frame <= e->frameEnd) {
            result |= e->input;
        }
    }
    return result;
}

// Apply input override for a player. Clears the override if not driven.
static void InjectInput(int player, uint16_t input) {
    InputSystem_SetOverride(player, input);
}

static void ClearInjection(int player) {
    InputSystem_ClearOverride(player);
}

// ============================================================================
// Lifecycle
// ============================================================================

void SIR_Init(void) {
    if (g_sirInitialized) return;

    g_scenarioCount = 0;
    int builtinCount = Scenarios_GetBuiltinCount();
    for (int i = 0; i < builtinCount && g_scenarioCount < SIR_MAX_SCENARIOS; i++) {
        if (Scenarios_GetBuiltin(i, &g_scenarios[g_scenarioCount])) {
            g_scenarioCount++;
        }
    }

    g_selectedIndex = -1;
    g_active = false;
    g_looping = false;
    g_mode = SIR_MODE_LOCAL_VS;
    g_frame = 0;
    g_lastP1 = 0;
    g_lastP2 = 0;
    g_sirInitialized = true;

    LOG_INFO("[SIR] Scripted Input Runner initialized (%d scenarios)", g_scenarioCount);
}

void SIR_Shutdown(void) {
    if (g_active) {
        SIR_Stop();
    }
    g_sirInitialized = false;
}

// ============================================================================
// Per-frame update
// ============================================================================

void SIR_OnFrame(void) {
    if (!g_active) return;

    const SIR_Scenario* sc = ActiveScenario();
    if (!sc) {
        SIR_Stop();
        return;
    }

    // Only inject during active gameplay (MODE_MATCH substate 3)
    if (!AS2_IsInGameplay()) {
        return;
    }

    // End of scenario?
    if (g_frame >= sc->totalFrames) {
        if (g_looping) {
            g_frame = 0;
            LOG_INFO("[SIR] Scenario '%s' looping from frame 0", sc->name);
        } else {
            LOG_INFO("[SIR] Scenario '%s' completed (%d frames)", sc->name, sc->totalFrames);
            SIR_Stop();
            return;
        }
    }

    // Compute P1 input
    uint16_t p1 = ComputeInput(sc, g_frame, 0);
    g_lastP1 = p1;
    InjectInput(0, p1);

    // P2 handling depends on mode
    switch (g_mode) {
    case SIR_MODE_LOCAL_VS:
        // P2 gets scripted input if scenario provides it, else idle (0)
        {
            uint16_t p2 = sc->hasP2 ? ComputeInput(sc, g_frame, 1) : 0;
            g_lastP2 = p2;
            InjectInput(1, p2);
        }
        break;

    case SIR_MODE_VS_CPU:
        // P2 is game-owned CPU — do NOT inject P2
        g_lastP2 = 0;
        ClearInjection(1);
        break;

    case SIR_MODE_DUAL_SCRIPT:
        // Both players scripted
        {
            uint16_t p2 = ComputeInput(sc, g_frame, 1);
            g_lastP2 = p2;
            InjectInput(1, p2);
        }
        break;
    }

    g_frame++;
}

// ============================================================================
// Control API
// ============================================================================

void SIR_SelectScenario(int index) {
    if (index < 0 || index >= g_scenarioCount) {
        g_selectedIndex = -1;
        return;
    }
    // Stop current run if switching scenarios
    if (g_active) SIR_Stop();
    g_selectedIndex = index;
}

bool SIR_Start(void) {
    if (g_selectedIndex < 0 || g_selectedIndex >= g_scenarioCount) {
        LOG_WARN("[SIR] Cannot start — no scenario selected");
        return false;
    }

    const SIR_Scenario* sc = ActiveScenario();
    g_frame = 0;
    g_lastP1 = 0;
    g_lastP2 = 0;
    g_active = true;

    LOG_INFO("[SIR] Started scenario '%s' (mode=%d, loop=%s, frames=%d)",
             sc->name, (int)g_mode, g_looping ? "on" : "off", sc->totalFrames);
    return true;
}

void SIR_Stop(void) {
    if (!g_active) return;

    // Clear all overrides so the player regains control
    ClearInjection(0);
    ClearInjection(1);

    const SIR_Scenario* sc = ActiveScenario();
    LOG_INFO("[SIR] Stopped at frame %d/%d ('%s')",
             g_frame, sc ? sc->totalFrames : 0, sc ? sc->name : "???");

    g_active = false;
    g_lastP1 = 0;
    g_lastP2 = 0;
}

void SIR_Restart(void) {
    SIR_Stop();
    g_frame = 0;
    SIR_Start();
}

void SIR_SetMode(SIR_RunMode mode) {
    g_mode = mode;
}

SIR_RunMode SIR_GetMode(void) {
    return g_mode;
}

void SIR_SetLooping(bool loop) {
    g_looping = loop;
}

bool SIR_IsLooping(void) {
    return g_looping;
}

// ============================================================================
// Scenario registry
// ============================================================================

int SIR_GetScenarioCount(void) {
    return g_scenarioCount;
}

const SIR_Scenario* SIR_GetScenario(int index) {
    if (index < 0 || index >= g_scenarioCount) return nullptr;
    return &g_scenarios[index];
}

int SIR_GetSelectedIndex(void) {
    return g_selectedIndex;
}

// ============================================================================
// Status query
// ============================================================================

SIR_Status SIR_GetStatus(void) {
    SIR_Status s;
    memset(&s, 0, sizeof(s));
    s.active       = g_active;
    s.looping      = g_looping;
    s.mode         = g_mode;
    s.currentFrame = g_frame;
    s.lastP1Input  = g_lastP1;
    s.lastP2Input  = g_lastP2;

    const SIR_Scenario* sc = ActiveScenario();
    if (sc) {
        s.totalFrames  = sc->totalFrames;
        s.scenarioName = sc->name;
    }
    return s;
}

bool SIR_IsActive(void) {
    return g_active;
}

// ============================================================================
// ImGui Panel
// ============================================================================

static const char* ModeName(SIR_RunMode m) {
    switch (m) {
    case SIR_MODE_LOCAL_VS:    return "Local VS (P1+P2 scripted)";
    case SIR_MODE_VS_CPU:      return "VS CPU (P1 scripted)";
    case SIR_MODE_DUAL_SCRIPT: return "Dual Script (P1+P2 independent)";
    }
    return "???";
}

static void InputBitsDisplay(const char* label, uint16_t input) {
    ImGui::Text("%s: 0x%04X", label, input);
    ImGui::SameLine();
    ImGui::TextDisabled("[%s%s%s%s %s%s%s%s%s%s]",
        (input & INPUT_UP)    ? "U" : ".",
        (input & INPUT_DOWN)  ? "D" : ".",
        (input & INPUT_LEFT)  ? "L" : ".",
        (input & INPUT_RIGHT) ? "R" : ".",
        (input & INPUT_A)     ? "A" : ".",
        (input & INPUT_B)     ? "B" : ".",
        (input & INPUT_C)     ? "C" : ".",
        (input & INPUT_D)     ? "D" : ".",
        (input & INPUT_START) ? "S" : ".",
        (input & INPUT_SELECT)? "E" : ".");
}

void SIR_RenderImGui(void) {
    // ---------- Scenario Selection ----------
    ImGui::Text("Scenario:");
    ImGui::SameLine();

    const char* preview = (g_selectedIndex >= 0 && g_selectedIndex < g_scenarioCount)
                          ? g_scenarios[g_selectedIndex].name
                          : "<none>";

    if (ImGui::BeginCombo("##scenario", preview)) {
        for (int i = 0; i < g_scenarioCount; i++) {
            bool selected = (i == g_selectedIndex);
            if (ImGui::Selectable(g_scenarios[i].name, selected)) {
                SIR_SelectScenario(i);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Show scenario info
    if (g_selectedIndex >= 0 && g_selectedIndex < g_scenarioCount) {
        const SIR_Scenario* sc = &g_scenarios[g_selectedIndex];
        ImGui::TextDisabled("  %d frames, %d entries, P2: %s",
                            sc->totalFrames, sc->entryCount,
                            sc->hasP2 ? "scripted" : "idle/CPU");
    }

    ImGui::Separator();

    // ---------- Mode ----------
    ImGui::Text("Mode:");
    int modeInt = (int)g_mode;
    ImGui::RadioButton("Local VS",    &modeInt, (int)SIR_MODE_LOCAL_VS);
    ImGui::SameLine();
    ImGui::RadioButton("VS CPU",      &modeInt, (int)SIR_MODE_VS_CPU);
    ImGui::SameLine();
    ImGui::RadioButton("Dual Script", &modeInt, (int)SIR_MODE_DUAL_SCRIPT);
    g_mode = (SIR_RunMode)modeInt;

    // ---------- Loop ----------
    ImGui::Checkbox("Loop", &g_looping);

    ImGui::Separator();

    // ---------- Controls ----------
    bool canStart = (g_selectedIndex >= 0) && !g_active;
    bool canStop  = g_active;

    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("Start")) {
        SIR_Start();
    }
    if (!canStart) ImGui::EndDisabled();

    ImGui::SameLine();

    if (!canStop) ImGui::BeginDisabled();
    if (ImGui::Button("Stop")) {
        SIR_Stop();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        SIR_Restart();
    }
    if (!canStop) ImGui::EndDisabled();

    ImGui::Separator();

    // ---------- Status ----------
    if (g_active) {
        const SIR_Scenario* sc = ActiveScenario();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "RUNNING");
        ImGui::SameLine();
        ImGui::Text(" '%s'", sc ? sc->name : "???");

        // Progress bar
        float progress = sc ? (float)g_frame / (float)sc->totalFrames : 0.0f;
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "Frame %d / %d",
                 g_frame, sc ? sc->totalFrames : 0);
        ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);

        ImGui::Text("Mode: %s", ModeName(g_mode));

        InputBitsDisplay("P1 Input", g_lastP1);
        InputBitsDisplay("P2 Input", g_lastP2);

        if (!AS2_IsInGameplay()) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                               "Waiting for gameplay (Mode 8 Sub 3)...");
        }
    } else {
        ImGui::TextDisabled("Not running");
    }

    ImGui::Separator();

    // ---------- Determinism hint ----------
    ImGui::TextWrapped(
        "Tip: Use with Determinism tab to verify reproducibility. "
        "Run a scenario, capture trace A. Restart + rerun, capture trace B. Compare."
    );
}
