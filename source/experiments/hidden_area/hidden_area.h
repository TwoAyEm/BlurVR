// Hidden-area cull (experiment, [Render] HiddenAreaCull=1): the lenses never show about a quarter of each eye's flat image (the corners
// and the nose side; SteamVR's hidden-area mesh).  Before the main pass draws, those pixels get a depth that fails every later depth test of
// the pass, so the geometry shaders do not run there.  The depth buffer is stamped lazily, once after each depth clear of the main target.
//
// Design notes (section 17 of NOTES.md): depth, not stencil, because a depth stamp costs nothing per draw and the game's own stencil use is
// unknown; the mask is built from SteamVR's mesh as a grid of rectangles, hidden under both vertical orientations of the mesh and eroded by
// a margin, so a wrong guess about the mesh convention or the sampling of later passes never reaches visible pixels; the stamp's state is
// saved and restored by hand (an earlier version that leaked state turned the cars black).
#pragma once
#include "common.h"

// The game cleared the depth buffer of the main-sized target (z = the value it cleared to).
void HiddenAreaNoteDepthClear(float z);
// A main-pass scene draw is about to run: stamp the depth buffer if it needs it.
void HiddenAreaBeforeMainDraw(IDirect3DDevice9* device);
