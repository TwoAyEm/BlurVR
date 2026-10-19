// Light-streak sprites: captured in a layer and re-projected into each eye.
#include "streaks.h"
#include "camera.h"
#include "core.h"
#include "d3d_hooks.h"
#include "hud.h"
#include "matrix.h"
#include "render_state.h"
#include "vr_runtime.h"

bool g_streakUsed = false;

// ---- light-streak sprites: drawn by the game once, in mono screen space, into a low-res glow buffer. We capture them
// in a layer
// ---- and re-project the layer into each eye through the head-tracking transform so they stay locked to the world
// direction.
static IDirect3DTexture9* g_streakTexture = NULL;
static IDirect3DSurface9* g_streakSurface = NULL;
static IDirect3DStateBlock9* g_streakStateBlock = NULL;
static UINT g_streakWidth = 0;
static UINT g_streakHeight = 0;
static bool g_streakFailed = false;

void ReleaseStreakLayer()
{
    if (g_streakStateBlock) {
        g_streakStateBlock->Release();
        g_streakStateBlock = NULL;
    }
    if (g_streakSurface) {
        g_streakSurface->Release();
        g_streakSurface = NULL;
    }
    if (g_streakTexture) {
        g_streakTexture->Release();
        g_streakTexture = NULL;
    }
    g_streakWidth = g_streakHeight = 0;
    g_streakUsed = false;
    g_streakFailed = false;
}

bool EnsureStreakLayer(IDirect3DDevice9* device)
{
    if (g_streakTexture && g_streakWidth == g_rtWidth && g_streakHeight == g_rtHeight)
        return true;
    if (g_streakFailed || !g_rtWidth || !g_rtHeight)
        return false;
    if (g_streakSurface) {
        g_streakSurface->Release();
        g_streakSurface = NULL;
    }
    if (g_streakTexture) {
        g_streakTexture->Release();
        g_streakTexture = NULL;
    }
    PFN_CreateTexture createTexture = (PFN_CreateTexture)g_deviceOriginals[VT_CreateTexture];
    HRESULT hr = createTexture(device, g_rtWidth, g_rtHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                               D3DPOOL_DEFAULT, &g_streakTexture, NULL);
    if (FAILED(hr) || !g_streakTexture) {
        Log("streak layer creation failed (0x%08x)", (unsigned)hr);
        g_streakTexture = NULL;
        g_streakFailed = true;
        return false;
    }
    g_streakTexture->GetSurfaceLevel(0, &g_streakSurface);
    if (!g_streakStateBlock)
        device->CreateStateBlock(D3DSBT_ALL, &g_streakStateBlock);
    g_streakWidth = g_rtWidth;
    g_streakHeight = g_rtHeight;
    Log("streak layer %ux%u created", g_streakWidth, g_streakHeight);
    return g_streakSurface != NULL && g_streakStateBlock != NULL;
}

HRESULT DrawStreakToLayer(IDirect3DDevice9* device, const DrawCall& draw)
{
    PFN_SetRenderTarget setRenderTarget = (PFN_SetRenderTarget)g_deviceOriginals[VT_SetRenderTarget];
    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetDepthStencilSurface setDepthStencil =
        (PFN_SetDepthStencilSurface)g_deviceOriginals[VT_SetDepthStencilSurface];
    IDirect3DSurface9* savedTarget = NULL;
    device->GetRenderTarget(0, &savedTarget);
    if (!savedTarget)
        return draw();
    IDirect3DSurface9* savedDepth = NULL;
    device->GetDepthStencilSurface(&savedDepth);
    DWORD savedSeparateAlpha = 0, savedSrcAlpha = 0, savedDestAlpha = 0, savedOpAlpha = 0, savedScissor = 0;
    device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &savedSeparateAlpha);
    device->GetRenderState(D3DRS_SRCBLENDALPHA, &savedSrcAlpha);
    device->GetRenderState(D3DRS_DESTBLENDALPHA, &savedDestAlpha);
    device->GetRenderState(D3DRS_BLENDOPALPHA, &savedOpAlpha);
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &savedScissor);
    if (savedDepth)
        setDepthStencil(device, NULL);
    setRenderTarget(device, 0, g_streakSurface);
    if (!g_streakUsed) {
        D3DVIEWPORT9 fullViewport;
        fullViewport.X = 0;
        fullViewport.Y = 0;
        fullViewport.Width = g_streakWidth;
        fullViewport.Height = g_streakHeight;
        fullViewport.MinZ = 0;
        fullViewport.MaxZ = 1;
        setViewport(device, &fullViewport);
        setRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
        device->Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        g_streakUsed = true;
    }
    setViewport(device, &g_gameViewport);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
    setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    setRenderState(device, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    setRenderState(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    setRenderState(device, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    setRenderState(device, D3DRS_COLORWRITEENABLE, 0xF);
    HRESULT hr = draw();
    setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, savedSeparateAlpha);
    setRenderState(device, D3DRS_SRCBLENDALPHA, savedSrcAlpha);
    setRenderState(device, D3DRS_DESTBLENDALPHA, savedDestAlpha);
    setRenderState(device, D3DRS_BLENDOPALPHA, savedOpAlpha);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, savedScissor);
    setRenderTarget(device, 0, savedTarget);
    if (savedDepth) {
        setDepthStencil(device, savedDepth);
        savedDepth->Release();
    }
    setViewport(device, &g_gameViewport);
    savedTarget->Release();
    return hr;
}

