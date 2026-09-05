/**
 * Alice Senki 2 - Constants / Addresses
 *
 * Centralized memory addresses, offsets, and fixed-size constants used by the mod.
 * Keep raw address macros out of feature headers.
 */

#pragma once

#include <stdint.h>

// Game state addresses + mode/substate constants
#include "game_state.h"

// ============================================================================
// Game Base Address (assuming no ASLR for old games)
// ============================================================================

#define GAME_BASE 0x00400000

// ============================================================================
// Critical Function Addresses (from disassembly analysis)
// ============================================================================

// Main game loop - vanilla limiter waits for integer 17ms (~58.8fps), handles mode switching
#define ADDR_MAIN_LOOP          (GAME_BASE + 0x1D2AC0)  // sub_5D2AC0

// Match mode handler (case 8 in main loop)
#define ADDR_MATCH_MODE         (GAME_BASE + 0x0C8F60)  // sub_4C8F60
#define ADDR_ASSET_LOAD_FROM_ARCHIVE     (GAME_BASE + 0x0A5390)  // sub_4A5390 - Asset_LoadFromArchive
#define ADDR_ASSET_LOAD_ALL_FROM_ARCHIVE (GAME_BASE + 0x14A460)  // Asset_LoadAllFromArchive
#define ADDR_HANDLE_ALLOC       (GAME_BASE + 0x212DF0)  // sub_612DF0
#define ADDR_HANDLE_RENDER_BIND (GAME_BASE + 0x2132E0)  // sub_6132E0
#define ADDR_HANDLE_SET_SOURCE  (GAME_BASE + 0x2207A0)  // sub_6207A0
#define ADDR_IMAGE_REGISTER_HANDLE (GAME_BASE + 0x220930)  // sub_620930
#define ADDR_IMAGE_CREATE_FROM_DECODED_BMP (GAME_BASE + 0x23A9B0)  // sub_63A9B0
#define ADDR_IMAGE_CREATE_FROM_FORMAT (GAME_BASE + 0x23AB70)  // sub_63AB70
#define ADDR_IMAGE_PARSE_FROM_BUFFER (GAME_BASE + 0x23E730)  // sub_63E730
#define ADDR_IMAGE_FORMAT_FREE  (GAME_BASE + 0x23E8C0)  // sub_63E8C0
#define ADDR_IMAGE_CREATE_SURFACE (GAME_BASE + 0x245DC0)  // Image_Create
#define ADDR_IMAGE_CREATE_SUBRECT (GAME_BASE + 0x245D10)  // Image_CreateSubRect
#define ADDR_IMAGE_UPLOAD_TO_HANDLE (GAME_BASE + 0x2460C0)  // sub_6460C0
#define ADDR_PIXELFORMAT_BUILD  (GAME_BASE + 0x246770)  // sub_646770
#define ADDR_HANDLE_FREE        (GAME_BASE + 0x212FF0)  // Handle_Free
#define ADDR_HANDLE_SYSTEM_ACTIVE 0x8FEA50  // g_HandleSystemActive
#define ADDR_HANDLE_TABLE       (GAME_BASE + 0x4FEA6C)  // g_HandleTable[32768]

// Input system
#define ADDR_INPUT_POLL         (GAME_BASE + 0x161F50)  // sub_561F50 - Main input poll
#define ADDR_INPUT_PROCESS      (GAME_BASE + 0x162060)  // sub_562060 - Input processing
#define ADDR_INPUT_DISPATCHER   (GAME_BASE + 0x1625E0)  // sub_5625E0 - Input_TryGetNextFrame (per-frame input dispatch)
#define ADDR_SEND_INPUT         (GAME_BASE + 0x162450)  // sub_562450 - Send local input packet
#define ADDR_RECV_INPUT         (GAME_BASE + 0x1623D0)  // sub_5623D0 - Receive remote input packet
#define ADDR_GET_SYNC_INPUT     (GAME_BASE + 0x1624E0)  // sub_5624E0 - Get synchronized inputs
#define ADDR_ADVANCE_FRAME      (GAME_BASE + 0x162760)  // sub_562760 - Frame_AdvanceSimulation
#define ADDR_MATCH_SYNC_INIT    (GAME_BASE + 0x162550)  // sub_562550 - Netplay_InitialSync
#define ADDR_KEYBOARD_STATE     (GAME_BASE + 0x22FD00)  // sub_62FD00 - Keyboard check
#define ADDR_JOYSTICK_STATE     (GAME_BASE + 0x22FF50)  // sub_62FF50 - Joystick check

// Vanilla timeout counters — must be kept at 0 to prevent auto-disconnect
#define ADDR_HOST_TIMEOUT_CTR   0x8EA200   // dword_8EA200
#define ADDR_CLIENT_TIMEOUT_CTR 0x8EA3A8   // g_NetTimeoutCounter

// Timing
// sub_635F80 - wrapper around timeGetTime() used by the main loop frame limiter
#define ADDR_GET_TICK           (GAME_BASE + 0x235F80)  // sub_635F80
#define ADDR_REPLAY_SAVE        (GAME_BASE + 0x19B830)  // sub_59B830
#define ADDR_REPLAY_SELECT_DRAW (GAME_BASE + 0x19BF90)  // sub_59BF90

// Static CRT functions (game has statically linked CRT - NOT importing from msvcrt.dll)
// RNG uses TLS-based _tiddata struct: seed at [_getptd()+0x14]
#define ADDR_STATIC_SRAND       (GAME_BASE + 0x314590)  // Static CRT srand()  — 0x714590
#define ADDR_STATIC_RAND        (GAME_BASE + 0x31459D)  // Static CRT rand()   — 0x71459D
#define ADDR_GETPTD             (GAME_BASE + 0x316D29)  // _getptd() — returns per-thread data struct
#define RNG_SEED_OFFSET         0x14                     // holdrand offset within _tiddata

// Entity state functions
#define ADDR_ENTITY_STATE_SET   (GAME_BASE + 0x0BF630)  // sub_4BF630
#define ADDR_ENTITY_RESET       (GAME_BASE + 0x09E720)  // sub_49E720
#define ADDR_ENTITY_INIT        (GAME_BASE + 0x09E050)  // sub_49E050
#define ADDR_CAN_ACT_SET        (GAME_BASE + 0x09E040)  // sub_49E040
#define ADDR_COMBO_TRACKING     (GAME_BASE + 0x09E8F0)  // sub_49E8F0
#define ADDR_ENTITY_SET_ACTION_RESET (GAME_BASE + 0x09F440)  // sub_49F440
#define ADDR_ENTITY_RESET_HIT_DATA   ADDR_ENTITY_RESET       // sub_49E720

// Entity checksum function
#define ADDR_ENTITY_GET_CHECKSUM (GAME_BASE + 0x09EE60)  // sub_49EE60

// Sound/effect
#define ADDR_EFFECT_SET_PARAMS  (GAME_BASE + 0x0C3ED0)  // sub_4C3ED0 — Effect_SetParams1 (entity state writer, NOT sound)
#define ADDR_MATCH_RENDER_PLAYERS (GAME_BASE + 0x0C6B60) // sub_4C6B60 — match player renderer
#define ADDR_MATCH_HUD_RENDER     (GAME_BASE + 0x0C05B0) // sub_4C05B0 — match HUD (nickname bars)
#define ADDR_NAME_BAR_TEXTURE     0x816038               // dword_816038 — gradient bar texture
#define ADDR_SE_PLAY            (GAME_BASE + 0x0C3C00)  // sub_4C3C00 — SE_Play (actual sound effect trigger)
// Audio_IsPlaying (0x62C840): queries LIVE DirectSound buffer state. The
// simulation branches on it — Entity_UpdateAudio only plays a voice and
// updates the captured voice bookkeeping when it returns false — so truth and
// replay ticks, which run at different real times, can take DIFFERENT
// branches. Hooked for record/replay (rollback_audio), the qoh99 hkSoundStatus
// pattern; masking the resulting bytes only hid the divergence.
#define ADDR_AUDIO_IS_PLAYING   0x62C840
// Audio_Play_Wrapper (0x5D3410): Audio_Stop + Audio_Play. Hooked so the
// canonical voice model knows the frame a voice STARTED, without which
// Audio_IsPlaying cannot be answered deterministically.
#define ADDR_AUDIO_PLAY_VOICE 0x5D3410
#define ADDR_MATCH_SCORE_STATS  (GAME_BASE + 0x15BCD0)  // sub_55BCD0 — Match_UpdateScoreStats: cumulative
                                                        // score/rank/continuation `+=` globals OUTSIDE the snapshot
                                                        // regions, called on the round-end commit tick. Hooked so a
                                                        // rollback resim across the commit cannot double-apply
                                                        // (SAVESTATE_AUDIT F6).

// Legacy aliases (kept for backward compatibility / reference)
#define ADDR_SOUND_TRIGGER      ADDR_EFFECT_SET_PARAMS   // DEPRECATED: was misidentified as sound trigger
#define ADDR_BGM_CONTROL        ADDR_SE_PLAY             // DEPRECATED: was misidentified as BGM control

// Network functions (existing game netcode - UDP based)
#define ADDR_NET_INIT_HOST      (GAME_BASE + 0x1FB9E0)  // sub_5FB9E0
#define ADDR_NET_INIT_CLIENT    (GAME_BASE + 0x1FBBC0)  // sub_5FBBC0
#define ADDR_NET_RECV_HOST      (GAME_BASE + 0x1FBA80)  // sub_5FBA80
#define ADDR_NET_RECV_CLIENT    (GAME_BASE + 0x1FBC40)  // sub_5FBC40
#define ADDR_NET_SEND_HOST      (GAME_BASE + 0x1FBB60)  // sub_5FBB60
#define ADDR_NET_SEND_CLIENT    (GAME_BASE + 0x1FBCA0)  // sub_5FBCA0
#define ADDR_NET_CLOSE_HOST     (GAME_BASE + 0x1FBBA0)  // sub_5FBBA0
#define ADDR_NET_CLOSE_CLIENT   (GAME_BASE + 0x1FBCE0)  // sub_5FBCE0

// ============================================================================
// Global State Addresses
// ============================================================================

#define ADDR_GAME_STATE_BASE    0x816358  // dword_816358

// Debug/Logging
#define ADDR_QUIT_FLAG          0x816358
// NAMING TRAP (SAVESTATE_AUDIT §1): ADDR_FRAME_COUNTER (0x81635C) is the
// RENDER-LOOP frame counter (++ once per Game_MainLoop pass, presentation
// side, TIMING — excluded from the gameplay digest). Its near-namesake
// ADDR_FRAME_DISPLAY (0x816494, `Frame_Display`) is SIM state: incremented
// once per SIM tick by Frame_AdvanceDisplay and read by sim logic every tick
// (215900 forced-draw check, replay-end check). Do not confuse the two —
// 0x816494 is captured/restored/HASHED by GameSnapshot; 0x81635C is
// captured/restored but never hashed.
#define ADDR_FRAME_COUNTER      0x81635C
#define ADDR_SIM_FRAME_COUNTER  0x816490
#define ADDR_LAST_FRAME_TIME    0x816360
#define ADDR_FPS_COUNT          0x816364
#define ADDR_MATCH_INTRO_FADE_TIMER (ADDR_GAME_STATE_BASE + 0x18)  // 0x816370 - native intro/fade countdown that releases match input lock
#define ADDR_MATCH_ACTIVE       ADDR_GAME_TYPE  // DEPRECATED (was misnamed). Use ADDR_GAME_TYPE.
#define ADDR_CURRENT_PLAYER     0x76C5FC

// ============================================================================
// Vanilla Netplay Frame Tracking Addresses
// ============================================================================

#define ADDR_FRAME_SIMULATION   0x816490
#define ADDR_FRAME_DISPLAY      0x816494  // `Frame_Display` — SIM state (see naming-trap
                                          // note at ADDR_FRAME_COUNTER above): ++ per sim
                                          // tick, sim-read (215900 forced-draw check).
#define ADDR_FRAME_WRITE_IDX    0x816498
#define ADDR_FRAME_NET_IDX      0x81649C
#define ADDR_REMOTE_FRAME       0x87FC20

#define ADDR_VANILLA_LOCAL_INPUTS   0x8164A0
#define ADDR_VANILLA_REMOTE_INPUTS  0x87FC24
#define VANILLA_LOCAL_INPUT_SIZE    170978
#define VANILLA_REMOTE_INPUT_SIZE   157672

#define ADDR_VANILLA_SYNC_LOCAL     0x8E93A4
#define ADDR_VANILLA_SYNC_REMOTE    0x8E93AE

#define ADDR_MATCH_BASE         0x76C5F8
#define MATCH_HEADER_SIZE       0x1D30

// Per-frame temp data: 68 bytes at match+0x700 (0x76CCF8).
// Cleared by Match_ClearPerFrameTempData (sub_4C3BE0) at the top of
// Game_Update_MatchLoop, OUTSIDE the while(!Input_TryGetNextFrame) loop.
// During rollback resimulation the while-loop iterates multiple times
// without returning to the outer function, so this clearing never runs
// between resim frames.  We must clear it ourselves before each frame
// to prevent stale collision/hit temp data from bleeding across frames.
// Sound-system handle storage inside the match struct. These are DirectSound
// handles, not gameplay state, and their VALUES are per-process: the allocator
// (0x62A480) bakes a process-global monotonic serial into every handle --
//     *(_DWORD *)*v3 = dword_9D0454++;            (decomp:321905)
//     return v2 | ((*(_DWORD *)*v3 | 0x1000) << 16);  (decomp:321934)
// so two peers that allocated a DIFFERENT NUMBER of sounds before the match
// hold different handle values for the same sounds. Stored unmasked inside the
// hashed region, that is a silent cross-machine desync source -- and invisible
// to same-machine testing, because two instances of the same build share an
// allocation history. Digest-masked on the F4/F7h rationale (sound-system
// bookkeeping, no gameplay meaning); still captured and restored.
// F10a: 175 UI/HUD sprite handles from DATA/prm.bin, match+12..+711.
// Written once per match load by sub_4C0490 (decomp:112341-112376), which is
// called with the match base (decomp:117524, inside sub_4C8FF0 whose a2 is
// &unk_76C5F8). Destination indices -- NOT the source asset index, which runs
// to 237 and is easy to mistake for the count:
//     a1[3]..a1[31]   29   a1[32],a1[33]    2
//     a1[34]..a1[81]  48   a1[82],a1[83]    2
//     a1[84]..a1[177] 94
// = 175 dwords at a1[3..177] = bytes 12..711, and 12 + 175*4 == 712, so the
// array butts the announcer block exactly.
//
// These are IMAGE handles, so the allocator is sub_612DF0 rather than the sound
// allocator, and BOTH halves of the value are process-history dependent: the
// low word is the first free slot in g_HandleTable, the high word is a
// monotonic serial (dword_91EA74++ | 0x800) with no reset site. Two peers whose
// processes have loaded a different NUMBER of images hold different values for
// the same sprites, with identical gameplay. The simulation never branches on a
// handle -- they are opaque ids handed to draw calls -- and an
// absolute-address/match-symbol scan finds no reference to the range outside
// the loader. Same class as F9 and F7h, and same blind spot: invisible to
// same-machine testing, where both instances allocate in lockstep.
// Process-global IMAGE-handle allocation serial, incremented by sub_612DF0
// (decomp:302974 `*v3 = dword_91EA74++`, wrapped at 2047) and packed into the
// high word of every handle it returns. No reset site anywhere in the binary,
// which is what makes the handle tables below process-history dependent.
#define ADDR_IMAGE_HANDLE_SERIAL     0x91EA74

