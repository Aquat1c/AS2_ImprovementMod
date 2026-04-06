/**
 * Alice Senki 2 - Input Synchronization Hooks
 * 
 * Hooks the game's input sync functions for rollback integration.
 * 
 * Two operating modes:
 * 
 * 1. PASSTHROUGH (default): All hooks call the original functions.
 *    Used during vanilla netplay (CharSel, lobby) and offline play.
 * 
 * 2. ROLLBACK (s_rollback_active = true): Vanilla sync is suppressed.
 *    Hook_InputDispatcher drives frame stepping via RollbackSession's
 *    two-phase API (BeginFrame + ProcessNextEvent).
 *    Send/Recv/GetSync are no-ops (GekkoNet handles networking).
 */

#include "input_sync_hooks.h"
#include "rollback_session.h"
#include "charsel_sync.h"
#include "netplay_hooks.h"
#include "as2_rollback.h"
#include "log_window.h"
#include "input_system.h"
#include "session_manager.h"
#include "adaptive_delay.h"
#include "visual_smoothing.h"
#include "test_harness.h"
#include <MinHook.h>
#include <windows.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>

// ============================================================================
// GAME FUNCTION ADDRESSES
// ============================================================================

#define ADDR_INPUT_DISPATCHER     0x5625E0
#define ADDR_SEND_INPUT           0x562450
#define ADDR_RECV_INPUT           0x5623D0
#define ADDR_GET_SYNC_INPUT       0x5624E0
#define ADDR_RESET_INPUT          0x562550
#define ADDR_ADVANCE_FRAME        0x562760

// Vanilla timeout counters — must be kept at 0 to prevent auto-disconnect
#define ADDR_HOST_TIMEOUT_CTR     0x8EA200   // dword_8EA200
#define ADDR_CLIENT_TIMEOUT_CTR   0x8EA3A8   // g_NetTimeoutCounter

// ============================================================================
// FUNCTION TYPEDEFS
// ============================================================================

typedef int  (__cdecl *InputDispatcher_t)(int16_t* out);
typedef int  (__cdecl *SendInputPacket_t)(int frame);
typedef void (__cdecl *RecvInputPacket_t)();
typedef int  (__cdecl *GetSyncInput_t)(int frame, int16_t* out);
typedef int  (__cdecl *AdvanceFrame_t)();
typedef int  (__cdecl *MatchSyncInit_t)();

// ============================================================================
// ORIGINAL FUNCTION POINTERS
// ============================================================================

static InputDispatcher_t g_origInputDispatcher = nullptr;
static SendInputPacket_t g_origSendInputPacket = nullptr;
static RecvInputPacket_t g_origRecvInputPacket = nullptr;
static GetSyncInput_t    g_origGetSyncInput    = nullptr;
static AdvanceFrame_t    g_origAdvanceFrame     = nullptr;
static MatchSyncInit_t   g_origMatchSyncInit   = nullptr;

// ============================================================================
// INTERNAL STATE
// ============================================================================

static bool     s_input_hooks_installed = false;
static bool     s_session_terminated    = false;

// Rollback mode
static bool     s_rollback_active       = false;
static bool     s_needs_gekko_update    = true;    // true = call BeginFrame on next dispatch
static bool     s_warned_missing_rollback_inputs = false;

// Runtime delay hotkey state (- = decrease, = = increase)
static bool     s_minus_was_down        = false;
static bool     s_equals_was_down       = false;

// Load barrier freeze — game reached substate 3 but bootstrap isn't complete
static bool     s_load_barrier_freeze   = false;

// Statistics
static uint32_t s_frames_processed = 0;
static uint32_t s_vanilla_frames   = 0;

// Timesync v2: two-layer pacing with EMA smoothing, hysteresis, and jitter adaptation.
//  Layer 1 — Timing nudge: sub-ms busy-wait proportional to smoothed ahead.
//  Layer 2 — Hard skip: skip 1-2 frames when significantly ahead (with hysteresis).
static int      s_timesync_skip_budget      = 0;     // Remaining skips to consume (0..kMaxSkipBudget)
static float    s_timesync_last_fresh_ahead = 0.0f;  // Raw GekkoNet reading at last evaluation
static float    s_timesync_nudge_us         = 0.0f;  // Current nudge amount in microseconds
static uint32_t s_timesync_nudge_count      = 0;     // Total frames where nudge was applied
static float    s_timesync_smoothed_ahead   = 0.0f;  // EMA-filtered frames_ahead
static float    s_timesync_jitter           = 0.0f;  // EMA of |raw - smoothed| deviation
static bool     s_timesync_skip_armed       = false; // Hysteresis latch for hard skip
static int      s_timesync_pathological_ctr = 0;     // Consecutive frames above pathological threshold
static float    s_timesync_effective_dz     = 0.0f;  // Current effective deadzone (for HUD)

// Logging throttle for periodic state dumps
static uint32_t s_log_throttle_counter = 0;
static constexpr uint32_t kLogInterval = 300; // Every 5 seconds at 60fps
static uint32_t s_lastCharSelDispatchMode = UINT32_MAX;
static uint32_t s_lastCharSelDispatchSubstate = UINT32_MAX;

// ============================================================================
// HELPERS
// ============================================================================

// ── QPC busy-wait for sub-millisecond timing nudge ──────────────────────
// Used by the timesync layer to gently slow down when ahead. Sleep() has
// ~1-15ms granularity, so busy-wait with QueryPerformanceCounter is the
// only way to get precise sub-frame delays (100-2000µs range).
static LARGE_INTEGER s_qpc_freq = {};   // Cached, set once

static void InitTimesyncTimer() {
    QueryPerformanceFrequency(&s_qpc_freq);
}

static void BusyWaitMicroseconds(float us) {
    if (us <= 0.0f || s_qpc_freq.QuadPart == 0) return;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    const double targetTicks = (double)start.QuadPart +
                               (double)us * (double)s_qpc_freq.QuadPart / 1000000.0;

    // Poll the socket periodically during the wait so remote packets
    // arrive sooner — reduces frames-ahead on the next BeginFrame.
    constexpr double kPollIntervalUs = 500.0; // poll every ~500µs
    double nextPollTicks = (double)start.QuadPart +
                           kPollIntervalUs * (double)s_qpc_freq.QuadPart / 1000000.0;

    do {
        QueryPerformanceCounter(&now);
        if ((double)now.QuadPart >= nextPollTicks) {
            SessionManager::DriveSocketPoll();
            nextPollTicks = (double)now.QuadPart +
                            kPollIntervalUs * (double)s_qpc_freq.QuadPart / 1000000.0;
        }
    } while ((double)now.QuadPart < targetTicks);
}

static inline void ResetVanillaTimeouts() {
    // Prevent the game's vanilla timeout logic from firing.
    // The game checks these counters > 1800 → sets Netplay_IsConnected = 0.
    *reinterpret_cast<uint32_t*>(ADDR_HOST_TIMEOUT_CTR)   = 0;
    *reinterpret_cast<uint32_t*>(ADDR_CLIENT_TIMEOUT_CTR) = 0;
}

