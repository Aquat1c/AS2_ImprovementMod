/**
 * RollbackEngine core
 *
 * The custom rollback engine: socket-free, clock-free, game-memory-free.
 * It owns NO ENet, NO QPC, NO game addresses.
 * It decides which canonical inputs a frame consumes and when the caller
 * must stall or restore/replay. The game adapter (rollback_session_engine2)
 * and the unit/soak harness (tests/engine2_tests) exercise the identical
 * object.
 *
 * Enforced invariants:
 *   - no debt/gap quantity exists; the only network hold is
 *     speculativeFrames() >= R_local (the sole reader of R)
 *   - one canonical u32 frame counter, wrap-safe half-open frontiers,
 *     never reset — epochs qualify state identity, not time
 *   - no wall clock: every timeout in here is frame-counted
 *   - capture-once: one physical sample per visible source frame;
 *     assigned slots are immutable forever (repeat calls adopt)
 *   - never fabricate remote inputs: predict-and-correct or stall;
 *     Conflict / InvalidValue are fail-closed terminals
 *   - the producer keeps feeding the opponent through stalls, bounded
 *     by the peer's advertised consumption capacity
 *   - predicted frames never cross an exact-input lifecycle window
 *
 * Internal invariant violations (capture inside a rollback transaction,
 * commit without a plan, ring overflow) do not assert in release: they set a
 * typed fail-closed terminal the caller must check (`Terminal()`), which the
 * unit tests pin.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "net/frame_arithmetic.h"
#include "net/protocol.h"
#include "rollback/desync_diag.h"
#include "rollback/input_timeline.h"
#include "rollback/prediction.h"
#include "rollback/run_state.h"

namespace Rollback {

// Configuration

struct EngineConfig {
    uint8_t  local_player = 0;        // 0/1 from PlayerMapping_DeriveFromRole
    uint8_t  input_delay = 0;         // D_local, 0..15, peer-local
    uint8_t  max_rollback = 7;        // R_local, 1..15; sole use: the prediction-limit check
    uint16_t neutral_input = 0x0000;  // 0xFFFF unrepresentable
    uint32_t first_frame = 0;         // canonical frame at arm (never resets)
    uint32_t max_remote_future = 120; // remote input beyond frontier+N dropped
    uint32_t history_capacity = 4096; // input records per side (ring)
};

/// Valid AS2 input word: INPUT_UP..INPUT_R2 (input_system.h bits 0..13).
// Bits 0..13 are the game's buttons; bit 14 is the reserved non-game transport
// bit (INPUT_MENU_HOLD). It must survive sealing or it never reaches the peer.
constexpr uint16_t ENGINE_INPUT_VALID_MASK = 0x7FFF;
/// The game's "no input yet" history marker — invalid on the wire.
constexpr uint16_t ENGINE_INPUT_INVALID_WORD = 0xFFFF;

/// SyncHash cadence: every 30 confirmed frames.
constexpr uint32_t ENGINE_SYNC_HASH_INTERVAL = 30;
/// Bounded queue of not-yet-comparable peer hashes.
constexpr uint32_t ENGINE_SYNC_HASH_QUEUE_MAX = 128;
/// Producer hard cap: kInputWindowFrames(32) − 2, so the un-acked suffix
/// always fits one InputStream packet.
constexpr uint32_t ENGINE_PRODUCER_HARD_CAP = 30;
/// How far past the local sim frontier the producer may seal, ON TOP of the
/// configured delay. This is an INPUT LATENCY budget, not a bandwidth one:
/// a sealed frame is immutable, so every frame the producer runs ahead of
/// the sim is a frame whose input the player can no longer change.
/// produced_frontier cannot regress, and the engine survives match
/// boundaries, so excess lead outlives the match that caused it.
constexpr uint32_t ENGINE_PRODUCER_SIM_LEAD_CAP = 2;
/// Delay-lowering drain timeout, frame-counted ("5 s" at 60 Hz).
constexpr uint32_t ENGINE_DELAY_DRAIN_TIMEOUT_FRAMES = 300;
/// Executed-frame bookkeeping ring (must exceed R + confirm-consumer lag).
constexpr uint32_t ENGINE_EXEC_RING = 1024;

// Typed results

/// Ingest taxonomy.
enum class IngestResult : uint8_t {
    Applied = 0,
    DuplicateIdentical,  // ignore
    Conflict,            // fatal protocol violation (terminal set)
    InvalidValue,        // fatal: 0xFFFF or bits outside the AS2 mask
    TooFarFuture,        // drop: beyond remote frontier + max_remote_future
    Stale,               // ignore: at/behind the retention window
};

enum class EngineActionKind : uint8_t {
    Advance = 0,   // run one canonical frame with `inputs`
    Stall,         // hold this opportunity for `hold_cause`
    Rollback,      // restore at `frame`, replay [frame, replay_until)
};

struct EngineAction {
    EngineActionKind kind = EngineActionKind::Stall;
    HoldCause hold_cause = HoldCause::None;   // Stall only
    uint32_t  frame = 0;                      // Advance frame / rollback from
    uint32_t  replay_until = 0;               // Rollback: exclusive frontier
    uint16_t  inputs[2] = {0, 0};             // Advance: canonical P1/P2
    bool      remote_predicted = false;       // Advance: remote input predicted
};

/// The immutable confirm seam: feeds SyncHash, spectator push,
/// and the replay recorder. A predicted value can never appear here.
struct ConfirmedFrame {
    uint32_t frame = 0;
    uint16_t inputs[2] = {0, 0};
    uint64_t pre_state_hash = 0;
    uint32_t epoch = 0;
    // Diagnostics captured pre-tick by the adapter (SyncHash fields).
    uint32_t rng_state = 0;
    uint16_t hp0 = 0;
    uint16_t hp1 = 0;
};

/// Outgoing SyncHash record (adapter adds session_id and sends).
struct OutgoingSyncHash {
    uint32_t epoch = 0;
    uint32_t frame = 0;
    uint64_t gameplay_hash = 0;
    uint32_t rng_state = 0;
    uint16_t hp0 = 0;
    uint16_t hp1 = 0;
};

/// Peer-hash verification outcome.
enum class HashVerify : uint8_t {
    NoneReady = 0,     // nothing comparable yet
    Ok,                // exact match across all fields
    StaleEpoch,        // ignored (rotation raced the pipe)
    QueuedFuture,      // future frame/epoch queued
    Mismatch,          // ConfirmedDesync terminal set
    InconsistentEpoch, // protocol-violation terminal set
};

/// Fail-closed terminal causes.
enum class EngineTerminal : uint8_t {
    None = 0,
    InputConflict,        // ingest: different value for an actual frame
    InputInvalid,         // ingest: 0xFFFF or out-of-mask bits
    ConfirmedDesync,      // SyncHash mismatch at a confirmed frame
    EpochInconsistent,    // same frame, different epoch lineage
    HashQueueOverflow,    // peer hash queue exceeded bound (D-3)
    InternalInvariant,    // engine misuse (capture in transaction, etc.)
    BadConfig,            // rejected EngineConfig at arm
};

const char* EngineTerminalName(EngineTerminal t);

// The engine

class RollbackEngine {
public:
    RollbackEngine() = default;

    // Lifecycle

    /// Arm once per session. `epoch` is the transport-minted generation
    /// (host-minted u32, starts at 1; 0 = "no epoch").
    bool Arm(const EngineConfig& config, uint32_t epoch);

    /// Epoch rotation at a match boundary: canonical counter and
    /// input streams CONTINUE; only state identity and the game-frame origin
    /// change. Savestate/audio-journal invalidation is the director's job.
    /// Illegal inside a rollback transaction.
    bool RotateEpoch(uint32_t new_epoch, uint32_t epoch_frame_origin);

    void Disarm();
    bool Armed() const { return armed_; }
    EngineTerminal Terminal() const { return terminal_; }
    const char* TerminalDetail() const { return terminal_detail_; }

    // Capture / delay

    /// Exactly one physical sample per visible source frame; schedules the
    /// word at `source_frame + D_local`. Repeat calls while stalled (same
    /// source frame) ADOPT the sealed record. Returns true when a new record
    /// was sealed, false when adopted. Illegal inside a rollback transaction.
    bool CaptureLocalInput(uint32_t source_frame, uint16_t value);

    /// D changes are peer-local. Raising fills only missing future
    /// slots with neutral (immediate). Lowering drains the longer pipeline at
    /// a fully-confirmed boundary without recapture; if the drain has not
    /// completed after ENGINE_DELAY_DRAIN_TIMEOUT_FRAMES capture
    /// opportunities, the proven old value is restored.
    bool RequestInputDelay(uint8_t new_delay);
    uint8_t ActiveDelay() const { return delay_active_; }
    bool DelayChangePending() const { return delay_pending_valid_; }

    // Producer while stalled

    /// Seal one local record at the produced frontier (called once per frame
    /// period by the adapter while the sim holds; the engine itself stays
    /// clock-free). Prefers the fresh device sample so presses during a
    /// stall survive. Bounded by min(peerR + peerD + 2, 30) frames ahead of
    /// the peer's ack. Returns true when a record was sealed.
    bool ProduceLocalInputAhead(uint16_t fresh_sample);

    /// Fenced OFF during lifecycle barriers and frontend phases.
    void SetProducerFenced(bool fenced) { producer_fenced_ = fenced; }
    bool ProducerFenced() const { return producer_fenced_; }

    // Ingest

    IngestResult ReceiveRemoteInput(uint32_t frame, uint16_t value);

    /// Full InputStream window ingest: idempotent merge, ack re-anchoring,
    /// peer advisory capture. Returns false when a terminal was set.
    bool IngestInputStream(const Net::InputStreamPayload& p);

    /// Adopt the peer's advisory PressureReport fields (HUD/coverage inputs;
    /// adv_delay/adv_rollback bound the producer, never applied locally).
    void SetPeerAdvisory(const Net::PressureReport& pressure);

    // Lifecycle exact windows

    /// The adapter computes LifecycleWindow_IsExactInputNext each pass and
    /// mirrors it here; tests drive the flag directly.
    void SetLifecycleExactNext(bool exact) { lifecycle_exact_next_ = exact; }

    // Test-only prediction tap

    /// Installed by the adapter when stress hooks are enabled (null in
    /// production): every predicted remote value passes through the tap, so
    /// the harness can force a genuine mispredict → rollback without ever
    /// touching actual inputs. The engine masks the
    /// result to ENGINE_INPUT_VALID_MASK.
    using PredictionTap = uint16_t (*)(uint16_t predicted);
    void SetPredictionTap(PredictionTap tap) { prediction_tap_ = tap; }

    /// Forced-rollback mode (determinism suite / stress hooks): while
    /// depth > 0, every pass whose sim frontier advanced since the last
    /// forced transaction synthesizes a depth-N correction — a genuine
    /// BeginRollback/replay/Finish cycle over the SAME sealed local and
    /// actual/predicted remote inputs — even when every prediction was
    /// correct. A deterministic sim therefore reproduces identical state;
    /// any divergence surfaces as a SyncHash / confirmed-stream mismatch.
    /// The synthetic mismatch is clamped to the current epoch's frame
    /// origin (state identity changed there) and yields to a real
    /// pending mismatch. Fully deterministic (no clock, no RNG);
    /// 0 disables (production default).
    void SetForcedRollback(uint8_t depth);
    uint8_t ForcedRollbackDepth() const { return forced_rollback_depth_; }

    // Per-opportunity decision

    /// Evaluated once per scheduler pass (and between batch iterations):
    ///   pending mismatch                       -> Rollback   (runs FIRST)
    ///   exact boundary && !remote actual       -> Stall(LifecycleBoundary)
    ///   !local input sealed for the frame      -> Stall(LocalInputMissing)
    ///   speculativeFrames() >= R_local         -> Stall(PredictionLimit)
    ///   else                                   -> Advance
    /// The PredictionLimit comparison is the ONLY reader of R_local.
    EngineAction NextAction();

    /// Seal the advance planned by the last NextAction(): the adapter saved
    /// the pre-tick snapshot (hash = digest of sim-affecting bytes) and ran
    /// the tick. Diagnostics ride into the confirm seam.
    bool CommitAdvance(uint32_t frame, uint64_t pre_state_hash,
                       uint32_t rng_state = 0, uint16_t hp0 = 0, uint16_t hp1 = 0);

    // Rollback transaction

    bool BeginRollback(uint32_t from);
    /// Canonical inputs for the next replay tick (actuals where known now,
    /// fresh hold-last prediction otherwise).
    bool NextReplayInputs(uint32_t* frame, uint16_t inputs[2]);
    bool CommitReplayFrame(uint32_t frame, uint64_t pre_state_hash,
                           uint32_t rng_state = 0, uint16_t hp0 = 0, uint16_t hp1 = 0);
    /// Replay complete: cursor reached the speculative frontier.
    bool FinishRollback();
    /// Corrected replay crossed a native boundary EARLIER than the
    /// speculative timeline did: discard the speculative suffix, truncate
    /// the frontier at the replay cursor.
    bool FinishRollbackAtBoundary();
    /// An uncommitted replay tick would cross a lifetime boundary without a
    /// full actual prefix: stop before it (frontier truncates to cursor).
    bool FinishRollbackBeforeBoundary();
    bool InRollback() const { return in_rollback_; }
    /// Next frame the open transaction will replay (valid while InRollback).
    /// The adapter's boundary bail consults it: replaying a CONFIRMED frame
    /// across a native boundary is a faithful re-run (forced deep-rollback
    /// mode replays confirmed spans every frame), so the bail may only
    /// truncate once the cursor has reached the speculative suffix —
    /// truncating below the confirmed frontier would regress the canonical
    /// counter.
    uint32_t ReplayCursor() const { return replay_cursor_; }
    uint32_t ReplayTarget() const { return replay_target_; }
    /// True while every frame replayed so far in this transaction re-ran with
    /// byte-identical inputs — i.e. the replay is reproducing stored state
    /// rather than computing new state.
    bool ReplayPrefixIdentical() const { return replay_prefix_identical_; }

    /// Suppress the determinism comparison for the next CommitReplayFrame.
    /// Set by the adapter when it reused a STORED digest instead of hashing
    /// live memory: comparing a stored value against itself is a tautology,
    /// and counting it would inflate replay_verifications with checks that
    /// cannot fail.
    void SuppressNextReplayVerify() { suppress_replay_verify_ = true; }

    // Confirm pipeline

    bool PopConfirmedFrame(ConfirmedFrame* out);

    // SyncHash

    bool PopOutgoingSyncHash(OutgoingSyncHash* out);
    HashVerify ReceiveSyncHash(const Net::SyncHashPayload& p);
    /// Re-check queued peer hashes against newly confirmed frames. Call
    /// after confirmation advances (adapter: once per pass).
    HashVerify PumpSyncHashVerify();

    /// Evidence of the failing SyncHash pair (local vs peer, ALL fields),
    /// captured at the instant the ConfirmedDesync terminal was set.
    /// Additive diagnostics: nothing in the decision path reads it.
    /// Returns false until a mismatch has occurred (cleared at Arm).
    bool GetDesyncEvidence(DesyncEvidence* out) const {
        if (!desync_evidence_.valid) return false;
        if (out) *out = desync_evidence_;
        return true;
    }

    // Wire building

    /// Redundant window anchored at peer_ack_through+1 — exactly the
    /// un-acked suffix, capped at 32. session_id is left 0 for the adapter.
    /// False when nothing has been sealed yet.
    bool BuildInputStream(Net::InputStreamPayload* out);
    bool HasUnackedSuffix() const;
    void FillPressureReport(Net::PressureReport* out) const;

    // Frontiers / telemetry (all half-open)

    uint32_t SimFrontier() const { return sim_frontier_; }
    uint32_t ConfirmedFrontier() const { return confirmed_frontier_; }
    uint32_t RemoteActualFrontier() const { return remote_actual_frontier_; }
    uint32_t ProducedFrontier() const { return produced_frontier_; }
    uint32_t PeerAckFrontier() const { return peer_ack_frontier_; }
    uint32_t Epoch() const { return epoch_; }
    uint32_t EpochFrameOrigin() const { return epoch_frame_origin_; }
    uint32_t FirstFrame() const { return config_.first_frame; }
    uint8_t  MaxRollback() const { return config_.max_rollback; }
    uint8_t  PeerAdvisoryDelay() const { return peer_adv_delay_; }
    uint8_t  PeerAdvisoryRollback() const { return peer_adv_rollback_; }
    uint32_t PeerProducedFrontier() const { return peer_produced_frontier_; }
    uint8_t  PeerPredictionDepth() const { return peer_prediction_depth_; }
    bool     HasPendingMismatch() const { return pending_mismatch_valid_; }
    uint32_t PendingMismatchFrame() const { return pending_mismatch_; }

    /// forwardDistance(remote actual frontier, sim frontier), clamped >= 0.
    uint32_t SpeculativeFrames() const;
    /// signedLead(local produced, peer produced) — TELEMETRY ONLY;
    /// no decision reads it.
    int32_t  FramesAheadSigned() const;
    /// game_abs_frame = canonical − epoch_frame_origin.
    uint32_t GameAbsFrame(uint32_t canonical) const {
        return canonical - epoch_frame_origin_;
    }
    RunState LastRunState() const { return last_run_state_; }

    struct Stats {
        uint32_t rollbacks = 0;
        uint32_t forced_transactions = 0;  // SetForcedRollback-synthesized
        uint32_t real_rollbacks = 0;       // genuine prediction corrections
        // Depth of the last transaction OF EACH KIND.
        bool     last_rollback_forced = false;
        uint32_t last_forced_rollback_length = 0;
        uint32_t last_real_rollback_length = 0;
        uint32_t max_rollback_depth = 0;
        uint32_t last_rollback_from = 0;
        uint32_t last_rollback_length = 0;
        uint32_t predictions_used = 0;      // advances on a predicted remote
        uint32_t mispredictions = 0;
        uint32_t correct_predictions = 0;
        uint32_t remote_inputs_applied = 0;
        uint32_t local_inputs_sealed = 0;
        uint32_t producer_seals = 0;
        uint32_t confirmed_frames = 0;
        uint32_t sync_hashes_sent = 0;
        uint32_t sync_hashes_verified = 0;
        // Local replay determinism self-test. A replayed frame
        // whose inputs are byte-identical to its original execution MUST
        // reproduce the identical pre-state hash. Verified on every replayed
        // frame, so forced depth-N rollback becomes a real determinism test
        // on ONE machine at the exact frame, instead of waiting for the
        // 30-frame cross-peer digest to notice something downstream.
        uint32_t replay_verifications = 0;
        uint32_t replay_mismatches = 0;
        uint32_t last_replay_mismatch_frame = 0;
        uint64_t last_replay_expect_hash = 0;
        uint64_t last_replay_actual_hash = 0;
    };
    const Stats& GetStats() const { return stats_; }

    /// Outcome of the most recent CommitReplayFrame verification, for the
    /// forensic trace: what the frame produced the FIRST time versus what the
    /// replay just produced, and whether the comparison was even possible.
    struct ReplayVerify {
        bool     checked = false;   // false = inputs differed / other epoch
        bool     match = false;
        uint32_t frame = 0;
        uint64_t expected = 0;      // pre-state hash of the original execution
        uint64_t actual = 0;        // pre-state hash of the replay
        uint16_t inputs[2] = {0, 0};
        bool     remote_predicted = false;
    };
    const ReplayVerify& LastReplayVerify() const { return last_replay_verify_; }

    /// Recorded pre-state hash for an executed frame, if the ring still holds
    /// it in this epoch. Read-only; used by the trace to show what a replay is
    /// expected to reproduce BEFORE it runs.
    bool PeekExecPreHash(uint32_t frame, uint64_t* out) const;

private:
    struct ExecRecord {
        uint32_t frame = 0;
        uint16_t inputs[2] = {0, 0};
        uint64_t pre_hash = 0;
        uint32_t epoch = 0;
        uint32_t rng_state = 0;
        uint16_t hp0 = 0;
        uint16_t hp1 = 0;
        bool     remote_predicted = false;
        bool     committed = false;
        bool     valid = false;
    };

    struct HashRecord {
        uint32_t epoch = 0;
        uint32_t frame = 0;
        uint64_t hash = 0;
        uint32_t rng_state = 0;
        uint16_t hp0 = 0;
        uint16_t hp1 = 0;
        bool     valid = false;
    };

    struct PeerHashRecord {
        uint32_t epoch = 0;
        uint32_t frame = 0;
        uint64_t hash = 0;
        uint32_t rng_state = 0;
        uint16_t hp0 = 0;
        uint16_t hp1 = 0;
        bool     valid = false;
    };

    void SetTerminal(EngineTerminal t, const char* fmt, ...);
    bool SealLocal(uint32_t frame, uint16_t value);
    void AdvanceRemoteFrontier();
    void MarkMismatch(uint32_t frame);
    void AdvanceConfirmed();
    ExecRecord* ExecAt(uint32_t frame);
    const ExecRecord* ExecAt(uint32_t frame) const;
    HashRecord* FindLocalHash(uint32_t frame);
    void ClassifyPass(WorkKind kind, HoldCause hold);
    uint16_t RemoteValueOrPredict(uint32_t frame, bool* predicted);

    EngineConfig config_{};
    bool armed_ = false;
    EngineTerminal terminal_ = EngineTerminal::None;
    char terminal_detail_[160] = "";

    uint32_t epoch_ = 0;
    uint32_t epoch_frame_origin_ = 0;

    // Frontiers (half-open, wrap-safe).
    uint32_t sim_frontier_ = 0;        // next canonical frame to simulate
    uint32_t produced_frontier_ = 0;   // one past newest sealed local input
    uint32_t remote_actual_frontier_ = 0;  // contiguous remote actual prefix
    uint32_t confirmed_frontier_ = 0;  // one past newest confirmed frame
    uint32_t confirmed_pop_ = 0;       // consumer cursor into the confirm seam
    uint32_t peer_ack_frontier_ = 0;   // one past newest local frame peer acked

    // Delay pipeline.
    uint8_t  delay_active_ = 0;
    uint8_t  delay_pending_ = 0;
    bool     delay_pending_valid_ = false;
    uint32_t delay_drain_countdown_ = 0;
    uint32_t last_capture_source_ = 0;
    bool     has_captured_ = false;

    // Rollback transaction.
    bool     in_rollback_ = false;
    uint32_t replay_cursor_ = 0;
    uint32_t replay_target_ = 0;
    bool     replay_inputs_pending_ = false;
    uint16_t replay_pending_inputs_[2] = {0, 0};
    bool     replay_pending_predicted_ = false;

    bool     pending_mismatch_valid_ = false;
    uint32_t pending_mismatch_ = 0;

    // ConfirmedDesync evidence (diagnostics only; see GetDesyncEvidence).
    DesyncEvidence desync_evidence_{};

    // Advance plan seal (NextAction -> CommitAdvance).
    bool     advance_plan_valid_ = false;
    uint32_t advance_plan_frame_ = 0;
    uint16_t advance_plan_inputs_[2] = {0, 0};
    bool     advance_plan_predicted_ = false;

    bool     lifecycle_exact_next_ = false;
    bool     producer_fenced_ = false;
    PredictionTap prediction_tap_ = nullptr;

    // Forced-rollback mode (test-only; see SetForcedRollback).
    uint8_t  forced_rollback_depth_ = 0;
    uint32_t forced_rollback_done_ = 0;      // frontier already force-corrected
    bool     forced_rollback_done_valid_ = false;

    // Peer advisory (PressureReport; never applied locally).
    uint8_t  peer_adv_delay_ = 0;
    uint8_t  peer_adv_rollback_ = 0;
    uint32_t peer_produced_frontier_ = 0;
    uint8_t  peer_prediction_depth_ = 0;
    uint8_t  peer_run_state_ = 0;

    InputRing local_;
    InputRing remote_;
    HoldLastPredictor predictor_;

    std::vector<ExecRecord> exec_;         // ENGINE_EXEC_RING, frame-mapped
    uint32_t confirm_count_ = 0;           // confirmed frames since arm
    std::vector<HashRecord> local_hashes_; // cadence ring (128)
    std::vector<PeerHashRecord> peer_hash_queue_; // bounded 128
    uint32_t peer_hash_count_ = 0;

    std::vector<OutgoingSyncHash> outgoing_hashes_; // small FIFO ring
    uint32_t outgoing_hash_head_ = 0;
    uint32_t outgoing_hash_count_ = 0;

    RunState last_run_state_ = RunState::Running;
    Stats stats_{};
    ReplayVerify last_replay_verify_{};
    bool pending_mismatch_forced_ = false;
    // Determinism self-test scope: true while every frame replayed so far in
    // this transaction re-ran with byte-identical inputs.
    bool replay_prefix_identical_ = false;
    bool suppress_replay_verify_ = false;
};

} // namespace Rollback
