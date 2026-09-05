#include "ui/pause_menu.h"

#include "core/game_state.h"
#include "core/mod_main.h"
#include "net/netplay_menu_render.h"
#include "patches/memory_utils.h"
#include "training/character_moves.h"
#include "training/practice_tools.h"
#include "ui/strings.h"
#include "input_system.h"
#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// --- Native seam ---------------------------------------------------------
typedef int (__cdecl* PauseInput_t)(int);
typedef int (__cdecl* PauseRender_t)(int*);

PauseInput_t  g_origPauseInput = nullptr;
PauseRender_t g_origPauseRender = nullptr;
bool g_installed = false;
char g_installError[128] = {};

// Vanilla return protocol.
constexpr int kResultResume = 0;
constexpr int kResultCharSelect = 1;
constexpr int kResultExitMatch = 2;
constexpr int kResultStay = 255;

// --- Geometry ------------------------------------------------------------
// Vanilla's rhythm, with the tab strip taking the first row band so the rows
// below still land on the 32 px grid the sprite quads used.
constexpr int kTabY = 64;
constexpr int kRowFirstY = 96;
constexpr int kRowPitch = 32;
constexpr int kRowCount = 10;          // visible rows, y 96 .. 384
// A page may hold more than fits; the extra scroll into view rather than being
// silently dropped, which would have cost the HUD page its Back row.
constexpr int kRowMax = 20;
constexpr int kLabelX = 64;
constexpr int kValueX = 320;
constexpr int kHintY = 424;
constexpr int kTabGap = 22;

// Column budgets. Anything wider is truncated rather than allowed to run into
// the next column, which is what made a long trigger name overlap its value.
constexpr int kLabelWidth = kValueX - kLabelX - 8;   // 248
constexpr int kValueRight = 592;
constexpr int kValueWidth = kValueRight - kValueX;   // 272

// A trigger row carries two values - whether it is on, and what it will do -
// so it gets its own narrower split.
constexpr int kTrigLabelWidth = 230;
constexpr int kTrigStateX = 300;
constexpr int kTrigStateWidth = 70;
constexpr int kTrigActionX = 380;
constexpr int kTrigActionWidth = 212;

// Backing bands. The vanilla dim alone leaves small text fighting the stage, so
// the strip and the hint get a second pass and the rows a lighter one.
constexpr int kBandLeft = 40;
constexpr int kBandRight = 600;
constexpr int kTabBandTop = 58;
constexpr int kTabBandBottom = 92;
constexpr int kRowBandTop = 92;
constexpr int kRowBandBottom = 412;
constexpr int kHintBandTop = 412;
constexpr int kHintBandBottom = 446;

