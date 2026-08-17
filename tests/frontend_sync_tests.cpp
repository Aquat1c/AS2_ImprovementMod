#include "net/barrier_protocol.h"
#include "net/continue_flow.h"
#include "net/delay_policy.h"
#include "net/frontend_input_sync.h"
#include "net/match_lifecycle.h"
#include "net/player_side_mapping.h"
#include "net/pregame_sync.h"
#include "net/session_manager.h"
#include "net/transition_barrier.h"
#include "net/stagesel_sync.h"
#include "net/stage_watchdog_tracker.h"
#include "net/winscreen_sync.h"
#include "core/game_state.h"
#include "input/input_system.h"
#include "rollback/netplay_log.h"
#include "rollback/online_wiring.h"
#include "rollback/rollback_session.h"
#include "ui/log_window.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace {

struct SentPacket {
    Net::PacketType type;
    std::vector<uint8_t> payload;
};

static std::vector<SentPacket> g_sentPackets;
static bool g_sessionConnected = false;
static Net::SessionRole g_sessionRole = Net::SessionRole::Host;
static Net::ConnectionStats g_sessionStats = {};
static int g_testChecks = 0;
static int g_testFailures = 0;

// Continue-flow / director stub state (rematch handoff cycles).
struct BarrierProposal {
    Net::NetTransitionKind kind;
    uint8_t intent;
};
static std::vector<BarrierProposal> g_barrierProposals;
static bool g_matchOwned = false;
static bool g_lockedConfigAvailable = false;
static int g_beginRematchCalls = 0;
static int g_onRematchCalls = 0;
static uint8_t g_lastExpectedPostMatchIntent = 0;

#define TEST_CHECK(cond, msg)                                                     \
    do {                                                                          \
        ++g_testChecks;                                                           \
        if (!(cond)) {                                                            \
            ++g_testFailures;                                                     \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);         \
        }                                                                         \
    } while (0)

static void ClearSentPackets() {
    g_sentPackets.clear();
}

static const SentPacket* FindLastPacket(Net::PacketType type) {
    for (auto it = g_sentPackets.rbegin(); it != g_sentPackets.rend(); ++it) {
        if (it->type == type) {
            return &*it;
        }
    }
    return nullptr;
}

static bool HasPendingLocalDelayBump(uint16_t* outDelay = nullptr) {
    Net::FrontendInputSyncSnapshot snap{};
    Net::FrontendInputSync_GetSnapshot(&snap);
    if (outDelay) {
        *outDelay = snap.pending_delay;
    }
    return snap.pending_delay_bump;
}

static Net::CharSelFrameInputPayload MakeFrameInput(uint32_t epochId,
                                                    Net::FrontendSyncPhase phase,
                                                    uint32_t frame,
                                                    uint16_t input) {
    Net::CharSelFrameInputPayload payload{};
    payload.epoch_id = epochId;
    payload.phase = (uint16_t)phase;
    payload.phase_id = (uint8_t)Net::FrontendSyncPhaseToPhaseId(phase);
    payload.frame = frame;
    payload.ack_frame = 0;
    payload.input_count = 1;
    payload.inputs[0] = input;
    return payload;
}

static Net::WinScreenFrameInputPayload MakeWinScreenFrameInput(uint32_t epochId,
                                                               uint32_t frame,
                                                               uint16_t input) {
    Net::WinScreenFrameInputPayload payload{};
    payload.epoch_id = epochId;
    payload.phase = (uint16_t)Net::FrontendSyncPhase::WinScreen;
    payload.phase_id = (uint8_t)Net::FrontendPhaseId::WinScreen;
    payload.frame = frame;
    payload.ack_frame = 0;
    payload.input_count = 1;
    payload.inputs[0] = input;
    return payload;
}

static void ResetSubsystems(uint32_t nowMs = 100) {
    ClearSentPackets();
    g_sessionConnected = false;
    g_sessionRole = Net::SessionRole::Host;
    memset(&g_sessionStats, 0, sizeof(g_sessionStats));
    g_barrierProposals.clear();
    g_matchOwned = false;
    g_lockedConfigAvailable = false;
    g_beginRematchCalls = 0;
    g_onRematchCalls = 0;
    g_lastExpectedPostMatchIntent = 0;

    Net::FrontendInputSync_Test_SetClockMs(nowMs);
    Net::ContinueFlow_Shutdown();
    Net::WinScreenSync_Shutdown();
    Net::FrontendInputSync_Shutdown();
    Net::DelayPolicy_Shutdown();
    Net::StageSelSync_Shutdown();

    Net::DelayPolicy_Init();
    Net::FrontendInputSync_Init();
    Net::WinScreenSync_Init();
    Net::StageSelSync_Init();
    Net::ContinueFlow_Init();
    Net::ContinueFlow_Test_SetGameState(0, 0);
}

// M5: the frontend delay is a locally derived, peer-local value (INV-23) —
// no negotiation exists; the epoch begins with the local delay directly.
static void BeginLocalPhase(uint16_t frontendDelay,
                            Net::FrontendSyncPhase phase,
                            Net::PacketType packetType = Net::PacketType::CharSelFrameInput) {
    Net::FrontendInputSync_BeginEpoch(Net::SessionRole::Host, 0x12345678u, frontendDelay, "test epoch");
    Net::FrontendInputSync_BeginInputPhase(phase, packetType, "test phase");
}

static void BeginLocalWinScreen(uint16_t frontendDelay) {
    Net::FrontendInputSync_BeginEpoch(Net::SessionRole::Join, 0x87654321u, frontendDelay, "test winscreen epoch");
    g_sessionRole = Net::SessionRole::Join;
    Net::WinScreenSync_Begin();
}

static void TestJitterAwareLocalDelayDerivation() {
    ResetSubsystems();

    Net::DelayPolicy_UpdateMeasurement(18.0f, 0.0f);
    const int stableProposal = Net::FrontendInputSync_ComputeDelayProposal();

    Net::DelayPolicy_UpdateMeasurement(18.0f, 40.0f);
    const int jitteryProposal = Net::FrontendInputSync_ComputeDelayProposal();

    TEST_CHECK(jitteryProposal > stableProposal,
        "high RTT variance should raise the local frontend delay");

    // M5 (INV-23): the epoch adopts the locally derived value directly —
    // there is no negotiation and no remote input to the number.
    Net::FrontendInputSync_BeginEpoch(Net::SessionRole::Host, 0x100u,
        (uint16_t)jitteryProposal, "local delay adoption test");
    TEST_CHECK(Net::FrontendInputSync_GetFrontendDelay() == (uint16_t)jitteryProposal,
        "the frontend delay should be exactly the locally derived value");
}

static void TestJitterPressureTriggersIncreaseOnlyDelayBump() {
    ResetSubsystems(100);

    Net::DelayPolicy_UpdateMeasurement(18.0f, 0.0f);
    const uint16_t initialDelay = (uint16_t)Net::FrontendInputSync_ComputeDelayProposal();
    BeginLocalPhase(initialDelay, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0);
    ClearSentPackets();

    Net::DelayPolicy_UpdateMeasurement(18.0f, 40.0f);
    Net::FrontendInputSync_Test_SetClockMs(350);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(600);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(850);
    Net::FrontendInputSync_HasInputsForCurrentFrame();

    // M5: the bump is a LOCAL scheduled increase (no wire message, INV-23).
    uint16_t pendingDelay = 0;
    TEST_CHECK(HasPendingLocalDelayBump(&pendingDelay),
        "sustained jitter pressure should schedule a local delay bump");
    TEST_CHECK(pendingDelay > initialDelay,
        "jitter-triggered delay bump should only increase the frontend delay");
}

