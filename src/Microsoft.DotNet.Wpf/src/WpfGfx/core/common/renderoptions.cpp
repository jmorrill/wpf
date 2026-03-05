// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+--------------------------------------------------------------------------
//

//
//----------------------------------------------------------------------------

#include "precomp.hpp"

void WINAPI
RenderOptions_ForceSoftwareRenderingModeForProcess(BOOL fForce)
{
    RenderOptions::ForceSoftwareRenderingForProcess(fForce);
}

BOOL WINAPI
RenderOptions_IsSoftwareRenderingForcedForProcess()
{
    return RenderOptions::IsSoftwareRenderingForcedForProcess();
}

void WINAPI
RenderOptions_EnableHardwareAccelerationInRdp(BOOL fEnable)
{
    RenderOptions::EnableHardwareAccelerationInRdp(fEnable);
}

// m_cs must be entered before accessing m_fForceSoftware because multiple
// managed threads plus the render thread could try to access it
static CCriticalSection m_cs;
static bool m_fForceSoftware;
static bool m_fHwAccelerationInRdpEnabled;
#ifdef WPF_D2D_ENABLED
static bool m_fD2DRenderingEnabled;
#endif

//+---------------------------------------------------------------------------------
//
//  RenderOptions::Init
//
//  Synopsis:   Initializes the RenderOptions. This is not thread safe and should only be called once
//                  at DLL load.
//
//----------------------------------------------------------------------------------

HRESULT 
RenderOptions::Init()
{
    m_fForceSoftware = false;
    m_fHwAccelerationInRdpEnabled = false;
#ifdef WPF_D2D_ENABLED
    m_fD2DRenderingEnabled = false;

    //
    // Check the WPF_USE_D2D environment variable.  Setting WPF_USE_D2D=1
    // opts the process into the D2D/D3D11 rendering backend without
    // requiring managed P/Invoke.  The hardware capability probe still
    // runs later via D2DRenderOptions::IsD2DRenderingSupported().
    //
    {
        WCHAR buf[16];
        DWORD len = GetEnvironmentVariableW(L"WPF_USE_D2D", buf, ARRAYSIZE(buf));
        if (len > 0 && len < ARRAYSIZE(buf) && buf[0] == L'1')
        {
            m_fD2DRenderingEnabled = true;
        }
    }
#endif
    RRETURN(m_cs.Init());
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions::DeInit
//
//  Synopsis:   Deinits the RenderOptions. This is not thread safe and should only be called once
//                  at DLL unload
//
//----------------------------------------------------------------------------------

void
RenderOptions::DeInit()
{
    m_cs.DeInit();
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions::ForceSoftwareRenderingForProcess
//
//  Synopsis:   Sets whether or not we should force all render targets to software for the process
//
//                  Note: this uses BOOL not bool because fForce is marshalled from managed code
//
//----------------------------------------------------------------------------------

void 
RenderOptions::ForceSoftwareRenderingForProcess(BOOL fForce)
{
    CGuard<CCriticalSection> guard(m_cs);
    m_fForceSoftware = !!fForce;
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions::IsSoftwareRenderingForcedForProcess
//
//  Synopsis:   Sets whether or not we should force all render targets to software for the process
//
//                  Note: this returns BOOL not bool to facilitate managed marshalling
//
//----------------------------------------------------------------------------------

BOOL 
RenderOptions::IsSoftwareRenderingForcedForProcess()
{
    CGuard<CCriticalSection> guard(m_cs);
    
    return m_fForceSoftware;
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions::EnableGraphicHWAccelerationInRdp
//
//  Synopsis:   Sets whether or not Hardware Acceleration should be enabled for RDP.
//
//----------------------------------------------------------------------------------
void
RenderOptions::EnableHardwareAccelerationInRdp(BOOL fEnable)
{
    CGuard<CCriticalSection> guard(m_cs);
    m_fHwAccelerationInRdpEnabled = !!fEnable;
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions::IsHardwareAccelerationInRdpEnabled
//
//  Synopsis:   return whether or not Hardware Acceleration for RDP is enabled.
//
//----------------------------------------------------------------------------------
BOOL
RenderOptions::IsHardwareAccelerationInRdpEnabled()
{
    return m_fHwAccelerationInRdpEnabled;
}

#ifdef WPF_D2D_ENABLED

//+---------------------------------------------------------------------------------
//
//  RenderOptions_SetD2DRenderingEnabled
//
//  Synopsis:   Exported function to enable/disable D2D rendering path.
//
//----------------------------------------------------------------------------------
void WINAPI
RenderOptions_SetD2DRenderingEnabled(BOOL fEnabled)
{
    RenderOptions::SetD2DRenderingEnabled(!!fEnabled);
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions_IsD2DRenderingEnabled
//
//  Synopsis:   Exported function to query D2D rendering state.
//
//----------------------------------------------------------------------------------
BOOL WINAPI
RenderOptions_IsD2DRenderingEnabled()
{
    return RenderOptions::IsD2DRenderingEnabled();
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions::SetD2DRenderingEnabled
//
//  Synopsis:   Sets whether D2D rendering path should be used.
//
//----------------------------------------------------------------------------------
void
RenderOptions::SetD2DRenderingEnabled(bool fEnabled)
{
    CGuard<CCriticalSection> guard(m_cs);
    m_fD2DRenderingEnabled = fEnabled;
}

//+---------------------------------------------------------------------------------
//
//  RenderOptions::IsD2DRenderingEnabled
//
//  Synopsis:   Returns whether D2D rendering path is enabled.
//
//----------------------------------------------------------------------------------
bool
RenderOptions::IsD2DRenderingEnabled()
{
    CGuard<CCriticalSection> guard(m_cs);
    return m_fD2DRenderingEnabled;
}

#endif // WPF_D2D_ENABLED

