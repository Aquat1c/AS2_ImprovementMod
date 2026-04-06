/**
 * Alice Senki 2 - State Digest Implementation
 */

#include "state_digest.h"
#include "core/as2_rollback.h"
#include "core/as2_constants.h"
#include "log_window.h"

#include <string.h>
#include <stddef.h>

namespace StateDigest {

namespace {

static uint32_t s_desyncCount = 0;
static constexpr int kRecentLocalDigests = 8;
static PacketCodec::StateDigestPayload s_localDigestHistory[kRecentLocalDigests] = {};
static bool s_localDigestValid[kRecentLocalDigests] = {};
static int s_localDigestHead = 0;
static CompactSaveState_t s_checksumSnapshot = {};

// Safe memory read helpers
static uint32_t ReadU32(uintptr_t addr) {
    __try { return *reinterpret_cast<uint32_t*>(addr); }
    __except(1) { return 0; }
}

static int16_t ReadS16(uintptr_t addr) {
    __try { return *reinterpret_cast<int16_t*>(addr); }
    __except(1) { return 0; }
}

static uint16_t ReadU16(uintptr_t addr) {
    __try { return *reinterpret_cast<uint16_t*>(addr); }
    __except(1) { return 0; }
}

static int32_t ReadS32(uintptr_t addr) {
    __try { return *reinterpret_cast<int32_t*>(addr); }
    __except(1) { return 0; }
}

static bool FindRecordedLocal(uint32_t frame, PacketCodec::StateDigestPayload* out) {
    if (!out) return false;

    for (int i = 0; i < kRecentLocalDigests; ++i) {
        if (s_localDigestValid[i] && s_localDigestHistory[i].frame_number == frame) {
            *out = s_localDigestHistory[i];
            return true;
        }
    }

    return false;
}

} // namespace

void Capture(PacketCodec::StateDigestPayload* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    // Global state
    out->frame_number = ReadU32(ADDR_SIM_FRAME_COUNTER);
    out->game_mode    = ReadU32(ADDR_GAME_MODE);
    out->sub_state    = ReadU32(ADDR_SUB_STATE);
    out->rng_seed     = AS2_GetRngSeed();

    // Player entity state
    uintptr_t p1 = GetEntityBase(0);
    uintptr_t p2 = GetEntityBase(1);

    if (p1) {
        out->p1_hp        = ReadS16(p1 + ENTITY_OFF_HP);
        out->p1_x         = ReadS16(p1 + ENTITY_OFF_X_POS);
        out->p1_y         = ReadS16(p1 + ENTITY_OFF_Y_POS);
        out->p1_meter     = ReadU16(p1 + ENTITY_OFF_METER);
        out->p1_action_id = ReadU32(p1 + ENTITY_OFF_ACTION_ID);
        out->p1_input     = ReadU16(ADDR_P1_INPUT_STATE);
    }

    if (p2) {
        out->p2_hp        = ReadS16(p2 + ENTITY_OFF_HP);
        out->p2_x         = ReadS16(p2 + ENTITY_OFF_X_POS);
        out->p2_y         = ReadS16(p2 + ENTITY_OFF_Y_POS);
        out->p2_meter     = ReadU16(p2 + ENTITY_OFF_METER);
        out->p2_action_id = ReadU32(p2 + ENTITY_OFF_ACTION_ID);
        out->p2_input     = ReadU16(ADDR_P2_INPUT_STATE);
    }

    // Match state
    out->round_timer    = ReadS32(ADDR_ROUND_TIMER);
    out->quick_checksum = AS2_GetQuickChecksum();

    if (AS2_IsInPlayableGameplay()) {
        AS2_SaveStateFast(&s_checksumSnapshot);
        out->full_state_crc = AS2_ComputeCompactStateChecksum(&s_checksumSnapshot);
    } else {
        // Fallback for callers outside the interactive fighting window.
        out->full_state_crc = PacketCodec::Crc32(out, offsetof(PacketCodec::StateDigestPayload, full_state_crc));
    }
}

void RecordLocal(const PacketCodec::StateDigestPayload* digest) {
    if (!digest) return;

    s_localDigestHistory[s_localDigestHead] = *digest;
    s_localDigestValid[s_localDigestHead] = true;
    s_localDigestHead = (s_localDigestHead + 1) % kRecentLocalDigests;
}

bool Compare(const PacketCodec::StateDigestPayload* local,
             const PacketCodec::StateDigestPayload* remote) {
    if (!local || !remote) return false;

    // Frame number mismatch is expected (async delivery); only compare if same frame
    if (local->frame_number != remote->frame_number) {
        return true;  // Can't compare different frames — treat as "ok"
    }

    bool match = true;

    if (local->rng_seed != remote->rng_seed) {
        LOG_NET_WARN("[Desync] Frame %u: RNG mismatch local=0x%08X remote=0x%08X",
                    local->frame_number, local->rng_seed, remote->rng_seed);
        match = false;
    }

    if (local->p1_hp != remote->p1_hp || local->p2_hp != remote->p2_hp) {
        LOG_NET_WARN("[Desync] Frame %u: HP mismatch P1(%d vs %d) P2(%d vs %d)",
                    local->frame_number,
                    local->p1_hp, remote->p1_hp,
                    local->p2_hp, remote->p2_hp);
        match = false;
    }

    if (local->p1_x != remote->p1_x || local->p1_y != remote->p1_y ||
        local->p2_x != remote->p2_x || local->p2_y != remote->p2_y) {
        LOG_NET_WARN("[Desync] Frame %u: Position mismatch P1(%d,%d vs %d,%d) P2(%d,%d vs %d,%d)",
                    local->frame_number,
                    local->p1_x, local->p1_y, remote->p1_x, remote->p1_y,
                    local->p2_x, local->p2_y, remote->p2_x, remote->p2_y);
        match = false;
    }

    if (local->p1_action_id != remote->p1_action_id || local->p2_action_id != remote->p2_action_id) {
        LOG_NET_WARN("[Desync] Frame %u: Action mismatch P1(%u vs %u) P2(%u vs %u)",
                    local->frame_number,
                    local->p1_action_id, remote->p1_action_id,
                    local->p2_action_id, remote->p2_action_id);
        match = false;
    }

    if (local->quick_checksum != remote->quick_checksum) {
        LOG_NET_WARN("[Desync] Frame %u: Quick checksum mismatch local=0x%04X remote=0x%04X",
                    local->frame_number, local->quick_checksum, remote->quick_checksum);
        match = false;
    }

    if (local->full_state_crc != remote->full_state_crc) {
        LOG_NET_WARN("[Desync] Frame %u: Full-state CRC mismatch local=0x%08X remote=0x%08X",
                    local->frame_number, local->full_state_crc, remote->full_state_crc);
        match = false;
    }

    if (!match) {
        s_desyncCount++;
        if (s_desyncCount == 1) {
            LOG_NET_ERROR("[Desync] First desync detected at frame %u!", local->frame_number);
        }
    } else {
        // Could reset on success, but keep cumulative for debug visibility
    }

    return match;
}

bool CompareRemote(const PacketCodec::StateDigestPayload* remote) {
    if (!remote) return false;

    PacketCodec::StateDigestPayload local = {};
    if (!FindRecordedLocal(remote->frame_number, &local)) {
        LOG_NET_DEBUG("[Desync] No matching local digest recorded for remote frame %u", remote->frame_number);
        return true;
    }

    return Compare(&local, remote);
}

uint32_t GetDesyncCount() {
    return s_desyncCount;
}

void ResetDesyncCount() {
    s_desyncCount = 0;
    memset(s_localDigestHistory, 0, sizeof(s_localDigestHistory));
    memset(s_localDigestValid, 0, sizeof(s_localDigestValid));
    s_localDigestHead = 0;
}

} // namespace StateDigest
