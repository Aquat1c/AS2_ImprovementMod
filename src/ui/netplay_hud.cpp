/**
 * Netplay HUD — ImGui-based overlay for online match stats.
 *
 * Vanilla match HUD (decomp ~113076, mode 3/5) draws dword_816038 twice per side:
 * blend 3 @ alpha 0x80 (dark mask), then blend 2 @ alpha 0x80 (player RGB), then white text.
 * Bar spans screen edge through nickname at y=85..100 (15px). We mirror that layout with
 * configurable colors synced over the post-handshake PeerIdentity exchange.
 */

#include "ui/netplay_hud.h"
#include "rollback/stress_hooks.h"

#include "core/game_state.h"
#include "core/mod_main.h"
#include "net/connection_supervisor.h"
#include "net/delay_policy.h"
#include "patches/frame_scheduler.h"
#include "rollback/rollback_session.h"
#include "rollback/run_state.h"
#include "ui/mod_menu.h"
#include "ui/netplay_hud_style.h"

#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace {

constexpr float kNativeW = 640.0f;
constexpr float kNativeH = 480.0f;

// Vanilla match HUD bar height at y=85..100 (15px). Y position is configurable (see netplay_hud_style).
constexpr float kBarHeightRatio = 15.0f / kNativeH;
constexpr float kNickMarginXRatio = 2.0f / kNativeW;
constexpr float kBarPadH = 4.0f;

constexpr uint8_t kVanillaBarAlpha = 0x80;
constexpr uint8_t kVanillaMaskAlpha = 0x80;

constexpr float kStatsPadH = 3.0f;
constexpr float kStatsPadW = 6.0f;
constexpr float kStatsRounding = 3.0f;

constexpr ImU32 kShadowCol = IM_COL32(0, 0, 0, 160);
constexpr ImU32 kStatsBg = IM_COL32(0, 0, 0, 160);
constexpr ImU32 kStatsText = IM_COL32(255, 255, 255, 230);

static uint32_t s_cachedHudFrame = UINT32_MAX;
static uint32_t s_cachedHudMode = UINT32_MAX;
static uint32_t s_cachedHudSubstate = UINT32_MAX;
static uint32_t s_cachedHudGameType = UINT32_MAX;
static bool s_cachedHudValid = false;
static MatchHudData s_cachedHud = {};

static bool IsUtf8ContinuationByte(unsigned char value) {
    return (value & 0xC0) == 0x80;
}

static size_t Utf8PrefixBytes(const char* text, size_t codepointCount) {
    size_t offset = 0;
    size_t count = 0;
    while (text && text[offset] && count < codepointCount) {
        ++offset;
        while (text[offset] && IsUtf8ContinuationByte((unsigned char)text[offset])) {
            ++offset;
        }
        ++count;
    }
    return offset;
}

static size_t Utf8CountCodepoints(const char* text) {
    size_t count = 0;
    for (size_t offset = 0; text && text[offset]; ++offset) {
        if (!IsUtf8ContinuationByte((unsigned char)text[offset])) {
            ++count;
        }
    }
    return count;
}

static void ClipUtf8Text(char* out, size_t outCap, const char* text, size_t maxChars) {
    if (!out || outCap == 0) {
        return;
    }
    out[0] = '\0';
    if (!text || !text[0]) {
        return;
    }

    if (Utf8CountCodepoints(text) <= maxChars || maxChars < 4) {
        strncpy_s(out, outCap, text, _TRUNCATE);
        return;
    }
    const size_t prefixBytes = Utf8PrefixBytes(text, maxChars - 3);
    _snprintf_s(out, outCap, _TRUNCATE, "%.*s...", (int)prefixBytes, text);
}

static ImU32 MakeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return IM_COL32(r, g, b, a);
}

struct HudFontBinding {
    ImFont* font = nullptr;
    float sizePx = 16.0f;
};

