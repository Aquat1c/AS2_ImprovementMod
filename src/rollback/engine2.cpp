/**
 * Alice Senki 2 - RollbackEngine core implementation (re0.7 M4, plan §2.7)
 *
 * Pure: no Win32, no game memory, no sockets, no clock, no logging. Every
 * failure mode is a typed fail-closed terminal the caller observes.
 */

#include "rollback/engine2.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace Rollback {

using Net::frameAfter;
using Net::frameAtOrAfter;
using Net::frameBefore;
using Net::forwardDistance;
using Net::signedLead;

const char* EngineTerminalName(EngineTerminal t) {
    switch (t) {
        case EngineTerminal::None:              return "none";
        case EngineTerminal::InputConflict:     return "input-conflict";
        case EngineTerminal::InputInvalid:      return "input-invalid";
        case EngineTerminal::ConfirmedDesync:   return "confirmed-desync";
        case EngineTerminal::EpochInconsistent: return "epoch-inconsistent";
        case EngineTerminal::HashQueueOverflow: return "hash-queue-overflow";
        case EngineTerminal::InternalInvariant: return "internal-invariant";
        case EngineTerminal::BadConfig:         return "bad-config";
    }
    return "?";
}

// ============================================================================
// Internal helpers
// ============================================================================

void RollbackEngine::SetTerminal(EngineTerminal t, const char* fmt, ...) {
    if (terminal_ != EngineTerminal::None) {
        return;  // first terminal wins; terminals are sticky (INV-20)
    }
    terminal_ = t;
    va_list args;
    va_start(args, fmt);
    vsnprintf(terminal_detail_, sizeof(terminal_detail_), fmt, args);
    va_end(args);
    terminal_detail_[sizeof(terminal_detail_) - 1] = '\0';
}

RollbackEngine::ExecRecord* RollbackEngine::ExecAt(uint32_t frame) {
    if (exec_.empty()) return nullptr;
    ExecRecord* rec = &exec_[frame % exec_.size()];
    return (rec->valid && rec->frame == frame) ? rec : nullptr;
}

const RollbackEngine::ExecRecord* RollbackEngine::ExecAt(uint32_t frame) const {
    if (exec_.empty()) return nullptr;
    const ExecRecord* rec = &exec_[frame % exec_.size()];
    return (rec->valid && rec->frame == frame) ? rec : nullptr;
}

RollbackEngine::HashRecord* RollbackEngine::FindLocalHash(uint32_t frame) {
    for (HashRecord& rec : local_hashes_) {
        if (rec.valid && rec.frame == frame) return &rec;
    }
    return nullptr;
}

void RollbackEngine::ClassifyPass(WorkKind kind, HoldCause hold) {
    RunStateInputs in{};
    in.missing_remote_depth = SpeculativeFrames();
    in.local_max_rollback = config_.max_rollback;
    in.kind = kind;
    in.hold = hold;
    in.session_terminated = (terminal_ != EngineTerminal::None) || !armed_;
    last_run_state_ = ClassifyRunState(in);
}

