/**
 * Alice Senki 2 - SyncTrace diagnostics
 */

#include "net/sync_trace.h"

#include "diagnostics/async_log.h"
#include "net/barrier_protocol.h"
#include "net/delay_policy.h"
#include "net/match_lifecycle.h"
#include "net/netplay_phase_runtime.h"
#include "net/player_side_mapping.h"
#include "net/session_manager.h"
#include "net/sync_policy.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_debug.h"
#include "rollback/rollback_session.h"
#include "core/as2_constants.h"
#include "core/game_state.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string.h>

namespace Net {
namespace {

constexpr uint16_t kSyncTraceSchema = 1;
constexpr uint32_t kPairSlots = 256;
constexpr int32_t kIntegrityTraceInterval = 60;
constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

struct TracePair {
    bool has_local;
    bool has_remote;
    bool compared;
    SyncTracePayload local;
    SyncTracePayload remote;
};

static bool s_initialized = false;
static bool s_enabled = false;
static bool s_integrityMode = false;
static bool s_fullCrc = false;
static uint32_t s_seq = 0;
static int32_t s_lastGameplayTraceFrame = -1;
static TracePair s_pairs[kPairSlots] = {};

static bool IsCompareActive() {
    return s_initialized && (s_enabled || s_integrityMode);
}

static bool ShouldWriteCsv() {
    return s_enabled;
}

static bool EnvFlagEnabled(const char* name) {
    char value[32] = {};
    const DWORD len = GetEnvironmentVariableA(name, value, (DWORD)sizeof(value));
    if (len == 0 || len >= sizeof(value)) {
        return false;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "on") == 0 ||
           strcmp(value, "ON") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0;
}

static uint8_t ClampU8(int value) {
    if (value < 0) {
        return 0;
    }
    return value > 255 ? 255 : (uint8_t)value;
}

static uint8_t PlayerSlotByte(int slot) {
    return (slot == 0 || slot == 1) ? (uint8_t)slot : 0xFFu;
}

static uint32_t FnvMixByte(uint32_t hash, uint8_t value) {
    hash ^= value;
    hash *= kFnvPrime;
    return hash;
}

static uint32_t FnvMix32(uint32_t hash, uint32_t value) {
    hash = FnvMixByte(hash, (uint8_t)(value & 0xFF));
    hash = FnvMixByte(hash, (uint8_t)((value >> 8) & 0xFF));
    hash = FnvMixByte(hash, (uint8_t)((value >> 16) & 0xFF));
    hash = FnvMixByte(hash, (uint8_t)((value >> 24) & 0xFF));
    return hash;
}

static uint32_t TraceKeyFrame(const SyncTracePayload& p) {
    const auto domain = (SyncTraceDomain)p.domain;
    if (domain == SyncTraceDomain::FrontendLockstep) {
        return p.frontend_frame;
    }
    if (domain == SyncTraceDomain::GameplayRollback) {
        return p.rb_frame >= 0 ? (uint32_t)p.rb_frame : (uint32_t)p.game_abs_frame;
    }
    return p.seq;
}

static uint32_t TraceKeyHash(const SyncTracePayload& p) {
    uint32_t hash = kFnvOffset;
    hash = FnvMix32(hash, p.epoch_id);
    hash = FnvMix32(hash, TraceKeyFrame(p));
    hash = FnvMixByte(hash, p.domain);
    hash = FnvMixByte(hash, p.frontend_phase);
    hash = FnvMixByte(hash, p.rollback_phase);
    return hash;
}

static bool SameTraceKey(const SyncTracePayload& a, const SyncTracePayload& b) {
    return a.domain == b.domain &&
           a.epoch_id == b.epoch_id &&
           a.frontend_phase == b.frontend_phase &&
           a.rollback_phase == b.rollback_phase &&
           TraceKeyFrame(a) == TraceKeyFrame(b);
}

static TracePair* FindPair(const SyncTracePayload& p, bool create) {
    const uint32_t hash = TraceKeyHash(p);
    TracePair* slot = &s_pairs[hash & (kPairSlots - 1)];

    if ((slot->has_local && SameTraceKey(slot->local, p)) ||
        (slot->has_remote && SameTraceKey(slot->remote, p))) {
        return slot;
    }

    if (!create) {
        return nullptr;
    }

    memset(slot, 0, sizeof(*slot));
    return slot;
}

static uint16_t InputForSlot(const SyncTracePayload& p, uint8_t slot) {
    if (p.local_player == slot) {
        return p.local_input;
    }
    if (p.remote_player == slot) {
        return p.remote_input;
    }
    return 0;
}

static uint32_t FrontendStateCrc(uint32_t epochId,
                                 FrontendSyncPhase phase,
                                 uint32_t frame,
                                 uint16_t p1Input,
                                 uint16_t p2Input) {
    uint32_t hash = kFnvOffset;
    hash = FnvMix32(hash, epochId);
    hash = FnvMix32(hash, (uint32_t)phase);
    hash = FnvMix32(hash, frame);
    hash = FnvMix32(hash, p1Input);
    hash = FnvMix32(hash, p2Input);
    return hash;
}

static void InputsByGameSlot(uint16_t localInput,
                             uint16_t remoteInput,
                             uint16_t* outP1,
                             uint16_t* outP2) {
    const int localSlot = PlayerMapping_GetLocalGameSlot();
    const int remoteSlot = PlayerMapping_GetRemoteGameSlot();

    if (localSlot == 0 && remoteSlot == 1) {
        *outP1 = localInput;
        *outP2 = remoteInput;
        return;
    }
    if (localSlot == 1 && remoteSlot == 0) {
        *outP1 = remoteInput;
        *outP2 = localInput;
        return;
    }

    *outP1 = localInput;
    *outP2 = remoteInput;
}

static void AppendTraceCsv(const char* source,
                           const SyncTracePayload& p) {
    if (!ShouldWriteCsv()) {
        return;
    }
    char row[1024];
    snprintf(row,
             sizeof(row),
             "%s,%s,%u,%u,%s,%s,%s,%s,%s,%s,%s,%d,%d,%u,0x%04X,0x%04X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,0x%08X,0x%08X,0x%08X,0x%04X",
             source ? source : "?",
             SyncTraceDomainName((SyncTraceDomain)p.domain),
             p.epoch_id,
             p.seq,
             GameModeName(p.native_mode),
             NativeSubstateName(p.native_mode, p.native_substate),
             SyncModeName((SyncMode)p.sync_mode),
             LockstepContextName((LockstepContext)p.lockstep_context),
             FrontendSyncPhaseName((FrontendSyncPhase)p.frontend_phase),
             MatchLifecyclePhaseName((MatchLifecyclePhase)p.match_lifecycle_phase),
             MatchRollbackPhaseName((MatchRollbackPhase)p.rollback_phase),
             p.game_abs_frame,
             p.rb_frame,
             p.frontend_frame,
             p.local_input,
             p.remote_input,
             p.local_input_frame,
             p.remote_latest_frame,
             p.consume_frame,
             p.remote_ack_frame,
             p.local_visible_delay,
             p.remote_visible_delay,
             p.local_effective_delay,
             p.remote_effective_delay,
             p.rollback_budget,
             p.predicted_frames,
             p.last_rollback_depth,
             p.max_rollback_depth,
             p.state_crc,
             p.p1_crc,
             p.p2_crc,
             p.flags);
    Diagnostics::AsyncLog_EnqueueCsv("synctrace_local", row);
}

static void AppendCompareCsv(const char* result,
                             const SyncTracePayload& local,
                             const SyncTracePayload& remote,
                             const char* reason) {
    if (!ShouldWriteCsv()) {
        return;
    }
    char row[1024];
    snprintf(row,
             sizeof(row),
             "%s,%s,%u,%u,%u,%u,%s,%s,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%04X,0x%04X,0x%04X,0x%04X,0x%04X,0x%04X,%s",
             result ? result : "?",
             SyncTraceDomainName((SyncTraceDomain)local.domain),
             local.epoch_id,
             TraceKeyFrame(local),
             local.seq,
             remote.seq,
             GameModeName(local.native_mode),
             NativeSubstateName(local.native_mode, local.native_substate),
             local.state_crc,
             remote.state_crc,
             local.p1_crc,
             remote.p1_crc,
             local.p2_crc,
             remote.p2_crc,
             InputForSlot(local, 0),
             InputForSlot(remote, 0),
             InputForSlot(local, 1),
             InputForSlot(remote, 1),
             local.flags,
             remote.flags,
             reason ? reason : "");
    Diagnostics::AsyncLog_EnqueueCsv("synctrace_compare", row);
}

static void ComparePair(TracePair* pair) {
    if (!pair || !pair->has_local || !pair->has_remote || pair->compared) {
        return;
    }

    pair->compared = true;
    const SyncTracePayload& local = pair->local;
    const SyncTracePayload& remote = pair->remote;
    const auto domain = (SyncTraceDomain)local.domain;

    if (domain == SyncTraceDomain::FrontendLockstep) {
        const uint16_t localP1 = InputForSlot(local, 0);
        const uint16_t remoteP1 = InputForSlot(remote, 0);
        const uint16_t localP2 = InputForSlot(local, 1);
        const uint16_t remoteP2 = InputForSlot(remote, 1);
        const bool match =
            local.state_crc == remote.state_crc &&
            localP1 == remoteP1 &&
            localP2 == remoteP2;

        AppendCompareCsv(match ? "OK" : "MISMATCH",
                         local,
                         remote,
                         match ? "frontend_aligned" : "frontend_input_or_crc");
        if (!match) {
            Rollback::NetplayLog_Verbose(
                "SYNCCHECK",
                (int32_t)local.frontend_frame,
                "Frontend SyncTrace mismatch: epoch=%u phase=%s frame=%u "
                "local_crc=0x%08X remote_crc=0x%08X p1=0x%04X/0x%04X p2=0x%04X/0x%04X",
                local.epoch_id,
                FrontendSyncPhaseName((FrontendSyncPhase)local.frontend_phase),
                local.frontend_frame,
                local.state_crc,
                remote.state_crc,
                localP1,
                remoteP1,
                localP2,
                remoteP2);
        } else {
            Rollback::NetplayLog_Verbose(
                "SYNCCHECK",
                (int32_t)local.frontend_frame,
                "Frontend SyncTrace OK: epoch=%u phase=%s frame=%u crc=0x%08X",
                local.epoch_id,
                FrontendSyncPhaseName((FrontendSyncPhase)local.frontend_phase),
                local.frontend_frame,
                local.state_crc);
        }
        return;
    }

    if (domain == SyncTraceDomain::GameplayRollback) {
        if ((local.flags & SYNC_TRACE_FLAG_ROLLING_BACK) != 0 ||
            (remote.flags & SYNC_TRACE_FLAG_ROLLING_BACK) != 0) {
            AppendCompareCsv("DEFER", local, remote, "rolling_back");
            pair->compared = false;
            return;
        }

        if ((local.flags & SYNC_TRACE_FLAG_VALID_STATE_CRC) == 0 ||
            (remote.flags & SYNC_TRACE_FLAG_VALID_STATE_CRC) == 0) {
            AppendCompareCsv("DEFER", local, remote, "missing_state_crc");
            pair->compared = false;
            return;
        }

        const int32_t remoteConfirmedRb = (int32_t)remote.remote_ack_frame;
        if (!Rollback::RollbackDebug_IsRbFrameReadyToCompare(local.rb_frame, remoteConfirmedRb)) {
            AppendCompareCsv("DEFER", local, remote, "gameplay_not_settled");
            pair->compared = false;
            return;
        }

        pair->compared = true;

        bool match = local.state_crc == remote.state_crc;
        if ((local.flags & SYNC_TRACE_FLAG_VALID_PLAYER_CRC) != 0 &&
            (remote.flags & SYNC_TRACE_FLAG_VALID_PLAYER_CRC) != 0) {
            match = match &&
                    local.p1_crc == remote.p1_crc &&
                    local.p2_crc == remote.p2_crc;
        }

        AppendCompareCsv(match ? "OK" : "MISMATCH",
                         local,
                         remote,
                         match ? "gameplay_aligned" : "gameplay_crc");
        if (!match) {
            Rollback::NetplayLog_Verbose(
                "SYNCCHECK",
                local.rb_frame,
                "Gameplay SyncTrace mismatch (diagnostic only): epoch=%u rb=%d abs=%d/%d "
                "state=0x%08X/0x%08X p1=0x%08X/0x%08X p2=0x%08X/0x%08X flags=0x%04X/0x%04X",
                local.epoch_id,
                local.rb_frame,
                local.game_abs_frame,
                remote.game_abs_frame,
                local.state_crc,
                remote.state_crc,
                local.p1_crc,
                remote.p1_crc,
                local.p2_crc,
                remote.p2_crc,
                local.flags,
                remote.flags);
        } else {
            Rollback::NetplayLog_Verbose(
                "SYNCCHECK",
                local.rb_frame,
                "Gameplay SyncTrace OK: epoch=%u rb=%d state=0x%08X",
                local.epoch_id,
                local.rb_frame,
                local.state_crc);
        }
        return;
    }

    AppendCompareCsv("INFO", local, remote, "uncompared_domain");
}

static void StoreLocalTrace(const SyncTracePayload& p) {
    TracePair* pair = FindPair(p, true);
    pair->local = p;
    pair->has_local = true;
    pair->compared = false;
    ComparePair(pair);
}

static void StoreRemoteTrace(const SyncTracePayload& p) {
    TracePair* pair = FindPair(p, true);
    pair->remote = p;
    pair->has_remote = true;
    pair->compared = false;
    ComparePair(pair);
}

static void FillCommon(SyncTracePayload* out, SyncTraceDomain domain) {
    memset(out, 0, sizeof(*out));
    out->schema = kSyncTraceSchema;
    out->domain = (uint8_t)domain;
    out->seq = ++s_seq;
    out->game_abs_frame = -1;
    out->rb_frame = -1;
    out->local_player = PlayerSlotByte(PlayerMapping_GetLocalGameSlot());
    out->remote_player = PlayerSlotByte(PlayerMapping_GetRemoteGameSlot());

    SyncPolicySnapshot sync{};
    SyncPolicy_GetSnapshot(&sync);
    out->sync_mode = (uint8_t)sync.current_mode;
    out->lockstep_context = (uint8_t)sync.lockstep_context;
    if (sync.current_mode == SyncMode::Passive) {
        out->flags |= SYNC_TRACE_FLAG_PASSIVE_PHASE;
    }

    out->frontend_phase = (uint8_t)FrontendInputSync_GetPhase();
    out->match_lifecycle_phase = (uint8_t)MatchLifecycle_GetPhase();
    out->rollback_phase = (uint8_t)NetplayPhaseRuntime_GetPhase();
    out->native_mode = ClampU8((int)GetGameMode());
    out->native_substate = ClampU8((int)GetSubstate());
    out->game_type = ClampU8((int)GetGameType());

    out->local_input_frame = FrontendInputSync_GetLocalInputFrame();
    out->remote_latest_frame = FrontendInputSync_GetRemoteLatestFrame();
    out->consume_frame = FrontendInputSync_GetConsumeFrame();
    out->remote_ack_frame = FrontendInputSync_GetRemoteAckFrame();

    DelayPolicySnapshot delay{};
    DelayPolicy_GetSnapshot(&delay);
    out->local_visible_delay = ClampU8(delay.active_delay);
    out->remote_visible_delay = ClampU8(delay.remote_announced_delay);
    out->local_effective_delay = ClampU8(delay.effective_local_delay);
    out->remote_effective_delay = ClampU8(delay.effective_remote_delay);
    out->rollback_budget = ClampU8(delay.rollback_budget);

    if (Rollback::RollbackSession_IsActive()) {
        Rollback::RollbackSessionSnapshot rb{};
        Rollback::RollbackSession_GetSnapshot(&rb);
        out->game_abs_frame = rb.game_abs_frame_current;
        out->rb_frame = rb.rb_frame_current;
        out->remote_latest_frame = (uint32_t)rb.rb_frame_last_remote_received;
        out->consume_frame = (uint32_t)rb.rb_frame_current;
        out->remote_ack_frame = (uint32_t)rb.rb_frame_last_confirmed;
        out->rollback_budget = ClampU8(rb.rollback_budget);
        out->predicted_frames = ClampU8(rb.predicted_frames_outstanding);
        out->last_rollback_depth = ClampU8(rb.last_rollback_replay_length);
        out->max_rollback_depth = ClampU8(rb.max_rollback_distance);
        if (rb.is_rolling_back) {
            out->flags |= SYNC_TRACE_FLAG_ROLLING_BACK;
        }
    } else {
        out->game_abs_frame = (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    }
}

static void AddFullPlayerCrcs(SyncTracePayload* p) {
    if (!s_fullCrc || !p) {
        return;
    }
    p->p1_crc = CalcCRC32((const void*)ADDR_P1_ENTITY_BASE, ENTITY_SIZE);
    p->p2_crc = CalcCRC32((const void*)ADDR_P2_ENTITY_BASE, ENTITY_SIZE);
    p->flags |= SYNC_TRACE_FLAG_VALID_PLAYER_CRC;
}

static void RetryDeferredCompares() {
    for (uint32_t i = 0; i < kPairSlots; ++i) {
        TracePair* pair = &s_pairs[i];
        if (!pair->has_local || !pair->has_remote || pair->compared) {
            continue;
        }
        if ((SyncTraceDomain)pair->local.domain != SyncTraceDomain::GameplayRollback) {
            continue;
        }
        ComparePair(pair);
    }
}

static void EmitLocalTrace(SyncTracePayload* p) {
    if (!p || !IsCompareActive()) {
        return;
    }

    AppendTraceCsv("local", *p);
    StoreLocalTrace(*p);

    if (Session_IsConnected()) {
        const bool sent = BarrierProtocol_SendPacket(
            PacketType::SyncTrace,
            p,
            sizeof(*p));
        if (!sent && ShouldWriteCsv()) {
            Rollback::NetplayLog_Verbose(
                "SYNCTRCE",
                p->rb_frame >= 0 ? p->rb_frame : (int32_t)p->frontend_frame,
                "Failed to send SyncTrace packet: domain=%s seq=%u",
                SyncTraceDomainName((SyncTraceDomain)p->domain),
                p->seq);
        }
    }
}

} // namespace

void SyncTrace_Init() {
    memset(s_pairs, 0, sizeof(s_pairs));
    s_seq = 0;
    s_lastGameplayTraceFrame = -1;
    s_initialized = true;
    s_enabled = EnvFlagEnabled("AS2_SYNC_TRACE");
    s_integrityMode = false;
    s_fullCrc = EnvFlagEnabled("AS2_SYNC_TRACE_FULL_CRC");
    Rollback::NetplayLog_Write(
        "SYNCTRCE",
        -1,
        "SyncTrace init: full_trace=%d integrity_on_rollback=%d full_crc=%d env=AS2_SYNC_TRACE/AS2_SYNC_TRACE_INTEGRITY",
        s_enabled ? 1 : 0,
        EnvFlagEnabled("AS2_SYNC_TRACE_INTEGRITY") ? 1 : 0,
        s_fullCrc ? 1 : 0);
}

void SyncTrace_Shutdown() {
    if (s_initialized) {
        Rollback::NetplayLog_Write("SYNCTRCE", -1, "SyncTrace shutdown");
    }
    s_initialized = false;
    s_enabled = false;
    s_integrityMode = false;
    memset(s_pairs, 0, sizeof(s_pairs));
}

void SyncTrace_SetEnabled(bool enabled, const char* reason) {
    if (!s_initialized) {
        return;
    }
    if (s_enabled == enabled) {
        return;
    }
    s_enabled = enabled;
    Rollback::NetplayLog_Write(
        "SYNCTRCE",
        -1,
        "SyncTrace %s (%s)",
        enabled ? "enabled" : "disabled",
        reason ? reason : "manual");
}

bool SyncTrace_IsEnabled() {
    return s_initialized && s_enabled;
}

void SyncTrace_SetIntegrityActive(bool active, const char* reason) {
    if (!s_initialized) {
        return;
    }
    if (s_integrityMode == active) {
        return;
    }
    s_integrityMode = active;
    if (!active) {
        s_lastGameplayTraceFrame = -1;
    }
    Rollback::NetplayLog_Write(
        "SYNCTRCE",
        -1,
        "SyncTrace integrity compare %s (%s)",
        active ? "armed" : "disarmed",
        reason ? reason : "manual");
}

bool SyncTrace_IsIntegrityActive() {
    return s_initialized && s_integrityMode;
}

bool SyncTrace_ShouldArmIntegrityOnRollback() {
    return s_enabled || EnvFlagEnabled("AS2_SYNC_TRACE_INTEGRITY");
}

bool SyncTrace_IsCompareActive() {
    return IsCompareActive();
}

void SyncTrace_ResetSession(const char* reason) {
    memset(s_pairs, 0, sizeof(s_pairs));
    s_lastGameplayTraceFrame = -1;
    if (IsCompareActive()) {
        Rollback::NetplayLog_Write(
            "SYNCTRCE",
            -1,
            "SyncTrace session reset: %s",
            reason ? reason : "unspecified");
    }
}

void SyncTrace_FrameUpdate() {
    if (!IsCompareActive()) {
        return;
    }
    RetryDeferredCompares();

    if (!Rollback::RollbackSession_IsActive()) {
        s_lastGameplayTraceFrame = -1;
        return;
    }
    if (!MatchLifecycle_IsGameplayPlayable()) {
        return;
    }
    if (Rollback::RollbackSession_IsRollingBack()) {
        return;
    }

    Rollback::RollbackSessionSnapshot rb{};
    Rollback::RollbackSession_GetSnapshot(&rb);
    if (!rb.active || rb.rb_frame_current < 0) {
        return;
    }
    if (rb.rb_frame_current == s_lastGameplayTraceFrame) {
        return;
    }

    const bool fullTrace = s_enabled;
    if (!fullTrace) {
        if (rb.rb_frame_current <= 0 ||
            (rb.rb_frame_current % kIntegrityTraceInterval) != 0) {
            return;
        }
    }

    s_lastGameplayTraceFrame = rb.rb_frame_current;

    SyncTracePayload payload{};
    FillCommon(&payload, SyncTraceDomain::GameplayRollback);
    payload.epoch_id = rb.baseline_checksum != 0
        ? rb.baseline_checksum
        : (uint32_t)rb.frame_origin_abs;
    payload.game_abs_frame = rb.game_abs_frame_current;
    payload.rb_frame = rb.rb_frame_current;
    payload.consume_frame = (uint32_t)rb.rb_frame_current;
    payload.remote_ack_frame = (uint32_t)rb.rb_frame_last_confirmed;
    payload.remote_latest_frame = (uint32_t)rb.rb_frame_last_remote_received;
    payload.rollback_budget = ClampU8(rb.rollback_budget);
    payload.predicted_frames = ClampU8(rb.predicted_frames_outstanding);
    payload.last_rollback_depth = ClampU8(rb.last_rollback_replay_length);
    payload.max_rollback_depth = ClampU8(rb.max_rollback_distance);
    payload.state_crc = rb.current_checksum;
    if (Rollback::RollbackDebug_TryGetChecksumForFrame(rb.rb_frame_current, &payload.state_crc)) {
        payload.flags |= SYNC_TRACE_FLAG_VALID_STATE_CRC;
    } else if (rb.current_checksum != 0) {
        payload.state_crc = rb.current_checksum;
        payload.flags |= SYNC_TRACE_FLAG_VALID_STATE_CRC;
    }
    if (rb.rb_frame_current <= rb.rb_frame_last_confirmed) {
        payload.flags |= SYNC_TRACE_FLAG_ROLLBACK_SETTLED;
    }
    if (rb.is_rolling_back) {
        payload.flags |= SYNC_TRACE_FLAG_ROLLING_BACK;
    }
    AddFullPlayerCrcs(&payload);
    EmitLocalTrace(&payload);
}

void SyncTrace_OnFrontendFrameConsumed(uint32_t epochId,
                                       FrontendSyncPhase phase,
                                       uint32_t frame,
                                       uint16_t localInput,
                                       uint16_t remoteInput) {
    if (!s_initialized || !s_enabled) {
        return;
    }

    uint16_t p1Input = 0;
    uint16_t p2Input = 0;
    InputsByGameSlot(localInput, remoteInput, &p1Input, &p2Input);

    SyncTracePayload payload{};
    FillCommon(&payload, SyncTraceDomain::FrontendLockstep);
    payload.flags |= SYNC_TRACE_FLAG_VALID_STATE_CRC;
    payload.epoch_id = epochId;
    payload.frontend_phase = (uint8_t)phase;
    payload.frontend_frame = frame;
    payload.local_input = localInput;
    payload.remote_input = remoteInput;
    payload.consume_frame = frame;
    payload.state_crc = FrontendStateCrc(epochId, phase, frame, p1Input, p2Input);
    EmitLocalTrace(&payload);
}

void SyncTrace_OnLifecycleTransition(const char* reason) {
    if (!s_initialized || !s_enabled) {
        return;
    }

    SyncTracePayload payload{};
    FillCommon(&payload, SyncTraceDomain::LifecycleEvent);
    payload.epoch_id = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    payload.state_crc = FrontendStateCrc(
        payload.epoch_id,
        (FrontendSyncPhase)payload.frontend_phase,
        payload.seq,
        (uint16_t)payload.native_mode,
        (uint16_t)payload.native_substate);
    payload.flags |= SYNC_TRACE_FLAG_VALID_STATE_CRC;
    Rollback::NetplayLog_Write(
        "SYNCTRCE",
        payload.game_abs_frame,
        "Lifecycle trace: reason=%s mode=%s/%s sync=%s phase=%s",
        reason ? reason : "transition",
        GameModeName(payload.native_mode),
        NativeSubstateName(payload.native_mode, payload.native_substate),
        SyncModeName((SyncMode)payload.sync_mode),
        MatchLifecyclePhaseName((MatchLifecyclePhase)payload.match_lifecycle_phase));
    EmitLocalTrace(&payload);
}

void SyncTrace_OnRemoteTrace(const SyncTracePayload* payload) {
    if (!payload) {
        return;
    }
    if (payload->schema != kSyncTraceSchema) {
        Rollback::NetplayLog_Verbose(
            "SYNCTRCE",
            -1,
            "Ignored SyncTrace with unsupported schema=%u",
            payload->schema);
        return;
    }
    if (!IsCompareActive()) {
        return;
    }

    AppendTraceCsv("remote", *payload);
    StoreRemoteTrace(*payload);
}

} // namespace Net
