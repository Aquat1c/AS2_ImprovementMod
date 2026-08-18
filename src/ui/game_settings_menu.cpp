#include "ui/game_settings_menu.h"

#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

#include "core/as2_constants.h"
#include "net/game_settings_sync.h"
#include "net/netplay_menu_render.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include "core/game_state.h"
#include "net/mode_ownership.h"
#include "net/netplay_menu_controller.h"
#include "input/input_system.h"

namespace NetMenu {

namespace {

// Working-buffer offsets in the native options screen map straight onto these
// globals (decomp:200315 copy-in, decomp:200404 copy-back).
constexpr uintptr_t kAddrDifficulty      = 0x8E93EC; // +48, 0..2
constexpr uintptr_t kAddrRounds          = 0x8E93ED; // +49, 0..2 => 1..3 wins
constexpr uintptr_t kAddrStageSelect     = 0x8E93EE; // +50, flag
constexpr uintptr_t kAddrSimpleEffects   = 0x8E93EF; // +51, flag
constexpr uintptr_t kAddrBattleRecording = 0x8E93F0; // +52, flag
constexpr uintptr_t kAddrVoiceVolumes    = 0x8E93F1; // +53, 23 entries, 0..10
constexpr uintptr_t kAddrSeVolume        = 0x8E9409; // +76, 0..10
constexpr uintptr_t kAddrBgmVolume       = 0x8E940A; // +77, 0..10
constexpr uintptr_t kAddrSystemVoice     = 0x8E940B; // +78, 0..18, unlock-gated
constexpr uintptr_t kAddrAiLearning      = 0x8E940D; // +80, flag

constexpr int kVoiceEntryCount = 23; // 0x8E93F1..0x8E9407 inclusive
constexpr int kSystemVoiceMax  = 18;
constexpr int kVolumeMax       = 10;

// Native geometry (decomp sub_55D6F0): panel from 16,16, 32px row pitch, labels
// at x=32, values at x=208, highlight bar spanning 32..335.
constexpr int kPanelLeft   = 16;
constexpr int kPanelTop    = 16;
constexpr int kPanelRight  = 383;  // vanilla general settings panel (decomp sub_55D6F0)
constexpr int kRowPitch    = 32;  // vanilla row pitch; 22 crammed the rows together
constexpr int kLabelX      = 32;
constexpr int kValueX      = 208;  // vanilla value column
constexpr int kBarLeft     = 32;
constexpr int kBarRight    = 335;  // vanilla selection bar right edge
constexpr float kSettingsTextSize = 22.0f; // vanilla glyphs fill ~2/3 of the 32px band
constexpr float kKeyTextSize      = 19.0f; // key config rows are 22px, so it stays smaller
constexpr float kKeyPadSize       = 15.0f; // the pad half of a binding, set below the key
constexpr float kRootLabelSize    = 26.0f; // the category page sits a step above
constexpr float kRootHintSize     = 17.0f;
// Vanilla's category screen is a step up from its settings screen: 47px glyphs
// on 64px rows vs 17-23px on 32px. 44 keeps that hierarchy without the hint
// column running off a 383-wide panel.
constexpr int kRootRowPitch = 52;  // label + hint stack, both inside the bar

// Vanilla label sprites sample as pure neutral grey (222/214/212/207, R=G=B)
// peaking at white, so nothing here carries a warm tint.
constexpr uint8_t kInk       = 232;  // primary text
constexpr uint8_t kInkBright = 255;  // headings and the selected row
constexpr uint8_t kInkDim    = 168;  // hints and secondary values
constexpr uint8_t kInkFaint  = 128;  // disabled
constexpr int kTextYOffset = 8;  // centres 19px text in the 32px band  // centres our text in the 32px band
// The header needs its own band, otherwise row 0 draws straight through it.
constexpr int kHeaderY     = kPanelTop + 6;
constexpr int kFirstRowTop = kPanelTop + 40;

// Top of a row's band, and the baseline for its text.
inline int RowTop(int index, int pitch)  { return kFirstRowTop + index * pitch; }
inline int RowText(int index, int pitch) { return RowTop(index, pitch) + kTextYOffset; }
constexpr int kVoiceWindowRows = 11; // visible rows in the scrolling voice list

// Mode 12 state we drive when handing off to a native sub-screen.
constexpr uintptr_t kAddrGameMode        = 0x81638C;
constexpr uintptr_t kAddrOptionsSubstate = 0x816390;
constexpr uintptr_t kAddrOptionsTimer    = 0x816394;
constexpr uintptr_t kAddrOptionsBuffer   = 0x7AC318;

// Native option sub-screens, in the order the native category menu lists them:
// general(3), key config(4), battle history(5), titles(6).
constexpr int kNativeSubstateBattleHistory = 5;
constexpr int kNativeSubstateTitles        = 6;

int  s_pendingNativeSubstate = -1;
// Set once we have handed off, so we can take the player back to our own
// settings when the native viewer closes instead of dropping them at the title.
bool s_awaitingNativeReturn = false;

typedef int (__cdecl *SetBgmVolume_t)(int level255);
constexpr uintptr_t kAddrSetBgmVolume = 0x62D870;

const char* const kCharacterNames[kVoiceEntryCount] = {
    "Rance", "Hatsune", "Patton", "Seed", "Raysen", "Aria", "Maria", "Shizuka",
    "Fanel", "Miki", "Menad", "Hanny King", "Satsu", "Tiger Joe", "Escalayer",
    "Makutsudo", "Alietta", "Nalzgis", "Demon Rance", "Little Princess",
    "Kenzan", "Kayblis", "System",
};

struct RowDef {
    uintptr_t      addr;
    uint8_t        maxValue;
    const wchar_t* iniKey;
    const char*    name;
};

const RowDef kRows[kGameRowMax] = {
    { kAddrDifficulty,      2,               L"difficulty",       "Difficulty"         },
    { kAddrRounds,          2,               L"round_count",      "Rounds"             },
    { kAddrSimpleEffects,   1,               L"simple_effects",   "Simple Effects"     },
    { kAddrBattleRecording, 1,               L"battle_recording", "Battle Recording"   },
    { 0,                    0,               nullptr,             "Voice Volume"       },
    { kAddrSeVolume,        kVolumeMax,      L"se_volume",        "SE Volume"          },
    { kAddrBgmVolume,       kVolumeMax,      L"bgm_volume",       "BGM Volume"         },
    { kAddrSystemVoice,     kSystemVoiceMax, L"system_voice",     "System Voice"       },
    { kAddrAiLearning,      1,               L"ai_learning",      "AI Learning"        },
};

wchar_t s_iniPath[MAX_PATH] = {};
bool    s_iniPathResolved = false;

void ResolveIniPath() {
    if (s_iniPathResolved) {
        return;
    }
    s_iniPathResolved = true;

    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        wcscpy_s(s_iniPath, L"as2_rollback_settings.ini");
        return;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t* fwd   = wcsrchr(path, L'/');
    if (!slash || (fwd && fwd > slash)) {
        slash = fwd;
    }
    if (slash) {
        slash[1] = L'\0';
    } else {
        path[0] = L'\0';
    }
    swprintf_s(s_iniPath, L"%lsas2_rollback_settings.ini", path);
}

void PersistInt(const wchar_t* key, int value) {
    if (!key) {
        return;
    }
    ResolveIniPath();
    wchar_t buf[16];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(L"GameSettings", key, buf, s_iniPath);
}

uint8_t ReadRow(const RowDef& def) {
    if (!def.addr) {
        return 0;
    }
    const uint8_t raw = ReadMemory<uint8_t>(def.addr);
    return raw > def.maxValue ? def.maxValue : raw;
}

void ApplyVolumeSideEffect(int row, uint8_t value) {
    // BGM has a live setter; the SE byte is read when a sound is played, so
    // writing it is enough.
    if (row == kGameRowBgmVolume) {
        ((SetBgmVolume_t)kAddrSetBgmVolume)(255 * (int)value / kVolumeMax);
    }
}

void FormatValue(int row, uint8_t value, char* out, size_t outSize) {
    switch (row) {
        case kGameRowDifficulty: {
            static const char* const k[] = { "Easy", "Normal", "Hard" };
            _snprintf_s(out, outSize, _TRUNCATE, "%s", k[value <= 2 ? value : 2]);
            break;
        }
        case kGameRowRounds:
            _snprintf_s(out, outSize, _TRUNCATE, "%d", (int)value + 1);
            break;
        case kGameRowSeVolume:
        case kGameRowBgmVolume:
            _snprintf_s(out, outSize, _TRUNCATE, "%d", (int)value);
            break;
        case kGameRowSystemVoice:
            _snprintf_s(out, outSize, _TRUNCATE, "%s",
                        value < kVoiceEntryCount ? kCharacterNames[value] : "?");
            break;
        case kGameRowVoiceVolume:
            _snprintf_s(out, outSize, _TRUNCATE, "Select");
            break;
        default:
            _snprintf_s(out, outSize, _TRUNCATE, "%s", value ? "On" : "Off");
            break;
    }
}

// Panel and highlight follow the native fill/blend pattern.
void DrawPanel(int rowCount, uint8_t alpha) {
    const int bottom = RowTop(rowCount, kRowPitch) + 8;
    // Twice, because one 50% pass over the bright book background leaves the
    // text hard to read.
    MenuSetBlend(1, (uint8_t)(alpha / 2));
    MenuFillRect(kPanelLeft, kPanelTop, kPanelRight, bottom, 0, 0, 0);
    MenuFillRect(kPanelLeft, kPanelTop, kPanelRight, bottom, 0, 0, 0);
    MenuSetBlend(1, alpha);
}

void DrawHighlight(int visibleIndex, uint8_t alpha) {
    const int top = RowTop(visibleIndex, kRowPitch);
    // Red, the way the vanilla options screen marks the selected row.
    MenuSetBlend(2, (uint8_t)(alpha / 2));
    MenuFillRect(kBarLeft, top, kBarRight, top + kRowPitch - 2, 255, 0, 0);
    MenuSetBlend(1, alpha);
}

} // namespace

int GameSettingsMenu_RowCount() {
    return kGameRowMax + 1; // rows plus Back
}

int GameSettingsMenu_RowAt(int visibleIndex) {
    // Every row is always visible: the native screen hides AI behind an unlock
    // flag, but UnlockAllContent sets that flag on every launch, so the gate
    // could never be false here.
    return (visibleIndex >= 0 && visibleIndex < kGameRowMax) ? visibleIndex : -1;
}

bool GameSettingsMenu_RowOpensSubPage(int visibleIndex) {
    return GameSettingsMenu_RowAt(visibleIndex) == kGameRowVoiceVolume;
}

bool GameSettingsMenu_Locked() {
    return Net::GameSettingsSync_LocalOptionsLocked();
}

void GameSettingsMenu_RenderScreen(uint32_t selectedIndex, uint8_t alpha) {
    const int  rows   = GameSettingsMenu_RowCount();
    const bool locked = GameSettingsMenu_Locked();

    // One extra row of panel so the footer note has room inside it.
    DrawPanel(rows + 1, alpha);
    MenuDrawTextSized(kLabelX, kHeaderY, kInkBright, kInkBright, kInkBright, kSettingsTextSize, "SETTINGS");
    DrawHighlight((int)selectedIndex, alpha);

    for (int i = 0; i < rows - 1; ++i) {
        const int row = GameSettingsMenu_RowAt(i);
        if (row < 0) {
            continue;
        }
        const RowDef& def = kRows[row];
        const int y = RowText(i, kRowPitch);

        const uint8_t shade = locked ? kInkFaint : kInk;
        MenuDrawTextSized(kLabelX, y, shade, shade, shade, kSettingsTextSize, def.name);

        char value[64];
        FormatValue(row, ReadRow(def), value, sizeof(value));
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
                    row == kGameRowVoiceVolume ? "%s >" : "< %s >", value);
        MenuDrawTextSized(kValueX, y,
                     locked ? 130 : 240,
                     locked ? 120 : 230,
                     locked ? 100 : 200, kSettingsTextSize, text);
    }

