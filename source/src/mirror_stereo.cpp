// Stereo rear-view mirror.  See mirror_stereo.h.
#include "mirror_stereo.h"
#include "camera.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "game_mode.h"
#include "render_state.h"
#include "shader_tracking.h"
#include "stereo_scene.h"
#include "vr_runtime.h"

IDirect3DTexture9* g_mirrorEyeTexture[2] = {NULL, NULL};
static IDirect3DSurface9* g_mirrorEyeSurface[2] = {NULL, NULL};
static UINT g_eyeTextureWidth = 0, g_eyeTextureHeight = 0;
static D3DFORMAT g_eyeTextureFormat = D3DFMT_UNKNOWN;
static unsigned long long g_mirrorSceneFrame = ~0ull; // frame in which the mirror scene was drawn in stereo
static unsigned long long g_mirrorCopyFrame = ~0ull;  // frame in which the eye textures were filled
static bool g_mirrorCopyOk = false;

// the rear camera's projection and orientation, remembered for draws that carry no recoverable view-projection
static double g_mirrorProj[4];
static CameraAxes g_mirrorAxes;
static CameraAxes g_mainAxes; // the main camera, as seen by the last main-pass draw

void NoteMainCameraAxes()
{
    g_mainAxes = g_lastCameraAxes;
    g_mainCamera.axes = g_lastCameraAxes;
    g_mainCamera.xScale = g_projXScale;
    g_mainCamera.yScale = g_projYScale;
    g_mainCamera.valid = g_lastCameraAxes.valid;
}

bool MirrorSceneRecent()
{
    return g_mirrorSceneFrame != ~0ull && g_vr.frameCount >= g_mirrorSceneFrame && g_vr.frameCount - g_mirrorSceneFrame <= 2;
}

bool IsMirrorScenePass()
{
    if (!g_cfg.mirror3d || !g_cfg.stereo || !g_cfg.hudWorldLocked || !g_vr.active || !g_matrixLayout || !g_rtHeight ||
        strcmp(g_currentLevel, "showroom") == 0) // (no mirror in the menu)
        return false;
    if (!g_renderTarget0 || g_renderTarget0 == g_backBufferSurface || g_rtWidth < 300 ||
        (double)g_rtWidth / g_rtHeight <= 3.0 || (double)g_rtWidth / g_rtHeight >= 12.0 || g_rtHeight < 64 ||
        IsMainSizedTarget() || HasBackBufferShape(g_rtWidth, g_rtHeight))
        return false;
    return g_currentVsInfo && !g_currentVsInfo->tracked.empty() && g_mainAxes.valid;
}

static double Dot(const double* a, const double* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// where the eye is, in the main camera's space (the point the eye matrix maps to the origin)
static void EyePosition(const Matrix4& d, double* out)
{
    for (int j = 0; j < 3; j++)
        out[j] = -(d.m[3][0] * d.m[j][0] + d.m[3][1] * d.m[j][1] + d.m[3][2] * d.m[j][2]);
}

HRESULT DrawMirrorSceneStereo(IDirect3DDevice9* device, const DrawCall& draw)
{
    // the rear camera
    double projection[4];
    CameraAxes axes;
    bool found = false;
    for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
        if (tracked.kind != TK_ViewProj || tracked.reg + 4 > 256)
            continue;
        found = RecoverCamera(&g_vsConstants[tracked.reg * 4], projection[0], projection[1], projection[2],
                              projection[3], axes);
        break;
    }
    if (found) {
        memcpy(g_mirrorProj, projection, sizeof(projection));
        g_mirrorAxes = axes;
    } else if (g_mirrorAxes.valid) {
        memcpy(projection, g_mirrorProj, sizeof(projection));
        axes = g_mirrorAxes;
    } else {
        static int logged = 0;
        if (logged++ < 3)
            Log("mirror pass %ux%u: rear camera not recovered, drawing it mono", g_rtWidth, g_rtHeight);
        return draw();
    }
    static bool announced = false;
    if (!announced) {
        announced = true;
        Log("mirror pass %ux%u: drawing it once per eye (projection a=%.3f b=%.3f)", g_rtWidth, g_rtHeight,
            projection[0], projection[1]);
    }

    // where each eye sits relative to the rear camera, along its own axes
    double eyePos[2][3], mid[3];
    for (int eye = 0; eye < 2; eye++)
        EyePosition(g_vr.eyeCameraDelta[eye], eyePos[eye]);
    for (int j = 0; j < 3; j++)
        mid[j] = 0.5 * (eyePos[0][j] + eyePos[1][j]);

    Matrix4 oldProj = Identity();
    oldProj.m[0][0] = projection[0];
    oldProj.m[1][1] = projection[1];
    oldProj.m[2][2] = projection[2];
    oldProj.m[2][3] = 1;
    oldProj.m[3][2] = projection[3];
    oldProj.m[3][3] = 0;

    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    D3DVIEWPORT9 base;
    device->GetViewport(&base);
    HRESULT hr = D3D_OK;
    for (int eye = 0; eye < 2; eye++) {
        double inCamera[3], world[3] = {0, 0, 0};
        for (int j = 0; j < 3; j++)
            inCamera[j] = g_cfg.mirrorHeadParallax * mid[j] + g_cfg.mirrorEyeScale * (eyePos[eye][j] - mid[j]);
        for (int j = 0; j < 3; j++)
            world[j] = inCamera[0] * g_mainAxes.right[j] + inCamera[1] * g_mainAxes.up[j] +
                       inCamera[2] * g_mainAxes.forward[j];
        double shift[2] = {Dot(world, axes.right), Dot(world, axes.up)};
        Matrix4 delta = Identity();
        delta.m[3][0] = -shift[0];
        delta.m[3][1] = -shift[1];
        Matrix4 newProj = oldProj;
        newProj.m[2][0] = projection[0] * shift[0] / fmax(0.3, g_cfg.mirrorConvergence);
        newProj.m[2][1] = projection[1] * shift[1] / fmax(0.3, g_cfg.mirrorConvergence);
        D3DVIEWPORT9 half = base;
        half.Width = base.Width / 2;
        half.X = base.X + (DWORD)eye * half.Width;
        setViewport(device, &half);
        ApplyCameraShift(device, oldProj, delta, newProj);
        hr = draw();
        RestoreEyeConstants(device);
    }
    setViewport(device, &base);
    g_mirrorSceneFrame = g_vr.frameCount;
    return hr;
}

