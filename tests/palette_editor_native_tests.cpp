#include "core/as2_constants.h"
#include "core/game_state.h"
#include <windows.h>
static_assert(ADDR_RENDER_DRAW_AREA == 0xA02F2C, "Preview must restore the active draw rectangle");
static RECT testArea{0, 0, 640, 480};
static bool testDrawClipped = false, testEnlarged = false, testShowPreview = false;
static int __cdecl TestDrawArea(int l, int t, int r, int b) {
    testArea = {l, t, r, b};
    return 0;
}
static int __cdecl TestQuad(long l, int t, long r, int, long, int b, long, int, int, int) {
    testDrawClipped =
        testArea.left == 56 && testArea.top == 140 && testArea.right == 264 && testArea.bottom == 350;
    testEnlarged = r - l > 208 || b - t > 210;
    return 0;
}
#undef ADDR_RENDER_SET_DRAW_AREA
#undef ADDR_RENDER_DRAW_AREA
#undef ADDR_RENDER_DRAW_QUAD
#define ADDR_RENDER_SET_DRAW_AREA ((uintptr_t)&TestDrawArea)
#define ADDR_RENDER_DRAW_AREA ((uintptr_t)&testArea)
#define ADDR_RENDER_DRAW_QUAD ((uintptr_t)&TestQuad)
static uint32_t TestGameMode() { return MODE_MATCH; }
static uint32_t TestGameType() { return GAMETYPE_TRAINING; }
#define GetGameMode TestGameMode
#define GetGameType TestGameType
#include "../src/ui/palette_editor.cpp"
#undef GetGameMode
#undef GetGameType
#include <stdlib.h>
static Net::NetplayPaletteBank testBank{}, testVanilla{};
static bool testCustom = false, testFailSave = false;
static int testSlot = 0, testWrites = 0;
static int testPreviewIndex = -999;
static bool testDim = false, testSaveBody = false;
static int testSaveNumbers = 0;
static int testZoomHighlights = 0;
static uint8_t testTextAlpha = 255;
static FILE *testRenderTrace = nullptr;
static bool testReloadQueued = false, testResolvedAfterReload = false;
static int testSavedSlot = -1;
namespace Net {
void NetplayPaletteRuntime_GetLocalContext(NetplayPaletteLocalContext *out) {
    *out = {};
    out->available = true;
    out->has_live_bank = true;
    out->has_vanilla_bank = true;
    out->has_applied_custom_bank = testCustom;
    out->game_slot = (uint8_t)testSlot;
    out->character_id = 0;
    out->base_palette = 0;
}
bool NetplayPaletteRuntime_CopyLocalBankForSource(NetplayPaletteBankSource source, NetplayPaletteBank *out) {
    *out = source == NetplayPaletteBankSource::VanillaSource ? testVanilla : testBank;
    return true;
}
bool NetplayPaletteRuntime_SetLocalCustomBank(const NetplayPaletteBank *b, bool save) {
    if (save && testFailSave)
        return false;
    testBank = *b;
    testReloadQueued = true;
    testCustom = true;
    ++testWrites;
    return true;
}
bool NetplayPaletteRuntime_ClearLocalCustomBank(bool) {
    testBank = testVanilla;
    testCustom = false;
    return true;
}
bool NetplayPaletteRuntime_SetOfflineEditorGameSlot(uint8_t slot) {
    testSlot = slot;
    return true;
}
bool NetplayPaletteStorage_HasBank(uint8_t, uint8_t slot) { return slot == testSavedSlot; }
void NetplayPaletteStorage_ClearCache() {}
bool NetplayPaletteStorage_SaveBank(const NetplayPaletteBank *bank) {
    if (testFailSave)
        return false;
    testSavedSlot = bank->base_palette;
    return true;
}
int NetplayPaletteStorage_ListPresets(uint8_t, uint8_t, NetplayPalettePresetInfo *, int) { return 0; }
} // namespace Net
namespace Ui {
const char *S(Str id) {
#define TEST_STRING(key, en, ja) case Str::key: return en;
    switch (id) { AS2_UI_STRINGS(TEST_STRING) default: return ""; }
#undef TEST_STRING
}
} // namespace Ui
void PaletteAssetHook_FrameUpdate() { testReloadQueued = false; }
bool PaletteAssetHook_GetPreview(int, int index, int *handle, int *width, int *height, int *count) {
    testPreviewIndex = index;
    testResolvedAfterReload = !testReloadQueued;
    if (testShowPreview) {
        *handle = 1;
        *width = 100;
        *height = 100;
        *count = 5;
    }
    return testShowPreview;
}
static void Check(bool ok) {
    if (!ok) {
        puts("native palette state check failed");
        exit(1);
    }
}
void AdjustChannel(int delta) {
    const uint8_t *e = s_workingBank.data + NativeIndex() * 4;
    uint8_t rgb[] = {e[2], e[1], e[0]};
    rgb[n_field - 5] = (uint8_t)ClampInt(rgb[n_field - 5] + delta, 0, 255);
    NativeSetRgb(rgb[0], rgb[1], rgb[2]);
}
int main() {
    testBank.valid = true;
    for (int i = 1; i < 5; ++i) {
        testBank.data[i * 4 + 2] = (uint8_t)(i * 40);
        testBank.data[i * 4 + 3] = 0;
    }
    testVanilla = testBank;
    Check(PaletteEditor_OpenNative());
    Check(n_frame == -1 && NativeCount() == 1 && n_visibleCount == 4);
    PaletteEditor_RenderNative();
    Check(testPreviewIndex == -1 && testDim);
    Check(n_field == 3);
    PaletteEditor_UpdateNative(INPUT_A);
    Check(n_field == 5);
    PaletteEditor_UpdateNative(INPUT_B);
    Check(n_field == 3);
    Check(PaletteEditor_UpdateNative(INPUT_C));
    Check(!testCustom && testWrites == 0);
    Check(PaletteEditor_UpdateNative(INPUT_D));
    Check(!testCustom && testWrites == 0);
    n_field = 5;
    Check(PaletteEditor_UpdateNative(INPUT_LEFT));
    Check(testCustom && NativeDirty());
    Check(PaletteEditor_UpdateNative(INPUT_C));
    Check(!NativeDirty());
    Check(!PaletteEditor_UpdateNative(INPUT_START));
    Check(!testCustom);
    Check(PaletteEditor_OpenNative());
    n_field = 6;
    PaletteEditor_UpdateNative(INPUT_RIGHT);
    PaletteEditor_UpdateNative(INPUT_START);
    Check(n_exit && s_nativeOpen);
    testFailSave = true;
    PaletteEditor_UpdateNative(INPUT_A);
    Check(n_exit && n_save && s_nativeOpen && NativeDirty());
    PaletteEditor_UpdateNative(INPUT_A);
    Check(n_save && NativeDirty());
    PaletteEditor_UpdateNative(INPUT_B);
    testFailSave = false;
    PaletteEditor_UpdateNative(INPUT_DOWN);
    Check(!PaletteEditor_UpdateNative(INPUT_A));
    Check(!testCustom);
    Check(PaletteEditor_OpenNative());
    n_field = 6;
    PaletteEditor_UpdateNative(INPUT_RIGHT);
    n_field = 0;
    PaletteEditor_UpdateNative(INPUT_RIGHT);
    Check(n_exit && testSlot == 0);
    PaletteEditor_UpdateNative(INPUT_DOWN);
    PaletteEditor_UpdateNative(INPUT_A);
    Check(testSlot == 1 && !NativeDirty());
    n_field = 6;
    PaletteEditor_UpdateNative(INPUT_RIGHT);
    n_field = 11;
    PaletteEditor_UpdateNative(INPUT_A);
    Check(n_save && NativeDirty());
    PaletteEditor_UpdateNative(INPUT_A);
    Check(!NativeDirty() && testCustom);
    Check(!PaletteEditor_UpdateNative(INPUT_START));
    Check(testCustom);
    Check(PaletteEditor_OpenNative());
    n_field = 10;
    PaletteEditor_UpdateNative(INPUT_A);
    n_shade = 0;
    n_field = 5;
    AdjustChannel(-1);
    uint8_t shade = s_workingBank.data[6];
    n_shade = -1;
    n_field = 6;
    AdjustChannel(1);
    Check(s_workingBank.data[6] == shade);
    PaletteEditor_CloseNative();
    Check(testCustom);
    Check(PaletteEditor_OpenNative());
    n_field = 10;
    PaletteEditor_UpdateNative(INPUT_A);
    NetplayPaletteBank initial = s_workingBank;
    n_shade = -1;
    n_field = 5;
    AdjustChannel(-80);
    AdjustChannel(80);
    Check(memcmp(initial.data, s_workingBank.data, 1024) == 0);
    AdjustChannel(175);
    AdjustChannel(-175);
    Check(memcmp(initial.data, s_workingBank.data, 1024) == 0);
    AdjustChannel(-80);
    PaletteEditor_UpdateNative(INPUT_C);
    PaletteEditor_UpdateNative(INPUT_D);
    AdjustChannel(80);
    Check(memcmp(initial.data, s_workingBank.data, 1024) == 0);
    PaletteEditor_CloseNative();
    Check(PaletteEditor_OpenNative());
    n_field = 10;
    PaletteEditor_UpdateNative(INPUT_A);
    NativeSyncPicker();
    NativeSetHsv(0, 0, 0);
    n_dragSnapshot = false;
    n_dragControl = 0;
    const int pointerUndo = s_undoCount;
    Check(NativePickerPointer(kPickerLeft, kPickerBottom, true));
    Check(n_dragControl == 5 && !n_dragSnapshot && s_undoCount == pointerUndo);
    Check(NativePickerPointer(kPickerRight, kPickerTop, false));
    Check(n_dragSnapshot && s_undoCount == pointerUndo + 1);
    n_dragControl = 0;
    n_dragSnapshot = false;
    int beforeDrag = s_undoCount;
    NativeDragHsv(n_hue, n_saturation, n_value);
    Check(!n_dragSnapshot && s_undoCount == beforeDrag);
    NativeDragHsv(n_hue, n_saturation * .5f, n_value);
    Check(n_dragSnapshot && s_undoCount == beforeDrag + 1);
    NativeDragHsv(n_hue, n_saturation * .5f, n_value);
    Check(s_undoCount == beforeDrag + 1);
    NativeSetRgb(0, 0, 0);
    Check(NativeSave());
    PaletteEditor_CloseNative();
    Check(PaletteEditor_OpenNative());
    Check(n_visibleCount == 4 && n_groupCount == 1);
    PaletteEditor_CloseNative();
    Check(PaletteEditor_OpenNative());
    Check(NativeApplyHex("12AbEf"));
    const auto hexBank = s_workingBank;
    Check(!NativeApplyHex("12345Z"));
    Check(memcmp(hexBank.data, s_workingBank.data, 1024) == 0);
    const int historyBefore = s_undoCount;
    NativeToggleOriginal();
    Check(n_original);
    Check(memcmp(hexBank.data, s_workingBank.data, 1024) == 0 && s_undoCount == historyBefore);
    NativeToggleOriginal();
    Check(!n_original);
    NativeToggleOriginal();
    PaletteEditor_UpdateNative(INPUT_C);
    Check(!n_original);
    PaletteEditor_UpdateNative(INPUT_D);
    NativeBeginSave(false);
    Check(n_save && n_saveSlot == 0);
    PaletteEditor_RenderNative();
    Check(testSaveBody && testSaveNumbers == 8);
    PaletteEditor_UpdateNative(INPUT_UP);
    Check(n_saveSlot == 7);
    PaletteEditor_UpdateNative(INPUT_DOWN);
    Check(n_saveSlot == 0);
    n_saveSlot = 8;
    Check(!NativeSave());
    n_saveSlot = -1;
    Check(!NativeSave());
    n_saveSlot = 3;
    Check(NativeConfirmSave());
    Check(!n_save && testSavedSlot == 3);
    NativeBeginSave(false);
    n_saveSlot = 3;
    Check(!NativeConfirmSave() && n_overwrite);
    PaletteEditor_UpdateNative(INPUT_DOWN);
    PaletteEditor_UpdateNative(INPUT_A);
    Check(n_save && !n_overwrite);
    Check(!NativeConfirmSave() && n_overwrite);
    Check(NativeConfirmSave() && !n_save);
    n_frame = 4;
    NativeToggleCurrentFrame();
    Check(n_frame == -1);
    NativeToggleCurrentFrame();
    Check(n_frame == 4);
    n_zoom = 1;
    NativeZoom(1);
    Check(n_zoom > 1);
    NativeZoom(100);
    Check(n_zoom == 4.f);
    testShowPreview = true;
    PaletteEditor_RenderNative();
    testShowPreview = false;
    Check(testDrawClipped && testEnlarged && testArea.left == 0 && testArea.top == 0 &&
          testArea.right == 640 && testArea.bottom == 480);
    NativeZoom(-100);
    Check(n_zoom == .25f);
    PaletteEditor_CloseNative();
    Check(PaletteEditor_OpenNative());
    const int focusBeforePan = n_field;
    const bool gridBeforePan = n_grid;
    Check(NativePreviewPointer(140, 200, true, true, 0));
    Check(n_field == focusBeforePan && n_grid == gridBeforePan);
    testZoomHighlights = 0;
    PaletteEditor_RenderNative();
    Check(testZoomHighlights == 0);
    Check(NativePreviewPointer(170, 225, true, false, 0));
    Check(n_panX == 30 && n_panY == 25);
    NativePreviewPointer(180, 230, false, false, 0);
    Check(!n_panDragging && n_panX == 30 && n_panY == 25);
    n_zoom = 1;
    n_panX = 0;
    n_panY = 0;
    Check(NativePreviewPointer(200, 200, false, false, 1));
    Check(n_zoom == 1.25f);
    Check(n_panX == -10 && n_panY == 11.75f);
    NativePreviewPointer(200, 200, false, false, -1);
    Check(n_zoom == 1);
    Check(NativePreviewPointer(244, 364, true, true, 0));
    Check(n_zoom == 1.25f);
    testZoomHighlights = 0;
    PaletteEditor_RenderNative();
    Check(testZoomHighlights == 0);
    Check(NativePreviewPointer(76, 364, true, true, 0));
    Check(n_zoom == 1);
    NativePreviewPointer(160, 364, true, true, 0);
    Check(n_zoom == 1);
    NativePreviewPointer(400, 300, false, false, 1);
    Check(n_zoom == 1);
    PaletteEditor_CloseNative();
    Check(PaletteEditor_OpenNative());
    testShowPreview = true;
    for (int edit = 0; edit < 8; ++edit) {
        NativeSetRgb((uint8_t)(50 + edit), 30, 40);
        Check(testReloadQueued);
        PaletteEditor_RenderNative();
        Check(testResolvedAfterReload && !testReloadQueued);
    }
    testShowPreview = false;
    n_zoom = 1;
    n_panX = n_panY = 0;
    n_wheelPoint = {200, 200};
    n_wheelDelta = WHEEL_DELTA;
    NativeConsumeWheel(true);
    Check(n_zoom == 1.25f && n_panX == -10 && n_panY == 11.75f);
    NativeConsumeWheel(true);
    Check(n_zoom == 1.25f);
    n_wheelDelta = WHEEL_DELTA;
    NativeConsumeWheel(false);
    Check(n_zoom == 1.25f && n_wheelDelta == 0);
    const char *tracePath = getenv("AS2_PALETTE_RENDER_TRACE");
    if (tracePath)
        testRenderTrace = fopen(tracePath, "w");
    for (int dialog = 0; dialog < 4; ++dialog) {
        n_exit = dialog == 0;
        n_save = dialog == 1 || dialog == 2;
        n_overwrite = dialog == 2;
        n_hex = dialog == 3;
        n_saveSlot = 7;
        n_exitChoice = n_overwriteChoice = 0;
        memcpy(n_hexText, "12ABEF", 7);
        if (testRenderTrace)
            fprintf(testRenderTrace, "dialog\t%d\n", dialog);
        PaletteEditor_RenderNative();
    }
    if (testRenderTrace) {
        fclose(testRenderTrace);
        testRenderTrace = nullptr;
    }
    PaletteEditor_CloseNative();
    puts("Native editor empty history, undo, discard, save failure and side-switch checks passed");
}

