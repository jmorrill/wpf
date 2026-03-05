// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2DSurfaceRenderTarget declaration.
//
//      This class is the D2D1 equivalent of CHwSurfaceRenderTarget, providing
//      rendering operations via an ID2D1DeviceContext1 instead of
//      IDirect3DDevice9.
//
//----------------------------------------------------------------------------

#pragma once

using Microsoft::WRL::ComPtr;

class CAliasedClip;
class CD2DResourceManager;

//
// D2D error handling macros – mirrors IFC pattern used in WpfGfx
//

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

#ifndef D2D_SAFE_RELEASE
#define D2D_SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

using Microsoft::WRL::ComPtr;

//+-----------------------------------------------------------------------------
//
//  Enum:       D2DRenderTargetType
//
//  Synopsis:   Return value for GetType() – must compose with
//              InternalRenderTargetType flags from InternalRT.h
//
//------------------------------------------------------------------------------
enum
{
    D2DRasterRenderTarget = 0x00001000
};

//+-----------------------------------------------------------------------------
//
//  Class:
//      CD2DSurfaceRenderTarget
//
//  Synopsis:
//      D2D1-based render target implementing IRenderTargetInternal.  Holds an
//      ID2D1DeviceContext1 and renders all WPF primitives through Direct2D
//      calls.  Provides 3D interop via the shared DXGI surface between D2D1
//      and D3D11.
//
//  Notes:
//      This is the base class for CD2DHwndRenderTarget (windowed) and
//      CD2DTextureRenderTarget (off-screen).  It parallels
//      CHwSurfaceRenderTarget, which uses IDirect3DDevice9.
//
//      Construction follows a two-phase pattern: the protected constructor
//      initialises members, then Init() creates D2D resources against a
//      caller-provided bitmap or surface.
//
//------------------------------------------------------------------------------

class CD2DSurfaceRenderTarget : public IRenderTargetInternal
{
protected:

    //+------------------------------------------------------------------------
    //
    //  Member:    CD2DSurfaceRenderTarget::CD2DSurfaceRenderTarget
    //
    //  Synopsis:  ctor – sets weak device reference and display association.
    //
    //-------------------------------------------------------------------------

    CD2DSurfaceRenderTarget(
        __inout_ecount(1) CD2DDevice *pDevice,
        DisplayId associatedDisplay
        );

    //+------------------------------------------------------------------------
    //
    //  Member:    CD2DSurfaceRenderTarget::~CD2DSurfaceRenderTarget
    //
    //  Synopsis:  dtor – releases D2D/D3D resources.
    //
    //-------------------------------------------------------------------------

    virtual ~CD2DSurfaceRenderTarget();

    //+------------------------------------------------------------------------
    //
    //  Member:    CD2DSurfaceRenderTarget::Init
    //
    //  Synopsis:  Second-phase init – binds the device context to the target
    //             bitmap with the given dimensions.
    //
    //-------------------------------------------------------------------------

    HRESULT Init(
        __in_ecount(1) ID2D1Bitmap1 *pTargetBitmap,
        UINT width,
        UINT height
        );

    //+------------------------------------------------------------------------
    //
    //  Member:    CD2DSurfaceRenderTarget::IsValid
    //
    //  Synopsis:  Returns FALSE when the render target is no longer usable
    //             (e.g., after a device lost or mode change).
    //
    //-------------------------------------------------------------------------

    virtual bool IsValid() const;

public:

    // -----------------------------------------------------------------------
    // IUnknown – simple ref counting
    // -----------------------------------------------------------------------

    STDMETHOD(QueryInterface)(
        __in REFIID riid,
        __deref_out void **ppv
        );

    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();

    // -----------------------------------------------------------------------
    // IMILRenderTarget
    // -----------------------------------------------------------------------

    STDMETHOD_(VOID, GetBounds)(
        __out_ecount(1) MilRectF * const pBounds
        );

    STDMETHOD(Clear)(
        __in_ecount_opt(1) const MilColorF *pColor,
        __in_ecount_opt(1) const CAliasedClip *pAliasedClip
        );

    STDMETHOD(Begin3D)(
        __in_ecount(1) MilRectF const &rcBounds,
        MilAntiAliasMode::Enum AntiAliasMode,
        bool fUseZBuffer,
        FLOAT rZ
        );

    STDMETHOD(End3D)();

    // -----------------------------------------------------------------------
    // IRenderTargetInternal
    // -----------------------------------------------------------------------

    STDMETHOD_(__outro_ecount(1) const CMILMatrix *, GetDeviceTransform)() const override;

