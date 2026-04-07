#include "patches/input_override.h"
#include "patches/input_sync_hooks.h"
#include "input_system.h"
#include "patches/memory_utils.h"
#include "as2_constants.h"
#include "log_window.h"
#include "net/netplay_menu_controller.h"
#include "net/session_manager.h"
#include "net/charsel_sync.h"
#include "net/stagesel_sync.h"
#include "core/game_state.h"
#include "imgui.h"

// ============================================================================
// Original function pointer storage (populated by hook_installer)
// ============================================================================

KeyboardState_t g_origKeyboardState = nullptr;
JoystickState_t g_origJoystickState = nullptr;
InputProcess_t g_origInputProcess = nullptr;
InputDispatcher_t g_origInputDispatcher = nullptr;
DInputKBRefresh_t g_origDInputKBRefresh = nullptr;
DInputJoyRefresh_t g_origDInputJoyRefresh = nullptr;
GetKeyboardState_t g_origGetKeyboardState = nullptr;

// ============================================================================
// Module config access
// ============================================================================

// Defined in mod_main.cpp
extern bool ModConfig_UseSDLInput();
extern bool ModConfig_VerboseLogging();

// ============================================================================
// Internal state
// ============================================================================

static int g_hookCallCount = 0;
static int g_lastInputUpdateFrame = -1;

InputDebugInfo g_inputDebug = {};

static inline bool IsCharSelDispatcherLockstepSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_SELECT ||
           substate == CHARSEL_SUB_CONFIRM;
}

static inline bool IsStageSelRawLockstepSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_STAGESEL_GRID ||
           substate == CHARSEL_SUB_STAGESEL_CONFIRM;
}

// ============================================================================
// Frame-based input update tracking
// ============================================================================

static void EnsureInputUpdated() {
    int currentFrame = ReadMemory<int>(ADDR_SIM_FRAME_COUNTER);
    if (currentFrame != g_lastInputUpdateFrame) {
        InputSystem_Update();
        g_lastInputUpdateFrame = currentFrame;
    }
}

// ============================================================================
// Scancode / conversion helpers
// ============================================================================

uint16_t ScanCodeToInputFlag(int scancode) {
    switch (scancode) {
        case 0xC8: return INPUT_UP;
        case 0xD0: return INPUT_DOWN;
        case 0xCB: return INPUT_LEFT;
        case 0xCD: return INPUT_RIGHT;
        case 72:   return INPUT_UP;
        case 80:   return INPUT_DOWN;
        case 75:   return INPUT_LEFT;
        case 77:   return INPUT_RIGHT;
        case 0x2C: return INPUT_A;
        case 0x2D: return INPUT_B;
        case 0x2E: return INPUT_C;
        case 0x1E: return INPUT_D;
        case 0x1F: return INPUT_L1;
        case 0x20: return INPUT_R1;
        case 0x10: return INPUT_L2;
        case 0x11: return INPUT_R2;
        case 0x01: return INPUT_SELECT;
        case 0x1C: return INPUT_START;
        case 0x39: return INPUT_SELECT;
        case 0x0E: return INPUT_SELECT;
        default:   return 0;
    }
}

uint16_t ConvertToGameJoyFormat(uint16_t input) {
    uint16_t result = 0;
    if (input & INPUT_UP)    result |= 0x0008;
    if (input & INPUT_DOWN)  result |= 0x0001;
    if (input & INPUT_LEFT)  result |= 0x0002;
    if (input & INPUT_RIGHT) result |= 0x0004;
    result |= (input & 0xFFF0);
    return result;
}

uint8_t SDLScancodeToDIK(int sdlScancode) {
    switch (sdlScancode) {
        case 4:  return 0x1E;  case 5:  return 0x30;  case 6:  return 0x2E;
        case 7:  return 0x20;  case 8:  return 0x12;  case 9:  return 0x21;
        case 10: return 0x22;  case 11: return 0x23;  case 12: return 0x17;
        case 13: return 0x24;  case 14: return 0x25;  case 15: return 0x26;
        case 16: return 0x32;  case 17: return 0x31;  case 18: return 0x18;
        case 19: return 0x19;  case 20: return 0x10;  case 21: return 0x13;
        case 22: return 0x1F;  case 23: return 0x14;  case 24: return 0x16;
        case 25: return 0x2F;  case 26: return 0x11;  case 27: return 0x2D;
        case 28: return 0x15;  case 29: return 0x2C;
        case 30: return 0x02;  case 31: return 0x03;  case 32: return 0x04;
        case 33: return 0x05;  case 34: return 0x06;  case 35: return 0x07;
        case 36: return 0x08;  case 37: return 0x09;  case 38: return 0x0A;
        case 39: return 0x0B;
        case 40: return 0x1C;  case 41: return 0x01;  case 42: return 0x0E;
        case 43: return 0x0F;  case 44: return 0x39;
        case 79: return 0xCD;  case 80: return 0xCB;  case 81: return 0xD0;
        case 82: return 0xC8;
        case 89: return 0x4F;  case 90: return 0x50;  case 91: return 0x51;
        case 92: return 0x4B;  case 93: return 0x4C;  case 94: return 0x4D;
        case 95: return 0x47;  case 96: return 0x48;  case 97: return 0x49;
        case 98: return 0x52;  case 88: return 0x9C;
        default: return 0;
    }
}

