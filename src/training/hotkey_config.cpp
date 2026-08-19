#include "training/hotkey_config.h"

#include "training/practice_tools.h"
#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Defaults / Config
// ============================================================================

static const char* kHotkeyConfigFilename = "as2_practice_hotkeys.cfg";
static const uint32_t kHotkeyConfigMagic = 0x48325341u;   // "AS2H"
// 2 added the savestate rows; a version mismatch drops the old file rather than
// reading a shorter binding array into a longer one.
static const uint32_t kHotkeyConfigVersion = 2u;

struct HotkeyConfigHeader {
    uint32_t magic;
    uint32_t version;
};

static const KeyBinding_t kDefaultBindings[HOTKEY_COUNT] = {
    { SDL_SCANCODE_F4,  -1, -1, 0 },  // HOTKEY_HITBOX_TOGGLE
    { SDL_SCANCODE_F7,  -1, -1, 0 },  // HOTKEY_PAUSE_TOGGLE
    { SDL_SCANCODE_F8,  -1, -1, 0 },  // HOTKEY_FRAME_STEP
    { SDL_SCANCODE_F9,  -1, -1, 0 },  // HOTKEY_CONTROL_SWAP
    { SDL_SCANCODE_F5,  -1, -1, 0 },  // HOTKEY_STATE_SAVE
    { SDL_SCANCODE_F6,  -1, -1, 0 },  // HOTKEY_STATE_LOAD
    { SDL_SCANCODE_1,   -1, -1, 0 },  // HOTKEY_POSITION_LOAD
    { SDL_SCANCODE_2,   -1, -1, 0 },  // HOTKEY_POSITION_SAVE
    { SDL_SCANCODE_F10, -1, -1, 0 },  // HOTKEY_MACRO_RECORD
    { SDL_SCANCODE_DELETE, -1, -1, 0 },  // HOTKEY_MACRO_PLAY
    { SDL_SCANCODE_F12, -1, -1, 0 },  // HOTKEY_MACRO_SLOT_NEXT
};

static const char* kActionNames[HOTKEY_COUNT] = {
    "Hitbox Toggle",
    "Pause Toggle",
    "Frame Step",
    "Control Swap",
    "Save State",
    "Load State",
    "Position Load",
    "Position Save",
    "Macro Record",
    "Macro Play/Stop",
    "Macro Slot Next",
};

// Savestates are offered in arcade and versus as well, so gating them on
// training would take away keys that already work there.
static const bool kActionPracticeOnly[HOTKEY_COUNT] = {
    true,   // HOTKEY_HITBOX_TOGGLE
    true,   // HOTKEY_PAUSE_TOGGLE
    true,   // HOTKEY_FRAME_STEP
    true,   // HOTKEY_CONTROL_SWAP
    false,  // HOTKEY_STATE_SAVE
    false,  // HOTKEY_STATE_LOAD
    true,   // HOTKEY_POSITION_LOAD
    true,   // HOTKEY_POSITION_SAVE
    true,   // HOTKEY_MACRO_RECORD
    true,   // HOTKEY_MACRO_PLAY
    true,   // HOTKEY_MACRO_SLOT_NEXT
};

static const char* kControlActionNames[INPUT_ACTION_COUNT] = {
    "Up",
    "Down",
    "Left",
    "Right",
    "A (Light)",
    "B (Medium)",
    "C (Heavy)",
    "D (Special)",
    "Start",
    "Select",
    "L1",
    "R1",
    "L2",
    "R2",
};

// ============================================================================
// State
// ============================================================================

static KeyBinding_t s_bindings[HOTKEY_COUNT] = {};
static bool s_prevDown[HOTKEY_COUNT] = {};
static bool s_currDown[HOTKEY_COUNT] = {};
static bool s_initialized = false;
static int s_rebindAction = -1;

// ============================================================================
// Internal Helpers
// ============================================================================

static bool IsGameWindowFocused() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }

    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return foregroundPid == GetCurrentProcessId();
}

