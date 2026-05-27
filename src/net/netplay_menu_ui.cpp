/**
 * Alice Senki 2 - Netplay Menu UI
 *
 * In-game rendering using the game's native DXLib-based render primitives.
 * All drawing goes through function pointers to the game's own render API.
 */

#include "net/netplay_menu_ui.h"
#include "net/netplay_menu_state.h"

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

typedef int (__cdecl *RenderFillRect_t)(int left, int top, int right, int bottom, int color, int drawFlag);
typedef int (__cdecl *RenderSetBlendMode_t)(int blendMode, unsigned __int8 alphaValue);
typedef int (__cdecl *RenderCreateColor_t)(unsigned __int8 r, unsigned __int8 g, unsigned __int8 b);
typedef int (__cdecl *RenderDrawSprite_t)(int x, int y, int spriteHandle, int transFlag);
typedef int (__cdecl *DrawFormatString_t)(int x, int y, unsigned int color, char* fmt, ...);
typedef int (__cdecl *RenderSetDrawColor_t)(unsigned __int8 r, unsigned __int8 g, unsigned __int8 b);

// ============================================================================
// Layout constants — 640x480, font 8px/char half-width, 16px tall
// ============================================================================

constexpr int kPanelLeft    = 44;
constexpr int kPanelTop     = 28;
constexpr int kPanelRight   = 596;
constexpr int kPanelBottom  = 452;
constexpr int kContentLeft  = 58;
constexpr int kContentRight = 582;
constexpr int kLabelX       = 66;
constexpr int kValueX       = 218;
constexpr int kHeaderBottom = 78;
constexpr int kRowStartY    = 86;
constexpr int kRowStep      = 26;
constexpr int kInfoStep     = 20;
constexpr int kFooterTop    = 418;
constexpr int kFooterStep   = 16;
constexpr int kFadeFrames   = 25;
constexpr int kContentBottom = kFooterTop - 6;

constexpr size_t kLabelChars  = 18;   // (218-66)/8 - 1
constexpr size_t kValueChars  = 44;   // (582-218)/8 - 1
constexpr size_t kFullChars   = 63;   // (582-66)/8 - 1
constexpr size_t kFooterChars = 63;

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

static void GameSetBlend(int mode, uint8_t alpha) {
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

static void GameDrawText(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* fmt, ...) {
    char utf8Buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(utf8Buf, sizeof(utf8Buf), _TRUNCATE, fmt, args);
    va_end(args);

    const std::string gameText = Utf8ToGameText(utf8Buf);
    const char* drawText = gameText.empty() ? utf8Buf : gameText.c_str();
    ((DrawFormatString_t)ADDR_DRAW_FORMAT_STRING)(x, y, (unsigned int)GameCreateColor(r, g, b), (char*)"%s", (char*)drawText);
}

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

static bool HasRowSpace(int y) {
    return y >= kHeaderBottom && (y + 20) <= kContentBottom;
}

static bool HasInfoSpace(int y) {
    return y >= kHeaderBottom && (y + 16) <= kContentBottom;
}

static void RenderSectionLabel(int y, const char* label, uint8_t alpha) {
    if (!HasInfoSpace(y)) return;

    char clippedLabel[64];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kLabelChars);
    GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 180));
    GameDrawText(kLabelX, y, 180, 160, 130, "%s", clippedLabel);
}

static const char* GetHeaderBadge(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:
            return "ONLINE";
        case NetMenu::MenuState::SpectateEntry:
        case NetMenu::MenuState::SpectatorConnecting:
        case NetMenu::MenuState::SpectatorConnected:
            return "WATCH";
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

    const int width = (int)strlen(label) * 8;
    const int x = kContentRight - width;
    GameSetBlend(1, alpha);
    GameDrawText(x, kPanelTop + 12, 200, 180, 160, "%s", label);
}

static const char* GetStateSummary(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::SpectatorConnecting:
            return "Reaching the watch server...";
        case NetMenu::MenuState::SpectatorConnected:
            return (snap->spectator_client_match_id != 0 || snap->spectator_playback_active)
                ? "Watching a live match."
                : "Waiting for a match to start.";
        case NetMenu::MenuState::Connecting:
        case NetMenu::MenuState::Handshake:
            return "Connecting to the room...";
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
        case NetMenu::MenuState::SpectatorConnecting:
        case NetMenu::MenuState::SpectatorConnected:
            return "Watch a Match";
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
    if (snap->status[0]) return true;
    return GetStateSummary(snap) != nullptr;
}

