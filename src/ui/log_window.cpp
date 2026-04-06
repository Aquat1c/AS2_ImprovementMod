/**
 * Alice Senki 2 - Log Window Implementation
 * 
 * Enhanced logging with:
 * - Categories for filtering subsystems
 * - Rate limiting to prevent spam  
 * - Duplicate message suppression
 * - State change detection
 */

#include "log_window.h"
#include "imgui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include <mutex>
#include <stdlib.h>
#include <unordered_map>
#include <windows.h>

// ============================================================================
// Internal Types
// ============================================================================

struct LogEntry {
    LogLevel level;
    LogCategory category;
    std::string timestamp;
    std::string message;
    ImVec4 color;
    int repeat_count;  // Number of times this message was repeated
};

// Rate limiting entry
struct RateLimitEntry {
    DWORD lastLogTime;
    std::string lastMessage;
};

// ============================================================================
// Internal State
// ============================================================================

static std::vector<LogEntry> g_logEntries;
static std::mutex g_logMutex;
static int g_maxEntries = 1000;
static bool g_autoScroll = true;
static LogLevel g_minLevel = LOG_DEBUG;
static bool g_scrollToBottom = false;
static FILE* g_logFile = nullptr;
static FILE* g_packetLogFile = nullptr;
static FILE* g_netcodeLogFile = nullptr;
static FILE* g_gekkoLogFile = nullptr;

// Category enabled flags (default: most enabled, verbose ones disabled)
static bool g_categoryEnabled[LOG_CAT_COUNT] = {
    true,   // LOG_CAT_GENERAL
    true,   // LOG_CAT_STATE
    true,   // LOG_CAT_HANDSHAKE
    true,   // LOG_CAT_READYSYNC
    true,   // LOG_CAT_NETPLAY
    true,   // LOG_CAT_INPUT
    true,   // LOG_CAT_ROLLBACK
    true,   // LOG_CAT_SAVESTATE
    false,  // LOG_CAT_TIMING (very verbose)
    false,  // LOG_CAT_PACKET (very verbose)
    false   // LOG_CAT_PUMP (very verbose)
};

static const char* g_categoryNames[LOG_CAT_COUNT] = {
    "General",
    "State",
    "Handshake",
    "ReadySync",
    "Netplay",
    "Input",
    "Rollback",
    "Savestate",
    "Timing",
    "Packet",
    "Pump"
};

// Rate limiting state
static std::unordered_map<std::string, RateLimitEntry> g_rateLimitMap;
static std::mutex g_rateLimitMutex;

// Packet logging is opt-in because it can be extremely high volume.
static bool g_packetLogEnabled = false;

// Avoid per-line flushes; flush only periodically / on errors.
static unsigned int g_logLinesSinceFlush = 0;
static unsigned int g_pktLinesSinceFlush = 0;
static unsigned int g_netLinesSinceFlush = 0;
static unsigned int g_gekkoLinesSinceFlush = 0;

static void FlushIfNeeded(FILE* f, LogLevel level, unsigned int* linesSinceFlush, unsigned int flushEveryLines) {
    if (!f || !linesSinceFlush) return;
    (*linesSinceFlush)++;
    if (level >= LOG_ERROR || (*linesSinceFlush) >= flushEveryLines) {
        fflush(f);
        *linesSinceFlush = 0;
    }
}

// Duplicate message suppression
static std::string g_lastMessage;
static LogLevel g_lastLevel = LOG_DEBUG;
static int g_lastRepeatCount = 0;
static bool g_suppressDuplicates = true;  // Enable by default

// Level colors
static const ImVec4 g_levelColors[] = {
    ImVec4(0.6f, 0.6f, 0.6f, 1.0f),  // DEBUG - gray
    ImVec4(1.0f, 1.0f, 1.0f, 1.0f),  // INFO - white
    ImVec4(1.0f, 0.8f, 0.0f, 1.0f),  // WARNING - yellow
    ImVec4(1.0f, 0.3f, 0.3f, 1.0f),  // ERROR - red
};

static const char* g_levelNames[] = {
    "DBG", "INF", "WRN", "ERR"
};

// ============================================================================
// Implementation
// ============================================================================

static char g_externalLogDir[MAX_PATH] = {0};

void LogWindow_SetLogDir(const char* dir) {
    if (dir && dir[0]) {
        strncpy_s(g_externalLogDir, sizeof(g_externalLogDir), dir, _TRUNCATE);
    }
}

