/**
 * Netplay HUD — vanilla game render path (sub_4C05B0 / charsel helper).
 * Optional alternative to the ImGui overlay; draws nickname bars via the game's APIs.
 */

#include "ui/netplay_hud_vanilla.h"

#include "core/game_state.h"
#include "core/mod_main.h"
#include "ui/mod_menu.h"
#include "ui/netplay_hud_style.h"
#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

constexpr int kNativeW = 640;
constexpr int kNativeH = 480;
constexpr int kBarHeight = 15;
constexpr int kCharSelNickY = 368;
constexpr int kNickMarginX = 2;
constexpr int kBarPadH = 4;

constexpr uintptr_t kAddrRenderDrawTexQuad = 0x5D3060;
constexpr uintptr_t kAddrRenderSetBlend = 0x5D2F80;
constexpr uintptr_t kAddrRenderSetDrawColor = 0x5D3030;
constexpr uintptr_t kAddrDrawFormatString = 0x629A20;
constexpr uintptr_t kAddrGraphicsRgbFormat = 0x61FF60;

typedef int (__cdecl *RenderDrawTexturedQuad_t)(LONG x1,
                                                int y1,
                                                LONG x2,
                                                int y2,
                                                LONG x3,
                                                int y3,
                                                LONG x4,
                                                int y4,
                                                int textureHandle,
                                                int renderFlags);
typedef int (__cdecl *RenderSetBlendMode_t)(int blendMode, unsigned char alphaValue);
typedef int (__cdecl *RenderSetDrawColor_t)(unsigned char r, unsigned char g, unsigned char b);
typedef unsigned int (__cdecl *GraphicsConvertRGB_t)(unsigned char r,
                                                     unsigned char g,
                                                     unsigned char b);
typedef int (__cdecl *DrawFormatString_t)(int x, int y, unsigned int color, char* fmt, ...);
typedef __int16 (__cdecl *MatchHudRender_t)(int game, int match);

static MatchHudRender_t s_origMatchHudRender = nullptr;

