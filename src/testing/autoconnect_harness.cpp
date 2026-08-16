/**
 * Alice Senki 2 - Autoconnect Test Harness Implementation
 *
 * Shared memory writer + fighting game AI for automated testing.
 * Ported from old_files/old_netplay/src/netplay/test_harness.cpp.
 */

#include "testing/autoconnect_harness.h"
#include "testing/harness_shared_memory.h"
#include "testing/rematch_soak.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "core/mod_main.h"
#include "input/input_system.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "rollback/rollback_session.h"
#include "rollback/determinism_verify.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <intrin.h>

// ============================================================================
// Internal state
// ============================================================================

static HANDLE              s_shmHandle = nullptr;
static HarnessSharedData*  s_shm       = nullptr;
static bool                s_active    = false;
static bool                s_isHost    = false;
static int                 s_matchDurationSec = 0;
static uint32_t            s_matchStartFrame  = 0;

// Fighting AI state (reset each match)
static struct {
    enum Action : uint8_t {
        Idle, Approach, Retreat, Jump, AirAttack,
        QCF, DP, HCF,
        DashForward, DashBack,
        Poke, Combo, Throw, Block, Wakeup
    };
    Action   action    = Idle;
    int      step      = 0;
    int      remaining = 0;
    uint32_t rng       = 0;
} s_ai;

// ============================================================================
// Memory helpers
// ============================================================================

static uint8_t  RdU8 (uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a);  }
static int16_t  RdS16(uintptr_t a) { return *reinterpret_cast<volatile int16_t*>(a);  }
static uint16_t RdU16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
static uint32_t RdU32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

// ============================================================================
// Shared memory log ring buffer
// ============================================================================

static void ShmLog(const char* fmt, ...) {
    if (!s_shm) return;
    uint32_t idx = s_shm->log_head % HARNESS_LOG_LINES;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_shm->log_lines[idx], HARNESS_LOG_LINE_LEN, fmt, args);
    va_end(args);
    s_shm->log_head++;
    s_shm->log_count++;
}

// ============================================================================
// SHM create / destroy
// ============================================================================

static bool CreateSharedMemory(bool isHost, const char* nickname) {
    const char* name = isHost ? HARNESS_SHM_NAME_HOST : HARNESS_SHM_NAME_CLIENT;

    s_shmHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(HarnessSharedData), name);

    if (!s_shmHandle) {
        LOG_ERROR("[Harness] CreateFileMapping failed: %lu", GetLastError());
        return false;
    }

    s_shm = (HarnessSharedData*)MapViewOfFile(
        s_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HarnessSharedData));

    if (!s_shm) {
        LOG_ERROR("[Harness] MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(s_shmHandle);
        s_shmHandle = nullptr;
        return false;
    }

    memset(s_shm, 0, sizeof(HarnessSharedData));
    s_shm->magic   = HARNESS_SHM_MAGIC;
    s_shm->version = HARNESS_SHM_VERSION;
    s_shm->active  = 1;
    s_shm->is_host = isHost ? 1 : 0;
    if (nickname)
        strncpy_s(s_shm->nickname, sizeof(s_shm->nickname), nickname, _TRUNCATE);

    LOG_INFO("[Harness] Shared memory created: %s", name);
    return true;
}

static void DestroySharedMemory() {
    if (s_shm) {
        s_shm->active = 0;
        s_shm->write_seq++;
        UnmapViewOfFile(s_shm);
        s_shm = nullptr;
    }
    if (s_shmHandle) {
        CloseHandle(s_shmHandle);
        s_shmHandle = nullptr;
    }
}

// ============================================================================
// Game state snapshot → SHM
// ============================================================================

