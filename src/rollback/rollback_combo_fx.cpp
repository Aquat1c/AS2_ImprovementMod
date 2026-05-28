#include "rollback/rollback_combo_fx.h"

#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace Rollback {
namespace {

static bool s_initialized = false;
static bool s_sessionActive = false;
static int s_rollbackBudget = 8;
static int32_t s_currentRbFrame = -1;
static int32_t s_currentGameAbsFrame = -1;
static bool s_currentRollingBack = false;
static int32_t s_lastSnapshotFrame = -999999;
static uint32_t s_updateStatsCount = 0;
static uint32_t s_updateStat1243Count = 0;
static uint32_t s_timerChangeCount = 0;
static uint32_t s_slotAddCount = 0;
static uint32_t s_setParams1Count = 0;
static uint32_t s_setParams2Count = 0;

struct ComboSnapshot {
    uint8_t display_combo;
    uint8_t scale1;
    uint8_t scale2;
    uint8_t scale3;
    uint8_t scale4;
    uint8_t reset_flag;
    uint32_t reaction_type;
    uint32_t reaction_class;
    uint8_t shown_flag;
    uint8_t anim_timer;
    uint8_t life_timer;
    uint8_t keep_flag;
    uint32_t attached_crc;
    uint32_t render_crc;
};

static ComboSnapshot s_lastSaveP1 = {};
static ComboSnapshot s_lastSaveP2 = {};
static bool s_hasLastSaveSnapshot = false;
static int32_t s_lastSaveSnapshotLogFrame = -999999;
static uint32_t s_saveSnapshotLogged = 0;
static uint32_t s_saveSnapshotSkipped = 0;

static uint32_t SafeCrc(uintptr_t address, size_t size) {
    uint8_t buf[96] = {};
    if (size > sizeof(buf)) {
        size = sizeof(buf);
    }
    if (!CopyMemorySafe(buf, reinterpret_cast<const void*>(address), size)) {
        return 0xBAD00000u | (uint32_t)(address & 0xFFFFu);
    }
    return CalcCRC32(buf, size);
}

static ComboSnapshot CaptureEntity(uintptr_t base) {
    ComboSnapshot s{};
    s.display_combo = ReadMemory<uint8_t>(base + ENTITY_OFF_DISPLAY_COMBO_COUNT);
    s.scale1 = ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE1);
    s.scale2 = ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE2);
    s.scale3 = ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE3);
    s.scale4 = ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE4);
    s.reset_flag = ReadMemory<uint8_t>(base + ENTITY_OFF_HIT_REACTION_RESET_FLAG);
    s.reaction_type = ReadMemory<uint32_t>(base + ENTITY_OFF_HIT_REACTION_TYPE);
    s.reaction_class = ReadMemory<uint32_t>(base + ENTITY_OFF_HIT_REACTION_CLASS);
    s.shown_flag = ReadMemory<uint8_t>(base + ENTITY_OFF_HIT_REACTION_SHOWN_FLAG);
    s.anim_timer = ReadMemory<uint8_t>(base + ENTITY_OFF_HIT_REACTION_ANIM_TIMER);
    s.life_timer = ReadMemory<uint8_t>(base + ENTITY_OFF_HIT_REACTION_LIFE_TIMER);
    s.keep_flag = ReadMemory<uint8_t>(base + ENTITY_OFF_HIT_REACTION_KEEP_FLAG);
    s.attached_crc = SafeCrc(base + ENTITY_OFF_ATTACHED_FX_SLOTS, ENTITY_ATTACHED_FX_SLOTS_SIZE);
    s.render_crc = SafeCrc(base + ENTITY_OFF_RENDER_MAIN_SPRITE, ENTITY_RENDER_OVERLAY_FIELDS_SIZE);
    return s;
}

static bool PresentationStateChanged(const ComboSnapshot& a, const ComboSnapshot& b) {
    return a.display_combo != b.display_combo ||
           a.scale1 != b.scale1 ||
           a.scale2 != b.scale2 ||
           a.scale3 != b.scale3 ||
           a.scale4 != b.scale4 ||
           a.reset_flag != b.reset_flag ||
           a.reaction_type != b.reaction_type ||
           a.reaction_class != b.reaction_class ||
           a.shown_flag != b.shown_flag ||
           a.anim_timer != b.anim_timer ||
           a.life_timer != b.life_timer ||
           a.keep_flag != b.keep_flag ||
           a.attached_crc != b.attached_crc;
}

