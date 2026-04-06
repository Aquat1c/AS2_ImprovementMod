/**
 * Alice Senki 2 - Mod Main Module
 *
 * Slim lifecycle / game-state core.  All rollback logic has been removed.
 * Input hooks live in input_override, tick hooks in tick_hooks,
 * hook installation in hook_installer.
 */

#include "mod_main.h"
#include "input_system.h"
#include "log_window.h"
#include "mod_menu.h"
#include "game_console.h"
#include "patches/memory_utils.h"
#include "patches/unlock_patch.h"
#include "patches/input_override.h"
#include "patches/hook_installer.h"
#include "patches/tick_hooks.h"
#include "imgui.h"
#include <stdio.h>
#include <string.h>
#include <mmsystem.h>

// ============================================================================
// Configuration
// ============================================================================

struct ModConfig {
    bool useSDLInput;
    bool bypassGameInput;
    int  inputDisplayMode;
    bool showHitboxes;
    bool showFrameData;
    bool verboseLogging;
    int  rollbackFrames;
    int  inputDelay;
};

static ModConfig g_config = {
    true,   // useSDLInput
    false,  // bypassGameInput
    1,      // inputDisplayMode
    false,  // showHitboxes
    false,  // showFrameData
    false,  // verboseLogging
    8,      // rollbackFrames (unused, kept for compat)
    0       // inputDelay    (unused, kept for compat)
};

// ============================================================================
// Internal state
// ============================================================================

static HMODULE g_gameModule = nullptr;
static bool g_initialized = false;
static bool g_forceBorderlessFullscreen = false;

// ============================================================================
// Config accessors (for other modules)
// ============================================================================

bool ModConfig_UseSDLInput() {
    return g_config.useSDLInput;
}

bool ModConfig_VerboseLogging() {
    return g_config.verboseLogging;
}

// ============================================================================
// Verbose logging control
// ============================================================================

void SetVerboseLogging(bool enabled) {
    g_config.verboseLogging = enabled;
}

bool GetVerboseLogging() {
    return g_config.verboseLogging;
}

// ============================================================================
// Game State Queries
// ============================================================================

uint32_t AS2_GetFrameNumber() {
    return ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

bool AS2_IsInMatch() {
    return GetGameMode() == MODE_MATCH;
}

bool AS2_IsInGameplay() {
    return IsInActiveGameplay();
}

bool AS2_IsInPlayableGameplay() {
    return IsInPlayableMatchGameplay();
}

bool AS2_IsInPauseMenu() {
    return GetGameMode() == MODE_MATCH && GetSubstate() == 4;
}

bool AS2_IsInMenu() {
    uint32_t mode = GetGameMode();
    return mode != MODE_MATCH || GetSubstate() != 3;
}

// ============================================================================
// Entity / HP access
// ============================================================================

uint16_t GetP1HP() {
    return ReadMemory<uint16_t>(ADDR_P1_HP_DIRECT);
}

uint16_t GetP2HP() {
    return ReadMemory<uint16_t>(ADDR_P2_HP_DIRECT);
}

uintptr_t GetEntityBase(int player) {
    if (player == 0) {
        return ADDR_P1_HP_DIRECT - ENTITY_OFF_HP;
    } else {
        return ADDR_P2_HP_DIRECT - ENTITY_OFF_HP;
    }
}

// ============================================================================
// Quick Checksum (replicates sub_49EE60)
// ============================================================================

uint32_t AS2_GetQuickChecksum() {
    uintptr_t p1Base = GetEntityBase(0);
    uintptr_t p2Base = GetEntityBase(1);

    uint16_t p1HP = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_HP);
    uint16_t p1Meter = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_METER);
    int16_t  p1X = ReadMemory<int16_t>(p1Base + ENTITY_OFF_X_POS);
    int16_t  p1Y = ReadMemory<int16_t>(p1Base + ENTITY_OFF_Y_POS);

    uint16_t p2HP = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_HP);
    uint16_t p2Meter = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_METER);
    int16_t  p2X = ReadMemory<int16_t>(p2Base + ENTITY_OFF_X_POS);
    int16_t  p2Y = ReadMemory<int16_t>(p2Base + ENTITY_OFF_Y_POS);

    uint32_t sum = (uint32_t)p1HP + (uint32_t)p1Meter + (uint32_t)(uint16_t)p1X + (uint32_t)(uint16_t)p1Y
                 + (uint32_t)p2HP + (uint32_t)p2Meter + (uint32_t)(uint16_t)p2X + (uint32_t)(uint16_t)p2Y;

    return (uint16_t)(sum % 0x10000);
}

