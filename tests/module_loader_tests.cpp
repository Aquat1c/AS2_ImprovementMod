// Real-API fixtures for the proxy's bounded module loading policy and the
// bootstrap re-entry gate. Runs on Windows and under Wine; results are only
// meaningful where executed.

#include "../d3d9_proxy/module_loader.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

using namespace As2Loader;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                           \
    } while (0)

#define CHECK_MSG(cond, ...)                                                        \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                \
            printf(__VA_ARGS__);                                                    \
            printf("\n");                                                           \
        }                                                                           \
    } while (0)

static void TestPathClassification() {
    CHECK(IsFullyQualifiedPath(L"C:\\Games\\AS2\\as2_rollback.dll"));
    CHECK(IsFullyQualifiedPath(L"Z:\\home\\user\\game\\d3d9.dll"));
    CHECK(IsFullyQualifiedPath(L"c:/lower/slash.dll"));
    CHECK(IsFullyQualifiedPath(L"\\\\server\\share\\x.dll"));
    CHECK(IsFullyQualifiedPath(L"\\\\?\\C:\\long\\x.dll"));
    CHECK(!IsFullyQualifiedPath(L"as2_rollback.dll"));
    CHECK(!IsFullyQualifiedPath(L"..\\as2_rollback.dll"));
    CHECK(!IsFullyQualifiedPath(L"C:relative.dll"));
    CHECK(!IsFullyQualifiedPath(L"\\rooted\\no_drive.dll"));
    CHECK(!IsFullyQualifiedPath(L""));
    CHECK(!IsFullyQualifiedPath(nullptr));

    LoadResult relative = LoadPackageModule(L"as2_rollback.dll", nullptr);
    CHECK(relative.module == nullptr);
    CHECK(relative.error == ERROR_BAD_PATHNAME);

    LoadResult rooted = LoadPackageModule(L"\\as2_rollback.dll", nullptr);
    CHECK(rooted.module == nullptr);
    CHECK(rooted.error == ERROR_BAD_PATHNAME);
}

struct Fixture {
    wchar_t dir[MAX_PATH] = {};
    wchar_t dll[MAX_PATH] = {};
    wchar_t source[MAX_PATH] = {};
    bool ready = false;
};

static bool PrepareFixture(Fixture& fx) {
    wchar_t temp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temp)) {
        printf("SKIP fixture: GetTempPathW failed err=%lu\n", GetLastError());
        return false;
    }
    swprintf_s(fx.dir, MAX_PATH, L"%sas2_loader_fixture_%lu", temp, GetCurrentProcessId());
    if (!CreateDirectoryW(fx.dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        printf("SKIP fixture: CreateDirectoryW failed err=%lu\n", GetLastError());
        return false;
    }
    if (!BuildSystemModulePath(L"version.dll", fx.source, MAX_PATH)) {
        printf("SKIP fixture: BuildSystemModulePath failed\n");
        return false;
    }
    swprintf_s(fx.dll, MAX_PATH, L"%s\\as2_loader_probe.dll", fx.dir);
    if (!CopyFileW(fx.source, fx.dll, FALSE)) {
        printf("SKIP fixture: CopyFileW failed err=%lu\n", GetLastError());
        return false;
    }
    fx.ready = true;
    return true;
}

static void CleanupFixture(Fixture& fx) {
    if (fx.dll[0]) DeleteFileW(fx.dll);
    if (fx.dir[0]) RemoveDirectoryW(fx.dir);
}

