#include "replay/replay_runtime.h"

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "core/mod_main.h"
#include "input/input_system.h"
#include "net/netplay_palette_runtime.h"
#include "net/player_side_mapping.h"
#include "net/session_manager.h"
#include "patches/memory_utils.h"
#include "patches/tick_hooks.h"
#include "rollback/game_snapshot.h"
#include "rollback/resimulation.h"
#include "ui/log_window.h"

#include "MinHook.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <windows.h>

namespace Replay {

namespace {

namespace fs = std::filesystem;

constexpr uint16_t kReplayInputMask =
    INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT |
    INPUT_A | INPUT_B | INPUT_C | INPUT_D;
constexpr size_t kReplayHeaderSize = 0x48;
constexpr size_t kReplayTapeSize = static_cast<size_t>(INPUT_HISTORY_MAX) * 2;
constexpr size_t kReplayPlayerBlockSize = 22;
constexpr size_t kReplayPaletteTrailerHeaderSize = 16;
constexpr size_t kReplayPaletteTrailerRecordSize = 8 + Net::NETPLAY_PALETTE_BANK_SIZE;
constexpr int32_t kReplayMenuSelectSubstate = 2;
constexpr int32_t kReplayMenuFadeOutSubstate = 3;
constexpr size_t kReplaySelectDisplayBytes = ADDR_REPLAY_HEADER_BASE - ADDR_REPLAY_SELECT_DISPLAY;
constexpr int32_t kCoarseCheckpointInterval = 600;
constexpr int32_t kSeekFramesPerHotkey = 60;
constexpr int32_t kTakeoverCountdownFrames = 60;
constexpr int32_t kReplayBrowserPageSize = 11;
constexpr float kSeekScale = 16.0f;
constexpr float kSpeedSteps[] = {0.5f, 1.0f, 1.25f, 1.5f, 2.0f, 4.0f};

constexpr uintptr_t kAddrRenderFillRect = 0x5D2F50;
constexpr uintptr_t kAddrRenderSetBlendMode = 0x5D2F80;
constexpr uintptr_t kAddrRenderCreateColor = 0x5D3150;
constexpr uintptr_t kAddrRenderDrawSprite = 0x5D3130;
constexpr uintptr_t kAddrDrawFormatString = 0x629A20;
constexpr uintptr_t kAddrReplayMenuBackgroundHandle = 0x815E04;

constexpr int kReplayBrowserPanelLeft = 28;
constexpr int kReplayBrowserPanelTop = 34;
constexpr int kReplayBrowserPanelRight = 612;
constexpr int kReplayBrowserPanelBottom = 446;
constexpr int kReplayBrowserHeaderY = 46;
constexpr int kReplayBrowserPathY = 64;
constexpr int kReplayBrowserSubheaderY = 64;
constexpr int kReplayBrowserListLeft = 42;
constexpr int kReplayBrowserListRight = 308;
constexpr int kReplayBrowserListTop = 88;
constexpr int kReplayBrowserListBottom = 410;
constexpr int kReplayBrowserRowHeight = 26;
constexpr int kReplayBrowserDetailLeft = 322;
constexpr int kReplayBrowserDetailRight = 598;
constexpr int kReplayBrowserDetailTop = 88;
constexpr int kReplayBrowserDetailBottom = 410;
constexpr int kReplayBrowserStatusY = 424;

constexpr uintptr_t kMenuInputWordUp = 0;
constexpr uintptr_t kMenuInputWordDown = 2;
constexpr uintptr_t kMenuInputWordLeft = 4;
constexpr uintptr_t kMenuInputWordRight = 6;
constexpr uintptr_t kMenuInputWordConfirmA = 8;
constexpr uintptr_t kMenuInputWordCancelB = 10;
constexpr uintptr_t kMenuInputWordConfirmC = 12;
constexpr uintptr_t kMenuInputWordCancelD = 14;

using RenderFillRect_t = int (__cdecl *)(int left, int top, int right, int bottom, int color, int drawFlag);
using RenderSetBlendMode_t = int (__cdecl *)(int blendMode, unsigned __int8 alphaValue);
using RenderCreateColor_t = int (__cdecl *)(unsigned __int8 r, unsigned __int8 g, unsigned __int8 b);
using RenderDrawSprite_t = int (__cdecl *)(int x, int y, int spriteHandle, int transFlag);
using DrawFormatString_t = int (__cdecl *)(int x, int y, unsigned int color, char* fmt, ...);
using ReplaySave_t = char (__cdecl *)(int matchBase);
using ReplaySelectDraw_t = int (__cdecl *)();

constexpr int kHotkeyPause = VK_OEM_5;
constexpr int kHotkeyStepForward = VK_OEM_6;
constexpr int kHotkeyStepBackward = VK_OEM_4;
constexpr int kHotkeySpeedSlower = VK_OEM_MINUS;
constexpr int kHotkeySpeedFaster = VK_OEM_PLUS;
constexpr int kHotkeyToggleHud = VK_INSERT;
constexpr int kHotkeyTakeoverP1 = '1';
constexpr int kHotkeyTakeoverP2 = '2';
constexpr int kHotkeyTakeoverExit = '0';

constexpr std::array<uint8_t, 8> kReplayPaletteTrailerMagic = {
    'A', 'S', '2', 'R', 'P', 'A', 'L', '1'
};
constexpr uint32_t kReplayPaletteTrailerVersion = 1;
constexpr uint32_t kReplayPaletteFlagP1 = 1u << 0;
constexpr uint32_t kReplayPaletteFlagP2 = 1u << 1;

struct PendingPreparedInputs {
    bool valid = false;
    int32_t frame = -1;
    TakeoverMode takeover_mode = TakeoverMode::None;
    uint16_t p1 = 0;
    uint16_t p2 = 0;
};

struct ReplayFileMetadata {
    bool valid = false;
    std::array<uint8_t, kReplayHeaderSize> header{};
    uint32_t p1_char = 0;
    uint32_t p2_char = 0;
    int32_t frames = 0;
    std::string modified_time;
};

enum class ReplayBrowserEntryType {
    ParentDirectory,
    Directory,
    ReplayFile,
};

struct ReplayBrowserEntry {
    ReplayBrowserEntryType type = ReplayBrowserEntryType::ReplayFile;
    fs::path full_path;
    std::string relative_path;
    std::string display_name;
    ReplayFileMetadata metadata;
};

struct ReplayPaletteOverrideState {
    bool present = false;
    Net::NetplayPaletteBank bank{};
};

struct ReplayFileDiskState {
    uintmax_t size = 0;
    fs::file_time_type modified = fs::file_time_type{};
};

using ReplayDirectorySnapshot = std::map<std::wstring, ReplayFileDiskState>;

static bool s_initialized = false;
static bool s_replayLaunchPending = false;
static bool s_replayMatchActive = false;
static bool s_replayHudVisible = true;
static bool s_paused = false;
static bool s_savedPauseBlocked = false;
static int32_t s_currentFrame = -1;
static int32_t s_totalFrames = 0;
static int32_t s_seekTargetFrame = -1;
static int32_t s_takeoverStartFrame = -1;
static int32_t s_takeoverCountdownRemaining = 0;
static int32_t s_lastLoggedFrame = -2;
static int s_speedIndex = 1;
static int s_stepBudget = 0;
static float s_savedGlobalTickScale = 1.0f;
static TakeoverMode s_takeoverMode = TakeoverMode::None;
static bool s_takeoverActive = false;
static PendingPreparedInputs s_pendingPreparedInputs{};
static std::unique_ptr<Rollback::GameSnapshot> s_takeoverStartSnapshot;
static std::map<int32_t, std::unique_ptr<Rollback::GameSnapshot>> s_coarseCheckpoints;
static std::map<int32_t, uint16_t> s_overrideP1;
static std::map<int32_t, uint16_t> s_overrideP2;

static std::vector<ReplayBrowserEntry> s_browserEntries;
static int32_t s_browserSelected = 0;
static int32_t s_browserScroll = 0;
static fs::path s_browserCurrentDirectory;
static bool s_browserNeedsScan = true;
static bool s_browserMenuWasActive = false;
static std::string s_browserStatus;

static bool s_pauseKeyWasDown = false;
static bool s_stepForwardKeyWasDown = false;
static bool s_stepBackwardKeyWasDown = false;
static bool s_speedSlowerKeyWasDown = false;
static bool s_speedFasterKeyWasDown = false;
static bool s_toggleHudKeyWasDown = false;
static bool s_takeoverP1KeyWasDown = false;
static bool s_takeoverP2KeyWasDown = false;
static bool s_takeoverExitKeyWasDown = false;

static bool s_menuUpWasDown = false;
static bool s_menuDownWasDown = false;
static bool s_menuPageUpWasDown = false;
static bool s_menuPageDownWasDown = false;
static bool s_menuHomeWasDown = false;
static bool s_menuEndWasDown = false;
static bool s_menuConfirmWasDown = false;
static bool s_menuCancelWasDown = false;
static bool s_menuBackWasDown = false;
static ReplaySave_t s_originalReplaySave = nullptr;
static ReplaySelectDraw_t s_originalReplaySelectDraw = nullptr;
static ReplayPaletteOverrideState s_loadedReplayPalette[2] = {};

static uint32_t ReadU32(const uint8_t* data);
static std::string WideToUtf8(const std::wstring& text);
static std::string WideToGameText(const std::wstring& text);
static bool IsReplayExtension(const fs::path& path);
static bool IsReplayMenuContext();
static bool IsReplayMenuSelectContext();
static void RenderReplayBrowserHud();
static bool ReadReplayMetadata(const fs::path& path, ReplayFileMetadata* outMetadata);
static bool ShouldRenameNetplayReplaySave(const Net::SessionSnapshot& session);
static bool RenameReplaySaveForNetplay(const fs::path& replayPath,
                                       const ReplayFileMetadata& metadata,
                                       fs::path* outRenamedPath);

using GetWindowHandle_t = LPVOID (__cdecl *)();

static bool RawKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static HWND GetGameWindowHandle() {
    auto getWindowHandle = reinterpret_cast<GetWindowHandle_t>(ADDR_SYS_GET_WINDOW_HANDLE);
    return getWindowHandle ? reinterpret_cast<HWND>(getWindowHandle()) : nullptr;
}

static bool IsGameWindowFocused() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }

    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return foregroundPid == GetCurrentProcessId();
}

static bool KeyDown(int vk) {
    return IsGameWindowFocused() && RawKeyDown(vk);
}

static bool ConsumeEdge(int vk, bool* wasDown) {
    const bool down = RawKeyDown(vk);
    const bool pressed = IsGameWindowFocused() && down && !*wasDown;
    *wasDown = down;
    return pressed;
}

static void ResetPreparedInputs() {
    s_pendingPreparedInputs = {};
}

static void ClearInjectedInputsOnly() {
    InputSystem_ClearNetplayInput(0);
    InputSystem_ClearNetplayInput(1);
}

static void ClearReplayDispatcherState() {
    ClearInjectedInputsOnly();
    ResetPreparedInputs();
}

static bool ReplayMenuInputJustPressed(uint16_t button) {
    return InputSystem_JustPressed(0, button) || InputSystem_JustPressed(1, button);
}

static void ResetMatchHotkeyEdges() {
    s_pauseKeyWasDown = RawKeyDown(kHotkeyPause);
    s_stepForwardKeyWasDown = RawKeyDown(kHotkeyStepForward);
    s_stepBackwardKeyWasDown = RawKeyDown(kHotkeyStepBackward);
    s_speedSlowerKeyWasDown = RawKeyDown(kHotkeySpeedSlower);
    s_speedFasterKeyWasDown = RawKeyDown(kHotkeySpeedFaster);
    s_toggleHudKeyWasDown = RawKeyDown(kHotkeyToggleHud);
    s_takeoverP1KeyWasDown = RawKeyDown(kHotkeyTakeoverP1);
    s_takeoverP2KeyWasDown = RawKeyDown(kHotkeyTakeoverP2);
    s_takeoverExitKeyWasDown = RawKeyDown(kHotkeyTakeoverExit);
}

