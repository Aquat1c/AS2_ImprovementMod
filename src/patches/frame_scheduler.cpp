/**
 * Alice Senki 2 - FrameScheduler (re0.7 M2, master plan §2.8)
 *
 * Win32 driver around the pure core (frame_scheduler_core.h):
 *
 *   - Install: byte-signature scan of the frame-limiter busy-spin cluster at
 *     the bottom of Game_MainLoop (0x5D2AC0). The decomp-derived pattern
 *     (`sub eax,[816360]`) does NOT exist in the shipping exe; the REAL bytes
 *     (read from as2.exe .text, verified 2026-08-17, cluster at 0x5D2C01) are
 *     a head check + spin loop, each block being:
 *         6A 00                     push 0
 *         E8 <rel32 → sub_635F80>   call Sys_GetTimeMs(0)
 *         8B 15 60 63 81 00         mov  edx, dword_816360
 *         83 C4 04                  add  esp, 4
 *         2B C2                     sub  eax, edx
 *         83 F8 11                  cmp  eax, 17
 *     head block + `7D 17` (jge over the loop), then the identical loop block
 *     + `7C E9` (jl back to the loop head) — 46 bytes total.
 *     The whole 46-byte cluster is replaced with
 *     `call FrameScheduler_WaitForNextFrame` + NOPs, so both vanilla jumps are
 *     structurally neutralized. The re-stamp that follows at 0x5D2C2F
 *     (`push 0; call sub_635F80; mov dword_816360, eax`) is untouched, so
 *     `dword_816360` and the vanilla FPS counter stay coherent (M2 journal).
 *     The signature must match EXACTLY once or the install fails loud and the
 *     legacy virtual-clock path stays active (risk R-1).
 *
 *   - WaitForNextFrame: absolute QPC deadlines with the Bresenham cadence
 *     carry (§2.8.2), Sleep(1)-until-2ms + spin tail, rebase-never-compress on
 *     multi-frame lateness, the §2.8.4 pace slew applied inside the period,
 *     and the §2.10 STAT emission (moved here from netplay_pacing per the M0
 *     obligation — the line format is frozen).
 *
 * Wall-clock use in this file is pacing/telemetry only. No wall clock ever
 * enters a simulation decision (INV-16): the sims-per-pass plan is made by
 * the dispatcher from engine facts; this module only labels and paces it.
 */

#include "patches/frame_scheduler.h"
#include "patches/frame_scheduler_core.h"
#include "patches/tick_hooks.h"
#include "patches/memory_utils.h"
#include "as2_constants.h"
#include "log_window.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"
#include "net/connection_supervisor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <intrin.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

// ── Install state ───────────────────────────────────────────────────────────

constexpr uint32_t kMaxPatchLen = 46;

bool      s_installed = false;
uintptr_t s_patchAddr = 0;
uint32_t  s_patchLen = 0;
uint8_t   s_originalBytes[kMaxPatchLen] = {};

// ── Clock / core state (game thread only) ───────────────────────────────────

uint64_t s_qpf = 0;
Sched::DeadlineClock     s_clock{};
Sched::PaceSlew          s_slew{};
Sched::CadenceDebtLedger s_debt{};
Sched::IntervalStats     s_intervals{};
Rollback::HoldEpisodeLedger s_holdLedger{};

bool  s_compat58 = false;
float s_periodAdjustUs = 0.0f;
float s_lastSpeedScale = 1.0f;

// Pace-slew sample (no producer until the v2 PressureReport is live).
uint64_t s_slewSampleQpc = 0;
int32_t  s_slewPeerDepth = 0;
int32_t  s_slewLocalDepth = 0;
constexpr uint32_t kSlewFreshnessUs = 64000;  // ≤ 4 frames / 64 ms

// ── Per-pass accounting ─────────────────────────────────────────────────────

uint32_t            s_passSimFrames = 0;
uint32_t            s_passCatchupFrames = 0;
uint32_t            s_passCatchupTries = 0;
Rollback::HoldCause s_passHoldCause = Rollback::HoldCause::None;
uint64_t            s_passStartQpc = 0;     // stamped when the wait releases
uint64_t            s_prevWaitEntryQpc = 0; // pass-interval measurement
Rollback::RunState  s_runState = Rollback::RunState::Running;

