#include "diagnostics/async_log.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <thread>

namespace Diagnostics {
namespace {

constexpr uint32_t kQueueCapacity = 4096;
constexpr size_t kStreamNameBytes = 24;
constexpr size_t kTagBytes = 16;
constexpr size_t kTextBytes = 1024;

enum class AsyncLogEntryType : uint8_t {
    Text,
    Csv,
    Flush,
};

struct AsyncLogEntry {
    AsyncLogEntryType type;
    uint64_t flush_id;
    int32_t frame;
    char stream[kStreamNameBytes];
    char tag[kTagBytes];
    char text[kTextBytes];
};

static AsyncLogEntry s_queue[kQueueCapacity];
static uint32_t s_head = 0;
static uint32_t s_tail = 0;
static uint32_t s_count = 0;

static std::mutex s_queueMutex;
static std::condition_variable s_queueCv;
static std::condition_variable s_flushCv;
static std::thread s_writerThread;
static bool s_running = false;
static bool s_shutdownRequested = false;

static std::mutex s_fileMutex;
static FILE* s_netplayFile = nullptr;
static FILE* s_spectatorFile = nullptr;
static FILE* s_syncTraceLocalFile = nullptr;
static FILE* s_syncTraceCompareFile = nullptr;
static char s_logDir[MAX_PATH] = {};
static char s_netplayPath[MAX_PATH] = {};
static char s_spectatorPath[MAX_PATH] = {};
static char s_syncTraceLocalPath[MAX_PATH] = {};
static char s_syncTraceComparePath[MAX_PATH] = {};

static std::atomic<uint64_t> s_enqueued{0};
static std::atomic<uint64_t> s_written{0};
static std::atomic<uint64_t> s_dropped{0};
static std::atomic<uint64_t> s_nextFlushId{1};
static std::atomic<uint64_t> s_completedFlushId{0};

static uint64_t s_lastDroppedSummary = 0;

static void CopyString(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, dstSize, src, _TRUNCATE);
}

static FILE* FileForStream(const char* stream) {
    if (stream && strcmp(stream, "spectator") == 0) {
        return s_spectatorFile;
    }
    if (stream && strcmp(stream, "synctrace_local") == 0) {
        return s_syncTraceLocalFile;
    }
    if (stream && strcmp(stream, "synctrace_compare") == 0) {
        return s_syncTraceCompareFile;
    }
    return s_netplayFile;
}

static void WriteTimestamp(FILE* f) {
    if (!f) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ",
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds);
}

