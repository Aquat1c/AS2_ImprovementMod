// ============================================================================
// Letterbox Scaler - Clean render target redirection for aspect-ratio scaling
// ============================================================================
// 
// Strategy:
// 1. Create an offscreen render target at the game's native resolution (640x480)
// 2. Redirect all game rendering to this offscreen target
// 3. After the game finishes rendering (EndScene), copy the offscreen target
//    to the real backbuffer with scaling using StretchRect
// 4. This ensures the game always thinks it's rendering to 640x480 while
//    we can scale the output to any window size
//
// This approach is similar to what professional game capture and overlay
// tools use, and is more reliable than trying to manipulate viewports or
// Present parameters.
// ============================================================================

#pragma once

#include <d3d9.h>
#include <cstdio>

// Forward declaration for logging
extern void ProxyLog(const char* format, ...);

class LetterboxScaler {
public:
    LetterboxScaler();
    ~LetterboxScaler();

    // Initialize the scaler with the device and native game resolution
    bool Initialize(IDirect3DDevice9* pDevice, UINT nativeWidth, UINT nativeHeight);
    
    // Release all resources (call before device Reset or shutdown)
    void Release();
    
    // Call at the start of each frame (after BeginScene)
    // Redirects rendering to our offscreen target
    void BeginFrame(IDirect3DDevice9* pDevice);
    
    // Call at the end of each frame (before EndScene)
    // Copies the offscreen target to the real backbuffer with scaling
    void EndFrame(IDirect3DDevice9* pDevice);
    
    // Set the target window/screen size for scaling
    void SetTargetSize(UINT width, UINT height);
    
    // Enable/disable the scaler
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }
    
    // Check if initialized
    bool IsInitialized() const { return m_initialized; }
    
    // Check if we're in a frame (between BeginFrame/EndFrame)
    bool IsInFrame() const { return m_inFrame; }
    
    // Get the offscreen surface for render target redirection
    IDirect3DSurface9* GetOffscreenSurface() const { return m_pOffscreenSurface; }
    
    // Set/get the backbuffer surface (for tracking what to redirect)
    void SetBackBuffer(IDirect3DSurface9* pBackBuffer);
    IDirect3DSurface9* GetBackBuffer() const { return m_pBackBuffer; }
    
    // Check if a surface is the backbuffer (for redirection logic)
    bool IsBackBuffer(IDirect3DSurface9* pSurface) const;

private:
    // Create the offscreen render target
    bool CreateRenderTarget(IDirect3DDevice9* pDevice);
    
    // Create vertex buffer for fullscreen quad
    bool CreateQuadVertexBuffer(IDirect3DDevice9* pDevice);
    
    // Draw the scaled quad
    void DrawScaledQuad(IDirect3DDevice9* pDevice);

    // State
    bool m_initialized;
    bool m_enabled;
    bool m_inFrame;  // Track if we're between BeginFrame/EndFrame
    
    // Native game resolution
    UINT m_nativeWidth;
    UINT m_nativeHeight;
    
    // Target output resolution
    UINT m_targetWidth;
    UINT m_targetHeight;
    
    // Offscreen render target (game renders here)
    IDirect3DTexture9* m_pOffscreenTexture;
    IDirect3DSurface9* m_pOffscreenSurface;
    
    // Depth stencil for offscreen rendering (if game uses one)
    IDirect3DSurface9* m_pOffscreenDepthStencil;
    
    // Saved original render target/depth stencil
    IDirect3DSurface9* m_pOriginalRenderTarget;
    IDirect3DSurface9* m_pOriginalDepthStencil;
    
    // Track the backbuffer for SetRenderTarget redirection
    IDirect3DSurface9* m_pBackBuffer;
    
    // Vertex buffer for fullscreen quad
    IDirect3DVertexBuffer9* m_pQuadVB;
    
    // Vertex format for textured quad
    struct ScaledVertex {
        float x, y, z, rhw;
        float u, v;
    };
    static const DWORD SCALED_VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_TEX1;
};

// Global instance
extern LetterboxScaler g_letterboxScaler;