// ============================================================================
// Keyboard input injection (SDL → DInput buffer)
// ============================================================================

static void InjectKeyboardInput() {
    if (!ModConfig_UseSDLInput()) return;
    
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    const PlayerBindings_t* bindings = InputSystem_GetBindings(0);
    if (!bindings) return;
    
    uint16_t input = InputSystem_GetInput(0);
    
    auto injectKey = [&](const KeyBinding_t* binding, uint16_t flag) {
        if (binding->keyboard_key > 0 && (input & flag)) {
            uint8_t dik = SDLScancodeToDIK(binding->keyboard_key);
            if (dik > 0) {
                keyBuffer[dik] |= 0x80;
            }
        }
    };
    
    injectKey(&bindings->up,    INPUT_UP);
    injectKey(&bindings->down,  INPUT_DOWN);
    injectKey(&bindings->left,  INPUT_LEFT);
    injectKey(&bindings->right, INPUT_RIGHT);
    injectKey(&bindings->a,     INPUT_A);
    injectKey(&bindings->b,     INPUT_B);
    injectKey(&bindings->c,     INPUT_C);
    injectKey(&bindings->d,     INPUT_D);
    injectKey(&bindings->start, INPUT_START);
    injectKey(&bindings->select,INPUT_SELECT);
    injectKey(&bindings->l1,    INPUT_L1);
    injectKey(&bindings->r1,    INPUT_R1);
}

// ============================================================================
// Window focus helper
// ============================================================================

static bool IsGameWindowFocused() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return (foregroundPid == GetCurrentProcessId());
}

// ============================================================================
// Hook: Win32 GetKeyboardState
// ============================================================================

BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    if ((InputSystem_IsBindingActive() || NetMenu::ConsumesGameInput()) && lpKeyState) {
        memset(lpKeyState, 0, 256);
        return TRUE;
    }

    if (ModConfig_UseSDLInput() && lpKeyState) {
        memset(lpKeyState, 0, 256);
        EnsureInputUpdated();

        const uint16_t input = InputSystem_GetInput(0);
        auto setPressed = [&](int vk, bool pressed) {
            if (vk < 0 || vk >= 256) return;
            lpKeyState[vk] = pressed ? 0x80 : 0x00;
        };

        setPressed(VK_UP,    (input & INPUT_UP) != 0);
        setPressed(VK_DOWN,  (input & INPUT_DOWN) != 0);
        setPressed(VK_LEFT,  (input & INPUT_LEFT) != 0);
        setPressed(VK_RIGHT, (input & INPUT_RIGHT) != 0);
        setPressed('Z', (input & INPUT_A) != 0);
        setPressed('X', (input & INPUT_B) != 0);
        setPressed('C', (input & INPUT_C) != 0);
        setPressed('A', (input & INPUT_D) != 0);
        setPressed('S', (input & INPUT_L1) != 0);
        setPressed('D', (input & INPUT_R1) != 0);
        setPressed('Q', (input & INPUT_L2) != 0);
        setPressed('W', (input & INPUT_R2) != 0);
        setPressed(VK_RETURN, (input & INPUT_START) != 0);
        setPressed(VK_ESCAPE, (input & INPUT_SELECT) != 0);
        setPressed(VK_SPACE,  (input & INPUT_SELECT) != 0);
        setPressed(VK_BACK,   (input & INPUT_SELECT) != 0);

        return TRUE;
    }
    
    return g_origGetKeyboardState(lpKeyState);
}

// ============================================================================
// Hook: DInput keyboard buffer refresh (sub_630130)
// ============================================================================

int __cdecl Hook_DInputKBRefresh() {
    EnsureInputUpdated();
    
    if (ModConfig_UseSDLInput()) {
        uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
        memset(keyBuffer, 0, 256);
        return 0;
    }
    
    return g_origDInputKBRefresh();
}

// ============================================================================
// Hook: DInput joystick buffer refresh (sub_6302F0)
// ============================================================================

