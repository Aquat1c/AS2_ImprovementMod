/**
 * Alice Senki 2 - Local Match Runner
 *
 * Runs a full local match with two standalone AI players through the
 * GekkoNet StressSession rollback pipeline.  The AI reads entity state
 * directly from game memory (which is captured in savestates, so it
 * behaves correctly across rollback save/load cycles) and outputs
 * uint16_t input bitmasks matching the JOY_ constants.
 *
 * The custom AI is modeled on the decompiled AI decision tree but uses
 * its own LCG (not the game's CRT rand()), making it fully deterministic
 * and rollback-safe.
 *
 * Detailed per-frame logs go to as2_matchrunner.log.
 */

#include "match_runner.h"
#include "stress_test.h"
#include "as2_rollback.h"
#include "as2_constants.h"
#include "log_window.h"

#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC
#endif
#include <gekkonet.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace MatchRunner {

// ============================================================================
// Constants
// ============================================================================

static constexpr int kMaxSavestateSlots = 16;

// Entity offsets (from decompilation analysis)
// NOTE: These are relative to the entity *base* (ADDR_P1_ENTITY_BASE / ADDR_P2_ENTITY_BASE).
// Savestates capture the full entity memory, so they are valid after rollback loads.
static constexpr int kOff_HP          = 0x00B0;   // int16_t  health
static constexpr int kOff_Meter       = 0x00B4;   // int16_t  meter gauge
static constexpr int kOff_X           = 0x00B8;   // int16_t  X position
static constexpr int kOff_Y           = 0x00BA;   // int16_t  Y position
static constexpr int kOff_Facing      = 0x00BC;   // int8_t   facing direction (1=right, -1=left, 0xFF=left)
static constexpr int kOff_ActionID    = 0x044C;   // int32_t  current action/animation ID
static constexpr int kOff_CharID      = 0x00B0;   // int32_t  character ID (at entity pointer + 176, see below)

// The entity has a pointer at offset 0 which points to a vtable/object.
// The character ID is at *(*(entity)) + 176.
// However, for our AI we only need HP/position/action/facing, not char ID.

// Action state IDs (from decompilation of character AI)
static constexpr int kAction_Idle             =  22;
static constexpr int kAction_Crouch           =  24;
static constexpr int kAction_WalkForward      =  25;
static constexpr int kAction_Recovery73       =  73;
static constexpr int kAction_Recovery82       =  82;
static constexpr int kAction_Blocking85       =  85;
static constexpr int kAction_Blocking86       =  86;
static constexpr int kAction_HitstunStart     =  64;
static constexpr int kAction_HitstunEnd       =  72;
static constexpr int kAction_KnockdownStart   =  85;
static constexpr int kAction_KnockdownEnd     =  98;
static constexpr int kAction_AirRecovery92    =  92;
static constexpr int kAction_AirRecovery93    =  93;
static constexpr int kAction_LaunchRecovery94 =  94;

// Meter thresholds
static constexpr int kMeter_Super   = 0x0BB8;  // 3000 (super)
static constexpr int kMeter_Special = 0x07D0;  // 2000 (EX special)
static constexpr int kMeter_Low     = 0x03E8;  // 1000 (basic special)

// ============================================================================
// Match Runner Log — dedicated file
// ============================================================================

static FILE* s_logFile = nullptr;

