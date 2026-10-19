#include "hidden_area.h"
#include <vector>
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "device_state.h"
#include "render_state.h"
#include "vr_runtime.h"

// GetHiddenAreaMesh returns an 8-byte struct (pointer, count) by value; on 32-bit x86 it comes back in EDX:EAX (see lens_model.cpp).
typedef unsigned long long(__stdcall* PFN_HiddenAreaMeshRaw)(EVREye, EHiddenAreaMeshType);

static const int kGrid = 64; // the mask is kGrid x kGrid cells per eye image

struct Run {
    float u0, u1, v0, v1; // a rectangle of hidden cells, in 0..1 of the eye image
};
static std::vector<Run> g_runs[2];
static bool g_maskBuilt = false;
static bool g_failed = false;

static bool InTriangle(float x, float y, const HmdVector2_t* t)
{
    float d1 = (x - t[1].v[0]) * (t[0].v[1] - t[1].v[1]) - (t[0].v[0] - t[1].v[0]) * (y - t[1].v[1]);
    float d2 = (x - t[2].v[0]) * (t[1].v[1] - t[2].v[1]) - (t[1].v[0] - t[2].v[0]) * (y - t[2].v[1]);
    float d3 = (x - t[0].v[0]) * (t[2].v[1] - t[0].v[1]) - (t[2].v[0] - t[0].v[0]) * (y - t[0].v[1]);
    bool negative = d1 < 0 || d2 < 0 || d3 < 0, positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}
static bool InMesh(const std::vector<HmdVector2_t>& mesh, float x, float y)
{
    for (size_t t = 0; t + 2 < mesh.size(); t += 3)
        if (InTriangle(x, y, &mesh[t]))
            return true;
    return false;
}

static bool BuildMask()
{
    if (!g_vr.system)
        return false;
    for (int eye = 0; eye < 2; eye++) {
        PFN_HiddenAreaMeshRaw getMesh = (PFN_HiddenAreaMeshRaw)(void*)g_vr.system->GetHiddenAreaMesh;
        unsigned long long raw = getMesh((EVREye)eye, EHiddenAreaMeshType_k_eHiddenAreaMesh_Standard);
        const HmdVector2_t* vertices = (const HmdVector2_t*)(uintptr_t)(unsigned)(raw & 0xFFFFFFFFull);
        unsigned triangles = (unsigned)(raw >> 32);
        if (!vertices || triangles == 0 || triangles > 10000) {
            Log("hidden-area cull: SteamVR gave no hidden-area mesh for eye %d", eye);
            return false;
        }
        std::vector<HmdVector2_t> mesh(vertices, vertices + (size_t)triangles * 3);
        // a cell is hidden when its centre is inside the mesh as given AND inside it turned upside down (the mesh is nearly symmetric, the
        // convention for v is not certain: whatever the truth, only what is hidden either way is used)
        std::vector<char> hidden((size_t)kGrid * kGrid, 0);
        for (int j = 0; j < kGrid; j++)
            for (int i = 0; i < kGrid; i++) {
                float u = (i + 0.5f) / kGrid, v = (j + 0.5f) / kGrid;
                hidden[(size_t)j * kGrid + i] = InMesh(mesh, u, v) && InMesh(mesh, u, 1.0f - v);
            }
        // erode by the margin: a hidden cell stays only if every cell within the margin is hidden too (outside the image counts as hidden)
        int margin = g_cfg.hiddenAreaMargin < 0 ? 0 : g_cfg.hiddenAreaMargin;
        std::vector<char> stamped((size_t)kGrid * kGrid, 0);
        for (int j = 0; j < kGrid; j++)
            for (int i = 0; i < kGrid; i++) {
                bool all = hidden[(size_t)j * kGrid + i] != 0;
                for (int dj = -margin; dj <= margin && all; dj++)
                    for (int di = -margin; di <= margin && all; di++) {
                        int jj = j + dj, ii = i + di;
                        if (jj >= 0 && jj < kGrid && ii >= 0 && ii < kGrid && !hidden[(size_t)jj * kGrid + ii])
                            all = false;
                    }
                stamped[(size_t)j * kGrid + i] = all;
            }
        // runs of stamped cells per row, joined with the identical run of the row above
        g_runs[eye].clear();
        std::vector<Run> open;
        std::vector<std::pair<int, int>> openCells;
        size_t cells = 0;
        for (int j = 0; j < kGrid; j++) {
            std::vector<std::pair<int, int>> rowRuns;
            for (int i = 0; i < kGrid;) {
                if (!stamped[(size_t)j * kGrid + i]) {
                    i++;
                    continue;
                }
                int start = i;
                while (i < kGrid && stamped[(size_t)j * kGrid + i])
                    i++;
                rowRuns.push_back(std::make_pair(start, i));
                cells += (size_t)(i - start);
            }
            std::vector<Run> nextOpen;
            std::vector<std::pair<int, int>> nextCells;
            for (const std::pair<int, int>& run : rowRuns) {
                bool joined = false;
                for (size_t k = 0; k < openCells.size(); k++)
                    if (openCells[k] == run) {
                        open[k].v1 = (j + 1.0f) / kGrid;
                        nextOpen.push_back(open[k]);
                        nextCells.push_back(run);
                        openCells[k] = std::make_pair(-1, -1);
                        joined = true;
                        break;
                    }
                if (!joined) {
                    Run r = {(float)run.first / kGrid, (float)run.second / kGrid, (float)j / kGrid, (j + 1.0f) / kGrid};
                    nextOpen.push_back(r);
                    nextCells.push_back(run);
                }
            }
            for (size_t k = 0; k < openCells.size(); k++)
                if (openCells[k].first >= 0)
                    g_runs[eye].push_back(open[k]);
            open = nextOpen;
            openCells = nextCells;
        }
        for (const Run& r : open)
            g_runs[eye].push_back(r);
        Log("hidden-area cull: eye %d mask built, %u of %d cells stamped (%.1f%% of the image, margin %d), %u rectangles", eye, (unsigned)cells,
            kGrid * kGrid, 100.0 * cells / (kGrid * kGrid), margin, (unsigned)g_runs[eye].size());
    }
    return true;
}