static void TestStarvationDelayBumpRequiresRemoteFrame() {
    ResetSubsystems(100);

    Net::DelayPolicy_UpdateMeasurement(18.0f, 0.0f);
    BeginLocalPhase(3, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0);
    ClearSentPackets();

    Net::FrontendInputSync_Test_SetClockMs(1100);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(1350);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(1600);
    Net::FrontendInputSync_HasInputsForCurrentFrame();

    TEST_CHECK(!HasPendingLocalDelayBump(),
        "phase-entry starvation before any remote frame should not increase frontend delay");

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::CharSelFrameInputPayload remote0 =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 0, 0);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote0);

    uint16_t local = 0;
    uint16_t remote = 0;
    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "first remote frame should allow frontend frame zero to consume");
    ClearSentPackets();

    Net::FrontendInputSync_Test_SetClockMs(2600);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(2850);
    Net::FrontendInputSync_HasInputsForCurrentFrame();

    TEST_CHECK(HasPendingLocalDelayBump(),
        "starvation after remote traffic has started should still schedule a local delay bump");
}

static void TestOutOfOrderFrontendInputWaitsForMissingCurrentFrame() {
    ResetSubsystems();
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0x0010);

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::CharSelFrameInputPayload future = MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 1, 0x0008);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&future);
    TEST_CHECK(!Net::FrontendInputSync_HasInputsForCurrentFrame(),
        "future remote input must not advance the current frame while the current frame is missing");

    Net::CharSelFrameInputPayload current = MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 0, 0x0020);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&current);
    TEST_CHECK(Net::FrontendInputSync_HasInputsForCurrentFrame(),
        "current remote input should unblock the waiting frontend frame");

    uint16_t local = 0;
    uint16_t remote = 0;
    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "current frontend frame should consume once both peers are present");
    TEST_CHECK(remote == 0x0020,
        "the missing current-frame input must become the first consumed remote input");
    TEST_CHECK(Net::FrontendInputSync_HasInputsForCurrentFrame(),
        "buffered future remote input should become ready after the blocked current frame advances");
    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "the buffered future remote input should consume deterministically on the next frame");
    TEST_CHECK(remote == 0x0008,
        "the buffered future remote input should be preserved for the next consume frame");
}

static void TestCharSelInputUsesNegotiatedFrontendDelay() {
    ResetSubsystems();
    BeginLocalPhase(4, Net::FrontendSyncPhase::CharSel);

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::FrontendInputSync_CaptureLocalInput(0x0040);
    for (uint32_t frame = 0; frame <= 4; frame++) {
        Net::CharSelFrameInputPayload remote =
            MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, frame, 0);
        Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote);
    }

    uint16_t local = 0;
    uint16_t remote = 0;
    for (uint32_t frame = 0; frame < 4; frame++) {
        TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
            "char-select setup frames should consume while waiting for delayed local input");
        TEST_CHECK(local == 0,
            "char-select local input must be blank before the negotiated frontend delay elapses");
    }

    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "char-select delayed local input should consume at the shared delay frame");
    TEST_CHECK(local == 0x0040,
        "char-select local input should appear exactly after the negotiated frontend delay");
}

static void TestFrontendInputBeyondRingDoesNotAliasOrRecover() {
    ResetSubsystems();
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0x0010);

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::CharSelFrameInputPayload alias =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 512, 0x2222);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&alias);

    TEST_CHECK(!Net::FrontendInputSync_HasRecoveryRequest(),
        "remote frontend input beyond the retain window should not request session recovery");
    TEST_CHECK(!Net::FrontendInputSync_HasInputsForCurrentFrame(),
        "future input exactly one ring ahead must not alias the current consume frame");
}

static void TestConsumedLocalFrontendInputRemainsResendableUntilAcked() {
    ResetSubsystems(100);
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0x0010);

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::CharSelFrameInputPayload remote0 =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 0, 0x0040);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote0);

    uint16_t local = 0;
    uint16_t remote = 0;
    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "frontend frame zero should consume before testing resend retention");
    ClearSentPackets();
    Net::FrontendInputSync_Test_SetClockMs(250);

    Net::CharSelFrameInputPayload remote1 =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 1, 0x0080);
    remote1.ack_frame = 0;
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote1);

    const SentPacket* resend = FindLastPacket(Net::PacketType::CharSelFrameInput);
    const auto* payload = resend && resend->payload.size() >= sizeof(Net::CharSelFrameInputPayload)
        ? reinterpret_cast<const Net::CharSelFrameInputPayload*>(resend->payload.data())
        : nullptr;
    TEST_CHECK(payload != nullptr,
        "a peer ack behind our consumed frame should trigger a targeted resend");
    TEST_CHECK(payload && payload->frame == 0,
        "targeted resend should be able to resend the consumed-but-unacked local frame");
}

static void TestFrontendSendAheadCannotOverwritePeerAckWindow() {
    ResetSubsystems(100);
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    for (uint32_t i = 0; i < 296; i++) {
        Net::FrontendInputSync_CaptureLocalInput((uint16_t)(0x1000u + i));
        Net::CharSelFrameInputPayload remote =
            MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, i, (uint16_t)(0x2000u + i));
        remote.ack_frame = i < 39 ? i : 39;
        Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote);

        uint16_t local = 0;
        uint16_t remoteInput = 0;
        TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remoteInput, nullptr),
            "setup should consume contiguous frontend frames before send-ahead cap test");
    }

    ClearSentPackets();
    for (uint32_t i = 0; i < 400; i++) {
        Net::FrontendInputSync_CaptureLocalInput((uint16_t)(0x3000u + i));
    }

    Net::FrontendInputSyncSnapshot snap{};
    Net::FrontendInputSync_GetSnapshot(&snap);
    TEST_CHECK(snap.local_input_frame <= snap.consume_id.frame + snap.max_local_lead,
        "frontend send-ahead must stop at the bounded local lead cap while the current remote frame is blocked");

    Net::FrontendInputSync_Test_SetClockMs(1000);
    Net::CharSelFrameInputPayload remote =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 296, 0x4000);
    remote.ack_frame = 39;
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote);

    const SentPacket* resend = FindLastPacket(Net::PacketType::CharSelFrameInput);
    const auto* payload = resend && resend->payload.size() >= sizeof(Net::CharSelFrameInputPayload)
        ? reinterpret_cast<const Net::CharSelFrameInputPayload*>(resend->payload.data())
        : nullptr;
    TEST_CHECK(payload != nullptr,
        "a far-behind peer ack should trigger a resend after local send-ahead pressure");
    TEST_CHECK(payload && payload->frame == 54,
        "targeted resend should send the newest frame whose history still covers the peer ack");
    TEST_CHECK(payload && payload->input_count == 16,
        "targeted resend history must still include the oldest peer-unacked frame");
}

