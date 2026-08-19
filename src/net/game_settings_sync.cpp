/**
 * Alice Senki 2 - Netplay game settings sync
 *
 * Decomp notes for the round setting:
 *   - sub_55C4F0 copies byte_8E93ED into the game-options menu row at +0x31.
 *   - sub_55CDE7 clamps that row to 0..2.
 *   - sub_55C8E0 writes the edited row back to byte_8E93ED.
 *   - match code reads LOBYTE(dword_816470), adds 1, and compares it against
 *     each player's win count.
 */

#include "net/game_settings_sync.h"

#include "core/as2_constants.h"
#include "core/local_rematch.h"
#include "net/locked_match_config.h"
#include "patches/memory_utils.h"
#include "input_system.h"
#include "patches/input_override.h"
#include "patches/tick_hooks.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

namespace Net {
namespace {

constexpr uint8_t kMinRoundOption = 0;
constexpr uint8_t kMaxRoundOption = 2;
constexpr uint8_t kFallbackRoundOption = 1; // First to 2 wins.
constexpr uintptr_t kSettingsBlockAAddr = 0x8E93B8;
constexpr size_t kSettingsBlockASize = 0x33;
constexpr uintptr_t kSettingsBlockBAddr = 0x8E93EC;
constexpr size_t kSettingsBlockBSize = 0x20C;
constexpr uintptr_t kGameOptionDifficulty = 0x8E93EC;

static bool    s_sessionCached = false;
static uint8_t s_savedRoundOption = kFallbackRoundOption;
static uint8_t s_savedMatchRoundOption = kFallbackRoundOption;
static uint8_t s_lastAppliedRoundOption = 0xFF;
static char    s_status[128] = "Game settings sync idle.";
static bool    s_initialized = false;
static bool    s_settingsPathResolved = false;
static bool    s_persistentLoaded = false;
static bool    s_persistentApplied = false;
static uint8_t s_persistedRoundOption = kFallbackRoundOption;
static uint8_t s_lastObservedRoundOption = 0xFF;
static uint8_t s_persistedBlockA[kSettingsBlockASize] = {};
static uint8_t s_persistedBlockB[kSettingsBlockBSize] = {};
static wchar_t s_settingsPathW[MAX_PATH] = {};
static char    s_settingsPathUtf8[MAX_PATH * 3] = {};

static void CopyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) src = "";
    strncpy_s(dst, dstSize, src, _TRUNCATE);
}

static void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, args);
    va_end(args);
}

static uint8_t ReadMatchRoundOption() {
    return ReadMemory<uint8_t>(ADDR_MATCH_ROUND_COUNT);
}

static bool IsVanillaConfigLoaded() {
    return ReadMemory<uint32_t>(ADDR_CONFIG_VERSION) == 258;
}

static uint8_t NormalizeRoundOptionQuiet(uint8_t roundOption) {
    return (roundOption >= kMinRoundOption && roundOption <= kMaxRoundOption)
        ? roundOption
        : kFallbackRoundOption;
}

static void LogSettings(const char* event,
                        const char* reason,
                        uint8_t option,
                        uint8_t previousOption,
                        bool wrotePersistent,
                        bool wroteMatch) {
    char label[32] = {};
    GameSettingsSync_FormatRoundLabel(option, label, sizeof(label));
    char previousLabel[32] = {};
    GameSettingsSync_FormatRoundLabel(previousOption, previousLabel, sizeof(previousLabel));

    LOG_NETPLAY(LOG_INFO,
        "[GameSettings] %s: %s -> %s reason=%s wrote_persist=%d wrote_match=%d",
        event ? event : "event",
        previousLabel,
        label,
        reason ? reason : "?",
        wrotePersistent ? 1 : 0,
        wroteMatch ? 1 : 0);
    Rollback::NetplayLog_Write(
        "GSET", -1,
        "%s: previous_raw=%u previous_wins=%d new_raw=%u new_wins=%d reason=%s wrote_persist=%d wrote_match=%d",
        event ? event : "event",
        previousOption,
        GameSettingsSync_RoundsToWin(previousOption),
        option,
        GameSettingsSync_RoundsToWin(option),
        reason ? reason : "?",
        wrotePersistent ? 1 : 0,
        wroteMatch ? 1 : 0);
}

