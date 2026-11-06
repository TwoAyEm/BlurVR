// Upscaling of the race picture on the Direct3D 11 side.  See upscale_d3d11.h.
//
// The two pixel shaders follow the published FidelityFX Super Resolution 1.0 algorithm (EASU: edge-adaptive spatial upsampling over a 12-tap neighbourhood with a
// direction and length measured on the luma of the four nearest texels; RCAS: robust contrast adaptive sharpening over a plus-shaped neighbourhood).
// Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence (LICENSE_fsr.txt).
#include "common.h"
#include "core.h"
#include "upscale_d3d11.h"
#include <d3dcompiler.h>

namespace {

const char* kShaderSource = R"HLSL(
cbuffer Constants : register(b0)
{
    float4 con0;    // xy: source texels per destination pixel, zw: 0.5 * that - 0.5
    float4 srcRect; // the eye's rectangle in the source (texels, last texel included)
    float4 dstRect; // the eye's rectangle in the destination (pixels, last pixel included)
    float4 misc;    // x: sharpening strength (1 = strongest)
};
Texture2D<float4> tex : register(t0);

float4 VsMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 SrcLoad(int2 p)
{
    p = clamp(p, int2(srcRect.xy), int2(srcRect.zw));
    return tex.Load(int3(p, 0)).rgb;
}
float Luma(float3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// one of the four quads of the neighbourhood: a is above, b left, c the centre, d right, e below; w is the quad's bilinear weight
void EasuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / max(lenX, 1e-5);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / max(lenY, 1e-5);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

void EasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

float4 PsEasu(float4 sv : SV_Position) : SV_Target
{
    float2 ip = floor(sv.xy) - dstRect.xy;
    float2 pp = ip * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;
    int2 b0 = int2(fp) + int2(srcRect.xy);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    float3 bC = SrcLoad(b0 + int2(0, -1));
    float3 cC = SrcLoad(b0 + int2(1, -1));
    float3 eC = SrcLoad(b0 + int2(-1, 0));
    float3 fC = SrcLoad(b0 + int2(0, 0));
    float3 gC = SrcLoad(b0 + int2(1, 0));
    float3 hC = SrcLoad(b0 + int2(2, 0));
    float3 iC = SrcLoad(b0 + int2(-1, 1));
    float3 jC = SrcLoad(b0 + int2(0, 1));
    float3 kC = SrcLoad(b0 + int2(1, 1));
    float3 lC = SrcLoad(b0 + int2(2, 1));
    float3 nC = SrcLoad(b0 + int2(0, 2));
    float3 oC = SrcLoad(b0 + int2(1, 2));
    float lB = Luma(bC), lCc = Luma(cC), lE = Luma(eC), lF = Luma(fC), lG = Luma(gC), lH = Luma(hC);
    float lI = Luma(iC), lJ = Luma(jC), lK = Luma(kC), lL = Luma(lC), lN = Luma(nC), lO = Luma(oC);
    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    EasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), lB, lE, lF, lG, lJ);
    EasuSet(dir, len, pp.x * (1.0 - pp.y), lCc, lF, lG, lH, lK);
    EasuSet(dir, len, (1.0 - pp.x) * pp.y, lF, lI, lJ, lK, lN);
    EasuSet(dir, len, pp.x * pp.y, lG, lJ, lK, lL, lO);
    // direction: normalised, with a default for flat areas
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(max(dirR, 1e-12));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    // length: how much the kernel is stretched along the edge
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    float3 min4 = min(min(fC, gC), min(jC, kC));
    float3 max4 = max(max(fC, gC), max(jC, kC));
    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;
    EasuTap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    EasuTap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    EasuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);
    EasuTap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);
    EasuTap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);
    EasuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);
    EasuTap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);
    EasuTap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);
    EasuTap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);
    EasuTap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);
    EasuTap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);
    EasuTap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);
    float3 pix = min(max4, max(min4, aC / aW));
    return float4(saturate(pix), 1.0);
}

