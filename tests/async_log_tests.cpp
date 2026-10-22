#include "diagnostics/async_log.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

static int Fail(const char* message) {
    fprintf(stderr, "async_log_tests: %s\n", message ? message : "failed");
    return 1;
}

static bool FileContains(const char* path, const char* needle) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return false;
    }

    char line[2048];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

int main() {
    char tempPath[MAX_PATH] = {};
    if (!GetTempPathA(sizeof(tempPath), tempPath)) {
        return Fail("GetTempPathA failed");
    }

    char logDir[MAX_PATH] = {};
    snprintf(logDir,
             sizeof(logDir),
             "%sas2_async_log_test_%lu",
             tempPath,
             GetCurrentProcessId());
    CreateDirectoryA(logDir, nullptr);

    Diagnostics::AsyncLog_Init();
    Diagnostics::AsyncLog_SetLogDir(logDir);

    if (!Diagnostics::AsyncLog_EnqueueText("TEST", 12, "hello async netplay")) {
        Diagnostics::AsyncLog_Shutdown();
        return Fail("failed to enqueue main log line");
    }
    if (!Diagnostics::AsyncLog_EnqueueTextToStream("spectator", "SPEC", 13, "hello async spectator")) {
        Diagnostics::AsyncLog_Shutdown();
        return Fail("failed to enqueue spectator log line");
    }
    Diagnostics::AsyncLog_RequestFlush(true);

    Diagnostics::AsyncLogStats stats{};
    Diagnostics::AsyncLog_GetStats(&stats);
    if (stats.enqueued < 3) {
        Diagnostics::AsyncLog_Shutdown();
        return Fail("expected at least three queued entries including flush");
    }

    Diagnostics::AsyncLog_Shutdown();

    char netplayPath[MAX_PATH] = {};
    snprintf(netplayPath,
             sizeof(netplayPath),
             "%s\\as2_netplay_fullpath_%lu.log",
             logDir,
             GetCurrentProcessId());
    if (!FileContains(netplayPath, "hello async netplay")) {
        return Fail("netplay log did not contain queued line");
    }

    char spectatorPath[MAX_PATH] = {};
    snprintf(spectatorPath,
             sizeof(spectatorPath),
             "%s\\as2_spectator_fullpath_%lu.log",
             logDir,
             GetCurrentProcessId());
    if (!FileContains(spectatorPath, "hello async spectator")) {
        return Fail("spectator log did not contain queued line");
    }

    printf("async_log_tests: passed\n");
    return 0;
}
