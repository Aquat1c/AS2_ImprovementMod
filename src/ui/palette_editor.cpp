#include "ui/palette_editor.h"

#include "net/netplay_palette_runtime.h"
#include "training/practice_tools.h"
#include "imgui.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace Net;

constexpr int kPaletteEntryCount = NETPLAY_PALETTE_BANK_SIZE / 4;
constexpr int kHistoryDepth = 32;

struct PaletteHistoryEntry {
    bool               valid;
    NetplayPaletteBank bank;
    uint8_t            selected_index;
    uint8_t            range_start;
    uint8_t            range_end;
};

static bool                    s_initialized = false;
static bool                    s_hasWorkingBank = false;
static uint8_t                 s_selectedIndex = 0;
static uint8_t                 s_rangeStart = 0;
static uint8_t                 s_rangeEnd = 15;
static uint8_t                 s_loadedGameSlot = 0xFF;
static uint8_t                 s_loadedCharacter = 0xFF;
static uint8_t                 s_loadedBasePalette = 0xFF;
static uint8_t                 s_hexIndex = 0xFF;
static uint8_t                 s_hexGameSlot = 0xFF;
static uint8_t                 s_hexBasePalette = 0xFF;
static NetplayPaletteBankSource s_loadedSource = NetplayPaletteBankSource::LiveMemory;
static bool                    s_autoApplyLive = true;
static bool                    s_showAlpha = false;
static bool                    s_showGridLabels = true;
static char                    s_hexInput[16] = {};
static float                   s_clipboardColor[4] = {};
static bool                    s_hasClipboardColor = false;
static float                   s_gradientStart[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static float                   s_gradientEnd[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static float                   s_hueShiftDegrees = 0.0f;
static float                   s_saturationScale = 1.0f;
static float                   s_valueScale = 1.0f;
static float                   s_alphaScale = 1.0f;
static NetplayPaletteBank      s_workingBank = {};
static PaletteHistoryEntry     s_undoHistory[kHistoryDepth] = {};
static PaletteHistoryEntry     s_redoHistory[kHistoryDepth] = {};
static int                     s_undoCount = 0;
static int                     s_redoCount = 0;

static float Clamp01(float value) {
    if (value <= 0.0f) {
        return 0.0f;
    }
    if (value >= 1.0f) {
        return 1.0f;
    }
    return value;
}

static int ClampInt(int value, int minValue, int maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static uint8_t FloatToByte(float value) {
    return (uint8_t)(Clamp01(value) * 255.0f + 0.5f);
}

static const char* BankSourceLabel(NetplayPaletteBankSource source) {
    switch (source) {
        case NetplayPaletteBankSource::LiveMemory:
            return "Live Memory";
        case NetplayPaletteBankSource::VanillaSource:
            return "Vanilla Source";
        case NetplayPaletteBankSource::SavedCustom:
            return "Saved Custom";
        default:
            return "Unknown";
    }
}

static bool IsSourceAvailable(const NetplayPaletteLocalContext& context, NetplayPaletteBankSource source) {
    switch (source) {
        case NetplayPaletteBankSource::LiveMemory:
            return context.has_live_bank;
        case NetplayPaletteBankSource::VanillaSource:
            return context.has_vanilla_bank;
        case NetplayPaletteBankSource::SavedCustom:
            return context.has_custom_bank;
        default:
            return false;
    }
}

static NetplayPaletteBankSource GetDefaultSource(const NetplayPaletteLocalContext& context) {
    if (context.has_live_bank) {
        return NetplayPaletteBankSource::LiveMemory;
    }
    if (context.has_custom_bank) {
        return NetplayPaletteBankSource::SavedCustom;
    }
    return NetplayPaletteBankSource::VanillaSource;
}

static void GetRangeBounds(int* outStart, int* outEnd) {
    if (!outStart || !outEnd) {
        return;
    }
    const int start = (int)s_rangeStart;
    const int end = (int)s_rangeEnd;
    *outStart = start < end ? start : end;
    *outEnd = start < end ? end : start;
}

static void LoadEntryColor(uint8_t index, float outColor[4]) {
    if (!outColor) {
        return;
    }

    const uint8_t* entry = &s_workingBank.data[index * 4];
    outColor[0] = entry[2] / 255.0f;
    outColor[1] = entry[1] / 255.0f;
    outColor[2] = entry[0] / 255.0f;
    outColor[3] = entry[3] / 255.0f;
}

static ImVec4 GetEntryColor(uint8_t index) {
    float color[4] = {};
    LoadEntryColor(index, color);
    if (!s_showAlpha) {
        color[3] = 1.0f;
    }
    return ImVec4(color[0], color[1], color[2], color[3]);
}

static void SetEntryColor(uint8_t index, const float color[4]) {
    uint8_t* entry = &s_workingBank.data[index * 4];
    entry[0] = FloatToByte(color[2]);
    entry[1] = FloatToByte(color[1]);
    entry[2] = FloatToByte(color[0]);
    entry[3] = FloatToByte(color[3]);
}

static void CopyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, dstSize, src, _TRUNCATE);
}

static void ResetHistory() {
    memset(s_undoHistory, 0, sizeof(s_undoHistory));
    memset(s_redoHistory, 0, sizeof(s_redoHistory));
    s_undoCount = 0;
    s_redoCount = 0;
}

static void CaptureHistoryEntry(PaletteHistoryEntry* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->valid = s_hasWorkingBank;
    if (s_hasWorkingBank) {
        out->bank = s_workingBank;
        out->selected_index = s_selectedIndex;
        out->range_start = s_rangeStart;
        out->range_end = s_rangeEnd;
    }
}

static void PushHistoryEntry(PaletteHistoryEntry* stack, int* count, const PaletteHistoryEntry& entry) {
    if (!stack || !count || !entry.valid) {
        return;
    }

    if (*count >= kHistoryDepth) {
        memmove(&stack[0], &stack[1], sizeof(stack[0]) * (kHistoryDepth - 1));
        *count = kHistoryDepth - 1;
    }

    stack[*count] = entry;
    ++(*count);
}

static void CopySelectedToClipboard() {
    if (!s_hasWorkingBank) {
        return;
    }
    LoadEntryColor(s_selectedIndex, s_clipboardColor);
    s_hasClipboardColor = true;
}

static void SyncHexInputFromSelection() {
    if (!s_hasWorkingBank) {
        s_hexInput[0] = '\0';
        s_hexIndex = 0xFF;
        s_hexGameSlot = 0xFF;
        s_hexBasePalette = 0xFF;
        return;
    }

    const uint8_t* entry = &s_workingBank.data[s_selectedIndex * 4];
    _snprintf_s(s_hexInput,
        sizeof(s_hexInput),
        _TRUNCATE,
        "%02X%02X%02X%02X",
        entry[2],
        entry[1],
        entry[0],
        entry[3]);
    s_hexIndex = s_selectedIndex;
    s_hexGameSlot = s_loadedGameSlot;
    s_hexBasePalette = s_loadedBasePalette;
}

static void SyncGradientEndpointsFromRange() {
    if (!s_hasWorkingBank) {
        return;
    }

    int rangeStart = 0;
    int rangeEnd = 0;
    GetRangeBounds(&rangeStart, &rangeEnd);
    LoadEntryColor((uint8_t)rangeStart, s_gradientStart);
    LoadEntryColor((uint8_t)rangeEnd, s_gradientEnd);
}

static void ResetTransformState() {
    s_hueShiftDegrees = 0.0f;
    s_saturationScale = 1.0f;
    s_valueScale = 1.0f;
    s_alphaScale = 1.0f;
}

static void ResetWorkingTools() {
    ResetHistory();
    if (s_selectedIndex >= kPaletteEntryCount) {
        s_selectedIndex = 0;
    }
    s_rangeStart = s_selectedIndex;
    s_rangeEnd = (uint8_t)ClampInt((int)s_selectedIndex + 15, 0, kPaletteEntryCount - 1);
    SyncGradientEndpointsFromRange();
    SyncHexInputFromSelection();
    ResetTransformState();
}

static bool ParseHexColor(const char* text, float fallbackAlpha, float outColor[4]) {
    if (!text || !outColor) {
        return false;
    }

    const char* scan = text;
    if (*scan == '#') {
        ++scan;
    }

    char compact[16] = {};
    size_t count = 0;
    while (*scan && count + 1 < sizeof(compact)) {
        if (*scan != ' ' && *scan != '\t') {
            compact[count++] = *scan;
        }
        ++scan;
    }
    compact[count] = '\0';

    if (count != 6 && count != 8) {
        return false;
    }

    char* end = nullptr;
    const unsigned long rgba = strtoul(compact, &end, 16);
    if (!end || *end != '\0') {
        return false;
    }

    if (count == 6) {
        outColor[0] = ((rgba >> 16) & 0xFF) / 255.0f;
        outColor[1] = ((rgba >> 8) & 0xFF) / 255.0f;
        outColor[2] = (rgba & 0xFF) / 255.0f;
        outColor[3] = fallbackAlpha;
        return true;
    }

    outColor[0] = ((rgba >> 24) & 0xFF) / 255.0f;
    outColor[1] = ((rgba >> 16) & 0xFF) / 255.0f;
    outColor[2] = ((rgba >> 8) & 0xFF) / 255.0f;
    outColor[3] = (rgba & 0xFF) / 255.0f;
    return true;
}

static bool ReloadWorkingBank(NetplayPaletteBankSource preferredSource) {
    NetplayPaletteLocalContext context{};
    NetplayPaletteRuntime_GetLocalContext(&context);

    if (!context.available) {
        s_hasWorkingBank = false;
        return false;
    }

    NetplayPaletteBankSource source = preferredSource;
    if (!IsSourceAvailable(context, source)) {
        source = GetDefaultSource(context);
    }

    s_hasWorkingBank = NetplayPaletteRuntime_CopyLocalBankForSource(source, &s_workingBank);
    if (!s_hasWorkingBank && source != GetDefaultSource(context)) {
        source = GetDefaultSource(context);
        s_hasWorkingBank = NetplayPaletteRuntime_CopyLocalBankForSource(source, &s_workingBank);
    }

    if (s_hasWorkingBank) {
        s_loadedGameSlot = context.game_slot;
        s_loadedCharacter = s_workingBank.character_id;
        s_loadedBasePalette = s_workingBank.base_palette;
        s_loadedSource = source;
        ResetWorkingTools();
    }
    return s_hasWorkingBank;
}

static void ApplyWorkingBankLive() {
    if (s_hasWorkingBank) {
        NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false);
    }
}

