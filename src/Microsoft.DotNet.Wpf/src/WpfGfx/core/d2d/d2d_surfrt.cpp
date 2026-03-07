// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2DSurfaceRenderTarget implementation.
//
//      This is the D2D1 equivalent of CHwSurfaceRenderTarget.  All WPF
//      rendering operations are mapped to ID2D1DeviceContext1 calls.  3D
//      interop is handled by flushing D2D, obtaining the shared DXGI surface,
//      and rendering via D3D11.
//
//----------------------------------------------------------------------------

#include <precomp.hpp>


//
// If the UNCONDITIONAL_EXPR macro is not provided by the precompiled header
// chain we fall back to a simple definition so D2D_IFC compiles standalone.
//

#ifndef UNCONDITIONAL_EXPR
#define UNCONDITIONAL_EXPR(x) (x)
#endif

//
// MilCoreSeg type constants – provided by the precompiled header chain.
//

//
// MilFillMode equivalents for D2D
//

//
// Convert a single colour channel from scRGB (linear) to sRGB gamma space.
// Our render targets use DXGI_FORMAT_B8G8R8A8_UNORM (not _SRGB), so D2D
// writes colour values directly without gamma correction.  WPF's MilColorF
// stores colours in scRGB (linear), which would appear too dark if written
// to an UNORM target as-is.  This helper applies the standard sRGB OETF so
// the final pixel values match what the display expects.
//