int __cdecl Hook_DInputJoyRefresh(int joyID) {
    EnsureInputUpdated();
    
    if (ModConfig_UseSDLInput()) {
        for (int i = 0; i < DINPUT_JOY_MAX; i++) {
            uintptr_t joyBase = ADDR_DINPUT_JOYSTICK + (i * DINPUT_JOY_STRUCT_SIZE);
            WriteMemory<int32_t>(joyBase + 0, 0);
            WriteMemory<int32_t>(joyBase + 4, 0);
            for (int j = 0; j < 24; j++) {
                WriteMemory<uint8_t>(joyBase + DINPUT_JOY_BTN_OFFSET + j, 0);
            }
        }
        return 0;
    }
    
    int result = g_origDInputJoyRefresh(joyID);
    
    if (joyID & 0x1000) {
        return result;
    }
    
    int joyIndex = (joyID & 0xFFF) - 1;
    if (joyIndex < 0 || joyIndex >= DINPUT_JOY_MAX) {
        return result;
    }
    
    static const uintptr_t P1_JOY_ID_ADDR = 0x816358 + 864440;
    static const uintptr_t P2_JOY_ID_ADDR = 0x816358 + 864484;
    int p1JoyID = ReadMemory<int>(P1_JOY_ID_ADDR);
    int p2JoyID = ReadMemory<int>(P2_JOY_ID_ADDR);
    
    int playerIndex = -1;
    if (joyID == p1JoyID) playerIndex = 0;
    else if (joyID == p2JoyID) playerIndex = 1;
    
    if (playerIndex < 0) {
        return result;
    }
    
    return result;
}

// ============================================================================
// Hook: Keyboard state (sub_62FD00)
// ============================================================================

int __cdecl Hook_KeyboardState(int keyCode) {
    g_hookCallCount++;
    g_inputDebug.keyboardHookCalls++;
    g_inputDebug.lastKeyCode = keyCode;
    
    EnsureInputUpdated();

    if (InputSystem_IsBindingActive() || NetMenu::ConsumesGameInput()) {
        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = 0;
        return 0;
    }
    
    if (ModConfig_UseSDLInput()) {
        const uint16_t flag = ScanCodeToInputFlag(keyCode);
        const uint16_t input = InputSystem_GetInput(0);
        const int pressed = (flag != 0 && (input & flag) != 0) ? 1 : 0;

        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = pressed;

        if (pressed) {
            g_inputDebug.keyboardInjectedCount++;
            g_inputDebug.lastInjectedKeyInput = flag;
        }
        return pressed;
    }
    
    int origResult = g_origKeyboardState(keyCode);
    g_inputDebug.lastOrigResult = origResult;
    g_inputDebug.lastFinalResult = origResult;
    return origResult;
}

// ============================================================================
// Hook: Joystick state (sub_62FF50)
// ============================================================================

int __cdecl Hook_JoystickState(int playerID) {
    g_hookCallCount++;
    g_inputDebug.joystickHookCalls++;
    g_inputDebug.lastPlayerID = playerID;
    
    EnsureInputUpdated();

    if (InputSystem_IsBindingActive()) {
        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = 0;
        return 0;
    }
    
    if (ModConfig_UseSDLInput()) {
        g_inputDebug.lastOrigResult = 0;
        
        static const uintptr_t GAME_CONFIG_BASE = 0x816358;
        static const uintptr_t P1_JOY_ID_ADDR = GAME_CONFIG_BASE + 864440;
        static const uintptr_t P2_JOY_ID_ADDR = GAME_CONFIG_BASE + 864484;
        
        int p1JoyID = ReadMemory<int>(P1_JOY_ID_ADDR);
        int p2JoyID = ReadMemory<int>(P2_JOY_ID_ADDR);
        g_inputDebug.p1JoyID = p1JoyID;
        g_inputDebug.p2JoyID = p2JoyID;
        
        int playerIndex = (playerID == p1JoyID) ? 0 : (playerID == p2JoyID) ? 1 : 0;
        g_inputDebug.lastMappedPlayer = playerIndex;
        
        // During netplay: suppress local P2 hardware input entirely.
        // P2 is controlled by the remote peer (charsel_sync / rollback input).
        if (playerIndex == 1 && Net::Session_IsConnected()) {
            g_inputDebug.sdlInputP2 = 0;
            g_inputDebug.gameInputP2 = 0;
            g_inputDebug.lastFinalResult = 0;
            return 0;
        }

        uint16_t sdlInput = InputSystem_GetInput(playerIndex);
        uint16_t gameInput = ConvertToGameJoyFormat(sdlInput);
        
        if (playerIndex == 0) {
            g_inputDebug.sdlInputP1 = sdlInput;
            g_inputDebug.gameInputP1 = gameInput;
        } else {
            g_inputDebug.sdlInputP2 = sdlInput;
            g_inputDebug.gameInputP2 = gameInput;
        }
        
        if (gameInput != 0) {
            g_inputDebug.joystickInjectedCount++;
            g_inputDebug.lastInjectedJoyInput = gameInput;
        }
        
        g_inputDebug.lastFinalResult = (int)gameInput;
        return (int)gameInput;
    }
    
    int origResult = g_origJoystickState(playerID);
    g_inputDebug.lastOrigResult = origResult;
    g_inputDebug.lastFinalResult = origResult;
    return origResult;
}

