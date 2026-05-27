/**
 * Alice Senki 2 - Game State Definitions
 * 
 * This file contains all game mode, substate, and game type constants.
 * These values are used to track the current state of the game and are
 * critical for proper rollback netcode operation.
 * 
 * Memory Layout:
 *   - Game Mode:  dword_81638C (0x81638C) - Which handler runs in main loop
 *   - Sub-State:  dword_816390 (0x816390) - Sub-state within current mode
 *   - Game Type:  dword_816410 (0x816410) - Gameplay type (arcade/vs/netplay/etc)
 * 
 * Research Source: internal reverse-engineering notes.
 */

#pragma once

#include <stdint.h>

// ============================================================================
// Memory Addresses for Game State
// ============================================================================

#define ADDR_GAME_MODE       0x81638C  // dword_81638C - Current game mode (0-13)
#define ADDR_SUB_STATE       0x816390  // dword_816390 - Sub-state within mode
#define ADDR_SUB_STATE_TIMER 0x816394  // dword_816394 - Timer/counter for substates
#define ADDR_GAME_TYPE       0x816410  // dword_816410 - Game type (0-10)

// ============================================================================
// Game Mode Values (dword_81638C)
// ============================================================================
// Main game mode, controls which handler function runs in the main loop.
// The main loop at sub_5D2AC0 dispatches to mode handlers via switch.
// Mode 1 appears unused (skipped in main loop switch).

/**
 * MODE_BOOT (0) - Boot/Logo Screen
 * Handler: sub_4FEDE0 @ 0x4FEDE0
 * Substates: 0-7
 * 
 * Initial game mode after launch. Displays company logos and initializes
 * core systems. Loads config.dat for settings.
 * 
 * Flow:
 *   0: Load initial assets, play logo video
 *   1-6: Logo display/fade states  
 *   7: Transition to MODE_MENU (3) via sub_5D2EB0(3,1)
 */
#define MODE_BOOT       0

/**
 * MODE_TITLE (2) - Title Screen
 * Handler: sub_55BC30 @ 0x55BC30
 * Substates: Simple (plays video then transitions)
 * 
 * Opening title screen. Randomly selects and plays one of three
 * opening videos (op01.avi, op02.avi, op03.avi) then transitions
 * to main menu.
 * 
 * Flow:
 *   - Plays random OP video
 *   - Transitions to MODE_MENU (3)
 *   - Can be skipped with input to go to menu
 */
#define MODE_TITLE      2

/**
 * MODE_MENU (3) - Main Menu
 * Handler: sub_5FB200 @ 0x5FB200
 * Substates: Menu navigation states
 * 
 * Main menu screen. Loads tit.bin/tit.pal assets.
 * Handles menu selection and transitions to other modes.
 * 
 * Menu Options:
 *   - Arcade Mode -> MODE_CHARSEL
 *   - VS Mode -> MODE_CHARSEL  
 *   - Training -> MODE_CHARSEL
 *   - Network -> MODE_LOBBY
 *   - Replay -> MODE_REPLAY_SELECT
 *   - Gallery -> MODE_GALLERY
 *   - Options -> MODE_OPTIONS
 */
#define MODE_MENU       3

/**
 * MODE_LOBBY (4) - Network Lobby
 * Handler: sub_558330 @ 0x558330
 * Substates: 0-9 (see LOBBY_SUB_* constants)
 * 
 * Network lobby for setting up online matches. Loads net.bin/net.pal.
 * Handles host/join selection, connection establishment, and chat.
 * 
 * Flow:
 *   0: Initialize, load assets
 *   1-3: Host/join selection UI
 *   4-7: Connection and handshake
 *   8: Return to MODE_MENU (3) on cancel
 *   9: Transition to MODE_CHARSEL (6) on successful connect
 */
#define MODE_LOBBY      4

/**
 * MODE_REPLAY_SELECT (5) - Replay Select
 * Handler: sub_59BBF0 @ 0x59BBF0
 * Substates: 0-4
 * 
 * Screen for selecting replay files to watch. Loads rep.bin/rep.pal.
 * Lists available .rec replay files and allows selection.
 * 
 * Flow:
 *   0: Initialize, load replay list
 *   1: Fade in
 *   2: Selection loop
 *   3: Fade out
 *   4: Transition based on selection:
 *      - If replay selected: MODE_PREMATCH_INTRO (7)
 *      - If cancelled: MODE_MENU (3)
 */
#define MODE_REPLAY_SELECT  5

// Legacy alias — old code may still reference this
#define MODE_VS_SELECT  MODE_REPLAY_SELECT

