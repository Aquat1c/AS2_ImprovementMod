#include "replay/replay_runtime.h"
#include "training/practice_tools.h"
#include "net/netplay_menu_render.h"

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "core/mod_main.h"
#include "input/input_system.h"
#include "net/netplay_palette_runtime.h"
#include "net/mode_ownership.h"
#include "net/player_side_mapping.h"
#include "net/set_tracker.h"
#include "net/session_manager.h"
#include "patches/memory_utils.h"
#include "patches/tick_hooks.h"
#include "rollback/game_snapshot.h"
#include "rollback/netplay_log.h"
#include "rollback/resimulation.h"
#include "ui/log_window.h"
#include "ui/mod_menu.h"

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
constexpr size_t kReplayHeaderP1PaletteOffset = 0x0C;
constexpr size_t kReplayHeaderP2PaletteOffset = 0x0D;
constexpr size_t kReplayHeaderP1NameOffset = 0x0E;
constexpr size_t kReplayHeaderP2NameOffset = 0x23;
constexpr size_t kReplayHeaderNameBytes = 21;
constexpr size_t kReplayPaletteTrailerHeaderSize = 16;
constexpr size_t kReplayPaletteTrailerRecordSize = 8 + Net::NETPLAY_PALETTE_BANK_SIZE;
constexpr int32_t kReplayMenuSelectSubstate = 2;
constexpr int32_t kReplayMenuFadeOutSubstate = 3;
constexpr size_t kReplaySelectDisplayBytes = ADDR_REPLAY_HEADER_BASE - ADDR_REPLAY_SELECT_DISPLAY;
constexpr int32_t kCoarseCheckpointInterval = 600;
constexpr int32_t kSeekFramesPerHotkey = 60;
constexpr int32_t kTakeoverCountdownFrames = 60;
// The scroll clamp and the draw loop must agree or the selection can sit
// outside the drawn rows. 7 since the folder summary took a row's worth of
// height above the list.
constexpr int32_t kReplayBrowserPageSize = 7;
constexpr float kSeekScale = 16.0f;
constexpr float kSpeedSteps[] = {0.5f, 1.0f, 1.25f, 1.5f, 2.0f, 4.0f};

constexpr uintptr_t kAddrRenderFillRect = 0x5D2F50;
constexpr uintptr_t kAddrRenderSetBlendMode = 0x5D2F80;
constexpr uintptr_t kAddrRenderCreateColor = 0x5D3150;
constexpr uintptr_t kAddrRenderDrawSprite = 0x5D3130;
constexpr uintptr_t kAddrDrawFormatString = 0x629A20;
constexpr uintptr_t kAddrReplayMenuBackgroundHandle = 0x815E04;

// Vanilla label sprites sample neutral grey, so nothing here is tinted.
constexpr uint8_t kRepInk       = 232;
constexpr uint8_t kRepInkBright = 255;
constexpr uint8_t kRepInkDim    = 168;
constexpr uint8_t kRepInkFaint  = 132;
constexpr float   kRepTitleSize = 26.0f;
constexpr float   kRepBodySize  = 20.0f;
constexpr float   kRepNoteSize  = 16.0f;

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

// re0.7 M7 (S-5): confirmed-stream trailer chunk. Appended after the palette
// trailer on netplay replay saves; carries the epoch-tagged confirmed input
// stream plus a pre-state digest every 30 frames so playback can verify the
// file is canonical (a predicted value can never enter it) and hash-clean.
// The extension is additive: 0.6-era files (no chunk) load and play
// unchanged; the chunk scanner ignores unknown trailing chunks.
constexpr std::array<uint8_t, 8> kReplayConfirmedTrailerMagic = {
    'A', 'S', '2', 'R', 'C', 'F', 'M', '1'
};
constexpr uint32_t kReplayConfirmedTrailerVersion = 1;
// magic[8] + version + epoch + first_game_abs + record_count + hash_count +
// payload_crc (all u32).
constexpr size_t kReplayConfirmedTrailerHeaderSize = 8 + 6 * sizeof(uint32_t);
constexpr size_t kReplayConfirmedInputRecordSize = 2 * sizeof(uint16_t);
constexpr size_t kReplayConfirmedHashRecordSize = sizeof(uint32_t) + sizeof(uint64_t);
constexpr uint32_t kReplayConfirmedHashCadence = 30;   // §2.7.7 SyncHash cadence
// Hash-verify sync-acquire: the replay playback machine reaches gameplay via
// the native loader, so the first records may not be bit-aligned with the
// recorder's baseline. Arm strict verification on the first agreeing digest;
// give up (fail-degrade, loud log) if none of the first N digests agree.
constexpr uint32_t kReplayVerifyAcquireWindowHashes = 20;

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

// ── M7 (S-5): confirmed-stream recorder (host side, fed from the engine2
// confirm seam) ─────────────────────────────────────────────────────────────
struct ConfirmedInputRecord {
    uint16_t p1;
    uint16_t p2;
};
static_assert(sizeof(ConfirmedInputRecord) == 4,
    "ConfirmedInputRecord is memcpy'd to/from the trailer chunk — must stay 4 bytes");
struct ConfirmedHashRecord {
    uint32_t index;   // record index (game_abs − first_game_abs)
    uint64_t hash;    // Block64 confirmed pre-tick gameplay digest
};
static uint32_t s_recEpoch = 0;
static int32_t s_recFirstGameAbs = -1;
static bool s_recContiguityBroken = false;
static uint32_t s_recDropped = 0;
static std::vector<ConfirmedInputRecord> s_recRecords;
static std::vector<ConfirmedHashRecord> s_recHashes;

// ── M7 (S-5): loaded confirmed stream + playback verification latches ──────
struct LoadedConfirmedStream {
    bool present = false;
    uint32_t epoch = 0;
    int32_t first_game_abs = -1;
    std::vector<ConfirmedInputRecord> records;
    std::map<int32_t, uint64_t> hashes;   // record index → digest
};
static LoadedConfirmedStream s_loadedConfirmed;
static bool s_verifyDisabled = false;
static bool s_verifyAcquired = false;
static uint32_t s_verifyEntryMismatches = 0;
static uint32_t s_verifyInputMismatches = 0;
static uint32_t s_verifyHashMismatches = 0;
static uint32_t s_verifyHashChecks = 0;

static fs::path s_netplaySetFolder;
static std::string s_netplaySetKey;

static uint32_t ReadU32(const uint8_t* data);
static std::string WideToUtf8(const std::wstring& text);
static std::string WideToGameText(const std::wstring& text);
static bool IsReplayExtension(const fs::path& path);
static bool IsReplayMenuContext();
static bool IsReplayMenuSelectContext();
static void DeactivateReplayMatch(const char* reason);
static bool ExitReplayPlaybackToReplayMenu(const char* reason);
static void RenderReplayBrowserHud();
static bool ReadReplayMetadata(const fs::path& path, ReplayFileMetadata* outMetadata);
static bool ShouldRenameNetplayReplaySave(const Net::SessionSnapshot& session);
static bool WriteNetplayNamesIntoHeader(const fs::path& replayPath);
static bool RenameReplaySaveForNetplay(const fs::path& replayPath,
                                       const ReplayFileMetadata& metadata,
                                       fs::path* outRenamedPath);
static bool MoveReplayIntoNetplaySetFolder(const fs::path& replayPath,
                                           const ReplayFileMetadata& metadata,
                                           fs::path* outMovedPath);

using GetWindowHandle_t = LPVOID (__cdecl *)();