static void PushUndoSnapshot() {
    PaletteHistoryEntry entry{};
    CaptureHistoryEntry(&entry);
    PushHistoryEntry(s_undoHistory, &s_undoCount, entry);
    memset(s_redoHistory, 0, sizeof(s_redoHistory));
    s_redoCount = 0;
}

static void FinishBankEdit(const NetplayPaletteLocalContext& context) {
    SyncHexInputFromSelection();
    if (s_autoApplyLive && !context.match_active) {
        ApplyWorkingBankLive();
    }
}

static bool BeginBankEdit() {
    if (!s_hasWorkingBank) {
        return false;
    }
    PushUndoSnapshot();
    return true;
}

static bool UndoWorkingBank(const NetplayPaletteLocalContext& context) {
    if (!s_hasWorkingBank || s_undoCount <= 0) {
        return false;
    }

    PaletteHistoryEntry current{};
    CaptureHistoryEntry(&current);
    PushHistoryEntry(s_redoHistory, &s_redoCount, current);

    const PaletteHistoryEntry entry = s_undoHistory[s_undoCount - 1];
    memset(&s_undoHistory[s_undoCount - 1], 0, sizeof(s_undoHistory[0]));
    --s_undoCount;

    s_workingBank = entry.bank;
    s_selectedIndex = entry.selected_index;
    s_rangeStart = entry.range_start;
    s_rangeEnd = entry.range_end;
    FinishBankEdit(context);
    return true;
}