static void ResetMenuHotkeyEdges() {
    s_menuUpWasDown = false;
    s_menuDownWasDown = false;
    s_menuPageUpWasDown = false;
    s_menuPageDownWasDown = false;
    s_menuHomeWasDown = RawKeyDown(VK_HOME);
    s_menuEndWasDown = RawKeyDown(VK_END);
    s_menuConfirmWasDown = false;
    s_menuCancelWasDown = RawKeyDown(VK_ESCAPE);
    s_menuBackWasDown = RawKeyDown(VK_BACK);
    InputSystem_ResetRepeatState(0);
    InputSystem_ResetRepeatState(1);
}

static void ResetLoadedReplayPaletteState() {
    memset(s_loadedReplayPalette, 0, sizeof(s_loadedReplayPalette));
}

static void AppendU32(std::vector<uint8_t>* bytes, uint32_t value) {
    if (!bytes) {
        return;
    }

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&value);
    bytes->insert(bytes->end(), raw, raw + sizeof(value));
}

static ReplayDirectorySnapshot CaptureReplayDirectorySnapshot() {
    ReplayDirectorySnapshot snapshot;

    std::error_code ec;
    const fs::path root = fs::path(L"replay");
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return snapshot;
    }

    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const fs::directory_entry entry = *it;
        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        if (!IsReplayExtension(entry.path())) {
            continue;
        }

        ReplayFileDiskState state{};
        state.size = entry.file_size(ec);
        if (ec) {
            ec.clear();
            state.size = 0;
        }

        state.modified = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            state.modified = fs::file_time_type{};
        }

        snapshot[entry.path().filename().wstring()] = state;
    }

    return snapshot;
}

static bool ResolveReplaySavePath(const ReplayDirectorySnapshot& before,
                                  const ReplayDirectorySnapshot& after,
                                  fs::path* outPath) {
    if (!outPath) {
        return false;
    }

    bool foundChanged = false;
    std::wstring bestName;
    ReplayFileDiskState bestState{};

    for (const auto& [name, state] : after) {
        const auto beforeIt = before.find(name);
        const bool changed = beforeIt == before.end() ||
            beforeIt->second.size != state.size ||
            beforeIt->second.modified != state.modified;

        if (!changed) {
            continue;
        }

        if (!foundChanged || state.modified > bestState.modified) {
            bestName = name;
            bestState = state;
            foundChanged = true;
        }
    }

    if (!foundChanged) {
        return false;
    }

    *outPath = fs::path(L"replay") / bestName;
    return true;
}

static uint32_t BuildReplayPaletteFlags(const ReplayPaletteOverrideState (&banks)[2]) {
    uint32_t flags = 0;
    if (banks[0].present) {
        flags |= kReplayPaletteFlagP1;
    }
    if (banks[1].present) {
        flags |= kReplayPaletteFlagP2;
    }
    return flags;
}

static bool BuildReplayPaletteSaveData(ReplayPaletteOverrideState (&banks)[2]) {
    memset(banks, 0, sizeof(banks));

    bool anyPresent = false;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        Net::NetplayPaletteBank bank{};
        if (!Net::NetplayPaletteRuntime_CopySpectatorBank(slot, &bank) || !bank.valid) {
            continue;
        }

        banks[slot].present = true;
        banks[slot].bank = bank;
        anyPresent = true;
    }

    return anyPresent;
}

static bool AppendReplayPaletteTrailer(const fs::path& path,
                                       const ReplayPaletteOverrideState (&banks)[2]) {
    const uint32_t flags = BuildReplayPaletteFlags(banks);
    if (flags == 0) {
        return true;
    }

    std::vector<uint8_t> trailer;
    trailer.reserve(kReplayPaletteTrailerHeaderSize + 2 * kReplayPaletteTrailerRecordSize);
    trailer.insert(trailer.end(), kReplayPaletteTrailerMagic.begin(), kReplayPaletteTrailerMagic.end());
    AppendU32(&trailer, kReplayPaletteTrailerVersion);
    AppendU32(&trailer, flags);

    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (!banks[slot].present) {
            continue;
        }

        const Net::NetplayPaletteBank& bank = banks[slot].bank;
        const uint32_t crc = CalcCRC32(bank.data, Net::NETPLAY_PALETTE_BANK_SIZE);
        trailer.push_back(bank.character_id);
        trailer.push_back(bank.base_palette);
        trailer.push_back(0);
        trailer.push_back(0);
        AppendU32(&trailer, crc);
        trailer.insert(trailer.end(), bank.data, bank.data + Net::NETPLAY_PALETTE_BANK_SIZE);
    }

    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) {
        LOG_ERROR("[Replay] Failed to append palette trailer: %s", WideToUtf8(path.wstring()).c_str());
        return false;
    }

    stream.write(reinterpret_cast<const char*>(trailer.data()), static_cast<std::streamsize>(trailer.size()));
    if (!stream) {
        LOG_ERROR("[Replay] Palette trailer write failed: %s", WideToUtf8(path.wstring()).c_str());
        return false;
    }

    LOG_INFO("[Replay] Appended palette trailer to %s (p1=%d p2=%d)",
        WideToUtf8(path.wstring()).c_str(),
        banks[0].present ? 1 : 0,
        banks[1].present ? 1 : 0);
    return true;
}

static uint8_t ReplayHeaderPaletteForSlot(const ReplayFileMetadata& metadata, uint8_t slot) {
    return slot == 0 ? metadata.header[0x0C] : metadata.header[0x22];
}

static uint32_t ReplayHeaderCharacterForSlot(const ReplayFileMetadata& metadata, uint8_t slot) {
    return slot == 0 ? metadata.p1_char : metadata.p2_char;
}

static void ParseReplayPaletteTrailer(std::ifstream& stream,
                                     const ReplayFileMetadata& metadata,
                                     const char* replayPathForLog) {
    ResetLoadedReplayPaletteState();

    const std::streampos trailerStart = stream.tellg();
    if (trailerStart < 0) {
        return;
    }

    stream.seekg(0, std::ios::end);
    const std::streampos fileEnd = stream.tellg();
    if (fileEnd < trailerStart) {
        stream.clear();
        stream.seekg(trailerStart);
        return;
    }

    const size_t trailerBytes = static_cast<size_t>(fileEnd - trailerStart);
    stream.clear();
    stream.seekg(trailerStart);
    if (trailerBytes == 0) {
        return;
    }

    std::vector<uint8_t> trailer(trailerBytes, 0);
    stream.read(reinterpret_cast<char*>(trailer.data()), static_cast<std::streamsize>(trailer.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(trailer.size())) {
        LOG_WARN("[Replay] Failed to read trailer bytes for %s", replayPathForLog ? replayPathForLog : "(unknown)");
        stream.clear();
        return;
    }

    if (trailer.size() < kReplayPaletteTrailerHeaderSize ||
        memcmp(trailer.data(), kReplayPaletteTrailerMagic.data(), kReplayPaletteTrailerMagic.size()) != 0) {
        return;
    }

    const uint32_t version = ReadU32(trailer.data() + kReplayPaletteTrailerMagic.size());
    const uint32_t flags = ReadU32(trailer.data() + kReplayPaletteTrailerMagic.size() + sizeof(uint32_t));
    const uint32_t supportedFlags = kReplayPaletteFlagP1 | kReplayPaletteFlagP2;
    if (version != kReplayPaletteTrailerVersion || (flags & ~supportedFlags) != 0) {
        LOG_WARN("[Replay] Ignoring unsupported palette trailer in %s (version=%u flags=0x%08X)",
            replayPathForLog ? replayPathForLog : "(unknown)",
            version,
            flags);
        return;
    }

    size_t expectedSize = kReplayPaletteTrailerHeaderSize;
    if ((flags & kReplayPaletteFlagP1) != 0) {
        expectedSize += kReplayPaletteTrailerRecordSize;
    }
    if ((flags & kReplayPaletteFlagP2) != 0) {
        expectedSize += kReplayPaletteTrailerRecordSize;
    }
    if (trailer.size() != expectedSize) {
        LOG_WARN("[Replay] Ignoring malformed palette trailer in %s (bytes=%zu expected=%zu)",
            replayPathForLog ? replayPathForLog : "(unknown)",
            trailer.size(),
            expectedSize);
        return;
    }

    size_t offset = kReplayPaletteTrailerHeaderSize;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        const uint32_t flag = slot == 0 ? kReplayPaletteFlagP1 : kReplayPaletteFlagP2;
        if ((flags & flag) == 0) {
            continue;
        }

        const uint8_t characterId = trailer[offset + 0];
        const uint8_t basePalette = trailer[offset + 1];
        const uint32_t storedCrc = ReadU32(trailer.data() + offset + 4);
        const uint8_t* bankData = trailer.data() + offset + 8;
        const uint32_t computedCrc = CalcCRC32(bankData, Net::NETPLAY_PALETTE_BANK_SIZE);
        const uint32_t expectedCharacterId = ReplayHeaderCharacterForSlot(metadata, slot);
        const uint8_t expectedBasePalette = ReplayHeaderPaletteForSlot(metadata, slot);

        if (characterId != expectedCharacterId || basePalette != expectedBasePalette) {
            LOG_WARN("[Replay] Ignoring mismatched palette trailer bank in %s for P%d (char=%u/%u base=%u/%u)",
                replayPathForLog ? replayPathForLog : "(unknown)",
                slot + 1,
                characterId,
                expectedCharacterId,
                basePalette,
                expectedBasePalette);
            offset += kReplayPaletteTrailerRecordSize;
            continue;
        }

        if (computedCrc != storedCrc) {
            LOG_WARN("[Replay] Ignoring corrupt palette trailer bank in %s for P%d (stored=0x%08X actual=0x%08X)",
                replayPathForLog ? replayPathForLog : "(unknown)",
                slot + 1,
                storedCrc,
                computedCrc);
            offset += kReplayPaletteTrailerRecordSize;
            continue;
        }

        ReplayPaletteOverrideState& loaded = s_loadedReplayPalette[slot];
        loaded.present = true;
        loaded.bank.valid = true;
        loaded.bank.character_id = characterId;
        loaded.bank.base_palette = basePalette;
        loaded.bank.crc32 = storedCrc;
        memcpy(loaded.bank.data, bankData, Net::NETPLAY_PALETTE_BANK_SIZE);
        offset += kReplayPaletteTrailerRecordSize;
    }

    LOG_INFO("[Replay] Loaded palette trailer from %s (p1=%d p2=%d)",
        replayPathForLog ? replayPathForLog : "(unknown)",
        s_loadedReplayPalette[0].present ? 1 : 0,
        s_loadedReplayPalette[1].present ? 1 : 0);
}

static char __cdecl Hook_ReplaySave(int matchBase) {
    Net::SessionSnapshot session{};
    Net::Session_GetSnapshot(&session);

    ReplayPaletteOverrideState banks[2] = {};
    ReplayDirectorySnapshot before;
    const bool hasPaletteTrailer = BuildReplayPaletteSaveData(banks);
    const bool shouldRenameNetplayReplay = ShouldRenameNetplayReplaySave(session);
    if (hasPaletteTrailer || shouldRenameNetplayReplay) {
        before = CaptureReplayDirectorySnapshot();
    }

    const char result = s_originalReplaySave ? s_originalReplaySave(matchBase) : 0;

    if (!hasPaletteTrailer && !shouldRenameNetplayReplay) {
        return result;
    }

    const ReplayDirectorySnapshot after = CaptureReplayDirectorySnapshot();
    fs::path replayPath;
    if (!ResolveReplaySavePath(before, after, &replayPath)) {
        LOG_WARN("[Replay] Saved replay could not be resolved for post-save processing");
        return result;
    }

    if (hasPaletteTrailer) {
        AppendReplayPaletteTrailer(replayPath, banks);
    }

    if (shouldRenameNetplayReplay) {
        ReplayFileMetadata metadata{};
        if (ReadReplayMetadata(replayPath, &metadata) && metadata.valid) {
            RenameReplaySaveForNetplay(replayPath, metadata, nullptr);
        } else {
            LOG_WARN("[Replay] Skipped netplay replay rename because saved replay metadata could not be read");
        }
    }

    return result;
}