int ClampInt(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// ImGui anchors AddText at the top of the line box, which for this face sits
// (ascent - capHeight) / (ascent - descent) = 0.2921 of the size above the cap
// top. Vanilla's Latin ink starts 2 px into the row band, so shift by that much
// less. Size-dependent, because the hint line is drawn much smaller.
int TextYOffset(float size) {
    return 2 - (int)(size * 0.2921f + 0.5f);
}

// caps = 0.509 * SizePixels for Shippori Mincho Bold: 49 is the vanilla 25 px,
// 44 is the 22 px this settled on - a clear step down while still reading at
// vanilla scale. The tab strip uses the Key Config size so it sits under the
// rows the way that screen's key column does.
constexpr float kRowTextSize = 44.0f;
constexpr float kTabTextSize = 19.0f;
constexpr float kHintTextSize = 19.0f;

// Vanilla tints, applied to label and value together.
constexpr uint8_t kInkSelected[3] = { 255, 0, 0 };
constexpr uint8_t kInkNormal[3] = { 255, 255, 255 };
constexpr uint8_t kInkDisabled[3] = { 128, 128, 128 };
constexpr uint8_t kInkDisabledSel[3] = { 128, 0, 0 };

// --- Tabs ----------------------------------------------------------------
// Six tabs at the Key Config size need 288 px of names; 512 px is available
// between x 64 and x 576, so they fit with room for real gaps.
enum Page : int {
    kPageDummy = 0,
    kPageRecovery,
    kPageTriggers,
    kPageDisplay,
    kPageMatch,
    kPageState,
    kPageExit,
    kPageTabCount,
    // Reached from a row, not from the strip, so it is not a tab.
    kPageRecoveryAdvanced = kPageTabCount,
    kPageHud,
    kPageCount,
};

const Str kPageNames[kPageCount] = {
    Str::Pm_TabDummy, Str::Pm_TabRecovery, Str::Pm_TabTriggers, Str::Pm_TabDisplay,
    Str::Pm_TabMatch, Str::Pm_TabState, Str::Pm_TabExit,
    Str::Pm_TabAutoRecovery, Str::Pm_TabHud,
};

enum RowKind : uint8_t {
    kRowSetting,
    kRowAction,
    kRowSubPage,
    kRowBack,
};

struct Row {
    RowKind kind;
    int id;              // setting id, action id, or target page
    const char* label;   // overrides the setting label when set
};

// --- Motion popup --------------------------------------------------------
// 22 motions is too many to cycle through one row at a time, so opening an
// enabled trigger drops a grid over the menu. Moving in the grid sets the
// motion immediately - there is nothing to confirm.
constexpr int kPopupCols = 5;
constexpr int kPopupLeft = 56;
constexpr int kPopupRight = 600;
constexpr int kPopupTop = 96;
constexpr int kPopupBottom = 446;
constexpr int kPopupColPitch = 104;
constexpr int kPopupGridX = 72;
constexpr int kPopupGridY = 152;
constexpr int kPopupRowPitch = 28;
constexpr float kPopupTextSize = 26.0f;

// The grid has to hold the longest movelist in the guide AND the whole motion
// list, or entries past the bottom row would be unreachable - which is exactly
// what adding two cancel routes to a 4x8 grid would have done silently.
constexpr int kPopupGridRows = 8;
constexpr int kPopupGridCapacity = kPopupCols * kPopupGridRows;
static_assert(Training::kCharacterMoveMaxCount <= kPopupGridCapacity,
              "movelist popup grid is too small for the roster");

// Cursor rows past the grid: the two action fields, in order.
constexpr int kPopupFieldButton = 0;
constexpr int kPopupFieldDelay = 1;
constexpr int kPopupFieldCount = 2;

bool g_popupOpen = false;
int  g_popupCursor = 0;   // 0..motions-1 in the grid, then the fields

// --- Numeric entry -------------------------------------------------------
// "Custom" recovery restores a number, so the menu has to be able to set one.
// Cycling by a step is fine for a nudge, but a value in the thousands wants
// typing, so the row opens a small keyboard field.
bool g_numericEditing = false;
int  g_numericSetting = -1;
char g_numericBuffer[16] = {};
bool g_numericPrevKeyDown[256] = {};

// --- Menu state ----------------------------------------------------------
int g_page = kPageDummy;
// First visible row on a page longer than the window.
int g_rowScroll = 0;
// 0 is the tab strip, 1..count are the rows, so the whole menu is one cursor
// and the strip is reachable with nothing but a d-pad.
int g_cursor = 1;
bool g_wasActiveLastFrame = false;
char g_hint[96] = {};
int g_hintFrames = 0;
// Last status serial this menu has shown, so an unchanged message is not
// re-raised every frame and does not keep resetting its own timer.
uint32_t g_statusSerial = 0;
int g_pendingResult = kResultStay;

// --- Row construction ----------------------------------------------------

int BuildRows(int page, Row* out, int cap) {
    int n = 0;
    auto push = [&](RowKind kind, int id, const char* label) {
        if (n < cap) {
            out[n].kind = kind;
            out[n].id = id;
            out[n].label = label;
            ++n;
        }
    };

    switch (page) {
        case kPageDummy: {
            push(kRowSetting, PRACTICE_SET_DUMMY_BACKEND, nullptr);
            push(kRowSetting, PRACTICE_SET_NATIVE_CPU, nullptr);
            push(kRowSetting, PRACTICE_SET_CONTROL_SWAP, nullptr);
            if (PracticeSetting_Get(PRACTICE_SET_DUMMY_BACKEND) == 0) {
                // Mod: the mod's own dummy logic.
                push(kRowSetting, PRACTICE_SET_BLOCK_MODE, nullptr);
                push(kRowSetting, PRACTICE_SET_DEFENSIVE_RESPONSE, nullptr);
                push(kRowSetting, PRACTICE_SET_STANCE, nullptr);
                push(kRowSetting, PRACTICE_SET_JUMP_MODE, nullptr);
                push(kRowSetting, PRACTICE_SET_JUMP_CADENCE, nullptr);
            } else {
                push(kRowSetting, PRACTICE_SET_NATIVE_AIR_TECH, nullptr);
                push(kRowSetting, PRACTICE_SET_NATIVE_GROUND_TECH, nullptr);
                push(kRowSetting, PRACTICE_SET_NATIVE_BLOCK_TYPE, nullptr);
                push(kRowSetting, PRACTICE_SET_NATIVE_DUMMY_STATE, nullptr);
            }
            break;
        }
        case kPageRecovery:
            // Vanilla's own Life / Spirit automatic restoration first: this tab
            // is what 体力 and 気力 were. The mod's per-event auto-recovery is a
            // different mechanism, so it lives one level down.
            push(kRowSetting, PRACTICE_SET_HEALTH_REGEN, nullptr);
            push(kRowSetting, PRACTICE_SET_METER_LEVEL, nullptr);
            push(kRowSubPage, kPageRecoveryAdvanced, S(Str::Pm_Advanced));
            break;
        case kPageRecoveryAdvanced:
            push(kRowSetting, PRACTICE_SET_RECOVERY_HP, nullptr);
            push(kRowSetting, PRACTICE_SET_RECOVERY_HP_VALUE, nullptr);
            push(kRowSetting, PRACTICE_SET_RECOVERY_METER, nullptr);
            push(kRowSetting, PRACTICE_SET_RECOVERY_METER_VALUE, nullptr);
            push(kRowSetting, PRACTICE_SET_RECOVERY_GUARD, nullptr);
            push(kRowSetting, PRACTICE_SET_RECOVERY_GUARD_VALUE, nullptr);
            push(kRowSetting, PRACTICE_SET_RECOVERY_BOTH_NEUTRAL, nullptr);
            push(kRowSetting, PRACTICE_SET_RECOVERY_DELAY, nullptr);
            push(kRowBack, kPageRecovery, S(Str::Common_Back));
            break;
        case kPageTriggers:
            push(kRowSetting, PRACTICE_SET_TRIGGERS_ENABLED, nullptr);
            push(kRowSetting, PRACTICE_SET_TRIGGER_TARGET, nullptr);
            push(kRowSetting, PRACTICE_SET_TRIGGER_RANDOM, nullptr);
            push(kRowSetting, PRACTICE_SET_WAKE_BUFFER, nullptr);
            // One row per trigger: Left/Right toggles it, and the value shows
            // the action it will run, so the whole set reads at a glance.
            for (int t = 0; t < PracticeTrigger_Count(); ++t) {
                push(kRowSetting, PRACTICE_SET_TRIGGER_1 + t, nullptr);
            }
            break;
        case kPageHud:
            // One row per element, in HudElement order. Ten plus Back is one
            // more than the window shows, so the page scrolls.
            for (int e = 0; e < HUD_ELEM_COUNT; ++e) {
                push(kRowSetting, PRACTICE_SET_HUD_FIRST + e, nullptr);
            }
            push(kRowBack, kPageDisplay, S(Str::Common_Back));
            break;
        case kPageDisplay:
            push(kRowSetting, PRACTICE_SET_HITBOXES, nullptr);
            push(kRowSetting, PRACTICE_SET_COMBO_OVERLAY, nullptr);
            push(kRowSetting, PRACTICE_SET_INPUT_DISPLAY, nullptr);
            push(kRowSetting, PRACTICE_SET_DAMAGE_DISPLAY, nullptr);
            push(kRowSetting, PRACTICE_SET_FRAME_ADVANTAGE, nullptr);
            push(kRowSubPage, kPageHud, S(Str::Pm_HudPage));
            break;
        case kPageMatch:
            push(kRowAction, PRACTICE_ACT_POSITION_MID, S(Str::Pm_MidScreen));
            push(kRowAction, PRACTICE_ACT_POSITION_CORNER, S(Str::Pm_Corner));
            push(kRowAction, PRACTICE_ACT_POSITION_ROUND_START, S(Str::Pm_RoundStart));
            push(kRowAction, PRACTICE_ACT_SWAP_SIDES, S(Str::Pm_SwapSides));
            push(kRowAction, PRACTICE_ACT_SAVE_POSITION, S(Str::Pm_SavePosition));
            push(kRowAction, PRACTICE_ACT_LOAD_POSITION, S(Str::Pm_LoadPosition));
            push(kRowAction, PRACTICE_ACT_ROUND_RESET, S(Str::Pm_RoundReset));
            break;
        case kPageState:
            push(kRowSetting, PRACTICE_SET_PAUSED, nullptr);
            push(kRowAction, PRACTICE_ACT_FRAME_STEP, S(Str::Pm_FrameStep));
            push(kRowSetting, PRACTICE_SET_MACRO_SLOT, nullptr);
            push(kRowAction, PRACTICE_ACT_MACRO_RECORD, S(Str::Pm_RecordMacro));
            push(kRowAction, PRACTICE_ACT_MACRO_PLAY, S(Str::Pm_PlayMacro));
            push(kRowAction, PRACTICE_ACT_SAVE_STATE, S(Str::Pm_SaveState));
            push(kRowAction, PRACTICE_ACT_LOAD_STATE, S(Str::Pm_LoadState));
            break;
        case kPageExit:
            push(kRowAction, -1, S(Str::Pm_Resume));
            // Vanilla routes this to the replay list during playback
            // (sub_4CA120 sets match+10 = 3 for game type 5), so name it so.
            push(kRowAction, -2,
                 GetGameType() == GAMETYPE_REPLAY ? S(Str::Pm_ReplayList)
                                                  : S(Str::Pm_CharacterSelect));
            // Title Screen removed: its route is the buggy one, and Exit Match
            // reaches the same place reliably.
            push(kRowAction, -3, S(Str::Pm_ExitMatch));
            break;
        default:
            break;
    }

    return n;
}

// The practice tabs only mean anything in training: the native rows write
// training-only addresses and the mod's dummy automation is gated to it.
// Everywhere else the menu is the Exit tab alone, still an improvement on nine
// greyed-out Japanese rows.
bool PageAvailable(int page) {
    return page == kPageExit || GetGameType() == GAMETYPE_TRAINING;
}

int FirstAvailablePage() {
    for (int p = 0; p < kPageCount; ++p) {
        if (PageAvailable(p)) {
            return p;
        }
    }
    return kPageExit;
}

bool RowEnabled(const Row& row) {
    if (row.kind == kRowSetting) {
        return PracticeSetting_Enabled(row.id);
    }
    return true;
}

// --- Drawing -------------------------------------------------------------

void DrawText(int x, int y, const uint8_t ink[3], float size, const char* text) {
    NetMenu::MenuDrawTextSized(x, y + TextYOffset(size), ink[0], ink[1], ink[2], size, text);
}

// Truncates to the column instead of overlapping the next one. Measured against
// the real atlas, so it holds for any future label without a hand-tuned limit.
void DrawTextClipped(int x, int y, const uint8_t ink[3], float size,
                     const char* text, int maxWidth) {
    if (!text || !text[0]) {
        return;
    }
    if (NetMenu::MenuMeasureText(text, size) <= (float)maxWidth) {
        DrawText(x, y, ink, size, text);
        return;
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "%s", text);
    size_t len = strlen(buf);
    while (len > 1) {
        --len;
        buf[len] = '\0';
        // Leave room for the ellipsis the truncated string will carry.
        if (NetMenu::MenuMeasureText(buf, size) <= (float)maxWidth -
            NetMenu::MenuMeasureText("...", size)) {
            break;
        }
    }
    snprintf(buf + len, sizeof(buf) - len, "...");
    DrawText(x, y, ink, size, buf);
}

void RenderRow(int index, const Row& row, bool selected) {
    const int y = kRowFirstY + kRowPitch * index;
    const bool enabled = RowEnabled(row);
    const uint8_t* ink = enabled ? (selected ? kInkSelected : kInkNormal)
                                 : (selected ? kInkDisabledSel : kInkDisabled);

    const char* label = row.label;
    if (!label && row.kind == kRowSetting) {
        label = PracticeSetting_Label(row.id);
    }
    if (!label) {
        label = "?";
    }

    const bool triggerRow = row.kind == kRowSetting &&
                            row.id >= PRACTICE_SET_TRIGGER_1 &&
                            row.id <= PRACTICE_SET_TRIGGER_6;
    if (triggerRow) {
        const bool on = PracticeSetting_Get(row.id) != 0;
        DrawTextClipped(kLabelX, y, ink, kRowTextSize, label, kTrigLabelWidth);
        DrawTextClipped(kTrigStateX, y, ink, kRowTextSize,
                        on ? S(Str::Common_On) : S(Str::Common_Off), kTrigStateWidth);
        if (on) {
            // Only meaningful once it is on, and dimmer than the state so the
            // eye reads the toggle first.
            const uint8_t* actionInk = selected ? ink : kInkDisabled;
            DrawTextClipped(kTrigActionX, y, actionInk, kRowTextSize,
                            PracticeTrigger_Summary(row.id - PRACTICE_SET_TRIGGER_1),
                            kTrigActionWidth);
        }
        return;
    }

    // Action and Back rows draw nothing in the value column, so their label owns
    // the whole line instead of stopping at the label/value split - which is
    // what was truncating "Character Select" with half the row still empty.
    const bool hasValueColumn = (row.kind == kRowSetting || row.kind == kRowSubPage);
    DrawTextClipped(kLabelX, y, ink, kRowTextSize, label,
                    hasValueColumn ? kLabelWidth : (kValueRight - kLabelX));

    if (row.kind == kRowSetting) {
        if (g_numericEditing && g_numericSetting == row.id) {
            // Caret makes it obvious the row is taking keystrokes.
            char buf[24];
            snprintf(buf, sizeof(buf), "%s_", g_numericBuffer);
            const uint8_t edit[3] = { 255, 224, 140 };
            DrawTextClipped(kValueX, y, edit, kRowTextSize, buf, kValueWidth);
            return;
        }
        DrawTextClipped(kValueX, y, ink, kRowTextSize,
                        PracticeSetting_ValueText(row.id), kValueWidth);
    } else if (row.kind == kRowSubPage) {
        DrawTextClipped(kValueX, y, ink, kRowTextSize, ">", kValueWidth);
    }
}

// Laid out from real advance widths, so renaming a tab cannot overlap another.
void RenderTabs(bool focused) {
    int x = kLabelX;
    for (int p = 0; p < kPageTabCount; ++p) {
        if (!PageAvailable(p)) {
            continue;
        }
        const int width = (int)(NetMenu::MenuMeasureText(S(kPageNames[p]), kTabTextSize) + 0.5f);
        const bool current = (p == g_page) ||
                             (g_page == kPageRecoveryAdvanced && p == kPageRecovery) ||
                             (g_page == kPageHud && p == kPageDisplay);
        if (current) {
            // An underline carries the selection when the strip is unfocused,
            // so the tab is still readable while the cursor is down in the rows.
            NetMenu::MenuSetBlend(1, focused ? 0xC0 : 0x70);
            NetMenu::MenuFillRect(x, kTabBandBottom - 5, x + width, kTabBandBottom - 3,
                                  focused ? 255 : 210, focused ? 60 : 210, focused ? 60 : 210);
            NetMenu::MenuSetBlend(0, 0xFF);
            NetMenu::MenuSetTextAlpha(0xFF);
        }
        const uint8_t* ink = current ? (focused ? kInkSelected : kInkNormal)
                                     : kInkDisabled;
        DrawText(x, kTabY, ink, kTabTextSize, S(kPageNames[p]));
        x += width + kTabGap;
    }
}

// Small arrows when a page holds more rows than the window shows, so the player
// can tell there is more rather than assuming the list ends.
void RenderScrollMarks(int first, int count) {
    if (count <= kRowCount) {
        return;
    }
    const uint8_t dim[3] = { 176, 176, 176 };
    if (first > 0) {
        DrawText(kValueRight + 4, kRowFirstY, dim, kHintTextSize, "^");
    }
    if (first + kRowCount < count) {
        DrawText(kValueRight + 4, kRowFirstY + kRowPitch * (kRowCount - 1),
                 dim, kHintTextSize, "v");
    }
}

void RenderHint() {
    const uint8_t dim[3] = { 176, 176, 176 };
    if (g_hintFrames > 0 && g_hint[0]) {
        const uint8_t warm[3] = { 255, 224, 140 };
        DrawText(kLabelX, kHintY, warm, kHintTextSize, g_hint);
        return;
    }
    if (g_numericEditing) {
        const uint8_t edit[3] = { 255, 224, 140 };
        DrawText(kLabelX, kHintY, edit, kHintTextSize, S(Str::Pm_HintNumeric));
        return;
    }
    DrawText(kLabelX, kHintY, dim, kHintTextSize, S(Str::Pm_HintDefault));
}

void RenderBacking() {
    // Vanilla's own dim first, over the whole screen.
    NetMenu::MenuSetBlend(1, 0x80);
    NetMenu::MenuFillRect(0, 0, 639, 479, 0, 0, 0);

    // Then a lighter pass behind the rows and a heavier one behind the strip and
    // the hint, where the text is small enough to lose against a busy stage.
    NetMenu::MenuSetBlend(1, 0x40);
    NetMenu::MenuFillRect(kBandLeft, kRowBandTop, kBandRight, kRowBandBottom, 0, 0, 0);
    NetMenu::MenuSetBlend(1, 0x70);
    NetMenu::MenuFillRect(kBandLeft, kTabBandTop, kBandRight, kTabBandBottom, 0, 0, 0);
    NetMenu::MenuFillRect(kBandLeft, kHintBandTop, kBandRight, kHintBandBottom, 0, 0, 0);

    // Hairlines mark where one band ends and the next begins.
    NetMenu::MenuSetBlend(1, 0x50);
    NetMenu::MenuFillRect(kBandLeft, kTabBandBottom - 1, kBandRight, kTabBandBottom, 210, 210, 210);
    NetMenu::MenuFillRect(kBandLeft, kHintBandTop, kBandRight, kHintBandTop + 1, 210, 210, 210);

    NetMenu::MenuSetBlend(0, 0xFF);
    NetMenu::MenuSetTextAlpha(0xFF);
}

// The dummy's own moves when the guide knows them, otherwise the full motion
// list with a separate button field.
bool PopupUsesMoveList() {
    return PracticeTrigger_MoveCount() > 0;
}

int PopupEntryCount() {
    return PopupUsesMoveList() ? PracticeTrigger_MoveCount()
                               : PracticeTrigger_MotionCount();
}

const char* PopupEntryLabel(int i) {
    return PopupUsesMoveList() ? PracticeTrigger_MoveLabel(i)
                               : PracticeTrigger_MotionLabel(i);
}

int PopupSelectedEntry() {
    return PopupUsesMoveList() ? PracticeTrigger_SelectedMove()
                               : PracticeTrigger_GetMotion();
}

void PopupApplyEntry(int i) {
    if (PopupUsesMoveList()) {
        PracticeTrigger_SelectMove(i);
    } else {
        PracticeTrigger_SetMotion(i);
    }
}

void RenderPopup() {
    const int motions = PopupEntryCount();
    if (motions > kPopupGridCapacity) {
        // Never silently drop the tail: an entry the cursor cannot reach is a
        // move the player cannot pick.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOG_WARN("[PauseMenu] motion popup holds %d of %d entries; the rest are "
                     "unreachable - widen the grid", kPopupGridCapacity, motions);
        }
    }
    const int gridRows = (motions + kPopupCols - 1) / kPopupCols;

    // Dim the menu behind it hard enough that the popup reads as the only live
    // surface, then a border so its edge is unambiguous.
    NetMenu::MenuSetBlend(1, 0xB0);
    NetMenu::MenuFillRect(kPopupLeft, kPopupTop, kPopupRight, kPopupBottom, 0, 0, 0);
    NetMenu::MenuSetBlend(1, 0x60);
    NetMenu::MenuFillRect(kPopupLeft, kPopupTop, kPopupRight, kPopupTop + 2, 220, 220, 220);
    NetMenu::MenuFillRect(kPopupLeft, kPopupBottom - 2, kPopupRight, kPopupBottom, 220, 220, 220);
    NetMenu::MenuFillRect(kPopupLeft, kPopupTop, kPopupLeft + 2, kPopupBottom, 220, 220, 220);
    NetMenu::MenuFillRect(kPopupRight - 2, kPopupTop, kPopupRight, kPopupBottom, 220, 220, 220);
    NetMenu::MenuSetBlend(0, 0xFF);
    NetMenu::MenuSetTextAlpha(0xFF);

    const uint8_t title[3] = { 255, 224, 140 };
    DrawText(kPopupGridX, kPopupTop + 28, title, kRowTextSize,
             PracticeTrigger_Name(PracticeTrigger_SelectedSlot()));

    const int motion = PopupSelectedEntry();
    for (int i = 0; i < motions; ++i) {
        const int col = i % kPopupCols;
        const int row = i / kPopupCols;
        const int x = kPopupGridX + col * kPopupColPitch;
        const int y = kPopupGridY + row * kPopupRowPitch;
        const bool cursor = (g_popupCursor == i);
        const uint8_t* ink = cursor ? kInkSelected
                                    : (i == motion ? kInkNormal : kInkDisabled);
        DrawTextClipped(x, y, ink, kPopupTextSize,
                        PopupEntryLabel(i), kPopupColPitch - 8);
    }

    const int fieldY = kPopupGridY + gridRows * kPopupRowPitch + 12;
    const bool onButton = !PopupUsesMoveList() && (g_popupCursor == motions + kPopupFieldButton);
    const bool onDelay = PopupUsesMoveList() ? (g_popupCursor == motions)
                                             : (g_popupCursor == motions + kPopupFieldDelay);

    if (!PopupUsesMoveList()) {
        DrawText(kPopupGridX, fieldY, onButton ? kInkSelected : kInkNormal,
                 kPopupTextSize, PracticeSetting_Label(PRACTICE_SET_TRIGGER_BUTTON));
        DrawText(kPopupGridX + 120, fieldY, onButton ? kInkSelected : kInkNormal,
                 kPopupTextSize, PracticeSetting_ValueText(PRACTICE_SET_TRIGGER_BUTTON));
    }

    DrawText(kPopupGridX + 260, fieldY, onDelay ? kInkSelected : kInkNormal,
             kPopupTextSize, PracticeSetting_Label(PRACTICE_SET_TRIGGER_DELAY));
    DrawText(kPopupGridX + 360, fieldY, onDelay ? kInkSelected : kInkNormal,
             kPopupTextSize, PracticeSetting_ValueText(PRACTICE_SET_TRIGGER_DELAY));

    const uint8_t dim[3] = { 176, 176, 176 };
    DrawText(kPopupGridX, kPopupBottom - 20, dim, kHintTextSize, S(Str::Pm_HintPopup));
}

