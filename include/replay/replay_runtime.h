#pragma once

#include <stdint.h>

namespace Net {
struct NetplayPaletteBank;
}

namespace Replay {

enum class TakeoverMode : uint8_t {
    None = 0,
    P1,
    P2,
    Both,
};

bool ReplayRuntime_InstallHooks();
void ReplayRuntime_Init();
void ReplayRuntime_Shutdown();
void ReplayRuntime_FrameUpdate();
void ReplayRuntime_RenderHUD();

bool ReplayRuntime_ShouldFreezeFrame();
bool ReplayRuntime_IsReplayMatchActive();
bool ReplayRuntime_IsReplayMenuActive();
bool ReplayRuntime_ShouldConsumeMenuInput();
void ReplayRuntime_OnFrontendInputsProcessed();
bool ReplayRuntime_CopyPaletteOverrideBank(uint8_t gameSlot, Net::NetplayPaletteBank* out);
void ReplayRuntime_OnDispatcherAdvance(int16_t* outputInputs);

} // namespace Replay