#define MATCH_UI_IMAGE_HANDLES_OFF   0x00C                // match+12..711 (175 handles)
#define MATCH_UI_IMAGE_HANDLES_SIZE  0x2BC

#define MATCH_ANNOUNCER_HANDLES_OFF  0x2C8                // match+712..763
#define MATCH_ANNOUNCER_HANDLES_SIZE 0x34
// F10b: 53 effect-sprite handles from DATA/eft.bin, match+764..+975. Written by
// sub_4A9280 (decomp:108566-108570): Asset_LoadAllFromArchive(a1 + 764, ...),
// called with the match base at decomp:117526. The shipped DATA/eft.bin header
// dword is 0x30810435, and 0x30810435 ^ 0x30810400 == 53 assets == 212 bytes,
// so 764 + 212 == 976 and the array butts SE_Handles exactly. With F10a below
// it, match+12..1859 is now one contiguous span of non-gameplay handle storage.
// Same allocator and same process-global provenance as F10a.
#define MATCH_EFT_IMAGE_HANDLES_OFF  0x2FC                // match+764..975 (53 handles)
#define MATCH_EFT_IMAGE_HANDLES_SIZE 0x0D4

// F10d: the STAGE BACKGROUND image handle, match+7468 (0x76E324) -- a single
// dword sitting immediately below ADDR_EFFECT_ARRAY (match+7472).
//     sub_4C3D90 (decomp:114303-114304):
//         result = Asset_LoadFromArchive(aDataStgBin, aDataStgPal,
//                                        BYTE1(dword_816470), 0);
//         *(_DWORD *)(a1 + 7468) = result;
// Same sub_612DF0 provenance as F10a-c, so the VALUE carries the process-global
// serial. Its only other accessor in the decomp is Weather_Draw, which reads it
// to draw; no simulation reader.
//
// Found by byte-level fine-diag (AS2_FINE_DIAG=1) under an asymmetric
// handle_serial_skew run: after F10a/F10b/F10c the ONLY remaining unmasked
// differing 64-byte window was match+[7424,7488), and this dword is the one
// thing written inside it.
// Weather particle pool: 200 slots x 28 bytes at match+1868, seeded once by
// Weather_Init and thereafter touched ONLY by the render phase - sub_4C47C0
// updates it (and calls rand() doing so), sub_4C6260 / sub_4C63A0 draw it.
// It ends at exactly 7468, where the stage image handle begins.
//
// Inert on every stage but Patton (stage id 2), the one stage with a weather
// effect - which is why a render-cadence window sitting inside the hashed
// region went unnoticed.
#define MATCH_WEATHER_PARTICLES_OFF  0x74C                 // match+1868
#define MATCH_WEATHER_PARTICLES_SIZE 0x15E0                // 200 * 28 = 5600

#define MATCH_STAGE_IMAGE_HANDLE_OFF 0x1D2C                // match+7468, 1 handle
#define MATCH_STAGE_IMAGE_HANDLE_SIZE 0x004

#define MATCH_SE_HANDLES_OFF         0x3D0                // match+976..1791 (204 handles)
#define MATCH_SE_HANDLES_SIZE        0x330
#define MATCH_PER_FRAME_TEMP_OFFSET  0x700                // match + 0x700 = 0x76CCF8
#define MATCH_PER_FRAME_TEMP_SIZE    0x44                 // 68 bytes (memset to 0)
#define ADDR_MATCH_PER_FRAME_TEMP    (ADDR_MATCH_BASE + MATCH_PER_FRAME_TEMP_OFFSET)

// Match context gap: state between the 16-byte match header and the effect array.
// Contains camera scroll, screen shake, weather particles, and misc match state.
// Camera scroll (match+1860) is read by HitDef_Create, boundary clamping, and AI logic.
// Weather particles (match+1868, 200×28 bytes) call rand() every frame — if not
// restored during rollback, the RNG sequence diverges and ALL gameplay desyncs.
#define ADDR_MATCH_CONTEXT      (ADDR_MATCH_BASE + 16)   // 0x76C608
#define MATCH_CONTEXT_SIZE      (ADDR_EFFECT_ARRAY - ADDR_MATCH_CONTEXT)  // 7456 bytes (0x1D20)

// Camera scroll (offsets from ADDR_MATCH_BASE, NOT match_context).
// Weather_UpdateScroll (sub_4C4230) receives gameState = match_base.
// scrollX: clamped [0, 959], scrollY: clamped [0, 319].
// World-to-screen: screenX = worldX/10 - scrollX, screenY = worldY/10 - scrollY.
#define MATCH_OFF_SCROLL_X      0x744                     // int16; match_base + 0x744
#define MATCH_OFF_SCROLL_Y      0x746                     // int16; match_base + 0x746
#define ADDR_SCROLL_X           (ADDR_MATCH_BASE + MATCH_OFF_SCROLL_X)  // 0x76CD3C
#define ADDR_SCROLL_Y           (ADDR_MATCH_BASE + MATCH_OFF_SCROLL_Y)  // 0x76CD3E

// ============================================================================
// Mode 9 continue screen (sub 4, handler sub_601BB0 @ 0x601BB0)
// ============================================================================

// byte_8EA3B0 — continue-screen cursor (0 = YES, 1 = NO). Decomp: sub_601BB0
// toggles it on either player's LEFT/RIGHT just-pressed words and confirms on
// A/C; no countdown. Banners/SEs (handles 0x8EA3C4..D0, SEs 0x8EA450/54) are
// loaded unconditionally by sub_5FBEE0 for every mode-9 entry.
#define ADDR_CONTINUE_CURSOR    0x8EA3B0

// ============================================================================
// Netplay state addresses (vanilla)
// ============================================================================

#define ADDR_NETPLAY_PORT       0x8E9480   // Network port (hostshort)
#define ADDR_NETPLAY_ROLE       0x816474   // 1=host, 0=client
#define ADDR_NETPLAY_CONNECTED  0x816475   // 1=connected

// Input Configuration (offsets from ADDR_GAME_STATE_BASE)
#define OFF_P1_JOY_ID           864440
#define OFF_P1_BUTTON_MASKS     864444
#define OFF_P2_JOY_ID           864484
#define OFF_P2_BUTTON_MASKS     864488
#define OFF_P1_INPUT_STATE      867110
#define OFF_P2_INPUT_STATE      867318

// ============================================================================
// Entity Memory Layout
// ============================================================================

#define ADDR_ENTITY_ARRAY       0x776668
#define ADDR_ENTITY_ARRAY_ALT   0x77666C
#define ENTITY_ARRAY_STRIDE     27203

#define ADDR_P1_ENTITY_BASE     0x776668
#define ADDR_P2_ENTITY_BASE     0x790F74
#define ENTITY_SIZE             0x1A90C

// ============================================================================
// Sound System Addresses
// ============================================================================

#define ADDR_DSOUND_INTERFACE   0x9CC414
#define ADDR_DSOUND_PRIMARY     0x9CC418
#define ADDR_DSOUND_LOADER      0x9CC408
#define ADDR_DSOUND_PERF        0x9CC40C
#define ADDR_SOUND_POOL         0x9CC44C
#define ADDR_SOUND_MODE         0x9D046C
#define ADDR_SOUND_STATE        0x9D0470
#define SOUND_POOL_SIZE         4096

#define ADDR_SOUND_INIT         (GAME_BASE + 0x229AA0)
#define ADDR_SOUND_LOAD         (GAME_BASE + 0x14A840)
#define ADDR_SOUND_PLAY         (GAME_BASE + 0x22AB40)
#define ADDR_SOUND_CREATE       (GAME_BASE + 0x22C060)
#define ADDR_SOUND_QUICK        (GAME_BASE + 0x22C340)

// --- Menu presentation: vanilla asset/SFX/BGM loaders (net.bin / rep.bin) ---
#define ADDR_MENU_SFX_LOAD3         (GAME_BASE + 0x14AA00)  // sub_54AA00(int* dst3, char* binPath) - loads a 3-handle SFX bank
#define ADDR_BGM_PLAY_TRACK         (GAME_BASE + 0x1D3380)  // BGM_PlayTrack(track) - 0..77, 255 stops (loads track from bgm.bin: ~0.5s)
#define ADDR_BGM_LOAD_TRACK         (GAME_BASE + 0x14ABE0)  // sub_54ABE0(char* bgmBin, track) - loads one track, returns handle
#define ADDR_AUDIO_PLAY             (GAME_BASE + 0x22C5F0)  // Audio_Play(handle, mode, immediate) - mode 3 = looping BGM
#define ADDR_AUDIO_STOP             (GAME_BASE + 0x22C780)  // Audio_Stop(handle) - stops playback, keeps buffer resident
#define ADDR_AUDIO_PLAY_HANDLE      (GAME_BASE + 0x1D3410)  // Audio_Play_Wrapper(handle)
#define ADDR_AUDIO_SET_VOLUME_LEVEL (GAME_BASE + 0x1D3450)  // Audio_SetVolumeLevel(handle, level)
#define ADDR_MENU_SFX_VOLUME_BYTE   0x8E9409                // BYTE1(dword_8E9408) - menu SFX volume level
#define ADDR_BGM_CURRENT_HANDLE     0x816374                // dword_816374 - currently playing BGM handle (-1 = none)
#define ADDR_BGM_ENABLED            0x816378                // dword_816378 - BGM enabled flag (1 = on)

// Network menu (vanilla mode 4 slots; mode 4 never runs in the mod, so we reuse them)
#define ADDR_NET_MENU_BG_HANDLE     0x7AC2A4  // data\net.bin background sprite (dword_7AC2A4)
#define ADDR_NET_MENU_SFX_CURSOR    0x7AC304  // wave\net.bin handle: cursor move (dword_7AC304)
#define ADDR_NET_MENU_SFX_CONFIRM   0x7AC308  // confirm (dword_7AC308)
#define ADDR_NET_MENU_SFX_CANCEL    0x7AC30C  // cancel (dword_7AC30C)

// Replay select menu (vanilla mode 5 - init still runs and loads these handles)
#define ADDR_REPLAY_MENU_SFX_CURSOR  0x815E88  // wave\rep.bin handle: cursor move (dword_815E88)
#define ADDR_REPLAY_MENU_SFX_CONFIRM 0x815E8C  // confirm (dword_815E8C)
#define ADDR_REPLAY_MENU_SFX_CANCEL  0x815E90  // cancel (dword_815E90)

#define NET_MENU_BGM_TRACK   74  // network menu music
#define MAIN_MENU_BGM_TRACK  0   // title/main menu music

// ============================================================================
// Input Structures
// ============================================================================

#define INPUT_OFF_CURRENT       64
#define INPUT_OFF_PREVIOUS      76
#define INPUT_OFF_JUST_PRESSED  88

#define ADDR_P1_INPUT_BUFFER    0x8E9E62
#define ADDR_P2_INPUT_BUFFER    0x8E9F32
// Global per-player input buffer is 104 words (208 bytes):
// held, previous, just-pressed, cooldown, rapid-fire, hold counters, reserves.
#define INPUT_BUFFER_SIZE       208

#define ADDR_P1_INPUT_STATE     0x8E9E9A
#define ADDR_P2_INPUT_STATE     0x8E9F6A
// Just-pressed array is 10 words (20 bytes) at base + 56.
#define INPUT_STATE_SIZE        20

// Title-screen state dword (decomp dword_8EA000): LOWORD = attract-timeout
// counter (0..1800), BYTE2 = title menu cursor. Its low WORD sits INSIDE the
// last two bytes of the P2 input-buffer span (0x8E9F32 + 208 = 0x8EA002), so
// per-side title wall-time residue leaks into every baseline capture and
// engine2 sync hash unless zeroed at the synchronized pre-capture point
// (2026-08-17 f0 confirmed-desync root cause, live-verified: host held 0x0072
// there, instB 0x0000).
#define ADDR_TITLE_SCREEN_STATE          0x8EA000
#define TITLE_STATE_IN_INPUT_SPAN_SIZE   2

#define ADDR_INPUT_READ_IDX     0x816490
#define ADDR_INPUT_DISPLAY_IDX  0x816494
#define ADDR_INPUT_WRITE_IDX    0x816498
#define ADDR_INPUT_NET_IDX      0x81649C

#define ADDR_P1_INPUT_HISTORY   0x8164A0
#define ADDR_P2_INPUT_HISTORY   0x87FC24
#define INPUT_HISTORY_MAX       216000
#define INPUT_HISTORY_P1_SIZE   170978
#define INPUT_HISTORY_P2_SIZE   157672

#define ADDR_REPLAY_SELECT_INDEX   0x7AC540
#define ADDR_REPLAY_SELECT_COUNT   0x7AC544
#define ADDR_REPLAY_SELECT_RESULT  0x7AC548
#define ADDR_REPLAY_SELECT_DISPLAY 0x7AC54C
#define ADDR_REPLAY_HEADER_BASE    0x7AC63C
#define ADDR_REPLAY_ELEMENT_COUNT  0x7AC680
#define ADDR_REPLAY_INPUT_BASE     0x7AC684

#define INPUT_HISTORY_WINDOW    20

#define ADDR_DINPUT_KEYBOARD    0x9D09CC
#define ADDR_DINPUT_JOYSTICK    0x9D2AE8
#define DINPUT_JOY_STRUCT_SIZE  664
#define DINPUT_JOY_MAX          16
// ADDR_DINPUT_JOYSTICK points at the first axis field (DXLib base+0x10).
// sub_62FF50 reads button bytes from DXLib base+0x40, so the relative offset
// from ADDR_DINPUT_JOYSTICK is 0x30.
#define DINPUT_JOY_BTN_OFFSET   48

#define ADDR_DINPUT_INTERFACE   0x9D09B8
#define ADDR_DINPUT_KB_DEVICE   0x9D09C0
// Mouse DInput device (DIMOUSESTATE2, 20-byte GetDeviceState in sub_62FE60).
// Acquired in sub_62EE80 with DISCL_BACKGROUND|DISCL_NONEXCLUSIVE (0x0A).
#define ADDR_DINPUT_MOUSE_DEVICE 0x9D09BC
#define ADDR_DINPUT_FALLBACK    0x9D09B0

