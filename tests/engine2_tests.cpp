// re0.7 M4 — engine2 core unit tests (plan §7.1 T-ENG-1..12), the
// deterministic socket-free soak harness (§7.2 model: scripted inputs,
// virtual clock, loss/jitter/reorder shims — no sockets), and the
// savestate-cost microbench (§7.5 criterion 4).
//
// The RollbackEngine under test is the identical object the game adapter
// (rollback_session_engine2.cpp) drives — the QOH99 method: the core owns no
// sockets, no clock, no game memory, so the harness below IS a full peer.
//
// T-ENG-9 note: the "no other code path reads R" half is pinned behaviorally
// here (exactly R speculative advances before the first PredictionLimit
// stall); the link-time/grep half is a review-gate item (the sole reader is
// RollbackEngine::NextAction, marked INV-4 in engine2.cpp).

#include "rollback/engine2.h"
#include "rollback/block_digest.h"
#include "rollback/lifecycle_window.h"
#include "rollback/game_snapshot.h"   // struct sizes only (microbench)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

using namespace Rollback;

namespace {

int g_checks = 0;
int g_failures = 0;

#define TEST_CHECK(cond, msg)                                             \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
        }                                                                 \
    } while (0)

// Deterministic PRNG (xorshift32) — the harness virtual "network" and the
// scripted input source. No wall clock anywhere.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t Next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
};

uint16_t ScriptedInput(Rng& rng) {
    // Sparse-ish button mask inside the valid AS2 mask.
    return (uint16_t)(rng.Next() & 0x001F);
}

EngineConfig MakeConfig(uint8_t player, uint8_t delay, uint8_t rollback,
                        uint32_t first_frame = 0) {
    EngineConfig c{};
    c.local_player = player;
    c.input_delay = delay;
    c.max_rollback = rollback;
    c.neutral_input = 0x0000;
    c.first_frame = first_frame;
    c.max_remote_future = 120;
    c.history_capacity = 4096;
    return c;
}

// ── Toy deterministic sim + peer harness ────────────────────────────────────
//
// The "game" is a u64 state evolved by Block64 over the canonical input
// pair. pre_state_hash = the state value before the tick — exactly the
// adapter's save→simulate→commit shape.

struct Peer {
    RollbackEngine eng;
    uint64_t state = 0x1234567812345678ull;
    std::map<uint32_t, uint64_t> snapshots;   // frame -> pre-tick state
    std::vector<ConfirmedFrame> confirmed;
    uint32_t advances = 0;
    uint32_t stalls_pred = 0;
    uint32_t stalls_life = 0;
    uint32_t stalls_input = 0;

    void Tick(uint16_t p1, uint16_t p2) {
        uint16_t in[2] = {p1, p2};
        state = Block64(in, sizeof(in), state);
    }

    // One scheduler pass: capture, correct if queued, then at most one
    // visible advance; producer feeds on stalls (INV-24).
    void StepPass(uint16_t local_input) {
        if (eng.Terminal() != EngineTerminal::None) return;
        eng.CaptureLocalInput(eng.SimFrontier(), local_input);

        for (int guard = 0; guard < 64; ++guard) {
            const EngineAction a = eng.NextAction();
            if (a.kind == EngineActionKind::Rollback) {
                if (!eng.BeginRollback(a.frame)) return;
                auto it = snapshots.find(a.frame);
                if (it == snapshots.end()) {
                    std::printf("HARNESS: no snapshot at %u\n", a.frame);
                    ++g_failures;
                    return;
                }
                state = it->second;
                uint32_t f = 0;
                uint16_t in[2] = {0, 0};
                while (eng.NextReplayInputs(&f, in)) {
                    snapshots[f] = state;
                    if (!eng.CommitReplayFrame(f, state)) return;
                    Tick(in[0], in[1]);
                }
                if (!eng.FinishRollback()) return;
                continue;  // corrections first; may still advance this pass
            }
            if (a.kind == EngineActionKind::Advance) {
                snapshots[a.frame] = state;
                if (!eng.CommitAdvance(a.frame, state)) return;
                Tick(a.inputs[0], a.inputs[1]);
                ++advances;
            } else {
                switch (a.hold_cause) {
                    case HoldCause::PredictionLimit:  ++stalls_pred; break;
                    case HoldCause::LifecycleBoundary: ++stalls_life; break;
                    case HoldCause::LocalInputMissing: ++stalls_input; break;
                    default: break;
                }
                eng.ProduceLocalInputAhead(local_input);
            }
            break;  // one visible opportunity per pass
        }

        ConfirmedFrame cf{};
        while (eng.PopConfirmedFrame(&cf)) {
            confirmed.push_back(cf);
        }
    }
};

// Virtual lossy/jittery/reordering link measured in whole frame periods.
struct Link {
    struct Packet {
        Net::InputStreamPayload payload;
        uint32_t deliver_at;
    };
    std::vector<Packet> in_flight;
    Rng rng;
    uint32_t loss_pct;        // 0..100
    uint32_t base_delay;      // frames
    uint32_t jitter_extra;    // uniform [0, jitter_extra] frames

    Link(uint32_t seed, uint32_t loss, uint32_t base, uint32_t jitter)
        : rng(seed), loss_pct(loss), base_delay(base), jitter_extra(jitter) {}

    void Send(const Net::InputStreamPayload& p, uint32_t now) {
        if (rng.Below(100) < loss_pct) return;   // loss shim
        Packet pkt{};
        pkt.payload = p;
        pkt.deliver_at = now + base_delay + rng.Below(jitter_extra + 1);
        in_flight.push_back(pkt);                 // arrival order = reorder shim
    }

    void DeliverDue(uint32_t now, RollbackEngine& dst) {
        for (size_t i = 0; i < in_flight.size();) {
            if ((int32_t)(now - in_flight[i].deliver_at) >= 0) {
                dst.IngestInputStream(in_flight[i].payload);
                in_flight[i] = in_flight.back();
                in_flight.pop_back();             // random order pop = reorder
            } else {
                ++i;
            }
        }
    }
};

