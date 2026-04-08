/**
 * Alice Senki 2 — Hitbox / Hurtbox / Collision Viewer
 *
 * All geometry and coordinate math derived from decompilation:
 *   - Entity_UpdateHitDetection  (AABB collision formula)
 *   - Weather_UpdateScroll       (camera/scroll system)
 *   - Sprite rendering functions (world-to-screen: pos/10 - scroll)
 *   - HitDef_Create / HitDef_Init / HitDef_SetBaseStats
 *
 * Coordinate conventions (from decompilation):
 *   World positions : entity+0xB8 / +0xBA are int16 in ×10 fixed-point.
 *   Screen coords   : worldPos / 10 - scroll{X,Y}.
 *   Box center      : entityPos/10 + 2*offset*facing  (X axis mirrors by facing)
 *   Box extent      : halfW/halfH are the AABB half-extents directly
 *
 * Animation frame layout (104 bytes):
 *   Offset  0-7  : Collision/push box (1 entry × 8B)
 *   Offset  8-39 : Hitboxes / attack boxes (4 entries × 8B)
 *   Offset 40-71 : Hurtboxes / vulnerable boxes (4 entries × 8B)
 *   Offset 72-103: Extended box set (4 entries × 8B, TBD)
 *   Game resolution : 640 × 480 logical.
 */

#include "hitbox_viewer.h"
#include "mod_main.h"
#include "memory_utils.h"
#include "as2_constants.h"
#include "log_window.h"
#include "game_state.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>

// ============================================================================
// Toggle state
// ============================================================================

static bool  g_enabled         = false;
static bool  g_showHurtboxes   = true;   // per-animation boxes (green)
static bool  g_showHitboxes    = true;   // global HitDef entries (red)
static bool  g_showPushboxes   = true;   // active rect (yellow)
static bool  g_showPositions   = true;   // axis cross + facing arrow
static bool  g_showInfo        = false;  // text overlay with box data
static bool  g_showMatchState  = false;  // match context debug info
static float g_fillAlpha       = 0.25f;  // fill transparency [0..1]
static bool  g_logPerFrame     = false;  // per-frame logging (VERY verbose)

// Tracking for animation changes (log on change)
static uint32_t s_lastP1Anim = 0xFFFFFFFF;
static uint32_t s_lastP2Anim = 0xFFFFFFFF;
static uint32_t s_lastP1Action = 0xFFFFFFFF;
static uint32_t s_lastP2Action = 0xFFFFFFFF;

// ============================================================================
// Colour constants
// ============================================================================

static ImU32 ColFill(ImU32 rgb, float a) {
    return (rgb & 0x00FFFFFF) | ((ImU32)(a * 255.0f) << 24);
}

static const ImU32 COL_HURTBOX   = IM_COL32(0, 220, 0, 255);
static const ImU32 COL_HITBOX    = IM_COL32(255, 40, 40, 255);
static const ImU32 COL_PUSHBOX   = IM_COL32(255, 220, 0, 255);
static const ImU32 COL_HITDEF_PT = IM_COL32(255, 80, 80, 255);  // HitDef position dot
static const ImU32 COL_P1_CROSS  = IM_COL32(255, 120, 120, 255);
static const ImU32 COL_P2_CROSS  = IM_COL32(120, 120, 255, 255);

// ============================================================================
// Helpers
// ============================================================================

static const float GAME_W = 640.0f;
static const float GAME_H = 480.0f;

struct ScreenTransform {
    int16_t scrollX;
    int16_t scrollY;
    float   scaleX;   // displayPixels / gamePixel
    float   scaleY;
};

static ScreenTransform GetTransform() {
    ScreenTransform t;
    t.scrollX = ReadMemory<int16_t>(ADDR_SCROLL_X);
    t.scrollY = ReadMemory<int16_t>(ADDR_SCROLL_Y);
    ImGuiIO& io = ImGui::GetIO();
    t.scaleX = io.DisplaySize.x / GAME_W;
    t.scaleY = io.DisplaySize.y / GAME_H;
    return t;
}

/// Convert a game-screen-space point (after /10 and -scroll) to display pixels.
static ImVec2 GameToDisplay(float gx, float gy, const ScreenTransform& t) {
    return ImVec2(gx * t.scaleX, gy * t.scaleY);
}