static void MR_Log(const char* fmt, ...) {
    if (!s_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(s_logFile, fmt, args);
    va_end(args);
    fputc('\n', s_logFile);
}

static void MR_OpenLog() {
    if (s_logFile) return;
    const char* logDir = LogWindow_GetLogDir();
    char path[MAX_PATH];
    if (logDir && logDir[0]) {
        snprintf(path, sizeof(path), "%s\\as2_matchrunner_%lu.log", logDir, GetCurrentProcessId());
    } else {
        snprintf(path, sizeof(path), "as2_matchrunner.log");
    }
    s_logFile = fopen(path, "w");
    if (s_logFile) {
        setvbuf(s_logFile, nullptr, _IOFBF, 256 * 1024);
        MR_Log("=== Alice Senki 2 - Match Runner Log ===");
    }
}

static void MR_CloseLog() {
    if (s_logFile) {
        MR_Log("=== Log Closed ===");
        fclose(s_logFile);
        s_logFile = nullptr;
    }
}

// ============================================================================
// Standalone AI — reads from game memory, outputs uint16_t bitmask
// ============================================================================

namespace AI {

// Per-player AI context
struct Context {
    uint32_t rng;           // Private LCG state
    AIStyle  style;
    int      holdFrames;    // Frames to hold current input
    uint16_t heldInput;     // Input held across frames
};

static Context s_ctx[2] = {};

// ── Private RNG (LCG identical to MSVC CRT) ─────────────────────────────

static uint32_t NextRandom(Context* ctx) {
    ctx->rng = ctx->rng * 214013u + 2531011u;
    return (ctx->rng >> 16) & 0x7FFF;
}

// Chance: returns true with probability 1/(base * scale).
// scale = 1 for "hard", 3 for "medium", 6 for "easy".
// We use scale=1 (hard AI) for maximum exercise of game state.
static bool Chance(Context* ctx, int base) {
    if (base <= 0) return true;
    return (NextRandom(ctx) % base) == 0;
}

// ── Entity state reads ──────────────────────────────────────────────────

struct EntitySnapshot {
    int16_t  hp;
    int16_t  meter;
    int16_t  x;
    int16_t  y;
    int8_t   facing;
    int32_t  actionID;
    uintptr_t baseAddr;
};

static EntitySnapshot ReadEntity(uintptr_t base) {
    EntitySnapshot e;
    e.baseAddr = base;
    e.hp       = *reinterpret_cast<volatile int16_t*>(base + kOff_HP);
    e.meter    = *reinterpret_cast<volatile int16_t*>(base + kOff_Meter);
    e.x        = *reinterpret_cast<volatile int16_t*>(base + kOff_X);
    e.y        = *reinterpret_cast<volatile int16_t*>(base + kOff_Y);
    e.facing   = *reinterpret_cast<volatile int8_t*>(base + kOff_Facing);
    e.actionID = *reinterpret_cast<volatile int32_t*>(base + kOff_ActionID);
    return e;
}

// Helper: absolute distance between two entities
static int Distance(const EntitySnapshot& a, const EntitySnapshot& b) {
    int dx = (int)a.x - (int)b.x;
    return dx < 0 ? -dx : dx;
}

// Helper: is action in hitstun range?
static bool InHitstun(int action) {
    return action >= kAction_HitstunStart && action <= kAction_HitstunEnd;
}

// Helper: is action in knockdown/recovery range?
static bool InKnockdown(int action) {
    return action >= kAction_KnockdownStart && action <= kAction_KnockdownEnd;
}

// ── Input bitmask helpers ───────────────────────────────────────────────
// These mirror Input_SetButtonState / Input_ProcessDirectionalInput from decomp
// but output a bitmask directly.  Facing flip: if facing == -1 (0xFF as signed byte),
// swap left/right.

static uint16_t DirBit(int dir, int8_t facing) {
    // dir: 0=DOWN,1=UP,2=LEFT,3=RIGHT,4=DOWN+LEFT,5=DOWN+RIGHT,6=UP+LEFT,7=UP+RIGHT
    uint16_t bits = 0;
    bool flip = (facing == -1); // 0xFF signed = -1

    switch (dir) {
    case 0: bits = JOY_DOWN; break;
    case 1: bits = JOY_UP; break;
    case 2: bits = flip ? JOY_RIGHT : JOY_LEFT; break;
    case 3: bits = flip ? JOY_LEFT  : JOY_RIGHT; break;
    case 4: bits = JOY_DOWN | (flip ? JOY_RIGHT : JOY_LEFT); break;
    case 5: bits = JOY_DOWN | (flip ? JOY_LEFT  : JOY_RIGHT); break;
    case 6: bits = JOY_UP   | (flip ? JOY_RIGHT : JOY_LEFT); break;
    case 7: bits = JOY_UP   | (flip ? JOY_LEFT  : JOY_RIGHT); break;
    }
    return bits;
}

static uint16_t BtnBit(int btn, int8_t facing) {
    // btn: 0=A(light),1=B(med),2=C(heavy),3=D(special)
    int actual = btn;
    bool flip = (facing == -1);
    if (flip) {
        if (btn == 2) actual = 3;
        else if (btn == 3) actual = 2;
    }
    return (uint16_t)(JOY_BTN_A << actual);
}

// ── AI Style Generators ─────────────────────────────────────────────────

static uint16_t AI_Random(Context* ctx, const EntitySnapshot& /*me*/, const EntitySnapshot& /*opp*/) {
    uint16_t input = 0;
    uint32_t r = NextRandom(ctx);
    if ((r & 0xFF) < 77)  input |= (1 << (r & 3));            // direction
    r = NextRandom(ctx);
    if ((r & 0xFF) < 51)  input |= (1 << (4 + (r & 3)));     // button
    return input;
}

static uint16_t AI_Aggro(Context* ctx, const EntitySnapshot& me, const EntitySnapshot& opp) {
    int dist = Distance(me, opp);
    uint16_t input = 0;

    // Always approach
    if (dist > 1500) {
        // Dash forward: RIGHT (toward opponent)
        input |= DirBit(3, me.facing); // 3 = forward
        // Jump approach sometimes
        if (Chance(ctx, 8))
            input |= JOY_UP;
    }

    if (dist < 2000) {
        // Close range — attack!
        if (me.meter >= kMeter_Low && Chance(ctx, 15)) {
            // Special: down + D
            input |= DirBit(0, me.facing) | BtnBit(3, me.facing);
        } else if (Chance(ctx, 3)) {
            // Heavy attack
            input |= BtnBit(2, me.facing);
        } else if (Chance(ctx, 2)) {
            // Medium attack
            input |= BtnBit(1, me.facing);
        } else {
            // Light attack
            input |= BtnBit(0, me.facing);
        }
    }

    // Opponent in hitstun — keep pressing attack for combo
    if (InHitstun(opp.actionID)) {
        if (Chance(ctx, 2))
            input |= BtnBit(1, me.facing);
        else
            input |= BtnBit(0, me.facing);
    }

    return input;
}

static uint16_t AI_Defensive(Context* ctx, const EntitySnapshot& me, const EntitySnapshot& opp) {
    int dist = Distance(me, opp);
    uint16_t input = 0;

    // Block when close
    if (dist < 2500) {
        // Hold back = blocking
        input |= DirBit(2, me.facing); // 2 = back
        // Crouch block sometimes
        if (Chance(ctx, 3))
            input |= JOY_DOWN;
    }

    // Punish when opponent is in recovery
    if (opp.actionID == kAction_Recovery73 || opp.actionID == kAction_Recovery82) {
        if (Chance(ctx, 2)) {
            input = BtnBit(2, me.facing); // Heavy punish
        }
    }

    // Anti-air: jump + attack if opponent is above
    if (opp.y > me.y + 500 && dist < 2000) {
        input = JOY_UP | BtnBit(1, me.facing);
    }

    // Retreat when low HP
    if (me.hp < 3000 && dist < 1500) {
        input |= DirBit(2, me.facing); // Back away
    }

    return input;
}

static uint16_t AI_Balanced(Context* ctx, const EntitySnapshot& me, const EntitySnapshot& opp) {
    int dist = Distance(me, opp);
    uint16_t input = 0;
    uint32_t r = NextRandom(ctx);
    int decision = r % 10; // 0-9

    // Close range
    if (dist < 1500) {
        if (InHitstun(opp.actionID)) {
            // Opponent is hit — combo!
            input |= BtnBit(r % 3, me.facing);
        } else if (decision < 4) {
            // 40% attack
            if (Chance(ctx, 3))
                input |= BtnBit(2, me.facing);  // Heavy
            else
                input |= BtnBit(r % 2, me.facing); // Light/Med
        } else if (decision < 7) {
            // 30% block
            input |= DirBit(2, me.facing);
            if (Chance(ctx, 2))
                input |= JOY_DOWN;
        } else {
            // 30% retreat
            input |= DirBit(2, me.facing);
        }
    }
    // Mid range
    else if (dist < 3000) {
        if (decision < 5) {
            // 50% approach
            input |= DirBit(3, me.facing);
            if (Chance(ctx, 5))
                input |= BtnBit(0, me.facing); // Poke
        } else if (decision < 7) {
            // 20% jump approach
            input |= JOY_UP | DirBit(3, me.facing);
        } else {
            // 30% spacing / idle
            if (Chance(ctx, 4))
                input |= DirBit(2, me.facing); // Back up
        }
    }
    // Far range
    else {
        if (decision < 6) {
            // 60% approach
            input |= DirBit(3, me.facing);
        } else if (decision < 8) {
            // 20% jump
            input |= JOY_UP | DirBit(3, me.facing);
        } else {
            // 20% do nothing / neutral
        }
    }

    // Super if meter is full + close
    if (me.meter >= kMeter_Super && dist < 2000 && Chance(ctx, 20)) {
        input = DirBit(5, me.facing) | BtnBit(3, me.facing); // QCF+D
    }

    return input;
}

static uint16_t AI_Mirror(Context* ctx, const EntitySnapshot& me, const EntitySnapshot& opp) {
    (void)ctx;
    // Read opponent's last packed input from the history buffer.
    // This is interesting because both sides do it — creates feedback loop.
    volatile uint32_t* pWriteIdx = reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_WRITE_IDX);
    uint32_t frame = *pWriteIdx;
    if (frame == 0) return 0;

    // Read the OTHER player's input from history
    uintptr_t histAddr = (me.baseAddr == ADDR_P1_ENTITY_BASE)
        ? ADDR_P2_INPUT_HISTORY
        : ADDR_P1_INPUT_HISTORY;
    uint16_t oppInput = *reinterpret_cast<volatile uint16_t*>(histAddr + ((frame - 1) * sizeof(uint16_t)));
    return oppInput;
}

// ── Public Generate ─────────────────────────────────────────────────────

static void Init(int playerIdx, AIStyle style, uint32_t seed) {
    s_ctx[playerIdx].rng = seed + (playerIdx * 7919u); // Offset seeds so P1 != P2
    s_ctx[playerIdx].style = style;
    s_ctx[playerIdx].holdFrames = 0;
    s_ctx[playerIdx].heldInput = 0;
}

static uint16_t Generate(int playerIdx) {
    Context* ctx = &s_ctx[playerIdx];

    // Input hold system — hold the same input for a few frames to make
    // motions look more natural and exercise the input buffer properly
    if (ctx->holdFrames > 0) {
        ctx->holdFrames--;
        return ctx->heldInput;
    }

    uintptr_t myBase  = (playerIdx == 0) ? ADDR_P1_ENTITY_BASE : ADDR_P2_ENTITY_BASE;
    uintptr_t oppBase = (playerIdx == 0) ? ADDR_P2_ENTITY_BASE : ADDR_P1_ENTITY_BASE;

    EntitySnapshot me  = ReadEntity(myBase);
    EntitySnapshot opp = ReadEntity(oppBase);

    uint16_t input;
    switch (ctx->style) {
    case AI_RANDOM:     input = AI_Random(ctx, me, opp);     break;
    case AI_AGGRO:      input = AI_Aggro(ctx, me, opp);      break;
    case AI_DEFENSIVE:  input = AI_Defensive(ctx, me, opp);  break;
    case AI_BALANCED:   input = AI_Balanced(ctx, me, opp);   break;
    case AI_MIRROR:     input = AI_Mirror(ctx, me, opp);     break;
    default:            input = AI_Random(ctx, me, opp);     break;
    }

    // Hold this input for 1-4 frames (more natural, exercises hold detection)
    ctx->holdFrames = (int)(NextRandom(ctx) % 4);
    ctx->heldInput = input;

    return input;
}

} // namespace AI