static int __cdecl Hook_ReplaySelectDraw() {
    if (!s_initialized || !IsReplayMenuContext()) {
        return s_originalReplaySelectDraw ? s_originalReplaySelectDraw() : 0;
    }

    RenderReplayBrowserHud();
    return 0;
}

static void ResetMatchRuntimeState() {
    s_replayLaunchPending = false;
    s_replayMatchActive = false;
    s_replayHudVisible = true;
    s_paused = false;
    s_savedPauseBlocked = false;
    s_currentFrame = -1;
    s_totalFrames = 0;
    s_seekTargetFrame = -1;
    s_takeoverStartFrame = -1;
    s_takeoverCountdownRemaining = 0;
    s_lastLoggedFrame = -2;
    s_speedIndex = 1;
    s_stepBudget = 0;
    s_savedGlobalTickScale = 1.0f;
    s_takeoverMode = TakeoverMode::None;
    s_takeoverActive = false;
    s_takeoverStartSnapshot.reset();
    s_coarseCheckpoints.clear();
    s_overrideP1.clear();
    s_overrideP2.clear();
    ClearReplayDispatcherState();
}

static void ResetBrowserState() {
    s_browserEntries.clear();
    s_browserSelected = 0;
    s_browserScroll = 0;
    s_browserCurrentDirectory.clear();
    s_browserNeedsScan = true;
    s_browserMenuWasActive = false;
    s_browserStatus.clear();
}

static bool IsReplayMenuContext() {
    return GetGameMode() == MODE_REPLAY_SELECT;
}

static bool IsReplayMenuSelectContext() {
    return IsReplayMenuContext() && GetSubstate() == kReplayMenuSelectSubstate;
}

static bool IsReplayMatchContext() {
    if (GetGameMode() != MODE_MATCH) {
        return false;
    }

    const bool replayOwned = GetGameType() == GAMETYPE_REPLAY || s_replayLaunchPending || s_replayMatchActive;
    if (!replayOwned) {
        return false;
    }

    const uint32_t substate = GetSubstate();
    return substate == MATCH_SUB_SETUP ||
           substate == MATCH_SUB_INIT ||
           substate == MATCH_SUB_GAMEPLAY ||
           substate == MATCH_SUB_PAUSE;
}

static const char* TakeoverModeName(TakeoverMode mode) {
    switch (mode) {
        case TakeoverMode::P1: return "P1";
        case TakeoverMode::P2: return "P2";
        case TakeoverMode::Both: return "BOTH";
        default: return "NONE";
    }
}

static void SetBrowserStatus(const char* format, ...) {
    char buffer[256] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_browserStatus = buffer;
}

static uint32_t ReadU32(const uint8_t* data) {
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

static uint16_t ReadU16(const uint8_t* data) {
    uint16_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

static std::string WideToCodePage(const std::wstring& text, UINT codePage) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(codePage, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(codePage, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

static std::string WideToUtf8(const std::wstring& text) {
    return WideToCodePage(text, CP_UTF8);
}

static std::string WideToGameText(const std::wstring& text) {
    return WideToCodePage(text, 932);
}

static std::string NormalizeDisplayPath(const fs::path& path) {
    std::string result = WideToUtf8(path.wstring());
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

static std::string NormalizeGameDisplayPath(const fs::path& path) {
    std::string result = WideToGameText(path.wstring());
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

static std::string MakeSortKey(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    return text;
}

static std::string FormatLastWriteTime(const fs::path& path) {
    std::error_code ec;
    const fs::file_time_type fileTime = fs::last_write_time(path, ec);
    if (ec) {
        return {};
    }

    const auto systemNow = std::chrono::system_clock::now();
    const auto fileNow = fs::file_time_type::clock::now();
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - fileNow + systemNow);

    const std::time_t rawTime = std::chrono::system_clock::to_time_t(systemTime);
    std::tm localTime = {};
    if (localtime_s(&localTime, &rawTime) != 0) {
        return {};
    }

    char buffer[32] = {};
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return buffer;
}

static std::string FormatReplayTimestampForFilename(const fs::path& path) {
    std::error_code ec;
    fs::file_time_type fileTime = fs::last_write_time(path, ec);
    if (ec) {
        fileTime = fs::file_time_type::clock::now();
    }

    const auto systemNow = std::chrono::system_clock::now();
    const auto fileNow = fs::file_time_type::clock::now();
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - fileNow + systemNow);

    const std::time_t rawTime = std::chrono::system_clock::to_time_t(systemTime);
    std::tm localTime = {};
    if (localtime_s(&localTime, &rawTime) != 0) {
        return "replay";
    }

    char buffer[32] = {};
    strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &localTime);
    return buffer;
}

static std::string GetCharacterDisplayName(uint32_t characterId) {
    switch (characterId) {
        case 0: return "Rance";
        case 1: return "Hatsune";
        case 2: return "Patton";
        case 3: return "Seed";
        case 4: return "Raysen";
        case 5: return "Aria";
        case 6: return "Maria";
        case 7: return "Shizuka";
        case 8: return "Fanel";
        case 9: return "Miki";
        case 10: return "Menad";
        case 11: return "Hanny King";
        case 12: return "Satsu";
        case 13: return "Tiger Joe";
        case 14: return "Escalayer";
        case 15: return "Makutsudo";
        case 16: return "Alietta";
        case 17: return "Nalzgis";
        default: {
            char buffer[32] = {};
            snprintf(buffer, sizeof(buffer), "Char%u", characterId);
            return buffer;
        }
    }
}

static std::string SanitizeReplayFilenameComponent(const std::string& text,
                                                   const char* fallback) {
    std::string sanitized;
    sanitized.reserve(text.size());

    bool previousUnderscore = false;
    for (unsigned char ch : text) {
        char outChar = static_cast<char>(ch);
        if (ch <= 31 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*' ||
            ch == ' ' || ch == '.') {
            outChar = '_';
        }

        if (outChar == '_') {
            if (previousUnderscore) {
                continue;
            }
            previousUnderscore = true;
        } else {
            previousUnderscore = false;
        }

        sanitized.push_back(outChar);
    }

    while (!sanitized.empty() && sanitized.front() == '_') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }

    if (!sanitized.empty()) {
        return sanitized;
    }

    return fallback ? fallback : "Unknown";
}

static bool ShouldRenameNetplayReplaySave(const Net::SessionSnapshot& session) {
    return session.remote_peer.nickname[0] != '\0' &&
        (session.active || Net::PlayerMapping_IsAssigned());
}

static std::string BuildNetplayReplayFilename(const fs::path& replayPath,
                                              const ReplayFileMetadata& metadata) {
    Net::SessionSnapshot session{};
    Net::Session_GetSnapshot(&session);

    const std::string localNickname = SanitizeReplayFilenameComponent(
        session.local_nickname[0] ? session.local_nickname : "Local",
        "Local");
    const std::string remoteNickname = SanitizeReplayFilenameComponent(
        session.remote_peer.nickname[0] ? session.remote_peer.nickname : "Remote",
        "Remote");

    int localSlot = Net::PlayerMapping_GetLocalGameSlot();
    if (localSlot != 0 && localSlot != 1) {
        localSlot = Net::Session_GetRole() == Net::SessionRole::Host ? 0 : 1;
    }

    const std::string p1Nickname = localSlot == 0 ? localNickname : remoteNickname;
    const std::string p2Nickname = localSlot == 0 ? remoteNickname : localNickname;
    const std::string p1Character = SanitizeReplayFilenameComponent(GetCharacterDisplayName(metadata.p1_char), "P1Char");
    const std::string p2Character = SanitizeReplayFilenameComponent(GetCharacterDisplayName(metadata.p2_char), "P2Char");

    return FormatReplayTimestampForFilename(replayPath) + "_" +
        p1Nickname + "_" + p1Character + "_vs_" +
        p2Nickname + "_" + p2Character + ".rep";
}

static bool RenameReplaySaveForNetplay(const fs::path& replayPath,
                                       const ReplayFileMetadata& metadata,
                                       fs::path* outRenamedPath) {
    const std::string fileName = BuildNetplayReplayFilename(replayPath, metadata);
    if (fileName.empty()) {
        return false;
    }

    fs::path candidate = replayPath.parent_path() / fs::path(fileName);
    const std::string stem = candidate.stem().string();
    const std::string extension = candidate.extension().string();

    int suffix = 2;
    std::error_code ec;
    while (candidate != replayPath && fs::exists(candidate, ec)) {
        ec.clear();

        char suffixBuffer[16] = {};
        snprintf(suffixBuffer, sizeof(suffixBuffer), "_%d", suffix++);
        candidate = replayPath.parent_path() / fs::path(stem + suffixBuffer + extension);
    }

    if (candidate == replayPath) {
        if (outRenamedPath) {
            *outRenamedPath = replayPath;
        }
        return true;
    }

    fs::rename(replayPath, candidate, ec);
    if (ec) {
        LOG_WARN("[Replay] Failed to rename saved replay %s -> %s (%s)",
            WideToUtf8(replayPath.wstring()).c_str(),
            WideToUtf8(candidate.wstring()).c_str(),
            ec.message().c_str());
        return false;
    }

    LOG_INFO("[Replay] Renamed saved replay: %s -> %s",
        WideToUtf8(replayPath.wstring()).c_str(),
        WideToUtf8(candidate.wstring()).c_str());
    if (outRenamedPath) {
        *outRenamedPath = candidate;
    }
    return true;
}

static bool IsReplayExtension(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return ext == L".rep";
}

static bool ReadReplayMetadata(const fs::path& path, ReplayFileMetadata* outMetadata) {
    if (!outMetadata) {
        return false;
    }

    *outMetadata = {};
    outMetadata->modified_time = FormatLastWriteTime(path);

    std::error_code ec;
    const uintmax_t fileSize = fs::file_size(path, ec);
    if (ec || fileSize < kReplayHeaderSize) {
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    stream.read(reinterpret_cast<char*>(outMetadata->header.data()), static_cast<std::streamsize>(outMetadata->header.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(outMetadata->header.size())) {
        return false;
    }

    const uint32_t magic = ReadU32(outMetadata->header.data() + 0x00);
    const uint32_t storedFrames = ReadU32(outMetadata->header.data() + 0x44);
    const uintmax_t availableFrames = (fileSize - kReplayHeaderSize) / 2;

    if (magic != 258 || storedFrames > INPUT_HISTORY_MAX || availableFrames < storedFrames) {
        return false;
    }

    outMetadata->valid = true;
    outMetadata->p1_char = ReadU32(outMetadata->header.data() + 0x04);
    outMetadata->p2_char = ReadU32(outMetadata->header.data() + 0x08);
    outMetadata->frames = static_cast<int32_t>(storedFrames);
    return true;
}

static void ClampBrowserSelection() {
    if (s_browserEntries.empty()) {
        s_browserSelected = 0;
        s_browserScroll = 0;
        WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_COUNT, 0);
        WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_INDEX, 0);
        return;
    }

    s_browserSelected = std::clamp(s_browserSelected, 0, static_cast<int32_t>(s_browserEntries.size() - 1));
    const int32_t maxScroll = (std::max)(0, static_cast<int32_t>(s_browserEntries.size()) - kReplayBrowserPageSize);
    if (s_browserSelected < s_browserScroll) {
        s_browserScroll = s_browserSelected;
    } else if (s_browserSelected >= s_browserScroll + kReplayBrowserPageSize) {
        s_browserScroll = s_browserSelected - kReplayBrowserPageSize + 1;
    }
    s_browserScroll = std::clamp(s_browserScroll, 0, maxScroll);
    WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_COUNT, static_cast<uint32_t>(s_browserEntries.size()));
    WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_INDEX, static_cast<uint32_t>(s_browserSelected));
}