float3 DstLoad(int2 p)
{
    p = clamp(p, int2(dstRect.xy), int2(dstRect.zw));
    return tex.Load(int3(p, 0)).rgb;
}

float4 PsRcas(float4 sv : SV_Position) : SV_Target
{
    int2 p = int2(floor(sv.xy));
    float3 e = DstLoad(p);
    float3 b = DstLoad(p + int2(0, -1));
    float3 d = DstLoad(p + int2(-1, 0));
    float3 f = DstLoad(p + int2(1, 0));
    float3 h = DstLoad(p + int2(0, 1));
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = mn4 / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobe3 = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * misc.x;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 pix = (lobe * (b + d + f + h) + e) * rcpL;
    return float4(saturate(pix), 1.0);
}
)HLSL";

struct Constants {
    float con0[4];
    float srcRect[4];
    float dstRect[4];
    float misc[4];
};

struct State {
    bool tried = false, ready = false;
    ID3D11Device* device = NULL;
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* easu = NULL;
    ID3D11PixelShader* rcas = NULL;
    ID3D11Buffer* constants = NULL;
    ID3D11RasterizerState* raster = NULL;
    ID3D11DepthStencilState* depth = NULL;
    ID3D11BlendState* blend = NULL;
    ID3D11Texture2D* source = NULL;
    ID3D11ShaderResourceView* sourceView = NULL;
    ID3D11Texture2D* mid = NULL;
    ID3D11ShaderResourceView* midView = NULL;
    ID3D11RenderTargetView* midTarget = NULL;
    ID3D11Texture2D* out = NULL;
    ID3D11RenderTargetView* outTarget = NULL;
    UINT width = 0, height = 0;
} g;

template <class T> void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = NULL;
    }
}

bool Fail(const char* what, HRESULT hr)
{
    static unsigned said = 0;
    if (said++ < 6)
        Log("Upscale: %s failed 0x%08x", what, (unsigned)hr);
    return false;
}

typedef HRESULT(WINAPI* PFN_D3DCompileLocal)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

ID3DBlob* Compile(PFN_D3DCompileLocal compile, const char* entry, const char* target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = compile(kShaderSource, strlen(kShaderSource), "upscale", NULL, NULL, entry, target, 1 << 15 /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
    if (FAILED(hr)) {
        Log("Upscale: compiling %s failed 0x%08x: %s", entry, (unsigned)hr, errors ? (const char*)errors->GetBufferPointer() : "");
        SafeRelease(code);
    }
    SafeRelease(errors);
    return code;
}

bool Init(ID3D11Device* device)
{
    if (g.ready && g.device == device)
        return true;
    if (g.tried && g.device == device)
        return false;
    ReleaseUpscale();
    g.tried = true;
    g.device = device;
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompileLocal compile = compiler ? (PFN_D3DCompileLocal)GetProcAddress(compiler, "D3DCompile") : NULL;
    if (!compile) {
        Log("Upscale: d3dcompiler_47.dll is not available: no upscaling");
        return false;
    }
    ID3DBlob* vsCode = Compile(compile, "VsMain", "vs_4_0");
    ID3DBlob* easuCode = Compile(compile, "PsEasu", "ps_4_0");
    ID3DBlob* rcasCode = Compile(compile, "PsRcas", "ps_4_0");
    bool ok = vsCode && easuCode && rcasCode;
    if (ok) {
        HRESULT hr = device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), NULL, &g.vs);
        ok = SUCCEEDED(hr) || Fail("CreateVertexShader", hr);
        if (ok) {
            hr = device->CreatePixelShader(easuCode->GetBufferPointer(), easuCode->GetBufferSize(), NULL, &g.easu);
            ok = SUCCEEDED(hr) || Fail("CreatePixelShader (upscale)", hr);
        }
        if (ok) {
            hr = device->CreatePixelShader(rcasCode->GetBufferPointer(), rcasCode->GetBufferSize(), NULL, &g.rcas);
            ok = SUCCEEDED(hr) || Fail("CreatePixelShader (sharpen)", hr);
        }
    }
    SafeRelease(vsCode);
    SafeRelease(easuCode);
    SafeRelease(rcasCode);
    if (ok) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(Constants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device->CreateBuffer(&bd, NULL, &g.constants);
        ok = SUCCEEDED(hr) || Fail("CreateBuffer", hr);
    }
    if (ok) {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd = {};
        D3D11_BLEND_DESC bld = {};
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = (SUCCEEDED(device->CreateRasterizerState(&rd, &g.raster)) && SUCCEEDED(device->CreateDepthStencilState(&dd, &g.depth)) &&
              SUCCEEDED(device->CreateBlendState(&bld, &g.blend))) ||
             Fail("creating the render states", E_FAIL);
    }
    g.ready = ok;
    if (!ok) {
        ID3D11Device* keep = g.device;
        ReleaseUpscale();
        g.tried = true;
        g.device = keep;
    } else {
        Log("Upscale: shaders ready");
    }
    return ok;
}