// Returns true when the mod owns synchronization (§14):
//   - During Gameplay: RollbackSession active → suppress vanilla Mode 8 lockstep
//   - During CharSel: CharSelSync active → suppress vanilla Mode 6 lockstep
//   - During load barrier: freeze engaged → suppress vanilla netplay
//   - During Mode 8 loading gap: session active but no sync system yet
// When none of these apply, vanilla passthrough is used.
static inline bool IsModOwnedSync() {
    static bool s_lastModOwned = false;
    static int  s_lastReason   = -1;

    bool owned = false;
    int  reason = 0;

    if (s_rollback_active && RollbackSession::IsActive()) {
        owned = true; reason = 1;
    } else if (CharSelSync::IsActive()) {
        owned = true; reason = 2;
    } else if (s_load_barrier_freeze) {
        owned = true; reason = 3;
    } else {
        // Cover the gap between CharSel end and rollback start (Mode 8 substates 0-2).
        // During this gap, mod-owned sessions MUST suppress any vanilla netplay path.
        SessionManager::Snapshot snap{};
        if (SessionManager::GetSnapshot(&snap) && snap.active && !snap.has_error) {
            owned = true; reason = 4;
        }
    }

    // Log transitions
    if (owned != s_lastModOwned || (owned && reason != s_lastReason)) {
        static const char* reasonNames[] = { "none", "rollback", "charsel", "load_barrier", "session_gap" };
        LOG_NET_INFO("[InputSync] ModOwnedSync: %s -> %s (reason=%s)",
                     s_lastModOwned ? "MOD" : "VANILLA",
                     owned ? "MOD" : "VANILLA",
                     reasonNames[reason]);
        s_lastModOwned = owned;
        s_lastReason   = reason;
    }
    return owned;
}

static const char* GetRollbackStateNameForLog() {
    switch (RollbackSession::GetState()) {
    case RollbackSession::State::Inactive: return "Inactive";
    case RollbackSession::State::Syncing:  return "Syncing";
    case RollbackSession::State::Running:  return "Running";
    case RollbackSession::State::Error:    return "Error";
    default:                               return "Unknown";
    }
}

// Decompose a 16-bit input bitmask into human-readable button names.
// Returns something like "Left+A+Start" or "none". buf must be >= 64 bytes.
static const char* DecomposeInput(uint16_t input, char* buf, size_t bufSize) {
    if (input == 0) { snprintf(buf, bufSize, "none"); return buf; }
    static const struct { uint16_t mask; const char* name; } kButtons[] = {
        {0x0001, "Up"}, {0x0002, "Dn"}, {0x0004, "Lt"}, {0x0008, "Rt"},
        {0x0010, "A"},  {0x0020, "B"},   {0x0040, "C"},  {0x0080, "D"},
        {0x0100, "St"}, {0x0200, "Se"},
    };
    buf[0] = '\0';
    size_t pos = 0;
    for (auto& b : kButtons) {
        if (input & b.mask) {
            if (pos > 0 && pos + 1 < bufSize) buf[pos++] = '+';
            size_t len = strlen(b.name);
            if (pos + len < bufSize) { memcpy(buf + pos, b.name, len); pos += len; }
        }
    }
    buf[pos] = '\0';
    return buf;
}

static inline bool IsCharSelLockstepDispatchSubstate(uint32_t substate) {
    // Sub 7 (Preview / stage grid) and Sub 11 (Stage / auto-pick) are excluded:
    // they do NOT call Input_TryGetNextFrame, so Hook_InputDispatcher never fires
    // for them. They are handled as direct-buffer substates in Hook_InputProcess.
    return substate == CHARSEL_SUB_SELECT  ||
           substate == CHARSEL_SUB_CANCEL  ||
           substate == CHARSEL_SUB_CONFIRM;
}

static const char* GetCharSelDispatchSubstateName(uint32_t substate) {
    switch (substate) {
    case CHARSEL_SUB_INIT:        return "Init";
    case CHARSEL_SUB_FADEIN:      return "FadeIn";
    case CHARSEL_SUB_SELECT:      return "Select";
    case CHARSEL_SUB_CANCEL:      return "Cancel";
    case CHARSEL_SUB_CONFIRM:     return "Confirm";
    case CHARSEL_SUB_TRANS5:      return "Trans5";
    case CHARSEL_SUB_TRANS6:      return "Trans6";
    case CHARSEL_SUB_PREVIEW:     return "Preview";
    case CHARSEL_SUB_STAGE_INTRO: return "StageIntro";
    case CHARSEL_SUB_LOADING:     return "Loading";
    case CHARSEL_SUB_PREMATCH:    return "PreMatch";
    case CHARSEL_SUB_STAGE:       return "Stage";
    case CHARSEL_SUB_BACK_MENU:   return "BackMenu";
    case CHARSEL_SUB_BACK_LOBBY:  return "BackLobby";
    case CHARSEL_SUB_TO_MATCH:    return "ToMatch";
    default:                      return "Unknown";
    }
}

static int GetCharSelLocalInputPlayerIndex() {
    // Both Host and Client humans use primary physical controller 0.
    // CharSelSync::ConsumeCurrentFrame automatically routes localInput to P1 for Host and P2 for Client.
    // If Control Swap is enabled, it means they are using physical controller 1 instead.
    return InputSystem_GetControlSwap() ? 1 : 0;
}

