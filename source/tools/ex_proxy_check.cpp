// Loads the built d3d9.dll proxy the way the game does, with SharedTransport=1, and exercises what the game will ask of an Ex device:
// managed textures, cube and volume textures, vertex and index buffers (they must come back working although an Ex device has no
// managed pool), locks, drawing, Present, Reset.  The proxy's BlurVR.log (in a BlurVR folder next to it) shows the Ex messages.
//
//   mkdir proxy\BlurVR ; copy d3d9.dll proxy ; write proxy\BlurVR\BlurVR.ini with  [General] Enabled=1 Log=1  [Render] SharedTransport=1
//   python -m ziglang c++ -target x86-windows-gnu -O1 tools/ex_proxy_check.cpp -o ex_proxy_check.exe -luser32
//   ex_proxy_check.exe proxy\d3d9.dll
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

// A texture loader that fills every mip level assuming the tight row pitch (width * 4 bytes), the way a managed texture allowed.
// Each level gets its own solid colour; the level is then drawn minified so the sampler picks it, and the colour is read back.
static int CheckTightPitchFill(IDirect3DDevice9* dev, D3DPOOL pool, DWORD usage, const char* label)
{
    IDirect3DTexture9* t = NULL;
    if (FAILED(dev->CreateTexture(256, 256, 0, usage, D3DFMT_A8R8G8B8, pool, &t, NULL))) {
        printf("%s: CreateTexture failed\n", label);
        return -1;
    }
    UINT n = t->GetLevelCount();
    for (UINT l = 0; l < n; l++) {
        D3DLOCKED_RECT lr;
        D3DSURFACE_DESC d;
        t->GetLevelDesc(l, &d);
        t->LockRect(l, &lr, NULL, 0);
        DWORD color = 0xFF000000 | ((40 + l * 30) << 16) | ((200 - l * 20) << 8) | (20 + l * 10);
        DWORD* out = (DWORD*)lr.pBits;
        for (UINT i = 0; i < d.Width * d.Height; i++) // contiguous, as if the pitch were width * 4
            out[i] = color;
        t->UnlockRect(l);
    }
    IDirect3DSurface9 *rt = NULL, *old = NULL, *sys = NULL;
    dev->CreateRenderTarget(300, 300, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->CreateOffscreenPlainSurface(300, 300, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    dev->GetRenderTarget(0, &old);
    dev->SetRenderTarget(0, rt);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    int wrong = 0;
    printf("%s, tight-pitch fill:", label);
    for (UINT l = 0; l < n && l < 8; l++) {
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1, 0);
        float size = (float)(256 >> l);
        struct V { float x, y, z, w, u, v; } q[4] = {{100 - 0.5f, 100 - 0.5f, 0, 1, 0, 0}, {100 + size - 0.5f, 100 - 0.5f, 0, 1, 1, 0},
                                                      {100 - 0.5f, 100 + size - 0.5f, 0, 1, 0, 1}, {100 + size - 0.5f, 100 + size - 0.5f, 0, 1, 1, 1}};
        dev->SetTexture(0, t);
        dev->BeginScene();
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        dev->EndScene();
        dev->GetRenderTargetData(rt, sys);
        D3DLOCKED_RECT lr;
        sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
        DWORD px = *(DWORD*)((BYTE*)lr.pBits + (100 + (int)size / 2) * lr.Pitch + (100 + (int)size / 2) * 4);
        sys->UnlockRect();
        DWORD want = 0xFF000000 | ((40 + l * 30) << 16) | ((200 - l * 20) << 8) | (20 + l * 10);
        printf(" L%u %s", l, px == want ? "ok" : "WRONG");
        wrong += px != want;
    }
    printf("\n");
    dev->SetRenderTarget(0, old);
    return wrong;
}

// The same for a managed volume texture (a colour grading look-up table, say): every level filled assuming tight rows and slices.
static int CheckVolumeFill(IDirect3DDevice9* dev, D3DPOOL pool, DWORD usage, const char* label)
{
    IDirect3DVolumeTexture9* v = NULL;
    if (FAILED(dev->CreateVolumeTexture(32, 32, 32, 0, usage, D3DFMT_A8R8G8B8, pool, &v, NULL))) {
        printf("%s: CreateVolumeTexture failed\n", label);
        return -1;
    }
    UINT n = v->GetLevelCount();
    for (UINT l = 0; l < n; l++) {
        D3DLOCKED_BOX box;
        D3DVOLUME_DESC d;
        v->GetLevelDesc(l, &d);
        v->LockBox(l, &box, NULL, 0);
        DWORD color = 0xFF000000 | ((40 + l * 30) << 16) | ((200 - l * 20) << 8) | (20 + l * 10);
        DWORD* out = (DWORD*)box.pBits;
        for (UINT i = 0; i < d.Width * d.Height * d.Depth; i++)
            out[i] = color;
        v->UnlockBox(l);
    }
    IDirect3DSurface9 *rt = NULL, *old = NULL, *sys = NULL;
    dev->CreateRenderTarget(300, 300, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->CreateOffscreenPlainSurface(300, 300, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    dev->GetRenderTarget(0, &old);
    dev->SetRenderTarget(0, rt);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
    int wrong = 0;
    printf("%s, tight-pitch volume fill:", label);
    for (UINT l = 0; l < n && l < 6; l++) {
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1, 0);
        float size = (float)(32 >> l);
        struct V { float x, y, z, w, u, v, t; } q[4] = {{100 - 0.5f, 100 - 0.5f, 0, 1, 0, 0, 0.5f}, {100 + size - 0.5f, 100 - 0.5f, 0, 1, 1, 0, 0.5f},
                                                        {100 - 0.5f, 100 + size - 0.5f, 0, 1, 0, 1, 0.5f}, {100 + size - 0.5f, 100 + size - 0.5f, 0, 1, 1, 1, 0.5f}};
        dev->SetTexture(0, v);
        dev->BeginScene();
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        dev->EndScene();
        dev->GetRenderTargetData(rt, sys);
        D3DLOCKED_RECT lr;
        sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
        DWORD px = *(DWORD*)((BYTE*)lr.pBits + (100 + (int)size / 2) * lr.Pitch + (100 + (int)size / 2) * 4);
        sys->UnlockRect();
        DWORD want = 0xFF000000 | ((40 + l * 30) << 16) | ((200 - l * 20) << 8) | (20 + l * 10);
        printf(" L%u %s", l, px == want ? "ok" : "WRONG");
        wrong += px != want;
    }
    printf("\n");
    dev->SetRenderTarget(0, old);
    return wrong;
}

int main(int argc, char** argv)
{
    HMODULE m = LoadLibraryA(argv[1]);
    typedef IDirect3D9*(WINAPI * C)(UINT);
    C create = (C)GetProcAddress(m, "Direct3DCreate9");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "t";
    RegisterClassA(&wc);
    // "big" as the third argument: a window far bigger than the monitor, asked for at creation and again with SetWindowPos (FitDesktopWindow
    // makes both fit the monitor's work area)
    bool big = argc > 2 && !strcmp(argv[2], "big");
    HWND hwnd = CreateWindowA("t", "t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, big ? 5000 : 800, big ? 3000 : 600, NULL, NULL, wc.hInstance, NULL);
    if (big) {
        RECT r;
        GetWindowRect(hwnd, &r);
        printf("window asked for 5000x3000 is %ldx%ld\n", r.right - r.left, r.bottom - r.top);
        SetWindowPos(hwnd, NULL, 0, 0, 6000, 4000, SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(hwnd, &r);
        printf("after SetWindowPos 6000x4000 it is %ldx%ld\n", r.right - r.left, r.bottom - r.top);
    }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferWidth = 800; pp.BackBufferHeight = 600;
    pp.hDeviceWindow = hwnd; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, 0x44, &pp, &dev);
    printf("CreateDevice via proxy -> 0x%08lx\n", hr);
    IDirect3DDevice9Ex* ex = NULL;
    printf("device is Ex: %s\n", SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex)) ? "yes" : "no");
    IDirect3DTexture9* t = NULL;
    printf("managed texture -> 0x%08lx\n", dev->CreateTexture(256, 256, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL));
    D3DLOCKED_RECT lr;
    printf("  lock level 0 -> 0x%08lx\n", t->LockRect(0, &lr, NULL, 0)); memset(lr.pBits, 0x80, 16); t->UnlockRect(0);
    printf("  lock level 3 -> 0x%08lx\n", t->LockRect(3, &lr, NULL, 0)); t->UnlockRect(3);
    D3DSURFACE_DESC d; t->GetLevelDesc(0, &d); printf("  pool reported: %d\n", (int)d.Pool);
    IDirect3DTexture9* dxt = NULL;
    printf("managed DXT5 -> 0x%08lx\n", dev->CreateTexture(512, 512, 0, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &dxt, NULL));
    IDirect3DCubeTexture9* cube = NULL;
    printf("managed cube -> 0x%08lx\n", dev->CreateCubeTexture(64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL));
    IDirect3DVolumeTexture9* vol = NULL;
    printf("managed volume -> 0x%08lx\n", dev->CreateVolumeTexture(16, 16, 16, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vol, NULL));
    IDirect3DVertexBuffer9* vb = NULL; void* p = NULL;
    printf("managed VB -> 0x%08lx\n", dev->CreateVertexBuffer(4096, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, NULL));
    printf("  VB lock -> 0x%08lx\n", vb->Lock(0, 0, &p, 0)); vb->Unlock();
    IDirect3DIndexBuffer9* ib = NULL;
    printf("managed IB -> 0x%08lx\n", dev->CreateIndexBuffer(4096, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL));
    printf("  IB lock -> 0x%08lx\n", ib->Lock(0, 0, &p, 0)); ib->Unlock();
    // render something and present a few frames
    int wrongLevels = CheckTightPitchFill(dev, D3DPOOL_MANAGED, 0, "managed texture through the proxy");
    printf("tight-pitch fill: %s\n", wrongLevels == 0 ? "every level correct" : "SOME LEVELS WRONG");
    int wrongVolume = CheckVolumeFill(dev, D3DPOOL_MANAGED, 0, "managed volume through the proxy");
    printf("tight-pitch volume fill: %s\n", wrongVolume == 0 ? "every level correct" : "SOME LEVELS WRONG");
    HRESULT last = S_OK;
    for (int i = 0; i < 30; i++) {
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(i * 8, 20, 90), 1.0f, 0);
        dev->BeginScene();
        dev->SetTexture(0, t);
        float tri[3][5] = {{100, 100, 0.5f, 1, 0}, {300, 100, 0.5f, 1, 0}, {200, 300, 0.5f, 1, 0}};
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
        dev->EndScene();
        last = dev->Present(NULL, NULL, NULL, NULL);
    }
    printf("Present x30, last -> 0x%08lx\n", last);
    printf("TestCooperativeLevel -> 0x%08lx\n", dev->TestCooperativeLevel());
    D3DPRESENT_PARAMETERS pp2 = pp; pp2.BackBufferWidth = 640; pp2.BackBufferHeight = 480;
    printf("Reset -> 0x%08lx\n", dev->Reset(&pp2));
    dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    printf("Present after Reset -> 0x%08lx\n", dev->Present(NULL, NULL, NULL, NULL));
    { // the desktop mirror presents a piece of the back buffer: the proxy asks for the COPY swap effect, which is what allows a source rectangle
        RECT piece = {0, 0, 400, 300};
        printf("Present with a source rectangle -> 0x%08lx\n", dev->Present(&piece, NULL, NULL, NULL));
    }
    return 0;
}
