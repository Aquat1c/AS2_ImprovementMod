/**
 * Alice Senki 2 - Unified Mod Menu
 * Single ImGui window with tabbed interface
 */

#include "mod_menu.h"
#include "hitbox_viewer.h"
#include "input_system.h"
#include "log_window.h"
#include "mod_main.h"
#include "palette_editor.h"
#include "patches/input_override.h"
#include "patches/tick_hooks.h"
#include "game_console.h"
#include "rollback/determinism_verify.h"
#include "rollback/savestate.h"
#include "testing/scripted_input_runner.h"
#include "training/practice_tools.h"
#include "rollback/rollback_session.h"
#include "imgui.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// Menu State
// ============================================================================

static bool g_menuOpen = false;
static int g_currentTab = 0;
static bool g_showAdvanced = false;
static float g_lastMenuWindowScale = -1.0f;

// Binding state (unified: one capture mode for both KB and gamepad)
static bool g_waitingForBind = false;
static int g_bindingPlayer = 0;
static int g_bindingButton = 0;
static const char* g_bindingName = nullptr;
static int g_bindSource = -1;  // 0=keyboard, 1=gamepad, 2=axis

// Button names for display
static const char* g_buttonNames[] = {
    "Up", "Down", "Left", "Right",
    "A (Light)", "B (Medium)", "C (Heavy)", "D (Special)",
    "Start", "Select", "L1", "R1", "L2", "R2"
};

typedef bool (__cdecl *IsProxyMenuVisible_t)();
typedef void (__cdecl *SetProxyMenuVisible_t)(bool visible);

static HMODULE s_d3d9Module = nullptr;
static IsProxyMenuVisible_t s_isMenuVisible = nullptr;
static SetProxyMenuVisible_t s_setMenuVisible = nullptr;
static bool s_proxyMenuApiLookedUp = false;

static void ResolveProxyMenuApi() {
    if (s_proxyMenuApiLookedUp) {
        return;
    }

    s_proxyMenuApiLookedUp = true;
    s_d3d9Module = GetModuleHandleA("d3d9.dll");
    if (!s_d3d9Module) {
        return;
    }

    s_isMenuVisible = (IsProxyMenuVisible_t)GetProcAddress(s_d3d9Module, "IsMenuVisible");
    s_setMenuVisible = (SetProxyMenuVisible_t)GetProcAddress(s_d3d9Module, "SetMenuVisible");
}

static bool ProxyMenuVisible() {
    ResolveProxyMenuApi();
    return s_isMenuVisible ? s_isMenuVisible() : true;
}

static void ProxySetMenuVisible(bool visible, const char* reason) {
    ResolveProxyMenuApi();
    if (!s_setMenuVisible) {
        LOG_WARN("[ModMenu] Failed to set proxy visibility=%d reason=%s (SetMenuVisible export unavailable)",
                 visible ? 1 : 0,
                 reason ? reason : "unknown");
        return;
    }

    LOG_INFO("[ModMenu] Requesting proxy visibility=%d reason=%s requested_open=%d actual_open=%d",
             visible ? 1 : 0,
             reason ? reason : "unknown",
             g_menuOpen ? 1 : 0,
             (visible && g_menuOpen) ? 1 : 0);
    s_setMenuVisible(visible);
}

static bool IsMenuActuallyOpen() {
    return g_menuOpen && ProxyMenuVisible();
}

static void SetMenuRequestedOpen(bool open, const char* reason) {
    const bool previous = g_menuOpen;
    if (previous == open) {
        return;
    }

    g_menuOpen = open;
    const bool proxyVisible = ProxyMenuVisible();
    LOG_INFO("[ModMenu] Requested open %d -> %d reason=%s proxy_visible=%d actual_open=%d",
             previous ? 1 : 0,
             g_menuOpen ? 1 : 0,
             reason ? reason : "unknown",
             proxyVisible ? 1 : 0,
             IsMenuActuallyOpen() ? 1 : 0);
}

// ============================================================================
// Helper Functions
// ============================================================================

// ============================================================================
// Helper: Get display string for a binding
// ============================================================================