static void WriteLogHeader(FILE* file, const char* title, DWORD pid, const char* path) {
    if (!file) {
        return;
    }

    fprintf(file, "=== %s ===\n", title ? title : "ALICE SENKI 2 LOG");
    fprintf(file, "=== PID: %lu ===\n", pid);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(file, "=== Started: %04u-%02u-%02u %02u:%02u:%02u ===\n\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond);

    WriteTimestamp(file);
    fprintf(file, "[STARTUP ]        LogDir=%s\n", s_logDir);
    WriteTimestamp(file);
    fprintf(file, "[STARTUP ]        File=%s\n", path ? path : "");
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

static void WriteTextLine(FILE* file,
                          const char* tag,
                          int32_t frame,
                          const char* text) {
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
    fprintf(file, "%s\n", text ? text : "");
}

static void WriteCsvLine(FILE* file, const char* row) {
    if (!file) {
        return;
    }
    fprintf(file, "%s\n", row ? row : "");
}

static void FlushFilesLocked() {
    if (s_netplayFile) {
        fflush(s_netplayFile);
    }
    if (s_spectatorFile) {
        fflush(s_spectatorFile);
    }
    if (s_syncTraceLocalFile) {
        fflush(s_syncTraceLocalFile);
    }
    if (s_syncTraceCompareFile) {
        fflush(s_syncTraceCompareFile);
    }
}

static void WriteStatsLineLocked(const char* reason) {
    if (!s_netplayFile) {
        return;
    }

    uint32_t depth = 0;
    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        depth = s_count;
    }

    char line[256];
    snprintf(line,
             sizeof(line),
             "reason=%s enqueued=%llu written=%llu dropped=%llu queue_depth=%u capacity=%u",
             reason ? reason : "stats",
             (unsigned long long)s_enqueued.load(std::memory_order_relaxed),
             (unsigned long long)s_written.load(std::memory_order_relaxed),
             (unsigned long long)s_dropped.load(std::memory_order_relaxed),
             depth,
             kQueueCapacity);
    WriteTextLine(s_netplayFile, "LOGSTATS", -1, line);
}

static void MaybeWriteDroppedSummaryLocked() {
    const uint64_t dropped = s_dropped.load(std::memory_order_relaxed);
    if (dropped == s_lastDroppedSummary) {
        return;
    }

    char line[256];
    snprintf(line,
             sizeof(line),
             "dropped_logs=%llu queue_capacity=%u",
             (unsigned long long)dropped,
             kQueueCapacity);
    WriteTextLine(s_netplayFile, "LOGSTATS", -1, line);
    s_lastDroppedSummary = dropped;
}

static bool PopEntry(AsyncLogEntry* out) {
    std::unique_lock<std::mutex> lock(s_queueMutex);
    s_queueCv.wait(lock, [] {
        return s_shutdownRequested || s_count > 0;
    });

    if (s_count == 0) {
        return false;
    }

    if (out) {
        *out = s_queue[s_head];
    }
    s_head = (s_head + 1) % kQueueCapacity;
    --s_count;
    return true;
}

static bool EnqueueEntry(const AsyncLogEntry& entry) {
    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        if (!s_running || s_shutdownRequested || s_count >= kQueueCapacity) {
            s_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        s_queue[s_tail] = entry;
        s_tail = (s_tail + 1) % kQueueCapacity;
        ++s_count;
        s_enqueued.fetch_add(1, std::memory_order_relaxed);
    }

    s_queueCv.notify_one();
    return true;
}

static void WriterMain() {
    for (;;) {
        AsyncLogEntry entry{};
        const bool hasEntry = PopEntry(&entry);
        if (!hasEntry) {
            std::lock_guard<std::mutex> fileLock(s_fileMutex);
            MaybeWriteDroppedSummaryLocked();
            FlushFilesLocked();
            break;
        }

        {
            std::lock_guard<std::mutex> fileLock(s_fileMutex);
            switch (entry.type) {
                case AsyncLogEntryType::Text: {
                    FILE* file = FileForStream(entry.stream);
                    WriteTextLine(file, entry.tag, entry.frame, entry.text);
                    if (file) {
                        s_written.fetch_add(1, std::memory_order_relaxed);
                    }
                    MaybeWriteDroppedSummaryLocked();
                    break;
                }

                case AsyncLogEntryType::Csv: {
                    FILE* file = FileForStream(entry.stream);
                    WriteCsvLine(file, entry.text);
                    if (file) {
                        s_written.fetch_add(1, std::memory_order_relaxed);
                    }
                    MaybeWriteDroppedSummaryLocked();
                    break;
                }

                case AsyncLogEntryType::Flush:
                    MaybeWriteDroppedSummaryLocked();
                    FlushFilesLocked();
                    s_completedFlushId.store(entry.flush_id, std::memory_order_release);
                    s_flushCv.notify_all();
                    break;
            }
        }
    }
}

} // namespace

void AsyncLog_Init() {
    std::lock_guard<std::mutex> lock(s_queueMutex);
    if (s_running) {
        return;
    }

    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_shutdownRequested = false;
    s_running = true;
    s_enqueued.store(0, std::memory_order_relaxed);
    s_written.store(0, std::memory_order_relaxed);
    s_dropped.store(0, std::memory_order_relaxed);
    s_nextFlushId.store(1, std::memory_order_relaxed);
    s_completedFlushId.store(0, std::memory_order_relaxed);
    s_lastDroppedSummary = 0;
    s_writerThread = std::thread(WriterMain);
}

void AsyncLog_Shutdown() {
    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        if (!s_running) {
            return;
        }
        s_shutdownRequested = true;
    }
    s_queueCv.notify_all();

    if (s_writerThread.joinable()) {
        s_writerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        s_running = false;
    }

    std::lock_guard<std::mutex> fileLock(s_fileMutex);
    WriteStatsLineLocked("shutdown");
    CloseLogFile(&s_netplayFile, "=== NETPLAY LOG CLOSED ===");
    CloseLogFile(&s_spectatorFile, "=== SPECTATOR LOG CLOSED ===");
    CloseLogFile(&s_syncTraceLocalFile, nullptr);
    CloseLogFile(&s_syncTraceCompareFile, nullptr);
}

