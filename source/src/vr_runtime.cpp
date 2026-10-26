// OpenVR / SteamVR session: initialisation, head pose, per-eye camera deltas, floating menu screen, and submitting the finished frame to the compositor.
// With [VR] Api=openxr the runtime calls go through openxr_runtime.cpp instead; the maths and the game side are the same.
#include "vr_runtime.h"
#include "config.h"
#include "d3d_hooks.h"
#include "display_modes.h"
#include "core.h"
#include "draw_dispatch.h"
#include "fps_counter.h"
#include "frame_dump.h"
#include "game_mode.h"
#include "hud.h"
#include "lens_model.h"
#include "openxr_runtime.h"
#include "perf_probe.h"
#include "streaks.h"

VrState g_vr;
bool g_vrStartDeferred = false;
DWORD g_vrDeferredSince = 0;
unsigned g_preVrSceneDraws = 0;
unsigned g_preVrSceneFrames = 0;

unsigned g_sceneDrawsThisFrame = 0;
static bool g_prevKeyDown[3] = {false, false, false};

void ReleaseStagingResources()
{
    if (g_vr.readbackSurface) {
        g_vr.readbackSurface->Release();
        g_vr.readbackSurface = NULL;
    }
    if (g_vr.stagingSurface) {
        g_vr.stagingSurface->Release();
        g_vr.stagingSurface = NULL;
    }
    if (g_vr.stagingTexture) {
        g_vr.stagingTexture->Release();
        g_vr.stagingTexture = NULL;
    }
    g_vr.stagingWidth = g_vr.stagingHeight = 0;
}
static bool EnsureStagingResources(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT fmt)
{
    if (g_vr.stagingTexture && g_vr.stagingWidth == w && g_vr.stagingHeight == h && g_vr.stagingFormat == fmt)
        return true;
    ReleaseStagingResources();
    HRESULT hr =
        device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_vr.stagingTexture, NULL);
    if (FAILED(hr)) {
        Log("CreateTexture stage failed 0x%08x", (unsigned)hr);
        ReleaseStagingResources();
        return false;
    }
    g_vr.stagingTexture->GetSurfaceLevel(0, &g_vr.stagingSurface);
    hr = device->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &g_vr.readbackSurface, NULL);
    if (FAILED(hr)) {
        Log("CreateOffscreenPlainSurface failed 0x%08x", (unsigned)hr);
        ReleaseStagingResources();
        return false;
    }
    g_vr.stagingWidth = w;
    g_vr.stagingHeight = h;
    g_vr.stagingFormat = fmt;
    Log("stage resources %ux%u fmt %d", w, h, (int)fmt);
    return true;
}
void ReleaseSharedTransport()
{
    if (g_vr.sharedTexture11) {
        g_vr.sharedTexture11->Release();
        g_vr.sharedTexture11 = NULL;
    }
    if (g_vr.sharedFence) {
        g_vr.sharedFence->Release();
        g_vr.sharedFence = NULL;
    }
    if (g_vr.sharedSurface9) {
        g_vr.sharedSurface9->Release();
        g_vr.sharedSurface9 = NULL;
    }
    if (g_vr.sharedTexture9) {
        g_vr.sharedTexture9->Release();
        g_vr.sharedTexture9 = NULL;
    }
    g_vr.sharedWidth = g_vr.sharedHeight = 0;
}
static bool SharedTransportFailed(const char* what, HRESULT hr)
{
    Log("SharedTransport: %s failed 0x%08x - back to the copy through the CPU", what, (unsigned)hr);
    ReleaseSharedTransport();
    g_vr.sharedFailed = true;
    return false;
}
// The shared texture: an A8R8G8B8 render target on the game's (Ex) device that the Direct3D 11 device SteamVR uses opens by
// its shared handle.  Rebuilt when the back buffer changes size.
static bool EnsureSharedTransport(IDirect3DDevice9* device, UINT w, UINT h)
{
    if (g_vr.sharedTexture11 && g_vr.sharedWidth == w && g_vr.sharedHeight == h)
        return true;
    if (g_vr.sharedFailed || !g_vr.d3d11Device)
        return false;
    ReleaseSharedTransport();
    HANDLE handle = NULL;
    HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                       &g_vr.sharedTexture9, &handle);
    if (FAILED(hr) || !handle)
        return SharedTransportFailed("creating the shared render target", hr);
    g_vr.sharedTexture9->GetSurfaceLevel(0, &g_vr.sharedSurface9);
    hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &g_vr.sharedFence);
    if (FAILED(hr))
        return SharedTransportFailed("creating the fence", hr);
    hr = g_vr.d3d11Device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void**)&g_vr.sharedTexture11);
    if (FAILED(hr))
        return SharedTransportFailed("opening the texture in Direct3D 11 (different graphics adapters?)", hr);
    D3D11_TEXTURE2D_DESC opened;
    g_vr.sharedTexture11->GetDesc(&opened);
    g_vr.sharedWidth = w;
    g_vr.sharedHeight = h;
    Log("SharedTransport: shared texture %ux%u ready (Direct3D 11 format %d, bind flags 0x%x, misc 0x%x)", opened.Width,
        opened.Height, (int)opened.Format, opened.BindFlags, opened.MiscFlags);
    return true;
}
// Copy the finished frame into the shared texture on the GPU and wait until the GPU is done with it (an event query), so the
// compositor never reads a half-written frame.  False = use the copy through the CPU for this frame.
static bool CopyFrameShared(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, const D3DSURFACE_DESC& desc)
{
    if (!EnsureSharedTransport(device, desc.Width, desc.Height))
        return false;
    PerfSetPhase(device, PH_COPY);
    HRESULT hr;
    {
        PerfScope perfCopy(PS_READBACK);
        hr = device->StretchRect(backBuffer, NULL, g_vr.sharedSurface9, NULL, D3DTEXF_NONE);
        if (SUCCEEDED(hr))
            hr = g_vr.sharedFence->Issue(D3DISSUE_END);
    }
    if (FAILED(hr)) {
        static int failures = 0;
        if (failures++ < 5)
            Log("SharedTransport: StretchRect into the shared texture failed 0x%08x", (unsigned)hr);
        return false;
    }
    PerfScope perfWait(PS_UPLOAD);
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    HRESULT state;
    for (unsigned spins = 0; (state = g_vr.sharedFence->GetData(NULL, 0, D3DGETDATA_FLUSH)) == S_FALSE; spins++) {
        YieldProcessor();
        if ((spins & 255) == 255) {
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - start.QuadPart) * 1000 > 200 * frequency.QuadPart) { // the GPU is not answering
                static int timeouts = 0;
                if (timeouts++ < 5)
                    Log("SharedTransport: the GPU did not finish the frame within 200 ms");
                return false;
            }
        }
    }
    if (state != S_OK)
        return false;
    if (g_vr.sharedFrames++ == 0)
        Log("SharedTransport: first race frame handed over as a shared texture");
    return true;
}

