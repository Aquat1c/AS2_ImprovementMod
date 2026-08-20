#include "practice_internal.h"

#include "training/action_state_classifier.h"
#include "training/frame_advantage.h"
#include "training/hotkey_config.h"
#include "training/input_macro.h"
#include "training/auto_block.h"
#include "training/character_moves.h"
#include "patches/practice_defense_hooks.h"
#include "core/mod_main.h"
#include "rollback/savestate.h"
#include "patches/memory_utils.h"
#include "ui/hitbox_viewer.h"
#include "ui/mod_menu.h"
#include "input_system.h"
#include "as2_constants.h"
#include "log_window.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

uintptr_t GetEntityBase(int playerIndex);

using PracticeInternal::IsPracticeModeNow;
using PracticeInternal::PushToast;
using PracticeInternal::TOAST_DURATION;
using PracticeInternal::TOAST_FADE_START;
using PracticeInternal::UpdateToasts;

namespace {

auto& s_initialized = PracticeInternal::g_initialized;
auto& s_paused = PracticeInternal::g_paused;
auto& s_stepRequested = PracticeInternal::g_stepRequested;
auto& s_stepCounter = PracticeInternal::g_stepCounter;
auto& s_wasActive = PracticeInternal::g_wasActive;
auto& s_toasts = PracticeInternal::g_toasts;
auto& s_toastCount = PracticeInternal::g_toastCount;

static const int kPracticePlayerCount = 2;
static const int kPracticeRosterCount = 22;
static const int kMeterMax = 9000;
static const int kGuardGaugeMax = 10000;
// The command table's longest charge is 45 frames (0x723480 cmd 39-41, which is
// Nalzgis's [2]8X); at 30 it never armed. The check is "held > threshold - 1",
// so this has to clear 45 outright.
static const int kSyntheticChargeFrames = 50;
static const int kJumpHoldFrames = 3;
static const int kTriggerCount = 5;
// Defensive response for the mod-driven dummy; see kDefensiveResponseLabels.
static int g_defensiveResponse = 0;
static const uint32_t kInvalidFrame = 0xFFFFFFFFu;
static const int16_t kStageCenterX = 8000;
static const int kTrainingNeutralAction = 22;
static const int kTrainingNeutralSprite = 25;
static const int kTrainingNeutralRenderGroup = 3;
static const uint16_t kTrainingNeutralAnimIndex = 0x20;

enum PracticeTabId {
    PRACTICE_TAB_OVERVIEW = 0,
    PRACTICE_TAB_OPPONENT,
    PRACTICE_TAB_VALUES,
    PRACTICE_TAB_OPTIONS,
    PRACTICE_TAB_TRIGGERS,
    PRACTICE_TAB_MACROS,
    PRACTICE_TAB_HOTKEYS,
};

enum DummyBlockMode {
    DUMMY_BLOCK_NONE = 0,
    DUMMY_BLOCK_ALL,
    DUMMY_BLOCK_FIRST_HIT,
    DUMMY_BLOCK_AFTER_FIRST_HIT,
    DUMMY_BLOCK_RANDOM,
    DUMMY_BLOCK_ADAPTIVE,
};

// Auto-Block combo order; kept as-is so saved configs stay valid.
static Training::BlockPolicy BlockPolicyForMode(int blockMode) {
    switch (blockMode) {
        case DUMMY_BLOCK_ALL:             return Training::BlockPolicy::All;
        case DUMMY_BLOCK_FIRST_HIT:       return Training::BlockPolicy::FirstHit;
        case DUMMY_BLOCK_AFTER_FIRST_HIT: return Training::BlockPolicy::AfterFirstHit;
        case DUMMY_BLOCK_RANDOM:          return Training::BlockPolicy::Random;
        case DUMMY_BLOCK_ADAPTIVE:        return Training::BlockPolicy::Adaptive;
        case DUMMY_BLOCK_NONE:
        default:                          return Training::BlockPolicy::Off;
    }
}

enum DummyStanceMode {
    DUMMY_STANCE_NEUTRAL = 0,
    DUMMY_STANCE_STAND,
    DUMMY_STANCE_CROUCH,
    DUMMY_STANCE_JUMP,
};

enum DummyJumpMode {
    DUMMY_JUMP_DISABLED = 0,
    DUMMY_JUMP_NEUTRAL,
    DUMMY_JUMP_FORWARD,
    DUMMY_JUMP_BACKWARD,
    DUMMY_JUMP_RANDOM,
};

enum RecoveryHpMode {
    RECOVERY_HP_OFF = 0,
    RECOVERY_HP_FULL,
    RECOVERY_HP_CUSTOM,
};

enum RecoveryMeterMode {
    RECOVERY_METER_OFF = 0,
    RECOVERY_METER_ZERO,
    RECOVERY_METER_3000,
    RECOVERY_METER_6000,
    RECOVERY_METER_9000,
    RECOVERY_METER_CUSTOM,
};

enum RecoveryGuardMode {
    RECOVERY_GUARD_OFF = 0,
    RECOVERY_GUARD_FULL,
    RECOVERY_GUARD_CUSTOM,
};

enum DummyControlMode {
    DUMMY_CONTROL_MOD = 0,
    DUMMY_CONTROL_NATIVE,
};

enum TriggerTarget {
    TRIGGER_TARGET_P1 = 0,
    TRIGGER_TARGET_P2,
    TRIGGER_TARGET_BOTH,
};

enum TriggerEventId {
    TRIGGER_AFTER_BLOCK = 0,
    TRIGGER_ON_WAKEUP,
    TRIGGER_AFTER_HITSTUN,
    TRIGGER_AFTER_AIRTECH,
    TRIGGER_AFTER_GROUNDTECH,
};

enum ScriptActionKind {
    SCRIPT_ACTION_NONE = 0,
    SCRIPT_ACTION_5,
    SCRIPT_ACTION_2,
    SCRIPT_ACTION_J,
    SCRIPT_ACTION_6,
    SCRIPT_ACTION_4,
    SCRIPT_ACTION_236,
    SCRIPT_ACTION_623,
    SCRIPT_ACTION_214,
    SCRIPT_ACTION_421,
    SCRIPT_ACTION_624,
    SCRIPT_ACTION_412,
    SCRIPT_ACTION_22,
    SCRIPT_ACTION_41236,
    SCRIPT_ACTION_214236,
    SCRIPT_ACTION_CHARGE_2_8,
    SCRIPT_ACTION_2_HOLD_8,
    SCRIPT_ACTION_CHARGE_4_6,
    SCRIPT_ACTION_4_HOLD_6,
    SCRIPT_ACTION_JUMP,
    SCRIPT_ACTION_DASH_FORWARD,
    SCRIPT_ACTION_DASH_BACKWARD,
    // Appended, never inserted: the index is what a saved trigger stores and
    // what character_moves.h refers to.
    SCRIPT_ACTION_63214,
    SCRIPT_ACTION_236236,
    SCRIPT_ACTION_214214,
    SCRIPT_ACTION_632146,
    SCRIPT_ACTION_360,
    SCRIPT_ACTION_2369,
    SCRIPT_ACTION_21416,
    SCRIPT_ACTION_66,
    SCRIPT_ACTION_3,
    SCRIPT_ACTION_1,
};

enum ScriptActionButton {
    SCRIPT_BUTTON_A = 0,
    SCRIPT_BUTTON_B,
    SCRIPT_BUTTON_C,
    SCRIPT_BUTTON_D,
};

enum WeightClassId {
    WEIGHT_CLASS_VERY_LIGHT = 0,
    WEIGHT_CLASS_LIGHTER,
    WEIGHT_CLASS_LIGHT,
    WEIGHT_CLASS_MEDIUM,
    WEIGHT_CLASS_HEAVY,
};

struct RecoveryConfig {
    int hpMode;
    int hpCustom;
    int meterMode;
    int meterCustom;
    int guardMode;
    int guardCustom;
};

struct TriggerConfig {
    bool enabled;
    int delayFrames;
    int actionKind;
    int actionButton;
};

struct PracticeConfig {
    int activeTab;
    int dummyControlMode;
    int blockMode;
    int stanceMode;
    int jumpMode;
    int jumpCadenceFrames;
    bool comboOverlayEnabled;
    bool recoveryRequireBothNeutral;
    int recoveryDelayFrames;
    RecoveryConfig recovery[kPracticePlayerCount];
    bool triggerMasterEnabled;
    bool triggerRandomize;
    int triggerTarget;
    int wakeBufferFrames;
    TriggerConfig triggers[kTriggerCount];
};

struct PlayerValueEditor {
    bool seeded;
    bool dirty;
    uint32_t seededCharId;
    int hp;
    int meter;
    int guardGauge;
    int x;
    int y;
};

struct PositionSnapshot {
    bool valid;
    int16_t x[kPracticePlayerCount];
    int16_t y[kPracticePlayerCount];
    uint8_t facing[kPracticePlayerCount];
};

struct PositionPreset {
    const char* label;
    int16_t x[kPracticePlayerCount];
    int16_t y[kPracticePlayerCount];
    uint8_t facing[kPracticePlayerCount];
};

// Facing convention: int8_t stored as uint8_t in entity memory.
// 1   = facing right (positive X direction).
// 0xFF = facing left  (-1 as int8_t; used by hitbox viewer as multiplier).
static const PositionPreset kMidScreenPositionPreset = {
    "Mid Screen",
    { kStageCenterX, kStageCenterX },
    { 7599, 7599 },
    { 1, 0xFF },  // P1 faces right, P2 faces left
};

static const PositionPreset kRoundStartPositionPreset = {
    "Round Start",
    { 6600, 9600 },
    { 7599, 7599 },
    { 1, 0xFF },  // P1 faces right, P2 faces left
};

static const PositionPreset kRightCornerPositionPreset = {
    "Right Corner",
    { 15019, 15499 },
    { 7599, 7599 },
    { 1, 0xFF },  // P2 is cornered right; P1 faces right, P2 faces left
};

static const PositionPreset kLeftCornerPositionPreset = {
    "Left Corner",
    { 980, 500 },
    { 7599, 7599 },
    { 0xFF, 1 },  // P2 is cornered left; P1 faces left, P2 faces right
};

struct PlayerSnapshot {
    bool valid;
    uintptr_t base;
    uintptr_t charData;
    uint32_t charId;
    uint16_t hp;
    uint16_t maxHp;
    uint16_t meter;
    uint16_t guardGauge;
    int16_t x;
    int16_t y;
    int16_t xVel;
    int16_t yVel;
    int16_t xAccel;
    int16_t yAccel;
    uint8_t facingRaw;
    bool facingRight;
    uint32_t actionId;
    uint8_t nativeActionable;
    uint8_t attackState;
    uint32_t attackFlags;
    uint8_t hitActive;
    uint8_t blockstun;
    uint8_t hitstunDuration;
    uint16_t knockbackTimer;
    uint16_t comboCount;
    bool airborne;
};

struct TrainingRenderState {
    uint32_t actionId;
    uint32_t mainSprite;
    uint32_t renderGroup;
    uint32_t overlaySprite;
    uint8_t overlayOrder;
    int16_t overlayX;
    int16_t overlayY;
    uint32_t overlayBlend;
    uint8_t overlayAlpha;
    uint8_t flashFlag;
    uint32_t tintState;
    uint32_t tintTimer;
    uint32_t animIndex;
    bool plausible;
};

struct ScriptRuntime {
    bool active;
    bool inputStartedLogged;
    uint32_t triggerFrame;
    int delayFrames;
    int actionKind;
    int actionButton;
    uint16_t lastInput;
};

struct PlayerRuntime {
    PlayerSnapshot prev;
    bool hasPrev;
    bool pendingAirTechCompletion;
    bool pendingGroundTechCompletion;
    bool neutralValid;
    uint32_t neutralSinceFrame;
    int jumpCooldown;
    int jumpHoldFrames;
    uint16_t jumpHoldInput;
    ScriptRuntime script;
};

struct ComboSummary {
    bool valid;
    uint8_t attacker;
    uint8_t defender;
    uint16_t hits;
    int damage;
    int attackerMeterDelta;
    int defenderMeterDelta;
    uint8_t scale[4];
    uint32_t defenderCharId;
    uint16_t defenderMaxHp;
};

struct ComboTracker {
    bool active;
    uint16_t hits;
    int damage;
    int attackerMeterDelta;
    int defenderMeterDelta;
    uint16_t defenderHpAtStart;
    uint16_t attackerMeterAtStart;
    uint16_t defenderMeterAtStart;
    uint8_t scale[4];
    ComboSummary last;
};

static PracticeConfig s_practiceConfig = {};
static PlayerValueEditor s_valueEditors[kPracticePlayerCount] = {};
static PositionSnapshot s_positionSnapshot = {};
static PlayerRuntime s_playerRuntime[kPracticePlayerCount] = {};
static ComboTracker s_comboTrackers[kPracticePlayerCount] = {};
static bool s_ownedOverrideActive[kPracticePlayerCount] = {};
static uint16_t s_lastOwnedOverrideInput[kPracticePlayerCount] = {};
static uint32_t s_lastObservedSimFrame = kInvalidFrame;
static uint32_t s_lastNeutralResetFrame[kPracticePlayerCount] = { kInvalidFrame, kInvalidFrame };

// Trigger status overlay tracking — which trigger last fired and on what frame
static int      s_lastFiredTriggerId    = -1;
static uint32_t s_lastFiredTriggerFrame = 0;

static const uint32_t kMatchHeaderEndRouteOffset = 10;
static const uint8_t kMatchRouteCharSel = 1;
static const uint8_t kMatchRouteMenu = 2;

static const char* kPracticeTabLabels[] = {
    "Overview",
    "Opponent",
    "Values",
    "Options",
    "Triggers",
    "Macros",
    "Hotkeys",
};

static const char* kDummyControlModeLabels[] = {
    "Mod",
    "Vanilla",
};

static const char* kNativeHealthLabels[] = {
    "Off",
    "10%",
    "20%",
    "30%",
    "40%",
    "50%",
    "60%",
    "70%",
    "80%",
    "90%",
    "100%",
};

static const char* kNativeMeterLabels[] = {
    "Off",
    "1 Bar",
    "2 Bars",
    "3 Bars",
    "4 Bars",
    "5 Bars",
    "6 Bars",
    "7 Bars",
    "8 Bars",
    "9 Bars",
};

static const char* kNativeCpuLabels[] = {
    "Off",
    "On",
};

static const char* kNativeAirTechLabels[] = {
    "Off",
    "Up",
    "Forward",
    "Neutral",
    "Back",
};

static const char* kNativeGroundTechLabels[] = {
    "Off",
    "Forward",
    "Neutral",
    "Back",
};

static const char* kNativeBlockTypeLabels[] = {
    "Off",
    "Normal",
    "1 Hit",
};

static const char* kNativeDummyStateLabels[] = {
    "Off",
    "Stand",
    "Crouch",
    "Jump",
};

static const char* kBlockModeLabels[] = {
    "None",
    "All",
    "First Hit",
    "After First Hit",
    "Random",
    "Adaptive",
};

static const char* kStanceModeLabels[] = {
    "Neutral",
    "Stand",
    "Crouch",
    "Jump",
};

static const char* kJumpModeLabels[] = {
    "Disabled",
    "Neutral",
    "Forward",
    "Backward",
    "Random",
};

static const char* kRecoveryHpLabels[] = {
    "Off",
    "Full",
    "Custom",
};

static const char* kRecoveryMeterLabels[] = {
    "Off",
    "0",
    "3000",
    "6000",
    "9000",
    "Custom",
};

static const char* kRecoveryGuardLabels[] = {
    "Off",
    "Full",
    "Custom",
};

static const char* kTriggerTargetLabels[] = {
    "P1",
    "P2",
    "Both",
};

static const char* kTriggerLabels[] = {
    "After Block",
    "On Wakeup",
    "After Hitstun",
    "After Airtech",
    "After Ground Tech",
};

static const char* kScriptActionLabels[] = {
    "None",
    "5X",
    "2X",
    "jX",
    "6X",
    "4X",
    "236X",
    "623X",
    "214X",
    "421X",
    "624X",
    "412X",
    "22X",
    "41236X",
    "214236X",
    "[2]8X",
    "2[8]X",
    "[4]6X",
    "4[6]X",
    "Jump",
    "Dash Forward",
    "Dash Back",
    "63214X",
    "236236X",
    "214214X",
    "632146X",
    "360X",
    "2369X",
    "21416X",
    "66X",
    "3X",
    "1X",
};

// Numpad notation, one frame per digit, with the button pressed on the last.
// Directions are facing-relative, so 6 is toward the opponent. nullptr means the
// kind is not a plain motion (charges hold, and jX is the button alone because
// it relies on the dummy already being airborne).
//
// These are transcribed from the game's own command table at 0x723480, dumped
// out of as2.exe - not guessed. Input_UpdateCommandStates walks 5 bytes per
// step: required direction (exact match of the held mask), required bits,
// forbidden bits, required button, and the frames allowed to reach the next
// step. Directions are matched against what is held *this* frame and the
// machine advances at most one step per frame, so one direction per frame is
// exactly what it wants. The attack button is not part of the steps at all - it
// is a separate gate checked against a 6-frame buffered press when the
// direction sequence completes, which is why pressing it on the final frame
// works.
//
// A step that does not match simply does not advance, so extra diagonals are
// harmless; only the listed directions have to arrive in order and in time.
static const char* const kScriptMotionNumpad[] = {
    nullptr,      // None
    "5",          // 5X   plain normals are not commands: direction + button
    "2",          // 2X
    nullptr,      // jX
    "6",          // 6X
    "4",          // 4X
    "236",        // cmd 19-22
    "623",        // cmd 27-29  (6, 2, then anything holding forward)
    "214",        // cmd 23-26
    "421",        // cmd 30-32
    "632",        // cmd 17     (there is no 624 command in the table)
    "412",        // cmd 18
    "252",        // cmd 13-16  22X is down, NEUTRAL, down - not down twice
    "41236",      // cmd 69-71
    "214236",
    nullptr,      // [2]8X
    nullptr,      // 2[8]X
    nullptr,      // [4]6X
    nullptr,      // 4[6]X
    nullptr,      // Jump
    nullptr,      // Dash Forward
    nullptr,      // Dash Back
    "63214",      // cmd 72-74
    "236236",     // cmd 90
    "214214",     // cmd 91
    "632146",     // cmd 92
    "6248",       // cmd 46 - the four cardinals in order, which is the 360
    "2369",       // cmd 42-44
    "21416",      // cmd 89
    "656",        // cmd 2-4  the dash attack: forward, neutral, forward + button
    "3",          // 3X
    "1",          // 1X
};

static_assert(sizeof(kScriptMotionNumpad) == sizeof(kScriptActionLabels),
              "motion labels and their numpad sequences must stay in step");

static const char* kScriptButtonLabels[] = {
    "A",
    "B",
    "C",
    "D",
};

static const char* kCharacterNames[kPracticeRosterCount] = {
    "Rance",
    "Hatsune",
    "Patton",
    "Seed",
    "Raysen",
    "Aria",
    "Maria",
    "Shizuka",
    "Fanel",
    "Miki",
    "Menad",
    "Hanny King",
    "Satsu",
    "Tiger Joe",
    "Escalayer",
    "Makutsudo",
    "Alietta",
    "Nalzgis",
    "Demon Rance",
    "Little Princess",
    "TADA",
    "Nalzgis (Boss)",
};


static const char* kWeightClassLabels[] = {
    "Very Light",
    "Lighter",
    "Light",
    "Medium",
    "Heavy",
};

static int ClampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static uint16_t ClampU16(int value, int maxValue) {
    return (uint16_t)ClampInt(value, 0, maxValue);
}

static int16_t ClampS16(int value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

static uint32_t Hash32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

static bool DeterministicCoinFlip(uint32_t simFrame, uint32_t salt) {
    return (Hash32(simFrame ^ salt) & 1u) != 0;
}

static const char* SideLabel(int player) {
    return player == 0 ? "P1" : "P2";
}

static const char* GetTriggerLabel(int triggerId) {
    if (triggerId >= 0 && triggerId < kTriggerCount) {
        return kTriggerLabels[triggerId];
    }
    return "Unknown Trigger";
}

static const char* GetActionLabel(int actionKind) {
    if (actionKind >= 0 && actionKind < (int)IM_ARRAYSIZE(kScriptActionLabels)) {
        return kScriptActionLabels[actionKind];
    }
    return "Unknown Action";
}

static uint16_t ButtonMaskFromConfig(int actionButton) {
    switch (actionButton) {
        case SCRIPT_BUTTON_A: return INPUT_A;
        case SCRIPT_BUTTON_B: return INPUT_B;
        case SCRIPT_BUTTON_C: return INPUT_C;
        case SCRIPT_BUTTON_D: return INPUT_D;
        default: return INPUT_A;
    }
}

static const char* GetCharacterName(uint32_t charId) {
    if (charId < kPracticeRosterCount) {
        return kCharacterNames[charId];
    }
    return "Unknown";
}


static uint16_t GetCharacterMaxHp(uint32_t charId) {
    if (charId < kPracticeRosterCount) {
        return ReadMemory<uint16_t>(ADDR_CHARACTER_MAX_HP_TABLE + charId * sizeof(uint16_t));
    }
    return 10000;
}

static uint8_t GetCharacterWeightValue(uint32_t charId) {
    if (charId < kPracticeRosterCount) {
        return ReadMemory<uint8_t>(ADDR_CHARACTER_WEIGHT_TABLE + charId);
    }
    return 100;
}

static uint8_t GetCharacterWeightClass(uint32_t charId) {
    switch (GetCharacterWeightValue(charId)) {
        case 95:
            return WEIGHT_CLASS_VERY_LIGHT;
        case 98:
            return WEIGHT_CLASS_LIGHTER;
        case 100:
            return WEIGHT_CLASS_LIGHT;
        case 105:
            return WEIGHT_CLASS_MEDIUM;
        case 110:
            return WEIGHT_CLASS_HEAVY;
        default:
            return WEIGHT_CLASS_LIGHT;
    }
}

static const char* GetWeightClassLabel(uint32_t charId) {
    return kWeightClassLabels[GetCharacterWeightClass(charId)];
}

static uint32_t ReadEntityCharacterId(uintptr_t entityBase) {
    if (!entityBase) {
        return 0;
    }

    const uintptr_t charData = ReadMemory<uintptr_t>(entityBase);
    if (!charData) {
        return 0;
    }

    const uint32_t charId = ReadMemory<uint32_t>(charData + 176);
    return (charId < kPracticeRosterCount) ? charId : 0;
}

static Training::ActionStateSample ToActionStateSample(const PlayerSnapshot& snapshot) {
    Training::ActionStateSample out{};
    out.actionId = snapshot.actionId;
    out.nativeActionableCandidate = snapshot.nativeActionable;
    out.attackState = snapshot.attackState;
    out.hitActive = snapshot.hitActive;
    out.blockstun = snapshot.blockstun;
    out.hitstunDuration = snapshot.hitstunDuration;
    out.knockbackTimer = snapshot.knockbackTimer;
    return out;
}

static bool IsActionableWithContext(const PlayerSnapshot& snapshot,
                                    Training::ActionableContext context) {
    return Training::EvaluateActionability(
        ToActionStateSample(snapshot),
        context,
        false,
        Training::ActionabilitySource::LegacyActionId).actionable;
}

static bool IsActionable(const PlayerSnapshot& snapshot) {
    return IsActionableWithContext(snapshot, Training::ActionableContext::PracticeTrigger);
}

static bool IsBlockstun(uint32_t actionId) {
    return Training::IsBlockstun(actionId);
}

static bool IsHitstun(uint32_t actionId) {
    return Training::IsHitstun(actionId);
}

static bool IsStunned(uint32_t actionId) {
    return Training::IsContactStartState(actionId);
}

static bool IsTechState(uint32_t actionId) {
    return Training::IsTechOrPostTech(actionId);
}

static bool IsAirTechState(uint32_t actionId) {
    return Training::IsAirTech(actionId);
}

static bool IsGroundTechState(uint32_t actionId) {
    return Training::IsGroundTech(actionId);
}

static bool IsPostTechState(uint32_t actionId) {
    return Training::IsPostTech(actionId);
}

static bool IsWakeupNoTechState(uint32_t actionId) {
    return Training::IsWakeupNoTech(actionId);
}

// The engine's own airborne byte, not a Y comparison. Ground level is 7599 in
// world units - the position presets say so - so "y <= 0" was false for every
// grounded fighter, which is why the crouch stance and the jump loop both did
// nothing at all under mod control: their only gate never opened.
static bool IsGroundedAction(const PlayerSnapshot& snapshot) {
    return !snapshot.airborne && !IsAirTechState(snapshot.actionId);
}

static uint16_t ReadComboCount(uintptr_t entityBase) {
    if (!entityBase) {
        return 0;
    }

    // The live combo count is the per-entity byte at +0xD0.
    return ReadMemory<uint8_t>(entityBase + ENTITY_OFF_DISPLAY_COMBO_COUNT);
}

enum P1DirectionBindingIndex {
    P1_DIRECTION_BIND_UP = 0,
    P1_DIRECTION_BIND_DOWN,
    P1_DIRECTION_BIND_LEFT,
    P1_DIRECTION_BIND_RIGHT,
};

static bool BindingHasAnyComponent(const KeyBinding_t* binding) {
    return binding &&
           (binding->keyboard_key > 0 ||
            binding->gamepad_button >= 0 ||
            binding->gamepad_axis >= 0);
}

static void RemoveOverlappingBindingComponents(KeyBinding_t* target,
                                               const KeyBinding_t* blocker) {
    if (!target || !blocker) {
        return;
    }

    if (target->keyboard_key > 0 && target->keyboard_key == blocker->keyboard_key) {
        target->keyboard_key = 0;
    }
    if (target->gamepad_button >= 0 && target->gamepad_button == blocker->gamepad_button) {
        target->gamepad_button = -1;
    }
    if (target->gamepad_axis >= 0 &&
        target->gamepad_axis == blocker->gamepad_axis &&
        target->axis_direction == blocker->axis_direction) {
        target->gamepad_axis = -1;
        target->axis_direction = 0;
    }
}

static const KeyBinding_t* GetP1DirectionBinding(int bindingIndex) {
    const PlayerBindings_t* p1Bindings = InputSystem_GetBindings(0);
    return p1Bindings ? InputSystem_GetBindingByIndexConst(p1Bindings, bindingIndex) : nullptr;
}

static bool IsFilteredBindingDown(const KeyBinding_t* binding,
                                  const KeyBinding_t* blocker) {
    if (!BindingHasAnyComponent(binding)) {
        return false;
    }

    KeyBinding_t filtered = *binding;
    RemoveOverlappingBindingComponents(&filtered, blocker);
    return BindingHasAnyComponent(&filtered) && InputSystem_IsBindingDown(0, &filtered);
}

static int GetEditableHpCap(const PlayerSnapshot& snapshot) {
    const int tableMax = (int)snapshot.maxHp;
    const int currentHp = (int)snapshot.hp;
    return tableMax > currentHp ? tableMax : currentHp;
}

static uint8_t ReadComboScale(uintptr_t entityBase, int index) {
    if (!entityBase) {
        return 0;
    }

    switch (index) {
        case 0: return ReadMemory<uint8_t>(entityBase + ENTITY_OFF_COMBO_SCALE1);
        case 1: return ReadMemory<uint8_t>(entityBase + ENTITY_OFF_COMBO_SCALE2);
        case 2: return ReadMemory<uint8_t>(entityBase + ENTITY_OFF_COMBO_SCALE3);
        case 3: return ReadMemory<uint8_t>(entityBase + ENTITY_OFF_COMBO_SCALE4);
        default: return 0;
    }
}

static PlayerSnapshot ReadPlayerSnapshot(int player) {
    PlayerSnapshot snapshot = {};
    snapshot.base = GetEntityBase(player);
    if (!snapshot.base) {
        return snapshot;
    }

    snapshot.valid = true;
    snapshot.charData = ReadMemory<uintptr_t>(snapshot.base);
    snapshot.charId = ReadEntityCharacterId(snapshot.base);
    snapshot.maxHp = GetCharacterMaxHp(snapshot.charId);
    snapshot.hp = ReadMemory<uint16_t>(snapshot.base + ENTITY_OFF_HP);
    snapshot.meter = ReadMemory<uint16_t>(snapshot.base + ENTITY_OFF_METER);
    snapshot.guardGauge = ReadMemory<uint16_t>(snapshot.base + ENTITY_OFF_GUARD_GAUGE);
    snapshot.x = ReadMemory<int16_t>(snapshot.base + ENTITY_OFF_X_POS);
    snapshot.y = ReadMemory<int16_t>(snapshot.base + ENTITY_OFF_Y_POS);
    snapshot.xVel = ReadMemory<int16_t>(snapshot.base + ENTITY_OFF_X_VEL);
    snapshot.yVel = ReadMemory<int16_t>(snapshot.base + ENTITY_OFF_Y_VEL);
    snapshot.xAccel = ReadMemory<int16_t>(snapshot.base + ENTITY_OFF_X_ACCEL);
    snapshot.yAccel = ReadMemory<int16_t>(snapshot.base + ENTITY_OFF_Y_ACCEL);
    snapshot.facingRaw = ReadMemory<uint8_t>(snapshot.base + ENTITY_OFF_FACING);
    snapshot.facingRight = (int8_t)snapshot.facingRaw > 0;  // 1=right, -1/0xFF=left
    snapshot.actionId = ReadMemory<uint32_t>(snapshot.base + ENTITY_OFF_ACTION_ID);
    // Route 2 (the shared stand/air A branch), retained for the audit log.
    snapshot.nativeActionable =
        ReadMemory<uint8_t>(snapshot.base + ENTITY_OFF_COMMAND_ROUTE_TIMERS + ENTITY_ROUTE_A_STAND_OR_AIR);
    snapshot.attackState = ReadMemory<uint8_t>(snapshot.base + ENTITY_OFF_ATTACK_STATE);
    snapshot.attackFlags = ReadMemory<uint32_t>(snapshot.base + ENTITY_OFF_ATTACK_TYPE);
    snapshot.hitActive = ReadMemory<uint8_t>(snapshot.base + ENTITY_OFF_HIT_ACTIVE);
    snapshot.blockstun = ReadMemory<uint8_t>(snapshot.base + ENTITY_OFF_BLOCKSTUN);
    snapshot.hitstunDuration = ReadMemory<uint8_t>(snapshot.base + ENTITY_OFF_HITSTUN_DURATION);
    snapshot.knockbackTimer = ReadMemory<uint16_t>(snapshot.base + ENTITY_OFF_KNOCKBACK_TIMER);
    snapshot.comboCount = ReadComboCount(snapshot.base);
    snapshot.airborne = ReadMemory<uint8_t>(snapshot.base + ENTITY_OFF_AIRBORNE) == 1;
    return snapshot;
}

static void ReleaseOwnedOverride(int player) {
    if (player < 0 || player >= kPracticePlayerCount) {
        return;
    }

    if (!s_ownedOverrideActive[player]) {
        return;
    }

    LOG_INFO("[Practice] Release override %s input=0x%04X",
             SideLabel(player),
             (unsigned int)s_lastOwnedOverrideInput[player]);
    InputSystem_ClearOverride(player);
    s_ownedOverrideActive[player] = false;
    s_lastOwnedOverrideInput[player] = 0;
}

static void SetOwnedOverride(int player, uint16_t input) {
    if (player < 0 || player >= kPracticePlayerCount) {
        return;
    }

    if (!s_ownedOverrideActive[player] || s_lastOwnedOverrideInput[player] != input) {
        LOG_INFO("[Practice] Override %s input=0x%04X",
                 SideLabel(player),
                 (unsigned int)input);
    }
    InputSystem_SetOverride(player, input);
    s_ownedOverrideActive[player] = true;
    s_lastOwnedOverrideInput[player] = input;
}

static void ClearComboTrackers(void) {
    memset(s_comboTrackers, 0, sizeof(s_comboTrackers));
}

static void ClearPlayerRuntime(void) {
    memset(s_playerRuntime, 0, sizeof(s_playerRuntime));
}

static void ClearValueEditors(void) {
    memset(s_valueEditors, 0, sizeof(s_valueEditors));
}

static void ResetPracticeTransientRuntime(bool clearEditors) {
    ReleaseOwnedOverride(0);
    ReleaseOwnedOverride(1);
    PracticeDefense_Reset();
    ClearPlayerRuntime();
    ClearComboTrackers();
    s_lastObservedSimFrame = kInvalidFrame;
    s_lastFiredTriggerId = -1;
    s_lastFiredTriggerFrame = 0;
    if (clearEditors) {
        ClearValueEditors();
        s_positionSnapshot.valid = false;
    }
}

static void ClearPracticeRuntimeAfterPositionReset(const char* reason) {
    ResetPracticeTransientRuntime(false);
    Rollback::NetplayLog_Write(
        "TRAINRESET", -1,
        "runtime_cleared reason=%s frame=%u",
        reason ? reason : "position_reset",
        ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
    LOG_INFO("[TRAINRESET] runtime_cleared reason=%s",
             reason ? reason : "position_reset");
}

static bool TriggerTargetIncludesPlayer(int target, int player) {
    if (target == TRIGGER_TARGET_BOTH) {
        return true;
    }
    return target == (player == 0 ? TRIGGER_TARGET_P1 : TRIGGER_TARGET_P2);
}

static int ComputeHpTierIndex(uint16_t hp, uint16_t maxHp) {
    if (maxHp == 0) {
        return 0;
    }

    if (hp >= maxHp) {
        return 0;
    }

    const int bucket = (int)((maxHp - hp) / (maxHp / 10 ? (maxHp / 10) : 1));
    return ClampInt(bucket, 0, 10);
}

static void ZeroPlayerMotion(uintptr_t entityBase) {
    if (!entityBase) {
        return;
    }

    WriteMemory<int16_t>(entityBase + ENTITY_OFF_X_VEL, 0);
    WriteMemory<int16_t>(entityBase + ENTITY_OFF_Y_VEL, 0);
    WriteMemory<int16_t>(entityBase + ENTITY_OFF_X_ACCEL, 0);
    WriteMemory<int16_t>(entityBase + ENTITY_OFF_Y_ACCEL, 0);
}

using EntitySetActionResetFn = int (__cdecl *)(int actionId, void* entity);
using EntityResetHitDataFn = int (__cdecl *)(int entity);
using EffectSetParamsFn = int (__cdecl *)(int entity, int sprite, int renderGroup, uint16_t animIndex);

static bool IsPlausibleMainSprite(uint32_t sprite) {
    return sprite < 1024;
}

static bool IsPlausibleOverlaySprite(uint32_t sprite) {
    return sprite == 0xFFFFFFFFu || sprite < 1024;
}

static bool IsPlausibleAnimIndex(uint32_t animIndex) {
    return animIndex < 4096;
}

static TrainingRenderState ReadTrainingRenderState(uintptr_t entityBase) {
    TrainingRenderState state{};
    if (!entityBase) {
        return state;
    }

    state.actionId = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_ACTION_ID);
    state.mainSprite = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_MAIN_SPRITE);
    state.renderGroup = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_GROUP);
    state.overlaySprite = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_OVERLAY_SPRITE);
    state.overlayOrder = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_RENDER_OVERLAY_ORDER);
    state.overlayX = ReadMemory<int16_t>(entityBase + ENTITY_OFF_RENDER_OVERLAY_X);
    state.overlayY = ReadMemory<int16_t>(entityBase + ENTITY_OFF_RENDER_OVERLAY_Y);
    state.overlayBlend = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_OVERLAY_BLEND);
    state.overlayAlpha = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_RENDER_OVERLAY_ALPHA);
    state.flashFlag = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_RENDER_FLASH_FLAG);
    state.tintState = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_TINT_STATE);
    state.tintTimer = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_TINT_TIMER);
    state.animIndex = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_ANIM_INDEX);
    state.plausible =
        IsPlausibleMainSprite(state.mainSprite) &&
        IsPlausibleOverlaySprite(state.overlaySprite) &&
        IsPlausibleAnimIndex(state.animIndex);
    return state;
}

