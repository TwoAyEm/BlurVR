// Per-draw-call dispatch: decide how each game draw is handled and run it.
#pragma once
#include "common.h"

// Entry point of every game draw call: decide how to handle it (once, per eye, into a HUD layer, skipped...) and execute it through draw().
HRESULT RunDraw(IDirect3DDevice9* device, const DrawCall& draw);