// ============================================================================
// Input Dispatcher Hook (sub_5625E0 / Input_TryGetNextFrame)
//
// During charsel lockstep: replaces vanilla input dispatch with lockstep data.
// Both peers see identical P1/P2 inputs → identical charsel navigation.
// ============================================================================

// Shared constant: just-pressed starts at word offset 28 in the input buffer.
#define JUST_PRESSED_OFFSET_WORDS 28

// Edge detection state for raw array overwrite
static uint16_t s_dispPrevP1 = 0;
static uint16_t s_dispPrevP2 = 0;

// Logging throttle
static uint32_t s_dispatchCount = 0;
static uint32_t s_dispatchWaitCount = 0;
static bool     s_dispatchFirstLog = false;

// Charsel: prevent producing more than one input per game-loop iteration.
// The game calls the dispatcher in a while-loop; we return 0 once (produce
// a frame) then -1 to break out. This flag resets when -1 is returned.
static bool s_charsel_produced_this_loop = false;

int __cdecl Hook_InputDispatcher(__int16* outputInputs) {
    if (!outputInputs) return -1;

    // ── Load barrier freeze ─────────────────────────────────────────
    // Game reached match loading but bootstrap hasn't completed.
    // Freeze gameplay (return -1) while keeping game loop alive for
    // SessionManager updates, packet exchange, and ImGui rendering.
    if (InputSyncHooks_IsGameplayFreezeActive()) {
        return -1;
    }

    // ── CharSel lockstep ────────────────────────────────────────────
    // Replaces vanilla dispatch with deterministic lockstep.
    // Both sides exchange inputs frame-by-frame. Game only advances
    // when BOTH local and remote inputs are available.
    if (Net::CharSelSync_IsLockstepActive()) {
        const uint32_t gameMode = GetGameMode();
        const uint32_t subState = GetSubstate();

        // Only run lockstep during active selection substates
        if (gameMode != MODE_CHARSEL || !IsCharSelDispatcherLockstepSubstate(subState)) {
            // Not in a lockstep substate — freeze and suppress vanilla
            return -1;
        }

        // Log first intercept
        if (!s_dispatchFirstLog) {
            s_dispatchFirstLog = true;
            s_dispatchCount = 0;
            s_dispatchWaitCount = 0;
            s_charsel_produced_this_loop = false;
            LOG_NETPLAY(LOG_INFO, "[InputDispatch] Lockstep intercept ACTIVE (mode=%u sub=%u)",
                gameMode, subState);
        }

        // Frame gate: only produce one frame per game-loop iteration.
        // The while-loop calls us repeatedly; after producing one frame,
        // return -1 to break out. Reset when we're called again next iteration.
        if (s_charsel_produced_this_loop) {
            s_charsel_produced_this_loop = false;  // Reset for next iteration
            return -1;
        }

        // Poll SDL input and get local packed input
        InputSystem_Update();
        uint16_t localInput = InputSystem_GetInput(0);

        // Buffer locally and send to peer (with redundant history)
        Net::CharSelSync_CaptureLocalInput(localInput);

        // Lockstep gate: only advance when both inputs are available
        if (!Net::CharSelSync_HasInputsForCurrentFrame()) {
            s_dispatchWaitCount++;
            if (s_dispatchWaitCount <= 3 || (s_dispatchWaitCount % 60) == 0) {
                LOG_NETPLAY(LOG_DEBUG, "[InputDispatch] Waiting for remote input (wait#%u)",
                    s_dispatchWaitCount);
            }
            return -1;  // Freeze — wait for remote input
        }

        // Consume confirmed inputs for this frame
        uint16_t p1 = 0, p2 = 0;
        if (!Net::CharSelSync_ConsumeCurrentFrame(&p1, &p2)) {
            LOG_NETPLAY(LOG_WARNING, "[InputDispatch] Lockstep inconsistency: inputs ready but consume failed");
            return -1;
        }

        s_dispatchCount++;
        s_dispatchWaitCount = 0;

        // Log consumed frames (first 5, then every 60)
        if (s_dispatchCount <= 5 || (s_dispatchCount % 60) == 0) {
            LOG_NETPLAY(LOG_DEBUG, "[InputDispatch] Consumed frame #%u: P1=0x%04X P2=0x%04X (sub=%u)",
                s_dispatchCount, p1, p2, subState);
        }

        // Stage select uses a shared cursor / confirm menu.
        // Merge both players' confirmed inputs into one shared input so both
        // peers drive the same stage UI path deterministically.
        // Write to output array
        outputInputs[0] = (__int16)p1;
        outputInputs[1] = (__int16)p2;

        // Advance Frame_Inputs (vanilla dispatcher does this)
        volatile int32_t* pFrameWrite = reinterpret_cast<volatile int32_t*>(ADDR_INPUT_WRITE_IDX);
        (*pFrameWrite)++;

        // Edge detection for raw array just-pressed
        uint16_t justP1 = p1 & ~s_dispPrevP1;
        uint16_t justP2 = p2 & ~s_dispPrevP2;
        s_dispPrevP1 = p1;
        s_dispPrevP2 = p2;

        // Overwrite P1/P2 raw input arrays (held + just-pressed).
        // In GAMETYPE_VS_HUMAN, the vanilla engine DIRECTLY polls the raw
        // arrays (0x8E9E62 for P1, 0x8E9F32 for P2) for menu navigation,
        // completely ignoring the lockstep arrays. By forcibly overwriting
        // these, we guarantee both cursors are enslaved by network lockstep.
        for (int i = 0; i < 10; i++) {
            uint16_t mask = (uint16_t)(1 << i);
            WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (i * 2),
                (uint16_t)((p1 & mask) ? 1 : 0));
            WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                (uint16_t)((justP1 & mask) ? 1 : 0));
            WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (i * 2),
                (uint16_t)((p2 & mask) ? 1 : 0));
            WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                (uint16_t)((justP2 & mask) ? 1 : 0));
        }

        s_charsel_produced_this_loop = true;
        return 0;
    }

    // ── Vanilla passthrough (offline/local play only) ────────────────
    // Reset logging state when not intercepting
    if (s_dispatchFirstLog) {
        s_dispatchFirstLog = false;
        s_dispatchCount = 0;
        s_dispatchWaitCount = 0;
    }
    return g_origInputDispatcher(outputInputs);
}

