#include "rollback/rollback_status_fx.h"

#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"

#include <algorithm>
#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace Rollback {
namespace {

static constexpr int kDefaultReserve = 512;
static constexpr int kRollbackBudgetFallback = 12;
static constexpr int kShiftMatchWindowFrames = 4;
static constexpr int kPruneMarginFrames = 8;

enum class StatusFxOrigin : uint8_t {
    Normal,
    RollbackResim,
};

struct StatusFxEvent {
    int32_t rb_frame;
    int32_t game_abs_frame;
    int16_t x;
    int16_t y;
    uint16_t effect_id;
    uint8_t type;
    uint8_t side;
    uint8_t ordinal;
    uint16_t slot_before;
    uint16_t slot_after;
    uint32_t mode;
    uint32_t substate;
    uint32_t substate_timer;
    uint32_t caller;
    StatusFxOrigin origin;
    bool consumed;
};

static bool s_initialized = false;
static bool s_sessionActive = false;
static bool s_inHook = false;
static bool s_inRollbackBatch = false;
static int s_rollbackBudget = kRollbackBudgetFallback;
static int32_t s_currentRbFrame = 0;
static int32_t s_currentGameAbsFrame = 0;
static bool s_currentRollingBack = false;
static RollbackStatusFxSnapshot s_stats = {};
static uint8_t s_suppressedSlotActive[EFFECT_MAX_SLOTS] = {};
static uint16_t s_suppressedSlotEffect[EFFECT_MAX_SLOTS] = {};
static int32_t s_suppressedSlotUntil[EFFECT_MAX_SLOTS] = {};
static uint32_t s_drawSuppressCount = 0;

// ----------------------------------------------------------------------------
// Status-message animation smoothing across rollback.
//
// Each effect entry stores its age at +0x0A (set to 0 on spawn, incremented per
// frame by Effect_Update, drives the message's pop/fade and lifetime). On a
// rollback the message is re-spawned at the corrected frame, so its age resets
// and the visible pop re-triggers — the same artifact the combo counter had.
//
// We keep a display-time age shadow per (effect_id, side) and substitute it into
// +0x0A only for the duration of the draw, restoring the true value immediately
// after. Effect_Update (sim phase) always sees the true age, so lifetime/despawn
// timing and determinism are unaffected — this changes only the rendered frame.
// ----------------------------------------------------------------------------
static constexpr uintptr_t EFFECT_OFF_AGE = 0x0A;   // word age/animation counter
static constexpr int kMaxMsgShadows = 16;
static constexpr int32_t kMsgAgeMax = 0x7FFF;

struct MsgAgeShadow {
    bool     used = false;
    uint16_t effect_id = 0;
    uint8_t  side = 0;
    int32_t  age = 0;
    bool     present_last = false;
    bool     present_this = false;
};
static MsgAgeShadow s_msgShadows[kMaxMsgShadows] = {};
static uint32_t s_msgSmoothedCount = 0;

static void ClearMsgAgeShadows() {
    for (int i = 0; i < kMaxMsgShadows; ++i) {
        s_msgShadows[i] = MsgAgeShadow{};
    }
}

static MsgAgeShadow* FindOrCreateMsgShadow(uint16_t effect_id, uint8_t side) {
    MsgAgeShadow* freeSlot = nullptr;
    for (int i = 0; i < kMaxMsgShadows; ++i) {
        if (s_msgShadows[i].used &&
            s_msgShadows[i].effect_id == effect_id &&
            s_msgShadows[i].side == side) {
            return &s_msgShadows[i];
        }
        if (!s_msgShadows[i].used && !freeSlot) {
            freeSlot = &s_msgShadows[i];
        }
    }
    if (freeSlot) {
        *freeSlot = MsgAgeShadow{};
        freeSlot->used = true;
        freeSlot->effect_id = effect_id;
        freeSlot->side = side;
    }
    return freeSlot;
}

static std::vector<StatusFxEvent> s_committed;
static std::vector<StatusFxEvent> s_pendingPredicted;
static std::vector<StatusFxEvent> s_corrected;

static bool IsPrmStatusMessageId(int effect_id) {
    return effect_id >= 166 && effect_id <= 190;
}

static bool IsSuspectStatusEffectId(int effect_id) {
    return effect_id >= 231 && effect_id <= 239;
}

static bool IsMonitoredStatusFx(int effect_id) {
    return IsPrmStatusMessageId(effect_id) || IsSuspectStatusEffectId(effect_id);
}

static uint16_t NormalizeEffectSlot(uint16_t slot) {
    return (uint16_t)(slot % EFFECT_MAX_SLOTS);
}

static void ClearSuppressedSlots() {
    memset(s_suppressedSlotActive, 0, sizeof(s_suppressedSlotActive));
    memset(s_suppressedSlotEffect, 0, sizeof(s_suppressedSlotEffect));
    memset(s_suppressedSlotUntil, 0, sizeof(s_suppressedSlotUntil));
}

static void SuppressSlot(uint16_t slot, uint16_t effect_id, int32_t ttl_frames) {
    if (effect_id == 0 || !IsMonitoredStatusFx(effect_id)) {
        return;
    }

    const uint16_t normalized = NormalizeEffectSlot(slot);
    const int32_t ttl = std::max<int32_t>(1, ttl_frames);
    s_suppressedSlotActive[normalized] = 1;
    s_suppressedSlotEffect[normalized] = effect_id;
    s_suppressedSlotUntil[normalized] = s_currentRbFrame + ttl;
}

static void SuppressEventSlot(const StatusFxEvent& ev, int32_t ttl_frames) {
    SuppressSlot(ev.slot_before, ev.effect_id, ttl_frames);
}

static void ReleaseSlot(uint16_t slot, uint16_t effect_id) {
    const uint16_t normalized = NormalizeEffectSlot(slot);
    if (!s_suppressedSlotActive[normalized]) {
        return;
    }

    if (effect_id != 0 && s_suppressedSlotEffect[normalized] != effect_id) {
        return;
    }

    s_suppressedSlotActive[normalized] = 0;
    s_suppressedSlotEffect[normalized] = 0;
    s_suppressedSlotUntil[normalized] = 0;
}

static void ReleaseEventSlot(const StatusFxEvent& ev) {
    ReleaseSlot(ev.slot_before, ev.effect_id);
}

static void PruneSuppressedSlots() {
    for (int i = 0; i < EFFECT_MAX_SLOTS; ++i) {
        if (s_suppressedSlotActive[i] && s_suppressedSlotUntil[i] < s_currentRbFrame) {
            s_suppressedSlotActive[i] = 0;
            s_suppressedSlotEffect[i] = 0;
            s_suppressedSlotUntil[i] = 0;
        }
    }
}

static bool IsSlotSuppressed(uint16_t slot, uint16_t effect_id) {
    const uint16_t normalized = NormalizeEffectSlot(slot);
    if (!s_suppressedSlotActive[normalized]) {
        return false;
    }

    if (s_suppressedSlotUntil[normalized] < s_currentRbFrame) {
        s_suppressedSlotActive[normalized] = 0;
        s_suppressedSlotEffect[normalized] = 0;
        s_suppressedSlotUntil[normalized] = 0;
        return false;
    }

    return s_suppressedSlotEffect[normalized] == effect_id;
}

static uint8_t GuessSideFromPosition(int16_t x, int16_t y) {
    const int p1x = ReadMemory<int16_t>(ADDR_P1_ENTITY_BASE + ENTITY_OFF_X_POS);
    const int p1y = ReadMemory<int16_t>(ADDR_P1_ENTITY_BASE + ENTITY_OFF_Y_POS);
    const int p2x = ReadMemory<int16_t>(ADDR_P2_ENTITY_BASE + ENTITY_OFF_X_POS);
    const int p2y = ReadMemory<int16_t>(ADDR_P2_ENTITY_BASE + ENTITY_OFF_Y_POS);

    const int dx1 = abs((int)x - p1x);
    const int dy1 = abs((int)y - p1y);
    const int dx2 = abs((int)x - p2x);
    const int dy2 = abs((int)y - p2y);
    const int score1 = dx1 + (dy1 / 4);
    const int score2 = dx2 + (dy2 / 4);

    if (score1 + 64 < score2) return 0;
    if (score2 + 64 < score1) return 1;
    return 2;
}

static uint8_t ComputeOrdinal(const std::vector<StatusFxEvent>& list,
                              int32_t rb_frame,
                              uint16_t effect_id,
                              uint8_t side,
                              uint8_t type) {
    uint8_t count = 0;
    for (const StatusFxEvent& ev : list) {
        if (ev.rb_frame == rb_frame &&
            ev.effect_id == effect_id &&
            ev.side == side &&
            ev.type == type) {
            ++count;
        }
    }
    return count;
}

static bool SideMatches(uint8_t a, uint8_t b) {
    return a == b || a == 2 || b == 2;
}

static void ReserveVectors() {
    s_committed.reserve(kDefaultReserve);
    s_pendingPredicted.reserve(kDefaultReserve);
    s_corrected.reserve(kDefaultReserve);
}

static void TrimOldCommittedEvents() {
    const int32_t keepAfter = s_currentRbFrame - s_rollbackBudget - kShiftMatchWindowFrames - kPruneMarginFrames;
    s_committed.erase(
        std::remove_if(s_committed.begin(), s_committed.end(),
            [keepAfter](const StatusFxEvent& ev) {
                return ev.rb_frame < keepAfter;
            }),
        s_committed.end());
}

static int FindExactMatch(const StatusFxEvent& pred) {
    for (int i = 0; i < (int)s_corrected.size(); ++i) {
        const StatusFxEvent& corr = s_corrected[i];
        if (corr.consumed) continue;
        if (corr.effect_id != pred.effect_id) continue;
        if (!SideMatches(corr.side, pred.side)) continue;
        if (corr.type != pred.type) continue;
        if (corr.ordinal != pred.ordinal) continue;
        if (corr.rb_frame != pred.rb_frame) continue;
        return i;
    }
    return -1;
}

static int FindShiftedMatch(const StatusFxEvent& pred) {
    int best = -1;
    int bestScore = 999999;
    for (int i = 0; i < (int)s_corrected.size(); ++i) {
        const StatusFxEvent& corr = s_corrected[i];
        if (corr.consumed) continue;
        if (corr.effect_id != pred.effect_id) continue;
        if (!SideMatches(corr.side, pred.side)) continue;
        if (corr.type != pred.type) continue;

        const int dist = abs(corr.rb_frame - pred.rb_frame);
        if (dist > kShiftMatchWindowFrames) {
            continue;
        }

        const int ordinalPenalty = corr.ordinal == pred.ordinal ? 0 : 100;
        const int score = dist + ordinalPenalty;
        if (score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

static void RecordStatusFxEvent(int effect_id,
                                char type,
                                int16_t x,
                                int16_t y,
                                uint16_t slot_before,
                                uint16_t slot_after,
                                int32_t rb_frame,
                                int32_t game_abs_frame,
                                bool rolling,
                                uint32_t caller) {
    if (effect_id < 0 || effect_id > 4095) {
        ++s_stats.total_invalid_effect_ids;
        NetplayLog_Write("STATUSFX", rb_frame,
            "INVALID id=%d type=%u rb=%d game=%d rolling=%d caller=0x%08X",
            effect_id,
            (unsigned)(uint8_t)type,
            rb_frame,
            game_abs_frame,
            rolling ? 1 : 0,
            caller);
        return;
    }

    StatusFxEvent ev{};
    ev.rb_frame = rb_frame;
    ev.game_abs_frame = game_abs_frame;
    ev.x = x;
    ev.y = y;
    ev.effect_id = (uint16_t)effect_id;
    ev.type = (uint8_t)type;
    ev.side = GuessSideFromPosition(x, y);
    ev.slot_before = slot_before;
    ev.slot_after = slot_after;
    ev.mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    ev.substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    ev.substate_timer = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    ev.caller = caller;
    ev.origin = rolling ? StatusFxOrigin::RollbackResim : StatusFxOrigin::Normal;

    if (ev.side == 2) {
        ++s_stats.total_unknown_side;
    }

    if (rolling) {
        ev.ordinal = ComputeOrdinal(s_corrected, ev.rb_frame, ev.effect_id, ev.side, ev.type);
        s_corrected.push_back(ev);
        ++s_stats.total_resim_seen;
        NetplayLog_Write("STATUSFX", rb_frame,
            "EVENT RESIM rb=%d game=%d mode=%u sub=%u/%u id=%u type=%u side=%u ord=%u x=%d y=%d slot=%u->%u caller=0x%08X",
            ev.rb_frame, ev.game_abs_frame, ev.mode, ev.substate, ev.substate_timer,
            ev.effect_id, ev.type, ev.side, ev.ordinal,
            ev.x, ev.y, ev.slot_before, ev.slot_after, ev.caller);
    } else {
        ev.ordinal = ComputeOrdinal(s_committed, ev.rb_frame, ev.effect_id, ev.side, ev.type);
        s_committed.push_back(ev);
        ++s_stats.total_normal_seen;
        NetplayLog_Write("STATUSFX", rb_frame,
            "EVENT NORMAL rb=%d game=%d mode=%u sub=%u/%u id=%u type=%u side=%u ord=%u x=%d y=%d slot=%u->%u caller=0x%08X",
            ev.rb_frame, ev.game_abs_frame, ev.mode, ev.substate, ev.substate_timer,
            ev.effect_id, ev.type, ev.side, ev.ordinal,
            ev.x, ev.y, ev.slot_before, ev.slot_after, ev.caller);
    }

    TrimOldCommittedEvents();
}

} // namespace

EffectEnqueue_t g_origEffectEnqueue = nullptr;
EffectDrawQueue_t g_origEffectDrawQueue = nullptr;

void RollbackStatusFx_Init() {
    if (s_initialized) {
        return;
    }

    ReserveVectors();
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.last_load_frame = -1;
    s_stats.last_reconcile_frame = -1;
    ClearSuppressedSlots();
    ClearMsgAgeShadows();
    s_msgSmoothedCount = 0;
    s_drawSuppressCount = 0;
    s_initialized = true;
    NetplayLog_Write("STATUSFX", -1,
        "INIT rollback_budget=%d monitored=166..190,231..239 reserve=%d draw_filter=1",
        s_rollbackBudget,
        kDefaultReserve);
}

void RollbackStatusFx_Shutdown() {
    RollbackStatusFx_OnSessionEnd("RollbackStatusFx_Shutdown");
    s_committed.clear();
    s_pendingPredicted.clear();
    s_corrected.clear();
    s_initialized = false;
}

void RollbackStatusFx_OnSessionBegin(int rollback_budget) {
    s_sessionActive = true;
    s_inRollbackBatch = false;
    s_rollbackBudget = rollback_budget > 0 ? rollback_budget : kRollbackBudgetFallback;
    s_currentRbFrame = 0;
    s_currentGameAbsFrame = 0;
    s_currentRollingBack = false;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.last_load_frame = -1;
    s_stats.last_reconcile_frame = -1;
    ClearSuppressedSlots();
    ClearMsgAgeShadows();
    s_msgSmoothedCount = 0;
    s_drawSuppressCount = 0;
    s_committed.clear();
    s_pendingPredicted.clear();
    s_corrected.clear();
    ReserveVectors();

    NetplayLog_Write("STATUSFX", 0,
        "SESSION BEGIN budget=%d monitored=166..190,231..239 draw_filter=1",
        s_rollbackBudget);
}

void RollbackStatusFx_OnSessionEnd(const char* reason) {
    if (!s_sessionActive && s_committed.empty() && s_pendingPredicted.empty() && s_corrected.empty()) {
        return;
    }

    NetplayLog_Write("STATUSFX", s_currentRbFrame,
        "SESSION END reason=\"%s\" committed=%d pending=%d corrected=%d normal=%d resim=%d exact=%d shifted=%d corrected_only=%d ghosts=%d draw_suppressed=%u unknown_side=%d invalid=%d",
        reason ? reason : "unknown",
        (int)s_committed.size(),
        (int)s_pendingPredicted.size(),
        (int)s_corrected.size(),
        s_stats.total_normal_seen,
        s_stats.total_resim_seen,
        s_stats.total_exact_matches,
        s_stats.total_shifted_matches,
        s_stats.total_corrected_only,
        s_stats.total_ghosts,
        s_drawSuppressCount,
        s_stats.total_unknown_side,
        s_stats.total_invalid_effect_ids);

    s_sessionActive = false;
    s_inRollbackBatch = false;
    s_committed.clear();
    s_pendingPredicted.clear();
    s_corrected.clear();
    ClearSuppressedSlots();
    ClearMsgAgeShadows();
    s_msgSmoothedCount = 0;
    s_drawSuppressCount = 0;
}

void RollbackStatusFx_OnEngineLoad(int32_t load_rb_frame, int32_t load_game_abs_frame) {
    if (!s_sessionActive) {
        return;
    }

    s_currentRbFrame = load_rb_frame;
    s_currentGameAbsFrame = load_game_abs_frame;

    size_t moved = 0;
    for (auto it = s_committed.begin(); it != s_committed.end();) {
        if (it->rb_frame > load_rb_frame) {
            StatusFxEvent pending = *it;
            pending.consumed = false;
            SuppressEventSlot(pending, s_rollbackBudget + kPruneMarginFrames);
            s_pendingPredicted.push_back(pending);
            it = s_committed.erase(it);
            ++moved;
        } else {
            ++it;
        }
    }

    s_corrected.clear();
    s_inRollbackBatch = true;
    s_stats.last_load_frame = load_rb_frame;

    NetplayLog_Write("STATUSFX", load_rb_frame,
        "LOAD rb=%d game=%d moved_pending=%u committed_left=%u pending_total=%u",
        load_rb_frame,
        load_game_abs_frame,
        (unsigned)moved,
        (unsigned)s_committed.size(),
        (unsigned)s_pendingPredicted.size());
}

void RollbackStatusFx_OnAdvanceBegin(int32_t rb_frame, int32_t game_abs_frame, bool rolling_back) {
    s_currentRbFrame = rb_frame;
    s_currentGameAbsFrame = game_abs_frame;
    s_currentRollingBack = rolling_back;

    if (rolling_back) {
        s_inRollbackBatch = true;
    }

    NetplayLog_Verbose("STATUSFX", rb_frame,
        "ADVANCE_BEGIN rb=%d game=%d rolling=%d pending=%u corrected=%u committed=%u",
        rb_frame,
        game_abs_frame,
        rolling_back ? 1 : 0,
        (unsigned)s_pendingPredicted.size(),
        (unsigned)s_corrected.size(),
        (unsigned)s_committed.size());
}

void RollbackStatusFx_OnEngineBatchEnd(int32_t rb_frame, int32_t game_abs_frame) {
    if (!s_sessionActive) {
        return;
    }

    s_currentRbFrame = rb_frame;
    s_currentGameAbsFrame = game_abs_frame;

    if (!s_inRollbackBatch && s_pendingPredicted.empty() && s_corrected.empty()) {
        TrimOldCommittedEvents();
        return;
    }

    int exact = 0;
    int shifted = 0;
    int ghosts = 0;
    int correctedOnly = 0;

    NetplayLog_Write("STATUSFX", rb_frame,
        "RECONCILE START rb=%d game=%d pending=%u corrected=%u committed=%u",
        rb_frame,
        game_abs_frame,
        (unsigned)s_pendingPredicted.size(),
        (unsigned)s_corrected.size(),
        (unsigned)s_committed.size());

    for (StatusFxEvent& pred : s_pendingPredicted) {
        int idx = FindExactMatch(pred);
        if (idx >= 0) {
            s_corrected[idx].consumed = true;
            ReleaseEventSlot(pred);
            ReleaseEventSlot(s_corrected[idx]);
            ++exact;
            ++s_stats.total_exact_matches;
            NetplayLog_Write("STATUSFX", rb_frame,
                "MATCH EXACT pred_rb=%d corr_rb=%d id=%u side=%u type=%u ord=%u pred_slot=%u corr_slot=%u",
                pred.rb_frame,
                s_corrected[idx].rb_frame,
                pred.effect_id,
                pred.side,
                pred.type,
                pred.ordinal,
                pred.slot_before,
                s_corrected[idx].slot_before);
            continue;
        }

        idx = FindShiftedMatch(pred);
        if (idx >= 0) {
            s_corrected[idx].consumed = true;
            ReleaseEventSlot(pred);
            ReleaseEventSlot(s_corrected[idx]);
            ++shifted;
            ++s_stats.total_shifted_matches;
            NetplayLog_Write("STATUSFX", rb_frame,
                "MATCH SHIFTED pred_rb=%d corr_rb=%d shift=%d id=%u side=%u type=%u ord=%u pred_slot=%u corr_slot=%u",
                pred.rb_frame,
                s_corrected[idx].rb_frame,
                s_corrected[idx].rb_frame - pred.rb_frame,
                pred.effect_id,
                pred.side,
                pred.type,
                pred.ordinal,
                pred.slot_before,
                s_corrected[idx].slot_before);
            continue;
        }

        ++ghosts;
        ++s_stats.total_ghosts;
        SuppressEventSlot(pred, s_rollbackBudget + kPruneMarginFrames);
        NetplayLog_Write("STATUSFX", rb_frame,
            "GHOST PREDICTED pred_rb=%d game=%d mode=%u sub=%u/%u id=%u side=%u type=%u ord=%u x=%d y=%d slot=%u caller=0x%08X",
            pred.rb_frame,
            pred.game_abs_frame,
            pred.mode,
            pred.substate,
            pred.substate_timer,
            pred.effect_id,
            pred.side,
            pred.type,
            pred.ordinal,
            pred.x,
            pred.y,
            pred.slot_before,
            pred.caller);
    }

    for (StatusFxEvent& corr : s_corrected) {
        if (corr.consumed) {
            continue;
        }

        ++correctedOnly;
        ++s_stats.total_corrected_only;
        ReleaseEventSlot(corr);
        NetplayLog_Write("STATUSFX", rb_frame,
            "CORRECTED ONLY corr_rb=%d game=%d mode=%u sub=%u/%u id=%u side=%u type=%u ord=%u x=%d y=%d slot=%u caller=0x%08X",
            corr.rb_frame,
            corr.game_abs_frame,
            corr.mode,
            corr.substate,
            corr.substate_timer,
            corr.effect_id,
            corr.side,
            corr.type,
            corr.ordinal,
            corr.x,
            corr.y,
            corr.slot_before,
            corr.caller);
    }

    for (const StatusFxEvent& corr : s_corrected) {
        StatusFxEvent committed = corr;
        committed.origin = StatusFxOrigin::Normal;
        committed.consumed = false;
        s_committed.push_back(committed);
    }

    NetplayLog_Write("STATUSFX", rb_frame,
        "RECONCILE END rb=%d exact=%d shifted=%d ghosts=%d corrected_only=%d committed=%u",
        rb_frame,
        exact,
        shifted,
        ghosts,
        correctedOnly,
        (unsigned)s_committed.size());

    s_pendingPredicted.clear();
    s_corrected.clear();
    s_inRollbackBatch = false;
    s_stats.last_reconcile_frame = rb_frame;
    TrimOldCommittedEvents();
    PruneSuppressedSlots();
}

char __cdecl Hook_Effect_Enqueue(int effect_id, char type, int16_t x, int16_t y) {
    if (!g_origEffectEnqueue) {
        return 0;
    }

    if (s_inHook) {
        return g_origEffectEnqueue(effect_id, type, x, y);
    }

    s_inHook = true;

    const uint32_t caller = (uint32_t)(uintptr_t)_ReturnAddress();
    const bool active = RollbackSession_IsActive();
    const bool monitored = IsMonitoredStatusFx(effect_id);
    const bool rolling = active && RollbackSession_IsRollingBack();
    const int32_t rbFrame = active ? RollbackSession_GetCurrentFrame() : -1;
    const int32_t gameAbs = active ? RollbackSession_GetCurrentGameAbsFrame()
                                   : (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    const uint16_t slotBefore = active ? (uint16_t)ReadMemory<uint32_t>(ADDR_EFFECT_INDEX) : 0;

    const char result = g_origEffectEnqueue(effect_id, type, x, y);

    if (!active) {
        if (monitored) {
            NetplayLog_Verbose("STATUSFX", rbFrame,
                "OUTSIDE_SESSION effect_id=%d type=%u x=%d y=%d game=%d caller=0x%08X",
                effect_id,
                (unsigned)(uint8_t)type,
                x,
                y,
                gameAbs,
                caller);
        }
        s_inHook = false;
        return result;
    }

    const uint16_t slotAfter = (uint16_t)ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
    if (monitored) {
        RecordStatusFxEvent(effect_id,
                            type,
                            x,
                            y,
                            slotBefore,
                            slotAfter,
                            rbFrame,
                            gameAbs,
                            rolling,
                            caller);
    } else {
        ++s_stats.total_unmonitored_seen;
        NetplayLog_Verbose("STATUSFX", rbFrame,
            "UNMONITORED effect_id=%d type=%u x=%d y=%d rb=%d game=%d rolling=%d slot=%u->%u caller=0x%08X",
            effect_id,
            (unsigned)(uint8_t)type,
            x,
            y,
            rbFrame,
            gameAbs,
            rolling ? 1 : 0,
            slotBefore,
            slotAfter,
            caller);
    }

    s_inHook = false;
    return result;
}

int __cdecl Hook_Effect_DrawQueue(int match) {
    if (!g_origEffectDrawQueue) {
        return 0;
    }

    if (!RollbackSession_IsActive()) {
        return g_origEffectDrawQueue(match);
    }

    struct HiddenSlot {
        uint16_t slot;
        uint32_t effect_id;
    };

    HiddenSlot hidden[EFFECT_MAX_SLOTS];
    int hiddenCount = 0;

    struct SmoothedSlot {
        uint16_t slot;
        uint16_t true_age;
    };
    SmoothedSlot smoothed[EFFECT_MAX_SLOTS];
    int smoothedCount = 0;

    const bool rolling = RollbackSession_IsRollingBack();
    PruneSuppressedSlots();

    // Age smoothing runs only on the real display draw (post-sim, not mid-resim).
    const bool doSmooth = !rolling;
    if (doSmooth) {
        for (int i = 0; i < kMaxMsgShadows; ++i) {
            s_msgShadows[i].present_this = false;
        }
    }

    for (uint16_t slot = 0; slot < EFFECT_MAX_SLOTS; ++slot) {
        const uintptr_t base = ADDR_EFFECT_ARRAY + ((uintptr_t)slot * EFFECT_ENTRY_SIZE);
        const uint32_t effectId = ReadMemory<uint32_t>(base);
        if (!IsMonitoredStatusFx((int)effectId)) {
            continue;
        }

        const bool hide = rolling || IsSlotSuppressed(slot, (uint16_t)effectId);
        if (hide) {
            if (WriteMemory<uint32_t>(base, 0)) {
                hidden[hiddenCount].slot = slot;
                hidden[hiddenCount].effect_id = effectId;
                ++hiddenCount;
            }
            continue;
        }

        // Visible status message — keep its pop/fade animation continuous across
        // rollback by substituting a display-time age for the resimulated one.
        if (!doSmooth) {
            continue;
        }
        const int16_t x = ReadMemory<int16_t>(base + 6);
        const int16_t y = ReadMemory<int16_t>(base + 8);
        const uint8_t side = GuessSideFromPosition(x, y);
        const int32_t trueAge = (int32_t)ReadMemory<uint16_t>(base + EFFECT_OFF_AGE);
        MsgAgeShadow* sh = FindOrCreateMsgShadow((uint16_t)effectId, side);
        if (!sh) {
            continue;
        }

        // A message present last frame keeps advancing in display time; a newly
        // appearing one adopts the game's age so it pops normally. During normal
        // play both stay in lockstep (no override); only a rollback that
        // re-anchors the spawn frame makes them diverge — and then the shadow
        // suppresses the visible re-pop.
        int32_t newAge;
        if (sh->present_this) {
            // A second live slot this frame shares the same (effect_id, side)
            // key — reuse the age already advanced so duplicates stay in sync and
            // the shadow advances at most once per display frame.
            newAge = sh->age;
        } else {
            newAge = sh->present_last ? sh->age + 1 : trueAge;
            if (newAge < 0) newAge = 0;
            if (newAge > kMsgAgeMax) newAge = kMsgAgeMax;
            sh->age = newAge;
            sh->present_this = true;
        }

        if ((uint16_t)newAge != (uint16_t)trueAge &&
            WriteMemory<uint16_t>(base + EFFECT_OFF_AGE, (uint16_t)newAge)) {
            smoothed[smoothedCount].slot = slot;
            smoothed[smoothedCount].true_age = (uint16_t)trueAge;
            ++smoothedCount;
        }
    }

    const int result = g_origEffectDrawQueue(match);

    for (int i = 0; i < hiddenCount; ++i) {
        const uintptr_t base = ADDR_EFFECT_ARRAY + ((uintptr_t)hidden[i].slot * EFFECT_ENTRY_SIZE);
        WriteMemory<uint32_t>(base, hidden[i].effect_id);
    }
    for (int i = 0; i < smoothedCount; ++i) {
        const uintptr_t base = ADDR_EFFECT_ARRAY + ((uintptr_t)smoothed[i].slot * EFFECT_ENTRY_SIZE);
        WriteMemory<uint16_t>(base + EFFECT_OFF_AGE, smoothed[i].true_age);
    }

    if (doSmooth) {
        for (int i = 0; i < kMaxMsgShadows; ++i) {
            if (!s_msgShadows[i].used) {
                continue;
            }
            s_msgShadows[i].present_last = s_msgShadows[i].present_this;
            if (!s_msgShadows[i].present_this) {
                s_msgShadows[i].used = false;
            }
        }
        if (smoothedCount > 0) {
            s_msgSmoothedCount += (uint32_t)smoothedCount;
            NetplayLog_Verbose("STATUSFX", s_currentRbFrame,
                "DRAW_SMOOTH smoothed=%d total=%u rb=%d game=%d",
                smoothedCount,
                s_msgSmoothedCount,
                s_currentRbFrame,
                s_currentGameAbsFrame);
        }
    }

    if (hiddenCount > 0) {
        s_drawSuppressCount += (uint32_t)hiddenCount;
        NetplayLog_Verbose("STATUSFX", s_currentRbFrame,
            "DRAW_FILTER hidden=%d rolling=%d rb=%d game=%d",
            hiddenCount,
            rolling ? 1 : 0,
            s_currentRbFrame,
            s_currentGameAbsFrame);
    }

    return result;
}

void RollbackStatusFx_GetSnapshot(RollbackStatusFxSnapshot* out) {
    if (!out) {
        return;
    }

    *out = s_stats;
    out->committed_count = (int32_t)s_committed.size();
    out->pending_count = (int32_t)s_pendingPredicted.size();
    out->corrected_count = (int32_t)s_corrected.size();
}

} // namespace Rollback