static bool RawKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static HWND GetGameWindowHandle() {
    auto getWindowHandle = reinterpret_cast<GetWindowHandle_t>(ADDR_WINDOW_GET_HANDLE);
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

// Play one of the vanilla replay-select SFX handles (wave\rep.bin). The handles
// at 0x815E88/8C/90 are populated by the vanilla mode-5 init that still runs;
// we only override the draw, so they are valid while the browser is active.
static void PlayReplayMenuSfx(uintptr_t handleAddr) {
    using ReplayAudioPlay_t = int (__cdecl*)(int handle);
    const uint32_t handle = ReadMemory<uint32_t>(handleAddr);
    if (handle != 0) {
        reinterpret_cast<ReplayAudioPlay_t>(ADDR_AUDIO_PLAY_HANDLE)(static_cast<int>(handle));
    }
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

static void ResetLoadedConfirmedStream() {
    s_loadedConfirmed.present = false;
    s_loadedConfirmed.epoch = 0;
    s_loadedConfirmed.first_game_abs = -1;
    s_loadedConfirmed.records.clear();
    s_loadedConfirmed.hashes.clear();
    s_verifyDisabled = false;
    s_verifyAcquired = false;
    s_verifyEntryMismatches = 0;
    s_verifyInputMismatches = 0;
    s_verifyHashMismatches = 0;
    s_verifyHashChecks = 0;
}

static void ResetConfirmedRecorder(const char* reason) {
    if (!s_recRecords.empty()) {
        LOG_INFO("[Replay] Confirmed recorder reset (%s): epoch=%u records=%zu hashes=%zu dropped=%u",
            reason ? reason : "?",
            s_recEpoch,
            s_recRecords.size(),
            s_recHashes.size(),
            s_recDropped);
    }
    s_recEpoch = 0;
    s_recFirstGameAbs = -1;
    s_recContiguityBroken = false;
    s_recDropped = 0;
    s_recRecords.clear();
    s_recHashes.clear();
}

static void ResetLoadedReplayPaletteState() {
    memset(s_loadedReplayPalette, 0, sizeof(s_loadedReplayPalette));
    // Loaded-file state travels together: a new load or unload also drops
    // the confirmed-stream verification data (M7).
    ResetLoadedConfirmedStream();
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

// M7 (S-5): append the confirmed-stream chunk for the recorder's current
// epoch (one match — the native save is one file per match, so the epoch
// boundary IS the chapter boundary). Consumes the recorder.
static bool AppendReplayConfirmedTrailer(const fs::path& path) {
    if (s_recRecords.empty() || s_recFirstGameAbs < 0) {
        return true;   // nothing recorded (offline match)
    }

    std::vector<uint8_t> payload;
    payload.reserve(s_recRecords.size() * kReplayConfirmedInputRecordSize +
                    s_recHashes.size() * kReplayConfirmedHashRecordSize);
    const uint8_t* recordBytes = reinterpret_cast<const uint8_t*>(s_recRecords.data());
    payload.insert(payload.end(), recordBytes,
                   recordBytes + s_recRecords.size() * kReplayConfirmedInputRecordSize);
    for (const ConfirmedHashRecord& hashRecord : s_recHashes) {
        AppendU32(&payload, hashRecord.index);
        const uint8_t* hashBytes = reinterpret_cast<const uint8_t*>(&hashRecord.hash);
        payload.insert(payload.end(), hashBytes, hashBytes + sizeof(hashRecord.hash));
    }

    std::vector<uint8_t> chunk;
    chunk.reserve(kReplayConfirmedTrailerHeaderSize + payload.size());
    chunk.insert(chunk.end(), kReplayConfirmedTrailerMagic.begin(),
                 kReplayConfirmedTrailerMagic.end());
    AppendU32(&chunk, kReplayConfirmedTrailerVersion);
    AppendU32(&chunk, s_recEpoch);
    AppendU32(&chunk, (uint32_t)s_recFirstGameAbs);
    AppendU32(&chunk, (uint32_t)s_recRecords.size());
    AppendU32(&chunk, (uint32_t)s_recHashes.size());
    AppendU32(&chunk, CalcCRC32(payload.data(), payload.size()));
    chunk.insert(chunk.end(), payload.begin(), payload.end());

    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) {
        LOG_ERROR("[Replay] Failed to append confirmed trailer: %s", WideToUtf8(path.wstring()).c_str());
        return false;
    }
    stream.write(reinterpret_cast<const char*>(chunk.data()),
                 static_cast<std::streamsize>(chunk.size()));
    if (!stream) {
        LOG_ERROR("[Replay] Confirmed trailer write failed: %s", WideToUtf8(path.wstring()).c_str());
        return false;
    }

    LOG_INFO("[Replay] Appended confirmed trailer to %s (epoch=%u first_abs=%d records=%zu hashes=%zu dropped=%u contiguous=%d)",
        WideToUtf8(path.wstring()).c_str(),
        s_recEpoch,
        s_recFirstGameAbs,
        s_recRecords.size(),
        s_recHashes.size(),
        s_recDropped,
        s_recContiguityBroken ? 0 : 1);
    ResetConfirmedRecorder("consumed by replay save");
    return true;
}

static uint8_t ReplayHeaderPaletteForSlot(const ReplayFileMetadata& metadata, uint8_t slot) {
    return slot == 0
        ? metadata.header[kReplayHeaderP1PaletteOffset]
        : metadata.header[kReplayHeaderP2PaletteOffset];
}

static uint32_t ReplayHeaderCharacterForSlot(const ReplayFileMetadata& metadata, uint8_t slot) {
    return slot == 0 ? metadata.p1_char : metadata.p2_char;
}

static std::string ReplayHeaderNameForSlot(const ReplayFileMetadata& metadata, uint8_t slot) {
    const size_t offset = slot == 0 ? kReplayHeaderP1NameOffset : kReplayHeaderP2NameOffset;
    std::string name(reinterpret_cast<const char*>(metadata.header.data() + offset),
        kReplayHeaderNameBytes);
    while (!name.empty() && (name.back() == '\0' || name.back() == ' ')) {
        name.pop_back();
    }
    return name;
}

// Parses one palette chunk starting at data[0]. Returns the chunk's byte
// length (0 = malformed, caller stops scanning).
static size_t ParsePaletteChunk(const uint8_t* data,
                                size_t available,
                                const ReplayFileMetadata& metadata,
                                const char* replayPathForLog) {
    if (available < kReplayPaletteTrailerHeaderSize) {
        return 0;
    }

    const uint32_t version = ReadU32(data + kReplayPaletteTrailerMagic.size());
    const uint32_t flags = ReadU32(data + kReplayPaletteTrailerMagic.size() + sizeof(uint32_t));
    const uint32_t supportedFlags = kReplayPaletteFlagP1 | kReplayPaletteFlagP2;
    if (version != kReplayPaletteTrailerVersion || (flags & ~supportedFlags) != 0) {
        LOG_WARN("[Replay] Ignoring unsupported palette trailer in %s (version=%u flags=0x%08X)",
            replayPathForLog ? replayPathForLog : "(unknown)",
            version,
            flags);
        return 0;
    }

    size_t expectedSize = kReplayPaletteTrailerHeaderSize;
    if ((flags & kReplayPaletteFlagP1) != 0) {
        expectedSize += kReplayPaletteTrailerRecordSize;
    }
    if ((flags & kReplayPaletteFlagP2) != 0) {
        expectedSize += kReplayPaletteTrailerRecordSize;
    }
    if (available < expectedSize) {
        LOG_WARN("[Replay] Ignoring truncated palette trailer in %s (bytes=%zu expected=%zu)",
            replayPathForLog ? replayPathForLog : "(unknown)",
            available,
            expectedSize);
        return 0;
    }

    size_t offset = kReplayPaletteTrailerHeaderSize;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        const uint32_t flag = slot == 0 ? kReplayPaletteFlagP1 : kReplayPaletteFlagP2;
        if ((flags & flag) == 0) {
            continue;
        }

        const uint8_t characterId = data[offset + 0];
        const uint8_t basePalette = data[offset + 1];
        const uint32_t storedCrc = ReadU32(data + offset + 4);
        const uint8_t* bankData = data + offset + 8;
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
    return expectedSize;
}

// M7 (S-5): parses one confirmed-stream chunk. Returns the chunk's byte
// length (0 = malformed).
static size_t ParseConfirmedChunk(const uint8_t* data,
                                  size_t available,
                                  const char* replayPathForLog) {
    if (available < kReplayConfirmedTrailerHeaderSize) {
        return 0;
    }

    size_t off = kReplayConfirmedTrailerMagic.size();
    const uint32_t version = ReadU32(data + off); off += 4;
    const uint32_t epoch = ReadU32(data + off); off += 4;
    const int32_t firstGameAbs = (int32_t)ReadU32(data + off); off += 4;
    const uint32_t recordCount = ReadU32(data + off); off += 4;
    const uint32_t hashCount = ReadU32(data + off); off += 4;
    const uint32_t storedCrc = ReadU32(data + off); off += 4;

    if (version != kReplayConfirmedTrailerVersion) {
        LOG_WARN("[Replay] Ignoring unsupported confirmed trailer in %s (version=%u)",
            replayPathForLog ? replayPathForLog : "(unknown)", version);
        return 0;
    }
    if (recordCount > (uint32_t)INPUT_HISTORY_MAX ||
        hashCount > recordCount / kReplayConfirmedHashCadence + 1) {
        LOG_WARN("[Replay] Ignoring implausible confirmed trailer in %s (records=%u hashes=%u)",
            replayPathForLog ? replayPathForLog : "(unknown)", recordCount, hashCount);
        return 0;
    }

    const size_t payloadBytes =
        (size_t)recordCount * kReplayConfirmedInputRecordSize +
        (size_t)hashCount * kReplayConfirmedHashRecordSize;
    const size_t chunkBytes = kReplayConfirmedTrailerHeaderSize + payloadBytes;
    if (available < chunkBytes) {
        LOG_WARN("[Replay] Ignoring truncated confirmed trailer in %s (bytes=%zu expected=%zu)",
            replayPathForLog ? replayPathForLog : "(unknown)", available, chunkBytes);
        return 0;
    }

    const uint8_t* payload = data + kReplayConfirmedTrailerHeaderSize;
    const uint32_t computedCrc = CalcCRC32(payload, payloadBytes);
    if (computedCrc != storedCrc) {
        LOG_WARN("[Replay] Ignoring corrupt confirmed trailer in %s (stored=0x%08X actual=0x%08X)",
            replayPathForLog ? replayPathForLog : "(unknown)", storedCrc, computedCrc);
        return chunkBytes;   // well-formed length, bad payload — skip it
    }

    s_loadedConfirmed.present = true;
    s_loadedConfirmed.epoch = epoch;
    s_loadedConfirmed.first_game_abs = firstGameAbs;
    s_loadedConfirmed.records.resize(recordCount);
    if (recordCount > 0) {
        memcpy(s_loadedConfirmed.records.data(), payload,
               (size_t)recordCount * kReplayConfirmedInputRecordSize);
    }
    s_loadedConfirmed.hashes.clear();
    const uint8_t* hashData = payload + (size_t)recordCount * kReplayConfirmedInputRecordSize;
    for (uint32_t i = 0; i < hashCount; ++i) {
        const uint8_t* rec = hashData + (size_t)i * kReplayConfirmedHashRecordSize;
        const uint32_t index = ReadU32(rec);
        uint64_t hash = 0;
        memcpy(&hash, rec + 4, sizeof(hash));
        s_loadedConfirmed.hashes[(int32_t)index] = hash;
    }

    LOG_INFO("[Replay] Loaded confirmed trailer from %s: epoch=%u first_abs=%d records=%u hashes=%u",
        replayPathForLog ? replayPathForLog : "(unknown)",
        epoch, firstGameAbs, recordCount, hashCount);
    return chunkBytes;
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

    // M7: chunk scan — the trailing region holds zero or more self-
    // identifying chunks (palette, confirmed stream) in any order. Old
    // palette-only files parse exactly as before; unknown bytes stop the
    // scan (forward compat, never fatal).
    size_t offset = 0;
    while (trailer.size() - offset >= kReplayPaletteTrailerMagic.size()) {
        const uint8_t* chunk = trailer.data() + offset;
        const size_t available = trailer.size() - offset;
        size_t consumed = 0;
        if (memcmp(chunk, kReplayPaletteTrailerMagic.data(),
                   kReplayPaletteTrailerMagic.size()) == 0) {
            consumed = ParsePaletteChunk(chunk, available, metadata, replayPathForLog);
        } else if (memcmp(chunk, kReplayConfirmedTrailerMagic.data(),
                          kReplayConfirmedTrailerMagic.size()) == 0) {
            consumed = ParseConfirmedChunk(chunk, available, replayPathForLog);
        } else {
            if (offset == 0) {
                // No known chunk at the trailer start — legacy file with
                // unrelated trailing bytes; keep the old silent behavior.
                return;
            }
            LOG_WARN("[Replay] Unknown trailer chunk in %s at offset %zu — stopping scan",
                replayPathForLog ? replayPathForLog : "(unknown)", offset);
            return;
        }
        if (consumed == 0) {
            return;
        }
        offset += consumed;
    }
}

static char __cdecl Hook_ReplaySave(int matchBase) {
    Net::SessionSnapshot session{};
    Net::Session_GetSnapshot(&session);

    ReplayPaletteOverrideState banks[2] = {};
    ReplayDirectorySnapshot before;
    const bool hasPaletteTrailer = BuildReplayPaletteSaveData(banks);
    const bool shouldRenameNetplayReplay = ShouldRenameNetplayReplaySave(session);
    // M7 (S-5): the confirmed chunk is netplay-context-gated exactly like the
    // rename — a later LOCAL save must never inherit a stale netplay stream.
    const bool hasConfirmedTrailer = shouldRenameNetplayReplay &&
                                     !s_recRecords.empty() &&
                                     s_recFirstGameAbs >= 0;
    if (hasPaletteTrailer || shouldRenameNetplayReplay || hasConfirmedTrailer) {
        before = CaptureReplayDirectorySnapshot();
    }

    const char result = s_originalReplaySave ? s_originalReplaySave(matchBase) : 0;

    if (!hasPaletteTrailer && !shouldRenameNetplayReplay && !hasConfirmedTrailer) {
        return result;
    }

    const ReplayDirectorySnapshot after = CaptureReplayDirectorySnapshot();
    fs::path replayPath;
    if (!ResolveReplaySavePath(before, after, &replayPath)) {
        // No file changed in the replay directory. If the vanilla save itself
        // reported nothing saved, that is simply "there was no replay to
        // post-process" — an ordinary outcome (and the norm in automated soak
        // runs), not an anomaly. Only the inconsistent case deserves a warning:
        // vanilla says it saved, yet no file appeared.
        if (result) {
            LOG_WARN("[Replay] Saved replay could not be resolved for post-save "
                     "processing (vanilla save reported success)");
        } else {
            LOG_INFO("[Replay] No replay written by the vanilla save — nothing to "
                     "post-process");
        }
        return result;
    }

    if (hasPaletteTrailer) {
        AppendReplayPaletteTrailer(replayPath, banks);
    }

    if (hasConfirmedTrailer) {
        AppendReplayConfirmedTrailer(replayPath);
    }

    if (shouldRenameNetplayReplay) {
        ReplayFileMetadata metadata{};
        if (ReadReplayMetadata(replayPath, &metadata) && metadata.valid) {
            // Before the rename, so the path is still the one just written.
            WriteNetplayNamesIntoHeader(replayPath);

            fs::path processedPath = replayPath;
            if (RenameReplaySaveForNetplay(replayPath, metadata, &processedPath)) {
                fs::path movedPath;
                if (!MoveReplayIntoNetplaySetFolder(processedPath, metadata, &movedPath)) {
                    LOG_WARN("[Replay] Netplay replay set-folder move failed; replay remains at %s",
                        WideToUtf8(processedPath.wstring()).c_str());
                }
            }
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

static std::string FormatReplayTimestampForFolder(const fs::path& path) {
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
        return "unknown_time";
    }

    char buffer[32] = {};
    strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &localTime);
    return buffer;
}

// One roster, not a second copy of it.
//
// This was its own switch that stopped at 17, so every replay featuring Demon
// Rance, Little Princess, TADA or Nalzgis Boss was filed as "Char18".."Char21".
// It also would have been a second place to get ids 19/20 the wrong way round.
static std::string GetCharacterDisplayName(uint32_t characterId) {
    const char* name = PracticeTools_CharacterName(characterId);
    if (name && strcmp(name, "Unknown") != 0) {
        return name;
    }
    char buffer[32] = {};
    snprintf(buffer, sizeof(buffer), "Char%u", characterId);
    return buffer;
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

struct NetplayReplayNames {
    std::string local_nickname;
    std::string remote_nickname;
    std::string p1_nickname;
    std::string p2_nickname;
};

static NetplayReplayNames BuildNetplayReplayNames() {
    Net::SessionSnapshot session{};
    Net::Session_GetSnapshot(&session);

    NetplayReplayNames names{};
    names.local_nickname = SanitizeReplayFilenameComponent(
        session.local_nickname[0] ? session.local_nickname : "Local",
        "Local");
    names.remote_nickname = SanitizeReplayFilenameComponent(
        session.remote_peer.nickname[0] ? session.remote_peer.nickname : "Remote",
        "Remote");

    int localSlot = Net::PlayerMapping_GetLocalGameSlot();
    if (localSlot != 0 && localSlot != 1) {
        localSlot = Net::Session_GetRole() == Net::SessionRole::Host ? 0 : 1;
    }

    names.p1_nickname = localSlot == 0 ? names.local_nickname : names.remote_nickname;
    names.p2_nickname = localSlot == 0 ? names.remote_nickname : names.local_nickname;
    return names;
}

static std::string BuildNetplayReplayFilename(const fs::path& replayPath,
                                              const ReplayFileMetadata& metadata) {
    const NetplayReplayNames names = BuildNetplayReplayNames();
    const std::string p1Character = SanitizeReplayFilenameComponent(GetCharacterDisplayName(metadata.p1_char), "P1Char");
    const std::string p2Character = SanitizeReplayFilenameComponent(GetCharacterDisplayName(metadata.p2_char), "P2Char");

    return FormatReplayTimestampForFilename(replayPath) + "_" +
        names.p1_nickname + "_" + p1Character + "_vs_" +
        names.p2_nickname + "_" + p2Character + ".rep";
}

// UTF-8 in, the game's own encoding out. The inverse of GameTextToUtf8 further
// down; both are local because the netplay menu's copies are file-static.
static std::string Utf8ToGameText(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int wideSize = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wideSize <= 1) {
        return utf8;
    }
    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wideSize);
    wide.resize(static_cast<size_t>(wideSize - 1));

    const int size = WideCharToMultiByte(932, 0, wide.c_str(), -1, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 1) {
        return utf8;
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(932, 0, wide.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

// The header has two 21-byte name fields the game fills in for its own replays
// and leaves empty for ours, which is why the browser had to reconstruct the
// nicknames from the filename. Writing them puts the data where it belongs: it
// survives a rename, it needs no parsing, and the game's own replay HUD picks it
// up because that is already where it reads the names from.
static bool WriteNetplayNamesIntoHeader(const fs::path& replayPath) {
    const NetplayReplayNames names = BuildNetplayReplayNames();
    if (names.p1_nickname.empty() && names.p2_nickname.empty()) {
        return false;
    }

    std::fstream stream(replayPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        LOG_WARN("[Replay] Could not open %s to write nicknames",
                 WideToUtf8(replayPath.wstring()).c_str());
        return false;
    }

    // The field is fixed width and the game reads it as its own encoding, so it
    // is converted and zero-padded rather than written as UTF-8.
    const auto writeField = [&](size_t offset, const std::string& utf8) {
        std::array<char, kReplayHeaderNameBytes> field{};
        const std::string encoded = Utf8ToGameText(utf8);
        const size_t length = (std::min)(encoded.size(), field.size() - 1u);
        std::memcpy(field.data(), encoded.data(), length);
        stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.write(field.data(), static_cast<std::streamsize>(field.size()));
    };

    writeField(kReplayHeaderP1NameOffset, names.p1_nickname);
    writeField(kReplayHeaderP2NameOffset, names.p2_nickname);
    stream.flush();
    if (!stream) {
        LOG_WARN("[Replay] Failed writing nicknames into %s",
                 WideToUtf8(replayPath.wstring()).c_str());
        return false;
    }

    LOG_INFO("[Replay] Wrote nicknames into header: p1='%s' p2='%s'",
             names.p1_nickname.c_str(), names.p2_nickname.c_str());
    return true;
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

// Who is playing, and nothing else.
//
// This used to include the local game slot and the session role. Both change
// when the two sides swap - which happens between matches of a normal set - so
// the folder was being torn down and rebuilt mid-session. That is what produced
// runs like 23-24-55 / 23-27-21 / 23-31-02: three folders, seven minutes, one
// set. Characters were never in the key and must not be: a set is the people,
// not the matchup.
static std::string BuildNetplayReplaySetKey(const NetplayReplayNames& names) {
    char buffer[256] = {};
    snprintf(buffer, sizeof(buffer), "%s|%s",
        names.p1_nickname.c_str(),
        names.p2_nickname.c_str());
    return buffer;
}

static void ResetNetplayReplaySetFolder(const char* reason) {
    if (!s_netplaySetFolder.empty()) {
        LOG_INFO("[Replay] Reset netplay replay set folder (%s): %s",
            reason ? reason : "unknown",
            WideToUtf8(s_netplaySetFolder.wstring()).c_str());
    }
    s_netplaySetFolder.clear();
    s_netplaySetKey.clear();
}

static bool EnsureNetplayReplaySetFolder(const fs::path& replayPath,
                                         const ReplayFileMetadata& metadata,
                                         fs::path* outFolder) {
    if (!outFolder) {
        return false;
    }

    const NetplayReplayNames names = BuildNetplayReplayNames();
    const std::string key = BuildNetplayReplaySetKey(names);

    if (!s_netplaySetKey.empty() && s_netplaySetKey != key) {
        ResetNetplayReplaySetFolder("participant change");
    }
    // The set-counter comparison that used to sit here also split folders
    // mid-session. SetTracker_Reset only runs on disconnect, so a falling
    // total_matches meant a rematch had restarted the count - a new match, not
    // a new set. Session end is now the only thing that closes a folder, via
    // ReplayRuntime_OnNetplaySessionEnd.

    std::error_code ec;
    if (!s_netplaySetFolder.empty()) {
        if (!fs::exists(s_netplaySetFolder, ec)) {
            ec.clear();
            if (!fs::create_directories(s_netplaySetFolder, ec) || ec) {
                LOG_WARN("[Replay] Existing netplay set folder could not be recreated: %s (%s)",
                    WideToUtf8(s_netplaySetFolder.wstring()).c_str(),
                    ec.message().c_str());
                ResetNetplayReplaySetFolder("folder missing");
            }
        }
    }

    if (s_netplaySetFolder.empty()) {
        // No characters in the name. The folder holds a whole set, and the
        // characters in it change from match to match - naming it after
        // whichever pair happened to play first was both wrong for most of its
        // contents and made per-session folders read as per-matchup ones. The
        // individual replay filenames still carry the characters.
        const std::string baseName =
            names.p1_nickname + "_vs_" + names.p2_nickname + " - " +
            FormatReplayTimestampForFolder(replayPath);
        fs::path candidate = fs::path(L"replay") / L"netplay" / fs::path(baseName);

        int suffix = 2;
        while (fs::exists(candidate, ec)) {
            ec.clear();
            char suffixBuffer[16] = {};
            snprintf(suffixBuffer, sizeof(suffixBuffer), "_%d", suffix++);
            candidate = fs::path(L"replay") / L"netplay" / fs::path(baseName + suffixBuffer);
        }

        if (!fs::create_directories(candidate, ec) && ec) {
            LOG_WARN("[Replay] Failed to create netplay replay set folder %s (%s)",
                WideToUtf8(candidate.wstring()).c_str(),
                ec.message().c_str());
            return false;
        }

        s_netplaySetFolder = candidate;
        s_netplaySetKey = key;
        LOG_INFO("[Replay] Created netplay replay set folder: %s",
            WideToUtf8(s_netplaySetFolder.wstring()).c_str());
    }

    *outFolder = s_netplaySetFolder;
    return true;
}

static bool MoveReplayIntoNetplaySetFolder(const fs::path& replayPath,
                                           const ReplayFileMetadata& metadata,
                                           fs::path* outMovedPath) {
    fs::path folder;
    if (!EnsureNetplayReplaySetFolder(replayPath, metadata, &folder)) {
        return false;
    }

    fs::path candidate = folder / replayPath.filename();
    const std::string stem = candidate.stem().string();
    const std::string extension = candidate.extension().string();

    int suffix = 2;
    std::error_code ec;
    while (candidate != replayPath && fs::exists(candidate, ec)) {
        ec.clear();

        char suffixBuffer[16] = {};
        snprintf(suffixBuffer, sizeof(suffixBuffer), "_%d", suffix++);
        candidate = folder / fs::path(stem + suffixBuffer + extension);
    }

    if (candidate == replayPath) {
        if (outMovedPath) {
            *outMovedPath = replayPath;
        }
        return true;
    }

    fs::rename(replayPath, candidate, ec);
    if (ec) {
        LOG_WARN("[Replay] Failed to move saved replay into set folder %s -> %s (%s)",
            WideToUtf8(replayPath.wstring()).c_str(),
            WideToUtf8(candidate.wstring()).c_str(),
            ec.message().c_str());
        return false;
    }

    LOG_INFO("[Replay] Moved saved replay into set folder: %s -> %s",
        WideToUtf8(replayPath.wstring()).c_str(),
        WideToUtf8(candidate.wstring()).c_str());
    if (outMovedPath) {
        *outMovedPath = candidate;
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
    const char* reason = "replay_speed";
    char reasonBuf[48] = {};
    if (s_seekTargetFrame >= 0) {
        scale = kSeekScale;
        reason = "replay_seek_fast";
    } else if (s_paused) {
        scale = 1.0f;
        reason = "replay_paused";
    } else {
        _snprintf_s(reasonBuf, sizeof(reasonBuf), _TRUNCATE, "replay_speed_%.2fx", scale);
        reason = reasonBuf;
    }

    SetGlobalTickScale(scale, reason);
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

static bool HandleMatchHotkeys() {
    const bool shiftDown = KeyDown(VK_SHIFT);

    if (ConsumeEdge(kHotkeyToggleHud, &s_toggleHudKeyWasDown)) {
        ReplayRuntime_SetHudVisible(!s_replayHudVisible);
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
        if (s_takeoverMode != TakeoverMode::None) {
            ExitTakeover();
        } else {
            return ExitReplayPlaybackToReplayMenu("Exited replay playback.");
        }
    }

    return false;
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

    const std::string p1HeaderName = ReplayHeaderNameForSlot(metadata, 0);
    const std::string p2HeaderName = ReplayHeaderNameForSlot(metadata, 1);
    LOG_INFO("[Replay] Header parse %s: p1_char=%u p1_palette=%u p1_name='%s' "
             "p2_char=%u p2_palette=%u p2_name='%s' frames=%d",
        entry.relative_path.c_str(),
        metadata.p1_char,
        metadata.header[kReplayHeaderP1PaletteOffset],
        p1HeaderName.c_str(),
        metadata.p2_char,
        metadata.header[kReplayHeaderP2PaletteOffset],
        p2HeaderName.c_str(),
        metadata.frames);

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
    WriteMemory<uint8_t>(ADDR_CHARSEL_P1_PALETTE,
                         metadata.header[kReplayHeaderP1PaletteOffset]);
    WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_CHARSEL_P1_PALETTE + 1),
                         metadata.header.data() + kReplayHeaderP1NameOffset,
                         kReplayHeaderNameBytes);
    WriteMemory<uint8_t>(ADDR_CHARSEL_P2_PALETTE,
                         metadata.header[kReplayHeaderP2PaletteOffset]);
    WriteMemoryBlockSafe(reinterpret_cast<void*>(ADDR_CHARSEL_P2_PALETTE + 1),
                         metadata.header.data() + kReplayHeaderP2NameOffset,
                         kReplayHeaderNameBytes);
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
            PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CURSOR);
        }
    }

    if (ReplayMenuInputJustPressed(INPUT_DOWN)) {
        if (!s_browserEntries.empty()) {
            s_browserSelected = (s_browserSelected + 1) % static_cast<int32_t>(s_browserEntries.size());
            ClampBrowserSelection();
            PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CURSOR);
        }
    }

    if (ReplayMenuInputJustPressed(INPUT_LEFT)) {
        s_browserSelected -= kReplayBrowserPageSize;
        ClampBrowserSelection();
        PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CURSOR);
    }

    if (ReplayMenuInputJustPressed(INPUT_RIGHT)) {
        s_browserSelected += kReplayBrowserPageSize;
        ClampBrowserSelection();
        PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CURSOR);
    }

    if (ConsumeEdge(VK_HOME, &s_menuHomeWasDown)) {
        s_browserSelected = 0;
        ClampBrowserSelection();
        PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CURSOR);
    }

    if (ConsumeEdge(VK_END, &s_menuEndWasDown)) {
        s_browserSelected = static_cast<int32_t>(s_browserEntries.size()) - 1;
        ClampBrowserSelection();
        PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CURSOR);
    }

    if (ReplayMenuInputJustPressed(INPUT_A) ||
        ReplayMenuInputJustPressed(INPUT_C) ||
        ReplayMenuInputJustPressed(INPUT_START)) {
        if (s_browserEntries.empty()) {
            SetBrowserStatus("No replay folders or files are available here.");
        } else {
            PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CONFIRM);
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
        PlayReplayMenuSfx(ADDR_REPLAY_MENU_SFX_CANCEL);
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
    SetGlobalTickScale(s_savedGlobalTickScale, "replay_deactivate_restore");
    Rollback::StateHistory_Reset();
    ResetMatchRuntimeState();
    ResetLoadedReplayPaletteState();
    ResetMatchHotkeyEdges();
}