// Game_MainLoop (sub_5D2AC0). Calls keybd_event(7,0,KEYEVENTF_KEYUP,0) EVERY frame: a phantom
// VK 0x07 injection that defeats the shell's clean Win-down/up (Start menu) and Alt+Shift layout
// chord detection. ROOT CAUSE of "Win key / Alt+Shift dead while game focused" (vanilla + mod).
// Fixed by IAT-hooking keybd_event in wsock32_proxy and dropping bVk==0x07. See SHELL_HOTKEY_POLICY.md.
#define ADDR_GAME_MAINLOOP      0x5D2AC0
#define DXLIB_PHANTOM_WINKEY_VK 0x07

// Frame-limiter busy-spin cluster inside Game_MainLoop (decomp L266504–266510).
// REAL shipping-exe bytes (cluster at 0x5D2C01, verified 2026-08-17 — the
// decomp's `sub eax,[mem]` form does not exist in the binary): two identical
// 21-byte blocks `push 0 / call sub_635F80 / mov edx,[ADDR_LAST_FRAME_TIME] /
// add esp,4 / sub eax,edx / cmp eax,17`, joined head `7D 17` (jge over the
// loop) and loop `7C E9` (jl back) — 46 bytes; the dword_816360 re-stamp at
// 0x5D2C2F follows and is left untouched. The re0.7 FrameScheduler
// byte-signature-scans this window and detours the cluster to
// FrameScheduler_WaitForNextFrame (src/patches/frame_scheduler.cpp);
// the signature must match exactly once or the install fails loud (risk R-1).
#define ADDR_FRAME_LIMITER_SCAN_BEGIN  ADDR_GAME_MAINLOOP
#define ADDR_FRAME_LIMITER_SCAN_SIZE   0x600

// Window_GetHandle (sub_6349F0) returns HWND global wParam @ 0x9DB648.
// Do NOT use sub_620C70 — IDA mislabels it Sys_GetWindowHandle but it returns IDirectDraw* ppv.
#define ADDR_WINDOW_GET_HANDLE   (GAME_BASE + 0x2349F0)
#define ADDR_GAME_HWND           0x9DB648
#define ADDR_SYS_GET_WINDOW_HANDLE ADDR_WINDOW_GET_HANDLE

// sub_634910 — DXLib message-pump idle gate. When uiParam (window active) is 0 the
// game thread spins until focus returns, freezing netplay post-match phases.
#define ADDR_GAME_MESSAGE_PUMP_IDLE (GAME_BASE + 0x234910)  // sub_634910
#define ADDR_GAME_WINDOW_ACTIVE       0x9DB6A4                // uiParam (WM_ACTIVATE)
#define ADDR_GAME_RUN_IN_BACKGROUND   0x9E5CA0                // g_bRunInBackground

#define ADDR_DINPUT_KB_REFRESH  (GAME_BASE + 0x230130)
#define ADDR_DINPUT_JOY_REFRESH (GAME_BASE + 0x2302F0)

// Vanilla shell-hotkey suppression state.
// On NT-family Windows the game installs an external message hook DLL and on
// older Win9x it uses SPI_SETSCREENSAVERRUNNING; both are gated by this flag.
// When non-zero, game wndproc uses its custom handler and may return 0 for shell keys
// instead of calling DefWindowProc. Also used as g_LogToDebugOnly for file logging.
#define ADDR_GAME_WNDPROC_CUSTOM_HANDLER  0x9DB660
#define ADDR_GAME_WNDPROC_CUSTOM_PROC_PTR 0x9DB668
#define ADDR_GAME_WNDPROC_MSG_CALLBACK    0x9E5CB8
#define ADDR_GAME_WNDPROC                 (GAME_BASE + 0x233490)  // sub_633490 DXLib wndproc
// sub_63a110: DXLib per-frame windowed-size enforcer. Compares the client rect
// against the engine's expected size and MoveWindow/sub_634bc0's it back on any
// mismatch. In retail this whole function is skipped because the vanilla
// msg-hook helper arms ADDR_GAME_WNDPROC_CUSTOM_HANDLER (0x9DB660) = 1 at
// startup; the shell-hotkey layer forces that gate to 0, which re-arms this
// enforcer and makes every user edge-drag resize snap back to 640x480 (the
// wndproc invokes it on WM_SIZE for every wParam except SIZE_MAXIMIZED, which
// is why only maximize survived). See ShellHotkeyPatch_Install.
#define ADDR_GAME_WINDOW_SIZE_ENFORCER    (GAME_BASE + 0x23A110)  // sub_63a110
#define ADDR_GAME_CURSOR_REQUEST_STATE    0x9DB6D0  // g_nCursorRequestState
#define ADDR_GAME_CURSOR_CURRENT_SHOWN    0x9DB6D4  // g_bCursorCurrentState
#define ADDR_GAME_MOUSE_WHEEL_COUNTER     0x9DB6D8  // WM_MOUSEWHEEL delta accumulator

#define ADDR_SHELL_HOTKEY_SUPPRESS_FLAG  0x9E5B74
#define ADDR_SHELL_HOTKEY_MSG_HOOK       0x9E5B7C
#define ADDR_SHELL_HOTKEY_LOADED_FLAG    0x9E5B80
#define ADDR_SHELL_HOTKEY_HOOK_MODULE    0x9E5C8C
#define ADDR_SHELL_HOTKEY_TEMP_DLL_PATH  0x9E5B84
#define ADDR_SHELL_HOTKEY_TEMP_DLL_OWNED 0x9E5C88

// Verified Layer-B patch RVAs in shipping as2.exe (sub_633490 region).
#define RVA_PATCH_SC_KEYMENU_SWALLOW      0x233DFE
#define RVA_PATCH_SC_TASKLIST_SWALLOW     0x233E21
#define RVA_PATCH_SC_SCREENSAVE_SWALLOW   0x233E38
#define RVA_PATCH_DEFWINDOWPROC_GATE      0x233F25
#define RVA_PATCH_CUSTOM_PROC_GUARD       0x2334B5
#define RVA_PATCH_SHELL_HELPER_ARM_GUARD  0x233979
#define RVA_PATCH_CURSOR_HIDE_LOOP        0x234107
#define RVA_PATCH_DI_KB_COOP_ARG1         0x22F1A6
#define RVA_PATCH_DI_KB_COOP_ARG2         0x22F2F1

#define JOY_DOWN    0x0001
#define JOY_UP      0x0002
#define JOY_LEFT    0x0004
#define JOY_RIGHT   0x0008
#define JOY_BTN_A   0x0010
#define JOY_BTN_B   0x0020
#define JOY_BTN_C   0x0040
#define JOY_BTN_D   0x0080
#define JOY_START   0x0100
#define JOY_SELECT  0x0200

// ============================================================================
// Effect/Projectile System Addresses
// ============================================================================
// Effect array: 200 slots, 32 bytes per entry, circular buffer
// sub_4A92C0 spawns effects, sub_4A9330 updates all effects

// Gap between ADDR_EFFECT_INDEX and ADDR_MATCH_BASE (12 bytes at 0x76C5EC..0x76C5F7)
// Contains: byte_76C5EC (render fade/blend), 3B padding, SE_ChannelIndex (audio), 4B unknown.
// Not critical for simulation, but captured for determinism completeness.
#define ADDR_PRE_MATCH_GAP      0x76C5EC
#define PRE_MATCH_GAP_SIZE      12        // 0x76C5EC to 0x76C5F7 inclusive

// AI pattern-learning cross-frame statics (SAVESTATE_AUDIT F1): four dwords
// immediately below ADDR_EFFECT_INDEX — dword_76C5D8/dword_76C5DC
// (AI_ExecutePattern, decomp L101691) and dword_76C5E0/dword_76C5E4
// (AI_RecordPattern, decomp L101854). They gate CRT rand() consumption and
// persist across matches, so they are captured, restored AND hashed by
// GameSnapshot (SIM region). Zeroed at the netplay startup handoff so both
// peers hash identical values from frame 0.
#define ADDR_AI_LEARN_STATICS   0x76C5D8
#define AI_LEARN_STATICS_SIZE   16        // 0x76C5D8 .. 0x76C5E7 inclusive

// AI learning master gate: the config.dat "CPU learning" option byte
// (`AI_PatternModeEnabled` = byte_8E940D, loaded at decomp L200417). Gates
// AI_RecordPattern (runs for HUMAN players too, twice per sim tick), the
// substate-0 learning-block load (sub_49FE50) and the match-end save/free
// (sub_4A0A90). Forced to 0 for the duration of a netplay session by the
// match director (SAVESTATE_AUDIT F1 fix).
#define ADDR_AI_PATTERN_MODE    0x8E940D

#define ADDR_EFFECT_ARRAY       0x76E328  // dword_76E328[] - Effect entity pointers
#define ADDR_EFFECT_INDEX       0x76C5E8  // dword_76C5E8 - Current write index (wraps at 200)
#define ADDR_EFFECT_TYPE        0x76E32C  // byte_76E32C[] - Effect type per slot
#define ADDR_EFFECT_X           0x76E32E  // word_76E32E[] - X position per slot
#define ADDR_EFFECT_Y           0x76E330  // word_76E330[] - Y position per slot
#define ADDR_EFFECT_TIMER       0x76E332  // word_76E332[] - Timer/lifetime per slot
#define ADDR_EFFECT_DATA        0x76E334  // unk_76E334[] - Additional effect data (5 DWORDs)

#define EFFECT_MAX_SLOTS        200       // Maximum effects (0-199, wraps at 0xC7)
#define EFFECT_ENTRY_SIZE       32        // 32 bytes per effect entry

// Summon/Assist Array (separate from visual effects)
// 100 slots × 272 bytes = 27200 bytes, immediately after effects
#define ADDR_SUMMON_ARRAY       0x76FC28  // unk_76FC28[] - Summon entity array
#define SUMMON_MAX_SLOTS        100       // Maximum summons
#define SUMMON_ENTRY_SIZE       272       // 272 bytes per summon

// HitDef entry layout (within ADDR_SUMMON_ARRAY, 272 bytes per entry).
// Created by HitDef_Create (sub_4BE100 area), used by Entity_UpdateHitDetection.
#define HITDEF_OFF_OWNER        0         // byte  — owning player index
#define HITDEF_OFF_ID           4         // DWORD — unique ID (0 = free slot)
#define HITDEF_OFF_TYPE         8         // byte  — hitbox type
#define HITDEF_OFF_ACTIVE       9         // byte  — invulnerability countdown (starts 1, decrements to 0)
// Shared attack payload starts at entry+12 and mirrors the direct-entity block
// at entity+1736: Entity_UpdateSummonHitDetection passes entry+12 to the same
// resolvers that receive entity+1736. Offset +16 is the attack/guard mask, NOT
// damage - the native AI threat lookup (sub_4A8C20) reads it as a mask and the
// summon collision path bit-tests 0x1000 / 0x20000 on it. The real damage field
// is not traced yet, so no replacement offset is claimed.
#define HITDEF_OFF_PAYLOAD       12       // payload base (same layout as entity+1736)
#define HITDEF_OFF_ATTACK_STATE  12       // byte  — payload+0: 0=inactive, 1=active
#define HITDEF_OFF_ATTACK_MASK   16       // DWORD — payload+4: guard lanes + attack flags
#define HITDEF_OFF_ATTACK_LEVEL  20       // DWORD — payload+8: attack level
#define HITDEF_OFF_HIT_ACTIVE    24       // byte  — payload+12: active hit-data flag
#define HITDEF_OFF_ACTIVE_FLAG   HITDEF_OFF_HIT_ACTIVE    // legacy alias
#define HITDEF_OFF_ATK_LEVEL     HITDEF_OFF_ATTACK_LEVEL  // legacy alias
#define HITDEF_OFF_RESULT        156      // DWORD — last contact resolution code
#define HITDEF_OFF_RESULT_POS    160      // DWORD — packed contact position
#define HITDEF_OFF_BLOCKSTUN    26        // WORD  — blockstun frames
#define HITDEF_OFF_HITSTUN      28        // WORD  — hitstun frames
#define HITDEF_OFF_KNOCKBACK    30        // WORD  — knockback force (init 10000)
#define HITDEF_OFF_X            196       // int16 — world X position (×10)
#define HITDEF_OFF_Y            198       // int16 — world Y position (×10)
#define HITDEF_OFF_FACING       200       // int8  — facing direction
#define HITDEF_OFF_ANIM_OWNER   201       // byte  — player index whose anim data to use
#define HITDEF_OFF_ANIM_FRAME_IDX 228     // WORD  — animation frame index for box lookup

// Per-animation box system (within animation frame data, 104 bytes per frame).
// Frame layout (each entry = int16 xOff, yOff, halfW, halfH = 8 bytes):
//   Offset  0-7:   Collision/push box (1 entry)
//                  — Entity_ResolveBodyCollision: body push
//                  — Entity_ResolveAttackCollision: defender clash target
//   Offset  8-39:  Attack hitboxes (4 entries)
//                  — Entity_UpdateHitDetection: player melee vs summon hurtbox@40
//                  — Entity_UpdateGrabAlignment: player melee vs player hurtbox@40
//                  — Entity_UpdateDamageApplication: player melee vs player ext-hurtbox@72
//                  — Entity_UpdateSummonCollision: summon-vs-summon
//   Offset 40-71:  Hurtboxes / primary vulnerable boxes (4 entries)
//                  — Entity_UpdateHitDetection: defender/summon target
//                  — Entity_UpdateGrabAlignment: defender (player-vs-player melee)
//                  — Entity_UpdateSummonHitDetection: defender (summon vs player)
//                  Hit by ALL attack types (melee + projectiles/summons).
//   Offset 72-103: Extended hurtboxes (4 entries)
//                  — Entity_UpdateDamageApplication: defender (player-vs-player melee ONLY)
//                  — Entity_UpdateThrowInteraction: both sides (mutual overlap = tech throw)
//                  NOT hit by projectiles/summons (Entity_UpdateSummonHitDetection uses @40 only).
//                  Provides additional melee-only vulnerability beyond primary hurtbox@40.
// Screen-space: center = entityPos/10 + 2*offset*facing; extent = halfExtent.
// Verified pointer arithmetic from reverse-engineering notes:
//   animDataBase = entity + 4104; frame@N = animDataBase + 104*animIdx + N.
//   Entity_UpdateGrabAlignment: attacker hitbox@8 vs defender hurtbox@40 (0x4A4950)
//   Entity_UpdateDamageApplication: attacker hitbox@8 vs defender ext-hurtbox@72 (0x4A76F0)
//   Both dispatch to same character-specific handler table (dword_73E070).
//   NOTE: grab boxes use the SAME field layout as hit/hurt (xOff,yOff,halfW,halfH; no
//   swap). Entity_UpdateGrabAlignment ALSO connects when the attacker's
//   ATTACK_FLAG_CONTACT_OVERRIDE (0x20000 @ entity+0x6CC) is set, which bypasses the
//   box overlap entirely — so normal proximity throws (action 103) and command throws
//   connect with these grab-box slots EMPTY. Empty hit/hurt/ext slots during a throw
//   are therefore expected; the connection is flag-driven, not box-driven.
#define ANIM_COLLISION_OFFSET   0         // frame offset: collision/push box (1 entry)
#define ANIM_HITBOX_OFFSET      8         // frame offset: attack/hitbox set (4 entries)
#define ANIM_HURTBOX_OFFSET     40        // frame offset: hurtbox/vulnerable set (4 entries)
#define ANIM_EXT_HURTBOX_OFFSET 72        // frame offset: extended hurtbox set (4 entries, melee-only)
#define HURTBOX_ENTRY_SIZE      8         // 4 × int16 per box entry
#define HURTBOX_COUNT_PER_FRAME 4         // 4 box entries per set

