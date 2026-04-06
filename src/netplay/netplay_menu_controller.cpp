#include "netplay_menu_controller.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "as2_rollback.h"
#include "charsel_sync.h"
#include "game_state.h"
#include "gekko_bridge.h"
#include "input_system.h"
#include "input_sync_hooks.h"
#include "log_window.h"
#include "match_bootstrap.h"
#include "netplay_config.h"
#include "netplay_hooks.h"
#include "session_manager.h"
#include "upnp_manager.h"

#include <MinHook.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

namespace NetplayMenuController {

static void RenderInGameMenu();

namespace {

static void OpenDisconnectError(const char* why);

constexpr uintptr_t ADDR_MAIN_MENU_STATE_MACHINE = 0x5FB200;
constexpr uintptr_t ADDR_SET_GAME_MODE = 0x5D2EB0;
constexpr uintptr_t ADDR_RENDER_FILL_RECT = 0x5D2F50;
constexpr uintptr_t ADDR_RENDER_SET_BLEND = 0x5D2F80;
constexpr uintptr_t ADDR_RENDER_SET_COLOR = 0x5D3030;
constexpr uintptr_t ADDR_RENDER_DRAW_SPRITE = 0x5D3130;
constexpr uintptr_t ADDR_RENDER_CREATE_COLOR = 0x5D3150;
constexpr uintptr_t ADDR_DRAW_FORMAT_STRING = 0x629A20;
constexpr uintptr_t ADDR_FADE_TIMER = 0x816370;
constexpr uintptr_t ADDR_TITLE_MENU_STATE = 0x8EA000;
constexpr uintptr_t ADDR_TITLE_MENU_SELECTION = 0x8EA002;
constexpr uintptr_t ADDR_TITLE_MENU_BG_ACTIVE = 0x8EA00C;
constexpr uintptr_t ADDR_MY_NICKNAME = 0x8E9468;
constexpr uintptr_t ADDR_MY_PORT = 0x8E9480;
constexpr uintptr_t ADDR_MY_TARGET_IP1 = 0x8E9484;
constexpr uintptr_t ADDR_MY_TARGET_IP2 = 0x8E9488;
constexpr uintptr_t ADDR_MY_TARGET_IP3 = 0x8E948C;
constexpr uintptr_t ADDR_MY_TARGET_IP4 = 0x8E9490;
constexpr uintptr_t ADDR_MY_TARGET_PORT = 0x8E9494;
constexpr uintptr_t ADDR_LOBBY_PORT = 0x7AC234;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP1 = 0x7AC260;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP2 = 0x7AC264;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP3 = 0x7AC268;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP4 = 0x7AC26C;
constexpr uintptr_t ADDR_LOBBY_TARGET_PORT = 0x7AC270;
constexpr uintptr_t ADDR_LOBBY_MY_NICK = 0x7AC274;
constexpr uintptr_t ADDR_P1_CPU_FLAG = 0x8E9F0C;        // byte_8E9F0C: entity+172 for P1 (1=AI, 0=human)
constexpr uintptr_t ADDR_CHARSEL_STATE = 0x8E9F10;
constexpr uintptr_t ADDR_CHARSEL_P1_DATA = 0x8E9F14;
constexpr uintptr_t ADDR_CHARSEL_P1_COLOR = 0x8E9F2A;
constexpr uintptr_t ADDR_CHARSEL_P1_COLOR_B = 0x8E9F2C;
constexpr uintptr_t ADDR_P2_CPU_FLAG = 0x8E9FDC;        // byte_8E9FDC: entity+172 for P2 (1=AI, 0=human)
constexpr uintptr_t ADDR_CHARSEL_MODE_FLAG = 0x8E9FE0;
constexpr uintptr_t ADDR_CHARSEL_P2_DATA = 0x8E9FE4;
constexpr uintptr_t ADDR_CHARSEL_P2_COLOR = 0x8E9FFA;
constexpr uintptr_t ADDR_CHARSEL_P2_COLOR_B = 0x8E9FFC;

constexpr int kFadeFrames = 25;
constexpr size_t kNickCap = 21;
constexpr int kPanelLeft = 56;
constexpr int kPanelTop = 46;
constexpr int kPanelRight = 584;
constexpr int kPanelBottom = 438;
constexpr int kRowLeft = 72;
constexpr int kRowRight = 568;
constexpr int kLabelX = 80;
constexpr int kValueX = 268;
constexpr int kHeaderBottom = 88;
constexpr int kStatusY = 100;
constexpr int kRowStartY = 128;
constexpr int kRowStep = 26;
constexpr int kFooterTop = 358;
constexpr int kFooterLineStep = 16;
constexpr size_t kHeaderChars = 22;
constexpr size_t kStatusChars = 56;
constexpr size_t kRowLabelChars = 20;
constexpr size_t kRowValueChars = 32;
constexpr size_t kFooterChars = 40;

typedef int (__cdecl *SetGameMode_t)(int mode, char fade);
typedef char (__cdecl *MainMenuStateMachine_t)();
typedef int (__cdecl *RenderFillRect_t)(LONG left, LONG top, int right, int bottom, int color, int drawFlag);
typedef int (__cdecl *RenderSetBlendMode_t)(int blendMode, unsigned __int8 alphaValue);
typedef int (__cdecl *RenderSetDrawColor_t)(unsigned __int8 red, unsigned __int8 green, unsigned __int8 blue);
typedef int (__cdecl *RenderDrawSprite_t)(int x, int y, int spriteHandle, int flags);
typedef int (__cdecl *RenderCreateColor_t)(unsigned __int8 red, unsigned __int8 green, unsigned __int8 blue);
typedef int (__cdecl *DrawFormatString_t)(int xLeft, int yTop, unsigned int color, char *Format, ...);
typedef int (__cdecl *NetInitHost_t)(unsigned short listenPortHostOrder);
typedef int (__cdecl *NetInitClient_t)(unsigned short port, char *ipAddress);
typedef int (__cdecl *NetCloseSocket_t)();
enum class MenuPhase { Hidden = 0, Opening, Active, Closing };
enum class TextEditField : uint32_t { None = 0, Nickname, RemoteEndpoint, ListenPort };

static MainMenuStateMachine_t s_origMainMenuStateMachine = nullptr;
static SetGameMode_t s_origSetGameMode = nullptr;
static NetInitHost_t s_origNetInitHost = nullptr;
static NetInitClient_t s_origNetInitClient = nullptr;
static NetCloseSocket_t s_origNetCloseHost = nullptr;
static NetCloseSocket_t s_origNetCloseClient = nullptr;
static bool s_initialized = false;
static bool s_hookInstalled = false;
static bool s_interceptEnabled = true;
static bool s_captureInput = false;
static bool s_lastInterceptFade = false;
static bool s_waitForNeutral = false;
static bool s_pendingMenuRestore = false;
static uint32_t s_interceptCount = 0;
static uint32_t s_lastInterceptSourceMode = MODE_MENU;
static uint32_t s_lastObservedMode = MODE_MENU;
static uint32_t s_lastObservedType = GAMETYPE_ARCADE;
static uint32_t s_lastStableGameType = GAMETYPE_ARCADE;
static NetplayRootBranch s_activeRootBranch = NetplayRootBranch::DirectPlay;
static SettingsCategory s_activeSettingsCategory = SettingsCategory::Identity;
static uint32_t s_stateRevision = 0;
static uint32_t s_selectedIndex = 0;
static uint32_t s_recentPeerViewIndex = 0;
static uint32_t s_lastMockRevision = 0;
static int s_fadeFrames = 0;
static MenuPhase s_phase = MenuPhase::Hidden;
static TextEditField s_textEditField = TextEditField::None;
static NetplayMenuState s_state = NetplayMenuState::Inactive;
static NetplayConfig::Config s_config{};
static char s_status[128] = "Waiting for network menu selection.";
static char s_lastError[128] = "";
static char s_textEditBuffer[64] = "";

// ── Public IP lookup (async) ────────────────────────────────────
static char s_publicIP[48] = "";            // e.g. "203.0.113.42"
static char s_yourAddress[64] = "";         // e.g. "203.0.113.42:10700"
static volatile LONG s_publicIPState = 0;   // 0=idle, 1=fetching, 2=done, 3=failed
static HANDLE s_ipThread = NULL;
static char s_clipboardFlash[48] = "";      // brief "Copied!" feedback
static DWORD s_clipboardFlashTime = 0;

static DWORD WINAPI FetchPublicIPThread(LPVOID) {
    HINTERNET hSession = WinHttpOpen(L"AS2Rollback/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { InterlockedExchange(&s_publicIPState, 3); return 1; }

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.ipify.org", INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); InterlockedExchange(&s_publicIPState, 3); return 1; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/", NULL,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); InterlockedExchange(&s_publicIPState, 3); return 1; }

    // Set a short timeout (5s connect, 5s send, 5s receive)
    WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 5000);

    BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        InterlockedExchange(&s_publicIPState, 3);
        return 1;
    }

    char buf[128] = {};
    DWORD bytesRead = 0;
    WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead);
    buf[bytesRead] = '\0';

    // Validate: should be a simple dotted-quad IP, no HTML
    bool valid = (bytesRead >= 7 && bytesRead <= 45 && buf[0] >= '0' && buf[0] <= '9');
    if (valid) {
        // Strip any trailing whitespace/newlines
        while (bytesRead > 0 && (buf[bytesRead-1] == '\n' || buf[bytesRead-1] == '\r' || buf[bytesRead-1] == ' '))
            buf[--bytesRead] = '\0';
        strncpy_s(s_publicIP, sizeof(s_publicIP), buf, _TRUNCATE);
        InterlockedExchange(&s_publicIPState, 2);
    } else {
        InterlockedExchange(&s_publicIPState, 3);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 0;
}

static void BeginPublicIPFetch() {
    LONG state = InterlockedCompareExchange(&s_publicIPState, 1, 0);
    if (state == 2) return;  // already have it
    if (state == 1) return;  // already fetching
    // state was 0 or 3 (idle or failed) — start fetch
    if (state == 3) InterlockedExchange(&s_publicIPState, 1);
    s_ipThread = CreateThread(NULL, 0, FetchPublicIPThread, NULL, 0, NULL);
    if (!s_ipThread) InterlockedExchange(&s_publicIPState, 3);
}

static void UpdateYourAddress() {
    if (s_publicIPState == 2 && s_publicIP[0]) {
        _snprintf_s(s_yourAddress, sizeof(s_yourAddress), _TRUNCATE, "%s:%u", s_publicIP, s_config.listen_port);
    } else {
        // Fall back to UPnP-discovered external IP if ipify fetch failed.
        const char* upnpIP = UpnpManager::GetExternalIP();
        if (upnpIP && upnpIP[0]) {
            _snprintf_s(s_yourAddress, sizeof(s_yourAddress), _TRUNCATE, "%s:%u", upnpIP, s_config.listen_port);
        } else {
            _snprintf_s(s_yourAddress, sizeof(s_yourAddress), _TRUNCATE, "?:%u", s_config.listen_port);
        }
    }
}

