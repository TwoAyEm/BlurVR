// The IDirect3DDevice9 hooks: state tracking, shader tracking, draw interception, Present/Reset.
#include "device_hooks.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "device_state.h"
#include "display_modes.h"
#include "draw_dispatch.h"
#include "game_camera.h"
#include "game_mode.h"
#include "hud.h"
#include "mirror_stereo.h"
#include "perf_probe.h"
#include "photo_stereo.h"
#include "texture_pitch.h"
#include "window_spoof.h"
#include "render_state.h"
#include "shader_tracking.h"
#include "streaks.h"
#include "vr_runtime.h"

static HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    if (g_vrStartDeferred) {
        if (g_preVrSceneDraws >= 20)
            g_preVrSceneFrames++;
        else
            g_preVrSceneFrames = 0;
        g_preVrSceneDraws = 0;
        bool scene = g_preVrSceneFrames >= 15, timeout = GetTickCount() - g_vrDeferredSince > 90000;
        if (scene || timeout) {
            Log("starting VR now (%s)", scene ? "3D menu scene is being drawn" : "90 s fallback");
            if (scene)
                g_levelLoading = false;
            StartVrInitThread();
        }
    }
    bool raceFrame = g_vr.active && IsRaceMode() && g_sceneDrawsThisFrame > 0;
    DeviceRestoreAll(d); // the game's constants and scissor state are back before anything else draws
    if (g_vr.active)
        VrSubmitFrame(d);
    PerfFrameClose(d, raceFrame);
    // The picture the player sees goes to the headset; the window on the desktop is only a mirror.  Its Present is a copy of the
    // whole back buffer into the desktop compositor's surface: DesktopPresentEvery presents it only every n-th race frame.
    bool skipPresent = false;
    if (g_cfg.enabled && g_cfg.desktopPresentEvery > 1 && raceFrame) {
        static unsigned presentCounter = 0;
        skipPresent = (presentCounter++ % (unsigned)g_cfg.desktopPresentEvery) != 0;
    }
    RECT mirrorRect = {0, 0, 0, 0};
    bool mirrorRectUsed = false;
    if (!skipPresent && raceFrame && g_cfg.desktopView && g_vr.active) // the headset's copy is long made: the window gets one eye's view
        mirrorRectUsed = VrShowEyeOnDesktop(d, &mirrorRect);
    HRESULT hr;
    {
        PerfScope perfPresent(PS_PRESENT);
        if (skipPresent)
            hr = D3D_OK;
        else {
            auto present = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*),
                                VT_Present);
            hr = present(d, mirrorRectUsed ? &mirrorRect : a, b, w, r);
            if (mirrorRectUsed && FAILED(hr)) { // the driver does not take a source rectangle: the whole back buffer from now on
                g_desktopRectFailed = true;
                Log("Present with a source rectangle failed 0x%08x: the desktop window shows the whole back buffer", (unsigned)hr);
                hr = present(d, a, b, w, r);
            }
        }
        // an Ex device reports a covered or resized window with success codes a plain device never returns
        if (g_deviceIsEx && (hr == 0x08760878 /*S_PRESENT_OCCLUDED*/ || hr == 0x08760877 /*S_PRESENT_MODE_CHANGED*/))
            hr = D3D_OK;
    }
    if (g_vr.active) {
        PerfScope perfPose(PS_POSE);
        VrUpdatePose();
    }
    PerfPoll();
    GameCameraPoll();
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp)
{
    DeviceRestoreAll(d);
    ReleaseStagingResources();
    ReleaseSharedTransport();
    ReleaseTexturePitchState();
    ReleasePerf();
    ReleaseHud();
    ReleaseStreakLayer();
    ReleaseMirrorStereo();
    ReleasePhotoStereo();
    g_renderTargetTextures.clear();
    AdjustPresentParams(pp, true);
    if (pp)
        WindowSpoofWindowed(pp->Windowed != 0);
    if (pp && pp->Windowed)
        WindowSpoofIntendedSize(pp->BackBufferWidth, pp->BackBufferHeight); // what the game's window should look like to it from now on
    HRESULT hr = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*), VT_Reset)(d, pp);
    if (SUCCEEDED(hr)) {
        DeviceStateReset();
        g_rtWidth = g_rtHeight = 0;
        RefreshRenderTargetState(d);
        Log("Reset -> backbuffer %ux%u", g_backBufferWidth, g_backBufferHeight);
    }
    return hr;
}

