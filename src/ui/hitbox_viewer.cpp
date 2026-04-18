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
 *   Offset  8-39 : Attack hitboxes (4 entries × 8B)
 *   Offset 40-71 : Hurtboxes / primary vulnerable boxes (4 entries × 8B)
 *   Offset 72-103: Extended hurtboxes / melee-only vulnerable area (4 entries × 8B)
 *                  Checked by Entity_UpdateDamageApplication (player melee only),
 *                  Entity_UpdateThrowInteraction (tech throw: mutual @72 overlap).
 *                  NOT checked by Entity_UpdateSummonHitDetection (summons use @40).
 *   Game resolution : 640 × 480 logical.
 */

#include "hitbox_viewer.h"
#include "mod_main.h"
#include "memory_utils.h"
#include "as2_constants.h"
#include "log_window.h"
#include "game_state.h"
#include "imgui.h"
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>

// ============================================================================
// Toggle state
// ============================================================================

static bool  g_enabled         = false;
static bool  g_showHurtboxes   = true;
static bool  g_showHitboxes    = true;
static bool  g_showPushboxes   = true;
static bool  g_showThrowboxes  = true;
static bool  g_showPositions   = true;
static bool  g_showStateFlags  = true;
static bool  g_showInfo        = false;
static bool  g_showMatchState  = false;
static float g_fillAlpha       = 0.25f;
static bool  g_logPerFrame     = false;

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
static const ImU32 COL_THROWBOX  = IM_COL32(0, 200, 255, 255);
static const ImU32 COL_HITDEF_PT = IM_COL32(255, 80, 80, 255);
static const ImU32 COL_P1_CROSS  = IM_COL32(255, 120, 120, 255);
static const ImU32 COL_P2_CROSS  = IM_COL32(120, 120, 255, 255);
static const ImU32 COL_CANCEL    = IM_COL32(255, 245, 120, 255);
static const ImU32 COL_INVINCIBLE = IM_COL32(90, 175, 255, 255);
static const ImU32 COL_ARMOR      = IM_COL32(255, 160, 0, 255);
static const ImU32 COL_IMMUNE     = IM_COL32(180, 0, 255, 255);
static const ImU32 COL_PROX_GUARD = IM_COL32(100, 220, 255, 255);

// ============================================================================
// Helpers
// ============================================================================

static const float GAME_W = 640.0f;
static const float GAME_H = 480.0f;

struct ScreenTransform {
    int16_t scrollX;
    int16_t scrollY;
    float   scaleX;
    float   scaleY;
};

