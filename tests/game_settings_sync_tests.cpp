#include "net/game_settings_sync.h"
#include "net/locked_match_config.h"
#include "core/as2_constants.h"
#include "core/local_rematch.h"
#include "input_system.h"
#include "patches/input_override.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include "ui/strings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void Check(bool passed, const char* message) {
    if (!passed) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

// Simulate game memory without competing with the process heap for fixed addresses.
static uint8_t gameMemory[0xE0000] = {};

template<typename T> T ReadMemory(uintptr_t address) {
    Check(address >= 0x810000 && address - 0x810000 <= sizeof(gameMemory) - sizeof(T),
          "read stays within simulated game memory");
    T value{};
    std::memcpy(&value, gameMemory + address - 0x810000, sizeof(value));
    return value;
}

template<typename T> bool WriteMemory(uintptr_t address, T value) {
    Check(address >= 0x810000 && address - 0x810000 <= sizeof(gameMemory) - sizeof(T),
          "write stays within simulated game memory");
    std::memcpy(gameMemory + address - 0x810000, &value, sizeof(value));
    return true;
}

template uint8_t ReadMemory<uint8_t>(uintptr_t);
template uint32_t ReadMemory<uint32_t>(uintptr_t);
template bool WriteMemory<uint8_t>(uintptr_t, uint8_t);
template bool WriteMemory<uint32_t>(uintptr_t, uint32_t);

// Only unrelated UI, input preferences and logging are stubbed.
void LogWindow_Log(LogLevel, const char*, ...) {}
void LogWindow_LogCat(LogCategory, LogLevel, const char*, ...) {}
bool LogWindow_IsCategoryEnabled(LogCategory) { return false; }
void InputOverride_GetIniSnapshot(InputGuardIniSnapshot* out) { *out = {}; }
bool InputSystem_IsBackgroundInputEnabled() { return false; }
void InputSystem_SetBackgroundInputEnabled(bool) {}
namespace LocalRematch { bool IsEnabled() { return true; } }
namespace Rollback {
void NetplayLog_Write(const char*, int32_t, const char*, ...) {}
}
namespace Ui {
Lang Strings_Language() { return Lang::English; }
const char* Strings_LanguageCode(Lang) { return "en"; }
bool Strings_ParseLanguageCode(const char*, Lang*) { return false; }
void Strings_SetLanguage(Lang) {}
}

static void CheckActive(uint8_t expected) {
    Check(Net::GameSettingsSync_BossOption() == expected, "persistent option uses session agreement");
    Check(ReadMemory<uint8_t>(ADDR_CHARSEL_SPECIAL_CHARS) == expected, "character-select row uses session agreement");
    Check(ReadMemory<uint8_t>(0x816477) == expected, "native network copy uses session agreement");
}

static void CheckPersisted(uint8_t local) {
    Check(Net::GameSettingsSync_SaveLocalSettings("test persistence"), "save local preferences");
    Net::GameSettingsSyncSnapshot snapshot{};
    Net::GameSettingsSync_GetSnapshot(&snapshot);
    FILE* file = nullptr;
    Check(fopen_s(&file, snapshot.settings_path, "rb") == 0 && file, "open saved settings");
    char data[8192] = {};
    const size_t bytes = std::fread(data, 1, sizeof(data) - 1, file);
    Check(!std::ferror(file) && bytes < sizeof(data) - 1, "read complete settings file");
    std::fclose(file);
    char friendly[32] = {};
    sprintf_s(friendly, "boss_characters=%u\r\n", local);
    Check(std::strstr(data, friendly) != nullptr, "friendly INI retains local preference");
    const char* raw = std::strstr(data, "settings_block_b=");
    Check(raw != nullptr, "raw settings block exists");
    raw += std::strlen("settings_block_b=") + 2 * (ADDR_GAMEOPT_SPECIAL_CHARS - 0x8E93EC);
    Check(raw[0] == '0' && raw[1] == '0' + local, "raw INI retains local preference");
}