// ============================================================================
// Internal State
// ============================================================================

namespace {

static GekkoSession*      s_session       = nullptr;
static GekkoConfig        s_gekkoConfig   = {};
static Config             s_config        = {};
static Results            s_results       = {};
static bool               s_running       = false;

// Savestate ring buffer
static CompactSaveState_t* s_savestates   = nullptr;
static int                s_savestateCount = 0;

// Player handles
static int                s_p1Handle      = -1;
static int                s_p2Handle      = -1;

// Rollback depth tracker
static int                s_currentRollbackDepth = 0;

// Round tracking
static int                s_lastSubState  = -1;
static int                s_roundTransitions = 0;

// ============================================================================
// Helpers
// ============================================================================

static void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(s_results.status, sizeof(s_results.status), _TRUNCATE, fmt, args);
    va_end(args);
}

static void SnapshotPlayerState(PlayerState* out, uintptr_t base, uint16_t lastInput) {
    out->hp        = *reinterpret_cast<volatile int16_t*>(base + kOff_HP);
    out->meter     = *reinterpret_cast<volatile int16_t*>(base + kOff_Meter);
    out->x_pos     = *reinterpret_cast<volatile int16_t*>(base + kOff_X);
    out->y_pos     = *reinterpret_cast<volatile int16_t*>(base + kOff_Y);
    out->facing    = *reinterpret_cast<volatile int8_t*>(base + kOff_Facing);
    out->action_id = *reinterpret_cast<volatile uint32_t*>(base + kOff_ActionID);
    out->last_input = lastInput;
}

