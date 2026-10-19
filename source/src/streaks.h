// Light-streak sprites: captured in a layer and re-projected into each eye.
#pragma once
#include "common.h"

extern bool g_streakUsed;
// Free the streak layer (before a device Reset).
void ReleaseStreakLayer();
// Create the off-screen layer for light streaks.
bool EnsureStreakLayer(IDirect3DDevice9* device);
// Run a game streak draw into the streak layer.
HRESULT DrawStreakToLayer(IDirect3DDevice9* device, const DrawCall& draw);
// Re-project the collected streaks into each eye.
void FlushStreakLayer(IDirect3DDevice9* device);