static bool RedoWorkingBank(const NetplayPaletteLocalContext& context) {
    if (!s_hasWorkingBank || s_redoCount <= 0) {
        return false;
    }

    PaletteHistoryEntry current{};
    CaptureHistoryEntry(&current);
    PushHistoryEntry(s_undoHistory, &s_undoCount, current);

    const PaletteHistoryEntry entry = s_redoHistory[s_redoCount - 1];
    memset(&s_redoHistory[s_redoCount - 1], 0, sizeof(s_redoHistory[0]));
    --s_redoCount;

    s_workingBank = entry.bank;
    s_selectedIndex = entry.selected_index;
    s_rangeStart = entry.range_start;
    s_rangeEnd = entry.range_end;
    FinishBankEdit(context);
    return true;
}

static void FillRangeWithColor(int rangeStart, int rangeEnd, const float color[4]) {
    for (int index = rangeStart; index <= rangeEnd; ++index) {
        SetEntryColor((uint8_t)index, color);
    }
}

static void ApplyGradientToRange(int rangeStart, int rangeEnd, const float startColor[4], const float endColor[4]) {
    const int span = rangeEnd - rangeStart;
    for (int index = rangeStart; index <= rangeEnd; ++index) {
        const float t = span > 0 ? (float)(index - rangeStart) / (float)span : 0.0f;
        float color[4] = {
            startColor[0] + (endColor[0] - startColor[0]) * t,
            startColor[1] + (endColor[1] - startColor[1]) * t,
            startColor[2] + (endColor[2] - startColor[2]) * t,
            startColor[3] + (endColor[3] - startColor[3]) * t,
        };
        SetEntryColor((uint8_t)index, color);
    }
}

