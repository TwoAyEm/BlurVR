// Upscaling of the race picture on the Direct3D 11 side ([VR] Upscale, NOTES.md section 32).  The game renders at the size of [VR] Resolution; this makes a larger picture
// for the headset with an edge-adaptive upscale (the published algorithm of AMD's FidelityFX Super Resolution 1.0, "EASU", MIT licence, see LICENSE_fsr.txt) followed by
// a contrast adaptive sharpening ("RCAS").  Each eye is done alone so that nothing is taken across the seam between the two halves.
#pragma once
#include "common.h"

// The size of the upscaled picture for a render of w x h: `factor` times as large, the width even (each eye gets half of it).
void UpscaleOutputSize(UINT w, UINT h, double factor, UINT* outW, UINT* outH);

// `source` is the finished picture, two eyes side by side, srcW x srcH, a texture with a shader resource view (the one the game rendered into, opened in Direct3D 11).
// Returns the upscaled picture (dstW x dstH, B8G8R8A8, the same format as the source) or NULL when it cannot be made (the log says why, once); the texture is owned here and
// stays valid until the next call.  sharpness 0 = no sharpening, 1 = the strongest.  The work is queued on `context`, which the caller flushes.
ID3D11Texture2D* UpscaleFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT srcW, UINT srcH, UINT dstW, UINT dstH, double sharpness);

void ReleaseUpscale();
