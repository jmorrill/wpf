// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2DTextureRenderTarget and CD2DBitmapSource implementation.
//
//----------------------------------------------------------------------------

#include <precomp.hpp>

#ifndef UNCONDITIONAL_EXPR
#define UNCONDITIONAL_EXPR(x) (x)
#endif

#ifndef D2D_IFC
#define D2D_IFC(expr)                   \
    do {                                \
        hr = (expr);                    \
        if (FAILED(hr))                 \
        {                               \
            goto Cleanup;               \
        }                               \
    } while (UNCONDITIONAL_EXPR(0))
#endif

#ifndef RRETURN
#define RRETURN(hr) return (hr)
#endif


// ====================================================================
// CD2DBitmapSource
// ====================================================================

CD2DBitmapSource::CD2DBitmapSource(
    __in ID2D1Bitmap1 *pBitmap,
    __in ID2D1DeviceContext1 *pContext,
    UINT width,
    UINT height
    )
    : m_cRef(1),
      m_uWidth(width),
      m_uHeight(height),
      m_pBitmap(pBitmap),
      m_pContext(pContext)
{
}

CD2DBitmapSource::~CD2DBitmapSource()
{
}

// IUnknown

STDMETHODIMP
CD2DBitmapSource::QueryInterface(REFIID riid, void **ppv)
{
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown))
    {
        *ppv = static_cast<IWGXBitmapSource *>(this);
    }
    else if (riid == IID_IWGXBitmapSource)
    {
        *ppv = static_cast<IWGXBitmapSource *>(this);
    }
    else if (riid == IID_ID2DBitmapSourceInternal)
    {
        *ppv = static_cast<ID2DBitmapSourceInternal *>(this);
    }
    else
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG)
CD2DBitmapSource::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG)
CD2DBitmapSource::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);
    if (cRef == 0)
    {
        delete this;
    }
    return static_cast<ULONG>(cRef);
}

// IWGXBitmapSource

STDMETHODIMP
CD2DBitmapSource::GetSize(UINT *puWidth, UINT *puHeight)
{
    if (!puWidth || !puHeight) return E_POINTER;
    *puWidth  = m_uWidth;
    *puHeight = m_uHeight;
    return S_OK;
}

STDMETHODIMP
CD2DBitmapSource::GetPixelFormat(MilPixelFormat::Enum *pPixelFormat)
{
    if (!pPixelFormat) return E_POINTER;
    *pPixelFormat = MilPixelFormat::PBGRA32bpp;
    return S_OK;
}

STDMETHODIMP
CD2DBitmapSource::GetResolution(double *pDpiX, double *pDpiY)
{
    if (!pDpiX || !pDpiY) return E_POINTER;
    *pDpiX = 96.0;
    *pDpiY = 96.0;
    return S_OK;
}

STDMETHODIMP
CD2DBitmapSource::CopyPalette(IWICPalette *pIPalette)
{
    UNREFERENCED_PARAMETER(pIPalette);
    return WINCODEC_ERR_PALETTEUNAVAILABLE;
}

STDMETHODIMP
CD2DBitmapSource::CopyPixels(
    const MILRect *prc,
    UINT cbStride,
    UINT cbBufferSize,
    BYTE *pvPixels
    )
{
    HRESULT hr = S_OK;

    if (!pvPixels) return E_POINTER;

    //
    // GPU readback path — create a CPU-readable staging bitmap, copy from
    // the render target bitmap, then Map() to read the pixel data.
    //

    UINT srcX = 0, srcY = 0;
    UINT copyW = m_uWidth, copyH = m_uHeight;

    if (prc != nullptr)
    {
        srcX  = static_cast<UINT>(prc->X);
        srcY  = static_cast<UINT>(prc->Y);
        copyW = static_cast<UINT>(prc->Width);
        copyH = static_cast<UINT>(prc->Height);
    }

    {
        //
        // Create a staging bitmap with CPU_READ.
        //

        ComPtr<ID2D1Bitmap1> pStagingBitmap;

        D2D1_BITMAP_PROPERTIES1 stagingProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );

        D2D_IFC(m_pContext->CreateBitmap(
            D2D1::SizeU(copyW, copyH),
            nullptr,
            0,
            stagingProps,
            &pStagingBitmap
        ));

        //
        // Copy from the source bitmap to the staging bitmap.
        //

        D2D1_POINT_2U destPt = D2D1::Point2U(0, 0);
        D2D1_RECT_U   srcRect = D2D1::RectU(srcX, srcY, srcX + copyW, srcY + copyH);

        D2D_IFC(pStagingBitmap->CopyFromBitmap(&destPt, m_pBitmap.Get(), &srcRect));

        //
        // Map the staging bitmap and copy to the caller's buffer.
        //

        D2D1_MAPPED_RECT mappedRect = {};
        D2D_IFC(pStagingBitmap->Map(D2D1_MAP_OPTIONS_READ, &mappedRect));

        UINT rowBytes = copyW * 4;

        for (UINT y = 0; y < copyH; ++y)
        {
            BYTE *pDst = pvPixels + y * cbStride;
            const BYTE *pSrc = mappedRect.bits + y * mappedRect.pitch;

            UINT bytesToCopy = min(rowBytes, cbStride);
            if ((y * cbStride + bytesToCopy) > cbBufferSize) break;

            memcpy(pDst, pSrc, bytesToCopy);
        }

        pStagingBitmap->Unmap();
    }