static bool CallEntitySetActionReset(uintptr_t entityBase, int actionId) {
    __try {
        reinterpret_cast<EntitySetActionResetFn>(ADDR_ENTITY_SET_ACTION_RESET)(
            actionId,
            reinterpret_cast<void*>(entityBase));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallEntityResetHitData(uintptr_t entityBase) {
    __try {
        reinterpret_cast<EntityResetHitDataFn>(ADDR_ENTITY_RESET_HIT_DATA)(
            static_cast<int>(entityBase));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallEffectSetParams(uintptr_t entityBase,
                                int sprite,
                                int renderGroup,
                                uint16_t animIndex) {
    __try {
        reinterpret_cast<EffectSetParamsFn>(ADDR_EFFECT_SET_PARAMS)(
            static_cast<int>(entityBase),
            sprite,
            renderGroup,
            animIndex);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LogTrainingRenderState(const char* prefix,
                                   int player,
                                   uintptr_t entityBase,
                                   const TrainingRenderState& state) {
    Rollback::NetplayLog_Write(
        "TRAINRESET", -1,
        "%s p=%s entity=0x%08X action=%u main=%u group=%u overlay=0x%08X "
        "order=%u overlay_xy=(%d,%d) blend=0x%08X alpha=%u flash=%u tint=%u/%u "
        "anim=%u plausible=%d",
        prefix ? prefix : "state",
        SideLabel(player),
        (unsigned)entityBase,
        state.actionId,
        state.mainSprite,
        state.renderGroup,
        state.overlaySprite,
        state.overlayOrder,
        (int)state.overlayX,
        (int)state.overlayY,
        state.overlayBlend,
        state.overlayAlpha,
        state.flashFlag,
        state.tintState,
        state.tintTimer,
        state.animIndex,
        state.plausible ? 1 : 0);
}

static bool NormalizeTrainingRenderFields(uintptr_t entityBase) {
    const bool effectParamsSet = CallEffectSetParams(
        entityBase,
        kTrainingNeutralSprite,
        kTrainingNeutralRenderGroup,
        kTrainingNeutralAnimIndex);

    // sub_4C3ED0 intentionally preserves +0x081C when renderGroup == 3, so
    // write the known standing group explicitly for position-reset recovery.
    const bool groupSet =
        WriteMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_GROUP, kTrainingNeutralRenderGroup);
    const bool flashCleared =
        WriteMemory<uint8_t>(entityBase + ENTITY_OFF_RENDER_FLASH_FLAG, 0);
    const bool tintSet =
        WriteMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_TINT_STATE, 1);
    const bool tintTimerCleared =
        WriteMemory<uint32_t>(entityBase + ENTITY_OFF_RENDER_TINT_TIMER, 0);
    return effectParamsSet && groupSet && flashCleared && tintSet && tintTimerCleared;
}

static void ResetPlayerToTrainingNeutral(int player, uintptr_t entityBase, const char* reason) {
    if (!entityBase) {
        return;
    }

    const uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    if (player >= 0 && player < kPracticePlayerCount &&
        s_lastNeutralResetFrame[player] == simFrame) {
        Rollback::NetplayLog_Write(
            "TRAINRESET", -1,
            "coalesced p=%s frame=%u reason=%s entity=0x%08X",
            SideLabel(player),
            simFrame,
            reason ? reason : "unknown",
            (unsigned)entityBase);
        return;
    }
    if (player >= 0 && player < kPracticePlayerCount) {
        s_lastNeutralResetFrame[player] = simFrame;
    }

    const TrainingRenderState before = ReadTrainingRenderState(entityBase);
    if (!before.plausible) {
        LOG_WARN("[TRAINRESET] pre_bad_render_state p=%s entity=0x%08X main=0x%08X overlay=0x%08X anim=0x%08X",
                 SideLabel(player),
                 (unsigned)entityBase,
                 before.mainSprite,
                 before.overlaySprite,
                 before.animIndex);
    }
    LogTrainingRenderState("begin", player, entityBase, before);

    const bool actionReset = CallEntitySetActionReset(entityBase, kTrainingNeutralAction);
    const bool hitReset = CallEntityResetHitData(entityBase);
    const bool renderNorm = NormalizeTrainingRenderFields(entityBase);

    WriteMemory<uint8_t>(entityBase + ENTITY_OFF_ATTACK_STATE, 0);
    WriteMemory<uint32_t>(entityBase + ENTITY_OFF_ATTACK_TYPE, 0);
    WriteMemory<uint8_t>(entityBase + ENTITY_OFF_HIT_ACTIVE, 0);
    ZeroPlayerMotion(entityBase);

    const TrainingRenderState after = ReadTrainingRenderState(entityBase);
    Rollback::NetplayLog_Write(
        "TRAINRESET", -1,
        "vanilla_reset p=%s frame=%u reason=%s entity=0x%08X action=%d action_reset=%d hit_reset=%d render_norm=%d",
        SideLabel(player),
        simFrame,
        reason ? reason : "unknown",
        (unsigned)entityBase,
        kTrainingNeutralAction,
        actionReset ? 1 : 0,
        hitReset ? 1 : 0,
        renderNorm ? 1 : 0);
    LogTrainingRenderState("end", player, entityBase, after);

    LOG_INFO("[TRAINRESET] neutral_reset reason=%s p=%s entity=0x%08X action=%u->%u "
             "main=%u->%u overlay=0x%08X->0x%08X anim=%u->%u plausible=%d",
             reason ? reason : "unknown",
             SideLabel(player),
             (unsigned)entityBase,
             before.actionId,
             after.actionId,
             before.mainSprite,
             after.mainSprite,
             before.overlaySprite,
             after.overlaySprite,
             before.animIndex,
             after.animIndex,
             after.plausible ? 1 : 0);

    if (!actionReset || !hitReset || !renderNorm || !after.plausible) {
        Rollback::NetplayLog_Write(
            "TRAINRESET", -1,
            "ERROR neutral_reset incomplete p=%s action_reset=%d hit_reset=%d render_norm=%d plausible=%d",
            SideLabel(player),
            actionReset ? 1 : 0,
            hitReset ? 1 : 0,
            renderNorm ? 1 : 0,
            after.plausible ? 1 : 0);
    }
}

static void WritePlayerHpFields(uintptr_t entityBase, int hp) {
    if (!entityBase) {
        return;
    }

    const uint16_t clampedHp = ClampU16(hp, 0xFFFF);
    WriteMemory<uint16_t>(entityBase + ENTITY_OFF_HP, clampedHp);
    WriteMemory<uint16_t>(entityBase + ENTITY_OFF_HP_DISPLAY_PREVIOUS, clampedHp);
    WriteMemory<uint16_t>(entityBase + ENTITY_OFF_HP_DISPLAY, clampedHp);
}

static bool CanUsePositionTools(void);

static void WritePlayerValues(int player, const PlayerValueEditor& editor, const char* reason) {
    PlayerSnapshot before = ReadPlayerSnapshot(player);
    if (!before.valid) {
        return;
    }

    const int hpCap = GetEditableHpCap(before);
    const bool canApplyPosition = CanUsePositionTools();
    const int16_t appliedX = canApplyPosition ? ClampS16(editor.x) : before.x;
    const int16_t appliedY = canApplyPosition ? ClampS16(editor.y) : before.y;
    const bool positionChanged = canApplyPosition &&
        (appliedX != before.x || appliedY != before.y);
    const uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    const TrainingRenderState renderBefore = ReadTrainingRenderState(before.base);

    WritePlayerHpFields(before.base, ClampInt(editor.hp, 0, hpCap));
    WriteMemory<uint16_t>(before.base + ENTITY_OFF_METER, ClampU16(editor.meter, kMeterMax));
    WriteMemory<uint16_t>(before.base + ENTITY_OFF_GUARD_GAUGE, ClampU16(editor.guardGauge, kGuardGaugeMax));
    if (canApplyPosition) {
        WriteMemory<int16_t>(before.base + ENTITY_OFF_X_POS, appliedX);
        WriteMemory<int16_t>(before.base + ENTITY_OFF_Y_POS, appliedY);
        ZeroPlayerMotion(before.base);
        if (positionChanged) {
            ResetPlayerToTrainingNeutral(player, before.base, reason ? reason : "value_position");
        }
    }
    const TrainingRenderState renderAfter = ReadTrainingRenderState(before.base);

    LOG_INFO("[Practice] %s value apply (%s): HP %u->%u Meter %u->%u Guard %u->%u Pos (%d,%d)->(%d,%d)",
             SideLabel(player),
             reason ? reason : "apply",
             before.hp,
             (unsigned int)ClampU16(editor.hp, hpCap),
             before.meter,
             (unsigned int)ClampU16(editor.meter, kMeterMax),
             before.guardGauge,
             (unsigned int)ClampU16(editor.guardGauge, kGuardGaugeMax),
             before.x,
             before.y,
             (int)appliedX,
             (int)appliedY);

    if (positionChanged) {
        Rollback::NetplayLog_Write(
            "TRAINRESET", -1,
            "value_position frame=%u reason=%s p=%s act=%u->%u spr=%u/0x%08X->%u/0x%08X anim=%u->%u",
            simFrame,
            reason ? reason : "value_position",
            SideLabel(player),
            renderBefore.actionId,
            renderAfter.actionId,
            renderBefore.mainSprite,
            renderBefore.overlaySprite,
            renderAfter.mainSprite,
            renderAfter.overlaySprite,
            renderBefore.animIndex,
            renderAfter.animIndex);
        ClearPracticeRuntimeAfterPositionReset(reason ? reason : "value_position");
    }
}

static void CopySnapshotToEditor(int player, const PlayerSnapshot& snapshot) {
    if (player < 0 || player >= kPracticePlayerCount || !snapshot.valid) {
        return;
    }

    PlayerValueEditor& editor = s_valueEditors[player];
    editor.seeded = true;
    editor.dirty = false;
    editor.seededCharId = snapshot.charId;
    editor.hp = snapshot.hp;
    editor.meter = snapshot.meter;
    editor.guardGauge = snapshot.guardGauge;
    editor.x = snapshot.x;
    editor.y = snapshot.y;
}

static void RefreshValueEditorsFromLiveState(void) {
    for (int player = 0; player < kPracticePlayerCount; ++player) {
        CopySnapshotToEditor(player, ReadPlayerSnapshot(player));
    }
}

static bool IsPositionSetBlockedByMatchState(void) {
    if (!PracticeTools_IsPracticeModeActive()) {
        return true;
    }

    // The pause menu is substate 4, and every position tool it offers was
    // silently returning false because of this check. Pausing is only reachable
    // from gameplay and freezes the simulation, so it is a safe context; the
    // intro / transition guards below are what actually protect the write.
    if (GetGameMode() != MODE_MATCH) {
        return true;
    }
    const uint32_t substate = GetSubstate();
    if (substate != MATCH_SUB_GAMEPLAY && substate != MATCH_SUB_PAUSE) {
        return true;
    }

    const uint8_t introLock = GetMatchHeaderByte(MATCH_HEADER_INTRO_LOCK_OFFSET);
    const uint8_t transitionLock = GetMatchHeaderByte(MATCH_HEADER_TRANSITION_OFFSET);
    const uint32_t introFadeTimer = ReadMemory<uint32_t>(ADDR_MATCH_INTRO_FADE_TIMER);

    // Position tools must stay locked until the engine has fully released the
    // opening input lock and must relock as soon as round-end transition starts.
    if (introLock != 0 || transitionLock != 0 || introFadeTimer != 0) {
        return true;
    }

    const PlayerSnapshot p1 = ReadPlayerSnapshot(0);
    const PlayerSnapshot p2 = ReadPlayerSnapshot(1);

    // The first interactive-looking frames can still be in the engine's startup
    // actions 0/1 before character intro state has fully settled.
    if (!p1.valid || !p2.valid) {
        return true;
    }

    return p1.actionId <= 1 || p2.actionId <= 1;
}

static bool CanUsePositionTools(void) {
    return !IsPositionSetBlockedByMatchState();
}

static void SeedValueEditorIfNeeded(int player, const PlayerSnapshot& snapshot) {
    if (player < 0 || player >= kPracticePlayerCount || !snapshot.valid) {
        return;
    }

    PlayerValueEditor& editor = s_valueEditors[player];
    const bool liveMismatch =
        editor.hp != snapshot.hp ||
        editor.meter != snapshot.meter ||
        editor.guardGauge != snapshot.guardGauge ||
        editor.x != snapshot.x ||
        editor.y != snapshot.y;

    if (!editor.seeded ||
        editor.seededCharId != snapshot.charId ||
        (!editor.dirty && liveMismatch)) {
        CopySnapshotToEditor(player, snapshot);
    }
}

static bool SavePositionSnapshot(const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    if (!CanUsePositionTools()) {
        LOG_INFO("[Practice] Position snapshot save skipped: gameplay not interactive yet");
        return false;
    }

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        if (!snapshots[player].valid) {
            LOG_WARN("[Practice] Position snapshot save skipped: %s unavailable", SideLabel(player));
            return false;
        }
    }

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        s_positionSnapshot.x[player] = snapshots[player].x;
        s_positionSnapshot.y[player] = snapshots[player].y;
        s_positionSnapshot.facing[player] = snapshots[player].facingRaw;
    }
    s_positionSnapshot.valid = true;
    LOG_INFO("[Practice] Position snapshot saved: P1=(%d,%d,%u) P2=(%d,%d,%u)",
             s_positionSnapshot.x[0],
             s_positionSnapshot.y[0],
             (unsigned int)s_positionSnapshot.facing[0],
             s_positionSnapshot.x[1],
             s_positionSnapshot.y[1],
             (unsigned int)s_positionSnapshot.facing[1]);
    return true;
}

// Center the camera scroll on the midpoint between two world-space X coordinates.
// scrollX is clamped [0, 959] per Weather_UpdateScroll (sub_4C4230).
// World-to-screen: screenX = worldX/10 - scrollX.
static void UpdateScrollForPositions(int16_t p1x, int16_t p2x) {
    const int32_t midWorldX = ((int32_t)p1x + (int32_t)p2x) / 2;
    int32_t scrollX = midWorldX / 10 - 320;  // center 640-pixel viewport
    if (scrollX < 0)   scrollX = 0;
    if (scrollX > 959) scrollX = 959;
    WriteMemory<int16_t>(ADDR_SCROLL_X, (int16_t)scrollX);
}

static bool ApplyPositionData(const int16_t x[kPracticePlayerCount],
                              const int16_t y[kPracticePlayerCount],
                              const uint8_t facing[kPracticePlayerCount]) {
    if (!CanUsePositionTools()) {
        LOG_INFO("[Practice] Position apply skipped: gameplay not interactive yet");
        return false;
    }

    const uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    uintptr_t bases[kPracticePlayerCount] = {};
    TrainingRenderState before[kPracticePlayerCount] = {};
    for (int player = 0; player < kPracticePlayerCount; ++player) {
        bases[player] = GetEntityBase(player);
        before[player] = ReadTrainingRenderState(bases[player]);
    }
    Rollback::NetplayLog_Write(
        "TRAINRESET", -1,
        "begin frame=%u reason=position_set p1_act=%u p1_spr=%u/0x%08X p1_anim=%u "
        "p2_act=%u p2_spr=%u/0x%08X p2_anim=%u",
        simFrame,
        before[0].actionId,
        before[0].mainSprite,
        before[0].overlaySprite,
        before[0].animIndex,
        before[1].actionId,
        before[1].mainSprite,
        before[1].overlaySprite,
        before[1].animIndex);

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        const uintptr_t entityBase = bases[player];
        if (!entityBase) {
            continue;
        }

        WriteMemory<int16_t>(entityBase + ENTITY_OFF_X_POS, x[player]);
        WriteMemory<int16_t>(entityBase + ENTITY_OFF_Y_POS, y[player]);
        WriteMemory<uint8_t>(entityBase + ENTITY_OFF_FACING, facing[player]);
        ZeroPlayerMotion(entityBase);
        ResetPlayerToTrainingNeutral(player, entityBase, "position_set");
    }

    const TrainingRenderState afterP1 = ReadTrainingRenderState(bases[0]);
    const TrainingRenderState afterP2 = ReadTrainingRenderState(bases[1]);
    Rollback::NetplayLog_Write(
        "TRAINRESET", -1,
        "end frame=%u reason=position_set p1_act=%u p1_spr=%u/0x%08X p1_anim=%u plausible=%d "
        "p2_act=%u p2_spr=%u/0x%08X p2_anim=%u plausible=%d",
        simFrame,
        afterP1.actionId,
        afterP1.mainSprite,
        afterP1.overlaySprite,
        afterP1.animIndex,
        afterP1.plausible ? 1 : 0,
        afterP2.actionId,
        afterP2.mainSprite,
        afterP2.overlaySprite,
        afterP2.animIndex,
        afterP2.plausible ? 1 : 0);

    UpdateScrollForPositions(x[0], x[1]);

    FrameAdvantage_CancelCalculation();
    FrameAdvantage_ClearDisplay();
    RefreshValueEditorsFromLiveState();
    ClearPracticeRuntimeAfterPositionReset("position_set");
    return true;
}

static bool LoadPositionSnapshot(void) {
    if (!s_positionSnapshot.valid) {
        return false;
    }

    LOG_INFO("[Practice] Position snapshot restore: P1=(%d,%d,%u) P2=(%d,%d,%u)",
             s_positionSnapshot.x[0],
             s_positionSnapshot.y[0],
             (unsigned int)s_positionSnapshot.facing[0],
             s_positionSnapshot.x[1],
             s_positionSnapshot.y[1],
             (unsigned int)s_positionSnapshot.facing[1]);
    return ApplyPositionData(s_positionSnapshot.x, s_positionSnapshot.y, s_positionSnapshot.facing);
}

static bool ApplyPositionPreset(const PositionPreset& preset) {
    LOG_INFO("[Practice] Position preset apply: %s", preset.label ? preset.label : "Preset");
    return ApplyPositionData(preset.x, preset.y, preset.facing);
}

static const PositionPreset* GetLoadPositionPresetFromHotkey(void) {
    if (!CanUsePositionTools()) {
        return nullptr;
    }

    const KeyBinding_t* loadBinding = HotkeyConfig_GetBinding(HOTKEY_POSITION_LOAD);
    if (IsFilteredBindingDown(GetP1DirectionBinding(P1_DIRECTION_BIND_UP), loadBinding)) {
        return &kRoundStartPositionPreset;
    }
    if (IsFilteredBindingDown(GetP1DirectionBinding(P1_DIRECTION_BIND_DOWN), loadBinding)) {
        return &kMidScreenPositionPreset;
    }
    if (IsFilteredBindingDown(GetP1DirectionBinding(P1_DIRECTION_BIND_RIGHT), loadBinding)) {
        return &kRightCornerPositionPreset;
    }
    if (IsFilteredBindingDown(GetP1DirectionBinding(P1_DIRECTION_BIND_LEFT), loadBinding)) {
        return &kLeftCornerPositionPreset;
    }

    return nullptr;
}

static void SwapPlayerPositions(const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    if (!CanUsePositionTools()) {
        LOG_INFO("[Practice] Position swap skipped: gameplay not interactive yet");
        return;
    }

    if (!snapshots[0].valid || !snapshots[1].valid) {
        return;
    }

    const uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    const TrainingRenderState beforeP1 = ReadTrainingRenderState(snapshots[0].base);
    const TrainingRenderState beforeP2 = ReadTrainingRenderState(snapshots[1].base);
    Rollback::NetplayLog_Write(
        "TRAINRESET", -1,
        "begin frame=%u reason=position_swap p1_act=%u p1_spr=%u/0x%08X p1_anim=%u "
        "p2_act=%u p2_spr=%u/0x%08X p2_anim=%u",
        simFrame,
        beforeP1.actionId,
        beforeP1.mainSprite,
        beforeP1.overlaySprite,
        beforeP1.animIndex,
        beforeP2.actionId,
        beforeP2.mainSprite,
        beforeP2.overlaySprite,
        beforeP2.animIndex);

    LOG_INFO("[Practice] Swap positions: P1 (%d,%d) <-> P2 (%d,%d)",
             snapshots[0].x,
             snapshots[0].y,
             snapshots[1].x,
             snapshots[1].y);
    WriteMemory<int16_t>(snapshots[0].base + ENTITY_OFF_X_POS, snapshots[1].x);
    WriteMemory<int16_t>(snapshots[1].base + ENTITY_OFF_X_POS, snapshots[0].x);
    WriteMemory<int16_t>(snapshots[0].base + ENTITY_OFF_Y_POS, snapshots[0].y);
    WriteMemory<int16_t>(snapshots[1].base + ENTITY_OFF_Y_POS, snapshots[1].y);
    ZeroPlayerMotion(snapshots[0].base);
    ZeroPlayerMotion(snapshots[1].base);
    ResetPlayerToTrainingNeutral(0, snapshots[0].base, "position_swap");
    ResetPlayerToTrainingNeutral(1, snapshots[1].base, "position_swap");
    // Swap preserves the midpoint, so the same scroll center is still correct.
    UpdateScrollForPositions(snapshots[0].x, snapshots[1].x);
    RefreshValueEditorsFromLiveState();
    const TrainingRenderState afterP1 = ReadTrainingRenderState(snapshots[0].base);
    const TrainingRenderState afterP2 = ReadTrainingRenderState(snapshots[1].base);
    Rollback::NetplayLog_Write(
        "TRAINRESET", -1,
        "end frame=%u reason=position_swap p1_act=%u p1_spr=%u/0x%08X p1_anim=%u plausible=%d "
        "p2_act=%u p2_spr=%u/0x%08X p2_anim=%u plausible=%d",
        simFrame,
        afterP1.actionId,
        afterP1.mainSprite,
        afterP1.overlaySprite,
        afterP1.animIndex,
        afterP1.plausible ? 1 : 0,
        afterP2.actionId,
        afterP2.mainSprite,
        afterP2.overlaySprite,
        afterP2.animIndex,
        afterP2.plausible ? 1 : 0);
    ClearPracticeRuntimeAfterPositionReset("position_swap");
}

static uint16_t ForwardMask(const PlayerSnapshot& snapshot) {
    return snapshot.facingRight ? INPUT_RIGHT : INPUT_LEFT;
}

static uint16_t BackMask(const PlayerSnapshot& snapshot) {
    return snapshot.facingRight ? INPUT_LEFT : INPUT_RIGHT;
}

static int ReadNativeTrainingSetting(uintptr_t address, int maxValue) {
    return ClampInt((int)ReadMemory<uint8_t>(address), 0, maxValue);
}

static void WriteNativeTrainingSetting(uintptr_t address,
                                       int value,
                                       int maxValue,
                                       const char* label) {
    const int clampedValue = ClampInt(value, 0, maxValue);
    WriteMemory<uint8_t>(address, (uint8_t)clampedValue);
    LOG_INFO("[Practice] Native training %s=%d",
             label ? label : "setting",
             clampedValue);
}

// Which side the dummy is on. Control swap hands the human P2, which makes P1
// the dummy - so the whole Dummy page, the automation input and the CPU
// ownership have to move with it. They did not: the dummy was hardwired to P2,
// so swapping left the mod driving nobody and the vanilla AI took the side
// over, which is exactly the "P2 control makes P1 act like vanilla" report.
static int DummySide(void) {
    return InputSystem_GetControlSwap() ? 0 : 1;
}

static int HumanSide(void) {
    return 1 - DummySide();
}

static bool HasP2DummyAutomationConfigured(void) {
    if (s_practiceConfig.dummyControlMode != DUMMY_CONTROL_MOD) {
        return false;
    }

    return s_practiceConfig.blockMode != DUMMY_BLOCK_NONE ||
           s_practiceConfig.stanceMode != DUMMY_STANCE_NEUTRAL ||
           s_practiceConfig.jumpMode != DUMMY_JUMP_DISABLED;
}

static void ComputeDesiredCpuFlags(uint8_t* outP1Cpu, uint8_t* outP2Cpu) {
    const int dummy = DummySide();

    // The vanilla AI owns the dummy; the human owns the other side. Which side
    // that is follows the swap rather than being P2 by definition.
    uint8_t cpu[kPracticePlayerCount] = { 0, 0 };
    cpu[dummy] = 1;

    if (InputMacro_GetState() == MACRO_REPLAYING) {
        cpu[0] = 0;
        cpu[1] = 0;
    } else {
        // Mod control means the mod's own injected input drives the dummy, so
        // the AI has to let go of it or the two fight over the same entity.
        if (s_practiceConfig.dummyControlMode == DUMMY_CONTROL_MOD) {
            cpu[dummy] = 0;
        }
        for (int player = 0; player < kPracticePlayerCount; ++player) {
            if (s_playerRuntime[player].script.active) {
                cpu[player] = 0;
            }
        }
    }

    if (outP1Cpu) *outP1Cpu = cpu[0];
    if (outP2Cpu) *outP2Cpu = cpu[1];
}

static void WriteDesiredCpuFlags(void) {
    uint8_t desiredP1Cpu = 0;
    uint8_t desiredP2Cpu = 1;
    ComputeDesiredCpuFlags(&desiredP1Cpu, &desiredP2Cpu);
    WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, desiredP1Cpu);
    WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, desiredP2Cpu);
}

