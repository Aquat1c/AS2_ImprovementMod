/**
 * Alice Senki 2 - Hitbox/Hurtbox/Collision Display
 * Debug visualization and structure discovery
 * 
 * BOX SYSTEM ARCHITECTURE (From Decompilation):
 * 
 * 1. BOX ENABLE FLAGS (Priority System):
 *    - Location: Entity +1652 to +1675 (24 bytes)
 *    - Values: 0-15 = priority (higher overrides lower), 0xFF = disabled
 *    - Functions:
 *      - sub_4BEF80 (0x4BEF80): Enable 5 flags (+1652,+1653,+1667,+1668,+1674,+1675)
 *      - sub_4BF010 (0x4BF010): Enable 13 flags (+1654 through +1666)
 *      - sub_4BF160 (0x4BF160): Enable 5 flags (+1669 through +1673)
 *      - sub_4BF350 (0x4BF350): Clear all (memset 24 bytes to 0)
 * 
 * 2. BOX COORDINATE DATA (Per-Animation Multi-Box Array):
 *    - Base: Entity +4104 (0x1008)
 *    - Index: Entity +4100 (0x1004) stores current animation index
 *    - Structure: 104 bytes per animation frame containing 3 box types
 *    - Access: entity + 104 * animation_index + 4104
 *    
 *    THREE BOX TYPES PER ANIMATION FRAME:
 *    - Box 1 (Collision/Pushbox): +0 to +39 (40 bytes) at base+0
 *    - Box 2 (Hurtbox):          +40 to +79 (40 bytes) at base+40
 *    - Box 3 (Hitbox):           +72 to +103 (32+ bytes) at base+72
 *    
 *    Individual Box Layout (Int16, scaled ×20):
 *      +0: x_offset
 *      +2: y_offset
 *      +4: width
 *      +6: height
 *      +8-39: Unknown (additional box parameters)
 * 
 * 3. BOX PROCESSING ARRAY:
 *    - Location: Entity +41791 (0xA33F)
 *    - Count: 94 boxes per player
 *    - Size: 4 bytes per box
 *    - Cleared by sub_4BE640 each frame
 * 
 * CURRENT STATUS:
 * - Old implementation scanned +0x600-0x800 (INCORRECT)
 * - Need to read from THREE separate box offsets (+0, +40, +72) within animation array
 * - All coordinates scaled by 20× for game units
 * - Box enable flags separate from coordinate data
 */

#include "hitbox_display.h"
#include "mod_main.h"
#include "memory_utils.h"
#include "log_window.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>

// ============================================================================
// Configuration
// ============================================================================

static bool g_enabled = false;
static bool g_showHitboxes = true;
static bool g_showHurtboxes = true;
static bool g_showCollision = true;

// Tracking for action changes
static uint32_t g_lastP1Action = 0;
static uint32_t g_lastP2Action = 0;
static bool g_logOnActionChange = false;  // Default OFF to reduce console spam
static bool g_scanForHitboxValues = false;  // Default OFF - enable for research

// Game constants
static const float GAME_WIDTH = 640.0f;
static const float GAME_HEIGHT = 480.0f;

// Memory addresses  
static const uintptr_t ADDR_CAMERA_X = 0x76CD4C;
static const uintptr_t ADDR_CAMERA_Y = 0x76CD4E;

// Known offsets
static const uint32_t OFF_ACTION_ID = 0x44C;   // Current action ID
static const uint32_t OFF_POS_X = 184;         // +0xB8
static const uint32_t OFF_POS_Y = 186;         // +0xBA
static const uint32_t OFF_FACING = 189;        // +0xBD
static const uint32_t OFF_HP = 176;            // +0xB0

// Colors
static const ImU32 COLOR_HITBOX     = IM_COL32(255, 0, 0, 100);
static const ImU32 COLOR_HITBOX_OUT = IM_COL32(255, 0, 0, 255);
static const ImU32 COLOR_HURTBOX    = IM_COL32(0, 255, 0, 100);
static const ImU32 COLOR_HURTBOX_OUT= IM_COL32(0, 255, 0, 255);
static const ImU32 COLOR_COLLISION  = IM_COL32(255, 255, 0, 100);
static const ImU32 COLOR_COLL_OUT   = IM_COL32(255, 255, 0, 255);