    const int backY = RowText(rows - 1, kRowPitch);
    MenuDrawTextSized(kLabelX, backY, kInk, kInk, kInk, kSettingsTextSize, "Back");

    if (locked) {
        MenuDrawTextSized(kLabelX, backY + kRowPitch, kInkDim, kInkDim, kInkDim, kSettingsTextSize,
                     "Locked while an online session is active.");
    } else if (GameSettingsMenu_RowAt((int)selectedIndex) == kGameRowAiLearning) {
        // Say so rather than silently ignoring the setting online.
        MenuDrawTextSized(kLabelX, backY + kRowPitch, kInkDim, kInkDim, kInkDim, kSettingsTextSize,
                     "Offline only. Netplay always disables CPU learning.");
    }
}

bool GameSettingsMenu_Adjust(int visibleIndex, bool left, bool right,
                             char* outStatus, size_t outStatusSize) {
    if (outStatus && outStatusSize) {
        outStatus[0] = '\0';
    }
    const int row = GameSettingsMenu_RowAt(visibleIndex);
    if (row < 0 || (!left && !right) || row == kGameRowVoiceVolume) {
        return false;
    }

    const RowDef& def = kRows[row];
    const uint8_t cur = ReadRow(def);
    uint8_t next = cur;

    if (row == kGameRowSystemVoice) {
        // Wraps rather than clamps. No availability check: the native screen
        // skips locked entries, but UnlockAllContent marks them all available.
        int probe = (int)cur + (left ? -1 : 1);
        if (probe < 0) {
            probe = kSystemVoiceMax;
        } else if (probe > kSystemVoiceMax) {
            probe = 0;
        }
        next = (uint8_t)probe;
    } else if (left && cur > 0) {
        next = (uint8_t)(cur - 1);
    } else if (right && cur < def.maxValue) {
        next = (uint8_t)(cur + 1);
    }

    if (next == cur) {
        return false;
    }

    // A netplay session caches the match config; editing it mid-session would
    // diverge from what the peers agreed on.
    if (GameSettingsMenu_Locked()) {
        if (outStatus && outStatusSize) {
            _snprintf_s(outStatus, outStatusSize, _TRUNCATE,
                        "Locked while an online session is active.");
        }
        return false;
    }

    WriteMemory<uint8_t>(def.addr, next);
    ApplyVolumeSideEffect(row, next);
    PersistInt(def.iniKey, (int)next);

    if (outStatus && outStatusSize) {
        char value[64];
        FormatValue(row, next, value, sizeof(value));
        _snprintf_s(outStatus, outStatusSize, _TRUNCATE, "%s: %s", def.name, value);
    }
    LOG_NETPLAY(LOG_INFO, "[GameSettings] %s = %u", def.name, (unsigned)next);
    return true;
}