static bool ImportantPresentationStateChanged(const ComboSnapshot& a, const ComboSnapshot& b) {
    return a.display_combo != b.display_combo ||
           a.scale1 != b.scale1 ||
           a.scale2 != b.scale2 ||
           a.scale3 != b.scale3 ||
           a.scale4 != b.scale4 ||
           a.reset_flag != b.reset_flag ||
           a.reaction_type != b.reaction_type ||
           a.reaction_class != b.reaction_class ||
           a.shown_flag != b.shown_flag ||
           a.keep_flag != b.keep_flag ||
           a.attached_crc != b.attached_crc;
}

static int EntitySide(uintptr_t entity) {
    if (entity == ADDR_P1_ENTITY_BASE) return 0;
    if (entity == ADDR_P2_ENTITY_BASE) return 1;
    return 2;
}

static void LogSnapshotValues(const char* phase,
                              int32_t rb_frame,
                              int32_t game_abs_frame,
                              const ComboSnapshot& p1,
                              const ComboSnapshot& p2,
                              const char* detail) {
    NetplayLog_Write("COMBOFX", rb_frame,
        "%s snapshot rb=%d game=%d rolling=%d "
        "%s "
        "P1{hud=%u sc=%u/%u/%u/%u react=%u/%u/%u/%u/%u/%u/%u fx=0x%08X render=0x%08X} "
        "P2{hud=%u sc=%u/%u/%u/%u react=%u/%u/%u/%u/%u/%u/%u fx=0x%08X render=0x%08X}",
        phase ? phase : "UNKNOWN",
        rb_frame,
        game_abs_frame,
        s_currentRollingBack ? 1 : 0,
        detail ? detail : "",
        p1.display_combo, p1.scale1, p1.scale2, p1.scale3, p1.scale4,
        p1.reset_flag, p1.reaction_type, p1.reaction_class, p1.shown_flag,
        p1.anim_timer, p1.life_timer, p1.keep_flag, p1.attached_crc, p1.render_crc,
        p2.display_combo, p2.scale1, p2.scale2, p2.scale3, p2.scale4,
        p2.reset_flag, p2.reaction_type, p2.reaction_class, p2.shown_flag,
        p2.anim_timer, p2.life_timer, p2.keep_flag, p2.attached_crc, p2.render_crc);
}

static void LogSnapshot(const char* phase, int32_t rb_frame, int32_t game_abs_frame) {
    const ComboSnapshot p1 = CaptureEntity(ADDR_P1_ENTITY_BASE);
    const ComboSnapshot p2 = CaptureEntity(ADDR_P2_ENTITY_BASE);
    LogSnapshotValues(phase, rb_frame, game_abs_frame, p1, p2, "");
}

static void LogEntityTransition(const char* tag,
                                uintptr_t entity,
                                const ComboSnapshot& before,
                                const ComboSnapshot& after,
                                const char* detail) {
    if (!s_sessionActive || !PresentationStateChanged(before, after)) {
        return;
    }

    const int side = EntitySide(entity);
    NetplayLog_Write("COMBOFX", s_currentRbFrame,
        "%s rb=%d game=%d rolling=%d side=%d entity=0x%08X %s "
        "before{hud=%u sc=%u/%u/%u/%u react=%u/%u/%u/%u/%u/%u/%u fx=0x%08X render=0x%08X} "
        "after{hud=%u sc=%u/%u/%u/%u react=%u/%u/%u/%u/%u/%u/%u fx=0x%08X render=0x%08X}",
        tag ? tag : "CHANGE",
        s_currentRbFrame,
        s_currentGameAbsFrame,
        s_currentRollingBack ? 1 : 0,
        side,
        (unsigned)entity,
        detail ? detail : "",
        before.display_combo, before.scale1, before.scale2, before.scale3, before.scale4,
        before.reset_flag, before.reaction_type, before.reaction_class, before.shown_flag,
        before.anim_timer, before.life_timer, before.keep_flag, before.attached_crc, before.render_crc,
        after.display_combo, after.scale1, after.scale2, after.scale3, after.scale4,
        after.reset_flag, after.reaction_type, after.reaction_class, after.shown_flag,
        after.anim_timer, after.life_timer, after.keep_flag, after.attached_crc, after.render_crc);
}

static void UpdateContextFromSession() {
    if (RollbackSession_IsActive()) {
        s_currentRbFrame = RollbackSession_GetCurrentFrame();
        s_currentGameAbsFrame = RollbackSession_GetCurrentGameAbsFrame();
        s_currentRollingBack = RollbackSession_IsRollingBack();
    }
}

} // namespace