static HudFontBinding ResolveHudFontBinding(uint8_t fontSizePreset) {
    HudFontBinding binding{};
    binding.sizePx = NetplayHudStyle::GetFontSizePxForPreset(fontSizePreset);
    binding.font = ImGui::GetFont();

    typedef void* (*GetNetplayHudFontFn)(int);
    static GetNetplayHudFontFn getHudFont = nullptr;
    static bool lookedUp = false;
    if (!lookedUp) {
        lookedUp = true;
        if (HMODULE proxy = GetModuleHandleA("d3d9.dll")) {
            getHudFont = (GetNetplayHudFontFn)GetProcAddress(proxy, "GetNetplayHudFont");
        }
    }
    if (getHudFont) {
        int presetIndex = (int)fontSizePreset;
        if (presetIndex < 0 || presetIndex > 2) {
            presetIndex = (int)NetplayHudStyle::HudFontSize::Large;
        }
        if (ImFont* hudFont = (ImFont*)getHudFont(presetIndex)) {
            binding.font = hudFont;
        }
    }
    return binding;
}

static ImVec2 CalcHudTextSize(const HudFontBinding& font, const char* text) {
    if (!font.font || !text) {
        return ImVec2(0.0f, 0.0f);
    }
    return font.font->CalcTextSizeA(font.sizePx, FLT_MAX, 0.0f, text);
}

static void DrawShadowedText(ImDrawList* dl,
                             const HudFontBinding& font,
                             const ImVec2& pos,
                             ImU32 col,
                             const char* text) {
    if (!dl || !font.font || !text) {
        return;
    }
    const float shadow = font.sizePx >= 14.0f ? 1.0f : 0.5f;
    dl->AddText(font.font, font.sizePx, ImVec2(pos.x + shadow, pos.y + shadow), kShadowCol, text);
    dl->AddText(font.font, font.sizePx, pos, col, text);
}

static void FormatPlayerLeftLabel(char* out, size_t outCap, const char* name, int wins) {
    if (!out || outCap == 0) {
        return;
    }
    const char* base = (name && name[0]) ? name : "Player";
    _snprintf_s(out, outCap, _TRUNCATE, "%s (%d)", base, wins);
}

// Vanilla draws dword_816038 twice: alpha mask pass, then tinted color pass.
static void DrawVanillaNicknameBar(ImDrawList* dl,
                                   float barLeft,
                                   float barRight,
                                   float barTop,
                                   float barBottom,
                                   uint8_t r,
                                   uint8_t g,
                                   uint8_t b,
                                   bool fadeAtLeft) {
    if (!dl || barRight <= barLeft || barBottom <= barTop) {
        return;
    }

    const ImU32 maskSolid = MakeColor(0, 0, 0, kVanillaMaskAlpha);
    const ImU32 maskFade = MakeColor(0, 0, 0, 0);
    if (fadeAtLeft) {
        dl->AddRectFilledMultiColor(
            ImVec2(barLeft, barTop),
            ImVec2(barRight, barBottom),
            maskFade, maskSolid, maskSolid, maskFade);
    } else {
        dl->AddRectFilledMultiColor(
            ImVec2(barLeft, barTop),
            ImVec2(barRight, barBottom),
            maskSolid, maskFade, maskFade, maskSolid);
    }

    const ImU32 colorSolid = MakeColor(r, g, b, kVanillaBarAlpha);
    const ImU32 colorFade = MakeColor(r, g, b, 0);
    if (fadeAtLeft) {
        dl->AddRectFilledMultiColor(
            ImVec2(barLeft, barTop),
            ImVec2(barRight, barBottom),
            colorFade, colorSolid, colorSolid, colorFade);
    } else {
        dl->AddRectFilledMultiColor(
            ImVec2(barLeft, barTop),
            ImVec2(barRight, barBottom),
            colorSolid, colorFade, colorFade, colorSolid);
    }
}

