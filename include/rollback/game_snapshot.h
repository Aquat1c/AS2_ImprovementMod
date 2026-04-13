#pragma once

#include "as2_constants.h"

#include <stddef.h>
#include <stdint.h>

namespace Rollback {

static constexpr size_t GAME_SNAPSHOT_MAIN_SIZE =
    (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE;
static constexpr size_t GAME_SNAPSHOT_PRE_MATCH_SIZE = PRE_MATCH_GAP_SIZE;
static constexpr size_t GAME_SNAPSHOT_INPUT_SIZE = INPUT_BUFFER_SIZE;

struct GameSnapshot {
    bool     valid;
    int32_t  frame;
    uint32_t checksum;
    uint32_t rng_seed;

    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t game_mode;
    uint32_t substate;
    uint32_t substate_timer;
    uint32_t game_type;
    uint32_t match_phase_timer;
    uint32_t input_read_idx;
    uint32_t input_write_idx;

    uint8_t main_state[GAME_SNAPSHOT_MAIN_SIZE];
    uint8_t pre_match_gap[GAME_SNAPSHOT_PRE_MATCH_SIZE];
    uint8_t input_p1[GAME_SNAPSHOT_INPUT_SIZE];
    uint8_t input_p2[GAME_SNAPSHOT_INPUT_SIZE];
};

void GameSnapshot_Clear(GameSnapshot* snapshot);
bool GameSnapshot_Capture(GameSnapshot* snapshot, int32_t frame);
bool GameSnapshot_Restore(const GameSnapshot* snapshot);

} // namespace Rollback