static void TestStageMergeOpposingDirectionsAndConfirm() {
    ResetSubsystems();
    Net::StageSelSync_Begin();

    const uint16_t opposing = Net::StageSelSync_MergeConfirmed(3, 0x0004, 0x0008);
    TEST_CHECK(opposing == 0,
        "opposing stage directions should cancel out deterministically");

    const uint16_t confirm = Net::StageSelSync_MergeConfirmed(4, 0x0010, 0x0010);
    TEST_CHECK(confirm == 0x0010,
        "same-frame stage confirm should remain deterministic when both peers press confirm");
}

static void TestWinScreenAdvanceReleasesFromEitherPeer() {
    ResetSubsystems();
    BeginLocalWinScreen(2);

    Net::WinScreenSync_CaptureLocalInput(0);
    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::WinScreenFrameInputPayload remoteAdvance =
        MakeWinScreenFrameInput(epochId, 0, INPUT_A);
    Net::WinScreenSync_OnRemoteFrameInput(&remoteAdvance);

    uint16_t p1 = 0;
    uint16_t p2 = 0;
    TEST_CHECK(Net::WinScreenSync_HasInputsForCurrentFrame(),
        "winscreen frame should be ready after local and remote inputs arrive");
    TEST_CHECK(Net::WinScreenSync_ConsumeCurrentFrame(&p1, &p2),
        "winscreen frame should consume deterministically");
    TEST_CHECK((p1 & INPUT_A) != 0 && (p2 & INPUT_A) != 0,
        "a remote winscreen advance should release a synchronized confirm pulse for both game slots");
    TEST_CHECK(!Net::WinScreenSync_LocalConfirmed() && Net::WinScreenSync_RemoteConfirmed(),
        "remote advance should be remembered without requiring local advance");

    Net::WinScreenSync_CaptureLocalInput(0);
    Net::WinScreenFrameInputPayload remoteHold =
        MakeWinScreenFrameInput(epochId, 1, 0);
    Net::WinScreenSync_OnRemoteFrameInput(&remoteHold);

    TEST_CHECK(Net::WinScreenSync_ConsumeCurrentFrame(&p1, &p2),
        "winscreen should keep consuming after the advance gate has released");
    TEST_CHECK((p1 & INPUT_A) != 0 && (p2 & INPUT_A) != 0,
        "released winscreen should keep confirm held until native Mode 9 exits");
    TEST_CHECK(Net::WinScreenSync_RemoteConfirmed() && !Net::WinScreenSync_BothConfirmed(),
        "winscreen should record the actual peer that requested the skip");
}

static void TestPhaseTransitionPreservesDelayAndResetsPhaseCounters() {
    ResetSubsystems();
    BeginLocalPhase(4, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0x0010);

    Net::FrontendInputSyncSnapshot before{};
    Net::FrontendInputSync_GetSnapshot(&before);
    TEST_CHECK(before.frontend_delay == 4,
        "charsel phase should start with the locally derived frontend delay");
    TEST_CHECK(before.local_input_frame > 0,
        "charsel phase should advance local input lead once capture begins");

    Net::FrontendInputSync_BeginInputPhase(Net::FrontendSyncPhase::StageSel,
        Net::PacketType::CharSelFrameInput,
        "stage transition test");

    Net::FrontendInputSyncSnapshot after{};
    Net::FrontendInputSync_GetSnapshot(&after);
    TEST_CHECK(after.phase == Net::FrontendSyncPhase::StageSel,
        "phase transition should advance to stage select explicitly");
    TEST_CHECK(after.frontend_delay == before.frontend_delay,
        "stage select should preserve the frontend delay across the phase edge");
    TEST_CHECK(after.consume_id.frame == 0,
        "phase transition should reset the per-phase consume frame");
    TEST_CHECK(after.local_input_frame == 0,
        "phase transition should reset the local input frame counter");
    TEST_CHECK(after.remote_latest_frame == 0,
        "phase transition should reset only the per-phase remote frame tracking");
}

static void TestStaleEpochAndForeignPhaseIdInputIsIgnored() {
    ResetSubsystems();
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);
    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::FrontendInputSync_CaptureLocalInput(0);

    // Section 3.4: acceptance is keyed on (epoch, phase_id) - a stale epoch
    // is dropped outright (no serials exist to diverge, INV-7).
    Net::CharSelFrameInputPayload staleEpoch =
        MakeFrameInput(epochId - 1u, Net::FrontendSyncPhase::CharSel, 0, 0x0040);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&staleEpoch);
    TEST_CHECK(!Net::FrontendInputSync_HasInputsForCurrentFrame(),
        "stale-epoch frame input must be ignored");

    // A coherent-but-unexpected phase_id is counted (interrogation source),
    // never applied.
    Net::CharSelFrameInputPayload wrongPhase =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 0, 0x0055);
    wrongPhase.phase_id = (uint8_t)Net::FrontendPhaseId::WinScreen;
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&wrongPhase);
    TEST_CHECK(!Net::FrontendInputSync_HasInputsForCurrentFrame(),
        "frame input with a mismatched phase_id must not be applied");

    Net::CharSelFrameInputPayload current =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 0, 0x0080);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&current);

    uint16_t local = 0;
    uint16_t remote = 0;
    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "matching (epoch, phase_id) input should unblock the frontend frame");
    TEST_CHECK(remote == 0x0080,
        "mismatched-identity input must not overwrite current remote input");
}

static void TestStarvationInterrogationAndEscalationLadder() {
    ResetSubsystems();
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0);
    ClearSentPackets();

    // 120 lockstep ticks of zero accepted remote frames -> ResyncRequest
    // (INV-11: interrogate on a live transport, never time out silently).
    for (int i = 0; i < 120; ++i) {
        Net::FrontendInputSync_FrameUpdate();
    }
    const SentPacket* req = FindLastPacket(Net::PacketType::ResyncRequest);
    TEST_CHECK(req != nullptr,
        "120 starved lockstep ticks should emit a ResyncRequest");
    if (req && req->payload.size() >= sizeof(Net::ResyncRequestPayload)) {
        const auto* rp = reinterpret_cast<const Net::ResyncRequestPayload*>(req->payload.data());
        TEST_CHECK(rp->epoch == Net::FrontendInputSync_GetEpochId(),
            "ResyncRequest should carry the current epoch");
        TEST_CHECK(rp->phase_id == (uint8_t)Net::FrontendPhaseId::CharSel,
            "ResyncRequest should carry the current phase_id");
    }

    // A mismatched reply confirms a divergent identity -> Realign escalation.
    Net::ResyncReplyPayload reply{};
    reply.epoch = Net::FrontendInputSync_GetEpochId() + 5u;
    reply.phase_id = (uint8_t)Net::FrontendPhaseId::WinScreen;
    reply.local_frame = 0;
    Net::FrontendInputSync_OnRemoteResyncReply(&reply);
    TEST_CHECK(Net::FrontendInputSync_ConsumeResyncEscalation() ==
                   Net::FrontendResyncEscalation::Realign,
        "an identity-mismatch reply should escalate to an EpochAlign re-run");
    TEST_CHECK(Net::FrontendInputSync_ConsumeResyncEscalation() ==
                   Net::FrontendResyncEscalation::None,
        "the escalation latch should consume exactly once");

    // Three failed interrogation cycles -> pregame restart escalation.
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (int i = 0; i < 120; ++i) {
            Net::FrontendInputSync_FrameUpdate();
        }
    }
    TEST_CHECK(Net::FrontendInputSync_ConsumeResyncEscalation() ==
                   Net::FrontendResyncEscalation::Restart,
        "three failed interrogation cycles should escalate to a pregame restart");

    // A request from the peer is always answered with our identity tuple.
    ClearSentPackets();
    Net::ResyncRequestPayload peerReq{};
    peerReq.epoch = 999;
    peerReq.phase_id = (uint8_t)Net::FrontendPhaseId::StageSel;
    Net::FrontendInputSync_OnRemoteResyncRequest(&peerReq);
    TEST_CHECK(FindLastPacket(Net::PacketType::ResyncReply) != nullptr,
        "a ResyncRequest must always be answered with a ResyncReply");
}

