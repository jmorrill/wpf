// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//------------------------------------------------------------------------------
//
//  Description:
//      Precompiled header for d2d directory
//
//------------------------------------------------------------------------------

#include <wpfsdl.h>

// D2D code uses goto-with-cleanup pattern; suppress C4533 for POD init skipped by goto
#pragma warning(disable: 4533)
// Suppress UINT64-to-SIZE_T narrowing in DXGI memory queries
#pragma warning(disable: 4244)

// system includes
#include "std.h"
#include "wgx_core_types.h"

#include <strsafe.h>

// D3D11, D2D1, DXGI, DirectWrite
#include <d3d11.h>
#include <d3d11_1.h>
#include <d2d1_1.h>
#include <d2d1_2.h>
#include <d2d1_3.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d2d1effectauthor.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <dwrite_1.h>
#include <DirectXMath.h>

// WRL ComPtr
#include <wrl/client.h>

// debug output, allocator, etc.
#include "common\common.h"

// geometry classes
#include "geometry\geometry.h"

// COMBase
#include "api\api_include.h"

// common render target classes
#include "targets\targets.h"

// glyph rendering
#include "glyph\glyph.h"

// D2D local headers
#include "d2d_device.h"
#include "d2d_device_manager.h"
#include "d2d_geometry.h"
#include "d2d_brush.h"
#include "d2d_bitmap.h"
#include "d2d_surfrt.h"
#include "d2d_displayrt.h"
#include "d2d_effects.h"
#include "d2d_text.h"
#include "d2d_3d.h"
#include "d2d_resources.h"
#include "d2d_renderoptions.h"
#include "d2d_factory.h"
#include "d2d_texturert.h"
#include "d2d_init.h"

// SW intermediate RT creator (needed for brush realization)
#include "sw\SwIntermediateRTCreator.h"

// Meta render target (needed for CMetaBitmapRenderTarget unwrapping)
#include "meta\meta.h"