/**
 * MODE_CHARSEL (6) - Character Select
 * Handler: sub_5BD450 @ 0x5BD450
 * Substates: 0-14 (see CHARSEL_SUB_* constants)
 * 
 * Character selection screen. Loads sel.bin/sel.pal.
 * 
 * CRITICAL FOR NETPLAY:
 * - Vanilla netplay starts input sync HERE (substate 1->2 transition)
 * - Uses sub_562550 (MatchSyncInit) at substate 1 frame 159
 * - Uses sub_5625E0 (InputDispatcher) loop in substates 2,4
 * 
 * Flow:
 *   0: Initialize, load assets (sub_5BD530)
 *   1: Fade-in (sub_5BD910) - calls MatchSyncInit at frame 159!
 *   2: Main selection loop (sub_5BE7B0) - NETPLAY INPUT SYNC ACTIVE
 *      → 4 (normal/skip stage sel) or → 5 (if STAGESEL_ENABLE=1) or → 3 (cancel)
 *   3: Cancel handling (sub_5BFE10)
 *   4: Confirm transition (sub_5C0030) - 25f fade, routes to 11/12/13
 *   5: Stage select: portrait slide-in (16f) → 6
 *   6: Stage select: grid zoom animation (80f) → 7
 *   7: Stage select: INTERACTIVE GRID (24-slot, 12x2 layout) → 8 or 9
 *   8: Stage select: confirm/cancel menu (2-item) → back to 7 or → 9
 *   9: Stage select: fade transition (25f) → 11 (or 12/13 for cancel/netplay)
 *  10: Fade-back transition (25f) → sub 1 (loops for next round in arcade)
 *  11: Matchup data commit — writes stage ID, configures matchup
 *  12: Return to MODE_MENU (3)
 *  13: Return to MODE_LOBBY (4) for netplay
 *  14: Transition to MODE_MATCH (8) or MODE_PREMATCH_INTRO (7)
 */
#define MODE_CHARSEL    6

/**
 * MODE_PREMATCH_INTRO (7) - Pre-Match VS Intro / Loading
 * Handler: sub_46FC80 @ 0x46FC80
 * Substates: 0-38 (0x00-0x26)
 * 
 * NOT stage selection — this is a non-interactive pre-match cinematic.
 * Loads dmo.bin (demo intros) + stg.bin (stage background assets).
 * 39 substates of animation/rendering. No player input is processed
 * for selection — this is purely a visual loading/intro sequence.
 * 
 * Stage selection happens INSIDE Mode 6 (CharSel) substates 5-9,
 * gated by the STAGESEL_ENABLE byte at 0x8E93EE.
 * 
 * Flow:
 *   0: Init
 *   1: Load dmo.bin + stg.bin assets
 *   2-37: VS intro animation sequence
 *   38 (0x26): Transition to MODE_MATCH (8) via sub_5D2EB0(8,0)
 *
 * Netplay note: Vanilla netplay (dword_816410==3) skips Mode 7 entirely
 * and goes directly from CharSel sub 14 to Mode 8.
 */
#define MODE_PREMATCH_INTRO  7

// Legacy alias
#define MODE_STAGESEL  MODE_PREMATCH_INTRO

/**
 * MODE_MATCH (8) - Match/Gameplay
 * Handler: sub_4C8F60 @ 0x4C8F60
 * Substates: 0-5 (see MATCH_SUB_* constants)
 * 
 * *** MAIN GAMEPLAY MODE - ROLLBACK TARGET ***
 * 
 * This is where actual fighting takes place. ALL game types (arcade,
 * vs, training, netplay) use this mode for gameplay.
 * 
 * Match structure at 0x76C5F8:
 *   - Effects array at match+0x1D30 (200 slots × 32 bytes)
 *   - Player entities at dword_776668[0] (P1) and dword_77666C[0] (P2)
 * 
 * Flow:
 *   0 (LOAD_ASSETS): Load character/stage assets (sub_4C8FF0)
 *   1 (SETUP): Reset state, MatchSyncInit for netplay (sub_4C92B0)
 *   2 (INIT): Match initialization, intro sequence (sub_4C9330)
 *   3 (GAMEPLAY): Core match loop (sub_4C9B50) - ROLLBACK HERE
 *      IMPORTANT: this substate is broader than "interactive fighting".
 *      It also covers the opening input-lock / intro pacing and the
 *      25-frame round-end transition while the game is still in state 8.
 *   4 (PAUSE): Pause menu handling (sub_4CA120)
 *   5 (END): Match end, results, transition (sub_4CA210)
 */
#define MODE_MATCH      8

/**
 * MODE_WINSCREEN (9) - Win Screen Handler
 * Handler: sub_5FBD00 @ 0x5FBD00
 * Substates: 0-36 (0x00-0x24)
 * 
 * Handles story mode progression between matches.
 * Shows story dialogue, cutscenes, and manages the arcade ladder.
 * Uses byte_8EA3B0 for story state tracking.
 * 
 * Flow varies based on arcade progress and character selected.
 */
#define MODE_WINSCREEN      9

/**
 * MODE_END (10) - Ending/Credits
 * Handler: sub_4767B0 @ 0x4767B0
 * Substates: 0-9+
 * 
 * Ending and credits screen. Loads end.bin/end.pal.
 * Plays ending sequence after completing arcade mode.
 * Different paths based on game type:
 *   - GameType 0 (Arcade): Substate 4
 *   - GameType non-0: Random between substate 4 and 9
 */
#define MODE_END       10