/// World position (×10 fixed-point) → display pixels.
static ImVec2 WorldToDisplay(int16_t wx, int16_t wy, const ScreenTransform& t) {
    float gx = (float)(wx / 10) - (float)t.scrollX;
    float gy = (float)(wy / 10) - (float)t.scrollY;
    return GameToDisplay(gx, gy, t);
}

/// Draw an outlined filled rectangle (game-screen coords).
static void DrawBox(ImDrawList* dl, float l, float top, float r, float bot,
                    ImU32 outline, float alpha, const ScreenTransform& t) {
    ImVec2 p1 = GameToDisplay(l, top, t);
    ImVec2 p2 = GameToDisplay(r, bot, t);
    // Normalise
    if (p1.x > p2.x) { float tmp = p1.x; p1.x = p2.x; p2.x = tmp; }
    if (p1.y > p2.y) { float tmp = p1.y; p1.y = p2.y; p2.y = tmp; }
    dl->AddRectFilled(p1, p2, ColFill(outline, alpha));
    dl->AddRect(p1, p2, outline, 0.0f, 0, 2.0f);
}

// ============================================================================
// Animation-frame box reader  (generic for all box sets)
// ============================================================================

struct BoxEntry {
    int16_t xOff;      // centre x offset (raw — multiply by 2*facing for screen)
    int16_t yOff;      // centre y offset (raw — multiply by 2 for screen)
    int16_t halfW;     // half-extent width
    int16_t halfH;     // half-extent height
};

/// Read 'count' box entries from an animation frame at a given offset.
/// If animIdxOverride >= 0, uses that instead of entity's current anim index.
static int ReadAnimBoxes(uintptr_t entity, int frameOffset, BoxEntry* out, int count,
                         bool doLog, const char* label, int animIdxOverride = -1) {
    uint32_t animIdx = (animIdxOverride >= 0)
        ? (uint32_t)animIdxOverride
        : ReadMemory<uint32_t>(entity + ENTITY_OFF_ANIM_INDEX);
    uintptr_t frameBase = entity + ENTITY_OFF_ANIM_DATA
                        + (uintptr_t)ANIM_DATA_STRIDE * animIdx
                        + frameOffset;
    int active = 0;

    if (doLog) {
        LOG_INFO("[HBV] %s animIdx=%u entity=0x%08X off=%d frameBase=0x%08X",
                 label, animIdx, (uint32_t)entity, frameOffset, (uint32_t)frameBase);
    }

    for (int i = 0; i < count; i++) {
        uintptr_t ep = frameBase + (uintptr_t)i * HURTBOX_ENTRY_SIZE;
        BoxEntry e;
        e.xOff  = ReadMemory<int16_t>(ep + 0);
        e.yOff  = ReadMemory<int16_t>(ep + 2);
        e.halfW = ReadMemory<int16_t>(ep + 4);
        e.halfH = ReadMemory<int16_t>(ep + 6);
        out[i] = e;
        if (e.halfW > 0 && e.halfH > 0) active++;

        if (doLog) {
            LOG_INFO("[HBV]   %s box[%d] @0x%08X: off(%d,%d) half(%d,%d) %s",
                     label, i, (uint32_t)ep,
                     e.xOff, e.yOff, e.halfW, e.halfH,
                     (e.halfW > 0 && e.halfH > 0) ? "ACTIVE" : "skip");
        }
    }

    // Raw hex dump for diagnosis
    if (doLog) {
        uintptr_t rawBase = entity + ENTITY_OFF_ANIM_DATA
                          + (uintptr_t)ANIM_DATA_STRIDE * animIdx;
        char hex[256];
        int pos = 0;
        for (int i = 0; i < 104 && pos < 240; i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ",
                            ReadMemory<uint8_t>(rawBase + i));
        }
        LOG_INFO("[HBV] %s raw frame[%u] 104B: %s", label, animIdx, hex);
    }
    return active;
}

