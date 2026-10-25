// The OpenXR header, wrapped like openvr_wrapper.h: only the Direct3D 11 binding, and no prototypes (nothing is linked: BlurVR loads
// openxr_loader.dll from the BlurVR folder at run time and gets every function from it).
#pragma once
#include "common.h"

#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
