// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//+------------------------------------------------------------------------
//
//  Implementation:
//      CD2DResourceManager — GPU resource lifecycle management for the
//      D2D/D3D11 rendering backend.
//
//      Unlike the D3D9 code path which aggressively evicts textures and
//      surfaces from VRAM, this manager keeps resources resident in GPU
//      memory and relies on D3D11/DXGI automatic residency management.
//      Resources are aged out only after CACHE_MAX_AGE_FRAMES of disuse,
//      and can be force-trimmed via TrimResources() in response to DXGI
//      budget change notifications.
//
//------------------------------------------------------------------------

#include <precomp.hpp>

//
// Suppress warnings that fire inside the IFC macro expansion.
//

#pragma warning(push)
#pragma warning(disable : 4127) // conditional expression is constant

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

#ifndef RRETURN
#define RRETURN(hr) return (hr)
#endif

// -----------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------

//
// Estimate GPU memory consumed by a D2D bitmap.
// For the common BGRA32 case this is width * height * 4.
//
static SIZE_T
EstimateBitmapBytes(
    __in ID2D1Bitmap1 *pBitmap
    )
{
    D2D1_SIZE_U size = pBitmap->GetPixelSize();
    D2D1_PIXEL_FORMAT fmt = pBitmap->GetPixelFormat();

    UINT bytesPerPixel = 4; // default for B8G8R8A8_UNORM
    switch (fmt.format)
    {
    case DXGI_FORMAT_R8_UNORM:
        bytesPerPixel = 1;
        break;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
        bytesPerPixel = 2;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        bytesPerPixel = 8;
        break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        bytesPerPixel = 16;
        break;
    default:
        bytesPerPixel = 4;
        break;
    }

    return static_cast<SIZE_T>(size.width) * size.height * bytesPerPixel;
}

// -----------------------------------------------------------------------
//  Construction / Destruction
// -----------------------------------------------------------------------

CD2DResourceManager::CD2DResourceManager()
    : m_cRef(1)
    , m_pDevice(nullptr)
    , m_currentFrame(0)
{
}

