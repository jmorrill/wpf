// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2DTextRenderer implementation.
//
//      Converts WPF glyph-run data to DirectWrite structures and renders
//      them through the D2D1 device context.
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

//+------------------------------------------------------------------------
//
//  Function:  CD2DTextRenderer::ConvertToGlyphRun
//
//  Synopsis:  Populate a DWRITE_GLYPH_RUN from WPF DrawGlyphsParameters.
//
//  Notes:     DrawGlyphsParameters contains a CGlyphRunResource which in
//             turn exposes a fully-formed DWRITE_GLYPH_RUN via
//             GetDWriteGlyphRun().  We copy the fields so the caller can
//             optionally mutate them (e.g. for bidi adjustment) without
//             affecting the original resource.
//
//             ppFontFace is AddRef'd and must be Released by the caller.
//
//-------------------------------------------------------------------------

HRESULT
CD2DTextRenderer::ConvertToGlyphRun(
    __in IDWriteFactory1 *pDWriteFactory,
    __in const DrawGlyphsParameters &params,
    __out DWRITE_GLYPH_RUN *pGlyphRun,
    __deref_out IDWriteFontFace **ppFontFace
    )
{
    HRESULT hr = S_OK;

    *ppFontFace = nullptr;
    ZeroMemory(pGlyphRun, sizeof(*pGlyphRun));

    //
    // Build the DWRITE_GLYPH_RUN via the bridge function declared in
    // GlyphRunCore.h and implemented in glyphrunslave.cpp.
    //

    if (params.pGlyphRun == nullptr)
    {
        goto Cleanup;
    }

    D2D_IFC(BuildDWriteGlyphRunFromResource(params.pGlyphRun, pGlyphRun, ppFontFace));

    UNREFERENCED_PARAMETER(pDWriteFactory);

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2DTextRenderer::DrawGlyphRun
//
//  Synopsis:  Render a WPF glyph run via the D2D1 device context.
//
//  Details:
//      1. Convert DrawGlyphsParameters → DWRITE_GLYPH_RUN.
//      2. Save the current text antialiasing mode.
//      3. If forceClearType, set mode to D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE.
//      4. Call pContext->DrawGlyphRun() at the baseline origin.
//      5. Restore the previous antialiasing mode.
//
//  Notes:     The baseline origin is taken from the bounding rectangle
//             stored in DrawGlyphsParameters::rcBounds (left, top).
//
//-------------------------------------------------------------------------

HRESULT
CD2DTextRenderer::DrawGlyphRun(
    __in ID2D1DeviceContext1 *pContext,
    __in IDWriteFactory1 *pDWriteFactory,
    __in const DrawGlyphsParameters &params,
    __in ID2D1Brush *pBrush,
    bool forceClearType
    )
{
    HRESULT hr = S_OK;

    DWRITE_GLYPH_RUN glyphRun;
    IDWriteFontFace *pFontFace = nullptr;

    //
    // Step 1 – Convert parameters.
    //

    D2D_IFC(ConvertToGlyphRun(pDWriteFactory, params, &glyphRun, &pFontFace));

    if (glyphRun.fontFace == nullptr || glyphRun.glyphCount == 0)
    {
        //
        // Nothing to render.
        //
        goto Cleanup;
    }

    {
        //
        // Step 2 – Save current text AA mode.
        //

        D2D1_TEXT_ANTIALIAS_MODE previousAAMode = pContext->GetTextAntialiasMode();

        //
        // Step 3 – Override AA mode if requested.
        //

        D2D1_TEXT_ANTIALIAS_MODE desiredAAMode = forceClearType
            ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE
            : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;

        pContext->SetTextAntialiasMode(desiredAAMode);

        //
        // Step 4 – Determine baseline origin and draw.
        //
        // The baseline origin comes from the glyph run resource.
        //

        MilPoint2F origin = GetGlyphRunOrigin(params.pGlyphRun);
        D2D1_POINT_2F baselineOrigin = D2D1::Point2F(origin.X, origin.Y);

        DWRITE_MEASURING_MODE measuringMode = IsGlyphRunDisplayMeasured(params.pGlyphRun)
            ? DWRITE_MEASURING_MODE_GDI_CLASSIC
            : DWRITE_MEASURING_MODE_NATURAL;

        pContext->DrawGlyphRun(
            baselineOrigin,
            &glyphRun,
            pBrush,
            measuringMode
        );

        //
        // Step 5 – Restore previous AA mode.
        //

        pContext->SetTextAntialiasMode(previousAAMode);
    }

Cleanup:
    //
    // Release the font face we AddRef'd in ConvertToGlyphRun.
    //

    if (pFontFace != nullptr)
    {
        pFontFace->Release();
        pFontFace = nullptr;
    }

    RRETURN(hr);
}
