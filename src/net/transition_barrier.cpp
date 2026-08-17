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
    // EpochAlign payload (§4.5); zero for every other kind.
    uint32_t localEpoch     = 0;
    uint8_t  localFirstPhase = 0;
    uint8_t  localNativeMode = 0;
    uint32_t remoteEpoch    = 0;
    uint8_t  remoteFirstPhase = 0;
    uint8_t  remoteNativeMode = 0;
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
    p.epoch = slot.localEpoch;
    p.first_phase = slot.localFirstPhase;
    p.native_mode = slot.localNativeMode;
    Session_SendPacket(CHANNEL_CONTROL, PacketType::PhaseTransitionProposal,
                       &p, sizeof(p), true);
    slot.lastSendTick = GetTickCount();
}

void SendAck(NetTransitionKind kind, uint32_t seq, uint8_t intent,
             const PhaseTransitionPayload& echo) {
    PhaseTransitionPayload p{};
    p.transition_seq = seq;
    p.kind = (uint8_t)kind;
    p.intent = intent;
    p.session_id = echo.session_id;
    // Echo the align fields verbatim (INV-13 discipline for the new fields).
    p.epoch = echo.epoch;
    p.first_phase = echo.first_phase;
    p.native_mode = echo.native_mode;
    Session_SendPacket(CHANNEL_CONTROL, PacketType::PhaseTransitionAck,
                       &p, sizeof(p), true);
}

void MaybeCommit(NetTransitionKind kind, BarrierSlot& slot) {
    if (slot.committed) return;
    if (slot.localProposed && slot.localAcked && slot.remoteProposed) {
        // EpochAlign (§4.5, INV-10): commit only when both sides proposed the
        // SAME {epoch, first_phase}. A mismatch is surfaced (never silently
        // corrected) and the higher epoch wins by re-proposal on the adopter
        // side (match_setup drives that).
        if (kind == NetTransitionKind::EpochAlign &&
            (slot.localEpoch != slot.remoteEpoch ||
             slot.localFirstPhase != slot.remoteFirstPhase)) {
            static DWORD s_lastMismatchLogTick = 0;
            const DWORD now = GetTickCount();
            if (s_lastMismatchLogTick == 0 || (now - s_lastMismatchLogTick) >= 1000) {
                s_lastMismatchLogTick = now;
                Rollback::NetplayLog_Write("TRANSIT", -1,
                    "EpochAlign proposal mismatch (commit refused): local={%u,%u} remote={%u,%u}",
                    slot.localEpoch, slot.localFirstPhase,
                    slot.remoteEpoch, slot.remoteFirstPhase);
            }
            return;
        }
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

void TransitionBarrier_Clear(NetTransitionKind kind, const char* reason) {
    BarrierSlot* slot = SlotFor(kind);
    if (!slot) return;
    const bool hadState = slot->localProposed || slot->remoteProposed;
    *slot = BarrierSlot{};
    if (s_initialized && hadState) {
        Rollback::NetplayLog_Write("TRANSIT", -1, "Clear %s: %s",
            NetTransitionKindName(kind), reason ? reason : "?");
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

void TransitionBarrier_ProposeEpochAlign(uint32_t epoch, uint8_t firstPhase,
                                         uint8_t nativeMode, uint32_t sessionId) {
    if (!s_initialized) return;
    BarrierSlot* slot = SlotFor(NetTransitionKind::EpochAlign);
    if (!slot) return;
    if (slot->localProposed) {
        // Same payload already in flight/committed: idempotent no-op.
        if (slot->localEpoch == epoch && slot->localFirstPhase == firstPhase) {
            return;
        }
        // Payload change before/after commit re-proposes with a fresh seq
        // (adoption of a higher epoch, §4.5).
        slot->localAcked = false;
        slot->committed = false;
        slot->commitConsumed = false;
    }
    slot->localProposed = true;
    slot->localIntent = 0;
    slot->sessionId = sessionId;
    slot->localEpoch = epoch;
    slot->localFirstPhase = firstPhase;
    slot->localNativeMode = nativeMode;
    slot->localSeq = s_nextSeq++;
    LOG_NETPLAY(LOG_INFO,
        "[Transition] Propose EpochAlign seq=%u epoch=%u first_phase=%u native_mode=%u",
        slot->localSeq, epoch, firstPhase, nativeMode);
    SendProposal(NetTransitionKind::EpochAlign, *slot);
    MaybeCommit(NetTransitionKind::EpochAlign, *slot);
}

bool TransitionBarrier_GetRemoteEpochAlign(uint32_t* epoch, uint8_t* firstPhase,
                                           uint8_t* nativeMode) {
    const BarrierSlot* slot = SlotFor(NetTransitionKind::EpochAlign);
    if (!slot || !slot->remoteProposed) return false;
    if (epoch) *epoch = slot->remoteEpoch;
    if (firstPhase) *firstPhase = slot->remoteFirstPhase;
    if (nativeMode) *nativeMode = slot->remoteNativeMode;
    return true;
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
        slot->remoteEpoch = p->epoch;
        slot->remoteFirstPhase = p->first_phase;
        slot->remoteNativeMode = p->native_mode;
        SendAck(kind, p->transition_seq, p->intent, *p);
        if (!duplicate) {
            LOG_NETPLAY(LOG_INFO, "[Transition] Remote proposed %s seq=%u intent=%u epoch=%u",
                NetTransitionKindName(kind), p->transition_seq, p->intent, p->epoch);
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