CD2DResourceManager::~CD2DResourceManager()
{
    //
    // Release all cached resources.  ComPtr destructors handle the
    // actual Release() calls on the D2D objects.
    //

    m_bitmapCache.clear();
    m_geometryCache.clear();
    m_intermediatePool.clear();

    if (m_pDevice != nullptr)
    {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
}

// -----------------------------------------------------------------------
//  Create (static factory)
// -----------------------------------------------------------------------

HRESULT
CD2DResourceManager::Create(
    __in CD2DDevice *pDevice,
    __deref_out CD2DResourceManager **ppManager
    )
{
    HRESULT hr = S_OK;
    CD2DResourceManager *pManager = nullptr;

    if (pDevice == nullptr || ppManager == nullptr)
    {
        hr = E_INVALIDARG;
        goto Cleanup;
    }

    *ppManager = nullptr;

    pManager = new(std::nothrow) CD2DResourceManager();
    if (pManager == nullptr)
    {
        hr = E_OUTOFMEMORY;
        goto Cleanup;
    }

    D2D_IFC(pManager->Init(pDevice));

    //
    // Transfer ownership to caller.
    //

    *ppManager = pManager;
    pManager = nullptr;

Cleanup:
    if (pManager != nullptr)
    {
        pManager->Release();
    }
    RRETURN(hr);
}

// -----------------------------------------------------------------------
//  Init
// -----------------------------------------------------------------------

HRESULT
CD2DResourceManager::Init(
    CD2DDevice *pDevice
    )
{
    HRESULT hr = S_OK;

    D2D_IFC(m_csResources.Init());

    m_pDevice = pDevice;
    m_pDevice->AddRef();

Cleanup:
    RRETURN(hr);
}

// -----------------------------------------------------------------------
//  AddRef / Release
// -----------------------------------------------------------------------

LONG
CD2DResourceManager::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

LONG
CD2DResourceManager::Release()
{
    LONG cRef = InterlockedDecrement(&m_cRef);
    if (cRef == 0)
    {
        delete this;
    }
    return cRef;
}

// -----------------------------------------------------------------------
//  Bitmap caching
// -----------------------------------------------------------------------

HRESULT
CD2DResourceManager::GetCachedBitmap(
    __in ID2D1DeviceContext1 *pContext,
    __in IWGXBitmapSource *pSource,
    UINT sourceGeneration,
    __deref_out ID2D1Bitmap1 **ppBitmap
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(pSource);
    Assert(ppBitmap);

    *ppBitmap = nullptr;

    UINT_PTR key = reinterpret_cast<UINT_PTR>(pSource);

    CGuard<CCriticalSection> guard(m_csResources);

    //
    // Look up existing entry.
    //

    auto it = m_bitmapCache.find(key);
    if (it != m_bitmapCache.end())
    {
        BitmapCacheEntry &entry = it->second;

        //
        // Cache hit — verify the generation counter to detect source
        // invalidation (e.g. WriteableBitmap updates).
        //

        if (entry.generation == sourceGeneration)
        {
            entry.lastUsedFrame = m_currentFrame;
            *ppBitmap = entry.pBitmap.Get();
            (*ppBitmap)->AddRef();
            goto Cleanup;
        }

        //
        // Stale entry — remove it so we re-create below.
        //

        m_bitmapCache.erase(it);
    }

    //
    // Cache miss — convert the WPF bitmap source into a D2D bitmap.
    //

    {
        ComPtr<ID2D1Bitmap1> spBitmap;
        D2D_IFC(CD2DBitmapConverter::ConvertBitmapSource(pContext, pSource, &spBitmap));

        BitmapCacheEntry newEntry;
        newEntry.pBitmap = spBitmap;
        newEntry.generation = sourceGeneration;
        newEntry.lastUsedFrame = m_currentFrame;
        newEntry.estimatedBytes = EstimateBitmapBytes(spBitmap.Get());

        *ppBitmap = spBitmap.Get();
        (*ppBitmap)->AddRef();

        m_bitmapCache[key] = std::move(newEntry);
    }

Cleanup:
    RRETURN(hr);
}

void
CD2DResourceManager::EvictBitmap(
    __in IWGXBitmapSource *pSource
    )
{
    CGuard<CCriticalSection> guard(m_csResources);

    UINT_PTR key = reinterpret_cast<UINT_PTR>(pSource);
    m_bitmapCache.erase(key);
}

void
CD2DResourceManager::EvictAllBitmaps()
{
    CGuard<CCriticalSection> guard(m_csResources);
    m_bitmapCache.clear();
}

// -----------------------------------------------------------------------
//  Geometry caching
// -----------------------------------------------------------------------

HRESULT
CD2DResourceManager::GetCachedGeometry(
    __in ID2D1Factory1 *pFactory,
    __in IShapeData *pShape,
    UINT shapeVersion,
    __deref_out ID2D1Geometry **ppGeometry
    )
{
    HRESULT hr = S_OK;

    Assert(pFactory);
    Assert(pShape);
    Assert(ppGeometry);

    *ppGeometry = nullptr;

    UINT_PTR key = reinterpret_cast<UINT_PTR>(pShape);

    CGuard<CCriticalSection> guard(m_csResources);

    //
    // Look up existing entry.
    //

    auto it = m_geometryCache.find(key);
    if (it != m_geometryCache.end())
    {
        GeometryCacheEntry &entry = it->second;

        if (entry.version == shapeVersion)
        {
            entry.lastUsedFrame = m_currentFrame;
            *ppGeometry = entry.pGeometry.Get();
            (*ppGeometry)->AddRef();
            goto Cleanup;
        }

        //
        // Stale version — discard and re-create.
        //

        m_geometryCache.erase(it);
    }

    //
    // Cache miss — convert shape data to D2D geometry.
    //

    {
        ComPtr<ID2D1Geometry> spGeometry;
        D2D_IFC(CD2DGeometryConverter::ConvertShapeToGeometry(pFactory, pShape, &spGeometry));

        GeometryCacheEntry newEntry;
        newEntry.pGeometry = spGeometry;
        newEntry.version = shapeVersion;
        newEntry.lastUsedFrame = m_currentFrame;

        *ppGeometry = spGeometry.Get();
        (*ppGeometry)->AddRef();

        m_geometryCache[key] = std::move(newEntry);
    }

Cleanup:
    RRETURN(hr);
}

void
CD2DResourceManager::EvictGeometry(
    __in IShapeData *pShape
    )
{
    CGuard<CCriticalSection> guard(m_csResources);

    UINT_PTR key = reinterpret_cast<UINT_PTR>(pShape);
    m_geometryCache.erase(key);
}

void
CD2DResourceManager::EvictAllGeometries()
{
    CGuard<CCriticalSection> guard(m_csResources);
    m_geometryCache.clear();
}

// -----------------------------------------------------------------------
//  Intermediate render target pool
// -----------------------------------------------------------------------

HRESULT
CD2DResourceManager::AcquireIntermediateBitmap(
    __in ID2D1DeviceContext1 *pContext,
    UINT width,
    UINT height,
    __deref_out ID2D1Bitmap1 **ppBitmap
    )
{
    HRESULT hr = S_OK;

    Assert(pContext);
    Assert(ppBitmap);

    *ppBitmap = nullptr;

    CGuard<CCriticalSection> guard(m_csResources);

    //
    // Search for an unused bitmap whose dimensions are >= the requested
    // size.  Prefer the smallest match to minimise wasted memory.
    //

    int bestIdx = -1;
    SIZE_T bestArea = SIZE_T_MAX;

    for (int i = 0; i < static_cast<int>(m_intermediatePool.size()); i++)
    {
        IntermediateEntry &entry = m_intermediatePool[i];

        if (!entry.inUse &&
            entry.width >= width &&
            entry.height >= height)
        {
            SIZE_T area = static_cast<SIZE_T>(entry.width) * entry.height;
            if (area < bestArea)
            {
                bestArea = area;
                bestIdx = i;
            }
        }
    }

    if (bestIdx >= 0)
    {
        //
        // Reuse an existing pooled bitmap.
        //

        IntermediateEntry &entry = m_intermediatePool[bestIdx];
        entry.inUse = true;
        entry.lastUsedFrame = m_currentFrame;

        *ppBitmap = entry.pBitmap.Get();
        (*ppBitmap)->AddRef();

        goto Cleanup;
    }

    //
    // No suitable bitmap in the pool — create a new one.
    //
    // D2D1_BITMAP_OPTIONS_TARGET allows the bitmap to be set as the
    // render target on a device context.
    //

    {
        D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
        bitmapProps.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bitmapProps.dpiX = 96.0f;
        bitmapProps.dpiY = 96.0f;
        bitmapProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        bitmapProps.colorContext  = nullptr;

        ComPtr<ID2D1Bitmap1> spBitmap;
        D2D_IFC(pContext->CreateBitmap(
            D2D1::SizeU(width, height),
            nullptr,   // no initial data
            0,
            &bitmapProps,
            &spBitmap
        ));

        IntermediateEntry newEntry;
        newEntry.pBitmap = spBitmap;
        newEntry.width   = width;
        newEntry.height  = height;
        newEntry.inUse   = true;
        newEntry.lastUsedFrame = m_currentFrame;

        m_intermediatePool.push_back(std::move(newEntry));

        *ppBitmap = spBitmap.Get();
        (*ppBitmap)->AddRef();
    }

Cleanup:
    RRETURN(hr);
}

void
CD2DResourceManager::ReleaseIntermediateBitmap(
    __in ID2D1Bitmap1 *pBitmap
    )
{
    CGuard<CCriticalSection> guard(m_csResources);

    for (size_t i = 0; i < m_intermediatePool.size(); i++)
    {
        if (m_intermediatePool[i].pBitmap.Get() == pBitmap)
        {
            m_intermediatePool[i].inUse = false;
            break;
        }
    }
}

// -----------------------------------------------------------------------
//  Memory diagnostics
// -----------------------------------------------------------------------

HRESULT
CD2DResourceManager::GetMemoryStats(
    __out MemoryStats *pStats
    )
{
    HRESULT hr = S_OK;

    Assert(pStats);

    ZeroMemory(pStats, sizeof(*pStats));

    CGuard<CCriticalSection> guard(m_csResources);

    //
    // Sum bitmap cache bytes.
    //

    for (auto &pair : m_bitmapCache)
    {
        pStats->cachedBitmapBytes += pair.second.estimatedBytes;
    }

    //
    // Geometry count (geometry objects are GPU-side path data; we don't
    // have an easy way to estimate their byte size, so we report count).
    //

    pStats->cachedGeometryCount = m_geometryCache.size();

    //
    // Intermediate pool — sum all entries regardless of inUse state,
    // since the memory stays allocated.
    //

    for (auto &entry : m_intermediatePool)
    {
        pStats->intermediatePoolBytes +=
            static_cast<SIZE_T>(entry.width) * entry.height * 4;
    }

    pStats->totalEstimatedGPUBytes =
        pStats->cachedBitmapBytes + pStats->intermediatePoolBytes;

    //
    // Query DXGI adapter memory budget if IDXGIAdapter3 is available.
    // This interface was introduced in Windows 10 (DXGI 1.4).
    //

    {
        IDXGIDevice *pDXGIDevice = m_pDevice->GetDXGIDevice();
        if (pDXGIDevice != nullptr)
        {
            ComPtr<IDXGIAdapter> spAdapter;
            if (SUCCEEDED(pDXGIDevice->GetAdapter(&spAdapter)))
            {
                ComPtr<IDXGIAdapter3> spAdapter3;
                if (SUCCEEDED(spAdapter.As(&spAdapter3)))
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
                    if (SUCCEEDED(spAdapter3->QueryVideoMemoryInfo(
                            0,
                            DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                            &localInfo)))
                    {
                        pStats->dxgiLocalBudget = localInfo.Budget;
                        pStats->dxgiLocalUsage  = localInfo.CurrentUsage;
                    }
                }
            }
        }
    }

    RRETURN(hr);
}

