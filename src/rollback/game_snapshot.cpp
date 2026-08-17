#include "rollback/game_snapshot.h"

#include "patches/memory_utils.h"
#include "rollback/block_digest.h"
#include "rollback/determinism_verify.h"
#include "ui/log_window.h"

#include <string.h>
#include <windows.h>
#include <xmmintrin.h>

namespace Rollback {

namespace {

constexpr uintptr_t kMainStart = ADDR_MATCH_BASE;
constexpr size_t kMainSize = GAME_SNAPSHOT_MAIN_SIZE;
constexpr uintptr_t kPreMatchStart = ADDR_PRE_MATCH_GAP;
constexpr size_t kPreMatchSize = GAME_SNAPSHOT_PRE_MATCH_SIZE;
constexpr uintptr_t kAiLearnStart = ADDR_AI_LEARN_STATICS;
constexpr size_t kAiLearnSize = GAME_SNAPSHOT_AI_LEARN_SIZE;
constexpr uintptr_t kInputP1Start = ADDR_P1_INPUT_BUFFER;
constexpr uintptr_t kInputP2Start = ADDR_P2_INPUT_BUFFER;
constexpr size_t kInputSize = GAME_SNAPSHOT_INPUT_SIZE;

// ── Digest masks inside main_state (SAVESTATE_AUDIT F2/F4/F5) ───────────────
// Windows written at render cadence (tint, super-bg scratch) or gated by
// wall-clock audio (voice bookkeeping). They MUST stay captured+restored
// (rollback must reproduce them locally) but can never enter the cross-peer
// digest: peers whose render:sim ratios or audio timing differ hold different
// bytes there with perfectly synced gameplay. Offsets are relative to
// main_state[0] (= ADDR_MATCH_BASE), sorted ascending, non-overlapping.
struct MaskRange {
    size_t offset;
    size_t size;
};

constexpr size_t kP1EntityOff = ADDR_P1_ENTITY_BASE - ADDR_MATCH_BASE;
constexpr size_t kP2EntityOff = ADDR_P2_ENTITY_BASE - ADDR_MATCH_BASE;

constexpr MaskRange kMainDigestMasks[] = {
    { kP1EntityOff + ENTITY_OFF_RENDER_TINT_STATE, ENTITY_RENDER_TINT_MASK_SIZE },     // F5
    { kP1EntityOff + ENTITY_OFF_SUPERBG_STATE,     ENTITY_SUPERBG_SCRATCH_MASK_SIZE }, // F2
    { kP1EntityOff + ENTITY_OFF_VOICE_BOOKKEEPING, ENTITY_VOICE_BOOKKEEPING_SIZE },    // F4
    { kP2EntityOff + ENTITY_OFF_RENDER_TINT_STATE, ENTITY_RENDER_TINT_MASK_SIZE },     // F5
    { kP2EntityOff + ENTITY_OFF_SUPERBG_STATE,     ENTITY_SUPERBG_SCRATCH_MASK_SIZE }, // F2
    { kP2EntityOff + ENTITY_OFF_VOICE_BOOKKEEPING, ENTITY_VOICE_BOOKKEEPING_SIZE },    // F4
};
constexpr size_t kMainDigestMaskCount =
    sizeof(kMainDigestMasks) / sizeof(kMainDigestMasks[0]);

constexpr bool MasksSortedDisjointInBounds() {
    size_t prev_end = 0;
    for (size_t i = 0; i < kMainDigestMaskCount; ++i) {
        if (kMainDigestMasks[i].size == 0) return false;
        if (kMainDigestMasks[i].offset < prev_end) return false;
        prev_end = kMainDigestMasks[i].offset + kMainDigestMasks[i].size;
        if (prev_end > kMainSize) return false;
    }
    return true;
}
static_assert(MasksSortedDisjointInBounds(),
              "main_state digest masks must be sorted, disjoint and in-bounds");

// Fold main_state into the digest, skipping the mask windows. Shared by the
// snapshot-copy and live-memory digest paths — identical segmentation is what
// keeps GameSnapshot_HashGameplay and GameSnapshot_HashGameplayLive
// byte-for-byte equal (the Block64 tail fold makes segmentation part of the
// hash value, so both paths MUST walk the same table).
uint64_t HashMainMasked(uint64_t h, const uint8_t* mainBytes) {
    size_t pos = 0;
    for (size_t i = 0; i < kMainDigestMaskCount; ++i) {
        h = Block64_Update(h, mainBytes + pos, kMainDigestMasks[i].offset - pos);
        pos = kMainDigestMasks[i].offset + kMainDigestMasks[i].size;
    }
    return Block64_Update(h, mainBytes + pos, kMainSize - pos);
}

uint32_t SnapshotChecksum(const uint8_t* mainState, size_t mainSize, uint32_t effectIndex) {
    struct ChecksumParts {
        uint32_t main_crc;
        uint32_t effect_index;
    } parts{};

    parts.main_crc = CalcCRC32(mainState, mainSize);
    parts.effect_index = effectIndex;
    return CalcCRC32(&parts, sizeof(parts));
}

} // namespace

void GameSnapshot_Clear(GameSnapshot* snapshot) {
    if (!snapshot) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->frame = -1;
}

bool GameSnapshot_Capture(GameSnapshot* snapshot, int32_t frame) {
    if (!snapshot) {
        return false;
    }

    snapshot->valid = false;
    snapshot->frame = frame;
    snapshot->sim_frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    snapshot->display_frame = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    snapshot->game_mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    snapshot->substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    snapshot->substate_timer = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    snapshot->game_type = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    snapshot->match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
    snapshot->rng_seed = DetVer_GetRngSeed();
    snapshot->effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
    // F3: sim-incremented, sim-read Frame_Display (0x816494) — distinct from
    // the render-loop counter captured into display_frame above.
    snapshot->frame_display = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);