// ============================================================================
// Input Processing Hook (sub_562060)
// ============================================================================

static_assert(ADDR_P1_INPUT_BUFFER == 0x8E9E62,
              "Alt-buffer IS the main buffer — they must be the same address");
static_assert(ADDR_P2_INPUT_BUFFER == 0x8E9F32,
              "Alt-buffer IS the main buffer — they must be the same address");

static const uint16_t g_buttonMasks[10] = {
    0x0001, 0x0002, 0x0004, 0x0008, 0x0010,
    0x0020, 0x0040, 0x0080, 0x0100, 0x0200
};

static uint16_t ReadHeldMaskFromAltBuffer(uintptr_t altBufferAddr) {
    uint16_t heldMask = 0;
    __try {
        for (int i = 0; i < 10; i++) {
            const uint16_t heldVal = ReadMemory<uint16_t>(altBufferAddr + (i * 2));
            if (heldVal) {
                heldMask |= g_buttonMasks[i];
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return heldMask;
}

int __cdecl Hook_InputProcess(int gameState) {
    const bool consumeForCustomMenu = InputSystem_IsBindingActive() || NetMenu::ConsumesGameInput();

    if (ModConfig_UseSDLInput()) {
        EnsureInputUpdated();
    }

    const uint16_t prevHeldP1 = ReadHeldMaskFromAltBuffer(ADDR_P1_INPUT_BUFFER);
    const uint16_t prevHeldP2 = ReadHeldMaskFromAltBuffer(ADDR_P2_INPUT_BUFFER);

    int result = g_origInputProcess(gameState);

    auto clearLiveInputBuffers = []() {
        static const uint8_t zeroBuffer[INPUT_BUFFER_SIZE] = {};
        static const uint8_t zeroState[INPUT_STATE_SIZE] = {};
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_BUFFER, zeroBuffer, sizeof(zeroBuffer));
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_BUFFER, zeroBuffer, sizeof(zeroBuffer));
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_STATE, zeroState, sizeof(zeroState));
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_STATE, zeroState, sizeof(zeroState));
    };

    if (consumeForCustomMenu) {
        clearLiveInputBuffers();
        return result;
    }

    if (Net::CharSelSync_IsLockstepActive()) {
        const uint32_t gameMode = GetGameMode();
        const uint32_t subState = GetSubstate();

        if (gameMode == MODE_CHARSEL && IsStageSelRawLockstepSubstate(subState)) {
            InputSystem_Update();
            const uint16_t localInput = InputSystem_GetInput(0);
            Net::CharSelSync_CaptureLocalInput(localInput);

            if (!Net::CharSelSync_HasInputsForCurrentFrame()) {
                clearLiveInputBuffers();
                return result;
            }

            uint16_t p1 = 0;
            uint16_t p2 = 0;
            if (!Net::CharSelSync_ConsumeCurrentFrame(&p1, &p2)) {
                clearLiveInputBuffers();
                return result;
            }

            const uint16_t merged = Net::StageSelSync_MergeConfirmed(
                (uint32_t)ReadMemory<int>(ADDR_INPUT_WRITE_IDX), p1, p2);
            const uint16_t pressedMerged = (uint16_t)(merged & (uint16_t)~prevHeldP1);

            for (int i = 0; i < 10; i++) {
                const uint16_t mask = g_buttonMasks[i];

                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (i * 2),
                    (merged & mask) ? 1 : 0);
                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2),
                    (pressedMerged & mask) ? 1 : 0);

                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (i * 2), 0);
                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2), 0);
            }

            return result;
        }
    }

    // Netplay override: when rollback session is active, inject rollback-controlled
    // inputs instead of SDL data. The netplay inputs were stored by
    // RollbackSession_FrameUpdate Step 7 via InputSystem_SetNetplayInput.
    if (InputSystem_IsNetplayInputActive(0) || InputSystem_IsNetplayInputActive(1)) {
        uint16_t currentP1 = InputSystem_IsNetplayInputActive(0)
                                 ? InputSystem_GetNetplayInput(0) : 0;
        uint16_t currentP2 = InputSystem_IsNetplayInputActive(1)
                                 ? InputSystem_GetNetplayInput(1) : 0;

        const uint16_t pressedP1 = (uint16_t)(currentP1 & (uint16_t)~prevHeldP1);
        const uint16_t pressedP2 = (uint16_t)(currentP2 & (uint16_t)~prevHeldP2);

        for (int i = 0; i < 10; i++) {
            const uint16_t mask = g_buttonMasks[i];

            {
                const uintptr_t heldAddr = ADDR_P1_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                WriteMemory<uint16_t>(heldAddr, (currentP1 & mask) ? 1 : 0);
                WriteMemory<uint16_t>(justPressedAddr, (pressedP1 & mask) ? 1 : 0);
            }

            {
                const uintptr_t heldAddr = ADDR_P2_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                WriteMemory<uint16_t>(heldAddr, (currentP2 & mask) ? 1 : 0);
                WriteMemory<uint16_t>(justPressedAddr, (pressedP2 & mask) ? 1 : 0);
            }
        }

        return result;
    }

    if (ModConfig_UseSDLInput()) {
        const uint16_t allowedMask = (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT |
                                      INPUT_A | INPUT_B | INPUT_C | INPUT_D |
                                      INPUT_START | INPUT_SELECT);

        const InputState_t* p1State = InputSystem_GetState(0);
        const InputState_t* p2State = InputSystem_GetState(1);
        uint16_t currentP1 = (uint16_t)((p1State ? p1State->current : 0) & allowedMask);
        uint16_t currentP2 = (uint16_t)((p2State ? p2State->current : 0) & allowedMask);

        // During netplay: suppress local P2 hardware input.
        // P2 is controlled by the remote peer via charsel_sync or rollback input injection.
        if (Net::Session_IsConnected()) {
            currentP2 = 0;
        }

        if (InputSystem_GetControlSwap()) {
            const uint16_t tmp = currentP1;
            currentP1 = currentP2;
            currentP2 = tmp;
        }

        const uint16_t pressedP1 = (uint16_t)(currentP1 & (uint16_t)~prevHeldP1);
        const uint16_t pressedP2 = (uint16_t)(currentP2 & (uint16_t)~prevHeldP2);

        for (int i = 0; i < 10; i++) {
            const uint16_t mask = g_buttonMasks[i];

            {
                const uintptr_t heldAddr = ADDR_P1_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                const uint16_t heldVal = (currentP1 & mask) ? 1 : 0;
                const uint16_t pressedVal = (pressedP1 & mask) ? 1 : 0;
                WriteMemory<uint16_t>(heldAddr, heldVal);
                WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + (i * 2), heldVal);
            }

            {
                const uintptr_t heldAddr = ADDR_P2_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                const uint16_t heldVal = (currentP2 & mask) ? 1 : 0;
                const uint16_t pressedVal = (pressedP2 & mask) ? 1 : 0;
                WriteMemory<uint16_t>(heldAddr, heldVal);
                WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + (i * 2), heldVal);
            }
        }

        // During netplay: charsel lockstep capture is handled by Hook_InputDispatcher.
        // Do NOT capture here — it would cause unbounded send-head growth.
    }

    return result;
}

