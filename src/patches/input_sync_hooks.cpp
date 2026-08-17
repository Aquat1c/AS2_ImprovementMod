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
#include "patches/frame_scheduler.h"
#include "patches/memory_utils.h"
#include "as2_constants.h"
#include "log_window.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"
#include "net/session_manager.h"
#include "net/charsel_sync.h"
#include "replay/replay_runtime.h"
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
typedef char (__cdecl *MatchUpdateScoreStats_t)();

// ============================================================================
// Original function pointers
// ============================================================================

static SendInputPacket_t g_origSendInputPacket = nullptr;
static RecvInputPacket_t g_origRecvInputPacket = nullptr;
static GetSyncInput_t    g_origGetSyncInput    = nullptr;
static AdvanceFrame_t    g_origAdvanceFrame     = nullptr;
static MatchSyncInit_t   g_origMatchSyncInit   = nullptr;
static MatchUpdateScoreStats_t g_origMatchUpdateScoreStats = nullptr;

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

// ── Render-phase RNG isolation (SAVESTATE_AUDIT F2, P0) ─────────────────────
// The mode-8 render phase (Game_Update_MatchLoop post-loop: sub_4C47C0 super
// backgrounds — 15 rand() call sites — plus everything up to the next pass)
// shares the single CRT LCG with the sim. Render cadence is not sim cadence:
// a rollback resim runs N sim frames inside one outer pass → 1 render, while
// the straight-through peer rendered N times; the local "background off"
// option even changes consumption at 1:1 cadence. Any render rand() therefore
// permanently diverges the shared stream.
//
// Isolation contract (per DECOMP_TIMING_STUDY §1.2): Frame_AdvanceSimulation
// (hooked below) is called exactly once per outer pass, AFTER the sim
// while-loop and immediately BEFORE the render-phase gate — so the seed
// captured there is the post-sim seed. The next sim-side code to run is the
// next pass's first Input_TryGetNextFrame dispatch (sim work only happens
// inside the while-loop), so restoring the captured seed at dispatcher entry
// erases every rand() consumed by the render phase / present / frontend from
// the sim's stream. Net effect: the sim RNG stream on both peers is exactly
// "as if render never called rand()" — cadence-independent — while the
// render still gets naturally varying (cosmetic-only, per-peer) values.
// This is the least-invasive equivalent of the audit's private-cosmetic-PRNG
// recommendation: no new PRNG, no binary patch, two existing hook points.
static bool     s_renderRngPending = false;
static uint32_t s_renderRngSeed = 0;

static inline bool RenderRngIsolationActive() {
    // Netplay mode-8 only: offline play keeps vanilla behavior (the offline
    // forced-rollback determinism harness runs under an active session and
    // is covered). CharSel stepped passes also reach Hook_AdvanceFrame —
    // the mode gate keeps them out.
    return Rollback::RollbackSession_IsActive() && GetGameMode() == MODE_MATCH;
}

// ============================================================================
// Helpers
// ============================================================================

static inline void ResetVanillaTimeouts() {
    *reinterpret_cast<volatile uint32_t*>(ADDR_HOST_TIMEOUT_CTR) = 0;
    *reinterpret_cast<volatile uint32_t*>(ADDR_CLIENT_TIMEOUT_CTR) = 0;
}