static int GetRecoveryHpTarget(int player, const PlayerSnapshot& snapshot) {
    if (ReadNativeTrainingSetting(ADDR_TRAINING_HEALTH_REGEN_SETTING, 10) != 0) {
        return -1;
    }

    const RecoveryConfig& cfg = s_practiceConfig.recovery[player];
    switch (cfg.hpMode) {
        case RECOVERY_HP_FULL: return snapshot.maxHp;
        case RECOVERY_HP_CUSTOM: return ClampInt(cfg.hpCustom, 0, snapshot.maxHp);
        default: return -1;
    }
}

static int GetRecoveryMeterTarget(int player) {
    if (ReadNativeTrainingSetting(ADDR_TRAINING_METER_LEVEL_SETTING, 9) != 0) {
        return -1;
    }

    const RecoveryConfig& cfg = s_practiceConfig.recovery[player];
    switch (cfg.meterMode) {
        case RECOVERY_METER_ZERO: return 0;
        case RECOVERY_METER_3000: return 3000;
        case RECOVERY_METER_6000: return 6000;
        case RECOVERY_METER_9000: return 9000;
        case RECOVERY_METER_CUSTOM: return ClampInt(cfg.meterCustom, 0, kMeterMax);
        default: return -1;
    }
}

static int GetRecoveryGuardTarget(int player) {
    const RecoveryConfig& cfg = s_practiceConfig.recovery[player];
    switch (cfg.guardMode) {
        case RECOVERY_GUARD_FULL: return kGuardGaugeMax;
        case RECOVERY_GUARD_CUSTOM: return ClampInt(cfg.guardCustom, 0, kGuardGaugeMax);
        default: return -1;
    }
}

// nullptr for anything that is not a plain numpad motion.
static const char* MotionNumpad(int actionKind) {
    if (actionKind <= 0 ||
        actionKind >= (int)(sizeof(kScriptMotionNumpad) / sizeof(kScriptMotionNumpad[0]))) {
        return nullptr;
    }
    return kScriptMotionNumpad[actionKind];
}

static int GetActionSequenceLength(int actionKind) {
    if (const char* numpad = MotionNumpad(actionKind)) {
        return (int)strlen(numpad);
    }
    switch (actionKind) {
        case SCRIPT_ACTION_NONE: return 0;
        case SCRIPT_ACTION_DASH_FORWARD:
        case SCRIPT_ACTION_DASH_BACKWARD:
            return 3;
        case SCRIPT_ACTION_CHARGE_2_8:
        case SCRIPT_ACTION_2_HOLD_8:
        case SCRIPT_ACTION_CHARGE_4_6:
        case SCRIPT_ACTION_4_HOLD_6:
            return kSyntheticChargeFrames + 1;
        default:
            return 1;
    }
}

static uint16_t GetActionSequenceInput(const PlayerSnapshot& snapshot,
                                       int actionKind,
                                       int actionButton,
                                       int frameIndex) {
    const uint16_t buttonMask = ButtonMaskFromConfig(actionButton);
    const uint16_t forward = ForwardMask(snapshot);
    const uint16_t back = BackMask(snapshot);

    // Every plain motion is its numpad string, one digit per frame, so a new
    // command is a table entry rather than another case here.
    if (const char* numpad = MotionNumpad(actionKind)) {
        const int length = (int)strlen(numpad);
        const int index = ClampInt(frameIndex, 0, length - 1);
        uint16_t mask = 0;
        switch (numpad[index]) {
            case '1': mask = (uint16_t)(INPUT_DOWN | back); break;
            case '2': mask = INPUT_DOWN; break;
            case '3': mask = (uint16_t)(INPUT_DOWN | forward); break;
            case '4': mask = back; break;
            case '6': mask = forward; break;
            case '7': mask = (uint16_t)(INPUT_UP | back); break;
            case '8': mask = INPUT_UP; break;
            case '9': mask = (uint16_t)(INPUT_UP | forward); break;
            case '5':
            default:  mask = 0; break;
        }
        // The button lands on the last frame, which is what makes "236" a move
        // rather than a walk.
        return (index == length - 1) ? (uint16_t)(mask | buttonMask) : mask;
    }

    switch (actionKind) {
        case SCRIPT_ACTION_J:
            // Button alone: this one assumes the dummy is already airborne.
            return buttonMask;
        case SCRIPT_ACTION_CHARGE_2_8:
        case SCRIPT_ACTION_2_HOLD_8:
            return frameIndex < kSyntheticChargeFrames ? INPUT_DOWN : (uint16_t)(INPUT_UP | buttonMask);
        case SCRIPT_ACTION_CHARGE_4_6:
        case SCRIPT_ACTION_4_HOLD_6:
            return frameIndex < kSyntheticChargeFrames ? back : (uint16_t)(forward | buttonMask);
        case SCRIPT_ACTION_JUMP:
            return INPUT_UP;
        case SCRIPT_ACTION_DASH_FORWARD:
            return frameIndex == 0 ? forward : (frameIndex == 2 ? forward : 0);
        case SCRIPT_ACTION_DASH_BACKWARD:
            return frameIndex == 0 ? back : (frameIndex == 2 ? back : 0);
        default:
            return 0;
    }
}

static void ScheduleTriggerAction(int player,
                                  int triggerId,
                                  uint32_t simFrame,
                                  bool applyWakeBuffer) {
    if (player < 0 || player >= kPracticePlayerCount) {
        return;
    }

    if (!s_practiceConfig.triggerMasterEnabled ||
        !TriggerTargetIncludesPlayer(s_practiceConfig.triggerTarget, player)) {
        return;
    }

    const TriggerConfig& cfg = s_practiceConfig.triggers[triggerId];
    if (!cfg.enabled || cfg.actionKind == SCRIPT_ACTION_NONE) {
        return;
    }

    const uint32_t salt = (uint32_t)(triggerId + 1) * 0x9E3779B9u + (uint32_t)(player * 31);
    if (s_practiceConfig.triggerRandomize && !DeterministicCoinFlip(simFrame, salt)) {
        LOG_INFO("[Practice] Trigger skip: %s %s randomized off at frame %u",
                 SideLabel(player),
                 GetTriggerLabel(triggerId),
                 simFrame);
        return;
    }

    ScriptRuntime& script = s_playerRuntime[player].script;
    if (script.active) {
        LOG_INFO("[Practice] Trigger overwrite: %s old=%s new=%s at frame %u",
                 SideLabel(player),
                 GetActionLabel(script.actionKind),
                 GetActionLabel(cfg.actionKind),
                 simFrame);
    }
    script.active = true;
    s_lastFiredTriggerId    = triggerId;
    s_lastFiredTriggerFrame = simFrame;
    script.inputStartedLogged = false;
    script.triggerFrame = simFrame;
    script.delayFrames = ClampInt(cfg.delayFrames, 0, 120);
    if (applyWakeBuffer) {
        script.delayFrames = ClampInt(script.delayFrames - s_practiceConfig.wakeBufferFrames, 0, 120);
    }
    script.actionKind = cfg.actionKind;
    script.actionButton = cfg.actionButton;
    script.lastInput = 0xFFFFu;
    LOG_INFO("[Practice] Trigger scheduled: %s trigger=%s action=%s%s frame=%u delay=%d buffer=%d",
             SideLabel(player),
             GetTriggerLabel(triggerId),
             GetActionLabel(cfg.actionKind),
             kScriptButtonLabels[cfg.actionButton],
             simFrame,
             script.delayFrames,
             applyWakeBuffer ? s_practiceConfig.wakeBufferFrames : 0);
}

static void UpdateComboTrackers(const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    for (int attacker = 0; attacker < kPracticePlayerCount; ++attacker) {
        ComboTracker& tracker = s_comboTrackers[attacker];
        const PlayerSnapshot& attackerSnapshot = snapshots[attacker];
        const int defender = 1 - attacker;
        const PlayerSnapshot& defenderSnapshot = snapshots[defender];

        if (!attackerSnapshot.valid || !defenderSnapshot.valid) {
            continue;
        }

        const uint16_t liveHits = attackerSnapshot.comboCount;
        if (liveHits > 0) {
            const bool wasInactive = !tracker.active;
            if (wasInactive) {
                tracker.active = true;
                tracker.hits = 0;
                tracker.damage = 0;
                tracker.attackerMeterDelta = 0;
                tracker.defenderMeterDelta = 0;
                tracker.defenderHpAtStart = defenderSnapshot.hp;
                tracker.attackerMeterAtStart = attackerSnapshot.meter;
                tracker.defenderMeterAtStart = defenderSnapshot.meter;
                if (s_playerRuntime[defender].hasPrev && s_playerRuntime[defender].prev.valid) {
                    tracker.defenderHpAtStart = s_playerRuntime[defender].prev.hp;
                    tracker.defenderMeterAtStart = s_playerRuntime[defender].prev.meter;
                }
                if (s_playerRuntime[attacker].hasPrev && s_playerRuntime[attacker].prev.valid) {
                    tracker.attackerMeterAtStart = s_playerRuntime[attacker].prev.meter;
                }
                memset(tracker.scale, 0, sizeof(tracker.scale));
                LOG_INFO("[Practice] Combo start: attacker=%s defender=%s frame=%u start_hp=%u curr_hp=%u atk_meter=%u def_meter=%u",
                         SideLabel(attacker),
                         SideLabel(defender),
                         ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER),
                         (unsigned int)tracker.defenderHpAtStart,
                         (unsigned int)defenderSnapshot.hp,
                         (unsigned int)tracker.attackerMeterAtStart,
                         (unsigned int)tracker.defenderMeterAtStart);
            }

            int liveDamage = (int)tracker.defenderHpAtStart - (int)defenderSnapshot.hp;
            if (liveDamage < 0) {
                liveDamage = 0;
            }

            int liveAtkMeter = (int)attackerSnapshot.meter - (int)tracker.attackerMeterAtStart;
            int liveDefMeter = (int)defenderSnapshot.meter - (int)tracker.defenderMeterAtStart;

            const bool comboAdvanced = wasInactive || liveHits > tracker.hits || liveDamage != tracker.damage
                || liveAtkMeter != tracker.attackerMeterDelta || liveDefMeter != tracker.defenderMeterDelta;
            if (liveHits > tracker.hits) {
                tracker.hits = liveHits;
            }
            tracker.damage = liveDamage;
            tracker.attackerMeterDelta = liveAtkMeter;
            tracker.defenderMeterDelta = liveDefMeter;
            if (comboAdvanced) {
                tracker.scale[0] = ReadComboScale(attackerSnapshot.base, 0);
                tracker.scale[1] = ReadComboScale(attackerSnapshot.base, 1);
                tracker.scale[2] = ReadComboScale(attackerSnapshot.base, 2);
                tracker.scale[3] = ReadComboScale(attackerSnapshot.base, 3);
            }
        } else if (tracker.active) {
            tracker.last.valid = tracker.hits > 0 || tracker.damage > 0;
            if (tracker.last.valid) {
                tracker.last.attacker = (uint8_t)attacker;
                tracker.last.defender = (uint8_t)defender;
                tracker.last.hits = tracker.hits;
                tracker.last.damage = tracker.damage;
                tracker.last.attackerMeterDelta = tracker.attackerMeterDelta;
                tracker.last.defenderMeterDelta = tracker.defenderMeterDelta;
                tracker.last.defenderCharId = defenderSnapshot.charId;
                tracker.last.defenderMaxHp = defenderSnapshot.maxHp;
                memcpy(tracker.last.scale, tracker.scale, sizeof(tracker.last.scale));
                LOG_INFO("[Practice] Combo end: attacker=%s hits=%u damage=%d atk_meter=%+d def_meter=%+d scale=[%u,%u,%u,%u]",
                         SideLabel(attacker),
                         tracker.last.hits,
                         tracker.last.damage,
                         tracker.last.attackerMeterDelta,
                         tracker.last.defenderMeterDelta,
                         (unsigned int)tracker.last.scale[0],
                         (unsigned int)tracker.last.scale[1],
                         (unsigned int)tracker.last.scale[2],
                         (unsigned int)tracker.last.scale[3]);
            }

            tracker.active = false;
            tracker.hits = 0;
            tracker.damage = 0;
            tracker.attackerMeterDelta = 0;
            tracker.defenderMeterDelta = 0;
            tracker.defenderHpAtStart = 0;
            tracker.attackerMeterAtStart = 0;
            tracker.defenderMeterAtStart = 0;
            memset(tracker.scale, 0, sizeof(tracker.scale));
        }
    }
}

