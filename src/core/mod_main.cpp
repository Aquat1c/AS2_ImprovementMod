/**
 * Alice Senki 2 - Mod Main Module
 *
 * Slim lifecycle / game-state core.  All rollback logic has been removed.
 * Input hooks live in input_override, tick hooks in tick_hooks,
 * hook installation in hook_installer.
 */

#include "mod_main.h"
#include "input_system.h"
#include "log_window.h"
#include "mod_menu.h"
#include "hitbox_viewer.h"
#include "ui/netplay_hud.h"
#include "game_console.h"
#include "patches/memory_utils.h"
#include "patches/unlock_patch.h"
#include "patches/input_override.h"
#include "patches/hook_installer.h"
#include "patches/filesystem_patch.h"
#include "patches/palette_asset_hook.h"
#include "patches/tick_hooks.h"
#include "rollback/determinism_verify.h"
#include "rollback/savestate.h"
#include "rollback/rollback_session.h"
#include "rollback/rollback_debug.h"
#include "rollback/netplay_log.h"
#include "rollback/stress_hooks.h"
#include "rollback/online_wiring.h"
#include "net/spectator_runtime.h"
#include "net/spectator_client.h"
#include "net/netplay_palette_runtime.h"
#include "net/enet_transport.h"
#include "net/session_manager.h"
#include "net/mode_ownership.h"
#include "net/netplay_menu_controller.h"
#include "net/pregame_sync.h"
#include "net/match_lifecycle.h"
#include "net/sync_policy.h"
#include "net/delay_policy.h"
#include "net/gameplay_bridge.h"
#include "net/set_tracker.h"
#include "net/player_side_mapping.h"
#include "testing/scripted_input_runner.h"
#include "training/practice_tools.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <mmsystem.h>

// ============================================================================
// Configuration
// ============================================================================

struct ModConfig {
    bool useSDLInput;
    bool bypassGameInput;
    int  inputDisplayMode;
    bool showHitboxes;
    bool showFrameData;
    bool verboseLogging;
    int  rollbackFrames;
    int  inputDelay;
};

static ModConfig g_config = {
    true,   // useSDLInput
    false,  // bypassGameInput
    1,      // inputDisplayMode
    false,  // showHitboxes
    false,  // showFrameData
    true,   // verboseLogging
    8,      // rollbackFrames (unused, kept for compat)
    0       // inputDelay    (unused, kept for compat)
};

// ============================================================================
// Internal state
// ============================================================================

static HMODULE g_gameModule = nullptr;
static bool g_initialized = false;
static bool g_forceBorderlessFullscreen = false;
static bool g_imguiBaseStyleCaptured = false;
static float g_lastAppliedImGuiScale = -1.0f;
static ImGuiStyle g_imguiBaseStyle = {};

typedef void (__cdecl *ProxyGetResolution_t)(int* width, int* height);

// ============================================================================
// Config accessors (for other modules)
// ============================================================================

bool ModConfig_UseSDLInput() {
    return g_config.useSDLInput;
}

bool ModConfig_VerboseLogging() {
    return g_config.verboseLogging;
}