void RenderMenu() {
    RenderBacking();

    if (g_popupOpen) {
        // Modal. The popup's backdrop is drawn with the game's own primitives,
        // which paint before the queued overlay text, so drawing the rows as
        // well would show them straight through it.
        RenderPopup();
        return;
    }

    RenderTabs(g_cursor == 0);

    Row rows[kRowMax];
    const int count = BuildRows(g_page, rows, kRowMax);
    const int first = ClampInt(g_rowScroll, 0, count > kRowCount ? count - kRowCount : 0);
    const int shown = (count - first) < kRowCount ? (count - first) : kRowCount;
    for (int i = 0; i < shown; ++i) {
        RenderRow(i, rows[first + i], (first + i + 1) == g_cursor);
    }
    RenderScrollMarks(first, count);
    RenderHint();
}

// --- Numeric entry -------------------------------------------------------

bool JustPressed(uint16_t button);

void BeginNumericEdit(int setting) {
    g_numericEditing = true;
    g_numericSetting = setting;
    snprintf(g_numericBuffer, sizeof(g_numericBuffer), "%d", PracticeSetting_Get(setting));
    memset(g_numericPrevKeyDown, 0, sizeof(g_numericPrevKeyDown));
}

void EndNumericEdit(bool commit) {
    if (commit && g_numericSetting >= 0) {
        PracticeSetting_SetNumeric(g_numericSetting, atoi(g_numericBuffer));
    }
    g_numericEditing = false;
    g_numericSetting = -1;
    g_numericBuffer[0] = '\0';
}