static const char* GetBindingDisplayStr(const KeyBinding_t* bind, char* buf, int bufSize) {
    if (!bind) { snprintf(buf, bufSize, "---"); return buf; }

    // Check keyboard
    if (bind->keyboard_key > 0) {
        const char* name = InputSystem_GetKeyName(bind->keyboard_key);
        snprintf(buf, bufSize, "[KB] %s", name);
        return buf;
    }
    // Check gamepad button
    if (bind->gamepad_button >= 0) {
        const char* name = InputSystem_GetGamepadButtonName(bind->gamepad_button);
        snprintf(buf, bufSize, "[GP] %s", name);
        return buf;
    }
    // Check gamepad axis
    if (bind->gamepad_axis >= 0) {
        const char* name = InputSystem_GetGamepadAxisName(bind->gamepad_axis, bind->axis_direction);
        snprintf(buf, bufSize, "[GP] %s", name);
        return buf;
    }

    snprintf(buf, bufSize, "---");
    return buf;
}

// ============================================================================
// Tab: Input Config (unified binding, auto-save, SDL3 Gamepad)
// ============================================================================

static void TabInputConfig() {
    const float uiScale = ModUI_GetScale();

    // Background input toggle
    bool bgInput = InputSystem_IsBackgroundInputEnabled();
    if (ImGui::Checkbox("Background Input (work when unfocused)", &bgInput)) {
        InputSystem_SetBackgroundInputEnabled(bgInput);
    }

    ImGui::Separator();

    // Binding capture modal
    if (g_waitingForBind) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1),
            "Press any key or gamepad button for P%d %s  (ESC to cancel)",
            g_bindingPlayer + 1, g_bindingName ? g_bindingName : "?");

        // Check ESC to cancel
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            InputSystem_CancelBinding();
            g_waitingForBind = false;
        } else {
            // Try to finish binding (keyboard or gamepad, unified)
            KeyBinding_t captured = {};
            int source = -1;
            if (InputSystem_FinishBinding(&captured, &source)) {
                PlayerBindings_t* bindings = (PlayerBindings_t*)InputSystem_GetBindings(g_bindingPlayer);
                KeyBinding_t* target = InputSystem_GetBindingByIndex(bindings, g_bindingButton);
                if (target) {
                    if (source == 0) {
                        // Keyboard: only update keyboard slot
                        target->keyboard_key = captured.keyboard_key;
                    } else {
                        // Gamepad button or axis: update gamepad slots
                        target->gamepad_button = captured.gamepad_button;
                        target->gamepad_axis = captured.gamepad_axis;
                        target->axis_direction = captured.axis_direction;
                    }
                    g_bindSource = source;
                    // Auto-save
                    InputSystem_SaveConfig("as2_input.cfg");
                }
                g_waitingForBind = false;
            }
        }
        ImGui::Separator();
    }

    // Player tabs
    if (ImGui::BeginTabBar("PlayerInputTabs")) {
        for (int player = 0; player < 2; player++) {
            char tabLabel[32];
            snprintf(tabLabel, sizeof(tabLabel), "Player %d###P%d", player + 1, player);

            if (ImGui::BeginTabItem(tabLabel)) {
                // Gamepad status
                if (InputSystem_HasGamepad(player)) {
                    const char* name = InputSystem_GetGamepadName(player);
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Gamepad: %s",
                                       name ? name : "Connected");
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No gamepad detected");
                }

                // Live input display
                uint16_t input = InputSystem_GetInput(player);
                ImGui::Text("Input: ");
                ImGui::SameLine();
                const char* labels[] = {"U","D","L","R"," ","A","B","C","D"};
                uint16_t masks[]     = {INPUT_UP, INPUT_DOWN, INPUT_LEFT, INPUT_RIGHT,
                                        0, INPUT_A, INPUT_B, INPUT_C, INPUT_D};
                for (int i = 0; i < 9; i++) {
                    if (masks[i] == 0) { ImGui::SameLine(); ImGui::Text(" "); continue; }
                    ImGui::SameLine();
                    ImGui::TextColored(
                        (input & masks[i]) ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1),
                        "%s", labels[i]);
                }

                ImGui::Separator();

                // Bindings table
                PlayerBindings_t* bindings = (PlayerBindings_t*)InputSystem_GetBindings(player);

                // Column headers
                float col1 = ModUI_Scale(100.0f);
                float col2 = col1 + ModUI_Scale(160.0f);
                float col3 = col2 + ModUI_Scale(160.0f);
                ImGui::Text("Action");
                ImGui::SameLine(col1); ImGui::Text("Keyboard");
                ImGui::SameLine(col2); ImGui::Text("Gamepad");
                ImGui::SameLine(col3); ImGui::Text("");
                ImGui::Separator();

                const char* sections[] = {"-- Directions --", "-- Buttons --", "-- System --"};
                int sectionRanges[][2] = {{0,4}, {4,8}, {8,14}};

                for (int s = 0; s < 3; s++) {
                    ImGui::TextDisabled("%s", sections[s]);

                    for (int i = sectionRanges[s][0]; i < sectionRanges[s][1]; i++) {
                        KeyBinding_t* bind = InputSystem_GetBindingByIndex(bindings, i);
                        if (!bind) continue;

                        char id[32];

                        // Action name
                        ImGui::Text("%-12s", g_buttonNames[i]);

                        // Keyboard binding button
                        ImGui::SameLine(col1);
                        bool isCapturingKB = g_waitingForBind && g_bindingPlayer == player
                                             && g_bindingButton == i;

                        char kbStr[48];
                        if (bind->keyboard_key > 0) {
                            snprintf(kbStr, sizeof(kbStr), "%s", InputSystem_GetKeyName(bind->keyboard_key));
                        } else {
                            snprintf(kbStr, sizeof(kbStr), "---");
                        }

                        if (isCapturingKB) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                            snprintf(id, sizeof(id), "Press...##KB%d_%d", player, i);
                        } else {
                            snprintf(id, sizeof(id), "%s##KB%d_%d", kbStr, player, i);
                        }

                        if (ImGui::Button(id, ImVec2(ModUI_Scale(140.0f), 0.0f))) {
                            if (!g_waitingForBind) {
                                InputSystem_StartBinding(player, i);
                                g_waitingForBind = true;
                                g_bindingPlayer = player;
                                g_bindingButton = i;
                                g_bindingName = g_buttonNames[i];
                            }
                        }
                        if (isCapturingKB) ImGui::PopStyleColor();

                        // Gamepad binding display (read-only, set via unified capture)
                        ImGui::SameLine(col2);
                        char gpStr[48];
                        if (bind->gamepad_button >= 0) {
                            snprintf(gpStr, sizeof(gpStr), "%s",
                                     InputSystem_GetGamepadButtonName(bind->gamepad_button));
                        } else if (bind->gamepad_axis >= 0) {
                            snprintf(gpStr, sizeof(gpStr), "%s",
                                     InputSystem_GetGamepadAxisName(bind->gamepad_axis, bind->axis_direction));
                        } else {
                            snprintf(gpStr, sizeof(gpStr), "---");
                        }
                        ImGui::Text("%-20s", gpStr);

                        // Clear button
                        ImGui::SameLine(col3);
                        snprintf(id, sizeof(id), "X##C%d_%d", player, i);
                        if (ImGui::SmallButton(id)) {
                            bind->keyboard_key = 0;
                            bind->gamepad_button = -1;
                            bind->gamepad_axis = -1;
                            bind->axis_direction = 0;
                            InputSystem_SaveConfig("as2_input.cfg");
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear binding");
                    }
                    ImGui::Spacing();
                }

                ImGui::Separator();

                if (ImGui::Button("Reset Defaults")) {
                    InputSystem_ResetDefaults(player);
                    InputSystem_SaveConfig("as2_input.cfg");
                }

                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

// ============================================================================
// Tab: Advanced Debug (consolidated developer tools)
// ============================================================================

static void TabAdvancedDebug() {
    if (ImGui::CollapsingHeader("Game State", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Frame: %d", AS2_GetFrameNumber());
        ImGui::Text("Game Mode: %d  In Match: %s", GetGameMode(), AS2_IsInMatch() ? "Yes" : "No");
        ImGui::Spacing();
        ImGui::Text("P1: HP=%d  Entity=0x%08X", GetP1HP(), GetEntityBase(0));
        ImGui::Text("P2: HP=%d  Entity=0x%08X", GetP2HP(), GetEntityBase(1));
        ImGui::Spacing();
        ImGui::Text("Input: P1=0x%04X  P2=0x%04X", InputSystem_GetInput(0), InputSystem_GetInput(1));

        bool controlSwap = InputSystem_GetControlSwap();
        if (ImGui::Checkbox("Swap P1/P2 Controls", &controlSwap)) {
            InputSystem_SetControlSwap(controlSwap);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("P1 physical input drives P2 character and vice versa.\n"
                              "Used for netplay join-side testing.");
        }
    }

    if (ImGui::CollapsingHeader("Timing")) {
        float globalScale = GetGlobalTickScale();
        if (ImGui::SliderFloat("Global Tick Scale", &globalScale, 0.25f, 8.0f, "%.2fx")) {
            SetGlobalTickScale(globalScale);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Scales the game tick source used by the main-loop frame limiter.\n"
                              "1.00x = normal. >1.00x makes the game run faster.");
        }
    }

    if (ImGui::CollapsingHeader("Input Debug")) {
        RenderInputDebugContent();
    }

    if (ImGui::CollapsingHeader("Game Console")) {
        bool gameDebug = GameConsole_IsEnabled();
        if (ImGui::Checkbox("Capture Game Debug Output", &gameDebug)) {
            GameConsole_SetEnabled(gameDebug);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Captures the game's internal Log_Write calls\n"
                              "and routes them to the mod's log window with [Game] prefix.");
        }
        ImGui::Text("Log_Write calls: %d", GameConsole_GetCallCount());
        if (ImGui::Button("Test Hook")) {
            GameConsole_TestLog();
        }
    }

}

