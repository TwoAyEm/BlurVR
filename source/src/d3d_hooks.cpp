// Loading the real d3d9.dll, vtable patching, and the Direct3D function-pointer types the hooks use.
#include "d3d_hooks.h"
#include "core.h"

HMODULE g_realD3D9Module;
PFN_Direct3DCreate9 g_realDirect3DCreate9;

void LoadRealD3D9()
{
    if (g_realD3D9Module)
        return;
    // [General] ChainD3D9: another mod's d3d9.dll (renamed; relative to BlurVR\ or absolute) that is loaded in place of the system
    // one.  It forwards to the system d3d9 itself, so both mods see the game's calls.
    char chain[MAX_PATH] = "";
    GetPrivateProfileStringA("General", "ChainD3D9", "", chain, MAX_PATH, (std::string(g_modDir) + "BlurVR.ini").c_str());
    if (chain[0]) {
        std::string c = chain;
        if (c.find(':') == std::string::npos && c[0] != '\\')
            c = std::string(g_modDir) + c;
        g_realD3D9Module = LoadLibraryExA(c.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        Log("ChainD3D9: %s -> %p%s", c.c_str(), (void*)g_realD3D9Module, g_realD3D9Module ? "" : " (failed, using the system d3d9)");
    }
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    std::string p = std::string(sys) + "\\d3d9.dll";
    if (!g_realD3D9Module)
        g_realD3D9Module = LoadLibraryA(p.c_str());
    if (g_realD3D9Module)
        g_realDirect3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_realD3D9Module, "Direct3DCreate9");
    Log("real d3d9: %s -> %p", p.c_str(), (void*)g_realD3D9Module);
}

bool g_d3dIsEx = false;
bool g_deviceIsEx = false;
void* g_d3dOriginals[32];
void* g_deviceOriginals[128];

void PatchVtable(void** vt, int idx, void* hook, void** orig)
{
    if (vt[idx] == hook)
        return;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}
