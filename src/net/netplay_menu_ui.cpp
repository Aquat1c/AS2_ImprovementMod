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

// ============================================================================
// Game render function addresses
// ============================================================================

namespace {

constexpr uintptr_t ADDR_RENDER_FILL_RECT   = 0x5D2F50;
constexpr uintptr_t ADDR_RENDER_SET_BLEND    = 0x5D2F80;
constexpr uintptr_t ADDR_RENDER_CREATE_COLOR = 0x5D3150;
constexpr uintptr_t ADDR_DRAW_FORMAT_STRING  = 0x629A20;

typedef int (__cdecl *RenderFillRect_t)(int left, int top, int right, int bottom, int color, int drawFlag);
typedef int (__cdecl *RenderSetBlendMode_t)(int blendMode, unsigned __int8 alphaValue);
typedef int (__cdecl *RenderCreateColor_t)(unsigned __int8 r, unsigned __int8 g, unsigned __int8 b);
typedef int (__cdecl *DrawFormatString_t)(int x, int y, unsigned int color, char* fmt, ...);

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

static void GameSetBlend(int mode, uint8_t alpha) {
    ((RenderSetBlendMode_t)ADDR_RENDER_SET_BLEND)(mode, alpha);
}

static void GameFillRect(int l, int t, int r, int b, uint8_t cr, uint8_t cg, uint8_t cb) {
    ((RenderFillRect_t)ADDR_RENDER_FILL_RECT)(l, t, r, b, GameCreateColor(cr, cg, cb), 1);
}

static void GameDrawText(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    ((DrawFormatString_t)ADDR_DRAW_FORMAT_STRING)(x, y, (unsigned int)GameCreateColor(r, g, b), (char*)"%s", buf);
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
// Menu page rendering
// ============================================================================

static void RenderMenuRoot(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Direct Play", nullptr, snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Settings",    nullptr, snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Close Menu",  nullptr, snap->selected_index == 2, true, alpha);
}

static void RenderDirectConnect(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Host",  nullptr, snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Join",  nullptr, snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",  nullptr, snap->selected_index == 2, true, alpha);
}

static void RenderHostEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Start Host",  nullptr,  snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Listen Port", "10700",  snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",        nullptr,  snap->selected_index == 2, true, alpha);
}

static void RenderJoinEntry(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Join Host",        nullptr,           snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Remote Endpoint", "127.0.0.1:10700",  snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",            nullptr,             snap->selected_index == 2, true, alpha);
}

static void RenderSettings(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Input Delay", "0",    snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Verbose Log", "Off",  snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Back",        nullptr, snap->selected_index == 2, true, alpha);
}

static void RenderConnecting(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Cancel", nullptr, snap->selected_index == 0, true, alpha);
}

static void RenderConnectedSession(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Launch CharSel", nullptr, snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Disconnect",     nullptr, snap->selected_index == 1, true, alpha);
}

static void RenderCharSelTransition(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Launch", nullptr, snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Back",   nullptr, snap->selected_index == 1, true, alpha);
}

static void RenderPostMatch(const NetMenu::MenuSnapshot* snap, uint8_t alpha) {
    int y = kRowStartY;
    RenderRow(y, "Rematch",    nullptr, snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Return",     nullptr, snap->selected_index == 1, true, alpha); y += kRowStep;
    RenderRow(y, "Disconnect", nullptr, snap->selected_index == 2, true, alpha);
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
    RenderRow(y, "OK",         nullptr, snap->selected_index == 0, true, alpha); y += kRowStep;
    RenderRow(y, "Close Menu", nullptr, snap->selected_index == 1, true, alpha);
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
        case NetMenu::MenuState::ConnectedSession:    return "Session Ready";
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

    float fadeNorm = (float)snap->fade_frames / (float)kFadeFrames;
    if (fadeNorm < 0.0f) fadeNorm = 0.0f;
    if (fadeNorm > 1.0f) fadeNorm = 1.0f;
    uint8_t alpha = (uint8_t)Alpha8(fadeNorm, 255);
    if (alpha < 8) return;

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
    GameDrawText(kLabelX, kFooterTop + 6, 160, 168, 190, "A=Select  B=Back  Up/Down=Navigate");

    // Restore blend mode
    GameSetBlend(0, 255);
}

} // namespace NetMenuUI
