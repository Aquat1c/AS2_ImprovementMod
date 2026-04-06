/**
 * Alice Senki 2 - Mod Main Module
 * 
 * Integrates:
 * - SDL3 Input System (keyboard + gamepad)
 * - ImGui UI (unified menu)
 * - Log Window
 *
 * NOTE: Rollback-specific includes and calls have been stripped.
 * Rollback will be reimplemented from scratch in new modules.
 */

#include "as2_rollback.h"
#include "input_system.h"
#include "log_window.h"
#include "mod_menu.h"
#include "game_console.h"
#include "patches/memory_utils.h"
#include "patches/unlock_patch.h"
#include "patches/locale_patch.h"
#include "patches/filesystem_patch.h"
#include "MinHook.h"
#include "imgui.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <mmsystem.h>

// CRC32 util is used for desync detection.


// ============================================================================
// Configuration
// ============================================================================

struct ModConfig {
    bool useSDLInput;          // Use SDL3 instead of game's input
    bool bypassGameInput;      // Completely bypass game input (for netplay)
    int inputDisplayMode;      // 0=off, 1=simple, 2=detailed
    bool showHitboxes;         // Show hitbox overlay
    bool showFrameData;        // Show frame advantage etc
    bool verboseLogging;       // Enable verbose debug logging
    int rollbackFrames;        // Max rollback frames
    int inputDelay;            // Frames of input delay
};

static ModConfig g_config = {
    true,   // useSDLInput
    false,  // bypassGameInput - Disabled - causing crash when opening pause menu
    1,      // inputDisplayMode
    false,  // showHitboxes
    false,  // showFrameData
    false,  // verboseLogging - Disabled by default to reduce log spam
    8,      // rollbackFrames
    0       // inputDelay
};

// ============================================================================
// Rollback Synchronized Input Override (fed by InputSyncHooks)
// ============================================================================

static volatile LONG g_syncInputOverrideActive = 0;
static volatile uint16_t g_syncInputOverrideP1 = 0;
static volatile uint16_t g_syncInputOverrideP2 = 0;

// ============================================================================
// Visual Frame Counter (incremented once per EndScene/Present)
// ============================================================================
// This is used to ensure only 1 game tick runs per rendered frame,
    // even if a future netplay/rollback implementation produces multiple advance events.
// 
// g_visualFrameCounter: Global counter, never resets (for internal logic)
// g_matchVisualFrame: Match-specific counter, resets to 0 at match start (for debugging)
static volatile uint32_t g_visualFrameCounter = 0;
static volatile uint32_t g_matchVisualFrame = 0;
static volatile bool g_inMatch = false;

uint32_t AS2_GetVisualFrameCounter(void) {
    return g_visualFrameCounter;
}

uint32_t AS2_GetMatchVisualFrame(void) {
    return g_matchVisualFrame;
}

void AS2_IncrementVisualFrameCounter(void) {
    InterlockedIncrement(&g_visualFrameCounter);
    if (g_inMatch) {
        InterlockedIncrement(&g_matchVisualFrame);
    }
}

void AS2_ResetMatchVisualFrame(void) {
    g_matchVisualFrame = 0;
    g_inMatch = true;
    LOG_INFO("[VisualFrame] Match started - reset match visual frame counter to 0");
}

void AS2_EndMatch(void) {
    g_inMatch = false;
    LOG_INFO("[VisualFrame] Match ended at visual frame %u", g_matchVisualFrame);
}

// ============================================================================
// Match Win Tracking
// ============================================================================

static int s_p1_wins = 0;  // Host wins
static int s_p2_wins = 0;  // Joiner wins
static int s_lastMatchWinner = -1;  // -1=no match yet, 0=P1(host), 1=P2(joiner)

void AS2_RecordMatchResult(void) {
    // Winner byte at match_base + 4: 0=P1 win, 1=P2 win, 2=draw, 0xFF=no result
    uint8_t winner = *reinterpret_cast<volatile uint8_t*>(ADDR_MATCH_BASE + 4);
    if (winner == 0) {
        s_p1_wins++;
        s_lastMatchWinner = 0;
        LOG_INFO("[WinTrack] P1 (host) wins! Score: %d-%d", s_p1_wins, s_p2_wins);
    } else if (winner == 1) {
        s_p2_wins++;
        s_lastMatchWinner = 1;
        LOG_INFO("[WinTrack] P2 (joiner) wins! Score: %d-%d", s_p1_wins, s_p2_wins);
    } else if (winner == 2) {
        LOG_INFO("[WinTrack] Draw! Score unchanged: %d-%d (stage nav unchanged)", s_p1_wins, s_p2_wins);
    } else {
        LOG_INFO("[WinTrack] No winner recorded (byte=0x%02X)", winner);
    }
}

void AS2_ResetMatchWins(void) {
    s_p1_wins = 0;
    s_p2_wins = 0;
    s_lastMatchWinner = -1;
    LOG_INFO("[WinTrack] Win counts reset");
}

void AS2_GetMatchWins(int* outP1, int* outP2) {
    if (outP1) *outP1 = s_p1_wins;
    if (outP2) *outP2 = s_p2_wins;
}

int AS2_GetLastMatchWinner(void) {
    return s_lastMatchWinner;
}

// ============================================================================
// Tick Scaling / Time Warp (for rollback catch-up)
// ============================================================================

typedef DWORD (__cdecl* GetTick_t)();
static GetTick_t g_origGetTick = nullptr;

static volatile float g_globalTickScale = 1.0f;
static volatile float g_rollbackTickScale = 8.0f;

static DWORD g_timeWarpBaseReal = 0;
static DWORD g_timeWarpBaseFake = 0;
static float g_timeWarpLastScale = 1.0f;
static float g_lastLoggedEffectiveScale = 1.0f;
static bool g_lastLoggedResimulating = false;
static DWORD g_lastTickLogReal = 0;

// Tick baseline synchronization for rollback netplay
static uint32_t g_syncedTickBaseline = 0;
static bool g_tickBaselineSynced = false;

static DWORD __cdecl Hook_GetTick() {
    DWORD realRaw = g_origGetTick ? g_origGetTick() : (GetTickCount() & 0x7FFFFFFF);
    
    // Apply synchronized baseline if active (for rollback netplay)
    if (g_tickBaselineSynced) {
        realRaw = ((realRaw - g_syncedTickBaseline) & 0x7FFFFFFF);
    }
    
    const DWORD real = (realRaw & 0x7FFFFFFF);

    float effectiveScale = g_globalTickScale;
    if (effectiveScale <= 1.0f) {
        // Clean-slate: rollback removed, no resimulation speedup.
    }

    if (effectiveScale <= 1.0f) {
        g_timeWarpLastScale = 1.0f;

        if (GetVerboseLogging()) {
            const bool resim = false;
            const DWORD since = (real - g_lastTickLogReal) & 0x7FFFFFFF;
            if (g_lastLoggedEffectiveScale != 1.0f || (resim != g_lastLoggedResimulating) || since > 1000) {
                g_lastLoggedEffectiveScale = 1.0f;
                g_lastLoggedResimulating = resim;
                g_lastTickLogReal = real;
                LOG_DEBUG("[Timing] tick_scale=1.00x (resim=%d)", resim ? 1 : 0);
            }
        }

        return real;
    }

    if (g_timeWarpLastScale != effectiveScale) {
        g_timeWarpBaseReal = real;
        g_timeWarpBaseFake = real;
        g_timeWarpLastScale = effectiveScale;

        if (GetVerboseLogging()) {
            const bool resim = false;
            const DWORD since = (real - g_lastTickLogReal) & 0x7FFFFFFF;
            if (g_lastLoggedEffectiveScale != effectiveScale || (resim != g_lastLoggedResimulating) || since > 250) {
                g_lastLoggedEffectiveScale = effectiveScale;
                g_lastLoggedResimulating = resim;
                g_lastTickLogReal = real;
                LOG_INFO("[Timing] tick_scale=%.2fx (global=%.2fx rollback=%.2fx resim=%d)",
                         effectiveScale, g_globalTickScale, g_rollbackTickScale, resim ? 1 : 0);
            }
        }

        return real;
    }

    const DWORD baseReal = (g_timeWarpBaseReal & 0x7FFFFFFF);
    const DWORD baseFake = (g_timeWarpBaseFake & 0x7FFFFFFF);
    const DWORD delta = (real - baseReal) & 0x7FFFFFFF;

    const double scaledDelta = (double)delta * (double)effectiveScale;
    const uint64_t fake64 = (uint64_t)baseFake + (uint64_t)(scaledDelta + 0.5);
    const DWORD fake = (DWORD)(fake64 & 0x7FFFFFFF);

    if (GetVerboseLogging()) {
        const DWORD since = (real - g_lastTickLogReal) & 0x7FFFFFFF;
        if (since > 2000) {
            g_lastTickLogReal = real;
            LOG_DEBUG("[Timing] tick(real=%u fake=%u scale=%.2fx)", real, fake, effectiveScale);
        }
    }

    return fake;
}

extern "C" void AS2_SetSynchronizedInputOverride(uint16_t p1Input, uint16_t p2Input) {
    g_syncInputOverrideP1 = p1Input;
    g_syncInputOverrideP2 = p2Input;
    InterlockedExchange(&g_syncInputOverrideActive, 1);
}

extern "C" void AS2_ClearSynchronizedInputOverride(void) {
    InterlockedExchange(&g_syncInputOverrideActive, 0);
}

extern "C" bool AS2_IsSynchronizedInputOverrideActive(void) {
    return InterlockedCompareExchange(&g_syncInputOverrideActive, 0, 0) != 0;
}

extern "C" void AS2_SetGlobalTickScale(float scale) {
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 32.0f) scale = 32.0f;

    const float prev = g_globalTickScale;
    g_globalTickScale = scale;
    g_timeWarpLastScale = 0.0f;
    if (GetVerboseLogging() && prev != scale) {
        LOG_INFO("[Timing] GlobalTickScale %.2fx -> %.2fx", prev, scale);
    }
}

extern "C" float AS2_GetGlobalTickScale(void) {
    return g_globalTickScale;
}

extern "C" void AS2_SetRollbackTickScale(float scale) {
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 32.0f) scale = 32.0f;

    const float prev = g_rollbackTickScale;
    g_rollbackTickScale = scale;
    g_timeWarpLastScale = 0.0f;
    if (GetVerboseLogging() && prev != scale) {
        LOG_INFO("[Timing] RollbackTickScale %.2fx -> %.2fx", prev, scale);
    }
}

extern "C" float AS2_GetRollbackTickScale(void) {
    return g_rollbackTickScale;
}

// ============================================================================
// Forward Declarations
// ============================================================================

static void WriteJoystickInputDirect(int joyIndex, uint16_t input);
static void EnsureInputUpdated();
static void DeferredInit();

// ============================================================================
// Globals
// ============================================================================

static HMODULE g_gameModule = nullptr;
static bool g_initialized = false;

// Original function pointers
static KeyboardState_t g_origKeyboardState = nullptr;
static JoystickState_t g_origJoystickState = nullptr;

// Input processing hook
typedef int (__cdecl *InputProcess_t)(int gameState);
static InputProcess_t g_origInputProcess = nullptr;

// DirectInput buffer refresh hook function pointers
typedef int (__cdecl *DInputKBRefresh_t)();
typedef int (__cdecl *DInputJoyRefresh_t)(int joyID);
static DInputKBRefresh_t g_origDInputKBRefresh = nullptr;
static DInputJoyRefresh_t g_origDInputJoyRefresh = nullptr;

// Win32 GetKeyboardState hook - to prevent Alt+Shift Windows language switching issues
typedef BOOL (WINAPI *GetKeyboardState_t)(PBYTE lpKeyState);
static GetKeyboardState_t g_origGetKeyboardState = nullptr;

// ChangeDisplaySettings hook to prevent exclusive fullscreen
typedef LONG (WINAPI *ChangeDisplaySettingsA_t)(DEVMODEA* lpDevMode, DWORD dwFlags);
static ChangeDisplaySettingsA_t g_origChangeDisplaySettings = nullptr;
static bool g_forceBorderlessFullscreen = false;  // Disabled - handled by d3d9_proxy

// RNG hooks for deterministic rollback
typedef void (__cdecl *srand_t)(unsigned int seed);
typedef int (__cdecl *rand_t)();
static srand_t g_orig_srand = nullptr;
static rand_t g_orig_rand = nullptr;
static uint32_t g_currentRngSeed = 1;  // MSVC CRT default seed is 1
static uint32_t g_visualRngSeed = 1;   // Visual-only RNG stream (not saved/restored)
static bool g_rngHooksInstalled = false;

// Savestate storage - unified on CompactSaveState_t for all slots
#define SAVESTATE_SLOT_COUNT 16
static CompactSaveState_t g_saveStates[SAVESTATE_SLOT_COUNT];
static bool g_stateValid[SAVESTATE_SLOT_COUNT] = {false};
static int g_rollbackIndex = 0;

// Netplay state
static bool g_netplayActive = false;
static int g_localPlayerID = 0;

// ============================================================================
// Rollback Trace Logging
// ============================================================================

namespace {

static bool g_rollbackTraceLogging = false;
static FILE* g_rollbackTraceTextFile = nullptr;
static FILE* g_rollbackTraceBinaryFile = nullptr;
static char g_rollbackTraceTextPath[MAX_PATH] = {};
static char g_rollbackTraceBinaryPath[MAX_PATH] = {};
static volatile LONG g_rollbackTraceEventCounter = 0;
static uint64_t g_rollbackTraceSequence = 0;
static LARGE_INTEGER g_rollbackTraceQpcFrequency = {};

static constexpr uint32_t kRollbackTraceRecordMagic = 0x31525442;  // "BTR1"
static constexpr uint32_t kRollbackTraceRecordVersion = 1;
static constexpr uint32_t kRollbackTraceRecordBytes = 1;

#pragma pack(push, 1)
struct RollbackTraceRecordHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t kind;
    uint32_t event_id;
    int32_t frame;
    uint64_t sequence;
    uint32_t address;
    uint32_t before_size;
    uint32_t after_size;
    uint32_t before_crc;
    uint32_t after_crc;
    char scope[32];
    char name[64];
};
#pragma pack(pop)

static void RollbackTraceEnsureTimer() {
    if (g_rollbackTraceQpcFrequency.QuadPart == 0) {
        QueryPerformanceFrequency(&g_rollbackTraceQpcFrequency);
    }
}

static double RollbackTraceElapsedUs(LARGE_INTEGER start, LARGE_INTEGER end) {
    RollbackTraceEnsureTimer();
    if (g_rollbackTraceQpcFrequency.QuadPart == 0) {
        return 0.0;
    }
    const LONGLONG delta = end.QuadPart - start.QuadPart;
    return (double)delta * 1000000.0 / (double)g_rollbackTraceQpcFrequency.QuadPart;
}

static void CloseRollbackTraceFiles() {
    if (g_rollbackTraceTextFile) {
        fflush(g_rollbackTraceTextFile);
        fclose(g_rollbackTraceTextFile);
        g_rollbackTraceTextFile = nullptr;
    }
    if (g_rollbackTraceBinaryFile) {
        fflush(g_rollbackTraceBinaryFile);
        fclose(g_rollbackTraceBinaryFile);
        g_rollbackTraceBinaryFile = nullptr;
    }
}

static bool EnsureRollbackTraceFilesOpen() {
    if (!g_rollbackTraceLogging) {
        return false;
    }

    if (g_rollbackTraceTextFile && g_rollbackTraceBinaryFile) {
        return true;
    }

    const char* logDir = LogWindow_GetLogDir();
    const DWORD pid = GetCurrentProcessId();
    if (logDir && logDir[0]) {
        _snprintf_s(g_rollbackTraceTextPath,
                    sizeof(g_rollbackTraceTextPath),
                    _TRUNCATE,
                    "%s\\as2_rollback_trace_%lu.log",
                    logDir,
                    (unsigned long)pid);
        _snprintf_s(g_rollbackTraceBinaryPath,
                    sizeof(g_rollbackTraceBinaryPath),
                    _TRUNCATE,
                    "%s\\as2_rollback_trace_%lu.bin",
                    logDir,
                    (unsigned long)pid);
    } else {
        _snprintf_s(g_rollbackTraceTextPath,
                    sizeof(g_rollbackTraceTextPath),
                    _TRUNCATE,
                    "as2_rollback_trace_%lu.log",
                    (unsigned long)pid);
        _snprintf_s(g_rollbackTraceBinaryPath,
                    sizeof(g_rollbackTraceBinaryPath),
                    _TRUNCATE,
                    "as2_rollback_trace_%lu.bin",
                    (unsigned long)pid);
    }

    if (!g_rollbackTraceTextFile) {
        fopen_s(&g_rollbackTraceTextFile, g_rollbackTraceTextPath, "w");
    }
    if (!g_rollbackTraceBinaryFile) {
        fopen_s(&g_rollbackTraceBinaryFile, g_rollbackTraceBinaryPath, "wb");
    }

    if (!g_rollbackTraceTextFile || !g_rollbackTraceBinaryFile) {
        CloseRollbackTraceFiles();
        return false;
    }

    RollbackTraceEnsureTimer();

    fprintf(g_rollbackTraceTextFile,
            "# AS2 rollback trace\n# text=%s\n# binary=%s\n",
            g_rollbackTraceTextPath,
            g_rollbackTraceBinaryPath);
    fflush(g_rollbackTraceTextFile);
    return true;
}

static void FormatTracePreview(const void* data, size_t size, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    out[0] = '\0';
    if (!data || size == 0) {
        _snprintf_s(out, outSize, _TRUNCATE, "<empty>");
        return;
    }

    const uint8_t* bytes = (const uint8_t*)data;
    if (size <= 16) {
        size_t pos = 0;
        for (size_t i = 0; i < size && pos + 4 < outSize; i++) {
            pos += (size_t)_snprintf_s(out + pos, outSize - pos, _TRUNCATE, "%02X%s", bytes[i], (i + 1 < size) ? " " : "");
        }
        return;
    }

    char head[96] = {};
    char tail[96] = {};
    size_t headPos = 0;
    size_t tailPos = 0;
    for (size_t i = 0; i < 8 && headPos + 4 < sizeof(head); i++) {
        headPos += (size_t)_snprintf_s(head + headPos, sizeof(head) - headPos, _TRUNCATE, "%02X%s", bytes[i], (i + 1 < 8) ? " " : "");
    }
    for (size_t i = size - 8; i < size && tailPos + 4 < sizeof(tail); i++) {
        tailPos += (size_t)_snprintf_s(tail + tailPos, sizeof(tail) - tailPos, _TRUNCATE, "%02X%s", bytes[i], (i + 1 < size) ? " " : "");
    }
    _snprintf_s(out, outSize, _TRUNCATE, "%s ... %s", head, tail);
}

static void WriteTraceHexDump(FILE* file, const char* label, const void* data, size_t size) {
    if (!file || !label || !data || size == 0) {
        return;
    }
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t offset = 0; offset < size; offset += 16) {
        fprintf(file, "      %s +0x%04zX |", label, offset);
        const size_t lineSize = ((offset + 16) <= size) ? 16 : (size - offset);
        for (size_t i = 0; i < lineSize; i++) {
            fprintf(file, " %02X", bytes[offset + i]);
        }
        fprintf(file, "\n");
    }
}

static bool ShouldDumpTraceHex(size_t beforeSize, size_t afterSize) {
    const size_t maxSize = beforeSize > afterSize ? beforeSize : afterSize;
    return maxSize > 0 && maxSize <= 256;
}

} // namespace

void AS2_SetRollbackTraceLogging(bool enabled) {
    if (g_rollbackTraceLogging == enabled) {
        return;
    }

    g_rollbackTraceLogging = enabled;
    if (!enabled) {
        CloseRollbackTraceFiles();
        return;
    }

    if (EnsureRollbackTraceFilesOpen()) {
        LOG_INFO("[RollbackTrace] Enabled text='%s' binary='%s'",
                 g_rollbackTraceTextPath,
                 g_rollbackTraceBinaryPath);
    } else {
        g_rollbackTraceLogging = false;
        LOG_WARN("[RollbackTrace] Failed to open trace files");
    }
}

bool AS2_GetRollbackTraceLogging() {
    return g_rollbackTraceLogging;
}

const char* AS2_GetRollbackTraceTextPath() {
    return g_rollbackTraceTextPath;
}

const char* AS2_GetRollbackTraceBinaryPath() {
    return g_rollbackTraceBinaryPath;
}

uint32_t AS2_NextRollbackTraceEventId() {
    return (uint32_t)InterlockedIncrement(&g_rollbackTraceEventCounter);
}

void AS2_LogRollbackTraceMessage(const char* scope, int frame, uint32_t eventId, const char* fmt, ...) {
    if (!AS2_GetRollbackTraceLogging() || !EnsureRollbackTraceFilesOpen()) {
        return;
    }

    char message[2048] = {};
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);

    const uint64_t sequence = ++g_rollbackTraceSequence;
    fprintf(g_rollbackTraceTextFile,
            "[%08llu] scope=%s frame=%d event=%u %s\n",
            (unsigned long long)sequence,
            (scope && scope[0]) ? scope : "trace",
            frame,
            eventId,
            message);
    fflush(g_rollbackTraceTextFile);
}

void AS2_DumpRollbackTraceBytes(const char* scope,
                                int frame,
                                uint32_t eventId,
                                const char* name,
                                uintptr_t address,
                                const void* beforeData,
                                size_t beforeSize,
                                const void* afterData,
                                size_t afterSize) {
    if (!AS2_GetRollbackTraceLogging() || !EnsureRollbackTraceFilesOpen()) {
        return;
    }

    RollbackTraceRecordHeader header = {};
    header.magic = kRollbackTraceRecordMagic;
    header.version = kRollbackTraceRecordVersion;
    header.kind = kRollbackTraceRecordBytes;
    header.event_id = eventId;
    header.frame = frame;
    header.sequence = ++g_rollbackTraceSequence;
    header.address = (uint32_t)address;
    header.before_size = (uint32_t)beforeSize;
    header.after_size = (uint32_t)afterSize;
    header.before_crc = beforeData && beforeSize ? CalcCRC32(beforeData, beforeSize) : 0;
    header.after_crc = afterData && afterSize ? CalcCRC32(afterData, afterSize) : 0;
    strncpy_s(header.scope, sizeof(header.scope), (scope && scope[0]) ? scope : "trace", _TRUNCATE);
    strncpy_s(header.name, sizeof(header.name), (name && name[0]) ? name : "bytes", _TRUNCATE);

    fwrite(&header, sizeof(header), 1, g_rollbackTraceBinaryFile);
    if (beforeData && beforeSize) {
        fwrite(beforeData, beforeSize, 1, g_rollbackTraceBinaryFile);
    }
    if (afterData && afterSize) {
        fwrite(afterData, afterSize, 1, g_rollbackTraceBinaryFile);
    }
    fflush(g_rollbackTraceBinaryFile);

    char beforePreview[192] = {};
    char afterPreview[192] = {};
    FormatTracePreview(beforeData, beforeSize, beforePreview, sizeof(beforePreview));
    FormatTracePreview(afterData, afterSize, afterPreview, sizeof(afterPreview));

    fprintf(g_rollbackTraceTextFile,
            "[%08llu] scope=%s frame=%d event=%u patch=%s addr=0x%08X before=%u crc=0x%08X after=%u crc=0x%08X\n",
            (unsigned long long)header.sequence,
            header.scope,
            frame,
            eventId,
            header.name,
            header.address,
            header.before_size,
            header.before_crc,
            header.after_size,
            header.after_crc);
    fprintf(g_rollbackTraceTextFile, "      before-preview: %s\n", beforePreview);
    fprintf(g_rollbackTraceTextFile, "      after-preview:  %s\n", afterPreview);
    if (ShouldDumpTraceHex(beforeSize, afterSize)) {
        if (beforeData && beforeSize) {
            WriteTraceHexDump(g_rollbackTraceTextFile, "before", beforeData, beforeSize);
        }
        if (afterData && afterSize) {
            WriteTraceHexDump(g_rollbackTraceTextFile, "after ", afterData, afterSize);
        }
    }
    fflush(g_rollbackTraceTextFile);
}

void AS2_DumpCompactStateTrace(const char* scope, int frame, uint32_t eventId, const CompactSaveState_t* state) {
    if (!state) {
        return;
    }

    char summary[512] = {};
    if (AS2_FormatCompactStateSummary(state, summary, sizeof(summary))) {
        AS2_LogRollbackTraceMessage(scope, frame, eventId, "state-summary %s", summary);
    }

    const size_t metaSize = offsetof(CompactSaveState_t, global);
    const uint32_t historyStart = (state->input.write_idx >= INPUT_HISTORY_WINDOW)
                                ? (state->input.write_idx - INPUT_HISTORY_WINDOW)
                                : 0;

    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "state_meta", 0, nullptr, 0, state, metaSize);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "global", ADDR_GAME_MODE, nullptr, 0, &state->global, sizeof(state->global));
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p1_entity", GetEntityBase(0), nullptr, 0, state->p1_entity, ENTITY_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p2_entity", GetEntityBase(1), nullptr, 0, state->p2_entity, ENTITY_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "match_context", ADDR_MATCH_CONTEXT, nullptr, 0, state->match_context, MATCH_CONTEXT_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "match", ADDR_MATCH_BASE, nullptr, 0, &state->match, sizeof(state->match));
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "pre_match_gap", ADDR_PRE_MATCH_GAP, nullptr, 0, state->pre_match_gap, PRE_MATCH_GAP_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "effects_index", ADDR_EFFECT_INDEX, nullptr, 0, &state->effects.effect_index, sizeof(state->effects.effect_index));
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "effects_array", ADDR_EFFECT_ARRAY, nullptr, 0, &state->effects.effects[0], EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "input_indices", ADDR_INPUT_READ_IDX, nullptr, 0, &state->input.read_idx, sizeof(state->input.read_idx) * 4);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p1_input_buffer", ADDR_P1_INPUT_BUFFER, nullptr, 0, state->input.p1_buffer, INPUT_BUFFER_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p2_input_buffer", ADDR_P2_INPUT_BUFFER, nullptr, 0, state->input.p2_buffer, INPUT_BUFFER_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p1_input_state", ADDR_P1_INPUT_STATE, nullptr, 0, state->input.p1_state, INPUT_STATE_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p2_input_state", ADDR_P2_INPUT_STATE, nullptr, 0, state->input.p2_state, INPUT_STATE_SIZE);
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p1_input_history_window", ADDR_P1_INPUT_HISTORY + historyStart * sizeof(uint16_t), nullptr, 0, state->input.p1_history, INPUT_HISTORY_WINDOW * sizeof(uint16_t));
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "p2_input_history_window", ADDR_P2_INPUT_HISTORY + historyStart * sizeof(uint16_t), nullptr, 0, state->input.p2_history, INPUT_HISTORY_WINDOW * sizeof(uint16_t));
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "current_inputs", ADDR_P1_INPUT_HISTORY + state->input.write_idx * sizeof(uint16_t), nullptr, 0, &state->input.p1_input, sizeof(state->input.p1_input) + sizeof(state->input.p2_input));
    AS2_DumpRollbackTraceBytes(scope, frame, eventId, "summons", ADDR_SUMMON_ARRAY, nullptr, 0, &state->summons.summons[0], SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE);
}

