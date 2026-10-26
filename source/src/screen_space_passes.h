// Per-eye versions of the game's screen-space passes: motion blur, shadow decals, deferred lights, and the bloom
// switch.
#pragma once
#include "common.h"
#include "matrix.h"
#include "vr_runtime.h"

// Run the final composite draw with the quarter-size bloom texture replaced by black (bloom off).
HRESULT DrawWithoutBloom(IDirect3DDevice9* device, const DrawCall& draw);
// Run the full-screen motion blur once per eye with a per-eye velocity remap; with [VR] MotionBlur=0 the pass is drawn once as a plain copy of the picture.
HRESULT MotionBlurPerEye(IDirect3DDevice9* device, const DrawCall& draw);
// The motion blur's g_mMatrix (original: c0..c3, rows u, v, depth, 1; columns x, y, w used) for one eye: the game's camera motion as that eye sees it, whichever way the head looks.
// `delta` is the eye's camera delta (game camera -> eye), projX / projY the game's projection scales; false when something is degenerate.  Writes 16 floats to `out`.
bool BlurMatrixForEye(const float* original, const EyeBox& box, int eye, const Matrix4& delta, double projX, double projY, float* out);
// The same for the blur shaders that take the pixel's (u, v, depth, 1) as the depth buffer holds it (c + d/Z with Z the distance along the view axis; depthC, depthD = c, d).
bool BlurMatrixForEyeRawDepth(const float* original, const EyeBox& box, int eye, const Matrix4& delta, double projX, double projY, double depthC, double depthD, float* out);
// Adjust a shadow decal draw for one eye; false when the draw is not a decal.
bool ApplyShadowDecalForEye(IDirect3DDevice9* device, int eye, float* savedConstants);
// Find the pixel shader register holding the frustum far corners (deferred lights).
int FindFarCornerRegister();
// Run a deferred light pass once per eye, with far corners/positions rebuilt for that eye.
HRESULT DrawLightsPerEye(IDirect3DDevice9* device, const DrawCall& draw, int farReg);
// Adjust a light volume draw for one eye.
bool ApplyLightVolumeForEye(IDirect3DDevice9* device, int eye, float* saved);
