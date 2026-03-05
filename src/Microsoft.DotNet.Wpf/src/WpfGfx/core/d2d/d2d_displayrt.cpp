// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+-----------------------------------------------------------------------------
//
//
//  Description:
//      CD2DDisplayRenderTarget implementation.
//
//      Manages an IDXGISwapChain1, an ID2D1DeviceContext targeting the swap
//      chain back buffer, dirty-rect tracking, presentation, resize, and
//      VBlank synchronization for HWND-based rendering via Direct2D.
//
//------------------------------------------------------------------------------

#include <precomp.hpp>

// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::Create
//
//  Synopsis:
//      1. Obtain the CD2DDevice for the display's adapter.
//      2. Allocate and initialize a CD2DDisplayRenderTarget.
//
// ---------------------------------------------------------------------------

HRESULT
CD2DDisplayRenderTarget::Create(
    __in_opt HWND hwnd,
    MilWindowLayerType::Enum eWindowLayerType,
    __in_ecount(1) CDisplay const *pDisplay,
    MilRTInitialization::Flags dwFlags,
    __deref_out CD2DDisplayRenderTarget **ppRenderTarget
    )
{
    HRESULT hr = S_OK;

    Assert(hwnd);

    *ppRenderTarget = NULL;

    CD2DDevice *pD2DDevice = NULL;

    //
    // Obtain the D2D device manager singleton and get the D2D device
    // corresponding to this displays adapter.
    //

    CD2DDeviceManager *pD2DDeviceManager = CD2DDeviceManager::Get();

    //
    // Lazily create the device manager singleton if it wasn't created
    // during D2DSubsystem::Init() (e.g. env var was set but not yet
    // propagated at that time).
    //

    if (pD2DDeviceManager == nullptr)
    {
        IFC(CD2DDeviceManager::Create(&pD2DDeviceManager));
    }

    IFC(pD2DDeviceManager->GetD2DDevice(
        pDisplay,
        &pD2DDevice
        ));

    //
    // Allocate the render target.
    //

    {
        DisplayId associatedDisplay = pDisplay->GetDisplayId();

        *ppRenderTarget = new CD2DDisplayRenderTarget(
            pD2DDevice,
            associatedDisplay
            );

        IFCOOM(*ppRenderTarget);
        (*ppRenderTarget)->AddRef();
    }

    //
    // Call Init — creates swap chain, D2D target bitmap, etc.
    //

    IFC((*ppRenderTarget)->Init(
        hwnd,
        eWindowLayerType,
        pDisplay,
        dwFlags
        ));

Cleanup:
    if (FAILED(hr))
    {
        ReleaseInterface(*ppRenderTarget);
    }
    ReleaseInterfaceNoNULL(pD2DDevice);
    // Note: pD2DDeviceManager is a raw singleton pointer, do NOT call
    // Release() here — that destroys the singleton.  The singleton
    // lifetime is managed by D2DSubsystem::DeInit().

    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::CD2DDisplayRenderTarget
//
//  Synopsis:  ctor
//
// ---------------------------------------------------------------------------

CD2DDisplayRenderTarget::CD2DDisplayRenderTarget(
    __in_ecount(1) CD2DDevice *pDevice,
    DisplayId associatedDisplay
    )
    : CD2DSurfaceRenderTarget(pDevice, associatedDisplay)
{
    m_hwnd                  = NULL;
    m_fAllowTearing         = FALSE;
    m_ptOrigin.x            = 0;
    m_ptOrigin.y            = 0;
    m_eWindowLayerType      = MilWindowLayerType::NotLayered;
    m_fEnableRendering      = TRUE;
    m_hrDisplayInvalid      = S_OK;
    m_dwFlags               = MilRTInitialization::Default;
    m_transparencyFlags     = MilTransparency::Opaque;
    m_constantAlpha         = 255;
    m_colorKey              = 0;
    m_uFrameNumber          = 0;
    m_cDirtyRects           = 0;

    ZeroMemory(m_rgDirtyRects, sizeof(m_rgDirtyRects));
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::~CD2DDisplayRenderTarget
//
//  Synopsis:  dtor — release D2D and DXGI resources.
//
// ---------------------------------------------------------------------------

CD2DDisplayRenderTarget::~CD2DDisplayRenderTarget()
{
    //
    // Flush any pending D2D draw batch before tearing down.
    //

    FlushDrawBatch();

    //
    // Release the swap chain target before the chain goes away.
    // m_pD2DContext and m_pTargetBitmap are base class members —
    // the base destructor handles their final cleanup.
    //

    if (m_pD2DContext)
    {
        m_pD2DContext->SetTarget(nullptr);
    }

    m_pTargetBitmap.Reset();
    m_pSwapChain.Reset();
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::QueryInterface
//
// ---------------------------------------------------------------------------

STDMETHODIMP
CD2DDisplayRenderTarget::QueryInterface(
    REFIID riid,
    void **ppv
    )
{
    HRESULT hr = E_NOINTERFACE;

    if (ppv == NULL)
    {
        return E_INVALIDARG;
    }

    *ppv = NULL;

    if (riid == __uuidof(IUnknown))
    {
        *ppv = static_cast<IUnknown *>(static_cast<CD2DSurfaceRenderTarget *>(this));
    }
    else if (riid == IID_IRenderTargetInternal)
    {
        *ppv = static_cast<IRenderTargetInternal *>(static_cast<CD2DSurfaceRenderTarget *>(this));
    }
    else
    {
        // IRenderTargetHWNDInternal has no GUID; callers use static_cast directly.
        // Return E_NOINTERFACE for anything else.
    }

    if (*ppv)
    {
        AddRef();
        hr = S_OK;
    }

    return hr;
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::AddRef / Release
//
//  Synopsis:  Delegate to the base COM implementation.
//
// ---------------------------------------------------------------------------

STDMETHODIMP_(ULONG)
CD2DDisplayRenderTarget::AddRef()
{
    return CD2DSurfaceRenderTarget::AddRef();
}

STDMETHODIMP_(ULONG)
CD2DDisplayRenderTarget::Release()
{
    return CD2DSurfaceRenderTarget::Release();
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::Init
//
//  Synopsis:
//      1. Store HWND and flags.
//      2. Get client rect dimensions.
//      3. Call CreateSwapChainAndTarget().
//
// ---------------------------------------------------------------------------

HRESULT
CD2DDisplayRenderTarget::Init(
    __in_opt HWND hwnd,
    MilWindowLayerType::Enum eWindowLayerType,
    __in_ecount(1) CDisplay const *pDisplay,
    MilRTInitialization::Flags dwFlags
    )
{
    HRESULT hr = S_OK;

    Assert(hwnd);

    m_hwnd              = hwnd;
    m_eWindowLayerType  = eWindowLayerType;
    m_dwFlags           = dwFlags;

    //
    // Create the D2D device context from the device.
    //

    {
        // Use ID2D1Device1 to get an ID2D1DeviceContext1 that matches the
        // base class member type (ComPtr<ID2D1DeviceContext1>).
        ID2D1Device1 *pD2DDevice = GetD2DDeviceNoRef()->GetD2DDevice();

        //
        // ENABLE_MULTI_THREADED_OPTIMIZATIONS: The MS D2D perf guide
        // explicitly recommends this for apps rendering significant
        // geometric content.  D2D distributes path tessellation across
        // all logical CPU cores.
        //

        IFC(pD2DDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS,
            &m_pD2DContext
            ));

        //
        // Cache the D2D factory for per-draw use (avoids GetFactory COM
        // round-trips in the base class draw methods).
        //

        m_pD2DContext->GetFactory(m_pD2DFactory.ReleaseAndGetAddressOf());

        //
        // CRITICAL: Set context DPI to 96×96 to match the back-buffer
        // bitmap DPI.  D2D device contexts inherit system DPI from the
        // factory (e.g. 144 at 150% scaling).  A mismatch between the
        // context DPI and the target bitmap DPI causes D2D to insert
        // internal software scaling / compositing passes, which show
        // up as high CPU / low GPU utilisation.
        //

        m_pD2DContext->SetDpi(96.0f, 96.0f);

        //
        // Use PIXELS unit mode so D2D does not apply its own DPI scaling
        // on top of WPF's already-scaled coordinates.  This avoids a
        // redundant scale pass and wasted fill rate.
        //

        m_pD2DContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

        //
        // Use PER_PRIMITIVE (default) antialias mode.  With this mode,
        // D2D uses GPU-based MSAA for edge antialiasing, which shifts
        // work from CPU to GPU.  ALIASED mode forces a CPU-side
        // scan-converter for edge computation which causes high CPU
        // utilization.
        //

        m_pD2DContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        //
        // Use grayscale text AA — lighter weight than ClearType and avoids
        // subpixel colour fringing on transparent backgrounds.
        //

        m_pD2DContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }

    //
    // Determine initial swap chain size from the HWND client rect.
    //

    {
        RECT rcClient = {};
        ::GetClientRect(hwnd, &rcClient);

        UINT uWidth  = static_cast<UINT>(rcClient.right  - rcClient.left);
        UINT uHeight = static_cast<UINT>(rcClient.bottom - rcClient.top);

        //
        // A minimized window may report 0×0.  Defer swap chain creation
        // until a non-zero Resize arrives.
        //

        if (uWidth > 0 && uHeight > 0)
        {
            IFC(CreateSwapChainAndTarget(uWidth, uHeight));

            m_uWidth  = uWidth;
            m_uHeight = uHeight;

            //
            // Debug breadcrumb — verify D2D init was reached.
            //
            {
                WCHAR breadcrumb[MAX_PATH];
                if (GetTempPathW(MAX_PATH, breadcrumb) > 0)
                {
                    wcscat_s(breadcrumb, MAX_PATH, L"wpf_d2d_init.log");
                    HANDLE hf = CreateFileW(breadcrumb, GENERIC_WRITE, FILE_SHARE_READ,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hf != INVALID_HANDLE_VALUE)
                    {
                        WCHAR msg[256];
                        int n = swprintf_s(msg, ARRAYSIZE(msg),
                            L"D2D DisplayRT Init OK  %ux%u\r\n", uWidth, uHeight);
                        DWORD w;
                        WriteFile(hf, msg, n * sizeof(WCHAR), &w, NULL);
                        CloseHandle(hf);
                    }
                }
            }
        }
        else
        {
            m_fEnableRendering = FALSE;
        }
    }

Cleanup:
    if (FAILED(hr))
    {
        m_fEnableRendering = FALSE;
    }

    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::CreateSwapChainAndTarget
//
//  Synopsis:
//      1. Create DXGI swap chain via IDXGIFactory2::CreateSwapChainForHwnd.
//      2. Get back buffer as IDXGISurface.
//      3. Create ID2D1Bitmap1 from DXGI surface with TARGET | CANNOT_DRAW.
//      4. Set as target on D2D device context.
//
// ---------------------------------------------------------------------------

HRESULT
CD2DDisplayRenderTarget::CreateSwapChainAndTarget(
    UINT width,
    UINT height
    )
{
    HRESULT hr = S_OK;

    ComPtr<IDXGIDevice1>   pDXGIDevice;
    ComPtr<IDXGIAdapter>   pDXGIAdapter;
    ComPtr<IDXGIFactory2>  pDXGIFactory;
    ComPtr<IDXGISurface>   pBackBufferSurface;

    Assert(m_hwnd);
    Assert(width > 0 && height > 0);

    //
    // Obtain the DXGI factory from the underlying D3D11 device so that
    // the swap chain is created on the same adapter.
    //

    {
        ID3D11Device *pD3D11Device = GetD2DDeviceNoRef()->GetD3D11Device();

        IFC(pD3D11Device->QueryInterface(IID_PPV_ARGS(&pDXGIDevice)));
        IFC(pDXGIDevice->GetAdapter(&pDXGIAdapter));
        IFC(pDXGIAdapter->GetParent(IID_PPV_ARGS(&pDXGIFactory)));
    }

    //
    // Describe the swap chain.
    //
    // - FLIP_DISCARD requires full-frame repaints (SetEmpty in desktoprt.cpp).
    //   WPF's animation system needs this because valid-content tracking
    //   doesn't account for multi-buffer rotation.  Triple-buffer avoids
    //   stalls when GPU is slightly behind VSync.
    //
    // - Tearing is disabled - present with SyncInterval=1 for flicker-free UI.
    //
    // - Layered (per-pixel alpha) windows require PREMULTIPLIED alpha
    //   mode; all others use IGNORE.
    //

    {
        DXGI_SWAP_CHAIN_DESC1 swapDesc = {};

        swapDesc.Width              = width;
        swapDesc.Height             = height;
        swapDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.Stereo             = FALSE;
        swapDesc.SampleDesc.Count   = 1;
        swapDesc.SampleDesc.Quality = 0;
        swapDesc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount        = 3;    // Triple-buffer for FLIP_DISCARD
        swapDesc.Scaling            = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.Flags              = 0;

        //
        // Check whether tearing (non-VSync’d) presentation is supported.
        // Without ALLOW_TEARING, even SyncInterval=0 is VSync’d on
        // FLIP swap chains, causing a 2× FPS cliff when frame time
        // marginally exceeds the VSync period.
        //

        m_fAllowTearing = FALSE;

        if (m_eWindowLayerType != MilWindowLayerType::NotLayered)
        {
            swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        }
        else
        {
            swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        }

        //
        // Release any existing swap chain and target bitmap before
        // creating a new one.
        //

        if (m_pD2DContext)
        {
            m_pD2DContext->SetTarget(nullptr);
        }

        m_pTargetBitmap.Reset();
        m_pSwapChain.Reset();

        IFC(pDXGIFactory->CreateSwapChainForHwnd(
            pDXGIDevice.Get(),
            m_hwnd,
            &swapDesc,
            NULL,       // pFullscreenDesc
            NULL,       // pRestrictToOutput
            &m_pSwapChain
            ));
    }

    //
    // Acquire the back buffer and create the D2D target bitmap.
    //

    IFC(AcquireBackBufferAsBitmap());

Cleanup:
    if (FAILED(hr))
    {
        m_pTargetBitmap.Reset();
        m_pSwapChain.Reset();
        m_fEnableRendering = FALSE;

        //
        // Translate known DXGI device errors to the WPF display-invalid code.
        //

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            m_hrDisplayInvalid = WGXERR_DISPLAYSTATEINVALID;
            hr = WGXERR_DISPLAYSTATEINVALID;
        }
    }

    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::AcquireBackBufferAsBitmap
//
//  Synopsis:
//      Get the current back buffer as an IDXGISurface, wrap it in an
//      ID2D1Bitmap1 with TARGET | CANNOT_DRAW, and set it as the device
//      context target.  Called after swap chain creation and after every
//      Present (flip-model swap chains rotate buffers).
//
// ---------------------------------------------------------------------------

HRESULT
CD2DDisplayRenderTarget::AcquireBackBufferAsBitmap()
{
    HRESULT hr = S_OK;

    ComPtr<IDXGISurface> pBackBuffer;

    Assert(m_pSwapChain);
    Assert(m_pD2DContext);

    //
    // Release our reference to the current bitmap.  We set the new
    // target directly (no need to clear to nullptr first).
    //

    m_pTargetBitmap.Reset();

    //
    // Get the back buffer as a DXGI surface.
    //

    IFC(m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)));

    //
    // Create an ID2D1Bitmap1 wrapping the DXGI surface.
    //

    {
        D2D1_BITMAP_PROPERTIES1 bmpProps = {};

        bmpProps.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        bmpProps.pixelFormat.alphaMode = (m_eWindowLayerType != MilWindowLayerType::NotLayered)
                                            ? D2D1_ALPHA_MODE_PREMULTIPLIED
                                            : D2D1_ALPHA_MODE_IGNORE;
        bmpProps.dpiX       = 96.0f;
        bmpProps.dpiY       = 96.0f;
        bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        IFC(m_pD2DContext->CreateBitmapFromDxgiSurface(
            pBackBuffer.Get(),
            &bmpProps,
            &m_pTargetBitmap
            ));
    }

    //
    // Set as the active render target on the device context.
    //

    m_pD2DContext->SetTarget(m_pTargetBitmap.Get());

Cleanup:
    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::HandleDeviceLost
//
//  Synopsis:
//      Checks whether an HRESULT indicates DXGI device loss and, if so,
//      records the display-invalid state.
//
// ---------------------------------------------------------------------------

HRESULT
CD2DDisplayRenderTarget::HandleDeviceLost(
    HRESULT hrLost
    )
{
    if (hrLost == DXGI_ERROR_DEVICE_REMOVED ||
        hrLost == DXGI_ERROR_DEVICE_RESET)
    {
        m_fEnableRendering = FALSE;
        m_hrDisplayInvalid = WGXERR_DISPLAYSTATEINVALID;
        return WGXERR_DISPLAYSTATEINVALID;
    }

    return hrLost;
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::SetPosition
//
//  Synopsis:
//      Remember the presentation origin for layered-window updates.
//
// ---------------------------------------------------------------------------

void
CD2DDisplayRenderTarget::SetPosition(
    POINT ptOrigin
    )
{
    m_ptOrigin = ptOrigin;
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::UpdatePresentProperties
//
//  Synopsis:
//      Remember transparency properties for layered-window presentation.
//
// ---------------------------------------------------------------------------

void
CD2DDisplayRenderTarget::UpdatePresentProperties(
    MilTransparency::Flags transparencyFlags,
    BYTE constantAlpha,
    COLORREF colorKey
    )
{
    m_transparencyFlags = transparencyFlags;
    m_constantAlpha     = constantAlpha;
    m_colorKey          = colorKey;
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::Present
//
//  Synopsis:
//      1. Flush the D2D device context via EndDraw().
//      2. Present the swap chain with dirty rects when available.
//      3. Re-acquire the back buffer for the next frame (flip model).
//
// ---------------------------------------------------------------------------

STDMETHODIMP
CD2DDisplayRenderTarget::Present(
    __in_ecount(1) const RECT *pRect
    )
{
    HRESULT hr = S_OK;

    //
    // Breadcrumb: record that Present() was reached.
    //

    {
        static LONG s_presentCount = 0;
        LONG count = InterlockedIncrement(&s_presentCount);

        WCHAR breadcrumb[MAX_PATH];
        if (GetTempPathW(MAX_PATH, breadcrumb) > 0)
        {
            wcscat_s(breadcrumb, MAX_PATH, L"wpf_d2d_present.log");

            HANDLE hBC = CreateFileW(breadcrumb, GENERIC_WRITE, FILE_SHARE_READ,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hBC != INVALID_HANDLE_VALUE)
            {
                WCHAR msg[256];
                int msgLen = swprintf_s(msg, ARRAYSIZE(msg),
                    L"Present called %ld times. EnableRendering=%d SwapChain=%p D2DCtx=%p\r\n",
                    count, (int)m_fEnableRendering, (void*)m_pSwapChain.Get(), (void*)m_pD2DContext.Get());
                DWORD written;
                WriteFile(hBC, msg, msgLen * sizeof(WCHAR), &written, NULL);
                CloseHandle(hBC);
            }
        }
    }

    //
    // Don't present if rendering is disabled.
    //

    if (!m_fEnableRendering)
    {
        MIL_THR(m_hrDisplayInvalid);
        goto Cleanup;
    }

    if (!m_pSwapChain || !m_pD2DContext)
    {
        IFC(E_FAIL);
    }

    //
    // Diagnostic counters — write periodic log BEFORE any failable ops
    // so the first-frame log is captured even if EndDraw/Present1 fail.
    //

    WriteDiagLog();

    //
    // Flush all pending D2D drawing commands via the batched path.
    //

    HRESULT hrEndDraw = FlushDrawBatch();

    if (FAILED(hrEndDraw))
    {
        IFC(HandleDeviceLost(hrEndDraw));
        IFC(hrEndDraw);
    }

    //
    // Present with VSync.
    //
    // With FLIP_DISCARD the back buffer is not preserved, so dirty rects
    // are not meaningful and the rcLocalDeviceValidContentBounds.SetEmpty()
    // in desktoprt.cpp ensures a full repaint each frame.  Present with
    // SyncInterval=1 for clean VSync-aligned frames.  The tearing flag
    // is not used for windowed desktop presentation to avoid visual
    // artefacts — DWM composes at VSync anyway.
    //

    {
        DXGI_PRESENT_PARAMETERS presentParams = {};

        MIL_THR(m_pSwapChain->Present1(
            1,              // SyncInterval — wait for VSync
            0,              // Flags — no tearing
            &presentParams
            ));
    }

    if (FAILED(hr))
    {
        IFC(HandleDeviceLost(hr));
        goto Cleanup;
    }

    //
    // For flip-model swap chains the buffers rotate after Present, so we
    // must re-acquire the back buffer and rebuild the target bitmap.
    //

    IFC(AcquireBackBufferAsBitmap());

Cleanup:
    //
    // Always clear the dirty rect list after present, even on failure.
    //

    m_cDirtyRects = 0;

    if (FAILED(hr))
    {
        if (hr == WGXERR_DISPLAYSTATEINVALID ||
            hr == DXGI_ERROR_DEVICE_REMOVED  ||
            hr == DXGI_ERROR_DEVICE_RESET)
        {
            m_hrDisplayInvalid = WGXERR_DISPLAYSTATEINVALID;
        }

        m_fEnableRendering = FALSE;
    }

    RRETURN1(hr, S_PRESENT_OCCLUDED);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::ScrollBlt
//
//  Synopsis:
//      Present with a scroll rect and offset.  IDXGISwapChain1::Present1
//      supports a scroll rectangle that the compositor can use to optimise
//      the blit.  When scroll cannot be expressed that way we fall back to
//      a D3D11 CopySubresourceRegion.
//
// ---------------------------------------------------------------------------

STDMETHODIMP
CD2DDisplayRenderTarget::ScrollBlt(
    __in_ecount(1) const RECT *prcSource,
    __in_ecount(1) const RECT *prcDest
    )
{
    HRESULT hr = S_OK;

    if (!m_fEnableRendering || !m_pSwapChain)
    {
        IFC(E_FAIL);
    }

    //
    // DXGI scroll support:  The scroll rect is the destination rectangle
    // and the offset is (Source.left - Dest.left, Source.top - Dest.top).
    //

    {
        DXGI_PRESENT_PARAMETERS presentParams = {};
        RECT  scrollRect = *prcDest;
        POINT scrollOffset;

        scrollOffset.x = prcSource->left - prcDest->left;
        scrollOffset.y = prcSource->top  - prcDest->top;

        presentParams.DirtyRectsCount = 0;
        presentParams.pDirtyRects     = NULL;
        presentParams.pScrollRect     = &scrollRect;
        presentParams.pScrollOffset   = &scrollOffset;

        MIL_THR(m_pSwapChain->Present1(
            1,      // SyncInterval
            0,      // Flags
            &presentParams
            ));

        if (FAILED(hr))
        {
            IFC(HandleDeviceLost(hr));
        }
    }

    //
    // Re-acquire the back buffer after present (flip model).
    //

    IFC(AcquireBackBufferAsBitmap());

Cleanup:
    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::InvalidateRect
//
//  Synopsis:
//      Add a rectangle to the dirty region tracked for the next Present.
//      If the dirty list overflows, the entire surface is presented.
//
// ---------------------------------------------------------------------------

STDMETHODIMP
CD2DDisplayRenderTarget::InvalidateRect(
    __in_ecount(1) CMILSurfaceRect const *prc
    )
{
    HRESULT hr = S_OK;

    if (!m_fEnableRendering)
    {
        goto Cleanup;
    }

    //
    // If we have reached the maximum number of dirty rects, reset the
    // list to zero which signals Present() to present the full frame.
    //

    if (m_cDirtyRects >= MAX_DIRTY_RECTS)
    {
        //
        // Setting the count to 0 tells Present() that we don't have a
        // usable dirty list — it will present the entire frame.
        //

        m_cDirtyRects = 0;
        goto Cleanup;
    }

    {
        RECT &rcDirty = m_rgDirtyRects[m_cDirtyRects];

        rcDirty.left   = prc->left;
        rcDirty.top    = prc->top;
        rcDirty.right  = prc->right;
        rcDirty.bottom = prc->bottom;

        m_cDirtyRects++;
    }

Cleanup:
    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::ClearInvalidatedRects
//
//  Synopsis:
//      Reset the dirty rect list.
//
// ---------------------------------------------------------------------------

STDMETHODIMP
CD2DDisplayRenderTarget::ClearInvalidatedRects()
{
    m_cDirtyRects = 0;
    return S_OK;
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::Resize
//
//  Synopsis:
//      1. Release the current D2D bitmap target.
//      2. ResizeBuffers on the swap chain.
//      3. Re-create the target bitmap from the new back buffer.
//
// ---------------------------------------------------------------------------

STDMETHODIMP
CD2DDisplayRenderTarget::Resize(
    UINT width,
    UINT height
    )
{
    HRESULT hr = S_OK;

    //
    // A zero-size resize (e.g. window minimized) disables rendering
    // until the next non-zero Resize.
    //

    if (width == 0 || height == 0)
    {
        m_fEnableRendering = FALSE;
        goto Cleanup;
    }

    //
    // If we have no swap chain yet (first non-zero resize after a
    // minimized-start), create one from scratch.
    //

    if (!m_pSwapChain)
    {
        IFC(CreateSwapChainAndTarget(width, height));
        goto Cleanup;
    }

    //
    // Release D2D's reference to the back buffer before resizing.
    //

    if (m_pD2DContext)
    {
        m_pD2DContext->SetTarget(nullptr);
    }
    m_pTargetBitmap.Reset();

    //
    // Resize the swap chain buffers.  Passing 0 for format and flags
    // preserves the existing settings.
    //

    {
        UINT swapFlags = m_fAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        MIL_THR(m_pSwapChain->ResizeBuffers(
            0,                          // BufferCount — keep existing
            width,
            height,
            DXGI_FORMAT_UNKNOWN,        // preserve existing format
            swapFlags                   // preserve ALLOW_TEARING
            ));
    }

    if (FAILED(hr))
    {
        IFC(HandleDeviceLost(hr));
        goto Cleanup;
    }

    //
    // Re-acquire the resized back buffer.
    //

    IFC(AcquireBackBufferAsBitmap());

    m_uWidth  = width;
    m_uHeight = height;

    m_fEnableRendering = TRUE;
    m_hrDisplayInvalid = S_OK;

    //
    // Clear the dirty list — the entire surface must be redrawn.
    //

    m_cDirtyRects = 0;

Cleanup:
    if (FAILED(hr))
    {
        if (hr == WGXERR_DISPLAYSTATEINVALID ||
            hr == DXGI_ERROR_DEVICE_REMOVED  ||
            hr == DXGI_ERROR_DEVICE_RESET)
        {
            m_hrDisplayInvalid = WGXERR_DISPLAYSTATEINVALID;
        }

        m_fEnableRendering = FALSE;
    }

    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::ResizeSwapChain
//
//  Synopsis:
//      Internal helper — thin wrapper around Resize for subclass use.
//
// ---------------------------------------------------------------------------

HRESULT
CD2DDisplayRenderTarget::ResizeSwapChain(
    UINT width,
    UINT height
    )
{
    return Resize(width, height);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::AdvanceFrame
//
//  Synopsis:
//      Record frame number and release per-frame temporaries if needed.
//
// ---------------------------------------------------------------------------

STDMETHODIMP_(VOID)
CD2DDisplayRenderTarget::AdvanceFrame(
    UINT uFrameNumber
    )
{
    m_uFrameNumber = uFrameNumber;

    //
    // Future: release per-frame temporary resources here if we
    // introduce any (e.g. intermediate render target caches).
    //
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::WaitForVBlank
//
//  Synopsis:
//      Calls IDXGIOutput::WaitForVBlank() on the output associated with
//      the swap chain.
//
// ---------------------------------------------------------------------------

STDMETHODIMP
CD2DDisplayRenderTarget::WaitForVBlank()
{
    BEGIN_MILINSTRUMENTATION_HRESULT_LIST_WITH_DEFAULTS
        WGXERR_NO_HARDWARE_DEVICE
    END_MILINSTRUMENTATION_HRESULT_LIST

    HRESULT hr = WGXERR_NO_HARDWARE_DEVICE;

    if (SUCCEEDED(m_hrDisplayInvalid) && m_pSwapChain)
    {
        ComPtr<IDXGIOutput> pOutput;

        MIL_THR(m_pSwapChain->GetContainingOutput(&pOutput));

        if (SUCCEEDED(hr) && pOutput)
        {
            MIL_THR(pOutput->WaitForVBlank());
        }
    }

    RRETURN(hr);
}


// ---------------------------------------------------------------------------
//
//  CD2DDisplayRenderTarget::IsValid
//
//  Synopsis:
//      Returns FALSE when the display is invalid or rendering is disabled.
//
// ---------------------------------------------------------------------------

bool
CD2DDisplayRenderTarget::IsValid() const
{
    if (!m_fEnableRendering)
    {
        return false;
    }

    if (FAILED(m_hrDisplayInvalid))
    {
        return false;
    }

    //
    // Check whether the underlying DXGI device has been removed.
    //

    if (m_pSwapChain)
    {
        ComPtr<IDXGIDevice> pDXGIDevice;
        HRESULT hrDevice = S_OK;

        ID3D11Device *pD3D11Device = GetD2DDeviceNoRef()->GetD3D11Device();
        if (pD3D11Device != nullptr)
        {
            hrDevice = pD3D11Device->GetDeviceRemovedReason();
        }

        if (FAILED(hrDevice))
        {
            return false;
        }
    }

    return true;
}