static bool EnsureFrameTexture(UINT w, UINT h)
{
    if (g_vr.frameTexture && g_vr.frameTextureWidth == w && g_vr.frameTextureHeight == h)
        return true;
    if (g_vr.frameTexture) {
        g_vr.frameTexture->Release();
        g_vr.frameTexture = NULL;
    }
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = g_vr.d3d11Device->CreateTexture2D(&td, NULL, &g_vr.frameTexture);
    if (FAILED(hr)) {
        Log("D3D11 CreateTexture2D failed 0x%08x", (unsigned)hr);
        return false;
    }
    g_vr.frameTextureWidth = w;
    g_vr.frameTextureHeight = h;
    Log("D3D11 frame texture %ux%u", w, h);
    return true;
}

// GetEyeToHeadTransform returns a 48-byte struct by value: on 32-bit x86 the caller passes a hidden pointer to the result as
// the first argument (the function hands it back in EAX), so it is called with that pointer spelled out.
typedef HmdMatrix34_t*(__stdcall* PFN_EyeToHeadRaw)(HmdMatrix34_t*, EVREye);

// One eye's eye-to-head transform, from the API in use (OpenXR's is the one of the last located frame)
static void GetEyeToHead(int eye, HmdMatrix34_t* m)
{
    if (g_cfg.vrApi == kVrApiOpenXr)
        XrRuntimeEyeToHead(eye, m->m);
    else
        ((PFN_EyeToHeadRaw)(void*)g_vr.system->GetEyeToHeadTransform)(m, (EVREye)eye);
}

// Head space to eye space for one eye (row vectors, D3D's left-handed axes), from SteamVR's eye-to-head transform: on headsets
// with canted displays it holds a rotation, and the eye's tangent box (GetProjectionRaw) is given in that rotated frame.  The
// translation is scaled by IpdScale and PositionScale like the half IPD it replaces.  False when the runtime gives something that
// is not a plain rigid transform with the eye within 10 cm of the head.
static bool ReadHeadToEye(int eye, Matrix4& headToEye, double translation[3])
{
    HmdMatrix34_t m;
    GetEyeToHead(eye, &m);
    double rotation[3][3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++)
            rotation[r][c] = m.m[r][c] * (((r == 2) ? -1.0 : 1.0) * ((c == 2) ? -1.0 : 1.0));
        translation[r] = m.m[r][3] * (r == 2 ? -1.0 : 1.0);
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double d = rotation[i][0] * rotation[j][0] + rotation[i][1] * rotation[j][1] + rotation[i][2] * rotation[j][2];
            if (fabs(d - (i == j ? 1.0 : 0.0)) > 0.01)
                return false;
        }
    if (sqrt(translation[0] * translation[0] + translation[1] * translation[1] + translation[2] * translation[2]) > 0.1)
        return false;
    // eye -> head is p_head = R p_eye + t (column vectors); head -> eye as a row-vector matrix is [R; -t R]
    double scale = g_cfg.ipdScale * g_cfg.positionScale;
    headToEye = Identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            headToEye.m[i][j] = rotation[i][j];
    for (int j = 0; j < 3; j++)
        headToEye.m[3][j] = -scale * (translation[0] * rotation[0][j] + translation[1] * rotation[1][j] + translation[2] * rotation[2][j]);
    return true;
}

static void LogEyeToHead()
{
    for (int eye = 0; eye < 2; eye++) {
        HmdMatrix34_t m;
        GetEyeToHead(eye, &m);
        Matrix4 headToEye;
        double t[3];
        bool usable = ReadHeadToEye(eye, headToEye, t);
        // the eye's forward axis (-z in OpenVR) in head space: yaw > 0 = turned to the right, pitch > 0 = up
        double fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
        Log("eye %d to head: offset %.1f %.1f %.1f mm, forward yaw %.2f deg pitch %.2f deg, roll %.2f deg%s", eye,
            m.m[0][3] * 1000.0, m.m[1][3] * 1000.0, m.m[2][3] * 1000.0, atan2(fx, -fz) * 57.29578,
            atan2(fy, sqrt(fx * fx + fz * fz)) * 57.29578, atan2(m.m[1][0], m.m[1][1]) * 57.29578,
            usable ? "" : " (not usable: half the IPD along x is used)");
    }
}

// GetProjectionRaw's vertical tangents count +y downwards: OpenVR's own ComposeProjection maps the tangent `bottom` to the top of
// the picture (y_ndc = (2 tan - (bottom + top)) / (bottom - top), tan = bottom -> +1), so `bottom` is how far the eye sees upwards
// and `-top` how far down.  Reading them the other way round went unnoticed on headsets that see as far up as down (Vive, Index),
// but a Quest 2 sees 44 degrees up and 55 down: the picture was rendered for 55 up / 44 down and shown as 44 / 55, which stretches
// the lower half, squeezes the upper one and bends straight lines in the periphery as the head turns.
static float g_rawTangents[2][4]; // left, right, top, bottom as GetProjectionRaw gave them
static void SplitVerticalTangents(float top, float bottom, double& up, double& down)
{
    if (top < bottom) {
        up = bottom;
        down = -top;
    } else { // (a runtime that gives them with +y up)
        up = top;
        down = -bottom;
    }
}

