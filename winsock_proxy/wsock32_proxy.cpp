/**
 * Minimal wsock32.dll proxy — duplicate-instance bypass
 *
 * The game statically imports WSOCK32.dll, so this DLL loads at process start
 * (before DXLib_Init).  Its sole purpose is to IAT-hook FindWindowA so the
 * DXLib duplicate-instance check always sees "no other window" and allows a
 * second copy to run.
 *
 * All 16 wsock32 functions the game actually uses are forwarded to the real
 * wsock32.dll via GetProcAddress.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

// ============================================================================
// Logging (minimal — just a file, no console)
// ============================================================================

static FILE* g_logFile = nullptr;

static void WsockLog(const char* fmt, ...) {
    if (!g_logFile) {
        char dir[MAX_PATH];
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char* slash = strrchr(dir, '\\');
        if (slash) *slash = '\0';

        char logsBase[MAX_PATH];
        snprintf(logsBase, MAX_PATH, "%s\\logs", dir);
        CreateDirectoryA(logsBase, nullptr);

        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\wsock32_proxy_%lu.log",
                 logsBase, GetCurrentProcessId());
        g_logFile = fopen(path, "w");
        if (!g_logFile) return;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

// ============================================================================
// FindWindowA IAT hook — always returns NULL so DXLib sees no duplicate
// ============================================================================

static HWND WINAPI Hooked_FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName) {
    WsockLog("[DUPBYPASS] FindWindowA intercepted: class='%s' -> returning NULL",
             lpClassName ? lpClassName : "(null)");
    return NULL;
}

static void PatchFindWindowA() {
    HMODULE game = GetModuleHandleA(NULL);
    if (!game) return;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(game);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(game) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress) return;

    auto base = reinterpret_cast<uint8_t*>(game);
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + importDir.VirtualAddress);

    // Resolve the real FindWindowA so we can match IAT entries
    auto realFn = reinterpret_cast<uintptr_t>(::FindWindowA);

    for (; desc->Name; ++desc) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            if (thunk->u1.Function == realFn) {
                DWORD oldProt = 0;
                if (VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t),
                                   PAGE_READWRITE, &oldProt)) {
                    thunk->u1.Function =
                        reinterpret_cast<uintptr_t>(&Hooked_FindWindowA);
                    VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t),
                                   oldProt, &oldProt);
                    WsockLog("[DUPBYPASS] FindWindowA IAT patched OK");
                } else {
                    WsockLog("[DUPBYPASS] VirtualProtect failed (err %lu)",
                             GetLastError());
                }
                return;
            }
        }
    }
    WsockLog("[DUPBYPASS] FindWindowA not found in IAT (already patched?)");
}

// ============================================================================
// Set DXLib duplicate flag directly in memory
// ============================================================================

static void SetDXLibDuplicateFlag() {
    // 0x9DB884 = dword_9DB884 in sub_630C80.  When non-zero the duplicate
    // check branch is skipped even if FindWindowA returns a handle.
    volatile uint32_t* flag = reinterpret_cast<volatile uint32_t*>(0x9DB884);
    *flag = 1;
    WsockLog("[DUPBYPASS] Set dword_9DB884 = 1");
}

// ============================================================================
// Forward real wsock32 exports
// ============================================================================

static HMODULE g_hRealWsock32 = nullptr;

#define DECL_FWD(name) static decltype(&::name) g_real_##name = nullptr;
DECL_FWD(socket)
DECL_FWD(htons)
DECL_FWD(WSAStartup)
DECL_FWD(bind)
DECL_FWD(ioctlsocket)
DECL_FWD(WSAGetLastError)
DECL_FWD(recvfrom)
DECL_FWD(sendto)
DECL_FWD(WSACleanup)
DECL_FWD(inet_addr)
DECL_FWD(closesocket)
DECL_FWD(htonl)
DECL_FWD(WSAAsyncSelect)
DECL_FWD(accept)
DECL_FWD(recv)
DECL_FWD(send)
#undef DECL_FWD

static bool LoadRealWsock32() {
    // Load the real wsock32.dll from System32
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\wsock32.dll", sysDir);

    g_hRealWsock32 = LoadLibraryA(path);
    if (!g_hRealWsock32) {
        WsockLog("ERROR: Cannot load real wsock32.dll from %s (err %lu)",
                 path, GetLastError());
        return false;
    }
    WsockLog("Real wsock32.dll loaded from %s", path);

#define RESOLVE(name) \
    g_real_##name = reinterpret_cast<decltype(g_real_##name)>( \
        GetProcAddress(g_hRealWsock32, #name)); \
    if (!g_real_##name) WsockLog("WARN: " #name " not found in real wsock32");

    RESOLVE(socket)
    RESOLVE(htons)
    RESOLVE(WSAStartup)
    RESOLVE(bind)
    RESOLVE(ioctlsocket)
    RESOLVE(WSAGetLastError)
    RESOLVE(recvfrom)
    RESOLVE(sendto)
    RESOLVE(WSACleanup)
    RESOLVE(inet_addr)
    RESOLVE(closesocket)
    RESOLVE(htonl)
    RESOLVE(WSAAsyncSelect)
    RESOLVE(accept)
    RESOLVE(recv)
    RESOLVE(send)
#undef RESOLVE

    return true;
}

// ============================================================================
// Exported winsock functions (forwarded to real wsock32.dll)
// ============================================================================

extern "C" {

__declspec(dllexport) SOCKET PASCAL proxy_socket(int af, int type, int protocol) {
    return g_real_socket ? g_real_socket(af, type, protocol) : INVALID_SOCKET;
}

__declspec(dllexport) u_short PASCAL proxy_htons(u_short hostshort) {
    return g_real_htons ? g_real_htons(hostshort) : 0;
}

__declspec(dllexport) int PASCAL proxy_WSAStartup(WORD wVersionRequired, LPWSADATA lpWSAData) {
    return g_real_WSAStartup ? g_real_WSAStartup(wVersionRequired, lpWSAData) : WSASYSNOTREADY;
}

__declspec(dllexport) int PASCAL proxy_bind(SOCKET s, const struct sockaddr* addr, int namelen) {
    return g_real_bind ? g_real_bind(s, addr, namelen) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_ioctlsocket(SOCKET s, long cmd, u_long* argp) {
    return g_real_ioctlsocket ? g_real_ioctlsocket(s, cmd, argp) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_WSAGetLastError(void) {
    return g_real_WSAGetLastError ? g_real_WSAGetLastError() : 0;
}

__declspec(dllexport) int PASCAL proxy_recvfrom(SOCKET s, char* buf, int len, int flags,
                                                 struct sockaddr* from, int* fromlen) {
    return g_real_recvfrom ? g_real_recvfrom(s, buf, len, flags, from, fromlen) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_sendto(SOCKET s, const char* buf, int len, int flags,
                                               const struct sockaddr* to, int tolen) {
    return g_real_sendto ? g_real_sendto(s, buf, len, flags, to, tolen) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_WSACleanup(void) {
    return g_real_WSACleanup ? g_real_WSACleanup() : SOCKET_ERROR;
}

__declspec(dllexport) unsigned long PASCAL proxy_inet_addr(const char* cp) {
    return g_real_inet_addr ? g_real_inet_addr(cp) : INADDR_NONE;
}

__declspec(dllexport) int PASCAL proxy_closesocket(SOCKET s) {
    return g_real_closesocket ? g_real_closesocket(s) : SOCKET_ERROR;
}

__declspec(dllexport) u_long PASCAL proxy_htonl(u_long hostlong) {
    return g_real_htonl ? g_real_htonl(hostlong) : 0;
}

__declspec(dllexport) int PASCAL proxy_WSAAsyncSelect(SOCKET s, HWND hWnd, u_int wMsg, long lEvent) {
    return g_real_WSAAsyncSelect ? g_real_WSAAsyncSelect(s, hWnd, wMsg, lEvent) : SOCKET_ERROR;
}

__declspec(dllexport) SOCKET PASCAL proxy_accept(SOCKET s, struct sockaddr* addr, int* addrlen) {
    return g_real_accept ? g_real_accept(s, addr, addrlen) : INVALID_SOCKET;
}

__declspec(dllexport) int PASCAL proxy_recv(SOCKET s, char* buf, int len, int flags) {
    return g_real_recv ? g_real_recv(s, buf, len, flags) : SOCKET_ERROR;
}

__declspec(dllexport) int PASCAL proxy_send(SOCKET s, const char* buf, int len, int flags) {
    return g_real_send ? g_real_send(s, buf, len, flags) : SOCKET_ERROR;
}

} // extern "C"

// ============================================================================
// DLL Entry Point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        WsockLog("wsock32 proxy loaded (PID %lu)", GetCurrentProcessId());

        // 1. Set the DXLib duplicate-allow flag
        SetDXLibDuplicateFlag();

        // 2. Patch FindWindowA in the game's IAT
        PatchFindWindowA();

        // 3. Load real wsock32.dll and resolve function pointers
        if (!LoadRealWsock32()) {
            WsockLog("FATAL: Could not load real wsock32.dll — aborting");
            return FALSE;
        }

        WsockLog("wsock32 proxy init complete");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_hRealWsock32) {
            FreeLibrary(g_hRealWsock32);
            g_hRealWsock32 = nullptr;
        }
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }
    return TRUE;
}