static void ConvertPathForLog() {
    s_settingsPathUtf8[0] = '\0';
    if (!s_settingsPathW[0]) {
        return;
    }

    WideCharToMultiByte(CP_UTF8,
                        0,
                        s_settingsPathW,
                        -1,
                        s_settingsPathUtf8,
                        sizeof(s_settingsPathUtf8),
                        nullptr,
                        nullptr);
}

static void ResolveSettingsPath() {
    if (s_settingsPathResolved) {
        return;
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        wcscpy_s(s_settingsPathW, L"as2_rollback_settings.ini");
        s_settingsPathResolved = true;
        ConvertPathForLog();
        return;
    }

    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t* fwdSlash = wcsrchr(path, L'/');
    if (!slash || (fwdSlash && fwdSlash > slash)) {
        slash = fwdSlash;
    }
    if (slash) {
        slash[1] = L'\0';
    } else {
        path[0] = L'\0';
    }

    swprintf_s(s_settingsPathW,
               L"%lsas2_rollback_settings.ini",
               path);
    s_settingsPathResolved = true;
    ConvertPathForLog();
}

static bool SettingsFileExists() {
    ResolveSettingsPath();
    const DWORD attrs = GetFileAttributesW(s_settingsPathW);
    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void ReadSettingsBlock(uintptr_t addr, uint8_t* out, size_t size) {
    if (!out) return;
    for (size_t i = 0; i < size; ++i) {
        out[i] = ReadMemory<uint8_t>(addr + i);
    }
}

static void WriteSettingsBlock(uintptr_t addr, const uint8_t* data, size_t size) {
    if (!data) return;
    for (size_t i = 0; i < size; ++i) {
        WriteMemory<uint8_t>(addr + i, data[i]);
    }
}

// Simple Effects (0x8E93EF, byte 3 of block B) is forced off and never offered.
//
// It is not a display option. It gates sub_4C47C0, which writes particle state
// and calls rand(); a peer running with it on steps the shared RNG a different
// number of times per frame than a peer running with it off, so a mismatch is a
// guaranteed desync rather than a cosmetic difference. Forcing one value on
// every machine is the only way it cannot be set wrong.
//
// Enforced here rather than at the menu because the raw block restore is what
// reintroduced it: a stale 1 in the ini - written by the setting-id collision
// that briefly let Appearance rows drive this byte - was faithfully restored on
// every launch, which is what removed the super background from the whole game.
constexpr uintptr_t kAddrSimpleEffects = 0x8E93EF;

static void ForceSimpleEffectsOff(const char* reason) {
    const uint8_t current = ReadMemory<uint8_t>(kAddrSimpleEffects);
    if (current == 0) {
        return;
    }
    WriteMemory<uint8_t>(kAddrSimpleEffects, 0);
    LOG_INFO("[GameSettings] Simple Effects was %u, forced to 0 (%s) - it gates "
             "sub_4C47C0's rand() calls and would desync netplay",
             current, reason ? reason : "?");
}

static void CopyCurrentSettingsToPersisted() {
    ForceSimpleEffectsOff("snapshot");
    ReadSettingsBlock(kSettingsBlockAAddr, s_persistedBlockA, sizeof(s_persistedBlockA));
    ReadSettingsBlock(kSettingsBlockBAddr, s_persistedBlockB, sizeof(s_persistedBlockB));
    // Belt and braces: even if the byte were set between the force and the read,
    // the copy that reaches the ini is 0.
    s_persistedBlockB[kAddrSimpleEffects - kSettingsBlockBAddr] = 0;
    s_persistedRoundOption = GameSettingsSync_ReadRoundOption();
    s_lastObservedRoundOption = s_persistedRoundOption;
}

static bool CurrentSettingsMatchPersisted() {
    uint8_t blockA[kSettingsBlockASize] = {};
    uint8_t blockB[kSettingsBlockBSize] = {};
    ReadSettingsBlock(kSettingsBlockAAddr, blockA, sizeof(blockA));
    ReadSettingsBlock(kSettingsBlockBAddr, blockB, sizeof(blockB));
    return memcmp(blockA, s_persistedBlockA, sizeof(blockA)) == 0 &&
        memcmp(blockB, s_persistedBlockB, sizeof(blockB)) == 0;
}

static void BytesToHex(const uint8_t* data, size_t size, char* out, size_t outSize) {
    static const char kHex[] = "0123456789ABCDEF";
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!data || outSize < size * 2 + 1) return;
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    out[size * 2] = '\0';
}