void ReleaseMirrorStereo()
{
    for (int eye = 0; eye < 2; eye++) {
        if (g_mirrorEyeSurface[eye]) {
            g_mirrorEyeSurface[eye]->Release();
            g_mirrorEyeSurface[eye] = NULL;
        }
        if (g_mirrorEyeTexture[eye]) {
            g_mirrorEyeTexture[eye]->Release();
            g_mirrorEyeTexture[eye] = NULL;
        }
    }
    g_eyeTextureWidth = g_eyeTextureHeight = 0;
    g_mirrorCopyFrame = g_mirrorSceneFrame = ~0ull;
    g_mirrorCopyOk = false;
}

bool PrepareMirrorEyeTextures(IDirect3DDevice9* device, IDirect3DBaseTexture9* mirrorBase)
{
    if (!mirrorBase || mirrorBase->GetType() != D3DRTYPE_TEXTURE || !MirrorSceneRecent())
        return false;
    if (g_mirrorCopyFrame == g_vr.frameCount)
        return g_mirrorCopyOk;
    g_mirrorCopyFrame = g_vr.frameCount;
    g_mirrorCopyOk = false;
    IDirect3DTexture9* source = (IDirect3DTexture9*)mirrorBase;
    D3DSURFACE_DESC desc;
    if (FAILED(source->GetLevelDesc(0, &desc)))
        return false;
    if (!g_mirrorEyeTexture[0] || g_eyeTextureWidth != desc.Width || g_eyeTextureHeight != desc.Height ||
        g_eyeTextureFormat != desc.Format) {
        ReleaseMirrorStereo();
        PFN_CreateTexture createTexture = (PFN_CreateTexture)g_deviceOriginals[VT_CreateTexture];
        for (int eye = 0; eye < 2; eye++) {
            if (FAILED(createTexture(device, desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                     D3DPOOL_DEFAULT, &g_mirrorEyeTexture[eye], NULL)) ||
                !g_mirrorEyeTexture[eye] || FAILED(g_mirrorEyeTexture[eye]->GetSurfaceLevel(0, &g_mirrorEyeSurface[eye]))) {
                Log("mirror: could not create the per-eye textures (%ux%u format %d)", desc.Width, desc.Height,
                    (int)desc.Format);
                ReleaseMirrorStereo();
                g_mirrorCopyFrame = g_vr.frameCount;
                return false;
            }
        }
        g_eyeTextureWidth = desc.Width;
        g_eyeTextureHeight = desc.Height;
        g_eyeTextureFormat = desc.Format;
        g_mirrorSceneFrame = g_vr.frameCount;
        g_mirrorCopyFrame = g_vr.frameCount;
        Log("mirror: per-eye textures %ux%u created", desc.Width, desc.Height);
    }
    IDirect3DSurface9* sourceSurface = NULL;
    if (FAILED(source->GetSurfaceLevel(0, &sourceSurface)) || !sourceSurface)
        return false;
    bool ok = true;
    for (int eye = 0; eye < 2; eye++) {
        RECT part = {(LONG)(eye * (desc.Width / 2)), 0, (LONG)((eye + 1) * (desc.Width / 2)), (LONG)desc.Height};
        HRESULT hr = device->StretchRect(sourceSurface, &part, g_mirrorEyeSurface[eye], NULL, D3DTEXF_LINEAR);
        if (FAILED(hr)) {
            static int logged = 0;
            if (logged++ < 3)
                Log("mirror: StretchRect failed hr=0x%08x", (unsigned)hr);
            ok = false;
        }
    }
    sourceSurface->Release();
    g_mirrorCopyOk = ok;
    return ok;
}