/**
 * MODE_GALLERY (11) - Memory/Gallery Mode
 * Handler: sub_52D190 @ 0x52D190
 * Substates: 0-5+
 * 
 * Gallery/Memory viewer. Loads mem.bin/mem.pal.
 * Shows unlocked artwork, music, and character profiles.
 * Uses byte_8163FA[] for unlock tracking.
 */
#define MODE_GALLERY   11

/**
 * MODE_OPTIONS (12) - Options Menu
 * Handler: sub_55C3F0 @ 0x55C3F0
 * Substates: 0-7
 * 
 * Game configuration menu. Saves to config.dat.
 * Options include:
 *   - Difficulty
 *   - Sound volume (BGM/SE)
 *   - Key/button config
 *   - Screen settings
 */
#define MODE_OPTIONS   12

/**
 * MODE_PALETTE (13) - Palette/Color Select
 * Handler: sub_5627A0 @ 0x5627A0
 * Substates: 0-4+
 * 
 * Color palette selection for characters. Loads pal.bin/pal.pal.
 * Allows custom color selection before matches.
 */
#define MODE_PALETTE   13

// ============================================================================
// Game Type Values (dword_816410)
// ============================================================================
// Determines gameplay type and affects mode handler behavior.
// Particularly affects MODE_MATCH (8) logic for netplay sync.

#define GAMETYPE_ARCADE    0   // Arcade/Story mode - single player ladder
#define GAMETYPE_VS_CPU    1   // VS CPU - human picks both chars, P2 AI in match
#define GAMETYPE_VS_HUMAN  2   // VS Human (2P local) - each player has own input, we use this for our netplay in mod!
#define GAMETYPE_NETPLAY   3   // Network play - online versus - WE DON'T USE THIS, IT'S VANILLA NETPLAY HANDLER, NOT OUR MOD!!!!
#define GAMETYPE_TRAINING  4   // Training mode (5th main-menu option)
#define GAMETYPE_REPLAY    5   // Replay playback
#define GAMETYPE_DEMO     10   // Demo/attract mode (shown on title screen)

// ============================================================================
// Mode 8 (Match) Substates (dword_816390)
// ============================================================================
// Substates for MODE_MATCH - the main gameplay mode.
// Handler: sub_4C8F60 @ 0x4C8F60 - switches on dword_816390
// Match state structure at 0x76C5F8 (unk_76C5F8)

/** 
 * Substate 0: Load character and stage assets
 * Handler: sub_4C8FF0 @ 0x4C8FF0
 * Loads ran.bin/ran.pal for characters, initializes entity arrays,
 * sets up sound handles based on selected characters.
 * Transitions to substate 1 after loading complete.
 */
#define MATCH_SUB_LOAD_ASSETS   0

/** 
 * Substate 1: Setup/reset state
 * Handler: sub_4C92B0 @ 0x4C92B0
 * Initializes entity pointers (P1 at a2+41072, P2 at a2+149884),
 * clears input history arrays (byte_8E93C1, byte_8E93C2, etc.),
 * calls sub_562550 (MatchSyncInit) to start input sync for netplay.
 * Sets a1[14] = 2 to transition to MATCH_SUB_INIT.
 */
#define MATCH_SUB_SETUP         1

/** 
 * Substate 2: Match initialization / Intro sequence
 * Handler: sub_4C9330 @ 0x4C9330
 * Resets game state struct (byte at a2+0 through a2+11),
 * initializes all player state fields (position, HP, meter, etc.),
 * calls sub_562350 (ResetInputBuffers) for each player,
 * plays intro animations and "FIGHT" announcer.
 * Sets a1[14] = 3 to transition to MATCH_SUB_GAMEPLAY.
 */
#define MATCH_SUB_INIT          2

/** 
 * Substate 3: Core match loop - ROLLBACK ACTIVE HERE
 * Handler: sub_4C9B50 @ 0x4C9B50
 * *** PRIMARY ROLLBACK TARGET ***
 * Contains the while(!sub_5625E0(inputs)) loop that processes frames and is
 * the rollback target for actual simulation. This substate is NOT equivalent
 * to "inputs currently affect the fight":
 *   - At round start it includes the opening intro/input-lock pacing.
 *   - At round end it includes the 25-frame transition countdown before
 *     substate 5 commits the mode change.
 * Checks win/lose conditions, timeout (215900 frames = ~60min), and for
 * netplay (dword_816410==3) disconnect timeout (1800 frames).
 * Exits loop when sub_5625E0 returns non-zero (waiting for input).
 * After loop: renders current frame via sub_4C4770, sub_4C47C0, etc.
 */
#define MATCH_SUB_GAMEPLAY      3

/** 
 * Substate 4: Pause menu handling
 * Handler: sub_4CA120 @ 0x4CA120
 * Displays pause menu, handles resume/quit selection.
 * Still renders the game frame (frozen) behind the menu.
 * sub_4C8250 handles the pause menu logic.
 * Returns to substate 3 on resume, or transitions out on quit.
 */
#define MATCH_SUB_PAUSE         4