static int HexValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return (int)(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return (int)(ch - L'a') + 10;
    if (ch >= L'A' && ch <= L'F') return (int)(ch - L'A') + 10;
    return -1;
}

static bool ParseHexBytes(const wchar_t* text, uint8_t* out, size_t size) {
    if (!text || !out) return false;
    size_t nibbleCount = 0;
    int high = -1;
    memset(out, 0, size);
    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') {
            continue;
        }
        const int value = HexValue(*p);
        if (value < 0) return false;
        if ((nibbleCount / 2) >= size) return false;
        if (high < 0) {
            high = value;
        } else {
            out[nibbleCount / 2] = (uint8_t)((high << 4) | value);
            high = -1;
        }
        ++nibbleCount;
    }
    return high < 0 && nibbleCount == size * 2;
}

static bool ReadIniString(const wchar_t* section,
                          const wchar_t* key,
                          wchar_t* out,
                          size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = L'\0';
    ResolveSettingsPath();
    GetPrivateProfileStringW(
        section,
        key,
        L"",
        out,
        (DWORD)outCount,
        s_settingsPathW);
    return out[0] != L'\0';
}

static bool ReadIniInt(const wchar_t* section, const wchar_t* key, int* out) {
    if (!out) return false;
    wchar_t value[64] = {};
    if (!ReadIniString(section, key, value, sizeof(value) / sizeof(value[0]))) {
        return false;
    }
    wchar_t* end = nullptr;
    const long parsed = wcstol(value, &end, 10);
    if (end == value) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

static uint8_t ClampByteSetting(int value, int minValue, int maxValue, uint8_t fallback) {
    if (value < minValue || value > maxValue) {
        return fallback;
    }
    return (uint8_t)value;
}

static void ApplyFriendlyIniOverrides() {
    int value = 0;
    if (ReadIniInt(L"GameSettings", L"difficulty", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(kGameOptionDifficulty);
        WriteMemory<uint8_t>(kGameOptionDifficulty,
            ClampByteSetting(value, 0, 2, current));
    }
    if (ReadIniInt(L"GameSettings", L"rounds_to_win", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT);
        const uint8_t option = (value >= 1 && value <= 3)
            ? (uint8_t)(value - 1)
            : current;
        WriteMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT, option);
    } else if (ReadIniInt(L"GameSettings", L"round_count", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT);
        WriteMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT,
            ClampByteSetting(value, 0, 2, current));
    }
    if (ReadIniInt(L"GameSettings", L"stage_select", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_STAGESEL_ENABLE);
        WriteMemory<uint8_t>(ADDR_STAGESEL_ENABLE,
            ClampByteSetting(value, 0, 1, current));
    }

    // Rows the mod's settings menu owns. Ranges mirror the ones the native
    // options screen enforces (decomp sub_55CB90).
    struct IniByteSetting {
        const wchar_t* key;
        uintptr_t      addr;
        int            maxValue;
    };
    static const IniByteSetting kIniBytes[] = {
        { L"battle_recording", 0x8E93F0, 1  },
        { L"se_volume",        0x8E9409, 10 },
        { L"bgm_volume",       0x8E940A, 10 },
        { L"system_voice",     0x8E940B, 18 },
        { L"ai_learning",      0x8E940D, 1  },
    };
    for (const IniByteSetting& setting : kIniBytes) {
        if (ReadIniInt(L"GameSettings", setting.key, &value)) {
            const uint8_t current = ReadMemory<uint8_t>(setting.addr);
            WriteMemory<uint8_t>(setting.addr,
                ClampByteSetting(value, 0, setting.maxValue, current));
        }
    }

    // Per-character voice volumes: 0x8E93F1 .. 0x8E9407.
    for (int i = 0; i < 23; ++i) {
        wchar_t key[32];
        swprintf_s(key, L"voice_volume_%d", i);
        if (ReadIniInt(L"GameSettings", key, &value)) {
            const uintptr_t addr = 0x8E93F1 + (uintptr_t)i;
            const uint8_t current = ReadMemory<uint8_t>(addr);
            WriteMemory<uint8_t>(addr, ClampByteSetting(value, 0, 10, current));
        }
    }

    if (ReadIniInt(L"TrainingSettings", L"health_regen", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_HEALTH_REGEN_SETTING);
        WriteMemory<uint8_t>(ADDR_TRAINING_HEALTH_REGEN_SETTING,
            ClampByteSetting(value, 0, 10, current));
    }
    if (ReadIniInt(L"TrainingSettings", L"meter_level", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_METER_LEVEL_SETTING);
        WriteMemory<uint8_t>(ADDR_TRAINING_METER_LEVEL_SETTING,
            ClampByteSetting(value, 0, 9, current));
    }
    if (ReadIniInt(L"TrainingSettings", L"cpu_control", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE);
        WriteMemory<uint8_t>(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE,
            ClampByteSetting(value, 0, 1, current));
    }
    if (ReadIniInt(L"TrainingSettings", L"air_tech", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_AIR_TECH_SETTING);
        WriteMemory<uint8_t>(ADDR_TRAINING_AIR_TECH_SETTING,
            ClampByteSetting(value, 0, 4, current));
    }
    if (ReadIniInt(L"TrainingSettings", L"ground_tech", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_GROUND_TECH_SETTING);
        WriteMemory<uint8_t>(ADDR_TRAINING_GROUND_TECH_SETTING,
            ClampByteSetting(value, 0, 3, current));
    }
    if (ReadIniInt(L"TrainingSettings", L"block_type", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_BLOCK_TYPE_SETTING);
        WriteMemory<uint8_t>(ADDR_TRAINING_BLOCK_TYPE_SETTING,
            ClampByteSetting(value, 0, 2, current));
    }
    if (ReadIniInt(L"TrainingSettings", L"dummy_state", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_DUMMY_STATE_SETTING);
        WriteMemory<uint8_t>(ADDR_TRAINING_DUMMY_STATE_SETTING,
            ClampByteSetting(value, 0, 3, current));
    }
    if (ReadIniInt(L"TrainingSettings", L"damage_display", &value)) {
        const uint8_t current = ReadMemory<uint8_t>(ADDR_TRAINING_DAMAGE_DISPLAY);
        WriteMemory<uint8_t>(ADDR_TRAINING_DAMAGE_DISPLAY,
            ClampByteSetting(value, 0, 1, current));
    }
}