static float ClampFloat(float value, float minValue, float maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

float ModUI_GetScale() {
    static HMODULE s_proxyModule = nullptr;
    static ProxyGetResolution_t s_getNativeResolution = nullptr;
    static ProxyGetResolution_t s_getScreenResolution = nullptr;
    static bool s_lookedUp = false;

    if (!s_lookedUp) {
        s_lookedUp = true;
        s_proxyModule = GetModuleHandleA("d3d9.dll");
        if (s_proxyModule) {
            s_getNativeResolution = (ProxyGetResolution_t)GetProcAddress(s_proxyModule, "GetNativeResolution");
            s_getScreenResolution = (ProxyGetResolution_t)GetProcAddress(s_proxyModule, "GetScreenResolution");
        }
    }

    if (!s_getNativeResolution || !s_getScreenResolution) {
        return 1.0f;
    }

    int nativeWidth = 0;
    int nativeHeight = 0;
    int screenWidth = 0;
    int screenHeight = 0;
    s_getNativeResolution(&nativeWidth, &nativeHeight);
    s_getScreenResolution(&screenWidth, &screenHeight);
    if (nativeWidth <= 0 || nativeHeight <= 0 || screenWidth <= 0 || screenHeight <= 0) {
        return 1.0f;
    }

    const float scaleX = (float)screenWidth / (float)nativeWidth;
    const float scaleY = (float)screenHeight / (float)nativeHeight;
    float upscale = scaleX < scaleY ? scaleX : scaleY;
    if (upscale < 1.0f) {
        upscale = 1.0f;
    }

    return ClampFloat(1.0f / upscale, 0.40f, 1.0f);
}

float ModUI_Scale(float value) {
    return value * ModUI_GetScale();
}

static void ApplySharedImGuiScale() {
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();
    if (!g_imguiBaseStyleCaptured) {
        g_imguiBaseStyle = style;
        g_imguiBaseStyleCaptured = true;
    }

    const float scale = ModUI_GetScale();
    if (fabsf(scale - g_lastAppliedImGuiScale) < 0.001f) {
        return;
    }

    style = g_imguiBaseStyle;
    style.ScaleAllSizes(scale);
    io.FontGlobalScale = scale;
    g_lastAppliedImGuiScale = scale;
}

// ============================================================================
// Verbose logging control
// ============================================================================

void SetVerboseLogging(bool enabled) {
    g_config.verboseLogging = enabled;
    Rollback::NetplayLog_SetVerbose(enabled);
    LOG_INFO("[Mod] Verbose logging %s", enabled ? "enabled" : "disabled");
}

bool GetVerboseLogging() {
    return g_config.verboseLogging;
}

// ============================================================================
// Game State Queries
// ============================================================================

uint32_t AS2_GetFrameNumber() {
    return ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

bool AS2_IsInMatch() {
    return GetGameMode() == MODE_MATCH;
}

bool AS2_IsInGameplay() {
    return IsInActiveGameplay();
}

bool AS2_IsInPlayableGameplay() {
    return IsInPlayableMatchGameplay();
}

bool AS2_IsInPauseMenu() {
    return GetGameMode() == MODE_MATCH && GetSubstate() == 4;
}

bool AS2_IsInMenu() {
    uint32_t mode = GetGameMode();
    return mode != MODE_MATCH || GetSubstate() != 3;
}

// ============================================================================
// Entity / HP access
// ============================================================================

uint16_t GetP1HP() {
    return ReadMemory<uint16_t>(ADDR_P1_HP_DIRECT);
}

uint16_t GetP2HP() {
    return ReadMemory<uint16_t>(ADDR_P2_HP_DIRECT);
}

uintptr_t GetEntityBase(int player) {
    if (player == 0) {
        return ADDR_P1_HP_DIRECT - ENTITY_OFF_HP;
    } else {
        return ADDR_P2_HP_DIRECT - ENTITY_OFF_HP;
    }
}

// ============================================================================
// Quick Checksum (replicates sub_49EE60)
// ============================================================================

uint32_t AS2_GetQuickChecksum() {
    uintptr_t p1Base = GetEntityBase(0);
    uintptr_t p2Base = GetEntityBase(1);

    uint16_t p1HP = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_HP);
    uint16_t p1Meter = ReadMemory<uint16_t>(p1Base + ENTITY_OFF_METER);
    int16_t  p1X = ReadMemory<int16_t>(p1Base + ENTITY_OFF_X_POS);
    int16_t  p1Y = ReadMemory<int16_t>(p1Base + ENTITY_OFF_Y_POS);

    uint16_t p2HP = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_HP);
    uint16_t p2Meter = ReadMemory<uint16_t>(p2Base + ENTITY_OFF_METER);
    int16_t  p2X = ReadMemory<int16_t>(p2Base + ENTITY_OFF_X_POS);
    int16_t  p2Y = ReadMemory<int16_t>(p2Base + ENTITY_OFF_Y_POS);

    uint32_t sum = (uint32_t)p1HP + (uint32_t)p1Meter + (uint32_t)(uint16_t)p1X + (uint32_t)(uint16_t)p1Y
                 + (uint32_t)p2HP + (uint32_t)p2Meter + (uint32_t)(uint16_t)p2X + (uint32_t)(uint16_t)p2Y;

    return (uint16_t)(sum % 0x10000);
}

// ============================================================================
// Clear Vanilla Netplay Buffers
// ============================================================================

