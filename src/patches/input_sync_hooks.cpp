/**
 * Alice Senki 2 - Input Sync Hooks
 *
 * Hooks vanilla netplay functions to suppress them when the mod owns
 * synchronization (charsel lockstep, load barrier, active session).
 *
 * Also provides the load barrier freeze mechanism: when enabled,
 * Hook_InputDispatcher returns -1 to pause gameplay while keeping
 * the game loop alive for packet exchange and rendering.
 *
 * Reference: old_files/old_netplay/src/netplay/input_sync_hooks.cpp
 */

#include "patches/input_sync_hooks.h"
#include "patches/memory_utils.h"
#include "as2_constants.h"
#include "log_window.h"
#include "rollback/netplay_log.h"
#include "net/session_manager.h"
#include "net/charsel_sync.h"
#include "core/game_state.h"
#include "training/practice_tools.h"
#include "MinHook.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Typedefs for vanilla netplay functions
// ============================================================================

typedef int  (__cdecl *SendInputPacket_t)(int frame);
typedef void (__cdecl *RecvInputPacket_t)();
typedef int  (__cdecl *GetSyncInput_t)(int frame, int16_t* out);
typedef int  (__cdecl *AdvanceFrame_t)();
typedef int  (__cdecl *MatchSyncInit_t)();

// ============================================================================
// Original function pointers
// ============================================================================

static SendInputPacket_t g_origSendInputPacket = nullptr;
static RecvInputPacket_t g_origRecvInputPacket = nullptr;
static GetSyncInput_t    g_origGetSyncInput    = nullptr;
static AdvanceFrame_t    g_origAdvanceFrame     = nullptr;
static MatchSyncInit_t   g_origMatchSyncInit   = nullptr;

// ============================================================================
// Internal state
// ============================================================================

static bool s_installed = false;

// Load barrier freeze — game reached match loading but bootstrap isn't complete.
// When true, Hook_InputDispatcher (in input_override.cpp) returns -1 to pause
// gameplay while keeping the game loop alive.
static bool s_load_barrier_freeze = false;

// Runtime freeze — gameplay timesync intentionally pauses local advancement
// when the simulator is too far ahead of confirmed remote input.
static bool s_timesync_freeze = false;

// ============================================================================
// Helpers
// ============================================================================

static inline void ResetVanillaTimeouts() {
    *reinterpret_cast<volatile uint32_t*>(ADDR_HOST_TIMEOUT_CTR) = 0;
    *reinterpret_cast<volatile uint32_t*>(ADDR_CLIENT_TIMEOUT_CTR) = 0;
}

static inline bool IsGameplayFreezeActiveInternal() {
    return s_load_barrier_freeze || s_timesync_freeze || PracticeTools_ShouldFreezeFrame();
}

static inline bool ShouldSuppressAdvanceFrame() {
    if (IsGameplayFreezeActiveInternal()) return true;
    if (Net::CharSelSync_IsLockstepActive()) return true;
    return false;
}

// ============================================================================
// Hook implementations
// ============================================================================

/**
 * Hook for sub_562450 - Send Local Input Packet
 * Suppressed when mod owns sync.
 */
static int __cdecl Hook_SendInputPacket(int frame) {
    if (InputSyncHooks_IsModOwnedSync()) {
        ResetVanillaTimeouts();
        return 0;
    }
    return g_origSendInputPacket ? g_origSendInputPacket(frame) : 0;
}

/**
 * Hook for sub_5623D0 - Receive Remote Input Packet
 * Suppressed when mod owns sync.
 */
static void __cdecl Hook_RecvInputPacket() {
    if (InputSyncHooks_IsModOwnedSync()) {
        ResetVanillaTimeouts();
        return;
    }
    if (g_origRecvInputPacket) g_origRecvInputPacket();
}

/**
 * Hook for sub_5624E0 - Get Synchronized Inputs
 * Suppressed when mod owns sync. Returns zeroed inputs.
 */
