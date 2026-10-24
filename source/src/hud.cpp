// HUD / UI layers: draw the game's 2D UI into off-screen layers and compose them into each eye as a world-locked panel.
#include "hud.h"
#include <algorithm>
#include "ai_hud.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "game_mode.h"
#include "matrix.h"
#include "render_state.h"
#include "vr_runtime.h"

int g_hudDrawsThisFrame = 0; // HUD draws executed this frame

unsigned long long g_backdropDropTime = 0;
bool g_sceneCompositeDrawn = false;
bool g_menuDialogToTopLayer = false;
bool g_topVeilRequested = false;
bool g_loadingVideoDropped = false;
bool g_lateUiToTopLayer = false;
int g_topVeilAlpha = 165;
bool g_postBackdropToTopLayer = false;
int g_lateBackdropDrops = 0;
bool g_dimStrongRequested = false;
bool g_dimStartWindow = false;
bool g_dimRequested = false;
bool g_menuDimLast = false;  // the last composed frame had the pause/results dim up
bool g_menuGlowFrame = false; // this frame is a menu frame in the race (pause/results/restart): the UI's glow is wanted
static float g_dimLevel = 0.f;
static float g_dimTargetLevel = 0.f;

D3DVIEWPORT9 ComputeHudViewport(const D3DVIEWPORT9& base, int eye)
{
    // Map the game's (back-buffer sized) viewport into a correctly proportioned HUD panel inside this eye's half.
    double imageWidth = g_backBufferWidth ? g_backBufferWidth : 1920.0,
           imageHeight = g_backBufferHeight ? g_backBufferHeight : 1080.0;
    double eyeW = imageWidth / 2.0;
    const EyeBox& box = g_vr.eyeBox[eye];
    double tanWidth = fmax(1e-3, box.right - box.left), tanHeight = fmax(1e-3, box.up + box.down);
    double pxPerTan = eyeW / tanWidth;
    double panelWidth = eyeW * g_cfg.hudScale;
    double centerX, centerY; // panel centre in pixels (full SBS image)
    if (g_cfg.hudWorldLocked && g_vr.poseValid) {
        // World-locked: a point straight ahead of the game camera at hudDistance, seen through this eye's current head
        // pose.
        double distance = fmax(0.3, g_cfg.hudDistance);
        const Matrix4& delta = g_vr.eyeCameraDelta[eye];
        double vx = delta.m[3][0], vy = delta.m[3][1] + 0, vz = distance * delta.m[2][2] + delta.m[3][2];
        vx += distance * delta.m[2][0];
        vy += distance * delta.m[2][1];
        if (vz < 0.25)
            vz = 0.25;
        double tx = vx / vz, ty = vy / vz;
        panelWidth *= distance / vz;
        centerX = eye * eyeW + (tx - box.left) / tanWidth * eyeW;
        centerY = (box.up - ty) / tanHeight * imageHeight;
    } else {
        // Head-locked: centred on this eye's optical axis, converged for the apparent distance
        double shift =
            0.5 * g_vr.ipd * g_cfg.ipdScale / fmax(0.3, g_cfg.hudDistance) * pxPerTan * (eye == 0 ? 1.0 : -1.0);
        double fwd = (-box.left) / tanWidth;
        centerX = eye * eyeW + fwd * eyeW + shift;
        centerY = imageHeight / 2.0;
    }
    centerX += g_cfg.hudXOffset * eyeW * (eye == 0 ? 1.0 : -1.0);
    centerY += g_cfg.hudOffsetY * imageHeight;
    double scale = panelWidth / imageWidth, panelHeight = imageHeight * scale;
    double x0 = centerX - panelWidth / 2.0, y0 = centerY - panelHeight / 2.0;
    // keep the whole panel inside this eye's half (it sticks to the edge if you look far away from it)
    double minX = eye * eyeW, maxX = eye * eyeW + eyeW - panelWidth;
    if (maxX < minX)
        maxX = minX;
    if (x0 < minX)
        x0 = minX;
    if (x0 > maxX)
        x0 = maxX;
    double maxY = imageHeight - panelHeight;
    if (maxY < 0)
        maxY = 0;
    if (y0 < 0)
        y0 = 0;
    if (y0 > maxY)
        y0 = maxY;
    D3DVIEWPORT9 viewport = base;
    double x = x0 + base.X * scale, y = y0 + base.Y * scale;
    viewport.X = (DWORD)(x < 0 ? 0 : x);
    viewport.Y = (DWORD)(y < 0 ? 0 : y);
    viewport.Width = (DWORD)fmax(1.0, base.Width * scale);
    viewport.Height = (DWORD)fmax(1.0, base.Height * scale);
    return viewport;
}

static IDirect3DTexture9* g_hudTexture = NULL;
IDirect3DSurface9* g_hudSurface = NULL;
static IDirect3DStateBlock9* g_hudStateBlock = NULL;
static IDirect3DSurface9* g_coverageTarget = NULL;
static IDirect3DSurface9* g_coverageReadback = NULL;
static IDirect3DTexture9* g_occlusionTexture = NULL;
static IDirect3DSurface9* g_occlusionSurface = NULL;
static IDirect3DQuery9* g_occlusionQuery = NULL;
static IDirect3DTexture9* g_topLayerTexture = NULL;
IDirect3DSurface9* g_topLayerSurface = NULL;
bool g_topLayerUsed = false;
static IDirect3DBaseTexture9* g_sceneBloom = NULL; // the game's bloom buffer (referenced)
static IDirect3DTexture9* g_multiplyTexture = NULL;
static IDirect3DTexture9* g_mirrorTexture[2] = {NULL, NULL}; // per-eye 3D-mirror layers
IDirect3DSurface9* g_mirrorSurface[2] = {NULL, NULL};
bool g_mirrorUsed[2] = {false, false};
static bool g_mirrorCreated = false;
IDirect3DSurface9* g_multiplySurface = NULL;
static bool g_multiplyUsed = false;
static UINT g_hudWidth = 0;
static UINT g_hudHeight = 0;
bool g_hudUsed = false;
static bool g_hudFailed = false;
unsigned long long g_gutterClearedFrame = ~0ull;

