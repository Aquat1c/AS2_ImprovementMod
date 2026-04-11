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
// Layout constants
// ============================================================================

constexpr int kPanelLeft   = 56;
constexpr int kPanelTop    = 46;
constexpr int kPanelRight  = 584;
constexpr int kPanelBottom = 438;
constexpr int kRowLeft     = 72;
constexpr int kRowRight    = 568;
constexpr int kLabelX      = 80;
constexpr int kValueX      = 250;
constexpr int kHeaderBottom = 88;
constexpr int kStatusY     = 100;
constexpr int kRowStartY   = 128;
constexpr int kRowStep     = 26;
constexpr int kFooterTop   = 392;
constexpr int kFooterStep  = 16;
constexpr int kFadeFrames  = 25;
constexpr int kInfoStep    = 22;
constexpr int kContentBottom = kFooterTop - 6;

constexpr size_t kRowLabelChars    = 18;
constexpr size_t kRowValueChars    = 34;
constexpr size_t kInfoLabelChars   = 16;
constexpr size_t kInfoValueChars   = 32;
constexpr size_t kStatusChars      = 54;
constexpr size_t kFooterChars      = 54;
constexpr size_t kErrorChars       = 54;

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

static void GameDrawText(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    ((DrawFormatString_t)ADDR_DRAW_FORMAT_STRING)(x, y, (unsigned int)GameCreateColor(r, g, b), (char*)"%s", buf);
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

    const size_t len = strlen(normalized);
    if (len <= maxChars || maxChars < 4) {
        strncpy_s(out, outCap, normalized, _TRUNCATE);
        return;
    }
    _snprintf_s(out, outCap, _TRUNCATE, "%.*s...", (int)(maxChars - 3), normalized);
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

static bool HasRowSpace(int y) {
    return y >= kRowStartY && (y + 18) <= kContentBottom;
}

static bool HasInfoSpace(int y) {
    return y >= kRowStartY && (y + 14) <= kContentBottom;
}

// ============================================================================
// Row rendering
// ============================================================================

static void RenderRow(int y, const char* label, const char* value, bool selected, bool enabled, uint8_t alpha) {
    if (!HasRowSpace(y)) return;

    if (selected) {
        GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 196));
        GameFillRect(kRowLeft, y - 3, kRowRight, y + 18, enabled ? 140 : 70, enabled ? 28 : 44, enabled ? 28 : 58);
        GameFillRect(kRowLeft, y - 3, kRowLeft + 4, y + 18, 255, 225, 96);
    }

    GameSetBlend(1, alpha);
    char clippedLabel[64];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kRowLabelChars);
    GameDrawText(kLabelX, y, enabled ? 255 : 188, enabled ? 255 : 192, enabled ? 255 : 204, "%s", clippedLabel);
    if (value && value[0]) {
        char clippedValue[160];
        ClipText(clippedValue, sizeof(clippedValue), value, kRowValueChars);
        GameDrawText(kValueX, y, enabled ? 196 : 160, enabled ? 220 : 166, enabled ? 255 : 180, "%s", clippedValue);
    }
}

// ============================================================================
// Info line rendering (non-selectable, for display data)
// ============================================================================

static void RenderInfoLine(int y, const char* label, const char* value, uint8_t alpha) {
    if (!HasInfoSpace(y)) return;

    GameSetBlend(1, alpha);
    char clippedLabel[64];
    ClipText(clippedLabel, sizeof(clippedLabel), label, kInfoLabelChars);
    GameDrawText(kLabelX, y, 180, 188, 202, "%s", clippedLabel);
    if (value && value[0]) {
        char clippedValue[160];
        ClipText(clippedValue, sizeof(clippedValue), value, kInfoValueChars);
        GameDrawText(kValueX, y, 230, 234, 242, "%s", clippedValue);
    }
}

static const char* ConnectModeLabel(int mode) {
    switch (mode) {
        case 0: return "Auto D->R";
        case 1: return "Direct";
        case 2: return "Relay";
        default: return "Unknown";
    }
}

// ============================================================================
// Menu page rendering
// ============================================================================

static void RenderMenuRoot(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Direct Play", "Host / Join", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Settings",    "Config",      snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Close Menu",  nullptr,       snap->selected_index == 2, true, alpha);
}