static void ScanReplayBrowser() {
    const std::string previousPath =
        (s_browserSelected >= 0 && s_browserSelected < static_cast<int32_t>(s_browserEntries.size()))
            ? s_browserEntries[s_browserSelected].relative_path
            : std::string();
    const ReplayBrowserEntryType previousType =
        (s_browserSelected >= 0 && s_browserSelected < static_cast<int32_t>(s_browserEntries.size()))
            ? s_browserEntries[s_browserSelected].type
            : ReplayBrowserEntryType::ReplayFile;

    s_browserEntries.clear();
    s_browserSelected = 0;
    s_browserScroll = 0;
    s_browserStatus.clear();

    const fs::path root = fs::path(L"replay");
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        s_browserCurrentDirectory.clear();
        SetBrowserStatus("No replay directory found at replay/.");
        s_browserNeedsScan = false;
        return;
    }

    fs::path currentDirectory = root / s_browserCurrentDirectory;
    if (!fs::exists(currentDirectory, ec) || !fs::is_directory(currentDirectory, ec)) {
        ec.clear();
        s_browserCurrentDirectory.clear();
        currentDirectory = root;
    }

    if (!s_browserCurrentDirectory.empty()) {
        ReplayBrowserEntry parentEntry{};
        parentEntry.type = ReplayBrowserEntryType::ParentDirectory;
        parentEntry.full_path = currentDirectory.parent_path();
        parentEntry.relative_path = NormalizeDisplayPath(s_browserCurrentDirectory.parent_path());
        parentEntry.display_name = "...";
        s_browserEntries.push_back(std::move(parentEntry));
    }

    std::vector<ReplayBrowserEntry> directoryEntries;
    std::vector<ReplayBrowserEntry> replayEntries;
    bool hasFolders = false;
    bool hasReplays = false;

    for (fs::directory_iterator it(currentDirectory, fs::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const fs::directory_entry entry = *it;

        if (entry.is_directory(ec)) {
            ec.clear();
            ReplayBrowserEntry browserEntry{};
            browserEntry.type = ReplayBrowserEntryType::Directory;
            browserEntry.full_path = entry.path();

            fs::path relativePath = browserEntry.full_path.lexically_relative(root);
            if (relativePath.empty()) {
                relativePath = browserEntry.full_path.filename();
            }

            browserEntry.relative_path = NormalizeDisplayPath(relativePath);
            browserEntry.display_name = WideToGameText(browserEntry.full_path.filename().wstring());
            if (browserEntry.display_name.empty()) {
                browserEntry.display_name = browserEntry.relative_path;
            }
            directoryEntries.push_back(std::move(browserEntry));
            hasFolders = true;
            continue;
        }

        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        if (!IsReplayExtension(entry.path())) {
            continue;
        }

        ReplayBrowserEntry browserEntry{};
        browserEntry.type = ReplayBrowserEntryType::ReplayFile;
        browserEntry.full_path = entry.path();

        fs::path relativePath = browserEntry.full_path.lexically_relative(root);
        if (relativePath.empty()) {
            relativePath = browserEntry.full_path.filename();
        }
        browserEntry.relative_path = NormalizeDisplayPath(relativePath);
        browserEntry.display_name = WideToGameText(browserEntry.full_path.stem().wstring());
        if (browserEntry.display_name.empty()) {
            browserEntry.display_name = NormalizeGameDisplayPath(browserEntry.full_path.filename());
        }
        ReadReplayMetadata(browserEntry.full_path, &browserEntry.metadata);
        replayEntries.push_back(std::move(browserEntry));
        hasReplays = true;
    }

    std::sort(directoryEntries.begin(), directoryEntries.end(), [](const ReplayBrowserEntry& lhs, const ReplayBrowserEntry& rhs) {
        return MakeSortKey(lhs.display_name) < MakeSortKey(rhs.display_name);
    });
    std::sort(replayEntries.begin(), replayEntries.end(), [](const ReplayBrowserEntry& lhs, const ReplayBrowserEntry& rhs) {
        return MakeSortKey(lhs.display_name) < MakeSortKey(rhs.display_name);
    });

    s_browserEntries.insert(s_browserEntries.end(), directoryEntries.begin(), directoryEntries.end());
    s_browserEntries.insert(s_browserEntries.end(), replayEntries.begin(), replayEntries.end());

    if (!previousPath.empty()) {
        for (int32_t i = 0; i < static_cast<int32_t>(s_browserEntries.size()); i++) {
            if (s_browserEntries[i].relative_path == previousPath && s_browserEntries[i].type == previousType) {
                s_browserSelected = i;
                break;
            }
        }
    }

    ClampBrowserSelection();
    s_browserNeedsScan = false;

    if (!hasFolders && !hasReplays) {
        if (s_browserCurrentDirectory.empty()) {
            SetBrowserStatus("No replay folders or files found under replay/.");
        } else {
            SetBrowserStatus("Folder is empty. Press Esc or Backspace to return.");
        }
    } else {
        LOG_INFO("[Replay] Browser scanned %zu entry(s) in %s",
            s_browserEntries.size(),
            s_browserCurrentDirectory.empty() ? "replay/" : NormalizeDisplayPath(s_browserCurrentDirectory).c_str());
    }
}

static void EnsureBrowserScanned() {
    if (s_browserNeedsScan) {
        ScanReplayBrowser();
    }
}

static bool EnterBrowserDirectory(const ReplayBrowserEntry& entry) {
    if (entry.type != ReplayBrowserEntryType::Directory) {
        return false;
    }

    const fs::path root = fs::path(L"replay");
    fs::path relativeDirectory = entry.full_path.lexically_relative(root);
    if (relativeDirectory.empty()) {
        relativeDirectory = entry.full_path.filename();
    }

    s_browserCurrentDirectory = relativeDirectory;
    s_browserNeedsScan = true;
    EnsureBrowserScanned();
    LOG_INFO("[Replay] Browser entered folder: %s", entry.relative_path.c_str());
    return true;
}

static bool ReturnToBrowserParentDirectory() {
    if (s_browserCurrentDirectory.empty()) {
        return false;
    }

    const fs::path childDirectory = s_browserCurrentDirectory;
    s_browserCurrentDirectory = s_browserCurrentDirectory.parent_path();
    s_browserNeedsScan = true;
    EnsureBrowserScanned();

    const std::string childPath = NormalizeDisplayPath(childDirectory);
    for (int32_t i = 0; i < static_cast<int32_t>(s_browserEntries.size()); ++i) {
        if (s_browserEntries[i].type == ReplayBrowserEntryType::Directory &&
            s_browserEntries[i].relative_path == childPath) {
            s_browserSelected = i;
            ClampBrowserSelection();
            break;
        }
    }

    LOG_INFO("[Replay] Browser returned to folder: %s",
        s_browserCurrentDirectory.empty() ? "replay/" : NormalizeDisplayPath(s_browserCurrentDirectory).c_str());
    return true;
}

static int32_t ClampReplayFrame(int32_t frame) {
    if (s_totalFrames <= 0) {
        return -1;
    }

    if (frame < -1) {
        return -1;
    }

    const int32_t maxFrame = s_totalFrames - 1;
    if (frame > maxFrame) {
        return maxFrame;
    }

    return frame;
}

static int32_t ReadObservedFrame() {
    const int32_t writeIdx = static_cast<int32_t>(ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX));
    return ClampReplayFrame(writeIdx - 1);
}

static uint16_t ReadBaseReplayInput(int player, int32_t frame) {
    if (frame < 0 || frame >= s_totalFrames) {
        return 0;
    }

    const uintptr_t addr = ADDR_REPLAY_INPUT_BASE + (frame * 2) + (player == 0 ? 0 : 1);
    return static_cast<uint16_t>(ReadMemory<uint8_t>(addr) & kReplayInputMask);
}

static uint16_t ReadLiveTakeoverInput() {
    return static_cast<uint16_t>(InputSystem_GetInput(0) & kReplayInputMask);
}

static void ApplySpeedScale() {
    float scale = kSpeedSteps[s_speedIndex];
    if (s_seekTargetFrame >= 0) {
        scale = kSeekScale;
    } else if (s_paused) {
        scale = 1.0f;
    }

    SetGlobalTickScale(scale);
}

static void EraseOverridesFrom(int32_t firstFrame) {
    s_overrideP1.erase(s_overrideP1.lower_bound(firstFrame), s_overrideP1.end());
    s_overrideP2.erase(s_overrideP2.lower_bound(firstFrame), s_overrideP2.end());
}

static void InvalidateCheckpointsFrom(int32_t firstFrame) {
    s_coarseCheckpoints.erase(s_coarseCheckpoints.lower_bound(firstFrame), s_coarseCheckpoints.end());
}

static void SetSparseOverride(std::map<int32_t, uint16_t>& overrides,
                              int32_t frame,
                              uint16_t effectiveInput,
                              uint16_t baseInput) {
    if (effectiveInput == baseInput) {
        overrides.erase(frame);
        return;
    }

    overrides[frame] = effectiveInput;
}

static void CommitPreparedOverrides(int32_t frame) {
    if (!s_pendingPreparedInputs.valid || s_pendingPreparedInputs.frame != frame) {
        return;
    }

    if (s_pendingPreparedInputs.takeover_mode == TakeoverMode::P1 ||
        s_pendingPreparedInputs.takeover_mode == TakeoverMode::Both) {
        SetSparseOverride(s_overrideP1, frame, s_pendingPreparedInputs.p1, ReadBaseReplayInput(0, frame));
    }

    if (s_pendingPreparedInputs.takeover_mode == TakeoverMode::P2 ||
        s_pendingPreparedInputs.takeover_mode == TakeoverMode::Both) {
        SetSparseOverride(s_overrideP2, frame, s_pendingPreparedInputs.p2, ReadBaseReplayInput(1, frame));
    }
}

static bool BuildEffectiveInputsForFrame(int32_t frame, PendingPreparedInputs* outPrepared) {
    if (!outPrepared) {
        return false;
    }

    *outPrepared = {};
    if (!s_replayMatchActive || frame < 0 || frame >= s_totalFrames) {
        return false;
    }

    const auto p1Override = s_overrideP1.find(frame);
    const auto p2Override = s_overrideP2.find(frame);
    uint16_t p1 = p1Override != s_overrideP1.end() ? p1Override->second : ReadBaseReplayInput(0, frame);
    uint16_t p2 = p2Override != s_overrideP2.end() ? p2Override->second : ReadBaseReplayInput(1, frame);
    TakeoverMode preparedTakeover = TakeoverMode::None;

    if (s_takeoverActive && s_seekTargetFrame < 0 && frame > s_takeoverStartFrame) {
        const uint16_t liveInput = ReadLiveTakeoverInput();
        if (s_takeoverMode == TakeoverMode::P1 || s_takeoverMode == TakeoverMode::Both) {
            p1 = liveInput;
            preparedTakeover = TakeoverMode::P1;
        }
        if (s_takeoverMode == TakeoverMode::P2 || s_takeoverMode == TakeoverMode::Both) {
            p2 = liveInput;
            preparedTakeover = preparedTakeover == TakeoverMode::P1 ? TakeoverMode::Both : TakeoverMode::P2;
        }
    }

    outPrepared->valid = true;
    outPrepared->frame = frame;
    outPrepared->takeover_mode = preparedTakeover;
    outPrepared->p1 = p1;
    outPrepared->p2 = p2;
    return true;
}

