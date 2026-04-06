/**
 * Alice Senki 2 - Filesystem Shift-JIS Path Patch
 *
 * On non-Japanese Windows, kernel32's CreateFileA converts narrow paths using
 * the system's NLS ANSI codepage (not GetACP). Shift-JIS filenames in the game
 * get garbled. We intercept A-suffix APIs, convert through CP932, and call W-suffix.
 *
 * Extracted from as2_rollback.cpp (non-rollback patch).
 */

#include "filesystem_patch.h"
#include "locale_patch.h"  // CP_JAPANESE_SHIFTJIS

// Original function pointers — populated by MinHook.
CreateFileA_t g_origCreateFileA = nullptr;
DeleteFileA_t g_origDeleteFileA = nullptr;
FindFirstFileA_t g_origFindFirstFileA = nullptr;
GetFileAttributesA_t g_origGetFileAttributesA = nullptr;

static bool PathContainsNonASCII(const char* path) {
    if (!path) return false;
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        if (*p > 0x7F) return true;
    }
    return false;
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecAttr,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (lpFileName && PathContainsNonASCII(lpFileName)) {
        WCHAR widePath[MAX_PATH];
        int len = MultiByteToWideChar(CP_JAPANESE_SHIFTJIS, 0, lpFileName, -1, widePath, MAX_PATH);
        if (len > 0) {
            return CreateFileW(widePath, dwDesiredAccess, dwShareMode, lpSecAttr,
                               dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
        }
    }
    return g_origCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecAttr,
                             dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL WINAPI Hook_DeleteFileA(LPCSTR lpFileName) {
    if (lpFileName && PathContainsNonASCII(lpFileName)) {
        WCHAR widePath[MAX_PATH];
        int len = MultiByteToWideChar(CP_JAPANESE_SHIFTJIS, 0, lpFileName, -1, widePath, MAX_PATH);
        if (len > 0) {
            return DeleteFileW(widePath);
        }
    }
    return g_origDeleteFileA(lpFileName);
}

HANDLE WINAPI Hook_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    if (lpFileName && PathContainsNonASCII(lpFileName)) {
        WCHAR widePath[MAX_PATH];
        int len = MultiByteToWideChar(CP_JAPANESE_SHIFTJIS, 0, lpFileName, -1, widePath, MAX_PATH);
        if (len > 0) {
            // Use wide find, then convert result back to narrow
            WIN32_FIND_DATAW wideData;
            HANDLE h = FindFirstFileW(widePath, &wideData);
            if (h != INVALID_HANDLE_VALUE && lpFindFileData) {
                lpFindFileData->dwFileAttributes = wideData.dwFileAttributes;
                lpFindFileData->ftCreationTime = wideData.ftCreationTime;
                lpFindFileData->ftLastAccessTime = wideData.ftLastAccessTime;
                lpFindFileData->ftLastWriteTime = wideData.ftLastWriteTime;
                lpFindFileData->nFileSizeHigh = wideData.nFileSizeHigh;
                lpFindFileData->nFileSizeLow = wideData.nFileSizeLow;
                lpFindFileData->dwReserved0 = wideData.dwReserved0;
                lpFindFileData->dwReserved1 = wideData.dwReserved1;
                WideCharToMultiByte(CP_JAPANESE_SHIFTJIS, 0, wideData.cFileName, -1,
                    lpFindFileData->cFileName, MAX_PATH, NULL, NULL);
                WideCharToMultiByte(CP_JAPANESE_SHIFTJIS, 0, wideData.cAlternateFileName, -1,
                    lpFindFileData->cAlternateFileName, 14, NULL, NULL);
            }
            return h;
        }
    }
    return g_origFindFirstFileA(lpFileName, lpFindFileData);
}

DWORD WINAPI Hook_GetFileAttributesA(LPCSTR lpFileName) {
    if (lpFileName && PathContainsNonASCII(lpFileName)) {
        WCHAR widePath[MAX_PATH];
        int len = MultiByteToWideChar(CP_JAPANESE_SHIFTJIS, 0, lpFileName, -1, widePath, MAX_PATH);
        if (len > 0) {
            return GetFileAttributesW(widePath);
        }
    }
    return g_origGetFileAttributesA(lpFileName);
}