// Entity attack/hit state offsets (relative to entity base)
// Set by Entity_SetAttackByte, Entity_InitHitData, Entity_SetCollisionData
#define ENTITY_OFF_AIRBORNE      0x06C4   // +1732, BYTE — 1 = airborne; selects the air branch in sub_4A5EF0
#define ENTITY_OFF_ATTACK_STATE  0x06C8   // +1736, BYTE — 0=inactive, 1=active attack
#define ENTITY_OFF_ATTACK_TYPE   0x06CC   // +1740, DWORD — attack type flags
#define ENTITY_OFF_HIT_ACTIVE    0x06D4   // +1748, BYTE — hit data active flag

// Attack mask bits (entity+1740 / HitDef entry+16).
// The low three bits are guard-compatibility lanes, matched against the
// defender's own lanes at +1940 by sub_4A5EF0 (bit 0 vs bit 0, bit 1 vs bit 1).
// Bit 0 is the STAND lane (overhead when alone), bit 1 the CROUCH lane (low when
// alone) - the native adaptive helper sub_4A8D00 crouches unless (mask & 3) == 1.
#define ATTACK_GUARD_STAND           0x00001  // stand-guard lane
#define ATTACK_GUARD_CROUCH          0x00002  // crouch-guard lane
#define ATTACK_GUARD_AIR             0x00004  // air-guardable
#define ATTACK_GUARD_GROUND_MASK     0x00003  // both ground lanes
#define ATTACK_FLAG_BYPASS_DEF_1932  0x00800  // bypasses the defender +1932 gate (incoming-attack property)
#define ATTACK_FLAG_HITDEF_NO_PLAYER 0x01000  // suppresses the HitDef-to-player hit path
#define ATTACK_FLAG_SPECIAL_GUARD    0x02000  // ordinary guard requires defender +1940 & 0x2000
#define ATTACK_FLAG_CONTACT_OVERRIDE 0x20000  // Bypasses box-size / overlap checks in grab, damage, hit-detection, and summon-collision paths
#define ATTACK_FLAG_BYPASS_NATIVE_DEF 0x80000 // character-specific defense handlers reject the attack

// Verified clash / max-hit block (entity+0x77C..0x79D).
// Entity_SetClashData writes the raw clash payload used by Entity_ResolveAttackCollision.
// Entity_UpdateMaxHitData writes the raw max-hit lanes consulted by clash/melee/summon logic.
// The first max-hit lane (+1932) is still used directly as a defender invulnerability gate in
// Entity_UpdateGrabAlignment / Entity_UpdateHitDetection / summon-player checks, which matches
// the old viewer's simple "entity+0x78C != 0" indicator. Move scripts also write special
// 0x10000 / 0x8000 bits into the clash continuation ID (+1928) together with the +1948 marker,
// while later damage-resolution helpers consult the same bit positions on the max-hit flags
// (+1940). Only clash rank (+1916) and continuation ID (+1928) are fully named; the raw A-D
// fields are used directly in overlap math but their higher-level gameplay labels remain
// partially unverified.
#define ENTITY_OFF_CLASH_RANK        0x077C  // +1916, BYTE  — clash rank / priority
#define ENTITY_OFF_CLASH_RAW_A       0x077E  // +1918, WORD  — raw clash field A
#define ENTITY_OFF_CLASH_RAW_B       0x0780  // +1920, WORD  — raw clash field B
#define ENTITY_OFF_CLASH_RAW_C       0x0782  // +1922, WORD  — raw clash field C
#define ENTITY_OFF_CLASH_RAW_D       0x0784  // +1924, WORD  — raw clash field D
#define ENTITY_OFF_CLASH_ID          0x0788  // +1928, DWORD — clash continuation action / ID / scripted special flag source
#define ENTITY_OFF_MAX_HIT_RAW_A     0x078C  // +1932, WORD  — max-hit raw lane A / legacy invulnerability gate
#define ENTITY_OFF_MAX_HIT_RAW_B     0x078E  // +1934, WORD  — max-hit raw lane B
#define ENTITY_OFF_MAX_HIT_RAW_C     0x0790  // +1936, WORD  — max-hit raw lane C
#define ENTITY_OFF_MAX_HIT_RAW_D     0x0792  // +1938, WORD  — max-hit raw aux value
#define ENTITY_OFF_MAX_HIT_ID        0x0794  // +1940, DWORD — max-hit raw aux ID
#define ENTITY_OFF_MAX_HIT_FLAGS     ENTITY_OFF_MAX_HIT_ID  // Preferred alias when treating +1940 as bitflags
#define ENTITY_OFF_INVINCIBILITY     ENTITY_OFF_MAX_HIT_RAW_A  // Legacy alias used by the original viewer; nonzero still blocks several defender hit checks
#define ENTITY_OFF_MAX_HIT_ACTIVE    0x0798  // +1944, DWORD — max-hit block active flag
#define ENTITY_OFF_HIT_MARKER_1948   0x079C  // +1948, BYTE  — special hit marker / timer used with 0x10000/0x8000
#define ENTITY_OFF_HIT_MARKER_1949   0x079D  // +1949, BYTE  — reaction/clash helper marker (not treated as generic invuln)

// Max-hit / collision-state flag bits (+1940).
// This block behaves like a copied defense/frame-state mask in the collision helpers:
//   0x0200 cleanly NEGATES an incoming strike: sub_4A5D30 (case 6 of the strike resolver dispatched
//          from Entity_UpdateDamageApplication) returns 8 ("no hit", skipped at 107480) when the
//          defender has it, unless the attack carries 0x80000. This is the primary per-move
//          strike-invuln flag (the move keeps its hurtboxes but strikes do not connect).
//   0x2000 is the SPECIAL-GUARD capability, not invulnerability: sub_4A5EF0
//          proceeds when the attack lacks ATTACK_FLAG_SPECIAL_GUARD **or** the
//          defender carries this bit, so it lets an otherwise unguardable attack
//          be guarded normally.
//   0x4000 suppresses the normal follow-through branch in sub_4A6030.
//   0x8000 / 0x10000 are special invuln states that still require +1948.
#define CLASH_ID_FLAG_INVINCIBLE         0x10000
#define CLASH_ID_FLAG_SPECIAL_INVULN     0x08000
#define MAX_HIT_FLAG_STRIKE_INVULN       0x00200
#define MAX_HIT_FLAG_SPECIAL_GUARD       0x02000
#define MAX_HIT_FLAG_PROJECTILE_INVULN   0x04000
#define MAX_HIT_FLAG_INVINCIBLE          0x10000
#define MAX_HIT_FLAG_SPECIAL_INVULN      0x08000

// Defender guard/defense capability bits, same DWORD (+1940). The low lanes are
// matched against the attack mask's low lanes; the higher bits gate the
// character-specific defense handlers dispatched ahead of ordinary guard.
#define DEFENSE_GUARD_STAND              0x00001  // can stand-guard
#define DEFENSE_GUARD_CROUCH             0x00002  // can crouch-guard
#define DEFENSE_GUARD_AIR                0x00004  // native air-guard capability
#define DEFENSE_GUARD_GROUND_MASK        0x00003  // both ground lanes
#define DEFENSE_CAT1_UNIQUE              0x00010  // sub_4A4E40 gate (result 3)
#define DEFENSE_CAT5_ABSOLUTE            0x00100  // sub_4A5C20 gate (result 7)
#define DEFENSE_CAT6_DODGE               0x00200  // sub_4A5D30 gate (result 8)
#define DEFENSE_GUARD_POINT              0x00400  // sub_4A5D80 gate (result 9)
#define DEFENSE_SPECIAL_GUARD            0x02000  // permits guarding ATTACK_FLAG_SPECIAL_GUARD
#define DEFENSE_JUST_PARRY               0x00020  // sub_4A50A0 capability path (result 4)
#define DEFENSE_REPEL                    0x00040  // sub_4A5570 capability path (result 5)

// How an action grants those bits: the six-argument form of the max-hit setter
// (0x49E8F0). The decomp labels several call sites Entity_SetCollisionData /
// Entity_SetClashData, but a six-argument call lands here and its LAST argument
// is the +1940 mask - which is why literals like 0x23 / 0x43 / 0x103 / 0x403
// appear at the call sites.
//
//   Entity_UpdateMaxHitData(entity, lanesA, lanesB, lanesC, aux, MASK)
//
// Actions observed granting a capability (action-ID switch, not reaction code):
#define ACTION_CAT1_UNIQUE_A     30   // 0x1E, extends via route 22 on D + FORWARD
#define ACTION_CAT1_UNIQUE_AIR   32   // 0x20, same with the airborne Y gate
#define ACTION_ABSOLUTE_DEF_GND  49   // 0x31, granted 0x103
#define ACTION_ABSOLUTE_DEF_AIR  52   // 0x34
#define ACTION_DODGE_GND_A       59   // 0x3B, granted 0x200
#define ACTION_DODGE_GND_B       60   // 0x3C, the BACK-held variant
#define ACTION_DODGE_AIR_A       61   // 0x3D
#define ACTION_DODGE_AIR_B       62   // 0x3E
#define ACTION_GUARD_POINT       135  // 0x87, granted 0x403

// Just-parry / repel are NOT actions: sub_49EED0 reaction codes 2..4 and 5..7
// are the follow-through states, and the entry is the timer path below.

// --- Native defensive-window arming --------------------------------------
// Entity_ProcessCommandMatches (0x4BEA20) ends by dispatching on the CHARACTER
// ID at *(entity+0) + 176 - not on the defence category - to the routine that
// decides whether this frame's input opens the character's defensive window:
//     0, 2, 5, 13, 18  -> Entity_CheckGuardState  (repel,      category 3)
//     3, 8             -> Entity_CheckHitState    (just parry, category 2)
//     7, 10, 14, 16    -> Entity_CheckAirTech     (push away,  category 4)
//     1                -> sub_4F2840
// Every other id reaches nothing here: dodge, absolute defence and the
// category-1 counter are guard CANCELS resolved in Entity_UpdateAction_Standard
// from live buttons, not windows opened ahead of the hit.
//
// The three window routines are the practice dummy's seam. Each opens a state
// that must ALREADY be open when the hit lands, so a dummy that waits to see a
// hitbox can never open it in time whatever it presses. Hooking the routine and
// arming through the engine's own setters puts the dummy in exactly the state a
// perfectly timed human input would have produced.
#define ADDR_ENTITY_CHECK_HIT_STATE   (GAME_BASE + 0x024AF0)  // just-parry arm
#define ADDR_ENTITY_CHECK_GUARD_STATE (GAME_BASE + 0x024B80)  // repel arm
#define ADDR_ENTITY_CHECK_AIR_TECH    (GAME_BASE + 0x024C90)  // push-away arm
#define ADDR_ENTITY_SET_HIT_STATE_1965 (GAME_BASE + 0x09EA10) // opens the parry window
#define ADDR_ENTITY_SET_HIT_REACTION   (GAME_BASE + 0x09EA50) // opens the repel window
#define ADDR_ENTITY_UPDATE_HIT_REACTION (GAME_BASE + 0x09EB50)// upgrades +1989 once
#define ADDR_ENTITY_SET_HIT_FLAG_1993  (GAME_BASE + 0x09EBC0) // arms push-away
#define ADDR_ENTITY_SET_HIT_BYTE_1949  (GAME_BASE + 0x09E990) // action permits defence
#define ADDR_ENTITY_SET_HIT_STATE_1973 (GAME_BASE + 0x09EA30) // records DOWN / BACK held
#define ADDR_ENTITY_SET_HIT_FLAGS_1996 (GAME_BASE + 0x09EBE0) // push-away DOWN / BACK held

// The guard-cancel offer (sub_424910). Reached through the route-23 slot at
// +1675, which the blockstun handlers open themselves, so it runs while the
// defender is in one of the six blockstun actions. It writes the action it
// wants into the caller's pending-action triple:
//   category 5 + (+823 > 0)         -> action 49 (52 airborne, needs y < 6960)
//                                      and consumes command 26, the 214D
//   category 6 + 500 meter + D EDGE -> action 60 with BACK held, 59 without
// Everything else falls through. Action 49's own handler (sub_430af0) then sets
// +1940 = 0x103 for its first 57 frames and 0 after, which IS the counter-guard
// window; sub_4A5C20 fires inside it and hands off to sub_49EED0 code 11/12,
// whose handlers enter actions 50->51 / 53->54 - also 0x103, so the counter
// guard chains through follow-up hits.
#define ADDR_ENTITY_UPDATE_ACTION_ATTACKS (GAME_BASE + 0x024910)


// Route slot 0 is the counter-guard command for every category-5 character:
// all six dispatchers send case 0 to sub_522F30 or sub_5D99C0, and both queue
// action 49 and consume command 26 once the +823 charge is there. The route
// byte at +1652 is what Entity_ProcessCommandMatches tests to decide the
// command matched, so writing it IS "the 214D was input" - the same kind of
// state write that opens the parry window, rather than a simulated motion.
// It is also the LOWEST priority slot: the loop walks 23 down to 0 and exits on
// the first match, so a real special always wins over it.
#define ENTITY_ROUTE_COUNTER_GUARD 0
#define ACTION_ABSOLUTE_DEFENCE_GROUND 49
#define ACTION_ABSOLUTE_DEFENCE_AIR    52

// --- Just-parry window (defender-relative) --------------------------------
// Entity_CheckHitState arms the window when BACK is FRESHLY pressed (+86 == 1)
// and +1965 reads idle (0xFF). +1965 takes a KIND, not a frame count:
//     blockstun actions 64/65/67/68/70/71 -> kind 5, class 3
//     actions 34..39                      -> kind 6, class 2
//     anything else                       -> kind 7, class 1
// and +1972 is set to 11 alongside. sub_4A50A0 then needs a live window plus
// +1949 == 1, with +1974 (BACK held) and +1973 (DOWN held) selecting the lane.
// A continuous hold never arms it - the fresh edge is the whole mechanic.
// The shared prerequisite for EVERY category's path-B resolution: the action
// the defender is in must permit its defensive mechanic. Action handlers call
// Entity_SetHitByte_1949(entity, 1) after Entity_ResetHitData; nothing in the
// game ever writes 0, so an action that simply does not set it leaves the whole
// mechanic switched off however well the window is armed. This is what made a
// perfectly armed repel window resolve as a plain hit.
#define ENTITY_OFF_DEFENCE_ALLOWED   0x079D  // +1949, BYTE, 1 = action permits it
#define ENTITY_OFF_PARRY_FIRED       0x07AC  // +1964, BYTE, set by the parry resolver
#define ENTITY_OFF_PARRY_WINDOW      0x07AD  // +1965, BYTE, 0xFF = idle, else kind 5/6/7
#define ENTITY_OFF_PARRY_KIND        0x07B0  // +1968, DWORD, 1/2/3 by originating state
#define ENTITY_OFF_PARRY_TIMER       0x07B4  // +1972, BYTE, set to 11 when the window opens
#define ENTITY_OFF_PARRY_STANCE      0x07B5  // +1973, BYTE, 1 = DOWN held (crouch parry)
#define ENTITY_OFF_PARRY_GUARD_HELD  0x07B6  // +1974, BYTE, 1 = BACK held
#define PARRY_WINDOW_IDLE            0xFF
#define PARRY_KIND_BLOCKSTUN         5
#define PARRY_KIND_CROUCH            6
#define PARRY_KIND_STAND             7