// Reliable in-order hash channel (control ch0 model).
void ExchangeSyncHashes(Peer& from, Peer& to, uint32_t* mismatches) {
    OutgoingSyncHash h{};
    while (from.eng.PopOutgoingSyncHash(&h)) {
        Net::SyncHashPayload p{};
        p.epoch = h.epoch;
        p.frame = h.frame;
        p.gameplay_hash = h.gameplay_hash;
        p.rng_state = h.rng_state;
        p.hp0 = h.hp0;
        p.hp1 = h.hp1;
        const HashVerify v = to.eng.ReceiveSyncHash(p);
        if (v == HashVerify::Mismatch || v == HashVerify::InconsistentEpoch) {
            ++*mismatches;
        }
    }
    const HashVerify v = to.eng.PumpSyncHashVerify();
    if (v == HashVerify::Mismatch || v == HashVerify::InconsistentEpoch) {
        ++*mismatches;
    }
}

// ── T-ENG-1: capture-once immutability ──────────────────────────────────────

void TestCaptureOnce() {
    RollbackEngine e;
    TEST_CHECK(e.Arm(MakeConfig(0, 2, 8), 1), "arm ok");

    // First capture seals source+delay.
    TEST_CHECK(e.CaptureLocalInput(0, 0x0010), "first capture seals");
    // Repeat capture for the same source frame (stall) ADOPTS, never resamples.
    TEST_CHECK(!e.CaptureLocalInput(0, 0x0020), "repeat capture adopts");
    TEST_CHECK(e.ProducedFrontier() == 3, "prefix(2) + 1 sealed");

    // The sealed value is the FIRST sample — slot immutable (INV-18).
    Net::InputStreamPayload p{};
    TEST_CHECK(e.BuildInputStream(&p), "stream builds");
    TEST_CHECK(p.newest_frame == 2 && p.count == 3, "window covers prefix+seal");
    TEST_CHECK(p.inputs[2] == 0x0010, "sealed value is the first sample");
    TEST_CHECK(p.inputs[0] == 0x0000 && p.inputs[1] == 0x0000, "delay prefix neutral");
}

// ── T-ENG-2: delay relabel ──────────────────────────────────────────────────

void TestDelayRelabel() {
    // Raise: fills only missing future slots with neutral, immediate.
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 2, 8), 1);
        e.CaptureLocalInput(0, 0x0001);          // seals frame 2
        TEST_CHECK(e.RequestInputDelay(4), "raise accepted");
        TEST_CHECK(e.ActiveDelay() == 4, "raise immediate");
        TEST_CHECK(e.ProducedFrontier() == 5, "raise sealed 2 neutral fill frames");
        // Next capture (source 1) targets 1+4=5 == produced -> seals.
        TEST_CHECK(e.CaptureLocalInput(1, 0x0002), "post-raise capture seals");
        Net::InputStreamPayload p{};
        e.BuildInputStream(&p);
        TEST_CHECK(p.inputs[3] == 0x0000 && p.inputs[4] == 0x0000,
                   "fill frames are neutral (no recapture)");
        TEST_CHECK(p.inputs[5] == 0x0002, "post-raise capture lands at source+4");
    }
    // Lower: drains at a fully-confirmed boundary without recapture.
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 4, 8), 1);
        // Fully confirmed: no frames executed yet, remote at frontier.
        TEST_CHECK(e.RequestInputDelay(2), "lower accepted (pending)");
        TEST_CHECK(e.DelayChangePending(), "lower pends until a capture sees the boundary");
        e.CaptureLocalInput(0, 0x0003);          // boundary holds -> switch
        TEST_CHECK(!e.DelayChangePending() && e.ActiveDelay() == 2,
                   "lower drained at fully-confirmed boundary");
        // Pipeline surplus (prefix 4 deep, mapping now +2): captures adopt
        // until the pipeline shrinks — no recapture of sealed slots.
        TEST_CHECK(!e.CaptureLocalInput(1, 0x0004), "surplus adopts (drain)");
    }
    // Revert: drain never reaches a boundary -> proven old value restored.
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 4, 8), 1);
        // Execute frames on pure prediction so speculation stays nonzero.
        for (int i = 0; i < 3; ++i) {
            const EngineAction a = e.NextAction();
            TEST_CHECK(a.kind == EngineActionKind::Advance, "predictive advance");
            e.CommitAdvance(a.frame, 0);
        }
        TEST_CHECK(e.RequestInputDelay(2), "lower accepted");
        for (uint32_t i = 0; i < ENGINE_DELAY_DRAIN_TIMEOUT_FRAMES + 2; ++i) {
            e.CaptureLocalInput(e.SimFrontier(), 0x0001);
        }
        TEST_CHECK(!e.DelayChangePending(), "drain timeout cleared the request");
        TEST_CHECK(e.ActiveDelay() == 4, "reverted to the proven old value");
        TEST_CHECK(e.Terminal() == EngineTerminal::None, "revert is not a fault");
    }
}

// ── T-ENG-3: hold-last predict + earliest mismatch ──────────────────────────

void TestPredictMismatch() {
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);

    // Remote actual at frame 0 = 0x0008; frames 1.. predicted hold-last.
    TEST_CHECK(e.ReceiveRemoteInput(0, 0x0008) == IngestResult::Applied, "actual 0");
    for (uint32_t f = 0; f < 5; ++f) {
        e.CaptureLocalInput(e.SimFrontier(), 0x0001);
        const EngineAction a = e.NextAction();
        TEST_CHECK(a.kind == EngineActionKind::Advance, "advance");
        if (f >= 1) {
            TEST_CHECK(a.remote_predicted && a.inputs[1] == 0x0008,
                       "hold-last-actual prediction");
        }
        e.CommitAdvance(a.frame, 0);
    }

    // Matching actual for frame 1 -> retained speculation, no rollback.
    TEST_CHECK(e.ReceiveRemoteInput(1, 0x0008) == IngestResult::Applied, "actual 1");
    TEST_CHECK(!e.HasPendingMismatch(), "matching prediction costs nothing");

    // Out-of-order differing actuals: frame 4 first, then frame 2 —
    // earliest mismatch must win.
    TEST_CHECK(e.ReceiveRemoteInput(4, 0x0004) == IngestResult::Applied, "actual 4");
    TEST_CHECK(e.HasPendingMismatch() && e.PendingMismatchFrame() == 4, "mismatch at 4");
    TEST_CHECK(e.ReceiveRemoteInput(2, 0x0002) == IngestResult::Applied, "actual 2");
    TEST_CHECK(e.PendingMismatchFrame() == 2, "earliest mismatch selected");

    const EngineAction a = e.NextAction();
    TEST_CHECK(a.kind == EngineActionKind::Rollback && a.frame == 2 &&
               a.replay_until == 5, "rollback from earliest mismatch to frontier");
}

// ── T-ENG-4: rollback transaction ───────────────────────────────────────────