static void TestDuplicateFrontendInputAndStageSyncAreIdempotent() {
    ResetSubsystems();
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0);

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::CharSelFrameInputPayload frame0 = MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 0, 0x0040);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&frame0);
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&frame0);

    uint16_t local = 0;
    uint16_t remote = 0;
    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "duplicate frontend input packets should still allow one deterministic consume");
    TEST_CHECK(remote == 0x0040,
        "duplicate frontend input packets must not mutate the confirmed remote input");
    TEST_CHECK(!Net::FrontendInputSync_HasInputsForCurrentFrame(),
        "duplicate frontend input packets must not manufacture an extra confirmed frame");

    Net::StageWatchdogTrackerState tracker{};
    Net::StageWatchdogTracker_Reset(&tracker);

    Net::StageSyncPayload initial{};
    initial.epoch_id = epochId;
    initial.phase = (uint16_t)Net::FrontendSyncPhase::StageSel;
    initial.frame = 5;
    initial.stage_id = 3;
    initial.stage_cursor = 3;
    TEST_CHECK(Net::StageWatchdogTracker_Apply(&tracker, &initial) == Net::StageWatchdogApplyResult::AcceptedNewer,
        "the first StageSync watchdog packet should be accepted");
    TEST_CHECK(Net::StageWatchdogTracker_Apply(&tracker, &initial) == Net::StageWatchdogApplyResult::Duplicate,
        "duplicate StageSync watchdog packets should be idempotent");

    Net::StageSyncPayload older = initial;
    older.frame = 4;
    older.stage_cursor = 1;
    TEST_CHECK(Net::StageWatchdogTracker_Apply(&tracker, &older) == Net::StageWatchdogApplyResult::IgnoredOutOfOrder,
        "older StageSync watchdog packets must not overwrite newer watchdog state");

    Net::StageSyncPayload upgrade = initial;
    upgrade.confirmed = 1;
    upgrade.committed_stage_id = 3;
    TEST_CHECK(Net::StageWatchdogTracker_Apply(&tracker, &upgrade) == Net::StageWatchdogApplyResult::AcceptedSameFrameUpgrade,
        "same-frame stronger StageSync watchdog packets should be accepted once");

    Net::StageSyncPayload regression = upgrade;
    regression.frame = 6;
    regression.confirmed = 0;
    regression.committed_stage_id = 0;
    TEST_CHECK(Net::StageWatchdogTracker_Apply(&tracker, &regression) == Net::StageWatchdogApplyResult::IgnoredRegression,
        "weaker StageSync watchdog packets must not regress confirmed watchdog truth");
}

static void TestNoLiveDelayDecreaseDuringActivePhase() {
    ResetSubsystems(100);
    BeginLocalPhase(4, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0);
    ClearSentPackets();

    Net::DelayPolicy_UpdateMeasurement(10.0f, 0.0f);
    Net::FrontendInputSync_Test_SetClockMs(250);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(500);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(750);
    Net::FrontendInputSync_HasInputsForCurrentFrame();

    TEST_CHECK(!HasPendingLocalDelayBump(),
        "frontend delay must not auto-decrease during an active phase");
}

static void TestGameplayDelaySharedSafeAndExpertModes() {
    ResetSubsystems();

    Net::DelayPolicy_SetConfiguredDelay(2);
    Net::DelayPolicy_SetGameplayDelayMode(Net::GameplayDelayMode::SharedSafe);

    Net::DelayNegotiationData remote{};
    remote.local_input_delay = 0;
    remote.max_rollback = Net::ROLLBACK_BUDGET_DEFAULT;
    remote.gameplay_delay_mode = Net::GameplayDelayMode::SharedSafe;
    Net::DelayPolicy_NegotiateSession(&remote);

    Net::DelayPolicySnapshot snap{};
    Net::DelayPolicy_GetSnapshot(&snap);
    TEST_CHECK(snap.active_delay == 2,
        "shared-safe mode should apply the larger visible delay locally");
    TEST_CHECK(snap.resolved_visible_remote_delay == 2,
        "shared-safe mode should resolve the remote visible delay to the shared value");
    TEST_CHECK(snap.effective_local_delay == 3 && snap.effective_remote_delay == 3,
        "shared-safe mode should preserve the hidden gameplay floor on both sides");

    Net::DelayPolicy_SetGameplayDelayMode(Net::GameplayDelayMode::AsymmetricExpert);
    remote.gameplay_delay_mode = Net::GameplayDelayMode::AsymmetricExpert;
    Net::DelayPolicy_NegotiateSession(&remote);
    Net::DelayPolicy_GetSnapshot(&snap);

    TEST_CHECK(snap.active_delay == 2,
        "expert mode should keep the local visible delay as configured");
    TEST_CHECK(snap.resolved_visible_remote_delay == 0,
        "expert mode should preserve the peer's announced visible delay");
    TEST_CHECK(snap.effective_local_delay == 3 && snap.effective_remote_delay == 1,
        "expert mode should match the older asymmetric effective-delay behavior");
}

static void TestLocalInputLatchPreservesTapWhileLeadCapped() {
    ResetSubsystems(100);
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);

    Net::FrontendInputSync_CaptureLocalInput(0);
    Net::FrontendInputSync_CaptureLocalInput(0);
    Net::FrontendInputSync_CaptureLocalInput(INPUT_A);
    Net::FrontendInputSync_CaptureLocalInput(0);

    Net::FrontendInputSyncSnapshot capped{};
    Net::FrontendInputSync_GetSnapshot(&capped);
    TEST_CHECK(capped.local_input_frame == capped.consume_id.frame + capped.max_local_lead,
        "local capture should be capped before testing the input latch");

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    Net::CharSelFrameInputPayload remote0 =
        MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, 0, 0);
    remote0.ack_frame = 1;
    Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote0);

    uint16_t local = 0;
    uint16_t remote = 0;
    TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
        "remote frame zero should open one local lead slot");

    Net::FrontendInputSync_CaptureLocalInput(0);

    uint16_t latchedLocal = 0;
    for (uint32_t frame = 1; frame <= 4; frame++) {
        Net::CharSelFrameInputPayload remoteFrame =
            MakeFrameInput(epochId, Net::FrontendSyncPhase::CharSel, frame, 0);
        remoteFrame.ack_frame = frame + 1u;
        Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remoteFrame);

        TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remote, nullptr),
            "frontend frames should consume while checking the latched tap");
        if (frame == 4) {
            latchedLocal = local;
        }
    }

    TEST_CHECK((latchedLocal & INPUT_A) != 0,
        "a short local tap while capture is capped should be emitted once a lead slot opens");
}

