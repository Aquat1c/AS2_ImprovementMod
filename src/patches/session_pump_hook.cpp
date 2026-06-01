#include "patches/session_pump_hook.h"

#include "core/game_state.h"
#include "net/match_lifecycle.h"
#include "net/session_manager.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

namespace Net {

AssetLoadFromArchive_t g_origAssetLoadFromArchive = nullptr;

namespace {

static bool s_initialized = false;
static bool s_pumping = false;
static DWORD s_lastPumpTick = 0;
static uint32_t s_assetCallCount = 0;
static uint32_t s_pumpCount = 0;
static uint32_t s_skippedTooSoon = 0;
static uint32_t s_startupAssetTraceCount = 0;
static bool s_startupTraceReachedTitle = false;
static DWORD s_startupTraceBeginTick = 0;

constexpr DWORD kMinPumpIntervalMs = 4;
constexpr uint32_t kMaxStartupAssetTraceLines = 512;

static bool IsWinScreenArchive(const char* archive) {
    return archive &&
           (strstr(archive, "win") != nullptr ||
            strstr(archive, "WIN") != nullptr);
}

static bool IsPumpableLifecycle(MatchLifecyclePhase phase) {
    return phase == MatchLifecyclePhase::MatchEnd ||
           phase == MatchLifecyclePhase::WinScreenActive ||
           phase == MatchLifecyclePhase::PostMatchRoute ||
           phase == MatchLifecyclePhase::ReturningToCharSel ||
           phase == MatchLifecyclePhase::ReturningToMenu;
}

static bool ContainsInsensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !needle[0]) {
        return false;
    }

    const size_t needleLen = strlen(needle);
    if (needleLen == 0) {
        return false;
    }

    for (const char* p = haystack; *p; ++p) {
        size_t i = 0;
        while (p[i] && i < needleLen &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needleLen) {
            return true;
        }
    }

    return false;
}

static void TraceStartupAssetLoad(const char* archive,
                                  const char* patch,
                                  int assetIndex,
                                  int patchIndex) {
    if (s_startupTraceReachedTitle || s_startupAssetTraceCount >= kMaxStartupAssetTraceLines) {
        return;
    }

    const bool titleHit =
        ContainsInsensitive(archive, "tit.bin") ||
        ContainsInsensitive(patch, "tit.bin");
    const DWORD elapsed = GetTickCount() - s_startupTraceBeginTick;

    ++s_startupAssetTraceCount;
    LOG_INFO("[STARTUPTRACE][AssetLoad] #%u +%lums archive='%s' patch='%s' asset=%d patch_idx=%d mode=%u sub=%u phase=%s title_hit=%d",
             s_startupAssetTraceCount,
             (unsigned long)elapsed,
             archive ? archive : "",
             patch ? patch : "",
             assetIndex,
             patchIndex,
             (unsigned)GetGameMode(),
             (unsigned)GetSubstate(),
             MatchLifecyclePhaseName(MatchLifecycle_GetPhase()),
             titleHit ? 1 : 0);

    if (titleHit) {
        s_startupTraceReachedTitle = true;
        LOG_INFO("[STARTUPTRACE] Reached title marker via Asset_LoadFromArchive at +%lums after %u loads",
                 (unsigned long)elapsed,
                 s_startupAssetTraceCount);
    }
}

static void MaybePumpSession(const char* stage,
                             bool isWinScreenContext,
                             int assetIndex,
                             int patchIndex) {
    if (!s_initialized || s_pumping || !Session_IsConnected()) {
        return;
    }

    const MatchLifecyclePhase phase = MatchLifecycle_GetPhase();
    const uint32_t mode = GetGameMode();
    if (!IsPumpableLifecycle(phase) && mode != MODE_WINSCREEN) {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD elapsed = s_lastPumpTick != 0 ? (now - s_lastPumpTick) : 0;
    if (s_lastPumpTick != 0 && elapsed < kMinPumpIntervalMs) {
        ++s_skippedTooSoon;
        return;
    }

    s_lastPumpTick = now;
    ++s_pumpCount;

    s_pumping = true;
    Session_Update();
    s_pumping = false;

    if (s_pumpCount <= 5 || (s_pumpCount % 10) == 0) {
        Rollback::NetplayLog_Write(
            "STALL_PUMP", -1,
            "Asset load session pump #%u call#%u stage=%s winscreen=%d elapsed=%lums "
            "mode=%u sub=%u phase=%s asset=%d patch=%d skipped_soon=%u",
            s_pumpCount,
            s_assetCallCount,
            stage ? stage : "?",
            isWinScreenContext ? 1 : 0,
            (unsigned long)elapsed,
            mode,
            GetSubstate(),
            MatchLifecyclePhaseName(phase),
            assetIndex,
            patchIndex,
            s_skippedTooSoon);
    }
}

} // namespace

void SessionPumpHook_Init() {
    s_initialized = true;
    s_pumping = false;
    s_lastPumpTick = 0;
    s_assetCallCount = 0;
    s_pumpCount = 0;
    s_skippedTooSoon = 0;
    s_startupAssetTraceCount = 0;
    s_startupTraceReachedTitle = false;
    s_startupTraceBeginTick = GetTickCount();
    LOG_INFO("[STARTUPTRACE] SessionPumpHook init (tracking Asset_LoadFromArchive until tit.bin)");
}

void SessionPumpHook_Shutdown() {
    s_initialized = false;
    s_pumping = false;
}

int __cdecl Hook_Asset_LoadFromArchive(const char* archive,
                                        char* patch,
                                        int assetIndex,
                                        int patchIndex) {
    ++s_assetCallCount;
    TraceStartupAssetLoad(archive, patch, assetIndex, patchIndex);
    const bool isWinScreenContext = IsWinScreenArchive(archive);

    MaybePumpSession("before", isWinScreenContext, assetIndex, patchIndex);

    const int result = g_origAssetLoadFromArchive
        ? g_origAssetLoadFromArchive(archive, patch, assetIndex, patchIndex)
        : 0;

    MaybePumpSession("after", isWinScreenContext, assetIndex, patchIndex);
    return result;
}

} // namespace Net