static void RenderStatusCard(const NetMenu::MenuSnapshot* snap, uint8_t alpha, float fadeNorm, int* outBottom) {
    const char* summary = GetStateSummary(snap);
    const char* primary = snap->status[0] ? snap->status : summary;
    if (!primary || !primary[0]) {
        if (outBottom) *outBottom = kRowStartY;
        return;
    }

    const int cardTop = kHeaderBottom + 6;
    const int cardBottom = cardTop + 20;
    if (outBottom) *outBottom = cardBottom + 6;

    char primaryText[192] = {};
    ClipText(primaryText, sizeof(primaryText), primary, kFullChars);

    GameSetBlend(1, alpha);
    GameDrawText(kLabelX, cardTop + 2, 220, 190, 140, "%s", primaryText);
}

// ============================================================================
// Row rendering
// ============================================================================

static void RenderRow(int y, const char* label, const char* value, bool selected, bool enabled, uint8_t alpha) {
    if (!HasRowSpace(y)) return;

    if (selected) {
        GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 40));
        GameFillRect(kContentLeft - 2, y - 6, kContentRight + 2, y + 22, 0, 0, 0);
        GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 128));
        GameFillRect(kContentLeft, y - 4, kContentRight, y + 20,
            enabled ? 180 : 120, enabled ? 60 : 70, enabled ? 50 : 70);
    } else {
        GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 22));
        GameFillRect(kContentLeft - 2, y - 6, kContentRight + 2, y + 22, 0, 0, 0);
    }

    GameSetBlend(1, alpha);
    char clippedLabel[80];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kLabelChars);
    GameDrawText(kLabelX, y,
        selected ? (enabled ? 255 : 200) : (enabled ? 236 : 168),
        selected ? (enabled ? 248 : 200) : (enabled ? 228 : 168),
        selected ? (enabled ? 240 : 204) : (enabled ? 216 : 172),
        "%s", clippedLabel);
    if (value && value[0]) {
        char clippedValue[192];
        ClipText(clippedValue, sizeof(clippedValue), value, kValueChars);
        GameDrawText(kValueX, y,
            selected ? (enabled ? 210 : 164) : (enabled ? 168 : 140),
            selected ? (enabled ? 210 : 164) : (enabled ? 192 : 144),
            selected ? (enabled ? 220 : 172) : (enabled ? 208 : 152),
            "%s", clippedValue);
    }
}

// ============================================================================
// Info line rendering (non-selectable, for display data)
// ============================================================================

