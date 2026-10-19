// What the game is rendering into right now (render target, viewport, scissor, bound textures) and classification of
// the current pass (main scene, half-size, reflection).
#pragma once
#include "common.h"

// Size of the current render target.
extern UINT g_rtWidth;
extern UINT g_rtHeight;
// Size of the back buffer.
extern UINT g_backBufferWidth;
extern UINT g_backBufferHeight;
extern bool g_hasDepthBuffer;
// Render target 0 as last set by the game.
extern IDirect3DSurface9* g_renderTarget0;
// The device back buffer.
extern IDirect3DSurface9* g_backBufferSurface;
// Viewport as the game last set it.
extern D3DVIEWPORT9 g_gameViewport;
extern BOOL g_gameScissorEnabled;
extern RECT g_gameScissorRect;
// Textures currently bound to the 16 pixel shader samplers.
extern IDirect3DBaseTexture9* g_boundTextures[16];
// Textures that the game created as render targets.
extern std::unordered_set<void*> g_renderTargetTextures;
// Re-read the render target, depth buffer and viewport from the device.
void RefreshRenderTargetState(IDirect3DDevice9* device);
// Is the current target about as big (and the same shape) as the back buffer?
bool IsMainSizedTarget();
// A texture of the shape the back buffer has now or has had earlier in this run (aspect within 3%), whatever its size: the scene, bloom
// and glow buffers are, the rear-view mirror picture is not.  Needed because a back buffer wider than 3:1 (a window clamped to a
// two-monitor desktop) is "wide and short" too, and the game keeps buffers of that shape after a Reset to another size.
bool HasBackBufferShape(UINT width, UINT height);
// Is it a half/quarter-size pass that draws 3D geometry (glow and bloom sources)?
bool IsHalfResPass();
// Is it the half-size planar-reflection pass?
bool IsReflectionSizedPass();
// Is the game drawing the main 3D scene right now?
bool IsMainPass();
