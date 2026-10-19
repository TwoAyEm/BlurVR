// Photo viewer in stereo.  See photo_stereo.h.
#include "photo_stereo.h"
#include <algorithm>
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "game_mode.h"
#include "hud.h"
#include "render_state.h"
#include "shader_tracking.h"
#include "vr_runtime.h"

static IDirect3DTexture9* g_photoEyeTexture[2] = {NULL, NULL};
static IDirect3DSurface9* g_photoEyeSurface[2] = {NULL, NULL};
static UINT g_photoWidth = 0, g_photoHeight = 0;
static D3DFORMAT g_photoFormat = D3DFMT_UNKNOWN;
static unsigned long long g_photoCopyFrame = ~0ull;
static IDirect3DBaseTexture9* g_photoCopySource = NULL;
static bool g_photoCopyOk = false;

static IDirect3DBaseTexture9* g_photoTexture = NULL; // the photo's render-target texture (compared only)
static unsigned long long g_photoClaimedFor = 0;     // the g_photoOpenTime the texture above was picked for

bool IsPhotoDraw()
{
    if (!g_photoOpenTime || !g_cfg.stereo || !g_cfg.hudWorldLocked || !g_vr.active || !g_vr.poseValid || !IsRaceMode())
        return false;
    // a draw to the back buffer in a frame that has the 3D menu scene, made with the UI's vertex shader (or none), that
    // samples a full-size render-target picture in slot 0 (the game's own post-processing and composite passes use other
    // vertex shaders)
    if (g_sceneDrawsThisFrame == 0 || !g_renderTarget0 || g_renderTarget0 != g_backBufferSurface || !IsMainSizedTarget() ||
        strcmp(g_currentLevel, "showroom") != 0)
        return false;
    if (g_currentVsInfo && !g_currentVsInfo->ui)
        return false;
    IDirect3DBaseTexture9* texture = g_boundTextures[0];
    if (!texture || texture->GetType() != D3DRTYPE_TEXTURE || !g_renderTargetTextures.count((void*)texture))
        return false;
    if (texture != g_photoTexture) {
        // the viewer draws the photo it has just loaded right away: the first such draw after the file was opened picks the
        // texture (later ones, for example the backdrop of a popup, are not the photo)
        if (g_photoClaimedFor == g_photoOpenTime || GetTickCount64() - g_photoOpenTime > 3000)
            return false;
        D3DSURFACE_DESC desc;
        if (FAILED(((IDirect3DTexture9*)texture)->GetLevelDesc(0, &desc)) ||
            desc.Width * 10 < g_backBufferWidth * 9 || desc.Height * 10 < g_backBufferHeight * 9)
            return false;
        g_photoTexture = texture;
        g_photoClaimedFor = g_photoOpenTime;
        Log("photo viewer: the photo is the render-target texture %p (%ux%u)", (void*)texture, desc.Width, desc.Height);
    }
    return true;
}

void ReleasePhotoStereo()
{
    for (int eye = 0; eye < 2; eye++) {
        if (g_photoEyeSurface[eye]) {
            g_photoEyeSurface[eye]->Release();
            g_photoEyeSurface[eye] = NULL;
        }
        if (g_photoEyeTexture[eye]) {
            g_photoEyeTexture[eye]->Release();
            g_photoEyeTexture[eye] = NULL;
        }
    }
    g_photoWidth = g_photoHeight = 0;
    g_photoCopyFrame = ~0ull;
    g_photoCopySource = NULL;
    g_photoCopyOk = false;
}

// Where each eye image of the photo sits, as a fraction of one eye image's width (+ = to the right), so that the two
// images are a proper pair on a flat panel.  Each eye image was rendered with its own off-axis projection (the headset's
// tangent box), so a point at infinity is at (0 - left) / (right - left) of the image: about 60% across in the left image and
// 40% in the right one.  Both are moved so that this point lands on the middle: infinity then sits on the panel, nearer
// things come out of it.
static void PhotoShifts(double* shift)
{
    for (int eye = 0; eye < 2; eye++) {
        const EyeBox& box = g_vr.eyeBox[eye];
        double width = box.right - box.left;
        double infinityAt = width > 1e-3 ? -box.left / width : 0.5;
        shift[eye] = 0.5 - infinityAt;
    }
}