const char* LogWindow_GetLogDir(void) {
    return g_externalLogDir;
}

void LogWindow_Init(void) {
    g_logEntries.clear();
    g_logEntries.reserve(g_maxEntries);
    
    // Use PID-stamped log filenames so two game instances don't clobber each other.
    DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH];

    // Create or reuse logs/<YYYY-MM-DD_HH-MM-SS>/ directory for this session
    char logDir[MAX_PATH];
    if (g_externalLogDir[0]) {
        // Use the directory already created by d3d9_proxy
        strncpy_s(logDir, sizeof(logDir), g_externalLogDir, _TRUNCATE);
    } else {
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(logDir, sizeof(logDir), "logs\\%04d-%02d-%02d_%02d-%02d-%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        CreateDirectoryA("logs", nullptr);
        CreateDirectoryA(logDir, nullptr);
    }

    // Open log file
    snprintf(path, sizeof(path), "%s\\as2_rollback_%lu.log", logDir, pid);
    g_logFile = fopen(path, "w");
    if (g_logFile) {
        // Buffer file IO to avoid frame hitches.
        setvbuf(g_logFile, nullptr, _IOFBF, 256 * 1024);
    }

    // Packet log is opt-in via env var to avoid massive IO + stutter.
    const char* pktEnv = getenv("AS2_PACKET_LOG");
    g_packetLogEnabled = (pktEnv && pktEnv[0] != '\0' && strcmp(pktEnv, "0") != 0);
    if (g_packetLogEnabled) {
        snprintf(path, sizeof(path), "%s\\as2_packets_%lu.log", logDir, pid);
        g_packetLogFile = fopen(path, "w");
        if (g_packetLogFile) {
            setvbuf(g_packetLogFile, nullptr, _IOFBF, 512 * 1024);
        }
    }

    // Netcode log — always enabled, separate file for network diagnostics
    snprintf(path, sizeof(path), "%s\\as2_netcode_%lu.log", logDir, pid);
    g_netcodeLogFile = fopen(path, "w");
    if (g_netcodeLogFile) {
        setvbuf(g_netcodeLogFile, nullptr, _IOFBF, 256 * 1024);
    }

    // Gekko log — always enabled, separate file for rollback transport internals
    snprintf(path, sizeof(path), "%s\\as2_gekko_%lu.log", logDir, pid);
    g_gekkoLogFile = fopen(path, "w");
    if (g_gekkoLogFile) {
        setvbuf(g_gekkoLogFile, nullptr, _IOFBF, 256 * 1024);
    }
    
    LOG_INFO("Log window initialized (PID %lu, logDir=%s)", pid, logDir);
}