constexpr uint32_t kCatchupWallBudgetUs = 6000;  // §2.8.3 hidden-work budget

// ── Per-second STAT window ──────────────────────────────────────────────────

uint64_t s_secStartQpc = 0;
uint32_t s_secSimFrames = 0;
uint32_t s_secHolds[5] = {};  // indexed by HoldCause
int32_t  s_secRollbackBase = -1;
int32_t  s_secMaxReplayLen = 0;
int32_t  s_lastTelemetryFrame = -1;

// Last emitted second (for GetSnapshot).
uint32_t s_lastP50Us = 0;
uint32_t s_lastP99Us = 0;
uint32_t s_lastHolds[5] = {};

// ── Diagnostics ─────────────────────────────────────────────────────────────

uint32_t s_rebaseCount = 0;
uint64_t s_lastRebaseLogQpc = 0;
uint64_t s_lastNowQpc = 0;
uint32_t s_qpcRegressionCount = 0;
bool     s_deadClockLatched = false;

// ── Helpers ─────────────────────────────────────────────────────────────────

inline uint64_t Qpc() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}

inline uint64_t UsToTicks(uint64_t us) { return us * s_qpf / 1000000ull; }
inline uint64_t TicksToUs(uint64_t ticks) { return ticks * 1000000ull / s_qpf; }

bool ReadSchedulerEnabledSetting() {
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
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
        wcscat_s(path, L"as2_rollback_settings.ini");
    } else {
        wcscpy_s(path, L"as2_rollback_settings.ini");
    }

    wchar_t value[16] = {};
    GetPrivateProfileStringW(L"ModSettings", L"frame_scheduler", L"1",
                             value, (DWORD)(sizeof(value) / sizeof(value[0])), path);
    return !(value[0] == L'0' && value[1] == L'\0');
}

// One elapsed-time check block of the limiter (21 bytes):
//   push 0 / call Sys_GetTimeMs / mov edx,[dword_816360] / add esp,4 /
//   sub eax,edx / cmp eax,17
// Empirical bytes from the shipping exe (0x5D2C01 / 0x5D2C18) — the decomp's
// `sub eax,[mem]` form never matched (2026-08-17 live-run finding).
constexpr uint32_t kLimiterBlockLen = 21;
// Full cluster: head block + jge-short over the loop + loop block + jl-short
// back to the loop head.
constexpr uint32_t kLimiterClusterLen = kLimiterBlockLen + 2 + kLimiterBlockLen + 2;
static_assert(kLimiterClusterLen <= kMaxPatchLen, "patch buffer too small");

bool MatchLimiterBlock(const uint8_t* p) {
    if (p[0] != 0x6A || p[1] != 0x00) return false;          // push 0
    if (p[2] != 0xE8) return false;                          // call rel32
    const int32_t rel = *reinterpret_cast<const int32_t*>(p + 3);
    const uintptr_t target = (uintptr_t)(p + 7) + (intptr_t)rel;
    if (target != (uintptr_t)ADDR_GET_TICK) return false;
    if (p[7] != 0x8B || p[8] != 0x15) return false;          // mov edx, [imm32]
    if (*reinterpret_cast<const uint32_t*>(p + 9) != (uint32_t)ADDR_LAST_FRAME_TIME) return false;
    if (p[13] != 0x83 || p[14] != 0xC4 || p[15] != 0x04) return false;  // add esp, 4
    if (p[16] != 0x2B || p[17] != 0xC2) return false;        // sub eax, edx
    if (p[18] != 0x83 || p[19] != 0xF8 || p[20] != 0x11) return false;  // cmp eax, 17
    return true;
}

