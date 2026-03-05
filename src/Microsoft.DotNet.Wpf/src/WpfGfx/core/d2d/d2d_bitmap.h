// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------
//
//  Description:
//      CD2DBitmapConverter - Utility class that converts WPF
//      IWGXBitmapSource pixel data and pixel format information
//      into Direct2D ID2D1Bitmap1 objects.
//
//------------------------------------------------------------------------

#pragma once

class CD2DBitmapConverter
{
public:
    // Convert a WPF IWGXBitmapSource to a D2D1 bitmap
    static HRESULT ConvertBitmapSource(
        __in ID2D1DeviceContext1 *pContext,
        __in IWGXBitmapSource *pSource,
        __deref_out ID2D1Bitmap1 **ppBitmap
    );

    // Map WPF pixel format to DXGI format
    static DXGI_FORMAT MilPixelFormatToDXGI(MilPixelFormat::Enum fmt);

    // Map WPF pixel format to D2D alpha mode
    static D2D1_ALPHA_MODE MilPixelFormatToAlphaMode(MilPixelFormat::Enum fmt);

private:
    CD2DBitmapConverter() = delete;
};
