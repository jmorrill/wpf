// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+-----------------------------------------------------------------------------
//
//
//  Description:
//      Factory function for creating D2D display render targets and a helper
//      to determine whether the D2D backend should be used.
//
//      D2DDisplayRenderTarget_Create is the D2D equivalent of
//      CHwDisplayRenderTarget::Create and is called from
//      CDesktopRenderTarget::Init when the D2D backend is active.
//
//------------------------------------------------------------------------------

#pragma once

class CDisplay;

//+-----------------------------------------------------------------------------
//
//  Function:
//      D2DDisplayRenderTarget_Create
//
//  Synopsis:
//      Creates a CD2DDisplayRenderTarget for the given HWND and display,
//      then returns it via the IRenderTargetHWNDInternal interface used
//      by the meta render target infrastructure.
//
//      This is analogous to CHwDisplayRenderTarget::Create in the D3D9 path.
//
//  Returns:
//      S_OK on success, or a failure HRESULT if D2D target creation fails.
//
//------------------------------------------------------------------------------

HRESULT D2DDisplayRenderTarget_Create(
    __in_opt HWND hwnd,
    MilWindowLayerType::Enum eWindowLayerType,
    __in_ecount(1) CDisplay const *pDisplay,
    MilRTInitialization::Flags dwFlags,
    __deref_out IRenderTargetHWNDInternal **ppHWNDRenderTarget,
    __deref_out IRenderTargetInternal **ppInternalRenderTarget
    );

//+-----------------------------------------------------------------------------
//
//  Function:
//      D2DRendering_ShouldUse
//
//  Synopsis:
//      Returns TRUE if the D2D rendering backend should be attempted.
//      This combines the opt-in switch (from managed AppContext) with the
//      runtime capability check (D3D11 + D2D1 available).
//
//------------------------------------------------------------------------------

bool D2DRendering_ShouldUse();