uint16_t RollbackEngine::RemoteValueOrPredict(uint32_t frame, bool* predicted) {
    if (remote_.Has(frame)) {
        if (predicted) *predicted = false;
        return remote_.Value(frame);
    }
    if (predicted) *predicted = true;
    uint16_t value = predictor_.Held();
    // Test-only prediction tap (M6 stress hook, §2.7.8 "StressHooks_* query
    // points"): lets the harness corrupt a prediction to force a genuine
    // mismatch/rollback without touching actuals (which would be a Conflict
    // terminal, INV-19). Null in production unless stress hooks install it.
    if (prediction_tap_) {
        value = prediction_tap_(value) & ENGINE_INPUT_VALID_MASK;
    }
    return value;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool RollbackEngine::Arm(const EngineConfig& config, uint32_t epoch) {
    if (armed_) {
        SetTerminal(EngineTerminal::InternalInvariant, "Arm while armed");
        return false;
    }
    // A failed previous Arm (BadConfig) must not poison a valid retry.
    terminal_ = EngineTerminal::None;
    terminal_detail_[0] = '\0';
    // max_rollback cap raised 15 -> 32 (2026-08-17 deep-rollback cells,
    // then budgets >= 30 for depth-30 forced runs): uint8 advisory wire
    // fields and the adapter's 64-slot StateHistory accommodate 32; the
    // producer hard cap (30) independently bounds run-ahead.
    if (config.local_player > 1 ||
        config.max_rollback < 1 || config.max_rollback > 32 ||
        config.input_delay > 15 ||
        config.neutral_input == ENGINE_INPUT_INVALID_WORD ||
        (config.neutral_input & ~ENGINE_INPUT_VALID_MASK) != 0 ||
        config.history_capacity < 256 ||
        epoch == 0) {
        SetTerminal(EngineTerminal::BadConfig,
                    "bad config: player=%u delay=%u R=%u neutral=0x%04X cap=%u epoch=%u",
                    config.local_player, config.input_delay, config.max_rollback,
                    config.neutral_input, config.history_capacity, epoch);
        return false;
    }

    config_ = config;
    terminal_ = EngineTerminal::None;
    terminal_detail_[0] = '\0';
    epoch_ = epoch;
    epoch_frame_origin_ = config.first_frame;

    sim_frontier_ = config.first_frame;
    produced_frontier_ = config.first_frame;
    remote_actual_frontier_ = config.first_frame;
    confirmed_frontier_ = config.first_frame;
    confirmed_pop_ = config.first_frame;
    peer_ack_frontier_ = config.first_frame;

    delay_active_ = config.input_delay;
    delay_pending_valid_ = false;
    delay_drain_countdown_ = 0;
    has_captured_ = false;
    last_capture_source_ = 0;

    in_rollback_ = false;
    replay_inputs_pending_ = false;
    pending_mismatch_valid_ = false;
    desync_evidence_ = DesyncEvidence{};
    advance_plan_valid_ = false;
    lifecycle_exact_next_ = false;
    producer_fenced_ = false;
    // Forced-rollback depth survives re-arm (like the prediction tap: the
    // installer owns it); the per-frontier marker must not.
    forced_rollback_done_valid_ = false;

    peer_adv_delay_ = 0;
    peer_adv_rollback_ = 0;
    peer_produced_frontier_ = config.first_frame;
    peer_prediction_depth_ = 0;
    peer_run_state_ = 0;

    local_.Reset(config.history_capacity, config.neutral_input);
    remote_.Reset(config.history_capacity, config.neutral_input);
    predictor_.Reset(config.neutral_input);

    exec_.assign(ENGINE_EXEC_RING, ExecRecord{});
    confirm_count_ = 0;
    local_hashes_.assign(ENGINE_SYNC_HASH_QUEUE_MAX, HashRecord{});
    peer_hash_queue_.assign(ENGINE_SYNC_HASH_QUEUE_MAX, PeerHashRecord{});
    peer_hash_count_ = 0;
    outgoing_hashes_.assign(16, OutgoingSyncHash{});
    outgoing_hash_head_ = 0;
    outgoing_hash_count_ = 0;

    last_run_state_ = RunState::Running;
    stats_ = Stats{};

    // Delay = pure relabel: a neutral prefix of D frames is queued at arm
    // (§2.7.3-D). These are canonical, immutable records like any other.
    for (uint8_t i = 0; i < delay_active_; ++i) {
        if (!SealLocal(produced_frontier_, config_.neutral_input)) {
            return false;
        }
    }

    armed_ = true;
    return true;
}

bool RollbackEngine::RotateEpoch(uint32_t new_epoch, uint32_t epoch_frame_origin) {
    if (!armed_ || terminal_ != EngineTerminal::None) return false;
    if (in_rollback_) {
        SetTerminal(EngineTerminal::InternalInvariant, "RotateEpoch inside rollback");
        return false;
    }
    if (new_epoch == 0 || !frameAfter(new_epoch, epoch_)) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "RotateEpoch not strictly increasing: %u -> %u", epoch_, new_epoch);
        return false;
    }
    epoch_ = new_epoch;
    epoch_frame_origin_ = epoch_frame_origin;
    // A mismatch pending on a now-pre-origin frame is dead with its match
    // (see MarkMismatch): the savestates below the origin are being wiped
    // by the adapter, so correcting it is impossible by construction.
    if (pending_mismatch_valid_ &&
        frameBefore(pending_mismatch_, epoch_frame_origin_)) {
        pending_mismatch_valid_ = false;
    }
    return true;
}

void RollbackEngine::Disarm() {
    armed_ = false;
}

void RollbackEngine::SetForcedRollback(uint8_t depth) {
    // Cap raised 15 -> 48 (2026-08-17 deep-rollback acceptance: sustained
    // per-frame depth-30 forced transactions in live runs). Bound: the
    // adapter's StateHistory is 64 direct-mapped slots; forced replays
    // re-capture their whole window every frame, so a restore point
    // forced_depth back is always fresh, and 48 + speculation(<=16) stays
    // within one ring revolution.
    //
    // The done-latch is NEVER reset here (2026-08-17, run 20-46 livelock):
    // the adapter refreshes stress hooks every pass, and near a round
    // boundary its fight-substate gate flaps the depth 0<->N as replays
    // re-run the transition — a depth-change-keyed reset re-armed the SAME
    // frontier every pass and the identical depth-30 transaction
    // re-synthesized forever (observed: from=4067 until=4097 x5916, frontier
    // pinned 20 s -> ProgressDeadline teardown). The latch clears at Arm;
    // a mid-session depth change simply takes effect from the NEXT advanced
    // frontier.
    forced_rollback_depth_ = depth > 48 ? (uint8_t)48 : depth;
}

// ============================================================================
// Capture / delay (§2.7.3 C+D)
// ============================================================================

bool RollbackEngine::SealLocal(uint32_t frame, uint16_t value) {
    if (!frameAtOrAfter(frame, produced_frontier_) || frame != produced_frontier_) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "SealLocal out of order: frame=%u produced=%u", frame, produced_frontier_);
        return false;
    }
    const InputRing::SetResult r = local_.Set(frame, value);
    if (r != InputRing::SetResult::Stored) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "SealLocal ring refused: frame=%u r=%d", frame, (int)r);
        return false;
    }
    produced_frontier_ = frame + 1;
    ++stats_.local_inputs_sealed;
    return true;
}