// ── Clipboard helpers ───────────────────────────────────────────
static bool CopyToClipboard(const char* text) {
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

static bool PasteFromClipboard(char* dst, size_t cap) {
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
            while (len > 0 && (dst[len-1] == '\n' || dst[len-1] == '\r' || dst[len-1] == ' '))
                dst[--len] = '\0';
            ok = true;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return ok;
}

static void FlashClipboardMessage(const char* msg) {
    strncpy_s(s_clipboardFlash, sizeof(s_clipboardFlash), msg, _TRUNCATE);
    s_clipboardFlashTime = GetTickCount();
}

static bool HasClipboardFlash() {
    if (!s_clipboardFlash[0]) return false;
    if (GetTickCount() - s_clipboardFlashTime > 2000) { s_clipboardFlash[0] = '\0'; return false; }
    return true;
}

static uint8_t ReadU8(uintptr_t a, uint8_t d = 0) { __try { return *(volatile uint8_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return d; } }
static uint16_t ReadU16(uintptr_t a, uint16_t d = 0) { __try { return *(volatile uint16_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return d; } }
static uint32_t ReadU32(uintptr_t a, uint32_t d = 0) { __try { return *(volatile uint32_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return d; } }
static void WriteU8(uintptr_t a, uint8_t v) { __try { *(volatile uint8_t*)a = v; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void WriteU16(uintptr_t a, uint16_t v) { __try { *(volatile uint16_t*)a = v; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void WriteU32(uintptr_t a, uint32_t v) { __try { *(volatile uint32_t*)a = v; } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy_s(dst, cap, src, _TRUNCATE);
}

static void CopyDisplayText(char* dst, size_t cap, const char* src, size_t maxChars) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t len = strlen(src);
    if (len <= maxChars || maxChars < 4) {
        CopyText(dst, cap, src);
        return;
    }

    _snprintf_s(dst, cap, _TRUNCATE, "%.*s...", (int)(maxChars - 3), src);
}

template <typename... Args>
static void SetStatus(const char* fmt, Args... args) { _snprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, args...); }
template <typename... Args>
static void SetError(const char* fmt, Args... args) { _snprintf_s(s_lastError, sizeof(s_lastError), _TRUNCATE, fmt, args...); }
static void ClearError() { s_lastError[0] = '\0'; }
static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static bool MenuVisible() { return s_phase != MenuPhase::Hidden; }
static bool IsTextEditing() { return s_textEditField != TextEditField::None; }
static float FadeAlpha() { return (float)s_fadeFrames / (float)kFadeFrames; }
static int Alpha8(float normalized, int maxAlpha) {
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    return (int)(normalized * (float)maxAlpha);
}

static int GameCreateColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((RenderCreateColor_t)ADDR_RENDER_CREATE_COLOR)(r, g, b);
}

static void GameSetBlendMode(int mode, uint8_t alpha) {
    ((RenderSetBlendMode_t)ADDR_RENDER_SET_BLEND)(mode, alpha);
}

static void GameFillRect(int left, int top, int right, int bottom, uint8_t r, uint8_t g, uint8_t b) {
    ((RenderFillRect_t)ADDR_RENDER_FILL_RECT)(left, top, right, bottom, GameCreateColor(r, g, b), 1);
}

static void GameDrawSprite(int x, int y, int spriteHandle) {
    if (spriteHandle > 0) {
        ((RenderDrawSprite_t)ADDR_RENDER_DRAW_SPRITE)(x, y, spriteHandle, 1);
    }
}

static void GameDrawTextf(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    ((DrawFormatString_t)ADDR_DRAW_FORMAT_STRING)(x, y, (unsigned int)GameCreateColor(r, g, b), (char*)"%s", buffer);
}

static void GameSetDrawColor(uint8_t r, uint8_t g, uint8_t b) {
    ((RenderSetDrawColor_t)ADDR_RENDER_SET_COLOR)(r, g, b);
}

static uint32_t GetSafeMenuGameType() {
    uint32_t type = s_lastStableGameType;
    if (type == GAMETYPE_NETPLAY) type = GAMETYPE_VS_HUMAN;
    return type;
}

static void ClearVanillaNetplayFlags() {
    // Keep vanilla netplay fully disarmed. These bytes are no longer used as
    // mod state; we only clear them so the game never thinks a vanilla session
    // is active.
    WriteU8(ADDR_NETPLAY_ROLE, 0);
    WriteU8(ADDR_NETPLAY_CONNECTED, 0);
}

static bool ShouldBlockVanillaSocketLifecycle() {
    if (MenuVisible()) return true;
    if (CharSelSync::IsActive()) return true;
    if (MatchBootstrap::IsActive()) return true;
    if (InputSyncHooks::IsRollbackActive()) return true;
    if (InputSyncHooks::IsLoadBarrierFrozen()) return true;

    SessionManager::Snapshot snap{};
    return SessionManager::GetSnapshot(&snap) && (snap.active || snap.has_error);
}

static void SanitizeOwnedGameType(uint32_t mode, bool sessionActive, const char* reason) {
    uint32_t desiredType = GetGameType();
    bool shouldSanitize = false;

    if (MenuVisible() && mode == MODE_MENU) {
        desiredType = GetSafeMenuGameType();
        shouldSanitize = true;
    } else if (sessionActive && (mode == MODE_CHARSEL || mode == MODE_MATCH || mode == MODE_MENU)) {
        // Mod-owned CharSel/Match intentionally stay in offline VS_HUMAN.
        desiredType = GAMETYPE_VS_HUMAN;
        shouldSanitize = true;
    }

    if (!shouldSanitize) {
        return;
    }

    const uint32_t currentType = GetGameType();
    if (currentType == desiredType) {
        ClearVanillaNetplayFlags();
        return;
    }

    WriteU32(ADDR_GAME_TYPE, desiredType);
    ClearVanillaNetplayFlags();
    LOG_NETPLAY(LOG_WARNING,
        "[Menu] Sanitized game type %u -> %u during mod-owned flow (%s, mode=%u, menu=%d, session=%d)",
        currentType,
        desiredType,
        reason ? reason : "no-reason",
        mode,
        MenuVisible() ? 1 : 0,
        sessionActive ? 1 : 0);
}

static const char* GetStateDisplayName(NetplayMenuState state) {
    switch (state) {
        case NetplayMenuState::MenuRoot: return "Root";
        case NetplayMenuState::DirectConnectEntry: return "Host / Join";
        case NetplayMenuState::HostEntry: return "Host";
        case NetplayMenuState::JoinEntry: return "Join";
        case NetplayMenuState::SettingsCategoryMenu: return "Settings";
        case NetplayMenuState::SettingsEntry: return "Settings";
        case NetplayMenuState::Connecting: return "Connecting";
        case NetplayMenuState::Handshake: return "Handshake";
        case NetplayMenuState::ConnectedSession: return "Session Ready";
        case NetplayMenuState::CharSelTransition: return "Character Select";
        case NetplayMenuState::PostMatch: return "Post Match";
        case NetplayMenuState::DisconnectError: return "Disconnected";
        default: return "Inactive";
    }
}

static const char* GetRootBranchName(NetplayRootBranch branch) {
    switch (branch) {
        case NetplayRootBranch::DirectPlay: return "Direct Play";
        case NetplayRootBranch::Lobbies: return "Lobbies";
        case NetplayRootBranch::Settings: return "Settings";
        case NetplayRootBranch::Spectate: return "Spectate";
        default: return "Netplay";
    }
}

static const char* GetSettingsCategoryName(SettingsCategory category) {
    switch (category) {
        case SettingsCategory::Identity: return "Identity";
        case SettingsCategory::Endpoint: return "Endpoint";
        case SettingsCategory::SessionMatch: return "Session / Match";
        case SettingsCategory::TransportCompatibility: return "Transport";
        case SettingsCategory::Diagnostics: return "Diagnostics";
        default: return "Settings";
    }
}

static const char* GetHeaderTitle() {
    return s_activeRootBranch == NetplayRootBranch::Settings ? "Settings" : "Netplay";
}

static const char* GetHeaderSubtitle() {
    if (s_state == NetplayMenuState::SettingsCategoryMenu) {
        return "Categories";
    }
    if (s_state == NetplayMenuState::SettingsEntry) {
        return GetSettingsCategoryName(s_activeSettingsCategory);
    }
    if (s_state == NetplayMenuState::MenuRoot) {
        return "Root";
    }
    if (s_state == NetplayMenuState::HostEntry) {
        return "Host";
    }
    if (s_state == NetplayMenuState::JoinEntry) {
        return "Join";
    }
    return GetStateDisplayName(s_state);
}

static void TransitionTo(NetplayMenuState next, const char* why) {
    if (s_state == next) return;
    LOG_NETPLAY(LOG_INFO, "[Menu] State %s -> %s (%s)", GetStateName(s_state), GetStateName(next), why ? why : "no reason");
    s_state = next;
    ++s_stateRevision;
    // Kick off public IP lookup when entering Direct Play or connection states
    if (next == NetplayMenuState::DirectConnectEntry ||
        next == NetplayMenuState::HostEntry ||
        next == NetplayMenuState::JoinEntry ||
        next == NetplayMenuState::Connecting)
        BeginPublicIPFetch();
}

static void LoadCommittedConfig() {
    NetplayConfig::Load(&s_config);
    NetplayConfig::Clamp(&s_config);
    s_recentPeerViewIndex = 0;
}

static void CommitConfig(const char* reason) {
    NetplayConfig::Clamp(&s_config);
    const bool saved = NetplayConfig::Commit(&s_config);
    if (s_recentPeerViewIndex >= s_config.recent_peer_count) {
        s_recentPeerViewIndex = 0;
    }
    char endpoint[48];
    NetplayConfig::FormatEndpoint(&s_config, endpoint, sizeof(endpoint));
    LOG_NETPLAY(LOG_INFO,
        "[Config] Commit (%s): nick='%s' listen=%u target=%s delay=%d verbose=%d stats=%d hud=%d saved=%d recent=%u",
        reason ? reason : "unspecified",
        s_config.nickname,
        s_config.listen_port,
        endpoint,
        s_config.preferred_delay_frames,
        s_config.verbose_logging ? 1 : 0,
        s_config.show_connection_stats ? 1 : 0,
        s_config.show_netplay_hud ? 1 : 0,
        saved ? 1 : 0,
        s_config.recent_peer_count);
}

static void GetRecentPeerSummary(char* out, size_t cap) {
    if (!out || cap == 0) return;
    NetplayConfig::RecentPeer peer{};
    if (!NetplayConfig::GetRecentPeer(&s_config, s_recentPeerViewIndex, &peer)) {
        CopyText(out, cap, "No history");
        return;
    }

    char peerLabel[64];
    NetplayConfig::FormatRecentPeerLabel(&peer, peerLabel, sizeof(peerLabel));
    _snprintf_s(out, cap, _TRUNCATE, "%u/%u %s", s_recentPeerViewIndex + 1, s_config.recent_peer_count, peerLabel);
}

static void ClearTextEditState() {
    s_textEditField = TextEditField::None;
    s_textEditBuffer[0] = '\0';
}

static void BeginTextEdit(TextEditField field, const char* initialText, const char* statusText) {
    s_textEditField = field;
    CopyText(s_textEditBuffer, sizeof(s_textEditBuffer), initialText ? initialText : "");
    s_waitForNeutral = true;
    if (statusText && statusText[0]) {
        SetStatus("%s", statusText);
    }
}

static bool BrowseRecentPeer(int delta) {
    if (s_config.recent_peer_count == 0) {
        SetStatus("No recent peer saved yet.");
        return false;
    }

    int next = (int)s_recentPeerViewIndex + delta;
    while (next < 0) next += (int)s_config.recent_peer_count;
    while (next >= (int)s_config.recent_peer_count) next -= (int)s_config.recent_peer_count;
    s_recentPeerViewIndex = (uint32_t)next;

    char recent[80];
    GetRecentPeerSummary(recent, sizeof(recent));
    SetStatus("Recent peer selected: %s", recent);
    return true;
}

static void ResetCharSelFields() {
    WriteU32(ADDR_CHARSEL_STATE, 0);
    WriteU16(ADDR_CHARSEL_P1_DATA, 0);
    WriteU16(ADDR_CHARSEL_P2_DATA, 0);
    WriteU16(ADDR_CHARSEL_P1_COLOR, 0);
    WriteU8(ADDR_CHARSEL_P1_COLOR_B, 0);
    WriteU16(ADDR_CHARSEL_P2_COLOR, 0);
    WriteU8(ADDR_CHARSEL_P2_COLOR_B, 0);
    WriteU32(ADDR_CHARSEL_MODE_FLAG, 1);

    // Reset P1/P2 character cursor positions to vanilla defaults so both
    // sides start deterministically. Vanilla charsel init sets P1 cursor to
    // the grid index of the last-played character (default 0 = top-left) and
    // P2 cursor similarly (default 2 = top-right). We force these defaults
    // to prevent stale cursor positions from prior sessions/demos.
    WriteU8(ADDR_CHARSEL_P1_CURSOR, 0);   // Grid index 0 (top-left)
    WriteU8(ADDR_CHARSEL_P1_CONFIRM, 0);
    WriteU8(ADDR_CHARSEL_P1_AGE, 0);
    WriteU8(ADDR_CHARSEL_P2_CURSOR, 2);   // Grid index 2 (top-right)
    WriteU8(ADDR_CHARSEL_P2_CONFIRM, 0);
    WriteU8(ADDR_CHARSEL_P2_AGE, 0);

    // Reset stage cursor to position 0 — prevents desync when peers have
    // stale stage IDs from prior games or demo mode.
    WriteU8(ADDR_CHARSEL_STAGE_ID, 0);
}

static void HideMenuForExternalModeLaunch(const char* why) {
    LOG_NETPLAY(LOG_INFO, "[Menu] Releasing custom menu for external mode launch (%s)", why ? why : "no reason");
    SessionManager::Reset();
    s_lastMockRevision = 0;
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    s_pendingMenuRestore = false;
    ClearTextEditState();
    s_waitForNeutral = false;
    s_selectedIndex = 0;
    ClearError();
    SetStatus("Waiting for network menu selection.");
    TransitionTo(NetplayMenuState::Inactive, why ? why : "external mode launch");
    InputSystem_ResetRepeatState(0);
}

static bool LaunchOfflineVsDebug() {
    char endpoint[48];
    NetplayConfig::FormatEndpoint(&s_config, endpoint, sizeof(endpoint));
    LOG_NETPLAY(LOG_INFO,
        "[Menu] Launching offline VS debug path from custom netplay menu (target=%s, nick='%s')",
        endpoint,
        s_config.nickname);

    CommitConfig("offline vs debug launch");
    HideMenuForExternalModeLaunch("launch offline VS debug");

    // Mirror the vanilla VS Human (2P) path from the main menu handler before entering Mode 6.
    // Game type 2 = VS Human: both players have separate inputs, no AI.
    // byte_8E9FDC (P2 CPU flag) must be 0 so AI_Update() does NOT control P2.
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ClearVanillaNetplayFlags();
    WriteU8(ADDR_P1_CPU_FLAG, 0);
    WriteU8(ADDR_P2_CPU_FLAG, 0);

    // Force character selection enabled to prevent desync
    WriteU8(ADDR_CHARSEL_ENABLE, 1);

    const int result = s_origSetGameMode ? s_origSetGameMode(MODE_CHARSEL, 1) : ((SetGameMode_t)ADDR_SET_GAME_MODE)(MODE_CHARSEL, 1);

    ResetCharSelFields();

    LOG_NETPLAY(LOG_INFO,
        "[Menu] Offline VS debug launch requested: result=%d mode=%u sub=%u type=%u p1_cpu=%u p2_cpu=%u charsel_flag=%u",
        result,
        GetGameMode(),
        GetSubstate(),
        GetGameType(),
        ReadU8(ADDR_P1_CPU_FLAG, 0),
        ReadU8(ADDR_P2_CPU_FLAG, 0),
        ReadU32(ADDR_CHARSEL_MODE_FLAG, 0));
    return true;
}

static bool LaunchNetplayCharSel() {
    const bool isHost = SessionManager::IsHost();
    char endpoint[48];
    NetplayConfig::FormatEndpoint(&s_config, endpoint, sizeof(endpoint));
    LOG_NETPLAY(LOG_INFO,
        "[Menu] Launching NETPLAY CharSel (target=%s, nick='%s', role=%s)",
        endpoint,
        s_config.nickname,
        isHost ? "Host" : "Client");

    CommitConfig("netplay charsel launch");

    // Do NOT call HideMenuForExternalModeLaunch — that resets SessionManager.
    // We keep the session alive; only hide the menu UI.
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    ClearTextEditState();
    s_waitForNeutral = false;
    s_selectedIndex = 0;
    ClearError();
    TransitionTo(NetplayMenuState::Inactive, "netplay charsel launch");
    InputSystem_ResetRepeatState(0);

    // Use OFFLINE local VS mode (GAMETYPE_VS_HUMAN = 2) for CharSel.
    // The game reads both P1 and P2 inputs from local buffers — no vanilla
    // netplay sync chain is triggered. The mod relays the remote player's
    // raw input each frame via Hook_InputDispatcher and later intercepts
    // the CharSel -> StageSel transition to force Match directly.
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ClearVanillaNetplayFlags();
    WriteU8(ADDR_P1_CPU_FLAG, 0);
    WriteU8(ADDR_P2_CPU_FLAG, 0);

    // Force character selection enabled to prevent desync — both sides must
    // have this ON so BYTE2(dword_8E93B8) gates the same AI behavior paths.
    WriteU8(ADDR_CHARSEL_ENABLE, 1);

    // Force stage selection enabled to prevent desync — if one side has it OFF
    // (e.g. player toggled it via Options menu), the charsel substate flow and
    // MODE_STAGESEL cinematic would differ, causing divergent game state.
    WriteU8(ADDR_STAGESEL_ENABLE, 1);

    const int result = s_origSetGameMode ? s_origSetGameMode(MODE_CHARSEL, 1) : ((SetGameMode_t)ADDR_SET_GAME_MODE)(MODE_CHARSEL, 1);

    ResetCharSelFields();
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);

    // Transition SessionManager to CharSel state so that:
    // 1. CharSelSync::Begin() activates mod-owned charsel input relay
    // 2. Hook_MatchSyncInit suppresses vanilla blocking sync at Mode 8 entry
    // 3. IsModOwnedSync() covers the CharSel -> Match loading gap
    SessionManager::EnterCharSel();

    LOG_NETPLAY(LOG_INFO,
        "[Menu] Netplay CharSel launch: result=%d mode=%u sub=%u type=%u mod_role=%s mod_connected=%u p1_cpu=%u p2_cpu=%u charsel_flag=%u",
        result,
        GetGameMode(),
        GetSubstate(),
        GetGameType(),
        isHost ? "Host" : "Client",
        NetplayHooks::IsConnected() ? 1 : 0,
        ReadU8(ADDR_P1_CPU_FLAG, 0),
        ReadU8(ADDR_P2_CPU_FLAG, 0),
        ReadU32(ADDR_CHARSEL_MODE_FLAG, 0));
    return true;
}

static void SyncMockSessionState() {
    SessionManager::FrameUpdate();

    // Check if the host told us (joiner) to enter CharSel
    if (SessionManager::ConsumePendingCharSelStart()) {
        // If the game already auto-transitioned to CharSel (e.g. returning from
        // win screen) and our netplay_core auto-detected it, CharSelSync is
        // already running. Don't re-launch (SetGameMode would reset the screen).
        if (GetGameMode() == MODE_CHARSEL && CharSelSync::IsActive()) {
            LOG_NETPLAY(LOG_INFO, "[Menu] Joiner received CharSel start signal — already in CharSel with sync active, ignoring");
        } else {
            LOG_NETPLAY(LOG_INFO, "[Menu] Joiner received CharSel start signal from host — auto-launching CharSel");
            LaunchNetplayCharSel();
        }
        return;
    }

    SessionManager::Snapshot snapshot{};
    if (!SessionManager::GetSnapshot(&snapshot)) return;
    if (snapshot.revision == s_lastMockRevision) return;
    s_lastMockRevision = snapshot.revision;

    if (snapshot.status[0]) {
        SetStatus("%s", snapshot.status);
    }

    switch (snapshot.state) {
        case SessionManager::State::Idle:
            break;
        case SessionManager::State::Connecting:
            s_selectedIndex = 0;
            TransitionTo(NetplayMenuState::Connecting, "mock session connecting");
            break;
        case SessionManager::State::Handshake:
            s_selectedIndex = 0;
            TransitionTo(NetplayMenuState::Handshake, "mock session handshake");
            break;
        case SessionManager::State::Connected:
            if (snapshot.mode == SessionManager::Mode::Join) {
                NetplayConfig::RecordRecentPeer(&s_config, snapshot.peer_nickname, NetplayHooks::GetBuildSignature());
                CommitConfig("mock session connected");
            }
            s_selectedIndex = 0;
            s_activeRootBranch = NetplayRootBranch::DirectPlay;
            TransitionTo(NetplayMenuState::ConnectedSession, "mock session ready");
            break;
        case SessionManager::State::CharSel:
            s_selectedIndex = 0;
            TransitionTo(NetplayMenuState::CharSelTransition, "mock charsel handoff");
            break;
        case SessionManager::State::Error: {
            char reason[128];
            CopyText(reason, sizeof(reason), snapshot.last_error[0] ? snapshot.last_error : snapshot.status);
            SessionManager::Reset();
            s_lastMockRevision = 0;
            OpenDisconnectError(reason);
            break;
        }
        default:
            break;
    }
}

static void EnterCustomMenuContext() {
    const uint32_t currentMode = GetGameMode();
    if (currentMode != MODE_MENU) {
        // Use the real SetGameMode to properly transition — this calls
        // Handle_ReleaseAll() and sets sub=0, so the game's mode handler
        // reloads menu background sprites on the next frame.
        // fade=0 avoids stopping BGM before the menu restarts it at sub 0.
        LOG_NETPLAY(LOG_INFO, "[Menu] EnterCustomMenuContext: using SetGameMode from mode %u", currentMode);
        if (s_origSetGameMode) {
            s_origSetGameMode(MODE_MENU, 0);
        } else {
            ((SetGameMode_t)ADDR_SET_GAME_MODE)(MODE_MENU, 0);
        }
        // Sub=0 hasn't run yet — assets not loaded. Set pending flag so
        // FrameUpdate() can finish setup once the game processes sub 0.
        s_pendingMenuRestore = true;
    } else {
        // Already in MODE_MENU — sprites are loaded, just set sub=3 directly.
        WriteU32(ADDR_SUB_STATE, 3);
        WriteU32(ADDR_SUB_STATE_TIMER, 0);
        WriteU32(ADDR_FADE_TIMER, 0);
        s_pendingMenuRestore = false;
    }
    WriteU32(ADDR_GAME_TYPE, GetSafeMenuGameType());
    WriteU8(ADDR_TITLE_MENU_SELECTION, 3);
    ClearVanillaNetplayFlags();
    WriteU8(ADDR_P1_CPU_FLAG, 0);
    WriteU8(ADDR_P2_CPU_FLAG, 0);
    WriteU32(ADDR_CHARSEL_MODE_FLAG, 0);
    WriteU32(ADDR_CHARSEL_STATE, 0);
    WriteU16(ADDR_CHARSEL_P1_DATA, 0);
    WriteU16(ADDR_CHARSEL_P2_DATA, 0);
    WriteU16(ADDR_CHARSEL_P1_COLOR, 0);
    WriteU8(ADDR_CHARSEL_P1_COLOR_B, 0);
    WriteU16(ADDR_CHARSEL_P2_COLOR, 0);
    WriteU8(ADDR_CHARSEL_P2_COLOR_B, 0);
    WriteU8(ADDR_CHARSEL_P1_CURSOR, 0);
    WriteU8(ADDR_CHARSEL_P1_CONFIRM, 0);
    WriteU8(ADDR_CHARSEL_P1_AGE, 0);
    WriteU8(ADDR_CHARSEL_P2_CURSOR, 2);
    WriteU8(ADDR_CHARSEL_P2_CONFIRM, 0);
    WriteU8(ADDR_CHARSEL_P2_AGE, 0);
    WriteU8(ADDR_CHARSEL_STAGE_ID, 0);
}

static void RestoreMainMenuContext() {
    WriteU32(ADDR_GAME_MODE, MODE_MENU);
    WriteU32(ADDR_SUB_STATE, 3);
    WriteU32(ADDR_SUB_STATE_TIMER, 0);
    WriteU32(ADDR_FADE_TIMER, 0);
    WriteU32(ADDR_GAME_TYPE, GetSafeMenuGameType());
    WriteU8(ADDR_TITLE_MENU_SELECTION, 3);
    ClearVanillaNetplayFlags();
}

static void ResetMenuInputState() {
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    ClearTextEditState();
    InputSystem_ResetRepeatState(0);
}

static void OpenMenu() {
    EnterCustomMenuContext();
    LoadCommittedConfig();
    CommitConfig("menu open");
    SessionManager::Reset();
    s_activeRootBranch = NetplayRootBranch::DirectPlay;
    s_activeSettingsCategory = SettingsCategory::Identity;
    s_phase = MenuPhase::Opening;
    s_fadeFrames = 0;
    s_captureInput = true;
    s_lastMockRevision = 0;
    s_pendingMenuRestore = false;
    ResetMenuInputState();
    ClearError();
    SetStatus("Opening custom netplay menu.");
    LOG_NETPLAY(LOG_INFO, "[Menu] Opening custom Mode 3 netplay branch (%d-frame fade)", kFadeFrames);
    TransitionTo(NetplayMenuState::MenuRoot, "Network selected from main menu");
}

static void FinishClose() {
    SessionManager::Reset();
    s_phase = MenuPhase::Hidden;
    s_fadeFrames = 0;
    s_captureInput = false;
    s_pendingMenuRestore = false;
    ClearTextEditState();
    s_waitForNeutral = true;
    s_selectedIndex = 0;
    s_recentPeerViewIndex = 0;
    s_lastMockRevision = 0;
    SetStatus("Waiting for network menu selection.");
    TransitionTo(NetplayMenuState::Inactive, "menu closed");
    LOG_NETPLAY(LOG_INFO, "[Menu] Custom netplay menu closed; control returned to the main menu");
    InputSystem_ResetRepeatState(0);
}

static void BeginClose(const char* why) {
    if (!MenuVisible() || s_phase == MenuPhase::Closing) return;
    LOG_NETPLAY(LOG_INFO, "[Menu] Closing custom menu (%s)", why ? why : "no reason");
    SetStatus("Closing custom netplay menu.");
    s_phase = MenuPhase::Closing;
    s_waitForNeutral = true;
    ClearTextEditState();
}

static void OpenDisconnectError(const char* why) {
    uint32_t currentMode = GetGameMode();
    uint32_t currentType = GetGameType();

    LOG_NETPLAY(LOG_WARNING, "[Menu] OpenDisconnectError: reason='%s' mode=%u type=%u menuVisible=%d",
        why ? why : "(null)", currentMode, currentType, MenuVisible() ? 1 : 0);

    // Clean up all mod sync systems unconditionally
    if (CharSelSync::IsActive()) {
        LOG_NETPLAY(LOG_INFO, "[Menu] Ending CharSelSync during disconnect");
        CharSelSync::End();
    }
    if (MatchBootstrap::IsActive()) {
        LOG_NETPLAY(LOG_INFO, "[Menu] Aborting MatchBootstrap during disconnect");
        MatchBootstrap::Abort("Disconnect");
    }
    if (InputSyncHooks::IsLoadBarrierFrozen()) {
        InputSyncHooks::SetLoadBarrierFreeze(false);
    }
    if (InputSyncHooks::IsRollbackActive()) {
        InputSyncHooks::SetRollbackActive(false);
    }

    // Clean up netplay flags — game should NOT think it's in netplay
    WriteU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    ClearVanillaNetplayFlags();

    // If not in menu mode, force the game back to our custom menu
    if (currentMode != MODE_MENU) {
        LOG_NETPLAY(LOG_WARNING, "[Menu] Forcing return to custom menu from mode %u", currentMode);
        EnterCustomMenuContext();
    }

    SessionManager::Reset();
    s_lastMockRevision = 0;
    ClearTextEditState();
    SetError("%s", why ? why : "Disconnected.");
    SetStatus("%s", why ? why : "Disconnected.");
    s_phase = MenuPhase::Active;
    s_fadeFrames = kFadeFrames;
    s_captureInput = true;
    s_selectedIndex = 0;
    s_waitForNeutral = true;
    TransitionTo(NetplayMenuState::DisconnectError, "disconnect");
}

static int ItemCount(NetplayMenuState st) {
    switch (st) {
        case NetplayMenuState::MenuRoot: return 3;
        case NetplayMenuState::DirectConnectEntry: return 3;  // Host, Join, Back
        case NetplayMenuState::HostEntry: return 3;            // Host, Listen Port, Back
        case NetplayMenuState::JoinEntry: return 4;            // Join, Remote Endpoint, Recent Peers, Back
        case NetplayMenuState::SettingsCategoryMenu: return 6;  // kept for compatibility
        case NetplayMenuState::SettingsEntry: return 5;         // flat: nick, delay, hud, verbose, back
        case NetplayMenuState::Connecting: return 1;
        case NetplayMenuState::Handshake: return 1;
        case NetplayMenuState::ConnectedSession: return 2;
        case NetplayMenuState::CharSelTransition: return 2;
        case NetplayMenuState::PostMatch: return 3;
        case NetplayMenuState::DisconnectError: return 2;
        default: return 0;
    }
}

static void MoveSelection(int delta) {
    int count = ItemCount(s_state);
    if (count <= 0) { s_selectedIndex = 0; return; }
    int next = (int)s_selectedIndex + delta;
    while (next < 0) next += count;
    while (next >= count) next -= count;
    s_selectedIndex = (uint32_t)next;
}

static void ToggleBool(bool* value) {
    if (!value) return;
    *value = !*value;
}

static bool MenuJustPressed(uint16_t button) { return InputSystem_JustPressed(0, button); }
static bool ConfirmPressed() { return MenuJustPressed(INPUT_A) || MenuJustPressed(INPUT_START); }
static bool BackPressed() { return MenuJustPressed(INPUT_B) || MenuJustPressed(INPUT_SELECT); }

static char __cdecl Hook_MainMenuStateMachine() {
    // When returning to MODE_MENU from another mode (e.g. gameplay disconnect),
    // we must let the vanilla handler run for sub=0 to load background sprites
    // before our custom menu takes over at sub=3.
    if (s_pendingMenuRestore) {
        const uint32_t sub = GetSubstate();
        if (sub < 1) {
            // Sub 0 hasn't processed yet — let vanilla handler run to load assets
            const char result = s_origMainMenuStateMachine ? s_origMainMenuStateMachine() : 0;
            // After vanilla processes sub 0, it advances to sub 1; check again
            if (GetSubstate() >= 1) {
                LOG_NETPLAY(LOG_INFO, "[Menu] Pending menu restore: vanilla loaded assets (sub=%u), forcing sub=3", GetSubstate());
                WriteU32(ADDR_SUB_STATE, 3);
                WriteU32(ADDR_SUB_STATE_TIMER, 0);
                WriteU32(ADDR_FADE_TIMER, 0);
                s_pendingMenuRestore = false;
            }
            return (char)ReadU32(ADDR_SUB_STATE, sub);
        } else {
            // Assets already loaded (sub >= 1), skip to sub 3
            LOG_NETPLAY(LOG_INFO, "[Menu] Pending menu restore: assets ready (sub=%u), forcing sub=3", sub);
            WriteU32(ADDR_SUB_STATE, 3);
            WriteU32(ADDR_SUB_STATE_TIMER, 0);
            WriteU32(ADDR_FADE_TIMER, 0);
            s_pendingMenuRestore = false;
        }
    }

    const bool wasVisible = MenuVisible();
    if (!wasVisible) {
        const uint32_t mode = GetGameMode();
        const uint32_t sub = GetSubstate();
        const uint8_t titleSelection = ReadU8(ADDR_TITLE_MENU_SELECTION, 0xFF);
        if (s_interceptEnabled &&
            mode == MODE_MENU &&
            sub == 3 &&
            titleSelection == 3 &&
            ConfirmPressed()) {
            LOG_NETPLAY(LOG_INFO, "[Menu] Short-circuiting vanilla option 4 from the main menu into the custom menu");
            OpenMenu();
            EnterCustomMenuContext();
            SanitizeOwnedGameType(MODE_MENU, false, "main-menu-short-circuit");
            RenderInGameMenu();
            return (char)ReadU32(ADDR_SUB_STATE, 3);
        }

        const char result = s_origMainMenuStateMachine ? s_origMainMenuStateMachine() : 0;
        if (MenuVisible()) {
            // The vanilla option-4 handler keeps mutating netplay state after our
            // SetGameMode intercept fires. Re-sanitize immediately in the same frame.
            EnterCustomMenuContext();
            SanitizeOwnedGameType(MODE_MENU, false, "main-menu-open");
            RenderInGameMenu();
            return (char)ReadU32(ADDR_SUB_STATE, 3);
        }
        return result;
    }

    EnterCustomMenuContext();
    SanitizeOwnedGameType(MODE_MENU, false, "main-menu-visible");
    RenderInGameMenu();
    return (char)ReadU32(ADDR_SUB_STATE, 3);
}

static int __cdecl Hook_SetGameMode(int mode, char fade) {
    uint32_t sourceMode = GetGameMode();
    SessionManager::Snapshot snap{};
    const bool hasSession = SessionManager::GetSnapshot(&snap) && snap.active && !snap.has_error;

    // Intercept ALL transitions to MODE_LOBBY when enabled.
    // From MODE_MENU: normal "Network" menu selection → open our custom menu.
    // From any other mode (CharSel, Match): vanilla disconnect fallback → redirect.
    if (s_interceptEnabled && mode == MODE_LOBBY) {
        if (sourceMode == MODE_MENU) {
            // Normal path: user selected "Network" from main menu
            s_lastInterceptSourceMode = sourceMode;
            s_lastInterceptFade = (fade != 0);
            ++s_interceptCount;
            LOG_NETPLAY(LOG_INFO, "[Menu] Intercepted MODE_MENU -> MODE_LOBBY (fade=%d, stableType=%u)", fade ? 1 : 0, s_lastStableGameType);
            OpenMenu();
            return 0;
        } else {
            // Vanilla disconnect fallback — game trying to go to lobby from CharSel/Match
            LOG_NETPLAY(LOG_WARNING, "[Menu] Intercepted mode %u -> MODE_LOBBY (vanilla fallback!) — redirecting to custom menu", sourceMode);
            OpenDisconnectError("Connection lost (vanilla lobby redirect intercepted)");
            return 0;
        }
    }

    // Graceful quit from CharSel: player selected "Quit" from the cancel
    // dialog. Both sides transition through CONFIRM → BACK_MENU deterministically
    // (synced via lockstep). Intercept the MODE_MENU transition and clean up
    // the mod session before letting the game return to the main menu.
    if (s_interceptEnabled && mode == MODE_MENU && sourceMode == MODE_CHARSEL && hasSession) {
        LOG_NETPLAY(LOG_INFO, "[Menu] Intercepted CharSel -> MODE_MENU (player quit) — disconnecting session");
        OpenDisconnectError("Player quit from character select.");
        return 0;
    }

    // MODE_STAGESEL (7) is a cinematic-only mode that plays the stage reveal
    // animation, then transitions to MODE_MATCH (8). Let it pass through —
    // CharSelSync::PollGameState detects this transition and locks config there.
    // (Previously this was redirected directly to MODE_MATCH, skipping the cinematic.)

    // Log other mode transitions for debugging
    LOG_NETPLAY(LOG_DEBUG, "[Menu] SetGameMode(%u, fade=%d) from mode=%u — passthrough", mode, fade ? 1 : 0, sourceMode);
    return s_origSetGameMode ? s_origSetGameMode(mode, fade) : 0;
}

static int __cdecl Hook_NetInitHost(unsigned short listenPortHostOrder) {
    if (ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING,
            "[Menu] Blocked vanilla host socket init on port %u while mod-owned networking is active",
            (unsigned)listenPortHostOrder);
        return -1;
    }
    return s_origNetInitHost ? s_origNetInitHost(listenPortHostOrder) : -1;
}

static int __cdecl Hook_NetInitClient(unsigned short port, char *ipAddress) {
    if (ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING,
            "[Menu] Blocked vanilla client socket init to %s:%u while mod-owned networking is active",
            ipAddress ? ipAddress : "(null)",
            (unsigned)port);
        return -1;
    }
    return s_origNetInitClient ? s_origNetInitClient(port, ipAddress) : -1;
}

static int __cdecl Hook_NetCloseHost() {
    if (ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING, "[Menu] Blocked vanilla host socket close while mod-owned networking is active");
        return 0;
    }
    return s_origNetCloseHost ? s_origNetCloseHost() : -1;
}

static int __cdecl Hook_NetCloseClient() {
    if (ShouldBlockVanillaSocketLifecycle()) {
        LOG_NETPLAY(LOG_WARNING, "[Menu] Blocked vanilla client socket close while mod-owned networking is active");
        return 0;
    }
    return s_origNetCloseClient ? s_origNetCloseClient() : -1;
}

} // namespace