// Returns true while the field owns the input. Digits, backspace, Enter and
// Escape only - there is nothing else a number needs.
bool UpdateNumericEdit() {
    if (!g_numericEditing) {
        return false;
    }

    for (int vk = 0; vk < 256; ++vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_numericPrevKeyDown[vk]) {
            const size_t len = strlen(g_numericBuffer);
            if (vk == VK_RETURN) {
                EndNumericEdit(true);
                return true;
            }
            if (vk == VK_ESCAPE) {
                EndNumericEdit(false);
                return true;
            }
            if (vk == VK_BACK) {
                if (len > 0) {
                    g_numericBuffer[len - 1] = '\0';
                }
            } else if (len + 1 < sizeof(g_numericBuffer)) {
                char digit = 0;
                if (vk >= '0' && vk <= '9') {
                    digit = (char)vk;
                } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
                    digit = (char)('0' + (vk - VK_NUMPAD0));
                }
                if (digit) {
                    g_numericBuffer[len] = digit;
                    g_numericBuffer[len + 1] = '\0';
                }
            }
        }
        g_numericPrevKeyDown[vk] = down;
    }

    // A pad can still confirm or cancel, so this is not keyboard-only.
    if (JustPressed(INPUT_A) || JustPressed(INPUT_C) || JustPressed(INPUT_START)) {
        EndNumericEdit(true);
    } else if (JustPressed(INPUT_B)) {
        EndNumericEdit(false);
    }
    return true;
}

