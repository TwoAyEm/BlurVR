// Per-draw-call dispatch: decide how each game draw is handled and run it.
#include "draw_dispatch.h"
#include "camera.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "device_state.h"
#include "game_mode.h"
#include "hud.h"
#include "mirror_stereo.h"
#include "perf_probe.h"
#include "photo_stereo.h"
#include "render_state.h"
#include "screen_space_passes.h"
#include "shader_tracking.h"
#include "stereo_scene.h"
#include "streaks.h"
#include "vr_runtime.h"

static bool g_drawIsMirror = false; // the draw just classified is the rear-view mirror picture (plan 2)
static int g_mirrorSlot = -1;                       // ... and the texture slot it samples the mirror render target from
static IDirect3DBaseTexture9* g_mirrorSource = NULL;

// The occlusion query behind HudDrawCoversScreen makes the CPU wait for the GPU to drain its queue, about 2.5 ms of CPU time at 3840x2160 every
// time it happens in a frame, and in a race the UI's small fill textures make a dozen draws candidates: the probe showed the UI draws taking
// 3.35 ms of the mod's 5.4 ms.  The same candidates come back in the same order with the same coverage every frame, so in a race the
// coverage of each is measured without waiting: its draw runs into the scratch target with a query of its own every CoverageEvery frames
// (staggered by position), the result is picked up a frame or two later, and every frame uses the last result.  A draw that has no result
// yet (first sight, or the HUD changed at that position) is measured with the wait when it matters (the race-start intro, the menus), and
// is taken as not covering until its result arrives when it does not (the steady race).
struct CoverEntry {
    bool valid, got;
    int ps, vs;
    void* tex0;
    double frac;
};
static CoverEntry g_cover[64];
static unsigned long long g_coverFrame = ~0ull;
static unsigned g_coverOrdinal = 0;
static bool CoverageOfCandidate(IDirect3DDevice9* device, const D3DVIEWPORT9& base, double minFrac, double* outFrac,
                                const DrawCall& draw, bool needNow)
{
    if (g_coverFrame != g_vr.frameCount) {
        g_coverFrame = g_vr.frameCount;
        g_coverOrdinal = 0;
    }
    unsigned ordinal = g_coverOrdinal++;
    unsigned long long every = g_cfg.coverageEvery < 1 ? 1 : (unsigned long long)g_cfg.coverageEvery;
    if (every == 1 || ordinal >= 64)
        return HudDrawCoversScreen(device, base, minFrac, outFrac, draw);
    CoverEntry& entry = g_cover[ordinal];
    CoverageSlot& slot = HudCoverageSlot(ordinal);
    int ps = g_currentPsInfo ? g_currentPsInfo->id : -1, vs = g_currentVsInfo ? g_currentVsInfo->id : -1;
    void* tex0 = (void*)g_boundTextures[0];
    double polled;
    if (slot.pending && HudCoveragePoll(slot, &polled) && entry.valid && slot.ps == entry.ps && slot.vs == entry.vs &&
        slot.tex0 == entry.tex0) {
        entry.frac = polled; // (only if the draw measured is the one at this position now: checked below by the signature)
        entry.got = true;
    }
    bool same = entry.valid && entry.ps == ps && entry.vs == vs && entry.tex0 == tex0;
    if (!same) {
        bool hadBefore = entry.valid;
        if (needNow || !hadBefore) { // measured now, waiting
            double frac = 0.0;
            bool covers = HudDrawCoversScreen(device, base, minFrac, &frac, draw);
            entry.valid = true;
            entry.got = covers || frac > 0.0;
            entry.ps = ps;
            entry.vs = vs;
            entry.tex0 = tex0;
            entry.frac = frac;
            if (outFrac)
                *outFrac = frac;
            return covers;
        }
        // something else is at this position now: not covering until a measurement of it comes back
        entry.valid = true;
        entry.got = false;
        entry.ps = ps;
        entry.vs = vs;
        entry.tex0 = tex0;
        entry.frac = 0.0;
        if (!slot.pending) {
            slot.ps = ps;
            slot.vs = vs;
            slot.tex0 = tex0;
            HudCoverageIssue(device, base, slot, draw);
        }
    } else if (!slot.pending && (g_vr.frameCount + ordinal) % every == 0) {
        slot.ps = ps;
        slot.vs = vs;
        slot.tex0 = tex0;
        HudCoverageIssue(device, base, slot, draw);
    }
    if (outFrac)
        *outFrac = entry.got ? entry.frac : 0.0;
    return entry.got && entry.frac >= minFrac;
}