static void RenderInfoLine(int y, const char* label, const char* value, uint8_t alpha) {
    if (!HasInfoSpace(y)) return;

    GameSetBlend(1, alpha);
    char clippedLabel[80];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kLabelChars);
    GameDrawText(kLabelX, y, 148, 140, 128, "%s", clippedLabel);
    if (value && value[0]) {
        char clippedValue[192];
        ClipText(clippedValue, sizeof(clippedValue), value, kValueChars);
        GameDrawText(kValueX, y, 210, 206, 200, "%s", clippedValue);
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
    GameDrawText(modalLeft + 28, modalTop + 20, 255, 242, 224, "%s", clippedTitle);

    char wrappedLines[3][96] = {};
    const int lineCount = WrapTextLines(
        snap->prompt_body,
        wrappedLines,
        3,
        40);
    int bodyY = modalTop + 54;
    for (int index = 0; index < lineCount; index++) {
        GameDrawText(modalLeft + 28, bodyY, 214, 208, 200, "%s", wrappedLines[index]);
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
    RenderRow(y, "Play Online", "Host or join", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Watch", "Spectate a room", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Settings", "Name and routing", snap->selected_index == 2, true, alpha); y += kRowStep;
    RenderRow(y, "Close", "Return to game", snap->selected_index == 3, true, alpha);
}

static void RenderDirectConnect(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Host", "Open a room", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Join", "Connect to a host", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back", "Online menu", snap->selected_index == 2, true, alpha);
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
    RenderRow(y, "Back", "Play Online", snap->selected_index == 2, true, alpha);
    y += kRowStep + 6;

    RenderSectionLabel(y, "Share This Room", alpha);
    y += 16;
    char addrBuf[144];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Address", addrBuf, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Route", snap->nat_route_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Mapping", snap->nat_mapping_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Punch", snap->nat_punch_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "STUN", snap->nat_stun_status, alpha);
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
    RenderRow(y, "Back", "Play Online", snap->selected_index == 2, true, alpha);
    y += kRowStep + 6;
    RenderSectionLabel(y, "Connection", alpha);
    y += 16;
    RenderInfoLine(y, "Route", snap->nat_route_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Mapping", snap->nat_mapping_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Punch", snap->nat_punch_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "STUN", snap->nat_stun_status, alpha);
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
    y += kRowStep + 6;

    RenderSectionLabel(y, "Details", alpha);
    y += 16;
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
    RenderRow(y, "Network", "Routing and servers", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Watch", "Spectator and palettes", snap->selected_index == 2, true, alpha); y += kRowStep;
    RenderRow(y, "Diagnostics", "Debug logs", snap->selected_index == 3, true, alpha); y += kRowStep;
    RenderRow(y, "Back", "Online menu", snap->selected_index == 4, true, alpha);
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

        char tolVal[48];
        _snprintf_s(tolVal, sizeof(tolVal), _TRUNCATE, "< %d > smoothness bias", snap->rollback_tolerance);
        RenderRow(y, "Stability Bias", tolVal, snap->selected_index == 3, true, alpha); y += kRowStep;

        const bool perPlayerDelayMode = snap->gameplay_delay_mode == 1;
        RenderRow(y,
            "Delay mode",
            perPlayerDelayMode ? "< Per-player > default" : "< Shared max >",
            snap->selected_index == 4,
            true,
            alpha);
        y += kRowStep;

        if (!perPlayerDelayMode) {
            RenderInfoLine(y, "Shared", "Both peers use the higher delay", alpha);
            y += kInfoStep;
        }

        RenderRow(y, "Back", "Settings", snap->selected_index == 5, true, alpha);
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

        char spectatorPortVal[16];
        if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::SpectatorPort) {
            FormatEditBufferWithCursor(spectatorPortVal, sizeof(spectatorPortVal), snap->text_edit_buffer, snap->text_cursor_pos);
        } else {
            _snprintf_s(spectatorPortVal, sizeof(spectatorPortVal), _TRUNCATE, "%u", snap->spectator_listen_port);
        }
        RenderRow(y, "Watch Port", spectatorPortVal, snap->selected_index == 1, true, alpha); y += kRowStep;

        RenderRow(y, "Sync Palettes", snap->palette_sync_enabled ? "< On > share colors" : "< Off > share colors", snap->selected_index == 2, true, alpha); y += kRowStep;
        RenderRow(y, "Preview Remote", snap->remote_palette_preview_enabled ? "< On > see opponent" : "< Off > see opponent", snap->selected_index == 3, true, alpha); y += kRowStep;

        RenderRow(y, "Back", "Settings", snap->selected_index == 4, true, alpha);
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
    y += kRowStep + 6;

    RenderSectionLabel(y, "Details", alpha);
    y += 16;

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
        char playersBuf[160];
        _snprintf_s(playersBuf, sizeof(playersBuf), _TRUNCATE, "%s vs %s",
            snap->spectator_p1_name[0] ? snap->spectator_p1_name : "P1",
            snap->spectator_p2_name[0] ? snap->spectator_p2_name : "P2");
        RenderInfoLine(y, "Players", playersBuf, alpha);
        y += kInfoStep;
    }
    RenderInfoLine(y, "Status", snap->spectator_client_status, alpha);
}