static void TestFrontendInputPacketsCarrySixteenFramesOfHistory() {
    ResetSubsystems(100);
    BeginLocalPhase(2, Net::FrontendSyncPhase::CharSel);
    ClearSentPackets();

    for (uint16_t i = 0; i < 20; i++) {
        Net::FrontendInputSync_CaptureLocalInput((uint16_t)(0x100u + i));
        Net::CharSelFrameInputPayload remote =
            MakeFrameInput(Net::FrontendInputSync_GetEpochId(),
                           Net::FrontendSyncPhase::CharSel,
                           i,
                           0);
        remote.ack_frame = (uint32_t)i + 1u;
        Net::FrontendInputSync_OnRemoteCharSelFrameInput(&remote);
        uint16_t local = 0;
        uint16_t remoteInput = 0;
        TEST_CHECK(Net::FrontendInputSync_ConsumeCurrentFrame(&local, &remoteInput, nullptr),
            "history setup should keep the frontend queue moving");
    }

    const SentPacket* packet = FindLastPacket(Net::PacketType::CharSelFrameInput);
    TEST_CHECK(packet != nullptr,
        "capturing frontend input should send a CharSelFrameInput packet");
    TEST_CHECK(packet && packet->payload.size() == sizeof(Net::CharSelFrameInputPayload),
        "frontend input packet payload should match the expanded wire struct size");

    const auto* payload = packet && packet->payload.size() >= sizeof(Net::CharSelFrameInputPayload)
        ? reinterpret_cast<const Net::CharSelFrameInputPayload*>(packet->payload.data())
        : nullptr;
    TEST_CHECK(payload && payload->input_count == 16,
        "frontend input packets should carry sixteen redundant input frames once history is available");
    TEST_CHECK(payload && payload->inputs[0] == 0x113,
        "frontend input history should place the latest input first");
    TEST_CHECK(payload && payload->inputs[15] == 0x104,
        "frontend input history should preserve the sixteenth newest input");
}

// ── Simultaneous navigation over a jittery simulated link ──────────────────
//
// Two peers streaming interleaved frontend inputs concurrently: peer A is
// the module under test; peer B is a deterministic scripted model whose
// frame-input packets arrive through a jittery/reordering/duplicating link
// (the existing test transport stubs capture A's outbound stream). Per
// phase (charsel, stagesel, winscreen) the test asserts:
//   - A consumes B's script byte-identically and in frame order
//   - A's outbound redundant windows are internally consistent and carry
//     exactly the values A consumed for its own side — i.e. B's consumed
//     stream is byte-identical by construction
//   - a clean (delivering) cell never triggers the INV-11 starvation
//     interrogation (zero ResyncRequest packets)

struct TestRng {
    uint32_t s;
    explicit TestRng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t Next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
};

static uint16_t NavWord(TestRng& rng) {
    static const uint16_t words[8] = {
        0, INPUT_LEFT, INPUT_RIGHT, INPUT_UP, INPUT_DOWN, INPUT_A, 0, 0,
    };
    return words[rng.Below(8)];
}

struct JitterFrontendLink {
    struct Entry {
        Net::CharSelFrameInputPayload p;
        uint32_t at;
    };
    std::vector<Entry> q;
    TestRng rng;
    uint32_t base;
    uint32_t jitter;
    bool winscreen_route;

    JitterFrontendLink(uint32_t seed, uint32_t b, uint32_t j, bool ws)
        : rng(seed), base(b), jitter(j), winscreen_route(ws) {}

    void Send(const Net::CharSelFrameInputPayload& p, uint32_t now, bool dup) {
        q.push_back(Entry{p, now + base + rng.Below(jitter + 1)});
        if (dup) {
            q.push_back(Entry{p, now + base + rng.Below(jitter + 1)});
        }
    }

    void DeliverOne(const Net::CharSelFrameInputPayload& p) {
        if (!winscreen_route) {
            Net::FrontendInputSync_OnRemoteCharSelFrameInput(&p);
            return;
        }
        Net::WinScreenFrameInputPayload w{};
        w.epoch_id = p.epoch_id;
        w.phase = p.phase;
        w.phase_id = p.phase_id;
        w.frame = p.frame;
        w.ack_frame = p.ack_frame;
        w.input_count = 1;
        w.inputs[0] = p.inputs[0];
        Net::FrontendInputSync_OnRemoteWinScreenFrameInput(&w);
    }

    void DeliverDue(uint32_t now) {
        for (size_t i = 0; i < q.size();) {
            if ((int32_t)(now - q[i].at) >= 0) {
                DeliverOne(q[i].p);           // swap-remove pop = reorder shim
                q[i] = q.back();
                q.pop_back();
            } else {
                ++i;
            }
        }
    }
};