void TestRollbackTransaction() {
    // Full begin/commit/finish through the harness.
    {
        Peer p;
        p.eng.Arm(MakeConfig(0, 0, 8), 1);
        Rng rng(7);
        for (int i = 0; i < 6; ++i) p.StepPass(ScriptedInput(rng));
        const uint64_t stats_before = p.eng.GetStats().rollbacks;
        // Differing actual at frame 1 forces a correction.
        p.eng.ReceiveRemoteInput(1, 0x0111);
        p.StepPass(0x0001);
        TEST_CHECK(p.eng.GetStats().rollbacks == stats_before + 1, "one rollback ran");
        TEST_CHECK(!p.eng.InRollback(), "transaction closed");
        TEST_CHECK(p.eng.Terminal() == EngineTerminal::None, "clean");
    }
    // Boundary truncation: FinishRollbackAtBoundary discards the suffix.
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8), 1);
        for (uint32_t f = 0; f < 5; ++f) {
            e.CaptureLocalInput(e.SimFrontier(), 0x0001);
            const EngineAction a = e.NextAction();
            e.CommitAdvance(a.frame, 100 + f);
        }
        e.ReceiveRemoteInput(1, 0x0208);   // mismatch at 1
        const EngineAction a = e.NextAction();
        TEST_CHECK(a.kind == EngineActionKind::Rollback && a.frame == 1, "rb from 1");
        TEST_CHECK(e.BeginRollback(1), "begin");
        uint32_t f = 0; uint16_t in[2];
        TEST_CHECK(e.NextReplayInputs(&f, in) && f == 1, "replay 1");
        TEST_CHECK(e.CommitReplayFrame(1, 201), "commit 1");
        TEST_CHECK(e.NextReplayInputs(&f, in) && f == 2, "replay 2");
        TEST_CHECK(e.CommitReplayFrame(2, 202), "commit 2");
        // Replay hit a native boundary earlier than speculation did.
        TEST_CHECK(e.FinishRollbackAtBoundary(), "truncate at boundary");
        TEST_CHECK(e.SimFrontier() == 3, "frontier truncated to replay cursor");
        TEST_CHECK(!e.InRollback(), "closed");
        // The discarded frames re-run later with the SAME sealed local inputs.
        const EngineAction b = e.NextAction();
        TEST_CHECK(b.kind == EngineActionKind::Advance && b.frame == 3,
                   "discarded frame re-advances");
    }
    // Capture during the transaction is an invariant violation.
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8), 1);
        for (uint32_t f = 0; f < 3; ++f) {
            e.CaptureLocalInput(e.SimFrontier(), 0x0001);
            const EngineAction a = e.NextAction();
            e.CommitAdvance(a.frame, 0);
        }
        e.ReceiveRemoteInput(0, 0x0100);
        TEST_CHECK(e.BeginRollback(0), "begin");
        e.CaptureLocalInput(e.SimFrontier(), 0x0002);
        TEST_CHECK(e.Terminal() == EngineTerminal::InternalInvariant,
                   "capture inside transaction is fail-closed");
    }
}

// ── T-ENG-5: confirm ordering ───────────────────────────────────────────────

void TestConfirmOrdering() {
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 4), 1);

    for (uint32_t f = 0; f < 4; ++f) {
        e.CaptureLocalInput(e.SimFrontier(), (uint16_t)(f + 1));
        const EngineAction a = e.NextAction();
        e.CommitAdvance(a.frame, 1000 + f);
    }
    ConfirmedFrame cf{};
    TEST_CHECK(!e.PopConfirmedFrame(&cf), "predicted values never pop");

    // Actuals 0..2 arrive: exactly frames 0..2 confirm, in order, with the
    // recorded pre-hash.
    for (uint32_t f = 0; f < 3; ++f) e.ReceiveRemoteInput(f, 0x0000);
    for (uint32_t f = 0; f < 3; ++f) {
        TEST_CHECK(e.PopConfirmedFrame(&cf), "confirm pops");
        TEST_CHECK(cf.frame == f, "in order");
        TEST_CHECK(cf.pre_state_hash == 1000 + f, "pre-hash carried");
        TEST_CHECK(cf.inputs[0] == (uint16_t)(f + 1), "local input carried");
    }
    TEST_CHECK(!e.PopConfirmedFrame(&cf), "frame 3 not confirmable yet");
}

// ── T-ENG-6: ingest taxonomy ────────────────────────────────────────────────

void TestIngestTaxonomy() {
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8), 1);
        TEST_CHECK(e.ReceiveRemoteInput(0, 0x0010) == IngestResult::Applied, "applied");
        TEST_CHECK(e.ReceiveRemoteInput(0, 0x0010) == IngestResult::DuplicateIdentical,
                   "duplicate identical ignored");
        TEST_CHECK(e.ReceiveRemoteInput(200, 0x0001) == IngestResult::TooFarFuture,
                   "beyond frontier+120 dropped");
        TEST_CHECK(e.Terminal() == EngineTerminal::None, "no terminal yet");
        TEST_CHECK(e.ReceiveRemoteInput(0, 0x0020) == IngestResult::Conflict,
                   "conflict detected");
        TEST_CHECK(e.Terminal() == EngineTerminal::InputConflict,
                   "conflict is terminal (INV-19)");
    }
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8), 1);
        TEST_CHECK(e.ReceiveRemoteInput(0, 0xFFFF) == IngestResult::InvalidValue,
                   "0xFFFF is the game's no-input marker: invalid");
        TEST_CHECK(e.Terminal() == EngineTerminal::InputInvalid, "invalid is terminal");
    }
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8), 1);
        TEST_CHECK(e.ReceiveRemoteInput(0, 0x4000) == IngestResult::InvalidValue,
                   "out-of-mask bits invalid");
    }
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8, /*first=*/1000), 1);
        TEST_CHECK(e.ReceiveRemoteInput(500, 0x0001) == IngestResult::Stale,
                   "before first_frame is stale");
        TEST_CHECK(e.Terminal() == EngineTerminal::None, "stale is benign");
    }
}

// ── T-ENG-7: window math (ack anchoring, hole refill) ───────────────────────