enum class BoxRenderPass {
    FillOnly,
    OutlineOnly,
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

static ImVec2 GameToDisplay(float gx, float gy, const ScreenTransform& t) {
    return ImVec2(gx * t.scaleX, gy * t.scaleY);
}

static ImVec2 WorldToDisplay(int16_t wx, int16_t wy, const ScreenTransform& t) {
    float gx = (float)(wx / 10) - (float)t.scrollX;
    float gy = (float)(wy / 10) - (float)t.scrollY;
    return GameToDisplay(gx, gy, t);
}

static void DrawBox(ImDrawList* dl, float l, float top, float r, float bot,
                    ImU32 outline, float alpha, const ScreenTransform& t,
                    BoxRenderPass pass, float outlinePadding = 0.0f) {
    ImVec2 p1 = GameToDisplay(l, top, t);
    ImVec2 p2 = GameToDisplay(r, bot, t);
    if (p1.x > p2.x) { float tmp = p1.x; p1.x = p2.x; p2.x = tmp; }
    if (p1.y > p2.y) { float tmp = p1.y; p1.y = p2.y; p2.y = tmp; }
    if (pass == BoxRenderPass::FillOnly) {
        dl->AddRectFilled(p1, p2, ColFill(outline, alpha));
        return;
    }
    if (outlinePadding != 0.0f) {
        p1.x -= outlinePadding;
        p1.y -= outlinePadding;
        p2.x += outlinePadding;
        p2.y += outlinePadding;
    }
    dl->AddRect(p1, p2, outline, 0.0f, 0, 2.0f);
}

struct BoxEntry {
    int16_t xOff;
    int16_t yOff;
    int16_t halfW;
    int16_t halfH;
};

struct ClashSnapshot {
    uint8_t rank;
    int16_t rawA;
    int16_t rawB;
    int16_t rawC;
    int16_t rawD;
    uint32_t continuationId;
};

struct MaxHitSnapshot {
    uint16_t rawA;
    uint16_t rawB;
    uint16_t rawC;
    int16_t rawD;
    uint32_t rawId;
    uint32_t active;
    uint8_t marker1948;
    uint8_t marker1949;
};

struct CancelRouteSnapshot {
    uint32_t charId;
    uint16_t meter;
    uint8_t familyType;
    uint8_t gate16;
    uint8_t gate22;
    uint8_t gate26;
    uint8_t airborne;
    uint16_t airMoveInput;
    uint32_t gateState;
    uint8_t command20Timer;
    uint8_t command21Timer;
    uint16_t super1Gate;
    uint8_t super1Lock;
    uint16_t super2GateA;
    uint16_t super2GateB;
    uint16_t super2GateC;
    uint16_t super2GateD;
    uint16_t action107Threshold;
    uint16_t case20Action;
    uint16_t case21Action;
    uint8_t case20Ready;
    uint8_t case21Ready;
    uint8_t case21State4Path;
    uint8_t readyActionCount;
    uint16_t readyActions[4];
};

struct Action107RouteConfig {
    bool valid;
    uintptr_t gateOffset;
    uint16_t meterThreshold;
};

static constexpr uint8_t kCancelFamilyTypeByCharId[22] = {
    3, 6, 3, 2, 6, 3, 5, 4, 2, 5, 4, 5, 6, 3, 4, 5, 4, 1, 3, 5, 5, 1,
};

static constexpr uintptr_t kEntityOffRawInputWord30 = 30;
static constexpr uintptr_t kEntityOffAirMoveInput = 78;
static constexpr uintptr_t kEntityOffCommandSlot16Gate = 783;
static constexpr uintptr_t kEntityOffCommandSlot22Gate = 807;
static constexpr uintptr_t kEntityOffCommandSlot26Gate = 823;
static constexpr uintptr_t kEntityOffCancelWindowAux = 1672;
static constexpr uintptr_t kEntityOffCancelWindowKind = 1673;
static constexpr uintptr_t kEntityOffSuper1Gate = 146;
static constexpr uintptr_t kEntityOffSuper1Lock = 434;
static constexpr uintptr_t kEntityOffSuper2GateA = 130;
static constexpr uintptr_t kEntityOffSuper2GateB = 76;
static constexpr uintptr_t kEntityOffSuper2GateC = 74;
static constexpr uintptr_t kEntityOffSuper2GateD = 132;
static constexpr uintptr_t kEntityOffAirborneFlag = 1732;
static constexpr uintptr_t kEntityOffCancelGateState = 1952;
static constexpr uint16_t kAction49 = 49;
static constexpr uint16_t kAction52 = 52;
static constexpr uint16_t kAction59 = 59;
static constexpr uint16_t kAction60 = 60;
static constexpr uint16_t kAction61 = 61;
static constexpr uint16_t kAction62 = 62;
static constexpr uint16_t kAction107 = 107;
static constexpr uint16_t kAction137 = 137;
static constexpr uint16_t kAction138 = 138;
static constexpr uint16_t kAction139 = 139;
static constexpr uint16_t kAction140 = 140;

// Action-ID labels verified against decomp shared handlers and AI-reference
// patterns. See mod/docs/frame_advantage_design.md for the full table.
static const char* ActionLabel(uint32_t actionId) {
    switch (actionId) {
        case 2:  return "NeutralStand";
        case 4:  return "WalkFwd";
        case 5:  return "WalkBack";
        case 7:  return "NeutralCrouch";
        case 10: return "PrejumpN";
        case 11: return "PrejumpFwd";
        case 12: return "PrejumpBack";
        case 13: return "DoubleJump";
        case 22: return "JumpAir";
        case 23: return "Landing";
        case 63: return "StandProxGuard";
        case 64: return "StandBlockHold";
        case 65: return "StandBlockHit";
        case 66: return "CrouchProxGuard";
        case 67: return "CrouchBlockHold";
        case 68: return "CrouchBlockHit";
        case 69: return "AirProxGuard";
        case 70: return "AirBlockHold";
        case 71: return "AirBlockHit";
        case 72: return "Hitstun";
        case 73: return "Launched";
        case 74: return "WakeupNoTech";
        case 78: return "AirTech";
        case 79: return "GroundTechFwd";
        case 80: return "GroundTechNeutral";
        case 81: return "GroundTechBack";
        default: return nullptr;
    }
}

struct HitboxViewerFrameContext {
    ScreenTransform transform;
    uint32_t gameMode;
    uint32_t subState;
    uint32_t gameType;
    uint32_t simFrame;
    uint32_t dispFrame;
    uint32_t phaseTimer;
    uint32_t roundTimer;
    int16_t scrollVicinity[8];
};

struct AnimFrameSnapshot {
    bool valid;
    uintptr_t entityBase;
    uint32_t animIdx;
    uint8_t rawFrame[ANIM_DATA_STRIDE];
    BoxEntry collisionBox;
    BoxEntry hitBoxes[HURTBOX_COUNT_PER_FRAME];
    BoxEntry hurtBoxes[HURTBOX_COUNT_PER_FRAME];
    BoxEntry throwBoxes[HURTBOX_COUNT_PER_FRAME];
};

struct EntitySnapshot {
    const char* label;
    int playerIndex;
    uintptr_t base;
    int16_t posX;
    int16_t posY;
    int8_t facing;
    int16_t hp;
    uint32_t animIdx;
    uint32_t actionId;
    uint16_t actionPhase;
    uint16_t actionFrame;
    uint8_t attackState;
    uint32_t attackType;
    uint8_t hitActive;
    ClashSnapshot clash;
    MaxHitSnapshot maxHit;
    CancelRouteSnapshot cancelRoutes;
    const AnimFrameSnapshot* animFrame;
};

struct HitDefSnapshot {
    int index;
    uint32_t id;
    uint8_t owner;
    uint8_t animOwner;
    uint16_t summonAnimIdx;
    int16_t worldX;
    int16_t worldY;
    int8_t facing;
    uint8_t type;
    uint8_t invuln;
    uint8_t flag;
    uint32_t damage;
    const AnimFrameSnapshot* animFrame;
};

struct AnimFrameCache {
    static const int kMaxEntries = SUMMON_MAX_SLOTS + 2;
    AnimFrameSnapshot entries[kMaxEntries];
    int count;
};

template<typename T>
static T ReadLocalValue(const uint8_t* bytes, size_t offset) {
    T value{};
    if (!bytes) {
        return value;
    }
    memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

static BoxEntry DecodeBoxEntry(const uint8_t* frameBytes, size_t offset);
static void DecodeBoxEntries(const uint8_t* frameBytes, int frameOffset, BoxEntry* out, int count);
static int CountActiveBoxes(const BoxEntry* boxes, int count);
static uintptr_t GetAnimFrameBase(uintptr_t entityBase, uint32_t animIdx);
static void CopyAnimFrameBytes(uintptr_t entityBase, uint32_t animIdx, uint8_t* outFrame);
static void LogAnimFrameSnapshot(const AnimFrameSnapshot& snapshot, const char* label);
static const AnimFrameSnapshot* GetOrLoadAnimFrameSnapshot(AnimFrameCache* cache,
                                                           uintptr_t entityBase,
                                                           uint32_t animIdx,
                                                           const char* label);
static HitboxViewerFrameContext BuildFrameContext();
static EntitySnapshot BuildEntitySnapshot(AnimFrameCache* cache,
                                          uintptr_t entityBase,
                                          const char* label);
template<typename T>
static T ReadHitDefValue(const uint8_t* entryBytes, uintptr_t entryBase, size_t offset);
static int BuildHitDefSnapshots(AnimFrameCache* cache,
                                HitDefSnapshot* out,
                                int capacity,
                                uintptr_t p1EntityBase,
                                uintptr_t p2EntityBase);
static void RenderBoxSet(ImDrawList* dl,
                         const EntitySnapshot& entity,
                         const BoxEntry* boxes,
                         int count,
                         ImU32 colour,
                         const ScreenTransform& t,
                         const char* boxLabel,
                         BoxRenderPass pass,
                         float outlinePadding = 0.0f);
static void RenderHurtboxes(ImDrawList* dl, const EntitySnapshot& entity, const ScreenTransform& t, BoxRenderPass pass);
static void RenderEntityHitboxes(ImDrawList* dl, const EntitySnapshot& entity, const ScreenTransform& t, BoxRenderPass pass);
static void RenderThrowboxes(ImDrawList* dl, const EntitySnapshot& entity, const ScreenTransform& t, BoxRenderPass pass);
static void RenderStateFlags(ImDrawList* dl, const EntitySnapshot& entity, const ScreenTransform& t);
static void RenderHitDefs(ImDrawList* dl, const HitDefSnapshot* hitDefs, int hitDefCount, const ScreenTransform& t, BoxRenderPass pass);
static void RenderPushbox(ImDrawList* dl, const EntitySnapshot& entity, const ScreenTransform& t, BoxRenderPass pass);
static void RenderPosition(ImDrawList* dl, const EntitySnapshot& entity, ImU32 colour, const ScreenTransform& t);
static void RenderInfoOverlay(ImDrawList* dl, const HitboxViewerFrameContext& ctx, const EntitySnapshot& p1, const EntitySnapshot& p2);
static uint32_t ReadPlayerCharacterId(int playerIndex);
static bool IsCancelGroundAction(uint32_t actionId);
static bool IsCancelAirAction(uint32_t actionId);
static Action107RouteConfig GetAction107RouteConfig(uint32_t charId);
static void AppendReadyCancelAction(CancelRouteSnapshot* snapshot, uint16_t actionId);
static void FormatReadyCancelActions(const CancelRouteSnapshot& snapshot, char* out, size_t outCap);
static CancelRouteSnapshot BuildCancelRouteSnapshot(int playerIndex,
                                                    uintptr_t entityBase,
                                                    uint32_t actionId,
                                                    int16_t posY);

static BoxEntry DecodeBoxEntry(const uint8_t* frameBytes, size_t offset) {
    BoxEntry box{};
    box.xOff = ReadLocalValue<int16_t>(frameBytes, offset + 0);
    box.yOff = ReadLocalValue<int16_t>(frameBytes, offset + 2);
    box.halfW = ReadLocalValue<int16_t>(frameBytes, offset + 4);
    box.halfH = ReadLocalValue<int16_t>(frameBytes, offset + 6);
    return box;
}

static void DecodeBoxEntries(const uint8_t* frameBytes, int frameOffset, BoxEntry* out, int count) {
    if (!out) {
        return;
    }
    for (int i = 0; i < count; i++) {
        out[i] = DecodeBoxEntry(frameBytes, frameOffset + (i * HURTBOX_ENTRY_SIZE));
    }
}

static int CountActiveBoxes(const BoxEntry* boxes, int count) {
    int active = 0;
    if (!boxes) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (boxes[i].halfW > 0 && boxes[i].halfH > 0) {
            active++;
        }
    }
    return active;
}

static uintptr_t GetAnimFrameBase(uintptr_t entityBase, uint32_t animIdx) {
    return entityBase + ENTITY_OFF_ANIM_DATA + ((uintptr_t)ANIM_DATA_STRIDE * animIdx);
}

static void CopyAnimFrameBytes(uintptr_t entityBase, uint32_t animIdx, uint8_t* outFrame) {
    if (!outFrame) {
        return;
    }

    const uintptr_t frameBase = GetAnimFrameBase(entityBase, animIdx);
    if (CopyMemorySafe(outFrame, (const void*)frameBase, ANIM_DATA_STRIDE)) {
        return;
    }

    for (int i = 0; i < ANIM_DATA_STRIDE; i++) {
        outFrame[i] = ReadMemory<uint8_t>(frameBase + i);
    }
}

static void LogAnimFrameSnapshot(const AnimFrameSnapshot& snapshot, const char* label) {
    if (!g_logPerFrame || !label || !label[0] || !snapshot.valid) {
        return;
    }

    const uintptr_t frameBase = GetAnimFrameBase(snapshot.entityBase, snapshot.animIdx);
    LOG_INFO("[HBV] %s animIdx=%u entity=0x%08X frameBase=0x%08X",
             label, snapshot.animIdx, (uint32_t)snapshot.entityBase, (uint32_t)frameBase);

    auto logBoxes = [&](const char* groupLabel, const BoxEntry* boxes, int count, int frameOffset) {
        for (int i = 0; i < count; i++) {
            const BoxEntry& box = boxes[i];
            LOG_INFO("[HBV]   %s.%s box[%d] @0x%08X: off(%d,%d) half(%d,%d) %s",
                     label,
                     groupLabel,
                     i,
                     (uint32_t)(frameBase + frameOffset + ((uintptr_t)i * HURTBOX_ENTRY_SIZE)),
                     box.xOff,
                     box.yOff,
                     box.halfW,
                     box.halfH,
                     (box.halfW > 0 && box.halfH > 0) ? "ACTIVE" : "skip");
        }
    };

    logBoxes("coll", &snapshot.collisionBox, 1, ANIM_COLLISION_OFFSET);
    logBoxes("hit", snapshot.hitBoxes, HURTBOX_COUNT_PER_FRAME, ANIM_HITBOX_OFFSET);
    logBoxes("hurt", snapshot.hurtBoxes, HURTBOX_COUNT_PER_FRAME, ANIM_HURTBOX_OFFSET);
    logBoxes("ext", snapshot.throwBoxes, HURTBOX_COUNT_PER_FRAME, ANIM_EXT_HURTBOX_OFFSET);

    char hex[256];
    int pos = 0;
    for (int i = 0; i < ANIM_DATA_STRIDE && pos < 240; i++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", snapshot.rawFrame[i]);
    }
    LOG_INFO("[HBV] %s raw frame[%u] 104B: %s", label, snapshot.animIdx, hex);
}

static const AnimFrameSnapshot* GetOrLoadAnimFrameSnapshot(AnimFrameCache* cache,
                                                           uintptr_t entityBase,
                                                           uint32_t animIdx,
                                                           const char* label) {
    if (!cache) {
        return nullptr;
    }

    for (int i = 0; i < cache->count; i++) {
        AnimFrameSnapshot& cached = cache->entries[i];
        if (cached.valid &&
            cached.entityBase == entityBase &&
            cached.animIdx == animIdx) {
            return &cached;
        }
    }

    if (cache->count >= AnimFrameCache::kMaxEntries) {
        return nullptr;
    }

    AnimFrameSnapshot& snapshot = cache->entries[cache->count++];
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.entityBase = entityBase;
    snapshot.animIdx = animIdx;

    CopyAnimFrameBytes(entityBase, animIdx, snapshot.rawFrame);
    snapshot.collisionBox = DecodeBoxEntry(snapshot.rawFrame, ANIM_COLLISION_OFFSET);
    DecodeBoxEntries(snapshot.rawFrame, ANIM_HITBOX_OFFSET, snapshot.hitBoxes, HURTBOX_COUNT_PER_FRAME);
    DecodeBoxEntries(snapshot.rawFrame, ANIM_HURTBOX_OFFSET, snapshot.hurtBoxes, HURTBOX_COUNT_PER_FRAME);
    DecodeBoxEntries(snapshot.rawFrame, ANIM_EXT_HURTBOX_OFFSET, snapshot.throwBoxes, HURTBOX_COUNT_PER_FRAME);

    LogAnimFrameSnapshot(snapshot, label);
    return &snapshot;
}

static HitboxViewerFrameContext BuildFrameContext() {
    HitboxViewerFrameContext ctx{};
    ctx.transform = GetTransform();
    ctx.gameMode = GetGameMode();
    ctx.subState = GetSubstate();
    ctx.gameType = GetGameType();
    ctx.simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    ctx.dispFrame = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    ctx.phaseTimer = GetMatchPhaseTimer();
    ctx.roundTimer = ReadMemory<uint32_t>(ADDR_ROUND_TIMER);

    int scrollIndex = 0;
    for (int i = -4; i < 12 && scrollIndex < 8; i += 2) {
        ctx.scrollVicinity[scrollIndex++] = ReadMemory<int16_t>(ADDR_SCROLL_X + i);
    }
    return ctx;
}

static uint32_t ReadPlayerCharacterId(int playerIndex) {
    return ReadMemory<uint32_t>(playerIndex == 0 ? ADDR_CHARSEL_P1_CHAR_ID : ADDR_CHARSEL_P2_CHAR_ID);
}

static bool IsCancelGroundAction(uint32_t actionId) {
    return actionId == 64 || actionId == 65 || actionId == 67 || actionId == 68;
}

static bool IsCancelAirAction(uint32_t actionId) {
    return actionId == 70 || actionId == 71;
}

static Action107RouteConfig GetAction107RouteConfig(uint32_t charId) {
    switch (charId) {
        case 9:
            return {true, kEntityOffCommandSlot16Gate, 0x07D0u};
        case 10:
            return {true, kEntityOffCommandSlot16Gate, 0x1388u};
        case 11:
            return {true, kEntityOffCommandSlot16Gate, 0x01F4u};
        case 12:
            return {true, kEntityOffCommandSlot16Gate, 0x0BB8u};
        case 16:
            return {true, kEntityOffCommandSlot26Gate, 0x03E8u};
        case 18:
            return {true, kEntityOffCommandSlot22Gate, 0x07D0u};
        case 19:
            return {true, kEntityOffCommandSlot16Gate, 0x03E8u};
        case 20:
            return {true, kEntityOffCommandSlot16Gate, 0x03E8u};
        default:
            return {false, 0, 0};
    }
}

static void AppendReadyCancelAction(CancelRouteSnapshot* snapshot, uint16_t actionId) {
    if (!snapshot) {
        return;
    }

    for (uint8_t i = 0; i < snapshot->readyActionCount; ++i) {
        if (snapshot->readyActions[i] == actionId) {
            return;
        }
    }

    if (snapshot->readyActionCount >= static_cast<uint8_t>(sizeof(snapshot->readyActions) / sizeof(snapshot->readyActions[0]))) {
        return;
    }

    snapshot->readyActions[snapshot->readyActionCount++] = actionId;
}

static void FormatReadyCancelActions(const CancelRouteSnapshot& snapshot, char* out, size_t outCap) {
    if (!out || outCap == 0) {
        return;
    }

    out[0] = '\0';
    if (snapshot.readyActionCount == 0) {
        snprintf(out, outCap, "none");
        return;
    }

    size_t pos = 0;
    for (uint8_t i = 0; i < snapshot.readyActionCount && pos + 1 < outCap; ++i) {
        pos += snprintf(out + pos, outCap - pos, i == 0 ? "%u" : ",%u", snapshot.readyActions[i]);
        if (pos >= outCap) {
            out[outCap - 1] = '\0';
            return;
        }
    }
}

static CancelRouteSnapshot BuildCancelRouteSnapshot(int playerIndex,
                                                    uintptr_t entityBase,
                                                    uint32_t actionId,
                                                    int16_t posY) {
    CancelRouteSnapshot snapshot{};
    snapshot.charId = ReadPlayerCharacterId(playerIndex);
    snapshot.meter = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_METER);
    if (snapshot.charId < (sizeof(kCancelFamilyTypeByCharId) / sizeof(kCancelFamilyTypeByCharId[0]))) {
        snapshot.familyType = kCancelFamilyTypeByCharId[snapshot.charId];
    }
    snapshot.gate16 = ReadMemory<uint8_t>(entityBase + kEntityOffCommandSlot16Gate);
    snapshot.gate22 = ReadMemory<uint8_t>(entityBase + kEntityOffCommandSlot22Gate);
    snapshot.gate26 = ReadMemory<uint8_t>(entityBase + kEntityOffCommandSlot26Gate);
    snapshot.airborne = ReadMemory<uint8_t>(entityBase + kEntityOffAirborneFlag);
    snapshot.airMoveInput = ReadMemory<uint16_t>(entityBase + kEntityOffAirMoveInput);
    snapshot.gateState = ReadMemory<uint32_t>(entityBase + kEntityOffCancelGateState);
    snapshot.command20Timer = ReadMemory<uint8_t>(entityBase + kEntityOffCancelWindowAux);
    snapshot.command21Timer = ReadMemory<uint8_t>(entityBase + kEntityOffCancelWindowKind);
    snapshot.super1Gate = ReadMemory<uint16_t>(entityBase + kEntityOffSuper1Gate);
    snapshot.super1Lock = ReadMemory<uint8_t>(entityBase + kEntityOffSuper1Lock);
    snapshot.super2GateA = ReadMemory<uint16_t>(entityBase + kEntityOffSuper2GateA);
    snapshot.super2GateB = ReadMemory<uint16_t>(entityBase + kEntityOffSuper2GateB);
    snapshot.super2GateC = ReadMemory<uint16_t>(entityBase + kEntityOffSuper2GateC);
    snapshot.super2GateD = ReadMemory<uint16_t>(entityBase + kEntityOffSuper2GateD);