bool RollbackEngine::CaptureLocalInput(uint32_t source_frame, uint16_t value) {
    if (!armed_ || terminal_ != EngineTerminal::None) return false;
    if (in_rollback_) {
        // No local capture is legal inside the transaction (§2.7.5).
        SetTerminal(EngineTerminal::InternalInvariant,
                    "capture inside rollback transaction: source=%u", source_frame);
        return false;
    }
    value = (uint16_t)(value & ENGINE_INPUT_VALID_MASK);

    // §2.7.3-D: a pending LOWER delay drains at a fully-confirmed boundary
    // (no speculation in flight) without recapture; the frame-counted drain
    // timeout reverts to the proven old value (no wall clock, INV-16).
    if (delay_pending_valid_) {
        if (frameAtOrAfter(remote_actual_frontier_, sim_frontier_)) {
            delay_active_ = delay_pending_;
            delay_pending_valid_ = false;
        } else if (delay_drain_countdown_ == 0 || --delay_drain_countdown_ == 0) {
            delay_pending_valid_ = false;  // revert: keep the proven value
        }
    }

    has_captured_ = true;
    last_capture_source_ = source_frame;

    const uint32_t target = source_frame + delay_active_;
    if (frameBefore(target, produced_frontier_)) {
        // Repeat call while stalled, or pipeline surplus during a delay
        // drain: ADOPT the sealed record (INV-18) — never resample.
        return false;
    }
    // Fill any gap with neutral (only reachable if the caller skipped source
    // frames; the delay-raise path pre-fills, so this is a safety net).
    while (frameBefore(produced_frontier_, target)) {
        if (!SealLocal(produced_frontier_, config_.neutral_input)) return false;
    }
    return SealLocal(target, value);
}

bool RollbackEngine::RequestInputDelay(uint8_t new_delay) {
    if (!armed_ || terminal_ != EngineTerminal::None) return false;
    if (new_delay > 15) return false;
    if (delay_pending_valid_ && new_delay == delay_pending_) return true;
    if (!delay_pending_valid_ && new_delay == delay_active_) return true;

    if (new_delay > delay_active_) {
        // Raising fills only the missing future slots with neutral —
        // immediate, no recapture (§2.7.3-D).
        const uint8_t fill = (uint8_t)(new_delay - delay_active_);
        for (uint8_t i = 0; i < fill; ++i) {
            if (!SealLocal(produced_frontier_, config_.neutral_input)) return false;
        }
        delay_active_ = new_delay;
        delay_pending_valid_ = false;
        return true;
    }

    // Lowering: drain at a fully-confirmed boundary (checked per capture
    // opportunity), frame-counted timeout.
    delay_pending_ = new_delay;
    delay_pending_valid_ = true;
    delay_drain_countdown_ = ENGINE_DELAY_DRAIN_TIMEOUT_FRAMES;
    return true;
}

// ============================================================================
// Producer while stalled (§2.7.3-P, INV-24)
// ============================================================================

bool RollbackEngine::ProduceLocalInputAhead(uint16_t fresh_sample) {
    if (!armed_ || terminal_ != EngineTerminal::None) return false;
    if (producer_fenced_ || in_rollback_) return false;

    const uint32_t peer_capacity =
        (uint32_t)peer_adv_rollback_ + (uint32_t)peer_adv_delay_ + 2u;
    const uint32_t bound = peer_capacity < ENGINE_PRODUCER_HARD_CAP
                               ? peer_capacity
                               : ENGINE_PRODUCER_HARD_CAP;

    const int32_t lead_signed = signedLead(produced_frontier_, peer_ack_frontier_);
    const uint32_t lead = lead_signed > 0 ? (uint32_t)lead_signed : 0u;
    if (lead >= bound) {
        return false;
    }
    if (!SealLocal(produced_frontier_, (uint16_t)(fresh_sample & ENGINE_INPUT_VALID_MASK))) {
        return false;
    }
    ++stats_.producer_seals;
    // Structural invariant: the un-acked suffix always fits one packet
    // (produced − peer_ack ≤ 32; bound ≤ 30 guarantees it).
    if (signedLead(produced_frontier_, peer_ack_frontier_) > 32) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "producer suffix >32: produced=%u ack=%u",
                    produced_frontier_, peer_ack_frontier_);
        return false;
    }
    return true;
}

// ============================================================================
// Ingest (§2.7.3-X, INV-19)
// ============================================================================

void RollbackEngine::MarkMismatch(uint32_t frame) {
    // Pre-origin frames are DEAD (2026-08-17, run 21-12 rotation abort):
    // an epoch rotation at a match boundary can leave a short speculative
    // suffix from the OLD match below the new frame origin (observed:
    // origin 10823 minted with confirmed 10821). The peer's actuals for
    // those frames arrive late; a mismatch there would demand a restore
    // below the origin — into savestate slots the rotation just wiped
    // (fail-closed abort: "restore failed at frame 10821 (epoch 2)").
    // State identity changed at the origin (§2.6.5) and the old match's
    // archive was sealed at OnMatchEnd: the values can never affect the
    // new match, so the mismatch is absorbed, never corrected.
    if (frameBefore(frame, epoch_frame_origin_)) {
        return;
    }
    if (in_rollback_ && !frameBefore(frame, replay_cursor_)) {
        // The running replay will consume this actual itself.
        return;
    }
    if (!pending_mismatch_valid_ || frameBefore(frame, pending_mismatch_)) {
        pending_mismatch_ = frame;
        pending_mismatch_valid_ = true;
    }
}

