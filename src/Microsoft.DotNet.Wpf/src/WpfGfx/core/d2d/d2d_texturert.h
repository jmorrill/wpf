// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2DTextureRenderTarget — off-screen D2D1 render target backed by an
//      ID2D1Bitmap1 with TARGET option.  Used as intermediate render targets
//      for opacity layers, effects, and other compositing operations.
//
//      Also declares CD2DBitmapSource, a lightweight IWGXBitmapSource wrapper
//      around an ID2D1Bitmap1 that supports GPU-side fast-path detection.
//
//----------------------------------------------------------------------------

#pragma once

using Microsoft::WRL::ComPtr;

// {7B3A1F2E-D4C5-4A8B-9E6F-1C2D3E4F5A6B}
// Use EXTERN_C + selectany so the GUID can be referenced from any translation
// unit without linker conflicts.
EXTERN_C __declspec(selectany) const GUID IID_ID2DBitmapSourceInternal =
    { 0x7b3a1f2e, 0xd4c5, 0x4a8b, { 0x9e, 0x6f, 0x1c, 0x2d, 0x3e, 0x4f, 0x5a, 0x6b } };

//+-----------------------------------------------------------------------------
//
//  Interface:  ID2DBitmapSourceInternal
//
//  Synopsis:   Private interface for detecting D2D-backed bitmap sources.
//              When DrawBitmap receives an IWGXBitmapSource, it QI's for this
//              to check if the bits are already on the GPU as an ID2D1Bitmap.
//
//------------------------------------------------------------------------------

DECLARE_INTERFACE_(ID2DBitmapSourceInternal, IUnknown)
{
    STDMETHOD(GetD2DBitmap)(
        __deref_out ID2D1Bitmap1 **ppBitmap
        ) PURE;
};


//+-----------------------------------------------------------------------------
//
//  Class:      CD2DBitmapSource
//
//  Synopsis:   IWGXBitmapSource + ID2DBitmapSourceInternal wrapper around an
//              ID2D1Bitmap1.  Returned by CD2DTextureRenderTarget::GetBitmapSource.
//
//              When the parent CD2DSurfaceRenderTarget::DrawBitmap receives
//              this, it detects the internal interface and uses the D2D bitmap
//              directly (zero-copy GPU compositing).  If a non-D2D consumer
//              reads from this, CopyPixels does a GPU readback via a staging
//              bitmap.
//
//------------------------------------------------------------------------------

class CD2DBitmapSource :
    public IWGXBitmapSource,
    public ID2DBitmapSourceInternal
{
public:

    CD2DBitmapSource(
        __in ID2D1Bitmap1 *pBitmap,
        __in ID2D1DeviceContext1 *pContext,
        UINT width,
        UINT height
        );

    virtual ~CD2DBitmapSource();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();

    // IWGXBitmapSource
    STDMETHOD(GetSize)(UINT *puWidth, UINT *puHeight);
    STDMETHOD(GetPixelFormat)(MilPixelFormat::Enum *pPixelFormat);
    STDMETHOD(GetResolution)(double *pDpiX, double *pDpiY);
    STDMETHOD(CopyPalette)(IWICPalette *pIPalette);
    STDMETHOD(CopyPixels)(const MILRect *prc, UINT cbStride, UINT cbBufferSize, BYTE *pvPixels);

    // ID2DBitmapSourceInternal
    STDMETHOD(GetD2DBitmap)(__deref_out ID2D1Bitmap1 **ppBitmap);

private:

    LONG m_cRef;
    UINT m_uWidth;
    UINT m_uHeight;
    ComPtr<ID2D1Bitmap1> m_pBitmap;
    ComPtr<ID2D1DeviceContext1> m_pContext;
};


//+-----------------------------------------------------------------------------
//
//  Class:      CD2DTextureRenderTarget
//
//  Synopsis:   Off-screen render target backed by an ID2D1Bitmap1.
//              Inherits all drawing operations from CD2DSurfaceRenderTarget
//              and implements IMILRenderTargetBitmap so that
//              WPF's layer / intermediate system can use it.
//
//------------------------------------------------------------------------------

class CD2DTextureRenderTarget :
    public CD2DSurfaceRenderTarget,
    public IMILRenderTargetBitmap
{
public:

    static HRESULT Create(
        __in CD2DDevice *pDevice,
        DisplayId associatedDisplay,
        UINT width,
        UINT height,
        __deref_out CD2DTextureRenderTarget **ppRT
        );

    // IUnknown — must override because of dual inheritance
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // -----------------------------------------------------------------------
    // IMILRenderTarget — explicit overrides to resolve diamond inheritance.
    // IMILRenderTargetBitmap inherits IMILRenderTarget, and
    // CD2DSurfaceRenderTarget inherits IMILRenderTarget via
    // IRenderTargetInternal.  The compiler sees two IMILRenderTarget bases,
    // so we must explicitly forward to the CD2DSurfaceRenderTarget impls.
    // -----------------------------------------------------------------------

    STDMETHOD_(VOID, GetBounds)(
        __out_ecount(1) MilRectF * const pBounds
        ) override
    {
        CD2DSurfaceRenderTarget::GetBounds(pBounds);
    }

    STDMETHOD(Clear)(
        __in_ecount_opt(1) const MilColorF *pColor,
        __in_ecount_opt(1) const CAliasedClip *pAliasedClip
        ) override
    {
        return CD2DSurfaceRenderTarget::Clear(pColor, pAliasedClip);
    }

    STDMETHOD(Begin3D)(
        __in_ecount(1) MilRectF const &rcBounds,
        MilAntiAliasMode::Enum AntiAliasMode,
        bool fUseZBuffer,
        FLOAT rZ
        ) override
    {
        return CD2DSurfaceRenderTarget::Begin3D(rcBounds, AntiAliasMode, fUseZBuffer, rZ);
    }

    STDMETHOD(End3D)() override
    {
        return CD2DSurfaceRenderTarget::End3D();
    }

    // IMILRenderTargetBitmap
    STDMETHOD(GetBitmapSource)(
        __deref_out_ecount(1) IWGXBitmapSource ** const ppIBitmapSource
        );

    STDMETHOD(GetCacheableBitmapSource)(
        __deref_out_ecount(1) IWGXBitmapSource ** const ppIBitmapSource
        );

    STDMETHOD(GetBitmap)(
        __deref_out_ecount(1) IWGXBitmap ** const ppIBitmap
        );

    STDMETHOD(GetNumQueuedPresents)(
        __out_ecount(1) UINT *puNumQueuedPresents
        );

private:

    CD2DTextureRenderTarget(
        __in CD2DDevice *pDevice,
        DisplayId associatedDisplay
        );

    virtual ~CD2DTextureRenderTarget();

    HRESULT Init(UINT width, UINT height);
};

