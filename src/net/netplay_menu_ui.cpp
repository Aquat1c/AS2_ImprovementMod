/**
 * Alice Senki 2 - Netplay Menu UI
 *
 * In-game rendering using the game's native DXLib-based render primitives.
 * All drawing goes through function pointers to the game's own render API.
 */

#include "net/netplay_menu_ui.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include "net/netplay_menu_state.h"
#include "ui/game_settings_menu.h"
#include "net/netplay_menu_render.h"
#include "net/player_side_mapping.h"
#include "net/session_manager.h"
#include "net/set_tracker.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ============================================================================
// Game render function addresses
// ============================================================================

namespace {

constexpr uintptr_t ADDR_RENDER_FILL_RECT    = 0x5D2F50;
constexpr uintptr_t ADDR_RENDER_SET_BLEND     = 0x5D2F80;
constexpr uintptr_t ADDR_RENDER_SET_COLOR     = 0x5D3030;
constexpr uintptr_t ADDR_RENDER_CREATE_COLOR  = 0x5D3150;
constexpr uintptr_t ADDR_RENDER_DRAW_SPRITE   = 0x5D3130;
constexpr uintptr_t ADDR_DRAW_FORMAT_STRING   = 0x629A20;

constexpr uintptr_t ADDR_TITLE_MENU_BG_ACTIVE = 0x8EA00C;
constexpr uintptr_t ADDR_NET_MENU_BG = 0x7AC2A4;  // data\net.bin sprite (vanilla dword_7AC2A4)

typedef int (__cdecl *RenderFillRect_t)(int left, int top, int right, int bottom, int color, int drawFlag);
typedef int (__cdecl *RenderSetBlendMode_t)(int blendMode, unsigned __int8 alphaValue);
typedef int (__cdecl *RenderCreateColor_t)(unsigned __int8 r, unsigned __int8 g, unsigned __int8 b);
typedef int (__cdecl *RenderDrawSprite_t)(int x, int y, int spriteHandle, int transFlag);
typedef int (__cdecl *DrawFormatString_t)(int x, int y, unsigned int color, char* fmt, ...);
typedef int (__cdecl *RenderSetDrawColor_t)(unsigned __int8 r, unsigned __int8 g, unsigned __int8 b);

// ============================================================================
// Layout constants — 640x480, font 8px/char half-width, 16px tall
// ============================================================================

constexpr int kPanelLeft    = 16;
constexpr int kPanelTop     = 16;  // same as the key config screen
constexpr int kPanelRight   = 604;
constexpr int kPanelBottom  = 452;
constexpr int kContentLeft  = 24;
constexpr int kContentRight = 588;
constexpr int kLabelX       = 32;
constexpr int kValueX       = 240;
constexpr int kSelectorX    = 220;
constexpr int kHintX        = 360;
constexpr int kHeaderBottom = 96;   // title + subtitle + status need room
constexpr int kRowStartY    = 106;
constexpr int kRowStep      = 32;  // vanilla row pitch, kept comfortable to navigate
constexpr int kSectionGap   = 18;  // between the action rows and an info block
constexpr int kSectionLabel = 22;  // between a section heading and its first line
constexpr int kInfoStep     = 24;
constexpr int kFooterTop    = 418;
constexpr int kFooterStep   = 16;
constexpr int kFadeFrames   = 25;
constexpr int kContentBottom = kFooterTop - 6;

// Widths are in characters, so they track the face being drawn. The 19px
// Mincho averages ~10px per character, not the 8px the vanilla bitmap font used,
// so every column holds proportionally fewer.
constexpr size_t kLabelChars  = 18;   // 32 -> kSelectorX(220), 188px / 10
constexpr size_t kValueChars  = 30;   // 240 -> kContentRight(544)
constexpr size_t kSelectorChars = 13; // fixed selector column width
constexpr size_t kHintChars   = 18;   // 360 -> 544
constexpr size_t kFullChars   = 50;   // 32 -> 544
constexpr size_t kFooterChars = 50;

static void FormatPlayerMatchupLine(char* out, size_t outCap,
                                    const char* p1Name, int p1Wins,
                                    int p2Wins,
                                    const char* p2Name) {
    if (!out || outCap == 0) {
        return;
    }
    _snprintf_s(out, outCap, _TRUNCATE, "%s (%d)    (%d)    %s",
        p1Name && p1Name[0] ? p1Name : "P1",
        p1Wins,
        p2Wins,
        p2Name && p2Name[0] ? p2Name : "P2");
}

static void FormatSessionMatchupLine(char* out, size_t outCap,
                                     const NetMenu::MenuSnapshot* snap) {
    if (!out || outCap == 0 || !snap) {
        return;
    }

    int p1Wins = 0;
    int p2Wins = 0;
    Net::SetTracker_GetGameSideWins(&p1Wins, &p2Wins);

    const char* local = snap->local_nickname[0] ? snap->local_nickname : "Local";
    const char* peer = snap->peer_nickname[0] ? snap->peer_nickname : "Remote";

    int localSlot = Net::PlayerMapping_GetLocalGameSlot();
    if (localSlot != 0 && localSlot != 1) {
        localSlot = Net::Session_GetRole() == Net::SessionRole::Host ? 0 : 1;
    }

    const char* p1Name = localSlot == 0 ? local : peer;
    const char* p2Name = localSlot == 0 ? peer : local;
    FormatPlayerMatchupLine(out, outCap, p1Name, p1Wins, p2Wins, p2Name);
}

// ============================================================================
// Render helpers
// ============================================================================

static int GameCreateColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((RenderCreateColor_t)ADDR_RENDER_CREATE_COLOR)(r, g, b);
}

static void GameDrawSprite(int x, int y, int spriteHandle) {
    if (spriteHandle > 0) {
        ((RenderDrawSprite_t)ADDR_RENDER_DRAW_SPRITE)(x, y, spriteHandle, 1);
    }
}

static uint32_t ReadU32(uintptr_t a, uint32_t d = 0) {
    __try { return *(volatile uint32_t*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return d; }
}

// The overlay draws through ImGui, which knows nothing about the game's blend
// state, so text ignored every fade. Remember the alpha the menu just set and
// carry it into the text colour.
static uint8_t s_menuTextAlpha = 255;

static void GameSetBlend(int mode, uint8_t alpha) {
    if (mode != 0) {
        s_menuTextAlpha = alpha;
    }
    ((RenderSetBlendMode_t)ADDR_RENDER_SET_BLEND)(mode, alpha);
}

static void GameFillRect(int l, int t, int r, int b, uint8_t cr, uint8_t cg, uint8_t cb) {
    ((RenderFillRect_t)ADDR_RENDER_FILL_RECT)(l, t, r, b, GameCreateColor(cr, cg, cb), 1);
}

static void GameSetDrawColor(uint8_t r, uint8_t g, uint8_t b) {
    ((RenderSetDrawColor_t)ADDR_RENDER_SET_COLOR)(r, g, b);
}

static bool IsUtf8ContinuationByte(unsigned char value) {
    return (value & 0xC0) == 0x80;
}

static size_t Utf8CodepointBytes(const char* text, size_t offset) {
    size_t next = offset + 1;
    while (text && text[next] && IsUtf8ContinuationByte((unsigned char)text[next])) {
        ++next;
    }
    return next - offset;
}

static size_t Utf8PrefixBytes(const char* text, size_t displayUnits) {
    size_t offset = 0;
    size_t units = 0;
    while (text && text[offset]) {
        const size_t codepointBytes = Utf8CodepointBytes(text, offset);
        const size_t codepointUnits = ((unsigned char)text[offset] & 0x80) ? 2 : 1;
        if (units + codepointUnits > displayUnits) {
            break;
        }
        offset += codepointBytes;
        units += codepointUnits;
    }
    return offset;
}

static size_t Utf8CountCodepoints(const char* text) {
    size_t units = 0;
    for (size_t offset = 0; text && text[offset]; ) {
        units += ((unsigned char)text[offset] & 0x80) ? 2 : 1;
        offset += Utf8CodepointBytes(text, offset);
    }
    return units;
}

static std::wstring Utf8ToWide(const char* text) {
    if (!text || !text[0]) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), size);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