template <typename T>
static void TraceScalarWriteInternal(const char* scope,
                                     int frame,
                                     uint32_t eventId,
                                     const char* name,
                                     uintptr_t address,
                                     T value,
                                     bool doTrace) {
    T before = {};
    if (doTrace) {
        before = ReadMemory<T>(address);
    }
    *reinterpret_cast<T*>(address) = value;
    if (doTrace) {
        T after = ReadMemory<T>(address);
        AS2_DumpRollbackTraceBytes(scope, frame, eventId, name, address, &before, sizeof(before), &after, sizeof(after));
    }
}

static void TraceBlockWriteInternal(const char* scope,
                                    int frame,
                                    uint32_t eventId,
                                    const char* name,
                                    uintptr_t address,
                                    const void* src,
                                    size_t size,
                                    bool doTrace) {
    if (!src || size == 0) {
        return;
    }

    uint8_t* before = nullptr;
    if (doTrace) {
        before = (uint8_t*)malloc(size);
        if (before && !CopyMemorySafe(before, (const void*)address, size)) {
            free(before);
            before = nullptr;
        }
    }

    memcpy((void*)address, src, size);

    if (doTrace) {
        AS2_DumpRollbackTraceBytes(scope,
                                   frame,
                                   eventId,
                                   name,
                                   address,
                                   before,
                                   before ? size : 0,
                                   (const void*)address,
                                   size);
    }

    if (before) {
        free(before);
    }
}

// ============================================================================
// Game State Access
// ============================================================================

uint32_t AS2_GetFrameNumber() {
    return ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

// GetGameMode() and GetSubstate() are now defined as inline functions in game_state.h

// Game state tracking
// Mode 8 = VS/Training match
// Sub-states within mode 8:
//   0 = Match initialization
//   1 = Unknown
//   2 = Match state setup
//   3 = Main gameplay (fighting)
//   4 = Pause menu
//   5 = Match end/result
bool AS2_IsInMatch() {
    return GetGameMode() == MODE_MATCH;
}

bool AS2_IsInGameplay() {
    // Mode 8 Substate 3 is the core match loop, but not always the
    // interactive fighting window.
    return IsInActiveGameplay();
}

bool AS2_IsInPlayableGameplay() {
    return IsInPlayableMatchGameplay();
}

bool AS2_IsInPauseMenu() {
    // In match mode AND in pause menu sub-state (4)
    return GetGameMode() == MODE_MATCH && GetSubstate() == 4;
}

bool AS2_IsInMenu() {
    // Not in match mode = in some menu
    uint32_t mode = GetGameMode();
    return mode != MODE_MATCH || GetSubstate() != 3;
}

// ============================================================================
// FPU State Management (for floating-point determinism)
// ============================================================================
// DXLib games rely heavily on x87 FPU for rendering and physics.
// The FPU has internal state (registers, status, control word) that affects
// calculations. Without saving/restoring this state, rollback can produce
// different results even with identical game state, causing desyncs.
//
// We save/restore ONLY control words (precision + rounding mode), never the
// full register stack. Exception masks are always forced ON.

// FPU diagnostics counters
static uint32_t g_fpuSaveCount = 0;
static uint32_t g_fpuRestoreCount = 0;
static uint32_t g_fpuAnomalyCount = 0;
static uint16_t g_fpuLastSavedCW = 0;
static uint32_t g_fpuLastSavedMXCSR = 0;
static uint16_t g_fpuLastRestoredCW = 0;
static uint32_t g_fpuLastRestoredMXCSR = 0;

static constexpr uint16_t kManagedFpuControlWordMask = 0x0F3F;

// Decode FPU control word bits into human-readable string
static void FormatFpuControlWord(uint16_t cw, char* buf, size_t bufSize) {
    const char* precision = "??";
    switch ((cw >> 8) & 3) {
        case 0: precision = "24bit"; break;
        case 1: precision = "??res"; break;
        case 2: precision = "53bit"; break;
        case 3: precision = "64bit"; break;
    }
    const char* rounding = "??";
    switch ((cw >> 10) & 3) {
        case 0: rounding = "near"; break;
        case 1: rounding = "down"; break;
        case 2: rounding = "up"; break;
        case 3: rounding = "trunc"; break;
    }
    bool im = (cw & 0x01) != 0;  // Invalid operation mask
    bool dm = (cw & 0x02) != 0;  // Denormal mask
    bool zm = (cw & 0x04) != 0;  // Divide-by-zero mask
    bool om = (cw & 0x08) != 0;  // Overflow mask
    bool um = (cw & 0x10) != 0;  // Underflow mask
    bool pm = (cw & 0x20) != 0;  // Precision mask
    snprintf(buf, bufSize, "0x%04X [prec=%s round=%s masks=%c%c%c%c%c%c]",
             cw, precision, rounding,
             im?'I':'.', dm?'D':'.', zm?'Z':'.', om?'O':'.', um?'U':'.', pm?'P':'.');
}

static bool FpuControlWordManagedBitsMatch(uint16_t expected, uint16_t actual) {
    return (expected & kManagedFpuControlWordMask) ==
           (actual & kManagedFpuControlWordMask);
}

// Decode MXCSR bits into human-readable string
static void FormatMXCSR(uint32_t mxcsr, char* buf, size_t bufSize) {
    const char* rounding = "??";
    switch ((mxcsr >> 13) & 3) {
        case 0: rounding = "near"; break;
        case 1: rounding = "down"; break;
        case 2: rounding = "up"; break;
        case 3: rounding = "trunc"; break;
    }
    bool ftz = (mxcsr & 0x8000) != 0;
    bool im = (mxcsr & 0x0080) != 0;
    bool dm = (mxcsr & 0x0100) != 0;
    bool zm = (mxcsr & 0x0200) != 0;
    bool om = (mxcsr & 0x0400) != 0;
    bool um = (mxcsr & 0x0800) != 0;
    bool pm = (mxcsr & 0x1000) != 0;
    // Status flags (exception occurred bits 0-5)
    uint8_t statusFlags = mxcsr & 0x3F;
    snprintf(buf, bufSize, "0x%08X [round=%s ftz=%d masks=%c%c%c%c%c%c status=0x%02X]",
             mxcsr, rounding, ftz ? 1 : 0,
             im?'I':'.', dm?'D':'.', zm?'Z':'.', om?'O':'.', um?'U':'.', pm?'P':'.',
             statusFlags);
}

// Capture the LIVE FPU state for diagnostic purposes (does not modify anything)
void AS2_GetCurrentFpuState(uint16_t* outCW, uint16_t* outSW, uint32_t* outMXCSR) {
    uint16_t cw = 0, sw = 0;
    uint32_t mxcsr = 0;
    __asm {
        fnstcw word ptr [cw]
        fnstsw word ptr [sw]
        stmxcsr dword ptr [mxcsr]
    }
    if (outCW) *outCW = cw;
    if (outSW) *outSW = sw;
    if (outMXCSR) *outMXCSR = mxcsr;
}

// Log the full current FPU state with a tag
void AS2_LogFpuState(const char* tag) {
    uint16_t cw = 0, sw = 0;
    uint32_t mxcsr = 0;
    AS2_GetCurrentFpuState(&cw, &sw, &mxcsr);

    char cwStr[128], mxcsrStr[128];
    FormatFpuControlWord(cw, cwStr, sizeof(cwStr));
    FormatMXCSR(mxcsr, mxcsrStr, sizeof(mxcsrStr));

    // x87 status word: top-of-stack (bits 11-13), condition codes, exception flags
    uint8_t  topOfStack = (sw >> 11) & 7;
    uint8_t  swExcFlags = sw & 0x3F;
    bool     stackFault = (sw & 0x40) != 0;
    bool     errorSummary = (sw & 0x80) != 0;

    LOG_INFO("[FPU][%s] CW=%s SW=0x%04X[top=%d exc=0x%02X sf=%d es=%d] MXCSR=%s",
             tag, cwStr, sw, topOfStack, swExcFlags, stackFault ? 1 : 0, errorSummary ? 1 : 0,
             mxcsrStr);
}

// Check if FPU state looks healthy, return false and log if anomalous
bool AS2_ValidateFpuState(const char* context) {
    uint16_t cw = 0, sw = 0;
    uint32_t mxcsr = 0;
    AS2_GetCurrentFpuState(&cw, &sw, &mxcsr);

    bool healthy = true;
    char reason[256] = {0};
    int rlen = 0;

    // Check 1: x87 exception masks should all be SET (bits 0-5 = 0x3F)
    if ((cw & 0x3F) != 0x3F) {
        rlen += snprintf(reason + rlen, sizeof(reason) - rlen, "CW_MASKS_UNSET(0x%04X) ", cw);
        healthy = false;
    }

    // Check 2: SSE exception masks should all be SET (bits 7-12 = 0x1F80)
    if ((mxcsr & 0x1F80) != 0x1F80) {
        rlen += snprintf(reason + rlen, sizeof(reason) - rlen, "MXCSR_MASKS_UNSET(0x%08X) ", mxcsr);
        healthy = false;
    }

    // Check 3: x87 stack fault flag
    if (sw & 0x40) {
        rlen += snprintf(reason + rlen, sizeof(reason) - rlen, "X87_STACK_FAULT ");
        healthy = false;
    }

    // Check 4: x87 error summary flag
    if (sw & 0x80) {
        rlen += snprintf(reason + rlen, sizeof(reason) - rlen, "X87_ERROR_SUMMARY ");
        healthy = false;
    }

    // Check 5: MXCSR status flags (pending exceptions in bits 0-5)
    if (mxcsr & 0x3F) {
        rlen += snprintf(reason + rlen, sizeof(reason) - rlen, "MXCSR_PENDING_EXC(0x%02X) ", mxcsr & 0x3F);
        // Not necessarily fatal - just worth noting
    }

    if (!healthy) {
        g_fpuAnomalyCount++;
        char cwStr[128], mxcsrStr[128];
        FormatFpuControlWord(cw, cwStr, sizeof(cwStr));
        FormatMXCSR(mxcsr, mxcsrStr, sizeof(mxcsrStr));
        LOG_ERROR("[FPU][ANOMALY #%u][%s] %s CW=%s SW=0x%04X MXCSR=%s",
                  g_fpuAnomalyCount, context, reason, cwStr, sw, mxcsrStr);
    }

    return healthy;
}

void AS2_SaveFpuState(uint8_t* buffer) {
    if (!buffer) return;

    // Full FSAVE captures the complete x87 FPU state (108 bytes):
    // control word, status word, tag word, IP, DP, and all 8 ST registers.
    // FSAVE reinitializes the FPU after saving, so we immediately FRSTOR
    // to restore the live FPU state and avoid FLT_STACK_CHECK crashes
    // that occur when subsequent C++ float operations hit a reset FPU.
    __asm {
        mov eax, buffer
        fsave [eax]           // Save full 108-byte x87 state; FPU is reinitialized
        frstor [eax]          // Immediately restore so FPU continues normally
    }

    g_fpuSaveCount++;
    g_fpuLastSavedCW = *(const uint16_t*)buffer;        // CW is at offset 0 of FSAVE layout
    g_fpuLastSavedMXCSR = *(const uint32_t*)(buffer + 4); // Not MXCSR — this is SW in FSAVE; actual MXCSR saved below

    // Log every save during first 10, then every 300th
    if (g_fpuSaveCount <= 10 || (g_fpuSaveCount % 300) == 0) {
        char cwStr[128], mxcsrStr[128];
        FormatFpuControlWord(g_fpuLastSavedCW, cwStr, sizeof(cwStr));
        FormatMXCSR(g_fpuLastSavedMXCSR, mxcsrStr, sizeof(mxcsrStr));
        LOG_DEBUG("[FPU] Save #%u: CW=%s (FSAVE full 108B)", g_fpuSaveCount, cwStr);
    }

    // Validate: warn if saved CW has unmasked exceptions (shouldn't happen in normal flow)
    if ((g_fpuLastSavedCW & 0x3F) != 0x3F) {
        g_fpuAnomalyCount++;
        LOG_ERROR("[FPU][ANOMALY #%u] Save #%u captured CW with UNMASKED exceptions: 0x%04X "
                  "(bits 0-5 should be 0x3F but got 0x%02X). Something modified the FPU before save!",
                  g_fpuAnomalyCount, g_fpuSaveCount, g_fpuLastSavedCW, g_fpuLastSavedCW & 0x3F);
    }
}

void AS2_SaveMxcsr(uint32_t* mxcsrOut) {
    if (!mxcsrOut) return;
    __asm {
        mov eax, mxcsrOut
        stmxcsr dword ptr [eax]
    }
}

void AS2_RestoreFpuState(const uint8_t* buffer) {
    if (!buffer) return;

    // Full FRSTOR restores the complete x87 FPU state from the 108-byte FSAVE image.
    // This includes: control word, status word, tag word, instruction/data pointers,
    // and all 8 ST registers — ensuring exact floating-point determinism across rollback.
    //
    // Safety: we still force exception masks ON in the restored control word to prevent
    // FLT_INEXACT_RESULT / STATUS_FLOAT_MULTIPLE_FAULTS crashes when GDI32 or DXLib
    // performs float arithmetic after our restore.

    // Make a mutable copy for exception-mask fixup
    uint8_t safe_buf[108];
    memcpy(safe_buf, buffer, 108);

    // Fixup CW in the FSAVE image: force all exception masks ON (bits 0-5)
    uint16_t* cw_ptr = (uint16_t*)&safe_buf[0];
    uint16_t saved_cw = *cw_ptr;
    *cw_ptr = (saved_cw & 0x0F00) | 0x003F;  // Keep precision+rounding, mask all exceptions

    g_fpuRestoreCount++;
    g_fpuLastRestoredCW = *cw_ptr;

    // Log every restore during first 10, then every 300th, or if bits changed
    bool cwChanged = (saved_cw & 0x0F00) != (g_fpuLastSavedCW & 0x0F00);
    if (g_fpuRestoreCount <= 10 || (g_fpuRestoreCount % 300) == 0 || cwChanged) {
        char cwStr[128], safeCwStr[128];
        FormatFpuControlWord(saved_cw, cwStr, sizeof(cwStr));
        FormatFpuControlWord(*cw_ptr, safeCwStr, sizeof(safeCwStr));
        LOG_DEBUG("[FPU] Restore #%u: raw CW=%s -> safe CW=%s (FRSTOR full 108B)%s",
                  g_fpuRestoreCount, cwStr, safeCwStr,
                  cwChanged ? " [CW BITS CHANGED]" : "");
    }

    // Anomaly: if saved CW had unmasked exceptions, warn loudly (we're fixing it)
    if ((saved_cw & 0x3F) != 0x3F) {
        g_fpuAnomalyCount++;
        LOG_ERROR("[FPU][ANOMALY #%u] Restore #%u: saved CW had UNMASKED exceptions: 0x%04X -> forced safe: 0x%04X",
                  g_fpuAnomalyCount, g_fpuRestoreCount, saved_cw, *cw_ptr);
    }

    __asm {
        lea eax, safe_buf
        frstor [eax]
    }

    // Post-restore validation: confirm the FPU is sane after restore
    uint16_t verify_cw = 0;
    __asm {
        fnstcw word ptr [verify_cw]
    }

    const bool cwManagedMatch = FpuControlWordManagedBitsMatch(*cw_ptr, verify_cw);
    const uint16_t ignoredCwBits = (uint16_t)((verify_cw ^ *cw_ptr) & ~kManagedFpuControlWordMask);
    if (cwManagedMatch && ignoredCwBits != 0 &&
        (g_fpuRestoreCount <= 10 || (g_fpuRestoreCount % 300) == 0)) {
        LOG_DEBUG("[FPU] Restore #%u: x87 readback changed ignored CW bits (loaded=0x%04X readback=0x%04X delta=0x%04X)",
                  g_fpuRestoreCount, *cw_ptr, verify_cw, ignoredCwBits);
    }

    if (!cwManagedMatch) {
        g_fpuAnomalyCount++;
        LOG_ERROR("[FPU][ANOMALY #%u] Post-restore verification FAILED! Expected CW=0x%04X got 0x%04X",
                  g_fpuAnomalyCount, *cw_ptr, verify_cw);
    }
}

void AS2_RestoreMxcsr(const uint32_t* mxcsrIn) {
    if (!mxcsrIn) return;
    // Force all SSE exception masks ON (bits 7-12), keep rounding mode (bits 13-14)
    uint32_t safe_mxcsr = (*mxcsrIn & 0x00006000) | 0x00001F80;
    g_fpuLastRestoredMXCSR = safe_mxcsr;
    __asm {
        ldmxcsr dword ptr [safe_mxcsr]
    }
    // Post-restore validation
    uint32_t verify_mxcsr = 0;
    __asm {
        stmxcsr dword ptr [verify_mxcsr]
    }
    if ((verify_mxcsr & 0x7F80) != (safe_mxcsr & 0x7F80)) {
        g_fpuAnomalyCount++;
        LOG_ERROR("[FPU][ANOMALY #%u] MXCSR post-restore FAILED! Expected 0x%08X got 0x%08X",
                  g_fpuAnomalyCount, safe_mxcsr, verify_mxcsr);
    }
}

// Public accessors for crash handler
uint32_t AS2_GetFpuSaveCount()    { return g_fpuSaveCount; }
uint32_t AS2_GetFpuRestoreCount() { return g_fpuRestoreCount; }
uint32_t AS2_GetFpuAnomalyCount() { return g_fpuAnomalyCount; }

// ============================================================================
// Pre-match arena zero
// ============================================================================
// Zero entity blocks, match context, and match-header memory.  This
// eliminates residual data from ANY previous match (including demo matches
// the game runs automatically on the title screen).  The game's own asset
// loading re-initialises every field it reads, so anything it does NOT
// touch will be harmlessly zero on both peers.
//
// This function is called:
//   1. At the Mode→Match(8) transition in NetplayCore (pre-load safety net),
//      immediately before AS2_ScrubTransientMatchState()
//   2. On session connect (UpdateAutoHooks, State→Connected transition)
void AS2_ZeroMatchArena() {
    // Entity P1 (108 KB)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_P1_ENTITY_BASE, ENTITY_SIZE, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_P1_ENTITY_BASE, 0, ENTITY_SIZE);
            VirtualProtect((void*)ADDR_P1_ENTITY_BASE, ENTITY_SIZE, oldProtect, &oldProtect);
        }
    }
    // Entity P2 (108 KB)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_P2_ENTITY_BASE, ENTITY_SIZE, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_P2_ENTITY_BASE, 0, ENTITY_SIZE);
            VirtualProtect((void*)ADDR_P2_ENTITY_BASE, ENTITY_SIZE, oldProtect, &oldProtect);
        }
    }
    // Match context — camera, weather particles, screen shake (7456 bytes)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_MATCH_CONTEXT, MATCH_CONTEXT_SIZE, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_MATCH_CONTEXT, 0, MATCH_CONTEXT_SIZE);
            VirtualProtect((void*)ADDR_MATCH_CONTEXT, MATCH_CONTEXT_SIZE, oldProtect, &oldProtect);
        }
    }
    // Match header — match_active, winner_index, win_timer, etc. (16 bytes)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_MATCH_BASE, 16, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_MATCH_BASE, 0, 16);
            VirtualProtect((void*)ADDR_MATCH_BASE, 16, oldProtect, &oldProtect);
        }
    }
    // Pre-match gap (12 bytes between effect_index and match_header)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_PRE_MATCH_GAP, 0, PRE_MATCH_GAP_SIZE);
            VirtualProtect((void*)ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE, oldProtect, &oldProtect);
        }
    }

    LOG_INFO("[Scrub] Zeroed match arena: P1 entity (%u bytes), P2 entity (%u bytes), "
             "match_context (%u bytes), match_header (16 bytes), pre_match_gap (%u bytes)",
             ENTITY_SIZE, ENTITY_SIZE, MATCH_CONTEXT_SIZE, PRE_MATCH_GAP_SIZE);
}

static void ResetMatchInputStateForBootstrap() {
    // Mirror vanilla match-start behavior with a stronger clean slate:
    //  - Reset frame indices used by the input/history pipeline
    //  - Clear the full per-player button arrays (held/current/pressed/counters)
    //  - Refill long-term history with the game's empty sentinel (0xFFFF)
    //
    // Without this, menu-confirm/stage-select carryover can leave different
    // hold/cooldown/history bytes on each peer at the Mode 8 -> gameplay handoff.
    // Those bytes now participate in rollback savestates and load-barrier hashes,
    // so match bootstrap can fail before rollback even starts.
    WriteMemory<uint32_t>(ADDR_INPUT_READ_IDX, 0);
    WriteMemory<uint32_t>(ADDR_INPUT_DISPLAY_IDX, 0);
    WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, 0);
    WriteMemory<uint32_t>(ADDR_INPUT_NET_IDX, 0);

    uint8_t zeroInput[INPUT_BUFFER_SIZE] = {};
    uint8_t zeroState[INPUT_STATE_SIZE] = {};
    WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_BUFFER, zeroInput, sizeof(zeroInput));
    WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_BUFFER, zeroInput, sizeof(zeroInput));
    WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_STATE, zeroState, sizeof(zeroState));
    WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_STATE, zeroState, sizeof(zeroState));

    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_P1_INPUT_HISTORY, INPUT_HISTORY_P1_SIZE, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_P1_INPUT_HISTORY, 0xFF, INPUT_HISTORY_P1_SIZE);
            VirtualProtect((void*)ADDR_P1_INPUT_HISTORY, INPUT_HISTORY_P1_SIZE, oldProtect, &oldProtect);
        }
    }
    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_P2_INPUT_HISTORY, INPUT_HISTORY_P2_SIZE, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_P2_INPUT_HISTORY, 0xFF, INPUT_HISTORY_P2_SIZE);
            VirtualProtect((void*)ADDR_P2_INPUT_HISTORY, INPUT_HISTORY_P2_SIZE, oldProtect, &oldProtect);
        }
    }

    LOG_INFO("[Scrub] Reset match input state: indices cleared, buffers zeroed, histories filled with 0xFFFF");
}

// Pre-match transient state scrub
// ============================================================================
// Zeroes transient arrays (effects, summons, pre-match gap), resets FPU to
// deterministic defaults, and clears the input pipeline.
//
// CALLING CONVENTION:
//   Called at the Mode→Match(8) transition in netplay_core.cpp, BEFORE the
//   game initializes entities in substates 0-2.  This ensures the game's own
//   init builds on a completely clean slate — no leftover effects/summons
//   from a previous match, no stale input state from CharSel.
//
//   Previously this was called from MatchBootstrap::NotifyLocalLoaded()
//   (during the load barrier, AFTER substate 2), which interfered with the
//   round-start intro animation.  The game sets the intro freeze flag and
//   fade timer during substate 2; scrubbing state after that point was
//   resetting things the game had already set up.
//
// On rematches the game only resets effect/summon write-indices, not the
// full arrays, so leftover data from the previous match persists.  Because
// rollback means each peer may have ended the last match with different
// transient state, the load-barrier hashes could diverge unless we zero
// these regions before the game initializes.
void AS2_ScrubTransientMatchState() {
    // NOTE: When called at Mode→Match entry, AS2_ZeroMatchArena() has already
    // been called immediately before us, so entities/context/header/gap are
    // already zeroed.  We still zero pre-match gap here for completeness
    // (harmless redundancy).

    // 1. Effects: zero all 200 slots + reset write index
    {
        uint8_t zeroBuf[EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE] = {};
        WriteMemoryBlockSafe((void*)ADDR_EFFECT_ARRAY, zeroBuf, sizeof(zeroBuf));
        WriteMemory<uint32_t>(ADDR_EFFECT_INDEX, 0);
    }

    // 2. Summons: zero all 100 slots
    {
        uint8_t zeroBuf[SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE] = {};
        WriteMemoryBlockSafe((void*)ADDR_SUMMON_ARRAY, zeroBuf, sizeof(zeroBuf));
    }

    // 3. Pre-match gap: zero the 12-byte region to match arena-zeroed state
    {
        DWORD oldProtect = 0;
        if (VirtualProtect((void*)ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE, PAGE_READWRITE, &oldProtect)) {
            memset((void*)ADDR_PRE_MATCH_GAP, 0, PRE_MATCH_GAP_SIZE);
            VirtualProtect((void*)ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE, oldProtect, &oldProtect);
        }
    }

    // 4. FPU: set to deterministic defaults (double precision, round-to-nearest,
    //    all exceptions masked).  This matches the MSVC CRT startup state.
    //    x87 CW  = 0x027F: PC=10 (double), RC=00 (nearest), all masks ON
    //    MXCSR   = 0x1F80: RC=00, all masks ON, no flush-to-zero / DAZ
    {
        uint16_t defaultCW = 0x027F;
        uint32_t defaultMXCSR = 0x1F80;
        __asm {
            fldcw word ptr [defaultCW]
            ldmxcsr dword ptr [defaultMXCSR]
        }
    }

    // 5. Input pipeline: clear any menu/stage-select carryover before the
    //    load barrier snapshot becomes the rollback baseline.
    ResetMatchInputStateForBootstrap();

    LOG_INFO("[Scrub] Zeroed effects (%d slots), summons (%d slots), pre_match_gap (%u bytes), reset FPU to defaults, reset match input state",
             EFFECT_MAX_SLOTS, SUMMON_MAX_SLOTS, PRE_MATCH_GAP_SIZE);
}