    STDMETHOD(DrawBitmap)(
        __inout_ecount(1) CContextState *pContextState,
        __inout_ecount(1) IWGXBitmapSource *pIBitmap,
        __inout_ecount_opt(1) IMILEffectList *pIEffect
        ) override;

    STDMETHOD(DrawMesh3D)(
        __inout_ecount(1) CContextState *pContextState,
        __inout_ecount_opt(1) BrushContext *pBrushContext,
        __inout_ecount(1) CMILMesh3D *pMesh3D,
        __inout_ecount_opt(1) CMILShader *pShader,
        __inout_ecount_opt(1) IMILEffectList *pIEffect
        ) override;

    STDMETHOD(DrawPath)(
        __inout_ecount(1) CContextState *pContextState,
        __inout_ecount_opt(1) BrushContext *pBrushContext,
        __inout_ecount(1) IShapeData *pPath,
        __inout_ecount_opt(1) CPlainPen *pPen,
        __inout_ecount_opt(1) CBrushRealizer *pStrokeBrush,
        __inout_ecount_opt(1) CBrushRealizer *pFillBrush
        ) override;

    STDMETHOD(DrawInfinitePath)(
        __inout_ecount(1) CContextState *pContextState,
        __inout_ecount(1) BrushContext *pBrushContext,
        __inout_ecount(1) CBrushRealizer *pFillBrush
        ) override;

    STDMETHOD(ComposeEffect)(
        __inout_ecount(1) CContextState *pContextState,
        __in_ecount(1) CMILMatrix *pScaleTransform,
        __inout_ecount(1) CMilEffectDuce *pEffect,
        UINT uIntermediateWidth,
        UINT uIntermediateHeight,
        __in_opt IMILRenderTargetBitmap *pImplicitInput
        ) override;

    STDMETHOD(DrawGlyphs)(
        __inout_ecount(1) DrawGlyphsParameters &pars
        ) override;

    STDMETHOD(DrawVideo)(
        __inout_ecount(1) CContextState *pContextState,
        __inout_ecount_opt(1) IAVSurfaceRenderer *pSurfaceRenderer,
        __inout_ecount_opt(1) IWGXBitmapSource *pIBitmapSource,
        __inout_ecount_opt(1) IMILEffectList *pIEffect
        ) override;

    STDMETHOD(BeginLayer)(
        __in_ecount(1) MilRectF const &LayerBounds,
        MilAntiAliasMode::Enum AntiAliasMode,
        __in_ecount_opt(1) IShapeData const *pGeometricMask,
        __in_ecount_opt(1) CMILMatrix const *pGeometricMaskToTarget,
        FLOAT flAlphaScale,
        __in_ecount_opt(1) CBrushRealizer *pAlphaMask
        ) override;

    STDMETHOD(EndLayer)() override;

    STDMETHOD_(void, EndAndIgnoreAllLayers)() override;

    STDMETHOD(GetType)(
        __out DWORD *pRenderTargetType
        ) override;

    STDMETHOD(SetClearTypeHint)(
        __in bool forceClearType
        ) override;

    UINT GetRealizationCacheIndex() override;

    STDMETHOD(GetNumQueuedPresents)(
        __out_ecount(1) UINT *puNumQueuedPresents
        ) override;

    // -----------------------------------------------------------------------
    // CIntermediateRTCreator
    // -----------------------------------------------------------------------

    STDMETHOD(CreateRenderTargetBitmap)(
        UINT width,
        UINT height,
        IntermediateRTUsage usageInfo,
        MilRTInitialization::Flags dwFlags,
        __deref_out_ecount(1) IMILRenderTargetBitmap **ppIRenderTargetBitmap,
        __in_opt DynArray<bool> const *pActiveDisplays = NULL
        ) override;

    STDMETHOD(ReadEnabledDisplays)(
        __inout DynArray<bool> *pEnabledDisplays
        ) override;

protected:

    // -----------------------------------------------------------------------
    // D2D1 conversion helpers
    // -----------------------------------------------------------------------

    //+------------------------------------------------------------------------
    //
    //  Member:    EnsureD2DResources
    //
    //  Synopsis:  Lazily create / re-create D2D device-context resources when
    //             needed (e.g. after a device-lost).
    //
    //-------------------------------------------------------------------------

    HRESULT EnsureD2DResources();

    //+------------------------------------------------------------------------
    //
    //  Member:    ConvertBrush
    //
    //  Synopsis:  Translate a WPF CBrushRealizer to an ID2D1Brush.  Phase 1
    //             returns a solid-color fallback; full brush conversion is
    //             deferred to Phase 2.
    //
    //-------------------------------------------------------------------------

