/**
 * Alice Senki 2 - Rollback Netcode Mod
 * Main header file with game structures and function definitions
 * 
 * Memory Layout Analysis:
 * - Entity array at dword_776668[]: Array of entity pointers
 * - P1 entity: dword_776668[0], HP at entity+0xB0 = 0x776718
 * - P2 entity: dword_776668[27203] or dword_77666C[0], HP at = 0x791024
 * - P2 offset from P1: 0x791024 - 0x776718 = 0x1A90C (108812 bytes)
 * - Actual entity size estimate: ~0x1A90C/2 = ~0xD486 per entity (54406 bytes)
 *   But based on array index 27203, entity stride = sizeof(int)*27203 = 108812 bytes
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

// Centralized addresses/offsets + game state constants
#include "as2_constants.h"

// ============================================================================
// Rollback Synchronized Input Override
// ============================================================================
// InputSyncHooks feeds per-frame synchronized P1/P2 inputs here.
// Hook_InputProcess consumes them to populate the game's internal input buffers
// (including just-pressed) in a way that's rollback-friendly.

#ifdef __cplusplus
extern "C" {
#endif

void AS2_SetSynchronizedInputOverride(uint16_t p1Input, uint16_t p2Input);
void AS2_ClearSynchronizedInputOverride(void);
bool AS2_IsSynchronizedInputOverrideActive(void);

// Visual frame counter - incremented once per EndScene (render frame)
// Used by input sync hooks to ensure only 1 game tick per visual frame
uint32_t AS2_GetVisualFrameCounter(void);
void AS2_IncrementVisualFrameCounter(void);

// Match-specific visual frame counter - resets to 0 at match start
uint32_t AS2_GetMatchVisualFrame(void);
void AS2_ResetMatchVisualFrame(void);
void AS2_EndMatch(void);

// Tick scaling / time warp controls
// 1.0f = normal speed. Values > 1.0f speed up the game's main-loop frame limiter.
// Global scale applies always; rollback scale applies only during rollback/resimulation.
void AS2_SetGlobalTickScale(float scale);
float AS2_GetGlobalTickScale(void);
void AS2_SetRollbackTickScale(float scale);
float AS2_GetRollbackTickScale(void);

// FPU diagnostics - capture, log, and validate floating-point state
void AS2_GetCurrentFpuState(uint16_t* outCW, uint16_t* outSW, uint32_t* outMXCSR);
void AS2_LogFpuState(const char* tag);
bool AS2_ValidateFpuState(const char* context);
uint32_t AS2_GetFpuSaveCount(void);
uint32_t AS2_GetFpuRestoreCount(void);
uint32_t AS2_GetFpuAnomalyCount(void);

// RNG diagnostics
void AS2_GetRngStats(uint32_t* outSimSeed, uint32_t* outVisSeed,
                     uint32_t* outSimCalls, uint32_t* outVisCalls);

// RNG audit mode — records call sites to validate visual/simulation classification.
// Compile with -DAS2_RNG_AUDIT to enable.
#ifdef AS2_RNG_AUDIT
void AS2_RngAudit_Begin(uint32_t frameLimitOverride);
void AS2_RngAudit_End(void);
bool AS2_RngAudit_IsActive(void);
void AS2_RngAudit_DumpToLog(void);
uint32_t AS2_RngAudit_CheckForUnclassified(void);
#endif

// Match win tracking for HUD display
// Call AS2_RecordMatchResult() when leaving MODE_MATCH during a netplay session.
// Reads match+4 (winner byte: 0=P1, 1=P2, 2=draw).
void AS2_RecordMatchResult(void);
void AS2_ResetMatchWins(void);
void AS2_GetMatchWins(int* outP1, int* outP2);

// Returns who won the last completed match:
//   -1 = no match played yet (first match), 0 = P1 (host), 1 = P2 (joiner).
// Draw does NOT change the value — previous winner retains stage nav control.
int AS2_GetLastMatchWinner(void);

// Pre-match state scrub — zeroes transient arrays (effects, summons) and
// resets FPU to a deterministic default, clears input pipeline.
// Called at Mode→Match(8) entry in netplay_core.cpp, BEFORE the game
// initializes entities in substates 0-2.  Previously called at load barrier
// (NotifyLocalLoaded) but that interfered with the intro freeze setup.
void AS2_ScrubTransientMatchState(void);

// Zero entity blocks, match context, and match header.  Call at the
// Mode→Match(8) transition BEFORE the game loads character assets so
// that residual data from a previous match (e.g. demo match) is cleared.
// The game re-initialises every field it cares about during LoadAssets;
// any bytes it does not touch will be zero on both peers.
void AS2_ZeroMatchArena(void);

#ifdef __cplusplus
}
#endif

// ============================================================================
// Structures
// ============================================================================

#pragma pack(push, 1)

// Effect/Projectile entry structure (from decompilation)
// Array of 200 slots at ADDR_EFFECT_ARRAY
typedef struct {
    uint32_t owner;           // +0: Owner entity pointer
    uint8_t  type;            // +4: Effect type (0-247+)
    uint8_t  padding1;        // +5: Alignment
    int16_t  x_pos;           // +6: X position
    int16_t  y_pos;           // +8: Y position
    int16_t  timer;           // +10: Lifetime timer
    int32_t  vel_x;           // +12: X velocity
    int32_t  vel_y;           // +16: Y velocity
    int32_t  data1;           // +20: Additional data
    int32_t  data2;           // +24: Additional data
    int32_t  data3;           // +28: Additional data
} Effect_t;

// Summon/Assist entry structure (from decompilation)
// Array of 100 slots at ADDR_SUMMON_ARRAY, 272 bytes each
// These are mini-entities for assists, shadows, summons
// Slot is ACTIVE if summon_type (+4) != 0
typedef struct {
    uint8_t  owner_id;        // +0:   Owner character ID
    uint8_t  pad1[3];         // +1:   Padding
    uint32_t summon_type;     // +4:   Summon type ID (non-zero = active slot)
    uint8_t  state_flag;      // +8:   State flag
    uint8_t  sub_flag;        // +9:   Sub-flag (NOT the active indicator)
    uint8_t  pad2[2];         // +10:  Padding
    uint32_t state_flag2;     // +12:  Secondary state flag
    uint8_t  state_data[176]; // +16:  State data (action, animation, etc.)
    uint32_t action_id;       // +192: Current action ID
    int16_t  x_pos;           // +196: X position
    int16_t  y_pos;           // +198: Y position
    uint8_t  facing;          // +200: Facing direction
    uint8_t  parent_id;       // +201: Parent character ID
    uint8_t  anim_frame;      // +202: Animation frame
    uint8_t  anim_state;      // +203: Animation state
    uint8_t  additional[68];  // +204: Additional state data
} Summon_t;

// Simplified entity structure (partial)
typedef struct {
    uint8_t  pad0[0xB0];           // 0x000-0x0AF
    uint16_t hp;                   // 0x0B0
    uint16_t pad1;                 // 0x0B2
    uint16_t meter;                // 0x0B4
    uint16_t pad2;                 // 0x0B6
    uint16_t x_pos;                // 0x0B8
    uint16_t y_pos;                // 0x0BA
    uint8_t  pad3[2];              // 0x0BC
    uint8_t  facing;               // 0x0BD
    uint8_t  pad4[0x3AE];          // 0x0BE-0x46B
    uint32_t action_id;            // 0x46C (unverified)
    uint16_t animation;            // 0x470
    // ... more fields
} Entity_t;

// Character state block (at +41072 from character base)
// Contains input history, combo info, etc.
typedef struct {
    uint8_t  data[108];            // Raw state data
    // Key offsets within this block (relative):
    // +182 (0xA126 from base): combo counter P1 (byte)
    // +176 (0xA120 from base): guard gauge (word)
    // +212 (0xA144 from base): HP display (word)
} CharacterStateBlock_t;

// Input state for one player (14 words per history frame)
typedef struct {
    uint16_t buttons[10];          // Button states
    uint16_t composite[4];         // Composite states (direction + button combos)
} InputFrame_t;

// ============================================================================
// ROLLBACK SAVESTATE - Determinism-first
// ============================================================================
// For rollback correctness validation we snapshot the full player entity structs
// (P1/P2) instead of a hand-picked compact slice. This is local-only (never
// transmitted over the network).
//
// Note: This makes the savestate significantly larger (roughly:
//   2 * ENTITY_SIZE (~212KB) + summons (~27KB) + effects (~6KB) + misc).

// Critical entity offsets that affect gameplay (from documentation)
// See docs/ACTION_SYSTEM_DOCUMENTATION.md for detailed analysis

// Animation State Block structure (entity + 0x444):
//   +0x000 (0x444): pending_action_1 (DWORD)
//   +0x004 (0x448): pending_action_2 (DWORD)  
//   +0x008 (0x44C): CURRENT ACTION ID (DWORD) <<<< IMPORTANT!
//   +0x010 (0x454): action_priority (DWORD)
//   +0x014 (0x458): action_flag (DWORD)
//   +0x02C (0x470): animation_phase (DWORD)
//   +0x040 (0x484): frame_counter (DWORD)

// Per-character state block used heavily by gameplay/action logic.
// Decomp shows many reads/writes relative to entity+0xA060 (41072), including combo/guard state.
// We snapshot a conservative prefix to preserve determinism without copying the full 108KB entity.

// Compact entity state - only critical fields (~560 bytes per entity)
#pragma pack(push, 1)
typedef struct {
    // Core state (+0xB0 to +0xD0) - 32 bytes
    uint8_t  core[ENTITY_CORE_END - ENTITY_CORE_START];
    
    // Action state base block (+0x444 to +0x46C) - 40 bytes
    // Contains: reaction state slots, ACTION HANDLER DISPATCH ID (0x44C),
    // action priority, action flag.  Critical for gameplay — must be saved,
    // restored, and included in digest checksums.
    uint8_t  input_buffer[ENTITY_INPUT_END - ENTITY_INPUT_START];
    
    // Action/animation state (+0x46C to +0x4C0) - 84 bytes
    uint8_t  action[ENTITY_ACTION_END - ENTITY_ACTION_START];
    
    // State block A (+0x690 to +0x6CC) - 60 bytes
    uint8_t  state_a[ENTITY_STATE_A_END - ENTITY_STATE_A_START];
    
    // State block B (+0x6CC to +0x778) - 172 bytes
    uint8_t  state_b[ENTITY_STATE_B_END - ENTITY_STATE_B_START];
    
    // Combat/hit tracking (+0x778 to +0x7D0) - 88 bytes (was "combo")
    // Includes: opponent_index, hit_type, hit_confirmed, ai_reaction, etc.
    uint8_t  combat[ENTITY_COMBAT_END - ENTITY_COMBAT_START];
    
    // Timer block (+0x7A0 to +0x7B4) - 20 bytes
    uint8_t  timer[ENTITY_TIMER_END - ENTITY_TIMER_START];
    
    // Hitstun array (critical for combo state) - 64 bytes
    // Located at +0x1A7F0 but we only need first 64 bytes
    uint8_t  hitstun[64];

    // Character state block prefix (+0xA060 .. +0xA660)
    // Contains combo counter (0xA126), guard gauge (0xA120), and other action-related state.
    uint8_t  char_state[ENTITY_CHAR_STATE_SIZE];
} CompactEntity_t;  // Total: 576 bytes per entity
#pragma pack(pop)

// Verify struct size at compile time
static_assert(sizeof(CompactEntity_t) == (576 - 16 + ENTITY_CHAR_STATE_SIZE), "CompactEntity_t size mismatch");

// Savestate structure for rollback/resimulation (local-only)
//
// FORMAT VERSION HISTORY:
//   0 (implicit): original — FPU captured as CW(2B)+MXCSR(4B) in first 6 bytes of fpu_state[108]
//   1:            FPU captured as full FSAVE(108B) in fpu_state[108], MXCSR separate in mxcsr(4B)
//   2:            visual_rng_seed added for weather particle RNG determinism across rollback
//
// TOTAL SIZE BREAKDOWN:
//   Metadata:       20 bytes  (frame_number, checksum, rng_seed, format_version, visual_rng_seed)
//   FPU:           112 bytes  (fpu_state[108] + mxcsr[4])
//   Global:         32 bytes  (frame_counter, game_mode, sub_state, sub_state_timer, game_type, padding)
//   P1 entity: ENTITY_SIZE bytes (108,812 — full entity blob, including all sub-fields)
//   P2 entity: ENTITY_SIZE bytes (108,812)
//   Match ctx:   7,456 bytes  (camera, screen shake, weather particles 200×28B)
//   Match state:    64 bytes  (round_timer, win_count, combo, intro_fade, match_header[16], padding)
//   Pre-match:      12 bytes  (render blend timer, audio SE channel)
//   Effects:     6,404 bytes  (effect_index(4) + 200×32B effect entries)
//   Input:         556 bytes  (4 frame indices + 2×208B buffers + 2×20B states + 2×40B history + misc)
//   Summons:    27,200 bytes  (100×272B summon entries)
//
#pragma pack(push, 1)
typedef struct {
    // --- Metadata (16 bytes) ---
    uint32_t frame_number;         // Game frame when state was captured
    uint32_t checksum;             // CRC32 for deterministic state validation
    // g_currentRngSeed: simulation RNG stream seed (hooked rand() at 0x7145A0)
    // Required: every rand() call in simulation advances this; different seed = different simulation
    // Missing: rand()-dependent moves (summon spawns, particle triggers) produce different results
    uint32_t rng_seed;
    // Format version: 0 = legacy CW+MXCSR, 1 = full FSAVE+MXCSR, 2 = + visual_rng_seed
    uint32_t format_version;
    // g_visualRngSeed: visual-only RNG stream seed (weather particles, camera effects).
    // Required: Weather_UpdateParticles (0x4C4480) calls rand() which is classified as visual,
    //   but its output modifies match_context state (particle positions/velocities at match+1868).
    //   Since match_context is captured in the savestate, the visual RNG must also be saved/restored
    //   to keep particle state deterministic across rollback on both peers.
    // Missing: after rollback on one peer, particle state diverges → checksum mismatch → false desync.
    uint32_t visual_rng_seed;
    
    // --- FPU State (112 bytes) ---
    // Full x87 FPU state via FSAVE (108 bytes): control word, status word, tag word,
    // instruction pointer, data pointer, and x87 register stack (ST(0)-ST(7)).
    // Required: DXLib uses x87 for sin/cos in effect trajectories, hit calculations, camera math.
    // Missing: stale x87 register stack or rounding mode produces different float results after restore.
    // MXCSR (4 bytes): SSE control/status register for rounding mode and exception masks.
    // Note: FSAVE reinitializes the FPU; we immediately FRSTOR after saving to avoid disruption.
    uint8_t  fpu_state[108];
    uint32_t mxcsr;                // SSE rounding/exception state (was fpu_padding[4])
    
    // --- Global game state (32 bytes) ---
    struct {
        // 0x81635C: master frame counter — incremented by the game's main loop.
        // Normalized to GekkoNet frame in HandleSaveEvent to ensure determinism across rollback histories.
        uint32_t frame_counter;
        // 0x81638C: game mode (4=lobby, 6=charsel, 7=stagesel, 8=match)
        uint32_t game_mode;
        // 0x816390: substate within mode (0-5 for match: load/setup/init/gameplay/pause/end)
        uint32_t sub_state;
        // 0x816394: substate timer — incremented per frame within current substate.
        // Intentionally excluded from desync digest (differs harmlessly between peers).
        uint32_t sub_state_timer;
        // 0x816410: game type (2=VS_HUMAN during charsel, 3=netplay during match)
        uint32_t game_type;
        uint8_t  padding[12];
    } global;
    
    // --- Full entity snapshots (ENTITY_SIZE × 2 = 217,624 bytes) ---
    // Captured as flat blobs from GetEntityBase(0) and GetEntityBase(1).
    // Required: entities contain ALL gameplay state — HP, meter, position, velocity,
    //   action/animation state, hitstop (entity+1238), hitstun, combo counters (entity+0xA126),
    //   guard gauge (entity+0xA120), input history (entity+0xA060+), summon references,
    //   hit flags, knockdown/recovery state, charge timers, invincibility frames.
    // Missing: any gameplay field not restored causes divergent simulation outcomes.
    // Note: entity+4 contains opponent pointer — safe because entity bases are static addresses.
    uint8_t  p1_entity[ENTITY_SIZE];
    uint8_t  p2_entity[ENTITY_SIZE];
    
    // --- Match context (7,456 bytes at 0x76C608..0x76E328) ---
    // Region between the 16-byte match header and the effect array.
    // Contains: camera scroll (match+1860) used by boundary clamping and HitDef_Create,
    //   screen shake counter (match+1864), weather particles (match+1868, 200×28=5600B),
    //   and miscellaneous match state.
    // Required: weather particles call rand() every frame via Weather_UpdateParticles.
    //   If not restored, the RNG sequence diverges and ALL gameplay desyncs.
    //   Camera scroll affects boundary calculations.
    // Missing: RNG divergence → different summon spawns, different particle triggers, gameplay desync.
    uint8_t  match_context[MATCH_CONTEXT_SIZE];
    
    // --- Match state (64 bytes) ---
    struct {
        // 0x790E50: shared HUD round timer (dword). [1] kept zeroed for alignment.
        int32_t  round_timer[2];
        // 0x790E54: per-player win count (words)
        uint16_t win_count[2];
        // 0x790E56: per-player current combo count (bytes)
        uint8_t  combo_count[2];
        // 0x816370: shared phase timer used by Match intro lock and end-transition countdown.
        // Required: controls when the "Fight!" announcer unlocks input at match start.
        // Missing: one peer unlocks input earlier → different simulation frame for first input.
        uint32_t intro_fade_timer;
        // Match header (16 bytes at ADDR_MATCH_BASE = 0x76C5F8):
        //   +0: byte — opening intro/input-lock active flag
        //   +1: byte — end-transition active flag  
        //   +2: word — announcer/phase timer (normalized to 0xFFFF in HandleSaveEvent
        //              to prevent drift from different rollback histories)
        //   +4: byte — winner_index (-1=none, 0=P1, 1=P2, 2=draw)
        //   +5: byte — padding
        //   +6: word — post-KO / post-draw countdown timer
        //   +8: word — demo timeout counter (training mode: triggers at 3600)
        //   +10: byte — end-round route selector (0xFF=unresolved, 1=disconnect, etc.)
        //   +11: byte — frame-processed / should-render gate
        uint8_t  match_header[16];
        uint8_t  padding[30];
    } match;
    
    // --- Pre-match gap (12 bytes at 0x76C5EC..0x76C5F7) ---
    // Between ADDR_EFFECT_INDEX(+4) and ADDR_MATCH_BASE. Contains render blend
    // timer, audio SE channel index, and padding. Captured for determinism completeness.
    // Required: SE channel index may affect which sound channel is reused, which
    //   could indirectly gate sound-related code paths.
    uint8_t pre_match_gap[PRE_MATCH_GAP_SIZE];
    
    // --- Effect array (6,404 bytes) ---
    // Projectile and visual effects: 200 slots × 32 bytes at ADDR_EFFECT_ARRAY (0x76E328).
    // Effect_Update (sub_4A9330) iterates all slots each frame.
    // Effects are purely visual (no gameplay collision), BUT Effect_Update calls rand()
    // for velocity/angle initialization — if effect slots differ between peers, different
    // numbers of rand() calls occur, diverging the simulation RNG seed.
    // Required: consistent effect array → consistent rand() call count → deterministic RNG.
    struct {
        uint32_t effect_index;              // 0x76C5E8: circular write index (wraps at 200)
        Effect_t effects[EFFECT_MAX_SLOTS]; // 200 × 32 = 6400 bytes
    } effects;
    
    // --- Input system state (556 bytes) ---
    // The game's input pipeline has multiple layers per player (104 words = 208 bytes):
    //   Words  0-13: current frame held buttons (10 buttons + 4 composite)
    //   Words 14-27: previous frame (shifted via qmemcpy before processing)
    //   Words 28-41: just-pressed flags (edge detection)
    //   Words 42-55: repeat/cooldown timers (autorepeat state)
    //   Words 56-69: hold counters (move hold detection)
    //   Words 70-83: per-button directional/command state
    //   Words 84-103: additional reserved state
    // Required: input history and hold counters drive command detection (dragon punch, etc).
    //   After rollback, stale input state → phantom or missed special move inputs.
    // Note: ADDR_P1_INPUT_BUFFER (0x8E9E62) is also read by Hook_InputProcess as the
    //   "alt buffer" for edge detection — the same memory region, not a separate allocation.
    struct {
        // Frame counter indices (0x816490-0x81649C) — normalized in HandleSaveEvent
        uint32_t read_idx;
        uint32_t display_idx;
        uint32_t write_idx;
        uint32_t net_idx;
        // Full per-player input buffers (208 bytes each at 0x8E9E62 / 0x8E9F32)
        uint8_t  p1_buffer[INPUT_BUFFER_SIZE];
        uint8_t  p2_buffer[INPUT_BUFFER_SIZE];
        // Processed just-pressed state (10 words = 20 bytes each at 0x8E9E9A / 0x8E9F6A)
        // Kept separately for quick diagnostics and diffing.
        uint8_t  p1_state[INPUT_STATE_SIZE];
        uint8_t  p2_state[INPUT_STATE_SIZE];
        // Recent bitmask input history (last 20 frames from 0x8164A0 / 0x87FC24)
        // Required: command detection reads history to match motion inputs (236236+A, etc).
        uint16_t p1_history[INPUT_HISTORY_WINDOW];
        uint16_t p2_history[INPUT_HISTORY_WINDOW];
        uint16_t p1_input;
        uint16_t p2_input;
        uint8_t  padding[4];
    } input;

    // --- Summon/Assist array (27,200 bytes) ---
    // 100 slots × 272 bytes at ADDR_SUMMON_ARRAY (0x76FC28).
    // Summons are mini-entities for assists, shadows, and helpers. Active when summon_type != 0.
    // The game iterates the full array during Entity_UpdateSummons, including collision checks.
    // Required: summons have HP, position, action state, and hit flags that affect simulation.
    //   Missing: summon positions/states diverge → different collision outcomes.
    struct {
        Summon_t summons[SUMMON_MAX_SLOTS];
    } summons;
    
} CompactSaveState_t;  // Total: ~245KB (depends on ENTITY_SIZE)
#pragma pack(pop)

static_assert(sizeof(Summon_t) == SUMMON_ENTRY_SIZE, "Summon_t size mismatch");
static_assert((ADDR_P2_INPUT_BUFFER - ADDR_P1_INPUT_BUFFER) == INPUT_BUFFER_SIZE,
              "INPUT_BUFFER_SIZE must span exactly one player's global input buffer");

// ============================================================================
// LEGACY SAVESTATE - Full structure for manual saves/debugging
// ============================================================================

// Legacy full savestate structure for the old custom rollback system / debugging.
// NOTE: This is NOT the state used by rollback/resimulation (see CompactSaveState_t above).
// Total size depends on ENTITY_SIZE and other captured subsystems.
typedef struct {
    // Metadata
    uint32_t frame_number;         // Game frame when state was captured
    uint32_t checksum;             // CRC32 for state validation
    uint64_t timestamp;            // System timestamp when captured
    
    // RNG state (MSVC CRT uses a simple LCG, state is a single DWORD)
    uint32_t rng_seed;             // CRT rand() seed at __mb_cur_max or similar
    
    // Global game state (64 bytes)
    struct {
        uint32_t quit_flag;        // 0x816358
        uint32_t frame_counter;    // 0x81635C
        uint32_t last_frame_time;  // 0x816360
        uint32_t fps_count;        // 0x816364
        uint32_t game_mode;        // 0x81638C
        uint32_t sub_state;        // 0x816390
        uint32_t sub_state_timer;  // 0x816394
        uint32_t match_active;     // 0x816410
        uint8_t  padding[32];      // Reserved for future use
    } global;
    
    // Entity states (ENTITY_SIZE each; currently ~108KB each)
    uint8_t p1_entity[ENTITY_SIZE];
    uint8_t p2_entity[ENTITY_SIZE];
    
    // Match-specific state (round timer, win counts, etc.)
    struct {
        int32_t  round_timer[2];   // Timer for each player
        uint16_t win_count[2];     // Win count per player  
        uint8_t  combo_count[2];   // Current combo per player
        uint8_t  padding[54];      // Alignment
    } match;
    
    // Effect/Projectile array state (200 slots * 32 bytes = 6400 bytes)
    struct {
        uint32_t effect_index;              // Current write index (0-199)
        Effect_t effects[EFFECT_MAX_SLOTS]; // All effect slots
    } effects;
    
    // Summon/Assist array state (100 slots * 272 bytes = 27200 bytes)
    // Stores full summon entities including animation state
    struct {
        Summon_t summons[SUMMON_MAX_SLOTS]; // All summon slots
    } summons;
    
    // Sound system state (debug/verification only; not required for simulation determinism)
    struct {
        uint32_t sound_mode;        // 0x9D046C - Sound mode flag
        uint32_t sound_state;       // 0x9D0470 - Audio state
        // Note: Full sound buffer pool (4096 entries) too large
        // Only track playing sounds for verification
        uint16_t active_sounds;     // Count of currently playing sounds
        uint16_t last_sound_id;     // Last sound triggered
        uint8_t  padding[56];       // Reserved
    } sound;
    
    // Input system state (CRITICAL for motion input detection during rollback)
    struct {
        // Frame counters
        uint32_t read_idx;          // 0x816490 - Current read frame
        uint32_t display_idx;       // 0x816494 - Display/sync frame
        uint32_t write_idx;         // 0x816498 - Recording frame
        uint32_t net_idx;           // 0x81649C - Network sync frame
        
        // Full per-player input buffers (104 words / 208 bytes each).
        // These contain held, previous, just-pressed, cooldown, rapid-fire,
        // hold-duration, and reserved state used by the game's input pipeline.
        uint8_t p1_buffer[INPUT_BUFFER_SIZE];   // 0x8E9E62
        uint8_t p2_buffer[INPUT_BUFFER_SIZE];   // 0x8E9F32
        
        // Processed input state (just-pressed flags for current frame).
        // This is the 10-word slice at base + 56 inside the full input buffer,
        // kept separately in the savestate for diagnostics and quick diffing.
        uint8_t p1_state[INPUT_STATE_SIZE];     // 0x8E9E9A (10 words)
        uint8_t p2_state[INPUT_STATE_SIZE];     // 0x8E9F6A (10 words)
        
        // Recent input history (bitmask format, for motion input rollback)
        // Store last N frames from the long-term history arrays
        uint16_t p1_history[INPUT_HISTORY_WINDOW];  // Last 20 frames from 0x8164A0
        uint16_t p2_history[INPUT_HISTORY_WINDOW];  // Last 20 frames from 0x87FC24
        
        // Current frame packed input (matches history format)
        uint16_t p1_input;
        uint16_t p2_input;
        uint16_t p1_input_prev;
        uint16_t p2_input_prev;
        
        uint8_t  padding[4];        // Alignment
    } input;
    
    // Reduced extended buffer since summons/input are now explicit
    uint8_t extended_state[512];
    
} SaveState_t;

// Lightweight state for display/debug (no extended buffer)
typedef struct {
    uint32_t frame_number;
    uint16_t p1_hp;
    uint16_t p2_hp;
    uint16_t p1_meter;
    uint16_t p2_meter;
    int16_t  p1_x;
    int16_t  p1_y;
    int16_t  p2_x;
    int16_t  p2_y;
    uint8_t  p1_facing;
    uint8_t  p2_facing;
    uint16_t p1_action;
    uint16_t p2_action;
} StatePreview_t;

#pragma pack(pop)

#pragma pack(pop)

// ============================================================================
// Function Typedefs for Hooks
// ============================================================================

// Main loop function
typedef int (*MainLoop_t)();

// Input poll: reads keyboard/joystick into game state
typedef void* (*InputPoll_t)(int gameState);

// Input process: processes input for both players
typedef int (*InputProcess_t)(int gameState);

// Keyboard state check: returns 1 if key pressed
typedef int (*KeyboardState_t)(int keyCode);

// Joystick state: returns button bitmask
typedef int (*JoystickState_t)(int playerID);

// Entity reset
typedef int (*EntityReset_t)(int entity);

// ============================================================================
// RNG API (for deterministic rollback)
// ============================================================================

// Get current RNG seed (for savestate)
uint32_t AS2_GetRngSeed();

// Enable/disable extra verbose RNG logging (for debugging)
void AS2_SetExtraVerboseRngLogging(bool enable);

// Synchronize tick baseline for rollback netplay (fixes dword_816360 desync)
void AS2_SyncTickBaseline(uint32_t baseline);

// Clear synchronized tick baseline and restore normal timing behavior.
void AS2_ClearTickBaselineSync();

// Set RNG seed (for loadstate)
void AS2_SetRngSeed(uint32_t seed);

// Get/set visual RNG seed (for savestate/loadstate — weather particle determinism)
uint32_t AS2_GetVisualRngSeed();
void AS2_SetVisualRngSeed(uint32_t seed);

// ============================================================================
// Savestate API
// ============================================================================

// Save current game state to slot (0-7 for rollback, 8+ for manual saves)
bool AS2_SaveState(int slot);

// Load game state from slot
bool AS2_LoadState(int slot);

// Get preview of state in slot (for UI display)
bool AS2_GetStatePreview(int slot, StatePreview_t* preview);

// Check if slot has valid state
bool AS2_HasState(int slot);

// Buffer-based save/load for rollback integration
// Saves current game state directly to provided buffer (sizeof(CompactSaveState_t))
bool AS2_SaveStateToBuffer(CompactSaveState_t* buffer);

// Loads game state from provided buffer
bool AS2_LoadStateFromBuffer(const CompactSaveState_t* buffer);

// Fast save/load for rollback hot path — skips the full-buffer memset and
// batches VirtualProtect calls. Save callers that mutate the captured state
// afterward (for example counter normalization in the rollback session) should
// recompute the checksum with AS2_ComputeCompactStateChecksum() before using it
// for integrity checks.
void AS2_SaveStateFast(CompactSaveState_t* buffer);
void AS2_LoadStateFast(const CompactSaveState_t* buffer);

// Traced variants for rollback debugging. These preserve the fast path
// behavior but also emit detailed rollback trace records for every captured
// region and every byte range patched into live memory.
void AS2_SaveStateFastTrace(CompactSaveState_t* buffer, const char* traceScope, int traceFrame, uint32_t traceEventId);
void AS2_LoadStateFastTrace(const CompactSaveState_t* buffer, const char* traceScope, int traceFrame, uint32_t traceEventId);

// ============================================================================
// Rollback Savestate API (local-only)
// ============================================================================

// Save current game state to buffer (for rollback)
bool AS2_SaveCompactState(CompactSaveState_t* buffer);

// Load game state from buffer
bool AS2_LoadCompactState(const CompactSaveState_t* buffer);

// Build the deterministic rollback digest for an already-captured state and
// return its CRC32 checksum. Useful when a caller mutates a fast-captured state
// before handing it to GekkoNet or other integrity checks.
uint32_t AS2_ComputeCompactStateChecksum(const CompactSaveState_t* buffer);

// Format a compact one-line summary of a rollback savestate for diagnostics.
// Returns false if the output buffer is invalid.
bool AS2_FormatCompactStateSummary(const CompactSaveState_t* buffer, char* out, size_t outSize);

// Get size of rollback state (local-only; inputs are what travel over the network)
inline size_t AS2_GetCompactStateSize() { return sizeof(CompactSaveState_t); }

// ============================================================================
// FPU State Management (for floating-point determinism)
// ============================================================================

// Save current x87 FPU state to buffer (108 bytes via FSAVE + immediate FRSTOR)
void AS2_SaveFpuState(uint8_t* buffer);

// Restore x87 FPU state from buffer (108 bytes via FRSTOR with exception mask safety)
void AS2_RestoreFpuState(const uint8_t* buffer);

// Save/restore MXCSR (SSE control/status, 4 bytes) separately from x87 state
void AS2_SaveMxcsr(uint32_t* mxcsrOut);
void AS2_RestoreMxcsr(const uint32_t* mxcsrIn);

// ============================================================================
// Heap Tracking (for rollback memory leak prevention)
// ============================================================================

// Initialize heap tracking hooks (call once at startup)
void AS2_HeapTracker_Init();

// Shutdown heap tracking
void AS2_HeapTracker_Shutdown();

// Set current frame for heap tracking
void AS2_HeapTracker_SetFrame(int frame);

// Called when entering rollback mode - start tracking allocations
void AS2_HeapTracker_BeginRollback(int targetFrame);

// Called when exiting rollback - free allocations from rolled-back frames
void AS2_HeapTracker_EndRollback();

// Called when a frame is confirmed (will not be rolled back)
void AS2_HeapTracker_ConfirmFrame(int frame);

// Check if heap tracking is active
bool AS2_HeapTracker_IsActive();

// Get stats for debugging
void AS2_HeapTracker_GetStats(int* outTrackedAllocs, int* outRollbackFreed);

// Get current frame number
uint32_t AS2_GetFrameNumber();

// Check if in active match (mode 8 with match active)
bool AS2_IsInMatch();

// Check if in the Mode 8 Substate 3 core loop.
// This includes the opening intro lock and round-end transition countdown.
bool AS2_IsInGameplay();

// Check if inputs are currently affecting the fight inside Mode 8 Substate 3.
bool AS2_IsInPlayableGameplay();

// Check if in pause menu (mode 8, sub-state 4)
bool AS2_IsInPauseMenu();

// Check if in any menu (not in active gameplay)
bool AS2_IsInMenu();

// Desync debugging - logs all checksummed state values
void AS2_LogDesyncState(int netplayFrame);

// Desync debugging - logs per-field CRC breakdown of a saved state
// Call on SaveEvent to identify exactly which digest component diverges between peers.
void AS2_LogDigestBreakdown(const CompactSaveState_t* buffer, int frame);

// Periodic state logging - logs key values every N frames without spam
// Call every frame; internally tracks frame counter and only logs periodically
void AS2_LogPeriodicState(int netplayFrame, bool force = false);

// Clear vanilla netplay buffers to prevent interference with any custom netplay modes
void AS2_ClearVanillaNetplayBuffers();

// Log vanilla frame counters vs a caller-provided netplay frame for debugging
void AS2_LogFrameCounterState(int netplayFrame, const char* context);

// ============================================================================
// Checksum API (for desync detection)
// ============================================================================

// Calculate quick checksum using the game's native Entity_GetPositionChecksum function
// Returns: (P1_HP + P1_Meter + P1_X + P1_Y + P2_HP + P2_Meter + P2_X + P2_Y) % 0x10000
// This is fast and matches what the game uses internally for various randomization.
// Note: Requires valid P1 entity pointer (entity+4 must point to opponent)
uint16_t AS2_GetQuickChecksum();

// Game state accessors (for UI)
// GetGameMode() and GetSubstate() are now inline in game_state.h
uint16_t GetP1HP();
uint16_t GetP2HP();
uintptr_t GetEntityBase(int player);

// Debug content for embedding in tabs
void RenderInputDebugContent();
void RenderEffectDebugContent();

// Verbose logging control
void SetVerboseLogging(bool enabled);
bool GetVerboseLogging();

// Rollback trace logging control. When enabled, the rollback path writes a
// dedicated text index plus a binary trace stream containing the exact bytes
// captured or patched for each save/load/input event.
void AS2_SetRollbackTraceLogging(bool enabled);
bool AS2_GetRollbackTraceLogging();
const char* AS2_GetRollbackTraceTextPath();
const char* AS2_GetRollbackTraceBinaryPath();
uint32_t AS2_NextRollbackTraceEventId();
void AS2_LogRollbackTraceMessage(const char* scope, int frame, uint32_t eventId, const char* fmt, ...);
void AS2_DumpRollbackTraceBytes(const char* scope,
                                int frame,
                                uint32_t eventId,
                                const char* name,
                                uintptr_t address,
                                const void* beforeData,
                                size_t beforeSize,
                                const void* afterData,
                                size_t afterSize);
void AS2_DumpCompactStateTrace(const char* scope, int frame, uint32_t eventId, const CompactSaveState_t* state);



// Memory helper functions (safe versions with SEH protection)
bool ReadMemory(uintptr_t address, void* buffer, size_t size);
template<typename T> T ReadMemory(uintptr_t address);
bool WriteMemory(uintptr_t address, void* value, size_t size);
template<typename T> bool WriteMemory(uintptr_t address, T value);
bool CopyMemorySafe(void* dst, const void* src, size_t size);
bool WriteMemoryBlockSafe(void* dst, const void* src, size_t size);

// ============================================================================
// Exported Functions
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Called by d3d9 proxy on load
__declspec(dllexport) void ModInit(HMODULE gameModule);

// Called by d3d9 proxy on unload  
__declspec(dllexport) void ModShutdown();

// Called each frame by D3D Present hook (for ImGui rendering)
__declspec(dllexport) void ModOnPresent(void* pDevice);

// Returns true when a mod-owned screen should suppress the general tooling UI.
__declspec(dllexport) bool ModWantsExclusiveOverlay();

// Called to toggle the mod menu
__declspec(dllexport) void ModToggleMenu();

// Called when game is exiting (normal exit or crash)
// exitCode: 0 = normal exit, -1 = crash
// reason: description of why (e.g. "WM_CLOSE" or "CRASH: ACCESS_VIOLATION at 0x12345678")
__declspec(dllexport) void ModOnGameExit(int exitCode, const char* reason);

#ifdef __cplusplus
}
#endif

