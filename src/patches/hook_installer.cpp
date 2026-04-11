#include "patches/hook_installer.h"
#include "patches/input_override.h"
#include "patches/input_sync_hooks.h"
#include "patches/tick_hooks.h"
#include "patches/locale_patch.h"
#include "patches/filesystem_patch.h"
#include "patches/palette_asset_hook.h"
#include "training/practice_tools.h"
#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"

bool InstallHooks() {
    LOG_INFO("Installing hooks...");
    
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("MH_Initialize failed! Status: %d", status);
        return false;
    }
    
    // --- Input hooks ---
    
    LOG_INFO("ADDR_KEYBOARD_STATE = 0x%08X (sub_62FD00)", ADDR_KEYBOARD_STATE);
    LOG_INFO("ADDR_JOYSTICK_STATE = 0x%08X (sub_62FF50)", ADDR_JOYSTICK_STATE);
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_KEYBOARD_STATE),
            reinterpret_cast<void*>(&Hook_KeyboardState),
            reinterpret_cast<void**>(&g_origKeyboardState));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook KeyboardState! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_62FD00 (keyboard state)");
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_JOYSTICK_STATE),
            reinterpret_cast<void*>(&Hook_JoystickState),
            reinterpret_cast<void**>(&g_origJoystickState));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook JoystickState! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_62FF50 (joystick state)");
    
    LOG_INFO("ADDR_DINPUT_KB_REFRESH = 0x%08X (sub_630130)", ADDR_DINPUT_KB_REFRESH);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_DINPUT_KB_REFRESH),
            reinterpret_cast<void*>(&Hook_DInputKBRefresh),
            reinterpret_cast<void**>(&g_origDInputKBRefresh));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DInputKBRefresh! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_630130 (DirectInput keyboard buffer refresh)");
    }
    
    LOG_INFO("ADDR_DINPUT_JOY_REFRESH = 0x%08X (sub_6302F0)", ADDR_DINPUT_JOY_REFRESH);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_DINPUT_JOY_REFRESH),
            reinterpret_cast<void*>(&Hook_DInputJoyRefresh),
            reinterpret_cast<void**>(&g_origDInputJoyRefresh));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DInputJoyRefresh! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_6302F0 (DirectInput joystick buffer refresh)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetKeyboardState),
            reinterpret_cast<void*>(&Hook_GetKeyboardState),
            reinterpret_cast<void**>(&g_origGetKeyboardState));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetKeyboardState! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetKeyboardState (prevents Alt+Shift issues)");
    }
    
    LOG_INFO("ADDR_INPUT_PROCESS = 0x%08X (sub_562060)", ADDR_INPUT_PROCESS);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_INPUT_PROCESS),
            reinterpret_cast<void*>(&Hook_InputProcess),
            reinterpret_cast<void**>(&g_origInputProcess));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook InputProcess! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_562060 (input processing - just pressed flags)");
    
    // --- Input dispatcher hook (charsel lockstep) ---
    
    LOG_INFO("ADDR_INPUT_DISPATCHER = 0x%08X (sub_5625E0)", ADDR_INPUT_DISPATCHER);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_INPUT_DISPATCHER),
            reinterpret_cast<void*>(&Hook_InputDispatcher),
            reinterpret_cast<void**>(&g_origInputDispatcher));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook InputDispatcher! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_5625E0 (input dispatcher - charsel lockstep)");
    
    // --- Locale hooks ---
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetOEMCP),
            reinterpret_cast<void*>(&Hook_GetOEMCP),
            reinterpret_cast<void**>(&g_origGetOEMCP));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetOEMCP! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetOEMCP (Japanese locale patch - returns 932)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetACP),
            reinterpret_cast<void*>(&Hook_GetACP),
            reinterpret_cast<void**>(&g_origGetACP));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetACP! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetACP (Japanese locale patch - returns 932)");
    }
    
    // --- Filesystem hooks ---
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&CreateFileA),
            reinterpret_cast<void*>(&Hook_CreateFileA),
            reinterpret_cast<void**>(&g_origCreateFileA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook CreateFileA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked CreateFileA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&DeleteFileA),
            reinterpret_cast<void*>(&Hook_DeleteFileA),
            reinterpret_cast<void**>(&g_origDeleteFileA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DeleteFileA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked DeleteFileA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&FindFirstFileA),
            reinterpret_cast<void*>(&Hook_FindFirstFileA),
            reinterpret_cast<void**>(&g_origFindFirstFileA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook FindFirstFileA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked FindFirstFileA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetFileAttributesA),
            reinterpret_cast<void*>(&Hook_GetFileAttributesA),
            reinterpret_cast<void**>(&g_origGetFileAttributesA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetFileAttributesA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked GetFileAttributesA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&MultiByteToWideChar),
            reinterpret_cast<void*>(&Hook_MultiByteToWideChar),
            reinterpret_cast<void**>(&g_origMultiByteToWideChar));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook MultiByteToWideChar! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked MultiByteToWideChar (CP_ACP/CP_OEMCP -> CP932 redirect)");
    }
    
    // --- Tick/timing hook ---
    
    LOG_INFO("ADDR_GET_TICK = 0x%08X (sub_635F80)", ADDR_GET_TICK);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_GET_TICK),
            reinterpret_cast<void*>(&Hook_GetTick),
            reinterpret_cast<void**>(&g_origGetTick));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetTick (sub_635F80)! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_635F80 (tick/time source)");
    }

    // --- Command history hook (practice mode: redirect to P2 when swapped) ---

    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_CMD_HISTORY_UPDATE),
            reinterpret_cast<void*>(&Hook_CmdHistoryUpdate),
            reinterpret_cast<void**>(&g_origCmdHistoryUpdate));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook CmdHistoryUpdate! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_4C8C50 (command history update - practice swap redirect)");
    }

    if (!PaletteAssetHook_Install()) {
        LOG_WARN("Failed to install character palette asset hook (continuing anyway)");
    }
    
    // --- Enable all hooks ---
    
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        LOG_ERROR("MH_EnableHook failed! Status: %d", status);
        return false;
    }
    
    LOG_INFO("Input hooks installed and enabled!");

    // --- Vanilla netplay suppression hooks ---
    // These must be installed AFTER MH_EnableHook(MH_ALL_HOOKS) since
    // InputSyncHooks_Install creates + enables its own hooks.
    if (!InputSyncHooks_Install()) {
        LOG_WARN("Failed to install vanilla netplay suppression hooks (continuing anyway)");
    }

    return true;
}

void RemoveHooks() {
    LOG_INFO("Removing hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