// The per-eye tangent boxes and everything built from them, from the raw tangents read at start-up.
static void ComputeEyeBoxes()
{
    double fovScale = g_cfg.fovScale;
    for (int eye = 0; eye < 2; eye++) {
        const float* raw = g_rawTangents[eye];
        double up, down;
        SplitVerticalTangents(raw[2], raw[3], up, down);
        g_vr.eyeBox[eye] = {raw[0] * fovScale, raw[1] * fovScale, up * fovScale, down * fovScale};
        Log("eye %d tangents l=%.3f r=%.3f up=%.3f down=%.3f (raw top %.3f bottom %.3f)", eye, raw[0], raw[1], up, down, raw[2], raw[3]);
    }
    g_vr.unionBox = {fmin(g_vr.eyeBox[0].left, g_vr.eyeBox[1].left), fmax(g_vr.eyeBox[0].right, g_vr.eyeBox[1].right),
                     fmax(g_vr.eyeBox[0].up, g_vr.eyeBox[1].up), fmax(g_vr.eyeBox[0].down, g_vr.eyeBox[1].down)};
    for (int eye = 0; eye < 2; eye++) { // mono fallback: both eyes sample their part of the union image
        const EyeBox& thisEye = g_vr.eyeBox[eye];
        const EyeBox& unionOfEyes = g_vr.unionBox;
        g_vr.monoBounds[eye].uMin = (float)((thisEye.left - unionOfEyes.left) / (unionOfEyes.right - unionOfEyes.left));
        g_vr.monoBounds[eye].uMax =
            (float)((thisEye.right - unionOfEyes.left) / (unionOfEyes.right - unionOfEyes.left));
        g_vr.monoBounds[eye].vMin = (float)((unionOfEyes.up - thisEye.up) / (unionOfEyes.up + unionOfEyes.down));
        g_vr.monoBounds[eye].vMax = (float)((unionOfEyes.up + thisEye.down) / (unionOfEyes.up + unionOfEyes.down));
    }
}

// [VR] Api=openxr: the runtime, its Direct3D 11 device and the field of view come from openxr_runtime.cpp, the rest of the start is the same
// as with OpenVR.  There is no chaperone to recentre through, so the first pose sets the mod's own reference pose (as without IVRChaperone).
static bool VrInitOpenXr()
{
    float eyeToHead[2][3][4];
    if (!XrRuntimeInit(g_rawTangents, eyeToHead)) {
        Log("OpenXR could not be started: VR is off and the game runs flat (set Api=openvr under [VR] to use SteamVR)");
        return false;
    }
    ComputeEyeBoxes();
    double dx = eyeToHead[1][0][3] - eyeToHead[0][0][3], dy = eyeToHead[1][1][3] - eyeToHead[0][1][3], dz = eyeToHead[1][2][3] - eyeToHead[0][2][3];
    double ipd = sqrt(dx * dx + dy * dy + dz * dz);
    if (ipd > 0.04 && ipd < 0.09)
        g_vr.ipd = ipd;
    Log("IPD %.1f mm", g_vr.ipd * 1000.0);
    LogEyeToHead();
    for (int i = 0; i < 3; i++)
        g_vr.eyeCameraDelta[i] = Identity();
    g_vr.recenterPending = true;
    Log("VR initialised (OpenXR)");
    return true;
}

