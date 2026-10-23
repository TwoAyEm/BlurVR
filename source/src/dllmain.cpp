// d3d9.dll exports (forwarded to the real d3d9.dll) and DllMain.
#include "common.h"
#include "camera.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "display_modes.h"
#include "game_mode.h"
#include "perf_probe.h"
#include "video_settings.h"
#include "window_spoof.h"

// ----------------------------------------------------------------------------------------------
// d3d9.dll exports
// ----------------------------------------------------------------------------------------------
extern "C" {

typedef HRESULT(WINAPI* PFN_Create9Ex)(UINT, IDirect3D9Ex**);

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    LoadRealD3D9();
    if (!g_realDirect3DCreate9)
        return NULL;
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        InstallFileOpenHooks();
    }
    IDirect3D9* d3d = NULL;
    if (g_cfg.vr && g_cfg.sharedTransport) {
        // SharedTransport: the game gets an IDirect3D9Ex (same vtable layout for everything it calls) so that its device can be
        // an IDirect3DDevice9Ex, the only kind that can share textures with Direct3D 11 (see display_modes.cpp)
        PFN_Create9Ex create9Ex = g_realD3D9Module ? (PFN_Create9Ex)GetProcAddress(g_realD3D9Module, "Direct3DCreate9Ex") : NULL;
        IDirect3D9Ex* ex = NULL;
        if (create9Ex && SUCCEEDED(create9Ex(sdk, &ex)) && ex) {
            d3d = (IDirect3D9*)ex;
            g_d3dIsEx = true;
            Log("SharedTransport: Direct3DCreate9Ex(%u) -> %p", sdk, (void*)d3d);
        } else
            Log("SharedTransport: Direct3DCreate9Ex is not available, the frame goes through the CPU as before");
    }
    if (!d3d)
        d3d = g_realDirect3DCreate9(sdk);
    Log("Direct3DCreate9(%u) -> %p", sdk, (void*)d3d);
    InstallWindowSizeSpoof(true); // every module (DllMain patched the executable)
    if (d3d)
        HookDirect3D(d3d);
    return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    LoadRealD3D9();
    PFN_Create9Ex f = g_realD3D9Module ? (PFN_Create9Ex)GetProcAddress(g_realD3D9Module, "Direct3DCreate9Ex") : NULL;
    return f ? f(sdk, out) : E_FAIL;
}

static FARPROC RealProc(const char* n)
{
    LoadRealD3D9();
    return g_realD3D9Module ? GetProcAddress(g_realD3D9Module, n) : NULL;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n)
{
    typedef int(WINAPI * F)(D3DCOLOR, LPCWSTR);
    F f = (F)RealProc("D3DPERF_BeginEvent");
    return f ? f(c, n) : 0;
}
int WINAPI D3DPERF_EndEvent(void)
{
    typedef int(WINAPI * F)(void);
    F f = (F)RealProc("D3DPERF_EndEvent");
    return f ? f() : 0;
}
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n)
{
    typedef void(WINAPI * F)(D3DCOLOR, LPCWSTR);
    F f = (F)RealProc("D3DPERF_SetMarker");
    if (f)
        f(c, n);
}
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n)
{
    typedef void(WINAPI * F)(D3DCOLOR, LPCWSTR);
    F f = (F)RealProc("D3DPERF_SetRegion");
    if (f)
        f(c, n);
}
BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    typedef BOOL(WINAPI * F)(void);
    F f = (F)RealProc("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}
void WINAPI D3DPERF_SetOptions(DWORD o)
{
    typedef void(WINAPI * F)(DWORD);
    F f = (F)RealProc("D3DPERF_SetOptions");
    if (f)
        f(o);
}
DWORD WINAPI D3DPERF_GetStatus(void)
{
    typedef DWORD(WINAPI * F)(void);
    F f = (F)RealProc("D3DPERF_GetStatus");
    return f ? f() : 0;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH && reserved) // the process is exiting (not just this DLL being unloaded)
        VideoSettingsEnd();
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = (HMODULE)h;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(g_selfModule, g_gameDir, MAX_PATH);
        char* slash = strrchr(g_gameDir, '\\');
        if (slash)
            slash[1] = 0;
        snprintf(g_modDir, MAX_PATH, "%sBlurVR\\", g_gameDir);
        LoadConfig();
        g_modeOverride = g_cfg.startMode;
        g_matrixLayout = g_cfg.matrixLayoutOverride;
        g_perfActive = g_cfg.perfEnabled != 0;
        if (g_cfg.log) {
            std::string lp = std::string(g_modDir) + "BlurVR.log";
            // keep the log of the last real run: a launch that is closed a few seconds in must not erase it
            WIN32_FILE_ATTRIBUTE_DATA previous;
            if (GetFileAttributesExA(lp.c_str(), GetFileExInfoStandard, &previous) && previous.nFileSizeHigh == 0 &&
                previous.nFileSizeLow >= 20000)
                CopyFileA(lp.c_str(), (std::string(g_modDir) + "BlurVR.previous.log").c_str(), FALSE);
            strncpy(g_logPath, lp.c_str(), MAX_PATH - 1);
            FILE* truncate = fopen(lp.c_str(), "w");
            if (truncate) {
                fclose(truncate);
                g_logEnabled = true;
            }
        }
        Log("BlurVR %s proxy loaded. root=%s", BLURVR_VERSION, g_gameDir);
        if (!g_cfg.enabled)
            Log("Enabled=0 in the ini: the mod does nothing and the game runs as it would without it");
        else if (!g_cfg.stereo)
            Log("Stereo=0 in the ini: flat mode, SteamVR is not started and only the [Display] settings apply");
        VideoSettingsStart();          // the game's saved display mode, before the game reads it
        InstallWindowSizeSpoof(false); // the executable's imports, before the game looks at its window
    }
    return TRUE;
}
