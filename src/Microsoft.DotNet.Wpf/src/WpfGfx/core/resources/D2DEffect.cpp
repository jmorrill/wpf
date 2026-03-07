// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//  Description:
//      D2D Effect resource implementation.
//
//      Receives MILCMD_D2DEFFECT from managed code, parses the fixed header
//      and variable-length property + input payloads, and stores them for
//      the D2D rendering pipeline.
//
//      On the legacy (non-D2D) render path every virtual is a no-op so
//      the element renders normally.
//
//----------------------------------------------------------------------------

#include "precomp.hpp"

MtDefine(CMilD2DEffectDuce, MILRender, "CMilD2DEffectDuce");

//+------------------------------------------------------------------------
//
//  Destructor
//
//-------------------------------------------------------------------------

CMilD2DEffectDuce::~CMilD2DEffectDuce()
{
    ClearProperties();
    ClearInputs();
}

//+------------------------------------------------------------------------
//
//  Method:   ClearProperties
//
//  Synopsis: Free all property data allocations.
//
//-------------------------------------------------------------------------

void
CMilD2DEffectDuce::ClearProperties()
{
    if (m_pProperties)
    {
        for (UINT32 i = 0; i < m_propertyCount; i++)
        {
            WPFFree(ProcessHeap, m_pProperties[i].pData);
        }
        WPFFree(ProcessHeap, m_pProperties);
        m_pProperties = nullptr;
    }
    m_propertyCount = 0;
}

//+------------------------------------------------------------------------
//
//  Method:   ClearInputs
//
//  Synopsis: Free the input array.
//
//-------------------------------------------------------------------------

void
CMilD2DEffectDuce::ClearInputs()
{
    if (m_pInputs)
    {
        WPFFree(ProcessHeap, m_pInputs);
        m_pInputs = nullptr;
    }
    m_inputCount = 0;
}

//+------------------------------------------------------------------------
//
//  Method:   ProcessUpdate
//
//  Synopsis: Parse MILCMD_D2DEFFECT fixed header plus variable-length
//            property and input payloads.
//
//  Layout of variable payload (all little-endian):
//    Properties (repeated PropertyCount times):
//      UINT32 index
//      UINT32 type
//      UINT32 dataSize
//      BYTE   data[dataSize]
//
//    Inputs (repeated InputCount times):
//      UINT32 inputIndex
//      UINT32 inputKind   (0=brush, 1=effect)
//      UINT32 hResource   (HMIL_RESOURCE handle)
//
//-------------------------------------------------------------------------

