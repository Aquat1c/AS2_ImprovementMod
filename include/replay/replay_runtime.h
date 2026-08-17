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
bool ReplayRuntime_HasVisibleHud();

bool ReplayRuntime_ShouldFreezeFrame();
bool ReplayRuntime_IsReplayMatchActive();
bool ReplayRuntime_IsReplayMenuActive();
bool ReplayRuntime_ShouldConsumeMenuInput();
void ReplayRuntime_OnFrontendInputsProcessed();
bool ReplayRuntime_CopyPaletteOverrideBank(uint8_t gameSlot, Net::NetplayPaletteBank* out);
void ReplayRuntime_OnDispatcherAdvance(int16_t* outputInputs);

/// re0.7 M7 (S-5): confirmed-stream recorder feed. Called from the engine2
/// confirm seam for every CONFIRMED gameplay frame (a predicted value can
/// never reach this — plan §2.7.3-F). `epoch` changing starts a new chapter
/// (the recorder holds exactly one epoch — one match — at a time; the native
/// one-file-per-match replay save consumes it). `game_abs_frame` is in the
/// game's own per-match frame-counter domain, i.e. the native replay tape
/// index space. `pre_state_hash` is the Block64 confirmed pre-tick gameplay
/// digest; the recorder keeps one every 30 frames for playback verification.
void ReplayRuntime_OnConfirmedFrame(uint32_t epoch,
                                    int32_t game_abs_frame,
                                    uint16_t p1_input,
                                    uint16_t p2_input,
                                    uint64_t pre_state_hash);

} // namespace Replay