// ============================================================================
// Helper Functions
// ============================================================================

static ImVec2 WorldToScreen(int16_t worldX, int16_t worldY, float screenWidth, float screenHeight) {
    int16_t cameraX = ReadMemory<int16_t>(ADDR_CAMERA_X);
    int16_t cameraY = ReadMemory<int16_t>(ADDR_CAMERA_Y);
    
    float screenX = (float)(worldX / 10) - (float)cameraX;
    float screenY = (float)(worldY / 10) - (float)cameraY;
    
    float scaleX = screenWidth / GAME_WIDTH;
    float scaleY = screenHeight / GAME_HEIGHT;
    
    return ImVec2(screenX * scaleX, screenY * scaleY);
}

static void DrawBoxWithOutline(ImDrawList* drawList, ImVec2 p1, ImVec2 p2, ImU32 fillColor, ImU32 outlineColor) {
    ImVec2 topLeft(fminf(p1.x, p2.x), fminf(p1.y, p2.y));
    ImVec2 bottomRight(fmaxf(p1.x, p2.x), fmaxf(p1.y, p2.y));
    drawList->AddRectFilled(topLeft, bottomRight, fillColor);
    drawList->AddRect(topLeft, bottomRight, outlineColor, 0.0f, 0, 2.0f);
}

// Check if value looks like a hitbox dimension (reasonable size)
static bool IsHitboxValue(int16_t val) {
    return val > 50 && val < 2000;
}

// Scan memory for potential hitbox data patterns
static void ScanForHitboxPatterns(const char* player, uintptr_t base, uint32_t actionId) {
    LOG_INFO("=== %s ACTION %d - SCANNING FOR HITBOX PATTERNS ===", player, actionId);
    
    int16_t posX = ReadMemory<int16_t>(base + OFF_POS_X);
    int16_t posY = ReadMemory<int16_t>(base + OFF_POS_Y);
    LOG_INFO("  Pos: %d,%d", posX, posY);
    
    // Scan offsets from 0x600 to 0x800 looking for hitbox-like values
    // Hitbox structure is typically: x_off, y_off, width, height, ???, type
    for (uint32_t offset = 0x600; offset < 0x800; offset += 16) {
        int16_t v0 = ReadMemory<int16_t>(base + offset);
        int16_t v1 = ReadMemory<int16_t>(base + offset + 2);
        int16_t v2 = ReadMemory<int16_t>(base + offset + 4);
        int16_t v3 = ReadMemory<int16_t>(base + offset + 6);
        int32_t v4 = ReadMemory<int32_t>(base + offset + 8);
        int32_t v5 = ReadMemory<int32_t>(base + offset + 12);
        
        // Look for patterns that could be hitboxes:
        // - v2 (width) and v3 (height) should be positive reasonable values
        // - v5 (type) should be 1-11 for active, 0 or 12 for inactive
        bool hasHitboxValues = (IsHitboxValue(v2) || IsHitboxValue(v3));
        bool hasActiveType = (v5 >= 1 && v5 <= 11);
        
        // Also check collision pattern: just width, height at offset
        bool hasCollisionValues = IsHitboxValue(v0) && IsHitboxValue(v1);
        
        if (hasHitboxValues || hasActiveType || hasCollisionValues) {
            LOG_INFO("  +0x%03X: [%d %d %d %d] unk:%d type:%d %s%s%s", 
                     offset, v0, v1, v2, v3, v4, v5,
                     hasHitboxValues ? "[HAS_SIZE]" : "",
                     hasActiveType ? "[ACTIVE_TYPE]" : "",
                     hasCollisionValues ? "[COLL_PAIR]" : "");
        }
    }
    
    // Also dump the known areas in detail
    LOG_INFO("  --- Known offset blocks (int16 x 8) ---");
    
    for (uint32_t blockOff = 0x680; blockOff <= 0x700; blockOff += 0x10) {
        char buf[256];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  +0x%03X:", blockOff);
        for (int i = 0; i < 8; i++) {
            int16_t val = ReadMemory<int16_t>(base + blockOff + i * 2);
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %d", val);
        }
        LOG_INFO("%s", buf);
    }
}