// ============================================================================
// Tab: Log
// ============================================================================

static void TabLog() {
    // Verbose logging toggle
    bool verbose = GetVerboseLogging();
    if (ImGui::Checkbox("Netplay Debug Logging", &verbose)) {
        SetVerboseLogging(verbose);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable detailed netplay diagnostics.\n"
                          "Errors and key session events are always logged.");
    }
    ImGui::Separator();
    
    // Log rendering is handled by LogWindow_RenderContent
    LogWindow_RenderContent();
}

// ============================================================================
// Public API
// ============================================================================

void ModMenu_Init() {
    g_menuOpen = false;
    g_currentTab = 0;
    PaletteEditor_Init();
}

void ModMenu_SetOpen(bool open) {
    SetMenuRequestedOpen(open, "ModMenu_SetOpen");
}

void ModMenu_Toggle() {
    SetMenuRequestedOpen(!g_menuOpen, "ModMenu_Toggle");
}

bool ModMenu_IsRequestedOpen() {
    return g_menuOpen;
}

bool ModMenu_IsOpen() {
    return IsMenuActuallyOpen();
}

void ModMenu_Render() {
    if (!IsMenuActuallyOpen()) return;

    const float uiScale = ModUI_GetScale();
    const ImVec2 defaultPos(ModUI_Scale(10.0f), ModUI_Scale(10.0f));
    const ImVec2 defaultSize(ModUI_Scale(500.0f), ModUI_Scale(440.0f));
    
    ImGui::SetNextWindowPos(defaultPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar;
    
    ImGui::SetNextWindowBgAlpha(0.85f);
    
    bool windowOpen = g_menuOpen;
    if (!ImGui::Begin("Settings", &windowOpen, flags)) {
        ImGui::End();
        if (windowOpen != g_menuOpen) {
            SetMenuRequestedOpen(windowOpen, windowOpen ? "Settings window reopened" : "Settings window closed");
        }
        return;
    }

    const float scaleDelta = uiScale - g_lastMenuWindowScale;
    if (scaleDelta < -0.01f || scaleDelta > 0.01f) {
        ImGui::SetWindowSize(defaultSize, ImGuiCond_Always);
        g_lastMenuWindowScale = uiScale;
    }
    
    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Config")) {
                InputSystem_SaveConfig("as2_input.cfg");
                LOG_INFO("Config saved");
            }
            if (ImGui::MenuItem("Load Config")) {
                InputSystem_LoadConfig("as2_input.cfg");
                LOG_INFO("Config loaded");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Hide Menu", "F1")) {
                ProxySetMenuVisible(false, "Settings/File Hide Menu");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Advanced Mode", nullptr, &g_showAdvanced);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    
    // Main tabs — end-user tabs always visible, developer tabs gated by Advanced Mode
    if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Controls")) {
            TabInputConfig();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            TabLog();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Palette")) {
            PaletteEditor_Render();
            ImGui::EndTabItem();
        }
        if (g_showAdvanced) {
            if (ImGui::BeginTabItem("Debug")) {
                TabAdvancedDebug();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Determinism")) {
                DetVer_RenderImGui();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Savestate")) {
                Savestate_RenderImGui();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Scenarios")) {
                SIR_RenderImGui();
                ImGui::EndTabItem();
            }
        }
        if (!Rollback::RollbackSession_IsActive()) {
            if (ImGui::BeginTabItem("Hitbox")) {
                HitboxViewer_RenderControls();
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("Practice")) {
            PracticeTools_RenderImGui();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    
    ImGui::End();
    if (windowOpen != g_menuOpen) {
        SetMenuRequestedOpen(windowOpen, windowOpen ? "Settings window reopened" : "Settings window closed");
    }
}