static bool SavePersistentSettings(const char* reason) {
    ResolveSettingsPath();
    CopyCurrentSettingsToPersisted();

    char blockA[kSettingsBlockASize * 2 + 1] = {};
    char blockB[kSettingsBlockBSize * 2 + 1] = {};
    BytesToHex(s_persistedBlockA, sizeof(s_persistedBlockA), blockA, sizeof(blockA));
    BytesToHex(s_persistedBlockB, sizeof(s_persistedBlockB), blockB, sizeof(blockB));

    InputGuardIniSnapshot inputGuard = {};
    InputOverride_GetIniSnapshot(&inputGuard);

    FILE* file = nullptr;
    if (_wfopen_s(&file, s_settingsPathW, L"wb") != 0 || !file) {
        SetStatus("Failed to save local settings.");
        Rollback::NetplayLog_Write(
            "GSET", -1,
            "Failed to save persisted settings: path=%s reason=%s",
            s_settingsPathUtf8[0] ? s_settingsPathUtf8 : "as2_rollback_settings.ini",
            reason ? reason : "?");
        return false;
    }

    fprintf(file,
        "; Alice Senki 2 rollback mod settings\r\n"
        "; Friendly values are safe to edit. Raw blocks preserve vanilla settings we do not name yet.\r\n"
        "\r\n"
        "[ModSettings]\r\n"
        "; The 60fps cadence correction is always on and has no key.\r\n"
        "; input_guard_shell_hotkeys_ime: 1 clears vanilla Win/Alt+Shift suppression hooks\r\n"
        "input_guard_shell_hotkeys_ime=%d\r\n"
        "; local_rematch: 1 shows the continue prompt after a local VS match\r\n"
        "local_rematch=%d\r\n"
        "; background_input: 1 keeps reading pads while the window is unfocused\r\n"
        "background_input=%d\r\n"
        "\r\n"
        "[GameSettings]\r\n"
        "; difficulty: 0=easy, 1=normal, 2=hard\r\n"
        "difficulty=%u\r\n"
        "; rounds_to_win accepts 1, 2, or 3\r\n"
        "rounds_to_win=%d\r\n"
        "; stage_select: 0=off, 1=on\r\n"
        "stage_select=%u\r\n"
        "\r\n"
        "[TrainingSettings]\r\n"
        "health_regen=%u\r\n"
        "meter_level=%u\r\n"
        "cpu_control=%u\r\n"
        "air_tech=%u\r\n"
        "ground_tech=%u\r\n"
        "block_type=%u\r\n"
        "dummy_state=%u\r\n"
        "damage_display=%u\r\n"
        "\r\n"
        "[RawSettings]\r\n"
        "settings_block_a=%s\r\n"
        "settings_block_b=%s\r\n",
        inputGuard.shell_hotkeys_ime ? 1 : 0,
        LocalRematch::IsEnabled() ? 1 : 0,
        InputSystem_IsBackgroundInputEnabled() ? 1 : 0,
        ReadMemory<uint8_t>(kGameOptionDifficulty),
        GameSettingsSync_RoundsToWin(s_persistedRoundOption),
        ReadMemory<uint8_t>(ADDR_STAGESEL_ENABLE),
        ReadMemory<uint8_t>(ADDR_TRAINING_HEALTH_REGEN_SETTING),
        ReadMemory<uint8_t>(ADDR_TRAINING_METER_LEVEL_SETTING),
        ReadMemory<uint8_t>(ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE),
        ReadMemory<uint8_t>(ADDR_TRAINING_AIR_TECH_SETTING),
        ReadMemory<uint8_t>(ADDR_TRAINING_GROUND_TECH_SETTING),
        ReadMemory<uint8_t>(ADDR_TRAINING_BLOCK_TYPE_SETTING),
        ReadMemory<uint8_t>(ADDR_TRAINING_DUMMY_STATE_SETTING),
        ReadMemory<uint8_t>(ADDR_TRAINING_DAMAGE_DISPLAY),
        blockA,
        blockB);
    fclose(file);

    char label[32] = {};
    GameSettingsSync_FormatRoundLabel(s_persistedRoundOption, label, sizeof(label));
    SetStatus("Saved local settings: %s", label);
    Rollback::NetplayLog_Write(
        "GSET", -1,
        "Saved persisted settings: rounds_raw=%u rounds_wins=%d path=%s reason=%s",
        s_persistedRoundOption,
        GameSettingsSync_RoundsToWin(s_persistedRoundOption),
        s_settingsPathUtf8[0] ? s_settingsPathUtf8 : "as2_rollback_settings.ini",
        reason ? reason : "?");
    return true;
}