static void RunSimultaneousNavigationPhase(Net::FrontendSyncPhase phase,
                                           uint32_t frames,
                                           uint32_t seed) {
    const bool winscreenRoute = phase == Net::FrontendSyncPhase::WinScreen;
    const Net::PacketType packetType = winscreenRoute
        ? Net::PacketType::WinScreenFrameInput
        : Net::PacketType::CharSelFrameInput;
    if (winscreenRoute) {
        // Production invariant: the WinScreen input phase is only ever begun
        // by WinScreenSync_Begin (which activates the sync BEFORE the phase).
        // SendInputPacket gates WinScreenFrameInput on WinScreenSync_IsActive,
        // so a raw BeginInputPhase here would silently drop every outbound
        // window and void the coverage assertions below.
        Net::WinScreenSync_Begin();
        TEST_CHECK(Net::WinScreenSync_IsActive(),
            "simnav winscreen leg activated the winscreen sync");
    } else {
        Net::FrontendInputSync_BeginInputPhase(phase, packetType, "simnav phase");
    }
    ClearSentPackets();

    const uint32_t epochId = Net::FrontendInputSync_GetEpochId();
    const uint16_t delay = Net::FrontendInputSync_GetFrontendDelay();
    JitterFrontendLink link(seed, 1, 4, winscreenRoute);
    TestRng remoteScript(seed ^ 0x51D0u);
    TestRng localScript(seed ^ 0x10CAu);

    std::vector<uint16_t> remoteExpected(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        remoteExpected[f] = NavWord(remoteScript);
    }

    std::vector<uint16_t> consumedLocal;
    std::vector<uint16_t> consumedRemote;
    uint32_t nextRemoteSend = 0;
    uint32_t now = 0;
    int64_t guard = (int64_t)frames * 40;   // int64: exhaustion goes negative

    while (consumedRemote.size() < frames && guard-- > 0) {
        ++now;
        Net::FrontendInputSync_Test_SetClockMs(1000 + now * 16);
        link.DeliverDue(now);

        // Peer B streams one frame per tick, jittered; every 7th duplicated.
        if (nextRemoteSend < frames) {
            Net::CharSelFrameInputPayload p = MakeFrameInput(
                epochId, phase, nextRemoteSend, remoteExpected[nextRemoteSend]);
            p.ack_frame = (uint32_t)consumedRemote.size();
            link.Send(p, now, (nextRemoteSend % 7) == 6);
            ++nextRemoteSend;
        }

        // Peer A navigates concurrently (send-ahead capped internally).
        Net::FrontendInputSync_CaptureLocalInput(NavWord(localScript));

        while (consumedRemote.size() < frames &&
               Net::FrontendInputSync_HasInputsForCurrentFrame()) {
            uint16_t l = 0;
            uint16_t r = 0;
            if (!Net::FrontendInputSync_ConsumeCurrentFrame(&l, &r, nullptr)) {
                break;
            }
            consumedLocal.push_back(l);
            consumedRemote.push_back(r);
        }

        Net::FrontendInputSync_FrameUpdate();   // starvation-counter driver
    }

    TEST_CHECK(guard > 0, "simnav phase completed without wedging");
    TEST_CHECK(consumedRemote.size() == frames,
        "simnav phase consumed the full remote stream");

    // A consumed B's script byte-identically, in frame order.
    bool remoteIdentical = consumedRemote.size() == frames;
    for (uint32_t f = 0; f < frames && remoteIdentical; ++f) {
        if (consumedRemote[f] != remoteExpected[f]) remoteIdentical = false;
    }
    TEST_CHECK(remoteIdentical,
        "consumed remote stream is byte-identical to the peer's script "
        "despite jitter/reorder/duplication");

    // Reconstruct what peer B consumes for A's side from A's outbound
    // redundant windows: internally consistent (a frame never resent with a
    // different value) and equal to A's own consumed local stream.
    std::map<uint32_t, uint16_t> sentByFrame;
    bool contradiction = false;
    for (const SentPacket& sp : g_sentPackets) {
        if (sp.type != packetType) continue;
        // CharSel and WinScreen frame-input payloads share the leading
        // layout the reconstruction needs.
        if (sp.payload.size() < sizeof(Net::CharSelFrameInputPayload)) continue;
        const auto* p = reinterpret_cast<const Net::CharSelFrameInputPayload*>(
            sp.payload.data());
        if (p->phase != (uint16_t)phase) continue;
        for (uint16_t i = 0; i < p->input_count; ++i) {
            const uint32_t f = p->frame - i;   // inputs[0] = newest
            const uint16_t v = p->inputs[i];
            auto it = sentByFrame.find(f);
            if (it == sentByFrame.end()) {
                sentByFrame[f] = v;
            } else if (it->second != v) {
                contradiction = true;
            }
        }
    }
    TEST_CHECK(!contradiction,
        "outbound redundant windows never contradict an earlier send");
    bool localIdentical = true;
    uint32_t coveredFrames = 0;
    for (const auto& kv : sentByFrame) {
        if (kv.first >= consumedLocal.size()) continue;   // beyond consume
        ++coveredFrames;
        if (consumedLocal[kv.first] != kv.second) localIdentical = false;
    }
    TEST_CHECK(localIdentical,
        "the peer's reconstructed view of A's stream is byte-identical to "
        "A's consumed local stream");
    TEST_CHECK(coveredFrames + delay >= frames,
        "outbound windows cover every consumed frame past the delay prefix");

    // Clean cell: zero starvation interrogation.
    TEST_CHECK(FindLastPacket(Net::PacketType::ResyncRequest) == nullptr,
        "a delivering (jittery) cell must never trigger the INV-11 interrogation");
    Net::FrontendInputSyncSnapshot snap{};
    Net::FrontendInputSync_GetSnapshot(&snap);
    TEST_CHECK(snap.resync_cycles == 0,
        "no interrogation cycles were consumed in a clean cell");

    if (winscreenRoute) {
        Net::WinScreenSync_Abort();   // production teardown: stops the phase
    } else {
        Net::FrontendInputSync_EndPhase("simnav phase end");
    }
}

static void TestSimultaneousNavigationOverJitteryLink() {
    ResetSubsystems(100);
    Net::FrontendInputSync_BeginEpoch(Net::SessionRole::Host, 0x600u, 3,
        "simnav epoch");
    RunSimultaneousNavigationPhase(Net::FrontendSyncPhase::CharSel, 240, 0x51AA01u);
    RunSimultaneousNavigationPhase(Net::FrontendSyncPhase::StageSel, 160, 0x51AA02u);
    RunSimultaneousNavigationPhase(Net::FrontendSyncPhase::WinScreen, 160, 0x51AA03u);
}

// ── Continue-flow rematch handoff cycles (state-machine level) ─────────────
//
// Drives the REAL continue_flow state machine through >= 3 winscreen ->
// EpochAlign -> next-phase handoffs with mixed YES/NO outcomes, feeding
// ContinueFlow_OnConsumedFrame exclusively through the winscreen lockstep
// consume path (the only legal input source). The game-memory boundary is
// the AS2_FRONTEND_SYNC_TESTING shim; the director side (barrier proposals,
// pregame rematch begin, epoch rebind) is recorded by the test stubs.

// One winscreen lockstep step: capture local, deliver the remote frame, and
// consume (which steps the continue flow). Returns the consumed pair.
static bool StepWinScreenFrame(uint32_t frame, uint16_t localWord,
                               uint16_t remoteWord,
                               uint16_t* outP1, uint16_t* outP2) {
    Net::WinScreenSync_CaptureLocalInput(localWord);
    Net::WinScreenFrameInputPayload remote = MakeWinScreenFrameInput(
        Net::FrontendInputSync_GetEpochId(), frame, remoteWord);
    remote.ack_frame = frame;
    Net::WinScreenSync_OnRemoteFrameInput(&remote);
    return Net::WinScreenSync_ConsumeCurrentFrame(outP1, outP2);
}

static size_t CountProposals(Net::NetTransitionKind kind, uint8_t intent) {
    size_t n = 0;
    for (const BarrierProposal& bp : g_barrierProposals) {
        if (bp.kind == kind && bp.intent == intent) ++n;
    }
    return n;
}

