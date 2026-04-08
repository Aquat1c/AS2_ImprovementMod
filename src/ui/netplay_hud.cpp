/**
 * Netplay HUD — ImGui-based overlay for online match stats.
 *
 * Renders player nicknames with win counts (top) and connection stats
 * (ping, delay, rollback) at the bottom using ImGui foreground draw list.
 */

#include "ui/netplay_hud.h"
#include "core/mod_main.h"

#include "imgui.h"
#include <cstdio>
#include <cstring>

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

        char p1text[48];
        char p2text[48];
        snprintf(p1text, sizeof(p1text), "%s (%d)", hud.p1_name, hud.p1_wins);
        snprintf(p2text, sizeof(p2text), "%s (%d)", hud.p2_name, hud.p2_wins);

        // Convert to uppercase for consistency
        for (char* p = p1text; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
        for (char* p = p2text; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;

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

    // --- BOTTOM: Connection stats bar ---
    {
        char stats[80];
        if (hud.ping_ms >= 0.0f) {
            snprintf(stats, sizeof(stats), "PING:%dms  D:%d  RB:%d",
                     (int)(hud.ping_ms + 0.5f), hud.delay_frames, hud.rollback_frames);
        } else {
            snprintf(stats, sizeof(stats), "PING:--  D:%d  RB:%d",
                     hud.delay_frames, hud.rollback_frames);
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