// ============================================================================
// Event Handlers
// ============================================================================

static void HandleSaveEvent(const GekkoGameEvent* event) {
    int frame = event->data.save.frame;
    int slotIdx = frame % kMaxSavestateSlots;
    CompactSaveState_t* slot = &s_savestates[slotIdx];

    AS2_SaveStateToBuffer(slot);

    uint32_t checksum = slot->checksum;
    s_results.last_save_checksum = checksum;
    s_results.last_checked_frame = frame;

    if (event->data.save.checksum)
        *(event->data.save.checksum) = checksum;
    if (event->data.save.state_len)
        *(event->data.save.state_len) = sizeof(CompactSaveState_t);
    if (event->data.save.state)
        memcpy(event->data.save.state, slot, sizeof(CompactSaveState_t));

    s_results.save_count++;

    if (s_config.log_per_frame) {
        MR_Log("[SAVE] frame=%d slot=%d crc=0x%08X", frame, slotIdx, checksum);
    }
}

static void HandleLoadEvent(const GekkoGameEvent* event) {
    int frame = event->data.load.frame;

    if (event->data.load.state && event->data.load.state_len >= sizeof(CompactSaveState_t)) {
        const CompactSaveState_t* state = (const CompactSaveState_t*)event->data.load.state;
        AS2_LoadStateFromBuffer(state);
    } else {
        int slotIdx = frame % kMaxSavestateSlots;
        AS2_LoadStateFromBuffer(&s_savestates[slotIdx]);
    }

    s_results.load_count++;

    if (s_config.log_rollbacks) {
        MR_Log("[LOAD] frame=%d (rollback start)", frame);
    }
}

