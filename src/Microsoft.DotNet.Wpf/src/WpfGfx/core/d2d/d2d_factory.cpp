// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+-----------------------------------------------------------------------------
//
//
//  Description:
//      Implementation of:
//        - D2DRendering_ShouldUse()
//        - D2DDisplayRenderTarget_Create()
//
//      These are the primary integration points between the existing WPF
//      CDesktopRenderTarget infrastructure and the new D2D backend.
//
//------------------------------------------------------------------------------

#include <precomp.hpp>

//+-----------------------------------------------------------------------------
//
//  Function:
//      D2DRendering_ShouldUse
//
//  Synopsis:
//      Returns true when both:
//        1. The managed layer has opted in via AppContext switch, and
//        2. The current system supports D2D rendering (D3D11 + D2D1).
//
//------------------------------------------------------------------------------

bool
D2DRendering_ShouldUse()
{
    return RenderOptions::IsD2DRenderingEnabled()
        && D2DRenderOptions::IsD2DRenderingSupported();
}

//+-----------------------------------------------------------------------------
//
//  Function:
//      D2DDisplayRenderTarget_Create
//
//  Synopsis:
//      Creates a CD2DDisplayRenderTarget for the specified HWND/display and
//      returns it as an IRenderTargetHWNDInternal*, which is the interface
//      the CDesktopRenderTarget meta data expects.
//
//      Flow:
//        1. Call CD2DDisplayRenderTarget::Create to get a CD2DDisplayRenderTarget*.
//        2. QI for IRenderTargetHWNDInternal.
//        3. Release the raw pointer (the QI AddRef keeps it alive).
//
//  Arguments:
//      hwnd                - Target HWND (may be NULL for off-screen tests).
//      eWindowLayerType    - Layer type (opaque, transparent, etc.).
//      pDisplay            - CDisplay describing the adapter/monitor.
//      dwFlags             - MilRTInitialization flags.
//      ppRenderTarget      - [out] The created render target.
//
//  Returns:
//      HRESULT
//
//------------------------------------------------------------------------------

HRESULT
D2DDisplayRenderTarget_Create(
    __in_opt HWND hwnd,
    MilWindowLayerType::Enum eWindowLayerType,
    __in_ecount(1) CDisplay const *pDisplay,
    MilRTInitialization::Flags dwFlags,
    __deref_out IRenderTargetHWNDInternal **ppHWNDRenderTarget,
    __deref_out IRenderTargetInternal **ppInternalRenderTarget
    )
{
    HRESULT hr = S_OK;

    CD2DDisplayRenderTarget *pD2DRT = NULL;

    Assert(ppHWNDRenderTarget);
    Assert(ppInternalRenderTarget);
    *ppHWNDRenderTarget = NULL;
    *ppInternalRenderTarget = NULL;

    //
    // Step 1: Create the D2D display render target.
    //

    IFC(CD2DDisplayRenderTarget::Create(
        hwnd,
        eWindowLayerType,
        pDisplay,
        dwFlags,
        &pD2DRT
        ));

    //
    // Step 2: Hand back both interface pointers.
    //
    // CD2DDisplayRenderTarget inherits from both CD2DSurfaceRenderTarget
    // (which provides IRenderTargetInternal) and IRenderTargetHWNDInternal.
    // We give ownership of the ref to the caller via ppInternalRenderTarget
    // (IUnknown-based), and hand out the HWND interface as a raw pointer.
    //

    *ppHWNDRenderTarget = static_cast<IRenderTargetHWNDInternal *>(pD2DRT);
    *ppInternalRenderTarget = static_cast<IRenderTargetInternal *>(pD2DRT);
    pD2DRT->AddRef();  // one ref for ppInternalRenderTarget
    // ppHWNDRenderTarget is non-ref-counted (same object, caller uses pInternalRT for lifetime)

    // Transfer the Create ref to the second interface tracked by caller
    pD2DRT = NULL;  // prevent Cleanup from releasing

Cleanup:

    ReleaseInterface(pD2DRT);

    RRETURN(hr);
}