// --- Input ---------------------------------------------------------------

// Vanilla accepts either player's pad; keep that. The practice runtime releases
// its input overrides while this menu is up, so P2 is a human pad here and not
// the dummy's own automation.
//
// Every row and every direction is edge-triggered, exactly like the vanilla
// menu: no auto-repeat, so a held direction moves the cursor once.
bool JustPressed(uint16_t button) {
    return InputSystem_JustPressed(0, button) || InputSystem_JustPressed(1, button);
}

void SetHint(const char* text) {
    if (!text) {
        g_hint[0] = '\0';
        g_hintFrames = 0;
        return;
    }
    snprintf(g_hint, sizeof(g_hint), "%s", text);
    g_hintFrames = 90;
}

void CycleTab(int delta) {
    int page = g_page < kPageTabCount ? g_page : kPageRecovery;
    for (int guard = 0; guard < kPageTabCount; ++guard) {
        page = (page + delta + kPageTabCount) % kPageTabCount;
        if (PageAvailable(page)) {
            break;
        }
    }
    g_page = page;
}

int ConfirmRow(const Row& row) {
    switch (row.kind) {
        case kRowSubPage:
        case kRowBack:
            g_page = row.id;
            g_cursor = 1;
            return kResultStay;
        case kRowSetting:
            // An enabled trigger opens its action editor; everything else just
            // advances, so a toggle needs one button rather than two.
            if (row.id >= PRACTICE_SET_TRIGGER_1 && row.id <= PRACTICE_SET_TRIGGER_6 &&
                PracticeSetting_Get(row.id) != 0 && PracticeSetting_Enabled(row.id)) {
                PracticeTrigger_SelectSlot(row.id - PRACTICE_SET_TRIGGER_1);
                const int selected = PopupSelectedEntry();
                g_popupCursor = selected >= 0 ? selected : 0;
                g_popupOpen = true;
                return kResultStay;
            }
            if (PracticeSetting_IsNumeric(row.id) && PracticeSetting_Enabled(row.id)) {
                BeginNumericEdit(row.id);
                return kResultStay;
            }
            PracticeSetting_Cycle(row.id, 1);
            return kResultStay;
        case kRowAction:
            if (row.id == -1) return kResultResume;
            if (row.id == -2) return kResultCharSelect;
            if (row.id == -3) return kResultExitMatch;
            SetHint(PracticeAction_Invoke(row.id));
            // Several actions also raise a toast. For a row the player pressed
            // here, the action's own return is the message worth showing, so
            // consume the serial rather than letting it overwrite next frame.
            PracticeTools_LatestStatus(&g_statusSerial);
            return kResultStay;
        default:
            return kResultStay;
    }
}

