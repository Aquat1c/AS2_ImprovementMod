#include "ui/netplay_hud_style.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace NetplayHudStyle {

namespace {

static const char* kConfigFile = "as2_netplay.cfg";

static Settings s_local = {};
static bool s_loaded = false;

struct ColorPreset {
    const char* label;
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

static constexpr ColorPreset kTrailPresets[] = {
    {"Blue",   20,  60, 120},
    {"Red",   120,  20,  30},
    {"Green",  20, 100,  40},
    {"Gold",  180, 140,  30},
    {"Purple", 90,  30, 120},
    {"Cyan",   20, 120, 140},
    {"Orange",180,  80,  20},
    {"White", 200, 200, 200},
};

static constexpr ColorPreset kTextPresets[] = {
    {"White",  255, 255, 255},
    {"Gold",   255, 232, 160},
    {"Silver", 210, 210, 220},
    {"Cyan",   180, 240, 255},
};

static constexpr ColorPreset kScorePresets[] = {
    {"Gold",   255, 232, 160},
    {"White",  255, 255, 255},
    {"Silver", 200, 200, 210},
    {"Amber",  255, 200,  80},
};

static constexpr uint16_t kTrailLengthMin = 64;
static constexpr uint16_t kTrailLengthMax = 320;
static constexpr uint16_t kTrailLengthStep = 16;
static constexpr uint16_t kTrailLengthDefault = 160;

static constexpr float kNativeHeight = 480.0f;
static constexpr float kNickYMenuSafeRatio = 85.0f / kNativeHeight;
static constexpr float kNickYTopRatio = 2.0f / kNativeHeight;

static int FindPresetIndex(const ColorPreset* presets, size_t count, uint8_t r, uint8_t g, uint8_t b) {
    for (size_t i = 0; i < count; ++i) {
        if (presets[i].r == r && presets[i].g == g && presets[i].b == b) {
            return (int)i;
        }
    }
    return 0;
}

static void ApplyPreset(const ColorPreset& preset, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = preset.r;
    *g = preset.g;
    *b = preset.b;
}

static void CyclePreset(const ColorPreset* presets,
                        size_t count,
                        int delta,
                        uint8_t* r,
                        uint8_t* g,
                        uint8_t* b) {
    int index = FindPresetIndex(presets, count, *r, *g, *b);
    index = (index + delta) % (int)count;
    if (index < 0) {
        index += (int)count;
    }
    ApplyPreset(presets[index], r, g, b);
}

static uint8_t EncodeTrailLength(uint16_t px) {
    const float t = (float)(px - kTrailLengthMin) / (float)(kTrailLengthMax - kTrailLengthMin);
    const float clamped = (std::max)(0.0f, (std::min)(1.0f, t));
    return (uint8_t)(clamped * 255.0f + 0.5f);
}

static uint16_t DecodeTrailLength(uint8_t wire) {
    const float t = (float)wire / 255.0f;
    const uint16_t px = (uint16_t)(kTrailLengthMin + t * (kTrailLengthMax - kTrailLengthMin) + 0.5f);
    return (uint16_t)(std::min<uint16_t>(kTrailLengthMax, (std::max<uint16_t>(kTrailLengthMin, px))));
}

static void ClampSettings(Settings* settings) {
    if (!settings) {
        return;
    }
    if (settings->trail_length_px < kTrailLengthMin) {
        settings->trail_length_px = kTrailLengthMin;
    }
    if (settings->trail_length_px > kTrailLengthMax) {
        settings->trail_length_px = kTrailLengthMax;
    }
}

static void ParseConfigLine(const char* key, const char* val, Settings* settings) {
    if (!key || !val || !settings) {
        return;
    }

    const int value = atoi(val);
    if (_stricmp(key, "hud_trail_r") == 0) {
        settings->trail_r = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_trail_g") == 0) {
        settings->trail_g = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_trail_b") == 0) {
        settings->trail_b = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_text_r") == 0) {
        settings->text_r = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_text_g") == 0) {
        settings->text_g = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_text_b") == 0) {
        settings->text_b = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_score_r") == 0) {
        settings->score_r = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_score_g") == 0) {
        settings->score_g = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_score_b") == 0) {
        settings->score_b = (uint8_t)(std::max)(0, (std::min)(255, value));
    } else if (_stricmp(key, "hud_trail_length") == 0) {
        if (value >= (int)kTrailLengthMin && value <= (int)kTrailLengthMax) {
            settings->trail_length_px = (uint16_t)value;
        }
    } else if (_stricmp(key, "hud_vertical_position") == 0) {
        if (_stricmp(val, "top") == 0) {
            settings->vertical_position = (uint8_t)HudVerticalPosition::Top;
        } else {
            settings->vertical_position = (uint8_t)HudVerticalPosition::MenuSafe;
        }
    } else if (_stricmp(key, "hud_font_size") == 0) {
        if (_stricmp(val, "small") == 0) {
            settings->font_size = (uint8_t)HudFontSize::Small;
        } else if (_stricmp(val, "normal") == 0) {
            settings->font_size = (uint8_t)HudFontSize::Normal;
        } else {
            settings->font_size = (uint8_t)HudFontSize::Large;
        }
    } else if (_stricmp(key, "hud_render_mode") == 0) {
        if (_stricmp(val, "vanilla") == 0) {
            settings->render_mode = (uint8_t)HudRenderMode::Vanilla;
        } else {
            settings->render_mode = (uint8_t)HudRenderMode::ImGui;
        }
    }
}

} // namespace

void SetDefaults(Settings* out) {
    if (!out) {
        return;
    }
    ApplyPreset(kTrailPresets[0], &out->trail_r, &out->trail_g, &out->trail_b);
    ApplyPreset(kTextPresets[0], &out->text_r, &out->text_g, &out->text_b);
    ApplyPreset(kScorePresets[0], &out->score_r, &out->score_g, &out->score_b);
    out->trail_length_px = kTrailLengthDefault;
    out->vertical_position = (uint8_t)HudVerticalPosition::MenuSafe;
    out->font_size = (uint8_t)HudFontSize::Large;
    out->render_mode = (uint8_t)HudRenderMode::ImGui;
}

void Init() {
    if (s_loaded) {
        return;
    }
    SetDefaults(&s_local);
    Load();
    s_loaded = true;
}

void Load() {
    SetDefaults(&s_local);

    FILE* file = nullptr;
    if (fopen_s(&file, kConfigFile, "r") != 0 || !file) {
        return;
    }

    char line[256] = {};
    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '[' || line[0] == '#') {
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        ParseConfigLine(line, eq + 1, &s_local);
    }