static void TestContinueRematchHandoffCycles() {
    ResetSubsystems(100);
    g_sessionConnected = true;
    g_matchOwned = true;
    g_lockedConfigAvailable = true;

    Net::FrontendInputSync_BeginEpoch(Net::SessionRole::Host, 0x700u, 2,
        "handoff epoch");

    // Cycle outcomes: YES (fast path), NO (charsel route), YES again — the
    // mixed-outcome ladder repeated across three epochs.
    const bool cycleYes[3] = {true, false, true};
    uint32_t epochId = 0x700u;

    for (int cycle = 0; cycle < 3; ++cycle) {
        const bool yes = cycleYes[cycle];
        const size_t proposalsBefore = g_barrierProposals.size();
        const int beginRematchBefore = g_beginRematchCalls;

        // Mode 9 win pose on screen; winscreen lockstep begins (this also
        // resets the continue flow to Idle for the new phase).
        Net::ContinueFlow_Test_SetGameState(MODE_WINSCREEN, STORY_SUB_DIALOGUE_ADV);
        Net::WinScreenSync_Begin();
        TEST_CHECK(Net::WinScreenSync_IsActive(),
            "winscreen lockstep must begin for every cycle");

        uint16_t p1 = 0;
        uint16_t p2 = 0;

        // f0: neutral — flow arms (mode 9, owned match, connected session).
        TEST_CHECK(StepWinScreenFrame(0, 0, 0, &p1, &p2),
            "winscreen frame 0 consumes");
        // f1: remote advance at the win pose — the consumed stream carries
        // the (skip-propagated) advance and the prompt is forced (sub 4).
        TEST_CHECK(StepWinScreenFrame(1, 0, INPUT_A, &p1, &p2),
            "winscreen frame 1 consumes");
        TEST_CHECK(Net::ContinueFlow_IsPromptActive(),
            "advance in the consumed stream at the win pose forces the prompt");
        TEST_CHECK(Net::ContinueFlow_Test_GetSubState() == STORY_SUB_DIALOGUE_END,
            "the prompt owns mode 9 sub 4");

        // f2: confirm still held — the entry carry gate must not lock.
        TEST_CHECK(StepWinScreenFrame(2, 0, INPUT_A, &p1, &p2), "carry frame");
        TEST_CHECK(Net::ContinueFlow_GetLocalChoiceState() ==
                       Net::ContinueChoiceState::Deciding &&
                   Net::ContinueFlow_GetRemoteChoiceState() ==
                       Net::ContinueChoiceState::Deciding,
            "the held confirm at prompt entry never locks a choice");
        // f3: both released.
        TEST_CHECK(StepWinScreenFrame(3, 0, 0, &p1, &p2), "release frame");

        if (yes) {
            // Fresh confirms after the release: the local A captured at
            // step 4 surfaces at consumed frame 6 through the 2-frame
            // frontend delay; the remote A at step 5 locks live. Both sides
            // end LockedYes on frame 6 -> rematch resolution.
            TEST_CHECK(StepWinScreenFrame(4, INPUT_A, 0, &p1, &p2),
                "delay-fill frame consumes");
            TEST_CHECK(StepWinScreenFrame(5, 0, INPUT_A, &p1, &p2),
                "resolution frame consumes");
            // Local A captured at step 4 surfaces at f6; remote locks at f5.
            TEST_CHECK(Net::ContinueFlow_GetRemoteChoiceState() ==
                           Net::ContinueChoiceState::LockedYes,
                "remote fresh confirm locks YES");
            TEST_CHECK(StepWinScreenFrame(6, 0, 0, &p1, &p2),
                "local delayed confirm frame consumes");
            TEST_CHECK(Net::ContinueFlow_IsRematchLatched(),
                "both YES resolves to a rematch latch");
            TEST_CHECK(Net::ContinueFlow_Test_GetSubState() == STORY_SUB_EVENT_SETUP,
                "YES routes the vanilla sub 5 fade");
            TEST_CHECK(g_beginRematchCalls == beginRematchBefore + 1,
                "rematch resolution begins the pregame fast path exactly once");
            TEST_CHECK(g_lastExpectedPostMatchIntent ==
                           (uint8_t)Net::PostMatchIntentWire::Rematch,
                "the director is told the lockstep-derived Rematch intent");
            TEST_CHECK(!Net::WinScreenSync_IsActive(),
                "rematch resolution finalizes the winscreen lockstep");

            // Barrier ladder: WinScreenExit then PostMatchDecision(Rematch).
            size_t exitIdx = SIZE_MAX;
            size_t decisionIdx = SIZE_MAX;
            for (size_t i = proposalsBefore; i < g_barrierProposals.size(); ++i) {
                if (g_barrierProposals[i].kind ==
                        Net::NetTransitionKind::WinScreenExit &&
                    exitIdx == SIZE_MAX) {
                    exitIdx = i;
                }
                if (g_barrierProposals[i].kind ==
                        Net::NetTransitionKind::PostMatchDecision &&
                    g_barrierProposals[i].intent ==
                        (uint8_t)Net::PostMatchIntentWire::Rematch) {
                    decisionIdx = i;
                }
            }
            TEST_CHECK(exitIdx != SIZE_MAX && decisionIdx != SIZE_MAX &&
                           exitIdx < decisionIdx,
                "YES cycle proposes WinScreenExit before PostMatchDecision(Rematch)");

            // EpochAlign commit -> next phase (fast path: straight to the
            // locked pregame; the latch is consumed by mode ownership).
            epochId += 1;
            Net::FrontendInputSync_RebindEpoch(epochId, "epoch align commit (test)");
            Net::ContinueFlow_ConsumeRematchLatch();
            TEST_CHECK(!Net::ContinueFlow_IsRematchLatched(),
                "the rematch latch consumes exactly once");
            TEST_CHECK(Net::ContinueFlow_GetSuppressMask() == 0,
                "no suppression after the latch is consumed");
        } else {
            // f4: remote toggles the cursor (RIGHT -> NO).
            TEST_CHECK(StepWinScreenFrame(4, 0, INPUT_RIGHT, &p1, &p2),
                "cursor toggle frame consumes");
            // f5: remote locks NO; local A captured this step surfaces at f7.
            TEST_CHECK(StepWinScreenFrame(5, INPUT_A, INPUT_A, &p1, &p2),
                "remote lock frame consumes");
            TEST_CHECK(Net::ContinueFlow_GetRemoteChoiceState() ==
                           Net::ContinueChoiceState::LockedNo,
                "remote locks NO after the cursor toggle");
            TEST_CHECK(StepWinScreenFrame(6, 0, 0, &p1, &p2), "gap frame");
            TEST_CHECK(StepWinScreenFrame(7, 0, 0, &p1, &p2),
                "local delayed lock frame consumes");
            TEST_CHECK(!Net::ContinueFlow_IsPromptActive(),
                "any-NO resolves the prompt");
            TEST_CHECK(Net::ContinueFlow_Test_GetSubState() == STORY_SUB_PREMATCH,
                "NO routes the plain sub 8 fade (GAME OVER slide skipped)");
            TEST_CHECK(Net::ContinueFlow_GetSuppressMask() ==
                           (uint16_t)(INPUT_A | INPUT_C),
                "decline holds the A/C carry gate");
            TEST_CHECK(Net::ContinueFlow_ShouldHoldWinScreenFinalize(),
                "decline holds the winscreen finalize until the carry clears");
            TEST_CHECK(g_lastExpectedPostMatchIntent ==
                           (uint8_t)Net::PostMatchIntentWire::CharselRestart,
                "the director is told the lockstep-derived CharselRestart intent");
            TEST_CHECK(CountProposals(Net::NetTransitionKind::PostMatchDecision,
                           (uint8_t)Net::PostMatchIntentWire::CharselRestart) >=
                           1,
                "NO cycle proposes PostMatchDecision(CharselRestart)");
            TEST_CHECK(g_beginRematchCalls == beginRematchBefore,
                "a decline never begins the pregame fast path");

            // Native exit: the sub-8 fade leaves mode 9 -> the winscreen
            // sync aborts (state-machine analog of the FrameUpdate route
            // check) and the exit barrier fires.
            const size_t exitsBefore =
                CountProposals(Net::NetTransitionKind::WinScreenExit, 0);
            Net::ContinueFlow_Test_SetGameState(MODE_CHARSEL, 0);
            Net::WinScreenSync_Abort();
            TEST_CHECK(CountProposals(Net::NetTransitionKind::WinScreenExit, 0) ==
                           exitsBefore + 1,
                "leaving the winscreen proposes the exit barrier");
            TEST_CHECK(!Net::ContinueFlow_ShouldHoldWinScreenFinalize(),
                "the abort path resets the continue flow");

            // EpochAlign commit -> charsel input phase (any-NO route).
            epochId += 1;
            Net::FrontendInputSync_RebindEpoch(epochId, "epoch align commit (test)");
            Net::FrontendInputSync_BeginInputPhase(
                Net::FrontendSyncPhase::CharSel,
                Net::PacketType::CharSelFrameInput,
                "post-decline charsel (test)");
            TEST_CHECK(Net::FrontendInputSync_GetPhase() ==
                           Net::FrontendSyncPhase::CharSel,
                "the NO route lands in a fresh charsel phase");
            Net::FrontendInputSync_EndPhase("post-decline charsel end (test)");
        }
        TEST_CHECK(Net::FrontendInputSync_GetEpochId() == epochId,
            "every handoff adopts the aligned epoch");
    }

    TEST_CHECK(g_onRematchCalls == 2,
        "both YES cycles notified the director rematch hook");
}

} // namespace