static void FlushToSharedMemory(const char* phaseName, uint32_t phaseOrdinal, uint32_t frameCounter) {
    if (!s_shm) return;

    s_shm->phase = phaseOrdinal;
    s_shm->frame_counter = frameCounter;
    if (phaseName)
        strncpy_s(s_shm->phase_name, sizeof(s_shm->phase_name), phaseName, _TRUNCATE);

    // Session info
    {
        Net::SessionSnapshot snap{};
        Net::Session_GetSnapshot(&snap);
        s_shm->session_state    = (uint32_t)snap.state;
        s_shm->conn_rtt_ms      = snap.stats.rtt_ms;
        s_shm->packets_sent     = snap.stats.packets_sent;
        s_shm->packets_received = snap.stats.packets_received;
        s_shm->desync_count     = 0; // tracked separately
        if (snap.remote_peer.valid)
            strncpy_s(s_shm->peer_nickname, sizeof(s_shm->peer_nickname),
                      snap.remote_peer.nickname, _TRUNCATE);
        strncpy_s(s_shm->status_text, sizeof(s_shm->status_text),
                  snap.status_text, _TRUNCATE);
        strncpy_s(s_shm->error_text, sizeof(s_shm->error_text),
                  snap.error_text, _TRUNCATE);
    }

    // Game state
    s_shm->game_mode     = GetGameMode();
    s_shm->sub_state     = GetSubstate();
    s_shm->game_type     = GetGameType();
    s_shm->sim_frame     = RdU32(ADDR_FRAME_SIMULATION);
    s_shm->display_frame = RdU32(ADDR_FRAME_DISPLAY);
    s_shm->write_frame   = RdU32(ADDR_FRAME_WRITE_IDX);
    s_shm->net_frame     = RdU32(ADDR_FRAME_NET_IDX);
    s_shm->vanilla_role      = RdU8(ADDR_NETPLAY_ROLE);
    s_shm->vanilla_connected = RdU8(ADDR_NETPLAY_CONNECTED);
    s_shm->rng_seed     = DetVer_GetRngSeed();
    s_shm->checksum     = AS2_GetQuickChecksum();

    // Entity state: reconstruct combined input bitmask from per-button held arrays
    {
        static const uint16_t kButtonMasks[10] = {
            0x0001, 0x0002, 0x0004, 0x0008, 0x0010,
            0x0020, 0x0040, 0x0080, 0x0100, 0x0200
        };
        uint16_t p1 = 0, p2 = 0;
        for (int i = 0; i < 10; i++) {
            if (RdU16(ADDR_P1_INPUT_BUFFER + i * 2)) p1 |= kButtonMasks[i];
            if (RdU16(ADDR_P2_INPUT_BUFFER + i * 2)) p2 |= kButtonMasks[i];
        }
        s_shm->p1_input = p1;
        s_shm->p2_input = p2;
    }

    uintptr_t p1base = GetEntityBase(0);
    uintptr_t p2base = GetEntityBase(1);
    if (p1base) {
        s_shm->p1_hp     = RdS16(p1base + ENTITY_OFF_HP);
        s_shm->p1_meter  = RdU16(p1base + ENTITY_OFF_METER);
        s_shm->p1_x      = RdS16(p1base + ENTITY_OFF_X_POS);
        s_shm->p1_y      = RdS16(p1base + ENTITY_OFF_Y_POS);
        s_shm->p1_action  = RdU32(p1base + ENTITY_OFF_ACTION_ID);
    }
    if (p2base) {
        s_shm->p2_hp     = RdS16(p2base + ENTITY_OFF_HP);
        s_shm->p2_meter  = RdU16(p2base + ENTITY_OFF_METER);
        s_shm->p2_x      = RdS16(p2base + ENTITY_OFF_X_POS);
        s_shm->p2_y      = RdS16(p2base + ENTITY_OFF_Y_POS);
        s_shm->p2_action  = RdU32(p2base + ENTITY_OFF_ACTION_ID);
    }

    // Rollback session info
    {
        Rollback::RollbackSessionSnapshot rbSnap{};
        Rollback::RollbackSession_GetSnapshot(&rbSnap);
        if (rbSnap.active) {
            s_shm->rb_local_frame    = rbSnap.rb_frame_current;
            s_shm->rb_remote_frame   = rbSnap.rb_frame_last_remote_received;
            s_shm->rb_frames_ahead   = (float)(rbSnap.rb_frame_current - rbSnap.rb_frame_last_remote_received);
            s_shm->rb_state          = rbSnap.is_rolling_back ? 2 : 1;
            s_shm->rb_advance_count  = (uint32_t)rbSnap.rb_frame_current;

            s_shm->total_rollbacks        = (uint32_t)rbSnap.rollback_count;
            s_shm->max_rollback_depth     = (uint32_t)rbSnap.max_rollback_distance;
            s_shm->total_frames           = (uint32_t)rbSnap.rb_frame_current;
        }
    }

    // Memory fence + sequence bump
    _ReadWriteBarrier();
    s_shm->write_seq++;
}