bool VrInit()
{
    if (g_vr.tried)
        return g_vr.active;
    g_vr.tried = true;
    if (!g_cfg.vr) {
        Log(g_cfg.enabled ? "Stereo=0 in ini: flat mode, SteamVR is not used" : "Disabled in ini");
        return false;
    }
    if (g_cfg.vrApi == kVrApiOpenXr)
        return VrInitOpenXr();
    Log("VR API: OpenVR (SteamVR)");

    std::string dllPath = std::string(g_modDir) + "openvr_api.dll";
    g_vr.openvrDll = LoadLibraryExA(dllPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_vr.openvrDll)
        g_vr.openvrDll = LoadLibraryA("openvr_api.dll");
    if (!g_vr.openvrDll) {
        Log("Could not load openvr_api.dll (%s)", dllPath.c_str());
        return false;
    }
    auto pInit = (PFN_VR_InitInternal)GetProcAddress(g_vr.openvrDll, "VR_InitInternal");
    auto pHmd = (PFN_VR_IsHmdPresent)GetProcAddress(g_vr.openvrDll, "VR_IsHmdPresent");
    g_vr.getIface = (PFN_VR_GetGenericInterface)GetProcAddress(g_vr.openvrDll, "VR_GetGenericInterface");
    if (!pInit || !g_vr.getIface) {
        Log("openvr_api.dll missing exports");
        return false;
    }
    if (pHmd && !pHmd()) {
        Log("No HMD present / SteamVR runtime not installed: VR disabled (game runs flat)");
        return false;
    }

    EVRInitError err = EVRInitError_VRInitError_None;
    pInit(&err, EVRApplicationType_VRApplication_Scene);
    if (err != EVRInitError_VRInitError_None) {
        Log("VR_InitInternal failed: %d", (int)err);
        return false;
    }

    g_vr.system =
        (struct VR_IVRSystem_FnTable*)g_vr.getIface((std::string("FnTable:") + IVRSystem_Version).c_str(), &err);
    if (!g_vr.system) {
        Log("IVRSystem interface failed: %d", (int)err);
        return false;
    }
    g_vr.compositor = (struct VR_IVRCompositor_FnTable*)g_vr.getIface(
        (std::string("FnTable:") + IVRCompositor_Version).c_str(), &err);
    if (!g_vr.compositor) {
        Log("IVRCompositor interface failed: %d", (int)err);
        return false;
    }
    g_vr.overlay =
        (struct VR_IVROverlay_FnTable*)g_vr.getIface((std::string("FnTable:") + IVROverlay_Version).c_str(), &err);
    {
        EVRInitError e2;
        g_vr.applications = (struct VR_IVRApplications_FnTable*)g_vr.getIface(
            (std::string("FnTable:") + IVRApplications_Version).c_str(), &e2);
    }
    if (!g_vr.overlay)
        Log("IVROverlay interface failed: %d (menus will not be windowed)", (int)err);
    {
        EVRInitError chaperoneError;
        g_vr.chaperone = (struct VR_IVRChaperone_FnTable*)g_vr.getIface(
            (std::string("FnTable:") + IVRChaperone_Version).c_str(), &chaperoneError);
        if (!g_vr.chaperone)
            Log("IVRChaperone interface failed: %d (recentre falls back to the mod's own reference)",
                (int)chaperoneError);
    }

    // per-eye projection boxes
    for (int eye = 0; eye < 2; eye++) {
        float* raw = g_rawTangents[eye];
        g_vr.system->GetProjectionRaw((EVREye)eye, &raw[0], &raw[1], &raw[2], &raw[3]);
    }
    ComputeEyeBoxes();
    ETrackedPropertyError propertyError = ETrackedPropertyError_TrackedProp_Success;
    float ipd = g_vr.system->GetFloatTrackedDeviceProperty(
        k_unTrackedDeviceIndex_Hmd, ETrackedDeviceProperty_Prop_UserIpdMeters_Float, &propertyError);
    if (propertyError == ETrackedPropertyError_TrackedProp_Success && ipd > 0.04f && ipd < 0.09f)
        g_vr.ipd = ipd;
    Log("IPD %.1f mm", g_vr.ipd * 1000.0);
    LogEyeToHead();
    for (int i = 0; i < 3; i++)
        g_vr.eyeCameraDelta[i] = Identity();

    // D3D11 device on the adapter SteamVR wants
    int32_t adapterIdx = 0;
    g_vr.system->GetDXGIOutputInfo(&adapterIdx);
    IDXGIFactory1* factory = NULL;
    IDXGIAdapter1* adapter = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) && factory) {
        if (adapterIdx < 0 || FAILED(factory->EnumAdapters1((UINT)adapterIdx, &adapter)))
            adapter = NULL;
    }
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_vr.d3d11Device,
                                   &featureLevel, &g_vr.d3d11Context);
    if (adapter)
        adapter->Release();
    if (factory)
        factory->Release();
    if (FAILED(hr)) {
        Log("D3D11CreateDevice failed 0x%08x", (unsigned)hr);
        return false;
    }
    Log("D3D11 device ok (adapter %d, FL 0x%x)", (int)adapterIdx, (unsigned)featureLevel);

    // small dark texture for the scene layer while the menu screen is shown
    {
        static UINT px[16 * 16];
        for (int i = 0; i < 16 * 16; i++)
            px[i] = 0xFF0C0C12;
        D3D11_TEXTURE2D_DESC textureDesc;
        memset(&textureDesc, 0, sizeof(textureDesc));
        textureDesc.Width = 16;
        textureDesc.Height = 16;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initialData;
        initialData.pSysMem = px;
        initialData.SysMemPitch = 16 * 4;
        initialData.SysMemSlicePitch = 0;
        g_vr.d3d11Device->CreateTexture2D(&textureDesc, &initialData, &g_vr.darkTexture);
    }

    if (g_vr.overlay) {
        EVROverlayError overlayError =
            g_vr.overlay->CreateOverlay((char*)"blurvr.menu", (char*)"Blur", &g_vr.menuOverlay);
        if (overlayError != EVROverlayError_VROverlayError_None) {
            Log("CreateOverlay failed %d", (int)overlayError);
            g_vr.overlay = NULL;
        } else {
            g_vr.overlay->SetOverlayWidthInMeters(g_vr.menuOverlay, (float)g_cfg.menuWidth);
            Log("menu overlay created");
        }
    }

    g_vr.compositor->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseSeated);
    if (!g_vr.chaperone)
        g_vr.recenterPending = true; // no way to ask SteamVR: remember the starting pose ourselves
    Log("VR initialised");
    StartLensReportThread();
    return true;
}