// --- Repel window (defender-relative) -------------------------------------
// Entity_CheckGuardState arms it only from a tap taken out of NEUTRAL: the
// PREVIOUS neutral word (+60) must read 1, and then FORWARD just-pressed (+84)
// with no UP/DOWN edge gives reaction 5 (7 airborne), or DOWN just-pressed
// (+66) with no LEFT/RIGHT edge gives reaction 6 - the low arm. Entity_
// SetHitReaction writes the reaction at +1980 and a 24-frame window at +1990;
// Entity_UpdateHitReaction then upgrades +1989 once, while the defender is
// standing neutral, from the attacker's airborne flag.
#define ENTITY_OFF_HIT_REACTION_STATE 0x07BC // +1980, DWORD, -1 = no reaction armed
#define ENTITY_OFF_HIT_REACTION_CLASS 0x07C0 // +1984, DWORD, 1 stand / 2 crouch / 3 air
#define ENTITY_OFF_HIT_REACTION_LATCH 0x07C4 // +1988, BYTE, 1 once +1989 was upgraded
#define ENTITY_OFF_HIT_REACTION_KIND 0x07C5  // +1989, BYTE, repel's non-capability path
#define ENTITY_OFF_HIT_REACTION_TIMER 0x07C6 // +1990, BYTE, 24-frame repel window
#define REPEL_REACTION_HIGH          5       // forward tap, grounded
#define REPEL_REACTION_LOW           6       // down tap
#define REPEL_REACTION_AIR           7       // forward tap, airborne
#define REPEL_REACTION_IDLE          (-1)

// --- Push-away arming (defender-relative) ---------------------------------
// Entity_CheckAirTech needs BACK held (+30) and D held (+22). With D FRESHLY
// pressed (+78), +1995 idle and the action in the free list it arms the free
// variant; otherwise meter > 99 arms the paid one. Entity_SetHitFlag_1993
// writes +1993 = 1, +1994 = which variant, +1995 = 6.
#define ENTITY_OFF_PUSH_AWAY_ARMED   0x07C9  // +1993, BYTE, 1 = armed this window
#define ENTITY_OFF_PUSH_AWAY_FREE    0x07CA  // +1994, BYTE, 1 = free variant
#define ENTITY_OFF_PUSH_AWAY_TIMER   0x07CB  // +1995, BYTE, 0xFF = idle, else 6
#define ENTITY_OFF_PUSH_AWAY_DOWN    0x07CC  // +1996, BYTE, 1 = DOWN held
#define ENTITY_OFF_PUSH_AWAY_BACK    0x07CD  // +1997, BYTE, 1 = BACK held
#define PUSH_AWAY_TIMER_IDLE         0xFF

// --- Per-entity input words, slots 10..13 --------------------------------
// Input_ProcessRawInput derives four extra slots after the ten buttons:
//     10 FORWARD, 11 BACK (both facing-relative), 12 NEUTRAL, 13 B+C
// so BACK is +8 + 2*11 = +30 current, +36 + 2*11 = +58 previous,
// +64 + 2*11 = +86 derived/just-pressed.
#define ENTITY_INPUT_IDX_FORWARD     10
#define ENTITY_INPUT_IDX_BACK        11
#define ENTITY_INPUT_IDX_NEUTRAL     12
#define ENTITY_INPUT_IDX_BC          13
#define ENTITY_OFF_INPUT_BACK        0x001E  // +30, current
#define ENTITY_OFF_INPUT_BACK_EDGE   0x0056  // +86, just-pressed

// Guard-cancel options out of blockstun, from Entity_UpdateAction_Standard
// (route 22) keyed on the current blockstun action:
//   category 5 + resource byte +823  -> action 49 / 52 (Absolute Defense)
//   category 6 + meter >= 500 + D    -> action 59/60 / 61/62 (Dodge)
#define ENTITY_OFF_DEFENSE_RESOURCE  0x0337  // +823, BYTE, category-5 stock
#define DODGE_METER_COST             500
#define PUSH_AWAY_METER_MIN          100     // sub_4A5910 gate, category 4
#define ADDR_DEFENSE_REACTION_DISPATCH (GAME_BASE + 0x09EED0) // sub_49EED0

// --- Contact-time defence resolvers, one per category ---------------------
// The collision pass switches on dword_73E070[defender char id] and calls one
// of these BEFORE guard point and ordinary guard. Returning 1 declines, and the
// contact falls through to the ordinary guard and then the normal-hit handler.
// Each takes (attackerCtx, sourceObject, contactPos, attackerFacing, payload)
// with the defender reached through attackerCtx+4 - except the dodge, which
// takes two arguments.
#define ADDR_DEFENCE_RESOLVE_COUNTER   (GAME_BASE + 0x0A4E40)  // sub_4A4E40, category 1
#define ADDR_DEFENCE_RESOLVE_PARRY     (GAME_BASE + 0x0A50A0)  // sub_4A50A0, category 2
#define ADDR_DEFENCE_RESOLVE_REPEL     (GAME_BASE + 0x0A5570)  // sub_4A5570, category 3
#define ADDR_DEFENCE_RESOLVE_PUSH_AWAY (GAME_BASE + 0x0A5910)  // sub_4A5910, category 4
#define ADDR_DEFENCE_RESOLVE_ABSOLUTE  (GAME_BASE + 0x0A5C20)  // sub_4A5C20, category 5
#define ADDR_DEFENCE_RESOLVE_DODGE     (GAME_BASE + 0x0A5D30)  // sub_4A5D30, category 6

// Defender capability byte +1940, as the resolvers read it. Each category has a
// combination that lets it fire from the guard flags alone, bypassing the
// window path entirely.
#define DEFENDER_FLAGS_PARRY_DIRECT  0x23
#define DEFENDER_FLAGS_REPEL_DIRECT  0x43
// Categories 1, 5 and 6 have no window at all - their whole gate is one bit of
// +1940, written wholesale from the action's own flags by Entity_UpdateMaxHitData.
#define DEFENDER_FLAG_GUARD_COUNTER    0x0010
#define DEFENDER_FLAG_ABSOLUTE_DEFENCE 0x0100
#define DEFENDER_FLAG_DODGE            0x0200

// Contact resolution codes. Every handler except ordinary guard also stores its
// code at defender+1944; ordinary guard (10) only ever appears as the resolver
// return value, which the caller stores at attacker+1880 (direct/grab) or
// HitDef entry+156 (summon). Do not test +1944 for 10.
#define CONTACT_RESULT_NONE              1
#define CONTACT_RESULT_UNIQUE_DEFENSE    3
#define CONTACT_RESULT_JUST_PARRY        4
#define CONTACT_RESULT_REPEL             5
#define CONTACT_RESULT_PUSH_AWAY         6
#define CONTACT_RESULT_ABSOLUTE_DEFENSE  7
#define CONTACT_RESULT_DODGE             8
#define CONTACT_RESULT_GUARD_POINT       9
#define CONTACT_RESULT_GUARD             10
#define CONTACT_RESULT_HIT               11
#define ENTITY_OFF_CONTACT_RESULT        0x0758   // +1880, DWORD - last contact result (attacker side)

// Active rect / pushbox (entity-relative single rects).
// Managed by Input_SetNextRect / Input_ApplyNextRect.
#define ENTITY_OFF_PENDING_RECT 0x0690    // +1680: pending rect (x,y,w,h,unk,type) 12 bytes
#define ENTITY_OFF_ACTIVE_RECT  0x06A0    // +1696: active rect  (x,y,w,h,unk,type) 12 bytes
#define ENTITY_OFF_PENDING2_RECT 0x06B0   // +1712: pending rect 2
#define ENTITY_OFF_GLOBAL_X     0x06C0    // +1728: global X (int16)
#define ENTITY_OFF_GLOBAL_Y     0x06C2    // +1730: global Y (int16)
#define ACTIVE_RECT_TYPE_INACTIVE 12      // type value meaning rect is inactive

// Key effect functions
#define ADDR_EFFECT_SPAWN       (GAME_BASE + 0x0A92C0)  // sub_4A92C0 - Spawn effect
#define ADDR_EFFECT_CLEAR       (GAME_BASE + 0x0A92A0)  // sub_4A92A0 - Clear all effects
#define ADDR_EFFECT_UPDATE      (GAME_BASE + 0x0A9330)  // sub_4A9330 - Update all effects
#define ADDR_EFFECT_DRAW        (GAME_BASE + 0x0AB0F0)  // sub_4AB0F0 - Draw global effect queue
#define ADDR_SUMMON_SPAWN       (GAME_BASE + 0x0BE100)  // sub_4BE100 - Spawn summon
#define ADDR_SUMMON_UPDATE      (GAME_BASE + 0x0BE3E0)  // sub_4BE3E0 - Update summons

#define ADDR_ENTITY_UPDATE_COMBO_STATS     (GAME_BASE + 0x09EC00)  // sub_49EC00 - Entity_UpdateComboStats
#define ADDR_ENTITY_UPDATE_COMBO_STAT_1243 (GAME_BASE + 0x09EC90)  // sub_49EC90 - Entity_UpdateComboStat_1243
#define ADDR_ENTITY_EFFECT_ARRAY_INIT      (GAME_BASE + 0x0C3E90)  // sub_4C3E90 - EffectArray_Init
#define ADDR_ENTITY_EFFECT_SET_PARAMS1     (GAME_BASE + 0x0C3ED0)  // sub_4C3ED0 - Effect_SetParams1
#define ADDR_ENTITY_EFFECT_SET_PARAMS2     (GAME_BASE + 0x0C3F30)  // sub_4C3F30 - Effect_SetParams2
#define ADDR_ENTITY_EFFECT_SLOTS_ADD       (GAME_BASE + 0x0C3FB0)  // sub_4C3FB0 - EffectSlots_Add

// Collision / defense resolution chain. Each phase below scans for contacts and
// then dispatches, in order: character-specific defense (dword_73E070 category)
// -> generic guard point -> ordinary guard -> normal hit. All four resolvers
// share the signature (attackerCtx, sourceObject, contactPos, facing, payload)
// where *(attackerCtx + ENTITY_OFF_OPPONENT) is the DEFENDER entity and
// sourceObject is 0 for direct/grab contacts or the HitDef entry base.
#define ADDR_COLLISION_GRAB_PHASE   (GAME_BASE + 0x0A4950)  // Entity_UpdateGrabAlignment
#define ADDR_COLLISION_DAMAGE_PHASE (GAME_BASE + 0x0A76F0)  // Entity_UpdateDamageApplication
#define ADDR_COLLISION_SUMMON_PHASE (GAME_BASE + 0x0A8390)  // Entity_UpdateSummonHitDetection
#define ADDR_DEFENSE_CAT1_UNIQUE    (GAME_BASE + 0x0A4E40)  // result 3
#define ADDR_DEFENSE_CAT2_PARRY     (GAME_BASE + 0x0A50A0)  // result 4
#define ADDR_DEFENSE_CAT3_REPEL     (GAME_BASE + 0x0A5570)  // result 5
#define ADDR_DEFENSE_CAT4_PUSHAWAY  (GAME_BASE + 0x0A5910)  // result 6
#define ADDR_DEFENSE_CAT5_ABSOLUTE  (GAME_BASE + 0x0A5C20)  // result 7
#define ADDR_DEFENSE_CAT6_DODGE     (GAME_BASE + 0x0A5D30)  // result 8
#define ADDR_DEFENSE_GUARD_POINT    (GAME_BASE + 0x0A5D80)  // result 9
#define ADDR_DEFENSE_ORDINARY_GUARD (GAME_BASE + 0x0A5EF0)  // result 10 (auto-block hook)
#define ADDR_DEFENSE_NORMAL_HIT     (GAME_BASE + 0x0A6710)  // result 11
#define ADDR_DEFENSE_CATEGORY_TABLE 0x73E070                // dword_73E070[charId]
#define ADDR_NATIVE_THREAT_MASK     (GAME_BASE + 0x0A8C20)  // sub_4A8C20 - AI threat-mask lookup
#define ADDR_NATIVE_ADAPTIVE_GUARD  (GAME_BASE + 0x0A8D00)  // sub_4A8D00 - native stand/crouch helper

// Effect types (common ones from switch in sub_4A9330)
#define EFFECT_TYPE_STANDARD_30F    0x01  // 30 frame lifetime
#define EFFECT_TYPE_STANDARD_45F    0x02  // 45 frame lifetime
#define EFFECT_TYPE_STANDARD_20F    0x03  // 20 frame lifetime
#define EFFECT_TYPE_RANDOM_VEL      0x06  // Random velocity
#define EFFECT_TYPE_LARGE_SPREAD    0x0B  // Large random spread
#define EFFECT_TYPE_PROJECTILE_BASE 0x63  // Projectile types start
#define EFFECT_TYPE_SUPER_BASE      0xC1  // Super effects start

// ============================================================================
// Combo Counter System Addresses
// ============================================================================

#define ENTITY_OFF_DISPLAY_COMBO_COUNT 0x00D0 // +208, live per-entity combo/hit counter
#define ENTITY_OFF_GUARD_GAUGE  0x00B2    // +178, word - guard gauge fill (reset=10000; decremented on guard damage)
#define ENTITY_OFF_HP_DISPLAY_PREVIOUS 0x00D2 // +210, word - HP bar trailing value
#define ENTITY_OFF_HP_DISPLAY   0x00D4    // +212, word - HP bar visible value
#define ENTITY_OFF_GAME_STATE   42952     // 0xA7D8 - Game state (11 = active match)

// Character structure size (P1 base to P2 base offset)
#define CHARACTER_STRUCT_SIZE   108812    // 0x1A90C bytes per character

// ============================================================================
// Entity Structure Offsets
// ============================================================================