MatchUpdateComboTimers_t g_origMatchUpdateComboTimers = nullptr;
EntityUpdateComboStats_t g_origEntityUpdateComboStats = nullptr;
EntityUpdateComboStat1243_t g_origEntityUpdateComboStat1243 = nullptr;
EffectSlotsAdd_t g_origEffectSlotsAdd = nullptr;
EffectSetParams1_t g_origEffectSetParams1 = nullptr;
EffectSetParams2_t g_origEffectSetParams2 = nullptr;

void RollbackComboFx_Init() {
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    NetplayLog_Write("COMBOFX", -1,
        "INIT combo/hit-reaction state monitor active_fields=entity-local state_preserving=1");
}

void RollbackComboFx_Shutdown() {
    RollbackComboFx_OnSessionEnd("RollbackComboFx_Shutdown");
    s_initialized = false;
}

void RollbackComboFx_OnSessionBegin(int rollback_budget) {
    s_sessionActive = true;
    s_rollbackBudget = rollback_budget > 0 ? rollback_budget : 8;
    s_currentRbFrame = 0;
    s_currentGameAbsFrame = 0;
    s_currentRollingBack = false;
    s_lastSnapshotFrame = -999999;
    s_updateStatsCount = 0;
    s_updateStat1243Count = 0;
    s_timerChangeCount = 0;
    s_slotAddCount = 0;
    s_setParams1Count = 0;
    s_setParams2Count = 0;
    s_lastSaveP1 = ComboSnapshot{};
    s_lastSaveP2 = ComboSnapshot{};
    s_hasLastSaveSnapshot = false;
    s_lastSaveSnapshotLogFrame = -999999;
    s_saveSnapshotLogged = 0;
    s_saveSnapshotSkipped = 0;
    NetplayLog_Write("COMBOFX", 0,
        "SESSION BEGIN budget=%d hooks=combo_timers,combo_stats,combo_1243,effect_slots,set_params1,set_params2",
        s_rollbackBudget);
    LogSnapshot("BEGIN", 0, 0);
}

void RollbackComboFx_OnSessionEnd(const char* reason) {
    if (!s_sessionActive) {
        return;
    }
    NetplayLog_Write("COMBOFX", s_currentRbFrame,
        "SESSION END reason=\"%s\" stats=%u stat1243=%u timers=%u slots=%u params1=%u params2=%u save_logged=%u save_skipped=%u",
        reason ? reason : "unknown",
        s_updateStatsCount,
        s_updateStat1243Count,
        s_timerChangeCount,
        s_slotAddCount,
        s_setParams1Count,
        s_setParams2Count,
        s_saveSnapshotLogged,
        s_saveSnapshotSkipped);
    s_sessionActive = false;
}

void RollbackComboFx_OnGekkoSave(int32_t rb_frame, int32_t game_abs_frame) {
    if (!s_sessionActive) return;

    const ComboSnapshot p1 = CaptureEntity(ADDR_P1_ENTITY_BASE);
    const ComboSnapshot p2 = CaptureEntity(ADDR_P2_ENTITY_BASE);
    const bool changed = !s_hasLastSaveSnapshot ||
                         ImportantPresentationStateChanged(s_lastSaveP1, p1) ||
                         ImportantPresentationStateChanged(s_lastSaveP2, p2);
    const bool periodic = rb_frame - s_lastSaveSnapshotLogFrame >= 120;

    if (changed || periodic || s_currentRollingBack) {
        char detail[96];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
            "reason=%s%s%s skipped=%u",
            changed ? "changed" : "",
            periodic ? "+periodic" : "",
            s_currentRollingBack ? "+rolling" : "",
            s_saveSnapshotSkipped);
        LogSnapshotValues("SAVE", rb_frame, game_abs_frame, p1, p2, detail);
        s_lastSaveSnapshotLogFrame = rb_frame;
        ++s_saveSnapshotLogged;
        s_saveSnapshotSkipped = 0;
    } else {
        ++s_saveSnapshotSkipped;
    }

    s_lastSaveP1 = p1;
    s_lastSaveP2 = p2;
    s_hasLastSaveSnapshot = true;
}

void RollbackComboFx_OnGekkoLoad(int32_t rb_frame, int32_t game_abs_frame) {
    if (!s_sessionActive) return;
    LogSnapshot("LOAD", rb_frame, game_abs_frame);
}

