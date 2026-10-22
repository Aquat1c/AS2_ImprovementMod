#include "ui/palette_editor.h"
#include "ui/palette_ramps.h"
#include "ui/strings.h"
#include "net/netplay_menu_render.h"
#include "patches/palette_asset_hook.h"
#include "patches/memory_utils.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"

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
    char               action[40];
    NetplayPaletteBank bank;
    NetplayPaletteBank ramp_baseline;
    uint8_t            selected_index;
    uint8_t            range_start;
    uint8_t            range_end;
};

static bool                    s_nativeOpen = false;
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
static NetplayPaletteBank      s_rampBaseline = {};
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

static void CaptureHistoryEntry(PaletteHistoryEntry* out, const char* action = nullptr) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->valid = s_hasWorkingBank;
    CopyText(out->action, sizeof(out->action), action ? action : "Edit");
    if (s_hasWorkingBank) {
        out->bank = s_workingBank;
        out->ramp_baseline = s_rampBaseline;
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

static void PushUndoSnapshot(const char* action = nullptr) {
    PaletteHistoryEntry entry{};
    CaptureHistoryEntry(&entry, action);
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

static bool ReplaceWorkingBank(const NetplayPaletteBank& bank, bool keepSelection, bool pushUndo, const char* action = nullptr) {
    if (!bank.valid) {
        return false;
    }

    if (pushUndo && s_hasWorkingBank) {
        PushUndoSnapshot(action);
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

    if (!ReplaceWorkingBank(bank, true, true, "Load preset")) {
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

static bool BeginBankEdit(const char* action = nullptr) {
    if (!s_hasWorkingBank) {
        return false;
    }
    PushUndoSnapshot(action);
    return true;
}

static bool UndoWorkingBank(const NetplayPaletteLocalContext& context) {
    if (!s_hasWorkingBank || s_undoCount <= 0) {
        return false;
    }

    const PaletteHistoryEntry entry = s_undoHistory[s_undoCount - 1];
    memset(&s_undoHistory[s_undoCount - 1], 0, sizeof(s_undoHistory[0]));
    --s_undoCount;

    PaletteHistoryEntry current{};
    CaptureHistoryEntry(&current, entry.action);
    PushHistoryEntry(s_redoHistory, &s_redoCount, current);

    s_workingBank = entry.bank;
    s_rampBaseline = entry.ramp_baseline;
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

    const PaletteHistoryEntry entry = s_redoHistory[s_redoCount - 1];
    memset(&s_redoHistory[s_redoCount - 1], 0, sizeof(s_redoHistory[0]));
    --s_redoCount;

    PaletteHistoryEntry current{};
    CaptureHistoryEntry(&current, entry.action);
    PushHistoryEntry(s_undoHistory, &s_undoCount, current);

    s_workingBank = entry.bank;
    s_rampBaseline = entry.ramp_baseline;
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

            char label[64] = {};
            _snprintf_s(label, sizeof(label), _TRUNCATE, "P%d  -  Char %u, Pal %u",
                slot + 1, player.character_id, player.base_palette);
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f)) &&
                NetplayPaletteRuntime_SetOfflineEditorGameSlot((uint8_t)slot)) {
                ReloadWorkingBank(GetDefaultSource(context));
            }

            if (context.game_slot == slot) {
                ImGui::PopStyleColor(2);
            }
        }
        ImGui::EndTable();
    }
}