static void RenderSpectatorConnected(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Stop Watching", "Return to menu", snap->selected_index == 0, true, alpha);
    y += kRowStep + 6;

    RenderSectionLabel(y, "Match", alpha);
    y += 16;

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
        char playersBuf[160];
        _snprintf_s(playersBuf, sizeof(playersBuf), _TRUNCATE, "%s vs %s",
            snap->spectator_p1_name[0] ? snap->spectator_p1_name : "P1",
            snap->spectator_p2_name[0] ? snap->spectator_p2_name : "P2");
        RenderInfoLine(y, "Players", playersBuf, alpha);
        y += kInfoStep;

        char scoreBuf[32];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->spectator_p1_wins,
            snap->spectator_p2_wins);
        RenderInfoLine(y, "Set Score", scoreBuf, alpha);
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
    y += kRowStep + 6;

    RenderSectionLabel(y, "Details", alpha);
    y += 16;

    char addrBuf[144];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Address", addrBuf, alpha);
    y += kInfoStep;

    if (snap->rtt_ms > 0.0f) {
        char pingBuf[32];
        _snprintf_s(pingBuf, sizeof(pingBuf), _TRUNCATE, "%.0f ms", snap->rtt_ms);
        RenderInfoLine(y, "Ping", pingBuf, alpha);
        y += kInfoStep;
    }
    {
        char fpsBuf[96];
        FormatFrameTimingInfo(snap, fpsBuf, sizeof(fpsBuf));
        RenderInfoLine(y, "FPS", fpsBuf, alpha);
        y += kInfoStep;
    }
    RenderInfoLine(y, "Route", snap->nat_route_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Mapping", snap->nat_mapping_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Punch", snap->nat_punch_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "STUN", snap->nat_stun_status, alpha);
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

    // Local identity
    if (snap->local_nickname[0]) {
        RenderInfoLine(y, "You", snap->local_nickname, alpha);
        y += kInfoStep;
    }
    // Remote identity
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Opponent", snap->peer_nickname, alpha);
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
        char pingBuf[72];
        _snprintf_s(pingBuf, sizeof(pingBuf), _TRUNCATE, "%.0fms  delay %d  rb %d",
            snap->rtt_ms, snap->recommended_delay, snap->recommended_max_rollback);
        RenderInfoLine(y, "Ping", pingBuf, alpha);
        y += kInfoStep;
    }
    if (snap->stall_threshold > 0) {
        char stallBuf[72];
        _snprintf_s(stallBuf, sizeof(stallBuf), _TRUNCATE, "%d frame%s%s",
            snap->stall_threshold,
            snap->stall_threshold == 1 ? "" : "s",
            snap->stall_warning ? " (warning)" : "");
        RenderInfoLine(y, "Stall", stallBuf, alpha);
        y += kInfoStep;
    }
    RenderInfoLine(y,
        "Delay Mode",
        snap->gameplay_delay_mode == 1 ? "Per-player" : "Shared max",
        alpha);
    y += kInfoStep;
    {
        char fpsBuf[96];
        FormatFrameTimingInfo(snap, fpsBuf, sizeof(fpsBuf));
        RenderInfoLine(y, "FPS", fpsBuf, alpha);
        y += kInfoStep;
    }
    if (snap->current_rounds_label[0]) {
        RenderInfoLine(y, "Rounds", snap->current_rounds_label, alpha);
        y += kInfoStep;
    }
    // Score
    if (snap->local_wins > 0 || snap->remote_wins > 0) {
        char scoreBuf[48];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->local_wins, snap->remote_wins);
        RenderInfoLine(y, "Set Score", scoreBuf, alpha);
        y += kInfoStep;
    }
    // Show your address for sharing
    char addrBuf[144];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Address", addrBuf, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "Punch", snap->nat_punch_status, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "STUN", snap->nat_stun_status, alpha);
}

static void RenderCharSelTransition(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Char Select", "Start next game", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Back to Room",   "Stay connected", snap->selected_index == 1, true, alpha);
    y += kRowStep + 8;

    if (snap->local_nickname[0]) {
        RenderInfoLine(y, "You", snap->local_nickname, alpha);
        y += kInfoStep;
    }
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Opponent", snap->peer_nickname, alpha);
        y += kInfoStep;
    }
    // Show set score if any matches have been played
    if (snap->local_wins > 0 || snap->remote_wins > 0) {
        char scoreBuf[48];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->local_wins, snap->remote_wins);
        RenderInfoLine(y, "Set Score", scoreBuf, alpha);
    }
}