// Core entity offsets (relative to entity base)
#define ENTITY_OFF_HP           0x00B0  // +176, 2 bytes
#define ENTITY_OFF_METER        0x00B4  // +180, 2 bytes
#define ENTITY_OFF_X_POS        0x00B8  // +184, 2 bytes
#define ENTITY_OFF_Y_POS        0x00BA  // +186, 2 bytes
#define ENTITY_OFF_PUSH_DIR     0x00BC  // +188, 1 byte (collision push direction)
#define ENTITY_OFF_FACING       0x00BD  // +189, 1 byte
#define ENTITY_OFF_CHAR_ID      0x00B0  // +176, character ID (from word_776718 indexing)
#define ENTITY_OFF_ACTION_ID    0x044C  // +1100, DWORD — action handler dispatch ID (changes on walk/crouch/jump/attack/hitstun; see ENTITY_INPUT_START note)
#define ENTITY_OFF_ANIMATION    0x0470  // +1136, 2 bytes
#define ENTITY_OFF_OPPONENT     0x0004  // +4, pointer to opponent entity

// State blocks
#define ENTITY_OFF_STATE_A      0x0690  // +1680, set by sub_4BF630
#define ENTITY_OFF_STATE_B      0x06CC  // +1740, cleared by sub_49E720
#define ENTITY_OFF_COMBO        ENTITY_OFF_MAX_HIT_RAW_A  // Legacy alias retained for compatibility; this is not the HUD combo count
#define ENTITY_OFF_FLAG_CE      0x00B6  // +182, flag used in AI calculations
#define ENTITY_OFF_HITSTUN      0x1A7F0 // +108528, hitstun array (sub_4C1F60)

// Physics / velocity (suspected from entity core range 0xBE-0xD0)
#define ENTITY_OFF_X_VEL        0x00BE  // +190, int16 — X velocity/speed component
#define ENTITY_OFF_Y_VEL        0x00C0  // +192, int16 — Y velocity/speed component
#define ENTITY_OFF_X_ACCEL      0x00C2  // +194, int16 — X acceleration
#define ENTITY_OFF_Y_ACCEL      0x00C4  // +196, int16 — Y acceleration (gravity)
#define ENTITY_OFF_CORE_C6      0x00C6  // +198, int16 — unknown core field
#define ENTITY_OFF_CORE_C8      0x00C8  // +200, int16 — unknown core field
#define ENTITY_OFF_CORE_CA      0x00CA  // +202, int16 — unknown core field
#define ENTITY_OFF_CORE_CC      0x00CC  // +204, int16 — unknown core field
#define ENTITY_OFF_CORE_CE      0x00CE  // +206, int16 — unknown core field

// Action state buffer offsets (relative to entity base)
#define ENTITY_OFF_ACTION_PHASE 0x0498  // +1176, current phase within action (WORD)
#define ENTITY_OFF_ACTION_FRAME 0x04A4  // +1188, current frame within action phase (WORD)
#define ENTITY_OFF_ACTION_PRIORITY 0x04DC // +1244, attack priority (DWORD)

// Combat hitstun/blockstun (within state-B / combat block)
#define ENTITY_OFF_CHIP_DAMAGE  0x06D8  // +1752, WORD — chip/block damage
#define ENTITY_OFF_KNOCKBACK_FORCE 0x06DA // +1754, WORD — knockback force init (10000)
#define ENTITY_OFF_KNOCKBACK_TIMER 0x06DC // +1756, WORD — knockback timer (20 * hitstun)
#define ENTITY_OFF_HIT_TIMER_BASE 0x06DE // +1758, WORD — hit timer base (init=20)
#define ENTITY_OFF_HITSTUN_DURATION 0x06E0 // +1760, BYTE — hitstun duration
#define ENTITY_OFF_HIT_RECOVERY 0x06E1  // +1761, BYTE — hit recovery rate
#define ENTITY_OFF_HITSTUN_PRIMARY 0x06E2 // +1762, BYTE — primary hitstun value
#define ENTITY_OFF_CURRENT_DAMAGE 0x06E4 // +1764, WORD — current damage copy
#define ENTITY_OFF_HIT_EFFECT   0x06E8  // +1768, DWORD — hit effect flag (init=8)
#define ENTITY_OFF_BLOCKSTUN    0x06FC  // +1788, BYTE — blockstun counter
#define ENTITY_OFF_BLOCKSTUN2   0x06FD  // +1789, BYTE — blockstun copy
#define ENTITY_OFF_HIT_REACTION 0x070C  // +1804, BYTE — extended hit reaction time

// Combo scaling (within action block)
#define ENTITY_OFF_COMBO_SCALE1 0x04D5  // +1237, BYTE — combo multiplier 1
#define ENTITY_OFF_COMBO_SCALE2 0x04D7  // +1239, BYTE — combo multiplier 2
#define ENTITY_OFF_COMBO_SCALE3 0x04D9  // +1241, BYTE — combo multiplier 3
#define ENTITY_OFF_COMBO_SCALE4 0x04DB  // +1243, BYTE — combo special stat

// Combo / hit-reaction presentation fields. These are deterministic entity-local
// state and should be snapshotted/logged, not cleared as baseline residue.
#define ENTITY_OFF_HIT_REACTION_RESET_FLAG 0x07B8
#define ENTITY_OFF_HIT_REACTION_TYPE       0x07BC
#define ENTITY_OFF_HIT_REACTION_CLASS      0x07C0
#define ENTITY_OFF_HIT_REACTION_SHOWN_FLAG 0x07C4
#define ENTITY_OFF_HIT_REACTION_ANIM_TIMER 0x07C5
#define ENTITY_OFF_HIT_REACTION_LIFE_TIMER 0x07C6
#define ENTITY_OFF_HIT_REACTION_KEEP_FLAG  0x07C7
// F7h. Originally sized empirically: a static divergence appeared at
// p1_entity+0x1A650..+0x1A68F the moment P2 hit 50 hp (KO announcer voice),
// and a 64-byte window covered it. The STRUCTURE is now known, and 64 bytes
// was far too small — it covered only the first 14 entries of a table of up
// to 99.
//
//   +0x1A64C/0x1A650/0x1A654  Entity_UpdateAudio's per-entity audio state
//                             (decomp:114100-114123)
//   +0x1A658 .. +0x1A658+4*N  the entity's VOICE HANDLE TABLE, filled by
//                             sub_54AA00 at MODE_MATCH substate 0
//                             (decomp:117591-117610), N = dword_73DC9C[charId]
//
// N ranges 49..99 across the 22 characters (decomp:14586), so the table can
// reach +0x1A658 + 396 = +0x1A7E4. Every entry is a handle carrying the
// process-global allocation serial `dword_9D0454++` (decomp:321907-321935) —
// no reset site anywhere — so two peers whose processes have loaded a
// different NUMBER of sounds hold different VALUES for the identical voice.
// Same defect class as the F9 announcer-handle mask; invisible to
// same-machine testing, where both instances share an allocation history.
//
// Masking the whole table is safe: a decomp-wide scan for any reader or
// writer of entity+[0x1A658,0x1A7E4) — match+[149192,149588) P1 and
// +[258004,258400) P2, by match-relative, dword-scaled and absolute forms —
// returns ONLY the filler above. The simulation never reads a handle value;
// it asks Audio_IsPlaying, which rollback_audio answers from the canonical
// voice model (these handles are class 0x10000000, so they are in scope).
// Table end 0x1A7E4 sits 296 B inside ENTITY_SIZE 0x1A90C.
#define ENTITY_VOICE_TAIL_MASK_OFF   0x1A650
#define ENTITY_VOICE_TAIL_MASK_SIZE  0x194   // 0x1A7E4 - 0x1A650

// F7e: the four hit-reaction DISPLAY bytes above are HUD/render-cadence
// bookkeeping (combo-pop animation; the mod's rollback_combo_fx owns their
// rollback correctness and netplay_hud_vanilla smooths +0x7C5 around
// render). The fine-diag ring caught them as transient per-side hash noise
// under real combat — digest-masked (captured/restored unchanged).
#define ENTITY_HIT_REACTION_DISPLAY_MASK_OFF  0x07C4
#define ENTITY_HIT_REACTION_DISPLAY_MASK_SIZE 4
#define ENTITY_OFF_ATTACHED_FX_SLOTS       0x07F0
#define ENTITY_ATTACHED_FX_SLOTS_SIZE      0x0028
#define ENTITY_RENDER_OVERLAY_FIELDS_SIZE  0x0019

// Command-route timer vector, 24 bytes. NOT a flag block, and +0x0676 is NOT a
// scalar "actionable" bit - it is simply route 2.
//
// Entity_ProcessCommandMatches scans route 23 down to route 0 and, for every
// nonzero route, calls the character's dispatcher for that route. The dispatcher
// may queue an action; the scan stops as soon as one does. So a nonzero route
// means "the engine will consider this input branch on this tick".
//
// Action scripts grant routes through three writers:
//   Input_UpdateMinValues_Group1 -> routes 0, 1, 15, 16, 22 (and clears 23)
//   Input_UpdateMinValues_Group2 -> routes 2..14
//   Input_UpdateMinValues_Group3 -> routes 17..21
// Each raises a route timer to at least the supplied value, so a route byte
// mixes state permission with short input retention. Input_Clear zeroes all 24.
#define ENTITY_OFF_COMMAND_ROUTE_TIMERS 0x0674  // +1652 .. +1675
#define ENTITY_COMMAND_ROUTE_COUNT      24

// Route map, verified identical across all 22 character dispatchers for the
// normals: routes 2/3/4 and 5/6/7 both run the SHARED A/B/C handlers, and they
// split by posture, not by ground/air:
//   route 2/3/4 -> sub_424110 / sub_424180 / sub_4241F0
//                  airborne (+0x06C4 == 1) -> air normals   92/93/94-95
//                  grounded, DOWN released -> stand normals 85/86/87-88
//   route 5/6/7 -> Entity_TryAction_Light / _Medium / _Heavy
//                  grounded, DOWN held     -> crouch normals 89/90/91
#define ENTITY_ROUTE_A_STAND_OR_AIR  2
#define ENTITY_ROUTE_B_STAND_OR_AIR  3
#define ENTITY_ROUTE_C_STAND_OR_AIR  4
#define ENTITY_ROUTE_A_CROUCH        5
#define ENTITY_ROUTE_B_CROUCH        6
#define ENTITY_ROUTE_C_CROUCH        7
#define ENTITY_ROUTE_COMMAND_1       12
#define ENTITY_ROUTE_SUPER_1         20
#define ENTITY_ROUTE_SUPER_2         21
#define ENTITY_ROUTE_STATE_STANDARD  22   // Entity_UpdateAction_Standard
#define ENTITY_ROUTE_STATE_ATTACKS   23   // Entity_UpdateAction_Attacks

// Pending-action slots. Entity_SetReactionState_Slot1/2 write one and clear the
// other; neither replaces the current action, which the transition pass applies
// later in the same tick. That ordering is why an action-ID edge is a late
// recovery timestamp.
#define ENTITY_OFF_PENDING_ACTION_1  0x0444  // +1092, DWORD
#define ENTITY_OFF_PENDING_ACTION_2  0x0448  // +1096, DWORD
#define ENTITY_OFF_ACTION_STATE_CLASS 0x0454 // +1108, DWORD - class of the action being left

// Terminal neutral handoff targets.
#define ACTION_TARGET_STAND_NEUTRAL  2
#define ACTION_TARGET_CROUCH_NEUTRAL 7
#define ACTION_TARGET_AIR_NEUTRAL    22
#define ACTION_TARGET_LANDING        23

// Per-entity input words. Input_SetButtonState(entity, idx) writes
// *(WORD*)(entity + 8 + 2*idx) and mirrors idx 2<->3 when facing left, so 2/3
// are LEFT/RIGHT. Three parallel blocks of ten words:
//   +0x0008 current, +0x0024 previous, +0x0040 derived (just-pressed/held)
// The route handlers read the DERIVED block for buttons (A at +0x0048 = +72)
// and the CURRENT block for directions (DOWN at +0x000A = +10).
#define ENTITY_OFF_INPUT_CURRENT     0x0008
#define ENTITY_OFF_INPUT_PREVIOUS    0x0024
#define ENTITY_OFF_INPUT_DERIVED     0x0040
#define ENTITY_INPUT_WORD_COUNT      10
#define ENTITY_INPUT_IDX_UP          0
#define ENTITY_INPUT_IDX_DOWN        1
#define ENTITY_INPUT_IDX_LEFT        2
#define ENTITY_INPUT_IDX_RIGHT       3
#define ENTITY_INPUT_IDX_A           4
#define ENTITY_INPUT_IDX_B           5
#define ENTITY_INPUT_IDX_C           6
#define ENTITY_INPUT_IDX_D           7
// +0x000A is the DOWN input word, not a ground/air lane: routes 5/6/7 require
// it set (crouching normals), the grounded path of routes 2/3/4 requires it
// clear (standing normals). Ground vs air is ENTITY_OFF_AIRBORNE.
#define ENTITY_OFF_INPUT_DOWN        0x000A

// Native Training Life/Spirit automatic-restoration gate, written by
// sub_49DA10 and read by Match_UpdateTrainingModeSettings (0x4C8120) alongside
// "both fighters are in action 2". It is NOT an actionability flag: action 0x81
// sets it at startup while zeroing every route for the body of the move.
#define ENTITY_OFF_TRAINING_RESTORE_GATE 0x00CF  // +207

