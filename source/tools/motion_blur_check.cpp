// Host check of the motion blur switch ([VR] MotionBlur, NOTES.md section 5): the mod is linked as it is (every src\*.cpp except dllmain.cpp) and MotionBlurPerEye is run on a real
// Direct3D 9 device with a stand-in for the game's blur shader (a shader of either model whose constant table names g_mMatrix and g_vStepScale and that paints everything red).
// With MotionBlur=0 the pass has to copy the picture instead (alpha forced to 1, like the blur does), with the blur's own shader bound again afterwards and the tracking untouched;
// with MotionBlur=1, or when the bound shader is not known, the stand-in must be drawn as before, once per eye.  Built and run by tools/motion_blur_check.py.
#include <stdio.h>
#include <algorithm>
#include "camera.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "render_state.h"
#include "screen_space_passes.h"
#include "shader_tracking.h"
#include "vr_runtime.h"

static int g_failures = 0;
#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL line %d: %s -- ", __LINE__, #cond);       \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static const int kW = 64, kH = 32;
static const unsigned kRed = 0xFFFF0000;

// ---- a stand-in for the game's blur shader: the real constant table (so the mod's parser finds the blur), and a body that paints red -------------------------------------
static std::vector<DWORD> StandInBlur(DWORD version)
{
    const char* names[] = {"g_mMatrix", "g_vStepScale"};
    const WORD regs[] = {0, 4}, counts[] = {4, 1};
    std::vector<BYTE> blob(28 + 2 * 20, 0);
    DWORD offsets[2];
    for (int i = 0; i < 2; i++) {
        offsets[i] = (DWORD)blob.size();
        for (const char* p = names[i]; *p; p++)
            blob.push_back((BYTE)*p);
        blob.push_back(0);
    }
    while (blob.size() % 4)
        blob.push_back(0);
    *(DWORD*)&blob[0] = 28;
    *(DWORD*)&blob[12] = 2;
    *(DWORD*)&blob[16] = 28;
    for (int i = 0; i < 2; i++) {
        BYTE* entry = &blob[28 + i * 20];
        *(DWORD*)(entry + 0) = offsets[i];
        *(WORD*)(entry + 4) = 2; // D3DXRS_FLOAT4
        *(WORD*)(entry + 6) = regs[i];
        *(WORD*)(entry + 8) = counts[i];
    }
    std::vector<DWORD> tokens;
    tokens.push_back(version);
    tokens.push_back(((DWORD)(1 + blob.size() / 4) << 16) | 0xFFFE);
    tokens.push_back(0x42415443); // 'CTAB'
    for (size_t i = 0; i < blob.size(); i += 4)
        tokens.push_back(*(DWORD*)&blob[i]);
    const DWORD define[] = {0x05000051, 0xA00F0014, 0x3F800000, 0x00000000, 0x00000000, 0x3F800000}; // def c20, 1, 0, 0, 1
    const DWORD declare[] = {0x0200001F, 0x80000005, 0x90030000};                                      // dcl_texcoord v0.xy (ps_3_0 wants its inputs declared)
    const DWORD body[] = {0x02000001, 0x800F0800, 0xA0E40014,                                          // mov oC0, c20
                          0x0000FFFF};
    tokens.insert(tokens.end(), define, define + 6);
    if ((version & 0xFF00) == 0x0300)
        tokens.insert(tokens.end(), declare, declare + 3);
    tokens.insert(tokens.end(), body, body + 4);
    return tokens;
}
// the vertex shaders that go with the two pixel shader models (Direct3D 9 pairs a 3.0 pixel shader with a 3.0 vertex shader and a 2.0 one with a 2.0 one): position and texture coordinate passed on
static const DWORD kPassVertexShader30[] = {0xFFFE0300,
                                            0x0200001F, 0x80000000, 0x900F0000, // dcl_position v0
                                            0x0200001F, 0x80000005, 0x900F0001, // dcl_texcoord v1
                                            0x0200001F, 0x80000000, 0xE00F0000, // dcl_position o0
                                            0x0200001F, 0x80000005, 0xE0030001, // dcl_texcoord o1.xy
                                            0x02000001, 0xE00F0000, 0x90E40000, // mov o0, v0
                                            0x02000001, 0xE0030001, 0x90E40001, // mov o1.xy, v1
                                            0x0000FFFF};