void RollbackComboFx_OnAdvanceBegin(int32_t rb_frame, int32_t game_abs_frame, bool rolling_back) {
    if (!s_sessionActive) return;
    s_currentRbFrame = rb_frame;
    s_currentGameAbsFrame = game_abs_frame;
    s_currentRollingBack = rolling_back;
    if (rolling_back || rb_frame - s_lastSnapshotFrame >= 120) {
        s_lastSnapshotFrame = rb_frame;
        LogSnapshot(rolling_back ? "ADVANCE_RESIM" : "ADVANCE", rb_frame, game_abs_frame);
    }
}

void RollbackComboFx_OnGekkoBatchEnd(int32_t rb_frame, int32_t game_abs_frame) {
    if (!s_sessionActive) return;
    s_currentRbFrame = rb_frame;
    s_currentGameAbsFrame = game_abs_frame;
    NetplayLog_Verbose("COMBOFX", rb_frame,
        "BATCH_END rb=%d game=%d rolling=%d stats=%u stat1243=%u timers=%u slots=%u params1=%u params2=%u",
        rb_frame,
        game_abs_frame,
        s_currentRollingBack ? 1 : 0,
        s_updateStatsCount,
        s_updateStat1243Count,
        s_timerChangeCount,
        s_slotAddCount,
        s_setParams1Count,
        s_setParams2Count);
}

int __cdecl Hook_Match_UpdateComboTimers(int match) {
    if (!g_origMatchUpdateComboTimers) {
        return 0;
    }

    if (!RollbackSession_IsActive()) {
        return g_origMatchUpdateComboTimers(match);
    }

    UpdateContextFromSession();
    const ComboSnapshot p1Before = CaptureEntity(ADDR_P1_ENTITY_BASE);
    const ComboSnapshot p2Before = CaptureEntity(ADDR_P2_ENTITY_BASE);
    const int result = g_origMatchUpdateComboTimers(match);
    const ComboSnapshot p1After = CaptureEntity(ADDR_P1_ENTITY_BASE);
    const ComboSnapshot p2After = CaptureEntity(ADDR_P2_ENTITY_BASE);

    if (PresentationStateChanged(p1Before, p1After) ||
        PresentationStateChanged(p2Before, p2After)) {
        ++s_timerChangeCount;
        const bool important = s_currentRollingBack ||
                               ImportantPresentationStateChanged(p1Before, p1After) ||
                               ImportantPresentationStateChanged(p2Before, p2After);
        if (important) {
            NetplayLog_Write("COMBOFX", s_currentRbFrame,
                "TIMER_DECAY rb=%d game=%d rolling=%d important=1 match=0x%08X result=0x%08X",
                s_currentRbFrame,
                s_currentGameAbsFrame,
                s_currentRollingBack ? 1 : 0,
                (unsigned)match,
                (unsigned)result);
            LogEntityTransition("TIMER_DECAY_P1", ADDR_P1_ENTITY_BASE, p1Before, p1After, "");
            LogEntityTransition("TIMER_DECAY_P2", ADDR_P2_ENTITY_BASE, p2Before, p2After, "");
        } else {
            NetplayLog_Verbose("COMBOFX", s_currentRbFrame,
                "TIMER_DECAY rb=%d game=%d rolling=%d important=0 match=0x%08X result=0x%08X",
                s_currentRbFrame,
                s_currentGameAbsFrame,
                s_currentRollingBack ? 1 : 0,
                (unsigned)match,
                (unsigned)result);
        }
    }
    return result;
}

int __cdecl Hook_Entity_UpdateComboStats(int entity,
                                         unsigned char a2,
                                         unsigned char a3,
                                         unsigned char a4,
                                         unsigned char a5,
                                         unsigned char a6,
                                         unsigned char a7) {
    if (!g_origEntityUpdateComboStats) {
        return entity;
    }

    if (!RollbackSession_IsActive()) {
        return g_origEntityUpdateComboStats(entity, a2, a3, a4, a5, a6, a7);
    }

    UpdateContextFromSession();
    const uintptr_t entityBase = (uintptr_t)entity;
    const uint32_t opponent = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_OPPONENT);
    const ComboSnapshot before = CaptureEntity(entityBase);
    const ComboSnapshot oppBefore = opponent ? CaptureEntity(opponent) : ComboSnapshot{};
    const int result = g_origEntityUpdateComboStats(entity, a2, a3, a4, a5, a6, a7);
    const ComboSnapshot after = CaptureEntity(entityBase);
    const ComboSnapshot oppAfter = opponent ? CaptureEntity(opponent) : ComboSnapshot{};

    ++s_updateStatsCount;
    char detail[96];
    _snprintf_s(detail, sizeof(detail), _TRUNCATE,
        "args=%u/%u/%u/%u/%u/%u opponent=0x%08X",
        a2, a3, a4, a5, a6, a7, opponent);
    LogEntityTransition("UPDATE_STATS", entityBase, before, after, detail);
    if (opponent) {
        LogEntityTransition("UPDATE_STATS_OPP", opponent, oppBefore, oppAfter, detail);
    }
    return result;
}

