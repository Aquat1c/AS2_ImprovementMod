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

void RollbackStatusFx_Init() {
    if (s_initialized) {
        return;
    }

    ReserveVectors();
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.last_load_frame = -1;
    s_stats.last_reconcile_frame = -1;
    s_initialized = true;
    NetplayLog_Write("STATUSFX", -1,
        "INIT rollback_budget=%d monitored=166..190,231..239 reserve=%d",
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
    s_committed.clear();
    s_pendingPredicted.clear();
    s_corrected.clear();
    ReserveVectors();

    NetplayLog_Write("STATUSFX", 0,
        "SESSION BEGIN budget=%d monitored=166..190,231..239",
        s_rollbackBudget);
}

void RollbackStatusFx_OnSessionEnd(const char* reason) {
    if (!s_sessionActive && s_committed.empty() && s_pendingPredicted.empty() && s_corrected.empty()) {
        return;
    }

    NetplayLog_Write("STATUSFX", s_currentRbFrame,
        "SESSION END reason=\"%s\" committed=%d pending=%d corrected=%d normal=%d resim=%d exact=%d shifted=%d corrected_only=%d ghosts=%d unknown_side=%d invalid=%d",
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
        s_stats.total_unknown_side,
        s_stats.total_invalid_effect_ids);

    s_sessionActive = false;
    s_inRollbackBatch = false;
    s_committed.clear();
    s_pendingPredicted.clear();
    s_corrected.clear();
}

void RollbackStatusFx_OnGekkoLoad(int32_t load_rb_frame, int32_t load_game_abs_frame) {
    if (!s_sessionActive) {
        return;
    }

    size_t moved = 0;
    for (auto it = s_committed.begin(); it != s_committed.end();) {
        if (it->rb_frame > load_rb_frame) {
            it->consumed = false;
            s_pendingPredicted.push_back(*it);
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

void RollbackStatusFx_OnGekkoBatchEnd(int32_t rb_frame, int32_t game_abs_frame) {
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
    const uint16_t slotBefore = (uint16_t)ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);

    const char result = g_origEffectEnqueue(effect_id, type, x, y);

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
