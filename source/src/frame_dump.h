// Save what is sent to the headset as .bmp files in the BlurVR folder, so a picture that looks wrong in the headset can be looked
// at on the PC (and attached to a bug report).  A key press (numpad 8 by default, [Debug] KeyDump) saves the finished side-by-side
// picture and the HUD, top and multiply layers; at most 12 sets per run.
#pragma once
#include "common.h"

// Called once per presented frame with the finished back buffer, after the HUD layers were composed into it.
void FrameDumpPoll(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);