static const DWORD kPassVertexShader20[] = {0xFFFE0200,
                                            0x0200001F, 0x80000000, 0x900F0000, // dcl_position v0
                                            0x0200001F, 0x80000005, 0x900F0001, // dcl_texcoord v1
                                            0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
                                            0x02000001, 0xE0030000, 0x90E40001, // mov oT0.xy, v1
                                            0x0000FFFF};

struct Vertex3 {
    float x, y, z, w, u, v;
};

// what the mod makes on the device: the version token of every pixel shader it creates (the check's own stand-ins go straight to the device)
static void* g_realCreatePixelShader = NULL;
static std::vector<DWORD> g_createdShaders;
static HRESULT STDMETHODCALLTYPE RecordCreatePixelShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DPixelShader9** shader)
{
    g_createdShaders.push_back(function[0]);
    return ((PFN_CreatePixelShader)g_realCreatePixelShader)(device, function, shader);
}

static LRESULT CALLBACK CheckProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return DefWindowProcA(window, message, w, l);
}

static bool ReadTarget(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* copy, std::vector<unsigned>& pixels)
{
    D3DLOCKED_RECT locked;
    if (FAILED(device->GetRenderTargetData(target, copy)) || FAILED(copy->LockRect(&locked, NULL, D3DLOCK_READONLY)))
        return false;
    pixels.resize((size_t)kW * kH);
    for (int y = 0; y < kH; y++)
        memcpy(&pixels[(size_t)y * kW], (const BYTE*)locked.pBits + (size_t)y * locked.Pitch, (size_t)kW * 4);
    copy->UnlockRect();
    return true;
}

static unsigned SourcePixel(int x, int y)
{
    return 0x40000000u | (unsigned)(x * 4) << 16 | (unsigned)(y * 8) << 8 | (unsigned)((x + y) * 3 & 0xFF);
}