// Returns true while the popup owns the input.
bool UpdatePopup() {
    if (!g_popupOpen) {
        return false;
    }

    const int motions = PopupEntryCount();
    const int gridRows = (motions + kPopupCols - 1) / kPopupCols;
    // With a move list the button is part of the entry, so only Delay remains.
    const int fields = PopupUsesMoveList() ? 1 : kPopupFieldCount;
    const int total = motions + fields;

    if (g_popupCursor >= total) {
        g_popupCursor = total - 1;
    }

    const bool inGrid = g_popupCursor < motions;
    const int hDelta = JustPressed(INPUT_RIGHT) ? 1 : (JustPressed(INPUT_LEFT) ? -1 : 0);

    if (JustPressed(INPUT_DOWN)) {
        if (inGrid) {
            const int next = g_popupCursor + kPopupCols;
            g_popupCursor = (next < motions) ? next : motions;
        } else if (fields > 1 && g_popupCursor == motions + kPopupFieldButton) {
            g_popupCursor = motions + kPopupFieldDelay;
        } else {
            g_popupCursor = 0;
        }
    } else if (JustPressed(INPUT_UP)) {
        if (inGrid) {
            const int prev = g_popupCursor - kPopupCols;
            g_popupCursor = (prev >= 0) ? prev : motions + fields - 1;
        } else if (fields > 1 && g_popupCursor == motions + kPopupFieldDelay) {
            g_popupCursor = motions + kPopupFieldButton;
        } else {
            // Back into the last populated grid row, same column where possible.
            const int lastRowStart = (gridRows - 1) * kPopupCols;
            g_popupCursor = (lastRowStart < motions) ? lastRowStart : motions - 1;
        }
    } else if (hDelta != 0) {
        if (g_popupCursor < motions) {
            g_popupCursor = (g_popupCursor + hDelta + motions) % motions;
        } else if (!PopupUsesMoveList() && g_popupCursor == motions + kPopupFieldButton) {
            PracticeSetting_Cycle(PRACTICE_SET_TRIGGER_BUTTON, hDelta);
        } else {
            PracticeSetting_Cycle(PRACTICE_SET_TRIGGER_DELAY, hDelta);
        }
    }

    // Moving in the grid IS the choice, so the motion follows the cursor and
    // there is nothing to confirm.
    if (g_popupCursor < motions) {
        PopupApplyEntry(g_popupCursor);
    }

    if (JustPressed(INPUT_B) || JustPressed(INPUT_START) ||
        JustPressed(INPUT_A) || JustPressed(INPUT_C)) {
        g_popupOpen = false;
    }
    return true;
}

