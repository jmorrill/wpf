#pragma once

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// D2D Rendering Backend - Master Include
//
// Include this from the main wpfgfx std.h when WPF_D2D_ENABLED is defined
//

#ifdef WPF_D2D_ENABLED

#include "d2d_precomp.h"
#include "d2d_device.h"
#include "d2d_device_manager.h"
#include "d2d_geometry.h"
#include "d2d_brush.h"
#include "d2d_bitmap.h"
#include "d2d_surfrt.h"
#include "d2d_displayrt.h"
#include "d2d_effects.h"
#include "d2d_text.h"
#include "d2d_3d.h"
#include "d2d_resources.h"
#include "d2d_renderoptions.h"
#include "d2d_factory.h"
#include "d2d_init.h"

#endif // WPF_D2D_ENABLED