int __cdecl Hook_Entity_UpdateComboStat_1243(int entity, unsigned char value) {
    if (!g_origEntityUpdateComboStat1243) {
        return entity;
    }

    if (!RollbackSession_IsActive()) {
        return g_origEntityUpdateComboStat1243(entity, value);
    }

    UpdateContextFromSession();
    const uintptr_t entityBase = (uintptr_t)entity;
    const ComboSnapshot before = CaptureEntity(entityBase);
    const int result = g_origEntityUpdateComboStat1243(entity, value);
    const ComboSnapshot after = CaptureEntity(entityBase);

    ++s_updateStat1243Count;
    char detail[64];
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "value=%u", value);
    LogEntityTransition("UPDATE_STAT_1243", entityBase, before, after, detail);
    return result;
}

int __cdecl Hook_EffectSlots_Add(int entity, int slot_id) {
    if (!g_origEffectSlotsAdd) {
        return 0;
    }

    if (!RollbackSession_IsActive()) {
        return g_origEffectSlotsAdd(entity, slot_id);
    }

    UpdateContextFromSession();
    const uintptr_t entityBase = (uintptr_t)entity;
    const ComboSnapshot before = CaptureEntity(entityBase);
    const int result = g_origEffectSlotsAdd(entity, slot_id);
    const ComboSnapshot after = CaptureEntity(entityBase);

    ++s_slotAddCount;
    if (PresentationStateChanged(before, after)) {
        char detail[64];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, "slot_id=%d result=%d", slot_id, result);
        LogEntityTransition("ATTACHED_SLOT_ADD", entityBase, before, after, detail);
    }
    return result;
}

int __cdecl Hook_Effect_SetParams1(int entity, int sprite, int group, uint16_t anim) {
    if (!g_origEffectSetParams1) {
        return entity;
    }

    if (!RollbackSession_IsActive()) {
        return g_origEffectSetParams1(entity, sprite, group, anim);
    }

    UpdateContextFromSession();
    const int result = g_origEffectSetParams1(entity, sprite, group, anim);

    ++s_setParams1Count;
    NetplayLog_Verbose("COMBOFX", s_currentRbFrame,
        "SET_PARAMS1_COUNT rb=%d game=%d rolling=%d entity=0x%08X sprite=%d group=%d anim=%u count=%u",
        s_currentRbFrame,
        s_currentGameAbsFrame,
        s_currentRollingBack ? 1 : 0,
        (unsigned)entity,
        sprite,
        group,
        (unsigned)anim,
        s_setParams1Count);
    return result;
}

int __cdecl Hook_Effect_SetParams2(int entity, char draw_order, int overlay_sprite,
                                   int16_t overlay_x, int16_t overlay_y,
                                   int blend, char alpha) {
    if (!g_origEffectSetParams2) {
        return entity;
    }

    if (!RollbackSession_IsActive()) {
        return g_origEffectSetParams2(entity, draw_order, overlay_sprite, overlay_x, overlay_y, blend, alpha);
    }

    UpdateContextFromSession();
    const int result = g_origEffectSetParams2(entity, draw_order, overlay_sprite, overlay_x, overlay_y, blend, alpha);

    ++s_setParams2Count;
    NetplayLog_Verbose("COMBOFX", s_currentRbFrame,
        "SET_PARAMS2_COUNT rb=%d game=%d rolling=%d entity=0x%08X draw=%d overlay=%d pos=%d/%d blend=%d alpha=%d count=%u",
        s_currentRbFrame,
        s_currentGameAbsFrame,
        s_currentRollingBack ? 1 : 0,
        (unsigned)entity,
        (int)(uint8_t)draw_order,
        overlay_sprite,
        overlay_x,
        overlay_y,
        blend,
        (int)(uint8_t)alpha,
        s_setParams2Count);
    return result;
}

} // namespace Rollback