// -----------------------------------------------------------------------
//  TrimResources
// -----------------------------------------------------------------------

void
CD2DResourceManager::TrimResources(
    SIZE_T targetBytes
    )
{
    CGuard<CCriticalSection> guard(m_csResources);

    //
    // Strategy: evict the oldest entries first (lowest lastUsedFrame)
    // from both caches until our estimated footprint drops below
    // targetBytes.
    //
    // Phase 1 — Evict unused intermediate bitmaps, oldest first.
    //

    SIZE_T currentBytes = 0;

    for (auto &pair : m_bitmapCache)
    {
        currentBytes += pair.second.estimatedBytes;
    }
    for (auto &entry : m_intermediatePool)
    {
        currentBytes += static_cast<SIZE_T>(entry.width) * entry.height * 4;
    }

    if (currentBytes <= targetBytes)
    {
        return;
    }

    //
    // Build a sorted list of eviction candidates from intermediates
    // that are not currently in use.
    //

    {
        // Sort pool indices by lastUsedFrame ascending (oldest first).
        std::vector<size_t> indices;
        indices.reserve(m_intermediatePool.size());
        for (size_t i = 0; i < m_intermediatePool.size(); i++)
        {
            if (!m_intermediatePool[i].inUse)
            {
                indices.push_back(i);
            }
        }

        // Simple selection: evict oldest intermediates first.
        // Sort by lastUsedFrame ascending.
        for (size_t a = 0; a < indices.size() && currentBytes > targetBytes; a++)
        {
            size_t oldestIdx = a;
            for (size_t b = a + 1; b < indices.size(); b++)
            {
                if (m_intermediatePool[indices[b]].lastUsedFrame <
                    m_intermediatePool[indices[oldestIdx]].lastUsedFrame)
                {
                    oldestIdx = b;
                }
            }
            if (oldestIdx != a)
            {
                std::swap(indices[a], indices[oldestIdx]);
            }

            size_t idx = indices[a];
            SIZE_T entryBytes =
                static_cast<SIZE_T>(m_intermediatePool[idx].width) *
                m_intermediatePool[idx].height * 4;

            m_intermediatePool[idx].pBitmap.Reset();
            m_intermediatePool[idx].width = 0;
            m_intermediatePool[idx].height = 0;

            currentBytes -= entryBytes;
        }

        //
        // Compact the pool vector — remove entries whose bitmap is null.
        //

        auto newEnd = std::remove_if(
            m_intermediatePool.begin(),
            m_intermediatePool.end(),
            [](const IntermediateEntry &e) { return e.pBitmap == nullptr; }
        );
        m_intermediatePool.erase(newEnd, m_intermediatePool.end());
    }

    //
    // Phase 2 — Evict bitmap cache entries, oldest first.
    //

    while (currentBytes > targetBytes && !m_bitmapCache.empty())
    {
        auto oldest = m_bitmapCache.begin();
        for (auto it = m_bitmapCache.begin(); it != m_bitmapCache.end(); ++it)
        {
            if (it->second.lastUsedFrame < oldest->second.lastUsedFrame)
            {
                oldest = it;
            }
        }

        currentBytes -= oldest->second.estimatedBytes;
        m_bitmapCache.erase(oldest);
    }

    //
    // Phase 3 — If still over budget, evict geometries (they're small,
    // but every bit helps under pressure).
    //

    if (currentBytes > targetBytes)
    {
        m_geometryCache.clear();
    }
}

