# D2D/D3D11 Backend — Integration Changes Required in Existing WPF Files

This document describes the **exact** modifications needed in the existing WPF
native and managed source files to wire up the new D2D rendering backend.
Each section names the file, quotes the relevant existing code, and shows the
required patch.

---

## 1. `core/targets/desktoprt.cpp` — Route to D2D in `CDesktopRenderTarget::Init()`

### Include the new header

At the top of the file, after the existing `#include "precomp.hpp"`:

```cpp
// ---- ADD ----
#include "d2d_factory.h"
// ---- END ADD ----
```

### Add D2D path before the adapter loop

Inside `CDesktopRenderTarget::Init()`, just **before** the comment and `for`
loop that enumerates adapters:

```
    //
    // Create all of the render targets
    //
    for (UINT i = 0; i < m_cRT; i++)
```

Insert the following block:

```cpp
    // ---- ADD: D2D/D3D11 fast path ----
    //
    // If the managed layer has opted in to D2D rendering and the system
    // supports it, attempt to create a D2D display render target covering
    // the entire desktop.  On success we use a single sub-RT (matching
    // the existing UpdateLayeredWindow-single-Sw-RT pattern via
    // SetSingleSubRT) and skip the per-adapter D3D9/Sw loop.
    //

    if (D2DRendering_ShouldUse())
    {
        MetaData &d2dMetadata = m_rgMetaData[0];
        CDisplay const *pDisplay = DisplaySet()->Display(0);
        Assert(pDisplay);

        IRenderTargetHWNDInternal *pD2DRTHWND = NULL;

        MIL_THR(D2DDisplayRenderTarget_Create(
            hwnd,
            eWindowLayerType,
            pDisplay,
            dwFlags,
            &pD2DRTHWND
            ));

        if (SUCCEEDED(hr))
        {
            //
            // D2D creation succeeded.
            // Wire the D2D render target into the meta data and skip
            // the D3D9/Sw adapter loop entirely.
            //

            d2dMetadata.pInternalRTHWND = pD2DRTHWND;

            // pInternalRT keeps its own ref
            d2dMetadata.pInternalRT = NULL;
            hr = pD2DRTHWND->QueryInterface(
                __uuidof(IRenderTargetInternal),
                reinterpret_cast<void **>(&d2dMetadata.pInternalRT)
                );

            if (SUCCEEDED(hr))
            {
                // No HW/SW sub-target — null them out so the destructor
                // does not try to release them.
                d2dMetadata.pHwDisplayRT = NULL;
                d2dMetadata.pSwHWNDRT   = NULL;

                // Collapse to a single sub-RT that covers the desktop.
                SetSingleSubRT();

                // Jump past the per-adapter loop to the common tail.
                goto D2DSuccess;
            }

            // QI failed — release and fall through to D3D9 path.
            pD2DRTHWND->Release();
            pD2DRTHWND = NULL;
        }

        // D2D path failed — reset hr and fall through to D3D9 path.
        hr = S_OK;
    }

    // ---- END ADD ----
```

And after the existing `IFC(EditMetaData());` line near the end of `Init()`,
add the D2D success label:

```cpp
    IFC(EditMetaData());

    // ---- ADD ----
D2DSuccess:
    // ---- END ADD ----

Cleanup:
```

---

## 2. `core/common/renderoptions.h` — Forward-declare D2D option export

No changes needed in this file. The D2D option state lives in
`d2d_renderoptions.h` / `d2d_renderoptions.cpp` to maintain separation.

---

## 3. `core/common/renderoptions.cpp` — No changes

The new exported functions (`RenderOptions_SetD2DRenderingEnabled`,
`RenderOptions_IsD2DRenderingEnabled`) are defined in `d2d_renderoptions.cpp`
following the same pattern. No edits to the existing file are necessary.

---

## 4. `core/dll/wpfgfx.def` — Add new exports

Append the following lines after the existing `RenderOptions_*` exports:

```def
    RenderOptions_SetD2DRenderingEnabled
    RenderOptions_IsD2DRenderingEnabled
```

The full tail of the file should read:

```def
    RenderOptions_ForceSoftwareRenderingModeForProcess
    RenderOptions_IsSoftwareRenderingForcedForProcess
    RenderOptions_EnableHardwareAccelerationInRdp

    RenderOptions_SetD2DRenderingEnabled
    RenderOptions_IsD2DRenderingEnabled
```

---

## 5. `core/meta/metart.h` — Add `pD2DDisplayRT` to `MetaData` union

Inside the `MetaData` struct, in the `CDesktopRenderTarget` arm of the union,
add a pointer for the D2D display render target:

### Before

```cpp
    // Used by CDesktopRenderTarget
    struct {
        IRenderTargetHWNDInternal *pInternalRTHWND;
        CHwDisplayRenderTarget *pHwDisplayRT;
        CSwRenderTargetHWND *pSwHWNDRT;

        // Bounds of device in virtual coordinate space.
        CMILSurfaceRect rcVirtualDeviceBounds;

        // Bounds of target area that is expected to have some valid content.
        CMILSurfaceRect rcLocalDeviceValidContentBounds;
    };
```

### After

