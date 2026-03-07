// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//-----------------------------------------------------------------------------
//
//  Description:
//      D2D Effect resource header.
//
//      CMilD2DEffectDuce receives a generic D2D built-in effect description
//      from managed code (CLSID + property bag + input descriptors) and
//      stores it for the D2D rendering pipeline to consume.
//
//      On the legacy (non-D2D) render path the effect is a no-op: the
//      element renders without any effect applied.
//
//-----------------------------------------------------------------------------

#pragma once

MtExtern(CMilD2DEffectDuce);

//
// Property entry stored after ProcessUpdate parses the variable payload.
//

struct D2DEffectPropertyEntry
{
    UINT32 Index;
    UINT32 Type;       // D2DEffectPropertyType enum value
    UINT32 DataSize;
    BYTE  *pData;      // Owned allocation, freed in destructor / clear
};

//
// Input entry stored after ProcessUpdate parses the variable payload.
//

struct D2DEffectInputEntry
{
    UINT32 InputIndex;
    UINT32 InputKind;  // 0 = brush, 1 = effect
    HMIL_RESOURCE hResource;
};


// Class: CMilD2DEffectDuce
class CMilD2DEffectDuce : public CMilEffectDuce
{
    friend class CResourceFactory;

    // Bridge functions for the D2D pipeline
    friend bool   D2DEffect_IsD2DEffect(__in CMilEffectDuce *pEffect);
    friend HRESULT D2DEffect_GetClsid(__in CMilEffectDuce *pEffect, __out GUID *pClsid);
    friend UINT32  D2DEffect_GetPropertyCount(__in CMilEffectDuce *pEffect);
    friend HRESULT D2DEffect_GetProperty(__in CMilEffectDuce *pEffect, UINT32 index,
                                         __out UINT32 *pType, __out const BYTE **ppData, __out UINT32 *pSize);
    friend UINT32  D2DEffect_GetInputCount(__in CMilEffectDuce *pEffect);
    friend HRESULT D2DEffect_GetInput(__in CMilEffectDuce *pEffect, UINT32 index,
                                      __out UINT32 *pKind, __out HMIL_RESOURCE *phResource);
    friend CMilSlaveHandleTable* D2DEffect_GetHandleTable(__in CMilEffectDuce *pEffect);
    friend CMilEffectDuce* D2DEffect_ResolveInputEffect(__in CMilEffectDuce *pEffect, HMIL_RESOURCE hResource);

protected:

    DECLARE_METERHEAP_CLEAR(ProcessHeap, Mt(CMilD2DEffectDuce));

    CMilD2DEffectDuce(__inout_ecount(1) CComposition* pComposition) : CMilEffectDuce(pComposition)
    {
        m_pComposition = pComposition;
        m_propertyCount = 0;
        m_pProperties = nullptr;
        m_inputCount = 0;
        m_pInputs = nullptr;
        m_padding = 0.0f;
        m_pHandleTable = nullptr;
        ZeroMemory(&m_clsid, sizeof(m_clsid));
    }

    ~CMilD2DEffectDuce();

public:

    __override virtual bool IsOfType(MIL_RESOURCE_TYPE type) const
    {
        return type == TYPE_D2DEFFECT || CMilEffectDuce::IsOfType(type);
    }

    HRESULT ProcessUpdate(
        __in_ecount(1) CMilSlaveHandleTable* pHandleTable,
        __in_ecount(1) const MILCMD_D2DEFFECT* pCmd,
        __in_bcount_opt(cbPayload) LPCVOID pPayload,
        UINT cbPayload
        );

    //
    // Legacy render path — no-op.  The element renders without effect.
    //

    override HRESULT ApplyEffect(
        __in CContextState *pContextState,
        __in CHwSurfaceRenderTarget *pDestRT,
        __in CMILMatrix *pScaleTransform,
        __in CD3DDeviceLevel1 *pDevice,
        UINT uIntermediateWidth,
        UINT uIntermediateHeight,
        __in_opt CHwTextureRenderTarget *pImplicitInput
        );

    override HRESULT ApplyEffectSw(
        __in CContextState *pContextState,
        __in CSwRenderTargetSurface *pDestRT,
        __in CMILMatrix *pScaleTransform,
        UINT uIntermediateWidth,
        UINT uIntermediateHeight,
        __in_opt IWGXBitmap *pImplicitInput
        );

    override HRESULT PrepareSoftwarePass(
        __in const CMatrix<CoordinateSpace::RealizationSampling,CoordinateSpace::DeviceHPC> *pRealizationSamplingToDevice,
        __inout CPixelShaderState *pPixelShaderState,
        __deref_out CPixelShaderCompiler **ppPixelShaderCompiler
        )
    {
        RRETURN(E_UNEXPECTED);
    }

    override HRESULT TransformBoundsForInflation(__inout CMilRectF *bounds);

    //
    // Accessors for D2D rendering pipeline
    //

    const GUID& GetClsid() const { return m_clsid; }
    UINT32 GetPropertyCount() const { return m_propertyCount; }
    const D2DEffectPropertyEntry* GetProperties() const { return m_pProperties; }
    UINT32 GetInputCount() const { return m_inputCount; }
    const D2DEffectInputEntry* GetInputs() const { return m_pInputs; }

private:

    void ClearProperties();
    void ClearInputs();

    CComposition *m_pComposition;
    GUID m_clsid;
    FLOAT m_padding;

    UINT32 m_propertyCount;
    D2DEffectPropertyEntry *m_pProperties;

    UINT32 m_inputCount;
    D2DEffectInputEntry *m_pInputs;

    CMilSlaveHandleTable *m_pHandleTable;
};
