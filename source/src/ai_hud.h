// The game's AI HUD (UIAiHud): the markers over the AI cars (name, weapon, health, lock-on).  The game places each one in the WORLD, above its car,
// and draws it in 2D; BlurVR shows the 2D UI on a flat panel.  This module hooks the function that places them, so that the world position of every
// marker is known; with the VR half running each marker is shown over its car instead (AiHudComposeMarkers).  NOTES.md section 26.
#pragma once
#include "common.h"

// Called once per presented frame (from H_Present).  Installs the hook the first time, when the VR half runs and the function is found in this Blur.exe.
void AiHudPoll();

// World markers: called from ComposeHudLayers once the UI has been drawn into the HUD layer, before the flat panel is drawn.  Takes the
// part of the layer around each drawn AI marker, shows it over its car in both eyes (a billboard at the car's depth, facing the head), and clears it from the layer
// and from the game's glow of its UI (glowSurface, which may be NULL) so that the panel does not show it as well.  Does nothing unless the markers can be placed.
void AiHudComposeMarkers(IDirect3DDevice9* device, IDirect3DTexture9* hudTexture, IDirect3DSurface9* hudSurface, IDirect3DSurface9* glowSurface, UINT glowWidth,
                         UINT glowHeight);