static void PublishRollbackInputs(int16_t* out) {
    uint16_t p1Input = 0;
    uint16_t p2Input = 0;
    const bool haveInputs = RollbackSession::GetCurrentInputs(&p1Input, &p2Input);
    const int rbFrame = RollbackSession::GetCurrentFrame();
    const uint32_t traceEventId = AS2_NextRollbackTraceEventId();
    const int16_t beforeOut[2] = { out[0], out[1] };

    if (haveInputs) {
        s_warned_missing_rollback_inputs = false;
    } else if (!s_warned_missing_rollback_inputs) {
        LOG_NET_WARN("[InputSync] Rollback advance without synchronized inputs; injecting zeros");
        s_warned_missing_rollback_inputs = true;
    }

    out[0] = (int16_t)p1Input;
    out[1] = (int16_t)p2Input;

    volatile uint32_t* pFrameWrite = reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_WRITE_IDX);
    const uint32_t writeFrame = *pFrameWrite;
    const uint16_t beforeP1History = *reinterpret_cast<volatile uint16_t*>(ADDR_P1_INPUT_HISTORY + (writeFrame * sizeof(uint16_t)));
    const uint16_t beforeP2History = *reinterpret_cast<volatile uint16_t*>(ADDR_P2_INPUT_HISTORY + (writeFrame * sizeof(uint16_t)));

    *reinterpret_cast<volatile uint16_t*>(ADDR_P1_INPUT_HISTORY + (writeFrame * sizeof(uint16_t))) = p1Input;
    *reinterpret_cast<volatile uint16_t*>(ADDR_P2_INPUT_HISTORY + (writeFrame * sizeof(uint16_t))) = p2Input;
    *pFrameWrite = writeFrame + 1;
    const uint16_t afterP1History = *reinterpret_cast<volatile uint16_t*>(ADDR_P1_INPUT_HISTORY + (writeFrame * sizeof(uint16_t)));
    const uint16_t afterP2History = *reinterpret_cast<volatile uint16_t*>(ADDR_P2_INPUT_HISTORY + (writeFrame * sizeof(uint16_t)));
    const uint32_t afterWriteFrame = *pFrameWrite;

    AS2_LogRollbackTraceMessage("publish",
                                rbFrame,
                                traceEventId,
                                "publish valid=%d write=%u->%u p1=0x%04X p2=0x%04X",
                                haveInputs ? 1 : 0,
                                writeFrame,
                                afterWriteFrame,
                                p1Input,
                                p2Input);
    AS2_DumpRollbackTraceBytes("publish", rbFrame, traceEventId, "dispatcher_out", 0, beforeOut, sizeof(beforeOut), out, sizeof(int16_t) * 2);
    AS2_DumpRollbackTraceBytes("publish", rbFrame, traceEventId, "p1_history_slot", ADDR_P1_INPUT_HISTORY + (writeFrame * sizeof(uint16_t)), &beforeP1History, sizeof(beforeP1History), &afterP1History, sizeof(afterP1History));
    AS2_DumpRollbackTraceBytes("publish", rbFrame, traceEventId, "p2_history_slot", ADDR_P2_INPUT_HISTORY + (writeFrame * sizeof(uint16_t)), &beforeP2History, sizeof(beforeP2History), &afterP2History, sizeof(afterP2History));
    AS2_DumpRollbackTraceBytes("publish", rbFrame, traceEventId, "write_idx", ADDR_FRAME_WRITE_IDX, &writeFrame, sizeof(writeFrame), &afterWriteFrame, sizeof(afterWriteFrame));
    TestHarness::RecordInputs((uint32_t)((rbFrame >= 0) ? rbFrame : 0), p1Input, p2Input);

    if (rbFrame < 120 || p1Input != 0 || p2Input != 0 || (writeFrame % 60) == 0) {
        LOG_NET_DEBUG("[InputSync] Rollback publish: host=%d rb_frame=%d write=%u->%u p1=0x%04X p2=0x%04X valid=%d",
            SessionManager::IsHost() ? 1 : 0,
            rbFrame,
            writeFrame,
            writeFrame + 1,
            p1Input,
            p2Input,
            haveInputs ? 1 : 0);
    }
}

// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

/**
 * Hook for sub_5625E0 - Main Input Dispatcher
 * 
 * Called by the game in: while (!Input_TryGetNextFrame(buf)) { simulate(); }
 * Return 0 = process frame, non-zero = break loop.
 *
 * In rollback mode:
 *   1. First call per visual frame: BeginFrame(localInput)
 *   2. Each call: ProcessNextEvent() → FrameAdvance returns 0, NoMoreEvents returns -1
 */