void TestWindowMath() {
    RollbackEngine a, b;
    a.Arm(MakeConfig(0, 0, 8), 1);
    b.Arm(MakeConfig(1, 0, 8), 1);

    // Give A a generous producer bound via peer advisory.
    Net::PressureReport adv{};
    adv.adv_delay = 13;
    adv.adv_rollback = 15;
    a.SetPeerAdvisory(adv);

    // Seal 30 producer inputs while "stalled" — bound holds the suffix ≤ 30.
    int sealed = 0;
    for (int i = 0; i < 40; ++i) {
        if (a.ProduceLocalInputAhead((uint16_t)(i & 0x000F))) ++sealed;
    }
    TEST_CHECK(sealed == 30, "producer bound min(peerR+peerD+2, 30) = 30");
    TEST_CHECK(a.Terminal() == EngineTerminal::None, "suffix ≤ 32 invariant holds");

    // The whole un-acked suffix fits one packet: a single delivery refills
    // every hole on the receiver.
    Net::InputStreamPayload p{};
    TEST_CHECK(a.BuildInputStream(&p), "stream builds");
    TEST_CHECK(p.count == 30 && p.newest_frame == 29, "window = exact suffix");
    TEST_CHECK(b.IngestInputStream(p), "ingest ok");
    TEST_CHECK(b.RemoteActualFrontier() == 30, "hole refill from a single packet");

    // Ack re-anchoring: B's reply carries ack_through = 29; A's next window
    // re-anchors at 30 (only the new suffix).
    Net::InputStreamPayload q{};
    b.CaptureLocalInput(0, 0x0001);
    TEST_CHECK(b.BuildInputStream(&q), "b stream builds");
    TEST_CHECK(q.ack_through == 29, "ack rides every stream packet");
    TEST_CHECK(a.IngestInputStream(q), "a ingests ack");
    TEST_CHECK(a.PeerAckFrontier() == 30, "ack frontier re-anchored");
    a.CaptureLocalInput(a.SimFrontier(), 0x0002);  // adopts (produced ahead)
    Net::InputStreamPayload r{};
    TEST_CHECK(a.BuildInputStream(&r), "a stream rebuilds");
    TEST_CHECK(r.count == 1 && r.newest_frame == 29,
               "fully-acked window degenerates to redundant newest");
}

// ── T-ENG-8: producer bound + fencing ───────────────────────────────────────

void TestProducer() {
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);

    // Default advisory (0,0): bound = 2.
    TEST_CHECK(e.ProduceLocalInputAhead(0x0001), "seal 1");
    TEST_CHECK(e.ProduceLocalInputAhead(0x0001), "seal 2");
    TEST_CHECK(!e.ProduceLocalInputAhead(0x0001), "bound 0+0+2 reached");

    Net::PressureReport adv{};
    adv.adv_delay = 1;
    adv.adv_rollback = 2;
    e.SetPeerAdvisory(adv);
    int extra = 0;
    while (e.ProduceLocalInputAhead(0x0002)) ++extra;
    TEST_CHECK(extra == 3, "bound follows peer capacity (2+1+2 = 5 total)");

    e.SetProducerFenced(true);
    Net::PressureReport wide{};
    wide.adv_delay = 15;
    wide.adv_rollback = 15;
    e.SetPeerAdvisory(wide);
    TEST_CHECK(!e.ProduceLocalInputAhead(0x0003), "fenced producer seals nothing");
    e.SetProducerFenced(false);
    TEST_CHECK(e.ProduceLocalInputAhead(0x0003), "unfenced resumes");
}

// ── T-ENG-9: INV-4 pin ──────────────────────────────────────────────────────

void TestPredictionLimitPin() {
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);

    // No remote input ever arrives: exactly R=8 speculative frames advance
    // before the FIRST Stall(PredictionLimit) — never a 9th.
    uint32_t advances = 0;
    for (int i = 0; i < 20; ++i) {
        e.CaptureLocalInput(e.SimFrontier(), 0x0001);
        const EngineAction a = e.NextAction();
        if (a.kind == EngineActionKind::Advance) {
            e.CommitAdvance(a.frame, 0);
            ++advances;
        } else {
            TEST_CHECK(a.hold_cause == HoldCause::PredictionLimit,
                       "the hold is PredictionLimit");
            break;
        }
    }
    TEST_CHECK(advances == 8, "exactly R speculative frames before the stall");
    TEST_CHECK(e.SpeculativeFrames() == 8, "depth equals R at the ceiling");

    // One actual releases exactly one more frame.
    e.ReceiveRemoteInput(0, 0x0000);
    const EngineAction a = e.NextAction();
    TEST_CHECK(a.kind == EngineActionKind::Advance, "released by one actual");
}

// ── T-ENG-10: wrap safety ───────────────────────────────────────────────────

void TestWrapSafety() {
    const uint32_t first = 0xFFFFFF00u;
    Peer a, b;
    a.eng.Arm(MakeConfig(0, 1, 6, first), 1);
    b.eng.Arm(MakeConfig(1, 1, 6, first), 1);
    Rng inputs(42);
    uint32_t hash_mismatches = 0;

    for (int frame = 0; frame < 2000; ++frame) {
        const uint16_t ia = ScriptedInput(inputs);
        const uint16_t ib = ScriptedInput(inputs);
        a.StepPass(ia);
        b.StepPass(ib);
        // Lossless immediate link.
        Net::InputStreamPayload p{};
        if (a.eng.BuildInputStream(&p)) b.eng.IngestInputStream(p);
        if (b.eng.BuildInputStream(&p)) a.eng.IngestInputStream(p);
        ExchangeSyncHashes(a, b, &hash_mismatches);
        ExchangeSyncHashes(b, a, &hash_mismatches);
    }
    TEST_CHECK(a.eng.Terminal() == EngineTerminal::None, "A clean across wrap");
    TEST_CHECK(b.eng.Terminal() == EngineTerminal::None, "B clean across wrap");
    TEST_CHECK(hash_mismatches == 0, "hash chain clean across the u32 wrap");
    TEST_CHECK(a.eng.SimFrontier() < first, "frontier wrapped past zero");
    TEST_CHECK(a.advances > 1900, "pipeline flowed across the wrap");

    const size_t n = std::min(a.confirmed.size(), b.confirmed.size());
    TEST_CHECK(n > 1800, "confirm streams advanced across the wrap");
    bool equal = true;
    for (size_t i = 0; i < n; ++i) {
        if (a.confirmed[i].frame != b.confirmed[i].frame ||
            a.confirmed[i].inputs[0] != b.confirmed[i].inputs[0] ||
            a.confirmed[i].inputs[1] != b.confirmed[i].inputs[1] ||
            a.confirmed[i].pre_state_hash != b.confirmed[i].pre_state_hash) {
            equal = false;
            break;
        }
    }
    TEST_CHECK(equal, "confirmed streams identical across the wrap");
}