static void LoadPersistentSettingsIfReady(const char* reason) {
    if (s_persistentLoaded || !IsVanillaConfigLoaded()) {
        return;
    }

    if (!SettingsFileExists()) {
        SavePersistentSettings(reason ? reason : "create settings file");
        s_persistentLoaded = true;
        s_persistentApplied = true;
        return;
    }

    wchar_t rawA[kSettingsBlockASize * 2 + 16] = {};
    wchar_t rawB[kSettingsBlockBSize * 2 + 16] = {};
    uint8_t blockA[kSettingsBlockASize] = {};
    uint8_t blockB[kSettingsBlockBSize] = {};
    if (ReadIniString(L"RawSettings", L"settings_block_a", rawA, sizeof(rawA) / sizeof(rawA[0])) &&
        ParseHexBytes(rawA, blockA, sizeof(blockA))) {
        WriteSettingsBlock(kSettingsBlockAAddr, blockA, sizeof(blockA));
    }
    if (ReadIniString(L"RawSettings", L"settings_block_b", rawB, sizeof(rawB) / sizeof(rawB[0])) &&
        ParseHexBytes(rawB, blockB, sizeof(blockB))) {
        WriteSettingsBlock(kSettingsBlockBAddr, blockB, sizeof(blockB));
    }

    ForceSimpleEffectsOff("raw block restore");

    // Runtime-only was wrong for this one: it is a standing preference about the
    // window, not a per-session choice, so losing it every launch reads as the
    // toggle not working.
    int backgroundInput = 0;
    if (ReadIniInt(L"ModSettings", L"background_input", &backgroundInput)) {
        InputSystem_SetBackgroundInputEnabled(backgroundInput != 0);
    }

    ApplyFriendlyIniOverrides();
    CopyCurrentSettingsToPersisted();
    s_persistentLoaded = true;
    s_persistentApplied = true;
    SavePersistentSettings(reason ? reason : "normalize loaded settings");

    char label[32] = {};
    GameSettingsSync_FormatRoundLabel(s_persistedRoundOption, label, sizeof(label));
    SetStatus("Loaded local settings: %s", label);
    Rollback::NetplayLog_Write(
        "GSET", -1,
        "Loaded persisted settings: rounds_raw=%u rounds_wins=%d path=%s reason=%s",
        s_persistedRoundOption,
        GameSettingsSync_RoundsToWin(s_persistedRoundOption),
        s_settingsPathUtf8[0] ? s_settingsPathUtf8 : "as2_rollback_settings.ini",
        reason ? reason : "?");
}