static void WriteReplayHistoryFrame(int32_t frame, uint16_t p1, uint16_t p2) {
    if (frame < 0 || frame >= INPUT_HISTORY_MAX) {
        return;
    }

    WriteMemory<uint16_t>(ADDR_P1_INPUT_HISTORY + (static_cast<uintptr_t>(frame) * sizeof(uint16_t)), p1);
    WriteMemory<uint16_t>(ADDR_P2_INPUT_HISTORY + (static_cast<uintptr_t>(frame) * sizeof(uint16_t)), p2);
}

static bool CaptureSnapshot(std::unique_ptr<Rollback::GameSnapshot>& outSnapshot, int32_t frame) {
    auto snapshot = std::make_unique<Rollback::GameSnapshot>();
    Rollback::GameSnapshot_Clear(snapshot.get());
    if (!Rollback::GameSnapshot_Capture(snapshot.get(), frame)) {
        return false;
    }

    outSnapshot = std::move(snapshot);
    return true;
}

static bool CaptureCoarseCheckpoint(int32_t frame, const char* reason) {
    if (frame < -1) {
        return false;
    }

    std::unique_ptr<Rollback::GameSnapshot> snapshot;
    if (!CaptureSnapshot(snapshot, frame)) {
        LOG_ERROR("[Replay] Failed to capture checkpoint at frame %d (%s)",
            frame,
            reason ? reason : "checkpoint");
        return false;
    }

    s_coarseCheckpoints[frame] = std::move(snapshot);
    LOG_INFO("[Replay] Captured checkpoint at frame %d (%s)", frame, reason ? reason : "checkpoint");
    return true;
}

static void AlignStateHistoryAfterRestore(int32_t frame) {
    Rollback::StateHistory_DiscardFramesAfter(frame);
    if (!Rollback::StateHistory_HasFrame(frame)) {
        Rollback::StateHistory_CaptureFrame(frame);
    }
}

static bool RestoreSnapshot(const Rollback::GameSnapshot& snapshot, int32_t frame, const char* reason) {
    if (!Rollback::GameSnapshot_Restore(&snapshot)) {
        LOG_ERROR("[Replay] Failed to restore snapshot at frame %d (%s)",
            frame,
            reason ? reason : "restore");
        return false;
    }

    AlignStateHistoryAfterRestore(frame);
    s_currentFrame = frame;
    s_seekTargetFrame = -1;
    s_stepBudget = 0;
    ClearReplayDispatcherState();
    LOG_INFO("[Replay] Restored frame %d (%s)", frame, reason ? reason : "restore");
    return true;
}

static void LogFrameAdvance(int32_t observedFrame) {
    if (observedFrame == s_lastLoggedFrame) {
        return;
    }

    s_lastLoggedFrame = observedFrame;
    if (observedFrame < 0 || (observedFrame % 120) != 0) {
        return;
    }

    LOG_INFO("[Replay] Frame=%d/%d paused=%d seeking=%d takeover=%s countdown=%d speed=%.2fx",
        observedFrame,
        s_totalFrames,
        s_paused ? 1 : 0,
        s_seekTargetFrame >= 0 ? 1 : 0,
        TakeoverModeName(s_takeoverMode),
        s_takeoverCountdownRemaining,
        s_seekTargetFrame >= 0 ? kSeekScale : kSpeedSteps[s_speedIndex]);
}

static void CaptureStableFrameState(int32_t observedFrame) {
    if (!Rollback::StateHistory_CaptureFrame(observedFrame)) {
        LOG_WARN("[Replay] Failed to capture recent state for frame %d", observedFrame);
    }

    if (observedFrame >= 0 && (observedFrame % kCoarseCheckpointInterval) == 0) {
        if (s_coarseCheckpoints.find(observedFrame) == s_coarseCheckpoints.end()) {
            CaptureCoarseCheckpoint(observedFrame, "periodic");
        }
    }
}

static void UpdateObservedFrame() {
    const int32_t observedFrame = ReadObservedFrame();
    if (observedFrame == s_currentFrame) {
        return;
    }

    if (observedFrame < s_currentFrame) {
        LOG_WARN("[Replay] Observed frame regressed unexpectedly: %d -> %d",
            s_currentFrame,
            observedFrame);
        s_currentFrame = observedFrame;
        ClearReplayDispatcherState();
        return;
    }

    if (observedFrame > s_currentFrame + 1) {
        LOG_WARN("[Replay] Observed frame skipped: %d -> %d",
            s_currentFrame,
            observedFrame);
    }

    CommitPreparedOverrides(observedFrame);
    ClearReplayDispatcherState();

    if (!s_takeoverActive && s_takeoverMode != TakeoverMode::None && s_takeoverCountdownRemaining > 0) {
        --s_takeoverCountdownRemaining;
        if (s_takeoverCountdownRemaining <= 0) {
            s_takeoverCountdownRemaining = 0;
            s_takeoverActive = true;
            LOG_INFO("[Replay] Takeover ACTIVE for %s at frame %d",
                TakeoverModeName(s_takeoverMode),
                observedFrame + 1);
        }
    }

    s_currentFrame = observedFrame;
    CaptureStableFrameState(observedFrame);
    LogFrameAdvance(observedFrame);

    if (s_stepBudget > 0) {
        s_stepBudget = 0;
    }

    if (s_seekTargetFrame >= 0 && observedFrame >= s_seekTargetFrame) {
        LOG_INFO("[Replay] Seek complete at frame %d", observedFrame);
        s_seekTargetFrame = -1;
        s_paused = true;
    }
}

static int32_t FindBestCheckpointFrame(int32_t targetFrame) {
    auto it = s_coarseCheckpoints.upper_bound(targetFrame);
    if (it == s_coarseCheckpoints.begin()) {
        return -1;
    }

    --it;
    return it->first;
}

static bool RewindToFrame(int32_t targetFrame, const char* reason) {
    targetFrame = ClampReplayFrame(targetFrame);
    if (targetFrame == s_currentFrame) {
        return true;
    }

    s_paused = true;
    s_seekTargetFrame = -1;
    s_stepBudget = 0;
    ClearReplayDispatcherState();

    if (Rollback::StateHistory_HasFrame(targetFrame) && Rollback::StateHistory_LoadFrame(targetFrame)) {
        Rollback::StateHistory_DiscardFramesAfter(targetFrame);
        s_currentFrame = targetFrame;
        LOG_INFO("[Replay] Rewound to recent frame %d (%s)", targetFrame, reason ? reason : "rewind");
        return true;
    }

    const int32_t checkpointFrame = FindBestCheckpointFrame(targetFrame);
    if (checkpointFrame < -1) {
        LOG_ERROR("[Replay] No checkpoint available for target frame %d", targetFrame);
        return false;
    }

    auto it = s_coarseCheckpoints.find(checkpointFrame);
    if (it == s_coarseCheckpoints.end() || !it->second) {
        LOG_ERROR("[Replay] Missing checkpoint storage for frame %d", checkpointFrame);
        return false;
    }

    if (!RestoreSnapshot(*it->second, checkpointFrame, reason)) {
        return false;
    }

    if (checkpointFrame != targetFrame) {
        s_seekTargetFrame = targetFrame;
        LOG_INFO("[Replay] Seeking from checkpoint %d to frame %d (%s)",
            checkpointFrame,
            targetFrame,
            reason ? reason : "seek");
    }

    return true;
}

static bool BeginTakeover(TakeoverMode mode) {
    if (!s_replayMatchActive || mode == TakeoverMode::None) {
        return false;
    }

    if (!CaptureSnapshot(s_takeoverStartSnapshot, s_currentFrame)) {
        LOG_ERROR("[Replay] Failed to capture takeover start snapshot at frame %d", s_currentFrame);
        return false;
    }

    s_takeoverStartFrame = s_currentFrame;
    s_takeoverMode = mode;
    s_takeoverActive = false;
    s_takeoverCountdownRemaining = kTakeoverCountdownFrames;
    s_paused = false;
    s_seekTargetFrame = -1;
    s_stepBudget = 0;
    EraseOverridesFrom(s_currentFrame + 1);
    InvalidateCheckpointsFrom(s_currentFrame + 1);
    ClearReplayDispatcherState();
    LOG_INFO("[Replay] Takeover armed for %s at frame %d (countdown=%d)",
        TakeoverModeName(mode),
        s_currentFrame,
        s_takeoverCountdownRemaining);
    return true;
}

static bool RetryTakeover() {
    if (!s_takeoverStartSnapshot || s_takeoverMode == TakeoverMode::None) {
        return false;
    }

    EraseOverridesFrom(s_takeoverStartFrame + 1);
    InvalidateCheckpointsFrom(s_takeoverStartFrame + 1);
    s_takeoverActive = false;
    s_takeoverCountdownRemaining = kTakeoverCountdownFrames;
    s_paused = false;
    if (!RestoreSnapshot(*s_takeoverStartSnapshot, s_takeoverStartFrame, "takeover retry")) {
        return false;
    }

    LOG_INFO("[Replay] Takeover retry from frame %d (%s)",
        s_takeoverStartFrame,
        TakeoverModeName(s_takeoverMode));
    return true;
}

static bool ExitTakeover() {
    if (!s_takeoverStartSnapshot || s_takeoverMode == TakeoverMode::None) {
        return false;
    }

    const int32_t restoreFrame = s_takeoverStartFrame;
    EraseOverridesFrom(restoreFrame + 1);
    InvalidateCheckpointsFrom(restoreFrame + 1);
    const bool restored = RestoreSnapshot(*s_takeoverStartSnapshot, restoreFrame, "takeover exit");
    s_takeoverMode = TakeoverMode::None;
    s_takeoverActive = false;
    s_takeoverCountdownRemaining = 0;
    s_takeoverStartFrame = -1;
    s_takeoverStartSnapshot.reset();
    s_paused = false;
    if (restored) {
        LOG_INFO("[Replay] Takeover exited to base replay at frame %d", restoreFrame);
    }
    return restored;
}

static void ChangeSpeed(int delta) {
    const int speedCount = static_cast<int>(sizeof(kSpeedSteps) / sizeof(kSpeedSteps[0]));
    const int nextIndex = std::clamp(s_speedIndex + delta, 0, speedCount - 1);
    if (nextIndex == s_speedIndex) {
        return;
    }

    s_speedIndex = nextIndex;
    LOG_INFO("[Replay] Speed set to %.2fx", kSpeedSteps[s_speedIndex]);
}

