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

static FILE*    s_logFile = nullptr;
static FILE*    s_spectatorLogFile = nullptr;
static bool     s_verbose = false;
static char     s_logDir[MAX_PATH] = {};
static unsigned s_netplayLinesSinceFlush = 0;
static unsigned s_spectatorLinesSinceFlush = 0;
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

static void FlushIfNeeded(FILE* file, unsigned* linesSinceFlush) {
    if (!file || !linesSinceFlush) {
        return;
    }

    (*linesSinceFlush)++;
    if (*linesSinceFlush >= 50) {
        fflush(file);
        *linesSinceFlush = 0;
    }
}

static void WriteLogHeader(FILE* file, const char* title, DWORD pid) {
    if (!file) {
        return;
    }

    fprintf(file, "=== %s ===\n", title ? title : "ALICE SENKI 2 LOG");
    fprintf(file, "=== PID: %lu ===\n", pid);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(file, "=== Started: %04u-%02u-%02u %02u:%02u:%02u ===\n\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(file);
}

static void WriteStartupLine(FILE* file, const char* label, const char* value) {
    if (!file) {
        return;
    }

    WriteTimestamp(file);
    fprintf(file, "[STARTUP ]        %s%s\n", label ? label : "", value ? value : "");
    fflush(file);
}

static void CloseLogFile(FILE** file, const char* footer) {
    if (!file || !*file) {
        return;
    }

    if (footer && footer[0]) {
        fprintf(*file, "%s\n", footer);
    }
    fflush(*file);
    fclose(*file);
    *file = nullptr;
}

static void WriteLogLineV(FILE* file,
                          unsigned* linesSinceFlush,
                          const char* tag,
                          int32_t frame,
                          const char* fmt,
                          va_list ap) {
    if (!file) {
        return;
    }

    WriteTimestamp(file);
    fprintf(file, "[%-8s] ", tag ? tag : "?");

    if (frame >= 0) {
        fprintf(file, "f%-6d ", frame);
    } else {
        fprintf(file, "       ");
    }

    vfprintf(file, fmt, ap);
    fprintf(file, "\n");
    FlushIfNeeded(file, linesSinceFlush);
}

static void WriteStateChangeLine(FILE* file,
                                 unsigned* linesSinceFlush,
                                 const char* tag,
                                 int32_t frame,
                                 const char* field,
                                 const char* before,
                                 const char* after,
                                 const char* reason) {
    if (!file) {
        return;
    }

    WriteTimestamp(file);
    fprintf(file, "[%-8s] ", tag ? tag : "?");
    if (frame >= 0) {
        fprintf(file, "f%-6d ", frame);
    } else {
        fprintf(file, "       ");
    }

    fprintf(file, "CHANGE %s: \"%s\" -> \"%s\" (%s)\n",
        field ? field : "?",
        before ? before : "?",
        after ? after : "?",
        reason ? reason : "?");
    FlushIfNeeded(file, linesSinceFlush);
}

static void WriteValueChangeLine(FILE* file,
                                 unsigned* linesSinceFlush,
                                 const char* tag,
                                 int32_t frame,
                                 const char* field,
                                 int before,
                                 int after,
                                 const char* reason) {
    if (!file) {
        return;
    }

    WriteTimestamp(file);
    fprintf(file, "[%-8s] ", tag ? tag : "?");
    if (frame >= 0) {
        fprintf(file, "f%-6d ", frame);
    } else {
        fprintf(file, "       ");
    }

    fprintf(file, "CHANGE %s: %d -> %d (%s)\n",
        field ? field : "?",
        before,
        after,
        reason ? reason : "?");
    FlushIfNeeded(file, linesSinceFlush);
}

// ============================================================================
// Lifecycle
// ============================================================================

void NetplayLog_Init() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    // File will be opened when SetLogDir is called
    s_verbose = false;
    s_netplayLinesSinceFlush = 0;
    s_spectatorLinesSinceFlush = 0;
}

void NetplayLog_Shutdown() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    CloseLogFile(&s_logFile, "=== NETPLAY LOG CLOSED ===");
    CloseLogFile(&s_spectatorLogFile, "=== SPECTATOR LOG CLOSED ===");
}

