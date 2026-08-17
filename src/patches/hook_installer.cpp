#include "patches/hook_installer.h"
#include "patches/frame_scheduler.h"
#include "patches/input_override.h"
#include "patches/input_sync_hooks.h"
#include "patches/tick_hooks.h"
#include "patches/locale_patch.h"
#include "patches/filesystem_patch.h"
#include "patches/palette_asset_hook.h"
#include "patches/charsel_palette_select.h"
#include "patches/charsel_select_actions.h"
#include "patches/render_guard.h"
#include "patches/session_pump_hook.h"
#include "patches/netplay_background_run.h"
#include "patches/shell_hotkey_patch.h"
#include "ui/netplay_hud_vanilla.h"
#include "replay/replay_runtime.h"
#include "rollback/rollback_audio.h"
#include "rollback/rollback_combo_fx.h"
#include "rollback/rollback_status_fx.h"
#include "training/practice_tools.h"
#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"

bool InstallHooks() {
    LOG_INFO("Installing hooks...");
    
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("MH_Initialize failed! Status: %d", status);
        return false;
    }
    
    // --- Input hooks ---
    
    LOG_INFO("ADDR_KEYBOARD_STATE = 0x%08X (sub_62FD00)", ADDR_KEYBOARD_STATE);
    LOG_INFO("ADDR_JOYSTICK_STATE = 0x%08X (sub_62FF50)", ADDR_JOYSTICK_STATE);

    const bool enableShellHotkeyImeWorkarounds = InputOverride_AreShellHotkeyImeWorkaroundsEnabled();
    const bool enableSystemKeyWorkarounds = InputOverride_AreSystemKeyWorkaroundsEnabled();
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_KEYBOARD_STATE),
            reinterpret_cast<void*>(&Hook_KeyboardState),
            reinterpret_cast<void**>(&g_origKeyboardState));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook KeyboardState! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_62FD00 (keyboard state)");
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_JOYSTICK_STATE),
            reinterpret_cast<void*>(&Hook_JoystickState),
            reinterpret_cast<void**>(&g_origJoystickState));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook JoystickState! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_62FF50 (joystick state)");
    
    LOG_INFO("ADDR_DINPUT_KB_REFRESH = 0x%08X (sub_630130)", ADDR_DINPUT_KB_REFRESH);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_DINPUT_KB_REFRESH),
            reinterpret_cast<void*>(&Hook_DInputKBRefresh),
            reinterpret_cast<void**>(&g_origDInputKBRefresh));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DInputKBRefresh! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_630130 (DirectInput keyboard buffer refresh)");
    }
    
    LOG_INFO("ADDR_DINPUT_JOY_REFRESH = 0x%08X (sub_6302F0)", ADDR_DINPUT_JOY_REFRESH);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_DINPUT_JOY_REFRESH),
            reinterpret_cast<void*>(&Hook_DInputJoyRefresh),
            reinterpret_cast<void**>(&g_origDInputJoyRefresh));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook DInputJoyRefresh! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_6302F0 (DirectInput joystick buffer refresh)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetKeyboardState),
            reinterpret_cast<void*>(&Hook_GetKeyboardState),
            reinterpret_cast<void**>(&g_origGetKeyboardState));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetKeyboardState! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetKeyboardState (keyboard input mediation)");
    }

    status = MH_CreateHook(
            reinterpret_cast<void*>(&ClipCursor),
            reinterpret_cast<void*>(&Hook_ClipCursor),
            reinterpret_cast<void**>(&g_origClipCursor));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook ClipCursor! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 ClipCursor (prevents mouse trapping)");
    }

    if (enableShellHotkeyImeWorkarounds) {
        status = MH_CreateHook(
                reinterpret_cast<void*>(&GetProcAddress),
                reinterpret_cast<void*>(&Hook_GetProcAddress),
                reinterpret_cast<void**>(&g_origGetProcAddress));
        if (status != MH_OK) {
            LOG_WARN("Failed to hook GetProcAddress! Status: %d (continuing anyway)", status);
        } else {
            LOG_INFO("Hooked Win32 GetProcAddress (shell hotkey helper interception)");
        }

        status = MH_CreateHook(
                reinterpret_cast<void*>(&SystemParametersInfoA),
                reinterpret_cast<void*>(&Hook_SystemParametersInfoA),
                reinterpret_cast<void**>(&g_origSystemParametersInfoA));
        if (status != MH_OK) {
            LOG_WARN("Failed to hook SystemParametersInfoA! Status: %d (continuing anyway)", status);
        } else {
            LOG_INFO("Hooked Win32 SystemParametersInfoA (shell hotkey interception)");
        }

        HMODULE user32 = GetModuleHandleA("user32.dll");
        FARPROC winNlsEnableIme = user32 ? GetProcAddress(user32, "WINNLSEnableIME") : nullptr;
        if (!winNlsEnableIme) {
            LOG_WARN("Failed to resolve WINNLSEnableIME from user32.dll (continuing anyway)");
        } else {
            status = MH_CreateHook(
                    reinterpret_cast<void*>(winNlsEnableIme),
                    reinterpret_cast<void*>(&Hook_WINNLSEnableIME),
                    reinterpret_cast<void**>(&g_origWINNLSEnableIME));
            if (status != MH_OK) {
                LOG_WARN("Failed to hook WINNLSEnableIME! Status: %d (continuing anyway)", status);
            } else {
                LOG_INFO("Hooked Win32 WINNLSEnableIME (IME interception)");
            }
        }
    } else {
        LOG_INFO("Shell hotkey/IME workarounds disabled - skipping GetProcAddress/SystemParametersInfoA/WINNLSEnableIME hooks");
    }

    if (enableShellHotkeyImeWorkarounds) {
        void* dinputSetCooperativeLevelTarget = InputOverride_GetDInputKeyboardSetCooperativeLevelTarget();
        if (!dinputSetCooperativeLevelTarget) {
            LOG_WARN("Failed to locate DInput keyboard SetCooperativeLevel (continuing anyway)");
        } else {
            status = MH_CreateHook(
                    dinputSetCooperativeLevelTarget,
                    reinterpret_cast<void*>(&Hook_DInputKeyboardSetCooperativeLevel),
                    reinterpret_cast<void**>(&g_origDInputKeyboardSetCooperativeLevel));
            if (status != MH_OK) {
                LOG_WARN("Failed to hook DInput keyboard SetCooperativeLevel! Status: %d (continuing anyway)", status);
            } else {
                LOG_INFO("Hooked DInput keyboard SetCooperativeLevel (shell/layout workaround)");
            }
        }
    } else {
        LOG_INFO("Shell/layout DInput cooperative-level workaround disabled - skipping SetCooperativeLevel hook");
    }
    
    LOG_INFO("ADDR_INPUT_PROCESS = 0x%08X (sub_562060)", ADDR_INPUT_PROCESS);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_INPUT_PROCESS),
            reinterpret_cast<void*>(&Hook_InputProcess),
            reinterpret_cast<void**>(&g_origInputProcess));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook InputProcess! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_562060 (input processing - just pressed flags)");
    
    // --- Input dispatcher hook (charsel lockstep) ---
    
    LOG_INFO("ADDR_INPUT_DISPATCHER = 0x%08X (sub_5625E0)", ADDR_INPUT_DISPATCHER);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_INPUT_DISPATCHER),
            reinterpret_cast<void*>(&Hook_InputDispatcher),
            reinterpret_cast<void**>(&g_origInputDispatcher));
    if (status != MH_OK) {
        LOG_ERROR("Failed to hook InputDispatcher! Status: %d", status);
        return false;
    }
    LOG_INFO("Hooked sub_5625E0 (input dispatcher - charsel lockstep)");

    // --- Rollback presentation sidecar hooks ---
    // These are non-fatal: audio/status/combo presentation remains playable
    // without them, but rollback-corrected presentation can show stale effects.

    LOG_INFO("ADDR_SE_PLAY = 0x%08X (sub_4C3C00)", ADDR_SE_PLAY);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_SE_PLAY),
            reinterpret_cast<void*>(&Rollback::Hook_SE_Play),
            reinterpret_cast<void**>(&Rollback::g_origSEPlay));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook SE_Play! Status: %d (rollback audio disabled)", status);
    } else {
        LOG_INFO("Hooked sub_4C3C00 (SE_Play - rollback-aware audio journal)");
    }

    // Audio_IsPlaying: the simulation branches on live DirectSound state
    // (Entity_UpdateAudio plays a voice and updates CAPTURED bookkeeping only
    // when it returns false). Recorded on truth ticks and replayed during
    // rollback so both take the same branch — qoh99's hkSoundStatus pattern.
    LOG_INFO("ADDR_AUDIO_IS_PLAYING = 0x%08X (Audio_IsPlaying)", ADDR_AUDIO_IS_PLAYING);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_AUDIO_IS_PLAYING),
            reinterpret_cast<void*>(&Rollback::Hook_Audio_IsPlaying),
            reinterpret_cast<void**>(&Rollback::g_origAudioIsPlaying));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook Audio_IsPlaying! Status: %d (audio status not replayed)", status);
    } else {
        LOG_INFO("Hooked Audio_IsPlaying (record/replay for rollback determinism)");
    }

    LOG_INFO("ADDR_AUDIO_PLAY_VOICE = 0x%08X (Audio_Play_Wrapper)", ADDR_AUDIO_PLAY_VOICE);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_AUDIO_PLAY_VOICE),
            reinterpret_cast<void*>(&Rollback::Hook_Audio_Play_Wrapper),
            reinterpret_cast<void**>(&Rollback::g_origAudioPlayWrapper));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook Audio_Play_Wrapper! Status: %d", status);
    } else {
        LOG_INFO("Hooked Audio_Play_Wrapper (canonical voice-start stamps)");
    }

    LOG_INFO("ADDR_EFFECT_SPAWN = 0x%08X (sub_4A92C0 Effect_Enqueue)", ADDR_EFFECT_SPAWN);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_EFFECT_SPAWN),
            reinterpret_cast<void*>(&Rollback::Hook_Effect_Enqueue),
            reinterpret_cast<void**>(&Rollback::g_origEffectEnqueue));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook Effect_Enqueue! Status: %d (status FX correction disabled)", status);
    } else {
        LOG_INFO("Hooked sub_4A92C0 (Effect_Enqueue - rollback status FX journal)");
    }

    LOG_INFO("ADDR_EFFECT_DRAW = 0x%08X (sub_4AB0F0 Effect_DrawQueue)", ADDR_EFFECT_DRAW);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_EFFECT_DRAW),
            reinterpret_cast<void*>(&Rollback::Hook_Effect_DrawQueue),
            reinterpret_cast<void**>(&Rollback::g_origEffectDrawQueue));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook Effect_DrawQueue! Status: %d (status FX draw filter disabled)", status);
    } else {
        LOG_INFO("Hooked sub_4AB0F0 (Effect_DrawQueue - rollback status FX draw filter)");
    }

    struct PresentationHookEntry {
        uintptr_t target;
        void* detour;
        void** original;
        const char* name;
    };

    PresentationHookEntry presentationHooks[] = {
        { ADDR_MATCH_RENDER_PLAYERS,           (void*)&RenderGuard::Hook_MatchRenderPlayers,        (void**)&RenderGuard::g_origMatchRenderPlayers,        "Match_RenderPlayers" },
        { ADDR_MATCH_UPDATE_COMBO_TIMERS,      (void*)&Rollback::Hook_Match_UpdateComboTimers,      (void**)&Rollback::g_origMatchUpdateComboTimers,      "Match_UpdateComboTimers" },
        { ADDR_ENTITY_UPDATE_COMBO_STATS,      (void*)&Rollback::Hook_Entity_UpdateComboStats,      (void**)&Rollback::g_origEntityUpdateComboStats,      "Entity_UpdateComboStats" },
        { ADDR_ENTITY_UPDATE_COMBO_STAT_1243,  (void*)&Rollback::Hook_Entity_UpdateComboStat_1243,  (void**)&Rollback::g_origEntityUpdateComboStat1243,  "Entity_UpdateComboStat_1243" },
        { ADDR_ENTITY_EFFECT_SLOTS_ADD,        (void*)&Rollback::Hook_EffectSlots_Add,              (void**)&Rollback::g_origEffectSlotsAdd,              "EffectSlots_Add" },
        { ADDR_ENTITY_EFFECT_SET_PARAMS1,      (void*)&Rollback::Hook_Effect_SetParams1,            (void**)&Rollback::g_origEffectSetParams1,            "Effect_SetParams1" },
        { ADDR_ENTITY_EFFECT_SET_PARAMS2,      (void*)&Rollback::Hook_Effect_SetParams2,            (void**)&Rollback::g_origEffectSetParams2,            "Effect_SetParams2" },
    };

    for (auto& h : presentationHooks) {
        status = MH_CreateHook(reinterpret_cast<void*>(h.target), h.detour, h.original);
        if (status != MH_OK) {
            LOG_WARN("Failed to hook %s @ 0x%08X! Status: %d (combo FX state monitor degraded)",
                     h.name,
                     (unsigned)h.target,
                     status);
        } else {
            LOG_INFO("Hooked %s @ 0x%08X (rollback combo FX state monitor)",
                     h.name,
                     (unsigned)h.target);
        }
    }

    LOG_INFO("ADDR_ASSET_LOAD_FROM_ARCHIVE = 0x%08X (sub_4A5390)", ADDR_ASSET_LOAD_FROM_ARCHIVE);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_ASSET_LOAD_FROM_ARCHIVE),
            reinterpret_cast<void*>(&Net::Hook_Asset_LoadFromArchive),
            reinterpret_cast<void**>(&Net::g_origAssetLoadFromArchive));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook Asset_LoadFromArchive! Status: %d (win-screen stall pump disabled)", status);
    } else {
        LOG_INFO("Hooked sub_4A5390 (Asset_LoadFromArchive - session pump during blocking loads)");
    }
    
    // --- Locale hooks ---
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetOEMCP),
            reinterpret_cast<void*>(&Hook_GetOEMCP),
            reinterpret_cast<void**>(&g_origGetOEMCP));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetOEMCP! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetOEMCP (Japanese locale patch - returns 932)");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetACP),
            reinterpret_cast<void*>(&Hook_GetACP),
            reinterpret_cast<void**>(&g_origGetACP));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetACP! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked Win32 GetACP (Japanese locale patch - returns 932)");
    }
    
    // --- Filesystem hooks ---

    if (!FilesystemPatch_InstallHooks()) {
        LOG_WARN("Failed to install filesystem hooks! File overrides may not work correctly");
    }
    
    status = MH_CreateHook(
            reinterpret_cast<void*>(&MultiByteToWideChar),
            reinterpret_cast<void*>(&Hook_MultiByteToWideChar),
            reinterpret_cast<void**>(&g_origMultiByteToWideChar));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook MultiByteToWideChar! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked MultiByteToWideChar (CP_ACP/CP_OEMCP -> CP932 redirect)");
    }
    
    // --- Tick/timing hook ---
    
    LOG_INFO("ADDR_GET_TICK = 0x%08X (sub_635F80)", ADDR_GET_TICK);
    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_GET_TICK),
            reinterpret_cast<void*>(&Hook_GetTick),
            reinterpret_cast<void**>(&g_origGetTick));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook GetTick (sub_635F80)! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_635F80 (tick/time source)");
    }

    // --- Command history hook (practice mode: redirect to P2 when swapped) ---

    status = MH_CreateHook(
            reinterpret_cast<void*>(ADDR_CMD_HISTORY_UPDATE),
            reinterpret_cast<void*>(&Hook_CmdHistoryUpdate),
            reinterpret_cast<void**>(&g_origCmdHistoryUpdate));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook CmdHistoryUpdate! Status: %d (continuing anyway)", status);
    } else {
        LOG_INFO("Hooked sub_4C8C50 (command history update - practice swap redirect)");
    }

    if (!PaletteAssetHook_Install()) {
        LOG_WARN("Failed to install character palette asset hook (continuing anyway)");
    }

    if (!Net::CharSelPaletteSelect_Install()) {
        LOG_WARN("Failed to install char-select palette flow hooks (continuing anyway)");
    }

    if (!Net::CharSelSelectActions_Install()) {
        LOG_WARN("Failed to install char/stage select action patch (continuing anyway)");
    }

    if (!Replay::ReplayRuntime_InstallHooks()) {
        LOG_WARN("Failed to install replay save hook (continuing anyway)");
    }

    if (!ShellHotkeyPatch_Install()) {
        LOG_WARN("Failed to install game wndproc shell hotkey patch (continuing anyway)");
    }

    if (!NetplayBackgroundRun::InstallHook()) {
        LOG_WARN("Failed to install netplay inactive-window bypass hook (continuing anyway)");
    }

    if (!NetplayHudVanilla_InstallHooks()) {
        LOG_WARN("Failed to install vanilla netplay HUD hook (continuing anyway)");
    }
    
    // --- Enable all hooks ---
    
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        LOG_ERROR("MH_EnableHook failed! Status: %d", status);
        return false;
    }
    
    LOG_INFO("Input hooks installed and enabled!");

    InputOverride_EnsureDInputKeyboardCooperativeLevel("hook activation");

    if (g_origClipCursor) {
        g_origClipCursor(nullptr);
        LOG_INFO("Released any existing cursor clip after hook activation");
    }

    // --- Vanilla netplay suppression hooks ---
    // These must be installed AFTER MH_EnableHook(MH_ALL_HOOKS) since
    // InputSyncHooks_Install creates + enables its own hooks.
    if (!InputSyncHooks_Install()) {
        LOG_WARN("Failed to install vanilla netplay suppression hooks (continuing anyway)");
    }

    // --- Frame limiter detour (re0.7 M2, plan §2.8) ---
    // Byte-signature scan of the 17 ms busy-spin cluster in Game_MainLoop and
    // detour to the mod-owned FrameScheduler. On failure the legacy pinned
    // Hook_GetTick virtual-clock limiter stays active (risk R-1 fallback) —
    // loud, but not fatal.
    if (!FrameScheduler_Install()) {
        LOG_WARN("FrameScheduler limiter detour NOT installed — running on the "
                 "legacy virtual-clock frame limiter (R-1 fallback)");
    }

    return true;
}

void RemoveHooks() {
    LOG_INFO("Removing hooks...");
    FrameScheduler_Shutdown();
    NetplayBackgroundRun::Shutdown();
    ShellHotkeyPatch_Remove();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