    fclose(file);
    ClampSettings(&s_local);
}

void GetLocal(Settings* out) {
    if (!out) {
        return;
    }
    if (!s_loaded) {
        Init();
    }
    *out = s_local;
}

void SetLocal(const Settings* settings) {
    if (!settings) {
        return;
    }
    s_local = *settings;
    ClampSettings(&s_local);
    s_loaded = true;
}

void CycleTrailPreset(int delta) {
    if (!s_loaded) {
        Init();
    }
    CyclePreset(kTrailPresets, sizeof(kTrailPresets) / sizeof(kTrailPresets[0]), delta,
                &s_local.trail_r, &s_local.trail_g, &s_local.trail_b);
}

void CycleTextPreset(int delta) {
    if (!s_loaded) {
        Init();
    }
    CyclePreset(kTextPresets, sizeof(kTextPresets) / sizeof(kTextPresets[0]), delta,
                &s_local.text_r, &s_local.text_g, &s_local.text_b);
}

void CycleScorePreset(int delta) {
    if (!s_loaded) {
        Init();
    }
    CyclePreset(kScorePresets, sizeof(kScorePresets) / sizeof(kScorePresets[0]), delta,
                &s_local.score_r, &s_local.score_g, &s_local.score_b);
}

static constexpr float kHudFontSizePx[] = { 12.0f, 14.0f, 16.0f };