// Finds the limiter cluster. Must match exactly once in the Game_MainLoop
// scan window or the install refuses (fail loud, R-1).
bool FindLimiterCluster(uintptr_t* outAddr, uint32_t* outLen) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(ADDR_FRAME_LIMITER_SCAN_BEGIN);
    int found = 0;
    uintptr_t addr = 0;
    uint32_t len = 0;

    for (uint32_t off = 0; off + kLimiterClusterLen <= ADDR_FRAME_LIMITER_SCAN_SIZE; ++off) {
        const uint8_t* p = base + off;
        // Head check block, then jge short hopping exactly over the spin loop.
        if (!MatchLimiterBlock(p)) continue;
        const uint8_t* jge = p + kLimiterBlockLen;
        if (jge[0] != 0x7D) continue;
        if ((int8_t)jge[1] != (int8_t)(kLimiterBlockLen + 2)) continue;
        // The spin-loop body: identical block, then jl short back to its head.
        const uint8_t* loop = jge + 2;
        if (!MatchLimiterBlock(loop)) continue;
        const uint8_t* jl = loop + kLimiterBlockLen;
        if (jl[0] != 0x7C) continue;
        if ((int8_t)jl[1] != -(int8_t)(kLimiterBlockLen + 2)) continue;
        ++found;
        addr = (uintptr_t)p;
        len = kLimiterClusterLen;
    }

    if (found != 1) {
        LOG_ERROR("[FrameScheduler] Limiter signature scan FAILED: matches=%d "
                  "(scan 0x%08X+0x%X). Falling back to the legacy virtual-clock "
                  "limiter (risk R-1).",
                  found,
                  (unsigned)ADDR_FRAME_LIMITER_SCAN_BEGIN,
                  (unsigned)ADDR_FRAME_LIMITER_SCAN_SIZE);
        return false;
    }
    *outAddr = addr;
    *outLen = len;
    return true;
}

int32_t HoldCauseIndex(Rollback::HoldCause cause) {
    const int32_t idx = (int32_t)cause;
    return (idx >= 0 && idx < 5) ? idx : 0;
}

void ResetSecondWindow(uint64_t now) {
    s_secStartQpc = now;
    s_secSimFrames = 0;
    memset(s_secHolds, 0, sizeof(s_secHolds));
    s_secMaxReplayLen = 0;
    s_intervals.Reset();
}

void EmitStat(uint64_t now) {
    const uint64_t elapsedUs = TicksToUs(now - s_secStartQpc);
    if (elapsedUs == 0) {
        ResetSecondWindow(now);
        return;
    }

    Rollback::NetplayStatSample sample{};
    sample.sim_fps = (float)((double)s_secSimFrames * 1.0e6 / (double)elapsedUs);
    sample.present_p50_us = s_intervals.Percentile(50);
    sample.present_p99_us = s_intervals.Percentile(99);
    sample.holds_prediction = s_secHolds[HoldCauseIndex(Rollback::HoldCause::PredictionLimit)];
    sample.holds_lifecycle = s_secHolds[HoldCauseIndex(Rollback::HoldCause::LifecycleBoundary)];
    sample.holds_local_input = s_secHolds[HoldCauseIndex(Rollback::HoldCause::LocalInputMissing)];
    sample.holds_external = s_secHolds[HoldCauseIndex(Rollback::HoldCause::ExternalSuspension)];
    sample.slew_ppm = s_slew.Ppm();
    sample.debt_frames = (int32_t)s_debt.Owed();
    sample.silence_ms = Net::ConnectionSupervisor_GetInboundSilenceMs();

    int32_t frame = -1;
    if (Rollback::RollbackSession_IsActive()) {
        Rollback::RollbackTimesyncTelemetry telemetry{};
        Rollback::RollbackSession_GetTimesyncTelemetry(&telemetry);
        frame = telemetry.rb_frame_current;
        if (s_secRollbackBase < 0 || telemetry.rollback_count < s_secRollbackBase) {
            s_secRollbackBase = telemetry.rollback_count;
        }
        const int32_t rollbacks = telemetry.rollback_count - s_secRollbackBase;
        sample.rollbacks = rollbacks > 0 ? (uint32_t)rollbacks : 0u;
        sample.rollback_max_depth = rollbacks > 0 ? (uint32_t)s_secMaxReplayLen : 0u;
        s_secRollbackBase = telemetry.rollback_count;
    } else {
        s_secRollbackBase = -1;
    }

    Rollback::NetplayLog_Stat(frame, sample);

    s_lastP50Us = sample.present_p50_us;
    s_lastP99Us = sample.present_p99_us;
    memcpy(s_lastHolds, s_secHolds, sizeof(s_lastHolds));
    ResetSecondWindow(now);
}

