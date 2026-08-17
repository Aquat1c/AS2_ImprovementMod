/**
 * Alice Senki 2 - TransitionBarrier unit tests (re0.7 M6)
 *
 * Closes the M5 gate deviation ("EpochAlign unit coverage"): a dedicated
 * target that links the REAL src/net/transition_barrier.cpp with stubbed
 * transport/logging, driving the remote side by injecting wire payloads
 * through TransitionBarrier_OnPacket and observing our sends via the
 * Session_SendPacket stub.
 *
 * Pins:
 *   T-TB-1  plain barrier commit: propose + remote proposal + ack of our seq
 *   T-TB-2  EpochAlign commit rule (INV-10): {epoch, first_phase} must match
 *           on both sides; mismatch refuses commit until a matching
 *           re-proposal arrives
 *   T-TB-3  ack echoes the remote proposal's align fields VERBATIM (INV-13)
 *   T-TB-4  idempotent re-ack of duplicate proposals; stale acks ignored
 *   T-TB-5  Clear semantics: a cleared slot forgets local+remote state; a
 *           stale remote proposal alone can never commit
 *   T-TB-6  ConsumeCommit fires exactly once and resets the slot
 *   T-TB-7  match-end ladder kinds commit under EVERY arrival order of the
 *           three steps' proposals/acks (the barrier half of T-LADDER; the
 *           director's consume-order gate lives in match_director and is
 *           exercised in-game)
 */

#include "net/transition_barrier.h"
#include "net/protocol.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ui/log_window.h"

static int s_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++s_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Stubs ───────────────────────────────────────────────────────────────────

struct SentPacket {
    Net::PacketType type;
    Net::PhaseTransitionPayload payload;
};
static std::vector<SentPacket> s_sent;

namespace Net {

bool Session_SendPacket(uint8_t /*channel*/, PacketType type,
                        const void* payload, size_t payloadLen, bool /*reliable*/) {
    if (payloadLen == sizeof(PhaseTransitionPayload)) {
        SentPacket sp{};
        sp.type = type;
        memcpy(&sp.payload, payload, sizeof(sp.payload));
        s_sent.push_back(sp);
    }
    return true;
}

} // namespace Net

namespace Rollback {
void NetplayLog_Write(const char*, int32_t, const char*, ...) {}
} // namespace Rollback

extern "C" {
void LogWindow_Log(LogLevel, const char*, ...) {}
void LogWindow_LogCat(LogCategory, LogLevel, const char*, ...) {}
bool LogWindow_IsCategoryEnabled(LogCategory) { return false; }
void LogWindow_LogNet(LogLevel, const char*, ...) {}
void LogWindow_LogRateLimited(LogLevel, unsigned int, const char*, ...) {}
} // extern "C"

// ── Helpers ─────────────────────────────────────────────────────────────────

using Net::NetTransitionKind;
using Net::PacketType;
using Net::PhaseTransitionPayload;

static const PhaseTransitionPayload* LastSentOfType(PacketType type) {
    for (auto it = s_sent.rbegin(); it != s_sent.rend(); ++it) {
        if (it->type == type) return &it->payload;
    }
    return nullptr;
}

static size_t CountSentOfType(PacketType type) {
    size_t n = 0;
    for (const auto& sp : s_sent) {
        if (sp.type == type) ++n;
    }
    return n;
}

static void InjectRemoteProposal(NetTransitionKind kind, uint32_t seq,
                                 uint8_t intent = 0, uint32_t epoch = 0,
                                 uint8_t firstPhase = 0, uint8_t nativeMode = 0) {
    PhaseTransitionPayload p{};
    p.transition_seq = seq;
    p.kind = (uint8_t)kind;
    p.intent = intent;
    p.epoch = epoch;
    p.first_phase = firstPhase;
    p.native_mode = nativeMode;
    Net::TransitionBarrier_OnPacket(PacketType::PhaseTransitionProposal, &p, sizeof(p));
}

static void AckOurLastProposal(NetTransitionKind kind) {
    // Find our newest outbound proposal of this kind and ack its seq.
    for (auto it = s_sent.rbegin(); it != s_sent.rend(); ++it) {
        if (it->type == PacketType::PhaseTransitionProposal &&
            it->payload.kind == (uint8_t)kind) {
            PhaseTransitionPayload ack = it->payload;
            Net::TransitionBarrier_OnPacket(PacketType::PhaseTransitionAck,
                                            &ack, sizeof(ack));
            return;
        }
    }
    std::printf("FAIL: no outbound proposal of kind %u to ack\n", (unsigned)kind);
    ++s_failures;
}