// ============================================================================
// Clear Vanilla Netplay Buffers
// ============================================================================

void AS2_ClearVanillaNetplayBuffers() {
    LOG_INFO("[AS2] Clearing vanilla netplay buffers...");

    __try {
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_DISPLAY) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_WRITE_IDX) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_NET_IDX) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_REMOTE_FRAME) = 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla frame counters");
    }

    __try {
        memset(reinterpret_cast<void*>(ADDR_VANILLA_LOCAL_INPUTS), 0xFF, VANILLA_LOCAL_INPUT_SIZE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla local input buffer");
    }

    __try {
        memset(reinterpret_cast<void*>(ADDR_VANILLA_REMOTE_INPUTS), 0xFF, VANILLA_REMOTE_INPUT_SIZE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla remote input buffer");
    }

    __try {
        memset(reinterpret_cast<void*>(ADDR_VANILLA_SYNC_LOCAL), 0, 10);
        memset(reinterpret_cast<void*>(ADDR_VANILLA_SYNC_REMOTE), 0, 10);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla sync flags");
    }
}

// ============================================================================
// Deferred Initialization (first frame when D3D9 is ready)
// ============================================================================

static void DeferredInit() {
    LOG_INFO("Performing deferred initialization...");

    if (!InputSystem_Init()) {
        LOG_WARN("Failed to initialize SDL input system - using native input only");
    } else {
        LOG_INFO("SDL3 input system initialized");
    }

    if (!InstallHooks()) {
        LOG_ERROR("Failed to install hooks!");
        return;
    }

    GameConsole_Init();

    LOG_INFO("Frame Counter: 0x%08X = %d", ADDR_SIM_FRAME_COUNTER, AS2_GetFrameNumber());
    LOG_INFO("Game Mode: 0x%08X = %d", ADDR_GAME_MODE, GetGameMode());
    LOG_INFO("P1 HP: 0x%08X = %d", ADDR_P1_HP_DIRECT, GetP1HP());
    LOG_INFO("P2 HP: 0x%08X = %d", ADDR_P2_HP_DIRECT, GetP2HP());

    g_initialized = true;

    ModMenu_Init();

    LOG_INFO("========================================");
    LOG_INFO("Initialization complete!");
    LOG_INFO("Hotkeys: F1=Menu");
    LOG_INFO("========================================");
}

// ============================================================================
// Exported Functions
// ============================================================================

extern "C" {

__declspec(dllexport) void ModSetImGuiContext(void* ctx) {
    ImGui::SetCurrentContext((ImGuiContext*)ctx);
}

__declspec(dllexport) void ModSetLogDir(const char* dir) {
    LogWindow_SetLogDir(dir);
}

__declspec(dllexport) void ModInit(HMODULE gameModule) {
    g_gameModule = gameModule;

    LogWindow_Init();
    timeBeginPeriod(1);

    LOG_INFO("========================================");
    LOG_INFO("Alice Senki 2 - Mod v0.3");
    LOG_INFO("Build: %s %s", __DATE__, __TIME__);
    LOG_INFO("========================================");
    LOG_INFO("Game module: 0x%p", gameModule);

    HMODULE selfModule = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&ModInit, &selfModule)) {
        LOG_INFO("Mod DLL base: 0x%p", selfModule);
    }

    LOG_INFO("Initialization deferred - will complete when game is ready...");
}

__declspec(dllexport) void ModShutdown() {
    LOG_INFO("Mod shutdown...");

    if (g_initialized) {
        RemoveHooks();
        InputSystem_Shutdown();
    }

    timeEndPeriod(1);
    LogWindow_Shutdown();
}

__declspec(dllexport) void ModOnGameExit(int exitCode, const char* reason) {
    LOG_INFO("=====================================================");
    if (exitCode == 0) {
        LOG_INFO("[EXIT] Game exiting normally: %s", reason ? reason : "Unknown");
    } else {
        LOG_ERROR("[EXIT] Game CRASHED! Exit code: %d", exitCode);
        LOG_ERROR("[EXIT] Reason: %s", reason ? reason : "Unknown");
    }
    LOG_INFO("=====================================================");
    LOG_INFO("[EXIT] Cleanup complete");
}

