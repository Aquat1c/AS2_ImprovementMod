/**
 * Alice Senki 2 - Game Debug Console
 * Hooks the game's internal Log_Write function to capture all game debug output
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Game Console API
// ============================================================================

/**
 * Initialize the game console system.
 * Hooks the game's Log_Write function.
 * Call this during mod initialization.
 */
void GameConsole_Init();

/**
 * Shutdown the game console system.
 */
void GameConsole_Shutdown();

/**
 * Enable or disable game log capture.
 * When enabled, captured Log_Write messages appear in our log window.
 */
void GameConsole_SetEnabled(bool enable);

/**
 * Check if game log capture is currently enabled.
 */
bool GameConsole_IsEnabled();

/**
 * Get the number of times Log_Write has been called since init.
 * Useful for debugging to verify the hook is working.
 */
int GameConsole_GetCallCount();

/**
 * Call the game's Log_Printf to test the hook.
 * This sends a test message through the game's logging system.
 */
void GameConsole_TestLog();

/**
 * Write a message directly to the game console (if open).
 * Useful for mod messages that should appear in the game console.
 */
void GameConsole_Write(const char* message);

/**
 * Write a formatted message to the game console.
 */
void GameConsole_Printf(const char* format, ...);

#ifdef __cplusplus
}
#endif
