/**
 * Alice Senki 2 - Japanese Locale Patch
 * 
 * Hooks GetOEMCP, GetACP, and MultiByteToWideChar to return/use
 * codepage 932 (Shift-JIS), allowing the game to work correctly
 * on non-Japanese Windows systems.
 *
 * Extracted from as2_rollback.cpp (non-rollback patch).
 */

#include "locale_patch.h"

// Original function pointers — populated by MinHook.
GetOEMCP_t g_origGetOEMCP = nullptr;
GetACP_t g_origGetACP = nullptr;
MultiByteToWideChar_t g_origMultiByteToWideChar = nullptr;

UINT WINAPI Hook_GetOEMCP() {
    return CP_JAPANESE_SHIFTJIS;  // 932 = Japanese Shift-JIS
}

UINT WINAPI Hook_GetACP() {
    return CP_JAPANESE_SHIFTJIS;  // 932 = Japanese Shift-JIS
}

int WINAPI Hook_MultiByteToWideChar(UINT CodePage, DWORD dwFlags,
    LPCCH lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar)
{
    // Redirect CP_ACP (0) and CP_OEMCP (1) to CP932 so all internal
    // MultiByteToWideChar calls in kernel32/DXLib use Shift-JIS.
    if (CodePage == CP_ACP || CodePage == CP_OEMCP) {
        CodePage = CP_JAPANESE_SHIFTJIS;
    }
    return g_origMultiByteToWideChar(CodePage, dwFlags, lpMultiByteStr, cbMultiByte,
                                     lpWideCharStr, cchWideChar);
}