bool Initialize() {
    if (s_initialized) return s_hookInstalled;
    s_initialized = true;
    s_lastObservedMode = GetGameMode();
    s_lastObservedType = GetGameType();
    s_lastStableGameType = (s_lastObservedType == GAMETYPE_NETPLAY) ? GAMETYPE_VS_HUMAN : s_lastObservedType;
    MH_STATUS status = MH_CreateHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE, (LPVOID)&Hook_MainMenuStateMachine, (LPVOID*)&s_origMainMenuStateMachine);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to create main menu hook: %d", status);
        SetStatus("Failed to install in-game menu hook.");
        return false;
    }
    status = MH_EnableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to enable main menu hook: %d", status);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to enable in-game menu hook.");
        return false;
    }

    status = MH_CreateHook((LPVOID)ADDR_SET_GAME_MODE, (LPVOID)&Hook_SetGameMode, (LPVOID*)&s_origSetGameMode);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to create SetGameMode hook: %d", status);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to install in-game netplay hook.");
        return false;
    }
    status = MH_EnableHook((LPVOID)ADDR_SET_GAME_MODE);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to enable SetGameMode hook: %d", status);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to enable in-game netplay hook.");
        return false;
    }

    status = MH_CreateHook((LPVOID)ADDR_NET_INIT_HOST, (LPVOID)&Hook_NetInitHost, (LPVOID*)&s_origNetInitHost);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to create host socket init hook: %d", status);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to install vanilla socket ownership hook.");
        return false;
    }
    status = MH_EnableHook((LPVOID)ADDR_NET_INIT_HOST);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to enable host socket init hook: %d", status);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to enable vanilla socket ownership hook.");
        return false;
    }

    status = MH_CreateHook((LPVOID)ADDR_NET_INIT_CLIENT, (LPVOID)&Hook_NetInitClient, (LPVOID*)&s_origNetInitClient);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to create client socket init hook: %d", status);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to install vanilla socket ownership hook.");
        return false;
    }
    status = MH_EnableHook((LPVOID)ADDR_NET_INIT_CLIENT);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to enable client socket init hook: %d", status);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to enable vanilla socket ownership hook.");
        return false;
    }

    status = MH_CreateHook((LPVOID)ADDR_NET_CLOSE_HOST, (LPVOID)&Hook_NetCloseHost, (LPVOID*)&s_origNetCloseHost);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to create host socket close hook: %d", status);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to install vanilla socket ownership hook.");
        return false;
    }
    status = MH_EnableHook((LPVOID)ADDR_NET_CLOSE_HOST);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to enable host socket close hook: %d", status);
        MH_RemoveHook((LPVOID)ADDR_NET_CLOSE_HOST);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to enable vanilla socket ownership hook.");
        return false;
    }

    status = MH_CreateHook((LPVOID)ADDR_NET_CLOSE_CLIENT, (LPVOID)&Hook_NetCloseClient, (LPVOID*)&s_origNetCloseClient);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to create client socket close hook: %d", status);
        MH_DisableHook((LPVOID)ADDR_NET_CLOSE_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_CLOSE_HOST);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to install vanilla socket ownership hook.");
        return false;
    }
    status = MH_EnableHook((LPVOID)ADDR_NET_CLOSE_CLIENT);
    if (status != MH_OK) {
        LOG_NETPLAY(LOG_ERROR, "[Menu] Failed to enable client socket close hook: %d", status);
        MH_RemoveHook((LPVOID)ADDR_NET_CLOSE_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_CLOSE_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_CLOSE_HOST);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        SetStatus("Failed to enable vanilla socket ownership hook.");
        return false;
    }
    s_hookInstalled = true;
    LOG_NETPLAY(LOG_INFO,
        "[Menu] Installed menu/net ownership hooks at 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X",
        (unsigned)ADDR_MAIN_MENU_STATE_MACHINE,
        (unsigned)ADDR_SET_GAME_MODE,
        (unsigned)ADDR_NET_INIT_HOST,
        (unsigned)ADDR_NET_INIT_CLIENT,
        (unsigned)ADDR_NET_CLOSE_HOST,
        (unsigned)ADDR_NET_CLOSE_CLIENT);
    return true;
}

