/**
 * Alice Senki 2 - Netplay game settings sync
 *
 * Synchronizes host-owned vanilla match settings that are outside the
 * rollback state itself. Currently this covers the number-of-rounds option.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Net {

struct LockedMatchConfig;

struct GameSettingsSyncSnapshot {
    bool    session_cached;
    bool    persistent_loaded;
    uint8_t local_round_option;
    uint8_t current_round_option;
    uint8_t match_round_option;
    uint8_t persisted_round_option;
    uint8_t last_applied_round_option;
    int     current_rounds_to_win;
    int     persisted_rounds_to_win;
    char    settings_path[260];
    char    status[128];
};

void GameSettingsSync_Init();
void GameSettingsSync_Shutdown();
void GameSettingsSync_FrameUpdate();

// Called when a netplay room/session begins. The cached values are restored
// only when the session ends, not between rematches in the same room.
void GameSettingsSync_BeginNetplaySession(const char* reason);
void GameSettingsSync_RestoreLocalSession(const char* reason);

// Round option is the vanilla zero-based byte: 0,1,2 => first to 1,2,3 wins.
uint8_t GameSettingsSync_ReadRoundOption();
uint8_t GameSettingsSync_NormalizeRoundOption(uint8_t roundOption, const char* reason);
int GameSettingsSync_RoundsToWin(uint8_t roundOption);
void GameSettingsSync_FormatRoundLabel(uint8_t roundOption, char* out, size_t outSize);

uint8_t GameSettingsSync_BuildHostRoundOption(const char* reason);
void GameSettingsSync_ApplyRoundOption(uint8_t roundOption, const char* reason);
void GameSettingsSync_ApplyLockedConfig(const LockedMatchConfig* config, const char* reason);
void GameSettingsSync_GetSnapshot(GameSettingsSyncSnapshot* out);

} // namespace Net
