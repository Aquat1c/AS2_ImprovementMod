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
constexpr int kValueX      = 268;
constexpr int kHeaderBottom = 88;
constexpr int kStatusY     = 100;
constexpr int kRowStartY   = 128;
constexpr int kRowStep     = 26;
constexpr int kFooterTop   = 358;
constexpr int kFooterStep  = 16;
constexpr int kFadeFrames  = 25;

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

// ============================================================================
// Row rendering
// ============================================================================

static void RenderRow(int y, const char* label, const char* value, bool selected, bool enabled, uint8_t alpha) {
    if (selected) {
        GameSetBlend(1, (uint8_t)Alpha8((float)alpha / 255.0f, 196));
        GameFillRect(kRowLeft, y - 3, kRowRight, y + 18, enabled ? 140 : 70, enabled ? 28 : 44, enabled ? 28 : 58);
        GameFillRect(kRowLeft, y - 3, kRowLeft + 4, y + 18, 255, 225, 96);
    }

    GameSetBlend(1, alpha);
    GameDrawText(kLabelX, y, enabled ? 255 : 188, enabled ? 255 : 192, enabled ? 255 : 204, "%s", label ? label : "");
    if (value && value[0]) {
        GameDrawText(kValueX, y, enabled ? 196 : 160, enabled ? 220 : 166, enabled ? 255 : 180, "%s", value);
    }
}

// ============================================================================
// Info line rendering (non-selectable, for display data)
// ============================================================================

static void RenderInfoLine(int y, const char* label, const char* value, uint8_t alpha) {
    GameSetBlend(1, alpha);
    GameDrawText(kLabelX, y, 180, 188, 202, "%s", label ? label : "");
    if (value && value[0]) {
        GameDrawText(kValueX, y, 230, 234, 242, "%s", value);
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
    char addrBuf[80];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Your Address", addrBuf, alpha);
}

static void RenderJoinEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Join Host",        nullptr,           snap->selected_index == 0, true, alpha); y += kRowStep;

    // Remote Endpoint: show text edit buffer if editing, else the current value
    char endpointVal[72];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::RemoteEndpoint) {
        FormatEditBufferWithCursor(endpointVal, sizeof(endpointVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(endpointVal, sizeof(endpointVal), _TRUNCATE, "%s", snap->remote_endpoint);
    }
    RenderRow(y, "Remote Endpoint", endpointVal,  snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",            nullptr,       snap->selected_index == 2, true, alpha);
}

static void RenderSettings(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;

    // Nickname: show text edit buffer if editing, else current value
    char nickVal[40];
    if (snap->is_text_editing && snap->text_edit_field == NetMenu::TextEditField::Nickname) {
        FormatEditBufferWithCursor(nickVal, sizeof(nickVal), snap->text_edit_buffer, snap->text_cursor_pos);
    } else {
        _snprintf_s(nickVal, sizeof(nickVal), _TRUNCATE, "%s", snap->local_nickname);
    }
    RenderRow(y, "Nickname",    nickVal,  snap->selected_index == 0, true, alpha); y += kRowStep;

    // Input Delay: show current value with left/right hint
    char delayVal[16];
    _snprintf_s(delayVal, sizeof(delayVal), _TRUNCATE, "< %d >", snap->preferred_delay);
    RenderRow(y, "Input Delay", delayVal, snap->selected_index == 1, true, alpha); y += kRowStep;

    // Verbose Log (placeholder for now)
    RenderRow(y, "Verbose Log", "Off",    snap->selected_index == 2, true, alpha); y += kRowStep;

    RenderRow(y, "Back",        nullptr,  snap->selected_index == 3, true, alpha);
}

static void RenderConnecting(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Cancel", "Stop", snap->selected_index == 0, true, alpha);
    y += kRowStep + 8;

    // Show your address for sharing
    char addrBuf[80];
    if (snap->clipboard_flash[0]) {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s  (%s)", snap->your_address, snap->clipboard_flash);
    } else {
        _snprintf_s(addrBuf, sizeof(addrBuf), _TRUNCATE, "%s", snap->your_address);
    }
    RenderInfoLine(y, "Your Address", addrBuf, alpha);

    if (snap->rtt_ms > 0.0f) {
        y += 22;
        char pingBuf[32];
        _snprintf_s(pingBuf, sizeof(pingBuf), _TRUNCATE, "%.0f ms", snap->rtt_ms);
        RenderInfoLine(y, "Ping", pingBuf, alpha);
    }
}

static void RenderConnectedSession(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;

    // Row 0: Rollback Frames (adjustable left/right)
    {
        char rbVal[24];
        _snprintf_s(rbVal, sizeof(rbVal), _TRUNCATE, "< %d >", snap->rollback_budget);
        RenderRow(y, "Rollback Frames", rbVal, snap->selected_index == 0, true, alpha);
        y += kRowStep;
    }

    // Row 1: Input Delay (adjustable left/right)
    {
        char delVal[24];
        _snprintf_s(delVal, sizeof(delVal), _TRUNCATE, "< %d >", snap->rollback_delay);
        RenderRow(y, "Input Delay", delVal, snap->selected_index == 1, true, alpha);
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
    y += 22;

    // Local identity
    if (snap->local_nickname[0]) {
        RenderInfoLine(y, "You", snap->local_nickname, alpha);
        y += 22;
    }
    // Remote identity
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Peer", snap->peer_nickname, alpha);
        y += 22;
    }
    // Accept status
    {
        char acceptBuf[64];
        _snprintf_s(acceptBuf, sizeof(acceptBuf), _TRUNCATE, "%s / %s",
            snap->local_accepted  ? "You: Ready" : "You: Pending",
            snap->remote_accepted ? "Peer: Ready" : "Peer: Pending");
        RenderInfoLine(y, "Status", acceptBuf, alpha);
        y += 22;
    }
    // Ping
    if (snap->rtt_ms > 0.0f) {
        char pingBuf[48];
        _snprintf_s(pingBuf, sizeof(pingBuf), _TRUNCATE, "%.0f ms  (rec. delay: %d)",
            snap->rtt_ms, snap->recommended_delay);
        RenderInfoLine(y, "Ping", pingBuf, alpha);
        y += 22;
    }
    // Score
    if (snap->local_wins > 0 || snap->remote_wins > 0) {
        char scoreBuf[48];
        _snprintf_s(scoreBuf, sizeof(scoreBuf), _TRUNCATE, "%d - %d",
            snap->local_wins, snap->remote_wins);
        RenderInfoLine(y, "Score", scoreBuf, alpha);
        y += 22;
    }
    // Show your address for sharing
    char addrBuf[80];
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
        y += 22;
    }
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Peer", snap->peer_nickname, alpha);
        y += 22;
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
        y += 22;
    }
    if (snap->peer_nickname[0]) {
        RenderInfoLine(y, "Peer", snap->peer_nickname, alpha);
        y += 22;
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
        GameDrawText(kLabelX, y, 255, 128, 128, "%s", snap->last_error);
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
        GameDrawText(kLabelX, kStatusY, 180, 200, 230, "%s", snap->status);
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
    GameDrawText(kLabelX, kFooterTop + 6, 160, 168, 190,  "%s", hint1);
    GameDrawText(kLabelX, kFooterTop + 6 + kFooterStep, 220, 224, 236,
        "AS2 Rollback | %s", NetMenu::MenuStateName(snap->state));

    // Restore render state (blend + draw color) so the game's next frame
    // starts clean — matches what the old working code does.
    GameSetBlend(0, 255);
    GameSetDrawColor(255, 255, 255);
}

} // namespace NetMenuUI
