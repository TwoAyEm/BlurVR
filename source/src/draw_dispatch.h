// Per-draw-call dispatch: decide how each game draw is handled and run it.
#pragma once
#include "common.h"

// Entry point of every game draw call: decide how to handle it (once, per eye, into a HUD layer, skipped...) and execute it through draw().
HRESULT RunDraw(IDirect3DDevice9* device, const DrawCall& draw);

// A menu on top of a race (pause, results): the game dims the race with a blended UI-shader fill that covers most of the screen.  Found in the draw stream
// (an occlusion query around the real draw, flat; the HUD path's pause dim, VR), so it works with or without VR.  Used by the menu key repeat limiter
// (input_limiter.cpp), which needs to know when the arrow keys navigate instead of steering.
bool RaceMenuOverlay();
// Once per presented frame: pick up the finished queries and settle whether the overlay is up (a covering fill for 250 ms) or down (none for 150 ms).
void RaceMenuOverlayEndFrame();
// The occlusion queries are default-pool objects: released before a Reset.
void ReleaseRaceMenuQueries();
