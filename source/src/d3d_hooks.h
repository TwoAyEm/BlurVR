// Loading the real d3d9.dll, vtable patching, and the Direct3D function-pointer types the hooks use.
#pragma once
#include "common.h"

typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9)(UINT);

typedef HRESULT(STDMETHODCALLTYPE* PFN_SetShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);

typedef HRESULT(STDMETHODCALLTYPE* PFN_SetRenderState)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetScissorRect)(IDirect3DDevice9*, const RECT*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetClipPlane)(IDirect3DDevice9*, DWORD, const float*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                      IDirect3DTexture9**, HANDLE*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVolumeTexture)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT,
                                                            D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCubeTexture)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                          IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVertexBuffer)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                                           IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateIndexBuffer)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                          IDirect3DIndexBuffer9**, HANDLE*);

// SharedTransport: the IDirect3D9 the game got is really an IDirect3D9Ex, and its device an IDirect3DDevice9Ex (which has no
// managed pool and can share textures with Direct3D 11).
extern bool g_d3dIsEx;
extern bool g_deviceIsEx;

enum DeviceVtable { // IDirect3DDevice9 vtable indices
    VT_Reset = 16,
    VT_Present = 17,
    VT_CreateTexture = 23,
    VT_CreateVolumeTexture = 24,
    VT_CreateCubeTexture = 25,
    VT_CreateVertexBuffer = 26,
    VT_CreateIndexBuffer = 27,
    VT_SetRenderTarget = 37,
    VT_SetDepthStencilSurface = 39,
    VT_Clear = 43,
    VT_SetClipPlane = 55,
    VT_SetRenderState = 57,
    VT_SetScissorRect = 75,
    VT_SetViewport = 47,
    VT_SetTexture = 65,
    VT_DrawPrimitive = 81,
    VT_DrawIndexedPrimitive = 82,
    VT_DrawPrimitiveUP = 83,
    VT_DrawIndexedPrimitiveUP = 84,
    VT_CreateVertexShader = 91,
    VT_SetVertexShader = 92,
    VT_SetVertexShaderConstantF = 94,
    VT_CreatePixelShader = 106,
    VT_SetPixelShader = 107,
    VT_SetPixelShaderConstantF = 109,
};
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePixelShader)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);

// Call the original (unhooked) function stored for a vtable slot
#define ORIG(T, idx) ((T)g_deviceOriginals[idx])

// The genuine system d3d9.dll that every call is forwarded to.
extern HMODULE g_realD3D9Module;
// The genuine Direct3DCreate9.
extern PFN_Direct3DCreate9 g_realDirect3DCreate9;
// Load the genuine d3d9.dll from the system folder.
void LoadRealD3D9();
// Original IDirect3D9 methods, by vtable slot.
extern void* g_d3dOriginals[32];
// Original IDirect3DDevice9 methods, by vtable slot (see DeviceVtable).  Hooks call through these.
extern void* g_deviceOriginals[128];
// Replace vt[idx] with hook and store the previous entry in *orig.
void PatchVtable(void** vt, int idx, void* hook, void** orig);
