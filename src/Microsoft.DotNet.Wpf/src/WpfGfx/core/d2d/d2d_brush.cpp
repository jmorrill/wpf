// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------
//
//  Description:
//      CD2DBrushConverter implementation.
//
//      Converts WPF brush representations to Direct2D ID2D1Brush objects.
//      The main entry point ConvertBrush() attempts to extract the
//      realized CMILBrush from a CBrushRealizer and create the
//      appropriate D2D brush type.  When full type inspection is not
//      yet available (pending integration with internal WPF headers),
//      a compilation-safe fallback path creates a diagnostic magenta
//      brush so that missing conversion paths are visually obvious.
//
//------------------------------------------------------------------------

#include <precomp.hpp>

// ---------------------------------------------------------------------------
//  ConvertBrush
//
//  Converts a WPF CBrushRealizer to a D2D brush.
//
//  CBrushRealizer holds a realized CMILBrush* accessible via
//  GetRealizedBrushNoRef().  The CMILBrush subclass hierarchy includes:
//    - CMILBrushSolid        (solid color)
//    - CMILBrushBitmap       (image/tile brush)
//    - CMILBrushGradient     (linear/radial gradient)
//
//  For the initial D2D back-end bring-up we:
//    1. Handle the NULL-brush case (transparent).
//    2. Attempt to read the solid color from the realizer's
//       GetOpacityFromRealizedBrush() for opacity.
//    3. As a compilation-safe fallback, emit a diagnostic magenta brush.
//
//  TODO: Once full internal header integration is complete, inspect
//        CMILBrush subclass type and create the matching D2D brush
//        (solid / linear gradient / radial gradient / bitmap).
// ---------------------------------------------------------------------------

HRESULT CD2DBrushConverter::ConvertBrush(
    __in ID2D1DeviceContext1 *pContext,
    __in ID2D1Factory1 *pFactory,
    __in_ecount_opt(1) CBrushRealizer *pBrushRealizer,
    __in_ecount_opt(1) CContextState *pContextState,
    __in_ecount_opt(1) BrushContext *pBrushContext,
    __deref_out ID2D1Brush **ppBrush
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(ppBrush);

    *ppBrush = NULL;

    //
    // Case 1: NULL brush realizer => transparent brush
    //
    if (!pBrushRealizer)
    {
        ComPtr<ID2D1SolidColorBrush> spTransparent;
        IFC(pContext->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
            &spTransparent
        ));
        *ppBrush = spTransparent.Detach();
        goto Cleanup;
    }

    //
    // Case 2: Attempt to get the realised brush and inspect type.
    //
    {
        CMILBrush *pMILBrush = pBrushRealizer->GetRealizedBrushNoRef(
            false /* fConvertNULLToTransparent */
        );

        if (!pMILBrush)
        {
            // Realizer produced nothing — treat as transparent.
            ComPtr<ID2D1SolidColorBrush> spTransparent;
            IFC(pContext->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                &spTransparent
            ));
            *ppBrush = spTransparent.Detach();
            goto Cleanup;
        }

        //
        // TODO: Full CMILBrush subclass type detection.
        //
        // When the requisite internal headers are available, inspect pMILBrush
        // using dynamic_cast or type-flag methods to determine whether it is:
        //   - CMILBrushSolid   => CreateSolidBrush()
        //   - CMILBrushBitmap  => CreateBitmapBrush()
        //   - CMILBrushLinearGradient => CreateLinearGradientBrush()
        //   - CMILBrushRadialGradient => CreateRadialGradientBrush()
        //
        // For now, extract opacity and create a diagnostic fallback brush.
        //

        FLOAT opacity = pBrushRealizer->GetOpacityFromRealizedBrush();

        //
        // Diagnostic fallback: bright magenta so missing conversion paths
        // are immediately visible during development.
        //
        ComPtr<ID2D1SolidColorBrush> spFallback;
        IFC(pContext->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 0.0f, 1.0f, opacity),
            &spFallback
        ));
        *ppBrush = spFallback.Detach();
    }

Cleanup:
    RRETURN(hr);
}

// ---------------------------------------------------------------------------
//  CreateSolidBrush
// ---------------------------------------------------------------------------

HRESULT CD2DBrushConverter::CreateSolidBrush(
    __in ID2D1DeviceContext1 *pContext,
    __in const MilColorF &color,
    FLOAT opacity,
    __deref_out ID2D1SolidColorBrush **ppBrush
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(ppBrush);

    *ppBrush = NULL;

    D2D1_COLOR_F d2dColor = CD2DGeometryConverter::ToD2DColor(color);

    //
    // Pre-multiply opacity into the alpha channel so the brush carries
    // the correct effective opacity.
    //
    d2dColor.a *= opacity;

    IFC(pContext->CreateSolidColorBrush(d2dColor, ppBrush));

Cleanup:
    RRETURN(hr);
}

