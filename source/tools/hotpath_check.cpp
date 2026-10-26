// Host-side check and benchmark of the per-draw path (RunDraw and everything it calls for stereo duplication).
//
// The mod is linked as it is (every src\*.cpp except dllmain.cpp) against a fake Direct3D device: the original device
// functions the mod calls through g_deviceOriginals are replaced by a mirror of the device state (vertex and pixel constants,
// scissor, viewport, textures), and shaders are made from synthetic constant tables so the real parsers fill the shader infos.
// A synthetic frame (shadow maps, reflection pass, main pass with shadow decals / light volumes / reflection lookups, deferred
// lights, post passes) is replayed through RunDraw, and every time the mod issues a draw the whole device state is hashed.
//
//   check  <frames>      replays frames of random geometry and prints a hash of the device state at every draw (the two
//                        eyes of one game draw are summed, so their order does not matter) and at the end of each frame.
//                        Two builds of the sources that must render identically print the same hash.
//   micro                prints the cost of the matrix building blocks and a few helpers
//   mirror               checks that a planar reflection drawn into a render target as big as the picture (the showroom's) is found from its camera and drawn with
//                        exactly what the half-size reflection pass of a race gets (render_state.cpp: NoteMainSizedCamera), and that the game's clip plane for
//                        the pass is carried to each eye (stereo_scene.cpp: ApplyEyeConstants)
//   bench  <ns per call> replays one frame many times and prints nanoseconds per draw for each kind of draw; every call to the
//                        device costs the given extra time (a Direct3D 9 call through the driver is around 80 ns)
//
// Build and run with tools\hotpath_check.py (python tools\hotpath_check.py --src <src folder> [--compare <other src folder>]).
#include <algorithm>
#include <utility>
#include <x86intrin.h>
#include "camera.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "draw_dispatch.h"
#include "game_camera.h"
#include "game_mode.h"
#include "render_state.h"
#include "shader_tracking.h"
#include "stereo_scene.h"
#include "vr_runtime.h"
#if __has_include("device_state.h")
#include "device_state.h"
#define HAVE_DEVICE_STATE 1
#endif

// ---- a mirror of the device state, standing in for the original device functions -------------------------------------------
static bool g_hashing = false;
static unsigned long long g_hash = 14695981039346656037ull;
static unsigned long long g_calls[8];
static unsigned long long g_drawCalls = 0;
static unsigned long long g_groupSum = 0; // digests of the draws one RunDraw call issued
static double g_callCostNs = 0;
static LARGE_INTEGER g_counterFrequency;

static float g_devVs[256 * 4], g_devPs[256 * 4];
static DWORD g_devScissorEnable = 0;
static RECT g_devScissor;
static D3DVIEWPORT9 g_devViewport;
static IDirect3DBaseTexture9* g_devTexture[16];