static bool SameFile(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

static void TestPackageLoad(const Fixture& fx) {
    LoadResult ok = LoadPackageModule(fx.dll, nullptr);
    CHECK_MSG(ok.module != nullptr, "err=%lu", ok.error);
    if (ok.module) {
        CHECK(ok.error == 0);
        CHECK(!ok.selfResolved);
        CHECK_MSG(SameFile(ok.loadedPath, fx.dll), "loaded=%ls", ok.loadedPath);
        CHECK(GetProcAddress(ok.module, "GetFileVersionInfoSizeW") != nullptr);
        if (ok.flagsUnsupported) {
            printf("NOTE search flags unsupported on this loader; altered-search fallback used\n");
        }
        FreeLibrary(ok.module);
    }

    // Self-resolution guard: asking for our own module must be refused.
    HMODULE self = GetModuleHandleW(nullptr);
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(self, selfPath, MAX_PATH);
    LoadResult selfLoad = LoadPackageModule(selfPath, self);
    CHECK(selfLoad.module == nullptr);
    CHECK(selfLoad.selfResolved);

    // Missing file: precise error, no module.
    wchar_t missing[MAX_PATH] = {};
    swprintf_s(missing, MAX_PATH, L"%s\\does_not_exist.dll", fx.dir);
    LoadResult miss = LoadPackageModule(missing, nullptr);
    CHECK(miss.module == nullptr);
    CHECK_MSG(miss.error == ERROR_MOD_NOT_FOUND, "err=%lu", miss.error);
}

static void TestBoundedSearchExcludesCwdAndPath(const Fixture& fx) {
    // Bounded flags with a base name: neither CWD nor PATH may satisfy it.
    wchar_t oldCwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, oldCwd);
    CHECK(SetCurrentDirectoryW(fx.dir));

    const DWORD bareFlags = LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    SetLastError(0);
    HMODULE viaCwd = LoadLibraryExW(L"as2_loader_probe.dll", nullptr, bareFlags);
    const DWORD cwdErr = GetLastError();
    if (viaCwd && cwdErr == ERROR_INVALID_PARAMETER) {
        printf("NOTE loader rejects search flags; CWD exclusion not testable here\n");
    }
    CHECK_MSG(viaCwd == nullptr, "bounded search found the CWD-only helper");
    if (viaCwd) FreeLibrary(viaCwd);

    // Legacy search does consult the CWD.
    HMODULE legacy = LoadLibraryW(L"as2_loader_probe.dll");
    CHECK_MSG(legacy != nullptr, "legacy search missed CWD helper err=%lu", GetLastError());
    if (legacy) FreeLibrary(legacy);

    SetCurrentDirectoryW(oldCwd);

    // PATH-only helper.
    wchar_t oldPath[32 * 1024] = {};
    const DWORD pathLen = GetEnvironmentVariableW(L"PATH", oldPath, 32 * 1024);
    static wchar_t newPath[34 * 1024];
    swprintf_s(newPath, 34 * 1024, L"%s;%s", fx.dir, pathLen ? oldPath : L"");
    CHECK(SetEnvironmentVariableW(L"PATH", newPath));

    SetLastError(0);
    HMODULE viaPath = LoadLibraryExW(L"as2_loader_probe.dll", nullptr, bareFlags);
    CHECK_MSG(viaPath == nullptr, "bounded search found the PATH-only helper");
    if (viaPath) FreeLibrary(viaPath);

    HMODULE legacyPath = LoadLibraryW(L"as2_loader_probe.dll");
    CHECK_MSG(legacyPath != nullptr, "legacy search missed PATH helper err=%lu", GetLastError());
    if (legacyPath) FreeLibrary(legacyPath);

    SetEnvironmentVariableW(L"PATH", pathLen ? oldPath : nullptr);
}

static void TestSystemLoad() {
    LoadResult sys = LoadSystemModule(L"version.dll", nullptr);
    CHECK_MSG(sys.module != nullptr, "err=%lu", sys.error);
    if (sys.module) {
        wchar_t systemDir[MAX_PATH] = {};
        GetSystemDirectoryW(systemDir, MAX_PATH);
        CHECK_MSG(_wcsnicmp(sys.loadedPath, systemDir, wcslen(systemDir)) == 0 ||
                  wcsstr(sys.loadedPath, L"SysWOW64") != nullptr ||
                  wcsstr(sys.loadedPath, L"syswow64") != nullptr,
                  "loaded=%ls", sys.loadedPath);
        FreeLibrary(sys.module);
    }

    LoadResult missing = LoadSystemModule(L"as2_no_such_system_module.dll", nullptr);
    CHECK(missing.module == nullptr);
    CHECK_MSG(missing.error == ERROR_MOD_NOT_FOUND, "err=%lu", missing.error);
}