static void UpdatePlayerRuntimeForFrame(uint32_t simFrame,
                                        const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    for (int player = 0; player < kPracticePlayerCount; ++player) {
        PlayerRuntime& runtime = s_playerRuntime[player];
        const PlayerSnapshot& current = snapshots[player];
        const PlayerSnapshot& opponent = snapshots[1 - player];

        if (!current.valid || !opponent.valid) {
            continue;
        }

        if (runtime.jumpCooldown > 0) {
            runtime.jumpCooldown--;
        }

        const bool actionable = IsActionable(current);
        if (actionable) {
            if (!runtime.neutralValid) {
                runtime.neutralValid = true;
                runtime.neutralSinceFrame = simFrame;
            }
        } else {
            runtime.neutralValid = false;
            runtime.neutralSinceFrame = simFrame;
        }

        // Pressure tracking for auto-block lives in the collision hooks now: it
        // has to see HitDefs and contacts that land while the dummy is already
        // in blockstun, neither of which produces an action-state edge here.

        if (runtime.hasPrev) {
            const bool enteredStun = !IsStunned(runtime.prev.actionId) && IsStunned(current.actionId);
            if (enteredStun) {
                runtime.pendingAirTechCompletion = false;
                runtime.pendingGroundTechCompletion = false;
                LOG_INFO("[Practice] Contact seen: %s frame=%u prev_act=%u curr_act=%u",
                         SideLabel(player),
                         simFrame,
                         runtime.prev.actionId,
                         current.actionId);
            }

            if (!IsAirTechState(runtime.prev.actionId) && IsAirTechState(current.actionId)) {
                runtime.pendingAirTechCompletion = true;
                runtime.pendingGroundTechCompletion = false;
                LOG_INFO("[Practice] Air tech start: %s frame=%u action=%u",
                         SideLabel(player),
                         simFrame,
                         current.actionId);
            }
            if (!IsGroundTechState(runtime.prev.actionId) && IsGroundTechState(current.actionId)) {
                runtime.pendingGroundTechCompletion = true;
                runtime.pendingAirTechCompletion = false;
                LOG_INFO("[Practice] Ground tech start: %s frame=%u action=%u",
                         SideLabel(player),
                         simFrame,
                         current.actionId);
            }

            if (runtime.pendingAirTechCompletion &&
                !IsAirTechState(current.actionId) &&
                !IsPostTechState(current.actionId) &&
                !IsActionable(current)) {
                LOG_INFO("[Practice] Air tech completion canceled: %s frame=%u action=%u",
                         SideLabel(player),
                         simFrame,
                         current.actionId);
                runtime.pendingAirTechCompletion = false;
            }
            if (runtime.pendingGroundTechCompletion &&
                !IsGroundTechState(current.actionId) &&
                !IsPostTechState(current.actionId) &&
                !IsActionable(current)) {
                LOG_INFO("[Practice] Ground tech completion canceled: %s frame=%u action=%u",
                         SideLabel(player),
                         simFrame,
                         current.actionId);
                runtime.pendingGroundTechCompletion = false;
            }

            if (s_practiceConfig.triggerMasterEnabled) {
                if (IsBlockstun(runtime.prev.actionId) && IsActionable(current)) {
                    ScheduleTriggerAction(player, TRIGGER_AFTER_BLOCK, simFrame, false);
                }

                if (IsHitstun(runtime.prev.actionId) && (IsActionable(current) || IsTechState(current.actionId))) {
                    ScheduleTriggerAction(player, TRIGGER_AFTER_HITSTUN, simFrame, false);
                }

                if (s_practiceConfig.wakeBufferFrames > 0) {
                    if (!IsAirTechState(runtime.prev.actionId) && IsAirTechState(current.actionId)) {
                        ScheduleTriggerAction(player, TRIGGER_AFTER_AIRTECH, simFrame, true);
                        runtime.pendingAirTechCompletion = false;
                    }
                    if (!IsGroundTechState(runtime.prev.actionId) && IsGroundTechState(current.actionId)) {
                        ScheduleTriggerAction(player, TRIGGER_AFTER_GROUNDTECH, simFrame, true);
                        runtime.pendingGroundTechCompletion = false;
                    }
                    if (!IsWakeupNoTechState(runtime.prev.actionId) && IsWakeupNoTechState(current.actionId)) {
                        ScheduleTriggerAction(player, TRIGGER_ON_WAKEUP, simFrame, true);
                    }
                } else {
                    if (runtime.pendingAirTechCompletion && IsActionable(current)) {
                        ScheduleTriggerAction(player, TRIGGER_AFTER_AIRTECH, simFrame, false);
                        runtime.pendingAirTechCompletion = false;
                    }
                    if (runtime.pendingGroundTechCompletion && IsActionable(current)) {
                        ScheduleTriggerAction(player, TRIGGER_AFTER_GROUNDTECH, simFrame, false);
                        runtime.pendingGroundTechCompletion = false;
                    }
                    if (IsWakeupNoTechState(runtime.prev.actionId) && IsActionable(current)) {
                        ScheduleTriggerAction(player, TRIGGER_ON_WAKEUP, simFrame, false);
                    }
                }
            }
        }

        runtime.prev = current;
        runtime.hasPrev = true;
    }
}

static void ApplyContinuousRecovery(uint32_t simFrame,
                                    const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    bool bothNeutral = true;
    uint32_t bothNeutralFrame = 0;

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        if (!s_playerRuntime[player].neutralValid) {
            bothNeutral = false;
            break;
        }

        if (s_playerRuntime[player].neutralSinceFrame > bothNeutralFrame) {
            bothNeutralFrame = s_playerRuntime[player].neutralSinceFrame;
        }
    }

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        const PlayerSnapshot& snapshot = snapshots[player];
        if (!snapshot.valid) {
            continue;
        }

        const PlayerRuntime& runtime = s_playerRuntime[player];
        if (!runtime.neutralValid) {
            continue;
        }

        uint32_t readyFrame = runtime.neutralSinceFrame;
        if (s_practiceConfig.recoveryRequireBothNeutral) {
            if (!bothNeutral) {
                continue;
            }
            readyFrame = bothNeutralFrame;
        }

        if (simFrame < readyFrame + (uint32_t)ClampInt(s_practiceConfig.recoveryDelayFrames, 0, 120)) {
            continue;
        }

        const int hpTarget = GetRecoveryHpTarget(player, snapshot);
        const int meterTarget = GetRecoveryMeterTarget(player);
        const int guardTarget = GetRecoveryGuardTarget(player);
        if (hpTarget >= 0 && snapshot.hp != (uint16_t)hpTarget) {
            LOG_INFO("[Practice] Recovery %s HP %u->%u at frame %u",
                     SideLabel(player),
                     snapshot.hp,
                     (unsigned int)hpTarget,
                     simFrame);
            WritePlayerHpFields(snapshot.base, ClampInt(hpTarget, 0, snapshot.maxHp));
        }
        if (meterTarget >= 0 && snapshot.meter != (uint16_t)meterTarget) {
            LOG_INFO("[Practice] Recovery %s Meter %u->%u at frame %u",
                     SideLabel(player),
                     snapshot.meter,
                     (unsigned int)meterTarget,
                     simFrame);
            WriteMemory<uint16_t>(snapshot.base + ENTITY_OFF_METER, ClampU16(meterTarget, kMeterMax));
        }
        if (guardTarget >= 0 && snapshot.guardGauge != (uint16_t)guardTarget) {
            LOG_INFO("[Practice] Recovery %s Guard %u->%u at frame %u",
                     SideLabel(player),
                     snapshot.guardGauge,
                     (unsigned int)guardTarget,
                     simFrame);
            WriteMemory<uint16_t>(snapshot.base + ENTITY_OFF_GUARD_GAUGE, ClampU16(guardTarget, kGuardGaugeMax));
        }
    }
}

// The Defense row lists only what the dummy's own category can perform, so a
// stored choice can become unreachable the moment the dummy changes character -
// on a control swap, on a rematch with a different pick, on a side swap. Leaving
// it stale made the row read as one mechanic while the driver silently ran
// none. Falling back to Native keeps a mechanic selected whatever the new
// character is.
static void ClampDefensiveResponseToCategory(int category) {
    const Training::DefensiveResponse current =
        (Training::DefensiveResponse)ClampInt(g_defensiveResponse, 0,
                                              (int)Training::DefensiveResponse::Count - 1);
    if (Training::ResponseSupportedByCategory(current, category)) {
        return;
    }
    g_defensiveResponse = (int)Training::DefensiveResponse::CharacterNative;
    LOG_INFO("[Practice] Defense '%s' is not available to defence category %d; "
             "falling back to '%s'",
             Training::DefensiveResponseLabel(current),
             category,
             Training::DefensiveResponseLabel(Training::DefensiveResponse::CharacterNative));
}

static void PushDefenseConfig() {
    // Re-derived every frame because the dummy can change underneath it.
    static int s_lastDefenceCategory = -1;
    const int defenceCategory = PracticeDefense_DummyDefenseCategory();
    if (defenceCategory != s_lastDefenceCategory) {
        LOG_INFO("[Practice] Dummy defence category %d -> %d (dummy=%s)",
                 s_lastDefenceCategory, defenceCategory, SideLabel(DummySide()));
        s_lastDefenceCategory = defenceCategory;
        ClampDefensiveResponseToCategory(defenceCategory);
    }

    PracticeDefenseConfig config{};
    config.enabled = (s_practiceConfig.dummyControlMode == DUMMY_CONTROL_MOD);
    config.policy = BlockPolicyForMode(s_practiceConfig.blockMode);
    config.randomPercent = 50;
    // Native adaptive crouches unless the attack is stand-only, so crouch is the
    // compatible default for masks that permit either lane.
    config.preferCrouch = (s_practiceConfig.stanceMode != DUMMY_STANCE_STAND);
    config.response = (Training::DefensiveResponse)ClampInt(
        g_defensiveResponse, 0, (int)Training::DefensiveResponse::Count - 1);
    config.dummyPlayer = DummySide();
    config.macroOwnsDummyInput = (InputMacro_GetState() == MACRO_REPLAYING);
    config.paused = s_paused;
    PracticeDefense_SetConfig(config);
}

// Whether the dummy should be holding an anticipatory guard this frame. The
// authoritative decision is made at contact time by the guard-resolver hook;
// this only produces natural posture and a truthful input display.
//
// First / After First Hit key off contact GROUPS counted from real collision
// outcomes, not from action-state edges: a second hit landing while the dummy
// is already in blockstun produces no state edge at all.
static bool ShouldHoldAutoBlock(const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    const PlayerSnapshot& dummy = snapshots[DummySide()];
    const PlayerSnapshot& attacker = snapshots[HumanSide()];
    if (!dummy.valid || !attacker.valid) {
        return false;
    }
    // A pending defensive mechanic has to reach the dummy even when the block
    // policy is not asking for a guard this frame - repel and the guard counter
    // are not guard actions at all, and gating them behind auto-block is what
    // made the Defense row look dead.
    return PracticeDefense_WantsAnticipatoryGuard() ||
           PracticeDefense_DefenseInput() != Training::DefenseInputKind::None;
}

// Semantic back / down-back, resolved to physical left/right from the dummy's
// live facing here at injection time so a cross-up cannot stale it.
static uint16_t BuildDummyBlockInput(const PlayerSnapshot& dummy,
                                     const PlayerSnapshot& attacker) {
    (void)attacker;
    // One frame without BACK so the next press is the fresh edge the native
    // parry window arms on. Without this the dummy holds guard forever and can
    // never parry, which is exactly how the engine is meant to behave.
    // Every defensive mechanic is driven by an input a player could make; the
    // engine decides whether it takes.
    const int8_t facing = dummy.facingRight ? (int8_t)1 : (int8_t)-1;
    switch (PracticeDefense_DefenseInput()) {
        case Training::DefenseInputKind::ReleaseGuard:
            // One frame with no direction, so the next press is a fresh edge.
            PracticeDefense_NoteAnticipatoryFacing(facing, false);
            return 0;
        case Training::DefenseInputKind::ForwardTap:
            PracticeDefense_NoteAnticipatoryFacing(facing, true);
            return ForwardMask(dummy);
        case Training::DefenseInputKind::DownTap:
            // Down alone: the low arm needs the down edge with no left or right
            // held, so a diagonal would fail the check.
            PracticeDefense_NoteAnticipatoryFacing(facing, true);
            return INPUT_DOWN;
        case Training::DefenseInputKind::DodgePress:
            // BACK keeps the back-dodge variant rather than the forward one.
            PracticeDefense_NoteAnticipatoryFacing(facing, true);
            return (uint16_t)(BackMask(dummy) | INPUT_D);
        case Training::DefenseInputKind::CounterForward:
            PracticeDefense_NoteAnticipatoryFacing(facing, true);
            return (uint16_t)(ForwardMask(dummy) | INPUT_D);
        case Training::DefenseInputKind::ArmGuardState: {
            // 214D is a motion, so it is fed a frame at a time through the same
            // sequence builder the trigger runner uses rather than as one mask.
            PracticeDefense_NoteAnticipatoryFacing(facing, true);
            const int step = ClampInt(PracticeDefense_ArmStep(), 0,
                                      Training::kGuardStateMotionFrames - 1);
            return GetActionSequenceInput(dummy, SCRIPT_ACTION_214,
                                          SCRIPT_BUTTON_D, step);
        }
        case Training::DefenseInputKind::None:
        default:
            break;
    }

    const Training::SemanticDirection direction = PracticeDefense_GetAnticipatoryGuard();
    uint16_t resolved = Training::ResolveSemanticDirection(direction, dummy.facingRight);
    if (resolved == 0) {
        // Nothing decoded yet: plain back keeps proximity guard natural - but
        // it has to respect the stance, or a crouching dummy stands up for
        // every frame no lane was decoded on, which is most of them.
        resolved = BackMask(dummy);
        if (s_practiceConfig.stanceMode == DUMMY_STANCE_CROUCH) {
            resolved |= INPUT_DOWN;
        }
    }
    PracticeDefense_NoteAnticipatoryFacing(dummy.facingRight ? (int8_t)1 : (int8_t)-1, true);
    return resolved;
}

static uint16_t BuildJumpInput(const PlayerSnapshot& snapshot, int jumpMode, uint32_t simFrame) {
    int effectiveMode = jumpMode;
    if (effectiveMode == DUMMY_JUMP_RANDOM) {
        effectiveMode = DeterministicCoinFlip(simFrame, 0xA51C3E27u)
            ? DUMMY_JUMP_FORWARD
            : DUMMY_JUMP_BACKWARD;
    }

    uint16_t input = INPUT_UP;
    if (effectiveMode == DUMMY_JUMP_FORWARD) {
        input |= ForwardMask(snapshot);
    } else if (effectiveMode == DUMMY_JUMP_BACKWARD) {
        input |= BackMask(snapshot);
    }
    return input;
}

static uint16_t ComputeScriptInput(int player,
                                   const PlayerSnapshot snapshots[kPracticePlayerCount],
                                   uint32_t simFrame) {
    PlayerRuntime& runtime = s_playerRuntime[player];
    if (!runtime.script.active) {
        return 0;
    }

    const uint32_t startFrame = runtime.script.triggerFrame + (uint32_t)runtime.script.delayFrames;
    if (simFrame < startFrame) {
        return 0;
    }

    const int sequenceFrame = (int)(simFrame - startFrame);
    const int sequenceLength = GetActionSequenceLength(runtime.script.actionKind);
    if (sequenceFrame >= sequenceLength) {
        LOG_INFO("[Practice] Trigger complete: %s action=%s%s end_frame=%u",
                 SideLabel(player),
                 GetActionLabel(runtime.script.actionKind),
                 kScriptButtonLabels[runtime.script.actionButton],
                 simFrame);
        runtime.script.active = false;
        runtime.script.inputStartedLogged = false;
        runtime.script.lastInput = 0xFFFFu;
        return 0;
    }

    if (!runtime.script.inputStartedLogged) {
        LOG_INFO("[Practice] Trigger start: %s action=%s%s start_frame=%u",
                 SideLabel(player),
                 GetActionLabel(runtime.script.actionKind),
                 kScriptButtonLabels[runtime.script.actionButton],
                 startFrame);
        runtime.script.inputStartedLogged = true;
    }

    const uint16_t input = GetActionSequenceInput(snapshots[player],
                                                  runtime.script.actionKind,
                                                  runtime.script.actionButton,
                                                  sequenceFrame);
    if (runtime.script.lastInput != input) {
        LOG_INFO("[Practice] Trigger step: %s action=%s%s seq=%d/%d input=0x%04X frame=%u",
                 SideLabel(player),
                 GetActionLabel(runtime.script.actionKind),
                 kScriptButtonLabels[runtime.script.actionButton],
                 sequenceFrame + 1,
                 sequenceLength,
                 (unsigned int)input,
                 simFrame);
        runtime.script.lastInput = input;
    }
    return input;
}

static uint16_t ComputeAutomationInput(int player,
                                       const PlayerSnapshot snapshots[kPracticePlayerCount],
                                       uint32_t simFrame) {
    uint16_t input = ComputeScriptInput(player, snapshots, simFrame);
    if (input != 0) {
        return input;
    }

    if (player != DummySide()) {
        return 0;
    }

    if (s_practiceConfig.dummyControlMode != DUMMY_CONTROL_MOD) {
        return 0;
    }

    const PlayerSnapshot& dummy = snapshots[player];
    const PlayerSnapshot& attacker = snapshots[1 - player];
    PlayerRuntime& runtime = s_playerRuntime[player];
    if (!dummy.valid || !attacker.valid) {
        return 0;
    }

    if (ShouldHoldAutoBlock(snapshots)) {
        return BuildDummyBlockInput(dummy, attacker);
    }

    if (runtime.jumpHoldFrames > 0) {
        if (!IsGroundedAction(dummy) || !IsActionable(dummy)) {
            LOG_INFO("[Practice] Dummy jump accepted: action=%u y=%d frame=%u",
                     dummy.actionId,
                     dummy.y,
                     simFrame);
            runtime.jumpHoldFrames = 0;
            runtime.jumpHoldInput = 0;
        } else {
            runtime.jumpHoldFrames--;
            if (runtime.jumpHoldFrames == 0) {
                LOG_INFO("[Practice] Dummy jump hold expired: action=%u y=%d frame=%u",
                         dummy.actionId,
                         dummy.y,
                         simFrame);
            }
            return runtime.jumpHoldInput;
        }
    }

    if (IsActionable(dummy) && IsGroundedAction(dummy)) {
        const bool wantsJumpLoop = s_practiceConfig.stanceMode == DUMMY_STANCE_JUMP ||
                                   s_practiceConfig.jumpMode != DUMMY_JUMP_DISABLED;
        if (wantsJumpLoop && runtime.jumpCooldown <= 0) {
            const int jumpMode = s_practiceConfig.jumpMode == DUMMY_JUMP_DISABLED
                ? DUMMY_JUMP_NEUTRAL
                : s_practiceConfig.jumpMode;
            runtime.jumpCooldown = ClampInt(s_practiceConfig.jumpCadenceFrames, 1, 120);
            runtime.jumpHoldFrames = kJumpHoldFrames;
            runtime.jumpHoldInput = BuildJumpInput(dummy, jumpMode, simFrame);
            LOG_INFO("[Practice] Dummy jump loop start: mode=%s frame=%u hold=%d input=0x%04X",
                     kJumpModeLabels[jumpMode],
                     simFrame,
                     runtime.jumpHoldFrames,
                     (unsigned int)runtime.jumpHoldInput);
            return runtime.jumpHoldInput;
        }
    }

    if (s_practiceConfig.stanceMode == DUMMY_STANCE_CROUCH && IsGroundedAction(dummy)) {
        return INPUT_DOWN;
    }

    return 0;
}

static void ApplyAutomationOverrides(uint32_t simFrame,
                                     const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    // The pause menu reads both pads, and an active override replaces
    // g_inputState[p].current wholesale - so auto-block holding back or a macro
    // playing back would drive the menu cursor. The simulation is frozen in
    // this substate anyway, so the automation has nothing to do.
    if (GetSubstate() == MATCH_SUB_PAUSE) {
        ReleaseOwnedOverride(0);
        ReleaseOwnedOverride(1);
        return;
    }

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        if (player == DummySide() && InputMacro_GetState() == MACRO_REPLAYING) {
            ReleaseOwnedOverride(player);
            continue;
        }

        const uint16_t input = ComputeAutomationInput(player, snapshots, simFrame);
        if (input != 0) {
            SetOwnedOverride(player, input);
        } else {
            ReleaseOwnedOverride(player);
        }
    }
}

static void ResetPracticeState(void) {
    if (s_paused || InputSystem_GetControlSwap()) {
        LOG_INFO("[Practice] Cleaning up practice state");
    }

    ResetPracticeTransientRuntime(true);
    InputMacro_Stop();
    FrameAdvantage_ResetState();
    s_paused = false;
    s_stepRequested = false;
    s_stepCounter = 0;

    if (InputSystem_GetControlSwap()) {
        PracticeTools_ApplyControlSwapState(false);
    }
}

static bool TryLoadRoundStartReset(const char* sourceTag) {
    const SavestateInfo* roundStartInfo = Savestate_GetRoundStartInfo();
    const uint32_t requestFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    const uint32_t slotFrame = (roundStartInfo && roundStartInfo->valid)
        ? roundStartInfo->frame
        : 0u;

    LOG_INFO("[Practice] Round-start reset requested: source=%s frame=%u slot_valid=%d slot_frame=%u",
             sourceTag ? sourceTag : "unknown",
             requestFrame,
             (roundStartInfo && roundStartInfo->valid) ? 1 : 0,
             slotFrame);

    if (!roundStartInfo || !roundStartInfo->valid) {
        LOG_WARN("[Practice] Round-start reset skipped: source=%s no round-start autosave",
                 sourceTag ? sourceTag : "unknown");
        return false;
    }

    if (!Savestate_LoadRoundStart()) {
        LOG_WARN("[Practice] Round-start reset FAILED: source=%s request_frame=%u slot_frame=%u",
                 sourceTag ? sourceTag : "unknown",
                 requestFrame,
                 slotFrame);
        return false;
    }

    LOG_INFO("[Practice] Round-start reset complete: source=%s request_frame=%u restored_slot_frame=%u",
             sourceTag ? sourceTag : "unknown",
             requestFrame,
             slotFrame);
    PushToast("Round start restored", IM_COL32(120, 255, 180, 255));
    return true;
}

static void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void CopyBindingName(const KeyBinding_t* binding, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    InputSystem_GetBindingDisplayName(binding, out, (int)outSize);
}

static void FormatHotkeyChord(char* out,
                              size_t outSize,
                              HotkeyAction action,
                              const KeyBinding_t* modifierBinding = nullptr) {
    if (!out || outSize == 0) {
        return;
    }

    char actionKey[128] = {};
    HotkeyConfig_GetBindingDisplayName(action, actionKey, (int)sizeof(actionKey));
    if (!modifierBinding) {
        snprintf(out, outSize, "%s", actionKey);
        return;
    }

    char modifierKey[128] = {};
    CopyBindingName(modifierBinding, modifierKey, sizeof(modifierKey));
    snprintf(out, outSize, "%s+%s", actionKey, modifierKey);
}

static void FormatHotkeyButtonLabel(char* out,
                                    size_t outSize,
                                    const char* text,
                                    const char* id,
                                    HotkeyAction action,
                                    const KeyBinding_t* modifierBinding = nullptr) {
    char chord[256] = {};
    FormatHotkeyChord(chord, sizeof(chord), action, modifierBinding);
    snprintf(out, outSize, "%s [%s]##%s", text, chord, id ? id : text);
}

static void FormatPositionPresetHint(char* out, size_t outSize) {
    char loadKey[128] = {};
    char upKey[128] = {};
    char downKey[128] = {};
    char rightKey[128] = {};
    char leftKey[128] = {};
    HotkeyConfig_GetBindingDisplayName(HOTKEY_POSITION_LOAD, loadKey, (int)sizeof(loadKey));
    CopyBindingName(GetP1DirectionBinding(P1_DIRECTION_BIND_UP), upKey, sizeof(upKey));
    CopyBindingName(GetP1DirectionBinding(P1_DIRECTION_BIND_DOWN), downKey, sizeof(downKey));
    CopyBindingName(GetP1DirectionBinding(P1_DIRECTION_BIND_RIGHT), rightKey, sizeof(rightKey));
    CopyBindingName(GetP1DirectionBinding(P1_DIRECTION_BIND_LEFT), leftKey, sizeof(leftKey));
    snprintf(out,
             outSize,
             "Presets: [%s] saved, [%s]+%s round start, [%s]+%s mid, [%s]+%s right corner, [%s]+%s left corner",
             loadKey,
             loadKey,
             upKey,
             loadKey,
             downKey,
             loadKey,
             rightKey,
             loadKey,
             leftKey);
}