// ============================================================================
// Direct Input Injection
// ============================================================================

static const uint8_t SCANCODE_UP    = 0xC8;
static const uint8_t SCANCODE_DOWN  = 0xD0;
static const uint8_t SCANCODE_LEFT  = 0xCB;
static const uint8_t SCANCODE_RIGHT = 0xCD;
static const uint8_t SCANCODE_Z     = 0x2C;
static const uint8_t SCANCODE_X     = 0x2D;
static const uint8_t SCANCODE_A     = 0x1E;
static const uint8_t SCANCODE_S     = 0x1F;
static const uint8_t SCANCODE_ENTER = 0x1C;
static const uint8_t SCANCODE_ESC   = 0x01;

void WriteKeyboardInput(uint8_t scancode, bool pressed) {
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    if (pressed) {
        keyBuffer[scancode] |= 0x80;
    } else {
        keyBuffer[scancode] &= 0x7F;
    }
}

static void WriteJoystickInputDirect(int joyIndex, uint16_t input) {
    if (joyIndex < 0 || joyIndex >= DINPUT_JOY_MAX) return;
    
    uintptr_t joyBase = ADDR_DINPUT_JOYSTICK + (joyIndex * DINPUT_JOY_STRUCT_SIZE);
    
    int32_t xAxis = 0;
    if (input & INPUT_LEFT)  xAxis = -1000;
    if (input & INPUT_RIGHT) xAxis = +1000;
    WriteMemory<int32_t>(joyBase + 0, xAxis);
    
    int32_t yAxis = 0;
    if (input & INPUT_UP)   yAxis = -1000;
    if (input & INPUT_DOWN) yAxis = +1000;
    WriteMemory<int32_t>(joyBase + 4, yAxis);
    
    uint8_t* buttons = reinterpret_cast<uint8_t*>(joyBase + 64);
    for (int i = 0; i < 24; i++) {
        buttons[i] = 0;
    }
    
    if (input & INPUT_A)      buttons[0] = 0x80;
    if (input & INPUT_B)      buttons[1] = 0x80;
    if (input & INPUT_C)      buttons[2] = 0x80;
    if (input & INPUT_D)      buttons[3] = 0x80;
    if (input & INPUT_START)  buttons[7] = 0x80;
    if (input & INPUT_SELECT) buttons[6] = 0x80;
}