static void RenderPaletteSummary(const NetplayPaletteLocalContext& context) {
    ImGui::Text("P%d  |  Character %u, Palette %u", context.game_slot + 1, context.character_id, context.base_palette);
    if (s_loadedPresetName[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("  Preset: %s", s_loadedPresetName);
    }
}

static void RenderPaletteToolbar(const NetplayPaletteLocalContext& context) {
    // Primary action bar
    ImGui::BeginDisabled(s_undoCount <= 0);
    char undoLabel[24] = {};
    _snprintf_s(undoLabel, sizeof(undoLabel), _TRUNCATE, "Undo (%d)", s_undoCount);
    if (ImGui::Button(undoLabel)) {
        UndoWorkingBank(context);
    }
    if (s_undoCount > 0 && ImGui::BeginItemTooltip()) {
        ImGui::Text("Undo: %s  (Ctrl+Z)", s_undoHistory[s_undoCount - 1].action);
        ImGui::EndTooltip();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s_redoCount <= 0);
    char redoLabel[24] = {};
    _snprintf_s(redoLabel, sizeof(redoLabel), _TRUNCATE, "Redo (%d)", s_redoCount);
    if (ImGui::Button(redoLabel)) {
        RedoWorkingBank(context);
    }
    if (s_redoCount > 0 && ImGui::BeginItemTooltip()) {
        ImGui::Text("Redo: %s  (Ctrl+Y)", s_redoHistory[s_redoCount - 1].action);
        ImGui::EndTooltip();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, ModUI_Scale(12.0f));

    if (ImGui::Button("Apply") && s_hasWorkingBank) {
        ApplyWorkingBankLive();
    }
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Write current edits into live game memory.");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to Disk") && s_hasWorkingBank) {
        NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, true);
    }
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Save as the permanent custom palette for this character.");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        ReloadWorkingBank(s_loadedSource);
    }
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Discard all edits and reload from the current source.");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Custom")) {
        NetplayPaletteRuntime_ClearLocalCustomBank(true);
        ReloadWorkingBank(GetDefaultSource(context));
    }
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Delete the saved custom palette and revert to vanilla.");
        ImGui::EndTooltip();
    }

    // Second row: Auto-apply, Freeze, Source combo, Display settings
    ImGui::Checkbox("Auto-Apply", &s_autoApplyLive);
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Automatically push every edit to live memory.");
        ImGui::EndTooltip();
    }

    if (!context.match_active && PracticeTools_IsPracticeModeActive()) {
        ImGui::SameLine();
        bool paused = PracticeTools_IsPaused();
        if (ImGui::Checkbox("Freeze", &paused)) {
            PracticeTools_SetPaused(paused);
        }
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted("Pause the game to inspect palette changes.");
            ImGui::EndTooltip();
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ModUI_Scale(160.0f));
    if (ImGui::BeginCombo("Source", BankSourceLabel(s_loadedSource))) {
        for (int i = 0; i < 4; ++i) {
            const NetplayPaletteBankSource source = (NetplayPaletteBankSource)i;
            if (!IsSourceAvailable(context, source)) {
                continue;
            }
            const bool selected = (source == s_loadedSource);
            if (ImGui::Selectable(BankSourceLabel(source), selected)) {
                ReloadWorkingBank(source);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Display...")) {
        ImGui::OpenPopup("PaletteDisplaySettings");
    }

    if (ImGui::BeginPopup("PaletteDisplaySettings")) {
        ImGui::SeparatorText("Grid");
        ImGui::Checkbox("Show Row Labels", &s_showGridLabels);
        ImGui::Checkbox("Show Cell Indices", &s_showGridIndices);
        ImGui::Checkbox("Highlight Range", &s_highlightGrid);
        ImGui::Checkbox("Show Transparency", &s_showAlpha);
        if (ImGui::Checkbox("Hide Empty Tail", &s_hideEmptyTail)) {
            ClampSelectionToVisibleEntries();
            SyncGradientEndpointsFromRange();
            SyncHexInputFromSelection();
        }
        ImGui::SeparatorText("Sizing");
        ImGui::SetNextItemWidth(ModUI_Scale(140.0f));
        if (ImGui::SliderFloat("Cell Size", &s_gridCellSize, 18.0f, 32.0f, "%.0f px")) {
            s_gridCellSize = ClampFloat(s_gridCellSize, 18.0f, 32.0f);
        }
        ImGui::SetNextItemWidth(ModUI_Scale(140.0f));
        if (ImGui::SliderFloat("Picker Scale", &s_pickerScale, 0.75f, 1.75f, "%.2fx")) {
            s_pickerScale = ClampFloat(s_pickerScale, 0.75f, 1.75f);
        }
        ImGui::EndPopup();
    }
}

static void RenderPaletteGrid(const NetplayPaletteLocalContext& context) {
    const float uiScale = ModUI_GetScale();
    const float cellSize = s_gridCellSize * uiScale;
    const ImGuiIO& io = ImGui::GetIO();
    const int visibleCount = GetVisibleEntryCount();
    const int rowCount = (visibleCount + 15) / 16;
    s_hoveredIndex = 0xFF;
    ImGui::TextDisabled("Click: select | Ctrl+Click: range start | Shift+Click: range end | Drag: swap | Right-click: tools");
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
                        if (sourceIndex != index && BeginBankEdit("Swap entries")) {
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
                if (ImGui::MenuItem("Paste Clipboard To Entry") && BeginBankEdit("Paste to entry")) {
                    s_selectedIndex = index;
                    SetEntryColor(index, s_clipboardColor);
                    FinishBankEdit(context);
                }
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Swap With Selected", nullptr, false, index != s_selectedIndex) && BeginBankEdit("Swap entries")) {
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

    // Compact entry navigation
    ImGui::BeginDisabled(s_selectedIndex <= 0);
    if (ImGui::ArrowButton("##PrevEntry", ImGuiDir_Left)) {
        --s_selectedIndex;
        SyncHexInputFromSelection();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    int selectedIndex = (int)s_selectedIndex;
    ImGui::SetNextItemWidth(ModUI_Scale(100.0f));
    if (ImGui::SliderInt("##EntryNav", &selectedIndex, 0, visibleMaxIndex)) {
        s_selectedIndex = (uint8_t)selectedIndex;
        SyncHexInputFromSelection();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s_selectedIndex >= visibleMaxIndex);
    if (ImGui::ArrowButton("##NextEntry", ImGuiDir_Right)) {
        ++s_selectedIndex;
        SyncHexInputFromSelection();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("/ %d", visibleMaxIndex);

    if (ImGui::SmallButton("Range Start")) {
        s_rangeStart = s_selectedIndex;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Range End")) {
        s_rangeEnd = s_selectedIndex;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Entry %u (0x%02X)", s_selectedIndex, s_selectedIndex);

    // Color picker
    float selectedColor[4] = {};
    LoadEntryColor(s_selectedIndex, selectedColor);

    if (s_hexIndex != s_selectedIndex || s_hexGameSlot != s_loadedGameSlot || s_hexBasePalette != s_loadedBasePalette) {
        SyncHexInputFromSelection();
    }

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
        BeginBankEdit("Color edit")) {
        SetEntryColor(s_selectedIndex, selectedColor);
        FinishBankEdit(context);
    }
    ImGui::PopItemWidth();

    // HSV fine-tune sliders
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
    if (hsbaChanged && BeginBankEdit("Color edit")) {
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

    // Hex input
    ImGui::SeparatorText("Hex Input");
    ImGui::SetNextItemWidth(ModUI_Scale(120.0f));
    bool applyHex = ImGui::InputText("##HexRGBA",
        s_hexInput,
        sizeof(s_hexInput),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply##Hex")) {
        applyHex = true;
    }
    if (applyHex) {
        float parsedColor[4] = {};
        if (ParseHexColor(s_hexInput, selectedColor[3], parsedColor) && BeginBankEdit("Hex edit")) {
            SetEntryColor(s_selectedIndex, parsedColor);
            FinishBankEdit(context);
        } else {
            SyncHexInputFromSelection();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("RRGGBB / RRGGBBAA");

    // Clipboard tools
    ImGui::SeparatorText("Clipboard");
    if (ImGui::Button("Copy")) {
        CopySelectedToClipboard();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!s_hasClipboardColor);
    if (ImGui::Button("Paste") && BeginBankEdit("Paste to entry")) {
        SetEntryColor(s_selectedIndex, s_clipboardColor);
        FinishBankEdit(context);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fill Range") && BeginBankEdit("Fill range")) {
        int rangeStart = 0;
        int rangeEnd = 0;
        GetRangeBounds(&rangeStart, &rangeEnd);
        FillRangeWithColor(rangeStart, rangeEnd, selectedColor);
        FinishBankEdit(context);
    }
    ImGui::EndDisabled();

    if (s_hasClipboardColor) {
        ImGui::ColorButton("##ClipPreview",
            ImVec4(s_clipboardColor[0], s_clipboardColor[1], s_clipboardColor[2], s_clipboardColor[3]),
            s_showAlpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha,
            ImVec2(ModUI_Scale(20.0f), ModUI_Scale(20.0f)));
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f, %.0f, %.0f, %.0f",
            s_clipboardColor[0] * 255.0f,
            s_clipboardColor[1] * 255.0f,
            s_clipboardColor[2] * 255.0f,
            s_clipboardColor[3] * 255.0f);
    }
}

static void RenderRangeTools(const NetplayPaletteLocalContext& context) {
    const int visibleMaxIndex = GetVisibleMaxIndex();

    // Range selection
    ImGui::SeparatorText("Range");
    int rangeStart = (int)s_rangeStart;
    int rangeEnd = (int)s_rangeEnd;
    if (ImGui::DragIntRange2("##Range",
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
    ImGui::TextDisabled("%d entries selected", normalizedEnd - normalizedStart + 1);

    if (ImGui::SmallButton("Select All")) {
        s_rangeStart = 0;
        s_rangeEnd = (uint8_t)visibleMaxIndex;
        SyncGradientEndpointsFromRange();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Single Entry")) {
        s_rangeStart = s_selectedIndex;
        s_rangeEnd = s_selectedIndex;
        SyncGradientEndpointsFromRange();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reverse") && BeginBankEdit("Reverse range")) {
        ReverseRange(normalizedStart, normalizedEnd);
        FinishBankEdit(context);
        SyncGradientEndpointsFromRange();
    }
    ImGui::TextDisabled("Ctrl+Click grid = range start, Shift+Click = range end");

    // Gradient
    ImGui::SeparatorText("Gradient");
    ImGuiColorEditFlags gradientFlags = ImGuiColorEditFlags_DisplayHSV;
    if (!s_showAlpha) {
        gradientFlags |= ImGuiColorEditFlags_NoAlpha;
    }
    ImGui::ColorEdit4("Start##Grad", s_gradientStart, gradientFlags);
    ImGui::ColorEdit4("End##Grad", s_gradientEnd, gradientFlags);

    if (ImGui::SmallButton("From Selected##GradS")) {
        LoadEntryColor(s_selectedIndex, s_gradientStart);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("From Selected##GradE")) {
        LoadEntryColor(s_selectedIndex, s_gradientEnd);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("From Range Ends")) {
        SyncGradientEndpointsFromRange();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!s_hasClipboardColor);
    if (ImGui::SmallButton("Clipboard##GradE")) {
        memcpy(s_gradientEnd, s_clipboardColor, sizeof(s_gradientEnd));
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Apply Gradient", ImVec2(-FLT_MIN, 0.0f)) && BeginBankEdit("Apply gradient")) {
        ApplyGradientToRange(normalizedStart, normalizedEnd, s_gradientStart, s_gradientEnd);
        FinishBankEdit(context);
    }

    // HSV Transform
    ImGui::SeparatorText("HSV Transform");
    ImGui::SliderFloat("Hue Shift", &s_hueShiftDegrees, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat("Sat Scale", &s_saturationScale, 0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Val Scale", &s_valueScale, 0.0f, 2.0f, "%.2fx");
    if (s_showAlpha) {
        ImGui::SliderFloat("Alpha Scale", &s_alphaScale, 0.0f, 2.0f, "%.2fx");
    }

    if (ImGui::Button("Apply Transform", ImVec2(ModUI_Scale(160.0f), 0.0f)) && BeginBankEdit("Apply transform")) {
        ApplyTransformToRange(normalizedStart, normalizedEnd);
        FinishBankEdit(context);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(ModUI_Scale(80.0f), 0.0f))) {
        ResetTransformState();
    }
}

static void RenderSourceCompare(const NetplayPaletteLocalContext& context) {
    ImGui::TextDisabled("Compare entry %u across all available source banks.", s_selectedIndex);

    if (ImGui::BeginTable("SourceCompare", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, ModUI_Scale(56.0f));
        ImGui::TableSetupColumn("Color");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ModUI_Scale(60.0f));
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
                ImGui::SameLine();
                ImGui::TextDisabled("(active)");
            }

            ImGui::TableNextColumn();
            ImGui::ColorButton("##sourceColor",
                preview,
                s_showAlpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha,
                ImVec2(ModUI_Scale(40.0f), ModUI_Scale(20.0f)));

            ImGui::TableNextColumn();
            ImGui::Text("#%02X%02X%02X%02X", entry[2], entry[1], entry[0], entry[3]);

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Load")) {
                ReloadWorkingBank(source);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

static void RenderPresetTools() {
    EnsurePresetListCurrent();

    ImGui::TextDisabled("Presets for Character %u, Palette %u", s_workingBank.character_id, s_workingBank.base_palette);

    // Preset list
    ImGui::Text("Saved Presets (%d)", s_presetCount);
    ImGui::BeginChild("PresetList", ImVec2(0.0f, ModUI_Scale(160.0f)), true);
    for (int presetIndex = 0; presetIndex < s_presetCount; ++presetIndex) {
        const bool isSelected = presetIndex == s_selectedPreset;
        if (ImGui::Selectable(s_presets[presetIndex].name, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
            s_selectedPreset = presetIndex;
            CopyText(s_presetName, sizeof(s_presetName), s_presets[presetIndex].name);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                LoadPresetIntoWorkingBank(presetIndex, s_autoApplyLive);
            }
        }
        if (ImGui::IsItemHovered()) {
            RenderPresetTooltip(presetIndex);
        }
    }
    if (s_presetCount <= 0) {
        ImGui::TextDisabled("No presets saved yet.");
    }
    ImGui::EndChild();

    // Preview of selected preset
    const bool hasSelectedPreset = s_selectedPreset >= 0 && s_selectedPreset < s_presetCount;
    if (hasSelectedPreset) {
        NetplayPaletteBank previewBank{};
        if (LoadPresetByIndex(s_selectedPreset, &previewBank)) {
            RenderBankPreviewRow(previewBank, 16);
        }
    }

    // Actions
    ImGui::SeparatorText("Actions");
    ImGui::BeginDisabled(!hasSelectedPreset);
    if (ImGui::Button("Load")) {
        LoadPresetIntoWorkingBank(s_selectedPreset, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load & Apply")) {
        LoadPresetIntoWorkingBank(s_selectedPreset, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        DeleteSelectedPreset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Random")) {
        const int randomIndex = PickRandomPresetIndex();
        if (randomIndex >= 0) {
            LoadPresetIntoWorkingBank(randomIndex, s_autoApplyLive);
        } else {
            SetStatusText(s_presetStatus, sizeof(s_presetStatus), "No other presets available.");
        }
    }
    ImGui::EndDisabled();

    // Save
    ImGui::SeparatorText("Save");
    ImGui::SetNextItemWidth(ModUI_Scale(200.0f));
    ImGui::InputText("Name", s_presetName, sizeof(s_presetName));
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        SaveWorkingBankAsPreset(s_presetName, false);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
        RefreshPresetList();
        SetStatusText(s_presetStatus, sizeof(s_presetStatus), "Refreshed.");
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
    if (s_nativeOpen) return;
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

    // Keyboard shortcuts
    if (s_hasWorkingBank && !ImGui::GetIO().WantTextInput) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) {
                RedoWorkingBank(context);
            } else {
                UndoWorkingBank(context);
            }
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            RedoWorkingBank(context);
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopySelectedToClipboard();
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            if (s_hasClipboardColor && BeginBankEdit("Paste to entry")) {
                SetEntryColor(s_selectedIndex, s_clipboardColor);
                FinishBankEdit(context);
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (BeginBankEdit("Clear entry")) {
                const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                SetEntryColor(s_selectedIndex, zero);
                FinishBankEdit(context);
            }
        }
    }

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
        if (ImGui::BeginTabBar("PaletteToolTabs")) {
            if (ImGui::BeginTabItem("Color")) {
                RenderSelectedTools(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Effects")) {
                RenderRangeTools(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Presets")) {
                RenderPresetTools();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Banks")) {
                RenderSourceCompare(context);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::EndTable();
    }
}

namespace {
NetplayPaletteBank n_opening{};
PaletteRamps::Group n_groups[256]{};
int n_groupCount = 0, n_group = 0, n_shade = 0, n_field = 3, n_frame = -1;
int n_visibleCount = 0;
float n_hue = 0, n_saturation = 0, n_value = 0;
constexpr int kSwatchesPerPage = 24;
constexpr int kPickerLeft = 284, kPickerRight = 532, kPickerTop = 246, kPickerBottom = 362;
constexpr int kHueLeft = 550, kHueRight = 570;
bool n_grouped = true, n_grid = false, n_hadCustom = false, n_exit = false;
int n_exitChoice = 0, n_destination = -1;
bool n_mouseDown = false, n_dragSnapshot = false;
bool n_rightDown = false, n_original = false, n_save = false, n_overwrite = false, n_saveLeave = false,
     n_hex = false;
constexpr int kNativeSaveSlots = 8;
int n_overwriteChoice = 0;
int n_saveSlot = 0, n_previousFrame = 0, n_hexDigit = 0;
float n_zoom = 1, n_panX = 0, n_panY = 0;
bool n_panDragging = false;
int n_panLastX = 0, n_panLastY = 0;
SRWLOCK n_wheelLock = SRWLOCK_INIT;
int n_wheelDelta = 0;
POINT n_wheelPoint{};
char n_hexText[7] = {};
bool n_hexKeys[256] = {};
int n_dragControl = 0;
const char *n_error = nullptr;
using Ui::S;
using Ui::Str;
bool NativeDirty() { return memcmp(s_workingBank.data, n_opening.data, sizeof(n_opening.data)) != 0; }
bool NativeContext(NetplayPaletteLocalContext &context) {
    NetplayPaletteRuntime_GetLocalContext(&context);
    return context.available && !context.match_active && GetGameMode() == MODE_MATCH &&
           GetGameType() == GAMETYPE_TRAINING && context.game_slot == s_loadedGameSlot &&
           context.character_id == s_loadedCharacter && context.base_palette == s_loadedBasePalette;
}
int NativeCount() { return n_grouped ? n_groupCount : n_visibleCount; }
PaletteRamps::Group NativeGroup() {
    return n_grouped && n_groupCount ? n_groups[n_group]
                                     : PaletteRamps::Group{n_group + 1, n_group + 1, n_group + 1};
}
int NativeIndex() {
    auto g = NativeGroup();
    return n_shade < 0 ? g.key : g.first + n_shade;
}
void NativeSyncPicker() {
    float color[4];
    LoadEntryColor((uint8_t)NativeIndex(), color);
    float hue;
    ImGui::ColorConvertRGBtoHSV(color[0], color[1], color[2], hue, n_saturation, n_value);
    if (n_saturation > 0 && n_value > 0)
        n_hue = hue;
}
void NativeSelection() {
    n_shade = -1;
    s_selectedIndex = (uint8_t)NativeGroup().key;
    NativeSyncPicker();
}
bool NativeLoad() {
    NetplayPaletteLocalContext context{};
    NetplayPaletteRuntime_GetLocalContext(&context);
    if (!context.available || context.match_active || !ReloadWorkingBank(GetDefaultSource(context)))
        return false;
    n_opening = s_workingBank;
    n_hadCustom = context.has_applied_custom_bank;
    s_rampBaseline = s_workingBank;
    NetplayPaletteBank layoutBank = s_workingBank;
    NetplayPaletteRuntime_CopyLocalBankForSource(NetplayPaletteBankSource::VanillaSource, &layoutBank);
    const int sourceLast = FindLastNonZeroEntry(layoutBank);
    const int editedLast = FindLastNonZeroEntry(s_workingBank);
    if (editedLast > sourceLast)
        memcpy(layoutBank.data + (sourceLast + 1) * 4, s_workingBank.data + (sourceLast + 1) * 4,
               (editedLast - sourceLast) * 4);
    n_groupCount = PaletteRamps::Build(layoutBank.data, n_groups);
    n_visibleCount = sourceLast > editedLast ? sourceLast : editedLast;
    n_group = 0;
    n_shade = -1;
    n_field = 3;
    n_frame = -1;
    n_saveSlot = ClampInt(s_loadedBasePalette, 0, kNativeSaveSlots - 1);
    n_previousFrame = 0;
    n_zoom = 1;
    n_panX = n_panY = 0;
    n_panDragging = false;
    AcquireSRWLockExclusive(&n_wheelLock);
    n_wheelDelta = 0;
    ReleaseSRWLockExclusive(&n_wheelLock);
    n_original = false;
    n_save = false;
    n_hex = false;
    n_dragControl = 0;
    n_dragSnapshot = false;
    n_grid = true;
    n_exit = false;
    n_error = nullptr;
    if (!n_groupCount)
        n_grouped = false;
    NativeSelection();
    return true;
}
bool NativeSave() {
    if (n_saveSlot < 0 || n_saveSlot >= kNativeSaveSlots)
        return false;
    NetplayPaletteBank bank = s_workingBank;
    bank.base_palette = (uint8_t)n_saveSlot;
    bank.crc32 = CalcCRC32(bank.data, sizeof(bank.data));
    bool saved = n_saveSlot == s_loadedBasePalette
                     ? NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, true)
                     : NetplayPaletteStorage_SaveBank(&bank);
    if (!saved) {
        n_error = S(Str::Pe_SaveFailed);
        return false;
    }
    n_opening = s_workingBank;
    n_hadCustom = true;
    n_original = false;
    NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false);
    n_error = nullptr;
    return true;
}
void NativeBeginSave(bool leave) {
    n_save = true;
    n_overwrite = false;
    n_saveLeave = leave;
    n_saveSlot = ClampInt(s_loadedBasePalette, 0, kNativeSaveSlots - 1);
    n_error = nullptr;
    NetplayPaletteStorage_ClearCache();
}
bool NativeConfirmSave() {
    if (!n_overwrite && NetplayPaletteStorage_HasBank(s_loadedCharacter, (uint8_t)n_saveSlot)) {
        n_overwrite = true;
        n_overwriteChoice = 0;
        return false;
    }
    if (!NativeSave())
        return false;
    n_save = false;
    n_overwrite = false;
    return true;
}
void NativeToggleOriginal() {
    if (!n_original) {
        NetplayPaletteBank bank{};
        if (!NetplayPaletteRuntime_CopyLocalBankForSource(NetplayPaletteBankSource::VanillaSource, &bank))
            return;
        if (!NetplayPaletteRuntime_SetLocalCustomBank(&bank, false))
            return;
    } else if (!NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false))
        return;
    n_original = !n_original;
}
void NativeToggleCurrentFrame() {
    if (n_frame < 0)
        n_frame = n_previousFrame;
    else {
        n_previousFrame = n_frame;
        n_frame = -1;
    }
}
void NativeZoom(float step, float anchorX = 160, float anchorY = 247) {
    float previous = n_zoom;
    n_zoom = fminf(4.f, fmaxf(.25f, n_zoom + step * .25f));
    float ratio = n_zoom / previous;
    n_panX = (anchorX - 160) - (anchorX - 160 - n_panX) * ratio;
    n_panY = (anchorY - 247) - (anchorY - 247 - n_panY) * ratio;
}
void NativeConsumeWheel(bool apply) {
    AcquireSRWLockExclusive(&n_wheelLock);
    const float wheel = (float)n_wheelDelta / WHEEL_DELTA;
    const POINT point = n_wheelPoint;
    n_wheelDelta = 0;
    ReleaseSRWLockExclusive(&n_wheelLock);
    if (apply && wheel)
        NativeZoom(wheel, (float)point.x, (float)point.y);
}
bool NativePreviewPointer(int x, int y, bool down, bool click, float wheel) {
    if (!down)
        n_panDragging = false;
    if (n_panDragging) {
        n_panX += x - n_panLastX;
        n_panY += y - n_panLastY;
        n_panLastX = x;
        n_panLastY = y;
        return true;
    }
    if (x >= 56 && x < 264 && y >= 140 && y < 350) {
        if (wheel)
            NativeZoom(wheel, (float)x, (float)y);
        if (click) {
            n_panDragging = true;
            n_panLastX = x;
            n_panLastY = y;
        }
        return click || wheel != 0;
    }
    if (click && y >= 354 && y < 378) {
        int step = x >= 56 && x < 96 ? -1 : x >= 224 && x < 264 ? 1 : 0;
        if (step) {
            n_field = 13;
            n_grid = false;
            NativeZoom((float)step);
            return true;
        }
    }
    return false;
}
void NativeDiscard() {
    if (n_hadCustom)
        NetplayPaletteRuntime_SetLocalCustomBank(&n_opening, false);
    else
        NetplayPaletteRuntime_ClearLocalCustomBank(false);
    s_workingBank = n_opening;
}
bool NativeLeave() {
    NativeDiscard();
    if (n_destination < 0) {
        s_nativeOpen = false;
        return false;
    }
    if (!NetplayPaletteRuntime_SetOfflineEditorGameSlot((uint8_t)n_destination) || !NativeLoad()) {
        n_error = S(Str::Pe_Unavailable);
        n_exit = false;
        return true;
    }
    return true;
}
bool NativeSetRgb(uint8_t red, uint8_t green, uint8_t blue, bool snapshot = true) {
    auto group = NativeGroup();
    int index = NativeIndex();
    const uint8_t *entry = s_workingBank.data + index * 4;
    if (entry[2] == red && entry[1] == green && entry[0] == blue)
        return false;
    if (snapshot)
        BeginBankEdit("Recolour");
    if (n_grouped && n_shade < 0) {
        PaletteRamps::Recolor(s_rampBaseline.data, s_workingBank.data, group, red, green, blue);
    } else {
        uint8_t *e = s_workingBank.data + index * 4;
        e[2] = red;
        e[1] = green;
        e[0] = blue;
        for (int i = 0; i < n_groupCount; ++i) {
            const auto &ramp = n_groups[i];
            if (index >= ramp.first && index <= ramp.last) {
                memcpy(s_rampBaseline.data + ramp.first * 4, s_workingBank.data + ramp.first * 4,
                       (ramp.last - ramp.first + 1) * 4);
                break;
            }
        }
    }
    n_original = false;
    NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false);
    return true;
}
bool NativeSetHsv(float hue, float saturation, float value, bool snapshot = true) {
    n_hue = hue - floorf(hue);
    n_saturation = Clamp01(saturation);
    n_value = Clamp01(value);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(n_hue, n_saturation, n_value, r, g, b);
    return NativeSetRgb(FloatToByte(r), FloatToByte(g), FloatToByte(b), snapshot);
}
void NativeDragHsv(float hue, float saturation, float value) {
    n_dragSnapshot = NativeSetHsv(hue, saturation, value, !n_dragSnapshot) || n_dragSnapshot;
}
bool NativeApplyHex(const char *text) {
    if (!text || strlen(text) != 6)
        return false;
    for (int i = 0; i < 6; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
              (text[i] >= 'A' && text[i] <= 'F')))
            return false;
    float color[4];
    if (!ParseHexColor(text, 0, color))
        return false;
    NativeSetRgb(FloatToByte(color[0]), FloatToByte(color[1]), FloatToByte(color[2]));
    NativeSyncPicker();
    return true;
}
void NativeBeginHex() {
    const auto *c = s_workingBank.data + NativeIndex() * 4;
    snprintf(n_hexText, sizeof(n_hexText), "%02X%02X%02X", c[2], c[1], c[0]);
    n_hex = true;
    n_hexDigit = 0;
    for (int key = 0; key < 256; ++key)
        n_hexKeys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
}
void NativeHexInput(uint16_t pressed) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        for (int key = 0; key < 256; ++key)
            n_hexKeys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
        return;
    }
    bool typed = false, enter = false, escape = false;
    for (int key = 0; key < 256; ++key) {
        bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        bool edge = down && !n_hexKeys[key];
        n_hexKeys[key] = down;
        if (!edge)
            continue;
        int digit = key >= '0' && key <= '9'                 ? key - '0'
                    : key >= 'A' && key <= 'F'               ? key - 'A' + 10
                    : key >= VK_NUMPAD0 && key <= VK_NUMPAD9 ? key - VK_NUMPAD0
                                                             : -1;
        if (digit >= 0) {
            n_hexText[n_hexDigit] = "0123456789ABCDEF"[digit];
            n_hexDigit = (n_hexDigit + 1) % 6;
            typed = true;
        }
        if (key == VK_RETURN)
            enter = true;
        if (key == VK_ESCAPE)
            escape = true;
    }
    if (typed)
        return;
    if (escape || (pressed & INPUT_B)) {
        n_hex = false;
        return;
    }
    if (pressed & INPUT_LEFT)
        n_hexDigit = (n_hexDigit + 5) % 6;
    if (pressed & INPUT_RIGHT)
        n_hexDigit = (n_hexDigit + 1) % 6;
    int change = (pressed & INPUT_UP) ? 1 : (pressed & INPUT_DOWN) ? -1 : 0;
    if (change) {
        const char *digits = "0123456789ABCDEF";
        int value = (int)(strchr(digits, n_hexText[n_hexDigit]) - digits);
        n_hexText[n_hexDigit] = digits[(value + change + 16) % 16];
    }
    if (enter || (pressed & INPUT_A)) {
        if (NativeApplyHex(n_hexText))
            n_hex = false;
    }
}
bool NativePickerPointer(int x, int y, bool click) {
    if (click && y >= kPickerTop && y <= kPickerBottom) {
        if (x >= kPickerLeft && x <= kPickerRight)
            n_dragControl = 5;
        else if (x >= kHueLeft && x <= kHueRight)
            n_dragControl = 6;
    }
    if (!n_dragControl)
        return false;
    n_field = n_dragControl;
    n_grid = false;
    if (n_dragControl == 5)
        NativeDragHsv(n_hue, (float)(x - kPickerLeft) / (kPickerRight - kPickerLeft),
                      1.f - (float)(y - kPickerTop) / (kPickerBottom - kPickerTop));
    else
        NativeDragHsv(Clamp01((float)(y - kPickerTop) / (kPickerBottom - kPickerTop)) * .999999f,
                      n_saturation, n_value);
    return true;
}
bool NativeScreenPoint(POINT &point) {
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    RECT client{};
    if (pid != GetCurrentProcessId() || !ScreenToClient(window, &point) || !GetClientRect(window, &client))
        return false;
    float width = (float)client.right, height = (float)client.bottom, left = 0, top = 0;
    using KeepAspect = bool (*)();
    auto keep = (KeepAspect)GetProcAddress(GetModuleHandleA("d3d9.dll"), "IsKeepAspectRatio");
    if (keep && keep()) {
        float scale = fminf(width / 640, height / 480);
        left = (width - 640 * scale) * .5f;
        top = (height - 480 * scale) * .5f;
        width = 640 * scale;
        height = 480 * scale;
    }
    if (width <= 0 || height <= 0)
        return false;
    point.x = (int)((point.x - left) * 640 / width);
    point.y = (int)((point.y - top) * 480 / height);
    return true;
}
uint16_t NativePointer() {
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, click = down && !n_mouseDown;
    n_mouseDown = down;
    bool right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0, rightClick = right && !n_rightDown;
    n_rightDown = right;
    if (!down) {
        n_dragSnapshot = false;
        n_dragControl = 0;
    }
    POINT point{};
    if (!GetCursorPos(&point) || !NativeScreenPoint(point)) {
        n_panDragging = false;
        NativeConsumeWheel(false);
        return 0;
    }
    NativeConsumeWheel(!n_hex && !n_save && !n_exit);
    const int x = point.x, y = point.y;
    if (n_hex || n_save || n_exit)
        n_panDragging = false;
    if (n_hex) {
        if (click && y >= 225 && y <= 267 && x >= 180 && x < 444)
            n_hexDigit = (x - 180) / 44;
        if (click && y >= 284 && y < 318) {
            if (x >= 160 && x < 308)
                return INPUT_A;
            if (x >= 332 && x < 480)
                return INPUT_B;
        }
        return 0;
    }
    if (n_save) {
        if (click && !n_overwrite && x >= 208 && x < 432 && y >= 132 && y < 388) {
            n_saveSlot = (y - 132) / 32;
            return INPUT_A;
        }
        if (click && n_overwrite && x >= 172 && x < 468 && y >= 196 && y < 276) {
            n_overwriteChoice = (y - 196) / 40;
            return INPUT_A;
        }
        return 0;
    }
    if (n_exit) {
        if (click && x >= 172 && x < 468 && y >= 196 && y < 310) {
            n_exitChoice = (y - 196) / 38;
            return INPUT_A;
        }
        return 0;
    }
    if (rightClick && x >= 144 && x < 264 && y >= 108 && y < 132) {
        NativeToggleCurrentFrame();
        return 0;
    }
    if (click && x >= 284 && x < 416 && y >= 366 && y < 385) {
        n_field = 7;
        n_grid = false;
        NativeBeginHex();
        return 0;
    }
    if (NativePreviewPointer(x, y, down, click, 0))
        return 0;
    if (click && x >= 284 && x < 572 && y >= 143 && y < 215) {
        int index = (n_group / kSwatchesPerPage) * kSwatchesPerPage + (y - 143) / 24 * 8 + (x - 284) / 36;
        if (index < NativeCount()) {
            n_group = index;
            NativeSelection();
            n_field = 3;
            n_grid = true;
        }
    } else if (click && x >= 284 && x < 572 && y >= 221 && y < 236) {
        auto g = NativeGroup();
        n_shade = (x - 284) * (g.last - g.first + 1) / 288;
        s_selectedIndex = (uint8_t)NativeIndex();
        NativeSyncPicker();
        n_field = 4;
        n_grid = false;
    } else if (NativePickerPointer(x, y, click)) {
        return 0;
    } else if (click) {
        n_grid = false;
        if (y >= 108 && y < 132) {
            if (x >= 64 && x < 134) {
                n_field = 0;
                return INPUT_RIGHT;
            }
            if (x >= 144 && x < 264) {
                n_field = 1;
                return INPUT_RIGHT;
            }
            if (x >= 284 && x < 572) {
                n_field = 2;
                return INPUT_A;
            }
        }
        if (y >= 386 && y < 410) {
            const int edges[] = {64, 144, 224, 352, 480, 576};
            for (int i = 0; i < 5; ++i)
                if (x >= edges[i] && x < edges[i + 1]) {
                    n_field = 8 + i;
                    return INPUT_A;
                }
        }
    }
    return 0;
}
void NativeUnderline(int left, int right, int y) {
    NetMenu::MenuSetBlend(0, 255);
    NetMenu::MenuFillRect(left, y, right, y + 1, 248, 192, 72);
}
void NativeSelectBar(int left, int top, int right, int bottom, bool, uint8_t alpha) {
    NetMenu::MenuSetBlend(2, (uint8_t)(alpha * 80 / 255));
    NetMenu::MenuFillRect(left, top, right, bottom, 255, 0, 0);
    NetMenu::MenuSetBlend(0, 255);
}
int NativeKeycap(int x, int y, const char *key, const char *label, uint8_t) {
    NetMenu::MenuDrawTextSized(x, y, 248, 192, 72, 18, key);
    NetMenu::MenuDrawTextSized(x + 22, y, 176, 168, 150, 18, label);
    return 38 + (int)NetMenu::MenuMeasureText(label, 18);
}
void NativeText(int x, int y, const char *text, bool selected = false, float size = 22) {
    NetMenu::MenuSetTextAlpha(255);
    NetMenu::MenuDrawTextSized(x, y, selected ? 248 : 236, selected ? 192 : 232, selected ? 72 : 224, size,
                               text);
}
void NativeRule(int left, int right, int y) {
    NetMenu::MenuSetBlend(0, 255);
    NetMenu::MenuFillRect(left, y, right, y + 2, 160, 116, 40);
}
void NativePanel(int left, int top, int right, int bottom, int headerBottom, const char *title) {
    NetMenu::MenuSetBlend(0, 255);
    NetMenu::MenuFillRect(left, top, right, bottom, 192, 144, 48);
    NetMenu::MenuFillRect(left + 2, top + 2, right - 2, bottom - 2, 22, 13, 6);
    NetMenu::MenuFillRect(left + 4, headerBottom + 2, right - 4, bottom - 4, 64, 54, 42);
    NativeRule(left + 4, right - 4, headerBottom);
    const float measured = NetMenu::MenuMeasureText(title, 24);
    const float available = (float)(right - left - 32);
    NativeText(left + 16, top + 9, title, false, measured > available ? 24 * available / measured : 24);
}
void NativeField(int id, int x, int y, int width, const char *text) {
    if (n_field == id && !n_grid)
        NativeSelectBar(x - 4, y - 1, x + width, y + 23, true, 255);
    const float measured = NetMenu::MenuMeasureText(text, 22);
    NativeText(x, y, text, n_field == id, measured > width ? fmaxf(16, 22 * width / measured) : 22);
}
} // namespace

extern "C" __declspec(dllexport) bool ModPaletteMouseWheel(int delta, int screenX, int screenY) {
    if (!s_nativeOpen || n_hex || n_save || n_exit)
        return false;
    POINT point{screenX, screenY};
    if (!NativeScreenPoint(point) || point.x < 56 || point.x >= 264 || point.y < 140 || point.y >= 350)
        return false;
    AcquireSRWLockExclusive(&n_wheelLock);
    n_wheelPoint = point;
    n_wheelDelta += delta;
    ReleaseSRWLockExclusive(&n_wheelLock);
    return true;
}
bool PaletteEditor_OpenNative() {
    if (GetGameMode() != MODE_MATCH || GetGameType() != GAMETYPE_TRAINING)
        return false;
    if (!s_initialized)
        PaletteEditor_Init();
    if (!NativeLoad())
        return false;
    s_nativeOpen = true;
    return true;
}
void PaletteEditor_CloseNative() {
    if (!s_nativeOpen)
        return;
    NetplayPaletteLocalContext context{};
    if (NativeContext(context))
        NativeDiscard();
    s_nativeOpen = false;
}
bool PaletteEditor_UpdateNative(uint16_t pressed) {
    if (!s_nativeOpen)
        return false;
    NetplayPaletteLocalContext context{};
    if (!NativeContext(context)) {
        s_nativeOpen = false;
        return false;
    }
    pressed |= NativePointer();
    if (n_hex) {
        NativeHexInput(pressed);
        return true;
    }
    if (n_save) {
        if (pressed & (INPUT_B | INPUT_START)) {
            if (n_overwrite)
                n_overwrite = false;
            else
                n_save = false;
            return true;
        }
        if (n_overwrite) {
            if (pressed & (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT))
                n_overwriteChoice = 1 - n_overwriteChoice;
            if ((pressed & INPUT_A) && n_overwriteChoice == 1) {
                n_overwrite = false;
                return true;
            }
        } else {
            int step = (pressed & (INPUT_RIGHT | INPUT_DOWN)) ? 1
                       : (pressed & (INPUT_LEFT | INPUT_UP)) ? -1 : 0;
            if (step)
                n_saveSlot = (n_saveSlot + step + kNativeSaveSlots) % kNativeSaveSlots;
        }
        if ((pressed & INPUT_A) && NativeConfirmSave() && n_saveLeave)
            return NativeLeave();
        return true;
    }
    if (n_exit) {
        if (pressed & INPUT_UP)
            n_exitChoice = (n_exitChoice + 2) % 3;
        if (pressed & INPUT_DOWN)
            n_exitChoice = (n_exitChoice + 1) % 3;
        if (pressed & (INPUT_B | INPUT_START))
            n_exit = false;
        if (pressed & INPUT_A) {
            if (n_exitChoice == 2)
                n_exit = false;
            else if (n_exitChoice == 1)
                return NativeLeave();
            else
                NativeBeginSave(true);
        }
        return true;
    }
    if ((pressed & INPUT_C) && UndoWorkingBank(context)) {
        n_original = false;
        NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false);
        NativeSyncPicker();
    }
    if ((pressed & INPUT_D) && RedoWorkingBank(context)) {
        n_original = false;
        NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false);
        NativeSyncPicker();
    }
    const bool back = (pressed & INPUT_B) != 0;
    if ((pressed & INPUT_START) ||
        (back && n_field != 5 && n_field != 6 && n_field != 4 && n_field != 7 && n_field != 13) ||
        ((pressed & INPUT_A) && n_field == 12)) {
        n_destination = -1;
        if (NativeDirty()) {
            n_exit = true;
            n_exitChoice = 0;
            return true;
        }
        return NativeLeave();
    }
    int horizontal = (pressed & INPUT_RIGHT) ? 1 : (pressed & INPUT_LEFT) ? -1 : 0;
    int vertical = (pressed & INPUT_DOWN) ? 1 : (pressed & INPUT_UP) ? -1 : 0;
    if (n_field == 3) {
        if (vertical < 0 && n_group < 8) {
            n_field = 2;
            n_grid = false;
            return true;
        }
        if (vertical > 0 && n_group + 8 >= NativeCount()) {
            n_field = 4;
            n_grid = false;
            return true;
        }
        if (horizontal || vertical) {
            n_group =
                ClampInt(n_group + horizontal + vertical * 8, 0, NativeCount() > 0 ? NativeCount() - 1 : 0);
            NativeSelection();
        }
        if (pressed & INPUT_A) {
            n_field = 5;
            n_grid = false;
        }
        return true;
    }
    if (n_field == 5) {
        if (horizontal || vertical)
            NativeSetHsv(n_hue, n_saturation + horizontal * .02f, n_value - vertical * .02f);
        if (pressed & INPUT_A)
            n_field = 6;
        if (back) {
            n_field = 3;
            n_grid = true;
        }
        return true;
    }
    if (n_field == 6) {
        if (horizontal)
            NativeSetHsv(n_hue + horizontal * .01f, n_saturation, n_value);
        if (vertical < 0 || back)
            n_field = 5;
        if (vertical > 0)
            n_field = 7;
        if (pressed & INPUT_A) {
            n_field = 3;
            n_grid = true;
        }
        return true;
    }
    if (n_field == 4) {
        if (horizontal) {
            auto g = NativeGroup();
            int count = g.last - g.first + 2;
            n_shade = ((n_shade + 1 + horizontal + count) % count) - 1;
            s_selectedIndex = (uint8_t)NativeIndex();
            NativeSyncPicker();
        }
        if (vertical > 0 || (pressed & INPUT_A))
            n_field = 5;
        if (vertical < 0 || back) {
            n_field = 3;
            n_grid = true;
        }
        return true;
    }
    if (n_field == 7) {
        if (pressed & INPUT_A)
            NativeBeginHex();
        if (vertical < 0 || back)
            n_field = 6;
        if (vertical > 0)
            n_field = 11;
        return true;
    }
    if (n_field == 13) {
        if (horizontal)
            NativeZoom((float)horizontal);
        if (vertical < 0 || back)
            n_field = 1;
        if (vertical > 0)
            n_field = 8;
        return true;
    }
    if (n_field >= 8) {
        if (horizontal)
            n_field = 8 + (n_field - 8 + horizontal + 5) % 5;
        if (vertical < 0)
            n_field = n_field <= 10 ? 13 : 7;
        if (vertical > 0) {
            n_field = 3;
            n_grid = true;
        }
    } else if (vertical) {
        if (n_field == 1 && vertical > 0) {
            n_field = 13;
            return true;
        }
        n_field = ClampInt(n_field + vertical, 0, 3);
        n_grid = n_field == 3;
        return true;
    }
    if (n_field == 0 && (horizontal || (pressed & INPUT_A))) {
        n_destination = 1 - context.game_slot;
        if (NativeDirty()) {
            n_exit = true;
            n_exitChoice = 0;
        } else
            return NativeLeave();
    } else if (n_field == 1 && (horizontal || (pressed & INPUT_A))) {
        int handle, width, height, count = 0;
        PaletteAssetHook_GetPreview(context.game_slot, n_frame, &handle, &width, &height, &count);
        int step = horizontal ? horizontal : 1;
        n_frame = count ? ((n_frame + 1 + step + count + 1) % (count + 1)) - 1 : -1;
    } else if (n_field == 2 && (horizontal || (pressed & INPUT_A))) {
        n_grouped = !n_grouped && n_groupCount > 0;
        n_group = 0;
        NativeSelection();
    } else if (pressed & INPUT_A) {
        if (n_field == 8 && UndoWorkingBank(context)) {
            n_original = false;
            NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false);
            NativeSyncPicker();
        } else if (n_field == 9 && RedoWorkingBank(context)) {
            n_original = false;
            NetplayPaletteRuntime_SetLocalCustomBank(&s_workingBank, false);
            NativeSyncPicker();
        } else if (n_field == 10) {
            NativeToggleOriginal();
        } else if (n_field == 11)
            NativeBeginSave(false);
    }
    return true;
}
void PaletteEditor_RenderNative() {
    if (!s_nativeOpen)
        return;
    NetplayPaletteLocalContext context{};
    if (!NativeContext(context))
        return;
    // Reload before queuing preview geometry so EndScene cannot free its texture mid-frame.
    PaletteAssetHook_FrameUpdate();
    NetMenu::MenuSetTextAlpha(255);
    NetMenu::MenuSetBlend(1, 96);
    NetMenu::MenuFillRect(0, 0, 639, 479, 0, 0, 0);
    NetMenu::MenuSetBlend(0, 255);
    char text[64];
    if (n_save) {
        if (n_overwrite) {
            snprintf(text, sizeof(text), "%s (%d)", S(Str::Pe_Overwrite), n_saveSlot + 1);
            NativePanel(160, 142, 480, 320, 184, text);
            const char *actions[] = {S(Str::Pe_Replace), S(Str::Common_Back)};
            for (int i = 0; i < 2; ++i) {
                if (i == n_overwriteChoice)
                    NativeSelectBar(172, 196 + i * 40, 468, 232 + i * 40, true, 255);
                NativeText(184, 200 + i * 40, actions[i], i == n_overwriteChoice, 24);
            }
            if (n_error)
                NativeText(176, 292, n_error, false, 18);
        } else {
            NativePanel(200, 80, 440, 420, 120, S(Str::Pe_Save));
            for (int i = 0; i < kNativeSaveSlots; ++i) {
                if (i == n_saveSlot)
                    NativeSelectBar(208, 132 + i * 32, 432, 162 + i * 32, true, 255);
                snprintf(text, sizeof(text), "%d", i + 1);
                NativeText(220, 134 + i * 32, text, i == n_saveSlot, 24);
            }
            if (n_error)
                NativeText(212, 392, n_error, false, 18);
        }
        return;
    }
    if (n_hex) {
        NativePanel(144, 156, 496, 332, 198, "HEX");
        for (int i = 0; i < 6; ++i) {
            char digit[] = {n_hexText[i], 0};
            NativeText(180 + i * 44, 225, digit, i == n_hexDigit, 38);
        }
        NativeUnderline(180 + n_hexDigit * 44, 208 + n_hexDigit * 44, 267);
        NativeText(160, 284, S(Str::Pe_Apply));
        NativeText(332, 284, S(Str::Common_Back));
        return;
    }
    if (n_exit) {
        NativePanel(160, 142, 480, 346, 184, S(Str::Pe_Unsaved));
        const char *actions[] = {S(Str::Pe_Save), S(Str::Pe_Discard), S(Str::Pe_Continue)};
        for (int i = 0; i < 3; ++i) {
            if (n_exitChoice == i)
                NativeSelectBar(172, 196 + i * 38, 468, 230 + i * 38, true, 255);
            NativeText(184, 200 + i * 38, actions[i], n_exitChoice == i, 24);
        }
        if (n_error)
            NativeText(176, 320, n_error, false, 18);
        return;
    }
    NativePanel(40, 58, 600, 446, 98, S(Str::Pe_Title));
    snprintf(text, sizeof(text), "P%d", context.game_slot + 1);
    NativeField(0, 64, 108, 70, text);
    if (n_frame < 0)
        snprintf(text, sizeof(text), "%s", S(Str::Pe_CurrentFrame));
    else
        snprintf(text, sizeof(text), "%s %d", S(Str::Pe_Frame), n_frame + 1);
    NativeField(1, 144, 108, 116, text);
    NetMenu::MenuSetBlend(0, 255);
    NetMenu::MenuFillRect(56, 140, 264, 376, 30, 25, 20);
    int handle, width, height, count;
    if (PaletteAssetHook_GetPreview(context.game_slot, n_frame, &handle, &width, &height, &count)) {
        float scale = fminf(2.f, fminf(196.f / width, 202.f / height)) * n_zoom;
        int w = (int)(width * scale), h = (int)(height * scale), x = 160 - w / 2 + (int)n_panX,
            y = 247 - h / 2 + (int)n_panY;
        using Quad = int(__cdecl *)(long, int, long, int, long, int, long, int, int, int);
        using DrawArea = int(__cdecl *)(int, int, int, int);
        const RECT savedArea = *reinterpret_cast<const RECT *>(ADDR_RENDER_DRAW_AREA);
        auto setArea = reinterpret_cast<DrawArea>(ADDR_RENDER_SET_DRAW_AREA);
        setArea(56, 140, 264, 350);
        NetMenu::MenuSetBlend(1, 255);
        ((Quad)ADDR_RENDER_DRAW_QUAD)(x, y, x + w - 1, y, x + w - 1, y + h - 1, x, y + h - 1, handle, 1);
        setArea(savedArea.left, savedArea.top, savedArea.right, savedArea.bottom);
        NetMenu::MenuSetBlend(0, 255);
    } else
        NativeText(68, 235, S(Str::Pe_Unavailable));
    NativeText(64, 354, "-");
    snprintf(text, sizeof(text), "%.0f%%", n_zoom * 100);
    NativeText(160 - (int)NetMenu::MenuMeasureText(text, 22) / 2, 354, text, n_field == 13);
    NativeText(232, 354, "+");
    NativeField(2, 284, 108, 288, S(n_grouped ? Str::Pe_Grouped : Str::Pe_AllColors));
    const int first = (n_group / kSwatchesPerPage) * kSwatchesPerPage,
              end = ClampInt(first + kSwatchesPerPage, 0, NativeCount());
    for (int i = first; i < end; ++i) {
        int index = n_grouped ? n_groups[i].key : i + 1;
        const uint8_t *c = s_workingBank.data + index * 4;
        int x = 284 + (i - first) % 8 * 36, y = 143 + (i - first) / 8 * 24;
        NetMenu::MenuFillRect(x, y, x + 29, y + 18, c[2], c[1], c[0]);
        if (i == n_group) {
            NativeUnderline(x, x + 29, y + 20);
            if (n_field == 3) {
                NetMenu::MenuFillRect(x, y, x + 29, y, 255, 230, 170);
                NetMenu::MenuFillRect(x, y, x, y + 18, 255, 230, 170);
            }
        }
    }
    auto group = NativeGroup();
    for (int i = group.first; i <= group.last; ++i) {
        int x = 284 + (i - group.first) * 288 / (group.last - group.first + 1);
        int next = 284 + (i - group.first + 1) * 288 / (group.last - group.first + 1);
        const uint8_t *c = s_workingBank.data + i * 4;
        NetMenu::MenuFillRect(x, 221, next - 2, 235, c[2], c[1], c[0]);
        if (n_shade >= 0 && i == NativeIndex())
            NativeUnderline(x, next - 2, 238);
    }
    if (n_field == 4 && n_shade < 0)
        NativeUnderline(284, 572, 238);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 32; ++x) {
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(n_hue, x / 31.f, 1 - y / 23.f, r, g, b);
            int left = kPickerLeft + x * (kPickerRight - kPickerLeft) / 32;
            int right = kPickerLeft + (x + 1) * (kPickerRight - kPickerLeft) / 32;
            int top = kPickerTop + y * (kPickerBottom - kPickerTop) / 24;
            int bottom = kPickerTop + (y + 1) * (kPickerBottom - kPickerTop) / 24;
            NetMenu::MenuFillRect(left, top, right, bottom, FloatToByte(r), FloatToByte(g), FloatToByte(b));
        }
    for (int y = 0; y < 48; ++y) {
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(y / 48.f, 1, 1, r, g, b);
        int top = kPickerTop + y * (kPickerBottom - kPickerTop) / 48;
        int bottom = kPickerTop + (y + 1) * (kPickerBottom - kPickerTop) / 48;
        NetMenu::MenuFillRect(kHueLeft, top, kHueRight, bottom, FloatToByte(r), FloatToByte(g),
                              FloatToByte(b));
    }
    int px = kPickerLeft + (int)(n_saturation * (kPickerRight - kPickerLeft));
    int py = kPickerTop + (int)((1 - n_value) * (kPickerBottom - kPickerTop));
    NetMenu::MenuFillRect(px - 3, py - 1, px + 3, py + 1, 0, 0, 0);
    NetMenu::MenuFillRect(px - 1, py - 3, px + 1, py + 3, 255, 255, 255);
    int hy = kPickerTop + (int)(n_hue * (kPickerBottom - kPickerTop));
    NetMenu::MenuFillRect(kHueLeft - 2, hy, kHueRight + 2, hy + 1, 255, 255, 255);
    if (n_field == 5)
        NativeUnderline(kPickerLeft, kPickerRight, kPickerBottom + 2);
    if (n_field == 6)
        NativeUnderline(kHueLeft, kHueRight, kPickerBottom + 2);
    const uint8_t *c = s_workingBank.data + NativeIndex() * 4;
    snprintf(text, sizeof(text), "#%02X%02X%02X", c[2], c[1], c[0]);
    NativeField(7, 284, 364, 128, text);
    NativeField(8, 64, 386, 72, S(Str::Pe_Undo));
    NativeField(9, 144, 386, 72, S(Str::Pe_Redo));
    NativeField(10, 224, 386, 112, S(Str::Pe_Restore));
    if (n_original)
        NativeUnderline(224, 336, 410);
    NativeField(11, 352, 386, 96, S(Str::Pe_Save));
    NativeField(12, 480, 386, 90, S(Str::Common_Back));
    NativeRule(44, 596, 416);
    if (n_error)
        NativeText(56, 422, n_error, false, 18);
    else {
        int x = 56;
        x += NativeKeycap(x, 422, "A", S(Str::Pe_Select), 255);
        x += NativeKeycap(x, 422, "B", S(Str::Common_Back), 255);
        x += NativeKeycap(x, 422, "C", S(Str::Pe_Undo), 255);
        NativeKeycap(x, 422, "D", S(Str::Pe_Redo), 255);
    }
}