void Shutdown() {
    if (!s_initialized) return;
    if (s_hookInstalled) {
        MH_DisableHook((LPVOID)ADDR_NET_CLOSE_CLIENT);
        MH_RemoveHook((LPVOID)ADDR_NET_CLOSE_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_CLOSE_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_CLOSE_HOST);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_CLIENT);
        MH_DisableHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_RemoveHook((LPVOID)ADDR_NET_INIT_HOST);
        MH_DisableHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_RemoveHook((LPVOID)ADDR_MAIN_MENU_STATE_MACHINE);
        MH_DisableHook((LPVOID)ADDR_SET_GAME_MODE);
        MH_RemoveHook((LPVOID)ADDR_SET_GAME_MODE);
    }
    s_hookInstalled = false;
    s_initialized = false;
    FinishClose();
}

static void AdjustSelectedValue(int delta) {
    bool changed = false;
    if (s_state == NetplayMenuState::JoinEntry) {
        if (s_selectedIndex == 2) {
            BrowseRecentPeer(delta);
        }
    } else if (s_state == NetplayMenuState::SettingsEntry) {
        // Flat settings: 0=Nickname, 1=Delay, 2=HUD, 3=Verbose, 4=Back
        if (s_selectedIndex == 1) {
            s_config.preferred_delay_frames = ClampInt(s_config.preferred_delay_frames + delta, 0, 6);
            changed = true;
        } else if (s_selectedIndex == 2) {
            ToggleBool(&s_config.show_netplay_hud);
            changed = true;
        } else if (s_selectedIndex == 3) {
            ToggleBool(&s_config.verbose_logging);
            changed = true;
        }
    }

    if (changed) {
        CommitConfig("menu setting changed");
    }
}