static void RenderDirectConnect(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Host",  "Wait for peer", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Join",  "Dial remote",   snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",  "Root",          snap->selected_index == 2, true, alpha);
}

static void RenderHostEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Start Host",  nullptr,  snap->selected_index == 0, true, alpha); y += kRowStep;

    // Listen Port: show text edit buffer if editing, else the current value
    char portVal[16];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::ListenPort) {
        FormatEditBufferWithCursor(portVal, sizeof(portVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(portVal, sizeof(portVal), _TRUNCATE, "%u", snap->listen_port);
    }
    RenderRow(y, "Listen Port", portVal, snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",        nullptr,  snap->selected_index == 2, true, alpha);
    y += kRowStep + 8;

    // Show your address for sharing
    char addrBuf[144];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Your Address", addrBuf, alpha);
    y += kInfoStep;
    RenderInfoLine(y, "NAT", snap->nat_status, alpha);
}

static void RenderJoinEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Join Host",        nullptr,           snap->selected_index == 0, true, alpha); y += kRowStep;

    // Remote Endpoint: show text edit buffer if editing, else the current value
    char endpointVal[128];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::RemoteEndpoint) {
        FormatEditBufferWithCursor(endpointVal, sizeof(endpointVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(endpointVal, sizeof(endpointVal), _TRUNCATE, "%s", snap->remote_endpoint);
    }
    RenderRow(y, "Remote Endpoint", endpointVal,  snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",            nullptr,       snap->selected_index == 2, true, alpha);
    y += kRowStep + 8;
    RenderInfoLine(y, "NAT", snap->nat_status, alpha);
}

static void RenderSettings(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    constexpr int kSettingsStep = 18;
    int y = kRowStartY;

    char nickVal[64];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::Nickname) {
        FormatEditBufferWithCursor(nickVal, sizeof(nickVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(nickVal, sizeof(nickVal), _TRUNCATE, "%s", snap->local_nickname);
    }
    RenderRow(y, "Nickname", nickVal, snap->selected_index == 0, true, alpha); y += kSettingsStep;

    char delayVal[24];
    _snprintf_s(delayVal, sizeof(delayVal), _TRUNCATE, "< %d >", snap->preferred_delay);
    RenderRow(y, "Input delay", delayVal, snap->selected_index == 1, true, alpha); y += kSettingsStep;

    char rbVal[24];
    _snprintf_s(rbVal, sizeof(rbVal), _TRUNCATE, "< %d >", snap->rollback_budget);
    RenderRow(y, "Max rollback", rbVal, snap->selected_index == 2, true, alpha); y += kSettingsStep;

    char tolVal[24];
    _snprintf_s(tolVal, sizeof(tolVal), _TRUNCATE, "< %d >", snap->rollback_tolerance);
    RenderRow(y, "Recommendation bias", tolVal, snap->selected_index == 3, true, alpha); y += kSettingsStep;

    char modeVal[24];
    _snprintf_s(modeVal, sizeof(modeVal), _TRUNCATE, "< %s >", ConnectModeLabel(snap->connection_mode));
    RenderRow(y, "Connect Mode", modeVal, snap->selected_index == 4, true, alpha); y += kSettingsStep;

    RenderRow(y, "UPnP",        snap->upnp_enabled ? "< On >" : "< Off >", snap->selected_index == 5, true, alpha); y += kSettingsStep;
    RenderRow(y, "STUN",        snap->stun_enabled ? "< On >" : "< Off >", snap->selected_index == 6, true, alpha); y += kSettingsStep;
    RenderRow(y, "Hole Punch",  snap->hole_punch_enabled ? "< On >" : "< Off >", snap->selected_index == 7, true, alpha); y += kSettingsStep;
    RenderRow(y, "IPv6 Parse",  snap->allow_ipv6_endpoint ? "< On >" : "< Off >", snap->selected_index == 8, true, alpha); y += kSettingsStep;

    char relayVal[120];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::RelayEndpoint) {
        FormatEditBufferWithCursor(relayVal, sizeof(relayVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else if (snap->relay_endpoint[0]) {
        _snprintf_s(relayVal, sizeof(relayVal), _TRUNCATE, "%s", snap->relay_endpoint);
    } else {
        _snprintf_s(relayVal, sizeof(relayVal), _TRUNCATE, "(none)");
    }
    RenderRow(y, "Relay Endpoint", relayVal, snap->selected_index == 9, true, alpha); y += kSettingsStep;

    char stunVal[120];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::StunEndpoint) {
        FormatEditBufferWithCursor(stunVal, sizeof(stunVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(stunVal, sizeof(stunVal), _TRUNCATE, "%s", snap->stun_endpoint);
    }
    RenderRow(y, "STUN Server", stunVal, snap->selected_index == 10, true, alpha); y += kSettingsStep;

    RenderRow(y, "Back", nullptr, snap->selected_index == 11, true, alpha);

    RenderInfoLine(kFooterTop - kInfoStep, "NAT", snap->nat_status, alpha);
}

static void RenderConnecting(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Cancel", "Stop", snap->selected_index == 0, true, alpha);
    y += kRowStep + 8;

    // Show your address for sharing
    char addrBuf[144];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Your Address", addrBuf, alpha);
    y += kInfoStep;

    if (snap->rtt_ms > 0.0f) {
        char pingBuf[32];
        _snprintf_s(pingBuf, sizeof(pingBuf), _TRUNCATE, "%.0f ms", snap->rtt_ms);
        RenderInfoLine(y, "Ping", pingBuf, alpha);
        y += kInfoStep;
    }
    RenderInfoLine(y, "NAT", snap->nat_status, alpha);
}

static void RenderConnectedSession(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;

    // Row 0: Rollback Frames (adjustable left/right)
    {
        char rbVal[24];
        _snprintf_s(rbVal, sizeof(rbVal), _TRUNCATE, "< %d >", snap->rollback_budget);
        RenderRow(y, "Max rollback", rbVal, snap->selected_index == 0, true, alpha);
        y += kRowStep;
    }

    // Row 1: Input delay (adjustable left/right)
    {
        char delVal[24];
        _snprintf_s(delVal, sizeof(delVal), _TRUNCATE, "< %d >", snap->preferred_delay);
        RenderRow(y, "Input delay", delVal, snap->selected_index == 1, true, alpha);
        y += kRowStep;
    }

    // Row 2: Accept Match
    {
        const char* label = snap->local_accepted ? "Accepted" : "Accept Match";
        const char* hint  = snap->local_accepted ? "Waiting..." : "Enter";
        RenderRow(y, label, hint, snap->selected_index == 2, !snap->local_accepted, alpha);
        y += kRowStep;
    }

    // Row 3: Decline
    RenderRow(y, "Decline", "Leave", snap->selected_index == 3, true, alpha);
    y += kRowStep + 8;

    // Info section: role, identity, accept status, ping, recommended delay, score, address

    // Role
    RenderInfoLine(y, "Role", snap->is_host ? "Host" : "Client", alpha);
    y += kInfoStep;

    // Local identity
    if (snap->local_nickname[0]) {
        RenderInfoLine(y, "You", snap->local_nickname, alpha);
        y += kInfoStep;
    }
    // Remote identity
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Peer", snap->peer_nickname, alpha);
        y += kInfoStep;
    }
    // Accept status
    {
        char acceptBuf[64];
        _snprintf_s(acceptBuf, sizeof(acceptBuf), _TRUNCATE, "%s / %s",
            snap->local_accepted  ? "You: Ready" : "You: Pending",
            snap->remote_accepted ? "Peer: Ready" : "Peer: Pending");
        RenderInfoLine(y, "Status", acceptBuf, alpha);
        y += kInfoStep;
    }
    // Ping / recommendations
    if (snap->rtt_ms > 0.0f) {
        char pingBuf[72];
        _snprintf_s(pingBuf, sizeof(pingBuf), _TRUNCATE, "%.0f ms  rec D:%d RB:%d",
            snap->rtt_ms, snap->recommended_delay, snap->recommended_max_rollback);
        RenderInfoLine(y, "Ping", pingBuf, alpha);
        y += kInfoStep;
    }
    if (snap->stall_threshold > 0) {
        char stallBuf[72];
        _snprintf_s(stallBuf, sizeof(stallBuf), _TRUNCATE, "%d%s",
            snap->stall_threshold,
            snap->stall_warning ? "  (risk)" : "");
        RenderInfoLine(y, "Stall Limit", stallBuf, alpha);
        y += kInfoStep;
    }
    // Score
    if (snap->local_wins > 0 || snap->remote_wins > 0) {
        char scoreBuf[48];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->local_wins, snap->remote_wins);
        RenderInfoLine(y, "Score", scoreBuf, alpha);
        y += kInfoStep;
    }
    // Show your address for sharing
    char addrBuf[144];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Your Address", addrBuf, alpha);
}

static void RenderCharSelTransition(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Launch", "CharSel", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Back",   "Session", snap->selected_index == 1, true, alpha);
    y += kRowStep + 8;

    if (snap->local_nickname[0]) {
        RenderInfoLine(y, "You", snap->local_nickname, alpha);
        y += kInfoStep;
    }
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Peer", snap->peer_nickname, alpha);
        y += kInfoStep;
    }
    // Show set score if any matches have been played
    if (snap->local_wins > 0 || snap->remote_wins > 0) {
        char scoreBuf[48];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->local_wins, snap->remote_wins);
        RenderInfoLine(y, "Score", scoreBuf, alpha);
    }
}

static void RenderPostMatch(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Rematch",    "CharSel", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Return",     "Session", snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Disconnect", "Leave",   snap->selected_index == 2, true, alpha);
    y += kRowStep + 8;

    if (snap->local_nickname[0]) {
        RenderInfoLine(y, "You", snap->local_nickname, alpha);
        y += kInfoStep;
    }
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Peer", snap->peer_nickname, alpha);
        y += kInfoStep;
    }
    // Show set score
    {
        char scoreBuf[48];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->local_wins, snap->remote_wins);
        RenderInfoLine(y, "Score", scoreBuf, alpha);
    }
}