static void FreshBarrier() {
    s_sent.clear();
    Net::TransitionBarrier_Reset("test");
}

// ── Tests ───────────────────────────────────────────────────────────────────

static void TestPlainCommit() {
    FreshBarrier();
    Net::TransitionBarrier_Propose(NetTransitionKind::GameplayStart, 0, 42);
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::GameplayStart));
    InjectRemoteProposal(NetTransitionKind::GameplayStart, 100);
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::GameplayStart));
    AckOurLastProposal(NetTransitionKind::GameplayStart);
    CHECK(Net::TransitionBarrier_IsCommitted(NetTransitionKind::GameplayStart));
}

static void TestEpochAlignCommitRule() {
    FreshBarrier();
    // Local proposes {epoch 5, CharSel}; remote proposes {epoch 5, None}:
    // commit must be REFUSED (INV-10) even with both proposed + acked.
    Net::TransitionBarrier_ProposeEpochAlign(5, 1 /*CharSel*/, 6, 42);
    InjectRemoteProposal(NetTransitionKind::EpochAlign, 200, 0, 5, 0 /*None*/, 9);
    AckOurLastProposal(NetTransitionKind::EpochAlign);
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::EpochAlign));

    // Matching re-proposal from the remote → commit forms.
    InjectRemoteProposal(NetTransitionKind::EpochAlign, 201, 0, 5, 1 /*CharSel*/, 9);
    CHECK(Net::TransitionBarrier_IsCommitted(NetTransitionKind::EpochAlign));

    // Remote payload is readable while proposed.
    uint32_t e = 0; uint8_t f = 0, m = 0;
    CHECK(Net::TransitionBarrier_GetRemoteEpochAlign(&e, &f, &m));
    CHECK(e == 5 && f == 1 && m == 9);
}

static void TestEpochMismatchRefusal() {
    FreshBarrier();
    // Different EPOCH with same first_phase also refuses.
    Net::TransitionBarrier_ProposeEpochAlign(7, 1, 6, 42);
    InjectRemoteProposal(NetTransitionKind::EpochAlign, 300, 0, 8, 1, 6);
    AckOurLastProposal(NetTransitionKind::EpochAlign);
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::EpochAlign));
    // Local adoption of the higher epoch (match_setup behavior) re-proposes
    // and the commit forms.
    Net::TransitionBarrier_ProposeEpochAlign(8, 1, 6, 42);
    AckOurLastProposal(NetTransitionKind::EpochAlign);
    CHECK(Net::TransitionBarrier_IsCommitted(NetTransitionKind::EpochAlign));
}

static void TestAckEchoesVerbatim() {
    FreshBarrier();
    const size_t acksBefore = CountSentOfType(PacketType::PhaseTransitionAck);
    InjectRemoteProposal(NetTransitionKind::EpochAlign, 400, 0, 12345, 3, 9);
    CHECK(CountSentOfType(PacketType::PhaseTransitionAck) == acksBefore + 1);
    const PhaseTransitionPayload* ack = LastSentOfType(PacketType::PhaseTransitionAck);
    CHECK(ack != nullptr);
    if (ack) {
        // INV-13: the align fields are echoed from the RECEIVED bytes.
        CHECK(ack->transition_seq == 400);
        CHECK(ack->epoch == 12345);
        CHECK(ack->first_phase == 3);
        CHECK(ack->native_mode == 9);
    }
}

static void TestIdempotentReAckAndStaleAck() {
    FreshBarrier();
    InjectRemoteProposal(NetTransitionKind::WinScreenExit, 500);
    InjectRemoteProposal(NetTransitionKind::WinScreenExit, 500);   // duplicate
    CHECK(CountSentOfType(PacketType::PhaseTransitionAck) == 2);   // re-acked both times
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::WinScreenExit));

    // A stale ack (wrong seq) never satisfies our proposal.
    Net::TransitionBarrier_Propose(NetTransitionKind::WinScreenExit, 0, 0);
    PhaseTransitionPayload staleAck{};
    staleAck.transition_seq = 0xDEAD;
    staleAck.kind = (uint8_t)NetTransitionKind::WinScreenExit;
    Net::TransitionBarrier_OnPacket(PacketType::PhaseTransitionAck,
                                    &staleAck, sizeof(staleAck));
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::WinScreenExit));
    AckOurLastProposal(NetTransitionKind::WinScreenExit);
    CHECK(Net::TransitionBarrier_IsCommitted(NetTransitionKind::WinScreenExit));
}