static void ApplyRoundOptionToMemory(uint8_t option,
                                     const char* event,
                                     const char* reason,
                                     bool updateLastApplied) {
    const uint8_t normalized = GameSettingsSync_NormalizeRoundOption(
        option,
        reason ? reason : "apply round option");
    const uint8_t previousPersistent = GameSettingsSync_NormalizeRoundOption(
        ReadMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT),
        "apply previous persistent read");

    const bool wrotePersistent = WriteMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT, normalized);
    const bool wroteMatch = WriteMemory<uint8_t>(ADDR_MATCH_ROUND_COUNT, normalized);

    if (updateLastApplied) {
        s_lastAppliedRoundOption = normalized;
    }
    s_lastObservedRoundOption = normalized;

    char label[32] = {};
    GameSettingsSync_FormatRoundLabel(normalized, label, sizeof(label));
    SetStatus("%s: %s",
        event ? event : "Applied rounds",
        label);

    LogSettings(event ? event : "Apply rounds",
                reason ? reason : "round option",
                normalized,
                previousPersistent,
                wrotePersistent,
                wroteMatch);
}

static void CacheLocalIfNeeded(const char* reason) {
    if (s_sessionCached) return;

    s_savedRoundOption = GameSettingsSync_NormalizeRoundOption(
        ReadMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT),
        "cache persistent round option");
    s_savedMatchRoundOption = GameSettingsSync_NormalizeRoundOption(
        ReadMatchRoundOption(),
        "cache match round option");
    s_sessionCached = true;

    char label[32] = {};
    GameSettingsSync_FormatRoundLabel(s_savedRoundOption, label, sizeof(label));
    SetStatus("Cached local rounds: %s", label);
    LOG_NETPLAY(LOG_INFO,
        "[GameSettings] Cached local session settings: round=%s raw=%u match_raw=%u reason=%s",
        label,
        s_savedRoundOption,
        s_savedMatchRoundOption,
        reason ? reason : "?");
    Rollback::NetplayLog_Write(
        "GSET", -1,
        "Cached local session settings: round_raw=%u round_wins=%d match_raw=%u reason=%s",
        s_savedRoundOption,
        GameSettingsSync_RoundsToWin(s_savedRoundOption),
        s_savedMatchRoundOption,
        reason ? reason : "?");
}

} // namespace

void GameSettingsSync_Init() {
    if (s_initialized) {
        return;
    }
    ResolveSettingsPath();
    s_initialized = true;
    SetStatus("Game settings sync ready.");
    Rollback::NetplayLog_Write(
        "GSET", -1,
        "Game settings persistence init: path=%s",
        s_settingsPathUtf8[0] ? s_settingsPathUtf8 : "as2_rollback_settings.ini");
}

void GameSettingsSync_Shutdown() {
    if (!s_initialized) {
        return;
    }
    if (s_persistentLoaded && !s_sessionCached && IsVanillaConfigLoaded()) {
        SavePersistentSettings("shutdown");
    }
    s_initialized = false;
}

void GameSettingsSync_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    LoadPersistentSettingsIfReady("frame update");
    if (!s_persistentLoaded || s_sessionCached || !IsVanillaConfigLoaded()) {
        return;
    }

    // The native options screen can still set it, and the game writes its own
    // config file. Holding it at 0 here means no route - ours, the game's, or a
    // stale file - can leave the two peers stepping rand() differently.
    ForceSimpleEffectsOff("frame guard");

    if (!CurrentSettingsMatchPersisted()) {
        SavePersistentSettings("local settings changed");
    }
}

void GameSettingsSync_BeginNetplaySession(const char* reason) {
    LoadPersistentSettingsIfReady(reason ? reason : "begin netplay session");
    CacheLocalIfNeeded(reason ? reason : "begin netplay session");
}

