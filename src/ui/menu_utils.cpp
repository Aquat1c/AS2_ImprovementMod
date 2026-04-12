/**
 * Alice Senki 2 - Menu Utilities
 *
 * Reusable UI helpers for the netplay menu frontend.
 * Ported from the old working netplay_menu_controller.cpp.
 */

#include "ui/menu_utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

namespace MenuUtils {

// ============================================================================
// Internal state
// ============================================================================

static char           s_publicIP[48]         = "";
static char           s_yourAddress[96]      = "";
static volatile LONG  s_publicIPState        = 0;  // 0=idle, 1=fetching, 2=done, 3=failed
static volatile LONG  s_publicIPGeneration   = 0;
static HANDLE         s_ipThread             = NULL;
static char           s_clipboardFlash[48]   = "";
static DWORD          s_clipboardFlashTime   = 0;

struct PublicIPThreadContext {
    LONG generation;
};

static LONG GetCurrentPublicIPGeneration() {
    return InterlockedCompareExchange(&s_publicIPGeneration, 0, 0);
}

static bool IsPublicIPGenerationCurrent(LONG generation) {
    return GetCurrentPublicIPGeneration() == generation;
}

static void CompletePublicIPFetch(LONG generation, LONG state, const char* ipText) {
    if (!IsPublicIPGenerationCurrent(generation)) {
        return;
    }

    if (state == 2 && ipText && ipText[0]) {
        strncpy_s(s_publicIP, sizeof(s_publicIP), ipText, _TRUNCATE);
    }

    InterlockedExchange(&s_publicIPState, state);
}

static void ReapPublicIPThreadIfFinished() {
    if (!s_ipThread) {
        return;
    }

    if (WaitForSingleObject(s_ipThread, 0) == WAIT_OBJECT_0) {
        CloseHandle(s_ipThread);
        s_ipThread = NULL;
    }
}

// ============================================================================
// Public IP Lookup
// ============================================================================

static DWORD WINAPI FetchPublicIPThread(LPVOID param) {
    PublicIPThreadContext* context = static_cast<PublicIPThreadContext*>(param);
    const LONG generation = context ? context->generation : 0;
    free(context);

    HINTERNET hSession = WinHttpOpen(L"AS2Rollback/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { CompletePublicIPFetch(generation, 3, nullptr); return 1; }

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.ipify.org",
        INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        CompletePublicIPFetch(generation, 3, nullptr);
        return 1;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/", NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        CompletePublicIPFetch(generation, 3, nullptr);
        return 1;
    }

    WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 5000);

    BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        CompletePublicIPFetch(generation, 3, nullptr);
        return 1;
    }

    char buf[128] = {};
    DWORD bytesRead = 0;
    WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead);
    buf[bytesRead] = '\0';

    // Validate: plain IP string (IPv4 or IPv6), no HTML/whitespace.
    bool valid = (bytesRead >= 2 && bytesRead <= 64);
    for (DWORD i = 0; i < bytesRead && valid; i++) {
        const char ch = buf[i];
        const bool ok =
            (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f') ||
            (ch >= 'A' && ch <= 'F') ||
            ch == '.' || ch == ':' || ch == '\n' || ch == '\r';
        if (!ok) {
            valid = false;
        }
    }
    if (valid) {
        // Strip trailing whitespace/newlines
        while (bytesRead > 0 && (buf[bytesRead - 1] == '\n' ||
               buf[bytesRead - 1] == '\r' || buf[bytesRead - 1] == ' '))
            buf[--bytesRead] = '\0';
        CompletePublicIPFetch(generation, 2, buf);
    } else {
        CompletePublicIPFetch(generation, 3, nullptr);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 0;
}

void BeginPublicIPFetch() {
    ReapPublicIPThreadIfFinished();

    LONG state = InterlockedCompareExchange(&s_publicIPState, 1, 0);
    if (state == 2) return;  // already have it
    if (state == 1 && s_ipThread) return;  // already fetching

    PublicIPThreadContext* context = static_cast<PublicIPThreadContext*>(malloc(sizeof(PublicIPThreadContext)));
    if (!context) {
        InterlockedExchange(&s_publicIPState, 3);
        return;
    }

    context->generation = InterlockedIncrement(&s_publicIPGeneration);
    InterlockedExchange(&s_publicIPState, 1);
    s_ipThread = CreateThread(NULL, 0, FetchPublicIPThread, context, 0, NULL);
    if (!s_ipThread) {
        free(context);
        InterlockedExchange(&s_publicIPState, 3);
    }
}

IPFetchState GetPublicIPState() {
    LONG state = InterlockedCompareExchange(&s_publicIPState, 0, 0);
    switch (state) {
        case 1:  return IPFetchState::Fetching;
        case 2:  return IPFetchState::Done;
        case 3:  return IPFetchState::Failed;
        default: return IPFetchState::Idle;
    }
}

const char* GetPublicIP() {
    return s_publicIP;
}

// ============================================================================
// "Your Address" Formatting
// ============================================================================

const char* UpdateYourAddress(uint16_t listenPort) {
    if (s_publicIPState == 2 && s_publicIP[0]) {
        if (strchr(s_publicIP, ':')) {
            _snprintf_s(s_yourAddress, sizeof(s_yourAddress), _TRUNCATE,
                "[%s]:%u", s_publicIP, (unsigned)listenPort);
        } else {
            _snprintf_s(s_yourAddress, sizeof(s_yourAddress), _TRUNCATE,
                "%s:%u", s_publicIP, (unsigned)listenPort);
        }
    } else {
        _snprintf_s(s_yourAddress, sizeof(s_yourAddress), _TRUNCATE,
            "?:%u", (unsigned)listenPort);
    }
    return s_yourAddress;
}

const char* GetYourAddress() {
    return s_yourAddress;
}

// ============================================================================
// Clipboard
// ============================================================================

bool CopyToClipboard(const char* text) {
    if (!text || !text[0]) return false;
    if (!OpenClipboard(NULL)) return false;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (hMem) {
        char* p = (char*)GlobalLock(hMem);
        if (p) { memcpy(p, text, len); GlobalUnlock(hMem); }
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
    return hMem != NULL;
}

bool PasteFromClipboard(char* dst, size_t cap) {
    if (!dst || cap == 0) return false;
    if (!OpenClipboard(NULL)) return false;
    HANDLE hData = GetClipboardData(CF_TEXT);
    bool ok = false;
    if (hData) {
        const char* p = (const char*)GlobalLock(hData);
        if (p) {
            strncpy_s(dst, cap, p, _TRUNCATE);
            // Strip trailing whitespace
            size_t len = strlen(dst);
            while (len > 0 && (dst[len - 1] == '\n' || dst[len - 1] == '\r' ||
                   dst[len - 1] == ' '))
                dst[--len] = '\0';
            ok = true;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return ok;
}

// ============================================================================
// Clipboard Flash Feedback
// ============================================================================

void FlashClipboardMessage(const char* msg) {
    strncpy_s(s_clipboardFlash, sizeof(s_clipboardFlash), msg ? msg : "", _TRUNCATE);
    s_clipboardFlashTime = GetTickCount();
}

bool HasClipboardFlash() {
    if (!s_clipboardFlash[0]) return false;
    if (GetTickCount() - s_clipboardFlashTime > 2000) {
        s_clipboardFlash[0] = '\0';
        return false;
    }
    return true;
}

const char* GetClipboardFlash() {
    if (!HasClipboardFlash()) return "";
    return s_clipboardFlash;
}

// ============================================================================
// Display String Helpers
// ============================================================================

void CopyDisplayText(char* dst, size_t cap, const char* src, size_t maxChars) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t len = strlen(src);
    if (len <= maxChars || maxChars < 4) {
        strncpy_s(dst, cap, src, _TRUNCATE);
        return;
    }
    _snprintf_s(dst, cap, _TRUNCATE, "%.*s...", (int)(maxChars - 3), src);
}

// ============================================================================
// Lifecycle
// ============================================================================

void Cleanup() {
    InterlockedIncrement(&s_publicIPGeneration);
    ReapPublicIPThreadIfFinished();

    if (s_ipThread) {
        CloseHandle(s_ipThread);
        s_ipThread = NULL;
    }
    s_publicIP[0] = '\0';
    s_yourAddress[0] = '\0';
    s_clipboardFlash[0] = '\0';
    InterlockedExchange(&s_publicIPState, 0);
}

} // namespace MenuUtils
