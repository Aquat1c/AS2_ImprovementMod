#pragma once

#include <stddef.h>
#include <stdint.h>

// Mod-owned replacement for the game's native 各種設定 screen. The native one is
// Japanese-only and its key-config half is dead weight since the mod owns the
// bindings, so these rows are the only way a player reaches the settings in
// English. Row order and geometry follow the original (decomp sub_55CB90 for
// the values, sub_55D6F0 for the layout).
namespace NetMenu {

// Stage select and special characters are absent on purpose: the mod forces
// both on (netplay sync and UnlockAllContent respectively), so offering them as
// choices would be a lie.
enum GameSettingRow : int {
    kGameRowDifficulty = 0,
    kGameRowRounds,
    kGameRowSimpleEffects,
    kGameRowBattleRecording,
    kGameRowVoiceVolume,
    kGameRowSeVolume,
    kGameRowBgmVolume,
    kGameRowSystemVoice,
    kGameRowAiLearning,
    kGameRowMax,
};

// Rows visible right now (AI and special characters are unlock-gated exactly as
// the native screen gates them), plus the trailing Back entry.
int  GameSettingsMenu_RowCount();

// Maps a visible row index to a GameSettingRow, since gated rows shift indices.
int  GameSettingsMenu_RowAt(int visibleIndex);

// Draws the whole screen in the native layout, chrome included.
void GameSettingsMenu_RenderScreen(uint32_t selectedIndex, uint8_t alpha);

bool GameSettingsMenu_Adjust(int visibleIndex, bool left, bool right,
                             char* outStatus, size_t outStatusSize);

// True when the row opens a sub-page rather than holding a value.
bool GameSettingsMenu_RowOpensSubPage(int visibleIndex);

// Per-character voice volume sub-page.
int  GameSettingsVoice_RowCount();
void GameSettingsVoice_RenderScreen(uint32_t selectedIndex, uint8_t alpha);
bool GameSettingsVoice_Adjust(int index, bool left, bool right,
                              char* outStatus, size_t outStatusSize);

bool GameSettingsMenu_Locked();

// Category page, mirroring the native options menu: general settings, key
// settings, the two viewers, exit.
enum GameSettingsRootRow : int {
    kGameRootGeneral = 0,
    kGameRootKeys,
    kGameRootBattleHistory,
    kGameRootTitles,
    kGameRootExit,
    kGameRootCount,
};

int  GameSettingsRoot_RowCount();
void GameSettingsRoot_RenderScreen(uint32_t selectedIndex, uint8_t alpha);

// Hands the player to one of the native option sub-screens (the viewers we do
// not reimplement). Applied once mode 12 has finished its own init.
// Key config: two columns (P1/P2) over the 14 game buttons, with live capture.
int  GameSettingsKeys_RowCount();
void GameSettingsKeys_RenderScreen(uint32_t selectedIndex, uint8_t alpha);
// Left/right moves between the player columns rather than editing a value.
bool GameSettingsKeys_Adjust(bool left, bool right, char* outStatus, size_t outStatusSize);
// Returns true when the row was handled here; sets outClose for the Back row.
bool GameSettingsKeys_Confirm(int row, bool* outClose, char* outStatus, size_t outStatusSize);
// True while waiting for the player to press something; the menu must not eat
// that input.
bool GameSettingsKeys_CaptureActive();
void GameSettingsKeys_CancelCapture();

void GameSettingsMenu_RequestNativeSubstate(int substate);
void GameSettingsMenu_FrameUpdate();

} // namespace NetMenu