// Log entity data when action changes
static void LogEntityData(const char* player, uintptr_t base, uint32_t actionId) {
    // Only scan on action change if enabled - significantly reduces log spam
    if (!g_scanForHitboxValues) {
        return;
    }
    
    // Only full scan for attack-like actions (usually > 10 or specific ranges)
    // Action 0-5 are typically idle/walk/crouch
    if (actionId > 5) {
        ScanForHitboxPatterns(player, base, actionId);
    }
}

// ============================================================================
// Public API
// ============================================================================

void HitboxDisplay_Init() {
    g_enabled = false;
    g_lastP1Action = 0;
    g_lastP2Action = 0;
    LOG_INFO("[HITBOX] Display system initialized - will scan on attack actions");
}

void HitboxDisplay_Render() {
    if (!g_enabled) return;
    
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) return;
    
    ImGuiIO& io = ImGui::GetIO();
    float screenWidth = io.DisplaySize.x;
    float screenHeight = io.DisplaySize.y;
    
    // Draw debug info box
    drawList->AddRectFilled(ImVec2(10, 10), ImVec2(420, 265), IM_COL32(0, 0, 0, 200));
    drawList->AddRect(ImVec2(10, 10), ImVec2(420, 265), IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.0f);
    
    bool inMatch = AS2_IsInMatch();
    char debugText[256];
    
    snprintf(debugText, sizeof(debugText), "InMatch: %s  AutoScan: %s", 
             inMatch ? "YES" : "NO", g_scanForHitboxValues ? "ON" : "OFF");
    drawList->AddText(ImVec2(15, 15), IM_COL32(255, 255, 0, 255), debugText);
    
    if (!inMatch) return;
    
    // Use live entity pointers from dword_77666C[] instead of static bases.
    // Index 0 = P1, index 27203 = P2.
    uintptr_t p1Base = ReadMemory<uint32_t>(ADDR_ENTITY_ARRAY_ALT);
    uintptr_t p2Base = ReadMemory<uint32_t>(ADDR_ENTITY_ARRAY_ALT + ENTITY_ARRAY_STRIDE * sizeof(uint32_t));
    if (!p1Base || !p2Base) {
        drawList->AddText(ImVec2(15, 35), IM_COL32(255, 100, 100, 255), "Entity pointers not ready");
        return;
    }
    
    // Read current action IDs
    uint32_t p1Action = ReadMemory<uint32_t>(p1Base + OFF_ACTION_ID);
    uint32_t p2Action = ReadMemory<uint32_t>(p2Base + OFF_ACTION_ID);
    
    // Check for action changes and log
    if (g_logOnActionChange) {
        if (p1Action != g_lastP1Action) {
            LogEntityData("P1", p1Base, p1Action);
            g_lastP1Action = p1Action;
        }
        if (p2Action != g_lastP2Action) {
            LogEntityData("P2", p2Base, p2Action);
            g_lastP2Action = p2Action;
        }
    }
    
    // Track animation changes for detailed box logging
    static uint32_t s_lastP1Anim = 0xFFFFFFFF;
    static uint32_t s_lastP2Anim = 0xFFFFFFFF;
    uint32_t p1AnimIdx = ReadMemory<uint32_t>(p1Base + 0x1004);
    uint32_t p2AnimIdx = ReadMemory<uint32_t>(p2Base + 0x1004);
    const bool p1AnimChanged = (p1AnimIdx != s_lastP1Anim);
    const bool p2AnimChanged = (p2AnimIdx != s_lastP2Anim);
    s_lastP1Anim = p1AnimIdx;
    s_lastP2Anim = p2AnimIdx;
    
    // Position info
    int16_t p1X = ReadMemory<int16_t>(p1Base + OFF_POS_X);
    int16_t p1Y = ReadMemory<int16_t>(p1Base + OFF_POS_Y);
    int8_t p1Face = ReadMemory<int8_t>(p1Base + OFF_FACING);
    int16_t p1HP = ReadMemory<int16_t>(p1Base + OFF_HP);
    
    int16_t p2X = ReadMemory<int16_t>(p2Base + OFF_POS_X);
    int16_t p2Y = ReadMemory<int16_t>(p2Base + OFF_POS_Y);
    
    struct BoxData {
        int16_t x;
        int16_t y;
        int16_t w;
        int16_t h;
        int32_t type;
    };



    // Read box data using correct animation system
    auto readBoxFromAnim = [p1AnimChanged, p2AnimChanged](uintptr_t base, uint32_t boxTypeOffset, bool isP1) {
        BoxData b{};
        // Get current animation index
        uint32_t animIndex = ReadMemory<uint32_t>(base + 0x1004);  // ENTITY_OFF_ANIM_INDEX
        
        // Calculate animation data base + box type offset
        uintptr_t boxPtr = base + 104 * animIndex + 0x1008 + boxTypeOffset;  // ENTITY_OFF_ANIM_DATA
        
        // Read box coordinates (INT16, don't scale yet - we'll apply ×20 during conversion)
        b.x = ReadMemory<int16_t>(boxPtr + 0);
        b.y = ReadMemory<int16_t>(boxPtr + 2);
        b.w = ReadMemory<int16_t>(boxPtr + 4);
        b.h = ReadMemory<int16_t>(boxPtr + 6);
        b.type = (b.w > 0 && b.h > 0) ? 1 : 0;  // Active if non-zero dimensions
        
        // Log on animation change
        const bool animChanged = isP1 ? p1AnimChanged : p2AnimChanged;
        if (animChanged) {
            const char* boxName = (boxTypeOffset == 0) ? "COLL" : (boxTypeOffset == 40) ? "HURT" : "HIT";
            LOG_INFO("[Hitbox] %s %s: Entity=0x%08X AnimIdx=%u BoxPtr=0x%08X -> x=%d y=%d w=%d h=%d",
                     isP1 ? "P1" : "P2", boxName, (uint32_t)base, animIndex, (uint32_t)boxPtr, 
                     b.x, b.y, b.w, b.h);
        }
        
        return b;
    };
    
    BoxData p1Coll = readBoxFromAnim(p1Base, 0, true);   // Collision/pushbox at +0
    BoxData p1Hurt = readBoxFromAnim(p1Base, 40, true);  // Hurtbox at +40
    BoxData p1Hit  = readBoxFromAnim(p1Base, 72, true);  // Hitbox at +72
    
    int8_t p2Face = ReadMemory<int8_t>(p2Base + OFF_FACING);
    int16_t p2HP = ReadMemory<int16_t>(p2Base + OFF_HP);
    
    BoxData p2Coll = readBoxFromAnim(p2Base, 0, false);   // Collision/pushbox at +0
    BoxData p2Hurt = readBoxFromAnim(p2Base, 40, false);  // Hurtbox at +40
    BoxData p2Hit  = readBoxFromAnim(p2Base, 72, false);  // Hitbox at +72
    
    // Display info
    snprintf(debugText, sizeof(debugText), "P1: Act=%d Pos=%d,%d Face=%d HP=%d", 
             p1Action, p1X, p1Y, p1Face, p1HP);
    drawList->AddText(ImVec2(15, 35), IM_COL32(255, 150, 150, 255), debugText);
    
    snprintf(debugText, sizeof(debugText), "P2: Act=%d Pos=%d,%d Face=%d HP=%d", 
             p2Action, p2X, p2Y, p2Face, p2HP);
    drawList->AddText(ImVec2(15, 50), IM_COL32(150, 150, 255, 255), debugText);
    
    snprintf(debugText, sizeof(debugText), "P1 Anim=%d | P2 Anim=%d", p1AnimIdx, p2AnimIdx);
    drawList->AddText(ImVec2(15, 65), IM_COL32(200, 200, 255, 255), debugText);
    
    // P1 box data (raw INT16 values from animation data)
    snprintf(debugText, sizeof(debugText), "P1 COLL: x=%d y=%d w=%d h=%d %s", 
             p1Coll.x, p1Coll.y, p1Coll.w, p1Coll.h, p1Coll.type ? "ACTIVE" : "");
    drawList->AddText(ImVec2(15, 80), IM_COL32(255, 255, 100, 255), debugText);
    
    snprintf(debugText, sizeof(debugText), "P1 HURT: x=%d y=%d w=%d h=%d %s", 
             p1Hurt.x, p1Hurt.y, p1Hurt.w, p1Hurt.h, p1Hurt.type ? "ACTIVE" : "");
    drawList->AddText(ImVec2(15, 95), IM_COL32(100, 255, 100, 255), debugText);
    
    snprintf(debugText, sizeof(debugText), "P1 HIT:  x=%d y=%d w=%d h=%d %s", 
             p1Hit.x, p1Hit.y, p1Hit.w, p1Hit.h, p1Hit.type ? "ACTIVE" : "");
    drawList->AddText(ImVec2(15, 110), IM_COL32(255, 100, 100, 255), debugText);

    // P2 box data (raw INT16 values from animation data)
    snprintf(debugText, sizeof(debugText), "P2 COLL: x=%d y=%d w=%d h=%d %s", 
             p2Coll.x, p2Coll.y, p2Coll.w, p2Coll.h, p2Coll.type ? "ACTIVE" : "");
    drawList->AddText(ImVec2(15, 135), IM_COL32(255, 255, 140, 255), debugText);
    
    snprintf(debugText, sizeof(debugText), "P2 HURT: x=%d y=%d w=%d h=%d %s", 
             p2Hurt.x, p2Hurt.y, p2Hurt.w, p2Hurt.h, p2Hurt.type ? "ACTIVE" : "");
    drawList->AddText(ImVec2(15, 150), IM_COL32(140, 255, 140, 255), debugText);
    
    snprintf(debugText, sizeof(debugText), "P2 HIT:  x=%d y=%d w=%d h=%d %s", 
             p2Hit.x, p2Hit.y, p2Hit.w, p2Hit.h, p2Hit.type ? "ACTIVE" : "");
    drawList->AddText(ImVec2(15, 165), IM_COL32(255, 140, 140, 255), debugText);
    
    // Camera info
    int16_t cameraX = ReadMemory<int16_t>(ADDR_CAMERA_X);
    int16_t cameraY = ReadMemory<int16_t>(ADDR_CAMERA_Y);
    snprintf(debugText, sizeof(debugText), "Camera: %d,%d", cameraX, cameraY);
    drawList->AddText(ImVec2(15, 190), IM_COL32(200, 200, 200, 255), debugText);
    
    // Instructions
    drawList->AddText(ImVec2(15, 215), IM_COL32(150, 255, 150, 255), "Yellow=Collision/Push, Green=Hurtbox, Red=Hitbox");
    drawList->AddText(ImVec2(15, 230), IM_COL32(150, 150, 150, 255), "Boxes read from per-animation data (anim+4104)");
    
    // Draw position markers
    ImVec2 p1Pos = WorldToScreen(p1X, p1Y, screenWidth, screenHeight);
    drawList->AddCircleFilled(p1Pos, 6.0f, IM_COL32(255, 100, 100, 255));
    drawList->AddLine(ImVec2(p1Pos.x - 10, p1Pos.y), ImVec2(p1Pos.x + 10, p1Pos.y), IM_COL32(255, 100, 100, 255), 2.0f);
    drawList->AddLine(ImVec2(p1Pos.x, p1Pos.y - 10), ImVec2(p1Pos.x, p1Pos.y + 10), IM_COL32(255, 100, 100, 255), 2.0f);
    
    ImVec2 p2Pos = WorldToScreen(p2X, p2Y, screenWidth, screenHeight);
    drawList->AddCircleFilled(p2Pos, 6.0f, IM_COL32(100, 100, 255, 255));
    drawList->AddLine(ImVec2(p2Pos.x - 10, p2Pos.y), ImVec2(p2Pos.x + 10, p2Pos.y), IM_COL32(100, 100, 255, 255), 2.0f);
    drawList->AddLine(ImVec2(p2Pos.x, p2Pos.y - 10), ImVec2(p2Pos.x, p2Pos.y + 10), IM_COL32(100, 100, 255, 255), 2.0f);
    
    auto drawBoxes = [&, p1AnimChanged, p2AnimChanged](int16_t baseX, int16_t baseY, int8_t face, const BoxData& hit, const BoxData& hurt, const BoxData& coll, bool isP1, ImU32 hitFill, ImU32 hitOut, ImU32 hurtFill, ImU32 hurtOut, ImU32 collFill, ImU32 collOut) {
        // Log box positions on animation change
        const bool animChanged = isP1 ? p1AnimChanged : p2AnimChanged;
        
        // Draw collision box (pushbox) - most common, test this first
        if (g_showCollision && coll.w > 0 && coll.h > 0) {
            // FIGHTING GAME BOX CONVENTION:
            // - X offset is mirrored by facing direction (1=right, -1=left)
            // - Y offset is from character base (negative = above ground)
            // - Width also needs to respect facing direction for proper rectangle corners
            // - Box is drawn from (x,y) as TOP-LEFT to (x+w, y+h) as BOTTOM-RIGHT
            int16_t offsetX = coll.x * 20 * face;  // Apply facing to offset
            int16_t offsetY = coll.y * 20;
            int16_t boxW = coll.w * 20 * (face < 0 ? -1 : 1);  // Width direction matches facing
            int16_t boxH = coll.h * 20;
            
            // Calculate actual corners
            int16_t boxX = baseX + offsetX;
            int16_t boxY = baseY + offsetY;
            
            if (animChanged) {
                LOG_INFO("[Hitbox] %s COLL: base(%d,%d) raw(%d,%d,%d,%d) offset(%d,%d) final_box(%d,%d,%d,%d)",
                         isP1 ? "P1" : "P2", baseX, baseY, coll.x, coll.y, coll.w, coll.h, 
                         offsetX, offsetY, boxX, boxY, boxW, boxH);
            }
            
            // Normalize rectangle (ensure p1.x < p2.x and p1.y < p2.y)
            ImVec2 bp1 = WorldToScreen(boxW > 0 ? boxX : boxX + boxW, boxY, screenWidth, screenHeight);
            ImVec2 bp2 = WorldToScreen(boxW > 0 ? boxX + boxW : boxX, boxY + boxH, screenWidth, screenHeight);
            
            if (animChanged) {
                LOG_INFO("[Hitbox] %s COLL screen: (%.1f,%.1f) to (%.1f,%.1f)", 
                         isP1 ? "P1" : "P2", bp1.x, bp1.y, bp2.x, bp2.y);
            }
            
            DrawBoxWithOutline(drawList, bp1, bp2, collFill, collOut);
        }
        
        // Draw hurtbox (vulnerable area)
        if (g_showHurtboxes && hurt.w > 0 && hurt.h > 0) {
            int16_t offsetX = hurt.x * 20 * face;
            int16_t offsetY = hurt.y * 20;
            int16_t boxW = hurt.w * 20 * (face < 0 ? -1 : 1);
            int16_t boxH = hurt.h * 20;
            int16_t boxX = baseX + offsetX;
            int16_t boxY = baseY + offsetY;
            
            if (animChanged) {
                LOG_INFO("[Hitbox] %s HURT: base(%d,%d) raw(%d,%d,%d,%d) offset(%d,%d) final_box(%d,%d,%d,%d)",
                         isP1 ? "P1" : "P2", baseX, baseY, hurt.x, hurt.y, hurt.w, hurt.h,
                         offsetX, offsetY, boxX, boxY, boxW, boxH);
            }
            
            ImVec2 bp1 = WorldToScreen(boxW > 0 ? boxX : boxX + boxW, boxY, screenWidth, screenHeight);
            ImVec2 bp2 = WorldToScreen(boxW > 0 ? boxX + boxW : boxX, boxY + boxH, screenWidth, screenHeight);
            DrawBoxWithOutline(drawList, bp1, bp2, hurtFill, hurtOut);
        }
        
        // Draw hitbox (attack box)
        if (g_showHitboxes && hit.w > 0 && hit.h > 0) {
            int16_t offsetX = hit.x * 20 * face;
            int16_t offsetY = hit.y * 20;
            int16_t boxW = hit.w * 20 * (face < 0 ? -1 : 1);
            int16_t boxH = hit.h * 20;
            int16_t boxX = baseX + offsetX;
            int16_t boxY = baseY + offsetY;
            
            if (animChanged) {
                LOG_INFO("[Hitbox] %s HIT: base(%d,%d) raw(%d,%d,%d,%d) offset(%d,%d) final_box(%d,%d,%d,%d)",
                         isP1 ? "P1" : "P2", baseX, baseY, hit.x, hit.y, hit.w, hit.h,
                         offsetX, offsetY, boxX, boxY, boxW, boxH);
            }
            
            ImVec2 bp1 = WorldToScreen(boxW > 0 ? boxX : boxX + boxW, boxY, screenWidth, screenHeight);
            ImVec2 bp2 = WorldToScreen(boxW > 0 ? boxX + boxW : boxX, boxY + boxH, screenWidth, screenHeight);
            DrawBoxWithOutline(drawList, bp1, bp2, hitFill, hitOut);
        }
    };

    drawBoxes(p1X, p1Y, p1Face, p1Hit, p1Hurt, p1Coll, true, COLOR_HITBOX, COLOR_HITBOX_OUT, COLOR_HURTBOX, COLOR_HURTBOX_OUT, COLOR_COLLISION, COLOR_COLL_OUT);
    drawBoxes(p2X, p2Y, p2Face, p2Hit, p2Hurt, p2Coll, false, COLOR_HITBOX, COLOR_HITBOX_OUT, COLOR_HURTBOX, COLOR_HURTBOX_OUT, COLOR_COLLISION, COLOR_COLL_OUT);
}