static void DrawNicknameBarBehindText(ImDrawList* dl,
                                      float textLeft,
                                      float textWidth,
                                      float textY,
                                      float textHeight,
                                      float barOuterX,
                                      bool alignRight,
                                      uint16_t barExtendPx,
                                      uint8_t barR,
                                      uint8_t barG,
                                      uint8_t barB) {
    const float textInnerPad = kBarPadH;
    const float barTop = textY;
    const float barBottom = textY + textHeight;

    if (alignRight) {
        const float barLeft = textLeft - textInnerPad;
        const float desiredRight = textLeft + textWidth + textInnerPad + (float)barExtendPx;
        const float barRight = (std::max)(desiredRight, barOuterX);
        DrawVanillaNicknameBar(dl, barLeft, barRight, barTop, barBottom, barR, barG, barB, false);
    } else {
        const float barRight = textLeft + textWidth + textInnerPad;
        const float desiredLeft = textLeft - textInnerPad - (float)barExtendPx;
        const float barLeft = (std::min)(desiredLeft, barOuterX);
        DrawVanillaNicknameBar(dl, barLeft, barRight, barTop, barBottom, barR, barG, barB, true);
    }
}

static void DrawPlayerSideLeft(ImDrawList* dl,
                               const HudFontBinding& font,
                               const char* name,
                               int wins,
                               uint8_t barR,
                               uint8_t barG,
                               uint8_t barB,
                               uint8_t textR,
                               uint8_t textG,
                               uint8_t textB,
                               float textAnchorX,
                               float barOuterX,
                               float y,
                               float barHeight,
                               uint16_t barExtendPx) {
    char label[96] = {};
    FormatPlayerLeftLabel(label, sizeof(label), name, wins);

    const ImVec2 textSize = CalcHudTextSize(font, label);
    const float textX = textAnchorX;
    const float textY = y;
    const float rowHeight = (std::max)(barHeight, textSize.y);

    DrawNicknameBarBehindText(dl,
                              textX,
                              textSize.x,
                              textY,
                              rowHeight,
                              barOuterX,
                              false,
                              barExtendPx,
                              barR,
                              barG,
                              barB);
    DrawShadowedText(dl, font, ImVec2(textX, textY), MakeColor(textR, textG, textB, 255), label);
}

static void DrawPlayerSideRight(ImDrawList* dl,
                                const HudFontBinding& font,
                                const char* name,
                                int wins,
                                uint8_t barR,
                                uint8_t barG,
                                uint8_t barB,
                                uint8_t textR,
                                uint8_t textG,
                                uint8_t textB,
                                uint8_t scoreR,
                                uint8_t scoreG,
                                uint8_t scoreB,
                                float textAnchorX,
                                float barOuterX,
                                float y,
                                float barHeight,
                                uint16_t barExtendPx) {
    char scorePart[16] = {};
    _snprintf_s(scorePart, sizeof(scorePart), _TRUNCATE, "(%d) ", wins);

    char namePart[96] = {};
    ClipUtf8Text(namePart, sizeof(namePart), name && name[0] ? name : "Player", 16);

    const ImVec2 scoreSize = CalcHudTextSize(font, scorePart);
    const ImVec2 nameSize = CalcHudTextSize(font, namePart);
    const float totalWidth = scoreSize.x + nameSize.x;
    const float blockLeft = textAnchorX - totalWidth;
    const float textY = y;
    const float rowHeight = (std::max)(barHeight, (std::max)(scoreSize.y, nameSize.y));

    DrawNicknameBarBehindText(dl,
                              blockLeft,
                              totalWidth,
                              textY,
                              rowHeight,
                              barOuterX,
                              true,
                              barExtendPx,
                              barR,
                              barG,
                              barB);

    DrawShadowedText(dl,
                     font,
                     ImVec2(blockLeft, textY),
                     MakeColor(scoreR, scoreG, scoreB, 255),
                     scorePart);
    DrawShadowedText(dl,
                     font,
                     ImVec2(blockLeft + scoreSize.x, textY),
                     MakeColor(textR, textG, textB, 255),
                     namePart);
}

