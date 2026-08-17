/**
 * Alice Senki 2 - RollbackSession facade over engine2 (re0.7 M4, plan §2.7.8)
 *
 * The engine2 adapter behind the UNCHANGED rollback_session.h header:
 * compiled when the CMake option AS2_WITH_GEKKO is OFF. Until the M6
 * cutover gate passes, field builds keep AS2_WITH_GEKKO=ON and ship the
 * GekkoNet implementation in rollback_session.cpp; this TU is the
 * engine-bring-up build (M4 exit gate: engine core unit-tested offline,
 * adapter compiling against the whole tree).
 *
 * Division of labor (§2.7): the RollbackEngine core (rollback/engine2) is
 * socket-free/clock-free/game-memory-free; THIS adapter owns everything it
 * must not: StateHistory save/restore, wall-clock idle resend, packet
 * send/ingest via the Session_* facade, FrameScheduler hold notifications
 * (M2 obligation), Session2_Terminate terminals (M3 obligation), and the
 * lifecycle exact-window sampling.
 *
 * Dispatcher contract preserved (two-phase, §2.7.8): BeginFrame(localInput)
 * captures + plans; ProcessNextEvent() drains the pass plan — rollback
 * transaction steps first, then advances — exactly like the Gekko adapter,
 * so input_override's netplay branch keeps its shape at the M6 rewiring.
 */

#include "rollback/rollback_session.h"

#include "core/game_state.h"
#include "core/as2_constants.h"
#include "net/connection_supervisor.h"
#include "net/delay_policy.h"
#include "net/protocol.h"
#include "net/session_manager.h"
#include "net/session2.h"
#include "patches/frame_scheduler.h"
#include "patches/memory_utils.h"
#include "rollback/desync_dump.h"
#include "rollback/engine2.h"
#include "rollback/lifecycle_window.h"
#include "rollback/netplay_log.h"
#include "rollback/resimulation.h"
#include "rollback/run_state.h"
#include "rollback/stress_hooks.h"
#include "ui/log_window.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