static bool IsImGuiKeyboardEntryActive() {
    if (!ImGui::GetCurrentContext()) {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput || ImGui::IsAnyItemActive();
}

static bool BindingHasAnyComponent(const KeyBinding_t* binding) {
    return binding &&
           (binding->keyboard_key > 0 ||
            binding->gamepad_button >= 0 ||
            binding->gamepad_axis >= 0);
}

static void ResetBinding(KeyBinding_t* binding) {
    if (!binding) {
        return;
    }

    binding->keyboard_key = 0;
    binding->gamepad_button = -1;
    binding->gamepad_axis = -1;
    binding->axis_direction = 0;
}

static void ResetToDefaults() {
    memcpy(s_bindings, kDefaultBindings, sizeof(s_bindings));
    memset(s_prevDown, 0, sizeof(s_prevDown));
    memset(s_currDown, 0, sizeof(s_currDown));
}

static void RemoveOverlappingBindingComponents(KeyBinding_t* target,
                                               const KeyBinding_t* blocker) {
    if (!target || !blocker) {
        return;
    }

    if (target->keyboard_key > 0 && target->keyboard_key == blocker->keyboard_key) {
        target->keyboard_key = 0;
    }
    if (target->gamepad_button >= 0 && target->gamepad_button == blocker->gamepad_button) {
        target->gamepad_button = -1;
    }
    if (target->gamepad_axis >= 0 &&
        target->gamepad_axis == blocker->gamepad_axis &&
        target->axis_direction == blocker->axis_direction) {
        target->gamepad_axis = -1;
        target->axis_direction = 0;
    }
}

static bool BindingHasIndependentComponent(const KeyBinding_t* binding,
                                           const KeyBinding_t* blocker) {
    if (!BindingHasAnyComponent(binding)) {
        return false;
    }

    KeyBinding_t filtered = *binding;
    RemoveOverlappingBindingComponents(&filtered, blocker);
    return BindingHasAnyComponent(&filtered);
}

static bool LoadConfig(const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        return false;
    }

    HotkeyConfigHeader header = {};
    const size_t headerRead = fread(&header, sizeof(header), 1, file);
    if (headerRead != 1 ||
        header.magic != kHotkeyConfigMagic ||
        header.version != kHotkeyConfigVersion) {
        fclose(file);
        remove(filename);
        return false;
    }

    const size_t bindingRead = fread(s_bindings, sizeof(s_bindings), 1, file);
    fclose(file);
    return bindingRead == 1;
}

static void SaveConfig(const char* filename) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        return;
    }

    const HotkeyConfigHeader header = { kHotkeyConfigMagic, kHotkeyConfigVersion };
    fwrite(&header, sizeof(header), 1, file);
    fwrite(s_bindings, sizeof(s_bindings), 1, file);
    fclose(file);
}

static bool BuildControlConflictText(const KeyBinding_t* binding, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }

    out[0] = '\0';
    if (!BindingHasAnyComponent(binding)) {
        return false;
    }

    size_t used = 0;
    int conflictCount = 0;
    for (int player = 0; player < 2; ++player) {
        const PlayerBindings_t* bindings = InputSystem_GetBindings(player);
        if (!bindings) {
            continue;
        }

        for (int actionIndex = 0; actionIndex < INPUT_ACTION_COUNT; ++actionIndex) {
            const KeyBinding_t* controlBinding = InputSystem_GetBindingByIndexConst(bindings, actionIndex);
            if (!controlBinding || !InputSystem_DoBindingsOverlap(binding, controlBinding)) {
                continue;
            }

            const size_t remaining = outSize - used;
            if (remaining <= 1) {
                return conflictCount > 0;
            }

            const int written = snprintf(out + used,
                                         remaining,
                                         "%sP%d %s",
                                         conflictCount > 0 ? ", " : "",
                                         player + 1,
                                         kControlActionNames[actionIndex]);
            if (written < 0) {
                out[outSize - 1] = '\0';
                return conflictCount > 0;
            }

            const size_t advanced = (size_t)written;
            used = (advanced < remaining) ? (used + advanced) : (outSize - 1);
            conflictCount++;
        }
    }

    return conflictCount > 0;
}

static bool BuildUnavailablePresetText(char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }

    out[0] = '\0';

    const KeyBinding_t* loadBinding = HotkeyConfig_GetBinding(HOTKEY_POSITION_LOAD);
    const PlayerBindings_t* p1Bindings = InputSystem_GetBindings(0);
    if (!BindingHasAnyComponent(loadBinding) || !p1Bindings) {
        return false;
    }

    struct UnavailablePreset {
        const KeyBinding_t* binding;
        const char* label;
    } presets[] = {
        { &p1Bindings->up, "Round Start" },
        { &p1Bindings->down, "Mid Screen" },
        { &p1Bindings->right, "Right Corner" },
        { &p1Bindings->left, "Left Corner" },
    };

    size_t used = 0;
    int count = 0;
    for (const UnavailablePreset& preset : presets) {
        if (!InputSystem_DoBindingsOverlap(loadBinding, preset.binding) ||
            BindingHasIndependentComponent(preset.binding, loadBinding)) {
            continue;
        }

        const size_t remaining = outSize - used;
        if (remaining <= 1) {
            return count > 0;
        }

        const int written = snprintf(out + used,
                                     remaining,
                                     "%s%s",
                                     count > 0 ? ", " : "",
                                     preset.label);
        if (written < 0) {
            out[outSize - 1] = '\0';
            return count > 0;
        }

        const size_t advanced = (size_t)written;
        used = (advanced < remaining) ? (used + advanced) : (outSize - 1);
        count++;
    }

    return count > 0;
}