void RollbackEngine::AdvanceRemoteFrontier() {
    while (remote_.Has(remote_actual_frontier_)) {
        ++remote_actual_frontier_;
    }
}

IngestResult RollbackEngine::ReceiveRemoteInput(uint32_t frame, uint16_t value) {
    if (!armed_ || terminal_ != EngineTerminal::None) return IngestResult::Stale;

    if (value == ENGINE_INPUT_INVALID_WORD ||
        (value & ~ENGINE_INPUT_VALID_MASK) != 0) {
        SetTerminal(EngineTerminal::InputInvalid,
                    "invalid remote input: frame=%u value=0x%04X", frame, value);
        return IngestResult::InvalidValue;
    }
    if (frameBefore(frame, config_.first_frame)) {
        return IngestResult::Stale;
    }
    if (frameAtOrAfter(frame, remote_actual_frontier_ + config_.max_remote_future)) {
        return IngestResult::TooFarFuture;
    }

    switch (remote_.Set(frame, value)) {
        case InputRing::SetResult::Occupied:
            return IngestResult::DuplicateIdentical;
        case InputRing::SetResult::Conflict:
            SetTerminal(EngineTerminal::InputConflict,
                        "remote input conflict: frame=%u stored=0x%04X got=0x%04X",
                        frame, remote_.Value(frame), value);
            return IngestResult::Conflict;
        case InputRing::SetResult::StaleSlot:
            return IngestResult::Stale;
        case InputRing::SetResult::Stored:
            break;
    }

    ++stats_.remote_inputs_applied;
    predictor_.OnActual(frame, value);

    // §2.7.3-R: a late actual that differs from the value an executed frame
    // consumed marks the EARLIEST mismatch. Matching predictions cost
    // nothing (retained speculation).
    if (frameBefore(frame, sim_frontier_)) {
        const ExecRecord* rec = ExecAt(frame);
        if (rec && rec->committed) {
            const int remote_slot = config_.local_player == 0 ? 1 : 0;
            if (rec->inputs[remote_slot] != value) {
                ++stats_.mispredictions;
                MarkMismatch(frame);
            } else if (rec->remote_predicted) {
                ++stats_.correct_predictions;
            }
        }
    }

    AdvanceRemoteFrontier();
    if (!in_rollback_) {
        AdvanceConfirmed();
    }
    return IngestResult::Applied;
}

bool RollbackEngine::IngestInputStream(const Net::InputStreamPayload& p) {
    if (!armed_ || terminal_ != EngineTerminal::None) return terminal_ == EngineTerminal::None;
    if (p.count < 1 || p.count > Net::INPUT_STREAM_MAX_INPUTS) {
        return true;  // malformed count: drop, never terminal (§2.3 forward compat)
    }

    // Ack re-anchoring: monotonic, wrap-safe. Input frames are canonical and
    // globally unique for the session (INV-15), so epoch does not gate them.
    const uint32_t ack_candidate = p.ack_through + 1u;
    if (frameAfter(ack_candidate, peer_ack_frontier_)) {
        peer_ack_frontier_ = ack_candidate;
    }
    SetPeerAdvisory(p.pressure);

    const uint32_t oldest = p.newest_frame - (uint32_t)(p.count - 1);
    for (uint8_t i = 0; i < p.count; ++i) {
        const IngestResult r = ReceiveRemoteInput(oldest + i, p.inputs[i]);
        if (r == IngestResult::Conflict || r == IngestResult::InvalidValue) {
            return false;
        }
    }
    return true;
}

void RollbackEngine::SetPeerAdvisory(const Net::PressureReport& pressure) {
    peer_adv_delay_ = pressure.adv_delay;
    peer_adv_rollback_ = pressure.adv_rollback;
    peer_prediction_depth_ = pressure.prediction_depth;
    peer_run_state_ = pressure.run_state;
    if (frameAfter(pressure.produced_through, peer_produced_frontier_)) {
        peer_produced_frontier_ = pressure.produced_through;
    }
}

// ============================================================================
// Per-opportunity decision (§2.7.4)
// ============================================================================

uint32_t RollbackEngine::SpeculativeFrames() const {
    const int32_t lead = signedLead(sim_frontier_, remote_actual_frontier_);
    return lead > 0 ? (uint32_t)lead : 0u;
}

int32_t RollbackEngine::FramesAheadSigned() const {
    return signedLead(produced_frontier_, peer_produced_frontier_);
}