// Fetch the next HMD pose; derive per-eye camera deltas and the menu-screen anchor.
void VrUpdatePose()
{
    { // drain SteamVR events (also lets us see dashboard / focus / quit notifications during startup)
        VREvent_t event;
        int n = 0;
        while (g_vr.system && g_vr.system->PollNextEvent(&event, sizeof(event)) && n++ < 64)
            if (event.eventType == 503 || event.eventType == 502) {
                g_vr.menuNeedsPlacement = true;
                g_vr.menuPlaceUntilFrame = g_vr.frameCount + 45;
            } else if (event.eventType == EVREventType_VREvent_SeatedZeroPoseReset ||
                       event.eventType == EVREventType_VREvent_StandingZeroPoseReset) {
                // the user recentred in SteamVR: our seated frame follows by itself, only the floating menu has to move
                Log("SteamVR recentre (event %d)", (int)event.eventType);
                g_vr.menuNeedsPlacement = true;
                g_vr.menuPlaceUntilFrame = g_vr.frameCount + 45;
            }
    }
    TrackedDevicePose_t poses[64];
    XrPoseResult xrPose;
    const float(*pose)[4];
    if (g_cfg.vrApi == kVrApiOpenXr) {
        bool xrValid = XrRuntimeNextPose(&xrPose); // (waits for the next frame, like WaitGetPoses, and handles the runtime's events)
        if (xrPose.menuReplace) {
            g_vr.menuNeedsPlacement = true;
            g_vr.menuPlaceUntilFrame = g_vr.frameCount + 45;
        }
        if (xrPose.recenter)
            g_vr.recenterPending = true;
        if (!xrValid) {
            g_vr.poseValid = false;
            return;
        }
        pose = xrPose.head;
    } else {
        EVRCompositorError compositorError = g_vr.compositor->WaitGetPoses(poses, 64, NULL, 0);
        if (compositorError != EVRCompositorError_VRCompositorError_None) {
            g_vr.poseValid = false;
            return;
        }
        const TrackedDevicePose_t& hmdPose = poses[k_unTrackedDeviceIndex_Hmd];
        if (!hmdPose.bPoseIsValid) {
            g_vr.poseValid = false;
            return;
        }
        pose = hmdPose.mDeviceToAbsoluteTracking.m;
    }

    // menu-screen anchor (right-handed tracking space): flat forward direction
    g_vr.hmdPos[0] = pose[0][3];
    g_vr.hmdPos[1] = pose[1][3];
    g_vr.hmdPos[2] = pose[2][3];
    {
        double fx = -pose[0][2], fz = -pose[2][2], n = sqrt(fx * fx + fz * fz);
        if (n > 0.2) {
            g_vr.hmdFwd[0] = fx / n;
            g_vr.hmdFwd[1] = 0;
            g_vr.hmdFwd[2] = fz / n;
        }
    }

    // OpenVR (right-handed, -Z forward) -> D3D (left-handed, +Z forward): flip Z.
    double rotation[3][3], translation[3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            rotation[r][c] = pose[r][c] * (((r == 2) ? -1.0 : 1.0) * ((c == 2) ? -1.0 : 1.0));
    translation[0] = pose[0][3];
    translation[1] = pose[1][3];
    translation[2] = -pose[2][3];

    if (g_vr.recenterPending) {
        g_vr.recenterPending = false;
        if (g_vr.chaperone) {
            // Recentre SteamVR itself (the same as its own "recentre" button).  We track in SteamVR's seated space, whose
            // origin is the seated zero pose, so the reference is simply "zero": the car's seat is wherever SteamVR's
            // zero is, and a recentre done in SteamVR works exactly like one done here.
            g_vr.chaperone->ResetZeroPose(ETrackingUniverseOrigin_TrackingUniverseSeated);
            g_vr.refYaw = 0;
            g_vr.refPos[0] = g_vr.refPos[1] = g_vr.refPos[2] = 0;
            Log("recentred via SteamVR (head was at yaw %.1f deg, pos %.2f %.2f %.2f in seated space)",
                atan2(rotation[0][2], rotation[2][2]) * 57.29578, translation[0], translation[1], translation[2]);
        } else { // no chaperone interface: keep our own reference
            g_vr.refYaw = atan2(rotation[0][2], rotation[2][2]);
            g_vr.refPos[0] = translation[0];
            g_vr.refPos[1] = translation[1];
            g_vr.refPos[2] = translation[2];
            Log("recentered (own reference): yaw %.1f deg, pos %.2f %.2f %.2f", g_vr.refYaw * 57.29578, translation[0],
                translation[1], translation[2]);
        }
    }

    Matrix4 headDelta = Identity();
    if (g_vr.headTrackingOn) {
        double cosYaw = cos(g_vr.refYaw), sinYaw = sin(g_vr.refYaw);
        double yawUndo[3][3] = {{cosYaw, 0, -sinYaw}, {0, 1, 0}, {sinYaw, 0, cosYaw}}; // rotation by -yaw about Y
        double relRotation[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double s = 0;
                for (int k = 0; k < 3; k++)
                    s += yawUndo[i][k] * rotation[k][j];
                relRotation[i][j] = s;
            }
        if (!g_cfg.trackRotation)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    relRotation[i][j] = (i == j) ? 1.0 : 0.0;
        double deltaPosition[3] = {translation[0] - g_vr.refPos[0], translation[1] - g_vr.refPos[1],
                                   translation[2] - g_vr.refPos[2]},
               relPosition[3];
        for (int i = 0; i < 3; i++) {
            double s = 0;
            for (int k = 0; k < 3; k++)
                s += yawUndo[i][k] * deltaPosition[k];
            relPosition[i] = s * g_cfg.positionScale;
        }
        if (!g_cfg.trackPosition)
            relPosition[0] = relPosition[1] = relPosition[2] = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                headDelta.m[i][j] = relRotation[i][j];
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int i = 0; i < 3; i++)
                s += relPosition[i] * relRotation[i][j];
            headDelta.m[3][j] = -s;
        }
    }
    double halfIpd = 0.5 * g_vr.ipd * g_cfg.ipdScale * g_cfg.positionScale;
    g_vr.eyeCameraDelta[2] = headDelta;
    Matrix4 headToEye[2];
    double eyeOffset[2][3];
    if (g_cfg.eyeTransform && ReadHeadToEye(0, headToEye[0], eyeOffset[0]) && ReadHeadToEye(1, headToEye[1], eyeOffset[1])) {
        // (read every frame: the IPD can change while playing)
        for (int eye = 0; eye < 2; eye++)
            g_vr.eyeCameraDelta[eye] = Multiply(headDelta, headToEye[eye]);
        double dx = eyeOffset[1][0] - eyeOffset[0][0], dy = eyeOffset[1][1] - eyeOffset[0][1], dz = eyeOffset[1][2] - eyeOffset[0][2];
        double ipd = sqrt(dx * dx + dy * dy + dz * dz);
        if (ipd > 0.04 && ipd < 0.09)
            g_vr.ipd = ipd;
    } else {
        g_vr.eyeCameraDelta[0] = headDelta;
        g_vr.eyeCameraDelta[0].m[3][0] += halfIpd; // left eye sits at -half in head space: view-space shift +half
        g_vr.eyeCameraDelta[1] = headDelta;
        g_vr.eyeCameraDelta[1].m[3][0] -= halfIpd;
    }
    g_vr.poseValid = true;
}