    if (snapshot.command20Timer != 0 &&
        snapshot.super1Gate != 0 &&
        snapshot.meter >= 0x03E8u &&
        snapshot.super1Lock == 0) {
        if (snapshot.airborne == 0) {
            snapshot.case20Action = kAction137;
            snapshot.case20Ready = 1;
        } else if (posY < 7280) {
            snapshot.case20Action = kAction138;
            snapshot.case20Ready = 1;
        }
    }

    const bool super2Window = ((snapshot.super2GateA != 0 && snapshot.super2GateB == 1) ||
                               (snapshot.super2GateC == 1 && snapshot.super2GateD != 0));
    if (snapshot.command21Timer != 0 && super2Window && snapshot.gateState == 4u) {
        // Another 139/140 caller (AI_DecideAirTech in the refactored decomp) accepts
        // gateState==4 without the normal 0x1F4 meter threshold. Keep that path visible
        // in the overlay separately until the human-facing route is fully confirmed.
        snapshot.case21State4Path = 1;
    }
    if (snapshot.command21Timer != 0 && super2Window && snapshot.meter >= 0x01F4u) {
        if (snapshot.airborne == 0) {
            snapshot.case21Action = kAction139;
            snapshot.case21Ready = 1;
        } else if (posY < 7280) {
            snapshot.case21Action = kAction140;
            snapshot.case21Ready = 1;
        }
    }