static bool QueryActiveHud(MatchHudData* outHud) {
    const uint32_t frame = AS2_GetFrameNumber();
    const uint32_t mode = GetGameMode();
    const uint32_t substate = GetSubstate();
    const uint32_t gameType = GetGameType();
    if (s_cachedHudFrame != frame ||
        s_cachedHudMode != mode ||
        s_cachedHudSubstate != substate ||
        s_cachedHudGameType != gameType) {
        s_cachedHudFrame = frame;
        s_cachedHudMode = mode;
        s_cachedHudSubstate = substate;
        s_cachedHudGameType = gameType;
        s_cachedHudValid = ModGetMatchHudData(&s_cachedHud) && s_cachedHud.active;
        if (!s_cachedHudValid) {
            memset(&s_cachedHud, 0, sizeof(s_cachedHud));
        }
    }

    if (outHud) {
        *outHud = s_cachedHud;
    }
    return s_cachedHudValid;
}

} // namespace

bool NetplayHud_HasVisibleHud() {
    return QueryActiveHud(nullptr);
}

void NetplayHud_Render() {
    MatchHudData hud{};
    if (!QueryActiveHud(&hud)) {
        return;
    }

    // Use the shared overlay draw list so the mod menu stays on top when it's open.
    ImDrawList* dl = ModMenu_OverlayDrawList();
    if (!dl) {
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float W = display.x > 0.0f ? display.x : kNativeW;
    const float H = display.y > 0.0f ? display.y : kNativeH;
    const bool modMenuOpen = ModMenu_IsOpen();
    const bool drawOverlayNicknames =
        NetplayHudStyle::GetRenderMode() == NetplayHudStyle::HudRenderMode::ImGui;

    if (drawOverlayNicknames &&
        (GetGameMode() == MODE_MATCH ||
         GetGameMode() == MODE_CHARSEL ||
         GetGameMode() == MODE_WINSCREEN)) {
        const float nickY = H * NetplayHudStyle::GetNickYRatio(modMenuOpen);
        const HudFontBinding p1Font = ResolveHudFontBinding(hud.p1_font_size);
        const HudFontBinding p2Font = ResolveHudFontBinding(hud.p2_font_size);
        const float p1BarHeight = H * kBarHeightRatio * (p1Font.sizePx / 14.0f);
        const float p2BarHeight = H * kBarHeightRatio * (p2Font.sizePx / 14.0f);
        const float nickMargin = W * kNickMarginXRatio;

        DrawPlayerSideLeft(dl,
                           p1Font,
                           hud.p1_name,
                           hud.p1_wins,
                           hud.p1_trail_r,
                           hud.p1_trail_g,
                           hud.p1_trail_b,
                           hud.p1_text_r,
                           hud.p1_text_g,
                           hud.p1_text_b,
                           nickMargin,
                           0.0f,
                           nickY,
                           p1BarHeight,
                           hud.p1_trail_length_px);

        DrawPlayerSideRight(dl,
                            p2Font,
                            hud.p2_name,
                            hud.p2_wins,
                            hud.p2_trail_r,
                            hud.p2_trail_g,
                            hud.p2_trail_b,
                            hud.p2_text_r,
                            hud.p2_text_g,
                            hud.p2_text_b,
                            hud.p2_score_r,
                            hud.p2_score_g,
                            hud.p2_score_b,
                            W - nickMargin,
                            W,
                            nickY,
                            p2BarHeight,
                            hud.p2_trail_length_px);
    }

    // ── Progress-stall warning banner (M3 obligation, consumed at M6) ──
    // 8 s of zero canonical-frame progress while the transport still pings:
    // §2.4 progress deadline pre-warning before the 20 s teardown.
    if (Net::ConnectionSupervisor_IsProgressStallWarned()) {
        char warn[96] = {};
        snprintf(warn, sizeof(warn),
                 "Opponent's game stopped responding (%us)",
                 Net::ConnectionSupervisor_GetProgressStallMs() / 1000u);
        const ImVec2 wsz = ImGui::CalcTextSize(warn);
        const float warnW = wsz.x + kStatsPadW * 2.0f;
        const float warnH = wsz.y + kStatsPadH * 2.0f;
        const float warnX = (W - warnW) * 0.5f;
        const float warnY = H * 0.18f;
        dl->AddRectFilled(ImVec2(warnX, warnY),
                          ImVec2(warnX + warnW, warnY + warnH),
                          IM_COL32(96, 24, 24, 200), kStatsRounding);
        dl->AddText(ImVec2(warnX + kStatsPadW, warnY + kStatsPadH),
                    IM_COL32(255, 200, 120, 255), warn);
    }

    char stats[160] = {};
    if (hud.show_connection_stats) {
        // RB shows achieved/budget (2026-08-17): the first number is the
        // LIVE rollback depth (last transaction's replay length — reads 30
        // every frame in forced depth-30 mode), the second the configured
        // budget. The old single-number RB was the budget alone, which
        // reads "RB:0"-ish while deep rollback is demonstrably running.
        if (hud.ping_ms >= 0.0f) {
            snprintf(stats, sizeof(stats), "PING:%dms  D:%d  RB:%d/%d",
                     (int)(hud.ping_ms + 0.5f), hud.delay_frames,
                     hud.rollback_depth_now, hud.rollback_frames);
        } else {
            snprintf(stats, sizeof(stats), "PING:--  D:%d  RB:%d/%d",
                     hud.delay_frames,
                     hud.rollback_depth_now, hud.rollback_frames);
        }

        // Forced-rollback badge: when the stress config arms per-frame depth-N
        // rollback, say so on screen. "RB:30/30" alone is ambiguous — it looks
        // the same as a link that merely permits depth 30 — and the operator
        // has repeatedly (and correctly) refused to take the logs' word for it.
        // FORCE:N present == every frame is executing a depth-N restore+replay.
        if (const int forced = Rollback::StressHooks_GetForcedRollbackDepth()) {
            const size_t len = strlen(stats);
            snprintf(stats + len, sizeof(stats) - len, "  FORCE:%d", forced);
        }

        // Coverage badge (M6, INV-6): the delay-policy verdict is shown,
        // never silently corrected. FullSpeed draws nothing.
        const Net::CoverageClass cov =
            Net::DelayPolicy_ClassifyLocalCoverage(nullptr, nullptr);
        if (cov == Net::CoverageClass::Underbuffered ||
            cov == Net::CoverageClass::Marginal) {
            const size_t len = strlen(stats);
            snprintf(stats + len, sizeof(stats) - len, "  [%s]",
                     Net::CoverageClassName(cov));
        }

        // Hold-cause line (M6, §2.10 vocabulary replacing the NETCLASS/debt
        // readouts): live run state + peer readouts from PressureReport.
        if (Rollback::RollbackSession_IsActive()) {
            FrameSchedulerSnapshot sched{};
            FrameScheduler_GetSnapshot(&sched);
            Rollback::RollbackSessionSnapshot rb{};
            Rollback::RollbackSession_GetSnapshot(&rb);
            if (sched.run_state != Rollback::RunState::Running ||
                rb.peer_prediction_depth > 0) {
                const size_t len = strlen(stats);
                snprintf(stats + len, sizeof(stats) - len,
                         "  %s  peer-depth:%u",
                         Rollback::RunStateName(sched.run_state),
                         rb.peer_prediction_depth);
            }
        }
    } else if (hud.status_text[0]) {
        snprintf(stats, sizeof(stats), "%s", hud.status_text);
    }

    if (!stats[0]) {
        return;
    }

    const ImVec2 sz = ImGui::CalcTextSize(stats);
    const float statsBarW = sz.x + kStatsPadW * 2.0f;
    const float statsBarH = sz.y + kStatsPadH * 2.0f;
    const float statsBarX = (W - statsBarW) * 0.5f;
    const float statsBarY = H - statsBarH;

    dl->AddRectFilled(
        ImVec2(statsBarX, statsBarY),
        ImVec2(statsBarX + statsBarW, statsBarY + statsBarH),
        kStatsBg,
        kStatsRounding);

    dl->AddText(
        ImVec2(statsBarX + kStatsPadW, statsBarY + kStatsPadH),
        kStatsText,
        stats);
}