static void FinishTextEdit(bool commit) {
    const TextEditField field = s_textEditField;
    if (field == TextEditField::None) return;

    if (!commit) {
        if (field == TextEditField::Nickname) {
            SetStatus("Nickname edit cancelled.");
        } else if (field == TextEditField::RemoteEndpoint) {
            SetStatus("Endpoint edit cancelled.");
        } else if (field == TextEditField::ListenPort) {
            SetStatus("Port edit cancelled.");
        }
        ClearTextEditState();
        s_waitForNeutral = true;
        InputSystem_ResetRepeatState(0);
        return;
    }

    if (field == TextEditField::Nickname) {
        CopyText(s_config.nickname, sizeof(s_config.nickname), s_textEditBuffer);
        CommitConfig("nickname edit finished");
        SetStatus("Nickname updated to '%s'.", s_config.nickname);
    } else if (field == TextEditField::RemoteEndpoint) {
        if (!NetplayConfig::ApplyEndpointString(&s_config, s_textEditBuffer)) {
            SetStatus("Invalid endpoint. Use ip:port, for example 127.0.0.1:10700.");
            return;
        }

        CommitConfig("endpoint edit finished");
        char endpoint[48];
        NetplayConfig::FormatEndpoint(&s_config, endpoint, sizeof(endpoint));
        SetStatus("Remote endpoint updated to %s.", endpoint);
    } else if (field == TextEditField::ListenPort) {
        int port = atoi(s_textEditBuffer);
        if (port < 1 || port > 65535) {
            SetStatus("Invalid port. Enter a number between 1 and 65535.");
            return;
        }
        s_config.listen_port = (uint16_t)port;
        CommitConfig("listen port edit finished");
        SetStatus("Listen port updated to %u.", s_config.listen_port);
    }

    ClearTextEditState();
    s_waitForNeutral = true;
    InputSystem_ResetRepeatState(0);
}

static void HandleTextEditing() {
    bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    static bool prevDown[256] = {};

    // Handle Ctrl+V paste before the per-key loop
    {
        bool vDown = (GetAsyncKeyState('V') & 0x8000) != 0;
        if (ctrlDown && vDown && !prevDown['V']) {
            char pasteBuffer[64] = {};
            if (PasteFromClipboard(pasteBuffer, sizeof(pasteBuffer))) {
                size_t pLen = strlen(pasteBuffer);
                size_t curLen = strlen(s_textEditBuffer);
                size_t maxLen = sizeof(s_textEditBuffer) - 1;
                size_t space = maxLen - curLen;
                if (pLen > space) pLen = space;
                if (pLen > 0) {
                    memcpy(s_textEditBuffer + curLen, pasteBuffer, pLen);
                    s_textEditBuffer[curLen + pLen] = '\0';
                }
            }
            prevDown['V'] = true;
            return; // consume this frame
        }
    }

    for (int vk = 0; vk < 256; ++vk) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !prevDown[vk]) {
            size_t len = strlen(s_textEditBuffer);
            if (vk == VK_RETURN) {
                FinishTextEdit(true);
            } else if (vk == VK_ESCAPE) {
                FinishTextEdit(false);
            } else if ((vk == VK_BACK || vk == VK_DELETE) && len > 0) {
                s_textEditBuffer[len - 1] = '\0';
            } else if (s_textEditField == TextEditField::Nickname) {
                if (vk == VK_SPACE && len + 1 < sizeof(s_config.nickname)) {
                    s_textEditBuffer[len] = ' ';
                    s_textEditBuffer[len + 1] = '\0';
                } else if (vk >= '0' && vk <= '9' && len + 1 < sizeof(s_config.nickname)) {
                    s_textEditBuffer[len] = (char)vk;
                    s_textEditBuffer[len + 1] = '\0';
                } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 && len + 1 < sizeof(s_config.nickname)) {
                    s_textEditBuffer[len] = (char)('0' + (vk - VK_NUMPAD0));
                    s_textEditBuffer[len + 1] = '\0';
                } else if (vk >= 'A' && vk <= 'Z' && len + 1 < sizeof(s_config.nickname)) {
                    s_textEditBuffer[len] = (char)(shiftDown ? vk : (vk + 32));
                    s_textEditBuffer[len + 1] = '\0';
                }
            } else if (s_textEditField == TextEditField::RemoteEndpoint) {
                if (vk >= '0' && vk <= '9' && len + 1 < sizeof(s_textEditBuffer)) {
                    s_textEditBuffer[len] = (char)vk;
                    s_textEditBuffer[len + 1] = '\0';
                } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 && len + 1 < sizeof(s_textEditBuffer)) {
                    s_textEditBuffer[len] = (char)('0' + (vk - VK_NUMPAD0));
                    s_textEditBuffer[len + 1] = '\0';
                } else if ((vk == VK_OEM_PERIOD || vk == VK_DECIMAL) && len + 1 < sizeof(s_textEditBuffer)) {
                    s_textEditBuffer[len] = '.';
                    s_textEditBuffer[len + 1] = '\0';
                } else if ((vk == VK_OEM_1 || (shiftDown && vk == VK_OEM_1)) && len + 1 < sizeof(s_textEditBuffer)) {
                    s_textEditBuffer[len] = ':';
                    s_textEditBuffer[len + 1] = '\0';
                }
            } else if (s_textEditField == TextEditField::ListenPort) {
                if (vk >= '0' && vk <= '9' && len + 1 < sizeof(s_textEditBuffer)) {
                    s_textEditBuffer[len] = (char)vk;
                    s_textEditBuffer[len + 1] = '\0';
                } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 && len + 1 < sizeof(s_textEditBuffer)) {
                    s_textEditBuffer[len] = (char)('0' + (vk - VK_NUMPAD0));
                    s_textEditBuffer[len + 1] = '\0';
                }
            }
        }
        prevDown[vk] = down;
    }
}

