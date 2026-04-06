// ============================================================================
// Letterbox Scaler Implementation
// ============================================================================

#include "letterbox_scaler.h"
#include <cstring>

// Global instance
LetterboxScaler g_letterboxScaler;

LetterboxScaler::LetterboxScaler()
    : m_initialized(false)
    , m_enabled(false)
    , m_inFrame(false)
    , m_nativeWidth(640)
    , m_nativeHeight(480)
    , m_targetWidth(1920)
    , m_targetHeight(1440)
    , m_pOffscreenTexture(nullptr)
    , m_pOffscreenSurface(nullptr)
    , m_pOffscreenDepthStencil(nullptr)
    , m_pOriginalRenderTarget(nullptr)
    , m_pOriginalDepthStencil(nullptr)
    , m_pBackBuffer(nullptr)
    , m_pQuadVB(nullptr)
{
}

LetterboxScaler::~LetterboxScaler() {
    Release();
}

bool LetterboxScaler::Initialize(IDirect3DDevice9* pDevice, UINT nativeWidth, UINT nativeHeight) {
    if (m_initialized) {
        ProxyLog("[SCALER] Already initialized, releasing first...");
        Release();
    }
    
    ProxyLog("[SCALER] Initializing LetterboxScaler...");
    ProxyLog("[SCALER]   Native resolution: %ux%u", nativeWidth, nativeHeight);
    ProxyLog("[SCALER]   Target resolution: %ux%u", m_targetWidth, m_targetHeight);
    
    m_nativeWidth = nativeWidth;
    m_nativeHeight = nativeHeight;
    
    // Create the offscreen render target
    if (!CreateRenderTarget(pDevice)) {
        ProxyLog("[SCALER] ERROR: Failed to create render target!");
        Release();
        return false;
    }
    
    // Create vertex buffer for the fullscreen quad
    if (!CreateQuadVertexBuffer(pDevice)) {
        ProxyLog("[SCALER] ERROR: Failed to create vertex buffer!");
        Release();
        return false;
    }
    
    m_initialized = true;
    m_enabled = true;
    ProxyLog("[SCALER] Initialization complete!");
    
    return true;
}

void LetterboxScaler::Release() {
    ProxyLog("[SCALER] Releasing resources...");
    
    // Release in reverse order of creation
    if (m_pQuadVB) {
        m_pQuadVB->Release();
        m_pQuadVB = nullptr;
    }
    
    if (m_pOriginalDepthStencil) {
        m_pOriginalDepthStencil->Release();
        m_pOriginalDepthStencil = nullptr;
    }
    
    if (m_pOriginalRenderTarget) {
        m_pOriginalRenderTarget->Release();
        m_pOriginalRenderTarget = nullptr;
    }
    
    if (m_pBackBuffer) {
        m_pBackBuffer->Release();
        m_pBackBuffer = nullptr;
    }
    
    if (m_pOffscreenDepthStencil) {
        m_pOffscreenDepthStencil->Release();
        m_pOffscreenDepthStencil = nullptr;
    }
    
    if (m_pOffscreenSurface) {
        m_pOffscreenSurface->Release();
        m_pOffscreenSurface = nullptr;
    }
    
    if (m_pOffscreenTexture) {
        m_pOffscreenTexture->Release();
        m_pOffscreenTexture = nullptr;
    }
    
    m_initialized = false;
    m_enabled = false;
    m_inFrame = false;
    
    ProxyLog("[SCALER] Resources released.");
}