void WriteMatchInput(int player, uint16_t input) {
    uintptr_t bufferAddr = (player == 0) ? ADDR_P1_INPUT_BUFFER : ADDR_P2_INPUT_BUFFER;
    
    WriteMemory<uint16_t>(bufferAddr + 0,  (input & INPUT_DOWN)   ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 2,  (input & INPUT_UP)     ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 4,  (input & INPUT_LEFT)   ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 6,  (input & INPUT_RIGHT)  ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 8,  (input & INPUT_A)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 10, (input & INPUT_B)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 12, (input & INPUT_C)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 14, (input & INPUT_D)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 16, (input & INPUT_START)  ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 18, (input & INPUT_SELECT) ? 1 : 0);
}

void WritePlayerInput(int player, uint16_t input) {
    WriteMatchInput(player, input);
}

uint16_t ReadPlayerInput(int player) {
    uintptr_t bufferAddr = (player == 0) ? ADDR_P1_INPUT_BUFFER : ADDR_P2_INPUT_BUFFER;
    uint16_t result = 0;
    for (int i = 0; i < 10; i++) {
        if (ReadMemory<uint16_t>(bufferAddr + i * 2) == 1) {
            result |= (1 << i);
        }
    }
    return result;
}

// ============================================================================
// ImGui Debug Content
// ============================================================================

void UpdateInputDebugInfo() {
    g_inputDebug.sdlInputP1 = InputSystem_GetInput(0);
    g_inputDebug.sdlInputP2 = InputSystem_GetInput(1);
    
    for (int i = 0; i < 10; i++) {
        g_inputDebug.gameBufferP1[i] = ReadMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + i * 2);
        g_inputDebug.gameBufferP2[i] = ReadMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + i * 2);
    }
    
    uintptr_t joyBase = ADDR_DINPUT_JOYSTICK;
    g_inputDebug.dinputJoyAxisX = ReadMemory<int32_t>(joyBase + 0);
    g_inputDebug.dinputJoyAxisY = ReadMemory<int32_t>(joyBase + 4);
    for (int i = 0; i < 8; i++) {
        g_inputDebug.dinputJoyButtons[i] = ReadMemory<uint8_t>(joyBase + 64 + i);
    }
    
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    g_inputDebug.keyState_Up    = keyBuffer[SCANCODE_UP];
    g_inputDebug.keyState_Down  = keyBuffer[SCANCODE_DOWN];
    g_inputDebug.keyState_Left  = keyBuffer[SCANCODE_LEFT];
    g_inputDebug.keyState_Right = keyBuffer[SCANCODE_RIGHT];
    g_inputDebug.keyState_Z     = keyBuffer[SCANCODE_Z];
    g_inputDebug.keyState_X     = keyBuffer[SCANCODE_X];
    g_inputDebug.keyState_Enter = keyBuffer[SCANCODE_ENTER];
}