static void RenderHurtboxes(ImDrawList* dl, uintptr_t entity,
                             const char* label, bool animChanged,
                             const ScreenTransform& t) {
    int16_t posX   = ReadMemory<int16_t>(entity + ENTITY_OFF_X_POS);
    int16_t posY   = ReadMemory<int16_t>(entity + ENTITY_OFF_Y_POS);
    int8_t  facing = ReadMemory<int8_t>(entity + ENTITY_OFF_FACING);

    bool doLog = animChanged || g_logPerFrame;

    BoxEntry boxes[4];
    ReadAnimBoxes(entity, ANIM_HURTBOX_OFFSET, boxes, HURTBOX_COUNT_PER_FRAME, doLog, label);

    float ex = (float)(posX / 10);
    float ey = (float)(posY / 10);

    if (doLog) {
        LOG_INFO("[HBV] %s pos=(%d,%d) pos/10=(%.1f,%.1f) facing=%d scroll=(%d,%d)",
                 label, posX, posY, ex, ey, facing, t.scrollX, t.scrollY);
    }

    for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; i++) {
        const BoxEntry& b = boxes[i];
        if (b.halfW <= 0 || b.halfH <= 0) continue;

        float cx = ex + 2.0f * b.xOff * facing;
        float cy = ey + 2.0f * b.yOff;
        // Decompilation: collision test uses abs(diff)/2 <= halfW_A + halfW_B
        // => true screen half-extent = 2 * halfW (and 2 * halfH)
        float hw = 2.0f * b.halfW;
        float hh = 2.0f * b.halfH;

        // Convert to game-screen coords (subtract scroll)
        float left  = cx - hw - t.scrollX;
        float right = cx + hw - t.scrollX;
        float top   = cy - hh - t.scrollY;
        float bot   = cy + hh - t.scrollY;

        if (doLog) {
            LOG_INFO("[HBV]   %s draw[%d]: centre=(%.1f,%.1f) hw=%.1f hh=%.1f -> screen L=%.1f T=%.1f R=%.1f B=%.1f",
                     label, i, cx, cy, hw, hh, left, top, right, bot);
        }

        DrawBox(dl, left, top, right, bot, COL_HURTBOX, g_fillAlpha, t);
    }
}

// ============================================================================
// Per-entity hitboxes / attack boxes  (offset 8 in animation frame)
// ============================================================================

static void RenderEntityHitboxes(ImDrawList* dl, uintptr_t entity,
                                  const char* label, bool animChanged,
                                  const ScreenTransform& t) {
    int16_t posX   = ReadMemory<int16_t>(entity + ENTITY_OFF_X_POS);
    int16_t posY   = ReadMemory<int16_t>(entity + ENTITY_OFF_Y_POS);
    int8_t  facing = ReadMemory<int8_t>(entity + ENTITY_OFF_FACING);

    BoxEntry boxes[4];
    ReadAnimBoxes(entity, ANIM_HITBOX_OFFSET, boxes, HURTBOX_COUNT_PER_FRAME,
                  animChanged || g_logPerFrame, label);

    float ex = (float)(posX / 10);
    float ey = (float)(posY / 10);

    for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; i++) {
        const BoxEntry& b = boxes[i];
        if (b.halfW <= 0 || b.halfH <= 0) continue;

        float cx = ex + 2.0f * b.xOff * facing;
        float cy = ey + 2.0f * b.yOff;
        float hw = 2.0f * b.halfW;
        float hh = 2.0f * b.halfH;

        float left  = cx - hw - t.scrollX;
        float right = cx + hw - t.scrollX;
        float top   = cy - hh - t.scrollY;
        float bot   = cy + hh - t.scrollY;

        DrawBox(dl, left, top, right, bot, COL_HITBOX, g_fillAlpha, t);
    }
}

// ============================================================================
// HitDef / projectile hitbox reader  (global array, 100 entries)
// ============================================================================

