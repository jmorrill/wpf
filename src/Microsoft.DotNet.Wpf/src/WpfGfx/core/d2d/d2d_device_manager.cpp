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
//      Implements CD2DDeviceManager, the singleton that manages CD2DDevice
//      instances per DXGI adapter.
//
//      This is analogous to CD3DDeviceManager in the D3D9 backend. The
//      manager owns an IDXGIFactory2 for adapter enumeration and caches
//      one CD2DDevice per adapter index. On device-lost, the affected
//      device is invalidated and released so that the next request creates
//      a fresh device.
//
//  $ENDTAG
//
//------------------------------------------------------------------------------

#include <precomp.hpp>

using Microsoft::WRL::ComPtr;

//
// Simple IFC-style macro for HRESULT error handling with goto Cleanup.
//

#ifndef D2D_IFC
#define D2D_IFC(expr) { hr = (expr); if (FAILED(hr)) goto Cleanup; }
#endif

//
// Singleton instance pointer.
//

/*static*/ CD2DDeviceManager* CD2DDeviceManager::s_pInstance = nullptr;

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::CD2DDeviceManager
//
//  Synopsis:
//      Private constructor. Zeroes the per-adapter device array.
//
//------------------------------------------------------------------------------

CD2DDeviceManager::CD2DDeviceManager()
    : m_cAdapters(0)
{
    ZeroMemory(m_rgDevices, sizeof(m_rgDevices));
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::~CD2DDeviceManager
//
//  Synopsis:
//      Releases all cached CD2DDevice instances.
//
//------------------------------------------------------------------------------

CD2DDeviceManager::~CD2DDeviceManager()
{
    for (UINT i = 0; i < MAX_ADAPTERS; i++)
    {
        if (m_rgDevices[i] != nullptr)
        {
            m_rgDevices[i]->Release();
            m_rgDevices[i] = nullptr;
        }
    }
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::Create
//
//  Synopsis:
//      Creates and initializes the singleton CD2DDeviceManager. This must be
//      called once during engine startup before any device requests are made.
//
//  Arguments:
//      ppManager - Receives a pointer to the singleton manager (not AddRef'd).
//
//  Returns:
//      S_OK on success, or an appropriate HRESULT error code.
//
//------------------------------------------------------------------------------

/*static*/ HRESULT
CD2DDeviceManager::Create(
    __deref_out CD2DDeviceManager **ppManager
    )
{
    HRESULT hr = S_OK;
    CD2DDeviceManager *pManager = nullptr;

    if (ppManager == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppManager = nullptr;

    //
    // If the singleton already exists, return it directly.
    //

    if (s_pInstance != nullptr)
    {
        *ppManager = s_pInstance;
        goto Cleanup;
    }

    //
    // Allocate the manager
    //

    pManager = new(std::nothrow) CD2DDeviceManager();
    if (pManager == nullptr)
    {
        hr = E_OUTOFMEMORY;
        goto Cleanup;
    }

    //
    // Initialize — this creates the DXGI factory and enumerates adapters.
    //

    D2D_IFC(pManager->Init());

    //
    // Commit to the singleton slot.
    //

    s_pInstance = pManager;
    pManager = nullptr;

    *ppManager = s_pInstance;

Cleanup:
    delete pManager;

    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::Get
//
//  Synopsis:
//      Returns the singleton instance. Must be called after a successful
//      Create(). Returns nullptr if the singleton has not been created.
//
//------------------------------------------------------------------------------

/*static*/ CD2DDeviceManager*
CD2DDeviceManager::Get()
{
    return s_pInstance;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::Release
//
//  Synopsis:
//      Destroys the singleton instance and releases all managed devices.
//      After this call, Get() returns nullptr.
//
//------------------------------------------------------------------------------

/*static*/ void
CD2DDeviceManager::Release()
{
    if (s_pInstance != nullptr)
    {
        delete s_pInstance;
        s_pInstance = nullptr;
    }
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::Init
//
//  Synopsis:
//      Initializes the manager by creating an IDXGIFactory2 and enumerating
//      the available DXGI adapters. The adapter count is capped at
//      MAX_ADAPTERS.
//
//  Notes:
//      IDXGIFactory2 is required for CreateSwapChainForHwnd and other
//      modern swap chain creation paths.
//
//------------------------------------------------------------------------------

HRESULT
CD2DDeviceManager::Init()
{
    HRESULT hr = S_OK;

    //
    // Initialize the critical section for thread safety.
    //

    D2D_IFC(m_csManager.Init());

    //
    // Create the DXGI factory. We use CreateDXGIFactory1 which returns at
    // least IDXGIFactory1; we then QI for IDXGIFactory2 to get the modern
    // swap chain creation methods.
    //

    D2D_IFC(CreateDXGIFactory1(
        __uuidof(IDXGIFactory2),
        reinterpret_cast<void**>(m_pDXGIFactory.GetAddressOf())
        ));

    //
    // Enumerate adapters and record the count (capped at MAX_ADAPTERS).
    //

    {
        ComPtr<IDXGIAdapter> pAdapter;
        m_cAdapters = 0;

        while (m_cAdapters < MAX_ADAPTERS)
        {
            HRESULT hrEnum = m_pDXGIFactory->EnumAdapters(m_cAdapters, &pAdapter);
            if (hrEnum == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }

            D2D_IFC(hrEnum);

            //
            // We don't create devices eagerly — just count the adapters.
            // Devices are created on demand in GetD2DDeviceForAdapter().
            //

            pAdapter.Reset();
            m_cAdapters++;
        }
    }

    if (m_cAdapters == 0)
    {
        //
        // No adapters found. This is a fatal configuration error.
        //

        hr = DXGI_ERROR_NOT_FOUND;
        goto Cleanup;
    }

Cleanup:
    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::GetD2DDevice
//
//  Synopsis:
//      Maps a CDisplay to its DXGI adapter and returns the corresponding
//      CD2DDevice, creating one if necessary.
//
//      For this initial implementation, we map the display to adapter 0.
//      A full implementation would query the CDisplay for monitor info
//      and locate the matching DXGI output/adapter.
//
//  Arguments:
//      pDisplay - The display to get a device for.
//      ppDevice - Receives the CD2DDevice (AddRef'd).
//
//  Returns:
//      S_OK on success, or an appropriate HRESULT error code.
//
//------------------------------------------------------------------------------

HRESULT
CD2DDeviceManager::GetD2DDevice(
    __in_ecount(1) CDisplay const *pDisplay,
    __deref_out CD2DDevice **ppDevice
    )
{
    HRESULT hr = S_OK;

    if (pDisplay == nullptr || ppDevice == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppDevice = nullptr;

    //
    // Map the CDisplay to an adapter index.
    //
    // TODO: Implement proper display-to-adapter mapping using DXGI output
    //       enumeration and HMONITOR matching. For now, default to adapter 0.
    //

    {
        UINT adapterIndex = 0;

        //
        // Find the first HARDWARE adapter that has at least one output.
        // Skip any adapter flagged as software/WARP.
        //

        bool fFoundHW = false;

        for (UINT i = 0; i < m_cAdapters; i++)
        {
            ComPtr<IDXGIAdapter> pAdapter;
            HRESULT hrEnum = m_pDXGIFactory->EnumAdapters(i, &pAdapter);
            if (FAILED(hrEnum))
            {
                break;
            }

            //
            // Skip software/WARP adapters.
            //

            {
                ComPtr<IDXGIAdapter1> pAdapter1;
                if (SUCCEEDED(pAdapter.As(&pAdapter1)))
                {
                    DXGI_ADAPTER_DESC1 desc1 = {};
                    if (SUCCEEDED(pAdapter1->GetDesc1(&desc1)) &&
                        (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                    {
                        continue;   // skip WARP / Basic Render Driver
                    }
                }
            }

            //
            // Check if this adapter has at least one output (display).
            //

            ComPtr<IDXGIOutput> pOutput;
            hrEnum = pAdapter->EnumOutputs(0, &pOutput);
            if (hrEnum == DXGI_ERROR_NOT_FOUND)
            {
                //
                // No outputs — this adapter doesn't drive a display.
                // Still prefer it over WARP if we haven't found one yet.
                //

                if (!fFoundHW)
                {
                    adapterIndex = i;
                }
                continue;
            }
            if (FAILED(hrEnum))
            {
                continue;
            }

            //
            // Hardware adapter with at least one output — best choice.
            //

            adapterIndex = i;
            fFoundHW = true;
            break;
        }

        D2D_IFC(GetD2DDeviceForAdapter(adapterIndex, ppDevice));
    }

Cleanup:
    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::GetD2DDeviceForAdapter
//
//  Synopsis:
//      Returns the CD2DDevice for the specified adapter index. If no device
//      exists yet, or the existing device is invalid, a new one is created.
//
//  Arguments:
//      adapterIndex - Zero-based DXGI adapter index.
//      ppDevice     - Receives the CD2DDevice (AddRef'd).
//
//  Returns:
//      S_OK on success, or an appropriate HRESULT error code.
//
//  Thread Safety:
//      Protected by m_csManager critical section.
//
//------------------------------------------------------------------------------

HRESULT
CD2DDeviceManager::GetD2DDeviceForAdapter(
    UINT adapterIndex,
    __deref_out CD2DDevice **ppDevice
    )
{
    HRESULT hr = S_OK;

    if (ppDevice == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppDevice = nullptr;

    if (adapterIndex >= m_cAdapters)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    //
    // Take the lock to protect the device cache.
    //

    {
        CGuard<CCriticalSection> guard(m_csManager);

        //
        // If we already have a valid device for this adapter, return it.
        //

        if (m_rgDevices[adapterIndex] != nullptr &&
            m_rgDevices[adapterIndex]->IsValid())
        {
            m_rgDevices[adapterIndex]->AddRef();
            *ppDevice = m_rgDevices[adapterIndex];
            goto Cleanup;
        }

        //
        // The existing device is either null or invalid. Release the old
        // one (if any) and create a new one.
        //

        if (m_rgDevices[adapterIndex] != nullptr)
        {
            m_rgDevices[adapterIndex]->Release();
            m_rgDevices[adapterIndex] = nullptr;
        }

        D2D_IFC(CreateNewDevice(adapterIndex, ppDevice));

        //
        // Cache the new device for future lookups. AddRef for the cache.
        //

        (*ppDevice)->AddRef();
        m_rgDevices[adapterIndex] = *ppDevice;
    }

Cleanup:
    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::CreateNewDevice
//
//  Synopsis:
//      Creates a new CD2DDevice for the specified adapter index. The caller
//      receives an AddRef'd device.
//
//  Arguments:
//      adapterIndex - Zero-based DXGI adapter index.
//      ppDevice     - Receives the newly created CD2DDevice (AddRef'd).
//
//  Notes:
//      This method must be called under the m_csManager lock.
//
//------------------------------------------------------------------------------

HRESULT
CD2DDeviceManager::CreateNewDevice(
    UINT adapterIndex,
    __deref_out CD2DDevice **ppDevice
    )
{
    HRESULT hr = S_OK;
    ComPtr<IDXGIAdapter> pAdapter;

    if (ppDevice == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppDevice = nullptr;

    //
    // Get the DXGI adapter at the requested index.
    //

    D2D_IFC(m_pDXGIFactory->EnumAdapters(adapterIndex, &pAdapter));

    //
    // Create the CD2DDevice. Debug layer is disabled by default; a debug
    // build could enable it via a registry key or environment variable.
    //

    {
        bool fDebugLayer = false;

#if DBG
        fDebugLayer = true;
#endif

        D2D_IFC(CD2DDevice::Create(
            pAdapter.Get(),
            fDebugLayer,
            ppDevice
            ));
    }

Cleanup:
    return hr;
}

//+-----------------------------------------------------------------------------
//
//  Member:
//      CD2DDeviceManager::HandleDeviceLost
//
//  Synopsis:
//      Called when a device-lost condition is detected. Locates the lost device
//      in the cache by adapter LUID, invalidates it, and releases the cached
//      reference so that the next GetD2DDevice or GetD2DDeviceForAdapter call
//      creates a fresh device.
//
//  Arguments:
//      pLostDevice - The device that has been lost.
//
//------------------------------------------------------------------------------

void
CD2DDeviceManager::HandleDeviceLost(
    __in_ecount(1) CD2DDevice *pLostDevice
    )
{
    if (pLostDevice == nullptr)
    {
        return;
    }

    CGuard<CCriticalSection> guard(m_csManager);

    LUID lostLuid = pLostDevice->GetAdapterLuid();

    for (UINT i = 0; i < m_cAdapters; i++)
    {
        if (m_rgDevices[i] != nullptr)
        {
            LUID deviceLuid = m_rgDevices[i]->GetAdapterLuid();

            if (deviceLuid.LowPart == lostLuid.LowPart &&
                deviceLuid.HighPart == lostLuid.HighPart)
            {
                //
                // Found the matching device. Invalidate it and release the
                // cached reference.
                //

                m_rgDevices[i]->Invalidate();
                m_rgDevices[i]->Release();
                m_rgDevices[i] = nullptr;

                break;
            }
        }
    }

    //
    // Attempt to re-enumerate adapters from the DXGI factory in case
    // the adapter configuration has changed (e.g., GPU hot-remove).
    // This is best-effort; failures are silently ignored.
    //

    {
        ComPtr<IDXGIAdapter> pAdapter;
        UINT cAdapters = 0;

        while (cAdapters < MAX_ADAPTERS)
        {
            HRESULT hrEnum = m_pDXGIFactory->EnumAdapters(cAdapters, &pAdapter);
            if (hrEnum == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(hrEnum))
            {
                break;
            }
            pAdapter.Reset();
            cAdapters++;
        }

        if (cAdapters > 0)
        {
            m_cAdapters = cAdapters;
        }
    }
}
