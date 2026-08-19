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
//
// Simple Effects is absent for a harder reason: it is not a display option. It
// gates sub_4C47C0, which writes particle state AND calls rand() - so a peer
// with it on steps the shared RNG a different number of times per frame than a
// peer with it off, and the match desyncs. It is forced to 0 in
// game_settings_sync rather than offered, so there is no way to set it wrong.
enum GameSettingRow : int {
    kGameRowDifficulty = 0,
    kGameRowRounds,
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
    kGameRootSystem,
    kGameRootKeys,
    kGameRootBattleHistory,
    kGameRootTitles,
    kGameRootExit,
    kGameRootCount,
};

int  GameSettingsRoot_RowCount();

// Settings that until now existed only in the ImGui overlay. Practice options
// are deliberately absent: those belong to the pause menu replacement.
int  GameSettingsSystem_RowCount();
void GameSettingsSystem_RenderScreen(uint32_t selectedIndex, uint8_t alpha);
bool GameSettingsSystem_Adjust(int row, bool left, bool right,
                               char* outStatus, size_t outStatusSize);
// True for the row that leads into the practice hotkey rebinder.
bool GameSettingsSystem_RowOpensSubPage(int row);
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

// Practice hotkeys: the keys the mod itself owns (freeze, step, savestates,
// macros), which were compiled in until now. Same capture flow as key config,
// one column because they are not per-player.
int  GameSettingsHotkeys_RowCount();
void GameSettingsHotkeys_RenderScreen(uint32_t selectedIndex, uint8_t alpha);
// Left/right unbinds rather than editing a value.
bool GameSettingsHotkeys_Adjust(int row, bool left, bool right,
                                char* outStatus, size_t outStatusSize);
bool GameSettingsHotkeys_Confirm(int row, bool* outClose,
                                 char* outStatus, size_t outStatusSize);
bool GameSettingsHotkeys_CaptureActive();
void GameSettingsHotkeys_CancelCapture();

void GameSettingsMenu_RequestNativeSubstate(int substate);
void GameSettingsMenu_FrameUpdate();

} // namespace NetMenu