static void RenderPostMatch(const NetMenu::MenuSnapshot* snap, uint8_t alpha, int startY) {
    int y = startY;
    RenderRow(y, "Play Again",    "Character select", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Back to Room",  "Stay connected", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Disconnect",    "Leave the room",   snap->selected_index == 2, true, alpha);
    y += kRowStep + 8;

    if (snap->local_nickname[0]) {
        RenderInfoLine(y, "You", snap->local_nickname, alpha);
        y += kInfoStep;
    }
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Opponent", snap->peer_nickname, alpha);
        y += kInfoStep;
    }
    // Show set score
    {
        char scoreBuf[48];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->local_wins, snap->remote_wins);
        RenderInfoLine(y, "Set Score", scoreBuf, alpha);
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
        case NetMenu::MenuState::SpectatorConnecting:
        case NetMenu::MenuState::SpectatorConnected:
            return "Watch a Match";
        case NetMenu::MenuState::SettingsCategoryMenu:
            return "Settings";
        case NetMenu::MenuState::SettingsEntry:
            switch (snap->settings_category) {
                case NetMenu::SettingsCategory::Identity:    return "Player";
                case NetMenu::SettingsCategory::Endpoint:    return "Network";
                case NetMenu::SettingsCategory::SessionMatch: return "Watch";
                case NetMenu::SettingsCategory::Diagnostics:  return "Diagnostics";
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
        case NetMenu::MenuState::SpectatorConnecting: return "Connecting";
        case NetMenu::MenuState::SpectatorConnected:
            return (snap->spectator_client_match_id != 0 || snap->spectator_playback_active)
                ? "Watching live"
                : "Waiting for a match";
        case NetMenu::MenuState::SettingsCategoryMenu: return "Choose a category";
        case NetMenu::MenuState::SettingsEntry:
            switch (snap->settings_category) {
                case NetMenu::SettingsCategory::Identity:    return "Name and gameplay tuning";
                case NetMenu::SettingsCategory::Endpoint:    return "Routing and server addresses";
                case NetMenu::SettingsCategory::SessionMatch: return "Spectator and palette options";
                case NetMenu::SettingsCategory::Diagnostics:  return "Logging controls";
                default: return "Adjust settings";
            }
        case NetMenu::MenuState::Connecting:          return "Connecting to room";
        case NetMenu::MenuState::Handshake:           return "Confirming session";
        case NetMenu::MenuState::ConnectedSession:    return "Ready check";
        case NetMenu::MenuState::CharSelTransition:   return "Open character select";
        case NetMenu::MenuState::PostMatch:           return "Choose the next step";
        case NetMenu::MenuState::DisconnectError:     return "Review the message";
        default:                                      return "";
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace NetMenuUI {

void Render(const NetMenu::MenuSnapshot* snap) {
    if (!snap || !snap->menu_active) return;

    // Background sprite
    const int bgHandle = (int)ReadU32(ADDR_TITLE_MENU_BG_ACTIVE, 0);
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
    GameDrawSprite(0, 0, bgHandle);

    float fadeNorm = (float)snap->fade_frames / (float)kFadeFrames;
    if (fadeNorm < 0.0f) fadeNorm = 0.0f;
    if (fadeNorm > 1.0f) fadeNorm = 1.0f;
    uint8_t alpha = (uint8_t)Alpha8(fadeNorm, 255);
    if (!alpha) return;

    // Light dim overlay for readability
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 100));
    GameFillRect(0, 0, 639, 479, 0, 0, 0);

    // Header shadow
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 40));
    GameFillRect(kContentLeft - 4, kPanelTop + 6, kContentRight + 4, kHeaderBottom + 2, 0, 0, 0);

    // Header
    GameSetBlend(1, alpha);
    GameDrawText(kLabelX, kPanelTop + 10, 248, 238, 220, "%s", GetHeaderTitle(snap));
    GameDrawText(kLabelX, kPanelTop + 32, 168, 152, 128, "%s", GetHeaderSubtitle(snap));
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

    // Footer shadow
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 40));
    GameFillRect(kContentLeft - 4, kFooterTop - 2, kContentRight + 4, kPanelBottom + 8, 0, 0, 0);

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
                hint1 = "A Choose  B Back  C Copy Address";
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
    GameDrawText(kLabelX, kFooterTop + 6, 176, 168, 156,  "%s", clippedHint);

    char footerLine[96];
    if (snap->prompt_active) {
        _snprintf_s(footerLine, sizeof(footerLine), _TRUNCATE,
            "Decision Required");
    } else {
        _snprintf_s(footerLine, sizeof(footerLine), _TRUNCATE,
            "%s", GetFooterLabel(snap));
    }
    char clippedFooter[96];
    ClipText(clippedFooter, sizeof(clippedFooter), footerLine, kFooterChars);
    GameDrawText(kLabelX, kFooterTop + 6 + kFooterStep, 238, 228, 212, "%s", clippedFooter);

    // Restore render state (blend + draw color) so the game's next frame
    // starts clean — matches what the old working code does.
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
}

} // namespace NetMenuUI
