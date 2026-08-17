/**
 * Alice Senki 2 - Desync State Dump Implementation
 *
 * Comprehensive game state snapshot for desync diagnosis.
 * Includes: entity combat fields, input buffers, per-region CRC,
 * entity sub-region CRC, hex dumps, checksum history, and more.
 */

#include "rollback/desync_dump.h"
#include "rollback/desync_fine_diag.h"
#include "rollback/rollback_debug.h"
#include "rollback/rollback_session.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "ui/log_window.h"

#include <string.h>
#include <windows.h>
#include <stdio.h>
#include <xmmintrin.h>

namespace Rollback {

// ============================================================================
// Cooldown State
// ============================================================================

static DWORD s_lastDumpTickMs = 0;
static int   s_dumpCount      = 0;
static constexpr DWORD kDumpCooldownMs = 10000; // 10 seconds real-time

// ============================================================================
// Shared region table (diagnostics view of the snapshot capture membership;
// this table never drives capture/restore — see game_snapshot.cpp for that)
// ============================================================================

static const DesyncRegionInfo s_regionTable[] = {
    { "match_header",   ADDR_MATCH_BASE, 16 },
    { "match_context",  ADDR_MATCH_BASE + 16,
                        ADDR_EFFECT_ARRAY - (ADDR_MATCH_BASE + 16) },
    { "effect_array",   ADDR_EFFECT_ARRAY, ADDR_SUMMON_ARRAY - ADDR_EFFECT_ARRAY },
    { "effect_index",   ADDR_EFFECT_INDEX, sizeof(uint32_t) },
    { "summon_array",   ADDR_SUMMON_ARRAY, ADDR_P1_ENTITY_BASE - ADDR_SUMMON_ARRAY },
    { "p1_entity",      ADDR_P1_ENTITY_BASE, (size_t)ENTITY_SIZE },
    { "p2_entity",      ADDR_P2_ENTITY_BASE, (size_t)ENTITY_SIZE },
    { "pre_match_gap",  ADDR_PRE_MATCH_GAP, (size_t)PRE_MATCH_GAP_SIZE },
    { "p1_inputbuf",    ADDR_P1_INPUT_BUFFER, (size_t)INPUT_BUFFER_SIZE },
    { "p2_inputbuf",    ADDR_P2_INPUT_BUFFER, (size_t)INPUT_BUFFER_SIZE },
    { "per_frame_temp", ADDR_MATCH_PER_FRAME_TEMP, (size_t)MATCH_PER_FRAME_TEMP_SIZE },
};

size_t DesyncDump_GetRegionTable(const DesyncRegionInfo** out) {
    if (out) *out = s_regionTable;
    return sizeof(s_regionTable) / sizeof(s_regionTable[0]);
}

void DesyncDump_WriteRegionCRCsMachine(FILE* f) {
    if (!f) return;
    const DesyncRegionInfo* regions = nullptr;
    const size_t n = DesyncDump_GetRegionTable(&regions);
    for (size_t i = 0; i < n; ++i) {
        uint32_t crc = 0;
        __try {
            crc = CalcCRC32((const void*)regions[i].addr, regions[i].size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            crc = 0xDEADDEAD;
        }
        fprintf(f, "REGION name=%s addr=0x%08X size=%zu crc=0x%08X\n",
                regions[i].name, (uint32_t)regions[i].addr, regions[i].size, crc);
    }
}

// ============================================================================
// Helpers
// ============================================================================

static const char* FormatInput(uint16_t inp, char* buf, size_t bufSize) {
    // Produce a human-readable button string like "U+D+L+R+A+B+C+D+St+Se"
    buf[0] = '\0';
    size_t pos = 0;
    auto append = [&](const char* s) {
        size_t len = strlen(s);
        if (pos + len + 1 < bufSize) {
            if (pos > 0) buf[pos++] = '+';
            memcpy(buf + pos, s, len);
            pos += len;
            buf[pos] = '\0';
        }
    };
    if (inp & JOY_UP)      append("U");
    if (inp & JOY_DOWN)    append("D");
    if (inp & JOY_LEFT)    append("L");
    if (inp & JOY_RIGHT)   append("R");
    if (inp & JOY_BTN_A)   append("A");
    if (inp & JOY_BTN_B)   append("B");
    if (inp & JOY_BTN_C)   append("C");
    if (inp & JOY_BTN_D)   append("D");
    if (inp & JOY_START)   append("St");
    if (inp & JOY_SELECT)  append("Se");
    if (inp == 0)          append("none");
    return buf;
}

// ============================================================================
// Public: Hex Dump Region
// ============================================================================

void DesyncDump_HexDumpRegion(FILE* f, const char* label, uintptr_t addr, size_t size) {
    fprintf(f, "\n--- %s (0x%08X, %zu bytes, CRC32=0x%08X) ---\n",
            label, (uint32_t)addr, size,
            CalcCRC32((const void*)addr, size));
    const uint8_t* p = (const uint8_t*)addr;
    for (size_t i = 0; i < size; i += 16) {
        fprintf(f, "  %08X: ", (uint32_t)(addr + i));
        size_t rowEnd = (i + 16 < size) ? 16 : (size - i);
        for (size_t j = 0; j < rowEnd; ++j)
            fprintf(f, "%02X ", p[i + j]);
        // Pad short rows
        for (size_t j = rowEnd; j < 16; ++j)
            fprintf(f, "   ");
        // ASCII column
        fprintf(f, " |");
        for (size_t j = 0; j < rowEnd; ++j) {
            uint8_t c = p[i + j];
            fprintf(f, "%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        fprintf(f, "|\n");
    }
}

// ============================================================================
// Public: Entity Detail Dump
// ============================================================================

void DesyncDump_WriteEntityDetail(FILE* f, const char* label, uintptr_t base) {
    fprintf(f, "\n--- %s Entity Detail (0x%08X, %d bytes) ---\n",
            label, (uint32_t)base, ENTITY_SIZE);
    fprintf(f, "  Full CRC:        0x%08X\n", CalcCRC32((const void*)base, ENTITY_SIZE));

    // === Core vitals ===
    fprintf(f, "  [Core Vitals]\n");
    fprintf(f, "    HP:            %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_HP));
    fprintf(f, "    Meter:         %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_METER));
    fprintf(f, "    X:             %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_X_POS));
    fprintf(f, "    Y:             %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_Y_POS));
    fprintf(f, "    PushDir:       %d\n",  ReadMemory<int8_t>(base + ENTITY_OFF_PUSH_DIR));
    fprintf(f, "    Facing:        %d\n",  ReadMemory<int8_t>(base + ENTITY_OFF_FACING));
    fprintf(f, "    Flag_CE:       %d\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_FLAG_CE));

    // === Physics / velocity (0xBE-0xCE range) ===
    fprintf(f, "  [Physics / Velocity]\n");
    fprintf(f, "    X_Vel:         %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_X_VEL));
    fprintf(f, "    Y_Vel:         %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_Y_VEL));
    fprintf(f, "    X_Accel:       %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_X_ACCEL));
    fprintf(f, "    Y_Accel:       %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_Y_ACCEL));
    fprintf(f, "    Core_C6:       %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_CORE_C6));
    fprintf(f, "    Core_C8:       %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_CORE_C8));
    fprintf(f, "    Core_CA:       %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_CORE_CA));
    fprintf(f, "    Core_CC:       %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_CORE_CC));
    fprintf(f, "    Core_CE:       %d\n",  ReadMemory<int16_t>(base + ENTITY_OFF_CORE_CE));

    // === Action state ===
    fprintf(f, "  [Action State]\n");
    fprintf(f, "    ActionID:      %u\n",  ReadMemory<uint32_t>(base + ENTITY_OFF_ACTION_ID));
    fprintf(f, "    Animation:     %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_ANIMATION));
    fprintf(f, "    AnimIndex:     %u\n",  ReadMemory<uint32_t>(base + ENTITY_OFF_ANIM_INDEX));
    fprintf(f, "    ActionPhase:   %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_ACTION_PHASE));
    fprintf(f, "    ActionFrame:   %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_ACTION_FRAME));
    fprintf(f, "    ActionPriority:%u\n",  ReadMemory<uint32_t>(base + ENTITY_OFF_ACTION_PRIORITY));

    // === Hitstun / Blockstun / Knockback ===
    fprintf(f, "  [Hit/Block State]\n");
    fprintf(f, "    HitstunDuration: %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_HITSTUN_DURATION));
    fprintf(f, "    HitstunPrimary:  %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_HITSTUN_PRIMARY));
    fprintf(f, "    HitRecovery:     %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_HIT_RECOVERY));
    fprintf(f, "    Blockstun:       %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_BLOCKSTUN));
    fprintf(f, "    Blockstun2:      %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_BLOCKSTUN2));
    fprintf(f, "    HitReaction:     %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_HIT_REACTION));
    fprintf(f, "    KnockbackTimer:  %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_KNOCKBACK_TIMER));
    fprintf(f, "    KnockbackForce:  %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_KNOCKBACK_FORCE));
    fprintf(f, "    HitTimerBase:    %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_HIT_TIMER_BASE));
    fprintf(f, "    HitEffect:       %u\n",  ReadMemory<uint32_t>(base + ENTITY_OFF_HIT_EFFECT));

    // === Damage tracking ===
    fprintf(f, "  [Damage]\n");
    fprintf(f, "    ChipDamage:    %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_CHIP_DAMAGE));
    fprintf(f, "    CurrentDamage: %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_CURRENT_DAMAGE));

    // === Combo tracking ===
    fprintf(f, "  [Combo]\n");
    fprintf(f, "    ComboHUD:      %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_DISPLAY_COMBO_COUNT));
    fprintf(f, "    ComboRawA:     %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_MAX_HIT_RAW_A));
    fprintf(f, "    ComboScale1:   %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE1));
    fprintf(f, "    ComboScale2:   %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE2));
    fprintf(f, "    ComboScale3:   %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE3));
    fprintf(f, "    ComboScale4:   %u\n",  ReadMemory<uint8_t>(base + ENTITY_OFF_COMBO_SCALE4));

    // === Guard / Character-specific state ===
    fprintf(f, "  [Guard/Char State]\n");
    fprintf(f, "    GuardGauge:    %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_GUARD_GAUGE));
    fprintf(f, "    HPDisplay:     %u\n",  ReadMemory<uint16_t>(base + ENTITY_OFF_HP_DISPLAY));
    fprintf(f, "    GameState:     %u\n",  ReadMemory<uint32_t>(base + ENTITY_OFF_GAME_STATE));

    // === State block A (0x690-0x6CC) ===
    fprintf(f, "  [State Block A: 0x690-0x6CC]  CRC=0x%08X\n",
            CalcCRC32((const void*)(base + ENTITY_STATE_A_START),
                      ENTITY_STATE_A_END - ENTITY_STATE_A_START));

    // === State block B (0x6CC-0x778) ===
    fprintf(f, "  [State Block B: 0x6CC-0x778]  CRC=0x%08X\n",
            CalcCRC32((const void*)(base + ENTITY_STATE_B_START),
                      ENTITY_STATE_B_END - ENTITY_STATE_B_START));

    // === Combat block (0x778-0x7D0) ===
    fprintf(f, "  [Combat Block: 0x778-0x7D0]   CRC=0x%08X\n",
            CalcCRC32((const void*)(base + ENTITY_COMBAT_START),
                      ENTITY_COMBAT_END - ENTITY_COMBAT_START));

    // === Timer block (0x7A0-0x7B4) ===
    fprintf(f, "  [Timer Block: 0x7A0-0x7B4]    CRC=0x%08X\n",
            CalcCRC32((const void*)(base + ENTITY_TIMER_START),
                      ENTITY_TIMER_END - ENTITY_TIMER_START));

    // === Action buffer (0x444-0x4C0) ===
    fprintf(f, "  [Action Buffer: 0x444-0x4C0]  CRC=0x%08X\n",
            CalcCRC32((const void*)(base + ENTITY_INPUT_START),
                      ENTITY_ACTION_END - ENTITY_INPUT_START));

    // === Entity sub-region CRC breakdown ===
    fprintf(f, "  [Sub-Region CRC Breakdown]\n");
    struct SubRegion { const char* name; uint32_t off; uint32_t size; };
    SubRegion regions[] = {
        { "Core (0x00-0xD0)",          0x0000, 0x00D0 },
        { "Mid (0xD0-0x444)",          0x00D0, 0x0444 - 0x00D0 },
        { "InputAction (0x444-0x4C0)", ENTITY_INPUT_START, ENTITY_ACTION_END - ENTITY_INPUT_START },
        { "Gap (0x4C0-0x690)",         0x04C0, 0x0690 - 0x04C0 },
        { "StateA (0x690-0x6CC)",      ENTITY_STATE_A_START, ENTITY_STATE_A_END - ENTITY_STATE_A_START },
        { "StateB (0x6CC-0x778)",      ENTITY_STATE_B_START, ENTITY_STATE_B_END - ENTITY_STATE_B_START },
        { "Combat (0x778-0x7D0)",      ENTITY_COMBAT_START, ENTITY_COMBAT_END - ENTITY_COMBAT_START },
        { "Timers (0x7D0-0x1000)",     0x07D0, 0x1000 - 0x07D0 },
        { "AnimData (0x1000-0xA000)",  0x1000, 0xA000 - 0x1000 },
        { "CharState (0xA000-0xA660)", 0xA000, 0x0660 },
        { "Tail (0xA660-end)",         0xA660, ENTITY_SIZE - 0xA660 },
    };
    for (auto& r : regions) {
        if (r.off + r.size <= (uint32_t)ENTITY_SIZE) {
            fprintf(f, "    %-30s CRC=0x%08X\n", r.name,
                    CalcCRC32((const void*)(base + r.off), r.size));
        }
    }

    // === Current input from entity input buffer (offset 0x444 area) ===
    fprintf(f, "  [Entity Input Buffer Raw: +0x444..+0x46C]\n");
    fprintf(f, "   ");
    const uint8_t* inp = (const uint8_t*)(base + ENTITY_INPUT_START);
    for (uint32_t i = 0; i < (ENTITY_INPUT_END - ENTITY_INPUT_START); ++i)
        fprintf(f, "%02X ", inp[i]);
    fprintf(f, "\n");

    // === Opponent pointer ===
    fprintf(f, "  OpponentPtr:     0x%08X\n",
            ReadMemory<uint32_t>(base + ENTITY_OFF_OPPONENT));
}

// ============================================================================
// Public: Full Dump
// ============================================================================

void DesyncDump_WriteFullDump(FILE* f, const DesyncDumpParams& params) {
    // ====== HEADER ======
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "================================================================================\n");
    fprintf(f, "  DESYNC STATE DUMP #%d\n", params.dump_number);
    fprintf(f, "================================================================================\n");
    fprintf(f, "Time:       %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fprintf(f, "PID:        %lu\n", GetCurrentProcessId());
    fprintf(f, "RB Frame:   %d\n", params.frame);
    fprintf(f, "Source:     %s\n", params.source ? params.source : "unknown");
    if (params.detail && params.detail[0]) {
        fprintf(f, "Detail:     %s\n", params.detail);
    }
    fprintf(f, "Local CRC:  0x%08X\n", params.local_crc);
    fprintf(f, "Remote CRC: 0x%08X\n", params.remote_crc);
    fprintf(f, "\n");

    // ====== ROLLBACK SESSION ======
    RollbackSessionSnapshot snap{};
    RollbackSession_GetSnapshot(&snap);
    fprintf(f, "--- Rollback Session ---\n");
    fprintf(f, "  frame_origin_abs:   %d\n", snap.frame_origin_abs);
    fprintf(f, "  game_abs_frame:     %d\n", snap.game_abs_frame_current);
    fprintf(f, "  rb_frame_current:   %d\n", snap.rb_frame_current);
    fprintf(f, "  rb_confirmed:       %d\n", snap.rb_frame_last_confirmed);
    fprintf(f, "  rb_remote_recv:     %d\n", snap.rb_frame_last_remote_received);
    fprintf(f, "  rollback_count:     %d\n", snap.rollback_count);
    fprintf(f, "  max_rollback_dist:  %d\n", snap.max_rollback_distance);
    fprintf(f, "  predicted_frames:   %d\n", snap.predicted_frames_outstanding);
    fprintf(f, "  mispredictions:     %d\n", snap.total_mispredictions);
    fprintf(f, "  active_delay:       %d\n", snap.active_delay);
    fprintf(f, "  rollback_budget:    %d\n", snap.rollback_budget);
    // Computed explicitly at dump time (GetSnapshot no longer hides the
    // 253 KB CRC pass — see rollback_session_engine2.cpp PERF note).
    fprintf(f, "  current_checksum:   0x%08X\n",
            RollbackSession_ComputeLiveStateChecksum());

    // ====== GLOBAL GAME STATE ======
    fprintf(f, "\n--- Global Game State ---\n");
    fprintf(f, "  game_mode:          %u\n", ReadMemory<uint32_t>(ADDR_GAME_MODE));
    fprintf(f, "  sub_state:          %u\n", ReadMemory<uint32_t>(ADDR_SUB_STATE));
    fprintf(f, "  sub_state_timer:    %u\n", ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER));
    fprintf(f, "  game_type:          %u\n", ReadMemory<uint32_t>(ADDR_GAME_TYPE));
    fprintf(f, "  sim_frame:          %u\n", ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
    fprintf(f, "  display_frame:      %u\n", ReadMemory<uint32_t>(ADDR_FRAME_COUNTER));
    fprintf(f, "  write_idx:          %u\n", ReadMemory<uint32_t>(ADDR_FRAME_WRITE_IDX));
    fprintf(f, "  net_idx:            %u\n", ReadMemory<uint32_t>(ADDR_FRAME_NET_IDX));
    fprintf(f, "  match_phase_timer:  %u\n", ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER));
    fprintf(f, "  round_timer:        %u\n", ReadMemory<uint32_t>(ADDR_ROUND_TIMER));
    fprintf(f, "  current_player:     %u\n", ReadMemory<uint32_t>(ADDR_CURRENT_PLAYER));
    fprintf(f, "  netplay_role:       %u\n", ReadMemory<uint8_t>(ADDR_NETPLAY_ROLE));
    fprintf(f, "  netplay_connected:  %u\n", ReadMemory<uint8_t>(ADDR_NETPLAY_CONNECTED));

    // ====== RNG ======
    uint32_t rng = DetVer_GetRngSeed();
    fprintf(f, "\n--- RNG ---\n");
    fprintf(f, "  seed:               0x%08X\n", rng);

    // ====== FPU ======
    uint16_t fpuCW = 0;
    uint32_t mxcsr = 0;
    __asm { fnstcw fpuCW }
    mxcsr = _mm_getcsr();
    fprintf(f, "\n--- FPU State ---\n");
    fprintf(f, "  x87 CW:            0x%04X\n", fpuCW);
    fprintf(f, "  MXCSR:             0x%08X\n", mxcsr);

    // ====== DETERMINISM TRACE ======
    const DeterminismFrameTrace* trace = DetVer_GetLatestTrace();
    if (trace && trace->frame >= 0) {
        fprintf(f, "\n--- Det-Ver Latest Trace (frame %d) ---\n", trace->frame);
        fprintf(f, "  rng_begin:  0x%08X  rng_end: 0x%08X  rand_calls: %d\n",
                trace->rng_begin, trace->rng_end, trace->rand_calls);
        fprintf(f, "  fpu_cw:     0x%04X->0x%04X  mxcsr: 0x%08X->0x%08X\n",
                trace->fpu_cw_begin, trace->fpu_cw_end,
                trace->mxcsr_begin, trace->mxcsr_end);
        fprintf(f, "  checksum:   0x%08X\n", trace->checksum);
        fprintf(f, "  callers:   ");
        for (int i = 0; i < DETVER_MAX_CALLERS && trace->rand_callers[i]; ++i)
            fprintf(f, " 0x%08X", trace->rand_callers[i]);
        fprintf(f, "\n");
    }

    // ====== P1 ENTITY (FULL DETAIL) ======
    DesyncDump_WriteEntityDetail(f, "P1", ADDR_P1_ENTITY_BASE);

    // ====== P2 ENTITY (FULL DETAIL) ======
    DesyncDump_WriteEntityDetail(f, "P2", ADDR_P2_ENTITY_BASE);

    // ====== CURRENT INPUTS (decoded) ======
    fprintf(f, "\n--- Current Inputs (from global input buffers) ---\n");
    {
        char buf1[128], buf2[128];
        uint16_t p1_held = ReadMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + INPUT_OFF_CURRENT);
        uint16_t p1_prev = ReadMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + INPUT_OFF_PREVIOUS);
        uint16_t p1_just = ReadMemory<uint16_t>(ADDR_P1_INPUT_BUFFER + INPUT_OFF_JUST_PRESSED);
        uint16_t p2_held = ReadMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + INPUT_OFF_CURRENT);
        uint16_t p2_prev = ReadMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + INPUT_OFF_PREVIOUS);
        uint16_t p2_just = ReadMemory<uint16_t>(ADDR_P2_INPUT_BUFFER + INPUT_OFF_JUST_PRESSED);

        fprintf(f, "  P1 held:    0x%04X  %s\n", p1_held, FormatInput(p1_held, buf1, sizeof(buf1)));
        fprintf(f, "  P1 prev:    0x%04X  %s\n", p1_prev, FormatInput(p1_prev, buf2, sizeof(buf2)));
        fprintf(f, "  P1 just:    0x%04X  %s\n", p1_just, FormatInput(p1_just, buf1, sizeof(buf1)));
        fprintf(f, "  P2 held:    0x%04X  %s\n", p2_held, FormatInput(p2_held, buf2, sizeof(buf2)));
        fprintf(f, "  P2 prev:    0x%04X  %s\n", p2_prev, FormatInput(p2_prev, buf1, sizeof(buf1)));
        fprintf(f, "  P2 just:    0x%04X  %s\n", p2_just, FormatInput(p2_just, buf2, sizeof(buf2)));
    }

    // ====== INPUT HISTORY (decoded, extended window) ======
    fprintf(f, "\n--- Input History (near frame %d) ---\n", params.frame);
    uint32_t writeIdx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);
    fprintf(f, "  write_idx=%u  read_idx=%u  net_idx=%u\n",
            writeIdx,
            ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX),
            ReadMemory<uint32_t>(ADDR_INPUT_NET_IDX));
    for (int delta = -16; delta <= 0; ++delta) {
        int32_t idx = (int32_t)writeIdx + delta;
        if (idx < 0 || idx >= (int32_t)INPUT_HISTORY_MAX) continue;
        uint16_t p1 = *(uint16_t*)(ADDR_P1_INPUT_HISTORY + idx * sizeof(uint16_t));
        uint16_t p2 = *(uint16_t*)(ADDR_P2_INPUT_HISTORY + idx * sizeof(uint16_t));
        char buf1[128], buf2[128];
        fprintf(f, "  idx=%-6d  P1=0x%04X %-20s  P2=0x%04X %-20s%s\n",
                idx, p1, FormatInput(p1, buf1, sizeof(buf1)),
                p2, FormatInput(p2, buf2, sizeof(buf2)),
                (delta == 0) ? "  <-- current" : "");
    }

    // ====== CHECKSUM HISTORY ======
    fprintf(f, "\n--- Checksum History (near frame %d) ---\n", params.frame);
    for (int delta = -16; delta <= 8; ++delta) {
        int32_t f2 = params.frame + delta;
        if (f2 < 0) continue;
        uint32_t crc = 0;
        if (RollbackDebug_TryGetChecksumForFrame(f2, &crc)) {
            fprintf(f, "  f%-6d  crc=0x%08X%s\n", f2, crc,
                    (f2 == params.frame) ? "  <-- DESYNC" : "");
        }
    }

    // ====== PER-REGION CRC BREAKDOWN ======
    // (single shared table — the machine `REGION` lines use the same one)
    fprintf(f, "\n--- Region CRC Breakdown ---\n");
    {
        const DesyncRegionInfo* regions = nullptr;
        const size_t regionCount = DesyncDump_GetRegionTable(&regions);
        for (size_t i = 0; i < regionCount; ++i) {
            fprintf(f, "  %-15s (0x%08X, %6zu B): 0x%08X\n",
                    regions[i].name, (uint32_t)regions[i].addr, regions[i].size,
                    CalcCRC32((const void*)regions[i].addr, regions[i].size));
        }
        fprintf(f, "  effect_index value=%u\n",
                ReadMemory<uint32_t>(ADDR_EFFECT_INDEX));
    }

    // ====== SCATTERED GLOBALS (savestate) ======
    fprintf(f, "\n--- Scattered Globals (savestate tracked) ---\n");
    fprintf(f, "  RNG seed:           0x%08X\n", rng);
    fprintf(f, "  sim_frame (0x%X):   %u\n",  ADDR_SIM_FRAME_COUNTER,
            ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
    fprintf(f, "  display_frame:      %u\n", ReadMemory<uint32_t>(ADDR_FRAME_COUNTER));
    fprintf(f, "  game_mode:          %u\n", ReadMemory<uint32_t>(ADDR_GAME_MODE));
    fprintf(f, "  sub_state:          %u\n", ReadMemory<uint32_t>(ADDR_SUB_STATE));
    fprintf(f, "  sub_state_timer:    %u\n", ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER));
    fprintf(f, "  game_type:          %u\n", ReadMemory<uint32_t>(ADDR_GAME_TYPE));
    fprintf(f, "  match_phase_timer:  %u\n", ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER));
    fprintf(f, "  effect_index:       %u\n", ReadMemory<uint32_t>(ADDR_EFFECT_INDEX));
    fprintf(f, "  read_idx:           %u\n", ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX));
    fprintf(f, "  write_idx:          %u\n", ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX));
    fprintf(f, "  host_timeout_ctr:   %u\n", ReadMemory<uint32_t>(ADDR_HOST_TIMEOUT_CTR));
    fprintf(f, "  client_timeout_ctr: %u\n", ReadMemory<uint32_t>(ADDR_CLIENT_TIMEOUT_CTR));

    // ====== HEX DUMPS ======
    // Input buffers (full 208 bytes)
    DesyncDump_HexDumpRegion(f, "P1 Input Buffer", ADDR_P1_INPUT_BUFFER, INPUT_BUFFER_SIZE);
    DesyncDump_HexDumpRegion(f, "P2 Input Buffer", ADDR_P2_INPUT_BUFFER, INPUT_BUFFER_SIZE);

    // Match header area
    DesyncDump_HexDumpRegion(f, "Match Header+Start (first 64B)", ADDR_MATCH_BASE, 64);

    // Pre-match gap
    DesyncDump_HexDumpRegion(f, "Pre-Match Gap", ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE);

    // Per-frame temp
    DesyncDump_HexDumpRegion(f, "Per-Frame Temp", ADDR_MATCH_PER_FRAME_TEMP, MATCH_PER_FRAME_TEMP_SIZE);

    // Entity core ranges (0x00-0xD0, most critical)
    DesyncDump_HexDumpRegion(f, "P1 Entity Core (0x00-0xD0)", ADDR_P1_ENTITY_BASE, 0xD0);
    DesyncDump_HexDumpRegion(f, "P2 Entity Core (0x00-0xD0)", ADDR_P2_ENTITY_BASE, 0xD0);

    // Entity action/input buffer (0x444-0x4C0)
    DesyncDump_HexDumpRegion(f, "P1 Action Buffer (0x444-0x4C0)",
                             ADDR_P1_ENTITY_BASE + ENTITY_INPUT_START,
                             ENTITY_ACTION_END - ENTITY_INPUT_START);
    DesyncDump_HexDumpRegion(f, "P2 Action Buffer (0x444-0x4C0)",
                             ADDR_P2_ENTITY_BASE + ENTITY_INPUT_START,
                             ENTITY_ACTION_END - ENTITY_INPUT_START);

    // Entity state blocks (0x690-0x778)
    DesyncDump_HexDumpRegion(f, "P1 State A+B (0x690-0x778)",
                             ADDR_P1_ENTITY_BASE + ENTITY_STATE_A_START,
                             ENTITY_STATE_B_END - ENTITY_STATE_A_START);
    DesyncDump_HexDumpRegion(f, "P2 State A+B (0x690-0x778)",
                             ADDR_P2_ENTITY_BASE + ENTITY_STATE_A_START,
                             ENTITY_STATE_B_END - ENTITY_STATE_A_START);

    // Entity combat+timer block (0x778-0x7D0)
    DesyncDump_HexDumpRegion(f, "P1 Combat+Timer (0x778-0x7D0)",
                             ADDR_P1_ENTITY_BASE + ENTITY_COMBAT_START,
                             ENTITY_COMBAT_END - ENTITY_COMBAT_START);
    DesyncDump_HexDumpRegion(f, "P2 Combat+Timer (0x778-0x7D0)",
                             ADDR_P2_ENTITY_BASE + ENTITY_COMBAT_START,
                             ENTITY_COMBAT_END - ENTITY_COMBAT_START);

    fprintf(f, "\n================================================================================\n");
    fprintf(f, "  END OF DUMP\n");
    fprintf(f, "================================================================================\n");
}

// ============================================================================
// Public: Convenience top-level dump with cooldown
// ============================================================================

bool DesyncDump_TryDump(int32_t frame,
                        uint32_t local_crc,
                        uint32_t remote_crc,
                        const char* source,
                        const char* detail) {
    DWORD now = GetTickCount();
    if (s_dumpCount > 0 && (now - s_lastDumpTickMs) < kDumpCooldownMs)
        return false;

    const char* logDir = LogWindow_GetLogDir();
    if (!logDir || !logDir[0]) return false;

    DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\desync_dump_%lu_f%d.txt", logDir, pid, frame);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        LOG_ERROR("[DesyncDump] Failed to write desync dump: %s", path);
        return false;
    }

    s_lastDumpTickMs = now;
    s_dumpCount++;

    DesyncDumpParams params{};
    params.frame       = frame;
    params.local_crc   = local_crc;
    params.remote_crc  = remote_crc;
    params.dump_number = s_dumpCount;
    params.source      = source;
    params.detail      = detail;

    DesyncDump_WriteFullDump(f, params);

    fclose(f);
    LOG_INFO("[DesyncDump] State dump #%d written: %s", s_dumpCount, path);
    return true;
}

bool DesyncDump_TryDumpWithDiagnostics(int32_t frame,
                                       uint32_t local_crc,
                                       uint32_t remote_crc,
                                       const char* source,
                                       const char* detail,
                                       const DesyncDiagRing* ring,
                                       const DesyncEvidence* evidence) {
    DWORD now = GetTickCount();
    if (s_dumpCount > 0 && (now - s_lastDumpTickMs) < kDumpCooldownMs)
        return false;

    const char* logDir = LogWindow_GetLogDir();
    if (!logDir || !logDir[0]) return false;

    DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\desync_dump_%lu_f%d.txt", logDir, pid, frame);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        LOG_ERROR("[DesyncDump] Failed to write desync dump: %s", path);
        return false;
    }

    s_lastDumpTickMs = now;
    s_dumpCount++;

    // ====== MACHINE-READABLE DIAGNOSTICS (written FIRST, so a truncated
    // file still carries the comparator's input; see
    // tools/compare_desync_dumps.py) ======
    fprintf(f, "================================================================================\n");
    fprintf(f, "  DESYNC DIAGNOSTICS (machine-readable; tools/compare_desync_dumps.py)\n");
    fprintf(f, "================================================================================\n");
    fprintf(f, "DIAG source=%s rb_frame=%d local_crc=0x%08X remote_crc=0x%08X\n",
            source ? source : "unknown", frame, local_crc, remote_crc);
    if (evidence) {
        DesyncDiag_WriteEvidence(f, *evidence);
    } else {
        fprintf(f, "EVIDENCE none\n");
    }
    if (ring) {
        DesyncDiag_WriteRing(f, *ring);
    } else {
        fprintf(f, "RINGCOUNT n=0\n");
    }
    DesyncDump_WriteRegionCRCsMachine(f);
    // F7d fine-grained localization: confirm-seam per-window CRCs + raw
    // context images for the ring frames (comparator: FINE* lines).
    FineDiag_WriteDump(f);
    fprintf(f, "\n");

    // ====== FULL HUMAN-READABLE DUMP ======
    DesyncDumpParams params{};
    params.frame       = frame;
    params.local_crc   = local_crc;
    params.remote_crc  = remote_crc;
    params.dump_number = s_dumpCount;
    params.source      = source;
    params.detail      = detail;
    DesyncDump_WriteFullDump(f, params);

    fclose(f);
    LOG_INFO("[DesyncDump] Diagnostics dump #%d written: %s", s_dumpCount, path);
    return true;
}

bool DesyncDump_TryBaselineMismatchDump(const BaselineMismatchDumpParams& params) {
    const char* logDir = LogWindow_GetLogDir();
    if (!logDir || !logDir[0]) return false;

    const int32_t frame =
        (params.mismatch_frame >= 0)
            ? params.mismatch_frame
            : (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    const DWORD now = GetTickCount();
    s_lastDumpTickMs = now;
    s_dumpCount++;

    DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\baseline_mismatch_%lu_f%d_n%d.txt",
                logDir, pid, frame, s_dumpCount);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        LOG_ERROR("[DesyncDump] Failed to write baseline mismatch dump: %s", path);
        return false;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "================================================================================\n");
    fprintf(f, "  BASELINE MISMATCH DUMP #%d\n", s_dumpCount);
    fprintf(f, "================================================================================\n");
    fprintf(f, "Time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fprintf(f, "Phase: %s\n", params.phase_name ? params.phase_name : "unknown");
    fprintf(f, "Local baseline:  crc=0x%08X mode=%u sub=%u sim=%d\n",
            params.local_crc,
            (unsigned)params.local_mode,
            (unsigned)params.local_substate,
            params.local_sim_frame);
    fprintf(f, "Remote baseline: crc=0x%08X mode=%u sub=%u sim=%d\n",
            params.remote_crc,
            (unsigned)params.remote_mode,
            (unsigned)params.remote_substate,
            params.remote_sim_frame);
    fprintf(f, "Vanilla sync counters (from sub_562550 lineage): sim=%u display=%u write=%u net=%u remote=%u\n",
            params.frame_simulation,
            params.frame_display,
            params.frame_write_idx,
            params.frame_net_idx,
            params.remote_frame_idx);
    fprintf(f, "Determinism seed: 0x%08X\n", params.rng_seed);
    fprintf(f, "================================================================================\n\n");

    DesyncDumpParams full{};
    full.frame = frame;
    full.local_crc = params.local_crc;
    full.remote_crc = params.remote_crc;
    full.dump_number = s_dumpCount;
    DesyncDump_WriteFullDump(f, full);
    fclose(f);

    LOG_INFO("[DesyncDump] Baseline mismatch dump #%d written: %s", s_dumpCount, path);
    return true;
}

// ============================================================================
// Public: Feed checksum (called from rollback_debug each frame)
// ============================================================================

// Kept for API compatibility; authoritative history lives in rollback_debug.
void DesyncDump_StoreChecksum(int32_t frame, uint32_t crc) {
    (void)frame;
    (void)crc;
}

void DesyncDump_Reset() {
    s_lastDumpTickMs = 0;
    s_dumpCount = 0;
}

} // namespace Rollback
