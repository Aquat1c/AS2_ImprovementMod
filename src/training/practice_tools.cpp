/**
 * Alice Senki 2 - Practice Mode Tools
 *
 * Pause/unpause, single-frame advance, controller swap with CPU flag
 * management, macro recording, and shared HUD helpers.
 *
 * Practice runtime, UI, trigger, and value-editor logic live in
 * practice_runtime.cpp to keep this file focused on shared state and
 * the small public surface that other systems call every frame.
 */

#include "training/practice_tools.h"

#include "practice_internal.h"

#include "training/frame_advantage.h"
#include "training/input_macro.h"
#include "patches/memory_utils.h"
#include "input_system.h"
#include "as2_constants.h"
#include "log_window.h"

#include <stdint.h>
#include <stdio.h>

namespace PracticeInternal {

bool g_initialized = false;
bool g_paused = false;
bool g_stepRequested = false;
int g_stepCounter = 0;
bool g_wasActive = false;
Toast g_toasts[TOAST_MAX] = {};
int g_toastCount = 0;

void PushToast(const char* text, ImU32 color) {
    if (g_toastCount >= TOAST_MAX) {
        for (int i = 0; i < TOAST_MAX - 1; i++) {
            g_toasts[i] = g_toasts[i + 1];
        }
        g_toastCount = TOAST_MAX - 1;
    }

    Toast& toast = g_toasts[g_toastCount++];
    snprintf(toast.text, sizeof(toast.text), "%s", text);
    toast.remaining = TOAST_DURATION;
    toast.color = color;
}

void UpdateToasts(float dt) {
    int write = 0;
    for (int i = 0; i < g_toastCount; i++) {
        g_toasts[i].remaining -= dt;
        if (g_toasts[i].remaining > 0.0f) {
            if (write != i) {
                g_toasts[write] = g_toasts[i];
            }
            write++;
        }
    }
    g_toastCount = write;
}

} // namespace PracticeInternal

using PracticeInternal::g_initialized;
using PracticeInternal::g_paused;
using PracticeInternal::g_stepCounter;
using PracticeInternal::g_stepRequested;
using PracticeInternal::IsPracticeModeNow;
using PracticeInternal::PushToast;

void PracticeTools_SetPaused(bool paused) {
    if (!g_initialized || g_paused == paused) {
        return;
    }

    g_paused = paused;
    g_stepRequested = false;
    g_stepCounter = 0;

    if (IsPracticeModeNow()) {
        PushToast(g_paused ? "PAUSED" : "UNPAUSED",
                  g_paused ? IM_COL32(255, 255, 100, 255) : IM_COL32(100, 255, 100, 255));
        LOG_INFO("[Practice] %s", g_paused ? "PAUSED" : "UNPAUSED");
    }
}

void PracticeTools_CaptureRuntimeState(PracticeToolsRuntimeState* out) {
    if (!out) {
        return;
    }

    out->paused = g_paused;
    out->stepRequested = g_stepRequested;
    out->stepCounter = (uint32_t)g_stepCounter;
}

bool PracticeTools_ShouldFreezeFrame() {
    if (!g_initialized || !g_paused) return false;
    if (!IsPracticeModeNow()) return false;
    if (g_stepRequested) {
        return false;
    }
    return true;
}

void PracticeTools_OnFrameAdvanced() {
    FrameAdvantage_OnFrameAdvanced(ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER));
    InputMacro_Tick();
    g_stepRequested = false;
}

bool PracticeTools_IsPracticeModeActive() {
    return g_initialized && IsPracticeModeNow();
}

bool PracticeTools_IsPaused() {
    return g_paused;
}

bool PracticeTools_IsControlSwapped() {
    return InputSystem_GetControlSwap();
}

void PracticeTools_Toast(const char* text, unsigned int color) {
    PushToast(text, (ImU32)color);
}

CmdHistoryUpdate_t g_origCmdHistoryUpdate = nullptr;

char __cdecl Hook_CmdHistoryUpdate(int16_t* matchBase) {
    if (IsPracticeModeNow() && InputSystem_GetControlSwap()) {
        matchBase = reinterpret_cast<int16_t*>(
            reinterpret_cast<uint8_t*>(matchBase) + CHARACTER_STRUCT_SIZE);
    }
    return g_origCmdHistoryUpdate ? g_origCmdHistoryUpdate(matchBase) : 0;
}