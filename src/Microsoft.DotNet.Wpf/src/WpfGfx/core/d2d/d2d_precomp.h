// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+-----------------------------------------------------------------------------
//
//
//  $TAG ENGR
//
//      $Module:    win_mil_graphics_d2d
//      $Keywords:
//
//  $Description:
//      Precompiled header for the D2D/D3D11 rendering backend modules.
//
//  $ENDTAG
//
//------------------------------------------------------------------------------

#pragma once

// Windows
#include <windows.h>
#include <unknwn.h>

// Direct3D 11
#include <d3d11.h>
#include <d3d11_1.h>

// DXGI
#include <dxgi1_2.h>
#include <dxgi1_3.h>

// Direct2D
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>

// DirectWrite
#include <dwrite_1.h>

// WRL for ComPtr
#include <wrl/client.h>

// WpfGfx common types (reference paths will be resolved by build system)
// Forward declarations for WPF types we use
