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
#define HITDEF_OFF_ACTIVE_FLAG  24        // byte  — active hitbox flag (-1 = inactive)
#define HITDEF_OFF_DAMAGE       16        // DWORD — damage value
#define HITDEF_OFF_ATK_LEVEL    20        // DWORD — attack level
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
#define ENTITY_OFF_ATTACK_STATE  0x06C8   // +1736, BYTE — 0=inactive, 1=active attack
#define ENTITY_OFF_ATTACK_TYPE   0x06CC   // +1740, DWORD — attack type flags
#define ENTITY_OFF_HIT_ACTIVE    0x06D4   // +1748, BYTE — hit data active flag

// Attack type flag bits (entity+1740)
#define ATTACK_FLAG_LOW_HIT          0x00001  // Low hit type (stand vs crouch)
#define ATTACK_FLAG_PROJ_IMMUNE      0x00800  // Projectile immunity / bypasses the defender +1932 gate in melee/summon checks
#define ATTACK_FLAG_CONTACT_OVERRIDE 0x20000  // Bypasses box-size / overlap checks in grab, damage, hit-detection, and summon-collision paths

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
//   0x2000 is checked as a direct melee-invuln gate in sub_4A5EF0.
//   0x4000 suppresses the normal follow-through branch in sub_4A6030.
//   0x8000 / 0x10000 are special invuln states that still require +1948.
#define CLASH_ID_FLAG_INVINCIBLE         0x10000
#define CLASH_ID_FLAG_SPECIAL_INVULN     0x08000
#define MAX_HIT_FLAG_STRIKE_INVULN       0x00200
#define MAX_HIT_FLAG_MELEE_INVULN        0x02000
#define MAX_HIT_FLAG_PROJECTILE_INVULN   0x04000
#define MAX_HIT_FLAG_INVINCIBLE          0x10000
#define MAX_HIT_FLAG_SPECIAL_INVULN      0x08000

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
#define ENTITY_OFF_ATTACHED_FX_SLOTS       0x07F0
#define ENTITY_ATTACHED_FX_SLOTS_SIZE      0x0028
#define ENTITY_RENDER_OVERLAY_FIELDS_SIZE  0x0019

// Box / route-flag system offsets. The byte at +0x0676 is a candidate
// actionability bit, but it lives inside this vanilla 24-byte flag block and
// must stay audit-only until runtime logs prove its exact semantics.
#define ENTITY_OFF_BOX_FLAGS 0x0674  // +1652, 24-byte vanilla per-frame flag block
#define ENTITY_OFF_NATIVE_ACTIONABLE_CANDIDATE 0x0676 // +1654, candidate actionability bit; audit before primary use
#define ENTITY_OFF_NATIVE_ACTIONABLE ENTITY_OFF_NATIVE_ACTIONABLE_CANDIDATE
#define ENTITY_OFF_RENDER_FLASH_FLAG     0x01B4  // +436, BYTE, extra flash/afterimage draw flag
#define ENTITY_OFF_RENDER_TINT_STATE     0x01B8  // +440, DWORD, 1 disables extra tint pass in sub_4C6B60
#define ENTITY_OFF_RENDER_TINT_TIMER     0x01BC  // +444, DWORD
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

// BYTE1(dword_8E93EC) — Number of rounds option (0..2, wins required = value + 1)
// The game copies this into LOBYTE(dword_816470) when constructing match
// config, then gameplay compares each player's win count against value + 1.
#define ADDR_GAMEOPT_ROUND_COUNT 0x8E93ED

// BYTE2(dword_8E93EC) — Stage Select enable/disable toggle (0 or 1)
// When 0, stage is auto-picked from character's home stage lookup table.
// When 1, the stage selection grid is shown during charsel.
#define ADDR_STAGESEL_ENABLE    0x8E93EE

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