#define ADDR_ENTITY_PROCESS_COMMAND_MATCHES (GAME_BASE + 0x0BEA20)
#define ADDR_MATCH_UPDATE_TRAINING_SETTINGS (GAME_BASE + 0x0C8120)
#define ADDR_INPUT_SET_FLAGS                (GAME_BASE + 0x0BF820)  // writes +1732..+1734
#define ENTITY_OFF_RENDER_FLASH_FLAG     0x01B4  // +436, BYTE, extra flash/afterimage draw flag
#define ENTITY_OFF_RENDER_TINT_STATE     0x01B8  // +440, DWORD, 1 disables extra tint pass in sub_4C6B60
#define ENTITY_OFF_RENDER_TINT_TIMER     0x01BC  // +444, DWORD
// F7e (2026-08-17 combat-load run 19-17-3x): the F5 digest mask is widened to
// start at the flash flag — +0x1B4..+0x1BF (flash byte + pad + tint dwords).
// The fine-diag ring caught transient per-side divergence in the 64B window
// covering +0x1B4 the moment real combat inputs started flowing; the flash
// flag is render-phase draw bookkeeping (render_guard/practice both treat it
// as neutral-render state).
// F7g follow-up (run 19-35-2x f3959, single-flicker at f3901): the only
// unexplained hashed divergence was p1_entity+0x1C4..+0x1CF — the tail of
// the same render flash/tint bookkeeping struct. Mask extended to
// +0x1B4..+0x1CF (28 B).
#define ENTITY_RENDER_FLASH_TINT_MASK_OFF  0x01B4
#define ENTITY_RENDER_FLASH_TINT_MASK_SIZE 0x1C
// F7f (2026-08-17 combat run 19-25-3x, byte-exact via FINEENT): three more
// per-PASS-cadence entity counters — +0x1A4 (render anim/overlay timer
// neighborhood) and +0x7F0/+0x7F8 (timer-block render effect counters).
// Their cross-side offset tracked the sides' pass-count delta exactly
// (A sim=435/B=436 -> counters exactly +1 apart, both frames), i.e. they
// increment once per outer pass (render phase) and pre-tick hashing
// samples them at sim cadence — the F7d class inside the entities.
// Run 19-47-2x f1469 byte evidence: the block is the hit-popup DISPLAY
// context (decomp hit-processing writes +0x1A4(b)/+0x1A5(b)/+0x1A6(w)/
// +0x1A8(w); HUD expiry FF-fills all six bytes at its own cadence — B
// showed ff-fill one frame before A). Mask widened to 8 bytes.
// 2026-08-18 NARROWING (same class as the F2/F7g and F7h fixes): the round-up
// to 8 swallowed entity+0x1AA/+0x1AB, which are SIMULATION state, not popup
// display bytes.
//
// The render-owned popup record is exactly SIX bytes, and the decomp gives the
// layout on both sides:
//   SIM writes it once per hit (decomp:106761-106765)
//       *(_BYTE *)(a1 + 420) = 0;      // +0x1A4 expiry clock
//       *(_BYTE *)(a1 + 421) = v15;    // +0x1A5 combo hit count
//       *(_WORD *)(a1 + 422) = v22;    // +0x1A6
//       *(_WORD *)(a1 + 424) = v21;    // +0x1A8
//   RENDER retires it by FF-filling those SAME six (sub_4C1F90,
//   decomp:113443-113448 P1 / 113550-113556 P2)
//       *(_BYTE *)(a1 + 41492) = -1;  *(_BYTE *)(a1 + 41493) = -1;
//       *(_WORD *)(a1 + 41494) = -1;  *(_WORD *)(a1 + 41496) = -1;
//   41492..41497 == entity+420..425. Nothing renders +426/+427.
//
// +0x1AA MUST BE HASHED -- it is the defensive-state flag with sim readers:
//   decomp:107062  if (*(_BYTE *)(v3 + 426) == 1) LOWORD(v12) >>= 1;  // halves
//                  the damage subtracted from HP at decomp:107069
//   decomp:105871  if (*(_BYTE *)(a1 + 426) == 1) { hitstun /= 2;
//                  knockback = 3*x/4; }
// Masking it would hide a real desync in damage and hitstun -- "digest-masked"
// is strictly weaker than "sim-free". Narrowed to 6.
#define ENTITY_RENDER_ANIM_TIMER_MASK_OFF   0x01A4
#define ENTITY_RENDER_ANIM_TIMER_MASK_SIZE  6

// The hit/combo popup DISPLAY record -- ONE ATOMIC SIX-BYTE UNIT. The sim
// writes all four fields exactly once per hit and READS NONE of them; render
// owns the lifetime (increments +0x1A4 per drawn frame, FF-fills all six at
// 90). GameSnapshot_Restore must therefore hold back the WHOLE record or none
// of it: holding back only the clock leaves a live counter pointing at an
// FF-filled combo field, which is what draws "55 HIT" with a garbage damage
// number after a rolled-back-past hit.
#define ENTITY_POPUP_DISPLAY_BLOCK_OFF      0x01A4
#define ENTITY_POPUP_DISPLAY_BLOCK_SIZE     6

// HOW MUCH OF THAT RECORD THE RESTORE MAY HOLD BACK. Same six bytes, and the
// question of whether it may be six was settled from the decomp rather than
// assumed -- twice, because the first answer was wrong.
//
// A loose grep for "+ 424)" appears to show ~15 readers of +0x1A8 in the
// sub_6Fxxxx cluster. They are FALSE POSITIVES of two kinds:
//   1. `*(_WORD *)(a2 + 236) + 424` is sprite-ID arithmetic -- it reads a word
//      at +236 and ADDS 424. Not a dereference of +424 at all.
//   2. The cluster that really does dereference +424 is a different struct.
//      Its largest offset anywhere is 424, so the whole object is ~428 bytes,
//      against ENTITY_SIZE 0x1A90C (108812) with live fields at +1200, +1244
//      and +0x1A650. It reads a DWORD there where the entity holds a WORD. And
//      decisively, sub_6F49F0 calls THROUGH a1+4 as a vtable
//      (`(**(int (__cdecl ***)(int,int,int))(a1 + 4))(a1, 1, 28)`) -- while
//      entity+4 is the OPPONENT POINTER (Entity_CheckPriority,
//      decomp:100441). That call would crash on an entity.
//
// With those excluded, a dereference-only scan leaves exactly two accessors of
// entity+0x1A4..+0x1A9: sub_4A6710 (sim, writes once per hit, reads none) and
// sub_4C1F90 (render, owns the lifetime). Holding the record back whole is
// therefore safe, and holding back only part of it is what tore the popup into
// "55 HIT" with a garbage damage number.
//
// (The 2026-08-18 frame-5099 desync briefly looked like evidence against this.
// It was not: Windows' 30-minute monitor timeout slept the display mid-match,
// D3D9 lost the device, recovery failed repeatedly with 0x8876086C, and both
// game threads stalled ~1s per frame. The session is now held awake with
// ES_DISPLAY_REQUIRED so that cannot confound a run again.)
#define ENTITY_POPUP_HOLDBACK_SIZE          6
// F7f follow-up (run 19-29-2x f2249): +0x7FC diverged next — the companion
// state byte written when the +0x7F8 timer wraps. Then run 19-38-2x f3929
// diverged at +0x800..+0x803 (the next dwords of the same region). The
// whole +0x7F0..+0x833 range is the entity's RENDER OUTPUT block — fx
// timers, then the sprite/overlay fields (+0x818 main sprite, +0x81C
// group, +0x820.. overlay x/y/blend/alpha) that render_guard itself
// classifies as render state and REWRITES on rollback corrections at its
// own cadence. Masked wholesale; captured/restored unchanged.
// F10c: the per-entity CHARACTER SPRITE HANDLE TABLE, entity+0x834.
// sub_4C8FF0 (decomp:117529-117534) loads each fighter's own archive into it:
//     v2 = a2 + 10793;                       // match+43172 = entity+0x834
//     Asset_LoadAllFromArchive(v2, &aDataRanBin[100 * charId], ...);
//     v2 += 27203;                           // += ENTITY_SIZE, once per entity
// Verified both ways: 43172 - kP1EntityOff(41072) == 2100 == 0x834, and
// 151984 - kP2EntityOff(149884) == 2100 as well.
//
// Every entry is an image handle from sub_612DF0, so the VALUE carries the
// process-global serial -- the same class as F9/F7h/F10a/F10b, and by far the
// largest instance: up to 450 handles per entity (the biggest character
// archive; ran.bin is 350, mar.bin 188).
//
// SIZE IS THE RESERVATION, NOT THE ARCHIVE. The table has to be a fixed
// reservation or the entity layout would shift per character, and the next
// known field bounds it: entity+4100 (0x1004) is the index for the 104-byte
// array at entity+4104. So the reservation is 0x1004 - 0x834 = 0x7D0 = 500
// dwords, which comfortably holds the 450-asset maximum.
//
// The span looked at first like it contained simulation accessors -- a naive
// scan attributes entity-relative 2364, 2858 and 3922 to it from
// Entity_UpdateHitDetection, Entity_UpdateDamageApplication and
// Players_ResetFlags. Those are OFFSET POINTERS, not entity bases:
// decomp:107560 uses playerPtr+2364 where playerPtr == entity+1736, and
// decomp:118029 uses v2+3922 where v2 == entity+178 -- both resolve to
// entity+4100, the index field, OUTSIDE this window. No accessor resolves
// inside it.
//
// This is what the handle_serial_skew test caught: with only the match-level
// tables masked, an asymmetric run agreed at BASELINE (agreement CRCs exclude
// loader/handle regions by design) and then desynced at frame 29 with
// identical rng and identical hp, because the gameplay digest does NOT
// exclude them. p1_entity and p2_entity were the differing regions in the
// desync dump; effect_array and summon_array matched.
#define ENTITY_SPRITE_HANDLES_OFF           0x0834   // .. +0x1003 (500 handles)
#define ENTITY_SPRITE_HANDLES_SIZE          0x07D0

#define ENTITY_RENDER_OUTPUT_BLOCK_OFF      0x07F0
#define ENTITY_RENDER_OUTPUT_BLOCK_SIZE     0x44
// +440/+444 are advanced/terminated by the RENDER-phase player renderer
// (sub_4C6B60, once per render frame) but live inside the hashed main_state
// region → digest-MASKED (captured/restored, never hashed). SAVESTATE_AUDIT F5.
#define ENTITY_RENDER_TINT_MASK_SIZE     8       // +440..+447 (both dwords)

// Super/stage animated-background scratch (SAVESTATE_AUDIT F2): the render
// phase (sub_4C47C0, post-loop) dispatches on the sim-set state dword at
// entity+1244 and mutates the per-player particle field block around
// entity+1248..+1850 at RENDER cadence (and calls rand() — see the render-RNG
// isolation in input_sync_hooks.cpp). Digest-MASKED per the audit fix list
// (mask +1244..+1850, both players); mask end rounded up to the +1852 dword
// boundary so the last mutated field (+1850) is fully covered.
#define ENTITY_OFF_SUPERBG_STATE         0x04DC  // +1244, DWORD — dispatch state (sim-set)
#define ENTITY_SUPERBG_SCRATCH_MASK_SIZE 0x0260  // 608 B: +1244 .. +1851 inclusive
// F7g (2026-08-17 combat run 19-32-0x, f3059): with supers active the fine
// ring pinned per-side churn to +0x4C4..+0x4DB — the 24 bytes immediately
// BEFORE the F2 mask (window +0x484..+0x4C3 stayed equal, bounding it).
// The superbg particle struct evidently begins at +0x4C4, not +0x4DC; the
// F2 digest mask now starts there (+0x4C4..+0x73B, 632 B). Capture/restore
// unchanged.
#define ENTITY_SUPERBG_MASK_OFF          0x04C4
#define ENTITY_SUPERBG_MASK_SIZE         0x0278
// 2026-08-18: the F2/F7g window swallowed SIMULATION bytes. entity+1236..1243
// (+0x4D4..+0x4DB) holds the hitstop/pause countdowns and combo-scale bytes,
// and the simulation branches on them — decomp:43968 (sub_424B80)
//     if ( !*(_BYTE *)(pEntity + 1238) || *(_BYTE *)(pEntity + 1242) )
// and decomp:111867 (sub_4BF8D0) reads +1238 again. The render function the
// mask exists for (sub_4C47C0) never touches +0x4C4..+0x4DB at all.
//
// Masking them meant any divergence there was INVISIBLE to the desync hash —
// the same "digest-masked is not the same as sim-free" trap that produced the
// frame-58 desync, and a standing suspect for the B1/F8 round-end divergence.
// The window is therefore split so these eight bytes are hashed again; the
// render-churn parts either side stay masked.
// 2026-08-18 (second narrowing): the window swallowed the ATTACK-TRADE
// PRIORITY pair as well. Entity_CheckPriority (sub_49ECB0, decomp:100433-100479)
// writes entity+1244 = priority and entity+1248 = 0, then takes the OPPONENT
// via `result = *(_DWORD *)(entity + 4)` and both READS and WRITES the
// opponent's +1244/+1248 to resolve a trade by Y-then-X position
// (decomp:100445-100476). Cross-entity sim state, and it decides who wins a
// simultaneous attack — the single most rollback-sensitive decision in a
// fighting game.
//
// Attribution scan over the whole decomp: +1244 has exactly 6 references and
// +1248 exactly 2, ALL inside Entity_CheckPriority. There is no render-side
// writer. And sub_4C47C0, the render function this mask exists for, touches no
// literal offset in +0x4C4..+0x73B at all — the original 632-byte window was
// never justified by that function's own accesses.
//
// Only the eight VERIFIED bytes are unmasked here; the rest of the high window
// stays masked rather than narrowed on speculation. That merges the two hashed
// gaps into one contiguous [+0x4D4,+0x4E4).
#define ENTITY_SUPERBG_MASK_LOW_OFF      0x04C4   // .. +0x4D3 (render churn)
#define ENTITY_SUPERBG_MASK_LOW_SIZE     0x0010
#define ENTITY_SIM_PAUSE_TIMERS_OFF      0x04D4   // .. +0x4E3  HASHED (sim):
#define ENTITY_SIM_PAUSE_TIMERS_SIZE     0x0010   //   pause/hitstop + trade priority
#define ENTITY_SUPERBG_MASK_HIGH_OFF     0x04E4   // .. +0x73B (particle scratch)
#define ENTITY_SUPERBG_MASK_HIGH_SIZE    0x0258

// Character voice bookkeeping (SAVESTATE_AUDIT F4): 3 dwords per entity at
// +107084 driven by Entity_UpdateAudio (sub_4C38F0) — [0] requested voice id
// (sim-written), [1] priority latch, [2] last-played id. [1]/[2] writes are
// gated by Audio_IsPlaying (live DSound buffer status = wall clock), so the
// block is captured/restored but digest-MASKED.
#define ENTITY_OFF_VOICE_BOOKKEEPING     0x1A24C // +107084 .. +107095
#define ENTITY_VOICE_BOOKKEEPING_SIZE    12
#define ENTITY_OFF_RENDER_MAIN_SPRITE    0x0818  // +2072, DWORD, main sprite/texture index
#define ENTITY_OFF_RENDER_GROUP          0x081C  // +2076, DWORD, render group/mode
#define ENTITY_OFF_RENDER_OVERLAY_SPRITE 0x0820  // +2080, DWORD, -1 or overlay sprite index
#define ENTITY_OFF_RENDER_OVERLAY_ORDER  0x0824  // +2084, BYTE, overlay draw order flag
#define ENTITY_OFF_RENDER_OVERLAY_X      0x0826  // +2086, WORD
#define ENTITY_OFF_RENDER_OVERLAY_Y      0x0828  // +2088, WORD
#define ENTITY_OFF_RENDER_OVERLAY_BLEND  0x082C  // +2092, DWORD
#define ENTITY_OFF_RENDER_OVERLAY_ALPHA  0x0830  // +2096, BYTE
#define ENTITY_OFF_ANIM_INDEX   0x1004  // +4100, DWORD
#define ENTITY_OFF_ANIM_DATA    0x1008  // +4104, animation data array
#define ENTITY_OFF_BOX_ARRAY    0xA33F  // +41791, box processing array
#define ENTITY_OFF_ASSET_HANDLE_TABLE 0x0834  // +2100 within player entity; match loader passes match_base + 43172 for P1 table

// Animation / box constants
#define ANIM_DATA_STRIDE        104
#define BOX_COORD_SCALE         20
#define BOX_TYPE_COLLISION      0
#define BOX_TYPE_HURTBOX        40
#define BOX_TYPE_HITBOX         72
#define BOX_STRUCT_SIZE         40

// ============================================================================
// Entity Base Pointers
// ============================================================================

