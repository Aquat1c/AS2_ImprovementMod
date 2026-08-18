/**
 * Netplay HUD appearance settings — local persistence and wire sync.
 */
#pragma once

#include <stdint.h>

namespace NetplayHudStyle {

enum class HudVerticalPosition : uint8_t {
    MenuSafe = 0, // y=85 — clears F1 mod menu overlap
    Top = 1,      // y=2 — screen top edge, above HP bars
};

enum class HudFontSize : uint8_t {
    Small = 0,
    Normal = 1,
    Large = 2,
};

// Local-only: ImGui overlay (default) or game's native nickname draw path.
enum class HudRenderMode : uint8_t {
    ImGui = 0,
    Vanilla = 1,
};

struct Settings {
    uint8_t trail_r;
    uint8_t trail_g;
    uint8_t trail_b;
    uint8_t text_r;
    uint8_t text_g;
    uint8_t text_b;
    uint8_t score_r;
    uint8_t score_g;
    uint8_t score_b;
    uint16_t trail_length_px;
    uint8_t vertical_position;
    uint8_t font_size;
    uint8_t render_mode;
};

struct WireStyle {
    uint8_t trail_r;
    uint8_t trail_g;
    uint8_t trail_b;
    uint8_t text_r;
    uint8_t text_g;
    uint8_t text_b;
    uint8_t trail_length;
    uint8_t score_r;
    uint8_t score_g;
    uint8_t score_b;
    uint8_t font_size;
};

struct ResolvedSideStyle {
    uint8_t trail_r;
    uint8_t trail_g;
    uint8_t trail_b;
    uint8_t text_r;
    uint8_t text_g;
    uint8_t text_b;
    uint8_t score_r;
    uint8_t score_g;
    uint8_t score_b;
    uint16_t trail_length_px;
    uint8_t font_size;
};

void Init();
void Load();
void SetDefaults(Settings* out);

void GetLocal(Settings* out);
void SetLocal(const Settings* settings);

void CycleTrailPreset(int delta);
void CycleTextPreset(int delta);
void CycleScorePreset(int delta);
void AdjustTrailLength(int deltaPixels);
void CycleVerticalPosition(int delta);
void CycleFontSize(int delta);
void CycleRenderMode(int delta);

const char* GetTrailPresetLabel();
const char* GetTextPresetLabel();
const char* GetScorePresetLabel();
const char* GetVerticalPositionLabel();
const char* GetFontSizeLabel();
const char* GetRenderModeLabel();

HudRenderMode GetRenderMode();
float GetNickYRatio(bool modMenuOpen);

/// Fixed top row, used by character select and the win screen.
float GetNickYTopRatio();
float GetNickYRatioForSide(uint8_t vertical_position, bool modMenuOpen);
float GetFontSizePx();
float GetFontSizePxForPreset(uint8_t font_size);
int GetFontSizePresetIndex();

void PackWire(WireStyle* out);
void UnpackWire(const WireStyle* wire, Settings* out);

void ResolveSideStyles(int localGameSlot,
                       bool remoteStyleValid,
                       const Settings* remoteStyle,
                       ResolvedSideStyle* outP1,
                       ResolvedSideStyle* outP2);

} // namespace NetplayHudStyle
