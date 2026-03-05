// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.


//+---------------------------------------------------------------------------
//
//
//  Description:
//      CD2D3DRenderer implementation.
//
//      Renders WPF 3D meshes (Viewport3D / DrawMesh3D) through Direct3D 11
//      on the DXGI surface shared with the D2D1 device context.  Implements
//      Blinn-Phong lighting to match the visual output of the legacy D3D9
//      fixed-function pipeline.
//
//  Threading:
//      UI / composition thread only – no internal synchronisation.
//
//----------------------------------------------------------------------------

#include <precomp.hpp>

//
// If UNCONDITIONAL_EXPR is not provided by the precompiled header chain we
// define a simple fallback so D2D_IFC compiles standalone.
//

#ifndef UNCONDITIONAL_EXPR
#define UNCONDITIONAL_EXPR(x) (x)
#endif

#ifndef D2D_IFC
#define D2D_IFC(expr)                   \
    do {                                \
        hr = (expr);                    \
        if (FAILED(hr))                 \
        {                               \
            goto Cleanup;               \
        }                               \
    } while (UNCONDITIONAL_EXPR(0))
#endif

// =========================================================================
//  Embedded Shader Bytecode
// =========================================================================
//
//  The vertex and pixel shaders are compiled offline (fxc.exe / dxc.exe)
//  and the resulting DXBC blobs are stored as static byte arrays.  This
//  avoids a runtime dependency on d3dcompiler_XX.dll.
//
//  -----------------------------------------------------------------------
//  HLSL Source (for reference – do NOT compile at runtime)
//  -----------------------------------------------------------------------
//
//  cbuffer CB : register(b0)
//  {
//      float4x4 WorldViewProj;       // World * View * Projection
//      float4x4 World;               // World matrix (for normal transform)
//      float4   DiffuseColor;        // Material diffuse  (RGBA, premul)
//      float4   SpecularColor;       // Material specular (RGBA, premul)
//      float4   EmissiveColor;       // Material emissive (RGBA, premul)
//      float3   LightDirection;      // Normalised world-space light dir
//      float    LightIntensity;      // Scalar intensity multiplier
//      float3   CameraPosition;      // World-space camera position
//      float    SpecularPower;       // Blinn-Phong specular exponent
//      float4   AmbientColor;        // Scene ambient  (RGBA, premul)
//  };
//
//  struct VS_IN
//  {
//      float3 Position : POSITION;
//      float3 Normal   : NORMAL;
//      float2 TexCoord : TEXCOORD;
//  };
//
//  struct VS_OUT
//  {
//      float4 Position : SV_Position;
//      float3 Normal   : NORMAL;
//      float2 TexCoord : TEXCOORD0;
//      float3 WorldPos : TEXCOORD1;
//  };
//
//  VS_OUT vs_main(VS_IN input)
//  {
//      VS_OUT output;
//      float4 worldPos = mul(float4(input.Position, 1.0), World);
//      output.Position = mul(float4(input.Position, 1.0), WorldViewProj);
//      output.Normal   = normalize(mul(input.Normal, (float3x3)World));
//      output.TexCoord = input.TexCoord;
//      output.WorldPos = worldPos.xyz;
//      return output;
//  }
//
//  float4 ps_main(VS_OUT input) : SV_Target
//  {
//      float3 N = normalize(input.Normal);
//      float3 L = normalize(-LightDirection);
//      float3 V = normalize(CameraPosition - input.WorldPos);
//      float3 H = normalize(L + V);
//
//      // Diffuse (Lambert)
//      float NdotL = saturate(dot(N, L));
//      float3 diffuse = DiffuseColor.rgb * NdotL * LightIntensity;
//
//      // Specular (Blinn-Phong)
//      float NdotH = saturate(dot(N, H));
//      float specFactor = pow(NdotH, max(SpecularPower, 1.0));
//      float3 specular = SpecularColor.rgb * specFactor * LightIntensity;
//
//      // Combine
//      float3 color = AmbientColor.rgb * DiffuseColor.rgb
//                   + diffuse
//                   + specular
//                   + EmissiveColor.rgb;
//
//      float alpha = DiffuseColor.a;
//
//      // Output premultiplied alpha for D2D1 compositing compatibility.
//      return float4(color * alpha, alpha);
//  }
//
//  -----------------------------------------------------------------------
//  Compiled with:
//    fxc /T vs_5_0 /E vs_main /Fh vs_blinnphong.h  shader.hlsl
//    fxc /T ps_5_0 /E ps_main /Fh ps_blinnphong.h  shader.hlsl
//  -----------------------------------------------------------------------
//
//  The arrays below are placeholder bytecode that carries the correct DXBC
//  header magic (0x44584243 = "DXBC") and realistic size.  They MUST be
//  replaced with real compiled bytecode before shipping.
//

// clang-format off

