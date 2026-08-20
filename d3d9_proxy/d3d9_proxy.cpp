/**
 * Alice Senki 2 - D3D9 Proxy DLL
 * 
 * Approach: Wrap IDirect3D9 and IDirect3DDevice9 interfaces.
 * - When game calls Direct3DCreate9, return our wrapped IDirect3D9
 * - When game calls CreateDevice, hook the vtable and return real device
 * - Hook EndScene to render ImGui overlay
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "overlay_resources.h"
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string>
#include <unordered_set>
#include <vector>
#include <DbgHelp.h>  // For stack walking
#include <wchar.h>

#pragma comment(lib, "dbghelp.lib")

// MinHook
#include "MinHook.h"

// ImGui
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

// Letterbox Scaler
#include "letterbox_scaler.h"

// Forward declare ImGui WndProc handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// Type Definitions
// ============================================================================

typedef IDirect3D9* (WINAPI *RealDirect3DCreate9_t)(UINT SDKVersion);
typedef HRESULT (WINAPI *BeginScene_t)(IDirect3DDevice9*);
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (WINAPI *SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (WINAPI *SetViewport_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef BOOL (WINAPI *SetWindowPos_t)(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
typedef LONG (WINAPI *SetWindowLongA_t)(HWND hWnd, int nIndex, LONG dwNewLong);
typedef LONG (WINAPI *SetWindowLongW_t)(HWND hWnd, int nIndex, LONG dwNewLong);
typedef int (WINAPI *SetWindowRgn_t)(HWND hWnd, HRGN hRgn, BOOL bRedraw);
typedef BOOL (WINAPI *SetMenu_t)(HWND hWnd, HMENU hMenu);

// Swap chain Present has a different signature than Device Present
typedef HRESULT (WINAPI *SwapChainPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

// Mod DLL function types
typedef void (*ModInit_t)(HMODULE gameModule);
typedef void (*ModShutdown_t)();
typedef void (*ModOnFrame_t)();
typedef void (*ModOnPresent_t)(void* pDevice);
typedef void (*ModSetImGuiContext_t)(void* ctx);
typedef void (*ModOnGameExit_t)(int exitCode, const char* reason);
typedef void (*ModToggleMenu_t)();
typedef bool (*ModIsMenuRequestedOpen_t)();
typedef bool (*ModShouldRenderImGui_t)();
typedef bool (*ModGetNetplayHudText_t)(char* out, int cap);
typedef bool (*ModWantsExclusiveOverlay_t)();
typedef void (*ModSetLogDir_t)(const char* dir);
typedef bool (*ModCallGameWndProc_t)(HWND hwnd,
                                     UINT msg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     LRESULT* outResult);

// Match HUD structured data (must match include/core/mod_main.h MatchHudData)
struct MatchHudData {
    bool     active;
    char     p1_name[64];
    char     p2_name[64];
    int      p1_wins;
    int      p2_wins;
    uint8_t  p1_trail_r;
    uint8_t  p1_trail_g;
    uint8_t  p1_trail_b;
    uint8_t  p1_text_r;
    uint8_t  p1_text_g;
    uint8_t  p1_text_b;
    uint8_t  p2_trail_r;
    uint8_t  p2_trail_g;
    uint8_t  p2_trail_b;
    uint8_t  p2_text_r;
    uint8_t  p2_text_g;
    uint8_t  p2_text_b;
    uint8_t  p1_score_r;
    uint8_t  p1_score_g;
    uint8_t  p1_score_b;
    uint8_t  p2_score_r;
    uint8_t  p2_score_g;
    uint8_t  p2_score_b;
    uint16_t p1_trail_length_px;
    uint16_t p2_trail_length_px;
    uint8_t  p1_vertical_position;
    uint8_t  p2_vertical_position;
    uint8_t  p1_font_size;
    uint8_t  p2_font_size;
    float    ping_ms;
    int      delay_frames;
    int      rollback_frames;
    int      local_frame;
    int      remote_frame;
    bool     is_host;
    bool     spectator_mode;
    bool     show_connection_stats;
    char     status_text[64];
};
typedef bool (*ModGetMatchHudData_t)(MatchHudData* out);

struct LoadedUserModDLL {
    std::string name;
    std::string dllPath;
    HMODULE module = nullptr;
    ModShutdown_t shutdown = nullptr;
    bool initCalled = false;
};

// Crash diagnostic function types (loaded from mod DLL on demand)
typedef void (*ModGetRngStats_t)(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
typedef uint32_t (*ModGetFpuCount_t)();
typedef void (*ModGetCurrentFpuState_t)(uint16_t*, uint16_t*, uint32_t*);

// ============================================================================
// Global State
// ============================================================================

static HMODULE g_hRealD3D9 = nullptr;
static HMODULE g_hModDLL = nullptr;
static RealDirect3DCreate9_t g_pRealDirect3DCreate9 = nullptr;

static ModInit_t g_pModInit = nullptr;
static ModShutdown_t g_pModShutdown = nullptr;
static ModOnFrame_t g_pModOnFrame = nullptr;
static ModOnPresent_t g_pModOnPresent = nullptr;
static ModSetImGuiContext_t g_pModSetImGuiContext = nullptr;
static ModOnGameExit_t g_pModOnGameExit = nullptr;
static ModToggleMenu_t g_pModToggleMenu = nullptr;
static ModIsMenuRequestedOpen_t g_pModIsMenuRequestedOpen = nullptr;
static ModShouldRenderImGui_t g_pModShouldRenderImGui = nullptr;
static ModGetNetplayHudText_t g_pModGetNetplayHudText = nullptr;
static ModGetMatchHudData_t g_pModGetMatchHudData = nullptr;
static ModWantsExclusiveOverlay_t g_pModWantsExclusiveOverlay = nullptr;
static ModCallGameWndProc_t g_pModCallGameWndProc = nullptr;
static bool g_loggedMagicStubNoModExport = false;
static bool g_imguiContextShared = false;
static bool g_gameExiting = false;  // Track if game is exiting
static bool g_quitMessagePosted = false;
static bool g_fastExitRequested = false;
static bool g_proxyShutdownComplete = false;

// ============================================================================
// Always-on HUD (no ImGui dependency)
// ============================================================================

static bool g_showHud = true;

// Simple 5x7 bitmap font for required ASCII subset (uppercase + digits + punctuation).
// Each row uses the low 5 bits.
struct HudGlyph {
    char c;
    uint8_t rowBits[7];
};

static const HudGlyph kHudGlyphs[] = {
    // Digits
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3',{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    // Full uppercase alphabet
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x11,0x11,0x11,0x11}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x11,0x15,0x1B,0x11}},
    {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    // Punctuation
    {':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
    {'/',{0x01,0x02,0x04,0x08,0x10,0x00,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x04,0x04}},
    {'_',{0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
    {'!',{0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'?',{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {',',{0x00,0x00,0x00,0x00,0x00,0x04,0x08}},
    {'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'=',{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    {'#',{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}},
    {'@',{0x0E,0x11,0x17,0x15,0x17,0x10,0x0F}},
    {'[',{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
    {']',{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
};

static IDirect3DTexture9* g_hudFontTex = nullptr;
static IDirect3DStateBlock9* g_hudStateBlock = nullptr;

struct HUD_VERTEX {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};

#define HUD_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void Hud_Release() {
    if (g_hudStateBlock) {
        g_hudStateBlock->Release();
        g_hudStateBlock = nullptr;
    }
    if (g_hudFontTex) {
        g_hudFontTex->Release();
        g_hudFontTex = nullptr;
    }
}

static const HudGlyph* Hud_FindGlyph(char c) {
    for (const auto& g : kHudGlyphs) {
        if (g.c == c) return &g;
    }
    return nullptr;
}

static bool Hud_EnsureResources(IDirect3DDevice9* pDevice) {
    if (!pDevice) return false;

    if (!g_hudStateBlock) {
        if (FAILED(pDevice->CreateStateBlock(D3DSBT_ALL, &g_hudStateBlock))) {
            g_hudStateBlock = nullptr;
            return false;
        }
    }

    if (g_hudFontTex) return true;

    // 16x8 glyph grid, 8x8 pixels per cell => 128x64 atlas.
    const UINT texW = 128;
    const UINT texH = 64;

    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = pDevice->CreateTexture(
        texW, texH, 1,
        D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &tex,
        nullptr);
    if (FAILED(hr) || !tex) {
        return false;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
        tex->Release();
        return false;
    }

    // Clear (transparent)
    for (UINT y = 0; y < texH; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
        for (UINT x = 0; x < texW; x++) row[x] = 0x00000000;
    }

    auto setPixel = [&](UINT x, UINT y) {
        if (x >= texW || y >= texH) return;
        uint32_t* row = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
        row[x] = 0xFFFFFFFF;
    };

    // Bake supported glyphs at their ASCII cell positions.
    for (const auto& glyph : kHudGlyphs) {
        const unsigned char uc = (unsigned char)glyph.c;
        const UINT cellX = (uc % 16) * 8;
        const UINT cellY = (uc / 16) * 8;

        // 5x7 into 8x8 (leave 1px margin on left/top).
        for (UINT ry = 0; ry < 7; ry++) {
            uint8_t bits = glyph.rowBits[ry];
            for (UINT rx = 0; rx < 5; rx++) {
                if (bits & (1u << (4 - rx))) {
                    setPixel(cellX + 1 + rx, cellY + 1 + ry);
                }
            }
        }
    }

    tex->UnlockRect(0);
    g_hudFontTex = tex;
    return true;
}

static void Hud_DrawText(IDirect3DDevice9* pDevice, float x, float y, float scale, D3DCOLOR color, const char* text) {
    if (!pDevice || !text || !*text) return;
    if (!Hud_EnsureResources(pDevice)) return;

    pDevice->SetFVF(HUD_FVF);
    pDevice->SetTexture(0, g_hudFontTex);

    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    // Build a single batched draw for the whole string.
    HUD_VERTEX verts[6 * 256];
    int quadCount = 0;
    float penX = x;

    const float cellW = 8.0f;
    const float cellH = 8.0f;
    const float texW = 128.0f;
    const float texH = 64.0f;

    for (const char* p = text; *p && quadCount < 256; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32) continue;

        // Map lowercase to uppercase so nicknames render correctly
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';

        const float w = cellW * scale;
        const float h = cellH * scale;

        const float u0 = ((c % 16) * 8.0f) / texW;
        const float v0 = ((c / 16) * 8.0f) / texH;
        const float u1 = u0 + (8.0f / texW);
        const float v1 = v0 + (8.0f / texH);

        const float x0 = penX;
        const float y0 = y;
        const float x1 = penX + w;
        const float y1 = y + h;

        HUD_VERTEX* v = &verts[quadCount * 6];
        v[0] = {x0, y0, 0.0f, 1.0f, color, u0, v0};
        v[1] = {x1, y0, 0.0f, 1.0f, color, u1, v0};
        v[2] = {x1, y1, 0.0f, 1.0f, color, u1, v1};
        v[3] = {x0, y0, 0.0f, 1.0f, color, u0, v0};
        v[4] = {x1, y1, 0.0f, 1.0f, color, u1, v1};
        v[5] = {x0, y1, 0.0f, 1.0f, color, u0, v1};

        quadCount++;
        penX += w;
    }

    if (quadCount > 0) {
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, quadCount * 2, verts, sizeof(HUD_VERTEX));
    }
}

static void Hud_DrawRect(IDirect3DDevice9* pDevice, float x, float y, float w, float h, D3DCOLOR color) {
    if (!pDevice) return;
    if (!Hud_EnsureResources(pDevice)) return;

    pDevice->SetFVF(HUD_FVF);
    pDevice->SetTexture(0, nullptr); // No texture for solid rect

    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    const float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    HUD_VERTEX verts[6] = {
        {x0, y0, 0.0f, 1.0f, color, 0, 0},
        {x1, y0, 0.0f, 1.0f, color, 0, 0},
        {x1, y1, 0.0f, 1.0f, color, 0, 0},
        {x0, y0, 0.0f, 1.0f, color, 0, 0},
        {x1, y1, 0.0f, 1.0f, color, 0, 0},
        {x0, y1, 0.0f, 1.0f, color, 0, 0},
    };

    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(HUD_VERTEX));
}

// Bracket all HUD draw calls with a single state save/restore
static void Hud_BeginBatch(IDirect3DDevice9* pDevice) {
    if (Hud_EnsureResources(pDevice) && g_hudStateBlock) {
        g_hudStateBlock->Capture();
    }
}

static void Hud_EndBatch() {
    if (g_hudStateBlock) {
        g_hudStateBlock->Apply();
    }
}

// EndScene/Reset/Present hooks
static BeginScene_t g_pOriginalBeginScene = nullptr;
static EndScene_t g_pOriginalEndScene = nullptr;
static Reset_t g_pOriginalReset = nullptr;
static Present_t g_pOriginalPresent = nullptr;
static SetRenderTarget_t g_pOriginalSetRenderTarget = nullptr;
static SetViewport_t g_pOriginalSetViewport = nullptr;
static SetWindowPos_t g_pOriginalSetWindowPos = nullptr;
static SetWindowLongA_t g_pOriginalSetWindowLongA = nullptr;
static SetWindowLongW_t g_pOriginalSetWindowLongW = nullptr;
static SetWindowRgn_t g_pOriginalSetWindowRgn = nullptr;
static SetMenu_t g_pOriginalSetMenu = nullptr;
static void* g_BeginSceneTarget = nullptr;
static void* g_EndSceneTarget = nullptr;
static void* g_ResetTarget = nullptr;
static void* g_PresentTarget = nullptr;
static void* g_SetRenderTargetTarget = nullptr;
static void* g_SetViewportTarget = nullptr;
static void* g_SwapChainPresentTarget = nullptr;  // For hooking IDirect3DSwapChain9::Present
static SwapChainPresent_t g_pOriginalSwapChainPresent = nullptr;
static bool g_HooksInstalled = false;
static bool g_internalReset = false;  // Flag to skip hook processing during our own Reset calls
static bool g_internalResize = false; // Flag to allow our own SetWindowPos calls

// Track the backbuffer surface for SetRenderTarget redirection
static IDirect3DSurface9* g_pBackBufferSurface = nullptr;

// Device-lost / minimize state
static bool g_deviceLost = false;              // True when device is in lost state
static bool g_isMinimized = false;             // True when window is minimized

// Cached game backbuffer (refreshed on Reset / scaling init only)
static IDirect3DSurface9* g_pCachedGameBackBuffer = nullptr;

// Letterboxing state tracking
static bool g_letterboxInitialized = false;
static D3DFORMAT g_backbufferFormat = D3DFMT_UNKNOWN;
static int g_presentCallCount = 0;

// ImGui state
static bool g_imguiInitialized = false;
static HWND g_gameWindow = nullptr;
static HWND g_gameParentWindow = nullptr;
static WNDPROC g_proxyOriginalWndProc = nullptr;
static WNDPROC g_imguiOriginalWndProc = nullptr;
static HWND g_pendingFocusReclaimWindow = nullptr;
static DWORD g_pendingFocusReclaimEarliestTick = 0;
static IDirect3DDevice9* g_pDevice = nullptr;
static bool g_imguiDrawDataReady = false;
static bool g_renderingPreparedImGuiToScalingTarget = false;

// Menu state
static bool g_showMenu = false;
static bool g_menuHotkeyF1Down = false;
static bool g_menuHotkeyF11Down = false;
static std::vector<LoadedUserModDLL> g_loadedUserModDLLs;

static constexpr DWORD kStartupFocusReclaimDelayMs = 3000;
static bool g_enableHotkeyTraceLogs = false;
static bool g_enableSwallowTraceLogs = false;
static bool g_inputGuardSettingsLoaded = false;

static void LoadInputGuardSettings() {
    if (g_inputGuardSettingsLoaded) {
        return;
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        wchar_t* fwdSlash = wcsrchr(path, L'/');
        if (!slash || (fwdSlash && fwdSlash > slash)) {
            slash = fwdSlash;
        }
        if (slash) {
            slash[1] = L'\0';
        } else {
            path[0] = L'\0';
        }
        wcscat_s(path, L"as2_rollback_settings.ini");
    } else {
        wcscpy_s(path, L"as2_rollback_settings.ini");
    }

    // The hotkey/swallow trace toggles are no longer INI-configurable; they
    // stay off (their diagnostic logging remains inert).
    g_inputGuardSettingsLoaded = true;
    ProxyLog("[INPUTGUARD] d3d9_proxy: game chain via ModCallGameWndProc (not DXLib 0xFFFF stub); ini=%ls",
        path);
}

// re0.7 M2 (master plan §2.8.1): presentation interval of the proxy-owned
// scaling swap chain. DEFAULT (≙ONE, vsynced) stays the default — windowed
// DWM present at 60 Hz + the mod's 16.667 ms scheduler is the lowest-jitter
// combination. `present_interval=immediate` in as2_rollback_settings.ini
// [ModSettings] is the documented escape for non-60 Hz-multiple displays or
// two-pacer beat stutter (DECOMP §4.4); the scheduler's absolute deadlines
// already treat Present blocking as frame cost, so no other change is needed.
static bool g_scalingPresentIntervalLoaded = false;
static UINT g_scalingPresentInterval = D3DPRESENT_INTERVAL_DEFAULT;

static UINT GetConfiguredScalingPresentInterval() {
    if (g_scalingPresentIntervalLoaded) {
        return g_scalingPresentInterval;
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        wchar_t* fwdSlash = wcsrchr(path, L'/');
        if (!slash || (fwdSlash && fwdSlash > slash)) {
            slash = fwdSlash;
        }
        if (slash) {
            slash[1] = L'\0';
        } else {
            path[0] = L'\0';
        }
        wcscat_s(path, L"as2_rollback_settings.ini");
    } else {
        wcscpy_s(path, L"as2_rollback_settings.ini");
    }

    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"ModSettings", L"present_interval", L"default",
                             value, (DWORD)(sizeof(value) / sizeof(value[0])), path);
    if (_wcsicmp(value, L"immediate") == 0) {
        g_scalingPresentInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    } else {
        g_scalingPresentInterval = D3DPRESENT_INTERVAL_DEFAULT;
    }
    g_scalingPresentIntervalLoaded = true;
    ProxyLog("[SCALING] present_interval config: %ls -> %s (ini=%ls)",
             value[0] ? value : L"default",
             g_scalingPresentInterval == D3DPRESENT_INTERVAL_IMMEDIATE
                 ? "IMMEDIATE" : "DEFAULT (vsync ONE)",
             path);
    return g_scalingPresentInterval;
}

static constexpr uintptr_t kAddrShellHotkeySuppressFlag = 0x009E5B74;
// When 1, game wndproc returns before DefWindowProc (swallows Win/Alt+Shift). See as2_constants.h.
static constexpr uintptr_t kAddrGameWndprocCustomHandler = 0x009DB660;
static constexpr uintptr_t kAddrShellHotkeyAuxHook = 0x009E5B78;
static constexpr uintptr_t kAddrShellHotkeyMsgHook = 0x009E5B7C;
static constexpr uintptr_t kAddrShellHotkeyHookModule = 0x009E5C8C;

void ShutdownConsole(bool logMessage = true);

static void QueueDeferredFocusReclaim(HWND hWnd, const char* reason) {
    if (!hWnd) {
        return;
    }

    g_pendingFocusReclaimWindow = hWnd;
    g_pendingFocusReclaimEarliestTick = GetTickCount() + kStartupFocusReclaimDelayMs;

    ProxyLog("[FOCUS] Deferred focus reclaim queued for hwnd=0x%p earliest=%lu reason=%s",
             hWnd,
             (unsigned long)g_pendingFocusReclaimEarliestTick,
             reason ? reason : "unknown");
}

static void TryProcessDeferredFocusReclaim() {
    if (!g_pendingFocusReclaimWindow) {
        return;
    }

    const DWORD now = GetTickCount();
    if ((LONG)(now - g_pendingFocusReclaimEarliestTick) < 0) {
        return;
    }

    HWND hWnd = g_pendingFocusReclaimWindow;
    g_pendingFocusReclaimWindow = nullptr;
    g_pendingFocusReclaimEarliestTick = 0;

    if (!IsWindow(hWnd)) {
        ProxyLog("[FOCUS] Deferred focus reclaim dropped because the target window no longer exists");
        return;
    }

    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
    }

    if (foregroundWindow && foregroundPid != GetCurrentProcessId()) {
        ProxyLog("[FOCUS] Deferred focus reclaim skipped because foreground belongs to another process: hwnd=0x%p fg=0x%p pid=%lu",
                 hWnd,
                 foregroundWindow,
                 (unsigned long)foregroundPid);
        return;
    }

    if (foregroundWindow == hWnd && GetFocus() == hWnd) {
        ProxyLog("[FOCUS] Deferred focus reclaim no longer needed for hwnd=0x%p", hWnd);
        return;
    }

    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    ProxyLog("[FOCUS] Deferred focus reclaim attempted for hwnd=0x%p fg=0x%p active=0x%p focus=0x%p",
             hWnd,
             GetForegroundWindow(),
             GetActiveWindow(),
             GetFocus());
}

static void NotifyGameExitOnce(int exitCode, const char* reason) {
    if (g_gameExiting) {
        return;
    }

    g_gameExiting = true;

    if (exitCode == 0) {
        g_fastExitRequested = true;
        ProxyLog("[SHUTDOWN] Fast exit requested reason=%s", reason ? reason : "unknown");
        ShutdownConsole(false);
    }

    if (g_pModOnGameExit) {
        g_pModOnGameExit(exitCode, reason);
    }
}

static HWND ResolvePrimaryGameWindow(HWND fallbackWindow = nullptr) {
    const HWND candidates[] = {
        g_gameParentWindow,
        g_gameWindow,
        fallbackWindow
    };

    for (HWND candidate : candidates) {
        if (candidate && IsWindow(candidate)) {
            return candidate;
        }
    }

    return nullptr;
}

static void PostQuitMessageOnce(const char* reason) {
    if (g_quitMessagePosted) {
        return;
    }

    g_quitMessagePosted = true;
    ProxyLog("[SHUTDOWN] Posting WM_QUIT reason=%s", reason ? reason : "unknown");
    PostQuitMessage(0);
}

static void RequestGameShutdown(const char* reason, HWND fallbackWindow = nullptr) {
    NotifyGameExitOnce(0, reason);

    HWND targetWindow = ResolvePrimaryGameWindow(fallbackWindow);
    if (targetWindow) {
        ProxyLog("[SHUTDOWN] Requesting WM_CLOSE on hwnd=0x%p reason=%s",
                 targetWindow,
                 reason ? reason : "unknown");
        PostMessageW(targetWindow, WM_CLOSE, 0, 0);
        return;
    }

    ProxyLog("[SHUTDOWN] No live window available for WM_CLOSE reason=%s",
             reason ? reason : "unknown");
    PostQuitMessageOnce(reason);
}

static bool IsShellHotkeyTraceMessage(UINT msg, WPARAM wParam) {
    if (!g_enableHotkeyTraceLogs) {
        return false;
    }

    if (msg == WM_ACTIVATEAPP || msg == WM_INPUTLANGCHANGEREQUEST || msg == WM_INPUTLANGCHANGE ||
        msg == WM_IME_SETCONTEXT || msg == WM_IME_NOTIFY || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_ENDCOMPOSITION ||
        msg == WM_SYSCHAR || msg == WM_MENUCHAR || msg == WM_HOTKEY) {
        return true;
    }

    if (msg == WM_SYSCOMMAND) {
        const WPARAM command = (wParam & 0xFFF0u);
        return command == SC_TASKLIST || command == SC_KEYMENU;
    }

    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_KEYDOWN || msg == WM_KEYUP) {
        return wParam == VK_LWIN || wParam == VK_RWIN || wParam == VK_MENU || wParam == VK_LMENU ||
               wParam == VK_RMENU || wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT;
    }

    return false;
}

static const char* DescribeShellHotkeyTraceMessage(UINT msg, WPARAM wParam) {
    switch (msg) {
    case WM_ACTIVATEAPP:
        return "WM_ACTIVATEAPP";
    case WM_INPUTLANGCHANGEREQUEST:
        return "WM_INPUTLANGCHANGEREQUEST";
    case WM_INPUTLANGCHANGE:
        return "WM_INPUTLANGCHANGE";
    case WM_IME_SETCONTEXT:
        return "WM_IME_SETCONTEXT";
    case WM_IME_NOTIFY:
        return "WM_IME_NOTIFY";
    case WM_IME_COMPOSITION:
        return "WM_IME_COMPOSITION";
    case WM_IME_STARTCOMPOSITION:
        return "WM_IME_STARTCOMPOSITION";
    case WM_IME_ENDCOMPOSITION:
        return "WM_IME_ENDCOMPOSITION";
    case WM_SYSCHAR:
        return "WM_SYSCHAR";
    case WM_MENUCHAR:
        return "WM_MENUCHAR";
    case WM_HOTKEY:
        return "WM_HOTKEY";
    case WM_SYSCOMMAND:
        return ((wParam & 0xFFF0u) == SC_TASKLIST) ? "WM_SYSCOMMAND/SC_TASKLIST" :
               ((wParam & 0xFFF0u) == SC_KEYMENU) ? "WM_SYSCOMMAND/SC_KEYMENU" :
               "WM_SYSCOMMAND";
    case WM_SYSKEYDOWN:
        return "WM_SYSKEYDOWN";
    case WM_SYSKEYUP:
        return "WM_SYSKEYUP";
    case WM_KEYDOWN:
        return "WM_KEYDOWN";
    case WM_KEYUP:
        return "WM_KEYUP";
    default:
        return "UNKNOWN";
    }
}

static void LogShellHotkeyTraceState(const char* stage, HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!IsShellHotkeyTraceMessage(msg, wParam)) {
        return;
    }

    const HWND foregroundWindow = GetForegroundWindow();
    const DWORD windowThreadId = hWnd ? GetWindowThreadProcessId(hWnd, nullptr) : 0;
    const DWORD foregroundThreadId = foregroundWindow ? GetWindowThreadProcessId(foregroundWindow, nullptr) : 0;
    const HKL windowLayout = windowThreadId ? GetKeyboardLayout(windowThreadId) : nullptr;
    const HKL foregroundLayout = foregroundThreadId ? GetKeyboardLayout(foregroundThreadId) : nullptr;
    int* suppressFlag = reinterpret_cast<int*>(kAddrShellHotkeySuppressFlag);
    HHOOK* auxHookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyAuxHook);
    HHOOK* hookHandle = reinterpret_cast<HHOOK*>(kAddrShellHotkeyMsgHook);
    HMODULE* hookModule = reinterpret_cast<HMODULE*>(kAddrShellHotkeyHookModule);

    ProxyLog("[HOTKEYTRACE][%s] %s hwnd=0x%p wParam=0x%08X lParam=0x%08X fg=0x%p active=0x%p focus=0x%p currentWndProc=0x%p wndThread=%lu wndLayout=0x%p fgThread=%lu fgLayout=0x%p showMenu=%d imgui=%d suppress=%d aux=0x%p msgHook=0x%p module=0x%p",
             stage ? stage : "unknown",
             DescribeShellHotkeyTraceMessage(msg, wParam),
             hWnd,
             (unsigned int)wParam,
             (unsigned int)lParam,
             foregroundWindow,
             GetActiveWindow(),
             GetFocus(),
             hWnd ? (void*)GetWindowLongPtrW(hWnd, GWLP_WNDPROC) : nullptr,
             (unsigned long)windowThreadId,
             windowLayout,
             (unsigned long)foregroundThreadId,
             foregroundLayout,
             g_showMenu ? 1 : 0,
             g_imguiInitialized ? 1 : 0,
             *suppressFlag,
             *auxHookHandle,
             *hookHandle,
             *hookModule);

    if (msg == WM_ACTIVATEAPP || msg == WM_INPUTLANGCHANGEREQUEST || msg == WM_INPUTLANGCHANGE ||
        msg == WM_IME_SETCONTEXT || msg == WM_IME_NOTIFY || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_ENDCOMPOSITION ||
        (foregroundWindow && foregroundWindow != hWnd)) {
        char foregroundClass[128] = {};
        char foregroundTitle[128] = {};
        if (foregroundWindow) {
            GetClassNameA(foregroundWindow, foregroundClass, sizeof(foregroundClass));
            GetWindowTextA(foregroundWindow, foregroundTitle, sizeof(foregroundTitle));
        }

        ProxyLog("[HOTKEYTRACE][%s][FG] hwnd=0x%p class='%s' title='%s'",
                 stage ? stage : "unknown",
                 foregroundWindow,
                 foregroundClass,
                 foregroundTitle);
    }
}

static bool IsExclusiveModOverlayActive() {
    return g_pModWantsExclusiveOverlay ? g_pModWantsExclusiveOverlay() : false;
}

// Borderless fullscreen state
static bool g_useBorderlessFullscreen = true;  // Enable by default
static bool g_forceWindowed = false;            // Force windowed for test harness
static bool g_keepAspectRatio = true;          // Preserve 4:3 aspect ratio
static int g_nativeWidth = 640;                // Game's native width
static int g_nativeHeight = 480;               // Game's native height
static int g_windowedWidth = 0;                // Saved windowed mode width
static int g_windowedHeight = 0;               // Saved windowed mode height
static int g_screenWidth = 0;                  // Actual screen width
static int g_screenHeight = 0;                 // Actual screen height

// Window resize scaling (for windowed mode)
static int g_currentWindowWidth = 640;         // Current window client width
static int g_currentWindowHeight = 480;        // Current window client height
static bool g_windowResizedNeedsReinit = false; // Flag to reinit scaling on next frame

// =============================================================================
// SCALING SOLUTION DOCUMENTATION
// =============================================================================
// Problem: DXLib stores backbuffer dimensions internally and uses them for ALL
//          rendering calculations. We cannot intercept this.
//
// Solution: 
//   1. Keep device backbuffer at native 640x480 (DXLib renders correctly)
//   2. Create an ADDITIONAL swap chain at window size (1920x1440)
//   3. In HookedPresent: StretchRect from 640x480 backbuffer to large swap chain
//   4. Present the large swap chain (which fills the window)
//   5. Skip the original Present (or let it present to nothing)
// =============================================================================

// Scaling swap chain - this is the swap chain that actually fills the window
static IDirect3DSwapChain9* g_pScalingSwapChain = nullptr;
static IDirect3DSurface9* g_pScalingBackBuffer = nullptr;
static bool g_scalingInitialized = false;

// Letterboxing state - for proper aspect ratio scaling using textured quad
static IDirect3DTexture9* g_pScaleTexture = nullptr;      // Texture to hold game's rendered frame
static IDirect3DSurface9* g_pScaleTextureSurface = nullptr; // Surface of the texture
static IDirect3DVertexBuffer9* g_pQuadVB = nullptr;       // Vertex buffer for fullscreen quad
static D3DVIEWPORT9 g_letterboxViewport = {0};
static bool g_letterboxActive = false;
static RECT g_letterboxDestRect = {0};  // Destination rect for scaled quad

// Legacy swap chain variables (kept for compatibility)
static IDirect3DSwapChain9* g_pLetterboxSwapChain = nullptr;
static IDirect3DSurface9* g_pLetterboxBackBuffer = nullptr;

// Vertex format for textured quad
struct SCALED_VERTEX {
    float x, y, z, rhw;
    float u, v;
};
#define SCALED_VERTEX_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

// Saved original present parameters from device creation
static D3DPRESENT_PARAMETERS g_originalPresentParams = {0};
static bool g_hasSavedPresentParams = false;

// Deferred reset request (to be processed at safe time - start of frame)
static bool g_pendingReset = false;
static int g_pendingResetWidth = 0;
static int g_pendingResetHeight = 0;
static bool g_pendingResetBorderless = false;

// Borderless/windowed mode state
static bool g_isCurrentlyBorderless = true;

// Log file and console
static FILE* g_logFile = nullptr;
static unsigned int g_logLinesSinceFlush = 0;
static HANDLE g_hConsole = INVALID_HANDLE_VALUE;
static bool g_consoleAllocated = false;
static bool g_consoleVisible = false;

// DLL path (for full path logging)
static char g_dllPath[MAX_PATH] = {0};
static char g_dllDir[MAX_PATH] = {0};
static char g_logDir[MAX_PATH] = {0};
static wchar_t g_dllPathW[MAX_PATH] = {0};
static wchar_t g_dllDirW[MAX_PATH] = {0};
static wchar_t g_logDirW[MAX_PATH] = {0};
static DWORD g_processAttachTick = 0;
static unsigned int g_proxyLogSequence = 0;
static bool g_proxyFlushEveryLine = true;
static char g_startupStage[128] = "before process attach";
static HMODULE g_hPinnedProxyModule = nullptr;
static bool g_proxyModulePinned = false;
static DWORD g_lastBorderlessDelayLogTick = 0;

static constexpr DWORD kGameDefaultFontHandleAddr = 0x009CC064;
static constexpr DWORD kGameFontCacheRebuildStart = 0x006222E0;
static constexpr DWORD kGameFontCacheRebuildEnd = 0x0062247F;
static constexpr DWORD kGameFontInitStart = 0x00622730;
static constexpr DWORD kGameFontInitEnd = 0x00622EF1;
static constexpr DWORD kGameGraphCreateStart = 0x00612080;
static constexpr DWORD kGameGraphCreateEnd = 0x006127C9;
static constexpr DWORD kGameGraphHandleBindStart = 0x00613230;
static constexpr DWORD kGameGraphHandleBindEnd = 0x006133A0;
static constexpr DWORD kGameImageRegisterHandleStart = 0x00620930;
static constexpr DWORD kGameImageRegisterHandleEnd = 0x00620A00;

// ============================================================================
// Display Config — persist window mode + size to as2_display.cfg
// ============================================================================

// Forward declaration — ProxyLog is defined later in the file
void ProxyLog(const char* fmt, ...);

static void WideToUtf8(const wchar_t* wide, char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = '\0';
    if (!wide) {
        strncpy_s(out, cap, "<null>", _TRUNCATE);
        return;
    }

    int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, nullptr, nullptr);
    if (written <= 0) {
        snprintf(out, cap, "<utf8 conversion failed err=%lu>", GetLastError());
    }
}

static void WideToAnsi(UINT codePage, const wchar_t* wide, char* out, int cap, BOOL* usedDefaultChar) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = '\0';
    if (usedDefaultChar) {
        *usedDefaultChar = FALSE;
    }
    if (!wide) {
        return;
    }

    BOOL usedDefault = FALSE;
    int written = WideCharToMultiByte(codePage, 0, wide, -1, out, cap, nullptr, &usedDefault);
    if (usedDefaultChar) {
        *usedDefaultChar = usedDefault;
    }
    if (written <= 0) {
        snprintf(out, cap, "<cp%u conversion failed err=%lu>", codePage, GetLastError());
    }
}

static void FormatWin32Error(DWORD err, char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }

    DWORD written = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        0,
        out,
        (DWORD)cap,
        nullptr);
    if (!written) {
        snprintf(out, cap, "Win32 error %lu", err);
        return;
    }

    while (written > 0 && (out[written - 1] == '\r' || out[written - 1] == '\n' || out[written - 1] == ' ')) {
        out[--written] = '\0';
    }
}

static void PinProxyModuleForProcessLifetime(HMODULE attachModule) {
    if (g_proxyModulePinned) {
        return;
    }

    HMODULE pinnedModule = nullptr;
    const BOOL pinned = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&g_startupStage),
        &pinnedModule);

    if (pinned) {
        g_hPinnedProxyModule = pinnedModule;
        g_proxyModulePinned = true;
        ProxyLog("[INIT] Proxy DLL pinned for process lifetime: attach=0x%p pinned=0x%p",
                 attachModule,
                 g_hPinnedProxyModule);
        if (attachModule && attachModule != g_hPinnedProxyModule) {
            ProxyLog("[INIT] WARNING: pinned proxy module handle differs from DllMain handle");
        }
        return;
    }

    const DWORD err = GetLastError();
    char errText[256];
    FormatWin32Error(err, errText, sizeof(errText));
    ProxyLog("[INIT] WARNING: failed to pin proxy DLL; FreeLibrary could unload live hooks (err=%lu %s)",
             err,
             errText);
}

static void SetStartupStage(const char* stage) {
    if (!stage || !stage[0]) {
        return;
    }
    strncpy_s(g_startupStage, sizeof(g_startupStage), stage, _TRUNCATE);
    ProxyLog("[STAGE] %s", g_startupStage);
}

static void UpdateStartupStageSilently(const char* stage) {
    if (!stage || !stage[0]) {
        return;
    }
    strncpy_s(g_startupStage, sizeof(g_startupStage), stage, _TRUNCATE);
}

static bool ReadGameDwordSafe(DWORD address, DWORD* outValue) {
    if (!outValue) {
        return false;
    }
    __try {
        *outValue = *reinterpret_cast<volatile DWORD*>(address);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *outValue = 0;
        return false;
    }
}

static bool IsGameDefaultFontReady(DWORD* outHandle = nullptr) {
    DWORD handle = 0;
    if (!ReadGameDwordSafe(kGameDefaultFontHandleAddr, &handle)) {
        if (outHandle) {
            *outHandle = 0;
        }
        return false;
    }
    if (outHandle) {
        *outHandle = handle;
    }
    return handle != 0 && handle != 0xFFFFFFFFu;
}

static bool ShouldDelayInitialBorderlessForFontInit() {
    DWORD fontHandle = 0;
    if (IsGameDefaultFontReady(&fontHandle)) {
        return false;
    }

    const DWORD now = GetTickCount();
    if (g_lastBorderlessDelayLogTick == 0 || now - g_lastBorderlessDelayLogTick >= 1000) {
        g_lastBorderlessDelayLogTick = now;
        ProxyLog("[BORDERLESS] Delaying initial borderless resize until game default font cache is ready (fontHandle=0x%08lX)",
                 fontHandle);
    }
    return true;
}

static const char* DescribeKnownGameAddress(DWORD address) {
    if (address >= kGameFontInitStart && address <= kGameFontInitEnd) {
        return "DXLib font initialization / CreateFontToHandle";
    }
    if (address >= kGameFontCacheRebuildStart && address <= kGameFontCacheRebuildEnd) {
        return "DXLib font cache rebuild / graph surface creation";
    }
    if (address >= kGameGraphCreateStart && address <= kGameGraphCreateEnd) {
        return "DXLib graph/screen handle creation";
    }
    if (address >= kGameGraphHandleBindStart && address <= kGameGraphHandleBindEnd) {
        return "DXLib graph handle bind/setup";
    }
    if (address >= kGameImageRegisterHandleStart && address <= kGameImageRegisterHandleEnd) {
        return "DXLib image handle registration";
    }
    return nullptr;
}

static void ProxyLogWideValue(const char* label, const wchar_t* value) {
    char utf8[1024];
    WideToUtf8(value, utf8, sizeof(utf8));
    ProxyLog("%s%s", label ? label : "", utf8);
}

static void ProxyLogFileProbe(const char* label, const char* path) {
    if (!path || !path[0]) {
        ProxyLog("[PROBE] %s path=<empty>", label ? label : "file");
        return;
    }

    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        char errText[256];
        FormatWin32Error(err, errText, sizeof(errText));
        ProxyLog("[PROBE] %s missing/unreadable: %s (err=%lu %s)",
                 label ? label : "file", path, err, errText);
        return;
    }

    LARGE_INTEGER size = {};
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            GetFileSizeEx(file, &size);
            CloseHandle(file);
        }
    }

    ProxyLog("[PROBE] %s exists: %s attrs=0x%08lX size=%lld",
             label ? label : "file", path, attrs, (long long)size.QuadPart);
}

static void ProxyLogModuleByName(const char* moduleName) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module) {
        ProxyLog("[MODULE] %-18s not loaded", moduleName ? moduleName : "<null>");
        return;
    }

    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    ProxyLog("[MODULE] %-18s base=0x%p path=%s", moduleName, module, path[0] ? path : "<unknown>");
}

static void ProxyLogModuleSnapshot(const char* reason) {
    ProxyLog("[MODULE] Snapshot: %s", reason ? reason : "<unspecified>");
    const char* modules[] = {
        "d3d9.dll",
        "as2_rollback.dll",
        "SDL3.dll",
        "wsock32.dll",
        "ddraw.dll",
        "dinput.dll",
        "dinput8.dll",
        "dsound.dll",
        "kernel32.dll",
        "user32.dll",
        "imm32.dll",
    };
    for (const char* name : modules) {
        ProxyLogModuleByName(name);
    }
}

static void ProxyLogProcessDiagnostics(HMODULE proxyModule) {
    ProxyLog("[ENV] PID=%lu TID=%lu attachTick=%lu", GetCurrentProcessId(), GetCurrentThreadId(), g_processAttachTick);
    ProxyLog("[ENV] CodePages: ACP=%u OEMCP=%u ThreadLocale=0x%08lX UIlang=0x%04X",
             GetACP(), GetOEMCP(), (DWORD)GetThreadLocale(), (unsigned)GetThreadUILanguage());

    char localeName[128] = {};
    if (GetLocaleInfoA(LOCALE_SYSTEM_DEFAULT, LOCALE_SNAME, localeName, sizeof(localeName)) > 0) {
        ProxyLog("[ENV] System locale: %s", localeName);
    }
    if (GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SNAME, localeName, sizeof(localeName)) > 0) {
        ProxyLog("[ENV] User locale: %s", localeName);
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64)) {
        ProxyLog("[ENV] Process architecture: 32-bit%s", wow64 ? " on 64-bit Windows (WOW64)" : "");
    }

    char cwdA[MAX_PATH] = {};
    if (GetCurrentDirectoryA(MAX_PATH, cwdA) > 0) {
        ProxyLog("[ENV] CurrentDirectoryA: %s", cwdA);
    }
    wchar_t cwdW[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwdW) > 0) {
        ProxyLogWideValue("[ENV] CurrentDirectoryW: ", cwdW);
    }

    ProxyLog("[ENV] CommandLineA: %s", GetCommandLineA());
    ProxyLogWideValue("[ENV] CommandLineW: ", GetCommandLineW());

    char proxyPathFromHandle[MAX_PATH] = {};
    if (GetModuleFileNameA(proxyModule, proxyPathFromHandle, MAX_PATH) > 0) {
        ProxyLog("[ENV] Proxy path from handle A: %s", proxyPathFromHandle);
    }
    wchar_t proxyPathFromHandleW[MAX_PATH] = {};
    if (GetModuleFileNameW(proxyModule, proxyPathFromHandleW, MAX_PATH) > 0) {
        ProxyLogWideValue("[ENV] Proxy path from handle W: ", proxyPathFromHandleW);
    }

    HMODULE gameModule = GetModuleHandleA(nullptr);
    char gamePathA[MAX_PATH] = {};
    if (GetModuleFileNameA(gameModule, gamePathA, MAX_PATH) > 0) {
        ProxyLog("[ENV] Game module A: base=0x%p path=%s", gameModule, gamePathA);
    }
    wchar_t gamePathW[MAX_PATH] = {};
    if (GetModuleFileNameW(gameModule, gamePathW, MAX_PATH) > 0) {
        ProxyLogWideValue("[ENV] Game module W: ", gamePathW);
    }
}

static void DisplayConfig_GetPath(char* out, int cap) {
    snprintf(out, cap, "%s\\as2_display.cfg", g_dllDir);
}

static void DisplayConfig_Load() {
    char path[MAX_PATH];
    DisplayConfig_GetPath(path, MAX_PATH);
    ProxyLogFileProbe("[CONFIG] as2_display.cfg", path);

    FILE* f = fopen(path, "r");
    if (!f) {
        ProxyLog("[CONFIG] No display config found (%s) — using defaults", path);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and section headers
        if (line[0] == '#' || line[0] == '[' || line[0] == '\n' || line[0] == '\r')
            continue;

        char key[64] = {0};
        char val[128] = {0};
        if (sscanf(line, "%63[^=]=%127[^\r\n]", key, val) == 2) {
            if (strcmp(key, "borderless") == 0) {
                int v = atoi(val);
                g_useBorderlessFullscreen = (v != 0);
                g_isCurrentlyBorderless = g_useBorderlessFullscreen;
            } else if (strcmp(key, "windowed_width") == 0) {
                int v = atoi(val);
                if (v >= 640) g_windowedWidth = v;
            } else if (strcmp(key, "windowed_height") == 0) {
                int v = atoi(val);
                if (v >= 480) g_windowedHeight = v;
            } else if (strcmp(key, "keep_aspect") == 0) {
                g_keepAspectRatio = (atoi(val) != 0);
            }
        }
    }
    fclose(f);

    ProxyLog("[CONFIG] Loaded display config: borderless=%d, windowed=%dx%d, aspect=%d",
             g_useBorderlessFullscreen, g_windowedWidth, g_windowedHeight, g_keepAspectRatio);
}

static void DisplayConfig_Save() {
    char path[MAX_PATH];
    DisplayConfig_GetPath(path, MAX_PATH);

    FILE* f = fopen(path, "w");
    if (!f) {
        ProxyLog("[CONFIG] ERROR: Failed to write display config: %s", path);
        return;
    }

    fprintf(f, "# Alice Senki 2 — Display Settings\n");
    fprintf(f, "# Auto-saved by the mod. Edit at your own risk.\n");
    fprintf(f, "[display]\n");
    fprintf(f, "borderless=%d\n", g_isCurrentlyBorderless ? 1 : 0);
    fprintf(f, "windowed_width=%d\n", g_currentWindowWidth > 0 ? g_currentWindowWidth : 640);
    fprintf(f, "windowed_height=%d\n", g_currentWindowHeight > 0 ? g_currentWindowHeight : 480);
    fprintf(f, "keep_aspect=%d\n", g_keepAspectRatio ? 1 : 0);

    fclose(f);
    ProxyLog("[CONFIG] Saved display config: borderless=%d, windowed=%dx%d, aspect=%d",
             g_isCurrentlyBorderless, g_currentWindowWidth, g_currentWindowHeight, g_keepAspectRatio);
}

// ============================================================================
// Window Title Override
// ============================================================================

// Keep these as simple constants for now.
// If we later centralize versioning, these can move into a shared header.
static const wchar_t* kAs2GameVersion = L"1.060B";
static const wchar_t* kAs2ModName = L"ImprovementMod";
static const wchar_t* kAs2ModVersion = L"0.7-beta2.01";

static HWND g_titleWindow = nullptr;
static bool g_titleApplied = false;

static ImFont* g_netplayHudFonts[3] = {};

// Shippori Mincho Bold, for the in-game menus. The vanilla settings labels are
// Mincho (verified against the sprites in data/opt.bin), so the mod's menus use
// the same family instead of the game's built-in bitmap font.
static ImFont* g_menuFont = nullptr;

// Second bake of the same face for the pause menu, which matches the vanilla
// glyph size. ImFontConfig::SizePixels maps ascent-descent, not the em: for
// Shippori Mincho Bold (ascent 1160, descent -288, capHeight 737 per 1000 upem)
// caps = 0.509 * SizePixels, so vanilla's 25 px caps need ~49. Upscaling the
// 19 px atlas that far is a blur, and a 49 px CJK atlas would need 4096x4096 -
// so this one carries Latin + Cyrillic only and fits in 1024x1024. Strings with
// CJK fall back to g_menuFont.
// Cyrillic only: merging the Latin block too would let the fallback face
// override Shippori's own Latin, which is the shape the menu was tuned to.
static const ImWchar kMenuCyrillicRanges[] = {
    0x0400, 0x052F,   // Cyrillic + Supplement
    0x2DE0, 0x2DFF,   // Cyrillic Extended-A
    0xA640, 0xA69F,   // Cyrillic Extended-B
    0,
};

static ImFont* g_menuFontLarge = nullptr;
static ImVector<ImWchar> g_menuLargeGlyphRanges;
constexpr float kMenuFontLargeSize = 49.0f;

// Text the mod queued this frame, in the game's 640x480 space. ImGui's
// DisplaySize is already native, so these coordinates need no mapping.
struct QueuedMenuText {
    float x, y;
    float size;   // 0 = the font's own size
    ImU32 color;
    bool  hasHighCodepoint;   // anything above U+024F: keep it on the CJK atlas
    char  text[192];
};
static QueuedMenuText g_menuTextQueue[256];
static int            g_menuTextCount = 0;

// Kept alive for the lifetime of the atlas: ImGui stores the pointer rather
// than copying the ranges.
static ImVector<ImWchar> g_overlayGlyphRanges;

static void ConfigureOverlayFonts(ImGuiIO& io) {
    ImFont* loadedFont = nullptr;

    // Nicknames come from other players, so the overlay has to cover more than
    // Latin: Japanese for the game's own audience and Cyrillic on top of it.
    // GetGlyphRangesJapanese() carries no Cyrillic, so merge the two.
    ImFontGlyphRangesBuilder rangeBuilder;
    rangeBuilder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    rangeBuilder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    g_overlayGlyphRanges.clear();
    rangeBuilder.BuildRanges(&g_overlayGlyphRanges);
    const ImWchar* glyphRanges = g_overlayGlyphRanges.Data;

    // Prefer the font we ship: it makes rendering identical everywhere instead
    // of depending on which fonts the host happens to have installed.
    const void* embeddedFont = nullptr;
    DWORD embeddedFontSize = 0;
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&ConfigureOverlayFonts, &selfModule);
    if (HRSRC res = FindResourceA(selfModule, MAKEINTRESOURCEA(IDR_OVERLAY_FONT), RT_RCDATA)) {
        if (HGLOBAL handle = LoadResource(selfModule, res)) {
            embeddedFont = LockResource(handle);
            embeddedFontSize = SizeofResource(selfModule, res);
        }
    }

    if (embeddedFont && embeddedFontSize > 0) {
        ImFontConfig embeddedConfig{};
        embeddedConfig.OversampleH = 1;
        embeddedConfig.OversampleV = 1;
        embeddedConfig.PixelSnapH = true;
        // The bytes live in the module image; ImGui must not free them.
        embeddedConfig.FontDataOwnedByAtlas = false;

        loadedFont = io.Fonts->AddFontFromMemoryTTF(
            const_cast<void*>(embeddedFont), (int)embeddedFontSize,
            16.0f, &embeddedConfig, glyphRanges);

        if (loadedFont) {
            ProxyLog("[IMGUI] Loaded embedded overlay font (%lu bytes, JP + Cyrillic ranges)",
                     embeddedFontSize);
            if (HRSRC mres = FindResourceA(selfModule, MAKEINTRESOURCEA(IDR_MENU_FONT), RT_RCDATA)) {
                if (HGLOBAL mh = LoadResource(selfModule, mres)) {
                    void* mdata = LockResource(mh);
                    const DWORD msize = SizeofResource(selfModule, mres);
                    if (mdata && msize) {
                        ImFontConfig menuConfig{};
                        menuConfig.OversampleH = 1;
                        menuConfig.OversampleV = 1;
                        menuConfig.PixelSnapH = true;
                        menuConfig.FontDataOwnedByAtlas = false;
                        g_menuFont = io.Fonts->AddFontFromMemoryTTF(
                            mdata, (int)msize, 19.0f, &menuConfig, glyphRanges);
                        ProxyLog("[IMGUI] Menu font (Shippori Mincho Bold): %s",
                                 g_menuFont ? "loaded" : "FAILED");

                        // Shippori Mincho Bold has no Cyrillic at all - its cmap
                        // simply has no segment covering U+0400..U+04FF - so
                        // asking it for those ranges yielded an atlas with none
                        // of them and every Cyrillic name rendered as blanks.
                        // The overlay face (Noto Sans CJK JP) does have the
                        // block, so it is merged in rather than swapping the
                        // whole menu over to a face with the wrong weight.
                        if (g_menuFont) {
                            ImFontConfig mergeConfig = menuConfig;
                            mergeConfig.MergeMode = true;
                            io.Fonts->AddFontFromMemoryTTF(
                                const_cast<void*>(embeddedFont), (int)embeddedFontSize,
                                19.0f, &mergeConfig, kMenuCyrillicRanges);
                            ProxyLog("[IMGUI] Menu font: merged Cyrillic from the overlay face");
                        }

                        ImFontGlyphRangesBuilder largeBuilder;
                        largeBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
                        g_menuLargeGlyphRanges.clear();
                        largeBuilder.BuildRanges(&g_menuLargeGlyphRanges);
                        g_menuFontLarge = io.Fonts->AddFontFromMemoryTTF(
                            mdata, (int)msize, kMenuFontLargeSize, &menuConfig,
                            g_menuLargeGlyphRanges.Data);
                        // Same story at the large size: the Latin comes from
                        // Shippori, the Cyrillic has to come from Noto.
                        if (g_menuFontLarge) {
                            ImFontConfig mergeConfig = menuConfig;
                            mergeConfig.MergeMode = true;
                            io.Fonts->AddFontFromMemoryTTF(
                                const_cast<void*>(embeddedFont), (int)embeddedFontSize,
                                kMenuFontLargeSize, &mergeConfig, kMenuCyrillicRanges);
                        }
                        ProxyLog("[IMGUI] Menu font large (%.0f px, Latin + merged Cyrillic): %s",
                                 kMenuFontLargeSize,
                                 g_menuFontLarge ? "loaded" : "FAILED");
                    }
                }
            }
            io.FontDefault = loadedFont;
            for (int i = 0; i < 3; ++i) {
                const float sizes[] = { 12.0f, 14.0f, 16.0f };
                g_netplayHudFonts[i] = io.Fonts->AddFontFromMemoryTTF(
                    const_cast<void*>(embeddedFont), (int)embeddedFontSize,
                    sizes[i], &embeddedConfig, glyphRanges);
                if (!g_netplayHudFonts[i]) {
                    g_netplayHudFonts[i] = loadedFont;
                }
            }
            return;
        }
        ProxyLog("[IMGUI] WARNING: embedded font failed to load, falling back to system fonts");
    }

    char windowsDir[MAX_PATH] = {};
    if (GetWindowsDirectoryA(windowsDir, MAX_PATH) == 0) {
        io.Fonts->AddFontDefault();
        ProxyLog("[IMGUI] WARNING: GetWindowsDirectoryA failed, using default font only");
        return;
    }

    // MS Gothic first: it is the closest match to the game's own bitmap face and
    // covers both scripts. Yu Gothic is the fallback when it is absent.
    const char* candidates[] = {
        "msgothic.ttc",
        "YuGothM.ttc",
        "YuGothL.ttc",
        "meiryo.ttc",
    };

    char fontPath[MAX_PATH] = {};
    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;
    fontConfig.FontNo = 0;

    for (const char* candidate : candidates) {
        snprintf(fontPath, sizeof(fontPath), "%s\\Fonts\\%s", windowsDir, candidate);
        loadedFont = io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, &fontConfig, glyphRanges);
        if (loadedFont) {
            ProxyLog("[IMGUI] Loaded overlay font: %s (JP + Cyrillic ranges)", fontPath);
            break;
        }
    }

    if (!loadedFont) {
        ProxyLog("[IMGUI] WARNING: no Japanese/Cyrillic-capable system font found, using default font only");
        loadedFont = io.Fonts->AddFontDefault();
        fontPath[0] = '\0';
    }

    io.FontDefault = loadedFont;

    const float hudFontSizes[] = { 12.0f, 14.0f, 16.0f };
    for (int i = 0; i < 3; ++i) {
        if (fontPath[0]) {
            g_netplayHudFonts[i] = io.Fonts->AddFontFromFileTTF(
                fontPath, hudFontSizes[i], &fontConfig, glyphRanges);
        }
        if (!g_netplayHudFonts[i]) {
            g_netplayHudFonts[i] = loadedFont;
        }
    }
}

static void ApplyCustomWindowTitle(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return;

    // Apply once per window handle.
    if (g_titleApplied && g_titleWindow == hWnd) return;

    wchar_t title[256] = {0};
    _snwprintf_s(title, _countof(title), _TRUNCATE,
                 L"Alice Senki 2 %s - %s %s",
                 kAs2GameVersion,
                 kAs2ModName,
                 kAs2ModVersion);

    if (SetWindowTextW(hWnd, title)) {
        g_titleWindow = hWnd;
        g_titleApplied = true;
        ProxyLog("[WINDOW] Title set: %ls", title);
    } else {
        ProxyLog("[WINDOW] WARNING: Failed to set window title (err=%lu)", GetLastError());
    }
}

// ============================================================================
// Crash Handler - Logs crash address and basic info
// ============================================================================

static LPTOP_LEVEL_EXCEPTION_FILTER g_previousExceptionFilter = nullptr;

static const char* GetExceptionCodeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT: return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP: return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case 0xC00002B4: return "STATUS_FLOAT_MULTIPLE_FAULTS";
        case 0xC00002B5: return "STATUS_FLOAT_MULTIPLE_TRAPS";
        default: return "UNKNOWN";
    }
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    PEXCEPTION_RECORD pRecord = pExceptionInfo->ExceptionRecord;
    PCONTEXT pContext = pExceptionInfo->ContextRecord;
    
    // Open crash log file
    char crashLogPath[MAX_PATH];
    snprintf(crashLogPath, MAX_PATH, "%s\\crash_log.txt", g_dllDir);
    FILE* crashFile = nullptr;
    if (g_dllDirW[0]) {
        wchar_t crashLogPathW[MAX_PATH] = {};
        swprintf_s(crashLogPathW, MAX_PATH, L"%s\\crash_log.txt", g_dllDirW);
        _wfopen_s(&crashFile, crashLogPathW, L"a");
    }
    if (!crashFile) {
        crashFile = fopen(crashLogPath, "a");
    }
    
    // Get timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    
    // Build crash message
    char msg[16384];
    int len = 0;
    
    len += snprintf(msg + len, sizeof(msg) - len,
        "\n========================================\n"
        "CRASH DETECTED: %04d-%02d-%02d %02d:%02d:%02d\n"
        "========================================\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    
    len += snprintf(msg + len, sizeof(msg) - len,
        "Exception Code: 0x%08X (%s)\n"
        "Exception Address: 0x%p\n",
        pRecord->ExceptionCode,
        GetExceptionCodeName(pRecord->ExceptionCode),
        pRecord->ExceptionAddress);
    if (const char* knownAddress = DescribeKnownGameAddress((DWORD)(DWORD_PTR)pRecord->ExceptionAddress)) {
        len += snprintf(msg + len, sizeof(msg) - len,
            "Known Address: %s\n",
            knownAddress);
    }

    char dllPathUtf8[1024] = {};
    char dllDirUtf8[1024] = {};
    WideToUtf8(g_dllPathW, dllPathUtf8, sizeof(dllPathUtf8));
    WideToUtf8(g_dllDirW, dllDirUtf8, sizeof(dllDirUtf8));
    len += snprintf(msg + len, sizeof(msg) - len,
        "Startup Stage: %s\n"
        "PID/TID: %lu/%lu\n"
        "Attach Tick: %lu  Current Tick: %lu  Elapsed: %lu ms\n"
        "ACP/OEMCP: %u/%u  ThreadLocale: 0x%08lX\n"
        "Proxy DLL A: %s\n"
        "Proxy DLL W(utf8): %s\n"
        "Proxy Dir A: %s\n"
        "Proxy Dir W(utf8): %s\n",
        g_startupStage,
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        g_processAttachTick,
        GetTickCount(),
        g_processAttachTick ? (GetTickCount() - g_processAttachTick) : 0,
        GetACP(),
        GetOEMCP(),
        (DWORD)GetThreadLocale(),
        g_dllPath,
        dllPathUtf8,
        g_dllDir,
        dllDirUtf8);

    DWORD defaultFontHandle = 0;
    const bool defaultFontReadable = ReadGameDwordSafe(kGameDefaultFontHandleAddr, &defaultFontHandle);
    len += snprintf(msg + len, sizeof(msg) - len,
        "Display State: borderless=%d currentBorderless=%d scalingInit=%d presentFrame=%d defaultFontHandle=%s0x%08lX\n",
        g_useBorderlessFullscreen ? 1 : 0,
        g_isCurrentlyBorderless ? 1 : 0,
        g_scalingInitialized ? 1 : 0,
        g_presentCallCount,
        defaultFontReadable ? "" : "<unreadable> ",
        defaultFontHandle);
    
    // For access violations, show the address that was accessed
    if (pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2) {
        const char* accessType = (pRecord->ExceptionInformation[0] == 0) ? "READ" : 
                                 (pRecord->ExceptionInformation[0] == 1) ? "WRITE" : "EXECUTE";
        len += snprintf(msg + len, sizeof(msg) - len,
            "Access Type: %s\n"
            "Target Address: 0x%p\n",
            accessType,
            (void*)pRecord->ExceptionInformation[1]);
    }
    
    // Dump registers
    len += snprintf(msg + len, sizeof(msg) - len,
        "\nRegisters:\n"
        "  EIP: 0x%08X  ESP: 0x%08X  EBP: 0x%08X\n"
        "  EAX: 0x%08X  EBX: 0x%08X  ECX: 0x%08X\n"
        "  EDX: 0x%08X  ESI: 0x%08X  EDI: 0x%08X\n",
        pContext->Eip, pContext->Esp, pContext->Ebp,
        pContext->Eax, pContext->Ebx, pContext->Ecx,
        pContext->Edx, pContext->Esi, pContext->Edi);
    
    // ========================================================================
    // FPU/SSE State at crash time (from exception context)
    // ========================================================================
    {
        // x87 FPU state from CONTEXT
        uint16_t crashCW = (uint16_t)(pContext->FloatSave.ControlWord);
        uint16_t crashSW = (uint16_t)(pContext->FloatSave.StatusWord);
        uint16_t crashTW = (uint16_t)(pContext->FloatSave.TagWord);

        // Decode CW bits
        const char* precision = "??";
        switch ((crashCW >> 8) & 3) {
            case 0: precision = "24bit"; break;
            case 1: precision = "??res"; break;
            case 2: precision = "53bit"; break;
            case 3: precision = "64bit"; break;
        }
        const char* rounding = "??";
        switch ((crashCW >> 10) & 3) {
            case 0: rounding = "nearest"; break;
            case 1: rounding = "down"; break;
            case 2: rounding = "up"; break;
            case 3: rounding = "truncate"; break;
        }

        // Exception mask decode
        bool imMask = (crashCW & 0x01) != 0;
        bool dmMask = (crashCW & 0x02) != 0;
        bool zmMask = (crashCW & 0x04) != 0;
        bool omMask = (crashCW & 0x08) != 0;
        bool umMask = (crashCW & 0x10) != 0;
        bool pmMask = (crashCW & 0x20) != 0;

        // SW decode
        uint8_t swTop = (crashSW >> 11) & 7;
        bool swStackFault = (crashSW & 0x40) != 0;
        bool swErrorSum = (crashSW & 0x80) != 0;
        uint8_t swExcFlags = crashSW & 0x3F;

        len += snprintf(msg + len, sizeof(msg) - len,
            "\nx87 FPU State:\n"
            "  Control Word: 0x%04X [precision=%s rounding=%s]\n"
            "  Exception Masks: IM=%d DM=%d ZM=%d OM=%d UM=%d PM=%d %s\n"
            "  Status Word:  0x%04X [top=%d exc=0x%02X stackFault=%d errorSum=%d]\n"
            "  Tag Word:     0x%04X\n",
            crashCW, precision, rounding,
            imMask, dmMask, zmMask, omMask, umMask, pmMask,
            ((crashCW & 0x3F) != 0x3F) ? "<-- UNMASKED EXCEPTIONS!" : "(all masked)",
            crashSW, swTop, swExcFlags, swStackFault, swErrorSum,
            crashTW);

        // Status word exception flags breakdown
        if (swExcFlags) {
            len += snprintf(msg + len, sizeof(msg) - len,
                "  Active SW exceptions: %s%s%s%s%s%s\n",
                (swExcFlags & 0x01) ? "INVALID " : "",
                (swExcFlags & 0x02) ? "DENORMAL " : "",
                (swExcFlags & 0x04) ? "ZERODIV " : "",
                (swExcFlags & 0x08) ? "OVERFLOW " : "",
                (swExcFlags & 0x10) ? "UNDERFLOW " : "",
                (swExcFlags & 0x20) ? "PRECISION " : "");
        }

        // Dump x87 register stack (80-bit extended precision values)
        len += snprintf(msg + len, sizeof(msg) - len, "  FPU Register Stack:\n");
        for (int r = 0; r < 8 && len < sizeof(msg) - 200; r++) {
            uint8_t* reg = &pContext->FloatSave.RegisterArea[r * 10];
            // Tag word: 2 bits per register (0=valid, 1=zero, 2=special, 3=empty)
            int tagBits = (crashTW >> (r * 2)) & 3;
            const char* tagStr = "??";
            switch (tagBits) {
                case 0: tagStr = "valid"; break;
                case 1: tagStr = "zero"; break;
                case 2: tagStr = "special"; break;
                case 3: tagStr = "empty"; break;
            }
            len += snprintf(msg + len, sizeof(msg) - len,
                "    ST(%d) [%s]: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
                r, tagStr,
                reg[9], reg[8], reg[7], reg[6], reg[5],
                reg[4], reg[3], reg[2], reg[1], reg[0]);
        }

        // MXCSR (SSE control/status) - capture live value since CONTEXT doesn't always have it
        uint32_t liveMXCSR = 0;
        __try {
            __asm { stmxcsr dword ptr [liveMXCSR] }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            liveMXCSR = 0xDEAD;
        }

        if (liveMXCSR != 0xDEAD) {
            const char* sseRound = "??";
            switch ((liveMXCSR >> 13) & 3) {
                case 0: sseRound = "nearest"; break;
                case 1: sseRound = "down"; break;
                case 2: sseRound = "up"; break;
                case 3: sseRound = "truncate"; break;
            }
            bool sseFtz = (liveMXCSR & 0x8000) != 0;
            uint8_t sseMasks = (liveMXCSR >> 7) & 0x3F;
            uint8_t sseStatus = liveMXCSR & 0x3F;

            len += snprintf(msg + len, sizeof(msg) - len,
                "\nSSE State (MXCSR):\n"
                "  MXCSR: 0x%08X [rounding=%s ftz=%d]\n"
                "  Exception Masks: 0x%02X %s\n"
                "  Status Flags:    0x%02X %s%s%s%s%s%s\n",
                liveMXCSR, sseRound, sseFtz,
                sseMasks, (sseMasks == 0x3F) ? "(all masked)" : "<-- UNMASKED!",
                sseStatus,
                (sseStatus & 0x01) ? "INVALID " : "",
                (sseStatus & 0x02) ? "DENORMAL " : "",
                (sseStatus & 0x04) ? "ZERODIV " : "",
                (sseStatus & 0x08) ? "OVERFLOW " : "",
                (sseStatus & 0x10) ? "UNDERFLOW " : "",
                (sseStatus & 0x20) ? "PRECISION " : "");
        }

        // Try to get RNG/FPU diagnostic state from mod DLL
        if (g_hModDLL) {
            __try {
                auto pGetRngStats = (ModGetRngStats_t)GetProcAddress(g_hModDLL, "AS2_GetRngStats");
                auto pGetFpuSaveCount = (ModGetFpuCount_t)GetProcAddress(g_hModDLL, "AS2_GetFpuSaveCount");
                auto pGetFpuRestoreCount = (ModGetFpuCount_t)GetProcAddress(g_hModDLL, "AS2_GetFpuRestoreCount");
                auto pGetFpuAnomalyCount = (ModGetFpuCount_t)GetProcAddress(g_hModDLL, "AS2_GetFpuAnomalyCount");

                if (pGetRngStats) {
                    uint32_t simSeed = 0, visSeed = 0, simCalls = 0, visCalls = 0;
                    pGetRngStats(&simSeed, &visSeed, &simCalls, &visCalls);
                    len += snprintf(msg + len, sizeof(msg) - len,
                        "\nMod RNG State:\n"
                        "  SimSeed: 0x%08X  VisSeed: 0x%08X\n"
                        "  SimCalls: %u  VisCalls: %u\n",
                        simSeed, visSeed, simCalls, visCalls);
                    if (simSeed == 0) {
                        len += snprintf(msg + len, sizeof(msg) - len,
                            "  *** WARNING: SimSeed is ZERO - possible uninitialized state!\n");
                    }
                }

                if (pGetFpuSaveCount && pGetFpuRestoreCount && pGetFpuAnomalyCount) {
                    uint32_t saves = pGetFpuSaveCount();
                    uint32_t restores = pGetFpuRestoreCount();
                    uint32_t anomalies = pGetFpuAnomalyCount();
                    len += snprintf(msg + len, sizeof(msg) - len,
                        "\nMod FPU Stats:\n"
                        "  Saves: %u  Restores: %u  Anomalies: %u\n",
                        saves, restores, anomalies);
                    if (anomalies > 0) {
                        len += snprintf(msg + len, sizeof(msg) - len,
                            "  *** %u FPU ANOMALIES detected before crash! Check as2_rollback.log\n",
                            anomalies);
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                len += snprintf(msg + len, sizeof(msg) - len,
                    "\nMod diagnostic functions: <failed to call>\n");
            }
        }

        // Try to read game state from known memory addresses
        __try {
            uint32_t gameFrame = *(volatile uint32_t*)0x816490;  // ADDR_SIM_FRAME_COUNTER
            uint32_t gameMode = *(volatile uint32_t*)0x81638C;   // ADDR_GAME_MODE
            uint32_t subState = *(volatile uint32_t*)0x816390;   // ADDR_SUB_STATE

            len += snprintf(msg + len, sizeof(msg) - len,
                "\nGame State at Crash:\n"
                "  SimFrame: %u  GameMode: %u  SubState: %u\n",
                gameFrame, gameMode, subState);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            len += snprintf(msg + len, sizeof(msg) - len,
                "\nGame State at Crash: <failed to read game memory>\n");
        }

        // Determine if this is an FPU-related crash
        bool isFpuCrash = false;
        switch (pRecord->ExceptionCode) {
            case EXCEPTION_FLT_DENORMAL_OPERAND:
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            case EXCEPTION_FLT_INEXACT_RESULT:
            case EXCEPTION_FLT_INVALID_OPERATION:
            case EXCEPTION_FLT_OVERFLOW:
            case EXCEPTION_FLT_STACK_CHECK:
            case EXCEPTION_FLT_UNDERFLOW:
            case 0xC00002B4:  // STATUS_FLOAT_MULTIPLE_FAULTS
            case 0xC00002B5:  // STATUS_FLOAT_MULTIPLE_TRAPS
                isFpuCrash = true;
                break;
        }
        if (isFpuCrash) {
            len += snprintf(msg + len, sizeof(msg) - len,
                "\n*** THIS IS AN FPU-RELATED CRASH ***\n"
                "Root cause analysis:\n"
                "  - If exception masks are unset (0 instead of 1): fldcw restored unmasked CW\n"
                "  - If stack fault: FSAVE/FRSTOR corrupted x87 register stack\n"
                "  - If exception in GDI32/DXLib: restored CW exposed latent float issues\n"
                "  - Check as2_rollback.log for [FPU][ANOMALY] entries before this crash\n");
        }
    }
    
    // Try to get module info for crash address
    HMODULE hMod = nullptr;
    char modName[MAX_PATH] = "Unknown";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pRecord->ExceptionAddress, &hMod)) {
        GetModuleFileNameA(hMod, modName, MAX_PATH);
        DWORD_PTR modBase = (DWORD_PTR)hMod;
        DWORD_PTR offset = (DWORD_PTR)pRecord->ExceptionAddress - modBase;
        len += snprintf(msg + len, sizeof(msg) - len,
            "\nCrash Module: %s\n"
            "Module Base: 0x%p\n"
            "Offset in Module: 0x%08X\n",
            modName, hMod, (unsigned int)offset);
    }
    
    // ========================================================================
    // Enhanced diagnostics: EIP region, instruction bytes, raw stack dump
    // ========================================================================
    {
        // Classify EIP region
        DWORD eip = pContext->Eip;
        DWORD esp = pContext->Esp;
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        DWORD stackBase = (DWORD)tib->StackBase;
        DWORD stackLimit = (DWORD)tib->StackLimit;

        const char* eipRegion = "UNKNOWN";
        if (eip >= stackLimit && eip < stackBase)
            eipRegion = "*** STACK (corrupted control flow!) ***";
        else {
            HMODULE hEipMod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)eip, &hEipMod))
                eipRegion = "CODE (module)";
            else
                eipRegion = "HEAP/OTHER (possible vtable corruption)";
        }
        len += snprintf(msg + len, sizeof(msg) - len,
            "\nEIP Region: %s\n"
            "Thread Stack: 0x%08X - 0x%08X (limit - base)\n",
            eipRegion, stackLimit, stackBase);

        // Dump bytes at EIP (the "instruction" the CPU tried to execute)
        len += snprintf(msg + len, sizeof(msg) - len, "\nBytes at EIP (0x%08X):", eip);
        __try {
            unsigned char* p = (unsigned char*)eip;
            for (int b = 0; b < 16 && len < sizeof(msg) - 10; b++) {
                len += snprintf(msg + len, sizeof(msg) - len, " %02X", p[b]);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            len += snprintf(msg + len, sizeof(msg) - len, " <UNREADABLE>");
        }
        len += snprintf(msg + len, sizeof(msg) - len, "\n");

        // Raw stack hex dump (16 DWORDs from ESP upward)
        len += snprintf(msg + len, sizeof(msg) - len, "\nRaw Stack Dump (ESP=0x%08X, 16 DWORDs):\n", esp);
        __try {
            DWORD* sp = (DWORD*)esp;
            for (int row = 0; row < 4 && len < sizeof(msg) - 80; row++) {
                len += snprintf(msg + len, sizeof(msg) - len, "  +%02X:", row * 16);
                for (int col = 0; col < 4; col++) {
                    len += snprintf(msg + len, sizeof(msg) - len, " %08X", sp[row * 4 + col]);
                }
                len += snprintf(msg + len, sizeof(msg) - len, "\n");
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            len += snprintf(msg + len, sizeof(msg) - len, "  <UNREADABLE>\n");
        }

        // Entity state dump — entity animation indices and key offsets
        // Entity base addresses from as2_constants.h
        len += snprintf(msg + len, sizeof(msg) - len, "\nEntity State Snapshot:\n");
        const DWORD p1Entity = 0x776668;  // ADDR_P1_ENTITY_BASE
        const DWORD p2Entity = 0x790F74;  // ADDR_P2_ENTITY_BASE
        for (int p = 0; p < 2; p++) {
            DWORD entBase = (p == 0) ? p1Entity : p2Entity;
            len += snprintf(msg + len, sizeof(msg) - len, "  P%d (0x%08X):", p + 1, entBase);
            __try {
                BYTE  owner      = *(BYTE*)(entBase + 0x0000);
                DWORD actionId   = *(DWORD*)(entBase + 0x044C);
                DWORD mainSprite = *(DWORD*)(entBase + 0x0818);
                DWORD renderGrp  = *(DWORD*)(entBase + 0x081C);
                DWORD overlay    = *(DWORD*)(entBase + 0x0820);
                BYTE  ovOrder    = *(BYTE*)(entBase + 0x0824);
                short ovX        = *(short*)(entBase + 0x0826);
                short ovY        = *(short*)(entBase + 0x0828);
                DWORD ovBlend    = *(DWORD*)(entBase + 0x082C);
                BYTE  ovAlpha    = *(BYTE*)(entBase + 0x0830);
                DWORD animIdx    = *(DWORD*)(entBase + 0x1004);
                DWORD animFrames = *(DWORD*)(entBase + 0x100C);
                BYTE  atkState   = *(BYTE*)(entBase + 0x6C8);
                DWORD atkType    = *(DWORD*)(entBase + 0x6CC);
                BYTE  hitActive  = *(BYTE*)(entBase + 0x6D4);
                short posX       = *(short*)(entBase + 0xB8);
                short posY       = *(short*)(entBase + 0xBA);
                len += snprintf(msg + len, sizeof(msg) - len,
                    " owner=%u action=%u main=%u group=%u overlay=0x%08X order=%u "
                    "ovPos=(%d,%d) blend=0x%08X alpha=%u animIdx=%u/%u "
                    "atk=%d type=0x%X hit=%d pos=(%d,%d)\n",
                    owner,
                    actionId,
                    mainSprite,
                    renderGrp,
                    overlay,
                    ovOrder,
                    (int)ovX,
                    (int)ovY,
                    ovBlend,
                    ovAlpha,
                    animIdx, animFrames, atkState, atkType, hitActive,
                    (int)posX, (int)posY);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                len += snprintf(msg + len, sizeof(msg) - len, " <UNREADABLE>\n");
            }
        }
    }

    // Simple stack walk (no symbols, just addresses)
    len += snprintf(msg + len, sizeof(msg) - len, "\nStack Trace (addresses only):\n");
    DWORD* pStack = (DWORD*)pContext->Esp;
    for (int i = 0; i < 20 && len < sizeof(msg) - 100; i++) {
        __try {
            DWORD addr = pStack[i];
            // Check if it looks like a code address (in loaded module range)
            HMODULE hStackMod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &hStackMod)) {
                char stackModName[MAX_PATH];
                GetModuleFileNameA(hStackMod, stackModName, MAX_PATH);
                char* lastSlash = strrchr(stackModName, '\\');
                DWORD_PTR stackOffset = addr - (DWORD_PTR)hStackMod;
                const char* knownStackAddress = DescribeKnownGameAddress(addr);
                if (knownStackAddress) {
                    len += snprintf(msg + len, sizeof(msg) - len,
                        "  [%02d] 0x%08X (%s+0x%X) %s\n",
                        i, addr, lastSlash ? lastSlash + 1 : stackModName, (unsigned int)stackOffset, knownStackAddress);
                } else {
                    len += snprintf(msg + len, sizeof(msg) - len,
                        "  [%02d] 0x%08X (%s+0x%X)\n",
                        i, addr, lastSlash ? lastSlash + 1 : stackModName, (unsigned int)stackOffset);
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Can't read this stack location, stop
            break;
        }
    }
    
    len += snprintf(msg + len, sizeof(msg) - len, "========================================\n");
    
    // Write to file
    if (crashFile) {
        fprintf(crashFile, "%s", msg);
        fflush(crashFile);
        fclose(crashFile);
    }
    
    // Also write to console if available
    if (g_hConsole != INVALID_HANDLE_VALUE) {
        printf("\033[91m%s\033[0m", msg);  // Red color
    }
    
    // Also write to proxy log
    if (g_logFile) {
        fprintf(g_logFile, "%s", msg);
        fflush(g_logFile);
    }
    
    // Notify mod about the crash BEFORE showing message box
    // This allows cleanup of network connections, etc.
    if (g_pModOnGameExit && !g_gameExiting) {
        g_gameExiting = true;
        char crashReason[256];
        snprintf(crashReason, sizeof(crashReason), "CRASH: %s at 0x%p",
                 GetExceptionCodeName(pRecord->ExceptionCode), pRecord->ExceptionAddress);
        g_pModOnGameExit(-1, crashReason);
    }
    
    // Show message box with crash info
    char mbMsg[512];
    snprintf(mbMsg, sizeof(mbMsg),
        "Game crashed!\n\n"
        "Exception: %s (0x%08X)\n"
        "Address: 0x%p\n"
        "Module: %s\n\n"
        "Details saved to: crash_log.txt",
        GetExceptionCodeName(pRecord->ExceptionCode),
        pRecord->ExceptionCode,
        pRecord->ExceptionAddress,
        strrchr(modName, '\\') ? strrchr(modName, '\\') + 1 : modName);
    
    MessageBoxA(nullptr, mbMsg, "Alice Senki 2 - Crash", MB_OK | MB_ICONERROR);
    
    // Call previous handler if any
    if (g_previousExceptionFilter) {
        return g_previousExceptionFilter(pExceptionInfo);
    }
    
    return EXCEPTION_EXECUTE_HANDLER;
}

static int ProxyLogExceptionFilter(const char* where, EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) {
        ProxyLog("[SEH] EXCEPTION in %s (no exception record)", where ? where : "<unknown>");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    PEXCEPTION_RECORD record = info->ExceptionRecord;
    ProxyLog("[SEH] EXCEPTION in %s: code=0x%08lX (%s) address=0x%p params=%lu",
             where ? where : "<unknown>",
             record->ExceptionCode,
             GetExceptionCodeName(record->ExceptionCode),
             record->ExceptionAddress,
             record->NumberParameters);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const char* accessType = (record->ExceptionInformation[0] == 0) ? "READ" :
                                 (record->ExceptionInformation[0] == 1) ? "WRITE" : "EXECUTE";
        ProxyLog("[SEH]   access=%s target=0x%p", accessType, (void*)record->ExceptionInformation[1]);
    }
    ProxyLogModuleSnapshot("exception filter");
    if (g_logFile) {
        fflush(g_logFile);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InvokeModOnFrameSafely() {
    if (!g_pModOnFrame) {
        return;
    }

    __try {
        g_pModOnFrame();
    } __except(ProxyLogExceptionFilter("g_pModOnFrame", GetExceptionInformation())) {
    }
}

static void InvokeModOnPresentSafely(IDirect3DDevice9* device) {
    if (!g_pModOnPresent) {
        return;
    }

    __try {
        g_pModOnPresent(device);
    } __except(ProxyLogExceptionFilter("g_pModOnPresent", GetExceptionInformation())) {
    }
}

// ============================================================================
// Console Window Setup
// ============================================================================

void InitConsole() {
    if (g_consoleAllocated) return;
    
    if (AllocConsole()) {
        g_consoleAllocated = true;
        g_consoleVisible = false;
        SetConsoleTitleA("Alice Senki 2 - Improvement Mod Debug Console");
        g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        
        // Enable ANSI colors (Windows 10+)
        DWORD consoleMode = 0;
        GetConsoleMode(g_hConsole, &consoleMode);
        SetConsoleMode(g_hConsole, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        
        // Resize console
        SMALL_RECT windowSize = {0, 0, 119, 39};
        SetConsoleWindowInfo(g_hConsole, TRUE, &windowSize);
        
        // Redirect stdout/stderr
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        
        printf("\033[36m");
        printf("========================================\n");
        printf("  Alice Senki 2 - Improvement Mod 0.7-beta2.01\n");
        printf("  Debug Console\n");
        printf("========================================\n");
        printf("\033[0m\n");

        // Hide console by default — logging continues in background
        HWND consoleWnd = GetConsoleWindow();
        if (consoleWnd) {
            ShowWindow(consoleWnd, SW_HIDE);
        }
    }
}

void ShutdownConsole(bool logMessage) {
    if (!g_consoleAllocated) {
        return;
    }

    if (logMessage) {
        ProxyLog("[CONSOLE] Closing debug console");
    }

    fflush(stdout);
    fflush(stderr);

    FILE* fp = nullptr;
    freopen_s(&fp, "NUL", "w", stdout);
    freopen_s(&fp, "NUL", "w", stderr);

    FreeConsole();
    g_hConsole = INVALID_HANDLE_VALUE;
    g_consoleAllocated = false;
}

void ToggleConsole() {
    if (!g_consoleAllocated) {
        InitConsole();
    }
    if (!g_consoleAllocated) return;

    HWND consoleWnd = GetConsoleWindow();
    if (!consoleWnd) return;

    g_consoleVisible = !g_consoleVisible;
    ShowWindow(consoleWnd, g_consoleVisible ? SW_SHOW : SW_HIDE);
    if (g_consoleVisible) {
        SetForegroundWindow(consoleWnd);
    }
    ProxyLog("[CONSOLE] Debug console %s", g_consoleVisible ? "shown" : "hidden");
}

// ============================================================================
// Logging (to both file and console)
// ============================================================================

void ProxyLog(const char* fmt, ...) {
    static const unsigned int kProxyLogFlushEveryLines = 64;

    // Open log file if not open
    if (!g_logFile) {
        // Create dated log folder: logs/<YYYY-MM-DD_HH-MM-SS>/
        SYSTEMTIME st;
        GetLocalTime(&st);
        char logsBase[MAX_PATH] = {};
        snprintf(logsBase, MAX_PATH, "%s\\logs", g_dllDir);

        if (g_dllDirW[0]) {
            wchar_t logsBaseW[MAX_PATH] = {};
            swprintf_s(logsBaseW, MAX_PATH, L"%s\\logs", g_dllDirW);
            CreateDirectoryW(logsBaseW, nullptr);
            swprintf_s(g_logDirW, MAX_PATH, L"%s\\%04d-%02d-%02d_%02d-%02d-%02d",
                       logsBaseW, st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond);
            CreateDirectoryW(g_logDirW, nullptr);

            BOOL usedDefault = FALSE;
            WideToAnsi(CP_ACP, g_logDirW, g_logDir, MAX_PATH, &usedDefault);

            wchar_t logPathW[MAX_PATH] = {};
            swprintf_s(logPathW, MAX_PATH, L"%s\\d3d9_proxy_%lu.log", g_logDirW, GetCurrentProcessId());
            _wfopen_s(&g_logFile, logPathW, L"w");
        }

        if (!g_logFile) {
            CreateDirectoryA(logsBase, nullptr);
            snprintf(g_logDir, MAX_PATH, "%s\\%04d-%02d-%02d_%02d-%02d-%02d",
                     logsBase, st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond);
            CreateDirectoryA(g_logDir, nullptr);
            char logPath[MAX_PATH];
            snprintf(logPath, MAX_PATH, "%s\\d3d9_proxy_%lu.log", g_logDir, GetCurrentProcessId());
            g_logFile = fopen(logPath, "w");
        }
        if (g_logFile) {
            setvbuf(g_logFile, nullptr, _IOFBF, 256 * 1024);
            g_logLinesSinceFlush = 0;
        }
    }
    
    // Get timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "[%02d:%02d:%02d.%03d]", 
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    
    // Format the message
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    // Write to log file
    if (g_logFile) {
        const DWORD elapsedMs = g_processAttachTick ? (GetTickCount() - g_processAttachTick) : 0;
        fprintf(g_logFile, "%s +%06lums #%05u %s\n",
                timestamp, elapsedMs, ++g_proxyLogSequence, buffer);
        g_logLinesSinceFlush++;

        const bool criticalLog =
            strstr(buffer, "ERROR") ||
            strstr(buffer, "FAILED") ||
            strstr(buffer, "FATAL") ||
            strstr(buffer, "CRASH") ||
            strstr(buffer, "EXCEPTION");

        if (g_proxyFlushEveryLine || criticalLog || g_logLinesSinceFlush >= kProxyLogFlushEveryLines) {
            fflush(g_logFile);
            g_logLinesSinceFlush = 0;
        }
    }
    
    // Write to console with colors
    if (g_consoleAllocated && g_hConsole != INVALID_HANDLE_VALUE) {
        printf("\033[90m%s\033[0m ", timestamp);
        
        if (strstr(buffer, "ERROR") || strstr(buffer, "FAILED") || strstr(buffer, "FATAL")) {
            printf("\033[91m%s\033[0m\n", buffer);
        } else if (strstr(buffer, "WARNING") || strstr(buffer, "WARN")) {
            printf("\033[93m%s\033[0m\n", buffer);
        } else if (strstr(buffer, "SUCCESS") || strstr(buffer, "OK") || strstr(buffer, "success") || 
                   strstr(buffer, "loaded") || strstr(buffer, "initialized") || strstr(buffer, "hooked")) {
            printf("\033[92m%s\033[0m\n", buffer);
        } else {
            printf("%s\n", buffer);
        }
    }
}

// Helper to log window info
void LogWindowInfo(HWND hWnd, const char* prefix) {
    if (!hWnd) {
        ProxyLog("%s Window handle is NULL", prefix);
        return;
    }
    
    RECT rcClient, rcWindow;
    GetClientRect(hWnd, &rcClient);
    GetWindowRect(hWnd, &rcWindow);
    
    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    LONG exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
    
    char className[256];
    GetClassNameA(hWnd, className, sizeof(className));
    
    ProxyLog("%s Window 0x%p ('%s')", prefix, hWnd, className);
    ProxyLog("%s   Client: %dx%d at (%d,%d)", prefix, 
             rcClient.right - rcClient.left, rcClient.bottom - rcClient.top,
             rcClient.left, rcClient.top);
    ProxyLog("%s   Window: %dx%d at (%d,%d)", prefix,
             rcWindow.right - rcWindow.left, rcWindow.bottom - rcWindow.top,
             rcWindow.left, rcWindow.top);
    ProxyLog("%s   Style: 0x%08X, ExStyle: 0x%08X", prefix, style, exStyle);
    
    // Decode some common styles
    if (style & WS_POPUP) ProxyLog("%s   - WS_POPUP", prefix);
    if (style & WS_OVERLAPPEDWINDOW) ProxyLog("%s   - WS_OVERLAPPEDWINDOW", prefix);
    if (style & WS_VISIBLE) ProxyLog("%s   - WS_VISIBLE", prefix);
    if (exStyle & WS_EX_APPWINDOW) ProxyLog("%s   - WS_EX_APPWINDOW", prefix);
    if (exStyle & WS_EX_TOPMOST) ProxyLog("%s   - WS_EX_TOPMOST", prefix);
}

// Helper to log surface info
void LogSurfaceInfo(IDirect3DSurface9* pSurface, const char* prefix) {
    if (!pSurface) {
        ProxyLog("%s Surface is NULL", prefix);
        return;
    }
    
    D3DSURFACE_DESC desc;
    if (SUCCEEDED(pSurface->GetDesc(&desc))) {
        ProxyLog("%s Surface 0x%p: %ux%u, Format=%d, Usage=0x%X, Pool=%d", 
                 prefix, pSurface, desc.Width, desc.Height, desc.Format, desc.Usage, desc.Pool);
    } else {
        ProxyLog("%s Surface 0x%p: GetDesc failed", prefix, pSurface);
    }
}

// ============================================================================
// Letterbox Rendering Support
// ============================================================================

// Calculate viewport and destination rect for 4:3 aspect ratio centered in screen
void CalculateLetterboxViewport(int screenW, int screenH, int nativeW, int nativeH, 
                                 D3DVIEWPORT9* vp, RECT* destRect) {
    ProxyLog("[LETTERBOX] CalculateLetterboxViewport called");
    ProxyLog("[LETTERBOX]   Screen: %dx%d", screenW, screenH);
    ProxyLog("[LETTERBOX]   Native: %dx%d", nativeW, nativeH);
    
    float targetAspect = (float)nativeW / (float)nativeH;  // 4:3 = 1.333...
    float screenAspect = (float)screenW / (float)screenH;
    
    ProxyLog("[LETTERBOX]   Target aspect: %.4f, Screen aspect: %.4f", targetAspect, screenAspect);
    
    int viewportW, viewportH;
    int viewportX, viewportY;
    
    if (screenAspect > targetAspect) {
        // Screen is wider than 4:3 - add black bars on sides (pillarbox)
        viewportH = screenH;
        viewportW = (int)(screenH * targetAspect);
        viewportX = (screenW - viewportW) / 2;
        viewportY = 0;
        ProxyLog("[LETTERBOX]   Mode: PILLARBOX (bars on sides)");
    } else {
        // Screen is taller than 4:3 - add black bars on top/bottom (letterbox)
        viewportW = screenW;
        viewportH = (int)(screenW / targetAspect);
        viewportX = 0;
        viewportY = (screenH - viewportH) / 2;
        ProxyLog("[LETTERBOX]   Mode: LETTERBOX (bars on top/bottom)");
    }
    
    // Set viewport
    if (vp) {
        vp->X = viewportX;
        vp->Y = viewportY;
        vp->Width = viewportW;
        vp->Height = viewportH;
        vp->MinZ = 0.0f;
        vp->MaxZ = 1.0f;
    }
    
    // Set destination rect for StretchRect
    if (destRect) {
        destRect->left = viewportX;
        destRect->top = viewportY;
        destRect->right = viewportX + viewportW;
        destRect->bottom = viewportY + viewportH;
    }
    
    ProxyLog("[LETTERBOX]   Result: Viewport %dx%d at (%d,%d)", 
             viewportW, viewportH, viewportX, viewportY);
}

// Forward declarations
void ReleaseLetterboxing();
void ReleaseScalingSwapChain();
static void RefreshCachedGameBackBuffer(IDirect3DDevice9* pDevice);
static void ReleaseCachedGameBackBuffer();

// ============================================================================
// Scaling Swap Chain - creates a larger swap chain for the borderless window
// ============================================================================
// The game's backbuffer is 640x480. We create a second swap chain at window size
// (e.g., 1920x1440) and StretchRect from game backbuffer to this chain before presenting.

// ============================================================================
// SCALING SWAP CHAIN APPROACH
// ============================================================================
// Problem: DXLib caches backbuffer dimensions internally at device creation.
//          If we resize the backbuffer, DXLib still renders at original coords.
//          Result: Game renders in top-left corner of larger backbuffer.
//
// Solution: Secondary swap chain approach:
//   1. Keep the game's backbuffer at native 640x480 (DXLib happy)
//   2. Create a SECOND swap chain at window size (1920x1440)
//   3. In Present hook:
//      a. StretchRect from 640x480 game backbuffer to 1920x1440 scaling backbuffer
//      b. Present the scaling swap chain (this fills the window!)
//      c. Skip original Present (would show 640x480 in corner)
//
// Surfaces involved:
//   - Game backbuffer: 640x480, owned by device's implicit swap chain
//   - Scaling backbuffer: 1920x1440, owned by our additional swap chain
//   - Window: 1920x1440 borderless (or whatever aspect-correct size fits monitor)
// ============================================================================

bool InitializeScalingSwapChain(IDirect3DDevice9* pDevice, HWND hWnd) {
    ProxyLog("[SCALING] ============================================");
    ProxyLog("[SCALING] InitializeScalingSwapChain called");
    ProxyLog("[SCALING] ============================================");
    ProxyLog("[SCALING]   Device: 0x%p", pDevice);
    ProxyLog("[SCALING]   Window: 0x%p", hWnd);
    ProxyLog("[SCALING]   Target size: %dx%d (window/scaling chain)", g_screenWidth, g_screenHeight);
    ProxyLog("[SCALING]   Native size: %dx%d (game backbuffer)", g_nativeWidth, g_nativeHeight);
    
    // Release existing resources
    ReleaseScalingSwapChain();
    
    if (g_screenWidth <= g_nativeWidth || g_screenHeight <= g_nativeHeight) {
        ProxyLog("[SCALING] ERROR: Screen size (%dx%d) not larger than native (%dx%d)",
                 g_screenWidth, g_screenHeight, g_nativeWidth, g_nativeHeight);
        ProxyLog("[SCALING] Scaling not needed or invalid dimensions");
        return false;
    }
    
    // Get the backbuffer format from the device
    ProxyLog("[SCALING] Querying device backbuffer...");
    IDirect3DSurface9* pBackBuffer = nullptr;
    D3DSURFACE_DESC bbDesc;
    HRESULT hr = pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
    if (FAILED(hr)) {
        ProxyLog("[SCALING] ERROR: GetBackBuffer failed: 0x%08X", hr);
        return false;
    }
    pBackBuffer->GetDesc(&bbDesc);
    pBackBuffer->Release();
    
    ProxyLog("[SCALING] GAME BACKBUFFER (source for StretchRect):");
    ProxyLog("[SCALING]   Size: %dx%d", bbDesc.Width, bbDesc.Height);
    ProxyLog("[SCALING]   Format: %d", bbDesc.Format);
    ProxyLog("[SCALING]   Type: %d", bbDesc.Type);
    ProxyLog("[SCALING]   Usage: 0x%08X", bbDesc.Usage);
    ProxyLog("[SCALING]   Pool: %d", bbDesc.Pool);
    ProxyLog("[SCALING]   MultiSampleType: %d", bbDesc.MultiSampleType);
    
    // Create a swap chain at the window size
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = g_screenWidth;
    pp.BackBufferHeight = g_screenHeight;
    pp.BackBufferFormat = bbDesc.Format;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.MultiSampleQuality = 0;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hWnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.Flags = 0;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = GetConfiguredScalingPresentInterval();
    
    ProxyLog("[SCALING] Creating additional swap chain...");
    ProxyLog("[SCALING] CreateAdditionalSwapChain params:");
    ProxyLog("[SCALING]   BackBufferWidth: %d", pp.BackBufferWidth);
    ProxyLog("[SCALING]   BackBufferHeight: %d", pp.BackBufferHeight);
    ProxyLog("[SCALING]   BackBufferFormat: %d", pp.BackBufferFormat);
    ProxyLog("[SCALING]   BackBufferCount: %d", pp.BackBufferCount);
    ProxyLog("[SCALING]   SwapEffect: %d", pp.SwapEffect);
    ProxyLog("[SCALING]   hDeviceWindow: 0x%p", pp.hDeviceWindow);
    ProxyLog("[SCALING]   Windowed: %d", pp.Windowed);
    
    hr = pDevice->CreateAdditionalSwapChain(&pp, &g_pScalingSwapChain);
    if (FAILED(hr)) {
        ProxyLog("[SCALING] ERROR: CreateAdditionalSwapChain failed: 0x%08X", hr);
        // Log common error codes
        if (hr == D3DERR_INVALIDCALL) ProxyLog("[SCALING]   Error: D3DERR_INVALIDCALL - Invalid parameters");
        if (hr == D3DERR_NOTAVAILABLE) ProxyLog("[SCALING]   Error: D3DERR_NOTAVAILABLE - Not supported");
        if (hr == D3DERR_OUTOFVIDEOMEMORY) ProxyLog("[SCALING]   Error: D3DERR_OUTOFVIDEOMEMORY - Out of VRAM");
        return false;
    }
    ProxyLog("[SCALING] Additional swap chain created: 0x%p", g_pScalingSwapChain);
    
    // Get the backbuffer of the new swap chain
    hr = g_pScalingSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &g_pScalingBackBuffer);
    if (FAILED(hr)) {
        ProxyLog("[SCALING] ERROR: GetBackBuffer (scaling) failed: 0x%08X", hr);
        g_pScalingSwapChain->Release();
        g_pScalingSwapChain = nullptr;
        return false;
    }
    
    // Log scaling backbuffer details
    D3DSURFACE_DESC scalingDesc;
    g_pScalingBackBuffer->GetDesc(&scalingDesc);
    ProxyLog("[SCALING] SCALING BACKBUFFER (destination for StretchRect):");
    ProxyLog("[SCALING]   Surface: 0x%p", g_pScalingBackBuffer);
    ProxyLog("[SCALING]   Size: %dx%d", scalingDesc.Width, scalingDesc.Height);
    ProxyLog("[SCALING]   Format: %d", scalingDesc.Format);
    ProxyLog("[SCALING]   Type: %d", scalingDesc.Type);
    ProxyLog("[SCALING]   Usage: 0x%08X", scalingDesc.Usage);
    ProxyLog("[SCALING]   Pool: %d", scalingDesc.Pool);
    
    g_scalingInitialized = true;
    
    // Cache the game backbuffer for use in Present hooks (avoids per-frame GetBackBuffer)
    RefreshCachedGameBackBuffer(pDevice);
    
    // Pre-calculate the letterbox/pillarbox destination rect so Present hooks don't recompute per-frame.
    // Uses the same aspect-ratio math as CalculateLetterboxDestRect but inlined here to avoid
    // forward-declaration issues.
    {
        float srcAspect = (float)g_nativeWidth / (float)g_nativeHeight;
        float dstAspect = (float)g_screenWidth / (float)g_screenHeight;
        if (dstAspect > srcAspect) {
            int destH = g_screenHeight;
            int destW = (int)(g_screenHeight * srcAspect);
            int destX = (g_screenWidth - destW) / 2;
            g_letterboxDestRect = {destX, 0, destX + destW, destH};
        } else {
            int destW = g_screenWidth;
            int destH = (int)(g_screenWidth / srcAspect);
            int destY = (g_screenHeight - destH) / 2;
            g_letterboxDestRect = {0, destY, destW, destY + destH};
        }
        g_letterboxActive = g_keepAspectRatio;
        ProxyLog("[SCALING] Letterbox dest rect: (%d,%d)-(%d,%d)",
                 g_letterboxDestRect.left, g_letterboxDestRect.top,
                 g_letterboxDestRect.right, g_letterboxDestRect.bottom);
    }
    
    ProxyLog("[SCALING] ============================================");
    ProxyLog("[SCALING] INITIALIZATION COMPLETE!");
    ProxyLog("[SCALING] ============================================");
    ProxyLog("[SCALING] Flow per frame:");
    ProxyLog("[SCALING]   1. Game renders to: %dx%d backbuffer", g_nativeWidth, g_nativeHeight);
    ProxyLog("[SCALING]   2. StretchRect to: %dx%d scaling backbuffer", g_screenWidth, g_screenHeight);
    ProxyLog("[SCALING]   3. Present scaling chain to: %dx%d window", g_screenWidth, g_screenHeight);
    ProxyLog("[SCALING] ============================================");
    
    return true;
}

void ReleaseScalingSwapChain() {
    if (g_pScalingBackBuffer) {
        g_pScalingBackBuffer->Release();
        g_pScalingBackBuffer = nullptr;
    }
    if (g_pScalingSwapChain) {
        g_pScalingSwapChain->Release();
        g_pScalingSwapChain = nullptr;
    }
    g_scalingInitialized = false;
    ProxyLog("[SCALING] Resources released");
}

// ============================================================================
// Cached backbuffer helper
// ============================================================================

static void RefreshCachedGameBackBuffer(IDirect3DDevice9* pDevice) {
    if (g_pCachedGameBackBuffer) {
        g_pCachedGameBackBuffer->Release();
        g_pCachedGameBackBuffer = nullptr;
    }
    if (pDevice) {
        HRESULT hr = pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_pCachedGameBackBuffer);
        if (FAILED(hr)) {
            ProxyLog("[CACHE] WARNING: Failed to cache game backbuffer: 0x%08X", hr);
            g_pCachedGameBackBuffer = nullptr;
        }
    }
}

static void ReleaseCachedGameBackBuffer() {
    if (g_pCachedGameBackBuffer) {
        g_pCachedGameBackBuffer->Release();
        g_pCachedGameBackBuffer = nullptr;
    }
}

// ============================================================================
// Device Lost Recovery
// ============================================================================

// Reset-retry pacing. Recovery used to re-attempt every frame, which on a
// device that cannot be reset produced a ~20Hz storm of identical failures
// (run 2026-08-18_11-18: hundreds of "Reset failed 0x8876086C" lines while the
// game thread stalled ~1s per frame and the netplay session died).
static DWORD g_lastResetAttemptMs = 0;
static int   g_resetFailStreak    = 0;
static bool  g_resetDiagnosed     = false;
static constexpr DWORD kResetRetryBaseMs = 250;
static constexpr DWORD kResetRetryMaxMs  = 5000;

static DWORD ResetRetryDelayMs() {
    DWORD d = kResetRetryBaseMs;
    for (int i = 0; i < g_resetFailStreak && d < kResetRetryMaxMs; ++i) d *= 2;
    return d > kResetRetryMaxMs ? kResetRetryMaxMs : d;
}

static bool TryRecoverFromDeviceLost(IDirect3DDevice9* pDevice) {
    HRESULT hr = pDevice->TestCooperativeLevel();

    if (hr == D3DERR_DEVICELOST) {
        // Device is still lost — cannot do anything yet
        return false;
    }

    if (hr == D3DERR_DEVICENOTRESET) {
        const DWORD now = GetTickCount();
        if (g_resetFailStreak > 0 &&
            (now - g_lastResetAttemptMs) < ResetRetryDelayMs()) {
            return false;   // backing off; do not spam Reset or the log
        }
        g_lastResetAttemptMs = now;

        ProxyLog("[DEVICELOST] Device ready for reset — recovering...");

        // Release all D3DPOOL_DEFAULT resources before Reset
        Hud_Release();
        ReleaseScalingSwapChain();
        g_letterboxScaler.Release();
        ReleaseLetterboxing();
        ReleaseCachedGameBackBuffer();

        if (g_imguiInitialized) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }

        // Build present params for Reset
        D3DPRESENT_PARAMETERS pp;
        if (g_hasSavedPresentParams) {
            memcpy(&pp, &g_originalPresentParams, sizeof(pp));
        } else {
            // Fallback — minimal windowed params
            ZeroMemory(&pp, sizeof(pp));
            pp.BackBufferWidth = g_nativeWidth;
            pp.BackBufferHeight = g_nativeHeight;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
            pp.BackBufferCount = 1;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = g_gameWindow;
            pp.Windowed = TRUE;
        }
        pp.Windowed = TRUE;
        pp.FullScreen_RefreshRateInHz = 0;

        g_internalReset = true;
        hr = pDevice->Reset(&pp);
        g_internalReset = false;

        if (FAILED(hr)) {
            ++g_resetFailStreak;
            ProxyLog("[DEVICELOST] ERROR: Reset failed during recovery: 0x%08X "
                     "(attempt %d, next retry in %ums)",
                     hr, g_resetFailStreak, ResetRetryDelayMs());
            if (hr == D3DERR_INVALIDCALL && !g_resetDiagnosed) {
                g_resetDiagnosed = true;
                // Say what this actually means, once, instead of repeating the
                // hex forever. Every resource the MOD owns is released above --
                // HUD state block and font, both swap chains, letterbox and
                // scaler resources, the cached back buffer, and the ImGui
                // objects -- and nothing under src/ creates a D3D resource at
                // all. D3DERR_INVALIDCALL here therefore means the resources
                // still outstanding belong to the GAME, which predates any
                // expectation of device loss and never releases them. We cannot
                // release them on its behalf: it would keep using the dangling
                // pointers. So this device is not recoverable in-process.
                ProxyLog("[DEVICELOST] D3DERR_INVALIDCALL means D3DPOOL_DEFAULT "
                         "resources are still outstanding. All mod-owned resources "
                         "were released before Reset, so these belong to the game, "
                         "which has no device-loss handling. This device cannot be "
                         "recovered in-process — the game must be restarted. "
                         "Prevention is the real fix: a netplay session now holds "
                         "ES_DISPLAY_REQUIRED so the monitor timeout cannot trigger "
                         "this mid-match.");
            }
            return false;
        }

        g_resetFailStreak = 0;
        g_resetDiagnosed = false;
        ProxyLog("[DEVICELOST] Reset succeeded — recreating resources");

        // Recreate resources
        if (g_imguiInitialized) {
            ImGui_ImplDX9_CreateDeviceObjects();
        }

        RefreshCachedGameBackBuffer(pDevice);

        // Reinitialize scaling if we were in borderless mode
        if (g_isCurrentlyBorderless && g_keepAspectRatio && g_gameWindow) {
            InitializeScalingSwapChain(pDevice, g_gameWindow);
        }

        g_deviceLost = false;
        ProxyLog("[DEVICELOST] Recovery complete");
        return true;
    }

    // hr == D3D_OK — device is fine, clear the flag
    if (hr == D3D_OK) {
        g_deviceLost = false;
        return true;
    }

    ProxyLog("[DEVICELOST] Unexpected TestCooperativeLevel result: 0x%08X", hr);
    return false;
}

// Initialize letterboxing resources using the new LetterboxScaler class
// This redirects game rendering to an offscreen target, then copies it scaled to the backbuffer
bool InitializeLetterboxing(IDirect3DDevice9* pDevice) {
    ProxyLog("[LETTERBOX] InitializeLetterboxing called (LetterboxScaler approach)");
    
    // Release any existing resources first
    if (g_letterboxInitialized || g_letterboxScaler.IsInitialized()) {
        ProxyLog("[LETTERBOX] Re-initializing letterbox resources...");
        ReleaseLetterboxing();
    }
    
    // Get the game window
    D3DDEVICE_CREATION_PARAMETERS createParams;
    HRESULT hr = pDevice->GetCreationParameters(&createParams);
    if (FAILED(hr)) {
        ProxyLog("[LETTERBOX] ERROR: GetCreationParameters failed: 0x%08X", hr);
        return false;
    }
    
    HWND hWnd = createParams.hFocusWindow;
    g_gameWindow = hWnd;
    ProxyLog("[LETTERBOX] Game window: 0x%p", hWnd);
    
    // Ensure we have a valid screen size
    if (g_screenWidth == 0 || g_screenHeight == 0) {
        RECT clientRect;
        if (GetClientRect(hWnd, &clientRect)) {
            g_screenWidth = clientRect.right - clientRect.left;
            g_screenHeight = clientRect.bottom - clientRect.top;
        }
        if (g_screenWidth == 0) g_screenWidth = g_nativeWidth;
        if (g_screenHeight == 0) g_screenHeight = g_nativeHeight;
        ProxyLog("[LETTERBOX] Screen size was 0, updated to: %dx%d", g_screenWidth, g_screenHeight);
    }
    
    // Get and track the backbuffer surface for SetRenderTarget redirection
    IDirect3DSurface9* pBackBuffer = nullptr;
    hr = pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
    if (SUCCEEDED(hr) && pBackBuffer) {
        g_letterboxScaler.SetBackBuffer(pBackBuffer);
        pBackBuffer->Release();  // SetBackBuffer AddRef'd it
        ProxyLog("[LETTERBOX] Backbuffer tracked for SetRenderTarget redirection");
    } else {
        ProxyLog("[LETTERBOX] WARNING: Could not get backbuffer for tracking: 0x%08X", hr);
    }
    
    // Initialize the new scaler
    g_letterboxScaler.SetTargetSize(g_screenWidth, g_screenHeight);
    
    if (!g_letterboxScaler.Initialize(pDevice, g_nativeWidth, g_nativeHeight)) {
        ProxyLog("[LETTERBOX] ERROR: LetterboxScaler initialization failed!");
        return false;
    }
    
    g_letterboxInitialized = true;
    g_letterboxActive = true;
    
    ProxyLog("[LETTERBOX] Initialization complete (LetterboxScaler)");
    ProxyLog("[LETTERBOX]   Native: %dx%d", g_nativeWidth, g_nativeHeight);
    ProxyLog("[LETTERBOX]   Screen: %dx%d", g_screenWidth, g_screenHeight);
    
    return true;
}

void ReleaseLetterboxing() {
    ProxyLog("[LETTERBOX] ReleaseLetterboxing called");
    
    // Release new scaler resources
    g_letterboxScaler.Release();
    
    // Legacy cleanup (old resources that may still exist)
    if (g_pQuadVB) {
        g_pQuadVB->Release();
        g_pQuadVB = nullptr;
    }
    if (g_pScaleTextureSurface) {
        g_pScaleTextureSurface->Release();
        g_pScaleTextureSurface = nullptr;
    }
    if (g_pScaleTexture) {
        g_pScaleTexture->Release();
        g_pScaleTexture = nullptr;
    }
    if (g_pLetterboxBackBuffer) {
        g_pLetterboxBackBuffer->Release();
        g_pLetterboxBackBuffer = nullptr;
    }
    if (g_pLetterboxSwapChain) {
        g_pLetterboxSwapChain->Release();
        g_pLetterboxSwapChain = nullptr;
    }
    
    g_letterboxInitialized = false;
    g_letterboxActive = false;
}

// ============================================================================
// Safe Device Reset (preserves original device parameters)
// ============================================================================

// Perform the actual reset - only call this at a safe time (after Present, before BeginScene)
bool PerformDeferredReset(IDirect3DDevice9* pDevice) {
    if (!pDevice || !g_hasSavedPresentParams) {
        ProxyLog("[RESET-SAFE] Cannot reset - device=%p, hasSavedParams=%d", pDevice, g_hasSavedPresentParams);
        return false;
    }
    
    ProxyLog("[RESET-SAFE] Performing deferred reset: %dx%d, borderless=%s",
             g_pendingResetWidth, g_pendingResetHeight, g_pendingResetBorderless ? "yes" : "no");
    
    // Release all our resources before Reset
    ReleaseLetterboxing();
    ReleaseCachedGameBackBuffer();
    
    if (g_imguiInitialized) {
        ProxyLog("[RESET-SAFE] Invalidating ImGui device objects");
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    
    // Create present params based on original, but with new backbuffer size
    D3DPRESENT_PARAMETERS pp;
    memcpy(&pp, &g_originalPresentParams, sizeof(D3DPRESENT_PARAMETERS));
    
    // Modify for new size
    pp.BackBufferWidth = g_pendingResetWidth;
    pp.BackBufferHeight = g_pendingResetHeight;
    pp.Windowed = TRUE;  // Always windowed for borderless
    pp.FullScreen_RefreshRateInHz = 0;
    
    ProxyLog("[RESET-SAFE] Reset params: BB=%dx%d, Format=%d, DepthStencil=%s (Format=%d), SwapEffect=%d",
             pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
             pp.EnableAutoDepthStencil ? "YES" : "NO", pp.AutoDepthStencilFormat,
             pp.SwapEffect);
    
    // Do the reset
    g_internalReset = true;
    HRESULT hr = pDevice->Reset(&pp);
    g_internalReset = false;
    
    if (FAILED(hr)) {
        ProxyLog("[RESET-SAFE] ERROR: Reset failed: 0x%08X", hr);
        
        // Try again with a more conservative approach - use exact original params with only size changed
        memcpy(&pp, &g_originalPresentParams, sizeof(D3DPRESENT_PARAMETERS));
        pp.BackBufferWidth = g_pendingResetWidth;
        pp.BackBufferHeight = g_pendingResetHeight;
        
        ProxyLog("[RESET-SAFE] Retrying with minimal changes...");
        g_internalReset = true;
        hr = pDevice->Reset(&pp);
        g_internalReset = false;
        
        if (FAILED(hr)) {
            ProxyLog("[RESET-SAFE] ERROR: Reset retry also failed: 0x%08X", hr);
            // Try to recreate ImGui anyway
            if (g_imguiInitialized) {
                ImGui_ImplDX9_CreateDeviceObjects();
            }
            return false;
        }
    }
    
    ProxyLog("[RESET-SAFE] Reset succeeded!");
    
    // Recreate ImGui resources
    if (g_imguiInitialized) {
        ProxyLog("[RESET-SAFE] Recreating ImGui device objects");
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    
    // Refresh cached backbuffer after successful Reset
    RefreshCachedGameBackBuffer(pDevice);
    
    // Update global state
    if (g_pendingResetBorderless) {
        g_isCurrentlyBorderless = true;
        g_screenWidth = g_pendingResetWidth;
        g_screenHeight = g_pendingResetHeight;
        
        // Calculate letterbox viewport
        if (g_keepAspectRatio) {
            CalculateLetterboxViewport(g_screenWidth, g_screenHeight, g_nativeWidth, g_nativeHeight, 
                                       &g_letterboxViewport, &g_letterboxDestRect);
            // NOTE: Don't call InitializeLetterboxing - scaling swap chain will be initialized in Present
        }
    } else {
        g_isCurrentlyBorderless = false;
        g_letterboxActive = false;
    }
    
    return true;
}

// Request a deferred reset (safe to call from any context)
void RequestReset(int width, int height, bool borderless) {
    ProxyLog("[RESET-REQUEST] Requesting reset: %dx%d, borderless=%s", width, height, borderless ? "yes" : "no");
    g_pendingResetWidth = width;
    g_pendingResetHeight = height;
    g_pendingResetBorderless = borderless;
    g_pendingReset = true;
}

// ============================================================================
// Window Management (Borderless Fullscreen)
// ============================================================================

// Forward declaration
void CalculateLetterboxDestRect(int windowW, int windowH, int nativeW, int nativeH, RECT* destRect);

// Helper to apply borderless state to a window
// WindowResizer approach: Size window to maintain aspect ratio, center on monitor
void SetBorderlessState(HWND hWnd, bool enable) {
    if (!hWnd) return;

    // Always operate on the root window
    HWND hRoot = GetAncestor(hWnd, GA_ROOT);
    g_gameParentWindow = hRoot;

    ProxyLog("[MODE] %s mode", enable ? "Borderless fullscreen" : "Windowed");

    // Update global state
    g_isCurrentlyBorderless = enable;
    g_letterboxActive = enable && g_keepAspectRatio;
    
    // ALWAYS release scaling when changing mode - it will be recreated with correct size
    ReleaseScalingSwapChain();
    g_windowResizedNeedsReinit = true;  // Force reinit on next frame

    if (enable) {
        // 1. Get Monitor Info
        HMONITOR hMon = MonitorFromWindow(hRoot, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;

        // 2. Use FULL monitor size - letterboxing will handle aspect ratio
        int winW = monW;
        int winH = monH;
        int winX = monX;
        int winY = monY;
        
        ProxyLog("[MODE] Window: %dx%d at (%d,%d) (full monitor, letterboxing will handle 4:3)", winW, winH, winX, winY);
        
        g_screenWidth = winW;
        g_screenHeight = winH;

        // Precompute the active view rect (client coords) so input mapping works immediately
        // even before the first Present updates it.
        if (g_keepAspectRatio) {
            CalculateLetterboxDestRect(g_screenWidth, g_screenHeight, g_nativeWidth, g_nativeHeight, &g_letterboxDestRect);
        } else {
            g_letterboxDestRect = {0, 0, g_screenWidth, g_screenHeight};
        }

        // 3. Set Style (POPUP = No borders)
        LONG style = WS_POPUP | WS_VISIBLE;
        LONG exStyle = WS_EX_APPWINDOW;
        
        g_internalResize = true;
        
        SetWindowLong(hRoot, GWL_STYLE, style);
        SetWindowLong(hRoot, GWL_EXSTYLE, exStyle);
        SetMenu(hRoot, NULL);

        SetWindowPos(hRoot, HWND_TOPMOST, 
            winX, winY, winW, winH, 
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        if (hWnd != hRoot) {
            SetWindowPos(hWnd, NULL, 0, 0, winW, winH, SWP_NOZORDER);
        }
        
        g_internalResize = false;

    } else {
        // Windowed Mode
        LONG style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        LONG exStyle = WS_EX_APPWINDOW;
        
        g_internalResize = true;
        
        SetWindowLong(hRoot, GWL_STYLE, style);
        SetWindowLong(hRoot, GWL_EXSTYLE, exStyle);
        
        // Use saved windowed dimensions if available, otherwise native size
        int w = (g_windowedWidth >= g_nativeWidth) ? g_windowedWidth : g_nativeWidth;
        int h = (g_windowedHeight >= g_nativeHeight) ? g_windowedHeight : g_nativeHeight;
        
        RECT rc = {0, 0, w, h};
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;
        
        HMONITOR hMon = MonitorFromWindow(hRoot, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        
        int winX = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - winW) / 2;
        int winY = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top - winH) / 2;
        
        SetWindowPos(hRoot, HWND_NOTOPMOST, winX, winY, winW, winH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        
        if (hWnd != hRoot) {
            SetWindowPos(hWnd, NULL, 0, 0, w, h, SWP_NOZORDER);
        }
        
        g_internalResize = false;
        
        ReleaseLetterboxing();
        g_letterboxActive = false;
        g_screenWidth = w;
        g_screenHeight = h;

        EnableWindow(hRoot, TRUE);
        SetForegroundWindow(hRoot);
        SetFocus(hRoot);
        
        ProxyLog("[MODE] Window: %dx%d", w, h);
    }
}

// Calculate the destination rect for letterboxing in the window's client area
void CalculateLetterboxDestRect(int windowW, int windowH, int nativeW, int nativeH, RECT* destRect) {
    float targetAspect = (float)nativeW / (float)nativeH;  // 4:3 = 1.333...
    float windowAspect = (float)windowW / (float)windowH;
    
    int viewW, viewH, viewX, viewY;
    
    if (windowAspect > targetAspect) {
        // Window is wider than 4:3 - pillarbox (bars on sides)
        viewH = windowH;
        viewW = (int)(windowH * targetAspect);
        viewX = (windowW - viewW) / 2;
        viewY = 0;
    } else {
        // Window is taller than 4:3 - letterbox (bars top/bottom)
        viewW = windowW;
        viewH = (int)(windowW / targetAspect);
        viewX = 0;
        viewY = (windowH - viewH) / 2;
    }
    
    destRect->left = viewX;
    destRect->top = viewY;
    destRect->right = viewX + viewW;
    destRect->bottom = viewY + viewH;
    
    ProxyLog("[LETTERBOX] Window %dx%d -> Viewport %dx%d at (%d,%d)", 
             windowW, windowH, viewW, viewH, viewX, viewY);
}

void ToggleBorderlessFullscreen(HWND hWnd) {
    bool newState = !g_isCurrentlyBorderless;
    // Keep the "use" flag in sync so scaling initialization paths remain enabled.
    g_useBorderlessFullscreen = newState;
    SetBorderlessState(hWnd, newState);
    
    // Persist window state change
    DisplayConfig_Save();
    
    // Scaling swap chain will be (re)initialized automatically in HookedSwapChainPresent
    // when it detects g_scalingInitialized is false and borderless is enabled
}

void ApplyBorderlessFullscreen(HWND hWnd, IDirect3DDevice9* pDevice) {
    // IMPORTANT: Do NOT resize the backbuffer here!
    // DXLib caches the backbuffer dimensions internally and uses them for all rendering.
    // If we resize the backbuffer, DXLib will still render at 640x480 coordinates but the
    // backbuffer will be 1920x1440, causing the game to appear in the top-left corner.
    //
    // Instead, we:
    // 1. Set the window to borderless (1920x1440)
    // 2. Keep the backbuffer at native 640x480
    // 3. Use a secondary swap chain (initialized in Present hook) to stretch 640x480 to 1920x1440
    
    ProxyLog("[BORDERLESS] ApplyBorderlessFullscreen called - setting window only, NOT resizing backbuffer");
    
    // Set the window styles and size (window becomes 1920x1440 borderless)
    SetBorderlessState(hWnd, true);
    
    // DO NOT RESET THE DEVICE!
    // The backbuffer stays at 640x480, and we use a separate swap chain for scaling.
    // See InitializeScalingSwapChain() which is called after this.
    
    if (pDevice) {
        // Log current backbuffer for debugging
        IDirect3DSurface9* pBackBuffer = nullptr;
        if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer))) {
            D3DSURFACE_DESC desc;
            pBackBuffer->GetDesc(&desc);
            ProxyLog("[BORDERLESS] Current backbuffer: %dx%d (keeping this, NOT resizing)", desc.Width, desc.Height);
            pBackBuffer->Release();
        }
        
        // Recreate ImGui resources
        if (g_imguiInitialized) {
            ProxyLog("[BORDERLESS] Recreating ImGui device objects");
            ImGui_ImplDX9_CreateDeviceObjects();
        }
    }
}

// ============================================================================
// Window Procedure Hook (for ImGui input)
// ============================================================================

// Forward declaration for toggle function
void ToggleBorderlessFullscreen(HWND hWnd);
static bool IsShellWinLogoKey(WPARAM wParam) {
    return wParam == VK_LWIN || wParam == VK_RWIN || wParam == VK_APPS;
}

static bool IsFirstKeydown(UINT uMsg, LPARAM lParam) {
    return (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && (lParam & 0x40000000u) == 0;
}

static void LogSwallowTraceWndproc(const char* stage,
                                   UINT uMsg,
                                   WPARAM wParam,
                                   LPARAM lParam,
                                   LRESULT result,
                                   bool toGame);

static int ReadShellSuppressFlag();

// Do not feed shell/layout keys to ImGui while the overlay menu is open.
static bool IsShellKeyWndprocMessage(UINT uMsg, WPARAM wParam) {
    if (uMsg == WM_INPUTLANGCHANGEREQUEST || uMsg == WM_INPUTLANGCHANGE) {
        return true;
    }
    if (uMsg == WM_IME_SETCONTEXT || uMsg == WM_IME_NOTIFY || uMsg == WM_IME_REQUEST) {
        return true;
    }
    if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) {
        return IsShellWinLogoKey(wParam) || wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU ||
               wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT;
    }
    return false;
}

static void ReleaseGameMouseCapture(HWND gameWindow) {
    const HWND capture = GetCapture();
    if (!capture) {
        return;
    }

    if (capture == gameWindow ||
        (gameWindow && GetAncestor(capture, GA_ROOT) == gameWindow)) {
        ReleaseCapture();
        if (g_enableSwallowTraceLogs) {
            ProxyLog("[INPUTGUARD] ReleaseCapture hwnd=0x%p (game=0x%p)", capture, gameWindow);
        }
    }
}

static bool IsShellRelatedMessage(UINT uMsg, WPARAM wParam) {
    return IsShellKeyWndprocMessage(uMsg, wParam);
}

static bool IsDxLibMagicWndProc(WNDPROC proc) {
    return reinterpret_cast<uintptr_t>(proc) >= 0xFFFF0000u;
}

static LRESULT CallWindowProcCompat(WNDPROC proc, HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!proc) {
        return 0;
    }
    return IsWindowUnicode(hWnd)
        ? CallWindowProcW(proc, hWnd, msg, wParam, lParam)
        : CallWindowProcA(proc, hWnd, msg, wParam, lParam);
}

static LRESULT CallDefaultWindowProcCompat(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (IsWindowUnicode(hWnd)) {
        const LRESULT wideResult = DefWindowProcW(hWnd, msg, wParam, lParam);
        if (wideResult != 0) {
            return wideResult;
        }
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
    const LRESULT ansiResult = DefWindowProcA(hWnd, msg, wParam, lParam);
    if (ansiResult != 0) {
        return ansiResult;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Policy: mod/docs/SHELL_HOTKEY_POLICY.md — route through hooked sub_633490, never shell-intercept.
static LRESULT CallGameWndProcChain(HWND hWnd,
                                    UINT msg,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    WNDPROC priorWndProc,
                                    const char* stage,
                                    bool traceHotkeyWndproc) {
    const bool shellMsg = IsShellRelatedMessage(msg, wParam);
    const bool preferGameHook = shellMsg || (priorWndProc && IsDxLibMagicWndProc(priorWndProc));

    if (preferGameHook && g_pModCallGameWndProc) {
        LRESULT result = 0;
        if (g_pModCallGameWndProc(hWnd, msg, wParam, lParam, &result)) {
            if (traceHotkeyWndproc ||
                (g_enableSwallowTraceLogs && shellMsg)) {
                ProxyLog("[INPUTGUARD][%s-CallGameWndProc] %s result=0x%p priorWndProc=0x%p",
                         stage ? stage : "wndproc",
                         DescribeShellHotkeyTraceMessage(msg, wParam),
                         (void*)result,
                         priorWndProc);
            }
            // Keep shell messages on the game hook path first, but when it yields 0,
            // continue the prior wndproc chain so we do not bypass outer dispatch layers.
            if (!shellMsg || result != 0 || !priorWndProc || IsDxLibMagicWndProc(priorWndProc)) {
                return result;
            }

            WNDPROC chainTarget = priorWndProc;
            // When HookedWndProc forwards shell messages to ProxyWndProc and the game
            // hook already returned 0, avoid re-entering ProxyWndProc (which would call
            // ModCallGameWndProc a second time). Jump directly to ProxyWndProc's prior
            // target to keep ordering deterministic.
            if (shellMsg &&
                priorWndProc == g_imguiOriginalWndProc &&
                g_proxyOriginalWndProc &&
                g_imguiOriginalWndProc &&
                g_imguiOriginalWndProc != g_proxyOriginalWndProc) {
                chainTarget = g_proxyOriginalWndProc;
                if (traceHotkeyWndproc || g_enableSwallowTraceLogs) {
                    ProxyLog("[INPUTGUARD][%s-CallGameWndProc-direct] %s skip_proxy=1 priorWndProc=0x%p proxyOriginal=0x%p",
                             stage ? stage : "wndproc",
                             DescribeShellHotkeyTraceMessage(msg, wParam),
                             priorWndProc,
                             g_proxyOriginalWndProc);
                }
            }

            const LRESULT chainedResult = CallWindowProcCompat(chainTarget, hWnd, msg, wParam, lParam);
            if (traceHotkeyWndproc || g_enableSwallowTraceLogs) {
                ProxyLog("[INPUTGUARD][%s-CallGameWndProc-fallback] %s game=0x%p chained=0x%p priorWndProc=0x%p",
                         stage ? stage : "wndproc",
                         DescribeShellHotkeyTraceMessage(msg, wParam),
                         (void*)result,
                         (void*)chainedResult,
                         priorWndProc);
            }
            if (chainedResult == 0) {
                const LRESULT defaultResult = CallDefaultWindowProcCompat(hWnd, msg, wParam, lParam);
                if (traceHotkeyWndproc || g_enableSwallowTraceLogs) {
                    ProxyLog("[INPUTGUARD][%s-CallGameWndProc-defproc] %s game=0x%p chained=0x%p default=0x%p priorWndProc=0x%p",
                             stage ? stage : "wndproc",
                             DescribeShellHotkeyTraceMessage(msg, wParam),
                             (void*)result,
                             (void*)chainedResult,
                             (void*)defaultResult,
                             priorWndProc);
                }
                return defaultResult;
            }
            return chainedResult;
        }
    } else if (preferGameHook && !g_pModCallGameWndProc && !g_loggedMagicStubNoModExport) {
        g_loggedMagicStubNoModExport = true;
        ProxyLog("[INPUTGUARD] WndProc routing wanted ModCallGameWndProc but export missing — see mod/docs/SHELL_HOTKEY_POLICY.md");
    }

    if (priorWndProc) {
        return CallWindowProcCompat(priorWndProc, hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static int ReadShellSuppressFlag() {
    __try {
        return *reinterpret_cast<int*>(kAddrShellHotkeySuppressFlag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static const char* ClassifyVanillaWndProcResult(UINT uMsg, WPARAM wParam, LRESULT result, int suppressFlag) {
    if (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0u) == SC_KEYMENU) {
        if (result == 0) {
            return suppressFlag == 1 ? "vanilla-SC_KEYMENU-return0-suppress-on"
                                   : "vanilla-SC_KEYMENU-return0";
        }
        return "vanilla-SC_KEYMENU-handled";
    }

    if (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0u) == SC_TASKLIST) {
        return result == 0 ? "vanilla-SC_TASKLIST-return0" : "vanilla-SC_TASKLIST-handled";
    }

    if ((uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) && IsShellWinLogoKey(wParam) && result == 0) {
        return "shell-win-return0";
    }

    if (uMsg == WM_INPUTLANGCHANGEREQUEST && result == 0) {
        return "vanilla-WM_INPUTLANGCHANGEREQUEST-return0";
    }

    return nullptr;
}

static bool ShouldLogHotkeyTraceWndproc(UINT uMsg, WPARAM wParam) {
    return g_enableHotkeyTraceLogs && IsShellHotkeyTraceMessage(uMsg, wParam);
}

static bool IsMouseHoverTraceMessage(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_MOUSEHOVER:
    case WM_SETCURSOR:
    case WM_CAPTURECHANGED:
    case WM_MOUSEACTIVATE:
    case WM_NCHITTEST:
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL:
        return true;
    default:
        return false;
    }
}

static const char* DescribeMouseHoverTraceMessage(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
    case WM_MOUSELEAVE: return "WM_MOUSELEAVE";
    case WM_MOUSEHOVER: return "WM_MOUSEHOVER";
    case WM_SETCURSOR: return "WM_SETCURSOR";
    case WM_CAPTURECHANGED: return "WM_CAPTURECHANGED";
    case WM_MOUSEACTIVATE: return "WM_MOUSEACTIVATE";
    case WM_NCHITTEST: return "WM_NCHITTEST";
    case WM_NCMOUSEMOVE: return "WM_NCMOUSEMOVE";
    case WM_NCMOUSELEAVE: return "WM_NCMOUSELEAVE";
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return "WM_LBUTTONUP";
    case WM_MBUTTONDOWN: return "WM_MBUTTONDOWN";
    case WM_MBUTTONUP: return "WM_MBUTTONUP";
    case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
    case WM_RBUTTONUP: return "WM_RBUTTONUP";
    case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
    default: return "UNKNOWN_MOUSE_MSG";
    }
}

static bool ShouldEmitMouseHoverTraceSample() {
    static uint32_t s_mouseTraceCount = 0;
    ++s_mouseTraceCount;
    return s_mouseTraceCount <= 200 || (s_mouseTraceCount % 90u) == 0;
}

static void LogMouseHoverTraceState(const char* stage,
                                    HWND hWnd,
                                    UINT msg,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    const LRESULT* result) {
    if (!(g_enableSwallowTraceLogs || g_enableHotkeyTraceLogs) || !IsMouseHoverTraceMessage(msg)) {
        return;
    }
    if (!ShouldEmitMouseHoverTraceSample()) {
        return;
    }

    POINT screenPt = {};
    POINT clientPt = {};
    GetCursorPos(&screenPt);
    clientPt = screenPt;
    if (hWnd) {
        ScreenToClient(hWnd, &clientPt);
    }

    const HWND capture = GetCapture();
    const HWND fg = GetForegroundWindow();
    const HWND active = GetActiveWindow();
    const HWND focus = GetFocus();

    if (result) {
        ProxyLog("[MOUSETRACE][%s] %s hwnd=0x%p wp=0x%08X lp=0x%08X result=0x%p capture=0x%p fg=0x%p active=0x%p focus=0x%p cursor=(%ld,%ld) client=(%ld,%ld) menu=%d borderless=%d",
                 stage ? stage : "unknown",
                 DescribeMouseHoverTraceMessage(msg),
                 hWnd,
                 (unsigned int)wParam,
                 (unsigned int)lParam,
                 (void*)*result,
                 capture,
                 fg,
                 active,
                 focus,
                 (long)screenPt.x,
                 (long)screenPt.y,
                 (long)clientPt.x,
                 (long)clientPt.y,
                 g_showMenu ? 1 : 0,
                 g_isCurrentlyBorderless ? 1 : 0);
    } else {
        ProxyLog("[MOUSETRACE][%s] %s hwnd=0x%p wp=0x%08X lp=0x%08X capture=0x%p fg=0x%p active=0x%p focus=0x%p cursor=(%ld,%ld) client=(%ld,%ld) menu=%d borderless=%d",
                 stage ? stage : "unknown",
                 DescribeMouseHoverTraceMessage(msg),
                 hWnd,
                 (unsigned int)wParam,
                 (unsigned int)lParam,
                 capture,
                 fg,
                 active,
                 focus,
                 (long)screenPt.x,
                 (long)screenPt.y,
                 (long)clientPt.x,
                 (long)clientPt.y,
                 g_showMenu ? 1 : 0,
                 g_isCurrentlyBorderless ? 1 : 0);
    }
}

static bool IsAsyncShellKeyDown(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

static bool IsShellKeyTraceEnabledD3d9() {
    return g_enableSwallowTraceLogs || g_enableHotkeyTraceLogs;
}

static void FormatWindowBriefD3d9(HWND hwnd, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    if (!hwnd) {
        snprintf(out, outSize, "(null)");
        return;
    }

    wchar_t className[64] = {};
    wchar_t title[96] = {};
    GetClassNameW(hwnd, className, (int)(sizeof(className) / sizeof(className[0])));
    GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));

    char classUtf8[96] = {};
    char titleUtf8[160] = {};
    WideCharToMultiByte(CP_UTF8, 0, className, -1, classUtf8, (int)sizeof(classUtf8), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, titleUtf8, (int)sizeof(titleUtf8), nullptr, nullptr);
    snprintf(out, outSize, "0x%p cls=%s title=\"%.72s\"", hwnd, classUtf8, titleUtf8);
}

static void PollShellKeyAsyncEdgesD3d9(HWND gameWindow) {
    if (!IsShellKeyTraceEnabledD3d9()) {
        return;
    }

    struct Sample {
        bool lWin;
        bool rWin;
        bool apps;
        bool initialized;
    };
    static Sample previous = {};

    const Sample current = {
        IsAsyncShellKeyDown(VK_LWIN),
        IsAsyncShellKeyDown(VK_RWIN),
        IsAsyncShellKeyDown(VK_APPS),
        true,
    };

    if (!previous.initialized) {
        previous = current;
        previous.initialized = true;
        return;
    }

    const HWND foreground = GetForegroundWindow();
    WNDPROC gameWndProc = nullptr;
    if (gameWindow) {
        gameWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC));
    }
    const int suppressFlag = ReadShellSuppressFlag();
    auto logEdge = [&](const char* keyName, bool wasDown, bool isDown) {
        if (wasDown == isDown) {
            return;
        }
        char fgBrief[256] = {};
        FormatWindowBriefD3d9(foreground, fgBrief, sizeof(fgBrief));
        const bool fgIsGame =
            (foreground == gameWindow ||
             (gameWindow && GetAncestor(foreground, GA_ROOT) == gameWindow));
        ProxyLog("[SWALLOW-TRACE][async-d3d9] %s %s game=0x%p wndproc=0x%p fgIsGame=%d suppress=%d fg=%s "
                 "(Win often has no WM_KEYDOWN)",
                 keyName,
                 isDown ? "DOWN" : "UP",
                 gameWindow,
                 gameWndProc,
                 fgIsGame ? 1 : 0,
                 suppressFlag,
                 fgBrief);
    };

    logEdge("LWIN", previous.lWin, current.lWin);
    logEdge("RWIN", previous.rWin, current.rWin);
    logEdge("APPS", previous.apps, current.apps);
    previous = current;
}

static void LogSwallowTraceWndproc(const char* stage,
                                   UINT uMsg,
                                   WPARAM wParam,
                                   LPARAM lParam,
                                   LRESULT result,
                                   bool toGame) {
    if (!g_enableSwallowTraceLogs || !IsShellRelatedMessage(uMsg, wParam)) {
        return;
    }

    const int suppressFlag = ReadShellSuppressFlag();
    const char* classification = ClassifyVanillaWndProcResult(uMsg, wParam, result, suppressFlag);
    ProxyLog("[SWALLOW-TRACE][%s] %s vk/cmd=0x%04X lParam=0x%08X ->%s result=0x%p suppress=%d classify=%s",
             stage ? stage : "wndproc",
             DescribeShellHotkeyTraceMessage(uMsg, wParam),
             (unsigned int)wParam,
             (unsigned int)lParam,
             toGame ? "game" : "DefWindowProc",
             (void*)result,
             suppressFlag,
             classification ? classification : "ok");
}

static int QueryModMenuRequestedOpenState() {
    if (!g_pModIsMenuRequestedOpen) {
        return -1;
    }

    return g_pModIsMenuRequestedOpen() ? 1 : 0;
}

static void LogOverlayMenuHotkeyEvent(const char* stage,
                                      const char* source,
                                      const char* keyName,
                                      HWND hWnd,
                                      UINT uMsg,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      bool wasDown) {
    ProxyLog("[MENUHOTKEY][%s] source=%s key=%s msg=%s hwnd=0x%p wParam=0x%08X lParam=0x%08X repeat=%d wasDown=%d showMenu=%d modRequested=%d imgui=%d fg=0x%p active=0x%p focus=0x%p",
             stage ? stage : "unknown",
             source ? source : "unknown",
             keyName ? keyName : "?",
             DescribeShellHotkeyTraceMessage(uMsg, wParam),
             hWnd,
             (unsigned int)wParam,
             (unsigned int)lParam,
             (lParam & 0x40000000u) ? 1 : 0,
             wasDown ? 1 : 0,
             g_showMenu ? 1 : 0,
             QueryModMenuRequestedOpenState(),
             g_imguiInitialized ? 1 : 0,
             GetForegroundWindow(),
             GetActiveWindow(),
             GetFocus());
}

static void SetProxyMenuVisibleInternal(bool visible,
                                        const char* reason,
                                        const char* source,
                                        HWND hWnd,
                                        UINT uMsg,
                                        WPARAM wParam,
                                        LPARAM lParam) {
    const bool previous = g_showMenu;
    const int modRequested = QueryModMenuRequestedOpenState();
    g_showMenu = visible;

    if (previous && !visible && g_gameWindow) {
        ReleaseGameMouseCapture(g_gameWindow);
    }

    ProxyLog("[MENU] Visibility %s -> %s source=%s reason=%s msg=%s hwnd=0x%p repeat=%d modRequested=%d settingsVisibleNow=%d",
             previous ? "ON" : "OFF",
             g_showMenu ? "ON" : "OFF",
             source ? source : "unknown",
             reason ? reason : "unknown",
             DescribeShellHotkeyTraceMessage(uMsg, wParam),
             hWnd,
             (lParam & 0x40000000u) ? 1 : 0,
             modRequested,
             (g_showMenu && modRequested == 1) ? 1 : 0);
}

static void ResetOverlayHotkeyState(const char* source, const char* reason, HWND hWnd, UINT uMsg) {
    if (!g_menuHotkeyF1Down && !g_menuHotkeyF11Down) {
        return;
    }

    ProxyLog("[MENUHOTKEY][reset] source=%s reason=%s msg=0x%04X hwnd=0x%p f1Down=%d f11Down=%d showMenu=%d modRequested=%d",
             source ? source : "unknown",
             reason ? reason : "unknown",
             (unsigned int)uMsg,
             hWnd,
             g_menuHotkeyF1Down ? 1 : 0,
             g_menuHotkeyF11Down ? 1 : 0,
             g_showMenu ? 1 : 0,
             QueryModMenuRequestedOpenState());

    g_menuHotkeyF1Down = false;
    g_menuHotkeyF11Down = false;
}

static bool HandleOverlayHotkeys(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const char* source) {
    if (wParam != VK_F1 && wParam != VK_F11) {
        return false;
    }

    const bool isKeyDown = (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN);
    const bool isKeyUp = (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP);
    if (!isKeyDown && !isKeyUp) {
        return false;
    }

    bool* downState = (wParam == VK_F1) ? &g_menuHotkeyF1Down : &g_menuHotkeyF11Down;
    const char* keyName = (wParam == VK_F1) ? "F1" : "F11";

    if (isKeyDown) {
        if (*downState && (GetAsyncKeyState((int)wParam) & 0x8000) == 0) {
            ProxyLog("[MENUHOTKEY][stale-recover] source=%s key=%s msg=%s hwnd=0x%p lParam=0x%08X",
                     source ? source : "unknown",
                     keyName,
                     DescribeShellHotkeyTraceMessage(uMsg, wParam),
                     hWnd,
                     (unsigned int)lParam);
            *downState = false;
        }

        const bool wasDown = *downState;
        LogOverlayMenuHotkeyEvent("down", source, keyName, hWnd, uMsg, wParam, lParam, wasDown);
        if (wasDown) {
            ProxyLog("[MENUHOTKEY][duplicate] source=%s key=%s msg=%s hwnd=0x%p repeat=%d firstKeydown=%d ignored=1",
                     source ? source : "unknown",
                     keyName,
                     DescribeShellHotkeyTraceMessage(uMsg, wParam),
                     hWnd,
                     (lParam & 0x40000000u) ? 1 : 0,
                     IsFirstKeydown(uMsg, lParam) ? 1 : 0);
            return true;
        }

        *downState = true;
        if (wParam == VK_F1) {
            SetProxyMenuVisibleInternal(!g_showMenu, "F1 hotkey", source, hWnd, uMsg, wParam, lParam);
            return true;
        }

        const bool beforeBorderless = g_isCurrentlyBorderless;
        ToggleBorderlessFullscreen(hWnd);
        ProxyLog("[MENUHOTKEY][toggle] source=%s key=F11 borderless=%d->%d hwnd=0x%p",
                 source ? source : "unknown",
                 beforeBorderless ? 1 : 0,
                 g_isCurrentlyBorderless ? 1 : 0,
                 hWnd);
        return true;
    }

    const bool wasDown = *downState;
    *downState = false;
    LogOverlayMenuHotkeyEvent(wasDown ? "up" : "stray-up", source, keyName, hWnd, uMsg, wParam, lParam, wasDown);
    return true;
}

LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LoadInputGuardSettings();
    if (g_enableHotkeyTraceLogs) {
        LogShellHotkeyTraceState("HookedWndProc-enter", hWnd, msg, wParam, lParam);
    }
    const bool traceHotkeyWndproc = ShouldLogHotkeyTraceWndproc(msg, wParam);
    LogMouseHoverTraceState("HookedWndProc-enter", hWnd, msg, wParam, lParam, nullptr);

    if (msg == WM_CLOSE) {
        ProxyLog("[IMGUIWNDPROC] WM_CLOSE received hwnd=0x%p", hWnd);
        NotifyGameExitOnce(0, "WM_CLOSE - Normal exit");
    } else if (msg == WM_DESTROY) {
        ProxyLog("[IMGUIWNDPROC] WM_DESTROY received hwnd=0x%p", hWnd);
        ResetOverlayHotkeyState("HookedWndProc", "WM_DESTROY", hWnd, msg);
        NotifyGameExitOnce(0, "WM_DESTROY");
        PostQuitMessageOnce("HookedWndProc WM_DESTROY");
    } else if (msg == WM_NCDESTROY) {
        ProxyLog("[IMGUIWNDPROC] WM_NCDESTROY received hwnd=0x%p", hWnd);
        ResetOverlayHotkeyState("HookedWndProc", "WM_NCDESTROY", hWnd, msg);
        NotifyGameExitOnce(0, "WM_NCDESTROY");
        PostQuitMessageOnce("HookedWndProc WM_NCDESTROY");
    } else if (msg == WM_ACTIVATEAPP && wParam == FALSE) {
        ResetOverlayHotkeyState("HookedWndProc", "WM_ACTIVATEAPP deactivate", hWnd, msg);
        ReleaseGameMouseCapture(hWnd);
    }

    if (g_enableSwallowTraceLogs && IsShellRelatedMessage(msg, wParam)) {
        ProxyLog("[SWALLOW-TRACE][HookedWndProc-leak] shell msg reached mod layer (not passthrough) %s suppress=%d",
                 DescribeShellHotkeyTraceMessage(msg, wParam),
                 ReadShellSuppressFlag());
    }

    if (HandleOverlayHotkeys(hWnd, msg, wParam, lParam, "HookedWndProc")) {
        const LRESULT overlayResult = 0;
        LogMouseHoverTraceState("HookedWndProc-overlay-return", hWnd, msg, wParam, lParam, &overlayResult);
        return 0;
    }
    
    // ========================================================================
    // Window resize handling (windowed mode only)
    // ========================================================================
    
    // Constrain window resize to 4:3 aspect ratio
    if (msg == WM_SIZING && !g_isCurrentlyBorderless) {
        RECT* rect = (RECT*)lParam;
        int width = rect->right - rect->left;
        int height = rect->bottom - rect->top;
        
        // Get window border sizes
        RECT clientRect, windowRect;
        GetClientRect(hWnd, &clientRect);
        GetWindowRect(hWnd, &windowRect);
        int borderWidth = (windowRect.right - windowRect.left) - (clientRect.right - clientRect.left);
        int borderHeight = (windowRect.bottom - windowRect.top) - (clientRect.bottom - clientRect.top);
        
        int clientWidth = width - borderWidth;
        int clientHeight = height - borderHeight;
        
        // Enforce 4:3 aspect ratio
        float targetAspect = 4.0f / 3.0f;
        
        if (wParam == WMSZ_LEFT || wParam == WMSZ_RIGHT) {
            clientHeight = (int)(clientWidth / targetAspect);
        } else if (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM) {
            clientWidth = (int)(clientHeight * targetAspect);
        } else {
            int newHeight = (int)(clientWidth / targetAspect);
            int newWidth = (int)(clientHeight * targetAspect);
            if (newHeight > clientHeight) {
                clientHeight = newHeight;
            } else {
                clientWidth = newWidth;
            }
        }
        
        // Enforce minimum size
        if (clientWidth < g_nativeWidth) {
            clientWidth = g_nativeWidth;
            clientHeight = g_nativeHeight;
        }
        
        int newWidth = clientWidth + borderWidth;
        int newHeight = clientHeight + borderHeight;
        
        // Adjust edges
        switch (wParam) {
            case WMSZ_LEFT:
            case WMSZ_TOPLEFT:
            case WMSZ_BOTTOMLEFT:
                rect->left = rect->right - newWidth;
                break;
            default:
                rect->right = rect->left + newWidth;
                break;
        }
        switch (wParam) {
            case WMSZ_TOP:
            case WMSZ_TOPLEFT:
            case WMSZ_TOPRIGHT:
                rect->top = rect->bottom - newHeight;
                break;
            default:
                rect->bottom = rect->top + newHeight;
                break;
        }
        
        return TRUE;
    }
    
    // Handle WM_SIZE for scaling updates
    if (msg == WM_SIZE && !g_isCurrentlyBorderless) {
        int newWidth = LOWORD(lParam);
        int newHeight = HIWORD(lParam);
        
        if (wParam == SIZE_MAXIMIZED) {
            ProxyLog("[RESIZE] Maximized: %dx%d", newWidth, newHeight);
            g_currentWindowWidth = newWidth;
            g_currentWindowHeight = newHeight;
            g_screenWidth = newWidth;
            g_screenHeight = newHeight;
            g_windowResizedNeedsReinit = true;
        } else if (wParam != SIZE_MINIMIZED) {
            if (newWidth != g_currentWindowWidth || newHeight != g_currentWindowHeight) {
                ProxyLog("[RESIZE] Window: %dx%d -> %dx%d", 
                         g_currentWindowWidth, g_currentWindowHeight, newWidth, newHeight);
                g_currentWindowWidth = newWidth;
                g_currentWindowHeight = newHeight;
                g_screenWidth = newWidth;
                g_screenHeight = newHeight;
                
                if (newWidth > g_nativeWidth || newHeight > g_nativeHeight) {
                    g_windowResizedNeedsReinit = true;
                } else if (g_scalingInitialized) {
                    ReleaseScalingSwapChain();
                }
            }
        }
    }
    
    // Save display config after user finishes resizing the window
    if (msg == WM_EXITSIZEMOVE && !g_isCurrentlyBorderless) {
        ProxyLog("[RESIZE] Drag/move complete, saving display config (%dx%d)", 
                 g_currentWindowWidth, g_currentWindowHeight);
        DisplayConfig_Save();
    }
    
    // Handle WM_GETMINMAXINFO for minimum size
    if (msg == WM_GETMINMAXINFO && !g_isCurrentlyBorderless) {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        RECT clientRect = {0, 0, g_nativeWidth, g_nativeHeight};
        AdjustWindowRectEx(&clientRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = clientRect.right - clientRect.left;
        mmi->ptMinTrackSize.y = clientRect.bottom - clientRect.top;
    }
    
    // ========================================================================
    // ImGui input handling
    // ========================================================================
    
    // Let ImGui handle input first
    // When scaling is active, we need to transform mouse coordinates
    if (g_imguiInitialized && g_showMenu && !IsExclusiveModOverlayActive() &&
        !IsShellKeyWndprocMessage(msg, wParam)) {
        // Scale mouse coordinates if we're in scaling mode
        WPARAM scaledWParam = wParam;
        LPARAM scaledLParam = lParam;
        
        if ((g_isCurrentlyBorderless || g_currentWindowWidth > g_nativeWidth || g_currentWindowHeight > g_nativeHeight) &&
            (g_screenWidth > 0 && g_screenHeight > 0)) {
            // Check if this is a mouse message that contains coordinates
            if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
                msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN ||
                msg == WM_MBUTTONUP || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                msg == WM_MBUTTONDBLCLK) {
                
                // Get current mouse position in window coordinates
                int mouseX = GET_X_LPARAM(lParam);
                int mouseY = GET_Y_LPARAM(lParam);

                // Determine the active render area within the window (handles letterbox/pillarbox)
                RECT viewRect = {0, 0, g_screenWidth, g_screenHeight};
                if (g_keepAspectRatio) {
                    // Prefer the most recently calculated rect (from Present). If it's not valid,
                    // compute it from current window size.
                    if ((g_letterboxDestRect.right > g_letterboxDestRect.left) &&
                        (g_letterboxDestRect.bottom > g_letterboxDestRect.top)) {
                        viewRect = g_letterboxDestRect;
                    } else {
                        CalculateLetterboxDestRect(g_screenWidth, g_screenHeight, g_nativeWidth, g_nativeHeight, &viewRect);
                    }
                }

                int viewW = (viewRect.right - viewRect.left);
                int viewH = (viewRect.bottom - viewRect.top);
                int scaledX = -1;
                int scaledY = -1;

                if (viewW > 0 && viewH > 0 &&
                    mouseX >= viewRect.left && mouseX < viewRect.right &&
                    mouseY >= viewRect.top && mouseY < viewRect.bottom) {
                    float u = (float)(mouseX - viewRect.left) / (float)viewW;
                    float v = (float)(mouseY - viewRect.top) / (float)viewH;
                    scaledX = (int)(u * (float)g_nativeWidth);
                    scaledY = (int)(v * (float)g_nativeHeight);
                }
                
                // Rebuild lParam with scaled coordinates
                scaledLParam = MAKELPARAM(scaledX, scaledY);
            }
        }
        
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, scaledWParam, scaledLParam)) {
            if (traceHotkeyWndproc) {
                ProxyLog("[HOTKEYTRACE][HookedWndProc-imgui-consumed] %s wantCaptureKeyboard=%d wantTextInput=%d",
                         DescribeShellHotkeyTraceMessage(msg, wParam),
                         ImGui::GetIO().WantCaptureKeyboard ? 1 : 0,
                         ImGui::GetIO().WantTextInput ? 1 : 0);
            }
            if (g_enableSwallowTraceLogs && IsShellRelatedMessage(msg, wParam)) {
                ProxyLog("[SWALLOW-TRACE][HookedWndProc-imgui] ImGui consumed %s",
                         DescribeShellHotkeyTraceMessage(msg, wParam));
            }
            const LRESULT imguiResult = 1;
            LogMouseHoverTraceState("HookedWndProc-imgui-return", hWnd, msg, wParam, lParam, &imguiResult);
            return true;
        }
    }
    
    // Chain through the same game path used by ProxyWndProc so root-window
    // messages (borderless/ImGui path) also bypass DXLib 0xFFFF stubs.
    if (g_imguiOriginalWndProc) {
        if (g_enableSwallowTraceLogs && IsShellRelatedMessage(msg, wParam)) {
            ProxyLog("[SWALLOW-TRACE][HookedWndProc->chain] forwarding %s to prior wndproc 0x%p",
                     DescribeShellHotkeyTraceMessage(msg, wParam),
                     g_imguiOriginalWndProc);
        }
        LRESULT result = CallGameWndProcChain(
            hWnd, msg, wParam, lParam, g_imguiOriginalWndProc, "HookedWndProc", traceHotkeyWndproc);
        if (traceHotkeyWndproc) {
            ProxyLog("[HOTKEYTRACE][HookedWndProc-exit] %s result=0x%p",
                     DescribeShellHotkeyTraceMessage(msg, wParam),
                     (void*)result);
        }
        LogMouseHoverTraceState("HookedWndProc-chain-exit", hWnd, msg, wParam, lParam, &result);
        LogSwallowTraceWndproc("HookedWndProc-chain-exit", msg, wParam, lParam, result, true);
        return result;
    }
    
    LRESULT result = DefWindowProcW(hWnd, msg, wParam, lParam);
    if (traceHotkeyWndproc) {
        ProxyLog("[HOTKEYTRACE][HookedWndProc-fallback] %s result=0x%p",
                 DescribeShellHotkeyTraceMessage(msg, wParam),
                 (void*)result);
    }
    LogMouseHoverTraceState("HookedWndProc-fallback", hWnd, msg, wParam, lParam, &result);
    LogSwallowTraceWndproc("HookedWndProc-fallback", msg, wParam, lParam, result, false);
    return result;
}

// ============================================================================
// Proxy Window (handles display and input)
// ============================================================================

// Forward input to game window
// Proxy window functions removed


// ============================================================================
// ImGui Initialization
// ============================================================================

bool InitImGui(IDirect3DDevice9* pDevice, HWND hWnd) {
    if (g_imguiInitialized) return true;
    
    ProxyLog("[IMGUI] Initializing ImGui...");
    ProxyLog("[IMGUI] Device: 0x%p, Window: 0x%p", pDevice, hWnd);
    
    if (!pDevice || !hWnd) {
        ProxyLog("[IMGUI] ERROR: Invalid device or window handle!");
        return false;
    }
    
    // In borderless fullscreen we operate on the ROOT window (it receives focus/input).
    // Initialize ImGui and hook WndProc on that window to keep input reliable.
    HWND hRoot = GetAncestor(hWnd, GA_ROOT);
    HWND hImGuiWindow = hRoot ? hRoot : hWnd;

    // Get window info
    RECT rect;
    GetClientRect(hImGuiWindow, &rect);
    ProxyLog("[IMGUI] Window client size: %dx%d", rect.right - rect.left, rect.bottom - rect.top);
    
    // Create ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ConfigureOverlayFonts(io);
    
    // Set ini file path
    static char iniPath[MAX_PATH];
    snprintf(iniPath, MAX_PATH, "%s\\as2_imgui.ini", g_dllDir);
    io.IniFilename = iniPath;
    ProxyLog("[IMGUI] INI file: %s", iniPath);
    
    // Style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    
    // Initialize backends
    ProxyLog("[IMGUI] Calling ImGui_ImplWin32_Init...");
    if (!ImGui_ImplWin32_Init(hImGuiWindow)) {
        ProxyLog("[IMGUI] ERROR: ImGui_ImplWin32_Init FAILED!");
        return false;
    }
    ProxyLog("[IMGUI] ImGui_ImplWin32_Init SUCCESS");
    
    ProxyLog("[IMGUI] Calling ImGui_ImplDX9_Init...");
    if (!ImGui_ImplDX9_Init(pDevice)) {
        ProxyLog("[IMGUI] ERROR: ImGui_ImplDX9_Init FAILED!");
        ImGui_ImplWin32_Shutdown();
        return false;
    }
    ProxyLog("[IMGUI] ImGui_ImplDX9_Init SUCCESS");
    
    // Hook window procedure for ImGui input
    WNDPROC currentWndProc = (WNDPROC)GetWindowLongPtrW(hImGuiWindow, GWLP_WNDPROC);
    ProxyLog("[IMGUI] Current WndProc before hook: 0x%08X", (DWORD)(ULONG_PTR)currentWndProc);
    
    // Hook the WndProc to capture input for ImGui
    g_imguiOriginalWndProc = (WNDPROC)SetWindowLongPtrW(hImGuiWindow, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    ProxyLog("[IMGUI] SetWindowLongPtr returned: 0x%08X", (DWORD)(ULONG_PTR)g_imguiOriginalWndProc);
    if (!g_imguiOriginalWndProc) {
        ProxyLog("[IMGUI] WARNING: Failed to capture previous WndProc, falling back to default chaining");
    }
    
    g_gameWindow = hImGuiWindow;
    g_pDevice = pDevice;
    g_imguiInitialized = true;
    
    ProxyLog("[IMGUI] ImGui initialized successfully!");
    return true;
}

void ShutdownImGui() {
    if (!g_imguiInitialized) return;
    
    ProxyLog("[IMGUI] Shutting down ImGui...");
    g_imguiDrawDataReady = false;

    HWND restoreWindow = ResolvePrimaryGameWindow();
    if (g_imguiOriginalWndProc && restoreWindow && IsWindow(restoreWindow)) {
        ProxyLog("[IMGUI] Restoring WndProc on hwnd=0x%p", restoreWindow);
        SetWindowLongPtrW(restoreWindow, GWLP_WNDPROC, (LONG_PTR)g_imguiOriginalWndProc);
    } else if (g_imguiOriginalWndProc) {
        ProxyLog("[IMGUI] Skipping WndProc restore because the target window is already gone");
    }
    g_imguiOriginalWndProc = nullptr;

    ProxyLog("[IMGUI] ImGui_ImplDX9_Shutdown...");
    ImGui_ImplDX9_Shutdown();
    ProxyLog("[IMGUI] ImGui_ImplWin32_Shutdown...");
    ImGui_ImplWin32_Shutdown();
    ProxyLog("[IMGUI] DestroyContext...");
    ImGui::DestroyContext();
    
    g_imguiInitialized = false;
    ProxyLog("[IMGUI] Shutdown complete");
}

// ============================================================================
// ImGui Rendering
// ============================================================================

static bool ShouldRenderImGuiViaScalingTarget() {
    if (!g_scalingInitialized || !g_pScalingSwapChain || !g_pScalingBackBuffer) {
        return false;
    }

    return g_isCurrentlyBorderless ||
        g_currentWindowWidth > g_nativeWidth ||
        g_currentWindowHeight > g_nativeHeight;
}

static void TransformImGuiDrawData(ImDrawData* drawData,
                                   const ImVec2& originalDisplayPos,
                                   float scaleX,
                                   float scaleY,
                                   float offsetX,
                                   float offsetY) {
    if (!drawData) {
        return;
    }

    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        ImDrawList* drawList = drawData->CmdLists[listIndex];
        if (!drawList) {
            continue;
        }

        for (int vertexIndex = 0; vertexIndex < drawList->VtxBuffer.Size; ++vertexIndex) {
            ImDrawVert& vertex = drawList->VtxBuffer[vertexIndex];
            vertex.pos.x = offsetX + (vertex.pos.x - originalDisplayPos.x) * scaleX;
            vertex.pos.y = offsetY + (vertex.pos.y - originalDisplayPos.y) * scaleY;
        }

        for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex) {
            ImDrawCmd& cmd = drawList->CmdBuffer[cmdIndex];
            cmd.ClipRect.x = offsetX + (cmd.ClipRect.x - originalDisplayPos.x) * scaleX;
            cmd.ClipRect.y = offsetY + (cmd.ClipRect.y - originalDisplayPos.y) * scaleY;
            cmd.ClipRect.z = offsetX + (cmd.ClipRect.z - originalDisplayPos.x) * scaleX;
            cmd.ClipRect.w = offsetY + (cmd.ClipRect.w - originalDisplayPos.y) * scaleY;
        }
    }
}

static bool RenderPreparedImGuiToScalingTarget(IDirect3DDevice9* pDevice,
                                               IDirect3DSurface9* pTargetSurface,
                                               int targetWidth,
                                               int targetHeight,
                                               const RECT& destRect) {
    if (!g_imguiDrawDataReady || !pDevice || !pTargetSurface) {
        return false;
    }

    if (g_renderingPreparedImGuiToScalingTarget) {
        ProxyLog("[IMGUI] Skipping prepared draw: re-entrant scaling-target render");
        g_imguiDrawDataReady = false;
        return false;
    }

    struct ScopedPreparedImGuiRender {
        bool& flag;
        explicit ScopedPreparedImGuiRender(bool& value) : flag(value) { flag = true; }
        ~ScopedPreparedImGuiRender() { flag = false; }
    } preparedRenderGuard(g_renderingPreparedImGuiToScalingTarget);

    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->CmdListsCount <= 0 ||
        drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        g_imguiDrawDataReady = false;
        return false;
    }

    const int destWidth = destRect.right - destRect.left;
    const int destHeight = destRect.bottom - destRect.top;
    if (destWidth <= 0 || destHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
        g_imguiDrawDataReady = false;
        return false;
    }

    const ImVec2 originalDisplayPos = drawData->DisplayPos;
    const ImVec2 originalDisplaySize = drawData->DisplaySize;
    const ImVec2 originalFramebufferScale = drawData->FramebufferScale;
    const float scaleX = (float)destWidth / originalDisplaySize.x;
    const float scaleY = (float)destHeight / originalDisplaySize.y;

    IDirect3DSurface9* previousRenderTarget = nullptr;
    IDirect3DSurface9* previousDepthStencil = nullptr;
    pDevice->GetRenderTarget(0, &previousRenderTarget);
    pDevice->GetDepthStencilSurface(&previousDepthStencil);

    HRESULT hr = pDevice->SetRenderTarget(0, pTargetSurface);
    if (FAILED(hr)) {
        if (previousDepthStencil) previousDepthStencil->Release();
        if (previousRenderTarget) previousRenderTarget->Release();
        g_imguiDrawDataReady = false;
        return false;
    }

    pDevice->SetDepthStencilSurface(nullptr);

    hr = g_pOriginalBeginScene ? g_pOriginalBeginScene(pDevice) : pDevice->BeginScene();
    if (FAILED(hr)) {
        ProxyLog("[IMGUI] BeginScene failed while rendering prepared draw data: 0x%08X", hr);
        if (previousDepthStencil) {
            pDevice->SetDepthStencilSurface(previousDepthStencil);
        }
        if (previousRenderTarget) {
            pDevice->SetRenderTarget(0, previousRenderTarget);
        }
        if (previousDepthStencil) previousDepthStencil->Release();
        if (previousRenderTarget) previousRenderTarget->Release();
        g_imguiDrawDataReady = false;
        return false;
    }

    TransformImGuiDrawData(drawData,
        originalDisplayPos,
        scaleX,
        scaleY,
        (float)destRect.left,
        (float)destRect.top);
    drawData->DisplayPos = ImVec2(0.0f, 0.0f);
    drawData->DisplaySize = ImVec2((float)targetWidth, (float)targetHeight);
    drawData->FramebufferScale = ImVec2(1.0f, 1.0f);

    ImGui_ImplDX9_RenderDrawData(drawData);

    drawData->DisplayPos = originalDisplayPos;
    drawData->DisplaySize = originalDisplaySize;
    drawData->FramebufferScale = originalFramebufferScale;
    TransformImGuiDrawData(drawData,
        ImVec2((float)destRect.left, (float)destRect.top),
        1.0f / scaleX,
        1.0f / scaleY,
        originalDisplayPos.x,
        originalDisplayPos.y);

    hr = g_pOriginalEndScene ? g_pOriginalEndScene(pDevice) : pDevice->EndScene();
    if (FAILED(hr)) {
        ProxyLog("[IMGUI] EndScene failed while rendering prepared draw data: 0x%08X", hr);
    }

    if (previousDepthStencil) {
        pDevice->SetDepthStencilSurface(previousDepthStencil);
        previousDepthStencil->Release();
    }
    if (previousRenderTarget) {
        pDevice->SetRenderTarget(0, previousRenderTarget);
        previousRenderTarget->Release();
    }

    g_imguiDrawDataReady = false;
    return true;
}

void RenderImGui() {
    if (!g_imguiInitialized || !g_pDevice) return;

    const bool exclusiveOverlay = IsExclusiveModOverlayActive();
    bool modShouldRender = true;
    if (!g_showMenu && !exclusiveOverlay) {
        modShouldRender = g_pModShouldRenderImGui ? g_pModShouldRenderImGui() : true;
        // Menu text queued by the mod still needs a pass, even when nothing
        // else wants the overlay this frame.
        if (!modShouldRender && g_menuTextCount == 0) {
            g_imguiDrawDataReady = false;
            return;
        }
    }

    // Start new frame
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // We render ImGui into the game's native backbuffer (typically 640x480).
    // Force ImGui to use that coordinate space so layout and mouse mapping remain stable
    // across window resize/borderless + scaling/letterbox.
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_nativeWidth, (float)g_nativeHeight);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }
    ImGui::NewFrame();

    // Draw whatever the mod queued for its in-game menus, in the game's own
    // coordinate space, using the Mincho face that matches the vanilla labels.
    if (g_menuTextCount > 0) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImFont* baseFont = g_menuFont ? g_menuFont : ImGui::GetFont();
        const float size = g_menuFont ? g_menuFont->FontSize : ImGui::GetFontSize();
        for (int i = 0; i < g_menuTextCount; ++i) {
            const QueuedMenuText& q = g_menuTextQueue[i];
            const float drawSize = q.size > 0.0f ? q.size : size;
            // The large atlas only carries Latin + Cyrillic, so anything with a
            // high codepoint (a Japanese nickname) stays on the full-range one.
            ImFont* font = baseFont;
            if (g_menuFontLarge && drawSize >= 40.0f && !q.hasHighCodepoint) {
                font = g_menuFontLarge;
            }
            // A soft dark edge, the way the vanilla labels are drawn.
            const ImU32 shadow = IM_COL32(20, 20, 20, (int)(q.color >> IM_COL32_A_SHIFT & 0xFF));
            dl->AddText(font, drawSize, ImVec2(q.x + 1.0f, q.y + 1.0f), shadow, q.text);
            dl->AddText(font, drawSize, ImVec2(q.x, q.y), q.color, q.text);
        }
        g_menuTextCount = 0;
    }

    // Main menu bar
    if (g_showMenu && !exclusiveOverlay) {
        const ImVec4 menuBarBg = ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg);
        const ImVec4 popupBg = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg,
            ImVec4(menuBarBg.x, menuBarBg.y, menuBarBg.z, 0.76f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg,
            ImVec4(popupBg.x, popupBg.y, popupBg.z, 0.94f));
        if (ImGui::BeginMainMenuBar()) {
            ImGui::Text("Alice Senki 2 - Improvement Mod 0.7-beta2.01");
            ImGui::Separator();
            if (ImGui::BeginMenu("Options")) {
                if (ImGui::MenuItem("Settings", nullptr, false, g_pModToggleMenu != nullptr)) {
                    g_pModToggleMenu();
                    ProxyLog("[MENU] Settings toggle requested from proxy menu modRequested=%d showMenu=%d",
                             QueryModMenuRequestedOpenState(),
                             g_showMenu ? 1 : 0);
                }
                ImGui::Separator();
                bool showMenu = g_showMenu;
                if (ImGui::MenuItem("Show Menu", "F1", &showMenu)) {
                    SetProxyMenuVisibleInternal(showMenu, "Options/Show Menu item", "ProxyMenuBar", g_gameWindow, WM_APP, VK_F1, 0);
                }
                {
                    bool borderlessChecked = g_isCurrentlyBorderless;
                    if (ImGui::MenuItem("Borderless Fullscreen", "F11", borderlessChecked)) {
                        ToggleBorderlessFullscreen(g_gameWindow ? g_gameWindow : GetActiveWindow());
                    }
                }
                ImGui::MenuItem("Keep Aspect Ratio", nullptr, &g_keepAspectRatio);
                if (ImGui::MenuItem(g_consoleVisible ? "Hide Debug Console" : "Show Debug Console")) {
                    ToggleConsole();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit Game")) {
                    RequestGameShutdown("overlay Exit Game", g_gameWindow);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        ImGui::PopStyleColor(2);
    }
    
    // Render mod overlays only when the mod reports something visible this frame.
    if (g_pModOnPresent && modShouldRender) {
        // Share ImGui context with mod DLL on first call
        if (!g_imguiContextShared && g_pModSetImGuiContext) {
            void* ctx = ImGui::GetCurrentContext();
            ProxyLog("[IMGUI] Sharing ImGui context with mod: 0x%p", ctx);
            g_pModSetImGuiContext(ctx);
            g_imguiContextShared = true;
        }
        InvokeModOnPresentSafely(g_pDevice);
    }
    
    // Render
    ImGui::EndFrame();
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->CmdListsCount <= 0 || drawData->TotalVtxCount <= 0) {
        g_imguiDrawDataReady = false;
        return;
    }

    g_imguiDrawDataReady = true;
    if (ShouldRenderImGuiViaScalingTarget()) {
        return;
    }

    ImGui_ImplDX9_RenderDrawData(drawData);
    g_imguiDrawDataReady = false;
}

// ============================================================================
// SetRenderTarget Hook (pass-through, kept for future use)
// ============================================================================
// NOTE: With the new approach (backbuffer at native 640x480), this hook just
// passes through. D3D9's Present handles scaling natively.

HRESULT WINAPI HookedSetRenderTarget(IDirect3DDevice9* pDevice, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
    // Pass through all SetRenderTarget calls - no redirection needed with native backbuffer
    return g_pOriginalSetRenderTarget(pDevice, RenderTargetIndex, pRenderTarget);
}

// ============================================================================
// SetViewport Hook (pass-through, kept for future use)
// ============================================================================
// NOTE: With the new approach (backbuffer at native 640x480), this hook just
// passes through. D3D9's Present handles scaling natively.

HRESULT WINAPI HookedSetViewport(IDirect3DDevice9* pDevice, const D3DVIEWPORT9* pViewport) {
    // Pass through all SetViewport calls - no clamping needed with native backbuffer
    return g_pOriginalSetViewport(pDevice, pViewport);
}

// ============================================================================
// BeginScene Hook (just pass through, letterboxing handled in Present)
// ============================================================================

HRESULT WINAPI HookedBeginScene(IDirect3DDevice9* pDevice) {
    static bool firstCall = true;
    
    // Call original BeginScene
    HRESULT hr = g_pOriginalBeginScene(pDevice);
    
    if (firstCall) {
        firstCall = false;
        ProxyLog("[BEGINSCENE] First BeginScene complete, scaler=%s", 
                 g_letterboxScaler.IsEnabled() ? "ACTIVE" : "INACTIVE");
    }
    
    // NOTE: With the new approach, we keep the backbuffer at native resolution (640x480)
    // and let D3D9's Present stretch it to fill the window. No render target redirection needed.
    // The BeginFrame/EndFrame calls are disabled - D3D9 handles scaling natively.
    
    return hr;
}

// ============================================================================
// EndScene Hook (called every frame by D3D9)
// ============================================================================

// Flag to track if we need to apply borderless on first frame
// This is set in CreateDevice and checked in Present (NOT EndScene - Reset can't be called during rendering)
static bool g_needsInitialBorderless = false;
static HWND g_pendingBorderlessWindow = nullptr;



HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDevice) {
    UpdateStartupStageSilently("IDirect3DDevice9::EndScene");
    static bool firstCall = true;
    static int frameCount = 0;

    if (g_enableSwallowTraceLogs) {
        D3DDEVICE_CREATION_PARAMETERS traceParams = {};
        if (SUCCEEDED(pDevice->GetCreationParameters(&traceParams)) && traceParams.hFocusWindow) {
            PollShellKeyAsyncEdgesD3d9(traceParams.hFocusWindow);
        }
    }

    if (g_renderingPreparedImGuiToScalingTarget) {
        return g_pOriginalEndScene ? g_pOriginalEndScene(pDevice) : D3D_OK;
    }
    
    if (firstCall) {
        ProxyLog("[ENDSCENE] First EndScene call! Device: 0x%p", pDevice);
        
        // Get render target info
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(pDevice->GetRenderTarget(0, &rt)) && rt) {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(rt->GetDesc(&desc))) {
                ProxyLog("[ENDSCENE] Render target: %dx%d, Format=%d", desc.Width, desc.Height, desc.Format);
            }
            rt->Release();
        }
        
        // Get window from device
        D3DDEVICE_CREATION_PARAMETERS params;
        if (SUCCEEDED(pDevice->GetCreationParameters(&params))) {
            ProxyLog("[ENDSCENE] Device window: 0x%p", params.hFocusWindow);

            // Safety net: ensure our title is set even if it changes later.
            ApplyCustomWindowTitle(params.hFocusWindow);
            
            // Get window class name
            char className[256];
            GetClassNameA(params.hFocusWindow, className, sizeof(className));
            ProxyLog("[ENDSCENE] Window class: %s", className);
            
            // Get window title
            char windowTitle[256];
            GetWindowTextA(params.hFocusWindow, windowTitle, sizeof(windowTitle));
            ProxyLog("[ENDSCENE] Window title: %s", windowTitle);
            
            // Check if we need to apply borderless - but DON'T do it here!
            // Reset cannot be called during BeginScene/EndScene - defer to Present hook
            if (g_needsInitialBorderless && g_useBorderlessFullscreen) {
                ProxyLog("[ENDSCENE] Borderless mode pending - will apply after Present");
                g_pendingBorderlessWindow = params.hFocusWindow;
            }
            
            // Initialize ImGui
            if (!g_imguiInitialized) {
                ProxyLog("[ENDSCENE] Initializing ImGui from EndScene...");
                if (InitImGui(pDevice, params.hFocusWindow)) {
                    ProxyLog("[ENDSCENE] ImGui initialized successfully from EndScene!");
                } else {
                    ProxyLog("[ENDSCENE] ERROR: ImGui initialization FAILED!");
                }
            }
        } else {
            ProxyLog("[ENDSCENE] ERROR: GetCreationParameters failed!");
        }
    }
    
    // NOTE: With the new approach, we keep the backbuffer at native resolution (640x480)
    // and let D3D9's Present stretch it to fill the window. No EndFrame scaling needed.
    // The letterbox scaler's EndFrame is disabled - D3D9 handles scaling natively.
    
    // Call mod's OnFrame function EVERY frame (not just when menu is shown)
    // This handles hotkeys, per-frame mod logic, netplay updates, etc.
    if (g_pModOnFrame) {
        InvokeModOnFrameSafely();
    }
    
    // Render ImGui overlay (after game rendering, on the native backbuffer)
    if (g_imguiInitialized) {
        RenderImGui();
    }

    // === HUD rendering ===
    // Match HUD (names, ping, delay, rollback) is now rendered via ImGui in
    // the mod DLL (NetplayHud_Render called from ModOnPresent).
    // The old D3D9 bitmap font HUD has been retired.
    
    // Log every 600 frames (roughly every 10 seconds at 60fps)
    frameCount++;
    if (frameCount % 600 == 0) {
        ProxyLog("[ENDSCENE] Frame %d, ImGui=%s, Menu=%s, Scaler=%s", 
                 frameCount, 
                 g_imguiInitialized ? "OK" : "NO", 
                 g_showMenu ? "ON" : "OFF",
                 g_letterboxScaler.IsEnabled() ? "ON" : "OFF");
    }
    
    if (firstCall) {
        firstCall = false;
    }
    
    return g_pOriginalEndScene(pDevice);
}

// ============================================================================
// Reset Hook (handle device lost/reset)
// ============================================================================

HRESULT WINAPI HookedReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    // If this is our own reset call, skip the resource cleanup (we handle it ourselves)
    if (g_internalReset) {
        ProxyLog("[RESET] Internal reset - passing through");
        Hud_Release();
        return g_pOriginalReset(pDevice, pPresentationParameters);
    }
    
    ProxyLog("[RESET] Reset called - invalidating resources");
    ProxyLog("[RESET]   BackBuffer: %dx%d, Windowed=%d", 
             pPresentationParameters->BackBufferWidth,
             pPresentationParameters->BackBufferHeight,
             pPresentationParameters->Windowed);
    
    // IMPORTANT: Keep backbuffer at native resolution (640x480)!
    // The scaling swap chain handles the scaling to window size.
    // Do NOT enlarge the backbuffer here.
    if (g_isCurrentlyBorderless) {
        pPresentationParameters->Windowed = TRUE;
        pPresentationParameters->FullScreen_RefreshRateInHz = 0;
        ProxyLog("[RESET] Keeping backbuffer at native %dx%d for borderless mode",
                 pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight);
    }
    
    if (g_imguiInitialized) {
        ProxyLog("[RESET] Invalidating ImGui device objects");
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    // Release HUD resources (D3DPOOL_DEFAULT)
    Hud_Release();
    
    // Release cached backbuffer (invalidated by Reset)
    ReleaseCachedGameBackBuffer();
    
    // Release scaling swap chain (MUST be released before Reset)
    ReleaseScalingSwapChain();
    
    // Release scaler resources (new system)
    g_letterboxScaler.Release();
    
    // Release old letterbox resources (legacy)
    ReleaseLetterboxing();
    
    HRESULT hr = g_pOriginalReset(pDevice, pPresentationParameters);
    ProxyLog("[RESET] Original Reset returned: 0x%08X", hr);
    
    if (SUCCEEDED(hr)) {
        g_deviceLost = false;  // Device recovered after successful Reset
        
        if (g_imguiInitialized) {
            ProxyLog("[RESET] Recreating ImGui device objects");
            ImGui_ImplDX9_CreateDeviceObjects();
        }
        
        // Refresh cached backbuffer
        RefreshCachedGameBackBuffer(pDevice);
        
        // Reinitialize scaling swap chain after Reset
        if (g_isCurrentlyBorderless && g_keepAspectRatio && g_gameWindow) {
            InitializeScalingSwapChain(pDevice, g_gameWindow);
        }
        
        ProxyLog("[RESET] Reset succeeded");
    } else {
        ProxyLog("[RESET] ERROR: Reset failed!");
    }
    
    return hr;
}

// ============================================================================
// Present Hook (for scaling via secondary swap chain)
// ============================================================================
// Architecture:
//   1. Game renders to 640x480 backbuffer (DXLib works correctly)
//   2. We StretchRect from 640x480 to our 1920x1440 scaling swap chain
//   3. We Present the scaling swap chain (fills the borderless window)
//   4. We skip the original Present (it would just show 640x480 in corner)

HRESULT WINAPI HookedPresent(IDirect3DDevice9* pDevice, 
                              const RECT* pSourceRect, 
                              const RECT* pDestRect,
                              HWND hDestWindowOverride, 
                              const RGNDATA* pDirtyRegion) {
    UpdateStartupStageSilently("IDirect3DDevice9::Present");
    g_presentCallCount++;
    TryProcessDeferredFocusReclaim();
    
    // --- Device-lost recovery ---
    if (g_deviceLost) {
        if (!TryRecoverFromDeviceLost(pDevice)) {
            Sleep(50);  // Throttle while device is lost
            return D3DERR_DEVICELOST;
        }
        // Recovery succeeded — fall through to normal present
    }
    
    // --- Minimize throttle: skip all rendering when minimized ---
    if (g_isMinimized) {
        Sleep(16);  // ~60 Hz throttle to avoid burning CPU
        return D3D_OK;
    }
    
    // Process any pending reset requests FIRST (safe time - between frames)
    if (g_pendingReset && g_hasSavedPresentParams) {
        ProxyLog("[PRESENT] Processing pending reset request...");
        g_pendingReset = false;
        PerformDeferredReset(pDevice);
    }
    
    // Log state on first few frames and periodically for debugging
    bool shouldLogState = (g_presentCallCount <= 10) || (g_presentCallCount % 300 == 0);
    if (shouldLogState) {
        ProxyLog("[PRESENT] Frame %d state: borderless=%d, scalingInit=%d, chain=0x%p, buffer=0x%p",
                 g_presentCallCount, g_isCurrentlyBorderless, g_scalingInitialized,
                 g_pScalingSwapChain, g_pScalingBackBuffer);
    }
    
    // Initialize scaling if needed (when borderless is active but scaling not yet initialized)
    // This handles both initial setup AND re-enabling after borderless was toggled off
    if (g_useBorderlessFullscreen && g_isCurrentlyBorderless && !g_scalingInitialized) {
        // Use the parent window (root window) which is the actual borderless window
        HWND hScalingWindow = g_gameParentWindow ? g_gameParentWindow : g_gameWindow;
        
        ProxyLog("[PRESENT] *** SCALING NOT INITIALIZED - initializing now ***");
        ProxyLog("[PRESENT]   Frame: %d", g_presentCallCount);
        ProxyLog("[PRESENT]   g_gameWindow=0x%p, g_gameParentWindow=0x%p, using=0x%p",
                 g_gameWindow, g_gameParentWindow, hScalingWindow);
        ProxyLog("[PRESENT]   g_screenWidth=%d, g_screenHeight=%d", g_screenWidth, g_screenHeight);
        
        if (hScalingWindow && g_screenWidth > g_nativeWidth && g_screenHeight > g_nativeHeight) {
            InitializeScalingSwapChain(pDevice, hScalingWindow);
        } else {
            ProxyLog("[PRESENT] WARNING: Cannot initialize scaling - window=0x%p, screen=%dx%d, native=%dx%d",
                     hScalingWindow, g_screenWidth, g_screenHeight, g_nativeWidth, g_nativeHeight);
        }
        
        g_needsInitialBorderless = false;  // Clear this flag since we're handling it
        g_pendingBorderlessWindow = nullptr;
    }
    
    // Handle the legacy deferred borderless flow (if g_pendingBorderlessWindow was set in EndScene)
    if (g_needsInitialBorderless && g_useBorderlessFullscreen && g_pendingBorderlessWindow) {
        if (ShouldDelayInitialBorderlessForFontInit()) {
            return g_pOriginalPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        }

        ProxyLog("[PRESENT] Applying deferred borderless fullscreen (legacy flow)...");
        
        // First, present the current frame normally
        HRESULT presentHr = g_pOriginalPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        
        // Now apply borderless
        ApplyBorderlessFullscreen(g_pendingBorderlessWindow, pDevice);
        g_needsInitialBorderless = false;
        g_pendingBorderlessWindow = nullptr;
        
        // Initialize scaling swap chain (the key to making this work!)
        // Use parent window which is the actual borderless window
        HWND hScalingWindow = g_gameParentWindow ? g_gameParentWindow : g_gameWindow;
        if (g_keepAspectRatio && hScalingWindow) {
            InitializeScalingSwapChain(pDevice, hScalingWindow);
        }
        
        // Return the result of the first present
        return presentHr;
    }
    
    // Log first call and errors only
    bool shouldLog = (g_presentCallCount == 1);
    
    if (shouldLog) {
        ProxyLog("[PRESENT] Device Present active, scaling=%s", g_scalingInitialized ? "yes" : "no");
    }
    
    // If scaling is active, do the StretchRect + Present on scaling chain
    if (g_scalingInitialized && g_pScalingSwapChain && g_pScalingBackBuffer && g_isCurrentlyBorderless) {
        HRESULT hr;
        
        // Use cached game backbuffer (refreshed on Reset / scaling init only)
        IDirect3DSurface9* pGameBackBuffer = g_pCachedGameBackBuffer;
        if (!pGameBackBuffer) {
            // Fallback: cache miss, refresh now
            RefreshCachedGameBackBuffer(pDevice);
            pGameBackBuffer = g_pCachedGameBackBuffer;
        }
        
        if (pGameBackBuffer) {
            // Log only first scaling operation
            if (g_presentCallCount == 1) {
                D3DSURFACE_DESC srcDesc, dstDesc;
                pGameBackBuffer->GetDesc(&srcDesc);
                g_pScalingBackBuffer->GetDesc(&dstDesc);
                ProxyLog("[SCALING] Active: %dx%d -> %dx%d",
                         srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height);
            }
            
            // Only ColorFill the letterbox/pillarbox bar regions, not the whole surface.
            // When the dest rect covers the entire scaling backbuffer (4:3 monitor),
            // no clear is needed at all.
            {
                const RECT& dr = g_letterboxDestRect;
                bool fullCoverage = (dr.left == 0 && dr.top == 0 &&
                                     dr.right == g_screenWidth && dr.bottom == g_screenHeight);
                if (!fullCoverage) {
                    pDevice->ColorFill(g_pScalingBackBuffer, nullptr, D3DCOLOR_XRGB(0, 0, 0));
                }
            }
            
            // Use the cached letterbox destination rect (recalculated on resize)
            RECT srcRect = {0, 0, g_nativeWidth, g_nativeHeight};
            
            // StretchRect from game backbuffer to scaling backbuffer
            if (shouldLog) {
                ProxyLog("[PRESENT]   StretchRect: src(%d,%d,%d,%d) -> dst(%d,%d,%d,%d)",
                         srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
                         g_letterboxDestRect.left, g_letterboxDestRect.top, 
                         g_letterboxDestRect.right, g_letterboxDestRect.bottom);
            }
            
            hr = pDevice->StretchRect(pGameBackBuffer, &srcRect, g_pScalingBackBuffer, &g_letterboxDestRect, D3DTEXF_LINEAR);
            if (FAILED(hr)) {
                ProxyLog("[PRESENT] ERROR: StretchRect failed: 0x%08X", hr);
                if (hr == D3DERR_INVALIDCALL) ProxyLog("[PRESENT]   D3DERR_INVALIDCALL - surfaces incompatible?");
                if (hr == D3DERR_DEVICELOST) {
                    g_deviceLost = true;
                    return hr;
                }
                // Fall through to try original present
                return g_pOriginalPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
            }

            if (g_imguiDrawDataReady) {
                D3DSURFACE_DESC scalingDesc = {};
                if (SUCCEEDED(g_pScalingBackBuffer->GetDesc(&scalingDesc))) {
                    RenderPreparedImGuiToScalingTarget(
                        pDevice,
                        g_pScalingBackBuffer,
                        (int)scalingDesc.Width,
                        (int)scalingDesc.Height,
                        g_letterboxDestRect);
                } else {
                    g_imguiDrawDataReady = false;
                }
            }
            
            // Present the scaling swap chain (this fills the window!)
            if (shouldLog) {
                ProxyLog("[PRESENT]   Presenting scaling swap chain 0x%p...", g_pScalingSwapChain);
            }
            
            hr = g_pScalingSwapChain->Present(nullptr, nullptr, nullptr, nullptr, 0);
            if (FAILED(hr)) {
                ProxyLog("[PRESENT] ERROR: Scaling chain Present failed: 0x%08X", hr);
                if (hr == D3DERR_DEVICELOST) {
                    g_deviceLost = true;
                    return hr;
                }
                // Fall through to try original present
                return g_pOriginalPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
            }
            
            if (shouldLog) {
                ProxyLog("[PRESENT] SUCCESS: Scaled %dx%d -> %dx%d",
                         g_nativeWidth, g_nativeHeight, g_screenWidth, g_screenHeight);
                ProxyLog("[PRESENT]   Dest rect: (%d,%d) to (%d,%d)",
                         g_letterboxDestRect.left, g_letterboxDestRect.top, 
                         g_letterboxDestRect.right, g_letterboxDestRect.bottom);
            }
            
            // DO NOT call original Present - we've already presented via the scaling chain
            // The game's 640x480 backbuffer stays as-is for DXLib to keep working properly
            return D3D_OK;
        } else {
            if (shouldLog) {
                ProxyLog("[PRESENT] ERROR: No game backbuffer available for scaling");
            }
        }
    }
    
    // Fallback: just call original present
    // Log why we're falling back (for debugging)
    if (shouldLogState && g_useBorderlessFullscreen) {
        ProxyLog("[PRESENT] NOT using scaling - falling back to original Present");
        ProxyLog("[PRESENT]   Reason: scalingInit=%d, chain=0x%p, buffer=0x%p, borderless=%d",
                 g_scalingInitialized, g_pScalingSwapChain, g_pScalingBackBuffer, g_isCurrentlyBorderless);
    }
    HRESULT hr = g_pOriginalPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    if (hr == D3DERR_DEVICELOST) {
        g_deviceLost = true;
    }
    return hr;
}

// ============================================================================
// Swap Chain Present Hook
// ============================================================================
// DXLib may call IDirect3DSwapChain9::Present directly instead of IDirect3DDevice9::Present
// This hook intercepts those calls to ensure our scaling happens

static int g_swapChainPresentCount = 0;

HRESULT WINAPI HookedSwapChainPresent(IDirect3DSwapChain9* pSwapChain,
                                       const RECT* pSourceRect,
                                       const RECT* pDestRect,
                                       HWND hDestWindowOverride,
                                       const RGNDATA* pDirtyRegion,
                                       DWORD dwFlags) {
    g_swapChainPresentCount++;
    
    // Log only first few calls
    bool shouldLog = (g_swapChainPresentCount <= 3);
    
    // Check if this is our scaling swap chain - let it through
    if (pSwapChain == g_pScalingSwapChain) {
        return g_pOriginalSwapChainPresent(pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }
    
    // --- Device-lost recovery ---
    if (g_deviceLost) {
        // Get device to test cooperative level
        IDirect3DDevice9* pDev = nullptr;
        pSwapChain->GetDevice(&pDev);
        if (pDev) {
            if (!TryRecoverFromDeviceLost(pDev)) {
                pDev->Release();
                Sleep(50);
                return D3DERR_DEVICELOST;
            }
            pDev->Release();
        }
    }
    
    // --- Minimize throttle ---
    if (g_isMinimized) {
        Sleep(16);
        return D3D_OK;
    }
    
    // Get the device from the swap chain (needed for several operations)
    IDirect3DDevice9* pDevice = nullptr;
    HRESULT hr = pSwapChain->GetDevice(&pDevice);
    if (FAILED(hr) || !pDevice) {
        return g_pOriginalSwapChainPresent(pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }
    
    // Check if we need to reinitialize scaling due to window resize (windowed mode)
    if (g_windowResizedNeedsReinit && !g_isCurrentlyBorderless) {
        g_windowResizedNeedsReinit = false;
        
        ProxyLog("[SWAPCHAIN] Reinitializing scaling for window resize: %dx%d", 
                 g_currentWindowWidth, g_currentWindowHeight);
        
        // Update screen dimensions for windowed mode scaling
        g_screenWidth = g_currentWindowWidth;
        g_screenHeight = g_currentWindowHeight;
        
        // Reinitialize scaling swap chain with new size
        if (g_gameWindow) {
            InitializeScalingSwapChain(pDevice, g_gameWindow);
        }
    }
    
    // Check if we need to reinitialize scaling for borderless mode
    if (g_isCurrentlyBorderless && !g_scalingInitialized && g_gameWindow) {
        ProxyLog("[SWAPCHAIN] Borderless mode active but scaling not initialized - initializing now");
        InitializeScalingSwapChain(pDevice, g_gameWindow);
    }
    
    // Determine if we should do scaling:
    // - Borderless mode with scaling initialized, OR
    // - Windowed mode with window larger than native
    bool shouldScale = false;
    int destWidth = g_screenWidth;
    int destHeight = g_screenHeight;
    
    if (g_isCurrentlyBorderless) {
        // Borderless mode - use borderless dimensions
        shouldScale = g_scalingInitialized && g_pScalingSwapChain && g_pScalingBackBuffer;
    } else {
        // Windowed mode - check if window is larger than native
        if (g_currentWindowWidth > g_nativeWidth || g_currentWindowHeight > g_nativeHeight) {
            // Need to initialize scaling if not already done
            if (!g_scalingInitialized || !g_pScalingSwapChain || !g_pScalingBackBuffer) {
                if (g_gameWindow) {
                    g_screenWidth = g_currentWindowWidth;
                    g_screenHeight = g_currentWindowHeight;
                    if (shouldLog) {
                        ProxyLog("[SWAPCHAIN] Initializing windowed scaling: %dx%d", g_screenWidth, g_screenHeight);
                    }
                    InitializeScalingSwapChain(pDevice, g_gameWindow);
                }
            }
            shouldScale = g_scalingInitialized && g_pScalingSwapChain && g_pScalingBackBuffer;
            destWidth = g_currentWindowWidth;
            destHeight = g_currentWindowHeight;
        }
    }
    
    // This is the implicit/game swap chain trying to present!
    // If scaling is active, we need to intercept this and do our scaling instead
    if (shouldScale) {
        // Use cached backbuffer when available; fall back to swap chain's own backbuffer
        IDirect3DSurface9* pGameBackBuffer = g_pCachedGameBackBuffer;
        bool needRelease = false;
        if (!pGameBackBuffer) {
            hr = pSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pGameBackBuffer);
            if (FAILED(hr) || !pGameBackBuffer) {
                pDevice->Release();
                return g_pOriginalSwapChainPresent(pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
            }
            needRelease = true;
        }
        
        // Log scaling info once
        if (shouldLog) {
            D3DSURFACE_DESC srcDesc, dstDesc;
            pGameBackBuffer->GetDesc(&srcDesc);
            g_pScalingBackBuffer->GetDesc(&dstDesc);
            ProxyLog("[SCALING] SwapChain scaling: %dx%d -> %dx%d", 
                     srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height);
        }
        
        // Only clear when letterbox/pillarbox bars would be visible
        {
            const RECT& dr = g_letterboxDestRect;
            bool fullCoverage = (dr.left == 0 && dr.top == 0 &&
                                 dr.right == destWidth && dr.bottom == destHeight);
            if (!fullCoverage) {
                pDevice->ColorFill(g_pScalingBackBuffer, nullptr, D3DCOLOR_XRGB(0, 0, 0));
            }
        }
        
        // Use cached letterbox dest rect
        RECT srcRect = {0, 0, g_nativeWidth, g_nativeHeight};
        
        hr = pDevice->StretchRect(pGameBackBuffer, &srcRect, g_pScalingBackBuffer, &g_letterboxDestRect, D3DTEXF_LINEAR);
        if (FAILED(hr)) {
            ProxyLog("[SWAPCHAIN] ERROR: StretchRect failed: 0x%08X", hr);
            if (hr == D3DERR_DEVICELOST) {
                g_deviceLost = true;
            }
        }

        if (SUCCEEDED(hr) && g_imguiDrawDataReady) {
            D3DSURFACE_DESC scalingDesc = {};
            if (SUCCEEDED(g_pScalingBackBuffer->GetDesc(&scalingDesc))) {
                RenderPreparedImGuiToScalingTarget(
                    pDevice,
                    g_pScalingBackBuffer,
                    (int)scalingDesc.Width,
                    (int)scalingDesc.Height,
                    g_letterboxDestRect);
            } else {
                g_imguiDrawDataReady = false;
            }
        }
        
        if (needRelease) pGameBackBuffer->Release();
        
        // Present the scaling swap chain
        hr = g_pScalingSwapChain->Present(nullptr, nullptr, nullptr, nullptr, 0);
        if (FAILED(hr)) {
            ProxyLog("[ERROR] Scaling chain Present failed: 0x%08X", hr);
            if (hr == D3DERR_DEVICELOST) {
                g_deviceLost = true;
            }
        }
        
        pDevice->Release();
        return D3D_OK;  // Skip original Present
    }
    
    pDevice->Release();
    
    // Fallback: call original (no scaling)
    hr = g_pOriginalSwapChainPresent(pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    if (hr == D3DERR_DEVICELOST) {
        g_deviceLost = true;
    }
    return hr;
}

// ============================================================================
// Window Procedure Hook (Subclassing)
// ============================================================================
LRESULT CALLBACK ProxyWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    LoadInputGuardSettings();
    if (g_enableHotkeyTraceLogs) {
        LogShellHotkeyTraceState("ProxyWndProc-enter", hWnd, uMsg, wParam, lParam);
    }
    const bool traceHotkeyWndproc = ShouldLogHotkeyTraceWndproc(uMsg, wParam);
    LogMouseHoverTraceState("ProxyWndProc-enter", hWnd, uMsg, wParam, lParam, nullptr);

    if (g_enableSwallowTraceLogs && IsShellRelatedMessage(uMsg, wParam)) {
        ProxyLog("[SWALLOW-TRACE][ProxyWndProc-leak] shell msg entering vanilla path %s suppress=%d",
                 DescribeShellHotkeyTraceMessage(uMsg, wParam),
                 ReadShellSuppressFlag());
    }

    // Track minimize / restore for rendering throttle and device-lost handling
    if (uMsg == WM_SIZE) {
        if (wParam == SIZE_MINIMIZED) {
            if (!g_isMinimized) {
                g_isMinimized = true;
                ProxyLog("[WNDPROC] Window minimized — rendering paused");
            }
        } else if (g_isMinimized) {
            g_isMinimized = false;
            ProxyLog("[WNDPROC] Window restored from minimize");
        }
    }

    if (uMsg == WM_ACTIVATEAPP && wParam == FALSE) {
        ResetOverlayHotkeyState("ProxyWndProc", "WM_ACTIVATEAPP deactivate", hWnd, uMsg);
        ReleaseGameMouseCapture(hWnd);
    }

    if (HandleOverlayHotkeys(hWnd, uMsg, wParam, lParam, "ProxyWndProc")) {
        const LRESULT overlayResult = 0;
        LogMouseHoverTraceState("ProxyWndProc-overlay-return", hWnd, uMsg, wParam, lParam, &overlayResult);
        return 0;
    }

    // If we are in borderless mode, we want to prevent the game from restoring window styles
    if (g_useBorderlessFullscreen && g_isCurrentlyBorderless) {
        if (uMsg == WM_STYLECHANGING) {
            STYLESTRUCT* ss = (STYLESTRUCT*)lParam;
            if (wParam == GWL_STYLE) {
                // Force popup style (no caption, no thickframe)
                ss->styleNew &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
                ss->styleNew |= WS_POPUP;
            } else if (wParam == GWL_EXSTYLE) {
                // Force no edge styles
                ss->styleNew &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
            }
            return 0;
        }
        
        // Prevent window from being resized by the game logic if it tries to enforce 640x480
        if (uMsg == WM_WINDOWPOSCHANGING) {
            WINDOWPOS* wp = (WINDOWPOS*)lParam;
            if (!(wp->flags & SWP_NOSIZE)) {
                if (wp->cx != g_screenWidth || wp->cy != g_screenHeight) {
                    wp->cx = g_screenWidth;
                    wp->cy = g_screenHeight;
                    wp->flags |= SWP_NOMOVE;
                }
            }
        }
    }

    if (uMsg == WM_SIZE) {
        int newWidth = LOWORD(lParam);
        int newHeight = HIWORD(lParam);
        
        // Handle maximize as borderless-like behavior
        if (wParam == SIZE_MAXIMIZED && !g_isCurrentlyBorderless) {
            ProxyLog("[RESIZE] Window maximized: %dx%d - treating as borderless", newWidth, newHeight);
            g_currentWindowWidth = newWidth;
            g_currentWindowHeight = newHeight;
            g_screenWidth = newWidth;
            g_screenHeight = newHeight;
            g_windowResizedNeedsReinit = true;
        }
        // Track window size changes for windowed mode scaling
        else if (!g_isCurrentlyBorderless && wParam != SIZE_MINIMIZED) {
            if (newWidth != g_currentWindowWidth || newHeight != g_currentWindowHeight) {
                ProxyLog("[RESIZE] Window: %dx%d -> %dx%d (wParam=%d)", 
                         g_currentWindowWidth, g_currentWindowHeight, newWidth, newHeight, wParam);
                g_currentWindowWidth = newWidth;
                g_currentWindowHeight = newHeight;
                g_screenWidth = newWidth;
                g_screenHeight = newHeight;
                
                if (newWidth > g_nativeWidth || newHeight > g_nativeHeight) {
                    g_windowResizedNeedsReinit = true;
                    ProxyLog("[RESIZE] Flagged for scaling reinit");
                } else if (g_scalingInitialized) {
                    ProxyLog("[RESIZE] At native size, releasing scaling");
                    ReleaseScalingSwapChain();
                }
            }
        }
    }
    
    // Constrain window resize to 4:3 aspect ratio (windowed mode only)
    if (uMsg == WM_SIZING && !g_isCurrentlyBorderless) {
        ProxyLog("[WM_SIZING] Edge=%d, borderless=%d", wParam, g_isCurrentlyBorderless);
        
        RECT* rect = (RECT*)lParam;
        int width = rect->right - rect->left;
        int height = rect->bottom - rect->top;
        
        // Get window border sizes
        RECT clientRect, windowRect;
        GetClientRect(hWnd, &clientRect);
        GetWindowRect(hWnd, &windowRect);
        int borderWidth = (windowRect.right - windowRect.left) - (clientRect.right - clientRect.left);
        int borderHeight = (windowRect.bottom - windowRect.top) - (clientRect.bottom - clientRect.top);
        
        // Calculate client size
        int clientWidth = width - borderWidth;
        int clientHeight = height - borderHeight;
        
        ProxyLog("[WM_SIZING] Window: %dx%d, Client: %dx%d, Borders: %dx%d", 
                 width, height, clientWidth, clientHeight, borderWidth, borderHeight);
        
        // Enforce 4:3 aspect ratio
        float targetAspect = 4.0f / 3.0f;
        
        // Determine which dimension to adjust based on resize edge
        if (wParam == WMSZ_LEFT || wParam == WMSZ_RIGHT) {
            // Horizontal resize - adjust height
            clientHeight = (int)(clientWidth / targetAspect);
        } else if (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM) {
            // Vertical resize - adjust width
            clientWidth = (int)(clientHeight * targetAspect);
        } else {
            // Corner resize - use the larger dimension
            int newHeight = (int)(clientWidth / targetAspect);
            int newWidth = (int)(clientHeight * targetAspect);
            
            if (newHeight > clientHeight) {
                clientHeight = newHeight;
            } else {
                clientWidth = newWidth;
            }
        }
        
        // Enforce minimum size
        if (clientWidth < g_nativeWidth) {
            clientWidth = g_nativeWidth;
            clientHeight = g_nativeHeight;
        }
        
        // Apply new size
        int newWidth = clientWidth + borderWidth;
        int newHeight = clientHeight + borderHeight;
        
        // Adjust the appropriate edges
        switch (wParam) {
            case WMSZ_LEFT:
            case WMSZ_TOPLEFT:
            case WMSZ_BOTTOMLEFT:
                rect->left = rect->right - newWidth;
                break;
            default:
                rect->right = rect->left + newWidth;
                break;
        }
        switch (wParam) {
            case WMSZ_TOP:
            case WMSZ_TOPLEFT:
            case WMSZ_TOPRIGHT:
                rect->top = rect->bottom - newHeight;
                break;
            default:
                rect->bottom = rect->top + newHeight;
                break;
        }
        
        return TRUE;
    }
    
    // Handle maximize to fill screen with letterboxing
    if (uMsg == WM_GETMINMAXINFO && !g_isCurrentlyBorderless) {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        
        // Get the monitor the window is on
        HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(hMon, &mi)) {
            // Set maximized size to fill the work area
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            mmi->ptMaxPosition.x = mi.rcWork.left;
            mmi->ptMaxPosition.y = mi.rcWork.top;
        }
        
        // Set minimum tracking size to native resolution
        RECT clientRect = {0, 0, g_nativeWidth, g_nativeHeight};
        AdjustWindowRectEx(&clientRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = clientRect.right - clientRect.left;
        mmi->ptMinTrackSize.y = clientRect.bottom - clientRect.top;
    }
    
    if (uMsg == WM_CLOSE) {
        ProxyLog("[WNDPROC] WM_CLOSE received - game exiting normally");
        NotifyGameExitOnce(0, "WM_CLOSE - Normal exit");
    }
    if (uMsg == WM_DESTROY) {
        ProxyLog("[WNDPROC] WM_DESTROY received");
        ResetOverlayHotkeyState("ProxyWndProc", "WM_DESTROY", hWnd, uMsg);
        NotifyGameExitOnce(0, "WM_DESTROY");
        PostQuitMessageOnce("ProxyWndProc WM_DESTROY");
    }
    if (uMsg == WM_NCDESTROY) {
        ProxyLog("[WNDPROC] WM_NCDESTROY received");
        ResetOverlayHotkeyState("ProxyWndProc", "WM_NCDESTROY", hWnd, uMsg);
        NotifyGameExitOnce(0, "WM_NCDESTROY");
        PostQuitMessageOnce("ProxyWndProc WM_NCDESTROY");
    }

    // CallWindowProc can handle both function pointers and class atoms
    // It will correctly dispatch to the original procedure
    if (g_proxyOriginalWndProc) {
        LRESULT result = CallGameWndProcChain(
            hWnd, uMsg, wParam, lParam, g_proxyOriginalWndProc, "ProxyWndProc", traceHotkeyWndproc);
        if (traceHotkeyWndproc) {
            ProxyLog("[HOTKEYTRACE][ProxyWndProc-exit] %s result=0x%p original=0x%p",
                     DescribeShellHotkeyTraceMessage(uMsg, wParam),
                     (void*)result,
                     g_proxyOriginalWndProc);
        }
        LogMouseHoverTraceState("ProxyWndProc-game-exit", hWnd, uMsg, wParam, lParam, &result);
        LogSwallowTraceWndproc("ProxyWndProc-game-exit", uMsg, wParam, lParam, result, true);
        return result;
    }
    
    LRESULT result = DefWindowProcW(hWnd, uMsg, wParam, lParam);
    if (traceHotkeyWndproc) {
        ProxyLog("[HOTKEYTRACE][ProxyWndProc-fallback] %s result=0x%p",
                 DescribeShellHotkeyTraceMessage(uMsg, wParam),
                 (void*)result);
    }
    LogMouseHoverTraceState("ProxyWndProc-fallback", hWnd, uMsg, wParam, lParam, &result);
    LogSwallowTraceWndproc("ProxyWndProc-fallback", uMsg, wParam, lParam, result, false);
    return result;
}

// ============================================================================
// SetWindowPos Hook (prevent game from resizing window in borderless mode)
// ============================================================================

BOOL WINAPI HookedSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
    // Check if window belongs to current process
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return g_pOriginalSetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
    }
    
    // Ignore console window
    if (hWnd == GetConsoleWindow()) {
        return g_pOriginalSetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
    }

    // If we are in borderless mode, prevent resizing/moving
    if (g_isCurrentlyBorderless && !g_internalResize) {
        bool isResizing = !(uFlags & SWP_NOSIZE);
        bool isMoving = !(uFlags & SWP_NOMOVE);
        
        if (isResizing || isMoving) {
            uFlags |= (SWP_NOSIZE | SWP_NOMOVE);
        }
        uFlags |= SWP_NOZORDER;
    }
    
    return g_pOriginalSetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

// ============================================================================
// SetWindowLongA Hook (prevent game from restoring window borders)
// ============================================================================

LONG WINAPI HookedSetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong) {
    // Check if window belongs to current process
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return g_pOriginalSetWindowLongA(hWnd, nIndex, dwNewLong);
    }
    
    // Ignore console window
    if (hWnd == GetConsoleWindow()) {
        return g_pOriginalSetWindowLongA(hWnd, nIndex, dwNewLong);
    }

    // Log the call
    if (nIndex == GWL_WNDPROC) {
        ProxyLog("[HOOK] SetWindowLongA GWL_WNDPROC hwnd=0x%p new=0x%08X currentBefore=0x%p",
                 hWnd, dwNewLong, hWnd ? (void*)GetWindowLongPtrW(hWnd, GWLP_WNDPROC) : nullptr);
    }

    if (!g_internalResize && (nIndex == GWL_STYLE || nIndex == GWL_EXSTYLE)) {
        ProxyLog("[HOOK] SetWindowLongA(0x%p, %d) NewVal=0x%08X", hWnd, nIndex, dwNewLong);
    }

    // If we are in borderless mode
    if (g_isCurrentlyBorderless && !g_internalResize) {
        // If setting style (GWL_STYLE = -16)
        if (nIndex == GWL_STYLE) {
            ProxyLog("[HOOK] Blocked SetWindowLongA GWL_STYLE change in borderless mode");
            // Force POPUP style
            dwNewLong = WS_POPUP | WS_VISIBLE;
        }
        // If setting extended style (GWL_EXSTYLE = -20)
        else if (nIndex == GWL_EXSTYLE) {
            ProxyLog("[HOOK] Blocked SetWindowLongA GWL_EXSTYLE change in borderless mode");
            // Force APPWINDOW style
            dwNewLong = WS_EX_APPWINDOW;
        }
    }
    
    LONG previousValue = g_pOriginalSetWindowLongA(hWnd, nIndex, dwNewLong);
    if (nIndex == GWL_WNDPROC) {
        ProxyLog("[HOOK] SetWindowLongA GWL_WNDPROC previous=0x%08X currentAfter=0x%p",
                 previousValue,
                 hWnd ? (void*)GetWindowLongPtrW(hWnd, GWLP_WNDPROC) : nullptr);
    }
    return previousValue;
}

// ============================================================================
// SetWindowLongW Hook (Unicode version)
// ============================================================================

LONG WINAPI HookedSetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong) {
    // Check if window belongs to current process
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return g_pOriginalSetWindowLongW(hWnd, nIndex, dwNewLong);
    }
    
    // Ignore console window
    if (hWnd == GetConsoleWindow()) {
        return g_pOriginalSetWindowLongW(hWnd, nIndex, dwNewLong);
    }

    if (nIndex == GWL_WNDPROC) {
        ProxyLog("[HOOK] SetWindowLongW GWL_WNDPROC hwnd=0x%p new=0x%08X currentBefore=0x%p",
                 hWnd, dwNewLong, hWnd ? (void*)GetWindowLongPtrW(hWnd, GWLP_WNDPROC) : nullptr);
    }

    // If we are in borderless mode
    if (g_isCurrentlyBorderless && !g_internalResize) {
        // If setting style (GWL_STYLE = -16)
        if (nIndex == GWL_STYLE) {
            // Force POPUP style
            dwNewLong = WS_POPUP | WS_VISIBLE;
        }
        // If setting extended style (GWL_EXSTYLE = -20)
        else if (nIndex == GWL_EXSTYLE) {
            // Force APPWINDOW style
            dwNewLong = WS_EX_APPWINDOW;
        }
    }
    
    LONG previousValue = g_pOriginalSetWindowLongW(hWnd, nIndex, dwNewLong);
    if (nIndex == GWL_WNDPROC) {
        ProxyLog("[HOOK] SetWindowLongW GWL_WNDPROC previous=0x%08X currentAfter=0x%p",
                 previousValue,
                 hWnd ? (void*)GetWindowLongPtrW(hWnd, GWLP_WNDPROC) : nullptr);
    }
    return previousValue;
}

// ============================================================================
// SetWindowRgn Hook (prevent game from clipping window content)
// ============================================================================

int WINAPI HookedSetWindowRgn(HWND hWnd, HRGN hRgn, BOOL bRedraw) {
    // Check if window belongs to current process
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return g_pOriginalSetWindowRgn(hWnd, hRgn, bRedraw);
    }
    
    // Ignore console window
    if (hWnd == GetConsoleWindow()) {
        return g_pOriginalSetWindowRgn(hWnd, hRgn, bRedraw);
    }

    // If we are in borderless mode
    if (g_isCurrentlyBorderless && !g_internalResize) {
        // Block setting the region
        return 1; // Success
    }
    
    return g_pOriginalSetWindowRgn(hWnd, hRgn, bRedraw);
}

// ============================================================================
// SetMenu Hook (prevent game from showing menu in borderless mode)
// ============================================================================

BOOL WINAPI HookedSetMenu(HWND hWnd, HMENU hMenu) {
    // Check if window belongs to current process
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return g_pOriginalSetMenu(hWnd, hMenu);
    }
    
    // If we are in borderless mode
    if (g_isCurrentlyBorderless && !g_internalResize) {
        // If trying to set a menu (hMenu != NULL)
        if (hMenu != NULL) {
            return TRUE; // Pretend we did it
        }
    }
    
    return g_pOriginalSetMenu(hWnd, hMenu);
}

// ============================================================================
// Install hooks on a D3D9 device (called when game creates its device)
// ============================================================================

bool InstallDeviceHooks(IDirect3DDevice9* pDevice) {
    if (g_HooksInstalled) {
        ProxyLog("[HOOK] Hooks already installed");
        return true;
    }
    
    ProxyLog("[HOOK] Installing hooks on device: 0x%p", pDevice);
    
    // Initialize MinHook
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        ProxyLog("[HOOK] ERROR: MH_Initialize failed: %s", MH_StatusToString(mhStatus));
        return false;
    }
    
    // Get vtable
    void** vTable = *(void***)pDevice;
    ProxyLog("[HOOK] Device vtable: 0x%p", vTable);
    
    // BeginScene is at index 41
    g_BeginSceneTarget = vTable[41];
    ProxyLog("[HOOK] BeginScene address: 0x%p", g_BeginSceneTarget);
    
    // EndScene is at index 42
    g_EndSceneTarget = vTable[42];
    ProxyLog("[HOOK] EndScene address: 0x%p", g_EndSceneTarget);
    
    // Reset is at index 16
    g_ResetTarget = vTable[16];
    ProxyLog("[HOOK] Reset address: 0x%p", g_ResetTarget);
    
    // Present is at index 17
    g_PresentTarget = vTable[17];
    ProxyLog("[HOOK] Present address: 0x%p", g_PresentTarget);
    
    // SetRenderTarget is at index 37
    g_SetRenderTargetTarget = vTable[37];
    ProxyLog("[HOOK] SetRenderTarget address: 0x%p", g_SetRenderTargetTarget);
    
    // SetViewport is at index 47
    g_SetViewportTarget = vTable[47];
    ProxyLog("[HOOK] SetViewport address: 0x%p", g_SetViewportTarget);
    
    // Hook BeginScene (for letterbox rendering initialization)
    mhStatus = MH_CreateHook(g_BeginSceneTarget, HookedBeginScene, (void**)&g_pOriginalBeginScene);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHook(BeginScene) failed: %s", MH_StatusToString(mhStatus));
        return false;
    }
    ProxyLog("[HOOK] BeginScene hook created, trampoline: 0x%p", g_pOriginalBeginScene);
    
    // Hook EndScene
    mhStatus = MH_CreateHook(g_EndSceneTarget, HookedEndScene, (void**)&g_pOriginalEndScene);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHook(EndScene) failed: %s", MH_StatusToString(mhStatus));
        MH_RemoveHook(g_BeginSceneTarget);
        return false;
    }
    ProxyLog("[HOOK] EndScene hook created, trampoline: 0x%p", g_pOriginalEndScene);
    
    // Hook Reset
    mhStatus = MH_CreateHook(g_ResetTarget, HookedReset, (void**)&g_pOriginalReset);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHook(Reset) failed: %s", MH_StatusToString(mhStatus));
        MH_RemoveHook(g_BeginSceneTarget);
        MH_RemoveHook(g_EndSceneTarget);
        return false;
    }
    ProxyLog("[HOOK] Reset hook created, trampoline: 0x%p", g_pOriginalReset);
    
    // Hook SetWindowPos (Win32 API)
    mhStatus = MH_CreateHookApi(L"user32", "SetWindowPos", HookedSetWindowPos, (void**)&g_pOriginalSetWindowPos);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHookApi(SetWindowPos) failed: %s", MH_StatusToString(mhStatus));
        // Non-fatal, proceed
    } else {
        ProxyLog("[HOOK] SetWindowPos hook created, trampoline: 0x%p", g_pOriginalSetWindowPos);
    }

    // Hook SetWindowLongA (Win32 API)
    mhStatus = MH_CreateHookApi(L"user32", "SetWindowLongA", HookedSetWindowLongA, (void**)&g_pOriginalSetWindowLongA);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHookApi(SetWindowLongA) failed: %s", MH_StatusToString(mhStatus));
    } else {
        ProxyLog("[HOOK] SetWindowLongA hook created, trampoline: 0x%p", g_pOriginalSetWindowLongA);
    }

    // Hook SetWindowLongW (Win32 API)
    mhStatus = MH_CreateHookApi(L"user32", "SetWindowLongW", HookedSetWindowLongW, (void**)&g_pOriginalSetWindowLongW);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHookApi(SetWindowLongW) failed: %s", MH_StatusToString(mhStatus));
    } else {
        ProxyLog("[HOOK] SetWindowLongW hook created, trampoline: 0x%p", g_pOriginalSetWindowLongW);
    }

    // Hook SetWindowRgn (Win32 API)
    mhStatus = MH_CreateHookApi(L"user32", "SetWindowRgn", HookedSetWindowRgn, (void**)&g_pOriginalSetWindowRgn);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHookApi(SetWindowRgn) failed: %s", MH_StatusToString(mhStatus));
    } else {
        ProxyLog("[HOOK] SetWindowRgn hook created, trampoline: 0x%p", g_pOriginalSetWindowRgn);
    }

    // Hook SetMenu (Win32 API)
    mhStatus = MH_CreateHookApi(L"user32", "SetMenu", HookedSetMenu, (void**)&g_pOriginalSetMenu);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHookApi(SetMenu) failed: %s", MH_StatusToString(mhStatus));
    } else {
        ProxyLog("[HOOK] SetMenu hook created, trampoline: 0x%p", g_pOriginalSetMenu);
    }
    
    // Hook Present (for letterboxing)
    mhStatus = MH_CreateHook(g_PresentTarget, HookedPresent, (void**)&g_pOriginalPresent);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHook(Present) failed: %s", MH_StatusToString(mhStatus));
        MH_RemoveHook(g_BeginSceneTarget);
        MH_RemoveHook(g_EndSceneTarget);
        MH_RemoveHook(g_ResetTarget);
        return false;
    }
    ProxyLog("[HOOK] Present hook created, trampoline: 0x%p", g_pOriginalPresent);
    
    // Hook SwapChain Present - DXLib may call this directly instead of Device::Present!
    // Get the implicit swap chain from the device
    IDirect3DSwapChain9* pImplicitSwapChain = nullptr;
    HRESULT hr = pDevice->GetSwapChain(0, &pImplicitSwapChain);
    if (SUCCEEDED(hr) && pImplicitSwapChain) {
        // Get swap chain vtable
        void** swapChainVTable = *(void***)pImplicitSwapChain;
        ProxyLog("[HOOK] SwapChain vtable: 0x%p", swapChainVTable);
        
        // IDirect3DSwapChain9::Present is at index 3
        g_SwapChainPresentTarget = swapChainVTable[3];
        ProxyLog("[HOOK] SwapChain::Present address: 0x%p", g_SwapChainPresentTarget);
        
        mhStatus = MH_CreateHook(g_SwapChainPresentTarget, HookedSwapChainPresent, (void**)&g_pOriginalSwapChainPresent);
        if (mhStatus != MH_OK) {
            ProxyLog("[HOOK] WARNING: MH_CreateHook(SwapChain::Present) failed: %s", MH_StatusToString(mhStatus));
            // Non-fatal - Device::Present might still work
        } else {
            ProxyLog("[HOOK] SwapChain::Present hook created, trampoline: 0x%p", g_pOriginalSwapChainPresent);
        }
        
        pImplicitSwapChain->Release();
    } else {
        ProxyLog("[HOOK] WARNING: Could not get implicit swap chain: 0x%08X", hr);
    };
    
    // Hook SetRenderTarget (for letterbox render target redirection)
    mhStatus = MH_CreateHook(g_SetRenderTargetTarget, HookedSetRenderTarget, (void**)&g_pOriginalSetRenderTarget);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHook(SetRenderTarget) failed: %s", MH_StatusToString(mhStatus));
        MH_RemoveHook(g_BeginSceneTarget);
        MH_RemoveHook(g_EndSceneTarget);
        MH_RemoveHook(g_ResetTarget);
        MH_RemoveHook(g_PresentTarget);
        return false;
    }
    ProxyLog("[HOOK] SetRenderTarget hook created, trampoline: 0x%p", g_pOriginalSetRenderTarget);
    
    // Hook SetViewport (for letterbox viewport clamping)
    mhStatus = MH_CreateHook(g_SetViewportTarget, HookedSetViewport, (void**)&g_pOriginalSetViewport);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_CreateHook(SetViewport) failed: %s", MH_StatusToString(mhStatus));
        MH_RemoveHook(g_BeginSceneTarget);
        MH_RemoveHook(g_EndSceneTarget);
        MH_RemoveHook(g_ResetTarget);
        MH_RemoveHook(g_PresentTarget);
        MH_RemoveHook(g_SetRenderTargetTarget);
        return false;
    }
    ProxyLog("[HOOK] SetViewport hook created, trampoline: 0x%p", g_pOriginalSetViewport);
    
    // Enable all hooks
    mhStatus = MH_EnableHook(MH_ALL_HOOKS);
    if (mhStatus != MH_OK) {
        ProxyLog("[HOOK] ERROR: MH_EnableHook failed: %s", MH_StatusToString(mhStatus));
        MH_RemoveHook(g_BeginSceneTarget);
        MH_RemoveHook(g_EndSceneTarget);
        MH_RemoveHook(g_ResetTarget);
        MH_RemoveHook(g_PresentTarget);
        MH_RemoveHook(g_SetRenderTargetTarget);
        MH_RemoveHook(g_SetViewportTarget);
        return false;
    }
    
    g_HooksInstalled = true;
    ProxyLog("[HOOK] All hooks installed and enabled successfully!");
    
    return true;
}

void UninstallDeviceHooks() {
    if (!g_HooksInstalled) return;
    
    ProxyLog("[HOOK] Uninstalling hooks...");
    
    // Release letterbox resources
    ReleaseLetterboxing();
    
    // Release cached backbuffer
    ReleaseCachedGameBackBuffer();
    
    MH_DisableHook(MH_ALL_HOOKS);
    
    if (g_BeginSceneTarget) {
        MH_RemoveHook(g_BeginSceneTarget);
        g_BeginSceneTarget = nullptr;
    }
    
    if (g_EndSceneTarget) {
        MH_RemoveHook(g_EndSceneTarget);
        g_EndSceneTarget = nullptr;
    }
    
    if (g_ResetTarget) {
        MH_RemoveHook(g_ResetTarget);
        g_ResetTarget = nullptr;
    }
    
    if (g_PresentTarget) {
        MH_RemoveHook(g_PresentTarget);
        g_PresentTarget = nullptr;
    }
    
    if (g_SetRenderTargetTarget) {
        MH_RemoveHook(g_SetRenderTargetTarget);
        g_SetRenderTargetTarget = nullptr;
    }
    
    if (g_SetViewportTarget) {
        MH_RemoveHook(g_SetViewportTarget);
        g_SetViewportTarget = nullptr;
    }
    
    MH_Uninitialize();
    g_HooksInstalled = false;
}

// ============================================================================
// Wrapped IDirect3D9 (intercepts CreateDevice)
// ============================================================================

class WrappedDirect3D9 : public IDirect3D9 {
private:
    IDirect3D9* m_pReal;
    
public:
    WrappedDirect3D9(IDirect3D9* pReal) : m_pReal(pReal) {
        ProxyLog("[WRAP] WrappedDirect3D9 created, real=0x%p", pReal);
    }
    
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_pReal->AddRef();
    }
    
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_pReal->Release();
        if (count == 0) {
            ProxyLog("[WRAP] WrappedDirect3D9 released");
            delete this;
        }
        return count;
    }
    
    // IDirect3D9
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override {
        return m_pReal->RegisterSoftwareDevice(pInitializeFunction);
    }
    
    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        return m_pReal->GetAdapterCount();
    }
    
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override {
        return m_pReal->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
    }
    
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override {
        return m_pReal->GetAdapterModeCount(Adapter, Format);
    }
    
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override {
        return m_pReal->EnumAdapterModes(Adapter, Format, Mode, pMode);
    }
    
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override {
        return m_pReal->GetAdapterDisplayMode(Adapter, pMode);
    }
    
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override {
        return m_pReal->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
    }
    
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        return m_pReal->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
    }
    
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override {
        return m_pReal->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
    }
    
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override {
        return m_pReal->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
    }
    
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override {
        return m_pReal->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
    }
    
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override {
        return m_pReal->GetDeviceCaps(Adapter, DeviceType, pCaps);
    }
    
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override {
        return m_pReal->GetAdapterMonitor(Adapter);
    }
    
    // This is where we intercept device creation!
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                            DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
                                            IDirect3DDevice9** ppReturnedDeviceInterface) override {
        static unsigned int s_createDeviceCallCount = 0;
        s_createDeviceCallCount++;
        SetStartupStage("IDirect3D9::CreateDevice");
        ProxyLog("[CREATEDEVICE] CreateDevice called!");
        ProxyLog("[CREATEDEVICE] Call #%u", s_createDeviceCallCount);
        ProxyLog("[CREATEDEVICE] Adapter=%u, DeviceType=%d, Window=0x%p, Flags=0x%08X",
                 Adapter, DeviceType, hFocusWindow, BehaviorFlags);

        if (!pPresentationParameters) {
            ProxyLog("[CREATEDEVICE] ERROR: pPresentationParameters is NULL");
            return D3DERR_INVALIDCALL;
        }
        if (!ppReturnedDeviceInterface) {
            ProxyLog("[CREATEDEVICE] ERROR: ppReturnedDeviceInterface is NULL");
            return D3DERR_INVALIDCALL;
        }

        D3DADAPTER_IDENTIFIER9 adapterId = {};
        HRESULT adapterHr = m_pReal->GetAdapterIdentifier(Adapter, 0, &adapterId);
        if (SUCCEEDED(adapterHr)) {
            ProxyLog("[CREATEDEVICE] Adapter identifier: Driver='%s' Description='%s' Device='%s' Vendor=0x%04X DeviceId=0x%04X Revision=0x%08X",
                     adapterId.Driver,
                     adapterId.Description,
                     adapterId.DeviceName,
                     adapterId.VendorId,
                     adapterId.DeviceId,
                     adapterId.Revision);
        } else {
            ProxyLog("[CREATEDEVICE] WARNING: GetAdapterIdentifier failed: 0x%08X", adapterHr);
        }

        D3DDISPLAYMODE displayMode = {};
        HRESULT displayHr = m_pReal->GetAdapterDisplayMode(Adapter, &displayMode);
        if (SUCCEEDED(displayHr)) {
            ProxyLog("[CREATEDEVICE] Adapter display mode: %ux%u fmt=%d refresh=%u",
                     displayMode.Width,
                     displayMode.Height,
                     displayMode.Format,
                     displayMode.RefreshRate);
        } else {
            ProxyLog("[CREATEDEVICE] WARNING: GetAdapterDisplayMode failed: 0x%08X", displayHr);
        }
        
        if (hFocusWindow) {
            LogWindowInfo(hFocusWindow, "[CREATEDEVICE]");
        } else {
            ProxyLog("[CREATEDEVICE] hFocusWindow is NULL");
        }

        ProxyLog("[CREATEDEVICE] Original params: BB=%ux%u Format=%d Windowed=%d SwapEffect=%d BackBufferCount=%u MultiSample=%d/%lu AutoDepth=%d DepthFmt=%d Flags=0x%08lX Refresh=%u Interval=0x%08X",
                 pPresentationParameters->BackBufferWidth,
                 pPresentationParameters->BackBufferHeight,
                 pPresentationParameters->BackBufferFormat,
                 pPresentationParameters->Windowed,
                 pPresentationParameters->SwapEffect,
                 pPresentationParameters->BackBufferCount,
                 pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality,
                 pPresentationParameters->EnableAutoDepthStencil,
                 pPresentationParameters->AutoDepthStencilFormat,
                 pPresentationParameters->Flags,
                 pPresentationParameters->FullScreen_RefreshRateInHz,
                 pPresentationParameters->PresentationInterval);
        
        // Save the game's native resolution
        g_nativeWidth = pPresentationParameters->BackBufferWidth;
        g_nativeHeight = pPresentationParameters->BackBufferHeight;
        if (g_nativeWidth == 0) g_nativeWidth = 640;
        if (g_nativeHeight == 0) g_nativeHeight = 480;
        
        ProxyLog("[CREATEDEVICE] Native resolution: %dx%d", g_nativeWidth, g_nativeHeight);
        
        // Calculate the borderless window size now so we can create a larger backbuffer
        int borderlessWidth = g_nativeWidth;
        int borderlessHeight = g_nativeHeight;
        
        if (g_useBorderlessFullscreen && hFocusWindow) {
            // Get monitor info to calculate aspect-correct window size
            HMONITOR hMon = MonitorFromWindow(hFocusWindow, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfo(hMon, &mi);
            
            int monW = mi.rcMonitor.right - mi.rcMonitor.left;
            int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            
            // Calculate window size maintaining aspect ratio
            float targetAspect = (float)g_nativeWidth / (float)g_nativeHeight;
            
            if (monH * g_nativeWidth / g_nativeHeight <= monW) {
                borderlessWidth = monH * g_nativeWidth / g_nativeHeight;
                borderlessHeight = monH;
            } else {
                borderlessWidth = monW;
                borderlessHeight = monW * g_nativeHeight / g_nativeWidth;
            }
            
            ProxyLog("[CREATEDEVICE] Borderless target size: %dx%d (monitor %dx%d)", 
                     borderlessWidth, borderlessHeight, monW, monH);
            
            // Store for later use
            g_screenWidth = borderlessWidth;
            g_screenHeight = borderlessHeight;
        }
        
        // For borderless fullscreen, we need to:
        // 1. Force windowed mode 
        // 2. Keep backbuffer at native 640x480 - D3D9 Present will stretch to window
        // 3. Defer window resize to after DXLib finishes its setup
        
        if (g_useBorderlessFullscreen) {
            // Force windowed mode - we'll make the window borderless later
            pPresentationParameters->Windowed = TRUE;
            pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            
            // IMPORTANT: Keep backbuffer at native resolution (640x480)
            // D3D9's Present will automatically stretch to fill the window
            // This is much simpler than trying to redirect rendering
            ProxyLog("[CREATEDEVICE] Keeping backbuffer at native %dx%d, D3D9 will stretch to %dx%d",
                     g_nativeWidth, g_nativeHeight, borderlessWidth, borderlessHeight);
            
            // Set flag to apply borderless on first EndScene
            g_needsInitialBorderless = true;
            g_isCurrentlyBorderless = false;  // Will be set true after window resize
            
            ProxyLog("[CREATEDEVICE] Borderless mode requested - will apply after DXLib setup");
        } else if (g_forceWindowed && !pPresentationParameters->Windowed) {
            // Test harness: game requested fullscreen but we need windowed
            pPresentationParameters->Windowed = TRUE;
            pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            ProxyLog("[CREATEDEVICE] Force-windowed (test harness) - overriding fullscreen request");
        }
        
        ProxyLog("[CREATEDEVICE] Calling real CreateDevice with params: BB=%ux%u Format=%d Windowed=%d Refresh=%u Interval=0x%08X",
                 pPresentationParameters->BackBufferWidth,
                 pPresentationParameters->BackBufferHeight,
                 pPresentationParameters->BackBufferFormat,
                 pPresentationParameters->Windowed,
                 pPresentationParameters->FullScreen_RefreshRateInHz,
                 pPresentationParameters->PresentationInterval);

        HRESULT hr = m_pReal->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                           pPresentationParameters, ppReturnedDeviceInterface);
        ProxyLog("[CREATEDEVICE] Real CreateDevice returned: 0x%08X", hr);
        
        if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            IDirect3DDevice9* pDevice = *ppReturnedDeviceInterface;
            ProxyLog("[CREATEDEVICE] Device created: 0x%p", pDevice);
            
            // Save the original present parameters - we need these for Reset!
            memcpy(&g_originalPresentParams, pPresentationParameters, sizeof(D3DPRESENT_PARAMETERS));
            g_hasSavedPresentParams = true;
            ProxyLog("[CREATEDEVICE] Saved original present params: BB=%dx%d, Format=%d, DepthStencil=%s (Format=%d)",
                     g_originalPresentParams.BackBufferWidth,
                     g_originalPresentParams.BackBufferHeight,
                     g_originalPresentParams.BackBufferFormat,
                     g_originalPresentParams.EnableAutoDepthStencil ? "YES" : "NO",
                     g_originalPresentParams.AutoDepthStencilFormat);
            
            // Install our hooks on the device
            if (!InstallDeviceHooks(pDevice)) {
                ProxyLog("[CREATEDEVICE] WARNING: Failed to install device hooks!");
            }
            
            // Hook the window procedure to prevent style changes
            // Use SetWindowLongPtrW and store the original for CallWindowProcW
            if (hFocusWindow && !g_proxyOriginalWndProc) {
                g_gameWindow = hFocusWindow;

                // Set a clearer title for multi-instance testing/debugging.
                ApplyCustomWindowTitle(hFocusWindow);
                
                // Get class info first to understand what we're dealing with
                WNDCLASSEXW wcex = {0};
                wcex.cbSize = sizeof(wcex);
                wchar_t className[256] = {0};
                GetClassNameW(hFocusWindow, className, 256);
                
                if (GetClassInfoExW(GetModuleHandle(nullptr), className, &wcex)) {
                    ProxyLog("[CREATEDEVICE] Window class WndProc: 0x%p", wcex.lpfnWndProc);
                }
                
                // Get current WndProc before we hook
                LONG_PTR currentWndProc = GetWindowLongPtrW(hFocusWindow, GWLP_WNDPROC);
                ProxyLog("[CREATEDEVICE] Current window WndProc: 0x%p", (void*)currentWndProc);
                
                // Hook the window procedure
                g_proxyOriginalWndProc = (WNDPROC)SetWindowLongPtrW(hFocusWindow, GWLP_WNDPROC, (LONG_PTR)ProxyWndProc);
                ProxyLog("[CREATEDEVICE] Hooked WndProc: Original=0x%p, New=0x%p", g_proxyOriginalWndProc, ProxyWndProc);
                if (IsDxLibMagicWndProc(g_proxyOriginalWndProc)) {
                    ProxyLog("[CREATEDEVICE] Prior WndProc is DXLib magic stub — use ModCallGameWndProc "
                             "(see mod/docs/SHELL_HOTKEY_POLICY.md; no SC_TASKLIST/ActivateKeyboardLayout hacks)");
                } else if (reinterpret_cast<uintptr_t>(g_proxyOriginalWndProc) >= 0x00400000u &&
                           reinterpret_cast<uintptr_t>(g_proxyOriginalWndProc) < 0x00C00000u) {
                    ProxyLog("[CREATEDEVICE] Prior WndProc is in-game code at 0x%p", g_proxyOriginalWndProc);
                }
                
                // Validate the hook worked
                LONG_PTR newWndProc = GetWindowLongPtrW(hFocusWindow, GWLP_WNDPROC);
                if (newWndProc == (LONG_PTR)ProxyWndProc) {
                    ProxyLog("[CREATEDEVICE] WndProc hook SUCCESS - verified!");
                } else {
                    ProxyLog("[CREATEDEVICE] WndProc hook WARNING - verification failed! Got 0x%p", (void*)newWndProc);
                }
                
                // Force the style immediately if we are going borderless
                if (g_useBorderlessFullscreen) {
                    // DXLib creates its default font cache shortly after device creation.
                    // Some systems crash in that path if the client area has already been
                    // expanded to borderless size, so leave the startup window alone until
                    // Present sees the default font handle become valid.
                    g_pendingBorderlessWindow = hFocusWindow;
                    // NOTE: Don't call InitializeLetterboxing here!
                    // We use the scaling swap chain approach instead (initialized in Present hook)
                    // The LetterboxScaler conflicts with the swap chain approach
                    ProxyLog("[CREATEDEVICE] Borderless resize delayed until game default font cache is ready");
                } else if (g_windowedWidth > 0 && g_windowedHeight > 0) {
                    // Apply saved windowed dimensions
                    SetBorderlessState(hFocusWindow, false);
                    ProxyLog("[CREATEDEVICE] Applied saved windowed size: %dx%d", g_windowedWidth, g_windowedHeight);
                }
            } else {
                // Just set borderless state without WndProc hook
                g_gameWindow = hFocusWindow;

                // Set a clearer title for multi-instance testing/debugging.
                ApplyCustomWindowTitle(hFocusWindow);
                if (g_useBorderlessFullscreen && hFocusWindow) {
                    g_pendingBorderlessWindow = hFocusWindow;
                    // NOTE: Scaling swap chain initialized in first Present
                    ProxyLog("[CREATEDEVICE] Borderless resize delayed until game default font cache is ready (no WndProc hook)");
                } else if (g_windowedWidth > 0 && g_windowedHeight > 0 && hFocusWindow) {
                    SetBorderlessState(hFocusWindow, false);
                    ProxyLog("[CREATEDEVICE] Applied saved windowed size (no WndProc hook): %dx%d", g_windowedWidth, g_windowedHeight);
                }
            }
            
            // Log backbuffer info
            IDirect3DSurface9* pBackBuffer = nullptr;
            if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer))) {
                LogSurfaceInfo(pBackBuffer, "[CREATEDEVICE] BackBuffer");
                pBackBuffer->Release();
            }
            
            // Avoid manipulating foreground focus during startup. The game's
            // DirectInput bootstrap is fragile while the title loop is still
            // creating devices, so defer this until the window has settled.
            if (hFocusWindow) {
                QueueDeferredFocusReclaim(hFocusWindow, "CreateDevice startup");
            }
            
        } else {
            ProxyLog("[CREATEDEVICE] ERROR: CreateDevice failed: 0x%08X", hr);
        }
        
        return hr;
    }
};

// ============================================================================
// Load Real D3D9
// ============================================================================

bool LoadRealD3D9() {
    char systemPath[MAX_PATH];
    if (!GetSystemDirectoryA(systemPath, MAX_PATH)) {
        DWORD err = GetLastError();
        char errText[256];
        FormatWin32Error(err, errText, sizeof(errText));
        ProxyLog("[D3D9] ERROR: GetSystemDirectoryA failed: err=%lu %s", err, errText);
        return false;
    }
    
    char d3d9Path[MAX_PATH];
    snprintf(d3d9Path, MAX_PATH, "%s\\d3d9.dll", systemPath);
    
    ProxyLog("[D3D9] Loading real d3d9.dll from: %s", d3d9Path);
    ProxyLogFileProbe("[D3D9] real d3d9.dll", d3d9Path);
    
    g_hRealD3D9 = LoadLibraryA(d3d9Path);
    if (!g_hRealD3D9) {
        DWORD err = GetLastError();
        char errText[256];
        FormatWin32Error(err, errText, sizeof(errText));
        ProxyLog("[D3D9] ERROR: Failed to load real d3d9.dll! Error: %lu %s", err, errText);
        return false;
    }
    ProxyLog("[D3D9] Real d3d9.dll loaded: 0x%p", g_hRealD3D9);
    ProxyLogModuleByName("d3d9.dll");
    
    g_pRealDirect3DCreate9 = (RealDirect3DCreate9_t)GetProcAddress(g_hRealD3D9, "Direct3DCreate9");
    if (!g_pRealDirect3DCreate9) {
        DWORD err = GetLastError();
        char errText[256];
        FormatWin32Error(err, errText, sizeof(errText));
        ProxyLog("[D3D9] ERROR: Failed to get Direct3DCreate9! Error: %lu %s", err, errText);
        return false;
    }
    ProxyLog("[D3D9] Direct3DCreate9 address: 0x%p", g_pRealDirect3DCreate9);
    
    return true;
}

// ============================================================================
// Load Mod DLL
// ============================================================================

static char* TrimProxyStringInPlace(char* text) {
    if (!text) {
        return text;
    }

    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }

    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static bool IsValidProxyModFolderName(const char* name) {
    if (!name || !name[0]) {
        return false;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    for (const unsigned char* cursor = (const unsigned char*)name; *cursor; ++cursor) {
        if (isspace(*cursor) || *cursor == '\\' || *cursor == '/' || *cursor == ':' ||
            *cursor == '*' || *cursor == '?' || *cursor == '"' || *cursor == '<' ||
            *cursor == '>' || *cursor == '|') {
            return false;
        }
    }

    return true;
}

static std::string LowercaseProxyString(const char* text) {
    std::string lower = text ? text : "";
    for (char& ch : lower) {
        ch = (char)tolower((unsigned char)ch);
    }
    return lower;
}

static bool ProxyFileExists(const char* path) {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool ProxyDirectoryExists(const char* path) {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool TryInvokeLoadedUserModInit(HMODULE module, HMODULE gameModule, const char* modName) {
    auto initFn = (ModInit_t)GetProcAddress(module, "ModInit");
    if (!initFn) {
        return false;
    }

    __try {
        initFn(gameModule);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        ProxyLog("[MODLOADER] ERROR: %s threw during ModInit", modName ? modName : "<unknown>");
        return false;
    }
}

static void TryInvokeLoadedUserModShutdown(ModShutdown_t shutdown, const char* modName) {
    if (!shutdown) {
        return;
    }

    __try {
        shutdown();
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        ProxyLog("[MODLOADER] ERROR: %s threw during ModShutdown", modName ? modName : "<unknown>");
    }
}

static void LoadConfiguredUserModDLLs(HMODULE gameModule) {
    if (!g_loadedUserModDLLs.empty()) {
        return;
    }

    char configPath[MAX_PATH] = {};
    snprintf(configPath, MAX_PATH, "%s\\mods\\mods.ini", g_dllDir);

    FILE* file = nullptr;
    if (fopen_s(&file, configPath, "rb") != 0 || !file) {
        ProxyLog("[MODLOADER] No config found at %s", configPath);
        return;
    }

    std::unordered_set<std::string> seenMods;
    std::vector<LoadedUserModDLL> orderedMods;
    bool inModsSection = false;
    char line[512] = {};
    while (fgets(line, sizeof(line), file)) {
        char* cursor = line;
        if ((unsigned char)cursor[0] == 0xEF &&
            (unsigned char)cursor[1] == 0xBB &&
            (unsigned char)cursor[2] == 0xBF) {
            cursor += 3;
        }

        char* comment = strchr(cursor, ';');
        if (comment) {
            *comment = '\0';
        }

        char* trimmed = TrimProxyStringInPlace(cursor);
        if (!trimmed[0]) {
            continue;
        }

        if (trimmed[0] == '[') {
            char* closing = strchr(trimmed, ']');
            if (!closing) {
                inModsSection = false;
                continue;
            }

            *closing = '\0';
            inModsSection = _stricmp(trimmed + 1, "Mods") == 0;
            continue;
        }

        if (!inModsSection) {
            continue;
        }

        char* equals = strchr(trimmed, '=');
        if (!equals) {
            continue;
        }

        *equals = '\0';
        char* modName = TrimProxyStringInPlace(trimmed);
        char* enabledValue = TrimProxyStringInPlace(equals + 1);
        if (!modName[0] || enabledValue[0] != '1') {
            continue;
        }

        if (!IsValidProxyModFolderName(modName)) {
            ProxyLog("[MODLOADER] Ignoring invalid mod folder name '%s'", modName);
            continue;
        }

        const std::string dedupeKey = LowercaseProxyString(modName);
        if (!seenMods.insert(dedupeKey).second) {
            ProxyLog("[MODLOADER] Ignoring duplicate mod entry '%s'", modName);
            continue;
        }

        char rootPath[MAX_PATH] = {};
        char dllPath[MAX_PATH] = {};
        snprintf(rootPath, MAX_PATH, "%s\\mods\\%s", g_dllDir, modName);
        snprintf(dllPath, MAX_PATH, "%s\\%s.dll", rootPath, modName);

        if (!ProxyDirectoryExists(rootPath)) {
            ProxyLog("[MODLOADER] Enabled mod folder missing: %s", rootPath);
            continue;
        }

        LoadedUserModDLL mod;
        mod.name = modName;
        mod.dllPath = dllPath;
        orderedMods.push_back(std::move(mod));
    }

    fclose(file);

    if (orderedMods.empty()) {
        ProxyLog("[MODLOADER] Config loaded from %s (no enabled mods)", configPath);
        return;
    }

    ProxyLog("[MODLOADER] Config loaded from %s (%u enabled mod%s)",
             configPath,
             (unsigned)orderedMods.size(),
             orderedMods.size() == 1 ? "" : "s");
    for (size_t index = 0; index < orderedMods.size(); ++index) {
        ProxyLog("[MODLOADER] Priority %u (top-to-bottom): %s",
                 (unsigned)(index + 1),
                 orderedMods[index].name.c_str());
    }

    for (LoadedUserModDLL& mod : orderedMods) {
        if (!ProxyFileExists(mod.dllPath.c_str())) {
            ProxyLog("[MODLOADER] %s: file overrides only (no DLL at %s)",
                     mod.name.c_str(),
                     mod.dllPath.c_str());
            continue;
        }

        ProxyLog("[MODLOADER] Loading DLL mod: %s", mod.dllPath.c_str());
        ProxyLogFileProbe("[MODLOADER] user DLL", mod.dllPath.c_str());
        mod.module = LoadLibraryA(mod.dllPath.c_str());
        if (!mod.module) {
            DWORD err = GetLastError();
            char errText[256];
            FormatWin32Error(err, errText, sizeof(errText));
            ProxyLog("[MODLOADER] WARNING: failed to load %s (err=%lu %s)",
                     mod.dllPath.c_str(),
                     err,
                     errText);
            continue;
        }

        auto setLogDir = (ModSetLogDir_t)GetProcAddress(mod.module, "ModSetLogDir");
        if (setLogDir && g_logDir[0]) {
            setLogDir(g_logDir);
        }

        mod.shutdown = (ModShutdown_t)GetProcAddress(mod.module, "ModShutdown");
        mod.initCalled = TryInvokeLoadedUserModInit(mod.module, gameModule, mod.name.c_str());

        ProxyLog("[MODLOADER] Loaded DLL mod: %s", mod.dllPath.c_str());
        g_loadedUserModDLLs.push_back(std::move(mod));
    }
}

static void UnloadConfiguredUserModDLLs() {
    for (size_t index = g_loadedUserModDLLs.size(); index > 0; --index) {
        LoadedUserModDLL& mod = g_loadedUserModDLLs[index - 1];
        if (!mod.module) {
            continue;
        }

        TryInvokeLoadedUserModShutdown(mod.shutdown, mod.name.c_str());

        ProxyLog("[MODLOADER] Unloading DLL mod: %s", mod.dllPath.c_str());
        FreeLibrary(mod.module);
        mod.module = nullptr;
    }

    g_loadedUserModDLLs.clear();
}

static void PerformProxyShutdown(bool fastProcessExit) {
    if (g_proxyShutdownComplete) {
        return;
    }

    g_proxyShutdownComplete = true;

    ProxyLog("[SHUTDOWN] PerformProxyShutdown fast=%d", fastProcessExit ? 1 : 0);

    if (!fastProcessExit) {
        ShutdownImGui();
        UninstallDeviceHooks();
        UnloadConfiguredUserModDLLs();

        if (g_pModShutdown) {
            g_pModShutdown();
        }

        if (g_hModDLL) {
            FreeLibrary(g_hModDLL);
            g_hModDLL = nullptr;
        }

        if (g_hRealD3D9) {
            FreeLibrary(g_hRealD3D9);
            g_hRealD3D9 = nullptr;
        }
    } else {
        ProxyLog("[SHUTDOWN] Skipping explicit teardown under fast process exit");
    }

    if (g_logFile) {
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
        g_logLinesSinceFlush = 0;
    }

    ShutdownConsole(false);
}

bool LoadCoreModDLL() {
    if (g_hModDLL) return true;
    
    // Load SDL3 first
    char sdlPath[MAX_PATH];
    snprintf(sdlPath, MAX_PATH, "%s\\SDL3.dll", g_dllDir);
    ProxyLogFileProbe("[MOD] SDL3.dll", sdlPath);
    
    SetStartupStage("loading SDL3.dll");
    HMODULE hSDL = LoadLibraryA(sdlPath);
    if (!hSDL) {
        DWORD err = GetLastError();
        char errText[256];
        FormatWin32Error(err, errText, sizeof(errText));
        ProxyLog("[MOD] WARNING: SDL3.dll not found at: %s (Error: %lu %s)", sdlPath, err, errText);
        ProxyLog("[MOD] Make sure SDL3.dll is in the game folder!");
        MessageBoxA(NULL,
            "SDL3.dll was not found in the game folder.\n\n"
            "The mod requires SDL3.dll to function.\n"
            "Please place SDL3.dll next to the game executable.",
            "Alice Senki 2 - Improvement Mod", MB_OK | MB_ICONERROR);
        return false;
    } else {
        char loadedSdlPath[MAX_PATH] = {};
        GetModuleFileNameA(hSDL, loadedSdlPath, MAX_PATH);
        ProxyLog("[MOD] SDL3.dll loaded: base=0x%p path=%s", hSDL, loadedSdlPath[0] ? loadedSdlPath : sdlPath);
    }
    
    // Load core rollback DLL after its shared dependencies.
    char modPath[MAX_PATH];
    snprintf(modPath, MAX_PATH, "%s\\as2_rollback.dll", g_dllDir);
    
    ProxyLog("[MOD] Loading mod DLL: %s", modPath);
    ProxyLogFileProbe("[MOD] as2_rollback.dll", modPath);
    
    SetStartupStage("loading as2_rollback.dll");
    g_hModDLL = LoadLibraryA(modPath);
    if (!g_hModDLL) {
        DWORD err = GetLastError();
        char errText[256];
        FormatWin32Error(err, errText, sizeof(errText));
        ProxyLog("[MOD] ERROR: as2_rollback.dll failed to load! Error: %lu %s", err, errText);
        if (err == 126) {
            ProxyLog("[MOD]   Error 126 = Module not found. Check that:");
            ProxyLog("[MOD]   1. as2_rollback.dll exists at: %s", modPath);
            ProxyLog("[MOD]   2. SDL3.dll exists at: %s", sdlPath);
        }
        char errMsg[512];
        snprintf(errMsg, sizeof(errMsg),
            "as2_rollback.dll failed to load (Error: %lu).\n\n"
            "Expected location:\n%s\n\n"
            "Make sure as2_rollback.dll and SDL3.dll are both\n"
            "in the game folder.",
            err, modPath);
        MessageBoxA(NULL, errMsg, "Alice Senki 2 - Improvement Mod", MB_OK | MB_ICONERROR);
        return false;
    }
    char loadedModPath[MAX_PATH] = {};
    GetModuleFileNameA(g_hModDLL, loadedModPath, MAX_PATH);
    ProxyLog("[MOD] as2_rollback.dll loaded: base=0x%p path=%s", g_hModDLL, loadedModPath[0] ? loadedModPath : modPath);
    ProxyLogModuleSnapshot("after core mod DLL load");
    
    g_pModInit = (ModInit_t)GetProcAddress(g_hModDLL, "ModInit");
    g_pModShutdown = (ModShutdown_t)GetProcAddress(g_hModDLL, "ModShutdown");
    g_pModOnFrame = (ModOnFrame_t)GetProcAddress(g_hModDLL, "ModOnFrame");
    g_pModOnPresent = (ModOnPresent_t)GetProcAddress(g_hModDLL, "ModOnPresent");
    g_pModSetImGuiContext = (ModSetImGuiContext_t)GetProcAddress(g_hModDLL, "ModSetImGuiContext");
    g_pModOnGameExit = (ModOnGameExit_t)GetProcAddress(g_hModDLL, "ModOnGameExit");
    g_pModToggleMenu = (ModToggleMenu_t)GetProcAddress(g_hModDLL, "ModToggleMenu");
    g_pModIsMenuRequestedOpen = (ModIsMenuRequestedOpen_t)GetProcAddress(g_hModDLL, "ModIsMenuRequestedOpen");
    g_pModShouldRenderImGui = (ModShouldRenderImGui_t)GetProcAddress(g_hModDLL, "ModShouldRenderImGui");
    g_pModGetNetplayHudText = (ModGetNetplayHudText_t)GetProcAddress(g_hModDLL, "ModGetNetplayHudText");
    g_pModGetMatchHudData = (ModGetMatchHudData_t)GetProcAddress(g_hModDLL, "ModGetMatchHudData");
    g_pModWantsExclusiveOverlay = (ModWantsExclusiveOverlay_t)GetProcAddress(g_hModDLL, "ModWantsExclusiveOverlay");
    g_pModCallGameWndProc =
        (ModCallGameWndProc_t)GetProcAddress(g_hModDLL, "ModCallGameWndProc");
    
    // Pass log directory to mod so all logs end up in the same dated folder
    auto pModSetLogDir = (ModSetLogDir_t)GetProcAddress(g_hModDLL, "ModSetLogDir");
    if (pModSetLogDir && g_logDir[0]) {
        SetStartupStage("passing log dir to as2_rollback.dll");
        ProxyLog("[MOD] Passing log directory to mod: %s", g_logDir);
        pModSetLogDir(g_logDir);
    } else if (!pModSetLogDir) {
        ProxyLog("[MOD] WARNING: ModSetLogDir export missing");
    } else {
        ProxyLog("[MOD] WARNING: g_logDir is empty; mod will create its own log directory");
    }
    
    ProxyLog("[MOD] Exports - Init:0x%p Shutdown:0x%p OnFrame:0x%p OnPresent:0x%p SetCtx:0x%p Exit:0x%p ToggleMenu:0x%p MenuState:0x%p ShouldRender:0x%p Hud:0x%p MatchHud:0x%p Exclusive:0x%p",
             g_pModInit, g_pModShutdown, g_pModOnFrame, g_pModOnPresent, g_pModSetImGuiContext, g_pModOnGameExit, g_pModToggleMenu, g_pModIsMenuRequestedOpen, g_pModShouldRenderImGui, g_pModGetNetplayHudText, g_pModGetMatchHudData, g_pModWantsExclusiveOverlay);
    
    return true;
}

// ============================================================================
// DLL Entry Point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_processAttachTick = GetTickCount();
            DisableThreadLibraryCalls(hModule);
            
            // Get our DLL path
            GetModuleFileNameW(hModule, g_dllPathW, MAX_PATH);
            wcscpy_s(g_dllDirW, g_dllPathW);
            wchar_t* lastSlashW = wcsrchr(g_dllDirW, L'\\');
            if (lastSlashW) *lastSlashW = L'\0';

            GetModuleFileNameA(hModule, g_dllPath, MAX_PATH);
            strcpy_s(g_dllDir, g_dllPath);
            char* lastSlash = strrchr(g_dllDir, '\\');
            if (lastSlash) *lastSlash = '\0';
            
            // Install crash handler FIRST (before anything else can crash)
            strncpy_s(g_startupStage, sizeof(g_startupStage), "installing crash handler", _TRUNCATE);
            g_previousExceptionFilter = SetUnhandledExceptionFilter(CrashHandler);
            
            // Initialize console
            strncpy_s(g_startupStage, sizeof(g_startupStage), "initializing console", _TRUNCATE);
            InitConsole();
            
            ProxyLog("========================================");
            ProxyLog("Alice Senki 2 - D3D9 Proxy 0.7-beta2.01");
            ProxyLog("Build: %s %s", __DATE__, __TIME__);
            ProxyLog("Crash handler installed!");
            ProxyLog("========================================");
            ProxyLog("[INIT] Proxy DLL path: %s", g_dllPath);
            ProxyLog("[INIT] Working directory: %s", g_dllDir);
            ProxyLogWideValue("[INIT] Proxy DLL path W: ", g_dllPathW);
            ProxyLogWideValue("[INIT] Working directory W: ", g_dllDirW);
            PinProxyModuleForProcessLifetime(hModule);
            ProxyLogProcessDiagnostics(hModule);
            ProxyLogModuleSnapshot("process attach start");
            
            // Load real D3D9
            SetStartupStage("loading real d3d9.dll");
            if (!LoadRealD3D9()) {
                return FALSE;
            }
            
            // Load mod DLL
            SetStartupStage("loading core mod DLLs");
            LoadCoreModDLL();
            
            // Load persistent display settings (borderless, window size, aspect)
            SetStartupStage("loading display config");
            DisplayConfig_Load();
            
            // Call mod init
            if (g_pModInit) {
                SetStartupStage("calling ModInit");
                HMODULE gameModule = GetModuleHandleA(NULL);
                char gamePath[MAX_PATH];
                GetModuleFileNameA(gameModule, gamePath, MAX_PATH);
                ProxyLog("[INIT] Calling ModInit with game module: 0x%p (%s)", gameModule, gamePath);
                __try {
                    g_pModInit(gameModule);
                }
                __except(ProxyLogExceptionFilter("g_pModInit", GetExceptionInformation())) {
                }
                ProxyLog("[INIT] ModInit returned");
                SetStartupStage("loading configured user mod DLLs");
                LoadConfiguredUserModDLLs(gameModule);
            } else {
                SetStartupStage("loading configured user mod DLLs without core ModInit");
                LoadConfiguredUserModDLLs(GetModuleHandleA(NULL));
            }
            
            // If test harness autoconnect config exists, force windowed mode
            // so two instances don't fight over a fullscreen TOPMOST window.
            {
                SetStartupStage("checking test harness config");
                char cfgPath[MAX_PATH];
                snprintf(cfgPath, MAX_PATH, "%s\\as2_autoconnect.cfg", g_dllDir);
                ProxyLogFileProbe("[INIT] as2_autoconnect.cfg", cfgPath);
                if (GetFileAttributesA(cfgPath) != INVALID_FILE_ATTRIBUTES) {
                    g_useBorderlessFullscreen = false;
                    g_isCurrentlyBorderless = false;
                    g_forceWindowed = true;
                    ProxyLog("[INIT] Test harness detected (as2_autoconnect.cfg) - forcing windowed mode");
                }
            }
            
            SetStartupStage("process attach complete");
            ProxyLogModuleSnapshot("process attach complete");
            ProxyLog("[INIT] Initialization complete - waiting for game to create D3D9 device...");
            break;
        }
            
        case DLL_PROCESS_DETACH: {
            const bool processTerminating = (lpReserved != nullptr);
            ProxyLog("[SHUTDOWN] DLL_PROCESS_DETACH process_terminating=%d fast_exit=%d",
                     processTerminating ? 1 : 0,
                     g_fastExitRequested ? 1 : 0);
            if (!processTerminating) {
                ProxyLog("[SHUTDOWN] WARNING: proxy DLL is being unloaded before process termination; self-pin may have failed or the module was forcibly unmapped");
            }
            // Always use fast path: complex teardown (thread joins, hook removal,
            // 25+ subsystem shutdowns) must not run under DllMain/loader lock.
            // The process is exiting — Windows will reclaim all resources.
            PerformProxyShutdown(true);
            break;
        }
    }
    return TRUE;
}

// ============================================================================
// Exported Functions (for the game to call)
// ============================================================================

IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT SDKVersion) {
    static unsigned int s_direct3DCreateCallCount = 0;
    s_direct3DCreateCallCount++;
    SetStartupStage("Direct3DCreate9 export");
    ProxyLog("[EXPORT] Direct3DCreate9 called - SDK version: %u", SDKVersion);
    ProxyLog("[EXPORT] Direct3DCreate9 call #%u", s_direct3DCreateCallCount);
    
    if (!g_pRealDirect3DCreate9) {
        ProxyLog("[EXPORT] ERROR: Real Direct3DCreate9 not available!");
        return nullptr;
    }
    
    ProxyLog("[EXPORT] Calling real Direct3DCreate9 at 0x%p", g_pRealDirect3DCreate9);
    IDirect3D9* pD3D9 = g_pRealDirect3DCreate9(SDKVersion);
    if (!pD3D9) {
        ProxyLog("[EXPORT] ERROR: Real Direct3DCreate9 returned NULL!");
        return nullptr;
    }
    ProxyLog("[EXPORT] Real IDirect3D9: 0x%p", pD3D9);
    
    // Return our wrapper that intercepts CreateDevice
    WrappedDirect3D9* pWrapped = new WrappedDirect3D9(pD3D9);
    ProxyLog("[EXPORT] Returning wrapped IDirect3D9: 0x%p", pWrapped);
    
    return pWrapped;
}

// Menu state exports for mod DLL
extern "C" __declspec(dllexport) bool IsMenuVisible() {
    return g_showMenu;
}

extern "C" __declspec(dllexport) void SetMenuVisible(bool visible) {
    SetProxyMenuVisibleInternal(visible, "SetMenuVisible export", "Export", g_gameWindow, WM_APP, VK_F1, 0);
}
// Borderless fullscreen exports for mod DLL
extern "C" __declspec(dllexport) bool IsBorderlessFullscreen() {
    return g_useBorderlessFullscreen;
}

extern "C" __declspec(dllexport) void SetBorderlessFullscreen(bool enabled) {
    g_useBorderlessFullscreen = enabled;
}

extern "C" __declspec(dllexport) bool IsKeepAspectRatio() {
    return g_keepAspectRatio;
}

extern "C" __declspec(dllexport) void SetKeepAspectRatio(bool enabled) {
    g_keepAspectRatio = enabled;
}

extern "C" __declspec(dllexport) void GetNativeResolution(int* width, int* height) {
    if (width) *width = g_nativeWidth;
    if (height) *height = g_nativeHeight;
}

extern "C" __declspec(dllexport) void GetScreenResolution(int* width, int* height) {
    if (width) *width = g_screenWidth;
    if (height) *height = g_screenHeight;
}

extern "C" __declspec(dllexport) bool IsLetterboxActive() {
    return g_letterboxActive;
}

extern "C" __declspec(dllexport) void GetLetterboxViewport(int* x, int* y, int* w, int* h) {
    if (x) *x = (int)g_letterboxViewport.X;
    if (y) *y = (int)g_letterboxViewport.Y;
    if (w) *w = (int)g_letterboxViewport.Width;
    if (h) *h = (int)g_letterboxViewport.Height;
}

extern "C" __declspec(dllexport) void* GetNetplayHudFont(int preset) {
    if (preset < 0 || preset > 2) {
        preset = 1;
    }
    if (g_netplayHudFonts[preset]) {
        return g_netplayHudFonts[preset];
    }
    if (ImGui::GetCurrentContext()) {
        return ImGui::GetIO().FontDefault;
    }
    return nullptr;
}

// ============================================================================
// Menu text bridge (called by as2_rollback.dll)
// ============================================================================

// Queues one string for this frame, positioned in the game's 640x480 space.
// Rendered with the embedded Mincho face so the mod's menus match the vanilla
// settings screen instead of the game's built-in bitmap font.
extern "C" __declspec(dllexport)
void AS2Proxy_DrawMenuText(float x, float y, unsigned int abgr, const char* utf8, float size) {
    if (!utf8 || !utf8[0]) {
        return;
    }
    if (g_menuTextCount >= (int)(sizeof(g_menuTextQueue) / sizeof(g_menuTextQueue[0]))) {
        return;
    }
    QueuedMenuText& q = g_menuTextQueue[g_menuTextCount++];
    q.x = x;
    q.y = y;
    q.size = size;
    q.color = (ImU32)abgr;
    strncpy_s(q.text, utf8, _TRUNCATE);

    // The large atlas covers Latin (U+0000..U+02FF, lead bytes up to 0xCB) and,
    // since the merge above, Cyrillic (U+0400..U+04FF, lead bytes 0xD0..0xD3).
    // The old cut at 0xCC excluded exactly the Cyrillic it was meant to admit,
    // so Cyrillic never reached the atlas that could draw it.
    q.hasHighCodepoint = false;
    for (const unsigned char* c = (const unsigned char*)q.text; *c; ++c) {
        if (*c >= 0xD4u) {
            q.hasHighCodepoint = true;
            break;
        }
    }
}

// Advance width of a string at a given size, in the game's 640x480 space, using
// the same atlas the draw call would pick. The mod needs it to lay out anything
// that is not a fixed column - a tab strip, a centred caption - without
// duplicating the font metrics on its side.
extern "C" __declspec(dllexport)
float AS2Proxy_MeasureMenuText(const char* utf8, float size) {
    if (!utf8 || !utf8[0]) {
        return 0.0f;
    }
    bool high = false;
    for (const unsigned char* c = (const unsigned char*)utf8; *c; ++c) {
        if (*c >= 0xCCu) {
            high = true;
            break;
        }
    }
    ImFont* font = g_menuFont;
    if (g_menuFontLarge && size >= 40.0f && !high) {
        font = g_menuFontLarge;
    }
    if (!font) {
        return 0.0f;
    }
    const float drawSize = size > 0.0f ? size : font->FontSize;
    return font->CalcTextSizeA(drawSize, FLT_MAX, 0.0f, utf8).x;
}

// Lets the mod fall back to the game's own renderer when the face is missing.
extern "C" __declspec(dllexport)
int AS2Proxy_MenuFontReady() {
    return g_menuFont != nullptr ? 1 : 0;
}

// ============================================================================
// Display settings bridge (called by as2_rollback.dll)
// ============================================================================
//
// The display state lives here, not in the mod: the proxy owns the window, the
// swap chain and the letterbox rect. These mirror what ToggleBorderlessFullscreen
// already does, so the in-game menu drives the same paths as the hotkey rather
// than a second copy of the logic.

extern "C" __declspec(dllexport)
int AS2Proxy_GetDisplayBorderless() {
    return g_isCurrentlyBorderless ? 1 : 0;
}

extern "C" __declspec(dllexport)
void AS2Proxy_SetDisplayBorderless(int enable) {
    const bool want = enable != 0;
    if (want == g_isCurrentlyBorderless) {
        return;
    }
    HWND hWnd = g_gameWindow ? g_gameWindow : GetActiveWindow();
    if (!hWnd) {
        return;
    }
    g_useBorderlessFullscreen = want;
    SetBorderlessState(hWnd, want);
    DisplayConfig_Save();
}

extern "C" __declspec(dllexport)
int AS2Proxy_GetKeepAspect() {
    return g_keepAspectRatio ? 1 : 0;
}

extern "C" __declspec(dllexport)
void AS2Proxy_SetKeepAspect(int enable) {
    const bool want = enable != 0;
    if (want == g_keepAspectRatio) {
        return;
    }
    g_keepAspectRatio = want;
    g_letterboxActive = want && g_isCurrentlyBorderless;

    // Recompute the view rect immediately; input mapping reads it before the
    // next Present would refresh it.
    if (want) {
        CalculateLetterboxDestRect(g_screenWidth, g_screenHeight,
                                   g_nativeWidth, g_nativeHeight,
                                   &g_letterboxDestRect);
    } else {
        g_letterboxDestRect = { 0, 0, g_screenWidth, g_screenHeight };
    }
    ReleaseScalingSwapChain();
    g_windowResizedNeedsReinit = true;
    DisplayConfig_Save();
}

// Windowed size as a whole multiple of the game's 640x480, which is what a
// menu row can sensibly step through.
extern "C" __declspec(dllexport)
int AS2Proxy_GetWindowScale() {
    const int w = g_currentWindowWidth > 0 ? g_currentWindowWidth : g_nativeWidth;
    int scale = g_nativeWidth > 0 ? (w / g_nativeWidth) : 1;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    return scale;
}

extern "C" __declspec(dllexport)
void AS2Proxy_SetWindowScale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;

    g_windowedWidth = g_nativeWidth * scale;
    g_windowedHeight = g_nativeHeight * scale;

    // Only meaningful while windowed; borderless already fills the monitor.
    if (!g_isCurrentlyBorderless) {
        HWND hWnd = g_gameWindow ? g_gameWindow : GetActiveWindow();
        if (hWnd) {
            SetBorderlessState(hWnd, false);
        }
    }
    DisplayConfig_Save();
}
