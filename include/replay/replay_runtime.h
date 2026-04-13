#pragma once

#include <stdint.h>

namespace Replay {

enum class TakeoverMode : uint8_t {
    None = 0,
    P1,
    P2,
    Both,
};

void ReplayRuntime_Init();
void ReplayRuntime_Shutdown();
void ReplayRuntime_FrameUpdate();
void ReplayRuntime_RenderHUD();

bool ReplayRuntime_ShouldFreezeFrame();
bool ReplayRuntime_IsReplayMatchActive();
bool ReplayRuntime_IsReplayMenuActive();
bool ReplayRuntime_ShouldConsumeMenuInput();
void ReplayRuntime_OnDispatcherAdvance(int16_t* outputInputs);

} // namespace Replay