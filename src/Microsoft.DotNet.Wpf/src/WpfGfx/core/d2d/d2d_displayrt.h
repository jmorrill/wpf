// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+-----------------------------------------------------------------------------
//
//
//  Description:
//      Contains CD2DDisplayRenderTarget which extends CD2DSurfaceRenderTarget
//      and implements IRenderTargetHWNDInternal for presenting D2D content
//      to an HWND via a DXGI swap chain.
//
//      This is the Direct2D equivalent of CHwDisplayRenderTarget +
//      CHwHWNDRenderTarget, managing swap chain creation, dirty rect tracking,
//      Present, Resize, and VBlank synchronization.
//
//------------------------------------------------------------------------------

#pragma once

using Microsoft::WRL::ComPtr;

//+-----------------------------------------------------------------------------
//
//  Class:
//      CD2DDisplayRenderTarget
//
//  Synopsis:
//      Manages an IDXGISwapChain1 and an ID2D1DeviceContext targeting the
//      swap chain back buffer.  Provides dirty-rect tracking, presentation,
//      resize, scroll-blt, and VBlank support for HWND-based rendering.
//
//------------------------------------------------------------------------------

class CD2DDisplayRenderTarget :
    public CD2DSurfaceRenderTarget,
    public IRenderTargetHWNDInternal
{
public:

    static HRESULT Create(
        __in_opt HWND hwnd,
        MilWindowLayerType::Enum eWindowLayerType,
        __in_ecount(1) CDisplay const *pDisplay,
        MilRTInitialization::Flags dwFlags,
        __deref_out CD2DDisplayRenderTarget **ppRenderTarget
        );

    //
    // IUnknown override to handle dual-interface
    //

    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    //
    // IRenderTargetHWNDInternal
    //

    void SetPosition(POINT ptOrigin) override;

    void UpdatePresentProperties(
        MilTransparency::Flags transparencyFlags,
        BYTE constantAlpha,
        COLORREF colorKey
        ) override;

    STDMETHOD(Present)(
        __in_ecount(1) const RECT *pRect
        ) override;

    STDMETHOD(ScrollBlt)(
        __in_ecount(1) const RECT *prcSource,
        __in_ecount(1) const RECT *prcDest
        ) override;

    STDMETHOD(InvalidateRect)(
        __in_ecount(1) CMILSurfaceRect const *prc
        ) override;

    STDMETHOD(ClearInvalidatedRects)() override;

    STDMETHOD(Resize)(
        UINT width,
        UINT height
        ) override;

    STDMETHOD_(VOID, AdvanceFrame)(
        UINT uFrameNumber
        ) override;

    STDMETHOD(WaitForVBlank)() override;

    //
    // Override validity check
    //

    bool IsValid() const override;

protected:

    CD2DDisplayRenderTarget(
        __in_ecount(1) CD2DDevice *pDevice,
        DisplayId associatedDisplay
        );

    virtual ~CD2DDisplayRenderTarget();

    HRESULT Init(
        __in_opt HWND hwnd,
        MilWindowLayerType::Enum eWindowLayerType,
        __in_ecount(1) CDisplay const *pDisplay,
        MilRTInitialization::Flags dwFlags
        );

    HRESULT CreateSwapChainAndTarget(
        UINT width,
        UINT height
        );

    HRESULT ResizeSwapChain(
        UINT width,
        UINT height
        );

private:

    HRESULT AcquireBackBufferAsBitmap();
    HRESULT HandleDeviceLost(HRESULT hrLost);

    //
    // Tearing support — enables true SyncInterval=0 on FLIP chains.
    //

    BOOL                            m_fAllowTearing;

    //
    // HWND and window state
    //

    HWND                            m_hwnd;
    POINT                           m_ptOrigin;
    MilWindowLayerType::Enum        m_eWindowLayerType;

    //
    // DXGI swap chain
    //

    ComPtr<IDXGISwapChain1>         m_pSwapChain;

    //
    // NOTE: m_pD2DContext and m_pTargetBitmap are inherited from
    // CD2DSurfaceRenderTarget (protected).  Do NOT redeclare them here
    // or the base class draw methods will operate on null pointers.
    //

    //
    // Rendering state
    //

    BOOL                            m_fEnableRendering;
    HRESULT                         m_hrDisplayInvalid;
    MilRTInitialization::Flags      m_dwFlags;

    //
    // Transparency / layered-window properties
    //

    MilTransparency::Flags          m_transparencyFlags;
    BYTE                            m_constantAlpha;
    COLORREF                        m_colorKey;

    //
    // Frame tracking
    //

    UINT                            m_uFrameNumber;

    //
    // Dirty region tracking
    //

    static const UINT MAX_DIRTY_RECTS = 16;
    RECT                            m_rgDirtyRects[MAX_DIRTY_RECTS];
    UINT                            m_cDirtyRects;
};


