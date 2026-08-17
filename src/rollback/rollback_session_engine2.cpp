/**
 * Alice Senki 2 - RollbackSession facade over engine2 (re0.7 M4, plan §2.7.8)
 *
 * The engine2 adapter behind the UNCHANGED rollback_session.h header — the
 * sole provider since the post-M8 legacy-backend removal.
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
 * transaction steps first, then advances — exactly like the 0.6 adapter,
 * so input_override's netplay branch kept its shape at the M6 rewiring.
 */

#include "rollback/rollback_session.h"

#include "core/game_state.h"
#include "core/as2_constants.h"
#include "net/connection_supervisor.h"
#include "net/delay_policy.h"
#include "net/frontend_input_sync.h"
#include "net/pregame_sync.h"
#include "net/protocol.h"
#include "net/session_manager.h"
#include "net/session2.h"
#include "net/spectator_runtime.h"
#include "net/time_probe.h"
#include "replay/replay_runtime.h"
#include "patches/frame_scheduler.h"
#include "rollback/determinism_verify.h"
#include "patches/memory_utils.h"
#include "rollback/desync_diag.h"
#include "rollback/desync_dump.h"
#include "rollback/desync_fine_diag.h"
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
bool     s_suspended = false;   // M6: match-boundary suspension — the engine
                                // stays armed; the next Begin under a higher
                                // epoch ROTATES instead of re-arming (§2.6.5)
bool     s_matchExitPending = false;  // M6: director-derived §2.7.6 signal
int      s_localPlayer = 0;
int      s_remotePlayer = 1;
uint32_t s_baselineChecksum = 0;
int32_t  s_frameOriginAbs = 0;
uint32_t s_epoch = 1;           // adopted from match_setup at Begin (M5);
                                // rotated across matches by Begin (M6)
char     s_sessionError[160] = "";
bool     s_terminalReported = false;

// M6 stress ingest: bounded delivery-delay queue for InputStream payloads
// (StressHooks_SetInputDeliveryDelay).
//
// Release is WALL-CLOCK, not sim-frame (2026-08-17 deep-rollback fix, run
// 20-22-3x): the original release condition compared against the SIM
// frontier, but the frontier stalls exactly when the sim runs out of remote
// actuals — the queue then never released (inputs waited for the sim, the
// sim waited for inputs) until the 64-slot overflow burst-delivered a
// second's worth at once. Observed: rollbacks=1/s at depth 12, hold_pred
// 18-24/s, sim ~20-28 fps, 31 ms pass intervals. A delivery delay emulates
// NETWORK latency, which is wall-clock by nature and keeps flowing while
// the receiver stalls; stress-only code, so no INV-16 sim-decision concern.
struct DelayedStreamPacket {
    Net::InputStreamPayload payload;
    DWORD   release_at_ms;
    bool    valid;
};
// 16 -> 64 (2026-08-17 deep-rollback cells): at ~60 sends/s + idle resends a
// 10-14 frame delivery delay keeps ~13-20 packets in flight — the 16-slot
// queue overflowed constantly and the overflow path delivered packets
// IMMEDIATELY, silently defeating the configured delay (observed: DD10 run
// with zero effective input lag and zero rollbacks).
constexpr size_t kDelayedStreamMax = 64;
DelayedStreamPacket s_delayedStream[kDelayedStreamMax] = {};

// Forward decls (defined with the ingest section below).
void IngestInputStreamNow(const Net::InputStreamPayload& p);
void DrainDelayedStreamQueue();

// Forced-deep-rollback visibility (2026-08-17 acceptance): one line per
// second while StressHooks forced rollback is armed, so a live run SHOWS the
// sustained per-frame transaction rate and the achieved depths (the engine
// clamps depth only at the epoch origin; nothing else may truncate it).
DWORD    s_forcedStatWindowStartMs = 0;
uint32_t s_forcedStatTx = 0;
uint32_t s_forcedStatDepthMin = 0;
uint32_t s_forcedStatDepthMax = 0;
uint32_t s_forcedStatTruncated = 0;
uint32_t s_forcedStatEngineBase = 0;   // stats_.forced_transactions at window start

void NoteRollbackTransactionDone(bool truncated);

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

// Rolling diagnostic ring (post-M8 divergence diagnostics): the last
// DESYNC_DIAG_RING_CAPACITY confirmed frames' hash/rng/hp/inputs, fed from
// the confirm seam — cheap, always on during netplay. Adapter-owned (the
// engine stays free of dump concerns); dumped on either side's
// ConfirmedDesync so tools/compare_desync_dumps.py can localize the
// divergent frame/field from the two files.
DesyncDiagRing s_diagRing;

