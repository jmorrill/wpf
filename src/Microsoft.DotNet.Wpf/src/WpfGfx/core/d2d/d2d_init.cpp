// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+-----------------------------------------------------------------------------
//
//
//  Description:
//      D2D subsystem initialization and cleanup implementation.
//
//      Init  — sets up D2DRenderOptions and, when D2D is enabled, eagerly
//              creates the CD2DDeviceManager singleton so device enumeration
//              happens once.
//
//      DeInit — releases the CD2DDeviceManager singleton and tears down
//               D2DRenderOptions state.
//
//      Order relative to existing subsystems
//      ======================================
//      Startup:  RenderOptions::Init()  →  D2DSubsystem::Init()
//      Shutdown: D2DSubsystem::DeInit() →  RenderOptions::DeInit()
//
//------------------------------------------------------------------------------

#include <precomp.hpp>

//+---------------------------------------------------------------------------------
//
//  D2DSubsystem::Init
//
//  Synopsis:   Initializes D2D render options and propagates the enable flag.
//              Hardware probing is deferred to avoid loader-lock failures.
//
//----------------------------------------------------------------------------------

HRESULT
D2DSubsystem::Init()
{
    HRESULT hr = S_OK;

    //
    // Step 1: Initialize D2D render option state (critical section, flags).
    //

    IFC(D2DRenderOptions::Init());

    //
    // Propagate the common RenderOptions flag into D2DRenderOptions so that
    // the D2D subsystem's own flag is in sync. This handles the case where
    // the env var WPF_USE_D2D was set and RenderOptions::Init() picked it up.
    //

    if (RenderOptions::IsD2DRenderingEnabled())
    {
        D2DRenderOptions::SetD2DRenderingEnabled(TRUE);
    }

    //
    // NOTE: We intentionally do NOT call IsD2DRenderingSupported() or
    // CD2DDeviceManager::Create() here. This function runs during
    // DLL_PROCESS_ATTACH (inside the loader lock), and D3D11CreateDevice /
    // DXGI calls fail with DXGI_ERROR_UNSUPPORTED (0x887A0001) when the
    // loader lock is held because the GPU driver DLLs cannot be loaded.
    //
    // The hardware capability probe and device manager creation will happen
    // lazily on the render thread the first time D2DRendering_ShouldUse()
    // or D2DDisplayRenderTarget_Create() is called.
    //

Cleanup:

    RRETURN(hr);
}

//+---------------------------------------------------------------------------------
//
//  D2DSubsystem::DeInit
//
//  Synopsis:   Tears down the D2D subsystem.
//              Releases the CD2DDeviceManager singleton (if created) and
//              cleans up D2DRenderOptions state.
//
//----------------------------------------------------------------------------------

void
D2DSubsystem::DeInit()
{
    //
    // Release the device manager singleton first — it holds D3D11/D2D1
    // resources that should be freed before we tear down options state.
    //

    CD2DDeviceManager::Release();

    //
    // Tear down render options (critical section etc.).
    //

    D2DRenderOptions::DeInit();
}