int UpdateMenu() {
    if (g_hintFrames > 0) {
        --g_hintFrames;
    }

    // Status messages raised outside the menu land on the same line. Polled
    // before input so a row's own result, set below, still wins for that frame.
    uint32_t serial = 0;
    const char* status = PracticeTools_LatestStatus(&serial);
    if (serial != g_statusSerial) {
        g_statusSerial = serial;
        if (status && status[0]) {
            SetHint(status);
        }
    }

    if (UpdateNumericEdit()) {
        return kResultStay;
    }

    if (UpdatePopup()) {
        return kResultStay;
    }

    Row rows[kRowMax];
    int count = BuildRows(g_page, rows, kRowMax);

    // Cursor 0 is the tab strip; 1..count are the rows. Up from the first row
    // reaches the strip with nothing but a d-pad, so no shoulder button has to
    // be bound for tabs to work.
    const int entries = count + 1;
    if (g_cursor >= entries) {
        g_cursor = entries - 1;
    }

    // Disabled rows stay selectable, exactly like vanilla's greyed ones, so the
    // list never silently skips a row the player can see.
    if (JustPressed(INPUT_UP)) {
        g_cursor = (g_cursor - 1 + entries) % entries;
    }
    if (JustPressed(INPUT_DOWN)) {
        g_cursor = (g_cursor + 1) % entries;
    }

    // Follow the cursor with the window, one row at a time, and pin it to the
    // top whenever the whole page fits.
    const int maxScroll = (count > kRowCount) ? (count - kRowCount) : 0;
    if (g_cursor == 0) {
        g_rowScroll = 0;
    } else {
        const int row = g_cursor - 1;
        if (row < g_rowScroll) {
            g_rowScroll = row;
        } else if (row >= g_rowScroll + kRowCount) {
            g_rowScroll = row - kRowCount + 1;
        }
    }
    g_rowScroll = ClampInt(g_rowScroll, 0, maxScroll);

    const int hDelta = JustPressed(INPUT_RIGHT) ? 1 : (JustPressed(INPUT_LEFT) ? -1 : 0);
    if (hDelta != 0) {
        if (g_cursor == 0) {
            CycleTab(hDelta);
        } else {
            const Row& target = rows[g_cursor - 1];
            if (target.kind == kRowSetting) {
                PracticeSetting_Cycle(target.id, hDelta);
            }
        }
    }

    // A value change can reshape the list (the Dummy tab swaps its lower half
    // with the backend), so re-read before acting on the cursor.
    count = BuildRows(g_page, rows, kRowMax);
    if (g_cursor > count) {
        g_cursor = count;
    }

    if (JustPressed(INPUT_A) || JustPressed(INPUT_C)) {
        if (g_cursor == 0) {
            g_cursor = 1;            // step down into the tab's rows
            return kResultStay;
        }
        return ConfirmRow(rows[g_cursor - 1]);
    }

    if (JustPressed(INPUT_B)) {
        g_cursor = 0;                // back up to the tab strip
        return kResultStay;
    }

    if (JustPressed(INPUT_START)) {
        return kResultResume;
    }

    return kResultStay;
}