void AS2_ClearVanillaNetplayBuffers() {
    LOG_INFO("[AS2] Clearing vanilla netplay buffers...");

    __try {
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_DISPLAY) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_WRITE_IDX) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_FRAME_NET_IDX) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_REMOTE_FRAME) = 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla frame counters");
    }

    __try {
        memset(reinterpret_cast<void*>(ADDR_VANILLA_LOCAL_INPUTS), 0xFF, VANILLA_LOCAL_INPUT_SIZE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla local input buffer");
    }

    __try {
        memset(reinterpret_cast<void*>(ADDR_VANILLA_REMOTE_INPUTS), 0xFF, VANILLA_REMOTE_INPUT_SIZE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla remote input buffer");
    }

    __try {
        memset(reinterpret_cast<void*>(ADDR_VANILLA_SYNC_LOCAL), 0, 10);
        memset(reinterpret_cast<void*>(ADDR_VANILLA_SYNC_REMOTE), 0, 10);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("[AS2] Failed to clear vanilla sync flags");
    }
}

// ============================================================================
// Deferred Initialization (first frame when D3D9 is ready)
// ============================================================================

static void DeferredInit() {
    LOG_INFO("Performing deferred initialization...");

    if (!InputSystem_Init()) {
        LOG_WARN("Failed to initialize SDL input system - using native input only");
    } else {
        LOG_INFO("SDL3 input system initialized");
    }

    if (!InstallHooks()) {
        LOG_ERROR("Failed to install hooks!");
        return;
    }

    GameConsole_Init();

    // Initialize determinism verification system
    DetVer_Init();
    DetVer_SetLogDir(LogWindow_GetLogDir());

    // Initialize savestate system
    Savestate_Init();
    Savestate_SetLogDir(LogWindow_GetLogDir());

    // Initialize networking
    Net::Transport_GlobalInit();
    Net::Session_Init();
    Net::NetplayPaletteRuntime_Init();
    Net::SpectatorRuntime_Init();
    Net::SpectatorClient_Init();

    // Initialize netplay menu controller and mode ownership hooks
    NetMenu::Init();
    if (!ModeOwnership::Install()) {
        LOG_ERROR("Failed to install mode ownership hooks!");
    }

    // Initialize pre-game synchronization layer
    Net::PregameSync_Init();

    // Initialize match lifecycle state management
    Net::MatchLifecycle_Init();

    // Initialize sync policy and delay policy
    Net::SyncPolicy_Init();
    Net::DelayPolicy_Init();

    // Initialize gameplay bridge (checks GekkoNet availability for future use)
    Net::GameplayBridge_Init();

    // Initialize rollback gameplay subsystems
    Rollback::RollbackSession_Init();
    Rollback::RollbackDebug_Init();

    // Initialize netplay full-path log, stress hooks, and online wiring
    Rollback::NetplayLog_Init();
    Rollback::NetplayLog_SetLogDir(LogWindow_GetLogDir());
    Rollback::NetplayLog_SetVerbose(g_config.verboseLogging);
    Rollback::OnlineWiring_Init();

    // Initialize scripted input runner
    SIR_Init();

    // Initialize practice mode tools
    PracticeTools_Init();

    LOG_INFO("Frame Counter: 0x%08X = %d", ADDR_SIM_FRAME_COUNTER, AS2_GetFrameNumber());
    LOG_INFO("Game Mode: 0x%08X = %d", ADDR_GAME_MODE, GetGameMode());
    LOG_INFO("P1 HP: 0x%08X = %d", ADDR_P1_HP_DIRECT, GetP1HP());
    LOG_INFO("P2 HP: 0x%08X = %d", ADDR_P2_HP_DIRECT, GetP2HP());

    g_initialized = true;

    ModMenu_Init();
    HitboxViewer_Init();

    LOG_INFO("========================================");
    LOG_INFO("Initialization complete!");
    LOG_INFO("Hotkeys: F1=Menu  F4=Hitbox  F5=SaveState  F6=LoadState  F7=Pause  F8=Step  F9=Swap");
    LOG_INFO("========================================");
}

// ============================================================================
// Exported Functions
// ============================================================================

extern "C" {

__declspec(dllexport) void ModSetImGuiContext(void* ctx) {
    ImGui::SetCurrentContext((ImGuiContext*)ctx);
    g_imguiBaseStyleCaptured = false;
    g_lastAppliedImGuiScale = -1.0f;
}

__declspec(dllexport) void ModSetLogDir(const char* dir) {
    LogWindow_SetLogDir(dir);
}

__declspec(dllexport) void ModInit(HMODULE gameModule) {
    g_gameModule = gameModule;

    LogWindow_Init();
    timeBeginPeriod(1);

    LOG_INFO("========================================");
    LOG_INFO("Alice Senki 2 - Mod v0.3");
    LOG_INFO("Build: %s %s", __DATE__, __TIME__);
    LOG_INFO("========================================");
    LOG_INFO("Game module: 0x%p", gameModule);

    HMODULE selfModule = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&ModInit, &selfModule)) {
        LOG_INFO("Mod DLL base: 0x%p", selfModule);
    }

    FilesystemPatch_Init(gameModule);
    if (!FilesystemPatch_InstallHooks()) {
        LOG_WARN("Filesystem hooks failed during ModInit; file overrides will be unavailable");
    }

    LOG_INFO("Initialization deferred - will complete when game is ready...");

    // Cache autoconnect config immediately at DLL load time.
    // The test harness overwrites this file 3s later with the client config,
    // so we must snapshot it before DeferredInit (which may run after that).
    NetMenu::CacheAutoConnectFile();
}

