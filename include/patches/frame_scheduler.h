/**
 * Alice Senki 2 - FrameScheduler (re0.7 M2, master plan §2.8)
 *
 * The mod-owned clock. Replaces the game's 17 ms busy-spin frame limiter
 * (Game_MainLoop 0x5D2AC0, decomp L266504–266510) with an absolute-deadline
 * QPC scheduler: byte-signature detour at the limiter site, Sleep(1)+spin
 * hybrid wait, integer-exact cadence profiles, bounded one-sided pace slew,
 * CadenceDebt ledger, and the §2.10 STAT emission (moved here from
 * netplay_pacing at M2 — the format is the §7 acceptance instrument).
 *
 * Ownership rules (INV-5 / INV-17):
 *   - This is the ONLY component that owns wall-clock frame timing.
 *   - `Hook_GetTick` is pinned to scale 1.0 while the scheduler is installed
 *     (tick_hooks.cpp); the R-1 fallback (install failure or
 *     `frame_scheduler=0` in as2_rollback_settings.ini) re-enables the legacy
 *     virtual-clock limiter correction for one release.
 *   - Pace slew is applied inside the scheduler's period, nowhere else.
 *
 * Threading: everything here runs on the game thread (install happens before
 * the game loop starts). No locks.
 */

#pragma once

#include <stdint.h>

#include "rollback/run_state.h"

struct FrameSchedulerSnapshot {
    bool     installed;
    bool     compat_58;           // active cadence profile is compat_58
    float    speed_scale;         // effective speed vs native (manual+adjust+slew)
    float    period_adjust_us;    // legacy pacing shim input (±1000 µs)
    int32_t  slew_ppm;            // active §2.8.4 pace slew
    uint32_t debt_frames;         // CadenceDebt owed
    uint32_t rebase_count;        // deadline rebases since install
    uint32_t present_p50_us;      // last emitted second's pass-interval p50
    uint32_t present_p99_us;      // last emitted second's pass-interval p99
    uint32_t holds_prediction;    // last emitted second, by HoldCause
    uint32_t holds_lifecycle;
    uint32_t holds_local_input;
    uint32_t holds_external;
    Rollback::RunState run_state; // classification of the most recent pass
};

// ── Install / lifecycle ─────────────────────────────────────────────────────

// Byte-signature scan of the limiter cluster + detour install. Fails loud
// (LOG_ERROR + netplay log) and returns false on any mismatch — callers must
// leave the legacy virtual-clock path active in that case (risk R-1).
bool FrameScheduler_Install();
bool FrameScheduler_IsInstalled();
// Restores the original limiter bytes (mod unload).
void FrameScheduler_Shutdown();

// Sticky dead-clock latch (§2.8.2 PacingClockDead): QPC made no progress
// across ≥500 Sleep(1) rounds inside a wait. session2 polls this and converts
// it into the fail-closed PacingClockDead terminal (INV-20, M3).
bool FrameScheduler_IsPacingClockDead();

// The detour target. Public only so the installer can reference it; never
// call it directly.
void __cdecl FrameScheduler_WaitForNextFrame();

// ── Per-pass plan notifications (game thread, from the dispatcher) ──────────

// The pass now ending ran zero sims for `cause`. First cause of a pass wins;
// ExternalSuspension discards all owed debt. `create_debt` declares that the
// hold owes a hidden catch-up frame (§2.8.5 CreateOne) — false for the M2
// legacy-pacing semanticHold shim, true only for engine2 PredictionLimit
// holds from M4 on.
void FrameScheduler_NotifyHold(Rollback::HoldCause cause, bool create_debt);

// One visible sim frame advanced in this pass.
void FrameScheduler_NotifySimFrame();

// Ask for one hidden catch-up frame this pass (§2.8.3 N = 1+k). True when
// debt is owed, fewer than kMaxCatchupExtraPerPass extras ran this pass,
// `max_depth_headroom` (= R_local − depth − 1, computed by the caller) is
// positive, and the 6000 µs hidden-work wall budget is not exhausted.
bool FrameScheduler_TryTakeCatchupFrame(int32_t max_depth_headroom);

// A hidden catch-up frame actually executed: consumes one owed debt frame
// and counts as a sim frame.
void FrameScheduler_NotifyCatchupFrame();

// External stall (load barrier, device churn, focus loss): discard owed debt.
void FrameScheduler_DiscardExternalDebt(const char* reason);

// ── Speed inputs ────────────────────────────────────────────────────────────

// Legacy netplay_pacing shim (M2..M5): continuous period adjustment in µs,
// positive = lengthen (slow down). Clamped to ±1000 µs. Deleted with the
// controller at M6.
void FrameScheduler_SetPeriodAdjustUs(float adjust_us, const char* reason);

// §2.8.4 pace-slew input hook: prediction depths from the freshest peer
// PressureReport. No caller until the v2 wire is live (M4/M6); the stale
// timeout (64 ms) keeps it released meanwhile.
void FrameScheduler_SubmitPeerDepthSample(int32_t peer_depth, int32_t local_depth);

// Session boundary: clears adjust/slew/debt and STAT session context. The
// deadline itself is never reset (INV-17: rebases only).
void FrameScheduler_OnSessionReset(const char* reason);

// ── Queries ─────────────────────────────────────────────────────────────────

float FrameScheduler_GetSpeedScale();
int32_t FrameScheduler_GetSlewPpm();
void FrameScheduler_GetSnapshot(FrameSchedulerSnapshot* out);