static int __cdecl Hook_GetSyncInput(int frame, int16_t* out) {
    if (InputSyncHooks_IsModOwnedSync()) {
        ResetVanillaTimeouts();
        if (out) { out[0] = 0; out[1] = 0; }
        return 0;
    }
    return g_origGetSyncInput ? g_origGetSyncInput(frame, out) : -1;
}

/**
 * Hook for sub_562760 - Frame_AdvanceSimulation
 * Suppressed only while frontend lockstep owns frame stepping or while the
 * load barrier intentionally freezes gameplay at the Mode 8 boundary.
 *
 * Unlike the old dispatcher-driven rollback path, the current gameplay path is
 * bridge-driven from ModOnFrame and still relies on the game's normal Match
 * loop to advance. Do not suppress Frame_AdvanceSimulation for live gameplay.
 */
static int __cdecl Hook_AdvanceFrame() {
    bool suppress = ShouldSuppressAdvanceFrame();
    if (suppress) {
        static uint32_t s_advSuppressCount = 0;
        s_advSuppressCount++;
        if (s_advSuppressCount <= 5 || (s_advSuppressCount % 120) == 0) {
            Rollback::NetplayLog_Write("SYNC", -1,
                "AdvanceFrame SUPPRESSED (#%u): lockstep=%d freeze=%d frameSim=%d frameDisp=%d",
                s_advSuppressCount,
                (int)Net::CharSelSync_IsLockstepActive(),
                (int)IsGameplayFreezeActiveInternal(),
                *(volatile int32_t*)ADDR_FRAME_SIMULATION,
                *(volatile int32_t*)ADDR_FRAME_DISPLAY);
            Rollback::NetplayLog_Flush();
        }
        return 0;
    }
    int result = g_origAdvanceFrame ? g_origAdvanceFrame() : 0;
    PracticeTools_OnFrameAdvanced();
    return result;
}

/**
 * Hook for sub_562550 - Netplay_InitialSync
 * Vanilla: BLOCKING wait for a sync packet at CharSel fade-in.
 * Suppressed when mod owns sync — the blocking recvfrom would hang forever.
 */
static int __cdecl Hook_MatchSyncInit() {
    if (InputSyncHooks_IsModOwnedSync()) {
        Rollback::NetplayLog_Write("SYNC", -1,
            "MatchSyncInit ENTER: clearing all frame counters + history");
        Rollback::NetplayLog_Flush();

        // Replicate vanilla Netplay_InitialSync state resets exactly.
        *reinterpret_cast<volatile int32_t*>(ADDR_FRAME_SIMULATION) = 0;
        *reinterpret_cast<volatile int32_t*>(ADDR_FRAME_DISPLAY)    = 0;
        *reinterpret_cast<volatile int32_t*>(ADDR_INPUT_WRITE_IDX)  = 0;
        *reinterpret_cast<volatile int32_t*>(ADDR_FRAME_NET_IDX)    = 0;
        *reinterpret_cast<volatile int32_t*>(ADDR_REMOTE_FRAME)     = 0;

        // Clear input history buffers (vanilla fills with 0xFF = empty marker)
        memset(reinterpret_cast<void*>(ADDR_P1_INPUT_HISTORY), 0xFF, INPUT_HISTORY_P1_SIZE);
        memset(reinterpret_cast<void*>(ADDR_P2_INPUT_HISTORY), 0xFF, INPUT_HISTORY_P2_SIZE);

        // Clear vanilla netplay combat/sync flags (10 bytes each)
        memset(reinterpret_cast<void*>(0x8E93A4), 0, 10);
        memset(reinterpret_cast<void*>(0x8E93AE), 0, 10);

        ResetVanillaTimeouts();

        Rollback::NetplayLog_Write("SYNC", -1,
            "MatchSyncInit DONE: frameSim=%d frameDisp=%d writeIdx=%d netIdx=%d remote=%d",
            *(volatile int32_t*)ADDR_FRAME_SIMULATION,
            *(volatile int32_t*)ADDR_FRAME_DISPLAY,
            *(volatile int32_t*)ADDR_INPUT_WRITE_IDX,
            *(volatile int32_t*)ADDR_FRAME_NET_IDX,
            *(volatile int32_t*)ADDR_REMOTE_FRAME);
        Rollback::NetplayLog_Flush();

        LOG_NETPLAY(LOG_INFO, "[InputSyncHooks] Suppressed vanilla Netplay_InitialSync — "
            "reset ALL frame counters + cleared input history");
        return 0;
    }
    return g_origMatchSyncInit ? g_origMatchSyncInit() : 0;
}