// ============================================================================
// Public API
// ============================================================================

bool AutoConnectHarness_Init(bool isHost, const char* nickname, int matchDurationSec) {
    if (s_active) return true; // already initialized

    s_isHost = isHost;
    s_matchDurationSec = matchDurationSec;
    s_matchStartFrame = 0;

    // Reset AI
    s_ai = {};
    s_ai.rng = isHost ? 0xDEADBEEF : 0xCAFEBABE;

    if (!CreateSharedMemory(isHost, nickname)) {
        LOG_ERROR("[Harness] Failed to create shared memory");
        return false;
    }

    s_active = true;
    LOG_INFO("[Harness] Initialized: role=%s duration=%ds",
             isHost ? "Host" : "Client", matchDurationSec);
    ShmLog("Harness initialized: %s", isHost ? "Host" : "Client");

    // M6: arm the rematch soak monitor if soak_rematches / AS2_SOAK_REMATCHES
    // is configured (no-op otherwise).
    RematchSoak_Init(isHost);
    return true;
}

void AutoConnectHarness_Update(const char* phaseName, uint32_t phaseOrdinal, uint32_t frameCounter) {
    if (!s_active) return;
    FlushToSharedMemory(phaseName, phaseOrdinal, frameCounter);

    // M6: rematch soak observation (no-op unless armed).
    RematchSoak_FrameUpdate(phaseName, frameCounter);
}

