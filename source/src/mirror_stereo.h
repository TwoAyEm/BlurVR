// Stereo rear-view mirror: the game renders the mirror picture into a small wide render target.  Its 3D
// draws are repeated once per eye, each into one half of that target, with the camera moved to that eye; the two halves
// are then copied into one texture per eye, which the UI draw of the mirror picture samples when it fills the
// per-eye mirror layers (see RunDraw and ComposeHudLayers).
#pragma once
#include "common.h"

// Remember the main camera's orientation (call after a main-pass draw was recognised).
void NoteMainCameraAxes();
// Is the current draw a 3D draw into the mirror render target?
bool IsMirrorScenePass();
// The mirror scene was drawn in stereo this frame or one of the last two (the mirror's UI draw may come before the scene in a frame: the lowest quality of the game composes its UI first).
bool MirrorSceneRecent();
// Run that draw once per eye.
HRESULT DrawMirrorSceneStereo(IDirect3DDevice9* device, const DrawCall& draw);
// Copy the two eye halves of the mirror texture into the per-eye textures (once per frame).  False when the mirror
// was not rendered in stereo this frame; then `g_mirrorEyeTexture` must not be used.
bool PrepareMirrorEyeTextures(IDirect3DDevice9* device, IDirect3DBaseTexture9* mirrorTexture);
extern IDirect3DTexture9* g_mirrorEyeTexture[2];
// Free the per-eye textures (before a device Reset).
void ReleaseMirrorStereo();