static void ActivateCurrentSelection() {
    switch (s_state) {
        case NetplayMenuState::MenuRoot:
            if (s_selectedIndex == 0) {
                s_activeRootBranch = NetplayRootBranch::DirectPlay;
                s_selectedIndex = 0;
                char endpoint[48];
                NetplayConfig::FormatEndpoint(&s_config, endpoint, sizeof(endpoint));
                SetStatus("Direct Play ready. Target %s", endpoint);
                TransitionTo(NetplayMenuState::DirectConnectEntry, "open direct connect");
            } else if (s_selectedIndex == 1) {
                s_activeRootBranch = NetplayRootBranch::Settings;
                s_selectedIndex = 0;
                SetStatus("Settings opened.");
                TransitionTo(NetplayMenuState::SettingsEntry, "open settings");
            } else {
                BeginClose("back from root");
            }
            break;
        case NetplayMenuState::DirectConnectEntry:
            if (s_selectedIndex == 0) {
                // Host sub-menu
                s_selectedIndex = 0;
                SetStatus("Host: configure port and start hosting.");
                TransitionTo(NetplayMenuState::HostEntry, "open host entry");
            } else if (s_selectedIndex == 1) {
                // Join sub-menu
                s_selectedIndex = 0;
                char endpoint[48];
                NetplayConfig::FormatEndpoint(&s_config, endpoint, sizeof(endpoint));
                SetStatus("Join: target %s", endpoint);
                TransitionTo(NetplayMenuState::JoinEntry, "open join entry");
            } else {
                s_selectedIndex = 0;
                SetStatus("Back to root.");
                TransitionTo(NetplayMenuState::MenuRoot, "back from direct connect");
            }
            break;
        case NetplayMenuState::HostEntry:
            if (s_selectedIndex == 0) {
                CommitConfig("direct play host");
                if (SessionManager::StartHost(&s_config)) {
                    SessionManager::Snapshot snapshot{};
                    SessionManager::GetSnapshot(&snapshot);
                    s_lastMockRevision = snapshot.revision;
                    s_selectedIndex = 0;
                    SetStatus("%s", snapshot.status);
                    TransitionTo(NetplayMenuState::Connecting, "host selected");
                } else {
                    SessionManager::Snapshot snapshot{};
                    SessionManager::GetSnapshot(&snapshot);
                    OpenDisconnectError(snapshot.last_error[0] ? snapshot.last_error : "Host failed.");
                }
            } else if (s_selectedIndex == 1) {
                char portStr[16];
                snprintf(portStr, sizeof(portStr), "%u", s_config.listen_port);
                BeginTextEdit(TextEditField::ListenPort, portStr, "Type port number (1-65535). Enter to save.");
            } else {
                s_selectedIndex = 0;
                SetStatus("Back to Direct Play.");
                TransitionTo(NetplayMenuState::DirectConnectEntry, "back from host entry");
            }
            break;
        case NetplayMenuState::JoinEntry:
            if (s_selectedIndex == 0) {
                CommitConfig("direct play join");
                if (SessionManager::StartJoin(&s_config)) {
                    SessionManager::Snapshot snapshot{};
                    SessionManager::GetSnapshot(&snapshot);
                    s_lastMockRevision = snapshot.revision;
                    s_selectedIndex = 0;
                    SetStatus("%s", snapshot.status);
                    TransitionTo(NetplayMenuState::Connecting, "join selected");
                } else {
                    SessionManager::Snapshot snapshot{};
                    SessionManager::GetSnapshot(&snapshot);
                    OpenDisconnectError(snapshot.last_error[0] ? snapshot.last_error : "Join failed.");
                }
            } else if (s_selectedIndex == 1) {
                char endpoint[48];
                NetplayConfig::FormatEndpoint(&s_config, endpoint, sizeof(endpoint));
                BeginTextEdit(TextEditField::RemoteEndpoint, endpoint, "Editing remote endpoint. Type ip:port, Enter to save.");
            } else if (s_selectedIndex == 2) {
                if (NetplayConfig::ApplyRecentPeer(&s_config, s_recentPeerViewIndex)) {
                    CommitConfig("join recent peer applied");
                    char recent[80];
                    GetRecentPeerSummary(recent, sizeof(recent));
                    SetStatus("Applied recent peer: %s", recent);
                } else {
                    SetStatus("No recent peer saved yet.");
                }
            } else {
                s_selectedIndex = 1;
                SetStatus("Back to Direct Play.");
                TransitionTo(NetplayMenuState::DirectConnectEntry, "back from join entry");
            }
            break;
        case NetplayMenuState::SettingsCategoryMenu:
            // Legacy state — redirect to flat settings
            s_selectedIndex = 0;
            TransitionTo(NetplayMenuState::SettingsEntry, "redirect to flat settings");
            break;
        case NetplayMenuState::SettingsEntry:
            // Flat settings: 0=Nickname, 1=Delay, 2=HUD, 3=Verbose, 4=Back
            if (s_selectedIndex == 0) {
                BeginTextEdit(TextEditField::Nickname, s_config.nickname, "Editing nickname. Type letters/numbers, Enter to save.");
            } else if (s_selectedIndex == 1) {
                // Delay adjusted via left/right, confirm toggles direction
                AdjustSelectedValue(1);
            } else if (s_selectedIndex == 2) {
                ToggleBool(&s_config.show_netplay_hud);
                CommitConfig("hud toggled");
            } else if (s_selectedIndex == 3) {
                ToggleBool(&s_config.verbose_logging);
                CommitConfig("verbose toggled");
            } else {
                s_activeRootBranch = NetplayRootBranch::DirectPlay;
                s_selectedIndex = 1;
                SetStatus("Back to root.");
                TransitionTo(NetplayMenuState::MenuRoot, "back from settings");
            }
            break;
        case NetplayMenuState::Connecting:
            SessionManager::Cancel();
            s_lastMockRevision = 0;
            s_selectedIndex = 0;
            SetStatus("Connection attempt cancelled.");
            TransitionTo(NetplayMenuState::DirectConnectEntry, "cancel connect");
            break;
        case NetplayMenuState::Handshake:
            SessionManager::Cancel();
            s_lastMockRevision = 0;
            s_selectedIndex = 0;
            SetStatus("Handshake cancelled.");
            TransitionTo(NetplayMenuState::DirectConnectEntry, "back from handshake");
            break;
        case NetplayMenuState::ConnectedSession:
            if (s_selectedIndex == 0) {
                // Don't call EnterCharSel here — that sends the Ready signal
                // to the joiner too early (before we actually enter Mode 6).
                // EnterCharSel is called inside LaunchNetplayCharSel instead.
                s_selectedIndex = 0;
                SetStatus("Ready to enter Character Select.");
                TransitionTo(NetplayMenuState::CharSelTransition, "prepare netplay charsel");
            } else {
                SessionManager::Disconnect("Disconnected from session.");
                SessionManager::Snapshot snapshot{};
                SessionManager::GetSnapshot(&snapshot);
                OpenDisconnectError(snapshot.last_error[0] ? snapshot.last_error : "Disconnected from session.");
            }
            break;
        case NetplayMenuState::CharSelTransition:
            if (s_selectedIndex == 0) {
                LaunchNetplayCharSel();
            } else {
                SessionManager::ReturnToSession();
                SessionManager::Snapshot snapshot{};
                SessionManager::GetSnapshot(&snapshot);
                s_lastMockRevision = snapshot.revision;
                s_selectedIndex = 0;
                SetStatus("Back to session.");
                TransitionTo(NetplayMenuState::ConnectedSession, "return from charsel staging");
            }
            break;
        case NetplayMenuState::PostMatch:
            if (s_selectedIndex == 0) {
                // Rematch → return to CharSel with same peer
                // Don't call EnterCharSel here — LaunchNetplayCharSel handles it
                LOG_INFO("[Menu] PostMatch → Rematch (return to CharSel)");
                s_selectedIndex = 0;
                SetStatus("Rematch — returning to Character Select.");
                TransitionTo(NetplayMenuState::CharSelTransition, "rematch from post-match");
            } else if (s_selectedIndex == 1) {
                // Return to session lobby
                LOG_INFO("[Menu] PostMatch → Return to Session");
                SessionManager::ReturnToSession();
                SessionManager::Snapshot snapshot{};
                SessionManager::GetSnapshot(&snapshot);
                s_lastMockRevision = snapshot.revision;
                s_selectedIndex = 0;
                SetStatus("Back to session.");
                TransitionTo(NetplayMenuState::ConnectedSession, "return to session from post-match");
            } else {
                // Disconnect
                LOG_INFO("[Menu] PostMatch → Disconnect");
                SessionManager::Disconnect("Left after match.");
                SessionManager::Snapshot snapshot{};
                SessionManager::GetSnapshot(&snapshot);
                OpenDisconnectError(snapshot.last_error[0] ? snapshot.last_error : "Left after match.");
            }
            break;
        case NetplayMenuState::DisconnectError:
            if (s_selectedIndex == 0) {
                ClearError();
                SessionManager::Reset();
                s_lastMockRevision = 0;
                s_selectedIndex = 0;
                SetStatus("Disconnect acknowledged. Back at root.");
                TransitionTo(NetplayMenuState::MenuRoot, "recover from disconnect");
            } else {
                BeginClose("close after disconnect");
            }
            break;
        default:
            break;
    }
}

static void HandleNavigationInput() {
    // C key: copy your address to clipboard (in states where it's relevant)
    {
        static bool s_prevCDown = false;
        bool cDown = (GetAsyncKeyState('C') & 0x8000) != 0;
        if (cDown && !s_prevCDown) {
            if (s_state == NetplayMenuState::DirectConnectEntry ||
                s_state == NetplayMenuState::HostEntry ||
                s_state == NetplayMenuState::JoinEntry ||
                s_state == NetplayMenuState::Connecting ||
                s_state == NetplayMenuState::Handshake ||
                s_state == NetplayMenuState::ConnectedSession) {
                UpdateYourAddress();
                if (CopyToClipboard(s_yourAddress)) {
                    FlashClipboardMessage("Copied!");
                }
            }
        }
        s_prevCDown = cDown;
    }

    if (MenuJustPressed(INPUT_UP)) MoveSelection(-1);
    if (MenuJustPressed(INPUT_DOWN)) MoveSelection(1);
    if (MenuJustPressed(INPUT_LEFT)) AdjustSelectedValue(-1);
    if (MenuJustPressed(INPUT_RIGHT)) AdjustSelectedValue(1);
    if (ConfirmPressed()) { ActivateCurrentSelection(); return; }
    if (!BackPressed()) return;
    switch (s_state) {
        case NetplayMenuState::MenuRoot:
            BeginClose("back from root");
            break;
        case NetplayMenuState::DirectConnectEntry:
            s_selectedIndex = 0;
            SetStatus("Back to root.");
            TransitionTo(NetplayMenuState::MenuRoot, "back from direct connect");
            break;
        case NetplayMenuState::HostEntry:
            s_selectedIndex = 0;
            SetStatus("Back to Direct Play.");
            TransitionTo(NetplayMenuState::DirectConnectEntry, "back from host entry");
            break;
        case NetplayMenuState::JoinEntry:
            s_selectedIndex = 1;
            SetStatus("Back to Direct Play.");
            TransitionTo(NetplayMenuState::DirectConnectEntry, "back from join entry");
            break;
        case NetplayMenuState::SettingsCategoryMenu:
            // Legacy — treat same as SettingsEntry back
            s_activeRootBranch = NetplayRootBranch::DirectPlay;
            s_selectedIndex = 1;
            SetStatus("Back to root.");
            TransitionTo(NetplayMenuState::MenuRoot, "back from settings");
            break;
        case NetplayMenuState::SettingsEntry:
            s_activeRootBranch = NetplayRootBranch::DirectPlay;
            s_selectedIndex = 1;
            SetStatus("Back to root.");
            TransitionTo(NetplayMenuState::MenuRoot, "back from settings");
            break;
        case NetplayMenuState::Connecting:
        case NetplayMenuState::Handshake:
            SessionManager::Cancel();
            s_lastMockRevision = 0;
            s_selectedIndex = 0;
            SetStatus("Back to direct connect.");
            TransitionTo(NetplayMenuState::DirectConnectEntry, "back from connection state");
            break;
        case NetplayMenuState::ConnectedSession:
            SessionManager::Disconnect("Session cancelled by user.");
            OpenDisconnectError("Session cancelled by user.");
            break;
        case NetplayMenuState::CharSelTransition:
            SessionManager::ReturnToSession();
            s_lastMockRevision = 0;
            s_selectedIndex = 0;
            SetStatus("Back to session.");
            TransitionTo(NetplayMenuState::ConnectedSession, "back from charsel staging");
            break;
        case NetplayMenuState::DisconnectError:
            BeginClose("back from disconnect");
            break;
        default:
            break;
    }
}

