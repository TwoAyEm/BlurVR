// The IDirect3DDevice9 hooks: state tracking, shader tracking, draw interception, Present/Reset.
#pragma once
#include "common.h"

// Patch the IDirect3DDevice9 vtable of a freshly created device.
void HookDevice(IDirect3DDevice9* dev);