__declspec(dllexport) void ModOnFrame() {
    if (!g_initialized) {
        DeferredInit();
    }
    if (!g_initialized) return;

    // Apply content unlocks after config.dat has been loaded
    {
        static bool s_unlockApplied = false;
        if (!s_unlockApplied) {
            uint32_t version = ReadMemory<uint32_t>(ADDR_CONFIG_VERSION);
            if (version == 258) {
                UnlockAllContent();
                s_unlockApplied = true;
            }
        }
    }

    // Update debug info
    UpdateInputDebugInfo();

    // Log when SDL input changes
    static uint16_t prevSdlP1 = 0;
    static uint16_t prevSdlP2 = 0;

    uint16_t sdlP1 = g_inputDebug.sdlInputP1;
    uint16_t sdlP2 = g_inputDebug.sdlInputP2;

    if (sdlP1 != prevSdlP1) {
        if (g_config.verboseLogging) {
            LOG_INFO("[INPUT] SDL P1: 0x%04X -> 0x%04X (U%d D%d L%d R%d A%d B%d C%d D%d St%d Se%d)",
                prevSdlP1, sdlP1,
                (sdlP1 & INPUT_UP) ? 1 : 0, (sdlP1 & INPUT_DOWN) ? 1 : 0,
                (sdlP1 & INPUT_LEFT) ? 1 : 0, (sdlP1 & INPUT_RIGHT) ? 1 : 0,
                (sdlP1 & INPUT_A) ? 1 : 0, (sdlP1 & INPUT_B) ? 1 : 0,
                (sdlP1 & INPUT_C) ? 1 : 0, (sdlP1 & INPUT_D) ? 1 : 0,
                (sdlP1 & INPUT_START) ? 1 : 0, (sdlP1 & INPUT_SELECT) ? 1 : 0);
        }
        prevSdlP1 = sdlP1;
    }

    if (sdlP2 != prevSdlP2) {
        if (g_config.verboseLogging) {
            LOG_INFO("[INPUT] SDL P2: 0x%04X -> 0x%04X", prevSdlP2, sdlP2);
        }
        prevSdlP2 = sdlP2;
    }

    // Track game mode changes
    static int prevGameMode = -1;
    static int prevSubState = -1;
    int curGameMode = GetGameMode();
    int curSubState = GetSubstate();

    if (curGameMode != prevGameMode) {
        if (g_config.verboseLogging) {
            LOG_INFO("[STATE] Game Mode changed: %d -> %d", prevGameMode, curGameMode);
        }
        prevGameMode = curGameMode;
    }
    if (curSubState != prevSubState) {
        if (g_config.verboseLogging) {
            LOG_INFO("[STATE] Sub-State changed: %d -> %d", prevSubState, curSubState);
        }
        prevSubState = curSubState;
    }

    // Periodic status log
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 300 == 0 && g_config.verboseLogging) {
        LOG_DEBUG("[STATUS] Hooks: KB=%d(%d inj) Joy=%d(%d inj) | SDL P1=0x%04X P2=0x%04X",
            g_inputDebug.keyboardHookCalls, g_inputDebug.keyboardInjectedCount,
            g_inputDebug.joystickHookCalls, g_inputDebug.joystickInjectedCount,
            g_inputDebug.sdlInputP1, g_inputDebug.sdlInputP2);
    }

    // Update SDL input
    InputSystem_Update();
}

__declspec(dllexport) void ModOnPresent(void* pDevice) {
    if (!g_initialized) return;
    ModMenu_Render();
}

__declspec(dllexport) bool ModWantsExclusiveOverlay() {
    return false;
}

__declspec(dllexport) void ModToggleMenu() {
    ModMenu_Toggle();
}

__declspec(dllexport) bool ModGetNetplayHudText(char* out, int cap) {
    (void)out; (void)cap;
    return false;
}

__declspec(dllexport) bool ModGetMatchHudData(MatchHudData* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    return false;
}

__declspec(dllexport) uint32_t GetCurrentFrame() {
    return AS2_GetFrameNumber();
}

__declspec(dllexport) uint16_t GetPlayerHP(int player) {
    return player == 0 ? GetP1HP() : GetP2HP();
}

__declspec(dllexport) bool* GetForceBorderlessPtr() {
    return &g_forceBorderlessFullscreen;
}

} // extern "C"