// ── T-ENG-11: epoch rotation ────────────────────────────────────────────────

void TestEpochRotation() {
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);

    // Run 35 confirmed frames under epoch 1 so a cadence record exists.
    for (uint32_t f = 0; f < 35; ++f) {
        e.CaptureLocalInput(e.SimFrontier(), 0x0001);
        e.ReceiveRemoteInput(f, 0x0000);
        const EngineAction a = e.NextAction();
        e.CommitAdvance(a.frame, 0xAA00 + f);
    }
    TEST_CHECK(e.ConfirmedFrontier() == 35, "35 confirmed");

    // Rotation: strictly increasing, origin remap, counter continues.
    TEST_CHECK(!e.RotateEpoch(1, 35), "non-increasing epoch refused");
    // (the refusal is an internal-invariant terminal on a real misuse — use
    // a fresh engine to keep exercising the happy path)
    RollbackEngine e2;
    e2.Arm(MakeConfig(0, 0, 8), 1);
    for (uint32_t f = 0; f < 35; ++f) {
        e2.CaptureLocalInput(e2.SimFrontier(), 0x0001);
        e2.ReceiveRemoteInput(f, 0x0000);
        const EngineAction a = e2.NextAction();
        e2.CommitAdvance(a.frame, 0xAA00 + f);
    }
    OutgoingSyncHash oh{};
    while (e2.PopOutgoingSyncHash(&oh)) {}
    TEST_CHECK(e2.RotateEpoch(2, 35), "rotation accepted");
    TEST_CHECK(e2.Epoch() == 2 && e2.SimFrontier() == 35,
               "canonical counter NEVER resets (INV-15)");
    TEST_CHECK(e2.GameAbsFrame(40) == 5, "origin remap correct");

    // Stale-epoch hash: ignored.
    Net::SyncHashPayload stale{};
    stale.epoch = 1;
    stale.frame = 29;
    stale.gameplay_hash = 0xDEAD;
    TEST_CHECK(e2.ReceiveSyncHash(stale) == HashVerify::StaleEpoch,
               "stale epoch ignored");
    // Future-epoch hash: queued.
    Net::SyncHashPayload future{};
    future.epoch = 3;
    future.frame = 100;
    future.gameplay_hash = 0xBEEF;
    TEST_CHECK(e2.ReceiveSyncHash(future) == HashVerify::QueuedFuture,
               "future epoch queued");
    // Inconsistent lineage: current-epoch claim about a frame confirmed
    // under the previous epoch -> protocol violation terminal.
    Net::SyncHashPayload inconsistent{};
    inconsistent.epoch = 2;
    inconsistent.frame = 29;                  // confirmed under epoch 1
    inconsistent.gameplay_hash = 0xAA00 + 29;
    TEST_CHECK(e2.ReceiveSyncHash(inconsistent) == HashVerify::InconsistentEpoch,
               "inconsistent epoch lineage is fatal");
    TEST_CHECK(e2.Terminal() == EngineTerminal::EpochInconsistent,
               "terminal set");
}

// ── T-ENG-12: SyncHash cadence + queue bound ────────────────────────────────

void TestSyncHashCadence() {
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);

    std::vector<uint32_t> cadence_frames;
    for (uint32_t f = 0; f < 200; ++f) {
        e.CaptureLocalInput(e.SimFrontier(), 0x0001);
        e.ReceiveRemoteInput(f, 0x0000);
        const EngineAction a = e.NextAction();
        e.CommitAdvance(a.frame, 0x5000 + f);
        OutgoingSyncHash h{};
        while (e.PopOutgoingSyncHash(&h)) cadence_frames.push_back(h.frame);
    }
    TEST_CHECK(cadence_frames.size() == 6, "200 confirms -> 6 cadence hashes");
    bool exact = true;
    for (size_t i = 0; i < cadence_frames.size(); ++i) {
        if (cadence_frames[i] != (uint32_t)(30 * (i + 1) - 1)) exact = false;
    }
    TEST_CHECK(exact, "every 30th confirmed frame exactly");

    // Exact-field compare: a wrong diagnostic field is a mismatch too.
    Net::SyncHashPayload p{};
    p.epoch = 1;
    p.frame = 29;
    p.gameplay_hash = 0x5000 + 29;
    p.rng_state = 999;                        // differs (local recorded 0)
    TEST_CHECK(e.ReceiveSyncHash(p) == HashVerify::Mismatch,
               "comparison exact across ALL fields");
    TEST_CHECK(e.Terminal() == EngineTerminal::ConfirmedDesync, "desync terminal");

    // Queue bound 128 -> protocol violation on overflow (D-3).
    RollbackEngine q;
    q.Arm(MakeConfig(0, 0, 8), 1);
    for (uint32_t i = 0; i < ENGINE_SYNC_HASH_QUEUE_MAX; ++i) {
        Net::SyncHashPayload fp{};
        fp.epoch = 1;
        fp.frame = 1000 + i;
        TEST_CHECK(q.ReceiveSyncHash(fp) == HashVerify::QueuedFuture, "queued");
    }
    Net::SyncHashPayload overflow{};
    overflow.epoch = 1;
    overflow.frame = 5000;
    TEST_CHECK(q.ReceiveSyncHash(overflow) == HashVerify::InconsistentEpoch,
               "queue bound 128 enforced");
    TEST_CHECK(q.Terminal() == EngineTerminal::HashQueueOverflow, "overflow terminal");
}

// ── Desync divergence diagnostics (post-M8): ring, evidence, formatting ─────

