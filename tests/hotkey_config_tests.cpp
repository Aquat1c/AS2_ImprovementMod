#include <windows.h>
static bool focused = true;
static HWND TestForeground() { return focused ? (HWND)1 : nullptr; }
static DWORD TestWindowPid(HWND, DWORD *pid) {
    *pid = GetCurrentProcessId();
    return 1;
}
#define GetForegroundWindow TestForeground
#define GetWindowThreadProcessId TestWindowPid
#include "../src/training/hotkey_config.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#include <stdlib.h>
static bool capturing = false;
static int pressedKey = 0, pressedButton = -1;
static void Check(bool ok) {
    if (!ok)
        abort();
}
namespace Ui {
const char *S(Str) { return "test"; }
} // namespace Ui
bool PracticeTools_IsPracticeModeActive() { return false; }
const PlayerBindings_t *InputSystem_GetBindings(int) { return nullptr; }
const KeyBinding_t *InputSystem_GetBindingByIndexConst(const PlayerBindings_t *, int) { return nullptr; }
bool InputSystem_DoBindingsOverlap(const KeyBinding_t *, const KeyBinding_t *) { return false; }
void InputSystem_GetBindingDisplayName(const KeyBinding_t *, char *out, int size) {
    if (size)
        out[0] = 0;
}
bool InputSystem_IsBindingActive() { return capturing; }
void InputSystem_CancelBinding() { capturing = false; }
void InputSystem_StartBinding(int, int) { capturing = true; }
bool InputSystem_FinishBinding(KeyBinding_t *, int *) { return false; }
bool InputSystem_IsBindingDown(int, const KeyBinding_t *b) {
    return (pressedKey && b->keyboard_key == pressedKey) ||
           (pressedButton >= 0 && b->gamepad_button == pressedButton);
}
int main() {
    ResetToDefaults();
    s_initialized = true;
    const auto action = HOTKEY_FULLSCREEN_TOGGLE;
    Check(HOTKEY_COUNT == 13);
    Check(s_bindings[action].keyboard_key == SDL_SCANCODE_F11);
    Check(!HotkeyConfig_ActionIsPracticeOnly(action));
    pressedKey = SDL_SCANCODE_F11;
    HotkeyConfig_Update();
    Check(HotkeyConfig_JustPressed(action));
    HotkeyConfig_Update();
    Check(!HotkeyConfig_JustPressed(action));
    s_bindings[action].keyboard_key = SDL_SCANCODE_F10;
    HotkeyConfig_Update();
    Check(!HotkeyConfig_JustPressed(action));
    pressedKey = SDL_SCANCODE_F10;
    HotkeyConfig_Update();
    Check(HotkeyConfig_JustPressed(action));
    capturing = true;
    pressedKey = 0;
    HotkeyConfig_Update();
    pressedKey = SDL_SCANCODE_F10;
    HotkeyConfig_Update();
    Check(!HotkeyConfig_JustPressed(action));
    capturing = false;
    HotkeyConfig_Update();
    Check(!HotkeyConfig_JustPressed(action));
    focused = false;
    pressedKey = 0;
    HotkeyConfig_Update();
    pressedKey = SDL_SCANCODE_F10;
    HotkeyConfig_Update();
    focused = true;
    HotkeyConfig_Update();
    Check(!HotkeyConfig_JustPressed(action));
    ResetBinding(&s_bindings[action]);
    HotkeyConfig_Update();
    Check(!HotkeyConfig_JustPressed(action));
    s_bindings[action].gamepad_button = 4;
    pressedButton = 4;
    HotkeyConfig_Update();
    Check(HotkeyConfig_JustPressed(action));
    const char *file = "hotkey_test.cfg";
    SaveConfig(file);
    FILE *saved = fopen(file, "rb");
    Check(saved != nullptr);
    Check(fseek(saved, sizeof(HotkeyConfigHeader) + 14 * sizeof(KeyBinding_t), SEEK_SET) == 0);
    KeyBinding_t wire{};
    Check(fread(&wire, sizeof(wire), 1, saved) == 1);
    fclose(saved);
    Check(wire.keyboard_key == 0 && wire.gamepad_button == 4);
    ResetToDefaults();
    Check(LoadConfig(file));
    Check(s_bindings[action].keyboard_key == 0 && s_bindings[action].gamepad_button == 4);
    FILE *f = fopen(file, "wb");
    Check(f != nullptr);
    HotkeyConfigHeader header = {kHotkeyConfigMagic, kHotkeyConfigVersion};
    fwrite(&header, sizeof(header), 1, f);
    fwrite(s_bindings, sizeof(KeyBinding_t), 12, f);
    fclose(f);
    ResetToDefaults();
    Check(LoadConfig(file));
    Check(s_bindings[action].keyboard_key == SDL_SCANCODE_F11);
    remove(file);
    return 0;
}