/** 
 * Substate 5: Match end / Results
 * Handler: sub_4CA210 @ 0x4CA210
 * Stores winner info in byte at a1+282 from byte at a2+4.
 * Calls sub_4A0A90 for each player to cleanup.
 * Transitions based on byte at a2+10 (win condition):
 *   0: MODE_STORY (9) - continue arcade
 *   1: MODE_CHARSEL (6) - rematch in VS
 *   2: MODE_MENU (3) - return to menu  
 *   3: MODE_REPLAY_SELECT (5) - replay finished
 *   4: MODE_TITLE (2) - demo ended
 *   5: MODE_LOBBY (4) - netplay match ended
 */
#define MATCH_SUB_END           5

// Match runtime flags inside the 16-byte header at 0x76C5F8.
// These are useful for distinguishing the broad Substate 3 loop from the
// smaller window where inputs actually affect gameplay.
#define ADDR_MATCH_HEADER                0x76C5F8
#define MATCH_HEADER_INTRO_LOCK_OFFSET   0
#define MATCH_HEADER_TRANSITION_OFFSET   1
// 0x816370 is a game-side fade / intro countdown that gates the opening lock.
#define ADDR_MATCH_PHASE_TIMER           0x816370

// ============================================================================
// Mode 6 (CharSel) Substates (dword_816390)
// ============================================================================
// Substates for MODE_CHARSEL - character selection.
// Handler: sub_5BD450 @ 0x5BD450 - switches on a1[14] (substate)
// Netplay sync starts here in vanilla, at substate 1->2 transition.

/** 
 * Substate 0: Initialize, load assets
 * Handler: sub_5BD530 @ 0x5BD530
 * Loads sel.bin/sel.pal, initializes sound handles,
 * sets up character grid based on unlocks (byte_815FE8[]).
 * Sets up player sides based on game mode.
 */
#define CHARSEL_SUB_INIT        0

/** 
 * Substate 1: Fade-in animation 
 * Handler: sub_5BD910 @ 0x5BD910
 * CRITICAL: Calls MatchSyncInit (sub_562550) at frame 159!
 * This starts vanilla netplay input sync.
 */
#define CHARSEL_SUB_FADEIN      1

/** 
 * Substate 2: Main selection loop
 * Handler: sub_5BE7B0 @ 0x5BE7B0
 * Uses InputDispatcher (sub_5625E0) in netplay mode.
 * Players select characters here. D-pad moves, A confirms.
 */
#define CHARSEL_SUB_SELECT      2

/** 
 * Substate 3: Cancel button handling
 * Handler: sub_5BFE10 @ 0x5BFE10
 * Handles B button press during selection.
 */
#define CHARSEL_SUB_CANCEL      3

/** 
 * Substate 4: Confirm transition
 * Handler: sub_5C0030 @ 0x5C0030
 * Uses InputDispatcher (sub_5625E0) in netplay mode.
 * Brief state after both players confirm selection.
 */
#define CHARSEL_SUB_CONFIRM     4

/** 
 * Substate 5: Stage select path — portrait slide-in animation (16 frames)
 * Handler: sub_5C0120 @ 0x5C0120
 * Only reached when STAGESEL_ENABLE (0x8E93EE) is 1 and game type is
 * not arcade(0) or netplay(3). Transitions to substate 6.
 */
#define CHARSEL_SUB_STAGESEL_SLIDE     5
#define CHARSEL_SUB_TRANS5  CHARSEL_SUB_STAGESEL_SLIDE  // Legacy alias

/** 
 * Substate 6: Stage select path — grid zoom animation (80 frames)
 * Handler: sub_5C06F0 @ 0x5C06F0
 * Visual transition to the stage grid. Transitions to substate 7.
 */
#define CHARSEL_SUB_STAGESEL_ZOOM      6
#define CHARSEL_SUB_TRANS6  CHARSEL_SUB_STAGESEL_ZOOM   // Legacy alias

/** 
 * Substate 7: Stage select grid — INTERACTIVE (24-slot, 12x2 layout)
 * Handler: sub_5C0B20 @ 0x5C0B20
 * The actual stage selection screen. Player navigates with d-pad, confirms
 * with A. Selection stored in dword_816024. Confirm → sub 9, menu → sub 8.
 */
#define CHARSEL_SUB_STAGESEL_GRID      7
#define CHARSEL_SUB_PREVIEW  CHARSEL_SUB_STAGESEL_GRID   // Legacy alias

/** 
 * Substate 8: Stage select confirm/cancel menu (2-item)
 * Handler: sub_5C12A0 @ 0x5C12A0
 * Shows "OK / Back" after stage is picked. Back → sub 7, proceed → sub 9/10.
 */
#define CHARSEL_SUB_STAGESEL_CONFIRM   8
#define CHARSEL_SUB_STAGE_INTRO  CHARSEL_SUB_STAGESEL_CONFIRM  // Legacy alias

/** 
 * Substate 9: Fade transition (25 frames)
 * Handler: sub_5C1710 @ 0x5C1710
 * Fade-out after stage confirm. Routes: netplay → 13, cancel → 12, normal → 11.
 */
#define CHARSEL_SUB_FADE_OUT    9
#define CHARSEL_SUB_LOADING  CHARSEL_SUB_FADE_OUT  // Legacy alias

