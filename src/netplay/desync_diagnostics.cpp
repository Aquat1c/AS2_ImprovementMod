/**
 * Alice Senki 2 - Desync Diagnostics Implementation
 *
 * Per-frame checksum computation, ring buffer storage, and comparison.
 */

#include "desync_diagnostics.h"
#include "rng_hooks.h"
#include "as2_constants.h"
#include "packet_codec.h"
#include "log_window.h"
#include <string.h>

namespace DesyncDiag {

// ============================================================================
// Internal State
// ============================================================================

static FrameChecksum s_ring[CHECKSUM_RING_SIZE] = {};
static int s_ringHead = 0;  // Next write position

static bool      s_desyncDetected = false;
static DesyncDump s_lastDesync = {};
static Stats     s_stats = {};

// ============================================================================
// Helpers
// ============================================================================

static uint32_t ComputeGameStateChecksum() {
    // Build a struct of gameplay-relevant state and CRC it
    struct {
        int16_t  p1_hp, p1_x, p1_y;
        int16_t  p2_hp, p2_x, p2_y;
        uint32_t rng_seed;
        uint32_t sim_frame;
        uint32_t p1_action, p2_action;
        int16_t  p1_meter, p2_meter;
    } state = {};
    
    // Read from game memory using correct entity offsets (as2_constants.h)
    uint8_t* p1 = (uint8_t*)ADDR_P1_ENTITY_BASE;
    uint8_t* p2 = (uint8_t*)ADDR_P2_ENTITY_BASE;
    
    state.p1_hp = *(int16_t*)(p1 + ENTITY_OFF_HP);
    state.p1_x  = *(int16_t*)(p1 + ENTITY_OFF_X_POS);
    state.p1_y  = *(int16_t*)(p1 + ENTITY_OFF_Y_POS);
    state.p2_hp = *(int16_t*)(p2 + ENTITY_OFF_HP);
    state.p2_x  = *(int16_t*)(p2 + ENTITY_OFF_X_POS);
    state.p2_y  = *(int16_t*)(p2 + ENTITY_OFF_Y_POS);
    
    state.p1_meter = *(int16_t*)(p1 + ENTITY_OFF_METER);
    state.p2_meter = *(int16_t*)(p2 + ENTITY_OFF_METER);
    
    state.rng_seed  = RngHooks::GetSimSeed();
    state.sim_frame = *(uint32_t*)ADDR_FRAME_SIMULATION;
    
    state.p1_action = *(uint32_t*)(p1 + ENTITY_OFF_ACTION_ID);
    state.p2_action = *(uint32_t*)(p2 + ENTITY_OFF_ACTION_ID);
    
    return PacketCodec::Crc32(&state, sizeof(state));
}

static void ReadEntitySnapshot(uint8_t* entity, int16_t* hp, int16_t* x, int16_t* y) {
    *hp = *(int16_t*)(entity + ENTITY_OFF_HP);
    *x  = *(int16_t*)(entity + ENTITY_OFF_X_POS);
    *y  = *(int16_t*)(entity + ENTITY_OFF_Y_POS);
}

// Entity timer offset — entity+0x1A7E8 (diverges during rollback resim)
static constexpr uint32_t ENTITY_OFF_TIMER = 0x1A7E8;

// ============================================================================
// API Implementation
// ============================================================================

void Init() {
    Reset();
    LOG_NET_INFO("[DesyncDiag] Initialized (ring=%d, delay=%d frames)",
             CHECKSUM_RING_SIZE, CHECKSUM_DELAY_FRAMES);
}

void Reset() {
    memset(s_ring, 0, sizeof(s_ring));
    s_ringHead = 0;
    s_desyncDetected = false;
    memset(&s_lastDesync, 0, sizeof(s_lastDesync));
    memset(&s_stats, 0, sizeof(s_stats));
    LOG_NET_INFO("[DesyncDiag] Reset");
}

void CaptureFrame(uint32_t frame) {
    FrameChecksum& rec = s_ring[s_ringHead];
    rec.frame    = frame;
    rec.checksum = ComputeGameStateChecksum();
    rec.rng_seed = RngHooks::GetSimSeed();
    
    uint8_t* p1 = (uint8_t*)ADDR_P1_ENTITY_BASE;
    uint8_t* p2 = (uint8_t*)ADDR_P2_ENTITY_BASE;
    
    ReadEntitySnapshot(p1, &rec.p1_hp, &rec.p1_x, &rec.p1_y);
    ReadEntitySnapshot(p2, &rec.p2_hp, &rec.p2_x, &rec.p2_y);
    
    // Per-section CRCs for narrowing desync source
    rec.crc_p1_entity  = PacketCodec::Crc32((void*)ADDR_P1_ENTITY_BASE, ENTITY_SIZE);
    rec.crc_p2_entity  = PacketCodec::Crc32((void*)ADDR_P2_ENTITY_BASE, ENTITY_SIZE);
    rec.crc_match_ctx  = PacketCodec::Crc32((void*)ADDR_MATCH_CONTEXT, MATCH_CONTEXT_SIZE);
    rec.crc_effects    = PacketCodec::Crc32((void*)ADDR_EFFECT_ARRAY, EFFECT_MAX_SLOTS * EFFECT_ENTRY_SIZE);
    rec.crc_summons    = PacketCodec::Crc32((void*)ADDR_SUMMON_ARRAY, SUMMON_MAX_SLOTS * SUMMON_ENTRY_SIZE);
    
    // Key divergence-prone values
    rec.p1_meter = *(int16_t*)(p1 + ENTITY_OFF_METER);
    rec.p2_meter = *(int16_t*)(p2 + ENTITY_OFF_METER);
    rec.p1_entity_timer = *(uint32_t*)(p1 + ENTITY_OFF_TIMER);
    rec.p2_entity_timer = *(uint32_t*)(p2 + ENTITY_OFF_TIMER);
    
    s_ringHead = (s_ringHead + 1) % CHECKSUM_RING_SIZE;
    s_stats.frames_captured++;
}

bool GetChecksum(uint32_t frame, uint32_t* outChecksum) {
    for (int i = 0; i < CHECKSUM_RING_SIZE; i++) {
        if (s_ring[i].frame == frame) {
            *outChecksum = s_ring[i].checksum;
            return true;
        }
    }
    return false;
}

bool GetFrameRecord(uint32_t frame, FrameChecksum* out) {
    for (int i = 0; i < CHECKSUM_RING_SIZE; i++) {
        if (s_ring[i].frame == frame) {
            *out = s_ring[i];
            return true;
        }
    }
    return false;
}

bool CompareRemoteChecksum(uint32_t frame, uint32_t remoteChecksum,
                           const FrameChecksum* remoteSnap) {
    s_stats.comparisons_made++;
    
    FrameChecksum localRec;
    if (!GetFrameRecord(frame, &localRec)) {
        // Frame no longer in ring — can't compare, skip
        return true;
    }
    
    if (localRec.checksum == remoteChecksum) {
        return true;  // Match
    }
    
    // DESYNC DETECTED
    s_stats.mismatches_found++;
    s_stats.last_desync_frame = frame;
    s_desyncDetected = true;
    
    s_lastDesync.frame = frame;
    s_lastDesync.local_checksum  = localRec.checksum;
    s_lastDesync.remote_checksum = remoteChecksum;
    s_lastDesync.local_p1_hp  = localRec.p1_hp;
    s_lastDesync.local_p1_x   = localRec.p1_x;
    s_lastDesync.local_p1_y   = localRec.p1_y;
    s_lastDesync.local_p2_hp  = localRec.p2_hp;
    s_lastDesync.local_p2_x   = localRec.p2_x;
    s_lastDesync.local_p2_y   = localRec.p2_y;
    s_lastDesync.local_rng    = localRec.rng_seed;
    
    // Section CRCs and divergence-prone values
    s_lastDesync.local_crc_p1_entity = localRec.crc_p1_entity;
    s_lastDesync.local_crc_p2_entity = localRec.crc_p2_entity;
    s_lastDesync.local_crc_match_ctx = localRec.crc_match_ctx;
    s_lastDesync.local_crc_effects   = localRec.crc_effects;
    s_lastDesync.local_crc_summons   = localRec.crc_summons;
    s_lastDesync.local_p1_meter = localRec.p1_meter;
    s_lastDesync.local_p2_meter = localRec.p2_meter;
    s_lastDesync.local_p1_timer = localRec.p1_entity_timer;
    s_lastDesync.local_p2_timer = localRec.p2_entity_timer;
    
    if (remoteSnap) {
        s_lastDesync.remote_p1_hp = remoteSnap->p1_hp;
        s_lastDesync.remote_p1_x  = remoteSnap->p1_x;
        s_lastDesync.remote_p1_y  = remoteSnap->p1_y;
        s_lastDesync.remote_p2_hp = remoteSnap->p2_hp;
        s_lastDesync.remote_p2_x  = remoteSnap->p2_x;
        s_lastDesync.remote_p2_y  = remoteSnap->p2_y;
        s_lastDesync.remote_rng   = remoteSnap->rng_seed;
    }
    
    LOG_NET_ERROR("======== DESYNC DETECTED at frame %u ========", frame);
    LOG_NET_ERROR("  Local checksum:  0x%08X", localRec.checksum);
    LOG_NET_ERROR("  Remote checksum: 0x%08X", remoteChecksum);
    LOG_NET_ERROR("  Local  P1: HP=%d X=%d Y=%d M=%d T=%u",
                  localRec.p1_hp, localRec.p1_x, localRec.p1_y,
                  localRec.p1_meter, localRec.p1_entity_timer);
    LOG_NET_ERROR("  Local  P2: HP=%d X=%d Y=%d M=%d T=%u",
                  localRec.p2_hp, localRec.p2_x, localRec.p2_y,
                  localRec.p2_meter, localRec.p2_entity_timer);
    LOG_NET_ERROR("  Local  RNG: 0x%08X", localRec.rng_seed);
    LOG_NET_ERROR("  Section CRCs: P1=0x%08X P2=0x%08X Match=0x%08X Fx=0x%08X Sum=0x%08X",
                  localRec.crc_p1_entity, localRec.crc_p2_entity,
                  localRec.crc_match_ctx, localRec.crc_effects, localRec.crc_summons);
    if (remoteSnap) {
        LOG_NET_ERROR("  Remote P1: HP=%d X=%d Y=%d M=%d T=%u",
                      remoteSnap->p1_hp, remoteSnap->p1_x, remoteSnap->p1_y,
                      remoteSnap->p1_meter, remoteSnap->p1_entity_timer);
        LOG_NET_ERROR("  Remote P2: HP=%d X=%d Y=%d M=%d T=%u",
                      remoteSnap->p2_hp, remoteSnap->p2_x, remoteSnap->p2_y,
                      remoteSnap->p2_meter, remoteSnap->p2_entity_timer);
        LOG_NET_ERROR("  Remote RNG: 0x%08X", remoteSnap->rng_seed);
        LOG_NET_ERROR("  Remote CRCs: P1=0x%08X P2=0x%08X Match=0x%08X Fx=0x%08X Sum=0x%08X",
                      remoteSnap->crc_p1_entity, remoteSnap->crc_p2_entity,
                      remoteSnap->crc_match_ctx, remoteSnap->crc_effects, remoteSnap->crc_summons);
    }
    LOG_NET_ERROR("=============================================");
    
    return false;
}

bool GetLastDesyncDump(DesyncDump* out) {
    if (!s_desyncDetected || !out) return false;
    *out = s_lastDesync;
    return true;
}

bool HasDesyncOccurred() { return s_desyncDetected; }

void GetStats(Stats* out) {
    if (out) *out = s_stats;
}

} // namespace DesyncDiag
