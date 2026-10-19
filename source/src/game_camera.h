// Hooks on the game's own code where the main view is given its camera (NOTES.md section 25).  The game culls with the frustum it
// hands over there, long before the first Direct3D call, so this is the place to make it cull for the head instead of its own camera.
#pragma once
#include "common.h"

// Called once per presented frame.  Installs the hooks the first time, when [Culling] HeadFrustum=1 and this is the Blur.exe
// build the addresses were read from.
void GameCameraPoll();
