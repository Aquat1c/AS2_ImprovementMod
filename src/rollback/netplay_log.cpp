/**
 * Alice Senki 2 - Dedicated Full-Path Rollback/Netplay Log Implementation
 */

#include "rollback/netplay_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <mutex>

namespace Rollback {

// ============================================================================
// Internal State
// ============================================================================

static FILE*    s_logFile    = nullptr;
static bool     s_verbose   = false;
static char     s_logDir[MAX_PATH] = {};
static unsigned s_linesSinceFlush = 0;
static std::mutex s_logMutex;

// ============================================================================
// Helpers
// ============================================================================

static void WriteTimestamp(FILE* f) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void FlushIfNeeded() {
    if (!s_logFile) return;
    s_linesSinceFlush++;
    if (s_linesSinceFlush >= 50) {
        fflush(s_logFile);
        s_linesSinceFlush = 0;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void NetplayLog_Init() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    // File will be opened when SetLogDir is called
    s_verbose = false;
    s_linesSinceFlush = 0;
}

void NetplayLog_Shutdown() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_logFile) {
        fprintf(s_logFile, "=== NETPLAY LOG CLOSED ===\n");
        fflush(s_logFile);
        fclose(s_logFile);
        s_logFile = nullptr;
    }
}

void NetplayLog_SetLogDir(const char* dir) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!dir || !dir[0]) return;
    strncpy_s(s_logDir, sizeof(s_logDir), dir, _TRUNCATE);

    // Close existing if any
    if (s_logFile) {
        fflush(s_logFile);
        fclose(s_logFile);
        s_logFile = nullptr;
    }

    // Open new log file
    DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\as2_netplay_fullpath_%lu.log", dir, pid);
    s_logFile = fopen(path, "w");
    if (s_logFile) {
        setvbuf(s_logFile, nullptr, _IOFBF, 256 * 1024);
        fprintf(s_logFile, "=== ALICE SENKI 2 — FULL-PATH NETPLAY LOG ===\n");
        fprintf(s_logFile, "=== PID: %lu ===\n", pid);
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(s_logFile, "=== Started: %04u-%02u-%02u %02u:%02u:%02u ===\n\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(s_logFile);
    }
}

// ============================================================================
// Verbosity
// ============================================================================

void NetplayLog_SetVerbose(bool verbose) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    s_verbose = verbose;
    if (s_logFile) {
        WriteTimestamp(s_logFile);
        fprintf(s_logFile, "[CONFIG ] Verbose mode: %s\n", verbose ? "ON" : "OFF");
        FlushIfNeeded();
    }
}

bool NetplayLog_IsVerbose() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    return s_verbose;
}

// ============================================================================
// Logging
// ============================================================================

void NetplayLog_Write(const char* tag, int32_t frame, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile) return;

    WriteTimestamp(s_logFile);

    // Tag (padded to 8 chars)
    fprintf(s_logFile, "[%-8s] ", tag ? tag : "?");

    // Frame number
    if (frame >= 0) {
        fprintf(s_logFile, "f%-6d ", frame);
    } else {
        fprintf(s_logFile, "       ");
    }

    // Message
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_logFile, fmt, ap);
    va_end(ap);

    fprintf(s_logFile, "\n");
    FlushIfNeeded();
}

void NetplayLog_Verbose(const char* tag, int32_t frame, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile || !s_verbose) return;

    WriteTimestamp(s_logFile);
    fprintf(s_logFile, "[%-8s] ", tag ? tag : "?");

    if (frame >= 0) {
        fprintf(s_logFile, "f%-6d ", frame);
    } else {
        fprintf(s_logFile, "       ");
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_logFile, fmt, ap);
    va_end(ap);

    fprintf(s_logFile, "\n");
    FlushIfNeeded();
}

void NetplayLog_StateChange(const char* tag, int32_t frame,
                            const char* field,
                            const char* before, const char* after,
                            const char* reason) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile) return;

    WriteTimestamp(s_logFile);
    fprintf(s_logFile, "[%-8s] ", tag ? tag : "?");
    if (frame >= 0) {
        fprintf(s_logFile, "f%-6d ", frame);
    } else {
        fprintf(s_logFile, "       ");
    }

    fprintf(s_logFile, "CHANGE %s: \"%s\" -> \"%s\" (%s)\n",
        field ? field : "?",
        before ? before : "?",
        after ? after : "?",
        reason ? reason : "?");
    FlushIfNeeded();
}

void NetplayLog_ValueChange(const char* tag, int32_t frame,
                            const char* field,
                            int before, int after,
                            const char* reason) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile) return;

    WriteTimestamp(s_logFile);
    fprintf(s_logFile, "[%-8s] ", tag ? tag : "?");
    if (frame >= 0) {
        fprintf(s_logFile, "f%-6d ", frame);
    } else {
        fprintf(s_logFile, "       ");
    }

    fprintf(s_logFile, "CHANGE %s: %d -> %d (%s)\n",
        field ? field : "?",
        before, after,
        reason ? reason : "?");
    FlushIfNeeded();
}

void NetplayLog_Flush() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_logFile) {
        fflush(s_logFile);
        s_linesSinceFlush = 0;
    }
}

} // namespace Rollback