    // FPU/MXCSR control words — captured per slot rather than assumed
    // (plan §2.7.7; TrialNetplay pins 0x027F, AS2 captures-and-restores).
    snapshot->fpu_cw = 0;
    snapshot->mxcsr = 0;
    __try {
        unsigned short cw;
        __asm { fnstcw cw }
        snapshot->fpu_cw = cw;
        snapshot->mxcsr = _mm_getcsr();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-fatal: zeros mean "do not restore".
    }

    __try {
        memcpy(snapshot->main_state, (const void*)kMainStart, kMainSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Capture AV at main region 0x%08X", kMainStart);
        return false;
    }

    __try {
        memcpy(snapshot->pre_match_gap, (const void*)kPreMatchStart, kPreMatchSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(snapshot->pre_match_gap, 0, kPreMatchSize);
    }

    // F1: AI pattern-learning statics (0x76C5D8..0x76C5E7). They gate rand()
    // consumption, so a fault here is capture-fatal like the main region —
    // hashing a zeroed stand-in would silently diverge from the live bytes.
    __try {
        memcpy(snapshot->ai_learn, (const void*)kAiLearnStart, kAiLearnSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Capture AV at AI-learn statics 0x%08X", kAiLearnStart);
        return false;
    }

    __try {
        memcpy(snapshot->input_p1, (const void*)kInputP1Start, kInputSize);
        memcpy(snapshot->input_p2, (const void*)kInputP2Start, kInputSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(snapshot->input_p1, 0, kInputSize);
        memset(snapshot->input_p2, 0, kInputSize);
    }

    snapshot->input_read_idx = ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX);
    snapshot->input_write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);
    snapshot->checksum = SnapshotChecksum(
        snapshot->main_state,
        kMainSize,
        snapshot->effect_index);
    snapshot->valid = true;
    return true;
}

bool GameSnapshot_Restore(const GameSnapshot* snapshot) {
    if (!snapshot || !snapshot->valid) {
        return false;
    }

    __try {
        memcpy((void*)kMainStart, snapshot->main_state, kMainSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Restore AV at main region 0x%08X", kMainStart);
        return false;
    }

    __try {
        memcpy((void*)kPreMatchStart, snapshot->pre_match_gap, kPreMatchSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    // F1: restore the AI-learning statics with the state they gated.
    __try {
        memcpy((void*)kAiLearnStart, snapshot->ai_learn, kAiLearnSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Restore AV at AI-learn statics 0x%08X", kAiLearnStart);
        return false;
    }

    DetVer_SetRngSeed(snapshot->rng_seed);
    WriteMemory<uint32_t>(ADDR_EFFECT_INDEX, snapshot->effect_index);

    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, snapshot->sim_frame);
    WriteMemory<uint32_t>(ADDR_FRAME_COUNTER, snapshot->display_frame);
    // F3: rewind Frame_Display with the sim so resim re-increments from the
    // restored value instead of drifting ahead by the rollback depth.
    WriteMemory<uint32_t>(ADDR_FRAME_DISPLAY, snapshot->frame_display);
    WriteMemory<uint32_t>(ADDR_GAME_MODE, snapshot->game_mode);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, snapshot->substate);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, snapshot->substate_timer);
    WriteMemory<uint32_t>(ADDR_GAME_TYPE, snapshot->game_type);
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, snapshot->match_phase_timer);

    __try {
        memcpy((void*)kInputP1Start, snapshot->input_p1, kInputSize);
        memcpy((void*)kInputP2Start, snapshot->input_p2, kInputSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    WriteMemory<uint32_t>(ADDR_INPUT_READ_IDX, snapshot->input_read_idx);
    WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, snapshot->input_write_idx);

    // Restore FPU/MXCSR control words captured with the slot.
    __try {
        if (snapshot->fpu_cw != 0) {
            unsigned short cw = snapshot->fpu_cw;
            __asm { fldcw cw }
        }
        if (snapshot->mxcsr != 0) {
            _mm_setcsr(snapshot->mxcsr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-fatal.
    }

    __try {
        memset((void*)ADDR_MATCH_PER_FRAME_TEMP, 0, MATCH_PER_FRAME_TEMP_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    return true;
}

uint64_t GameSnapshot_HashGameplay(const GameSnapshot* snapshot) {
    if (!snapshot || !snapshot->valid) {
        return 0;
    }
    // SIM-affecting membership only (INV-22, M4-6 audit + SAVESTATE_AUDIT):
    // display_frame (0x81635C), pre_match_gap (render state), and the FPU
    // control words are excluded; main_state is folded through the F2/F4/F5
    // digest masks; frame_display (F3) and the AI-learning statics (F1) are
    // hashed.
    uint64_t h = BLOCK64_SEED;
    struct SimHeader {
        uint32_t rng_seed;
        uint32_t sim_frame;
        uint32_t game_mode;
        uint32_t substate;
        uint32_t substate_timer;
        uint32_t game_type;
        uint32_t match_phase_timer;
        uint32_t input_read_idx;
        uint32_t input_write_idx;
        uint32_t effect_index;
        uint32_t frame_display;
    } header{};
    header.rng_seed = snapshot->rng_seed;
    header.sim_frame = snapshot->sim_frame;
    header.game_mode = snapshot->game_mode;
    header.substate = snapshot->substate;
    header.substate_timer = snapshot->substate_timer;
    header.game_type = snapshot->game_type;
    header.match_phase_timer = snapshot->match_phase_timer;
    header.input_read_idx = snapshot->input_read_idx;
    header.input_write_idx = snapshot->input_write_idx;
    header.effect_index = snapshot->effect_index;
    header.frame_display = snapshot->frame_display;
    h = Block64_Update(h, &header, sizeof(header));
    h = HashMainMasked(h, snapshot->main_state);
    h = Block64_Update(h, snapshot->ai_learn, sizeof(snapshot->ai_learn));
    h = Block64_Update(h, snapshot->input_p1, sizeof(snapshot->input_p1));
    h = Block64_Update(h, snapshot->input_p2, sizeof(snapshot->input_p2));
    return h;
}

bool GameSnapshot_HashGameplayLive(uint64_t* outHash) {
    if (!outHash) {
        return false;
    }
    *outHash = 0;

    // MUST mirror GameSnapshot_HashGameplay exactly: same SimHeader field
    // order, same region order, same digest-mask segmentation, same Block64
    // chaining — a captured snapshot of this instant hashes to the identical
    // value (M7 verification seam).
    struct SimHeader {
        uint32_t rng_seed;
        uint32_t sim_frame;
        uint32_t game_mode;
        uint32_t substate;
        uint32_t substate_timer;
        uint32_t game_type;
        uint32_t match_phase_timer;
        uint32_t input_read_idx;
        uint32_t input_write_idx;
        uint32_t effect_index;
        uint32_t frame_display;
    } header{};

    uint64_t h = BLOCK64_SEED;
    __try {
        header.rng_seed = DetVer_GetRngSeed();
        header.sim_frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        header.game_mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
        header.substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
        header.substate_timer = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
        header.game_type = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
        header.match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
        header.input_read_idx = ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX);
        header.input_write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);
        header.effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
        header.frame_display = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);
        h = Block64_Update(h, &header, sizeof(header));
        h = HashMainMasked(h, (const uint8_t*)kMainStart);
        h = Block64_Update(h, (const void*)kAiLearnStart, kAiLearnSize);
        h = Block64_Update(h, (const void*)kInputP1Start, kInputSize);
        h = Block64_Update(h, (const void*)kInputP2Start, kInputSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *outHash = h;
    return true;
}

} // namespace Rollback
