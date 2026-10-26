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
// The game draws its planar reflection into a render target of its own.  In a race that is a half-size one (IsHalfResPass and IsReflectionSizedPass); in the showroom it is as big as the
// picture, so only its camera gives it away.  For a main-pass draw whose view-projection has been recovered (`viewProj` = its registers, g_lastCameraAxes the camera): looks at whether that camera
// is the mirror of the main camera (under it, one axis turned round, game_camera.cpp) and marks or unmarks the current target; once a frame for each target, the rest is a compare.
void NoteMainSizedCamera(const float* viewProj);
// Is the game drawing its planar reflection right now: the half-size pass of a race, or a main-sized target found to be drawn through the mirrored camera?
bool IsReflectionPass();
// The mirrored camera's turned-round axis when the current target is a main-sized reflection target, -1 otherwise.
int MainSizedReflectionAxis();
// True when the current target is a main-sized reflection target (the draws into it do not describe the main camera).
bool IsMainSizedReflectionTarget();
// The device was reset: the surfaces are gone.
void ResetMainSizedReflectionTarget();