// Re-project the captured streak layer into the current (stereo) glow buffer, one perspective quad per eye.
void FlushStreakLayer(IDirect3DDevice9* device)
{
    if (!g_streakUsed || !g_streakStateBlock || !g_streakTexture)
        return;
    g_streakUsed = false;
    if (!g_rtWidth || !g_rtHeight || g_projXScale == 0 || g_projYScale == 0)
        return;
    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    PFN_SetTexture setTexture = (PFN_SetTexture)g_deviceOriginals[VT_SetTexture];
    PFN_SetVertexShader setVertexShader = (PFN_SetVertexShader)g_deviceOriginals[VT_SetVertexShader];
    PFN_DrawPrimitiveUP drawPrimitiveUP = (PFN_DrawPrimitiveUP)g_deviceOriginals[VT_DrawPrimitiveUP];
    IDirect3DBaseTexture9* savedTexture0 = g_boundTextures[0];
    g_streakStateBlock->Capture();
    D3DVIEWPORT9 fullViewport;
    fullViewport.X = 0;
    fullViewport.Y = 0;
    fullViewport.Width = g_rtWidth;
    fullViewport.Height = g_rtHeight;
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
    setTexture(device, 0, g_streakTexture);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
    device->SetSamplerState(0, D3DSAMP_BORDERCOLOR, 0x00000000);
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
    double imageWidth = g_rtWidth, imageHeight = g_rtHeight, eyeW = imageWidth / 2.0, distance = 40.0;
    double cornerNdc[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
    float texCoords[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int eye = 0; eye < 2; eye++) {
        const Matrix4& delta = g_vr.eyeCameraDelta[eye];
        const EyeBox& box = g_vr.eyeBox[eye];
        double tanWidth = fmax(1e-3, box.right - box.left), tanHeight = fmax(1e-3, box.up + box.down);
        ScreenVertex corners[4];
        bool allVisible = true;
        for (int i = 0; i < 4; i++) {
            double x = cornerNdc[i][0] / g_projXScale * distance, y = cornerNdc[i][1] / g_projYScale * distance;
            double vx = x * delta.m[0][0] + y * delta.m[1][0] + distance * delta.m[2][0] + delta.m[3][0];
            double vy = x * delta.m[0][1] + y * delta.m[1][1] + distance * delta.m[2][1] + delta.m[3][1];
            double vz = x * delta.m[0][2] + y * delta.m[1][2] + distance * delta.m[2][2] + delta.m[3][2];
            if (vz < 0.5) {
                allVisible = false;
                break;
            }
            corners[i].x = (float)(eye * eyeW + (vx / vz - box.left) / tanWidth * eyeW - 0.5);
            corners[i].y = (float)((box.up - vy / vz) / tanHeight * imageHeight - 0.5);
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
        r.bottom = (LONG)imageHeight;
        setScissorRect(device, &r);
        drawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, corners, sizeof(ScreenVertex));
    }
    g_streakStateBlock->Apply();
    setTexture(device, 0, savedTexture0);
    setViewport(device, &g_gameViewport);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
}