static bool ExitReplayPlaybackToReplayMenu(const char* reason) {
    if (!s_replayMatchActive && !IsReplayMatchContext()) {
        return false;
    }

    LOG_INFO("[Replay] Exit to replay menu requested (%s): mode=%u sub=%u frame=%d",
        reason ? reason : "no reason",
        GetGameMode(),
        GetSubstate(),
        s_currentFrame);

    DeactivateReplayMatch(reason ? reason : "exit to replay menu");
    s_replayLaunchPending = false;
    ResetLoadedReplayPaletteState();
    ResetBrowserState();
    ResetMatchHotkeyEdges();
    ResetMenuHotkeyEdges();
    ModeOwnership::CallOriginalSetGameMode(MODE_REPLAY_SELECT, 1);
    return true;
}

static void RenderReplayMatchHud() {
    if (!s_replayMatchActive || !s_replayHudVisible) {
        return;
    }

    ImDrawList* drawList = ModMenu_OverlayDrawList();  // stays below the mod menu when it's open
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

    snprintf(lines[3], sizeof(lines[3]), "0 Replay Menu  Bksl Pause  ] Fwd  [ Back");
    snprintf(lines[4], sizeof(lines[4]), "Shift+[ Rewind  +/- Speed  Ins HUD");
    snprintf(lines[5], sizeof(lines[5]), "1/2 Takeover or retry  0 exits takeover first");
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
    if (mode != 0) {
        NetMenu::MenuSetTextAlpha(alpha);
    }
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

// Explicit size, so the browser has a heading / body / note ladder instead of
// rendering everything at one weight.
static void GameDrawTextAt(int x, int y, uint8_t r, uint8_t g, uint8_t b,
                           float size, const char* text) {
    if (!text || !text[0]) {
        return;
    }
    NetMenu::MenuDrawTextSized(x, y, r, g, b, size, text);
}

static void GameDrawTextShadowed(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* text) {
    if (!text || !text[0]) {
        return;
    }

    // Same Mincho path as the settings and netplay menus; it draws its own
    // shadow, so only the fallback needs one here.
    NetMenu::MenuDrawText(x, y, r, g, b, text);
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

// A read on the folder currently open.
//
// Deliberately NOT recursive: it describes the directory being looked at, from
// the entries already listed, so opening a set folder summarises that set and
// the netplay root stays silent (it holds folders, not matches).
struct BrowserFolderSummary {
    bool        valid = false;
    int         replays = 0;
    int32_t     longestFrames = 0;
    int64_t     totalFrames = 0;
    std::string topP1;
    std::string topP2;
};

static std::string FormatReplayClock(int64_t frames) {
    const int64_t seconds = frames / 60;
    char buffer[32] = {};
    snprintf(buffer, sizeof(buffer), "%lld:%02lld",
             (long long)(seconds / 60), (long long)(seconds % 60));
    return buffer;
}

static BrowserFolderSummary BuildBrowserFolderSummary() {
    BrowserFolderSummary out{};
    std::map<uint32_t, int> p1Counts;
    std::map<uint32_t, int> p2Counts;

    for (const ReplayBrowserEntry& entry : s_browserEntries) {
        if (entry.type != ReplayBrowserEntryType::ReplayFile || !entry.metadata.valid) {
            continue;
        }
        ++out.replays;
        out.totalFrames += entry.metadata.frames;
        if (entry.metadata.frames > out.longestFrames) {
            out.longestFrames = entry.metadata.frames;
        }
        ++p1Counts[entry.metadata.p1_char];
        ++p2Counts[entry.metadata.p2_char];
    }

    if (out.replays == 0) {
        return out;
    }

    const auto topOf = [](const std::map<uint32_t, int>& counts) -> std::string {
        const uint32_t* best = nullptr;
        int bestCount = 0;
        for (const auto& pair : counts) {
            if (pair.second > bestCount) {
                bestCount = pair.second;
                best = &pair.first;
            }
        }
        return best ? GetCharacterDisplayName(*best) : std::string();
    };

    out.topP1 = topOf(p1Counts);
    out.topP2 = topOf(p2Counts);
    out.valid = true;
    return out;
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

// The header carries the player names in the game's own encoding, while the
// text bridge draws UTF-8, so they have to be converted before they can be
// shown. ASCII survives either way, which is why this only shows up once
// somebody plays with a Japanese or Cyrillic nickname.
static std::string GameTextToUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int wideSize = MultiByteToWideChar(932, 0, text.c_str(), -1, nullptr, 0);
    if (wideSize <= 1) {
        return text;
    }
    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(932, 0, text.c_str(), -1, wide.data(), wideSize);
    wide.resize(static_cast<size_t>(wideSize - 1));

    const int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                             nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 1) {
        return text;
    }
    std::string result(static_cast<size_t>(utf8Size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), utf8Size,
                        nullptr, nullptr);
    result.resize(static_cast<size_t>(utf8Size - 1));
    return result;
}

// Netplay saves put the nicknames in the *filename*, not the header - nothing
// writes the header's name fields, they are the game's own for local replays and
// stay empty otherwise. BuildNetplayReplayFilename lays it out as
//
//     {yyyymmdd}_{hhmmss}_{p1nick}_{p1char}_vs_{p2nick}_{p2char}.rep
//
// and the character names are already known from the metadata, so each side's
// nickname is whatever is left once its character suffix is removed.
static bool SplitNetplayReplayNames(const std::string& stem,
                                    const ReplayFileMetadata& metadata,
                                    std::string* outP1,
                                    std::string* outP2) {
    const std::string separator = "_vs_";
    const size_t split = stem.find(separator);
    if (split == std::string::npos) {
        return false;
    }

    std::string left = stem.substr(0, split);
    std::string right = stem.substr(split + separator.size());

    // Drop the leading timestamp, which is always two underscore-separated
    // fixed-width fields.
    size_t cursor = 0;
    for (int field = 0; field < 2; ++field) {
        const size_t underscore = left.find('_', cursor);
        if (underscore == std::string::npos) {
            return false;
        }
        cursor = underscore + 1;
    }
    left = left.substr(cursor);

    const auto stripCharacter = [](std::string& text, const std::string& character) {
        const std::string suffix = "_" + character;
        if (text.size() > suffix.size() &&
            text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0) {
            text.resize(text.size() - suffix.size());
            return true;
        }
        return false;
    };

    const std::string p1Character =
        SanitizeReplayFilenameComponent(GetCharacterDisplayName(metadata.p1_char), "P1Char");
    const std::string p2Character =
        SanitizeReplayFilenameComponent(GetCharacterDisplayName(metadata.p2_char), "P2Char");
    if (!stripCharacter(left, p1Character) || !stripCharacter(right, p2Character)) {
        return false;
    }

    // SanitizeReplayFilenameComponent turned every space into an underscore on
    // the way in, so turn them back for display. A nickname that genuinely
    // contained an underscore reads with a space instead, which is a better
    // trade than showing "Ev_Geniy".
    const auto unsanitize = [](std::string& text) {
        for (char& ch : text) {
            if (ch == '_') {
                ch = ' ';
            }
        }
    };
    unsanitize(left);
    unsanitize(right);

    *outP1 = left;
    *outP2 = right;
    return !left.empty() && !right.empty();
}

// "Aquatic (Fanel) vs Ev (Hatsune)". The filenames are far too long to read in
// the list column, so the detail bar is where the matchup actually gets said.
static std::string GetBrowserEntryDetailLine(const ReplayBrowserEntry& entry) {
    if (entry.type != ReplayBrowserEntryType::ReplayFile || !entry.metadata.valid) {
        return {};
    }

    std::string p1Name = GameTextToUtf8(ReplayHeaderNameForSlot(entry.metadata, 0));
    std::string p2Name = GameTextToUtf8(ReplayHeaderNameForSlot(entry.metadata, 1));
    if (p1Name.empty() && p2Name.empty()) {
        SplitNetplayReplayNames(entry.full_path.stem().string(), entry.metadata,
                                &p1Name, &p2Name);
    }
    const std::string p1Char = GetCharacterDisplayName(entry.metadata.p1_char);
    const std::string p2Char = GetCharacterDisplayName(entry.metadata.p2_char);

    // A replay saved outside netplay has no nicknames, so fall back to the
    // character-only form rather than printing empty brackets.
    if (p1Name.empty() && p2Name.empty()) {
        return p1Char + " vs " + p2Char;
    }
    return (p1Name.empty() ? p1Char : p1Name + " (" + p1Char + ")") + " vs " +
           (p2Name.empty() ? p2Char : p2Name + " (" + p2Char + ")");
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

    // Layout matches the settings and netplay screens: one panel, 32px rows,
    // a red bar on the selection, and a footer that follows the content.
    constexpr int kLeft      = 16;
    constexpr int kTop       = 16;
    constexpr int kRight     = 604;
    constexpr int kTextX     = 32;
    // Date and time were two columns, which split one value across a gap and
    // left the date 74px wide - narrow enough that "2026-04-19 20:33:11" was
    // clipped to "2026-04-...", i.e. the column showed no date at all. One
    // "Date" column now carries the whole stamp to the minute, and the duration
    // gets its own heading rather than borrowing the word "Time".
    constexpr int kMatchX    = 244;   // who fought          (18 chars)
    constexpr int kWhenX     = 396;   // YYYY-MM-DD HH:MM    (16 chars)
    constexpr int kLenX      = 540;   // mm:ss
    constexpr int kBarRight  = 588;
    constexpr int kRowPitch  = 32;
    constexpr int kFirstRow  = 150;  // heading, path, two summary lines, column titles
    // Driven by the scroll page size so the two can never disagree.
    constexpr int kRowsShown = kReplayBrowserPageSize;

    const int totalEntries = static_cast<int>(s_browserEntries.size());
    const bool hasSelection = s_browserSelected >= 0 && s_browserSelected < totalEntries;
    const ReplayBrowserEntry* selected = hasSelection
        ? &s_browserEntries[s_browserSelected] : nullptr;

    // Keep the selection on screen.
    if (s_browserScroll > s_browserSelected) {
        s_browserScroll = s_browserSelected;
    }
    if (s_browserSelected >= s_browserScroll + kRowsShown) {
        s_browserScroll = s_browserSelected - kRowsShown + 1;
    }
    if (s_browserScroll < 0) {
        s_browserScroll = 0;
    }

    const int listEnd = (std::min)(s_browserScroll + kRowsShown, totalEntries);
    const int shown = (std::max)(0, listEnd - s_browserScroll);

    // Panel sized to what it holds, plus the detail block and footer.
    const int listBottom = kFirstRow + shown * kRowPitch;
    const int detailTop  = listBottom + 10;
    const int footerTop  = detailTop + (selected ? 46 : 8);
    const int panelBottom = footerTop + 28;

    GameSetBlend(1, 128);
    GameFillRect(kLeft, kTop, kRight, panelBottom, 0, 0, 0);
    GameFillRect(kLeft, kTop, kRight, panelBottom, 0, 0, 0);
    GameSetBlend(1, 255);

    // Heading, with the folder being browsed underneath it.
    GameDrawTextAt(kTextX, kTop + 6, kRepInkBright, kRepInkBright, kRepInkBright,
                   kRepTitleSize, "REPLAYS");

    std::string pathText = s_browserCurrentDirectory.empty()
        ? std::string("replay/")
        : ("replay/" + s_browserCurrentDirectory.string());
    char clippedPath[160] = {};
    ClipGameText(clippedPath, sizeof(clippedPath), pathText.c_str(), 40);
    GameDrawTextAt(kTextX, kTop + 44, kRepInkDim, kRepInkDim, kRepInkDim,
                   kRepNoteSize, clippedPath);

    char counter[48] = {};
    if (totalEntries > 0) {
        snprintf(counter, sizeof(counter), "%d / %d", s_browserSelected + 1, totalEntries);
    } else {
        snprintf(counter, sizeof(counter), "empty");
    }
    GameDrawTextAt(kRight - 110, kTop + 44, kRepInkDim, kRepInkDim, kRepInkDim,
                   kRepNoteSize, counter);

    // Two summary lines: the counts distributed across the panel, then the
    // characters underneath. Distributing rather than running one long string
    // keeps the number you are usually after - the longest match - on the
    // panel's centre line instead of buried mid-sentence.
    const BrowserFolderSummary summary = BuildBrowserFolderSummary();
    if (summary.valid) {
        const auto textWidth = [](const char* text) {
            return (int)(GameTextCountChars(text) * (kRepNoteSize * 0.48f));
        };

        char countText[48] = {};
        snprintf(countText, sizeof(countText), "%d replay%s",
                 summary.replays, summary.replays == 1 ? "" : "s");

        char longestText[64] = {};
        snprintf(longestText, sizeof(longestText), "Longest Match %s",
                 FormatReplayClock(summary.longestFrames).c_str());

        char totalText[64] = {};
        snprintf(totalText, sizeof(totalText), "Total time %s",
                 FormatReplayClock(summary.totalFrames).c_str());

        const int summaryY = kTop + 64;
        const int centreX = (kTextX + kRight) / 2;

        GameDrawTextAt(kTextX, summaryY, kRepInkDim, kRepInkDim, kRepInkDim,
                       kRepNoteSize, countText);
        GameDrawTextAt(centreX - textWidth(longestText) / 2, summaryY,
                       kRepInk, kRepInk, kRepInk, kRepNoteSize, longestText);
        GameDrawTextAt(kRight - 16 - textWidth(totalText), summaryY,
                       kRepInkDim, kRepInkDim, kRepInkDim, kRepNoteSize, totalText);

        char charsText[160] = {};
        snprintf(charsText, sizeof(charsText),
                 "Most played characters:   P1: %s   P2: %s",
                 summary.topP1.c_str(), summary.topP2.c_str());
        char clippedChars[160] = {};
        ClipGameText(clippedChars, sizeof(clippedChars), charsText, 62);
        GameDrawTextAt(kTextX, summaryY + 20, kRepInkFaint, kRepInkFaint,
                       kRepInkFaint, kRepNoteSize, clippedChars);
    }

    GameDrawTextAt(kTextX + 8, kFirstRow - 20, kRepInkFaint, kRepInkFaint,
                   kRepInkFaint, kRepNoteSize, "Name");

    // Matchup/Date/Length describe a replay, and folder rows leave all three
    // blank. Heading three empty columns just labels dead space.
    //
    // Scoped to the VISIBLE page, not the whole directory: replay/netplay holds
    // 44 folders and 4 loose files, so a directory-wide test kept the headings
    // up over pages that are entirely folders. Because folders sort ahead of
    // files, this means the headings appear exactly when the first row that has
    // those values scrolls into view.
    bool anyReplayRow = false;
    for (int i = 0; i < kRowsShown; ++i) {
        const int index = s_browserScroll + i;
        if (index >= totalEntries) {
            break;
        }
        if (s_browserEntries[index].type == ReplayBrowserEntryType::ReplayFile) {
            anyReplayRow = true;
            break;
        }
    }

    if (anyReplayRow) {
        GameDrawTextAt(kMatchX, kFirstRow - 20, kRepInkFaint, kRepInkFaint,
                       kRepInkFaint, kRepNoteSize, "Matchup");
        GameDrawTextAt(kWhenX, kFirstRow - 20, kRepInkFaint, kRepInkFaint,
                       kRepInkFaint, kRepNoteSize, "Date");
        GameDrawTextAt(kLenX, kFirstRow - 20, kRepInkFaint, kRepInkFaint,
                       kRepInkFaint, kRepNoteSize, "Length");
    }

    if (totalEntries == 0) {
        GameDrawTextAt(kTextX, kFirstRow + 6, kRepInkDim, kRepInkDim, kRepInkDim,
                       kRepBodySize, "No replays here yet.");
    }

    // Rows.
    for (int i = 0; i < shown; ++i) {
        const int index = s_browserScroll + i;
        const ReplayBrowserEntry& entry = s_browserEntries[index];
        const int rowTop = kFirstRow + i * kRowPitch;
        const bool isSelected = index == s_browserSelected;

        if (isSelected) {
            GameSetBlend(2, 128);
            GameFillRect(kTextX, rowTop, kBarRight, rowTop + kRowPitch - 2, 255, 0, 0);
            GameSetBlend(1, 255);
        }

        const bool isFolder = entry.type != ReplayBrowserEntryType::ReplayFile;
        // A netplay filename repeats what the other three columns already show -
        // timestamp, characters, date - so the Name column carries the one thing
        // they do not: who played. The raw stem is kept for anything that cannot
        // be read that way, which is every replay the game saved itself.
        std::string shown = entry.display_name;
        if (isFolder) {
            // Set folders end in " - YYYY-MM-DD_HH-MM-SS". At a consistent font
            // the row fits 56 characters and the older character-laden names run
            // to 63, so the clock time is dropped here - it is the least useful
            // part of a folder label, and the detail line under the list still
            // shows the name in full.
            const size_t stamp = shown.rfind('_');
            if (stamp != std::string::npos && shown.size() - stamp == 9 &&
                shown.find(" - ") != std::string::npos) {
                shown.resize(stamp);
            }
        }
        if (!isFolder) {
            std::string p1;
            std::string p2;
            if (SplitNetplayReplayNames(entry.full_path.stem().string(), entry.metadata,
                                        &p1, &p2)) {
                shown = p1 + " vs " + p2;
            }
        }
        // 20 chars is what clears the Matchup column - but only while that
        // column is on screen. On a page with no replay rows the other three
        // columns are not drawn, so the name owns the row and clipping it there
        // was cropping for a neighbour that is not there.
        //
        // The wide figure is derived from the selection bar rather than picked:
        // the bar runs kTextX..kBarRight and the text starts 8px inside it, so
        // mirroring that 8px on the right is the whole usable width. Shippori
        // advances a little under half its size on this mixed-case text, which
        // is what the 0.48 is.
        //
        // Every row draws at kRepBodySize. Shrinking folder rows to buy characters
        // was worse than the clipping it fixed: the atlas is baked at 19px, so
        // 17 resamples down while 20 resamples up, and the two read as different
        // typefaces on the same screen.
        constexpr int kNameSpanPx = (kBarRight - 8) - (kTextX + 8);
        constexpr size_t kNameCharsWide =
            (size_t)((float)kNameSpanPx / (kRepBodySize * 0.48f));

        // Per ROW, not per page. A folder fills none of Matchup/Date/Length, so
        // its name runs the full width underneath them even when file rows on
        // the same page still need those columns kept clear. Only a row that
        // actually prints into them has to stop short.
        const size_t nameChars = isFolder ? kNameCharsWide : 20u;
        char name[128] = {};
        ClipGameText(name, sizeof(name),
                     (isFolder ? ("[ " + entry.display_name + " ]") : shown).c_str(),
                     nameChars);
        const uint8_t ink = isSelected ? kRepInkBright : kRepInk;
        GameDrawTextAt(kTextX + 8, rowTop + 6, ink, ink, ink, kRepBodySize, name);

        // Name alone says very little; the matchup and date are what a player
        // actually picks a replay by.
        if (!isFolder && entry.metadata.valid) {
            const std::string matchup = GetBrowserEntryMatchupText(entry);
            if (!matchup.empty()) {
                char clipped[64] = {};
                // 18 fits "Aria / Escalayer"; the column was 15 wide, which cut
                // the second character's name off mid-word on the longer pairs.
                ClipGameText(clipped, sizeof(clipped), matchup.c_str(), 18);
                GameDrawTextAt(kMatchX, rowTop + 8, kRepInkDim, kRepInkDim,
                               kRepInkDim, kRepNoteSize, clipped);
            }
            if (!entry.metadata.modified_time.empty()) {
                // FormatLastWriteTime gives "YYYY-MM-DD HH:MM:SS"; the seconds
                // are the only part worth dropping, and dropping them is what
                // makes the rest fit whole instead of being cut mid-date.
                std::string when = entry.metadata.modified_time;
                if (when.size() > 16) {
                    when.resize(16);
                }
                GameDrawTextAt(kWhenX, rowTop + 8, kRepInkFaint, kRepInkFaint,
                               kRepInkFaint, kRepNoteSize, when.c_str());
            }
        }

        // Folders have no duration, matchup or date - the other two columns
        // already skip them, and putting the word "Folder" under a Time heading
        // read as data rather than as a type. The bracketed name marks them, and
        // the detail line under the list says so in full.
        if (!isFolder) {
            const std::string value = GetBrowserEntryValueText(entry);
            if (!value.empty()) {
                GameDrawTextAt(kLenX, rowTop + 8, kRepInkDim, kRepInkDim, kRepInkDim,
                               kRepNoteSize, value.c_str());
            }
        }
    }

    // Details for whatever is highlighted, on one quiet block under the list.
    if (selected) {
        GameSetBlend(1, 70);
        GameFillRect(kTextX, detailTop - 4, kBarRight, detailTop - 3, 90, 90, 90);
        GameSetBlend(1, 255);

        if (selected->type == ReplayBrowserEntryType::ReplayFile &&
            selected->metadata.valid) {
            const std::string matchup = GetBrowserEntryDetailLine(*selected);
            char line[192] = {};
            ClipGameText(line, sizeof(line), matchup.c_str(), 44);
            GameDrawTextAt(kTextX + 8, detailTop + 4, kRepInk, kRepInk, kRepInk,
                           kRepNoteSize, line);

            char meta[128] = {};
            snprintf(meta, sizeof(meta), "%s   %s",
                     GetBrowserEntryValueText(*selected).c_str(),
                     selected->metadata.modified_time.c_str());
            char clippedMeta[128] = {};
            ClipGameText(clippedMeta, sizeof(clippedMeta), meta, 44);
            GameDrawTextAt(kTextX + 8, detailTop + 24, kRepInkFaint, kRepInkFaint,
                           kRepInkFaint, kRepNoteSize, clippedMeta);
        } else if (selected->type == ReplayBrowserEntryType::Directory) {
            // The row drops the clock time to fit; this is where it comes back.
            char full[192] = {};
            ClipGameText(full, sizeof(full), selected->display_name.c_str(), 60);
            GameDrawTextAt(kTextX + 8, detailTop + 4, kRepInk, kRepInk, kRepInk,
                           kRepNoteSize, full);
            GameDrawTextAt(kTextX + 8, detailTop + 24, kRepInkFaint, kRepInkFaint,
                           kRepInkFaint, kRepNoteSize, "Folder");
        } else {
            GameDrawTextAt(kTextX + 8, detailTop + 4, kRepInkDim, kRepInkDim, kRepInkDim,
                           kRepNoteSize,
                           selected->type == ReplayBrowserEntryType::ParentDirectory
                               ? "Go up one folder" : "Folder");
        }
    }

    GameDrawTextAt(kTextX, footerTop, kRepInkFaint, kRepInkFaint, kRepInkFaint,
                   kRepNoteSize, "A Open   B Back   Up/Down Move   Left/Right Page");

    GameSetBlend(0, 255);
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
    ResetConfirmedRecorder("runtime init");
    ResetNetplayReplaySetFolder("runtime init");
    ResetBrowserState();
    ResetMatchHotkeyEdges();
    ResetMenuHotkeyEdges();
    s_initialized = true;
    LOG_INFO("[Replay] Runtime initialized (0 replay menu, Bksl pause, ] step, [ back, Shift+[ rewind, Insert HUD, 1/2 takeover)");
}

void ReplayRuntime_Shutdown() {
    if (!s_initialized) {
        return;
    }

    DeactivateReplayMatch("shutdown");
    ResetLoadedReplayPaletteState();
    ResetConfirmedRecorder("runtime shutdown");
    ResetNetplayReplaySetFolder("runtime shutdown");
    ResetBrowserState();
    s_initialized = false;
    LOG_INFO("[Replay] Runtime shutdown");
}

void ReplayRuntime_SetHudVisible(bool visible) {
    if (s_replayHudVisible == visible) {
        return;
    }
    s_replayHudVisible = visible;
    LOG_INFO("[Replay] HUD %s", visible ? "shown" : "hidden");
}

bool ReplayRuntime_IsHudVisible() {
    return s_replayHudVisible;
}

void ReplayRuntime_OnNetplaySessionEnd(const char* reason) {
    ResetNetplayReplaySetFolder(reason ? reason : "session end");
}

void ReplayRuntime_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    // A set folder is exactly one connected session, so it must not survive the
    // session that opened it. OnlineWiring_OnDisconnect closes it; this closes
    // it too, in case the session ends by a route that does not run that hook.
    // Without it, reconnecting to the same opponent would append the next set
    // into the previous set's folder. Only sampled while a folder is open.
    if (!s_netplaySetFolder.empty()) {
        Net::SessionSnapshot setSession{};
        Net::Session_GetSnapshot(&setSession);
        if (!setSession.active) {
            ResetNetplayReplaySetFolder("session no longer active");
        }
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
    if (HandleMatchHotkeys()) {
        return;
    }
    ApplySpeedScale();
    ClearInjectedInputsOnly();
}

void ReplayRuntime_RenderHUD() {
    if (!s_initialized) {
        return;
    }

    RenderReplayMatchHud();
}

bool ReplayRuntime_HasVisibleHud() {
    return s_initialized && s_replayMatchActive && s_replayHudVisible;
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

// M7 (S-5): pre-tick verification of the native tape against the loaded
// confirmed stream. Input words are compared under the tape's 8-button mask
// (the native tape stores one byte per player). Digests use the same
// sync-acquire model as the spectator playback (the replay boots through the
// native loader, so the first records may not be bit-aligned with the
// recorder baseline); a replay is an offline artifact, so divergence
// fail-DEGRADES with loud logs — nothing to tear down.
static void VerifyConfirmedPlayback(int32_t dispatchedFrame,
                                    const PendingPreparedInputs& prepared) {
    if (!s_loadedConfirmed.present || s_verifyDisabled) {
        return;
    }
    if (s_takeoverMode != TakeoverMode::None) {
        // Takeover intentionally diverges from the recorded stream.
        s_verifyDisabled = true;
        LOG_INFO("[Replay] Confirmed-stream verification disabled (takeover armed)");
        return;
    }

    const int32_t index = dispatchedFrame - s_loadedConfirmed.first_game_abs;
    if (index < 0 || (size_t)index >= s_loadedConfirmed.records.size()) {
        return;
    }

    const ConfirmedInputRecord& rec = s_loadedConfirmed.records[(size_t)index];
    if (((rec.p1 ^ prepared.p1) & kReplayInputMask) != 0 ||
        ((rec.p2 ^ prepared.p2) & kReplayInputMask) != 0) {
        ++s_verifyInputMismatches;
        if (s_verifyInputMismatches <= 5 || (s_verifyInputMismatches % 300) == 0) {
            LOG_WARN("[Replay] Confirmed-stream INPUT mismatch at frame %d (idx=%d): "
                     "tape P1=0x%02X P2=0x%02X confirmed P1=0x%04X P2=0x%04X count=%u",
                dispatchedFrame, index,
                prepared.p1 & kReplayInputMask, prepared.p2 & kReplayInputMask,
                rec.p1, rec.p2, s_verifyInputMismatches);
        }
    }

    const auto hashIt = s_loadedConfirmed.hashes.find(index);
    if (hashIt == s_loadedConfirmed.hashes.end()) {
        return;
    }

    uint64_t liveHash = 0;
    if (!Rollback::GameSnapshot_HashGameplayLive(&liveHash)) {
        return;
    }
    ++s_verifyHashChecks;

    if (liveHash == hashIt->second) {
        if (!s_verifyAcquired) {
            s_verifyAcquired = true;
            LOG_INFO("[Replay] Confirmed-stream hash sync ACQUIRED at frame %d (idx=%d, checks=%u)",
                dispatchedFrame, index, s_verifyHashChecks);
        }
        return;
    }

    if (!s_verifyAcquired) {
        ++s_verifyEntryMismatches;
        if (s_verifyEntryMismatches >= kReplayVerifyAcquireWindowHashes) {
            s_verifyDisabled = true;
            LOG_WARN("[Replay] Confirmed-stream hash never acquired after %u cadence points — "
                     "verification disabled for this playback (fail-degrade)",
                s_verifyEntryMismatches);
        }
        return;
    }

    ++s_verifyHashMismatches;
    LOG_WARN("[Replay] REPLAY DESYNC: confirmed pre-state hash mismatch at frame %d "
             "(idx=%d recorded=0x%016llX live=0x%016llX mismatches=%u)",
        dispatchedFrame, index,
        (unsigned long long)hashIt->second,
        (unsigned long long)liveHash,
        s_verifyHashMismatches);
    Rollback::NetplayLog_Write("REPLAY", dispatchedFrame,
        "REPLAY DESYNC at idx=%d recorded=0x%016llX live=0x%016llX",
        index,
        (unsigned long long)hashIt->second,
        (unsigned long long)liveHash);
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

    VerifyConfirmedPlayback(dispatchedFrame, prepared);

    outputInputs[0] = static_cast<int16_t>(prepared.p1);
    outputInputs[1] = static_cast<int16_t>(prepared.p2);

    WriteReplayHistoryFrame(dispatchedFrame, prepared.p1, prepared.p2);
    InputSystem_SetNetplayInput(0, prepared.p1);
    InputSystem_SetNetplayInput(1, prepared.p2);
    s_pendingPreparedInputs = prepared;
}

void ReplayRuntime_OnConfirmedFrame(uint32_t epoch,
                                    int32_t game_abs_frame,
                                    uint16_t p1_input,
                                    uint16_t p2_input,
                                    uint64_t pre_state_hash) {
    if (!s_initialized || game_abs_frame < 0) {
        return;
    }

    // Epoch boundary = chapter boundary (S-5): the recorder holds exactly
    // one match's confirmed stream; the native one-file-per-match save
    // consumes it, and a new epoch discards whatever was not saved.
    if (epoch != s_recEpoch) {
        ResetConfirmedRecorder("epoch rotation");
        s_recEpoch = epoch;
    }
    if (s_recFirstGameAbs < 0) {
        s_recFirstGameAbs = game_abs_frame;
    }

    const int64_t expectedIndex = (int64_t)s_recRecords.size();
    const int64_t index = (int64_t)game_abs_frame - s_recFirstGameAbs;
    if (index != expectedIndex) {
        // The confirm seam pops in order, so this is defensive only.
        ++s_recDropped;
        if (!s_recContiguityBroken) {
            s_recContiguityBroken = true;
            LOG_WARN("[Replay] Confirmed recorder contiguity break: expected idx=%lld got %lld (abs=%d)",
                (long long)expectedIndex, (long long)index, game_abs_frame);
        }
        return;
    }
    if (s_recRecords.size() >= (size_t)INPUT_HISTORY_MAX) {
        ++s_recDropped;
        return;
    }

    ConfirmedInputRecord rec{};
    rec.p1 = p1_input;
    rec.p2 = p2_input;
    s_recRecords.push_back(rec);

    if ((s_recRecords.size() - 1) % kReplayConfirmedHashCadence == 0) {
        ConfirmedHashRecord hashRecord{};
        hashRecord.index = (uint32_t)(s_recRecords.size() - 1);
        hashRecord.hash = pre_state_hash;
        s_recHashes.push_back(hashRecord);
    }
}

} // namespace Replay
