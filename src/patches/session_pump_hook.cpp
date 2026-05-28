#include "patches/session_pump_hook.h"

#include "core/game_state.h"
#include "net/match_lifecycle.h"
#include "net/session_manager.h"
#include "rollback/netplay_log.h"

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

constexpr DWORD kMinPumpIntervalMs = 4;

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
    const bool isWinScreenContext = IsWinScreenArchive(archive);

    MaybePumpSession("before", isWinScreenContext, assetIndex, patchIndex);

    const int result = g_origAssetLoadFromArchive
        ? g_origAssetLoadFromArchive(archive, patch, assetIndex, patchIndex)
        : 0;

    MaybePumpSession("after", isWinScreenContext, assetIndex, patchIndex);
    return result;
}

} // namespace Net