bool ShouldOwn() {
    if (!g_installed) {
        return false;
    }
    if (GetGameMode() != MODE_MATCH) {
        return false;
    }
    // Netplay never reaches substate 4 in vanilla; do not start now.
    if (GetGameType() == GAMETYPE_NETPLAY) {
        return false;
    }
    return true;
}

void OnMenuOpened() {
    if (!PageAvailable(g_page)) {
        g_page = FirstAvailablePage();
    }
    g_cursor = 1;
    g_rowScroll = 0;
    g_popupOpen = false;
    EndNumericEdit(false);
    SetHint(nullptr);
    // Adopt the current serial without showing it: opening the menu is not the
    // moment to replay whatever was last toasted before it opened.
    PracticeTools_LatestStatus(&g_statusSerial);
    g_pendingResult = kResultStay;

    // Full state dump on every open. A report of "this setting does nothing"
    // is only actionable next to what was actually selected, which side the
    // dummy was, and whether the hook that would have carried it out installed.
    PracticeTools_LogDiagnosticSnapshot("pause menu opened");
}

// --- Hooks ---------------------------------------------------------------

int __cdecl Hook_PauseInput(int match) {
    if (!ShouldOwn()) {
        g_wasActiveLastFrame = false;
        return g_origPauseInput ? g_origPauseInput(match) : kResultStay;
    }

    if (!g_wasActiveLastFrame) {
        OnMenuOpened();
        g_wasActiveLastFrame = true;
    }

    g_pendingResult = UpdateMenu();
    if (g_pendingResult != kResultStay) {
        g_wasActiveLastFrame = false;
    }
    return g_pendingResult;
}

int __cdecl Hook_PauseRender(int* match) {
    if (!ShouldOwn()) {
        return g_origPauseRender ? g_origPauseRender(match) : 0;
    }
    RenderMenu();
    return 0;
}

bool CreateAndEnable(uintptr_t target, void* detour, void** original, const char* label) {
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
    if (status != MH_OK) {
        snprintf(g_installError, sizeof(g_installError), "%s create failed (%d)", label, (int)status);
        LOG_ERROR("[PauseMenu] %s", g_installError);
        return false;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        snprintf(g_installError, sizeof(g_installError), "%s enable failed (%d)", label, (int)status);
        LOG_ERROR("[PauseMenu] %s", g_installError);
        return false;
    }
    LOG_INFO("[PauseMenu] hooked %s @ 0x%08X", label, (unsigned)target);
    return true;
}

} // namespace

bool PauseMenu_Install() {
    if (g_installed) {
        return true;
    }
    g_installError[0] = '\0';
    PauseMenu_Reset();

    if (!CreateAndEnable(ADDR_PAUSE_MENU_INPUT,
                         reinterpret_cast<void*>(&Hook_PauseInput),
                         reinterpret_cast<void**>(&g_origPauseInput),
                         "pause input")) {
        return false;
    }
    if (!CreateAndEnable(ADDR_PAUSE_MENU_RENDER,
                         reinterpret_cast<void*>(&Hook_PauseRender),
                         reinterpret_cast<void**>(&g_origPauseRender),
                         "pause render")) {
        MH_DisableHook(reinterpret_cast<void*>(ADDR_PAUSE_MENU_INPUT));
        return false;
    }

    g_installed = true;
    return true;
}

void PauseMenu_Uninstall() {
    if (!g_installed) {
        return;
    }
    MH_DisableHook(reinterpret_cast<void*>(ADDR_PAUSE_MENU_INPUT));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_PAUSE_MENU_RENDER));
    g_installed = false;
}

bool PauseMenu_IsInstalled() {
    return g_installed;
}

const char* PauseMenu_GetInstallError() {
    return g_installError;
}

bool PauseMenu_IsActive() {
    return g_wasActiveLastFrame;
}

void PauseMenu_Reset() {
    g_page = kPageDummy;
    g_cursor = 1;
    g_rowScroll = 0;
    g_popupOpen = false;
    g_popupCursor = 0;
    g_numericEditing = false;
    g_numericSetting = -1;
    g_numericBuffer[0] = '\0';
    g_wasActiveLastFrame = false;
    g_hint[0] = '\0';
    g_hintFrames = 0;
    g_pendingResult = kResultStay;
}
