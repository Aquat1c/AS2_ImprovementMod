#include "ui/palette_editor.h"

#include "mod_main.h"
#include "net/netplay_palette_storage.h"
#include "net/netplay_palette_runtime.h"
#include "training/practice_tools.h"
#include "imgui.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

namespace {

using namespace Net;

constexpr int kPaletteEntryCount = NETPLAY_PALETTE_BANK_SIZE / 4;
constexpr int kHistoryDepth = 32;
constexpr int kMaxPresetCount = 128;

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
static bool                    s_showGridIndices = true;
static bool                    s_highlightGrid = false;
static bool                    s_hideEmptyTail = true;
static char                    s_hexInput[16] = {};
static float                   s_clipboardColor[4] = {};
static bool                    s_hasClipboardColor = false;
static float                   s_gradientStart[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static float                   s_gradientEnd[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static float                   s_hueShiftDegrees = 0.0f;
static float                   s_saturationScale = 1.0f;
static float                   s_valueScale = 1.0f;
static float                   s_alphaScale = 1.0f;
static float                   s_gridCellSize = 24.0f;
static float                   s_pickerScale = 1.0f;
static uint8_t                 s_hoveredIndex = 0xFF;
static NetplayPaletteBank      s_workingBank = {};
static PaletteHistoryEntry     s_undoHistory[kHistoryDepth] = {};
static PaletteHistoryEntry     s_redoHistory[kHistoryDepth] = {};
static NetplayPalettePresetInfo s_presets[kMaxPresetCount] = {};
static int                     s_undoCount = 0;
static int                     s_redoCount = 0;
static int                     s_presetCount = 0;
static int                     s_selectedPreset = -1;
static uint8_t                 s_presetCharacter = 0xFF;
static uint8_t                 s_presetBasePalette = 0xFF;
static char                    s_presetName[64] = {};
static char                    s_loadedPresetName[64] = {};
static char                    s_pendingPresetOverwrite[64] = {};
static char                    s_presetStatus[160] = {};

static void SyncGradientEndpointsFromRange();
static void SyncHexInputFromSelection();
static void ResetTransformState();

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

static float ClampFloat(float value, float minValue, float maxValue) {
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

static void SetStatusText(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(dst, dstSize, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static const char* BankSourceLabel(NetplayPaletteBankSource source) {
    switch (source) {
        case NetplayPaletteBankSource::LiveMemory:
            return "Live Memory";
        case NetplayPaletteBankSource::VanillaSource:
            return "Vanilla Source";
        case NetplayPaletteBankSource::AppliedCustom:
            return "Applied Custom";
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
        case NetplayPaletteBankSource::AppliedCustom:
            return context.has_applied_custom_bank;
        case NetplayPaletteBankSource::SavedCustom:
            return context.has_saved_custom_bank;
        default:
            return false;
    }
}

static NetplayPaletteBankSource GetDefaultSource(const NetplayPaletteLocalContext& context) {
    if (context.has_live_bank) {
        return NetplayPaletteBankSource::LiveMemory;
    }
    if (context.has_applied_custom_bank) {
        return NetplayPaletteBankSource::AppliedCustom;
    }
    if (context.has_saved_custom_bank) {
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

static bool IsIndexInRange(uint8_t index) {
    int rangeStart = 0;
    int rangeEnd = 0;
    GetRangeBounds(&rangeStart, &rangeEnd);
    return index >= rangeStart && index <= rangeEnd;
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

static void LoadBankEntryColor(const NetplayPaletteBank& bank, uint8_t index, float outColor[4]) {
    if (!outColor) {
        return;
    }

    const uint8_t* entry = &bank.data[index * 4];
    outColor[0] = entry[2] / 255.0f;
    outColor[1] = entry[1] / 255.0f;
    outColor[2] = entry[0] / 255.0f;
    outColor[3] = entry[3] / 255.0f;
}

static ImVec4 GetBankEntryColor(const NetplayPaletteBank& bank, uint8_t index, bool showAlpha) {
    float color[4] = {};
    LoadBankEntryColor(bank, index, color);
    if (!showAlpha) {
        color[3] = 1.0f;
    }
    return ImVec4(color[0], color[1], color[2], color[3]);
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

static void SwapEntries(uint8_t lhsIndex, uint8_t rhsIndex) {
    if (lhsIndex == rhsIndex) {
        return;
    }

    uint8_t temp[4] = {};
    memcpy(temp, &s_workingBank.data[lhsIndex * 4], sizeof(temp));
    memcpy(&s_workingBank.data[lhsIndex * 4], &s_workingBank.data[rhsIndex * 4], sizeof(temp));
    memcpy(&s_workingBank.data[rhsIndex * 4], temp, sizeof(temp));
}

static ImVec4 GetGridEntryColor(uint8_t index) {
    ImVec4 color = GetEntryColor(index);
    if (!s_highlightGrid) {
        return color;
    }

    const bool isSelected = index == s_selectedIndex;
    const bool isHovered = index == s_hoveredIndex;
    const bool isInRange = IsIndexInRange(index);
    if (isSelected || isHovered) {
        return color;
    }

    if (isInRange) {
        color.x = Clamp01(color.x * 0.65f + 0.08f);
        color.y = Clamp01(color.y * 0.65f + 0.08f);
        color.z = Clamp01(color.z * 0.65f + 0.08f);
        color.w = 1.0f;
        return color;
    }

    color.x *= 0.18f;
    color.y *= 0.18f;
    color.z *= 0.18f;
    color.w = 1.0f;
    return color;
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

static bool SanitizePresetName(const char* presetName, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }

    out[0] = '\0';
    if (!presetName) {
        return false;
    }

    size_t writeIndex = 0;
    while (*presetName && writeIndex + 1 < outSize) {
        const char ch = *presetName++;
        if (ch <= 31 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' ||
            ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            out[writeIndex++] = '_';
        } else {
            out[writeIndex++] = ch;
        }
    }

    while (writeIndex > 0 && (out[writeIndex - 1] == ' ' || out[writeIndex - 1] == '.')) {
        --writeIndex;
    }
    out[writeIndex] = '\0';
    return writeIndex > 0;
}

static bool IsZeroEntry(const NetplayPaletteBank& bank, int index) {
    if (index < 0 || index >= kPaletteEntryCount) {
        return true;
    }

    const uint8_t* entry = &bank.data[index * 4];
    return entry[0] == 0 && entry[1] == 0 && entry[2] == 0 && entry[3] == 0;
}

static int FindLastNonZeroEntry(const NetplayPaletteBank& bank) {
    for (int index = kPaletteEntryCount - 1; index >= 0; --index) {
        if (!IsZeroEntry(bank, index)) {
            return index;
        }
    }
    return 0;
}

static int GetVisibleEntryCountForBank(const NetplayPaletteBank& bank) {
    if (!s_hideEmptyTail) {
        return kPaletteEntryCount;
    }

    return ClampInt(FindLastNonZeroEntry(bank) + 1, 1, kPaletteEntryCount);
}

static int GetVisibleEntryCount() {
    if (!s_hasWorkingBank) {
        return kPaletteEntryCount;
    }
    return GetVisibleEntryCountForBank(s_workingBank);
}

static int GetVisibleMaxIndex() {
    return GetVisibleEntryCount() - 1;
}

static void ClampSelectionToVisibleEntries() {
    const int maxIndex = GetVisibleMaxIndex();
    s_selectedIndex = (uint8_t)ClampInt((int)s_selectedIndex, 0, maxIndex);
    s_rangeStart = (uint8_t)ClampInt((int)s_rangeStart, 0, maxIndex);
    s_rangeEnd = (uint8_t)ClampInt((int)s_rangeEnd, 0, maxIndex);
}

static void SyncWorkingToolsAfterBankReplace(bool keepSelection) {
    s_hoveredIndex = 0xFF;
    const int visibleMaxIndex = GetVisibleMaxIndex();
    if (!keepSelection || s_selectedIndex > visibleMaxIndex) {
        s_selectedIndex = 0;
    }
    if (!keepSelection || s_rangeStart > visibleMaxIndex || s_rangeEnd > visibleMaxIndex) {
        s_rangeStart = s_selectedIndex;
        s_rangeEnd = (uint8_t)ClampInt((int)s_selectedIndex + 15, 0, visibleMaxIndex);
    } else {
        ClampSelectionToVisibleEntries();
    }
    SyncGradientEndpointsFromRange();
    SyncHexInputFromSelection();
    ResetTransformState();
}

static void RefreshPresetList() {
    char previousSelection[64] = {};
    if (s_selectedPreset >= 0 && s_selectedPreset < s_presetCount) {
        CopyText(previousSelection, sizeof(previousSelection), s_presets[s_selectedPreset].name);
    }

    memset(s_presets, 0, sizeof(s_presets));
    s_presetCount = 0;
    s_selectedPreset = -1;
    s_presetCharacter = 0xFF;
    s_presetBasePalette = 0xFF;

    if (!s_hasWorkingBank) {
        return;
    }

    s_presetCount = NetplayPaletteStorage_ListPresets(
        s_workingBank.character_id,
        s_workingBank.base_palette,
        s_presets,
        kMaxPresetCount);
    s_presetCharacter = s_workingBank.character_id;
    s_presetBasePalette = s_workingBank.base_palette;
    if (previousSelection[0]) {
        for (int presetIndex = 0; presetIndex < s_presetCount; ++presetIndex) {
            if (_stricmp(s_presets[presetIndex].name, previousSelection) == 0) {
                s_selectedPreset = presetIndex;
                break;
            }
        }
    }
    if (s_selectedPreset < 0 && s_presetCount > 0) {
        s_selectedPreset = 0;
    }
}

static void EnsurePresetListCurrent() {
    if (!s_hasWorkingBank) {
        s_presetCount = 0;
        s_selectedPreset = -1;
        return;
    }

    if (s_presetCharacter != s_workingBank.character_id ||
        s_presetBasePalette != s_workingBank.base_palette) {
        RefreshPresetList();
    }
}

static bool LoadPresetByIndex(int presetIndex, NetplayPaletteBank* out) {
    if (!out || presetIndex < 0 || presetIndex >= s_presetCount || !s_hasWorkingBank) {
        return false;
    }

    return NetplayPaletteStorage_LoadPreset(
        s_workingBank.character_id,
        s_workingBank.base_palette,
        s_presets[presetIndex].name,
        out);
}

static void RenderBankPreviewRow(const NetplayPaletteBank& bank, int maxEntries) {
    const int visibleCount = GetVisibleEntryCountForBank(bank);
    const int previewCount = ClampInt(maxEntries, 1, visibleCount);
    for (int index = 0; index < previewCount; ++index) {
        ImGui::PushID(index);
        ImGui::ColorButton("##PresetPreviewColor",
            GetBankEntryColor(bank, (uint8_t)index, s_showAlpha),
            s_showAlpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha,
            ImVec2(ModUI_Scale(14.0f), ModUI_Scale(14.0f)));
        ImGui::PopID();
        if (index + 1 < previewCount) {
            ImGui::SameLine();
        }
    }
}

static void RenderPresetTooltip(int presetIndex) {
    if (presetIndex < 0 || presetIndex >= s_presetCount || !s_hasWorkingBank) {
        return;
    }

    NetplayPaletteBank bank{};
    if (!LoadPresetByIndex(presetIndex, &bank)) {
        return;
    }

    ImGui::BeginTooltip();
    ImGui::Text("%s", s_presets[presetIndex].name);
    ImGui::TextDisabled("Char %u  Base %u  CRC 0x%08X", bank.character_id, bank.base_palette, bank.crc32);
    ImGui::TextDisabled("Visible entries: %d / %d", GetVisibleEntryCountForBank(bank), kPaletteEntryCount);
    RenderBankPreviewRow(bank, 16);
    ImGui::EndTooltip();
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
    SyncWorkingToolsAfterBankReplace(false);
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
        s_loadedPresetName[0] = '\0';
        ResetWorkingTools();
        RefreshPresetList();
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

static int FindPresetIndexByName(const char* presetName) {
    char sanitized[64] = {};
    if (!SanitizePresetName(presetName, sanitized, sizeof(sanitized))) {
        return -1;
    }

    for (int presetIndex = 0; presetIndex < s_presetCount; ++presetIndex) {
        if (_stricmp(s_presets[presetIndex].name, sanitized) == 0) {
            return presetIndex;
        }
    }
    return -1;
}

static bool ReplaceWorkingBank(const NetplayPaletteBank& bank, bool keepSelection, bool pushUndo) {
    if (!bank.valid) {
        return false;
    }

    if (pushUndo && s_hasWorkingBank) {
        PushUndoSnapshot();
    }

    s_workingBank = bank;
    s_hasWorkingBank = true;
    s_loadedCharacter = bank.character_id;
    s_loadedBasePalette = bank.base_palette;
    SyncWorkingToolsAfterBankReplace(keepSelection);
    return true;
}

static bool LoadPresetIntoWorkingBank(int presetIndex, bool applyLive) {
    if (presetIndex < 0 || presetIndex >= s_presetCount) {
        SetStatusText(s_presetStatus, sizeof(s_presetStatus), "No preset selected.");
        return false;
    }

    NetplayPaletteBank bank{};
    if (!LoadPresetByIndex(presetIndex, &bank)) {
        SetStatusText(s_presetStatus,
            sizeof(s_presetStatus),
            "Failed to load preset '%s'.",
            s_presets[presetIndex].name);
        return false;
    }

    if (!ReplaceWorkingBank(bank, true, true)) {
        SetStatusText(s_presetStatus,
            sizeof(s_presetStatus),
            "Failed to replace the working bank with '%s'.",
            s_presets[presetIndex].name);
        return false;
    }

    CopyText(s_loadedPresetName, sizeof(s_loadedPresetName), s_presets[presetIndex].name);
    CopyText(s_presetName, sizeof(s_presetName), s_presets[presetIndex].name);
    s_selectedPreset = presetIndex;
    if (applyLive) {
        ApplyWorkingBankLive();
    }

    SetStatusText(s_presetStatus,
        sizeof(s_presetStatus),
        applyLive ? "Applied preset '%s' to live memory." : "Loaded preset '%s' into the editor.",
        s_presets[presetIndex].name);
    return true;
}

static bool SaveWorkingBankAsPreset(const char* presetName, bool overwriteExisting) {
    if (!s_hasWorkingBank) {
        SetStatusText(s_presetStatus, sizeof(s_presetStatus), "No working bank to save.");
        return false;
    }

    char sanitized[64] = {};
    if (!SanitizePresetName(presetName, sanitized, sizeof(sanitized))) {
        SetStatusText(s_presetStatus, sizeof(s_presetStatus), "Enter a valid preset name first.");
        return false;
    }

    if (!overwriteExisting && NetplayPaletteStorage_HasPreset(
            s_workingBank.character_id,
            s_workingBank.base_palette,
            sanitized)) {
        CopyText(s_pendingPresetOverwrite, sizeof(s_pendingPresetOverwrite), sanitized);
        ImGui::OpenPopup("Overwrite Preset?");
        return false;
    }

    if (!NetplayPaletteStorage_SavePreset(&s_workingBank, sanitized)) {
        SetStatusText(s_presetStatus,
            sizeof(s_presetStatus),
            "Failed to save preset '%s'.",
            sanitized);
        return false;
    }

    RefreshPresetList();
    s_selectedPreset = FindPresetIndexByName(sanitized);
    CopyText(s_presetName, sizeof(s_presetName), sanitized);
    SetStatusText(s_presetStatus, sizeof(s_presetStatus), "Saved preset '%s'.", sanitized);
    return true;
}

static bool DeleteSelectedPreset() {
    if (s_selectedPreset < 0 || s_selectedPreset >= s_presetCount || !s_hasWorkingBank) {
        SetStatusText(s_presetStatus, sizeof(s_presetStatus), "No preset selected.");
        return false;
    }

    char deletedName[64] = {};
    CopyText(deletedName, sizeof(deletedName), s_presets[s_selectedPreset].name);
    const int deletedIndex = s_selectedPreset;
    if (!NetplayPaletteStorage_DeletePreset(
            s_workingBank.character_id,
            s_workingBank.base_palette,
            deletedName)) {
        SetStatusText(s_presetStatus,
            sizeof(s_presetStatus),
            "Failed to delete preset '%s'.",
            deletedName);
        return false;
    }

    RefreshPresetList();
    if (s_presetCount > 0) {
        s_selectedPreset = ClampInt(deletedIndex, 0, s_presetCount - 1);
        CopyText(s_presetName, sizeof(s_presetName), s_presets[s_selectedPreset].name);
    } else {
        s_selectedPreset = -1;
        s_presetName[0] = '\0';
    }

    SetStatusText(s_presetStatus, sizeof(s_presetStatus), "Deleted preset '%s'.", deletedName);
    return true;
}

static int PickRandomPresetIndex() {
    if (s_presetCount <= 0) {
        return -1;
    }
    if (s_presetCount == 1) {
        return 0;
    }

    int randomIndex = s_selectedPreset;
    while (randomIndex == s_selectedPreset) {
        randomIndex = rand() % s_presetCount;
    }
    return randomIndex;
}

static void FinishBankEdit(const NetplayPaletteLocalContext& context) {
    ClampSelectionToVisibleEntries();
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
    s_loadedPresetName[0] = '\0';
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
    s_loadedPresetName[0] = '\0';
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

static void RenderPaletteSummary(const NetplayPaletteLocalContext& context) {
    int rangeStart = 0;
    int rangeEnd = 0;
    GetRangeBounds(&rangeStart, &rangeEnd);
    const int visibleCount = GetVisibleEntryCount();

    ImGui::Text("Editing Slot: P%d", context.game_slot + 1);
    ImGui::SameLine();
    ImGui::Text("Character: %u", context.character_id);
    ImGui::SameLine();
    ImGui::Text("Base: %u", context.base_palette);
    ImGui::SameLine();
    ImGui::TextDisabled("Loaded: %s", BankSourceLabel(s_loadedSource));

    ImGui::TextDisabled("Selected %u (0x%02X)  Range %u..%u (%d)  Visible:%d/%d  Undo:%d  Redo:%d",
        s_selectedIndex,
        s_selectedIndex,
        rangeStart,
        rangeEnd,
        rangeEnd - rangeStart + 1,
        visibleCount,
        kPaletteEntryCount,
        s_undoCount,
        s_redoCount);
    if (s_hoveredIndex < kPaletteEntryCount) {
        ImGui::SameLine();
        ImGui::TextDisabled("Hover %u (0x%02X)", s_hoveredIndex, s_hoveredIndex);
    }

    if (s_loadedPresetName[0]) {
        ImGui::TextDisabled("Editor preset: %s", s_loadedPresetName);
    }

    ImGui::TextDisabled("Live:%s  Applied:%s  Saved:%s  Asset:%s",
        context.has_live_bank ? "yes" : "no",
        context.has_applied_custom_bank ? "yes" : "no",
        context.has_saved_custom_bank ? "yes" : "no",
        context.asset_loaded ? "yes" : "no");
}

static void RenderPaletteToolbar(const NetplayPaletteLocalContext& context) {
    ImGui::Checkbox("Auto Apply Live", &s_autoApplyLive);
    ImGui::SameLine();
    ImGui::Checkbox("Show Transparency", &s_showAlpha);
    ImGui::SameLine();
    ImGui::Checkbox("Show Row Labels", &s_showGridLabels);
    ImGui::SameLine();
    ImGui::Checkbox("Show Box Indices", &s_showGridIndices);
    ImGui::SameLine();
    ImGui::Checkbox("Highlight Grid", &s_highlightGrid);
    ImGui::SameLine();
    if (ImGui::Checkbox("Hide Empty Tail", &s_hideEmptyTail)) {
        ClampSelectionToVisibleEntries();
        SyncGradientEndpointsFromRange();
        SyncHexInputFromSelection();
    }

    if (!context.match_active && PracticeTools_IsPracticeModeActive()) {
        ImGui::SameLine();
        bool paused = PracticeTools_IsPaused();
        if (ImGui::Checkbox("Freeze Frame", &paused)) {
            PracticeTools_SetPaused(paused);
        }
    }

    ImGui::SetNextItemWidth(ModUI_Scale(180.0f));
    if (ImGui::SliderFloat("Box Size", &s_gridCellSize, 18.0f, 32.0f, "%.0f px")) {
        if (s_gridCellSize < 18.0f) {
            s_gridCellSize = 18.0f;
        }
        if (s_gridCellSize > 32.0f) {
            s_gridCellSize = 32.0f;
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ModUI_Scale(180.0f));
    if (ImGui::SliderFloat("Picker Scale", &s_pickerScale, 0.75f, 1.75f, "%.2fx")) {
        s_pickerScale = ClampFloat(s_pickerScale, 0.75f, 1.75f);
    }

    if (ImGui::BeginTable("PaletteCommandBar", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Sources");
        ImGui::BeginDisabled(!context.has_live_bank);
        if (ImGui::Button("Load Live")) {
            ReloadWorkingBank(NetplayPaletteBankSource::LiveMemory);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.has_vanilla_bank);
        if (ImGui::Button("Load Vanilla")) {
            ReloadWorkingBank(NetplayPaletteBankSource::VanillaSource);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.has_applied_custom_bank);
        if (ImGui::Button("Load Applied")) {
            ReloadWorkingBank(NetplayPaletteBankSource::AppliedCustom);
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!context.has_saved_custom_bank);
        if (ImGui::Button("Load Saved")) {
            ReloadWorkingBank(NetplayPaletteBankSource::SavedCustom);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reload Working")) {
            ReloadWorkingBank(s_loadedSource);
        }

        ImGui::TableNextColumn();
        ImGui::TextDisabled("Working Bank");
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

        ImGui::EndTable();
    }
}

static void RenderPaletteGrid(const NetplayPaletteLocalContext& context) {
    const float uiScale = ModUI_GetScale();
    const float cellSize = s_gridCellSize * uiScale;
    const ImGuiIO& io = ImGui::GetIO();
    const int visibleCount = GetVisibleEntryCount();
    const int rowCount = (visibleCount + 15) / 16;
    s_hoveredIndex = 0xFF;
    ImGui::TextDisabled("Click select  Ctrl start range  Shift end range  Drag a box to swap  Right-click for quick tools");
    float gridHeight = ImGui::GetContentRegionAvail().y;
    if (gridHeight < ModUI_Scale(280.0f)) {
        gridHeight = ModUI_Scale(280.0f);
    }
    ImGui::BeginChild("PaletteGrid", ImVec2(0, gridHeight), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int row = 0; row < rowCount; ++row) {
        if (s_showGridLabels) {
            ImGui::Text("%02X", row * 16);
            ImGui::SameLine();
        }

        const int rowStart = row * 16;
        const int rowEntryCount = ClampInt(visibleCount - rowStart, 0, 16);
        for (int col = 0; col < rowEntryCount; ++col) {
            const uint8_t index = (uint8_t)(row * 16 + col);
            ImGui::PushID(index);
            const bool isSelected = index == s_selectedIndex;
            const bool isRangeEndpoint = index == s_rangeStart || index == s_rangeEnd;
            bool pushedBorderStyle = false;
            if (isSelected) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.83f, 0.22f, 1.0f));
                pushedBorderStyle = true;
            } else if (isRangeEndpoint) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.72f, 0.96f, 1.0f));
                pushedBorderStyle = true;
            }

            ImGuiColorEditFlags gridFlags = ImGuiColorEditFlags_NoTooltip;
            if (s_showAlpha) {
                gridFlags |= ImGuiColorEditFlags_AlphaPreviewHalf;
            } else {
                gridFlags |= ImGuiColorEditFlags_NoAlpha;
            }

            const ImVec4 displayColor = GetGridEntryColor(index);

            if (ImGui::ColorButton("##entry", displayColor, gridFlags, ImVec2(cellSize, cellSize))) {
                if (io.KeyCtrl) {
                    s_rangeStart = index;
                } else if (io.KeyShift) {
                    s_rangeEnd = index;
                } else {
                    s_selectedIndex = index;
                }
                SyncHexInputFromSelection();
            }

            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            if (s_showGridIndices) {
                char indexLabel[8] = {};
                _snprintf_s(indexLabel, sizeof(indexLabel), _TRUNCATE, "%02X", index);
                const ImVec2 textSize = ImGui::CalcTextSize(indexLabel);
                const float luma = displayColor.x * 0.299f + displayColor.y * 0.587f + displayColor.z * 0.114f;
                const ImU32 textColor = luma > 0.55f ? IM_COL32(12, 12, 12, 220) : IM_COL32(255, 255, 255, 220);
                drawList->AddText(
                    ImVec2(itemMin.x + (itemMax.x - itemMin.x - textSize.x) * 0.5f,
                           itemMin.y + (itemMax.y - itemMin.y - textSize.y) * 0.5f),
                    textColor,
                    indexLabel);
            }

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("PaletteEntryIndex", &index, sizeof(index));
                ImGui::Text("Swap entry %u (0x%02X)", index, index);
                ImGui::ColorButton("##dragPreview",
                    GetEntryColor(index),
                    s_showAlpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha,
                    ImVec2(ModUI_Scale(32.0f), ModUI_Scale(32.0f)));
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PaletteEntryIndex")) {
                    if (payload->DataSize == sizeof(uint8_t)) {
                        const uint8_t sourceIndex = *(const uint8_t*)payload->Data;
                        if (sourceIndex != index && BeginBankEdit()) {
                            SwapEntries(sourceIndex, index);
                            s_selectedIndex = index;
                            FinishBankEdit(context);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::IsItemHovered()) {
                s_hoveredIndex = index;
                const uint8_t* entry = &s_workingBank.data[index * 4];
                ImGui::BeginTooltip();
                ImGui::Text("Index %u (0x%02X)", index, index);
                ImGui::Text("RGBA %02X %02X %02X %02X", entry[2], entry[1], entry[0], entry[3]);
                ImGui::TextDisabled("Drag onto another box to swap the two entries.");
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
                ImGui::BeginDisabled(!s_hasClipboardColor);
                if (ImGui::MenuItem("Paste Clipboard To Entry") && BeginBankEdit()) {
                    s_selectedIndex = index;
                    SetEntryColor(index, s_clipboardColor);
                    FinishBankEdit(context);
                }
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Swap With Selected", nullptr, false, index != s_selectedIndex) && BeginBankEdit()) {
                    SwapEntries(index, s_selectedIndex);
                    s_selectedIndex = index;
                    FinishBankEdit(context);
                }
                if (ImGui::MenuItem("Use As Gradient Start")) {
                    LoadEntryColor(index, s_gradientStart);
                }
                if (ImGui::MenuItem("Use As Gradient End")) {
                    LoadEntryColor(index, s_gradientEnd);
                }
                ImGui::EndPopup();
            }

            if (pushedBorderStyle) {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            ImGui::PopID();
            if (col + 1 < rowEntryCount) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::EndChild();
}

static void RenderSelectedTools(const NetplayPaletteLocalContext& context) {
    const int visibleMaxIndex = GetVisibleMaxIndex();

    ImGui::TextDisabled("Selected entry %u (0x%02X)", s_selectedIndex, s_selectedIndex);
    if (s_hoveredIndex < kPaletteEntryCount) {
        ImGui::SameLine();
        ImGui::TextDisabled("Hover %u (0x%02X)", s_hoveredIndex, s_hoveredIndex);
    }

    float selectedColor[4] = {};
    LoadEntryColor(s_selectedIndex, selectedColor);

    if (s_hexIndex != s_selectedIndex || s_hexGameSlot != s_loadedGameSlot || s_hexBasePalette != s_loadedBasePalette) {
        SyncHexInputFromSelection();
    }

    if (ImGui::BeginTable("SelectedTools", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        ImGuiColorEditFlags pickerFlags = ImGuiColorEditFlags_DisplayHSV |
            ImGuiColorEditFlags_PickerHueBar |
            ImGuiColorEditFlags_AlphaBar;
        if (!s_showAlpha) {
            pickerFlags |= ImGuiColorEditFlags_NoAlpha;
        }
        const float pickerWidth = ClampFloat(
            ImGui::GetContentRegionAvail().x * s_pickerScale,
            ModUI_Scale(180.0f),
            ModUI_Scale(420.0f));
        ImGui::PushItemWidth(pickerWidth);
        if (ImGui::ColorPicker4("##SelectedPicker",
                selectedColor,
                pickerFlags,
                s_hasClipboardColor ? s_clipboardColor : nullptr) &&
            BeginBankEdit()) {
            SetEntryColor(s_selectedIndex, selectedColor);
            FinishBankEdit(context);
        }
        ImGui::PopItemWidth();

        ImGui::TableNextColumn();

        int selectedIndex = (int)s_selectedIndex;
        if (ImGui::SliderInt("Entry Index", &selectedIndex, 0, visibleMaxIndex)) {
            s_selectedIndex = (uint8_t)selectedIndex;
            SyncHexInputFromSelection();
        }

        if (ImGui::Button("Prev") && s_selectedIndex > 0) {
            --s_selectedIndex;
            SyncHexInputFromSelection();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next") && s_selectedIndex < visibleMaxIndex) {
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

        float hue = 0.0f;
        float saturation = 0.0f;
        float brightness = 0.0f;
        ImGui::ColorConvertRGBtoHSV(selectedColor[0], selectedColor[1], selectedColor[2], hue, saturation, brightness);
        float hueDegrees = hue * 360.0f;
        float saturationPct = saturation * 100.0f;
        float brightnessPct = brightness * 100.0f;
        float alphaPct = selectedColor[3] * 100.0f;

        bool hsbaChanged = false;
        hsbaChanged |= ImGui::SliderFloat("Hue", &hueDegrees, 0.0f, 360.0f, "%.1f deg");
        hsbaChanged |= ImGui::SliderFloat("Saturation", &saturationPct, 0.0f, 100.0f, "%.1f%%");
        hsbaChanged |= ImGui::SliderFloat("Brightness", &brightnessPct, 0.0f, 100.0f, "%.1f%%");
        if (s_showAlpha) {
            hsbaChanged |= ImGui::SliderFloat("Alpha", &alphaPct, 0.0f, 100.0f, "%.1f%%");
        }
        if (hsbaChanged && BeginBankEdit()) {
            while (hueDegrees < 0.0f) {
                hueDegrees += 360.0f;
            }
            while (hueDegrees >= 360.0f) {
                hueDegrees -= 360.0f;
            }
            ImGui::ColorConvertHSVtoRGB(hueDegrees / 360.0f,
                Clamp01(saturationPct / 100.0f),
                Clamp01(brightnessPct / 100.0f),
                selectedColor[0],
                selectedColor[1],
                selectedColor[2]);
            selectedColor[3] = Clamp01(alphaPct / 100.0f);
            SetEntryColor(s_selectedIndex, selectedColor);
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
                ImVec2(ModUI_Scale(40.0f), ModUI_Scale(40.0f)));
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
    ImGui::TextDisabled("Range operations apply to the selected span. Use Ctrl+Click and Shift+Click in the grid to set endpoints quickly.");
    const int visibleMaxIndex = GetVisibleMaxIndex();

    int rangeStart = (int)s_rangeStart;
    int rangeEnd = (int)s_rangeEnd;
    if (ImGui::DragIntRange2("Entry Range",
            &rangeStart,
            &rangeEnd,
            1.0f,
            0,
            visibleMaxIndex,
            "Start: %d",
            "End: %d")) {
        s_rangeStart = (uint8_t)ClampInt(rangeStart, 0, visibleMaxIndex);
        s_rangeEnd = (uint8_t)ClampInt(rangeEnd, 0, visibleMaxIndex);
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
    ImGuiColorEditFlags gradientFlags = ImGuiColorEditFlags_DisplayHSV;
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
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, ModUI_Scale(72.0f));
        ImGui::TableSetupColumn("Selected Entry");
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, ModUI_Scale(120.0f));
        ImGui::TableHeadersRow();

        for (int sourceIndex = 0; sourceIndex < 4; ++sourceIndex) {
            const NetplayPaletteBankSource source = (NetplayPaletteBankSource)sourceIndex;
            NetplayPaletteBank bank{};
            if (!NetplayPaletteRuntime_CopyLocalBankForSource(source, &bank)) {
                continue;
            }

            ImGui::PushID(sourceIndex);

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
                ImVec2(ModUI_Scale(40.0f), ModUI_Scale(20.0f)));

            ImGui::TableNextColumn();
            ImGui::Text("RGBA %02X %02X %02X %02X", entry[2], entry[1], entry[0], entry[3]);
            ImGui::TextDisabled("CRC 0x%08X  Visible:%d", bank.crc32, GetVisibleEntryCountForBank(bank));

            ImGui::TableNextColumn();
            if (ImGui::Button(BankSourceLabel(source))) {
                ReloadWorkingBank(source);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::TextDisabled("Current source: %s  |  Live:%s  Applied:%s  Saved:%s",
        BankSourceLabel(s_loadedSource),
        context.has_live_bank ? "yes" : "no",
        context.has_applied_custom_bank ? "yes" : "no",
        context.has_saved_custom_bank ? "yes" : "no");
}

static void RenderPresetTools() {
    EnsurePresetListCurrent();

    ImGui::TextDisabled("Named presets are scoped to the current character and base palette. Loading a preset only changes the editor until you apply it live or save it as the active custom bank.");
    ImGui::TextDisabled("Preset scope: Char %u  Base %u", s_workingBank.character_id, s_workingBank.base_palette);

    if (ImGui::BeginTable("PresetToolsLayout", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("PresetList", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("PresetActions", ImGuiTableColumnFlags_WidthStretch, 0.9f);

        ImGui::TableNextColumn();
        ImGui::Text("Saved Presets (%d)", s_presetCount);
        ImGui::BeginChild("PresetList", ImVec2(0.0f, ModUI_Scale(200.0f)), true);
        for (int presetIndex = 0; presetIndex < s_presetCount; ++presetIndex) {
            const bool isSelected = presetIndex == s_selectedPreset;
            if (ImGui::Selectable(s_presets[presetIndex].name, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                s_selectedPreset = presetIndex;
                CopyText(s_presetName, sizeof(s_presetName), s_presets[presetIndex].name);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    LoadPresetIntoWorkingBank(presetIndex, false);
                }
            }
            if (ImGui::IsItemHovered()) {
                RenderPresetTooltip(presetIndex);
            }
        }
        if (s_presetCount <= 0) {
            ImGui::TextDisabled("No named presets saved for this bank yet.");
        }
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::Text("Actions");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("Preset Name", s_presetName, sizeof(s_presetName));
        ImGui::TextDisabled("Invalid filename characters are replaced with underscores.");

        const bool hasSelectedPreset = s_selectedPreset >= 0 && s_selectedPreset < s_presetCount;

        ImGui::BeginDisabled(!hasSelectedPreset);
        if (ImGui::Button("Load To Editor", ImVec2(-FLT_MIN, 0.0f))) {
            LoadPresetIntoWorkingBank(s_selectedPreset, false);
        }
        if (ImGui::Button("Apply Selected Live", ImVec2(-FLT_MIN, 0.0f))) {
            LoadPresetIntoWorkingBank(s_selectedPreset, true);
        }
        if (ImGui::Button("Delete Selected", ImVec2(-FLT_MIN, 0.0f))) {
            DeleteSelectedPreset();
        }
        ImGui::EndDisabled();

        if (ImGui::Button("Save Current As Preset", ImVec2(-FLT_MIN, 0.0f))) {
            SaveWorkingBankAsPreset(s_presetName, false);
        }
        if (ImGui::Button("Refresh Presets", ImVec2(-FLT_MIN, 0.0f))) {
            RefreshPresetList();
            SetStatusText(s_presetStatus, sizeof(s_presetStatus), "Refreshed preset list.");
        }

        if (ImGui::Button("Random Load", ImVec2(-FLT_MIN, 0.0f))) {
            const int randomIndex = PickRandomPresetIndex();
            if (randomIndex >= 0) {
                LoadPresetIntoWorkingBank(randomIndex, false);
            } else {
                SetStatusText(s_presetStatus, sizeof(s_presetStatus), "No presets available to randomize.");
            }
        }
        if (ImGui::Button("Random Apply Live", ImVec2(-FLT_MIN, 0.0f))) {
            const int randomIndex = PickRandomPresetIndex();
            if (randomIndex >= 0) {
                LoadPresetIntoWorkingBank(randomIndex, true);
            } else {
                SetStatusText(s_presetStatus, sizeof(s_presetStatus), "No presets available to randomize.");
            }
        }

        if (hasSelectedPreset) {
            NetplayPaletteBank previewBank{};
            if (LoadPresetByIndex(s_selectedPreset, &previewBank)) {
                ImGui::Separator();
                ImGui::TextDisabled("Preview: %s", s_presets[s_selectedPreset].name);
                RenderBankPreviewRow(previewBank, 16);
                ImGui::TextDisabled("Visible:%d/%d  CRC:0x%08X",
                    GetVisibleEntryCountForBank(previewBank),
                    kPaletteEntryCount,
                    previewBank.crc32);
            }
        }

        ImGui::EndTable();
    }

    if (s_presetStatus[0]) {
        ImGui::TextWrapped("%s", s_presetStatus);
    }

    if (ImGui::BeginPopupModal("Overwrite Preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("'%s' already exists. Overwrite it?", s_pendingPresetOverwrite);
        ImGui::Separator();

        if (ImGui::Button("Overwrite", ImVec2(ModUI_Scale(120.0f), 0.0f))) {
            SaveWorkingBankAsPreset(s_pendingPresetOverwrite, true);
            s_pendingPresetOverwrite[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ModUI_Scale(120.0f), 0.0f))) {
            s_pendingPresetOverwrite[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace

void PaletteEditor_Init() {
    memset(&s_workingBank, 0, sizeof(s_workingBank));
    s_hasWorkingBank = false;
    s_selectedIndex = 0;
    s_rangeStart = 0;
    s_rangeEnd = 15;
    s_hoveredIndex = 0xFF;
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
    s_showGridIndices = true;
    s_highlightGrid = false;
    s_hideEmptyTail = true;
    s_gridCellSize = 24.0f;
    s_pickerScale = 1.0f;
    s_presetCount = 0;
    s_selectedPreset = -1;
    s_presetCharacter = 0xFF;
    s_presetBasePalette = 0xFF;
    s_presetName[0] = '\0';
    s_loadedPresetName[0] = '\0';
    s_pendingPresetOverwrite[0] = '\0';
    s_presetStatus[0] = '\0';
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

    if ((!s_hasWorkingBank && (context.has_live_bank ||
            context.has_applied_custom_bank ||
            context.has_saved_custom_bank ||
            context.has_vanilla_bank)) ||
        context.game_slot != s_loadedGameSlot ||
        context.character_id != s_loadedCharacter ||
        context.base_palette != s_loadedBasePalette ||
        !IsSourceAvailable(context, s_loadedSource)) {
        ReloadWorkingBank(GetDefaultSource(context));
    }

    RenderPaletteSummary(context);
    RenderPaletteToolbar(context);

    if (!s_hasWorkingBank) {
        ImGui::Separator();
        ImGui::TextDisabled("Palette bank not captured yet. Start or reload a match to populate live memory or source data.");
        return;
    }

    ImGui::Separator();
    if (ImGui::BeginTable("PaletteEditorLayout", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Palette", ImGuiTableColumnFlags_WidthStretch, 1.45f);
        ImGui::TableSetupColumn("Tools", ImGuiTableColumnFlags_WidthStretch, 1.0f);

        ImGui::TableNextColumn();
        RenderPaletteGrid(context);

        ImGui::TableNextColumn();
        if (ImGui::CollapsingHeader("Selected Color", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderSelectedTools(context);
        }
        if (ImGui::CollapsingHeader("Range, Gradient, Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderRangeTools(context);
        }
        if (ImGui::CollapsingHeader("Source Banks", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderSourceCompare(context);
        }
        if (ImGui::CollapsingHeader("Saved Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderPresetTools();
        }

        ImGui::EndTable();
    }
}