static float FontSizePxForPreset(uint8_t preset) {
    if (preset >= (uint8_t)(sizeof(kHudFontSizePx) / sizeof(kHudFontSizePx[0]))) {
        preset = (uint8_t)HudFontSize::Large;
    }
    return kHudFontSizePx[preset];
}

void CycleVerticalPosition(int delta) {
    if (!s_loaded) {
        Init();
    }
    int next = (int)s_local.vertical_position + delta;
    if (next < (int)HudVerticalPosition::MenuSafe) {
        next = (int)HudVerticalPosition::Top;
    }
    if (next > (int)HudVerticalPosition::Top) {
        next = (int)HudVerticalPosition::MenuSafe;
    }
    s_local.vertical_position = (uint8_t)next;
}

void CycleFontSize(int delta) {
    if (!s_loaded) {
        Init();
    }
    int next = (int)s_local.font_size + delta;
    if (next < (int)HudFontSize::Small) {
        next = (int)HudFontSize::Large;
    }
    if (next > (int)HudFontSize::Large) {
        next = (int)HudFontSize::Small;
    }
    s_local.font_size = (uint8_t)next;
}

void CycleRenderMode(int delta) {
    if (!s_loaded) {
        Init();
    }
    int next = (int)s_local.render_mode + delta;
    if (next < (int)HudRenderMode::ImGui) {
        next = (int)HudRenderMode::Vanilla;
    }
    if (next > (int)HudRenderMode::Vanilla) {
        next = (int)HudRenderMode::ImGui;
    }
    s_local.render_mode = (uint8_t)next;
}

HudRenderMode GetRenderMode() {
    if (!s_loaded) {
        Init();
    }
    return (HudRenderMode)s_local.render_mode;
}

void AdjustTrailLength(int deltaPixels) {
    if (!s_loaded) {
        Init();
    }
    int next = (int)s_local.trail_length_px + deltaPixels;
    if (next < (int)kTrailLengthMin) {
        next = (int)kTrailLengthMin;
    }
    if (next > (int)kTrailLengthMax) {
        next = (int)kTrailLengthMax;
    }
    s_local.trail_length_px = (uint16_t)next;
}

const char* GetTrailPresetLabel() {
    if (!s_loaded) {
        Init();
    }
    const int index = FindPresetIndex(kTrailPresets,
        sizeof(kTrailPresets) / sizeof(kTrailPresets[0]),
        s_local.trail_r,
        s_local.trail_g,
        s_local.trail_b);
    return kTrailPresets[index].label;
}

const char* GetTextPresetLabel() {
    if (!s_loaded) {
        Init();
    }
    const int index = FindPresetIndex(kTextPresets,
        sizeof(kTextPresets) / sizeof(kTextPresets[0]),
        s_local.text_r,
        s_local.text_g,
        s_local.text_b);
    return kTextPresets[index].label;
}

const char* GetVerticalPositionLabel() {
    if (!s_loaded) {
        Init();
    }
    return s_local.vertical_position == (uint8_t)HudVerticalPosition::Top ? "Top" : "Menu-safe";
}

float GetNickYRatio(bool modMenuOpen) {
    if (!s_loaded) {
        Init();
    }
    return GetNickYRatioForSide(s_local.vertical_position, modMenuOpen);
}

float GetNickYRatioForSide(uint8_t vertical_position, bool modMenuOpen) {
    if (modMenuOpen) {
        return kNickYMenuSafeRatio;
    }
    return vertical_position == (uint8_t)HudVerticalPosition::Top
        ? kNickYTopRatio
        : kNickYMenuSafeRatio;
}

float GetFontSizePxForPreset(uint8_t font_size) {
    return FontSizePxForPreset(font_size);
}

