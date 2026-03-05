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
//      Contains CD2DDeviceManager declaration.
//
//      CD2DDeviceManager is a singleton that manages CD2DDevice instances
//      per DXGI adapter. It is analogous to CD3DDeviceManager in the D3D9
//      backend.
//
//      The manager owns an IDXGIFactory2 used to enumerate adapters and
//      create modern swap chains. Device instances are cached per adapter
//      index and recreated on device-lost.
//
//  $ENDTAG
//
//------------------------------------------------------------------------------

#pragma once

#include "d2d_precomp.h"
#include "d2d_device.h"

using Microsoft::WRL::ComPtr;

//
// Forward declarations for WPF types referenced by the manager.
//

class CDisplay;

//+-----------------------------------------------------------------------------
//
//  Class:
//      CD2DDeviceManager
//
//  Synopsis:
//      Singleton manager that creates and caches CD2DDevice instances per
//      DXGI adapter. Provides device lookup by CDisplay or adapter index,
//      and handles device-lost recovery.
//
//      Thread safety is provided by CCriticalSection / CGuard, matching the
//      existing WPF pattern used by CD3DDeviceManager.
//
//------------------------------------------------------------------------------

class CD2DDeviceManager
{
public:

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      Create
    //
    //  Synopsis:
    //      Creates and initializes the singleton instance.
    //
    //--------------------------------------------------------------------------

    static HRESULT Create(
        __deref_out CD2DDeviceManager **ppManager
        );

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      Get
    //
    //  Synopsis:
    //      Returns the singleton instance. Must be called after Create().
    //
    //--------------------------------------------------------------------------

    static CD2DDeviceManager* Get();

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      Release
    //
    //  Synopsis:
    //      Destroys the singleton instance and releases all managed devices.
    //
    //--------------------------------------------------------------------------

    static void Release();

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      GetD2DDevice
    //
    //  Synopsis:
    //      Maps a CDisplay to its DXGI adapter and returns the corresponding
    //      CD2DDevice, creating one if necessary. The returned device is
    //      AddRef'd; the caller must release it.
    //
    //--------------------------------------------------------------------------

    HRESULT GetD2DDevice(
        __in_ecount(1) CDisplay const *pDisplay,
        __deref_out CD2DDevice **ppDevice
        );

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      GetD2DDeviceForAdapter
    //
    //  Synopsis:
    //      Returns the CD2DDevice for the specified adapter index, creating
    //      one if necessary. The returned device is AddRef'd.
    //
    //--------------------------------------------------------------------------

    HRESULT GetD2DDeviceForAdapter(
        UINT adapterIndex,
        __deref_out CD2DDevice **ppDevice
        );

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      HandleDeviceLost
    //
    //  Synopsis:
    //      Invalidates and releases the lost device so that the next call to
    //      GetD2DDevice or GetD2DDeviceForAdapter creates a fresh device.
    //
    //--------------------------------------------------------------------------

    void HandleDeviceLost(
        __in_ecount(1) CD2DDevice *pLostDevice
        );

private:

    CD2DDeviceManager();
    ~CD2DDeviceManager();

    //
    // Non-copyable
    //

    CD2DDeviceManager(const CD2DDeviceManager&);
    CD2DDeviceManager& operator=(const CD2DDeviceManager&);

    HRESULT Init();

    HRESULT CreateNewDevice(
        UINT adapterIndex,
        __deref_out CD2DDevice **ppDevice
        );

    //
    // Singleton instance
    //

    static CD2DDeviceManager *s_pInstance;

    //
    // DXGI factory for adapter enumeration and swap chain creation
    //

    ComPtr<IDXGIFactory2> m_pDXGIFactory;

    //
    // Per-adapter device cache. We support up to MAX_ADAPTERS GPUs.
    //

    static const UINT MAX_ADAPTERS = 16;
    CD2DDevice* m_rgDevices[MAX_ADAPTERS];
    UINT m_cAdapters;

    //
    // Thread safety — matches the CCriticalSection / CGuard pattern used
    // in CD3DDeviceManager.
    //

    CCriticalSection m_csManager;
};