void TestDesyncDiagnostics() {
    // Ring semantics: capacity 64, oldest-first indexing, overwrite oldest.
    DesyncDiagRing ring;
    TEST_CHECK(ring.Count() == 0, "fresh ring empty");
    DesyncDiagEntry probe{};
    TEST_CHECK(!ring.At(0, &probe), "At() on empty ring fails");

    for (uint32_t i = 0; i < 100; ++i) {
        DesyncDiagEntry e{};
        e.frame = i;
        e.epoch = 1;
        e.gameplay_hash = 0x1000ull + i;
        e.rng = 0xAB000000u + i;
        e.hp0 = (uint16_t)(500 - i);
        e.hp1 = (uint16_t)(400 - i);
        e.p1_input = (uint16_t)(i & 0x3FFF);
        e.p2_input = (uint16_t)((i * 3) & 0x3FFF);
        ring.Push(e);
    }
    TEST_CHECK(ring.Count() == DESYNC_DIAG_RING_CAPACITY, "ring capped at 64");
    TEST_CHECK(ring.At(0, &probe) && probe.frame == 36,
               "oldest retained is frame 36 (100 pushed - 64)");
    TEST_CHECK(ring.At(63, &probe) && probe.frame == 99, "newest is frame 99");
    TEST_CHECK(!ring.At(64, &probe), "At(capacity) fails");
    ring.Reset();
    TEST_CHECK(ring.Count() == 0 && !ring.At(0, &probe), "Reset empties");

    // Formatting pins: deterministic content, no wall clock anywhere.
    DesyncDiagEntry fe{};
    fe.frame = 209;
    fe.epoch = 3;
    fe.gameplay_hash = 0x0123456789ABCDEFull;
    fe.rng = 0xDEADBEEFu;
    fe.hp0 = 500;
    fe.hp1 = 32;
    fe.p1_input = 0x0005;
    fe.p2_input = 0x2001;
    char line[192];
    DesyncDiag_FormatRingEntry(fe, line, sizeof(line));
    TEST_CHECK(std::strcmp(line,
        "RING frame=209 epoch=3 hash=0123456789abcdef rng=deadbeef "
        "hp0=500 hp1=32 p1=0x0005 p2=0x2001") == 0,
        "ring line format pinned");

    DesyncEvidence ev{};
    ev.valid = true;
    ev.frame = 209;
    ev.epoch = 3;
    ev.local_hash = 0x1111111111111111ull;
    ev.peer_hash  = 0x2222222222222222ull;
    ev.local_rng = 0x00000010u;
    ev.peer_rng  = 0x00000010u;
    ev.local_hp0 = 100; ev.peer_hp0 = 100;
    ev.local_hp1 = 50;  ev.peer_hp1 = 50;
    char eline[320];
    DesyncDiag_FormatEvidence(ev, eline, sizeof(eline));
    TEST_CHECK(std::strcmp(eline,
        "EVIDENCE frame=209 epoch=3 local_hash=1111111111111111 "
        "peer_hash=2222222222222222 local_rng=00000010 peer_rng=00000010 "
        "local_hp0=100 local_hp1=50 peer_hp0=100 peer_hp1=50 "
        "first_divergent=hash") == 0,
        "evidence line format pinned");

    // First-divergent-field priority mirrors ReceiveSyncHash comparison
    // order: hash (authoritative) > rng > hp0 > hp1.
    TEST_CHECK(std::strcmp(DesyncEvidence_FirstDivergentField(ev), "hash") == 0,
               "hash diff wins");
    ev.peer_hash = ev.local_hash;
    ev.peer_rng = 0x00000011u;
    TEST_CHECK(std::strcmp(DesyncEvidence_FirstDivergentField(ev), "rng") == 0,
               "rng next");
    ev.peer_rng = ev.local_rng;
    ev.peer_hp0 = 99;
    TEST_CHECK(std::strcmp(DesyncEvidence_FirstDivergentField(ev), "hp0") == 0,
               "hp0 next");
    ev.peer_hp0 = ev.local_hp0;
    ev.peer_hp1 = 49;
    TEST_CHECK(std::strcmp(DesyncEvidence_FirstDivergentField(ev), "hp1") == 0,
               "hp1 last");
    ev.peer_hp1 = ev.local_hp1;
    TEST_CHECK(std::strcmp(DesyncEvidence_FirstDivergentField(ev), "none") == 0,
               "all-equal is none");
    DesyncEvidence inv{};
    TEST_CHECK(std::strcmp(DesyncEvidence_FirstDivergentField(inv), "none") == 0,
               "invalid evidence is none");

    // Engine capture: the ConfirmedDesync terminal must leave the FULL
    // failing pair readable (all SyncHash fields, both sides).
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);
    DesyncEvidence pre{};
    TEST_CHECK(!e.GetDesyncEvidence(&pre), "no evidence before any mismatch");
    for (uint32_t f = 0; f < 30; ++f) {
        e.CaptureLocalInput(e.SimFrontier(), 0x0001);
        e.ReceiveRemoteInput(f, 0x0000);
        const EngineAction a = e.NextAction();
        e.CommitAdvance(a.frame, 0xAB00 + f,
                        /*rng=*/0x11110000u + f,
                        /*hp0=*/(uint16_t)(300 + f),
                        /*hp1=*/(uint16_t)(200 + f));
    }
    Net::SyncHashPayload p{};
    p.epoch = 1;
    p.frame = 29;
    p.gameplay_hash = 0xAB00 + 29;           // matches local
    p.rng_state = 0x22220000u;               // diverges
    p.hp0 = (uint16_t)(300 + 29);
    p.hp1 = (uint16_t)(200 + 29);
    TEST_CHECK(e.ReceiveSyncHash(p) == HashVerify::Mismatch, "mismatch fires");
    TEST_CHECK(e.Terminal() == EngineTerminal::ConfirmedDesync,
               "ConfirmedDesync terminal");
    DesyncEvidence got{};
    TEST_CHECK(e.GetDesyncEvidence(&got), "evidence captured");
    TEST_CHECK(got.frame == 29 && got.epoch == 1, "evidence frame/epoch");
    TEST_CHECK(got.local_hash == 0xAB00 + 29 && got.peer_hash == 0xAB00 + 29,
               "hashes recorded (equal here)");
    TEST_CHECK(got.local_rng == 0x11110000u + 29 && got.peer_rng == 0x22220000u,
               "rng pair recorded");
    TEST_CHECK(got.local_hp0 == 300 + 29 && got.peer_hp0 == 300 + 29 &&
               got.local_hp1 == 200 + 29 && got.peer_hp1 == 200 + 29,
               "hp pairs recorded");
    TEST_CHECK(std::strcmp(DesyncEvidence_FirstDivergentField(got), "rng") == 0,
               "localizes the divergent field (rng)");

    // Re-arm clears the evidence (fresh session, fresh diagnostics).
    e.Disarm();
    e.Arm(MakeConfig(0, 0, 8), 2);
    TEST_CHECK(!e.GetDesyncEvidence(nullptr), "Arm clears evidence");
}

// ── Lifecycle window predicate (INV-25 / M4-7) ──────────────────────────────

