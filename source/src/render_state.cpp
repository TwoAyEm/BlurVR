// What the game is rendering into right now (render target, viewport, scissor, bound textures) and classification of the current pass (main scene, half-size, reflection).
#include "render_state.h"
#include "camera.h"
#include "config.h"
#include "core.h"
#include "game_camera.h"
#include "vr_runtime.h"

UINT g_rtWidth;
UINT g_rtHeight;
UINT g_backBufferWidth;
UINT g_backBufferHeight;

bool g_hasDepthBuffer = true;
IDirect3DSurface9* g_renderTarget0 = NULL;     // pointer identity only (not ref-counted)
IDirect3DSurface9* g_backBufferSurface = NULL; // pointer identity only
D3DVIEWPORT9 g_gameViewport;
BOOL g_gameScissorEnabled = FALSE;
RECT g_gameScissorRect = {0, 0, 0, 0};

IDirect3DBaseTexture9* g_boundTextures[16];
std::unordered_set<void*> g_renderTargetTextures;

// Shapes (width / height) the back buffer has had since the game started.  The game keeps making some of its buffers in the shape of
// the first back buffer after a Reset to a different one (a start clamped to 3.6:1 left a 960x267 glow target in a 1920x1080 game).
static double g_seenAspects[8];
static int g_seenAspectCount = 0;
static void NoteBackBufferShape()
{
    if (!g_backBufferWidth || !g_backBufferHeight)
        return;
    double aspect = (double)g_backBufferWidth / g_backBufferHeight;
    for (int i = 0; i < g_seenAspectCount; i++)
        if (fabs(aspect - g_seenAspects[i]) < 0.03 * g_seenAspects[i])
            return;
    if (g_seenAspectCount < 8)
        g_seenAspects[g_seenAspectCount++] = aspect;
}

void RefreshRenderTargetState(IDirect3DDevice9* device)
{
    IDirect3DSurface9* surface = NULL;
    if (SUCCEEDED(device->GetRenderTarget(0, &surface)) && surface) {
        D3DSURFACE_DESC surfaceDesc;
        if (SUCCEEDED(surface->GetDesc(&surfaceDesc))) {
            g_rtWidth = surfaceDesc.Width;
            g_rtHeight = surfaceDesc.Height;
        }
        g_renderTarget0 = surface;
        surface->Release();
    }
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface) {
        D3DSURFACE_DESC surfaceDesc;
        if (SUCCEEDED(surface->GetDesc(&surfaceDesc))) {
            g_backBufferWidth = surfaceDesc.Width;
            g_backBufferHeight = surfaceDesc.Height;
            NoteBackBufferShape();
        }
        g_backBufferSurface = surface;
        surface->Release();
    }
    IDirect3DSurface9* depthSurface = NULL;
    g_hasDepthBuffer = (SUCCEEDED(device->GetDepthStencilSurface(&depthSurface)) && depthSurface);
    if (depthSurface)
        depthSurface->Release();
    device->GetViewport(&g_gameViewport);
    DWORD scissorEnabled = 0;
    if (SUCCEEDED(device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorEnabled)))
        g_gameScissorEnabled = scissorEnabled ? TRUE : FALSE;
    device->GetScissorRect(&g_gameScissorRect);
}

bool IsMainSizedTarget()
{
    if (!g_rtWidth || !g_backBufferWidth)
        return false;
    if (g_rtWidth < g_cfg.mainPassMinWidthFraction * g_backBufferWidth)
        return false;
    double aspect = (double)g_rtWidth / g_rtHeight, backBufferAspect = (double)g_backBufferWidth / g_backBufferHeight;
    return fabs(aspect - backBufferAspect) < 0.03 * backBufferAspect;
}

bool HasBackBufferShape(UINT width, UINT height)
{
    if (!width || !height || !g_backBufferWidth || !g_backBufferHeight)
        return false;
    double aspect = (double)width / height, backBufferAspect = (double)g_backBufferWidth / g_backBufferHeight;
    if (fabs(aspect - backBufferAspect) < 0.03 * backBufferAspect)
        return true;
    for (int i = 0; i < g_seenAspectCount; i++)
        if (fabs(aspect - g_seenAspects[i]) < 0.03 * g_seenAspects[i])
            return true;
    return false;
}

// Half-resolution passes that draw 3D geometry (the lens-flare / glow sprite buffer is rendered at half size and
// composited over the full image). They must hold the same side-by-side layout as the main image, so they are
// stereo-duplicated too.
bool IsHalfResPass()
{
    if (!g_cfg.stereoHalfResPasses || !g_rtWidth || !g_backBufferWidth)
        return false;
    double widthRatio = (double)g_rtWidth / g_backBufferWidth;
    if (widthRatio < 0.20 || widthRatio > 0.55)
        return false; // half (960) and quarter (480) resolution glow/bloom-source passes
    double aspect = (double)g_rtWidth / g_rtHeight, backBufferAspect = (double)g_backBufferWidth / g_backBufferHeight;
    return fabs(aspect - backBufferAspect) < 0.03 * backBufferAspect;
}

