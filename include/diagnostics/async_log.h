#pragma once

#include <stdint.h>

namespace Diagnostics {

struct AsyncLogStats {
    uint64_t enqueued;
    uint64_t written;
    uint64_t dropped;
    uint32_t queue_capacity;
    uint32_t queue_depth;
};

void AsyncLog_Init();
void AsyncLog_Shutdown();
void AsyncLog_SetLogDir(const char* dir);

bool AsyncLog_EnqueueText(const char* tag, int32_t frame, const char* text);
bool AsyncLog_EnqueueTextToStream(const char* stream, const char* tag, int32_t frame, const char* text);
bool AsyncLog_EnqueueCsv(const char* stream, const char* row);

void AsyncLog_RequestFlush(bool wait);
void AsyncLog_GetStats(AsyncLogStats* out);

} // namespace Diagnostics