static void RenderHitDefs(ImDrawList* dl, const ScreenTransform& t) {
    int activeCount = 0;
    for (int i = 0; i < SUMMON_MAX_SLOTS; i++) {
        uintptr_t entry = ADDR_SUMMON_ARRAY + (uintptr_t)i * SUMMON_ENTRY_SIZE;

        uint32_t id     = ReadMemory<uint32_t>(entry + HITDEF_OFF_ID);
        if (id == 0) continue;                          // free slot
        uint8_t  active = ReadMemory<uint8_t>(entry + HITDEF_OFF_ACTIVE);
        if (active != 1) continue;                      // not active

        activeCount++;

        int16_t wx = ReadMemory<int16_t>(entry + HITDEF_OFF_X);
        int16_t wy = ReadMemory<int16_t>(entry + HITDEF_OFF_Y);
        int8_t  facing = ReadMemory<int8_t>(entry + HITDEF_OFF_FACING);

        // Resolve animation data owner (offset 201, NOT damage owner at offset 0)
        uint8_t animOwner = ReadMemory<uint8_t>(entry + HITDEF_OFF_ANIM_OWNER);
        uintptr_t ownerEntity = (animOwner == 0)
            ? (uintptr_t)ADDR_P1_ENTITY_BASE
            : (uintptr_t)ADDR_P2_ENTITY_BASE;

        if (g_logPerFrame) {
            uint8_t owner = ReadMemory<uint8_t>(entry + HITDEF_OFF_OWNER);
            uint8_t type = ReadMemory<uint8_t>(entry + HITDEF_OFF_TYPE);
            uint8_t flag = ReadMemory<uint8_t>(entry + HITDEF_OFF_ACTIVE_FLAG);
            uint32_t dmg = ReadMemory<uint32_t>(entry + HITDEF_OFF_DAMAGE);
            LOG_INFO("[HBV] HitDef[%d] id=%u owner=%u animOwner=%u type=%u active=%u flag=%d pos=(%d,%d) face=%d dmg=%u",
                     i, id, owner, animOwner, type, active, (int)(int8_t)flag, wx, wy, facing, dmg);
        }

        // Use summon's own animation frame index, not owner's current anim
        uint16_t summonAnimIdx = ReadMemory<uint16_t>(entry + HITDEF_OFF_ANIM_FRAME_IDX);
        BoxEntry boxes[4];
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "HitDef[%d]", i);
        ReadAnimBoxes(ownerEntity, ANIM_HITBOX_OFFSET, boxes, HURTBOX_COUNT_PER_FRAME,
                      g_logPerFrame, lbl, (int)summonAnimIdx);

        // Position the owner's animation boxes at the HitDef's world location
        float hx = (float)(wx / 10);
        float hy = (float)(wy / 10);
        bool anyDrawn = false;

        for (int j = 0; j < HURTBOX_COUNT_PER_FRAME; j++) {
            const BoxEntry& b = boxes[j];
            if (b.halfW <= 0 || b.halfH <= 0) continue;

            float cx = hx + 2.0f * b.xOff * facing;
            float cy = hy + 2.0f * b.yOff;
            float hw = 2.0f * b.halfW;
            float hh = 2.0f * b.halfH;

            float left  = cx - hw - t.scrollX;
            float right = cx + hw - t.scrollX;
            float top   = cy - hh - t.scrollY;
            float bot   = cy + hh - t.scrollY;

            DrawBox(dl, left, top, right, bot, COL_HITBOX, g_fillAlpha, t);
            anyDrawn = true;
        }

        // Always draw a position marker for the HitDef
        if (!anyDrawn) {
            ImVec2 p = WorldToDisplay(wx, wy, t);
            dl->AddCircleFilled(p, 5.0f * t.scaleX, COL_HITDEF_PT);
        }
    }

    if (g_logPerFrame && activeCount > 0) {
        LOG_INFO("[HBV] Active HitDefs: %d", activeCount);
    }
}

// ============================================================================
// Collision / push box  (offset 0 in animation frame, single entry)
// ============================================================================

static void RenderPushbox(ImDrawList* dl, uintptr_t entity,
                          const char* label, bool animChanged,
                          const ScreenTransform& t) {
    // Collision box is the single entry at frame offset 0
    BoxEntry box;
    ReadAnimBoxes(entity, ANIM_COLLISION_OFFSET, &box, 1,
                  animChanged || g_logPerFrame, label);

    if (box.halfW <= 0 || box.halfH <= 0) return;

    int16_t posX   = ReadMemory<int16_t>(entity + ENTITY_OFF_X_POS);
    int16_t posY   = ReadMemory<int16_t>(entity + ENTITY_OFF_Y_POS);
    int8_t  facing = ReadMemory<int8_t>(entity + ENTITY_OFF_FACING);

    float ex = (float)(posX / 10);
    float ey = (float)(posY / 10);
    float cx = ex + 2.0f * box.xOff * facing;
    float cy = ey + 2.0f * box.yOff;
    float hw = 2.0f * box.halfW;
    float hh = 2.0f * box.halfH;

    float left  = cx - hw - t.scrollX;
    float right = cx + hw - t.scrollX;
    float top   = cy - hh - t.scrollY;
    float bot   = cy + hh - t.scrollY;

    DrawBox(dl, left, top, right, bot, COL_PUSHBOX, g_fillAlpha, t);
}