static void TestWrongArchitecture() {
    wchar_t windows[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) {
        return;
    }
    wchar_t native[MAX_PATH] = {};
    swprintf_s(native, MAX_PATH, L"%s\\Sysnative\\version.dll", windows);
    if (GetFileAttributesW(native) == INVALID_FILE_ATTRIBUTES) {
        printf("NOTE Sysnative unavailable; wrong-architecture case not run\n");
        return;
    }
    LoadResult wrong = LoadPackageModule(native, nullptr);
    CHECK(wrong.module == nullptr);
    CHECK_MSG(wrong.error == ERROR_BAD_EXE_FORMAT, "err=%lu", wrong.error);
}

struct GateThreadArgs {
    BootstrapGate* gate;
    BootstrapGate::Outcome outcome;
    DWORD elapsedMs;
    DWORD timeoutMs;
};

static DWORD WINAPI GateThread(LPVOID param) {
    GateThreadArgs* args = (GateThreadArgs*)param;
    const DWORD start = GetTickCount();
    args->outcome = args->gate->Enter(args->timeoutMs);
    args->elapsedMs = GetTickCount() - start;
    return 0;
}

static void TestBootstrapGate() {
    BootstrapGate gate;
    CHECK(!gate.IsComplete());
    CHECK(gate.Enter(0) == BootstrapGate::Outcome::Acquired);
    CHECK(gate.IsRunning());
    CHECK(gate.OwnerThreadId() == GetCurrentThreadId());
    CHECK(gate.Enter(0) == BootstrapGate::Outcome::ReentrantSameThread);

    // Another thread waits until the owner completes.
    GateThreadArgs waiter = { &gate, BootstrapGate::Outcome::TimedOut, 0, 5000 };
    HANDLE thread = CreateThread(nullptr, 0, GateThread, &waiter, 0, nullptr);
    CHECK(thread != nullptr);
    Sleep(50);
    CHECK(gate.IsRunning());
    gate.Complete();
    if (thread) {
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
    CHECK(waiter.outcome == BootstrapGate::Outcome::CompletedElsewhere);
    CHECK(waiter.elapsedMs < 5000);
    CHECK(gate.Enter(0) == BootstrapGate::Outcome::CompletedElsewhere);
    CHECK(gate.IsComplete());

    // Bounded wait: a stuck owner yields TimedOut on other threads.
    BootstrapGate stuck;
    CHECK(stuck.Enter(0) == BootstrapGate::Outcome::Acquired);
    GateThreadArgs timedOut = { &stuck, BootstrapGate::Outcome::Acquired, 0, 100 };
    HANDLE t2 = CreateThread(nullptr, 0, GateThread, &timedOut, 0, nullptr);
    if (t2) {
        WaitForSingleObject(t2, 5000);
        CloseHandle(t2);
    }
    CHECK(timedOut.outcome == BootstrapGate::Outcome::TimedOut);
    CHECK(timedOut.elapsedMs >= 90);
}

static void TestWineDetectionShape() {
    char version[64], build[128], host[128];
    const bool wine = DetectWine(version, sizeof(version), build, sizeof(build), host, sizeof(host));
    printf("NOTE runtime: %s%s%s\n", wine ? "Wine " : "native Windows", wine ? version : "", wine ? "" : "");
    if (wine) {
        CHECK(version[0] != '\0');
    } else {
        CHECK(version[0] == '\0');
    }
}

int main() {
    TestPathClassification();

    Fixture fx;
    if (PrepareFixture(fx)) {
        TestPackageLoad(fx);
        TestBoundedSearchExcludesCwdAndPath(fx);
    } else {
        printf("SKIP package fixtures (see above)\n");
    }
    CleanupFixture(fx);

    TestSystemLoad();
    TestWrongArchitecture();
    TestBootstrapGate();
    TestWineDetectionShape();

    printf("module_loader_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