static std::string WideToGameText(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(932, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(932, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

static std::string Utf8ToGameText(const char* text) {
    const std::wstring wideText = Utf8ToWide(text);
    if (wideText.empty()) {
        return text ? std::string(text) : std::string();
    }
    return WideToGameText(wideText);
}

// The proxy hosts ImGui and the Mincho face that matches the vanilla labels.
// Its coordinate space is already the game's 640x480, so positions pass through
// unchanged. Resolved once; if it is missing we keep using the game's renderer.
typedef void  (*ProxyDrawMenuText_t)(float, float, unsigned int, const char*, float);
typedef int   (*ProxyMenuFontReady_t)();
typedef float (*ProxyMeasureMenuText_t)(const char*, float);

static ProxyDrawMenuText_t     s_proxyDrawMenuText = nullptr;
static ProxyMeasureMenuText_t  s_proxyMeasureMenuText = nullptr;
static bool                    s_proxyMenuTextResolved = false;

static ProxyDrawMenuText_t ResolveProxyMenuText() {
    if (s_proxyMenuTextResolved) {
        return s_proxyDrawMenuText;
    }
    s_proxyMenuTextResolved = true;

    HMODULE proxy = GetModuleHandleA("d3d9.dll");
    if (!proxy) {
        return nullptr;
    }
    auto ready = (ProxyMenuFontReady_t)GetProcAddress(proxy, "AS2Proxy_MenuFontReady");
    if (!ready || !ready()) {
        return nullptr;
    }
    s_proxyDrawMenuText =
        (ProxyDrawMenuText_t)GetProcAddress(proxy, "AS2Proxy_DrawMenuText");
    s_proxyMeasureMenuText =
        (ProxyMeasureMenuText_t)GetProcAddress(proxy, "AS2Proxy_MeasureMenuText");
    return s_proxyDrawMenuText;
}

// Netplay screens keep the tighter size their column widths were built around;
// the settings screens ask for a larger one explicitly.
constexpr float kNetplayTextSize = 22.0f;  // same proportion vanilla uses in a 32px row
static float s_menuTextSize = kNetplayTextSize;

static void GameDrawText(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* fmt, ...) {
    char utf8Buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(utf8Buf, sizeof(utf8Buf), _TRUNCATE, fmt, args);
    va_end(args);

    if (ProxyDrawMenuText_t draw = ResolveProxyMenuText()) {
        // ImGui packs colour as ABGR; UTF-8 goes straight through, so this path
        // also carries scripts the game's CP932 font cannot show.
        const unsigned int abgr = ((unsigned)s_menuTextAlpha << 24) |
                                  ((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r;
        draw((float)x, (float)y, abgr, utf8Buf, s_menuTextSize);
        return;
    }

    const std::string gameText = Utf8ToGameText(utf8Buf);
    const char* drawText = gameText.empty() ? utf8Buf : gameText.c_str();
    ((DrawFormatString_t)ADDR_DRAW_FORMAT_STRING)(x, y, (unsigned int)GameCreateColor(r, g, b), (char*)"%s", (char*)drawText);
}

// Same as GameDrawText but at an explicit size, so headings, body rows and
// footnotes can differ instead of all rendering at one weight.
static void GameDrawTextSized(int x, int y, uint8_t r, uint8_t g, uint8_t b,
                              float size, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    const float prev = s_menuTextSize;
    s_menuTextSize = size;
    GameDrawText(x, y, r, g, b, "%s", buf);
    s_menuTextSize = prev;
}

// One scale for the whole menu: heading, body, and the quieter notes under it.
constexpr float kTitleSize    = 26.0f;
constexpr float kBodySize     = 22.0f;
constexpr float kNoteSize     = 17.0f;
constexpr float kFootSize     = 16.0f;

static void ClipText(char* out, size_t outCap, const char* in, size_t maxChars) {
    if (!out || outCap == 0) return;
    out[0] = '\0';
    if (!in || !in[0]) return;

    char normalized[256] = {};
    size_t n = 0;
    for (size_t i = 0; in[i] && n + 1 < sizeof(normalized); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c < 32) continue;
        if (c == ' ' && n > 0 && normalized[n - 1] == ' ') continue;
        normalized[n++] = (char)c;
    }
    while (n > 0 && normalized[n - 1] == ' ') {
        --n;
    }
    normalized[n] = '\0';
    if (!normalized[0]) return;

    const size_t codepointCount = Utf8CountCodepoints(normalized);
    if (codepointCount <= maxChars || maxChars < 4) {
        strncpy_s(out, outCap, normalized, _TRUNCATE);
        return;
    }

    const size_t prefixBytes = Utf8PrefixBytes(normalized, maxChars - 3);
    _snprintf_s(out, outCap, _TRUNCATE, "%.*s...", (int)prefixBytes, normalized);
}

/// Format text edit buffer with cursor indicator at the given position.
/// Output: "> text|rest" where | is the cursor position.
static void FormatEditBufferWithCursor(char* out, size_t outLen, const char* buf, int cursorPos) {
    int len = (int)strlen(buf);
    if (cursorPos < 0) cursorPos = 0;
    if (cursorPos > len) cursorPos = len;
    _snprintf_s(out, outLen, _TRUNCATE, "> %.*s|%s", cursorPos, buf, buf + cursorPos);
}

static int Alpha8(float normalized, int maxAlpha) {
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    return (int)(normalized * (float)maxAlpha);
}

static int WrapTextLines(const char* text,
                         char lines[][96],
                         int maxLines,
                         size_t maxCharsPerLine) {
    if (!text || !text[0] || !lines || maxLines <= 0 || maxCharsPerLine == 0) {
        return 0;
    }

    for (int index = 0; index < maxLines; index++) {
        lines[index][0] = '\0';
    }

    char normalized[256] = {};
    ClipText(normalized, sizeof(normalized), text, sizeof(normalized) - 1);
    if (!normalized[0]) {
        return 0;
    }

    int lineCount = 0;
    char* context = nullptr;
    char* word = strtok_s(normalized, " ", &context);
    while (word && lineCount < maxLines) {
        char candidate[96] = {};
        if (lines[lineCount][0]) {
            _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s %s", lines[lineCount], word);
        } else {
            _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s", word);
        }

        if (Utf8CountCodepoints(candidate) > maxCharsPerLine) {
            if (lines[lineCount][0]) {
                lineCount++;
                if (lineCount >= maxLines) {
                    break;
                }
                ClipText(lines[lineCount], sizeof(lines[lineCount]), word, maxCharsPerLine);
            } else {
                ClipText(lines[lineCount], sizeof(lines[lineCount]), word, maxCharsPerLine);
            }
        } else {
            strncpy_s(lines[lineCount], sizeof(lines[lineCount]), candidate, _TRUNCATE);
        }

        word = strtok_s(nullptr, " ", &context);
        if (!word && lines[lineCount][0]) {
            lineCount++;
        }
    }

    return lineCount;
}

// The panel is drawn before the content, so it sizes itself from the extent the
// previous frame recorded. A menu holds still, so the one-frame lag never shows;
// it stops short screens sitting in a tall empty box.
static int s_contentBottomCur  = 0;
static int s_contentBottomPrev = 0;

static void NoteContentBottom(int y) {
    const int bottom = y + 20;
    if (bottom > s_contentBottomCur) {
        s_contentBottomCur = bottom;
    }
}

static bool HasRowSpace(int y) {
    return y >= kHeaderBottom && (y + 20) <= kContentBottom;
}

static bool HasInfoSpace(int y) {
    return y >= kHeaderBottom && (y + 16) <= kContentBottom;
}

static void RenderSectionLabel(int y, const char* label, uint8_t alpha) {
    if (!HasInfoSpace(y)) return;
    NoteContentBottom(y);

    char clippedLabel[64];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kLabelChars);
    GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 180));
    GameDrawText(kLabelX, y, 163, 163, 163, "%s", clippedLabel);
}

static const char* GetHeaderBadge(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:
            return "ONLINE";
        case NetMenu::MenuState::SpectateEntry:
        case NetMenu::MenuState::SpectatorConnected:
            return "WATCH";
        case NetMenu::MenuState::SpectatorConnecting:
            return snap->join_spectator_probe_active ? "PLAY" : "WATCH";
        case NetMenu::MenuState::SettingsEntry:
        case NetMenu::MenuState::SettingsCategoryMenu:
            return "SETUP";
        case NetMenu::MenuState::DisconnectError:
            return "NOTICE";
        default:
            return "PLAY";
    }
}

