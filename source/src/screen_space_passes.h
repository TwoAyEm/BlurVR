// Per-eye versions of the game's screen-space passes: motion blur, shadow decals, deferred lights, and the bloom
// switch.
#pragma once
#include "common.h"

// Run the final composite draw with the quarter-size bloom texture replaced by black (bloom off).
HRESULT DrawWithoutBloom(IDirect3DDevice9* device, const DrawCall& draw);
// Run the full-screen motion blur once per eye with a per-eye velocity remap; with [VR] MotionBlur=0 the pass is drawn once as a plain copy of the picture.
HRESULT MotionBlurPerEye(IDirect3DDevice9* device, const DrawCall& draw);
// Adjust a shadow decal draw for one eye; false when the draw is not a decal.
bool ApplyShadowDecalForEye(IDirect3DDevice9* device, int eye, float* savedConstants);
// Find the pixel shader register holding the frustum far corners (deferred lights).
int FindFarCornerRegister();
// Run a deferred light pass once per eye, with far corners/positions rebuilt for that eye.
HRESULT DrawLightsPerEye(IDirect3DDevice9* device, const DrawCall& draw, int farReg);
// Adjust a light volume draw for one eye.
bool ApplyLightVolumeForEye(IDirect3DDevice9* device, int eye, float* saved);