uint16_t AutoConnectHarness_RunFightingAI(bool isHost, uint32_t matchFrame) {
    // Reset AI on first frame of match
    if (matchFrame == 0) {
        s_ai = {};
        s_ai.rng = isHost ? 0xDEADBEEF : 0xCAFEBABE;
        s_matchStartFrame = matchFrame;
    }

    auto aiRand = [&]() -> uint32_t {
        s_ai.rng = s_ai.rng * 214013u + 2531011u;
        return (s_ai.rng >> 16) & 0x7FFF;
    };

    uint16_t input = 0;

    // Pick a new action when current one finishes
    if (s_ai.remaining <= 0) {
        uint32_t r = aiRand() % 100;
        if      (r < 15) { s_ai.action = s_ai.Approach;    s_ai.remaining = 20 + (aiRand() % 30); }
        else if (r < 25) { s_ai.action = s_ai.Retreat;     s_ai.remaining = 15 + (aiRand() % 20); }
        else if (r < 35) { s_ai.action = s_ai.DashForward; s_ai.remaining = 10; }
        else if (r < 42) { s_ai.action = s_ai.DashBack;    s_ai.remaining = 10; }
        else if (r < 52) { s_ai.action = s_ai.Jump;        s_ai.remaining = 6; }
        else if (r < 58) { s_ai.action = s_ai.AirAttack;   s_ai.remaining = 20; }
        else if (r < 68) { s_ai.action = s_ai.Poke;        s_ai.remaining = 8 + (aiRand() % 10); }
        else if (r < 76) { s_ai.action = s_ai.Combo;       s_ai.remaining = 24; }
        else if (r < 82) { s_ai.action = s_ai.QCF;         s_ai.remaining = 8; }
        else if (r < 87) { s_ai.action = s_ai.DP;          s_ai.remaining = 8; }
        else if (r < 91) { s_ai.action = s_ai.HCF;         s_ai.remaining = 12; }
        else if (r < 95) { s_ai.action = s_ai.Throw;       s_ai.remaining = 6; }
        else if (r < 98) { s_ai.action = s_ai.Block;       s_ai.remaining = 20 + (aiRand() % 30); }
        else              { s_ai.action = s_ai.Wakeup;      s_ai.remaining = 10; }
        s_ai.step = 0;
    }

    // Direction toward opponent (host = P1 faces right, client = P2 faces left)
    const uint16_t FWD  = isHost ? INPUT_RIGHT : INPUT_LEFT;
    const uint16_t BACK = isHost ? INPUT_LEFT  : INPUT_RIGHT;

    switch (s_ai.action) {
    case s_ai.Idle:
        break;

    case s_ai.Approach:
        input = FWD;
        if (s_ai.step % 12 < 2) input |= INPUT_A;
        break;

    case s_ai.Retreat:
        input = BACK;
        break;

    case s_ai.DashForward:
        if      (s_ai.step == 0) input = FWD;
        else if (s_ai.step == 1) input = 0;
        else if (s_ai.step == 2) input = FWD;
        else                     input = FWD;
        break;

    case s_ai.DashBack:
        if      (s_ai.step == 0) input = BACK;
        else if (s_ai.step == 1) input = 0;
        else if (s_ai.step == 2) input = BACK;
        else                     input = BACK;
        break;

    case s_ai.Jump:
        if (s_ai.step < 2)      input = INPUT_UP;
        else if (s_ai.step < 4) input = INPUT_UP | FWD;
        else                     input = FWD;
        break;

    case s_ai.AirAttack:
        if      (s_ai.step < 2)  input = INPUT_UP | FWD;
        else if (s_ai.step < 5)  input = FWD;
        else if (s_ai.step == 5) input = FWD | INPUT_A;
        else if (s_ai.step == 8) input = FWD | INPUT_B;
        else if (s_ai.step ==11) input = FWD | INPUT_C;
        else                      input = 0;
        break;

    case s_ai.Poke:
        switch (s_ai.step % 8) {
        case 0: input = FWD | INPUT_A; break;
        case 2: input = FWD | INPUT_B; break;
        case 4: input = INPUT_DOWN | INPUT_A; break;
        case 6: input = INPUT_DOWN | INPUT_B; break;
        default: input = 0; break;
        }
        break;

    case s_ai.Combo:
        switch (s_ai.step) {
        case 0:  input = FWD | INPUT_A; break;
        case 3:  input = INPUT_A; break;
        case 6:  input = INPUT_B; break;
        case 10: input = FWD | INPUT_C; break;
        case 14: input = INPUT_DOWN | INPUT_C; break;
        case 17: input = INPUT_DOWN; break;
        case 18: input = INPUT_DOWN | FWD; break;
        case 19: input = FWD | INPUT_A; break;
        default: input = 0; break;
        }
        break;

    case s_ai.QCF:
        switch (s_ai.step) {
        case 0: case 1: input = INPUT_DOWN; break;
        case 2: case 3: input = INPUT_DOWN | FWD; break;
        case 4:         input = FWD | INPUT_A; break;
        default:        input = 0; break;
        }
        break;

    case s_ai.DP:
        switch (s_ai.step) {
        case 0:         input = FWD; break;
        case 1: case 2: input = INPUT_DOWN; break;
        case 3:         input = INPUT_DOWN | FWD; break;
        case 4:         input = FWD | INPUT_B; break;
        default:        input = 0; break;
        }
        break;

    case s_ai.HCF:
        switch (s_ai.step) {
        case 0: case 1: input = BACK; break;
        case 2:         input = BACK | INPUT_DOWN; break;
        case 3: case 4: input = INPUT_DOWN; break;
        case 5:         input = INPUT_DOWN | FWD; break;
        case 6:         input = FWD | INPUT_C; break;
        default:        input = 0; break;
        }
        break;

    case s_ai.Throw:
        if      (s_ai.step < 2)  input = FWD;
        else if (s_ai.step == 2) input = FWD | INPUT_A | INPUT_B;
        else                      input = 0;
        break;

    case s_ai.Block:
        input = BACK;
        if ((s_ai.step / 8) % 3 == 0) input |= INPUT_DOWN;
        break;

    case s_ai.Wakeup:
        if (s_ai.step < 3) {
            input = INPUT_A;
        } else {
            switch (s_ai.step - 3) {
            case 0: input = FWD; break;
            case 1: input = INPUT_DOWN; break;
            case 2: input = INPUT_DOWN | FWD; break;
            case 3: input = FWD | INPUT_C; break;
            default: input = 0; break;
            }
        }
        break;
    }

    s_ai.step++;
    s_ai.remaining--;

    // Inject through the input system (player 0 = local player)
    InputSystem_SetOverride(0, input);

    return input;
}

void AutoConnectHarness_Shutdown() {
    if (!s_active) return;

    LOG_INFO("[Harness] Shutting down");
    ShmLog("Harness shutting down");

    // M6: flush the soak verdict (fails any in-flight iteration, emits the
    // summary exactly once).
    RematchSoak_Finish("harness shutdown");

    InputSystem_ClearOverride(0);
    DestroySharedMemory();

    s_active = false;
    s_matchStartFrame = 0;
}

bool AutoConnectHarness_IsActive() {
    return s_active;
}