static void RenderHeaderBadge(const char* label, uint8_t alpha) {
    if (!label || !label[0]) {
        return;
    }

    // Measured for the face actually in use; at 8px/char the badge ran off the
    // panel edge instead of sitting inside it.
    const int width = (int)((float)strlen(label) * kNoteSize * 0.62f);
    const int x = kContentRight - width;
    GameSetBlend(1, alpha);
    GameDrawTextSized(x, kPanelTop + 10, 184, 184, 184, kNoteSize, "%s", label);
}

static const char* GetStateSummary(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::SpectatorConnecting:
            return snap->join_spectator_probe_active
                ? "Checking whether the host is already in a match..."
                : "Reaching the watch server...";
        case NetMenu::MenuState::SpectatorConnected:
            return (snap->spectator_client_match_id != 0 || snap->spectator_playback_active)
                ? "Watching a live match."
                : "Waiting for a match to start.";
        case NetMenu::MenuState::Connecting:
        case NetMenu::MenuState::Handshake:
            return snap->connecting_as_host
                ? "Opening your room..."
                : "Connecting to the host...";
        case NetMenu::MenuState::ConnectedSession:
            return "Ready up to start the match.";
        case NetMenu::MenuState::CharSelTransition:
            return "Both players connected.";
        case NetMenu::MenuState::PostMatch:
            return "Match complete.";
        case NetMenu::MenuState::DisconnectError:
            return "Connection interrupted.";
        default:
            return nullptr;
    }
}

static const char* GetFooterLabel(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:
            return "Online Menu";
        case NetMenu::MenuState::SpectateEntry:
        case NetMenu::MenuState::SpectatorConnected:
            return "Watch a Match";
        case NetMenu::MenuState::SpectatorConnecting:
            return snap->join_spectator_probe_active ? "Play Online" : "Watch a Match";
        case NetMenu::MenuState::SettingsEntry:
        case NetMenu::MenuState::SettingsCategoryMenu:
            return "Connection Settings";
        case NetMenu::MenuState::DisconnectError:
            return "Connection Notice";
        default:
            return "Play Online";
    }
}

static bool ShouldShowStatusCard(const NetMenu::MenuSnapshot* snap) {
    // Settings pages already show the active value on each row; the status card
    // duplicates that feedback and steals vertical space from dense submenus.
    if (snap->state == NetMenu::MenuState::SettingsEntry ||
        snap->state == NetMenu::MenuState::SettingsCategoryMenu) {
        return false;
    }
    if (snap->status[0]) return true;
    return GetStateSummary(snap) != nullptr;
}

static void RenderStatusCard(const NetMenu::MenuSnapshot* snap, uint8_t alpha, float fadeNorm, int* outBottom) {
    if (!ShouldShowStatusCard(snap)) {
        if (outBottom) *outBottom = kRowStartY;
        return;
    }

    const char* summary = GetStateSummary(snap);
    const char* primary = snap->status[0] ? snap->status : summary;
    if (!primary || !primary[0]) {
        if (outBottom) *outBottom = kRowStartY;
        return;
    }

    const int cardTop = kHeaderBottom + 4;
    const int cardBottom = cardTop + 26;
    if (outBottom) *outBottom = cardBottom + 10;

    char primaryText[192] = {};
    ClipText(primaryText, sizeof(primaryText), primary, kFullChars);

    GameSetBlend(1, alpha);
    GameDrawTextSized(kLabelX, cardTop + 2, 193, 193, 193, kNoteSize,
                      "%s", primaryText);
}

// ============================================================================
// Row rendering
// ============================================================================

static void DrawRowHighlight(int y, bool selected, bool enabled, uint8_t alpha) {
    // Same treatment as the vanilla options screen: a red bar over the selected
    // row (decomp sub_55C720 / sub_55D6F0), everything else just dimmed.
    if (selected) {
        GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 70));
        GameFillRect(kContentLeft - 2, y - 6, kContentRight + 2, y + 24, 0, 0, 0);
        GameSetBlend(2, (uint8_t)Alpha8((float)alpha / 255.0f, 128));
        GameFillRect(kContentLeft, y - 5, kContentRight, y + 23,
            enabled ? 255 : 120, 0, 0);
        GameSetBlend(1, alpha);
    } else {
        GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 60));
        GameFillRect(kContentLeft - 2, y - 6, kContentRight + 2, y + 24, 0, 0, 0);
    }
}

static void RenderRow(int y, const char* label, const char* value, bool selected, bool enabled, uint8_t alpha) {
    if (!HasRowSpace(y)) return;
    NoteContentBottom(y);

    DrawRowHighlight(y, selected, enabled, alpha);

    GameSetBlend(1, alpha);
    char clippedLabel[80];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kLabelChars);
    GameDrawText(kLabelX, y,
        selected ? (enabled ? 255 : 200) : (enabled ? 232 : 168),
        selected ? (enabled ? 255 : 200) : (enabled ? 232 : 168),
        selected ? (enabled ? 255 : 200) : (enabled ? 232 : 168),
        "%s", clippedLabel);
    if (value && value[0]) {
        char clippedValue[192];
        ClipText(clippedValue, sizeof(clippedValue), value, kValueChars);
        GameDrawText(kValueX, y,
            selected ? (enabled ? 232 : 164) : (enabled ? 190 : 144),
            selected ? (enabled ? 232 : 164) : (enabled ? 190 : 144),
            selected ? (enabled ? 232 : 164) : (enabled ? 190 : 144),
            "%s", clippedValue);
    }
}

static void RenderSettingRow(int y,
                             const char* label,
                             const char* selector,
                             const char* hint,
                             bool selected,
                             bool enabled,
                             uint8_t alpha) {
    if (!HasRowSpace(y)) return;

    DrawRowHighlight(y, selected, enabled, alpha);
    GameSetBlend(1, alpha);

    char clippedLabel[80];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kLabelChars);
    GameDrawText(kLabelX, y,
        selected ? (enabled ? 255 : 200) : (enabled ? 236 : 168),
        selected ? (enabled ? 248 : 200) : (enabled ? 228 : 168),
        selected ? (enabled ? 240 : 204) : (enabled ? 216 : 172),
        "%s", clippedLabel);

    if (selector && selector[0]) {
        char clippedSelector[64];
        ClipText(clippedSelector, sizeof(clippedSelector), selector, kSelectorChars);
        GameDrawText(kSelectorX, y,
            selected ? (enabled ? 255 : 200) : (enabled ? 220 : 176),
            selected ? (enabled ? 248 : 200) : (enabled ? 232 : 184),
            selected ? (enabled ? 220 : 176) : (enabled ? 208 : 168),
            "%s", clippedSelector);
    }

    if (hint && hint[0]) {
        char clippedHint[128];
        ClipText(clippedHint, sizeof(clippedHint), hint, kHintChars);
        GameDrawText(kHintX, y,
            selected ? (enabled ? 196 : 156) : (enabled ? 168 : 140),
            selected ? (enabled ? 196 : 156) : (enabled ? 160 : 136),
            selected ? (enabled ? 188 : 148) : (enabled ? 152 : 132),
            "%s", clippedHint);
    }
}

// ============================================================================
// Info line rendering (non-selectable, for display data)
// ============================================================================

static void RenderInfoLine(int y, const char* label, const char* value, uint8_t alpha) {
    if (!HasInfoSpace(y)) return;
    NoteContentBottom(y);

    GameSetBlend(1, alpha);
    char clippedLabel[80];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kLabelChars);
    GameDrawText(kLabelX, y, 163, 163, 163, "%s", clippedLabel);
    if (value && value[0]) {
        // Value column, not the hint column. An info line has two columns, so
        // starting at kHintX (318) left a 252px gap that visually detached the
        // value from its label; kValueX also lines it up with the rows above.
        char clippedValue[192];
        ClipText(clippedValue, sizeof(clippedValue), value, kValueChars);
        GameDrawText(kValueX, y, 208, 208, 208, "%s", clippedValue);
    }
}

