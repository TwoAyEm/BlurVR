// Checks the shared-texture transport primitives on this machine (no game needed) and times them against the readback path:
//   a Direct3D 9Ex device with an X8R8G8B8 back buffer cleared to a known colour, StretchRect into a shared A8R8G8B8 render
//   target, an event query to wait for the GPU, the texture opened on a Direct3D 11 device, a colour read back to check it,
//   and the same frame sent the old way (StretchRect, GetRenderTargetData, LockRect, copy).
//
//   python -m ziglang c++ -target x86-windows-gnu -O2 -std=c++17 tools/shared_transport_check.cpp -o shared_transport_check.exe
//       -ld3d11 -ldxgi -luser32
//   shared_transport_check.exe [width height]
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double Now()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char** argv)
{
    UINT width = argc > 2 ? (UINT)atoi(argv[1]) : 2160, height = argc > 2 ? (UINT)atoi(argv[2]) : 1071;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "shared_transport_check";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("shared_transport_check", "t", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
    typedef HRESULT(WINAPI * PFN)(UINT, IDirect3D9Ex**);
    PFN create9ex = (PFN)GetProcAddress(LoadLibraryA("d3d9.dll"), "Direct3DCreate9Ex");
    IDirect3D9Ex* d3d = NULL;
    if (!create9ex || FAILED(create9ex(D3D_SDK_VERSION, &d3d))) {
        printf("Direct3DCreate9Ex failed\n");
        return 1;
    }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9Ex* dev = NULL;
    HRESULT hr = d3d->CreateDeviceEx(0, D3DDEVTYPE_HAL, hwnd, 0x44, &pp, NULL, &dev);
    printf("CreateDeviceEx %ux%u -> 0x%08lx\n", width, height, (unsigned long)hr);
    if (FAILED(hr))
        return 1;

    IDirect3DSurface9* backBuffer = NULL;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

    // shared transport
    IDirect3DTexture9* shared9 = NULL;
    HANDLE handle = NULL;
    hr = dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shared9, &handle);
    printf("shared A8R8G8B8 render target -> 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    IDirect3DSurface9* sharedSurface = NULL;
    shared9->GetSurfaceLevel(0, &sharedSurface);
    IDirect3DQuery9* fence = NULL;
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &fence);

    ID3D11Device* d11 = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL level;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &d11,
                           &level, &ctx);
    printf("D3D11CreateDevice -> 0x%08lx\n", (unsigned long)hr);
    ID3D11Texture2D* shared11 = NULL;
    hr = d11->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void**)&shared11);
    printf("OpenSharedResource -> 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    D3D11_TEXTURE2D_DESC desc;
    shared11->GetDesc(&desc);
    printf("opened: %ux%u format %d bind 0x%x misc 0x%x\n", desc.Width, desc.Height, (int)desc.Format, desc.BindFlags, desc.MiscFlags);

    // a small staging texture to read the opened texture's colour back on the D3D11 side
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging11 = NULL;
    d11->CreateTexture2D(&sd, NULL, &staging11);

    // readback transport
    IDirect3DTexture9* stage9 = NULL;
    IDirect3DSurface9 *stageSurface = NULL, *readback = NULL;
    dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &stage9, NULL);
    stage9->GetSurfaceLevel(0, &stageSurface);
    dev->CreateOffscreenPlainSurface(width, height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
    D3D11_TEXTURE2D_DESC ud = desc;
    ud.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ud.MiscFlags = 0;
    ID3D11Texture2D* uploaded11 = NULL;
    d11->CreateTexture2D(&ud, NULL, &uploaded11);

    // colour check
    for (int round = 0; round < 3; round++) {
        D3DCOLOR color = D3DCOLOR_XRGB(30 + round * 60, 120, 200 - round * 50);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, color, 1.0f, 0);
        dev->StretchRect(backBuffer, NULL, sharedSurface, NULL, D3DTEXF_NONE);
        fence->Issue(D3DISSUE_END);
        while (fence->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        }
        ctx->CopyResource(staging11, shared11);
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(staging11, 0, D3D11_MAP_READ, 0, &m);
        const unsigned char* p = (const unsigned char*)m.pData + (height / 2) * m.RowPitch + (width / 2) * 4;
        printf("round %d: cleared B%u G%u R%u -> opened texture reads B%u G%u R%u A%u\n", round, color & 255, (color >> 8) & 255,
               (color >> 16) & 255, p[0], p[1], p[2], p[3]);
        ctx->Unmap(staging11, 0);
    }

    // timing: 200 frames each way; every frame clears the back buffer so the GPU has something to do first
    const int frames = 200;
    double t0 = Now();
    for (int i = 0; i < frames; i++) {
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(i & 255, 0, 0), 1.0f, 0);
        dev->StretchRect(backBuffer, NULL, sharedSurface, NULL, D3DTEXF_NONE);
        fence->Issue(D3DISSUE_END);
        while (fence->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        }
    }
    double sharedMs = (Now() - t0) / frames;
    t0 = Now();
    for (int i = 0; i < frames; i++) {
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(i & 255, 0, 0), 1.0f, 0);
        dev->StretchRect(backBuffer, NULL, stageSurface, NULL, D3DTEXF_NONE);
        dev->GetRenderTargetData(stageSurface, readback);
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(readback->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
            ctx->UpdateSubresource(uploaded11, 0, NULL, lr.pBits, lr.Pitch, 0);
            readback->UnlockRect();
            ctx->Flush();
        }
    }
    double readbackMs = (Now() - t0) / frames;
    printf("per frame at %ux%u (%.1f MB): shared texture %.2f ms, readback + upload %.2f ms\n", width, height,
           width * height * 4 / 1048576.0, sharedMs, readbackMs);
    return 0;
}