// ============================================================================
// Analysis of the decompiled source (research/アリス戦記２.exe - Copy.c) shows:
// 
// 1. The main gameplay loop (sub_4C9B50 / Mode 8 sub-state 3) does NOT allocate
//    memory. It only calls update functions for entities, effects, input, etc.
// 
// 2. Memory allocations (calloc, malloc, HeapAlloc) only occur during:
//    - Game initialization
//    - Resource/file loading (PNG, data files)
//    - Mode transitions (loading stages, characters)
// 
// 3. During rollback resimulation, the same gameplay update functions run,
//    which don't allocate. Therefore, no "orphaned" allocations can occur.
//
// Conclusion: Heap tracking is NOT necessary for this game.

void AS2_HeapTracker_Init() {
    // NOT NEEDED for Alice Senki 2 - see analysis above
    LOG_DEBUG("[HeapTracker] Not needed for AS2 - gameplay loop has no allocations");
}

void AS2_HeapTracker_Shutdown() {}

// Stub implementations - heap tracking not needed for AS2
void AS2_HeapTracker_SetFrame(int frame) { (void)frame; }
void AS2_HeapTracker_BeginRollback(int targetFrame) { (void)targetFrame; }
void AS2_HeapTracker_EndRollback() {}
void AS2_HeapTracker_ConfirmFrame(int frame) { (void)frame; }
bool AS2_HeapTracker_IsActive() { return false; }
void AS2_HeapTracker_GetStats(int* outTrackedAllocs, int* outRollbackFreed) {
    if (outTrackedAllocs) *outTrackedAllocs = 0;
    if (outRollbackFreed) *outRollbackFreed = 0;
}

// ============================================================================
// Deterministic Digest & Desync Debugging
// ============================================================================

#pragma pack(push, 1)
typedef struct {
    uint32_t rng_seed;
    uint32_t fpu_crc;
    struct {
        uint32_t game_mode;
        uint32_t sub_state;
        // NOTE: sub_state_timer is INTENTIONALLY EXCLUDED from the checksum.
        uint32_t game_type;
    } global;
    
    // Match state (timers, wins, combos)
    struct {
        int32_t  round_timer[2];
        uint16_t win_count[2];
        uint8_t  combo_count[2];
        uint32_t intro_fade_timer;
        // Match header (bytes 0-15 at ADDR_MATCH_BASE) - critical for round state
        uint8_t  match_header[16];
        uint32_t match_context_crc;
        uint32_t pre_match_gap_crc;
    } match;

    struct {
        uint32_t effect_index;
        struct {
            uint8_t  type;
            int16_t  x_pos;
            int16_t  y_pos;
            int16_t  timer;
            int32_t  vel_x;
            int32_t  vel_y;
            int32_t  data1;
            int32_t  data2;
            int32_t  data3;
        } effects[EFFECT_MAX_SLOTS];
    } effects;

    struct {
        uint32_t read_idx;
        uint32_t display_idx;
        uint32_t write_idx;
        uint32_t net_idx;
        uint32_t p1_buffer_crc;
        uint32_t p2_buffer_crc;
        uint32_t p1_state_crc;
        uint32_t p2_state_crc;
        uint32_t p1_history_crc;
        uint32_t p2_history_crc;
        uint16_t p1_input;
        uint16_t p2_input;
    } input;

    CompactEntity_t p1;
    CompactEntity_t p2;

    struct {
        uint8_t  owner_id;
        uint32_t summon_type;
        uint8_t  state_flag;
        uint8_t  sub_flag;
        uint32_t state_flag2;
        uint32_t action_id;
        int16_t  x_pos;
        int16_t  y_pos;
        uint8_t  facing;
        uint8_t  parent_id;
        uint8_t  anim_frame;
        uint8_t  anim_state;
    } summons[SUMMON_MAX_SLOTS];
} DeterministicDigest_t;
#pragma pack(pop)

// Helper to capture raw memory into a CompactSaveState_t
void AS2_CaptureCurrentState(CompactSaveState_t* buffer) {
    if (!buffer) return;
    
    buffer->frame_number = AS2_GetFrameNumber();
    buffer->rng_seed = AS2_GetRngSeed();
    buffer->visual_rng_seed = AS2_GetVisualRngSeed();
    buffer->format_version = 2;  // Full FSAVE+MXCSR + visual RNG seed
    
    // Save FPU state for floating-point determinism (full FSAVE into fpu_state[108])
    AS2_SaveFpuState(buffer->fpu_state);
    // Save MXCSR (SSE control/status) separately
    AS2_SaveMxcsr(&buffer->mxcsr);
    
    // Global state
    buffer->global.frame_counter = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    buffer->global.game_mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    buffer->global.sub_state = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    buffer->global.sub_state_timer = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    buffer->global.game_type = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    
    // Match state
    // NOTE: The game only has ONE round timer (dword_790E50), NOT a per-player array.
    // word_790E54 and byte_790E56 are separate variables, not indices into the timer.
    // Reading ADDR_ROUND_TIMER+4 would read garbage. Set [1] to 0 for determinism.
    buffer->match.round_timer[0] = ReadMemory<int32_t>(ADDR_ROUND_TIMER);
    buffer->match.round_timer[1] = 0;  // No second timer - game uses single shared timer
    buffer->match.win_count[0] = ReadMemory<uint16_t>(ADDR_WIN_COUNT);
    buffer->match.win_count[1] = 0;    // word_790E54 is frame sub-counter, not P2 wins
    buffer->match.combo_count[0] = ReadMemory<uint8_t>(ADDR_COMBO_COUNT);
    buffer->match.combo_count[1] = 0;  // byte_790E57 is unrelated data
    buffer->match.intro_fade_timer = ReadMemory<uint32_t>(ADDR_MATCH_INTRO_FADE_TIMER);
    
    // Match header state (bytes 0-15 at ADDR_MATCH_BASE)
    // Critical for round end detection: winner_index, win_timer, result_code
    CopyMemorySafe(buffer->match.match_header, (void*)ADDR_MATCH_BASE, 16);
    
    // Full entity snapshots
    uintptr_t p1Base = GetEntityBase(0);
    uintptr_t p2Base = GetEntityBase(1);
    CopyMemorySafe(buffer->p1_entity, (void*)p1Base, ENTITY_SIZE);
    CopyMemorySafe(buffer->p2_entity, (void*)p2Base, ENTITY_SIZE);
    
    // Match context gap: camera scroll, screen shake, weather particles, misc state.
    // Weather particles call rand() every frame — without restoring this block,
    // rollback causes the RNG sequence to diverge, desyncing all gameplay.
    //
    // FIX: Clear per-frame temp data (68 bytes at match+0x700) in game memory
    // BEFORE capturing.  In normal play Match_ClearPerFrameTempData runs before
    // each frame, but during rollback resim multiple frames execute inside the
    // game's while(!Input_TryGetNextFrame) loop without returning to the outer
    // function — so the clearing never runs between resim frames.  By zeroing
    // here (which happens in the GekkoNet SaveEvent, always before AdvanceEvent)
    // we guarantee: (a) the savestate has cleared temp data, and (b) the game's
    // live memory is clean before the next frame starts.
    memset(reinterpret_cast<void*>(ADDR_MATCH_PER_FRAME_TEMP), 0, MATCH_PER_FRAME_TEMP_SIZE);
    CopyMemorySafe(buffer->match_context, (void*)ADDR_MATCH_CONTEXT, MATCH_CONTEXT_SIZE);
    
    // Pre-match gap (12 bytes between effect_index+4 and match_header)
    // Contains render blend timer, audio SE channel index, and padding.
    CopyMemorySafe(buffer->pre_match_gap, (void*)ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE);
    
    // Effects — raw memcpy captures all 32 bytes per entry including any padding
    // bytes that the field-by-field approach might miss (e.g., byte at offset 5).
    // Effect_t is #pragma pack(push,1) with sizeof == EFFECT_ENTRY_SIZE (32), so
    // the array layout matches game memory exactly.
    buffer->effects.effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
    static_assert(sizeof(Effect_t) == EFFECT_ENTRY_SIZE, "Effect_t size must match EFFECT_ENTRY_SIZE");
    CopyMemorySafe(&buffer->effects.effects[0], (void*)ADDR_EFFECT_ARRAY, EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE);
    
    // Input system
    buffer->input.read_idx = ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX);
    buffer->input.display_idx = ReadMemory<uint32_t>(ADDR_INPUT_DISPLAY_IDX);
    buffer->input.write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);
    buffer->input.net_idx = ReadMemory<uint32_t>(ADDR_INPUT_NET_IDX);
    
    CopyMemorySafe(buffer->input.p1_buffer, (void*)ADDR_P1_INPUT_BUFFER, INPUT_BUFFER_SIZE);
    CopyMemorySafe(buffer->input.p2_buffer, (void*)ADDR_P2_INPUT_BUFFER, INPUT_BUFFER_SIZE);
    CopyMemorySafe(buffer->input.p1_state, (void*)ADDR_P1_INPUT_STATE, INPUT_STATE_SIZE);
    CopyMemorySafe(buffer->input.p2_state, (void*)ADDR_P2_INPUT_STATE, INPUT_STATE_SIZE);
    
    uint32_t curIdx = buffer->input.write_idx;
    buffer->input.p1_input = ReadMemory<uint16_t>(ADDR_P1_INPUT_HISTORY + curIdx * 2);
    buffer->input.p2_input = ReadMemory<uint16_t>(ADDR_P2_INPUT_HISTORY + curIdx * 2);
    
    uint32_t historyStart = (buffer->input.write_idx >= INPUT_HISTORY_WINDOW) 
                          ? (buffer->input.write_idx - INPUT_HISTORY_WINDOW) : 0;
    for (int i = 0; i < INPUT_HISTORY_WINDOW; i++) {
        uint32_t frameIdx = historyStart + i;
        buffer->input.p1_history[i] = ReadMemory<uint16_t>(ADDR_P1_INPUT_HISTORY + frameIdx * 2);
        buffer->input.p2_history[i] = ReadMemory<uint16_t>(ADDR_P2_INPUT_HISTORY + frameIdx * 2);
    }

    // Summons
    CopyMemorySafe(&buffer->summons.summons[0], (void*)ADDR_SUMMON_ARRAY, SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE);
}

// Helper to build digest from a captured state
void AS2_BuildDigest(const CompactSaveState_t* buffer, DeterministicDigest_t* digest) {
    if (!buffer || !digest) return;
    memset(digest, 0, sizeof(*digest));

    digest->rng_seed = buffer->rng_seed;
    // Only CRC the FPU control word (first 2 bytes of FSAVE image) + MXCSR.
    // The full 108-byte FSAVE includes x87 register VALUES which contain
    // rendering garbage (different between peers due to frame count differences).
    // Only the control state (precision, rounding, exception masks) matters
    // for simulation determinism.
    {
        uint8_t fpuDeterministic[6] = {};
        memcpy(fpuDeterministic, buffer->fpu_state, 2);  // CW from FSAVE
        memcpy(fpuDeterministic + 2, &buffer->mxcsr, 4); // MXCSR
        digest->fpu_crc = CalcCRC32(fpuDeterministic, sizeof(fpuDeterministic));
    }
    digest->global.game_mode = buffer->global.game_mode;
    digest->global.sub_state = buffer->global.sub_state;
    digest->global.game_type = buffer->global.game_type;
    
    digest->match.round_timer[0] = buffer->match.round_timer[0];
    digest->match.round_timer[1] = buffer->match.round_timer[1];
    digest->match.win_count[0] = buffer->match.win_count[0];
    digest->match.win_count[1] = buffer->match.win_count[1];
    digest->match.combo_count[0] = buffer->match.combo_count[0];
    digest->match.combo_count[1] = buffer->match.combo_count[1];
    digest->match.intro_fade_timer = buffer->match.intro_fade_timer;
    // Copy match header (critical for round state)
    memcpy(digest->match.match_header, buffer->match.match_header, 16);
    digest->match.match_context_crc = CalcCRC32(buffer->match_context, MATCH_CONTEXT_SIZE);
    digest->match.pre_match_gap_crc = CalcCRC32(buffer->pre_match_gap, PRE_MATCH_GAP_SIZE);

    digest->effects.effect_index = buffer->effects.effect_index;
    for (int i = 0; i < EFFECT_MAX_SLOTS; i++) {
        digest->effects.effects[i].type = buffer->effects.effects[i].type;
        digest->effects.effects[i].x_pos = buffer->effects.effects[i].x_pos;
        digest->effects.effects[i].y_pos = buffer->effects.effects[i].y_pos;
        digest->effects.effects[i].timer = buffer->effects.effects[i].timer;
        digest->effects.effects[i].vel_x = buffer->effects.effects[i].vel_x;
        digest->effects.effects[i].vel_y = buffer->effects.effects[i].vel_y;
        digest->effects.effects[i].data1 = buffer->effects.effects[i].data1;
        digest->effects.effects[i].data2 = buffer->effects.effects[i].data2;
        digest->effects.effects[i].data3 = buffer->effects.effects[i].data3;
    }

    digest->input.read_idx = buffer->input.read_idx;
    digest->input.display_idx = buffer->input.display_idx;
    digest->input.write_idx = buffer->input.write_idx;
    digest->input.net_idx = buffer->input.net_idx;
    digest->input.p1_buffer_crc = CalcCRC32(buffer->input.p1_buffer, sizeof(buffer->input.p1_buffer));
    digest->input.p2_buffer_crc = CalcCRC32(buffer->input.p2_buffer, sizeof(buffer->input.p2_buffer));
    digest->input.p1_state_crc = CalcCRC32(buffer->input.p1_state, sizeof(buffer->input.p1_state));
    digest->input.p2_state_crc = CalcCRC32(buffer->input.p2_state, sizeof(buffer->input.p2_state));
    digest->input.p1_history_crc = CalcCRC32(buffer->input.p1_history, sizeof(buffer->input.p1_history));
    digest->input.p2_history_crc = CalcCRC32(buffer->input.p2_history, sizeof(buffer->input.p2_history));
    digest->input.p1_input = buffer->input.p1_input;
    digest->input.p2_input = buffer->input.p2_input;

    auto buildEntityDigest = [](CompactEntity_t* out, const uint8_t* entityBytes) {
        if (!out || !entityBytes) return;
        memset(out, 0, sizeof(*out));
        memcpy(out->core, entityBytes + ENTITY_CORE_START, sizeof(out->core));
        memcpy(out->input_buffer, entityBytes + ENTITY_INPUT_START, sizeof(out->input_buffer));
        memcpy(out->action, entityBytes + ENTITY_ACTION_START, sizeof(out->action));
        memcpy(out->state_a, entityBytes + ENTITY_STATE_A_START, sizeof(out->state_a));
        memcpy(out->state_b, entityBytes + ENTITY_STATE_B_START, sizeof(out->state_b));
        memcpy(out->combat, entityBytes + ENTITY_COMBAT_START, sizeof(out->combat));
        memcpy(out->timer, entityBytes + ENTITY_TIMER_START, sizeof(out->timer));
        memcpy(out->hitstun, entityBytes + ENTITY_OFF_HITSTUN, sizeof(out->hitstun));
        memcpy(out->char_state, entityBytes + ENTITY_CHAR_STATE_START, sizeof(out->char_state));
    };

    buildEntityDigest(&digest->p1, buffer->p1_entity);
    buildEntityDigest(&digest->p2, buffer->p2_entity);

    for (int i = 0; i < SUMMON_MAX_SLOTS; i++) {
        const Summon_t* s = &buffer->summons.summons[i];
        digest->summons[i].owner_id = s->owner_id;
        digest->summons[i].summon_type = s->summon_type;
        digest->summons[i].state_flag = s->state_flag;
        digest->summons[i].sub_flag = s->sub_flag;
        digest->summons[i].state_flag2 = s->state_flag2;
        digest->summons[i].action_id = s->action_id;
        digest->summons[i].x_pos = s->x_pos;
        digest->summons[i].y_pos = s->y_pos;
        digest->summons[i].facing = s->facing;
        digest->summons[i].parent_id = s->parent_id;
        digest->summons[i].anim_frame = s->anim_frame;
        digest->summons[i].anim_state = s->anim_state;
    }
}

uint32_t AS2_ComputeCompactStateChecksum(const CompactSaveState_t* buffer) {
    if (!buffer) {
        return 0;
    }

    DeterministicDigest_t digest;
    AS2_BuildDigest(buffer, &digest);

    // Zero out the ENTIRE input section except frame-counter indices.
    //
    // WHY: The input section contains multiple non-deterministic components:
    //  - p1/p2_buffer_crc: per-button device arrays from local controller polling
    //  - p1/p2_state_crc: input processing state (hold durations, transitions)
    //  - p1/p2_history_crc: accumulated combined bitmasks per frame — these
    //    include PREDICTED inputs that haven't been corrected yet. After rollback,
    //    only the re-simulated frames get corrected; earlier entries retain the
    //    predicted values from the original forward sim.
    //  - p1/p2_input: current combined bitmask at write_idx — may be a prediction
    //    on one peer and real input on the other.
    //
    // The frame-counter indices (read_idx, display_idx, write_idx, net_idx) ARE
    // deterministic and verify the frame stepping bookkeeping is in sync.
    //
    // The actual simulation state — entities, RNG, match context, effects —
    // is already checksummed through the other digest sections. If those match,
    // the game is in sync regardless of input buffer contents.
    digest.input.p1_buffer_crc = 0;
    digest.input.p2_buffer_crc = 0;
    digest.input.p1_state_crc = 0;
    digest.input.p2_state_crc = 0;
    digest.input.p1_history_crc = 0;
    digest.input.p2_history_crc = 0;
    digest.input.p1_input = 0;
    digest.input.p2_input = 0;

    return CalcCRC32(&digest, sizeof(digest));
}

void AS2_LogDesyncState(int netplayFrame) {
    LOG_ERROR("[DESYNC DUMP] === NetplayFrame %d ===", netplayFrame);
    
    CompactSaveState_t tempState;
    AS2_CaptureCurrentState(&tempState);
    
    DeterministicDigest_t digest;
    AS2_BuildDigest(&tempState, &digest);
    
    uint32_t checksum = CalcCRC32(&digest, sizeof(digest));
    LOG_ERROR("[DESYNC DUMP] Checksum: 0x%08X", checksum);
    
    LOG_ERROR("[DESYNC DUMP] RNG: 0x%08X", digest.rng_seed);
    LOG_ERROR("[DESYNC DUMP] Global: mode=%u sub=%u type=%u", 
        digest.global.game_mode, digest.global.sub_state, digest.global.game_type);
    LOG_ERROR("[DESYNC DUMP] Match: timer=[%d,%d] wins=[%u,%u] combo=[%u,%u]",
        digest.match.round_timer[0], digest.match.round_timer[1],
        digest.match.win_count[0], digest.match.win_count[1],
        digest.match.combo_count[0], digest.match.combo_count[1]);
        
    // Log Entity Details
    auto logEntity = [](const char* label, const CompactEntity_t& e) {
        LOG_ERROR("[DESYNC DUMP] %s Core: %02X %02X %02X %02X %02X %02X %02X %02X", label,
            e.core[0], e.core[1], e.core[2], e.core[3], e.core[4], e.core[5], e.core[6], e.core[7]);
        // Log action/anim from action block (assuming standard offsets within action block)
        // Action block is 128 bytes.
        // We can just hash the blocks to see if they differ.
        uint32_t crc_core = CalcCRC32(e.core, sizeof(e.core));
        uint32_t crc_action = CalcCRC32(e.action, sizeof(e.action));
        uint32_t crc_state_a = CalcCRC32(e.state_a, sizeof(e.state_a));
        uint32_t crc_state_b = CalcCRC32(e.state_b, sizeof(e.state_b));
        uint32_t crc_combat = CalcCRC32(e.combat, sizeof(e.combat));
        uint32_t crc_timer = CalcCRC32(e.timer, sizeof(e.timer));
        uint32_t crc_hitstun = CalcCRC32(e.hitstun, sizeof(e.hitstun));
        uint32_t crc_char = CalcCRC32(e.char_state, sizeof(e.char_state));
        
        LOG_ERROR("[DESYNC DUMP] %s CRCs: Core=%08X Act=%08X StA=%08X StB=%08X Cmb=%08X Tmr=%08X Hit=%08X Char=%08X",
            label, crc_core, crc_action, crc_state_a, crc_state_b, crc_combat, crc_timer, crc_hitstun, crc_char);
            
        // Log hitstun specifically as it's a common desync source
        LOG_ERROR("[DESYNC DUMP] %s Hitstun: %02X %02X %02X %02X", 
            label, e.hitstun[0], e.hitstun[1], e.hitstun[2], e.hitstun[3]);
    };
    
    logEntity("P1", digest.p1);
    logEntity("P2", digest.p2);
    
    // Log Effects
    LOG_ERROR("[DESYNC DUMP] Effects Index: %u", digest.effects.effect_index);
    for (int i = 0; i < EFFECT_MAX_SLOTS; i++) {
        if (digest.effects.effects[i].type != 0) {
            const auto& e = digest.effects.effects[i];
            LOG_ERROR("[DESYNC DUMP] Effect[%d]: type=%u pos=(%d,%d) timer=%d vel=(%d,%d) data=(%d,%d,%d)",
                i, e.type, e.x_pos, e.y_pos, e.timer, e.vel_x, e.vel_y, e.data1, e.data2, e.data3);
        }
    }
    
    // Log Summons
    for (int i = 0; i < SUMMON_MAX_SLOTS; i++) {
        if (digest.summons[i].summon_type != 0) {
            const auto& s = digest.summons[i];
            LOG_ERROR("[DESYNC DUMP] Summon[%d]: owner=%u type=%u state=%u act=%u pos=(%d,%d) anim=%u",
                i, s.owner_id, s.summon_type, s.state_flag, s.action_id, s.x_pos, s.y_pos, s.anim_frame);
        }
    }
}

void AS2_LogDigestBreakdown(const CompactSaveState_t* buffer, int frame) {
    if (!buffer) return;
    
    DeterministicDigest_t digest;
    AS2_BuildDigest(buffer, &digest);

    // Compute checksum with the same exclusions as AS2_ComputeCompactStateChecksum
    DeterministicDigest_t checksumDigest = digest;
    checksumDigest.input.p1_buffer_crc = 0;
    checksumDigest.input.p2_buffer_crc = 0;
    checksumDigest.input.p1_state_crc = 0;
    checksumDigest.input.p2_state_crc = 0;
    checksumDigest.input.p1_history_crc = 0;
    checksumDigest.input.p2_history_crc = 0;
    checksumDigest.input.p1_input = 0;
    checksumDigest.input.p2_input = 0;
    uint32_t checksum = CalcCRC32(&checksumDigest, sizeof(checksumDigest));
    
    // Per-section CRCs: these can be compared between host and client logs to
    // pinpoint exactly which digest field diverges and causes the checksum mismatch
    uint32_t rng_crc = CalcCRC32(&digest.rng_seed, sizeof(digest.rng_seed));
    uint32_t global_crc = CalcCRC32(&digest.global, sizeof(digest.global));
    uint32_t match_crc = CalcCRC32(&digest.match, sizeof(digest.match));
    uint32_t effects_crc = CalcCRC32(&digest.effects, sizeof(digest.effects));
    uint32_t input_crc = CalcCRC32(&checksumDigest.input, sizeof(checksumDigest.input));
    uint32_t p1_crc = CalcCRC32(&digest.p1, sizeof(digest.p1));
    uint32_t p2_crc = CalcCRC32(&digest.p2, sizeof(digest.p2));
    uint32_t summon_crc = CalcCRC32(&digest.summons, sizeof(digest.summons));
    
    LOG_INFO("[DigestBreakdown] frame=%d checksum=0x%08X fpu=0x%08X rng=0x%08X global=0x%08X "
             "match=0x%08X eff=0x%08X inp=0x%08X p1=0x%08X p2=0x%08X sum=0x%08X "
             "mcx=0x%08X pmg=0x%08X",
             frame, checksum,
             digest.fpu_crc, rng_crc, global_crc,
             match_crc, effects_crc, input_crc,
             p1_crc, p2_crc, summon_crc,
             digest.match.match_context_crc,
             digest.match.pre_match_gap_crc);

    // Per-subfield input breakdown: indices, history CRCs, current bitmask, and
    // device-level buffer/state CRCs (excluded from checksum but shown for debug)
    LOG_INFO("[DigestBreakdown] frame=%d inp_detail: idx=[%u/%u/%u/%u] "
             "hist_crc=[0x%08X/0x%08X] cur=[0x%04X/0x%04X] "
             "buf_crc=[0x%08X/0x%08X] st_crc=[0x%08X/0x%08X]",
             frame,
             digest.input.read_idx, digest.input.display_idx,
             digest.input.write_idx, digest.input.net_idx,
             digest.input.p1_history_crc, digest.input.p2_history_crc,
             digest.input.p1_input, digest.input.p2_input,
             digest.input.p1_buffer_crc, digest.input.p2_buffer_crc,
             digest.input.p1_state_crc, digest.input.p2_state_crc);
}