static void RenderPromptOverlay(const NetMenu::MenuSnapshot* snap, uint8_t alpha, float fadeNorm) {
    if (!snap->prompt_active || snap->prompt_option_count == 0) {
        return;
    }

    const int modalLeft = 116;
    const int modalTop = 142;
    const int modalRight = 524;
    const int modalBottom = 324;

    // Dim behind the modal
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 160));
    GameFillRect(0, 0, 639, 479, 0, 0, 0);

    // Modal body
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 210));
    GameFillRect(modalLeft, modalTop, modalRight, modalBottom, 24, 20, 18);

    GameSetBlend(1, alpha);
    char clippedTitle[96] = {};
    ClipText(clippedTitle, sizeof(clippedTitle),
        snap->prompt_title[0] ? snap->prompt_title : "Confirm",
        30);
    GameDrawText(modalLeft + 28, modalTop + 20, 244, 244, 244, "%s", clippedTitle);

    char wrappedLines[3][96] = {};
    const int lineCount = WrapTextLines(
        snap->prompt_body,
        wrappedLines,
        3,
        40);
    int bodyY = modalTop + 54;
    for (int index = 0; index < lineCount; index++) {
        GameDrawText(modalLeft + 28, bodyY, 209, 209, 209, "%s", wrappedLines[index]);
        bodyY += 18;
    }

    const int buttonGap = 10;
    const int buttonTop = modalBottom - 56;
    const int buttonBottom = modalBottom - 22;
    const int buttonWidth = (modalRight - modalLeft - 32 - (buttonGap * ((int)snap->prompt_option_count - 1))) /
        (int)snap->prompt_option_count;
    int buttonLeft = modalLeft + 16;
    for (uint32_t index = 0; index < snap->prompt_option_count; index++) {
        const bool selected = snap->prompt_selected_index == index;
        const char* label = snap->prompt_option_labels[index][0]
            ? snap->prompt_option_labels[index]
            : "Option";

        GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, selected ? 140 : 80));
        GameFillRect(buttonLeft, buttonTop, buttonLeft + buttonWidth, buttonBottom,
            selected ? 180 : 50,
            selected ? 60 : 40,
            selected ? 50 : 38);

        char clippedLabel[48];
        ClipText(clippedLabel, sizeof(clippedLabel), label, 18);
        GameSetBlend(1, alpha);
        GameDrawText(buttonLeft + 12, buttonTop + 9,
            selected ? 255 : 230,
            selected ? 244 : 220,
            selected ? 222 : 204,
            "%s",
            clippedLabel);
        buttonLeft += buttonWidth + buttonGap;
    }
}

static const char* ConnectModeLabel(int mode) {
    switch (mode) {
        case 0: return "Automatic";
        case 1: return "Direct Only";
        case 2: return "Relay N/A";
        default: return "Unknown";
    }
}

static const char* FrameTimingLabel(int mode) {
    switch (mode) {
        case 0: return "58.8 FPS";
        case 1: return "60.0 FPS";
        default: return "Unknown";
    }
}

static void FormatFrameTimingInfo(const NetMenu::MenuSnapshot* snap,
                                  char* out,
                                  size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    const char* local = FrameTimingLabel(snap ? snap->local_frame_timing_mode : -1);
    if (snap && snap->remote_frame_timing_valid) {
        const char* remote = FrameTimingLabel(snap->remote_frame_timing_mode);
        if (snap->frame_timing_session_locked) {
            _snprintf_s(out, outSize, _TRUNCATE, "%s locked, peer announced %s", local, remote);
        } else {
            _snprintf_s(out, outSize, _TRUNCATE, "%s local, peer %s", local, remote);
        }
    } else {
        _snprintf_s(out, outSize, _TRUNCATE, "%s%s",
            local,
            (snap && snap->frame_timing_session_locked) ? " locked" : "");
    }
}

// ============================================================================
// Menu page rendering
// ============================================================================

static void RenderMenuRoot(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    // Host/Join are the root actions now. Join covers watching too: if the
    // room is already playing, the gameplay connect fails and the existing
    // spectator probe offers "watch instead?".
    RenderRow(y, "Host", "Open a room", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Join", "Connect or watch", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Settings", "Name and routing", snap->selected_index == 2, true, alpha); y += kRowStep;
    RenderRow(y, "Close", "Return to game", snap->selected_index == 3, true, alpha);
}

static void RenderDirectConnect(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Host", "Open a room", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Join", "Connect to a host", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back", "Online menu", snap->selected_index == 2, true, alpha);
}

// Build "Status [ext-IP:port]" for host-side STUN display. Join/guest screens
// use snap->nat_stun_status directly (no endpoint) to avoid showing the user's
// own external address where it would be mistaken for the connection target.
static void FormatStunStatusWithEndpoint(char* out, size_t cap,
                                         const NetMenu::MenuSnapshot* snap) {
    if (snap->nat_stun_endpoint[0]) {
        _snprintf_s(out, cap, _TRUNCATE, "%s %s", snap->nat_stun_status, snap->nat_stun_endpoint);
    } else {
        strncpy_s(out, cap, snap->nat_stun_status, _TRUNCATE);
    }
}