// ============================================================================
// Position / facing marker
// ============================================================================

static void RenderPosition(ImDrawList* dl, uintptr_t entity, ImU32 colour,
                           const ScreenTransform& t) {
    int16_t posX = ReadMemory<int16_t>(entity + ENTITY_OFF_X_POS);
    int16_t posY = ReadMemory<int16_t>(entity + ENTITY_OFF_Y_POS);
    int8_t  face = ReadMemory<int8_t>(entity + ENTITY_OFF_FACING);

    ImVec2 c = WorldToDisplay(posX, posY, t);
    float  r = 4.0f * t.scaleX;

    dl->AddCircleFilled(c, r, colour);
    // Cross
    dl->AddLine(ImVec2(c.x - r * 2, c.y), ImVec2(c.x + r * 2, c.y), colour, 1.5f);
    dl->AddLine(ImVec2(c.x, c.y - r * 2), ImVec2(c.x, c.y + r * 2), colour, 1.5f);
    // Facing arrow
    float arrowLen = 12.0f * t.scaleX * face;
    dl->AddLine(c, ImVec2(c.x + arrowLen, c.y), colour, 2.5f);
}

// ============================================================================
// Info overlay text
// ============================================================================

static void RenderInfoOverlay(ImDrawList* dl, uintptr_t p1, uintptr_t p2,
                              const ScreenTransform& t) {
    char buf[256];
    float y = 10.0f;
    const float x = 10.0f;
    const float lineH = 14.0f;
    const ImU32 bg = IM_COL32(0, 0, 0, 180);
    const ImU32 white = IM_COL32(200, 200, 200, 255);
    const ImU32 dim   = IM_COL32(150, 150, 150, 255);

    // Count lines to size the panel
    int totalLines = 3; // camera + match header + separator
    if (g_showMatchState) totalLines += 6;
    for (int pi = 0; pi < 2; pi++) {
        totalLines += 1; // P header
        uintptr_t ent = (pi == 0) ? p1 : p2;
        BoxEntry hurtBoxes[4], hitBoxes[4];
        BoxEntry collBox;
        ReadAnimBoxes(ent, ANIM_HURTBOX_OFFSET, hurtBoxes, HURTBOX_COUNT_PER_FRAME, false, nullptr);
        ReadAnimBoxes(ent, ANIM_HITBOX_OFFSET, hitBoxes, HURTBOX_COUNT_PER_FRAME, false, nullptr);
        ReadAnimBoxes(ent, ANIM_COLLISION_OFFSET, &collBox, 1, false, nullptr);
        if (collBox.halfW > 0 && collBox.halfH > 0) totalLines++;
        for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; i++) {
            if (hurtBoxes[i].halfW > 0 && hurtBoxes[i].halfH > 0) totalLines++;
            if (hitBoxes[i].halfW > 0 && hitBoxes[i].halfH > 0) totalLines++;
        }
    }

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 420, y + lineH * totalLines + 6), bg);

    auto line = [&](ImU32 col, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        dl->AddText(ImVec2(x + 4, y), col, buf);
        y += lineH;
    };

    line(white, "Scroll: %d,%d  Display: %.0fx%.0f  Scale: %.2f,%.2f",
         t.scrollX, t.scrollY,
         ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y,
         t.scaleX, t.scaleY);

    // Match state block
    if (g_showMatchState) {
        uint32_t gameMode  = ReadMemory<uint32_t>(ADDR_GAME_MODE);
        uint32_t subState  = ReadMemory<uint32_t>(ADDR_SUB_STATE);
        uint32_t gameType  = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
        uint32_t simFrame  = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        uint32_t dispFrame = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
        uint32_t phaseTimer= ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
        uint32_t roundTimer= ReadMemory<uint32_t>(ADDR_ROUND_TIMER);

        line(dim, "Mode:%u Sub:%u Type:%u SimF:%u DispF:%u",
             gameMode, subState, gameType, simFrame, dispFrame);
        line(dim, "PhaseTimer:%u RoundTimer:%u MatchBase:0x%08X",
             phaseTimer, roundTimer, (uint32_t)ADDR_MATCH_BASE);
        line(dim, "ScrollAddr: X=0x%08X Y=0x%08X",
             (uint32_t)ADDR_SCROLL_X, (uint32_t)ADDR_SCROLL_Y);

        // Raw bytes around the scroll addresses for verification
        char scrollHex[128];
        int pos = 0;
        for (int i = -4; i < 12 && pos < 120; i += 2) {
            int16_t v = ReadMemory<int16_t>(ADDR_SCROLL_X + i);
            pos += snprintf(scrollHex + pos, sizeof(scrollHex) - pos, "%04X ", (uint16_t)v);
        }
        line(dim, "Scroll vicinity: %s", scrollHex);

        line(dim, "P1Base:0x%08X P2Base:0x%08X Stride:%d",
             (uint32_t)ADDR_P1_ENTITY_BASE, (uint32_t)ADDR_P2_ENTITY_BASE,
             ENTITY_SIZE);
        line(dim, "AnimData +0x%X  Stride:%d  Coll@%d Hit@%d Hurt@%d  %d\u00d7%dB",
             ENTITY_OFF_ANIM_DATA, ANIM_DATA_STRIDE,
             ANIM_COLLISION_OFFSET, ANIM_HITBOX_OFFSET, ANIM_HURTBOX_OFFSET,
             HURTBOX_COUNT_PER_FRAME, HURTBOX_ENTRY_SIZE);
    }

    // Player info
    for (int pi = 0; pi < 2; pi++) {
        uintptr_t ent = (pi == 0) ? p1 : p2;
        ImU32 col = (pi == 0) ? COL_P1_CROSS : COL_P2_CROSS;
        int16_t px = ReadMemory<int16_t>(ent + ENTITY_OFF_X_POS);
        int16_t py = ReadMemory<int16_t>(ent + ENTITY_OFF_Y_POS);
        int8_t  pf = ReadMemory<int8_t>(ent + ENTITY_OFF_FACING);
        int16_t hp = ReadMemory<int16_t>(ent + ENTITY_OFF_HP);
        uint32_t animIdx  = ReadMemory<uint32_t>(ent + ENTITY_OFF_ANIM_INDEX);
        uint32_t actionId = ReadMemory<uint32_t>(ent + ENTITY_OFF_ACTION_ID);

        line(col, "P%d Pos:%d,%d Face:%d HP:%d Act:%u Anim:%u",
             pi + 1, px, py, pf, hp, actionId, animIdx);

        BoxEntry hurtB[4], hitB[4];
        BoxEntry collB;
        ReadAnimBoxes(ent, ANIM_HURTBOX_OFFSET, hurtB, HURTBOX_COUNT_PER_FRAME, false, nullptr);
        ReadAnimBoxes(ent, ANIM_HITBOX_OFFSET, hitB, HURTBOX_COUNT_PER_FRAME, false, nullptr);
        ReadAnimBoxes(ent, ANIM_COLLISION_OFFSET, &collB, 1, false, nullptr);
        if (collB.halfW > 0 && collB.halfH > 0)
            line(col, "  coll off(%d,%d) half(%d,%d)", collB.xOff, collB.yOff, collB.halfW, collB.halfH);
        for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; i++) {
            if (hurtB[i].halfW > 0 && hurtB[i].halfH > 0)
                line(col, "  hurt%d off(%d,%d) half(%d,%d)", i, hurtB[i].xOff, hurtB[i].yOff, hurtB[i].halfW, hurtB[i].halfH);
            if (hitB[i].halfW > 0 && hitB[i].halfH > 0)
                line(col, "  hit%d off(%d,%d) half(%d,%d)", i, hitB[i].xOff, hitB[i].yOff, hitB[i].halfW, hitB[i].halfH);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void HitboxViewer_Init() {
    g_enabled = false;
    s_lastP1Anim = 0xFFFFFFFF;
    s_lastP2Anim = 0xFFFFFFFF;
    s_lastP1Action = 0xFFFFFFFF;
    s_lastP2Action = 0xFFFFFFFF;
    LOG_INFO("[HBV] Hitbox Viewer initialized");
}

void HitboxViewer_Render() {
    if (!g_enabled) return;
    if (!AS2_IsInMatch()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    ScreenTransform t = GetTransform();

    uintptr_t p1 = (uintptr_t)ADDR_P1_ENTITY_BASE;
    uintptr_t p2 = (uintptr_t)ADDR_P2_ENTITY_BASE;

    // Track animation/action changes for logging
    uint32_t p1Anim   = ReadMemory<uint32_t>(p1 + ENTITY_OFF_ANIM_INDEX);
    uint32_t p2Anim   = ReadMemory<uint32_t>(p2 + ENTITY_OFF_ANIM_INDEX);
    uint32_t p1Action = ReadMemory<uint32_t>(p1 + ENTITY_OFF_ACTION_ID);
    uint32_t p2Action = ReadMemory<uint32_t>(p2 + ENTITY_OFF_ACTION_ID);

    bool p1AnimChanged = (p1Anim != s_lastP1Anim);
    bool p2AnimChanged = (p2Anim != s_lastP2Anim);

    if (p1Action != s_lastP1Action) {
        LOG_INFO("[HBV] P1 action %u -> %u  anim %u -> %u",
                 s_lastP1Action, p1Action, s_lastP1Anim, p1Anim);
        s_lastP1Action = p1Action;
    }
    if (p2Action != s_lastP2Action) {
        LOG_INFO("[HBV] P2 action %u -> %u  anim %u -> %u",
                 s_lastP2Action, p2Action, s_lastP2Anim, p2Anim);
        s_lastP2Action = p2Action;
    }
    s_lastP1Anim = p1Anim;
    s_lastP2Anim = p2Anim;

    if (g_showHurtboxes) {
        RenderHurtboxes(dl, p1, "P1", p1AnimChanged, t);
        RenderHurtboxes(dl, p2, "P2", p2AnimChanged, t);
    }
    if (g_showHitboxes) {
        RenderEntityHitboxes(dl, p1, "P1", p1AnimChanged, t);
        RenderEntityHitboxes(dl, p2, "P2", p2AnimChanged, t);
        RenderHitDefs(dl, t);
    }
    if (g_showPushboxes) {
        RenderPushbox(dl, p1, "P1", p1AnimChanged, t);
        RenderPushbox(dl, p2, "P2", p2AnimChanged, t);
    }
    if (g_showPositions) {
        RenderPosition(dl, p1, COL_P1_CROSS, t);
        RenderPosition(dl, p2, COL_P2_CROSS, t);
    }
    if (g_showInfo) {
        RenderInfoOverlay(dl, p1, p2, t);
    }
}

void HitboxViewer_RenderControls() {
    ImGui::Checkbox("Enable Hitbox Viewer", &g_enabled);
    if (!g_enabled) return;

    ImGui::Separator();
    ImGui::Checkbox("Hurtboxes (green, @40)", &g_showHurtboxes);
    ImGui::Checkbox("Hitboxes / attacks (red, @8)", &g_showHitboxes);
    ImGui::Checkbox("Collision / pushbox (yellow, @0)", &g_showPushboxes);
    ImGui::Checkbox("Position markers", &g_showPositions);
    ImGui::Checkbox("Info overlay", &g_showInfo);
    ImGui::Checkbox("Match state debug", &g_showMatchState);
    ImGui::SliderFloat("Fill alpha", &g_fillAlpha, 0.0f, 1.0f, "%.2f");
    ImGui::Separator();
    ImGui::Checkbox("Per-frame logging (VERBOSE)", &g_logPerFrame);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Logs every box read every frame.\nWill flood the log. Use briefly.");

    if (ImGui::Button("Log Once Now")) {
        // Force a one-shot log by resetting the anim trackers
        s_lastP1Anim = 0xFFFFFFFF;
        s_lastP2Anim = 0xFFFFFFFF;
        LOG_INFO("[HBV] --- Manual one-shot log triggered ---");
    }
}
