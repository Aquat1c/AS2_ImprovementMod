/**
 * Alice Senki 2 - Japanese Locale Patch
 * 
 * Hooks GetOEMCP, GetACP, and MultiByteToWideChar to return/use
 * codepage 932 (Shift-JIS), allowing the game to work correctly
 * on non-Japanese Windows systems.
 *
 * Extracted from as2_rollback.cpp (non-rollback patch).
 */

#pragma once

#include <windows.h>

// Japanese codepage constant
#define CP_JAPANESE_SHIFTJIS 932

// Hook functions — install via MinHook targeting the Win32 originals.
UINT WINAPI Hook_GetOEMCP();
UINT WINAPI Hook_GetACP();
int  WINAPI Hook_MultiByteToWideChar(UINT CodePage, DWORD dwFlags,
         LPCCH lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);

// Original function pointers — set by MinHook during hook installation.
typedef UINT (WINAPI *GetOEMCP_t)();
typedef UINT (WINAPI *GetACP_t)();
typedef int  (WINAPI *MultiByteToWideChar_t)(UINT, DWORD, LPCCH, int, LPWSTR, int);

extern GetOEMCP_t g_origGetOEMCP;
extern GetACP_t g_origGetACP;
extern MultiByteToWideChar_t g_origMultiByteToWideChar;
