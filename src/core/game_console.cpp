/**
 * Alice Senki 2 - Game Debug Console
 * Hooks the game's internal Log_Write function to capture all game debug output
 */

#include "game_console.h"
#include "log_window.h"
#include "as2_constants.h"
#include "MinHook.h"
#include <Windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// int __cdecl Log_Write(CHAR *message)
#define ADDR_LOG_WRITE    0x6320D0

// int Log_Printf(char *Format, ...)
#define ADDR_LOG_PRINTF   0x632320

// Same RAM as ADDR_GAME_WNDPROC_CUSTOM_HANDLER in as2_constants.h (wndproc + logging).
#define ADDR_LOG_DEBUG_ONLY    ADDR_GAME_WNDPROC_CUSTOM_HANDLER

// Game's file logging disabled flag - when 0, Log_Write outputs to file
#define ADDR_LOG_FILE_DISABLED 0x9DC00C

static bool g_initialized = false;
static bool g_enabled = false;
static volatile bool g_inHook = false;  // Reentrancy guard
static int g_callCount = 0;

// int __cdecl Log_Write(char* message)
typedef int (__cdecl* Log_Write_t)(char* message);
static Log_Write_t g_originalLogWrite = nullptr;

typedef int (__cdecl* Log_Printf_t)(const char* format, ...);
static Log_Printf_t g_gameLogPrintf = nullptr;

static int __cdecl Hook_LogWrite(char* message) {
    g_callCount++;
    
    // Capture the message BEFORE calling original (in case original modifies it)
    if (g_enabled && message != nullptr && !g_inHook) {
        g_inHook = true;
        
        size_t len = strlen(message);
        if (len >= 1 && len < 2048) {
            char buffer[2048];
            memcpy(buffer, message, len);
            buffer[len] = '\0';

            while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' ||
                              buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
                buffer[--len] = '\0';
            }

            if (len > 0) {
                LOG_INFO("[Game] %s", buffer);
            }
        }
        
        g_inHook = false;
    }
    
    // Always call original
    if (g_originalLogWrite) {
        return g_originalLogWrite(message);
    }
    return -1;
}

void GameConsole_Init() {
    if (g_initialized) {
        return;
    }

    g_gameLogPrintf = (Log_Printf_t)ADDR_LOG_PRINTF;

    MH_STATUS status = MH_CreateHook(
        (LPVOID)ADDR_LOG_WRITE,
        (LPVOID)Hook_LogWrite,
        (LPVOID*)&g_originalLogWrite
    );
    
    if (status != MH_OK) {
        LOG_ERROR("[GameConsole] Failed to create Log_Write hook: %d", status);
        return;
    }
    
    status = MH_EnableHook((LPVOID)ADDR_LOG_WRITE);
    if (status != MH_OK) {
        LOG_ERROR("[GameConsole] Failed to enable Log_Write hook: %d", status);
        return;
    }

    g_initialized = true;
    g_callCount = 0;
    LOG_INFO("[GameConsole] Initialized - hooked game Log_Write @ 0x%X", ADDR_LOG_WRITE);
}

void GameConsole_Shutdown() {
    if (!g_initialized) {
        return;
    }

    GameConsole_SetEnabled(false);

    MH_DisableHook((LPVOID)ADDR_LOG_WRITE);

    g_initialized = false;
    LOG_INFO("[GameConsole] Shutdown complete (total calls: %d)", g_callCount);
}

void GameConsole_SetEnabled(bool enable) {
    if (enable == g_enabled) {
        return;
    }

    g_enabled = enable;
    
    if (enable) {
        LOG_INFO("[GameConsole] Enabled - capturing game Log_Write (calls so far: %d)", g_callCount);
    }
    else {
        LOG_INFO("[GameConsole] Disabled (calls so far: %d)", g_callCount);
    }
}

bool GameConsole_IsEnabled() {
    return g_enabled;
}

int GameConsole_GetCallCount() {
    return g_callCount;
}

void GameConsole_TestLog() {
    if (g_gameLogPrintf) {
        LOG_INFO("[GameConsole] Calling game's Log_Printf to test hook...");
        g_gameLogPrintf("=== MOD TEST: GameConsole hook test message ===");
        LOG_INFO("[GameConsole] Test complete (calls now: %d)", g_callCount);
    } else {
        LOG_ERROR("[GameConsole] Game Log_Printf not available");
    }
}

void GameConsole_Write(const char* message) {
    if (message == nullptr) {
        return;
    }
    LOG_INFO("[GameConsole] %s", message);
}

void GameConsole_Printf(const char* format, ...) {
    if (format == nullptr) {
        return;
    }
    
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    LOG_INFO("[GameConsole] %s", buffer);
}