void LogWindow_Shutdown(void) {
    if (g_logFile) {
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    if (g_packetLogFile) {
        fflush(g_packetLogFile);
        fclose(g_packetLogFile);
        g_packetLogFile = nullptr;
    }
    if (g_netcodeLogFile) {
        fflush(g_netcodeLogFile);
        fclose(g_netcodeLogFile);
        g_netcodeLogFile = nullptr;
    }
    if (g_gekkoLogFile) {
        fflush(g_gekkoLogFile);
        fclose(g_gekkoLogFile);
        g_gekkoLogFile = nullptr;
    }
    g_logEntries.clear();
}

static void GetTimestamp(char* timeBuf, size_t timeBufLen) {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    strftime(timeBuf, timeBufLen, "%H:%M:%S", tm_info);
}

void LogWindow_LogV(LogLevel level, const char* fmt, va_list args) {
    if (level < g_minLevel) return;
    
    // Format message
    char msgBuf[2048];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    
    // Get timestamp
    char timeBuf[32];
    GetTimestamp(timeBuf, sizeof(timeBuf));


    
    // Check for duplicate message suppression
    bool is_duplicate = false;
    if (g_suppressDuplicates && g_lastMessage == msgBuf && g_lastLevel == level) {
        is_duplicate = true;
        g_lastRepeatCount++;
        
        // Update the last entry's repeat count in the log buffer
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_logEntries.empty()) {
            g_logEntries.back().repeat_count = g_lastRepeatCount;
        }
        // Don't write duplicates to file/console - they'll show the count when a new message comes
        return;
    }
    
    // If we had repeats and this is a new message, flush the repeat info
    if (g_lastRepeatCount > 1) {
        // The count is already in the last log entry, nothing extra needed
    }
    
    // Reset tracking for new message
    g_lastMessage = msgBuf;
    g_lastLevel = level;
    g_lastRepeatCount = 1;
    
    // Create entry
    LogEntry entry;
    entry.level = level;
    entry.category = LOG_CAT_GENERAL;  // Default category for standard logs
    entry.timestamp = timeBuf;
    entry.message = msgBuf;
    entry.color = g_levelColors[level];
    entry.repeat_count = 1;
    
    // Add to buffer (thread-safe)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        
        // Remove oldest entries if at capacity
        while ((int)g_logEntries.size() >= g_maxEntries) {
            g_logEntries.erase(g_logEntries.begin());
        }
        
        g_logEntries.push_back(entry);
        g_scrollToBottom = g_autoScroll;
    }
    
    // Also write to file
    if (g_logFile) {
        fprintf(g_logFile, "[%s] [%s] %s\n", timeBuf, g_levelNames[level], msgBuf);
        FlushIfNeeded(g_logFile, level, &g_logLinesSinceFlush, 128);
    }
    
    // Output to console (stdout) with ANSI colors
    // This goes to the debug console window allocated by d3d9_proxy
    const char* colorCode = "";
    const char* resetCode = "\033[0m";
    switch (level) {
        case LOG_DEBUG:   colorCode = "\033[90m"; break;  // Gray
        case LOG_INFO:    colorCode = "\033[37m"; break;  // White
        case LOG_WARNING: colorCode = "\033[93m"; break;  // Yellow
        case LOG_ERROR:   colorCode = "\033[91m"; break;  // Red
    }
    printf("\033[90m[%s]\033[0m %s[MOD][%s] %s%s\n", 
           timeBuf, colorCode, g_levelNames[level], msgBuf, resetCode);
}

void LogWindow_LogPacketV(LogLevel level, const char* fmt, va_list args) {
    if (!g_packetLogFile) return;

    char msgBuf[4096];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);

    char timeBuf[32];
    GetTimestamp(timeBuf, sizeof(timeBuf));

    fprintf(g_packetLogFile, "[%s] [%s] %s\n", timeBuf, g_levelNames[level], msgBuf);
    FlushIfNeeded(g_packetLogFile, level, &g_pktLinesSinceFlush, 512);
}

void LogWindow_LogPacket(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWindow_LogPacketV(level, fmt, args);
    va_end(args);
}

void LogWindow_LogNetV(LogLevel level, const char* fmt, va_list args) {
    if (!g_netcodeLogFile) return;

    char msgBuf[2048];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);

    char timeBuf[32];
    GetTimestamp(timeBuf, sizeof(timeBuf));

    DWORD ms = GetTickCount() % 1000;
    fprintf(g_netcodeLogFile, "[%s.%03u] [%s] %s\n", timeBuf, (unsigned)ms, g_levelNames[level], msgBuf);
    FlushIfNeeded(g_netcodeLogFile, level, &g_netLinesSinceFlush, 256);
}

void LogWindow_LogNet(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWindow_LogNetV(level, fmt, args);
    va_end(args);
}

void LogWindow_LogGekkoV(LogLevel level, const char* fmt, va_list args) {
    if (!g_gekkoLogFile) return;

    char msgBuf[2048];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);

    char timeBuf[32];
    GetTimestamp(timeBuf, sizeof(timeBuf));

    DWORD ms = GetTickCount() % 1000;
    fprintf(g_gekkoLogFile, "[%s.%03u] [%s] %s\n", timeBuf, (unsigned)ms, g_levelNames[level], msgBuf);
    FlushIfNeeded(g_gekkoLogFile, level, &g_gekkoLinesSinceFlush, 256);
}

void LogWindow_LogGekko(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWindow_LogGekkoV(level, fmt, args);
    va_end(args);
}

void LogWindow_Log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWindow_LogV(level, fmt, args);
    va_end(args);
}

// ============================================================================
// Category-based Logging
// ============================================================================

void LogWindow_SetCategoryEnabled(LogCategory cat, bool enabled) {
    if (cat >= 0 && cat < LOG_CAT_COUNT) {
        g_categoryEnabled[cat] = enabled;
    }
}

