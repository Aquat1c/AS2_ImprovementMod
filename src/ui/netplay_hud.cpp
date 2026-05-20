/**
 * Netplay HUD — ImGui-based overlay for online match stats.
 *
 * Renders player nicknames in compact pills below the HP bar area (top)
 * and connection stats (ping, delay, rollback) at the bottom using
 * ImGui foreground draw list.
 *
 * Game HUD layout reference (640x480):
 *   y ≈ 0-65   : Character portraits + HP bars + guard gauge + win dots
 *   y ≈ 65-78  : Character name labels (game-rendered, e.g. "SHIZUKA")
 *   y ≈ 78+    : Free space — nickname pills go here
 *   y ≈ 430-480: Super meter gauge area
 */

#include "ui/netplay_hud.h"
#include "core/game_state.h"
#include "core/mod_main.h"

#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// ── Layout constants (ratios relative to native 640x480) ───────────────
constexpr float kNativeW = 640.0f;
constexpr float kNativeH = 480.0f;

// Nickname pills sit just below the game's character name labels.
// Expressed as fractions of the native resolution so they scale with any
// display size (borderless fullscreen, resolution scaling, etc.).
constexpr float kNickYRatio     = 78.0f  / kNativeH;
constexpr float kNickPadH       = 3.0f;   // vertical padding inside pill
constexpr float kNickPadW       = 6.0f;   // horizontal padding inside pill
constexpr float kNickMarginXRatio = 2.0f / kNativeW;  // distance from screen edge
constexpr float kNickRounding   = 3.0f;   // pill corner radius

// Bottom stats bar
constexpr float kStatsPadH      = 3.0f;
constexpr float kStatsPadW      = 6.0f;
constexpr float kStatsRounding  = 3.0f;

// ── Colours ─────────────────────────────────────────────────────────────
constexpr ImU32 kP1Bg           = IM_COL32( 20,  60, 120, 180);
constexpr ImU32 kP2Bg           = IM_COL32(120,  20,  30, 180);
constexpr ImU32 kNickText       = IM_COL32(255, 255, 255, 240);
constexpr ImU32 kShadowCol      = IM_COL32(  0,   0,   0, 160);
constexpr ImU32 kStatsBg        = IM_COL32(  0,   0,   0, 160);
constexpr ImU32 kStatsText      = IM_COL32(255, 255, 255, 230);

static uint32_t s_cachedHudFrame = UINT32_MAX;
static uint32_t s_cachedHudMode = UINT32_MAX;
static uint32_t s_cachedHudSubstate = UINT32_MAX;
static uint32_t s_cachedHudGameType = UINT32_MAX;
static bool s_cachedHudValid = false;
static MatchHudData s_cachedHud = {};

// ── UTF-8 helpers ───────────────────────────────────────────────────────

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
    if (!out || outCap == 0) return;
    out[0] = '\0';
    if (!text || !text[0]) return;

    if (Utf8CountCodepoints(text) <= maxChars || maxChars < 4) {
        strncpy_s(out, outCap, text, _TRUNCATE);
        return;
    }
    const size_t prefixBytes = Utf8PrefixBytes(text, maxChars - 3);
    _snprintf_s(out, outCap, _TRUNCATE, "%.*s...", (int)prefixBytes, text);
}

// ── Draw helper: shadowed text ──────────────────────────────────────────

static void DrawShadowedText(ImDrawList* dl, const ImVec2& pos, ImU32 col, const char* text) {
    dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), kShadowCol, text);
    dl->AddText(pos, col, text);
}

// ── Draw helper: nickname pill ──────────────────────────────────────────

static void DrawNickPill(ImDrawList* dl, const char* name, ImU32 bgCol,
                         float anchorX, float y, bool alignRight) {
    char clipped[96] = {};
    ClipUtf8Text(clipped, sizeof(clipped), name && name[0] ? name : "Player", 16);

    ImVec2 tsz = ImGui::CalcTextSize(clipped);
    float pillW = tsz.x + kNickPadW * 2.0f;
    float pillH = tsz.y + kNickPadH * 2.0f;

    float x = alignRight ? (anchorX - pillW) : anchorX;

    // Pill background
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + pillW, y + pillH), bgCol, kNickRounding);

    // Nickname text (shadowed)
    DrawShadowedText(dl, ImVec2(x + kNickPadW, y + kNickPadH), kNickText, clipped);
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

// ============================================================================
// Rendering
// ============================================================================

bool NetplayHud_HasVisibleHud() {
    return QueryActiveHud(nullptr);
}

void NetplayHud_Render() {
    MatchHudData hud{};
    if (!QueryActiveHud(&hud))
        return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float W = display.x > 0.0f ? display.x : kNativeW;
    const float H = display.y > 0.0f ? display.y : kNativeH;
    const float nickY      = H * kNickYRatio;
    const float nickMargin = W * kNickMarginXRatio;

    // --- TOP: Nickname pills below HP bar area ---
    if (GetGameMode() == MODE_MATCH) {
        DrawNickPill(dl, hud.p1_name, kP1Bg,
                     nickMargin, nickY, false);               // left-aligned
        DrawNickPill(dl, hud.p2_name, kP2Bg,
                     W - nickMargin, nickY, true);             // right-aligned
    }

    // --- BOTTOM: Connection stats or spectator status ---
    {
        char stats[80];
        if (hud.show_connection_stats) {
            if (hud.ping_ms >= 0.0f) {
                snprintf(stats, sizeof(stats), "PING:%dms  D:%d  RB:%d",
                         (int)(hud.ping_ms + 0.5f), hud.delay_frames, hud.rollback_frames);
            } else {
                snprintf(stats, sizeof(stats), "PING:--  D:%d  RB:%d",
                         hud.delay_frames, hud.rollback_frames);
            }
        } else if (hud.status_text[0]) {
            snprintf(stats, sizeof(stats), "%s", hud.status_text);
        } else {
            stats[0] = '\0';
        }

        if (!stats[0]) return;

        ImVec2 sz = ImGui::CalcTextSize(stats);
        float barW = sz.x + kStatsPadW * 2.0f;
        float barH = sz.y + kStatsPadH * 2.0f;
        float barX = (W - barW) * 0.5f;
        float barY = H - barH;

        dl->AddRectFilled(
            ImVec2(barX, barY),
            ImVec2(barX + barW, barY + barH),
            kStatsBg, kStatsRounding);

        dl->AddText(
            ImVec2(barX + kStatsPadW, barY + kStatsPadH),
            kStatsText, stats);
    }
}
