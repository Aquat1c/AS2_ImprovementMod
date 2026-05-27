/**
 * Alice Senki 2 - Dedicated Full-Path Rollback/Netplay Log Implementation
 */

#include "rollback/netplay_log.h"

#include "diagnostics/async_log.h"

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace Rollback {
namespace {

static std::atomic<bool> s_verbose{false};
static std::atomic<bool> s_logReady{false};

static void FormatLine(char* out, size_t outSize, const char* fmt, va_list ap) {
    if (!out || outSize == 0) {
        return;
    }

    if (!fmt) {
        out[0] = '\0';
        return;
    }

    vsnprintf(out, outSize, fmt, ap);
    out[outSize - 1] = '\0';
}

static void EnqueueFormatted(const char* tag, int32_t frame, const char* fmt, va_list ap) {
    if (!s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    char line[1024];
    FormatLine(line, sizeof(line), fmt, ap);
    Diagnostics::AsyncLog_EnqueueText(tag, frame, line);
}

static void EnqueueFormattedToStream(const char* stream,
                                     const char* tag,
                                     int32_t frame,
                                     const char* fmt,
                                     va_list ap) {
    if (!s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    char line[1024];
    FormatLine(line, sizeof(line), fmt, ap);
    Diagnostics::AsyncLog_EnqueueTextToStream(stream, tag, frame, line);
}

} // namespace

void NetplayLog_Init() {
    s_verbose.store(false, std::memory_order_release);
    s_logReady.store(false, std::memory_order_release);
    Diagnostics::AsyncLog_Init();
}

void NetplayLog_Shutdown() {
    if (s_logReady.load(std::memory_order_acquire)) {
        Diagnostics::AsyncLogStats stats{};
        Diagnostics::AsyncLog_GetStats(&stats);
        char line[256];
        snprintf(line,
                 sizeof(line),
                 "shutdown_request enqueued=%llu written=%llu dropped=%llu queue_depth=%u capacity=%u",
                 (unsigned long long)stats.enqueued,
                 (unsigned long long)stats.written,
                 (unsigned long long)stats.dropped,
                 stats.queue_depth,
                 stats.queue_capacity);
        Diagnostics::AsyncLog_EnqueueText("LOGSTATS", -1, line);
        Diagnostics::AsyncLog_RequestFlush(true);
    }
    s_logReady.store(false, std::memory_order_release);
    Diagnostics::AsyncLog_Shutdown();
}

void NetplayLog_SetLogDir(const char* dir) {
    if (!dir || !dir[0]) {
        return;
    }

    Diagnostics::AsyncLog_SetLogDir(dir);
    s_logReady.store(true, std::memory_order_release);
    Diagnostics::AsyncLog_EnqueueText("STARTUP", -1, "async_log_backend=1");
}

void NetplayLog_SetVerbose(bool verbose) {
    s_verbose.store(verbose, std::memory_order_release);
    if (!s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    Diagnostics::AsyncLog_EnqueueText(
        "CONFIG",
        -1,
        verbose ? "Verbose mode: ON" : "Verbose mode: OFF");
}

bool NetplayLog_IsVerbose() {
    return s_verbose.load(std::memory_order_acquire);
}

void NetplayLog_Write(const char* tag, int32_t frame, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    EnqueueFormatted(tag, frame, fmt, ap);
    va_end(ap);
}

void NetplayLog_WriteSpectator(const char* tag, int32_t frame, const char* fmt, ...) {
    if (!s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);

    va_list mainCopy;
    va_copy(mainCopy, ap);
    EnqueueFormatted(tag, frame, fmt, mainCopy);
    va_end(mainCopy);

    va_list spectatorCopy;
    va_copy(spectatorCopy, ap);
    EnqueueFormattedToStream("spectator", tag, frame, fmt, spectatorCopy);
    va_end(spectatorCopy);

    va_end(ap);
}

void NetplayLog_Verbose(const char* tag, int32_t frame, const char* fmt, ...) {
    if (!s_verbose.load(std::memory_order_acquire)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    EnqueueFormatted(tag, frame, fmt, ap);
    va_end(ap);
}

void NetplayLog_VerboseSpectator(const char* tag, int32_t frame, const char* fmt, ...) {
    if (!s_verbose.load(std::memory_order_acquire) ||
        !s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);

    va_list mainCopy;
    va_copy(mainCopy, ap);
    EnqueueFormatted(tag, frame, fmt, mainCopy);
    va_end(mainCopy);

    va_list spectatorCopy;
    va_copy(spectatorCopy, ap);
    EnqueueFormattedToStream("spectator", tag, frame, fmt, spectatorCopy);
    va_end(spectatorCopy);

    va_end(ap);
}

void NetplayLog_StateChange(const char* tag, int32_t frame,
                            const char* field,
                            const char* before, const char* after,
                            const char* reason) {
    if (!s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    char line[1024];
    snprintf(line,
             sizeof(line),
             "CHANGE %s: \"%s\" -> \"%s\" (%s)",
             field ? field : "?",
             before ? before : "?",
             after ? after : "?",
             reason ? reason : "?");
    Diagnostics::AsyncLog_EnqueueText(tag, frame, line);
}

void NetplayLog_ValueChange(const char* tag, int32_t frame,
                            const char* field,
                            int before, int after,
                            const char* reason) {
    if (!s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    char line[1024];
    snprintf(line,
             sizeof(line),
             "CHANGE %s: %d -> %d (%s)",
             field ? field : "?",
             before,
             after,
             reason ? reason : "?");
    Diagnostics::AsyncLog_EnqueueText(tag, frame, line);
}

void NetplayLog_Flush() {
    if (!s_logReady.load(std::memory_order_acquire)) {
        return;
    }

    Diagnostics::AsyncLog_RequestFlush(false);
}

} // namespace Rollback