bool LogWindow_IsCategoryEnabled(LogCategory cat) {
    if (cat >= 0 && cat < LOG_CAT_COUNT) {
        return g_categoryEnabled[cat];
    }
    return true;  // Unknown categories default to enabled
}

void LogWindow_SetAllCategoriesEnabled(bool enabled) {
    for (int i = 0; i < LOG_CAT_COUNT; i++) {
        g_categoryEnabled[i] = enabled;
    }
}

const char* LogWindow_GetCategoryName(LogCategory cat) {
    if (cat >= 0 && cat < LOG_CAT_COUNT) {
        return g_categoryNames[cat];
    }
    return "Unknown";
}

void LogWindow_LogCatV(LogCategory cat, LogLevel level, const char* fmt, va_list args) {
    // Category check is done by caller macro, but double-check
    if (cat >= 0 && cat < LOG_CAT_COUNT && !g_categoryEnabled[cat]) {
        return;
    }
    LogWindow_LogV(level, fmt, args);
}

void LogWindow_LogCat(LogCategory cat, LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWindow_LogCatV(cat, level, fmt, args);
    va_end(args);
}

// ============================================================================
// Rate-limited Logging
// ============================================================================

void LogWindow_LogRateLimited(LogLevel level, unsigned int intervalMs, const char* fmt, ...) {
    // Format the message first
    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);
    
    DWORD now = GetTickCount();
    
    {
        std::lock_guard<std::mutex> lock(g_rateLimitMutex);
        
        auto it = g_rateLimitMap.find(msgBuf);
        if (it != g_rateLimitMap.end()) {
            // Check if enough time has passed
            DWORD elapsed = now - it->second.lastLogTime;
            if (elapsed < intervalMs) {
                return;  // Too soon, skip this log
            }
            it->second.lastLogTime = now;
        } else {
            // First time seeing this message
            RateLimitEntry entry;
            entry.lastLogTime = now;
            entry.lastMessage = msgBuf;
            g_rateLimitMap[msgBuf] = entry;
        }
    }
    
    // Actually log it
    LogWindow_Log(level, "%s", msgBuf);
}

// ============================================================================
// State Change Logging
// ============================================================================

bool LogWindow_LogOnChange_Int(const char* name, int* prevValue, int newValue, LogLevel level, const char* fmt, ...) {
    if (*prevValue == newValue) {
        return false;  // No change
    }
    
    int oldValue = *prevValue;
    *prevValue = newValue;
    
    // Format and log
    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);
    
    LogWindow_Log(level, "%s", msgBuf);
    return true;
}

bool LogWindow_LogOnChange_Bool(const char* name, bool* prevValue, bool newValue, LogLevel level, const char* fmt, ...) {
    if (*prevValue == newValue) {
        return false;  // No change
    }
    
    *prevValue = newValue;
    
    // Format and log
    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);
    
    LogWindow_Log(level, "%s", msgBuf);
    return true;
}

LogLevel LogWindow_GetMinLevel(void) {
    return g_minLevel;
}