void GameSettingsSync_RestoreLocalSession(const char* reason) {
    if (!s_sessionCached) return;
    ApplyRoundOptionToMemory(
        s_savedRoundOption,
        "Restore local session rounds",
        reason ? reason : "session end",
        true);
    if (s_persistentLoaded && IsVanillaConfigLoaded()) {
        SavePersistentSettings("restore local session");
    }

    char label[32] = {};
    GameSettingsSync_FormatRoundLabel(s_savedRoundOption, label, sizeof(label));
    SetStatus("Restored local rounds: %s", label);

    s_sessionCached = false;
    s_lastAppliedRoundOption = 0xFF;
}

uint8_t GameSettingsSync_ReadRoundOption() {
    return GameSettingsSync_NormalizeRoundOption(
        ReadMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT),
        "read current round option");
}

uint8_t GameSettingsSync_NormalizeRoundOption(uint8_t roundOption, const char* reason) {
    if (roundOption >= kMinRoundOption && roundOption <= kMaxRoundOption) {
        return roundOption;
    }

    LOG_NETPLAY(LOG_WARNING,
        "[GameSettings] Invalid round option %u, using fallback %u (%s)",
        roundOption,
        kFallbackRoundOption,
        reason ? reason : "?");
    Rollback::NetplayLog_Write(
        "GSET", -1,
        "Invalid round option raw=%u fallback=%u fallback_wins=%d reason=%s",
        roundOption,
        kFallbackRoundOption,
        GameSettingsSync_RoundsToWin(kFallbackRoundOption),
        reason ? reason : "?");
    return kFallbackRoundOption;
}

int GameSettingsSync_RoundsToWin(uint8_t roundOption) {
    return (int)GameSettingsSync_NormalizeRoundOption(roundOption, "round label") + 1;
}

void GameSettingsSync_FormatRoundLabel(uint8_t roundOption, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    const int wins = GameSettingsSync_RoundsToWin(roundOption);
    _snprintf_s(out, outSize, _TRUNCATE,
        "First to %d win%s",
        wins,
        wins == 1 ? "" : "s");
}

bool GameSettingsSync_LocalOptionsLocked() {
    return s_sessionCached;
}

uint8_t GameSettingsSync_BuildHostRoundOption(const char* reason) {
    CacheLocalIfNeeded(reason ? reason : "host config build");

    const uint8_t option = GameSettingsSync_ReadRoundOption();
    char label[32] = {};
    GameSettingsSync_FormatRoundLabel(option, label, sizeof(label));
    SetStatus("Host rounds: %s", label);
    LOG_NETPLAY(LOG_INFO,
        "[GameSettings] Host selected rounds for config: %s raw=%u reason=%s",
        label,
        option,
        reason ? reason : "?");
    Rollback::NetplayLog_Write(
        "GSET", -1,
        "Host selected rounds for config: raw=%u wins=%d reason=%s",
        option,
        GameSettingsSync_RoundsToWin(option),
        reason ? reason : "?");
    return option;
}

void GameSettingsSync_ApplyLockedConfig(const LockedMatchConfig* config, const char* reason) {
    if (!config) return;
    GameSettingsSync_ApplyRoundOption(config->round_count,
        reason ? reason : "locked config");
}

void GameSettingsSync_ApplyRoundOption(uint8_t roundOption, const char* reason) {
    CacheLocalIfNeeded(reason ? reason : "apply round option");
    ApplyRoundOptionToMemory(
        roundOption,
        "Apply host rounds",
        reason ? reason : "round option",
        true);
}

void GameSettingsSync_GetSnapshot(GameSettingsSyncSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->session_cached = s_sessionCached;
    out->persistent_loaded = s_persistentLoaded;
    out->local_round_option = s_savedRoundOption;
    out->current_round_option = NormalizeRoundOptionQuiet(ReadMemory<uint8_t>(ADDR_GAMEOPT_ROUND_COUNT));
    out->match_round_option = NormalizeRoundOptionQuiet(ReadMatchRoundOption());
    out->persisted_round_option = s_persistedRoundOption;
    out->last_applied_round_option = s_lastAppliedRoundOption;
    out->current_rounds_to_win = GameSettingsSync_RoundsToWin(out->current_round_option);
    out->persisted_rounds_to_win = GameSettingsSync_RoundsToWin(out->persisted_round_option);
    CopyText(out->settings_path, sizeof(out->settings_path), s_settingsPathUtf8);
    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net