// ============================================================================
// Public API
// ============================================================================

bool InputSyncHooks_IsModOwnedSync() {
    if (Net::CharSelSync_IsLockstepActive()) return true;
    if (IsGameplayFreezeActiveInternal()) return true;
    if (Net::Session_IsConnected()) return true;
    return false;
}

bool InputSyncHooks_Install() {
    if (s_installed) return true;

    struct HookEntry {
        uintptr_t target;
        void*     detour;
        void**    original;
        const char* name;
    };

    HookEntry hooks[] = {
        { ADDR_SEND_INPUT,      (void*)&Hook_SendInputPacket, (void**)&g_origSendInputPacket, "SendInputPacket" },
        { ADDR_RECV_INPUT,      (void*)&Hook_RecvInputPacket, (void**)&g_origRecvInputPacket, "RecvInputPacket" },
        { ADDR_GET_SYNC_INPUT,  (void*)&Hook_GetSyncInput,    (void**)&g_origGetSyncInput,    "GetSyncInput"    },
        { ADDR_ADVANCE_FRAME,   (void*)&Hook_AdvanceFrame,    (void**)&g_origAdvanceFrame,    "AdvanceFrame"    },
        { ADDR_MATCH_SYNC_INIT, (void*)&Hook_MatchSyncInit,   (void**)&g_origMatchSyncInit,   "MatchSyncInit"   },
    };

    for (auto& h : hooks) {
        MH_STATUS st = MH_CreateHook((LPVOID)h.target, h.detour, h.original);
        if (st != MH_OK) {
            LOG_ERROR("[InputSyncHooks] Failed to create hook %s: %d", h.name, st);
            return false;
        }
        st = MH_EnableHook((LPVOID)h.target);
        if (st != MH_OK) {
            LOG_ERROR("[InputSyncHooks] Failed to enable hook %s: %d", h.name, st);
            return false;
        }
        LOG_INFO("[InputSyncHooks] Hooked %s @ 0x%08X", h.name, h.target);
    }

    s_installed = true;
    LOG_INFO("[InputSyncHooks] All vanilla netplay suppression hooks installed");
    return true;
}

void InputSyncHooks_SetLoadBarrierFreeze(bool freeze) {
    if (s_load_barrier_freeze == freeze) return;
    s_load_barrier_freeze = freeze;
    if (freeze) {
        ResetVanillaTimeouts();
        LOG_NETPLAY(LOG_INFO, "[InputSyncHooks] Load barrier freeze ENABLED — gameplay paused");
        Rollback::NetplayLog_Write("SYNC", -1,
            "Load barrier freeze ENABLED");
    } else {
        LOG_NETPLAY(LOG_INFO, "[InputSyncHooks] Load barrier freeze DISABLED — gameplay resuming");
        Rollback::NetplayLog_Write("SYNC", -1,
            "Load barrier freeze DISABLED");
    }
}

bool InputSyncHooks_IsLoadBarrierFrozen() {
    return s_load_barrier_freeze;
}

void InputSyncHooks_SetTimesyncFreeze(bool freeze) {
    if (s_timesync_freeze == freeze) return;
    s_timesync_freeze = freeze;

    if (freeze) {
        ResetVanillaTimeouts();
        Rollback::NetplayLog_Write("TSYNC", -1,
            "Runtime freeze ENABLED — holding local gameplay until remote catches up");
    } else {
        Rollback::NetplayLog_Write("TSYNC", -1,
            "Runtime freeze DISABLED — local gameplay advancing again");
    }
}

bool InputSyncHooks_IsTimesyncFrozen() {
    return s_timesync_freeze;
}

bool InputSyncHooks_IsGameplayFreezeActive() {
    return IsGameplayFreezeActiveInternal();
}