// Closes the accounting for the pass that just ended (called at wait entry).
void FinishPassAccounting(uint64_t now) {
    if (s_prevWaitEntryQpc != 0 && now > s_prevWaitEntryQpc) {
        const uint64_t deltaUs = TicksToUs(now - s_prevWaitEntryQpc);
        s_intervals.Add(deltaUs > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)deltaUs);
    }
    const bool firstPass = s_prevWaitEntryQpc == 0;
    s_prevWaitEntryQpc = now;

    if (!firstPass) {
        uint32_t sims = s_passSimFrames + s_passCatchupFrames;
        const Rollback::HoldCause cause = s_passHoldCause;
        const bool hold = (sims == 0 && cause != Rollback::HoldCause::None);
        if (sims == 0 && cause == Rollback::HoldCause::None) {
            // Vanilla-dispatch pass (offline/menu/frontend): the game ran its
            // single native step this pass.
            sims = 1;
        }

        if (hold) {
            s_secHolds[HoldCauseIndex(cause)] += 1;
        } else {
            s_secSimFrames += sims;
        }

        Rollback::RunStateInputs in{};
        in.kind = hold ? Rollback::WorkKind::Hold : Rollback::WorkKind::Advance;
        in.hold = cause;
        in.external_suspension = (cause == Rollback::HoldCause::ExternalSuspension);
        in.recoverable_debt_frames = s_debt.Owed();
        s_runState = Rollback::ClassifyRunState(in);
        s_holdLedger.Observe(s_runState, TicksToUs(now), 0, 0);

        // Track the deepest replay of the window for the STAT rb_max field.
        if (Rollback::RollbackSession_IsActive()) {
            Rollback::RollbackTimesyncTelemetry telemetry{};
            Rollback::RollbackSession_GetTimesyncTelemetry(&telemetry);
            if (telemetry.last_rollback_replay_length > s_secMaxReplayLen) {
                s_secMaxReplayLen = telemetry.last_rollback_replay_length;
            }
            s_lastTelemetryFrame = telemetry.rb_frame_current;
        }
    }

    if (s_secStartQpc == 0) {
        ResetSecondWindow(now);
    } else if (now - s_secStartQpc >= s_qpf) {
        EmitStat(now);
    }

    // Open the next pass.
    s_passSimFrames = 0;
    s_passCatchupFrames = 0;
    s_passCatchupTries = 0;
    s_passHoldCause = Rollback::HoldCause::None;
}

}  // anonymous namespace

// ============================================================================
// The detour target — runs once per Game_MainLoop pass at the limiter site.
// ============================================================================