static void ReverseRange(int rangeStart, int rangeEnd) {
    while (rangeStart < rangeEnd) {
        const uint8_t a = (uint8_t)rangeStart;
        const uint8_t b = (uint8_t)rangeEnd;
        const uint8_t temp0 = s_workingBank.data[a * 4 + 0];
        const uint8_t temp1 = s_workingBank.data[a * 4 + 1];
        const uint8_t temp2 = s_workingBank.data[a * 4 + 2];
        const uint8_t temp3 = s_workingBank.data[a * 4 + 3];
        s_workingBank.data[a * 4 + 0] = s_workingBank.data[b * 4 + 0];
        s_workingBank.data[a * 4 + 1] = s_workingBank.data[b * 4 + 1];
        s_workingBank.data[a * 4 + 2] = s_workingBank.data[b * 4 + 2];
        s_workingBank.data[a * 4 + 3] = s_workingBank.data[b * 4 + 3];
        s_workingBank.data[b * 4 + 0] = temp0;
        s_workingBank.data[b * 4 + 1] = temp1;
        s_workingBank.data[b * 4 + 2] = temp2;
        s_workingBank.data[b * 4 + 3] = temp3;
        ++rangeStart;
        --rangeEnd;
    }
}

static void ApplyTransformToRange(int rangeStart, int rangeEnd) {
    const float hueShift = s_hueShiftDegrees / 360.0f;
    for (int index = rangeStart; index <= rangeEnd; ++index) {
        float color[4] = {};
        LoadEntryColor((uint8_t)index, color);

        float hue = 0.0f;
        float saturation = 0.0f;
        float value = 0.0f;
        ImGui::ColorConvertRGBtoHSV(color[0], color[1], color[2], hue, saturation, value);
        hue += hueShift;
        while (hue < 0.0f) {
            hue += 1.0f;
        }
        while (hue >= 1.0f) {
            hue -= 1.0f;
        }
        saturation = Clamp01(saturation * s_saturationScale);
        value = Clamp01(value * s_valueScale);
        color[3] = Clamp01(color[3] * s_alphaScale);
        ImGui::ColorConvertHSVtoRGB(hue, saturation, value, color[0], color[1], color[2]);
        SetEntryColor((uint8_t)index, color);
    }
}

static void RenderOfflineSlotSelector(const NetplayPaletteRuntimeSnapshot& snapshot,
                                      const NetplayPaletteLocalContext& context) {
    if (context.match_active) {
        return;
    }

    int availableSlots = 0;
    for (int slot = 0; slot < 2; ++slot) {
        if (snapshot.player[slot].valid) {
            ++availableSlots;
        }
    }
    if (availableSlots <= 0) {
        return;
    }

    if (ImGui::BeginTable("PaletteSlots", availableSlots, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        for (int slot = 0; slot < 2; ++slot) {
            if (!snapshot.player[slot].valid) {
                continue;
            }

            const NetplayPalettePlayerState& player = snapshot.player[slot];
            ImGui::TableNextColumn();
            if (context.game_slot == slot) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.46f, 0.24f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.54f, 0.29f, 1.0f));
            }

            char label[32] = {};
            _snprintf_s(label, sizeof(label), _TRUNCATE, "P%d", slot + 1);
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f)) &&
                NetplayPaletteRuntime_SetOfflineEditorGameSlot((uint8_t)slot)) {
                ReloadWorkingBank(GetDefaultSource(context));
            }

            if (context.game_slot == slot) {
                ImGui::PopStyleColor(2);
            }

            ImGui::Text("Char %u  Base %u", player.character_id, player.base_palette);
            ImGui::TextDisabled("Live:%s  Src:%s  Saved:%s",
                player.live_bank_ready ? "yes" : "no",
                player.vanilla_bank_ready ? "yes" : "no",
                player.custom_bank_ready ? "yes" : "no");
        }
        ImGui::EndTable();
    }
}