// An Ex device has no managed pool.  The game's managed resources become default-pool ones that can be locked (dynamic usage),
// which is what the managed pool gave it: textures and buffers it fills by locking them once.  Counted and logged (the first of
// each kind, and every failure up to a limit) so a game that does something else with them shows up in the log.
static unsigned g_managedRemapped[5];
static unsigned g_remapFailures = 0;
static const char* const kManagedKinds[5] = {"texture", "volume texture", "cube texture", "vertex buffer", "index buffer"};
static void RemapManaged(int kind, DWORD& usage, D3DPOOL& pool)
{
    if (!g_deviceIsEx || pool != D3DPOOL_MANAGED)
        return;
    pool = D3DPOOL_DEFAULT;
    usage |= D3DUSAGE_DYNAMIC;
    if (g_managedRemapped[kind]++ == 0)
        Log("SharedTransport: first managed %s turned into a default-pool dynamic one", kManagedKinds[kind]);
}
static void NoteRemapResult(int kind, HRESULT hr, bool remapped, DWORD usage, UINT a, UINT b, int format)
{
    if (!remapped || SUCCEEDED(hr) || g_remapFailures++ >= 20)
        return;
    Log("SharedTransport: creating a remapped %s failed 0x%08x (usage 0x%x, %u x %u, format %d)", kManagedKinds[kind],
        (unsigned)hr, (unsigned)usage, a, b, format);
}

static HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f,
                                                 D3DPOOL pool, IDirect3DTexture9** pp, HANDLE* sh)
{
    bool remapped = g_deviceIsEx && pool == D3DPOOL_MANAGED;
    RemapManaged(0, usage, pool);
    HRESULT hr = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                  IDirect3DTexture9**, HANDLE*),
                      VT_CreateTexture)(d, w, h, lv, usage, f, pool, pp, sh);
    NoteRemapResult(0, hr, remapped, usage, w, h, (int)f);
    if (g_deviceIsEx && SUCCEEDED(hr) && pp && *pp)
        NoteTextureCreated(*pp, w, h, f, remapped);
    if (SUCCEEDED(hr) && pp && *pp) {
        if (usage & D3DUSAGE_RENDERTARGET)
            g_renderTargetTextures.insert((void*)*pp);
        else
            g_renderTargetTextures.erase((void*)*pp);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT depth, UINT lv, DWORD usage,
                                                       D3DFORMAT f, D3DPOOL pool, IDirect3DVolumeTexture9** pp, HANDLE* sh)
{
    bool remapped = g_deviceIsEx && pool == D3DPOOL_MANAGED;
    RemapManaged(1, usage, pool);
    HRESULT hr = ORIG(PFN_CreateVolumeTexture, VT_CreateVolumeTexture)(d, w, h, depth, lv, usage, f, pool, pp, sh);
    NoteRemapResult(1, hr, remapped, usage, w, h, (int)f);
    if (SUCCEEDED(hr) && pp && *pp)
        NoteVolumeTextureCreated(*pp, w, h, depth, f, remapped);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateCubeTexture(IDirect3DDevice9* d, UINT edge, UINT lv, DWORD usage, D3DFORMAT f,
                                                     D3DPOOL pool, IDirect3DCubeTexture9** pp, HANDLE* sh)
{
    bool remapped = g_deviceIsEx && pool == D3DPOOL_MANAGED;
    RemapManaged(2, usage, pool);
    HRESULT hr = ORIG(PFN_CreateCubeTexture, VT_CreateCubeTexture)(d, edge, lv, usage, f, pool, pp, sh);
    NoteRemapResult(2, hr, remapped, usage, edge, edge, (int)f);
    if (SUCCEEDED(hr) && pp && *pp)
        NoteCubeTextureCreated(*pp, edge, f, remapped);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateVertexBuffer(IDirect3DDevice9* d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                      IDirect3DVertexBuffer9** pp, HANDLE* sh)
{
    bool remapped = g_deviceIsEx && pool == D3DPOOL_MANAGED;
    RemapManaged(3, usage, pool);
    HRESULT hr = ORIG(PFN_CreateVertexBuffer, VT_CreateVertexBuffer)(d, length, usage, fvf, pool, pp, sh);
    NoteRemapResult(3, hr, remapped, usage, length, 0, (int)fvf);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateIndexBuffer(IDirect3DDevice9* d, UINT length, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                                     IDirect3DIndexBuffer9** pp, HANDLE* sh)
{
    bool remapped = g_deviceIsEx && pool == D3DPOOL_MANAGED;
    RemapManaged(4, usage, pool);
    HRESULT hr = ORIG(PFN_CreateIndexBuffer, VT_CreateIndexBuffer)(d, length, usage, f, pool, pp, sh);
    NoteRemapResult(4, hr, remapped, usage, length, 0, (int)f);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetRenderTarget(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* s)
{
    DeviceRestoreAll(d); // (the device resets the scissor rectangle with a new target; both sides then agree again)
    if (idx == 0 && g_streakUsed && s != g_renderTarget0)
        FlushStreakLayer(d);
    HRESULT hr =
        ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*), VT_SetRenderTarget)(d, idx, s);
    if (SUCCEEDED(hr) && idx == 0) {
        g_renderTarget0 = s;
        if (s) {
            D3DSURFACE_DESC surfaceDesc;
            if (SUCCEEDED(s->GetDesc(&surfaceDesc))) {
                g_rtWidth = surfaceDesc.Width;
                g_rtHeight = surfaceDesc.Height;
                g_gameViewport.X = 0;
                g_gameViewport.Y = 0;
                g_gameViewport.Width = surfaceDesc.Width;
                g_gameViewport.Height = surfaceDesc.Height;
                g_gameViewport.MinZ = 0;
                g_gameViewport.MaxZ = 1; // D3D resets the viewport
                g_gameScissorRect.left = 0;
                g_gameScissorRect.top = 0;
                g_gameScissorRect.right = surfaceDesc.Width;
                g_gameScissorRect.bottom = surfaceDesc.Height; // ... and the scissor rect
                DeviceNoteGameScissorRect(g_gameScissorRect);
            }
        } else
            g_rtWidth = g_rtHeight = 0;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetDepthStencilSurface(IDirect3DDevice9* d, IDirect3DSurface9* s)
{
    HRESULT hr =
        ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*), VT_SetDepthStencilSurface)(d, s);
    if (SUCCEEDED(hr))
        g_hasDepthBuffer = (s != NULL);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_Clear(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color,
                                         float z, DWORD stencil)
{
    DeviceRestoreAll(d);
    HRESULT hr = ORIG(PFN_Clear, VT_Clear)(d, count, rects, flags, color, z, stencil);
    if ((flags & D3DCLEAR_STENCIL) && g_vr.active)
        OnGameStencilClear(d, stencil);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp)
{
    HRESULT hr = ORIG(PFN_SetViewport, VT_SetViewport)(d, vp);
    if (SUCCEEDED(hr) && vp)
        g_gameViewport = *vp;
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v)
{
    if (st == D3DRS_SCISSORTESTENABLE) {
        g_gameScissorEnabled = v ? TRUE : FALSE;
        DeviceNoteGameScissorEnable(v != 0);
    }
    return ORIG(PFN_SetRenderState, VT_SetRenderState)(d, st, v);
}
static HRESULT STDMETHODCALLTYPE H_SetScissorRect(IDirect3DDevice9* d, const RECT* r)
{
    HRESULT hr = ORIG(PFN_SetScissorRect, VT_SetScissorRect)(d, r);
    if (SUCCEEDED(hr) && r) {
        g_gameScissorRect = *r;
        DeviceNoteGameScissorRect(*r);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t)
{
    if (stage < 16)
        g_boundTextures[stage] = t;
    return ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*), VT_SetTexture)(d, stage,
                                                                                                              t);
}

// The shader the game binds is looked up in a map by its pointer on every Set*Shader call; the same few shaders come back all
// the time, so a small direct-mapped cache answers most of them.  Creating a shader forgets it (a pointer can be reused).
static const int kShaderCacheSize = 16;
static struct {
    void* handle[kShaderCacheSize];
    VertexShaderInfo* info[kShaderCacheSize];
} g_vsCache;
static struct {
    void* handle[kShaderCacheSize];
    PixelShaderInfo* info[kShaderCacheSize];
} g_psCache;
static inline unsigned ShaderCacheSlot(const void* handle)
{
    return (unsigned)(((uintptr_t)handle >> 4) & (kShaderCacheSize - 1));
}

static HRESULT STDMETHODCALLTYPE H_CreateVertexShader(IDirect3DDevice9* d, const DWORD* fn,
                                                      IDirect3DVertexShader9** out)
{
    HRESULT hr = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**),
                      VT_CreateVertexShader)(d, fn, out);
    if (SUCCEEDED(hr) && out && *out && fn) {
        VertexShaderInfo* info = new VertexShaderInfo();
        int id = g_vertexShaderCount++;
        ParseVertexShaderConstants(fn, info, id);
        auto it = g_vertexShaders.find(*out);
        if (it != g_vertexShaders.end())
            delete it->second;
        g_vertexShaders[*out] = info;
        memset(&g_vsCache, 0, sizeof(g_vsCache));
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s)
{
    HRESULT hr =
        ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*), VT_SetVertexShader)(d, s);
    if (SUCCEEDED(hr)) {
        unsigned slot = ShaderCacheSlot(s);
        if (s && g_vsCache.handle[slot] == s)
            g_currentVsInfo = g_vsCache.info[slot];
        else {
            auto it = s ? g_vertexShaders.find(s) : g_vertexShaders.end();
            g_currentVsInfo = (it != g_vertexShaders.end()) ? it->second : NULL;
            if (s) {
                g_vsCache.handle[slot] = s;
                g_vsCache.info[slot] = g_currentVsInfo;
            }
        }
        g_currentVsHandle = s;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetVertexShaderConstantF(IDirect3DDevice9* d, UINT start, const float* data,
                                                            UINT count)
{
    if (data && start < 256) {
        UINT n = count;
        if (start + n > 256)
            n = 256 - start;
        memcpy(&g_vsConstants[start * 4], data, n * 4 * sizeof(float));
    }
    DeviceNoteGameVsConstants(start, count);
    return ORIG(PFN_SetShaderConstantF, VT_SetVertexShaderConstantF)(d, start, data, count);
}

static HRESULT STDMETHODCALLTYPE H_CreatePixelShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** out)
{
    HRESULT hr = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**),
                      VT_CreatePixelShader)(d, fn, out);
    if (SUCCEEDED(hr) && out && *out && fn) {
        PixelShaderInfo* info = new PixelShaderInfo();
        info->id = g_pixelShaderCount++;
        ParsePixelShaderConstants(fn, info);
        auto it = g_pixelShaders.find(*out);
        if (it != g_pixelShaders.end())
            delete it->second;
        g_pixelShaders[*out] = info;
        memset(&g_psCache, 0, sizeof(g_psCache));
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s)
{
    HRESULT hr = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*), VT_SetPixelShader)(d, s);
    if (SUCCEEDED(hr)) {
        unsigned slot = ShaderCacheSlot(s);
        if (s && g_psCache.handle[slot] == s)
            g_currentPsInfo = g_psCache.info[slot];
        else {
            auto it = s ? g_pixelShaders.find(s) : g_pixelShaders.end();
            g_currentPsInfo = (it != g_pixelShaders.end()) ? it->second : NULL;
            if (s) {
                g_psCache.handle[slot] = s;
                g_psCache.info[slot] = g_currentPsInfo;
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetPixelShaderConstantF(IDirect3DDevice9* d, UINT start, const float* data,
                                                           UINT count)
{
    if (data && start < 256) {
        UINT n = count;
        if (start + n > 256)
            n = 256 - start;
        memcpy(&g_psConstants[start * 4], data, n * 4 * sizeof(float));
    }
    return ORIG(PFN_SetShaderConstantF, VT_SetPixelShaderConstantF)(d, start, data, count);
}

static HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT c)
{
    auto fn = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT), VT_DrawPrimitive);
    return RunDraw(d, [&]() {
        PerfDriverScope perfDriver;
        return fn(d, t, s, c);
    });
}
static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mv,
                                                        UINT nv, UINT si, UINT pc)
{
    auto fn = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT),
                   VT_DrawIndexedPrimitive);
    return RunDraw(d, [&]() {
        PerfDriverScope perfDriver;
        return fn(d, t, bv, mv, nv, si, pc);
    });
}
static HRESULT STDMETHODCALLTYPE H_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, const void* v,
                                                   UINT st)
{
    auto fn = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT),
                   VT_DrawPrimitiveUP);
    return RunDraw(d, [&]() {
        PerfDriverScope perfDriver;
        return fn(d, t, pc, v, st);
    });
}
static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mv, UINT nv,
                                                          UINT pc, const void* id, D3DFORMAT f, const void* vd, UINT st)
{
    auto fn = ORIG(HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                               D3DFORMAT, const void*, UINT),
                   VT_DrawIndexedPrimitiveUP);
    return RunDraw(d, [&]() {
        PerfDriverScope perfDriver;
        return fn(d, t, mv, nv, pc, id, f, vd, st);
    });
}

