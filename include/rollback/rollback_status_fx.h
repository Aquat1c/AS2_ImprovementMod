#pragma once

#include <stdint.h>

namespace Rollback {

using EffectEnqueue_t = char (__cdecl *)(int effect_id, char type, int16_t x, int16_t y);
using EffectDrawQueue_t = int (__cdecl *)(int match);

extern EffectEnqueue_t g_origEffectEnqueue;
extern EffectDrawQueue_t g_origEffectDrawQueue;

void RollbackStatusFx_Init();
void RollbackStatusFx_Shutdown();
void RollbackStatusFx_OnSessionBegin(int rollback_budget);
void RollbackStatusFx_OnSessionEnd(const char* reason);

void RollbackStatusFx_OnGekkoLoad(int32_t load_rb_frame, int32_t load_game_abs_frame);
void RollbackStatusFx_OnAdvanceBegin(int32_t rb_frame, int32_t game_abs_frame, bool rolling_back);
void RollbackStatusFx_OnGekkoBatchEnd(int32_t rb_frame, int32_t game_abs_frame);

char __cdecl Hook_Effect_Enqueue(int effect_id, char type, int16_t x, int16_t y);
int __cdecl Hook_Effect_DrawQueue(int match);

struct RollbackStatusFxSnapshot {
    int32_t committed_count;
    int32_t pending_count;
    int32_t corrected_count;
    int32_t total_normal_seen;
    int32_t total_resim_seen;
    int32_t total_exact_matches;
    int32_t total_shifted_matches;
    int32_t total_corrected_only;
    int32_t total_ghosts;
    int32_t total_unmonitored_seen;
    int32_t total_invalid_effect_ids;
    int32_t total_unknown_side;
    int32_t last_load_frame;
    int32_t last_reconcile_frame;
};

void RollbackStatusFx_GetSnapshot(RollbackStatusFxSnapshot* out);

} // namespace Rollback