static void RenderHostEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Start Hosting",  "Open the room",  snap->selected_index == 0, true, alpha); y += kRowStep;

    // Listen Port: show text edit buffer if editing, else the current value
    char portVal[16];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::ListenPort) {
        FormatEditBufferWithCursor(portVal, sizeof(portVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(portVal, sizeof(portVal), _TRUNCATE, "%u", snap->listen_port);
    }
    RenderRow(y, "Room Port", portVal, snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back", "Online Menu", snap->selected_index == 2, true, alpha);
    y += kRowStep + kSectionGap;

    RenderSectionLabel(y, "Share This Room", alpha);
    y += kSectionLabel;
    char addrBuf[144];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Address", addrBuf, alpha);
    y += kInfoStep;
    // Route/Mapping/Punch/STUN are diagnostics, not player information; they
    // stay in the log.
    RenderInfoLine(y, "Status", snap->nat_route_status, alpha);
}

static void RenderJoinEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Connect to Host", "Start the connection", snap->selected_index == 0, true, alpha); y += kRowStep;

    // Remote Endpoint: show text edit buffer if editing, else the current value
    char endpointVal[128];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::RemoteEndpoint) {
        FormatEditBufferWithCursor(endpointVal, sizeof(endpointVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(endpointVal, sizeof(endpointVal), _TRUNCATE, "%s", snap->remote_endpoint);
    }
    RenderRow(y, "Host Address", endpointVal,  snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back", "Online Menu", snap->selected_index == 2, true, alpha);
    y += kRowStep + kSectionGap;
    RenderSectionLabel(y, "Connection", alpha);
    y += kSectionLabel;
    RenderInfoLine(y, "Status", snap->nat_route_status, alpha);
}

static void RenderSpectateEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Start Watching", "Connect", snap->selected_index == 0, true, alpha); y += kRowStep;

    char discoveryVal[64];
    if (snap->spectator_lan_discovery_active) {
        _snprintf_s(discoveryVal, sizeof(discoveryVal), _TRUNCATE, "Scanning nearby rooms...");
    } else if (snap->spectator_lan_result_count > 1) {
        _snprintf_s(discoveryVal, sizeof(discoveryVal), _TRUNCATE,
            "Found %u rooms", snap->spectator_lan_result_count);
    } else if (snap->spectator_lan_result_count == 1) {
        _snprintf_s(discoveryVal, sizeof(discoveryVal), _TRUNCATE, "Found 1 room");
    } else {
        _snprintf_s(discoveryVal, sizeof(discoveryVal), _TRUNCATE, "Search nearby rooms");
    }
    RenderRow(y, "Scan Local LAN", discoveryVal, snap->selected_index == 1, true, alpha); y += kRowStep;

    char endpointVal[128];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::SpectatorEndpoint) {
        FormatEditBufferWithCursor(endpointVal, sizeof(endpointVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(endpointVal, sizeof(endpointVal), _TRUNCATE, "%s", snap->spectator_endpoint);
    }
    RenderRow(y, "Watch Address", endpointVal, snap->selected_index == 2, true, alpha); y += kRowStep;
    RenderRow(y, "Back", "Online menu", snap->selected_index == 3, true, alpha);
    y += kRowStep + kSectionGap;

    RenderSectionLabel(y, "Details", alpha);
    y += kSectionLabel;
    RenderInfoLine(y, "LAN Search", snap->spectator_lan_discovery_status, alpha);
    y += kInfoStep;

    char serverBuf[64];
    _snprintf_s(serverBuf, sizeof(serverBuf), _TRUNCATE, "%s  Port %u  %d viewers",
        snap->spectators_enabled ? "On" : "Off",
        snap->spectator_listen_port,
        snap->connected_spectators);
    RenderInfoLine(y, "Server", serverBuf, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Punch", snap->spectator_punch_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Status", snap->spectator_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Palettes", snap->palette_status, alpha);
}

static void RenderSettingsCategoryMenu(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Player", "Name and gameplay", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Appearance", "HUD colors and trails", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Network", "Routing and servers", snap->selected_index == 2, true, alpha); y += kRowStep;
    RenderRow(y, "Watch", "Spectator and palettes", snap->selected_index == 3, true, alpha); y += kRowStep;
    RenderRow(y, "Diagnostics", "Debug logs", snap->selected_index == 4, true, alpha); y += kRowStep;
    RenderRow(y, "Back", "Online menu", snap->selected_index == 5, true, alpha);
}

static void RenderSettings(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;

    switch (snap->settings_category) {
    case NetMenu::SettingsCategory::Identity: {
        char nickVal[128];
        if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::Nickname) {
            FormatEditBufferWithCursor(nickVal, sizeof(nickVal), snap->text_edit_buffer, snap->text_cursor_pos);
        } else {
            _snprintf_s(nickVal, sizeof(nickVal), _TRUNCATE, "%s", snap->local_nickname);
        }
        RenderRow(y, "Display Name", nickVal, snap->selected_index == 0, true, alpha); y += kRowStep;

        char delayVal[48];
        _snprintf_s(delayVal, sizeof(delayVal), _TRUNCATE, "< %d > frames input lag", snap->preferred_delay);
        RenderRow(y, "Input delay", delayVal, snap->selected_index == 1, true, alpha); y += kRowStep;

        char rbVal[48];
        _snprintf_s(rbVal, sizeof(rbVal), _TRUNCATE, "< %d > prediction depth", snap->rollback_budget);
        RenderRow(y, "Max rollback", rbVal, snap->selected_index == 2, true, alpha); y += kRowStep;

        // Stability Bias (raw tolerance K) and Delay mode are expert knobs the
        // peers negotiate anyway; config-file only now.
        RenderRow(y, "Back", "Settings", snap->selected_index == 3, true, alpha);
        break;
    }
    case NetMenu::SettingsCategory::Appearance: {
        char selector[32];

        _snprintf_s(selector, sizeof(selector), _TRUNCATE, "< %s >",
            snap->hud_trail_color_label[0] ? snap->hud_trail_color_label : "Blue");
        RenderSettingRow(y, "Bar color", selector, "name bar",
            snap->selected_index == 0, true, alpha); y += kRowStep;

        _snprintf_s(selector, sizeof(selector), _TRUNCATE, "< %s >",
            snap->hud_text_color_label[0] ? snap->hud_text_color_label : "White");
        RenderSettingRow(y, "Text color", selector, "nickname text",
            snap->selected_index == 1, true, alpha); y += kRowStep;

        _snprintf_s(selector, sizeof(selector), _TRUNCATE, "< %s >",
            snap->hud_score_color_label[0] ? snap->hud_score_color_label : "Gold");
        RenderSettingRow(y, "Score color", selector, "P2 score",
            snap->selected_index == 2, true, alpha); y += kRowStep;

        _snprintf_s(selector, sizeof(selector), _TRUNCATE, "< %d >",
            snap->hud_trail_length > 0 ? snap->hud_trail_length : 160);
        RenderSettingRow(y, "Bar extend", selector, "px outward",
            snap->selected_index == 3, true, alpha); y += kRowStep;

        _snprintf_s(selector, sizeof(selector), _TRUNCATE, "< %s >",
            snap->hud_vertical_position_label[0] ? snap->hud_vertical_position_label : "Menu-safe");
        RenderSettingRow(y, "Name position", selector, "name row",
            snap->selected_index == 4, true, alpha); y += kRowStep;

        _snprintf_s(selector, sizeof(selector), _TRUNCATE, "< %s >",
            snap->hud_font_size_label[0] ? snap->hud_font_size_label : "Large");
        RenderSettingRow(y, "Font size", selector, "nickname text",
            snap->selected_index == 5, true, alpha); y += kRowStep;

        _snprintf_s(selector, sizeof(selector), _TRUNCATE, "< %s >",
            snap->hud_render_mode_label[0] ? snap->hud_render_mode_label : "Overlay");
        RenderSettingRow(y, "Render mode", selector, "nickname draw",
            snap->selected_index == 6, true, alpha); y += kRowStep;

        RenderInfoLine(y, "Sync", "Colors sent to opponent", alpha);
        y += kInfoStep;
        RenderInfoLine(y, "Local", "Name position applies to both sides", alpha);
        y += kInfoStep;
        RenderInfoLine(y, "F1 menu", "Top drops to menu-safe row", alpha);
        y += kInfoStep;
        RenderInfoLine(y, "Vanilla", "Game draw; stats stay overlay", alpha);
        y += kInfoStep;

        RenderRow(y, "Back", "Settings", snap->selected_index == 7, true, alpha);
        break;
    }
    case NetMenu::SettingsCategory::Endpoint: {
        char modeVal[48];
        _snprintf_s(modeVal, sizeof(modeVal), _TRUNCATE, "< %s > connection path", ConnectModeLabel(snap->connection_mode));
        RenderRow(y, "Route", modeVal, snap->selected_index == 0, true, alpha); y += kRowStep;

        RenderRow(y, "Use UPnP",        snap->upnp_enabled ? "< On > port mapping" : "< Off > port mapping", snap->selected_index == 1, true, alpha); y += kRowStep;
        RenderRow(y, "Use STUN",        snap->stun_enabled ? "< On > NAT traversal" : "< Off > NAT traversal", snap->selected_index == 2, true, alpha); y += kRowStep;
        RenderRow(y, "UDP Hole Punch",  snap->hole_punch_enabled ? "< On > direct connect" : "< Off > direct connect", snap->selected_index == 3, true, alpha); y += kRowStep;
        RenderRow(y, "Allow IPv6",      snap->allow_ipv6_endpoint ? "< On > dual-stack" : "< Off > dual-stack", snap->selected_index == 4, true, alpha); y += kRowStep;

        char relayVal[120];
        if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::RelayEndpoint) {
            FormatEditBufferWithCursor(relayVal, sizeof(relayVal), snap->text_edit_buffer, snap->text_cursor_pos);
        } else if (snap->relay_endpoint[0]) {
            _snprintf_s(relayVal, sizeof(relayVal), _TRUNCATE, "Custom relay");
        } else {
            _snprintf_s(relayVal, sizeof(relayVal), _TRUNCATE, "Default relay");
        }
        RenderRow(y, "Punch Relay", relayVal, snap->selected_index == 5, true, alpha); y += kRowStep;

        char stunVal[120];
        if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::StunEndpoint) {
            FormatEditBufferWithCursor(stunVal, sizeof(stunVal), snap->text_edit_buffer, snap->text_cursor_pos);
        } else {
            _snprintf_s(stunVal, sizeof(stunVal), _TRUNCATE, "%s", snap->stun_endpoint);
        }
        RenderRow(y, "STUN Server", stunVal, snap->selected_index == 6, true, alpha); y += kRowStep;

        RenderRow(y, "Back", "Settings", snap->selected_index == 7, true, alpha);
        break;
    }
    case NetMenu::SettingsCategory::SessionMatch: {
        RenderRow(y, "Watchers", snap->spectators_enabled ? "< On > allow spectation" : "< Off > allow spectation", snap->selected_index == 0, true, alpha); y += kRowStep;

        // Watch Port is bound ephemerally; the relay maps it on lookup, so
        // there is nothing for a player to choose. Config-file only.
        RenderRow(y, "Sync Palettes", snap->palette_sync_enabled ? "< On > share colors" : "< Off > share colors", snap->selected_index == 1, true, alpha); y += kRowStep;
        RenderRow(y, "Preview Remote", snap->remote_palette_preview_enabled ? "< On > see opponent" : "< Off > see opponent", snap->selected_index == 2, true, alpha); y += kRowStep;

        RenderRow(y, "Back", "Settings", snap->selected_index == 3, true, alpha);
        break;
    }
    case NetMenu::SettingsCategory::Diagnostics: {
        RenderRow(y, "Debug Logging", snap->debug_logging_enabled ? "< On > detailed logs" : "< Off > key events only", snap->selected_index == 0, true, alpha); y += kRowStep;
        RenderRow(y, "Back", "Settings", snap->selected_index == 1, true, alpha);
        break;
    }
    default:
        RenderRow(y, "Back", "Settings", snap->selected_index == 0, true, alpha);
        break;
    }
}

static void RenderSpectatorConnecting(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Cancel", "Stop connecting", snap->selected_index == 0, true, alpha);
    y += kRowStep + kSectionGap;

    RenderSectionLabel(y, "Details", alpha);
    y += kSectionLabel;

    if (snap->join_spectator_probe_active && snap->remote_endpoint[0]) {
        RenderInfoLine(y, "Host", snap->remote_endpoint, alpha);
        y += kInfoStep;
    }
    RenderInfoLine(y,
        snap->join_spectator_probe_active ? "Watch Port" : "Address",
        snap->spectator_endpoint,
        alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Punch", snap->spectator_punch_status, alpha);
    y += kInfoStep;
    if (snap->spectator_client_match_id != 0) {
        char matchBuf[40];
        _snprintf_s(matchBuf, sizeof(matchBuf), _TRUNCATE, "Game %u",
            snap->spectator_client_match_ordinal != 0 ? snap->spectator_client_match_ordinal : 1);
        RenderInfoLine(y, "Current Game", matchBuf, alpha);
        y += kInfoStep;
    }
    if (snap->spectator_p1_name[0] || snap->spectator_p2_name[0]) {
        char playersBuf[176];
        FormatPlayerMatchupLine(playersBuf, sizeof(playersBuf),
            snap->spectator_p1_name[0] ? snap->spectator_p1_name : "P1",
            snap->spectator_p1_wins,
            snap->spectator_p2_wins,
            snap->spectator_p2_name[0] ? snap->spectator_p2_name : "P2");
        RenderInfoLine(y, "Players", playersBuf, alpha);
        y += kInfoStep;
    }
    RenderInfoLine(y, "Status", snap->spectator_client_status, alpha);
}

static void RenderSpectatorConnected(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Stop Watching", "Return to menu", snap->selected_index == 0, true, alpha);
    y += kRowStep + kSectionGap;

    RenderSectionLabel(y, "Match", alpha);
    y += kSectionLabel;

    RenderInfoLine(y, "Address", snap->spectator_endpoint, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Punch", snap->spectator_punch_status, alpha);
    y += kInfoStep;

    if (snap->spectator_client_match_id != 0) {
        char matchBuf[40];
        _snprintf_s(matchBuf, sizeof(matchBuf), _TRUNCATE, "Game %u",
            snap->spectator_client_match_ordinal != 0 ? snap->spectator_client_match_ordinal : 1);
        RenderInfoLine(y, "Current Game", matchBuf, alpha);
        y += kInfoStep;
    }

    if (snap->spectator_p1_name[0] || snap->spectator_p2_name[0]) {
        char playersBuf[176];
        FormatPlayerMatchupLine(playersBuf, sizeof(playersBuf),
            snap->spectator_p1_name[0] ? snap->spectator_p1_name : "P1",
            snap->spectator_p1_wins,
            snap->spectator_p2_wins,
            snap->spectator_p2_name[0] ? snap->spectator_p2_name : "P2");
        RenderInfoLine(y, "Players", playersBuf, alpha);
        y += kInfoStep;
    }

    if (snap->spectator_client_relay_active) {
        char relayBuf[64];
        _snprintf_s(relayBuf, sizeof(relayBuf), _TRUNCATE, "Port %u  Viewers %u",
            snap->spectator_client_relay_port,
            snap->spectator_client_relay_spectators);
        RenderInfoLine(y, "Rebroadcast", relayBuf, alpha);
        y += kInfoStep;
    }

    if (snap->spectator_playback_active && snap->spectator_playback_status[0]) {
        RenderInfoLine(y, "Playback", snap->spectator_playback_status, alpha);
        y += kInfoStep;
    }

    RenderInfoLine(y, "Status", snap->spectator_client_status, alpha);
}

static void RenderConnecting(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Cancel", "Stop connecting", snap->selected_index == 0, true, alpha);
    y += kRowStep + kSectionGap;

    RenderSectionLabel(y, "Details", alpha);
    y += kSectionLabel;

    if (snap->connecting_as_host) {
        char addrBuf[144];
        if (snap->clipboard_flash[0]) {
            _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
        } else {
            _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
        }
        RenderInfoLine(y, "Address", addrBuf, alpha);
        y += kInfoStep;
    } else if (snap->remote_endpoint[0]) {
        RenderInfoLine(y, "Host", snap->remote_endpoint, alpha);
        y += kInfoStep;
    }

    // Ping/FPS/Mapping/Punch/STUN are diagnostics; they live in the log.
    RenderInfoLine(y, "Status", snap->nat_route_status, alpha);
}

static void RenderConnectedSession(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;

    // Row 0: Rollback Frames (adjustable left/right)
    {
        char rbVal[48];
        _snprintf_s(rbVal, sizeof(rbVal), _TRUNCATE, "< %d > prediction depth", snap->rollback_budget);
        RenderRow(y, "Max rollback", rbVal, snap->selected_index == 0, true, alpha);
        y += kRowStep;
    }

    // Row 1: Input delay (adjustable left/right)
    {
        char delVal[48];
        _snprintf_s(delVal, sizeof(delVal), _TRUNCATE, "< %d > frames input lag", snap->preferred_delay);
        RenderRow(y, "Input delay", delVal, snap->selected_index == 1, true, alpha);
        y += kRowStep;
    }

    // Row 2: Accept Match
    {
        const char* label = snap->local_accepted ? "Ready" : "Ready Up";
        const char* hint  = snap->local_accepted ? "Waiting" : "Confirm settings";
        RenderRow(y, label, hint, snap->selected_index == 2, !snap->local_accepted, alpha);
        y += kRowStep;
    }

    // Row 3: Decline
    RenderRow(y, "Leave Room", "Disconnect", snap->selected_index == 3, true, alpha);
    y += kRowStep + 8;

    // Info section: role, identity, accept status, ping, recommended delay, score, address

    // Role
    RenderInfoLine(y, "Role", snap->is_host ? "Host" : "Guest", alpha);
    y += kInfoStep;

    if (snap->local_nickname[0] || snap->peer_nickname[0]) {
        char matchupBuf[176] = {};
        FormatSessionMatchupLine(matchupBuf, sizeof(matchupBuf), snap);
        RenderInfoLine(y, "Players", matchupBuf, alpha);
        y += kInfoStep;
    }
    // Accept status
    {
        char acceptBuf[64];
        _snprintf_s(acceptBuf, sizeof(acceptBuf), _TRUNCATE, "%s / %s",
            snap->local_accepted  ? "You: Ready" : "You: Waiting",
            snap->remote_accepted ? "Opp: Ready" : "Opp: Waiting");
        RenderInfoLine(y, "Ready", acceptBuf, alpha);
        y += kInfoStep;
    }
    // Ping / recommendations
    if (snap->rtt_ms > 0.0f) {
        char pingBuf[32];
        _snprintf_s(pingBuf, sizeof(pingBuf), _TRUNCATE, "%.0f ms", snap->rtt_ms);
        RenderInfoLine(y, "Ping", pingBuf, alpha);
        y += kInfoStep;
    }
    // Stall threshold, delay mode and frame-timing internals: log only.
    if (snap->current_rounds_label[0]) {
        RenderInfoLine(y, "Rounds", snap->current_rounds_label, alpha);
        y += kInfoStep;
    }
    if (snap->is_host) {
        char addrBuf[144];
        if (snap->clipboard_flash[0]) {
            _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
        } else {
            _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
        }
        RenderInfoLine(y, "Address", addrBuf, alpha);
        y += kInfoStep;
    } else if (snap->remote_endpoint[0]) {
        RenderInfoLine(y, "Host", snap->remote_endpoint, alpha);
        y += kInfoStep;
    }
    RenderInfoLine(y, "Punch", snap->nat_punch_status, alpha);
    y += kInfoStep;
    {
        char stunBuf[96];
        if (snap->is_host) {
            FormatStunStatusWithEndpoint(stunBuf, sizeof(stunBuf), snap);
        } else {
            strncpy_s(stunBuf, sizeof(stunBuf), snap->nat_stun_status, _TRUNCATE);
        }
        RenderInfoLine(y, "STUN", stunBuf, alpha);
    }
}

static void RenderCharSelTransition(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Char Select", "Start next game", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Back to Room",   "Stay connected", snap->selected_index == 1, true, alpha);
    y += kRowStep + 8;

    if (snap->local_nickname[0] || snap->peer_nickname[0]) {
        char matchupBuf[176] = {};
        FormatSessionMatchupLine(matchupBuf, sizeof(matchupBuf), snap);
        RenderInfoLine(y, "Players", matchupBuf, alpha);
        y += kInfoStep;
    }
}

static void RenderPostMatch(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Play Again",    "Character select", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Back to Room",  "Stay connected", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Disconnect",    "Leave the room",   snap->selected_index == 2, true, alpha);
    y += kRowStep + 8;

    if (snap->local_nickname[0] || snap->peer_nickname[0]) {
        char matchupBuf[176] = {};
        FormatSessionMatchupLine(matchupBuf, sizeof(matchupBuf), snap);
        RenderInfoLine(y, "Players", matchupBuf, alpha);
        y += kInfoStep;
    }
}

static void RenderDisconnectError(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    // Show error text
    if (snap->last_error[0]) {
        GameSetBlend(1, alpha);
        char wrappedError[2][96] = {};
        const int errorLines = WrapTextLines(snap->last_error, wrappedError, 2, kFullChars);
        for (int index = 0; index < errorLines; ++index) {
            GameDrawText(kLabelX, y, 255, 128, 128, "%s", wrappedError[index]);
            y += 18;
        }
        y += 8;
    }
    y += kRowStep;
    RenderRow(y, "OK",         "Clear message", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Close", "Return to game", snap->selected_index == 1, true, alpha);
}

// ============================================================================
// State name for header
// ============================================================================

static const char* GetHeaderTitle(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:
            return "Online Menu";
        case NetMenu::MenuState::DirectConnectEntry:
        case NetMenu::MenuState::HostEntry:
        case NetMenu::MenuState::JoinEntry:
        case NetMenu::MenuState::Connecting:
        case NetMenu::MenuState::Handshake:
        case NetMenu::MenuState::ConnectedSession:
        case NetMenu::MenuState::CharSelTransition:
        case NetMenu::MenuState::PostMatch:
            return "Play Online";
        case NetMenu::MenuState::SpectateEntry:
        case NetMenu::MenuState::SpectatorConnected:
            return "Watch a Match";
        case NetMenu::MenuState::SpectatorConnecting:
            return snap->join_spectator_probe_active ? "Play Online" : "Watch a Match";
        case NetMenu::MenuState::SettingsCategoryMenu:
            return "Settings";
        case NetMenu::MenuState::SettingsEntry:
            switch (snap->settings_category) {
                case NetMenu::SettingsCategory::Identity:    return "Player";
                case NetMenu::SettingsCategory::Appearance:  return "Appearance";
                case NetMenu::SettingsCategory::Endpoint:    return "Network";
                case NetMenu::SettingsCategory::SessionMatch: return "Watch";
                case NetMenu::SettingsCategory::Diagnostics:  return "Diagnostics";
                case NetMenu::SettingsCategory::GameGeneral:  return "Game";
                default: return "Settings";
            }
        case NetMenu::MenuState::DisconnectError:
            return "Connection Notice";
        default:
            return "Online Menu";
    }
}

static const char* GetHeaderSubtitle(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:            return "Choose what to do";
        case NetMenu::MenuState::DirectConnectEntry:  return "Host or join";
        case NetMenu::MenuState::HostEntry:           return "Open a room";
        case NetMenu::MenuState::JoinEntry:           return "Connect to a host";
        case NetMenu::MenuState::SpectateEntry:       return "Find a live match";
        case NetMenu::MenuState::SpectatorConnecting:
            return snap->join_spectator_probe_active ? "Checking host status" : "Connecting";
        case NetMenu::MenuState::SpectatorConnected:
            return (snap->spectator_client_match_id != 0 || snap->spectator_playback_active)
                ? "Watching live"
                : "Waiting for a match";
        case NetMenu::MenuState::SettingsCategoryMenu: return "Choose a category";
        case NetMenu::MenuState::SettingsEntry:
            switch (snap->settings_category) {
                case NetMenu::SettingsCategory::Identity:    return "Name and gameplay tuning";
                case NetMenu::SettingsCategory::Appearance:  return "HUD colors and trail styling";
                case NetMenu::SettingsCategory::Endpoint:    return "Routing and server addresses";
                case NetMenu::SettingsCategory::SessionMatch: return "Spectator and palette options";
                case NetMenu::SettingsCategory::Diagnostics:  return "Logging controls";
                default: return "Adjust settings";
            }
        case NetMenu::MenuState::Connecting:
            return snap->connecting_as_host ? "Opening your room" : "Connecting to host";
        case NetMenu::MenuState::Handshake:
            return snap->connecting_as_host ? "Confirming your room" : "Confirming connection";
        case NetMenu::MenuState::ConnectedSession:    return "Ready check";
        case NetMenu::MenuState::CharSelTransition:   return "Open character select";
        case NetMenu::MenuState::PostMatch:           return "Choose the next step";
        case NetMenu::MenuState::DisconnectError:     return "Review the message";
        default:                                      return "";
    }
}

} // anonymous namespace