```cpp
    // Used by CDesktopRenderTarget
    struct {
        IRenderTargetHWNDInternal *pInternalRTHWND;
        CHwDisplayRenderTarget *pHwDisplayRT;
        CSwRenderTargetHWND *pSwHWNDRT;
        CD2DDisplayRenderTarget *pD2DDisplayRT;       // <-- NEW

        // Bounds of device in virtual coordinate space.
        CMILSurfaceRect rcVirtualDeviceBounds;

        // Bounds of target area that is expected to have some valid content.
        CMILSurfaceRect rcLocalDeviceValidContentBounds;
    };
```

You will also need a forward declaration at the top of `metart.h`:

```cpp
class CD2DDisplayRenderTarget;
```

And update `CDesktopRenderTarget::~CDesktopRenderTarget()` in `desktoprt.cpp`
to release the new pointer:

```cpp
    for (UINT i = 0; i < m_cRT; i++)
    {
        // ... existing pHwDisplayRT release ...

        if (m_rgMetaData[i].pD2DDisplayRT)
        {
            m_rgMetaData[i].pD2DDisplayRT->Release();
        }

        // ... existing pSwHWNDRT release ...
    }
```

And in the `#if DBG` assertion block in `Init()`:

```cpp
    Assert(metadata.pD2DDisplayRT == NULL);
```

---

## 6. DLL Initialization (`core/dll/dllentry.cpp` or equivalent DLL main)

Call `D2DSubsystem::Init()` during DLL load and `D2DSubsystem::DeInit()`
during DLL unload.

### Startup (after `RenderOptions::Init()`)

```cpp
#include "d2d_init.h"

// In DLL_PROCESS_ATTACH or equivalent startup path:
IFC(RenderOptions::Init());
IFC(D2DSubsystem::Init());          // <-- NEW
```

### Shutdown (before `RenderOptions::DeInit()`)

```cpp
// In DLL_PROCESS_DETACH or equivalent shutdown path:
D2DSubsystem::DeInit();              // <-- NEW
RenderOptions::DeInit();
```

---

## 7. Managed Side — `PresentationCore/System/Windows/Media/RenderOptions.cs`

Add a new property that reads the `AppContext` switch and calls the native
P/Invoke.

### P/Invoke declarations (add alongside existing `SecurityCritical` imports)

```csharp
[DllImport(DllImport.MilCore, EntryPoint = "RenderOptions_SetD2DRenderingEnabled")]
internal static extern void SetD2DRenderingEnabled(bool fEnabled);

[DllImport(DllImport.MilCore, EntryPoint = "RenderOptions_IsD2DRenderingEnabled")]
[return: MarshalAs(UnmanagedType.Bool)]
internal static extern bool IsD2DRenderingEnabled();
```

### Public property

```csharp
private const string UseD2DRenderingSwitchName =
    "Switch.System.Windows.Media.UseD2DRendering";

/// <summary>
///     Gets or sets whether the D2D/D3D11 rendering backend is used
///     instead of the legacy D3D9 backend.  This is an opt-in switch.
/// </summary>
public static bool UseD2DRendering
{
    get
    {
        // Check the AppContext switch first, then query native state.
        if (AppContext.TryGetSwitch(UseD2DRenderingSwitchName, out bool isEnabled))
        {
            return isEnabled;
        }

        return IsD2DRenderingEnabled();
    }
    set
    {
        AppContext.SetSwitch(UseD2DRenderingSwitchName, value);
        SetD2DRenderingEnabled(value);
    }
}
```

### Initialization path (`RenderOptions` static constructor or `MediaContext` init)

Ensure the native switch is synchronized at startup:

```csharp
static RenderOptions()
{
    // ... existing init ...

    // Synchronize AppContext switch → native
    if (AppContext.TryGetSwitch(UseD2DRenderingSwitchName, out bool useD2D) && useD2D)
    {
        SetD2DRenderingEnabled(true);
    }
}
```

---

## 8. Build System — Add new `.cpp` files

New source files must be added to the appropriate build definition
(CMakeLists.txt, sources, or `.vcxproj` depending on the WPF build flavor):

```
core/d2d/d2d_renderoptions.cpp
core/d2d/d2d_factory.cpp
core/d2d/d2d_init.cpp
```

These should compile alongside the existing `core/d2d/d2d_*.cpp` files that
are already in the build (e.g. `d2d_device.cpp`, `d2d_displayrt.cpp`, etc.).

---

## Summary of Touched Files

| File | Change |
|------|--------|
| `core/targets/desktoprt.cpp` | Add D2D fast-path before adapter loop |
| `core/dll/wpfgfx.def` | Two new exports |
| `core/meta/metart.h` | New `pD2DDisplayRT` field, forward decl |
| `core/targets/desktoprt.cpp` | Destructor: release `pD2DDisplayRT` |
| `core/dll/dllentry.cpp` | Call `D2DSubsystem::Init()` / `DeInit()` |
| `PresentationCore/.../RenderOptions.cs` | New `UseD2DRendering` property + P/Invokes |
| Build definitions | Add 3 new `.cpp` files |

## New Files Created (this changeset)

| File | Purpose |
|------|---------|
| `core/d2d/d2d_renderoptions.h` | D2D render option declarations |
| `core/d2d/d2d_renderoptions.cpp` | D2D render option impl + exported functions |
| `core/d2d/d2d_factory.h` | `D2DDisplayRenderTarget_Create` / `D2DRendering_ShouldUse` |
| `core/d2d/d2d_factory.cpp` | Implementation of above |
| `core/d2d/d2d_init.h` | `D2DSubsystem::Init()` / `DeInit()` |
| `core/d2d/d2d_init.cpp` | D2D subsystem lifecycle management |
