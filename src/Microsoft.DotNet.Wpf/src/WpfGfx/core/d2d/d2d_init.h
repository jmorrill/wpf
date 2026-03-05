// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+-----------------------------------------------------------------------------
//
//
//  Description:
//      D2D subsystem initialization and cleanup.
//
//      D2DSubsystem::Init() is called during DLL load and
//      D2DSubsystem::DeInit() during DLL unload. They bracket the lifetime
//      of all D2D-specific global state (render options, device manager).
//
//------------------------------------------------------------------------------

#pragma once

namespace D2DSubsystem
{
    // Initialize the D2D subsystem.
    // Should be called once at DLL load, after RenderOptions::Init().
    HRESULT Init();

    // Tear down the D2D subsystem.
    // Should be called once at DLL unload, before RenderOptions::DeInit().
    void DeInit();
};