    const Action107RouteConfig action107 = GetAction107RouteConfig(snapshot.charId);
    snapshot.action107Threshold = action107.meterThreshold;
    if (action107.valid) {
        const uint8_t gate = ReadMemory<uint8_t>(entityBase + action107.gateOffset);
        if (gate != 0 &&
            snapshot.gateState == 1 &&
            snapshot.airborne == 0 &&
            snapshot.meter >= action107.meterThreshold) {
            AppendReadyCancelAction(&snapshot, kAction107);
        }
    }

    if (snapshot.familyType == 5 && snapshot.gate26 != 0) {
        if (IsCancelGroundAction(actionId)) {
            AppendReadyCancelAction(&snapshot, kAction49);
        }
        if (IsCancelAirAction(actionId) && posY < 6960) {
            AppendReadyCancelAction(&snapshot, kAction52);
        }
    }

    if (snapshot.familyType == 6 && snapshot.meter >= 0x01F4u && snapshot.airMoveInput == 1) {
        if (IsCancelGroundAction(actionId)) {
            AppendReadyCancelAction(&snapshot,
                                    ReadMemory<uint16_t>(entityBase + kEntityOffRawInputWord30) == 1 ? kAction60 : kAction59);
        }
        if (IsCancelAirAction(actionId)) {
            AppendReadyCancelAction(&snapshot,
                                    ReadMemory<uint16_t>(entityBase + kEntityOffRawInputWord30) == 1 ? kAction62 : kAction61);
        }
    }

    return snapshot;
}

static EntitySnapshot BuildEntitySnapshot(AnimFrameCache* cache,
                                          uintptr_t entityBase,
                                          int playerIndex,
                                          const char* label) {
    EntitySnapshot snapshot{};
    snapshot.label = label;
    snapshot.playerIndex = playerIndex;
    snapshot.base = entityBase;
    snapshot.posX = ReadMemory<int16_t>(entityBase + ENTITY_OFF_X_POS);
    snapshot.posY = ReadMemory<int16_t>(entityBase + ENTITY_OFF_Y_POS);
    snapshot.facing = ReadMemory<int8_t>(entityBase + ENTITY_OFF_FACING);
    snapshot.hp = ReadMemory<int16_t>(entityBase + ENTITY_OFF_HP);
    snapshot.animIdx = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_ANIM_INDEX);
    snapshot.actionId = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_ACTION_ID);
    snapshot.actionPhase = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_ACTION_PHASE);
    snapshot.actionFrame = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_ACTION_FRAME);
    snapshot.attackState = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_ATTACK_STATE);
    snapshot.attackType = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_ATTACK_TYPE);
    snapshot.hitActive = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_HIT_ACTIVE);
    snapshot.clash.rank = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_CLASH_RANK);
    snapshot.clash.rawA = ReadMemory<int16_t>(entityBase + ENTITY_OFF_CLASH_RAW_A);
    snapshot.clash.rawB = ReadMemory<int16_t>(entityBase + ENTITY_OFF_CLASH_RAW_B);
    snapshot.clash.rawC = ReadMemory<int16_t>(entityBase + ENTITY_OFF_CLASH_RAW_C);
    snapshot.clash.rawD = ReadMemory<int16_t>(entityBase + ENTITY_OFF_CLASH_RAW_D);
    snapshot.clash.continuationId = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_CLASH_ID);
    snapshot.maxHit.rawA = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_MAX_HIT_RAW_A);
    snapshot.maxHit.rawB = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_MAX_HIT_RAW_B);
    snapshot.maxHit.rawC = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_MAX_HIT_RAW_C);
    snapshot.maxHit.rawD = ReadMemory<int16_t>(entityBase + ENTITY_OFF_MAX_HIT_RAW_D);
    snapshot.maxHit.rawId = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_MAX_HIT_ID);
    snapshot.maxHit.active = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_MAX_HIT_ACTIVE);
    snapshot.maxHit.marker1948 = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_HIT_MARKER_1948);
    snapshot.maxHit.marker1949 = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_HIT_MARKER_1949);
    snapshot.cancelRoutes = BuildCancelRouteSnapshot(playerIndex, entityBase, snapshot.actionId, snapshot.posY);
    snapshot.animFrame = GetOrLoadAnimFrameSnapshot(cache, entityBase, snapshot.animIdx, label);
    return snapshot;
}

template<typename T>
static T ReadHitDefValue(const uint8_t* entryBytes, uintptr_t entryBase, size_t offset) {
    if (entryBytes) {
        return ReadLocalValue<T>(entryBytes, offset);
    }
    return ReadMemory<T>(entryBase + offset);
}

static int BuildHitDefSnapshots(AnimFrameCache* cache,
                                HitDefSnapshot* out,
                                int capacity,
                                uintptr_t p1EntityBase,
                                uintptr_t p2EntityBase) {
    if (!cache || !out || capacity <= 0) {
        return 0;
    }

    uint8_t summonBytes[SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE] = {};
    const bool bulkCopied = CopyMemorySafe(summonBytes, (const void*)ADDR_SUMMON_ARRAY, sizeof(summonBytes));
    int count = 0;

    for (int i = 0; i < SUMMON_MAX_SLOTS && count < capacity; i++) {
        const uintptr_t entryBase = ADDR_SUMMON_ARRAY + ((uintptr_t)i * SUMMON_ENTRY_SIZE);
        const uint8_t* entryBytes = bulkCopied ? (summonBytes + ((uintptr_t)i * SUMMON_ENTRY_SIZE)) : nullptr;

        const uint32_t id = ReadHitDefValue<uint32_t>(entryBytes, entryBase, HITDEF_OFF_ID);
        if (id == 0) {
            continue;
        }

        const uint8_t owner = ReadHitDefValue<uint8_t>(entryBytes, entryBase, HITDEF_OFF_OWNER);
        if (owner == 0xFF) {
            continue;
        }

        const uint16_t summonAnimIdx = ReadHitDefValue<uint16_t>(entryBytes, entryBase, HITDEF_OFF_ANIM_FRAME_IDX);
        if (summonAnimIdx == 0xFFFF) {
            continue;
        }

        HitDefSnapshot& snapshot = out[count++];
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.index = i;
        snapshot.id = id;
        snapshot.owner = owner;
        snapshot.animOwner = ReadHitDefValue<uint8_t>(entryBytes, entryBase, HITDEF_OFF_ANIM_OWNER);
        snapshot.summonAnimIdx = summonAnimIdx;
        snapshot.worldX = ReadHitDefValue<int16_t>(entryBytes, entryBase, HITDEF_OFF_X);
        snapshot.worldY = ReadHitDefValue<int16_t>(entryBytes, entryBase, HITDEF_OFF_Y);
        snapshot.facing = ReadHitDefValue<int8_t>(entryBytes, entryBase, HITDEF_OFF_FACING);
        snapshot.type = ReadHitDefValue<uint8_t>(entryBytes, entryBase, HITDEF_OFF_TYPE);
        snapshot.invuln = ReadHitDefValue<uint8_t>(entryBytes, entryBase, HITDEF_OFF_ACTIVE);
        snapshot.flag = ReadHitDefValue<uint8_t>(entryBytes, entryBase, HITDEF_OFF_ACTIVE_FLAG);
        snapshot.damage = ReadHitDefValue<uint32_t>(entryBytes, entryBase, HITDEF_OFF_DAMAGE);

        const uintptr_t ownerEntity = (snapshot.animOwner == 0)
            ? p1EntityBase
            : p2EntityBase;

        char label[32];
        const char* logLabel = nullptr;
        if (g_logPerFrame) {
            snprintf(label, sizeof(label), "HitDef[%d]", i);
            logLabel = label;
        }
        snapshot.animFrame = GetOrLoadAnimFrameSnapshot(cache, ownerEntity, snapshot.summonAnimIdx, logLabel);
    }

    return count;
}