bool LetterboxScaler::CreateRenderTarget(IDirect3DDevice9* pDevice) {
    HRESULT hr;
    
    // Get the current backbuffer format
    IDirect3DSurface9* pBackBuffer = nullptr;
    hr = pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
    if (FAILED(hr)) {
        ProxyLog("[SCALER] ERROR: GetBackBuffer failed: 0x%08X", hr);
        return false;
    }
    
    D3DSURFACE_DESC bbDesc;
    pBackBuffer->GetDesc(&bbDesc);
    pBackBuffer->Release();
    
    ProxyLog("[SCALER] Backbuffer format: %d, size: %ux%u", bbDesc.Format, bbDesc.Width, bbDesc.Height);
    
    // Create render target texture at native resolution
    // Using D3DUSAGE_RENDERTARGET so we can render to it
    hr = pDevice->CreateTexture(
        m_nativeWidth, m_nativeHeight,
        1,                          // Mip levels
        D3DUSAGE_RENDERTARGET,      // Usage - render target
        bbDesc.Format,              // Same format as backbuffer
        D3DPOOL_DEFAULT,            // Must be DEFAULT for render targets
        &m_pOffscreenTexture,
        nullptr
    );
    
    if (FAILED(hr)) {
        ProxyLog("[SCALER] ERROR: CreateTexture (render target) failed: 0x%08X", hr);
        return false;
    }
    
    ProxyLog("[SCALER] Offscreen texture created: %p (%ux%u)", m_pOffscreenTexture, m_nativeWidth, m_nativeHeight);
    
    // Get the surface from the texture
    hr = m_pOffscreenTexture->GetSurfaceLevel(0, &m_pOffscreenSurface);
    if (FAILED(hr)) {
        ProxyLog("[SCALER] ERROR: GetSurfaceLevel failed: 0x%08X", hr);
        return false;
    }
    
    ProxyLog("[SCALER] Offscreen surface: %p", m_pOffscreenSurface);
    
    // Check if the game uses a depth stencil buffer
    IDirect3DSurface9* pCurrentDS = nullptr;
    hr = pDevice->GetDepthStencilSurface(&pCurrentDS);
    if (SUCCEEDED(hr) && pCurrentDS) {
        D3DSURFACE_DESC dsDesc;
        pCurrentDS->GetDesc(&dsDesc);
        pCurrentDS->Release();
        
        ProxyLog("[SCALER] Game uses depth stencil: format=%d, size=%ux%u", dsDesc.Format, dsDesc.Width, dsDesc.Height);
        
        // Create a matching depth stencil for our offscreen target
        hr = pDevice->CreateDepthStencilSurface(
            m_nativeWidth, m_nativeHeight,
            dsDesc.Format,
            dsDesc.MultiSampleType,
            dsDesc.MultiSampleQuality,
            TRUE,  // Discard - we don't need to preserve it
            &m_pOffscreenDepthStencil,
            nullptr
        );
        
        if (FAILED(hr)) {
            ProxyLog("[SCALER] WARNING: CreateDepthStencilSurface failed: 0x%08X (continuing without)", hr);
            // Not fatal - some games don't need depth
        } else {
            ProxyLog("[SCALER] Offscreen depth stencil created: %p", m_pOffscreenDepthStencil);
        }
    } else {
        ProxyLog("[SCALER] Game does not use depth stencil buffer");
    }
    
    return true;
}

bool LetterboxScaler::CreateQuadVertexBuffer(IDirect3DDevice9* pDevice) {
    HRESULT hr;
    
    // Create vertex buffer for 4 vertices (triangle strip)
    hr = pDevice->CreateVertexBuffer(
        4 * sizeof(ScaledVertex),
        D3DUSAGE_WRITEONLY,
        SCALED_VERTEX_FVF,
        D3DPOOL_DEFAULT,
        &m_pQuadVB,
        nullptr
    );
    
    if (FAILED(hr)) {
        ProxyLog("[SCALER] ERROR: CreateVertexBuffer failed: 0x%08X", hr);
        return false;
    }
    
    ProxyLog("[SCALER] Quad vertex buffer created: %p", m_pQuadVB);
    
    // Fill the vertex buffer - we'll update coordinates in EndFrame based on target size
    // For now, just create it
    return true;
}

void LetterboxScaler::SetTargetSize(UINT width, UINT height) {
    if (m_targetWidth != width || m_targetHeight != height) {
        ProxyLog("[SCALER] Target size changed: %ux%u -> %ux%u", m_targetWidth, m_targetHeight, width, height);
        m_targetWidth = width;
        m_targetHeight = height;
    }
}

void LetterboxScaler::SetEnabled(bool enabled) {
    if (m_enabled != enabled) {
        ProxyLog("[SCALER] %s", enabled ? "Enabled" : "Disabled");
        m_enabled = enabled;
    }
}