int32_t RbFrame(uint32_t canonical) {
    return (int32_t)(canonical - s_engine.FirstFrame());
}

// rb frame at which the CURRENT epoch began (0 until the first cross-match
// rotation). game_abs mapping is epoch-relative (§2.7.2): the canonical
// counter spans all matches, the game's own counter resets per match.
int32_t RbEpochOrigin() {
    return (int32_t)(s_engine.EpochFrameOrigin() - s_engine.FirstFrame());
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
            // The engine captured the failing SyncHash pair; name the first
            // divergent field (hash > rng > hp) and the confirmed frame, then
            // dump the diagnostic ring + per-region CRCs alongside it.
            DesyncEvidence ev{};
            const bool haveEv = s_engine.GetDesyncEvidence(&ev);
            if (haveEv) {
                NetplayLog_Write("ROLLBACK", RbFrame(ev.frame),
                    "CONFIRMED DESYNC localization: frame=%u (rb %d) "
                    "first_divergent=%s local_hash=%016llx peer_hash=%016llx "
                    "local_rng=%08x peer_rng=%08x local_hp=%u,%u peer_hp=%u,%u",
                    ev.frame, RbFrame(ev.frame),
                    DesyncEvidence_FirstDivergentField(ev),
                    (unsigned long long)ev.local_hash,
                    (unsigned long long)ev.peer_hash,
                    ev.local_rng, ev.peer_rng,
                    ev.local_hp0, ev.local_hp1, ev.peer_hp0, ev.peer_hp1);
                NetplayLog_Flush();
            }
            DesyncDump_TryDumpWithDiagnostics(
                haveEv ? RbFrame(ev.frame) : RbFrame(s_engine.ConfirmedFrontier()),
                ComputeLiveChecksumInternal(), 0,
                "engine2-synchash", s_engine.TerminalDetail(),
                &s_diagRing, haveEv ? &ev : nullptr);
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
    // The single immutable confirm seam (§2.7.3-F): feeds (a) the SyncHash
    // exchange, (b) the spectator sidecar push, (c) the replay recorder.
    // M7 (S-5/S-6): every popped frame is final by construction — a predicted
    // value can never reach the sidecar archive or a replay file, and rewrite
    // flags are gone (FRAME_FLAG_ROLLBACK_REWRITE retired on this path).
    ConfirmedFrame cf{};
    while (s_engine.PopConfirmedFrame(&cf)) {
        const int32_t rb = RbFrame(cf.frame);
        DesyncDump_StoreChecksum(rb, (uint32_t)cf.pre_state_hash);

        // Divergence-diagnostics ring (post-M8): confirmed frames only —
        // exactly the values a SyncHash exchange compares, plus the
        // canonical input pair, so both sides can localize a desync.
        {
            DesyncDiagEntry de{};
            de.frame = cf.frame;
            de.epoch = cf.epoch;
            de.gameplay_hash = cf.pre_state_hash;
            de.rng = cf.rng_state;
            de.hp0 = cf.hp0;
            de.hp1 = cf.hp1;
            de.p1_input = cf.inputs[0];
            de.p2_input = cf.inputs[1];
            s_diagRing.Push(de);
        }

        // Match-relative numbering (0-based per epoch): the sidecar protocol
        // and the replay tape keep the per-match frame identity the 0.6
        // per-match engine produced; the canonical counter spans the session
        // (INV-15) and stays internal. A record from before the current epoch
        // origin (rotation raced the drain) is skipped — its match is over
        // and its archive was sealed at OnMatchEnd.
        const int32_t matchRel = rb - RbEpochOrigin();
        if (matchRel >= 0) {
            const int32_t gameAbs = s_frameOriginAbs + matchRel;
            Net::SpectatorRuntime_OnConfirmedFrame(matchRel, gameAbs,
                cf.inputs[0], cf.inputs[1], cf.pre_state_hash);
            Replay::ReplayRuntime_OnConfirmedFrame(cf.epoch, gameAbs,
                cf.inputs[0], cf.inputs[1], cf.pre_state_hash);
        }
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
    // match_exit_pending (M6): director-derived from MatchLifecycle (phase
    // MatchEnd/PostMatchRoute or a non-zero vanilla exit-route byte) and
    // mirrored via RollbackSession_SetMatchExitPending — replaces the M4
    // conservative stand-in.
    s_engine.SetLifecycleExactNext(
        LifecycleWindow_IsExactInputNext(mode, substate, s_matchExitPending));
}

// SyncHash diagnostics (M6, §2.7.7): rng/hp ride the confirm seam as
// diagnostics; gameplay_hash stays authoritative either way.
void ReadSyncHashDiagnostics(uint32_t* rng, uint16_t* hp0, uint16_t* hp1) {
    __try {
        *rng = DetVer_GetRngSeed();
        *hp0 = (uint16_t)ReadMemory<int16_t>(ADDR_P1_HP_DIRECT);
        *hp1 = (uint16_t)ReadMemory<int16_t>(ADDR_P2_HP_DIRECT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *rng = 0;
        *hp0 = 0;
        *hp1 = 0;
    }
}

// Capture the pre-tick snapshot for `frame` and return its gameplay digest
// plus SyncHash diagnostics (§2.7.7: save → simulate → commit).
bool SavePreTick(uint32_t frame, uint64_t* hash,
                 uint32_t* rng, uint16_t* hp0, uint16_t* hp1) {
    if (!StateHistory_CaptureFrameHashed(RbFrame(frame), hash)) {
        return false;
    }
    ReadSyncHashDiagnostics(rng, hp0, hp1);
    // F7d localization ring: per-window CRCs + raw context image at the SAME
    // instant as the hashed capture (replays re-record, so the surviving
    // entry always reflects the confirmed capture of the frame).
    FineDiag_RecordPreTick(frame);
    return true;
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void RollbackSession_Init() {
    StateHistory_Init();
    s_initialized = true;
    LOG_INFO("[RollbackSession/engine2] Initialized (custom engine)");
}

void RollbackSession_Shutdown() {
    RollbackSession_End();
    StateHistory_Shutdown();
    s_initialized = false;
}

// Stress prediction tap (M6): installed while stress hooks are enabled so
// forced mismatches corrupt PREDICTIONS (a genuine mispredict → rollback),
// never actuals (INV-19).
static uint16_t StressPredictionTap(uint16_t predicted) {
    return StressHooks_MaybeCorruptPrediction(predicted);
}

// Forced-rollback round-seam clamp state (see ApplyStressHooks).
static uint32_t s_forcedFightWindowOrigin = 0;
static bool     s_forcedFightWindowValid = false;

// Live forced-rollback readout for the debug GUI. The operator has had to
// take the logs' word for this repeatedly; the panel shows the effective
// depth THIS pass and why it differs from the configured one, plus the last
// completed one-second window of executed transactions.
static ForcedRollbackLiveStats s_forcedLive = {};

// Stress hook application (M6 tap + post-M8 forced-rollback depth): kept in
// one place so every arm/rotate/poll site applies the identical mapping.
static void ApplyStressHooks(RollbackEngine& engine) {
    const bool on = StressHooks_IsEnabled();
    engine.SetPredictionTap(on ? &StressPredictionTap : nullptr);
    uint8_t forced = on ? (uint8_t)StressHooks_GetForcedRollbackDepth()
                        : (uint8_t)0;
    s_forcedLive.configured_depth = StressHooks_GetForcedRollbackDepth();
    s_forcedLive.gate_reason = on ? "active" : "stress hooks disabled";
    if (forced > 0) {
        // Forced transactions only inside the FIGHT substate (2026-08-17):
        // outside sub 3 the dispatcher is consulted at most ONCE per outer
        // pass (the multi-tick while loop lives only in the fight handler),
        // so a depth-30 transaction would dribble one replay tick per pass
        // and collapse intros / round transitions / winscreens to ~2 sim
        // fps. The confirmed-gated §2.8.6(d) bail covers any transaction
        // already straddling a seam when the gate flips.
        uint32_t mode = 0, sub = 0;
        __try {
            mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
            sub = ReadMemory<uint32_t>(ADDR_SUB_STATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mode = 0;
        }
        if (mode != (uint32_t)LIFECYCLE_MODE_MATCH ||
            sub != (uint32_t)LIFECYCLE_SUBSTATE_FIGHT) {
            forced = 0;
            s_forcedFightWindowValid = false;
            s_forcedLive.gate_reason =
                "off screen: forcing runs only in the fight substate (mode 8 sub 3)";
        } else {
            // ── Round-seam clamp (run 20-46 livelock root cause) ────────
            // A forced window must NEVER span a round transition: replaying
            // the KO re-runs round-init against the live state, the
            // substate flaps, and the seam thrashes (observed livelock:
            // identical from=4067 until=4097 depth-30 transaction repeated
            // 20 s). Clamp the depth to the frames executed since the
            // CURRENT fight window began; after every round start the depth
            // ramps 0 -> cfg over cfg frames and stays there mid-round.
            if (!s_forcedFightWindowValid) {
                s_forcedFightWindowOrigin = engine.SimFrontier();
                s_forcedFightWindowValid = true;
            }
            const uint32_t sinceFight = Net::forwardDistance(
                s_forcedFightWindowOrigin, engine.SimFrontier());
            if ((uint32_t)forced > sinceFight) {
                forced = (uint8_t)sinceFight;
                s_forcedLive.gate_reason =
                    "ramping after round start (a forced window may not span a round seam)";
            }
        }
    }
    s_forcedLive.effective_depth_now = forced;
    engine.SetForcedRollback(forced);
}

bool RollbackSession_Begin(const RollbackSessionConfig& config) {
    if (!s_initialized) return false;
    if (s_active) {
        RollbackSession_End();
    }

    // ── M6 cross-match epoch rotation (§2.6.5, INV-15/G2) ──────────────────
    // A suspended (match-boundary) engine under an unchanged session rotates
    // to the new epoch: the canonical frame counter and input streams
    // CONTINUE; only state identity and the game-frame origin change.
    // Fall back to a full re-arm when anything the immutable EngineConfig
    // carries has changed (R, player slot) or the engine faulted.
    if (s_engine.Armed() && s_suspended &&
        s_engine.Terminal() == EngineTerminal::None) {
        uint32_t newEpoch = Net::PregameSync_GetCurrentEpoch();
        if (newEpoch == 0) {
            newEpoch = s_epoch + 1;
        }
        const bool configCompatible =
            (uint8_t)(config.local_player & 1) == (uint8_t)(s_localPlayer & 1) &&
            (uint8_t)(config.rollback_budget < 1 ? 1
                : config.rollback_budget > 32 ? 32 : config.rollback_budget)
                == s_engine.MaxRollback();
        if (newEpoch > s_epoch && configCompatible) {
            if (s_engine.RotateEpoch(newEpoch, s_engine.SimFrontier())) {
                s_epoch = newEpoch;
                s_baselineChecksum = config.baseline_checksum;
                s_frameOriginAbs = config.frame_origin_abs;
                s_sessionError[0] = '\0';
                s_terminalReported = false;
                s_passStep = PassStep::Idle;
                s_rollingBack = false;
                s_lastStreamSendMs = GetTickCount();
                memset(s_delayedStream, 0, sizeof(s_delayedStream));

                // Epoch rotation execution (§2.6.5): invalidate every
                // savestate slot and re-tag under the new epoch — the AS2
                // analog of reregisterFrameData().
                StateHistory_Reset();
                StateHistory_SetTagContext(s_epoch, /*phase=*/(uint32_t)MODE_MATCH);
                DesyncDump_Reset();
                s_diagRing.Reset();
                FineDiag_Reset();

                // Delay may have changed between matches (peer-local knob).
                const int delay = config.initial_delay < 0 ? 0
                    : config.initial_delay > 15 ? 15 : config.initial_delay;
                if ((uint8_t)delay != s_engine.ActiveDelay()) {
                    s_engine.RequestInputDelay((uint8_t)delay);
                }

                ApplyStressHooks(s_engine);

                s_active = true;
                s_suspended = false;
                NetplayLog_Write("ROLLBACK", RbFrame(s_engine.SimFrontier()),
                    "engine2 EPOCH ROTATION: epoch=%u origin_canonical=%u "
                    "origin_abs=%d delay=%d budget=%u (engine survived the match boundary)",
                    s_epoch, s_engine.SimFrontier(), s_frameOriginAbs,
                    delay, s_engine.MaxRollback());
                return true;
            }
            NetplayLog_Write("ROLLBACK", RbFrame(s_engine.SimFrontier()),
                "engine2 RotateEpoch refused (%s) — falling back to full re-arm",
                s_engine.TerminalDetail());
        } else {
            NetplayLog_Write("ROLLBACK", RbFrame(s_engine.SimFrontier()),
                "engine2 rotation not applicable (epoch %u -> %u compatible=%d) — full re-arm",
                s_epoch, newEpoch, configCompatible ? 1 : 0);
        }
        // Not rotatable: disarm and fall through to the fresh-arm path.
        s_engine.Disarm();
        s_suspended = false;
    }

    EngineConfig ec{};
    ec.local_player = (uint8_t)(config.local_player & 1);
    ec.input_delay = (uint8_t)(config.initial_delay < 0 ? 0
                        : config.initial_delay > 15 ? 15 : config.initial_delay);
    ec.max_rollback = (uint8_t)(config.rollback_budget < 1 ? 1
                        : config.rollback_budget > 32 ? 32 : config.rollback_budget);
    ec.neutral_input = 0x0000;
    ec.first_frame = 0;          // canonical origin; epochs never reset it
    ec.max_remote_future = 120;
    ec.history_capacity = 4096;

    // Epoch authority (M5, §2.5): match_setup mints/adopts the session's
    // epoch; the adapter arms the engine and tags savestates under it. A
    // zero epoch means the harness drives the adapter without a pregame —
    // fall back to 1 so offline bring-up keeps working.
    s_epoch = Net::PregameSync_GetCurrentEpoch();
    if (s_epoch == 0) {
        s_epoch = 1;
    }
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
    memset(s_delayedStream, 0, sizeof(s_delayedStream));

    StateHistory_Reset();
    StateHistory_SetTagContext(s_epoch, /*phase=*/(uint32_t)MODE_MATCH);
    DesyncDump_Reset();
    s_diagRing.Reset();
    FineDiag_Reset();

    ApplyStressHooks(s_engine);

    s_active = true;
    s_suspended = false;
    NetplayLog_Write("ROLLBACK", 0,
        "engine2 session begin: local=P%d delay=%d budget=%d origin_abs=%d epoch=%u",
        s_localPlayer + 1, ec.input_delay, ec.max_rollback, s_frameOriginAbs, s_epoch);
    return true;
}

void RollbackSession_End() {
    if (!s_active && !s_suspended) return;
    if (s_active) {
        // M7 (S-5/S-6): flush any confirmed tail into the spectator archive
        // and the replay recorder before the engine identity disappears.
        DrainConfirmSeam();
    }
    const int32_t frame = RbFrame(s_engine.SimFrontier());
    s_active = false;
    s_suspended = false;
    s_matchExitPending = false;
    s_passStep = PassStep::Idle;
    s_rollingBack = false;
    memset(s_delayedStream, 0, sizeof(s_delayedStream));
    s_engine.SetPredictionTap(nullptr);
    s_engine.SetForcedRollback(0);
    s_engine.Disarm();
    StateHistory_SetTagContext(0, 0);
    NetplayLog_Write("ROLLBACK", frame,
        "engine2 session end: confirmed=%d rollbacks=%u",
        RbFrame(s_engine.ConfirmedFrontier()), s_engine.GetStats().rollbacks);
}

void RollbackSession_SuspendBetweenMatches(const char* reason) {
    // M6 (§2.6.3): the match ended but the SESSION lives on. Dispatch and
    // queries go inactive; the engine stays ARMED so the next Begin under a
    // higher epoch rotates (INV-15: the canonical counter never resets).
    if (!s_active) return;
    if (s_engine.Terminal() != EngineTerminal::None) {
        // A faulted engine has nothing worth preserving.
        RollbackSession_End();
        return;
    }
    // M7 (S-5/S-6): final seam flush at the match boundary — the director
    // fires SpectatorRuntime_OnMatchEnd right after this returns, and the
    // epoch rotates before the next drain would run (a stale-origin record
    // would otherwise be dropped by the seam's matchRel guard).
    DrainConfirmSeam();
    s_active = false;
    s_suspended = true;
    s_matchExitPending = false;
    s_passStep = PassStep::Idle;
    s_rollingBack = false;
    memset(s_delayedStream, 0, sizeof(s_delayedStream));
    // The frontend lockstep owns the winscreen stream — fence the producer
    // for the whole suspension (§2.7.3-P).
    s_engine.SetProducerFenced(true);
    NetplayLog_Write("ROLLBACK", RbFrame(s_engine.SimFrontier()),
        "engine2 suspended at match boundary (%s): canonical=%u confirmed=%d "
        "epoch=%u — engine stays armed for rotation",
        reason ? reason : "?",
        s_engine.SimFrontier(),
        RbFrame(s_engine.ConfirmedFrontier()),
        s_epoch);
}

void RollbackSession_SetMatchExitPending(bool pending) {
    s_matchExitPending = pending;
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

    // Refresh stress hooks on healthy passes too (idempotent): the forced
    // deep-rollback fight-substate gate must react within a frame of a
    // substate change (PollSession only runs on stall passes).
    ApplyStressHooks(s_engine);

    // Stress delivery-delay queue drains on healthy passes too (wall-clock
    // release; PollSession covers the stalled passes).
    DrainDelayedStreamQueue();

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
    // Stress hooks can be toggled live from the mod menu — keep the
    // prediction tap, forced-rollback depth, and delayed-queue state
    // coherent (M6).
    ApplyStressHooks(s_engine);
    DrainDelayedStreamQueue();
    // Producer fence (§2.7.3-P, M5 obligation closed): the stalled-producer
    // must not feed the gameplay stream while a frontend lockstep phase owns
    // the input exchange or the pregame machine is mid-barrier — those
    // regimes own their own streams. NOT fenced during the continue prompt's
    // gameplay tail: mode 9 rides the winscreen lockstep stream, which is an
    // input phase and therefore fences here by construction.
    s_engine.SetProducerFenced(
        Net::FrontendInputSync_IsInputPhaseActive() || Net::PregameSync_IsActive());
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
        // §2.8.6(d) mode-change bail (M6): if the last committed replay tick
        // crossed a native mode/substate boundary earlier than the
        // speculative timeline did, truncate the frontier at the cursor
        // instead of replaying a stale speculative suffix.
        //
        // Deep-forced-rollback amendment (2026-08-17): the bail may only
        // fire once the replay cursor has reached the CONFIRMED frontier.
        // Forced transactions (SetForcedRollback) replay confirmed spans
        // every frame; when such a window covers a round transition the
        // live substate legitimately leaves FIGHT mid-replay, and the old
        // unconditional truncation would set the sim frontier BELOW the
        // confirmed frontier — a canonical-counter regression (INV-15).
        // Confirmed frames are final: re-running them across a native
        // boundary is a faithful re-run, not stale speculation. Real
        // corrections are unaffected (a mismatch is always at/after the
        // confirmed frontier, so their cursors satisfy the gate already).
        {
            uint32_t mode = 0, substate = 0;
            bool readOk = true;
            __try {
                mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
                substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                readOk = false;
            }
            if (readOk &&
                (mode != (uint32_t)LIFECYCLE_MODE_MATCH ||
                 substate != (uint32_t)LIFECYCLE_SUBSTATE_FIGHT) &&
                !Net::frameBefore(s_engine.ReplayCursor(),
                                  s_engine.ConfirmedFrontier())) {
                NetplayLog_Write("ROLLBACK", RbFrame(s_engine.SimFrontier()),
                    "engine2 replay crossed a native boundary (mode=%u sub=%u) — "
                    "truncating at replay cursor (§2.8.6-d)",
                    mode, substate);
                if (!s_engine.FinishRollbackAtBoundary()) {
                    ReportEngineTerminal();
                    return EventResult::Error;
                }
                Net::DelayPolicy_OnRollbackApplied(s_engine.ActiveDelay());
                NoteRollbackTransactionDone(/*truncated=*/true);
                s_rollingBack = false;
                DrainConfirmSeam();
                return EventResult::Done;
            }
        }
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
        NoteRollbackTransactionDone(/*truncated=*/false);
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

namespace {

void IngestInputStreamNow(const Net::InputStreamPayload& p) {
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

// M6 stress ingest: release queued delivery-delayed packets whose wall-clock
// release time has arrived (called from the BeginFrame and PollSession
// drains, so it runs on healthy AND stalled passes — see the wall-clock
// rationale at DelayedStreamPacket).
void DrainDelayedStreamQueue() {
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < kDelayedStreamMax; ++i) {
        if (s_delayedStream[i].valid &&
            (int32_t)(now - s_delayedStream[i].release_at_ms) >= 0) {
            s_delayedStream[i].valid = false;
            IngestInputStreamNow(s_delayedStream[i].payload);
        }
    }
}

// Forced-deep-rollback per-second visibility line (declared above).
void NoteRollbackTransactionDone(bool truncated) {
    const int cfgDepth = StressHooks_IsEnabled()
        ? StressHooks_GetForcedRollbackDepth() : 0;
    if (cfgDepth <= 0) return;
    const uint32_t depth = s_engine.GetStats().last_rollback_length;
    if (s_forcedStatTx == 0) {
        s_forcedStatDepthMin = depth;
        s_forcedStatDepthMax = depth;
    } else {
        if (depth < s_forcedStatDepthMin) s_forcedStatDepthMin = depth;
        if (depth > s_forcedStatDepthMax) s_forcedStatDepthMax = depth;
    }
    ++s_forcedStatTx;
    if (truncated) ++s_forcedStatTruncated;
    const DWORD now = GetTickCount();
    if (s_forcedStatWindowStartMs == 0) {
        s_forcedStatWindowStartMs = now;
        s_forcedStatEngineBase = s_engine.GetStats().forced_transactions;
    }
    if ((DWORD)(now - s_forcedStatWindowStartMs) >= 1000) {
        const uint32_t engForced = s_engine.GetStats().forced_transactions;
        const uint32_t forcedTx = engForced - s_forcedStatEngineBase;
        const uint32_t realTx =
            s_forcedStatTx > forcedTx ? s_forcedStatTx - forcedTx : 0;
        // Evidence line: `transactions` counts ENGINE-SYNTHESIZED forced
        // restore/replay cycles only (SetForcedRollback pending-mismatch
        // path); real prediction corrections are reported separately.
        const auto& est = s_engine.GetStats();
        NetplayLog_Write("FORCED", RbFrame(s_engine.ConfirmedFrontier()),
            "forced_rb: transactions=%u/s depth=%d achieved_min=%u max=%u "
            "real_corrections=%u truncated=%u replay_verified=%u replay_bad=%u",
            forcedTx, cfgDepth, s_forcedStatDepthMin,
            s_forcedStatDepthMax, realTx, s_forcedStatTruncated,
            est.replay_verifications, est.replay_mismatches);
        if (est.replay_mismatches != s_forcedLive.replay_mismatches) {
            // Local nondeterminism: the same frame, replayed from a restored
            // snapshot with identical inputs, produced a different state.
            NetplayLog_Write("FORCED", (int32_t)est.last_replay_mismatch_frame,
                "REPLAY MISMATCH (local nondeterminism): frame=%u expected_pre_hash=0x%016llX "
                "replayed=0x%016llX total_bad=%u/%u",
                est.last_replay_mismatch_frame,
                (unsigned long long)est.last_replay_expect_hash,
                (unsigned long long)est.last_replay_actual_hash,
                est.replay_mismatches, est.replay_verifications);
            LOG_NETPLAY(LOG_WARNING,
                "[Forced] Replay mismatch at frame %u — the sim did not reproduce under save/restore",
                est.last_replay_mismatch_frame);
        }
        s_forcedLive.replay_verifications = est.replay_verifications;
        s_forcedLive.replay_mismatches = est.replay_mismatches;
        s_forcedLive.transactions_per_sec = forcedTx;
        s_forcedLive.real_corrections_per_sec = realTx;
        s_forcedLive.achieved_min = s_forcedStatDepthMin;
        s_forcedLive.achieved_max = s_forcedStatDepthMax;
        s_forcedLive.truncated_per_sec = s_forcedStatTruncated;
        s_forcedStatWindowStartMs = now;
        s_forcedStatEngineBase = engForced;
        s_forcedStatTx = 0;
        s_forcedStatTruncated = 0;
    }
}

} // namespace

void RollbackSession_GetForcedStats(ForcedRollbackLiveStats* out) {
    if (!out) return;
    *out = s_forcedLive;
    out->configured_depth = StressHooks_GetForcedRollbackDepth();
    out->config_source = StressHooks_GetConfigSource();
    if (out->configured_depth <= 0) {
        out->gate_reason = "not armed";
        out->effective_depth_now = 0;
    }
}

void RollbackSession_OnInputStreamPacket(const void* data, size_t len) {
    // v2 ingest (§3.2 InputStreamPayload) — the M5 routing-table entry point.
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

    // M6 stress ingest hook (§2.7.8): delivery-delay holds the whole packet
    // for N frames before the engine sees it (test methodology preserved).
    const int deliveryDelay = StressHooks_IsEnabled()
        ? StressHooks_GetInputDeliveryDelay() : 0;
    if (deliveryDelay > 0) {
        // Wall-clock release: N frames of emulated one-way latency at the
        // 60 Hz cadence (16.67 ms/frame, integer ms is plenty for a stress
        // knob). Stall-independent by construction.
        const DWORD releaseAt =
            GetTickCount() + (DWORD)((deliveryDelay * 1667) / 100);
        for (size_t i = 0; i < kDelayedStreamMax; ++i) {
            if (!s_delayedStream[i].valid) {
                s_delayedStream[i].payload = p;
                s_delayedStream[i].release_at_ms = releaseAt;
                s_delayedStream[i].valid = true;
                return;
            }
        }
        // Queue full: deliver the oldest immediately, hold this one in its
        // slot — the redundant window makes ordering irrelevant.
        IngestInputStreamNow(s_delayedStream[0].payload);
        s_delayedStream[0].payload = p;
        s_delayedStream[0].release_at_ms = releaseAt;
        return;
    }

    IngestInputStreamNow(p);
}

void RollbackSession_OnSyncHashPacket(const void* data, size_t len) {
    // §2.7.7 confirmed-frame verification ingest (routed by packet_router,
    // session_id gated there for this v2-only type).
    if (!s_active || !data) return;
    if (len < sizeof(Net::SyncHashPayload)) {
        NetplayLog_Verbose("ROLLBACK", RbFrame(s_engine.SimFrontier()),
            "engine2 ingest: short SyncHash payload (%zu < %zu), dropped",
            len, sizeof(Net::SyncHashPayload));
        return;
    }
    Net::SyncHashPayload p{};
    memcpy(&p, data, sizeof(p));

    const uint64_t sid = Net::Session2_GetSessionId();
    if (sid != 0 && p.session_id != 0 && p.session_id != sid) {
        return;  // §3.1: wrong session_id is structurally inert
    }

    (void)s_engine.ReceiveSyncHash(p);
    ReportEngineTerminal();
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
    return s_frameOriginAbs + (RbFrame(s_engine.SimFrontier()) - RbEpochOrigin());
}

int32_t RollbackSession_RbFrameToGameAbs(int32_t rb_frame) {
    return s_frameOriginAbs + (rb_frame - RbEpochOrigin());
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
    // Link stats from time_probe (M6, §2.9.3 — µs source, ms display).
    if (Net::TimeProbe_HasMeasurement()) {
        out->link_avg_ping = (float)Net::TimeProbe_GetRttP50Us() / 1000.0f;
        out->link_jitter = (float)Net::TimeProbe_GetJitterP95Us() / 1000.0f;
    }
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
    // PERF (2026-08-17 live-run finding): GetSnapshot used to compute
    // ComputeLiveChecksumInternal() here — a full 253 KB CRC pass hidden in
    // a counters getter. RollbackDebug_IsRbFrameReadyToCompare calls this
    // per DEFERRED synctrace pair per frame; with integrity tracing armed
    // the deferred set grew by one pair per 60 confirmed frames and the
    // per-frame cost grew ~22 µs/frame until the pair ran at 9 sim fps
    // (profiled: 50% CalcCRC32, 50% lock wait). Diagnostic consumers that
    // genuinely need the live CRC call
    // RollbackSession_ComputeLiveStateChecksum() explicitly at their own
    // (dump-time / debug-window) cadence.
    out->current_checksum = 0;

    out->total_predictions = (int32_t)st.predictions_used;
    out->total_mispredictions = (int32_t)st.mispredictions;
    out->total_correct_predictions = (int32_t)st.correct_predictions;

    out->local_inputs_sent = (int32_t)s_localInputsSent;
    out->remote_inputs_received = (int32_t)s_remoteInputsRecv;

    // Link stats from time_probe (M6, §2.9.3).
    if (Net::TimeProbe_HasMeasurement()) {
        out->link_avg_ping = (float)Net::TimeProbe_GetRttP50Us() / 1000.0f;
        out->link_jitter = (float)Net::TimeProbe_GetJitterP95Us() / 1000.0f;
    } else {
        out->link_avg_ping = 0.0f;
        out->link_jitter = 0.0f;
    }

    // Peer advisory readouts for the M6 HUD (INV-23: display only).
    out->peer_produced_frontier = s_engine.PeerProducedFrontier();
    out->peer_prediction_depth = s_engine.PeerPredictionDepth();
    out->peer_run_state = 0;
    out->peer_adv_delay = s_engine.PeerAdvisoryDelay();
    out->peer_adv_rollback = s_engine.PeerAdvisoryRollback();
}

uint32_t RollbackSession_ComputeLiveStateChecksum() {
    return ComputeLiveChecksumInternal();
}

void RollbackSession_NotifyPeerDesyncGoodbye(const char* human) {
    // The PEER detected the ConfirmedDesync and its goodbye names it; dump
    // OUR matching half of the evidence (ring + region CRCs for the same
    // confirmed-frame window) before teardown completes, so both files
    // exist for tools/compare_desync_dumps.py. Bounded: one cooldown-guarded
    // file write + a log line — no termination logic lives here (the
    // existing Disconnect-receive path proceeds unchanged).
    if (!s_active && !s_suspended) {
        return;  // nothing recorded — no session ring to dump
    }
    const int32_t rbConfirmed = RbFrame(s_engine.ConfirmedFrontier());
    NetplayLog_Write("ROLLBACK", rbConfirmed,
        "peer goodbye reason=ConfirmedDesync — dumping surviving-side "
        "diagnostics (confirmed=%d, peer said: %s)",
        rbConfirmed, (human && human[0]) ? human : "?");
    NetplayLog_Flush();

    // This side usually never saw a mismatch (its verify raced the peer's),
    // but include local evidence when it exists (both-sides-detect race).
    DesyncEvidence ev{};
    const bool haveEv = s_engine.GetDesyncEvidence(&ev);
    DesyncDump_TryDumpWithDiagnostics(rbConfirmed,
                                      ComputeLiveChecksumInternal(), 0,
                                      "peer-synchash-goodbye", human,
                                      &s_diagRing, haveEv ? &ev : nullptr);
}

} // namespace Rollback