// The game clips its UI with the stencil buffer: a mask draw writes a reference value, later draws only pass where it
// matches (the scrolling car lists, panels).  The layers get a stencil buffer of their own so that still works; the game
// uses a single one for all of its UI, so all layers share one too.
static IDirect3DSurface9* g_hudDepth = NULL;
static IDirect3DSurface9* g_uiGameDepth = NULL; // the game's own depth-stencil surface its UI was last drawn with (compared only)
static unsigned long long g_hudDepthClearedFrame = ~0ull;

static void ReleaseCoverageSlots();
void ReleaseHud()
{
    if (g_hudDepth) {
        g_hudDepth->Release();
        g_hudDepth = NULL;
    }
    g_uiGameDepth = NULL;
    g_hudDepthClearedFrame = ~0ull;
    if (g_hudStateBlock) {
        g_hudStateBlock->Release();
        g_hudStateBlock = NULL;
    }
    if (g_hudSurface) {
        g_hudSurface->Release();
        g_hudSurface = NULL;
    }
    if (g_hudTexture) {
        g_hudTexture->Release();
        g_hudTexture = NULL;
    }
    if (g_topLayerSurface) {
        g_topLayerSurface->Release();
        g_topLayerSurface = NULL;
    }
    if (g_topLayerTexture) {
        g_topLayerTexture->Release();
        g_topLayerTexture = NULL;
    }
    g_topLayerUsed = false;
    NoteSceneBloom(NULL);
    if (g_multiplySurface) {
        g_multiplySurface->Release();
        g_multiplySurface = NULL;
    }
    if (g_multiplyTexture) {
        g_multiplyTexture->Release();
        g_multiplyTexture = NULL;
    }
    if (g_coverageTarget) {
        g_coverageTarget->Release();
        g_coverageTarget = NULL;
    }
    if (g_occlusionSurface) {
        g_occlusionSurface->Release();
        g_occlusionSurface = NULL;
    }
    if (g_occlusionTexture) {
        g_occlusionTexture->Release();
        g_occlusionTexture = NULL;
    }
    if (g_occlusionQuery) {
        g_occlusionQuery->Release();
        g_occlusionQuery = NULL;
    }
    ReleaseCoverageSlots();
    if (g_coverageReadback) {
        g_coverageReadback->Release();
        g_coverageReadback = NULL;
    }
    for (int eye = 0; eye < 2; eye++) {
        if (g_mirrorSurface[eye]) {
            g_mirrorSurface[eye]->Release();
            g_mirrorSurface[eye] = NULL;
        }
        if (g_mirrorTexture[eye]) {
            g_mirrorTexture[eye]->Release();
            g_mirrorTexture[eye] = NULL;
        }
        g_mirrorUsed[eye] = false;
    }
    g_multiplyUsed = false;
    g_hudWidth = g_hudHeight = 0;
    g_hudUsed = false;
    g_hudFailed = false;
}

bool EnsureHudTargets(IDirect3DDevice9* device)
{
    bool wantMirror = true; // the per-eye layers also carry the photo viewer's picture, so they always exist
    if (g_hudTexture && g_hudWidth == g_backBufferWidth && g_hudHeight == g_backBufferHeight &&
        g_mirrorCreated == wantMirror)
        return true;
    if (g_hudFailed || !g_backBufferWidth || !g_backBufferHeight)
        return false;
    // (re)create every layer: the size or the mirror layer changed (in-game switch) or this is the first use
    IDirect3DStateBlock9* keepStateBlock = g_hudStateBlock;
    g_hudStateBlock = NULL;
    ReleaseHud();
    g_hudStateBlock = keepStateBlock;
    PFN_CreateTexture createTexture = (PFN_CreateTexture)g_deviceOriginals[VT_CreateTexture];
    UINT width = g_backBufferWidth, height = g_backBufferHeight;
    auto makeLayer = [&](IDirect3DTexture9*& texture, IDirect3DSurface9*& surface) {
        HRESULT result = createTexture(device, width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &texture, NULL);
        if (SUCCEEDED(result) && texture)
            texture->GetSurfaceLevel(0, &surface);
        return SUCCEEDED(result) && texture && surface;
    };
    if (!makeLayer(g_hudTexture, g_hudSurface)) {
        Log("HUD texture creation failed; falling back to the in-view HUD panel");
        g_hudTexture = NULL;
        g_hudSurface = NULL;
        g_hudFailed = true;
        return false;
    }
    makeLayer(g_multiplyTexture, g_multiplySurface);
    makeLayer(g_topLayerTexture, g_topLayerSurface);
    if (wantMirror) {
        makeLayer(g_mirrorTexture[0], g_mirrorSurface[0]);
        makeLayer(g_mirrorTexture[1], g_mirrorSurface[1]);
    }
    g_hudWidth = width;
    g_hudHeight = height;
    g_mirrorCreated = wantMirror;
    static const D3DFORMAT depthFormats[] = {D3DFMT_D24S8, D3DFMT_D24X4S4, D3DFMT_D15S1};
    for (D3DFORMAT format : depthFormats) {
        if (SUCCEEDED(device->CreateDepthStencilSurface(width, height, format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_hudDepth,
                                                        NULL)) &&
            g_hudDepth)
            break;
        g_hudDepth = NULL;
    }
    if (!g_hudDepth)
        Log("UI stencil buffer creation failed; the game's UI clipping (car lists, panels) stays off");
    if (!g_hudStateBlock)
        device->CreateStateBlock(D3DSBT_ALL, &g_hudStateBlock);
    Log("HUD layers %ux%u created (mirror layer %s, stencil %s)", g_hudWidth, g_hudHeight, wantMirror ? "on" : "off",
        g_hudDepth ? "on" : "off");
    return g_hudSurface != NULL;
}