/** 
 * Substate 10: Fade-back transition (25 frames)
 * Handler: sub_5C1A10 @ 0x5C1A10
 * Returns to substate 1 for the next round in arcade/story flow.
 */
#define CHARSEL_SUB_FADE_BACK  10
#define CHARSEL_SUB_PREMATCH  CHARSEL_SUB_FADE_BACK  // Legacy alias

/** 
 * Substate 11: Matchup data commit
 * Handler: sub_5C1CA0 @ 0x5C1CA0
 * NOT stage selection — this is the data commit step. Writes the stage grid
 * selection (dword_816024) into the matchup config (a1+281 = stage ID).
 * Configures opponent setup. Sets substate to 14.
 */
#define CHARSEL_SUB_MATCHUP_COMMIT  11
#define CHARSEL_SUB_STAGE  CHARSEL_SUB_MATCHUP_COMMIT  // Legacy alias

/** 
 * Substate 12: Return to MODE_MENU (3)
 * Handler: direct call to sub_5D2EB0(3, 1)
 */
#define CHARSEL_SUB_BACK_MENU  12

/** 
 * Substate 13: Return to MODE_LOBBY (4) for netplay
 * Handler: Calls sub_5FBBA0/sub_5FBCE0 then sub_5D2EB0(4, 1)
 * Host calls sub_5FBBA0, client calls sub_5FBCE0 to cleanup.
 */
#define CHARSEL_SUB_BACK_LOBBY 13

/** 
 * Substate 14: Transition to next mode
 * Handler: Calls sub_5D2EB0(8, 1) for netplay or sub_5D2EB0(7, 1) otherwise
 * Netplay (dword_816410==3): Goes directly to MODE_MATCH (8)
 * Other modes: Goes to MODE_PREMATCH_INTRO (7) first
 */
#define CHARSEL_SUB_TO_MATCH   14

// ============================================================================
// Mode 4 (Lobby) Substates (dword_816390)
// ============================================================================
// Substates for MODE_LOBBY - network lobby.
// Handler: sub_558330 at 0x558330 - handles state 0-9

/** Substate 0: Initialize lobby, load net.bin/net.pal, setup sound */
#define LOBBY_SUB_INIT          0

/** Substate 1: Fade-in, display lobby screen */
#define LOBBY_SUB_FADEIN        1

/** Substate 2: Main lobby interaction loop - D-pad moves cursor, A confirms, B cancels */
#define LOBBY_SUB_INTERACT      2

/** Substate 3: Stage info display / opponent info viewing */
#define LOBBY_SUB_INFO          3

/** Substate 4: Fade out - returning to menu */
#define LOBBY_SUB_FADEOUT       4

// ============================================================================
// Mode 5 (VS Select / Gallery) Substates (dword_816390)  
// ============================================================================
// Substates for MODE_GALLERY (Memory Select) - character unlock gallery
// Handler: sub_52D190 at 0x52D190

/** Substate 0: Initialize - load mem.bin/mem.pal, setup sound */
#define GALLERY_SUB_INIT        0

/** Substate 1: Fade-in with character grid display */
#define GALLERY_SUB_FADEIN      1

/** Substate 2: Main selection loop - cursor navigation */
#define GALLERY_SUB_SELECT      2

/** Substate 3: Character detail view - shows full portrait */
#define GALLERY_SUB_DETAIL      3

/** Substate 4: Fade-out - returning to menu */
#define GALLERY_SUB_FADEOUT     4

// ============================================================================
// Mode 12 (Options) Substates (dword_816390)
// ============================================================================
// Substates for MODE_OPTIONS - configuration menu.
// Handler: sub_55C3F0 at 0x55C3F0

/** Substate 0: Initialize - allocate config buffer (0x3E0 bytes), call sub_55C620 */
#define OPTIONS_SUB_INIT        0

/** Substate 1: Fade-in - countdown from 25 frames, call sub_55C720 for display */
#define OPTIONS_SUB_FADEIN      1

/** Substate 2: Main menu loop - call sub_55CA10 for selection handling */
#define OPTIONS_SUB_MAIN        2

/** Substate 3: Game options - difficulty, rounds, etc (sub_55CB90, sub_55D6F0) */
#define OPTIONS_SUB_GAME        3

/** Substate 4: Sound options - BGM/SE volume (sub_55E720, sub_5603B0) */
#define OPTIONS_SUB_SOUND       4

/** Substate 5: Display options - screen settings (sub_5614F0, sub_5616B0) */
#define OPTIONS_SUB_DISPLAY     5

/** Substate 6: Key config - button mappings (sub_561CB0, sub_563270) */
#define OPTIONS_SUB_KEYCONFIG   6

/** Substate 7: Exit - save config.dat, fade out, return to menu */
#define OPTIONS_SUB_EXIT        7

// ============================================================================
// Mode 9 (Story) Substates (dword_816390)
// ============================================================================
// Substates for MODE_STORY - arcade/story mode progression.
// Handler: sub_5FBD00 at 0x5FBD00
// Uses byte_8EA3B0[] for story state tracking.

