// Extra display mode for the game's resolution list, and the IDirect3D9 hooks.
#include "display_modes.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "device_hooks.h"
#include "render_state.h"
#include "vr_runtime.h"
#include "window_spoof.h"

// ---- extra display mode
// -----------------------------------------------------------------------------------------------------------------------------
// The game builds its resolution list from the adapter's display modes. Offer one more (CustomWidth x CustomHeight) so
// it can be picked in the game's options. A size the monitor does not really have cannot be a full-screen mode, so such
// a device is created as a window instead (only the headset sees it).
typedef UINT(STDMETHODCALLTYPE* PFN_ModeCount)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumModes)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
static void BuildModeList(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, std::vector<D3DDISPLAYMODE>& out,
                          bool* hadCustom)
{
    out.clear();
    if (hadCustom)
        *hadCustom = false;
    UINT n = ((PFN_ModeCount)g_d3dOriginals[6])(d3d, adapter, fmt);
    std::vector<UINT> refresh;
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m;
        if (FAILED(((PFN_EnumModes)g_d3dOriginals[7])(d3d, adapter, fmt, i, &m)))
            continue;
        out.push_back(m);
        if (m.Width == (UINT)g_cfg.customWidth && m.Height == (UINT)g_cfg.customHeight && hadCustom)
            *hadCustom = true;
        bool seen = false;
        for (UINT r : refresh)
            if (r == m.RefreshRate)
                seen = true;
        if (!seen)
            refresh.push_back(m.RefreshRate);
    }
    if (g_cfg.customWidth > 0 && g_cfg.customHeight > 0 && !(hadCustom && *hadCustom)) {
        if (refresh.empty())
            refresh.push_back(60);
        for (UINT r : refresh) {
            D3DDISPLAYMODE c;
            c.Width = g_cfg.customWidth;
            c.Height = g_cfg.customHeight;
            c.RefreshRate = r;
            c.Format = fmt;
            out.push_back(c);
        }
        // keep the usual ascending order
        for (size_t i = 1; i < out.size(); i++)
            for (size_t j = i; j > 0; j--) {
                const D3DDISPLAYMODE &a = out[j - 1], &b = out[j];
                bool gt = a.Width > b.Width ||
                          (a.Width == b.Width &&
                           (a.Height > b.Height || (a.Height == b.Height && a.RefreshRate > b.RefreshRate)));
                if (gt)
                    std::swap(out[j - 1], out[j]);
                else
                    break;
            }
    }
}
static UINT STDMETHODCALLTYPE H_GetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt)
{
    if (g_cfg.customWidth <= 0 || g_cfg.customHeight <= 0)
        return ((PFN_ModeCount)g_d3dOriginals[6])(d3d, adapter, fmt);
    std::vector<D3DDISPLAYMODE> l;
    BuildModeList(d3d, adapter, fmt, l, NULL);
    static int n = 0;
    if (n++ < 3) {
        std::string s;
        char b[48];
        for (const D3DDISPLAYMODE& m : l) {
            snprintf(b, sizeof(b), " %ux%u@%u", m.Width, m.Height, m.RefreshRate);
            s += b;
        }
        Log("display modes offered to the game (fmt %d):%s", (int)fmt, s.c_str());
    }
    return (UINT)l.size();
}
static HRESULT STDMETHODCALLTYPE H_EnumAdapterModes(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, UINT mode,
                                                    D3DDISPLAYMODE* out)
{
    if (g_cfg.customWidth <= 0 || g_cfg.customHeight <= 0 || !out)
        return ((PFN_EnumModes)g_d3dOriginals[7])(d3d, adapter, fmt, mode, out);
    std::vector<D3DDISPLAYMODE> l;
    BuildModeList(d3d, adapter, fmt, l, NULL);
    if (mode >= l.size())
        return D3DERR_INVALIDCALL;
    *out = l[mode];
    return D3D_OK;
}
static IDirect3D9* g_direct3D = NULL;
bool g_desktopRectUsable = false;
bool g_desktopRectFailed = false;
// a request for the custom size on a monitor that has no such mode: make it windowed
void AdjustPresentParams(D3DPRESENT_PARAMETERS* pp)
{
    if (pp && g_cfg.enabled && g_cfg.forceNoVsync && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        // The headset sets the pace (WaitGetPoses right after Present), so the game must not wait for the monitor as well:
        // with V-Sync on it ran at the monitor's 60 Hz whatever the headset's refresh rate.
        Log("V-Sync forced off (the game asked for presentation interval 0x%x)", (unsigned)pp->PresentationInterval);
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    if (pp && g_cfg.enabled && g_cfg.desktopView) {
        // Present takes a source rectangle only with the COPY swap effect (one back buffer, no multisampling), and a rectangle the size
        // of the window is what the desktop mirror presents, instead of the whole 4K back buffer
        g_desktopRectUsable = false;
        if (pp->Windowed && pp->MultiSampleType == D3DMULTISAMPLE_NONE) {
            if (pp->SwapEffect != D3DSWAPEFFECT_COPY || pp->BackBufferCount > 1) {
                Log("swap effect %d with %u back buffers -> copy with one (the desktop window presents a piece of the back buffer)",
                    (int)pp->SwapEffect, (unsigned)pp->BackBufferCount);
                pp->SwapEffect = D3DSWAPEFFECT_COPY;
                pp->BackBufferCount = 1;
            }
            g_desktopRectUsable = true;
        }
    }
    if (!pp || g_cfg.customWidth <= 0 || g_cfg.customHeight <= 0 || !g_direct3D)
        return;
    if (pp->Windowed || pp->BackBufferWidth != (UINT)g_cfg.customWidth ||
        pp->BackBufferHeight != (UINT)g_cfg.customHeight)
        return;
    std::vector<D3DDISPLAYMODE> l;
    bool had = false;
    D3DFORMAT f = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    BuildModeList(g_direct3D, D3DADAPTER_DEFAULT, f, l, &had);
    if (had)
        return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    Log("custom resolution %ux%u is not a real display mode -> windowed back buffer", g_cfg.customWidth,
        g_cfg.customHeight);
}

// What the game asks for (see window_spoof.h for why a clamped window matters).  A windowed device with no size of its own gets the saved video
// mode; a size the game passes is its own business: the game builds its render targets from it, not from the device.
static void LogCreateDeviceRequest(HWND window, D3DPRESENT_PARAMETERS* pp)
{
    RECT client = {0, 0, 0, 0};
    if (window)
        GetClientRect(window, &client);
    UINT savedWidth = 0, savedHeight = 0;
    bool haveSaved = ReadSavedVideoSize(&savedWidth, &savedHeight);
    Log("CreateDevice request: back buffer %ux%u, windowed %d, swap effect %d, multisample %d, back buffers %u, window client %ldx%ld, "
        "saved video mode %ux%u",
        pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->Windowed, (int)pp->SwapEffect, (int)pp->MultiSampleType,
        (unsigned)pp->BackBufferCount, client.right, client.bottom, savedWidth, savedHeight);
    if (g_cfg.startAtSavedSize && pp->Windowed && haveSaved && !pp->BackBufferWidth && !pp->BackBufferHeight) {
        pp->BackBufferWidth = savedWidth;
        pp->BackBufferHeight = savedHeight;
        Log("windowed start without a size: the back buffer takes the saved video mode %ux%u", savedWidth, savedHeight);
    }
    WindowSpoofWindowed(pp->Windowed != 0);
    if (pp->Windowed)
        WindowSpoofIntendedSize(pp->BackBufferWidth, pp->BackBufferHeight);
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                     IDirect3DDevice9**);
static HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                                D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    g_direct3D = d3d;
    g_deviceIsEx = false;
    if (pp && g_cfg.enabled)
        LogCreateDeviceRequest(wnd, pp);
    AdjustPresentParams(pp);
    HRESULT hr = D3DERR_INVALIDCALL;
    if (g_d3dIsEx && pp && out) {
        // SharedTransport: an Ex device (slot 20 of the IDirect3D9Ex vtable).  The game cannot tell, but it has no managed pool
        // (device_hooks.cpp turns those resources into default-pool ones) and it can share textures.
        typedef HRESULT(STDMETHODCALLTYPE * PFN_CreateDeviceEx)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                                D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
        D3DPRESENT_PARAMETERS saved = *pp;
        D3DDISPLAYMODEEX mode;
        memset(&mode, 0, sizeof(mode));
        mode.Size = sizeof(mode);
        mode.Width = pp->BackBufferWidth;
        mode.Height = pp->BackBufferHeight;
        mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
        mode.Format = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
        mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        IDirect3DDevice9Ex* exDevice = NULL;
        hr = ((PFN_CreateDeviceEx)(*(void***)d3d)[20])((IDirect3D9Ex*)d3d, adapter, type, wnd, flags, pp,
                                                       pp->Windowed ? NULL : &mode, &exDevice);
        if (SUCCEEDED(hr) && exDevice) {
            *out = (IDirect3DDevice9*)exDevice;
            g_deviceIsEx = true;
            Log("SharedTransport: CreateDeviceEx -> %p (an Ex device; managed resources become default-pool ones)",
                (void*)exDevice);
        } else {
            Log("SharedTransport: CreateDeviceEx failed 0x%08x, using a plain device (the frame goes through the CPU)",
                (unsigned)hr);
            *pp = saved;
            hr = D3DERR_INVALIDCALL;
        }
    }
    if (!g_deviceIsEx)
        hr = ((PFN_CreateDevice)g_d3dOriginals[16])(d3d, adapter, type, wnd, flags, pp, out);
    WindowSpoofDeviceCreated(); // the game has its device: it sees its window as it is again
    Log("CreateDevice -> 0x%08x (flags 0x%x, bb %ux%u fmt %d, windowed %d)", (unsigned)hr, (unsigned)flags,
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? (int)pp->BackBufferFormat : 0,
        pp ? (int)pp->Windowed : 0);
    if (SUCCEEDED(hr) && out && *out) {
        HookDevice(*out);
        g_rtWidth = g_rtHeight = 0;
        RefreshRenderTargetState(*out);
        if (g_cfg.deferVrStart) {
            g_vrStartDeferred = true;
            g_vrDeferredSince = GetTickCount();
            Log("VR start deferred until the first level begins loading");
        } else if (g_cfg.threadedVrInit) {
            // Start VR on a worker thread: the game stalls for ~0.75 s while it loads the intro videos right after
            // device creation, and SteamVR flags a running scene app that misses that long as "not responding" and
            // throws up the dashboard. This way we only become a running VR app once the game is presenting steadily.
            static bool started = false;
            if (!started) {
                started = true;
                CreateThread(NULL, 0, VrInitThread, NULL, 0, NULL);
            }
        } else if (VrInit()) {
            VrUpdatePose();
            g_vr.active = true;
            Log("VR active");
        }
    }
    return hr;
}

void HookDirect3D(IDirect3D9* d3d)
{
    void** vt = *(void***)d3d;
    PatchVtable(vt, 16, (void*)H_CreateDevice, &g_d3dOriginals[16]);
    PatchVtable(vt, 6, (void*)H_GetAdapterModeCount, &g_d3dOriginals[6]);
    PatchVtable(vt, 7, (void*)H_EnumAdapterModes, &g_d3dOriginals[7]);
}