static void HandleMatchHotkeys() {
    const bool shiftDown = KeyDown(VK_SHIFT);

    if (ConsumeEdge(kHotkeyToggleHud, &s_toggleHudKeyWasDown)) {
        s_replayHudVisible = !s_replayHudVisible;
        LOG_INFO("[Replay] HUD %s", s_replayHudVisible ? "shown" : "hidden");
    }

    if (ConsumeEdge(kHotkeyPause, &s_pauseKeyWasDown)) {
        s_paused = !s_paused;
        if (!s_paused) {
            s_seekTargetFrame = -1;
        }
        LOG_INFO("[Replay] %s at frame %d", s_paused ? "Paused" : "Resumed", s_currentFrame);
    }

    if (ConsumeEdge(kHotkeyStepForward, &s_stepForwardKeyWasDown)) {
        if (!s_paused) {
            s_paused = true;
        }
        s_stepBudget = 1;
        s_seekTargetFrame = -1;
        LOG_INFO("[Replay] Step forward armed from frame %d", s_currentFrame);
    }

    if (ConsumeEdge(kHotkeyStepBackward, &s_stepBackwardKeyWasDown)) {
        if (shiftDown) {
            RewindToFrame(s_currentFrame - kSeekFramesPerHotkey, "hotkey rewind");
        } else {
            RewindToFrame(s_currentFrame - 1, "frame-step backward");
        }
    }

    if (ConsumeEdge(kHotkeySpeedFaster, &s_speedFasterKeyWasDown)) {
        ChangeSpeed(+1);
    }

    if (ConsumeEdge(kHotkeySpeedSlower, &s_speedSlowerKeyWasDown)) {
        ChangeSpeed(-1);
    }

    if (ConsumeEdge(kHotkeyTakeoverP1, &s_takeoverP1KeyWasDown)) {
        if (s_takeoverMode == TakeoverMode::P1) {
            RetryTakeover();
        } else {
            BeginTakeover(TakeoverMode::P1);
        }
    }

    if (ConsumeEdge(kHotkeyTakeoverP2, &s_takeoverP2KeyWasDown)) {
        if (s_takeoverMode == TakeoverMode::P2) {
            RetryTakeover();
        } else {
            BeginTakeover(TakeoverMode::P2);
        }
    }

    if (ConsumeEdge(kHotkeyTakeoverExit, &s_takeoverExitKeyWasDown)) {
        ExitTakeover();
    }
}

static void TransitionReplayMenu(uint32_t resultValue) {
    WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_RESULT, resultValue);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, kReplayMenuFadeOutSubstate);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, 0);
}

static bool LaunchReplayEntry(const ReplayBrowserEntry& entry) {
    if (entry.type != ReplayBrowserEntryType::ReplayFile) {
        return false;
    }

    ReplayFileMetadata metadata;
    ResetLoadedReplayPaletteState();
    if (!ReadReplayMetadata(entry.full_path, &metadata) || !metadata.valid) {
        SetBrowserStatus("Cannot load invalid replay: %s", entry.display_name.c_str());
        LOG_ERROR("[Replay] Invalid replay selected: %s", entry.relative_path.c_str());
        return false;
    }

    std::ifstream stream(entry.full_path, std::ios::binary);
    if (!stream) {
        SetBrowserStatus("Failed to open replay: %s", entry.display_name.c_str());
        LOG_ERROR("[Replay] Failed to open replay: %s", entry.relative_path.c_str());
        return false;
    }

    stream.read(reinterpret_cast<char*>(metadata.header.data()), static_cast<std::streamsize>(metadata.header.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(metadata.header.size())) {
        SetBrowserStatus("Failed to read replay header: %s", entry.display_name.c_str());
        LOG_ERROR("[Replay] Failed to read replay header: %s", entry.relative_path.c_str());
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(metadata.frames) * 2;
    std::vector<uint8_t> tape(frameBytes, 0);
    if (frameBytes > 0) {
        stream.read(reinterpret_cast<char*>(tape.data()), static_cast<std::streamsize>(tape.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(tape.size())) {
            SetBrowserStatus("Replay input data is truncated: %s", entry.display_name.c_str());
            LOG_ERROR("[Replay] Replay input data truncated: %s", entry.relative_path.c_str());
            return false;
        }
    }

    ParseReplayPaletteTrailer(stream, metadata, entry.relative_path.c_str());

    if (!WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_REPLAY_HEADER_BASE),
                              metadata.header.data(),
                              metadata.header.size())) {
        SetBrowserStatus("Failed to write replay header into game memory.");
        ResetLoadedReplayPaletteState();
        return false;
    }

    if (frameBytes > 0 &&
        !WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_REPLAY_INPUT_BASE), tape.data(), tape.size())) {
        SetBrowserStatus("Failed to write replay input data into game memory.");
        ResetLoadedReplayPaletteState();
        return false;
    }

    if (frameBytes < kReplayTapeSize) {
        std::vector<uint8_t> zeros(kReplayTapeSize - frameBytes, 0);
        if (!WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_REPLAY_INPUT_BASE + frameBytes),
                                  zeros.data(),
                                  zeros.size())) {
            SetBrowserStatus("Failed to clear trailing replay input memory.");
            ResetLoadedReplayPaletteState();
            return false;
        }
    }

    WriteMemory<uint32_t>(ADDR_CHARSEL_P1_CHAR_ID, ReadU32(metadata.header.data() + 0x04));
    WriteMemory<uint32_t>(ADDR_CHARSEL_P2_CHAR_ID, ReadU32(metadata.header.data() + 0x08));
    WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_CHARSEL_P1_PALETTE),
                         metadata.header.data() + 0x0C,
                         kReplayPlayerBlockSize);
    WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_CHARSEL_P2_PALETTE),
                         metadata.header.data() + 0x22,
                         kReplayPlayerBlockSize);
    WriteMemory<uint16_t>(ADDR_CHARSEL_P1_VARIANT, ReadU16(metadata.header.data() + 0x38));
    WriteMemory<uint8_t>(ADDR_CHARSEL_P1_VARIANT_EXTRA, metadata.header[0x3A]);
    WriteMemory<uint16_t>(ADDR_CHARSEL_P2_VARIANT, ReadU16(metadata.header.data() + 0x3B));
    WriteMemory<uint8_t>(ADDR_CHARSEL_P2_VARIANT_EXTRA, metadata.header[0x3D]);
    WriteMemory<uint16_t>(ADDR_MATCH_CONFIG_FLAGS, ReadU16(metadata.header.data() + 0x3E));

    static const uint8_t zeroDisplay[kReplaySelectDisplayBytes] = {};
    WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_COUNT, 0);
    WriteMemory<uint32_t>(ADDR_REPLAY_SELECT_INDEX, 0);
    WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_REPLAY_SELECT_DISPLAY), zeroDisplay, sizeof(zeroDisplay));

    s_replayLaunchPending = true;
    TransitionReplayMenu(0);
    ResetMenuHotkeyEdges();
    LOG_INFO("[Replay] Launching replay from browser: %s (%d frames)",
        entry.relative_path.c_str(),
        metadata.frames);
    return true;
}

static void CancelReplaySelection() {
    TransitionReplayMenu(1);
    ResetMenuHotkeyEdges();
    LOG_INFO("[Replay] Replay browser cancelled");
}

static void HandleReplayMenuInput() {
    EnsureBrowserScanned();

    if (ReplayMenuInputJustPressed(INPUT_UP)) {
        if (!s_browserEntries.empty()) {
            s_browserSelected = (s_browserSelected + static_cast<int32_t>(s_browserEntries.size()) - 1) %
                static_cast<int32_t>(s_browserEntries.size());
            ClampBrowserSelection();
        }
    }

    if (ReplayMenuInputJustPressed(INPUT_DOWN)) {
        if (!s_browserEntries.empty()) {
            s_browserSelected = (s_browserSelected + 1) % static_cast<int32_t>(s_browserEntries.size());
            ClampBrowserSelection();
        }
    }

    if (ReplayMenuInputJustPressed(INPUT_LEFT)) {
        s_browserSelected -= kReplayBrowserPageSize;
        ClampBrowserSelection();
    }

    if (ReplayMenuInputJustPressed(INPUT_RIGHT)) {
        s_browserSelected += kReplayBrowserPageSize;
        ClampBrowserSelection();
    }

    if (ConsumeEdge(VK_HOME, &s_menuHomeWasDown)) {
        s_browserSelected = 0;
        ClampBrowserSelection();
    }

    if (ConsumeEdge(VK_END, &s_menuEndWasDown)) {
        s_browserSelected = static_cast<int32_t>(s_browserEntries.size()) - 1;
        ClampBrowserSelection();
    }

    if (ReplayMenuInputJustPressed(INPUT_A) ||
        ReplayMenuInputJustPressed(INPUT_C) ||
        ReplayMenuInputJustPressed(INPUT_START)) {
        if (s_browserEntries.empty()) {
            SetBrowserStatus("No replay folders or files are available here.");
        } else {
            const ReplayBrowserEntry& entry = s_browserEntries[s_browserSelected];
            switch (entry.type) {
                case ReplayBrowserEntryType::ParentDirectory:
                    ReturnToBrowserParentDirectory();
                    break;
                case ReplayBrowserEntryType::Directory:
                    EnterBrowserDirectory(entry);
                    break;
                case ReplayBrowserEntryType::ReplayFile:
                    LaunchReplayEntry(entry);
                    break;
            }
        }
    }

    const bool cancelPressed = ReplayMenuInputJustPressed(INPUT_B) ||
        ReplayMenuInputJustPressed(INPUT_D) ||
        ReplayMenuInputJustPressed(INPUT_SELECT) ||
        ConsumeEdge(VK_ESCAPE, &s_menuCancelWasDown) ||
        ConsumeEdge(VK_BACK, &s_menuBackWasDown);
    if (cancelPressed) {
        if (!ReturnToBrowserParentDirectory()) {
            CancelReplaySelection();
        }
    }
}

static void UpdateReplayMenuContext() {
    const bool menuActive = IsReplayMenuSelectContext();
    if (menuActive && !s_browserMenuWasActive) {
        ResetBrowserState();
        ResetMenuHotkeyEdges();
    } else if (!menuActive && s_browserMenuWasActive) {
        ResetMenuHotkeyEdges();
    }

    s_browserMenuWasActive = menuActive;

    if (menuActive) {
        HandleReplayMenuInput();
    }

}

static void ActivateReplayMatch() {
    ResetMatchRuntimeState();
    s_replayLaunchPending = false;
    s_replayMatchActive = true;
    s_savedGlobalTickScale = GetGlobalTickScale();
    s_savedPauseBlocked = InputSystem_IsPauseBlocked();
    s_totalFrames = static_cast<int32_t>(ReadMemory<uint32_t>(ADDR_REPLAY_ELEMENT_COUNT));
    s_currentFrame = ReadObservedFrame();
    InputSystem_SetPauseBlocked(true);
    ApplySpeedScale();
    ResetMatchHotkeyEdges();
    Rollback::StateHistory_Reset();
    CaptureCoarseCheckpoint(s_currentFrame, "match-start");
    AlignStateHistoryAfterRestore(s_currentFrame);
    LOG_INFO("[Replay] Activated: total_frames=%d start_frame=%d mode=%u sub=%u",
        s_totalFrames,
        s_currentFrame,
        GetGameMode(),
        GetSubstate());
}

static void DeactivateReplayMatch(const char* reason) {
    if (!s_replayMatchActive) {
        return;
    }

    LOG_INFO("[Replay] Deactivated at frame %d (%s)", s_currentFrame, reason ? reason : "inactive");
    ClearReplayDispatcherState();
    InputSystem_SetPauseBlocked(s_savedPauseBlocked);
    SetGlobalTickScale(s_savedGlobalTickScale);
    Rollback::StateHistory_Reset();
    ResetMatchRuntimeState();
    ResetLoadedReplayPaletteState();
    ResetMatchHotkeyEdges();
}