static void FormatHotkeyToast(char* out,
                              size_t outSize,
                              const char* text,
                              HotkeyAction action,
                              const KeyBinding_t* modifierBinding = nullptr) {
    char chord[256] = {};
    FormatHotkeyChord(chord, sizeof(chord), action, modifierBinding);
    snprintf(out, outSize, "%s [%s]", text, chord);
}

static void RenderNativeTrainingCombo(const char* label,
                                      uintptr_t address,
                                      int maxValue,
                                      const char* const* labels,
                                      int labelCount,
                                      const char* helpText = nullptr) {
    int value = ReadNativeTrainingSetting(address, maxValue);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo(label, &value, labels, labelCount)) {
        WriteNativeTrainingSetting(address, value, maxValue, label);
    }
    if (helpText) {
        ImGui::SameLine();
        HelpMarker(helpText);
    }
}

static bool QueuePracticeExitRoute(uint8_t route, const char* label) {
    if (!IsPracticeModeNow()) {
        return false;
    }

    if (GetSubstate() != MATCH_SUB_GAMEPLAY && GetSubstate() != MATCH_SUB_PAUSE) {
        LOG_WARN("[Practice] Exit route ignored: substate=%u route=%u",
                 GetSubstate(),
                 (unsigned int)route);
        return false;
    }

    if (s_paused) {
        PracticeTools_SetPaused(false);
    }

    ReleaseOwnedOverride(0);
    ReleaseOwnedOverride(1);
    WriteMemory<uint8_t>(ADDR_MATCH_HEADER + MATCH_HEADER_TRANSITION_OFFSET, 1);
    WriteMemory<uint8_t>(ADDR_MATCH_HEADER + kMatchHeaderEndRouteOffset, route);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, MATCH_SUB_GAMEPLAY);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0);

    LOG_INFO("[Practice] Queued match exit: %s route=%u",
             label ? label : "Exit",
             (unsigned int)route);
    PushToast(label ? label : "Exit queued", IM_COL32(255, 220, 120, 255));
    return true;
}

static void RenderPlayerStatus(const PlayerSnapshot& snapshot, const char* label) {
    if (!snapshot.valid) {
        ImGui::TextDisabled("%s: unavailable", label);
        return;
    }

    const float hpPct = snapshot.maxHp > 0 ? (float)snapshot.hp / (float)snapshot.maxHp * 100.0f : 0.0f;
    ImGui::Text("%s: %s  HP %u/%u (%.0f%%)  Meter %u  Guard %u",
                label, GetCharacterName(snapshot.charId),
                snapshot.hp, snapshot.maxHp, hpPct,
                snapshot.meter, snapshot.guardGauge);
}

static void RenderOverviewTab(const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    const bool canUsePositionTools = CanUsePositionTools();
    const KeyBinding_t* p1UpBinding = GetP1DirectionBinding(P1_DIRECTION_BIND_UP);
    const KeyBinding_t* p1DownBinding = GetP1DirectionBinding(P1_DIRECTION_BIND_DOWN);
    char savePositionLabel[192] = {};
    char loadSavedLabel[192] = {};
    char roundStartLabel[256] = {};
    char midScreenLabel[256] = {};
    char positionHint[512] = {};
    FormatHotkeyButtonLabel(savePositionLabel,
                            sizeof(savePositionLabel),
                            "Save Position",
                            "overview_save_position",
                            HOTKEY_POSITION_SAVE);
    FormatHotkeyButtonLabel(loadSavedLabel,
                            sizeof(loadSavedLabel),
                            "Load Saved",
                            "overview_load_saved",
                            HOTKEY_POSITION_LOAD);
    FormatHotkeyButtonLabel(roundStartLabel,
                            sizeof(roundStartLabel),
                            "Round Start",
                            "overview_round_start",
                            HOTKEY_POSITION_LOAD,
                            p1UpBinding);
    FormatHotkeyButtonLabel(midScreenLabel,
                            sizeof(midScreenLabel),
                            "Mid Screen",
                            "overview_mid_screen",
                            HOTKEY_POSITION_LOAD,
                            p1DownBinding);
    FormatPositionPresetHint(positionHint, sizeof(positionHint));

    // Keys are rebindable, so the labels read the live binding instead of the
    // defaults they used to hard-code.
    char pauseLabel[48], stepLabel[48], swapLabel[48];
    HotkeyConfig_FormatLabel(pauseLabel, sizeof(pauseLabel), "Paused", HOTKEY_PAUSE_TOGGLE);
    HotkeyConfig_FormatLabel(stepLabel, sizeof(stepLabel), "Step", HOTKEY_FRAME_STEP);
    HotkeyConfig_FormatLabel(swapLabel, sizeof(swapLabel), "Control P2", HOTKEY_CONTROL_SWAP);

    bool paused = s_paused;
    if (ImGui::Checkbox(pauseLabel, &paused)) {
        PracticeTools_SetPaused(paused);
    }

    ImGui::SameLine();
    if (ImGui::Button(stepLabel) && s_paused && !s_stepRequested) {
        s_stepRequested = true;
        s_stepCounter++;
    }

    if (s_paused && s_stepCounter > 0) {
        ImGui::SameLine();
        ImGui::Text("Step: %d", s_stepCounter);
    }

    bool swapped = InputSystem_GetControlSwap();
    if (ImGui::Checkbox(swapLabel, &swapped)) {
        PracticeTools_ApplyControlSwapState(swapped);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Controlling: %s", swapped ? "P2" : "P1");

    bool hitbox = HitboxViewer_IsEnabled();
    if (ImGui::Checkbox("Hitbox Viewer", &hitbox)) {
        HitboxViewer_SetEnabled(hitbox);
    }

    bool comboOverlay = s_practiceConfig.comboOverlayEnabled;
    if (ImGui::Checkbox("Combo Overlay", &comboOverlay)) {
        s_practiceConfig.comboOverlayEnabled = comboOverlay;
    }

    bool showInputs = ReadNativeTrainingSetting(ADDR_CMD_HISTORY_DISPLAY, 1) != 0;
    if (ImGui::Checkbox("Show Inputs", &showInputs)) {
        WriteNativeTrainingSetting(ADDR_CMD_HISTORY_DISPLAY,
                                   showInputs ? 1 : 0,
                                   1,
                                   "Show Inputs");
    }
    ImGui::SameLine();
    HelpMarker("Uses the game's native command-history display.");

    bool damageCounter = ReadNativeTrainingSetting(ADDR_TRAINING_DAMAGE_DISPLAY, 1) != 0;
    if (ImGui::Checkbox("Damage Counter", &damageCounter)) {
        WriteNativeTrainingSetting(ADDR_TRAINING_DAMAGE_DISPLAY,
                                   damageCounter ? 1 : 0,
                                   1,
                                   "Damage Counter");
    }
    ImGui::SameLine();
    HelpMarker("Uses the native training damage percentage display.");

    ImGui::Separator();

    if (ImGui::Button("Character Select")) {
        QueuePracticeExitRoute(kMatchRouteCharSel, "Character Select");
    }
    ImGui::SameLine();
    if (ImGui::Button("Main Menu")) {
        QueuePracticeExitRoute(kMatchRouteMenu, "Main Menu");
    }

    ImGui::Separator();

    ImGui::BeginDisabled(!canUsePositionTools);
    if (ImGui::Button(savePositionLabel)) {
        if (SavePositionSnapshot(snapshots)) {
            PushToast("Position saved", IM_COL32(120, 220, 255, 255));
        } else {
            PushToast("Position save unavailable", IM_COL32(255, 180, 120, 255));
        }
    }
    ImGui::SameLine();
    if (s_positionSnapshot.valid) {
        if (ImGui::Button(loadSavedLabel)) {
            if (LoadPositionSnapshot()) {
                PushToast("Saved position restored", IM_COL32(120, 255, 180, 255));
            }
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(loadSavedLabel);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(roundStartLabel)) {
        if (ApplyPositionPreset(kRoundStartPositionPreset)) {
            PushToast("Round Start", IM_COL32(120, 255, 180, 255));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(midScreenLabel)) {
        if (ApplyPositionPreset(kMidScreenPositionPreset)) {
            PushToast("Mid Screen", IM_COL32(120, 255, 180, 255));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Swap Sides")) {
        SwapPlayerPositions(snapshots);
        PushToast("Positions swapped", IM_COL32(255, 220, 120, 255));
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", positionHint);
    if (!canUsePositionTools) {
        ImGui::TextDisabled("Position tools unlock after the round intro ends.");
    }

    const SavestateInfo* roundStartInfo = Savestate_GetRoundStartInfo();
    if (roundStartInfo && roundStartInfo->valid) {
        if (ImGui::Button("Round Reset")) {
            TryLoadRoundStartReset("overview_tab");
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Round Reset");
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    RenderPlayerStatus(snapshots[0], "P1");
    RenderPlayerStatus(snapshots[1], "P2");
    ImGui::TextDisabled("Frame %u", ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
}

static void RenderAutoBlockTelemetry() {
    if (!ImGui::CollapsingHeader("Auto-Block Diagnostics")) {
        return;
    }

    const PracticeDefenseTelemetry& t = PracticeDefense_GetTelemetry();
    ImGui::Indent();

    ImGui::Text("Plan: %s  (%s scan, %u threat%s)",
                Training::GuardLaneLabel(t.plan.lane),
                t.plan.exactContactScan ? "exact" : "fallback",
                (unsigned int)t.plan.threatCount,
                t.plan.threatCount == 1 ? "" : "s");
    if (t.plan.lane == Training::GuardLane::Conflict) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "HIGH/LOW CONFLICT");
    }
    ImGui::Text("Sequence: %s  gen %u  contact groups %u  random %s",
                t.sequence.active ? "active" : "idle",
                (unsigned int)t.sequence.generation,
                (unsigned int)t.sequence.resolvedContactGroups,
                t.sequence.randomBlock ? "block" : "no-block");

    if (t.hasLastContact) {
        ImGui::Separator();
        if (t.lastSource == Training::ThreatSource::HitDef) {
            ImGui::Text("Threat: HitDef[%d] id %u", t.lastHitDefSlot, (unsigned int)t.lastHitDefId);
        } else {
            ImGui::Text("Threat: direct attack");
        }
        ImGui::Text("Mask: 0x%05X  %s",
                    (unsigned int)t.lastAttackMask,
                    Training::GroundGuardClassLabel(t.lastClass));
        ImGui::Text("Lane: %s   eligible %s   pre-armed %s   armed at contact %s",
                    Training::GuardLaneLabel(t.lastLane),
                    t.lastEligible ? "yes" : "no",
                    t.lastPrearmed ? "yes" : "no",
                    t.lastSafetyArm ? "yes" : "no");
        ImGui::Text("Flags: 0x%08X -> 0x%08X%s",
                    (unsigned int)t.lastFlagsBefore,
                    (unsigned int)t.lastFlagsTemp,
                    t.lastNativeGuardWithoutMod ? "  (already guarding natively)" : "");
        ImGui::Text("Facing: input %d  contact %d",
                    (int)t.facingAtInput, (int)t.facingAtContact);
        ImGui::Text("Result: %s (%u)",
                    Training::ContactResolutionLabel(t.lastResult),
                    (unsigned int)t.lastResult);
    } else {
        ImGui::TextDisabled("No contact resolved yet.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("guard calls %u | lanes applied %u | conflicts %u | no-lane %u | special-guard %u",
                        (unsigned int)t.guardHookCalls,
                        (unsigned int)t.laneApplications,
                        (unsigned int)t.conflictFrames,
                        (unsigned int)t.refusedNoLane,
                        (unsigned int)t.refusedSpecialGuard);
    if (t.lateBlockSignatures > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "LATE BLOCK regressions: %u", (unsigned int)t.lateBlockSignatures);
    }

    ImGui::Unindent();
}

static void RenderOpponentTab(const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    if (snapshots[1].valid) {
        ImGui::Text("Dummy: %s  Weight %u (%s)",
                    GetCharacterName(snapshots[1].charId),
                    (unsigned int)GetCharacterWeightValue(snapshots[1].charId),
                    GetWeightClassLabel(snapshots[1].charId));
        ImGui::Separator();
    }

    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Dummy Type", &s_practiceConfig.dummyControlMode,
                 kDummyControlModeLabels,
                 IM_ARRAYSIZE(kDummyControlModeLabels));
    ImGui::SameLine();
    HelpMarker(
        "Vanilla reuses the game's own training pause-menu settings for the\n"
        "AI-controlled side. Mod keeps the richer custom block/stance\n"
        "and auto-jump logic in this menu.");

    if (s_practiceConfig.dummyControlMode == DUMMY_CONTROL_NATIVE) {
        ImGui::TextDisabled("Vanilla dummy settings follow the AI-controlled side. Control swap moves them to P1.");
        RenderNativeTrainingCombo(
            "CPU",
            ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE,
            1,
            kNativeCpuLabels,
            IM_ARRAYSIZE(kNativeCpuLabels),
            "Off keeps the native training dummy active. On switches the opponent\n"
            "to full CPU AI and disables the native dummy option rows.");

        if (ReadNativeTrainingSetting(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE, 1) == 0) {
            RenderNativeTrainingCombo(
                "Air Tech",
                ADDR_TRAINING_AIR_TECH_SETTING,
                4,
                kNativeAirTechLabels,
                IM_ARRAYSIZE(kNativeAirTechLabels));
            RenderNativeTrainingCombo(
                "Ground Tech",
                ADDR_TRAINING_GROUND_TECH_SETTING,
                3,
                kNativeGroundTechLabels,
                IM_ARRAYSIZE(kNativeGroundTechLabels));
            RenderNativeTrainingCombo(
                "Block Type",
                ADDR_TRAINING_BLOCK_TYPE_SETTING,
                2,
                kNativeBlockTypeLabels,
                IM_ARRAYSIZE(kNativeBlockTypeLabels));
            RenderNativeTrainingCombo(
                "Dummy State",
                ADDR_TRAINING_DUMMY_STATE_SETTING,
                3,
                kNativeDummyStateLabels,
                IM_ARRAYSIZE(kNativeDummyStateLabels));
        } else {
            ImGui::TextDisabled("Full CPU AI is active. Turn CPU Off to configure native dummy behavior.");
        }
        return;
    }

    if (!PracticeDefense_HasOrdinaryGuardHook()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "Contact-time guard hook unavailable (%s).",
                           PracticeDefense_GetInstallError());
        ImGui::TextWrapped("Auto-block cannot guarantee the first active frame without it, "
                           "so it stays disabled rather than blocking one frame late.");
        ImGui::Separator();
    }

    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Auto-Block", &s_practiceConfig.blockMode, kBlockModeLabels, IM_ARRAYSIZE(kBlockModeLabels));
    ImGui::SameLine(); HelpMarker(
        "Guard height comes from the live attack mask on every contact, so multi-hit\n"
        "moves that change height mid-string are followed hit by hit. One guard lane\n"
        "is fixed per simulation frame, so a simultaneous overhead and low is reported\n"
        "as a conflict instead of being blocked twice.\n\n"
        "None: Dummy does not block.\n"
        "All: Blocks every guardable contact.\n"
        "First Hit: Blocks only the first contact of a string.\n"
        "After First Hit: Takes the first contact, then blocks once it can legally guard again.\n"
        "Random: 50/50 per string, rolled deterministically.\n"
        "Adaptive: Same as All; the lane always follows the attack.");

    RenderAutoBlockTelemetry();

    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Stance", &s_practiceConfig.stanceMode, kStanceModeLabels, IM_ARRAYSIZE(kStanceModeLabels));
    ImGui::SameLine(); HelpMarker(
        "Neutral: Default AI behavior.\n"
        "Stand: Force standing.\n"
        "Crouch: Hold down while grounded.\n"
        "Jump: Repeatedly jumps after landing.");

    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Auto-Jump", &s_practiceConfig.jumpMode, kJumpModeLabels, IM_ARRAYSIZE(kJumpModeLabels));

    if (s_practiceConfig.jumpMode != DUMMY_JUMP_DISABLED ||
        s_practiceConfig.stanceMode == DUMMY_STANCE_JUMP) {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Jump Cadence (frames)", &s_practiceConfig.jumpCadenceFrames);
        s_practiceConfig.jumpCadenceFrames = ClampInt(s_practiceConfig.jumpCadenceFrames, 1, 120);
    }
}

static void RenderPlayerValueEditor(int player, const PlayerSnapshot& snapshot) {
    PlayerValueEditor& editor = s_valueEditors[player];
    const int hpCap = GetEditableHpCap(snapshot);
    ImGui::PushID(player);

    if (!snapshot.valid) {
        ImGui::TextDisabled("Player unavailable");
        ImGui::PopID();
        return;
    }

    ImGui::Text("%s - %s", SideLabel(player), GetCharacterName(snapshot.charId));
    ImGui::TextDisabled("Max HP %u | Weight %u (%s) | Guard Max %u",
                        (unsigned int)hpCap,
                        (unsigned int)GetCharacterWeightValue(snapshot.charId),
                        GetWeightClassLabel(snapshot.charId),
                        (unsigned int)kGuardGaugeMax);
    ImGui::Text("Current: HP %u | Meter %u | Guard %u | Pos (%d, %d)",
                snapshot.hp,
                snapshot.meter,
                snapshot.guardGauge,
                snapshot.x,
                snapshot.y);

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("HP", &editor.hp)) {
        editor.dirty = true;
    }
    editor.hp = ClampInt(editor.hp, 0, hpCap);

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Meter", &editor.meter)) {
        editor.dirty = true;
    }
    editor.meter = ClampInt(editor.meter, 0, kMeterMax);

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Guard", &editor.guardGauge)) {
        editor.dirty = true;
    }
    editor.guardGauge = ClampInt(editor.guardGauge, 0, kGuardGaugeMax);

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("X", &editor.x)) {
        editor.dirty = true;
    }
    editor.x = ClampS16(editor.x);

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Y", &editor.y)) {
        editor.dirty = true;
    }
    editor.y = ClampS16(editor.y);

    if (ImGui::Button("Copy Current")) {
        CopySnapshotToEditor(player, snapshot);
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        WritePlayerValues(player, editor, "apply");
        CopySnapshotToEditor(player, ReadPlayerSnapshot(player));
        PushToast(player == 0 ? "P1 values applied" : "P2 values applied",
                  IM_COL32(120, 220, 255, 255));
    }
    ImGui::SameLine();
    if (ImGui::Button("Full")) {
        editor.hp = hpCap;
        editor.meter = kMeterMax;
        editor.guardGauge = kGuardGaugeMax;
        WritePlayerValues(player, editor, "full");
        CopySnapshotToEditor(player, ReadPlayerSnapshot(player));
    }
    ImGui::SameLine();
    if (ImGui::Button("Default")) {
        editor.hp = hpCap;
        editor.meter = 0;
        editor.guardGauge = kGuardGaugeMax;
        WritePlayerValues(player, editor, "default");
        CopySnapshotToEditor(player, ReadPlayerSnapshot(player));
    }

    ImGui::PopID();
}