__declspec(dllexport) void ModShutdown() {
    LOG_INFO("Mod shutdown...");

    if (g_initialized) {
        PracticeTools_Shutdown();
        SIR_Shutdown();
        Rollback::OnlineWiring_Shutdown();
        Rollback::NetplayLog_Shutdown();
        Rollback::RollbackDebug_Shutdown();
        Rollback::RollbackSession_Shutdown();
        Net::DelayPolicy_Shutdown();
        Net::SyncPolicy_Shutdown();
        Net::GameplayBridge_Shutdown();
        Net::MatchLifecycle_Shutdown();
        Net::PregameSync_Shutdown();
        NetMenu::Shutdown();
        ModeOwnership::Remove();
        Net::SpectatorClient_Shutdown();
        Net::SpectatorRuntime_Shutdown();
        PaletteAssetHook_Shutdown();
        Net::NetplayPaletteRuntime_Shutdown();
        Net::Session_Shutdown();
        Net::Transport_GlobalDeinit();
        Savestate_Shutdown();
        DetVer_Shutdown();
        FilesystemPatch_Shutdown();
        RemoveHooks();
        InputOverride_Shutdown();
        InputSystem_Shutdown();
    } else {
        FilesystemPatch_Shutdown();
        RemoveHooks();
    }

    timeEndPeriod(1);
    LogWindow_Shutdown();
}

__declspec(dllexport) void ModOnGameExit(int exitCode, const char* reason) {
    LOG_INFO("=====================================================");
    if (exitCode == 0) {
        LOG_INFO("[EXIT] Game exiting normally: %s", reason ? reason : "Unknown");
    } else {
        LOG_ERROR("[EXIT] Game CRASHED! Exit code: %d", exitCode);
        LOG_ERROR("[EXIT] Reason: %s", reason ? reason : "Unknown");
    }
    LOG_INFO("=====================================================");
    LOG_INFO("[EXIT] Cleanup complete");
}