static void RenderBoxSet(ImDrawList* dl,
                         const EntitySnapshot& entity,
                         const BoxEntry* boxes,
                         int count,
                         ImU32 colour,
                         const ScreenTransform& t,
                         const char* boxLabel,
                         BoxRenderPass pass,
                         float outlinePadding) {
    if (!boxes) {
        return;
    }

    const float ex = (float)(entity.posX / 10);
    const float ey = (float)(entity.posY / 10);
    if (g_logPerFrame) {
        LOG_INFO("[HBV] %s.%s pos=(%d,%d) pos/10=(%.1f,%.1f) facing=%d scroll=(%d,%d)",
                 entity.label ? entity.label : "Entity",
                 boxLabel ? boxLabel : "boxes",
                 entity.posX,
                 entity.posY,
                 ex,
                 ey,
                 entity.facing,
                 t.scrollX,
                 t.scrollY);
    }

    for (int i = 0; i < count; i++) {
        const BoxEntry& box = boxes[i];
        if (box.halfW <= 0 || box.halfH <= 0) {
            continue;
        }

        const float cx = ex + (2.0f * box.xOff * entity.facing);
        const float cy = ey + (2.0f * box.yOff);
        const float hw = 2.0f * box.halfW;
        const float hh = 2.0f * box.halfH;
        const float left = cx - hw - t.scrollX;
        const float right = cx + hw - t.scrollX;
        const float top = cy - hh - t.scrollY;
        const float bot = cy + hh - t.scrollY;

        if (g_logPerFrame) {
            LOG_INFO("[HBV]   %s.%s draw[%d]: centre=(%.1f,%.1f) hw=%.1f hh=%.1f -> screen L=%.1f T=%.1f R=%.1f B=%.1f",
                     entity.label ? entity.label : "Entity",
                     boxLabel ? boxLabel : "boxes",
                     i,
                     cx,
                     cy,
                     hw,
                     hh,
                     left,
                     top,
                     right,
                     bot);
        }

        DrawBox(dl, left, top, right, bot, colour, g_fillAlpha, t, pass, outlinePadding);
    }
}

static void RenderHurtboxes(ImDrawList* dl,
                            const EntitySnapshot& entity,
                            const ScreenTransform& t,
                            BoxRenderPass pass) {
    if (!entity.animFrame) {
        return;
    }
    RenderBoxSet(dl, entity, entity.animFrame->hurtBoxes, HURTBOX_COUNT_PER_FRAME, COL_HURTBOX, t, "hurt", pass);
}

static void RenderEntityHitboxes(ImDrawList* dl,
                                 const EntitySnapshot& entity,
                                 const ScreenTransform& t,
                                 BoxRenderPass pass) {
    if (!entity.animFrame) {
        return;
    }
    RenderBoxSet(dl, entity, entity.animFrame->hitBoxes, HURTBOX_COUNT_PER_FRAME, COL_HITBOX, t, "hit", pass);
}

static void RenderThrowboxes(ImDrawList* dl,
                             const EntitySnapshot& entity,
                             const ScreenTransform& t,
                             BoxRenderPass pass) {
    if (!entity.animFrame) {
        return;
    }
    const float outlinePadding = (pass == BoxRenderPass::OutlineOnly) ? 1.5f : 0.0f;
    RenderBoxSet(dl, entity, entity.animFrame->throwBoxes, HURTBOX_COUNT_PER_FRAME, COL_THROWBOX, t, "ext", pass, outlinePadding);
}

static uint32_t GetLegacyInvincibleGateValue(const EntitySnapshot& entity) {
    return entity.maxHit.rawA;
}

static uint32_t GetDirectInvincibleFlagBits(const EntitySnapshot& entity) {
    return entity.maxHit.rawId & (MAX_HIT_FLAG_MELEE_INVULN | MAX_HIT_FLAG_PROJECTILE_INVULN);
}

static uint32_t GetMarkerInvincibleStateBits(const EntitySnapshot& entity) {
    const uint32_t invulnFlags = CLASH_ID_FLAG_INVINCIBLE | CLASH_ID_FLAG_SPECIAL_INVULN;
    if (entity.maxHit.marker1948 == 0) {
        return 0;
    }
    return (entity.clash.continuationId | entity.maxHit.rawId) & invulnFlags;
}

static bool HasInvincibleState(const EntitySnapshot& entity) {
    return GetLegacyInvincibleGateValue(entity) != 0
        || GetDirectInvincibleFlagBits(entity) != 0
        || GetMarkerInvincibleStateBits(entity) != 0;
}

static const char* BoolWord(bool value) {
    return value ? "yes" : "no";
}

static void RenderStateFlags(ImDrawList* dl,
                             const EntitySnapshot& entity,
                             const ScreenTransform& t) {
    ImVec2 pos = WorldToDisplay(entity.posX, entity.posY, t);
    float yOff = -20.0f * t.scaleY;
    char buf[96];

    auto drawLabel = [&](ImU32 colour, const char* txt) {
        ImVec2 sz = ImGui::CalcTextSize(txt);
        float x = pos.x - sz.x * 0.5f;
        float y = pos.y + yOff;
        dl->AddRectFilled(ImVec2(x - 2, y - 1), ImVec2(x + sz.x + 2, y + sz.y + 1), IM_COL32(0, 0, 0, 180));
        dl->AddText(ImVec2(x, y), colour, txt);
        yOff -= (sz.y + 4.0f);
    };

    bool drewPrimaryCancel = false;
    if (entity.cancelRoutes.command20Timer != 0) {
        drawLabel(COL_CANCEL, "BREAK");
        drewPrimaryCancel = true;
    }
    if (entity.cancelRoutes.command21Timer != 0) {
        drawLabel(COL_CANCEL, "LIGHT BREAK");
        drewPrimaryCancel = true;
    }
    if (!drewPrimaryCancel && entity.cancelRoutes.readyActionCount != 0) {
        char readyText[32];
        FormatReadyCancelActions(entity.cancelRoutes, readyText, sizeof(readyText));
        snprintf(buf, sizeof(buf), "CANCEL %s", readyText);
        drawLabel(COL_CANCEL, buf);
    }

    if (HasInvincibleState(entity)) {
        drawLabel(COL_INVINCIBLE, "INVINCIBLE");
    }

    if (entity.attackType & ATTACK_FLAG_PROJ_IMMUNE) {
        drawLabel(COL_IMMUNE, "PROJ IMMUNE");
    }

    if (entity.attackState == 1 && entity.hitActive != 0) {
        drawLabel(COL_HITBOX, "ATK");
    }

    // Proximity guard — cancellable, shown so players can see the window
    if (entity.actionId == 63 || entity.actionId == 66 || entity.actionId == 69) {
        drawLabel(COL_PROX_GUARD, "PROX GUARD");
    }
}

static void RenderHitDefs(ImDrawList* dl,
                          const HitDefSnapshot* hitDefs,
                          int hitDefCount,
                          const ScreenTransform& t,
                          BoxRenderPass pass) {
    int activeCount = 0;
    for (int i = 0; i < hitDefCount; i++) {
        const HitDefSnapshot& hitDef = hitDefs[i];
        if (!hitDef.animFrame) {
            continue;
        }

        activeCount++;

        if (g_logPerFrame) {
            LOG_INFO("[HBV] HitDef[%d] id=%u owner=%u animOwner=%u type=%u invuln=%u flag=%d pos=(%d,%d) face=%d dmg=%u animIdx=%u",
                     hitDef.index,
                     hitDef.id,
                     hitDef.owner,
                     hitDef.animOwner,
                     hitDef.type,
                     hitDef.invuln,
                     (int)(int8_t)hitDef.flag,
                     hitDef.worldX,
                     hitDef.worldY,
                     hitDef.facing,
                     hitDef.damage,
                     hitDef.summonAnimIdx);
        }

        const float hx = (float)(hitDef.worldX / 10);
        const float hy = (float)(hitDef.worldY / 10);
        bool anyDrawn = false;

        for (int j = 0; j < HURTBOX_COUNT_PER_FRAME; j++) {
            const BoxEntry& box = hitDef.animFrame->hitBoxes[j];
            if (box.halfW <= 0 || box.halfH <= 0) {
                continue;
            }

            const float cx = hx + (2.0f * box.xOff * hitDef.facing);
            const float cy = hy + (2.0f * box.yOff);
            const float hw = 2.0f * box.halfW;
            const float hh = 2.0f * box.halfH;
            DrawBox(dl,
                    cx - hw - t.scrollX,
                    cy - hh - t.scrollY,
                    cx + hw - t.scrollX,
                    cy + hh - t.scrollY,
                    COL_HITBOX,
                    g_fillAlpha,
                    t,
                    pass);
            anyDrawn = true;
        }

        if (g_showHurtboxes) {
            for (int j = 0; j < HURTBOX_COUNT_PER_FRAME; j++) {
                const BoxEntry& box = hitDef.animFrame->hurtBoxes[j];
                if (box.halfW <= 0 || box.halfH <= 0) {
                    continue;
                }

                const float cx = hx + (2.0f * box.xOff * hitDef.facing);
                const float cy = hy + (2.0f * box.yOff);
                const float hw = 2.0f * box.halfW;
                const float hh = 2.0f * box.halfH;
                DrawBox(dl,
                    cx - hw - t.scrollX,
                    cy - hh - t.scrollY,
                    cx + hw - t.scrollX,
                    cy + hh - t.scrollY,
                    COL_HURTBOX,
                    g_fillAlpha,
                    t,
                    pass);
                anyDrawn = true;
            }
        }

        if (!anyDrawn && pass == BoxRenderPass::FillOnly) {
            ImVec2 p = WorldToDisplay(hitDef.worldX, hitDef.worldY, t);
            dl->AddCircleFilled(p, 5.0f * t.scaleX, COL_HITDEF_PT);
        }
    }

    if (g_logPerFrame && activeCount > 0) {
        LOG_INFO("[HBV] Active HitDefs: %d", activeCount);
    }
}