// ============================================================================
// Key config
// ============================================================================

namespace {

constexpr int kKeyButtonCount = 14;
constexpr int kKeyRowResetP1  = kKeyButtonCount;
constexpr int kKeyRowResetP2  = kKeyButtonCount + 1;
constexpr int kKeyRowBack     = kKeyButtonCount + 2;

// 17 rows do not fit at the 32px pitch, so this screen uses a tighter one and a
// wider panel to hold both player columns.
constexpr int kKeyRowPitch = 22;
constexpr int kKeyPanelRight = 560;
constexpr int kKeyLabelX     = 32;
constexpr int kKeyColumnX[2] = { 240, 404 };
constexpr int kKeyColumnWidth = 160;

const char* const kKeyButtonNames[kKeyButtonCount] = {
    "Up", "Down", "Left", "Right",
    "A (Light)", "B (Medium)", "C (Heavy)", "D (Special)",
    "Start", "Select", "L1", "R1", "L2", "R2",
};

int s_keyColumn = 0; // 0 = P1, 1 = P2
int s_captureRow = -1;
// The confirm button is itself a bound key, so a capture opened by pressing it
// would swallow that same press. Ignore input briefly so the player's finger
// leaves the key first. Input is suppressed while binding is active, so polling
// the input state for a release would read clear immediately and not help.
int s_captureArmDelay = 0;
constexpr int kCaptureArmFrames = 12;

// A binding holds a keyboard key AND a pad button/axis at the same time, so
// these are reported separately instead of whichever one happens to be first.
void DescribeKeyboard(const KeyBinding_t* bind, char* out, size_t outSize) {
    if (bind && bind->keyboard_key > 0) {
        _snprintf_s(out, outSize, _TRUNCATE, "%s",
                    InputSystem_GetKeyName(bind->keyboard_key));
    } else {
        _snprintf_s(out, outSize, _TRUNCATE, "---");
    }
}

void TrimAlternateName(char* text) {
    char* slash = strstr(text, " / ");
    if (slash) {
        *slash = '\0';
    }
}

void DescribePad(const KeyBinding_t* bind, char* out, size_t outSize) {
    if (!bind) {
        out[0] = '\0';
        return;
    }
    if (bind->gamepad_button >= 0) {
        _snprintf_s(out, outSize, _TRUNCATE, "%s",
                    InputSystem_GetGamepadButtonName(bind->gamepad_button));
    } else if (bind->gamepad_axis >= 0) {
        _snprintf_s(out, outSize, _TRUNCATE, "%s",
                    InputSystem_GetGamepadAxisName(bind->gamepad_axis,
                                                   bind->axis_direction));
    } else {
        out[0] = '\0';
    }
}

} // namespace