// Run the draw into the scratch target with `query` around it, then put the game's target, depth surface and viewport back.
static bool RunCoverageDraw(IDirect3DDevice9* device, const D3DVIEWPORT9& base, IDirect3DQuery9* query, const DrawCall& draw)
{
    if (!g_occlusionTexture) {
        PFN_CreateTexture createTexture = (PFN_CreateTexture)g_deviceOriginals[VT_CreateTexture];
        if (FAILED(createTexture(device, g_backBufferWidth, g_backBufferHeight, 1, D3DUSAGE_RENDERTARGET,
                                 D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_occlusionTexture, NULL)) ||
            !g_occlusionTexture)
            return false;
        g_occlusionTexture->GetSurfaceLevel(0, &g_occlusionSurface);
    }
    if (!g_occlusionSurface || !query)
        return false;
    PFN_SetRenderTarget setRenderTarget = (PFN_SetRenderTarget)g_deviceOriginals[VT_SetRenderTarget];
    IDirect3DSurface9* savedTarget = NULL;
    device->GetRenderTarget(0, &savedTarget);
    if (!savedTarget)
        return false;
    IDirect3DSurface9* savedDepth = NULL;
    device->GetDepthStencilSurface(&savedDepth);
    PFN_SetDepthStencilSurface setDepthStencil =
        (PFN_SetDepthStencilSurface)g_deviceOriginals[VT_SetDepthStencilSurface];
    if (savedDepth)
        setDepthStencil(device, NULL);
    setRenderTarget(device, 0, g_occlusionSurface);
    ((PFN_SetViewport)g_deviceOriginals[VT_SetViewport])(device, &base);
    ((PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState])(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    ((PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect])(device, &g_gameScissorRect);
    query->Issue(D3DISSUE_BEGIN);
    draw();
    query->Issue(D3DISSUE_END);
    setRenderTarget(device, 0, savedTarget);
    savedTarget->Release();
    if (savedDepth) {
        setDepthStencil(device, savedDepth);
        savedDepth->Release();
    }
    ((PFN_SetViewport)g_deviceOriginals[VT_SetViewport])(device, &base);
    return true;
}

// The number of pixels a draw can cover: the viewport, or the game's scissor rectangle when that is smaller.
static double CoverageArea(const D3DVIEWPORT9& base)
{
    double area = (double)base.Width * base.Height;
    if (g_gameScissorEnabled) {
        double scissorArea = (double)(g_gameScissorRect.right - g_gameScissorRect.left) *
                             (g_gameScissorRect.bottom - g_gameScissorRect.top);
        if (scissorArea > 0 && scissorArea < area)
            area = scissorArea;
    }
    return area;
}

// Used to drop the full-screen dim/blur quads the game draws on top of its own UI (they rely on the live screen behind
// them and would blank our separate HUD layer).  This version waits for the GPU's answer (a round trip that drains the GPU's queue:
// about 2.5 ms of CPU time when a frame is in flight).
bool HudDrawCoversScreen(IDirect3DDevice9* device, const D3DVIEWPORT9& base, double minFrac, double* outFrac,
                                const DrawCall& draw)
{
    if (!g_occlusionQuery && FAILED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_occlusionQuery)))
        return false;
    if (!RunCoverageDraw(device, base, g_occlusionQuery, draw))
        return false;
    DWORD passedPixels = 0;
    bool gotResult = true;
    ULONGLONG startTime = GetTickCount64();
    while (g_occlusionQuery->GetData(&passedPixels, sizeof(passedPixels), D3DGETDATA_FLUSH) == S_FALSE) {
        if (GetTickCount64() - startTime > 40) {
            gotResult = false;
            break;
        }
    }
    if (!gotResult) {
        static int n = 0;
        if (n++ < 10)
            Log("HUD occlusion query timed out");
    }
    double area = CoverageArea(base);
    if (outFrac)
        *outFrac = gotResult && area > 0 ? passedPixels / area : 0.0;
    return gotResult && passedPixels >= area * minFrac;
}

static CoverageSlot g_coverageSlots[64];
CoverageSlot& HudCoverageSlot(unsigned ordinal)
{
    return g_coverageSlots[ordinal < 64 ? ordinal : 63];
}
bool HudCoverageIssue(IDirect3DDevice9* device, const D3DVIEWPORT9& base, CoverageSlot& slot, const DrawCall& draw)
{
    if (slot.pending)
        return false;
    if (!slot.query && FAILED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &slot.query)))
        return false;
    if (!RunCoverageDraw(device, base, slot.query, draw))
        return false;
    slot.pending = true;
    slot.area = CoverageArea(base);
    slot.issuedFrame = g_vr.frameCount;
    return true;
}
bool HudCoveragePoll(CoverageSlot& slot, double* frac)
{
    if (!slot.pending || !slot.query)
        return false;
    DWORD passedPixels = 0;
    if (slot.query->GetData(&passedPixels, sizeof(passedPixels), 0) != S_OK) {
        if (g_vr.frameCount - slot.issuedFrame > 60) // never came back (a lost device, a reset): free the slot
            slot.pending = false;
        return false;
    }
    slot.pending = false;
    if (frac)
        *frac = slot.area > 0 ? passedPixels / slot.area : 0.0;
    return true;
}
static void ReleaseCoverageSlots()
{
    for (CoverageSlot& slot : g_coverageSlots) {
        if (slot.query)
            slot.query->Release();
        slot.query = NULL;
        slot.pending = false;
    }
}