void __cdecl FrameScheduler_WaitForNextFrame() {
    if (!s_installed) {
        return;
    }

    uint64_t now = Qpc();

    // QPC regression / discontinuity: reset spacing state, log, continue
    // visibly smooth (§2.8.2).
    if (s_lastNowQpc != 0 && now < s_lastNowQpc) {
        s_clock.RebaseTo(now);
        ++s_qpcRegressionCount;
        if (s_qpcRegressionCount <= 3) {
            Rollback::NetplayLog_Write("PACE", s_lastTelemetryFrame,
                "WARN QPC regression #%u: spacing state rebased",
                s_qpcRegressionCount);
        }
    }
    s_lastNowQpc = now;

    FinishPassAccounting(now);

    // Follow the cadence preference (menu toggle / session override). The
    // deadline is continuous across a profile switch (rebase-never-reset).
    const bool compat = !IsFrameLimiter60FpsPatchEnabled();
    if (compat != s_compat58) {
        s_compat58 = compat;
        s_clock.Configure(s_qpf, compat ? Sched::kCadenceCompat58
                                        : Sched::kCadenceProper60);
        Rollback::NetplayLog_Write("PACE", s_lastTelemetryFrame,
            "Cadence profile -> %s",
            compat ? Sched::kCadenceCompat58.name : Sched::kCadenceProper60.name);
    }

    // §2.8.4 pace slew: evaluated every pass INCLUDING stalls (the stalled
    // side is exactly who needs releasing). Stale sample ⇒ instant release.
    const bool slewFresh =
        s_slewSampleQpc != 0 && now >= s_slewSampleQpc &&
        TicksToUs(now - s_slewSampleQpc) <= kSlewFreshnessUs;
    const int32_t slewPpm = s_slew.Update(slewFresh, s_slewPeerDepth, s_slewLocalDepth);

    // Effective period: base cadence step (Bresenham-exact), divided by the
    // manual speed scale (replay fast-forward / mod-menu slider — the one
    // speed authority applies it HERE, not in the tick hook), plus the legacy
    // pacing shim's ±1 ms adjust, minus the slew (§2.8.2 application point:
    // next increment only — continuous, no jumps).
    const uint64_t baseStep = s_clock.NextBaseStep();
    double eff = (double)baseStep;
    float manual = GetGlobalTickScale();
    if (manual < 0.1f) manual = 0.1f;
    if (manual > 32.0f) manual = 32.0f;
    if (manual != 1.0f) {
        eff /= (double)manual;
    }
    if (s_periodAdjustUs != 0.0f) {
        eff += (double)s_periodAdjustUs * (double)s_qpf / 1.0e6;
    }
    if (slewPpm > 0) {
        eff *= (1.0 - (double)slewPpm / 1.0e6);
    }
    const double minStep = (double)(s_qpf / 1000ull);  // 1 ms floor
    const double maxStep = (double)s_qpf;              // 1 s ceiling
    if (eff < minStep) eff = minStep;
    if (eff > maxStep) eff = maxStep;
    s_lastSpeedScale = (float)((double)baseStep / eff);

    const Sched::DeadlineClock::AdvanceResult adv =
        s_clock.Advance(now, (uint64_t)eff);

    if (adv.rebase) {
        ++s_rebaseCount;
        // The single entry converting rebase lateness into debt, gated on the
        // typed cause of the last hold (§2.8.5). External stalls convert 0.
        const uint32_t converted = s_debt.ApplyRebasedFrames(adv.rebased_frames);
        if (s_lastRebaseLogQpc == 0 || now - s_lastRebaseLogQpc >= s_qpf) {
            s_lastRebaseLogQpc = now;
            Rollback::NetplayLog_Write("PACE", s_lastTelemetryFrame,
                "Deadline rebase #%u: late_frames=%u debt_converted=%u "
                "last_hold=%s owed=%u",
                s_rebaseCount,
                adv.rebased_frames,
                converted,
                Rollback::HoldCauseName(s_debt.LastHoldCause()),
                s_debt.Owed());
        }
    }

    if (adv.wait_until != 0) {
        // Hybrid wait: Sleep(1) until within 2 ms of the deadline, then spin.
        // timeBeginPeriod(1) is process-lifetime active (DXLib init), so the
        // worst-case oversleep (~1–2 ms) is absorbed by the spin tail.
        const uint64_t spinThresholdTicks = UsToTicks(2000);
        uint64_t lastSeen = now;
        uint32_t deadIters = 0;
        for (;;) {
            now = Qpc();
            if (now >= adv.wait_until) break;
            if (adv.wait_until - now > spinThresholdTicks) {
                Sleep(1);
                // Dead-clock latch: QPC frozen across ≥500 Sleep(1) rounds
                // (§2.8.2 PacingClockDead). session2 polls the sticky latch
                // (FrameScheduler_IsPacingClockDead) and fires the fail-closed
                // terminal; here: log loud once, rebase, keep the loop alive
                // rather than wedge the game thread.
                if (now == lastSeen) {
                    if (++deadIters >= 500) {
                        if (!s_deadClockLatched) {
                            s_deadClockLatched = true;
                            LOG_ERROR("[FrameScheduler] PacingClockDead: QPC made no "
                                      "progress across %u sleep rounds", deadIters);
                            Rollback::NetplayLog_Write("PACE", s_lastTelemetryFrame,
                                "ERROR PacingClockDead: QPC frozen; deadline rebased "
                                "(session2 converts this latch into the terminal)");
                        }
                        s_clock.RebaseTo(now);
                        break;
                    }
                } else {
                    lastSeen = now;
                    deadIters = 0;
                }
            } else {
                _mm_pause();
            }
        }
    }

    s_passStartQpc = Qpc();
    s_lastNowQpc = s_passStartQpc;
}

// ============================================================================
// Install / lifecycle
// ============================================================================