int GameSettingsKeys_RowCount() {
    return kKeyButtonCount + 3; // buttons, two resets, back
}

bool GameSettingsKeys_CaptureActive() {
    return InputSystem_IsBindingActive();
}

void GameSettingsKeys_CancelCapture() {
    InputSystem_CancelBinding();
    s_captureRow = -1;
    s_captureArmDelay = 0;
}

void GameSettingsKeys_RenderScreen(uint32_t selectedIndex, uint8_t alpha) {
    const int rows = GameSettingsKeys_RowCount();

    const int panelBottom = RowTop(rows, kKeyRowPitch) + 30;
    MenuSetBlend(1, (uint8_t)(alpha / 2));
    MenuFillRect(kPanelLeft, kPanelTop, kKeyPanelRight, panelBottom, 0, 0, 0);
    MenuFillRect(kPanelLeft, kPanelTop, kKeyPanelRight, panelBottom, 0, 0, 0);
    MenuSetBlend(1, alpha);

    MenuDrawTextSized(kKeyLabelX, kHeaderY, kInkBright, kInkBright, kInkBright, kSettingsTextSize, "KEY SETTINGS");
    for (int player = 0; player < 2; ++player) {
        char heading[16];
        _snprintf_s(heading, sizeof(heading), _TRUNCATE, "PLAYER %d", player + 1);
        MenuDrawTextSized(kKeyColumnX[player], kHeaderY, kInk, kInk, kInk,
                          kSettingsTextSize, heading);

        // Whichever pad you rebind with becomes this player's device; the
        // keyboard stays shared, so it only shows when no pad is held.
        const char* dev = InputSystem_HasGamepad(player)
                        ? InputSystem_GetGamepadName(player) : "Keyboard";
        char shown[64];
        _snprintf_s(shown, sizeof(shown), _TRUNCATE, "%s", dev ? dev : "Keyboard");
        MenuDrawTextSized(kKeyColumnX[player], kHeaderY + 20, kInkDim, kInkDim,
                          kInkDim, 13.0f, shown);
    }

    // Highlight only the cell in play, so it is obvious which column an edit
    // would land in.
    const int selRow = (int)selectedIndex;
    const int selTop = RowTop(selRow, kKeyRowPitch);
    MenuSetBlend(2, (uint8_t)(alpha / 2));
    if (selRow < kKeyButtonCount) {
        const int cx = kKeyColumnX[s_keyColumn];
        const int cellRight = (s_keyColumn == 0) ? kKeyColumnX[1] - 12
                                                 : kKeyPanelRight - 16;
        MenuFillRect(cx - 8, selTop, cellRight, selTop + kKeyRowPitch - 2, 255, 0, 0);
    } else {
        MenuFillRect(kKeyLabelX - 8, selTop, kKeyPanelRight - 16,
                     selTop + kKeyRowPitch - 2, 255, 0, 0);
    }
    MenuSetBlend(1, alpha);

    for (int i = 0; i < kKeyButtonCount; ++i) {
        const int y = RowTop(i, kKeyRowPitch) + 3;
        MenuDrawTextSized(kKeyLabelX, y, kInk, kInk, kInk, kKeyTextSize, kKeyButtonNames[i]);

        for (int player = 0; player < 2; ++player) {
            const bool capturing = s_captureRow == i && s_keyColumn == player &&
                                   InputSystem_IsBindingActive();
            if (capturing) {
                MenuDrawTextSized(kKeyColumnX[player], y, kInkBright, kInkBright,
                                  kInkBright, kKeyTextSize, "press...");
                continue;
            }

            const PlayerBindings_t* b = InputSystem_GetBindings(player);
            const KeyBinding_t* bind = InputSystem_GetBindingByIndexConst(b, i);

            // A player holding a pad is playing on the pad, so the keyboard
            // half is noise; show the device that is actually in their hands.
            if (InputSystem_HasGamepad(player)) {
                char pad[48];
                DescribePad(bind, pad, sizeof(pad));
                // Directions are always the d-pad, so naming them adds nothing.
                const char* text = (i < 4) ? "D-Pad" : pad;
                MenuDrawTextSized(kKeyColumnX[player], y, kInk, kInk, kInk,
                                  kKeyTextSize, text[0] ? text : "---");
            } else {
                char kb[48];
                DescribeKeyboard(bind, kb, sizeof(kb));
                MenuDrawTextSized(kKeyColumnX[player], y, kInk, kInk, kInk,
                                  kKeyTextSize, kb);
            }
        }
    }

    MenuDrawTextSized(kKeyLabelX, RowTop(kKeyRowResetP1, kKeyRowPitch) + 3,
                 kInk, kInk, kInk, kSettingsTextSize, "Reset Player 1 to defaults");
    MenuDrawTextSized(kKeyLabelX, RowTop(kKeyRowResetP2, kKeyRowPitch) + 3,
                 kInk, kInk, kInk, kSettingsTextSize, "Reset Player 2 to defaults");
    MenuDrawTextSized(kKeyLabelX, RowTop(kKeyRowBack, kKeyRowPitch) + 3,
                 kInk, kInk, kInk, kSettingsTextSize, "Back");

    const int footY = RowTop(rows, kKeyRowPitch) + 6;
    if (InputSystem_IsBindingActive()) {
        MenuDrawTextSized(kKeyLabelX, footY, kInkBright, kInkBright, kInkBright, kSettingsTextSize,
                     "Press any key or pad button. ESC cancels.");
    } else {
        MenuDrawTextSized(kKeyLabelX, footY, kInkDim, kInkDim, kInkDim, kSettingsTextSize,
                     "Left/Right picks the player, A rebinds.");
    }
}