/** Substate 0: Route determination (sub_5FBE90) - check win/lose, select next fight */
#define STORY_SUB_ROUTE         0

/** Substate 1: Pre-fight dialogue setup (sub_5FBEE0) */
#define STORY_SUB_DIALOGUE_INIT 1

/** Substate 2: Dialogue display (sub_601900) */
#define STORY_SUB_DIALOGUE      2

/** Substate 3: Dialogue advance (sub_6019F0) */
#define STORY_SUB_DIALOGUE_ADV  3

/** Substate 4: Dialogue end (sub_601BB0) */
#define STORY_SUB_DIALOGUE_END  4

/** Substate 5: Event setup (sub_601EF0) */
#define STORY_SUB_EVENT_SETUP   5

/** Substate 6: Event display (sub_6020D0) */
#define STORY_SUB_EVENT         6

/** Substate 7: Event end (sub_602370) */
#define STORY_SUB_EVENT_END     7

/** Substate 8: Pre-match (sub_6025A0) */
#define STORY_SUB_PREMATCH      8

/** Substate 9: Stage intro (sub_6026A0) */
#define STORY_SUB_STAGE_INTRO   9

/** Substate 10 (0xA): Stage display (sub_603190) */
#define STORY_SUB_STAGE         10

/** Substates 11-16 (0xB-0x10): Cutscene states (sub_6032E0) */
#define STORY_SUB_CUTSCENE_START 11
#define STORY_SUB_CUTSCENE_END   16

/** Substate 17 (0x11): Results init (sub_604C90) */
#define STORY_SUB_RESULTS_INIT  17

/** Substate 18 (0x12): Results display (sub_604DF0) */
#define STORY_SUB_RESULTS       18

/** Substates 19-35 (0x13-0x23): Ending sequence (sub_6069A0) */
#define STORY_SUB_ENDING_START  19
#define STORY_SUB_ENDING_END    35

/** Substate 36 (0x24): Story complete (sub_609AE0) */
#define STORY_SUB_COMPLETE      36

// ============================================================================
// Match Gameplay Update Functions (from Game_Update_MatchLoop @ 0x4C9B50)
// ============================================================================
// These functions are called every gameplay frame in MODE_MATCH SUBSTATE 3.
// The main loop is at sub_4C9B50.
// Listed in call order for understanding the game's frame update flow.
//
// This information is useful for:
// - Understanding what state needs to be saved/restored for rollback
// - Potential future hook points for optimization
// - Debugging desyncs by understanding update order

// Match_ClearPerFrameTempData - Clears temporary per-frame state
#define ADDR_MATCH_CLEAR_TEMP       0x004C9B50  // First call in loop

// Match_UpdateRoundTimer - Updates the round timer display
#define ADDR_MATCH_UPDATE_TIMER     0x004A0C10  // Match_UpdateRoundTimer

// Match_UpdateRoundState - Checks for round end conditions
#define ADDR_MATCH_UPDATE_ROUND     0x004A0D70  // Match_UpdateRoundState

// Input processing chain (critical for rollback!)
#define ADDR_MATCH_UPDATE_INPUT_HIST    0x0049DB00  // Match_UpdateInputHistory
#define ADDR_MATCH_UPDATE_INPUT_BUF     0x0049FCC0  // Match_UpdateInputBuffers
#define ADDR_MATCH_SHIFT_INPUT          0x0049DA10  // Match_ShiftInputState
#define ADDR_MATCH_UPDATE_STATE_FLAGS   0x0049DBB0  // Match_UpdateStateFlags
#define ADDR_MATCH_UPDATE_INPUT_TIMERS  0x0049DBE0  // Match_UpdateInputTimers
#define ADDR_MATCH_UPDATE_COMBO_TIMERS  0x0049DC50  // Match_UpdateComboTimers
#define ADDR_MATCH_UPDATE_STUN_TIMERS   0x0049DCE0  // Match_UpdateStunTimers

// Entity update chain
#define ADDR_ENTITY_UPDATE_REGEN    0x0049F3D0  // Entity_UpdateRegen_Global
#define ADDR_ENTITY_UPDATE_ORIENT   0x0049F450  // Entity_UpdateOrientation_Global
#define ADDR_ENTITY_UPDATE_STATE    0x0049F550  // Entity_UpdateStateTransition_Main
#define ADDR_ENTITY_UPDATE_STATE2   0x0049F8D0  // Entity_UpdateStateTransition_Sub

// Collision and combat
#define ADDR_ENTITY_RESOLVE_BODY    0x004A0200  // Entity_ResolveBodyCollision
#define ADDR_ENTITY_RESOLVE_ATTACK  0x004A05F0  // Entity_ResolveAttackCollision

// Effect/summon update (ADDR_EFFECT_UPDATE already defined in as2_rollback.h)
// #define ADDR_EFFECT_UPDATE       0x004A9330  // Effect_Update

// Input dispatcher (called in blocking loop during netplay)
#define ADDR_INPUT_TRY_GET_FRAME    0x005625E0  // Input_TryGetNextFrame (sub_5625E0)
#define ADDR_NETPLAY_INIT_SYNC      0x00562550  // Netplay_InitialSync (sub_562550)