float GetFontSizePx() {
    if (!s_loaded) {
        Init();
    }
    return FontSizePxForPreset(s_local.font_size);
}

int GetFontSizePresetIndex() {
    if (!s_loaded) {
        Init();
    }
    if (s_local.font_size >= (uint8_t)(sizeof(kHudFontSizePx) / sizeof(kHudFontSizePx[0]))) {
        return (int)HudFontSize::Large;
    }
    return (int)s_local.font_size;
}

const char* GetFontSizeLabel() {
    if (!s_loaded) {
        Init();
    }
    switch ((HudFontSize)s_local.font_size) {
        case HudFontSize::Small:  return "Small";
        case HudFontSize::Normal: return "Normal";
        default:                  return "Large";
    }
}

const char* GetScorePresetLabel() {
    if (!s_loaded) {
        Init();
    }
    const int index = FindPresetIndex(kScorePresets,
        sizeof(kScorePresets) / sizeof(kScorePresets[0]),
        s_local.score_r,
        s_local.score_g,
        s_local.score_b);
    return kScorePresets[index].label;
}

const char* GetRenderModeLabel() {
    if (!s_loaded) {
        Init();
    }
    return s_local.render_mode == (uint8_t)HudRenderMode::Vanilla ? "Vanilla" : "Overlay";
}

void PackWire(WireStyle* out) {
    if (!out) {
        return;
    }
    if (!s_loaded) {
        Init();
    }
    out->trail_r = s_local.trail_r;
    out->trail_g = s_local.trail_g;
    out->trail_b = s_local.trail_b;
    out->text_r = s_local.text_r;
    out->text_g = s_local.text_g;
    out->text_b = s_local.text_b;
    out->trail_length = EncodeTrailLength(s_local.trail_length_px);
    out->score_r = s_local.score_r;
    out->score_g = s_local.score_g;
    out->score_b = s_local.score_b;
    out->font_size = s_local.font_size;
}

void UnpackWire(const WireStyle* wire, Settings* out) {
    if (!wire || !out) {
        return;
    }
    out->trail_r = wire->trail_r;
    out->trail_g = wire->trail_g;
    out->trail_b = wire->trail_b;
    out->text_r = wire->text_r;
    out->text_g = wire->text_g;
    out->text_b = wire->text_b;
    out->trail_length_px = DecodeTrailLength(wire->trail_length);
    out->score_r = wire->score_r;
    out->score_g = wire->score_g;
    out->score_b = wire->score_b;
    out->font_size = wire->font_size;
    ClampSettings(out);
}

static void CopySideStyle(const Settings& source, ResolvedSideStyle* out) {
    out->trail_r = source.trail_r;
    out->trail_g = source.trail_g;
    out->trail_b = source.trail_b;
    out->text_r = source.text_r;
    out->text_g = source.text_g;
    out->text_b = source.text_b;
    out->score_r = source.score_r;
    out->score_g = source.score_g;
    out->score_b = source.score_b;
    out->trail_length_px = source.trail_length_px;
    out->font_size = source.font_size;
}

void ResolveSideStyles(int localGameSlot,
                       bool remoteStyleValid,
                       const Settings* remoteStyle,
                       ResolvedSideStyle* outP1,
                       ResolvedSideStyle* outP2) {
    if (!s_loaded) {
        Init();
    }

    Settings remote = {};
    if (remoteStyleValid && remoteStyle) {
        remote = *remoteStyle;
    } else {
        SetDefaults(&remote);
    }

    if (localGameSlot != 0 && localGameSlot != 1) {
        localGameSlot = 0;
    }

    const Settings* p1Source = localGameSlot == 0 ? &s_local : &remote;
    const Settings* p2Source = localGameSlot == 0 ? &remote : &s_local;

    if (outP1) {
        CopySideStyle(*p1Source, outP1);
    }
    if (outP2) {
        CopySideStyle(*p2Source, outP2);
    }
}

} // namespace NetplayHudStyle
