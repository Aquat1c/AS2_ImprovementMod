// One startup log shared by every mod module (wsock32 proxy, d3d9 proxy,
// core). Header-only: each DLL keeps its own copy of the state and appends to
// the same file with synchronous, per-line opens, so the last line always
// survives a hang. Path: <game dir>\logs\startup_<pid>.log. Whoever closes
// it (the wsock32 heartbeat, when the title screen is reached) writes a
// marker file that tells the other modules to stop.
#pragma once

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace StartupLog {

struct State {
    wchar_t path[MAX_PATH];
    wchar_t markerPath[MAX_PATH];
    char module[16];
    ULONGLONG processStart100ns;
    bool initialised;
    bool closedSeen;
};

inline State& Get() {
    static State s = {};
    return s;
}

inline ULONGLONG Now100ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

inline ULONGLONG ProcessStart100ns() {
    FILETIME creation = {}, exitTime = {}, kernel = {}, user = {};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
        return ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
    }
    return Now100ns();
}

inline DWORD ElapsedMs() {
    State& s = Get();
    const ULONGLONG start = s.processStart100ns ? s.processStart100ns : ProcessStart100ns();
    const ULONGLONG now = Now100ns();
    return now > start ? (DWORD)((now - start) / 10000ULL) : 0;
}

// gameDir: folder holding the game executable. module: short tag for lines.
inline void Init(const wchar_t* gameDir, const char* module) {
    State& s = Get();
    if (s.initialised || !gameDir || !gameDir[0]) {
        return;
    }
    wchar_t logs[MAX_PATH];
    swprintf_s(logs, MAX_PATH, L"%s\\logs", gameDir);
    CreateDirectoryW(logs, nullptr);
    swprintf_s(s.path, MAX_PATH, L"%s\\startup_%lu.log", logs, GetCurrentProcessId());
    swprintf_s(s.markerPath, MAX_PATH, L"%s\\startup_%lu.closed", logs, GetCurrentProcessId());
    strncpy_s(s.module, sizeof(s.module), module ? module : "?", _TRUNCATE);
    s.processStart100ns = ProcessStart100ns();
    s.initialised = true;

    // Stale markers from earlier runs would silence this one.
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, MAX_PATH, L"%s\\startup_*.closed", logs);
    WIN32_FIND_DATAW found = {};
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t stale[MAX_PATH];
            swprintf_s(stale, MAX_PATH, L"%s\\%s", logs, found.cFileName);
            if (_wcsicmp(stale, s.markerPath) != 0) {
                DeleteFileW(stale);
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
}

inline const wchar_t* Path() {
    return Get().path;
}

inline bool IsClosed() {
    State& s = Get();
    if (!s.initialised) {
        return true;
    }
    if (s.closedSeen) {
        return true;
    }
    if (GetFileAttributesW(s.markerPath) != INVALID_FILE_ATTRIBUTES) {
        s.closedSeen = true;
        return true;
    }
    return false;
}

inline void AppendRaw(const char* text, size_t len) {
    State& s = Get();
    HANDLE h = CreateFileW(s.path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, text, (DWORD)len, &written, nullptr);
    CloseHandle(h);
}

inline void Line(const char* fmt, ...) {
    State& s = Get();
    if (!s.initialised || IsClosed()) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1600];
    int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] +%06lums [%-7s] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ElapsedMs(), s.module);
    if (n < 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof(line) - 2) {
        len = sizeof(line) - 2;
    }
    line[len++] = '\n';
    line[len] = '\0';
    AppendRaw(line, len);
}

// Writes the trailer, drops the marker, and moves the file into moveToDir when given.
inline void Close(const char* reason, const wchar_t* moveToDir) {
    State& s = Get();
    if (!s.initialised || IsClosed()) {
        return;
    }
    Line("startup log closed: %s", reason ? reason : "done");
    HANDLE marker = CreateFileW(s.markerPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE) {
        CloseHandle(marker);
    }
    s.closedSeen = true;
    if (moveToDir && moveToDir[0]) {
        wchar_t dest[MAX_PATH];
        swprintf_s(dest, MAX_PATH, L"%s\\startup.log", moveToDir);
        if (MoveFileExW(s.path, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            wcscpy_s(s.path, dest);
        }
    }
    // The marker stays for the life of the process so no module reopens the log.
}

} // namespace StartupLog