void AsyncLog_SetLogDir(const char* dir) {
    if (!dir || !dir[0]) {
        return;
    }

    std::lock_guard<std::mutex> fileLock(s_fileMutex);
    CopyString(s_logDir, sizeof(s_logDir), dir);

    CloseLogFile(&s_netplayFile, nullptr);
    CloseLogFile(&s_spectatorFile, nullptr);
    CloseLogFile(&s_syncTraceLocalFile, nullptr);
    CloseLogFile(&s_syncTraceCompareFile, nullptr);

    const DWORD pid = GetCurrentProcessId();
    snprintf(s_netplayPath,
             sizeof(s_netplayPath),
             "%s\\as2_netplay_fullpath_%lu.log",
             s_logDir,
             pid);
    s_netplayFile = fopen(s_netplayPath, "w");
    if (s_netplayFile) {
        setvbuf(s_netplayFile, nullptr, _IOFBF, 256 * 1024);
        WriteLogHeader(
            s_netplayFile,
            "ALICE SENKI 2 - FULL-PATH NETPLAY LOG",
            pid,
            s_netplayPath);
    }

    snprintf(s_spectatorPath,
             sizeof(s_spectatorPath),
             "%s\\as2_spectator_fullpath_%lu.log",
             s_logDir,
             pid);
    s_spectatorFile = fopen(s_spectatorPath, "w");
    if (s_spectatorFile) {
        setvbuf(s_spectatorFile, nullptr, _IOFBF, 256 * 1024);
        WriteLogHeader(
            s_spectatorFile,
            "ALICE SENKI 2 - SPECTATOR LOG",
            pid,
            s_spectatorPath);
    }

    snprintf(s_syncTraceLocalPath,
             sizeof(s_syncTraceLocalPath),
             "%s\\as2_synctrace_local_%lu.csv",
             s_logDir,
             pid);
    s_syncTraceLocalFile = fopen(s_syncTraceLocalPath, "w");
    if (s_syncTraceLocalFile) {
        setvbuf(s_syncTraceLocalFile, nullptr, _IOFBF, 256 * 1024);
        fprintf(s_syncTraceLocalFile,
                "source,domain,epoch,seq,mode,substate,sync_mode,lockstep,frontend,phase,rollback_phase,game_abs,rb_frame,frontend_frame,local_input,remote_input,local_frame,remote_latest,consume,remote_ack,delay_l,delay_r,eff_l,eff_r,budget,predicted,last_rb,max_rb,state_crc,p1_crc,p2_crc,flags\n");
        fflush(s_syncTraceLocalFile);
    }

    snprintf(s_syncTraceComparePath,
             sizeof(s_syncTraceComparePath),
             "%s\\as2_synctrace_compare_%lu.csv",
             s_logDir,
             pid);
    s_syncTraceCompareFile = fopen(s_syncTraceComparePath, "w");
    if (s_syncTraceCompareFile) {
        setvbuf(s_syncTraceCompareFile, nullptr, _IOFBF, 256 * 1024);
        fprintf(s_syncTraceCompareFile,
                "result,domain,epoch,frame,local_seq,remote_seq,mode,substate,state_local,state_remote,p1_local,p1_remote,p2_local,p2_remote,input_l_local,input_l_remote,input_r_local,input_r_remote,flags_local,flags_remote,reason\n");
        fflush(s_syncTraceCompareFile);
    }
}

bool AsyncLog_EnqueueText(const char* tag, int32_t frame, const char* text) {
    return AsyncLog_EnqueueTextToStream("netplay", tag, frame, text);
}

bool AsyncLog_EnqueueTextToStream(const char* stream,
                                  const char* tag,
                                  int32_t frame,
                                  const char* text) {
    AsyncLogEntry entry{};
    entry.type = AsyncLogEntryType::Text;
    entry.frame = frame;
    CopyString(entry.stream, sizeof(entry.stream), stream ? stream : "netplay");
    CopyString(entry.tag, sizeof(entry.tag), tag ? tag : "?");
    CopyString(entry.text, sizeof(entry.text), text ? text : "");
    return EnqueueEntry(entry);
}

bool AsyncLog_EnqueueCsv(const char* stream, const char* row) {
    AsyncLogEntry entry{};
    entry.type = AsyncLogEntryType::Csv;
    entry.frame = -1;
    CopyString(entry.stream, sizeof(entry.stream), stream ? stream : "netplay");
    CopyString(entry.tag, sizeof(entry.tag), "CSV");
    CopyString(entry.text, sizeof(entry.text), row ? row : "");
    return EnqueueEntry(entry);
}

void AsyncLog_RequestFlush(bool wait) {
    const uint64_t flushId = s_nextFlushId.fetch_add(1, std::memory_order_relaxed);

    AsyncLogEntry entry{};
    entry.type = AsyncLogEntryType::Flush;
    entry.flush_id = flushId;
    CopyString(entry.stream, sizeof(entry.stream), "netplay");
    CopyString(entry.tag, sizeof(entry.tag), "FLUSH");

    if (!EnqueueEntry(entry)) {
        return;
    }

    if (!wait) {
        return;
    }

    std::unique_lock<std::mutex> lock(s_queueMutex);
    s_flushCv.wait(lock, [flushId] {
        return s_completedFlushId.load(std::memory_order_acquire) >= flushId;
    });
}

void AsyncLog_GetStats(AsyncLogStats* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->enqueued = s_enqueued.load(std::memory_order_relaxed);
    out->written = s_written.load(std::memory_order_relaxed);
    out->dropped = s_dropped.load(std::memory_order_relaxed);
    out->queue_capacity = kQueueCapacity;

    std::lock_guard<std::mutex> lock(s_queueMutex);
    out->queue_depth = s_count;
}

} // namespace Diagnostics