template <typename T>
static T ReadStateValue(const uint8_t* base, size_t offset) {
    T value{};
    if (!base) {
        return value;
    }
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

static int CountActiveEffects(const CompactSaveState_t* buffer) {
    if (!buffer) {
        return 0;
    }

    int active = 0;
    for (int i = 0; i < EFFECT_MAX_SLOTS; i++) {
        active += (buffer->effects.effects[i].type != 0);
    }
    return active;
}

static int CountActiveSummons(const CompactSaveState_t* buffer) {
    if (!buffer) {
        return 0;
    }

    int active = 0;
    for (int i = 0; i < SUMMON_MAX_SLOTS; i++) {
        active += (buffer->summons.summons[i].summon_type != 0);
    }
    return active;
}

static uint16_t ComputeQuickChecksumFromState(const CompactSaveState_t* buffer) {
    if (!buffer) {
        return 0;
    }

    const uint8_t* p1 = buffer->p1_entity;
    const uint8_t* p2 = buffer->p2_entity;

    const uint16_t p1HP = ReadStateValue<uint16_t>(p1, ENTITY_OFF_HP);
    const uint16_t p1Meter = ReadStateValue<uint16_t>(p1, ENTITY_OFF_METER);
    const int16_t p1X = ReadStateValue<int16_t>(p1, ENTITY_OFF_X_POS);
    const int16_t p1Y = ReadStateValue<int16_t>(p1, ENTITY_OFF_Y_POS);
    const uint16_t p2HP = ReadStateValue<uint16_t>(p2, ENTITY_OFF_HP);
    const uint16_t p2Meter = ReadStateValue<uint16_t>(p2, ENTITY_OFF_METER);
    const int16_t p2X = ReadStateValue<int16_t>(p2, ENTITY_OFF_X_POS);
    const int16_t p2Y = ReadStateValue<int16_t>(p2, ENTITY_OFF_Y_POS);

    const uint32_t sum = (uint32_t)p1HP + (uint32_t)p1Meter + (uint32_t)(uint16_t)p1X + (uint32_t)(uint16_t)p1Y
                       + (uint32_t)p2HP + (uint32_t)p2Meter + (uint32_t)(uint16_t)p2X + (uint32_t)(uint16_t)p2Y;
    return (uint16_t)(sum % 0x10000);
}

bool AS2_FormatCompactStateSummary(const CompactSaveState_t* buffer, char* out, size_t outSize) {
    if (!buffer || !out || outSize == 0) {
        return false;
    }

    const uint8_t* p1 = buffer->p1_entity;
    const uint8_t* p2 = buffer->p2_entity;

    const uint16_t p1HP = ReadStateValue<uint16_t>(p1, ENTITY_OFF_HP);
    const uint16_t p1Meter = ReadStateValue<uint16_t>(p1, ENTITY_OFF_METER);
    const int16_t p1X = ReadStateValue<int16_t>(p1, ENTITY_OFF_X_POS);
    const int16_t p1Y = ReadStateValue<int16_t>(p1, ENTITY_OFF_Y_POS);
    const uint8_t p1Facing = ReadStateValue<uint8_t>(p1, ENTITY_OFF_FACING);
    const int8_t  p1Push = ReadStateValue<int8_t>(p1, ENTITY_OFF_PUSH_DIR);
    const uint32_t p1Action = ReadStateValue<uint32_t>(p1, ENTITY_OFF_ACTION_ID);
    const uint16_t p1Anim = ReadStateValue<uint16_t>(p1, ENTITY_OFF_ANIMATION);

    const uint16_t p2HP = ReadStateValue<uint16_t>(p2, ENTITY_OFF_HP);
    const uint16_t p2Meter = ReadStateValue<uint16_t>(p2, ENTITY_OFF_METER);
    const int16_t p2X = ReadStateValue<int16_t>(p2, ENTITY_OFF_X_POS);
    const int16_t p2Y = ReadStateValue<int16_t>(p2, ENTITY_OFF_Y_POS);
    const uint8_t p2Facing = ReadStateValue<uint8_t>(p2, ENTITY_OFF_FACING);
    const int8_t  p2Push = ReadStateValue<int8_t>(p2, ENTITY_OFF_PUSH_DIR);
    const uint32_t p2Action = ReadStateValue<uint32_t>(p2, ENTITY_OFF_ACTION_ID);
    const uint16_t p2Anim = ReadStateValue<uint16_t>(p2, ENTITY_OFF_ANIMATION);

    const int activeEffects = CountActiveEffects(buffer);
    const int activeSummons = CountActiveSummons(buffer);
    const uint16_t quickChecksum = ComputeQuickChecksumFromState(buffer);

    snprintf(out, outSize,
             "frame=%u qsum=0x%04X rng=0x%08X vrng=0x%08X mode=%u sub=%u type=%u sim=%u timer=%d fade=%u wins=%u/%u combo=%u/%u fx=%d/%u sum=%d in=%u/%u/%u/%u cur=0x%04X/0x%04X mh=%02X%02X%02X%02X p1=%u/%u@(%d,%d) f=%u push=%d act=%u anim=%u p2=%u/%u@(%d,%d) f=%u push=%d act=%u anim=%u",
             buffer->frame_number,
             quickChecksum,
             buffer->rng_seed,
             buffer->visual_rng_seed,
             buffer->global.game_mode,
             buffer->global.sub_state,
             buffer->global.game_type,
             buffer->global.frame_counter,
             buffer->match.round_timer[0],
             buffer->match.intro_fade_timer,
             buffer->match.win_count[0],
             buffer->match.win_count[1],
             buffer->match.combo_count[0],
             buffer->match.combo_count[1],
             activeEffects,
             buffer->effects.effect_index,
             activeSummons,
             buffer->input.read_idx,
             buffer->input.display_idx,
             buffer->input.write_idx,
             buffer->input.net_idx,
             buffer->input.p1_input,
             buffer->input.p2_input,
             buffer->match.match_header[0],
             buffer->match.match_header[1],
             buffer->match.match_header[2],
             buffer->match.match_header[3],
             p1HP,
             p1Meter,
             p1X,
             p1Y,
             p1Facing,
             (int)p1Push,
             p1Action,
             p1Anim,
             p2HP,
             p2Meter,
             p2X,
             p2Y,
             p2Facing,
             (int)p2Push,
             p2Action,
             p2Anim);
    out[outSize - 1] = '\0';
    return true;
}

static bool AS2_FormatLiveStateSummary(char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }

    CompactSaveState_t currentState = {};
    AS2_CaptureCurrentState(&currentState);
    return AS2_FormatCompactStateSummary(&currentState, out, outSize);
}

// ============================================================================
// Periodic State Logging (detailed but not spammy)
// ============================================================================

void AS2_LogPeriodicState(int netplayFrame, bool force) {
    // Log every 60 frames (1 second at 60fps) or when forced
    static int s_lastLogFrame = -60;

    const int LOG_INTERVAL = 60;

    if (!force && (netplayFrame - s_lastLogFrame) < LOG_INTERVAL) {
        return;
    }
    s_lastLogFrame = netplayFrame;

    __try {
        // Get quick checksum for fast overview
        uint16_t quickSum = AS2_GetQuickChecksum();

        // Read entity positions/HP
        uintptr_t p1Base = GetEntityBase(0);
        uintptr_t p2Base = GetEntityBase(1);

        uint16_t p1HP = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_HP);
        uint16_t p1Meter = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_METER);
        int16_t p1X = ReadMemory<int16_t>(p1Base + ENTITY_OFF_X_POS);
        int16_t p1Y = ReadMemory<int16_t>(p1Base + ENTITY_OFF_Y_POS);

        uint16_t p2HP = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_HP);
        uint16_t p2Meter = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_METER);
        int16_t p2X = ReadMemory<int16_t>(p2Base + ENTITY_OFF_X_POS);
        int16_t p2Y = ReadMemory<int16_t>(p2Base + ENTITY_OFF_Y_POS);

        // Read RNG state (use our managed RNG seed)
        uint32_t rng = AS2_GetRngSeed();

        // Read effect index
        uint32_t effectIdx = 0;
        __try { effectIdx = *reinterpret_cast<uint32_t*>(ADDR_EFFECT_INDEX); } __except(1) {}

        // Read simulation frame counter
        uint32_t simFrame = 0;
        __try { simFrame = *reinterpret_cast<uint32_t*>(ADDR_SIM_FRAME_COUNTER); } __except(1) {}

        // Count active effects
        int activeEffects = 0;
        __try {
            for (int i = 0; i < EFFECT_MAX_SLOTS; i++) {
                uint8_t type = ReadMemory<uint8_t>(ADDR_EFFECT_TYPE + i * EFFECT_ENTRY_SIZE);
                if (type != 0) activeEffects++;
            }
        } __except(1) {}

        LOG_INFO("[PERIODIC] gF=%d simF=%u qSum=0x%04X rng=0x%08X fx=%u/%d",
                 netplayFrame, simFrame, quickSum, rng, activeEffects, effectIdx);
        LOG_INFO("[PERIODIC]   P1: HP=%u M=%u pos=(%d,%d)", p1HP, p1Meter, p1X, p1Y);
        LOG_INFO("[PERIODIC]   P2: HP=%u M=%u pos=(%d,%d)", p2HP, p2Meter, p2X, p2Y);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[PERIODIC] Failed to read state for frame %d", netplayFrame);
    }
}

// ============================================================================
// Clear Vanilla Netplay Buffers
// ============================================================================

void AS2_ClearVanillaNetplayBuffers() {
    LOG_INFO("[AS2] Clearing vanilla netplay buffers to prevent interference...");
    
    __try {
        // Clear vanilla frame tracking counters (not simulation counter - leave that alone)
        // ADDR_FRAME_DISPLAY, ADDR_FRAME_WRITE_IDX, ADDR_FRAME_NET_IDX, ADDR_REMOTE_FRAME
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_DISPLAY) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_WRITE_IDX) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_NET_IDX) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_REMOTE_FRAME) = 0;
        
        LOG_INFO("[AS2] Cleared vanilla frame counters");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla frame counters");
    }
    
    __try {
        // Clear local input history buffer (fill with 0xFF like vanilla does)
        memset(reinterpret_cast<void*>(ADDR_VANILLA_LOCAL_INPUTS), 0xFF, VANILLA_LOCAL_INPUT_SIZE);
        LOG_INFO("[AS2] Cleared vanilla local input history (%d bytes)", VANILLA_LOCAL_INPUT_SIZE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla local input buffer");
    }
    
    __try {
        // Clear remote input history buffer (fill with 0xFF like vanilla does)
        memset(reinterpret_cast<void*>(ADDR_VANILLA_REMOTE_INPUTS), 0xFF, VANILLA_REMOTE_INPUT_SIZE);
        LOG_INFO("[AS2] Cleared vanilla remote input history (%d bytes)", VANILLA_REMOTE_INPUT_SIZE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla remote input buffer");
    }
    
    __try {
        // Clear sync flags (10 bytes each)
        memset(reinterpret_cast<void*>(ADDR_VANILLA_SYNC_LOCAL), 0, 10);
        memset(reinterpret_cast<void*>(ADDR_VANILLA_SYNC_REMOTE), 0, 10);
        LOG_INFO("[AS2] Cleared vanilla sync flags");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla sync flags");
    }
    
    LOG_INFO("[AS2] Vanilla netplay buffer clear complete");
}

// ============================================================================
// Frame Counter Monitoring
// ============================================================================

void AS2_LogFrameCounterState(int netplayFrame, const char* context) {
    __try {
        uint32_t simFrame = *reinterpret_cast<uint32_t*>(ADDR_SIM_FRAME_COUNTER);
        uint32_t renderFrame = *reinterpret_cast<uint32_t*>(ADDR_FRAME_COUNTER);
        uint32_t vanillaDisplay = *reinterpret_cast<uint32_t*>(ADDR_FRAME_DISPLAY);
        uint32_t vanillaWrite = *reinterpret_cast<uint32_t*>(ADDR_FRAME_WRITE_IDX);
        uint32_t vanillaNet = *reinterpret_cast<uint32_t*>(ADDR_FRAME_NET_IDX);

        LOG_INFO("[FRAMES] %s: net=%d sim=%u render=%u van[disp=%u write=%u net=%u]",
                 context, netplayFrame, simFrame, renderFrame, vanillaDisplay, vanillaWrite, vanillaNet);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[FRAMES] %s: Failed to read frame counters (net=%d)", context, netplayFrame);
    }
}

uint16_t GetP1HP() {
    return ReadMemory<uint16_t>(ADDR_P1_HP_DIRECT);
}

uint16_t GetP2HP() {
    return ReadMemory<uint16_t>(ADDR_P2_HP_DIRECT);
}

uintptr_t GetEntityBase(int player) {
    if (player == 0) {
        return ADDR_P1_HP_DIRECT - ENTITY_OFF_HP;
    } else {
        return ADDR_P2_HP_DIRECT - ENTITY_OFF_HP;
    }
}

// ============================================================================
// Quick Checksum (using game's native Entity_GetPositionChecksum algorithm)
// ============================================================================
// This replicates sub_49EE60:
//   return (P1_HP + P1_Meter + P1_X + P1_Y + P2_HP + P2_Meter + P2_X + P2_Y) % 0x10000
// Offsets from entity base:
//   +176 (0xB0) = HP (uint16)
//   +180 (0xB4) = Meter (uint16)
//   +184 (0xB8) = X position (int16)
//   +186 (0xBA) = Y position (int16)
// This is a fast checksum for quick desync detection.

uint16_t AS2_GetQuickChecksum() {
    uintptr_t p1Base = GetEntityBase(0);
    uintptr_t p2Base = GetEntityBase(1);
    
    // Read P1 values
    uint16_t p1HP = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_HP);
    uint16_t p1Meter = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_METER);
    int16_t p1X = ReadMemory<int16_t>(p1Base + ENTITY_OFF_X_POS);
    int16_t p1Y = ReadMemory<int16_t>(p1Base + ENTITY_OFF_Y_POS);
    
    // Read P2 values
    uint16_t p2HP = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_HP);
    uint16_t p2Meter = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_METER);
    int16_t p2X = ReadMemory<int16_t>(p2Base + ENTITY_OFF_X_POS);
    int16_t p2Y = ReadMemory<int16_t>(p2Base + ENTITY_OFF_Y_POS);
    
    // Calculate checksum exactly like the game does
    uint32_t sum = (uint32_t)p1HP + (uint32_t)p1Meter + (uint32_t)(uint16_t)p1X + (uint32_t)(uint16_t)p1Y
                 + (uint32_t)p2HP + (uint32_t)p2Meter + (uint32_t)(uint16_t)p2X + (uint32_t)(uint16_t)p2Y;
    
    return (uint16_t)(sum % 0x10000);
}

// ============================================================================
// Savestate Implementation
// ============================================================================

bool AS2_SaveState(int slot) {
    if (slot < 0 || slot >= SAVESTATE_SLOT_COUNT) {
        LOG_ERROR("SaveState: Invalid slot %d", slot);
        return false;
    }
    
    CompactSaveState_t* state = &g_saveStates[slot];
    
    // Use the unified compact save function
    if (!AS2_SaveCompactState(state)) {
        LOG_ERROR("SaveState: Failed to save compact state to slot %d", slot);
        return false;
    }
    
    g_stateValid[slot] = true;
    LOG_INFO("Saved state to slot %d (frame %d, P1:%d P2:%d HP, size: %zu bytes)", 
             slot, state->frame_number, GetP1HP(), GetP2HP(), sizeof(CompactSaveState_t));
    
    return true;
}

bool AS2_LoadState(int slot) {
    if (slot < 0 || slot >= SAVESTATE_SLOT_COUNT) {
        LOG_ERROR("LoadState: Invalid slot %d", slot);
        return false;
    }
    
    if (!g_stateValid[slot]) {
        LOG_WARN("LoadState: Slot %d is empty", slot);
        return false;
    }
    
    CompactSaveState_t* state = &g_saveStates[slot];
    
    // Use the unified compact load function
    if (!AS2_LoadCompactState(state)) {
        LOG_ERROR("LoadState: Failed to load compact state from slot %d", slot);
        return false;
    }
    
    LOG_INFO("Loaded state from slot %d (frame %d)", slot, state->frame_number);
    return true;
}

bool AS2_GetStatePreview(int slot, StatePreview_t* preview) {
    if (slot < 0 || slot >= SAVESTATE_SLOT_COUNT || !preview || !g_stateValid[slot]) {
        return false;
    }
    
    CompactSaveState_t* state = &g_saveStates[slot];
    preview->frame_number = state->frame_number;

    const uint8_t* p1 = state->p1_entity;
    const uint8_t* p2 = state->p2_entity;

    preview->p1_hp = *(uint16_t*)(p1 + ENTITY_OFF_HP);
    preview->p2_hp = *(uint16_t*)(p2 + ENTITY_OFF_HP);
    preview->p1_meter = *(uint16_t*)(p1 + ENTITY_OFF_METER);
    preview->p2_meter = *(uint16_t*)(p2 + ENTITY_OFF_METER);
    preview->p1_x = *(int16_t*)(p1 + ENTITY_OFF_X_POS);
    preview->p1_y = *(int16_t*)(p1 + ENTITY_OFF_Y_POS);
    preview->p2_x = *(int16_t*)(p2 + ENTITY_OFF_X_POS);
    preview->p2_y = *(int16_t*)(p2 + ENTITY_OFF_Y_POS);
    preview->p1_facing = *(uint8_t*)(p1 + ENTITY_OFF_FACING);
    preview->p2_facing = *(uint8_t*)(p2 + ENTITY_OFF_FACING);

    // The preview field is named "action" historically, but we display animation/state value.
    preview->p1_action = *(uint16_t*)(p1 + ENTITY_OFF_ANIMATION);
    preview->p2_action = *(uint16_t*)(p2 + ENTITY_OFF_ANIMATION);
    
    return true;
}

bool AS2_HasState(int slot) {
    return slot >= 0 && slot < SAVESTATE_SLOT_COUNT && g_stateValid[slot];
}

// Buffer-based save/load for rollback integration
// Note: These are now just wrappers around the compact state functions
bool AS2_SaveStateToBuffer(CompactSaveState_t* buffer) {
    return AS2_SaveCompactState(buffer);
}

bool AS2_LoadStateFromBuffer(const CompactSaveState_t* buffer) {
    return AS2_LoadCompactState(buffer);
}

// ============================================================================
// ROLLBACK Savestate Implementation
// ============================================================================

bool AS2_SaveCompactState(CompactSaveState_t* buffer) {
    if (!buffer) {
        LOG_ERROR("SaveCompactState: null buffer");
        return false;
    }
    
    memset(buffer, 0, sizeof(CompactSaveState_t));
    
    // Pre-capture FPU sanity check
    AS2_ValidateFpuState("SaveCompactState-PRE");
    
    // 1. Capture raw memory state
    AS2_CaptureCurrentState(buffer);
    
    // Debug: Log action/animation state for players (sampled; otherwise this spams massively)
    if (GetVerboseLogging() && (buffer->frame_number % 300) == 0) {
        uintptr_t p1Base = GetEntityBase(0);
        uintptr_t p2Base = GetEntityBase(1);
        // Action ID at +0x44C (1100), Animation frame at +0x470 (1136)
        uint32_t p1_action = ReadMemory<uint32_t>(p1Base + 0x44C);  // Action ID
        uint32_t p1_anim = ReadMemory<uint32_t>(p1Base + 0x470);    // Animation frame (dword)
        uint32_t p2_action = ReadMemory<uint32_t>(p2Base + 0x44C);
        uint32_t p2_anim = ReadMemory<uint32_t>(p2Base + 0x470);
        LOG_DEBUG("SaveCompactState: P1 action=%d anim=%d, P2 action=%d anim=%d",
                  p1_action, p1_anim, p2_action, p2_anim);
    }
    
    // 2. Build deterministic digest and calculate checksum
    DeterministicDigest_t digest;
    AS2_BuildDigest(buffer, &digest);
    buffer->checksum = CalcCRC32(&digest, sizeof(digest));

    // 3. Diagnostic logging (preserved from original)
    const int rollbackFrame = -1;
    if (rollbackFrame >= 0 && rollbackFrame <= 20) {
        LOG_INFO("[Checksum] RollbackFrame %d (gameFrame=%d): rng=0x%08X mode=%u sub=%u type=%u",
                 rollbackFrame, buffer->frame_number, digest.rng_seed, 
                 digest.global.game_mode, digest.global.sub_state, digest.global.game_type);
        LOG_INFO("[Checksum]   match: timer=[%d,%d] fade=%u wins=[%u,%u] combo=[%u,%u]",
                 buffer->match.round_timer[0], buffer->match.round_timer[1],
                 buffer->match.intro_fade_timer,
                 buffer->match.win_count[0], buffer->match.win_count[1],
                 buffer->match.combo_count[0], buffer->match.combo_count[1]);
        LOG_INFO("[Checksum]   effects: idx=%u (includes FPU/match/input CRCs)",
                 digest.effects.effect_index);
        // Log first few bytes of entity digests to spot differences
        LOG_INFO("[Checksum]   p1_core: %02X %02X %02X %02X %02X %02X %02X %02X",
                 digest.p1.core[0], digest.p1.core[1], digest.p1.core[2], digest.p1.core[3],
                 digest.p1.core[4], digest.p1.core[5], digest.p1.core[6], digest.p1.core[7]);
        LOG_INFO("[Checksum]   p2_core: %02X %02X %02X %02X %02X %02X %02X %02X",
                 digest.p2.core[0], digest.p2.core[1], digest.p2.core[2], digest.p2.core[3],
                 digest.p2.core[4], digest.p2.core[5], digest.p2.core[6], digest.p2.core[7]);
        LOG_INFO("[Checksum]   FINAL checksum: 0x%08X (size=%zu)", buffer->checksum, sizeof(digest));
    }

    int activeSummons = CountActiveSummons(buffer);

    if (GetVerboseLogging() && (buffer->frame_number % 300) == 0) {
        char summary[512] = {};
        const char* summaryText = AS2_FormatCompactStateSummary(buffer, summary, sizeof(summary))
            ? summary
            : "summary-unavailable";
        LOG_DEBUG("SaveCompactState: checksum=0x%08X size=%zu summons=%d %s",
                  buffer->checksum, sizeof(CompactSaveState_t), activeSummons, summaryText);
    }
    return true;
}

bool AS2_LoadCompactState(const CompactSaveState_t* buffer) {
    if (!buffer) {
        LOG_ERROR("LoadCompactState: null buffer");
        return false;
    }
    
    // Pre-restore FPU check
    const bool suspiciousLoad = (buffer->global.game_mode == 0 || buffer->rng_seed == 0);
    if (GetVerboseLogging() || suspiciousLoad) {
        char currentSummary[512] = {};
        char incomingSummary[512] = {};
        const char* currentText = AS2_FormatLiveStateSummary(currentSummary, sizeof(currentSummary))
            ? currentSummary
            : "summary-unavailable";
        const char* incomingText = AS2_FormatCompactStateSummary(buffer, incomingSummary, sizeof(incomingSummary))
            ? incomingSummary
            : "summary-unavailable";
        LOG_INFO("LoadCompactState: current=%s incoming=%s", currentText, incomingText);
    }

    // Pre-restore FPU check
    AS2_ValidateFpuState("LoadCompactState-PRE");
    
    // Restore RNG state FIRST
    AS2_SetRngSeed(buffer->rng_seed);
    // Restore visual RNG seed (v2+): weather particles in match_context use visual rand();
    // without restoring this, particle state diverges between peers after rollback.
    if (buffer->format_version >= 2) {
        AS2_SetVisualRngSeed(buffer->visual_rng_seed);
    }
    
    // Restore FPU state for floating-point determinism
    if (buffer->format_version >= 1) {
        AS2_RestoreFpuState(buffer->fpu_state);
        AS2_RestoreMxcsr(&buffer->mxcsr);
    } else {
        // Legacy v0: CW at fpu_state[0..1], MXCSR at fpu_state[4..7]
        uint16_t saved_cw = *(const uint16_t*)buffer->fpu_state;
        uint16_t safe_cw = (saved_cw & 0x0F00) | 0x003F;
        uint32_t saved_mxcsr = *(const uint32_t*)(buffer->fpu_state + 4);
        uint32_t safe_mxcsr = (saved_mxcsr & 0x00006000) | 0x00001F80;
        __asm {
            fldcw word ptr [safe_cw]
            ldmxcsr dword ptr [safe_mxcsr]
        }
    }
    
    // Post-restore FPU check
    AS2_ValidateFpuState("LoadCompactState-POST");
    
    // Restore global state
    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, buffer->global.frame_counter);
    WriteMemory<uint32_t>(ADDR_GAME_MODE, buffer->global.game_mode);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, buffer->global.sub_state);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, buffer->global.sub_state_timer);
    WriteMemory<uint32_t>(ADDR_GAME_TYPE, buffer->global.game_type);
    
    // Restore full player entity states
    uintptr_t p1Base = GetEntityBase(0);
    uintptr_t p2Base = GetEntityBase(1);
    WriteMemoryBlockSafe((void*)p1Base, buffer->p1_entity, ENTITY_SIZE);
    WriteMemoryBlockSafe((void*)p2Base, buffer->p2_entity, ENTITY_SIZE);
    
    // Debug: Log action/animation state after restore (sampled)
    if (GetVerboseLogging()) {
        // Action ID at +0x44C (1100), Animation frame at +0x470 (1136)
        uint32_t p1_action = ReadMemory<uint32_t>(p1Base + 0x44C);  // Action ID
        uint32_t p1_anim = ReadMemory<uint32_t>(p1Base + 0x470);    // Animation frame
        uint32_t p2_action = ReadMemory<uint32_t>(p2Base + 0x44C);
        uint32_t p2_anim = ReadMemory<uint32_t>(p2Base + 0x470);
        LOG_DEBUG("LoadCompactState: P1 action=%d anim=%d, P2 action=%d anim=%d",
                  p1_action, p1_anim, p2_action, p2_anim);
    }
    
    // Restore match state
    // NOTE: Only restore the actual single values - [1] indices don't exist in game memory
    WriteMemory<int32_t>(ADDR_ROUND_TIMER, buffer->match.round_timer[0]);
    // Don't write round_timer[1] - there's no second timer at ADDR_ROUND_TIMER+4
    WriteMemory<uint16_t>(ADDR_WIN_COUNT, buffer->match.win_count[0]);
    // Don't write win_count[1] - word_790E54 is not P2 win count
    WriteMemory<uint8_t>(ADDR_COMBO_COUNT, buffer->match.combo_count[0]);
    // Don't write combo_count[1] - byte_790E57 is unrelated data
    WriteMemory<uint32_t>(ADDR_MATCH_INTRO_FADE_TIMER, buffer->match.intro_fade_timer);
    
    // Restore match header state (critical for round end detection)
    WriteMemoryBlockSafe((void*)ADDR_MATCH_BASE, buffer->match.match_header, 16);
    
    // Restore match context gap: camera scroll, screen shake, weather particles, misc state.
    // Without this, weather particles call rand() differently on rollback → RNG desync.
    WriteMemoryBlockSafe((void*)ADDR_MATCH_CONTEXT, buffer->match_context, MATCH_CONTEXT_SIZE);
    
    // Restore pre-match gap (render blend, audio SE channel, padding)
    WriteMemoryBlockSafe((void*)ADDR_PRE_MATCH_GAP, buffer->pre_match_gap, PRE_MATCH_GAP_SIZE);
    
    // Restore effects — raw memcpy (matches the save approach)
    WriteMemory<uint32_t>(ADDR_EFFECT_INDEX, buffer->effects.effect_index);
    WriteMemoryBlockSafe((void*)ADDR_EFFECT_ARRAY, &buffer->effects.effects[0], EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE);
    
    // Restore input system state
    WriteMemory<uint32_t>(ADDR_INPUT_READ_IDX, buffer->input.read_idx);
    WriteMemory<uint32_t>(ADDR_INPUT_DISPLAY_IDX, buffer->input.display_idx);
    WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, buffer->input.write_idx);
    WriteMemory<uint32_t>(ADDR_INPUT_NET_IDX, buffer->input.net_idx);
    
    WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_BUFFER, buffer->input.p1_buffer, INPUT_BUFFER_SIZE);
    WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_BUFFER, buffer->input.p2_buffer, INPUT_BUFFER_SIZE);
    WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_STATE, buffer->input.p1_state, INPUT_STATE_SIZE);
    WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_STATE, buffer->input.p2_state, INPUT_STATE_SIZE);
    
    // Restore input history
    uint32_t historyStart = (buffer->input.write_idx >= INPUT_HISTORY_WINDOW) 
                          ? (buffer->input.write_idx - INPUT_HISTORY_WINDOW) : 0;
    // Clear the restored history window to avoid stale inputs beyond the snapshot.
    {
        uint16_t zeroHistory[INPUT_HISTORY_WINDOW] = {0};
        WriteMemoryBlockSafe((void*)(ADDR_P1_INPUT_HISTORY + historyStart * 2), zeroHistory, sizeof(zeroHistory));
        WriteMemoryBlockSafe((void*)(ADDR_P2_INPUT_HISTORY + historyStart * 2), zeroHistory, sizeof(zeroHistory));
    }
    for (int i = 0; i < INPUT_HISTORY_WINDOW; i++) {
        uint32_t frameIdx = historyStart + i;
        WriteMemory<uint16_t>(ADDR_P1_INPUT_HISTORY + frameIdx * 2, buffer->input.p1_history[i]);
        WriteMemory<uint16_t>(ADDR_P2_INPUT_HISTORY + frameIdx * 2, buffer->input.p2_history[i]);
    }
    WriteMemory<uint16_t>(ADDR_P1_INPUT_HISTORY + buffer->input.write_idx * 2, buffer->input.p1_input);
    WriteMemory<uint16_t>(ADDR_P2_INPUT_HISTORY + buffer->input.write_idx * 2, buffer->input.p2_input);

    // Restore summons: write back the full 100-slot array
    WriteMemoryBlockSafe((void*)ADDR_SUMMON_ARRAY, &buffer->summons.summons[0], SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE);

    int activeSummons = CountActiveSummons(buffer);

    LOG_DEBUG("LoadCompactState: frame %d restored (%d active summons)", buffer->frame_number, activeSummons);

    if (GetVerboseLogging() || suspiciousLoad) {
        char restoredSummary[512] = {};
        const char* restoredText = AS2_FormatLiveStateSummary(restoredSummary, sizeof(restoredSummary))
            ? restoredSummary
            : "summary-unavailable";
        LOG_INFO("LoadCompactState: restored=%s", restoredText);
    }
    
    return true;
}