void FrameUpdate() {
    uint32_t mode = GetGameMode();
    uint32_t type = GetGameType();
    SessionManager::Snapshot snap{};
    bool sessionActive = SessionManager::GetSnapshot(&snap) && snap.active && !snap.has_error;

    if (mode != s_lastObservedMode || type != s_lastObservedType) {
        LOG_NETPLAY(LOG_DEBUG, "[Menu] Observed mode=%u type=%u (previous mode=%u type=%u)", mode, type, s_lastObservedMode, s_lastObservedType);
        s_lastObservedMode = mode;
        s_lastObservedType = type;
    }
    
    // ALWAYS pump the session manager — even when the menu is hidden (CharSel/Match).
    // This drives socket polling, ping/pong keepalive, CharSelSync, MatchBootstrap,
    // and timeout detection. Without this, the session dies during gameplay.
    SyncMockSessionState();
    
    // Re-read mode/type — SyncMockSessionState may have forced a mode change
    // (e.g., disconnect during CharSel/Match forces back to MODE_MENU).
    mode = GetGameMode();
    type = GetGameType();
    sessionActive = SessionManager::GetSnapshot(&snap) && snap.active && !snap.has_error;

    // The vanilla "Network" option and some downstream code paths still try to
    // restore GAMETYPE_NETPLAY. Keep mod-owned flows in the offline type we own.
    SanitizeOwnedGameType(mode, sessionActive, "frame-update");
    mode = GetGameMode();
    type = GetGameType();
    
    // Pending menu restore: after EnterCustomMenuContext() used SetGameMode()
    // to properly transition from a non-menu mode, the game needs one frame
    // to process sub=0 (loads background sprites). Once sub >= 1, assets are
    // loaded and we can skip to sub=3 (the main menu screen our overlay uses).
    if (s_pendingMenuRestore && mode == MODE_MENU) {
        const uint32_t sub = GetSubstate();
        if (sub >= 1) {
            LOG_NETPLAY(LOG_INFO, "[Menu] Pending menu restore complete: sub=%u -> forcing sub=3", sub);
            WriteU32(ADDR_SUB_STATE, 3);
            WriteU32(ADDR_SUB_STATE_TIMER, 0);
            WriteU32(ADDR_FADE_TIMER, 0);
            s_pendingMenuRestore = false;
        } else {
            // Sub 0 hasn't processed yet — wait one more frame
            return;
        }
    }

    if (!MenuVisible()) {
        if (type != GAMETYPE_NETPLAY) s_lastStableGameType = type;
        return;
    }
    if (mode != MODE_MENU) {
        LOG_NETPLAY(LOG_WARNING, "[Menu] Closing custom menu because mode changed to %u", mode);
        FinishClose();
        return;
    }
    if (s_phase == MenuPhase::Opening) {
        if (++s_fadeFrames >= kFadeFrames) { s_fadeFrames = kFadeFrames; s_phase = MenuPhase::Active; }
    } else if (s_phase == MenuPhase::Closing) {
        if (--s_fadeFrames <= 0) {
            RestoreMainMenuContext();
            FinishClose();
            return;
        }
    }
    // SyncMockSessionState() already called above (before MenuVisible check)
    if (s_waitForNeutral) {
        uint16_t held = InputSystem_GetInput(0);
        if ((held & (INPUT_ANY_DIR | INPUT_A | INPUT_B | INPUT_START | INPUT_SELECT)) == 0) {
            s_waitForNeutral = false;
            InputSystem_ResetRepeatState(0);
        }
        return;
    }
    if (s_phase != MenuPhase::Active) return;
    if (IsTextEditing()) HandleTextEditing();
    else HandleNavigationInput();
}

static void RenderMenuRow(int y, const char* label, const char* value, bool selected, bool enabled, uint8_t alpha) {
    char clippedLabel[64];
    char clippedValue[96];
    CopyDisplayText(clippedLabel, sizeof(clippedLabel), label ? label : "", kRowLabelChars);
    CopyDisplayText(clippedValue, sizeof(clippedValue), value ? value : "", kRowValueChars);

    if (selected) {
        GameSetBlendMode(1, (uint8_t)Alpha8((float)alpha / 255.0f, 196));
        GameFillRect(kRowLeft, y - 3, kRowRight, y + 18, enabled ? 140 : 70, enabled ? 28 : 44, enabled ? 28 : 58);
        GameFillRect(kRowLeft, y - 3, kRowLeft + 4, y + 18, 255, 225, 96);
    }

    GameSetBlendMode(1, alpha);
    GameDrawTextf(kLabelX, y, enabled ? 255 : 188, enabled ? 255 : 192, enabled ? 255 : 204, "%s", clippedLabel);
    if (clippedValue[0]) {
        GameDrawTextf(kValueX, y, enabled ? 196 : 160, enabled ? 220 : 166, enabled ? 255 : 180, "%s", clippedValue);
    }
}

static void RenderInfoLine(int y, const char* label, const char* value, uint8_t alpha) {
    char clippedLabel[64];
    char clippedValue[96];
    CopyDisplayText(clippedLabel, sizeof(clippedLabel), label ? label : "", kRowLabelChars);
    CopyDisplayText(clippedValue, sizeof(clippedValue), value ? value : "", kRowValueChars);

    GameSetBlendMode(1, alpha);
    GameDrawTextf(kLabelX, y, 180, 188, 202, "%s", clippedLabel);
    if (clippedValue[0]) {
        GameDrawTextf(kValueX, y, 230, 234, 242, "%s", clippedValue);
    }
}

static void GetFooterHints(const GekkoBridge::Snapshot* gekko, const SessionManager::Snapshot* mock, char* line1, size_t cap1, char* line2, size_t cap2, char* line3, size_t cap3) {
    if (!line1 || !line2 || !line3) return;

    if (s_textEditField == TextEditField::Nickname) {
        CopyText(line1, cap1, "Type letters or numbers.");
        CopyText(line2, cap2, "Enter saves. Esc cancels. Ctrl+V paste.");
    } else if (s_textEditField == TextEditField::RemoteEndpoint) {
        CopyText(line1, cap1, "Type full ip:port. Ctrl+V to paste.");
        CopyText(line2, cap2, "Enter saves. Esc cancels.");
    } else if (s_textEditField == TextEditField::ListenPort) {
        CopyText(line1, cap1, "Type port number (1-65535).");
        CopyText(line2, cap2, "Enter saves. Esc cancels.");
    } else {
        switch (s_state) {
            case NetplayMenuState::MenuRoot:
                CopyText(line1, cap1, "Direct Play or Settings.");
                CopyText(line2, cap2, "A opens. B closes.");
                break;
            case NetplayMenuState::DirectConnectEntry:
                CopyText(line1, cap1, "Choose Host or Join.");
                CopyText(line2, cap2, "A opens. B goes back.");
                break;
            case NetplayMenuState::HostEntry:
                CopyText(line1, cap1, "Start hosting or edit port.");
                CopyText(line2, cap2, "C copies your address.");
                break;
            case NetplayMenuState::JoinEntry:
                CopyText(line1, cap1, "Join a host. Left/Right browse peers.");
                CopyText(line2, cap2, "C copies your address.");
                break;
            case NetplayMenuState::SettingsCategoryMenu:
                CopyText(line1, cap1, "Up/Down navigate. A opens.");
                CopyText(line2, cap2, "B returns to root.");
                break;
            case NetplayMenuState::SettingsEntry:
                if (s_selectedIndex == 0) {
                    CopyText(line1, cap1, "A edits nickname.");
                    CopyText(line2, cap2, "B goes back to root.");
                } else if (s_selectedIndex == 1) {
                    CopyText(line1, cap1, "Left/Right adjust delay frames.");
                    CopyText(line2, cap2, "A also increments.");
                } else {
                    CopyText(line1, cap1, "A toggles. Left/Right also toggles.");
                    CopyText(line2, cap2, "B goes back to root.");
                }
                break;
            case NetplayMenuState::Connecting:
                CopyText(line1, cap1, "Waiting for peer. C copies address.");
                CopyText(line2, cap2, "B cancels.");
                break;
            case NetplayMenuState::Handshake:
                CopyText(line1, cap1, "Handshake in progress.");
                CopyText(line2, cap2, "B cancels.");
                break;
            case NetplayMenuState::ConnectedSession:
                CopyText(line1, cap1, "Session connected. C copies address.");
                CopyText(line2, cap2, "A enters Character Select.");
                break;
            case NetplayMenuState::CharSelTransition:
                CopyText(line1, cap1, "Launch netplay Character Select.");
                CopyText(line2, cap2, "A launches. B goes back.");
                break;
            case NetplayMenuState::DisconnectError:
                CopyText(line1, cap1, "A clears the error.");
                CopyText(line2, cap2, "B closes the menu.");
                break;
            default:
                CopyText(line1, cap1, "Up/Down move. Left/Right edit.");
                CopyText(line2, cap2, "A/Start OK. B/Select back.");
                break;
        }
    }

    _snprintf_s(line3, cap3, _TRUNCATE, "AS2 Rollback | %s",
        mock ? SessionManager::GetStateName(mock->state) : "Idle");
}