static void RenderHotkeyWarnings(HotkeyAction action) {
    const KeyBinding_t* binding = HotkeyConfig_GetBinding(action);

    if (action == HOTKEY_POSITION_LOAD) {
        char unavailableText[128] = {};
        if (BuildUnavailablePresetText(unavailableText, sizeof(unavailableText))) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "    Shares the same input as: %s. Those preset chords are unavailable.",
                               unavailableText);
        }
    }

    char conflictText[256] = {};
    if (BuildControlConflictText(binding, conflictText, sizeof(conflictText))) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "    Conflicts with controls: %s",
                           conflictText);
    }
}

// ============================================================================
// Key Name Lookup
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
    case VK_DELETE:   return "Delete";
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
        if (vk >= 'A' && vk <= 'Z') {
            snprintf(buf, sizeof(buf), "%c", (char)vk);
            return buf;
        }
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
    ResetToDefaults();
    LoadConfig(kHotkeyConfigFilename);
    s_rebindAction = -1;
    s_initialized = true;
}

// ============================================================================
// API
// ============================================================================

const KeyBinding_t* HotkeyConfig_GetBinding(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) {
        return nullptr;
    }
    return &s_bindings[action];
}

void HotkeyConfig_SetBinding(HotkeyAction action, const KeyBinding_t* binding) {
    if (action < 0 || action >= HOTKEY_COUNT) {
        return;
    }

    if (binding) {
        s_bindings[action] = *binding;
    } else {
        ResetBinding(&s_bindings[action]);
    }
    SaveConfig(kHotkeyConfigFilename);
}

void HotkeyConfig_GetBindingDisplayName(HotkeyAction action, char* out, int outSize) {
    if (!out || outSize <= 0) {
        return;
    }

    if (action < 0 || action >= HOTKEY_COUNT) {
        snprintf(out, outSize, "None");
        return;
    }

    InputSystem_GetBindingDisplayName(&s_bindings[action], out, outSize);
}

void HotkeyConfig_FormatLabel(char* out, int outSize, const char* text,
                              HotkeyAction action) {
    if (!out || outSize <= 0) {
        return;
    }
    if (!text) {
        text = "";
    }

    char bound[64] = {};
    HotkeyConfig_GetBindingDisplayName(action, bound, (int)sizeof(bound));
    if (bound[0]) {
        snprintf(out, outSize, "%s (%s)", text, bound);
    } else {
        snprintf(out, outSize, "%s", text);
    }
}

const char* HotkeyConfig_ActionName(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) return "?";
    return kActionNames[action];
}

// Applies to every action: the window is not ours, a binding capture owns the
// keyboard, or a text field is taking the keystrokes.
static bool IsInputUnavailable(void) {
    return !IsGameWindowFocused() ||
           InputSystem_IsBindingActive() ||
           IsImGuiKeyboardEntryActive();
}

static bool IsActionSuppressed(int action) {
    if (IsInputUnavailable()) {
        return true;
    }
    return kActionPracticeOnly[action] && !PracticeTools_IsPracticeModeActive();
}

bool HotkeyConfig_ActionIsPracticeOnly(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) {
        return true;
    }
    return kActionPracticeOnly[action];
}

bool HotkeyConfig_IsSuppressed(void) {
    return !PracticeTools_IsPracticeModeActive() || IsInputUnavailable();
}

void HotkeyConfig_Update(void) {
    if (!s_initialized) return;

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        const bool down = BindingHasAnyComponent(&s_bindings[i]) &&
                          InputSystem_IsBindingDown(0, &s_bindings[i]);
        if (IsActionSuppressed(i)) {
            s_prevDown[i] = down;
            s_currDown[i] = down;
            continue;
        }

        s_prevDown[i] = s_currDown[i];
        s_currDown[i] = down;
    }
}

bool HotkeyConfig_JustPressed(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) return false;
    if (s_rebindAction >= 0) return false;
    if (IsActionSuppressed(action)) return false;
    return s_currDown[action] && !s_prevDown[action];
}