static int __cdecl Hook_InputDispatcher(int16_t* out) {
    s_frames_processed++;
    s_log_throttle_counter++;
    
    if (!out) return -1;

    // Periodic state dump for debugging (every 5s)
    if (s_log_throttle_counter >= kLogInterval) {
        s_log_throttle_counter = 0;
        uint32_t gameType = *reinterpret_cast<volatile uint32_t*>(ADDR_GAME_TYPE);
        uint32_t gameMode = *reinterpret_cast<volatile uint32_t*>(ADDR_GAME_MODE);
        uint32_t subState = *reinterpret_cast<volatile uint32_t*>(ADDR_SUB_STATE);
        uint8_t  role = NetplayHooks::GetConnectionRole();
        uint8_t  connected = NetplayHooks::IsConnected() ? 1 : 0;
        uint32_t simFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_SIMULATION);
        uint32_t displayFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_DISPLAY);
        uint32_t writeFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_WRITE_IDX);
        uint32_t netFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_NET_IDX);
        LOG_NET_DEBUG("[InputSync] State: mode=%u sub=%u type=%u connected=%u role=%u rollback=%d(%s) charsel=%d barrier=%d sim=%u disp=%u write=%u net=%u rb_frame=%d match_visual=%u dispatcher_calls=%u",
            gameMode, subState, gameType, connected, role,
            s_rollback_active ? 1 : 0,
            GetRollbackStateNameForLog(),
            CharSelSync::IsActive() ? 1 : 0,
            s_load_barrier_freeze ? 1 : 0,
            simFrame,
            displayFrame,
            writeFrame,
            netFrame,
            RollbackSession::GetCurrentFrame(),
            AS2_GetMatchVisualFrame(),
            s_frames_processed);
    }
    
    // ── Load barrier freeze ─────────────────────────────────────────
    // Game reached Mode 8 substate 3 but bootstrap hasn't completed.
    // Freeze gameplay (return -1) while keeping game loop alive for
    // SessionManager updates, packet exchange, and ImGui rendering.
    if (s_load_barrier_freeze) {
        ResetVanillaTimeouts();
        return -1;
    }
    
    // ── Rollback-driven frame stepping ──────────────────────────────
    if (s_rollback_active && RollbackSession::IsActive()) {
        
        // ── Runtime delay hotkeys (- = decrease, = = increase) ───────
        // With the GekkoNet SetDelay fix, mid-match changes are now safe.
        // Hotkeys initiate a negotiated delay change via AdaptiveDelay.
        {
            bool minusDown  = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
            bool equalsDown = (GetAsyncKeyState(VK_OEM_PLUS)  & 0x8000) != 0;  // VK_OEM_PLUS is the =/+ key
            if (minusDown && !s_minus_was_down) {
                if (AdaptiveDelay::IsActive()) {
                    AdaptiveDelay::RequestManualChange(-1);
                } else {
                    // Fallback: update preference for next match
                    int cur = NetplayHooks::GetNetplayFrameDelay();
                    if (cur > 0) {
                        NetplayHooks::SetNetplayFrameDelay(cur - 1);
                        LOG_NET_INFO("[InputSync] Delay preference set to %d (next match) (-)", cur - 1);
                    }
                }
            }
            if (equalsDown && !s_equals_was_down) {
                if (AdaptiveDelay::IsActive()) {
                    AdaptiveDelay::RequestManualChange(+1);
                } else {
                    int cur = NetplayHooks::GetNetplayFrameDelay();
                    if (cur < 6) {
                        NetplayHooks::SetNetplayFrameDelay(cur + 1);
                        LOG_NET_INFO("[InputSync] Delay preference set to %d (next match) (=)", cur + 1);
                    }
                }
            }
            s_minus_was_down  = minusDown;
            s_equals_was_down = equalsDown;
        }

        // ── Two-Layer Timesync v2 ──────────────────────────────────
        //
        // GekkoNet's frames_ahead = rolling 26-frame average of
        //   (local_advantage - remote_advantage) / 2.
        // Positive = we're ahead, negative = we're behind.
        //
        // Improvements over v1:
        //   1. EMA smoothing — dampens jitter in the GekkoNet signal
        //   2. Hysteresis on hard skip — separate arm/disarm thresholds
        //   3. Jitter-adaptive deadzone — wider deadzone during noise
        //   4. Bounded multi-skip budget — faster convergence on large gaps
        //   5. Pathological state detection — nudge boost if stuck ahead
        //
        // LAYER 1 — Timing Nudge (primary, smooth):
        //   Busy-wait proportional to (smoothed_ahead - deadzone).
        //   Deadzone widens when jitter is high, preventing false nudges.
        //
        // LAYER 2 — Hard Skip (safety valve, rare):
        //   Arms when smoothed_ahead >= kHardSkipArm.
        //   Disarms when smoothed_ahead < kHardSkipDisarm (hysteresis).
        //   Budget = clamp(floor(smoothed - 1.0), 1, kMaxSkipBudget).
        //   Each skip calls NetworkPoll() to drain packets.
        //
        static constexpr float kSmoothAlpha         = 0.15f;   // EMA weight for new ahead samples
        static constexpr float kJitterAlpha         = 0.10f;   // EMA weight for jitter tracking
        static constexpr float kNudgeDeadzoneBase   = 0.15f;   // Minimum deadzone (frames)
        static constexpr float kJitterDeadzoneScale = 0.5f;    // Jitter contribution to deadzone
        static constexpr float kNudgeGain           = 800.0f;  // µs per 1.0 excess above deadzone
        static constexpr float kMaxNudgeUs          = 5000.0f; // Cap at 5ms (~30% of a frame)
        static constexpr float kHardSkipArm         = 2.0f;    // Arm hard skip above this
        static constexpr float kHardSkipDisarm      = 1.5f;    // Disarm hard skip below this
        static constexpr int   kMaxSkipBudget       = 3;       // Max frames to skip per cycle
        static constexpr float kPathologicalThresh  = 3.5f;    // Sustained-ahead alarm level
        static constexpr int   kPathologicalFrames  = 120;     // 2s at 60fps before boost kicks in
        static constexpr float kPathologicalBoost   = 2.0f;    // Nudge multiplier when pathological

        // Consume hard-skip budget before BeginFrame (one skip per dispatcher call)
        if (s_needs_gekko_update && s_timesync_skip_budget > 0) {
            RollbackSession::NetworkPoll();
            s_timesync_skip_budget--;
            RollbackSession::RecordTimesyncSkip();
            LOG_NET_DEBUG("[InputSync] Timesync: hard skip (smoothed=%.2f raw=%.2f budget_left=%d)",
                         s_timesync_smoothed_ahead, s_timesync_last_fresh_ahead, s_timesync_skip_budget);
            ResetVanillaTimeouts();
            return -1;
        }

        // Phase 1: once per visual frame, feed input and fetch events
        if (s_needs_gekko_update) {
            InputSystem_Update();
            uint16_t localInput = InputSystem_GetInput(0);
            const int rbFrame = RollbackSession::GetCurrentFrame();
            const float preAhead = RollbackSession::GetFramesAhead();
            const uint32_t traceEventId = AS2_NextRollbackTraceEventId();
            if (rbFrame < 120 || localInput != 0 || (rbFrame % 60) == 0) {
                LOG_NET_DEBUG("[InputSync] Rollback sample: host=%d rb_frame=%d local=0x%04X ahead=%.2f",
                    SessionManager::IsHost() ? 1 : 0,
                    rbFrame,
                    localInput,
                    preAhead);
            }
            AS2_LogRollbackTraceMessage("dispatcher-begin",
                                        rbFrame,
                                        traceEventId,
                                        "sample local=0x%04X ahead=%.2f needs_update=%d",
                                        localInput,
                                        preAhead,
                                        s_needs_gekko_update ? 1 : 0);
            AS2_DumpRollbackTraceBytes("dispatcher-begin", rbFrame, traceEventId, "local_input_sample", 0, nullptr, 0, &localInput, sizeof(localInput));
            RollbackSession::BeginFrame(localInput);
            s_needs_gekko_update = false;
        }
        
        // Phase 2: process events until FrameAdvance or exhausted
        RollbackSession::EventResult result = RollbackSession::ProcessNextEvent();
        
        if (result == RollbackSession::EventResult::FrameAdvance) {
            PublishRollbackInputs(out);
            ResetVanillaTimeouts();
            
            // FIX: Clear per-frame temp data before the game processes this frame.
            // In normal play, Match_ClearPerFrameTempData (sub_4C3BE0) zeros
            // 68 bytes at match+0x700 at the top of Game_Update_MatchLoop,
            // OUTSIDE the while-loop.  During rollback resimulation the while-loop
            // iterates multiple times without returning to the outer function, so
            // the clearing never runs between resim frames.  Clearing here ensures
            // each resim frame starts with zeroed temp data, matching normal play.
            // (AS2_CaptureCurrentState also clears during SaveEvent, but this is
            // defense-in-depth for any AdvanceEvent without a preceding SaveEvent.)
            memset(reinterpret_cast<void*>(ADDR_MATCH_PER_FRAME_TEMP), 0, MATCH_PER_FRAME_TEMP_SIZE);
            
            // Detect rollback-end transition for visual smoothing.
            {
                static bool s_was_rolling_back = false;
                bool currently_rolling_back = RollbackSession::IsRollingBack();
                if (s_was_rolling_back && !currently_rolling_back) {
                    VisualSmoothing::OnRollbackEnd();
                }
                s_was_rolling_back = currently_rolling_back;
            }
            
            // Tick adaptive delay on non-rollback confirmed frames.
            if (AdaptiveDelay::IsActive() && !RollbackSession::IsRollingBack()) {
                static uint32_t s_lastRollbackCount = 0;
                RollbackSession::Snapshot snap = {};
                RollbackSession::GetSnapshot(&snap);
                bool hadRollback = (snap.rollback_count != s_lastRollbackCount);
                s_lastRollbackCount = snap.rollback_count;
                AdaptiveDelay::OnFrame(RollbackSession::GetCurrentFrame(), hadRollback);
            }
            
            return 0;   // → game simulates one frame
        }

        if (result == RollbackSession::EventResult::Error) {
            LOG_NET_ERROR("[InputSync] Rollback session entered error state while dispatching frame %d",
                          RollbackSession::GetCurrentFrame());
            ResetVanillaTimeouts();
            return -1;
        }
        
        // No more events this visual frame — apply timesync from
        // fresh frames_ahead (just recalculated by gekko_update_session).
        {
            const float freshAhead = RollbackSession::GetFramesAhead();
            s_timesync_last_fresh_ahead = freshAhead;

            // ── EMA smoothing ──────────────────────────────────────
            // On first frame (smoothed ~0 but fresh may be nonzero),
            // snap the EMA to the raw value to avoid slow ramp-up.
            if (s_timesync_smoothed_ahead == 0.0f && freshAhead != 0.0f) {
                s_timesync_smoothed_ahead = freshAhead;
            } else {
                s_timesync_smoothed_ahead = kSmoothAlpha * freshAhead
                                          + (1.0f - kSmoothAlpha) * s_timesync_smoothed_ahead;
            }

            // ── Jitter tracking ────────────────────────────────────
            const float deviation = freshAhead - s_timesync_smoothed_ahead;
            const float absDeviation = deviation >= 0.0f ? deviation : -deviation;
            s_timesync_jitter = kJitterAlpha * absDeviation
                              + (1.0f - kJitterAlpha) * s_timesync_jitter;

            // ── Jitter-adaptive deadzone ───────────────────────────
            s_timesync_effective_dz = kNudgeDeadzoneBase + s_timesync_jitter * kJitterDeadzoneScale;

            const float smoothed = s_timesync_smoothed_ahead;

            // ── Layer 2: Hard skip with hysteresis ────────────────
            if (!s_timesync_skip_armed && smoothed >= kHardSkipArm) {
                s_timesync_skip_armed = true;
                // Budget: how many frames to skip. Larger gaps → more skips.
                int budget = (int)(smoothed - 1.0f);
                if (budget < 1) budget = 1;
                if (budget > kMaxSkipBudget) budget = kMaxSkipBudget;
                s_timesync_skip_budget = budget;
                LOG_NET_DEBUG("[InputSync] Timesync: arm hard skip (smoothed=%.2f raw=%.2f budget=%d jitter=%.3f)",
                             smoothed, freshAhead, budget, s_timesync_jitter);
            } else if (s_timesync_skip_armed && smoothed < kHardSkipDisarm) {
                s_timesync_skip_armed = false;
            }

            // ── Layer 1: Timing nudge ─────────────────────────────
            if (smoothed > s_timesync_effective_dz) {
                float excess = smoothed - s_timesync_effective_dz;
                s_timesync_nudge_us = excess * kNudgeGain;

                // Boost nudge during pathological sustained-ahead
                if (s_timesync_pathological_ctr > kPathologicalFrames) {
                    s_timesync_nudge_us *= kPathologicalBoost;
                }

                if (s_timesync_nudge_us > kMaxNudgeUs)
                    s_timesync_nudge_us = kMaxNudgeUs;
                BusyWaitMicroseconds(s_timesync_nudge_us);
                s_timesync_nudge_count++;
                if (s_timesync_nudge_count <= 5 || (s_timesync_nudge_count % 120) == 0) {
                    LOG_NET_DEBUG("[InputSync] Timesync: nudge %.0fus (smoothed=%.2f raw=%.2f dz=%.2f jitter=%.3f cnt=%u)",
                                 s_timesync_nudge_us, smoothed, freshAhead,
                                 s_timesync_effective_dz, s_timesync_jitter, s_timesync_nudge_count);
                }
            } else {
                s_timesync_nudge_us = 0.0f;
            }

            // ── Pathological state tracking ───────────────────────
            if (smoothed > kPathologicalThresh) {
                s_timesync_pathological_ctr++;
                if (s_timesync_pathological_ctr == kPathologicalFrames) {
                    LOG_NET_WARN("[InputSync] Timesync: PATHOLOGICAL — sustained ahead=%.2f for %d frames, boosting nudge",
                                 smoothed, kPathologicalFrames);
                }
            } else {
                // Decay counter slowly (don't reset instantly on momentary dip)
                if (s_timesync_pathological_ctr > 0)
                    s_timesync_pathological_ctr--;
            }
        }
        s_needs_gekko_update = true;
        return -1;
    }
    
    // ── Vanilla passthrough ─────────────────────────────────────────
    s_vanilla_frames++;
    AS2_ClearSynchronizedInputOverride();
    
    // ── CharSel lockstep input sync ───────────────────────────────
    // Both sides must have each other's input for frame N before
    // advancing. Hook_InputDispatcher buffers local input, sends it
    // with redundancy, and freezes (returns -1) until the remote side's
    // input for the current frame arrives.
    if (CharSelSync::IsActive()) {
        const uint32_t gameMode = GetGameMode();
        const uint32_t subState = GetSubstate();

        // ── Delay hotkeys during CharSel (- = decrease, = = increase) ──
        // Changes the delay preference that will be used when the rollback
        // session starts via match bootstrap negotiation.
        {
            bool minusDown  = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
            bool equalsDown = (GetAsyncKeyState(VK_OEM_PLUS)  & 0x8000) != 0;
            if (minusDown && !s_minus_was_down) {
                int cur = NetplayHooks::GetNetplayFrameDelay();
                if (cur > 0) {
                    int newDelay = cur - 1;
                    NetplayHooks::SetNetplayFrameDelay(newDelay);
                    LOG_NET_INFO("[InputSync] CharSel delay preference: %d -> %d (takes effect at match start) (-)",
                                 cur, newDelay);
                }
            }
            if (equalsDown && !s_equals_was_down) {
                int cur = NetplayHooks::GetNetplayFrameDelay();
                if (cur < 6) {
                    int newDelay = cur + 1;
                    NetplayHooks::SetNetplayFrameDelay(newDelay);
                    LOG_NET_INFO("[InputSync] CharSel delay preference: %d -> %d (takes effect at match start) (=)",
                                 cur, newDelay);
                }
            }
            s_minus_was_down  = minusDown;
            s_equals_was_down = equalsDown;
        }

        if (gameMode != MODE_CHARSEL || !IsCharSelLockstepDispatchSubstate(subState)) {
            if (gameMode != s_lastCharSelDispatchMode || subState != s_lastCharSelDispatchSubstate) {
                LOG_NET_INFO("[InputSync] CharSel lockstep gated: mode=%u sub=%u (%s) active=%d -- waiting for lockstep substate",
                    gameMode,
                    subState,
                    GetCharSelDispatchSubstateName(subState),
                    CharSelSync::IsActive() ? 1 : 0);
                s_lastCharSelDispatchMode = gameMode;
                s_lastCharSelDispatchSubstate = subState;
            }
            ResetVanillaTimeouts();
            return -1;
        }

        // Frame gating — prevent producing more than one input per game-loop
        // iteration.  The game calls this dispatcher in a while-loop; we return
        // 0 once (produce a frame) then -1 to break out.
        //
        // NOTE: We cannot use vanilla FrameSim/FrameWrite gating here because
        // Hook_AdvanceFrame suppresses the FrameSim increment during charsel.
        // Instead we track a simple flag that resets each visual frame.
        static bool s_charsel_produced_this_loop = false;
        if (s_charsel_produced_this_loop) {
            s_charsel_produced_this_loop = false;  // Reset for next iteration
            return -1;  // Break out of the while-loop
        }
        
        // Poll SDL and get local input
        InputSystem_Update();
        const int localInputPlayer = GetCharSelLocalInputPlayerIndex();
        uint16_t localInput = InputSystem_GetInput(localInputPlayer);

        if ((s_frames_processed <= 180 && (s_frames_processed % 60) == 0) || localInput != 0) {
            LOG_NET_DEBUG("[InputSync] CharSel local sample: host=%d localSlot=P%u sampledPlayer=P%d swap=%d input=0x%04X",
                SessionManager::IsHost() ? 1 : 0,
                SessionManager::IsHost() ? 1 : 2,
                localInputPlayer + 1,
                InputSystem_GetControlSwap() ? 1 : 0,
                localInput);
        }
        
        // Buffer locally and send to peer (with redundant history)
        CharSelSync::BufferAndSendLocalInput(localInput);
        
        // Lockstep gate: only advance when both inputs are available
        if (!CharSelSync::HasInputsForCurrentFrame()) {
            ResetVanillaTimeouts();
            return -1;  // Freeze — wait for remote input
        }
        
        // Consume confirmed inputs for this frame
        uint16_t p1Input = 0, p2Input = 0;
        if (!CharSelSync::ConsumeCurrentFrame(&p1Input, &p2Input)) {
            LOG_NET_WARN("[InputSync] CharSel lockstep inconsistency: inputs reported ready but consume failed");
            ResetVanillaTimeouts();
            return -1;
        }

        s_lastCharSelDispatchMode = gameMode;
        s_lastCharSelDispatchSubstate = subState;

        const uint16_t rawP1Input = p1Input;
        const uint16_t rawP2Input = p2Input;
        uint16_t sharedInput = 0;

        // ── CharSel lockstep debug logging (all substates) ────────────
        // Log when inputs arrive or periodically, to track desync between sides.
        {
            const uint32_t frameNum  = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_COUNTER);
            const int32_t  writeIdx  = *reinterpret_cast<volatile int32_t*>(ADDR_FRAME_WRITE_IDX);
            const uint8_t p1Cursor   = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P1_CURSOR);
            const uint8_t p2Cursor   = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P2_CURSOR);
            const int8_t  p1Enable   = *reinterpret_cast<volatile int8_t*>(ADDR_CHARSEL_P1_ENABLE);
            const int8_t  p2Enable   = *reinterpret_cast<volatile int8_t*>(ADDR_CHARSEL_P2_ENABLE);
            const uint8_t p1Confirm  = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P1_CONFIRM);
            const uint8_t p2Confirm  = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P2_CONFIRM);
            const uint32_t p1CharId  = *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID);
            const uint32_t p2CharId  = *reinterpret_cast<volatile uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID);
            const uint8_t p1Palette  = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P1_PALETTE);
            const uint8_t p2Palette  = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_P2_PALETTE);
            const uint8_t stageId    = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_STAGE_ID);

            // Log on any input or every 60 frames, plus on substate transitions
            bool shouldLog = (rawP1Input != 0 || rawP2Input != 0 || (frameNum % 60) == 0);
            if (subState != s_lastCharSelDispatchSubstate) shouldLog = true;

            if (shouldLog) {
                char bufP1[64], bufP2[64];
                DecomposeInput(rawP1Input, bufP1, sizeof(bufP1));
                DecomposeInput(rawP2Input, bufP2, sizeof(bufP2));
                LOG_NET_DEBUG("[CharSel] host=%d sub=%u(%s) frame=%u write=%d "
                              "p1[in=0x%04X(%s) cursor=%u en=%d conf=%d char=%u pal=%u] "
                              "p2[in=0x%04X(%s) cursor=%u en=%d conf=%d char=%u pal=%u] "
                              "stageId=%u delay_pref=%d",
                    SessionManager::IsHost() ? 1 : 0,
                    subState,
                    GetCharSelDispatchSubstateName(subState),
                    frameNum, writeIdx,
                    rawP1Input, bufP1, p1Cursor, p1Enable, p1Confirm, p1CharId, p1Palette,
                    rawP2Input, bufP2, p2Cursor, p2Enable, p2Confirm, p2CharId, p2Palette,
                    stageId,
                    NetplayHooks::GetNetplayFrameDelay());
            }
        }

        // Stage select inputs are handled by Hook_InputProcess's direct-buffer path
        // for substates 7/8/11 (NOT by this dispatcher). The IsCharSelLockstepDispatchSubstate
        // gate above ensures only substates 2/3/4 reach this point, so we don't need
        // stage-select handling here.

        out[0] = (int16_t)p1Input;
        out[1] = (int16_t)p2Input;

        // Advance frame write counter (vanilla dispatcher does this)
        volatile int32_t* pFrameWrite = reinterpret_cast<volatile int32_t*>(ADDR_FRAME_WRITE_IDX);
        (*pFrameWrite)++;

        // --- PER-BUTTON ARRAY WRITE FOR GAMETYPE_VS_HUMAN CHARSEL ---
        //
        // WHY THIS MUST BE HERE (NOT in Hook_InputProcess):
        //   In GAMETYPE_VS_HUMAN (2), the charsel handler ignores out[0]/out[1] and
        //   reads directly from per-button arrays at 0x8E9E62 (P1) / 0x8E9F32 (P2).
        //   These arrays must contain the lockstep-synced inputs BEFORE the charsel
        //   handler's cursor code runs.
        //
        //   Execution order per frame:
        //     1. Hook_InputProcess (sub_562060) — runs BEFORE charsel handler
        //     2. CharSel handler calls Input_TryGetNextFrame → Hook_InputDispatcher
        //     3. Hook_InputDispatcher writes per-button arrays HERE ← (this point)
        //     4. CharSel handler reads per-button arrays for cursor movement
        //
        //   Moving writes to step 1 would use stale/unconsumed inputs because
        //   the lockstep hasn't consumed this frame's inputs yet at that point.
        //
        // OWNERSHIP SPLIT:
        //   Substates 2/3/4 (Select/Cancel/Confirm): this dispatcher writes per-button arrays
        //   Substates 7/8/11 (Preview/Intro/Stage): Hook_InputProcess writes per-button arrays
        //   There is NO overlap — exactly one writer per substate.
        //
        // EDGE DETECTION:
        //   Uses static s_lastP1/s_lastP2 (NOT the alt-buffer approach from InputProcess)
        //   because the alt-buffer is contaminated by vanilla DInput between frames:
        //   InputProcess calls g_origInputProcess which writes DInput state to the per-button
        //   arrays before returning. By the time the dispatcher runs, the arrays have DInput
        //   values, not the hack's previous lockstep values. s_lastP1/s_lastP2 track the
        //   hack's own state, avoiding this contamination.
        const uint32_t gameType = *reinterpret_cast<volatile uint32_t*>(ADDR_GAME_TYPE);
        if (gameType == 2 /* GAMETYPE_VS_HUMAN */ && gameMode == 6 /* MODE_CHARSEL */) {
            static uint16_t s_lastP1 = 0;
            static uint16_t s_lastP2 = 0;

            uint16_t justPressedP1 = p1Input & ~s_lastP1;
            uint16_t justPressedP2 = p2Input & ~s_lastP2;

            s_lastP1 = p1Input;
            s_lastP2 = p2Input;

            // Map button bits to their physical indices in the 28-word raw buffers
            // Order: Up(0), Down(1), Left(2), Right(3), A(4), B(5), C(6), D(7), Start(8), Select(9)
            static const uint16_t maskMap[10] = {
                0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200
            };

            for (int i = 0; i < 10; ++i) {
                const uint16_t mask = maskMap[i];

                // Write P1 Held and JustPressed
                *reinterpret_cast<uint16_t*>(0x8E9E62 + (i * 2))      = (p1Input & mask) ? 1 : 0;
                *reinterpret_cast<uint16_t*>(0x8E9E9A + (i * 2))      = (justPressedP1 & mask) ? 1 : 0;

                // Write P2 Held and JustPressed
                *reinterpret_cast<uint16_t*>(0x8E9F32 + (i * 2))      = (p2Input & mask) ? 1 : 0;
                *reinterpret_cast<uint16_t*>(0x8E9F6A + (i * 2))      = (justPressedP2 & mask) ? 1 : 0;
            }

            if (subState == CHARSEL_SUB_PREVIEW ||
                subState == CHARSEL_SUB_STAGE_INTRO ||
                subState == CHARSEL_SUB_CANCEL ||
                subState == CHARSEL_SUB_STAGE) {
                const uint8_t stageId = *reinterpret_cast<volatile uint8_t*>(ADDR_CHARSEL_STAGE_ID);
                TestHarness::RecordStageEvent("DispatcherWrite",
                                              *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_COUNTER),
                                              -1,
                                              gameMode,
                                              subState,
                                              stageId,
                                              0,
                                              0,
                                              rawP1Input,
                                              rawP2Input,
                                              sharedInput,
                                              p1Input,
                                              p2Input);
            }
        }

        ResetVanillaTimeouts();
        s_charsel_produced_this_loop = true;
        return 0;
    }
    
    // ── Mod session gap (between CharSel end and rollback start) ───
    // SessionManager is active but no specific sync system owns the loop yet.
    // Do NOT call the vanilla dispatcher — it may trigger blocking recv or
    // other vanilla netplay side effects. Just return -1 (skip frame).
    {
        SessionManager::Snapshot snap{};
        if (SessionManager::GetSnapshot(&snap) && snap.active && !snap.has_error) {
            ResetVanillaTimeouts();
            return -1;
        }
    }

    // ── Plain vanilla (offline/local play only) ─────────────────────
    {
        int result = g_origInputDispatcher ? g_origInputDispatcher(out) : -1;
        return result;
    }
}