static void MixWords(unsigned long long& h, const void* data, size_t bytes)
{
    const unsigned char* p = (const unsigned char*)data;
    for (; bytes >= 8; bytes -= 8, p += 8) {
        unsigned long long w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; bytes; bytes--, p++)
        h = (h ^ *p) * 1099511628211ull;
}
static unsigned long long DeviceDigest()
{
    unsigned long long h = 0x9E3779B97F4A7C15ull;
    MixWords(h, g_devVs, sizeof(g_devVs));
    MixWords(h, g_devPs, sizeof(g_devPs));
    MixWords(h, &g_devScissorEnable, sizeof(g_devScissorEnable));
    MixWords(h, &g_devScissor, sizeof(g_devScissor));
    MixWords(h, &g_devViewport, sizeof(g_devViewport));
    MixWords(h, g_devTexture, sizeof(g_devTexture));
    return h;
}
static unsigned long long g_callCostCycles = 0;
static inline void CallCost()
{
    if (!g_callCostCycles)
        return;
    unsigned long long start = __rdtsc();
    while (__rdtsc() - start < g_callCostCycles) {
    }
}
static void CalibrateCallCost(double ns) // the time stamp counter against the performance counter, over 50 ms
{
    g_callCostNs = ns;
    g_callCostCycles = 0;
    if (ns <= 0)
        return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    unsigned long long c0 = __rdtsc();
    do
        QueryPerformanceCounter(&t1);
    while ((double)(t1.QuadPart - t0.QuadPart) / (double)g_counterFrequency.QuadPart < 0.05);
    unsigned long long c1 = __rdtsc();
    double cyclesPerNs = (double)(c1 - c0) / ((double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)g_counterFrequency.QuadPart);
    g_callCostCycles = (unsigned long long)(ns * cyclesPerNs);
}
static HRESULT STDMETHODCALLTYPE M_SetVertexShaderConstantF(IDirect3DDevice9*, UINT start, const float* data, UINT count)
{
    g_calls[0]++;
    CallCost();
    memcpy(&g_devVs[start * 4], data, count * 16);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE M_SetPixelShaderConstantF(IDirect3DDevice9*, UINT start, const float* data, UINT count)
{
    g_calls[1]++;
    CallCost();
    memcpy(&g_devPs[start * 4], data, count * 16);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE M_SetRenderState(IDirect3DDevice9*, D3DRENDERSTATETYPE state, DWORD value)
{
    g_calls[2]++;
    CallCost();
    if (state == D3DRS_SCISSORTESTENABLE)
        g_devScissorEnable = value;
    else if (g_hashing) { // any other render state would be new to this harness: let it show up in the hash
        MixWords(g_hash, &state, sizeof(state));
        MixWords(g_hash, &value, sizeof(value));
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE M_SetScissorRect(IDirect3DDevice9*, const RECT* rect)
{
    g_calls[3]++;
    CallCost();
    g_devScissor = *rect;
    return D3D_OK;
}
static float g_devClipPlane[4]; // clip plane 0 on the device (not in the digest: only the mirror mode sets one)
static unsigned g_clipPlaneCalls = 0;
static HRESULT STDMETHODCALLTYPE M_SetClipPlane(IDirect3DDevice9*, DWORD index, const float* plane)
{
    g_clipPlaneCalls++;
    if (index == 0)
        memcpy(g_devClipPlane, plane, sizeof(g_devClipPlane));
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE M_SetViewport(IDirect3DDevice9*, const D3DVIEWPORT9* viewport)
{
    g_calls[4]++;
    CallCost();
    g_devViewport = *viewport;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE M_SetTexture(IDirect3DDevice9*, DWORD stage, IDirect3DBaseTexture9* texture)
{
    g_calls[5]++;
    CallCost();
    g_devTexture[stage & 15] = texture;
    return D3D_OK;
}

static int g_playingKind = -1; // reported by Trap
// A device whose every method traps, so an unexpected call is reported instead of crashing somewhere later.
template <int N>
static HRESULT STDMETHODCALLTYPE Trap()
{
    fprintf(stderr, "unexpected call of IDirect3DDevice9 vtable slot %d while replaying draw kind %d\n", N, g_playingKind);
    exit(2);
    return E_FAIL;
}
template <int... Slots>
static void FillTraps(void** table, std::integer_sequence<int, Slots...>)
{
    ((table[Slots] = (void*)&Trap<Slots>), ...);
}
static unsigned long long g_queries = 0; // state queries: counted, not hashed (they change nothing)
static HRESULT STDMETHODCALLTYPE F_GetRenderState(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD* value)
{
    g_queries++;
    *value = 0;
    return D3D_OK;
}
static void* g_fakeVtable[128];
static void* g_fakeDevice = g_fakeVtable;

// ---- synthetic shaders (built from real constant tables, so the mod's own parsers run) -----------------------------------
struct CtabConstant {
    const char* name;
    WORD reg, count;
};
static std::vector<DWORD> MakeShader(DWORD version, const std::vector<CtabConstant>& constants)
{
    std::vector<BYTE> blob(28 + constants.size() * 20, 0);
    std::vector<DWORD> nameOffsets;
    for (const CtabConstant& c : constants) {
        nameOffsets.push_back((DWORD)blob.size());
        for (const char* p = c.name; *p; p++)
            blob.push_back((BYTE)*p);
        blob.push_back(0);
    }
    while (blob.size() % 4)
        blob.push_back(0);
    *(DWORD*)&blob[0] = 28;
    *(DWORD*)&blob[12] = (DWORD)constants.size();
    *(DWORD*)&blob[16] = 28;
    for (size_t i = 0; i < constants.size(); i++) {
        BYTE* entry = &blob[28 + i * 20];
        *(DWORD*)(entry + 0) = nameOffsets[i];
        *(WORD*)(entry + 4) = 2; // D3DXRS_FLOAT4
        *(WORD*)(entry + 6) = constants[i].reg;
        *(WORD*)(entry + 8) = constants[i].count;
    }
    std::vector<DWORD> tokens;
    tokens.push_back(version);
    tokens.push_back(((DWORD)(1 + blob.size() / 4) << 16) | 0xFFFE);
    tokens.push_back(0x42415443); // 'CTAB'
    for (size_t i = 0; i < blob.size(); i += 4)
        tokens.push_back(*(DWORD*)&blob[i]);
    tokens.push_back(0x0000FFFF);
    return tokens;
}
static int g_vsCount = 0;
static VertexShaderInfo* NewVs(const std::vector<CtabConstant>& constants)
{
    std::vector<DWORD> tokens = MakeShader(0xFFFE0300, constants);
    VertexShaderInfo* info = new VertexShaderInfo();
    ParseVertexShaderConstants(tokens.data(), info, g_vsCount++);
    return info;
}
static PixelShaderInfo* NewPs(const std::vector<CtabConstant>& constants)
{
    std::vector<DWORD> tokens = MakeShader(0xFFFF0300, constants);
    PixelShaderInfo* info = new PixelShaderInfo();
    info->id = g_pixelShaderCount++;
    ParsePixelShaderConstants(tokens.data(), info);
    return info;
}

// ---- random numbers and matrices ---------------------------------------------------------------------------------------
struct Rng {
    unsigned long long s;
    unsigned long long Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    double Uniform(double lo, double hi) { return lo + (hi - lo) * ((Next() >> 11) * (1.0 / 9007199254740992.0)); }
    int Below(int n) { return (int)(Next() % (unsigned long long)n); }
};
static Matrix4 Rigid(Rng& rng, double angle, double travel)
{
    double yaw = rng.Uniform(-angle, angle), pitch = rng.Uniform(-angle, angle), roll = rng.Uniform(-angle, angle);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    Matrix4 m = Identity();
    double r[3][3] = {{cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr},
                      {-cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr},
                      {sy * cp, -sp, cy * cp}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m.m[i][j] = r[i][j];
    for (int j = 0; j < 3; j++)
        m.m[3][j] = rng.Uniform(-travel, travel);
    return m;
}
static Matrix4 Projection(double a, double b, double c, double d)
{
    Matrix4 p = Identity();
    p.m[0][0] = a;
    p.m[1][1] = b;
    p.m[2][2] = c;
    p.m[2][3] = 1;
    p.m[3][2] = d;
    p.m[3][3] = 0;
    return p;
}

// ---- the synthetic frame -------------------------------------------------------------------------------------------------
enum Kind { K_SHADOWMAP, K_REFLECT, K_SCENE, K_SCENE_REFLECTIVE, K_SHADOW_DECAL, K_LIGHT_VOLUME, K_INTERLEAVED, K_LIGHTS, K_POST, K_MAIN_OTHER, K_MOTION_BLUR, K_COUNT };
static const char* const kKindNames[K_COUNT] = {"shadow map", "reflection pass", "scene", "scene (reflective)", "shadow decal",
                                                "light volume", "plain draw in scene", "deferred lights", "post (reduced target)", "main-sized other",
                                                "motion blur"};
static const int kKindPerFrame[K_COUNT] = {900, 230, 1500, 380, 80, 30, 60, 45, 70, 8, 1};

struct DrawRec {
    int kind, vs, ps;
    float vsConst[16 * 4]; // registers 0..15
    float lightCorners[16];
    float psConst[64 * 4]; // registers 0..63 (bound to the same registers the shaders use)
};
static std::vector<VertexShaderInfo*> g_vsPool[K_COUNT];
static std::vector<PixelShaderInfo*> g_psPool[K_COUNT];
static int kLayout = 1;   // memory layout of the matrices the game uploads (1 rows, 2 transposed)
static int g_variant = 0; // see ApplyVariant

static void BuildShaders()
{
    auto vs = [](int kind, std::vector<CtabConstant> c) { g_vsPool[kind].push_back(NewVs(c)); };
    auto ps = [](int kind, std::vector<CtabConstant> c) { g_psPool[kind].push_back(NewPs(c)); };
    VertexShaderInfo* fullScreen = NewVs({}); // id 0: the untracked full-screen vertex shader the post passes use
    for (int kind : {K_SHADOWMAP, K_REFLECT, K_SCENE}) {
        vs(kind, {{"g_mWorldViewProj", 0, 4}});
        vs(kind, {{"g_mWorldViewProj", 0, 4}, {"g_mWorldView", 4, 4}});
        vs(kind, {{"g_mViewProj", 0, 4}, {"g_vTint", 8, 1}});
        vs(kind, {{"g_mWorldViewProj", 0, 4}, {"g_mView", 8, 3}});
        vs(kind, {{"g_vViewMatrix0", 4, 1}, {"g_vViewMatrix1", 5, 1}, {"g_vViewMatrix2", 6, 1}, {"g_vViewMatrix3", 7, 1},
                  {"g_vViewProjMatrix0", 0, 1}, {"g_vViewProjMatrix1", 1, 1}, {"g_vViewProjMatrix2", 2, 1},
                  {"g_vViewProjMatrix3", 3, 1}});
    }
    vs(K_SCENE_REFLECTIVE, {{"g_mViewProj", 0, 4}, {"g_ReflectionViewMatrix0", 8, 1}, {"g_ReflectionViewMatrix1", 9, 1},
                            {"g_ReflectionViewMatrix3", 10, 1}});
    vs(K_SCENE_REFLECTIVE, {{"g_mWorldViewProj", 0, 4}, {"g_mViewProj", 4, 4}, {"g_ReflectionViewMatrix0", 8, 1},
                            {"g_ReflectionViewMatrix1", 9, 1}, {"g_ReflectionViewMatrix3", 10, 1}});
    vs(K_SHADOW_DECAL, {{"g_mWorldViewProj", 0, 4}});
    vs(K_LIGHT_VOLUME, {{"g_mWorldViewProj", 0, 4}});
    vs(K_LIGHTS, {{"g_vFar0", 8, 1}, {"g_vFar1", 9, 1}, {"g_vFar2", 10, 1}, {"g_vFar3", 11, 1}, {"g_vUVScale", 12, 1}});
    for (int kind : {K_INTERLEAVED, K_POST, K_MAIN_OTHER, K_MOTION_BLUR})
        g_vsPool[kind].push_back(fullScreen);
    for (int kind : {K_SHADOWMAP, K_REFLECT, K_SCENE, K_SCENE_REFLECTIVE, K_INTERLEAVED, K_POST, K_MAIN_OTHER})
        ps(kind, {{"g_tColour", 0, 1}});
    ps(K_SHADOW_DECAL, {{"g_ShadowPlaneSpaceMatrix", 0, 4}, {"g_ShadowMult", 4, 1}, {"g_vDepth", 5, 1}});
    ps(K_LIGHT_VOLUME, {{"g_vXY", 3, 1}, {"g_mLight", 4, 4}, {"g_vPosition", 8, 1}, {"g_vColour", 9, 1}});
    ps(K_LIGHTS, {{"g_vPosition", 21, 4}, {"g_vViewUpVector", 22, 1}, {"g_vColour", 28, 1}});
    ps(K_MOTION_BLUR, {{"g_mMatrix", 0, 4}, {"g_vStepScale", 4, 1}});
    // the reflection pass and the shadow map use the same pixel shader pool as the scene
}

struct FrameData {
    std::vector<DrawRec> draws;
};
static void GenerateFrame(Rng& rng, FrameData& frame, double a, double b, double c, double d)
{
    frame.draws.clear();
    Matrix4 view = Rigid(rng, 1.2, 300.0), proj = Projection(a, b, c, d);
    Matrix4 viewProj = Multiply(view, proj);
    Matrix4 reflectionView = Rigid(rng, 1.2, 300.0);
    Matrix4 reflectionViewProj = Multiply(reflectionView, proj);
    auto store = [](const Matrix4& m, float* out) { StoreMatrix(m, kLayout, out); };
    for (int kind = 0; kind < K_COUNT; kind++)
        for (int n = 0; n < kKindPerFrame[kind]; n++) {
            DrawRec rec;
            memset(&rec, 0, sizeof(rec));
            rec.kind = kind;
            rec.vs = rng.Below((int)g_vsPool[kind].size());
            rec.ps = rng.Below((int)g_psPool[kind].size());
            const VertexShaderInfo* info = g_vsPool[kind][rec.vs];
            Matrix4 world = Rigid(rng, 3.0, 40.0);
            for (const TrackedConstant& tracked : info->tracked) {
                Matrix4 m;
                bool reflectionPass = kind == K_REFLECT;
                const Matrix4& vp = reflectionPass ? reflectionViewProj : viewProj;
                const Matrix4& v = reflectionPass ? reflectionView : view;
                switch (tracked.kind) {
                    case TK_WorldViewProj: m = Multiply(world, vp); break;
                    case TK_ViewProj: m = vp; break;
                    case TK_WorldView: m = Multiply(world, v); break;
                    case TK_View: m = v; break;
                    default: m = v; break;
                }
                if (tracked.kind == TK_View3x3) {
                    for (int r = 0; r < 3; r++)
                        for (int cc = 0; cc < 4; cc++)
                            rec.vsConst[tracked.reg * 4 + r * 4 + cc] = (cc < 3) ? (float)v.m[r][cc] : 0.f;
                } else
                    store(m, &rec.vsConst[tracked.reg * 4]);
            }
            for (const ShaderConstant& k : info->constants) { // untracked constants: a few plausible values
                if (k.reg >= 16)
                    continue;
                bool tracked = false;
                for (const TrackedConstant& t : info->tracked)
                    if (k.reg >= t.reg && k.reg < t.reg + 4)
                        tracked = true;
                if (!tracked && k.count == 1)
                    for (int i = 0; i < 4; i++)
                        rec.vsConst[k.reg * 4 + i] = (float)rng.Uniform(-2, 2);
            }
            if (kind == K_SCENE_REFLECTIVE) { // x, y, w columns of the reflection pass's view-projection
                float cols[3][4];
                for (int i = 0; i < 4; i++) {
                    cols[0][i] = (float)reflectionViewProj.m[i][0];
                    cols[1][i] = (float)reflectionViewProj.m[i][1];
                    cols[2][i] = (float)reflectionViewProj.m[i][3];
                }
                memcpy(&rec.vsConst[8 * 4], cols[0], 16);
                memcpy(&rec.vsConst[9 * 4], cols[1], 16);
                memcpy(&rec.vsConst[10 * 4], cols[2], 16);
            }
            if (kind == K_LIGHTS) { // far corners: x, y, z (depth), w
                for (int i = 0; i < 4; i++) {
                    rec.vsConst[(8 + i) * 4 + 0] = (i & 1) ? 1.4f : -1.4f;
                    rec.vsConst[(8 + i) * 4 + 1] = (i & 2) ? 0.8f : -0.8f;
                    rec.vsConst[(8 + i) * 4 + 2] = 1.0f;
                    rec.vsConst[(8 + i) * 4 + 3] = 0.f;
                }
            }
            for (int i = 0; i < 64 * 4; i++)
                rec.psConst[i] = (float)rng.Uniform(-3, 3);
            frame.draws.push_back(rec);
        }
    // the game draws in passes: shadow maps first, then reflection, scene, lights, post; the scene kinds are mixed
    std::stable_sort(frame.draws.begin(), frame.draws.end(), [](const DrawRec& l, const DrawRec& r) {
        static const int order[K_COUNT] = {0, 1, 2, 2, 2, 2, 2, 3, 4, 4, 4};
        return order[l.kind] < order[r.kind];
    });
    size_t sceneBegin = 0, sceneEnd = 0;
    for (size_t i = 0; i < frame.draws.size(); i++) {
        if (frame.draws[i].kind == K_SCENE && !sceneBegin)
            sceneBegin = i;
        if (frame.draws[i].kind >= K_SCENE && frame.draws[i].kind <= K_INTERLEAVED)
            sceneEnd = i + 1;
    }
    for (size_t i = sceneEnd; i > sceneBegin + 1; i--)
        std::swap(frame.draws[i - 1], frame.draws[sceneBegin + (size_t)rng.Below((int)(i - sceneBegin))]);
}

// ---- state for each kind of draw -----------------------------------------------------------------------------------------
static void FlushDeviceState()
{
#ifdef HAVE_DEVICE_STATE
    DeviceRestoreAll((IDirect3DDevice9*)&g_fakeDevice);
#endif
}
static void SetKindState(int kind)
{
    FlushDeviceState(); // the game changes the render target: the mod puts the game's state back first (H_SetRenderTarget)
    static IDirect3DSurface9* const kOffscreen = (IDirect3DSurface9*)0x1000;
    g_renderTarget0 = kOffscreen;
    g_hasDepthBuffer = true;
    UINT w = 2160, h = 1071;
    switch (kind) {
        case K_SHADOWMAP: w = h = 1024; break;
        case K_REFLECT: w = 1080, h = 535; break;
        case K_POST: w = 540, h = 267; break;
        default: break;
    }
    g_rtWidth = w;
    g_rtHeight = h;
    g_gameViewport.X = g_gameViewport.Y = 0;
    g_gameViewport.Width = w;
    g_gameViewport.Height = h;
    g_gameViewport.MinZ = 0;
    g_gameViewport.MaxZ = 1;
    g_gameScissorRect.left = g_gameScissorRect.top = 0;
    g_gameScissorRect.right = w;
    g_gameScissorRect.bottom = h;
    // the device resets viewport and scissor rectangle with the new render target ...
    g_devViewport = g_gameViewport;
    g_devScissor = g_gameScissorRect;
    // ... and then the game sets its scissor state (off, or on with a rectangle in variant 7) through the hooked device
    g_gameScissorEnabled = (g_variant == 7) ? TRUE : FALSE;
    g_devScissorEnable = g_gameScissorEnabled ? 1 : 0;
#ifdef HAVE_DEVICE_STATE
    DeviceNoteGameScissorRect(g_gameScissorRect);
    DeviceNoteGameScissorEnable(g_gameScissorEnabled != FALSE);
#endif
    if (g_variant == 7) {
        g_gameScissorRect = {100, 50, (LONG)w - 100, (LONG)h - 50};
        g_devScissor = g_gameScissorRect;
#ifdef HAVE_DEVICE_STATE
        DeviceNoteGameScissorRect(g_gameScissorRect);
#endif
    }
}

static void SetEyes(Rng& rng)
{
    for (int e = 0; e < 2; e++) {
        Matrix4 head = Rigid(rng, 0.6, 0.15);
        head.m[3][0] += (e == 0) ? 0.032 : -0.032;
        g_vr.eyeCameraDelta[e] = head;
    }
    g_vr.eyeCameraDelta[2] = Rigid(rng, 0.6, 0.15);
}

// 0 default, 1 transposed matrix layout, 2 mono, 3 seam gutter, 4 reflection flip mode 2, 5 no reflection lookup fix,
// 6 no view rotation adjustment, 7 the game's scissor test is on
static const int kVariants = 8;
static void ApplyVariant(int variant)
{
    g_variant = variant;
    switch (variant) {
        case 1: kLayout = 2; break;
        case 2: g_cfg.stereo = 0; break;
        case 3: g_cfg.seamGutter = 24; break;
        case 4: g_cfg.reflectionFlipMode = 2; break;
        case 5: g_cfg.fixReflectionLookup = 0; break;
        case 6: g_cfg.adjustViewRot = 0; break;
        default: break;
    }
}
static HRESULT STDMETHODCALLTYPE M_Clear(IDirect3DDevice9*, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                                         DWORD stencil)
{
    g_calls[6]++;
    if (g_hashing) {
        MixWords(g_hash, &count, sizeof(count));
        if (rects)
            MixWords(g_hash, rects, count * sizeof(D3DRECT));
        MixWords(g_hash, &flags, sizeof(flags));
        MixWords(g_hash, &color, sizeof(color));
        MixWords(g_hash, &g_devScissorEnable, sizeof(g_devScissorEnable)); // a clear is cut by the scissor test when it is on
        MixWords(g_hash, &g_devScissor, sizeof(g_devScissor));
    }
    return D3D_OK;
}
static void InitWorld()
{
    memset(g_calls, 0, sizeof(g_calls));
    g_cfg.log = 0;
    g_logEnabled = false;
    g_vr.active = true;
    g_vr.poseValid = true;
    g_vr.eyeBox[0] = {-1.544, 1.047, 1.405, 1.411};
    g_vr.eyeBox[1] = {-1.052, 1.538, 1.404, 1.415};
    g_vr.unionBox = {-1.544, 1.538, 1.405, 1.415};
    g_modeOverride = 2; // race
    g_levelLoading = false;
    g_vrStartDeferred = false;
    g_backBufferWidth = 2160;
    g_backBufferHeight = 1071;
    g_backBufferSurface = (IDirect3DSurface9*)0x2000;
    g_matrixLayout = 0; // detected from the first view-projection
    strcpy(g_currentLevel, "track");
    FillTraps(g_fakeVtable, std::make_integer_sequence<int, 128>());
    g_fakeVtable[58] = (void*)F_GetRenderState;
    g_fakeVtable[43] = (void*)M_Clear; // device->Clear (the game's hooked entry point)
    g_deviceOriginals[VT_Clear] = (void*)M_Clear;
    g_deviceOriginals[VT_SetVertexShaderConstantF] = (void*)M_SetVertexShaderConstantF;
    g_deviceOriginals[VT_SetPixelShaderConstantF] = (void*)M_SetPixelShaderConstantF;
    g_deviceOriginals[VT_SetRenderState] = (void*)M_SetRenderState;
    g_deviceOriginals[VT_SetScissorRect] = (void*)M_SetScissorRect;
    g_deviceOriginals[VT_SetClipPlane] = (void*)M_SetClipPlane;
    g_deviceOriginals[VT_SetViewport] = (void*)M_SetViewport;
    g_deviceOriginals[VT_SetTexture] = (void*)M_SetTexture;
    BuildShaders();
    { // the game has drawn its scene before any of this happens: the projection is known
        float registers[16];
        StoreMatrix(Projection(0.8179, 1.6496, -0.00003, 0.11667), kLayout, registers);
        TryRecoverProjection(registers);
    }
}

// The game uploads vertex constants through the hooked device: the hook keeps its copy, tells the mod and calls the device.
static void GameSetVs(UINT start, UINT count, const float* data)
{
    memcpy(&g_vsConstants[start * 4], data, count * 16);
    memcpy(&g_devVs[start * 4], data, count * 16);
#ifdef HAVE_DEVICE_STATE
    DeviceNoteGameVsConstants(start, count);
#endif
}

// Replay a draw: put the game's state in place, call RunDraw.
static inline void PlayDraw(const DrawRec& rec, int& lastKind)
{
    g_playingKind = rec.kind;
    int targetClass = rec.kind == K_SHADOWMAP ? 1 : rec.kind == K_REFLECT ? 2 : rec.kind == K_POST ? 3 : 0;
    if (targetClass != lastKind) { // the game sets a render target only when the pass changes
        SetKindState(rec.kind);
        lastKind = targetClass;
    }
    g_currentVsInfo = g_vsPool[rec.kind][rec.vs];
    g_currentPsInfo = g_psPool[rec.kind][rec.ps];
    // the game uploads the registers its shader reads whose value changed, a run at a time; the other registers keep what an
    // earlier draw left in them
    bool uploads[16];
    for (UINT r = 0; r < 16; r++)
        uploads[r] = false;
    for (const TrackedConstant& tracked : g_currentVsInfo->tracked)
        for (UINT k = 0; k < (tracked.kind == TK_View3x3 ? 3u : 4u) && tracked.reg + k < 16; k++)
            uploads[tracked.reg + k] = true;
    for (const ShaderConstant& constant : g_currentVsInfo->constants)
        for (UINT k = 0; k < constant.count && constant.reg + k < 16; k++)
            uploads[constant.reg + k] = true;
    static unsigned playCount = 0;
    bool reupload = (++playCount % 5) == 0; // now and then the game uploads a constant again with the same value
    for (UINT r = 0; r < 16; r++)
        uploads[r] = uploads[r] && (reupload || memcmp(&g_vsConstants[r * 4], &rec.vsConst[r * 4], 16) != 0);
    for (UINT r = 0; r < 16;) {
        if (!uploads[r]) {
            r++;
            continue;
        }
        UINT end = r;
        while (end < 16 && uploads[end])
            end++;
        GameSetVs(r, end - r, &rec.vsConst[r * 4]);
        r = end;
    }
    memcpy(g_psConstants, rec.psConst, sizeof(rec.psConst));
    memcpy(g_devPs, rec.psConst, sizeof(rec.psConst));
    RunDraw((IDirect3DDevice9*)&g_fakeDevice, DrawCall([&]() {
                g_drawCalls++;
                if (g_hashing)
                    g_groupSum += DeviceDigest();
                return D3D_OK;
            }));
    if (g_hashing) {
        MixWords(g_hash, &g_groupSum, sizeof(g_groupSum));
        g_groupSum = 0;
    }
}

// ---- a reflection pass in a target as big as the picture -----------------------------------------------------------------
struct TestCamera {
    double pos[3], right[3], up[3], fwd[3];
};
static TestCamera MakeCamera(double yaw, double pitch, double x, double y, double z)
{
    TestCamera c;
    const double r[3] = {cos(yaw), 0, -sin(yaw)}, f[3] = {sin(yaw) * cos(pitch), -sin(pitch), cos(yaw) * cos(pitch)};
    const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    for (int i = 0; i < 3; i++) {
        c.right[i] = r[i];
        c.up[i] = u[i];
        c.fwd[i] = f[i];
    }
    c.pos[0] = x;
    c.pos[1] = y;
    c.pos[2] = z;
    return c;
}
// The game's planar reflection camera: position straight under the camera, the axes mirrored in the horizontal plane, and the up axis turned round to keep a proper rotation.
// `turned` turns the other two round as well (the rear-view camera does that).
static TestCamera MirrorOf(const TestCamera& c, double depth, bool turned)
{
    TestCamera m = c;
    m.pos[1] -= depth;
    for (int i = 0; i < 3; i++) {
        const double sign = i == 1 ? -1.0 : 1.0;
        m.right[i] = sign * c.right[i];
        m.up[i] = -sign * c.up[i];
        m.fwd[i] = sign * c.fwd[i];
        if (turned) {
            m.right[i] = -m.right[i];
            m.fwd[i] = -m.fwd[i];
        }
    }
    return m;
}
static Matrix4 ViewOf(const TestCamera& c)
{
    Matrix4 v = Identity();
    for (int i = 0; i < 3; i++) {
        v.m[i][0] = c.right[i];
        v.m[i][1] = c.up[i];
        v.m[i][2] = c.fwd[i];
    }
    const double* axes[3] = {c.right, c.up, c.fwd};
    for (int j = 0; j < 3; j++)
        v.m[3][j] = -(c.pos[0] * axes[j][0] + c.pos[1] * axes[j][1] + c.pos[2] * axes[j][2]);
    return v;
}
static int MirrorCheck(Rng& rng)
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("  %s: %s\n", ok ? "ok" : "FAILED", what);
        if (!ok)
            failures++;
    };
    const Matrix4 projection = Projection(0.8179, 1.6496, -0.00003, 0.11667);
    IDirect3DSurface9* const kMain = (IDirect3DSurface9*)0x3000;
    IDirect3DSurface9* const kReflection = (IDirect3DSurface9*)0x3100;
    IDirect3DSurface9* const kHalf = (IDirect3DSurface9*)0x3200;
    auto setTarget = [&](IDirect3DSurface9* surface, UINT width, UINT height) {
        g_renderTarget0 = surface;
        g_rtWidth = width;
        g_rtHeight = height;
        g_hasDepthBuffer = true;
    };
    auto publish = [&](const TestCamera& c, unsigned long long frame) {
        MainCameraPose pose = {};
        for (int i = 0; i < 3; i++) {
            pose.pos[i] = c.pos[i];
            pose.right[i] = c.right[i];
            pose.up[i] = c.up[i];
            pose.fwd[i] = c.fwd[i];
        }
        pose.projX = 0.8179;
        pose.projY = 1.6496;
        pose.frame = frame;
        PublishMainCameraPose(pose);
    };
    // a draw of the main pass through `camera` (what ClassifyDraw does for a draw with a recoverable view-projection)
    auto draw = [&](const TestCamera& camera, IDirect3DSurface9* target, unsigned long long frame) {
        float registers[16];
        StoreMatrix(Multiply(ViewOf(camera), projection), kLayout, registers);
        setTarget(target, 2160, 1071);
        g_vr.frameCount = frame;
        if (TryRecoverProjection(registers))
            NoteMainSizedCamera(registers);
    };
    const TestCamera head = MakeCamera(0.5, 0.07, 120.0, 0.9, 40.0);
    const TestCamera mirror = MirrorOf(head, 1.8, false);
    printf("finding the mirrored camera:\n");
    ResetMainSizedReflectionTarget();
    publish(head, 100);
    draw(head, kMain, 100);
    check(!IsMainSizedReflectionTarget(), "the main camera's own draws do not mark their target");
    draw(mirror, kReflection, 100);
    check(IsMainSizedReflectionTarget() && MainSizedReflectionAxis() == 1 && IsReflectionPass(), "the draws of the camera 1.8 m under it with the up axis turned round mark theirs (axis Y)");
    setTarget(kMain, 2160, 1071);
    check(!IsMainSizedReflectionTarget() && MainSizedReflectionAxis() < 0 && !IsReflectionPass(), "the main target is still the main pass");
    setTarget(kHalf, 1080, 535);
    check(IsReflectionPass() && MainSizedReflectionAxis() < 0, "the half-size pass is the reflection pass by its size, as before");
    publish(head, 101);
    draw(head, kReflection, 101);
    check(!IsMainSizedReflectionTarget(), "a main-camera draw into that target takes the mark off again");
    publish(head, 102);
    draw(mirror, kReflection, 102);
    check(IsMainSizedReflectionTarget(), "and the mirrored camera puts it back");
    ResetMainSizedReflectionTarget();
    check(!IsMainSizedReflectionTarget(), "a Reset forgets the surface");
    publish(head, 103);
    draw(MirrorOf(head, 0.1, false), kReflection, 103);
    check(!IsMainSizedReflectionTarget(), "a mirror only 0.1 m under the main camera is not taken for the reflection");
    publish(head, 104);
    draw(MirrorOf(head, 1.8, true), kReflection, 104);
    check(!IsMainSizedReflectionTarget(), "a camera under it with all three axes turned round (the rear-view camera) is refused");
    publish(head, 50);
    draw(mirror, kReflection, 105);
    check(!IsMainSizedReflectionTarget(), "a main camera pose that is more than three frames old is not trusted");
    publish(head, 106);
    draw(mirror, kReflection, 106);
    check(IsMainSizedReflectionTarget(), "a fresh one is");
    ResetMainSizedReflectionTarget();
    {
        TestCamera aside = mirror; // (the camera position is recovered from the view-projection: this one is not under the main camera)
        aside.pos[0] += 40;
        publish(head, 107);
        draw(aside, kReflection, 107);
        check(!IsMainSizedReflectionTarget(), "a mirrored camera 40 m to the side is refused");
    }

    printf("drawing it like the half-size pass:\n");
    ResetMainSizedReflectionTarget();
    publish(head, 200);
    draw(mirror, kReflection, 200); // marks the target
    // the same draw (a scene draw through the mirrored camera) in three situations; ApplyEyeConstants must write the same constants in the first two
    const VertexShaderInfo* sceneShaders[2] = {g_vsPool[K_SCENE][0], g_vsPool[K_SCENE][1]};
    int differentFromHalf = 0, identical = 0;
    for (int trial = 0; trial < 40; trial++) {
        SetEyes(rng);
        Matrix4 world = Rigid(rng, 3.0, 40.0);
        Matrix4 view = ViewOf(mirror);
        const Matrix4 viewProj = Multiply(view, projection);
        float registers[8 * 4];
        StoreMatrix(Multiply(world, viewProj), kLayout, &registers[0]);
        StoreMatrix(Multiply(world, view), kLayout, &registers[16]);
        std::vector<float> results[3];
        for (int situation = 0; situation < 3; situation++) {
            FlushDeviceState();
            switch (situation) {
                case 0: setTarget(kHalf, 1080, 535); g_cfg.reflectionFlipMode = 2; break; // the race's pass, told the axis
                case 1: setTarget(kReflection, 2160, 1071); g_cfg.reflectionFlipMode = -1; break; // the marked target, the axis from its draws
                case 2: setTarget(kMain, 2160, 1071); g_cfg.reflectionFlipMode = -1; break;       // a main-sized target that is not marked
            }
            g_currentVsInfo = (VertexShaderInfo*)sceneShaders[trial & 1];
            GameSetVs(0, 8, registers);
            for (int eye = 0; eye < 2; eye++) {
                ApplyEyeConstants((IDirect3DDevice9*)&g_fakeDevice, eye);
                results[situation].insert(results[situation].end(), g_devVs, g_devVs + 8 * 4);
                FlushDeviceState();
                GameSetVs(0, 8, registers);
            }
        }
        if (results[0] == results[1])
            identical++;
        if (results[0] != results[2])
            differentFromHalf++;
    }
    g_cfg.reflectionFlipMode = -1;
    check(identical == 40, "a marked main-sized target gets the constants of the half-size pass (40 of 40 draws)");
    check(differentFromHalf == 40, "an unmarked one does not (it is drawn through the head's own delta: 40 of 40 differ)");

    printf("a draw into that target, in the showroom and in a race (once for each eye, with the eyes' own constants):\n");
    {
        const int savedMode = g_modeOverride;
        const bool savedSeen = g_levelFilesSeen;
        char savedLevel[64];
        strcpy(savedLevel, g_currentLevel);
        unsigned showroomTwice = 0, showroomRewritten = 0, raceTwice = 0, raceRewritten = 0;
        for (int trial = 0; trial < 20; trial++) {
            SetEyes(rng);
            Matrix4 world = Rigid(rng, 3.0, 40.0);
            Matrix4 view = ViewOf(mirror);
            float registers[8 * 4];
            StoreMatrix(Multiply(world, Multiply(view, projection)), kLayout, &registers[0]);
            StoreMatrix(Multiply(world, view), kLayout, &registers[16]);
            for (int where = 0; where < 2; where++) {
                if (where == 0) { // the showroom, as the game's file opens tell it
                    g_modeOverride = 0;
                    g_levelFilesSeen = true;
                    strcpy(g_currentLevel, "showroom");
                } else {
                    g_modeOverride = 2;
                    strcpy(g_currentLevel, "track");
                }
                FlushDeviceState();
                const unsigned long long frame = 400 + 2 * trial + where;
                publish(head, frame);
                ResetMainSizedReflectionTarget();
                draw(mirror, kReflection, frame); // (a draw with a view-projection comes first in a frame: it marks the target)
                setTarget(kReflection, 2160, 1071);
                g_currentVsInfo = (VertexShaderInfo*)sceneShaders[trial & 1];
                g_currentPsInfo = g_psPool[K_SCENE][0];
                g_vr.frameCount = frame;
                GameSetVs(0, 8, registers);
                std::vector<float> seen;
                const unsigned long long before = g_drawCalls;
                RunDraw((IDirect3DDevice9*)&g_fakeDevice, DrawCall([&]() {
                            g_drawCalls++;
                            seen.insert(seen.end(), g_devVs, g_devVs + 8 * 4);
                            return D3D_OK;
                        }));
                const unsigned long long draws = g_drawCalls - before;
                const bool game = seen.size() >= 32 && memcmp(seen.data(), registers, sizeof(registers)) == 0;
                const bool rewritten = draws == 2 && !game && seen.size() == 64 && memcmp(seen.data(), seen.data() + 32, 32 * sizeof(float)) != 0;
                if (where == 0) {
                    showroomTwice += draws == 2;
                    showroomRewritten += rewritten;
                } else {
                    raceTwice += draws == 2;
                    raceRewritten += rewritten;
                }
            }
        }
        g_modeOverride = savedMode;
        g_levelFilesSeen = savedSeen;
        strcpy(g_currentLevel, savedLevel);
        printf("  (showroom: twice %u, rewritten %u; race: twice %u, rewritten %u)\n", showroomTwice, showroomRewritten, raceTwice, raceRewritten);
        check(showroomTwice == 20 && showroomRewritten == 20, "in the showroom it is drawn for each eye, with the eyes' own constants (20 of 20 draws)");
        check(raceTwice == 20 && raceRewritten == 20, "in a race it is drawn for each eye, with the eyes' own constants (20 of 20 draws)");
    }

    printf("the game's clip plane for the pass, carried to each eye:\n");
    {
        // The game clips the reflection scene with a plane in the clip space of its mirrored camera (the engine carries the world plane there with
        // the inverse of the view-projection).  The vertex shaders get the eye's view-projection, so the plane must follow: for every point the
        // eye's plane and clip coordinates must give the number the game's plane gives with the game's clip coordinates (= the world plane at the point).
        const int savedMode = g_modeOverride;
        const bool savedSeen = g_levelFilesSeen;
        char savedLevel[64];
        strcpy(savedLevel, g_currentLevel);
        unsigned draws = 0, same = 0, moved = 0, restored = 0, trials = 0, quiet = 0, quietTrials = 0;
        double worstError = 0;
        auto row = [](const double v[4], const Matrix4& m, double out[4]) {
            for (int j = 0; j < 4; j++) {
                out[j] = 0;
                for (int i = 0; i < 4; i++)
                    out[j] += v[i] * m.m[i][j];
            }
        };
        for (int trial = 0; trial < 20; trial++) {
            SetEyes(rng);
            const Matrix4 world = Rigid(rng, 3.0, 40.0);
            const Matrix4 view = ViewOf(mirror);
            const Matrix4 viewProj = Multiply(view, projection);
            float registers[8 * 4];
            StoreMatrix(Multiply(world, viewProj), kLayout, &registers[0]);
            StoreMatrix(Multiply(world, view), kLayout, &registers[16]);
            // a world plane (nearly the floor, at a random height) and the game's clip plane for it: (view-projection)^-1 * plane
            double worldPlane[4] = {rng.Uniform(-0.1, 0.1), 1.0, rng.Uniform(-0.1, 0.1), rng.Uniform(-3.0, 3.0)};
            Matrix4 viewProjInverse;
            Invert(viewProj, viewProjInverse);
            float gamePlane[4];
            for (int i = 0; i < 4; i++) {
                double s = 0;
                for (int k = 0; k < 4; k++)
                    s += viewProjInverse.m[i][k] * worldPlane[k];
                gamePlane[i] = (float)s;
            }
            for (int where = 0; where < 2; where++) {
                if (where == 0) { // the showroom
                    g_modeOverride = 0;
                    g_levelFilesSeen = true;
                    strcpy(g_currentLevel, "showroom");
                } else {
                    g_modeOverride = 2;
                    strcpy(g_currentLevel, "track");
                }
                FlushDeviceState();
                const unsigned long long frame = 600 + 2 * trial + where;
                publish(head, frame);
                ResetMainSizedReflectionTarget();
                draw(mirror, kReflection, frame);
                setTarget(kReflection, 2160, 1071);
                g_currentVsInfo = (VertexShaderInfo*)sceneShaders[trial & 1];
                g_currentPsInfo = g_psPool[K_SCENE][0];
                g_vr.frameCount = frame;
                GameSetVs(0, 8, registers);
                for (int pass = 0; pass < 2; pass++) { // 0: the game has clip plane 0 on (the reflection scene), 1: it has not (anything else)
                    DeviceNoteGameClipPlane(gamePlane);
                    DeviceNoteGameClipPlaneEnable(pass == 0 ? 1 : 0);
                    memcpy(g_devClipPlane, gamePlane, sizeof(gamePlane)); // (the game's own call went through to the device)
                    g_clipPlaneCalls = 0;
                    std::vector<float> seen;
                    std::vector<float> planes;
                    RunDraw((IDirect3DDevice9*)&g_fakeDevice, DrawCall([&]() {
                                seen.insert(seen.end(), g_devVs, g_devVs + 16);
                                planes.insert(planes.end(), g_devClipPlane, g_devClipPlane + 4);
                                return D3D_OK;
                            }));
                    if (pass == 1) {
                        quietTrials++;
                        quiet += g_clipPlaneCalls == 0 && memcmp(g_devClipPlane, gamePlane, sizeof(gamePlane)) == 0;
                        continue;
                    }
                    trials++;
                    for (size_t d = 0; d < planes.size() / 4; d++) {
                        Matrix4 eyeViewProjWorld;
                        LoadMatrix(&seen[d * 16], kLayout, eyeViewProjWorld);
                        bool ok = true;
                        for (int sample = 0; sample < 6; sample++) {
                            const double q[4] = {rng.Uniform(-5, 5), rng.Uniform(-5, 5), rng.Uniform(-5, 5), 1.0};
                            double clip[4], where4[4];
                            row(q, eyeViewProjWorld, clip);
                            row(q, world, where4);
                            double lhs = 0, rhs = 0, scale = 1;
                            for (int j = 0; j < 4; j++) {
                                lhs += (double)planes[d * 4 + j] * clip[j];
                                rhs += worldPlane[j] * where4[j];
                                scale += fabs((double)planes[d * 4 + j] * clip[j]);
                            }
                            const double error = fabs(lhs - rhs) / scale;
                            worstError = std::max(worstError, error);
                            ok = ok && error < 2e-3;
                        }
                        draws++;
                        same += ok;
                        moved += memcmp(&planes[d * 4], gamePlane, sizeof(gamePlane)) != 0;
                    }
                    FlushDeviceState();
                    restored += memcmp(g_devClipPlane, gamePlane, sizeof(gamePlane)) == 0;
                }
            }
        }
        g_modeOverride = savedMode;
        g_levelFilesSeen = savedSeen;
        strcpy(g_currentLevel, savedLevel);
        DeviceNoteGameClipPlaneEnable(0);
        printf("  (%u passes, %u draws: %u with the plane on the same points, %u with the plane moved, worst error %.2g; %u of %u passes left the device the game's plane; %u of %u passes without a game plane untouched)\n",
               trials, draws, same, moved, worstError, restored, trials, quiet, quietTrials);
        check(draws == 2 * trials && same == draws, "every draw gets a plane that keeps exactly the points the game's plane keeps (80 of 80 draws, two eyes each)");
        check(moved == draws, "and it is not the game's own numbers (the plane has moved with the eye)");
        check(restored == trials, "the game's plane is back on the device when the per-eye draws are over");
        check(quiet == quietTrials && quietTrials > 0, "with the game's clip plane off, the mod does not touch the device's plane");
    }
    printf("[VR] Reflections=0 (the game's own switch is not in this program, so the pass's draw calls are left out):\n");
    {
        const int savedMode = g_modeOverride;
        const bool savedSeen = g_levelFilesSeen;
        char savedLevel[64];
        strcpy(savedLevel, g_currentLevel);
        unsigned counts[3][2]; // by setting (reflections drawn, left out in a race, left out in the showroom) and draw (reflection scene draw, main scene draw)
        for (int setting = 0; setting < 3; setting++) {
            g_cfg.skipReflections = setting >= 1;
            g_levelFilesSeen = true;
            if (setting == 2) { // the showroom, as the game's file opens tell it
                g_modeOverride = 0;
                strcpy(g_currentLevel, "showroom");
            } else {
                g_modeOverride = 2;
                strcpy(g_currentLevel, "track");
            }
            ApplyReflectionSwitch(); // (it looks for the game's switch once, in a race; there is none here)
            for (int what = 0; what < 2; what++) {
                SetEyes(rng);
                Matrix4 world = Rigid(rng, 3.0, 40.0);
                Matrix4 view = ViewOf(what == 1 ? head : mirror);
                float registers[8 * 4];
                StoreMatrix(Multiply(world, Multiply(view, projection)), kLayout, &registers[0]);
                StoreMatrix(Multiply(world, view), kLayout, &registers[16]);
                FlushDeviceState();
                const unsigned long long frame = 800 + 2 * setting + what;
                publish(head, frame);
                ResetMainSizedReflectionTarget();
                if (what == 0) {
                    draw(mirror, kReflection, frame);
                    setTarget(kReflection, 2160, 1071);
                } else {
                    draw(head, kMain, frame);
                    setTarget(kMain, 2160, 1071);
                }
                g_currentVsInfo = (VertexShaderInfo*)sceneShaders[what & 1];
                g_currentPsInfo = g_psPool[K_SCENE][0];
                GameSetVs(0, 8, registers);
                const unsigned long long before = g_drawCalls;
                RunDraw((IDirect3DDevice9*)&g_fakeDevice, DrawCall([&]() {
                            g_drawCalls++;
                            return D3D_OK;
                        }));
                counts[setting][what] = (unsigned)(g_drawCalls - before);
            }
        }
        g_cfg.skipReflections = 0;
        g_modeOverride = savedMode;
        g_levelFilesSeen = savedSeen;
        strcpy(g_currentLevel, savedLevel);
        printf("  (draws issued; reflection scene draw / main scene draw: drawn %u/%u, left out in a race %u/%u, left out in the showroom %u/%u)\n", counts[0][0], counts[0][1], counts[1][0], counts[1][1],
               counts[2][0], counts[2][1]);
        check(counts[0][0] == 2 && counts[0][1] == 2, "with the setting off every draw is made (the scene draws once for each eye)");
        check(counts[1][0] == 0 && counts[1][1] == 2, "reflections left out in a race: the reflection scene draw goes, the main scene draw stays");
        check(counts[2][0] == 2 && counts[2][1] == 2, "the setting is for races: the showroom's floor keeps its reflection");
    }
    printf(failures ? "mirror: %d FAILURE(S)\n" : "mirror: all checks passed\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    QueryPerformanceFrequency(&g_counterFrequency);
    ApplyVariant(argc > 3 && std::string(argv[1]) == "check" ? atoi(argv[3]) : 0);
    InitWorld();
    Rng rng = {88172645463325252ull};
    std::string mode = argc > 1 ? argv[1] : "check";
    double a = 0.8179, b = 1.6496, c = -0.00003, d = 0.11667;
    if (mode == "check") {
        int frames = argc > 2 ? atoi(argv[2]) : 40;
        g_hashing = true;
        FrameData frame;
        int lastKind = -1;
        for (int f = 0; f < frames; f++) {
            double jitter = (f % 5) * 0.003;
            GenerateFrame(rng, frame, a + jitter, b + jitter, c, d);
            SetEyes(rng);
            g_vr.frameCount = f;
            g_sceneDrawsThisFrame = 0;
            for (const DrawRec& rec : frame.draws)
                PlayDraw(rec, lastKind);
            FlushDeviceState(); // Present
            unsigned long long endOfFrame = DeviceDigest();
            MixWords(g_hash, &endOfFrame, sizeof(endOfFrame));
        }
        printf("hash %016llx\n", g_hash);
        printf("draws %llu\n", g_drawCalls);
        printf("calls: vs-const %llu, ps-const %llu, render-state %llu, scissor %llu, viewport %llu, texture %llu, draws %llu\n",
               g_calls[0], g_calls[1], g_calls[2], g_calls[3], g_calls[4], g_calls[5], g_drawCalls);
        printf("queries: %llu (not compared)\n", g_queries);
        return 0;
    }
    if (mode == "mirror")
        return MirrorCheck(rng);
    if (mode == "micro") { // cost of the building blocks, to see what is worth caching
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        Matrix4 m1 = Rigid(rng, 1.0, 100.0), m2 = Rigid(rng, 1.0, 100.0), out;
        float regs[16];
        StoreMatrix(m1, 1, regs);
        volatile double sink = 0;
        auto run = [&](const char* name, auto fn) {
            const int reps = 2000000;
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < reps; i++)
                fn(i);
            QueryPerformanceCounter(&t1);
            printf("  %-28s %6.1f ns\n", name, (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)f.QuadPart / reps);
        };
        run("Invert", [&](int i) { m1.m[3][0] = i; Invert(m1, out); sink = out.m[0][0]; });
        run("Multiply", [&](int i) { m1.m[3][0] = i; out = Multiply(m1, m2); sink = out.m[0][0]; });
        run("LoadMatrix", [&](int i) { regs[0] = (float)i; LoadMatrix(regs, 1, out); sink = out.m[0][0]; });
        run("StoreMatrix", [&](int i) { m1.m[3][0] = i; StoreMatrix(m1, 1, regs); sink = regs[0]; });
        {
            static unsigned char bufA[200], bufB[200];
            run("memcmp 200 bytes", [&](int i) { bufA[7] = (unsigned char)i; sink = memcmp(bufA, bufB, 200); });
            run("word compare 200 bytes", [&](int i) {
                bufA[7] = (unsigned char)i;
                unsigned long long diff = 0;
                for (int w = 0; w < 25; w++) {
                    unsigned long long x, y;
                    memcpy(&x, bufA + w * 8, 8);
                    memcpy(&y, bufB + w * 8, 8);
                    diff |= x ^ y;
                }
                sink = (double)diff;
            });
            run("memcmp 16 bytes", [&](int i) { bufA[7] = (unsigned char)i; sink = memcmp(bufA, bufB, 16) != 0; });
        }
        {
            FrameData frame;
            GenerateFrame(rng, frame, 0.8179, 0.8179, -0.00003, 0.11667);
            SetEyes(rng);
            int lastKindMicro = -1;
            DrawRec scene = frame.draws[0];
            for (const DrawRec& rec : frame.draws)
                if (rec.kind == K_SCENE && g_vsPool[K_SCENE][rec.vs]->tracked.size() == 1) {
                    scene = rec;
                    break;
                }
            int targetClassMicro = 0;
            (void)targetClassMicro;
            PlayDraw(scene, lastKindMicro); // sets the state (kind scene)
            g_currentVsInfo = g_vsPool[K_SCENE][scene.vs];
            run("ApplyEyeConstants (1 matrix)", [&](int i) {
                float world[4] = {(float)i, 1.f, 2.f, 3.f};
                GameSetVs(0, 1, world);
                ApplyEyeConstants((IDirect3DDevice9*)&g_fakeDevice, i & 1);
            });
            run("RunDraw scene (state hot)", [&](int i) {
                float world[4] = {(float)i, 1.f, 2.f, 3.f};
                GameSetVs(0, 1, world);
                RunDraw((IDirect3DDevice9*)&g_fakeDevice, DrawCall([&]() { return D3D_OK; }));
            });
        }
#ifdef HAVE_DEVICE_STATE
        {
            float data[16];
            for (int k = 0; k < 16; k++)
                data[k] = (float)k;
            run("DeviceSetVsOverrides 4 regs", [&](int i) {
                data[0] = (float)i;
                VsWrite w = {0, 4, data};
                DeviceSetVsOverrides((IDirect3DDevice9*)&g_fakeDevice, &w, 1);
            });
            run("DeviceSetEyeScissor", [&](int i) {
                RECT r = {(i & 1) * 1000, 0, (i & 1) * 1000 + 1000, 1000};
                DeviceSetEyeScissor((IDirect3DDevice9*)&g_fakeDevice, r);
            });
        }
#endif
        {
            float viewProjRegs[16];
            StoreMatrix(Multiply(Rigid(rng, 1.0, 100.0), Projection(0.8179, 1.6496, -0.00003, 0.11667)), 1, viewProjRegs);
            g_rtWidth = 2160, g_rtHeight = 1071, g_backBufferWidth = 2160, g_backBufferHeight = 1071;
            run("TryRecoverProjection", [&](int i) { viewProjRegs[3] = viewProjRegs[3]; sink = TryRecoverProjection(viewProjRegs); });
            run("IsMainPass", [&](int i) { sink = IsMainPass(); });
            run("IsHalfResPass", [&](int i) { sink = IsHalfResPass(); });
            run("IsMainSizedTarget", [&](int i) { sink = IsMainSizedTarget(); });
            run("IsReflectionSizedPass", [&](int i) { sink = IsReflectionSizedPass(); });
        }
        run("BuildEyeProjection", [&](int i) { out = BuildEyeProjection(g_vr.eyeBox[0], i & 1); sink = out.m[0][0]; });
        return 0;
    }
    // bench
    CalibrateCallCost(argc > 2 ? atof(argv[2]) : 0);
    FrameData frame;
    GenerateFrame(rng, frame, a, b, c, d);
    SetEyes(rng);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    // warm up: layout detection, caches
    int lastKind = -1;
    for (int i = 0; i < 20; i++)
        for (const DrawRec& rec : frame.draws)
            PlayDraw(rec, lastKind);
    auto timeIt = [&](const std::vector<const DrawRec*>& draws, int reps) {
        double best = 1e30;
        for (int run = 0; run < 7; run++) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            for (int r = 0; r < reps; r++)
                for (const DrawRec* rec : draws)
                    PlayDraw(*rec, lastKind);
            QueryPerformanceCounter(&t1);
            double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart / ((double)reps * draws.size());
            best = std::min(best, ns);
        }
        return best;
    };
    printf("ns per draw (best of 7; includes the harness's own constant copies)\n");
    double frameNs = 0;
    for (int kind = 0; kind < K_COUNT; kind++) {
        std::vector<const DrawRec*> draws;
        for (const DrawRec& rec : frame.draws)
            if (rec.kind == kind)
                draws.push_back(&rec);
        double ns = timeIt(draws, std::max(1, 20000 / (int)draws.size()));
        frameNs += ns * draws.size();
        printf("  %-24s %4d draws  %8.0f ns\n", kKindNames[kind], (int)draws.size(), ns);
    }
    printf("one frame of %d draws: %.2f ms\n", (int)frame.draws.size(), frameNs / 1e6);
    return 0;
}
