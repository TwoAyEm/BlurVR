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

// The planar reflection pass is left out in races ([VR] Reflections=0): at the game's own switch where that is found (ApplyReflectionSwitch), else at the draw call
// (SkipReflectionDraw).
// The game's own switch for its planar reflection pass is a tweakable (Amax.Views.ReflectionEnable, a byte).  The reflection function (0x4297F0 in the Steam build) starts with
// `sub esp,0x208 / cmp byte [flag],0 / push ebx / push ebp / push esi / push edi / mov ebp,ecx / je ...` and returns at once when the byte is 0, so before any culling, state
// setting or drawing: with it the pass is left out at the source and the game's own work for it goes too, which leaving out the draw calls does not (the game still prepares each
// draw).  The function is found by those bytes, once, when the switch is first used (in a race, so the game's code is long unpacked).
static volatile BYTE* g_reflectionFlag = NULL;
static BYTE g_reflectionFlagGame = 1; // what the byte was before the first switch
static bool g_reflectionFlagSearched = false;
static unsigned g_reflectionFlagWrites = 0;
static volatile BYTE* FindReflectionEnableFlag()
{
    static const BYTE pattern[] = {0x81, 0xEC, 0x08, 0x02, 0x00, 0x00, 0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xE9, 0x0F, 0x84};
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return NULL;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    unsigned matches = 0;
    const BYTE* found = NULL;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[s].Name, ".bind", 8))
            continue;
        const BYTE* code = base + sections[s].VirtualAddress;
        DWORD size = sections[s].Misc.VirtualSize ? sections[s].Misc.VirtualSize : sections[s].SizeOfRawData;
        for (DWORD i = 0; i + sizeof(pattern) <= size; i++) {
            if (code[i] != 0x81 || code[i + 1] != 0xEC)
                continue;
            bool same = true;
            for (size_t k = 0; k < sizeof(pattern) && same; k++)
                if (!(k >= 8 && k < 12) && code[i + k] != pattern[k])
                    same = false;
            if (same) {
                matches++;
                found = code + i;
            }
        }
    }
    if (matches != 1) {
        Log("reflections: the game's reflection function was found %u times; the pass is left out at the draw call instead", matches);
        return NULL;
    }
    BYTE* flag = *(BYTE**)(found + 8);
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(flag, &info, sizeof(info)) || info.State != MEM_COMMIT || !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || *flag > 1) {
        Log("reflections: the byte the game's reflection function tests (%p) is not a writable flag; the pass is left out at the draw call instead", flag);
        return NULL;
    }
    Log("reflections: the game's own switch (Amax.Views.ReflectionEnable) is the byte at %p, now %u (the function at %p)", flag, (unsigned)*flag, found);
    return flag;
}
// The setting is for races: the showroom is a 3D scene too (IsRaceMode()), but its floor keeps the reflection.
static bool ReflectionsLeftOut()
{
    return g_cfg.skipReflections && IsRaceMode() && !InMenuScene();
}
// Once a frame: the byte is 0 while the setting leaves the reflections out and a race is on, and what the game had made it otherwise.
// It is written every frame it differs, so a level that sets the tweakable again does not bring the pass back.
void ApplyReflectionSwitch()
{
    const bool off = ReflectionsLeftOut();
    if (!g_reflectionFlagSearched && !off)
        return; // (the game's byte is not touched until a race needs it off)
    if (!g_reflectionFlagSearched) {
        g_reflectionFlagSearched = true;
        g_reflectionFlag = FindReflectionEnableFlag();
        if (g_reflectionFlag)
            g_reflectionFlagGame = *g_reflectionFlag;
    }
    if (!g_reflectionFlag)
        return;
    const BYTE wanted = off ? 0 : g_reflectionFlagGame;
    if (*g_reflectionFlag != wanted) {
        *g_reflectionFlag = wanted;
        if (g_reflectionFlagWrites++ < 20)
            Log("reflections: the game's reflection pass is %s at the source (the byte is now %u)", off ? "OFF" : "back on", (unsigned)wanted);
    }
}
// The game's switch was not found (another build of the game): the pass's draw calls are left out instead, which saves much less (the game still prepares each draw).
static bool SkipReflectionDraw(int plan)
{
    if (!g_reflectionFlagSearched || g_reflectionFlag || plan != 1 || !IsReflectionPass() || !ReflectionsLeftOut())
        return false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("reflections: the pass's draw calls are left out (the game's own switch was not found)");
    }
    return true;
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
        const float* viewProjRegisters = NULL;
        for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
            if (tracked.reg + 4 > 256)
                continue;
            if (tracked.kind == TK_ViewProj) {
                if (TryRecoverProjection(&g_vsConstants[tracked.reg * 4])) {
                    ok = true;
                    viewProjRegisters = &g_vsConstants[tracked.reg * 4];
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
        if (ok && IsMainPass()) {
            NoteMainSizedCamera(viewProjRegisters);
            if (!IsMainSizedReflectionTarget()) // (the mirrored camera of a reflection drawn into a target as big as the picture is not the main one)
                NoteMainCameraAxes();
        }
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

// ---- a menu on top of a race (see draw_dispatch.h)
// Blur's pause and results menus dim the race with a UI-shader fill that covers most of the screen (the VR path drops those fills: the `known` pieces whose
// coverage is 0.6 and more).  Flat there is no scratch target to measure on, so the coverage of the real draw is measured where it is made: an occlusion
// query around the draw counts the pixels it wrote, and the answer is picked up a frame or two later without waiting.  A blended UI-shader draw onto the
// back buffer in a race that wrote at least 60 % of the screen is such a fill.  In VR the pause dim the HUD path has already found is used instead
// (g_menuDimLast): the draws are redirected there, so a count on the real target would see nothing.  The fill has to be there for 250 ms before the
// overlay counts as up (a screen flash is shorter) and gone for 150 ms before it is down again.
struct RaceQuery {
    IDirect3DQuery9* query = NULL;
    bool pending = false;
    unsigned long long issuedMs = 0;
    int vs = 0;
};
static RaceQuery g_raceQueries[48];
static bool g_raceQueriesBroken = false;
static unsigned long long g_raceFillLastMs = 0, g_raceFillSinceMs = 0;
static bool g_raceMenuUp = false;
bool RaceMenuOverlay()
{
    return g_raceMenuUp;
}
void ReleaseRaceMenuQueries()
{
    for (RaceQuery& entry : g_raceQueries) {
        if (entry.query)
            entry.query->Release();
        entry.query = NULL;
        entry.pending = false;
    }
}
// Is this draw one whose coverage is worth measuring?  Then the slot of a query that has been started around it.
static RaceQuery* BeginRaceMenuMeasure(IDirect3DDevice9* device)
{
    if (g_raceQueriesBroken || g_vr.active || !g_renderTarget0 || g_renderTarget0 != g_backBufferSurface || !g_levelFilesSeen ||
        !IsMainSizedTarget() || !g_currentVsInfo || !g_currentVsInfo->ui || !g_currentVsInfo->tracked.empty())
        return NULL;
    DWORD alphaBlend = 0;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
    if (!alphaBlend || strcmp(g_currentLevel, "showroom") == 0)
        return NULL;
    RaceQuery* slot = NULL;
    for (RaceQuery& entry : g_raceQueries)
        if (!entry.pending) {
            slot = &entry;
            break;
        }
    if (!slot)
        return NULL;
    if (!slot->query && FAILED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &slot->query))) {
        g_raceQueriesBroken = true;
        Log("race menu: no occlusion queries on this device, a menu in a race is not recognised");
        return NULL;
    }
    if (FAILED(slot->query->Issue(D3DISSUE_BEGIN)))
        return NULL;
    slot->pending = true;
    slot->issuedMs = GetTickCount64();
    slot->vs = g_currentVsInfo->id;
    return slot;
}
// Round a draw with the query: started in the constructor (when the draw is a candidate), ended when RunDraw is left.
class RaceMenuMeasure {
public:
    explicit RaceMenuMeasure(IDirect3DDevice9* device) : slot_(NULL)
    {
        if (g_cfg.enabled && g_cfg.keyRepeatMs > 0)
            slot_ = BeginRaceMenuMeasure(device);
    }
    ~RaceMenuMeasure()
    {
        if (slot_)
            slot_->query->Issue(D3DISSUE_END);
    }

private:
    RaceQuery* slot_;
};
void RaceMenuOverlayEndFrame()
{
    unsigned long long now = GetTickCount64();
    const double area = (double)g_backBufferWidth * (double)g_backBufferHeight;
    for (RaceQuery& entry : g_raceQueries) {
        if (!entry.pending)
            continue;
        DWORD pixels = 0;
        HRESULT hr = entry.query->GetData(&pixels, sizeof(pixels), 0);
        if (hr == S_FALSE) {
            if (now - entry.issuedMs > 1000) // never came back (a reset, a lost device)
                entry.pending = false;
            continue;
        }
        entry.pending = false;
        if (hr != S_OK || area <= 0.0)
            continue;
        double coverage = pixels / area;
        if (g_logEnabled && coverage >= 0.2) {
            // (Log=1) which UI draws cover a good part of the screen: if a pause menu is not recognised, this says how much of it its fill covers
            static std::unordered_set<int> probed;
            int key = entry.vs * 16 + (int)(coverage * 10.0 + 0.5);
            if (probed.size() < 40 && probed.insert(key).second)
                Log("race menu probe: a blended UI draw (vertex shader %d) wrote %.0f%% of the screen", entry.vs, coverage * 100.0);
        }
        if (coverage >= 0.6)
            g_raceFillLastMs = now;
    }
    if (g_vr.active && g_menuDimLast)
        g_raceFillLastMs = now;
    bool recent = g_raceFillLastMs && now - g_raceFillLastMs <= 150;
    if (recent) {
        if (!g_raceFillSinceMs)
            g_raceFillSinceMs = g_raceFillLastMs;
        // (counted from the first to the last sighting of the fill, not up to now: the 150 ms of grace must not make a short flash last)
        if (!g_raceMenuUp && g_raceFillLastMs - g_raceFillSinceMs >= 250) {
            g_raceMenuUp = true;
            Log("race menu: up (a UI fill covering the screen has been drawn for 250 ms; menu key repeat limited)");
        }
    } else {
        g_raceFillSinceMs = 0;
        if (g_raceMenuUp) {
            g_raceMenuUp = false;
            Log("race menu: gone");
        }
    }
}

HRESULT RunDraw(IDirect3DDevice9* device, const DrawCall& draw)
{
    PerfDrawScope perf(device);
    RaceMenuMeasure raceMenuMeasure(device);
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
    if (g_cfg.skipReflections && SkipReflectionDraw(plan))
        return D3D_OK;
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
