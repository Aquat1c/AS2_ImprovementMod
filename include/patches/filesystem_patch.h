/**
 * Alice Senki 2 - Filesystem Shift-JIS Path Patch
 * 
 * On non-Japanese Windows, kernel32's CreateFileA converts narrow paths using
 * the system's NLS ANSI codepage (not GetACP). Shift-JIS filenames in the game
 * (e.g. config.dat path at 0x7445E4) get garbled. We intercept the A-suffix
 * APIs, convert through CP932 ourselves, and call the W-suffix equivalents.
 *
 * Extracted from as2_rollback.cpp (non-rollback patch).
 */

#pragma once

#include <windows.h>

// Hook functions — install via MinHook targeting the Win32 originals.
HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecAttr,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);

BOOL   WINAPI Hook_DeleteFileA(LPCSTR lpFileName);
HANDLE WINAPI Hook_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
DWORD  WINAPI Hook_GetFileAttributesA(LPCSTR lpFileName);

// Original function pointers — set by MinHook during hook installation.
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL   (WINAPI *DeleteFileA_t)(LPCSTR);
typedef HANDLE (WINAPI *FindFirstFileA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef DWORD  (WINAPI *GetFileAttributesA_t)(LPCSTR);

extern CreateFileA_t g_origCreateFileA;
extern DeleteFileA_t g_origDeleteFileA;
extern FindFirstFileA_t g_origFindFirstFileA;
extern GetFileAttributesA_t g_origGetFileAttributesA;
