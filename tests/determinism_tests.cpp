// re0.7 post-M8 — two-peer determinism suite (offline R-DET class).
//
// The QOH99 method, twice over: (1) the harness below IS a full peer pair —
// the RollbackEngine owns no sockets/clock/game memory, so a seeded
// simulated link exercises the identical object the game adapter drives;
// (2) the forced-rollback mode is the offline analog of QOH99's selftest
// (save → tick → restore → replay K → compare): every advanced frame is
// preceded by a genuine depth-N rollback transaction over the same sealed
// inputs, so any nondeterminism in the pipeline surfaces as a SyncHash /
// confirmed-stream divergence instead of hiding behind correct predictions.
//
// Matrix (every cell fully seeded — no wall clock, no global RNG, INV-16):
//   - network profiles: stable low/high ping, jittery low/high, 3%/10% loss,
//     loss+jitter combined, reorder+duplication
//   - per profile: rollback budgets 8 AND 12; forced-rollback-every-frame
//     (depth 1..8) enabled in at least half the cells
//   - per cell: a 3-epoch session (RotateEpoch at fully-confirmed
//     boundaries) with a DIFFERENT per-epoch state-blob generator (models
//     different characters/config payloads per match)
// Assertions per cell:
//   - zero desync: SyncHash chains identical AND per-frame confirmed
//     streams byte-identical across both peers (frames/inputs/hash/epoch)
//   - zero terminals of any kind
//   - canonical counter monotonic across all epochs (INV-15) and the
//     confirmed record epochs non-decreasing
//   - full-speed invariant: PredictionLimit holds only where the profile
//     makes them structurally unavoidable — ZERO in the stable-low cells,
//     bounded elsewhere (bound derived from delay+budget vs transit)

#include "rollback/engine2.h"
#include "rollback/block_digest.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace Rollback;

namespace {

int g_checks = 0;
int g_failures = 0;

#define TEST_CHECK(cond, msg)                                                 \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);       \
        }                                                                     \
    } while (0)

#define CELL_CHECK(cell, cond, msg)                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("FAIL [%s]: %s (%s:%d)\n", (cell), msg,               \
                        __FILE__, __LINE__);                                  \
        }                                                                     \
    } while (0)

// Deterministic PRNG (xorshift32) — the virtual network and the scripted
// input sources. Seeded per cell; no wall clock anywhere.
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

EngineConfig MakeConfig(uint8_t player, uint8_t delay, uint8_t rollback) {
    EngineConfig c{};
    c.local_player = player;
    c.input_delay = delay;
    c.max_rollback = rollback;
    c.neutral_input = 0x0000;
    c.first_frame = 0;
    c.max_remote_future = 120;
    c.history_capacity = 4096;
    return c;
}

// Per-epoch state seed: models a fresh match state blob per epoch (the
// director resets state identity at rotation; savestates are invalidated).
uint64_t EpochStateSeed(uint32_t epoch) {
    return 0x1234567812345678ull ^ (0x9E3779B97F4A7C15ull * (uint64_t)epoch);
}

// ── Cell specification ──────────────────────────────────────────────────────

struct CellSpec {
    const char* name;
    uint32_t loss_pct;      // 0..100, per packet copy
    uint32_t base_delay;    // one-way transit, whole frame periods
    uint32_t jitter_extra;  // + uniform [0, jitter_extra] frames
    uint32_t dup_pct;       // duplication percentage (reorder rides jitter)
    uint8_t  input_delay;   // D, both peers
    uint8_t  budget;        // R, both peers
    uint8_t  forced_depth;  // forced rollback depth per frame (0 = off)
    uint32_t seed;
};

