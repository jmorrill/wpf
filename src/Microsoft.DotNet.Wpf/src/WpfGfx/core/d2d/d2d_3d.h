// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2D3DRenderer declaration.
//
//      This class renders WPF 3D content (Viewport3D, DrawMesh3D) using
//      Direct3D 11.  D2D1 is 2D-only, so 3D meshes are drawn by switching
//      to the D3D11 immediate context that shares the DXGI surface with
//      the D2D1 device context.
//
//      Integration flow:
//        1. CD2DSurfaceRenderTarget::Begin3D() flushes D2D, obtains D3D11
//           render-target / depth-stencil views from the shared DXGI surface.
//        2. DrawMesh3D() delegates to CD2D3DRenderer::DrawMesh().
//        3. CD2DSurfaceRenderTarget::End3D() flushes D3D11 and resumes D2D.
//
//  Notes:
//      The shader pipeline implements Blinn-Phong lighting matching the
//      visual output of the legacy D3D9 fixed-function path.  Shader
//      bytecode is compiled offline and embedded as static arrays.
//
//----------------------------------------------------------------------------

#pragma once

using namespace DirectX;

//+------------------------------------------------------------------------
//
//  Struct:  D2D_MeshVertex
//
//  Synopsis:  Vertex layout for WPF 3D meshes.  Matches the input-layout
//             expected by the embedded Blinn-Phong vertex shader.
//
//-------------------------------------------------------------------------

struct D2D_MeshVertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
};

//+------------------------------------------------------------------------
//
//  Struct:  D2D_3DConstants
//
//  Synopsis:  Constant buffer layout for the Blinn-Phong shader pair.
//             Must be a multiple of 16 bytes (D3D11 cbuffer alignment).
//
//  Size: 256 bytes (16 x XMFLOAT4)
//
//-------------------------------------------------------------------------

struct D2D_3DConstants
{
    XMFLOAT4X4 WorldViewProj;       // 64 bytes
    XMFLOAT4X4 World;               // 64 bytes
    XMFLOAT4   DiffuseColor;        // 16 bytes
    XMFLOAT4   SpecularColor;       // 16 bytes
    XMFLOAT4   EmissiveColor;       // 16 bytes
    XMFLOAT3   LightDirection;      // 12 bytes
    float       LightIntensity;     //  4 bytes
    XMFLOAT3   CameraPosition;      // 12 bytes
    float       SpecularPower;      //  4 bytes
    XMFLOAT4   AmbientColor;        // 16 bytes
    float       _padding[8];        // 32 bytes padding to reach 256
};

static_assert(sizeof(D2D_3DConstants) == 256, "D2D_3DConstants must be 256 bytes for cbuffer alignment");

//+------------------------------------------------------------------------
//
//  Class:  CD2D3DRenderer
//
//  Synopsis:  Renders WPF 3D content (Viewport3D) using Direct3D 11.
//             Shares the DXGI surface with D2D1 for seamless 2D / 3D
//             compositing.
//
//  Ownership: Created on demand by CD2DSurfaceRenderTarget and held
//             for the lifetime of the render target.  Ref-counted so it
//             can be shared if multiple render targets use the same D3D11
//             device.
//
//  Thread:    UI / composition thread only (single-threaded).
//
//------------------------------------------------------------------------

class CD2D3DRenderer
{
public:

    //+--------------------------------------------------------------------
    //
    //  Function:  CD2D3DRenderer::Create
    //
    //  Synopsis:  Factory method.  Allocates the renderer, creates the
    //             D3D11 pipeline state objects (shaders, input layout,
    //             constant buffer, rasteriser / blend / depth-stencil
    //             states).
    //
    //---------------------------------------------------------------------

    static HRESULT Create(
        __in ID3D11Device *pDevice,
        __deref_out CD2D3DRenderer **ppRenderer
    );

    LONG AddRef();
    LONG Release();

    //+--------------------------------------------------------------------
    //
    //  Function:  Begin3D
    //
    //  Synopsis:  Bind the provided render-target and optional depth-stencil
    //             views and set the viewport.  Called after the caller has
    //             flushed D2D and obtained the DXGI shared surface views.
    //
    //---------------------------------------------------------------------