bool GameSettingsKeys_Adjust(bool left, bool right, char* outStatus, size_t outStatusSize) {
    if (outStatus && outStatusSize) {
        outStatus[0] = '\0';
    }
    if (!left && !right) {
        return false;
    }
    const int next = left ? 0 : 1;
    if (next == s_keyColumn) {
        return false;
    }
    s_keyColumn = next;
    if (outStatus && outStatusSize) {
        _snprintf_s(outStatus, outStatusSize, _TRUNCATE, "Player %d", next + 1);
    }
    return true;
}

bool GameSettingsKeys_Confirm(int row, bool* outClose, char* outStatus, size_t outStatusSize) {
    if (outClose) {
        *outClose = false;
    }
    if (outStatus && outStatusSize) {
        outStatus[0] = '\0';
    }

    if (row >= 0 && row < kKeyButtonCount) {
        s_captureRow = row;
        s_captureArmDelay = kCaptureArmFrames;
        InputSystem_StartBinding(s_keyColumn, row);
        if (outStatus && outStatusSize) {
            _snprintf_s(outStatus, outStatusSize, _TRUNCATE,
                        "Press a key for P%d %s", s_keyColumn + 1,
                        kKeyButtonNames[row]);
        }
        return true;
    }

    if (row == kKeyRowResetP1 || row == kKeyRowResetP2) {
        const int player = (row == kKeyRowResetP1) ? 0 : 1;
        InputSystem_ResetDefaults(player);
        InputSystem_SaveConfig("as2_input.cfg");
        if (outStatus && outStatusSize) {
            _snprintf_s(outStatus, outStatusSize, _TRUNCATE,
                        "Player %d reset to defaults", player + 1);
        }
        return true;
    }

    if (outClose) {
        *outClose = true;
    }
    return true;
}