namespace NetMenu {

// Shared with the game settings menu so both screens keep one row style.
void RenderMenuRow(int y, const char* label, const char* value,
                   bool selected, bool enabled, uint8_t alpha) {
    RenderRow(y, label, value, selected, enabled, alpha);
}

void RenderMenuInfoLine(int y, const char* label, const char* value, uint8_t alpha) {
    RenderInfoLine(y, label, value, alpha);
}

int MenuRowStep() {
    return kRowStep;
}

void MenuSetTextAlpha(uint8_t alpha) {
    s_menuTextAlpha = alpha;
}

void MenuSetBlend(int mode, uint8_t alpha) {
    GameSetBlend(mode, alpha);
}

void MenuFillRect(int l, int t, int r, int b, uint8_t cr, uint8_t cg, uint8_t cb) {
    GameFillRect(l, t, r, b, cr, cg, cb);
}

void MenuDrawText(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* text) {
    GameDrawText(x, y, r, g, b, "%s", text ? text : "");
}

float MenuMeasureText(const char* text, float size) {
    ResolveProxyMenuText();
    if (s_proxyMeasureMenuText && text) {
        return s_proxyMeasureMenuText(text, size);
    }
    // The game's own renderer is a fixed-cell font; approximate at half the size.
    return text ? (float)strlen(text) * size * 0.5f : 0.0f;
}

void MenuDrawTextSized(int x, int y, uint8_t r, uint8_t g, uint8_t b, float size, const char* text) {
    const float prev = s_menuTextSize;
    s_menuTextSize = size > 0.0f ? size : kNetplayTextSize;
    GameDrawText(x, y, r, g, b, "%s", text ? text : "");
    s_menuTextSize = prev;
}

} // namespace NetMenu


