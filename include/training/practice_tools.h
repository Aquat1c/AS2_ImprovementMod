/**
 * Alice Senki 2 - Practice Mode Tools
 *
 * Pause/unpause, single-frame advance, controller swap with CPU flag
 * management, hitbox toggle.  All features gated to training mode
 * (GAMETYPE_TRAINING in MODE_MATCH).
 *
 * Integration points:
 *   - input_sync_hooks.cpp: ShouldSuppressAdvanceFrame checks PracticeTools_ShouldFreezeFrame
 *   - input_override.cpp:   Hook_InputDispatcher returns -1 when practice freeze active
 *   - mod_main.cpp:         ModOnFrame calls PracticeTools_FrameUpdate
 *   - mod_main.cpp:         ModOnPresent calls PracticeTools_RenderHUD
 *   - mod_menu.cpp:         Practice tab calls PracticeTools_RenderImGui
 */

#pragma once

#include "patches/hud_toggle.h"

#include "training/auto_block.h"

#include <stdbool.h>
#include <stdint.h>

// Auto-block decisions must survive a savestate load unchanged, so the
// sequence generation, contact count, random roll and frame-latched lane
// travel with the slot rather than being re-derived.
struct PracticeToolsRuntimeState {
	bool paused;
	bool stepRequested;
	uint32_t stepCounter;
	Training::AutoBlockSequenceState autoBlockSequence;
	Training::FrameGuardPlan frameGuardPlan;
	uint32_t lastProcessedSimFrame;
};

// Lifecycle
void PracticeTools_Init();
void PracticeTools_Shutdown();

// Called once per frame from ModOnFrame (before input).
// Handles hotkey edge detection, auto-cleanup on mode exit.
void PracticeTools_FrameUpdate();

// ImGui tab contents — drawn inside the mod menu Practice tab.
void PracticeTools_RenderImGui();

// In-game HUD overlay — drawn every present frame via an ImGui overlay draw list.
// Shows frame step state, toast notifications, etc.
void PracticeTools_RenderHUD();
bool PracticeTools_HasVisibleHud();
bool PracticeTools_ShouldRenderHudBehindMenu();

// Freeze query — checked by input_sync_hooks to suppress frame advancement.
// IMPORTANT: This is a pure query — does NOT modify step state.
bool PracticeTools_ShouldFreezeFrame();

// Called by Hook_AdvanceFrame after g_origAdvanceFrame completes.
// Clears the one-shot step flag so the next frame re-freezes.
void PracticeTools_OnFrameAdvanced();

// State queries
bool PracticeTools_IsPracticeModeActive();
bool PracticeTools_IsPaused();
bool PracticeTools_IsControlSwapped();
void PracticeTools_ApplyControlSwapState(bool swapped);
void PracticeTools_SyncControlSwapState();
void PracticeTools_SetPaused(bool paused);
void PracticeTools_CaptureRuntimeState(PracticeToolsRuntimeState* out);
void PracticeTools_RestoreRuntimeState(const PracticeToolsRuntimeState* state);

// ============================================================================
// Settings bridge
// ============================================================================
// The pause menu needs to read and cycle the same practice settings the ImGui
// panel edits, without duplicating their ranges or their labels. Semantics stay
// in practice_runtime; the menu is a thin renderer over this.

enum PracticeSettingId : int {
	PRACTICE_SET_DUMMY_BACKEND = 0,
	PRACTICE_SET_BLOCK_MODE,
	PRACTICE_SET_STANCE,
	PRACTICE_SET_JUMP_MODE,
	PRACTICE_SET_JUMP_CADENCE,
	PRACTICE_SET_DEFENSIVE_RESPONSE,
	PRACTICE_SET_NATIVE_CPU,
	PRACTICE_SET_NATIVE_AIR_TECH,
	PRACTICE_SET_NATIVE_GROUND_TECH,
	PRACTICE_SET_NATIVE_BLOCK_TYPE,
	PRACTICE_SET_NATIVE_DUMMY_STATE,
	PRACTICE_SET_RECOVERY_HP,
	PRACTICE_SET_RECOVERY_METER,
	PRACTICE_SET_RECOVERY_GUARD,
	PRACTICE_SET_RECOVERY_BOTH_NEUTRAL,
	PRACTICE_SET_RECOVERY_DELAY,
	// The three "Custom" modes restore a number the menu has to be able to set,
	// so these are typed rather than cycled. See PracticeSetting_IsNumeric.
	PRACTICE_SET_RECOVERY_HP_VALUE,
	PRACTICE_SET_RECOVERY_METER_VALUE,
	PRACTICE_SET_RECOVERY_GUARD_VALUE,
	PRACTICE_SET_TRIGGERS_ENABLED,
	PRACTICE_SET_TRIGGER_TARGET,
	PRACTICE_SET_TRIGGER_RANDOM,
	PRACTICE_SET_WAKE_BUFFER,
	// One row per trigger, so all five and their configured actions are visible
	// at once. The row toggles the trigger; opening it edits the action in a
	// popup, where a 22-entry motion list has room to be a grid.
	PRACTICE_SET_TRIGGER_1,
	PRACTICE_SET_TRIGGER_2,
	PRACTICE_SET_TRIGGER_3,
	PRACTICE_SET_TRIGGER_4,
	PRACTICE_SET_TRIGGER_5,
	PRACTICE_SET_TRIGGER_6,
	// These address whichever slot PracticeTrigger_SelectSlot last chose.
	PRACTICE_SET_TRIGGER_BUTTON,
	PRACTICE_SET_TRIGGER_DELAY,
	PRACTICE_SET_HITBOXES,
	PRACTICE_SET_COMBO_OVERLAY,
	PRACTICE_SET_INPUT_DISPLAY,
	PRACTICE_SET_DAMAGE_DISPLAY,
	PRACTICE_SET_FRAME_ADVANTAGE,
	PRACTICE_SET_HEALTH_REGEN,
	PRACTICE_SET_METER_LEVEL,
	PRACTICE_SET_CONTROL_SWAP,
	PRACTICE_SET_MACRO_SLOT,
	PRACTICE_SET_PAUSED,
	// One row per HUD element, in HudElement order. Contiguous so the sub-page
	// can walk them as a range.
	PRACTICE_SET_HUD_FIRST,
	PRACTICE_SET_HUD_LAST = PRACTICE_SET_HUD_FIRST + HUD_ELEM_COUNT - 1,
	PRACTICE_SET_COUNT,
};