static int GetBarTextureHandle() {
    __try {
        return *(volatile int*)ADDR_NAME_BAR_TEXTURE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static unsigned int GameColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((GraphicsConvertRGB_t)kAddrGraphicsRgbFormat)(r, g, b);
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

static void ClipUtf8Text(char* out, size_t outCap, const char* text, size_t maxChars) {
    if (!out || outCap == 0) {
        return;
    }
    out[0] = '\0';
    if (!text || !text[0]) {
        return;
    }

    size_t count = 0;
    size_t prefixBytes = 0;
    for (size_t offset = 0; text[offset]; ) {
        if (count >= maxChars) {
            break;
        }
        ++count;
        ++offset;
        while (text[offset] && ((unsigned char)text[offset] & 0xC0) == 0x80) {
            ++offset;
        }
        prefixBytes = offset;
    }

    if (count <= maxChars && text[prefixBytes] == '\0') {
        strncpy_s(out, outCap, text, _TRUNCATE);
        return;
    }
    if (maxChars < 4) {
        strncpy_s(out, outCap, text, _TRUNCATE);
        return;
    }
    _snprintf_s(out, outCap, _TRUNCATE, "%.*s...", (int)prefixBytes, text);
}

static int SideNickY(bool charSel) {
    if (charSel) {
        return kCharSelNickY;
    }
    const float ratio = NetplayHudStyle::GetNickYRatio(ModMenu_IsOpen());
    return (int)(ratio * (float)kNativeH + 0.5f);
}

static void DrawBarLeftSide(int barRight, int top, int bottom, uint8_t r, uint8_t g, uint8_t b) {
    const int tex = GetBarTextureHandle();
    if (tex <= 0 || barRight <= 0) {
        return;
    }

    ((RenderSetBlendMode_t)kAddrRenderSetBlend)(3, 0x80);
    ((RenderDrawTexturedQuad_t)kAddrRenderDrawTexQuad)(
        barRight, top, barRight, bottom, 0, bottom, 0, top, tex, 1);
    ((RenderSetBlendMode_t)kAddrRenderSetBlend)(2, 0x80);
    ((RenderSetDrawColor_t)kAddrRenderSetDrawColor)(r, g, b);
    ((RenderDrawTexturedQuad_t)kAddrRenderDrawTexQuad)(
        barRight, top, barRight, bottom, 0, bottom, 0, top, tex, 1);
    ((RenderSetDrawColor_t)kAddrRenderSetDrawColor)(0xFF, 0xFF, 0xFF);
    ((RenderSetBlendMode_t)kAddrRenderSetBlend)(0, 0xFF);
}

static void DrawBarRightSide(int barLeft, int top, int bottom, uint8_t r, uint8_t g, uint8_t b) {
    const int tex = GetBarTextureHandle();
    if (tex <= 0 || barLeft >= kNativeW - 1) {
        return;
    }

    ((RenderSetBlendMode_t)kAddrRenderSetBlend)(3, 0x80);
    ((RenderDrawTexturedQuad_t)kAddrRenderDrawTexQuad)(
        barLeft, top, barLeft, bottom, kNativeW - 1, bottom, kNativeW - 1, top, tex, 1);
    ((RenderSetBlendMode_t)kAddrRenderSetBlend)(2, 0x80);
    ((RenderSetDrawColor_t)kAddrRenderSetDrawColor)(r, g, b);
    ((RenderDrawTexturedQuad_t)kAddrRenderDrawTexQuad)(
        barLeft, top, barLeft, bottom, kNativeW - 1, bottom, kNativeW - 1, top, tex, 1);
    ((RenderSetDrawColor_t)kAddrRenderSetDrawColor)(0xFF, 0xFF, 0xFF);
    ((RenderSetBlendMode_t)kAddrRenderSetBlend)(0, 0xFF);
}

static void DrawGameText(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* text) {
    if (!text || !text[0]) {
        return;
    }
    const std::string gameText = Utf8ToGameText(text);
    const char* drawText = gameText.empty() ? text : gameText.c_str();
    ((DrawFormatString_t)kAddrDrawFormatString)(
        x, y, GameColor(r, g, b), (char*)"%s", (char*)drawText);
}

static int EstimateTextWidthPx(const char* text) {
    return (int)strlen(text) * 12;
}

static void DrawPlayerLeft(const MatchHudData& hud, int y) {
    char label[96] = {};
    const char* base = hud.p1_name[0] ? hud.p1_name : "Player";
    _snprintf_s(label, sizeof(label), _TRUNCATE, "%s (%d)", base, hud.p1_wins);

    const int textX = kNickMarginX;
    const int textWidth = EstimateTextWidthPx(label);
    const int barRight = (std::min)(kNativeW - 1, textX + textWidth + kBarPadH);
    const int barLeftTarget = textX - kBarPadH - (int)hud.p1_trail_length_px;
    (void)barLeftTarget;

    DrawBarLeftSide(barRight, y, y + kBarHeight, hud.p1_trail_r, hud.p1_trail_g, hud.p1_trail_b);
    DrawGameText(textX, y, hud.p1_text_r, hud.p1_text_g, hud.p1_text_b, label);
}

static void DrawPlayerRight(const MatchHudData& hud, int y) {
    char scorePart[16] = {};
    char namePart[96] = {};
    _snprintf_s(scorePart, sizeof(scorePart), _TRUNCATE, "(%d) ", hud.p2_wins);
    ClipUtf8Text(namePart, sizeof(namePart), hud.p2_name[0] ? hud.p2_name : "Player", 16);

    const int scoreWidth = EstimateTextWidthPx(scorePart);
    const int nameWidth = EstimateTextWidthPx(namePart);
    const int totalWidth = scoreWidth + nameWidth;
    const int blockLeft = kNativeW - kNickMarginX - totalWidth;
    const int barLeft = (std::max)(0, blockLeft - kBarPadH - (int)hud.p2_trail_length_px);

    DrawBarRightSide(barLeft, y, y + kBarHeight, hud.p2_trail_r, hud.p2_trail_g, hud.p2_trail_b);
    DrawGameText(blockLeft, y, hud.p2_score_r, hud.p2_score_g, hud.p2_score_b, scorePart);
    DrawGameText(blockLeft + scoreWidth, y, hud.p2_text_r, hud.p2_text_g, hud.p2_text_b, namePart);
}

static bool QueryHud(MatchHudData* out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    return ModGetMatchHudData(out) && out->active;
}

static void DrawNicknamesForMode(bool charSel) {
    MatchHudData hud{};
    if (!QueryHud(&hud)) {
        return;
    }
    DrawPlayerLeft(hud, SideNickY(charSel));
    DrawPlayerRight(hud, SideNickY(charSel));
}

static __int16 __cdecl Hook_MatchHudRender(int game, int match) {
    const __int16 result = s_origMatchHudRender ? s_origMatchHudRender(game, match) : 0;
    if (NetplayHudStyle::GetRenderMode() != NetplayHudStyle::HudRenderMode::Vanilla) {
        return result;
    }

    MatchHudData hud{};
    if (!QueryHud(&hud)) {
        return result;
    }

    DrawNicknamesForMode(false);
    return result;
}

} // namespace

bool NetplayHudVanilla_InstallHooks() {
    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_MATCH_HUD_RENDER),
        reinterpret_cast<void*>(&Hook_MatchHudRender),
        reinterpret_cast<void**>(&s_origMatchHudRender));
    if (status != MH_OK) {
        LOG_ERROR("[NetplayHudVanilla] Failed to hook sub_4C05B0! Status: %d", status);
        return false;
    }
    LOG_INFO("[NetplayHudVanilla] Installed match HUD hook @ 0x%08X", ADDR_MATCH_HUD_RENDER);
    return true;
}

void NetplayHudVanilla_RenderMatch(int game, int match) {
    (void)game;
    (void)match;
    DrawNicknamesForMode(false);
}

void NetplayHudVanilla_RenderCharSel() {
    DrawNicknamesForMode(true);
}