bool FrameScheduler_Install() {
    if (s_installed) {
        return true;
    }

    if (!ReadSchedulerEnabledSetting()) {
        LOG_WARN("[FrameScheduler] Disabled via frame_scheduler=0 — legacy "
                 "virtual-clock limiter stays active (R-1 fallback flag)");
        return false;
    }

    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        LOG_ERROR("[FrameScheduler] QueryPerformanceFrequency failed — "
                  "falling back to the legacy limiter");
        return false;
    }
    s_qpf = (uint64_t)freq.QuadPart;

    // §2.8.1: timeBeginPeriod(1) is already process-lifetime active (DXLib
    // init calls timeBeginPeriod(wPeriodMin)); assert via timeGetDevCaps.
    TIMECAPS caps{};
    if (timeGetDevCaps(&caps, sizeof(caps)) == MMSYSERR_NOERROR && caps.wPeriodMin > 1) {
        LOG_WARN("[FrameScheduler] timeGetDevCaps wPeriodMin=%u (>1) — Sleep(1) "
                 "may oversleep; spin tail absorbs it", caps.wPeriodMin);
    }

    uintptr_t addr = 0;
    uint32_t len = 0;
    if (!FindLimiterCluster(&addr, &len)) {
        return false;  // fail loud already logged
    }

    memcpy(s_originalBytes, reinterpret_cast<const void*>(addr), len);

    uint8_t patch[kMaxPatchLen];
    memset(patch, 0x90, sizeof(patch));  // NOP fill
    patch[0] = 0xE8;
    const intptr_t rel =
        (intptr_t)&FrameScheduler_WaitForNextFrame - (intptr_t)(addr + 5);
    memcpy(patch + 1, &rel, 4);

    if (!WriteMemoryBlockSafe(reinterpret_cast<void*>(addr), patch, len)) {
        LOG_ERROR("[FrameScheduler] Failed to write the limiter detour at "
                  "0x%08X — falling back to the legacy limiter", (unsigned)addr);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(addr), len);

    s_patchAddr = addr;
    s_patchLen = len;
    s_compat58 = !IsFrameLimiter60FpsPatchEnabled();
    s_clock.Configure(s_qpf, s_compat58 ? Sched::kCadenceCompat58
                                        : Sched::kCadenceProper60);
    s_installed = true;

    LOG_INFO("[FrameScheduler] Limiter detour installed at 0x%08X (len=%u, "
             "head+jge+loop+jl cluster): cadence=%s qpf=%llu period_ticks=%llu "
             "— mod owns the clock (INV-5/INV-17); Hook_GetTick pinned to 1.0",
             (unsigned)addr,
             len,
             s_compat58 ? Sched::kCadenceCompat58.name : Sched::kCadenceProper60.name,
             (unsigned long long)s_qpf,
             (unsigned long long)s_clock.PeriodTicks());
    Rollback::NetplayLog_Write("PACE", -1,
        "FrameScheduler installed: addr=0x%08X len=%u cadence=%s",
        (unsigned)addr, len,
        s_compat58 ? Sched::kCadenceCompat58.name : Sched::kCadenceProper60.name);
    return true;
}

bool FrameScheduler_IsInstalled() {
    return s_installed;
}

bool FrameScheduler_IsPacingClockDead() {
    return s_deadClockLatched;
}

void FrameScheduler_Shutdown() {
    if (!s_installed) {
        return;
    }
    s_installed = false;
    if (s_patchAddr != 0 && s_patchLen != 0) {
        WriteMemoryBlockSafe(reinterpret_cast<void*>(s_patchAddr),
                             s_originalBytes, s_patchLen);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(s_patchAddr), s_patchLen);
        LOG_INFO("[FrameScheduler] Limiter detour removed (original bytes restored)");
    }
    s_patchAddr = 0;
    s_patchLen = 0;
}

// ============================================================================
// Per-pass plan notifications
// ============================================================================

void FrameScheduler_NotifyHold(Rollback::HoldCause cause, bool create_debt) {
    if (cause == Rollback::HoldCause::None) {
        return;
    }
    // First cause of a pass wins (the dispatcher may be re-entered several
    // times while returning -1 within one pass).
    if (s_passHoldCause == Rollback::HoldCause::None) {
        s_passHoldCause = cause;
        s_debt.OnHold(cause, create_debt);
    }
}

void FrameScheduler_NotifySimFrame() {
    ++s_passSimFrames;
}