static void RenderPaletteGrid() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::TextDisabled("Click = select  Ctrl+Click = range start  Shift+Click = range end  Right-click = quick tools");
    ImGui::BeginChild("PaletteGrid", ImVec2(0, 252), true);
    for (int row = 0; row < 16; ++row) {
        if (s_showGridLabels) {
            ImGui::Text("%03u", row * 16);
            ImGui::SameLine();
        }

        for (int col = 0; col < 16; ++col) {
            const uint8_t index = (uint8_t)(row * 16 + col);
            ImGui::PushID(index);
            const bool isSelected = index == s_selectedIndex;
            const bool isRangeEndpoint = index == s_rangeStart || index == s_rangeEnd;
            if (isSelected) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            } else if (isRangeEndpoint) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }

            ImGuiColorEditFlags gridFlags = ImGuiColorEditFlags_NoTooltip;
            if (s_showAlpha) {
                gridFlags |= ImGuiColorEditFlags_AlphaPreviewHalf;
            } else {
                gridFlags |= ImGuiColorEditFlags_NoAlpha;
            }

            if (ImGui::ColorButton("##entry", GetEntryColor(index), gridFlags, ImVec2(20, 20))) {
                if (io.KeyCtrl) {
                    s_rangeStart = index;
                } else if (io.KeyShift) {
                    s_rangeEnd = index;
                } else {
                    s_selectedIndex = index;
                }
                SyncHexInputFromSelection();
            }

            if (ImGui::IsItemHovered()) {
                const uint8_t* entry = &s_workingBank.data[index * 4];
                ImGui::BeginTooltip();
                ImGui::Text("Index %u", index);
                ImGui::Text("RGBA %02X %02X %02X %02X", entry[2], entry[1], entry[0], entry[3]);
                if (index == s_rangeStart) {
                    ImGui::TextDisabled("Range start");
                }
                if (index == s_rangeEnd) {
                    ImGui::TextDisabled("Range end");
                }
                ImGui::EndTooltip();
            }

            if (ImGui::BeginPopupContextItem("PaletteEntryContext")) {
                if (ImGui::MenuItem("Select Entry")) {
                    s_selectedIndex = index;
                    SyncHexInputFromSelection();
                }
                if (ImGui::MenuItem("Set Range Start")) {
                    s_rangeStart = index;
                }
                if (ImGui::MenuItem("Set Range End")) {
                    s_rangeEnd = index;
                }
                if (ImGui::MenuItem("Copy Entry Color")) {
                    s_selectedIndex = index;
                    CopySelectedToClipboard();
                    SyncHexInputFromSelection();
                }
                ImGui::EndPopup();
            }

            if (isSelected || isRangeEndpoint) {
                ImGui::PopStyleVar();
            }
            ImGui::PopID();
            if (col < 15) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::EndChild();
}