/**
 * Hook for sub_562450 - Send Local Input Packet
 * Suppressed when rollback active (GekkoNet sends via adapter).
 */
static int __cdecl Hook_SendInputPacket(int frame) {
    if (IsModOwnedSync() || s_load_barrier_freeze) {
        ResetVanillaTimeouts();
        LOG_DEBUG_1S("[InputSync] Suppressed vanilla SendInputPacket (frame=%d barrier=%d)",
                     frame, s_load_barrier_freeze ? 1 : 0);
        return 0;   // suppress vanilla send (mod owns sync or frozen)
    }
    if (g_origSendInputPacket) {
        return g_origSendInputPacket(frame);
    }
    return 0;
}

/**
 * Hook for sub_5623D0 - Receive Remote Input Packet
 * Suppressed when rollback active (GekkoNet receives via adapter).
 */
static void __cdecl Hook_RecvInputPacket() {
    if (IsModOwnedSync() || s_load_barrier_freeze) {
        ResetVanillaTimeouts();
        LOG_DEBUG_1S("[InputSync] Suppressed vanilla RecvInputPacket (barrier=%d)",
                     s_load_barrier_freeze ? 1 : 0);
        return;     // suppress vanilla recv (mod owns sync or frozen)
    }
    if (g_origRecvInputPacket) {
        g_origRecvInputPacket();
    }
}

