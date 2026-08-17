#pragma once

#include <stdint.h>

namespace Rollback {

using MatchUpdateComboTimers_t = int (__cdecl *)(int match);
using EntityUpdateComboStats_t = int (__cdecl *)(int entity,
                                                unsigned char a2,
                                                unsigned char a3,
                                                unsigned char a4,
                                                unsigned char a5,
                                                unsigned char a6,
                                                unsigned char a7);
using EntityUpdateComboStat1243_t = int (__cdecl *)(int entity, unsigned char value);
using EffectSlotsAdd_t = int (__cdecl *)(int entity, int slot_id);
using EffectSetParams1_t = int (__cdecl *)(int entity, int sprite, int group, uint16_t anim);
using EffectSetParams2_t = int (__cdecl *)(int entity, char draw_order, int overlay_sprite,
                                           int16_t overlay_x, int16_t overlay_y,
                                           int blend, char alpha);

extern MatchUpdateComboTimers_t g_origMatchUpdateComboTimers;
extern EntityUpdateComboStats_t g_origEntityUpdateComboStats;
extern EntityUpdateComboStat1243_t g_origEntityUpdateComboStat1243;
extern EffectSlotsAdd_t g_origEffectSlotsAdd;
extern EffectSetParams1_t g_origEffectSetParams1;
extern EffectSetParams2_t g_origEffectSetParams2;

void RollbackComboFx_Init();
void RollbackComboFx_Shutdown();
void RollbackComboFx_OnSessionBegin(int rollback_budget);
void RollbackComboFx_OnSessionEnd(const char* reason);
void RollbackComboFx_OnEngineSave(int32_t rb_frame, int32_t game_abs_frame);
void RollbackComboFx_OnEngineLoad(int32_t rb_frame, int32_t game_abs_frame);
void RollbackComboFx_OnAdvanceBegin(int32_t rb_frame, int32_t game_abs_frame, bool rolling_back);
void RollbackComboFx_OnEngineBatchEnd(int32_t rb_frame, int32_t game_abs_frame);

int __cdecl Hook_Match_UpdateComboTimers(int match);
int __cdecl Hook_Entity_UpdateComboStats(int entity,
                                         unsigned char a2,
                                         unsigned char a3,
                                         unsigned char a4,
                                         unsigned char a5,
                                         unsigned char a6,
                                         unsigned char a7);
int __cdecl Hook_Entity_UpdateComboStat_1243(int entity, unsigned char value);
int __cdecl Hook_EffectSlots_Add(int entity, int slot_id);
int __cdecl Hook_Effect_SetParams1(int entity, int sprite, int group, uint16_t anim);
int __cdecl Hook_Effect_SetParams2(int entity, char draw_order, int overlay_sprite,
                                   int16_t overlay_x, int16_t overlay_y,
                                   int blend, char alpha);

} // namespace Rollback