__declspec(dllexport) void ModOnFrame() {
    if (!g_initialized) {
        DeferredInit();
    }
    if (!g_initialized) return;

    // Apply content unlocks after config.dat has been loaded
    {
        static bool s_unlockApplied = false;
        if (!s_unlockApplied) {
            uint32_t version = ReadMemory<uint32_t>(ADDR_CONFIG_VERSION);
            if (version == 258) {
                UnlockAllContent();
                s_unlockApplied = true;
            }
        }
    }

    // Update netplay menu controller (pumps session, handles input, renders menu)
    ModeOwnership::FrameUpdate();
    NetMenu::FrameUpdate();

    // Update pre-game synchronization layer
    Net::PregameSync_FrameUpdate();

    // Update match lifecycle state management
    Net::MatchLifecycle_FrameUpdate();

    // Update sync policy and delay policy
    Net::SyncPolicy_FrameUpdate();
    Net::DelayPolicy_FrameUpdate();

    // Drain queued transport events and SDL input immediately before
    // rollback/pregame decisions so gameplay uses the newest packets and
    // local sample. ENet servicing itself runs on the dedicated network thread.
    Net::Session_Update();
    Net::NetplayPaletteRuntime_FrameUpdate();
    PaletteAssetHook_FrameUpdate();

    // Process savestate hotkeys (F5 save, F6 load)
    Savestate_ProcessHotkeys();

    // Process practice mode hotkeys and state
    PracticeTools_FrameUpdate();

    // Run scripted input runner (injects overrides before SDL update)
    SIR_OnFrame();

    // Poll SDL just before rollback collects local input.
    InputSystem_Update();

    // Update online wiring (manages rollback session lifecycle)
    Rollback::OnlineWiring_FrameUpdate();
    Net::SpectatorRuntime_FrameUpdate();
    Net::SpectatorClient_FrameUpdate();

    // Drive gameplay bridge per-frame (rollback session + delay policy consumption)
    // The bridge is the single entry point for per-frame gameplay runtime.
    if (Net::GameplayBridge_IsSessionActive()) {
        Net::GameplayBridge_FrameUpdate();
        Rollback::RollbackDebug_FrameUpdate();
    }

    // Update debug info
    UpdateInputDebugInfo();

    // Log when SDL input changes
    static uint16_t prevSdlP1 = 0;
    static uint16_t prevSdlP2 = 0;

    uint16_t sdlP1 = g_inputDebug.sdlInputP1;
    uint16_t sdlP2 = g_inputDebug.sdlInputP2;

    if (sdlP1 != prevSdlP1) {
        if (g_config.verboseLogging) {
            LOG_INFO("[INPUT] SDL P1: 0x%04X -> 0x%04X (U%d D%d L%d R%d A%d B%d C%d D%d St%d Se%d)",
                prevSdlP1, sdlP1,
                (sdlP1 & INPUT_UP) ? 1 : 0, (sdlP1 & INPUT_DOWN) ? 1 : 0,
                (sdlP1 & INPUT_LEFT) ? 1 : 0, (sdlP1 & INPUT_RIGHT) ? 1 : 0,
                (sdlP1 & INPUT_A) ? 1 : 0, (sdlP1 & INPUT_B) ? 1 : 0,
                (sdlP1 & INPUT_C) ? 1 : 0, (sdlP1 & INPUT_D) ? 1 : 0,
                (sdlP1 & INPUT_START) ? 1 : 0, (sdlP1 & INPUT_SELECT) ? 1 : 0);
        }
        prevSdlP1 = sdlP1;
    }

    if (sdlP2 != prevSdlP2) {
        if (g_config.verboseLogging) {
            LOG_INFO("[INPUT] SDL P2: 0x%04X -> 0x%04X", prevSdlP2, sdlP2);
        }
        prevSdlP2 = sdlP2;
    }

    // Track game mode changes
    static int prevGameMode = -1;
    static int prevSubState = -1;
    int curGameMode = GetGameMode();
    int curSubState = GetSubstate();

    if (curGameMode != prevGameMode) {
        if (g_config.verboseLogging) {
            LOG_INFO("[STATE] Game Mode changed: %d -> %d", prevGameMode, curGameMode);
        }
        prevGameMode = curGameMode;
    }
    if (curSubState != prevSubState) {
        if (g_config.verboseLogging) {
            LOG_INFO("[STATE] Sub-State changed: %d -> %d", prevSubState, curSubState);
        }
        prevSubState = curSubState;
    }

    // Periodic status log
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 300 == 0 && g_config.verboseLogging) {
        LOG_DEBUG("[STATUS] Hooks: KB=%d(%d inj) Joy=%d(%d inj) | SDL P1=0x%04X P2=0x%04X",
            g_inputDebug.keyboardHookCalls, g_inputDebug.keyboardInjectedCount,
            g_inputDebug.joystickHookCalls, g_inputDebug.joystickInjectedCount,
            g_inputDebug.sdlInputP1, g_inputDebug.sdlInputP2);
    }

    // Determinism verification: end previous frame, begin next
    {
        static bool s_detverFrameOpen = false;
        static int  s_detverPrevFrame = -1;
        int simFrame = (int)AS2_GetFrameNumber();

        if (DetVer_IsEnabled() && AS2_IsInGameplay()) {
            if (s_detverFrameOpen) {
                DetVer_EndFrame(s_detverPrevFrame);
            }
            DetVer_BeginFrame(simFrame);
            s_detverFrameOpen = true;
            s_detverPrevFrame = simFrame;
        } else if (s_detverFrameOpen) {
            DetVer_EndFrame(s_detverPrevFrame);
            s_detverFrameOpen = false;
        }
    }

}