static void TestClearSemantics() {
    FreshBarrier();
    Net::TransitionBarrier_Propose(NetTransitionKind::GameplayStart, 0, 0);
    InjectRemoteProposal(NetTransitionKind::GameplayStart, 600);
    AckOurLastProposal(NetTransitionKind::GameplayStart);
    CHECK(Net::TransitionBarrier_IsCommitted(NetTransitionKind::GameplayStart));

    // Clear wipes local AND remote state: the stale commit is gone and a
    // replayed stale remote proposal alone must never re-commit (the M5-4
    // "stale proposal from a previous match" guard).
    Net::TransitionBarrier_Clear(NetTransitionKind::GameplayStart, "test clear");
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::GameplayStart));
    CHECK(!Net::TransitionBarrier_RemoteProposed(NetTransitionKind::GameplayStart));
    InjectRemoteProposal(NetTransitionKind::GameplayStart, 600);
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::GameplayStart));
}

static void TestConsumeCommitOnce() {
    FreshBarrier();
    Net::TransitionBarrier_Propose(NetTransitionKind::PostMatchDecision, 1, 0);
    InjectRemoteProposal(NetTransitionKind::PostMatchDecision, 700, 1);
    AckOurLastProposal(NetTransitionKind::PostMatchDecision);
    CHECK(Net::TransitionBarrier_IsCommitted(NetTransitionKind::PostMatchDecision));
    CHECK(Net::TransitionBarrier_ConsumeCommit(NetTransitionKind::PostMatchDecision));
    CHECK(!Net::TransitionBarrier_ConsumeCommit(NetTransitionKind::PostMatchDecision));
    CHECK(!Net::TransitionBarrier_IsCommitted(NetTransitionKind::PostMatchDecision));
}

// The barrier half of T-LADDER: the three match-end steps' remote events can
// arrive in every order; each barrier still commits (the director's gate
// then enforces consume ORDER on top — not linkable here).
static void TestLadderKindsCommitUnderAllArrivalOrders() {
    const NetTransitionKind kinds[3] = {
        NetTransitionKind::WinScreenExit,
        NetTransitionKind::PostMatchDecision,
        NetTransitionKind::EpochAlign,
    };
    const int orders[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
    };
    for (int o = 0; o < 6; ++o) {
        FreshBarrier();
        // Local side proposes all three (resolution reached).
        Net::TransitionBarrier_Propose(NetTransitionKind::WinScreenExit, 0, 0);
        Net::TransitionBarrier_Propose(NetTransitionKind::PostMatchDecision, 0, 0);
        Net::TransitionBarrier_ProposeEpochAlign(9, 0 /*None: fast path*/, 9, 0);
        // Remote events land in permuted order.
        for (int i = 0; i < 3; ++i) {
            const NetTransitionKind k = kinds[orders[o][i]];
            if (k == NetTransitionKind::EpochAlign) {
                InjectRemoteProposal(k, 800 + o * 10 + i, 0, 9, 0, 9);
            } else {
                InjectRemoteProposal(k, 800 + o * 10 + i);
            }
            AckOurLastProposal(k);
        }
        for (int i = 0; i < 3; ++i) {
            CHECK(Net::TransitionBarrier_IsCommitted(kinds[i]));
        }
    }
}

int main() {
    std::printf("Running transition_barrier tests...\n");
    Net::TransitionBarrier_Init();

    TestPlainCommit();
    TestEpochAlignCommitRule();
    TestEpochMismatchRefusal();
    TestAckEchoesVerbatim();
    TestIdempotentReAckAndStaleAck();
    TestClearSemantics();
    TestConsumeCommitOnce();
    TestLadderKindsCommitUnderAllArrivalOrders();

    if (s_failures == 0) {
        std::printf("All transition_barrier tests PASSED\n");
        return 0;
    }
    std::printf("%d transition_barrier check(s) FAILED\n", s_failures);
    return 1;
}
