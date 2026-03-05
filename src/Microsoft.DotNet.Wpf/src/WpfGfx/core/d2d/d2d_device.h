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
//      Contains CD2DDevice declaration.
//
//      CD2DDevice wraps an ID3D11Device, IDXGIDevice, ID2D1Device1,
//      ID2D1Factory1, and IDWriteFactory1, providing a unified device
//      abstraction for the D2D/D3D11 rendering backend.
//
//      This is analogous to CD3DDeviceLevel1 in the D3D9 backend, but
//      significantly thinner since D2D1 handles much of the complexity
//      internally.
//
//  $ENDTAG
//
//------------------------------------------------------------------------------

#pragma once

using Microsoft::WRL::ComPtr;

//+-----------------------------------------------------------------------------
//
//  Class:
//      CD2DDevice
//
//  Synopsis:
//      Wraps an ID3D11Device1, IDXGIDevice, ID2D1Device1, ID2D1Factory1,
//      and IDWriteFactory1. Provides device creation, device context creation,
//      and adapter identification for the D2D/D3D11 rendering backend.
//
//      Reference counted via simple AddRef/Release pattern.
//
//------------------------------------------------------------------------------

class CD2DDevice
{
public:

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      Create
    //
    //  Synopsis:
    //      Factory method. Creates a new CD2DDevice wrapping a D3D11 device
    //      created on the specified DXGI adapter.
    //
    //  Arguments:
    //      pAdapter    - DXGI adapter to create the device on.
    //      fDebugLayer - If true, enable D3D11 debug layer.
    //      ppDevice    - Receives the newly created CD2DDevice.
    //
    //--------------------------------------------------------------------------

    static HRESULT Create(
        __in_ecount(1) IDXGIAdapter *pAdapter,
        bool fDebugLayer,
        __deref_out CD2DDevice **ppDevice
        );

    LONG AddRef();
    LONG Release();

    //+-------------------------------------------------------------------------
    //
    //  Accessors
    //
    //  Synopsis:
    //      Return internal COM interfaces. The returned pointers are NOT
    //      AddRef'd — the caller must not release them unless it explicitly
    //      AddRef's first.
    //
    //--------------------------------------------------------------------------

    ID3D11Device* GetD3D11Device() const;
    ID3D11Device1* GetD3D11Device1() const;
    ID2D1Device1* GetD2DDevice() const;
    ID2D1Factory1* GetD2DFactory() const;
    IDWriteFactory1* GetDWriteFactory() const;
    IDXGIDevice* GetDXGIDevice() const;

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      CreateDeviceContext
    //
    //  Synopsis:
    //      Creates a new ID2D1DeviceContext1 with multithreaded optimizations
    //      enabled. The caller owns the returned context.
    //
    //--------------------------------------------------------------------------

    HRESULT CreateDeviceContext(
        __deref_out ID2D1DeviceContext1 **ppContext
        );

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      GetD3D11ImmediateContext
    //
    //  Synopsis:
    //      Returns the D3D11 immediate context for 3D interop rendering.
    //      The returned pointer is NOT AddRef'd.
    //
    //--------------------------------------------------------------------------

    ID3D11DeviceContext* GetD3D11ImmediateContext() const;

    //+-------------------------------------------------------------------------
    //
    //  Members:
    //      IsValid / Invalidate
    //
    //  Synopsis:
    //      Device validity tracking. When a device-lost condition is detected,
    //      Invalidate() is called to mark this device as unusable. IsValid()
    //      returns the current validity state.
    //
    //--------------------------------------------------------------------------

    bool IsValid() const;
    void Invalidate();

    //+-------------------------------------------------------------------------
    //
    //  Member:
    //      GetAdapterLuid
    //
    //  Synopsis:
    //      Returns the LUID of the DXGI adapter this device was created on.
    //      Useful for identifying which GPU this device corresponds to.
    //
    //--------------------------------------------------------------------------

    LUID GetAdapterLuid() const;

private:

    CD2DDevice();
    ~CD2DDevice();

    //
    // Non-copyable
    //

    CD2DDevice(const CD2DDevice&);
    CD2DDevice& operator=(const CD2DDevice&);

    HRESULT Init(
        __in_ecount(1) IDXGIAdapter *pAdapter,
        bool fDebugLayer
        );

    LONG m_cRef;
    bool m_fValid;
    LUID m_adapterLuid;

    ComPtr<ID3D11Device1>         m_pD3D11Device;
    ComPtr<ID3D11DeviceContext>   m_pD3D11ImmediateContext;
    ComPtr<IDXGIDevice>           m_pDXGIDevice;
    ComPtr<ID2D1Factory1>         m_pD2DFactory;
    ComPtr<ID2D1Device1>          m_pD2DDevice;
    ComPtr<IDWriteFactory1>       m_pDWriteFactory;
};
