// Extra display mode for the game's resolution list, and the IDirect3D9 hooks.
#include "display_modes.h"
#include <algorithm>
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "device_hooks.h"
#include "render_state.h"
#include "video_settings.h"
#include "vr_runtime.h"
#include "window_spoof.h"

// ---- the game's resolution list
// -----------------------------------------------------------------------------------------------------------------------------
// The game builds its resolution list from the adapter's display modes.  In VR the monitor's modes (16:9, 4:3 ...) mean nothing: the
// picture is two eyes side by side.  So the game is offered headset sizes instead: the recommended sizes of the README (the headset's
// panel per eye x 1.4 on each side, SteamVR's default supersampling, both eyes together).  The Resolution of BlurVR.ini is always in the
// list too, whatever it is: it is what the game starts at.  None of these is a mode of the monitor, so a full-screen request for one is
// made a window (AdjustPresentParams); only the headset sees the picture.
struct HeadsetSize {
    int width, height;
};
static const HeadsetSize kHeadsetSizes[] = {
    {2160, 1200}, // HTC Vive, Oculus Rift CV1 (1080x1200 per eye) at the panel's own size: the default, light
    {3024, 1680}, // HTC Vive, Oculus Rift CV1 (1080x1200)
    {3584, 2016}, // Oculus Rift S (1280x1440)
    {4032, 2240}, // Valve Index, HTC Vive Pro, Meta Quest 1 (1440x1600)
    {4936, 2740}, // Valve Index at the size SteamVR itself renders
    {5040, 2688}, // Meta Quest Pro (1800x1920)
    {5128, 2688}, // Meta Quest 2, Meta Quest 3S (1832x1920)
    {5780, 3092}, // Meta Quest 3 (2064x2208)
    {6048, 3024}, // Valve Steam Frame, HP Reverb G2, Pico 4 (2160x2160)
    {6856, 3428}, // HTC Vive Pro 2, HTC Vive Focus 3 (2448x2448)
    {7168, 3584}, // Bigscreen Beyond (2560x2560)
};
typedef UINT(STDMETHODCALLTYPE* PFN_ModeCount)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumModes)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
// Has the monitor itself (not the list the game is shown) a mode of this size?
static bool MonitorHasMode(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, UINT width, UINT height)
{
    UINT n = ((PFN_ModeCount)g_d3dOriginals[6])(d3d, adapter, fmt);
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m;
        if (SUCCEEDED(((PFN_EnumModes)g_d3dOriginals[7])(d3d, adapter, fmt, i, &m)) && m.Width == width && m.Height == height)
            return true;
    }
    return false;
}
// The list the game is shown in VR: every headset size once per refresh rate the monitor has (the game lists a size per rate), ascending.
static void BuildModeList(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, std::vector<D3DDISPLAYMODE>& out)
{
    out.clear();
    UINT n = ((PFN_ModeCount)g_d3dOriginals[6])(d3d, adapter, fmt);
    std::vector<UINT> refresh;
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m;
        if (FAILED(((PFN_EnumModes)g_d3dOriginals[7])(d3d, adapter, fmt, i, &m)))
            continue;
        if (std::find(refresh.begin(), refresh.end(), m.RefreshRate) == refresh.end())
            refresh.push_back(m.RefreshRate);
    }
    if (refresh.empty())
        refresh.push_back(60);
    std::vector<HeadsetSize> sizes(kHeadsetSizes, kHeadsetSizes + sizeof(kHeadsetSizes) / sizeof(kHeadsetSizes[0]));
    bool haveCurrent = false;
    for (const HeadsetSize& size : sizes)
        haveCurrent = haveCurrent || (size.width == g_cfg.vrWidth && size.height == g_cfg.vrHeight);
    if (!haveCurrent)
        sizes.push_back({g_cfg.vrWidth, g_cfg.vrHeight});
    std::sort(sizes.begin(), sizes.end(), [](const HeadsetSize& a, const HeadsetSize& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    std::sort(refresh.begin(), refresh.end());
    for (const HeadsetSize& size : sizes)
        for (UINT rate : refresh) {
            D3DDISPLAYMODE mode;
            mode.Width = (UINT)size.width;
            mode.Height = (UINT)size.height;
            mode.RefreshRate = rate;
            mode.Format = fmt;
            out.push_back(mode);
        }
}
static UINT STDMETHODCALLTYPE H_GetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt)
{
    if (!g_cfg.vr)
        return ((PFN_ModeCount)g_d3dOriginals[6])(d3d, adapter, fmt);
    std::vector<D3DDISPLAYMODE> l;
    BuildModeList(d3d, adapter, fmt, l);
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
    if (!g_cfg.vr || !out)
        return ((PFN_EnumModes)g_d3dOriginals[7])(d3d, adapter, fmt, mode, out);
    std::vector<D3DDISPLAYMODE> l;
    BuildModeList(d3d, adapter, fmt, l);
    if (mode >= l.size())
        return D3DERR_INVALIDCALL;
    *out = l[mode];
    return D3D_OK;
}
static IDirect3D9* g_direct3D = NULL;
bool g_desktopRectUsable = false;
bool g_desktopRectFailed = false;
// V-Sync and the desktop window for VR, and a full-screen request for a size the monitor has no mode of: make it windowed
void AdjustPresentParams(D3DPRESENT_PARAMETERS* pp, bool isReset)
{
    if (isReset && pp && g_cfg.vr && pp->BackBufferWidth && pp->BackBufferHeight && g_direct3D) {
        // a size the game offers, picked in its options: in VR it is the new Resolution.  A size it asks for for another reason (the
        // window was dragged, say) is not in its list and is left alone.
        std::vector<D3DDISPLAYMODE> offered;
        BuildModeList(g_direct3D, D3DADAPTER_DEFAULT, pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat,
                      offered);
        for (const D3DDISPLAYMODE& mode : offered)
            if (mode.Width == pp->BackBufferWidth && mode.Height == pp->BackBufferHeight) {
                VideoSettingsNoteMode(mode.Width, mode.Height);
                break;
            }
    }
    // In VR the headset sets the pace (WaitGetPoses right after Present), so the game must not wait for the monitor as well: with V-Sync on it
    // ran at the monitor's 60 Hz whatever the headset's refresh rate.  So V-Sync is always off in VR; flat, ForceNoVSync=1 turns it off and 0
    // leaves the game's own setting (the monitor's refresh rate with V-Sync on).
    if (pp && g_cfg.enabled && (g_cfg.vr || g_cfg.forceNoVsync) && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        Log("V-Sync forced off (%s; the game asked for presentation interval 0x%x)", g_cfg.vr ? "VR: the headset sets the frame rate" : "ForceNoVSync=1",
            (unsigned)pp->PresentationInterval);
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    if (pp && g_cfg.vr && g_cfg.desktopView) {
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
    // Full screen needs a mode the monitor has; every headset size is one it has not, so such a request can only be a window.
    if (!pp || !g_cfg.vr || pp->Windowed || !pp->BackBufferWidth || !pp->BackBufferHeight || !g_direct3D)
        return;
    D3DFORMAT f = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    if (MonitorHasMode(g_direct3D, D3DADAPTER_DEFAULT, f, pp->BackBufferWidth, pp->BackBufferHeight))
        return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    Log("%ux%u is not a display mode of the monitor -> windowed back buffer", pp->BackBufferWidth, pp->BackBufferHeight);
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
    if (g_cfg.vr && g_cfg.startAtSavedSize && pp->Windowed && haveSaved && !pp->BackBufferWidth && !pp->BackBufferHeight) {
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
        // Enabled=0 or Stereo=0 (flat): no deferred start, no worker thread, no VrInit, nothing that touches SteamVR
        if (g_cfg.vr) {
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