static float g_clearZ = 0.0f;
static bool g_pending = true;
static unsigned long long g_stampFrame = ~0ull;
static unsigned g_stamps = 0;
static unsigned long long g_summaryFrame = 0;

void HiddenAreaNoteDepthClear(float z)
{
    g_clearZ = z;
    g_pending = true;
}

struct SavedState {
    D3DRENDERSTATETYPE state;
    DWORD value;
};

static void Stamp(IDirect3DDevice9* device)
{
    if (!g_maskBuilt) {
        g_maskBuilt = true;
        if (!BuildMask()) {
            g_failed = true;
            return;
        }
    }
    DeviceRestoreAll(device); // the device must hold the game's own state, which is what gets saved below
    PFN_SetRenderState setState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetVertexShader setVertexShader = (PFN_SetVertexShader)g_deviceOriginals[VT_SetVertexShader];
    PFN_SetPixelShader setPixelShader = (PFN_SetPixelShader)g_deviceOriginals[VT_SetPixelShader];
    PFN_DrawPrimitiveUP drawUP = (PFN_DrawPrimitiveUP)g_deviceOriginals[VT_DrawPrimitiveUP];

    // the stamp: as near as the game's depth test allows for the way it clears (cleared to 0: reversed depth, nearest = 1)
    float stampZ = g_clearZ < 0.5f ? 1.0f : 0.0f;
    D3DVIEWPORT9 base = g_gameViewport;
    float half = (float)(base.Width / 2);
    std::vector<float> vertices;
    for (int eye = 0; eye < 2; eye++)
        for (const Run& r : g_runs[eye]) {
            float x0 = (float)base.X + eye * half + r.u0 * half, x1 = (float)base.X + eye * half + r.u1 * half;
            float y0 = (float)base.Y + r.v0 * base.Height, y1 = (float)base.Y + r.v1 * base.Height;
            const float quad[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
            for (int k = 0; k < 6; k++) {
                vertices.push_back(quad[k][0]);
                vertices.push_back(quad[k][1]);
                vertices.push_back(stampZ);
                vertices.push_back(1.0f);
            }
        }
    if (vertices.empty())
        return;

    // save what the stamp changes
    SavedState saved[] = {{D3DRS_ZENABLE, 0},          {D3DRS_ZWRITEENABLE, 0},    {D3DRS_ZFUNC, 0},
                          {D3DRS_COLORWRITEENABLE, 0}, {D3DRS_ALPHATESTENABLE, 0}, {D3DRS_STENCILENABLE, 0},
                          {D3DRS_CULLMODE, 0},         {D3DRS_SCISSORTESTENABLE, 0}, {D3DRS_FILLMODE, 0},
                          {D3DRS_DEPTHBIAS, 0},        {D3DRS_SLOPESCALEDEPTHBIAS, 0}};
    const int count = (int)(sizeof(saved) / sizeof(saved[0]));
    for (int k = 0; k < count; k++)
        device->GetRenderState(saved[k].state, &saved[k].value);
    IDirect3DVertexShader9* vs = NULL;
    IDirect3DPixelShader9* ps = NULL;
    IDirect3DVertexBuffer9* stream = NULL;
    IDirect3DVertexDeclaration9* declaration = NULL;
    UINT streamOffset = 0, streamStride = 0;
    DWORD fvf = 0;
    device->GetVertexShader(&vs);
    device->GetPixelShader(&ps);
    device->GetStreamSource(0, &stream, &streamOffset, &streamStride);
    device->GetVertexDeclaration(&declaration);
    device->GetFVF(&fvf);

    setState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    setState(device, D3DRS_ZWRITEENABLE, TRUE);
    setState(device, D3DRS_ZFUNC, D3DCMP_ALWAYS);
    setState(device, D3DRS_COLORWRITEENABLE, 0);
    setState(device, D3DRS_ALPHATESTENABLE, FALSE);
    setState(device, D3DRS_STENCILENABLE, FALSE);
    setState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    setState(device, D3DRS_SCISSORTESTENABLE, FALSE);
    setState(device, D3DRS_FILLMODE, D3DFILL_SOLID);
    setState(device, D3DRS_DEPTHBIAS, 0);
    setState(device, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    setVertexShader(device, NULL);
    setPixelShader(device, NULL);
    device->SetFVF(D3DFVF_XYZRHW);
    HRESULT hr = drawUP(device, D3DPT_TRIANGLELIST, (UINT)(vertices.size() / 4 / 3), vertices.data(), 4 * sizeof(float));

    // put everything back
    for (int k = 0; k < count; k++)
        setState(device, saved[k].state, saved[k].value);
    setVertexShader(device, vs);
    setPixelShader(device, ps);
    device->SetStreamSource(0, stream, streamOffset, streamStride);
    if (declaration)
        device->SetVertexDeclaration(declaration);
    else if (fvf)
        device->SetFVF(fvf);
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
    if (stream)
        stream->Release();
    if (declaration)
        declaration->Release();

    g_stamps++;
    if (g_stamps <= 6)
        Log("hidden-area cull: stamp #%u at frame %llu, depth cleared to %.2f -> stamped %.1f, %u triangles, result 0x%08x, game z %u/%u func %u",
            g_stamps, g_vr.frameCount, g_clearZ, stampZ, (unsigned)(vertices.size() / 12), (unsigned)hr, (unsigned)saved[0].value,
            (unsigned)saved[1].value, (unsigned)saved[2].value);
    if (g_vr.frameCount >= g_summaryFrame + 900) {
        Log("hidden-area cull: %u stamps in %llu frames so far", g_stamps, g_vr.frameCount);
        g_summaryFrame = g_vr.frameCount;
    }
}

void HiddenAreaBeforeMainDraw(IDirect3DDevice9* device)
{
    if (g_failed)
        return;
    if (g_stampFrame != g_vr.frameCount) { // once per frame at least, whatever the game does with its depth buffer
        g_stampFrame = g_vr.frameCount;
        g_pending = true;
    }
    if (!g_pending)
        return;
    g_pending = false;
    Stamp(device);
}