// Full-speed invariant bound (runbook §7.5#2 analog, offline): holds are
// legitimate only when the prediction ceiling is genuinely exceeded.
// Structural slack = (D + R) − (worst one-way transit + 2). With
// non-negative slack and a lossless link the redundant 32-frame window
// keeps the remote-actual lag under the ceiling, so the bound is ZERO —
// the stable cells assert exactly that. Loss cells get a bound
// proportional to the expected refill gaps; jitter/duplication-only cells
// get a small fixed transient allowance (window refill covers reorder, but
// pipes can transiently spike). A genuinely underbuffered cell (negative
// slack) would get a proportional allowance — the matrix contains none by
// design (R-UNDER is a field run).
uint32_t DeriveHoldBound(const CellSpec& c, uint32_t total_frames) {
    const uint32_t worst_transit = c.base_delay + c.jitter_extra + 1;
    const int32_t slack = (int32_t)c.input_delay + (int32_t)c.budget -
                          (int32_t)(worst_transit + 2);
    uint32_t bound = 0;
    if (c.loss_pct > 0) {
        bound += total_frames * c.loss_pct / 50 + 64;   // 2x expected + fixed
    }
    if (slack < 0) {
        bound += total_frames / 4;                       // underbuffered cell
    } else if (c.jitter_extra > 0 || c.dup_pct > 0) {
        bound += 48;                                     // refill transient
    }
    return bound;
}

// ── Peer harness ────────────────────────────────────────────────────────────
//
// Same shape as the engine2_tests soak Peer: the "game" is a u64 state
// evolved by Block64 over the canonical input pair, extended with a
// per-epoch blob generator and split hold accounting.

struct Peer {
    RollbackEngine eng;
    uint64_t state = 0;
    uint32_t cur_epoch = 1;
    std::map<uint32_t, uint64_t> snapshots;   // canonical frame -> pre-tick
    std::vector<ConfirmedFrame> confirmed;
    uint32_t advances = 0;
    uint32_t holds_pred = 0;
    uint32_t holds_input = 0;
    uint32_t holds_life = 0;
    bool harness_fault = false;

    // Epoch rotation execution (the director's half): fresh state identity,
    // savestate invalidation. Canonical counter and input streams continue.
    void ResetEpochState(uint32_t epoch) {
        cur_epoch = epoch;
        state = EpochStateSeed(epoch);
        snapshots.clear();
    }

    void Tick(uint16_t p1, uint16_t p2) {
        // Per-epoch state-blob generator: the hashed payload differs across
        // epochs even for identical input words (models different
        // characters / match configs per epoch).
        const uint16_t blob[4] = {
            p1, p2,
            (uint16_t)(cur_epoch * 0x0101u),
            (uint16_t)(0x00A5u + cur_epoch),
        };
        state = Block64(blob, sizeof(blob), state);
    }

    bool RunRollback(const EngineAction& a) {
        if (!eng.BeginRollback(a.frame)) return false;
        auto it = snapshots.find(a.frame);
        if (it == snapshots.end()) {
            std::printf("HARNESS: no snapshot at %u\n", a.frame);
            ++g_failures;
            harness_fault = true;
            return false;
        }
        state = it->second;
        uint32_t f = 0;
        uint16_t in[2] = {0, 0};
        while (eng.NextReplayInputs(&f, in)) {
            snapshots[f] = state;
            if (!eng.CommitReplayFrame(f, state)) return false;
            Tick(in[0], in[1]);
        }
        return eng.FinishRollback();
    }

    void Drain() {
        ConfirmedFrame cf{};
        while (eng.PopConfirmedFrame(&cf)) confirmed.push_back(cf);
    }

    // One scheduler pass: capture, correct (real or forced), then at most
    // one visible advance; producer feeds on stalls (INV-24).
    void StepPass(uint16_t local_input) {
        if (eng.Terminal() != EngineTerminal::None || harness_fault) return;
        eng.CaptureLocalInput(eng.SimFrontier(), local_input);

        for (int guard = 0; guard < 64; ++guard) {
            const EngineAction a = eng.NextAction();
            if (a.kind == EngineActionKind::Rollback) {
                if (!RunRollback(a)) return;
                continue;  // corrections first; may still advance this pass
            }
            if (a.kind == EngineActionKind::Advance) {
                snapshots[a.frame] = state;
                if (!eng.CommitAdvance(a.frame, state)) return;
                Tick(a.inputs[0], a.inputs[1]);
                ++advances;
            } else {
                switch (a.hold_cause) {
                    case HoldCause::PredictionLimit:   ++holds_pred; break;
                    case HoldCause::LocalInputMissing: ++holds_input; break;
                    case HoldCause::LifecycleBoundary: ++holds_life; break;
                    default: break;
                }
                eng.ProduceLocalInputAhead(local_input);
            }
            break;  // one visible opportunity per pass
        }
        Drain();
    }