// 0 = normal draw, 1 = main-pass scene draw (stereo), 2 = HUD draw (per eye)
static int ClassifyDraw(IDirect3DDevice9* device)
{
    g_drawIsMirror = false;
    if (g_vrStartDeferred && !g_vr.active) {
        if (g_currentVsInfo && !g_currentVsInfo->tracked.empty() && IsMainSizedTarget())
            g_preVrSceneDraws++;
        return 0;
    }
    if (!g_vr.active || !g_vr.poseValid)
        return 0;
    if (!g_rtWidth) {
        DeviceRestoreAll(device); // (reading the device's state: it must hold the game's)
        RefreshRenderTargetState(device);
    }
    if (!IsRaceMode()) {
        if (g_levelLoading && g_levelFilesSeen && g_modeOverride == 0 && g_currentVsInfo &&
            !g_currentVsInfo->tracked.empty() && IsMainSizedTarget())
            g_sceneProbeDraws++;
        return 0;
    }
    if ((IsMainPass() || IsHalfResPass()) && g_currentVsInfo && !g_currentVsInfo->tracked.empty()) {
        bool ok = false, hasOther = false;
        for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
            if (tracked.reg + 4 > 256)
                continue;
            if (tracked.kind == TK_ViewProj) {
                if (TryRecoverProjection(&g_vsConstants[tracked.reg * 4])) {
                    ok = true;
                    break;
                } else if (g_decomposeFailLogs < 4) {
                    g_decomposeFailLogs++;
                    const float* registers = &g_vsConstants[tracked.reg * 4];
                    Log("VP decompose failed (layout %d): %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g",
                        g_matrixLayout, registers[0], registers[1], registers[2], registers[3], registers[4],
                        registers[5], registers[6], registers[7], registers[8], registers[9], registers[10],
                        registers[11], registers[12], registers[13], registers[14], registers[15]);
                }
            } else if (tracked.kind == TK_WorldViewProj || tracked.kind == TK_WorldView ||
                       tracked.kind == TK_Projection || tracked.kind == TK_View)
                hasOther = true;
        }
        if (ok && IsMainPass())
            NoteMainCameraAxes();
        if (ok || (hasOther && g_haveProjection)) {
            g_sceneDrawsThisFrame++;
            return 1;
        }
    }
    // HUD: draws to the back buffer, after the 3D scene, that do not read a render-target texture
    // Rear-view mirror: a wide, short render-target texture (e.g. 864x160) composited into the main image
    if (g_cfg.stereo && g_sceneDrawsThisFrame > 0 && IsMainSizedTarget() &&
        (!g_currentVsInfo || g_currentVsInfo->tracked.empty()) && strcmp(g_currentLevel, "showroom") != 0) { // (no mirror in the menu)
        for (int slot = 0; slot < 8; slot++) {
            IDirect3DBaseTexture9* boundTexture = g_boundTextures[slot];
            if (boundTexture && g_renderTargetTextures.count((void*)boundTexture) &&
                boundTexture->GetType() == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC surfaceDesc;
                if (SUCCEEDED(((IDirect3DTexture9*)boundTexture)->GetLevelDesc(0, &surfaceDesc)) &&
                    surfaceDesc.Height >= 64 && surfaceDesc.Width >= 300 &&
                    (double)surfaceDesc.Width / surfaceDesc.Height > 3.0 &&
                    (double)surfaceDesc.Width / surfaceDesc.Height < 12.0 && // not a 2160x16 strip (results screen)
                    !HasBackBufferShape(surfaceDesc.Width, surfaceDesc.Height)) { // nor the scene itself, on a 3.6:1 back buffer
                    g_drawIsMirror = true;
                    g_mirrorSlot = slot;
                    g_mirrorSource = boundTexture;
                    return 2;
                }
            }
        }
    }
    // Menu UI is composed into its own full-size off-screen layer before the 3D scene: redirect those UI draws to the
    // HUD layer too
    if (g_cfg.stereo && g_renderTarget0 && g_renderTarget0 != g_backBufferSurface &&
        IsMainSizedTarget() && g_currentVsInfo && g_currentVsInfo->tracked.empty() &&
        g_currentVsInfo->ui) {
        return 2;
    }
    if (g_cfg.stereo && g_sceneDrawsThisFrame > 0 && g_renderTarget0 &&
        g_renderTarget0 == g_backBufferSurface && IsMainSizedTarget()) {
        // HUD: alpha-blended, no depth test, drawn to the back buffer after the scene (panels sample a blurred scene
        // RT, so render-target textures may be bound; the final full-screen composite is not alpha blended and is
        // excluded).
        DWORD zEnable = 1, alphaBlend = 0;
        device->GetRenderState(D3DRS_ZENABLE, &zEnable);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
        // Everything untracked drawn to the back buffer after the scene is 2D UI, except the opaque full-screen
        // composite.
        bool composite = false;
        bool uiShader = g_currentVsInfo && g_currentVsInfo->ui;
        if (!uiShader &&
            (!g_currentVsInfo ||
             g_currentVsInfo->otherConstantNames.empty())) { // shader-less full-screen blits that read a full-size
                                                             // render target (the menu's scene composite)
            for (int slot = 0; slot < 8 && !composite; slot++) {
                IDirect3DBaseTexture9* boundTexture = g_boundTextures[slot];
                if (boundTexture && g_renderTargetTextures.count((void*)boundTexture) &&
                    boundTexture->GetType() == D3DRTYPE_TEXTURE)
                    composite = true; // scene composite, bloom add: full-screen, not HUD
            }
        }
        if (alphaBlend == 0) {
            for (int slot = 0; slot < 8 && !composite; slot++) {
                IDirect3DBaseTexture9* boundTexture = g_boundTextures[slot];
                if (boundTexture && g_renderTargetTextures.count((void*)boundTexture) &&
                    boundTexture->GetType() == D3DRTYPE_TEXTURE) {
                    D3DSURFACE_DESC surfaceDesc;
                    if (SUCCEEDED(((IDirect3DTexture9*)boundTexture)->GetLevelDesc(0, &surfaceDesc)) &&
                        surfaceDesc.Width >= g_backBufferWidth * 0.9)
                        composite = true;
                }
            }
        }
        (void)zEnable;
        if (!composite) {
            return 2;
        }
    }
    return 0;
}

