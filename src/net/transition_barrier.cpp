#include "net/transition_barrier.h"

#include <windows.h>
#include <string.h>

#include "net/session_manager.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

namespace Net {

namespace {

constexpr uint32_t kResendIntervalMs = 250;
constexpr size_t   kMaxKinds = 7;  // NetTransitionKind range (None..EpochAlign)

struct BarrierSlot {
    bool     localProposed  = false;
    bool     localAcked     = false;   // remote acked OUR proposal
    bool     remoteProposed = false;
    bool     committed      = false;
    bool     commitConsumed = false;
    uint8_t  localIntent    = 0;
    uint8_t  remoteIntent   = 0;
    uint32_t sessionId      = 0;
    uint32_t localSeq       = 0;
    uint32_t remoteSeq      = 0;
    DWORD    lastSendTick   = 0;
};

bool        s_initialized = false;
uint32_t    s_nextSeq     = 1;
BarrierSlot s_slots[kMaxKinds];

BarrierSlot* SlotFor(NetTransitionKind kind) {
    const size_t idx = (size_t)kind;
    if (idx == 0 || idx >= kMaxKinds) return nullptr;
    return &s_slots[idx];
}

void SendProposal(NetTransitionKind kind, BarrierSlot& slot) {
    PhaseTransitionPayload p{};
    p.transition_seq = slot.localSeq;
    p.kind = (uint8_t)kind;
    p.intent = slot.localIntent;
    p.session_id = slot.sessionId;
    Session_SendPacket(CHANNEL_CONTROL, PacketType::PhaseTransitionProposal,
                       &p, sizeof(p), true);
    slot.lastSendTick = GetTickCount();
}

void SendAck(NetTransitionKind kind, uint32_t seq, uint8_t intent, uint32_t sessionId) {
    PhaseTransitionPayload p{};
    p.transition_seq = seq;
    p.kind = (uint8_t)kind;
    p.intent = intent;
    p.session_id = sessionId;
    Session_SendPacket(CHANNEL_CONTROL, PacketType::PhaseTransitionAck,
                       &p, sizeof(p), true);
}

void MaybeCommit(NetTransitionKind kind, BarrierSlot& slot) {
    if (slot.committed) return;
    if (slot.localProposed && slot.localAcked && slot.remoteProposed) {
        slot.committed = true;
        slot.commitConsumed = false;
        LOG_NETPLAY(LOG_INFO,
            "[Transition] COMMITTED %s (local_seq=%u remote_seq=%u local_intent=%u remote_intent=%u)",
            NetTransitionKindName(kind),
            slot.localSeq, slot.remoteSeq,
            slot.localIntent, slot.remoteIntent);
        Rollback::NetplayLog_Write("TRANSIT", -1,
            "COMMIT kind=%s local_seq=%u remote_seq=%u intents=%u/%u",
            NetTransitionKindName(kind),
            slot.localSeq, slot.remoteSeq,
            slot.localIntent, slot.remoteIntent);
    }
}

}  // namespace

void TransitionBarrier_Init() {
    s_initialized = true;
    TransitionBarrier_Reset("init");
}

void TransitionBarrier_Shutdown() {
    s_initialized = false;
}

void TransitionBarrier_Reset(const char* reason) {
    for (size_t i = 0; i < kMaxKinds; ++i) {
        s_slots[i] = BarrierSlot{};
    }
    if (s_initialized && reason) {
        Rollback::NetplayLog_Write("TRANSIT", -1, "Reset: %s", reason);
    }
}

void TransitionBarrier_FrameUpdate() {
    if (!s_initialized) return;
    const DWORD now = GetTickCount();
    for (size_t i = 1; i < kMaxKinds; ++i) {
        BarrierSlot& slot = s_slots[i];
        if (slot.localProposed && !slot.localAcked &&
            (slot.lastSendTick == 0 || (now - slot.lastSendTick) >= kResendIntervalMs)) {
            SendProposal((NetTransitionKind)i, slot);
        }
    }
}

void TransitionBarrier_Propose(NetTransitionKind kind, uint8_t intent, uint32_t sessionId) {
    if (!s_initialized) return;
    BarrierSlot* slot = SlotFor(kind);
    if (!slot) return;
    if (slot->localProposed) {
        // Intent change before commit re-proposes with a fresh seq.
        if (slot->localIntent == intent || slot->committed) return;
        slot->localAcked = false;
        slot->committed = false;
        slot->commitConsumed = false;
    }
    slot->localProposed = true;
    slot->localIntent = intent;
    slot->sessionId = sessionId;
    slot->localSeq = s_nextSeq++;
    LOG_NETPLAY(LOG_INFO, "[Transition] Propose %s seq=%u intent=%u",
        NetTransitionKindName(kind), slot->localSeq, intent);
    SendProposal(kind, *slot);
    MaybeCommit(kind, *slot);
}

bool TransitionBarrier_IsCommitted(NetTransitionKind kind) {
    const BarrierSlot* slot = SlotFor(kind);
    return slot && slot->committed;
}

bool TransitionBarrier_RemoteProposed(NetTransitionKind kind) {
    const BarrierSlot* slot = SlotFor(kind);
    return slot && slot->remoteProposed;
}

uint8_t TransitionBarrier_GetRemoteIntent(NetTransitionKind kind) {
    const BarrierSlot* slot = SlotFor(kind);
    return slot ? slot->remoteIntent : 0;
}

bool TransitionBarrier_ConsumeCommit(NetTransitionKind kind) {
    BarrierSlot* slot = SlotFor(kind);
    if (!slot || !slot->committed || slot->commitConsumed) return false;
    slot->commitConsumed = true;
    // Clear the slot for the next use of this kind, keeping nothing sticky.
    *slot = BarrierSlot{};
    return true;
}

bool TransitionBarrier_OnPacket(PacketType type, const void* payload, size_t payloadLen) {
    if (!s_initialized) return false;
    if (type != PacketType::PhaseTransitionProposal &&
        type != PacketType::PhaseTransitionAck) {
        return false;
    }
    if (!payload || payloadLen < sizeof(PhaseTransitionPayload)) {
        Rollback::NetplayLog_Write("TRANSIT", -1,
            "Short %s: len=%zu", PacketTypeName(type), payloadLen);
        return true;
    }

    const PhaseTransitionPayload* p =
        static_cast<const PhaseTransitionPayload*>(payload);
    const NetTransitionKind kind = (NetTransitionKind)p->kind;
    BarrierSlot* slot = SlotFor(kind);
    if (!slot) {
        Rollback::NetplayLog_Write("TRANSIT", -1,
            "Unknown transition kind %u (seq=%u) — ignored", p->kind, p->transition_seq);
        return true;
    }

    if (type == PacketType::PhaseTransitionProposal) {
        // Idempotent: duplicates and stale seqs are re-acked, never fatal.
        const bool duplicate =
            slot->remoteProposed && slot->remoteSeq == p->transition_seq;
        slot->remoteProposed = true;
        slot->remoteSeq = p->transition_seq;
        slot->remoteIntent = p->intent;
        SendAck(kind, p->transition_seq, p->intent, p->session_id);
        if (!duplicate) {
            LOG_NETPLAY(LOG_INFO, "[Transition] Remote proposed %s seq=%u intent=%u",
                NetTransitionKindName(kind), p->transition_seq, p->intent);
        }
        MaybeCommit(kind, *slot);
        return true;
    }

    // Ack: only meaningful for our current seq; stale acks are ignored.
    if (slot->localProposed && p->transition_seq == slot->localSeq) {
        if (!slot->localAcked) {
            slot->localAcked = true;
            LOG_NETPLAY(LOG_INFO, "[Transition] Local %s seq=%u acked",
                NetTransitionKindName(kind), slot->localSeq);
        }
        MaybeCommit(kind, *slot);
    }
    return true;
}

}  // namespace Net