HRESULT
CMilD2DEffectDuce::ProcessUpdate(
    __in_ecount(1) CMilSlaveHandleTable* pHandleTable,
    __in_ecount(1) const MILCMD_D2DEFFECT* pCmd,
    __in_bcount_opt(cbPayload) LPCVOID pPayload,
    UINT cbPayload
    )
{
    HRESULT hr = S_OK;

    // Store the handle table for use during rendering (effect chaining).
    m_pHandleTable = pHandleTable;

    //
    // Free previous data.
    //

    ClearProperties();
    ClearInputs();

    //
    // Copy fixed fields.
    //

    m_clsid = pCmd->EffectClsid;
    m_padding = pCmd->Padding;

    //
    // Parse property payload.
    //

    UINT32 propertyCount = pCmd->PropertyCount;
    UINT32 propertyDataSize = pCmd->PropertyDataSize;

    const BYTE *pData = static_cast<const BYTE*>(pPayload);
    UINT cbRemaining = cbPayload;

    if (propertyCount > 0)
    {
        // Validate that the declared property data size doesn't exceed
        // the total payload.
        if (propertyDataSize > cbRemaining)
        {
            IFC(WGXERR_UCE_MALFORMEDPACKET);
        }

        m_pProperties = (D2DEffectPropertyEntry*)WPFAllocClear(
            ProcessHeap, Mt(CMilD2DEffectDuce),
            sizeof(D2DEffectPropertyEntry) * propertyCount);
        IFCOOM(m_pProperties);
        m_propertyCount = propertyCount;

        for (UINT32 i = 0; i < propertyCount; i++)
        {
            // Each entry header: 3 × UINT32 = 12 bytes
            if (cbRemaining < 12)
            {
                IFC(WGXERR_UCE_MALFORMEDPACKET);
            }

            const UINT32 *pHeader = reinterpret_cast<const UINT32*>(pData);
            m_pProperties[i].Index   = pHeader[0];
            m_pProperties[i].Type    = pHeader[1];
            m_pProperties[i].DataSize = pHeader[2];

            pData += 12;
            cbRemaining -= 12;

            UINT32 dataSize = m_pProperties[i].DataSize;
            if (dataSize > cbRemaining)
            {
                IFC(WGXERR_UCE_MALFORMEDPACKET);
            }

            if (dataSize > 0)
            {
                m_pProperties[i].pData = (BYTE*)WPFAlloc(ProcessHeap, Mt(CMilD2DEffectDuce), dataSize);
                IFCOOM(m_pProperties[i].pData);
                memcpy(m_pProperties[i].pData, pData, dataSize);
            }
            else
            {
                m_pProperties[i].pData = nullptr;
            }

            pData += dataSize;
            cbRemaining -= dataSize;
        }
    }

    //
    // Parse input payload.
    //

    UINT32 inputCount = pCmd->InputCount;

    if (inputCount > 0)
    {
        // Each input entry: 3 × UINT32 = 12 bytes
        UINT32 inputDataSize = inputCount * 12;
        if (inputDataSize > cbRemaining)
        {
            IFC(WGXERR_UCE_MALFORMEDPACKET);
        }

        m_pInputs = (D2DEffectInputEntry*)WPFAllocClear(
            ProcessHeap, Mt(CMilD2DEffectDuce),
            sizeof(D2DEffectInputEntry) * inputCount);
        IFCOOM(m_pInputs);
        m_inputCount = inputCount;

        for (UINT32 i = 0; i < inputCount; i++)
        {
            const UINT32 *pEntry = reinterpret_cast<const UINT32*>(pData);
            m_pInputs[i].InputIndex = pEntry[0];
            m_pInputs[i].InputKind  = pEntry[1];
            m_pInputs[i].hResource  = static_cast<HMIL_RESOURCE>(pEntry[2]);

            pData += 12;
            cbRemaining -= 12;
        }
    }

    //
    // Notify composition that this resource has been updated.
    //

    NotifyOnChanged(this);

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Method:   ApplyEffect
//
//  Synopsis: Legacy D3D render path – no-op.  Drawing the implicit input
//            unmodified is handled by the caller when we return S_OK
//            without rendering anything ourselves.
//
//-------------------------------------------------------------------------

HRESULT
CMilD2DEffectDuce::ApplyEffect(
    __in CContextState *pContextState,
    __in CHwSurfaceRenderTarget *pDestRT,
    __in CMILMatrix *pScaleTransform,
    __in CD3DDeviceLevel1 *pDevice,
    UINT uIntermediateWidth,
    UINT uIntermediateHeight,
    __in_opt CHwTextureRenderTarget *pImplicitInput
    )
{
    //
    // D2D effects are not supported on the legacy render path.
    // Return S_OK so the element still renders (without the effect).
    //

    UNREFERENCED_PARAMETER(pContextState);
    UNREFERENCED_PARAMETER(pDestRT);
    UNREFERENCED_PARAMETER(pScaleTransform);
    UNREFERENCED_PARAMETER(pDevice);
    UNREFERENCED_PARAMETER(uIntermediateWidth);
    UNREFERENCED_PARAMETER(uIntermediateHeight);
    UNREFERENCED_PARAMETER(pImplicitInput);

    return S_OK;
}

//+------------------------------------------------------------------------
//
//  Method:   ApplyEffectSw
//
//  Synopsis: Software render path – no-op.
//
//-------------------------------------------------------------------------

HRESULT
CMilD2DEffectDuce::ApplyEffectSw(
    __in CContextState *pContextState,
    __in CSwRenderTargetSurface *pDestRT,
    __in CMILMatrix *pScaleTransform,
    UINT uIntermediateWidth,
    UINT uIntermediateHeight,
    __in_opt IWGXBitmap *pImplicitInput
    )
{
    UNREFERENCED_PARAMETER(pContextState);
    UNREFERENCED_PARAMETER(pDestRT);
    UNREFERENCED_PARAMETER(pScaleTransform);
    UNREFERENCED_PARAMETER(uIntermediateWidth);
    UNREFERENCED_PARAMETER(uIntermediateHeight);
    UNREFERENCED_PARAMETER(pImplicitInput);

    return S_OK;
}

//+------------------------------------------------------------------------
//
//  Method:   TransformBoundsForInflation
//
//  Synopsis: Returns bounds unmodified — generic D2D effects do not
//            inflate bounds by default.
//
//-------------------------------------------------------------------------

HRESULT
CMilD2DEffectDuce::TransformBoundsForInflation(__inout CMilRectF *bounds)
{
    if (m_padding > 0.0f)
    {
        bounds->left   -= m_padding;
        bounds->top    -= m_padding;
        bounds->right  += m_padding;
        bounds->bottom += m_padding;
    }
    return S_OK;
}