// ============================================================================
// Inline Helper Functions
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get current game mode (MODE_BOOT through MODE_PALETTE)
 */
static inline uint32_t GetGameMode(void) {
    return *(volatile uint32_t*)ADDR_GAME_MODE;
}

/**
 * Get current substate within the mode
 */
static inline uint32_t GetSubstate(void) {
    return *(volatile uint32_t*)ADDR_SUB_STATE;
}

/**
 * Get current game type (GAMETYPE_ARCADE through GAMETYPE_DEMO)
 */
static inline uint32_t GetGameType(void) {
    return *(volatile uint32_t*)ADDR_GAME_TYPE;
}

/**
 * Check if currently in vanilla netplay mode.
 * Mod-owned online flow normally runs under VS_HUMAN instead.
 */
static inline bool IsNetplay(void) {
    return GetGameType() == GAMETYPE_NETPLAY;
}

/**
 * Check if in MODE_MATCH (gameplay)
 */
static inline bool IsInMatch(void) {
    return GetGameMode() == MODE_MATCH;
}

/**
 * Check if in MODE_MATCH with the vanilla GAMETYPE_NETPLAY type.
 */
static inline bool IsInNetplayMatch(void) {
    return IsInMatch() && IsNetplay();
}

/**
 * Check if in MODE_CHARSEL (character select)
 */
static inline bool IsInCharSel(void) {
    return GetGameMode() == MODE_CHARSEL;
}

/**
 * Check if in vanilla netplay character select.
 */
static inline bool IsInNetplayCharSel(void) {
    return IsInCharSel() && IsNetplay();
}

/**
 * Check if in MODE_LOBBY (network lobby)
 */
static inline bool IsInLobby(void) {
    return GetGameMode() == MODE_LOBBY;
}

/**
 * Check if currently in the Mode 8 Substate 3 core loop.
 * This is broader than "interactive gameplay" and includes the round-start
 * intro lock and the round-end transition countdown.
 */
static inline bool IsInActiveGameplay(void) {
    return IsInMatch() && GetSubstate() == MATCH_SUB_GAMEPLAY;
}

/**
 * Check if in the Mode 8 Substate 3 core loop during vanilla netplay.
 */
static inline bool IsInNetplayGameplay(void) {
    return IsInNetplayMatch() && GetSubstate() == MATCH_SUB_GAMEPLAY;
}

/**
 * Read the game-managed phase timer used by match intro / fade pacing.
 */
static inline uint32_t GetMatchPhaseTimer(void) {
    return *(volatile uint32_t*)ADDR_MATCH_PHASE_TIMER;
}

/**
 * Read a byte from the 16-byte match header at 0x76C5F8.
 */
static inline uint8_t GetMatchHeaderByte(uint32_t offset) {
    return *(volatile uint8_t*)(ADDR_MATCH_HEADER + offset);
}

/**
 * Returns true while the opening round-start lock / intro is still active.
 *
 * Notes:
 * - The raw match-header byte alone proved unreliable in prior rollback work.
 * - Substate timer alone is also insufficient because it is reset on some
 *   later Substate 3 re-entries (for example resume from pause).
 * - Combining the explicit intro-lock byte with the game-side phase timer is
 *   a more stable "opening lock is still active" signal for tooling/UI.
 */
static inline bool IsMatchIntroActive(void) {
    return IsInActiveGameplay() &&
           (GetMatchHeaderByte(MATCH_HEADER_INTRO_LOCK_OFFSET) != 0 ||
            GetMatchPhaseTimer() != 0);
}

/**
 * Returns true while the round-end / exit transition has started but the game
 * is still executing inside Substate 3.
 */
static inline bool IsMatchTransitionActive(void) {
    return IsInActiveGameplay() &&
           GetMatchHeaderByte(MATCH_HEADER_TRANSITION_OFFSET) != 0;
}

/**
 * Returns true only when Substate 3 is in its interactive fighting window.
 */
static inline bool IsInPlayableMatchGameplay(void) {
    return IsInActiveGameplay() &&
           !IsMatchIntroActive() &&
           !IsMatchTransitionActive();
}

/**
 * Check if in the vanilla CharSel input-sync substates.
 * Mod-owned online charsel does not rely on GAMETYPE_NETPLAY here.
 */
static inline bool IsInCharSelInputSync(void) {
    uint32_t sub = GetSubstate();
    return IsInNetplayCharSel() && 
           (sub == CHARSEL_SUB_SELECT || sub == CHARSEL_SUB_CONFIRM);
}

static inline const char* GameModeName(uint32_t mode) {
    switch (mode) {
        case MODE_BOOT:           return "Boot";
        case MODE_TITLE:          return "Title";
        case MODE_MENU:           return "Menu";
        case MODE_LOBBY:          return "Lobby";
        case MODE_REPLAY_SELECT:  return "ReplaySelect";
        case MODE_CHARSEL:        return "CharSel";
        case MODE_PREMATCH_INTRO: return "PrematchIntro";
        case MODE_MATCH:          return "Match";
        case MODE_WINSCREEN:      return "WinScreen";
        case MODE_END:            return "End";
        case MODE_GALLERY:        return "Gallery";
        case MODE_OPTIONS:        return "Options";
        case MODE_PALETTE:        return "Palette";
        default:                  return "UnknownMode";
    }
}