// ============================================================================
// Public API
// ============================================================================

namespace NetMenuUI {

void RenderFullscreenFade(uint8_t blackAlpha) {
    if (blackAlpha == 0) return;
    GameSetBlend(1, blackAlpha);
    GameFillRect(0, 0, 639, 479, 0, 0, 0);
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
}

void Render(const NetMenu::MenuSnapshot* snap) {
    if (!snap || !snap->menu_active) return;

    // Background sprite: prefer the vanilla net.bin background (loaded by the
    // controller's presentation step); fall back to the title background if it
    // isn't loaded yet.
    int bgHandle = (int)ReadU32(ADDR_NET_MENU_BG, 0);
    if (bgHandle == 0) {
        bgHandle = (int)ReadU32(ADDR_TITLE_MENU_BG_ACTIVE, 0);
    }
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
    GameDrawSprite(0, 0, bgHandle);

    float fadeNorm = (float)snap->fade_frames / (float)kFadeFrames;
    if (fadeNorm < 0.0f) fadeNorm = 0.0f;
    if (fadeNorm > 1.0f) fadeNorm = 1.0f;
    uint8_t alpha = (uint8_t)Alpha8(fadeNorm, 255);

    // Fade the whole scene (background included) from / to black, settling on a
    // constant readability dim once the menu is fully open. fadeNorm: 0 = black,
    // 1 = open. Drawing this over the background is what makes the open/close
    // transitions actually fade instead of popping the background in and out.
    constexpr int kSceneDim = 120;
    uint8_t sceneOverlay = (uint8_t)((float)kSceneDim + (1.0f - fadeNorm) * (255.0f - (float)kSceneDim));
    GameSetBlend(1, sceneOverlay);
    GameFillRect(0, 0, 639, 479, 0, 0, 0);

    if (!alpha) {
        GameSetBlend(0, 255);
        GameSetDrawColor(255, 255, 255);
        return;
    }

    // The game settings pages reproduce the native options screen, so they draw
    // the whole screen themselves instead of sitting inside the netplay frame.
    if (snap->state == NetMenu::MenuState::SettingsEntry &&
        (snap->settings_category == NetMenu::SettingsCategory::GameGeneral ||
         snap->settings_category == NetMenu::SettingsCategory::GameVoice  ||
         snap->settings_category == NetMenu::SettingsCategory::GameRoot   ||
         snap->settings_category == NetMenu::SettingsCategory::GameKeys   ||
         snap->settings_category == NetMenu::SettingsCategory::GameSystem  ||
         snap->settings_category == NetMenu::SettingsCategory::GameHotkeys)) {
        if (snap->settings_category == NetMenu::SettingsCategory::GameVoice) {
            NetMenu::GameSettingsVoice_RenderScreen(snap->selected_index, alpha);
        } else if (snap->settings_category == NetMenu::SettingsCategory::GameRoot) {
            NetMenu::GameSettingsRoot_RenderScreen(snap->selected_index, alpha);
        } else if (snap->settings_category == NetMenu::SettingsCategory::GameKeys) {
            NetMenu::GameSettingsKeys_RenderScreen(snap->selected_index, alpha);
        } else if (snap->settings_category == NetMenu::SettingsCategory::GameSystem) {
            NetMenu::GameSettingsSystem_RenderScreen(snap->selected_index, alpha);
        } else if (snap->settings_category == NetMenu::SettingsCategory::GameHotkeys) {
            NetMenu::GameSettingsHotkeys_RenderScreen(snap->selected_index, alpha);
        } else {
            NetMenu::GameSettingsMenu_RenderScreen(snap->selected_index, alpha);
        }
        GameSetBlend(0, 255);
        GameSetDrawColor(255, 255, 255);
        return;
    }

    // Panel backing, matched to the key config screen exactly: two passes at
    // half alpha. Three passes at 150 went muddy.
    s_contentBottomCur = kRowStartY;
    const int footerTop = (s_contentBottomPrev > 0 ? s_contentBottomPrev : kFooterTop) + 10;
    const int panelBottom = footerTop + 2 * kFooterStep + 12 < kPanelBottom
                          ? footerTop + 2 * kFooterStep + 12
                          : kPanelBottom;
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 128));
    GameFillRect(kPanelLeft, kPanelTop, kPanelRight, panelBottom, 0, 0, 0);
    GameFillRect(kPanelLeft, kPanelTop, kPanelRight, panelBottom, 0, 0, 0);

    // Header shadow
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 40));
    GameFillRect(kContentLeft - 4, kPanelTop + 6, kContentRight + 4, kHeaderBottom + 2, 0, 0, 0);

    // Header
    GameSetBlend(1, alpha);
    GameDrawTextSized(kLabelX, kPanelTop + 6, 255, 255, 255, kTitleSize,
                      "%s", GetHeaderTitle(snap));
    GameDrawTextSized(kLabelX, kPanelTop + 40, 160, 160, 160, kNoteSize,
                      "%s", GetHeaderSubtitle(snap));
    RenderHeaderBadge(GetHeaderBadge(snap), alpha);

    // Status card
    int contentStartY = kRowStartY;
    RenderStatusCard(snap, alpha, fadeNorm, &contentStartY);

    // Content
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:           RenderMenuRoot(snap, alpha, contentStartY);         break;
        case NetMenu::MenuState::DirectConnectEntry: RenderDirectConnect(snap, alpha, contentStartY);    break;
        case NetMenu::MenuState::HostEntry:          RenderHostEntry(snap, alpha, contentStartY);        break;
        case NetMenu::MenuState::JoinEntry:          RenderJoinEntry(snap, alpha, contentStartY);        break;
        case NetMenu::MenuState::SpectateEntry:      RenderSpectateEntry(snap, alpha, contentStartY);    break;
        case NetMenu::MenuState::SpectatorConnecting:RenderSpectatorConnecting(snap, alpha, contentStartY); break;
        case NetMenu::MenuState::SpectatorConnected: RenderSpectatorConnected(snap, alpha, contentStartY); break;
        case NetMenu::MenuState::SettingsCategoryMenu: RenderSettingsCategoryMenu(snap, alpha, contentStartY); break;
        case NetMenu::MenuState::SettingsEntry:      RenderSettings(snap, alpha, contentStartY);         break;
        case NetMenu::MenuState::Connecting:
        case NetMenu::MenuState::Handshake:          RenderConnecting(snap, alpha, contentStartY);       break;
        case NetMenu::MenuState::ConnectedSession:   RenderConnectedSession(snap, alpha, contentStartY); break;
        case NetMenu::MenuState::CharSelTransition:  RenderCharSelTransition(snap, alpha, contentStartY);break;
        case NetMenu::MenuState::PostMatch:          RenderPostMatch(snap, alpha, contentStartY);        break;
        case NetMenu::MenuState::DisconnectError:    RenderDisconnectError(snap, alpha, contentStartY);  break;
        default: break;
    }

    RenderPromptOverlay(snap, alpha, fadeNorm);

    // Footer sits just under the content, the way the key config screen does,
    // rather than pinned to the bottom of the screen.
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 40));
    GameFillRect(kContentLeft - 4, footerTop - 2, kContentRight + 4, panelBottom - 2, 0, 0, 0);

    // Footer
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 180));

    // Context-sensitive hints (line 1)
    const char* hint1 = "A Select  B Back  Up/Down Move";
    if (snap->prompt_active) {
        hint1 = "A Choose  B Cancel  Left/Right Switch";
    } else {
        switch (snap->state) {
            case NetMenu::MenuState::HostEntry:
            case NetMenu::MenuState::Connecting:
            case NetMenu::MenuState::Handshake:
            case NetMenu::MenuState::ConnectedSession:
                if (snap->connecting_as_host || snap->is_host) {
                    hint1 = "A Choose  B Back  C Copy Address";
                }
                break;
            case NetMenu::MenuState::SettingsEntry:
                hint1 = "A Choose  B Back  Left/Right Adjust";
                break;
            default:
                hint1 = "A Choose  B Back  Up/Down Move";
                break;
        }
    }
    char clippedHint[96];
    ClipText(clippedHint, sizeof(clippedHint), hint1, kFooterChars);
    GameDrawTextSized(kLabelX, footerTop + 4, 150, 150, 150, kFootSize, "%s", clippedHint);

    // Only worth a second line when it says something the heading does not.
    if (snap->prompt_active) {
        GameDrawTextSized(kLabelX, footerTop + 4 + kFooterStep, 255, 255, 255,
                          kFootSize, "Decision Required");
    }

    s_contentBottomPrev = s_contentBottomCur;

    // Restore render state (blend + draw color) so the game's next frame
    // starts clean — matches what the old working code does.
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
}

} // namespace NetMenuUI