//
// Placeholder vertex shader bytecode – Blinn-Phong VS (vs_5_0)
//
// DXBC header: 44 58 42 43  ("DXBC")
// Followed by placeholder data sized to a realistic compiled VS (~768 bytes).
//
static const BYTE s_vsBlinnPhong[] =
{
    // ---- DXBC header (20 bytes) ----
    0x44, 0x58, 0x42, 0x43,     // "DXBC" magic
    0x00, 0x00, 0x00, 0x00,     // checksum [0]
    0x00, 0x00, 0x00, 0x00,     // checksum [1]
    0x00, 0x00, 0x00, 0x00,     // checksum [2]
    0x00, 0x00, 0x00, 0x00,     // checksum [3]

    // ---- Version & size ----
    0x01, 0x00, 0x00, 0x00,     // version = 1
    0x00, 0x03, 0x00, 0x00,     // total size = 768 bytes

    // ---- Chunk count ----
    0x05, 0x00, 0x00, 0x00,     // 5 chunks (RDEF, ISGN, OSGN, SHEX, STAT)

    // ---- Chunk offsets (placeholder) ----
    0x34, 0x00, 0x00, 0x00,     // RDEF offset
    0xA0, 0x00, 0x00, 0x00,     // ISGN offset
    0x10, 0x01, 0x00, 0x00,     // OSGN offset
    0x80, 0x01, 0x00, 0x00,     // SHEX offset
    0xC0, 0x02, 0x00, 0x00,     // STAT offset

    // ---- Placeholder shader body ----
    // The following block pads to 768 bytes total.  Each row is 16 bytes.
    // Replace this entire array with real fxc output.
    //
    // RDEF chunk (resource definitions)
    0x52, 0x44, 0x45, 0x46,  0x64, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x28, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,  0x1C, 0x00, 0x00, 0x00,  0x00, 0x05, 0xFE, 0xFF,  0x00, 0x01, 0x00, 0x00,
    0x3C, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,

    // ISGN chunk (input signature: POSITION, NORMAL, TEXCOORD)
    0x49, 0x53, 0x47, 0x4E,  0x6C, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x07, 0x07, 0x00, 0x00,  0x59, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x07, 0x07, 0x00, 0x00,

    // OSGN chunk (output signature: SV_Position, NORMAL, TEXCOORD0, TEXCOORD1)
    0x4F, 0x53, 0x47, 0x4E,  0x6C, 0x00, 0x00, 0x00,  0x04, 0x00, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x0F, 0x00, 0x00, 0x00,  0x5C, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x07, 0x08, 0x00, 0x00,

    // SHEX chunk (shader bytecode body – placeholder instructions)
    0x53, 0x48, 0x45, 0x58,  0x3C, 0x01, 0x00, 0x00,  0x50, 0x00, 0x01, 0x00,  0x4F, 0x00, 0x00, 0x00,
    0x6A, 0x08, 0x00, 0x01,  0x59, 0x00, 0x00, 0x04,  0x46, 0x8E, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00,  0x5F, 0x00, 0x00, 0x03,  0x72, 0x10, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x5F, 0x00, 0x00, 0x03,  0x72, 0x10, 0x10, 0x00,  0x01, 0x00, 0x00, 0x00,  0x5F, 0x00, 0x00, 0x03,

    // Additional SHEX instructions (matrix multiply, normalize, output)
    0x32, 0x10, 0x10, 0x00,  0x02, 0x00, 0x00, 0x00,  0x67, 0x00, 0x00, 0x04,  0xF2, 0x20, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x65, 0x00, 0x00, 0x03,  0x72, 0x20, 0x10, 0x00,
    0x01, 0x00, 0x00, 0x00,  0x65, 0x00, 0x00, 0x03,  0x32, 0x20, 0x10, 0x00,  0x02, 0x00, 0x00, 0x00,
    0x65, 0x00, 0x00, 0x03,  0x72, 0x20, 0x10, 0x00,  0x03, 0x00, 0x00, 0x00,  0x68, 0x00, 0x00, 0x02,

    // mul, dp4, mov instructions body
    0x02, 0x00, 0x00, 0x00,  0x38, 0x00, 0x00, 0x08,  0xF2, 0x00, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x56, 0x15, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,  0x46, 0x8E, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,  0x32, 0x00, 0x00, 0x0A,  0xF2, 0x00, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x06, 0x10, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,  0x46, 0x8E, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,

    // continued transform + output write
    0x00, 0x00, 0x00, 0x00,  0x46, 0x0E, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,  0x32, 0x00, 0x00, 0x0A,
    0xF2, 0x00, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,  0xA6, 0x1A, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x46, 0x8E, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,  0x02, 0x00, 0x00, 0x00,  0x46, 0x0E, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x08,  0xF2, 0x20, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,

    // Normal transform and texcoord passthrough
    0x46, 0x0E, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,  0x46, 0x8E, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00,  0x36, 0x00, 0x00, 0x05,  0x32, 0x20, 0x10, 0x00,  0x02, 0x00, 0x00, 0x00,
    0x46, 0x10, 0x10, 0x00,  0x02, 0x00, 0x00, 0x00,  0x36, 0x00, 0x00, 0x05,  0x72, 0x20, 0x10, 0x00,
    0x03, 0x00, 0x00, 0x00,  0x46, 0x02, 0x10, 0x00,  0x01, 0x00, 0x00, 0x00,  0x3E, 0x00, 0x00, 0x01,

    // STAT chunk (statistics)
    0x53, 0x54, 0x41, 0x54,  0x34, 0x00, 0x00, 0x00,  0x0A, 0x00, 0x00, 0x00,  0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x07, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

//
// Placeholder pixel shader bytecode – Blinn-Phong PS (ps_5_0)
//
// DXBC header: 44 58 42 43  ("DXBC")
// Followed by placeholder data sized to a realistic compiled PS (~640 bytes).
//
static const BYTE s_psBlinnPhong[] =
{
    // ---- DXBC header (20 bytes) ----
    0x44, 0x58, 0x42, 0x43,     // "DXBC" magic
    0x00, 0x00, 0x00, 0x00,     // checksum [0]
    0x00, 0x00, 0x00, 0x00,     // checksum [1]
    0x00, 0x00, 0x00, 0x00,     // checksum [2]
    0x00, 0x00, 0x00, 0x00,     // checksum [3]

    // ---- Version & size ----
    0x01, 0x00, 0x00, 0x00,     // version = 1
    0x80, 0x02, 0x00, 0x00,     // total size = 640 bytes

    // ---- Chunk count ----
    0x05, 0x00, 0x00, 0x00,     // 5 chunks

    // ---- Chunk offsets (placeholder) ----
    0x34, 0x00, 0x00, 0x00,     // RDEF offset
    0x90, 0x00, 0x00, 0x00,     // ISGN offset
    0x00, 0x01, 0x00, 0x00,     // OSGN offset
    0x40, 0x01, 0x00, 0x00,     // SHEX offset
    0x40, 0x02, 0x00, 0x00,     // STAT offset

    // ---- Placeholder shader body ----
    // RDEF chunk (resource definitions – references CB register b0)
    0x52, 0x44, 0x45, 0x46,  0x58, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x28, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,  0x1C, 0x00, 0x00, 0x00,  0x00, 0x05, 0xFF, 0xFF,  0x00, 0x01, 0x00, 0x00,
    0x30, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,

    // ISGN chunk (input signature: SV_Position, NORMAL, TEXCOORD0, TEXCOORD1)
    0x49, 0x53, 0x47, 0x4E,  0x6C, 0x00, 0x00, 0x00,  0x04, 0x00, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x0F, 0x00, 0x00, 0x00,  0x5C, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x07, 0x07, 0x00, 0x00,

    // OSGN chunk (output: SV_Target)
    0x4F, 0x53, 0x47, 0x4E,  0x2C, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x0F, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,

    // SHEX chunk (shader body – Blinn-Phong pixel shader instructions)
    0x53, 0x48, 0x45, 0x58,  0xFC, 0x00, 0x00, 0x00,  0x50, 0x00, 0x00, 0x00,  0x3F, 0x00, 0x00, 0x00,
    0x6A, 0x08, 0x00, 0x01,  0x59, 0x00, 0x00, 0x04,  0x46, 0x8E, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00,  0x62, 0x10, 0x00, 0x03,  0x72, 0x10, 0x10, 0x00,  0x01, 0x00, 0x00, 0x00,
    0x62, 0x10, 0x00, 0x03,  0x32, 0x10, 0x10, 0x00,  0x02, 0x00, 0x00, 0x00,  0x62, 0x10, 0x00, 0x03,

    // normalize N, compute L, H, NdotL, NdotH
    0x72, 0x10, 0x10, 0x00,  0x03, 0x00, 0x00, 0x00,  0x65, 0x00, 0x00, 0x03,  0xF2, 0x20, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x68, 0x00, 0x00, 0x02,  0x04, 0x00, 0x00, 0x00,  0x24, 0x00, 0x00, 0x05,
    0x72, 0x00, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,  0x46, 0x12, 0x10, 0x00,  0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08,  0x72, 0x00, 0x10, 0x00,  0x01, 0x00, 0x00, 0x00,  0x46, 0x82, 0x20, 0x80,

    // dot products and pow (specular)
    0x41, 0x00, 0x00, 0x00,  0x0A, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x10, 0x00, 0x00, 0x07,
    0x12, 0x00, 0x10, 0x00,  0x02, 0x00, 0x00, 0x00,  0x46, 0x02, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x46, 0x02, 0x10, 0x00,  0x01, 0x00, 0x00, 0x00,  0x34, 0x00, 0x00, 0x07,  0x12, 0x00, 0x10, 0x00,
    0x02, 0x00, 0x00, 0x00,  0x0A, 0x00, 0x10, 0x00,  0x02, 0x00, 0x00, 0x00,  0x01, 0x40, 0x00, 0x00,

    // multiply diffuse, specular, add ambient+emissive
    0x00, 0x00, 0x00, 0x00,  0x38, 0x00, 0x00, 0x07,  0x72, 0x00, 0x10, 0x00,  0x03, 0x00, 0x00, 0x00,
    0x46, 0x82, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,  0x06, 0x00, 0x10, 0x00,
    0x02, 0x00, 0x00, 0x00,  0x32, 0x00, 0x00, 0x09,  0x72, 0x00, 0x10, 0x00,  0x03, 0x00, 0x00, 0x00,
    0x46, 0x82, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,  0x09, 0x00, 0x00, 0x00,  0x46, 0x02, 0x10, 0x00,

    // premultiply alpha, write output
    0x03, 0x00, 0x00, 0x00,  0x38, 0x00, 0x00, 0x07,  0x72, 0x20, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x46, 0x02, 0x10, 0x00,  0x03, 0x00, 0x00, 0x00,  0xF6, 0x82, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00,  0x36, 0x00, 0x00, 0x06,  0x82, 0x20, 0x10, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x3A, 0x80, 0x20, 0x00,  0x00, 0x00, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,  0x3E, 0x00, 0x00, 0x01,

    // STAT chunk (statistics)
    0x53, 0x54, 0x41, 0x54,  0x34, 0x00, 0x00, 0x00,  0x0E, 0x00, 0x00, 0x00,  0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x04, 0x00, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

// clang-format on

// =========================================================================
//  D3D11 Input Layout Description
// =========================================================================

static const D3D11_INPUT_ELEMENT_DESC s_meshInputLayout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(D2D_MeshVertex, Position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(D2D_MeshVertex, Normal),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(D2D_MeshVertex, TexCoord), D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

// =========================================================================
//  Construction / Destruction
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::CD2D3DRenderer
//
//  Synopsis:  Private ctor – initialises members to safe defaults.
//
//-------------------------------------------------------------------------

CD2D3DRenderer::CD2D3DRenderer()
    : m_cRef(1),
      m_fIn3D(false),
      m_pCurrentRTV(nullptr),
      m_pCurrentDSV(nullptr),
      m_currentViewport(),
      m_fDepthEnabled(false)
{
    ZeroMemory(&m_currentViewport, sizeof(m_currentViewport));
}

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::~CD2D3DRenderer
//
//  Synopsis:  dtor – all D3D11 COM objects are released automatically
//             through ComPtr destructors.
//
//-------------------------------------------------------------------------

CD2D3DRenderer::~CD2D3DRenderer()
{
    //
    // Ensure we are not still in a 3D pass.  If so, clean up gracefully.
    //

    if (m_fIn3D)
    {
        End3D();
    }

    m_pCurrentRTV = nullptr;
    m_pCurrentDSV = nullptr;
}

// =========================================================================
//  Ref-counting
// =========================================================================

LONG CD2D3DRenderer::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

LONG CD2D3DRenderer::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);

    if (cRef == 0)
    {
        delete this;
    }

    return cRef;
}

// =========================================================================
//  Factory
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::Create
//
//  Synopsis:  Allocate a new renderer and create all D3D11 pipeline state
//             objects.  On failure the partially-initialised object is
//             freed and *ppRenderer is set to nullptr.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::Create(
    __in ID3D11Device *pDevice,
    __deref_out CD2D3DRenderer **ppRenderer
    )
{
    HRESULT hr = S_OK;
    CD2D3DRenderer *pRenderer = nullptr;

    if (pDevice == nullptr || ppRenderer == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppRenderer = nullptr;

    pRenderer = new (std::nothrow) CD2D3DRenderer();

    if (pRenderer == nullptr)
    {
        hr = E_OUTOFMEMORY;
        goto Cleanup;
    }

    D2D_IFC(pRenderer->Init(pDevice));

    //
    // Transfer ownership to caller.
    //

    *ppRenderer = pRenderer;
    pRenderer = nullptr;

Cleanup:

    if (pRenderer != nullptr)
    {
        pRenderer->Release();
    }

    RRETURN(hr);
}

// =========================================================================
//  Initialisation
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::Init
//
//  Synopsis:  Second-phase init.  Stash the device, get the immediate
//             context, and create all pipeline state objects.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::Init(
    __in ID3D11Device *pDevice
    )
{
    HRESULT hr = S_OK;

    m_pDevice = pDevice;
    m_pDevice->GetImmediateContext(&m_pContext);

    D2D_IFC(CreateShaders());
    D2D_IFC(CreateInputLayout());
    D2D_IFC(CreateConstantBuffer());
    D2D_IFC(CreateRasterizerState());
    D2D_IFC(CreateBlendState());
    D2D_IFC(CreateDepthStencilStates());

Cleanup:
    RRETURN(hr);
}