void NetplayLog_SetLogDir(const char* dir) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!dir || !dir[0]) return;
    strncpy_s(s_logDir, sizeof(s_logDir), dir, _TRUNCATE);

    // Close existing if any
    CloseLogFile(&s_logFile, nullptr);
    CloseLogFile(&s_spectatorLogFile, nullptr);

    s_netplayLinesSinceFlush = 0;
    s_spectatorLinesSinceFlush = 0;

    // Open new log file
    DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\as2_netplay_fullpath_%lu.log", dir, pid);
    s_logFile = fopen(path, "w");
    if (s_logFile) {
        setvbuf(s_logFile, nullptr, _IOFBF, 256 * 1024);
        WriteLogHeader(s_logFile, "ALICE SENKI 2 - FULL-PATH NETPLAY LOG", pid);
        WriteStartupLine(s_logFile, "LogDir=", dir);
        WriteStartupLine(s_logFile, "File=", path);
    }

    snprintf(path, sizeof(path), "%s\\as2_spectator_fullpath_%lu.log", dir, pid);
    s_spectatorLogFile = fopen(path, "w");
    if (s_spectatorLogFile) {
        setvbuf(s_spectatorLogFile, nullptr, _IOFBF, 256 * 1024);
        WriteLogHeader(s_spectatorLogFile, "ALICE SENKI 2 - SPECTATOR LOG", pid);
        WriteStartupLine(s_spectatorLogFile, "LogDir=", dir);
        WriteStartupLine(s_spectatorLogFile, "File=", path);
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
        FlushIfNeeded(s_logFile, &s_netplayLinesSinceFlush);
        fflush(s_logFile);
    }
    if (s_spectatorLogFile) {
        WriteTimestamp(s_spectatorLogFile);
        fprintf(s_spectatorLogFile, "[CONFIG ] Verbose mode: %s\n", verbose ? "ON" : "OFF");
        FlushIfNeeded(s_spectatorLogFile, &s_spectatorLinesSinceFlush);
        fflush(s_spectatorLogFile);
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

    va_list ap;
    va_start(ap, fmt);
    WriteLogLineV(s_logFile, &s_netplayLinesSinceFlush, tag, frame, fmt, ap);
    va_end(ap);
}

void NetplayLog_WriteSpectator(const char* tag, int32_t frame, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile && !s_spectatorLogFile) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);

    if (s_logFile) {
        va_list apCopy;
        va_copy(apCopy, ap);
        WriteLogLineV(s_logFile, &s_netplayLinesSinceFlush, tag, frame, fmt, apCopy);
        va_end(apCopy);
    }
    if (s_spectatorLogFile) {
        va_list apCopy;
        va_copy(apCopy, ap);
        WriteLogLineV(s_spectatorLogFile, &s_spectatorLinesSinceFlush, tag, frame, fmt, apCopy);
        va_end(apCopy);
    }

    va_end(ap);
}

void NetplayLog_Verbose(const char* tag, int32_t frame, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile || !s_verbose) return;

    va_list ap;
    va_start(ap, fmt);
    WriteLogLineV(s_logFile, &s_netplayLinesSinceFlush, tag, frame, fmt, ap);
    va_end(ap);
}

void NetplayLog_VerboseSpectator(const char* tag, int32_t frame, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_verbose || (!s_logFile && !s_spectatorLogFile)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);

    if (s_logFile) {
        va_list apCopy;
        va_copy(apCopy, ap);
        WriteLogLineV(s_logFile, &s_netplayLinesSinceFlush, tag, frame, fmt, apCopy);
        va_end(apCopy);
    }
    if (s_spectatorLogFile) {
        va_list apCopy;
        va_copy(apCopy, ap);
        WriteLogLineV(s_spectatorLogFile, &s_spectatorLinesSinceFlush, tag, frame, fmt, apCopy);
        va_end(apCopy);
    }

    va_end(ap);
}

void NetplayLog_StateChange(const char* tag, int32_t frame,
                            const char* field,
                            const char* before, const char* after,
                            const char* reason) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile) return;
    WriteStateChangeLine(s_logFile,
        &s_netplayLinesSinceFlush,
        tag,
        frame,
        field,
        before,
        after,
        reason);
}

void NetplayLog_ValueChange(const char* tag, int32_t frame,
                            const char* field,
                            int before, int after,
                            const char* reason) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_logFile) return;
    WriteValueChangeLine(s_logFile,
        &s_netplayLinesSinceFlush,
        tag,
        frame,
        field,
        before,
        after,
        reason);
}

void NetplayLog_Flush() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_logFile) {
        fflush(s_logFile);
        s_netplayLinesSinceFlush = 0;
    }
    if (s_spectatorLogFile) {
        fflush(s_spectatorLogFile);
        s_spectatorLinesSinceFlush = 0;
    }
}

} // namespace Rollback