#define ADDR_P1_BASE_PRIMARY    (GAME_BASE + 0x376050)  // Direct pointer to P1
#define ADDR_P1_HP_DIRECT       0x00776718

#define ADDR_P2_BASE_PRIMARY    (GAME_BASE + 0x390D5C)  // Estimated P2 base
#define ADDR_P2_HP_DIRECT       0x00791024

// ============================================================================
// Round/Match State
// ============================================================================

#define ADDR_ROUND_TIMER        0x790E50
#define ADDR_WIN_COUNT          0x790E54
#define ADDR_COMBO_COUNT        0x790E56  // byte_790E56 combo timer/state bookkeeping; not the vanilla HUD hit counter
#define ADDR_CHARACTER_MAX_HP_TABLE (GAME_BASE + 0x33DCF4)  // word_73DCF4[22], int16_t max HP per character
#define ADDR_CHARACTER_WEIGHT_TABLE (GAME_BASE + 0x33DD38)  // byte_73DD38[charId], numeric knockback weight

#define ADDR_CHAR_DATA_TABLE    0x8E95F8
#define ADDR_CHAR_INFO_TABLE    0x8E9650
#define ADDR_MATCH_DATA         0x8E93EC

// ============================================================================
// Savestate / Compact Entity Regions
// ============================================================================

#define ENTITY_CORE_START       0x00B0
#define ENTITY_CORE_END         0x00D0

// Action state buffer base: contains reaction state slots, action dispatch ID,
// action priority, and action flags.  Despite the legacy name "INPUT", this is
// NOT an input-only region — entity+0x44C is the ACTION HANDLER DISPATCH ID
// used by every character's per-frame action switch.  DO NOT zero or exclude
// from savestates/digests.
#define ENTITY_INPUT_START      0x0444
#define ENTITY_INPUT_END        0x046C
#define ENTITY_ACTION_START     0x046C
#define ENTITY_ACTION_END       0x04C0
#define ENTITY_STATE_A_START    0x0690
#define ENTITY_STATE_A_END      0x06CC
#define ENTITY_STATE_B_START    0x06CC
#define ENTITY_STATE_B_END      0x0778
#define ENTITY_COMBAT_START     0x0778
#define ENTITY_COMBAT_END       0x07D0
#define ENTITY_TIMER_START      0x07A0
#define ENTITY_TIMER_END        0x07B4

#define ENTITY_CHAR_STATE_START 0xA060
#define ENTITY_CHAR_STATE_SIZE  0x0600

// ============================================================================
// Character Select (Mode 6) Addresses
// ============================================================================

// Base struct pointer for game state (a1 in sub_5BD450)
#define ADDR_CHARSEL_BASE       0x816358

// P1 cursor/control block (6 bytes starting at byte_816017)
#define ADDR_CHARSEL_P1_ENABLE  0x816017  // -1=disabled, 0=browsing, 1=selected
#define ADDR_CHARSEL_P1_CURSOR  0x816018  // Grid index (0-20, lookup via CHARSEL_GRID_TABLE)
#define ADDR_CHARSEL_P1_CONFIRM 0x816019  // 0=not confirmed, 1=confirmed
#define ADDR_CHARSEL_P1_AGE     0x81601A  // Counter/timer after confirm

// P2 cursor/control block (6 bytes starting at byte_81601D)
#define ADDR_CHARSEL_P2_ENABLE  0x81601D  // -1=disabled, 0=browsing, 1=selected
#define ADDR_CHARSEL_P2_CURSOR  0x81601E  // Grid index
#define ADDR_CHARSEL_P2_CONFIRM 0x81601F  // Confirmed flag
#define ADDR_CHARSEL_P2_AGE     0x816020  // Counter/timer after confirm

// Character/palette selection results (inside selection structs)
// P1 sel struct at CHARSEL_BASE + 867080, character at +176, palette at +180
// P2 sel struct at CHARSEL_BASE + 867288, character at +176, palette at +180
#define ADDR_CHARSEL_P1_CHAR_ID 0x8E9F10  // DWORD - P1 selected character ID
#define ADDR_CHARSEL_P1_PALETTE 0x8E9F14  // BYTE  - P1 palette (0-7, -1=unset)
#define ADDR_CHARSEL_P1_VARIANT 0x8E9F2A  // WORD  - P1 variant/color value
#define ADDR_CHARSEL_P1_VARIANT_EXTRA 0x8E9F2C  // BYTE  - P1 variant extra byte
#define ADDR_CHARSEL_P2_CHAR_ID 0x8E9FE0  // DWORD - P2 selected character ID
#define ADDR_CHARSEL_P2_PALETTE 0x8E9FE4  // BYTE  - P2 palette (0-7, -1=unset)
#define ADDR_CHARSEL_P2_VARIANT 0x8E9FFA  // WORD  - P2 variant/color value
#define ADDR_CHARSEL_P2_VARIANT_EXTRA 0x8E9FFC  // BYTE  - P2 variant extra byte

// Grid-to-character lookup table (21 entries)
#define ADDR_CHARSEL_GRID_TABLE 0x74C200  // dword_74C200[21] - maps grid index → char ID

// Misc CharSel state
#define ADDR_CHARSEL_CANCEL     0x816029  // 1 = cancel (return to menu)
#define ADDR_CHARSEL_DISCONNECT 0x81602C  // 1 = disconnect triggered
#define ADDR_CHARSEL_MATCH_CHAR 0x816024  // LOBYTE = character for match config
#define ADDR_MATCH_CONFIG_FLAGS  0x816470  // DWORD - round/stage/training match config block
#define ADDR_MATCH_ROUND_COUNT   0x816470  // LOBYTE(dword_816470) = vanilla round option (wins required = value + 1)
#define ADDR_STAGE_CURSOR       0x816024  // During sub=7 (Preview): LOBYTE=cursor pos, BYTE1=confirmed, BYTE2=roulette counter
#define ADDR_STAGE_AVAIL_TABLE  0x815FFF  // byte_815FFF[24]: 1 = stage selectable (grid confirm refused otherwise)
#define ADDR_CHARSEL_STAGE_ID   0x816471  // BYTE1(dword_816470) = stage ID
#define ADDR_CHARSEL_TEAM_COLOR 0x815FFE  // Team color selection

// ============================================================================
// DXLib Internal Addresses
// ============================================================================

// DXLib "allow duplicate instance" flag. When set to 1 before DXLib_Init,
// the FindWindowA check in sub_630C80 logs but does not abort.
#define ADDR_DXLIB_ALLOW_DUPLICATE 0x9DB884

// ============================================================================
// Save Data / Unlock Flags (config.dat → 80-byte array at 0x8163C0)
// ============================================================================

#define ADDR_CONFIG_VERSION     0x8163BC  // dword_8163BC — config.dat checksum/version (258 when loaded)
#define ADDR_UNLOCK_FLAGS_BASE  0x8163C0  // byte_8163C0[80] — full unlock flag array
#define ADDR_UNLOCK_FLAGS_SIZE  80        // Total size of the unlock flag block
#define ADDR_UNLOCK_ANY_CLEAR   0x8163C0  // Any arcade/story cleared
#define ADDR_UNLOCK_ARCADE_DONE 0x8163C8  // Arcade mode completed
#define ADDR_UNLOCK_STORY_DONE  0x8163C9  // Story/VS mode completed
#define ADDR_UNLOCK_CHAR_BASE   0x8163CC  // byte_8163CC[17] — character unlock flags
#define ADDR_UNLOCK_CHAR_COUNT  17        // Base roster size (indices 0–16)
#define ADDR_UNLOCK_BOSS        0x8163DD  // Boss character unlock flag
#define ADDR_UNLOCK_GALLERY_BASE 0x8163FA // byte_8163FA[~22] — gallery unlock flags
#define ADDR_UNLOCK_GALLERY_COUNT 22      // Approximate gallery entry count

// ============================================================================
// In-Match Settings (vanilla pause menu)
// ============================================================================

// dword_8E93B8 / dword_8E93BC / byte_8E93C0 — native training pause-menu state.
// LOBYTE(dword_8E93B8): Health regeneration percent (0=off, 1..10 = 10%..100%)
#define ADDR_TRAINING_HEALTH_REGEN_SETTING  0x8E93B8
// BYTE1(dword_8E93B8): Meter level (0=off, 1..9 = 1..9 bars)
#define ADDR_TRAINING_METER_LEVEL_SETTING   0x8E93B9
// BYTE2(dword_8E93B8): Opponent control mode (0=native training dummy, 1=full CPU AI)
// When set to 1, the native dummy option rows are disabled.
#define ADDR_TRAINING_DUMMY_BEHAVIOR_ENABLE 0x8E93BA
// HIBYTE(dword_8E93B8): Native air-tech option (0=off, 1=up, 2=forward, 3=neutral, 4=back)
#define ADDR_TRAINING_AIR_TECH_SETTING      0x8E93BB
// LOBYTE(dword_8E93BC): Native ground-tech option (0=off, 1=forward, 2=neutral, 3=back)
#define ADDR_TRAINING_GROUND_TECH_SETTING   0x8E93BC
// BYTE1(dword_8E93BC): Native block type (0=off, 1=normal, 2=1 hit)
#define ADDR_TRAINING_BLOCK_TYPE_SETTING    0x8E93BD
// BYTE2(dword_8E93BC): Native dummy state (0=off, 1=stand, 2=crouch, 3=jump)
#define ADDR_TRAINING_DUMMY_STATE_SETTING   0x8E93BE
// HIBYTE(dword_8E93BC): Native damage display toggle (0=off, 1=on)
#define ADDR_TRAINING_DAMAGE_DISPLAY        0x8E93BF
// byte_8E93C0: Native input/command display toggle (0=off, 1=on). Gates
// sub_4C8E20, which draws the command strip at y=352..447 in training only.
#define ADDR_TRAINING_INPUT_DISPLAY         0x8E93C0
// byte_8E93EA: pause-menu cursor row, 0..10. Wraps 0 <-> 10. Reset to 0 on
// match init (sub_4C92B0) and whenever the menu is left.
#define ADDR_PAUSE_MENU_CURSOR              0x8E93EA
#define PAUSE_MENU_ROW_COUNT                11
#define PAUSE_MENU_ROW_HEALTH_REGEN         0
#define PAUSE_MENU_ROW_METER_LEVEL          1
#define PAUSE_MENU_ROW_CPU                  2
#define PAUSE_MENU_ROW_AIR_TECH             3
#define PAUSE_MENU_ROW_GROUND_TECH          4
#define PAUSE_MENU_ROW_BLOCK_TYPE           5
#define PAUSE_MENU_ROW_DUMMY_STATE          6
#define PAUSE_MENU_ROW_DAMAGE_DISPLAY       7
#define PAUSE_MENU_ROW_INPUT_DISPLAY        8
#define PAUSE_MENU_ROW_RESTART              9   // handler returns 1
#define PAUSE_MENU_ROW_EXIT                 10  // handler returns 2

// Pause menu (MODE_MATCH substate 4). sub_4CA120 owns the substate: it calls the
// input handler, redraws the whole match scene, then draws the menu overlay, and
// finally maps the handler's return code onto the substate change.
//   0   -> resume (substate back to 3)
//   1   -> row 9 action
//   2   -> row 10 action
//   255 -> stay open
// Replacing input + render while leaving sub_4CA120 alone keeps the scene
// rendering and result plumbing native.
#define ADDR_PAUSE_MENU_SUBSTATE    (GAME_BASE + 0x0CA120)  // sub_4CA120 - substate 4 handler
#define ADDR_PAUSE_MENU_INPUT       (GAME_BASE + 0x0C8250)  // sub_4C8250 - cursor + value adjust
#define ADDR_PAUSE_MENU_RENDER      (GAME_BASE + 0x0C8870)  // sub_4C8870 - dim + 11 rows
#define ADDR_TRAINING_INPUT_DISPLAY_RENDER (GAME_BASE + 0x0C8E20)  // sub_4C8E20 - command strip

// --- Match HUD ------------------------------------------------------------
// sub_4C05B0 draws the whole HUD inline (36 quads, 15 sprites) with no
// per-element sub-calls, so per-element hiding filters the two primitives while
// it is on the stack - see patches/hud_toggle.h for the coordinate map. Its own
// address is ADDR_MATCH_HUD_RENDER above; netplay_hud_vanilla owns that hook.
#define ADDR_COMBO_DISPLAY_RENDER  (GAME_BASE + 0x0C1F90)  // sub_4C1F90 - combo/score popups
#define ADDR_STATUS_CALLOUT_RENDER (GAME_BASE + 0x0C7F30)  // sub_4C7F30 - per-player cut-in banner
#define ADDR_RENDER_DRAW_QUAD      (GAME_BASE + 0x1D3060)  // Render_DrawTexturedQuad
#define ADDR_RENDER_DRAW_SPRITE    (GAME_BASE + 0x1D3130)  // Render_DrawSprite
// Vanilla pause-menu geometry: label column x 64..319, value column x 320..447,
// row i spans y = 64 + 32*i .. 95 + 32*i. Selected rows tint red, disabled rows
// draw at half brightness.
#define PAUSE_MENU_LABEL_X      64
#define PAUSE_MENU_VALUE_X      320
#define PAUSE_MENU_RIGHT        447
#define PAUSE_MENU_FIRST_ROW_Y  64
#define PAUSE_MENU_ROW_PITCH    32

// BYTE1(dword_8E93EC) — Number of rounds option (0..2, wins required = value + 1)
// The game copies this into LOBYTE(dword_816470) when constructing match
// config, then gameplay compares each player's win count against value + 1.
#define ADDR_GAMEOPT_ROUND_COUNT 0x8E93ED

// BYTE2(dword_8E93EC) — Stage Select enable/disable toggle (0 or 1)
// When 0, stage is auto-picked from character's home stage lookup table.
// When 1, the stage selection grid is shown during charsel.
#define ADDR_STAGESEL_ENABLE    0x8E93EE
#define ADDR_GAMEOPT_SPECIAL_CHARS 0x8E940E   // special-character slots, forced on by the unlock patch

// ============================================================================
// CPU Flag Addresses (entity+172 inside player selection structs)
// ============================================================================

// byte_8E9F0C: P1 CPU flag (1=AI-controlled, 0=human)
#define ADDR_P1_CPU_FLAG        0x8E9F0C
// byte_8E9FDC: P2 CPU flag (1=AI-controlled, 0=human)
#define ADDR_P2_CPU_FLAG        0x8E9FDC

// ============================================================================
// Command History (training mode input display)
// ============================================================================

// Input_UpdateGlobalCommandHistory — updates the global command history
// display arrays (byte_8E93C2[], byte_8E93D6[], byte_8E93E9) from P1's
// input state.  Reads a1[20540..20580] where a1 = match base as _WORD*.
#define ADDR_CMD_HISTORY_UPDATE  0x4C8C50  // sub_4C8C50

// byte_8E93C0: command history display toggle (1=show, 0=hide)
// Toggled by pause menu option 8 in training mode.
#define ADDR_CMD_HISTORY_DISPLAY 0x8E93C0
