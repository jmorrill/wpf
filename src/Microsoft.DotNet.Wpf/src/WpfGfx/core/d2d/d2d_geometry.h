// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------
//
//  Description:
//      CD2DGeometryConverter - Utility class that converts WPF IShapeData
//      geometry to Direct2D ID2D1Geometry objects, and provides type
//      conversion helpers between WPF internal types and D2D1 types.
//
//------------------------------------------------------------------------

#pragma once

class CD2DGeometryConverter
{
public:
    // Convert WPF shape data to D2D geometry
    static HRESULT ConvertShapeToGeometry(
        __in ID2D1Factory1 *pFactory,
        __in_ecount(1) IShapeData *pShape,
        __deref_out ID2D1Geometry **ppGeometry
    );

    // Convert a simple rectangle to D2D geometry
    static HRESULT CreateRectangleGeometry(
        __in ID2D1Factory1 *pFactory,
        __in const MilRectF &rect,
        __deref_out ID2D1RectangleGeometry **ppGeometry
    );

    // Convert a pen to D2D stroke style
    static HRESULT ConvertPenToStrokeStyle(
        __in ID2D1Factory1 *pFactory,
        __in_ecount_opt(1) CPlainPen *pPen,
        __out FLOAT *pStrokeWidth,
        __deref_out_opt ID2D1StrokeStyle **ppStrokeStyle
    );

    // Convert MilPoint2F array to D2D1_POINT_2F array (should be binary compatible)
    static D2D1_POINT_2F ToD2DPoint(const MilPoint2F &pt);
    static D2D1_RECT_F ToD2DRect(const MilRectF &rc);
    static D2D1_COLOR_F ToD2DColor(const MilColorF &color);
    static D2D1_MATRIX_3X2_F ToD2DMatrix(const CMILMatrix &mat);

private:
    CD2DGeometryConverter() = delete; // static only

    // Internal helper: add a single IFigureData to an open geometry sink
    static HRESULT AddFigureToSink(
        __in_ecount(1) const IFigureData &figure,
        __in ID2D1GeometrySink *pSink
    );
};