    // Boundary-align drain: complete pending (real or forced) corrections
    // without capturing or advancing — the sim is parked at the epoch
    // boundary, exactly like the adapter during a match-boundary barrier.
    void PumpCorrections() {
        if (eng.Terminal() != EngineTerminal::None || harness_fault) return;
        for (int guard = 0; guard < 8; ++guard) {
            const EngineAction a = eng.NextAction();
            if (a.kind != EngineActionKind::Rollback) break;
            if (!RunRollback(a)) return;
        }
        Drain();
    }
};

// Virtual lossy/jittery/reordering/duplicating link measured in whole frame
// periods. Arrival scan order + jitter = the reorder shim (same model as the
// engine2_tests soak); dup_pct re-enqueues an identical copy with its own
// independent transit time.
struct Link {
    struct Packet {
        Net::InputStreamPayload payload;
        uint32_t deliver_at;
    };
    std::vector<Packet> in_flight;
    Rng rng;
    uint32_t loss_pct;
    uint32_t base_delay;
    uint32_t jitter_extra;
    uint32_t dup_pct;

    Link(uint32_t seed, const CellSpec& c)
        : rng(seed), loss_pct(c.loss_pct), base_delay(c.base_delay),
          jitter_extra(c.jitter_extra), dup_pct(c.dup_pct) {}

    void Enqueue(const Net::InputStreamPayload& p, uint32_t now) {
        if (rng.Below(100) < loss_pct) return;   // loss shim
        Packet pkt{};
        pkt.payload = p;
        pkt.deliver_at = now + base_delay + rng.Below(jitter_extra + 1);
        in_flight.push_back(pkt);
    }

    void Send(const Net::InputStreamPayload& p, uint32_t now) {
        Enqueue(p, now);
        if (dup_pct > 0 && rng.Below(100) < dup_pct) {
            Enqueue(p, now);                      // duplication shim
        }
    }