static inline const char* MatchSubstateName(uint32_t substate) {
    switch (substate) {
        case MATCH_SUB_LOAD_ASSETS: return "LoadAssets";
        case MATCH_SUB_SETUP:       return "Setup";
        case MATCH_SUB_INIT:        return "Init";
        case MATCH_SUB_GAMEPLAY:    return "Gameplay";
        case MATCH_SUB_PAUSE:       return "Pause";
        case MATCH_SUB_END:         return "End";
        default:                    return "UnknownMatchSub";
    }
}

static inline const char* CharSelSubstateName(uint32_t substate) {
    switch (substate) {
        case CHARSEL_SUB_INIT:              return "Init";
        case CHARSEL_SUB_FADEIN:            return "FadeIn";
        case CHARSEL_SUB_SELECT:            return "Select";
        case CHARSEL_SUB_CANCEL:            return "Cancel";
        case CHARSEL_SUB_CONFIRM:           return "Confirm";
        case CHARSEL_SUB_STAGESEL_SLIDE:    return "StageSlide";
        case CHARSEL_SUB_STAGESEL_ZOOM:     return "StageZoom";
        case CHARSEL_SUB_STAGESEL_GRID:     return "StageGrid";
        case CHARSEL_SUB_STAGESEL_CONFIRM:  return "StageConfirm";
        case CHARSEL_SUB_FADE_OUT:          return "FadeOut";
        case CHARSEL_SUB_FADE_BACK:         return "FadeBack";
        case CHARSEL_SUB_MATCHUP_COMMIT:    return "MatchupCommit";
        case CHARSEL_SUB_BACK_MENU:         return "BackMenu";
        case CHARSEL_SUB_BACK_LOBBY:        return "BackLobby";
        case CHARSEL_SUB_TO_MATCH:          return "ToMatch";
        default:                            return "UnknownCharSelSub";
    }
}

static inline const char* NativeSubstateName(uint32_t mode, uint32_t substate) {
    switch (mode) {
        case MODE_MATCH:
            return MatchSubstateName(substate);
        case MODE_CHARSEL:
            return CharSelSubstateName(substate);
        case MODE_LOBBY:
            switch (substate) {
                case LOBBY_SUB_INIT:     return "Init";
                case LOBBY_SUB_FADEIN:   return "FadeIn";
                case LOBBY_SUB_INTERACT: return "Interact";
                case LOBBY_SUB_INFO:     return "Info";
                case LOBBY_SUB_FADEOUT:  return "FadeOut";
                default:                 return "UnknownLobbySub";
            }
        case MODE_OPTIONS:
            switch (substate) {
                case OPTIONS_SUB_INIT:      return "Init";
                case OPTIONS_SUB_FADEIN:    return "FadeIn";
                case OPTIONS_SUB_MAIN:      return "Main";
                case OPTIONS_SUB_GAME:      return "Game";
                case OPTIONS_SUB_SOUND:     return "Sound";
                case OPTIONS_SUB_DISPLAY:   return "Display";
                case OPTIONS_SUB_KEYCONFIG: return "KeyConfig";
                case OPTIONS_SUB_EXIT:      return "Exit";
                default:                    return "UnknownOptionsSub";
            }
        case MODE_GALLERY:
            switch (substate) {
                case GALLERY_SUB_INIT:    return "Init";
                case GALLERY_SUB_FADEIN:  return "FadeIn";
                case GALLERY_SUB_SELECT:  return "Select";
                case GALLERY_SUB_DETAIL:  return "Detail";
                case GALLERY_SUB_FADEOUT: return "FadeOut";
                default:                  return "UnknownGallerySub";
            }
        case MODE_WINSCREEN:
            switch (substate) {
                case STORY_SUB_ROUTE:         return "Route";
                case STORY_SUB_DIALOGUE_INIT: return "DialogueInit";
                case STORY_SUB_DIALOGUE:      return "Dialogue";
                case STORY_SUB_DIALOGUE_ADV:  return "DialogueAdvance";
                case STORY_SUB_DIALOGUE_END:  return "DialogueEnd";
                case STORY_SUB_EVENT_SETUP:   return "EventSetup";
                case STORY_SUB_EVENT:         return "Event";
                case STORY_SUB_EVENT_END:     return "EventEnd";
                case STORY_SUB_PREMATCH:      return "Prematch";
                case STORY_SUB_STAGE_INTRO:   return "StageIntro";
                case STORY_SUB_STAGE:         return "Stage";
                case STORY_SUB_RESULTS_INIT:  return "ResultsInit";
                case STORY_SUB_RESULTS:       return "Results";
                case STORY_SUB_COMPLETE:      return "Complete";
                default:                      return "StorySequence";
            }
        default:
            return "Substate";
    }
}

#ifdef __cplusplus
}
#endif