EngineAction RollbackEngine::NextAction() {
    EngineAction action{};
    if (!armed_ || terminal_ != EngineTerminal::None || in_rollback_) {
        action.kind = EngineActionKind::Stall;
        action.hold_cause = HoldCause::None;
        ClassifyPass(WorkKind::Hold, HoldCause::None);
        return action;
    }

    // Corrections run FIRST on the opportunity that finds them queued.
    if (pending_mismatch_valid_) {
        action.kind = EngineActionKind::Rollback;
        action.frame = pending_mismatch_;
        action.replay_until = sim_frontier_;
        ClassifyPass(WorkKind::Correct, HoldCause::None);
        return action;
    }

    // Test-only forced rollback (SetForcedRollback): synthesize a depth-N
    // correction once per advanced frontier. Runs AFTER real corrections (a
    // genuine mismatch always wins the earliest-frame rule) and never
    // crosses the epoch frame origin — state identity changed there and
    // older snapshots are the director's to invalidate (§2.6.5). The
    // synthetic mismatch reuses the BeginRollback contract verbatim, so the
    // transaction is indistinguishable from a real one to the caller.
    if (forced_rollback_depth_ > 0 &&
        !(forced_rollback_done_valid_ && forced_rollback_done_ == sim_frontier_)) {
        const uint32_t executed = forwardDistance(epoch_frame_origin_, sim_frontier_);
        uint32_t depth = forced_rollback_depth_;
        if (depth > executed) depth = executed;
        if (depth > 0) {
            forced_rollback_done_ = sim_frontier_;
            forced_rollback_done_valid_ = true;
            pending_mismatch_ = sim_frontier_ - depth;
            pending_mismatch_valid_ = true;
            ++stats_.forced_transactions;  // unambiguous live evidence
            action.kind = EngineActionKind::Rollback;
            action.frame = pending_mismatch_;
            action.replay_until = sim_frontier_;
            ClassifyPass(WorkKind::Correct, HoldCause::None);
            return action;
        }
    }

    const bool remote_actual_here = remote_.Has(sim_frontier_);

    // INV-25: a predicted frame never crosses an exact-input window.
    if (lifecycle_exact_next_ && !remote_actual_here) {
        action.kind = EngineActionKind::Stall;
        action.hold_cause = HoldCause::LifecycleBoundary;
        ClassifyPass(WorkKind::Hold, action.hold_cause);
        return action;
    }

    // Our own capture is late — not a network wait.
    if (!local_.Has(sim_frontier_)) {
        action.kind = EngineActionKind::Stall;
        action.hold_cause = HoldCause::LocalInputMissing;
        ClassifyPass(WorkKind::Hold, action.hold_cause);
        return action;
    }

    // INV-1/INV-4: the ONLY network-driven hold, and the ONLY reader of
    // R_local in the entire engine. A hard local capacity fact — the
    // in-flight transit gap lives inside R by construction.
    if (SpeculativeFrames() >= config_.max_rollback) {
        action.kind = EngineActionKind::Stall;
        action.hold_cause = HoldCause::PredictionLimit;
        ClassifyPass(WorkKind::Hold, action.hold_cause);
        return action;
    }

    const int local_slot = config_.local_player == 0 ? 0 : 1;
    const int remote_slot = 1 - local_slot;
    bool predicted = false;

    action.kind = EngineActionKind::Advance;
    action.frame = sim_frontier_;
    action.inputs[local_slot] = local_.Value(sim_frontier_);
    action.inputs[remote_slot] = RemoteValueOrPredict(sim_frontier_, &predicted);
    action.remote_predicted = predicted;

    advance_plan_valid_ = true;
    advance_plan_frame_ = action.frame;
    advance_plan_inputs_[0] = action.inputs[0];
    advance_plan_inputs_[1] = action.inputs[1];
    advance_plan_predicted_ = predicted;

    ClassifyPass(WorkKind::Advance, HoldCause::None);
    return action;
}

bool RollbackEngine::CommitAdvance(uint32_t frame, uint64_t pre_state_hash,
                                   uint32_t rng_state, uint16_t hp0, uint16_t hp1) {
    if (!armed_ || terminal_ != EngineTerminal::None) return false;
    if (in_rollback_ || !advance_plan_valid_ || frame != advance_plan_frame_ ||
        frame != sim_frontier_) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "CommitAdvance without matching plan: frame=%u frontier=%u plan=%u/%d",
                    frame, sim_frontier_, advance_plan_frame_, advance_plan_valid_ ? 1 : 0);
        return false;
    }

    ExecRecord* rec = &exec_[frame % exec_.size()];
    if (rec->valid && rec->frame != frame &&
        !frameBefore(rec->frame, confirmed_pop_)) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "exec ring overflow: slot holds %u (pop=%u) for %u",
                    rec->frame, confirmed_pop_, frame);
        return false;
    }
    rec->valid = true;
    rec->frame = frame;
    rec->inputs[0] = advance_plan_inputs_[0];
    rec->inputs[1] = advance_plan_inputs_[1];
    rec->remote_predicted = advance_plan_predicted_;
    rec->pre_hash = pre_state_hash;
    rec->epoch = epoch_;
    rec->rng_state = rng_state;
    rec->hp0 = hp0;
    rec->hp1 = hp1;
    rec->committed = true;

    if (advance_plan_predicted_) {
        ++stats_.predictions_used;
    }
    advance_plan_valid_ = false;
    sim_frontier_ = frame + 1;
    AdvanceConfirmed();
    return true;
}

// ============================================================================
// Rollback transaction (§2.7.5)
// ============================================================================