void LetterboxScaler::BeginFrame(IDirect3DDevice9* pDevice) {
    if (!m_initialized || !m_enabled || m_inFrame) {
        return;
    }
    
    HRESULT hr;
    
    // Save the original render target
    hr = pDevice->GetRenderTarget(0, &m_pOriginalRenderTarget);
    if (FAILED(hr)) {
        ProxyLog("[SCALER] ERROR: GetRenderTarget failed: 0x%08X", hr);
        return;
    }
    
    // Save the original depth stencil (if any)
    m_pOriginalDepthStencil = nullptr;
    pDevice->GetDepthStencilSurface(&m_pOriginalDepthStencil);
    
    // Set our offscreen surface as the render target
    hr = pDevice->SetRenderTarget(0, m_pOffscreenSurface);
    if (FAILED(hr)) {
        ProxyLog("[SCALER] ERROR: SetRenderTarget (offscreen) failed: 0x%08X", hr);
        m_pOriginalRenderTarget->Release();
        m_pOriginalRenderTarget = nullptr;
        if (m_pOriginalDepthStencil) {
            m_pOriginalDepthStencil->Release();
            m_pOriginalDepthStencil = nullptr;
        }
        return;
    }
    
    // Set our offscreen depth stencil (if we have one)
    if (m_pOffscreenDepthStencil) {
        pDevice->SetDepthStencilSurface(m_pOffscreenDepthStencil);
    }
    
    // Set viewport to match native resolution
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = m_nativeWidth;
    vp.Height = m_nativeHeight;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    pDevice->SetViewport(&vp);
    
    m_inFrame = true;
}

void LetterboxScaler::EndFrame(IDirect3DDevice9* pDevice) {
    if (!m_initialized || !m_enabled || !m_inFrame) {
        return;
    }
    
    m_inFrame = false;
    
    if (!m_pOriginalRenderTarget) {
        return;
    }
    
    HRESULT hr;
    
    // Restore the original render target
    hr = pDevice->SetRenderTarget(0, m_pOriginalRenderTarget);
    if (FAILED(hr)) {
        ProxyLog("[SCALER] ERROR: SetRenderTarget (restore) failed: 0x%08X", hr);
    }
    
    // Restore the original depth stencil
    if (m_pOriginalDepthStencil) {
        pDevice->SetDepthStencilSurface(m_pOriginalDepthStencil);
        m_pOriginalDepthStencil->Release();
        m_pOriginalDepthStencil = nullptr;
    } else {
        pDevice->SetDepthStencilSurface(nullptr);
    }
    
    // Get backbuffer dimensions for proper scaling
    D3DSURFACE_DESC bbDesc;
    m_pOriginalRenderTarget->GetDesc(&bbDesc);
    
    // Release our reference to the original RT
    m_pOriginalRenderTarget->Release();
    m_pOriginalRenderTarget = nullptr;
    
    // Log the sizes (only every 100 frames to reduce spam)
    static int frameNum = 0;
    if (frameNum++ % 100 == 0) {
        ProxyLog("[SCALER] EndFrame: offscreen=%ux%u, backbuffer=%ux%u", 
                 m_nativeWidth, m_nativeHeight, bbDesc.Width, bbDesc.Height);
    }
    
    // Clear the backbuffer to black (for letterbox bars if any)
    pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    
    // Calculate letterbox/pillarbox dimensions
    float targetAspect = (float)m_nativeWidth / (float)m_nativeHeight;
    float screenAspect = (float)bbDesc.Width / (float)bbDesc.Height;
    
    UINT destX, destY, destW, destH;
    
    if (screenAspect > targetAspect) {
        // Screen is wider than content - pillarbox (bars on sides)
        destH = bbDesc.Height;
        destW = (UINT)(bbDesc.Height * targetAspect);
        destX = (bbDesc.Width - destW) / 2;
        destY = 0;
    } else {
        // Screen is taller than content - letterbox (bars on top/bottom)
        destW = bbDesc.Width;
        destH = (UINT)(bbDesc.Width / targetAspect);
        destX = 0;
        destY = (bbDesc.Height - destH) / 2;
    }
    
    // Set viewport to full backbuffer
    D3DVIEWPORT9 fullVP;
    fullVP.X = 0;
    fullVP.Y = 0;
    fullVP.Width = bbDesc.Width;
    fullVP.Height = bbDesc.Height;
    fullVP.MinZ = 0.0f;
    fullVP.MaxZ = 1.0f;
    pDevice->SetViewport(&fullVP);
    
    // Update vertex buffer with correct coordinates
    ScaledVertex* vertices = nullptr;
    hr = m_pQuadVB->Lock(0, 0, (void**)&vertices, D3DLOCK_DISCARD);
    if (SUCCEEDED(hr)) {
        // D3D9 pixel-perfect rendering requires -0.5 offset
        float left = (float)destX - 0.5f;
        float top = (float)destY - 0.5f;
        float right = (float)(destX + destW) - 0.5f;
        float bottom = (float)(destY + destH) - 0.5f;
        
        // Triangle strip: TL, TR, BL, BR
        vertices[0] = { left,  top,    0.0f, 1.0f, 0.0f, 0.0f };  // Top-left
        vertices[1] = { right, top,    0.0f, 1.0f, 1.0f, 0.0f };  // Top-right
        vertices[2] = { left,  bottom, 0.0f, 1.0f, 0.0f, 1.0f };  // Bottom-left
        vertices[3] = { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f };  // Bottom-right
        
        m_pQuadVB->Unlock();
    } else {
        ProxyLog("[SCALER] ERROR: VB Lock failed: 0x%08X", hr);
        return;
    }
    
    // Draw the scaled quad
    DrawScaledQuad(pDevice);
}