bool IsReflectionSizedPass() // only the half-size planar-reflection pass gets the mirrored-camera treatment
{
    if (!g_rtWidth || !g_backBufferWidth)
        return false;
    double widthRatio = (double)g_rtWidth / g_backBufferWidth;
    return widthRatio >= 0.45 && widthRatio <= 0.55;
}

// ---- the planar reflection in a target as big as the picture ----------------------------------------------------------------------------
static IDirect3DSurface9* g_mirroredTarget = NULL; // a main-sized target the mirrored camera draws into (pointer identity only)
static int g_mirroredAxis = -1;
static IDirect3DSurface9* g_testedTarget = NULL;
static unsigned long long g_testedFrame = ~0ull;

void NoteMainSizedCamera(const float* viewProj)
{
    if (g_renderTarget0 == g_testedTarget && g_vr.frameCount == g_testedFrame)
        return; // (this runs for every draw of the main pass: one look at a target a frame)
    g_testedTarget = g_renderTarget0;
    g_testedFrame = g_vr.frameCount;
    int axis = -1;
    double depth = 0;
    MainCameraPose main;
    if (g_lastCameraAxes.valid && g_matrixLayout && viewProj && GetMainCameraPose(main) && g_vr.frameCount <= main.frame + 3) {
        // where this draw's camera is: the translation row of the view-projection holds -position . (right, up, forward) (scaled by the projection's x and y)
        Matrix4 matrix;
        LoadMatrix(viewProj, g_matrixLayout, matrix);
        const double along[3] = {-matrix.m[3][0] / g_projXScale, -matrix.m[3][1] / g_projYScale, -matrix.m[3][3]};
        double position[3];
        for (int i = 0; i < 3; i++)
            position[i] = g_lastCameraAxes.right[i] * along[0] + g_lastCameraAxes.up[i] * along[1] + g_lastCameraAxes.forward[i] * along[2];
        depth = main.pos[1] - position[1];
        if (depth >= 0.3 && fabs(position[0] - main.pos[0]) <= 15 && fabs(position[2] - main.pos[2]) <= 15) { // (straight under the main camera, as the water is under the car)
            const double mirrored[3][3] = {{g_lastCameraAxes.right[0], g_lastCameraAxes.right[1], g_lastCameraAxes.right[2]},
                                           {g_lastCameraAxes.up[0], g_lastCameraAxes.up[1], g_lastCameraAxes.up[2]},
                                           {g_lastCameraAxes.forward[0], g_lastCameraAxes.forward[1], g_lastCameraAxes.forward[2]}};
            const double camera[3][3] = {{main.right[0], main.right[1], main.right[2]}, {main.up[0], main.up[1], main.up[2]}, {main.fwd[0], main.fwd[1], main.fwd[2]}};
            axis = MirrorFlipAxis(mirrored, camera);
        }
    }
    if (axis >= 0) {
        static unsigned logged = 0;
        if (g_mirroredTarget != g_renderTarget0 && logged++ < 4)
            Log("reflection target: the %ux%u render target %p is drawn through the mirrored camera (%.1f m under the main camera, axis %c turned round): its draws are the planar reflection", g_rtWidth,
                g_rtHeight, (void*)g_renderTarget0, depth, "XYZ"[axis]);
        g_mirroredTarget = g_renderTarget0;
        g_mirroredAxis = axis;
    } else if (g_mirroredTarget && g_mirroredTarget == g_renderTarget0) {
        g_mirroredTarget = NULL;
        g_mirroredAxis = -1;
    }
}

bool IsMainSizedReflectionTarget()
{
    return g_mirroredTarget && g_renderTarget0 == g_mirroredTarget;
}

int MainSizedReflectionAxis()
{
    return IsMainSizedReflectionTarget() ? g_mirroredAxis : -1;
}

bool IsReflectionPass()
{
    return (IsHalfResPass() && IsReflectionSizedPass()) || IsMainSizedReflectionTarget();
}

void ResetMainSizedReflectionTarget()
{
    g_mirroredTarget = g_testedTarget = NULL;
    g_mirroredAxis = -1;
    g_testedFrame = ~0ull;
}

bool IsMainPass()
{
    if (!g_rtWidth || !g_backBufferWidth)
        return false;
    if (g_cfg.requireDepthBuffer && !g_hasDepthBuffer)
        return false;
    if (g_rtWidth < g_cfg.mainPassMinWidthFraction * g_backBufferWidth)
        return false;
    double aspect = (double)g_rtWidth / g_rtHeight, backBufferAspect = (double)g_backBufferWidth / g_backBufferHeight;
    return fabs(aspect - backBufferAspect) < 0.03 * backBufferAspect;
}