int GameSettingsRoot_RowCount() {
    return kGameRootCount;
}

void GameSettingsRoot_RenderScreen(uint32_t selectedIndex, uint8_t alpha) {
    static const char* const kLabels[kGameRootCount] = {
        "General Settings",
        "Key Settings",
        "Battle History",
        "Titles",
        "Exit Settings",
    };
    static const char* const kHints[kGameRootCount] = {
        "rules and audio",
        "controls",
        "past results",
        "earned titles",
        "back to title",
    };

    const int bottom = RowTop(kGameRootCount, kRootRowPitch) + 8;
    MenuSetBlend(1, (uint8_t)(alpha / 2));
    MenuFillRect(kPanelLeft, kPanelTop, kPanelRight, bottom, 0, 0, 0);
    MenuFillRect(kPanelLeft, kPanelTop, kPanelRight, bottom, 0, 0, 0);
    MenuSetBlend(1, alpha);

    MenuDrawTextSized(kLabelX, kHeaderY, kInkBright, kInkBright, kInkBright,
                      kSettingsTextSize, "SETTINGS");

    const int selTop = RowTop((int)selectedIndex, kRootRowPitch);
    MenuSetBlend(2, (uint8_t)(alpha / 2));
    MenuFillRect(kBarLeft, selTop, kBarRight, selTop + kRootRowPitch - 2, 255, 0, 0);
    MenuSetBlend(1, alpha);

    for (int i = 0; i < kGameRootCount; ++i) {
        const int top = RowTop(i, kRootRowPitch);
        MenuDrawTextSized(kLabelX, top + 8, kInk, kInk, kInk,
                          kRootLabelSize, kLabels[i]);
        // Hint rides under the label rather than beside it, so a bigger label
        // does not push it out of the panel.
        MenuDrawTextSized(kLabelX + 10, top + 30,
                          kInkDim, kInkDim, kInkDim, kRootHintSize, kHints[i]);
    }
}