// ---------------------------------------------------------------------------
//  CreateLinearGradientBrush
// ---------------------------------------------------------------------------

HRESULT CD2DBrushConverter::CreateLinearGradientBrush(
    __in ID2D1DeviceContext1 *pContext,
    __in const D2D1_POINT_2F &startPt,
    __in const D2D1_POINT_2F &endPt,
    __in_ecount(cStops) const D2D1_GRADIENT_STOP *pStops,
    UINT cStops,
    FLOAT opacity,
    __deref_out ID2D1LinearGradientBrush **ppBrush
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(pStops || cStops == 0);
    Assert(ppBrush);

    *ppBrush = NULL;

    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgbProps = {};
    D2D1_BRUSH_PROPERTIES brushProps = {};

    //
    // Create gradient stop collection
    //
    ComPtr<ID2D1GradientStopCollection> spStopCollection;
    IFC(pContext->CreateGradientStopCollection(
        pStops,
        cStops,
        D2D1_GAMMA_2_2,
        D2D1_EXTEND_MODE_CLAMP,
        &spStopCollection
    ));

    //
    // Create the linear gradient brush
    //
    lgbProps.startPoint = startPt;
    lgbProps.endPoint   = endPt;

    brushProps = D2D1::BrushProperties(opacity);

    IFC(pContext->CreateLinearGradientBrush(
        &lgbProps,
        &brushProps,
        spStopCollection.Get(),
        ppBrush
    ));

Cleanup:
    RRETURN(hr);
}

// ---------------------------------------------------------------------------
//  CreateRadialGradientBrush
// ---------------------------------------------------------------------------

HRESULT CD2DBrushConverter::CreateRadialGradientBrush(
    __in ID2D1DeviceContext1 *pContext,
    __in const D2D1_POINT_2F &center,
    __in const D2D1_POINT_2F &offset,
    FLOAT radiusX, FLOAT radiusY,
    __in_ecount(cStops) const D2D1_GRADIENT_STOP *pStops,
    UINT cStops,
    FLOAT opacity,
    __deref_out ID2D1RadialGradientBrush **ppBrush
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(pStops || cStops == 0);
    Assert(ppBrush);

    *ppBrush = NULL;

    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rgbProps = {};
    D2D1_BRUSH_PROPERTIES brushProps = {};

    //
    // Create gradient stop collection
    //
    ComPtr<ID2D1GradientStopCollection> spStopCollection;
    IFC(pContext->CreateGradientStopCollection(
        pStops,
        cStops,
        D2D1_GAMMA_2_2,
        D2D1_EXTEND_MODE_CLAMP,
        &spStopCollection
    ));

    //
    // Create the radial gradient brush
    //
    rgbProps.center                = center;
    rgbProps.gradientOriginOffset  = offset;
    rgbProps.radiusX               = radiusX;
    rgbProps.radiusY               = radiusY;

    brushProps = D2D1::BrushProperties(opacity);

    IFC(pContext->CreateRadialGradientBrush(
        &rgbProps,
        &brushProps,
        spStopCollection.Get(),
        ppBrush
    ));

Cleanup:
    RRETURN(hr);
}

// ---------------------------------------------------------------------------
//  CreateBitmapBrush
//
//  Converts a WPF IWGXBitmapSource to a D2D1 bitmap and wraps it in a
//  D2D1 bitmap brush.
// ---------------------------------------------------------------------------

HRESULT CD2DBrushConverter::CreateBitmapBrush(
    __in ID2D1DeviceContext1 *pContext,
    __in IWGXBitmapSource *pBitmapSource,
    FLOAT opacity,
    __deref_out ID2D1BitmapBrush1 **ppBrush
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(pBitmapSource);
    Assert(ppBrush);

    *ppBrush = NULL;

    D2D1_BITMAP_BRUSH_PROPERTIES1 bbProps = {};
    D2D1_BRUSH_PROPERTIES brushProps = {};

    //
    // Convert the WPF bitmap source to a D2D1 bitmap.
    //
    ComPtr<ID2D1Bitmap1> spBitmap;
    IFC(CD2DBitmapConverter::ConvertBitmapSource(pContext, pBitmapSource, &spBitmap));

    //
    // Create the bitmap brush.
    //
    bbProps.extendModeX       = D2D1_EXTEND_MODE_CLAMP;
    bbProps.extendModeY       = D2D1_EXTEND_MODE_CLAMP;
    bbProps.interpolationMode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

    brushProps = D2D1::BrushProperties(opacity);

    IFC(pContext->CreateBitmapBrush(
        spBitmap.Get(),
        &bbProps,
        &brushProps,
        ppBrush
    ));

Cleanup:
    RRETURN(hr);
}