static void RenderInGameMenu() {
    // Don't render while waiting for assets to load after a mode transition
    if (s_pendingMenuRestore) return;

    const int bgHandle = (int)ReadU32(ADDR_TITLE_MENU_BG_ACTIVE, 0);
    const uint8_t alpha = (uint8_t)Alpha8(FadeAlpha(), 255);
    GekkoBridge::Snapshot gekko{};
    SessionManager::Snapshot mock{};
    GekkoBridge::GetSnapshot(&gekko);
    SessionManager::GetSnapshot(&mock);
    char headerTitle[32];
    char headerSubtitle[32];
    char statusText[128];
    char errorText[128];
    char rawFooterLine1[64];
    char rawFooterLine2[64];
    char rawFooterLine3[64];
    char footerLine1[64];
    char footerLine2[64];
    char footerLine3[64];
    CopyDisplayText(headerTitle, sizeof(headerTitle), GetHeaderTitle(), kHeaderChars);
    CopyDisplayText(headerSubtitle, sizeof(headerSubtitle), GetHeaderSubtitle(), kHeaderChars);
    CopyDisplayText(statusText, sizeof(statusText), s_status, kStatusChars);
    CopyDisplayText(errorText, sizeof(errorText), s_lastError, kStatusChars);
    GetFooterHints(&gekko, &mock, rawFooterLine1, sizeof(rawFooterLine1), rawFooterLine2, sizeof(rawFooterLine2), rawFooterLine3, sizeof(rawFooterLine3));
    CopyDisplayText(footerLine1, sizeof(footerLine1), rawFooterLine1, kFooterChars);
    CopyDisplayText(footerLine2, sizeof(footerLine2), rawFooterLine2, kFooterChars);
    CopyDisplayText(footerLine3, sizeof(footerLine3), rawFooterLine3, kFooterChars);

    GameSetBlendMode(0, 255);
    GameSetDrawColor(255, 255, 255);
    GameDrawSprite(0, 0, bgHandle);
    if (!alpha) return;

    GameSetBlendMode(1, (uint8_t)Alpha8((float)alpha / 255.0f, 72));
    GameFillRect(0, 0, 639, 479, 0, 0, 0);

    GameSetBlendMode(1, alpha);
    GameFillRect(kPanelLeft, kPanelTop, kPanelRight, kPanelBottom, 18, 20, 28);
    GameFillRect(kPanelLeft, kPanelTop, kPanelRight, kHeaderBottom, 38, 40, 54);
    GameFillRect(kPanelLeft, kPanelTop, kPanelRight, kPanelTop + 2, 196, 54, 54);
    GameFillRect(kPanelLeft, kPanelBottom - 2, kPanelRight, kPanelBottom, 196, 54, 54);
    GameFillRect(kPanelLeft, kPanelTop, kPanelLeft + 2, kPanelBottom, 196, 54, 54);
    GameFillRect(kPanelRight - 2, kPanelTop, kPanelRight, kPanelBottom, 196, 54, 54);
    GameFillRect(kPanelLeft + 16, kHeaderBottom, kPanelRight - 16, kHeaderBottom + 1, 90, 97, 116);

    GameDrawTextf(kPanelLeft + 16, kPanelTop + 12, 255, 228, 128, "%s", headerTitle);
    GameDrawTextf(kPanelRight - 180, kPanelTop + 12, 224, 228, 236, "%s", headerSubtitle);
    GameDrawTextf(kPanelLeft + 16, kStatusY, 224, 228, 236, "%s", statusText);
    if (s_lastError[0] != '\0' && s_state == NetplayMenuState::DisconnectError) {
        GameDrawTextf(kPanelLeft + 16, kStatusY + 18, 255, 160, 160, "%s", errorText);
    }

    char buf[128];
    int y = kRowStartY;
    switch (s_state) {
        case NetplayMenuState::MenuRoot:
            RenderMenuRow(y, "Direct Play", "Host / Join", s_selectedIndex == 0, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Settings", "Config", s_selectedIndex == 1, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Back", "Close", s_selectedIndex == 2, true, alpha); y += kRowStep + 8;
            NetplayConfig::FormatEndpoint(&s_config, buf, sizeof(buf));
            RenderInfoLine(y, "Target", buf, alpha); y += 22;
            snprintf(buf, sizeof(buf), "%u saved peer%s", s_config.recent_peer_count, s_config.recent_peer_count == 1 ? "" : "s");
            RenderInfoLine(y, "History", buf, alpha);
            break;
        case NetplayMenuState::DirectConnectEntry:
            RenderMenuRow(y, "Host", "Create room", s_selectedIndex == 0, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Join", "Connect to peer", s_selectedIndex == 1, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Back", "Root", s_selectedIndex == 2, true, alpha); y += kRowStep + 8;
            NetplayConfig::FormatEndpoint(&s_config, buf, sizeof(buf));
            RenderInfoLine(y, "Target", buf, alpha); y += 22;
            snprintf(buf, sizeof(buf), "Port %u", s_config.listen_port);
            RenderInfoLine(y, "Listen", buf, alpha);
            break;
        case NetplayMenuState::HostEntry:
            RenderMenuRow(y, "Start Hosting", "Listen for peer", s_selectedIndex == 0, true, alpha); y += kRowStep;
            if (s_textEditField == TextEditField::ListenPort) {
                snprintf(buf, sizeof(buf), "%s_", s_textEditBuffer);
            } else {
                snprintf(buf, sizeof(buf), "%u", s_config.listen_port);
            }
            RenderMenuRow(y, "Listen Port", buf, s_selectedIndex == 1, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Back", "Direct Play", s_selectedIndex == 2, true, alpha); y += kRowStep + 8;
            UpdateYourAddress();
            if (HasClipboardFlash()) {
                snprintf(buf, sizeof(buf), "%s  (%s)", s_yourAddress, s_clipboardFlash);
            } else {
                snprintf(buf, sizeof(buf), "%s", s_yourAddress);
            }
            RenderInfoLine(y, "Your Address", buf, alpha); y += 22;
            RenderInfoLine(y, "UPnP", UpnpManager::GetStatusText(), alpha); y += 22;
            snprintf(buf, sizeof(buf), "%dF delay | HUD %s",
                s_config.preferred_delay_frames,
                s_config.show_netplay_hud ? "On" : "Off");
            RenderInfoLine(y, "Session", buf, alpha);
            break;
        case NetplayMenuState::JoinEntry:
            RenderMenuRow(y, "Connect", "Dial remote", s_selectedIndex == 0, true, alpha); y += kRowStep;
            if (s_textEditField == TextEditField::RemoteEndpoint) {
                snprintf(buf, sizeof(buf), "%s_", s_textEditBuffer);
            } else {
                NetplayConfig::FormatEndpoint(&s_config, buf, sizeof(buf));
            }
            RenderMenuRow(y, "Remote Endpoint", buf, s_selectedIndex == 1, true, alpha); y += kRowStep;
            GetRecentPeerSummary(buf, sizeof(buf));
            RenderMenuRow(y, "Recent Peers", buf, s_selectedIndex == 2, s_config.recent_peer_count > 0, alpha); y += kRowStep;
            RenderMenuRow(y, "Back", "Direct Play", s_selectedIndex == 3, true, alpha); y += kRowStep + 8;
            UpdateYourAddress();
            if (HasClipboardFlash()) {
                snprintf(buf, sizeof(buf), "%s  (%s)", s_yourAddress, s_clipboardFlash);
            } else {
                snprintf(buf, sizeof(buf), "%s", s_yourAddress);
            }
            RenderInfoLine(y, "Your Address", buf, alpha);
            break;
        case NetplayMenuState::SettingsCategoryMenu:
            // Legacy state — render same as flat settings for safety
        case NetplayMenuState::SettingsEntry: {
            // Flat settings: 0=Nickname, 1=Delay, 2=HUD, 3=Verbose, 4=Back
            if (s_textEditField == TextEditField::Nickname) {
                snprintf(buf, sizeof(buf), "%s_", s_textEditBuffer);
            } else {
                snprintf(buf, sizeof(buf), "%s", s_config.nickname);
            }
            RenderMenuRow(y, "Nickname", buf, s_selectedIndex == 0, true, alpha); y += kRowStep;
            snprintf(buf, sizeof(buf), "%d frames", s_config.preferred_delay_frames);
            RenderMenuRow(y, "Input Delay", buf, s_selectedIndex == 1, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Netplay HUD", s_config.show_netplay_hud ? "Visible" : "Hidden", s_selectedIndex == 2, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Verbose Logging", s_config.verbose_logging ? "On" : "Off", s_selectedIndex == 3, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Back", "Return to root", s_selectedIndex == 4, true, alpha);
            break;
        }
        case NetplayMenuState::Connecting:
            RenderMenuRow(y, "Cancel", "Stop", s_selectedIndex == 0, true, alpha); y += kRowStep + 8;
            RenderInfoLine(y, "Flow", SessionManager::GetModeName(mock.mode), alpha); y += 22;
            RenderInfoLine(y, "Target", mock.endpoint, alpha); y += 22;
            UpdateYourAddress();
            if (HasClipboardFlash()) {
                snprintf(buf, sizeof(buf), "%s  (%s)", s_yourAddress, s_clipboardFlash);
            } else {
                snprintf(buf, sizeof(buf), "%s", s_yourAddress);
            }
            RenderInfoLine(y, "Your Address", buf, alpha); y += 22;
            RenderInfoLine(y, "UPnP", UpnpManager::GetStatusText(), alpha); y += 22;
            if (mock.rtt_ms > 0.0f) {
                snprintf(buf, sizeof(buf), "%.0f ms", mock.rtt_ms);
                RenderInfoLine(y, "Ping", buf, alpha); y += 22;
            }
            snprintf(buf, sizeof(buf), "%dF", mock.frames_remaining);
            RenderInfoLine(y, "ETA", buf, alpha);
            break;
        case NetplayMenuState::Handshake:
            RenderMenuRow(y, "Cancel", "Abort", s_selectedIndex == 0, true, alpha); y += kRowStep + 8;
            RenderInfoLine(y, "Flow", SessionManager::GetModeName(mock.mode), alpha); y += 22;
            RenderInfoLine(y, "Peer", mock.peer_nickname, alpha); y += 22;
            if (mock.rtt_ms > 0.0f) {
                snprintf(buf, sizeof(buf), "%.0f ms", mock.rtt_ms);
                RenderInfoLine(y, "Ping", buf, alpha); y += 22;
            }
            snprintf(buf, sizeof(buf), "%08X", NetplayHooks::GetBuildSignature());
            RenderInfoLine(y, "Build", buf, alpha);
            break;
        case NetplayMenuState::ConnectedSession:
            RenderMenuRow(y, "Character Select", "Enter", s_selectedIndex == 0, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Disconnect", "Leave", s_selectedIndex == 1, true, alpha); y += kRowStep + 8;
            RenderInfoLine(y, "Peer", mock.peer_nickname, alpha); y += 22;
            if (mock.rtt_ms > 0.0f) {
                snprintf(buf, sizeof(buf), "%.0f ms", mock.rtt_ms);
                RenderInfoLine(y, "Ping", buf, alpha); y += 22;
            }
            RenderInfoLine(y, "Target", mock.endpoint, alpha);
            break;
        case NetplayMenuState::CharSelTransition:
            RenderMenuRow(y, "Launch Match", "CharSel", s_selectedIndex == 0, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Back", "Session", s_selectedIndex == 1, true, alpha); y += kRowStep + 8;
            RenderInfoLine(y, "Path", "Netplay -> Mode 6", alpha); y += 22;
            RenderInfoLine(y, "Peer", mock.peer_nickname, alpha);
            break;
        case NetplayMenuState::PostMatch:
            RenderMenuRow(y, "Rematch", "CharSel", s_selectedIndex == 0, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Return to Session", "Lobby", s_selectedIndex == 1, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Disconnect", "Leave", s_selectedIndex == 2, true, alpha); y += kRowStep + 8;
            RenderInfoLine(y, "Peer", mock.peer_nickname, alpha); y += 22;
            RenderInfoLine(y, "Result", "Match Complete", alpha);
            break;
        case NetplayMenuState::DisconnectError:
            RenderMenuRow(y, "Acknowledge", "Clear", s_selectedIndex == 0, true, alpha); y += kRowStep;
            RenderMenuRow(y, "Close Menu", "Close", s_selectedIndex == 1, true, alpha);
            break;
        default:
            break;
    }

    GameSetBlendMode(1, alpha);
    GameFillRect(kPanelLeft + 16, kFooterTop, kPanelRight - 16, kFooterTop + 1, 90, 97, 116);
    GameDrawTextf(kPanelLeft + 16, kFooterTop + 10, 168, 174, 188, "%s", footerLine1);
    GameDrawTextf(kPanelLeft + 16, kFooterTop + 10 + kFooterLineStep, 168, 174, 188, "%s", footerLine2);
    GameDrawTextf(kPanelLeft + 16, kFooterTop + 10 + (kFooterLineStep * 2), 230, 234, 242, "%s", footerLine3);

    GameSetBlendMode(0, 255);
    GameSetDrawColor(255, 255, 255);
}

void HandleDisconnection(const char* reason) {
    LOG_NETPLAY(LOG_WARNING, "[Menu] HandleDisconnection: reason='%s' mode=%u type=%u menuVisible=%d",
        reason ? reason : "(null)", GetGameMode(), GetGameType(), MenuVisible() ? 1 : 0);
    // Do NOT gate on MenuVisible() — disconnect can happen during CharSel/Match
    // when the menu is hidden. OpenDisconnectError handles forcing back to menu mode.
    OpenDisconnectError(reason ? reason : "Disconnected.");
}
bool IsMenuActive() { return MenuVisible(); }
bool ConsumesGameInput() { return MenuVisible(); }

const char* GetStateName(NetplayMenuState state) {
    switch (state) {
        case NetplayMenuState::Inactive: return "Inactive";
        case NetplayMenuState::MenuRoot: return "MenuRoot";
        case NetplayMenuState::DirectConnectEntry: return "DirectConnectEntry";
        case NetplayMenuState::HostEntry: return "HostEntry";
        case NetplayMenuState::JoinEntry: return "JoinEntry";
        case NetplayMenuState::SettingsCategoryMenu: return "SettingsCategoryMenu";
        case NetplayMenuState::SettingsEntry: return "SettingsEntry";
        case NetplayMenuState::Connecting: return "Connecting";
        case NetplayMenuState::Handshake: return "Handshake";
        case NetplayMenuState::ConnectedSession: return "ConnectedSession";
        case NetplayMenuState::CharSelTransition: return "CharSelTransition";
        case NetplayMenuState::PostMatch: return "PostMatch";
        case NetplayMenuState::DisconnectError: return "DisconnectError";
        default: return "Unknown";
    }
}

bool GetSnapshot(Snapshot* out) {
    if (!out) return false;
    out->initialized = s_initialized;
    out->hook_installed = s_hookInstalled;
    out->menu_active = MenuVisible();
    out->captures_input = s_captureInput;
    out->intercept_enabled = s_interceptEnabled;
    out->last_intercept_fade = s_lastInterceptFade;
    out->intercept_count = s_interceptCount;
    out->last_intercept_source_mode = s_lastInterceptSourceMode;
    out->current_game_mode = GetGameMode();
    out->current_game_type = GetGameType();
    out->active_root_branch = s_activeRootBranch;
    out->active_settings_category = s_activeSettingsCategory;
    out->state_revision = s_stateRevision;
    out->selected_index = s_selectedIndex;
    out->fade_frames = s_fadeFrames;
    out->connection_stats_visible = s_config.show_connection_stats;
    out->hud_visible = s_config.show_netplay_hud;
    out->state = s_state;
    CopyText(out->status, sizeof(out->status), s_status);
    CopyText(out->last_error, sizeof(out->last_error), s_lastError);
    return true;
}

} // namespace NetplayMenuController
