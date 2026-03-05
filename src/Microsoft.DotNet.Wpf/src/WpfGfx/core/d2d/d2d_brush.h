// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------
//
//  Description:
//      CD2DBrushConverter - Utility class that converts WPF brush
//      representations (CBrushRealizer / CMILBrush) to Direct2D
//      ID2D1Brush objects.
//
//------------------------------------------------------------------------

#pragma once

class CD2DBrushConverter
{
public:
    // Convert a WPF brush to a D2D brush
    static HRESULT ConvertBrush(
        __in ID2D1DeviceContext1 *pContext,
        __in ID2D1Factory1 *pFactory,
        __in_ecount_opt(1) CBrushRealizer *pBrushRealizer,
        __in_ecount_opt(1) CContextState *pContextState,
        __in_ecount_opt(1) BrushContext *pBrushContext,
        __deref_out ID2D1Brush **ppBrush
    );

    // Create a solid color brush
    static HRESULT CreateSolidBrush(
        __in ID2D1DeviceContext1 *pContext,
        __in const MilColorF &color,
        FLOAT opacity,
        __deref_out ID2D1SolidColorBrush **ppBrush
    );

    // Create a linear gradient brush
    static HRESULT CreateLinearGradientBrush(
        __in ID2D1DeviceContext1 *pContext,
        __in const D2D1_POINT_2F &startPt,
        __in const D2D1_POINT_2F &endPt,
        __in_ecount(cStops) const D2D1_GRADIENT_STOP *pStops,
        UINT cStops,
        FLOAT opacity,
        __deref_out ID2D1LinearGradientBrush **ppBrush
    );

    // Create a radial gradient brush
    static HRESULT CreateRadialGradientBrush(
        __in ID2D1DeviceContext1 *pContext,
        __in const D2D1_POINT_2F &center,
        __in const D2D1_POINT_2F &offset,
        FLOAT radiusX, FLOAT radiusY,
        __in_ecount(cStops) const D2D1_GRADIENT_STOP *pStops,
        UINT cStops,
        FLOAT opacity,
        __deref_out ID2D1RadialGradientBrush **ppBrush
    );

    // Create a bitmap brush from WPF bitmap source
    static HRESULT CreateBitmapBrush(
        __in ID2D1DeviceContext1 *pContext,
        __in IWGXBitmapSource *pBitmapSource,
        FLOAT opacity,
        __deref_out ID2D1BitmapBrush1 **ppBrush
    );

private:
    CD2DBrushConverter() = delete;
};