int main() {
    // This target has its own output directory, so its INI cannot touch game settings.
    wchar_t settingsPath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, settingsPath, MAX_PATH);
    Check(length > 0 && length < MAX_PATH, "resolve isolated test directory");
    wchar_t* filename = wcsrchr(settingsPath, L'\\');
    Check(filename != nullptr, "test executable has an absolute path");
    wcscpy_s(filename + 1, MAX_PATH - (filename + 1 - settingsPath), L"as2_rollback_settings.ini");
    Check(DeleteFileW(settingsPath) || GetLastError() == ERROR_FILE_NOT_FOUND, "clear previous test settings");

    Check(WriteMemory<uint32_t>(ADDR_CONFIG_VERSION, 258), "mark native settings loaded");
    Net::GameSettingsSync_Init();
    Net::GameSettingsSync_FrameUpdate();

    for (uint8_t local = 0; local <= 1; ++local) {
        for (uint8_t remote = 0; remote <= 1; ++remote) {
            Check(WriteMemory<uint8_t>(ADDR_GAMEOPT_SPECIAL_CHARS, local), "set local preference");
            Net::GameSettingsSync_BeginNetplaySession("test connect");
            CheckActive(0);
            Check(Net::GameSettingsSync_LocalBossOption() == local,
                "identity advertises cached preference after initial disable");
            Net::GameSettingsSync_ApplyPeerBossOption(remote);
            CheckActive(local && remote);
            Net::GameSettingsSync_BeginNetplaySession("test rematch within set");
            CheckActive(local && remote);
            Check(Net::GameSettingsSync_LocalBossOption() == local, "rematch preserves original preference");
            CheckPersisted(local);
            // Native initialization may rewrite these bytes; the frame guard repairs them.
            WriteMemory<uint8_t>(ADDR_CHARSEL_SPECIAL_CHARS, !(local && remote));
            WriteMemory<uint8_t>(0x816477, !(local && remote));
            Net::GameSettingsSync_FrameUpdate();
            CheckActive(local && remote);
            Net::GameSettingsSync_RestoreLocalSession("test disconnect");
            CheckActive(local);
            Check(!Net::GameSettingsSync_LocalOptionsLocked(), "disconnect releases cached preference");

            Net::GameSettingsSync_BeginNetplaySession("test reconnect");
            Check(Net::GameSettingsSync_LocalBossOption() == local, "reconnect advertises restored preference");
            Net::GameSettingsSync_ApplyPeerBossOption(!remote);
            CheckActive(local && !remote);
            Net::GameSettingsSync_RestoreLocalSession("test reconnect end");
            CheckActive(local);
        }
    }

    for (uint8_t local = 0; local <= 1; ++local) {
        WriteMemory<uint8_t>(ADDR_GAMEOPT_SPECIAL_CHARS, local);
        Net::GameSettingsSync_BeginNetplaySession("test spectator begin");
        Net::LockedMatchConfig config{};
        config.boss_characters = !local;
        config.round_count = 2;
        Net::GameSettingsSync_ApplyLockedConfig(&config, "test spectator config");
        CheckActive(!local);
        Check(Net::GameSettingsSync_LocalBossOption() == local, "spectator keeps viewer preference cached");
        CheckPersisted(local);
        config.boss_characters = local;
        Net::GameSettingsSync_ApplyLockedConfig(&config, "test next spectator match");
        CheckActive(local);
        Net::GameSettingsSync_RestoreLocalSession("test spectator end");
        CheckActive(local);
        Check(!Net::GameSettingsSync_LocalOptionsLocked(), "spectator releases settings");
    }

    Net::GameSettingsSync_Shutdown();
    Check(DeleteFileW(settingsPath) != 0, "remove test settings");
    std::puts("Game settings lifecycle tests passed.");
    return 0;
}
