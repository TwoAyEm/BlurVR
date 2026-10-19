// Photo viewer (main menu): the game saves its photos from the frame it rendered, which in VR is the side-by-side picture
// (left eye | right eye), and the viewer then draws the photo straight to the back buffer.  There it would sit on the lenses,
// head-locked and huge.  The viewer's draw is run once per eye into the per-eye HUD layers instead, each eye getting its own
// half of the photo, and the layers are shown on the world-locked HUD panel.
#pragma once
#include "common.h"

// Is the current draw the photo viewer's picture?
bool IsPhotoDraw();
// Run it once per eye into the per-eye layers (instead of onto the back buffer).
HRESULT DrawPhotoStereo(IDirect3DDevice9* device, const DrawCall& draw);
// Free the per-eye photo textures (before a device Reset).
void ReleasePhotoStereo();