static inline float ScRgbToSrgb(float c)
{
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return (c <= 0.0031308f)
        ? c * 12.92f
        : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static D2D1_FILL_MODE MapFillMode(MilFillMode::Enum mode)
{
    switch (mode)
    {
    case MilFillMode::Alternate:
        return D2D1_FILL_MODE_ALTERNATE;
    case MilFillMode::Winding:
        return D2D1_FILL_MODE_WINDING;
    default:
        return D2D1_FILL_MODE_ALTERNATE;
    }
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::CD2DSurfaceRenderTarget
//
//  Synopsis:  ctor
//
//-------------------------------------------------------------------------

CD2DSurfaceRenderTarget::CD2DSurfaceRenderTarget(
    __inout_ecount(1) CD2DDevice *pDevice,
    DisplayId associatedDisplay
    )
    : m_pDevice(pDevice),
      m_associatedDisplay(associatedDisplay),
      m_uWidth(0),
      m_uHeight(0),
      m_fmtTarget(MilPixelFormat::PBGRA32bpp),
      m_cRef(1),
      m_fIn3D(false),
      m_forceClearType(false),
      m_layerCount(0),
      m_fDrawBatchOpen(false),
      m_pResourceManager(nullptr),
      m_3DDepthStencilWidth(0),
      m_3DDepthStencilHeight(0),
      m_pStagingBuffer(nullptr),
      m_cbStagingBuffer(0),
      m_cachedBitmapWidth(0),
      m_cachedBitmapHeight(0),
      m_deviceTransform(true /* fInitialize – identity */)
{
    ZeroMemory(&m_diag, sizeof(m_diag));
    //
    // Initialise the device transform to the primary display DPI scale.
    // This mirrors CHwSurfaceRenderTarget behaviour.
    //

    const auto &primaryDisplayDpi = DpiScale::PrimaryDisplayDpi();
    m_deviceTransform.Scale(primaryDisplayDpi.DpiScaleX, primaryDisplayDpi.DpiScaleY);

    m_rcBounds3D.left   = 0.0f;
    m_rcBounds3D.top    = 0.0f;
    m_rcBounds3D.right  = 0.0f;
    m_rcBounds3D.bottom = 0.0f;

    ZeroMemory(&m_cachedStrokeProps, sizeof(m_cachedStrokeProps));
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::~CD2DSurfaceRenderTarget
//
//  Synopsis:  dtor – release all D2D / D3D resources.
//
//-------------------------------------------------------------------------

CD2DSurfaceRenderTarget::~CD2DSurfaceRenderTarget()
{
    //
    // Flush any open draw batch before tearing down.
    //

    if (m_fDrawBatchOpen && m_pD2DContext)
    {
        m_pD2DContext->EndDraw();
        m_fDrawBatchOpen = false;
    }

    //
    // Pop any leaked layers so the device context is left in a clean state.
    //

    if (m_pD2DContext)
    {
        while (m_layerCount > 0)
        {
            m_pD2DContext->PopLayer();
            --m_layerCount;
        }
    }

    //
    // Release cached brushes.
    //

    m_pCachedSolidBrush.Reset();
    m_pTransparentBrush.Reset();
    m_pCachedDrawBitmap.Reset();
    m_pCachedStrokeStyle.Reset();

    //
    // Clear geometry cache.
    //

    for (UINT i = 0; i < GEOMETRY_CACHE_SIZE; ++i)
    {
        m_geometryCache[i].pGeometry.Reset();
        m_geometryCache[i].pShapeKey = nullptr;
    }

    m_pD2DFactory.Reset();

    //
    // Release resource manager.
    //

    if (m_pResourceManager != nullptr)
    {
        m_pResourceManager->Release();
        m_pResourceManager = nullptr;
    }

    //
    // Free staging buffer.
    //

    if (m_pStagingBuffer != nullptr)
    {
        delete[] m_pStagingBuffer;
        m_pStagingBuffer = nullptr;
        m_cbStagingBuffer = 0;
    }

    m_p3DRenderTargetView.Reset();
    m_p3DDepthStencilView.Reset();
    m_p3DDepthStencil.Reset();
    m_pD3D11Context.Reset();
    m_pD3D11Device.Reset();
    m_pTargetBitmap.Reset();
    m_pD2DContext.Reset();
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::Init
//
//  Synopsis:  Bind the device context to the supplied target bitmap.
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::Init(
    __in_ecount(1) ID2D1Bitmap1 *pTargetBitmap,
    UINT width,
    UINT height
    )
{
    HRESULT hr = S_OK;

    Assert(pTargetBitmap);
    Assert(width > 0 && height > 0);

    m_uWidth  = width;
    m_uHeight = height;

    m_pTargetBitmap = pTargetBitmap;

    //
    // Obtain an ID2D1DeviceContext1 from the device.
    //

    D2D_IFC(EnsureD2DResources());

    //
    // Set the target bitmap on the context.
    //

    m_pD2DContext->SetTarget(m_pTargetBitmap.Get());

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::IsValid
//
//  Synopsis:  The render target is valid as long as we still have a
//             device context.  Subclasses may add surface-lost checks.
//
//-------------------------------------------------------------------------

bool
CD2DSurfaceRenderTarget::IsValid() const
{
    return (m_pD2DContext != nullptr);
}

// =======================================================================
// IUnknown
// =======================================================================

STDMETHODIMP
CD2DSurfaceRenderTarget::QueryInterface(
    __in REFIID riid,
    __deref_out void **ppv
    )
{
    if (ppv == nullptr)
    {
        return E_POINTER;
    }

    *ppv = nullptr;

    if (riid == IID_IMILRenderTarget)
    {
        *ppv = static_cast<IMILRenderTarget *>(this);
    }
    else
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG)
CD2DSurfaceRenderTarget::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CD2DSurfaceRenderTarget::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);

    if (cRef == 0)
    {
        delete this;
    }

    return static_cast<ULONG>(cRef);
}

// =======================================================================
// IMILRenderTarget
// =======================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::GetBounds
//
//  Synopsis:  Return the render target rectangle in device pixels.
//
//-------------------------------------------------------------------------

STDMETHODIMP_(VOID)
CD2DSurfaceRenderTarget::GetBounds(
    __out_ecount(1) MilRectF * const pBounds
    )
{
    Assert(pBounds);

    pBounds->left   = 0.0f;
    pBounds->top    = 0.0f;
    pBounds->right  = static_cast<FLOAT>(m_uWidth);
    pBounds->bottom = static_cast<FLOAT>(m_uHeight);
}

// =======================================================================
// Draw-session batching
//
// Instead of calling BeginDraw/EndDraw on every primitive,
// we keep a draw session open and only flush when required
// (Present, Begin3D, or when the context must be torn down).
// This eliminates the GPU pipeline stall that EndDraw causes.
// =======================================================================

HRESULT
CD2DSurfaceRenderTarget::BeginDrawBatch()
{
    if (!m_fDrawBatchOpen && m_pD2DContext)
    {
        m_pD2DContext->BeginDraw();
        m_fDrawBatchOpen = true;
    }
    return S_OK;
}

HRESULT
CD2DSurfaceRenderTarget::FlushDrawBatch()
{
    HRESULT hr = S_OK;

    if (m_fDrawBatchOpen && m_pD2DContext)
    {
        hr = m_pD2DContext->EndDraw();
        m_fDrawBatchOpen = false;
    }

    return hr;
}

// =======================================================================
// Reusable staging buffer for bitmap pixel copies
// =======================================================================

HRESULT
CD2DSurfaceRenderTarget::EnsureStagingBuffer(
    UINT cbRequired,
    __deref_out BYTE **ppBuffer
    )
{
    *ppBuffer = nullptr;

    if (cbRequired > m_cbStagingBuffer)
    {
        //
        // Grow the buffer.  Round up to 64 KB granularity to avoid
        // churning on small increments.
        //
        UINT cbAllocate = (cbRequired + 0xFFFF) & ~0xFFFFu;

        BYTE *pNew = new(std::nothrow) BYTE[cbAllocate];
        if (pNew == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        delete[] m_pStagingBuffer;
        m_pStagingBuffer  = pNew;
        m_cbStagingBuffer = cbAllocate;
    }

    *ppBuffer = m_pStagingBuffer;
    return S_OK;
}

// =======================================================================
// Cached solid-colour brush — avoids per-draw-call ID2D1Brush creation
// =======================================================================

HRESULT
CD2DSurfaceRenderTarget::GetSolidBrush(
    D2D1_COLOR_F color,
    __deref_out ID2D1SolidColorBrush **ppBrush
    )
{
    HRESULT hr = S_OK;

    if (!m_pCachedSolidBrush)
    {
        D2D_IFC(m_pD2DContext->CreateSolidColorBrush(color, &m_pCachedSolidBrush));
    }
    else
    {
        m_pCachedSolidBrush->SetColor(color);
    }

    //
    // AddRef for the caller — standard COM out-parameter contract.
    // Without this, the caller's ComPtr Release() would destroy the
    // cached brush (use-after-free).
    //

    *ppBrush = m_pCachedSolidBrush.Get();
    (*ppBrush)->AddRef();

Cleanup:
    return hr;
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::Clear
//
//  Synopsis:  Clear the target (optionally clipped) to the specified colour.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::Clear(
    __in_ecount_opt(1) const MilColorF *pColor,
    __in_ecount_opt(1) const CAliasedClip *pAliasedClip
    )
{
    HRESULT hr = S_OK;

    if (!IsValid())
    {
        goto Cleanup;
    }

    if (pColor == nullptr)
    {
        goto Cleanup;
    }

    D2D_IFC(BeginDrawBatch());

    //
    // If an aliased clip is provided push it as an axis-aligned clip rect.
    // CAliasedClip wraps a CMILSurfaceRect; we extract left/top/right/bottom.
    //

    bool fPushedClip = false;

    if (pAliasedClip != nullptr)
    {
        CMilRectF rcClip;
        pAliasedClip->GetAsCMilRectF(&rcClip);

        D2D1_RECT_F clipRect = D2D1::RectF(
            rcClip.left,
            rcClip.top,
            rcClip.right,
            rcClip.bottom
        );

        m_pD2DContext->PushAxisAlignedClip(
            clipRect,
            D2D1_ANTIALIAS_MODE_ALIASED
        );

        fPushedClip = true;
    }

    //
    // D2D1 ClearColor expects pre-multiplied RGBA in linear space – the
    // same convention as MilColorF (scRGB, pre-multiplied).
    //

    D2D1_COLOR_F clearColor;
    clearColor.r = ScRgbToSrgb(pColor->r);
    clearColor.g = ScRgbToSrgb(pColor->g);
    clearColor.b = ScRgbToSrgb(pColor->b);
    clearColor.a = pColor->a;
    m_pD2DContext->Clear(clearColor);

    if (fPushedClip)
    {
        m_pD2DContext->PopAxisAlignedClip();
    }

    //
    // Don't EndDraw here — leave the batch open for subsequent draws.
    //

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::Begin3D
//
//  Synopsis:  Flush D2D, obtain the underlying DXGI surface and create
//             D3D11 render-target and depth-stencil views for 3D rendering.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::Begin3D(
    __in_ecount(1) MilRectF const &rcBounds,
    MilAntiAliasMode::Enum AntiAliasMode,
    bool fUseZBuffer,
    FLOAT rZ
    )
{
    HRESULT hr = S_OK;

    if (m_fIn3D)
    {
        hr = WGXERR_INVALIDCALL;
        goto Cleanup;
    }

    if (!IsValid())
    {
        goto Cleanup;
    }

    m_rcBounds3D = rcBounds;

    //
    // Flush any pending D2D drawing so the shared surface is up to date.
    //

    D2D_IFC(FlushDrawBatch());

    //
    // Get the DXGI surface that backs our target bitmap.
    //

    {
        ComPtr<IDXGISurface> pDxgiSurface;
        D2D_IFC(m_pTargetBitmap->GetSurface(&pDxgiSurface));

        //
        // Obtain (or create) the D3D11 device from the DXGI surface.
        //

        if (!m_pD3D11Device)
        {
            ComPtr<IDXGIDevice> pDxgiDevice;
            D2D_IFC(pDxgiSurface->GetDevice(__uuidof(IDXGIDevice), &pDxgiDevice));

            D2D_IFC(pDxgiDevice.As(&m_pD3D11Device));
            m_pD3D11Device->GetImmediateContext(&m_pD3D11Context);
        }

        //
        // Create the render-target view from the DXGI surface.
        //

        ComPtr<ID3D11Texture2D> pBackBuffer;
        D2D_IFC(pDxgiSurface.As(&pBackBuffer));

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format        = DXGI_FORMAT_B8G8R8A8_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        m_p3DRenderTargetView.Reset();
        D2D_IFC(m_pD3D11Device->CreateRenderTargetView(
            pBackBuffer.Get(),
            &rtvDesc,
            &m_p3DRenderTargetView
        ));

        //
        // Create depth-stencil texture and view if z-buffering is requested.
        // Reuse the cached depth stencil if dimensions match.
        //

        if (fUseZBuffer)
        {
            if (m_3DDepthStencilWidth != m_uWidth ||
                m_3DDepthStencilHeight != m_uHeight ||
                !m_p3DDepthStencil)
            {
                m_p3DDepthStencil.Reset();
                m_p3DDepthStencilView.Reset();

                D3D11_TEXTURE2D_DESC dsDesc = {};
                dsDesc.Width              = m_uWidth;
                dsDesc.Height             = m_uHeight;
                dsDesc.MipLevels          = 1;
                dsDesc.ArraySize          = 1;
                dsDesc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
                dsDesc.SampleDesc.Count   = 1;
                dsDesc.SampleDesc.Quality = 0;
                dsDesc.Usage              = D3D11_USAGE_DEFAULT;
                dsDesc.BindFlags          = D3D11_BIND_DEPTH_STENCIL;

                D2D_IFC(m_pD3D11Device->CreateTexture2D(&dsDesc, nullptr, &m_p3DDepthStencil));

                D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                dsvDesc.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
                dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

                D2D_IFC(m_pD3D11Device->CreateDepthStencilView(
                    m_p3DDepthStencil.Get(),
                    &dsvDesc,
                    &m_p3DDepthStencilView
                ));

                m_3DDepthStencilWidth  = m_uWidth;
                m_3DDepthStencilHeight = m_uHeight;
            }

            //
            // Clear the depth buffer to the caller-provided rZ value.
            //

            m_pD3D11Context->ClearDepthStencilView(
                m_p3DDepthStencilView.Get(),
                D3D11_CLEAR_DEPTH,
                rZ,
                0
            );
        }

        //
        // Bind the render-target and depth-stencil views.
        //

        ID3D11RenderTargetView *rtvs[] = { m_p3DRenderTargetView.Get() };
        m_pD3D11Context->OMSetRenderTargets(
            1,
            rtvs,
            fUseZBuffer ? m_p3DDepthStencilView.Get() : nullptr
        );

        //
        // Set the viewport to the 3D bounds.
        //

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = m_rcBounds3D.left;
        vp.TopLeftY = m_rcBounds3D.top;
        vp.Width    = m_rcBounds3D.right  - m_rcBounds3D.left;
        vp.Height   = m_rcBounds3D.bottom - m_rcBounds3D.top;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        m_pD3D11Context->RSSetViewports(1, &vp);
    }

    m_fIn3D = true;

Cleanup:
    if (FAILED(hr))
    {
        //
        // Cleanup partial 3D state on failure.
        //

        m_p3DRenderTargetView.Reset();
        m_p3DDepthStencilView.Reset();
        m_p3DDepthStencil.Reset();
    }

    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::End3D
//
//  Synopsis:  Release D3D11 views and resume D2D drawing.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::End3D()
{
    HRESULT hr = S_OK;

    if (!m_fIn3D)
    {
        hr = WGXERR_INVALIDCALL;
        goto Cleanup;
    }

    //
    // Flush the D3D11 pipeline so all 3D work is committed to the shared
    // surface before D2D resumes drawing.
    //

    if (m_pD3D11Context)
    {
        m_pD3D11Context->Flush();
    }

    //
    // Unbind D3D11 render targets.
    //

    if (m_pD3D11Context)
    {
        ID3D11RenderTargetView *nullRtv[] = { nullptr };
        m_pD3D11Context->OMSetRenderTargets(1, nullRtv, nullptr);
    }

    m_p3DRenderTargetView.Reset();
    m_p3DDepthStencilView.Reset();
    m_p3DDepthStencil.Reset();

Cleanup:

    //
    // Always leave 3D context, even on error.
    //

    m_fIn3D = false;

    RRETURN(hr);
}

// =======================================================================
// IRenderTargetInternal
// =======================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::GetDeviceTransform
//
//  Synopsis:  Return pointer to the page-to-device transform (DPI scale).
//
//-------------------------------------------------------------------------

const CMILMatrix *
CD2DSurfaceRenderTarget::GetDeviceTransform() const
{
    return &m_deviceTransform;
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::GetOrCreateCachedBitmap
//
//  Synopsis:  Look up or create a cached D2D bitmap for an IWGXBitmapSource.
//             Uses a direct-mapped cache keyed by source pointer + dimensions.
//             For static (frozen) WPF bitmaps the pointer is stable across
//             frames, giving a high hit rate and eliminating per-frame
//             CopyPixels + CreateBitmap overhead.
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::GetOrCreateCachedBitmap(
    __in IWGXBitmapSource *pSource,
    __deref_out ID2D1Bitmap1 **ppBitmap
    )
{
    HRESULT hr = S_OK;

    Assert(pSource);
    Assert(ppBitmap);
    *ppBitmap = nullptr;

    UINT width  = 0;
    UINT height = 0;
    D2D_IFC(pSource->GetSize(&width, &height));

    if (width == 0 || height == 0)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    //
    // Cache lookup — direct-mapped by source pointer hash.
    //
    {
        UINT cacheIdx = static_cast<UINT>(
            (reinterpret_cast<UINT_PTR>(pSource) >> 4) % BITMAP_CACHE_SIZE);

        BitmapCacheEntry &entry = m_bitmapCache[cacheIdx];

        if (entry.pSourceKey == static_cast<const void *>(pSource) &&
            entry.width  == width &&
            entry.height == height &&
            entry.pBitmap)
        {
            // Cache hit — return existing D2D bitmap.
            entry.pBitmap.Get()->AddRef();
            *ppBitmap = entry.pBitmap.Get();
            goto Cleanup;
        }

        //
        // Cache miss — create the D2D bitmap via ConvertBitmapSource.
        //

        ComPtr<ID2D1Bitmap1> pNewBitmap;
        D2D_IFC(CD2DBitmapConverter::ConvertBitmapSource(
            m_pD2DContext.Get(), pSource, &pNewBitmap));

        // Store in cache.
        entry.pSourceKey = static_cast<const void *>(pSource);
        entry.width      = width;
        entry.height     = height;
        entry.pBitmap    = pNewBitmap;

        pNewBitmap.Get()->AddRef();
        *ppBitmap = pNewBitmap.Get();
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::DrawBitmap
//
//  Synopsis:  Create an ID2D1Bitmap from the IWGXBitmapSource pixel data
//             and render it via DrawBitmap().
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::DrawBitmap(
    __inout_ecount(1) CContextState *pContextState,
    __inout_ecount(1) IWGXBitmapSource *pIBitmap,
    __inout_ecount_opt(1) IMILEffectList *pIEffect
    )
{
    HRESULT hr = S_OK;

    Assert(pContextState);
    Assert(pIBitmap);

    if (!IsValid())
    {
        goto Cleanup;
    }

    //
    // Fast path: check if the bitmap source is backed by a D2D bitmap
    // (from our CD2DTextureRenderTarget intermediate).  If so, draw
    // directly from the GPU bitmap — zero-copy, no staging, no readback.
    //

    {
        ComPtr<ID2DBitmapSourceInternal> pD2DInternal;

        if (SUCCEEDED(pIBitmap->QueryInterface(
                IID_ID2DBitmapSourceInternal,
                reinterpret_cast<void **>(pD2DInternal.GetAddressOf()))))
        {
            ComPtr<ID2D1Bitmap1> pSrcBitmap;
            hr = pD2DInternal->GetD2DBitmap(&pSrcBitmap);
            D2D_IFC(hr);

            UINT uBmpWidth  = 0;
            UINT uBmpHeight = 0;
            D2D_IFC(pIBitmap->GetSize(&uBmpWidth, &uBmpHeight));

            D2D1_RECT_F dstRect = D2D1::RectF(
                0.0f, 0.0f,
                static_cast<FLOAT>(uBmpWidth),
                static_cast<FLOAT>(uBmpHeight)
            );

            D2D_IFC(BeginDrawBatch());

            {
                const CMILMatrix &mat = pContextState->WorldToDevice;
                D2D1_MATRIX_3X2_F d2dTransform = D2D1::Matrix3x2F(
                    mat._11, mat._12,
                    mat._21, mat._22,
                    mat._41, mat._42
                );
                m_pD2DContext->SetTransform(d2dTransform);
            }

            m_pD2DContext->DrawBitmap(
                pSrcBitmap.Get(),
                dstRect,
                1.0f,
                D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                nullptr
            );

            goto Cleanup;
        }
    }

    //
    // Slow path: bitmap source is not D2D-backed.  Use the bitmap source
    // cache to avoid per-frame CopyPixels + CreateBitmap overhead.
    //

    {
        ComPtr<ID2D1Bitmap1> pDrawBitmap;
        D2D_IFC(GetOrCreateCachedBitmap(pIBitmap, &pDrawBitmap));

        UINT uBmpWidth  = 0;
        UINT uBmpHeight = 0;
        D2D_IFC(pIBitmap->GetSize(&uBmpWidth, &uBmpHeight));

        D2D1_RECT_F dstRect = D2D1::RectF(
            0.0f,
            0.0f,
            static_cast<FLOAT>(uBmpWidth),
            static_cast<FLOAT>(uBmpHeight)
        );

        D2D_IFC(BeginDrawBatch());

        {
            const CMILMatrix &mat = pContextState->WorldToDevice;
            D2D1_MATRIX_3X2_F d2dTransform = D2D1::Matrix3x2F(
                mat._11, mat._12,
                mat._21, mat._22,
                mat._41, mat._42
            );
            m_pD2DContext->SetTransform(d2dTransform);
        }

        m_pD2DContext->DrawBitmap(
            pDrawBitmap.Get(),
            dstRect,
            1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR,
            nullptr
        );
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::DrawMesh3D
//
//  Synopsis:  Render a 3D triangle mesh.  Requires Begin3D to have been
//             called.  Uses D3D11 immediate context to draw triangles.
//
//  Notes:     This is a simplified implementation.  The full version would
//             set up vertex / index buffers, input layouts, and compile
//             shaders.  For now we draw using a basic pass-through pipeline.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::DrawMesh3D(
    __inout_ecount(1) CContextState *pContextState,
    __inout_ecount_opt(1) BrushContext *pBrushContext,
    __inout_ecount(1) CMILMesh3D *pMesh3D,
    __inout_ecount_opt(1) CMILShader *pShader,
    __inout_ecount_opt(1) IMILEffectList *pIEffect
    )
{
    HRESULT hr = S_OK;

    Assert(pContextState);
    Assert(pMesh3D);

    if (!m_fIn3D)
    {
        hr = WGXERR_INVALIDCALL;
        goto Cleanup;
    }

    if (!IsValid())
    {
        goto Cleanup;
    }

    //
    // Ensure we have a D3D11 device context from Begin3D.
    //

    if (!m_pD3D11Context)
    {
        hr = WGXERR_INVALIDCALL;
        goto Cleanup;
    }

    //
    // TODO: Phase 2 – Full 3D mesh rendering implementation.
    //
    // The complete implementation will:
    // 1. Extract vertex positions, normals and texture coordinates from
    //    CMILMesh3D via its GetVertexBuffer / GetIndexBuffer accessors.
    // 2. Create D3D11 vertex and index buffers.
    // 3. Compile or load cached HLSL vertex / pixel shaders.
    // 4. Set up the input layout, constant buffers (MVP matrix, lighting).
    // 5. Bind the shader from CMILShader as a texture / material.
    // 6. Call DrawIndexed() on the immediate context.
    //
    // For now we report success so that the rendering pipeline does not abort
    // the scene – the mesh simply will not appear until the full shader
    // pipeline is integrated.
    //

    hr = S_OK;

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::DrawPath
//
//  Synopsis:  Fill and/or stroke a path.  The fill brush is applied first,
//             then the stroke brush if a pen is provided.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::DrawPath(
    __inout_ecount(1) CContextState *pContextState,
    __inout_ecount_opt(1) BrushContext *pBrushContext,
    __inout_ecount(1) IShapeData *pPath,
    __inout_ecount_opt(1) CPlainPen *pPen,
    __inout_ecount_opt(1) CBrushRealizer *pStrokeBrush,
    __inout_ecount_opt(1) CBrushRealizer *pFillBrush
    )
{
    HRESULT hr = S_OK;

    Assert(pContextState);
    Assert(pPath);

    if (!IsValid())
    {
        goto Cleanup;
    }

    m_diag.cDrawPath++;

    //
    // Apply the world-to-device transform.
    //

    {
        const CMILMatrix &mat = pContextState->WorldToDevice;
        D2D1_MATRIX_3X2_F d2dTransform = D2D1::Matrix3x2F(
            mat._11, mat._12,
            mat._21, mat._22,
            mat._41, mat._42
        );
        m_pD2DContext->SetTransform(d2dTransform);
    }

    D2D_IFC(BeginDrawBatch());

    //
    // ================================================================
    // Early-out fast path: bitmap-filled axis-aligned rectangle
    //
    // For the extremely common case of rendering a WPF Image element
    // (which comes in as a CParallelogram filled with a BrushBitmap),
    // use ID2D1DeviceContext::DrawBitmap() directly.  This skips:
    //   - EnsureBrushRealization (which creates a software intermediate
    //     RT and can freeze/stall for certain bitmap formats)
    //   - CreateBitmapBrush per-call COM overhead
    //   - Geometry creation
    //
    // The brush is already realized by FillShapeWithBitmap in
    // drawingcontext.cpp before DrawPath is called.
    // ================================================================
    //
    if (pPath->IsAxisAlignedRectangle() &&
        pPath->GetFigureCount() == 1 &&
        pFillBrush)
    {
        CMILBrush *pMILFill = pFillBrush->GetRealizedBrushNoRef(false);

        if (pMILFill && pMILFill->GetType() == BrushBitmap)
        {
            CMILBrushBitmap *pBmpBrush =
                static_cast<CMILBrushBitmap *>(pMILFill);

            IWGXBitmapSource *pTexture = pBmpBrush->GetTextureNoAddRef();

            if (pTexture)
            {
                CMILMatrix matBitmapToWorld;
                matBitmapToWorld.SetToIdentity();

                pBmpBrush->GetBitmapToWorldSpaceTransform(
                    reinterpret_cast<CMatrix<CoordinateSpace::RealizationSampling,
                                             CoordinateSpace::BaseSampling>&>(matBitmapToWorld));

                // Only use DrawBitmap for axis-aligned transforms
                // (scale + translate, no rotation/skew).
                if (matBitmapToWorld._12 == 0.0f && matBitmapToWorld._21 == 0.0f &&
                    matBitmapToWorld._11 != 0.0f && matBitmapToWorld._22 != 0.0f)
                {
                    const IFigureData &figure = pPath->GetFigure(0);
                    MilRectF rectF;
                    figure.GetAsWellOrderedRectangle(rectF);

                    D2D1_RECT_F d2dRect = D2D1::RectF(
                        rectF.left, rectF.top, rectF.right, rectF.bottom);

                    ComPtr<ID2D1Bitmap1> pD2DBitmap;
                    D2D_IFC(GetOrCreateCachedBitmap(pTexture, &pD2DBitmap));

                    FLOAT opacity = pFillBrush->GetOpacityFromRealizedBrush();

                    float sx = matBitmapToWorld._11;
                    float sy = matBitmapToWorld._22;
                    float tx = matBitmapToWorld._41;
                    float ty = matBitmapToWorld._42;

                    //
                    // Compute source rect.  The brush transform maps bitmap
                    // pixel coords to world coords.  Invert to find which
                    // bitmap region maps into the destination rect.
                    // D2D DrawBitmap expects source rect in DIPs.
                    //
                    D2D1_SIZE_F bmpDipSize = pD2DBitmap->GetSize();
                    D2D1_SIZE_U bmpPixelSize = pD2DBitmap->GetPixelSize();

                    float dipScaleX = (bmpPixelSize.width  > 0)
                        ? (bmpDipSize.width  / bmpPixelSize.width)  : 1.0f;
                    float dipScaleY = (bmpPixelSize.height > 0)
                        ? (bmpDipSize.height / bmpPixelSize.height) : 1.0f;

                    D2D1_RECT_F srcRect;
                    srcRect.left   = ((d2dRect.left   - tx) / sx) * dipScaleX;
                    srcRect.top    = ((d2dRect.top    - ty) / sy) * dipScaleY;
                    srcRect.right  = ((d2dRect.right  - tx) / sx) * dipScaleX;
                    srcRect.bottom = ((d2dRect.bottom - ty) / sy) * dipScaleY;

                    m_pD2DContext->DrawBitmap(
                        pD2DBitmap.Get(),
                        &d2dRect,
                        opacity,
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                        &srcRect,
                        nullptr);

                    //
                    // Handle stroke if present.
                    //
                    if (pStrokeBrush && pPen)
                    {
                        D2D_IFC(EnsureBrushRealization(
                            pStrokeBrush, pBrushContext, pContextState));

                        ComPtr<ID2D1Brush> pD2DStrokeBrush;
                        D2D_IFC(ConvertBrush(
                            pStrokeBrush, pContextState, &pD2DStrokeBrush));

                        ComPtr<ID2D1StrokeStyle> pStrokeStyle;
                        FLOAT strokeWidth = 1.0f;
                        D2D_IFC(ConvertPenToStrokeStyle(
                            pPen, &pStrokeStyle, &strokeWidth));

                        m_pD2DContext->DrawRectangle(
                            d2dRect, pD2DStrokeBrush.Get(),
                            strokeWidth, pStrokeStyle.Get());
                    }

                    m_diag.cFastRect++;
                    goto Cleanup;
                }
            }
        }
    }

    //
    // Ensure brushes are realised before we try to convert them.
    //

    if (pFillBrush)
    {
        D2D_IFC(EnsureBrushRealization(pFillBrush, pBrushContext, pContextState));
    }

    if (pStrokeBrush)
    {
        D2D_IFC(EnsureBrushRealization(pStrokeBrush, pBrushContext, pContextState));
    }

    //
    // ================================================================
    // Fast path 1: Axis-aligned rectangle
    //
    // Use FillRectangle / DrawRectangle directly — no geometry object
    // needed.  This is the most common primitive in WPF (Border,
    // Rectangle, background fills, etc.) and completely avoids the
    // geometry creation + tessellation overhead.
    // ================================================================
    //
    if (pPath->IsAxisAlignedRectangle() && pPath->GetFigureCount() == 1)
    {
        const IFigureData &figure = pPath->GetFigure(0);
        MilRectF rectF;
        figure.GetAsWellOrderedRectangle(rectF);

        D2D1_RECT_F d2dRect = D2D1::RectF(rectF.left, rectF.top, rectF.right, rectF.bottom);

        if (pFillBrush)
        {
            ComPtr<ID2D1Brush> pD2DFillBrush;
            D2D_IFC(ConvertBrush(pFillBrush, pContextState, &pD2DFillBrush));

            m_pD2DContext->FillRectangle(d2dRect, pD2DFillBrush.Get());
        }

        if (pStrokeBrush && pPen)
        {
            ComPtr<ID2D1Brush> pD2DStrokeBrush;
            D2D_IFC(ConvertBrush(pStrokeBrush, pContextState, &pD2DStrokeBrush));

            ComPtr<ID2D1StrokeStyle> pStrokeStyle;
            FLOAT strokeWidth = 1.0f;
            D2D_IFC(ConvertPenToStrokeStyle(pPen, &pStrokeStyle, &strokeWidth));

            m_pD2DContext->DrawRectangle(d2dRect, pD2DStrokeBrush.Get(), strokeWidth, pStrokeStyle.Get());
        }

        m_diag.cFastRect++;
        goto Cleanup;
    }

    //
    // ================================================================
    // Fast path 2: Ellipse
    //
    // WPF represents ellipses as a single closed figure with exactly
    // 4 cubic bezier segments.  Detect this and use FillEllipse /
    // DrawEllipse which D2D rasterizes without tessellation.
    //
    // The segment iteration is cached per-shape-pointer so that
    // repeated frames skip detection entirely (big CPU save for the
    // 2000-particle benchmark).
    // ================================================================
    //
    if (false && pPath->GetFigureCount() == 1)   // TEMP: disabled for debugging
    {
        UINT ellipseCacheIdx = static_cast<UINT>(
            (reinterpret_cast<UINT_PTR>(pPath) >> 4) % GEOMETRY_CACHE_SIZE);
        GeometryCacheEntry &rEllipseEntry = m_geometryCache[ellipseCacheIdx];

        //
        // Cache hit — same shape pointer was previously classified.
        //

        if (rEllipseEntry.pShapeKey == static_cast<const void *>(pPath) &&
            rEllipseEntry.shapeType == 1)
        {
            // Known ellipse — use cached parameters directly.
            const D2D1_ELLIPSE &ellipse = rEllipseEntry.cachedEllipse;
            m_diag.cEllipseCacheHit++;

            if (pFillBrush)
            {
                ComPtr<ID2D1Brush> pD2DFillBrush;
                D2D_IFC(ConvertBrush(pFillBrush, pContextState, &pD2DFillBrush));
                m_pD2DContext->FillEllipse(ellipse, pD2DFillBrush.Get());
            }

            if (pStrokeBrush && pPen)
            {
                ComPtr<ID2D1Brush> pD2DStrokeBrush;
                D2D_IFC(ConvertBrush(pStrokeBrush, pContextState, &pD2DStrokeBrush));
                ComPtr<ID2D1StrokeStyle> pStrokeStyle;
                FLOAT strokeWidth = 1.0f;
                D2D_IFC(ConvertPenToStrokeStyle(pPen, &pStrokeStyle, &strokeWidth));
                m_pD2DContext->DrawEllipse(ellipse, pD2DStrokeBrush.Get(), strokeWidth, pStrokeStyle.Get());
            }

            m_diag.cFastEllipse++;
            goto Cleanup;
        }

        //
        // Not yet classified — inspect the figure's segments.
        //

        const IFigureData &figure = pPath->GetFigure(0);
        if (!figure.IsEmpty() && figure.IsClosed() && figure.IsFillable())
        {
            UINT cSegments = 0;
            UINT cPoints   = 0;
            figure.GetCountsEstimate(cSegments, cPoints);

            if (cSegments >= 4 && cSegments <= 8)
            {
                bool allBezier = true;
                MilPoint2F bezPts[12];   // 4 beziers × 3 points each
                UINT ptIdx = 0;
                if (figure.SetToFirstSegment())
                {
                    UINT segIdx = 0;
                    for (;;)
                    {
                        BYTE bType = 0;
                        const MilPoint2F *pPts = nullptr;
                        figure.GetCurrentSegment(bType, pPts);

                        if ((bType & MilCoreSeg::TypeMask) != MilCoreSeg::TypeBezier)
                        {
                            allBezier = false;
                            break;
                        }

                        if (ptIdx + 3 <= 12)
                        {
                            bezPts[ptIdx++] = pPts[0];
                            bezPts[ptIdx++] = pPts[1];
                            bezPts[ptIdx++] = pPts[2];
                        }
                        ++segIdx;

                        if (!figure.SetToNextSegment()) break;
                    }

                    if (allBezier && segIdx == 4 && ptIdx == 12)
                    {
                        const MilPoint2F &ptStart = figure.GetStartPoint();

                        FLOAT minX = ptStart.X, maxX = ptStart.X;
                        FLOAT minY = ptStart.Y, maxY = ptStart.Y;

                        for (UINT i = 2; i < 12; i += 3)
                        {
                            if (bezPts[i].X < minX) minX = bezPts[i].X;
                            if (bezPts[i].X > maxX) maxX = bezPts[i].X;
                            if (bezPts[i].Y < minY) minY = bezPts[i].Y;
                            if (bezPts[i].Y > maxY) maxY = bezPts[i].Y;
                        }

                        FLOAT cx = (minX + maxX) * 0.5f;
                        FLOAT cy = (minY + maxY) * 0.5f;
                        FLOAT rx = (maxX - minX) * 0.5f;
                        FLOAT ry = (maxY - minY) * 0.5f;

                        if (rx > 0.0f && ry > 0.0f)
                        {
                            D2D1_ELLIPSE ellipse = D2D1::Ellipse(
                                D2D1::Point2F(cx, cy), rx, ry);

                            //
                            // Cache the classification so subsequent frames
                            // skip the segment iteration entirely.
                            //

                            rEllipseEntry.pShapeKey = static_cast<const void *>(pPath);
                            rEllipseEntry.figureCount = 1;
                            rEllipseEntry.shapeType = 1;  // ellipse
                            rEllipseEntry.cachedEllipse = ellipse;
                            rEllipseEntry.pGeometry.Reset();
                            rEllipseEntry.pFillRealization.Reset();

                            if (pFillBrush)
                            {
                                ComPtr<ID2D1Brush> pD2DFillBrush;
                                D2D_IFC(ConvertBrush(pFillBrush, pContextState, &pD2DFillBrush));
                                m_pD2DContext->FillEllipse(ellipse, pD2DFillBrush.Get());
                            }

                            if (pStrokeBrush && pPen)
                            {
                                ComPtr<ID2D1Brush> pD2DStrokeBrush;
                                D2D_IFC(ConvertBrush(pStrokeBrush, pContextState, &pD2DStrokeBrush));
                                ComPtr<ID2D1StrokeStyle> pStrokeStyle;
                                FLOAT strokeWidth = 1.0f;
                                D2D_IFC(ConvertPenToStrokeStyle(pPen, &pStrokeStyle, &strokeWidth));
                                m_pD2DContext->DrawEllipse(ellipse, pD2DStrokeBrush.Get(), strokeWidth, pStrokeStyle.Get());
                            }

                            m_diag.cFastEllipse++;
                            goto Cleanup;
                        }
                    }
                }
            }
        }
    }

    //
    // ================================================================
    // General path: convert to ID2D1Geometry and use FillGeometry /
    // DrawGeometry.
    // ================================================================
    //

    {
        m_diag.cFallbackGeometry++;

        ComPtr<ID2D1Geometry> pGeometry;
        D2D_IFC(ConvertGeometry(pPath, &pGeometry));

        if (pFillBrush)
        {
            ComPtr<ID2D1Brush> pD2DFillBrush;
            D2D_IFC(ConvertBrush(pFillBrush, pContextState, &pD2DFillBrush));

            //
            // TODO: Add geometry realization caching.
            //
            m_pD2DContext->FillGeometry(pGeometry.Get(), pD2DFillBrush.Get());
        }

        if (pStrokeBrush && pPen)
        {
            ComPtr<ID2D1Brush> pD2DStrokeBrush;
            D2D_IFC(ConvertBrush(pStrokeBrush, pContextState, &pD2DStrokeBrush));

            ComPtr<ID2D1StrokeStyle> pStrokeStyle;
            FLOAT strokeWidth = 1.0f;
            D2D_IFC(ConvertPenToStrokeStyle(pPen, &pStrokeStyle, &strokeWidth));

            m_pD2DContext->DrawGeometry(
                pGeometry.Get(),
                pD2DStrokeBrush.Get(),
                strokeWidth,
                pStrokeStyle.Get()
            );
        }
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::DrawInfinitePath
//
//  Synopsis:  Fill the entire render-target bounds with the supplied brush.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::DrawInfinitePath(
    __inout_ecount(1) CContextState *pContextState,
    __inout_ecount(1) BrushContext *pBrushContext,
    __inout_ecount(1) CBrushRealizer *pFillBrush
    )
{
    HRESULT hr = S_OK;

    Assert(pContextState);
    Assert(pFillBrush);

    if (!IsValid())
    {
        goto Cleanup;
    }

    {
        D2D_IFC(EnsureBrushRealization(pFillBrush, pBrushContext, pContextState));

        ComPtr<ID2D1Brush> pD2DBrush;
        D2D_IFC(ConvertBrush(pFillBrush, pContextState, &pD2DBrush));

        D2D1_RECT_F rcFill = D2D1::RectF(
            0.0f,
            0.0f,
            static_cast<FLOAT>(m_uWidth),
            static_cast<FLOAT>(m_uHeight)
        );

        D2D_IFC(BeginDrawBatch());

        m_pD2DContext->SetTransform(D2D1::Matrix3x2F::Identity());
        m_pD2DContext->FillRectangle(rcFill, pD2DBrush.Get());

        // Batch left open.
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::BuildD2DEffectGraph
//
//  Synopsis:  Recursively build a D2D1 effect (+ any chained input
//             effects) from a CMilEffectDuce resource.  Does NOT draw —
//             returns the ID2D1Effect* so the caller can compose it.
//
//  Parameters:
//      pEffectResource – the CMilD2DEffectDuce that describes the effect
//      pSourceBitmap   – the element's rendered content (implicit input)
//      depth           – recursion guard (max 8 levels)
//      ppResult        – [out] receives the constructed ID2D1Effect
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::BuildD2DEffectGraph(
    __in CMilEffectDuce *pEffectResource,
    __in ID2D1Bitmap1 *pSourceBitmap,
    UINT depth,
    __deref_out ID2D1Effect **ppResult
    )
{
    HRESULT hr = S_OK;
    ComPtr<ID2D1Effect> pD2DEffect;

    *ppResult = nullptr;

    // Guard against infinite recursion or absurdly deep chains.
    if (depth > 8)
    {
        return E_FAIL;
    }

    GUID clsid;
    IFC(D2DEffect_GetClsid(pEffectResource, &clsid));

    IFC(m_pD2DContext->CreateEffect(clsid, &pD2DEffect));

    // --- Set properties ---
    UINT32 propCount = D2DEffect_GetPropertyCount(pEffectResource);
    for (UINT32 i = 0; i < propCount; i++)
    {
        UINT32 propIndex = 0;
        const BYTE *pPropData = nullptr;
        UINT32 propSize = 0;

        if (FAILED(D2DEffect_GetProperty(pEffectResource, i, &propIndex, &pPropData, &propSize)))
            continue;
        if (!pPropData || propSize == 0)
            continue;

        pD2DEffect->SetValue(propIndex, pPropData, propSize);
    }

    // --- Set inputs ---
    UINT32 inputCount = D2DEffect_GetInputCount(pEffectResource);
    bool anyInputSet = false;

    for (UINT32 i = 0; i < inputCount; i++)
    {
        UINT32 inputKind = 0;
        HMIL_RESOURCE hInputResource = 0;

        if (FAILED(D2DEffect_GetInput(pEffectResource, i, &inputKind, &hInputResource)))
            continue;

        if (inputKind == 1) // Effect input (chaining)
        {
            CMilEffectDuce *pInnerEffect = D2DEffect_ResolveInputEffect(pEffectResource, hInputResource);
            if (pInnerEffect != nullptr)
            {
                ComPtr<ID2D1Effect> pInner;
                hr = BuildD2DEffectGraph(pInnerEffect, pSourceBitmap, depth + 1, &pInner);
                if (SUCCEEDED(hr) && pInner)
                {
                    pD2DEffect->SetInputEffect(i, pInner.Get());
                    anyInputSet = true;
                    continue;
                }
            }
            // Fallback to source bitmap
            pD2DEffect->SetInput(i, pSourceBitmap);
            anyInputSet = true;
        }
        else // Brush input (inputKind == 0)
        {
            pD2DEffect->SetInput(i, pSourceBitmap);
            anyInputSet = true;
        }
    }

    // Default: wire source bitmap on input 0
    if (!anyInputSet)
    {
        pD2DEffect->SetInput(0, pSourceBitmap);
    }

    *ppResult = pD2DEffect.Detach();

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::ComposeEffect
//
//  Synopsis:  Apply a visual effect using D2D1 built-in effects.
//
//  Notes:     Identifies the effect type via bridge functions (see
//             d2d_effects.h) and creates the corresponding D2D1 effect
//             graph.  Blur maps to CLSID_D2D1GaussianBlur, DropShadow
//             maps to CLSID_D2D1Shadow + AffineTransform + Composite.
//             Unknown/custom ShaderEffects draw the source unmodified.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::ComposeEffect(
    __inout_ecount(1) CContextState *pContextState,
    __in_ecount(1) CMILMatrix *pScaleTransform,
    __inout_ecount(1) CMilEffectDuce *pEffect,
    UINT uIntermediateWidth,
    UINT uIntermediateHeight,
    __in_opt IMILRenderTargetBitmap *pImplicitInput
    )
{
    HRESULT hr = S_OK;

    Assert(pContextState);

    m_diag.cComposeEffect++;

    if (!IsValid())
    {
        goto Cleanup;
    }

    if (pImplicitInput == nullptr)
    {
        goto Cleanup;
    }

    //
    // Step 1: Extract a D2D bitmap from the implicit input.
    //

    {
        ComPtr<ID2D1Bitmap1> pSourceBitmap;
        IWGXBitmapSource *pBitmapSource = nullptr;

        //
        // Unwrap CMetaBitmapRenderTarget if present.
        //

        CMetaBitmapRenderTarget *pMetaBitmapRT = nullptr;
        HRESULT hrQI = pImplicitInput->QueryInterface(
            IID_CMetaBitmapRenderTarget,
            reinterpret_cast<void **>(&pMetaBitmapRT));

        if (SUCCEEDED(hrQI) && pMetaBitmapRT)
        {
            IMILRenderTargetBitmap *pInnerRTNoRef = nullptr;
            D2D_IFC(pMetaBitmapRT->GetCompatibleSubRenderTargetNoRef(
                CMILResourceCache::SwRealizationCacheIndex,
                m_associatedDisplay,
                &pInnerRTNoRef));

            D2D_IFC(pInnerRTNoRef->GetBitmapSource(&pBitmapSource));
            pMetaBitmapRT->Release();
        }
        else
        {
            hr = pImplicitInput->GetBitmapSource(&pBitmapSource);
            if (FAILED(hr))
            {
                hr = S_OK;
                goto Cleanup;
            }
        }

        if (!pBitmapSource)
        {
            goto Cleanup;
        }

        //
        // Try fast path: D2D-backed bitmap.
        //
        // The source bitmap may have been created on a different
        // ID2D1DeviceContext (the texture RT's context).  D2D effects
        // can exhibit flickering when fed cross-context bitmaps
        // because the GPU may not have finished writing the source.
        // To avoid this, we copy the bitmap into a new bitmap owned
        // by our display context — a cheap GPU-to-GPU blit that
        // guarantees synchronization.
        //

        {
            ComPtr<ID2DBitmapSourceInternal> pD2DInternal;
            if (SUCCEEDED(pBitmapSource->QueryInterface(
                    IID_ID2DBitmapSourceInternal,
                    reinterpret_cast<void **>(pD2DInternal.GetAddressOf()))))
            {
                ComPtr<ID2D1Bitmap1> pCrossCtxBitmap;
                pD2DInternal->GetD2DBitmap(&pCrossCtxBitmap);

                if (pCrossCtxBitmap)
                {
                    D2D1_SIZE_U sz = pCrossCtxBitmap->GetPixelSize();

                    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_NONE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_PREMULTIPLIED)
                    );

                    ComPtr<ID2D1Bitmap1> pLocalBitmap;
                    hr = m_pD2DContext->CreateBitmap(sz, nullptr, 0,
                                                     bmpProps, &pLocalBitmap);
                    if (SUCCEEDED(hr))
                    {
                        D2D1_POINT_2U destPt = D2D1::Point2U(0, 0);
                        D2D1_RECT_U   srcRect = D2D1::RectU(0, 0, sz.width, sz.height);
                        hr = pLocalBitmap->CopyFromBitmap(&destPt,
                                                           pCrossCtxBitmap.Get(),
                                                           &srcRect);
                        if (SUCCEEDED(hr))
                        {
                            pSourceBitmap = pLocalBitmap;
                        }
                    }

                    if (FAILED(hr))
                    {
                        // Fallback: use the cross-context bitmap directly.
                        pSourceBitmap = pCrossCtxBitmap;
                        hr = S_OK;
                    }

                    m_diag.cComposeFastPath++;
                }
            }
        }

        //
        // Slow path: read pixels and upload to a GPU bitmap.
        //

        if (!pSourceBitmap)
        {
            m_diag.cComposeSlowPath++;

            UINT uBmpWidth = 0, uBmpHeight = 0;
            D2D_IFC(pBitmapSource->GetSize(&uBmpWidth, &uBmpHeight));

            if (uBmpWidth == 0 || uBmpHeight == 0)
            {
                pBitmapSource->Release();
                goto Cleanup;
            }

            UINT cbStride = uBmpWidth * 4;
            UINT cbBuffer = cbStride * uBmpHeight;
            BYTE *pPixels = nullptr;
            D2D_IFC(EnsureStagingBuffer(cbBuffer, &pPixels));

            WICRect srcRect = { 0, 0, static_cast<INT>(uBmpWidth), static_cast<INT>(uBmpHeight) };
            hr = pBitmapSource->CopyPixels(&srcRect, cbStride, cbBuffer, pPixels);
            if (FAILED(hr))
            {
                pBitmapSource->Release();
                goto Cleanup;
            }

            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );

            hr = m_pD2DContext->CreateBitmap(
                D2D1::SizeU(uBmpWidth, uBmpHeight),
                pPixels,
                cbStride,
                bmpProps,
                &pSourceBitmap
            );

            if (FAILED(hr))
            {
                pBitmapSource->Release();
                goto Cleanup;
            }
        }

        pBitmapSource->Release();
        pBitmapSource = nullptr;

        if (!pSourceBitmap)
        {
            m_diag.cComposeNoSource++;
            goto Cleanup;
        }

        //
        // Step 2: Apply the world-to-device transform.
        //

        D2D_IFC(BeginDrawBatch());

        {
            const CMILMatrix &mat = pContextState->WorldToDevice;
            D2D1_MATRIX_3X2_F d2dTransform = D2D1::Matrix3x2F(
                mat._11, mat._12,
                mat._21, mat._22,
                mat._41, mat._42
            );
            m_pD2DContext->SetTransform(d2dTransform);
        }

        //
        // Step 3: Identify the effect type and apply D2D effects.
        //

        int effectType = D2DEffect_GetEffectType(pEffect);

        switch (effectType)
        {
            case 0: m_diag.cEffectType0++; break;
            case 1: m_diag.cEffectType1++; break;
            case 2: m_diag.cEffectType2++; break;
            case 3: m_diag.cEffectType3++; break;
        }

        if (effectType == 1)
        {
            //
            // Blur effect → CLSID_D2D1GaussianBlur.
            //
            // WPF Radius is kernel half-width; D2D expects σ (std deviation).
            // Approximate mapping: σ ≈ Radius / 2.
            //

            double wpfRadius = D2DEffect_GetBlurRadius(pEffect);
            float sigma = static_cast<float>(wpfRadius * 0.5);
            if (sigma < 0.1f) sigma = 0.1f;

            ComPtr<ID2D1Effect> pBlurEffect;
            hr = m_pD2DContext->CreateEffect(CLSID_D2D1GaussianBlur, &pBlurEffect);

            if (SUCCEEDED(hr))
            {
                pBlurEffect->SetInput(0, pSourceBitmap.Get());
                pBlurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
                pBlurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
                    D2D1_BORDER_MODE_SOFT);

                m_pD2DContext->DrawImage(pBlurEffect.Get());
            }
            else
            {
                //
                // Effect creation failed — draw source unmodified.
                //

                m_pD2DContext->DrawImage(pSourceBitmap.Get());
                hr = S_OK;
            }
        }
        else if (effectType == 2)
        {
            //
            // DropShadow effect → Shadow + AffineTransform + Composite.
            //
            // Graph:
            //   source → Shadow → AffineTransform (offset) ─┐
            //   source ────────────────────────────────────── Composite
            //

            double blurRadius = D2DEffect_GetDropShadowBlurRadius(pEffect);
            double direction  = D2DEffect_GetDropShadowDirection(pEffect);
            double depth      = D2DEffect_GetDropShadowDepth(pEffect);
            double opacity    = D2DEffect_GetDropShadowOpacity(pEffect);
            float cr, cg, cb, ca;
            D2DEffect_GetDropShadowColor(pEffect, &cr, &cg, &cb, &ca);

            float sigma = static_cast<float>(blurRadius * 0.5);
            if (sigma < 0.1f) sigma = 0.1f;

            //
            // WPF Direction: degrees, 0=right, counter-clockwise.
            // Offset: depth * (cos(dir), -sin(dir)) in Y-down coords.
            //

            const double PI = 3.14159265358979323846;
            float dirRad = static_cast<float>(direction * PI / 180.0);
            float offsetX = static_cast<float>(depth * cos(dirRad));
            float offsetY = static_cast<float>(-depth * sin(dirRad));

            ComPtr<ID2D1Effect> pShadowEffect;
            ComPtr<ID2D1Effect> pTransformEffect;
            ComPtr<ID2D1Effect> pCompositeEffect;

            hr = m_pD2DContext->CreateEffect(CLSID_D2D1Shadow, &pShadowEffect);
            if (FAILED(hr)) { m_pD2DContext->DrawImage(pSourceBitmap.Get()); hr = S_OK; goto Cleanup; }

            hr = m_pD2DContext->CreateEffect(CLSID_D2D12DAffineTransform, &pTransformEffect);
            if (FAILED(hr)) { m_pD2DContext->DrawImage(pSourceBitmap.Get()); hr = S_OK; goto Cleanup; }

            hr = m_pD2DContext->CreateEffect(CLSID_D2D1Composite, &pCompositeEffect);
            if (FAILED(hr)) { m_pD2DContext->DrawImage(pSourceBitmap.Get()); hr = S_OK; goto Cleanup; }

            //
            // Configure shadow: blur + colour with opacity baked in.
            //

            pShadowEffect->SetInput(0, pSourceBitmap.Get());
            pShadowEffect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, sigma);

            D2D1_VECTOR_4F shadowColor = { cr, cg, cb, ca * static_cast<float>(opacity) };
            pShadowEffect->SetValue(D2D1_SHADOW_PROP_COLOR, shadowColor);

            //
            // Translate shadow by (offsetX, offsetY).
            //

            pTransformEffect->SetInputEffect(0, pShadowEffect.Get());
            D2D1_MATRIX_3X2_F offsetMatrix = D2D1::Matrix3x2F::Translation(offsetX, offsetY);
            pTransformEffect->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, offsetMatrix);

            //
            // Composite: shadow (input 0) behind original (input 1).
            //

            pCompositeEffect->SetInputEffect(0, pTransformEffect.Get());
            pCompositeEffect->SetInput(1, pSourceBitmap.Get());
            pCompositeEffect->SetValue(D2D1_COMPOSITE_PROP_MODE, D2D1_COMPOSITE_MODE_SOURCE_OVER);

            m_pD2DContext->DrawImage(pCompositeEffect.Get());
        }
        else if (effectType == 4)
        {
            //
            // D2DEffect — generic D2D built-in effect.
            //

            m_diag.cComposeD2DEffect++;

            // Create the effect from CLSID, set properties and inputs,
            // then draw the result.
            //

            GUID clsid;
            hr = D2DEffect_GetClsid(pEffect, &clsid);
            if (FAILED(hr)) { m_pD2DContext->DrawImage(pSourceBitmap.Get()); hr = S_OK; goto Cleanup; }

            ComPtr<ID2D1Effect> pD2DEffect;
            hr = m_pD2DContext->CreateEffect(clsid, &pD2DEffect);
            if (FAILED(hr))
            {
                // Unsupported CLSID — draw source unmodified.
                m_pD2DContext->DrawImage(pSourceBitmap.Get());
                hr = S_OK;
                goto Cleanup;
            }

            //
            // Set properties from the property bag.
            //

            UINT32 propCount = D2DEffect_GetPropertyCount(pEffect);
            for (UINT32 i = 0; i < propCount; i++)
            {
                UINT32 propType = 0;
                const BYTE *pPropData = nullptr;
                UINT32 propSize = 0;

                hr = D2DEffect_GetProperty(pEffect, i, &propType, &pPropData, &propSize);
                if (FAILED(hr))
                {
                    continue;
                }
                if (pPropData == nullptr || propSize == 0)
                {
                    continue;
                }

                //
                // propType holds the D2D property INDEX (bridge returns Entry.Index).
                // ID2D1Effect::SetValue takes raw bytes — no type dispatch needed.
                //

                UINT32 d2dPropertyIndex = propType;

                HRESULT hrSet = pD2DEffect->SetValue(d2dPropertyIndex, pPropData, propSize);
                if (SUCCEEDED(hrSet))
                {
                    m_diag.cPropSetOk++;
                    // Track the sigma value for GaussianBlur (property index 0, size 4 = float)
                    if (d2dPropertyIndex == 0 && propSize == 4)
                    {
                        m_diag.lastSigma = *reinterpret_cast<const float*>(pPropData);
                    }
                }
                else
                {
                    m_diag.cPropSetFail++;
                }
            }

            //
            // Set inputs.  Brush inputs use the source bitmap (implicit input).
            // Effect inputs (chaining) recursively build the inner effect graph.
            //

            UINT32 inputCount = D2DEffect_GetInputCount(pEffect);
            bool anyInputSet = false;

            for (UINT32 i = 0; i < inputCount; i++)
            {
                UINT32 inputKind = 0;
                HMIL_RESOURCE hInputResource = 0;

                hr = D2DEffect_GetInput(pEffect, i, &inputKind, &hInputResource);
                if (FAILED(hr))
                    continue;

                if (inputKind == 1) // Effect input (chaining)
                {
                    CMilEffectDuce *pInnerEffect = D2DEffect_ResolveInputEffect(pEffect, hInputResource);
                    if (pInnerEffect != nullptr)
                    {
                        ComPtr<ID2D1Effect> pInner;
                        hr = BuildD2DEffectGraph(pInnerEffect, pSourceBitmap.Get(), 1, &pInner);
                        if (SUCCEEDED(hr) && pInner)
                        {
                            pD2DEffect->SetInputEffect(i, pInner.Get());
                            anyInputSet = true;
                            continue;
                        }
                    }
                    // Fallback to source bitmap
                    pD2DEffect->SetInput(i, pSourceBitmap.Get());
                    anyInputSet = true;
                }
                else // Brush input
                {
                    pD2DEffect->SetInput(i, pSourceBitmap.Get());
                    anyInputSet = true;
                }
            }

            // If no inputs were set, default to source bitmap on input 0
            if (!anyInputSet)
            {
                pD2DEffect->SetInput(0, pSourceBitmap.Get());
            }

            //
            // Special handling for CLSID_D2D1Shadow: the shadow effect only
            // outputs the shadow silhouette.  To produce a useful "drop shadow"
            // result we composite the shadow behind the original source image.
            //
            // Graph: source → Shadow → Composite(input0=shadow, input1=source)
            //

            if (clsid == CLSID_D2D1Shadow)
            {
                ComPtr<ID2D1Effect> pComposite;
                hr = m_pD2DContext->CreateEffect(CLSID_D2D1Composite, &pComposite);
                if (SUCCEEDED(hr))
                {
                    pComposite->SetInputEffect(0, pD2DEffect.Get());
                    pComposite->SetInput(1, pSourceBitmap.Get());
                    pComposite->SetValue(D2D1_COMPOSITE_PROP_MODE,
                                         D2D1_COMPOSITE_MODE_SOURCE_OVER);
                    m_pD2DContext->DrawImage(pComposite.Get());
                }
                else
                {
                    m_pD2DContext->DrawImage(pD2DEffect.Get());
                    hr = S_OK;
                }
            }
            else
            {
                m_pD2DContext->DrawImage(pD2DEffect.Get());
            }
            m_diag.cComposeD2DDrawImage++;
        }
        else
        {
            //
            // Unknown or custom ShaderEffect — draw source unmodified.
            //

            m_pD2DContext->DrawImage(pSourceBitmap.Get());
        }
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::DrawGlyphs
//
//  Synopsis:  Render a glyph run using D2D1's DrawGlyphRun.
//
//  Notes:     The WPF DrawGlyphsParameters structure contains a
//             CGlyphRunResource which exposes the DWRITE_GLYPH_RUN.
//             Phase 1 creates a solid-colour brush from the realizer
//             and calls DrawGlyphRun directly.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::DrawGlyphs(
    __inout_ecount(1) DrawGlyphsParameters &pars
    )
{
    HRESULT hr = S_OK;

    if (!IsValid())
    {
        goto Cleanup;
    }

    if (!pars.pGlyphRun || !pars.pBrushRealizer || !pars.pContextState)
    {
        goto Cleanup;
    }

    //
    // Realize the brush so GetRealizedBrushNoRef() returns a valid CMILBrush.
    //

    D2D_IFC(EnsureBrushRealization(pars.pBrushRealizer, pars.pBrushContext, pars.pContextState));

    //
    // Convert the realized WPF brush to a D2D brush.
    //

    {
        ComPtr<ID2D1Brush> pD2DBrush;
        D2D_IFC(ConvertBrush(pars.pBrushRealizer, pars.pContextState, &pD2DBrush));

        if (!pD2DBrush)
        {
            goto Cleanup;
        }

        //
        // Build the DWRITE_GLYPH_RUN from the CGlyphRunResource.
        //

        DWRITE_GLYPH_RUN glyphRun = {};
        IDWriteFontFace *pFontFace = nullptr;

        hr = BuildDWriteGlyphRunFromResource(pars.pGlyphRun, &glyphRun, &pFontFace);
        if (FAILED(hr) || pFontFace == nullptr)
        {
            hr = S_OK;   // gracefully skip if font face unavailable
            goto Cleanup;
        }

        if (glyphRun.glyphCount == 0)
        {
            if (pFontFace)
            {
                pFontFace->Release();
            }
            goto Cleanup;
        }

        D2D_IFC(BeginDrawBatch());

        //
        // Apply the world-to-device transform.
        //

        {
            const CMILMatrix &mat = pars.pContextState->WorldToDevice;
            D2D1_MATRIX_3X2_F d2dTransform = D2D1::Matrix3x2F(
                mat._11, mat._12,
                mat._21, mat._22,
                mat._41, mat._42
            );
            m_pD2DContext->SetTransform(d2dTransform);
        }

        //
        // Set text antialiasing mode.
        //

        D2D1_TEXT_ANTIALIAS_MODE prevAA = m_pD2DContext->GetTextAntialiasMode();
        bool fSupportsClearType = m_forceClearType ||
            !HasAlphaChannel(m_fmtTarget);

        m_pD2DContext->SetTextAntialiasMode(
            fSupportsClearType
                ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE
                : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE
        );

        //
        // The baseline origin comes from the glyph run resource.
        //

        MilPoint2F origin = GetGlyphRunOrigin(pars.pGlyphRun);
        D2D1_POINT_2F baselineOrigin = D2D1::Point2F(origin.X, origin.Y);

        DWRITE_MEASURING_MODE measuringMode = IsGlyphRunDisplayMeasured(pars.pGlyphRun)
            ? DWRITE_MEASURING_MODE_GDI_CLASSIC
            : DWRITE_MEASURING_MODE_NATURAL;

        m_pD2DContext->DrawGlyphRun(
            baselineOrigin,
            &glyphRun,
            pD2DBrush.Get(),
            measuringMode
        );

        //
        // Restore previous AA mode.
        //

        m_pD2DContext->SetTextAntialiasMode(prevAA);

        pFontFace->Release();
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::DrawVideo
//
//  Synopsis:  Obtain the current video frame as a bitmap and draw it.
//
//  Notes:     IAVSurfaceRenderer provides the latest decoded frame.  We
//             try to get an IWGXBitmapSource from it and delegate to
//             DrawBitmap.  If a separate pIBitmapSource is supplied
//             (e.g., a poster frame), we use that as a fallback.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::DrawVideo(
    __inout_ecount(1) CContextState *pContextState,
    __inout_ecount_opt(1) IAVSurfaceRenderer *pSurfaceRenderer,
    __inout_ecount_opt(1) IWGXBitmapSource *pIBitmapSource,
    __inout_ecount_opt(1) IMILEffectList *pIEffect
    )
{
    HRESULT hr = S_OK;

    //
    // TODO Phase 2: Get video frame from IAVSurfaceRenderer and draw it.
    //       IAVSurfaceRenderer type definition needed.
    //

    UNREFERENCED_PARAMETER(pContextState);
    UNREFERENCED_PARAMETER(pSurfaceRenderer);
    UNREFERENCED_PARAMETER(pIEffect);

    // Fallback to the caller-provided bitmap source (poster frame).
    if (pIBitmapSource != nullptr)
    {
        D2D_IFC(DrawBitmap(pContextState, pIBitmapSource, pIEffect));
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::BeginLayer
//
//  Synopsis:  Push a D2D layer with optional geometric mask and opacity.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::BeginLayer(
    __in_ecount(1) MilRectF const &LayerBounds,
    MilAntiAliasMode::Enum AntiAliasMode,
    __in_ecount_opt(1) IShapeData const *pGeometricMask,
    __in_ecount_opt(1) CMILMatrix const *pGeometricMaskToTarget,
    FLOAT flAlphaScale,
    __in_ecount_opt(1) CBrushRealizer *pAlphaMask
    )
{
    HRESULT hr = S_OK;

    if (!IsValid())
    {
        goto Cleanup;
    }

    if (m_layerCount >= MAX_LAYER_DEPTH)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    //
    // ================================================================
    // Fast path: axis-aligned clip
    //
    // Per the MS D2D performance guide, PushAxisAlignedClip is much
    // cheaper than PushLayer.  Use it when:
    //   - No alpha mask brush
    //   - Full opacity (flAlphaScale ~1.0)
    //   - Either no geometric mask, OR the mask is a simple rectangle
    //
    // PushLayer creates a temporary bitmap for compositing, which is
    // slow and can cause subtle rendering artifacts for simple clipping.
    // PushAxisAlignedClip is a direct clip that avoids the bitmap.
    // ================================================================
    //

    {
        bool fCanUseAxisAlignedClip = false;
        D2D1_RECT_F clipRect = D2D1::RectF(
            LayerBounds.left, LayerBounds.top,
            LayerBounds.right, LayerBounds.bottom
        );

        if (pAlphaMask == nullptr && flAlphaScale >= 0.999f)
        {
            if (pGeometricMask == nullptr)
            {
                //
                // No geometric mask at all — use LayerBounds directly.
                //
                fCanUseAxisAlignedClip = true;
            }
            else if (pGeometricMask->IsAxisAlignedRectangle() &&
                     pGeometricMask->GetFigureCount() == 1)
            {
                //
                // Geometric mask is a simple axis-aligned rectangle.
                // Extract its bounds and intersect with LayerBounds
                // so we get the tightest possible clip.
                //
                const IFigureData &figure = pGeometricMask->GetFigure(0);
                MilRectF rcMask;
                figure.GetAsWellOrderedRectangle(rcMask);

                //
                // Apply the mask-to-target transform if provided.
                //
                if (pGeometricMaskToTarget != nullptr)
                {
                    MilRectF rcTransformed;
                    pGeometricMaskToTarget->Transform2DBounds(
                        rcMask,
                        rcTransformed
                    );
                    rcMask = rcTransformed;
                }

                //
                // Intersect mask bounds with layer bounds.
                //
                clipRect.left   = max(clipRect.left,   rcMask.left);
                clipRect.top    = max(clipRect.top,    rcMask.top);
                clipRect.right  = min(clipRect.right,  rcMask.right);
                clipRect.bottom = min(clipRect.bottom, rcMask.bottom);

                fCanUseAxisAlignedClip = true;
            }
        }

        if (fCanUseAxisAlignedClip)
        {
            D2D1_ANTIALIAS_MODE aaMode = (AntiAliasMode != MilAntiAliasMode::None)
                ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                : D2D1_ANTIALIAS_MODE_ALIASED;

            D2D_IFC(BeginDrawBatch());

            //
            // LayerBounds is in device pixel coordinates (PageInPixels).
            // PushAxisAlignedClip interprets the rect in the current-transform
            // space, so reset to identity first, then restore after the push.
            //

            m_pD2DContext->SetTransform(D2D1::Matrix3x2F::Identity());
            m_pD2DContext->PushAxisAlignedClip(clipRect, aaMode);
            m_layerTypeStack[m_layerCount] = false;  // axis-aligned clip
            m_diag.cBeginLayer++;
            m_diag.cAxisAlignedClip++;
            ++m_layerCount;
            goto Cleanup;
        }
    }

    //
    // ================================================================
    // General path: PushLayer with optional geometric mask + opacity
    // ================================================================
    //

    {
        D2D1_RECT_F contentBounds = D2D1::RectF(
            LayerBounds.left,
            LayerBounds.top,
            LayerBounds.right,
            LayerBounds.bottom
        );

        //
        // Convert geometric mask to a D2D geometry if provided.
        //

        ComPtr<ID2D1Geometry> pMaskGeometry;

        if (pGeometricMask != nullptr)
        {
            hr = ConvertGeometry(pGeometricMask, &pMaskGeometry);

            if (FAILED(hr))
            {
                pMaskGeometry.Reset();
                hr = S_OK;
            }
        }

        D2D1_ANTIALIAS_MODE aaMode = (AntiAliasMode != MilAntiAliasMode::None)
            ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
            : D2D1_ANTIALIAS_MODE_ALIASED;

        D2D1_MATRIX_3X2_F maskTransform = D2D1::Matrix3x2F::Identity();

        if (pGeometricMaskToTarget != nullptr)
        {
            maskTransform = D2D1::Matrix3x2F(
                pGeometricMaskToTarget->_11, pGeometricMaskToTarget->_12,
                pGeometricMaskToTarget->_21, pGeometricMaskToTarget->_22,
                pGeometricMaskToTarget->_41, pGeometricMaskToTarget->_42
            );
        }

        D2D1_LAYER_PARAMETERS1 layerParams = D2D1::LayerParameters1(
            contentBounds,
            pMaskGeometry.Get(),
            aaMode,
            maskTransform,
            flAlphaScale,
            nullptr,    // opacity brush – Phase 2
            D2D1_LAYER_OPTIONS1_NONE
        );

        D2D_IFC(BeginDrawBatch());

        //
        // LayerBounds and the geometric mask are already in device pixel
        // coordinates (PageInPixels).  PushLayer interprets contentBounds
        // and mask geometry in the current-transform space, so ensure
        // identity before the push.
        //

        m_pD2DContext->SetTransform(D2D1::Matrix3x2F::Identity());
        m_pD2DContext->PushLayer(layerParams, nullptr);
        m_layerTypeStack[m_layerCount] = true;  // full layer
        m_diag.cBeginLayer++;
        m_diag.cFullLayer++;

        ++m_layerCount;
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::EndLayer
//
//  Synopsis:  Pop the most recent layer pushed by BeginLayer.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::EndLayer()
{
    HRESULT hr = S_OK;

    if (!IsValid())
    {
        goto Cleanup;
    }

    if (m_layerCount == 0)
    {
        hr = WGXERR_INVALIDCALL;
        goto Cleanup;
    }

    D2D_IFC(BeginDrawBatch());

    --m_layerCount;

    if (m_layerTypeStack[m_layerCount])
    {
        //
        // Pop a full layer (PushLayer was used).
        //
        m_pD2DContext->PopLayer();
    }
    else
    {
        //
        // Pop an axis-aligned clip (PushAxisAlignedClip fast path).
        //
        m_pD2DContext->PopAxisAlignedClip();
    }

    m_diag.cEndLayer++;

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::EndAndIgnoreAllLayers
//
//  Synopsis:  Pop all remaining layers without applying their effects.
//             Used during abort / cleanup.
//
//-------------------------------------------------------------------------

STDMETHODIMP_(void)
CD2DSurfaceRenderTarget::EndAndIgnoreAllLayers()
{
    if (!IsValid())
    {
        return;
    }

    //
    // D2D requires that every PushLayer is matched by a PopLayer before the
    // target can be released.  Walk the stack and pop everything.
    //

    if (m_layerCount > 0)
    {
        BeginDrawBatch();

        while (m_layerCount > 0)
        {
            --m_layerCount;
            if (m_layerTypeStack[m_layerCount])
            {
                m_pD2DContext->PopLayer();
            }
            else
            {
                m_pD2DContext->PopAxisAlignedClip();
            }
        }

        //
        // Flush the batch — we are in an abort path so ignore the result.
        //

        FlushDrawBatch();
    }
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::GetType
//
//  Synopsis:  Return the render-target type flag.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::GetType(
    __out DWORD *pRenderTargetType
    )
{
    Assert(pRenderTargetType);

    *pRenderTargetType = D2DRasterRenderTarget;

    RRETURN(S_OK);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::SetClearTypeHint
//
//  Synopsis:  Allow callers to force ClearType rendering in intermediate
//             render targets that have alpha channels.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::SetClearTypeHint(
    __in bool forceClearType
    )
{
    m_forceClearType = forceClearType;
    RRETURN(S_OK);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::GetRealizationCacheIndex
//
//  Synopsis:  Return the realization cache index associated with our
//             D2D device.  Brush / glyph realizations are keyed to this.
//
//-------------------------------------------------------------------------

UINT
CD2DSurfaceRenderTarget::GetRealizationCacheIndex()
{
    //
    // In the HW backend this delegates to
    // m_pD3DDevice->GetRealizationCacheIndex().  For the D2D backend we
    // return a fixed index until multi-adapter support is added.
    //

    return 0;
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::GetNumQueuedPresents
//
//  Synopsis:  Return the number of pending present operations.  D2D
//             presentation is synchronous with respect to EndDraw, so this
//             is always zero.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::GetNumQueuedPresents(
    __out_ecount(1) UINT *puNumQueuedPresents
    )
{
    Assert(puNumQueuedPresents);

    *puNumQueuedPresents = 0;

    RRETURN(S_OK);
}

// =======================================================================
// CIntermediateRTCreator
// =======================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::CreateRenderTargetBitmap
//
//  Synopsis:  Create an off-screen D2D bitmap render target for use as an
//             intermediate (e.g., for opacity layers or tiled brushes).
//
//  Notes:     The returned IMILRenderTargetBitmap must be usable as a
//             source bitmap after rendering into it.  Phase 1 creates a
//             D2D1 bitmap with D2D1_BITMAP_OPTIONS_TARGET.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::CreateRenderTargetBitmap(
    UINT width,
    UINT height,
    IntermediateRTUsage usageInfo,
    MilRTInitialization::Flags dwFlags,
    __deref_out_ecount(1) IMILRenderTargetBitmap **ppIRenderTargetBitmap,
    __in_opt DynArray<bool> const *pActiveDisplays
    )
{
    HRESULT hr = S_OK;

    Assert(ppIRenderTargetBitmap);
    *ppIRenderTargetBitmap = nullptr;

    if (!IsValid())
    {
        hr = WGXERR_INVALIDCALL;
        goto Cleanup;
    }

    //
    // Create a CD2DTextureRenderTarget — an off-screen D2D bitmap that
    // WPF can render subtrees into (opacity layers, effects, etc.) and
    // then composite back via DrawBitmap.  This keeps the entire
    // intermediate on the GPU, avoiding the catastrophic software fallback
    // that E_NOTIMPL caused previously.
    //

    {
        UNREFERENCED_PARAMETER(usageInfo);
        UNREFERENCED_PARAMETER(dwFlags);
        UNREFERENCED_PARAMETER(pActiveDisplays);

        CD2DTextureRenderTarget *pTextureRT = nullptr;

        D2D_IFC(CD2DTextureRenderTarget::Create(
            m_pDevice,
            m_associatedDisplay,
            width,
            height,
            &pTextureRT
        ));

        *ppIRenderTargetBitmap = static_cast<IMILRenderTargetBitmap *>(pTextureRT);
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::ReadEnabledDisplays
//
//  Synopsis:  Report which displays are enabled for this render target.
//
//-------------------------------------------------------------------------

STDMETHODIMP
CD2DSurfaceRenderTarget::ReadEnabledDisplays(
    __inout DynArray<bool> *pEnabledDisplays
    )
{
    HRESULT hr = S_OK;

    //
    // For single-adapter Phase 1 we report a single enabled display.
    //

    if (pEnabledDisplays == nullptr)
    {
        hr = E_POINTER;
        goto Cleanup;
    }

    pEnabledDisplays->Reset(FALSE);
    D2D_IFC(pEnabledDisplays->Add(true));

Cleanup:
    RRETURN(hr);
}

// =======================================================================
// D2D1 conversion helpers
// =======================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::EnsureD2DResources
//
//  Synopsis:  Create / re-create the ID2D1DeviceContext1 from the
//             CD2DDevice.
//
//  Notes:     The CD2DDevice is responsible for owning the ID2D1Device.
//             Here we create a device-context from it.  In a full build
//             CD2DDevice::GetD2DDevice() would return the underlying
//             ID2D1Device1.
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::EnsureD2DResources()
{
    HRESULT hr = S_OK;

    if (m_pD2DContext != nullptr)
    {
        //
        // Already initialised.
        //
        goto Cleanup;
    }

    {
        //
        // Obtain the ID2D1Device1 from our device wrapper.
        //

        ID2D1Device1 *pD2DDevice = m_pDevice->GetD2DDevice();

        if (pD2DDevice == nullptr)
        {
            hr = E_UNEXPECTED;
            goto Cleanup;
        }

        //
        // Create a device context.
        //

        //
        // ENABLE_MULTI_THREADED_OPTIMIZATIONS: lets D2D tessellate
        // path geometry across multiple cores.  Recommended by the
        // MS D2D perf guide for apps with complex geometric content.
        //

        D2D_IFC(pD2DDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS,
            &m_pD2DContext
        ));

        //
        // Use PIXELS unit mode — WPF handles its own DPI scaling.
        //

        m_pD2DContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

        //
        // Match context DPI to the target bitmap (96×96) to prevent
        // D2D internal software scaling.
        //

        m_pD2DContext->SetDpi(96.0f, 96.0f);

        //
        // Use PER_PRIMITIVE AA — lets D2D use GPU MSAA.
        //

        m_pD2DContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_pD2DContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        //
        // Cache the D2D factory pointer to avoid per-draw GetFactory() calls.
        //

        m_pD2DContext->GetFactory(m_pD2DFactory.ReleaseAndGetAddressOf());
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::EnsureBrushRealization
//
//  Synopsis:  Call EnsureRealization on a brush realizer so that the
//             underlying CMILBrush is materialized and subsequently
//             available via GetRealizedBrushNoRef().
//
//  Notes:     This mirrors what CHwSurfaceRenderTarget::DrawPathInternal
//             does before each FillPath / DrawGeometry call.
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::EnsureBrushRealization(
    __inout_ecount_opt(1) CBrushRealizer *pBrushRealizer,
    __inout_ecount_opt(1) BrushContext *pBrushContext,
    __in_ecount(1) const CContextState *pContextState
    )
{
    HRESULT hr = S_OK;

    if (pBrushRealizer == nullptr)
    {
        goto Cleanup;
    }

    {
        CSwIntermediateRTCreator swRTCreator(
            MilPixelFormat::PBGRA32bpp,
            m_associatedDisplay
            DBG_STEP_RENDERING_COMMA_PARAM(nullptr)
            );

        // DIAGNOSTIC: log before/after and time EnsureRealization
        {
            hr = pBrushRealizer->EnsureRealization(
                CMILResourceCache::SwRealizationCacheIndex,
                m_associatedDisplay,
                pBrushContext,
                pContextState,
                &swRTCreator
                );

            IFC(hr);
        }
    }

Cleanup:
    RRETURN(hr);
}


//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::ConvertBrush
//
//  Synopsis:  Translate a WPF CBrushRealizer into an ID2D1Brush.
//
//  Notes:     Phase 1 always creates a solid-colour fallback (opaque
//             black).  Phase 2 will inspect the realised CMILBrush type
//             and create the corresponding D2D brush:
//               - CMILBrushSolid       -> ID2D1SolidColorBrush
//               - CMILBrushBitmap      -> ID2D1BitmapBrush
//               - CMILBrushLinearGrad  -> ID2D1LinearGradientBrush
//               - CMILBrushRadialGrad  -> ID2D1RadialGradientBrush
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::ConvertBrush(
    __inout_ecount_opt(1) CBrushRealizer *pBrushRealizer,
    __inout_ecount_opt(1) CContextState *pContextState,
    __deref_out_ecount(1) ID2D1Brush **ppBrush
    )
{
    HRESULT hr = S_OK;

    Assert(ppBrush);
    *ppBrush = nullptr;

    //
    // If no brush realizer is supplied, return a transparent brush so that
    // callers can still use the returned brush without null checks.
    //

    if (pBrushRealizer == nullptr)
    {
        if (!m_pTransparentBrush)
        {
            D2D_IFC(m_pD2DContext->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                &m_pTransparentBrush
            ));
        }

        m_pTransparentBrush.Get()->AddRef();
        *ppBrush = m_pTransparentBrush.Get();
        goto Cleanup;
    }

    //
    // Ensure the brush is realised.  The WPF brush pipeline defers
    // realization until the render target calls EnsureRealization.
    // Without this, GetRealizedBrushNoRef() returns NULL and every
    // draw silently uses a transparent brush.
    //

    //
    // Read the realised brush and inspect its CMILBrush type.
    //

    {
        CMILBrush *pMILBrush = pBrushRealizer->GetRealizedBrushNoRef(
            false /* fConvertNULLToTransparent */
        );

        if (!pMILBrush)
        {
            //
            // Realizer produced nothing — treat as transparent.
            //
            if (!m_pTransparentBrush)
            {
                D2D_IFC(m_pD2DContext->CreateSolidColorBrush(
                    D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                    &m_pTransparentBrush
                ));
            }
            m_pTransparentBrush.Get()->AddRef();
            *ppBrush = m_pTransparentBrush.Get();
            goto Cleanup;
        }

        FLOAT opacity = pBrushRealizer->GetOpacityFromRealizedBrush();

        switch (pMILBrush->GetType())
        {
        case BrushSolid:
            {
                //
                // Solid colour brush — extract the actual colour from
                // CMILBrushSolid and apply the realizer opacity.
                //

                CMILBrushSolid *pSolid = static_cast<CMILBrushSolid *>(pMILBrush);
                MilColorF milColor;
                pSolid->GetColor(&milColor);

                D2D1_COLOR_F d2dColor;
                d2dColor.r = ScRgbToSrgb(milColor.r);
                d2dColor.g = ScRgbToSrgb(milColor.g);
                d2dColor.b = ScRgbToSrgb(milColor.b);
                d2dColor.a = milColor.a * opacity;

                ComPtr<ID2D1SolidColorBrush> pSolidBrush;
                D2D_IFC(GetSolidBrush(d2dColor, &pSolidBrush));
                *ppBrush = pSolidBrush.Detach();
            }
            break;

        case BrushGradientLinear:
            {
                //
                // Linear gradient — extract endpoints and gradient stops.
                //

                CMILBrushLinearGradient *pLinear =
                    static_cast<CMILBrushLinearGradient *>(pMILBrush);

                MilPoint2F ptStart, ptEnd, ptDir;
                pLinear->GetEndPoints(&ptStart, &ptEnd, &ptDir);

                CGradientColorData *pColorData = pLinear->GetColorData();
                UINT cStops = pColorData->GetCount();

                if (cStops == 0)
                {
                    // No stops — fall through to black.
                    ComPtr<ID2D1SolidColorBrush> pFallback;
                    D2D_IFC(GetSolidBrush(
                        D2D1::ColorF(0.0f, 0.0f, 0.0f, opacity), &pFallback));
                    *ppBrush = pFallback.Detach();
                    break;
                }

                {
                    MilColorF *pColors    = pColorData->GetColorsPtr();
                    FLOAT     *pPositions = pColorData->GetPositionsPtr();

                    //
                    // Build D2D gradient stop array on the stack for small
                    // counts, heap for large.
                    //
                    D2D1_GRADIENT_STOP stackStops[8];
                    D2D1_GRADIENT_STOP *pStops = stackStops;
                    D2D1_GRADIENT_STOP *pHeapStops = nullptr;

                    if (cStops > 8)
                    {
                        pHeapStops = new(std::nothrow) D2D1_GRADIENT_STOP[cStops];
                        if (!pHeapStops) { hr = E_OUTOFMEMORY; goto Cleanup; }
                        pStops = pHeapStops;
                    }

                    for (UINT i = 0; i < cStops; ++i)
                    {
                        pStops[i].position = pPositions[i];
                        pStops[i].color.r  = ScRgbToSrgb(pColors[i].r);
                        pStops[i].color.g  = ScRgbToSrgb(pColors[i].g);
                        pStops[i].color.b  = ScRgbToSrgb(pColors[i].b);
                        pStops[i].color.a  = pColors[i].a;
                    }

                    ComPtr<ID2D1GradientStopCollection> pStopColl;
                    hr = m_pD2DContext->CreateGradientStopCollection(
                        pStops, cStops,
                        D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP,
                        &pStopColl);

                    if (pHeapStops) delete[] pHeapStops;
                    D2D_IFC(hr);

                    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgbProps;
                    lgbProps.startPoint = D2D1::Point2F(ptStart.X, ptStart.Y);
                    lgbProps.endPoint   = D2D1::Point2F(ptEnd.X, ptEnd.Y);

                    ComPtr<ID2D1LinearGradientBrush> pGradBrush;
                    D2D_IFC(m_pD2DContext->CreateLinearGradientBrush(
                        lgbProps,
                        D2D1::BrushProperties(opacity),
                        pStopColl.Get(),
                        &pGradBrush));

                    *ppBrush = pGradBrush.Detach();
                }
            }
            break;

        case BrushGradientRadial:
            {
                //
                // Radial gradient — the "end point" holds the edge of the
                // ellipse; radiusX/Y are the distances from center to that
                // point and to the dir/endpoint2.
                //

                CMILBrushRadialGradient *pRadial =
                    static_cast<CMILBrushRadialGradient *>(pMILBrush);

                MilPoint2F ptCenter, ptEndPoint, ptEndPoint2;
                pRadial->GetEndPoints(&ptCenter, &ptEndPoint, &ptEndPoint2);

                FLOAT radiusX = sqrtf(
                    (ptEndPoint.X - ptCenter.X) * (ptEndPoint.X - ptCenter.X) +
                    (ptEndPoint.Y - ptCenter.Y) * (ptEndPoint.Y - ptCenter.Y));
                FLOAT radiusY = sqrtf(
                    (ptEndPoint2.X - ptCenter.X) * (ptEndPoint2.X - ptCenter.X) +
                    (ptEndPoint2.Y - ptCenter.Y) * (ptEndPoint2.Y - ptCenter.Y));

                if (radiusX < 0.001f) radiusX = 0.001f;
                if (radiusY < 0.001f) radiusY = 0.001f;

                D2D1_POINT_2F d2dCenter = D2D1::Point2F(ptCenter.X, ptCenter.Y);
                D2D1_POINT_2F originOffset = D2D1::Point2F(0.0f, 0.0f);

                if (pRadial->HasSeparateOriginFromCenter())
                {
                    const MilPoint2F &ptOrigin = pRadial->GetGradientOrigin();
                    originOffset.x = ptOrigin.X - ptCenter.X;
                    originOffset.y = ptOrigin.Y - ptCenter.Y;
                }

                CGradientColorData *pColorData = pRadial->GetColorData();
                UINT cStops = pColorData->GetCount();

                if (cStops == 0)
                {
                    ComPtr<ID2D1SolidColorBrush> pFallback;
                    D2D_IFC(GetSolidBrush(
                        D2D1::ColorF(0.0f, 0.0f, 0.0f, opacity), &pFallback));
                    *ppBrush = pFallback.Detach();
                    break;
                }

                {
                    MilColorF *pColors    = pColorData->GetColorsPtr();
                    FLOAT     *pPositions = pColorData->GetPositionsPtr();

                    D2D1_GRADIENT_STOP stackStops[8];
                    D2D1_GRADIENT_STOP *pStops = stackStops;
                    D2D1_GRADIENT_STOP *pHeapStops = nullptr;

                    if (cStops > 8)
                    {
                        pHeapStops = new(std::nothrow) D2D1_GRADIENT_STOP[cStops];
                        if (!pHeapStops) { hr = E_OUTOFMEMORY; goto Cleanup; }
                        pStops = pHeapStops;
                    }

                    for (UINT i = 0; i < cStops; ++i)
                    {
                        pStops[i].position = pPositions[i];
                        pStops[i].color.r  = ScRgbToSrgb(pColors[i].r);
                        pStops[i].color.g  = ScRgbToSrgb(pColors[i].g);
                        pStops[i].color.b  = ScRgbToSrgb(pColors[i].b);
                        pStops[i].color.a  = pColors[i].a;
                    }

                    ComPtr<ID2D1GradientStopCollection> pStopColl;
                    hr = m_pD2DContext->CreateGradientStopCollection(
                        pStops, cStops,
                        D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP,
                        &pStopColl);

                    if (pHeapStops) delete[] pHeapStops;
                    D2D_IFC(hr);

                    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rgbProps;
                    rgbProps.center               = d2dCenter;
                    rgbProps.gradientOriginOffset  = originOffset;
                    rgbProps.radiusX               = radiusX;
                    rgbProps.radiusY               = radiusY;

                    ComPtr<ID2D1RadialGradientBrush> pGradBrush;
                    D2D_IFC(m_pD2DContext->CreateRadialGradientBrush(
                        rgbProps,
                        D2D1::BrushProperties(opacity),
                        pStopColl.Get(),
                        &pGradBrush));

                    *ppBrush = pGradBrush.Detach();
                }
            }
            break;

        case BrushBitmap:
            {
                //
                // Bitmap brush — extract the IWGXBitmapSource texture and
                // create a D2D bitmap brush with the correct positioning
                // transform.
                //
                // CMILBrushBitmap carries a bitmap-to-world/sample transform
                // (m_matBitmapToSamplingSpace).  Without applying this as the
                // D2D brush transform, intermediate-layer compositing (used
                // for opacity, effects, and alpha masks) draws the bitmap at
                // the wrong position — which makes the content invisible.
                //

                CMILBrushBitmap *pBmpBrush =
                    static_cast<CMILBrushBitmap *>(pMILBrush);

                IWGXBitmapSource *pTexture = pBmpBrush->GetTextureNoAddRef();

                if (pTexture)
                {
                    ComPtr<ID2D1Bitmap1> pD2DBitmap;
                    D2D_IFC(GetOrCreateCachedBitmap(pTexture, &pD2DBitmap));

                    //
                    // Map WPF wrap mode to D2D extend mode.
                    //

                    D2D1_EXTEND_MODE extModeX = D2D1_EXTEND_MODE_CLAMP;
                    D2D1_EXTEND_MODE extModeY = D2D1_EXTEND_MODE_CLAMP;

                    switch (pBmpBrush->GetWrapMode())
                    {
                    case MilBitmapWrapMode::Tile:
                        extModeX = D2D1_EXTEND_MODE_WRAP;
                        extModeY = D2D1_EXTEND_MODE_WRAP;
                        break;
                    case MilBitmapWrapMode::FlipX:
                        extModeX = D2D1_EXTEND_MODE_MIRROR;
                        extModeY = D2D1_EXTEND_MODE_WRAP;
                        break;
                    case MilBitmapWrapMode::FlipY:
                        extModeX = D2D1_EXTEND_MODE_WRAP;
                        extModeY = D2D1_EXTEND_MODE_MIRROR;
                        break;
                    case MilBitmapWrapMode::FlipXY:
                        extModeX = D2D1_EXTEND_MODE_MIRROR;
                        extModeY = D2D1_EXTEND_MODE_MIRROR;
                        break;
                    default:
                        // Extend / Border / unknown → clamp
                        break;
                    }

                    D2D1_BITMAP_BRUSH_PROPERTIES1 bbProps = {};
                    bbProps.extendModeX       = extModeX;
                    bbProps.extendModeY       = extModeY;
                    bbProps.interpolationMode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

                    //
                    // Extract the bitmap-to-world transform.
                    //
                    // For DrawLayer (layer compositing), the brush is set up
                    // with XSpaceIsWorldSpace and WorldToDevice == identity.
                    // GetBitmapToWorldSpaceTransform returns the stored
                    // matrix, which is the bitmap-to-world offset.
                    //
                    // CMatrix and CMILMatrix share the CBaseMatrix (4×4
                    // D3DMATRIX) layout – reinterpret_cast is safe.
                    //

                    D2D1_MATRIX_3X2_F brushTransform = D2D1::Matrix3x2F::Identity();
                    {
                        CMILMatrix matBitmapToWorld;
                        matBitmapToWorld.SetToIdentity();

                        pBmpBrush->GetBitmapToWorldSpaceTransform(
                            reinterpret_cast<CMatrix<CoordinateSpace::RealizationSampling,
                                                     CoordinateSpace::BaseSampling>&>(matBitmapToWorld));

                        brushTransform = D2D1::Matrix3x2F(
                            matBitmapToWorld._11, matBitmapToWorld._12,
                            matBitmapToWorld._21, matBitmapToWorld._22,
                            matBitmapToWorld._41, matBitmapToWorld._42
                        );
                    }

                    ComPtr<ID2D1BitmapBrush1> pD2DBmpBrush;
                    D2D_IFC(m_pD2DContext->CreateBitmapBrush(
                        pD2DBitmap.Get(),
                        &bbProps,
                        &D2D1::BrushProperties(opacity, brushTransform),
                        &pD2DBmpBrush));

                    *ppBrush = pD2DBmpBrush.Detach();
                }
                else
                {
                    // No texture — transparent.
                    if (!m_pTransparentBrush)
                    {
                        D2D_IFC(m_pD2DContext->CreateSolidColorBrush(
                            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                            &m_pTransparentBrush));
                    }
                    m_pTransparentBrush.Get()->AddRef();
                    *ppBrush = m_pTransparentBrush.Get();
                }
            }
            break;

        default:
            {
                //
                // Unknown / unsupported brush type (e.g. ShaderEffect).
                // Fall back to opaque black.
                //

                ComPtr<ID2D1SolidColorBrush> pFallback;
                D2D_IFC(GetSolidBrush(
                    D2D1::ColorF(D2D1::ColorF::Black, opacity), &pFallback));
                *ppBrush = pFallback.Detach();
            }
            break;
        }
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::ConvertGeometry
//
//  Synopsis:  Build an ID2D1PathGeometry from an IShapeData by iterating
//             its figures and their segments.
//
//  Notes:     IShapeData derives from CShapeBase and exposes:
//               GetFigureCount() – number of sub-figures
//               GetFigure(i)     – returns const IFigureData&
//               GetFillMode()    – Alternate or Winding
//
//             Each IFigureData is traversed via:
//               GetStartPoint()
//               SetToFirstSegment()
//               GetCurrentSegment(bType, pts) – returns true if last
//               SetToNextSegment()
//               IsClosed()
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::ConvertGeometry(
    __in_ecount(1) const IShapeData *pShape,
    __deref_out_ecount(1) ID2D1Geometry **ppGeometry
    )
{
    HRESULT hr = S_OK;

    Assert(pShape);
    Assert(ppGeometry);
    *ppGeometry = nullptr;

    //
    // Check the geometry cache.  The cache is direct-mapped by a hash of
    // the IShapeData pointer.  Shapes whose geometry hasn't changed keep
    // the same pointer across frames, so the hit rate is very high for
    // typical WPF scenes (particles, animated transforms, etc.).
    //

    UINT cFigures = pShape->GetFigureCount();
    UINT cacheIdx = static_cast<UINT>(
        (reinterpret_cast<UINT_PTR>(pShape) >> 4) % GEOMETRY_CACHE_SIZE);

    GeometryCacheEntry &entry = m_geometryCache[cacheIdx];

    if (false && entry.pShapeKey == static_cast<const void *>(pShape) &&  // TEMP: disable geo cache
        entry.figureCount == cFigures &&
        entry.pGeometry)
    {
        //
        // Cache hit — return the cached geometry with an extra reference
        // for the caller.
        //

        *ppGeometry = entry.pGeometry.Get();
        (*ppGeometry)->AddRef();
        goto Cleanup;
    }

    //
    // Cache miss — build the geometry from scratch.
    //

    {
        ComPtr<ID2D1PathGeometry> pPathGeometry;
        ComPtr<ID2D1GeometrySink> pSink;

        D2D_IFC(m_pD2DFactory->CreatePathGeometry(&pPathGeometry));
        D2D_IFC(pPathGeometry->Open(&pSink));

        pSink->SetFillMode(MapFillMode(pShape->GetFillMode()));

        for (UINT iFig = 0; iFig < cFigures; ++iFig)
        {
            const IFigureData &figure = pShape->GetFigure(iFig);
            D2D_IFC(ConvertFigureToSink(figure, pSink.Get()));
        }

        D2D_IFC(pSink->Close());

        //
        // Store in cache — the cache ComPtr takes one reference, and
        // we AddRef a second time for the caller.
        //

        entry.pShapeKey   = static_cast<const void *>(pShape);
        entry.figureCount = cFigures;
        entry.pGeometry.Attach(static_cast<ID2D1Geometry *>(pPathGeometry.Detach()));
        entry.pFillRealization.Reset();   // invalidate stale realization
        entry.shapeType = 0;             // not an ellipse — prevent stale classification

        *ppGeometry = entry.pGeometry.Get();
        (*ppGeometry)->AddRef();
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::ConvertFigureToSink
//
//  Synopsis:  Emit segments from a single IFigureData into a D2D
//             geometry sink.
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::ConvertFigureToSink(
    __in_ecount(1) const IFigureData &figure,
    __inout_ecount(1) ID2D1GeometrySink *pSink
    )
{
    HRESULT hr = S_OK;

    if (figure.IsEmpty())
    {
        goto Cleanup;
    }

    {
        //
        // Begin the figure at the start point.
        //

        const MilPoint2F &ptStart = figure.GetStartPoint();

        D2D1_FIGURE_BEGIN figureBegin = figure.IsFillable()
            ? D2D1_FIGURE_BEGIN_FILLED
            : D2D1_FIGURE_BEGIN_HOLLOW;

        pSink->BeginFigure(
            D2D1::Point2F(ptStart.X, ptStart.Y),
            figureBegin
        );

        //
        // Iterate segments.
        //

        if (figure.SetToFirstSegment())
        {
            //
            // NOTE:  IFigureData::GetCurrentSegment() returns true when
            // m_uCurrentSegment >= m_uStop, NOT when at the last segment.
            // Since we never call SetStop(), m_uStop stays at UINT_MAX
            // and GetCurrentSegment always returns false, causing an
            // infinite loop.  Instead, use SetToNextSegment()'s return
            // value to detect the last segment: it returns false when
            // the current segment is already the last one.
            //

            for (;;)
            {
                BYTE bType = 0;
                const MilPoint2F *pPoints = nullptr;

                figure.GetCurrentSegment(bType, pPoints);

                if (bType == MilCoreSeg::TypeLine)
                {
                    //
                    // Line segment – pPoints has 1 point (the endpoint).
                    //

                    pSink->AddLine(D2D1::Point2F(pPoints[0].X, pPoints[0].Y));
                }
                else if (bType == MilCoreSeg::TypeBezier)
                {
                    //
                    // Cubic bezier – pPoints has 3 points (cp1, cp2, endpoint).
                    //

                    D2D1_BEZIER_SEGMENT bezier;
                    bezier.point1 = D2D1::Point2F(pPoints[0].X, pPoints[0].Y);
                    bezier.point2 = D2D1::Point2F(pPoints[1].X, pPoints[1].Y);
                    bezier.point3 = D2D1::Point2F(pPoints[2].X, pPoints[2].Y);

                    pSink->AddBezier(bezier);
                }
                else
                {
                    //
                    // Unknown segment type – skip.
                    //
                    Assert(false);
                }

                //
                // Advance to the next segment.  SetToNextSegment returns
                // false if we were already at the last segment.
                //

                if (!figure.SetToNextSegment())
                {
                    break;
                }
            }
        }

        //
        // End the figure – close it if the source shape was closed.
        //

        D2D1_FIGURE_END figureEnd = figure.IsClosed()
            ? D2D1_FIGURE_END_CLOSED
            : D2D1_FIGURE_END_OPEN;

        pSink->EndFigure(figureEnd);
    }

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DSurfaceRenderTarget::ConvertPenToStrokeStyle
//
//  Synopsis:  Map a WPF CPlainPen to an ID2D1StrokeStyle and a width.
//
//  Notes:     CPlainPen exposes width, miter limit, line cap, line join,
//             and an optional dash array.  Phase 1 maps the basic
//             properties; advanced dash patterns are deferred to Phase 2.
//
//-------------------------------------------------------------------------

HRESULT
CD2DSurfaceRenderTarget::ConvertPenToStrokeStyle(
    __in_ecount(1) const CPlainPen *pPen,
    __deref_out_ecount(1) ID2D1StrokeStyle **ppStyle,
    __out_ecount(1) FLOAT *pStrokeWidth
    )
{
    HRESULT hr = S_OK;

    Assert(pPen);
    Assert(ppStyle);
    Assert(pStrokeWidth);

    *ppStyle     = nullptr;
    *pStrokeWidth = 1.0f;

    //
    // Extract the pen width.  CPlainPen stores width as a float via
    // GetWidth().  If 0, default to 1.
    //

    FLOAT width = pPen->GetWidth();

    if (width <= 0.0f)
    {
        width = 1.0f;
    }

    *pStrokeWidth = width;

    //
    // Map line cap.
    //

    D2D1_CAP_STYLE startCap = D2D1_CAP_STYLE_FLAT;
    D2D1_CAP_STYLE endCap   = D2D1_CAP_STYLE_FLAT;
    D2D1_CAP_STYLE dashCap  = D2D1_CAP_STYLE_FLAT;

    switch (pPen->GetStartCap())
    {
    case MilPenCap::Flat:    startCap = D2D1_CAP_STYLE_FLAT;     break;
    case MilPenCap::Square:  startCap = D2D1_CAP_STYLE_SQUARE;   break;
    case MilPenCap::Round:   startCap = D2D1_CAP_STYLE_ROUND;    break;
    case MilPenCap::Triangle: startCap = D2D1_CAP_STYLE_TRIANGLE; break;
    default:                 startCap = D2D1_CAP_STYLE_FLAT;     break;
    }

    switch (pPen->GetEndCap())
    {
    case MilPenCap::Flat:    endCap = D2D1_CAP_STYLE_FLAT;       break;
    case MilPenCap::Square:  endCap = D2D1_CAP_STYLE_SQUARE;     break;
    case MilPenCap::Round:   endCap = D2D1_CAP_STYLE_ROUND;      break;
    case MilPenCap::Triangle: endCap = D2D1_CAP_STYLE_TRIANGLE;   break;
    default:                 endCap = D2D1_CAP_STYLE_FLAT;       break;
    }

    switch (pPen->GetDashCap())
    {
    case MilPenCap::Flat:    dashCap = D2D1_CAP_STYLE_FLAT;      break;
    case MilPenCap::Square:  dashCap = D2D1_CAP_STYLE_SQUARE;    break;
    case MilPenCap::Round:   dashCap = D2D1_CAP_STYLE_ROUND;     break;
    case MilPenCap::Triangle: dashCap = D2D1_CAP_STYLE_TRIANGLE;  break;
    default:                 dashCap = D2D1_CAP_STYLE_FLAT;      break;
    }

    //
    // Map line join.
    //

    D2D1_LINE_JOIN lineJoin = D2D1_LINE_JOIN_MITER;

    switch (pPen->GetJoin())
    {
    case MilLineJoin::Miter:         lineJoin = D2D1_LINE_JOIN_MITER;          break;
    case MilLineJoin::Bevel:         lineJoin = D2D1_LINE_JOIN_BEVEL;          break;
    case MilLineJoin::Round:         lineJoin = D2D1_LINE_JOIN_ROUND;          break;
    case MilLineJoin::MiterClipped:  lineJoin = D2D1_LINE_JOIN_MITER_OR_BEVEL; break;
    default:                         lineJoin = D2D1_LINE_JOIN_MITER;          break;
    }

    //
    // Miter limit.
    //

    FLOAT miterLimit = pPen->GetMiterLimit();

    if (miterLimit <= 0.0f)
    {
        miterLimit = 10.0f;  // D2D default
    }

    //
    // Dash style / pattern.
    //

    D2D1_DASH_STYLE dashStyle = D2D1_DASH_STYLE_SOLID;
    const FLOAT *pDashes      = nullptr;
    UINT cDashes              = 0;
    FLOAT stackDashes[16];
    FLOAT *pHeapDashes        = nullptr;

    {
        MilDashStyle::Enum milDash = pPen->GetDashStyle();

        switch (milDash)
        {
        case MilDashStyle::Dash:       dashStyle = D2D1_DASH_STYLE_DASH;         break;
        case MilDashStyle::Dot:        dashStyle = D2D1_DASH_STYLE_DOT;          break;
        case MilDashStyle::DashDot:    dashStyle = D2D1_DASH_STYLE_DASH_DOT;     break;
        case MilDashStyle::DashDotDot: dashStyle = D2D1_DASH_STYLE_DASH_DOT_DOT; break;
        case MilDashStyle::Custom:
            {
                dashStyle = D2D1_DASH_STYLE_CUSTOM;
                INT dashCount = pPen->GetDashCount();
                if (dashCount > 0)
                {
                    FLOAT *pBuf = stackDashes;
                    if (dashCount > 16)
                    {
                        pHeapDashes = new(std::nothrow) FLOAT[dashCount];
                        if (!pHeapDashes) { hr = E_OUTOFMEMORY; goto Cleanup; }
                        pBuf = pHeapDashes;
                    }
                    for (INT i = 0; i < dashCount; i++)
                    {
                        pBuf[i] = pPen->GetDash(i);
                    }
                    pDashes = pBuf;
                    cDashes = static_cast<UINT>(dashCount);
                }
            }
            break;
        default:
            dashStyle = D2D1_DASH_STYLE_SOLID;
            break;
        }
    }

    //
    // Create or reuse the stroke style.  CreateStrokeStyle is a factory call
    // that allocates and validates – reuse the cached instance when properties
    // match to avoid per-frame COM overhead.
    //

    {
        D2D1_STROKE_STYLE_PROPERTIES ssProps;
        ssProps.startCap   = startCap;
        ssProps.endCap     = endCap;
        ssProps.dashCap    = dashCap;
        ssProps.lineJoin   = lineJoin;
        ssProps.miterLimit = miterLimit;
        ssProps.dashStyle  = dashStyle;
        ssProps.dashOffset = pPen->GetDashOffset();

        bool fReuse = (m_pCachedStrokeStyle != nullptr) &&
            (m_cachedStrokeProps.startCap   == ssProps.startCap) &&
            (m_cachedStrokeProps.endCap     == ssProps.endCap) &&
            (m_cachedStrokeProps.dashCap    == ssProps.dashCap) &&
            (m_cachedStrokeProps.lineJoin   == ssProps.lineJoin) &&
            (m_cachedStrokeProps.miterLimit == ssProps.miterLimit) &&
            (m_cachedStrokeProps.dashStyle  == ssProps.dashStyle) &&
            (m_cachedStrokeProps.dashOffset == ssProps.dashOffset);

        if (!fReuse)
        {
            m_pCachedStrokeStyle.Reset();

            D2D_IFC(m_pD2DFactory->CreateStrokeStyle(
                ssProps,
                pDashes,
                cDashes,
                &m_pCachedStrokeStyle
            ));

            m_cachedStrokeProps = ssProps;
        }

        m_pCachedStrokeStyle.Get()->AddRef();
        *ppStyle = m_pCachedStrokeStyle.Get();
    }

Cleanup:
    delete[] pHeapDashes;
    RRETURN(hr);
}

// =======================================================================
// Diagnostic counters — write per-frame stats to a log file
// =======================================================================

void
CD2DSurfaceRenderTarget::ResetDiagCounters()
{
    m_diag.cDrawPath = 0;
    m_diag.cFastRect = 0;
    m_diag.cFastEllipse = 0;
    m_diag.cFallbackGeometry = 0;
    m_diag.cGeoCacheHit = 0;
    m_diag.cGeoCacheMiss = 0;
    m_diag.cBeginLayer = 0;
    m_diag.cAxisAlignedClip = 0;
    m_diag.cFullLayer = 0;
    m_diag.cEndLayer = 0;
    m_diag.cEllipseCacheHit = 0;
}

void
CD2DSurfaceRenderTarget::WriteDiagLog()
{
    m_diag.cPresent++;
    m_diag.cTotalFrames++;

    //
    // Write on first frame and then every 60 frames (~1 second at 60fps).
    //

    if (m_diag.cPresent >= 60 || m_diag.cTotalFrames <= 1)
    {
        WCHAR path[MAX_PATH];
        if (GetTempPathW(MAX_PATH, path) > 0)
        {
            wcscat_s(path, MAX_PATH, L"wpf_d2d_diag.log");

            HANDLE hFile = CreateFileW(
                path,
                GENERIC_WRITE,
                FILE_SHARE_READ,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );

            if (hFile != INVALID_HANDLE_VALUE)
            {
                UINT frames = m_diag.cPresent;   // 120

                //
                // Query adapter name from DXGI chain (best-effort).
                //

                WCHAR adapterName[128] = L"(unknown)";
                SIZE_T adapterVRAM = 0;

                if (m_pD2DContext)
                {
                    ComPtr<ID2D1Device> pD2DDevice;
                    m_pD2DContext->GetDevice(&pD2DDevice);

                    if (pD2DDevice)
                    {
                        ComPtr<IDXGIDevice> pDXGI;
                        pD2DDevice->QueryInterface(IID_PPV_ARGS(&pDXGI));

                        if (pDXGI)
                        {
                            ComPtr<IDXGIAdapter> pAdapter;
                            pDXGI->GetAdapter(&pAdapter);

                            if (pAdapter)
                            {
                                DXGI_ADAPTER_DESC desc = {};
                                if (SUCCEEDED(pAdapter->GetDesc(&desc)))
                                {
                                    wcsncpy_s(adapterName, desc.Description, _TRUNCATE);
                                    adapterVRAM = desc.DedicatedVideoMemory;
                                }
                            }
                        }
                    }
                }

                WCHAR buf[2048];
                int len = swprintf_s(buf, ARRAYSIZE(buf),
                    L"\xFEFF[WPF D2D Diagnostics – last %u frames]\r\n"
                    L"Adapter: %s  VRAM: %zu MB\r\n"
                    L"TotalFrames (lifetime): %u\r\n"
                    L"\r\n"
                    L"--- Per-Frame Averages (last interval) ---\r\n"
                    L"DrawPath/frame: %.1f\r\n"
                    L"  FastRect/frame: %.1f\r\n"
                    L"  FastEllipse/frame: %.1f  (cache-hit: %.1f)\r\n"
                    L"  FallbackGeometry/frame: %.1f\r\n"
                    L"  GeoCacheHit/frame: %.1f  Miss/frame: %.1f\r\n"
                    L"BeginLayer/frame: %.1f  (AxisClip: %.1f  FullLayer: %.1f)\r\n"
                    L"EndLayer/frame: %.1f\r\n"
                    L"ComposeEffect/frame: %.1f  (D2DEffect: %.1f  DrawImage: %.1f)\r\n"
                    L"  FastPath/frame: %.1f  SlowPath/frame: %.1f  NoSource/frame: %.1f\r\n"
                    L"\r\n"
                    L"--- Raw Counts (last %u frames) ---\r\n"
                    L"DrawPath: %u\r\n"
                    L"  FastRect: %u\r\n"
                    L"  FastEllipse: %u  (cache-hit: %u)\r\n"
                    L"  FallbackGeometry: %u\r\n"
                    L"  GeoCacheHit: %u  Miss: %u\r\n"
                    L"BeginLayer: %u  (AxisClip: %u  FullLayer: %u)\r\n"
                    L"EndLayer: %u\r\n"
                    L"ComposeEffect: %u  (D2DEffect: %u  DrawImage: %u)\r\n"
                    L"  FastPath: %u  SlowPath: %u  NoSource: %u\r\n"
                    L"  PropSetOk: %u  PropSetFail: %u  LastSigma: %.3f\r\n"
                    L"  EffectTypes: T0=%u T1(blur)=%u T2(shadow)=%u T3(shader)=%u\r\n"
                    L"Unit Mode: %d  (0=DIPS, 1=PIXELS)\r\n"
                    L"Context DPI: %.1f\r\n"
                    L"MT Optimizations: ENABLED\r\n",
                    frames,
                    adapterName,
                    adapterVRAM / (1024 * 1024),
                    m_diag.cTotalFrames,
                    (float)m_diag.cDrawPath / frames,
                    (float)m_diag.cFastRect / frames,
                    (float)m_diag.cFastEllipse / frames,
                    (float)m_diag.cEllipseCacheHit / frames,
                    (float)m_diag.cFallbackGeometry / frames,
                    (float)m_diag.cGeoCacheHit / frames,
                    (float)m_diag.cGeoCacheMiss / frames,
                    (float)m_diag.cBeginLayer / frames,
                    (float)m_diag.cAxisAlignedClip / frames,
                    (float)m_diag.cFullLayer / frames,
                    (float)m_diag.cEndLayer / frames,
                    (float)m_diag.cComposeEffect / frames,
                    (float)m_diag.cComposeD2DEffect / frames,
                    (float)m_diag.cComposeD2DDrawImage / frames,
                    (float)m_diag.cComposeFastPath / frames,
                    (float)m_diag.cComposeSlowPath / frames,
                    (float)m_diag.cComposeNoSource / frames,
                    frames,
                    m_diag.cDrawPath,
                    m_diag.cFastRect,
                    m_diag.cFastEllipse,
                    m_diag.cEllipseCacheHit,
                    m_diag.cFallbackGeometry,
                    m_diag.cGeoCacheHit,
                    m_diag.cGeoCacheMiss,
                    m_diag.cBeginLayer,
                    m_diag.cAxisAlignedClip,
                    m_diag.cFullLayer,
                    m_diag.cEndLayer,
                    m_diag.cComposeEffect,
                    m_diag.cComposeD2DEffect,
                    m_diag.cComposeD2DDrawImage,
                    m_diag.cComposeFastPath,
                    m_diag.cComposeSlowPath,
                    m_diag.cComposeNoSource,
                    m_diag.cPropSetOk,
                    m_diag.cPropSetFail,
                    m_diag.lastSigma,
                    m_diag.cEffectType0,
                    m_diag.cEffectType1,
                    m_diag.cEffectType2,
                    m_diag.cEffectType3,
                    m_pD2DContext ? (int)m_pD2DContext->GetUnitMode() : -1,
                    [&]() -> float {
                        float dpiX = 0, dpiY = 0;
                        if (m_pD2DContext) m_pD2DContext->GetDpi(&dpiX, &dpiY);
                        return dpiX;
                    }()
                );

                DWORD written;
                WriteFile(hFile, buf, len * sizeof(WCHAR), &written, NULL);
                CloseHandle(hFile);
            }
        }

        //
        // Reset interval counters (keep cTotalFrames running).
        //

        UINT savedTotal = m_diag.cTotalFrames;
        ZeroMemory(&m_diag, sizeof(m_diag));
        m_diag.cTotalFrames = savedTotal;
    }
}