bool RollbackEngine::BeginRollback(uint32_t from) {
    if (!armed_ || terminal_ != EngineTerminal::None) return false;
    if (in_rollback_ || !pending_mismatch_valid_ || from != pending_mismatch_ ||
        !frameBefore(from, sim_frontier_)) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "BeginRollback invalid: from=%u mismatch=%u/%d frontier=%u",
                    from, pending_mismatch_, pending_mismatch_valid_ ? 1 : 0,
                    sim_frontier_);
        return false;
    }
    in_rollback_ = true;
    replay_cursor_ = from;
    replay_target_ = sim_frontier_;
    replay_inputs_pending_ = false;
    pending_mismatch_valid_ = false;
    advance_plan_valid_ = false;

    const uint32_t depth = forwardDistance(from, replay_target_);
    ++stats_.rollbacks;
    stats_.last_rollback_from = from;
    stats_.last_rollback_length = depth;
    if (depth > stats_.max_rollback_depth) stats_.max_rollback_depth = depth;
    return true;
}

bool RollbackEngine::NextReplayInputs(uint32_t* frame, uint16_t inputs[2]) {
    if (!in_rollback_ || !frameBefore(replay_cursor_, replay_target_)) {
        return false;
    }
    const int local_slot = config_.local_player == 0 ? 0 : 1;
    const int remote_slot = 1 - local_slot;

    bool predicted = false;
    replay_pending_inputs_[local_slot] = local_.Value(replay_cursor_);
    replay_pending_inputs_[remote_slot] = RemoteValueOrPredict(replay_cursor_, &predicted);
    replay_pending_predicted_ = predicted;
    replay_inputs_pending_ = true;

    if (frame) *frame = replay_cursor_;
    if (inputs) {
        inputs[0] = replay_pending_inputs_[0];
        inputs[1] = replay_pending_inputs_[1];
    }
    return true;
}

bool RollbackEngine::CommitReplayFrame(uint32_t frame, uint64_t pre_state_hash,
                                       uint32_t rng_state, uint16_t hp0, uint16_t hp1) {
    if (!in_rollback_ || !replay_inputs_pending_ || frame != replay_cursor_) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "CommitReplayFrame invalid: frame=%u cursor=%u pending=%d",
                    frame, replay_cursor_, replay_inputs_pending_ ? 1 : 0);
        return false;
    }
    ExecRecord* rec = &exec_[frame % exec_.size()];

    // ── Local replay determinism self-test (QOH99 model) ───────────────────
    // Re-executing a frame from a restored snapshot with byte-identical
    // inputs must land on a byte-identical pre-state. When it doesn't, the
    // SIMULATION is nondeterministic under save/restore — and this catches it
    // on one machine, at the exact frame, without a peer and without waiting
    // for the 30-frame cross-peer digest to notice the consequences later.
    //
    // Only compares when the replay consumed exactly what the original
    // execution did: a real correction legitimately changes the state, and a
    // record from another epoch describes different state identity entirely.
    if (rec->valid && rec->committed && rec->frame == frame &&
        rec->epoch == epoch_ &&
        rec->inputs[0] == replay_pending_inputs_[0] &&
        rec->inputs[1] == replay_pending_inputs_[1]) {
        ++stats_.replay_verifications;
        if (rec->pre_hash != pre_state_hash) {
            ++stats_.replay_mismatches;
            stats_.last_replay_mismatch_frame = frame;
            stats_.last_replay_expect_hash = rec->pre_hash;
            stats_.last_replay_actual_hash = pre_state_hash;
        }
    }

    rec->valid = true;
    rec->frame = frame;
    rec->inputs[0] = replay_pending_inputs_[0];
    rec->inputs[1] = replay_pending_inputs_[1];
    rec->remote_predicted = replay_pending_predicted_;
    rec->pre_hash = pre_state_hash;
    rec->epoch = epoch_;
    rec->rng_state = rng_state;
    rec->hp0 = hp0;
    rec->hp1 = hp1;
    rec->committed = true;

    replay_inputs_pending_ = false;
    replay_cursor_ = frame + 1;
    return true;
}

bool RollbackEngine::FinishRollback() {
    if (!in_rollback_ || replay_cursor_ != replay_target_) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "FinishRollback before target: cursor=%u target=%u",
                    replay_cursor_, replay_target_);
        return false;
    }
    in_rollback_ = false;
    replay_inputs_pending_ = false;
    AdvanceConfirmed();
    return true;
}

bool RollbackEngine::FinishRollbackAtBoundary() {
    if (!in_rollback_) {
        SetTerminal(EngineTerminal::InternalInvariant, "FinishRollbackAtBoundary outside txn");
        return false;
    }
    // Discard the speculative suffix [cursor, target): those frames'
    // exec records are invalid now; the frontier truncates to the cursor.
    for (uint32_t f = replay_cursor_; frameBefore(f, replay_target_); ++f) {
        ExecRecord* rec = &exec_[f % exec_.size()];
        if (rec->valid && rec->frame == f) {
            rec->valid = false;
        }
    }
    sim_frontier_ = replay_cursor_;
    in_rollback_ = false;
    replay_inputs_pending_ = false;
    advance_plan_valid_ = false;
    // A mismatch marked beyond the truncated frontier is moot.
    if (pending_mismatch_valid_ && !frameBefore(pending_mismatch_, sim_frontier_)) {
        pending_mismatch_valid_ = false;
    }
    AdvanceConfirmed();
    return true;
}