// ============================================================================
// Fast Save/Load for Rollback Hot Path
// ============================================================================
// These skip the expensive operations that AS2_SaveCompactState / AS2_LoadCompactState
// perform but that are unnecessary during GekkoNet event handling:
//   Save: skips memset(245KB), BuildDigest, CalcCRC32
//   Load: batches VirtualProtect into a single unprotect/reprotect pair,
//         replaces 40 individual WriteMemory<uint16_t> with a single memcpy

static void AS2_SaveStateFastInternal(CompactSaveState_t* buffer,
                                      const char* traceScope,
                                      int traceFrame,
                                      uint32_t traceEventId) {
    if (!buffer) return;

    LARGE_INTEGER start = {};
    LARGE_INTEGER end = {};
    QueryPerformanceCounter(&start);

    // Pre-capture FPU sanity check
    AS2_ValidateFpuState("SaveStateFast-PRE");

    // Capture raw memory state — this fills every meaningful field,
    // so the full memset(0) that AS2_SaveCompactState does is unnecessary.
    AS2_CaptureCurrentState(buffer);

    // The rollback session may normalize counters after capture before
    // finalizing the checksum, so leave checksum unset here and let the
    // caller recompute it on the finalized state if needed.
    buffer->checksum = 0;

    QueryPerformanceCounter(&end);
    const int saveUs = (int)(RollbackTraceElapsedUs(start, end) + 0.5);
    if (traceScope && traceScope[0] && traceEventId != 0 && AS2_GetRollbackTraceLogging()) {
        const int effectiveFrame = (traceFrame >= 0) ? traceFrame : (int)buffer->frame_number;
        AS2_LogRollbackTraceMessage(traceScope,
                                    effectiveFrame,
                                    traceEventId,
                                    "save-fast complete saved_frame=%u rng=0x%08X save_us=%d",
                                    buffer->frame_number,
                                    buffer->rng_seed,
                                    saveUs);
        AS2_DumpCompactStateTrace(traceScope, effectiveFrame, traceEventId, buffer);
    }
}

static void AS2_LoadStateFastInternal(const CompactSaveState_t* buffer,
                                      const char* traceScope,
                                      int traceFrame,
                                      uint32_t traceEventId) {
    if (!buffer) return;

    const bool doTrace = traceScope && traceScope[0] && traceEventId != 0 && AS2_GetRollbackTraceLogging();
    const int effectiveFrame = (traceFrame >= 0) ? traceFrame : (int)buffer->frame_number;

    LARGE_INTEGER start = {};
    LARGE_INTEGER end = {};
    QueryPerformanceCounter(&start);

    if (doTrace) {
        char summary[512] = {};
        if (AS2_FormatCompactStateSummary(buffer, summary, sizeof(summary))) {
            AS2_LogRollbackTraceMessage(traceScope, effectiveFrame, traceEventId, "load-fast incoming %s", summary);
        }
    }

    // Pre-restore FPU check
    AS2_ValidateFpuState("LoadStateFast-PRE");

    // Restore RNG state FIRST
    AS2_SetRngSeed(buffer->rng_seed);
    // Restore visual RNG seed (v2+): weather particles in match_context use visual rand();
    // without restoring this, particle state diverges between peers after rollback.
    if (buffer->format_version >= 2) {
        AS2_SetVisualRngSeed(buffer->visual_rng_seed);
    }

    // Restore FPU state for floating-point determinism
    if (buffer->format_version >= 1) {
        // Version 1+: full FRSTOR from 108-byte FSAVE image, then MXCSR separately
        AS2_RestoreFpuState(buffer->fpu_state);
        AS2_RestoreMxcsr(&buffer->mxcsr);
    } else {
        // Version 0 (legacy): fpu_state[0..1] = CW, fpu_state[4..7] = MXCSR (old layout)
        // Restore CW via FLDCW and MXCSR via LDMXCSR with safety masks
        uint16_t saved_cw = *(const uint16_t*)buffer->fpu_state;
        uint16_t safe_cw = (saved_cw & 0x0F00) | 0x003F;
        uint32_t saved_mxcsr = *(const uint32_t*)(buffer->fpu_state + 4);
        uint32_t safe_mxcsr = (saved_mxcsr & 0x00006000) | 0x00001F80;
        __asm {
            fldcw word ptr [safe_cw]
            ldmxcsr dword ptr [safe_mxcsr]
        }
        LOG_DEBUG("[FPU] Legacy v0 restore: CW=0x%04X->0x%04X MXCSR=0x%08X->0x%08X",
                  saved_cw, safe_cw, saved_mxcsr, safe_mxcsr);
    }

    // Post-restore FPU check
    AS2_ValidateFpuState("LoadStateFast-POST");

    uintptr_t p1Base = GetEntityBase(0);
    uintptr_t p2Base = GetEntityBase(1);

    // Region sizes for VirtualProtect
    struct RegionInfo { void* addr; size_t size; DWORD oldProtect; };
    RegionInfo regions[] = {
        { (void*)ADDR_GAME_MODE, (ADDR_INPUT_NET_IDX + 4) - ADDR_GAME_MODE, 0 },
        { (void*)p1Base, ENTITY_SIZE, 0 },
        { (void*)p2Base, ENTITY_SIZE, 0 },
        { (void*)ADDR_PRE_MATCH_GAP, (ADDR_SUMMON_ARRAY + SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE) - ADDR_PRE_MATCH_GAP, 0 },
        { (void*)ADDR_P1_INPUT_BUFFER, (ADDR_P2_INPUT_BUFFER + INPUT_BUFFER_SIZE) - ADDR_P1_INPUT_BUFFER, 0 },
        { (void*)ADDR_P1_INPUT_HISTORY, INPUT_HISTORY_P1_SIZE, 0 },
        { (void*)ADDR_P2_INPUT_HISTORY, INPUT_HISTORY_P2_SIZE, 0 },
    };
    constexpr int kRegionCount = sizeof(regions) / sizeof(regions[0]);

    for (int i = 0; i < kRegionCount; i++) {
        VirtualProtect(regions[i].addr, regions[i].size, PAGE_EXECUTE_READWRITE, &regions[i].oldProtect);
    }

    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "global.frame_counter", ADDR_SIM_FRAME_COUNTER, buffer->global.frame_counter, doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "global.game_mode", ADDR_GAME_MODE, buffer->global.game_mode, doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "global.sub_state", ADDR_SUB_STATE, buffer->global.sub_state, doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "global.sub_state_timer", ADDR_SUB_STATE_TIMER, buffer->global.sub_state_timer, doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "global.game_type", ADDR_GAME_TYPE, buffer->global.game_type, doTrace);

    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p1_entity", p1Base, buffer->p1_entity, ENTITY_SIZE, doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p2_entity", p2Base, buffer->p2_entity, ENTITY_SIZE, doTrace);

    TraceScalarWriteInternal<int32_t>(traceScope, effectiveFrame, traceEventId, "match.round_timer", ADDR_ROUND_TIMER, buffer->match.round_timer[0], doTrace);
    TraceScalarWriteInternal<uint16_t>(traceScope, effectiveFrame, traceEventId, "match.win_count", ADDR_WIN_COUNT, buffer->match.win_count[0], doTrace);
    TraceScalarWriteInternal<uint8_t>(traceScope, effectiveFrame, traceEventId, "match.combo_count", ADDR_COMBO_COUNT, buffer->match.combo_count[0], doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "match.intro_fade_timer", ADDR_MATCH_INTRO_FADE_TIMER, buffer->match.intro_fade_timer, doTrace);

    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "match_header", ADDR_MATCH_BASE, buffer->match.match_header, 16, doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "match_context", ADDR_MATCH_CONTEXT, buffer->match_context, MATCH_CONTEXT_SIZE, doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "pre_match_gap", ADDR_PRE_MATCH_GAP, buffer->pre_match_gap, PRE_MATCH_GAP_SIZE, doTrace);

    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "effects_index", ADDR_EFFECT_INDEX, buffer->effects.effect_index, doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "effects_array", ADDR_EFFECT_ARRAY, &buffer->effects.effects[0], EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE, doTrace);

    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "input.read_idx", ADDR_INPUT_READ_IDX, buffer->input.read_idx, doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "input.display_idx", ADDR_INPUT_DISPLAY_IDX, buffer->input.display_idx, doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "input.write_idx", ADDR_INPUT_WRITE_IDX, buffer->input.write_idx, doTrace);
    TraceScalarWriteInternal<uint32_t>(traceScope, effectiveFrame, traceEventId, "input.net_idx", ADDR_INPUT_NET_IDX, buffer->input.net_idx, doTrace);

    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p1_input_buffer", ADDR_P1_INPUT_BUFFER, buffer->input.p1_buffer, INPUT_BUFFER_SIZE, doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p2_input_buffer", ADDR_P2_INPUT_BUFFER, buffer->input.p2_buffer, INPUT_BUFFER_SIZE, doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p1_input_state", ADDR_P1_INPUT_STATE, buffer->input.p1_state, INPUT_STATE_SIZE, doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p2_input_state", ADDR_P2_INPUT_STATE, buffer->input.p2_state, INPUT_STATE_SIZE, doTrace);

    uint32_t historyStart = (buffer->input.write_idx >= INPUT_HISTORY_WINDOW)
                          ? (buffer->input.write_idx - INPUT_HISTORY_WINDOW) : 0;
    const uintptr_t p1HistoryAddr = ADDR_P1_INPUT_HISTORY + historyStart * sizeof(uint16_t);
    const uintptr_t p2HistoryAddr = ADDR_P2_INPUT_HISTORY + historyStart * sizeof(uint16_t);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p1_input_history_window", p1HistoryAddr, buffer->input.p1_history, INPUT_HISTORY_WINDOW * sizeof(uint16_t), doTrace);
    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "p2_input_history_window", p2HistoryAddr, buffer->input.p2_history, INPUT_HISTORY_WINDOW * sizeof(uint16_t), doTrace);
    TraceScalarWriteInternal<uint16_t>(traceScope, effectiveFrame, traceEventId, "p1_input_current", ADDR_P1_INPUT_HISTORY + buffer->input.write_idx * sizeof(uint16_t), buffer->input.p1_input, doTrace);
    TraceScalarWriteInternal<uint16_t>(traceScope, effectiveFrame, traceEventId, "p2_input_current", ADDR_P2_INPUT_HISTORY + buffer->input.write_idx * sizeof(uint16_t), buffer->input.p2_input, doTrace);

    TraceBlockWriteInternal(traceScope, effectiveFrame, traceEventId, "summons", ADDR_SUMMON_ARRAY, &buffer->summons.summons[0], SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE, doTrace);

    for (int i = 0; i < kRegionCount; i++) {
        DWORD dummy;
        VirtualProtect(regions[i].addr, regions[i].size, regions[i].oldProtect, &dummy);
    }

    QueryPerformanceCounter(&end);
    const int loadUs = (int)(RollbackTraceElapsedUs(start, end) + 0.5);
    if (doTrace) {
        AS2_LogRollbackTraceMessage(traceScope,
                                    effectiveFrame,
                                    traceEventId,
                                    "load-fast complete saved_frame=%u rng=0x%08X load_us=%d",
                                    buffer->frame_number,
                                    buffer->rng_seed,
                                    loadUs);
    }
}

void AS2_SaveStateFast(CompactSaveState_t* buffer) {
    AS2_SaveStateFastInternal(buffer, nullptr, -1, 0);
}

void AS2_LoadStateFast(const CompactSaveState_t* buffer) {
    AS2_LoadStateFastInternal(buffer, nullptr, -1, 0);
}

void AS2_SaveStateFastTrace(CompactSaveState_t* buffer, const char* traceScope, int traceFrame, uint32_t traceEventId) {
    AS2_SaveStateFastInternal(buffer, traceScope, traceFrame, traceEventId);
}

void AS2_LoadStateFastTrace(const CompactSaveState_t* buffer, const char* traceScope, int traceFrame, uint32_t traceEventId) {
    AS2_LoadStateFastInternal(buffer, traceScope, traceFrame, traceEventId);
}

// ============================================================================
// Rollback
// ============================================================================
// Clean-slate: rollback netcode has been removed. Savestates remain.

// ============================================================================
// Input Hooks - Intercept game's input reading to inject our SDL input
// ============================================================================

// Debug counter to verify hooks are being called
static int g_hookCallCount = 0;
static int g_lastLoggedCount = 0;

// Frame-based input update tracking
static int g_lastInputUpdateFrame = -1;

// Ensure SDL input is updated once per frame (called early from hooks)
static void EnsureInputUpdated() {
    int currentFrame = ReadMemory<int>(ADDR_SIM_FRAME_COUNTER);
    if (currentFrame != g_lastInputUpdateFrame) {
        InputSystem_Update();
        g_lastInputUpdateFrame = currentFrame;
    }
    // Note: "Just pressed" flags are now written in Hook_InputProcess() AFTER
    // the game's input processing completes. This is the proper fix for menus.
}

// Keyboard scancode to INPUT_* mapping table
// This maps DirectInput scancodes to our internal input flags
// The game reads keyboard config from 0x9D0AD0 - each player has 32 buttons x 4 alt keys
// But we'll use a simpler approach: map the default scancodes P1 uses
static uint16_t ScanCodeToInputFlag(int scancode) {
    // Default P1 keyboard layout from sub_62FF50:
    // Arrow keys for directions
    switch (scancode) {
        case 0xC8: return INPUT_UP;     // DIK_UP (200)
        case 0xD0: return INPUT_DOWN;   // DIK_DOWN (208)
        case 0xCB: return INPUT_LEFT;   // DIK_LEFT (203)
        case 0xCD: return INPUT_RIGHT;  // DIK_RIGHT (205)
        
        // NumPad directions
        case 72:  return INPUT_UP;     // DIK_NUMPAD8
        case 80:  return INPUT_DOWN;   // DIK_NUMPAD2
        case 75:  return INPUT_LEFT;   // DIK_NUMPAD4
        case 77:  return INPUT_RIGHT;  // DIK_NUMPAD6
        
        // Default face buttons: Z X C A S D
        case 0x2C: return INPUT_A;     // DIK_Z (44) - A/Light
        case 0x2D: return INPUT_B;     // DIK_X (45) - B/Medium
        case 0x2E: return INPUT_C;     // DIK_C (46) - C/Heavy
        case 0x1E: return INPUT_D;     // DIK_A (30) - D/Special
        
        // Additional buttons
        case 0x1F: return INPUT_L1;    // DIK_S (31)
        case 0x20: return INPUT_R1;    // DIK_D (32)
        case 0x10: return INPUT_L2;    // DIK_Q (16)
        case 0x11: return INPUT_R2;    // DIK_W (17)
        
        // Start/Select
        // NOTE: Escape is used as a cancel/back key in menus and can trigger exit
        // from some screens. Do NOT map it to INPUT_START.
        case 0x01: return INPUT_SELECT; // DIK_ESCAPE (1)
        case 0x1C: return INPUT_START;  // DIK_RETURN (28)
        case 0x39: return INPUT_SELECT; // DIK_SPACE (57)
        case 0x0E: return INPUT_SELECT; // DIK_BACKSPACE (14)
        
        default: return 0;
    }
}

// Detailed debug tracking
static struct InputDebugInfo {
    // Hook tracking
    int keyboardHookCalls;
    int joystickHookCalls;
    int lastKeyCode;
    int lastPlayerID;
    int lastOrigResult;
    int lastFinalResult;
    
    // Input injection tracking
    int keyboardInjectedCount;   // Times we returned 1 from keyboard hook
    int joystickInjectedCount;   // Times we added input in joystick hook
    uint16_t lastInjectedKeyInput;   // Last input injected via keyboard hook
    uint16_t lastInjectedJoyInput;   // Last input injected via joystick hook
    
    // Player mapping debug
    int p1JoyID;
    int p2JoyID;
    int lastMappedPlayer;
    
    // SDL input
    uint16_t sdlInputP1;
    uint16_t sdlInputP2;
    uint16_t sdlInputP1Raw;      // Before SOCD cleaning
    uint16_t sdlInputP2Raw;
    
    // Converted input
    uint16_t gameInputP1;
    uint16_t gameInputP2;
    
    // Game buffer values (read from actual buffers)
    uint16_t gameBufferP1[10];  // Down, Up, Left, Right, A, B, C, D, Start, Select
    uint16_t gameBufferP2[10];
    
    // DirectInput buffer values
    int32_t dinputJoyAxisX;
    int32_t dinputJoyAxisY;
    uint8_t dinputJoyButtons[8];
    
    // Keyboard state (partial - just arrow keys and typical buttons)
    uint8_t keyState_Up;
    uint8_t keyState_Down;
    uint8_t keyState_Left;
    uint8_t keyState_Right;
    uint8_t keyState_Z;
    uint8_t keyState_X;
    uint8_t keyState_Enter;
} g_inputDebug = {0};

// Convert our INPUT_* format to game's joystick bitmask format
// INPUT_*:  UP=0x01, DOWN=0x02, LEFT=0x04, RIGHT=0x08, A=0x10, B=0x20...
// Game joy from sub_62FF50:
//   Y-axis negative → bit 3 (0x08) = Up
//   Y-axis positive → bit 0 (0x01) = Down  
//   X-axis negative → bit 1 (0x02) = Left
//   X-axis positive → bit 2 (0x04) = Right
//   Buttons: bit 4+ = button 0+
// The game's default button masks map these to:
//   Up=0x08, Down=0x01, Left=0x02, Right=0x04
uint16_t ConvertToGameJoyFormat(uint16_t input) {
    uint16_t result = 0;
    
    // Directions (need remapping from INPUT_* to game's joystick bitmask)
    if (input & INPUT_UP)    result |= 0x0008;  // Game: bit 3
    if (input & INPUT_DOWN)  result |= 0x0001;  // Game: bit 0
    if (input & INPUT_LEFT)  result |= 0x0002;  // Game: bit 1
    if (input & INPUT_RIGHT) result |= 0x0004;  // Game: bit 2
    
    // Buttons (bits 4+ match, just copy)
    result |= (input & 0xFFF0);  // A, B, C, D, Start, Select, etc.
    
    return result;
}

// ============================================================================
// DirectInput Buffer Injection Hooks
// These are MUCH cleaner than hooking every state check - we hook the buffer
// refresh functions and inject our input AFTER the game fills the buffer.
// ============================================================================

// Map SDL scancode to DirectInput scancode
static uint8_t SDLScancodeToDIK(int sdlScancode) {
    // SDL3 scancodes to DirectInput DIK codes
    switch (sdlScancode) {
        // Letters (DIK_A=0x1E through DIK_Z)
        case 4:  return 0x1E;  // A
        case 5:  return 0x30;  // B
        case 6:  return 0x2E;  // C
        case 7:  return 0x20;  // D
        case 8:  return 0x12;  // E
        case 9:  return 0x21;  // F
        case 10: return 0x22;  // G
        case 11: return 0x23;  // H
        case 12: return 0x17;  // I
        case 13: return 0x24;  // J
        case 14: return 0x25;  // K
        case 15: return 0x26;  // L
        case 16: return 0x32;  // M
        case 17: return 0x31;  // N
        case 18: return 0x18;  // O
        case 19: return 0x19;  // P
        case 20: return 0x10;  // Q
        case 21: return 0x13;  // R
        case 22: return 0x1F;  // S
        case 23: return 0x14;  // T
        case 24: return 0x16;  // U
        case 25: return 0x2F;  // V
        case 26: return 0x11;  // W
        case 27: return 0x2D;  // X
        case 28: return 0x15;  // Y
        case 29: return 0x2C;  // Z
        
        // Numbers
        case 30: return 0x02;  // 1
        case 31: return 0x03;  // 2
        case 32: return 0x04;  // 3
        case 33: return 0x05;  // 4
        case 34: return 0x06;  // 5
        case 35: return 0x07;  // 6
        case 36: return 0x08;  // 7
        case 37: return 0x09;  // 8
        case 38: return 0x0A;  // 9
        case 39: return 0x0B;  // 0
        
        // Special keys
        case 40: return 0x1C;  // Return
        case 41: return 0x01;  // Escape
        case 42: return 0x0E;  // Backspace
        case 43: return 0x0F;  // Tab
        case 44: return 0x39;  // Space
        
        // Arrow keys
        case 79: return 0xCD;  // Right (DIK_RIGHT=205)
        case 80: return 0xCB;  // Left (DIK_LEFT=203)
        case 81: return 0xD0;  // Down (DIK_DOWN=208)
        case 82: return 0xC8;  // Up (DIK_UP=200)
        
        // Numpad
        case 89: return 0x4F;  // Numpad 1
        case 90: return 0x50;  // Numpad 2
        case 91: return 0x51;  // Numpad 3
        case 92: return 0x4B;  // Numpad 4
        case 93: return 0x4C;  // Numpad 5
        case 94: return 0x4D;  // Numpad 6
        case 95: return 0x47;  // Numpad 7
        case 96: return 0x48;  // Numpad 8
        case 97: return 0x49;  // Numpad 9
        case 98: return 0x52;  // Numpad 0
        case 88: return 0x9C;  // Numpad Enter
        
        default: return 0;
    }
}

// Inject our SDL input into the DirectInput keyboard buffer
static void InjectKeyboardInput() {
    if (!g_config.useSDLInput) return;
    
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    
    // Get our bindings and check which keys should be pressed
    // We inject P1's keyboard bindings into the DirectInput buffer
    const PlayerBindings_t* bindings = InputSystem_GetBindings(0);
    if (!bindings) return;
    
    // Get current SDL input for P1
    uint16_t input = InputSystem_GetInput(0);
    
    // Map each binding to DirectInput scancode and inject if pressed
    auto injectKey = [&](const KeyBinding_t* binding, uint16_t flag) {
        if (binding->keyboard_key > 0 && (input & flag)) {
            uint8_t dik = SDLScancodeToDIK(binding->keyboard_key);
            if (dik > 0) {
                keyBuffer[dik] |= 0x80;  // Set bit 7 = pressed
            }
        }
    };
    
    injectKey(&bindings->up,    INPUT_UP);
    injectKey(&bindings->down,  INPUT_DOWN);
    injectKey(&bindings->left,  INPUT_LEFT);
    injectKey(&bindings->right, INPUT_RIGHT);
    injectKey(&bindings->a,     INPUT_A);
    injectKey(&bindings->b,     INPUT_B);
    injectKey(&bindings->c,     INPUT_C);
    injectKey(&bindings->d,     INPUT_D);
    injectKey(&bindings->start, INPUT_START);
    injectKey(&bindings->select,INPUT_SELECT);
    injectKey(&bindings->l1,    INPUT_L1);
    injectKey(&bindings->r1,    INPUT_R1);
}

// Helper: Check if game window is focused (by process ID, not window title)
static bool IsGameWindowFocused() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return (foregroundPid == GetCurrentProcessId());
}