    HRESULT ConvertBrush(
        __inout_ecount_opt(1) CBrushRealizer *pBrushRealizer,
        __inout_ecount_opt(1) CContextState *pContextState,
        __deref_out_ecount(1) ID2D1Brush **ppBrush
        );

    //+------------------------------------------------------------------------
    //
    //  Member:    EnsureBrushRealization
    //
    //  Synopsis:  Call EnsureRealization on a brush realizer so that
    //             GetRealizedBrushNoRef returns a valid brush.
    //
    //-------------------------------------------------------------------------

    HRESULT EnsureBrushRealization(
        __inout_ecount_opt(1) CBrushRealizer *pBrushRealizer,
        __inout_ecount_opt(1) BrushContext *pBrushContext,
        __in_ecount(1) const CContextState *pContextState
        );

    //+------------------------------------------------------------------------
    //
    //  Member:    ConvertGeometry
    //
    //  Synopsis:  Build an ID2D1PathGeometry from an IShapeData by iterating
    //             figures and segments.
    //
    //-------------------------------------------------------------------------

    HRESULT ConvertGeometry(
        __in_ecount(1) const IShapeData *pShape,
        __deref_out_ecount(1) ID2D1Geometry **ppGeometry
        );

    //+------------------------------------------------------------------------
    //
    //  Member:    ConvertPenToStrokeStyle
    //
    //  Synopsis:  Map a WPF CPlainPen into an ID2D1StrokeStyle plus a stroke
    //             width value.
    //
    //-------------------------------------------------------------------------

    HRESULT ConvertPenToStrokeStyle(
        __in_ecount(1) const CPlainPen *pPen,
        __deref_out_ecount(1) ID2D1StrokeStyle **ppStyle,
        __out_ecount(1) FLOAT *pStrokeWidth
        );

    //+------------------------------------------------------------------------
    //
    //  Member:    ConvertFigureToSink
    //
    //  Synopsis:  Walk a single IFigureData and emit its segments into an
    //             already-opened ID2D1GeometrySink.
    //
    //-------------------------------------------------------------------------

    HRESULT ConvertFigureToSink(
        __in_ecount(1) const IFigureData &figure,
        __inout_ecount(1) ID2D1GeometrySink *pSink
        );

protected:

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    //
    // Device & context
    //

    CD2DDevice *m_pDevice;                          // weak ref, owned by device manager

    CD2DDevice* GetD2DDeviceNoRef() const { return m_pDevice; }
    ComPtr<ID2D1DeviceContext1> m_pD2DContext;
    ComPtr<ID2D1Bitmap1> m_pTargetBitmap;

    //
    // Surface metrics
    //

    UINT m_uWidth;
    UINT m_uHeight;
    MilPixelFormat::Enum m_fmtTarget;
    DisplayId m_associatedDisplay;
    CMILMatrix m_deviceTransform;

    //
    // Ref counting & state
    //

    LONG m_cRef;
    bool m_fIn3D;
    bool m_forceClearType;
    UINT m_layerCount;

    // -----------------------------------------------------------------------
    // Draw-session batching — avoids per-primitive BeginDraw/EndDraw.
    // BeginDrawBatch opens a D2D draw session if one isn't already active.
    // FlushDrawBatch calls EndDraw and resets the flag.
    // -----------------------------------------------------------------------

    bool m_fDrawBatchOpen;
    HRESULT BeginDrawBatch();
    HRESULT FlushDrawBatch();

    // -----------------------------------------------------------------------
    // Cached brushes — avoid per-draw-call reallocations.
    // -----------------------------------------------------------------------

    ComPtr<ID2D1SolidColorBrush> m_pCachedSolidBrush;
    ComPtr<ID2D1SolidColorBrush> m_pTransparentBrush;

    HRESULT GetSolidBrush(
        D2D1_COLOR_F color,
        __deref_out ID2D1SolidColorBrush **ppBrush
        );

    // -----------------------------------------------------------------------
    // Cached bitmap for DrawBitmap — reused when dimensions match.
    // -----------------------------------------------------------------------

    ComPtr<ID2D1Bitmap1> m_pCachedDrawBitmap;
    UINT m_cachedBitmapWidth;
    UINT m_cachedBitmapHeight;

    // -----------------------------------------------------------------------
    // Bitmap source cache — avoids per-frame CopyPixels + CreateBitmap for
    // static bitmaps (WPF Image elements, ImageBrush, etc.).
    // Direct-mapped by IWGXBitmapSource pointer hash (256 entries).
    // -----------------------------------------------------------------------