static void RenderValuesTab(const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    const bool canUsePositionTools = CanUsePositionTools();
    const KeyBinding_t* p1UpBinding = GetP1DirectionBinding(P1_DIRECTION_BIND_UP);
    const KeyBinding_t* p1DownBinding = GetP1DirectionBinding(P1_DIRECTION_BIND_DOWN);
    char savePositionLabel[192] = {};
    char loadSavedLabel[192] = {};
    char roundStartLabel[256] = {};
    char midScreenLabel[256] = {};
    char positionHint[512] = {};
    FormatHotkeyButtonLabel(savePositionLabel,
                            sizeof(savePositionLabel),
                            "Save Position",
                            "values_save_position",
                            HOTKEY_POSITION_SAVE);
    FormatHotkeyButtonLabel(loadSavedLabel,
                            sizeof(loadSavedLabel),
                            "Load Saved",
                            "values_load_saved",
                            HOTKEY_POSITION_LOAD);
    FormatHotkeyButtonLabel(roundStartLabel,
                            sizeof(roundStartLabel),
                            "Round Start",
                            "values_round_start",
                            HOTKEY_POSITION_LOAD,
                            p1UpBinding);
    FormatHotkeyButtonLabel(midScreenLabel,
                            sizeof(midScreenLabel),
                            "Mid Screen",
                            "values_mid_screen",
                            HOTKEY_POSITION_LOAD,
                            p1DownBinding);
    FormatPositionPresetHint(positionHint, sizeof(positionHint));

    if (ImGui::BeginTable("PracticeValues", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        RenderPlayerValueEditor(0, snapshots[0]);
        ImGui::TableNextColumn();
        RenderPlayerValueEditor(1, snapshots[1]);
        ImGui::EndTable();
    }

    ImGui::Separator();

    ImGui::BeginDisabled(!canUsePositionTools);
    if (ImGui::Button(savePositionLabel)) {
        if (SavePositionSnapshot(snapshots)) {
            PushToast("Position saved", IM_COL32(120, 220, 255, 255));
        } else {
            PushToast("Position save unavailable", IM_COL32(255, 180, 120, 255));
        }
    }
    ImGui::SameLine();
    if (s_positionSnapshot.valid) {
        if (ImGui::Button(loadSavedLabel)) {
            if (LoadPositionSnapshot()) {
                PushToast("Saved position restored", IM_COL32(120, 255, 180, 255));
            }
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(loadSavedLabel);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(roundStartLabel)) {
        if (ApplyPositionPreset(kRoundStartPositionPreset)) {
            PushToast("Round Start", IM_COL32(120, 255, 180, 255));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(midScreenLabel)) {
        if (ApplyPositionPreset(kMidScreenPositionPreset)) {
            PushToast("Mid Screen", IM_COL32(120, 255, 180, 255));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Swap Sides")) {
        SwapPlayerPositions(snapshots);
        PushToast("Positions swapped", IM_COL32(255, 220, 120, 255));
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", positionHint);
    if (!canUsePositionTools) {
        ImGui::TextDisabled("Position tools unlock after the round intro ends.");
    }
}

static void RenderOptionsTab(void) {
    const bool nativeHealthActive = ReadNativeTrainingSetting(ADDR_TRAINING_HEALTH_REGEN_SETTING, 10) != 0;
    const bool nativeMeterActive = ReadNativeTrainingSetting(ADDR_TRAINING_METER_LEVEL_SETTING, 9) != 0;

    ImGui::SeparatorText("Vanilla");
    RenderNativeTrainingCombo(
        "Health Regeneration",
        ADDR_TRAINING_HEALTH_REGEN_SETTING,
        10,
        kNativeHealthLabels,
        IM_ARRAYSIZE(kNativeHealthLabels),
        "Uses the game's native training HP refill, applied every frame to both players.");
    RenderNativeTrainingCombo(
        "Meter Level",
        ADDR_TRAINING_METER_LEVEL_SETTING,
        9,
        kNativeMeterLabels,
        IM_ARRAYSIZE(kNativeMeterLabels),
        "Uses the game's native meter refill, applied every frame to both players.");
    if (nativeHealthActive || nativeMeterActive) {
        ImGui::TextDisabled("Native Health/Meter settings override the overlapping custom recovery rows below while enabled.");
    }

    ImGui::SeparatorText("Auto-Recovery");

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        RecoveryConfig& recovery = s_practiceConfig.recovery[player];
        ImGui::PushID(player);
        if (ImGui::TreeNodeEx(SideLabel(player), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(nativeHealthActive);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("HP", &recovery.hpMode, kRecoveryHpLabels, IM_ARRAYSIZE(kRecoveryHpLabels));
            if (recovery.hpMode == RECOVERY_HP_CUSTOM) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("##HPVal", &recovery.hpCustom);
                recovery.hpCustom = ClampInt(recovery.hpCustom, 0, 50000);
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(nativeMeterActive);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Meter", &recovery.meterMode, kRecoveryMeterLabels, IM_ARRAYSIZE(kRecoveryMeterLabels));
            if (recovery.meterMode == RECOVERY_METER_CUSTOM) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("##MeterVal", &recovery.meterCustom);
                recovery.meterCustom = ClampInt(recovery.meterCustom, 0, kMeterMax);
            }
            ImGui::EndDisabled();

            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Guard", &recovery.guardMode, kRecoveryGuardLabels, IM_ARRAYSIZE(kRecoveryGuardLabels));
            if (recovery.guardMode == RECOVERY_GUARD_CUSTOM) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("##GuardVal", &recovery.guardCustom);
                recovery.guardCustom = ClampInt(recovery.guardCustom, 0, kGuardGaugeMax);
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::Checkbox("Require Both Neutral", &s_practiceConfig.recoveryRequireBothNeutral);
    ImGui::SameLine(); HelpMarker("Only restore values when both players are idle.");
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Recovery Delay (frames)", &s_practiceConfig.recoveryDelayFrames);
    s_practiceConfig.recoveryDelayFrames = ClampInt(s_practiceConfig.recoveryDelayFrames, 0, 120);

    ImGui::SeparatorText("Frame Advantage");
    FrameAdvantage_RenderImGui();
}

static const char* kTriggerDescriptions[] = {
    "Fires when the target exits blockstun and becomes actionable.",
    "Fires when the target enters untechable knockdown recovery.",
    "Fires when the target exits hitstun (becomes actionable or techs).",
    "Fires when the target begins air tech recovery.",
    "Fires when the target begins ground tech recovery.",
};

static void RenderTriggerSlot(int triggerId) {
    TriggerConfig& cfg = s_practiceConfig.triggers[triggerId];
    ImGui::PushID(triggerId);

    ImGui::Checkbox("##en", &cfg.enabled);
    ImGui::SameLine();
    ImGui::TextUnformatted(kTriggerLabels[triggerId]);
    ImGui::SameLine();
    HelpMarker(kTriggerDescriptions[triggerId]);

    if (cfg.enabled) {
        ImGui::Indent(24.0f);

        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("Motion", &cfg.actionKind, kScriptActionLabels, IM_ARRAYSIZE(kScriptActionLabels));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        ImGui::Combo("+##btn", &cfg.actionButton, kScriptButtonLabels, IM_ARRAYSIZE(kScriptButtonLabels));

        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("Delay (f)", &cfg.delayFrames);
        cfg.delayFrames = ClampInt(cfg.delayFrames, 0, 120);

        ImGui::Unindent(24.0f);
    }

    ImGui::PopID();
}

static void RenderTriggersTab(void) {
    ImGui::Checkbox("Enable Triggers", &s_practiceConfig.triggerMasterEnabled);

    if (!s_practiceConfig.triggerMasterEnabled) {
        ImGui::TextDisabled("Enable triggers to configure event-driven actions.");
        return;
    }

    ImGui::SetNextItemWidth(100.0f);
    ImGui::Combo("Target", &s_practiceConfig.triggerTarget, kTriggerTargetLabels, IM_ARRAYSIZE(kTriggerTargetLabels));
    ImGui::SameLine(); HelpMarker("Which player the trigger actions apply to.");

    ImGui::Checkbox("Random 50% Skip", &s_practiceConfig.triggerRandomize);
    ImGui::SameLine(); HelpMarker("Each activation has a 50% chance of being skipped for mix-up variation.");

    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("Wake Buffer (f)", &s_practiceConfig.wakeBufferFrames);
    s_practiceConfig.wakeBufferFrames = ClampInt(s_practiceConfig.wakeBufferFrames, 0, 30);
    ImGui::SameLine(); HelpMarker(
        "For wakeup and tech triggers, pre-inputs the action this many frames\n"
        "before the target becomes actionable.");

    ImGui::Separator();

    for (int triggerId = 0; triggerId < kTriggerCount; ++triggerId) {
        RenderTriggerSlot(triggerId);
        if (triggerId + 1 < kTriggerCount) {
            ImGui::Spacing();
        }
    }
}

static bool HasComboOverlayContent(void) {
    if (!s_practiceConfig.comboOverlayEnabled) {
        return false;
    }

    for (int attacker = 0; attacker < kPracticePlayerCount; ++attacker) {
        if (s_comboTrackers[attacker].active || s_comboTrackers[attacker].last.valid) {
            return true;
        }
    }

    return false;
}

static void RenderComboOverlayPanel(ImDrawList* dl,
                                    const ImVec2& topLeft,
                                    int attacker,
                                    const PlayerSnapshot snapshots[kPracticePlayerCount]) {
    const ComboTracker& tracker = s_comboTrackers[attacker];
    if (!tracker.active && !tracker.last.valid) {
        return;
    }

    const PlayerSnapshot& attackerSnapshot = snapshots[attacker];
    const PlayerSnapshot& defenderSnapshot = snapshots[1 - attacker];

    const uint16_t hits = tracker.active ? tracker.hits : tracker.last.hits;
    const int damage = tracker.active ? tracker.damage : tracker.last.damage;
    const int atkMeter = tracker.active ? tracker.attackerMeterDelta : tracker.last.attackerMeterDelta;
    const int defMeter = tracker.active ? tracker.defenderMeterDelta : tracker.last.defenderMeterDelta;
    const uint32_t defenderCharId = tracker.active ? defenderSnapshot.charId : tracker.last.defenderCharId;
    const uint16_t defenderMaxHp = tracker.active ? defenderSnapshot.maxHp : tracker.last.defenderMaxHp;
    const int attackerTier = ComputeHpTierIndex(attackerSnapshot.hp, attackerSnapshot.maxHp);
    const int defenderTier = ComputeHpTierIndex(defenderSnapshot.hp, defenderMaxHp);

    // Line 1: header
    // Line 2: hits + damage
    // Line 3: signed meter delta for both players
    // Line 4: defender character + weight
    // Line 5: HP tiers
    char line1[48];
    char line2[96];
    char line3[96];
    char line4[96];
    char line5[96];
    snprintf(line1, sizeof(line1), "%s Combo", attacker == 0 ? "P1" : "P2");
    snprintf(line2, sizeof(line2), "%u hits  |  %d dmg", hits, damage);
    snprintf(line3, sizeof(line3), "Atk %+d meter  |  Def %+d meter", atkMeter, defMeter);
    snprintf(line4, sizeof(line4), "vs %s  (%s)",
             GetCharacterName(defenderCharId),
             GetWeightClassLabel(defenderCharId));
    snprintf(line5, sizeof(line5), "Atk tier %d  |  Def tier %d", attackerTier, defenderTier);

    const float width = 240.0f;
    const float lineHeight = ImGui::GetTextLineHeight();
    const float height = lineHeight * 5.0f + 18.0f;
    dl->AddRectFilled(topLeft,
                      ImVec2(topLeft.x + width, topLeft.y + height),
                      IM_COL32(0, 0, 0, 180),
                      4.0f);
    dl->AddText(ImVec2(topLeft.x + 8.0f, topLeft.y + 6.0f),                     IM_COL32(255, 255, 255, 255), line1);
    dl->AddText(ImVec2(topLeft.x + 8.0f, topLeft.y + 6.0f + lineHeight),        IM_COL32(255, 230, 160, 255), line2);
    dl->AddText(ImVec2(topLeft.x + 8.0f, topLeft.y + 6.0f + lineHeight * 2.0f), IM_COL32(120, 220, 255, 255), line3);
    dl->AddText(ImVec2(topLeft.x + 8.0f, topLeft.y + 6.0f + lineHeight * 3.0f), IM_COL32(180, 220, 255, 255), line4);
    dl->AddText(ImVec2(topLeft.x + 8.0f, topLeft.y + 6.0f + lineHeight * 4.0f), IM_COL32(180, 255, 180, 255), line5);
}

} // namespace

// ============================================================================
// Settings bridge for the pause menu
// ============================================================================
// The menu renders rows; every range, label table and side effect stays here so
// the two surfaces can never disagree about what a setting means.

namespace {

struct SettingDesc {
    const char* label;
    const char* const* values;
    int valueCount;
    // Native settings live in game memory, mod settings in s_practiceConfig.
    uintptr_t nativeAddress;
};

int ClampCycle(int value, int count, int delta) {
    if (count <= 1) {
        return 0;
    }
    int next = (value + delta) % count;
    if (next < 0) {
        next += count;
    }
    return next;
}

const char* const kOnOffLabels[] = { "Off", "On" };

// Only responses with a traced legal-input path. Everything else the engine
// supports needs per-character command arming that is not mapped yet.
// The HUD rows are one contiguous range mapping straight onto HudElement, so
// they are handled ahead of each setting switch instead of as nine cases in
// four different places.
bool IsHudRow(int setting) {
    return setting >= PRACTICE_SET_HUD_FIRST && setting <= PRACTICE_SET_HUD_LAST;
}

int HudRowElement(int setting) {
    return setting - PRACTICE_SET_HUD_FIRST;
}

const char* const kDefensiveResponseLabels[] = {
    "Guard Only", "Native", "Just Parry", "Dodge", "Repel", "Counter",
    "Perfect Parry", "Absolute Def", "Parry",
};

// Only what the dummy's own category can perform. dword_73E070 gives each
// character exactly one mechanic, so the row is Guard Only / Native / that one -
// listing the other four would offer moves the engine will never produce.
// Categories 4 and 5 add nothing: push-away needs no input at all and absolute
// defence has none to give.
int DefensiveResponseOptions(int* out, int max) {
    int count = 0;
    if (count < max) out[count++] = (int)Training::DefensiveResponse::NormalGuard;
    if (count < max) out[count++] = (int)Training::DefensiveResponse::CharacterNative;

    // Every response the category can actually perform, not just its default:
    // push block has two, and which one you get is a timing difference worth
    // being able to pick.
    const int category = PracticeDefense_DummyDefenseCategory();
    for (int r = 0; r < (int)Training::DefensiveResponse::Count; ++r) {
        const Training::DefensiveResponse response = (Training::DefensiveResponse)r;
        if (response == Training::DefensiveResponse::NormalGuard ||
            response == Training::DefensiveResponse::CharacterNative) {
            continue;
        }
        if (Training::ResponseSupportedByCategory(response, category) && count < max) {
            out[count++] = r;
        }
    }

    // One line whenever the offered set changes. "The mechanic is not in the
    // menu" and "the mechanic is in the menu and does nothing" are different
    // bugs, and without this there is no way to tell them apart from a log.
    static int s_lastCategory = -1;
    static int s_lastCount = -1;
    if (category != s_lastCategory || count != s_lastCount) {
        s_lastCategory = category;
        s_lastCount = count;
        char list[192] = {};
        size_t used = 0;
        for (int i = 0; i < count && used + 24 < sizeof(list); ++i) {
            used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s",
                                     i ? ", " : "",
                                     Training::DefensiveResponseLabel(
                                         (Training::DefensiveResponse)out[i]));
        }
        LOG_INFO("[Practice] Defense options: category=%d count=%d [%s]",
                 category, count, list);
    }
    return count;
}

// Sized to the enum, not to "however many I expected". A category that gains a
// response - push block already has two - must never be silently truncated,
// because the symptom is a mechanic quietly missing from the menu with nothing
// logged.
constexpr int kDefensiveResponseMaxOptions = (int)Training::DefensiveResponse::Count;

// Storage stays the enum value, so a character swap cannot silently repoint the
// setting at a different mechanic; the row just shows the nearest valid entry.
int DefensiveResponseIndex() {
    int options[kDefensiveResponseMaxOptions];
    const int count = DefensiveResponseOptions(options, kDefensiveResponseMaxOptions);
    for (int i = 0; i < count; ++i) {
        if (options[i] == g_defensiveResponse) {
            return i;
        }
    }
    return 0;
}

// Recovery delay and jump cadence are stepped through presets rather than typed:
// a d-pad is a bad number editor, and the exact values stay in the ImGui panel.
const int kDelayPresets[] = { 0, 5, 10, 20, 40 };
const char* const kDelayLabels[] = { "0 f", "5 f", "10 f", "20 f", "40 f" };
const int kCadencePresets[] = { 10, 20, 30, 45, 60 };
const char* const kCadenceLabels[] = { "10 f", "20 f", "30 f", "45 f", "60 f" };
const int kWakeBufferPresets[] = { 0, 1, 3, 5, 8 };
const char* const kWakeBufferLabels[] = { "0 f", "1 f", "3 f", "5 f", "8 f" };

int PresetIndex(const int* presets, int count, int value) {
    for (int i = 0; i < count; ++i) {
        if (presets[i] == value) {
            return i;
        }
    }
    return 0;
}

// Which trigger slot the popup is editing. Menu-local, so it lives with the
// rest of the bridge rather than in the shared config.
int g_menuTriggerSlot = 0;

int TriggerRowIndex(int setting) {
    return setting - PRACTICE_SET_TRIGGER_1;
}

bool IsTriggerRow(int setting) {
    return setting >= PRACTICE_SET_TRIGGER_1 && setting <= PRACTICE_SET_TRIGGER_5;
}

TriggerConfig& MenuTrigger() {
    return s_practiceConfig.triggers[ClampInt(g_menuTriggerSlot, 0, kTriggerCount - 1)];
}

// The full names read well in a wide ImGui panel but overflow a 256 px column.
const char* const kShortTriggerLabels[] = {
    "Block", "Wakeup", "Hitstun", "Air Tech", "Ground Tech",
};

} // namespace

int PracticeSetting_Get(int setting) {
    if (IsHudRow(setting)) {
        // Index 0 = Shown, 1 = Hidden.
        return HudToggle_IsHidden(HudRowElement(setting)) ? 1 : 0;
    }
    switch (setting) {
        case PRACTICE_SET_DUMMY_BACKEND:  return s_practiceConfig.dummyControlMode;
        case PRACTICE_SET_BLOCK_MODE:     return s_practiceConfig.blockMode;
        case PRACTICE_SET_STANCE:         return s_practiceConfig.stanceMode;
        case PRACTICE_SET_JUMP_MODE:      return s_practiceConfig.jumpMode;
        case PRACTICE_SET_DEFENSIVE_RESPONSE: return DefensiveResponseIndex();
        case PRACTICE_SET_JUMP_CADENCE:
            return PresetIndex(kCadencePresets, (int)(sizeof(kCadencePresets) / sizeof(int)),
                               s_practiceConfig.jumpCadenceFrames);

        case PRACTICE_SET_NATIVE_CPU:
            return ReadNativeTrainingSetting(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE, 1);
        case PRACTICE_SET_NATIVE_AIR_TECH:
            return ReadNativeTrainingSetting(ADDR_TRAINING_AIR_TECH_SETTING, 4);
        case PRACTICE_SET_NATIVE_GROUND_TECH:
            return ReadNativeTrainingSetting(ADDR_TRAINING_GROUND_TECH_SETTING, 3);
        case PRACTICE_SET_NATIVE_BLOCK_TYPE:
            return ReadNativeTrainingSetting(ADDR_TRAINING_BLOCK_TYPE_SETTING, 2);
        case PRACTICE_SET_NATIVE_DUMMY_STATE:
            return ReadNativeTrainingSetting(ADDR_TRAINING_DUMMY_STATE_SETTING, 3);
        case PRACTICE_SET_HEALTH_REGEN:
            return ReadNativeTrainingSetting(ADDR_TRAINING_HEALTH_REGEN_SETTING, 10);
        case PRACTICE_SET_METER_LEVEL:
            return ReadNativeTrainingSetting(ADDR_TRAINING_METER_LEVEL_SETTING, 9);
        case PRACTICE_SET_INPUT_DISPLAY:
            return ReadNativeTrainingSetting(ADDR_TRAINING_INPUT_DISPLAY, 1);
        case PRACTICE_SET_DAMAGE_DISPLAY:
            return ReadNativeTrainingSetting(ADDR_TRAINING_DAMAGE_DISPLAY, 1);

        case PRACTICE_SET_RECOVERY_HP:    return s_practiceConfig.recovery[1].hpMode;
        case PRACTICE_SET_RECOVERY_METER: return s_practiceConfig.recovery[1].meterMode;
        case PRACTICE_SET_RECOVERY_GUARD: return s_practiceConfig.recovery[1].guardMode;
        case PRACTICE_SET_RECOVERY_BOTH_NEUTRAL:
            return s_practiceConfig.recoveryRequireBothNeutral ? 1 : 0;
        case PRACTICE_SET_RECOVERY_DELAY:
            return PresetIndex(kDelayPresets, (int)(sizeof(kDelayPresets) / sizeof(int)),
                               s_practiceConfig.recoveryDelayFrames);
        case PRACTICE_SET_RECOVERY_HP_VALUE:    return s_practiceConfig.recovery[1].hpCustom;
        case PRACTICE_SET_RECOVERY_METER_VALUE: return s_practiceConfig.recovery[1].meterCustom;
        case PRACTICE_SET_RECOVERY_GUARD_VALUE: return s_practiceConfig.recovery[1].guardCustom;

        case PRACTICE_SET_TRIGGERS_ENABLED: return s_practiceConfig.triggerMasterEnabled ? 1 : 0;
        case PRACTICE_SET_TRIGGER_TARGET:   return s_practiceConfig.triggerTarget;
        case PRACTICE_SET_TRIGGER_RANDOM:   return s_practiceConfig.triggerRandomize ? 1 : 0;
        case PRACTICE_SET_WAKE_BUFFER:
            return PresetIndex(kWakeBufferPresets, (int)(sizeof(kWakeBufferPresets) / sizeof(int)),
                               s_practiceConfig.wakeBufferFrames);

        case PRACTICE_SET_TRIGGER_BUTTON:  return MenuTrigger().actionButton;
        case PRACTICE_SET_TRIGGER_DELAY:
            return PresetIndex(kDelayPresets, (int)(sizeof(kDelayPresets) / sizeof(int)),
                               MenuTrigger().delayFrames);

        case PRACTICE_SET_HITBOXES:        return HitboxViewer_IsEnabled() ? 1 : 0;
        case PRACTICE_SET_COMBO_OVERLAY:   return s_practiceConfig.comboOverlayEnabled ? 1 : 0;
        case PRACTICE_SET_FRAME_ADVANTAGE: return FrameAdvantage_IsEnabled() ? 1 : 0;
        case PRACTICE_SET_CONTROL_SWAP:    return PracticeTools_IsControlSwapped() ? 1 : 0;
        case PRACTICE_SET_MACRO_SLOT:      return InputMacro_GetCurrentSlot();
        case PRACTICE_SET_PAUSED:          return PracticeTools_IsPaused() ? 1 : 0;

        default:
            if (IsTriggerRow(setting)) {
                return s_practiceConfig.triggers[TriggerRowIndex(setting)].enabled ? 1 : 0;
            }
            return 0;
    }
}

static int PracticeSetting_ValueCount(int setting) {
    if (IsHudRow(setting)) {
        return 2;
    }
    switch (setting) {
        case PRACTICE_SET_DUMMY_BACKEND:  return IM_ARRAYSIZE(kDummyControlModeLabels);
        case PRACTICE_SET_BLOCK_MODE:     return IM_ARRAYSIZE(kBlockModeLabels);
        case PRACTICE_SET_STANCE:         return IM_ARRAYSIZE(kStanceModeLabels);
        case PRACTICE_SET_JUMP_MODE:      return IM_ARRAYSIZE(kJumpModeLabels);
        case PRACTICE_SET_JUMP_CADENCE:   return (int)(sizeof(kCadencePresets) / sizeof(int));
        case PRACTICE_SET_DEFENSIVE_RESPONSE: {
            int options[kDefensiveResponseMaxOptions];
            return DefensiveResponseOptions(options, kDefensiveResponseMaxOptions);
        }
        case PRACTICE_SET_NATIVE_CPU:     return IM_ARRAYSIZE(kNativeCpuLabels);
        case PRACTICE_SET_NATIVE_AIR_TECH:     return IM_ARRAYSIZE(kNativeAirTechLabels);
        case PRACTICE_SET_NATIVE_GROUND_TECH:  return IM_ARRAYSIZE(kNativeGroundTechLabels);
        case PRACTICE_SET_NATIVE_BLOCK_TYPE:   return IM_ARRAYSIZE(kNativeBlockTypeLabels);
        case PRACTICE_SET_NATIVE_DUMMY_STATE:  return IM_ARRAYSIZE(kNativeDummyStateLabels);
        case PRACTICE_SET_HEALTH_REGEN:   return IM_ARRAYSIZE(kNativeHealthLabels);
        case PRACTICE_SET_METER_LEVEL:    return IM_ARRAYSIZE(kNativeMeterLabels);
        case PRACTICE_SET_RECOVERY_HP:    return IM_ARRAYSIZE(kRecoveryHpLabels);
        case PRACTICE_SET_RECOVERY_METER: return IM_ARRAYSIZE(kRecoveryMeterLabels);
        case PRACTICE_SET_RECOVERY_GUARD: return IM_ARRAYSIZE(kRecoveryGuardLabels);
        case PRACTICE_SET_RECOVERY_DELAY: return (int)(sizeof(kDelayPresets) / sizeof(int));
        case PRACTICE_SET_TRIGGER_TARGET: return IM_ARRAYSIZE(kTriggerTargetLabels);
        case PRACTICE_SET_WAKE_BUFFER:    return (int)(sizeof(kWakeBufferPresets) / sizeof(int));
        case PRACTICE_SET_TRIGGER_BUTTON: return IM_ARRAYSIZE(kScriptButtonLabels);
        case PRACTICE_SET_TRIGGER_DELAY:  return (int)(sizeof(kDelayPresets) / sizeof(int));
        case PRACTICE_SET_MACRO_SLOT:     return MACRO_MAX_SLOTS;
        default:                          return 2;   // on/off
    }
}

void PracticeSetting_Cycle(int setting, int delta) {
    if (IsHudRow(setting)) {
        HudToggle_SetHidden(HudRowElement(setting),
                            !HudToggle_IsHidden(HudRowElement(setting)));
        return;
    }
    if (delta == 0 || !PracticeSetting_Enabled(setting)) {
        return;
    }
    const int count = PracticeSetting_ValueCount(setting);
    const int next = ClampCycle(PracticeSetting_Get(setting), count, delta);

    switch (setting) {
        case PRACTICE_SET_DUMMY_BACKEND:  s_practiceConfig.dummyControlMode = next; break;
        case PRACTICE_SET_BLOCK_MODE:     s_practiceConfig.blockMode = next; break;
        case PRACTICE_SET_STANCE:         s_practiceConfig.stanceMode = next; break;
        case PRACTICE_SET_JUMP_MODE:      s_practiceConfig.jumpMode = next; break;
        case PRACTICE_SET_JUMP_CADENCE:   s_practiceConfig.jumpCadenceFrames = kCadencePresets[next]; break;
        case PRACTICE_SET_DEFENSIVE_RESPONSE: {
            int options[kDefensiveResponseMaxOptions];
            const int count = DefensiveResponseOptions(options, kDefensiveResponseMaxOptions);
            g_defensiveResponse = options[ClampInt(next, 0, count - 1)];
            break;
        }

        case PRACTICE_SET_NATIVE_CPU:
            WriteNativeTrainingSetting(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE, next, 1, "cpu"); break;
        case PRACTICE_SET_NATIVE_AIR_TECH:
            WriteNativeTrainingSetting(ADDR_TRAINING_AIR_TECH_SETTING, next, 4, "air_tech"); break;
        case PRACTICE_SET_NATIVE_GROUND_TECH:
            WriteNativeTrainingSetting(ADDR_TRAINING_GROUND_TECH_SETTING, next, 3, "ground_tech"); break;
        case PRACTICE_SET_NATIVE_BLOCK_TYPE:
            WriteNativeTrainingSetting(ADDR_TRAINING_BLOCK_TYPE_SETTING, next, 2, "block_type"); break;
        case PRACTICE_SET_NATIVE_DUMMY_STATE:
            WriteNativeTrainingSetting(ADDR_TRAINING_DUMMY_STATE_SETTING, next, 3, "dummy_state"); break;
        case PRACTICE_SET_HEALTH_REGEN:
            WriteNativeTrainingSetting(ADDR_TRAINING_HEALTH_REGEN_SETTING, next, 10, "health_regen"); break;
        case PRACTICE_SET_METER_LEVEL:
            WriteNativeTrainingSetting(ADDR_TRAINING_METER_LEVEL_SETTING, next, 9, "meter_level"); break;
        case PRACTICE_SET_INPUT_DISPLAY:
            WriteNativeTrainingSetting(ADDR_TRAINING_INPUT_DISPLAY, next, 1, "input_display"); break;
        case PRACTICE_SET_DAMAGE_DISPLAY:
            WriteNativeTrainingSetting(ADDR_TRAINING_DAMAGE_DISPLAY, next, 1, "damage_display"); break;

        // Recovery applies to both sides from this menu; the per-player split
        // stays in the ImGui panel.
        case PRACTICE_SET_RECOVERY_HP:
            for (int p = 0; p < kPracticePlayerCount; ++p) s_practiceConfig.recovery[p].hpMode = next;
            break;
        case PRACTICE_SET_RECOVERY_METER:
            for (int p = 0; p < kPracticePlayerCount; ++p) s_practiceConfig.recovery[p].meterMode = next;
            break;
        case PRACTICE_SET_RECOVERY_GUARD:
            for (int p = 0; p < kPracticePlayerCount; ++p) s_practiceConfig.recovery[p].guardMode = next;
            break;
        case PRACTICE_SET_RECOVERY_BOTH_NEUTRAL:
            s_practiceConfig.recoveryRequireBothNeutral = (next != 0); break;
        case PRACTICE_SET_RECOVERY_DELAY:
            s_practiceConfig.recoveryDelayFrames = kDelayPresets[next]; break;

        case PRACTICE_SET_TRIGGERS_ENABLED:
            s_practiceConfig.triggerMasterEnabled = (next != 0); break;
        case PRACTICE_SET_TRIGGER_TARGET: s_practiceConfig.triggerTarget = next; break;
        case PRACTICE_SET_TRIGGER_RANDOM: s_practiceConfig.triggerRandomize = (next != 0); break;
        case PRACTICE_SET_WAKE_BUFFER:
            s_practiceConfig.wakeBufferFrames = kWakeBufferPresets[next]; break;

        case PRACTICE_SET_TRIGGER_BUTTON:  MenuTrigger().actionButton = next; break;
        case PRACTICE_SET_TRIGGER_DELAY:   MenuTrigger().delayFrames = kDelayPresets[next]; break;

        case PRACTICE_SET_HITBOXES:        HitboxViewer_SetEnabled(next != 0); break;
        case PRACTICE_SET_COMBO_OVERLAY:   s_practiceConfig.comboOverlayEnabled = (next != 0); break;
        case PRACTICE_SET_FRAME_ADVANTAGE: FrameAdvantage_SetEnabled(next != 0); break;
        case PRACTICE_SET_CONTROL_SWAP:    PracticeTools_ApplyControlSwapState(next != 0); break;
        case PRACTICE_SET_MACRO_SLOT:
            // InputMacro_NextSlot refuses to move while recording or replaying,
            // so this must be bounded - a while-until-equal spins the game
            // thread forever the moment the macro system declines.
            for (int guard = 0; guard < MACRO_MAX_SLOTS; ++guard) {
                if (InputMacro_GetCurrentSlot() == next) {
                    break;
                }
                const int before = InputMacro_GetCurrentSlot();
                InputMacro_NextSlot();
                if (InputMacro_GetCurrentSlot() == before) {
                    break;   // declined; leave the slot where it is
                }
            }
            break;
        case PRACTICE_SET_PAUSED:          PracticeTools_SetPaused(next != 0); break;

        // Numeric rows step by a useful increment when cycled; typing is the
        // precise path.
        case PRACTICE_SET_RECOVERY_HP_VALUE:
        case PRACTICE_SET_RECOVERY_METER_VALUE:
        case PRACTICE_SET_RECOVERY_GUARD_VALUE: {
            const int step = (setting == PRACTICE_SET_RECOVERY_HP_VALUE) ? 100 : 500;
            PracticeSetting_SetNumeric(setting, PracticeSetting_Get(setting) + delta * step);
            break;
        }

        default:
            if (IsTriggerRow(setting)) {
                s_practiceConfig.triggers[TriggerRowIndex(setting)].enabled = (next != 0);
            }
            break;
    }

    // Every change, named, after it has been applied. "This setting does
    // nothing" and "the menu never reached this setting" are different bugs,
    // and without a line here they look identical in a log.
    LOG_INFO("[Practice] Setting '%s' -> '%s' (index %d of %d)",
             PracticeSetting_Label(setting),
             PracticeSetting_ValueText(setting),
             next, count);
}

const char* PracticeSetting_Label(int setting) {
    if (IsHudRow(setting)) {
        return HudToggle_Name(HudRowElement(setting));
    }
    switch (setting) {
        case PRACTICE_SET_DUMMY_BACKEND:  return "Type";
        case PRACTICE_SET_BLOCK_MODE:     return "Auto-Block";
        case PRACTICE_SET_STANCE:         return "Stance";
        case PRACTICE_SET_JUMP_MODE:      return "Auto-Jump";
        case PRACTICE_SET_JUMP_CADENCE:   return "Jump Rate";
        case PRACTICE_SET_DEFENSIVE_RESPONSE: return "Defense";
        case PRACTICE_SET_NATIVE_CPU:     return "CPU";
        case PRACTICE_SET_NATIVE_AIR_TECH:    return "Air Tech";
        case PRACTICE_SET_NATIVE_GROUND_TECH: return "Ground Tech";
        case PRACTICE_SET_NATIVE_BLOCK_TYPE:  return "Guard";
        case PRACTICE_SET_NATIVE_DUMMY_STATE: return "State";
        case PRACTICE_SET_RECOVERY_HP:    return "Health";
        case PRACTICE_SET_RECOVERY_METER: return "Meter";
        case PRACTICE_SET_RECOVERY_GUARD: return "Guard";
        case PRACTICE_SET_RECOVERY_BOTH_NEUTRAL: return "Both Neutral";
        case PRACTICE_SET_RECOVERY_DELAY: return "Delay";
        case PRACTICE_SET_TRIGGERS_ENABLED: return "Triggers";
        case PRACTICE_SET_TRIGGER_TARGET:   return "Target";
        case PRACTICE_SET_TRIGGER_RANDOM:   return "Random Skip";
        case PRACTICE_SET_WAKE_BUFFER:      return "Wake Buffer";
        case PRACTICE_SET_HITBOXES:         return "Hitboxes";
        case PRACTICE_SET_COMBO_OVERLAY:    return "Combo";
        case PRACTICE_SET_INPUT_DISPLAY:    return "Inputs";
        case PRACTICE_SET_DAMAGE_DISPLAY:   return "Damage";
        case PRACTICE_SET_FRAME_ADVANTAGE:  return "Frame Adv";
        case PRACTICE_SET_HEALTH_REGEN:     return "Regen";
        case PRACTICE_SET_METER_LEVEL:      return "Meter Level";
        case PRACTICE_SET_TRIGGER_BUTTON:   return "Button";
        case PRACTICE_SET_TRIGGER_DELAY:    return "Delay";
        case PRACTICE_SET_CONTROL_SWAP:     return "Control P2";
        case PRACTICE_SET_MACRO_SLOT:       return "Macro Slot";
        case PRACTICE_SET_PAUSED:           return "Freeze";
        case PRACTICE_SET_RECOVERY_HP_VALUE:    return "HP Value";
        case PRACTICE_SET_RECOVERY_METER_VALUE: return "Meter Value";
        case PRACTICE_SET_RECOVERY_GUARD_VALUE: return "Guard Value";
        default:
            if (IsTriggerRow(setting)) {
                return kShortTriggerLabels[TriggerRowIndex(setting)];
            }
            return "?";
    }
}

const char* PracticeSetting_ValueText(int setting) {
    if (IsHudRow(setting)) {
        return HudToggle_IsHidden(HudRowElement(setting)) ? "Hidden" : "Shown";
    }
    const int value = PracticeSetting_Get(setting);
    const int count = PracticeSetting_ValueCount(setting);
    const int idx = (value >= 0 && value < count) ? value : 0;

    switch (setting) {
        case PRACTICE_SET_DUMMY_BACKEND:  return kDummyControlModeLabels[idx];
        case PRACTICE_SET_BLOCK_MODE:
            // A guard cancel cannot happen without blockstun, so picking one
            // turns blocking on underneath. Say so instead of showing "None".
            return PracticeDefense_BlockForcedByResponse() ? "All (defense)"
                                                           : kBlockModeLabels[idx];
        case PRACTICE_SET_STANCE:         return kStanceModeLabels[idx];
        case PRACTICE_SET_JUMP_MODE:      return kJumpModeLabels[idx];
        case PRACTICE_SET_JUMP_CADENCE:   return kCadenceLabels[idx];
        case PRACTICE_SET_DEFENSIVE_RESPONSE: {
            int options[kDefensiveResponseMaxOptions];
            const int count = DefensiveResponseOptions(options, kDefensiveResponseMaxOptions);
            return kDefensiveResponseLabels[options[ClampInt(idx, 0, count - 1)]];
        }
        case PRACTICE_SET_NATIVE_CPU:     return kNativeCpuLabels[idx];
        case PRACTICE_SET_NATIVE_AIR_TECH:    return kNativeAirTechLabels[idx];
        case PRACTICE_SET_NATIVE_GROUND_TECH: return kNativeGroundTechLabels[idx];
        case PRACTICE_SET_NATIVE_BLOCK_TYPE:  return kNativeBlockTypeLabels[idx];
        case PRACTICE_SET_NATIVE_DUMMY_STATE: return kNativeDummyStateLabels[idx];
        case PRACTICE_SET_HEALTH_REGEN:   return kNativeHealthLabels[idx];
        case PRACTICE_SET_METER_LEVEL:    return kNativeMeterLabels[idx];
        case PRACTICE_SET_RECOVERY_HP:    return kRecoveryHpLabels[idx];
        case PRACTICE_SET_RECOVERY_METER: return kRecoveryMeterLabels[idx];
        case PRACTICE_SET_RECOVERY_GUARD: return kRecoveryGuardLabels[idx];
        case PRACTICE_SET_RECOVERY_DELAY: return kDelayLabels[idx];
        case PRACTICE_SET_TRIGGER_TARGET: return kTriggerTargetLabels[idx];
        case PRACTICE_SET_WAKE_BUFFER:    return kWakeBufferLabels[idx];
        case PRACTICE_SET_TRIGGER_BUTTON: return kScriptButtonLabels[idx];
        case PRACTICE_SET_TRIGGER_DELAY:  return kDelayLabels[idx];
        case PRACTICE_SET_RECOVERY_HP_VALUE:
        case PRACTICE_SET_RECOVERY_METER_VALUE:
        case PRACTICE_SET_RECOVERY_GUARD_VALUE: {
            static char numText[24];
            snprintf(numText, sizeof(numText), "%d", PracticeSetting_Get(setting));
            return numText;
        }
        case PRACTICE_SET_MACRO_SLOT: {
            static char slotText[24];
            snprintf(slotText, sizeof(slotText), "%d%s", idx + 1,
                     InputMacro_SlotHasData(idx) ? " *" : "");
            return slotText;
        }
        default:
            // A trigger row shows its configured action, so the whole set reads
            // at a glance without opening anything.
            if (IsTriggerRow(setting)) {
                return PracticeTrigger_Summary(TriggerRowIndex(setting));
            }
            return kOnOffLabels[idx != 0 ? 1 : 0];
    }
}

bool PracticeSetting_Enabled(int setting) {
    const bool advanced = s_practiceConfig.dummyControlMode == DUMMY_CONTROL_MOD;
    const bool cpuOn = ReadNativeTrainingSetting(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE, 1) != 0;

    switch (setting) {
        case PRACTICE_SET_BLOCK_MODE:
        case PRACTICE_SET_STANCE:
        case PRACTICE_SET_JUMP_MODE:
            return advanced;
        case PRACTICE_SET_JUMP_CADENCE:
            return advanced && s_practiceConfig.jumpMode != DUMMY_JUMP_DISABLED;
        case PRACTICE_SET_DEFENSIVE_RESPONSE: {
            // Live only where the mod can actually drive the mechanic; on the
            // rest the row would promise something that never happens.
            const int cat = PracticeDefense_DummyDefenseCategory();
            return advanced && (cat == Training::kDefenseCategoryJustParry ||
                                cat == Training::kDefenseCategoryDodge ||
                                cat == Training::kDefenseCategoryRepel ||
                                cat == Training::kDefenseCategoryUnique ||
                                cat == Training::kDefenseCategoryPushAway ||
                                cat == Training::kDefenseCategoryAbsolute);
        }

        // The vanilla menu greys these while CPU is on; keep that.
        case PRACTICE_SET_NATIVE_AIR_TECH:
        case PRACTICE_SET_NATIVE_GROUND_TECH:
        case PRACTICE_SET_NATIVE_BLOCK_TYPE:
        case PRACTICE_SET_NATIVE_DUMMY_STATE:
            return !advanced && !cpuOn;

        case PRACTICE_SET_TRIGGER_TARGET:
        case PRACTICE_SET_TRIGGER_RANDOM:
        case PRACTICE_SET_WAKE_BUFFER:
            return s_practiceConfig.triggerMasterEnabled;

        // The action fields only mean anything once the slot itself is on.
        case PRACTICE_SET_TRIGGER_BUTTON:
        case PRACTICE_SET_TRIGGER_DELAY:
            return s_practiceConfig.triggerMasterEnabled && MenuTrigger().enabled;

        // A custom value is only meaningful while its mode is set to Custom.
        case PRACTICE_SET_RECOVERY_HP_VALUE:
            return s_practiceConfig.recovery[1].hpMode == RECOVERY_HP_CUSTOM;
        case PRACTICE_SET_RECOVERY_METER_VALUE:
            return s_practiceConfig.recovery[1].meterMode == RECOVERY_METER_CUSTOM;
        case PRACTICE_SET_RECOVERY_GUARD_VALUE:
            return s_practiceConfig.recovery[1].guardMode == RECOVERY_GUARD_CUSTOM;

        default:
            if (IsTriggerRow(setting)) {
                return s_practiceConfig.triggerMasterEnabled;
            }
            if (IsHudRow(setting)) {
                // Without the hooks behind it the row would highlight and
                // accept input while the value never changed.
                return HudToggle_ElementAvailable(HudRowElement(setting));
            }
            return true;
    }
}

bool PracticeSetting_IsNumeric(int setting) {
    return setting == PRACTICE_SET_RECOVERY_HP_VALUE ||
           setting == PRACTICE_SET_RECOVERY_METER_VALUE ||
           setting == PRACTICE_SET_RECOVERY_GUARD_VALUE;
}

int PracticeSetting_NumericMin(int setting) {
    (void)setting;
    return 0;
}

int PracticeSetting_NumericMax(int setting) {
    switch (setting) {
        case PRACTICE_SET_RECOVERY_HP_VALUE: {
            const PlayerSnapshot snapshot = ReadPlayerSnapshot(1);
            return snapshot.valid ? GetEditableHpCap(snapshot) : 10000;
        }
        case PRACTICE_SET_RECOVERY_METER_VALUE: return kMeterMax;
        case PRACTICE_SET_RECOVERY_GUARD_VALUE: return kGuardGaugeMax;
        default:                                return 0;
    }
}

void PracticeSetting_SetNumeric(int setting, int value) {
    const int clamped = ClampInt(value,
                                 PracticeSetting_NumericMin(setting),
                                 PracticeSetting_NumericMax(setting));
    // Applied to both sides, matching how the mode rows behave from this menu.
    for (int p = 0; p < kPracticePlayerCount; ++p) {
        switch (setting) {
            case PRACTICE_SET_RECOVERY_HP_VALUE:    s_practiceConfig.recovery[p].hpCustom = clamped; break;
            case PRACTICE_SET_RECOVERY_METER_VALUE: s_practiceConfig.recovery[p].meterCustom = clamped; break;
            case PRACTICE_SET_RECOVERY_GUARD_VALUE: s_practiceConfig.recovery[p].guardCustom = clamped; break;
            default: break;
        }
    }
}

int PracticeTrigger_Count() {
    return kTriggerCount;
}

void PracticeTrigger_SelectSlot(int slot) {
    g_menuTriggerSlot = ClampInt(slot, 0, kTriggerCount - 1);
}

int PracticeTrigger_SelectedSlot() {
    return g_menuTriggerSlot;
}

const char* PracticeTrigger_Name(int slot) {
    return kShortTriggerLabels[ClampInt(slot, 0, kTriggerCount - 1)];
}

namespace {

// P2 is the dummy, so its character decides which movelist the picker offers.
const Training::CharacterMoveList* DummyMoveList() {
    const PlayerSnapshot dummy = ReadPlayerSnapshot(1);
    if (!dummy.valid || dummy.charId >= (uint32_t)Training::kCharacterMoveListCount) {
        return nullptr;
    }
    return &Training::kCharacterMoveLists[dummy.charId];
}

} // namespace

const char* PracticeTrigger_Summary(int slot) {
    const TriggerConfig& cfg = s_practiceConfig.triggers[ClampInt(slot, 0, kTriggerCount - 1)];
    if (!cfg.enabled) {
        return "";
    }

    static char text[48];
    const int motion = ClampInt(cfg.actionKind, 0, IM_ARRAYSIZE(kScriptActionLabels) - 1);
    const int button = ClampInt(cfg.actionButton, 0, IM_ARRAYSIZE(kScriptButtonLabels) - 1);
    if (motion == 0) {
        return "No action";
    }
    // Prefer the guide's notation so the row matches what the picker showed.
    const Training::CharacterMoveList* list = DummyMoveList();
    if (list && list->moves) {
        for (int i = 0; i < list->count; ++i) {
            if (list->moves[i].motion == motion && list->moves[i].button == button) {
                return list->moves[i].notation;
            }
        }
    }
    snprintf(text, sizeof(text), "%s+%s",
             kScriptActionLabels[motion], kScriptButtonLabels[button]);
    return text;
}

int PracticeTrigger_MoveCount() {
    const Training::CharacterMoveList* list = DummyMoveList();
    return (list && list->moves) ? list->count : 0;
}

const char* PracticeTrigger_MoveLabel(int index) {
    const Training::CharacterMoveList* list = DummyMoveList();
    if (!list || !list->moves || index < 0 || index >= list->count) {
        return "?";
    }
    // The guide's own notation, so "66A" instead of a "Dash Forward A" that
    // does not fit the column.
    return list->moves[index].notation;
}

int PracticeTrigger_SelectedMove() {
    const Training::CharacterMoveList* list = DummyMoveList();
    if (!list || !list->moves) {
        return -1;
    }
    const TriggerConfig& cfg = MenuTrigger();
    for (int i = 0; i < list->count; ++i) {
        if (list->moves[i].motion == cfg.actionKind &&
            list->moves[i].button == cfg.actionButton) {
            return i;
        }
    }
    return -1;
}

void PracticeTrigger_SelectMove(int index) {
    const Training::CharacterMoveList* list = DummyMoveList();
    if (!list || !list->moves || index < 0 || index >= list->count) {
        return;
    }
    MenuTrigger().actionKind = list->moves[index].motion;
    MenuTrigger().actionButton = list->moves[index].button;
}

const char* PracticeDummy_DefenceName() {
    const Training::CharacterMoveList* list = DummyMoveList();
    return (list && list->defenceName) ? list->defenceName : "";
}

int PracticeTrigger_MotionCount() {
    return IM_ARRAYSIZE(kScriptActionLabels);
}

const char* PracticeTrigger_MotionLabel(int motion) {
    return kScriptActionLabels[ClampInt(motion, 0, IM_ARRAYSIZE(kScriptActionLabels) - 1)];
}

int PracticeTrigger_GetMotion() {
    return MenuTrigger().actionKind;
}

void PracticeTrigger_SetMotion(int motion) {
    MenuTrigger().actionKind = ClampInt(motion, 0, IM_ARRAYSIZE(kScriptActionLabels) - 1);
}

const char* PracticeAction_Invoke(int action) {
    switch (action) {
        case PRACTICE_ACT_ROUND_RESET:
            return TryLoadRoundStartReset("pause_menu") ? "Round start restored"
                                                       : "No round-start autosave";
        case PRACTICE_ACT_SWAP_SIDES: {
            PlayerSnapshot snapshots[kPracticePlayerCount] = {
                ReadPlayerSnapshot(0),
                ReadPlayerSnapshot(1),
            };
            SwapPlayerPositions(snapshots);
            return "Positions swapped";
        }
        case PRACTICE_ACT_POSITION_MID:
            return ApplyPositionPreset(kMidScreenPositionPreset) ? "Mid screen" : "Unavailable";
        case PRACTICE_ACT_POSITION_CORNER:
            return ApplyPositionPreset(kRightCornerPositionPreset) ? "Corner" : "Unavailable";
        case PRACTICE_ACT_SAVE_STATE:
            return Savestate_Save() ? "State saved" : "Save failed";
        case PRACTICE_ACT_LOAD_STATE:
            return Savestate_Load() ? "State loaded" : "Load failed";
        case PRACTICE_ACT_SAVE_POSITION: {
            PlayerSnapshot snapshots[kPracticePlayerCount] = {
                ReadPlayerSnapshot(0),
                ReadPlayerSnapshot(1),
            };
            return SavePositionSnapshot(snapshots) ? "Position saved" : "Unavailable";
        }
        case PRACTICE_ACT_LOAD_POSITION:
            return LoadPositionSnapshot() ? "Position restored" : "No saved position";
        case PRACTICE_ACT_MACRO_RECORD:
            InputMacro_ToggleRecord();
            return InputMacro_GetState() == MACRO_IDLE ? "Recording stopped" : "Recording";
        case PRACTICE_ACT_MACRO_PLAY:
            InputMacro_TogglePlay();
            return InputMacro_GetState() == MACRO_REPLAYING ? "Playing" : "Playback stopped";
        case PRACTICE_ACT_POSITION_ROUND_START:
            return ApplyPositionPreset(kRoundStartPositionPreset) ? "Round start" : "Unavailable";
        case PRACTICE_ACT_FRAME_STEP:
            // Only meaningful while frozen, and the step lands once the menu
            // closes and the simulation ticks again.
            if (!s_paused) {
                return "Freeze first";
            }
            if (!s_stepRequested) {
                s_stepRequested = true;
                s_stepCounter++;
            }
            return "Step queued";
        default:
            return nullptr;
    }
}

void PracticeTools_RestoreRuntimeState(const PracticeToolsRuntimeState* state) {
    if (!state) {
        return;
    }

    ResetPracticeTransientRuntime(false);
    // Freeze is the player's view state, not part of the simulation being
    // restored: loading while frozen has to stay frozen so the state can be
    // stepped into. A step queued before the load is stale, so it is dropped.
    s_stepRequested = false;
    s_stepCounter = 0;

    PracticeDefenseRuntimeState defense{};
    defense.sequence = state->autoBlockSequence;
    defense.plan = state->frameGuardPlan;
    defense.lastProcessedSimFrame = state->lastProcessedSimFrame;
    PracticeDefense_RestoreState(&defense);

    LOG_INFO("[Practice] Restored runtime state: paused=%d (kept) step_requested=%d step_counter=%u "
             "seq_gen=%u seq_groups=%u seq_random=%d",
             s_paused ? 1 : 0,
             s_stepRequested ? 1 : 0,
             (unsigned int)s_stepCounter,
             (unsigned int)state->autoBlockSequence.generation,
             (unsigned int)state->autoBlockSequence.resolvedContactGroups,
             state->autoBlockSequence.randomBlock ? 1 : 0);
}

void PracticeTools_Init() {
    s_initialized = true;
    HotkeyConfig_Init();
    FrameAdvantage_Init();
    InputMacro_Init();
    memset(&s_practiceConfig, 0, sizeof(s_practiceConfig));
    s_practiceConfig.activeTab = PRACTICE_TAB_OVERVIEW;
    s_practiceConfig.dummyControlMode = DUMMY_CONTROL_NATIVE;
    s_practiceConfig.jumpCadenceFrames = 20;
    s_practiceConfig.comboOverlayEnabled = false;
    s_practiceConfig.recoveryDelayFrames = 0;
    s_practiceConfig.triggerTarget = TRIGGER_TARGET_P2;
    s_practiceConfig.wakeBufferFrames = 3;
    s_paused = false;
    s_stepRequested = false;
    s_stepCounter = 0;
    s_wasActive = false;
    s_toastCount = 0;
    ResetPracticeTransientRuntime(true);
    LOG_INFO("[Practice] Practice tools initialized");
}

void PracticeTools_Shutdown() {
    ResetPracticeState();
    InputMacro_Shutdown();
    FrameAdvantage_Shutdown();
    s_initialized = false;
}

void PracticeTools_FrameUpdate() {
    if (!s_initialized) return;

    // Hotkey edges are refreshed in ModOnFrame, ahead of every consumer.
    const bool active = IsPracticeModeNow();

    if (s_wasActive && !active) {
        ResetPracticeState();
    }

    if (!s_wasActive && active) {
        LOG_INFO("[Practice] Entered practice mode");
        ResetPracticeTransientRuntime(true);
    }

    s_wasActive = active;

    if (!active) {
        PracticeDefenseConfig idle{};
        PracticeDefense_SetConfig(idle);
        return;
    }

    PushDefenseConfig();

    UpdateToasts(1.0f / 60.0f);

    PlayerSnapshot snapshots[kPracticePlayerCount] = {
        ReadPlayerSnapshot(0),
        ReadPlayerSnapshot(1),
    };
    for (int player = 0; player < kPracticePlayerCount; ++player) {
        SeedValueEditorIfNeeded(player, snapshots[player]);
    }

    if (HotkeyConfig_JustPressed(HOTKEY_HITBOX_TOGGLE) && !Rollback::RollbackSession_IsActive()) {
        HitboxViewer_ToggleEnabled();
        PushToast(HitboxViewer_IsEnabled() ? "Hitboxes ON" : "Hitboxes OFF",
                  HitboxViewer_IsEnabled() ? IM_COL32(100, 255, 100, 255) : IM_COL32(255, 100, 100, 255));
        LOG_INFO("[Practice] Hitbox viewer %s", HitboxViewer_IsEnabled() ? "ON" : "OFF");
    }

    if (HotkeyConfig_JustPressed(HOTKEY_PAUSE_TOGGLE)) {
        PracticeTools_SetPaused(!s_paused);
    }

    if (HotkeyConfig_JustPressed(HOTKEY_FRAME_STEP) && s_paused && !s_stepRequested) {
        s_stepRequested = true;
        s_stepCounter++;
        LOG_INFO("[Practice] Step requested: counter=%d", s_stepCounter);
    }

    if (HotkeyConfig_JustPressed(HOTKEY_CONTROL_SWAP)) {
        const bool newSwap = !InputSystem_GetControlSwap();
        PracticeTools_ApplyControlSwapState(newSwap);
        PushToast(newSwap ? "Controls Swapped (P2)" : "Controls Normal (P1)",
                  IM_COL32(100, 200, 255, 255));
        LOG_INFO("[Practice] Controller swap %s (controlling %s)",
                 newSwap ? "ON" : "OFF",
                 newSwap ? "P2" : "P1");
    }

    bool positionResetThisFrame = false;
    if (HotkeyConfig_JustPressed(HOTKEY_POSITION_LOAD)) {
        if (const PositionPreset* preset = GetLoadPositionPresetFromHotkey()) {
            if (ApplyPositionPreset(*preset)) {
                positionResetThisFrame = true;
                PushToast(preset->label, IM_COL32(120, 255, 180, 255));
                LOG_INFO("[Practice] Position preset hotkey pressed: %s", preset->label);
            } else {
                PushToast("Wait for round start", IM_COL32(255, 180, 120, 255));
            }
        } else if (s_positionSnapshot.valid) {
            if (LoadPositionSnapshot()) {
                positionResetThisFrame = true;
                char toastLabel[96] = {};
                FormatHotkeyToast(toastLabel,
                                  sizeof(toastLabel),
                                  "Saved position restored",
                                  HOTKEY_POSITION_LOAD);
                PushToast(toastLabel, IM_COL32(120, 255, 180, 255));
                LOG_INFO("[Practice] Saved position hotkey pressed");
            } else {
                PushToast("Wait for round start", IM_COL32(255, 180, 120, 255));
            }
        } else {
            PushToast("No saved position", IM_COL32(255, 180, 120, 255));
        }
    }

    if (HotkeyConfig_JustPressed(HOTKEY_POSITION_SAVE)) {
        if (SavePositionSnapshot(snapshots)) {
            char toastLabel[96] = {};
            FormatHotkeyToast(toastLabel,
                              sizeof(toastLabel),
                              "Position saved",
                              HOTKEY_POSITION_SAVE);
            PushToast(toastLabel, IM_COL32(120, 220, 255, 255));
            LOG_INFO("[Practice] Position save hotkey pressed");
        } else if (!CanUsePositionTools()) {
            PushToast("Wait for round start", IM_COL32(255, 180, 120, 255));
        } else {
            PushToast("Position save unavailable", IM_COL32(255, 180, 120, 255));
        }
    }

    if (positionResetThisFrame) {
        Rollback::NetplayLog_Write(
            "TRAINRESET", -1,
            "position_reset_frame_early_out frame=%u",
            ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
        LOG_INFO("[TRAINRESET] position reset frame early-out frame=%u",
                 ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
        PracticeTools_SyncControlSwapState();
        return;
    }

    if (HotkeyConfig_JustPressed(HOTKEY_MACRO_RECORD)) {
        InputMacro_ToggleRecord();
    }

    if (HotkeyConfig_JustPressed(HOTKEY_MACRO_PLAY)) {
        InputMacro_TogglePlay();
    }

    if (HotkeyConfig_JustPressed(HOTKEY_MACRO_SLOT_NEXT)) {
        InputMacro_NextSlot();
    }

    const bool playable = IsInPlayableMatchGameplay();
    const uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    if (!playable) {
        ReleaseOwnedOverride(0);
        if (InputMacro_GetState() != MACRO_REPLAYING) {
            ReleaseOwnedOverride(1);
        }
        ClearPlayerRuntime();
        ClearComboTrackers();
        s_lastObservedSimFrame = kInvalidFrame;
    } else if (simFrame != s_lastObservedSimFrame) {
        UpdateComboTrackers(snapshots);
        UpdatePlayerRuntimeForFrame(simFrame, snapshots);
        s_lastObservedSimFrame = simFrame;
    }

    if (playable && (!s_paused || s_stepRequested)) {
        ApplyContinuousRecovery(simFrame, snapshots);
        ApplyAutomationOverrides(simFrame, snapshots);
    } else {
        ReleaseOwnedOverride(0);
        if (InputMacro_GetState() != MACRO_REPLAYING) {
            ReleaseOwnedOverride(1);
        }
    }

    PracticeTools_SyncControlSwapState();
}

void PracticeTools_LogDiagnosticSnapshot(const char* reason) {
    const PlayerSnapshot snapshots[kPracticePlayerCount] = {
        ReadPlayerSnapshot(0),
        ReadPlayerSnapshot(1),
    };
    const int dummy = DummySide();
    const uint32_t simFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    LOG_INFO("[Diag] ===== practice snapshot (%s) =====", reason ? reason : "unspecified");
    LOG_INFO("[Diag] context: game_type=%d mode=%d substate=%d frame=%u playable=%d "
             "practice_mode=%d paused=%d initialised=%d",
             GetGameType(), GetGameMode(), GetSubstate(), simFrame,
             IsInPlayableMatchGameplay() ? 1 : 0,
             IsPracticeModeNow() ? 1 : 0,
             s_paused ? 1 : 0,
             s_initialized ? 1 : 0);

    uint8_t expectedP1Cpu = 0;
    uint8_t expectedP2Cpu = 1;
    ComputeDesiredCpuFlags(&expectedP1Cpu, &expectedP2Cpu);
    LOG_INFO("[Diag] sides: dummy=%s human=%s control_swap=%d macro_state=%d | "
             "cpu p1=%u(want %u) p2=%u(want %u) | override p1=%d(0x%04X) p2=%d(0x%04X)",
             SideLabel(dummy), SideLabel(1 - dummy),
             InputSystem_GetControlSwap() ? 1 : 0,
             (int)InputMacro_GetState(),
             (unsigned)ReadMemory<uint8_t>(ADDR_P1_CPU_FLAG), (unsigned)expectedP1Cpu,
             (unsigned)ReadMemory<uint8_t>(ADDR_P2_CPU_FLAG), (unsigned)expectedP2Cpu,
             s_ownedOverrideActive[0] ? 1 : 0, (unsigned)s_lastOwnedOverrideInput[0],
             s_ownedOverrideActive[1] ? 1 : 0, (unsigned)s_lastOwnedOverrideInput[1]);

    for (int player = 0; player < kPracticePlayerCount; ++player) {
        const PlayerSnapshot& p = snapshots[player];
        if (!p.valid) {
            LOG_INFO("[Diag] %s: entity unavailable", SideLabel(player));
            continue;
        }
        LOG_INFO("[Diag] %s%s: char=%u(%s) action=%u(%s) hp=%u/%u meter=%u guard=%u "
                 "x=%d y=%d facing=%s airborne=%d grounded=%d actionable=%d "
                 "attack_state=%u hit_active=%u",
                 SideLabel(player), player == dummy ? " [dummy]" : " [human]",
                 (unsigned)p.charId, GetCharacterName(p.charId),
                 (unsigned)p.actionId, Training::ActionCategory(p.actionId),
                 (unsigned)p.hp, (unsigned)p.maxHp, (unsigned)p.meter, (unsigned)p.guardGauge,
                 (int)p.x, (int)p.y, p.facingRight ? "right" : "left",
                 p.airborne ? 1 : 0,
                 IsGroundedAction(p) ? 1 : 0,
                 IsActionable(p) ? 1 : 0,
                 (unsigned)p.attackState, (unsigned)p.hitActive);
        LOG_INFO("[Diag] %s defence bytes: allow1949=%u flags1940=0x%04X parry1965=0x%02X "
                 "down1973=%u back1974=%u react1980=%d kind1989=%u timer1990=%u "
                 "push1993=0x%02X free1994=%u pushdown1996=%u pushback1997=%u stock823=%u",
                 SideLabel(player),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_DEFENCE_ALLOWED),
                 (unsigned)ReadMemory<uint32_t>(p.base + ENTITY_OFF_MAX_HIT_FLAGS),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_PARRY_WINDOW),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_PARRY_STANCE),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_PARRY_GUARD_HELD),
                 (int)ReadMemory<int32_t>(p.base + ENTITY_OFF_HIT_REACTION_STATE),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_HIT_REACTION_KIND),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_HIT_REACTION_TIMER),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_PUSH_AWAY_ARMED),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_PUSH_AWAY_FREE),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_PUSH_AWAY_DOWN),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_PUSH_AWAY_BACK),
                 (unsigned)ReadMemory<uint8_t>(p.base + ENTITY_OFF_DEFENSE_RESOURCE));
    }

    // Hook state. "The setting does nothing" and "the hook that would have
    // carried it out never installed" look identical from the game.
    LOG_INFO("[Diag] hooks: defence_installed=%d ordinary_guard=%d native_arming=%d error='%s'",
             PracticeDefense_IsInstalled() ? 1 : 0,
             PracticeDefense_HasOrdinaryGuardHook() ? 1 : 0,
             PracticeDefense_ArmHooksActive() ? 1 : 0,
             PracticeDefense_GetInstallError());

    const PracticeDefenseArmState& arm = PracticeDefense_GetArmState();
    LOG_INFO("[Diag] defence: category=%d configured=%s effective=%s driver=%s gate_open=%d "
             "| arms=%u (parry %u repel %u push %u) forced1949=%u prepared=%u "
             "resolver_calls=%u declines=%u",
             arm.category,
             Training::DefensiveResponseLabel(arm.configured),
             Training::DefensiveResponseLabel(arm.effective),
             arm.hookDriven ? "native hook" : "input",
             arm.gateOpen ? 1 : 0,
             arm.arms, arm.parryArms, arm.repelArms, arm.pushAwayArms,
             arm.defenceAllowedForced, arm.contactsPrepared,
             arm.resolverCalls, arm.resolverDeclines);

    const PracticeDefenseTelemetry& tel = PracticeDefense_GetTelemetry();
    LOG_INFO("[Diag] contacts: total=%u guard=%u parry=%u repel=%u push=%u dodge=%u "
             "absolute=%u counter=%u hit=%u | lane_applications=%u conflicts=%u "
             "refused_no_lane=%u refused_special=%u late_blocks=%u",
             tel.contactTicks,
             tel.resultCounts[CONTACT_RESULT_GUARD],
             tel.resultCounts[CONTACT_RESULT_JUST_PARRY],
             tel.resultCounts[CONTACT_RESULT_REPEL],
             tel.resultCounts[CONTACT_RESULT_PUSH_AWAY],
             tel.resultCounts[CONTACT_RESULT_DODGE],
             tel.resultCounts[CONTACT_RESULT_ABSOLUTE_DEFENSE],
             tel.resultCounts[CONTACT_RESULT_UNIQUE_DEFENSE],
             tel.resultCounts[CONTACT_RESULT_HIT],
             tel.laneApplications, tel.conflictFrames,
             tel.refusedNoLane, tel.refusedSpecialGuard, tel.lateBlockSignatures);

    // Which defensive mechanics this dummy can be offered at all, so a missing
    // menu entry reads as "this character has no such mechanic" rather than as
    // a bug in the row.
    {
        int options[(int)Training::DefensiveResponse::Count];
        const int count =
            DefensiveResponseOptions(options, (int)Training::DefensiveResponse::Count);
        char list[192] = {};
        size_t used = 0;
        for (int i = 0; i < count && used + 24 < sizeof(list); ++i) {
            used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s",
                                     i ? ", " : "",
                                     Training::DefensiveResponseLabel(
                                         (Training::DefensiveResponse)options[i]));
        }
        LOG_INFO("[Diag] defence options for this dummy: [%s]", list);
    }

    // Every setting, with exactly the label and value text the menu draws, so a
    // report can be checked against what was actually selected.
    for (int setting = 0; setting < PRACTICE_SET_COUNT; ++setting) {
        const char* label = PracticeSetting_Label(setting);
        if (!label || !label[0]) {
            continue;
        }
        LOG_INFO("[Diag] setting %-22s = %-14s (raw %d%s)",
                 label,
                 PracticeSetting_ValueText(setting),
                 PracticeSetting_Get(setting),
                 PracticeSetting_Enabled(setting) ? "" : ", DISABLED");
    }

    LOG_INFO("[Diag] native training bytes: cpu=%u air_tech=%u ground_tech=%u block_type=%u "
             "dummy_state=%u health_regen=%u meter_level=%u damage_display=%u input_display=%u",
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_AIR_TECH_SETTING),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_GROUND_TECH_SETTING),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_BLOCK_TYPE_SETTING),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_DUMMY_STATE_SETTING),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_HEALTH_REGEN_SETTING),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_METER_LEVEL_SETTING),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_DAMAGE_DISPLAY),
             (unsigned)ReadMemory<uint8_t>(ADDR_TRAINING_INPUT_DISPLAY));
    LOG_INFO("[Diag] ===== end snapshot =====");
}