// Where the floating menu screen goes: flat in front of the head, facing it (right-handed tracking space)
static void ComputeMenuTransform(HmdMatrix34_t* out)
{
    double forward[3] = {g_vr.hmdFwd[0], 0, g_vr.hmdFwd[2]};
    double overlayZ[3] = {-forward[0], 0, -forward[2]};  // overlay +Z faces the user
    double overlayX[3] = {overlayZ[2], 0, -overlayZ[0]}; // up x z
    double position[3] = {g_vr.hmdPos[0] + forward[0] * g_cfg.menuDistance, g_vr.hmdPos[1] + g_cfg.menuHeight,
                          g_vr.hmdPos[2] + forward[2] * g_cfg.menuDistance};
    HmdMatrix34_t& transform = *out;
    transform.m[0][0] = (float)overlayX[0];
    transform.m[0][1] = 0;
    transform.m[0][2] = (float)overlayZ[0];
    transform.m[0][3] = (float)position[0];
    transform.m[1][0] = 0;
    transform.m[1][1] = 1;
    transform.m[1][2] = 0;
    transform.m[1][3] = (float)position[1];
    transform.m[2][0] = (float)overlayX[2];
    transform.m[2][1] = 0;
    transform.m[2][2] = (float)overlayZ[2];
    transform.m[2][3] = (float)position[2];
}
static void PlaceMenuOverlay()
{
    if (!g_vr.overlay)
        return;
    HmdMatrix34_t transform;
    ComputeMenuTransform(&transform);
    g_vr.overlay->SetOverlayTransformAbsolute(g_vr.menuOverlay, ETrackingUniverseOrigin_TrackingUniverseSeated, &transform);
    g_vr.menuNeedsPlacement = false;
}
// Api=openxr: the finished picture to the headset (openxr_runtime.cpp): a race frame as a projection layer, anything else on the floating
// menu screen, which is a quad layer placed the same way as the OpenVR overlay.
static void SubmitFrameOpenXr(bool sceneFrame, bool sharedCopied, UINT width, UINT height)
{
    if (sceneFrame) {
        XrRuntimeSubmitScene(sharedCopied ? g_vr.sharedTexture11 : g_vr.frameTexture, width, height);
        return;
    }
    static HmdMatrix34_t menuPose;
    static bool menuPlaced = false;
    // (re-placed for a few frames after a recentre, and while something else (a dashboard) has the focus over the game: the pose is unreliable then)
    if (g_vr.frameCount < g_vr.menuPlaceUntilFrame || (g_vr.frameCount < 1500 && XrRuntimeCovered()))
        g_vr.menuNeedsPlacement = true;
    if (!menuPlaced || (g_vr.menuNeedsPlacement && g_vr.poseValid)) {
        ComputeMenuTransform(&menuPose);
        menuPlaced = true;
        if (g_vr.poseValid)
            g_vr.menuNeedsPlacement = false;
    }
    XrRuntimeSubmitMenu(g_vr.frameTexture, width, height, menuPose.m, g_cfg.menuWidth);
}

// In-game switches: the 3D rear-view mirror (numpad 6 by default), the timing probe (numpad 7).  Pressing the mirror key flips it
// live and writes the new state to BlurVR.log.
static void PollSwitchKeys()
{
    static bool mirrorWasDown = false;
    bool mirrorDown = g_cfg.keyMirror && (GetAsyncKeyState(g_cfg.keyMirror) & 0x8000) != 0;
    if (mirrorDown && !mirrorWasDown) {
        g_cfg.mirror3d = g_cfg.mirror3d ? 0 : 1;
        Log("switch: 3D mirror = %d", g_cfg.mirror3d);
    }
    mirrorWasDown = mirrorDown;
    static bool perfWasDown = false;
    bool perfDown = g_cfg.keyPerf && (GetAsyncKeyState(g_cfg.keyPerf) & 0x8000) != 0;
    if (perfDown && !perfWasDown)
        PerfToggle();
    perfWasDown = perfDown;
    static bool fpsWasDown = false;
    bool fpsDown = g_cfg.keyFps && (GetAsyncKeyState(g_cfg.keyFps) & 0x8000) != 0;
    if (fpsDown && !fpsWasDown) {
        g_cfg.fpsCounter = g_cfg.fpsCounter ? 0 : 1;
        Log("switch: FPS counter = %d", g_cfg.fpsCounter);
    }
    fpsWasDown = fpsDown;
}

bool VrShowEyeOnDesktop(IDirect3DDevice9* device, RECT* presented)
{
    IDirect3DSurface9* source = g_vr.desktopSource;
    g_vr.desktopSource = NULL;
    int eye = g_cfg.desktopView - 1; // 1 = left, 2 = right
    if (!source || eye < 0 || eye > 1 || !g_vr.active || !g_vr.desktopSourceWidth || !g_vr.desktopSourceHeight)
        return false;
    D3DDEVICE_CREATION_PARAMETERS creation;
    RECT client;
    if (FAILED(device->GetCreationParameters(&creation)) || !creation.hFocusWindow ||
        !GetClientRect(creation.hFocusWindow, &client)) // (the window as it really is: this module's own call is not the spoofed one)
        return false;
    double windowW = client.right - client.left, windowH = client.bottom - client.top;
    const EyeBox& box = g_vr.eyeBox[eye];
    double tanW = box.right - box.left, tanH = box.up + box.down;
    if (windowW < 64 || windowH < 64 || tanW <= 0 || tanH <= 0) // (minimised, or no headset geometry yet)
        return false;
    // The eye's picture is flat (a tangent box, no lens warp).  The view straight ahead sits at this fraction of it; take the largest
    // piece around that point with the window's shape (in tangent space, so nothing is stretched) that stays inside the picture.
    double ahead = -box.left / tanW, top = box.up / tanH;
    double aspect = windowW / windowH;
    double half = fmin(fmin(-box.left, box.right), fmin(box.up, box.down) * aspect); // half width, as a tangent
    double halfHeight = half / aspect;
    double eyeW = g_vr.desktopSourceWidth / 2.0, eyeH = g_vr.desktopSourceHeight;
    double centreX = eye * eyeW + ahead * eyeW, centreY = top * eyeH;
    double reachX = half / tanW * eyeW, reachY = halfHeight / tanH * eyeH;
    RECT crop = {(LONG)(centreX - reachX), (LONG)(centreY - reachY), (LONG)(centreX + reachX), (LONG)(centreY + reachY)};
    if (crop.right - crop.left < 16 || crop.bottom - crop.top < 16)
        return false;
    IDirect3DSurface9* backBuffer = NULL;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        return false;
    bool small = false;
    D3DSURFACE_DESC desc;
    if (SUCCEEDED(backBuffer->GetDesc(&desc)) && desc.MultiSampleType == D3DMULTISAMPLE_NONE) {
        // At the window's own size (no more than the back buffer) into the corner when Present can take a rectangle: the copy and the
        // desktop compositor then handle that many pixels instead of the whole 3840x2160 back buffer.
        RECT dest = {0, 0, 0, 0};
        if (g_desktopRectUsable && !g_desktopRectFailed) {
            double scale = fmin(1.0, fmin((double)desc.Width / windowW, (double)desc.Height / windowH));
            dest.right = (LONG)(windowW * scale);
            dest.bottom = (LONG)(windowH * scale);
            small = dest.right >= 64 && dest.bottom >= 64;
        }
        if (SUCCEEDED(device->StretchRect(source, &crop, backBuffer, small ? &dest : NULL, D3DTEXF_LINEAR)) && small)
            *presented = dest;
        else
            small = false;
    }
    backBuffer->Release();
    return small;
}