Cleanup:
    RRETURN(hr);
}

// ID2DBitmapSourceInternal

STDMETHODIMP
CD2DBitmapSource::GetD2DBitmap(__deref_out ID2D1Bitmap1 **ppBitmap)
{
    if (!ppBitmap) return E_POINTER;

    *ppBitmap = m_pBitmap.Get();
    (*ppBitmap)->AddRef();
    return S_OK;
}


// ====================================================================
// CD2DTextureRenderTarget
// ====================================================================

CD2DTextureRenderTarget::CD2DTextureRenderTarget(
    __in CD2DDevice *pDevice,
    DisplayId associatedDisplay
    )
    : CD2DSurfaceRenderTarget(pDevice, associatedDisplay)
{
}

CD2DTextureRenderTarget::~CD2DTextureRenderTarget()
{
    //
    // Flush any open draw batch before tearing down.
    //

    if (m_fDrawBatchOpen && m_pD2DContext)
    {
        m_pD2DContext->EndDraw();
        m_fDrawBatchOpen = false;
    }
}

// -----------------------------------------------------------------------
// IUnknown
// -----------------------------------------------------------------------

STDMETHODIMP
CD2DTextureRenderTarget::QueryInterface(REFIID riid, void **ppv)
{
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown))
    {
        *ppv = static_cast<IUnknown *>(
            static_cast<CD2DSurfaceRenderTarget *>(this));
    }
    else if (riid == IID_IRenderTargetInternal)
    {
        *ppv = static_cast<IRenderTargetInternal *>(
            static_cast<CD2DSurfaceRenderTarget *>(this));
    }
    else if (riid == IID_IMILRenderTarget)
    {
        *ppv = static_cast<IMILRenderTarget *>(
            static_cast<CD2DSurfaceRenderTarget *>(this));
    }
    else if (riid == IID_IMILRenderTargetBitmap)
    {
        *ppv = static_cast<IMILRenderTargetBitmap *>(this);
    }
    else
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG)
CD2DTextureRenderTarget::AddRef()
{
    return CD2DSurfaceRenderTarget::AddRef();
}

STDMETHODIMP_(ULONG)
CD2DTextureRenderTarget::Release()
{
    return CD2DSurfaceRenderTarget::Release();
}

// -----------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------

