// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+-----------------------------------------------------------------------
//
//  Description:
//      CD2DBitmapConverter implementation.
//
//      Converts WPF IWGXBitmapSource data to Direct2D ID2D1Bitmap1 by
//      reading pixel format, dimensions, and pixel data through the
//      WPF bitmap source interface and creating a D2D bitmap with the
//      appropriate DXGI format and alpha mode.
//
//------------------------------------------------------------------------

#include <precomp.hpp>

// ---------------------------------------------------------------------------
//  MilPixelFormatToDXGI
//
//  Maps WPF MilPixelFormat::Enum values to the closest DXGI_FORMAT.
//  Only the most common formats used by the WPF rendering pipeline are
//  mapped here.  Exotic or indexed formats return DXGI_FORMAT_UNKNOWN.
// ---------------------------------------------------------------------------

DXGI_FORMAT CD2DBitmapConverter::MilPixelFormatToDXGI(MilPixelFormat::Enum fmt)
{
    switch (fmt)
    {
    //
    // 32 bpp BGRA variants — the workhorse WPF formats.
    //
    case MilPixelFormat::PBGRA32bpp:
        // Pre-multiplied BGRA — maps directly to DXGI B8G8R8A8.
        return DXGI_FORMAT_B8G8R8A8_UNORM;

    case MilPixelFormat::BGRA32bpp:
        // Straight-alpha BGRA — same DXGI pixel layout, but alpha
        // interpretation differs (handled by alpha mode, not format).
        return DXGI_FORMAT_B8G8R8A8_UNORM;

    case MilPixelFormat::BGR32bpp:
        // 32 bpp with unused alpha byte — use B8G8R8X8.
        return DXGI_FORMAT_B8G8R8X8_UNORM;

    //
    // 8 bpp gray
    //
    case MilPixelFormat::Gray8bpp:
        // Single-channel 8-bit — maps to R8.
        return DXGI_FORMAT_R8_UNORM;

    //
    // 16 bpp gray (fixed point)
    //
    case MilPixelFormat::Gray16bpp:
        // 16-bit luminance — maps to R16.
        return DXGI_FORMAT_R16_UNORM;

    //
    // 128 bpp float RGBA variants
    //
    case MilPixelFormat::RGBA128bppFloat:
    case MilPixelFormat::PRGBA128bppFloat:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;

    //
    // 64 bpp RGBA (16 bits per channel)
    //
    case MilPixelFormat::RGBA64bpp:
    case MilPixelFormat::PRGBA64bpp:
        return DXGI_FORMAT_R16G16B16A16_UNORM;

    //
    // Formats with no direct DXGI equivalent.
    // The caller must convert these to a supported format before creating
    // a D2D bitmap.
    //
    case MilPixelFormat::BGR24bpp:      // 24 bpp — no DXGI match
    case MilPixelFormat::RGB24bpp:
    case MilPixelFormat::Indexed1bpp:
    case MilPixelFormat::Indexed2bpp:
    case MilPixelFormat::Indexed4bpp:
    case MilPixelFormat::Indexed8bpp:
    case MilPixelFormat::BlackWhite:
    case MilPixelFormat::Gray2bpp:
    case MilPixelFormat::Gray4bpp:
    case MilPixelFormat::CMYK32bpp:
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
//  MilPixelFormatToAlphaMode
//
//  Returns the D2D1_ALPHA_MODE that matches the WPF pixel format's
//  alpha semantics.
// ---------------------------------------------------------------------------

D2D1_ALPHA_MODE CD2DBitmapConverter::MilPixelFormatToAlphaMode(MilPixelFormat::Enum fmt)
{
    switch (fmt)
    {
    case MilPixelFormat::PBGRA32bpp:
    case MilPixelFormat::PRGBA64bpp:
    case MilPixelFormat::PRGBA128bppFloat:
        return D2D1_ALPHA_MODE_PREMULTIPLIED;

    case MilPixelFormat::BGRA32bpp:
    case MilPixelFormat::RGBA64bpp:
    case MilPixelFormat::RGBA128bppFloat:
        //
        // Straight-alpha formats.  D2D does NOT support
        // D2D1_ALPHA_MODE_STRAIGHT for CreateBitmap — it returns
        // D2DERR_UNSUPPORTED_PIXEL_FORMAT.  Return UNKNOWN so that
        // ConvertBitmapSource falls through to the PBGRA32bpp
        // conversion path which pre-multiplies the alpha.
        //
        return D2D1_ALPHA_MODE_UNKNOWN;

    case MilPixelFormat::BGR32bpp:
    case MilPixelFormat::BGR24bpp:
    case MilPixelFormat::RGB24bpp:
        // Formats with no alpha channel — treat as opaque (ignore).
        return D2D1_ALPHA_MODE_IGNORE;

    case MilPixelFormat::Gray8bpp:
    case MilPixelFormat::Gray16bpp:
        return D2D1_ALPHA_MODE_IGNORE;

    default:
        return D2D1_ALPHA_MODE_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
//  GetBytesPerPixel
//
//  Returns the byte count per pixel for formats we support.
//  Returns 0 for unsupported formats.
// ---------------------------------------------------------------------------

static UINT GetBytesPerPixel(MilPixelFormat::Enum fmt)
{
    switch (fmt)
    {
    case MilPixelFormat::Gray8bpp:           return 1;
    case MilPixelFormat::Gray16bpp:          return 2;
    case MilPixelFormat::BGR24bpp:
    case MilPixelFormat::RGB24bpp:           return 3;
    case MilPixelFormat::BGR32bpp:
    case MilPixelFormat::BGRA32bpp:
    case MilPixelFormat::PBGRA32bpp:
    case MilPixelFormat::CMYK32bpp:          return 4;
    case MilPixelFormat::RGBA64bpp:
    case MilPixelFormat::PRGBA64bpp:         return 8;
    case MilPixelFormat::RGBA128bppFloat:
    case MilPixelFormat::PRGBA128bppFloat:
    case MilPixelFormat::RGB128bppFloat:     return 16;
    default:                                 return 0;
    }
}

// ---------------------------------------------------------------------------
//  ConvertBitmapSource
//
//  Reads pixel data from a WPF IWGXBitmapSource and creates a D2D1 bitmap.
//
//  IWGXBitmapSource exposes:
//    - GetSize(&width, &height)
//    - GetPixelFormat(&fmt)
//    - CopyPixels(prc, stride, bufferSize, pBuffer)
//
//  If the source pixel format has no direct DXGI equivalent, we fall back
//  to copying through PBGRA32bpp by requesting the source to produce that
//  format.  (IWGXBitmapSource implementations in WPF generally support
//  PBGRA32bpp output.)  If the source cannot supply PBGRA32bpp either,
//  we fail with WGXERR_UNSUPPORTEDPIXELFORMAT.
// ---------------------------------------------------------------------------

HRESULT CD2DBitmapConverter::ConvertBitmapSource(
    __in ID2D1DeviceContext1 *pContext,
    __in IWGXBitmapSource *pSource,
    __deref_out ID2D1Bitmap1 **ppBitmap
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(pSource);
    Assert(ppBitmap);

    *ppBitmap = NULL;

    //
    // Query source dimensions and pixel format.
    //
    UINT width  = 0;
    UINT height = 0;
    IFC(pSource->GetSize(&width, &height));

    MilPixelFormat::Enum milFmt = MilPixelFormat::Undefined;
    IFC(pSource->GetPixelFormat(&milFmt));

    //
    // Map to DXGI format.  If the native format has no direct match,
    // we will re-read as PBGRA32bpp below.
    //
    DXGI_FORMAT dxgiFmt = MilPixelFormatToDXGI(milFmt);
    D2D1_ALPHA_MODE alphaMode = MilPixelFormatToAlphaMode(milFmt);
    MilPixelFormat::Enum readFmt = milFmt;

    if (dxgiFmt == DXGI_FORMAT_UNKNOWN || alphaMode == D2D1_ALPHA_MODE_UNKNOWN)
    {
        //
        // Fall back to PBGRA32bpp — the universal WPF format.
        // This also handles straight-alpha formats (BGRA32bpp, etc.)
        // which D2D cannot use directly.
        //
        dxgiFmt   = DXGI_FORMAT_B8G8R8A8_UNORM;
        alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        readFmt   = MilPixelFormat::PBGRA32bpp;
    }

    {
        //
        // Compute stride and allocate staging buffer.
        //
        UINT bpp = GetBytesPerPixel(readFmt);
        if (bpp == 0)
        {
            // Unsupported format even after fallback — should not happen
            // for PBGRA32bpp, but guard anyway.
            IFC(WGXERR_UNSUPPORTEDPIXELFORMAT);
        }

        UINT stride = width * bpp;

        // Align stride to 4-byte boundary (common requirement).
        stride = (stride + 3) & ~3u;

        UINT bufferSize = stride * height;

        //
        // Allocate staging buffer.
        //
        BYTE *pPixels = new(std::nothrow) BYTE[bufferSize];
        IFCOOM(pPixels);

        //
        // Copy pixel data from the WPF source.
        // Passing NULL for prc copies the entire bitmap.
        //

        // BREADCRUMB: PRE_COPYPIXELS
        {
            LARGE_INTEGER cpNow; QueryPerformanceCounter(&cpNow);
            WCHAR ep[MAX_PATH];
            if (GetTempPathW(MAX_PATH, ep) > 0) {
                wcscat_s(ep, MAX_PATH, L"wpf_d2d_freeze.log");
                HANDLE hf = CreateFileW(ep, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE) {
                    WCHAR m[256]; int l = swprintf_s(m, ARRAYSIZE(m),
                        L"    CVT PRE_COPYPIXELS t=%lld %ux%u stride=%u bufSz=%u fmt=%d\r\n",
                        cpNow.QuadPart, width, height, stride, bufferSize, (int)readFmt);
                    DWORD dw; WriteFile(hf, m, l*sizeof(WCHAR), &dw, NULL);
                    CloseHandle(hf);
                }
            }
        }

        hr = pSource->CopyPixels(
            NULL,       // prc — NULL means full image
            stride,
            bufferSize,
            pPixels
        );

        // BREADCRUMB: POST_COPYPIXELS
        {
            LARGE_INTEGER cpNow; QueryPerformanceCounter(&cpNow);
            WCHAR ep[MAX_PATH];
            if (GetTempPathW(MAX_PATH, ep) > 0) {
                wcscat_s(ep, MAX_PATH, L"wpf_d2d_freeze.log");
                HANDLE hf = CreateFileW(ep, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE) {
                    WCHAR m[256]; int l = swprintf_s(m, ARRAYSIZE(m),
                        L"    CVT POST_COPYPIXELS t=%lld hr=%08lX %ux%u\r\n",
                        cpNow.QuadPart, (unsigned long)hr, width, height);
                    DWORD dw; WriteFile(hf, m, l*sizeof(WCHAR), &dw, NULL);
                    CloseHandle(hf);
                }
            }
        }

        if (FAILED(hr))
        {
            delete[] pPixels;
            goto Cleanup;
        }

        //
        // Create D2D bitmap from the staging buffer.
        //
        D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
        bitmapProps.pixelFormat.format    = dxgiFmt;
        bitmapProps.pixelFormat.alphaMode = alphaMode;
        bitmapProps.dpiX   = 96.0f;
        bitmapProps.dpiY   = 96.0f;
        bitmapProps.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        bitmapProps.colorContext   = NULL;

        //
        // Attempt to read the actual DPI from the source.
        //
        {
            double dpiX = 96.0;
            double dpiY = 96.0;
            if (SUCCEEDED(pSource->GetResolution(&dpiX, &dpiY)))
            {
                bitmapProps.dpiX = static_cast<FLOAT>(dpiX);
                bitmapProps.dpiY = static_cast<FLOAT>(dpiY);
            }
        }

        // BREADCRUMB: PRE_CREATEBITMAP
        {
            LARGE_INTEGER cbNow; QueryPerformanceCounter(&cbNow);
            WCHAR ep[MAX_PATH];
            if (GetTempPathW(MAX_PATH, ep) > 0) {
                wcscat_s(ep, MAX_PATH, L"wpf_d2d_freeze.log");
                HANDLE hf = CreateFileW(ep, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE) {
                    WCHAR m[256]; int l = swprintf_s(m, ARRAYSIZE(m),
                        L"    CVT PRE_CREATEBITMAP t=%lld %ux%u dxgi=%d alpha=%d\r\n",
                        cbNow.QuadPart, width, height, (int)dxgiFmt, (int)alphaMode);
                    DWORD dw; WriteFile(hf, m, l*sizeof(WCHAR), &dw, NULL);
                    CloseHandle(hf);
                }
            }
        }

        hr = pContext->CreateBitmap(
            D2D1::SizeU(width, height),
            pPixels,
            stride,
            &bitmapProps,
            ppBitmap
        );

        // BREADCRUMB: POST_CREATEBITMAP
        {
            LARGE_INTEGER cbNow; QueryPerformanceCounter(&cbNow);
            WCHAR ep[MAX_PATH];
            if (GetTempPathW(MAX_PATH, ep) > 0) {
                wcscat_s(ep, MAX_PATH, L"wpf_d2d_freeze.log");
                HANDLE hf = CreateFileW(ep, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE) {
                    WCHAR m[256]; int l = swprintf_s(m, ARRAYSIZE(m),
                        L"    CVT POST_CREATEBITMAP t=%lld hr=%08lX %ux%u\r\n",
                        cbNow.QuadPart, (unsigned long)hr, width, height);
                    DWORD dw; WriteFile(hf, m, l*sizeof(WCHAR), &dw, NULL);
                    CloseHandle(hf);
                }
            }
        }

        delete[] pPixels;
        IFC(hr);
    }

Cleanup:
    RRETURN(hr);
}