    void DeliverDue(uint32_t now, RollbackEngine& dst) {
        for (size_t i = 0; i < in_flight.size();) {
            if ((int32_t)(now - in_flight[i].deliver_at) >= 0) {
                dst.IngestInputStream(in_flight[i].payload);
                in_flight[i] = in_flight.back();
                in_flight.pop_back();             // random-order pop = reorder
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

// ── Cell runner ─────────────────────────────────────────────────────────────

constexpr uint32_t kEpochs = 3;         // >= 3 matches per cell
constexpr uint32_t kEpochFrames = 1200; // canonical frames per epoch

void RunCell(const CellSpec& c) {
    Peer a, b;
    TEST_CHECK(a.eng.Arm(MakeConfig(0, c.input_delay, c.budget), 1), "A arms");
    TEST_CHECK(b.eng.Arm(MakeConfig(1, c.input_delay, c.budget), 1), "B arms");
    a.eng.SetForcedRollback(c.forced_depth);
    b.eng.SetForcedRollback(c.forced_depth);
    a.ResetEpochState(1);
    b.ResetEpochState(1);

    Link ab(c.seed ^ 0x0000A11Cu, c);
    Link ba(c.seed ^ 0x0000E001u, c);
    Rng inputs_a(c.seed ^ 0x000000AAu);
    Rng inputs_b(c.seed ^ 0x000000BBu);

    uint32_t hash_mismatches = 0;
    uint32_t now = 0;
    bool alive = true;

    auto pump_wire = [&](void) {
        Net::InputStreamPayload p{};
        if (a.eng.BuildInputStream(&p)) ab.Send(p, now);
        if (b.eng.BuildInputStream(&p)) ba.Send(p, now);
        ExchangeSyncHashes(a, b, &hash_mismatches);
        ExchangeSyncHashes(b, a, &hash_mismatches);
    };
    auto peers_clean = [&](void) {
        return a.eng.Terminal() == EngineTerminal::None &&
               b.eng.Terminal() == EngineTerminal::None &&
               !a.harness_fault && !b.harness_fault;
    };

    for (uint32_t epoch = 1; epoch <= kEpochs && alive; ++epoch) {
        const uint32_t target = a.eng.EpochFrameOrigin() + kEpochFrames;

        // Battle phase: both sims run to the epoch boundary; a peer that
        // reaches it first parks (corrections only) while its input stream
        // keeps resending for the laggard (INV-24 by construction).
        int64_t guard = (int64_t)kEpochFrames * 64;
        while (((int32_t)(target - a.eng.SimFrontier()) > 0 ||
                (int32_t)(target - b.eng.SimFrontier()) > 0) &&
               guard-- > 0 && peers_clean()) {
            ++now;
            ab.DeliverDue(now, b.eng);
            ba.DeliverDue(now, a.eng);
            if ((int32_t)(target - a.eng.SimFrontier()) > 0) {
                a.StepPass(ScriptedInput(inputs_a));
            } else {
                a.PumpCorrections();
            }
            if ((int32_t)(target - b.eng.SimFrontier()) > 0) {
                b.StepPass(ScriptedInput(inputs_b));
            } else {
                b.PumpCorrections();
            }
            pump_wire();
        }
        CELL_CHECK(c.name, guard > 0, "battle phase reached the epoch boundary");
        CELL_CHECK(c.name,
                   a.eng.SimFrontier() == target && b.eng.SimFrontier() == target,
                   "both sims parked exactly at the boundary");

        // Align phase: drain the wire until BOTH peers confirmed the whole
        // epoch (the harness analog of the EpochAlign barrier — rotation is
        // legal only at a fully-confirmed boundary).
        guard = 60000;
        while ((a.eng.ConfirmedFrontier() != target ||
                b.eng.ConfirmedFrontier() != target) &&
               guard-- > 0 && peers_clean()) {
            // (int64 guard: exhaustion leaves it negative, never wrapped)
            ++now;
            ab.DeliverDue(now, b.eng);
            ba.DeliverDue(now, a.eng);
            a.PumpCorrections();
            b.PumpCorrections();
            pump_wire();
        }
        CELL_CHECK(c.name, guard > 0, "align phase fully confirmed the epoch");
        if (!peers_clean() ||
            a.eng.ConfirmedFrontier() != target ||
            b.eng.ConfirmedFrontier() != target) {
            alive = false;
            break;
        }

        if (epoch < kEpochs) {
            const uint32_t next = epoch + 1;
            CELL_CHECK(c.name, a.eng.RotateEpoch(next, target), "A rotates");
            CELL_CHECK(c.name, b.eng.RotateEpoch(next, target), "B rotates");
            CELL_CHECK(c.name,
                       a.eng.Epoch() == next && a.eng.SimFrontier() == target,
                       "rotation continues the canonical counter (INV-15)");
            a.ResetEpochState(next);
            b.ResetEpochState(next);
        }
    }

    const uint32_t total_frames = kEpochs * kEpochFrames;

    // Zero terminals.
    CELL_CHECK(c.name, a.eng.Terminal() == EngineTerminal::None, "A no terminal");
    CELL_CHECK(c.name, b.eng.Terminal() == EngineTerminal::None, "B no terminal");
    if (a.eng.Terminal() != EngineTerminal::None)
        std::printf("  [%s] A terminal: %s\n", c.name, a.eng.TerminalDetail());
    if (b.eng.Terminal() != EngineTerminal::None)
        std::printf("  [%s] B terminal: %s\n", c.name, b.eng.TerminalDetail());

    // Zero desync: hash chains clean AND actually exercised.
    CELL_CHECK(c.name, hash_mismatches == 0, "SyncHash chains identical");
    CELL_CHECK(c.name,
               a.eng.GetStats().sync_hashes_verified > total_frames / 60 &&
               b.eng.GetStats().sync_hashes_verified > total_frames / 60,
               "SyncHash verification actually ran");

    // Zero desync: per-frame confirmed streams byte-identical.
    CELL_CHECK(c.name,
               a.confirmed.size() == (size_t)total_frames &&
               b.confirmed.size() == (size_t)total_frames,
               "every canonical frame confirmed on both peers");
    const size_t n = std::min(a.confirmed.size(), b.confirmed.size());
    bool equal = true;
    size_t first_diff = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a.confirmed[i].frame != b.confirmed[i].frame ||
            a.confirmed[i].epoch != b.confirmed[i].epoch ||
            a.confirmed[i].inputs[0] != b.confirmed[i].inputs[0] ||
            a.confirmed[i].inputs[1] != b.confirmed[i].inputs[1] ||
            a.confirmed[i].pre_state_hash != b.confirmed[i].pre_state_hash) {
            equal = false;
            first_diff = i;
            break;
        }
    }
    CELL_CHECK(c.name, equal, "confirmed streams byte-identical");
    if (!equal) {
        std::printf("  [%s] first divergence at confirmed index %zu "
                    "(frame %u vs %u)\n",
                    c.name, first_diff, a.confirmed[first_diff].frame,
                    b.confirmed[first_diff].frame);
    }

    // Canonical counter monotonic across epochs (INV-15): confirmed frames
    // strictly increasing and record epochs non-decreasing over the WHOLE
    // session (three rotations included).
    bool monotonic = true;
    for (size_t i = 1; i < a.confirmed.size(); ++i) {
        if (a.confirmed[i].frame != a.confirmed[i - 1].frame + 1 ||
            a.confirmed[i].epoch < a.confirmed[i - 1].epoch) {
            monotonic = false;
            break;
        }
    }
    CELL_CHECK(c.name, monotonic,
               "canonical counter monotonic + epochs non-decreasing (INV-15)");
    CELL_CHECK(c.name, a.eng.Epoch() == kEpochs && b.eng.Epoch() == kEpochs,
               "session rotated through all epochs");

    // Forced-rollback cells: the forcing genuinely engaged — a transaction
    // per advanced frame (minus the depth-clamped epoch openers) at the
    // requested depth.
    if (c.forced_depth > 0) {
        CELL_CHECK(c.name,
                   a.eng.GetStats().rollbacks >= total_frames - kEpochs * c.forced_depth &&
                   b.eng.GetStats().rollbacks >= total_frames - kEpochs * c.forced_depth,
                   "forced rollback ran every advanced frame");
        CELL_CHECK(c.name,
                   a.eng.GetStats().max_rollback_depth >= c.forced_depth,
                   "forced depth-N transactions reached the requested depth");
    }

    // Full-speed invariant: holds only where structurally unavoidable.
    const uint32_t bound = DeriveHoldBound(c, total_frames);
    if (bound == 0) {
        CELL_CHECK(c.name, a.holds_pred == 0 && b.holds_pred == 0,
                   "ZERO PredictionLimit holds in a structurally clean cell");
    } else {
        CELL_CHECK(c.name, a.holds_pred <= bound && b.holds_pred <= bound,
                   "PredictionLimit holds within the derived bound");
    }
    CELL_CHECK(c.name, a.holds_life == 0 && b.holds_life == 0,
               "no lifecycle holds (no exact windows in this harness)");

    std::printf(
        "CELL %-24s frames=%u  A[adv=%u hold_pred=%u rb=%u depth_max=%u] "
        "B[adv=%u hold_pred=%u rb=%u depth_max=%u] hold_bound=%u verified=%u/%u\n",
        c.name, total_frames,
        a.advances, a.holds_pred, a.eng.GetStats().rollbacks,
        a.eng.GetStats().max_rollback_depth,
        b.advances, b.holds_pred, b.eng.GetStats().rollbacks,
        b.eng.GetStats().max_rollback_depth,
        bound,
        a.eng.GetStats().sync_hashes_verified,
        b.eng.GetStats().sync_hashes_verified);
}

// ── Forced-rollback unit pins (engine-level) ────────────────────────────────

void TestForcedRollbackUnit() {
    // Depth-1 forcing on a lossless immediate pipeline: one transaction per
    // advanced frame, sim keeps full speed (zero holds), state identical to
    // the unforced run.
    RollbackEngine e;
    e.Arm(MakeConfig(0, 0, 8), 1);
    e.SetForcedRollback(1);

    uint64_t state = EpochStateSeed(1);
    std::map<uint32_t, uint64_t> snaps;
    uint32_t rollbacks_seen = 0;

    for (uint32_t f = 0; f < 40; ++f) {
        e.CaptureLocalInput(e.SimFrontier(), (uint16_t)(f & 0x000F));
        e.ReceiveRemoteInput(f, (uint16_t)((f * 3) & 0x000F));
        for (int guard = 0; guard < 8; ++guard) {
            const EngineAction a = e.NextAction();
            if (a.kind == EngineActionKind::Rollback) {
                ++rollbacks_seen;
                TEST_CHECK(a.replay_until == e.SimFrontier(),
                           "forced replay targets the frontier");
                TEST_CHECK(e.BeginRollback(a.frame), "forced begin");
                state = snaps[a.frame];
                uint32_t rf = 0;
                uint16_t in[2];
                while (e.NextReplayInputs(&rf, in)) {
                    snaps[rf] = state;
                    e.CommitReplayFrame(rf, state);
                    const uint16_t blob[4] = {in[0], in[1], 0x0101u, 0x00A6u};
                    state = Block64(blob, sizeof(blob), state);
                }
                TEST_CHECK(e.FinishRollback(), "forced finish");
                continue;
            }
            if (a.kind == EngineActionKind::Advance) {
                snaps[a.frame] = state;
                e.CommitAdvance(a.frame, state);
                const uint16_t blob[4] = {a.inputs[0], a.inputs[1], 0x0101u, 0x00A6u};
                state = Block64(blob, sizeof(blob), state);
            }
            break;
        }
    }
    TEST_CHECK(e.Terminal() == EngineTerminal::None, "forced mode stays clean");
    TEST_CHECK(e.SimFrontier() == 40, "forced mode never blocks the sim");
    TEST_CHECK(rollbacks_seen == 39,
               "one forced transaction per advanced frame (none before the first)");
    TEST_CHECK(e.GetStats().max_rollback_depth == 1, "forced depth honored");

    // A real mismatch still wins: the earliest-frame correction runs as a
    // genuine rollback and forcing resumes afterwards.
    RollbackEngine r;
    r.Arm(MakeConfig(0, 0, 8), 1);
    r.SetForcedRollback(2);
    for (uint32_t f = 0; f < 6; ++f) {
        r.CaptureLocalInput(r.SimFrontier(), 0x0001);
        for (int guard = 0; guard < 8; ++guard) {
            const EngineAction a = r.NextAction();
            if (a.kind == EngineActionKind::Rollback) {
                r.BeginRollback(a.frame);
                uint32_t rf = 0;
                uint16_t in[2];
                while (r.NextReplayInputs(&rf, in)) r.CommitReplayFrame(rf, 0);
                r.FinishRollback();
                continue;
            }
            if (a.kind == EngineActionKind::Advance) r.CommitAdvance(a.frame, 0);
            break;
        }
    }
    r.ReceiveRemoteInput(1, 0x0008);   // differs from neutral prediction
    const EngineAction real = r.NextAction();
    TEST_CHECK(real.kind == EngineActionKind::Rollback && real.frame == 1,
               "a real mismatch preempts the forced schedule (earliest wins)");

    // Depth clamps to the epoch frame origin: right after a rotation the
    // forced transaction never crosses into the previous state identity.
    RollbackEngine q;
    q.Arm(MakeConfig(0, 0, 8), 1);
    q.SetForcedRollback(4);
    for (uint32_t f = 0; f < 10; ++f) {
        q.CaptureLocalInput(q.SimFrontier(), 0x0001);
        q.ReceiveRemoteInput(f, 0x0000);
        for (int guard = 0; guard < 8; ++guard) {
            const EngineAction a = q.NextAction();
            if (a.kind == EngineActionKind::Rollback) {
                q.BeginRollback(a.frame);
                uint32_t rf = 0;
                uint16_t in[2];
                while (q.NextReplayInputs(&rf, in)) q.CommitReplayFrame(rf, 0);
                q.FinishRollback();
                continue;
            }
            if (a.kind == EngineActionKind::Advance) q.CommitAdvance(a.frame, 0);
            break;
        }
    }
    TEST_CHECK(q.RotateEpoch(2, 10), "rotation for the clamp pin");
    q.CaptureLocalInput(q.SimFrontier(), 0x0001);
    q.ReceiveRemoteInput(10, 0x0000);
    EngineAction adv = q.NextAction();
    TEST_CHECK(adv.kind == EngineActionKind::Advance,
               "no forced rollback at a zero-length epoch (clamp to origin)");
    q.CommitAdvance(adv.frame, 0);
    q.CaptureLocalInput(q.SimFrontier(), 0x0001);
    const EngineAction post = q.NextAction();
    TEST_CHECK(post.kind == EngineActionKind::Rollback && post.frame == 10,
               "forced depth clamps at the epoch frame origin");
    TEST_CHECK(q.BeginRollback(post.frame), "clamped begin");
    uint32_t rf = 0;
    uint16_t in[2];
    while (q.NextReplayInputs(&rf, in)) q.CommitReplayFrame(rf, 0);
    TEST_CHECK(q.FinishRollback(), "clamped finish");
    TEST_CHECK(q.Terminal() == EngineTerminal::None, "clamp path stays clean");

    // SetForcedRollback(0) disables cleanly.
    q.SetForcedRollback(0);
    q.ReceiveRemoteInput(11, 0x0000);
    adv = q.NextAction();
    TEST_CHECK(adv.kind == EngineActionKind::Advance,
               "depth 0 disables the forced schedule");
}

// ── The matrix ──────────────────────────────────────────────────────────────
//
// RTT mapping at 60 Hz: ~20 ms RTT ≈ 1 frame one-way; ~150 ms RTT ≈ 4-5
// frames one-way; 20±15 ms ≈ +0..2 frames jitter; 150±60 ms ≈ base 3 +0..4.
// Budgets 8 and 12 cover every profile family; forced-rollback-every-frame
// is enabled in 9 of 17 cells, including depth-8 under the 12 budget.

const CellSpec kCells[] = {
    //  name                      loss base jit dup  D  R  force seed
    { "stable-low-r8",              0,  1,  0,  0,  2,  8,  0, 0xD0000001u },
    { "stable-low-r8-force1",       0,  1,  0,  0,  2,  8,  1, 0xD0000002u },
    { "stable-low-r12-force8",      0,  1,  0,  0,  2, 12,  8, 0xD0000003u },
    { "stable-high-r8",             0,  5,  0,  0,  2,  8,  0, 0xD0000004u },
    { "stable-high-r12-force2",     0,  5,  0,  0,  2, 12,  2, 0xD0000005u },
    { "jitter-low-r8-force1",       0,  1,  2,  0,  2,  8,  1, 0xD0000006u },
    { "jitter-low-r12",             0,  1,  2,  0,  2, 12,  0, 0xD0000007u },
    { "jitter-high-r8",             0,  3,  4,  0,  2,  8,  0, 0xD0000008u },
    { "jitter-high-r12-force3",     0,  3,  4,  0,  2, 12,  3, 0xD0000009u },
    { "loss3-r8-force1",            3,  1,  2,  0,  2,  8,  1, 0xD000000Au },
    { "loss3-r12-force2",           3,  1,  2,  0,  2, 12,  2, 0xD000000Bu },
    { "loss10-r8",                 10,  1,  2,  0,  2,  8,  0, 0xD000000Cu },
    { "loss10-r12-force2",         10,  1,  2,  0,  2, 12,  2, 0xD000000Du },
    { "loss-jitter-r8",             5,  3,  4,  0,  2,  8,  0, 0xD000000Eu },
    { "loss-jitter-r12-force8",     5,  3,  4,  0,  2, 12,  8, 0xD000000Fu },
    { "reorder-dup-r8-force1",      0,  1,  3, 20,  2,  8,  1, 0xD0000010u },
    { "reorder-dup-r12",            3,  1,  3, 20,  2, 12,  0, 0xD0000011u },
};

} // namespace

int main() {
    TestForcedRollbackUnit();
    for (const CellSpec& cell : kCells) {
        RunCell(cell);
    }
    std::printf("determinism_tests: %d checks, %d failures\n",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