    struct BitmapCacheEntry
    {
        const void *pSourceKey = nullptr;   // IWGXBitmapSource pointer
        UINT        width      = 0;
        UINT        height     = 0;
        ComPtr<ID2D1Bitmap1> pBitmap;
    };

    static const UINT BITMAP_CACHE_SIZE = 256;
    BitmapCacheEntry m_bitmapCache[BITMAP_CACHE_SIZE];

    HRESULT GetOrCreateCachedBitmap(
        __in IWGXBitmapSource *pSource,
        __deref_out ID2D1Bitmap1 **ppBitmap
    );

    // -----------------------------------------------------------------------
    // Cached stroke style for ConvertPenToStrokeStyle.
    // -----------------------------------------------------------------------

    ComPtr<ID2D1StrokeStyle> m_pCachedStrokeStyle;
    D2D1_STROKE_STYLE_PROPERTIES m_cachedStrokeProps;

    // -----------------------------------------------------------------------
    // Geometry cache — avoids per-frame ID2D1PathGeometry reconstruction.
    // Direct-mapped by IShapeData pointer hash (512 entries).
    // -----------------------------------------------------------------------

    struct GeometryCacheEntry
    {
        const void* pShapeKey = nullptr;
        UINT        figureCount = 0;
        ComPtr<ID2D1Geometry> pGeometry;
        ComPtr<ID2D1GeometryRealization> pFillRealization;

        //
        // Cached shape classification — avoids per-frame segment iteration.
        // shapeType: 0=unknown, 1=ellipse, 2=general
        //
        BYTE shapeType = 0;     // 0=not classified, 1=ellipse, 2=other
        D2D1_ELLIPSE cachedEllipse;
    };

    static const UINT GEOMETRY_CACHE_SIZE = 512;
    GeometryCacheEntry m_geometryCache[GEOMETRY_CACHE_SIZE];

    // -----------------------------------------------------------------------
    // Cached D2D factory — avoids per-draw GetFactory() COM round-trips.
    // -----------------------------------------------------------------------

    ComPtr<ID2D1Factory> m_pD2DFactory;

    // -----------------------------------------------------------------------
    // Resource manager — shared bitmap/geometry/intermediate bitmap caches.
    // -----------------------------------------------------------------------

    CD2DResourceManager *m_pResourceManager;

    // -----------------------------------------------------------------------
    // Cached 3D depth-stencil — reused across Begin3D calls on the same RT.
    // -----------------------------------------------------------------------

    UINT m_3DDepthStencilWidth;
    UINT m_3DDepthStencilHeight;

    //
    // Layer stack for BeginLayer / EndLayer
    // m_layerTypeStack tracks what was pushed at each depth:
    //   false = PushAxisAlignedClip (fast path)
    //   true  = PushLayer (full layer)
    //

    static const UINT MAX_LAYER_DEPTH = 64;
    ComPtr<ID2D1Layer> m_layerStack[MAX_LAYER_DEPTH];
    bool m_layerTypeStack[MAX_LAYER_DEPTH];    // true = PushLayer, false = PushAxisAlignedClip

    //
    // 3D interop – D3D11 resources created on demand for Begin3D / End3D
    //

    ComPtr<ID3D11Device> m_pD3D11Device;
    ComPtr<ID3D11DeviceContext> m_pD3D11Context;
    ComPtr<ID3D11RenderTargetView> m_p3DRenderTargetView;
    ComPtr<ID3D11DepthStencilView> m_p3DDepthStencilView;
    ComPtr<ID3D11Texture2D> m_p3DDepthStencil;
    MilRectF m_rcBounds3D;

    // -----------------------------------------------------------------------
    // Reusable staging buffer for bitmap pixel copies.
    // Grows as needed, never shrinks during the lifetime of the RT.
    // -----------------------------------------------------------------------

    BYTE  *m_pStagingBuffer;
    UINT   m_cbStagingBuffer;

    HRESULT EnsureStagingBuffer(UINT cbRequired, __deref_out BYTE **ppBuffer);

    // -----------------------------------------------------------------------
    // Per-frame diagnostic counters — written to log file on Present.
    // -----------------------------------------------------------------------

    struct D2DDiagCounters
    {
        UINT cDrawPath;
        UINT cFastRect;
        UINT cFastEllipse;
        UINT cFallbackGeometry;
        UINT cGeoCacheHit;
        UINT cGeoCacheMiss;
        UINT cBeginLayer;
        UINT cAxisAlignedClip;
        UINT cFullLayer;
        UINT cEndLayer;
        UINT cPresent;
        UINT cTotalFrames;
        UINT cEllipseCacheHit;
    };

    D2DDiagCounters m_diag;

    void ResetDiagCounters();
    void WriteDiagLog();
};