// Dumps everything the practice tools know to the log in one block: the match
// context, both fighters, which side the dummy is and who owns its input, the
// hook install state, the defence arming state, and every setting with the same
// label and value text the menu shows. Called when the pause menu opens, so a
// report of "setting X does nothing" arrives with the whole picture attached
// instead of having to be reconstructed from behaviour.
void PracticeTools_LogDiagnosticSnapshot(const char* reason);

int  PracticeSetting_Get(int setting);
// Steps the value by delta, wrapping at the ends the way the vanilla rows do.
void PracticeSetting_Cycle(int setting, int delta);
const char* PracticeSetting_Label(int setting);
const char* PracticeSetting_ValueText(int setting);
// False when the row exists but the current configuration makes it inert (the
// native tech rows while CPU is on, for instance).
bool PracticeSetting_Enabled(int setting);

// True for rows that hold a free number rather than a list of choices. The menu
// edits these by typing instead of cycling, because "Custom" is meaningless if
// the value behind it cannot be set.
bool PracticeSetting_IsNumeric(int setting);
int  PracticeSetting_NumericMin(int setting);
int  PracticeSetting_NumericMax(int setting);
void PracticeSetting_SetNumeric(int setting, int value);

// One-shot pause-menu actions.
enum PracticeActionId : int {
	PRACTICE_ACT_ROUND_RESET = 0,
	PRACTICE_ACT_SWAP_SIDES,
	PRACTICE_ACT_POSITION_MID,
	PRACTICE_ACT_POSITION_CORNER,
	PRACTICE_ACT_SAVE_STATE,
	PRACTICE_ACT_LOAD_STATE,
	PRACTICE_ACT_SAVE_POSITION,
	PRACTICE_ACT_LOAD_POSITION,
	PRACTICE_ACT_MACRO_RECORD,
	PRACTICE_ACT_MACRO_PLAY,
	PRACTICE_ACT_POSITION_ROUND_START,
	PRACTICE_ACT_FRAME_STEP,
	PRACTICE_ACT_COUNT,
};

// Returns a short status string for the hint line, or nullptr.
const char* PracticeAction_Invoke(int action);

/// Roster name for a character id (0..21), or "Unknown".
///
/// The single source of truth. Ids 19 and 20 are Little Princess and TADA in
/// that order - the game's own per-character dispatch and its sprite tables
/// both agree, and getting them the wrong way round mislabels two characters.
const char* PracticeTools_CharacterName(uint32_t charId);

// --- Trigger slots -------------------------------------------------------
// The pause menu shows one row per trigger and edits the selected one's action
// in a popup. The motion list is long enough to want a grid, so it is exposed
// by index rather than as a cycling value.

int         PracticeTrigger_Count();
void        PracticeTrigger_SelectSlot(int slot);
int         PracticeTrigger_SelectedSlot();
const char* PracticeTrigger_Name(int slot);
// "Off", or the configured action such as "236X+A".
const char* PracticeTrigger_Summary(int slot);

int         PracticeTrigger_MotionCount();
const char* PracticeTrigger_MotionLabel(int motion);
int         PracticeTrigger_GetMotion();
void        PracticeTrigger_SetMotion(int motion);

// The picker offers the dummy's own moves, taken from the translated guide, so
// it cannot suggest a motion that character does not have. Count is 0 when the
// guide had no parsed entry, in which case the caller falls back to the full
// motion list plus a separate button field.
int         PracticeTrigger_MoveCount();
const char* PracticeTrigger_MoveLabel(int index);
int         PracticeTrigger_SelectedMove();
void        PracticeTrigger_SelectMove(int index);
// Name of the dummy's defensive mechanic, or "" when the guide has none.
const char* PracticeDummy_DefenceName();

// Push a toast notification to the in-game HUD.
// Only visible in practice mode. Short messages, auto-fading.
void PracticeTools_Toast(const char* text, unsigned int color);

// The most recent status message, with a serial that increments per message.
// The pause menu polls this so a toast raised outside the menu - a hotkey
// savestate, a macro, an auto-recovery - is reported there too, rather than
// only in the ImGui overlay the player cannot see while paused.
const char* PracticeTools_LatestStatus(uint32_t* outSerial);

// --- Command History Hook (redirects to P2 data when controls swapped) ---
typedef char (__cdecl *CmdHistoryUpdate_t)(int16_t* matchBase);
extern CmdHistoryUpdate_t g_origCmdHistoryUpdate;
char __cdecl Hook_CmdHistoryUpdate(int16_t* matchBase);