bool RollbackEngine::FinishRollbackBeforeBoundary() {
    // Same truncation semantics: the state sits at the replay cursor and
    // everything at/after it is discarded speculation (§2.7.5).
    return FinishRollbackAtBoundary();
}

// ============================================================================
// Confirm pipeline (§2.7.3-F)
// ============================================================================

void RollbackEngine::AdvanceConfirmed() {
    while (true) {
        const uint32_t f = confirmed_frontier_;
        if (!frameBefore(f, sim_frontier_)) break;            // must be executed
        if (!frameBefore(f, remote_actual_frontier_)) break;  // must be actual
        if (pending_mismatch_valid_ && !frameBefore(f, pending_mismatch_)) break;
        const ExecRecord* rec = ExecAt(f);
        if (!rec || !rec->committed) break;

        confirmed_frontier_ = f + 1;
        ++confirm_count_;
        ++stats_.confirmed_frames;

        // SyncHash cadence: every 30th confirmed frame exactly (§2.7.7).
        if (confirm_count_ % ENGINE_SYNC_HASH_INTERVAL == 0) {
            const uint32_t cadence_idx =
                (confirm_count_ / ENGINE_SYNC_HASH_INTERVAL) % (uint32_t)local_hashes_.size();
            HashRecord& lh = local_hashes_[cadence_idx];
            lh.valid = true;
            lh.epoch = rec->epoch;
            lh.frame = f;
            lh.hash = rec->pre_hash;
            lh.rng_state = rec->rng_state;
            lh.hp0 = rec->hp0;
            lh.hp1 = rec->hp1;

            OutgoingSyncHash out{};
            out.epoch = rec->epoch;
            out.frame = f;
            out.gameplay_hash = rec->pre_hash;
            out.rng_state = rec->rng_state;
            out.hp0 = rec->hp0;
            out.hp1 = rec->hp1;
            const uint32_t cap = (uint32_t)outgoing_hashes_.size();
            if (outgoing_hash_count_ == cap) {
                // Sender lag: drop the oldest (bounded, never blocks).
                outgoing_hash_head_ = (outgoing_hash_head_ + 1) % cap;
                --outgoing_hash_count_;
            }
            outgoing_hashes_[(outgoing_hash_head_ + outgoing_hash_count_) % cap] = out;
            ++outgoing_hash_count_;
            ++stats_.sync_hashes_sent;
        }
    }
}

bool RollbackEngine::PopConfirmedFrame(ConfirmedFrame* out) {
    if (!frameBefore(confirmed_pop_, confirmed_frontier_)) return false;
    const ExecRecord* rec = ExecAt(confirmed_pop_);
    if (!rec) {
        SetTerminal(EngineTerminal::InternalInvariant,
                    "confirmed record missing at pop=%u", confirmed_pop_);
        return false;
    }
    if (out) {
        out->frame = rec->frame;
        out->inputs[0] = rec->inputs[0];
        out->inputs[1] = rec->inputs[1];
        out->pre_state_hash = rec->pre_hash;
        out->epoch = rec->epoch;
        out->rng_state = rec->rng_state;
        out->hp0 = rec->hp0;
        out->hp1 = rec->hp1;
    }
    ++confirmed_pop_;
    return true;
}

// ============================================================================
// SyncHash (§2.7.7)
// ============================================================================

bool RollbackEngine::PopOutgoingSyncHash(OutgoingSyncHash* out) {
    if (outgoing_hash_count_ == 0) return false;
    if (out) *out = outgoing_hashes_[outgoing_hash_head_];
    outgoing_hash_head_ = (outgoing_hash_head_ + 1) % (uint32_t)outgoing_hashes_.size();
    --outgoing_hash_count_;
    return true;
}