static void RenderPushbox(ImDrawList* dl,
                          const EntitySnapshot& entity,
                          const ScreenTransform& t,
                          BoxRenderPass pass) {
    if (!entity.animFrame) {
        return;
    }

    const BoxEntry& box = entity.animFrame->collisionBox;
    if (box.halfW <= 0 || box.halfH <= 0) {
        return;
    }

    const float ex = (float)(entity.posX / 10);
    const float ey = (float)(entity.posY / 10);
    const float cx = ex + (2.0f * box.xOff * entity.facing);
    const float cy = ey + (2.0f * box.yOff);
    const float hw = 2.0f * box.halfW;
    const float hh = 2.0f * box.halfH;

    DrawBox(dl,
            cx - hw - t.scrollX,
            cy - hh - t.scrollY,
            cx + hw - t.scrollX,
            cy + hh - t.scrollY,
            COL_PUSHBOX,
            g_fillAlpha,
            t,
            pass);
}

static void RenderPosition(ImDrawList* dl,
                           const EntitySnapshot& entity,
                           ImU32 colour,
                           const ScreenTransform& t) {
    ImVec2 c = WorldToDisplay(entity.posX, entity.posY, t);
    float  r = 4.0f * t.scaleX;

    dl->AddCircleFilled(c, r, colour);
    dl->AddLine(ImVec2(c.x - r * 2, c.y), ImVec2(c.x + r * 2, c.y), colour, 1.5f);
    dl->AddLine(ImVec2(c.x, c.y - r * 2), ImVec2(c.x, c.y + r * 2), colour, 1.5f);
    float arrowLen = 12.0f * t.scaleX * entity.facing;
    dl->AddLine(c, ImVec2(c.x + arrowLen, c.y), colour, 2.5f);
}

static void RenderEntityInfoPanel(const EntitySnapshot& entity) {
    const AnimFrameSnapshot* animFrame = entity.animFrame;
    const int hurtCount = animFrame ? CountActiveBoxes(animFrame->hurtBoxes, HURTBOX_COUNT_PER_FRAME) : 0;
    const int hitCount = animFrame ? CountActiveBoxes(animFrame->hitBoxes, HURTBOX_COUNT_PER_FRAME) : 0;
    const int extCount = animFrame ? CountActiveBoxes(animFrame->throwBoxes, HURTBOX_COUNT_PER_FRAME) : 0;
    const bool breakWindowActive = entity.cancelRoutes.command20Timer != 0;
    const bool lightBreakWindowActive = entity.cancelRoutes.command21Timer != 0;
    const ImVec4 playerCol = entity.playerIndex == 0
        ? ImVec4(1.00f, 0.55f, 0.55f, 1.00f)
        : ImVec4(0.55f, 0.65f, 1.00f, 1.00f);
    const ImVec4 activeCol = ImVec4(1.00f, 0.96f, 0.47f, 1.00f);
    const ImVec4 infoCol = ImVec4(0.75f, 0.78f, 0.85f, 1.00f);
    char buf[256];
    char readyText[32];
    FormatReadyCancelActions(entity.cancelRoutes, readyText, sizeof(readyText));

    auto text = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        ImGui::TextUnformatted(buf);
    };
    auto textColored = [&](const ImVec4& col, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        ImGui::TextColored(col, "%s", buf);
    };

    ImGui::TextColored(playerCol, "%s", entity.label);
    ImGui::Separator();
    text("Pos %d,%d  Face %d  HP %d  Meter %u",
         entity.posX, entity.posY, entity.facing, entity.hp, entity.cancelRoutes.meter);
    text("Action %u  Phase %u  Frame %u  Anim %u",
         entity.actionId, entity.actionPhase, entity.actionFrame, entity.animIdx);
    text("Atk state %u  Type 0x%X  Hit active %u",
         entity.attackState, entity.attackType, entity.hitActive);

    ImGui::Spacing();
    ImGui::TextDisabled("Cancel windows");
    ImGui::Indent();
    if (breakWindowActive) {
        textColored(activeCol, "BREAK window: %u frames", entity.cancelRoutes.command20Timer);
    } else {
        text("BREAK window: 0");
    }
    text("  usable: %s  route: %u",
         BoolWord(entity.cancelRoutes.case20Ready != 0),
         entity.cancelRoutes.case20Action);
    text("  gate146: %u  lock434: %u",
         entity.cancelRoutes.super1Gate,
         entity.cancelRoutes.super1Lock);
    if (lightBreakWindowActive) {
        textColored(activeCol, "LIGHT BREAK window: %u frames", entity.cancelRoutes.command21Timer);
    } else {
        text("LIGHT BREAK window: 0");
    }
    text("  usable: %s  route: %u",
         BoolWord(entity.cancelRoutes.case21Ready != 0),
         entity.cancelRoutes.case21Action);
    text("  gate130/76: %u/%u  gate74/132: %u/%u",
         entity.cancelRoutes.super2GateA,
         entity.cancelRoutes.super2GateB,
         entity.cancelRoutes.super2GateC,
         entity.cancelRoutes.super2GateD);
    text("  state4 path: %s", BoolWord(entity.cancelRoutes.case21State4Path != 0));
    textColored(infoCol, "Legacy routes: %s", readyText);
    ImGui::Unindent();

    ImGui::Spacing();
    ImGui::TextDisabled("Defence");
    ImGui::Indent();
    text("Invincible: %s", BoolWord(HasInvincibleState(entity)));
    text("  gate: 0x%X  flags: 0x%X  marker: 0x%X",
         GetLegacyInvincibleGateValue(entity),
         GetDirectInvincibleFlagBits(entity),
         GetMarkerInvincibleStateBits(entity));
    text("  marker1948: %u  marker1949: %u",
         entity.maxHit.marker1948,
         entity.maxHit.marker1949);
    if (entity.attackType & ATTACK_FLAG_PROJ_IMMUNE) {
        textColored(infoCol, "Projectile immunity is active.");
    }
    if (entity.attackType & ATTACK_FLAG_CONTACT_OVERRIDE) {
        ImGui::TextWrapped("Contact override is active: hit / grab checks can bypass the normal box-size and overlap tests.");
    }
    ImGui::Unindent();

    ImGui::Spacing();
    ImGui::TextDisabled("Boxes");
    ImGui::Indent();
    text("Counts  coll:%u  hurt:%d  hit:%d  ext:%d",
         (animFrame && animFrame->collisionBox.halfW > 0 && animFrame->collisionBox.halfH > 0) ? 1u : 0u,
         hurtCount,
         hitCount,
         extCount);
    if (!animFrame) {
        textColored(infoCol, "No animation frame snapshot is loaded.");
    } else {
        char treeLabel[32];
        snprintf(treeLabel, sizeof(treeLabel), "Frame boxes##%s", entity.label);
        if (ImGui::TreeNode(treeLabel)) {
            if (animFrame->collisionBox.halfW > 0 && animFrame->collisionBox.halfH > 0) {
                text("coll off(%d,%d) half(%d,%d)",
                     animFrame->collisionBox.xOff,
                     animFrame->collisionBox.yOff,
                     animFrame->collisionBox.halfW,
                     animFrame->collisionBox.halfH);
            }
            for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; i++) {
                const BoxEntry& hurtBox = animFrame->hurtBoxes[i];
                const BoxEntry& hitBox = animFrame->hitBoxes[i];
                const BoxEntry& throwBox = animFrame->throwBoxes[i];
                if (hurtBox.halfW > 0 && hurtBox.halfH > 0) {
                    text("hurt%d off(%d,%d) half(%d,%d)",
                         i, hurtBox.xOff, hurtBox.yOff, hurtBox.halfW, hurtBox.halfH);
                }
                if (hitBox.halfW > 0 && hitBox.halfH > 0) {
                    text("hit%d off(%d,%d) half(%d,%d)",
                         i, hitBox.xOff, hitBox.yOff, hitBox.halfW, hitBox.halfH);
                }
                if (throwBox.halfW > 0 && throwBox.halfH > 0) {
                    text("ext%d off(%d,%d) half(%d,%d)",
                         i, throwBox.xOff, throwBox.yOff, throwBox.halfW, throwBox.halfH);
                }
            }
            ImGui::TreePop();
        }
    }
    ImGui::Unindent();
}