void TestLifecycleWindow() {
    TEST_CHECK(LifecycleWindow_IsExactInputNext(8, 3, false) == false,
               "plain gameplay predicts normally (round-end audit)");
    TEST_CHECK(LifecycleWindow_IsExactInputNext(8, 3, true) == true,
               "match-exit tick is exact (Handle_ReleaseAll boundary)");
    TEST_CHECK(LifecycleWindow_IsExactInputNext(9, 0, false) == true,
               "outside gameplay is exact always");
    TEST_CHECK(LifecycleWindow_IsExactInputNext(8, 1, false) == true,
               "non-fight substate is exact");

    // Engine behavior: exact window without the remote actual vetoes the
    // predicted frame (Stall(LifecycleBoundary)); the actual releases it.
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);
    e.CaptureLocalInput(0, 0x0001);
    e.SetLifecycleExactNext(true);
    EngineAction a = e.NextAction();
    TEST_CHECK(a.kind == EngineActionKind::Stall &&
               a.hold_cause == HoldCause::LifecycleBoundary,
               "predicted frame vetoed at the boundary");
    e.ReceiveRemoteInput(0, 0x0002);
    a = e.NextAction();
    TEST_CHECK(a.kind == EngineActionKind::Advance && !a.remote_predicted,
               "actual input crosses the boundary");
}

// ── §7.2 deterministic soak: 100k frames, 3% loss, 40±15 ms jitter ─────────

void SoakRun() {
    Peer a, b;
    a.eng.Arm(MakeConfig(0, 2, 8), 1);
    b.eng.Arm(MakeConfig(1, 2, 8), 1);

    // 40±15 ms RTT at 60 Hz ≈ 1–2 frames one-way: base 1 frame + 0..2 jitter.
    Link ab(0xA11CE001, 3, 1, 2);
    Link ba(0xA11CE002, 3, 1, 2);
    Rng inputs(0xC0FFEE);
    uint32_t hash_mismatches = 0;

    const int kFrames = 100000;
    for (int frame = 0; frame < kFrames; ++frame) {
        const uint32_t now = (uint32_t)frame;
        ab.DeliverDue(now, b.eng);
        ba.DeliverDue(now, a.eng);

        a.StepPass(ScriptedInput(inputs));
        b.StepPass(ScriptedInput(inputs));

        Net::InputStreamPayload p{};
        if (a.eng.BuildInputStream(&p)) ab.Send(p, now);
        if (b.eng.BuildInputStream(&p)) ba.Send(p, now);

        // Reliable control channel: hashes never drop.
        ExchangeSyncHashes(a, b, &hash_mismatches);
        ExchangeSyncHashes(b, a, &hash_mismatches);

        if (a.eng.Terminal() != EngineTerminal::None ||
            b.eng.Terminal() != EngineTerminal::None) {
            break;
        }
    }

    TEST_CHECK(a.eng.Terminal() == EngineTerminal::None, "soak: A no terminal");
    TEST_CHECK(b.eng.Terminal() == EngineTerminal::None, "soak: B no terminal");
    if (a.eng.Terminal() != EngineTerminal::None)
        std::printf("  A terminal: %s\n", a.eng.TerminalDetail());
    if (b.eng.Terminal() != EngineTerminal::None)
        std::printf("  B terminal: %s\n", b.eng.TerminalDetail());

    TEST_CHECK(hash_mismatches == 0, "soak: hash chain clean");

    // Zero confirm stalls: both confirm frontiers track the slower sim
    // closely (the whole suffix refills from any single packet).
    const uint32_t conf_a = a.eng.ConfirmedFrontier();
    const uint32_t conf_b = b.eng.ConfirmedFrontier();
    TEST_CHECK(conf_a > (uint32_t)(kFrames * 9 / 10), "soak: A confirmed ~all frames");
    TEST_CHECK(conf_b > (uint32_t)(kFrames * 9 / 10), "soak: B confirmed ~all frames");

    const size_t n = std::min(a.confirmed.size(), b.confirmed.size());
    bool equal = true;
    size_t first_diff = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a.confirmed[i].frame != b.confirmed[i].frame ||
            a.confirmed[i].inputs[0] != b.confirmed[i].inputs[0] ||
            a.confirmed[i].inputs[1] != b.confirmed[i].inputs[1] ||
            a.confirmed[i].pre_state_hash != b.confirmed[i].pre_state_hash) {
            equal = false;
            first_diff = i;
            break;
        }
    }
    TEST_CHECK(equal, "soak: per-frame confirmed streams identical (zero conflicts)");
    if (!equal) {
        std::printf("  first divergence at confirmed index %zu (frame %u vs %u)\n",
                    first_diff, a.confirmed[first_diff].frame,
                    b.confirmed[first_diff].frame);
    }

    std::printf(
        "SOAK: %d frames  A[adv=%u pred_holds=%u rb=%u max_depth=%u] "
        "B[adv=%u pred_holds=%u rb=%u max_depth=%u] verified_hashes=%u/%u\n",
        kFrames,
        a.advances, a.stalls_pred, a.eng.GetStats().rollbacks,
        a.eng.GetStats().max_rollback_depth,
        b.advances, b.stalls_pred, b.eng.GetStats().rollbacks,
        b.eng.GetStats().max_rollback_depth,
        a.eng.GetStats().sync_hashes_verified,
        b.eng.GetStats().sync_hashes_verified);
}

// ── §7.5 #4 microbench: snapshot save/restore + hash cost ───────────────────
//
// Offline stand-in for the in-game savestate cost: same byte volume as the
// real GameSnapshot (main region + inputs), memcpy save + memcpy restore +
// Block64 gameplay digest. The 1.5 ms p99 acceptance is judged on the
// min-spec machine; this offline run reports the numbers and only fails on
// an order-of-magnitude blowout.