void GameSettingsMenu_RequestNativeSubstate(int substate) {
    s_pendingNativeSubstate = substate;
    ModeOwnership::CallOriginalSetGameMode(MODE_OPTIONS, 1);
}

void GameSettingsMenu_FrameUpdate() {
    // Poll every frame while waiting: FinishBinding reports the capture once,
    // and only then.
    if (s_captureRow >= 0) {
        if (s_captureArmDelay > 0) {
            --s_captureArmDelay;
            return;
        }

        KeyBinding_t captured{};
        int source = -1;
        if (InputSystem_FinishBinding(&captured, &source)) {
            PlayerBindings_t* bindings =
                (PlayerBindings_t*)InputSystem_GetBindings(s_keyColumn);
            KeyBinding_t* target =
                InputSystem_GetBindingByIndex(bindings, s_captureRow);
            if (target) {
                // Touch only the half that was pressed, so rebinding a key does
                // not wipe the pad binding on the same action.
                if (source == 0) {
                    target->keyboard_key = captured.keyboard_key;
                } else {
                    target->gamepad_button = captured.gamepad_button;
                    target->gamepad_axis = captured.gamepad_axis;
                    target->axis_direction = captured.axis_direction;
                }
                InputSystem_SaveConfig("as2_input.cfg");
                LOG_NETPLAY(LOG_INFO, "[GameSettings] rebound P%d button %d",
                            s_keyColumn + 1, s_captureRow);
            }
            s_captureRow = -1;
        }
    }

    if (s_pendingNativeSubstate >= 0) {
        // Let mode 12 allocate its own working buffer and settle on its menu
        // before dropping the player into the sub-screen they asked for.
        if (ReadMemory<uint32_t>(kAddrGameMode) != MODE_OPTIONS ||
            ReadMemory<uint32_t>(kAddrOptionsBuffer) == 0 ||
            ReadMemory<uint32_t>(kAddrOptionsSubstate) < 2) {
            return;
        }
        WriteMemory<uint32_t>(kAddrOptionsSubstate, (uint32_t)s_pendingNativeSubstate);
        WriteMemory<uint32_t>(kAddrOptionsTimer, 0);
        LOG_NETPLAY(LOG_INFO, "[GameSettings] entered native options substate %d",
                    s_pendingNativeSubstate);
        s_pendingNativeSubstate = -1;
        s_awaitingNativeReturn = true;
        return;
    }

    if (!s_awaitingNativeReturn) {
        return;
    }

    // The viewer drops back to the native category menu (substate 2) when it
    // closes. Take that as our cue to reclaim the screen.
    const uint32_t mode = ReadMemory<uint32_t>(kAddrGameMode);
    if (mode != MODE_OPTIONS) {
        s_awaitingNativeReturn = false;
        return;
    }
    if (ReadMemory<uint32_t>(kAddrOptionsSubstate) != 2) {
        return;
    }

    s_awaitingNativeReturn = false;
    // Do NOT change the mode by hand here. OpenMenu -> EnterCustomMenuContext
    // does it, and only that path sets the pending-restore flag that makes the
    // game run main-menu substate 0 and reload its sprites. Forcing MODE_MENU
    // first made EnterCustomMenuContext take its already-in-menu branch, which
    // jumps straight to substate 3 and leaves the vanilla menu with no art.
    HandleGameSettingsSelected();
    LOG_NETPLAY(LOG_INFO, "[GameSettings] native viewer closed, back to our settings");
}