// Hook for Win32 GetKeyboardState - prevents Alt+Shift language switching issue
// The game calls GetKeyboardState even when DirectInput is disabled
BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    if (InputSystem_IsBindingActive() && lpKeyState) {
        memset(lpKeyState, 0, 256);
        return TRUE;
    }

    // When using SDL input, provide an SDL-derived keyboard state.
    // This keeps menus controllable even when native keyboard polling is disabled,
    // while still preventing Alt+Shift language switching (we never set Alt/Shift).
    if (g_config.useSDLInput && lpKeyState) {
        memset(lpKeyState, 0, 256);

        // Keep SDL state up-to-date.
        EnsureInputUpdated();

        const uint16_t input = InputSystem_GetInput(0);
        auto setPressed = [&](int vk, bool pressed) {
            if (vk < 0 || vk >= 256) return;
            lpKeyState[vk] = pressed ? 0x80 : 0x00;
        };

        // Directions
        setPressed(VK_UP,    (input & INPUT_UP) != 0);
        setPressed(VK_DOWN,  (input & INPUT_DOWN) != 0);
        setPressed(VK_LEFT,  (input & INPUT_LEFT) != 0);
        setPressed(VK_RIGHT, (input & INPUT_RIGHT) != 0);

        // Common default buttons (based on ScanCodeToInputFlag defaults)
        setPressed('Z', (input & INPUT_A) != 0);
        setPressed('X', (input & INPUT_B) != 0);
        setPressed('C', (input & INPUT_C) != 0);
        setPressed('A', (input & INPUT_D) != 0);
        setPressed('S', (input & INPUT_L1) != 0);
        setPressed('D', (input & INPUT_R1) != 0);
        setPressed('Q', (input & INPUT_L2) != 0);
        setPressed('W', (input & INPUT_R2) != 0);

        // Start/Select
        setPressed(VK_RETURN, (input & INPUT_START) != 0);
        // Escape is cancel/back (Select), not Start.
        setPressed(VK_ESCAPE, (input & INPUT_SELECT) != 0);
        setPressed(VK_SPACE,  (input & INPUT_SELECT) != 0);
        setPressed(VK_BACK,   (input & INPUT_SELECT) != 0);

        return TRUE;
    }
    
    // Otherwise call the original
    return g_origGetKeyboardState(lpKeyState);
}

// ============================================================================
// RNG Hooks for Deterministic Rollback
// ============================================================================
// MSVC CRT rand() uses a simple Linear Congruential Generator (LCG):
//   seed = seed * 214013 + 2531011
//   return (seed >> 16) & 0x7fff
// We hook srand() and rand() to track the seed for save/restore.
//
// Note: Analysis of the game shows srand(time(0)) is called ONCE at game start
// in sub_5D29D0. Replays do NOT sync RNG - they just store inputs. For rollback
// we need to capture and restore the RNG seed state ourselves.

// Important: Not every rand() call is simulation-critical.
// The game uses rand() for some presentation-only systems (eg. camera/FX table updates).
// If those calls share the same RNG stream as gameplay, rollback re-sim can diverge simply
// because a purely visual system consumed a different number of random numbers.
//
// To keep rollback safe while avoiding giant "save everything" savestates, we split RNG into:
// - Simulation RNG: saved/restored via savestate.
// - Visual RNG: not saved/restored; allowed to diverge without affecting gameplay.

#include <intrin.h>

static uint32_t g_simRandCallCount = 0;
static uint32_t g_visRandCallCount = 0;
static bool g_extraVerboseRngLogging = false;  // Enable during critical startup phase

static inline bool IsVisualRandCaller(uintptr_t returnAddress) {
    // Mode 8 camera/FX system uses rand() in these functions:
    // - 0x004C4040 sub_4C4040 (init)
    // - 0x004C4230 sub_4C4230 (camera target update)
    // - 0x004C4480 sub_4C4480 (camera/FX table update)
    // Treat calls originating from these ranges as visual-only.
    const uintptr_t camInitStart   = (GAME_BASE + 0x0C4040);
    const uintptr_t camInitEnd     = camInitStart + 0x0500;
    const uintptr_t camTargetStart = (GAME_BASE + 0x0C4230);
    const uintptr_t camTargetEnd   = camTargetStart + 0x0800;
    const uintptr_t fxUpdateStart  = (GAME_BASE + 0x0C4480);
    const uintptr_t fxUpdateEnd    = fxUpdateStart + 0x2000;

    return (returnAddress >= camInitStart   && returnAddress < camInitEnd) ||
           (returnAddress >= camTargetStart && returnAddress < camTargetEnd) ||
           (returnAddress >= fxUpdateStart  && returnAddress < fxUpdateEnd);
}

void __cdecl Hook_srand(unsigned int seed) {
    // Initialize both streams to the same seed.
    g_currentRngSeed = seed;
    g_visualRngSeed = seed;
    g_simRandCallCount = 0;
    g_visRandCallCount = 0;
    // Log at INFO since srand is only called once at game start
    LOG_INFO("[RNG] srand(%u) - seed initialized", seed);
    // Call original to maintain normal behavior
    if (g_orig_srand) {
        g_orig_srand(seed);
    }
}

int __cdecl Hook_rand() {
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    const bool isVisual = IsVisualRandCaller(ra);

#ifdef AS2_RNG_AUDIT
    if (AS2_RngAudit_IsActive()) {
        AS2_RngAudit_RecordCall(ra, isVisual);
    }
#endif

    if (isVisual) {
        g_visualRngSeed = g_visualRngSeed * 214013 + 2531011;
        int result = (g_visualRngSeed >> 16) & 0x7fff;
        g_visRandCallCount++;
        if (g_visRandCallCount <= 3) {
            LOG_DEBUG("[RNG] rand(VIS) #%u: result=%d, seed=0x%08X, ra=0x%08X",
                      g_visRandCallCount, result, g_visualRngSeed, (uint32_t)ra);
        }
        return result;
    }

    g_currentRngSeed = g_currentRngSeed * 214013 + 2531011;
    int result = (g_currentRngSeed >> 16) & 0x7fff;
    g_simRandCallCount++;
    if (g_extraVerboseRngLogging || g_simRandCallCount <= 10) {
        LOG_INFO("[RNG-SIM] Call #%u: result=%d, newSeed=0x%08X, caller=0x%08X",
                  g_simRandCallCount, result, g_currentRngSeed, (uint32_t)ra);
    } else if ((g_simRandCallCount % 1000) == 0) {
        LOG_DEBUG("[RNG] rand(SIM) #%u: result=%d, seed=0x%08X, ra=0x%08X",
                  g_simRandCallCount, result, g_currentRngSeed, (uint32_t)ra);
    }
    return result;
}

// Get current RNG seed (for savestate)
uint32_t AS2_GetRngSeed() {
    return g_currentRngSeed;
}

// Enable extra verbose RNG logging (for debugging desyncs)
void AS2_SetExtraVerboseRngLogging(bool enable) {
    g_extraVerboseRngLogging = enable;
    if (enable) {
        LOG_INFO("[RNG] Extra verbose logging ENABLED");
    }
}

// ============================================================================
// RNG Audit Mode
// ============================================================================
// Records every rand() call site (by return address) during a configurable
// window, then dumps a summary showing which callers hit the SIM stream vs
// the VIS stream, plus any callers that didn't match either known set.
// Use to validate the visual/simulation classification is correct.

#ifdef AS2_RNG_AUDIT

struct RngCallRecord {
    uintptr_t return_address;  // _ReturnAddress() from Hook_rand
    uint32_t  sim_count;       // times this caller hit the SIM stream
    uint32_t  vis_count;       // times this caller hit the VIS stream
};

static constexpr size_t kRngAuditMaxCallers = 256;
static RngCallRecord   s_rngAuditLog[kRngAuditMaxCallers];
static size_t          s_rngAuditCount = 0;
static bool            s_rngAuditActive = false;
static uint32_t        s_rngAuditStartFrame = 0;
static uint32_t        s_rngAuditFrameLimit = 600;  // default: ~10 seconds at 60fps

static RngCallRecord* RngAudit_FindOrAdd(uintptr_t ra) {
    for (size_t i = 0; i < s_rngAuditCount; ++i) {
        if (s_rngAuditLog[i].return_address == ra) return &s_rngAuditLog[i];
    }
    if (s_rngAuditCount >= kRngAuditMaxCallers) return nullptr;
    RngCallRecord* r = &s_rngAuditLog[s_rngAuditCount++];
    r->return_address = ra;
    r->sim_count = 0;
    r->vis_count = 0;
    return r;
}

void AS2_RngAudit_RecordCall(uintptr_t ra, bool isVisual) {
    if (!s_rngAuditActive) return;
    RngCallRecord* r = RngAudit_FindOrAdd(ra);
    if (!r) return;  // table full
    if (isVisual) r->vis_count++;
    else          r->sim_count++;
}

void AS2_RngAudit_Begin(uint32_t frameLimitOverride) {
    s_rngAuditCount = 0;
    memset(s_rngAuditLog, 0, sizeof(s_rngAuditLog));
    s_rngAuditStartFrame = AS2_GetFrameNumber();
    s_rngAuditFrameLimit = (frameLimitOverride > 0) ? frameLimitOverride : 600;
    s_rngAuditActive = true;
    LOG_INFO("[RNG-AUDIT] Started at frame %u, will run for %u frames",
             s_rngAuditStartFrame, s_rngAuditFrameLimit);
}

void AS2_RngAudit_End() {
    s_rngAuditActive = false;
    LOG_INFO("[RNG-AUDIT] Ended. Recorded %zu unique callers.", s_rngAuditCount);
}

bool AS2_RngAudit_IsActive() {
    if (!s_rngAuditActive) return false;
    uint32_t elapsed = AS2_GetFrameNumber() - s_rngAuditStartFrame;
    if (elapsed >= s_rngAuditFrameLimit) {
        AS2_RngAudit_End();
        return false;
    }
    return true;
}

void AS2_RngAudit_DumpToLog() {
    LOG_INFO("[RNG-AUDIT] === DUMP: %zu unique callers ===", s_rngAuditCount);
    uint32_t unclassifiedCount = 0;
    for (size_t i = 0; i < s_rngAuditCount; ++i) {
        const RngCallRecord& r = s_rngAuditLog[i];
        const char* classification;
        if (r.sim_count > 0 && r.vis_count > 0) {
            classification = "MIXED?!";  // Should never happen
            unclassifiedCount++;
        } else if (r.sim_count > 0) {
            classification = "SIM";
        } else {
            classification = "VIS";
        }
        LOG_INFO("[RNG-AUDIT]   [%s] ra=0x%08X sim=%u vis=%u",
                 classification, (uint32_t)r.return_address, r.sim_count, r.vis_count);
    }
    if (unclassifiedCount > 0) {
        LOG_ERROR("[RNG-AUDIT] WARNING: %u callers hit BOTH streams! Classification may be wrong.",
                  unclassifiedCount);
    } else {
        LOG_INFO("[RNG-AUDIT] All callers correctly classified (single-stream only).");
    }
}

uint32_t AS2_RngAudit_CheckForUnclassified() {
    uint32_t count = 0;
    for (size_t i = 0; i < s_rngAuditCount; ++i) {
        if (s_rngAuditLog[i].sim_count > 0 && s_rngAuditLog[i].vis_count > 0) {
            count++;
        }
    }
    return count;
}

#endif // AS2_RNG_AUDIT

// Synchronize tick baseline for rollback netplay
void AS2_SyncTickBaseline(uint32_t baseline) {
    g_syncedTickBaseline = baseline;
    g_tickBaselineSynced = true;
    g_timeWarpBaseReal = 0;
    g_timeWarpBaseFake = 0;
    g_timeWarpLastScale = 1.0f;
    
    // Also write synchronized tick to game's stored tick (dword_816360)
    const uintptr_t ADDR_LAST_FRAME_TICK = 0x816360;
    WriteMemory<uint32_t>(ADDR_LAST_FRAME_TICK, baseline);
    
    LOG_INFO("[Timing] Tick baseline synchronized to %u (game tick also reset)", baseline);
}

void AS2_ClearTickBaselineSync() {
    if (!g_tickBaselineSynced && g_syncedTickBaseline == 0) {
        return;
    }

    g_syncedTickBaseline = 0;
    g_tickBaselineSynced = false;
    g_timeWarpBaseReal = 0;
    g_timeWarpBaseFake = 0;
    g_timeWarpLastScale = 1.0f;

    const uintptr_t ADDR_LAST_FRAME_TICK = 0x816360;
    const uint32_t realTick = g_origGetTick ? g_origGetTick() : (GetTickCount() & 0x7FFFFFFF);
    WriteMemory<uint32_t>(ADDR_LAST_FRAME_TICK, realTick);

    LOG_INFO("[Timing] Tick baseline sync cleared (game tick restored to %u)", realTick);
}

// Set RNG seed (for loadstate) 
void AS2_SetRngSeed(uint32_t seed) {
    uint32_t oldSeed = g_currentRngSeed;
    g_currentRngSeed = seed;
    uint32_t prevSimCount = g_simRandCallCount;
    g_simRandCallCount = 0;

    // Also set the actual CRT seed via srand
    if (g_orig_srand) {
        g_orig_srand(seed);
    }

    // Detailed diagnostics for seed restore
    if (seed == 0) {
        LOG_ERROR("[RNG] WARNING: Seed restored to ZERO! 0x%08X -> 0x00000000 (simCalls=%u before reset). "
                  "Seed=0 means LCG output is deterministic but may indicate uninitialized state!",
                  oldSeed, prevSimCount);
    } else if (oldSeed == seed) {
        LOG_DEBUG("[RNG] Seed restored (unchanged): 0x%08X (simCalls=%u reset)", seed, prevSimCount);
    } else {
        LOG_INFO("[RNG] Seed restored: 0x%08X -> 0x%08X (simCalls=%u reset)", oldSeed, seed, prevSimCount);
    }
}

// Get RNG diagnostic stats for crash reporting
void AS2_GetRngStats(uint32_t* outSimSeed, uint32_t* outVisSeed, 
                     uint32_t* outSimCalls, uint32_t* outVisCalls) {
    if (outSimSeed)  *outSimSeed  = g_currentRngSeed;
    if (outVisSeed)  *outVisSeed  = g_visualRngSeed;
    if (outSimCalls) *outSimCalls = g_simRandCallCount;
    if (outVisCalls) *outVisCalls = g_visRandCallCount;
}

// Get/set visual RNG seed for savestate/loadstate.
// Weather_UpdateParticles (0x4C4480) uses visual rand() but modifies match_context
// state (particle positions at match+1868). Must be saved/restored during rollback
// so particle state is deterministic across peers.
uint32_t AS2_GetVisualRngSeed() {
    return g_visualRngSeed;
}

void AS2_SetVisualRngSeed(uint32_t seed) {
    uint32_t old = g_visualRngSeed;
    g_visualRngSeed = seed;
    g_visRandCallCount = 0;
    if (old != seed) {
        LOG_DEBUG("[RNG] Visual seed restored: 0x%08X -> 0x%08X", old, seed);
    }
}

// Hook for ChangeDisplaySettingsA - prevent exclusive fullscreen mode changes
// When borderless fullscreen is enabled, we block resolution changes to keep
// the game in windowed mode (the d3d9 proxy handles making it borderless)
LONG WINAPI Hook_ChangeDisplaySettings(DEVMODEA* lpDevMode, DWORD dwFlags) {
    if (g_forceBorderlessFullscreen) {
        // When lpDevMode is NULL, it's a request to reset to default display mode
        // We allow this as it happens during cleanup
        if (lpDevMode == nullptr) {
            LOG_INFO("ChangeDisplaySettings: Allowing reset to default mode");
            return g_origChangeDisplaySettings(lpDevMode, dwFlags);
        }
        
        // Block the resolution change - game stays in windowed mode
        LOG_INFO("ChangeDisplaySettings: Blocked mode change to %ux%u (borderless mode active)",
                lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight);
        return DISP_CHANGE_SUCCESSFUL;  // Pretend it succeeded
    }
    
    return g_origChangeDisplaySettings(lpDevMode, dwFlags);
}

// Hook for sub_630130 - DirectInput keyboard buffer refresh
int __cdecl Hook_DInputKBRefresh() {
    // Ensure SDL input is updated this frame
    EnsureInputUpdated();
    
    // When using SDL input, DON'T call original DirectInput function
    // This prevents DirectInput from polling keyboard, which interferes
    // with Windows system hotkeys like Alt+Shift (language switching)
    if (g_config.useSDLInput) {
        // IMPORTANT: Clear the game's keyboard buffer to prevent any stale keys
        // The buffer is 256 bytes at ADDR_DINPUT_KEYBOARD (byte_9D09CC)
        uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
        memset(keyBuffer, 0, 256);
        return 0;  // Skip DirectInput keyboard polling entirely
    }
    
    // Only call original when NOT using SDL input
    return g_origDInputKBRefresh();
}

// Hook for sub_6302F0 - DirectInput joystick buffer refresh
int __cdecl Hook_DInputJoyRefresh(int joyID) {
    // Ensure SDL input is updated this frame
    EnsureInputUpdated();
    
    // When using SDL input, DON'T call original DirectInput function
    // This prevents DirectInput from polling joysticks, which can interfere
    // with Windows system functionality and override our SOCD-cleaned input
    if (g_config.useSDLInput) {
        // IMPORTANT: Clear ALL joystick buffers to prevent stale data
        // The game might call this with different joyID values
        for (int i = 0; i < DINPUT_JOY_MAX; i++) {
            uintptr_t joyBase = ADDR_DINPUT_JOYSTICK + (i * DINPUT_JOY_STRUCT_SIZE);
            // Clear X axis (offset 0) - negative = left, positive = right
            WriteMemory<int32_t>(joyBase + 0, 0);
            // Clear Y axis (offset 4) - negative = up, positive = down  
            WriteMemory<int32_t>(joyBase + 4, 0);
            // Clear button states (offset 64, 24 buttons)
            for (int j = 0; j < 24; j++) {
                WriteMemory<uint8_t>(joyBase + DINPUT_JOY_BTN_OFFSET + j, 0);
            }
        }
        return 0;  // Skip DirectInput joystick polling entirely
    }
    
    // Only call original when NOT using SDL input
    int result = g_origDInputJoyRefresh(joyID);
    
    // Extract the actual joystick index from joyID
    // joyID format: 0x1000 = keyboard fallback, otherwise (joyID & 0xFFF) - 1 = joystick index
    if (joyID & 0x1000) {
        // This is keyboard fallback mode, not a real joystick
        // Don't write to joystick buffer
        return result;
    }
    
    int joyIndex = (joyID & 0xFFF) - 1;
    if (joyIndex < 0 || joyIndex >= DINPUT_JOY_MAX) {
        // joyID=0 is common (means no joystick), skip silently
        return result;
    }
    
    // Determine which player this joystick corresponds to
    static const uintptr_t P1_JOY_ID_ADDR = 0x816358 + 864440;  // 0x8E9E60
    static const uintptr_t P2_JOY_ID_ADDR = 0x816358 + 864484;  // 0x8E9E8C
    int p1JoyID = ReadMemory<int>(P1_JOY_ID_ADDR);
    int p2JoyID = ReadMemory<int>(P2_JOY_ID_ADDR);
    
    int playerIndex = -1;
    if (joyID == p1JoyID) playerIndex = 0;
    else if (joyID == p2JoyID) playerIndex = 1;
    
    // If we can't map this joystick to a player, skip injection
    if (playerIndex < 0) {
        return result;
    }
    
    // Inject our SDL input into this joystick's buffer
    // DISABLED: Investigating crash when injecting to joystick buffer
    // The crash happens when Start is pressed and P2 has a joystick configured
    /*
    if (g_config.useSDLInput) {
        uint16_t input = InputSystem_GetInput(playerIndex);
        
        // If bypassing game input, clear first then write
        if (g_config.bypassGameInput) {
            WriteJoystickInputDirect(joyIndex, 0);
        }
        
        if (input != 0 || g_config.bypassGameInput) {
            WriteJoystickInputDirect(joyIndex, input);
        }
    }
    */
    
    return result;
}

int __cdecl Hook_KeyboardState(int keyCode) {
    g_hookCallCount++;
    g_inputDebug.keyboardHookCalls++;
    g_inputDebug.lastKeyCode = keyCode;
    
    // Ensure SDL input is updated this frame
    EnsureInputUpdated();

    if (InputSystem_IsBindingActive()) {
        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = 0;
        return 0;
    }
    
    // When using SDL input, return SDL-derived state for this scancode.
    // The game expects a truthy/non-zero result when the key is down.
    if (g_config.useSDLInput) {
        const uint16_t flag = ScanCodeToInputFlag(keyCode);
        const uint16_t input = InputSystem_GetInput(0);
        const int pressed = (flag != 0 && (input & flag) != 0) ? 1 : 0;

        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = pressed;

        if (pressed) {
            g_inputDebug.keyboardInjectedCount++;
            g_inputDebug.lastInjectedKeyInput = flag;
        }
        return pressed;
    }
    
    // Only call original when NOT using SDL input
    int origResult = g_origKeyboardState(keyCode);
    g_inputDebug.lastOrigResult = origResult;
    g_inputDebug.lastFinalResult = origResult;
    return origResult;
}

int __cdecl Hook_JoystickState(int playerID) {
    g_hookCallCount++;
    g_inputDebug.joystickHookCalls++;
    g_inputDebug.lastPlayerID = playerID;
    
    // Ensure SDL input is updated this frame
    EnsureInputUpdated();

    if (InputSystem_IsBindingActive()) {
        g_inputDebug.lastOrigResult = 0;
        g_inputDebug.lastFinalResult = 0;
        return 0;
    }
    
    // When using SDL input, return an SDL-derived joystick bitmask.
    // Menus read this directly (sub_62FF50), so returning 0 breaks navigation.
    if (g_config.useSDLInput) {
        // We still need to track debug info and determine player for logging
        g_inputDebug.lastOrigResult = 0;
        
        // Read game's joystick configuration to determine which player this is
        static const uintptr_t GAME_CONFIG_BASE = 0x816358;
        static const uintptr_t P1_JOY_ID_ADDR = GAME_CONFIG_BASE + 864440;  // 0x8E9E60
        static const uintptr_t P2_JOY_ID_ADDR = GAME_CONFIG_BASE + 864484;  // 0x8E9E8C
        
        int p1JoyID = ReadMemory<int>(P1_JOY_ID_ADDR);
        int p2JoyID = ReadMemory<int>(P2_JOY_ID_ADDR);
        g_inputDebug.p1JoyID = p1JoyID;
        g_inputDebug.p2JoyID = p2JoyID;
        
        int playerIndex = (playerID == p1JoyID) ? 0 : (playerID == p2JoyID) ? 1 : 0;
        g_inputDebug.lastMappedPlayer = playerIndex;
        
        // Get our SDL input for debug tracking
        uint16_t sdlInput = InputSystem_GetInput(playerIndex);
        uint16_t gameInput = ConvertToGameJoyFormat(sdlInput);
        
        if (playerIndex == 0) {
            g_inputDebug.sdlInputP1 = sdlInput;
            g_inputDebug.gameInputP1 = gameInput;
        } else {
            g_inputDebug.sdlInputP2 = sdlInput;
            g_inputDebug.gameInputP2 = gameInput;
        }
        
        if (gameInput != 0) {
            g_inputDebug.joystickInjectedCount++;
            g_inputDebug.lastInjectedJoyInput = gameInput;
        }
        
        g_inputDebug.lastFinalResult = (int)gameInput;
        return (int)gameInput;
    }
    
    // Only call original when NOT using SDL input
    int origResult = g_origJoystickState(playerID);
    g_inputDebug.lastOrigResult = origResult;
    g_inputDebug.lastFinalResult = origResult;
    return origResult;
}

// ============================================================================
// Input Processing Hook (sub_562060)
// ============================================================================
// This hook runs AFTER the game processes input. We take FULL CONTROL of the
// input buffers here - writing both held and just-pressed for ALL buttons.
// This prevents double-triggering since we're the ONLY source of input.

// Just-pressed buffer offsets (from base 0x8E9E62 / 0x8E9F32)
// Each is 10 words (20 bytes) at base+56
#define JUST_PRESSED_OFFSET_WORDS 28  // base+56 in bytes = base+28 in words

// Alternative input buffer addresses (word_8E9E62/word_8E9F32)
// These are used by sub_562320 for reading final input state.
// IMPORTANT: These ARE the same addresses as ADDR_P1_INPUT_BUFFER / ADDR_P2_INPUT_BUFFER.
// There is no separate "alt-buffer" allocation — the name is historical from early analysis
// that suspected a second buffer. The savestate already captures this region via input.p1_buffer.
#define ADDR_P1_INPUT_BUFFER_ALT    0x8E9E62
#define ADDR_P2_INPUT_BUFFER_ALT    0x8E9F32
static_assert(ADDR_P1_INPUT_BUFFER_ALT == ADDR_P1_INPUT_BUFFER, 
              "Alt-buffer IS the main buffer — they must be the same address");
static_assert(ADDR_P2_INPUT_BUFFER_ALT == ADDR_P2_INPUT_BUFFER,
              "Alt-buffer IS the main buffer — they must be the same address");

// Button order in game buffers: Up(0), Down(1), Left(2), Right(3), A(4), B(5), C(6), D(7), Start(8), Select(9)
static const uint16_t g_buttonMasks[10] = {
    0x0001,  // Up    (INPUT_UP)
    0x0002,  // Down  (INPUT_DOWN)
    0x0004,  // Left  (INPUT_LEFT)
    0x0008,  // Right (INPUT_RIGHT)
    0x0010,  // A     (INPUT_A)
    0x0020,  // B     (INPUT_B)
    0x0040,  // C     (INPUT_C)
    0x0080,  // D     (INPUT_D)
    0x0100,  // Start (INPUT_START)
    0x0200   // Select(INPUT_SELECT)
};