bool EnsureTargets(ID3D11Device* device, UINT w, UINT h)
{
    if (g.out && g.width == w && g.height == h)
        return true;
    SafeRelease(g.outTarget);
    SafeRelease(g.out);
    SafeRelease(g.midTarget);
    SafeRelease(g.midView);
    SafeRelease(g.mid);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&td, NULL, &g.mid);
    if (FAILED(hr))
        return Fail("creating the sharpening input", hr);
    hr = device->CreateTexture2D(&td, NULL, &g.out);
    if (FAILED(hr))
        return Fail("creating the upscaled picture", hr);
    if (FAILED(hr = device->CreateShaderResourceView(g.mid, NULL, &g.midView)) || FAILED(hr = device->CreateRenderTargetView(g.mid, NULL, &g.midTarget)) ||
        FAILED(hr = device->CreateRenderTargetView(g.out, NULL, &g.outTarget)))
        return Fail("creating the views", hr);
    g.width = w;
    g.height = h;
    return true;
}

void SetConstants(ID3D11DeviceContext* context, const Constants& c)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(g.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &c, sizeof(c));
        context->Unmap(g.constants, 0);
    }
}

} // namespace

void UpscaleOutputSize(UINT w, UINT h, double factor, UINT* outW, UINT* outH)
{
    if (factor < 1.0)
        factor = 1.0;
    if (factor > 4.0)
        factor = 4.0;
    UINT ow = (UINT)(w * factor + 0.5);
    UINT oh = (UINT)(h * factor + 0.5);
    ow &= ~1u; // (each eye gets half)
    if (ow > 16384)
        ow = 16384;
    if (oh > 16384)
        oh = 16384;
    *outW = ow;
    *outH = oh;
}