static inline bool IsGameplayFreezeActiveInternal() {
    return s_load_barrier_freeze ||
           s_timesync_freeze ||
           PracticeTools_ShouldFreezeFrame() ||
           Replay::ReplayRuntime_ShouldFreezeFrame();
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
 * Live rollback remains dispatcher-driven through the game's normal match loop.
 * Do not suppress Frame_AdvanceSimulation for active in-match rollback.
 */
static int __cdecl Hook_AdvanceFrame() {
    // F2 render-RNG isolation: this hook runs once per outer pass, after the
    // last sim iteration and before the render phase (see block comment at
    // the state above). Belt-and-braces restore first (normally a no-op —
    // the dispatcher already restored at pass entry), then capture the
    // post-sim seed for the upcoming render phase. Runs on the suppressed
    // path too: a 0-sim hold pass still renders (match+11 stays 1) and an
    // active super background still consumes rand() while frozen.
    InputSyncHooks_RestoreRenderRngIfPending();
    if (RenderRngIsolationActive()) {
        s_renderRngSeed = DetVer_GetRngSeed();
        s_renderRngPending = true;
    }

    const bool loadBarrierFreeze = s_load_barrier_freeze;
    const bool timesyncFreeze = s_timesync_freeze;
    const bool practiceFreeze = PracticeTools_ShouldFreezeFrame();
    const bool replayFreeze = Replay::ReplayRuntime_ShouldFreezeFrame();
    const bool charselLockstep = Net::CharSelSync_IsLockstepActive();
    bool suppress = ShouldSuppressAdvanceFrame();
    if (suppress) {
        static uint32_t s_advSuppressCount = 0;
        s_advSuppressCount++;
        if (s_advSuppressCount <= 5 || (s_advSuppressCount % 120) == 0) {
            Rollback::NetplayLog_Write("SYNC", -1,
                "AdvanceFrame SUPPRESSED (#%u): lockstep=%d freeze=%d frameSim=%d frameDisp=%d",
                s_advSuppressCount,
                (int)charselLockstep,
                (int)(loadBarrierFreeze || timesyncFreeze || practiceFreeze || replayFreeze),
                *(volatile int32_t*)ADDR_FRAME_SIMULATION,
                *(volatile int32_t*)ADDR_FRAME_DISPLAY);
            Rollback::NetplayLog_Flush();
        }

        // Runtime freeze is a hold-for-this-frame pulse, not a sticky mode.
        // The dispatcher/gate logic will re-arm it on the next held frame if
        // we still need to stall, hard-skip, or wait for startup release.
        if (timesyncFreeze) {
            s_timesync_freeze = false;
            Rollback::NetplayLog_Verbose("TSYNC", -1,
                "Runtime freeze pulse CONSUMED by AdvanceFrame suppression: "
                "load_barrier=%d practice=%d lockstep=%d frameSim=%d frameDisp=%d",
                loadBarrierFreeze ? 1 : 0,
                practiceFreeze ? 1 : 0,
                charselLockstep ? 1 : 0,
                *(volatile int32_t*)ADDR_FRAME_SIMULATION,
                *(volatile int32_t*)ADDR_FRAME_DISPLAY);
        }
        return 0;
    }
    int result = g_origAdvanceFrame ? g_origAdvanceFrame() : 0;

    // ── F7i (2026-08-17 round-boundary desync, runs 19-42/19-49) ────────────
    // Frame_Simulation (0x816490) increments once per OUTER PASS (this
    // function's vanilla body), so under per-side catch-up its value skews
    // between peers. F7d proved no MID-ROUND sim reader and digest-masked
    // it — but the fine-diag ring then caught the ROUND TIME-OVER check
    // consuming it: at f3985 both sides' entire 253 KB state was
    // byte-identical (all 4043 windows) with sim=3340 vs 3342, and the side
    // 2 passes ahead flipped substate 3->2 (round end) two confirmed frames
    // early. The counter IS sim-relevant at exactly that seam, so it must
    // be DETERMINISTIC, not masked: while the mod owns netplay gameplay,
    // mirror it from Frame_Display (0x816494), which ticks once per SIM
    // frame on the canonical timeline (F3) and is already hashed.
    // Capture/restore are unchanged (both counters ride the snapshot); the
    // F7d digest exclusion stays (the alias is now redundant with fdisp).
    if (Rollback::RollbackSession_IsActive() && GetGameMode() == MODE_MATCH) {
        *reinterpret_cast<volatile int32_t*>(ADDR_FRAME_SIMULATION) =
            *reinterpret_cast<volatile int32_t*>(ADDR_FRAME_DISPLAY);
    }

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

/**
 * Hook for sub_55BCD0 - Match_UpdateScoreStats (SAVESTATE_AUDIT F6, P2).
 *
 * Called on the round-end commit tick (transition timer == 25). Its `+=`
 * score/rank/arcade-continuation writes live OUTSIDE every snapshot region,
 * so a rollback across the commit tick would re-apply them on resim
 * (double-count persisted to config.dat). The commit tick sits inside the
 * predicted window by design (M4-7 keeps no round-boundary exact window).
 *
 * Suppression rule: skip while the engine is replaying (IsRollingBack). The
 * speculative first pass over the commit tick applied the stats; the resim
 * pass over the same tick is the double-apply, so skipping it nets exactly
 * one application. Frames beyond the pre-rollback frontier run as normal
 * advances (not rolling back) and apply fresh.
 *
 * Accepted residuals (audit "accept and document" clause): (a) if a
 * misprediction inside the replay window changes the round outcome, the
 * speculatively-applied stats stand for the superseded outcome — cosmetic
 * persistence noise, no sim-state impact (no mode-8 sim reader exists);
 * (b) offline manual savestate F6-load can still replay a commit tick and
 * re-apply — offline practice tooling, out of netplay scope.
 */
static char __cdecl Hook_MatchUpdateScoreStats() {
    if (Rollback::RollbackSession_IsRollingBack()) {
        Rollback::NetplayLog_Write("ROLLBACK",
            Rollback::RollbackSession_GetCurrentFrame(),
            "Match_UpdateScoreStats SUPPRESSED during resim (F6 double-apply guard)");
        return 0;
    }
    return g_origMatchUpdateScoreStats ? g_origMatchUpdateScoreStats() : 0;
}

// ============================================================================
// Public API
// ============================================================================

void InputSyncHooks_RestoreRenderRngIfPending() {
    if (!s_renderRngPending) {
        return;
    }
    s_renderRngPending = false;
    // Only restore while the session that captured is still the owner: if
    // the session ended mid-pass the capture is stale and the offline RNG
    // stream must not be rewound.
    if (Rollback::RollbackSession_IsActive()) {
        DetVer_SetRngSeed(s_renderRngSeed);
    }
}

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
        { ADDR_SEND_INPUT,        (void*)&Hook_SendInputPacket,       (void**)&g_origSendInputPacket,       "SendInputPacket" },
        { ADDR_RECV_INPUT,        (void*)&Hook_RecvInputPacket,       (void**)&g_origRecvInputPacket,       "RecvInputPacket" },
        { ADDR_GET_SYNC_INPUT,    (void*)&Hook_GetSyncInput,          (void**)&g_origGetSyncInput,          "GetSyncInput"    },
        { ADDR_ADVANCE_FRAME,     (void*)&Hook_AdvanceFrame,          (void**)&g_origAdvanceFrame,          "AdvanceFrame"    },
        { ADDR_MATCH_SYNC_INIT,   (void*)&Hook_MatchSyncInit,         (void**)&g_origMatchSyncInit,         "MatchSyncInit"   },
        { ADDR_MATCH_SCORE_STATS, (void*)&Hook_MatchUpdateScoreStats, (void**)&g_origMatchUpdateScoreStats, "Match_UpdateScoreStats" },
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
        // Load stalls are external causes: they never leave hidden-frame
        // repayment behind (§2.8.5 discardExternal).
        FrameScheduler_DiscardExternalDebt("load_barrier_freeze");
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