HashVerify RollbackEngine::ReceiveSyncHash(const Net::SyncHashPayload& p) {
    if (!armed_) return HashVerify::NoneReady;

    // Epoch relations (§2.7.7): stale -> ignore; future -> queue;
    // inconsistent lineage -> protocol-violation terminal.
    if (p.epoch != epoch_ && frameBefore(p.epoch, epoch_)) {
        return HashVerify::StaleEpoch;
    }

    if (p.epoch == epoch_ && frameBefore(p.frame, confirmed_frontier_)) {
        // Comparable now.
        HashRecord* lh = FindLocalHash(p.frame);
        if (!lh) {
            // Cadence record already rotated out — too old to compare.
            return HashVerify::StaleEpoch;
        }
        if (lh->epoch != p.epoch) {
            SetTerminal(EngineTerminal::EpochInconsistent,
                        "hash epoch lineage: frame=%u local_epoch=%u peer_epoch=%u",
                        p.frame, lh->epoch, p.epoch);
            return HashVerify::InconsistentEpoch;
        }
        // Comparison exact across all fields; gameplay_hash authoritative,
        // rng/hp are the first diagnostic to read from the dump.
        if (lh->hash != p.gameplay_hash || lh->rng_state != p.rng_state ||
            lh->hp0 != p.hp0 || lh->hp1 != p.hp1) {
            // Capture the failing pair — every field, both sides — for the
            // adapter's evidence dump (diagnostics only; nothing in the
            // decision path reads it).
            desync_evidence_.valid = true;
            desync_evidence_.frame = p.frame;
            desync_evidence_.epoch = p.epoch;
            desync_evidence_.local_hash = lh->hash;
            desync_evidence_.peer_hash = p.gameplay_hash;
            desync_evidence_.local_rng = lh->rng_state;
            desync_evidence_.peer_rng = p.rng_state;
            desync_evidence_.local_hp0 = lh->hp0;
            desync_evidence_.local_hp1 = lh->hp1;
            desync_evidence_.peer_hp0 = p.hp0;
            desync_evidence_.peer_hp1 = p.hp1;
            SetTerminal(EngineTerminal::ConfirmedDesync,
                        "sync hash mismatch: frame=%u local=%016llx peer=%016llx "
                        "rng=%08x/%08x hp=%u,%u/%u,%u",
                        p.frame,
                        (unsigned long long)lh->hash,
                        (unsigned long long)p.gameplay_hash,
                        lh->rng_state, p.rng_state,
                        lh->hp0, lh->hp1, p.hp0, p.hp1);
            return HashVerify::Mismatch;
        }
        ++stats_.sync_hashes_verified;
        return HashVerify::Ok;
    }

    // Future frame or future epoch: queue, bounded (D-3).
    if (peer_hash_count_ >= (uint32_t)peer_hash_queue_.size()) {
        SetTerminal(EngineTerminal::HashQueueOverflow,
                    "peer hash queue overflow at frame=%u epoch=%u", p.frame, p.epoch);
        return HashVerify::InconsistentEpoch;
    }
    for (PeerHashRecord& rec : peer_hash_queue_) {
        if (rec.valid) continue;
        rec.valid = true;
        rec.epoch = p.epoch;
        rec.frame = p.frame;
        rec.hash = p.gameplay_hash;
        rec.rng_state = p.rng_state;
        rec.hp0 = p.hp0;
        rec.hp1 = p.hp1;
        ++peer_hash_count_;
        break;
    }
    return HashVerify::QueuedFuture;
}

HashVerify RollbackEngine::PumpSyncHashVerify() {
    HashVerify worst = HashVerify::NoneReady;
    for (PeerHashRecord& rec : peer_hash_queue_) {
        if (!rec.valid) continue;
        const bool stale_epoch = rec.epoch != epoch_ && frameBefore(rec.epoch, epoch_);
        const bool comparable =
            rec.epoch == epoch_ && frameBefore(rec.frame, confirmed_frontier_);
        if (!stale_epoch && !comparable) continue;

        rec.valid = false;
        --peer_hash_count_;
        if (stale_epoch) continue;

        Net::SyncHashPayload p{};
        p.epoch = rec.epoch;
        p.frame = rec.frame;
        p.gameplay_hash = rec.hash;
        p.rng_state = rec.rng_state;
        p.hp0 = rec.hp0;
        p.hp1 = rec.hp1;
        const HashVerify r = ReceiveSyncHash(p);
        if (r == HashVerify::Mismatch || r == HashVerify::InconsistentEpoch) {
            return r;
        }
        if (r == HashVerify::Ok) worst = HashVerify::Ok;
    }
    return worst;
}

// ============================================================================
// Wire building (§3.2)
// ============================================================================

bool RollbackEngine::HasUnackedSuffix() const {
    return frameBefore(peer_ack_frontier_, produced_frontier_);
}

bool RollbackEngine::BuildInputStream(Net::InputStreamPayload* out) {
    if (!armed_ || !out) return false;
    if (produced_frontier_ == config_.first_frame) return false;  // nothing sealed

    const uint32_t newest = produced_frontier_ - 1u;

    // Window anchored at peer_ack_through+1: exactly the un-acked suffix,
    // clamped to the 32-slot window and the first sealed frame.
    uint32_t oldest = peer_ack_frontier_;
    if (frameAfter(oldest, newest)) {
        oldest = newest;  // fully acked: redundant newest only
    }
    const uint32_t window_floor = newest - (Net::INPUT_STREAM_MAX_INPUTS - 1u);
    if (frameBefore(oldest, window_floor)) {
        oldest = window_floor;
    }
    if (frameBefore(oldest, config_.first_frame)) {
        oldest = config_.first_frame;
    }

    memset(out, 0, sizeof(*out));
    out->session_id = 0;  // adapter stamps the handshake identity
    out->epoch = epoch_;
    out->newest_frame = newest;
    out->ack_through = remote_actual_frontier_ - 1u;
    out->count = (uint8_t)(forwardDistance(oldest, newest) + 1u);
    for (uint8_t i = 0; i < out->count; ++i) {
        out->inputs[i] = local_.Value(oldest + i);
    }
    FillPressureReport(&out->pressure);
    return true;
}

void RollbackEngine::FillPressureReport(Net::PressureReport* out) const {
    if (!out) return;
    out->produced_through = produced_frontier_;
    out->confirmed_frontier = confirmed_frontier_;
    const uint32_t depth = SpeculativeFrames();
    out->prediction_depth = depth > 255u ? (uint8_t)255u : (uint8_t)depth;
    out->run_state = (uint8_t)last_run_state_;
    out->adv_delay = delay_active_;
    out->adv_rollback = config_.max_rollback;
}

} // namespace Rollback