static void RenderReplayMatchHud() {
    if (!s_replayMatchActive || !s_replayHudVisible) {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) {
        return;
    }

    char lines[8][128] = {};
    snprintf(lines[0], sizeof(lines[0]), "REPLAY %d / %d  %.2fx",
        (std::max)(s_currentFrame, 0),
        (std::max)(s_totalFrames - 1, 0),
        s_seekTargetFrame >= 0 ? kSeekScale : kSpeedSteps[s_speedIndex]);

    if (s_seekTargetFrame >= 0) {
        snprintf(lines[1], sizeof(lines[1]), "SEEKING -> %d", s_seekTargetFrame);
    } else if (s_paused) {
        snprintf(lines[1], sizeof(lines[1]), "PAUSED");
    } else {
        snprintf(lines[1], sizeof(lines[1]), "PLAYING");
    }

    if (s_takeoverMode != TakeoverMode::None) {
        if (s_takeoverActive) {
            snprintf(lines[2], sizeof(lines[2]), "TAKEOVER %s ACTIVE", TakeoverModeName(s_takeoverMode));
        } else {
            snprintf(lines[2], sizeof(lines[2]), "TAKEOVER %s IN %d", TakeoverModeName(s_takeoverMode), s_takeoverCountdownRemaining);
        }
    }

    snprintf(lines[3], sizeof(lines[3]), "\\ Pause  ] Fwd  [ Back  Shift+[ Rewind");
    snprintf(lines[4], sizeof(lines[4]), "+/- Speed  1 P1  2 P2  0 Exit  Ins HUD");
    snprintf(lines[5], sizeof(lines[5]), "Press takeover key again to restart.");
    lines[6][0] = '\0';
    lines[7][0] = '\0';

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float maxWidth = 0.0f;
    float lineHeight = 0.0f;
    int lineCount = 0;
    for (const auto& line : lines) {
        if (!line[0]) {
            continue;
        }
        const ImVec2 size = ImGui::CalcTextSize(line);
        maxWidth = (std::max)(maxWidth, size.x);
        lineHeight = (std::max)(lineHeight, size.y);
        ++lineCount;
    }

    if (lineCount == 0) {
        return;
    }

    const float pad = 8.0f * ModUI_GetScale();
    const float x = 12.0f * ModUI_GetScale();
    const float y = 90.0f * ModUI_GetScale();
    const float width = maxWidth + pad * 2.0f;
    const float height = lineCount * lineHeight + pad * 2.0f + (lineCount - 1) * 2.0f;

    drawList->AddRectFilled(
        ImVec2(x, y),
        ImVec2((std::min)(x + width, displaySize.x - 8.0f), y + height),
        IM_COL32(0, 0, 0, 170),
        6.0f);

    int lineIndex = 0;
    for (const auto& line : lines) {
        if (!line[0]) {
            continue;
        }
        const ImU32 color = (lineIndex == 1 && s_paused)
            ? IM_COL32(255, 255, 100, 255)
            : IM_COL32(235, 235, 235, 255);
        drawList->AddText(
            ImVec2(x + pad, y + pad + lineIndex * (lineHeight + 2.0f)),
            color,
            line);
        ++lineIndex;
    }
}

static int GameCreateColor(uint8_t r, uint8_t g, uint8_t b) {
    return reinterpret_cast<RenderCreateColor_t>(kAddrRenderCreateColor)(r, g, b);
}

static void GameDrawSprite(int x, int y, int spriteHandle) {
    if (spriteHandle <= 0) {
        return;
    }

    reinterpret_cast<RenderDrawSprite_t>(kAddrRenderDrawSprite)(x, y, spriteHandle, 1);
}

static void GameSetBlend(int mode, uint8_t alpha) {
    reinterpret_cast<RenderSetBlendMode_t>(kAddrRenderSetBlendMode)(mode, alpha);
}

static void GameFillRect(int left, int top, int right, int bottom, uint8_t r, uint8_t g, uint8_t b) {
    reinterpret_cast<RenderFillRect_t>(kAddrRenderFillRect)(
        left,
        top,
        right,
        bottom,
        GameCreateColor(r, g, b),
        1);
}

static void GameDrawTextShadowed(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* text) {
    if (!text || !text[0]) {
        return;
    }

    const unsigned int shadowColor = static_cast<unsigned int>(GameCreateColor(0, 0, 0));
    const unsigned int textColor = static_cast<unsigned int>(GameCreateColor(r, g, b));
    reinterpret_cast<DrawFormatString_t>(kAddrDrawFormatString)(x + 1, y + 1, shadowColor, (char*)"%s", (char*)text);
    reinterpret_cast<DrawFormatString_t>(kAddrDrawFormatString)(x, y, textColor, (char*)"%s", (char*)text);
}

static bool IsGameTextLeadByte(unsigned char value) {
    return IsDBCSLeadByteEx(932, value) != FALSE;
}

static size_t GameTextPrefixBytes(const char* text, size_t displayUnits) {
    size_t offset = 0;
    size_t units = 0;
    while (text && text[offset]) {
        const bool wideChar = IsGameTextLeadByte((unsigned char)text[offset]) && text[offset + 1];
        const size_t charUnits = wideChar ? 2 : 1;
        if (units + charUnits > displayUnits) {
            break;
        }
        offset += wideChar ? 2 : 1;
        units += charUnits;
    }
    return offset;
}

static size_t GameTextCountChars(const char* text) {
    size_t units = 0;
    for (size_t offset = 0; text && text[offset]; ) {
        if (IsGameTextLeadByte((unsigned char)text[offset]) && text[offset + 1]) {
            offset += 2;
            units += 2;
        } else {
            ++offset;
            units += 1;
        }
    }
    return units;
}

static void ClipGameText(char* out, size_t outCap, const char* in, size_t maxChars) {
    if (!out || outCap == 0) {
        return;
    }

    out[0] = '\0';
    if (!in || !in[0]) {
        return;
    }

    const size_t charCount = GameTextCountChars(in);
    if (charCount <= maxChars || maxChars < 4) {
        strncpy_s(out, outCap, in, _TRUNCATE);
        return;
    }

    const size_t prefixBytes = GameTextPrefixBytes(in, maxChars - 3);
    _snprintf_s(out, outCap, _TRUNCATE, "%.*s...", static_cast<int>(prefixBytes), in);
}

static std::string GetBrowserCurrentPathText() {
    if (s_browserCurrentDirectory.empty()) {
        return "replay/";
    }

    return std::string("replay/") + NormalizeGameDisplayPath(s_browserCurrentDirectory);
}

static std::string GetBrowserEntryValueText(const ReplayBrowserEntry& entry) {
    switch (entry.type) {
        case ReplayBrowserEntryType::ParentDirectory:
            return "Up";
        case ReplayBrowserEntryType::Directory:
            return "Folder";
        case ReplayBrowserEntryType::ReplayFile:
            if (!entry.metadata.valid) {
                return "Invalid";
            }

            break;
    }

    const int32_t seconds = entry.metadata.frames / 60;
    char buffer[32] = {};
    snprintf(buffer, sizeof(buffer), "%d:%02d", seconds / 60, seconds % 60);
    return buffer;
}

static std::string GetBrowserEntryMatchupText(const ReplayBrowserEntry& entry) {
    if (entry.type != ReplayBrowserEntryType::ReplayFile || !entry.metadata.valid) {
        return {};
    }

    return GetCharacterDisplayName(entry.metadata.p1_char) + " / " +
        GetCharacterDisplayName(entry.metadata.p2_char);
}