bool FrameScheduler_TryTakeCatchupFrame(int32_t max_depth_headroom) {
    if (!s_installed) return false;
    if (s_debt.Owed() == 0) return false;
    if (max_depth_headroom <= 0) return false;
    if (s_passCatchupTries >= Sched::CadenceDebtLedger::kMaxCatchupExtraPerPass) {
        return false;
    }
    // §2.8.3 hidden-work wall budget, re-checked between iterations. On
    // overrun the batch collapses: the current iteration becomes the visible
    // frame (INV-21) — refusing here does exactly that.
    if (s_passStartQpc != 0) {
        const uint64_t elapsedUs = TicksToUs(Qpc() - s_passStartQpc);
        if (elapsedUs >= kCatchupWallBudgetUs) {
            return false;
        }
    }
    ++s_passCatchupTries;
    return true;
}

void FrameScheduler_NotifyCatchupFrame() {
    ++s_passCatchupFrames;
    // Partial batches still credit the hidden frames they ran (un-crediting
    // is a self-sustaining trap, §2.8.3).
    s_debt.Consume(1);
}

void FrameScheduler_DiscardExternalDebt(const char* reason) {
    if (s_debt.Owed() != 0) {
        Rollback::NetplayLog_Write("PACE", s_lastTelemetryFrame,
            "CadenceDebt discarded (external): owed=%u reason=%s",
            s_debt.Owed(), reason ? reason : "?");
    }
    s_debt.DiscardExternal();
}

// ============================================================================
// Speed inputs
// ============================================================================

void FrameScheduler_SetPeriodAdjustUs(float adjust_us, const char* reason) {
    float clamped = adjust_us;
    if (clamped > 1000.0f) clamped = 1000.0f;
    if (clamped < -1000.0f) clamped = -1000.0f;
    if (fabsf(clamped - s_periodAdjustUs) >= 100.0f) {
        Rollback::NetplayLog_Verbose("PACE", s_lastTelemetryFrame,
            "Period adjust %.0fus -> %.0fus reason=%s",
            s_periodAdjustUs, clamped, reason ? reason : "?");
    }
    s_periodAdjustUs = clamped;
}

void FrameScheduler_SubmitPeerDepthSample(int32_t peer_depth, int32_t local_depth) {
    s_slewSampleQpc = Qpc();
    s_slewPeerDepth = peer_depth;
    s_slewLocalDepth = local_depth;
}

void FrameScheduler_OnSessionReset(const char* reason) {
    s_periodAdjustUs = 0.0f;
    s_slew.Reset();
    s_slewSampleQpc = 0;
    s_debt.Reset();
    s_holdLedger.Reset();
    s_secRollbackBase = -1;
    s_lastTelemetryFrame = -1;
    // The deadline clock itself is deliberately untouched: clock state
    // rebases, never resets (INV-17 — the DECOMP §2.3 backward-snap class).
    Rollback::NetplayLog_Write("PACE", -1,
        "FrameScheduler session reset: adjust/slew/debt cleared, deadline "
        "continuous, reason=%s", reason ? reason : "?");
}

// ============================================================================
// Queries
// ============================================================================

float FrameScheduler_GetSpeedScale() {
    return s_installed ? s_lastSpeedScale : 1.0f;
}

int32_t FrameScheduler_GetSlewPpm() {
    return s_slew.Ppm();
}

void FrameScheduler_GetSnapshot(FrameSchedulerSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->installed = s_installed;
    out->compat_58 = s_compat58;
    out->speed_scale = FrameScheduler_GetSpeedScale();
    out->period_adjust_us = s_periodAdjustUs;
    out->slew_ppm = s_slew.Ppm();
    out->debt_frames = s_debt.Owed();
    out->rebase_count = s_rebaseCount;
    out->present_p50_us = s_lastP50Us;
    out->present_p99_us = s_lastP99Us;
    out->holds_prediction = s_lastHolds[HoldCauseIndex(Rollback::HoldCause::PredictionLimit)];
    out->holds_lifecycle = s_lastHolds[HoldCauseIndex(Rollback::HoldCause::LifecycleBoundary)];
    out->holds_local_input = s_lastHolds[HoldCauseIndex(Rollback::HoldCause::LocalInputMissing)];
    out->holds_external = s_lastHolds[HoldCauseIndex(Rollback::HoldCause::ExternalSuspension)];
    out->run_state = s_runState;
}