int main()
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = CheckProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "motion_blur_check";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "motion_blur_check", WS_OVERLAPPEDWINDOW, 100, 100, 200, 200, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = NULL;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 128;
    pp.BackBufferHeight = 128;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = window;
    HRESULT hr = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device) : E_FAIL;
    if (FAILED(hr) || !device) {
        printf("SKIP: no Direct3D 9 device here (0x%08x)\n", (unsigned)hr);
        return 0;
    }
    // the mod's hooks are not installed here: the "originals" are the device's own functions (which also checks the vtable numbers of d3d_hooks.h)
    void** vtable = *(void***)device;
    const int used[] = {VT_SetRenderState, VT_SetScissorRect, VT_SetPixelShaderConstantF, VT_CreatePixelShader, VT_SetPixelShader};
    for (int index : used)
        g_deviceOriginals[index] = vtable[index];
    g_realCreatePixelShader = vtable[VT_CreatePixelShader];
    g_deviceOriginals[VT_CreatePixelShader] = (void*)RecordCreatePixelShader;

    g_vr.eyeBox[0] = {-1.544, 1.047, 1.405, 1.411}; // the per-eye path reads the eyes' fields of view and the game's projection
    g_vr.eyeBox[1] = {-1.052, 1.538, 1.404, 1.415};
    g_projXScale = 0.8179;
    g_projYScale = 1.6496;
    g_rtWidth = kW;
    g_rtHeight = kH;

    IDirect3DTexture9 *source = NULL, *targetTexture = NULL;
    IDirect3DSurface9 *target = NULL, *copy = NULL;
    CHECK(SUCCEEDED(device->CreateTexture(kW, kH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &source, NULL)), "source texture");
    CHECK(SUCCEEDED(device->CreateTexture(kW, kH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &targetTexture, NULL)), "target texture");
    CHECK(SUCCEEDED(device->CreateOffscreenPlainSurface(kW, kH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, NULL)), "readback surface");
    if (!source || !targetTexture || !copy) {
        printf("SKIP: no surfaces\n");
        return 1;
    }
    targetTexture->GetSurfaceLevel(0, &target);
    D3DLOCKED_RECT locked;
    CHECK(SUCCEEDED(source->LockRect(0, &locked, NULL, D3DLOCK_DISCARD)), "lock source");
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            ((unsigned*)((BYTE*)locked.pBits + (size_t)y * locked.Pitch))[x] = SourcePixel(x, y);
    source->UnlockRect(0);

    // the two shader models: the stand-in blur (made on the device, noted the way the mod's hook notes the game's), the geometry that goes with each
    IDirect3DVertexShader9* vertexShaders[4] = {}; // by shader model
    IDirect3DVertexDeclaration9* declaration = NULL;
    const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                          {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                          D3DDECL_END()};
    CHECK(SUCCEEDED(device->CreateVertexShader(kPassVertexShader30, &vertexShaders[3])), "vs_3_0 for the check");
    CHECK(SUCCEEDED(device->CreateVertexShader(kPassVertexShader20, &vertexShaders[2])), "vs_2_0 for the check");
    CHECK(SUCCEEDED(device->CreateVertexDeclaration(elements, &declaration)), "vertex declaration");
    const Vertex3 quad3[4] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};

    for (int model : {3, 2}) {
        printf("ps_%d_0:\n", model);
        const std::vector<DWORD> tokens = StandInBlur(model == 3 ? 0xFFFF0300 : 0xFFFF0200);
        IDirect3DPixelShader9* blurShader = NULL;
        hr = device->CreatePixelShader(tokens.data(), &blurShader);
        CHECK(SUCCEEDED(hr) && blurShader, "the stand-in blur shader is accepted by the runtime (0x%08x)", (unsigned)hr);
        if (!blurShader)
            continue;
        PixelShaderInfo info;
        info.id = 1000 + model;
        ParsePixelShaderConstants(tokens.data(), &info);
        CHECK(info.motionBlur && info.shaderMajor == model, "the mod's parser sees a blur shader of model %d (motionBlur %d, model %d)", model, (int)info.motionBlur, info.shaderMajor);

        auto setUp = [&]() {
            device->SetRenderTarget(0, target);
            device->SetTexture(0, source);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetPixelShader(blurShader);
            D3DVIEWPORT9 viewport = {0, 0, (DWORD)kW, (DWORD)kH, 0.0f, 1.0f};
            device->SetViewport(&viewport);
            RECT scissor = {3, 4, kW - 5, kH - 6};
            device->SetScissorRect(&scissor);
            g_gameScissorRect = scissor;
            g_gameScissorEnabled = FALSE;
            device->SetVertexShader(vertexShaders[model]);
            device->SetVertexDeclaration(declaration);
            const float marker[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}; // the game's g_mMatrix
            device->SetPixelShaderConstantF(0, marker, 4);
            memcpy(g_psConstants, marker, sizeof(marker));
            g_currentPsInfo = &info;
            g_currentPsHandle = blurShader;
        };
        auto drawQuad = [&]() -> HRESULT {
            if (FAILED(device->BeginScene()))
                return E_FAIL;
            HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad3, sizeof(Vertex3));
            device->EndScene();
            return drawn;
        };
        DrawCall draw(drawQuad); // (a DrawCall only borrows what it is made from)
        int draws = 0;
        auto countQuad = [&]() -> HRESULT {
            draws++;
            return draw();
        };
        DrawCall countedDraw(countQuad);
        auto clear = [&]() { device->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0); }; // (setUp has the target set: setting it again would reset the scissor rectangle)
        auto settled = [&](const char* when) { // everything the pass touched is as the game left it
            IDirect3DPixelShader9* bound = NULL;
            device->GetPixelShader(&bound);
            CHECK(bound == blurShader, "%s: the game's shader is bound again", when);
            if (bound)
                bound->Release();
            CHECK(g_currentPsInfo == &info && g_currentPsHandle == (void*)blurShader, "%s: the mod's tracking of the bound shader is as it was", when);
            float constants[16];
            device->GetPixelShaderConstantF(0, constants, 4);
            bool same = true;
            for (int i = 0; i < 16; i++)
                same = same && constants[i] == (float)(i + 1);
            CHECK(same, "%s: g_mMatrix on the device is as the game set it", when);
            DWORD scissorTest = 7;
            device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
            RECT rect = {};
            device->GetScissorRect(&rect);
            CHECK(scissorTest == (DWORD)g_gameScissorEnabled && rect.left == 3 && rect.top == 4 && rect.right == kW - 5 && rect.bottom == kH - 6, "%s: the game's scissor state is back", when);
        };
        std::vector<unsigned> pixels;

        // MotionBlur=0: the picture is copied, once
        g_cfg.motionBlur = 0;
        setUp();
        clear();
        draws = 0;
        hr = MotionBlurPerEye(device, countedDraw);
        CHECK(SUCCEEDED(hr) && draws == 1, "MotionBlur=0: one draw (%d), hr 0x%08x", draws, (unsigned)hr);
        CHECK(ReadTarget(device, target, copy, pixels), "read the target");
        int wrong = 0;
        for (int y = 0; y < kH && pixels.size() == (size_t)kW * kH; y++)
            for (int x = 0; x < kW; x++)
                wrong += pixels[(size_t)y * kW + x] != ((SourcePixel(x, y) & 0x00FFFFFF) | 0xFF000000);
        CHECK(wrong == 0, "MotionBlur=0: %d of %d pixels differ from the source (alpha forced to 1)", wrong, kW * kH);
        settled("MotionBlur=0");
        const DWORD wanted = 0xFFFF0000u | (DWORD)model << 8;
        CHECK(g_createdShaders.size() >= 1 && std::count(g_createdShaders.begin(), g_createdShaders.end(), wanted) == 1, "MotionBlur=0: one copy shader of model %d made (%d made in all)", model, (int)g_createdShaders.size());
        clear();
        CHECK(SUCCEEDED(draw()), "draw with the game's shader afterwards");
        CHECK(ReadTarget(device, target, copy, pixels) && pixels[0] == kRed && pixels[kW * kH - 1] == kRed, "MotionBlur=0: afterwards the game's own shader draws (red)");
        // the second frame uses the same copy shader (nothing is made again)
        const size_t madeBefore = g_createdShaders.size();
        setUp();
        clear();
        CHECK(SUCCEEDED(MotionBlurPerEye(device, draw)) && ReadTarget(device, target, copy, pixels) && pixels[5 * kW + 7] == ((SourcePixel(7, 5) & 0x00FFFFFF) | 0xFF000000), "MotionBlur=0: the second frame copies too");
        CHECK(g_createdShaders.size() == madeBefore, "MotionBlur=0: the copy shader is made once, not every frame");

        // MotionBlur=0 but the bound shader is not known: the blur is drawn as the game does (once per eye)
        setUp();
        g_currentPsHandle = NULL;
        clear();
        draws = 0;
        hr = MotionBlurPerEye(device, countedDraw);
        CHECK(SUCCEEDED(hr) && draws == 2, "no shader handle: the blur is drawn once per eye (%d draws)", draws);
        CHECK(ReadTarget(device, target, copy, pixels) && pixels[0] == kRed && pixels[kW * kH - 1] == kRed, "no shader handle: the blur's shader drew");

        // MotionBlur=1: as before, once per eye, both halves painted, state back
        g_cfg.motionBlur = 1;
        setUp();
        clear();
        draws = 0;
        hr = MotionBlurPerEye(device, countedDraw);
        CHECK(SUCCEEDED(hr) && draws == 2, "MotionBlur=1: two draws, one per eye (%d)", draws);
        CHECK(ReadTarget(device, target, copy, pixels), "read the target");
        int notRed = 0;
        for (unsigned p : pixels)
            notRed += p != kRed;
        CHECK(notRed == 0, "MotionBlur=1: %d pixels not painted by the blur's shader", notRed);
        g_currentPsHandle = blurShader;
        settled("MotionBlur=1");

        device->SetPixelShader(NULL);
        blurShader->Release();
    }

    if (declaration)
        declaration->Release();
    for (IDirect3DVertexShader9* vertexShader : vertexShaders)
        if (vertexShader)
            vertexShader->Release();
    copy->Release();
    target->Release();
    targetTexture->Release();
    source->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    if (g_failures)
        printf("motion_blur_check: %d FAILURE(S)\n", g_failures);
    else
        printf("motion_blur_check: all checks passed\n");
    return g_failures ? 1 : 0;
}