void Microbench() {
    const size_t kBytes = sizeof(GameSnapshot);
    std::vector<uint8_t> live(kBytes, 0);
    std::vector<uint8_t> slot(kBytes, 0);
    Rng rng(0xBE9C11);
    for (size_t i = 0; i < kBytes; i += 4) {
        const uint32_t v = rng.Next();
        memcpy(&live[i], &v, std::min<size_t>(4, kBytes - i));
    }

    const int kIters = 512;
    std::vector<uint64_t> save_ns(kIters), restore_ns(kIters), hash_ns(kIters);
    volatile uint64_t sink = 0;
    for (int i = 0; i < kIters; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        memcpy(slot.data(), live.data(), kBytes);
        auto t1 = std::chrono::steady_clock::now();
        memcpy(live.data(), slot.data(), kBytes);
        auto t2 = std::chrono::steady_clock::now();
        sink ^= Block64(slot.data(), kBytes);
        auto t3 = std::chrono::steady_clock::now();
        save_ns[i] = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        restore_ns[i] = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        hash_ns[i] = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
    }
    auto pct = [](std::vector<uint64_t> v, double p) {
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)((double)v.size() * p))];
    };
    const uint64_t save_p99 = pct(save_ns, 0.99);
    const uint64_t restore_p99 = pct(restore_ns, 0.99);
    std::printf("BENCH: snapshot=%zu KB  save p50=%llu us p99=%llu us  "
                "restore p99=%llu us  hash p99=%llu us\n",
                kBytes / 1024,
                (unsigned long long)(pct(save_ns, 0.50) / 1000),
                (unsigned long long)(save_p99 / 1000),
                (unsigned long long)(restore_p99 / 1000),
                (unsigned long long)(pct(hash_ns, 0.99) / 1000));
    // Acceptance (§7.5 #4) is p99 ≤ 1.5 ms on min-spec; the offline gate
    // only trips on a blowout (10x) so CI noise cannot flake it.
    TEST_CHECK(save_p99 < 15ull * 1000 * 1000, "save p99 within 10x of budget");
    TEST_CHECK(restore_p99 < 15ull * 1000 * 1000, "restore p99 within 10x of budget");
}

} // namespace

// The forced-rollback determinism self-test (QOH99 model): a replayed frame
// with identical inputs must reproduce its original pre-state hash. Proves
// BOTH directions — a faithful replay is silent, a divergent one is caught at
// the exact frame — because a self-test that cannot fail proves nothing.
static void TestReplayDeterminismSelfTest() {
    // Faithful replay: same inputs, same hashes -> verified, zero bad.
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8), 1);
        for (uint32_t f = 0; f < 5; ++f) {
            e.CaptureLocalInput(e.SimFrontier(), 0x0001);
            const EngineAction a = e.NextAction();
            e.CommitAdvance(a.frame, 100 + f);
        }
        e.SetForcedRollback(3);
        const EngineAction a = e.NextAction();
        TEST_CHECK(a.kind == EngineActionKind::Rollback && a.frame == 2,
                   "forced depth-3 transaction from the frontier");
        TEST_CHECK(e.BeginRollback(a.frame), "begin forced rollback");
        uint32_t f = 0; uint16_t in[2];
        while (e.NextReplayInputs(&f, in)) {
            e.CommitReplayFrame(f, 100 + f);   // reproduce the original hash
        }
        TEST_CHECK(e.GetStats().replay_verifications == 3,
                   "every replayed frame is verified");
        TEST_CHECK(e.GetStats().replay_mismatches == 0,
                   "a faithful replay reports no mismatch");
    }
    // Nondeterministic replay: same inputs, different state -> caught.
    {
        RollbackEngine e;
        e.Arm(MakeConfig(0, 0, 8), 1);
        for (uint32_t f = 0; f < 5; ++f) {
            e.CaptureLocalInput(e.SimFrontier(), 0x0001);
            const EngineAction a = e.NextAction();
            e.CommitAdvance(a.frame, 100 + f);
        }
        e.SetForcedRollback(3);
        const EngineAction a = e.NextAction();
        TEST_CHECK(e.BeginRollback(a.frame), "begin forced rollback");
        uint32_t f = 0; uint16_t in[2];
        while (e.NextReplayInputs(&f, in)) {
            // Frame 3 lands somewhere else than it did the first time.
            e.CommitReplayFrame(f, f == 3 ? 0xDEAD : (100 + f));
        }
        TEST_CHECK(e.GetStats().replay_mismatches == 1,
                   "the divergent frame is caught");
        TEST_CHECK(e.GetStats().last_replay_mismatch_frame == 3,
                   "and named exactly");
        TEST_CHECK(e.GetStats().last_replay_expect_hash == 103 &&
                       e.GetStats().last_replay_actual_hash == 0xDEAD,
                   "with both hashes for the diff");
    }
}

// A real correction moves every frame after it, legitimately. The self-test
// must scope itself to the input-identical prefix, or deep prediction (where
// corrections are the norm) reports constant false nondeterminism — observed
// live at delivery_delay=30: replay_bad climbing into the hundreds with a
// perfectly deterministic sim.
static void TestReplaySelfTestIgnoresCorrectedSuffix() {
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);
    for (uint32_t f = 0; f < 6; ++f) {
        e.CaptureLocalInput(e.SimFrontier(), 0x0001);
        const EngineAction a = e.NextAction();
        e.CommitAdvance(a.frame, 100 + f);   // remote predicted as neutral
    }
    // The real remote input for frame 2 turns out to be different.
    e.ReceiveRemoteInput(2, 0x0208);
    const EngineAction a = e.NextAction();
    TEST_CHECK(a.kind == EngineActionKind::Rollback && a.frame == 2,
               "correction rolls back to the mispredicted frame");
    TEST_CHECK(e.BeginRollback(a.frame), "begin");

    uint32_t f = 0; uint16_t in[2];
    while (e.NextReplayInputs(&f, in)) {
        // Frame 2 onward legitimately lands somewhere new.
        e.CommitReplayFrame(f, 900 + f);
    }
    TEST_CHECK(e.GetStats().replay_mismatches == 0,
               "a corrected frame and everything after it is never called nondeterminism");
}

int main() {
    TestCaptureOnce();          // T-ENG-1
    TestDelayRelabel();         // T-ENG-2
    TestPredictMismatch();      // T-ENG-3
    TestRollbackTransaction();  // T-ENG-4
    TestConfirmOrdering();      // T-ENG-5
    TestIngestTaxonomy();       // T-ENG-6
    TestWindowMath();           // T-ENG-7
    TestProducer();             // T-ENG-8
    TestPredictionLimitPin();   // T-ENG-9
    TestWrapSafety();           // T-ENG-10
    TestEpochRotation();        // T-ENG-11
    TestSyncHashCadence();      // T-ENG-12
    TestDesyncDiagnostics();    // post-M8 divergence diagnostics
    TestLifecycleWindow();      // INV-25 / M4-7 predicate
    TestReplayDeterminismSelfTest();  // QOH99-model local determinism check
    TestReplaySelfTestIgnoresCorrectedSuffix();
    SoakRun();                  // §7.2 socket-free soak (M4 exit gate)
    Microbench();               // §7.5 #4

    std::printf("engine2_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