// last time a race-start intro piece (letterbox fill / blur picture) was dropped: the intro is still running only while
// these keep arriving (they stop the moment the player skips the lead-in)
static unsigned long long g_lastIntroPieceTime = 0;

// One of the game's own glow draws (additive, ONE / ONE) at pct % of its strength.  The blend factor scales what is added, up to
// 100 %; above that the draw is repeated for the rest.  0 drops it.
static HRESULT DrawGlowAtStrength(IDirect3DDevice9* device, const DrawCall& draw, int pct)
{
    if (pct <= 0)
        return D3D_OK;
    static int blendFactorSupported = -1;
    if (blendFactorSupported < 0) {
        D3DCAPS9 caps;
        blendFactorSupported = SUCCEEDED(device->GetDeviceCaps(&caps)) && (caps.SrcBlendCaps & D3DPBLENDCAPS_BLENDFACTOR) ? 1 : 0;
    }
    if (!blendFactorSupported) // (practically every card has it) no way to scale: the nearer of "as drawn" and "none"
        return pct >= 50 ? draw() : D3D_OK;
    if (pct > 200)
        pct = 200;
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    DWORD savedFactor = 0;
    device->GetRenderState(D3DRS_BLENDFACTOR, &savedFactor);
    setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR);
    HRESULT hr = D3D_OK;
    for (int left = pct; left > 0; left -= 100) {
        DWORD level = (DWORD)((left > 100 ? 100 : left) * 255 / 100);
        setRenderState(device, D3DRS_BLENDFACTOR, D3DCOLOR_ARGB(255, level, level, level));
        hr = draw();
    }
    setRenderState(device, D3DRS_BLENDFACTOR, savedFactor);
    setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_ONE);
    return hr;
}