    HRESULT Begin3D(
        __in ID3D11RenderTargetView *pRTV,
        __in_opt ID3D11DepthStencilView *pDSV,
        UINT width, UINT height,
        const MilRectF &rcBounds,
        bool fUseZBuffer,
        FLOAT rZ
    );

    //+--------------------------------------------------------------------
    //
    //  Function:  DrawMesh
    //
    //  Synopsis:  Render a single WPF 3D mesh with Blinn-Phong shading.
    //             Extracts vertex data from CMILMesh3D, material / light
    //             parameters from CMILShader, and issues an indexed draw
    //             call through the D3D11 immediate context.
    //
    //---------------------------------------------------------------------

    HRESULT DrawMesh(
        __in CContextState *pContextState,
        __in_opt BrushContext *pBrushContext,
        __in CMILMesh3D *pMesh3D,
        __in_opt CMILShader *pShader,
        __in_opt IMILEffectList *pIEffect
    );

    //+--------------------------------------------------------------------
    //
    //  Function:  End3D
    //
    //  Synopsis:  Unbind D3D11 render targets and restore state so D2D
    //             can resume drawing on the shared surface.
    //
    //---------------------------------------------------------------------

    HRESULT End3D();

private:

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    CD2D3DRenderer();
    ~CD2D3DRenderer();

    // Second-phase initialisation
    HRESULT Init(__in ID3D11Device *pDevice);

    // Pipeline state object creation
    HRESULT CreateShaders();
    HRESULT CreateInputLayout();
    HRESULT CreateConstantBuffer();
    HRESULT CreateRasterizerState();
    HRESULT CreateBlendState();
    HRESULT CreateDepthStencilStates();

    // -----------------------------------------------------------------------
    // Per-draw helpers
    // -----------------------------------------------------------------------

    //+--------------------------------------------------------------------
    //
    //  Function:  UploadMesh
    //
    //  Synopsis:  Read vertex positions, normals, tex-coords and triangle
    //             indices from CMILMesh3D, assemble D2D_MeshVertex data,
    //             and create GPU vertex / index buffers.
    //
    //---------------------------------------------------------------------

    HRESULT UploadMesh(
        __in CMILMesh3D *pMesh3D,
        __deref_out ID3D11Buffer **ppVertexBuffer,
        __deref_out ID3D11Buffer **ppIndexBuffer,
        __out UINT *pIndexCount
    );

    //+--------------------------------------------------------------------
    //
    //  Function:  ExtractMaterialParameters
    //
    //  Synopsis:  Walk the CMILShader material / light lists and populate
    //             D2D_3DConstants with diffuse, specular, emissive colours,
    //             light direction/intensity, camera position, etc.
    //
    //---------------------------------------------------------------------

    HRESULT ExtractMaterialParameters(
        __in_opt CMILShader *pShader,
        __in_opt CContextState *pContextState,
        __out D2D_3DConstants *pConstants
    );

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    LONG m_cRef;
    bool m_fIn3D;

    //
    // D3D11 device and immediate context
    //

    ComPtr<ID3D11Device>               m_pDevice;
    ComPtr<ID3D11DeviceContext>         m_pContext;

    //
    // Shader pipeline
    //

    ComPtr<ID3D11VertexShader>         m_pVertexShader;
    ComPtr<ID3D11PixelShader>          m_pPixelShader;
    ComPtr<ID3D11InputLayout>          m_pInputLayout;
    ComPtr<ID3D11Buffer>               m_pConstantBuffer;

    //
    // Fixed-function pipeline state objects
    //

    ComPtr<ID3D11RasterizerState>      m_pRasterizerState;
    ComPtr<ID3D11BlendState>           m_pBlendState;
    ComPtr<ID3D11DepthStencilState>    m_pDepthStencilStateEnabled;
    ComPtr<ID3D11DepthStencilState>    m_pDepthStencilStateDisabled;

    //
    // Current frame render state (valid between Begin3D / End3D)
    //

    ID3D11RenderTargetView            *m_pCurrentRTV;
    ID3D11DepthStencilView            *m_pCurrentDSV;
    D3D11_VIEWPORT                     m_currentViewport;
    bool                               m_fDepthEnabled;
};