void RenderInputDebugContent() {
    UpdateInputDebugInfo();
    
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "=== Hook Statistics ===");
    ImGui::Text("Total Hook Calls: %d", g_hookCallCount);
    ImGui::Text("Keyboard Hook: %d (injected: %d)", 
        g_inputDebug.keyboardHookCalls, g_inputDebug.keyboardInjectedCount);
    ImGui::Text("Joystick Hook: %d (injected: %d)", 
        g_inputDebug.joystickHookCalls, g_inputDebug.joystickInjectedCount);
    
    if (g_hookCallCount == 0) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "WARNING: Hooks not being called!");
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Check if hooks installed correctly.");
    }
    
    ImGui::Text("Last keyCode: 0x%02X | playerID: 0x%08X", 
        g_inputDebug.lastKeyCode, g_inputDebug.lastPlayerID);
    ImGui::Text("Last origResult: 0x%04X -> finalResult: 0x%04X", 
        g_inputDebug.lastOrigResult, g_inputDebug.lastFinalResult);
    ImGui::Text("Last KB inject: 0x%04X | Last Joy inject: 0x%04X",
        g_inputDebug.lastInjectedKeyInput, g_inputDebug.lastInjectedJoyInput);
    
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 1, 1), "Player Mapping:");
    ImGui::Text("P1 JoyID: 0x%X | P2 JoyID: 0x%X | MappedTo: P%d", 
        g_inputDebug.p1JoyID, g_inputDebug.p2JoyID, g_inputDebug.lastMappedPlayer + 1);
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(0, 1, 1, 1), "=== SDL Input ===");
    ImGui::Text("P1 SDL: 0x%04X -> Game: 0x%04X", g_inputDebug.sdlInputP1, g_inputDebug.gameInputP1);
    ImGui::Text("P2 SDL: 0x%04X -> Game: 0x%04X", g_inputDebug.sdlInputP2, g_inputDebug.gameInputP2);
    
    // Visual P1 buttons
    ImGui::Text("P1: ");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_UP ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "U");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_DOWN ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_LEFT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "L");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_RIGHT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "R");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_A ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "A");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_B ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "B");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_C ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "C");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_D ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_START ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "St");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_SELECT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "Se");
    
    // Visual P2 buttons
    ImGui::Text("P2: ");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_UP ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "U");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_DOWN ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_LEFT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "L");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_RIGHT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "R");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_A ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "A");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_B ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "B");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_C ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "C");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_D ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_START ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "St");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_SELECT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "Se");
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "=== Game Match Buffers ===");
    ImGui::Text("P1@%08X: D%d U%d L%d R%d A%d B%d C%d D%d St%d Se%d",
        ADDR_P1_INPUT_BUFFER,
        g_inputDebug.gameBufferP1[0], g_inputDebug.gameBufferP1[1],
        g_inputDebug.gameBufferP1[2], g_inputDebug.gameBufferP1[3],
        g_inputDebug.gameBufferP1[4], g_inputDebug.gameBufferP1[5],
        g_inputDebug.gameBufferP1[6], g_inputDebug.gameBufferP1[7],
        g_inputDebug.gameBufferP1[8], g_inputDebug.gameBufferP1[9]);
    ImGui::Text("P2@%08X: D%d U%d L%d R%d A%d B%d C%d D%d St%d Se%d",
        ADDR_P2_INPUT_BUFFER,
        g_inputDebug.gameBufferP2[0], g_inputDebug.gameBufferP2[1],
        g_inputDebug.gameBufferP2[2], g_inputDebug.gameBufferP2[3],
        g_inputDebug.gameBufferP2[4], g_inputDebug.gameBufferP2[5],
        g_inputDebug.gameBufferP2[6], g_inputDebug.gameBufferP2[7],
        g_inputDebug.gameBufferP2[8], g_inputDebug.gameBufferP2[9]);
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "=== DirectInput ===");
    ImGui::Text("Joy@%08X: X=%d Y=%d", 
        ADDR_DINPUT_JOYSTICK, g_inputDebug.dinputJoyAxisX, g_inputDebug.dinputJoyAxisY);
    ImGui::Text("Joy Btns: %02X %02X %02X %02X %02X %02X %02X %02X",
        g_inputDebug.dinputJoyButtons[0], g_inputDebug.dinputJoyButtons[1],
        g_inputDebug.dinputJoyButtons[2], g_inputDebug.dinputJoyButtons[3],
        g_inputDebug.dinputJoyButtons[4], g_inputDebug.dinputJoyButtons[5],
        g_inputDebug.dinputJoyButtons[6], g_inputDebug.dinputJoyButtons[7]);
    ImGui::Text("KB@%08X: U=%02X D=%02X L=%02X R=%02X Z=%02X X=%02X",
        ADDR_DINPUT_KEYBOARD,
        g_inputDebug.keyState_Up, g_inputDebug.keyState_Down,
        g_inputDebug.keyState_Left, g_inputDebug.keyState_Right,
        g_inputDebug.keyState_Z, g_inputDebug.keyState_X);
    
    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Hook Status:");
    ImGui::Text("KB hook: %s (orig: %p)", 
        g_origKeyboardState ? "OK" : "FAIL", g_origKeyboardState);
    ImGui::Text("Joy hook: %s (orig: %p)", 
        g_origJoystickState ? "OK" : "FAIL", g_origJoystickState);
}