static void RenderInfoPanelWindow(const HitboxViewerFrameContext& ctx,
                                  const EntitySnapshot& p1,
                                  const EntitySnapshot& p2) {
    const ImVec2 defaultPos(ModUI_Scale(10.0f), ModUI_Scale(70.0f));
    const ImVec2 defaultSize(ModUI_Scale(620.0f), ModUI_Scale(360.0f));
    char scrollHex[128];

    ImGui::SetNextWindowPos(defaultPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.84f);

    bool windowOpen = g_showInfo;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Hitbox Viewer Info", &windowOpen, flags)) {
        ImGui::End();
        g_showInfo = windowOpen;
        return;
    }
    g_showInfo = windowOpen;

    ImGui::Text("Scroll %d,%d  Display %.0fx%.0f  Scale %.2f,%.2f",
                ctx.transform.scrollX, ctx.transform.scrollY,
                ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y,
                ctx.transform.scaleX, ctx.transform.scaleY);
    ImGui::Separator();
    ImGui::BeginChild("HBVInfoScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    if (g_showMatchState && ImGui::CollapsingHeader("Match / camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Mode %u  Sub %u  Type %u  Sim %u  Disp %u",
                    ctx.gameMode, ctx.subState, ctx.gameType, ctx.simFrame, ctx.dispFrame);
        ImGui::Text("Phase timer %u  Round timer %u", ctx.phaseTimer, ctx.roundTimer);
        ImGui::Text("P1 base 0x%08X  P2 base 0x%08X  Stride %d",
                    (uint32_t)p1.base, (uint32_t)p2.base, ENTITY_SIZE);
        ImGui::Text("Anim data +0x%X  stride %d  coll@%d hit@%d hurt@%d ext@%d  %dx%dB",
                    ENTITY_OFF_ANIM_DATA, ANIM_DATA_STRIDE,
                    ANIM_COLLISION_OFFSET, ANIM_HITBOX_OFFSET, ANIM_HURTBOX_OFFSET, ANIM_EXT_HURTBOX_OFFSET,
                    HURTBOX_COUNT_PER_FRAME, HURTBOX_ENTRY_SIZE);
        int pos = 0;
        for (int i = 0; i < 8 && pos + 6 < (int)sizeof(scrollHex); i++) {
            pos += snprintf(scrollHex + pos, sizeof(scrollHex) - pos, "%04X ",
                            (uint16_t)ctx.scrollVicinity[i]);
        }
        ImGui::Text("Scroll vicinity %s", scrollHex);
        ImGui::Separator();
    }

    if (ImGui::BeginTable("HBVPlayerColumns",
                          2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Resizable)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        RenderEntityInfoPanel(p1);
        ImGui::TableSetColumnIndex(1);
        RenderEntityInfoPanel(p2);
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::End();
}