// The depth-stencil surface a layer draw runs with.  With a stencil buffer of ours the game's own surface is swapped for it
// (only when the game had one bound: without it the game's stencil states did nothing either), and depth testing is forced
// off, as it was when the layers had no depth buffer at all.
struct LayerDepth {
    IDirect3DSurface9* gameDepth = NULL; // the game's depth-stencil surface (reference held), NULL if it had none
    bool swapped = false;                // gameDepth has to be put back
    bool ours = false;                   // g_hudDepth is bound
    DWORD zEnable = TRUE, zWrite = TRUE;
};

static void BeginLayerDepth(IDirect3DDevice9* device, LayerDepth& depth, bool dropGameDepth)
{
    PFN_SetDepthStencilSurface setDepthStencil =
        (PFN_SetDepthStencilSurface)g_deviceOriginals[VT_SetDepthStencilSurface];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    device->GetDepthStencilSurface(&depth.gameDepth);
    if (!depth.gameDepth)
        return;
    if (g_hudDepth) {
        g_uiGameDepth = depth.gameDepth;
        device->GetRenderState(D3DRS_ZENABLE, &depth.zEnable);
        device->GetRenderState(D3DRS_ZWRITEENABLE, &depth.zWrite);
        setDepthStencil(device, g_hudDepth);
        setRenderState(device, D3DRS_ZENABLE, FALSE);
        setRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
        depth.ours = depth.swapped = true;
    } else if (dropGameDepth) {
        setDepthStencil(device, NULL);
        depth.swapped = true;
    }
}

static void EndLayerDepth(IDirect3DDevice9* device, LayerDepth& depth)
{
    if (!depth.gameDepth)
        return;
    if (depth.swapped)
        ((PFN_SetDepthStencilSurface)g_deviceOriginals[VT_SetDepthStencilSurface])(device, depth.gameDepth);
    if (depth.ours) {
        PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
        setRenderState(device, D3DRS_ZENABLE, depth.zEnable);
        setRenderState(device, D3DRS_ZWRITEENABLE, depth.zWrite);
    }
    depth.gameDepth->Release();
    depth.gameDepth = NULL;
}

// The game clears the stencil of its UI once per frame before it starts drawing it; the shared one of the layers is cleared at
// the first layer draw of each frame (the layer render target and g_hudDepth are bound).
static void ClearHudStencilOncePerFrame(IDirect3DDevice9* device)
{
    if (g_hudDepthClearedFrame == g_vr.frameCount)
        return;
    g_hudDepthClearedFrame = g_vr.frameCount;
    D3DVIEWPORT9 fullViewport;
    fullViewport.X = 0;
    fullViewport.Y = 0;
    fullViewport.Width = g_hudWidth;
    fullViewport.Height = g_hudHeight;
    fullViewport.MinZ = 0;
    fullViewport.MaxZ = 1;
    ((PFN_SetViewport)g_deviceOriginals[VT_SetViewport])(device, &fullViewport);
    ((PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState])(device, D3DRS_SCISSORTESTENABLE, FALSE);
    ((PFN_Clear)g_deviceOriginals[VT_Clear])(device, 0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, 0);
}

// The game cleared the stencil buffer its UI draws with, after the layers started on this frame's UI: do the same on ours
// (a Clear on the game's own render target never reaches the layers' stencil buffer).
void OnGameStencilClear(IDirect3DDevice9* device, DWORD value)
{
    if (!g_hudDepth || !g_uiGameDepth || !g_hudSurface || g_hudDepthClearedFrame != g_vr.frameCount)
        return;
    IDirect3DSurface9* gameDepth = NULL;
    device->GetDepthStencilSurface(&gameDepth);
    if (gameDepth != g_uiGameDepth) {
        if (gameDepth)
            gameDepth->Release();
        return;
    }
    IDirect3DSurface9* gameTarget = NULL;
    device->GetRenderTarget(0, &gameTarget);
    if (!gameTarget) {
        gameDepth->Release();
        return;
    }
    PFN_SetRenderTarget setRenderTarget = (PFN_SetRenderTarget)g_deviceOriginals[VT_SetRenderTarget];
    PFN_SetDepthStencilSurface setDepthStencil =
        (PFN_SetDepthStencilSurface)g_deviceOriginals[VT_SetDepthStencilSurface];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    setRenderTarget(device, 0, g_hudSurface);
    setDepthStencil(device, g_hudDepth);
    D3DVIEWPORT9 fullViewport;
    fullViewport.X = 0;
    fullViewport.Y = 0;
    fullViewport.Width = g_hudWidth;
    fullViewport.Height = g_hudHeight;
    fullViewport.MinZ = 0;
    fullViewport.MaxZ = 1;
    ((PFN_SetViewport)g_deviceOriginals[VT_SetViewport])(device, &fullViewport);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
    ((PFN_Clear)g_deviceOriginals[VT_Clear])(device, 0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, value);
    setRenderTarget(device, 0, gameTarget);
    setDepthStencil(device, gameDepth);
    ((PFN_SetViewport)g_deviceOriginals[VT_SetViewport])(device, &g_gameViewport);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    ((PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect])(device, &g_gameScissorRect);
    gameTarget->Release();
    gameDepth->Release();
}