static uint16_t s_lastP1Input = 0;
static uint16_t s_lastP2Input = 0;

static void HandleAdvanceEvent(const GekkoGameEvent* event) {
    bool rolling_back = event->data.adv.rolling_back;
    const uint8_t* inputs = event->data.adv.inputs;
    unsigned int input_len = event->data.adv.input_len;
    int frame = event->data.adv.frame;

    if (rolling_back) {
        s_currentRollbackDepth++;
        if (s_currentRollbackDepth > s_results.max_rollback_depth)
            s_results.max_rollback_depth = s_currentRollbackDepth;
    } else {
        if (s_currentRollbackDepth > 0) {
            s_results.rollback_count++;
            if (s_config.log_rollbacks)
                MR_Log("[ROLLBACK] depth=%d resolved at frame=%d", s_currentRollbackDepth, frame);
        }
        s_currentRollbackDepth = 0;
    }

    // Apply inputs
    uint16_t p1Input = 0, p2Input = 0;
    if (input_len >= 4 && inputs) {
        p1Input = *(const uint16_t*)(inputs);
        p2Input = *(const uint16_t*)(inputs + 2);
        AS2_SetSynchronizedInputOverride(p1Input, p2Input);
    }
    s_lastP1Input = p1Input;
    s_lastP2Input = p2Input;

    s_results.advance_count++;
    s_results.frames_simulated = frame;

    // Snapshot current player states
    SnapshotPlayerState(&s_results.p1, ADDR_P1_ENTITY_BASE, p1Input);
    SnapshotPlayerState(&s_results.p2, ADDR_P2_ENTITY_BASE, p2Input);

    // Per-frame log
    if (s_config.log_per_frame && !rolling_back) {
        MR_Log("[ADV] f=%d rb=%d p1(hp=%d m=%d x=%d act=%d in=0x%04X) p2(hp=%d m=%d x=%d act=%d in=0x%04X)",
            frame, rolling_back ? 1 : 0,
            s_results.p1.hp, s_results.p1.meter, s_results.p1.x_pos, s_results.p1.action_id, p1Input,
            s_results.p2.hp, s_results.p2.meter, s_results.p2.x_pos, s_results.p2.action_id, p2Input);
    }

    // Round transition detection
    uint32_t subState = *reinterpret_cast<volatile uint32_t*>(ADDR_SUB_STATE);
    if (s_lastSubState != -1 && (int)subState != s_lastSubState) {
        s_roundTransitions++;
        s_results.rounds_completed = s_roundTransitions;
        MR_Log("[ROUND] substate transition %d -> %d at frame=%d (transition #%d)",
            s_lastSubState, subState, frame, s_roundTransitions);

        // Optional max rounds check
        if (s_config.max_rounds > 0 && s_roundTransitions >= s_config.max_rounds) {
            MR_Log("[MATCH] Max rounds reached (%d), will stop", s_config.max_rounds);
            // Will be stopped in FrameUpdate
        }
    }
    s_lastSubState = (int)subState;
}