/**
 * Hook for sub_5624E0 - Get Synchronized Inputs
 * 
 * During CharSel: NOT called (game runs in local mode, no sync chain).
 * During Gameplay: suppressed (rollback inputs come via override).
 */
static int __cdecl Hook_GetSyncInput(int frame, int16_t* out) {
    // ── Mod-owned suppression ────────────────────────────────────────
    // Suppress during ANY mod-owned session (rollback, charsel, loading gap).
    // Vanilla GetSyncInput reads from per-button arrays and frame history
    // buffers that the mod hasn't populated — calling it would return garbage.
    if (IsModOwnedSync()) {
        ResetVanillaTimeouts();
        LOG_DEBUG_1S("[InputSync] Suppressed vanilla GetSyncInput (frame=%d)", frame);
        if (out) {
            out[0] = 0;
            out[1] = 0;
        }
        return 0;
    }
    
    // ── Vanilla passthrough (offline/local play only) ───────────────
    if (g_origGetSyncInput) {
        return g_origGetSyncInput(frame, out);
    }
    return -1;
}

/**
 * Hook for sub_562760 - Advance Frame Counter (Frame_AdvanceSimulation)
 * 
 * In the vanilla game, this increments Frame_Simulation (0x816490) ONCE
 * per game-loop exit (outside the while(!InputDispatcher) loop). During
 * rollback, the while-loop iterates N times (resimulating N frames) but
 * Frame_AdvanceSimulation only runs once at loop exit.
 *
 * During mod-owned sessions: suppress the increment entirely. The mod
 * manages frame counters via HandleSaveEvent/HandleLoadEvent overrides.
 * Letting the vanilla increment run causes sim counter drift (host goes
 * negative, client goes positive vs GekkoNet frame) — cosmetic but
 * introduces noise into debugging.
 *
 * During load barrier freeze: suppress to keep gameplay paused.
 * During offline play: pass through to vanilla.
 */