void HitboxDisplay_SetEnabled(bool enabled) {
    g_enabled = enabled;
    if (enabled) {
        g_lastP1Action = 0;
        g_lastP2Action = 0;
    }
    LOG_INFO("[HITBOX] Display %s", enabled ? "enabled" : "disabled");
}

bool HitboxDisplay_IsEnabled() {
    return g_enabled;
}

void HitboxDisplay_SetShowHitboxes(bool show) {
    g_showHitboxes = show;
}

void HitboxDisplay_SetShowHurtboxes(bool show) {
    g_showHurtboxes = show;
}

void HitboxDisplay_SetShowCollision(bool show) {
    g_showCollision = show;
}

void HitboxDisplay_SetShowPushbox(bool show) {
    g_showCollision = show;
}

bool HitboxDisplay_GetShowHitboxes() {
    return g_showHitboxes;
}

bool HitboxDisplay_GetShowHurtboxes() {
    return g_showHurtboxes;
}

bool HitboxDisplay_GetShowCollision() {
    return g_showCollision;
}

bool HitboxDisplay_GetShowPushbox() {
    return g_showCollision;
}

void HitboxDisplay_RenderControls() {
    // Master enable/disable
    ImGui::Checkbox("Enable Hitbox Display", &g_enabled);
    ImGui::Separator();
    
    ImGui::Checkbox("Log on Action Change", &g_logOnActionChange);
    ImGui::Checkbox("Auto Scan (actions >5)", &g_scanForHitboxValues);
    ImGui::Separator();
    ImGui::Checkbox("Show Hitboxes", &g_showHitboxes);
    ImGui::Checkbox("Show Hurtboxes", &g_showHurtboxes);
    ImGui::Checkbox("Show Collision", &g_showCollision);
    
    if (ImGui::Button("Force Full Scan P1")) {
        if (AS2_IsInMatch()) {
            uintptr_t base = ReadMemory<uint32_t>(ADDR_ENTITY_ARRAY_ALT);
            if (base) {
                ScanForHitboxPatterns("P1 (forced)", base, ReadMemory<uint32_t>(base + OFF_ACTION_ID));
            }
        }
    }
}
