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
//      Implements CD2DDevice, the D2D/D3D11 device wrapper.
//
//      CD2DDevice is created via the static Create() factory method, which
//      performs the following initialization sequence:
//
//          1. D3D11CreateDevice with BGRA support and feature level array
//          2. QueryInterface for IDXGIDevice
//          3. D2D1CreateFactory (multi-threaded)
//          4. ID2D1Factory1::CreateDevice from the DXGI device
//          5. DWriteCreateFactory for IDWriteFactory1
//          6. Get adapter LUID from DXGI adapter desc
//
//  $ENDTAG
//
//------------------------------------------------------------------------------

#include <precomp.hpp>

using Microsoft::WRL::ComPtr;

//
// Simple IFC-style macro for HRESULT error handling with goto Cleanup.
// This mirrors the IFC pattern used throughout WpfGfx.
//

#ifndef D2D_IFC
#define D2D_IFC(expr) { hr = (expr); if (FAILED(hr)) goto Cleanup; }
#endif

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::CD2DDevice
//
//  Synopsis:
//      Private constructor. Use Create() to instantiate.
//
//------------------------------------------------------------------------------

CD2DDevice::CD2DDevice()
    : m_cRef(1)
    , m_fValid(false)
{
    m_adapterLuid.LowPart = 0;
    m_adapterLuid.HighPart = 0;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::~CD2DDevice
//
//  Synopsis:
//      Private destructor. Release through Release().
//
//------------------------------------------------------------------------------

CD2DDevice::~CD2DDevice()
{
    //
    // ComPtr members are automatically released.
    //
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::Create
//
//  Synopsis:
//      Static factory method. Creates a D3D11 device on the specified adapter,
//      then initializes D2D1, DirectWrite, and DXGI interfaces.
//
//  Arguments:
//      pAdapter    - The DXGI adapter to create the device on.
//      fDebugLayer - If true, the D3D11 debug layer is enabled.
//      ppDevice    - Receives the newly created CD2DDevice. The caller
//                    owns the initial reference.
//
//  Returns:
//      S_OK on success, or an appropriate HRESULT error code.
//
//------------------------------------------------------------------------------

/*static*/ HRESULT
CD2DDevice::Create(
    __in_ecount(1) IDXGIAdapter *pAdapter,
    bool fDebugLayer,
    __deref_out CD2DDevice **ppDevice
    )
{
    HRESULT hr = S_OK;
    CD2DDevice *pDevice = nullptr;

    //
    // Validate parameters
    //

    if (pAdapter == nullptr || ppDevice == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppDevice = nullptr;

    //
    // Allocate the device wrapper
    //

    pDevice = new(std::nothrow) CD2DDevice();
    if (pDevice == nullptr)
    {
        hr = E_OUTOFMEMORY;
        goto Cleanup;
    }

    //
    // Initialize D3D11, D2D1, DirectWrite on this adapter
    //

    D2D_IFC(pDevice->Init(pAdapter, fDebugLayer));

    //
    // Transfer ownership to the caller
    //

    *ppDevice = pDevice;
    pDevice = nullptr;

Cleanup:
    if (pDevice != nullptr)
    {
        pDevice->Release();
    }

    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::Init
//
//  Synopsis:
//      Internal initialization. Creates and configures all D3D11, DXGI, D2D1,
//      and DirectWrite resources.
//
//  Notes:
//      D3D11_CREATE_DEVICE_BGRA_SUPPORT is mandatory for D2D interop.
//
//------------------------------------------------------------------------------

HRESULT
CD2DDevice::Init(
    __in_ecount(1) IDXGIAdapter *pAdapter,
    bool fDebugLayer
    )
{
    HRESULT hr = S_OK;

    ComPtr<ID3D11Device> pD3D11DeviceBase;

    //
    // Feature levels we support, in order of preference.
    //

    static const D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3
    };

    D3D_FEATURE_LEVEL actualFeatureLevel = D3D_FEATURE_LEVEL_9_3;

    //
    // Creation flags — BGRA support is required for D2D interop.
    //

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    if (fDebugLayer)
    {
        createFlags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    //
    // Step 1: Create the D3D11 device
    //

    D2D_IFC(D3D11CreateDevice(
        pAdapter,                       // pAdapter
        D3D_DRIVER_TYPE_UNKNOWN,        // DriverType (UNKNOWN when adapter is specified)
        nullptr,                        // Software
        createFlags,                    // Flags
        featureLevels,                  // pFeatureLevels
        ARRAYSIZE(featureLevels),       // FeatureLevels
        D3D11_SDK_VERSION,              // SDKVersion
        &pD3D11DeviceBase,              // ppDevice
        &actualFeatureLevel,            // pFeatureLevel
        nullptr                         // ppImmediateContext (retrieved below)
        ));

    //
    // QueryInterface for ID3D11Device1 — required for D2D1.1 interop.
    //

    D2D_IFC(pD3D11DeviceBase.As(&m_pD3D11Device));

    //
    // Get the immediate context for 3D interop rendering.
    //

    m_pD3D11Device->GetImmediateContext(&m_pD3D11ImmediateContext);

    //
    // Step 2: QueryInterface for IDXGIDevice
    //

    D2D_IFC(m_pD3D11Device.As(&m_pDXGIDevice));

    //
    // Step 3: Create the D2D1 factory (multi-threaded for thread safety)
    //

    D2D_IFC(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_MULTI_THREADED,
        __uuidof(ID2D1Factory1),
        reinterpret_cast<void**>(m_pD2DFactory.GetAddressOf())
        ));

    //
    // Step 4: Create the D2D1 device from the DXGI device
    //

    {
        ComPtr<ID2D1Device> pBaseDevice;
        D2D_IFC(m_pD2DFactory->CreateDevice(
            m_pDXGIDevice.Get(),
            &pBaseDevice
            ));
        D2D_IFC(pBaseDevice.As(&m_pD2DDevice));
    }

    //
    // Step 5: Create the DirectWrite factory
    //

    D2D_IFC(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory1),
        reinterpret_cast<IUnknown**>(m_pDWriteFactory.GetAddressOf())
        ));

    //
    // Step 6: Get the adapter LUID and verify hardware
    //

    {
        DXGI_ADAPTER_DESC adapterDesc = {};
        D2D_IFC(pAdapter->GetDesc(&adapterDesc));
        m_adapterLuid = adapterDesc.AdapterLuid;

        //
        // Emit adapter info to the debug output so we can verify
        // that D2D is using a real hardware GPU, not WARP.
        //

        {
            WCHAR buf[256];
            swprintf_s(buf, ARRAYSIZE(buf),
                L"[WPF D2D] Adapter: %s  VRAM: %zuMB  FL: 0x%X\n",
                adapterDesc.Description,
                adapterDesc.DedicatedVideoMemory / (1024 * 1024),
                static_cast<unsigned>(actualFeatureLevel));
            OutputDebugStringW(buf);
        }

        //
        // Check for software / WARP adapter via IDXGIAdapter1 flags.
        //

        {
            ComPtr<IDXGIAdapter1> pAdapter1;
            if (SUCCEEDED(pAdapter->QueryInterface(IID_PPV_ARGS(&pAdapter1))))
            {
                DXGI_ADAPTER_DESC1 desc1 = {};
                if (SUCCEEDED(pAdapter1->GetDesc1(&desc1)))
                {
                    if (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                    {
                        OutputDebugStringW(L"[WPF D2D] WARNING: adapter is SOFTWARE/WARP — rejecting\n");
                        hr = DXGI_ERROR_UNSUPPORTED;
                        goto Cleanup;
                    }
                }
            }
        }
    }

    //
    // All initialization succeeded — mark device as valid.
    //

    m_fValid = true;

Cleanup:
    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::AddRef
//
//  Synopsis:
//      Increments the reference count.
//
//------------------------------------------------------------------------------

LONG
CD2DDevice::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::Release
//
//  Synopsis:
//      Decrements the reference count. Deletes the object when the count
//      reaches zero.
//
//------------------------------------------------------------------------------

LONG
CD2DDevice::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);

    if (cRef == 0)
    {
        delete this;
    }

    return cRef;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetD3D11Device
//
//  Synopsis:
//      Returns the underlying ID3D11Device pointer (not AddRef'd).
//
//------------------------------------------------------------------------------

ID3D11Device*
CD2DDevice::GetD3D11Device() const
{
    return m_pD3D11Device.Get();
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetD3D11Device1
//
//  Synopsis:
//      Returns the underlying ID3D11Device1 pointer (not AddRef'd).
//
//------------------------------------------------------------------------------

ID3D11Device1*
CD2DDevice::GetD3D11Device1() const
{
    return m_pD3D11Device.Get();
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetD2DDevice
//
//  Synopsis:
//      Returns the underlying ID2D1Device1 pointer (not AddRef'd).
//
//------------------------------------------------------------------------------

ID2D1Device1*
CD2DDevice::GetD2DDevice() const
{
    return m_pD2DDevice.Get();
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetD2DFactory
//
//  Synopsis:
//      Returns the underlying ID2D1Factory1 pointer (not AddRef'd).
//
//------------------------------------------------------------------------------

ID2D1Factory1*
CD2DDevice::GetD2DFactory() const
{
    return m_pD2DFactory.Get();
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetDWriteFactory
//
//  Synopsis:
//      Returns the underlying IDWriteFactory1 pointer (not AddRef'd).
//
//------------------------------------------------------------------------------

IDWriteFactory1*
CD2DDevice::GetDWriteFactory() const
{
    return m_pDWriteFactory.Get();
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetDXGIDevice
//
//  Synopsis:
//      Returns the underlying IDXGIDevice pointer (not AddRef'd).
//
//------------------------------------------------------------------------------

IDXGIDevice*
CD2DDevice::GetDXGIDevice() const
{
    return m_pDXGIDevice.Get();
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::CreateDeviceContext
//
//  Synopsis:
//      Creates a new ID2D1DeviceContext1 with multithreaded optimizations
//      enabled. The caller owns the returned context.
//
//  Arguments:
//      ppContext - Receives the newly created device context.
//
//  Returns:
//      S_OK on success, or an appropriate HRESULT error code.
//
//------------------------------------------------------------------------------

HRESULT
CD2DDevice::CreateDeviceContext(
    __deref_out ID2D1DeviceContext1 **ppContext
    )
{
    HRESULT hr = S_OK;

    if (ppContext == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppContext = nullptr;

    if (!m_fValid)
    {
        hr = D2DERR_RECREATE_TARGET;
        goto Cleanup;
    }

    //
    // Create a device context. Use NONE (no multi-threaded optimizations)
    // to avoid the thread synchronization overhead that dominates when
    // rendering many small primitives (particles, icons, etc.).  D2D's
    // MT mode adds locking and work-distribution costs per draw call.
    //

    D2D_IFC(m_pD2DDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        ppContext
        ));

Cleanup:
    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetD3D11ImmediateContext
//
//  Synopsis:
//      Returns the D3D11 immediate context for 3D interop rendering.
//      The returned pointer is NOT AddRef'd.
//
//------------------------------------------------------------------------------

ID3D11DeviceContext*
CD2DDevice::GetD3D11ImmediateContext() const
{
    return m_pD3D11ImmediateContext.Get();
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::IsValid
//
//  Synopsis:
//      Returns true if this device is still valid and usable for rendering.
//
//------------------------------------------------------------------------------

bool
CD2DDevice::IsValid() const
{
    return m_fValid;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::Invalidate
//
//  Synopsis:
//      Marks this device as invalid. Called when a device-lost condition is
//      detected. Subsequent calls to CreateDeviceContext will fail with
//      D2DERR_RECREATE_TARGET.
//
//------------------------------------------------------------------------------

void
CD2DDevice::Invalidate()
{
    m_fValid = false;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDevice::GetAdapterLuid
//
//  Synopsis:
//      Returns the LUID of the DXGI adapter this device was created on.
//
//------------------------------------------------------------------------------

LUID
CD2DDevice::GetAdapterLuid() const
{
    return m_adapterLuid;
}