static bool g_deviceHooked = false;

void HookDevice(IDirect3DDevice9* dev)
{
    void** vt = *(void***)dev;
    if (g_deviceHooked)
        return;
#define HK(idx, fn) PatchVtable(vt, idx, (void*)fn, &g_deviceOriginals[idx])
    HK(VT_Present, H_Present);
    HK(VT_Reset, H_Reset);
    HK(VT_CreateTexture, H_CreateTexture);
    if (g_deviceIsEx) { // (no managed pool on an Ex device)
        HK(VT_CreateVolumeTexture, H_CreateVolumeTexture);
        HK(VT_CreateCubeTexture, H_CreateCubeTexture);
        HK(VT_CreateVertexBuffer, H_CreateVertexBuffer);
        HK(VT_CreateIndexBuffer, H_CreateIndexBuffer);
    }
    HK(VT_SetRenderTarget, H_SetRenderTarget);
    HK(VT_SetDepthStencilSurface, H_SetDepthStencilSurface);
    HK(VT_Clear, H_Clear);
    HK(VT_SetViewport, H_SetViewport);
    HK(VT_SetTexture, H_SetTexture);
    HK(VT_SetRenderState, H_SetRenderState);
    HK(VT_SetScissorRect, H_SetScissorRect);
    HK(VT_CreateVertexShader, H_CreateVertexShader);
    HK(VT_SetVertexShader, H_SetVertexShader);
    HK(VT_SetVertexShaderConstantF, H_SetVertexShaderConstantF);
    HK(VT_CreatePixelShader, H_CreatePixelShader);
    HK(VT_SetPixelShader, H_SetPixelShader);
    HK(VT_SetPixelShaderConstantF, H_SetPixelShaderConstantF);
    HK(VT_DrawPrimitive, H_DrawPrimitive);
    HK(VT_DrawIndexedPrimitive, H_DrawIndexedPrimitive);
    HK(VT_DrawPrimitiveUP, H_DrawPrimitiveUP);
    HK(VT_DrawIndexedPrimitiveUP, H_DrawIndexedPrimitiveUP);
#undef HK
    g_deviceHooked = true;
    Log("device vtable hooked");
}