void LogWindow_Render(bool* pOpen) {
    if (!ImGui::Begin("Log", pOpen, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }
    
    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Options")) {
            ImGui::Checkbox("Auto-scroll", &g_autoScroll);
            ImGui::Separator();
            
            if (ImGui::MenuItem("Clear")) {
                LogWindow_Clear();
            }
            
            if (ImGui::MenuItem("Save to file...")) {
                LogWindow_WriteToFile("as2_log_export.txt");
            }
            
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Filter")) {
            if (ImGui::MenuItem("Show All", nullptr, g_minLevel == LOG_DEBUG)) {
                g_minLevel = LOG_DEBUG;
            }
            if (ImGui::MenuItem("Info+", nullptr, g_minLevel == LOG_INFO)) {
                g_minLevel = LOG_INFO;
            }
            if (ImGui::MenuItem("Warnings+", nullptr, g_minLevel == LOG_WARNING)) {
                g_minLevel = LOG_WARNING;
            }
            if (ImGui::MenuItem("Errors Only", nullptr, g_minLevel == LOG_ERROR)) {
                g_minLevel = LOG_ERROR;
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Categories")) {
            for (int i = 0; i < LOG_CAT_COUNT; i++) {
                if (ImGui::MenuItem(g_categoryNames[i], nullptr, g_categoryEnabled[i])) {
                    g_categoryEnabled[i] = !g_categoryEnabled[i];
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Enable All")) {
                LogWindow_SetAllCategoriesEnabled(true);
            }
            if (ImGui::MenuItem("Disable Verbose")) {
                // Disable only the very verbose categories
                g_categoryEnabled[LOG_CAT_TIMING] = false;
                g_categoryEnabled[LOG_CAT_PACKET] = false;
                g_categoryEnabled[LOG_CAT_PUMP] = false;
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndMenuBar();
    }
    
    // Log entries - SIMPLIFIED to avoid clipper/mutex issues
    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    
    // Copy entries while holding lock, then render without lock
    std::vector<LogEntry> entriesToRender;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        // Only copy last 100 entries for performance
        size_t start = g_logEntries.size() > 100 ? g_logEntries.size() - 100 : 0;
        for (size_t i = start; i < g_logEntries.size(); i++) {
            if (g_logEntries[i].level >= g_minLevel) {
                entriesToRender.push_back(g_logEntries[i]);
            }
        }
    }
    
    // Render without holding lock
    for (const auto& entry : entriesToRender) {
        // Timestamp (gray)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[%s]", entry.timestamp.c_str());
        ImGui::SameLine();
        
        // Level tag
        ImGui::TextColored(entry.color, "[%s]", g_levelNames[entry.level]);
        ImGui::SameLine();
        
        // Message with repeat count if > 1
        if (entry.repeat_count > 1) {
            ImGui::TextColored(entry.color, "%s (x%d)", entry.message.c_str(), entry.repeat_count);
        } else {
            ImGui::TextColored(entry.color, "%s", entry.message.c_str());
        }
    }
    
    // Auto-scroll
    if (g_scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        g_scrollToBottom = false;
    }
    
    ImGui::EndChild();
    ImGui::End();
}

void LogWindow_RenderContent(void) {
    // Toolbar
    if (ImGui::Button("Clear")) {
        LogWindow_Clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &g_autoScroll);
    ImGui::SameLine();
    ImGui::Text("| Filter:");
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) g_minLevel = LOG_DEBUG;
    ImGui::SameLine();
    if (ImGui::SmallButton("Info+")) g_minLevel = LOG_INFO;
    ImGui::SameLine();
    if (ImGui::SmallButton("Warn+")) g_minLevel = LOG_WARNING;
    ImGui::SameLine();
    if (ImGui::SmallButton("Err")) g_minLevel = LOG_ERROR;
    
    ImGui::Separator();
    
    // Log entries
    ImGui::BeginChild("LogContent", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    
    std::vector<LogEntry> entriesToRender;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        size_t start = g_logEntries.size() > 100 ? g_logEntries.size() - 100 : 0;
        for (size_t i = start; i < g_logEntries.size(); i++) {
            if (g_logEntries[i].level >= g_minLevel) {
                entriesToRender.push_back(g_logEntries[i]);
            }
        }
    }
    
    for (const auto& entry : entriesToRender) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[%s]", entry.timestamp.c_str());
        ImGui::SameLine();
        ImGui::TextColored(entry.color, "[%s]", g_levelNames[entry.level]);
        ImGui::SameLine();
        ImGui::TextColored(entry.color, "%s", entry.message.c_str());
    }
    
    if (g_scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        g_scrollToBottom = false;
    }
    
    ImGui::EndChild();
}

void LogWindow_Clear(void) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logEntries.clear();
    
    // Reset duplicate tracking
    g_lastMessage.clear();
    g_lastRepeatCount = 0;
}

int LogWindow_GetCount(void) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return (int)g_logEntries.size();
}

void LogWindow_SetMaxEntries(int max) {
    g_maxEntries = max > 0 ? max : 100;
}

void LogWindow_SetAutoScroll(bool enabled) {
    g_autoScroll = enabled;
}

bool LogWindow_GetAutoScroll(void) {
    return g_autoScroll;
}

void LogWindow_SetMinLevel(LogLevel level) {
    g_minLevel = level;
}

bool LogWindow_WriteToFile(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return false;
    
    std::lock_guard<std::mutex> lock(g_logMutex);
    
    for (const auto& entry : g_logEntries) {
        fprintf(f, "[%s] [%s] %s\n", 
                entry.timestamp.c_str(), 
                g_levelNames[entry.level], 
                entry.message.c_str());
    }
    
    fclose(f);
    return true;
}
