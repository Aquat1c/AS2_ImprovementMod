#pragma once

#include <stdbool.h>
#include <stdint.h>

// Lifecycle
void FrameAdvantage_Init(void);
void FrameAdvantage_Shutdown(void);

// Enable/disable
void FrameAdvantage_SetEnabled(bool enabled);
bool FrameAdvantage_IsEnabled(void);

// Called after a gameplay simulation tick has fully completed.
void FrameAdvantage_OnFrameAdvanced(uint32_t simFrame);

// In-game HUD overlay.
void FrameAdvantage_RenderOverlay(void);
bool FrameAdvantage_HasVisibleOverlay(void);

// State management.
void FrameAdvantage_ResetState(void);
void FrameAdvantage_ClearDisplay(void);
void FrameAdvantage_CancelCalculation(void);

// ImGui controls (for Practice tab).
void FrameAdvantage_RenderImGui(void);