namespace Net {

bool BarrierProtocol_SendPacket(PacketType type, const void* payload, size_t len) {
    SentPacket packet{};
    packet.type = type;
    packet.payload.resize(len);
    if (payload && len > 0) {
        memcpy(packet.payload.data(), payload, len);
    }
    g_sentPackets.push_back(std::move(packet));
    return true;
}

bool Session_IsConnected() {
    return g_sessionConnected;
}

// time_probe.cpp (linked since M6 for delay_policy's probe re-feed) sends
// its probes through the raw session send — swallow them here.
bool Session_SendPacket(uint8_t, PacketType, const void*, size_t, bool) {
    return true;
}

SessionRole Session_GetRole() {
    return g_sessionRole;
}

void Session_Cancel() {}

void Session_GetStats(ConnectionStats* out) {
    if (out) {
        *out = g_sessionStats;
    }
}

PregamePhase PregameSync_GetPhase() { return PregamePhase::Idle; }
MatchLifecyclePhase MatchLifecycle_GetPhase() { return MatchLifecyclePhase::Inactive; }

void TransitionBarrier_Propose(NetTransitionKind kind, uint8_t intent, uint32_t) {
    g_barrierProposals.push_back(BarrierProposal{kind, intent});
}

// Continue-flow dependency stubs (the REAL continue_flow.cpp is linked since
// the rematch-handoff cycles test drives its state machine directly).
bool MatchLifecycle_IsMatchOwned() { return g_matchOwned; }
void MatchLifecycle_OnRematch() {}
int PlayerMapping_GetLocalGameSlot() { return 0; }

const LockedMatchConfig* PregameSync_GetLockedConfig() {
    static LockedMatchConfig cfg = [] {
        LockedMatchConfig c{};
        c.p1_character = 3;
        c.p1_palette = 1;
        c.p2_character = 7;
        c.p2_palette = 2;
        c.stage_id = 4;
        c.host_side = 0;
        return c;
    }();
    return g_lockedConfigAvailable ? &cfg : nullptr;
}

bool PregameSync_BeginRematch(const LockedMatchConfig*) {
    ++g_beginRematchCalls;
    return true;
}

} // namespace Net

namespace Rollback {

void OnlineWiring_OnRematch() { ++g_onRematchCalls; }
void OnlineWiring_SetExpectedPostMatchIntent(uint8_t intentWire) {
    g_lastExpectedPostMatchIntent = intentWire;
}

bool RollbackSession_IsActive() {
    return false;
}

void RollbackSession_GetTimesyncTelemetry(RollbackTimesyncTelemetry* out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

void NetplayLog_Init() {}
void NetplayLog_Shutdown() {}
void NetplayLog_SetLogDir(const char*) {}
void NetplayLog_SetVerbose(bool) {}
bool NetplayLog_IsVerbose() { return false; }
void NetplayLog_Write(const char*, int32_t, const char*, ...) {}
void NetplayLog_Verbose(const char*, int32_t, const char*, ...) {}
void NetplayLog_StateChange(const char*, int32_t, const char*, const char*, const char*, const char*) {}
void NetplayLog_ValueChange(const char*, int32_t, const char*, int, int, const char*) {}
void NetplayLog_Flush() {}

} // namespace Rollback

extern "C" {

void LogWindow_SetLogDir(const char*) {}
const char* LogWindow_GetLogDir(void) { return ""; }
void LogWindow_Init(void) {}
void LogWindow_Shutdown(void) {}
void LogWindow_Log(LogLevel, const char*, ...) {}
void LogWindow_LogV(LogLevel, const char*, va_list) {}
void LogWindow_LogCat(LogCategory, LogLevel, const char*, ...) {}
void LogWindow_LogCatV(LogCategory, LogLevel, const char*, va_list) {}
void LogWindow_LogRateLimited(LogLevel, unsigned int, const char*, ...) {}
bool LogWindow_LogOnChange_Int(const char*, int*, int, LogLevel, const char*, ...) { return false; }
bool LogWindow_LogOnChange_Bool(const char*, bool*, bool, LogLevel, const char*, ...) { return false; }
void LogWindow_LogPacket(LogLevel, const char*, ...) {}
void LogWindow_LogPacketV(LogLevel, const char*, va_list) {}
void LogWindow_LogNet(LogLevel, const char*, ...) {}
void LogWindow_LogNetV(LogLevel, const char*, va_list) {}
void LogWindow_SetCategoryEnabled(LogCategory, bool) {}
bool LogWindow_IsCategoryEnabled(LogCategory) { return true; }
void LogWindow_SetAllCategoriesEnabled(bool) {}
const char* LogWindow_GetCategoryName(LogCategory) { return "TEST"; }
void LogWindow_Render(bool*) {}
void LogWindow_Clear(void) {}
int LogWindow_GetCount(void) { return 0; }
void LogWindow_SetMaxEntries(int) {}
void LogWindow_SetAutoScroll(bool) {}
bool LogWindow_GetAutoScroll(void) { return false; }
void LogWindow_SetMinLevel(LogLevel) {}
LogLevel LogWindow_GetMinLevel(void) { return LOG_DEBUG; }
bool LogWindow_WriteToFile(const char*) { return false; }
void LogWindow_RenderContent(void) {}

} // extern "C"

int main() {
    std::printf("Running frontend sync tests...\n");

    TestJitterAwareLocalDelayDerivation();
    TestJitterPressureTriggersIncreaseOnlyDelayBump();
    TestStarvationDelayBumpRequiresRemoteFrame();
    TestOutOfOrderFrontendInputWaitsForMissingCurrentFrame();
    TestCharSelInputUsesNegotiatedFrontendDelay();
    TestFrontendInputBeyondRingDoesNotAliasOrRecover();
    TestConsumedLocalFrontendInputRemainsResendableUntilAcked();
    TestFrontendSendAheadCannotOverwritePeerAckWindow();
    TestStageMergeOpposingDirectionsAndConfirm();
    TestWinScreenAdvanceReleasesFromEitherPeer();
    TestPhaseTransitionPreservesDelayAndResetsPhaseCounters();
    TestStaleEpochAndForeignPhaseIdInputIsIgnored();
    TestStarvationInterrogationAndEscalationLadder();
    TestDuplicateFrontendInputAndStageSyncAreIdempotent();
    TestNoLiveDelayDecreaseDuringActivePhase();
    TestGameplayDelaySharedSafeAndExpertModes();
    TestLocalInputLatchPreservesTapWhileLeadCapped();
    TestFrontendInputPacketsCarrySixteenFramesOfHistory();
    TestSimultaneousNavigationOverJitteryLink();
    TestContinueRematchHandoffCycles();

    Net::FrontendInputSync_Test_ClearClockOverride();
    Net::ContinueFlow_Shutdown();
    Net::WinScreenSync_Shutdown();
    Net::FrontendInputSync_Shutdown();
    Net::DelayPolicy_Shutdown();
    Net::StageSelSync_Shutdown();

    std::printf("Frontend sync tests: %d checks, %d failures\n", g_testChecks, g_testFailures);
    return g_testFailures == 0 ? 0 : 1;
}
