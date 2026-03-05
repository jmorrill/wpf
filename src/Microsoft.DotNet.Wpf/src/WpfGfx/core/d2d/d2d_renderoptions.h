// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+--------------------------------------------------------------------------
//
//
//  Description:
//      D2D rendering option declarations. Controls whether the D2D/D3D11
//      backend is enabled and whether the current system supports it.
//
//      Follows the same pattern as core/common/renderoptions.h for the
//      existing D3D9 software/hardware toggle.
//
//----------------------------------------------------------------------------

#pragma once

namespace D2DRenderOptions
{
    // These two methods are not thread safe and should only be called at DLL load/unload
    HRESULT Init();
    void DeInit();

    // Enable/disable D2D rendering for this process.
    // fEnabled is BOOL (not bool) to facilitate managed marshalling.
    void SetD2DRenderingEnabled(BOOL fEnabled);
    BOOL IsD2DRenderingEnabled();

    // Check if the system supports D2D rendering.
    // Result is lazily cached on first call.
    // (D3D11 + D2D1 available, feature level >= 9.3)
    BOOL IsD2DRenderingSupported();

    // Force D2D rendering availability check (tests adapter capabilities).
    // Can be called multiple times; result is cached after the first
    // successful probe.
    HRESULT CheckD2DSupport(__out BOOL *pfSupported);
};
