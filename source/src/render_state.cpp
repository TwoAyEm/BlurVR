// What the game is rendering into right now (render target, viewport, scissor, bound textures) and classification of the current pass (main scene, half-size, reflection).
#include "render_state.h"
#include "config.h"

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