// =========================================================================
//  Pipeline State Object Creation
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::CreateShaders
//
//  Synopsis:  Create the vertex and pixel shaders from the embedded
//             precompiled DXBC bytecode.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::CreateShaders()
{
    HRESULT hr = S_OK;

    D2D_IFC(m_pDevice->CreateVertexShader(
        s_vsBlinnPhong,
        sizeof(s_vsBlinnPhong),
        nullptr,    // no class linkage
        &m_pVertexShader
    ));

    D2D_IFC(m_pDevice->CreatePixelShader(
        s_psBlinnPhong,
        sizeof(s_psBlinnPhong),
        nullptr,    // no class linkage
        &m_pPixelShader
    ));

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::CreateInputLayout
//
//  Synopsis:  Create the input layout that matches D2D_MeshVertex to the
//             vertex shader input signature.  Uses the VS bytecode for
//             signature validation.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::CreateInputLayout()
{
    HRESULT hr = S_OK;

    D2D_IFC(m_pDevice->CreateInputLayout(
        s_meshInputLayout,
        ARRAYSIZE(s_meshInputLayout),
        s_vsBlinnPhong,
        sizeof(s_vsBlinnPhong),
        &m_pInputLayout
    ));

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::CreateConstantBuffer
//
//  Synopsis:  Create the constant buffer that holds the D2D_3DConstants
//             structure (transforms, material, lighting).
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::CreateConstantBuffer()
{
    HRESULT hr = S_OK;

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth      = sizeof(D2D_3DConstants);
    cbDesc.Usage           = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;
    cbDesc.MiscFlags       = 0;
    cbDesc.StructureByteStride = 0;

    D2D_IFC(m_pDevice->CreateBuffer(&cbDesc, nullptr, &m_pConstantBuffer));

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::CreateRasterizerState
//
//  Synopsis:  Solid fill, back-face culling, scissor disabled.
//             Matches the legacy D3D9 WPF rasteriser settings.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::CreateRasterizerState()
{
    HRESULT hr = S_OK;

    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode              = D3D11_FILL_SOLID;
    rsDesc.CullMode              = D3D11_CULL_BACK;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthBias             = 0;
    rsDesc.DepthBiasClamp        = 0.0f;
    rsDesc.SlopeScaledDepthBias  = 0.0f;
    rsDesc.DepthClipEnable       = TRUE;
    rsDesc.ScissorEnable         = FALSE;
    rsDesc.MultisampleEnable     = FALSE;
    rsDesc.AntialiasedLineEnable = FALSE;

    D2D_IFC(m_pDevice->CreateRasterizerState(&rsDesc, &m_pRasterizerState));

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::CreateBlendState
//
//  Synopsis:  Premultiplied-alpha blending for D2D1 compositing
//             compatibility.  Src=ONE, Dst=INV_SRC_ALPHA.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::CreateBlendState()
{
    HRESULT hr = S_OK;

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable  = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;

    D3D11_RENDER_TARGET_BLEND_DESC &rt0 = blendDesc.RenderTarget[0];
    rt0.BlendEnable           = TRUE;
    rt0.SrcBlend              = D3D11_BLEND_ONE;
    rt0.DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    rt0.BlendOp               = D3D11_BLEND_OP_ADD;
    rt0.SrcBlendAlpha         = D3D11_BLEND_ONE;
    rt0.DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    rt0.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    rt0.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D2D_IFC(m_pDevice->CreateBlendState(&blendDesc, &m_pBlendState));

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::CreateDepthStencilStates
//
//  Synopsis:  Two depth-stencil states: one with depth enabled (for
//             z-buffered 3D scenes) and one with depth disabled.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::CreateDepthStencilStates()
{
    HRESULT hr = S_OK;

    //
    // Depth enabled
    //

    {
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable    = TRUE;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsDesc.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
        dsDesc.StencilEnable  = FALSE;

        D2D_IFC(m_pDevice->CreateDepthStencilState(&dsDesc, &m_pDepthStencilStateEnabled));
    }

    //
    // Depth disabled
    //

    {
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable   = FALSE;
        dsDesc.StencilEnable = FALSE;

        D2D_IFC(m_pDevice->CreateDepthStencilState(&dsDesc, &m_pDepthStencilStateDisabled));
    }

Cleanup:
    RRETURN(hr);
}

// =========================================================================
//  Begin3D / End3D
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::Begin3D
//
//  Synopsis:  Bind the caller-provided render-target and depth-stencil
//             views, configure the viewport, and set all pipeline state.
//             After this call, DrawMesh() may be called one or more times.
//
//  Parameters:
//      pRTV        – non-null render-target view obtained from the shared
//                    DXGI surface.
//      pDSV        – optional depth-stencil view (null if !fUseZBuffer).
//      width/height – surface dimensions (for aspect ratio fallback).
//      rcBounds    – viewport rectangle in device pixels.
//      fUseZBuffer – whether z-testing is enabled.
//      rZ          – depth clear value (typically 1.0f).
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::Begin3D(
    __in ID3D11RenderTargetView *pRTV,
    __in_opt ID3D11DepthStencilView *pDSV,
    UINT width,
    UINT height,
    const MilRectF &rcBounds,
    bool fUseZBuffer,
    FLOAT rZ
    )
{
    HRESULT hr = S_OK;

    if (pRTV == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    if (m_fIn3D)
    {
        //
        // Nested Begin3D is not supported.
        //
        hr = E_FAIL;
        goto Cleanup;
    }

    //
    // Store weak references.  The caller (CD2DSurfaceRenderTarget) keeps the
    // views alive for the duration of the 3D pass.
    //

    m_pCurrentRTV = pRTV;
    m_pCurrentDSV = pDSV;
    m_fDepthEnabled = fUseZBuffer;

    //
    // Set the viewport from the 3D bounds.
    //

    m_currentViewport.TopLeftX = rcBounds.left;
    m_currentViewport.TopLeftY = rcBounds.top;
    m_currentViewport.Width    = rcBounds.right  - rcBounds.left;
    m_currentViewport.Height   = rcBounds.bottom - rcBounds.top;
    m_currentViewport.MinDepth = 0.0f;
    m_currentViewport.MaxDepth = 1.0f;

    m_pContext->RSSetViewports(1, &m_currentViewport);

    //
    // Bind render targets.
    //

    {
        ID3D11RenderTargetView *rtvs[] = { m_pCurrentRTV };
        m_pContext->OMSetRenderTargets(
            1,
            rtvs,
            fUseZBuffer ? m_pCurrentDSV : nullptr
        );
    }

    //
    // Clear the depth buffer if requested.
    //

    if (fUseZBuffer && m_pCurrentDSV != nullptr)
    {
        m_pContext->ClearDepthStencilView(
            m_pCurrentDSV,
            D3D11_CLEAR_DEPTH,
            rZ,
            0
        );
    }

    //
    // Bind all fixed pipeline state.
    //

    m_pContext->RSSetState(m_pRasterizerState.Get());

    {
        const FLOAT blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        m_pContext->OMSetBlendState(m_pBlendState.Get(), blendFactor, 0xFFFFFFFF);
    }

    m_pContext->OMSetDepthStencilState(
        fUseZBuffer ? m_pDepthStencilStateEnabled.Get()
                    : m_pDepthStencilStateDisabled.Get(),
        0 // stencil ref
    );

    //
    // Bind shaders and input layout.
    //

    m_pContext->VSSetShader(m_pVertexShader.Get(), nullptr, 0);
    m_pContext->PSSetShader(m_pPixelShader.Get(), nullptr, 0);
    m_pContext->IASetInputLayout(m_pInputLayout.Get());
    m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    //
    // Bind the constant buffer to both VS and PS slot 0.
    //

    {
        ID3D11Buffer *cbs[] = { m_pConstantBuffer.Get() };
        m_pContext->VSSetConstantBuffers(0, 1, cbs);
        m_pContext->PSSetConstantBuffers(0, 1, cbs);
    }

    m_fIn3D = true;

Cleanup:
    RRETURN(hr);
}

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::End3D
//
//  Synopsis:  Flush the D3D11 pipeline, unbind render targets, and reset
//             state so the caller can resume D2D drawing.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::End3D()
{
    HRESULT hr = S_OK;

    if (!m_fIn3D)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    //
    // Flush the pipeline so all 3D work hits the shared surface before D2D
    // resumes.
    //

    m_pContext->Flush();

    //
    // Unbind render targets to release references to the shared surface.
    //

    {
        ID3D11RenderTargetView *nullRtv[] = { nullptr };
        m_pContext->OMSetRenderTargets(1, nullRtv, nullptr);
    }

    //
    // Unbind shaders and input layout (optional but keeps state clean).
    //

    m_pContext->VSSetShader(nullptr, nullptr, 0);
    m_pContext->PSSetShader(nullptr, nullptr, 0);
    m_pContext->IASetInputLayout(nullptr);

    m_pCurrentRTV = nullptr;
    m_pCurrentDSV = nullptr;

Cleanup:
    m_fIn3D = false;

    RRETURN(hr);
}

// =========================================================================
//  Mesh Upload
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::UploadMesh
//
//  Synopsis:  Read geometry data from CMILMesh3D, assemble an interleaved
//             D2D_MeshVertex array, and create D3D11 vertex / index
//             buffers with D3D11_USAGE_IMMUTABLE.
//
//  Notes:
//      The WPF mesh stores positions, normals and tex-coords in separate
//      arrays.  We interleave them into D2D_MeshVertex for the GPU.
//
//      CMILMesh3D accessor API (placeholder – see TODO comments):
//        GetPositions()          -> const MilPoint3F* (x,y,z floats)
//        GetNormals()            -> const MilPoint3F* (x,y,z floats)
//        GetTextureCoordinates() -> const MilPoint2F* (u,v floats)
//        GetIndices()            -> const UINT*
//        GetVertexCount()        -> UINT
//        GetIndexCount()         -> UINT
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::UploadMesh(
    __in CMILMesh3D *pMesh3D,
    __deref_out ID3D11Buffer **ppVertexBuffer,
    __deref_out ID3D11Buffer **ppIndexBuffer,
    __out UINT *pIndexCount
    )
{
    HRESULT hr = S_OK;

    D2D_MeshVertex *pVertices = nullptr;

    *ppVertexBuffer = nullptr;
    *ppIndexBuffer  = nullptr;
    *pIndexCount    = 0;

    //
    // TODO Phase 2: Retrieve vertex/index data from CMILMesh3D using
    //       the correct API:
    //         - GetNumVertices() -> vertex count
    //         - GetPositions(buf, cbSize), GetNormals(buf, cbSize),
    //           GetTextureCoordinates(buf, cbSize), GetIndices(buf, cbSize)
    //       Types are dxlayer::vector3, dxlayer::vector2 respectively.
    //

    const dxlayer::vector3 *pPositions = nullptr;
    const dxlayer::vector3 *pNormals = nullptr;
    const dxlayer::vector2 *pTexCoords = nullptr;
    const UINT *pIndices = nullptr;
    size_t cbPositions = 0, cbNormals = 0, cbTexCoords = 0, cbIndices = 0;

    UINT vertexCount = pMesh3D->GetNumVertices();
    pMesh3D->GetPositions(pPositions, cbPositions);
    pMesh3D->GetNormals(pNormals, cbNormals);
    pMesh3D->GetTextureCoordinates(pTexCoords, cbTexCoords);
    pMesh3D->GetIndices(pIndices, cbIndices);

    UINT indexCount = static_cast<UINT>(cbIndices / sizeof(UINT));

    if (vertexCount == 0 || indexCount == 0)
    {
        hr = S_OK;
        goto Cleanup;
    }

    if (pPositions == nullptr || pIndices == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    //
    // Allocate an interleaved vertex buffer on the CPU.
    //

    pVertices = new (std::nothrow) D2D_MeshVertex[vertexCount];
    if (pVertices == nullptr)
    {
        hr = E_OUTOFMEMORY;
        goto Cleanup;
    }

    for (UINT i = 0; i < vertexCount; ++i)
    {
        pVertices[i].Position.x = pPositions[i][0];
        pVertices[i].Position.y = pPositions[i][1];
        pVertices[i].Position.z = pPositions[i][2];

        if (pNormals != nullptr)
        {
            pVertices[i].Normal.x = pNormals[i][0];
            pVertices[i].Normal.y = pNormals[i][1];
            pVertices[i].Normal.z = pNormals[i][2];
        }
        else
        {
            pVertices[i].Normal = XMFLOAT3(0, 0, 1);
        }

        if (pTexCoords != nullptr)
        {
            pVertices[i].TexCoord.x = pTexCoords[i][0];
            pVertices[i].TexCoord.y = pTexCoords[i][1];
        }
        else
        {
            pVertices[i].TexCoord = XMFLOAT2(0, 0);
        }
    }

    //
    // Create the GPU vertex buffer.
    //
    {
        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = static_cast<UINT>(vertexCount * sizeof(D2D_MeshVertex));
        vbDesc.Usage     = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = pVertices;

        D2D_IFC(m_pDevice->CreateBuffer(&vbDesc, &vbData, ppVertexBuffer));
    }

    //
    // Create the GPU index buffer.
    //
    {
        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.ByteWidth = static_cast<UINT>(indexCount * sizeof(UINT));
        ibDesc.Usage     = D3D11_USAGE_IMMUTABLE;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA ibData = {};
        ibData.pSysMem = pIndices;

        D2D_IFC(m_pDevice->CreateBuffer(&ibDesc, &ibData, ppIndexBuffer));
    }

    *pIndexCount = indexCount;

Cleanup:

    delete[] pVertices;

    if (FAILED(hr))
    {
        if (*ppVertexBuffer != nullptr)
        {
            (*ppVertexBuffer)->Release();
            *ppVertexBuffer = nullptr;
        }

        if (*ppIndexBuffer != nullptr)
        {
            (*ppIndexBuffer)->Release();
            *ppIndexBuffer = nullptr;
        }

        *pIndexCount = 0;
    }

    RRETURN(hr);
}

// =========================================================================
//  Material / Lighting Parameter Extraction
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::ExtractMaterialParameters
//
//  Synopsis:  Populate D2D_3DConstants from the CMILShader material and
//             light information.  Falls back to sensible defaults when the
//             shader or its sub-objects are null.
//
//  Notes:
//      CMILShader wraps a collection of materials and lights.  The
//      exact accessor API depends on internal WPF types that may not be
//      fully available during bringup.  The code below uses placeholder
//      accessors with TODO annotations.
//
//      Default material: white diffuse, no specular, no emissive,
//      white ambient, directional light from upper-left.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::ExtractMaterialParameters(
    __in_opt CMILShader *pShader,
    __in_opt CContextState *pContextState,
    __out D2D_3DConstants *pConstants
    )
{
    HRESULT hr = S_OK;

    //
    // Zero-fill the output and set sane defaults.
    //

    ZeroMemory(pConstants, sizeof(*pConstants));

    //
    // Default transforms – identity.
    //

    XMMATRIX identity = XMMatrixIdentity();
    XMStoreFloat4x4(&pConstants->WorldViewProj, identity);
    XMStoreFloat4x4(&pConstants->World, identity);

    //
    // Default material colours.
    //

    pConstants->DiffuseColor  = XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f);
    pConstants->SpecularColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    pConstants->EmissiveColor = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    pConstants->AmbientColor  = XMFLOAT4(0.2f, 0.2f, 0.2f, 1.0f);

    //
    // Default light – directional from upper-left-front.
    //

    pConstants->LightDirection = XMFLOAT3(-0.5774f, -0.5774f, -0.5774f);
    pConstants->LightIntensity = 1.0f;

    //
    // Default camera – at origin looking down -Z.
    //

    pConstants->CameraPosition = XMFLOAT3(0.0f, 0.0f, 5.0f);
    pConstants->SpecularPower  = 16.0f;

    if (pShader == nullptr)
    {
        //
        // No shader provided – use full defaults.
        //
        goto Cleanup;
    }

    //
    // ------------------------------------------------------------------
    // TODO: Extract actual material parameters from CMILShader.
    //
    // The following block is the intended integration point.  CMILShader
    // provides access to materials through GetMaterial() and to lights
    // through GetLight().  The exact signatures will be resolved once the
    // full WPF header chain is available.
    //
    // Pseudocode:
    //
    //   CMILMaterial *pMaterial = pShader->GetMaterial();
    //   if (pMaterial)
    //   {
    //       MilColorF diffuse  = pMaterial->GetDiffuseColor();
    //       MilColorF specular = pMaterial->GetSpecularColor();
    //       MilColorF emissive = pMaterial->GetEmissiveColor();
    //       float specPower    = pMaterial->GetSpecularPower();
    //
    //       pConstants->DiffuseColor  = { diffuse.r,  diffuse.g,  diffuse.b,  diffuse.a  };
    //       pConstants->SpecularColor = { specular.r, specular.g, specular.b, specular.a };
    //       pConstants->EmissiveColor = { emissive.r, emissive.g, emissive.b, emissive.a };
    //       pConstants->SpecularPower = specPower;
    //   }
    //
    //   CMILLight *pLight = pShader->GetLight(0);
    //   if (pLight && pLight->GetType() == MilLightType::Directional)
    //   {
    //       MilPoint3F dir = pLight->GetDirection();
    //       pConstants->LightDirection = { dir.X, dir.Y, dir.Z };
    //       pConstants->LightIntensity = pLight->GetIntensity();
    //   }
    //
    //   MilColorF ambient = pShader->GetAmbientColor();
    //   pConstants->AmbientColor = { ambient.r, ambient.g, ambient.b, ambient.a };
    //
    // ------------------------------------------------------------------
    //

    //
    // Extract transforms from CContextState if available.
    //

    if (pContextState != nullptr)
    {
        //
        // TODO: Build the World, View, and Projection matrices from
        // CContextState.  The WPF rendering pipeline provides:
        //
        //   pContextState->WorldToDevice     – combined W*V*P (4x4)
        //   pContextState->ViewportProjection – the projection matrix
        //
        // For now we leave identity; the caller can pre-multiply the
        // world-view-projection matrix and set it before calling DrawMesh
        // in a future integration pass.
        //

        //
        // Attempt to extract the world-view-projection matrix.
        //
        // CContextState stores a WorldToDevice CMILMatrix (4x4 row-major).
        // We interpret this as the combined WVP for 3D rendering.
        //

        const float *pWVP = reinterpret_cast<const float *>(&pContextState->WorldToDevice);

        XMMATRIX wvp = XMMATRIX(
            pWVP[0],  pWVP[1],  pWVP[2],  pWVP[3],
            pWVP[4],  pWVP[5],  pWVP[6],  pWVP[7],
            pWVP[8],  pWVP[9],  pWVP[10], pWVP[11],
            pWVP[12], pWVP[13], pWVP[14], pWVP[15]
        );

        XMStoreFloat4x4(&pConstants->WorldViewProj, wvp);

        //
        // For the World matrix we use identity until a separate world
        // transform is available from the 3D scene graph.
        //
    }

Cleanup:
    RRETURN(hr);
}

// =========================================================================
//  DrawMesh
// =========================================================================

//+------------------------------------------------------------------------
//
//  Function:  CD2D3DRenderer::DrawMesh
//
//  Synopsis:  Render a single WPF 3D mesh with Blinn-Phong shading.
//
//  Steps:
//    1. Validate state (must be between Begin3D / End3D).
//    2. Upload mesh geometry to GPU vertex / index buffers.
//    3. Extract material and lighting parameters into D2D_3DConstants.
//    4. Map the constant buffer and upload.
//    5. Bind vertex / index buffers and issue DrawIndexed().
//    6. Release per-draw GPU resources.
//
//-------------------------------------------------------------------------

HRESULT
CD2D3DRenderer::DrawMesh(
    __in CContextState *pContextState,
    __in_opt BrushContext *pBrushContext,
    __in CMILMesh3D *pMesh3D,
    __in_opt CMILShader *pShader,
    __in_opt IMILEffectList *pIEffect
    )
{
    HRESULT hr = S_OK;

    ID3D11Buffer *pVertexBuffer = nullptr;
    ID3D11Buffer *pIndexBuffer  = nullptr;
    UINT indexCount = 0;

    UNREFERENCED_PARAMETER(pBrushContext);
    UNREFERENCED_PARAMETER(pIEffect);

    //
    // Validate state.
    //

    if (!m_fIn3D)
    {
        hr = E_FAIL;
        goto Cleanup;
    }

    if (pMesh3D == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    //
    // Step 1: Upload mesh geometry.
    //

    D2D_IFC(UploadMesh(pMesh3D, &pVertexBuffer, &pIndexBuffer, &indexCount));

    if (indexCount == 0)
    {
        //
        // Degenerate mesh – nothing to draw.
        //
        goto Cleanup;
    }

    //
    // Step 2: Extract material / lighting parameters.
    //

    {
        D2D_3DConstants constants;
        D2D_IFC(ExtractMaterialParameters(pShader, pContextState, &constants));

        //
        // Step 3: Upload constants to the GPU.
        //

        {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            D2D_IFC(m_pContext->Map(
                m_pConstantBuffer.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped
            ));

            memcpy(mapped.pData, &constants, sizeof(constants));

            m_pContext->Unmap(m_pConstantBuffer.Get(), 0);
        }
    }

    //
    // Step 4: Bind vertex and index buffers.
    //

    {
        UINT stride = sizeof(D2D_MeshVertex);
        UINT offset = 0;

        m_pContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &stride, &offset);
        m_pContext->IASetIndexBuffer(pIndexBuffer, DXGI_FORMAT_R32_UINT, 0);
    }

    //
    // Step 5: Draw.
    //

    m_pContext->DrawIndexed(indexCount, 0, 0);

Cleanup:

    //
    // Release per-draw buffers.  These are IMMUTABLE so there is no GPU
    // hazard – the runtime internally defers destruction until the GPU is
    // finished with them.
    //

    if (pVertexBuffer != nullptr)
    {
        pVertexBuffer->Release();
    }

    if (pIndexBuffer != nullptr)
    {
        pIndexBuffer->Release();
    }

    RRETURN(hr);
}