// Draw one 2D HUD call into the HUD texture, with alpha accumulated so the texture can be composited later.
HRESULT DrawHudToTexture(IDirect3DDevice9* device, const D3DVIEWPORT9& base, const DrawCall& draw)
{
    PFN_SetRenderTarget setRenderTarget = (PFN_SetRenderTarget)g_deviceOriginals[VT_SetRenderTarget];
    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    IDirect3DSurface9* savedTarget = NULL;
    device->GetRenderTarget(0, &savedTarget);
    if (!savedTarget)
        return draw();
    DWORD colorWriteMask = 0xF;
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWriteMask);
    const bool stencilOnly = colorWriteMask == 0;
    LayerDepth depth;
    BeginLayerDepth(device, depth, true);
    if (stencilOnly && !depth.ours) {
        // a stencil-only draw (a mask for the UI's clipping): it paints nothing, and with no stencil buffer to write it to
        // here, drawing it would only leave a solid box
        EndLayerDepth(device, depth);
        savedTarget->Release();
        return D3D_OK;
    }
    setRenderTarget(device, 0, g_hudSurface);
    if (depth.ours)
        ClearHudStencilOncePerFrame(device);
    if (!stencilOnly && !g_hudUsed) {
        D3DVIEWPORT9 fullViewport;
        fullViewport.X = 0;
        fullViewport.Y = 0;
        fullViewport.Width = g_hudWidth;
        fullViewport.Height = g_hudHeight;
        fullViewport.MinZ = 0;
        fullViewport.MaxZ = 1;
        setViewport(device, &fullViewport);
        setRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
        device->Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        g_hudUsed = true;
    }
    setViewport(device, &base);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);

    DWORD alphaBlend = 0, destBlend = 0, separateAlpha = 0, srcBlendAlpha = 0, destBlendAlpha = 0, blendOpAlpha = 0,
          srcBlend = 0;
    device->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &destBlend);
    device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separateAlpha);
    device->GetRenderState(D3DRS_SRCBLENDALPHA, &srcBlendAlpha);
    device->GetRenderState(D3DRS_DESTBLENDALPHA, &destBlendAlpha);
    device->GetRenderState(D3DRS_BLENDOPALPHA, &blendOpAlpha);
    // keep the alpha channel a proper "coverage" value: over-blend for normal draws, leave it alone for additive ones
    setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    setRenderState(device, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    if (alphaBlend && destBlend == D3DBLEND_ONE) {
        setRenderState(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
        setRenderState(device, D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    } else if (alphaBlend) {
        setRenderState(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        setRenderState(device, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    } else {
        setRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
        setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
        setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
        setRenderState(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        setRenderState(device, D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
    }

    HRESULT hr;
    struct Fail {
        static void Chk(HRESULT h, DWORD abv)
        {
            static int n = 0;
            if (FAILED(h) && n++ < 20)
                Log("HUD-texture draw failed hr=0x%08x ab=%u", (unsigned)h, (unsigned)abv);
        }
    };
    setRenderState(device, D3DRS_COLORWRITEENABLE, stencilOnly ? 0 : 0xF);
    hr = draw();
    Fail::Chk(hr, alphaBlend);

    setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, separateAlpha);
    setRenderState(device, D3DRS_SRCBLENDALPHA, srcBlendAlpha);
    setRenderState(device, D3DRS_DESTBLENDALPHA, destBlendAlpha);
    setRenderState(device, D3DRS_BLENDOPALPHA, blendOpAlpha);
    if (!alphaBlend) {
        setRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
        setRenderState(device, D3DRS_DESTBLEND, destBlend);
        setRenderState(device, D3DRS_SRCBLEND, srcBlend ? srcBlend : D3DBLEND_ONE);
    }
    setRenderTarget(device, 0, savedTarget);
    savedTarget->Release();
    EndLayerDepth(device, depth);
    setRenderState(device, D3DRS_COLORWRITEENABLE, colorWriteMask);
    setViewport(device, &base);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
    return hr;
}

// A UI draw that is being dropped may still be one of the game's stencil masks; later draws are clipped by it, so its stencil
// effect (and only that) is kept.
HRESULT DrawHudStencilOnly(IDirect3DDevice9* device, const D3DVIEWPORT9& base, const DrawCall& draw)
{
    if (!g_hudDepth)
        return D3D_OK;
    DWORD enabled = 0, passOp = D3DSTENCILOP_KEEP, failOp = D3DSTENCILOP_KEEP, zFailOp = D3DSTENCILOP_KEEP;
    device->GetRenderState(D3DRS_STENCILENABLE, &enabled);
    if (!enabled)
        return D3D_OK;
    device->GetRenderState(D3DRS_STENCILPASS, &passOp);
    device->GetRenderState(D3DRS_STENCILFAIL, &failOp);
    device->GetRenderState(D3DRS_STENCILZFAIL, &zFailOp);
    if (passOp == D3DSTENCILOP_KEEP && failOp == D3DSTENCILOP_KEEP && zFailOp == D3DSTENCILOP_KEEP)
        return D3D_OK;
    DWORD colorWriteMask = 0xF;
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWriteMask);
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    setRenderState(device, D3DRS_COLORWRITEENABLE, 0);
    HRESULT hr = DrawHudToTexture(device, base, draw); // sees colour writes off: stencil only
    setRenderState(device, D3DRS_COLORWRITEENABLE, colorWriteMask);
    return hr;
}

// Multiplicative UI blends (the dark glass behind menu panels) go to a second layer that starts white and is multiplied
// over the scene.
HRESULT DrawHudToMultiplyLayer(IDirect3DDevice9* device, const D3DVIEWPORT9& base, const DrawCall& draw)
{
    PFN_SetRenderTarget setRenderTarget = (PFN_SetRenderTarget)g_deviceOriginals[VT_SetRenderTarget];
    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    IDirect3DSurface9* savedTarget = NULL;
    device->GetRenderTarget(0, &savedTarget);
    if (!savedTarget)
        return draw();
    LayerDepth depth;
    BeginLayerDepth(device, depth, false); // (without a stencil buffer of ours the game's depth-stencil surface stays bound)
    setRenderTarget(device, 0, g_multiplySurface);
    if (depth.ours)
        ClearHudStencilOncePerFrame(device);
    if (!g_multiplyUsed) {
        D3DVIEWPORT9 fullViewport;
        fullViewport.X = 0;
        fullViewport.Y = 0;
        fullViewport.Width = g_hudWidth;
        fullViewport.Height = g_hudHeight;
        fullViewport.MinZ = 0;
        fullViewport.MaxZ = 1;
        setViewport(device, &fullViewport);
        setRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
        device->Clear(0, NULL, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);
        g_multiplyUsed = true;
    }
    setViewport(device, &base);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
    HRESULT hr = draw();
    setRenderTarget(device, 0, savedTarget);
    savedTarget->Release();
    EndLayerDepth(device, depth);
    setViewport(device, &base);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
    return hr;
}



static bool g_fullscreenVideoShown = false;
void NoteSceneBloom(IDirect3DBaseTexture9* bloom)
{
    if (bloom == g_sceneBloom)
        return;
    if (bloom)
        bloom->AddRef();
    if (g_sceneBloom)
        g_sceneBloom->Release();
    g_sceneBloom = bloom;
}

void ComposeHudLayers(IDirect3DDevice9* device)
{
    bool hudContent = g_hudUsed || g_multiplyUsed;
    bool topContent = g_topLayerUsed;
    g_topLayerUsed = false;
    bool mirrorContent = (g_mirrorUsed[0] || g_mirrorUsed[1]) && g_mirrorTexture[0] && g_mirrorTexture[1];
    g_mirrorUsed[0] = g_mirrorUsed[1] = false;
    bool topVeil = g_topVeilRequested;
    int topVeilA = g_topVeilAlpha;
    g_topVeilRequested = false;
    g_menuDialogToTopLayer = false;
    g_lateUiToTopLayer = false;
    g_sceneCompositeDrawn = false;
    g_postBackdropToTopLayer = false;
    int lateN = g_lateBackdropDrops;
    g_lateBackdropDrops = 0;
    bool videoFrame = g_loadingVideoDropped;
    g_loadingVideoDropped = false;
    bool wantDim = g_dimRequested && !videoFrame, dimWin = g_dimStartWindow, dimStrong = g_dimStrongRequested;
    g_menuDimLast = wantDim;
    g_dimRequested = false;
    g_dimStartWindow = false;
    g_dimStrongRequested = false;
    if (IsMovieFrame()) {
        // a movie (tutorial video, cutscene) fills the screen; the game keeps drawing its menu UI under it, which must
        // not be floated over the picture
        g_hudUsed = false;
        g_multiplyUsed = false;
        g_dimLevel = 0.f;
        return;
    }
    if (!hudContent && !wantDim && !topContent && !topVeil && !mirrorContent && g_dimLevel < 0.01f)
        return;
    g_hudUsed = false;
    if (!g_hudTexture || !g_hudStateBlock || !g_vr.poseValid || !IsRaceMode() || !g_cfg.stereo)
        return;
    g_dimTargetLevel =
        wantDim ? (dimWin ? 0.5f
                          : (dimStrong ? g_cfg.dimStrongPct * 0.01f * ((lateN >= 1 && lateN < 3) ? 0.6f : 1.f) : 0.3f))
                : 0.f;
    g_dimLevel += (g_dimTargetLevel - g_dimLevel) * (g_dimTargetLevel > g_dimLevel ? 0.5f : 0.35f);
    if (g_dimLevel < 0.01f)
        g_dimLevel = 0.f;
    if (!hudContent && !topContent && !topVeil && !mirrorContent && g_dimLevel <= 0.f)
        return;
    if (g_raceStartPhase && GetTickCount64() - g_raceStartTime > 60000)
        g_raceStartPhase = false;
    if (!g_raceStartPhase)
        g_fullscreenVideoShown = false;
    if (g_cfg.hideFullscreenOverlay && g_hudSurface && g_raceStartPhase && hudContent) {
        // a loading/intro video is drawn as an opaque full-screen picture into the HUD layer: measure coverage
        // (cheaply, every 3rd frame) and don't float it over the race
        static int tick = 0;
        static int hiCnt = 0, loCnt = 0;
        IDirect3DSurface9*& tinyTarget = g_coverageTarget;
        IDirect3DSurface9*& tinyReadback = g_coverageReadback;
        if (!tinyTarget) {
            device->CreateRenderTarget(96, 54, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &tinyTarget, NULL);
            device->CreateOffscreenPlainSurface(96, 54, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &tinyReadback, NULL);
        }
        if (tinyTarget && tinyReadback && (tick++ % 3) == 0 &&
            SUCCEEDED(device->StretchRect(g_hudSurface, NULL, tinyTarget, NULL, D3DTEXF_LINEAR)) &&
            SUCCEEDED(device->GetRenderTargetData(tinyTarget, tinyReadback))) {
            D3DLOCKED_RECT lockedRect;
            int n = 0;
            if (SUCCEEDED(tinyReadback->LockRect(&lockedRect, NULL, D3DLOCK_READONLY))) {
                for (int y = 0; y < 54; y++) {
                    const unsigned* r = (const unsigned*)((const BYTE*)lockedRect.pBits + (size_t)y * lockedRect.Pitch);
                    for (int x = 0; x < 96; x++)
                        if ((r[x] >> 24) > 0xF0)
                            n++;
                }
                tinyReadback->UnlockRect();
                double coverage = n / (96.0 * 54.0);
                if (coverage > 0.97) {
                    hiCnt++;
                    loCnt = 0;
                } else if (coverage < 0.9) {
                    loCnt++;
                    hiCnt = 0;
                }
                if (!g_fullscreenVideoShown && hiCnt >= 2) {
                    g_fullscreenVideoShown = true;
                    Log("full-screen overlay detected (coverage %.2f): HUD hidden", coverage);
                }
                if (loCnt >= 3) {
                    if (g_fullscreenVideoShown)
                        Log("full-screen overlay gone (coverage %.2f): HUD shown", coverage);
                    g_fullscreenVideoShown = false;
                    g_raceStartPhase = false;
                }
            }
        }
        if (g_fullscreenVideoShown) {
            g_multiplyUsed = false;
            return;
        }
    }
    IDirect3DSurface9* backBuffer = NULL;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        return;
    IDirect3DSurface9* savedTarget = NULL;
    device->GetRenderTarget(0, &savedTarget);
    PFN_SetRenderTarget setRenderTarget = (PFN_SetRenderTarget)g_deviceOriginals[VT_SetRenderTarget];
    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    PFN_SetTexture setTexture = (PFN_SetTexture)g_deviceOriginals[VT_SetTexture];
    PFN_SetVertexShader setVertexShader = (PFN_SetVertexShader)g_deviceOriginals[VT_SetVertexShader];
    PFN_DrawPrimitiveUP drawPrimitiveUP = (PFN_DrawPrimitiveUP)g_deviceOriginals[VT_DrawPrimitiveUP];

    g_hudStateBlock->Capture();
    setRenderTarget(device, 0, backBuffer);
    D3DVIEWPORT9 fullViewport;
    fullViewport.X = 0;
    fullViewport.Y = 0;
    fullViewport.Width = g_backBufferWidth;
    fullViewport.Height = g_backBufferHeight;
    fullViewport.MinZ = 0;
    fullViewport.MaxZ = 1;
    setViewport(device, &fullViewport);
    setVertexShader(device, NULL);
    device->SetPixelShader(NULL);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (DWORD st = 1; st < 4; st++) {
        setTexture(device, st, NULL);
        device->SetTextureStageState(st, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(st, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    setTexture(device, 0, g_hudTexture);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    {
        float noBias = 0.f;
        device->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, *(DWORD*)&noBias);
    }
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    setRenderState(device, D3DRS_ZENABLE, FALSE);
    setRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
    setRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    setRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
    setRenderState(device, D3DRS_STENCILENABLE, FALSE);
    setRenderState(device, D3DRS_FOGENABLE, FALSE);
    setRenderState(device, D3DRS_LIGHTING, FALSE);
    setRenderState(device, D3DRS_SRGBWRITEENABLE, FALSE);
    setRenderState(device, D3DRS_COLORWRITEENABLE, 0xF);
    setRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
    setRenderState(device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
    setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE);

    double bw = g_backBufferWidth, bh = g_backBufferHeight, eyeW = bw / 2.0;
    double tw0 = fmax(1e-3, g_vr.eyeBox[0].right - g_vr.eyeBox[0].left),
           tw1 = fmax(1e-3, g_vr.eyeBox[1].right - g_vr.eyeBox[1].left);
    float texCoords[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    // draw the bound layer (or, with eyeTexture, one texture per eye) as the world-locked panel at HudDistance
    auto drawLayerQuads = [&](IDirect3DTexture9* const* eyeTexture = NULL, bool screenUv = false) {
        double distance = fmax(0.3, g_cfg.hudDistance);
        double panelWidth = g_cfg.hudScale * 0.5 * (tw0 + tw1) * distance, panelHeight = panelWidth * bh / bw;
        double cyOff = -g_cfg.hudOffsetY * panelHeight;
        double cornerOffsets[4][2] = {{-panelWidth / 2, panelHeight / 2 + cyOff},
                                      {panelWidth / 2, panelHeight / 2 + cyOff},
                                      {-panelWidth / 2, -panelHeight / 2 + cyOff},
                                      {panelWidth / 2, -panelHeight / 2 + cyOff}};
        for (int eye = 0; eye < 2; eye++) {
            const Matrix4& delta = g_vr.eyeCameraDelta[eye];
            const EyeBox& box = g_vr.eyeBox[eye];
            double tanWidth = fmax(1e-3, box.right - box.left), tanHeight = fmax(1e-3, box.up + box.down);
            ScreenVertex corners[4];
            bool allVisible = true;
            for (int i = 0; i < 4; i++) {
                double x = cornerOffsets[i][0], y = cornerOffsets[i][1];
                double vx = x * delta.m[0][0] + y * delta.m[1][0] + distance * delta.m[2][0] + delta.m[3][0];
                double vy = x * delta.m[0][1] + y * delta.m[1][1] + distance * delta.m[2][1] + delta.m[3][1];
                double vz = x * delta.m[0][2] + y * delta.m[1][2] + distance * delta.m[2][2] + delta.m[3][2];
                if (vz < 0.08) {
                    allVisible = false;
                    break;
                }
                corners[i].x = (float)(eye * eyeW + (vx / vz - box.left) / tanWidth * eyeW +
                                       g_cfg.hudXOffset * eyeW * (eye == 0 ? 1.0 : -1.0) - 0.5);
                corners[i].y = (float)((box.up - vy / vz) / tanHeight * bh - 0.5);
                corners[i].z = 0.0f;
                corners[i].rhw = (float)(1.0 / vz);
                corners[i].u = texCoords[i][0];
                corners[i].v = texCoords[i][1];
            }
            if (!allVisible)
                continue;
            RECT r;
            r.left = (LONG)(eye * eyeW);
            r.right = (LONG)((eye + 1) * eyeW);
            r.top = 0;
            r.bottom = (LONG)bh;
            if (eye == 0)
                r.right -= g_cfg.seamGutter;
            else
                r.left += g_cfg.seamGutter;
            setScissorRect(device, &r);
            if (eyeTexture)
                setTexture(device, 0, eyeTexture[eye]);
            if (screenUv) { // second texture set: the same corners, as a position on the whole picture (to sample the game's bloom there)
                struct ScreenVertex2 { float x, y, z, rhw, u, v, u2, v2; } v2[4];
                for (int i = 0; i < 4; i++) {
                    v2[i].x = corners[i].x;
                    v2[i].y = corners[i].y;
                    v2[i].z = corners[i].z;
                    v2[i].rhw = corners[i].rhw;
                    v2[i].u = corners[i].u;
                    v2[i].v = corners[i].v;
                    v2[i].u2 = (float)((corners[i].x + 0.5) / bw);
                    v2[i].v2 = (float)((corners[i].y + 0.5) / bh);
                }
                device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX2);
                drawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v2, sizeof(ScreenVertex2));
                device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            } else
                drawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, corners, sizeof(ScreenVertex));
        }
    };
    auto drawHudQuads = [&]() { drawLayerQuads(); };
    // The game's own glow of its UI (the glow buffer of its last pass), added over the layers.
    auto addGameGlow = [&](int pct) {
        int k = pct > 200 ? 200 : pct;
        setTexture(device, 0, g_sceneBloom);
        setRenderState(device, D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, k * 255 / 200, k * 255 / 200, k * 255 / 200));
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
        setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ONE);
        drawHudQuads();
    };
    if (g_dimLevel > 0.f) {
        // darken the scene behind the UI (stands in for the game's own dim/blur backdrop, which would otherwise cover
        // the UI)
        setTexture(device, 0, NULL);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        setRenderState(device, D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB((int)(g_dimLevel * 255.f), 12, 6, 28));
        setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        for (int eye = 0; eye < 2; eye++) {
            float x0 = (float)(eye * eyeW) - 0.5f, x1 = (float)((eye + 1) * eyeW) - 0.5f;
            if (eye == 0)
                x1 -= (float)g_cfg.seamGutter;
            else
                x0 += (float)g_cfg.seamGutter;
            ScreenVertex quad[4] = {{x0, -0.5f, 0.f, 1.f, 0, 0},
                                    {x1, -0.5f, 0.f, 1.f, 1, 0},
                                    {x0, (float)bh - 0.5f, 0.f, 1.f, 0, 1},
                                    {x1, (float)bh - 0.5f, 0.f, 1.f, 1, 1}};
            RECT r;
            r.left = (LONG)(eye * eyeW);
            r.right = (LONG)((eye + 1) * eyeW);
            r.top = 0;
            r.bottom = (LONG)bh;
            setScissorRect(device, &r);
            drawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
        }
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
        setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        setTexture(device, 0, g_hudTexture);
    }
    if (g_multiplyUsed && g_multiplyTexture) {
        setTexture(device, 0, g_multiplyTexture);
        setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ZERO);
        setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        drawHudQuads();
        setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
        setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        setTexture(device, 0, g_hudTexture);
    }
    g_multiplyUsed = false;
    if (mirrorContent) { // the rear-view mirror picture, one layer per eye, at the HUD's distance
        drawLayerQuads(g_mirrorTexture);
        setTexture(device, 0, g_hudTexture);
    }
    if (hudContent && g_cfg.vr) { // (the AI cars' markers go over their cars; what is cut out of the layer is cleared, and from the game's glow of its UI too)
        IDirect3DSurface9* glowSurface = NULL;
        UINT glowWidth = 0, glowHeight = 0;
        D3DSURFACE_DESC glowDesc;
        if (g_sceneBloom && g_cfg.bloomPct > 0 && g_sceneBloom->GetType() == D3DRTYPE_TEXTURE &&
            SUCCEEDED(((IDirect3DTexture9*)g_sceneBloom)->GetLevelDesc(0, &glowDesc)) && SUCCEEDED(((IDirect3DTexture9*)g_sceneBloom)->GetSurfaceLevel(0, &glowSurface))) {
            glowWidth = glowDesc.Width;
            glowHeight = glowDesc.Height;
        }
        AiHudComposeMarkers(device, g_hudTexture, g_hudSurface, glowSurface, glowWidth, glowHeight);
        if (glowSurface)
            glowSurface->Release();
    }
    if (hudContent)
        drawHudQuads();
    if ((topContent || topVeil) && g_topLayerTexture) {
        // connecting spinner / dim that the game draws straight to the screen: above the UI
        if (topVeil) {
            setTexture(device, 0, NULL);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
            setRenderState(device, D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(topVeilA, 10, 6, 24));
            setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            for (int eye = 0; eye < 2; eye++) {
                float x0 = (float)(eye * eyeW) - 0.5f, x1 = (float)((eye + 1) * eyeW) - 0.5f;
                if (eye == 0)
                    x1 -= (float)g_cfg.seamGutter;
                else
                    x0 += (float)g_cfg.seamGutter;
                ScreenVertex quad[4] = {{x0, -0.5f, 0.f, 1.f, 0, 0},
                                        {x1, -0.5f, 0.f, 1.f, 1, 0},
                                        {x0, (float)bh - 0.5f, 0.f, 1.f, 0, 1},
                                        {x1, (float)bh - 0.5f, 0.f, 1.f, 1, 1}};
                RECT r;
                r.left = (LONG)(eye * eyeW);
                r.right = (LONG)((eye + 1) * eyeW);
                r.top = 0;
                r.bottom = (LONG)bh;
                setScissorRect(device, &r);
                drawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
            }
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
            setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        }
        setTexture(device, 0, g_topLayerTexture);
        if (topContent)
            drawHudQuads();
    }
    if (g_cfg.bloomPct > 0 && g_sceneBloom && (hudContent || topContent))
        addGameGlow(g_cfg.bloomPct);
    g_hudStateBlock->Apply();
    setRenderTarget(device, 0, savedTarget ? savedTarget : backBuffer);
    setViewport(device, &g_gameViewport);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
    if (savedTarget)
        savedTarget->Release();
    backBuffer->Release();
}

bool IsMultiplicativeBlend(IDirect3DDevice9* device)
{
    DWORD alphaBlend = 0, srcBlend = 0, destBlend = 0, blendOp = 0;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
    if (!alphaBlend)
        return false;
    device->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &destBlend);
    device->GetRenderState(D3DRS_BLENDOP, &blendOp);
    if (blendOp != D3DBLENDOP_ADD)
        return false;
    return (srcBlend == D3DBLEND_ZERO && (destBlend == D3DBLEND_SRCCOLOR || destBlend == D3DBLEND_SRCALPHA)) ||
           (srcBlend == D3DBLEND_DESTCOLOR && destBlend == D3DBLEND_ZERO);
}
