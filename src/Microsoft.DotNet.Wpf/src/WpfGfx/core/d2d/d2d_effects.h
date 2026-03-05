// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2DEffectPipeline declaration.
//
//      Maps WPF effects (Blur, DropShadow, custom ShaderEffect) to Direct2D
//      effect graphs.  D2D1 built-in effects provide hardware-accelerated
//      implementations; custom pixel shaders will be registered as D2D
//      custom effects in a later phase.
//
//----------------------------------------------------------------------------

#pragma once

// Forward declarations - effect types not in common PCH headers
class CMilEffectDuce;
class CMilBlurEffectDuce;
class CMilDropShadowEffectDuce;
class CMilShaderEffectDuce;

//+------------------------------------------------------------------------
//
//  Class: CD2DEffectPipeline
//
//  Synopsis: Maps WPF effects to Direct2D effect graphs.
//            Uses D2D1 built-in effects for blur, drop shadow.
//            Custom pixel shader effects use D2D custom effect interface.
//
//------------------------------------------------------------------------

class CD2DEffectPipeline
{
public:

    //+--------------------------------------------------------------------
    //
    //  Method:   ComposeEffect
    //
    //  Synopsis: Apply a WPF effect using a D2D1 effect graph.
    //
    //            Dispatches to the appropriate handler based on the
    //            concrete CMilEffectDuce subtype.
    //
    //---------------------------------------------------------------------

    static HRESULT ComposeEffect(
        __in ID2D1DeviceContext1 *pContext,
        __in ID2D1Factory1 *pFactory,
        __inout_ecount(1) CContextState *pContextState,
        __inout_ecount(1) CMILMatrix *pScaleTransform,
        __inout_ecount(1) CMilEffectDuce *pEffect,
        UINT uIntermediateWidth,
        UINT uIntermediateHeight,
        __in_opt IMILRenderTargetBitmap *pImplicitInput,
        __in_opt ID2D1Bitmap1 *pInputBitmap
    );

private:

    //+--------------------------------------------------------------------
    //
    //  Method:   ApplyBlurEffect
    //
    //  Synopsis: Create a D2D1 Gaussian blur effect and draw the result.
    //
    //---------------------------------------------------------------------

    static HRESULT ApplyBlurEffect(
        __in ID2D1DeviceContext1 *pContext,
        __in CMilBlurEffectDuce *pBlurEffect,
        __in CMILMatrix *pScaleTransform,
        __in ID2D1Bitmap1 *pSourceBitmap
    );

    //+--------------------------------------------------------------------
    //
    //  Method:   ApplyDropShadowEffect
    //
    //  Synopsis: Build a D2D1 shadow + offset + composite effect chain.
    //
    //---------------------------------------------------------------------

    static HRESULT ApplyDropShadowEffect(
        __in ID2D1DeviceContext1 *pContext,
        __in CMilDropShadowEffectDuce *pDropShadow,
        __in CMILMatrix *pScaleTransform,
        __in ID2D1Bitmap1 *pSourceBitmap
    );

    //+--------------------------------------------------------------------
    //
    //  Method:   ApplyShaderEffect
    //
    //  Synopsis: Handle custom ShaderEffect.  Initial implementation
    //            renders the source unmodified (graceful degradation).
    //
    //---------------------------------------------------------------------

    static HRESULT ApplyShaderEffect(
        __in ID2D1DeviceContext1 *pContext,
        __in CMilShaderEffectDuce *pShaderEffect,
        __in ID2D1Bitmap1 *pSourceBitmap
    );

    //
    // Utility: obtain an ID2D1Bitmap1 from an IMILRenderTargetBitmap.
    //

    static HRESULT GetBitmapFromImplicitInput(
        __in ID2D1DeviceContext1 *pContext,
        __in IMILRenderTargetBitmap *pImplicitInput,
        __deref_out ID2D1Bitmap1 **ppBitmap
    );

    CD2DEffectPipeline() = delete;
};

//+------------------------------------------------------------------------
//
//  Effect bridge functions
//
//  Synopsis: Free functions that provide access to WPF effect properties
//            without requiring the full resource headers.  Declarations
//            live here (D2D precomp) and implementations live in
//            resources/Effect.cpp (resources precomp with full headers).
//
//            Pattern mirrors the glyph bridge in GlyphRunCore.h /
//            glyphrunslave.cpp.
//
//------------------------------------------------------------------------

//
// Returns: 0 = Unknown, 1 = Blur, 2 = DropShadow, 3 = Shader
//

int D2DEffect_GetEffectType(__in CMilEffectDuce *pEffect);

//
// Blur effect properties
//

double D2DEffect_GetBlurRadius(__in CMilEffectDuce *pEffect);

//
// DropShadow effect properties
//

double D2DEffect_GetDropShadowBlurRadius(__in CMilEffectDuce *pEffect);
double D2DEffect_GetDropShadowDirection(__in CMilEffectDuce *pEffect);
double D2DEffect_GetDropShadowDepth(__in CMilEffectDuce *pEffect);
double D2DEffect_GetDropShadowOpacity(__in CMilEffectDuce *pEffect);
void   D2DEffect_GetDropShadowColor(
    __in CMilEffectDuce *pEffect,
    __out float *r,
    __out float *g,
    __out float *b,
    __out float *a
);
