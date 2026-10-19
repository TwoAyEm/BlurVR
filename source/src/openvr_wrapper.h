// The OpenVR C API header, wrapped so it compiles as C++ and does not try to load openvr_api.dll by itself
// (BlurVR loads it at run time from the BlurVR folder).
#pragma once

#define OPENVR_API_NODLL
// openvr_capi.h does "typedef char bool" when __WIN32 is defined; that breaks C++.
#pragma push_macro("__WIN32")
#undef __WIN32
#include "openvr_capi.h"
#pragma pop_macro("__WIN32")