void LetterboxScaler::DrawScaledQuad(IDirect3DDevice9* pDevice) {
    // Save state
    IDirect3DStateBlock9* pStateBlock = nullptr;
    pDevice->CreateStateBlock(D3DSBT_ALL, &pStateBlock);
    
    // Set up render states for textured quad
    pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    pDevice->SetRenderState(D3DRS_CLIPPING, FALSE);
    
    // Disable alpha test
    pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    
    // Set texture
    pDevice->SetTexture(0, m_pOffscreenTexture);
    
    // Set sampler states for bilinear filtering (smooth scaling)
    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    
    // Set texture stage states
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    
    // Draw
    pDevice->SetStreamSource(0, m_pQuadVB, 0, sizeof(ScaledVertex));
    pDevice->SetFVF(SCALED_VERTEX_FVF);
    HRESULT hr = pDevice->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    
    if (FAILED(hr)) {
        static int errorCount = 0;
        if (errorCount < 5) {
            ProxyLog("[SCALER] ERROR: DrawPrimitive failed: 0x%08X", hr);
            errorCount++;
        }
    }
    
    // Clear texture
    pDevice->SetTexture(0, nullptr);
    
    // Restore state
    if (pStateBlock) {
        pStateBlock->Apply();
        pStateBlock->Release();
    }
}

void LetterboxScaler::SetBackBuffer(IDirect3DSurface9* pBackBuffer) {
    // Release old reference if any
    if (m_pBackBuffer) {
        m_pBackBuffer->Release();
        m_pBackBuffer = nullptr;
    }
    
    // Store new reference
    if (pBackBuffer) {
        pBackBuffer->AddRef();
        m_pBackBuffer = pBackBuffer;
        ProxyLog("[SCALER] Backbuffer tracked: %p", m_pBackBuffer);
    }
}

bool LetterboxScaler::IsBackBuffer(IDirect3DSurface9* pSurface) const {
    if (!pSurface) return false;
    
    // Check if this is our tracked backbuffer
    if (m_pBackBuffer && pSurface == m_pBackBuffer) {
        return true;
    }
    
    // Also check the original render target we saved in BeginFrame
    // (this catches cases where the backbuffer wasn't explicitly tracked)
    if (m_pOriginalRenderTarget && pSurface == m_pOriginalRenderTarget) {
        return true;
    }
    
    return false;
}