static void RenderDisconnectError(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    // Show error text
    if (snap->last_error[0]) {
        GameSetBlend(1, alpha);
        char clippedErr[160];
        ClipText(clippedErr, sizeof(clippedErr), snap->last_error, kErrorChars);
        GameDrawText(kLabelX, y, 255, 128, 128, "%s", clippedErr);
        y += kRowStep;
    }
    y += kRowStep;
    RenderRow(y, "OK",         "Clear", snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Close Menu", "Close", snap->selected_index == 1, true, alpha);
}

// ============================================================================
// State name for header
// ============================================================================

static const char* GetHeaderTitle(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::SettingsEntry:
        case NetMenu::MenuState::SettingsCategoryMenu:
            return "Settings";
        default:
            return "Netplay";
    }
}

static const char* GetHeaderSubtitle(const NetMenu::MenuSnapshot* snap) {
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:            return "Root";
        case NetMenu::MenuState::DirectConnectEntry:  return "Direct Connect";
        case NetMenu::MenuState::HostEntry:           return "Host";
        case NetMenu::MenuState::JoinEntry:           return "Join";
        case NetMenu::MenuState::SettingsCategoryMenu: return "Categories";
        case NetMenu::MenuState::SettingsEntry:       return "Settings";
        case NetMenu::MenuState::Connecting:          return "Connecting...";
        case NetMenu::MenuState::Handshake:           return "Handshake...";
        case NetMenu::MenuState::ConnectedSession:    return "Accept Match";
        case NetMenu::MenuState::CharSelTransition:   return "Character Select";
        case NetMenu::MenuState::PostMatch:           return "Post Match";
        case NetMenu::MenuState::DisconnectError:     return "Disconnected";
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

    // Always draw the vanilla background sprite at full opacity with white
    // draw color, exactly as the old working code and vanilla case 3 do.
    const int bgHandle = (int)ReadU32(ADDR_TITLE_MENU_BG_ACTIVE, 0);
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
    GameDrawSprite(0, 0, bgHandle);

    // Calculate fade alpha for overlay elements
    float fadeNorm = (float)snap->fade_frames / (float)kFadeFrames;
    if (fadeNorm < 0.0f) fadeNorm = 0.0f;
    if (fadeNorm > 1.0f) fadeNorm = 1.0f;
    uint8_t alpha = (uint8_t)Alpha8(fadeNorm, 255);
    if (!alpha) return;

    // Full-screen dark overlay between background and panel
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 72));
    GameFillRect(0, 0, 639, 479, 0, 0, 0);

    // Background panel
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 210));
    GameFillRect(kPanelLeft, kPanelTop, kPanelRight, kPanelBottom, 16, 18, 28);
    // Border
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 255));
    GameFillRect(kPanelLeft, kPanelTop, kPanelRight, kPanelTop + 2, 64, 100, 180);
    GameFillRect(kPanelLeft, kPanelBottom - 2, kPanelRight, kPanelBottom, 64, 100, 180);
    GameFillRect(kPanelLeft, kPanelTop, kPanelLeft + 2, kPanelBottom, 64, 100, 180);
    GameFillRect(kPanelRight - 2, kPanelTop, kPanelRight, kPanelBottom, 64, 100, 180);

    // Header
    GameSetBlend(1, alpha);
    GameDrawText(kLabelX, kPanelTop + 10, 255, 255, 255, "%s", GetHeaderTitle(snap));
    GameDrawText(kLabelX, kPanelTop + 30, 160, 180, 220, "%s", GetHeaderSubtitle(snap));

    // Header separator
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 128));
    GameFillRect(kRowLeft, kHeaderBottom, kRowRight, kHeaderBottom + 1, 80, 100, 140);

    // Status line
    if (snap->status[0]) {
        GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 200));
        char clippedStatus[160];
        ClipText(clippedStatus, sizeof(clippedStatus), snap->status, kStatusChars);
        GameDrawText(kLabelX, kStatusY, 180, 200, 230, "%s", clippedStatus);
    }

    // Content
    switch (snap->state) {
        case NetMenu::MenuState::MenuRoot:           RenderMenuRoot(snap, alpha);         break;
        case NetMenu::MenuState::DirectConnectEntry: RenderDirectConnect(snap, alpha);    break;
        case NetMenu::MenuState::HostEntry:          RenderHostEntry(snap, alpha);        break;
        case NetMenu::MenuState::JoinEntry:          RenderJoinEntry(snap, alpha);        break;
        case NetMenu::MenuState::SettingsEntry:      RenderSettings(snap, alpha);         break;
        case NetMenu::MenuState::Connecting:
        case NetMenu::MenuState::Handshake:          RenderConnecting(snap, alpha);       break;
        case NetMenu::MenuState::ConnectedSession:   RenderConnectedSession(snap, alpha); break;
        case NetMenu::MenuState::CharSelTransition:  RenderCharSelTransition(snap, alpha);break;
        case NetMenu::MenuState::PostMatch:          RenderPostMatch(snap, alpha);        break;
        case NetMenu::MenuState::DisconnectError:    RenderDisconnectError(snap, alpha);  break;
        default: break;
    }

    // Footer hints
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 160));
    GameFillRect(kRowLeft, kFooterTop, kRowRight, kFooterTop + 1, 80, 100, 140);
    GameSetBlend(1, (uint8_t)Alpha8(fadeNorm, 180));

    // Context-sensitive hints (line 1)
    const char* hint1 = "A=Select  B=Back  Up/Down=Navigate";
    switch (snap->state) {
        case NetMenu::MenuState::HostEntry:
        case NetMenu::MenuState::Connecting:
        case NetMenu::MenuState::Handshake:
        case NetMenu::MenuState::ConnectedSession:
            hint1 = "A=Select  B=Back  C=Copy Address";
            break;
        case NetMenu::MenuState::SettingsEntry:
            hint1 = "A=Select  B=Back  Left/Right=Adjust";
            break;
        default: break;
    }
    char clippedHint[96];
    ClipText(clippedHint, sizeof(clippedHint), hint1, kFooterChars);
    GameDrawText(kLabelX, kFooterTop + 6, 160, 168, 190,  "%s", clippedHint);

    char footerLine[96];
    _snprintf_s(footerLine, sizeof(footerLine), _TRUNCATE,
        "AS2 Rollback | %s", NetMenu::MenuStateName(snap->state));
    char clippedFooter[96];
    ClipText(clippedFooter, sizeof(clippedFooter), footerLine, kFooterChars);
    GameDrawText(kLabelX, kFooterTop + 6 + kFooterStep, 220, 224, 236, "%s", clippedFooter);

    // Restore render state (blend + draw color) so the game's next frame
    // starts clean — matches what the old working code does.
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
}

} // namespace NetMenuUI