// ============================================================================
// Rebinding for the in-game settings page
// ============================================================================

void HotkeyConfig_BeginRebind(HotkeyAction action) {
    if (!s_initialized || action < 0 || action >= HOTKEY_COUNT) {
        return;
    }
    if (InputSystem_IsBindingActive()) {
        InputSystem_CancelBinding();
    }
    // Player and button are ignored - the capture result is read back rather
    // than written into the player's control set.
    InputSystem_StartBinding(0, 0);
    s_rebindAction = (int)action;
}

int HotkeyConfig_PollRebind(void) {
    if (s_rebindAction < 0) {
        return -1;
    }
    if (!InputSystem_IsBindingActive()) {
        s_rebindAction = -1;
        return -1;
    }

    KeyBinding_t captured = {};
    int source = -1;
    if (!InputSystem_FinishBinding(&captured, &source)) {
        return 0;
    }

    // Keyboard and pad halves are held at once, so a capture replaces only the
    // half it came from.
    if (source == 0) {
        s_bindings[s_rebindAction].keyboard_key = captured.keyboard_key;
    } else {
        s_bindings[s_rebindAction].gamepad_button = captured.gamepad_button;
        s_bindings[s_rebindAction].gamepad_axis = captured.gamepad_axis;
        s_bindings[s_rebindAction].axis_direction = captured.axis_direction;
    }
    SaveConfig(kHotkeyConfigFilename);
    s_rebindAction = -1;
    return 1;
}

void HotkeyConfig_CancelRebind(void) {
    if (InputSystem_IsBindingActive()) {
        InputSystem_CancelBinding();
    }
    s_rebindAction = -1;
}

bool HotkeyConfig_IsRebinding(void) {
    return s_rebindAction >= 0;
}

int HotkeyConfig_RebindAction(void) {
    return s_rebindAction;
}

void HotkeyConfig_ClearBinding(HotkeyAction action) {
    if (action < 0 || action >= HOTKEY_COUNT) {
        return;
    }
    ResetBinding(&s_bindings[action]);
    SaveConfig(kHotkeyConfigFilename);
}

void HotkeyConfig_ResetDefaults(void) {
    HotkeyConfig_CancelRebind();
    ResetToDefaults();
    SaveConfig(kHotkeyConfigFilename);
}

// ============================================================================
// ImGui
// ============================================================================

void HotkeyConfig_RenderImGui(void) {
    if (!s_initialized) return;

    ImGui::TextDisabled("Practice hotkeys read the P1 device set.");
    ImGui::TextDisabled("Click a row, then press a keyboard key or P1 controller input.");
    ImGui::Spacing();

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        const HotkeyAction action = (HotkeyAction)i;
        ImGui::PushID(i);

        if (s_rebindAction == i) {
            ImGui::Text("%-18s", kActionNames[i]);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f),
                               "Press a key or controller input...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel") || (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
                InputSystem_CancelBinding();
                s_rebindAction = -1;
            } else {
                KeyBinding_t captured = {};
                int source = -1;
                if (InputSystem_FinishBinding(&captured, &source)) {
                    if (source == 0) {
                        s_bindings[i].keyboard_key = captured.keyboard_key;
                    } else {
                        s_bindings[i].gamepad_button = captured.gamepad_button;
                        s_bindings[i].gamepad_axis = captured.gamepad_axis;
                        s_bindings[i].axis_direction = captured.axis_direction;
                    }
                    SaveConfig(kHotkeyConfigFilename);
                    s_rebindAction = -1;
                }
            }
        } else {
            char bindingLabel[128] = {};
            HotkeyConfig_GetBindingDisplayName(action, bindingLabel, (int)sizeof(bindingLabel));

            ImGui::Text("%-18s", kActionNames[i]);
            ImGui::SameLine();

            char label[160] = {};
            snprintf(label, sizeof(label), "[%s]##bind", bindingLabel);
            if (ImGui::SmallButton(label) && !InputSystem_IsBindingActive()) {
                InputSystem_StartBinding(0, 0);
                s_rebindAction = i;
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                ResetBinding(&s_bindings[i]);
                SaveConfig(kHotkeyConfigFilename);
            }
        }

        ImGui::PopID();
        RenderHotkeyWarnings(action);
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset to Defaults")) {
        if (s_rebindAction >= 0) {
            InputSystem_CancelBinding();
            s_rebindAction = -1;
        }
        ResetToDefaults();
        SaveConfig(kHotkeyConfigFilename);
    }
}