int GameSettingsVoice_RowCount() {
    return kVoiceEntryCount + 1;
}

void GameSettingsVoice_RenderScreen(uint32_t selectedIndex, uint8_t alpha) {
    const int rows = GameSettingsVoice_RowCount();

    // 23 entries do not fit on a 480px screen, so scroll a window the way the
    // native list does (decomp sub_55CB90 case 5 keeps a 12-row window).
    int scroll = (int)selectedIndex - (kVoiceWindowRows - 1);
    if (scroll < 0) {
        scroll = 0;
    }
    const int maxScroll = rows - kVoiceWindowRows;
    if (scroll > maxScroll) {
        scroll = maxScroll > 0 ? maxScroll : 0;
    }
    const int visible = rows - scroll < kVoiceWindowRows ? rows - scroll : kVoiceWindowRows;

    DrawPanel(visible, alpha);
    MenuDrawTextSized(kLabelX, kHeaderY, kInkBright, kInkBright, kInkBright, kSettingsTextSize, "VOICE VOLUME");
    DrawHighlight((int)selectedIndex - scroll, alpha);

    for (int slot = 0; slot < visible; ++slot) {
        const int index = scroll + slot;
        const int y = RowText(slot, kRowPitch);

        if (index >= kVoiceEntryCount) {
            MenuDrawTextSized(kLabelX, y, kInk, kInk, kInk, kSettingsTextSize, "Back");
            break;
        }

        MenuDrawTextSized(kLabelX, y, kInk, kInk, kInk, kSettingsTextSize, kCharacterNames[index]);

        char text[32];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "< %u >",
                    (unsigned)ReadMemory<uint8_t>(kAddrVoiceVolumes + index));
        MenuDrawTextSized(kValueX, y, kInkBright, kInkBright, kInkBright, kSettingsTextSize, text);
    }
}

bool GameSettingsVoice_Adjust(int index, bool left, bool right,
                              char* outStatus, size_t outStatusSize) {
    if (outStatus && outStatusSize) {
        outStatus[0] = '\0';
    }
    if (index < 0 || index >= kVoiceEntryCount || (!left && !right)) {
        return false;
    }

    const uintptr_t addr = kAddrVoiceVolumes + index;
    const uint8_t cur = ReadMemory<uint8_t>(addr);
    uint8_t next = cur;
    if (left && cur > 0) {
        next = (uint8_t)(cur - 1);
    } else if (right && cur < kVolumeMax) {
        next = (uint8_t)(cur + 1);
    }
    if (next == cur) {
        return false;
    }

    WriteMemory<uint8_t>(addr, next);

    wchar_t key[32];
    swprintf_s(key, L"voice_volume_%d", index);
    PersistInt(key, (int)next);

    if (outStatus && outStatusSize) {
        _snprintf_s(outStatus, outStatusSize, _TRUNCATE, "%s voice: %u",
                    kCharacterNames[index], (unsigned)next);
    }
    return true;
}

} // namespace NetMenu