ID3D11Texture2D* UpscaleFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT srcW, UINT srcH, UINT dstW, UINT dstH, double sharpness)
{
    if (!device || !context || !source || srcW < 4 || srcH < 4 || dstW < 4 || dstH < 4 || (srcW & 1) || (dstW & 1))
        return NULL;
    if (!Init(device))
        return NULL;
    if (source != g.source) {
        SafeRelease(g.sourceView);
        g.source = NULL;
        HRESULT hr = device->CreateShaderResourceView(source, NULL, &g.sourceView);
        if (FAILED(hr)) {
            Fail("a shader view of the game's picture (the shared texture has no shader resource bind flag?)", hr);
            return NULL;
        }
        g.source = source;
    }
    if (!EnsureTargets(device, dstW, dstH))
        return NULL;
    if (sharpness < 0.0)
        sharpness = 0.0;
    if (sharpness > 1.0)
        sharpness = 1.0;
    const bool sharpen = sharpness > 0.0;
    static bool said = false;
    if (!said) {
        said = true;
        Log("Upscale: %ux%u -> %ux%u (edge-adaptive upscale %s)", srcW, srcH, dstW, dstH, sharpen ? "+ sharpening" : "only");
    }

    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g.vs, NULL, 0);
    context->PSSetConstantBuffers(0, 1, &g.constants);
    context->RSSetState(g.raster);
    context->OMSetDepthStencilState(g.depth, 0);
    context->OMSetBlendState(g.blend, NULL, 0xFFFFFFFF);

    const UINT srcHalf = srcW / 2, dstHalf = dstW / 2;
    const double sx = (double)srcHalf / (double)dstHalf, sy = (double)srcH / (double)dstH;
    // pass 1: the upscale, one eye at a time, into the sharpening input (or straight into the result)
    ID3D11RenderTargetView* firstTarget = sharpen ? g.midTarget : g.outTarget;
    context->OMSetRenderTargets(1, &firstTarget, NULL);
    context->PSSetShader(g.easu, NULL, 0);
    context->PSSetShaderResources(0, 1, &g.sourceView);
    for (int eye = 0; eye < 2; eye++) {
        Constants c = {};
        c.con0[0] = (float)sx;
        c.con0[1] = (float)sy;
        c.con0[2] = (float)(0.5 * sx - 0.5);
        c.con0[3] = (float)(0.5 * sy - 0.5);
        c.srcRect[0] = (float)(eye * srcHalf);
        c.srcRect[1] = 0;
        c.srcRect[2] = (float)(eye * srcHalf + srcHalf - 1);
        c.srcRect[3] = (float)(srcH - 1);
        c.dstRect[0] = (float)(eye * dstHalf);
        c.dstRect[1] = 0;
        c.dstRect[2] = (float)(eye * dstHalf + dstHalf - 1);
        c.dstRect[3] = (float)(dstH - 1);
        SetConstants(context, c);
        D3D11_VIEWPORT vp = {(float)(eye * dstHalf), 0.0f, (float)dstHalf, (float)dstH, 0.0f, 1.0f};
        context->RSSetViewports(1, &vp);
        context->Draw(3, 0);
    }
    // pass 2: the sharpening, from the first pass's result into the result
    if (sharpen) {
        ID3D11RenderTargetView* second = g.outTarget;
        context->OMSetRenderTargets(1, &second, NULL);
        context->PSSetShader(g.rcas, NULL, 0);
        context->PSSetShaderResources(0, 1, &g.midView);
        for (int eye = 0; eye < 2; eye++) {
            Constants c = {};
            c.dstRect[0] = (float)(eye * dstHalf);
            c.dstRect[1] = 0;
            c.dstRect[2] = (float)(eye * dstHalf + dstHalf - 1);
            c.dstRect[3] = (float)(dstH - 1);
            c.misc[0] = (float)pow(2.0, -2.0 * (1.0 - sharpness)); // 1 = the strongest, a stop less for every half of the way down
            SetConstants(context, c);
            D3D11_VIEWPORT vp = {(float)(eye * dstHalf), 0.0f, (float)dstHalf, (float)dstH, 0.0f, 1.0f};
            context->RSSetViewports(1, &vp);
            context->Draw(3, 0);
        }
    }
    ID3D11ShaderResourceView* none = NULL;
    context->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView* noTarget = NULL;
    context->OMSetRenderTargets(1, &noTarget, NULL);
    return g.out;
}

void ReleaseUpscale()
{
    SafeRelease(g.outTarget);
    SafeRelease(g.out);
    SafeRelease(g.midTarget);
    SafeRelease(g.midView);
    SafeRelease(g.mid);
    SafeRelease(g.sourceView);
    g.source = NULL;
    SafeRelease(g.blend);
    SafeRelease(g.depth);
    SafeRelease(g.raster);
    SafeRelease(g.constants);
    SafeRelease(g.rcas);
    SafeRelease(g.easu);
    SafeRelease(g.vs);
    g.width = g.height = 0;
    g.ready = false;
    g.tried = false;
    g.device = NULL;
}
