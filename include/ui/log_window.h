/**
 * Alice Senki 2 - Log Window
 * Real-time scrolling log display using ImGui
 * 
 * Features:
 * - Log categories for filtering subsystems
 * - Rate limiting to prevent spam
 * - Duplicate message suppression
 * - Separate packet log file
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>

// ============================================================================
// Log Levels
// ============================================================================

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3
} LogLevel;

// ============================================================================
// Log Categories - Enable/disable logging for specific subsystems
// ============================================================================

typedef enum {
    LOG_CAT_GENERAL   = 0,    // General mod messages
    LOG_CAT_STATE     = 1,    // Game state changes (mode, substate)
    LOG_CAT_HANDSHAKE = 2,    // Mod handshake protocol
    LOG_CAT_READYSYNC = 3,    // Ready sync protocol
    LOG_CAT_NETPLAY   = 4,    // Netplay session management
    LOG_CAT_INPUT     = 5,    // Input sync hooks
    LOG_CAT_ROLLBACK  = 6,    // Rollback system
    LOG_CAT_SAVESTATE = 7,    // Savestate save/load
    LOG_CAT_TIMING    = 8,    // Frame timing / timewarp
    LOG_CAT_PACKET    = 9,    // Packet send/receive (very verbose)
    LOG_CAT_PUMP      = 10,   // Socket pump operations (very verbose)
    LOG_CAT_COUNT     = 11
} LogCategory;

// ============================================================================
// Public API
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Set external log directory (called by d3d9_proxy before LogWindow_Init)
void LogWindow_SetLogDir(const char* dir);

// Get the current session log directory (e.g. "logs/2026-04-04_12-00-00")
// Returns empty string if not yet set.
const char* LogWindow_GetLogDir(void);

// Initialize log window (call once at startup)
void LogWindow_Init(void);

// Shutdown and cleanup
void LogWindow_Shutdown(void);

// Force logs to hit disk immediately while debugging startup crashes.
void LogWindow_SetForceFlush(bool enabled);
void LogWindow_Flush(void);
// Flushes any buffered log lines at most once per second; call once per frame.
void LogWindow_PeriodicFlush(void);

// Standard logging
void LogWindow_Log(LogLevel level, const char* fmt, ...);
void LogWindow_LogV(LogLevel level, const char* fmt, va_list args);

// Category-based logging
void LogWindow_LogCat(LogCategory cat, LogLevel level, const char* fmt, ...);
void LogWindow_LogCatV(LogCategory cat, LogLevel level, const char* fmt, va_list args);

// Rate-limited logging - logs at most once per intervalMs for same message
void LogWindow_LogRateLimited(LogLevel level, unsigned int intervalMs, const char* fmt, ...);

// Log only on state change - returns true if value changed
bool LogWindow_LogOnChange_Int(const char* name, int* prevValue, int newValue, LogLevel level, const char* fmt, ...);
bool LogWindow_LogOnChange_Bool(const char* name, bool* prevValue, bool newValue, LogLevel level, const char* fmt, ...);

// Packet log (separate file, not shown in window)
void LogWindow_LogPacket(LogLevel level, const char* fmt, ...);
void LogWindow_LogPacketV(LogLevel level, const char* fmt, va_list args);

// Netcode log (separate file: as2_netcode.log — always enabled, not shown in imgui)
void LogWindow_LogNet(LogLevel level, const char* fmt, ...);
void LogWindow_LogNetV(LogLevel level, const char* fmt, va_list args);

// Gekko log (separate file: as2_gekko.log — always enabled, not shown in imgui)
void LogWindow_LogGekko(LogLevel level, const char* fmt, ...);
void LogWindow_LogGekkoV(LogLevel level, const char* fmt, va_list args);

// Category management
void LogWindow_SetCategoryEnabled(LogCategory cat, bool enabled);
bool LogWindow_IsCategoryEnabled(LogCategory cat);
void LogWindow_SetAllCategoriesEnabled(bool enabled);
const char* LogWindow_GetCategoryName(LogCategory cat);

// Convenience macros - Standard
#define LOG_DEBUG(fmt, ...)   LogWindow_Log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    LogWindow_Log(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    LogWindow_Log(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   LogWindow_Log(LOG_ERROR, fmt, ##__VA_ARGS__)

// Convenience macros - By category (checks category enabled first)
#define LOG_CAT(cat, level, fmt, ...) \
    do { if (LogWindow_IsCategoryEnabled(cat)) LogWindow_LogCat(cat, level, fmt, ##__VA_ARGS__); } while(0)

#define LOG_STATE(level, fmt, ...)     LOG_CAT(LOG_CAT_STATE, level, fmt, ##__VA_ARGS__)
#define LOG_HANDSHAKE(level, fmt, ...) LOG_CAT(LOG_CAT_HANDSHAKE, level, fmt, ##__VA_ARGS__)
#define LOG_READYSYNC(level, fmt, ...) LOG_CAT(LOG_CAT_READYSYNC, level, fmt, ##__VA_ARGS__)
#define LOG_NETPLAY(level, fmt, ...)   LOG_CAT(LOG_CAT_NETPLAY, level, fmt, ##__VA_ARGS__)
#define LOG_INPUT(level, fmt, ...)     LOG_CAT(LOG_CAT_INPUT, level, fmt, ##__VA_ARGS__)
#define LOG_ROLLBACK(level, fmt, ...)  LOG_CAT(LOG_CAT_ROLLBACK, level, fmt, ##__VA_ARGS__)
#define LOG_SAVESTATE(level, fmt, ...) LOG_CAT(LOG_CAT_SAVESTATE, level, fmt, ##__VA_ARGS__)
#define LOG_TIMING(level, fmt, ...)    LOG_CAT(LOG_CAT_TIMING, level, fmt, ##__VA_ARGS__)
#define LOG_PACKET(level, fmt, ...)    LOG_CAT(LOG_CAT_PACKET, level, fmt, ##__VA_ARGS__)
#define LOG_PUMP(level, fmt, ...)      LOG_CAT(LOG_CAT_PUMP, level, fmt, ##__VA_ARGS__)

// Rate-limited logging (logs at most once per N ms for same location)
#define LOG_RATE_LIMITED(level, intervalMs, fmt, ...) \
    LogWindow_LogRateLimited(level, intervalMs, fmt, ##__VA_ARGS__)

// Log once per second max
#define LOG_DEBUG_1S(fmt, ...)  LOG_RATE_LIMITED(LOG_DEBUG, 1000, fmt, ##__VA_ARGS__)
#define LOG_INFO_1S(fmt, ...)   LOG_RATE_LIMITED(LOG_INFO, 1000, fmt, ##__VA_ARGS__)

// Packet log macros (separate file)
#define LOG_PKT_DEBUG(fmt, ...)   LogWindow_LogPacket(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_PKT_INFO(fmt, ...)    LogWindow_LogPacket(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_PKT_WARN(fmt, ...)    LogWindow_LogPacket(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_PKT_ERROR(fmt, ...)   LogWindow_LogPacket(LOG_ERROR, fmt, ##__VA_ARGS__)

// Netcode log macros (separate file: as2_netcode.log — always written)
#define LOG_NET_DEBUG(fmt, ...)   LogWindow_LogNet(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_NET_INFO(fmt, ...)    LogWindow_LogNet(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_NET_WARN(fmt, ...)    LogWindow_LogNet(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_NET_ERROR(fmt, ...)   LogWindow_LogNet(LOG_ERROR, fmt, ##__VA_ARGS__)

// Gekko log macros (separate file: as2_gekko.log — always written)
#define LOG_GEKKO_DEBUG(fmt, ...)   LogWindow_LogGekko(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_GEKKO_INFO(fmt, ...)    LogWindow_LogGekko(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_GEKKO_WARN(fmt, ...)    LogWindow_LogGekko(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_GEKKO_ERROR(fmt, ...)   LogWindow_LogGekko(LOG_ERROR, fmt, ##__VA_ARGS__)

// Render the log window (call from ImGui render loop)
void LogWindow_Render(bool* pOpen);

// Clear all logs
void LogWindow_Clear(void);

// Get log count
int LogWindow_GetCount(void);

// Set maximum log entries (default: 1000)
void LogWindow_SetMaxEntries(int max);

// Auto-scroll control
void LogWindow_SetAutoScroll(bool enabled);
bool LogWindow_GetAutoScroll(void);

// Filter by log level
void LogWindow_SetMinLevel(LogLevel level);
LogLevel LogWindow_GetMinLevel(void);

// Write logs to file
bool LogWindow_WriteToFile(const char* filename);

// Render just the log content (for embedding in another window)
void LogWindow_RenderContent(void);

#ifdef __cplusplus
}
#endif