static void SubmitFrameBody(IDirect3DDevice9* device)
{
    g_vr.desktopSource = NULL;
    g_streakUsed = false;
    if (IsRaceMode())
        UpdateMovieState(g_sceneDrawsThisFrame > 0);
    else
        UpdateMovieState(true); // loading screens: no movie state to keep
    FpsCounterFrame();
    PerfSetPhase(device, PH_HUD_COMPOSE);
    {
        PerfScope perfCompose(PS_COMPOSE);
        ComposeHudLayers(device);
    }
    if (g_cfg.fpsCounter && IsRaceMode() && g_sceneDrawsThisFrame > 0) // (3D frames only, the showroom's included: a loading screen or a video is a flat picture, not a pair of eyes)
        FpsCounterDraw(device);
    NoteSceneBloom(NULL); // the game's glow buffer is only valid for the frame it was drawn in
    g_menuGlowFrame = IsRaceMode(); // the game's UI glow is kept in the race too (HUD, pause, restart, car selector)
    g_hudDrawsThisFrame = 0;
    PollSwitchKeys();
    ApplyReflectionSwitch();
    // hotkeys
    int hotkeys[3] = {g_cfg.recenterKey, g_cfg.headTrackingKey, g_cfg.modeKey};
    bool isDown[3];
    for (int i = 0; i < 3; i++) {
        isDown[i] = (GetAsyncKeyState(hotkeys[i]) & 0x8000) != 0;
    }
    if (isDown[0] && !g_prevKeyDown[0]) {
        g_vr.recenterPending = true;
        g_vr.menuNeedsPlacement = true;
    }
    if (isDown[1] && !g_prevKeyDown[1]) {
        g_vr.headTrackingOn = !g_vr.headTrackingOn;
        Log("head tracking %s", g_vr.headTrackingOn ? "ON" : "OFF");
    }
    if (isDown[2] && !g_prevKeyDown[2]) {
        g_modeOverride = (g_modeOverride + 1) % 3;
        Log("mode override: %s", g_modeOverride == 0 ? "AUTO" : g_modeOverride == 1 ? "FORCE MENU" : "FORCE RACE");
    }
    for (int i = 0; i < 3; i++)
        g_prevKeyDown[i] = isDown[i];

    if (g_levelLoading && g_levelFilesSeen && g_modeOverride == 0) {
        if (g_sceneProbeDraws >= 20)
            g_sceneProbeFrames++;
        else
            g_sceneProbeFrames = 0;
        if (g_sceneProbeFrames >= 15) {
            g_levelLoading = false;
            Log("3D scene detected (%u scene draws/frame) -> stereo", g_sceneProbeDraws);
        }
    }
    g_sceneProbeDraws = 0;
    static int lastRaceMode = -1;
    bool inRace = IsRaceMode();
    if (lastRaceMode != (int)inRace) {
        lastRaceMode = (int)inRace;
        Log("mode -> %s", inRace ? "3D (stereo, seated)" : "BOXED (floating screen)");
        if (inRace) {
            if (g_cfg.autoRecenter)
                g_vr.recenterPending = true;
            g_raceStartPhase = true;
            g_raceStartTime = GetTickCount64();
        } else {
            g_vr.menuNeedsPlacement = true;
            g_raceStartPhase = false;
        }
    }

    {   // a restarted race has no level load to mark its start: the scene disappears behind the game's loading screen
        // for a while and then comes back
        static int noSceneFrames = 0;
        bool inShowroom = strcmp(g_currentLevel, "showroom") == 0;
        if (inRace && g_sceneDrawsThisFrame == 0) {
            noSceneFrames++;
        } else {
            if (inRace && !inShowroom && noSceneFrames >= 20) {
                Log("race restart detected (scene back after %d frames): intro window re-armed", noSceneFrames);
                g_raceStartTime = GetTickCount64();
                g_raceStartPhase = true;
            }
            noSceneFrames = 0;
        }
    }

    IDirect3DSurface9* backBuffer = NULL;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        return;
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    static bool formatLogged = false;
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
        if (!formatLogged) {
            Log("Unsupported back buffer format %d (need X8R8G8B8/A8R8G8B8)", (int)desc.Format);
            formatLogged = true;
        }
        backBuffer->Release();
        return;
    }
    // race frames: straight into the shared texture when the game runs on an Ex device (SharedTransport)
    bool sharedCopied = inRace && g_sceneDrawsThisFrame > 0 && g_cfg.sharedTransport && g_deviceIsEx && !g_vr.sharedFailed &&
                        CopyFrameShared(device, backBuffer, desc);
    bool resourcesReady = !sharedCopied && EnsureStagingResources(device, desc.Width, desc.Height, desc.Format) &&
                          EnsureFrameTexture(desc.Width, desc.Height);
    bool copied = sharedCopied;
    if (resourcesReady) {
        HRESULT hr;
        PerfSetPhase(device, PH_COPY);
        {
            PerfScope perfReadback(PS_READBACK);
            hr = device->StretchRect(backBuffer, NULL, g_vr.stagingSurface, NULL, D3DTEXF_NONE);
            if (SUCCEEDED(hr))
                hr = device->GetRenderTargetData(g_vr.stagingSurface, g_vr.readbackSurface);
        }
        if (SUCCEEDED(hr)) {
            PerfScope perfUpload(PS_UPLOAD);
            D3DLOCKED_RECT lockedRect;
            bool menuFrame = !(inRace && g_sceneDrawsThisFrame > 0);
            if (SUCCEEDED(g_vr.readbackSurface->LockRect(&lockedRect, NULL, menuFrame ? 0 : D3DLOCK_READONLY))) {
                if (menuFrame && (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)) {
                    // the floating screen is blended by its alpha channel: the loading screen leaves a mix of alpha
                    // values in the back buffer, so make the picture opaque
                    for (UINT y = 0; y < desc.Height; y++) {
                        unsigned* row = (unsigned*)((BYTE*)lockedRect.pBits + (size_t)y * lockedRect.Pitch);
                        for (UINT x = 0; x < desc.Width; x++)
                            row[x] |= 0xFF000000u;
                    }
                }
                g_vr.d3d11Context->UpdateSubresource(g_vr.frameTexture, 0, NULL, lockedRect.pBits,
                                                     (UINT)lockedRect.Pitch, 0);
                g_vr.readbackSurface->UnlockRect();
                g_vr.d3d11Context->Flush();
                copied = true;
            }
        } else {
            static int n = 0;
            if (n++ < 5)
                Log("StretchRect/GetRenderTargetData failed 0x%08x", (unsigned)hr);
        }
    }
    FrameDumpPoll(device, backBuffer);
    if (copied && inRace && g_sceneDrawsThisFrame > 0) { // a copy of the finished race picture exists: the desktop window can use it
        g_vr.desktopSource = sharedCopied ? g_vr.sharedSurface9 : g_vr.stagingSurface;
        g_vr.desktopSourceWidth = desc.Width;
        g_vr.desktopSourceHeight = desc.Height;
    }
    backBuffer->Release();

    if (copied && g_cfg.vrApi == kVrApiOpenXr) {
        PerfScope perfSubmit(PS_SUBMIT);
        SubmitFrameOpenXr(inRace && g_sceneDrawsThisFrame > 0, sharedCopied, desc.Width, desc.Height);
    } else if (copied) {
        PerfScope perfSubmit(PS_SUBMIT);
        bool sceneFrame = inRace && g_sceneDrawsThisFrame > 0;
        Texture_t texture;
        texture.eType = ETextureType_TextureType_DirectX;
        texture.eColorSpace = EColorSpace_ColorSpace_Gamma;
        if (sceneFrame) {
            if (g_vr.menuOverlayShown && g_vr.overlay) {
                g_vr.overlay->HideOverlay(g_vr.menuOverlay);
                g_vr.menuOverlayShown = false;
            }
            texture.handle = sharedCopied ? (void*)g_vr.sharedTexture11 : (void*)g_vr.frameTexture;
            VRTextureBounds_t leftBounds = {0.0f, 0.0f, 0.5f, 1.0f}, rightBounds = {0.5f, 0.0f, 1.0f, 1.0f};
            VRTextureBounds_t* bounds[2];
            if (g_cfg.stereo) {
                bounds[0] = &leftBounds;
                bounds[1] = &rightBounds;
            } else {
                bounds[0] = &g_vr.monoBounds[0];
                bounds[1] = &g_vr.monoBounds[1];
            }
            for (int eye = 0; eye < 2; eye++) {
                EVRCompositorError compositorError =
                    g_vr.compositor->Submit((EVREye)eye, &texture, bounds[eye], EVRSubmitFlags_Submit_Default);
                if (compositorError != EVRCompositorError_VRCompositorError_None && g_vr.submitErrs++ < 10)
                    Log("Submit(eye %d) error %d", eye, (int)compositorError);
            }
        } else {
            // menu: dark scene layer + the game image on a floating screen
            if (g_vr.overlay) {
                if (g_vr.frameCount < g_vr.menuPlaceUntilFrame ||
                    (g_vr.frameCount < 1500 && g_vr.overlay->IsDashboardVisible()))
                    g_vr.menuNeedsPlacement = true;
                if (g_vr.menuNeedsPlacement && g_vr.poseValid)
                    PlaceMenuOverlay();
                texture.handle = g_vr.frameTexture;
                EVROverlayError textureError = g_vr.overlay->SetOverlayTexture(g_vr.menuOverlay, &texture);
                if (textureError != EVROverlayError_VROverlayError_None && g_vr.submitErrs++ < 10)
                    Log("SetOverlayTexture error %d", (int)textureError);
                if (!g_vr.menuOverlayShown) {
                    EVROverlayError showError = g_vr.overlay->ShowOverlay(g_vr.menuOverlay);
                    g_vr.menuOverlayShown = true;
                    Log("ShowOverlay -> %d (frame %llu)", (int)showError, g_vr.frameCount);
                }
            }
            if (g_vr.darkTexture) {
                texture.handle = g_vr.darkTexture;
                VRTextureBounds_t full = {0, 0, 1, 1};
                for (int eye = 0; eye < 2; eye++) {
                    EVRCompositorError darkCompositorError =
                        g_vr.compositor->Submit((EVREye)eye, &texture, &full, EVRSubmitFlags_Submit_Default);
                    if (darkCompositorError != EVRCompositorError_VRCompositorError_None && g_vr.submitErrs++ < 10)
                        Log("dark Submit(eye %d) error %d (frame %llu)", eye, (int)darkCompositorError,
                            g_vr.frameCount);
                }
            }
        }
    }

    g_vr.frameCount++;
    g_sceneDrawsThisFrame = 0;
}

void VrSubmitFrame(IDirect3DDevice9* device)
{
    SubmitFrameBody(device);
    if (g_cfg.vrApi == kVrApiOpenXr)
        XrRuntimeFinishFrame(); // (every way out of the body: a frame it handed no layer is ended with none)
}

DWORD WINAPI VrInitThread(LPVOID);
void StartVrInitThread()
{
    static bool started = false;
    if (started)
        return;
    started = true;
    g_vrStartDeferred = false;
    CreateThread(NULL, 0, VrInitThread, NULL, 0, NULL);
}
DWORD WINAPI VrInitThread(LPVOID)
{
    if (VrInit()) {
        VrUpdatePose();
        MemoryBarrier();
        g_vr.active = true;
        Log("VR active (threaded start)");
    }
    return 0;
}