static void RenderSelectedTools(const NetplayPaletteLocalContext& context) {
    float selectedColor[4] = {};
    LoadEntryColor(s_selectedIndex, selectedColor);

    if (s_hexIndex != s_selectedIndex || s_hexGameSlot != s_loadedGameSlot || s_hexBasePalette != s_loadedBasePalette) {
        SyncHexInputFromSelection();
    }

    if (ImGui::BeginTable("SelectedTools", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        ImGuiColorEditFlags pickerFlags = ImGuiColorEditFlags_DisplayRGB |
            ImGuiColorEditFlags_InputRGB |
            ImGuiColorEditFlags_PickerHueWheel |
            ImGuiColorEditFlags_AlphaBar;
        if (!s_showAlpha) {
            pickerFlags |= ImGuiColorEditFlags_NoAlpha;
        }
        if (ImGui::ColorPicker4("##SelectedPicker",
                selectedColor,
                pickerFlags,
                s_hasClipboardColor ? s_clipboardColor : nullptr) &&
            BeginBankEdit()) {
            SetEntryColor(s_selectedIndex, selectedColor);
            FinishBankEdit(context);
        }

        ImGui::TableNextColumn();

        int selectedIndex = (int)s_selectedIndex;
        if (ImGui::SliderInt("Entry Index", &selectedIndex, 0, kPaletteEntryCount - 1)) {
            s_selectedIndex = (uint8_t)selectedIndex;
            SyncHexInputFromSelection();
        }

        if (ImGui::Button("Prev") && s_selectedIndex > 0) {
            --s_selectedIndex;
            SyncHexInputFromSelection();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next") && s_selectedIndex + 1 < kPaletteEntryCount) {
            ++s_selectedIndex;
            SyncHexInputFromSelection();
        }
        ImGui::SameLine();
        if (ImGui::Button("Start <- Selected")) {
            s_rangeStart = s_selectedIndex;
        }
        ImGui::SameLine();
        if (ImGui::Button("End <- Selected")) {
            s_rangeEnd = s_selectedIndex;
        }

        int red = (int)s_workingBank.data[s_selectedIndex * 4 + 2];
        int green = (int)s_workingBank.data[s_selectedIndex * 4 + 1];
        int blue = (int)s_workingBank.data[s_selectedIndex * 4 + 0];
        int alpha = (int)s_workingBank.data[s_selectedIndex * 4 + 3];
        bool channelsChanged = false;
        channelsChanged |= ImGui::DragInt("R", &red, 1.0f, 0, 255);
        channelsChanged |= ImGui::DragInt("G", &green, 1.0f, 0, 255);
        channelsChanged |= ImGui::DragInt("B", &blue, 1.0f, 0, 255);
        if (s_showAlpha) {
            channelsChanged |= ImGui::DragInt("A", &alpha, 1.0f, 0, 255);
        }
        if (channelsChanged && BeginBankEdit()) {
            float color[4] = {
                ClampInt(red, 0, 255) / 255.0f,
                ClampInt(green, 0, 255) / 255.0f,
                ClampInt(blue, 0, 255) / 255.0f,
                ClampInt(alpha, 0, 255) / 255.0f,
            };
            SetEntryColor(s_selectedIndex, color);
            FinishBankEdit(context);
        }

        bool applyHex = ImGui::InputText("Hex RGBA",
            s_hexInput,
            sizeof(s_hexInput),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Apply Hex")) {
            applyHex = true;
        }
        if (applyHex) {
            float parsedColor[4] = {};
            if (ParseHexColor(s_hexInput, selectedColor[3], parsedColor) && BeginBankEdit()) {
                SetEntryColor(s_selectedIndex, parsedColor);
                FinishBankEdit(context);
            } else {
                SyncHexInputFromSelection();
            }
        }
        ImGui::TextDisabled("Format: RRGGBB or RRGGBBAA");

        if (ImGui::Button("Copy Selected")) {
            CopySelectedToClipboard();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!s_hasClipboardColor);
        if (ImGui::Button("Paste To Selected") && BeginBankEdit()) {
            SetEntryColor(s_selectedIndex, s_clipboardColor);
            FinishBankEdit(context);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Fill Range With Selected") && BeginBankEdit()) {
            int rangeStart = 0;
            int rangeEnd = 0;
            GetRangeBounds(&rangeStart, &rangeEnd);
            FillRangeWithColor(rangeStart, rangeEnd, selectedColor);
            FinishBankEdit(context);
        }

        if (s_hasClipboardColor) {
            ImGui::ColorButton("Clipboard Preview",
                ImVec4(s_clipboardColor[0], s_clipboardColor[1], s_clipboardColor[2], s_clipboardColor[3]),
                s_showAlpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha,
                ImVec2(40, 40));
            ImGui::SameLine();
            ImGui::Text("Clipboard");
            ImGui::TextDisabled("RGBA %.0f %.0f %.0f %.0f",
                s_clipboardColor[0] * 255.0f,
                s_clipboardColor[1] * 255.0f,
                s_clipboardColor[2] * 255.0f,
                s_clipboardColor[3] * 255.0f);
        } else {
            ImGui::TextDisabled("Clipboard empty");
        }

        ImGui::Text("BGRA %02X %02X %02X %02X",
            s_workingBank.data[s_selectedIndex * 4 + 0],
            s_workingBank.data[s_selectedIndex * 4 + 1],
            s_workingBank.data[s_selectedIndex * 4 + 2],
            s_workingBank.data[s_selectedIndex * 4 + 3]);
        ImGui::EndTable();
    }
}

static void RenderRangeTools(const NetplayPaletteLocalContext& context) {
    int rangeStart = (int)s_rangeStart;
    int rangeEnd = (int)s_rangeEnd;
    if (ImGui::DragIntRange2("Entry Range",
            &rangeStart,
            &rangeEnd,
            1.0f,
            0,
            kPaletteEntryCount - 1,
            "Start: %d",
            "End: %d")) {
        s_rangeStart = (uint8_t)ClampInt(rangeStart, 0, kPaletteEntryCount - 1);
        s_rangeEnd = (uint8_t)ClampInt(rangeEnd, 0, kPaletteEntryCount - 1);
    }

    int normalizedStart = 0;
    int normalizedEnd = 0;
    GetRangeBounds(&normalizedStart, &normalizedEnd);
    ImGui::Text("Range Size: %d entries", normalizedEnd - normalizedStart + 1);

    if (ImGui::Button("Use Range Endpoints")) {
        SyncGradientEndpointsFromRange();
    }
    ImGui::SameLine();
    if (ImGui::Button("Range = Selected Only")) {
        s_rangeStart = s_selectedIndex;
        s_rangeEnd = s_selectedIndex;
        SyncGradientEndpointsFromRange();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reverse Range") && BeginBankEdit()) {
        ReverseRange(normalizedStart, normalizedEnd);
        FinishBankEdit(context);
        SyncGradientEndpointsFromRange();
    }

    ImGui::Separator();
    ImGui::Text("Gradient");
    ImGuiColorEditFlags gradientFlags = ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB;
    if (!s_showAlpha) {
        gradientFlags |= ImGuiColorEditFlags_NoAlpha;
    }
    ImGui::ColorEdit4("Start Color", s_gradientStart, gradientFlags);
    ImGui::ColorEdit4("End Color", s_gradientEnd, gradientFlags);

    if (ImGui::Button("Selected -> Start")) {
        LoadEntryColor(s_selectedIndex, s_gradientStart);
    }
    ImGui::SameLine();
    if (ImGui::Button("Selected -> End")) {
        LoadEntryColor(s_selectedIndex, s_gradientEnd);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!s_hasClipboardColor);
    if (ImGui::Button("Clipboard -> End")) {
        memcpy(s_gradientEnd, s_clipboardColor, sizeof(s_gradientEnd));
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Apply Gradient") && BeginBankEdit()) {
        ApplyGradientToRange(normalizedStart, normalizedEnd, s_gradientStart, s_gradientEnd);
        FinishBankEdit(context);
    }

    ImGui::Separator();
    ImGui::Text("HSV Transform");
    ImGui::SliderFloat("Hue Shift", &s_hueShiftDegrees, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat("Saturation", &s_saturationScale, 0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Value", &s_valueScale, 0.0f, 2.0f, "%.2fx");
    if (s_showAlpha) {
        ImGui::SliderFloat("Alpha", &s_alphaScale, 0.0f, 2.0f, "%.2fx");
    }

    if (ImGui::Button("Apply Transform") && BeginBankEdit()) {
        ApplyTransformToRange(normalizedStart, normalizedEnd);
        FinishBankEdit(context);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Transform")) {
        ResetTransformState();
    }

    ImGui::Separator();
    ImGui::Text("Clipboard");
    ImGui::BeginDisabled(!s_hasClipboardColor);
    if (ImGui::Button("Paste Clipboard To Range") && BeginBankEdit()) {
        FillRangeWithColor(normalizedStart, normalizedEnd, s_clipboardColor);
        FinishBankEdit(context);
    }
    ImGui::EndDisabled();
}

static void RenderSourceCompare(const NetplayPaletteLocalContext& context) {
    if (ImGui::BeginTable("SourceCompare", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Selected Entry");
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        for (int sourceIndex = 0; sourceIndex < 3; ++sourceIndex) {
            const NetplayPaletteBankSource source = (NetplayPaletteBankSource)sourceIndex;
            NetplayPaletteBank bank{};
            if (!NetplayPaletteRuntime_CopyLocalBankForSource(source, &bank)) {
                continue;
            }

            const uint8_t* entry = &bank.data[s_selectedIndex * 4];
            const ImVec4 preview(entry[2] / 255.0f, entry[1] / 255.0f, entry[0] / 255.0f, entry[3] / 255.0f);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", BankSourceLabel(source));
            if (source == s_loadedSource) {
                ImGui::TextDisabled("Loaded");
            }

            ImGui::TableNextColumn();
            ImGui::ColorButton("##sourceColor",
                preview,
                s_showAlpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha,
                ImVec2(40, 20));

            ImGui::TableNextColumn();
            ImGui::Text("RGBA %02X %02X %02X %02X", entry[2], entry[1], entry[0], entry[3]);
            ImGui::TextDisabled("CRC 0x%08X", bank.crc32);

            ImGui::TableNextColumn();
            if (ImGui::Button(BankSourceLabel(source))) {
                ReloadWorkingBank(source);
            }
        }

        ImGui::EndTable();
    }

    ImGui::TextDisabled("Current source: %s  |  Live:%s  Vanilla:%s  Saved:%s",
        BankSourceLabel(s_loadedSource),
        context.has_live_bank ? "yes" : "no",
        context.has_vanilla_bank ? "yes" : "no",
        context.has_custom_bank ? "yes" : "no");
}

} // namespace

void PaletteEditor_Init() {
    memset(&s_workingBank, 0, sizeof(s_workingBank));
    s_hasWorkingBank = false;
    s_selectedIndex = 0;
    s_rangeStart = 0;
    s_rangeEnd = 15;
    s_loadedGameSlot = 0xFF;
    s_loadedCharacter = 0xFF;
    s_loadedBasePalette = 0xFF;
    s_hexIndex = 0xFF;
    s_hexGameSlot = 0xFF;
    s_hexBasePalette = 0xFF;
    s_loadedSource = NetplayPaletteBankSource::LiveMemory;
    s_autoApplyLive = true;
    s_showAlpha = false;
    s_showGridLabels = true;
    s_hexInput[0] = '\0';
    memset(s_clipboardColor, 0, sizeof(s_clipboardColor));
    s_hasClipboardColor = false;
    ResetHistory();
    ResetTransformState();
    s_initialized = true;
}

void PaletteEditor_Render() {
    if (!s_initialized) {
        PaletteEditor_Init();
    }

    NetplayPaletteRuntimeSnapshot snapshot{};
    NetplayPaletteRuntime_GetSnapshot(&snapshot);

    NetplayPaletteLocalContext context{};
    NetplayPaletteRuntime_GetLocalContext(&context);

    ImGui::TextWrapped("%s", context.status);
    if (!context.available) {
        ImGui::TextDisabled("No active in-match palette context yet.");
        return;
    }

    RenderOfflineSlotSelector(snapshot, context);

    ImGui::Text("Editing Slot: P%d", context.game_slot + 1);
    ImGui::SameLine();
    ImGui::Text("Character: %u", context.character_id);
    ImGui::SameLine();
    ImGui::Text("Base: %u", context.base_palette);

    if ((!s_hasWorkingBank && (context.has_live_bank || context.has_custom_bank || context.has_vanilla_bank)) ||
        context.game_slot != s_loadedGameSlot ||
        context.character_id != s_loadedCharacter ||
        context.base_palette != s_loadedBasePalette ||
        !IsSourceAvailable(context, s_loadedSource)) {
        ReloadWorkingBank(GetDefaultSource(context));
    }

    ImGui::Checkbox("Auto Apply Live", &s_autoApplyLive);
    ImGui::SameLine();
    ImGui::Checkbox("Show Alpha", &s_showAlpha);
    ImGui::SameLine();
    ImGui::Checkbox("Row Labels", &s_showGridLabels);

    if (!context.match_active && PracticeTools_IsPracticeModeActive()) {
        ImGui::SameLine();
        bool paused = PracticeTools_IsPaused();
        if (ImGui::Checkbox("Freeze Frame", &paused)) {
            PracticeTools_SetPaused(paused);
        }
    }

    ImGui::Text("Loaded From: %s", BankSourceLabel(s_loadedSource));
    ImGui::SameLine();
    ImGui::TextDisabled("Undo:%d  Redo:%d", s_undoCount, s_redoCount);

    ImGui::BeginDisabled(!context.has_live_bank);
    if (ImGui::Button("Load Live Memory")) {
        ReloadWorkingBank(NetplayPaletteBankSource::LiveMemory);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.has_vanilla_bank);
    if (ImGui::Button("Load Vanilla Source")) {
        ReloadWorkingBank(NetplayPaletteBankSource::VanillaSource);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.has_custom_bank);
    if (ImGui::Button("Load Saved Custom")) {
        ReloadWorkingBank(NetplayPaletteBankSource::SavedCustom);
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Reload Working")) {
        ReloadWorkingBank(s_loadedSource);
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply Live") && s_hasWorkingBank) {
        ApplyWorkingBankLive();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Custom") && s_hasWorkingBank) {
        NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Custom")) {
        NetplayPaletteRuntime_ClearLocalCustomBank(true);
        ReloadWorkingBank(GetDefaultSource(context));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s_undoCount <= 0);
    if (ImGui::Button("Undo")) {
        UndoWorkingBank(context);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s_redoCount <= 0);
    if (ImGui::Button("Redo")) {
        RedoWorkingBank(context);
    }
    ImGui::EndDisabled();

    if (!s_hasWorkingBank) {
        ImGui::Separator();
        ImGui::TextDisabled("Palette bank not captured yet. Start or reload a match to populate live memory or source data.");
        return;
    }

    ImGui::Separator();
    RenderPaletteGrid();

    if (ImGui::BeginTabBar("PaletteTools")) {
        if (ImGui::BeginTabItem("Selected")) {
            RenderSelectedTools(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Range Tools")) {
            RenderRangeTools(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sources")) {
            RenderSourceCompare(context);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}