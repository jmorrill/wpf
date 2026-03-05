// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------
//
//  Description:
//      CD2DGeometryConverter implementation.
//
//      Converts WPF IShapeData (CShapeBase) geometry to Direct2D
//      ID2D1Geometry objects by walking figures and segments via
//      IFigureData's traversal interface.  Also provides pen-to-stroke-style
//      conversion and lightweight type-conversion helpers.
//
//------------------------------------------------------------------------

#include <precomp.hpp>

// ---------------------------------------------------------------------------
//  Type conversion helpers
//
//  MilPoint2F  {FLOAT X, Y}          ->  D2D1_POINT_2F  {FLOAT x, y}
//  MilRectF    {FLOAT left,top,right,bottom} -> D2D1_RECT_F  {same layout}
//  MilColorF   (== D3DCOLORVALUE) {FLOAT r,g,b,a} -> D2D1_COLOR_F {same layout}
//  CMILMatrix  (4x4 row-major)    -> D2D1_MATRIX_3X2_F (2D affine)
// ---------------------------------------------------------------------------

D2D1_POINT_2F CD2DGeometryConverter::ToD2DPoint(const MilPoint2F &pt)
{
    return D2D1::Point2F(pt.X, pt.Y);
}

D2D1_RECT_F CD2DGeometryConverter::ToD2DRect(const MilRectF &rc)
{
    return D2D1::RectF(rc.left, rc.top, rc.right, rc.bottom);
}