static int __cdecl Hook_AdvanceFrame() {
    if (s_load_barrier_freeze) {
        return 0;
    }
    // Suppress during mod-owned sessions (rollback or charsel).
    // The mod manages Frame_Simulation directly via save/load overrides.
    if (s_rollback_active || IsModOwnedSync()) {
        // Apply visual smoothing before render (only on confirmed frames,
        // not during rollback resim). This writes interpolated positions
        // to entity memory; RestoreAfterRender in Hook_RenderPresent
        // will write the real sim positions back.
        // DISABLED: Visual smoothing temporarily disabled — suspected cause of
        // broken match flow (round start animation, attack lockout during intro).
        // if (s_rollback_active && !RollbackSession::IsRollingBack()) {
        //     VisualSmoothing::ApplyBeforeRender();
        // }
        return 0;
    }
    if (g_origAdvanceFrame) {
        return g_origAdvanceFrame();
    }
    return 0;
}

/**
 * Hook for sub_562550 - Match Sync Initialization (Netplay_InitialSync)
 * 
 * Vanilla behavior: BLOCKING wait for a sync packet at CharSel fade-in (frame 159).
 * Resets all frame counters and clears ~340KB of input history buffers.
 * 
 * When a mod session is active (SessionManager Connected/CharSel/Gameplay),
 * we suppress this entirely — the mod owns synchronization and the vanilla
 * blocking recv would hang forever since no vanilla sync packet will arrive.
 */
static int __cdecl Hook_MatchSyncInit() {
    // Suppress during ANY mod-owned session. Vanilla Netplay_InitialSync
    // does a blocking recvfrom loop that would hang forever since no vanilla
    // sync packet will arrive through our mod socket.
    if (IsModOwnedSync()) {
        // Replicate the state resets that Netplay_InitialSync() normally does
        // (sub_562550): zero all frame counters and clear the vanilla input
        // histories / sync mirrors so Match Substate 2 starts from a clean slate.
        *reinterpret_cast<volatile int32_t*>(ADDR_FRAME_SIMULATION) = 0;
        AS2_ClearVanillaNetplayBuffers();
        AS2_LogFrameCounterState(0, "SuppressedNetplayInitialSync");
        LOG_NET_INFO("[InputSync] Suppressed vanilla Netplay_InitialSync, reset buffers/counters");
        ResetVanillaTimeouts();
        return 0;
    }

    // No mod session — pass through to vanilla (offline/local play only)
    int result = 0;
    if (g_origMatchSyncInit) {
        result = g_origMatchSyncInit();
    }
    return result;
}

