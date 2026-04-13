/**
 * Netplay HUD — ImGui-based overlay for online match stats.
 *
 * Renders player nicknames with win counts (top) and connection stats
 * (ping, delay, rollback) at the bottom using ImGui foreground draw list.
 */

#include "ui/netplay_hud.h"
#include "core/mod_main.h"

#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

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

static void BuildPlayerLabel(char* out, size_t outCap, const char* name, int wins) {
    char clippedName[96] = {};
    ClipUtf8Text(clippedName, sizeof(clippedName), name && name[0] ? name : "Player", 18);
    _snprintf_s(out, outCap, _TRUNCATE, "%s (%d)", clippedName, wins);
}

} // namespace

// ============================================================================
// Rendering
// ============================================================================

void NetplayHud_Render() {
    MatchHudData hud{};
    if (!ModGetMatchHudData(&hud) || !hud.active)
        return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float W = 640.0f;
    const float H = 480.0f;

    // --- TOP: Player names with win counts above HP bars ---
    {
        const float topY = 2.0f;

        char p1text[128];
        char p2text[128];
        BuildPlayerLabel(p1text, sizeof(p1text), hud.p1_name, hud.p1_wins);
        BuildPlayerLabel(p2text, sizeof(p2text), hud.p2_name, hud.p2_wins);

        ImVec2 p1sz = ImGui::CalcTextSize(p1text);
        ImVec2 p2sz = ImGui::CalcTextSize(p2text);

        const float pad = 4.0f;
        const ImU32 shadowCol = IM_COL32(0, 0, 0, 180);

        // P1 (left, blue)
        const ImU32 p1col = IM_COL32(100, 200, 255, 255);
        dl->AddText(ImVec2(pad + 1.0f, topY + 1.0f), shadowCol, p1text);
        dl->AddText(ImVec2(pad, topY), p1col, p1text);

        // P2 (right, red)
        const ImU32 p2col = IM_COL32(255, 130, 130, 255);
        float p2x = W - pad - p2sz.x;
        dl->AddText(ImVec2(p2x + 1.0f, topY + 1.0f), shadowCol, p2text);
        dl->AddText(ImVec2(p2x, topY), p2col, p2text);
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

        if (!stats[0]) {
            return;
        }

        ImVec2 sz = ImGui::CalcTextSize(stats);
        const float pad = 4.0f;
        float barW = sz.x + pad * 2.0f;
        float barH = sz.y + pad * 2.0f;
        float barX = (W - barW) * 0.5f;
        float barY = H - barH;

        // Background
        dl->AddRectFilled(
            ImVec2(barX, barY),
            ImVec2(barX + barW, barY + barH),
            IM_COL32(0, 0, 0, 160));

        // Text
        dl->AddText(
            ImVec2(barX + pad, barY + pad),
            IM_COL32(255, 255, 255, 230),
            stats);
    }
}
