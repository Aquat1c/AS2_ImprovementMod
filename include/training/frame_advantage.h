#pragma once

#include "training/native_recovery.h"

#include <stdbool.h>
#include <stdint.h>

// Lifecycle
void FrameAdvantage_Init(void);
void FrameAdvantage_Shutdown(void);

// Enable/disable
void FrameAdvantage_SetEnabled(bool enabled);
bool FrameAdvantage_IsEnabled(void);

// Called after a gameplay simulation tick has fully completed. Contact and
// attack-window bookkeeping only; recovery timestamps come from the
// pre-command-dispatch events below.
void FrameAdvantage_OnFrameAdvanced(uint32_t simFrame);

// Observed at Entity_ProcessCommandMatches entry, which is the exact tick on
// which the engine will consider an ordinary neutral input. This is the only
// production source of recovery timestamps: an action-ID edge is one tick late
// because the queued neutral handoff is applied later in the tick, and a
// post-tick route profile describes a tick that has not run.
void FrameAdvantage_OnPreCommandDispatch(int player,
                                         uint32_t simFrame,
                                         const Training::NativeRecoverySample& sample,
                                         const Training::NativeRecoveryResult& result);

// Audit only: did a real input replace the pending terminal handoff this tick?
void FrameAdvantage_OnPostCommandDispatch(int player,
                                          uint32_t simFrame,
                                          uint32_t pending1,
                                          uint32_t pending2);

// In-game HUD overlay.
void FrameAdvantage_RenderOverlay(void);
bool FrameAdvantage_HasVisibleOverlay(void);

// State management.
void FrameAdvantage_ResetState(void);
void FrameAdvantage_ClearDisplay(void);
void FrameAdvantage_CancelCalculation(void);

// ImGui controls (for Practice tab).
void FrameAdvantage_RenderImGui(void);