D2D1_COLOR_F CD2DGeometryConverter::ToD2DColor(const MilColorF &color)
{
    // MilColorF is typedef D3DCOLORVALUE { r, g, b, a }.
    // D2D1_COLOR_F has the identical memory layout.
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

D2D1_MATRIX_3X2_F CD2DGeometryConverter::ToD2DMatrix(const CMILMatrix &mat)
{
    //
    // CMILMatrix is a 4x4 row-major matrix inheriting from D3DMATRIX.
    // D2D uses a 3x2 matrix for 2D affine transforms:
    //
    //  D2D  [_11 _12]     WPF 4x4 row 0,1 cols 0,1 + row 3 cols 0,1
    //       [_21 _22]
    //       [_31 _32]     (translation)
    //
    // We can access the raw floats through the D3DMATRIX-compatible layout.
    // D3DMATRIX stores data as m[row][col], so:
    //   _11 = m[0][0], _12 = m[0][1]
    //   _21 = m[1][0], _22 = m[1][1]
    //   _31 = m[3][0], _32 = m[3][1]  (translation is in row 3)
    //

    const float *pf = reinterpret_cast<const float*>(&mat);
    D2D1_MATRIX_3X2_F result;
    result._11 = pf[0];   // m[0][0]
    result._12 = pf[1];   // m[0][1]
    result._21 = pf[4];   // m[1][0]
    result._22 = pf[5];   // m[1][1]
    result._31 = pf[12];  // m[3][0] - translation X
    result._32 = pf[13];  // m[3][1] - translation Y
    return result;
}

// ---------------------------------------------------------------------------
//  AddFigureToSink
//
//  Walks a single IFigureData via its traversal interface and writes the
//  figure into an open ID2D1GeometrySink.
//
//  IFigureData traversal:
//    - GetStartPoint() -> first point
//    - SetToFirstSegment()
//    - GetCurrentSegment(bType, pPt) -> returns last-seg flag
//      bType: MilCoreSeg::TypeLine  => pPt has 1 point  (line endpoint)
//      bType: MilCoreSeg::TypeBezier => pPt has 3 points (control1, control2, endpoint)
//    - SetToNextSegment()
// ---------------------------------------------------------------------------

HRESULT CD2DGeometryConverter::AddFigureToSink(
    __in_ecount(1) const IFigureData &figure,
    __in ID2D1GeometrySink *pSink
    )
{
    HRESULT hr = S_OK;

    if (figure.IsEmpty() || figure.HasNoSegments())
    {
        // Nothing to emit for an empty figure.
        goto Cleanup;
    }

    {
        // Determine fill mode for this figure
        D2D1_FIGURE_BEGIN figureBegin =
            figure.IsFillable()
                ? D2D1_FIGURE_BEGIN_FILLED
                : D2D1_FIGURE_BEGIN_HOLLOW;

        D2D1_FIGURE_END figureEnd =
            figure.IsClosed()
                ? D2D1_FIGURE_END_CLOSED
                : D2D1_FIGURE_END_OPEN;

        // Start the figure
        const MilPoint2F &startPt = figure.GetStartPoint();
        pSink->BeginFigure(ToD2DPoint(startPt), figureBegin);

        // Walk segments
        bool fSetFirst = figure.SetToFirstSegment();
        if (fSetFirst)
        {
            for (;;)
            {
                BYTE bType = 0;
                const MilPoint2F *pSegPt = NULL;
                bool fLastSegment = figure.GetCurrentSegment(bType, pSegPt);

                BYTE segType = bType & MilCoreSeg::TypeMask;

                if (segType == MilCoreSeg::TypeLine)
                {
                    // Line segment - pSegPt points to 1 point (the endpoint)
                    D2D1_POINT_2F d2dPt = ToD2DPoint(pSegPt[0]);
                    pSink->AddLine(d2dPt);
                }
                else if (segType == MilCoreSeg::TypeBezier)
                {
                    // Cubic bezier - pSegPt points to 3 points
                    D2D1_BEZIER_SEGMENT bezier;
                    bezier.point1 = ToD2DPoint(pSegPt[0]);
                    bezier.point2 = ToD2DPoint(pSegPt[1]);
                    bezier.point3 = ToD2DPoint(pSegPt[2]);
                    pSink->AddBezier(&bezier);
                }
                else
                {
                    // Unknown segment type - skip
                    Assert(FALSE && "Unknown segment type in IFigureData");
                }

                if (fLastSegment)
                {
                    break;
                }

                figure.SetToNextSegment();
            }
        }

        pSink->EndFigure(figureEnd);
    }

Cleanup:
    RRETURN(hr);
}

// ---------------------------------------------------------------------------
//  ConvertShapeToGeometry
//
//  Converts a WPF IShapeData (CShapeBase) to an ID2D1Geometry.
//
//  Strategy:
//    1. If the shape is an axis-aligned rectangle, create an optimised
//       ID2D1RectangleGeometry.
//    2. Otherwise, create an ID2D1PathGeometry and walk all figures via
//       the IFigureData traversal interface.
// ---------------------------------------------------------------------------

HRESULT CD2DGeometryConverter::ConvertShapeToGeometry(
    __in ID2D1Factory1 *pFactory,
    __in_ecount(1) IShapeData *pShape,
    __deref_out ID2D1Geometry **ppGeometry
    )
{
    HRESULT hr = S_OK;

    Assert(pFactory);
    Assert(pShape);
    Assert(ppGeometry);

    *ppGeometry = NULL;

    //
    // Fast path: if the shape is an axis-aligned rectangle, create
    // an ID2D1RectangleGeometry directly.
    //
    if (pShape->IsAxisAlignedRectangle() && pShape->GetFigureCount() == 1)
    {
        const IFigureData &figure = pShape->GetFigure(0);
        MilRectF rectF;
        figure.GetAsWellOrderedRectangle(rectF);

        ComPtr<ID2D1RectangleGeometry> spRectGeom;
        IFC(pFactory->CreateRectangleGeometry(
            ToD2DRect(rectF),
            &spRectGeom
        ));

        *ppGeometry = spRectGeom.Detach();
        goto Cleanup;
    }

    //
    // General path: walk all figures and emit into a path geometry.
    //
    {
        ComPtr<ID2D1PathGeometry> spPathGeom;
        IFC(pFactory->CreatePathGeometry(&spPathGeom));

        ComPtr<ID2D1GeometrySink> spSink;
        IFC(spPathGeom->Open(&spSink));

        // Set fill mode
        MilFillMode::Enum fillMode = pShape->GetFillMode();
        spSink->SetFillMode(
            (fillMode == MilFillMode::Winding)
                ? D2D1_FILL_MODE_WINDING
                : D2D1_FILL_MODE_ALTERNATE
        );

        UINT figureCount = pShape->GetFigureCount();
        for (UINT i = 0; i < figureCount; i++)
        {
            const IFigureData &figure = pShape->GetFigure(i);
            IFC(AddFigureToSink(figure, spSink.Get()));
        }

        IFC(spSink->Close());

        *ppGeometry = spPathGeom.Detach();
    }

Cleanup:
    RRETURN(hr);
}

// ---------------------------------------------------------------------------
//  CreateRectangleGeometry
// ---------------------------------------------------------------------------

HRESULT CD2DGeometryConverter::CreateRectangleGeometry(
    __in ID2D1Factory1 *pFactory,
    __in const MilRectF &rect,
    __deref_out ID2D1RectangleGeometry **ppGeometry
    )
{
    HRESULT hr = S_OK;

    Assert(pFactory);
    Assert(ppGeometry);

    *ppGeometry = NULL;

    IFC(pFactory->CreateRectangleGeometry(
        ToD2DRect(rect),
        ppGeometry
    ));

Cleanup:
    RRETURN(hr);
}

// ---------------------------------------------------------------------------
//  Helper: MapMilCapToD2D
// ---------------------------------------------------------------------------

static D2D1_CAP_STYLE MapMilCapToD2D(MilPenCap::Enum cap)
{
    switch (cap)
    {
    case MilPenCap::Flat:     return D2D1_CAP_STYLE_FLAT;
    case MilPenCap::Square:   return D2D1_CAP_STYLE_SQUARE;
    case MilPenCap::Round:    return D2D1_CAP_STYLE_ROUND;
    case MilPenCap::Triangle: return D2D1_CAP_STYLE_TRIANGLE;
    default:                  return D2D1_CAP_STYLE_FLAT;
    }
}

// ---------------------------------------------------------------------------
//  Helper: MapMilJoinToD2D
// ---------------------------------------------------------------------------

static D2D1_LINE_JOIN MapMilJoinToD2D(MilLineJoin::Enum join)
{
    switch (join)
    {
    case MilLineJoin::Miter:        return D2D1_LINE_JOIN_MITER;
    case MilLineJoin::Bevel:        return D2D1_LINE_JOIN_BEVEL;
    case MilLineJoin::Round:        return D2D1_LINE_JOIN_ROUND;
    case MilLineJoin::MiterClipped: return D2D1_LINE_JOIN_MITER_OR_BEVEL;
    default:                        return D2D1_LINE_JOIN_MITER;
    }
}

// ---------------------------------------------------------------------------
//  Helper: MapMilDashStyleToD2D
// ---------------------------------------------------------------------------

static D2D1_DASH_STYLE MapMilDashStyleToD2D(MilDashStyle::Enum dashStyle)
{
    switch (dashStyle)
    {
    case MilDashStyle::Solid:      return D2D1_DASH_STYLE_SOLID;
    case MilDashStyle::Dash:       return D2D1_DASH_STYLE_DASH;
    case MilDashStyle::Dot:        return D2D1_DASH_STYLE_DOT;
    case MilDashStyle::DashDot:    return D2D1_DASH_STYLE_DASH_DOT;
    case MilDashStyle::DashDotDot: return D2D1_DASH_STYLE_DASH_DOT_DOT;
    case MilDashStyle::Custom:     return D2D1_DASH_STYLE_CUSTOM;
    default:                       return D2D1_DASH_STYLE_SOLID;
    }
}

// ---------------------------------------------------------------------------
//  ConvertPenToStrokeStyle
//
//  Extracts pen attributes from CPlainPen and creates a D2D1 stroke style.
//  If pPen is NULL, outputs stroke width 0 and no stroke style.
// ---------------------------------------------------------------------------

HRESULT CD2DGeometryConverter::ConvertPenToStrokeStyle(
    __in ID2D1Factory1 *pFactory,
    __in_ecount_opt(1) CPlainPen *pPen,
    __out FLOAT *pStrokeWidth,
    __deref_out_opt ID2D1StrokeStyle **ppStrokeStyle
    )
{
    HRESULT hr = S_OK;

    Assert(pFactory);
    Assert(pStrokeWidth);
    Assert(ppStrokeStyle);

    *pStrokeWidth = 0.0f;
    *ppStrokeStyle = NULL;

    if (!pPen || pPen->IsEmpty())
    {
        // No pen — caller should not stroke.
        goto Cleanup;
    }

    {
        *pStrokeWidth = pPen->GetWidth();

        D2D1_STROKE_STYLE_PROPERTIES strokeProps = {};
        strokeProps.startCap  = MapMilCapToD2D(pPen->GetStartCap());
        strokeProps.endCap    = MapMilCapToD2D(pPen->GetEndCap());
        strokeProps.dashCap   = MapMilCapToD2D(pPen->GetDashCap());
        strokeProps.lineJoin  = MapMilJoinToD2D(pPen->GetJoin());
        strokeProps.miterLimit = pPen->GetMiterLimit();
        strokeProps.dashOffset = pPen->GetDashOffset();

        MilDashStyle::Enum milDashStyle = pPen->GetDashStyle();
        strokeProps.dashStyle = MapMilDashStyleToD2D(milDashStyle);

        //
        // For custom dash patterns, extract the array from CPlainPen.
        //
        const FLOAT *pDashes = NULL;
        UINT cDashes = 0;
        DynArray<FLOAT> dashBuffer;

        if (milDashStyle == MilDashStyle::Custom)
        {
            INT dashCount = pPen->GetDashCount();
            if (dashCount > 0)
            {
                IFC(dashBuffer.AddMultiple(static_cast<UINT>(dashCount)));

                for (INT i = 0; i < dashCount; i++)
                {
                    dashBuffer[i] = pPen->GetDash(i);
                }

                pDashes = dashBuffer.GetDataBuffer();
                cDashes = static_cast<UINT>(dashCount);
            }
        }

        IFC(pFactory->CreateStrokeStyle(
            &strokeProps,
            pDashes,
            cDashes,
            ppStrokeStyle
        ));
    }

Cleanup:
    RRETURN(hr);
}
