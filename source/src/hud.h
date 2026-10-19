// HUD / UI layers: draw the game's 2D UI into off-screen layers and compose them into each eye as a world-locked panel.
#pragma once
#include "common.h"

struct ScreenVertex {
    float x, y, z, rhw, u, v;
};

extern int g_hudDrawsThisFrame;
extern unsigned long long g_backdropDropTime;
extern bool g_sceneCompositeDrawn;
extern bool g_menuDialogToTopLayer;
extern bool g_topVeilRequested;
extern bool g_loadingVideoDropped;
extern bool g_lateUiToTopLayer;
extern int g_topVeilAlpha;
extern bool g_postBackdropToTopLayer;
extern int g_lateBackdropDrops;
extern bool g_dimStrongRequested;
extern bool g_dimStartWindow;
extern bool g_dimRequested;
extern bool g_menuDimLast;
extern bool g_menuGlowFrame;
// Where a HUD layer lands inside the side-by-side image for the given eye.
D3DVIEWPORT9 ComputeHudViewport(const D3DVIEWPORT9& base, int eye);
extern IDirect3DSurface9* g_hudSurface;
extern IDirect3DSurface9* g_topLayerSurface;
extern bool g_topLayerUsed;
extern IDirect3DSurface9* g_multiplySurface;
extern IDirect3DSurface9* g_mirrorSurface[2]; // per-eye layers for the 3D mirror
extern bool g_mirrorUsed[2];
extern bool g_hudUsed;
extern unsigned long long g_gutterClearedFrame;
// Free the HUD layers (before a device Reset).
void ReleaseHud();
// Create the off-screen HUD, top and multiply layers if they do not exist yet.
bool EnsureHudTargets(IDirect3DDevice9* device);
// A coverage measurement that does not wait: the draw runs into the scratch target with this slot's own occlusion query, and the
// result is picked up later with HudCoveragePoll (a frame or two on).  One slot per UI draw position in the frame.
struct CoverageSlot {
    IDirect3DQuery9* query;
    bool pending;
    double area;                  // pixels the draw could cover, when it was issued
    int ps, vs;                   // what the measured draw was (the result belongs to that draw only)
    void* tex0;
    unsigned long long issuedFrame;
};
CoverageSlot& HudCoverageSlot(unsigned ordinal); // ordinal < 64
bool HudCoverageIssue(IDirect3DDevice9* device, const D3DVIEWPORT9& base, CoverageSlot& slot, const DrawCall& draw);
bool HudCoveragePoll(CoverageSlot& slot, double* frac); // true once the result is back (the slot is free again)
bool HudDrawCoversScreen(IDirect3DDevice9* device, const D3DVIEWPORT9& base, double minFrac, double* outFrac,
                         const DrawCall& draw);
// Run a game draw into the HUD layer instead of the screen.
HRESULT DrawHudToTexture(IDirect3DDevice9* device, const D3DVIEWPORT9& base, const DrawCall& draw);
// Run a game draw into the layer that is later multiplied over the scene.
HRESULT DrawHudToMultiplyLayer(IDirect3DDevice9* device, const D3DVIEWPORT9& base, const DrawCall& draw);
// For a UI draw that is dropped: if it writes the stencil (one of the game's clip masks) keep that effect, nothing else.
HRESULT DrawHudStencilOnly(IDirect3DDevice9* device, const D3DVIEWPORT9& base, const DrawCall& draw);
// The game cleared the stencil buffer of its UI: clear the layers' stencil buffer too.
void OnGameStencilClear(IDirect3DDevice9* device, DWORD value);
// Draw the finished HUD layers into each eye as a world-locked panel (called before Present).
// Draws the UI layers as world-locked panels. With a target, they go into that surface instead of the back buffer (used to put
// menu UI into the game's colour buffer before its bloom and tone-mapping passes, as the game itself does).
void ComposeHudLayers(IDirect3DDevice9* device);
void NoteSceneBloom(IDirect3DBaseTexture9* bloom); // the game's own bloom buffer of this frame (added over the menu layers)
// Is the current blend state a multiply (dst * src)?
bool IsMultiplicativeBlend(IDirect3DDevice9* device);