static void RenderReplayBrowserHud() {
    if (!IsReplayMenuContext()) {
        return;
    }

    EnsureBrowserScanned();

    GameDrawSprite(0, 0, ReadMemory<int>(kAddrReplayMenuBackgroundHandle));

    // Dim overlay
    GameSetBlend(1, 100);
    GameFillRect(0, 0, 639, 479, 0, 0, 0);
    GameSetBlend(0, 255);

    const int totalEntries = static_cast<int>(s_browserEntries.size());
    const int visibleEntries = totalEntries -
        (!s_browserCurrentDirectory.empty() && totalEntries > 0 ? 1 : 0);
    const bool hasSelection = totalEntries > 0 &&
        s_browserSelected >= 0 && s_browserSelected < totalEntries;
    const ReplayBrowserEntry* selectedEntry = hasSelection
        ? &s_browserEntries[s_browserSelected]
        : nullptr;
    const int32_t listEnd = (std::min)(s_browserScroll + kReplayBrowserPageSize,
        static_cast<int32_t>(s_browserEntries.size()));
    const int listRowsTop = kReplayBrowserListTop + 30;

    GameSetBlend(0, 255);

    // Header shadow
    GameSetBlend(1, 40);
    GameFillRect(kReplayBrowserPanelLeft + 8, kReplayBrowserHeaderY - 4, kReplayBrowserPanelRight - 8, kReplayBrowserListTop - 2, 0, 0, 0);
    GameSetBlend(0, 255);

    // List area shadow
    GameSetBlend(1, 30);
    GameFillRect(kReplayBrowserListLeft - 2, kReplayBrowserListTop - 2, kReplayBrowserListRight + 2, kReplayBrowserListBottom + 2, 0, 0, 0);
    GameSetBlend(0, 255);

    // Detail area shadow
    GameSetBlend(1, 30);
    GameFillRect(kReplayBrowserDetailLeft - 2, kReplayBrowserDetailTop - 2, kReplayBrowserDetailRight + 2, kReplayBrowserDetailBottom + 2, 0, 0, 0);
    GameSetBlend(0, 255);

    // Header
    const std::string pathText = GetBrowserCurrentPathText();
    char clippedPath[192] = {};
    ClipGameText(clippedPath, sizeof(clippedPath), pathText.c_str(), 38);

    char headerRight[64] = {};
    if (hasSelection) {
        snprintf(headerRight, sizeof(headerRight), "%d/%d  (%d items)",
            s_browserSelected + 1, totalEntries,
            (std::max)(visibleEntries, 0));
    } else {
        snprintf(headerRight, sizeof(headerRight), "%d items",
            (std::max)(visibleEntries, 0));
    }

    GameDrawTextShadowed(kReplayBrowserPanelLeft + 14, kReplayBrowserHeaderY, 248, 238, 220, "Replays");
    GameDrawTextShadowed(kReplayBrowserPanelLeft + 80, kReplayBrowserHeaderY, 148, 140, 128, clippedPath);
    GameDrawTextShadowed(kReplayBrowserPanelRight - 160, kReplayBrowserPathY, 148, 140, 128, headerRight);

    GameSetBlend(0, 255);

    // List entries
    for (int32_t i = s_browserScroll; i < listEnd; ++i) {
        const ReplayBrowserEntry& entry = s_browserEntries[i];
        const int rowIndex = i - s_browserScroll;
        const int rowTop = listRowsTop + rowIndex * kReplayBrowserRowHeight;
        const bool selected = i == s_browserSelected;

        if (selected) {
            GameSetBlend(1, 30);
            GameFillRect(
                kReplayBrowserListLeft + 4,
                rowTop - 4,
                kReplayBrowserListRight - 4,
                rowTop + kReplayBrowserRowHeight - 1,
                0, 0, 0);
            GameSetBlend(1, 128);
            GameFillRect(
                kReplayBrowserListLeft + 6,
                rowTop - 2,
                kReplayBrowserListRight - 6,
                rowTop + kReplayBrowserRowHeight - 3,
                180, 60, 50);
        } else {
            GameSetBlend(1, 22);
            GameFillRect(
                kReplayBrowserListLeft + 4,
                rowTop - 4,
                kReplayBrowserListRight - 4,
                rowTop + kReplayBrowserRowHeight - 1,
                0, 0, 0);
        }
        GameSetBlend(0, 255);

        const char* typeTag = "RPL";
        uint8_t tagR = 200, tagG = 204, tagB = 216;
        if (entry.type == ReplayBrowserEntryType::ParentDirectory) {
            typeTag = "UP";
            tagR = 160; tagG = 200; tagB = 236;
        } else if (entry.type == ReplayBrowserEntryType::Directory) {
            typeTag = "DIR";
            tagR = 224; tagG = 200; tagB = 130;
        } else if (!entry.metadata.valid) {
            typeTag = "BAD";
            tagR = 240; tagG = 140; tagB = 140;
        }

        char label[160] = {};
        ClipGameText(label, sizeof(label), entry.display_name.c_str(), 18);
        const std::string valueText = GetBrowserEntryValueText(entry);
        char clippedValue[64] = {};
        ClipGameText(clippedValue, sizeof(clippedValue), valueText.c_str(), 6);

        GameDrawTextShadowed(kReplayBrowserListLeft + 14, rowTop + 5, tagR, tagG, tagB, typeTag);
        GameDrawTextShadowed(kReplayBrowserListLeft + 50, rowTop + 5,
            entry.type == ReplayBrowserEntryType::ReplayFile && !entry.metadata.valid
                ? (selected ? 255 : 240) : (selected ? 255 : 220),
            entry.type == ReplayBrowserEntryType::ReplayFile && !entry.metadata.valid
                ? (selected ? 150 : 140) : (selected ? 244 : 216),
            entry.type == ReplayBrowserEntryType::ReplayFile && !entry.metadata.valid
                ? (selected ? 150 : 140) : (selected ? 228 : 212),
            label);
        GameDrawTextShadowed(kReplayBrowserListLeft + 200, rowTop + 5, 140, 148, 164, clippedValue);
    }

    // Scrollbar
    if (totalEntries > kReplayBrowserPageSize) {
        const int trackLeft = kReplayBrowserListRight - 10;
        const int trackTop = listRowsTop;
        const int trackBottom = kReplayBrowserListBottom - 8;
        const int trackHeight = trackBottom - trackTop;
        const int thumbHeight = (std::max)(20, trackHeight * kReplayBrowserPageSize / totalEntries);
        const int maxScroll = (std::max)(1, totalEntries - kReplayBrowserPageSize);
        const int thumbTop = trackTop + (trackHeight - thumbHeight) * s_browserScroll / maxScroll;

        GameSetBlend(1, 48);
        GameFillRect(trackLeft, trackTop, trackLeft + 3, trackBottom, 80, 80, 100);
        GameSetBlend(1, 120);
        GameFillRect(trackLeft, thumbTop, trackLeft + 3, thumbTop + thumbHeight, 180, 160, 140);
        GameSetBlend(0, 255);
    }

    // Detail panel content
    auto drawDetailPair = [&](int y, const char* label, const char* value, uint8_t vr, uint8_t vg, uint8_t vb) {
        if (!value || !value[0]) return;
        char clippedValue[192] = {};
        ClipGameText(clippedValue, sizeof(clippedValue), value, 30);
        GameDrawTextShadowed(kReplayBrowserDetailLeft + 14, y, 148, 140, 128, label);
        GameDrawTextShadowed(kReplayBrowserDetailLeft + 14, y + 14, vr, vg, vb, clippedValue);
    };

    auto drawDetailNote = [&](int y, uint8_t r, uint8_t g, uint8_t b, const char* value) {
        if (!value || !value[0]) return;
        char clippedValue[192] = {};
        ClipGameText(clippedValue, sizeof(clippedValue), value, 30);
        GameDrawTextShadowed(kReplayBrowserDetailLeft + 14, y, r, g, b, clippedValue);
    };

    if (selectedEntry) {
        char clippedName[192] = {};
        ClipGameText(clippedName, sizeof(clippedName), selectedEntry->display_name.c_str(), 30);
        GameDrawTextShadowed(kReplayBrowserDetailLeft + 14, kReplayBrowserDetailTop + 38, 240, 220, 160, clippedName);

        const char* typeLabel = "Replay File";
        uint8_t typeR = 200, typeG = 204, typeB = 216;
        if (selectedEntry->type == ReplayBrowserEntryType::ParentDirectory) {
            typeLabel = "Parent Folder";
            typeR = 160; typeG = 200; typeB = 236;
        } else if (selectedEntry->type == ReplayBrowserEntryType::Directory) {
            typeLabel = "Folder";
            typeR = 224; typeG = 200; typeB = 130;
        } else if (!selectedEntry->metadata.valid) {
            typeLabel = "Invalid Replay";
            typeR = 240; typeG = 140; typeB = 140;
        }
        GameDrawTextShadowed(kReplayBrowserDetailLeft + 14, kReplayBrowserDetailTop + 60, typeR, typeG, typeB, typeLabel);

        std::string locationText = NormalizeGameDisplayPath(selectedEntry->full_path.lexically_relative(fs::path(L"replay")));
        if (locationText.empty()) {
            locationText = selectedEntry->display_name;
        }

        if (selectedEntry->type == ReplayBrowserEntryType::ReplayFile) {
            if (selectedEntry->metadata.valid) {
                const std::string matchupText = GetBrowserEntryMatchupText(*selectedEntry);
                const std::string durationText = GetBrowserEntryValueText(*selectedEntry);

                char frameText[64] = {};
                snprintf(frameText, sizeof(frameText), "%d", selectedEntry->metadata.frames);

                drawDetailPair(kReplayBrowserDetailTop + 86, "Matchup", matchupText.c_str(), 210, 210, 220);
                drawDetailPair(kReplayBrowserDetailTop + 118, "Length", durationText.c_str(), 210, 210, 220);
                drawDetailPair(kReplayBrowserDetailTop + 150, "Frames", frameText, 210, 210, 220);
                drawDetailPair(kReplayBrowserDetailTop + 182, "Updated",
                    selectedEntry->metadata.modified_time.empty() ? "Unknown" : selectedEntry->metadata.modified_time.c_str(),
                    210, 210, 220);
                drawDetailPair(kReplayBrowserDetailTop + 214, "Location", locationText.c_str(), 160, 168, 184);

                drawDetailNote(kReplayBrowserDetailBottom - 38, 180, 190, 210,
                    "A/C or Enter to load.");
                drawDetailNote(kReplayBrowserDetailBottom - 20, 140, 148, 164,
                    "L/R page  Home/End jump");
            } else {
                drawDetailPair(kReplayBrowserDetailTop + 92, "Status",
                    "Invalid replay header.",
                    240, 140, 140);
                drawDetailPair(kReplayBrowserDetailTop + 124, "Location", locationText.c_str(), 160, 168, 184);
                drawDetailNote(kReplayBrowserDetailBottom - 24, 180, 190, 210,
                    "Select another replay.");
            }
        } else if (selectedEntry->type == ReplayBrowserEntryType::Directory) {
            drawDetailPair(kReplayBrowserDetailTop + 92, "Location", locationText.c_str(), 210, 210, 220);
            drawDetailNote(kReplayBrowserDetailTop + 136, 180, 190, 210,
                "A/C or Enter to open.");
        } else {
            drawDetailPair(kReplayBrowserDetailTop + 92, "Back to",
                s_browserCurrentDirectory.empty() ? "replay/" : NormalizeGameDisplayPath(s_browserCurrentDirectory.parent_path()).c_str(),
                210, 210, 220);
            drawDetailNote(kReplayBrowserDetailTop + 136, 180, 190, 210,
                "A/C or B/D to go up.");
        }
    } else {
        drawDetailNote(kReplayBrowserDetailTop + 38, 200, 204, 216,
            "No entries here.");
        drawDetailNote(kReplayBrowserDetailTop + 60, 140, 148, 164,
            "Add replays or go back.");
    }

    // Footer shadow
    GameSetBlend(1, 40);
    GameFillRect(kReplayBrowserPanelLeft + 8, kReplayBrowserStatusY - 8, kReplayBrowserPanelRight - 8, kReplayBrowserPanelBottom + 2, 0, 0, 0);

    // Footer
    GameSetBlend(0, 255);

    const char* footerText = s_browserStatus.empty()
        ? (!s_browserCurrentDirectory.empty()
            ? "U/D Move  L/R Page  A Open  B/Esc Up"
            : "U/D Move  L/R Page  A Open  B/Esc Exit")
        : s_browserStatus.c_str();
    char clippedFooter[192] = {};
    ClipGameText(clippedFooter, sizeof(clippedFooter), footerText, 62);
    GameDrawTextShadowed(
        kReplayBrowserPanelLeft + 16,
        kReplayBrowserStatusY,
        s_browserStatus.empty() ? 148 : 240,
        s_browserStatus.empty() ? 148 : 208,
        s_browserStatus.empty() ? 164 : 128,
        clippedFooter);
}

} // namespace

bool ReplayRuntime_InstallHooks() {
    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_REPLAY_SAVE),
        reinterpret_cast<void*>(&Hook_ReplaySave),
        reinterpret_cast<void**>(&s_originalReplaySave));
    if (status != MH_OK) {
        LOG_ERROR("[Replay] Failed to hook replay save function! Status: %d", status);
        return false;
    }

    LOG_INFO("[Replay] Hooked sub_59B830 (replay save post-process)");

    status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_REPLAY_SELECT_DRAW),
        reinterpret_cast<void*>(&Hook_ReplaySelectDraw),
        reinterpret_cast<void**>(&s_originalReplaySelectDraw));
    if (status != MH_OK) {
        LOG_ERROR("[Replay] Failed to hook replay select draw function! Status: %d", status);
        return false;
    }

    LOG_INFO("[Replay] Hooked sub_59BF90 (replay select draw)");
    return true;
}

void ReplayRuntime_Init() {
    ResetMatchRuntimeState();
    ResetLoadedReplayPaletteState();
    ResetBrowserState();
    ResetMatchHotkeyEdges();
    ResetMenuHotkeyEdges();
    s_initialized = true;
    LOG_INFO("[Replay] Runtime initialized (\\ pause, ] step, [ back, Shift+[ rewind, Insert HUD, 1/2 takeover)");
}

void ReplayRuntime_Shutdown() {
    if (!s_initialized) {
        return;
    }

    DeactivateReplayMatch("shutdown");
    ResetLoadedReplayPaletteState();
    ResetBrowserState();
    s_initialized = false;
    LOG_INFO("[Replay] Runtime shutdown");
}

void ReplayRuntime_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    UpdateReplayMenuContext();

    if (!IsReplayMatchContext()) {
        DeactivateReplayMatch("left replay match");
        return;
    }

    if (!s_replayMatchActive) {
        ActivateReplayMatch();
    }

    UpdateObservedFrame();
    HandleMatchHotkeys();
    ApplySpeedScale();
    ClearInjectedInputsOnly();
}

void ReplayRuntime_RenderHUD() {
    if (!s_initialized) {
        return;
    }

    RenderReplayMatchHud();
}

bool ReplayRuntime_ShouldFreezeFrame() {
    return s_replayMatchActive && s_paused && s_seekTargetFrame < 0 && s_stepBudget == 0;
}

bool ReplayRuntime_IsReplayMatchActive() {
    return s_replayMatchActive;
}

bool ReplayRuntime_IsReplayMenuActive() {
    return IsReplayMenuSelectContext();
}

bool ReplayRuntime_ShouldConsumeMenuInput() {
    return IsReplayMenuSelectContext();
}

void ReplayRuntime_OnFrontendInputsProcessed() {
    if (!s_initialized || !IsReplayMenuSelectContext()) {
        return;
    }
}

bool ReplayRuntime_CopyPaletteOverrideBank(uint8_t gameSlot, Net::NetplayPaletteBank* out) {
    if (!out || gameSlot > 1 || GetGameType() != GAMETYPE_REPLAY) {
        return false;
    }

    const ReplayPaletteOverrideState& loaded = s_loadedReplayPalette[gameSlot];
    if (!loaded.present || !loaded.bank.valid) {
        return false;
    }

    *out = loaded.bank;
    return true;
}

void ReplayRuntime_OnDispatcherAdvance(int16_t* outputInputs) {
    if (!s_replayMatchActive || !outputInputs) {
        return;
    }

    const int32_t dispatchedFrame = ReadObservedFrame();
    PendingPreparedInputs prepared{};
    if (!BuildEffectiveInputsForFrame(dispatchedFrame, &prepared)) {
        ClearReplayDispatcherState();
        return;
    }

    outputInputs[0] = static_cast<int16_t>(prepared.p1);
    outputInputs[1] = static_cast<int16_t>(prepared.p2);

    WriteReplayHistoryFrame(dispatchedFrame, prepared.p1, prepared.p2);
    InputSystem_SetNetplayInput(0, prepared.p1);
    InputSystem_SetNetplayInput(1, prepared.p2);
    s_pendingPreparedInputs = prepared;
}

} // namespace Replay
