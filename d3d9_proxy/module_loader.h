#pragma once

#include <windows.h>
#include <stddef.h>

// Older SDK headers gate these behind _WIN32_WINNT; the values are stable.
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_APPLICATION_DIR
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x00000200
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace As2Loader {

constexpr DWORD kPackageSearchFlags =
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
    LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
    LOAD_LIBRARY_SEARCH_SYSTEM32;

struct LoadResult {
    HMODULE module = nullptr;
    DWORD   error = 0;
    bool    flagsUnsupported = false;   // search flags rejected; altered-search fallback used
    bool    selfResolved = false;       // request resolved to the caller's own module
    wchar_t requestedPath[MAX_PATH] = {};
    wchar_t loadedPath[MAX_PATH] = {};
};

// Drive-absolute (C:\x) or UNC (\\server\x, \\?\x). Anything else is rejected.
bool IsFullyQualifiedPath(const wchar_t* path);

// GetSystemDirectoryW + "\" + baseName. False when the buffer is too small.
bool BuildSystemModulePath(const wchar_t* baseName, wchar_t* out, size_t cap);

// Package DLL by fully qualified path under the bounded search policy.
LoadResult LoadPackageModule(const wchar_t* fullPath, HMODULE self);

// System DLL by base name, resolved through the process' system directory.
LoadResult LoadSystemModule(const wchar_t* baseName, HMODULE self);

// True when ntdll exports wine_get_version. Output strings may be empty.
bool DetectWine(char* version, size_t versionCap,
                char* buildId, size_t buildCap,
                char* host, size_t hostCap);

// One-owner initialisation gate with explicit re-entry semantics.
class BootstrapGate {
public:
    enum class Outcome {
        Acquired,            // caller owns the bootstrap; call Complete() when done
        ReentrantSameThread, // called again from the owning thread mid-bootstrap
        CompletedElsewhere,  // another caller already finished
        TimedOut             // another thread is still bootstrapping
    };

    Outcome Enter(DWORD waitTimeoutMs);
    void Complete();
    bool IsComplete() const;
    bool IsRunning() const;
    DWORD OwnerThreadId() const;

private:
    volatile LONG m_state = 0;   // 0 idle, 1 running, 2 complete
    volatile DWORD m_owner = 0;
};

} // namespace As2Loader