// -----------------------------------------------------------------------
//  OnFrameComplete
// -----------------------------------------------------------------------

void
CD2DResourceManager::OnFrameComplete()
{
    CGuard<CCriticalSection> guard(m_csResources);

    m_currentFrame++;

    //
    // Age out bitmap cache entries that haven't been referenced in
    // CACHE_MAX_AGE_FRAMES frames (~5 seconds at 60 fps).
    //

    {
        auto it = m_bitmapCache.begin();
        while (it != m_bitmapCache.end())
        {
            if ((m_currentFrame - it->second.lastUsedFrame) > CACHE_MAX_AGE_FRAMES)
            {
                it = m_bitmapCache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    //
    // Age out geometry cache entries.
    //

    {
        auto it = m_geometryCache.begin();
        while (it != m_geometryCache.end())
        {
            if ((m_currentFrame - it->second.lastUsedFrame) > CACHE_MAX_AGE_FRAMES)
            {
                it = m_geometryCache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    //
    // Shrink the intermediate pool by releasing unused entries that
    // have not been touched in CACHE_MAX_AGE_FRAMES frames.
    //

    {
        auto newEnd = std::remove_if(
            m_intermediatePool.begin(),
            m_intermediatePool.end(),
            [this](const IntermediateEntry &e)
            {
                return !e.inUse &&
                       (m_currentFrame - e.lastUsedFrame) > CACHE_MAX_AGE_FRAMES;
            }
        );
        m_intermediatePool.erase(newEnd, m_intermediatePool.end());
    }
}

#pragma warning(pop)
