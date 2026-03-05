// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+--------------------------------------------------------------------------
//
//
//  Description:
//      D2D rendering option implementation.
//
//      This file mirrors the structure of core/common/renderoptions.cpp and
//      adds a capability probe that verifies D3D11 + D2D1 availability on
//      the current system before the D2D backend is used.
//
//      Thread safety: CCriticalSection + CGuard, matching existing WPF
//      patterns.
//
//----------------------------------------------------------------------------

#include <precomp.hpp>

// ---------------------------------------------------------------------------\n// NOTE: The exported C entry points (RenderOptions_SetD2DRenderingEnabled,\n// RenderOptions_IsD2DRenderingEnabled) are defined in\n// core/common/renderoptions.cpp under #ifdef WPF_D2D_ENABLED.\n// They call RenderOptions::SetD2DRenderingEnabled / IsD2DRenderingEnabled\n// which set the flag that the D2D backend checks.\n// Do NOT duplicate those definitions here.\n// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

// m_csD2D must be entered before accessing shared state because multiple
// managed threads plus the render thread could try to access it.
static CCriticalSection m_csD2D;

// Whether the managed layer has opted in to D2D rendering.
static bool m_fD2DEnabled;

// Lazy capability-check state.
static bool m_fD2DChecked;
static bool m_fD2DSupported;

//+---------------------------------------------------------------------------------
//
//  D2DRenderOptions::Init
//
//  Synopsis:   Initializes the D2DRenderOptions state.
//              This is not thread safe and should only be called once at DLL load.
//
//----------------------------------------------------------------------------------

HRESULT
D2DRenderOptions::Init()
{
    m_fD2DEnabled   = false;
    m_fD2DChecked   = false;
    m_fD2DSupported = false;

    RRETURN(m_csD2D.Init());
}

//+---------------------------------------------------------------------------------
//
//  D2DRenderOptions::DeInit
//
//  Synopsis:   Deinits the D2DRenderOptions state.
//              This is not thread safe and should only be called once at DLL unload.
//
//----------------------------------------------------------------------------------

void
D2DRenderOptions::DeInit()
{
    m_csD2D.DeInit();
}

//+---------------------------------------------------------------------------------
//
//  D2DRenderOptions::SetD2DRenderingEnabled
//
//  Synopsis:   Sets whether or not we should use the D2D/D3D11 rendering
//              backend for this process.
//
//              Note: uses BOOL not bool because fEnabled is marshalled from
//              managed code.
//
//----------------------------------------------------------------------------------

void
D2DRenderOptions::SetD2DRenderingEnabled(BOOL fEnabled)
{
    CGuard<CCriticalSection> guard(m_csD2D);
    m_fD2DEnabled = !!fEnabled;
}

//+---------------------------------------------------------------------------------
//
//  D2DRenderOptions::IsD2DRenderingEnabled
//
//  Synopsis:   Returns whether the managed layer has opted in to D2D rendering.
//
//              Note: returns BOOL not bool to facilitate managed marshalling.
//
//----------------------------------------------------------------------------------

BOOL
D2DRenderOptions::IsD2DRenderingEnabled()
{
    CGuard<CCriticalSection> guard(m_csD2D);
    return m_fD2DEnabled;
}

//+---------------------------------------------------------------------------------
//
//  D2DRenderOptions::IsD2DRenderingSupported
//
//  Synopsis:   Returns whether the current system supports D2D rendering.
//              The probe is performed lazily on the first call and the result
//              is cached.
//
//----------------------------------------------------------------------------------

BOOL
D2DRenderOptions::IsD2DRenderingSupported()
{
    CGuard<CCriticalSection> guard(m_csD2D);

    if (!m_fD2DChecked)
    {
        BOOL fSupported = FALSE;
        HRESULT hr = CheckD2DSupport(&fSupported);
        if (SUCCEEDED(hr))
        {
            m_fD2DSupported = !!fSupported;
        }
        else
        {
            m_fD2DSupported = false;
        }
        m_fD2DChecked = true;
    }

    return m_fD2DSupported;
}

//+---------------------------------------------------------------------------------
//
//  D2DRenderOptions::CheckD2DSupport
//
//  Synopsis:   Probes the current system for D2D/D3D11 rendering capability.
//
//              The check creates a D3D11 device with the BGRA support flag
//              (required by D2D interop) and a D2D1 factory.  If both
//              succeed the system is deemed capable.  All resources are
//              released immediately; this is a capability probe only.
//
//              Feature level requirement: >= D3D_FEATURE_LEVEL_9_3
//
//----------------------------------------------------------------------------------

HRESULT
D2DRenderOptions::CheckD2DSupport(__out BOOL *pfSupported)
{
    HRESULT hr = S_OK;

    Assert(pfSupported);
    *pfSupported = FALSE;

    ID3D11Device *pD3DDevice = NULL;
    ID2D1Factory1 *pD2DFactory = NULL;
    D3D_FEATURE_LEVEL featureLevelsRequested[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };
    D3D_FEATURE_LEVEL featureLevelReturned = static_cast<D3D_FEATURE_LEVEL>(0);

    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#if DBG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        NULL,                                   // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,                                   // no software module
        createDeviceFlags,
        featureLevelsRequested,
        ARRAYSIZE(featureLevelsRequested),
        D3D11_SDK_VERSION,
        &pD3DDevice,
        &featureLevelReturned,
        NULL                                    // don't need device context
        );

    if (FAILED(hr))
    {
        // D3D11 hardware is not available.  D2D rendering is not supported.
        hr = S_OK;
        goto Cleanup;
    }

    //
    // 2. Try to create a D2D1 factory.
    //

    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        NULL,
        reinterpret_cast<void**>(&pD2DFactory)
        );

    if (FAILED(hr))
    {
        // D2D1 is not available.  D2D rendering is not supported.
        hr = S_OK;
        goto Cleanup;
    }

    //
    // Both succeeded — the system supports D2D rendering.
    //

    *pfSupported = TRUE;

Cleanup:

    if (pD2DFactory)
    {
        pD2DFactory->Release();
    }

    if (pD3DDevice)
    {
        pD3DDevice->Release();
    }

    RRETURN(hr);
}
