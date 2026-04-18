#include "training/hotkey_config.h"
#include "imgui.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Defaults
// ============================================================================

static const int kDefaultKeys[HOTKEY_COUNT] = {
    VK_F4,          // HOTKEY_HITBOX_TOGGLE
    VK_F7,          // HOTKEY_PAUSE_TOGGLE
    VK_F8,          // HOTKEY_FRAME_STEP
    VK_F9,          // HOTKEY_CONTROL_SWAP
    VK_F10,         // HOTKEY_MACRO_RECORD
    VK_DELETE,      // HOTKEY_MACRO_PLAY
    VK_F12,         // HOTKEY_MACRO_SLOT_NEXT
};

static const char* kActionNames[HOTKEY_COUNT] = {
    "Hitbox Toggle",
    "Pause Toggle",
    "Frame Step",
    "Control Swap",
    "Macro Record",
    "Macro Play/Stop",
    "Macro Slot Next",
};

// ============================================================================
// State
// ============================================================================

static int  s_keys[HOTKEY_COUNT] = {};
static bool s_prevDown[HOTKEY_COUNT] = {};
static bool s_currDown[HOTKEY_COUNT] = {};
static bool s_initialized = false;

// Rebind state: which action is currently listening for a new key (-1 = none)
static int  s_rebindAction = -1;

// ============================================================================
// Key name lookup
// ============================================================================

const char* HotkeyConfig_KeyName(int vk) {
    static char buf[32];

    switch (vk) {
    case 0:           return "None";
    case VK_F1:       return "F1";
    case VK_F2:       return "F2";
    case VK_F3:       return "F3";
    case VK_F4:       return "F4";
    case VK_F5:       return "F5";
    case VK_F6:       return "F6";
    case VK_F7:       return "F7";
    case VK_F8:       return "F8";
    case VK_F9:       return "F9";
    case VK_F10:      return "F10";
    case VK_F11:      return "F11";
    case VK_F12:      return "F12";
    case VK_ESCAPE:   return "Esc";
    case VK_TAB:      return "Tab";
    case VK_SPACE:    return "Space";
    case VK_RETURN:   return "Enter";
    case VK_BACK:     return "Backspace";
    case VK_DELETE:    return "Delete";
    case VK_INSERT:   return "Insert";
    case VK_HOME:     return "Home";
    case VK_END:      return "End";
    case VK_PRIOR:    return "PageUp";
    case VK_NEXT:     return "PageDown";
    case VK_LEFT:     return "Left";
    case VK_RIGHT:    return "Right";
    case VK_UP:       return "Up";
    case VK_DOWN:     return "Down";
    case VK_NUMPAD0:  return "Num0";
    case VK_NUMPAD1:  return "Num1";
    case VK_NUMPAD2:  return "Num2";
    case VK_NUMPAD3:  return "Num3";
    case VK_NUMPAD4:  return "Num4";
    case VK_NUMPAD5:  return "Num5";
    case VK_NUMPAD6:  return "Num6";
    case VK_NUMPAD7:  return "Num7";
    case VK_NUMPAD8:  return "Num8";
    case VK_NUMPAD9:  return "Num9";
    case VK_MULTIPLY: return "Num*";
    case VK_ADD:      return "Num+";
    case VK_SUBTRACT: return "Num-";
    case VK_DECIMAL:  return "Num.";
    case VK_DIVIDE:   return "Num/";
    default:
        // A-Z keys
        if (vk >= 'A' && vk <= 'Z') {
            snprintf(buf, sizeof(buf), "%c", (char)vk);
            return buf;
        }
        // 0-9 keys
        if (vk >= '0' && vk <= '9') {
            snprintf(buf, sizeof(buf), "%c", (char)vk);
            return buf;
        }
        snprintf(buf, sizeof(buf), "VK_%02X", vk);
        return buf;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void HotkeyConfig_Init(void) {
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        s_keys[i] = kDefaultKeys[i];
        s_prevDown[i] = false;
        s_currDown[i] = false;
    }
    s_rebindAction = -1;
    s_initialized = true;
}

// ============================================================================
// API
// ============================================================================

int HotkeyConfig_GetKey(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) return 0;
    return s_keys[action];
}

void HotkeyConfig_SetKey(HotkeyAction action, int vk) {
    if (action < 0 || action >= HOTKEY_COUNT) return;
    s_keys[action] = vk;
}

const char* HotkeyConfig_ActionName(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) return "?";
    return kActionNames[action];
}

void HotkeyConfig_Update(void) {
    if (!s_initialized) return;

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        s_prevDown[i] = s_currDown[i];
        s_currDown[i] = (s_keys[i] != 0) && ((GetAsyncKeyState(s_keys[i]) & 0x8000) != 0);
    }
}

bool HotkeyConfig_JustPressed(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) return false;
    // Suppress hotkeys while rebinding
    if (s_rebindAction >= 0) return false;
    return s_currDown[action] && !s_prevDown[action];
}

// ============================================================================
// Scan for a newly-pressed key during rebind
// ============================================================================

static int ScanForKeyPress(void) {
    // Check F-keys first (most common rebind targets)
    for (int vk = VK_F1; vk <= VK_F12; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    // A-Z
    for (int vk = 'A'; vk <= 'Z'; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    // 0-9
    for (int vk = '0'; vk <= '9'; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    // Numpad
    for (int vk = VK_NUMPAD0; vk <= VK_DIVIDE; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    // Common special keys
    static const int specials[] = {
        VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
        VK_TAB, VK_SPACE, VK_BACK,
    };
    for (int vk : specials) {
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    return 0;
}

// ============================================================================
// ImGui
// ============================================================================

void HotkeyConfig_RenderImGui(void) {
    if (!s_initialized) return;

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        ImGui::PushID(i);

        if (s_rebindAction == i) {
            // Currently listening for a new key
            ImGui::Text("%-18s", kActionNames[i]);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Press a key...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) {
                s_rebindAction = -1;
            }

            int pressed = ScanForKeyPress();
            if (pressed != 0) {
                // Escape cancels without changing
                if (pressed == VK_ESCAPE) {
                    s_rebindAction = -1;
                } else {
                    s_keys[i] = pressed;
                    s_rebindAction = -1;
                }
            }
        } else {
            ImGui::Text("%-18s", kActionNames[i]);
            ImGui::SameLine();
            char label[64];
            snprintf(label, sizeof(label), "[%s]##btn", HotkeyConfig_KeyName(s_keys[i]));
            if (ImGui::SmallButton(label)) {
                s_rebindAction = i;
            }
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset to Defaults")) {
        for (int i = 0; i < HOTKEY_COUNT; i++) {
            s_keys[i] = kDefaultKeys[i];
        }
        s_rebindAction = -1;
    }
}
