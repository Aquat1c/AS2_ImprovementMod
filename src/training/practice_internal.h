#pragma once

#include "training/practice_tools.h"

#include "core/game_state.h"
#include "imgui.h"

#include <stdint.h>

namespace PracticeInternal {

constexpr int TOAST_MAX = 4;
constexpr float TOAST_DURATION = 1.5f;
constexpr float TOAST_FADE_START = 1.0f;

struct Toast {
    char text[64];
    float remaining;
    ImU32 color;
};

extern bool g_initialized;
extern bool g_paused;
extern bool g_stepRequested;
extern int g_stepCounter;
extern bool g_wasActive;
extern Toast g_toasts[TOAST_MAX];
extern int g_toastCount;

void PushToast(const char* text, ImU32 color = IM_COL32(255, 255, 255, 255));
void UpdateToasts(float dt);

inline bool IsPracticeModeNow() {
    return GetGameType() == GAMETYPE_TRAINING && GetGameMode() == MODE_MATCH;
}

} // namespace PracticeInternal