static uint16_t ReadHeldMaskFromAltBuffer(uintptr_t altBufferAddr) {
    uint16_t heldMask = 0;
    __try {
        for (int i = 0; i < 10; i++) {
            const uint16_t heldVal = ReadMemory<uint16_t>(altBufferAddr + (i * 2));
            if (heldVal) {
                heldMask |= g_buttonMasks[i];
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return heldMask;
}

int __cdecl Hook_InputProcess(int gameState) {
    const bool overrideActive = AS2_IsSynchronizedInputOverrideActive();
    const bool consumeForCustomMenu = InputSystem_IsBindingActive();
    const bool loadBarrierFrozen = false;

    // Keep SDL input state fresh whenever we might use it.
    if (!overrideActive && g_config.useSDLInput) {
        EnsureInputUpdated();
    }

    // Use one edge-detection method for offline + online:
    // derive just-pressed from the previous held state stored in the alt buffers.
    // Must read this BEFORE the original function clears/rewrites buffers.
    const uint16_t prevHeldP1 = ReadHeldMaskFromAltBuffer(ADDR_P1_INPUT_BUFFER_ALT);
    const uint16_t prevHeldP2 = ReadHeldMaskFromAltBuffer(ADDR_P2_INPUT_BUFFER_ALT);

    // Call original function - this clears buffers and does game's native processing.
    // We'll overwrite the buffers after this when SDL/rollback override is enabled.
    int result = g_origInputProcess(gameState);

    auto clearLiveInputBuffers = []() {
        static const uint8_t zeroBuffer[INPUT_BUFFER_SIZE] = {};
        static const uint8_t zeroState[INPUT_STATE_SIZE] = {};
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_BUFFER, zeroBuffer, sizeof(zeroBuffer));
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_BUFFER, zeroBuffer, sizeof(zeroBuffer));
        WriteMemoryBlockSafe((void*)ADDR_P1_INPUT_STATE, zeroState, sizeof(zeroState));
        WriteMemoryBlockSafe((void*)ADDR_P2_INPUT_STATE, zeroState, sizeof(zeroState));
    };

    if (loadBarrierFrozen) {
        // The load barrier must be a true sim freeze: once bootstrap has scrubbed
        // match-start input state, neither vanilla polling nor our SDL override
        // should be able to repopulate P1/P2 buffers before the frame-0 baseline.
        clearLiveInputBuffers();
        return result;
    }

    // When the custom menu is open or a key rebind is in progress, zero all
    // per-button arrays so vanilla DirectInput values written by the original
    // function don't leak through as game input (e.g. pressing a key during
    // rebind would otherwise register as an in-game action).
    if (consumeForCustomMenu) {
        clearLiveInputBuffers();
        return result;
    }

    if ((overrideActive || g_config.useSDLInput)) {
        // During CharSel (State 6) lockstep, different substates need different handling:
        //  - Substates 2/3/4/7/11: InputDispatcher drives lockstep
        //    and writes per-button arrays. Skip ALL processing here to avoid
        //    double-consuming lockstep frames and overwriting raw buffers.
        //  - Substate 8 (StageIntro): Game reads per-button arrays directly
        //    (no InputDispatcher call). Drive lockstep here.
        //  - All other State 6 substates: animations/setup, no input needed.
        // State 7 (Stage Selection) and State 8 (Match) are separate game modes.
        // CharSelSync lockstep: disabled (rollback modules removed).
        // TODO: reimplement with new netplay modules.
#if 0  // CharSelSync removed — entire lockstep block disabled
        if (false) {
            const uint32_t mode = GetGameMode();
            const uint32_t sub  = GetSubstate();

            // Track last direct-buffer substate across all code paths.
            // Declared here (not inside the isDirectBufferSubstate block)
            // so non-direct-buffer paths can reset it — ensuring re-entry
            // after a match (rematch) always triggers edge-detection cleanup.
            static uint32_t s_lastDirectBufferSub = UINT32_MAX;

            // Substates where Hook_InputDispatcher controls lockstep — the
            // game mode handler calls Input_TryGetNextFrame internally for
            // these, so Hook_InputDispatcher handles everything.
            const bool isInputDispatcherSubstate =
                (mode == MODE_CHARSEL) &&
                (sub == CHARSEL_SUB_SELECT || sub == CHARSEL_SUB_CANCEL ||
                 sub == CHARSEL_SUB_CONFIRM);

            // Substates where the game reads per-button arrays directly
            // (no InputDispatcher call) — drive lockstep via this hook.
            //  - Sub 7 (Preview): Interactive stage grid cursor + roulette
            //  - Sub 8 (StageIntro): Stage intro animation
            //  - Sub 11 (Stage): Auto/random stage pick (uses rand())
            const bool isDirectBufferSubstate =
                (mode == MODE_CHARSEL) &&
                (sub == CHARSEL_SUB_PREVIEW ||
                 sub == CHARSEL_SUB_STAGE_INTRO ||
                 sub == CHARSEL_SUB_STAGE);

            // InputDispatcher-driven substates — skip, it handles everything
            if (isInputDispatcherSubstate) {
                s_lastDirectBufferSub = UINT32_MAX;  // Reset so re-entry triggers cleanup
                return result;
            }

            // Direct-buffer substates: drive lockstep + write per-button arrays
            if (isDirectBufferSubstate) {
                // ── Edge detection reset on substate entry ──────────────
                // During transition substates (5,6), the original InputProcess
                // fills the alt buffers with local DInput. This would give each
                // peer different prevHeld → different just-pressed on the first
                // lockstep frame. Zero the alt buffers and override prevHeld to
                // guarantee identical edge detection on both peers.
                uint16_t effectivePrevHeldP1 = prevHeldP1;
                uint16_t effectivePrevHeldP2 = prevHeldP2;
                if (sub != s_lastDirectBufferSub) {
                    effectivePrevHeldP1 = 0;
                    effectivePrevHeldP2 = 0;
                    for (int i = 0; i < 10; i++) {
                        WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER_ALT + (i * 2), 0);
                        WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER_ALT + (i * 2), 0);
                    }
                    // ── RNG sync on stage select entry ──────────────
                    // Sub 7 (roulette) and sub 11 (auto-pick) call rand().
                    // Because CRT rand() seed differs between peers (set once
                    // at startup with a time-based value), roulette and auto-pick
                    // would produce different stage selections → desync.
                    // Seed the mod's RNG identically using the shared session ID.
                    if (sub == CHARSEL_SUB_PREVIEW || sub == CHARSEL_SUB_STAGE) {
                        uint32_t rngSeed = 0x12345678;  // TODO: restore session-based RNG seed with new netplay
                        if (rngSeed == 0) rngSeed = 0x12345678;
                        AS2_SetRngSeed(rngSeed);
                        LOG_NET_INFO("[InputProcess] RNG synced for stage select (sub=%u seed=0x%08X)",
                                     sub, rngSeed);
                    }
                    LOG_NET_INFO("[InputProcess] Direct buffer substate entry: %u -> %u, edge state reset",
                                 s_lastDirectBufferSub, sub);
                    s_lastDirectBufferSub = sub;
                }

                const int localPlayer = InputSystem_GetControlSwap() ? 1 : 0;
                const uint16_t localInput = InputSystem_GetInput(localPlayer);
                CharSelSync::BufferAndSendLocalInput(localInput);

                uint16_t currentP1 = 0, currentP2 = 0;
                if (CharSelSync::HasInputsForCurrentFrame()) {
                    CharSelSync::ConsumeCurrentFrame(&currentP1, &currentP2);
                }
                // else: zeros — cursor frozen until remote input arrives

                // ── Stage select: single-controller shared cursor ──────
                // Sub 7 (Preview) and sub 11 (Stage): one player controls the
                // stage cursor. First match: host. After a match: the winner.
                // Draw preserves previous controller (host if no prior match).
                if (sub == CHARSEL_SUB_PREVIEW || sub == CHARSEL_SUB_STAGE) {
                    int lastWinner = AS2_GetLastMatchWinner();
                    int stageController = (lastWinner >= 0) ? lastWinner : 0;

                    uint16_t controllerInput = (stageController == 0) ? currentP1 : currentP2;
                    uint16_t sharedInput = CharSelSync::CanonicalizeSharedStageInput(controllerInput, 0);
                    currentP1 = sharedInput;
                    currentP2 = 0;

                    // Periodic stage select logging
                    static uint32_t s_stageSelLogCounter = 0;
                    if (sharedInput != 0 || (s_stageSelLogCounter++ % 60) == 0) {
                        const uint8_t stageId = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_STAGE_ID);
                        const uint32_t stageCursor = *reinterpret_cast<volatile uint32_t*>(ADDR_STAGE_CURSOR);
                        LOG_NET_INFO("[StageSelect-DP] host=%d sub=%u controller=P%d(winner=%d) "
                                     "shared=0x%04X stageId=%u cursor={pos=%u conf=%u roul=%u}",
                            1,  // TODO: restore host detection with new netplay
                            sub,
                            stageController + 1,
                            lastWinner,
                            sharedInput,
                            stageId,
                            (uint8_t)(stageCursor & 0xFF),
                            (uint8_t)((stageCursor >> 8) & 0xFF),
                            (uint8_t)((stageCursor >> 16) & 0xFF));
                    }
                }
                // Sub 8 (StageIntro): block START to prevent unwanted exits
                else if (sub == CHARSEL_SUB_STAGE_INTRO) {
                    currentP1 &= ~(uint16_t)INPUT_START;
                    currentP2 &= ~(uint16_t)INPUT_START;
                }

                const uint16_t pressedP1 = (uint16_t)(currentP1 & (uint16_t)~effectivePrevHeldP1);
                const uint16_t pressedP2 = (uint16_t)(currentP2 & (uint16_t)~effectivePrevHeldP2);

                for (int i = 0; i < 10; i++) {
                    const uint16_t mask = g_buttonMasks[i];
                    {
                        const uintptr_t heldAddr = ADDR_P1_INPUT_BUFFER + (i * 2);
                        const uintptr_t justPressedAddr = ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                        const uint16_t heldVal = (currentP1 & mask) ? 1 : 0;
                        const uint16_t pressedVal = (pressedP1 & mask) ? 1 : 0;
                        WriteMemory<uint16_t>(heldAddr, heldVal);
                        WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                        WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER_ALT + (i * 2), heldVal);
                    }
                    {
                        const uintptr_t heldAddr = ADDR_P2_INPUT_BUFFER + (i * 2);
                        const uintptr_t justPressedAddr = ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                        const uint16_t heldVal = (currentP2 & mask) ? 1 : 0;
                        const uint16_t pressedVal = (pressedP2 & mask) ? 1 : 0;
                        WriteMemory<uint16_t>(heldAddr, heldVal);
                        WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                        WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER_ALT + (i * 2), heldVal);
                    }
                }

                return result;
            }

            // Other State 6 substates or State 7 (Stage Selection) — no writes needed
            if (mode == MODE_CHARSEL || mode == MODE_STAGESEL) {
                s_lastDirectBufferSub = UINT32_MAX;  // Reset so re-entry triggers cleanup
                return result;
            }
        }
#endif  // CharSelSync removed

        const uint16_t allowedMask = (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT |
                                      INPUT_A | INPUT_B | INPUT_C | INPUT_D |
                                      INPUT_START | INPUT_SELECT);

        uint16_t currentP1 = 0;
        uint16_t currentP2 = 0;

        if (overrideActive) {
            currentP1 = (uint16_t)(g_syncInputOverrideP1 & allowedMask);
            currentP2 = (uint16_t)(g_syncInputOverrideP2 & allowedMask);
        } else {
            const InputState_t* p1State = InputSystem_GetState(0);
            const InputState_t* p2State = InputSystem_GetState(1);
            currentP1 = (uint16_t)((p1State ? p1State->current : 0) & allowedMask);
            currentP2 = (uint16_t)((p2State ? p2State->current : 0) & allowedMask);
        }

        // Control swap: P1 physical input drives P2 game buffer and vice versa
        if (InputSystem_GetControlSwap()) {
            const uint16_t tmp = currentP1;
            currentP1 = currentP2;
            currentP2 = tmp;
        }

        const uint16_t pressedP1 = (uint16_t)(currentP1 & (uint16_t)~prevHeldP1);
        const uint16_t pressedP2 = (uint16_t)(currentP2 & (uint16_t)~prevHeldP2);

        for (int i = 0; i < 10; i++) {
            const uint16_t mask = g_buttonMasks[i];

            // P1
            {
                const uintptr_t heldAddr = ADDR_P1_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P1_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                const uint16_t heldVal = (currentP1 & mask) ? 1 : 0;
                const uint16_t pressedVal = (pressedP1 & mask) ? 1 : 0;
                WriteMemory<uint16_t>(heldAddr, heldVal);
                WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                WriteMemory<uint16_t>(ADDR_P1_INPUT_BUFFER_ALT + (i * 2), heldVal);
            }

            // P2
            {
                const uintptr_t heldAddr = ADDR_P2_INPUT_BUFFER + (i * 2);
                const uintptr_t justPressedAddr = ADDR_P2_INPUT_BUFFER + (JUST_PRESSED_OFFSET_WORDS * 2) + (i * 2);
                const uint16_t heldVal = (currentP2 & mask) ? 1 : 0;
                const uint16_t pressedVal = (pressedP2 & mask) ? 1 : 0;
                WriteMemory<uint16_t>(heldAddr, heldVal);
                WriteMemory<uint16_t>(justPressedAddr, pressedVal);
                WriteMemory<uint16_t>(ADDR_P2_INPUT_BUFFER_ALT + (i * 2), heldVal);
            }
        }
    }

    return result;
}

// ============================================================================
// Hook Installation
// ============================================================================

bool InstallHooks() {
    LOG_INFO("Installing hooks...");
    
    // NOTE: MinHook is already initialized by d3d9_proxy for EndScene hooks
    // MH_Initialize returns MH_ERROR_ALREADY_INITIALIZED which is fine
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("MH_Initialize failed! Status: %d", status);
        return false;
    }
    
    LOG_INFO("ADDR_KEYBOARD_STATE = 0x%08X (sub_62FD00)", ADDR_KEYBOARD_STATE);
    LOG_INFO("ADDR_JOYSTICK_STATE = 0x%08X (sub_62FF50)", ADDR_JOYSTICK_STATE);
    
    // Hook keyboard state function (sub_62FD00)
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_KEYBOARD_STATE),
            reinterpret_cast<void*>(&Hook_KeyboardState),
            reinterpret_cast<void**>(&g_origKeyboardState));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook KeyboardState! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_62FD00 (keyboard state)");
    
    // Hook joystick state function (sub_62FF50)
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_JOYSTICK_STATE),
            reinterpret_cast<void*>(&Hook_JoystickState),
            reinterpret_cast<void**>(&g_origJoystickState));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook JoystickState! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_62FF50 (joystick state)");
    
    // Hook DirectInput keyboard buffer refresh (sub_630130)
    // This is called BEFORE keyboard state checks to fill the buffer
    LOG_INFO("ADDR_DINPUT_KB_REFRESH = 0x%08X (sub_630130)", ADDR_DINPUT_KB_REFRESH);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_DINPUT_KB_REFRESH),
            reinterpret_cast<void*>(&Hook_DInputKBRefresh),
            reinterpret_cast<void**>(&g_origDInputKBRefresh));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DInputKBRefresh! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_630130 (DirectInput keyboard buffer refresh)");
    }
    
    // Hook DirectInput joystick buffer refresh (sub_6302F0)
    LOG_INFO("ADDR_DINPUT_JOY_REFRESH = 0x%08X (sub_6302F0)", ADDR_DINPUT_JOY_REFRESH);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_DINPUT_JOY_REFRESH),
            reinterpret_cast<void*>(&Hook_DInputJoyRefresh),
            reinterpret_cast<void**>(&g_origDInputJoyRefresh));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DInputJoyRefresh! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_6302F0 (DirectInput joystick buffer refresh)");
    }
    
    // Hook Win32 GetKeyboardState to prevent Alt+Shift language switching issues
    // The game calls this even when DirectInput is disabled, causing Windows to 
    // detect modifier key combinations as system hotkeys
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetKeyboardState),
            reinterpret_cast<void*>(&Hook_GetKeyboardState),
            reinterpret_cast<void**>(&g_origGetKeyboardState));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetKeyboardState! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetKeyboardState (prevents Alt+Shift issues)");
    }
    
    // Japanese locale patch: Hook GetOEMCP and GetACP to return codepage 932
    // This allows the game to work correctly on non-Japanese Windows systems
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetOEMCP),
            reinterpret_cast<void*>(&Hook_GetOEMCP),
            reinterpret_cast<void**>(&g_origGetOEMCP));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetOEMCP! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetOEMCP (Japanese locale patch - returns 932)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetACP),
            reinterpret_cast<void*>(&Hook_GetACP),
            reinterpret_cast<void**>(&g_origGetACP));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetACP! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetACP (Japanese locale patch - returns 932)");
    }
    
    // Filesystem ANSI→Wide hooks: convert Shift-JIS filenames through CP932
    // so config.dat, keymaps.dat, DXLib .ini files load on non-Japanese Windows
    status = MH_CreateHook(
            reinterpret_cast<void*>(&CreateFileA),
            reinterpret_cast<void*>(&Hook_CreateFileA),
            reinterpret_cast<void**>(&g_origCreateFileA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook CreateFileA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked CreateFileA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&DeleteFileA),
            reinterpret_cast<void*>(&Hook_DeleteFileA),
            reinterpret_cast<void**>(&g_origDeleteFileA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DeleteFileA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked DeleteFileA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&FindFirstFileA),
            reinterpret_cast<void*>(&Hook_FindFirstFileA),
            reinterpret_cast<void**>(&g_origFindFirstFileA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook FindFirstFileA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked FindFirstFileA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetFileAttributesA),
            reinterpret_cast<void*>(&Hook_GetFileAttributesA),
            reinterpret_cast<void**>(&g_origGetFileAttributesA));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetFileAttributesA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked GetFileAttributesA (Shift-JIS path conversion)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&MultiByteToWideChar),
            reinterpret_cast<void*>(&Hook_MultiByteToWideChar),
            reinterpret_cast<void**>(&g_origMultiByteToWideChar));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook MultiByteToWideChar! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked MultiByteToWideChar (CP_ACP/CP_OEMCP -> CP932 redirect)");
    }
    
    // Hook srand() and rand() for deterministic RNG in rollback
    // This is CRITICAL for rollback netcode - without tracking RNG, desyncs will occur
    // CRITICAL: The game has a STATICALLY LINKED CRT - it does NOT import rand/srand from
    // any DLL! The rand() function is compiled directly into the .exe at a fixed address.
    // We found this by searching for the MSVC LCG constants (214013, 2531011) in .text section.
    // Hooking msvcrt.dll was completely ineffective - those hooks were never called.
    
    void* pSrand = reinterpret_cast<void*>(ADDR_STATIC_SRAND);
    void* pRand = reinterpret_cast<void*>(ADDR_STATIC_RAND);
    LOG_INFO("Using STATIC CRT addresses: srand=0x%08X, rand=0x%08X", ADDR_STATIC_SRAND, ADDR_STATIC_RAND);
    
    if (pSrand) {
        status = MH_CreateHook(pSrand, reinterpret_cast<void*>(&Hook_srand),
                reinterpret_cast<void**>(&g_orig_srand));
        if (status != MH_OK) {
            LOG_WARN("Failed to hook static srand! Status: %d (RNG may desync)", status);
        } else {
            LOG_INFO("Hooked STATIC CRT srand at 0x%p (RNG tracking for rollback)", pSrand);
        }
    }
    
    if (pRand) {
        status = MH_CreateHook(pRand, reinterpret_cast<void*>(&Hook_rand),
                reinterpret_cast<void**>(&g_orig_rand));
        if (status != MH_OK) {
            LOG_WARN("Failed to hook static rand! Status: %d (RNG may desync)", status);
        } else {
            LOG_INFO("Hooked STATIC CRT rand at 0x%p (deterministic RNG for rollback)", pRand);
            g_rngHooksInstalled = true;
        }
    }
    
    // Hook ChangeDisplaySettingsA to prevent exclusive fullscreen mode changes
    // This works in conjunction with the d3d9 proxy borderless fullscreen
    /* DISABLED - Conflict with d3d9_proxy's new window management
    status = MH_CreateHook(
            reinterpret_cast<void*>(&ChangeDisplaySettingsA),
            reinterpret_cast<void*>(&Hook_ChangeDisplaySettings),
            reinterpret_cast<void**>(&g_origChangeDisplaySettings));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook ChangeDisplaySettingsA! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 ChangeDisplaySettingsA (borderless fullscreen support)");
    }
    */
    
    // Hook input processing function (sub_562060)
    // This is the KEY hook for proper menu input - we add "just pressed" flags AFTER
    // the original function processes input, ensuring our input works for menus
    LOG_INFO("ADDR_INPUT_PROCESS = 0x%08X (sub_562060)", ADDR_INPUT_PROCESS);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_INPUT_PROCESS),
            reinterpret_cast<void*>(&Hook_InputProcess),
            reinterpret_cast<void**>(&g_origInputProcess));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook InputProcess! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_562060 (input processing - just pressed flags)");

    // Hook tick function (sub_635F80) used by the main loop frame limiter.
    // By scaling its output we can temporarily accelerate the frame limiter for rollback catch-up.
    LOG_INFO("ADDR_GET_TICK = 0x%08X (sub_635F80)", ADDR_GET_TICK);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_GET_TICK),
            reinterpret_cast<void*>(&Hook_GetTick),
            reinterpret_cast<void**>(&g_origGetTick));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetTick (sub_635F80)! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_635F80 (tick/time source)");
    }
    
    // Enable all hooks
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        LOG_ERROR("MH_EnableHook failed! Status: %d", status);
        return false;
    }
    
    LOG_INFO("Input hooks installed and enabled!");
    return true;
}

void RemoveHooks() {
    LOG_INFO("Removing hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

// ============================================================================
// Direct Input Injection
// ============================================================================

// Keyboard scancode mapping for menu navigation
// The game uses DirectInput scancodes, not virtual keys
static const uint8_t SCANCODE_UP    = 0xC8;  // DIK_UP (200)
static const uint8_t SCANCODE_DOWN  = 0xD0;  // DIK_DOWN (208)
static const uint8_t SCANCODE_LEFT  = 0xCB;  // DIK_LEFT (203)
static const uint8_t SCANCODE_RIGHT = 0xCD;  // DIK_RIGHT (205)
static const uint8_t SCANCODE_Z     = 0x2C;  // DIK_Z (44) - typical confirm
static const uint8_t SCANCODE_X     = 0x2D;  // DIK_X (45) - typical cancel
static const uint8_t SCANCODE_A     = 0x1E;  // DIK_A (30)
static const uint8_t SCANCODE_S     = 0x1F;  // DIK_S (31)
static const uint8_t SCANCODE_ENTER = 0x1C;  // DIK_RETURN (28) - Start
static const uint8_t SCANCODE_ESC   = 0x01;  // DIK_ESCAPE (1) - Select/Back

// Write to DirectInput keyboard buffer (for MENU input)
// byte_9D09CC[scancode] - bit 7 indicates key pressed
void WriteKeyboardInput(uint8_t scancode, bool pressed) {
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    if (pressed) {
        keyBuffer[scancode] |= 0x80;  // Set bit 7
    } else {
        keyBuffer[scancode] &= 0x7F;  // Clear bit 7
    }
}

// Write to DirectInput joystick buffer (for MENU input when using joystick in menus)
// This writes to the raw DirectInput joystick state that menus read via sub_62FF50
// 
// IMPORTANT: Our internal INPUT_* format is different from game's joystick bitmask!
// INPUT_*:  UP=0x01, DOWN=0x02, LEFT=0x04, RIGHT=0x08
// Game joy: DOWN=0x01, LEFT=0x02, RIGHT=0x04, UP=0x08
// Buttons:  both use bits 4+ for buttons
static void WriteJoystickInputDirect(int joyIndex, uint16_t input) {
    if (joyIndex < 0 || joyIndex >= DINPUT_JOY_MAX) return;
    
    uintptr_t joyBase = ADDR_DINPUT_JOYSTICK + (joyIndex * DINPUT_JOY_STRUCT_SIZE);
    
    // X axis: offset +0 (int32)
    // -1000 = left, 0 = neutral, +1000 = right
    // INPUT_LEFT = 0x0004, INPUT_RIGHT = 0x0008
    int32_t xAxis = 0;
    if (input & INPUT_LEFT)  xAxis = -1000;
    if (input & INPUT_RIGHT) xAxis = +1000;
    WriteMemory<int32_t>(joyBase + 0, xAxis);
    
    // Y axis: offset +4 (int32)
    // -1000 = up (Y negative in game), 0 = neutral, +1000 = down (Y positive)
    // INPUT_UP = 0x0001, INPUT_DOWN = 0x0002
    int32_t yAxis = 0;
    if (input & INPUT_UP)   yAxis = -1000;
    if (input & INPUT_DOWN) yAxis = +1000;
    WriteMemory<int32_t>(joyBase + 4, yAxis);
    
    // Buttons: offset +64 + buttonIndex (byte each)
    // Bit 7 = pressed
    uint8_t* buttons = reinterpret_cast<uint8_t*>(joyBase + 64);
    
    // Clear all buttons first
    for (int i = 0; i < 24; i++) {
        buttons[i] = 0;
    }
    
    // Map our input bits to joystick buttons
    // INPUT_A=0x10, INPUT_B=0x20, INPUT_C=0x40, INPUT_D=0x80
    // INPUT_START=0x100, INPUT_SELECT=0x200
    if (input & INPUT_A)      buttons[0] = 0x80;
    if (input & INPUT_B)      buttons[1] = 0x80;
    if (input & INPUT_C)      buttons[2] = 0x80;
    if (input & INPUT_D)      buttons[3] = 0x80;
    if (input & INPUT_START)  buttons[7] = 0x80;
    if (input & INPUT_SELECT) buttons[6] = 0x80;
}

// Write our input to the game's match input buffer
// This is used during actual matches (word_8E9E62 / word_8E9F32)
// Buffer order: Down, Up, Left, Right, A, B, C, D, Start, Select (indices 0-9)
// NOTE: With hooks enabled, sub_562060 fills these from our hooked sub_62FF50,
//       but we write here too as a fallback for any direct buffer reads.
void WriteMatchInput(int player, uint16_t input) {
    uintptr_t bufferAddr = (player == 0) ? ADDR_P1_INPUT_BUFFER : ADDR_P2_INPUT_BUFFER;
    
    // Translate INPUT_* bits to buffer indices
    // Buffer index:  0=Down, 1=Up, 2=Left, 3=Right, 4=A, 5=B, 6=C, 7=D, 8=Start, 9=Select
    // INPUT_*:       DOWN=0x02, UP=0x01, LEFT=0x04, RIGHT=0x08, A=0x10, B=0x20, C=0x40, D=0x80, START=0x100, SELECT=0x200
    WriteMemory<uint16_t>(bufferAddr + 0,  (input & INPUT_DOWN)   ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 2,  (input & INPUT_UP)     ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 4,  (input & INPUT_LEFT)   ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 6,  (input & INPUT_RIGHT)  ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 8,  (input & INPUT_A)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 10, (input & INPUT_B)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 12, (input & INPUT_C)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 14, (input & INPUT_D)      ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 16, (input & INPUT_START)  ? 1 : 0);
    WriteMemory<uint16_t>(bufferAddr + 18, (input & INPUT_SELECT) ? 1 : 0);
}

// Write input - with hooks enabled, this is mainly for the match input buffers.
// The hooks (Hook_KeyboardState, Hook_JoystickState) handle menu input directly.
void WritePlayerInput(int player, uint16_t input) {
    // With hooks enabled, we only need to write to match buffers.
    // The hooked sub_62FF50 returns our input, which flows through sub_562060
    // to fill the match buffers naturally. But we write here as backup.
    WriteMatchInput(player, input);
    WriteMatchInput(player, input);
}

// Read current input from game's match buffer
uint16_t ReadPlayerInput(int player) {
    uintptr_t bufferAddr = (player == 0) ? ADDR_P1_INPUT_BUFFER : ADDR_P2_INPUT_BUFFER;
    uint16_t result = 0;
    
    for (int i = 0; i < 10; i++) {
        if (ReadMemory<uint16_t>(bufferAddr + i * 2) == 1) {
            result |= (1 << i);
        }
    }
    return result;
}

// ============================================================================
// ImGui Debug Content Functions
// ============================================================================
// Note: RenderInputConfigWindow, RenderSavestateWindow, RenderDebugWindow,
// and RenderNetplayWindow were removed as they were unused. The actual UI
// is rendered by mod_menu.cpp which calls the *Content functions below.

// Helper to update debug info by reading from game buffers
void UpdateInputDebugInfo() {
    // Read SDL input
    g_inputDebug.sdlInputP1 = InputSystem_GetInput(0);
    g_inputDebug.sdlInputP2 = InputSystem_GetInput(1);
    
    // Read game match buffers (P1: 0x8E9E62, P2: 0x8E9F32)
    for (int i = 0; i < 10; i++) {
        g_inputDebug.gameBufferP1[i] = ReadMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + i * 2);
        g_inputDebug.gameBufferP2[i] = ReadMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + i * 2);
    }
    
    // Read DirectInput joystick buffer (first joystick)
    uintptr_t joyBase = ADDR_DINPUT_JOYSTICK;
    g_inputDebug.dinputJoyAxisX = ReadMemory<int32_t>(joyBase + 0);
    g_inputDebug.dinputJoyAxisY = ReadMemory<int32_t>(joyBase + 4);
    for (int i = 0; i < 8; i++) {
        g_inputDebug.dinputJoyButtons[i] = ReadMemory<uint8_t>(joyBase + 64 + i);
    }
    
    // Read keyboard state (relevant keys)
    uint8_t* keyBuffer = reinterpret_cast<uint8_t*>(ADDR_DINPUT_KEYBOARD);
    g_inputDebug.keyState_Up    = keyBuffer[SCANCODE_UP];
    g_inputDebug.keyState_Down  = keyBuffer[SCANCODE_DOWN];
    g_inputDebug.keyState_Left  = keyBuffer[SCANCODE_LEFT];
    g_inputDebug.keyState_Right = keyBuffer[SCANCODE_RIGHT];
    g_inputDebug.keyState_Z     = keyBuffer[SCANCODE_Z];
    g_inputDebug.keyState_X     = keyBuffer[SCANCODE_X];
    g_inputDebug.keyState_Enter = keyBuffer[SCANCODE_ENTER];
}