HRESULT RunDraw(IDirect3DDevice9* device, const DrawCall& draw)
{
    PerfDrawScope perf(device);
    if (g_cfg.bloomPct != 100 && g_levelLoading && g_levelFilesSeen && !IsRaceMode() && g_renderTarget0 &&
        g_renderTarget0 == g_backBufferSurface && g_boundTextures[0] &&
        g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE && g_renderTargetTextures.count((void*)g_boundTextures[0])) {
        // the loading screen's glow: a small blurred copy of the picture added over the whole screen.  Bloom scales it like the glow of the
        // menus (0 drops it; on the floating screen it otherwise just washes the picture out, a restart, drawn through the HUD layer,
        // never shows it)
        DWORD blend = 0, src = 0, dest = 0;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
        device->GetRenderState(D3DRS_SRCBLEND, &src);
        device->GetRenderState(D3DRS_DESTBLEND, &dest);
        D3DSURFACE_DESC glowDesc;
        if (blend && src == D3DBLEND_ONE && dest == D3DBLEND_ONE &&
            SUCCEEDED(((IDirect3DTexture9*)g_boundTextures[0])->GetLevelDesc(0, &glowDesc)) &&
            glowDesc.Width * 4 <= g_backBufferWidth) {
            return DrawGlowAtStrength(device, draw, g_cfg.bloomPct);
        }
    }
    int plan = ClassifyDraw(device);
    perf.Classified(plan);
    if (plan != 1)
        DeviceRestoreAll(device); // only the per-eye draws below may find the device with the game's constants rewritten
    if (IsPhotoDraw())
        return DrawPhotoStereo(device, draw); // the photo viewer's picture: once per eye, onto the HUD panel
    if (plan == 0 && g_vr.active && g_cfg.bloomPct > 0 && g_menuGlowFrame && g_sceneDrawsThisFrame > 0 &&
        strcmp(g_currentLevel, "showroom") != 0 && g_renderTarget0 && g_renderTarget0 == g_backBufferSurface &&
        IsMainSizedTarget() && g_boundTextures[0] && g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE &&
        g_renderTargetTextures.count((void*)g_boundTextures[0])) { // (slot 1 can hold a stale texture: car selector)
        // pause / results menu in the race: the game adds the glow of its UI over the whole picture at the very end (a small
        // render target, added). Keep it for our layers instead, where it follows the UI in each eye.
        DWORD ab = 0, sb = 0, db = 0;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        device->GetRenderState(D3DRS_SRCBLEND, &sb);
        device->GetRenderState(D3DRS_DESTBLEND, &db);
        D3DSURFACE_DESC gd;
        if (ab && sb == D3DBLEND_ONE && db == D3DBLEND_ONE &&
            SUCCEEDED(((IDirect3DTexture9*)g_boundTextures[0])->GetLevelDesc(0, &gd)) && gd.Width * 8 < g_backBufferWidth) {
            NoteSceneBloom(g_boundTextures[0]);
            return D3D_OK;
        }
    }
    if (plan == 0 && g_vr.active && IsMainSizedTarget() && g_boundTextures[0] && g_boundTextures[1] && !g_boundTextures[2] &&
        g_renderTargetTextures.count((void*)g_boundTextures[0]) && g_renderTargetTextures.count((void*)g_boundTextures[1]) &&
        g_boundTextures[1]->GetType() == D3DRTYPE_TEXTURE && strcmp(g_currentLevel, "showroom") == 0) {
        // the game's last pass: the finished picture plus its bloom buffer (a quarter-size render target). The UI is
        // normally drawn into that picture before the bloom is added over it; our UI layers are drawn afterwards instead.
        D3DSURFACE_DESC bd;
        if (SUCCEEDED(((IDirect3DTexture9*)g_boundTextures[1])->GetLevelDesc(0, &bd)) && bd.Width * 5 > g_backBufferWidth &&
            bd.Width * 3 < g_backBufferWidth && g_cfg.bloomPct > 0 && g_renderTarget0 == g_backBufferSurface) {
            NoteSceneBloom(g_boundTextures[1]); // the UI's glow buffer, in the UI's own full-screen layout
            return D3D_OK;                      // (the UI and glow stretched over both eyes: they go onto our layers)
        }
    }
    if (plan == 0 && g_renderTarget0 && g_renderTarget0 == g_backBufferSurface && !g_sceneCompositeDrawn &&
        g_boundTextures[0] && g_renderTargetTextures.count((void*)g_boundTextures[0]) &&
        g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE) {
        DWORD currentAlphaBlend = 1;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &currentAlphaBlend);
        D3DSURFACE_DESC currentDesc;
        if (!currentAlphaBlend && SUCCEEDED(((IDirect3DTexture9*)g_boundTextures[0])->GetLevelDesc(0, &currentDesc)) &&
            currentDesc.Width >= g_backBufferWidth * 0.9)
            g_sceneCompositeDrawn = true;
    }
    if (plan == 0 && g_cfg.hudWorldLocked && g_cfg.stereo && g_vr.active && IsRaceMode() && g_renderTarget0 &&
        g_renderTarget0 == g_backBufferSurface && !g_currentVsInfo && g_boundTextures[0] &&
        g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE && !g_renderTargetTextures.count((void*)g_boundTextures[0]) &&
        strcmp(g_currentLevel, "showroom") == 0) {
        // the connecting/loading spinner and its dim are drawn first, straight to the screen, so our UI layer would
        // cover them: collect them in a separate layer drawn on top
        D3DSURFACE_DESC td;
        DWORD ab = 0, sb = 0, db = 0;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        device->GetRenderState(D3DRS_SRCBLEND, &sb);
        device->GetRenderState(D3DRS_DESTBLEND, &db);
        if (ab && sb == D3DBLEND_SRCALPHA && db == D3DBLEND_INVSRCALPHA &&
            SUCCEEDED(((IDirect3DTexture9*)g_boundTextures[0])->GetLevelDesc(0, &td)) &&
            ((td.Width == 1 && td.Height == 1) || (td.Width == 64 && td.Height == 768)) && EnsureHudTargets(device) &&
            g_topLayerSurface) {
            std::swap(g_hudSurface, g_topLayerSurface);
            std::swap(g_hudUsed, g_topLayerUsed);
            HRESULT layerResult = DrawHudToTexture(device, g_gameViewport, draw);
            std::swap(g_hudSurface, g_topLayerSurface);
            std::swap(g_hudUsed, g_topLayerUsed);
            return layerResult;
        }
    }
    if (plan == 0 && g_cfg.skipLensFlares && g_vr.active && IsRaceMode() && g_renderTarget0 &&
        g_renderTarget0 == g_backBufferSurface && (!g_currentVsInfo || g_currentVsInfo->tracked.empty()))
        return DrawWithoutBloom(device, draw);
    if (g_cfg.skipLensFlares && g_currentVsInfo && g_vr.active && IsRaceMode() &&
        (g_currentVsInfo->otherConstantNames.find("g_FlareSize") != std::string::npos ||
         g_currentVsInfo->otherConstantNames.find("g_FlareColour") != std::string::npos))
        return D3D_OK;
    if (plan == 0 && g_cfg.bloomPct > 0 && g_menuGlowFrame && g_vr.active && strcmp(g_currentLevel, "showroom") != 0 &&
        IsRaceMode() && IsHalfResPass() && g_renderTarget0 != g_backBufferSurface && g_currentVsInfo &&
        g_currentVsInfo->ui) {
        // the 2D elements' glow only: a UI draw that samples a render target (the rear-view mirror picture, the scene) stays out
        for (int t = 0; t < 4; t++)
            if (g_boundTextures[t] && g_boundTextures[t]->GetType() == D3DRTYPE_TEXTURE &&
                g_renderTargetTextures.count((void*)g_boundTextures[t]))
                return D3D_OK;
    }
    if (plan == 0 && g_vr.active && IsRaceMode() && IsHalfResPass() &&
        g_renderTarget0 != g_backBufferSurface && g_currentVsInfo &&
        !(g_cfg.bloomPct > 0 && (strcmp(g_currentLevel, "showroom") == 0 || g_menuGlowFrame)) && // menus keep the game's glow chain
        g_currentVsInfo->ui) {
        return D3D_OK;
    }
    if (plan == 0 && g_vr.active && IsRaceMode() && IsHalfResPass() &&
        g_renderTarget0 != g_backBufferSurface && g_currentVsInfo && g_currentVsInfo->otherConstantNames.empty() &&
        g_currentVsInfo->id != 0) {
        // untracked alpha-blended sprites (light streaks/halos) positioned by the game's mono camera -> would ghost
        // across both eyes
        DWORD ab = 0;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        if (ab && g_cfg.stereo && g_cfg.hudWorldLocked && EnsureStreakLayer(device)) {
            return DrawStreakToLayer(device, draw);
        }
    }
    if (plan == 0 && g_cfg.stereo && g_vr.active && IsRaceMode() && g_haveProjection &&
        g_currentPsInfo && g_currentPsInfo->motionBlur && g_rtWidth >= 64)
        return MotionBlurPerEye(device, draw);
    if (plan == 0 && g_cfg.stereo && g_vr.active && IsRaceMode() && g_haveProjection &&
        g_currentPsInfo && g_currentPsInfo->lightPosReg >= 0 && g_currentVsInfo && g_rtWidth >= 64) {
        int farCornerRegister = FindFarCornerRegister();
        if (farCornerRegister >= 0)
            return DrawLightsPerEye(device, draw, farCornerRegister);
    }
    if (plan == 0 && IsMirrorScenePass())
        return DrawMirrorSceneStereo(device, draw);
    if (plan == 0)
        return draw();
    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    D3DVIEWPORT9 base = g_gameViewport;
    HRESULT hr = D3D_OK;
    if (plan == 1) {
        int eyeCount = g_cfg.stereo ? 2 : 1;
        if (g_cfg.stereo && g_cfg.seamGutter > 0 && IsMainPass() &&
            g_gutterClearedFrame != g_vr.frameCount) {
            g_gutterClearedFrame = g_vr.frameCount;
            D3DRECT gutterRects[2];
            LONG half = (LONG)(base.Width / 2);
            gutterRects[0].x1 = (LONG)base.X + half - g_cfg.seamGutter;
            gutterRects[0].x2 = (LONG)base.X + half;
            gutterRects[0].y1 = (LONG)base.Y;
            gutterRects[0].y2 = (LONG)(base.Y + base.Height);
            gutterRects[1].x1 = (LONG)base.X + half;
            gutterRects[1].x2 = (LONG)base.X + half + g_cfg.seamGutter;
            gutterRects[1].y1 = gutterRects[0].y1;
            gutterRects[1].y2 = gutterRects[0].y2;
            DeviceSetScissorEnabled(device, false);
            ((PFN_Clear)g_deviceOriginals[VT_Clear])(device, 2, gutterRects, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        }
        // The two eyes are drawn in the opposite order every other draw: the second eye of one draw and the first of the next
        // are the same eye, so the scissor rectangle and the matrices that do not change between draws are already in place.
        // (The eyes cover separate halves of the target, so the order does not change the picture.)
        static unsigned eyeParity = 0;
        eyeParity++;
        // (the two helpers below return at once unless the pixel shader carries the constants they rewrite)
        const bool mayHaveShadowDecal = g_currentPsInfo && g_currentPsInfo->shadowMatrixReg >= 0;
        const bool mayHaveLightVolume = g_currentPsInfo && g_currentPsInfo->viewRayScaleReg >= 0 &&
                                        g_currentPsInfo->lightMatrixReg >= 0 && g_currentPsInfo->lightPosReg >= 0;
        const bool mayHaveCamera = g_currentPsInfo && g_currentPsInfo->eyeConstants;
        bool cameraApplied = false;
        for (int n = 0; n < eyeCount; n++) {
            int i = (eyeCount == 2) ? (int)((n + eyeParity) & 1) : 0;
            int e = g_cfg.stereo ? i : 2;
            if (g_cfg.stereo) {
                LONG half = (LONG)(base.Width / 2);
                RECT r;
                r.left = (LONG)base.X + i * half;
                r.right = r.left + half;
                r.top = (LONG)base.Y;
                r.bottom = (LONG)(base.Y + base.Height);
                LONG gutterPixels = (g_cfg.seamGutter != 0 && g_backBufferWidth)
                                        ? (LONG)((double)g_cfg.seamGutter * base.Width / g_backBufferWidth)
                                        : 0;
                if (i == 0)
                    r.right -= gutterPixels;
                else
                    r.left += gutterPixels;
                DeviceSetEyeScissor(device, r);
            }
            ApplyEyeConstants(device, e);
            if (mayHaveCamera && ApplyEyePixelConstants(device, e))
                cameraApplied = true;
            float savedShadowConstants[16];
            bool shadowApplied = mayHaveShadowDecal && ApplyShadowDecalForEye(device, e, savedShadowConstants);
            float savedLightConstants[24];
            bool lightApplied = !shadowApplied && mayHaveLightVolume && ApplyLightVolumeForEye(device, e, savedLightConstants);
            hr = draw();
            if (shadowApplied)
                ((PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF])(
                    device, (UINT)g_currentPsInfo->shadowMatrixReg, savedShadowConstants, 4);
            if (lightApplied) {
                PFN_SetShaderConstantF setPixelConst =
                    (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
                setPixelConst(device, (UINT)g_currentPsInfo->viewRayScaleReg, savedLightConstants, 1);
                setPixelConst(device, (UINT)g_currentPsInfo->lightMatrixReg, savedLightConstants + 4, 4);
                setPixelConst(device, (UINT)g_currentPsInfo->lightPosReg, savedLightConstants + 20, 1);
            }
        }
        if (cameraApplied)
            RestoreEyePixelConstants(device);
        // (the game's constants and scissor state come back when something else uses the device: see DeviceRestoreAll)
    } else if (g_drawIsMirror && g_cfg.mirror3d && g_cfg.hudWorldLocked && EnsureHudTargets(device) &&
               g_mirrorSurface[0] && g_mirrorSurface[1]) {
        // the rear-view mirror picture goes to one layer per eye; when the mirror scene was rendered in
        // stereo, each layer is filled from that eye's own picture
        bool stereoPictures = PrepareMirrorEyeTextures(device, g_mirrorSource);
        PFN_SetTexture setTexture = (PFN_SetTexture)g_deviceOriginals[VT_SetTexture];
        for (int eye = 0; eye < 2; eye++) {
            if (stereoPictures)
                setTexture(device, (DWORD)g_mirrorSlot, g_mirrorEyeTexture[eye]);
            std::swap(g_hudSurface, g_mirrorSurface[eye]);
            std::swap(g_hudUsed, g_mirrorUsed[eye]);
            hr = DrawHudToTexture(device, base, draw);
            std::swap(g_hudSurface, g_mirrorSurface[eye]);
            std::swap(g_hudUsed, g_mirrorUsed[eye]);
        }
        if (stereoPictures)
            setTexture(device, (DWORD)g_mirrorSlot, g_mirrorSource);
    } else if (g_cfg.hudWorldLocked && EnsureHudTargets(device) && g_multiplySurface && IsMultiplicativeBlend(device)) {
        hr = DrawHudToMultiplyLayer(device, base, draw);
    } else if (g_cfg.hudWorldLocked && EnsureHudTargets(device)) {
        // The game blurs its UI render target into a glow (the lavender halo and bright text around menu popups) and adds
        // it in its last pass. The UI goes to our layer instead, so also draw it into the game's own target: its glow
        // chain then runs on the real UI, and the glow is added over our layers (see NoteSceneBloom, ComposeHudLayers).
        const bool shadowUi = g_cfg.bloomPct > 0 && g_vr.active && strcmp(g_currentLevel, "showroom") == 0 &&
                              g_renderTarget0 && g_renderTarget0 != g_backBufferSurface && IsMainSizedTarget();
        // an opaque full-screen copy of the scene (menu backdrop) would paint a washed-out rectangle over the 3D view:
        // drop it
        DWORD ab0 = 1;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab0);
        {
            // loading/intro video (level start only): a 512x256 second texture next to a plain first one. The game's
            // rear-view mirror wrapper and the pieces drawn after it leave that same 512x256 texture bound for the whole
            // race, so this only applies while the intro is still running (its fill pieces keep arriving; they stop when
            // the player skips the lead-in and the race starts early)
            unsigned long long nowTick = GetTickCount64();
            bool introRunning = (nowTick - g_lastIntroPieceTime < 300) ||
                                (g_lastIntroPieceTime < g_raceStartTime && nowTick - g_raceStartTime < 1500);
            if (introRunning && g_cfg.dropBackdropCopies && g_raceStartTime && nowTick - g_raceStartTime < 10000 &&
                strcmp(g_currentLevel, "showroom") != 0 && g_boundTextures[0] && g_boundTextures[1] &&
                !g_boundTextures[2] && !g_renderTargetTextures.count((void*)g_boundTextures[0]) &&
                !g_renderTargetTextures.count((void*)g_boundTextures[1]) &&
                g_boundTextures[1]->GetType() == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC videoDesc;
                if (SUCCEEDED(((IDirect3DTexture9*)g_boundTextures[1])->GetLevelDesc(0, &videoDesc)) &&
                    videoDesc.Width == 512 && videoDesc.Height == 256) {
                    g_loadingVideoDropped = true;
                    DrawHudStencilOnly(device, base, draw);
                    return D3D_OK;
                }
            }
            // a blended full-size copy of the scene (the dimmed/blurred backdrop behind the count-in, results and pause
            // screens) would blank the whole view: drop it, keep the UI on top
            for (int t = 0; t < 8 && g_cfg.dropBackdropCopies; t++) {
                IDirect3DBaseTexture9* boundTexture = g_boundTextures[t];
                if (!boundTexture || !g_renderTargetTextures.count((void*)boundTexture) ||
                    boundTexture->GetType() != D3DRTYPE_TEXTURE)
                    continue;
                D3DSURFACE_DESC surfaceDesc;
                if (FAILED(((IDirect3DTexture9*)boundTexture)->GetLevelDesc(0, &surfaceDesc)))
                    continue;
                if (surfaceDesc.Width >= g_backBufferWidth * 0.9 && surfaceDesc.Height >= g_backBufferHeight * 0.9) {
                    g_backdropDropTime = GetTickCount64();
                    if (strcmp(g_currentLevel, "showroom") != 0) {
                        g_dimRequested = true;
                        g_dimStrongRequested = true;
                    } else if (g_sceneCompositeDrawn)
                        g_postBackdropToTopLayer = true; /* menu: what the game draws after its backdrop copy (button
                                                            hints of a popup) belongs above the dim */
                    DrawHudStencilOnly(device, base, draw);
                    return D3D_OK;
                }
            }
        }
        if (!ab0) {
            for (int t = 0; t < 8; t++) {
                IDirect3DBaseTexture9* boundTexture = g_boundTextures[t];
                if (!boundTexture || !g_renderTargetTextures.count((void*)boundTexture) ||
                    boundTexture->GetType() != D3DRTYPE_TEXTURE)
                    continue;
                D3DSURFACE_DESC surfaceDesc;
                if (FAILED(((IDirect3DTexture9*)boundTexture)->GetLevelDesc(0, &surfaceDesc)))
                    continue;
                if (surfaceDesc.Width >= g_backBufferWidth * 0.4 &&
                    fabs((double)surfaceDesc.Width / surfaceDesc.Height -
                         (double)g_backBufferWidth / g_backBufferHeight) <
                        0.1 * g_backBufferWidth / g_backBufferHeight) {
                    DrawHudStencilOnly(device, base, draw);
                    return D3D_OK;
                }
            }
        }
        bool inRace = strcmp(g_currentLevel, "showroom") != 0;
        if (g_cfg.dropBackdropCopies && (g_hudUsed || inRace) &&
            (!g_boundTextures[0] || (g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE &&
                                     !g_renderTargetTextures.count((void*)g_boundTextures[0])))) {
            // start of a race: the game draws its cinematic/count-in backdrop pieces (fills, blur picture, light
            // strips) as sizeable quads; end of race: a full-screen dim. Mid-race flourishes sit on an opaque black
            // mask quad (a tiny/no texture) that would hide the level: drop big untextured/solid-fill quads too.
            bool inWin = g_raceStartTime && GetTickCount64() - g_raceStartTime < 40000 &&
                         strcmp(g_currentLevel, "showroom") != 0;
            // the intro pieces (letterbox fills, blur picture, light strips) all carry a third texture; ordinary HUD
            // pieces (mirror frame, speedo...) do not, so they are never treated as intro pieces
            if (inWin && !g_boundTextures[2])
                inWin = false;
            bool known = false, cand = false;
            UINT cw = 0, ch = 0;
            if (!g_boundTextures[0])
                cand = inRace;
            else {
                D3DSURFACE_DESC candidateDesc;
                if (SUCCEEDED(((IDirect3DTexture9*)g_boundTextures[0])->GetLevelDesc(0, &candidateDesc))) {
                    cw = candidateDesc.Width;
                    ch = candidateDesc.Height;
                    // a restarted race has no level load, so nothing marks its start: its intro pieces (fill with a third
                    // texture bound) arrive long after the previous start
                    if (!inWin && g_raceStartTime && g_boundTextures[2] && inRace &&
                        GetTickCount64() - g_raceStartTime > 30000 &&
                        ((cw == 512 && ch == 256) || (cw == 16 && ch == 16) || (cw == 128 && ch == 128))) {
                        Log("race restart detected: intro window re-armed");
                        g_raceStartTime = GetTickCount64();
                        inWin = true;
                    }
                    known = (cw == 16 && ch == 16) || (cw == 512 && ch == 256) || (inWin && cw == 128 && ch == 128) ||
                            (!inRace && cw == 128 && ch == 64); // menu: a stretched small picture dims the screen behind a popup
                    cand = known || (inRace && cw <= 32 && ch <= 32);
                }
            }
            bool inCount = inWin && GetTickCount64() - g_raceStartTime < 20000;
            if (inCount && !g_cfg.countInDrop)
                cand = false; // the intro letterbox: draw its pieces into the HUD instead of hiding them
            double frac = 1.0;
            bool drop = false;
            if (cand) {
                drop = CoverageOfCandidate(
                    device, base, known ? (inCount ? 0.0 : (inWin ? 0.02 : (inRace && !g_hudUsed ? 0.15 : 0.6))) : 0.3,
                    &frac, draw, inWin || !inRace || (inRace && !g_hudUsed));
                if (known && inCount)
                    drop = true;
            } // count-in cinematic (CountInDrop=1): every fill/blur piece goes
            if (drop) {
                g_backdropDropTime = GetTickCount64();
                if (known && inWin)
                    g_lastIntroPieceTime = g_backdropDropTime;
                if (known && inRace && !inWin && frac >= 0.6 && g_hudUsed) {
                    g_lateUiToTopLayer = true;
                    g_topVeilRequested = true;
                    g_lateBackdropDrops++;
                    g_topVeilAlpha = g_lateBackdropDrops >= 3 ? g_cfg.veilRace : g_cfg.veilResultsScreen;
                } // 3+ covering fills = a popup on top of the results; 2 = just the results screen's own backdrop   //
                  // dim drawn over the UI built so far: what follows (menu panel) must stay above it
                else if (known && inRace && !inWin && frac >= 0.6) {
                    // (the race-start intro pieces no longer dim the view: the countdown shows on the plain scene)
                    g_dimRequested = true;
                    if (g_hudDrawsThisFrame >= 15)
                        g_dimStrongRequested = true;
                }
                if (known && !inRace && !g_sceneCompositeDrawn && frac >= 0.6) {
                    g_menuDialogToTopLayer = true;
                    g_topVeilRequested = true;
                    g_topVeilAlpha = g_cfg.veilMenu;
                } // menu dialog (connecting...) dims what is behind it: keep it above the UI
                DrawHudStencilOnly(device, base, draw);
                return D3D_OK;
            }
        }
        if (((g_menuDialogToTopLayer && !g_sceneCompositeDrawn && strcmp(g_currentLevel, "showroom") == 0) ||
             g_lateUiToTopLayer || g_postBackdropToTopLayer) &&
            g_topLayerSurface) {
            if (shadowUi)
                draw();
            std::swap(g_hudSurface, g_topLayerSurface);
            std::swap(g_hudUsed, g_topLayerUsed);
            HRESULT layerResult = DrawHudToTexture(device, base, draw);
            std::swap(g_hudSurface, g_topLayerSurface);
            std::swap(g_hudUsed, g_topLayerUsed);
            return layerResult;
        }
        g_hudDrawsThisFrame++;
        if (shadowUi)
            draw();
        hr = DrawHudToTexture(device, base, draw);
    } else {
        for (int i = 0; i < 2; i++) {
            D3DVIEWPORT9 v = ComputeHudViewport(base, i);
            setViewport(device, &v);
            hr = draw();
        }
        setViewport(device, &base);
    }
    return hr;
}