namespace NetMenu {
void MenuSetBlend(int mode, uint8_t alpha) {
    if (mode)
        testTextAlpha = alpha;
}
void MenuFillRect(int l, int t, int r, int b, uint8_t red, uint8_t green, uint8_t blue) {
    Check(l >= 0 && t >= 0 && r <= 640 && b <= 480 && l <= r && t <= b);
    if (testRenderTrace)
        fprintf(testRenderTrace, "rect\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", l, t, r, b, red, green, blue);
    if (t == 353 && b == 377 && (l == 60 || l == 228) && red > green)
        ++testZoomHighlights;
    if (l == 0 && t == 0 && r == 639 && b == 479 && !(red | green | blue))
        testDim = true;
    if (l == 204 && t == 122 && r == 436 && b == 416 && red == 64 && green == 54 && blue == 42)
        testSaveBody = true;
}
void MenuSetTextAlpha(uint8_t alpha) { testTextAlpha = alpha; }
void MenuDrawTextSized(int x, int y, uint8_t r, uint8_t g, uint8_t b, float size, const char *text) {
    Check(testTextAlpha == 255);
    if (testRenderTrace)
        fprintf(testRenderTrace, "text\t%d\t%d\t%d\t%d\t%d\t%.0f\t%s\n", x, y, r, g, b, size, text);
    if (x == 220 && y >= 134 && y <= 358 && text[0] >= '1' && text[0] <= '8' && !text[1])
        ++testSaveNumbers;
}
void MenuDrawPanel(int, int, int, int, uint8_t) {}
void MenuDrawSelectBar(int l, int t, int r, int b, bool, uint8_t) {
    MenuFillRect(l, t, r, b, 150, 50, 32);
    MenuFillRect(l, t, l + 3, b, 255, 200, 120);
}
void MenuDrawRule(int, int, int, uint8_t) {}
int MenuDrawKeycap(int, int, const char *, const char *, uint8_t) { return 0; }
} // namespace NetMenu
float ModUI_GetScale() { return 1; }
float ModUI_Scale(float v) { return v; }
bool PracticeTools_IsPracticeModeActive() { return true; }
bool PracticeTools_IsPaused() { return true; }
void PracticeTools_SetPaused(bool) {}
namespace Net {
void NetplayPaletteRuntime_GetSnapshot(NetplayPaletteRuntimeSnapshot *out) { *out = {}; }
bool NetplayPaletteStorage_HasPreset(uint8_t, uint8_t, const char *) { return false; }
bool NetplayPaletteStorage_LoadPreset(uint8_t, uint8_t, const char *, NetplayPaletteBank *) { return false; }
bool NetplayPaletteStorage_SavePreset(const NetplayPaletteBank *, const char *) { return false; }
bool NetplayPaletteStorage_DeletePreset(uint8_t, uint8_t, const char *) { return false; }
} // namespace Net

namespace NetMenu {
float MenuMeasureText(const char *text, float size) { return (float)strlen(text) * size * .5f; }
} // namespace NetMenu

uint32_t CalcCRC32(const void *, size_t) { return 0; }