// Fill one texture per eye with that eye's half of the photo, moved into place (PhotoShifts) and trimmed to the window both
// eyes have in common (a strip that only one eye can see is uncomfortable) on a transparent background, 1:1 and centred (the
// halves are about square, the layer is as wide as the screen).  Once per frame.
static bool PreparePhotoEyeTextures(IDirect3DDevice9* device, IDirect3DBaseTexture9* base)
{
    if (g_photoCopyFrame == g_vr.frameCount && g_photoCopySource == base)
        return g_photoCopyOk;
    g_photoCopyFrame = g_vr.frameCount;
    g_photoCopySource = base;
    g_photoCopyOk = false;
    IDirect3DTexture9* source = (IDirect3DTexture9*)base;
    D3DSURFACE_DESC desc;
    if (FAILED(source->GetLevelDesc(0, &desc)))
        return false;
    if (!g_photoEyeTexture[0] || g_photoWidth != desc.Width || g_photoHeight != desc.Height ||
        g_photoFormat != desc.Format) {
        ReleasePhotoStereo();
        g_photoCopyFrame = g_vr.frameCount;
        g_photoCopySource = base;
        PFN_CreateTexture createTexture = (PFN_CreateTexture)g_deviceOriginals[VT_CreateTexture];
        for (int eye = 0; eye < 2; eye++) {
            if (FAILED(createTexture(device, desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                     D3DPOOL_DEFAULT, &g_photoEyeTexture[eye], NULL)) ||
                !g_photoEyeTexture[eye] || FAILED(g_photoEyeTexture[eye]->GetSurfaceLevel(0, &g_photoEyeSurface[eye]))) {
                Log("photo viewer: could not create the per-eye textures (%ux%u format %d)", desc.Width, desc.Height,
                    (int)desc.Format);
                ReleasePhotoStereo();
                g_photoCopyFrame = g_vr.frameCount;
                g_photoCopySource = base;
                return false;
            }
        }
        g_photoWidth = desc.Width;
        g_photoHeight = desc.Height;
        g_photoFormat = desc.Format;
        Log("photo viewer: per-eye textures %ux%u created", desc.Width, desc.Height);
    }
    IDirect3DSurface9* sourceSurface = NULL;
    if (FAILED(source->GetSurfaceLevel(0, &sourceSurface)) || !sourceSurface)
        return false;
    bool ok = true;
    const double halfWidth = (double)(desc.Width / 2);
    double shift[2];
    PhotoShifts(shift);
    // the window (in eye image fractions, in the common frame the shifts refer to) that both eyes have
    double lo = fmax(shift[0], shift[1]), hi = 1.0 + fmin(shift[0], shift[1]);
    if (hi - lo < 0.2) { // nonsense tangent boxes: show the plain halves instead
        shift[0] = shift[1] = 0.0;
        lo = 0.0;
        hi = 1.0;
    }
    for (int eye = 0; eye < 2; eye++) {
        double sourceLo = fmax(0.0, lo - shift[eye]), sourceHi = fmin(1.0, hi - shift[eye]);
        LONG width = (LONG)((sourceHi - sourceLo) * halfWidth + 0.5);
        RECT part = {(LONG)(eye * halfWidth + sourceLo * halfWidth + 0.5), 0, 0, (LONG)desc.Height};
        part.right = part.left + width;
        LONG destLeft = (LONG)(desc.Width / 4 + (sourceLo + shift[eye]) * halfWidth + 0.5);
        RECT place = {destLeft, 0, destLeft + width, (LONG)desc.Height};
        HRESULT hr = D3DERR_INVALIDCALL;
        if (width > 0 && place.left >= 0 && (UINT)place.right <= desc.Width)
            hr = device->ColorFill(g_photoEyeSurface[eye], NULL, 0x00000000);
        if (SUCCEEDED(hr))
            hr = device->StretchRect(sourceSurface, &part, g_photoEyeSurface[eye], &place, D3DTEXF_NONE);
        if (FAILED(hr)) {
            static int logged = 0;
            if (logged++ < 3)
                Log("photo viewer: copying the eye halves failed hr=0x%08x", (unsigned)hr);
            ok = false;
        }
    }
    sourceSurface->Release();
    g_photoCopyOk = ok;
    return ok;
}

HRESULT DrawPhotoStereo(IDirect3DDevice9* device, const DrawCall& draw)
{
    IDirect3DBaseTexture9* source = g_boundTextures[0];
    if (!EnsureHudTargets(device) || !g_mirrorSurface[0] || !g_mirrorSurface[1] ||
        !PreparePhotoEyeTextures(device, source))
        return draw();
    static bool announced = false;
    if (!announced) {
        announced = true;
        Log("photo viewer: the photo is drawn once per eye into the per-eye HUD layers (left half = left eye)");
    }
    PFN_SetTexture setTexture = (PFN_SetTexture)g_deviceOriginals[VT_SetTexture];
    D3DVIEWPORT9 base = g_gameViewport;
    HRESULT hr = D3D_OK;
    for (int eye = 0; eye < 2; eye++) {
        setTexture(device, 0, g_photoEyeTexture[eye]);
        std::swap(g_hudSurface, g_mirrorSurface[eye]);
        std::swap(g_hudUsed, g_mirrorUsed[eye]);
        hr = DrawHudToTexture(device, base, draw);
        std::swap(g_hudSurface, g_mirrorSurface[eye]);
        std::swap(g_hudUsed, g_mirrorUsed[eye]);
    }
    setTexture(device, 0, source);
    return hr;
}