static void HandleSessionEvents() {
    if (!s_session) return;

    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(s_session, &count);
    if (!events || count <= 0) return;

    for (int i = 0; i < count; i++) {
        GekkoSessionEvent* ev = events[i];
        if (!ev) continue;

        switch (ev->type) {
        case GekkoSessionStarted:
            MR_Log("[SESSION] GekkoNet stress session started");
            SetStatus("Match running...");
            break;

        case GekkoDesyncDetected:
            s_results.desync_count++;
            s_results.desync_frame = ev->data.desynced.frame;
            s_results.desync_local_checksum = ev->data.desynced.local_checksum;
            s_results.desync_remote_checksum = ev->data.desynced.remote_checksum;
            MR_Log("[DESYNC] frame=%d local=0x%08X remote=0x%08X",
                ev->data.desynced.frame,
                ev->data.desynced.local_checksum,
                ev->data.desynced.remote_checksum);
            LOG_NETPLAY(LOG_ERROR,
                "[MatchRunner] DESYNC at frame %d! local=0x%08X remote=0x%08X",
                ev->data.desynced.frame,
                ev->data.desynced.local_checksum,
                ev->data.desynced.remote_checksum);
            SetStatus("DESYNC at frame %d!", ev->data.desynced.frame);

            // Dump state on desync if requested
            if (s_config.log_state_on_desync) {
                MR_Log("[DESYNC-DUMP] P1: hp=%d meter=%d x=%d y=%d act=%d facing=%d",
                    s_results.p1.hp, s_results.p1.meter,
                    s_results.p1.x_pos, s_results.p1.y_pos,
                    s_results.p1.action_id, s_results.p1.facing);
                MR_Log("[DESYNC-DUMP] P2: hp=%d meter=%d x=%d y=%d act=%d facing=%d",
                    s_results.p2.hp, s_results.p2.meter,
                    s_results.p2.x_pos, s_results.p2.y_pos,
                    s_results.p2.action_id, s_results.p2.facing);
                MR_Log("[DESYNC-DUMP] GameMode=%d SubState=%d FrameSim=%d",
                    *reinterpret_cast<volatile uint32_t*>(ADDR_GAME_MODE),
                    *reinterpret_cast<volatile uint32_t*>(ADDR_SUB_STATE),
                    *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_SIMULATION));
            }
            break;

        default:
            break;
        }
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

Config GetDefaultConfig() {
    Config cfg = {};
    cfg.input_delay      = 0;
    cfg.max_rollback     = 8;
    cfg.check_distance   = 10;
    cfg.desync_detection = true;
    cfg.p1_ai            = AI_BALANCED;
    cfg.p2_ai            = AI_AGGRO;
    cfg.random_seed      = 42;
    cfg.max_frames       = 0;       // Run until stopped
    cfg.max_rounds       = 0;       // No round limit
    cfg.log_per_frame    = true;
    cfg.log_rollbacks    = true;
    cfg.log_state_on_desync = true;
    return cfg;
}

const char* GetAIStyleName(AIStyle style) {
    switch (style) {
    case AI_RANDOM:     return "Random";
    case AI_AGGRO:      return "Aggro";
    case AI_DEFENSIVE:  return "Defensive";
    case AI_BALANCED:   return "Balanced";
    case AI_MIRROR:     return "Mirror";
    default:            return "Unknown";
    }
}

bool Start(const Config* config) {
    if (s_running) {
        LOG_NETPLAY(LOG_WARNING, "[MatchRunner] Already running");
        return false;
    }
    if (!config) return false;

    if (StressTest::IsRunning()) {
        LOG_NETPLAY(LOG_WARNING, "[MatchRunner] Cannot start while stress test is running");
        SetStatus("ERROR: Stress test is running");
        return false;
    }

    if (!AS2_IsInPlayableGameplay()) {
        LOG_NETPLAY(LOG_ERROR, "[MatchRunner] Must be in interactive gameplay");
        SetStatus("ERROR: Not in interactive gameplay");
        return false;
    }

    memcpy(&s_config, config, sizeof(Config));

    // Reset
    memset(&s_results, 0, sizeof(Results));
    s_results.running = true;
    s_currentRollbackDepth = 0;
    s_lastSubState = -1;
    s_roundTransitions = 0;
    s_lastP1Input = 0;
    s_lastP2Input = 0;

    // Open log
    MR_OpenLog();
    MR_Log("[START] seed=%u delay=%d maxRB=%d check=%d p1=%s p2=%s maxFrames=%d maxRounds=%d",
        config->random_seed, config->input_delay, config->max_rollback, config->check_distance,
        GetAIStyleName(config->p1_ai), GetAIStyleName(config->p2_ai),
        config->max_frames, config->max_rounds);

    // Initialize AI
    AI::Init(0, config->p1_ai, config->random_seed);
    AI::Init(1, config->p2_ai, config->random_seed);

    // Allocate savestate ring buffer
    s_savestates = (CompactSaveState_t*)calloc(kMaxSavestateSlots, sizeof(CompactSaveState_t));
    if (!s_savestates) {
        MR_Log("[ERROR] Failed to allocate savestate ring buffer");
        SetStatus("ERROR: Memory allocation failed");
        s_results.running = false;
        MR_CloseLog();
        return false;
    }
    s_savestateCount = kMaxSavestateSlots;

    // Create GekkoNet stress session
    if (!gekko_create(&s_session, GekkoStressSession)) {
        MR_Log("[ERROR] gekko_create(GekkoStressSession) failed");
        SetStatus("ERROR: Failed to create session");
        free(s_savestates);
        s_savestates = nullptr;
        s_results.running = false;
        MR_CloseLog();
        return false;
    }

    // Configure
    memset(&s_gekkoConfig, 0, sizeof(s_gekkoConfig));
    s_gekkoConfig.num_players = 2;
    s_gekkoConfig.max_spectators = 0;
    s_gekkoConfig.input_prediction_window = config->max_rollback > 0 ? config->max_rollback : 8;
    s_gekkoConfig.spectator_delay = 0;
    s_gekkoConfig.input_size = sizeof(uint16_t);
    s_gekkoConfig.state_size = sizeof(CompactSaveState_t);
    s_gekkoConfig.limited_saving = false;
    s_gekkoConfig.desync_detection = config->desync_detection;
    s_gekkoConfig.check_distance = config->check_distance > 0 ? config->check_distance : 10;

    gekko_start(s_session, &s_gekkoConfig);

    // Add both players as local
    GekkoNetAddress dummyAddr = {};
    s_p1Handle = gekko_add_actor(s_session, GekkoLocalPlayer, &dummyAddr);
    s_p2Handle = gekko_add_actor(s_session, GekkoLocalPlayer, &dummyAddr);

    if (s_p1Handle < 0 || s_p2Handle < 0) {
        MR_Log("[ERROR] Failed to add players (p1=%d, p2=%d)", s_p1Handle, s_p2Handle);
        gekko_destroy(&s_session);
        free(s_savestates);
        s_savestates = nullptr;
        s_results.running = false;
        MR_CloseLog();
        return false;
    }

    // Set input delay
    int delay = config->input_delay;
    if (delay < 0) delay = 0;
    if (delay > 15) delay = 15;
    gekko_set_local_delay(s_session, s_p1Handle, (unsigned char)delay);
    gekko_set_local_delay(s_session, s_p2Handle, (unsigned char)delay);

    s_running = true;
    SetStatus("Match running (P1=%s P2=%s)", GetAIStyleName(config->p1_ai), GetAIStyleName(config->p2_ai));

    LOG_NETPLAY(LOG_INFO,
        "[MatchRunner] Started: p1=%s p2=%s seed=%u delay=%d maxRB=%d",
        GetAIStyleName(config->p1_ai), GetAIStyleName(config->p2_ai),
        config->random_seed, delay, config->max_rollback);

    return true;
}

void Stop() {
    if (!s_running) return;

    if (s_session) {
        gekko_destroy(&s_session);
        s_session = nullptr;
    }

    if (s_savestates) {
        free(s_savestates);
        s_savestates = nullptr;
    }
    s_savestateCount = 0;

    AS2_ClearSynchronizedInputOverride();

    s_running = false;
    s_results.running = false;
    s_results.completed = true;
    s_results.passed = (s_results.desync_count == 0);

    s_p1Handle = -1;
    s_p2Handle = -1;

    if (s_results.passed) {
        SetStatus("PASSED: %d frames, %d rounds, %d rollbacks, 0 desyncs",
            s_results.frames_simulated, s_results.rounds_completed,
            s_results.rollback_count);
    } else {
        SetStatus("FAILED: %d desyncs (last at frame %d)",
            s_results.desync_count, s_results.desync_frame);
    }

    MR_Log("[STOP] result=%s frames=%d rounds=%d saves=%d loads=%d rollbacks=%d(max=%d) desyncs=%d",
        s_results.passed ? "PASS" : "FAIL",
        s_results.frames_simulated, s_results.rounds_completed,
        s_results.save_count, s_results.load_count,
        s_results.rollback_count, s_results.max_rollback_depth,
        s_results.desync_count);

    LOG_NETPLAY(LOG_INFO,
        "[MatchRunner] Stopped: %s | frames=%d rollbacks=%d desyncs=%d",
        s_results.passed ? "PASS" : "FAIL",
        s_results.frames_simulated, s_results.rollback_count, s_results.desync_count);

    MR_CloseLog();
}

bool FrameUpdate() {
    if (!s_running || !s_session) return false;

    // Check frame limit
    if (s_config.max_frames > 0 &&
        s_results.frames_simulated >= s_config.max_frames) {
        MR_Log("[MATCH] Max frames reached (%d)", s_config.max_frames);
        Stop();
        return false;
    }

    // Check round limit
    if (s_config.max_rounds > 0 &&
        s_roundTransitions >= s_config.max_rounds) {
        Stop();
        return false;
    }

    // Stop once the game leaves the interactive fighting window.
    if (!AS2_IsInPlayableGameplay()) {
        MR_Log("[MATCH] Game left interactive gameplay, stopping");
        Stop();
        return false;
    }

    // Generate inputs from our standalone AI
    uint16_t p1Input = AI::Generate(0);
    uint16_t p2Input = AI::Generate(1);

    // Feed to GekkoNet
    gekko_add_local_input(s_session, s_p1Handle, &p1Input);
    gekko_add_local_input(s_session, s_p2Handle, &p2Input);

    // Update session — returns game events
    int eventCount = 0;
    GekkoGameEvent** events = gekko_update_session(s_session, &eventCount);

    // Process session-level events
    HandleSessionEvents();

    // Process game events
    bool hadAdvance = false;
    if (events) {
        for (int i = 0; i < eventCount; i++) {
            GekkoGameEvent* ev = events[i];
            if (!ev) continue;

            switch (ev->type) {
            case GekkoSaveEvent:    HandleSaveEvent(ev);    break;
            case GekkoLoadEvent:    HandleLoadEvent(ev);    break;
            case GekkoAdvanceEvent: HandleAdvanceEvent(ev); hadAdvance = true; break;
            default: break;
            }
        }
    }

    return hadAdvance;
}

bool IsRunning() {
    return s_running;
}

bool GetResults(Results* out) {
    if (!out) return false;
    memcpy(out, &s_results, sizeof(Results));
    return true;
}

} // namespace MatchRunner