// Content-only version for embedding in tabs
void RenderInputDebugContent() {
    UpdateInputDebugInfo();
    
    // Hook Statistics
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "=== Hook Statistics ===");
    ImGui::Text("Total Hook Calls: %d", g_hookCallCount);
    ImGui::Text("Keyboard Hook: %d (injected: %d)", 
        g_inputDebug.keyboardHookCalls, g_inputDebug.keyboardInjectedCount);
    ImGui::Text("Joystick Hook: %d (injected: %d)", 
        g_inputDebug.joystickHookCalls, g_inputDebug.joystickInjectedCount);
    
    if (g_hookCallCount == 0) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "WARNING: Hooks not being called!");
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Check if hooks installed correctly.");
    }
    
    ImGui::Text("Last keyCode: 0x%02X | playerID: 0x%08X", 
        g_inputDebug.lastKeyCode, g_inputDebug.lastPlayerID);
    ImGui::Text("Last origResult: 0x%04X -> finalResult: 0x%04X", 
        g_inputDebug.lastOrigResult, g_inputDebug.lastFinalResult);
    ImGui::Text("Last KB inject: 0x%04X | Last Joy inject: 0x%04X",
        g_inputDebug.lastInjectedKeyInput, g_inputDebug.lastInjectedJoyInput);
    
    // Player mapping debug
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 1, 1), "Player Mapping:");
    ImGui::Text("P1 JoyID: 0x%X | P2 JoyID: 0x%X | MappedTo: P%d", 
        g_inputDebug.p1JoyID, g_inputDebug.p2JoyID, g_inputDebug.lastMappedPlayer + 1);
    
    ImGui::Separator();
    
    // SDL3 Input State
    ImGui::TextColored(ImVec4(0, 1, 1, 1), "=== SDL Input ===");
    ImGui::Text("P1 SDL: 0x%04X -> Game: 0x%04X", g_inputDebug.sdlInputP1, g_inputDebug.gameInputP1);
    ImGui::Text("P2 SDL: 0x%04X -> Game: 0x%04X", g_inputDebug.sdlInputP2, g_inputDebug.gameInputP2);
    
    // Visual P1 buttons
    ImGui::Text("P1: ");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_UP ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "U");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_DOWN ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_LEFT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "L");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_RIGHT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "R");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_A ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "A");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_B ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "B");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_C ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "C");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_D ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_START ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "St");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP1 & INPUT_SELECT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "Se");
    
    // Visual P2 buttons
    ImGui::Text("P2: ");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_UP ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "U");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_DOWN ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_LEFT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "L");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_RIGHT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "R");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_A ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "A");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_B ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "B");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_C ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "C");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_D ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "D");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_START ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "St");
    ImGui::SameLine();
    ImGui::TextColored(g_inputDebug.sdlInputP2 & INPUT_SELECT ? ImVec4(0,1,0,1) : ImVec4(0.3f,0.3f,0.3f,1), "Se");
    
    ImGui::Separator();
    
    // Game buffers
    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "=== Game Match Buffers ===");
    ImGui::Text("P1@%08X: D%d U%d L%d R%d A%d B%d C%d D%d St%d Se%d",
        ADDR_P1_INPUT_BUFFER,
        g_inputDebug.gameBufferP1[0], g_inputDebug.gameBufferP1[1],
        g_inputDebug.gameBufferP1[2], g_inputDebug.gameBufferP1[3],
        g_inputDebug.gameBufferP1[4], g_inputDebug.gameBufferP1[5],
        g_inputDebug.gameBufferP1[6], g_inputDebug.gameBufferP1[7],
        g_inputDebug.gameBufferP1[8], g_inputDebug.gameBufferP1[9]);
    ImGui::Text("P2@%08X: D%d U%d L%d R%d A%d B%d C%d D%d St%d Se%d",
        ADDR_P2_INPUT_BUFFER,
        g_inputDebug.gameBufferP2[0], g_inputDebug.gameBufferP2[1],
        g_inputDebug.gameBufferP2[2], g_inputDebug.gameBufferP2[3],
        g_inputDebug.gameBufferP2[4], g_inputDebug.gameBufferP2[5],
        g_inputDebug.gameBufferP2[6], g_inputDebug.gameBufferP2[7],
        g_inputDebug.gameBufferP2[8], g_inputDebug.gameBufferP2[9]);
    
    ImGui::Separator();
    
    // DirectInput buffers
    ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "=== DirectInput ===");
    ImGui::Text("Joy@%08X: X=%d Y=%d", 
        ADDR_DINPUT_JOYSTICK, g_inputDebug.dinputJoyAxisX, g_inputDebug.dinputJoyAxisY);
    ImGui::Text("Joy Btns: %02X %02X %02X %02X %02X %02X %02X %02X",
        g_inputDebug.dinputJoyButtons[0], g_inputDebug.dinputJoyButtons[1],
        g_inputDebug.dinputJoyButtons[2], g_inputDebug.dinputJoyButtons[3],
        g_inputDebug.dinputJoyButtons[4], g_inputDebug.dinputJoyButtons[5],
        g_inputDebug.dinputJoyButtons[6], g_inputDebug.dinputJoyButtons[7]);
    ImGui::Text("KB@%08X: U=%02X D=%02X L=%02X R=%02X Z=%02X X=%02X",
        ADDR_DINPUT_KEYBOARD,
        g_inputDebug.keyState_Up, g_inputDebug.keyState_Down,
        g_inputDebug.keyState_Left, g_inputDebug.keyState_Right,
        g_inputDebug.keyState_Z, g_inputDebug.keyState_X);
    
    ImGui::Separator();
    
    // Hook pointers
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Hook Status:");
    ImGui::Text("KB hook: %s (orig: %p)", 
        g_origKeyboardState ? "OK" : "FAIL", g_origKeyboardState);
    ImGui::Text("Joy hook: %s (orig: %p)", 
        g_origJoystickState ? "OK" : "FAIL", g_origJoystickState);
}

// Verbose logging control
void SetVerboseLogging(bool enabled) {
    g_config.verboseLogging = enabled;
}

bool GetVerboseLogging() {
    return g_config.verboseLogging;
}

// Render effect/projectile debug info
void RenderEffectDebugContent() {
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "=== Effect/Projectile System ===");
    
    // Effect index
    uint8_t effectIndex = (uint8_t)ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
    ImGui::Text("Effect Write Index: %d / %d", effectIndex, EFFECT_MAX_SLOTS);
    
    // Count active effects
    int activeEffects = 0;
    for (int i = 0; i < EFFECT_MAX_SLOTS; i++) {
        uint32_t owner = ReadMemory<uint32_t>(ADDR_EFFECT_ARRAY + (i * EFFECT_ENTRY_SIZE));
        if (owner != 0) activeEffects++;
    }
    ImGui::Text("Active Effects: %d", activeEffects);
    
    ImGui::Separator();
    
    // Show first 10 active effects
    if (activeEffects > 0 && ImGui::TreeNode("Active Effects (first 10)")) {
        int shown = 0;
        for (int i = 0; i < EFFECT_MAX_SLOTS && shown < 10; i++) {
            uintptr_t effectAddr = ADDR_EFFECT_ARRAY + (i * EFFECT_ENTRY_SIZE);
            uint32_t owner = ReadMemory<uint32_t>(effectAddr);
            if (owner == 0) continue;
            
            uint8_t type = ReadMemory<uint8_t>(effectAddr + 4);
            int16_t x = ReadMemory<int16_t>(effectAddr + 6);
            int16_t y = ReadMemory<int16_t>(effectAddr + 8);
            int16_t timer = ReadMemory<int16_t>(effectAddr + 10);
            
            ImGui::Text("[%3d] Type:%02X Pos:(%d,%d) Timer:%d Owner:%08X",
                i, type, x, y, timer, owner);
            shown++;
        }
        ImGui::TreePop();
    }
    
    ImGui::Separator();
    
    // Combo counters
    ImGui::TextColored(ImVec4(0, 1, 1, 1), "=== Combo Counters ===");
    
    // Read combo from character structure
    uintptr_t p1Base = GetEntityBase(0);
    uintptr_t p2Base = GetEntityBase(1);
    
    // Combo stored at +41254 from character base (within the 108812 byte structure)
    // But we need to access it from the actual match data base, not entity base
    uint8_t p1Combo = ReadMemory<uint8_t>(ADDR_COMBO_COUNT);
    uint8_t p2Combo = ReadMemory<uint8_t>(ADDR_COMBO_COUNT + 1);
    
    ImGui::Text("P1 Combo: %d", p1Combo);
    ImGui::Text("P2 Combo: %d", p2Combo);
    
    // Try reading from entity offsets too
    if (p1Base) {
        uint8_t p1ComboAlt = ReadMemory<uint8_t>(p1Base + ENTITY_OFF_COMBO_P1 - (p1Base - ADDR_P1_ENTITY_BASE));
        uint16_t p1Guard = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_GUARD_GAUGE - (p1Base - ADDR_P1_ENTITY_BASE));
        ImGui::Text("P1 Guard Gauge: %d", p1Guard);
    }
    
    ImGui::Separator();
    
    // Sound system
    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "=== Sound System ===");
    
    uint32_t dsInterface = ReadMemory<uint32_t>(ADDR_DSOUND_INTERFACE);
    uint32_t soundMode = ReadMemory<uint32_t>(ADDR_SOUND_MODE);
    uint32_t soundState = ReadMemory<uint32_t>(ADDR_SOUND_STATE);
    
    ImGui::Text("DirectSound: %s (%08X)", dsInterface ? "Init" : "Not Init", dsInterface);
    ImGui::Text("Sound Mode: %d | State: %d", soundMode, soundState);
    
    // Count active sounds in pool
    int activeSounds = 0;
    for (int i = 0; i < 256; i++) {  // Only check first 256 for performance
        uint32_t handle = ReadMemory<uint32_t>(ADDR_SOUND_POOL + (i * 4));
        if (handle != 0) activeSounds++;
    }
    ImGui::Text("Active Sound Handles (first 256): %d", activeSounds);
}

// ============================================================================
// Deferred Initialization (called on first frame when D3D9 is ready)
// ============================================================================

static void DeferredInit() {
    // Initialize SDL3 input (safe now that game window exists)
    LOG_INFO("Performing deferred initialization...");
    
    // Initialize SDL3 input system
    if (!InputSystem_Init()) {
        LOG_WARN("Failed to initialize SDL input system - using native input only");
    } else {
        LOG_INFO("SDL3 input system initialized");
    }
    
    // Install hooks
    if (!InstallHooks()) {
        LOG_ERROR("Failed to install hooks!");
        return;
    }
    
    // Initialize game debug console system (separate console window for game logs)
    GameConsole_Init();
    
    // NOTE: UnlockAllContent() is NOT called here — config.dat hasn't been loaded yet.
    // The game loads config.dat during Mode 0 Substate 0 (first main-loop frame),
    // which would overwrite anything we write here. Instead, ModOnFrame() applies
    // unlocks after detecting the config version field has been initialized.
    
    // Log initial state
    LOG_INFO("Frame Counter: 0x%08X = %d", ADDR_SIM_FRAME_COUNTER, AS2_GetFrameNumber());
    LOG_INFO("Game Mode: 0x%08X = %d", ADDR_GAME_MODE, GetGameMode());
    LOG_INFO("P1 HP: 0x%08X = %d", ADDR_P1_HP_DIRECT, GetP1HP());
    LOG_INFO("P2 HP: 0x%08X = %d", ADDR_P2_HP_DIRECT, GetP2HP());
    
    g_initialized = true;
    
    // Initialize the unified menu
    ModMenu_Init();
    
    LOG_INFO("========================================");
    LOG_INFO("Initialization complete!");
    LOG_INFO("Hotkeys: F1=Menu, F5/F6=Save/Load 1, F7/F8=Save/Load 2");
    LOG_INFO("========================================");
}

// ============================================================================
// Exported Functions
// ============================================================================

extern "C" {

// Called by d3d9_proxy to share ImGui context across DLL boundary
__declspec(dllexport) void ModSetImGuiContext(void* ctx) {
    ImGui::SetCurrentContext((ImGuiContext*)ctx);
}

// Called by d3d9_proxy to share log directory so all logs land in the same dated folder
__declspec(dllexport) void ModSetLogDir(const char* dir) {
    LogWindow_SetLogDir(dir);
}

__declspec(dllexport) void ModInit(HMODULE gameModule) {
    g_gameModule = gameModule;
    
    // Initialize log window first
    LogWindow_Init();

    // Improve timer granularity for ping / frame pacing measurements.
    // This can reduce coarse ~15.6ms quantization on some systems.
    timeBeginPeriod(1);

    // NOTE: Duplicate-instance bypass (FindWindowA IAT hook + DXLib flag)
    // is handled by wsock32_proxy.dll which loads before DXLib_Init runs.
    // d3d9.dll is dynamically loaded by DXLib, so patching here is too late.

    LOG_INFO("========================================");
    LOG_INFO("Alice Senki 2 - Mod v0.3");
    LOG_INFO("Build: %s %s", __DATE__, __TIME__);
    LOG_INFO("========================================");
    LOG_INFO("Game module: 0x%p", gameModule);
    // Log our DLL base to help resolve crash addresses
    HMODULE selfModule = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&ModInit, &selfModule)) {
        LOG_INFO("Mod DLL base: 0x%p", selfModule);
    } else {
        LOG_WARN("Failed to get Mod DLL base address");
    }
    
    // IMPORTANT: We defer SDL and hook initialization until the game is ready.
    // SDL_Init with SDL_INIT_EVENTS can hang during DLL loading.
    // Hooks will be installed on first ModOnPresent call (when D3D9 is ready).
    
    // Initialize savestate storage
    memset(g_saveStates, 0, sizeof(g_saveStates));
    memset(g_stateValid, 0, sizeof(g_stateValid));
    
    LOG_INFO("Initialization deferred - will complete when game is ready...");
}

__declspec(dllexport) void ModShutdown() {
    LOG_INFO("Mod shutdown...");
    
    AS2_SetRollbackTraceLogging(false);
    
    if (g_initialized) {
        RemoveHooks();
        InputSystem_Shutdown();
    }

    timeEndPeriod(1);
    
    LogWindow_Shutdown();
}

__declspec(dllexport) void ModOnGameExit(int exitCode, const char* reason) {
    LOG_INFO("=====================================================");
    if (exitCode == 0) {
        LOG_INFO("[EXIT] Game exiting normally: %s", reason ? reason : "Unknown");
    } else {
        LOG_ERROR("[EXIT] Game CRASHED! Exit code: %d", exitCode);
        LOG_ERROR("[EXIT] Reason: %s", reason ? reason : "Unknown");
    }
    LOG_INFO("=====================================================");
    
    // Force flush logs so we can see what happened
    LOG_INFO("[EXIT] Cleanup complete");
}

__declspec(dllexport) void ModOnFrame() {
    // Deferred initialization - run once when D3D9 is ready
    if (!g_initialized) {
        DeferredInit();
    }
    
    if (!g_initialized) return;
    
    // Apply content unlocks after config.dat has been loaded.
    // The game loads config.dat during Mode 0 Substate 0 and writes the version
    // field 258 (0x102) to dword_8163BC at 0x8163BC. We wait for that, then
    // overwrite the 80-byte unlock array once.
    {
        static bool s_unlockApplied = false;
        if (!s_unlockApplied) {
            uint32_t version = ReadMemory<uint32_t>(ADDR_CONFIG_VERSION);
            if (version == 258) {
                UnlockAllContent();
                s_unlockApplied = true;
            }
        }
    }
    
    // CRITICAL: Increment visual frame counter FIRST
    AS2_IncrementVisualFrameCounter();
    
    // Update debug info
    UpdateInputDebugInfo();
    
    // Track previous input for change detection
    static uint16_t prevSdlP1 = 0;
    static uint16_t prevSdlP2 = 0;
    static int prevOrigResult = 0;
    static int prevFinalResult = 0;
    
    // Log when SDL input changes (button press/release)
    uint16_t sdlP1 = g_inputDebug.sdlInputP1;
    uint16_t sdlP2 = g_inputDebug.sdlInputP2;
    
    if (sdlP1 != prevSdlP1) {
        if (g_config.verboseLogging) {
            LOG_INFO("[INPUT] SDL P1: 0x%04X -> 0x%04X (U%d D%d L%d R%d A%d B%d C%d D%d St%d Se%d)",
                prevSdlP1, sdlP1,
                (sdlP1 & INPUT_UP) ? 1 : 0,
                (sdlP1 & INPUT_DOWN) ? 1 : 0,
                (sdlP1 & INPUT_LEFT) ? 1 : 0,
                (sdlP1 & INPUT_RIGHT) ? 1 : 0,
                (sdlP1 & INPUT_A) ? 1 : 0,
                (sdlP1 & INPUT_B) ? 1 : 0,
                (sdlP1 & INPUT_C) ? 1 : 0,
                (sdlP1 & INPUT_D) ? 1 : 0,
                (sdlP1 & INPUT_START) ? 1 : 0,
                (sdlP1 & INPUT_SELECT) ? 1 : 0);
        }
        prevSdlP1 = sdlP1;
    }
    
    if (sdlP2 != prevSdlP2) {
        if (g_config.verboseLogging) {
            LOG_INFO("[INPUT] SDL P2: 0x%04X -> 0x%04X", prevSdlP2, sdlP2);
        }
        prevSdlP2 = sdlP2;
    }
    
    // Log hook results when they change (and have input)
    if (g_inputDebug.lastFinalResult != prevFinalResult && g_inputDebug.lastFinalResult != 0) {
        if (g_config.verboseLogging) {
            LOG_INFO("[HOOK] Joy result: orig=0x%04X final=0x%04X (playerID=0x%X)",
                g_inputDebug.lastOrigResult, g_inputDebug.lastFinalResult, g_inputDebug.lastPlayerID);
        }
        prevOrigResult = g_inputDebug.lastOrigResult;
        prevFinalResult = g_inputDebug.lastFinalResult;
    }
    
    // Track game mode changes
    static int prevGameMode = -1;
    static int prevSubState = -1;
    int curGameMode = GetGameMode();
    int curSubState = GetSubstate();
    
    if (curGameMode != prevGameMode) {
        if (g_config.verboseLogging) {
            LOG_INFO("[STATE] Game Mode changed: %d -> %d", prevGameMode, curGameMode);
        }
        prevGameMode = curGameMode;
    }
    if (curSubState != prevSubState) {
        if (g_config.verboseLogging) {
            LOG_INFO("[STATE] Sub-State changed: %d -> %d", prevSubState, curSubState);
        }
        prevSubState = curSubState;
    }
    
    // Periodic status log
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 300 == 0 && g_config.verboseLogging) {  // Every 5 seconds at 60fps
        LOG_DEBUG("[STATUS] Hooks: KB=%d(%d inj) Joy=%d(%d inj) | SDL P1=0x%04X P2=0x%04X",
            g_inputDebug.keyboardHookCalls, g_inputDebug.keyboardInjectedCount,
            g_inputDebug.joystickHookCalls, g_inputDebug.joystickInjectedCount,
            g_inputDebug.sdlInputP1, g_inputDebug.sdlInputP2);
        LOG_DEBUG("[STATUS] Last Key inj=0x%04X Joy inj=0x%04X | P1 JoyID=0x%X P2 JoyID=0x%X",
            g_inputDebug.lastInjectedKeyInput, g_inputDebug.lastInjectedJoyInput,
            g_inputDebug.p1JoyID, g_inputDebug.p2JoyID);
        LOG_DEBUG("[STATUS] Game P1 buf: D%d U%d L%d R%d A%d B%d C%d D%d | Mode=%d SubState=%d",
            g_inputDebug.gameBufferP1[0], g_inputDebug.gameBufferP1[1],
            g_inputDebug.gameBufferP1[2], g_inputDebug.gameBufferP1[3],
            g_inputDebug.gameBufferP1[4], g_inputDebug.gameBufferP1[5],
            g_inputDebug.gameBufferP1[6], g_inputDebug.gameBufferP1[7],
            curGameMode, curSubState);
    }
    
    // Update SDL input
    InputSystem_Update();
    
    // Savestate hotkeys using Windows API (more reliable than SDL for function keys)
    static bool f5_was_pressed = false;
    static bool f6_was_pressed = false;
    static bool f7_was_pressed = false;
    static bool f8_was_pressed = false;
    
    bool f5_pressed = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool f6_pressed = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    bool f7_pressed = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    bool f8_pressed = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    
    // F5 = Save slot 1, F6 = Load slot 1
    if (f5_pressed && !f5_was_pressed) {
        LOG_INFO("F5 pressed - saving to slot 1");
        AS2_SaveState(8);
    }
    if (f6_pressed && !f6_was_pressed) {
        LOG_INFO("F6 pressed - loading from slot 1");
        AS2_LoadState(8);
    }
    
    // F7 = Save slot 2, F8 = Load slot 2
    if (f7_pressed && !f7_was_pressed) {
        LOG_INFO("F7 pressed - saving to slot 2");
        AS2_SaveState(9);
    }
    if (f8_pressed && !f8_was_pressed) {
        LOG_INFO("F8 pressed - loading from slot 2");
        AS2_LoadState(9);
    }
    
    f5_was_pressed = f5_pressed;
    f6_was_pressed = f6_pressed;
    f7_was_pressed = f7_pressed;
    f8_was_pressed = f8_pressed;
}

__declspec(dllexport) void ModOnPresent(void* pDevice) {
    // NOTE: Initialization and ModOnFrame are now called from d3d9_proxy's HookedEndScene EVERY frame
    // This function (ModOnPresent) is only called when the mod menu is shown
    
    if (!g_initialized) return;
    
    // Render the unified menu
    ModMenu_Render();
}

__declspec(dllexport) bool ModWantsExclusiveOverlay() {
    return false;
}

__declspec(dllexport) void ModToggleMenu() {
    ModMenu_Toggle();
}

// Called by d3d9_proxy to display always-on HUD without ImGui.
// Stub: no netplay active after rollback code removal.
__declspec(dllexport) bool ModGetNetplayHudText(char* out, int cap) {
    (void)out; (void)cap;
    return false;
}

// Structured match HUD data for the D3D9 overlay.
struct MatchHudData {
    bool     active;
    char     p1_name[24];
    char     p2_name[24];
    int      p1_wins;
    int      p2_wins;
    float    ping_ms;
    int      delay_frames;
    int      rollback_frames;
    int      local_frame;
    int      remote_frame;
    bool     is_host;
};

// Stub: no netplay active after rollback code removal.
__declspec(dllexport) bool ModGetMatchHudData(MatchHudData* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    return false;
}

__declspec(dllexport) uint32_t GetCurrentFrame() { return AS2_GetFrameNumber(); }
__declspec(dllexport) uint16_t GetPlayerHP(int player) { 
    return player == 0 ? GetP1HP() : GetP2HP(); 
}

// Borderless fullscreen control
__declspec(dllexport) bool* GetForceBorderlessPtr() {
    return &g_forceBorderlessFullscreen;
}

} // extern "C"