// ============================================================================
// PUBLIC API
// ============================================================================

namespace InputSyncHooks {

bool Install() {
    if (s_input_hooks_installed) {
        LOG_NET_INFO("[InputSync] Hooks already installed");
        return true;
    }
    
    LOG_NET_INFO("[InputSync] Installing input sync hooks...");
    
    InitTimesyncTimer();
    
    MH_STATUS status;

    struct HookEntry {
        void* target;
        void* detour;
        void** original;
        const char* name;
    };

    HookEntry hooks[] = {
        { (void*)ADDR_INPUT_DISPATCHER, (void*)&Hook_InputDispatcher, (void**)&g_origInputDispatcher, "InputDispatcher" },
        { (void*)ADDR_SEND_INPUT,       (void*)&Hook_SendInputPacket, (void**)&g_origSendInputPacket, "SendInputPacket" },
        { (void*)ADDR_RECV_INPUT,       (void*)&Hook_RecvInputPacket, (void**)&g_origRecvInputPacket, "RecvInputPacket" },
        { (void*)ADDR_GET_SYNC_INPUT,   (void*)&Hook_GetSyncInput,   (void**)&g_origGetSyncInput,    "GetSyncInput"    },
        { (void*)ADDR_ADVANCE_FRAME,    (void*)&Hook_AdvanceFrame,    (void**)&g_origAdvanceFrame,    "AdvanceFrame"    },
        { (void*)ADDR_RESET_INPUT,      (void*)&Hook_MatchSyncInit,  (void**)&g_origMatchSyncInit,   "MatchSyncInit"   },
    };

    for (auto& h : hooks) {
        status = MH_CreateHook(h.target, h.detour, h.original);
        if (status != MH_OK) {
            LOG_NET_ERROR("[InputSync] Failed to create hook for %s: %d", h.name, status);
            return false;
        }
    }

    // Enable all hooks
    void* targets[] = {
        (void*)ADDR_INPUT_DISPATCHER, (void*)ADDR_SEND_INPUT,
        (void*)ADDR_RECV_INPUT,       (void*)ADDR_GET_SYNC_INPUT,
        (void*)ADDR_ADVANCE_FRAME,    (void*)ADDR_RESET_INPUT,
    };
    for (auto t : targets) {
        if (MH_EnableHook(t) != MH_OK) {
            LOG_NET_ERROR("[InputSync] Failed to enable hooks");
            return false;
        }
    }
    
    s_input_hooks_installed = true;
    LOG_NET_INFO("[InputSync] All input sync hooks installed (passthrough until rollback activated)");
    
    return true;
}

void Uninstall() {
    if (!s_input_hooks_installed) return;
    
    LOG_NET_INFO("[InputSync] Uninstalling input sync hooks...");
    
    void* targets[] = {
        (void*)ADDR_INPUT_DISPATCHER, (void*)ADDR_SEND_INPUT,
        (void*)ADDR_RECV_INPUT,       (void*)ADDR_GET_SYNC_INPUT,
        (void*)ADDR_ADVANCE_FRAME,    (void*)ADDR_RESET_INPUT,
    };
    for (auto t : targets) {
        MH_DisableHook(t);
        MH_RemoveHook(t);
    }
    
    s_input_hooks_installed = false;
    s_rollback_active = false;
    LOG_NET_INFO("[InputSync] Hooks uninstalled");
}

bool IsInstalled() {
    return s_input_hooks_installed;
}

void SetRollbackActive(bool active) {
    if (s_rollback_active == active) return;
    
    s_rollback_active = active;
    s_needs_gekko_update = true;
    s_warned_missing_rollback_inputs = false;
    s_timesync_skip_budget = 0;
    s_timesync_last_fresh_ahead = 0.0f;
    s_timesync_nudge_us = 0.0f;
    s_timesync_nudge_count = 0;
    
    // Suppress pause menu during netplay match (Start would open pause)
    InputSystem_SetPauseBlocked(active);
    
    if (active) {
        // Clear load barrier freeze when entering rollback mode
        s_load_barrier_freeze = false;
        // Reset vanilla timeout counters so the game doesn't disconnect
        ResetVanillaTimeouts();
        LOG_NET_INFO("[InputSync] Rollback mode ACTIVATED — vanilla sync suppressed");
    } else {
        AS2_ClearSynchronizedInputOverride();
        LOG_NET_INFO("[InputSync] Rollback mode DEACTIVATED — vanilla passthrough restored");
    }
}

bool IsRollbackActive() {
    return s_rollback_active;
}

void SetLoadBarrierFreeze(bool freeze) {
    if (s_load_barrier_freeze == freeze) return;
    s_load_barrier_freeze = freeze;
    if (freeze) {
        ResetVanillaTimeouts();
        LOG_NET_INFO("[InputSync] Load barrier freeze ENABLED — gameplay paused");
    } else {
        LOG_NET_INFO("[InputSync] Load barrier freeze DISABLED — gameplay resuming");
    }
}

bool IsLoadBarrierFrozen() {
    return s_load_barrier_freeze;
}

void GetStats(uint32_t* framesProcessed, uint32_t* vanillaFrames) {
    if (framesProcessed) *framesProcessed = s_frames_processed;
    if (vanillaFrames) *vanillaFrames = s_vanilla_frames;
}

void GetTimesyncSnapshot(TimesyncSnapshot* out) {
    if (!out) return;
    out->nudge_us         = s_timesync_nudge_us;
    out->nudge_count      = s_timesync_nudge_count;
    out->last_fresh_ahead = s_timesync_last_fresh_ahead;
    out->skip_budget      = s_timesync_skip_budget;
    out->smoothed_ahead   = s_timesync_smoothed_ahead;
    out->jitter           = s_timesync_jitter;
    out->effective_dz     = s_timesync_effective_dz;
    out->skip_armed       = s_timesync_skip_armed;
    out->pathological_ctr = s_timesync_pathological_ctr;
}

void ResetStats() {
    s_frames_processed = 0;
    s_vanilla_frames = 0;
}

void ResetForNewMatch() {
    s_session_terminated = false;
    s_needs_gekko_update = true;
    s_warned_missing_rollback_inputs = false;
    // Reset timesync v2 state
    s_timesync_skip_budget      = 0;
    s_timesync_last_fresh_ahead = 0.0f;
    s_timesync_nudge_us         = 0.0f;
    s_timesync_nudge_count      = 0;
    s_timesync_smoothed_ahead   = 0.0f;
    s_timesync_jitter           = 0.0f;
    s_timesync_skip_armed       = false;
    s_timesync_pathological_ctr = 0;
    s_timesync_effective_dz     = 0.0f;
    VisualSmoothing::Reset();
    LOG_NET_DEBUG("[InputSync] Reset for new match");
}

void ResetInjectionState() {
    const bool pendingEvents = RollbackSession::HasPendingEvents();
    s_needs_gekko_update = !pendingEvents;
    LOG_NET_DEBUG("[InputSync] Injection state reset (pendingEvents=%d nextGekkoUpdate=%d)",
                  pendingEvents ? 1 : 0,
                  s_needs_gekko_update ? 1 : 0);
}

} // namespace InputSyncHooks