HRESULT
CD2DTextureRenderTarget::Create(
    __in CD2DDevice *pDevice,
    DisplayId associatedDisplay,
    UINT width,
    UINT height,
    __deref_out CD2DTextureRenderTarget **ppRT
    )
{
    HRESULT hr = S_OK;

    *ppRT = nullptr;

    CD2DTextureRenderTarget *pRT = new(std::nothrow) CD2DTextureRenderTarget(
        pDevice, associatedDisplay);

    if (pRT == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    hr = pRT->Init(width, height);

    if (FAILED(hr))
    {
        delete pRT;
        return hr;
    }

    *ppRT = pRT;
    return S_OK;
}

// -----------------------------------------------------------------------
// Init — create a device context and an off-screen target bitmap
// -----------------------------------------------------------------------

HRESULT
CD2DTextureRenderTarget::Init(UINT width, UINT height)
{
    HRESULT hr = S_OK;

    //
    // Create a dedicated device context for this intermediate RT.
    //

    {
        ID2D1Device1 *pD2DDevice = m_pDevice->GetD2DDevice();
        if (pD2DDevice == nullptr)
        {
            return E_UNEXPECTED;
        }

        D2D_IFC(pD2DDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &m_pD2DContext
        ));

        //
        // Match the display render target settings: pixel mode, per-primitive AA.
        // Use PER_PRIMITIVE (not ALIASED) so intermediate content is properly
        // anti-aliased before effects are applied.
        //

        m_pD2DContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        m_pD2DContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_pD2DContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        m_pD2DContext->SetDpi(96.0f, 96.0f);

        //
        // Cache the factory.
        //

        m_pD2DContext->GetFactory(m_pD2DFactory.ReleaseAndGetAddressOf());
    }

    //
    // Create the off-screen target bitmap.
    //
    // D2D1_BITMAP_OPTIONS_TARGET allows rendering into it.
    // We do NOT set CPU_READ here — that would force a slow staging texture.
    // The fast-path compositing via ID2DBitmapSourceInternal avoids readback.
    //

    {
        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );

        D2D_IFC(m_pD2DContext->CreateBitmap(
            D2D1::SizeU(width, height),
            nullptr,    // no initial data
            0,          // pitch
            bmpProps,
            &m_pTargetBitmap
        ));
    }

    //
    // Set the target and dimensions.
    //

    m_pD2DContext->SetTarget(m_pTargetBitmap.Get());

    m_uWidth  = width;
    m_uHeight = height;

Cleanup:
    RRETURN(hr);
}

// -----------------------------------------------------------------------
// IMILRenderTargetBitmap
// -----------------------------------------------------------------------

STDMETHODIMP
CD2DTextureRenderTarget::GetBitmapSource(
    __deref_out_ecount(1) IWGXBitmapSource ** const ppIBitmapSource
    )
{
    HRESULT hr = S_OK;

    if (!ppIBitmapSource) return E_POINTER;
    *ppIBitmapSource = nullptr;

    //
    // Flush any pending draws so the bitmap contains the final content.
    //

    D2D_IFC(FlushDrawBatch());

    //
    // Release the target so that the bitmap is no longer bound to this
    // context.  A D2D bitmap still set as a target cannot be used as a
    // source by another device context — attempting to do so causes
    // EndDraw to return E_ACCESSDENIED.
    //

    m_pD2DContext->SetTarget(nullptr);

    //
    // Create a CD2DBitmapSource wrapper.  The caller owns the returned
    // reference.  The wrapper holds refs on the bitmap and context.
    //

    {
        CD2DBitmapSource *pBitmapSource = new(std::nothrow) CD2DBitmapSource(
            m_pTargetBitmap.Get(),
            m_pD2DContext.Get(),
            m_uWidth,
            m_uHeight
        );

        if (pBitmapSource == nullptr)
        {
            hr = E_OUTOFMEMORY;
            goto Cleanup;
        }

        *ppIBitmapSource = pBitmapSource;
        // Constructor sets ref count to 1 — transfer ownership.
    }

Cleanup:
    RRETURN(hr);
}

STDMETHODIMP
CD2DTextureRenderTarget::GetCacheableBitmapSource(
    __deref_out_ecount(1) IWGXBitmapSource ** const ppIBitmapSource
    )
{
    return GetBitmapSource(ppIBitmapSource);
}

STDMETHODIMP
CD2DTextureRenderTarget::GetBitmap(
    __deref_out_ecount(1) IWGXBitmap ** const ppIBitmap
    )
{
    //
    // IWGXBitmap extends IWGXBitmapSource with Lock/SetPalette/etc.
    // We don't support the full IWGXBitmap interface for GPU-backed
    // intermediates.  Return E_NOTIMPL — callers that need IWGXBitmap
    // can fall back to the software path.
    //

    if (ppIBitmap) *ppIBitmap = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP
CD2DTextureRenderTarget::GetNumQueuedPresents(
    __out_ecount(1) UINT *puNumQueuedPresents
    )
{
    if (!puNumQueuedPresents) return E_POINTER;
    *puNumQueuedPresents = 0;
    return S_OK;
}

