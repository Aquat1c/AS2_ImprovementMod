#include "module_loader.h"

#include <wchar.h>
#include <string.h>
#include <stdio.h>

namespace As2Loader {

namespace {

bool IsDriveLetter(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

void CaptureLoadedPath(LoadResult& result) {
    if (!result.module) {
        return;
    }
    const DWORD len = GetModuleFileNameW(result.module, result.loadedPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        result.loadedPath[0] = L'\0';
    }
}

void RejectSelf(LoadResult& result, HMODULE self) {
    if (!result.module || !self || result.module != self) {
        return;
    }
    FreeLibrary(result.module);   // balance the refcount we just added
    result.module = nullptr;
    result.selfResolved = true;
    result.error = ERROR_INVALID_DLL;
}

} // namespace

bool IsFullyQualifiedPath(const wchar_t* path) {
    if (!path || !path[0]) {
        return false;
    }
    if (IsDriveLetter(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    if (path[0] == L'\\' && path[1] == L'\\' && path[2] != L'\0') {
        return true;
    }
    return false;
}

bool BuildSystemModulePath(const wchar_t* baseName, wchar_t* out, size_t cap) {
    if (!baseName || !out || cap == 0) {
        return false;
    }
    out[0] = L'\0';
    wchar_t systemDir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    const int written = swprintf_s(out, cap, L"%s\\%s", systemDir, baseName);
    return written > 0;
}

LoadResult LoadPackageModule(const wchar_t* fullPath, HMODULE self) {
    LoadResult result;
    if (fullPath) {
        wcsncpy_s(result.requestedPath, MAX_PATH, fullPath, _TRUNCATE);
    }
    if (!IsFullyQualifiedPath(fullPath)) {
        result.error = ERROR_BAD_PATHNAME;
        return result;
    }

    SetLastError(0);
    result.module = LoadLibraryExW(fullPath, nullptr, kPackageSearchFlags);
    if (!result.module) {
        result.error = GetLastError();
        if (result.error == ERROR_INVALID_PARAMETER) {
            // Pre-KB2533623 loader: same absolute path, legacy altered search.
            result.flagsUnsupported = true;
            SetLastError(0);
            result.module = LoadLibraryExW(fullPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!result.module) {
                result.error = GetLastError();
            }
        }
    }

    if (result.module) {
        result.error = 0;
        CaptureLoadedPath(result);
        RejectSelf(result, self);
    }
    return result;
}

LoadResult LoadSystemModule(const wchar_t* baseName, HMODULE self) {
    LoadResult result;
    if (!BuildSystemModulePath(baseName, result.requestedPath, MAX_PATH)) {
        result.error = ERROR_BAD_PATHNAME;
        return result;
    }

    SetLastError(0);
    result.module = LoadLibraryExW(result.requestedPath, nullptr, 0);
    if (!result.module) {
        result.error = GetLastError();
        return result;
    }

    CaptureLoadedPath(result);
    RejectSelf(result, self);
    return result;
}

bool DetectWine(char* version, size_t versionCap,
                char* buildId, size_t buildCap,
                char* host, size_t hostCap) {
    if (version && versionCap) version[0] = '\0';
    if (buildId && buildCap) buildId[0] = '\0';
    if (host && hostCap) host[0] = '\0';

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return false;
    }

    typedef const char* (CDECL *GetVersionFn)(void);
    typedef void (CDECL *GetHostVersionFn)(const char** sysname, const char** release);

    GetVersionFn getVersion = (GetVersionFn)GetProcAddress(ntdll, "wine_get_version");
    if (!getVersion) {
        return false;
    }

    const char* v = getVersion();
    if (version && versionCap && v) {
        strncpy_s(version, versionCap, v, _TRUNCATE);
    }

    GetVersionFn getBuild = (GetVersionFn)GetProcAddress(ntdll, "wine_get_build_id");
    if (getBuild && buildId && buildCap) {
        const char* b = getBuild();
        if (b) strncpy_s(buildId, buildCap, b, _TRUNCATE);
    }

    GetHostVersionFn getHost = (GetHostVersionFn)GetProcAddress(ntdll, "wine_get_host_version");
    if (getHost && host && hostCap) {
        const char* sysname = nullptr;
        const char* release = nullptr;
        getHost(&sysname, &release);
        _snprintf_s(host, hostCap, _TRUNCATE, "%s %s",
                    sysname ? sysname : "?", release ? release : "?");
    }
    return true;
}

BootstrapGate::Outcome BootstrapGate::Enter(DWORD waitTimeoutMs) {
    const DWORD tid = GetCurrentThreadId();
    const LONG previous = InterlockedCompareExchange(&m_state, 1, 0);
    if (previous == 0) {
        m_owner = tid;
        return Outcome::Acquired;
    }
    if (previous == 2) {
        return Outcome::CompletedElsewhere;
    }
    if (m_owner == tid) {
        return Outcome::ReentrantSameThread;
    }

    const DWORD start = GetTickCount();
    for (;;) {
        if (InterlockedCompareExchange(&m_state, 2, 2) == 2) {
            return Outcome::CompletedElsewhere;
        }
        if (GetTickCount() - start >= waitTimeoutMs) {
            return Outcome::TimedOut;
        }
        Sleep(1);
    }
}

void BootstrapGate::Complete() {
    InterlockedExchange(&m_state, 2);
}

bool BootstrapGate::IsComplete() const {
    return InterlockedCompareExchange(const_cast<volatile LONG*>(&m_state), 2, 2) == 2;
}

bool BootstrapGate::IsRunning() const {
    return InterlockedCompareExchange(const_cast<volatile LONG*>(&m_state), 1, 1) == 1;
}

DWORD BootstrapGate::OwnerThreadId() const {
    return m_owner;
}

} // namespace As2Loader
