#include "rollback/game_snapshot.h"

#include "patches/memory_utils.h"
#include "rollback/determinism_verify.h"
#include "ui/log_window.h"

#include <string.h>
#include <windows.h>

namespace Rollback {

namespace {

constexpr uintptr_t kMainStart = ADDR_MATCH_BASE;
constexpr size_t kMainSize = GAME_SNAPSHOT_MAIN_SIZE;
constexpr uintptr_t kPreMatchStart = ADDR_PRE_MATCH_GAP;
constexpr size_t kPreMatchSize = GAME_SNAPSHOT_PRE_MATCH_SIZE;
constexpr uintptr_t kInputP1Start = ADDR_P1_INPUT_BUFFER;
constexpr uintptr_t kInputP2Start = ADDR_P2_INPUT_BUFFER;
constexpr size_t kInputSize = GAME_SNAPSHOT_INPUT_SIZE;

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

    DetVer_SetRngSeed(snapshot->rng_seed);
    WriteMemory<uint32_t>(ADDR_EFFECT_INDEX, snapshot->effect_index);

    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, snapshot->sim_frame);
    WriteMemory<uint32_t>(ADDR_FRAME_COUNTER, snapshot->display_frame);
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

    __try {
        memset((void*)ADDR_MATCH_PER_FRAME_TEMP, 0, MATCH_PER_FRAME_TEMP_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    return true;
}

} // namespace Rollback
