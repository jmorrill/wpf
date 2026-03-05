// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+------------------------------------------------------------------------
//
//  Class: CD2DResourceManager
//
//  Synopsis: Manages GPU resource lifecycle for the D2D rendering backend.
//           Unlike the D3D9 backend which aggressively evicts resources
//           to minimize VRAM usage, this manager keeps resources resident
//           and relies on D3D11/DXGI automatic residency management.
//
//           Key differences from D3D9 path:
//           - No manual texture eviction (DXGI handles this)  
//           - Default pool (GPU) for all textures (no managed/system copies)
//           - Bitmap atlas for small bitmaps to reduce state changes
//           - Geometry cache for frequently-used shapes
//
//------------------------------------------------------------------------

#pragma once

#include <vector>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

class CD2DResourceManager
{
public:
    static HRESULT Create(
        __in CD2DDevice *pDevice,
        __deref_out CD2DResourceManager **ppManager
    );

    LONG AddRef();
    LONG Release();

    //
    // Bitmap caching
    //

    // Get or create a cached D2D bitmap from a WPF bitmap source
    HRESULT GetCachedBitmap(
        __in ID2D1DeviceContext1 *pContext,
        __in IWGXBitmapSource *pSource,
        UINT sourceGeneration,        // version counter for invalidation
        __deref_out ID2D1Bitmap1 **ppBitmap
    );

    // Evict a specific bitmap from cache
    void EvictBitmap(__in IWGXBitmapSource *pSource);

    // Evict all cached bitmaps
    void EvictAllBitmaps();

    //
    // Geometry caching
    //

    // Get or create a cached D2D geometry from shape data
    HRESULT GetCachedGeometry(
        __in ID2D1Factory1 *pFactory,
        __in IShapeData *pShape,
        UINT shapeVersion,
        __deref_out ID2D1Geometry **ppGeometry
    );

    void EvictGeometry(__in IShapeData *pShape);
    void EvictAllGeometries();

    //
    // Intermediate render target pool
    //

    // Get a pooled intermediate bitmap for effects/layers
    HRESULT AcquireIntermediateBitmap(
        __in ID2D1DeviceContext1 *pContext,
        UINT width,
        UINT height,
        __deref_out ID2D1Bitmap1 **ppBitmap
    );

    // Return a bitmap to the pool
    void ReleaseIntermediateBitmap(__in ID2D1Bitmap1 *pBitmap);

    //
    // Memory diagnostics
    //

    struct MemoryStats
    {
        SIZE_T cachedBitmapBytes;
        SIZE_T cachedGeometryCount;
        SIZE_T intermediatePoolBytes;
        SIZE_T totalEstimatedGPUBytes;

        // DXGI budget info (if available via IDXGIAdapter3)
        SIZE_T dxgiLocalBudget;
        SIZE_T dxgiLocalUsage;
    };

    HRESULT GetMemoryStats(__out MemoryStats *pStats);

    // Trim resources when system is under memory pressure
    // (called from DXGI budget change notification)
    void TrimResources(SIZE_T targetBytes);

    // Called each frame to age out unused resources
    void OnFrameComplete();

private:
    CD2DResourceManager();
    ~CD2DResourceManager();
    HRESULT Init(CD2DDevice *pDevice);

    LONG m_cRef;
    CD2DDevice *m_pDevice;

    //
    // Bitmap cache — keyed by source pointer + generation
    //
    struct BitmapCacheEntry
    {
        ComPtr<ID2D1Bitmap1> pBitmap;
        UINT generation;
        UINT lastUsedFrame;
        SIZE_T estimatedBytes;
    };

    std::unordered_map<UINT_PTR, BitmapCacheEntry> m_bitmapCache;

    //
    // Geometry cache
    //
    struct GeometryCacheEntry
    {
        ComPtr<ID2D1Geometry> pGeometry;
        UINT version;
        UINT lastUsedFrame;
    };

    std::unordered_map<UINT_PTR, GeometryCacheEntry> m_geometryCache;

    //
    // Intermediate bitmap pool (sorted by size for best-fit reuse)
    //
    struct IntermediateEntry
    {
        ComPtr<ID2D1Bitmap1> pBitmap;
        UINT width;
        UINT height;
        bool inUse;
        UINT lastUsedFrame;
    };

    std::vector<IntermediateEntry> m_intermediatePool;

    UINT m_currentFrame;
    static const UINT CACHE_MAX_AGE_FRAMES = 300; // ~5 seconds at 60fps

    CCriticalSection m_csResources;
};
