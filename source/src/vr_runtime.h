// OpenVR / SteamVR session: initialisation, head pose, per-eye camera deltas, floating menu screen, and submitting the
// finished frame to the compositor.
#pragma once
#include "common.h"
#include "openvr_wrapper.h"
#include "matrix.h"

typedef intptr_t (*PFN_VR_InitInternal)(EVRInitError*, EVRApplicationType);
typedef bool (*PFN_VR_IsHmdPresent)();
typedef intptr_t (*PFN_VR_GetGenericInterface)(const char*, EVRInitError*);

struct EyeBox {
    double left, right, up, down;
}; // tangents of the half angles

struct VrState {
    bool tried = false, active = false, headTrackingOn = true;
    HMODULE openvrDll = NULL;
    PFN_VR_GetGenericInterface getIface = NULL;
    struct VR_IVRSystem_FnTable* system = NULL;
    struct VR_IVRCompositor_FnTable* compositor = NULL;
    struct VR_IVROverlay_FnTable* overlay = NULL;
    struct VR_IVRApplications_FnTable* applications = NULL;
    struct VR_IVRChaperone_FnTable* chaperone = NULL; // for ResetZeroPose (recentre)
    VROverlayHandle_t menuOverlay = 0;
    bool menuOverlayShown = false, menuNeedsPlacement = true;
    unsigned long long menuPlaceUntilFrame = 0; // keep re-placing the floating screen until this frame (pose is
                                                // unreliable while the dashboard/theatre is up)
    ID3D11Device* d3d11Device = NULL;
    ID3D11DeviceContext* d3d11Context = NULL;
    ID3D11Texture2D* frameTexture = NULL;
    UINT frameTextureWidth = 0, frameTextureHeight = 0;
    ID3D11Texture2D* darkTexture = NULL;
    IDirect3DTexture9* stagingTexture = NULL;
    IDirect3DSurface9* desktopSource = NULL; // this race frame's finished side-by-side picture (the shared or the staging copy), for the desktop window
    UINT desktopSourceWidth = 0, desktopSourceHeight = 0;
    IDirect3DSurface9* stagingSurface = NULL;
    IDirect3DSurface9* readbackSurface = NULL;
    UINT stagingWidth = 0, stagingHeight = 0;
    D3DFORMAT stagingFormat = D3DFMT_UNKNOWN;
    // SharedTransport (a Direct3D 9Ex device): race frames reach SteamVR as a texture shared with Direct3D 11, never through the CPU
    IDirect3DTexture9* sharedTexture9 = NULL;
    IDirect3DSurface9* sharedSurface9 = NULL;
    IDirect3DQuery9* sharedFence = NULL;
    ID3D11Texture2D* sharedTexture11 = NULL;
    UINT sharedWidth = 0, sharedHeight = 0;
    bool sharedFailed = false;
    unsigned long long sharedFrames = 0;
    EyeBox eyeBox[2], unionBox;
    VRTextureBounds_t monoBounds[2];
    double ipd = 0.063;
    bool poseValid = false, recenterPending = false;
    Matrix4 eyeCameraDelta[3]; // camera-space delta per eye (0 left, 1 right, 2 centre), row-vector
    double refYaw = 0, refPos[3] = {0, 0, 0}; // head pose that counts as "straight ahead" (zero when SteamVR does the recentring)
    double hmdPos[3] = {0, 1.5, 0}, hmdFwd[3] = {0, 0, -1}; // tracking space (right-handed), for the menu screen
    unsigned long long frameCount = 0, submitErrs = 0;
};

extern VrState g_vr;
extern bool g_vrStartDeferred;
extern DWORD g_vrDeferredSince;
extern unsigned g_preVrSceneDraws;
extern unsigned g_preVrSceneFrames;
extern unsigned g_sceneDrawsThisFrame;
void ReleaseStagingResources();
void ReleaseSharedTransport();
bool VrInit();
void VrUpdatePose();
void VrSubmitFrame(IDirect3DDevice9* device);
// Before the real Present of a race frame: paint one eye's view into the back buffer so the window on the desktop shows that instead of
// the whole side-by-side picture.  The headset got its copy in VrSubmitFrame, which is over by then.  True: the view was painted at the
// window's own size into the corner of the back buffer, and *presented is the rectangle to present (Present's source rectangle); false:
// it fills the whole back buffer and the whole back buffer is presented.
bool VrShowEyeOnDesktop(IDirect3DDevice9* device, RECT* presented);
DWORD WINAPI VrInitThread(LPVOID);
void StartVrInitThread();
DWORD WINAPI VrInitThread(LPVOID);