static void RenderInfoOverlay(ImDrawList* dl,
                              const HitboxViewerFrameContext& ctx,
                              const EntitySnapshot& p1,
                              const EntitySnapshot& p2) {
    (void)dl;
    RenderInfoPanelWindow(ctx, p1, p2);
    return;

    char buf[256];
    float y = 10.0f;
    const float x = 10.0f;
    const float lineH = 14.0f;
    const ImU32 bg = IM_COL32(0, 0, 0, 180);
    const ImU32 white = IM_COL32(200, 200, 200, 255);
    const ImU32 dim   = IM_COL32(150, 150, 150, 255);

    int totalLines = 3;
    if (g_showMatchState) {
        totalLines += 6;
    }

    const EntitySnapshot* players[2] = { &p1, &p2 };
    for (int pi = 0; pi < 2; pi++) {
        totalLines += 6;
        const AnimFrameSnapshot* animFrame = players[pi]->animFrame;
        if (players[pi]->attackType & ATTACK_FLAG_CONTACT_OVERRIDE) {
            totalLines++;
        }
        if (!animFrame) {
            continue;
        }
        if (animFrame->collisionBox.halfW > 0 && animFrame->collisionBox.halfH > 0) {
            totalLines++;
        }
        totalLines += CountActiveBoxes(animFrame->hurtBoxes, HURTBOX_COUNT_PER_FRAME);
        totalLines += CountActiveBoxes(animFrame->hitBoxes, HURTBOX_COUNT_PER_FRAME);
        totalLines += CountActiveBoxes(animFrame->throwBoxes, HURTBOX_COUNT_PER_FRAME);
    }

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 860, y + lineH * totalLines + 6), bg);

    auto line = [&](ImU32 col, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        dl->AddText(ImVec2(x + 4, y), col, buf);
        y += lineH;
    };

    line(white, "Scroll: %d,%d  Display: %.0fx%.0f  Scale: %.2f,%.2f",
         ctx.transform.scrollX, ctx.transform.scrollY,
         ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y,
         ctx.transform.scaleX, ctx.transform.scaleY);

    if (g_showMatchState) {
        line(dim, "Mode:%u Sub:%u Type:%u SimF:%u DispF:%u",
             ctx.gameMode, ctx.subState, ctx.gameType, ctx.simFrame, ctx.dispFrame);
        line(dim, "PhaseTimer:%u RoundTimer:%u MatchBase:0x%08X",
             ctx.phaseTimer, ctx.roundTimer, (uint32_t)ADDR_MATCH_BASE);
        line(dim, "ScrollAddr: X=0x%08X Y=0x%08X",
             (uint32_t)ADDR_SCROLL_X, (uint32_t)ADDR_SCROLL_Y);

        char scrollHex[128];
        int pos = 0;
        for (int i = 0; i < 8 && pos < 120; i++) {
            pos += snprintf(scrollHex + pos, sizeof(scrollHex) - pos, "%04X ",
                            (uint16_t)ctx.scrollVicinity[i]);
        }
        line(dim, "Scroll vicinity: %s", scrollHex);

        line(dim, "P1Base:0x%08X P2Base:0x%08X Stride:%d",
             (uint32_t)p1.base, (uint32_t)p2.base,
             ENTITY_SIZE);
        line(dim, "AnimData +0x%X  Stride:%d  Coll@%d Hit@%d Hurt@%d Ext@%d  %d×%dB",
             ENTITY_OFF_ANIM_DATA, ANIM_DATA_STRIDE,
             ANIM_COLLISION_OFFSET, ANIM_HITBOX_OFFSET, ANIM_HURTBOX_OFFSET, ANIM_EXT_HURTBOX_OFFSET,
             HURTBOX_COUNT_PER_FRAME, HURTBOX_ENTRY_SIZE);
    }

    for (int pi = 0; pi < 2; pi++) {
        const EntitySnapshot& entity = *players[pi];
        const AnimFrameSnapshot* animFrame = entity.animFrame;
        ImU32 col = (pi == 0) ? COL_P1_CROSS : COL_P2_CROSS;
        char readyText[32];
        FormatReadyCancelActions(entity.cancelRoutes, readyText, sizeof(readyText));
        line(col, "P%d Pos:%d,%d Face:%d HP:%d Action:%u Anim:%u Atk:%u Flags:0x%X",
             pi + 1, entity.posX, entity.posY, entity.facing, entity.hp,
             entity.actionId, entity.animIdx, entity.attackState, entity.attackType);
        line(col, "  case20 timer688:%u ready:%u act:%u gate146:%u lock434:%u meter:%u",
             entity.cancelRoutes.command20Timer,
             entity.cancelRoutes.case20Ready,
             entity.cancelRoutes.case20Action,
             entity.cancelRoutes.super1Gate,
             entity.cancelRoutes.super1Lock,
             entity.cancelRoutes.meter);
        line(col, "  case21 timer689:%u ready:%u act:%u gate130/76:%u/%u gate74/132:%u/%u state4:%u",
             entity.cancelRoutes.command21Timer,
             entity.cancelRoutes.case21Ready,
             entity.cancelRoutes.case21Action,
             entity.cancelRoutes.super2GateA,
             entity.cancelRoutes.super2GateB,
             entity.cancelRoutes.super2GateC,
             entity.cancelRoutes.super2GateD,
             entity.cancelRoutes.case21State4Path);
        line(col, "  legacy c:%u f:%u gs:%u 107:%u g:%u/%u/%u air:%u mv:%u r:%s",
             entity.cancelRoutes.charId,
             entity.cancelRoutes.familyType,
             entity.cancelRoutes.gateState,
             entity.cancelRoutes.action107Threshold,
             entity.cancelRoutes.gate16,
             entity.cancelRoutes.gate22,
             entity.cancelRoutes.gate26,
             entity.cancelRoutes.airborne,
             entity.cancelRoutes.airMoveInput,
             readyText);
        line(col, "  clash r:%u raw:%d/%d/%d/%d id:0x%X",
             entity.clash.rank,
             entity.clash.rawA,
             entity.clash.rawB,
             entity.clash.rawC,
             entity.clash.rawD,
             entity.clash.continuationId);
        line(col, "  max %u/%u/%u aux:%d flags:0x%X act:%u",
             entity.maxHit.rawA,
             entity.maxHit.rawB,
             entity.maxHit.rawC,
             entity.maxHit.rawD,
             entity.maxHit.rawId,
             entity.maxHit.active);
        line(col, "  inv gate:0x%X flags:0x%X marker:0x%X on:%u m1948:%u m1949:%u",
             GetLegacyInvincibleGateValue(entity),
             GetDirectInvincibleFlagBits(entity),
             GetMarkerInvincibleStateBits(entity),
             HasInvincibleState(entity) ? 1u : 0u,
             entity.maxHit.marker1948,
             entity.maxHit.marker1949);
        const int hurtCount = animFrame ? CountActiveBoxes(animFrame->hurtBoxes, HURTBOX_COUNT_PER_FRAME) : 0;
        const int hitCount = animFrame ? CountActiveBoxes(animFrame->hitBoxes, HURTBOX_COUNT_PER_FRAME) : 0;
        const int extCount = animFrame ? CountActiveBoxes(animFrame->throwBoxes, HURTBOX_COUNT_PER_FRAME) : 0;
        line(col, "  box counts coll:%u hurt:%d hit:%d ext:%d",
             (animFrame && animFrame->collisionBox.halfW > 0 && animFrame->collisionBox.halfH > 0) ? 1u : 0u,
             hurtCount,
             hitCount,
             extCount);
        if (entity.attackType & ATTACK_FLAG_CONTACT_OVERRIDE) {
            line(col, "  contact-override: hit/grab checks bypass box-size and overlap tests");
        }

        if (!animFrame) {
            continue;
        }

        if (animFrame->collisionBox.halfW > 0 && animFrame->collisionBox.halfH > 0) {
            line(col, "  coll off(%d,%d) half(%d,%d)",
                 animFrame->collisionBox.xOff,
                 animFrame->collisionBox.yOff,
                 animFrame->collisionBox.halfW,
                 animFrame->collisionBox.halfH);
        }

        for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; i++) {
            const BoxEntry& hurtBox = animFrame->hurtBoxes[i];
            const BoxEntry& hitBox = animFrame->hitBoxes[i];
            const BoxEntry& throwBox = animFrame->throwBoxes[i];
            if (hurtBox.halfW > 0 && hurtBox.halfH > 0) {
                line(col, "  hurt%d off(%d,%d) half(%d,%d)",
                     i, hurtBox.xOff, hurtBox.yOff, hurtBox.halfW, hurtBox.halfH);
            }
            if (hitBox.halfW > 0 && hitBox.halfH > 0) {
                line(col, "  hit%d off(%d,%d) half(%d,%d)",
                     i, hitBox.xOff, hitBox.yOff, hitBox.halfW, hitBox.halfH);
            }
            if (throwBox.halfW > 0 && throwBox.halfH > 0) {
                line(col, "  ext%d off(%d,%d) half(%d,%d)",
                     i, throwBox.xOff, throwBox.yOff, throwBox.halfW, throwBox.halfH);
            }
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

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    HitboxViewerFrameContext frameCtx = BuildFrameContext();
    AnimFrameCache animCache{};
    const uintptr_t p1EntityBase = GetEntityBase(0);
    const uintptr_t p2EntityBase = GetEntityBase(1);
    EntitySnapshot p1 = BuildEntitySnapshot(&animCache, p1EntityBase, 0, "P1");
    EntitySnapshot p2 = BuildEntitySnapshot(&animCache, p2EntityBase, 1, "P2");

    auto labelOr = [](uint32_t id) -> const char* {
        const char* l = ActionLabel(id);
        return l ? l : "?";
    };
    if (p1.actionId != s_lastP1Action) {
        if (g_logPerFrame) {
            LOG_INFO("[HBV] P1 action %u(%s) -> %u(%s)  anim %u -> %u",
                     s_lastP1Action, labelOr(s_lastP1Action),
                     p1.actionId, labelOr(p1.actionId),
                     s_lastP1Anim, p1.animIdx);
        }
        s_lastP1Action = p1.actionId;
    }
    if (p2.actionId != s_lastP2Action) {
        if (g_logPerFrame) {
            LOG_INFO("[HBV] P2 action %u(%s) -> %u(%s)  anim %u -> %u",
                     s_lastP2Action, labelOr(s_lastP2Action),
                     p2.actionId, labelOr(p2.actionId),
                     s_lastP2Anim, p2.animIdx);
        }
        s_lastP2Action = p2.actionId;
    }
    s_lastP1Anim = p1.animIdx;
    s_lastP2Anim = p2.animIdx;

    HitDefSnapshot hitDefs[SUMMON_MAX_SLOTS] = {};
    int hitDefCount = 0;
    if (g_showHitboxes) {
        hitDefCount = BuildHitDefSnapshots(&animCache, hitDefs, SUMMON_MAX_SLOTS, p1EntityBase, p2EntityBase);
    }

    auto renderBoxes = [&](BoxRenderPass pass) {
        if (g_showPushboxes) {
            RenderPushbox(dl, p1, frameCtx.transform, pass);
            RenderPushbox(dl, p2, frameCtx.transform, pass);
        }
        if (g_showHurtboxes) {
            RenderHurtboxes(dl, p1, frameCtx.transform, pass);
            RenderHurtboxes(dl, p2, frameCtx.transform, pass);
        }
        if (g_showThrowboxes) {
            if (pass == BoxRenderPass::FillOnly) {
                RenderThrowboxes(dl, p1, frameCtx.transform, pass);
                RenderThrowboxes(dl, p2, frameCtx.transform, pass);
            }
        }
        if (g_showHitboxes) {
            RenderEntityHitboxes(dl, p1, frameCtx.transform, pass);
            RenderEntityHitboxes(dl, p2, frameCtx.transform, pass);
            RenderHitDefs(dl, hitDefs, hitDefCount, frameCtx.transform, pass);
        }
        if (g_showThrowboxes) {
            if (pass == BoxRenderPass::OutlineOnly) {
                RenderThrowboxes(dl, p1, frameCtx.transform, pass);
                RenderThrowboxes(dl, p2, frameCtx.transform, pass);
            }
        }
    };

    renderBoxes(BoxRenderPass::FillOnly);
    renderBoxes(BoxRenderPass::OutlineOnly);

    if (g_showPositions) {
        RenderPosition(dl, p1, COL_P1_CROSS, frameCtx.transform);
        RenderPosition(dl, p2, COL_P2_CROSS, frameCtx.transform);
    }
    if (g_showStateFlags) {
        RenderStateFlags(dl, p1, frameCtx.transform);
        RenderStateFlags(dl, p2, frameCtx.transform);
    }
    if (g_showInfo) {
        RenderInfoOverlay(dl, frameCtx, p1, p2);
    }
}

void HitboxViewer_SetEnabled(bool enabled) { g_enabled = enabled; }
bool HitboxViewer_IsEnabled() { return g_enabled; }
void HitboxViewer_ToggleEnabled() { g_enabled = !g_enabled; }

void HitboxViewer_RenderControls() {
    ImGui::Checkbox("Enable Hitbox Viewer", &g_enabled);
    if (!g_enabled) return;

    ImGui::Separator();
    ImGui::Checkbox("Hurtboxes (green, @40)", &g_showHurtboxes);
    ImGui::Checkbox("Hitboxes / attacks (red, @8)", &g_showHitboxes);
    ImGui::Checkbox("Ext. hurtboxes (cyan, @72)", &g_showThrowboxes);
    ImGui::Checkbox("Collision / pushbox (yellow, @0)", &g_showPushboxes);
    ImGui::Checkbox("State flags (ATK / INVINCIBLE / PROJ IMMUNE / BREAK)", &g_showStateFlags);
    ImGui::Checkbox("Position markers", &g_showPositions);
    ImGui::Checkbox("Info panel (scrollable debug)", &g_showInfo);
    ImGui::Checkbox("Match state debug", &g_showMatchState);
    ImGui::SliderFloat("Fill alpha", &g_fillAlpha, 0.0f, 1.0f, "%.2f");
    ImGui::Separator();
    ImGui::Checkbox("Per-frame logging (VERBOSE)", &g_logPerFrame);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Logs every box read every frame.\nWill flood the log. Use briefly.");

    if (ImGui::Button("Log Once Now")) {
        s_lastP1Anim = 0xFFFFFFFF;
        s_lastP2Anim = 0xFFFFFFFF;
        LOG_INFO("[HBV] --- Manual one-shot log triggered ---");
    }
}