__declspec(dllexport) void ModOnPresent(void* pDevice) {
    if (!g_initialized) return;
    ApplySharedImGuiScale();
    HitboxViewer_Render();
    NetplayHud_Render();
    PracticeTools_RenderHUD();
    ModMenu_Render();
}

__declspec(dllexport) bool ModWantsExclusiveOverlay() {
    return false;
}

__declspec(dllexport) void ModToggleMenu() {
    ModMenu_Toggle();
}

__declspec(dllexport) bool ModGetNetplayHudText(char* out, int cap) {
    (void)out; (void)cap;
    return false;
}

__declspec(dllexport) bool ModGetMatchHudData(MatchHudData* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!g_initialized) return false;

    // Only show HUD when a session is active
    Net::SessionSnapshot sessionSnap{};
    Net::Session_GetSnapshot(&sessionSnap);
    if (!sessionSnap.active || sessionSnap.state < Net::SessionState::Connected)
        return false;

    // Show HUD during match, charsel, winscreen modes — any online phase
    Net::MatchLifecycleSnapshot lifeSnap{};
    Net::MatchLifecycle_GetSnapshot(&lifeSnap);

    bool inMatch = lifeSnap.active && lifeSnap.match_owned;
    bool rollbackActive = Net::GameplayBridge_IsSessionActive();

    // Show HUD if session is connected and either in match or rollback active
    if (!inMatch && !rollbackActive) return false;

    out->active = true;
    out->is_host = (Net::Session_GetRole() == Net::SessionRole::Host);

    // --- Names: P1 = game P1, P2 = game P2 ---
    // Get local and remote nicknames
    NetMenu::MenuSnapshot menuSnap{};
    NetMenu::GetSnapshot(&menuSnap);

    const char* localNick = menuSnap.local_nickname[0] ? menuSnap.local_nickname : "Local";
    const char* remoteNick = sessionSnap.remote_peer.nickname[0] ? sessionSnap.remote_peer.nickname : "Remote";

    int localSlot = Net::PlayerMapping_GetLocalGameSlot();
    if (localSlot == 0) {
        // Local is P1
        strncpy_s(out->p1_name, sizeof(out->p1_name), localNick, _TRUNCATE);
        strncpy_s(out->p2_name, sizeof(out->p2_name), remoteNick, _TRUNCATE);
    } else {
        // Local is P2
        strncpy_s(out->p1_name, sizeof(out->p1_name), remoteNick, _TRUNCATE);
        strncpy_s(out->p2_name, sizeof(out->p2_name), localNick, _TRUNCATE);
    }

    // --- Win counts: game-side P1/P2 ---
    Net::SetTracker_GetGameSideWins(&out->p1_wins, &out->p2_wins);

    // --- Ping: from session stats ---
    out->ping_ms = sessionSnap.stats.rtt_ms;

    // --- Delay and rollback: from rollback session if active ---
    if (rollbackActive) {
        Rollback::RollbackSessionSnapshot rbSnap{};
        Rollback::RollbackSession_GetSnapshot(&rbSnap);
        out->delay_frames = Net::DelayPolicy_GetActiveDelay();
        out->rollback_frames = rbSnap.rollback_budget;
        out->local_frame = rbSnap.rb_frame_current;
        out->remote_frame = rbSnap.rb_frame_last_remote_received;
    } else {
        // Pre-match: use delay policy values
        out->delay_frames = Net::DelayPolicy_GetActiveDelay();
        out->rollback_frames = 0;
        out->local_frame = 0;
        out->remote_frame = 0;
    }

    return true;
}

__declspec(dllexport) uint32_t GetCurrentFrame() {
    return AS2_GetFrameNumber();
}

__declspec(dllexport) uint16_t GetPlayerHP(int player) {
    return player == 0 ? GetP1HP() : GetP2HP();
}

__declspec(dllexport) bool* GetForceBorderlessPtr() {
    return &g_forceBorderlessFullscreen;
}

} // extern "C"