void PracticeTools_RenderImGui() {
    if (!PracticeTools_IsPracticeModeActive()) {
        ImGui::TextDisabled("Practice tools are only available in Training mode.");
        ImGui::TextDisabled("Select Training (5th option) from the main menu.");
        return;
    }

    PlayerSnapshot snapshots[kPracticePlayerCount] = {
        ReadPlayerSnapshot(0),
        ReadPlayerSnapshot(1),
    };
    for (int player = 0; player < kPracticePlayerCount; ++player) {
        SeedValueEditorIfNeeded(player, snapshots[player]);
    }

    ImGui::Text("Practice Mode Active");
    ImGui::Separator();

    if (ImGui::BeginTabBar("PracticeTabs")) {
        for (int tab = 0; tab < IM_ARRAYSIZE(kPracticeTabLabels); ++tab) {
            if (!ImGui::BeginTabItem(kPracticeTabLabels[tab])) {
                continue;
            }

            s_practiceConfig.activeTab = tab;
            switch (tab) {
                case PRACTICE_TAB_OVERVIEW:
                    RenderOverviewTab(snapshots);
                    break;
                case PRACTICE_TAB_OPPONENT:
                    RenderOpponentTab(snapshots);
                    break;
                case PRACTICE_TAB_VALUES:
                    RenderValuesTab(snapshots);
                    break;
                case PRACTICE_TAB_OPTIONS:
                    RenderOptionsTab();
                    break;
                case PRACTICE_TAB_TRIGGERS:
                    RenderTriggersTab();
                    break;
                case PRACTICE_TAB_MACROS:
                    InputMacro_RenderImGui();
                    break;
                case PRACTICE_TAB_HOTKEYS:
                    HotkeyConfig_RenderImGui();
                    break;
                default:
                    break;
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// Build a compact action string for the trigger overlay (e.g. "236A", "Jump").
// Actions indexed 1–18 use a button suffix; 19+ are button-less.
static void BuildTriggerActionLabel(int actionKind, int actionButton, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (actionKind <= 0 || actionKind >= (int)(sizeof(kScriptActionLabels) / sizeof(kScriptActionLabels[0]))) {
        snprintf(out, outSize, "-");
        return;
    }
    const char* base = kScriptActionLabels[actionKind];
    const size_t len = strlen(base);
    // Labels 1–18 all end with 'X' as a button placeholder
    if (len > 0 && base[len - 1] == 'X') {
        const char* btn = (actionButton >= 0 && actionButton < 4) ? kScriptButtonLabels[actionButton] : "?";
        snprintf(out, outSize, "%.*s%s", (int)(len - 1), base, btn);
    } else {
        snprintf(out, outSize, "%s", base);
    }
}

// EFZ-style persistent trigger status panel — right-aligned, stacked per enabled trigger.
// Gold = configured, green = recently fired (< 60 sim frames ago).
static void RenderTriggerStatusOverlay(ImDrawList* dl, const ImVec2& displaySize) {
    if (!s_practiceConfig.triggerMasterEnabled) return;

    constexpr float kStartY       = 90.0f;
    constexpr float kLineSpacing  = 18.0f;
    constexpr float kPadX         = 4.0f;
    constexpr float kPadY         = 2.0f;
    constexpr float kRightMargin  = 8.0f;
    constexpr uint32_t kHighlightFrames = 60u;

    const float lineHeight = ImGui::GetTextLineHeight();
    float y = kStartY;
    bool anyRendered = false;

    for (int i = 0; i < kTriggerCount; ++i) {
        const TriggerConfig& cfg = s_practiceConfig.triggers[i];
        if (!cfg.enabled || cfg.actionKind == SCRIPT_ACTION_NONE) continue;

        char actionLabel[24] = {};
        BuildTriggerActionLabel(cfg.actionKind, cfg.actionButton, actionLabel, sizeof(actionLabel));

        char line[64] = {};
        snprintf(line, sizeof(line), "%s: %s", kTriggerLabels[i], actionLabel);

        const bool recentlyFired =
            (s_lastFiredTriggerId == i) &&
            (s_lastObservedSimFrame != kInvalidFrame) &&
            (s_lastObservedSimFrame - s_lastFiredTriggerFrame < kHighlightFrames);

        const ImU32 textColor = recentlyFired
            ? IM_COL32(80, 255, 100, 255)    // green — just fired
            : IM_COL32(255, 215, 0, 255);    // gold  — configured

        const ImVec2 textSize = ImGui::CalcTextSize(line);
        const float x = displaySize.x - textSize.x - kRightMargin;

        dl->AddRectFilled(
            ImVec2(x - kPadX,               y - kPadY),
            ImVec2(x + textSize.x + kPadX,  y + lineHeight + kPadY),
            IM_COL32(0, 0, 0, 160), 3.0f);
        dl->AddText(ImVec2(x, y), textColor, line);

        y += kLineSpacing;
        anyRendered = true;
    }
    (void)anyRendered;
}

void PracticeTools_RenderHUD() {
    if (!s_initialized) return;
    if (!IsPracticeModeNow()) return;

    ImDrawList* dl = PracticeTools_ShouldRenderHudBehindMenu()
        ? ImGui::GetBackgroundDrawList()
        : ImGui::GetForegroundDrawList();
    if (!dl) return;

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    if (s_paused) {
        char buf[64];
        if (s_stepCounter > 0) {
            snprintf(buf, sizeof(buf), "STEP %d", s_stepCounter);
        } else {
            snprintf(buf, sizeof(buf), "PAUSED");
        }

        const ImVec2 textSize = ImGui::CalcTextSize(buf);
        const float x = (displaySize.x - textSize.x) * 0.5f;
        // Slot 0 (bottom-most of the center stack) — above the meter bars
        const float y = displaySize.y - 40.0f;

        dl->AddRectFilled(
            ImVec2(x - 6, y - 2),
            ImVec2(x + textSize.x + 6, y + textSize.y + 2),
            IM_COL32(0, 0, 0, 180));
        dl->AddText(ImVec2(x, y), IM_COL32(255, 255, 100, 255), buf);
    }

    FrameAdvantage_RenderOverlay();
    InputMacro_RenderOverlay();
    RenderTriggerStatusOverlay(dl, displaySize);

    if (HasComboOverlayContent()) {
        const PlayerSnapshot snapshots[kPracticePlayerCount] = {
            ReadPlayerSnapshot(0),
            ReadPlayerSnapshot(1),
        };
        RenderComboOverlayPanel(dl, ImVec2(12.0f, 100.0f), 0, snapshots);
        RenderComboOverlayPanel(dl, ImVec2(displaySize.x - 232.0f, 100.0f), 1, snapshots);
    }

    if (s_toastCount > 0) {
        // Start newest toast above the bottom-center indicator stack
        const float baseY = displaySize.y - 110.0f;

        for (int i = s_toastCount - 1; i >= 0; i--) {
            const PracticeInternal::Toast& t = s_toasts[i];

            float alpha = 1.0f;
            if (t.remaining < (TOAST_DURATION - TOAST_FADE_START)) {
                alpha = t.remaining / (TOAST_DURATION - TOAST_FADE_START);
                if (alpha < 0.0f) alpha = 0.0f;
            }

            ImU32 col = t.color;
            const uint8_t a = (uint8_t)(((col >> 24) & 0xFF) * alpha);
            col = (col & 0x00FFFFFF) | ((ImU32)a << 24);
            const ImU32 bgCol = IM_COL32(0, 0, 0, (uint8_t)(160 * alpha));

            const ImVec2 textSize = ImGui::CalcTextSize(t.text);
            const float x = (displaySize.x - textSize.x) * 0.5f;
            const float y = baseY - (float)(s_toastCount - 1 - i) * (textSize.y + 6.0f);

            dl->AddRectFilled(
                ImVec2(x - 6, y - 2),
                ImVec2(x + textSize.x + 6, y + textSize.y + 2),
                bgCol);
            dl->AddText(ImVec2(x, y), col, t.text);
        }
    }
}

void PracticeTools_ApplyControlSwapState(bool swapped) {
    const bool previous = InputSystem_GetControlSwap();
    InputSystem_SetControlSwap(swapped);
    if (previous != swapped) {
        // The dummy is now the other entity, so everything keyed to the old one
        // has to go: the half-finished defence sequence, a jump being held, an
        // override still driving the side the human just took back.
        for (int player = 0; player < kPracticePlayerCount; ++player) {
            s_playerRuntime[player].jumpHoldFrames = 0;
            s_playerRuntime[player].jumpHoldInput = 0;
            s_playerRuntime[player].jumpCooldown = 0;
            ReleaseOwnedOverride(player);
        }
        PracticeDefense_Reset();
        ClampDefensiveResponseToCategory(PracticeDefense_DummyDefenseCategory());

        LOG_INFO("[Practice] Control swap %s -> %s: human=%s dummy=%s "
                 "dummy_char=%s defence_category=%d control=%s",
                 previous ? "P2" : "P1",
                 swapped ? "P2" : "P1",
                 SideLabel(1 - DummySide()),
                 SideLabel(DummySide()),
                 GetCharacterName(ReadPlayerSnapshot(DummySide()).charId),
                 PracticeDefense_DummyDefenseCategory(),
                 kDummyControlModeLabels[ClampInt(s_practiceConfig.dummyControlMode, 0,
                                                  IM_ARRAYSIZE(kDummyControlModeLabels) - 1)]);
    }

    if (IsPracticeModeNow()) {
        WriteDesiredCpuFlags();
    }
}

void PracticeTools_SyncControlSwapState() {
    if (!IsPracticeModeNow()) {
        return;
    }

    uint8_t expectedP1Cpu = 0;
    uint8_t expectedP2Cpu = 1;
    ComputeDesiredCpuFlags(&expectedP1Cpu, &expectedP2Cpu);
    const uint8_t p1Cpu = ReadMemory<uint8_t>(ADDR_P1_CPU_FLAG);
    const uint8_t p2Cpu = ReadMemory<uint8_t>(ADDR_P2_CPU_FLAG);

    if (p1Cpu == expectedP1Cpu && p2Cpu == expectedP2Cpu) {
        return;
    }

    LOG_INFO("[Practice] Re-syncing CPU ownership (swap=%d, p1_cpu=%u->%u, p2_cpu=%u->%u)",
             InputSystem_GetControlSwap() ? 1 : 0,
             (unsigned int)p1Cpu,
             (unsigned int)expectedP1Cpu,
             (unsigned int)p2Cpu,
             (unsigned int)expectedP2Cpu);
    WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, expectedP1Cpu);
    WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, expectedP2Cpu);
}

bool PracticeTools_HasVisibleHud() {
    return s_initialized &&
           IsPracticeModeNow() &&
           (s_paused ||
            s_toastCount > 0 ||
            FrameAdvantage_HasVisibleOverlay() ||
            InputMacro_GetState() != MACRO_IDLE ||
            HasComboOverlayContent());
}

bool PracticeTools_ShouldRenderHudBehindMenu() {
    return ModMenu_IsOpen();
}

// Defined out here on purpose: the roster table and GetCharacterName live in
// this file's anonymous namespace, so the exported accessor cannot.
const char* PracticeTools_CharacterName(uint32_t charId) {
    return GetCharacterName(charId);
}
