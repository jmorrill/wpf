// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2DTextRenderer declaration.
//
//      Provides helper methods for rendering WPF glyph runs through
//      Direct2D / DirectWrite.  The main consumer is
//      CD2DSurfaceRenderTarget::DrawGlyphs, which delegates here for the
//      glyph-level work.
//
//----------------------------------------------------------------------------

#pragma once

//+------------------------------------------------------------------------
//
//  Class: CD2DTextRenderer
//
//  Synopsis: Static helper that converts WPF glyph parameters to
//            DirectWrite structures and renders them via the D2D1
//            device context.
//
//------------------------------------------------------------------------

class CD2DTextRenderer
{
public:

    //+--------------------------------------------------------------------
    //
    //  Method:   DrawGlyphRun
    //
    //  Synopsis: Render a WPF glyph run using DirectWrite.
    //
    //            Converts the DrawGlyphsParameters to a DWRITE_GLYPH_RUN,
    //            selects the appropriate text antialiasing mode, draws the
    //            glyph run, and restores the previous AA mode.
    //
    //---------------------------------------------------------------------

    static HRESULT DrawGlyphRun(
        __in ID2D1DeviceContext1 *pContext,
        __in IDWriteFactory1 *pDWriteFactory,
        __in const DrawGlyphsParameters &params,
        __in ID2D1Brush *pBrush,
        bool forceClearType
    );

    //+--------------------------------------------------------------------
    //
    //  Method:   ConvertToGlyphRun
    //
    //  Synopsis: Convert WPF glyph parameters to a DWRITE_GLYPH_RUN
    //            structure suitable for ID2D1DeviceContext::DrawGlyphRun.
    //
    //---------------------------------------------------------------------

    static HRESULT ConvertToGlyphRun(
        __in IDWriteFactory1 *pDWriteFactory,
        __in const DrawGlyphsParameters &params,
        __out DWRITE_GLYPH_RUN *pGlyphRun,
        __deref_out IDWriteFontFace **ppFontFace
    );

private:
    CD2DTextRenderer() = delete;
};
