#include "net/barrier_protocol.h"
#include "net/delay_policy.h"
#include "net/frontend_input_sync.h"
#include "net/session_manager.h"
#include "net/stagesel_sync.h"
#include "net/stage_watchdog_tracker.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"
#include "ui/log_window.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

struct SentPacket {
    Net::PacketType type;
    std::vector<uint8_t> payload;
};

static std::vector<SentPacket> g_sentPackets;
static bool g_sessionConnected = false;
static Net::ConnectionStats g_sessionStats = {};
static int g_testChecks = 0;
static int g_testFailures = 0;

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

static const Net::DelayChangeReqPayload* FindLastDelayReq() {
    const SentPacket* packet = FindLastPacket(Net::PacketType::DelayChangeReq);
    if (!packet || packet->payload.size() < sizeof(Net::DelayChangeReqPayload)) {
        return nullptr;
    }
    return reinterpret_cast<const Net::DelayChangeReqPayload*>(packet->payload.data());
}

static Net::CharSelFrameInputPayload MakeFrameInput(uint32_t epochId,
                                                    Net::FrontendSyncPhase phase,
                                                    uint32_t frame,
                                                    uint16_t input) {
    Net::CharSelFrameInputPayload payload{};
    payload.epoch_id = epochId;
    payload.phase = (uint16_t)phase;
    payload.frame = frame;
    payload.ack_frame = 0;
    payload.input_count = 1;
    payload.inputs[0] = input;
    return payload;
}

static void ResetSubsystems(uint32_t nowMs = 100) {
    ClearSentPackets();
    g_sessionConnected = false;
    memset(&g_sessionStats, 0, sizeof(g_sessionStats));

    Net::FrontendInputSync_Test_SetClockMs(nowMs);
    Net::FrontendInputSync_Shutdown();
    Net::DelayPolicy_Shutdown();
    Net::StageSelSync_Shutdown();

    Net::DelayPolicy_Init();
    Net::FrontendInputSync_Init();
    Net::StageSelSync_Init();
}

static void BeginNegotiatedPhase(uint16_t sharedDelay,
                                 Net::FrontendSyncPhase phase,
                                 Net::PacketType packetType = Net::PacketType::CharSelFrameInput) {
    Net::FrontendInputSync_BeginEpoch(Net::SessionRole::Host, 0x12345678u, sharedDelay, "test epoch");
    Net::FrontendInputSync_OnRemoteSyncAnnounce(sharedDelay, "test remote announce");
    TEST_CHECK(Net::FrontendInputSync_FinalizeDelayNegotiation("test finalize"),
        "frontend delay negotiation should succeed");
    Net::FrontendInputSync_BeginInputPhase(phase, packetType, "test phase");
}

static void TestJitterAwareProposalAndSharedNegotiation() {
    ResetSubsystems();

    Net::DelayPolicy_UpdateMeasurement(18.0f, 0.0f);
    const int stableProposal = Net::FrontendInputSync_ComputeDelayProposal();

    Net::DelayPolicy_UpdateMeasurement(18.0f, 20.0f);
    const int jitteryProposal = Net::FrontendInputSync_ComputeDelayProposal();

    TEST_CHECK(jitteryProposal > stableProposal,
        "high RTT variance should raise the local frontend delay proposal");

    Net::FrontendInputSync_BeginEpoch(Net::SessionRole::Host, 0x100u,
        (uint16_t)stableProposal, "asymmetric proposal test");
    Net::FrontendInputSync_OnRemoteSyncAnnounce((uint16_t)jitteryProposal, "remote proposal");
    TEST_CHECK(Net::FrontendInputSync_FinalizeDelayNegotiation("negotiate asymmetric"),
        "asymmetric frontend proposals should negotiate successfully");
    TEST_CHECK(Net::FrontendInputSync_GetSharedDelay() == (uint16_t)std::max(stableProposal, jitteryProposal),
        "shared frontend delay should be the max of local and remote recommendations");
}