namespace Rollback {

// ============================================================================
// State
// ============================================================================

namespace {

RollbackEngine s_engine;

bool     s_initialized = false;
bool     s_active = false;
int      s_localPlayer = 0;
int      s_remotePlayer = 1;
uint32_t s_baselineChecksum = 0;
int32_t  s_frameOriginAbs = 0;
uint32_t s_epoch = 1;           // rotated by the director from M5/M6 on
char     s_sessionError[160] = "";
bool     s_terminalReported = false;

// Two-phase pass plan.
enum class PassStep : uint8_t {
    Idle = 0,        // no plan — ProcessNextEvent returns Done
    Replay,          // rollback transaction replay frames pending
    AdvanceReady,    // one advance is planned and may be emitted
};
PassStep s_passStep = PassStep::Idle;
bool     s_rollingBack = false;    // last emitted Advance was a replay
uint16_t s_advP1 = 0;
uint16_t s_advP2 = 0;
uint16_t s_lastLocalSample = 0;

// Test-harness input injection.
bool     s_hasInjectedInput = false;
uint16_t s_injectedInput = 0;

// Wall-clock idle resend (adapter-owned; the engine stays clock-free).
DWORD    s_lastStreamSendMs = 0;
constexpr DWORD kIdleResendMs = 50;   // §2.7.3-X

uint32_t s_localInputsSent = 0;
uint32_t s_remoteInputsRecv = 0;

int32_t RbFrame(uint32_t canonical) {
    return (int32_t)(canonical - s_engine.FirstFrame());
}

uint32_t ComputeLiveChecksumInternal() {
    struct ChecksumParts {
        uint32_t main_crc;
        uint32_t effect_index;
    } parts{};
    __try {
        parts.main_crc = CalcCRC32(
            (const void*)ADDR_MATCH_BASE,
            (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE);
        parts.effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
        return CalcCRC32(&parts, sizeof(parts));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADDEAD;
    }
}

// The single terminal funnel out of the engine (INV-12/INV-20, M3
// obligation): typed engine terminals route through Session2_Terminate;
// the dispatcher's existing abort path (input_override) consumes
// RollbackSession_GetErrorReason for the UI side.
void ReportEngineTerminal() {
    const EngineTerminal t = s_engine.Terminal();
    if (t == EngineTerminal::None || s_terminalReported) {
        return;
    }
    s_terminalReported = true;
    snprintf(s_sessionError, sizeof(s_sessionError), "%s: %s",
             EngineTerminalName(t), s_engine.TerminalDetail());
    LOG_ERROR("[RollbackSession/engine2] terminal: %s", s_sessionError);
    NetplayLog_Write("ROLLBACK", RbFrame(s_engine.SimFrontier()),
        "ENGINE TERMINAL: %s", s_sessionError);
    NetplayLog_Flush();

    switch (t) {
        case EngineTerminal::ConfirmedDesync: {
            // Evidence dump BEFORE terminating (D-1: whoever detects, decides).
            DesyncDump_TryDump(RbFrame(s_engine.ConfirmedFrontier()),
                               ComputeLiveChecksumInternal(), 0,
                               "engine2-synchash", s_engine.TerminalDetail());
            Net::Session2_Terminate(Net::Session2TerminalReason::ConfirmedDesync,
                                    s_sessionError);
            break;
        }
        case EngineTerminal::InputConflict:
        case EngineTerminal::InputInvalid:
        case EngineTerminal::EpochInconsistent:
        case EngineTerminal::HashQueueOverflow:
            Net::Session2_Terminate(Net::Session2TerminalReason::ProtocolViolation,
                                    s_sessionError);
            break;
        default:
            // Internal invariant / config faults are local failures: end the
            // session state without inventing a network kill path.
            break;
    }
}

void SendInputStreamIfDue(bool force) {
    if (!s_active) return;
    Net::InputStreamPayload p{};
    const DWORD now = GetTickCount();
    const bool idleDue = s_engine.HasUnackedSuffix() &&
                         (DWORD)(now - s_lastStreamSendMs) >= kIdleResendMs;
    if (!force && !idleDue) return;
    if (!s_engine.BuildInputStream(&p)) return;
    p.session_id = Net::Session2_GetSessionId();

    // Egress stress query point preserved (test methodology, §2.7.8).
    if (StressHooks_IsEnabled() && StressHooks_ShouldDropPacket()) {
        return;
    }
    // Whole struct sent: fixed 32-slot window schema (M1 note).
    if (Net::Session_SendPacket(Net::CHANNEL_GAMEPLAY, Net::PacketType::InputStream,
                                &p, sizeof(p), false)) {
        ++s_localInputsSent;
        s_lastStreamSendMs = now;
    }
}

void SendPendingSyncHashes() {
    OutgoingSyncHash h{};
    while (s_engine.PopOutgoingSyncHash(&h)) {
        Net::SyncHashPayload p{};
        p.session_id = Net::Session2_GetSessionId();
        p.epoch = h.epoch;
        p.frame = h.frame;
        p.gameplay_hash = h.gameplay_hash;
        p.rng_state = h.rng_state;
        p.hp0 = h.hp0;
        p.hp1 = h.hp1;
        Net::Session_SendPacket(Net::CHANNEL_CONTROL, Net::PacketType::SyncHash,
                                &p, sizeof(p), true);
    }
}

void DrainConfirmSeam() {
    // M4: the confirm seam is drained to bound the ring; the spectator/replay
    // consumers rewire onto it at M7 (S-5/S-6).
    ConfirmedFrame cf{};
    while (s_engine.PopConfirmedFrame(&cf)) {
        DesyncDump_StoreChecksum(RbFrame(cf.frame), (uint32_t)cf.pre_state_hash);
    }
    SendPendingSyncHashes();
    const HashVerify v = s_engine.PumpSyncHashVerify();
    (void)v;  // terminals surfaced via ReportEngineTerminal below
    ReportEngineTerminal();
}

// Sample the exact-window predicate for the NEXT tick (INV-25, M4-7).
void RefreshLifecycleWindow() {
    uint32_t mode = 0, substate = 0;
    __try {
        mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
        substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_engine.SetLifecycleExactNext(true);  // fail safe
        return;
    }
    // match_exit_pending: the match region's exit route is armed the moment
    // the win screen handoff resolves. Until the M6 director wires the
    // MatchLifecycle-driven signal, the conservative M4 stand-in is
    // "any tick outside plain mode-8/substate-3 gameplay".
    const bool exit_pending = false;
    s_engine.SetLifecycleExactNext(
        LifecycleWindow_IsExactInputNext(mode, substate, exit_pending));
}

// Capture the pre-tick snapshot for `frame` and return its gameplay digest
// plus SyncHash diagnostics (§2.7.7: save → simulate → commit).
bool SavePreTick(uint32_t frame, uint64_t* hash,
                 uint32_t* rng, uint16_t* hp0, uint16_t* hp1) {
    if (!StateHistory_CaptureFrameHashed(RbFrame(frame), hash)) {
        return false;
    }
    // rng/hp SyncHash diagnostics: the snapshot holds the authoritative
    // rng_seed already; the diagnostic side-channel wires up with the M6
    // HUD/dump pass. gameplay_hash stays authoritative either way (§2.7.7).
    *rng = 0;
    *hp0 = 0;
    *hp1 = 0;
    return true;
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void RollbackSession_Init() {
    StateHistory_Init();
    s_initialized = true;
    LOG_INFO("[RollbackSession/engine2] Initialized (custom engine, AS2_WITH_GEKKO=OFF)");
}

void RollbackSession_Shutdown() {
    RollbackSession_End();
    StateHistory_Shutdown();
    s_initialized = false;
}

bool RollbackSession_Begin(const RollbackSessionConfig& config) {
    if (!s_initialized) return false;
    if (s_active) {
        RollbackSession_End();
    }

    EngineConfig ec{};
    ec.local_player = (uint8_t)(config.local_player & 1);
    ec.input_delay = (uint8_t)(config.initial_delay < 0 ? 0
                        : config.initial_delay > 15 ? 15 : config.initial_delay);
    ec.max_rollback = (uint8_t)(config.rollback_budget < 1 ? 1
                        : config.rollback_budget > 15 ? 15 : config.rollback_budget);
    ec.neutral_input = 0x0000;
    ec.first_frame = 0;          // canonical origin; epochs never reset it
    ec.max_remote_future = 120;
    ec.history_capacity = 4096;

    s_epoch = 1;                 // M5's match_setup takes epoch authority
    if (!s_engine.Arm(ec, s_epoch)) {
        LOG_ERROR("[RollbackSession/engine2] Arm failed: %s", s_engine.TerminalDetail());
        return false;
    }

    s_localPlayer = config.local_player;
    s_remotePlayer = config.remote_player;
    s_baselineChecksum = config.baseline_checksum;
    s_frameOriginAbs = config.frame_origin_abs;
    s_sessionError[0] = '\0';
    s_terminalReported = false;
    s_passStep = PassStep::Idle;
    s_rollingBack = false;
    s_localInputsSent = 0;
    s_remoteInputsRecv = 0;
    s_lastStreamSendMs = GetTickCount();

    StateHistory_Reset();
    StateHistory_SetTagContext(s_epoch, /*phase=*/(uint32_t)MODE_MATCH);
    DesyncDump_Reset();

    s_active = true;
    NetplayLog_Write("ROLLBACK", 0,
        "engine2 session begin: local=P%d delay=%d budget=%d origin_abs=%d epoch=%u",
        s_localPlayer + 1, ec.input_delay, ec.max_rollback, s_frameOriginAbs, s_epoch);
    return true;
}

void RollbackSession_End() {
    if (!s_active) return;
    s_active = false;
    s_passStep = PassStep::Idle;
    s_rollingBack = false;
    s_engine.Disarm();
    StateHistory_SetTagContext(0, 0);
    NetplayLog_Write("ROLLBACK", RbFrame(s_engine.SimFrontier()),
        "engine2 session end: confirmed=%d rollbacks=%u",
        RbFrame(s_engine.ConfirmedFrontier()), s_engine.GetStats().rollbacks);
}

bool RollbackSession_IsActive() {
    return s_active;
}

// ============================================================================
// Two-phase frame processing
// ============================================================================

void RollbackSession_BeginFrame(uint16_t localInput) {
    if (!s_active) return;

    if (s_hasInjectedInput) {
        localInput = s_injectedInput;
        s_hasInjectedInput = false;
    }
    s_lastLocalSample = localInput;

    RefreshLifecycleWindow();

    // Capture-once keyed to the sim frontier (§2.7.3-C): repeats during a
    // stall adopt; the sealed word schedules at source + D_local.
    if (!s_engine.InRollback()) {
        s_engine.CaptureLocalInput(s_engine.SimFrontier(), localInput);
    }
    SendInputStreamIfDue(/*force=*/true);
    s_passStep = PassStep::Idle;  // plan is derived lazily in ProcessNextEvent
    DrainConfirmSeam();
}

bool RollbackSession_PollSession() {
    if (!s_active) return false;
    // Producer while stalled (INV-24): one seal per frame period; the
    // scheduler paces the caller, so one call per pass is the cadence.
    if (s_engine.ProduceLocalInputAhead(s_lastLocalSample)) {
        SendInputStreamIfDue(/*force=*/true);
    } else {
        SendInputStreamIfDue(/*force=*/false);
    }
    DrainConfirmSeam();
    return s_engine.Terminal() == EngineTerminal::None;
}

EventResult RollbackSession_ProcessNextEvent() {
    if (!s_active) return EventResult::Error;
    if (s_engine.Terminal() != EngineTerminal::None) {
        ReportEngineTerminal();
        return EventResult::Error;
    }

    // Continue an open rollback transaction: emit the next replay frame.
    if (s_engine.InRollback()) {
        uint32_t frame = 0;
        uint16_t inputs[2] = {0, 0};
        if (s_engine.NextReplayInputs(&frame, inputs)) {
            uint64_t hash = 0;
            uint32_t rng = 0;
            uint16_t hp0 = 0, hp1 = 0;
            if (!SavePreTick(frame, &hash, &rng, &hp0, &hp1)) {
                LOG_ERROR("[RollbackSession/engine2] replay pre-save failed at %u", frame);
                return EventResult::Error;
            }
            if (!s_engine.CommitReplayFrame(frame, hash, rng, hp0, hp1)) {
                ReportEngineTerminal();
                return EventResult::Error;
            }
            s_advP1 = inputs[0];
            s_advP2 = inputs[1];
            s_rollingBack = true;
            return EventResult::Advance;
        }
        if (!s_engine.FinishRollback()) {
            ReportEngineTerminal();
            return EventResult::Error;
        }
        Net::DelayPolicy_OnRollbackApplied(s_engine.ActiveDelay());
        s_rollingBack = false;
        DrainConfirmSeam();
        // Fall through: the driver re-reads frontiers and may still advance
        // on the same opportunity (§2.8.6 corrections-run-first).
    }

    const EngineAction action = s_engine.NextAction();
    switch (action.kind) {
        case EngineActionKind::Rollback: {
            if (!s_engine.BeginRollback(action.frame)) {
                ReportEngineTerminal();
                return EventResult::Error;
            }
            // Tag-validated restore (fail-closed, §2.7.5).
            if (!StateHistory_LoadFrameTagged(RbFrame(action.frame), s_epoch)) {
                snprintf(s_sessionError, sizeof(s_sessionError),
                         "restore failed at frame %d (epoch %u)",
                         RbFrame(action.frame), s_epoch);
                NetplayLog_Write("ROLLBACK", RbFrame(action.frame),
                    "ENGINE2 restore FAILED (tag mismatch or missing slot)");
                return EventResult::Error;
            }
            NetplayLog_Write("ROLLBACK", RbFrame(action.frame),
                "engine2 rollback begin: from=%d until=%d depth=%u",
                RbFrame(action.frame), RbFrame(action.replay_until),
                s_engine.GetStats().last_rollback_length);
            // First replay frame is emitted by the next call in this loop.
            return RollbackSession_ProcessNextEvent();
        }

        case EngineActionKind::Advance: {
            uint64_t hash = 0;
            uint32_t rng = 0;
            uint16_t hp0 = 0, hp1 = 0;
            if (!SavePreTick(action.frame, &hash, &rng, &hp0, &hp1)) {
                LOG_ERROR("[RollbackSession/engine2] pre-save failed at %u", action.frame);
                return EventResult::Error;
            }
            if (!s_engine.CommitAdvance(action.frame, hash, rng, hp0, hp1)) {
                ReportEngineTerminal();
                return EventResult::Error;
            }
            s_advP1 = action.inputs[0];
            s_advP2 = action.inputs[1];
            s_rollingBack = false;
            SendInputStreamIfDue(/*force=*/false);
            return EventResult::Advance;
        }

        case EngineActionKind::Stall:
        default: {
            // M2 obligation: typed hold causes reach the scheduler;
            // PredictionLimit is the only debt-creating hold (§2.8.5).
            if (action.hold_cause != HoldCause::None) {
                FrameScheduler_NotifyHold(
                    action.hold_cause,
                    action.hold_cause == HoldCause::PredictionLimit);
            }
            s_rollingBack = false;
            DrainConfirmSeam();
            return EventResult::Done;
        }
    }
}

bool RollbackSession_HasPendingFrame() {
    if (!s_active) return false;
    if (s_engine.InRollback()) return true;
    if (s_engine.HasPendingMismatch()) return true;
    // An advance is available when nothing blocks the next frame.
    RollbackEngine& e = s_engine;
    return e.Terminal() == EngineTerminal::None &&
           e.SpeculativeFrames() < e.MaxRollback();
}

bool RollbackSession_DrainPendingNonAdvanceEvents() {
    if (!s_active) return false;
    if (s_engine.Terminal() != EngineTerminal::None) return false;
    // engine2 has no queued save/load events: state capture happens inline
    // with each advance. The stream still contains an Advance whenever a
    // correction or an advance is possible.
    if (s_engine.InRollback() || s_engine.HasPendingMismatch()) return false;
    DrainConfirmSeam();
    return true;
}

void RollbackSession_GetAdvanceInputs(uint16_t* p1, uint16_t* p2) {
    if (p1) *p1 = s_advP1;
    if (p2) *p2 = s_advP2;
}

// ============================================================================
// Packet ingestion
// ============================================================================

void RollbackSession_BufferGekkoPacket(const void* data, size_t len) {
    // v2 ingest: the router still funnels PacketType::InputStream (23)
    // through this facade entry until the M5 routing-table cutover renames
    // it. Payload is the §3.2 InputStreamPayload.
    if (!s_active || !data) return;
    if (len < sizeof(Net::InputStreamPayload)) {
        NetplayLog_Verbose("ROLLBACK", RbFrame(s_engine.SimFrontier()),
            "engine2 ingest: short InputStream payload (%zu < %zu), dropped",
            len, sizeof(Net::InputStreamPayload));
        return;
    }
    Net::InputStreamPayload p{};
    memcpy(&p, data, sizeof(p));

    const uint64_t sid = Net::Session2_GetSessionId();
    if (sid != 0 && p.session_id != 0 && p.session_id != sid) {
        return;  // §3.1: wrong session_id is structurally inert
    }

    const int32_t localDepth = (int32_t)s_engine.SpeculativeFrames();
    if (!s_engine.IngestInputStream(p)) {
        ReportEngineTerminal();
        return;
    }
    ++s_remoteInputsRecv;

    // M2 obligation: feed the §2.8.4 pace-slew input from PressureReport.
    FrameScheduler_SubmitPeerDepthSample((int32_t)p.pressure.prediction_depth,
                                         localDepth);
}

// ============================================================================
// Queries
// ============================================================================

int32_t RollbackSession_GetCurrentFrame() {
    return RbFrame(s_engine.SimFrontier());
}

int32_t RollbackSession_GetFrameOriginAbs() {
    return s_frameOriginAbs;
}

int32_t RollbackSession_GetCurrentGameAbsFrame() {
    return s_frameOriginAbs + RbFrame(s_engine.SimFrontier());
}

int32_t RollbackSession_RbFrameToGameAbs(int32_t rb_frame) {
    return s_frameOriginAbs + rb_frame;
}

bool RollbackSession_IsRollingBack() {
    return s_active && (s_rollingBack || s_engine.InRollback());
}

bool RollbackSession_IsSessionRunning() {
    // ProgressDeadline arms on this (M3): "gameplay timeline advancing is
    // expected". True once armed and not terminal.
    return s_active && s_engine.Terminal() == EngineTerminal::None;
}

bool RollbackSession_IsPeerInterrupted() {
    // Supervisor Interrupted verdict passthrough (§2.7.8 facade table).
    return s_active && Net::ConnectionSupervisor_IsInterrupted();
}

float RollbackSession_FramesAhead() {
    // Telemetry only (INV-1): signedLead(local produced, peer produced).
    return (float)s_engine.FramesAheadSigned();
}

int RollbackSession_GetActiveDelay() {
    return (int)s_engine.ActiveDelay();
}

bool RollbackSession_SetLocalDelay(int delay) {
    if (!s_active || delay < 0 || delay > 15) return false;
    return s_engine.RequestInputDelay((uint8_t)delay);
}

int RollbackSession_GetRollbackBudget() {
    return (int)s_engine.MaxRollback();
}

bool RollbackSession_ShouldSuppressSideEffects() {
    return RollbackSession_IsRollingBack();
}

const char* RollbackSession_GetErrorReason() {
    return s_sessionError;
}

bool RollbackSession_TakeErrorReason(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    if (s_sessionError[0] == '\0') {
        out[0] = '\0';
        return false;
    }
    strncpy(out, s_sessionError, outSize - 1);
    out[outSize - 1] = '\0';
    s_sessionError[0] = '\0';
    return true;
}

void RollbackSession_GetTimesyncTelemetry(RollbackTimesyncTelemetry* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    const int32_t current = RbFrame(s_engine.SimFrontier());
    const RollbackEngine::Stats& st = s_engine.GetStats();

    out->rb_frame_current = current;
    out->rb_frame_last_confirmed = RbFrame(s_engine.ConfirmedFrontier()) - 1;
    out->rb_frame_last_remote_received = RbFrame(s_engine.RemoteActualFrontier()) - 1;
    out->rb_frame_remote_contiguous = out->rb_frame_last_remote_received;
    out->raw_remote_gap = (int32_t)s_engine.SpeculativeFrames();
    out->effective_remote_delay = Net::DelayPolicy_GetEffectiveRemoteDelay();
    // INV-1: no debt quantity exists in engine2 — the field survives for the
    // header contract and reports the raw depth until the M6 HUD rework.
    out->prediction_debt = 0;
    out->rollback_budget = (int32_t)s_engine.MaxRollback();
    out->game_abs_frame_current = RollbackSession_GetCurrentGameAbsFrame();
    out->frame_origin_abs = s_frameOriginAbs;
    out->rollback_count = (int32_t)st.rollbacks;
    out->last_rollback_replay_length = (int32_t)st.last_rollback_length;
    out->max_rollback_distance = (int32_t)st.max_rollback_depth;
    out->predicted_frames_outstanding = (int32_t)s_engine.SpeculativeFrames();
    out->frames_ahead = (float)s_engine.FramesAheadSigned();
    // Link stats come from time_probe at M6; zeros until then.
}

// ============================================================================
// Test-harness input injection
// ============================================================================

void RollbackSession_InjectLocalInput(uint16_t input) {
    s_hasInjectedInput = true;
    s_injectedInput = input;
}

// ============================================================================
// Diagnostics
// ============================================================================

void RollbackSession_GetSnapshot(RollbackSessionSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    const RollbackEngine::Stats& st = s_engine.GetStats();

    out->active = s_active;
    out->local_player = s_localPlayer;
    out->remote_player = s_remotePlayer;

    out->frame_origin_abs = s_frameOriginAbs;
    out->rb_frame_current = RbFrame(s_engine.SimFrontier());
    out->game_abs_frame_current = RollbackSession_GetCurrentGameAbsFrame();
    out->rb_frame_last_confirmed = RbFrame(s_engine.ConfirmedFrontier()) - 1;
    out->rb_frame_last_remote_received = RbFrame(s_engine.RemoteActualFrontier()) - 1;
    out->rb_frame_last_saved_state = StateHistory_GetNewestFrame();

    out->rollback_count = (int32_t)st.rollbacks;
    out->rb_last_rollback_start_frame = RbFrame(st.last_rollback_from);
    out->last_rollback_replay_length = (int32_t)st.last_rollback_length;
    out->max_rollback_distance = (int32_t)st.max_rollback_depth;
    out->predicted_frames_outstanding = (int32_t)s_engine.SpeculativeFrames();

    out->is_rolling_back = RollbackSession_IsRollingBack();
    out->side_effects_suppressed = out->is_rolling_back;
    out->frames_ahead = (float)s_engine.FramesAheadSigned();

    out->active_delay = Net::DelayPolicy_GetActiveDelay();
    out->rollback_budget = (int32_t)s_engine.MaxRollback();

    out->baseline_checksum = s_baselineChecksum;
    out->current_checksum = ComputeLiveChecksumInternal();

    out->total_predictions = (int32_t)st.predictions_used;
    out->total_mispredictions = (int32_t)st.mispredictions;
    out->total_correct_predictions = (int32_t)st.correct_predictions;

    out->local_inputs_sent = (int32_t)s_localInputsSent;
    out->remote_inputs_received = (int32_t)s_remoteInputsRecv;

    // Snapshot keeps the legacy field names until the M5 facade rename
    // (inventory §11); link stats arrive with time_probe (M6).
    out->gekko_avg_ping = 0.0f;
    out->gekko_jitter = 0.0f;
}

uint32_t RollbackSession_ComputeLiveStateChecksum() {
    return ComputeLiveChecksumInternal();
}

} // namespace Rollback
