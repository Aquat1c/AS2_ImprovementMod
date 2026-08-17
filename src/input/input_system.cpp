/**
 * Alice Senki 2 - Input System Implementation
 * SDL3 for keyboard + gamepads (SDL3 Gamepad API covers XInput, DirectInput, DualSense, etc.)
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "input_system.h"
#include "input/gamepad_worker.h"
#include "net/churn_pause.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Internal State
// ============================================================================

static bool g_initialized = false;
static InputState_t g_inputState[2] = {};
static PlayerBindings_t g_bindings[2] = {};

// SDL3 Gamepad state — indexed by player slot (0=P1, 1=P2).
// Slots are stable: a controller that disconnects and reconnects returns to
// its original slot (matched by GUID). Each slot is either live or nullptr.
static SDL_Gamepad* g_playerGamepad[2] = {};       // open handle for each player
static SDL_GUID     g_playerGUID[2] = {};           // hardware GUID for reconnect matching
static bool         g_playerGUIDValid[2] = {};      // whether g_playerGUID[i] is populated

// Background worker owns SDL_OpenGamepad / SDL_CloseGamepad. Main thread keeps
// slot assignment and reads only from live handles (never queued-for-close).
static constexpr int MAX_PENDING_CLOSE_RETRY = 8;
static SDL_Gamepad* g_pendingCloseRetry[MAX_PENDING_CLOSE_RETRY] = {};
static int          g_pendingCloseRetryCount = 0;
static bool         g_gamepadSubsystemInitialized = false;
static bool         g_gamepadSubsystemFailed = false;
static DWORD g_nextDeferredGamepadLogTick = 0;

// DXLib's startup font/cache code is fragile while Steam Input devices are
// present. Avoid touching SDL's gamepad backends until this handle is live.
static constexpr uintptr_t ADDR_GAME_DEFAULT_FONT_HANDLE = 0x009CC064;

// Override state for netplay
static bool g_overrideActive[2] = {};
static uint16_t g_overrideInput[2] = {};

// Background input mode
static bool g_backgroundInputEnabled = false;
static bool g_windowActiveStateKnown = false;
static bool g_lastWindowActive = false;

// Control swap
static bool g_controlSwap = false;

// Pause blocking during netplay match
static bool g_pauseBlocked = false;

// Key binding capture state
static bool g_bindingMode = false;
static int g_bindingPlayer = 0;
static int g_bindingButton = 0;
static int g_bindingCooldown = 0;  // Frames to suppress input after binding completes
static const int BINDING_COOLDOWN_FRAMES = 30;  // ~0.5 seconds at 60fps

// Netplay input storage
static bool g_netplayInputActive[2] = {};
static uint16_t g_netplayInput[2] = {};

// ============================================================================
// Input Repeat System
// ============================================================================

#define INPUT_REPEAT_DELAY_MENU 5

struct ButtonRepeatState {
    int cooldown;
    bool wasPressed;
};

static ButtonRepeatState g_repeatState[2][10] = {};

static int ButtonMaskToIndex(uint16_t button) {
    switch (button) {
        case INPUT_UP:     return 0;
        case INPUT_DOWN:   return 1;
        case INPUT_LEFT:   return 2;
        case INPUT_RIGHT:  return 3;
        case INPUT_A:      return 4;
        case INPUT_B:      return 5;
        case INPUT_C:      return 6;
        case INPUT_D:      return 7;
        case INPUT_START:  return 8;
        case INPUT_SELECT: return 9;
        default:           return -1;
    }
}

// ============================================================================
// Default Bindings (SDL3 Gamepad button/axis enum values)
// ============================================================================

static void SetDefaultBindings(PlayerBindings_t* b, int player) {
    memset(b, 0, sizeof(PlayerBindings_t));

    // Initialize all gamepad fields to "unbound"
    KeyBinding_t* flat = &b->up;
    for (int i = 0; i < INPUT_ACTION_COUNT; i++) {
        flat[i].gamepad_button = -1;
        flat[i].gamepad_axis = -1;
        flat[i].axis_direction = 0;
    }

    if (player == 0) {
        // P1: Arrow keys + ZXCV + AS + Enter/Backspace
        // Gamepad: D-pad + left stick, face buttons, shoulders, triggers
        b->up    = {SDL_SCANCODE_UP,        SDL_GAMEPAD_BUTTON_DPAD_UP,        SDL_GAMEPAD_AXIS_LEFTY, -1};
        b->down  = {SDL_SCANCODE_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_AXIS_LEFTY,  1};
        b->left  = {SDL_SCANCODE_LEFT,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,      SDL_GAMEPAD_AXIS_LEFTX, -1};
        b->right = {SDL_SCANCODE_RIGHT,     SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     SDL_GAMEPAD_AXIS_LEFTX,  1};
        b->a     = {SDL_SCANCODE_Z,         SDL_GAMEPAD_BUTTON_WEST,           -1, 0};
        b->b     = {SDL_SCANCODE_X,         SDL_GAMEPAD_BUTTON_SOUTH,          -1, 0};
        b->c     = {SDL_SCANCODE_C,         SDL_GAMEPAD_BUTTON_EAST,           -1, 0};
        b->d     = {SDL_SCANCODE_V,         SDL_GAMEPAD_BUTTON_NORTH,          -1, 0};
        b->l1    = {SDL_SCANCODE_A,         SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  -1, 0};
        b->r1    = {SDL_SCANCODE_S,         SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1, 0};
        b->start = {SDL_SCANCODE_RETURN,    SDL_GAMEPAD_BUTTON_START,          -1, 0};
        b->select= {SDL_SCANCODE_BACKSPACE, SDL_GAMEPAD_BUTTON_BACK,           -1, 0};
        b->l2    = {0,                      -1,  SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  1};
        b->r2    = {0,                      -1,  SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1};
    } else {
        // P2: Numpad for directions, numpad digits for actions
        b->up    = {SDL_SCANCODE_KP_8,      SDL_GAMEPAD_BUTTON_DPAD_UP,        SDL_GAMEPAD_AXIS_LEFTY, -1};
        b->down  = {SDL_SCANCODE_KP_2,      SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_AXIS_LEFTY,  1};
        b->left  = {SDL_SCANCODE_KP_4,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,      SDL_GAMEPAD_AXIS_LEFTX, -1};
        b->right = {SDL_SCANCODE_KP_6,      SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     SDL_GAMEPAD_AXIS_LEFTX,  1};
        b->a     = {SDL_SCANCODE_KP_7,      SDL_GAMEPAD_BUTTON_WEST,           -1, 0};
        b->b     = {SDL_SCANCODE_KP_9,      SDL_GAMEPAD_BUTTON_SOUTH,          -1, 0};
        b->c     = {SDL_SCANCODE_KP_1,      SDL_GAMEPAD_BUTTON_EAST,           -1, 0};
        b->d     = {SDL_SCANCODE_KP_3,      SDL_GAMEPAD_BUTTON_NORTH,          -1, 0};
        b->l1    = {SDL_SCANCODE_KP_DIVIDE, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  -1, 0};
        b->r1    = {SDL_SCANCODE_KP_MULTIPLY, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1, 0};
        b->start = {SDL_SCANCODE_KP_ENTER,  SDL_GAMEPAD_BUTTON_START,          -1, 0};
        b->select= {SDL_SCANCODE_KP_0,      SDL_GAMEPAD_BUTTON_BACK,           -1, 0};
        b->l2    = {0,                      -1,  SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  1};
        b->r2    = {0,                      -1,  SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1};
    }
}

// ============================================================================
// Window Focus Check
// ============================================================================

static bool IsGameWindowActive() {
    if (g_backgroundInputEnabled) return true;

    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;

    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return (foregroundPid == GetCurrentProcessId());
}

static void LogGameWindowActiveState(bool active) {
    HWND foreground = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &foregroundPid);
    }

    Rollback::NetplayLog_Write("INPUT", -1,
        "Window active state changed: active=%d background=%d fg=0x%p fg_pid=%lu self_pid=%lu",
        active ? 1 : 0,
        g_backgroundInputEnabled ? 1 : 0,
        static_cast<void*>(foreground),
        static_cast<unsigned long>(foregroundPid),
        static_cast<unsigned long>(GetCurrentProcessId()));
}

// ============================================================================
// SDL3 Gamepad Management
// ============================================================================

static bool ReadGameDefaultFontHandle(uint32_t* outHandle) {
    uint32_t value = 0;
    __try {
        value = *reinterpret_cast<volatile uint32_t*>(ADDR_GAME_DEFAULT_FONT_HANDLE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
    }

    if (outHandle) {
        *outHandle = value;
    }
    return value != 0 && value != 0xFFFFFFFFu;
}

static void ConfigureGamepadHints() {
    // Avoid pumping SDL device enumeration on unrelated USB/audio plug events.
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_SENSORS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "0");
    LOG_INFO("[Input] SDL gamepad hints: AUTO_UPDATE_JOYSTICKS=%s AUTO_UPDATE_SENSORS=%s HIDAPI_STEAM=%s",
             SDL_GetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS)
                 ? SDL_GetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS)
                 : "(unset)",
             SDL_GetHint(SDL_HINT_AUTO_UPDATE_SENSORS)
                 ? SDL_GetHint(SDL_HINT_AUTO_UPDATE_SENSORS)
                 : "(unset)",
             SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM)
                 ? SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM)
                 : "(unset)");
}

static void LogGamepadDetails(int slot, SDL_Gamepad* gp, const char* eventName) {
    if (!gp) return;

    SDL_GamepadType type = SDL_GetGamepadType(gp);
    SDL_GamepadType realType = SDL_GetRealGamepadType(gp);
    const char* typeName = SDL_GetGamepadStringForType(type);
    const char* realTypeName = SDL_GetGamepadStringForType(realType);
    const char* name = SDL_GetGamepadName(gp);
    const char* path = SDL_GetGamepadPath(gp);

    LOG_INFO("[Input] Gamepad %s slot=%d id=%u name='%s' path='%s' vid=0x%04X pid=0x%04X ver=0x%04X type=%s real=%s",
             eventName ? eventName : "opened",
             slot,
             (unsigned)SDL_GetGamepadID(gp),
             name ? name : "(null)",
             path ? path : "(null)",
             (unsigned)SDL_GetGamepadVendor(gp),
             (unsigned)SDL_GetGamepadProduct(gp),
             (unsigned)SDL_GetGamepadProductVersion(gp),
             typeName ? typeName : "unknown",
             realTypeName ? realTypeName : "unknown");
}

static void SyncSlotSnapshotToWorker() {
    Input::GamepadWorkerSlotSnapshot snapshot{};
    for (int slot = 0; slot < 2; slot++) {
        snapshot.occupied[slot] = g_playerGamepad[slot] != nullptr;
        snapshot.guid_valid[slot] = g_playerGUIDValid[slot];
        snapshot.guid[slot] = g_playerGUID[slot];
        if (g_playerGamepad[slot]) {
            snapshot.live_instance_id[slot] = SDL_GetGamepadID(g_playerGamepad[slot]);
        }
    }
    Input::GamepadWorker_UpdateSlotSnapshot(&snapshot);
}

static bool QueueGamepadClose(SDL_Gamepad* gamepad) {
    if (!gamepad) {
        return true;
    }

    if (Input::GamepadWorker_RequestClose(gamepad)) {
        return true;
    }

    Net::ChurnPause_NotifyPendingCloseRetry();

    if (g_pendingCloseRetryCount < MAX_PENDING_CLOSE_RETRY) {
        g_pendingCloseRetry[g_pendingCloseRetryCount++] = gamepad;
        LOG_WARN("[Input] Gamepad close queue full; deferring retry handle=0x%p count=%d",
                 static_cast<void*>(gamepad),
                 g_pendingCloseRetryCount);
        return true;
    }

    LOG_WARN("[Input] Gamepad close retry overflow; closing on main thread handle=0x%p",
             static_cast<void*>(gamepad));
    Net::ChurnPause_NotifyMainThreadBlockingIo();
    SDL_CloseGamepad(gamepad);
    return false;
}

static void FlushPendingCloseRetries() {
    if (g_pendingCloseRetryCount > 0) {
        Net::ChurnPause_NotifyPendingCloseRetry();
    }

    for (int i = 0; i < g_pendingCloseRetryCount; ) {
        SDL_Gamepad* handle = g_pendingCloseRetry[i];
        if (!handle) {
            g_pendingCloseRetry[i] = g_pendingCloseRetry[--g_pendingCloseRetryCount];
            g_pendingCloseRetry[g_pendingCloseRetryCount] = nullptr;
            continue;
        }

        if (Input::GamepadWorker_RequestClose(handle)) {
            g_pendingCloseRetry[i] = g_pendingCloseRetry[--g_pendingCloseRetryCount];
            g_pendingCloseRetry[g_pendingCloseRetryCount] = nullptr;
            continue;
        }

        ++i;
    }
}

static void ApplyGamepadAttachResult(const Input::GamepadWorkerOpenResult& result) {
    if (result.slot < 0 || result.slot > 1) {
        if (result.gamepad) {
            QueueGamepadClose(result.gamepad);
        }
        return;
    }

    if (!result.gamepad) {
        LOG_WARN("[Input] SDL_OpenGamepad failed for slot=%d id=%u: %s",
                 result.slot,
                 (unsigned)result.instance_id,
                 SDL_GetError());
        return;
    }

    if (g_playerGamepad[result.slot]) {
        Rollback::NetplayLog_Write("INPUT", -1,
            "Discarding duplicate gamepad attach: slot=%d id=%u existing=0x%p new=0x%p",
            result.slot,
            (unsigned)result.instance_id,
            static_cast<void*>(g_playerGamepad[result.slot]),
            static_cast<void*>(result.gamepad));
        QueueGamepadClose(result.gamepad);
        return;
    }

    g_playerGamepad[result.slot] = result.gamepad;
    if (result.guid_valid) {
        g_playerGUID[result.slot] = result.guid;
        g_playerGUIDValid[result.slot] = true;
    }
    LogGamepadDetails(result.slot, result.gamepad, "connected");
    Net::ChurnPause_NotifyLocalGamepadChurn(
        Net::ChurnPauseReason::GamepadConnect,
        400);
    Rollback::NetplayLog_Write("INPUT", -1,
        "Gamepad attach applied: slot=%d id=%u handle=0x%p worker_ms=%lu",
        result.slot,
        (unsigned)result.instance_id,
        static_cast<void*>(result.gamepad),
        (unsigned long)result.duration_ms);
}

static void DrainGamepadWorkerResults() {
    Input::GamepadWorkerResult workerResult{};
    while (Input::GamepadWorker_TryPopResult(&workerResult)) {
        ApplyGamepadAttachResult(workerResult.attach);
    }
}

static void DetachGamepadSlot(int slot, SDL_JoystickID instanceId, const char* reason) {
    if (slot < 0 || slot > 1 || !g_playerGamepad[slot]) {
        return;
    }

    LogGamepadDetails(slot, g_playerGamepad[slot], reason ? reason : "disconnected");
    SDL_Gamepad* handle = g_playerGamepad[slot];
    g_playerGamepad[slot] = nullptr;
    QueueGamepadClose(handle);
    Net::ChurnPause_NotifyLocalGamepadChurn(
        Net::ChurnPauseReason::GamepadDisconnect,
        600);
    Rollback::NetplayLog_Write("INPUT", -1,
        "Gamepad disconnect queued close: slot=%d id=%u handle=0x%p reason=%s",
        slot,
        (unsigned)instanceId,
        static_cast<void*>(handle),
        reason ? reason : "?");
}

static void CheckDisconnectedGamepads() {
    for (int p = 0; p < 2; p++) {
        SDL_Gamepad* gp = g_playerGamepad[p];
        if (!gp) {
            continue;
        }

        if (SDL_GamepadConnected(gp)) {
            continue;
        }

        const SDL_JoystickID instanceId = SDL_GetGamepadID(gp);
        DetachGamepadSlot(p, instanceId, "disconnected");
        Input::GamepadWorker_CancelOpen(instanceId);
    }
}

static void UpdateLiveGamepadState() {
    if (!g_gamepadSubsystemInitialized) {
        return;
    }

    static DWORD s_lastSlowUpdateLogMs = 0;
    const DWORD startMs = GetTickCount();
    SDL_UpdateGamepads();
    const DWORD durationMs = GetTickCount() - startMs;
    if (durationMs >= 16) {
        const DWORD now = GetTickCount();
        if (s_lastSlowUpdateLogMs == 0 ||
            (DWORD)(now - s_lastSlowUpdateLogMs) >= 1000) {
            s_lastSlowUpdateLogMs = now;
            Rollback::NetplayLog_Write("INPUT", -1,
                "SDL_UpdateGamepads slow: duration_ms=%lu",
                (unsigned long)durationMs);
        }
    }
}

static void HandleGamepadEvents() {
    if (!g_gamepadSubsystemInitialized) return;

    SyncSlotSnapshotToWorker();
    DrainGamepadWorkerResults();
    FlushPendingCloseRetries();
    CheckDisconnectedGamepads();
    SyncSlotSnapshotToWorker();
    DrainGamepadWorkerResults();
    FlushPendingCloseRetries();
}

static bool EnsureGamepadSubsystemReady() {
    if (g_gamepadSubsystemInitialized) return true;
    if (g_gamepadSubsystemFailed) return false;

    uint32_t fontHandle = 0;
    if (!ReadGameDefaultFontHandle(&fontHandle)) {
        DWORD now = GetTickCount();
        if (now >= g_nextDeferredGamepadLogTick) {
            LOG_INFO("[Input] Deferring SDL gamepad init until game default font is ready (handle=0x%08X)",
                     (unsigned)fontHandle);
            g_nextDeferredGamepadLogTick = now + 1000;
        }
        return false;
    }

    ConfigureGamepadHints();
    LOG_INFO("[Input] Initializing SDL gamepad subsystem after default font ready (handle=0x%08X)",
             (unsigned)fontHandle);

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        LOG_ERROR("[Input] SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed: %s", SDL_GetError());
        g_gamepadSubsystemFailed = true;
        return false;
    }

    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
    LOG_INFO("[Input] SDL gamepad/joystick OS events disabled (manual update + worker scan)");

    g_gamepadSubsystemInitialized = true;
    if (!Input::GamepadWorker_Init()) {
        LOG_ERROR("[Input] Failed to start gamepad worker thread");
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        g_gamepadSubsystemFailed = true;
        return false;
    }
    SyncSlotSnapshotToWorker();
    Input::GamepadWorker_RequestBootstrap();
    return true;
}

// ============================================================================
// SDL3 Gamepad Reading
// ============================================================================

static const int16_t STICK_DEADZONE = 8000;
static const int16_t TRIGGER_THRESHOLD = 8000;

static uint16_t ReadGamepadPlayer(int player) {
    if (!g_gamepadSubsystemInitialized) return 0;
    if (player < 0 || player >= 2) return 0;
    SDL_Gamepad* gp = g_playerGamepad[player];
    if (!gp) return 0;
    if (!IsGameWindowActive()) return 0;

    const PlayerBindings_t* b = &g_bindings[player];
    uint16_t input = 0;

    // Generic binding check: button OR axis threshold
    auto checkBinding = [&](const KeyBinding_t& bind, uint16_t flag) {
        if (bind.gamepad_button >= 0) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)bind.gamepad_button))
                input |= flag;
        }
        if (bind.gamepad_axis >= 0) {
            int16_t val = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)bind.gamepad_axis);
            // Triggers use 0..32767; sticks use -32768..32767
            if (bind.axis_direction > 0 && val > STICK_DEADZONE)  input |= flag;
            if (bind.axis_direction < 0 && val < -STICK_DEADZONE) input |= flag;
        }
    };

    checkBinding(b->up, INPUT_UP);
    checkBinding(b->down, INPUT_DOWN);
    checkBinding(b->left, INPUT_LEFT);
    checkBinding(b->right, INPUT_RIGHT);
    checkBinding(b->a, INPUT_A);
    checkBinding(b->b, INPUT_B);
    checkBinding(b->c, INPUT_C);
    checkBinding(b->d, INPUT_D);
    checkBinding(b->start, INPUT_START);
    checkBinding(b->select, INPUT_SELECT);
    checkBinding(b->l1, INPUT_L1);
    checkBinding(b->r1, INPUT_R1);
    checkBinding(b->l2, INPUT_L2);
    checkBinding(b->r2, INPUT_R2);

    // SOCD cleaning
    if ((input & INPUT_UP) && (input & INPUT_DOWN))
        input &= ~(INPUT_UP | INPUT_DOWN);
    if ((input & INPUT_LEFT) && (input & INPUT_RIGHT))
        input &= ~(INPUT_LEFT | INPUT_RIGHT);

    return input;
}

// ============================================================================
// Keyboard Reading — Win32 GetAsyncKeyState (reliable without own window)
// ============================================================================

int InputSystem_ScancodeToVirtualKey(int sc) {
    // SDL_SCANCODE values → Win32 Virtual Key codes
    // Covers ALL standard SDL3 scancodes for complete keyboard binding support.
    switch (sc) {
        // Letters (SDL_SCANCODE_A=4 .. SDL_SCANCODE_Z=29)
        case SDL_SCANCODE_A: return 'A';  case SDL_SCANCODE_B: return 'B';
        case SDL_SCANCODE_C: return 'C';  case SDL_SCANCODE_D: return 'D';
        case SDL_SCANCODE_E: return 'E';  case SDL_SCANCODE_F: return 'F';
        case SDL_SCANCODE_G: return 'G';  case SDL_SCANCODE_H: return 'H';
        case SDL_SCANCODE_I: return 'I';  case SDL_SCANCODE_J: return 'J';
        case SDL_SCANCODE_K: return 'K';  case SDL_SCANCODE_L: return 'L';
        case SDL_SCANCODE_M: return 'M';  case SDL_SCANCODE_N: return 'N';
        case SDL_SCANCODE_O: return 'O';  case SDL_SCANCODE_P: return 'P';
        case SDL_SCANCODE_Q: return 'Q';  case SDL_SCANCODE_R: return 'R';
        case SDL_SCANCODE_S: return 'S';  case SDL_SCANCODE_T: return 'T';
        case SDL_SCANCODE_U: return 'U';  case SDL_SCANCODE_V: return 'V';
        case SDL_SCANCODE_W: return 'W';  case SDL_SCANCODE_X: return 'X';
        case SDL_SCANCODE_Y: return 'Y';  case SDL_SCANCODE_Z: return 'Z';

        // Numbers (SDL_SCANCODE_1=30 .. SDL_SCANCODE_0=39)
        case SDL_SCANCODE_1: return '1';  case SDL_SCANCODE_2: return '2';
        case SDL_SCANCODE_3: return '3';  case SDL_SCANCODE_4: return '4';
        case SDL_SCANCODE_5: return '5';  case SDL_SCANCODE_6: return '6';
        case SDL_SCANCODE_7: return '7';  case SDL_SCANCODE_8: return '8';
        case SDL_SCANCODE_9: return '9';  case SDL_SCANCODE_0: return '0';

        // Common special keys
        case SDL_SCANCODE_RETURN:    return VK_RETURN;
        case SDL_SCANCODE_ESCAPE:    return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB:       return VK_TAB;
        case SDL_SCANCODE_SPACE:     return VK_SPACE;

        // Punctuation / symbols (US layout positions)
        case SDL_SCANCODE_MINUS:        return VK_OEM_MINUS;   // -/_
        case SDL_SCANCODE_EQUALS:       return VK_OEM_PLUS;    // =/+
        case SDL_SCANCODE_LEFTBRACKET:  return VK_OEM_4;       // [/{
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;       // ]/}
        case SDL_SCANCODE_BACKSLASH:    return VK_OEM_5;       // \/|
        case SDL_SCANCODE_NONUSHASH:    return VK_OEM_5;       // Non-US # (same position)
        case SDL_SCANCODE_SEMICOLON:    return VK_OEM_1;       // ;/:
        case SDL_SCANCODE_APOSTROPHE:   return VK_OEM_7;       // '/"
        case SDL_SCANCODE_GRAVE:        return VK_OEM_3;       // `/~
        case SDL_SCANCODE_COMMA:        return VK_OEM_COMMA;   // ,/<
        case SDL_SCANCODE_PERIOD:       return VK_OEM_PERIOD;  // ./>
        case SDL_SCANCODE_SLASH:        return VK_OEM_2;       // //?
        case SDL_SCANCODE_NONUSBACKSLASH: return VK_OEM_102;   // Non-US \/| (102-key)

        // Caps Lock
        case SDL_SCANCODE_CAPSLOCK:  return VK_CAPITAL;

        // Function keys F1-F24
        case SDL_SCANCODE_F1:  return VK_F1;   case SDL_SCANCODE_F2:  return VK_F2;
        case SDL_SCANCODE_F3:  return VK_F3;   case SDL_SCANCODE_F4:  return VK_F4;
        case SDL_SCANCODE_F5:  return VK_F5;   case SDL_SCANCODE_F6:  return VK_F6;
        case SDL_SCANCODE_F7:  return VK_F7;   case SDL_SCANCODE_F8:  return VK_F8;
        case SDL_SCANCODE_F9:  return VK_F9;   case SDL_SCANCODE_F10: return VK_F10;
        case SDL_SCANCODE_F11: return VK_F11;  case SDL_SCANCODE_F12: return VK_F12;
        case SDL_SCANCODE_F13: return VK_F13;  case SDL_SCANCODE_F14: return VK_F14;
        case SDL_SCANCODE_F15: return VK_F15;  case SDL_SCANCODE_F16: return VK_F16;
        case SDL_SCANCODE_F17: return VK_F17;  case SDL_SCANCODE_F18: return VK_F18;
        case SDL_SCANCODE_F19: return VK_F19;  case SDL_SCANCODE_F20: return VK_F20;
        case SDL_SCANCODE_F21: return VK_F21;  case SDL_SCANCODE_F22: return VK_F22;
        case SDL_SCANCODE_F23: return VK_F23;  case SDL_SCANCODE_F24: return VK_F24;

        // Print Screen, Scroll Lock, Pause
        case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
        case SDL_SCANCODE_SCROLLLOCK:  return VK_SCROLL;
        case SDL_SCANCODE_PAUSE:       return VK_PAUSE;

        // Navigation cluster
        case SDL_SCANCODE_INSERT:   return VK_INSERT;
        case SDL_SCANCODE_HOME:     return VK_HOME;
        case SDL_SCANCODE_PAGEUP:   return VK_PRIOR;
        case SDL_SCANCODE_DELETE:   return VK_DELETE;
        case SDL_SCANCODE_END:      return VK_END;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;

        // Arrow keys
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_LEFT:  return VK_LEFT;
        case SDL_SCANCODE_DOWN:  return VK_DOWN;
        case SDL_SCANCODE_UP:    return VK_UP;

        // Numpad
        case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK;
        case SDL_SCANCODE_KP_DIVIDE:    return VK_DIVIDE;
        case SDL_SCANCODE_KP_MULTIPLY:  return VK_MULTIPLY;
        case SDL_SCANCODE_KP_MINUS:     return VK_SUBTRACT;
        case SDL_SCANCODE_KP_PLUS:      return VK_ADD;
        case SDL_SCANCODE_KP_ENTER:     return VK_RETURN;
        case SDL_SCANCODE_KP_1:         return VK_NUMPAD1;
        case SDL_SCANCODE_KP_2:         return VK_NUMPAD2;
        case SDL_SCANCODE_KP_3:         return VK_NUMPAD3;
        case SDL_SCANCODE_KP_4:         return VK_NUMPAD4;
        case SDL_SCANCODE_KP_5:         return VK_NUMPAD5;
        case SDL_SCANCODE_KP_6:         return VK_NUMPAD6;
        case SDL_SCANCODE_KP_7:         return VK_NUMPAD7;
        case SDL_SCANCODE_KP_8:         return VK_NUMPAD8;
        case SDL_SCANCODE_KP_9:         return VK_NUMPAD9;
        case SDL_SCANCODE_KP_0:         return VK_NUMPAD0;
        case SDL_SCANCODE_KP_PERIOD:    return VK_DECIMAL;
        case SDL_SCANCODE_KP_EQUALS:    return VK_OEM_NEC_EQUAL;  // Numpad =
        case SDL_SCANCODE_KP_COMMA:     return VK_SEPARATOR;      // Numpad ,

        // Modifier keys
        case SDL_SCANCODE_LCTRL:  return VK_LCONTROL;
        case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
        case SDL_SCANCODE_LALT:   return VK_LMENU;
        case SDL_SCANCODE_LGUI:   return VK_LWIN;
        case SDL_SCANCODE_RCTRL:  return VK_RCONTROL;
        case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
        case SDL_SCANCODE_RALT:   return VK_RMENU;
        case SDL_SCANCODE_RGUI:   return VK_RWIN;

        // Application / Menu key
        case SDL_SCANCODE_APPLICATION: return VK_APPS;

        // System keys
        case SDL_SCANCODE_POWER: return VK_SLEEP;  // Closest match
        case SDL_SCANCODE_SLEEP: return VK_SLEEP;

        // Media keys
        case SDL_SCANCODE_MUTE:          return VK_VOLUME_MUTE;
        case SDL_SCANCODE_VOLUMEUP:      return VK_VOLUME_UP;
        case SDL_SCANCODE_VOLUMEDOWN:    return VK_VOLUME_DOWN;
        case SDL_SCANCODE_MEDIA_NEXT_TRACK:      return VK_MEDIA_NEXT_TRACK;
        case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK:  return VK_MEDIA_PREV_TRACK;
        case SDL_SCANCODE_MEDIA_STOP:            return VK_MEDIA_STOP;
        case SDL_SCANCODE_MEDIA_PLAY:            return VK_MEDIA_PLAY_PAUSE;
        case SDL_SCANCODE_MEDIA_PLAY_PAUSE:      return VK_MEDIA_PLAY_PAUSE;
        case SDL_SCANCODE_MEDIA_SELECT:          return VK_LAUNCH_MEDIA_SELECT;

        // Browser / app launch keys
        case SDL_SCANCODE_AC_SEARCH:    return VK_BROWSER_SEARCH;
        case SDL_SCANCODE_AC_HOME:      return VK_BROWSER_HOME;
        case SDL_SCANCODE_AC_BACK:      return VK_BROWSER_BACK;
        case SDL_SCANCODE_AC_FORWARD:   return VK_BROWSER_FORWARD;
        case SDL_SCANCODE_AC_STOP:      return VK_BROWSER_STOP;
        case SDL_SCANCODE_AC_REFRESH:   return VK_BROWSER_REFRESH;
        case SDL_SCANCODE_AC_BOOKMARKS: return VK_BROWSER_FAVORITES;

        // International keys
        case SDL_SCANCODE_INTERNATIONAL1: return VK_OEM_5;      // Yen / backslash (JP)
        case SDL_SCANCODE_INTERNATIONAL2: return VK_OEM_AUTO;   // Katakana/Hiragana
        case SDL_SCANCODE_INTERNATIONAL3: return 0xDC;          // Yen (JP) - VK_OEM_5 alternate
        case SDL_SCANCODE_INTERNATIONAL4: return VK_CONVERT;    // Henkan
        case SDL_SCANCODE_INTERNATIONAL5: return VK_NONCONVERT; // Muhenkan
        case SDL_SCANCODE_LANG1:          return VK_HANGUL;     // Hangul/Kana
        case SDL_SCANCODE_LANG2:          return VK_HANJA;      // Hanja/Eisu

        // Select / Execute / Help / Clear (rare but present in SDL)
        case SDL_SCANCODE_SELECT:  return VK_SELECT;
        case SDL_SCANCODE_EXECUTE: return VK_EXECUTE;
        case SDL_SCANCODE_HELP:    return VK_HELP;
        case SDL_SCANCODE_CLEAR:   return VK_CLEAR;
        case SDL_SCANCODE_MENU:    return VK_APPS;

        default: return 0;  // Unknown scancode
    }
}

static uint16_t ReadKeyboardPlayer(int player) {
    if (player < 0 || player > 1) return 0;
    if (!IsGameWindowActive()) return 0;

    const PlayerBindings_t* b = &g_bindings[player];
    uint16_t input = 0;

    auto checkKey = [&](const KeyBinding_t& bind, uint16_t flag) {
        if (bind.keyboard_key > 0) {
            int vk = InputSystem_ScancodeToVirtualKey(bind.keyboard_key);
            if (vk > 0 && (GetAsyncKeyState(vk) & 0x8000))
                input |= flag;
        }
    };

    checkKey(b->up, INPUT_UP);
    checkKey(b->down, INPUT_DOWN);
    checkKey(b->left, INPUT_LEFT);
    checkKey(b->right, INPUT_RIGHT);
    checkKey(b->a, INPUT_A);
    checkKey(b->b, INPUT_B);
    checkKey(b->c, INPUT_C);
    checkKey(b->d, INPUT_D);
    checkKey(b->start, INPUT_START);
    checkKey(b->select, INPUT_SELECT);
    checkKey(b->l1, INPUT_L1);
    checkKey(b->r1, INPUT_R1);
    checkKey(b->l2, INPUT_L2);
    checkKey(b->r2, INPUT_R2);

    return input;
}

// ============================================================================
// Public API — Init / Shutdown / Update
// ============================================================================

bool InputSystem_Init(void) {
    if (g_initialized) return true;

    SetDefaultBindings(&g_bindings[0], 0);
    SetDefaultBindings(&g_bindings[1], 1);
    InputSystem_LoadConfig("as2_input.cfg");

    g_initialized = true;
    g_gamepadSubsystemInitialized = false;
    g_gamepadSubsystemFailed = false;
    memset(g_playerGamepad, 0, sizeof(g_playerGamepad));
    memset(g_playerGUID, 0, sizeof(g_playerGUID));
    memset(g_playerGUIDValid, 0, sizeof(g_playerGUIDValid));
    memset(g_pendingCloseRetry, 0, sizeof(g_pendingCloseRetry));
    g_pendingCloseRetryCount = 0;
    g_nextDeferredGamepadLogTick = 0;
    printf("[Input] Input system initialized (Win32 keyboard; SDL3 gamepad deferred)\n");
    LOG_INFO("[Input] Input system initialized (Win32 keyboard active; SDL3 gamepad deferred until default font ready)");
    return true;
}

void InputSystem_Shutdown(void) {
    if (!g_initialized) return;
    InputSystem_SaveConfig("as2_input.cfg");

    DrainGamepadWorkerResults();

    for (int i = 0; i < 2; i++) {
        if (g_playerGamepad[i]) {
            QueueGamepadClose(g_playerGamepad[i]);
            g_playerGamepad[i] = nullptr;
        }
    }

    FlushPendingCloseRetries();
    Input::GamepadWorker_Shutdown();

    for (int i = 0; i < g_pendingCloseRetryCount; i++) {
        if (g_pendingCloseRetry[i]) {
            SDL_CloseGamepad(g_pendingCloseRetry[i]);
            g_pendingCloseRetry[i] = nullptr;
        }
    }
    g_pendingCloseRetryCount = 0;

    if (g_gamepadSubsystemInitialized) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    g_gamepadSubsystemInitialized = false;
    g_gamepadSubsystemFailed = false;
    g_initialized = false;
    printf("[Input] Input system shutdown\n");
}

void InputSystem_Update(void) {
    if (!g_initialized) return;

    // Tick down binding cooldown
    if (g_bindingCooldown > 0) g_bindingCooldown--;

    // Gamepad/Steam-facing SDL startup is intentionally delayed until after
    // the game's early DXLib font/cache setup is complete.
    if (EnsureGamepadSubsystemReady()) {
        UpdateLiveGamepadState();
        HandleGamepadEvents();
    }

    const bool windowActive = IsGameWindowActive();
    if (!g_windowActiveStateKnown || g_lastWindowActive != windowActive) {
        LogGameWindowActiveState(windowActive);
        g_lastWindowActive = windowActive;
        g_windowActiveStateKnown = true;
    }

    for (int p = 0; p < 2; p++) {
        g_inputState[p].previous = g_inputState[p].current;

        if (g_overrideActive[p]) {
            g_inputState[p].current = g_overrideInput[p];
        } else if (g_bindingMode || g_bindingCooldown > 0) {
            // Suppress game input while in binding capture mode or cooldown
            g_inputState[p].current = 0;
        } else {
            uint16_t kbInput = ReadKeyboardPlayer(p);
            uint16_t gpInput = ReadGamepadPlayer(p);
            g_inputState[p].current = kbInput | gpInput;

            // SOCD cleaning (keyboard + gamepad combined)
            uint16_t combined = g_inputState[p].current;
            if ((combined & INPUT_UP) && (combined & INPUT_DOWN))
                g_inputState[p].current &= ~(INPUT_UP | INPUT_DOWN);
            if ((combined & INPUT_LEFT) && (combined & INPUT_RIGHT))
                g_inputState[p].current &= ~(INPUT_LEFT | INPUT_RIGHT);
        }

        g_inputState[p].pressed  =  g_inputState[p].current & ~g_inputState[p].previous;
        g_inputState[p].released = ~g_inputState[p].current &  g_inputState[p].previous;
    }
}

// ============================================================================
// Getters
// ============================================================================

uint16_t InputSystem_GetInput(int player) {
    if (player < 0 || player > 1) return 0;
    return g_inputState[player].current;
}

const InputState_t* InputSystem_GetState(int player) {
    if (player < 0 || player > 1) return nullptr;
    return &g_inputState[player];
}

bool InputSystem_IsPressed(int player, uint16_t button) {
    if (player < 0 || player > 1) return false;
    return (g_inputState[player].current & button) != 0;
}

bool InputSystem_JustPressed(int player, uint16_t button) {
    if (player < 0 || player > 1) return false;
    return (g_inputState[player].pressed & button) != 0;
}

bool InputSystem_InputReady(int player, uint16_t button) {
    if (player < 0 || player > 1) return false;

    int btnIndex = ButtonMaskToIndex(button);
    if (btnIndex < 0) return false;

    ButtonRepeatState* state = &g_repeatState[player][btnIndex];
    bool isPressed = (g_inputState[player].current & button) != 0;

    if (isPressed) {
        if (!state->wasPressed) {
            state->cooldown = INPUT_REPEAT_DELAY_MENU;
            state->wasPressed = true;
            return true;
        }
        if (state->cooldown > 0) {
            state->cooldown--;
            return false;
        }
        state->cooldown = INPUT_REPEAT_DELAY_MENU;
        return true;
    }

    state->wasPressed = false;
    state->cooldown = 0;
    return false;
}

void InputSystem_ResetRepeatState(int player) {
    if (player < 0 || player > 1) return;
    memset(g_repeatState[player], 0, sizeof(g_repeatState[player]));
}

// ============================================================================
// Bindings management
// ============================================================================

const PlayerBindings_t* InputSystem_GetBindings(int player) {
    if (player < 0 || player > 1) return nullptr;
    return &g_bindings[player];
}

void InputSystem_SetBindings(int player, const PlayerBindings_t* bindings) {
    if (player < 0 || player > 1 || !bindings) return;
    memcpy(&g_bindings[player], bindings, sizeof(PlayerBindings_t));
}

void InputSystem_ResetDefaults(int player) {
    if (player < 0 || player > 1) return;
    SetDefaultBindings(&g_bindings[player], player);
}

// ============================================================================
// Override / Background / Swap
// ============================================================================

// TEMP DIAG (2026-08-17 input-eater hunt): rate-limited caller logging for
// override set/clear on player 0 — the harness AI's combat inputs reach
// SetOverride but the dispatcher samples zeros; name the clearer.
#include <intrin.h>
static uint32_t s_ovrSetLogCount = 0;
static uint32_t s_ovrClearLogCount = 0;

void InputSystem_SetOverride(int player, uint16_t input) {
    if (player < 0 || player > 1) return;
    if (player == 0 && input != 0) {
        ++s_ovrSetLogCount;
        if (s_ovrSetLogCount <= 20 || (s_ovrSetLogCount % 600) == 0) {
            printf("[Input] SetOverride #%u p0=0x%04X caller=%p\n",
                   s_ovrSetLogCount, input, _ReturnAddress());
        }
    }
    g_overrideActive[player] = true;
    g_overrideInput[player] = input;
}

void InputSystem_ClearOverride(int player) {
    if (player < 0 || player > 1) return;
    if (player == 0 && g_overrideActive[0]) {
        ++s_ovrClearLogCount;
        if (s_ovrClearLogCount <= 20 || (s_ovrClearLogCount % 600) == 0) {
            printf("[Input] ClearOverride #%u p0 (was 0x%04X) caller=%p\n",
                   s_ovrClearLogCount, g_overrideInput[0], _ReturnAddress());
        }
    }
    g_overrideActive[player] = false;
    g_overrideInput[player] = 0;
}

void InputSystem_SetBackgroundInputEnabled(bool enabled) { g_backgroundInputEnabled = enabled; }
bool InputSystem_IsBackgroundInputEnabled()              { return g_backgroundInputEnabled; }

void InputSystem_SetControlSwap(bool enabled) { g_controlSwap = enabled; }
bool InputSystem_GetControlSwap(void)         { return g_controlSwap; }
void InputSystem_ToggleControlSwap(void)      { g_controlSwap = !g_controlSwap; }

// ============================================================================
// Netplay input storage
// ============================================================================

void InputSystem_SetNetplayInput(int player, uint16_t input) {
    if (player < 0 || player > 1) return;
    g_netplayInputActive[player] = true;
    g_netplayInput[player] = input;
}

uint16_t InputSystem_GetNetplayInput(int player) {
    if (player < 0 || player > 1) return 0;
    return g_netplayInput[player];
}

void InputSystem_ClearNetplayInput(int player) {
    if (player < 0 || player > 1) return;
    g_netplayInputActive[player] = false;
    g_netplayInput[player] = 0;
}

bool InputSystem_IsNetplayInputActive(int player) {
    if (player < 0 || player > 1) return false;
    return g_netplayInputActive[player];
}

// ============================================================================
// Gamepad queries
// ============================================================================

bool InputSystem_HasGamepad(int player) {
    if (!g_gamepadSubsystemInitialized) return false;
    return (player >= 0 && player < 2 && g_playerGamepad[player] != nullptr);
}

bool InputSystem_HasXInput(int player) {
    return InputSystem_HasGamepad(player);
}

const char* InputSystem_GetGamepadName(int player) {
    if (!g_gamepadSubsystemInitialized) return nullptr;
    if (player < 0 || player >= 2 || !g_playerGamepad[player]) return nullptr;
    return SDL_GetGamepadName(g_playerGamepad[player]);
}

// ============================================================================
// Pause Menu Suppression
// ============================================================================

void InputSystem_SetPauseBlocked(bool blocked) { g_pauseBlocked = blocked; }
bool InputSystem_IsPauseBlocked(void)          { return g_pauseBlocked; }

// ============================================================================
// Unified Binding Capture
// ============================================================================

void InputSystem_StartBinding(int player, int buttonIndex) {
    g_bindingMode = true;
    g_bindingCooldown = 0;
    g_bindingPlayer = player;
    g_bindingButton = buttonIndex;
}

bool InputSystem_IsBindingActive(void) {
    return g_bindingMode || g_bindingCooldown > 0;
}

bool InputSystem_FinishBinding(KeyBinding_t* outBinding, int* outSource) {
    if (!g_bindingMode || !outBinding) return false;

    // 1. Check keyboard via Win32 GetAsyncKeyState
    //    (SDL_GetKeyboardState doesn't work — SDL doesn't own the game window)
    //    We scan all SDL scancodes via their VK mapping and check GetAsyncKeyState.
    static const struct { int scancode; int vk; } kScanVK[] = {
        // Letters
        {SDL_SCANCODE_A, 'A'}, {SDL_SCANCODE_B, 'B'}, {SDL_SCANCODE_C, 'C'},
        {SDL_SCANCODE_D, 'D'}, {SDL_SCANCODE_E, 'E'}, {SDL_SCANCODE_F, 'F'},
        {SDL_SCANCODE_G, 'G'}, {SDL_SCANCODE_H, 'H'}, {SDL_SCANCODE_I, 'I'},
        {SDL_SCANCODE_J, 'J'}, {SDL_SCANCODE_K, 'K'}, {SDL_SCANCODE_L, 'L'},
        {SDL_SCANCODE_M, 'M'}, {SDL_SCANCODE_N, 'N'}, {SDL_SCANCODE_O, 'O'},
        {SDL_SCANCODE_P, 'P'}, {SDL_SCANCODE_Q, 'Q'}, {SDL_SCANCODE_R, 'R'},
        {SDL_SCANCODE_S, 'S'}, {SDL_SCANCODE_T, 'T'}, {SDL_SCANCODE_U, 'U'},
        {SDL_SCANCODE_V, 'V'}, {SDL_SCANCODE_W, 'W'}, {SDL_SCANCODE_X, 'X'},
        {SDL_SCANCODE_Y, 'Y'}, {SDL_SCANCODE_Z, 'Z'},
        // Numbers
        {SDL_SCANCODE_1, '1'}, {SDL_SCANCODE_2, '2'}, {SDL_SCANCODE_3, '3'},
        {SDL_SCANCODE_4, '4'}, {SDL_SCANCODE_5, '5'}, {SDL_SCANCODE_6, '6'},
        {SDL_SCANCODE_7, '7'}, {SDL_SCANCODE_8, '8'}, {SDL_SCANCODE_9, '9'},
        {SDL_SCANCODE_0, '0'},
        // Special
        {SDL_SCANCODE_RETURN, VK_RETURN}, {SDL_SCANCODE_BACKSPACE, VK_BACK},
        {SDL_SCANCODE_TAB, VK_TAB}, {SDL_SCANCODE_SPACE, VK_SPACE},
        // Punctuation
        {SDL_SCANCODE_MINUS, VK_OEM_MINUS}, {SDL_SCANCODE_EQUALS, VK_OEM_PLUS},
        {SDL_SCANCODE_LEFTBRACKET, VK_OEM_4}, {SDL_SCANCODE_RIGHTBRACKET, VK_OEM_6},
        {SDL_SCANCODE_BACKSLASH, VK_OEM_5}, {SDL_SCANCODE_SEMICOLON, VK_OEM_1},
        {SDL_SCANCODE_APOSTROPHE, VK_OEM_7}, {SDL_SCANCODE_GRAVE, VK_OEM_3},
        {SDL_SCANCODE_COMMA, VK_OEM_COMMA}, {SDL_SCANCODE_PERIOD, VK_OEM_PERIOD},
        {SDL_SCANCODE_SLASH, VK_OEM_2}, {SDL_SCANCODE_NONUSBACKSLASH, VK_OEM_102},
        // Caps / Scroll / Num lock
        {SDL_SCANCODE_CAPSLOCK, VK_CAPITAL}, {SDL_SCANCODE_SCROLLLOCK, VK_SCROLL},
        {SDL_SCANCODE_NUMLOCKCLEAR, VK_NUMLOCK},
        // F-keys
        {SDL_SCANCODE_F1, VK_F1}, {SDL_SCANCODE_F2, VK_F2}, {SDL_SCANCODE_F3, VK_F3},
        {SDL_SCANCODE_F4, VK_F4}, {SDL_SCANCODE_F5, VK_F5}, {SDL_SCANCODE_F6, VK_F6},
        {SDL_SCANCODE_F7, VK_F7}, {SDL_SCANCODE_F8, VK_F8}, {SDL_SCANCODE_F9, VK_F9},
        {SDL_SCANCODE_F10, VK_F10}, {SDL_SCANCODE_F11, VK_F11}, {SDL_SCANCODE_F12, VK_F12},
        {SDL_SCANCODE_F13, VK_F13}, {SDL_SCANCODE_F14, VK_F14}, {SDL_SCANCODE_F15, VK_F15},
        {SDL_SCANCODE_F16, VK_F16}, {SDL_SCANCODE_F17, VK_F17}, {SDL_SCANCODE_F18, VK_F18},
        {SDL_SCANCODE_F19, VK_F19}, {SDL_SCANCODE_F20, VK_F20}, {SDL_SCANCODE_F21, VK_F21},
        {SDL_SCANCODE_F22, VK_F22}, {SDL_SCANCODE_F23, VK_F23}, {SDL_SCANCODE_F24, VK_F24},
        // Print Screen, Pause
        {SDL_SCANCODE_PRINTSCREEN, VK_SNAPSHOT}, {SDL_SCANCODE_PAUSE, VK_PAUSE},
        // Navigation
        {SDL_SCANCODE_INSERT, VK_INSERT}, {SDL_SCANCODE_HOME, VK_HOME},
        {SDL_SCANCODE_PAGEUP, VK_PRIOR}, {SDL_SCANCODE_DELETE, VK_DELETE},
        {SDL_SCANCODE_END, VK_END}, {SDL_SCANCODE_PAGEDOWN, VK_NEXT},
        // Arrows
        {SDL_SCANCODE_RIGHT, VK_RIGHT}, {SDL_SCANCODE_LEFT, VK_LEFT},
        {SDL_SCANCODE_DOWN, VK_DOWN}, {SDL_SCANCODE_UP, VK_UP},
        // Numpad
        {SDL_SCANCODE_KP_DIVIDE, VK_DIVIDE}, {SDL_SCANCODE_KP_MULTIPLY, VK_MULTIPLY},
        {SDL_SCANCODE_KP_MINUS, VK_SUBTRACT}, {SDL_SCANCODE_KP_PLUS, VK_ADD},
        {SDL_SCANCODE_KP_ENTER, VK_RETURN},
        {SDL_SCANCODE_KP_1, VK_NUMPAD1}, {SDL_SCANCODE_KP_2, VK_NUMPAD2},
        {SDL_SCANCODE_KP_3, VK_NUMPAD3}, {SDL_SCANCODE_KP_4, VK_NUMPAD4},
        {SDL_SCANCODE_KP_5, VK_NUMPAD5}, {SDL_SCANCODE_KP_6, VK_NUMPAD6},
        {SDL_SCANCODE_KP_7, VK_NUMPAD7}, {SDL_SCANCODE_KP_8, VK_NUMPAD8},
        {SDL_SCANCODE_KP_9, VK_NUMPAD9}, {SDL_SCANCODE_KP_0, VK_NUMPAD0},
        {SDL_SCANCODE_KP_PERIOD, VK_DECIMAL},
        // Modifiers
        {SDL_SCANCODE_LCTRL, VK_LCONTROL}, {SDL_SCANCODE_LSHIFT, VK_LSHIFT},
        {SDL_SCANCODE_LALT, VK_LMENU}, {SDL_SCANCODE_LGUI, VK_LWIN},
        {SDL_SCANCODE_RCTRL, VK_RCONTROL}, {SDL_SCANCODE_RSHIFT, VK_RSHIFT},
        {SDL_SCANCODE_RALT, VK_RMENU}, {SDL_SCANCODE_RGUI, VK_RWIN},
        // Application / Menu
        {SDL_SCANCODE_APPLICATION, VK_APPS},
        // Media
        {SDL_SCANCODE_MUTE, VK_VOLUME_MUTE}, {SDL_SCANCODE_VOLUMEUP, VK_VOLUME_UP},
        {SDL_SCANCODE_VOLUMEDOWN, VK_VOLUME_DOWN},
    };

    for (const auto& entry : kScanVK) {
        if (entry.vk == VK_ESCAPE) continue;  // ESC is cancel, not a bindable key
        if (GetAsyncKeyState(entry.vk) & 0x8000) {
            outBinding->keyboard_key = entry.scancode;
            outBinding->gamepad_button = -1;
            outBinding->gamepad_axis = -1;
            outBinding->axis_direction = 0;
            if (outSource) *outSource = 0;
            g_bindingMode = false;
            g_bindingCooldown = BINDING_COOLDOWN_FRAMES;
            return true;
        }
    }

    // 2. Check gamepad buttons and axes
    SDL_Gamepad* gp = (g_bindingPlayer >= 0 && g_bindingPlayer < 2)
                      ? g_playerGamepad[g_bindingPlayer] : nullptr;
    if (gp) {
        // Buttons
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) {
                outBinding->keyboard_key = 0;
                outBinding->gamepad_button = b;
                outBinding->gamepad_axis = -1;
                outBinding->axis_direction = 0;
                if (outSource) *outSource = 1;
                g_bindingMode = false;
                g_bindingCooldown = BINDING_COOLDOWN_FRAMES;
                return true;
            }
        }
        // Axes (higher threshold to avoid accidental triggers)
        const int16_t BIND_THRESHOLD = 16000;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
            int16_t val = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (val > BIND_THRESHOLD) {
                outBinding->keyboard_key = 0;
                outBinding->gamepad_button = -1;
                outBinding->gamepad_axis = a;
                outBinding->axis_direction = 1;
                if (outSource) *outSource = 2;
                g_bindingMode = false;
                g_bindingCooldown = BINDING_COOLDOWN_FRAMES;
                return true;
            }
            if (val < -BIND_THRESHOLD) {
                outBinding->keyboard_key = 0;
                outBinding->gamepad_button = -1;
                outBinding->gamepad_axis = a;
                outBinding->axis_direction = -1;
                if (outSource) *outSource = 2;
                g_bindingMode = false;
                g_bindingCooldown = BINDING_COOLDOWN_FRAMES;
                return true;
            }
        }
    }

    return false;
}

void InputSystem_CancelBinding(void) {
    g_bindingMode = false;
    g_bindingCooldown = BINDING_COOLDOWN_FRAMES;
}

// ============================================================================
// Binding Index Helper
// ============================================================================

KeyBinding_t* InputSystem_GetBindingByIndex(PlayerBindings_t* bindings, int index) {
    if (!bindings || index < 0 || index >= INPUT_ACTION_COUNT) return nullptr;
    // PlayerBindings_t is a flat array of INPUT_ACTION_COUNT KeyBinding_t
    return &(reinterpret_cast<KeyBinding_t*>(bindings))[index];
}

const KeyBinding_t* InputSystem_GetBindingByIndexConst(const PlayerBindings_t* bindings, int index) {
    if (!bindings || index < 0 || index >= INPUT_ACTION_COUNT) return nullptr;
    return &(reinterpret_cast<const KeyBinding_t*>(bindings))[index];
}

bool InputSystem_IsBindingDown(int player, const KeyBinding_t* binding) {
    if (!binding || !IsGameWindowActive()) {
        return false;
    }

    if (binding->keyboard_key > 0) {
        const int vk = InputSystem_ScancodeToVirtualKey(binding->keyboard_key);
        if (vk > 0 && (GetAsyncKeyState(vk) & 0x8000)) {
            return true;
        }
    }

    if (player >= 0 && player < 2) {
        SDL_Gamepad* gp = g_playerGamepad[player];
        if (gp) {
            if (binding->gamepad_button >= 0 &&
                SDL_GetGamepadButton(gp, (SDL_GamepadButton)binding->gamepad_button)) {
                return true;
            }

            if (binding->gamepad_axis >= 0) {
                const int16_t value = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)binding->gamepad_axis);
                if ((binding->axis_direction > 0 && value > STICK_DEADZONE) ||
                    (binding->axis_direction < 0 && value < -STICK_DEADZONE)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool InputSystem_DoBindingsOverlap(const KeyBinding_t* lhs, const KeyBinding_t* rhs) {
    if (!lhs || !rhs) {
        return false;
    }

    if (lhs->keyboard_key > 0 && lhs->keyboard_key == rhs->keyboard_key) {
        return true;
    }

    if (lhs->gamepad_button >= 0 && lhs->gamepad_button == rhs->gamepad_button) {
        return true;
    }

    if (lhs->gamepad_axis >= 0 &&
        lhs->gamepad_axis == rhs->gamepad_axis &&
        lhs->axis_direction == rhs->axis_direction) {
        return true;
    }

    return false;
}

void InputSystem_GetBindingDisplayName(const KeyBinding_t* binding, char* out, int outSize) {
    if (!out || outSize <= 0) {
        return;
    }

    out[0] = '\0';
    if (!binding) {
        snprintf(out, outSize, "None");
        return;
    }

    int written = 0;
    auto appendPart = [&](const char* text) {
        if (!text || !text[0] || written >= outSize - 1) {
            return;
        }

        if (written > 0) {
            written += snprintf(out + written, outSize - written, " / ");
            if (written >= outSize - 1) {
                out[outSize - 1] = '\0';
                return;
            }
        }

        written += snprintf(out + written, outSize - written, "%s", text);
        if (written >= outSize - 1) {
            out[outSize - 1] = '\0';
        }
    };

    if (binding->keyboard_key > 0) {
        appendPart(InputSystem_GetKeyName(binding->keyboard_key));
    }
    if (binding->gamepad_button >= 0) {
        appendPart(InputSystem_GetGamepadButtonName(binding->gamepad_button));
    }
    if (binding->gamepad_axis >= 0) {
        appendPart(InputSystem_GetGamepadAxisName(binding->gamepad_axis, binding->axis_direction));
    }

    if (!out[0]) {
        snprintf(out, outSize, "None");
    }
}

// ============================================================================
// Config File I/O (versioned format)
// ============================================================================

#define CONFIG_MAGIC   0x49325341  // "AS2I"
#define CONFIG_VERSION 2           // v2 = SDL3 Gamepad bindings

struct ConfigHeader {
    uint32_t magic;
    uint32_t version;
};

bool InputSystem_LoadConfig(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("[Input] No config file found, using defaults\n");
        return false;
    }

    ConfigHeader hdr = {};
    size_t hdrRead = fread(&hdr, sizeof(hdr), 1, f);

    if (hdrRead == 1 && hdr.magic == CONFIG_MAGIC && hdr.version == CONFIG_VERSION) {
        // v2 format: header + bindings
        size_t read = fread(g_bindings, sizeof(PlayerBindings_t), 2, f);
        fclose(f);
        if (read == 2) {
            printf("[Input] Loaded v2 config from %s\n", filename);
            return true;
        }
    } else {
        fclose(f);
    }

    // Incompatible or corrupt — reset to defaults
    printf("[Input] Config incompatible (expected v%d), resetting to defaults\n", CONFIG_VERSION);
    SetDefaultBindings(&g_bindings[0], 0);
    SetDefaultBindings(&g_bindings[1], 1);
    remove(filename);
    return false;
}

bool InputSystem_SaveConfig(const char* filename) {
    FILE* f = fopen(filename, "wb");
    if (!f) return false;

    ConfigHeader hdr = {CONFIG_MAGIC, CONFIG_VERSION};
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(g_bindings, sizeof(PlayerBindings_t), 2, f);
    fclose(f);

    printf("[Input] Saved config to %s\n", filename);
    return true;
}

// ============================================================================
// Direct Game Buffer Write
// ============================================================================
//
// Input Buffer Layout (per player, 10 buttons):
//   Base+0:   Held state    (10 words)
//   Base+28:  Cur held      (10 words) — gameplay reads this
//   Base+56:  Just-pressed  (10 words) — menus read this
//   Base+84:  Cooldown      (10 words)
//   Base+140: Hold counter  (10 words)
//
// Button order matches game's mask array at OFF_P1_BUTTON_MASKS:
//   Slot 0=Up(8), 1=Down(1), 2=Left(2), 3=Right(4), 4..9=A,B,C,D,Start,Select
// The SDL bit values differ from game joy bits, but SLOT ORDER must match.

#define GAME_P1_INPUT_BASE  0x8E9E62
#define GAME_P2_INPUT_BASE  0x8E9F32
#define INPUT_ARRAY_JUSTPRESSED 28   // word offset for just-pressed array

static const uint16_t g_buttonMasks[10] = {
    INPUT_UP, INPUT_DOWN, INPUT_LEFT, INPUT_RIGHT,
    INPUT_A, INPUT_B, INPUT_C, INPUT_D,
    INPUT_START, INPUT_SELECT
};

void InputSystem_WriteToGameBuffers(int player) {
    if (player < 0 || player > 1 || !g_initialized) return;

    uintptr_t base = (player == 0) ? GAME_P1_INPUT_BASE : GAME_P2_INPUT_BASE;

    uint16_t current, justPressed;

    if (g_netplayInputActive[player]) {
        // Netplay/rollback path: use rollback-controlled inputs.
        // Derive just-pressed by reading the game buffer's previous held state.
        uint16_t prevHeld = 0;
        __try {
            for (int i = 0; i < 10; i++) {
                if (*(uint16_t*)(base + i * 2))
                    prevHeld |= g_buttonMasks[i];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            prevHeld = 0;
        }
        current = g_netplayInput[player];
        justPressed = current & ~prevHeld;
    } else {
        current     = g_inputState[player].current;
        justPressed = g_inputState[player].pressed;
    }

    // Pause suppression: strip Start just-pressed during netplay match
    if (g_pauseBlocked) {
        justPressed &= ~INPUT_START;
    }

    for (int i = 0; i < 10; i++) {
        uint16_t mask = g_buttonMasks[i];
        uint16_t* heldPtr       = (uint16_t*)(base + i * 2);
        uint16_t* justPressedPtr = (uint16_t*)(base + INPUT_ARRAY_JUSTPRESSED * 2 + i * 2);

        __try {
            *heldPtr       = (current     & mask) ? 1 : 0;
            *justPressedPtr = (justPressed & mask) ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("[Input] Write failed at 0x%08X (P%d btn %d)\n",
                   (unsigned)base, player + 1, i);
        }
    }
}

void InputSystem_WriteToGameBuffersBothPlayers(void) {
    InputSystem_WriteToGameBuffers(0);
    InputSystem_WriteToGameBuffers(1);
}

// ============================================================================
// Display Name Helpers
// ============================================================================

const char* InputSystem_GetKeyName(int sdlScancode) {
    if (sdlScancode <= 0) return "None";
    SDL_Keycode key = SDL_GetKeyFromScancode((SDL_Scancode)sdlScancode, SDL_KMOD_NONE, false);
    const char* name = SDL_GetKeyName(key);
    if (name && name[0]) return name;
    return "Unknown";
}

const char* InputSystem_GetGamepadButtonName(int btn) {
    switch (btn) {
        case SDL_GAMEPAD_BUTTON_SOUTH:           return "A / Cross";
        case SDL_GAMEPAD_BUTTON_EAST:            return "B / Circle";
        case SDL_GAMEPAD_BUTTON_WEST:            return "X / Square";
        case SDL_GAMEPAD_BUTTON_NORTH:           return "Y / Triangle";
        case SDL_GAMEPAD_BUTTON_BACK:            return "Back / Select";
        case SDL_GAMEPAD_BUTTON_GUIDE:           return "Guide";
        case SDL_GAMEPAD_BUTTON_START:           return "Start / Options";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:      return "L Stick Click";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:     return "R Stick Click";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:   return "LB / L1";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:  return "RB / R1";
        case SDL_GAMEPAD_BUTTON_DPAD_UP:         return "D-Pad Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:       return "D-Pad Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:       return "D-Pad Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:      return "D-Pad Right";
        case SDL_GAMEPAD_BUTTON_MISC1:           return "Misc";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:   return "R Paddle 1";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:    return "L Paddle 1";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:   return "R Paddle 2";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:    return "L Paddle 2";
        case SDL_GAMEPAD_BUTTON_TOUCHPAD:        return "Touchpad";
        default:                                 return "Unknown";
    }
}

const char* InputSystem_GetGamepadAxisName(int axis, int direction) {
    switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            return direction > 0 ? "L Stick Right" : "L Stick Left";
        case SDL_GAMEPAD_AXIS_LEFTY:
            return direction > 0 ? "L Stick Down" : "L Stick Up";
        case SDL_GAMEPAD_AXIS_RIGHTX:
            return direction > 0 ? "R Stick Right" : "R Stick Left";
        case SDL_GAMEPAD_AXIS_RIGHTY:
            return direction > 0 ? "R Stick Down" : "R Stick Up";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            return "L Trigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return "R Trigger";
        default: return "Unknown Axis";
    }
}