static void TestJitterPressureTriggersIncreaseOnlyDelayBump() {
    ResetSubsystems(100);

    Net::DelayPolicy_UpdateMeasurement(18.0f, 0.0f);
    const uint16_t initialDelay = (uint16_t)Net::FrontendInputSync_ComputeDelayProposal();
    BeginNegotiatedPhase(initialDelay, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0);
    ClearSentPackets();

    Net::DelayPolicy_UpdateMeasurement(18.0f, 20.0f);
    Net::FrontendInputSync_Test_SetClockMs(350);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(600);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(850);
    Net::FrontendInputSync_HasInputsForCurrentFrame();

    const Net::DelayChangeReqPayload* req = FindLastDelayReq();
    TEST_CHECK(req != nullptr, "sustained jitter pressure should emit a delay bump request");
    if (req) {
        TEST_CHECK(req->new_delay > initialDelay,
            "jitter-triggered delay bump should only increase the shared delay");
        TEST_CHECK(req->reason_code == (uint8_t)Net::FrontendDelayBumpReason::JitterPressure,
            "jitter-triggered delay bump should carry the jitter pressure reason");
    }
}

static void TestOutOfOrderFrontendInputWaitsForMissingCurrentFrame() {
    ResetSubsystems();
    BeginNegotiatedPhase(2, Net::FrontendSyncPhase::CharSel);
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

static void TestPhaseTransitionPreservesSharedDelayAndResetsPhaseCounters() {
    ResetSubsystems();
    BeginNegotiatedPhase(4, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0x0010);

    Net::FrontendInputSyncSnapshot before{};
    Net::FrontendInputSync_GetSnapshot(&before);
    TEST_CHECK(before.shared_delay == 4,
        "charsel phase should start with the negotiated shared delay");
    TEST_CHECK(before.local_input_frame > 0,
        "charsel phase should advance local input lead once capture begins");

    Net::FrontendInputSync_BeginInputPhase(Net::FrontendSyncPhase::StageSel,
        Net::PacketType::CharSelFrameInput,
        "stage transition test");

    Net::FrontendInputSyncSnapshot after{};
    Net::FrontendInputSync_GetSnapshot(&after);
    TEST_CHECK(after.phase == Net::FrontendSyncPhase::StageSel,
        "phase transition should advance to stage select explicitly");
    TEST_CHECK(after.shared_delay == before.shared_delay,
        "stage select should preserve the negotiated shared frontend delay");
    TEST_CHECK(after.consume_id.frame == 0,
        "phase transition should reset the per-phase consume frame");
    TEST_CHECK(after.local_input_frame == 0,
        "phase transition should reset the local input frame counter");
    TEST_CHECK(after.remote_latest_frame == 0,
        "phase transition should reset only the per-phase remote frame tracking");
}

static void TestDuplicateFrontendInputAndStageSyncAreIdempotent() {
    ResetSubsystems();
    BeginNegotiatedPhase(2, Net::FrontendSyncPhase::CharSel);
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
    BeginNegotiatedPhase(4, Net::FrontendSyncPhase::CharSel);
    Net::FrontendInputSync_CaptureLocalInput(0);
    ClearSentPackets();

    Net::DelayPolicy_UpdateMeasurement(10.0f, 0.0f);
    Net::FrontendInputSync_Test_SetClockMs(250);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(500);
    Net::FrontendInputSync_HasInputsForCurrentFrame();
    Net::FrontendInputSync_Test_SetClockMs(750);
    Net::FrontendInputSync_HasInputsForCurrentFrame();

    TEST_CHECK(FindLastDelayReq() == nullptr,
        "frontend delay must not auto-decrease during an active phase");
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

void Session_GetStats(ConnectionStats* out) {
    if (out) {
        *out = g_sessionStats;
    }
}

} // namespace Net

namespace Rollback {

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
void LogWindow_LogGekko(LogLevel, const char*, ...) {}
void LogWindow_LogGekkoV(LogLevel, const char*, va_list) {}
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

    TestJitterAwareProposalAndSharedNegotiation();
    TestJitterPressureTriggersIncreaseOnlyDelayBump();
    TestOutOfOrderFrontendInputWaitsForMissingCurrentFrame();
    TestStageMergeOpposingDirectionsAndConfirm();
    TestPhaseTransitionPreservesSharedDelayAndResetsPhaseCounters();
    TestDuplicateFrontendInputAndStageSyncAreIdempotent();
    TestNoLiveDelayDecreaseDuringActivePhase();

    Net::FrontendInputSync_Test_ClearClockOverride();
    Net::FrontendInputSync_Shutdown();
    Net::DelayPolicy_Shutdown();
    Net::StageSelSync_Shutdown();

    std::printf("Frontend sync tests: %d checks, %d failures\n", g_testChecks, g_testFailures);
    return g_testFailures == 0 ? 0 : 1;
}