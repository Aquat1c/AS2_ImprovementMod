#pragma once

#include <stdint.h>

namespace Rollback {

using SEPlay_t = char (__cdecl *)(int sound_id);

extern SEPlay_t g_origSEPlay;

void RollbackAudio_Init();
void RollbackAudio_Shutdown();
void RollbackAudio_OnSessionBegin(int rollback_budget);
void RollbackAudio_OnSessionEnd(const char* reason);

void RollbackAudio_OnEngineLoad(int32_t load_rb_frame, int32_t load_game_abs_frame);
void RollbackAudio_OnAdvanceBegin(int32_t rb_frame, int32_t game_abs_frame, bool rolling_back);
void RollbackAudio_OnEngineBatchEnd(int32_t rb_frame, int32_t game_abs_frame);

char __cdecl Hook_SE_Play(int sound_id);

struct RollbackAudioSnapshot {
    int32_t committed_count;
    int32_t pending_count;
    int32_t corrected_count;
    int32_t total_normal_played;
    int32_t total_resim_captured;
    int32_t total_exact_matches;
    int32_t total_shifted_matches;
    int32_t total_late_played;
    int32_t total_ghosts;
    int32_t total_invalid_sound_ids;
    int32_t total_vanilla_frame_dupes;
};

void RollbackAudio_GetSnapshot(RollbackAudioSnapshot* out);

} // namespace Rollback
