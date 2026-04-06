/**
 * Alice Senki 2 - Game Debug Console
 * Hooks the game's internal Log_Write function to capture all game debug output
 */

#include "game_console.h"
#include "log_window.h"
#include "MinHook.h"
#include <Windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// ============================================================================
// Game Memory Addresses
// ============================================================================

// Game's Log_Write function - this is where ALL game logging goes through
// int __cdecl Log_Write(CHAR *message)
#define ADDR_LOG_WRITE    0x6320D0

// Game's Log_Printf function - formats and calls Log_Write
// int Log_Printf(char *Format, ...)
#define ADDR_LOG_PRINTF   0x632320

// Game's debug-only flag - when 1, Log_Write outputs to OutputDebugStringA
// Note: Setting this can cause side effects in other parts of the game
#define ADDR_LOG_DEBUG_ONLY    0x9DB660

// Game's file logging disabled flag - when 0, Log_Write outputs to file
#define ADDR_LOG_FILE_DISABLED 0x9DC00C

// ============================================================================
// State
// ============================================================================

static bool g_initialized = false;
static bool g_enabled = false;
static volatile bool g_inHook = false;  // Reentrancy guard
static int g_callCount = 0;  // Track how many times Log_Write is called

// Hook storage - Log_Write signature: int __cdecl Log_Write(char* message)
typedef int (__cdecl* Log_Write_t)(char* message);
static Log_Write_t g_originalLogWrite = nullptr;

// Direct function pointer to game's Log_Printf
typedef int (__cdecl* Log_Printf_t)(const char* format, ...);
static Log_Printf_t g_gameLogPrintf = nullptr;

// ============================================================================
// Log_Write Hook
// ============================================================================

static int __cdecl Hook_LogWrite(char* message) {
    g_callCount++;  // Always count calls
    
    // Capture the message BEFORE calling original (in case original modifies it)
    if (g_enabled && message != nullptr && !g_inHook) {
        g_inHook = true;
        
        size_t len = strlen(message);
        if (len >= 1 && len < 2048) {
            // Create a clean copy without trailing whitespace
            char buffer[2048];
            memcpy(buffer, message, len);
            buffer[len] = '\0';
            
            // Trim trailing whitespace
            while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' || 
                              buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
                buffer[--len] = '\0';
            }
            
            // Only log if there's actual content
            if (len > 0) {
                // Output to our log window
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

// ============================================================================
// Public API
// ============================================================================

void GameConsole_Init() {
    if (g_initialized) {
        return;
    }
    
    // Get pointer to game's Log_Printf for testing
    g_gameLogPrintf = (Log_Printf_t)ADDR_LOG_PRINTF;
    
    // Hook the game's Log_Write function directly
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
    
    // Disable the console
    GameConsole_SetEnabled(false);
    
    // Disable the hook
    MH_DisableHook((LPVOID)ADDR_LOG_WRITE);
    
    g_initialized = false;
    LOG_INFO("[GameConsole] Shutdown complete (total calls: %d)", g_callCount);
}

void GameConsole_SetEnabled(bool enable) {
    if (enable == g_enabled) {
        return;  // No change
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
    // Call the game's Log_Printf to test our hook
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
