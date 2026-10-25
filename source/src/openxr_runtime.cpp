// OpenXR session ([VR] Api=openxr).  See openxr_runtime.h for what this file is for and vr_runtime.cpp for how it is used.
#include "openxr_runtime.h"
#include "openxr_wrapper.h"
#include <d3d11_4.h>
#include "config.h"
#include "core.h"
#include "vr_runtime.h"

// The structs of the API hold 64-bit times, flags and handles, and the runtime lays them out with those 8-byte aligned (as MSVC does).
static_assert(alignof(XrTime) == 8, "OpenXR structs need 8-byte alignment of 64-bit members");

// The functions taken from the loader, by name: nothing is linked.
#define XR_FUNCTIONS(X)                                                                                                                          \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) X(xrGetSystem) X(xrGetSystemProperties)                                  \
    X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace)     \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage)         \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrPollEvent) X(xrBeginSession) X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame)           \
    X(xrEndFrame) X(xrLocateViews)

namespace {
struct Functions {
#define X(name) PFN_##name name;
    XR_FUNCTIONS(X)
#undef X
};
struct XrRuntimeState {
    HMODULE loader = NULL;
    PFN_xrGetInstanceProcAddr getProc = NULL;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements = NULL;
    Functions fn;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool running = false, lost = false;
    bool frameBegun = false, shouldRender = false, posesLocated = false;
    XrTime displayTime = 0;
    XrDuration displayPeriod = 0; // nanoseconds, as xrWaitFrame last reported it (0 = none yet)
    double loggedHz = 0;
    XrFovf fov[2];          // the field of view of each eye as the runtime gave it at start: the one the picture is submitted with
    XrPosef eyePose[2];     // this frame's eyes in the local space
    float eyeToHead[2][3][4];
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t swapchainFormat = 0;
    UINT swapchainWidth = 0, swapchainHeight = 0;
    std::vector<ID3D11Texture2D*> images; // owned by the runtime
};
} // namespace
static XrRuntimeState g_xr;

static const char* ResultText(XrResult result)
{
    static char text[4][XR_MAX_RESULT_STRING_SIZE];
    static unsigned next = 0;
    char* out = text[next++ & 3];
    if (g_xr.fn.xrResultToString && g_xr.instance != XR_NULL_HANDLE && XR_SUCCEEDED(g_xr.fn.xrResultToString(g_xr.instance, result, out)))
        return out;
    snprintf(out, XR_MAX_RESULT_STRING_SIZE, "%d", (int)result);
    return out;
}
static const char* StateName(XrSessionState state)
{
    switch (state) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
    }
}

// OpenXR's pose (a quaternion and a position) as OpenVR's [R | t] matrix, and back.  Both are right-handed with -Z forward.
static void PoseToMatrix(const XrPosef& pose, float m[3][4])
{
    const float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
    m[0][3] = pose.position.x;
    m[1][3] = pose.position.y;
    m[2][3] = pose.position.z;
}
static XrPosef MatrixToPose(const float m[3][4])
{
    XrPosef pose;
    float x, y, z, w;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (m[2][1] - m[1][2]) / s;
        y = (m[0][2] - m[2][0]) / s;
        z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        w = (m[2][1] - m[1][2]) / s;
        x = 0.25f * s;
        y = (m[0][1] + m[1][0]) / s;
        z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        w = (m[0][2] - m[2][0]) / s;
        x = (m[0][1] + m[1][0]) / s;
        y = 0.25f * s;
        z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        w = (m[1][0] - m[0][1]) / s;
        x = (m[0][2] + m[2][0]) / s;
        y = (m[1][2] + m[2][1]) / s;
        z = 0.25f * s;
    }
    pose.orientation = {x, y, z, w};
    pose.position = {m[0][3], m[1][3], m[2][3]};
    return pose;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------------------------------------------------------------
static void EndFrame(const XrCompositionLayerBaseHeader* const* layers, uint32_t layerCount)
{
    XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
    end.displayTime = g_xr.displayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = layerCount;
    end.layers = layers;
    XrResult result = g_xr.fn.xrEndFrame(g_xr.session, &end);
    g_xr.frameBegun = false;
    if (XR_FAILED(result)) {
        static int failures = 0;
        if (failures++ < 10)
            Log("OpenXR: xrEndFrame failed: %s", ResultText(result));
    }
}
static bool BeginFrame()
{
    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state = {XR_TYPE_FRAME_STATE};
    XrResult result = g_xr.fn.xrWaitFrame(g_xr.session, &waitInfo, &state);
    if (XR_FAILED(result)) {
        static int failures = 0;
        if (failures++ < 10)
            Log("OpenXR: xrWaitFrame failed: %s", ResultText(result));
        return false;
    }
    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    result = g_xr.fn.xrBeginFrame(g_xr.session, &beginInfo);
    if (XR_FAILED(result)) {
        static int failures = 0;
        if (failures++ < 10)
            Log("OpenXR: xrBeginFrame failed: %s", ResultText(result));
        return false;
    }
    g_xr.frameBegun = true; // (a frame that was still open is discarded by the runtime, XR_FRAME_DISCARDED is a success)
    g_xr.displayTime = state.predictedDisplayTime;
    g_xr.displayPeriod = state.predictedDisplayPeriod;
    if (state.predictedDisplayPeriod > 0) { // (logged when it first arrives and when it changes by more than a percent, at most 8 times)
        const double hz = 1e9 / (double)state.predictedDisplayPeriod;
        static int logged = 0;
        if ((g_xr.loggedHz <= 0 || fabs(hz - g_xr.loggedHz) > g_xr.loggedHz * 0.01) && logged++ < 8) {
            g_xr.loggedHz = hz;
            Log("OpenXR: display period %.2f ms (%.1f Hz)", (double)state.predictedDisplayPeriod / 1e6, hz);
        }
    }
    // SteamVR keeps a new application SYNCHRONIZED, with shouldRender false, until it has submitted a frame that has layers: that frame is rendered
    g_xr.shouldRender = state.shouldRender != 0 || g_xr.sessionState == XR_SESSION_STATE_SYNCHRONIZED;
    g_xr.posesLocated = false;
    return true;
}
static bool LocateViews(XrSpace space, XrView views[2], const char* what)
{
    XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g_xr.displayTime;
    locate.space = space;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    views[0] = {XR_TYPE_VIEW};
    views[1] = {XR_TYPE_VIEW};
    uint32_t count = 0;
    XrResult result = g_xr.fn.xrLocateViews(g_xr.session, &locate, &viewState, 2, &count, views);
    const XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    bool valid = XR_SUCCEEDED(result) && count == 2 && (viewState.viewStateFlags & needed) == needed;
    static int reported = 0;
    if (!valid && reported++ < 6)
        Log("OpenXR: the eyes in the %s space are not usable: xrLocateViews %s, %u views, flags 0x%x (session %s)", what, ResultText(result), count,
            (unsigned)viewState.viewStateFlags, StateName(g_xr.sessionState));
    return valid;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------------------------------------------------------------------------
static void HandleSessionState(XrSessionState state, XrPoseResult* result)
{
    XrSessionState previous = g_xr.sessionState;
    g_xr.sessionState = state;
    Log("OpenXR: session %s -> %s", StateName(previous), StateName(state));
    switch (state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XrResult beginResult = g_xr.fn.xrBeginSession(g_xr.session, &begin);
        if (XR_SUCCEEDED(beginResult))
            g_xr.running = true;
        else
            Log("OpenXR: xrBeginSession failed: %s", ResultText(beginResult));
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        g_xr.frameBegun = false;
        g_xr.running = false;
        g_xr.fn.xrEndSession(g_xr.session);
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        g_xr.frameBegun = false;
        g_xr.running = false;
        g_xr.lost = true;
        Log("OpenXR: the runtime ended the session; the headset gets nothing more until the game is restarted");
        break;
    default:
        break;
    }
    if (result && ((previous == XR_SESSION_STATE_FOCUSED) != (state == XR_SESSION_STATE_FOCUSED) ||
                   (previous == XR_SESSION_STATE_VISIBLE) != (state == XR_SESSION_STATE_VISIBLE)))
        result->menuReplace = true; // a dashboard or system menu came or went: the pose is unreliable for a moment
}
static void PollEvents(XrPoseResult* result)
{
    for (int i = 0; i < 64; i++) {
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        if (g_xr.fn.xrPollEvent(g_xr.instance, &event) != XR_SUCCESS)
            break;
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            HandleSessionState(((const XrEventDataSessionStateChanged*)&event)->state, result);
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            if (((const XrEventDataReferenceSpaceChangePending*)&event)->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                Log("OpenXR: the runtime recentred the tracking space");
                if (result)
                    result->recenter = result->menuReplace = true;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Log("OpenXR: the runtime is going away");
            g_xr.lost = true;
            g_xr.running = false;
            g_xr.frameBegun = false;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------------------------------------------------------------------------
static void Abandon()
{
    if (g_xr.instance != XR_NULL_HANDLE && g_xr.fn.xrDestroyInstance)
        g_xr.fn.xrDestroyInstance(g_xr.instance); // (the session and spaces go with it)
    g_xr.instance = XR_NULL_HANDLE;
    g_xr.session = XR_NULL_HANDLE;
    g_xr.running = false;
}
static bool Fail(const char* what, XrResult result)
{
    Log("OpenXR: %s failed: %s", what, ResultText(result));
    Abandon();
    return false;
}
static void LogNoRuntimeHint()
{
    Log("OpenXR: this is a 32-bit game, so it needs the runtime's 32-bit build: the loader reads the active runtime from "
        "HKLM\\SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1 (ActiveRuntime).  SteamVR 2.17 and later registers one; for another runtime set "
        "[VR] OpenXRRuntime= to the .json of its 32-bit build");
}

static bool LoadLoader()
{
    std::string path = std::string(g_modDir) + "openxr_loader.dll";
    g_xr.loader = LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_xr.loader)
        g_xr.loader = LoadLibraryA("openxr_loader.dll");
    if (!g_xr.loader) {
        Log("OpenXR: could not load openxr_loader.dll (%s), error %lu", path.c_str(), (unsigned long)GetLastError());
        return false;
    }
    g_xr.getProc = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(g_xr.loader, "xrGetInstanceProcAddr");
    if (!g_xr.getProc) {
        Log("OpenXR: openxr_loader.dll has no xrGetInstanceProcAddr");
        return false;
    }
    return true;
}
static bool LoadFunctions()
{
#define X(name)                                                                                                                                  \
    if (XR_FAILED(g_xr.getProc(g_xr.instance, #name, (PFN_xrVoidFunction*)&g_xr.fn.name)) || !g_xr.fn.name) {                                      \
        Log("OpenXR: the runtime has no %s", #name);                                                                                             \
        return false;                                                                                                                            \
    }
    XR_FUNCTIONS(X)
#undef X
    if (XR_FAILED(g_xr.getProc(g_xr.instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&g_xr.getD3D11Requirements)) ||
        !g_xr.getD3D11Requirements) {
        Log("OpenXR: the runtime has no xrGetD3D11GraphicsRequirementsKHR");
        return false;
    }
    return true;
}
static bool HasExtension(const char* wanted)
{
    PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
    if (XR_FAILED(g_xr.getProc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate)) || !enumerate)
        return false;
    uint32_t count = 0;
    XrResult result = enumerate(NULL, 0, &count, NULL);
    if (XR_FAILED(result)) {
        Log("OpenXR: listing the runtime's extensions failed: %d", (int)result);
        LogNoRuntimeHint();
        return false;
    }
    std::vector<XrExtensionProperties> extensions(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(enumerate(NULL, count, &count, extensions.data())))
        return false;
    for (const XrExtensionProperties& extension : extensions)
        if (!strcmp(extension.extensionName, wanted))
            return true;
    Log("OpenXR: the runtime does not offer %s (Direct3D 11)", wanted);
    return false;
}

// The Direct3D 11 device on the adapter the runtime renders on; the finished picture is copied into the runtime's images on it.
static bool CreateDevice(const XrGraphicsRequirementsD3D11KHR& requirements)
{
    IDXGIFactory1* factory = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) {
        Log("OpenXR: CreateDXGIFactory1 failed");
        return false;
    }
    IDXGIAdapter1* adapter = NULL;
    for (UINT i = 0; !adapter; i++) {
        IDXGIAdapter1* candidate = NULL;
        if (FAILED(factory->EnumAdapters1(i, &candidate)) || !candidate)
            break;
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(candidate->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart &&
            desc.AdapterLuid.HighPart == (LONG)requirements.adapterLuid.HighPart) {
            char name[128];
            WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);
            Log("OpenXR: graphics adapter %s", name);
            adapter = candidate;
        } else
            candidate->Release();
    }
    factory->Release();
    if (!adapter) {
        Log("OpenXR: the graphics adapter the runtime renders on (LUID %08x:%08x) was not found", (unsigned)requirements.adapterLuid.HighPart,
            (unsigned)requirements.adapterLuid.LowPart);
        return false;
    }
    const D3D_FEATURE_LEVEL candidates[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL levels[3];
    UINT levelCount = 0;
    for (D3D_FEATURE_LEVEL level : candidates)
        if (level >= requirements.minFeatureLevel)
            levels[levelCount++] = level;
    D3D_FEATURE_LEVEL obtained;
    HRESULT hr = levelCount ? D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, levelCount,
                                                D3D11_SDK_VERSION, &g_vr.d3d11Device, &obtained, &g_vr.d3d11Context)
                            : E_FAIL;
    adapter->Release();
    if (FAILED(hr)) {
        Log("OpenXR: D3D11CreateDevice failed 0x%08x (the runtime needs feature level 0x%x)", (unsigned)hr, (unsigned)requirements.minFeatureLevel);
        return false;
    }
    ID3D11Multithread* multithread = NULL; // (the runtime may use the device from its own threads)
    if (SUCCEEDED(g_vr.d3d11Device->QueryInterface(__uuidof(ID3D11Multithread), (void**)&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    Log("OpenXR: D3D11 device ok (FL 0x%x)", (unsigned)obtained);
    return true;
}

// The first frames: the field of view and the eye-to-head transforms are known once the session runs and a frame has been located.
static bool ReadViews(float rawTangents[2][4], float eyeToHead[2][3][4])
{
    for (int attempt = 0; attempt < 300 && !g_xr.lost; attempt++) {
        PollEvents(NULL);
        if (!g_xr.running) {
            Sleep(10);
            continue;
        }
        if (!BeginFrame())
            continue;
        XrView views[2];
        bool located = LocateViews(g_xr.view, views, "view");
        EndFrame(NULL, 0);
        if (!located)
            continue;
        bool sensible = true;
        for (int eye = 0; eye < 2; eye++)
            if (views[eye].fov.angleRight - views[eye].fov.angleLeft < 0.2f || views[eye].fov.angleUp - views[eye].fov.angleDown < 0.2f)
                sensible = false;
        if (!sensible)
            continue;
        for (int eye = 0; eye < 2; eye++) {
            const XrFovf& fov = views[eye].fov;
            g_xr.fov[eye] = fov;
            // up and down as tangents with +y up: SplitVerticalTangents (vr_runtime.cpp) takes this form as it is
            rawTangents[eye][0] = tanf(fov.angleLeft);
            rawTangents[eye][1] = tanf(fov.angleRight);
            rawTangents[eye][2] = tanf(fov.angleUp);
            rawTangents[eye][3] = tanf(fov.angleDown);
            PoseToMatrix(views[eye].pose, g_xr.eyeToHead[eye]);
            memcpy(eyeToHead[eye], g_xr.eyeToHead[eye], sizeof(g_xr.eyeToHead[eye]));
            Log("OpenXR: eye %d field of view left %.1f right %.1f up %.1f down %.1f degrees", eye, fov.angleLeft * 57.29578f,
                fov.angleRight * 57.29578f, fov.angleUp * 57.29578f, fov.angleDown * 57.29578f);
        }
        return true;
    }
    Log("OpenXR: the session did not deliver a field of view (state %s)", StateName(g_xr.sessionState));
    return false;
}

bool XrRuntimeInit(float rawTangents[2][4], float eyeToHead[2][3][4])
{
    if (g_cfg.openxrRuntime[0]) { // before the loader looks for a runtime
        std::string path = g_cfg.openxrRuntime;
        if (path.find(':') == std::string::npos && path[0] != '\\' && path[0] != '/')
            path = std::string(g_modDir) + path;
        SetEnvironmentVariableA("XR_RUNTIME_JSON", path.c_str());
        Log("OpenXR: runtime from [VR] OpenXRRuntime: %s", path.c_str());
    }
    if (!LoadLoader())
        return false;
    if (!HasExtension(XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
        return false;

    PFN_xrCreateInstance createInstance = NULL;
    if (XR_FAILED(g_xr.getProc(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance)) || !createInstance) {
        Log("OpenXR: the loader has no xrCreateInstance");
        return false;
    }
    XrInstanceCreateInfo instanceInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(instanceInfo.applicationInfo.applicationName, "BlurVR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    strncpy(instanceInfo.applicationInfo.engineName, "BlurVR", XR_MAX_ENGINE_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;
    XrResult result = createInstance(&instanceInfo, &g_xr.instance);
    if (XR_FAILED(result)) {
        Log("OpenXR: xrCreateInstance failed: %d", (int)result);
        LogNoRuntimeHint();
        g_xr.instance = XR_NULL_HANDLE;
        return false;
    }
    if (!LoadFunctions()) {
        Abandon();
        return false;
    }
    XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(g_xr.fn.xrGetInstanceProperties(g_xr.instance, &instanceProperties)))
        Log("OpenXR: runtime %s %u.%u.%u", instanceProperties.runtimeName, (unsigned)XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
            (unsigned)XR_VERSION_MINOR(instanceProperties.runtimeVersion), (unsigned)XR_VERSION_PATCH(instanceProperties.runtimeVersion));

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    DWORD started = GetTickCount();
    while ((result = g_xr.fn.xrGetSystem(g_xr.instance, &systemInfo, &g_xr.systemId)) == XR_ERROR_FORM_FACTOR_UNAVAILABLE &&
           GetTickCount() - started < 5000)
        Sleep(250); // (the runtime may still be starting its headset)
    if (XR_FAILED(result)) {
        if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
            Log("OpenXR: the runtime reports no headset (is it connected and the runtime running?)");
        return Fail("xrGetSystem", result);
    }
    XrSystemProperties systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    g_xr.fn.xrGetSystemProperties(g_xr.instance, g_xr.systemId, &systemProperties);

    uint32_t viewCount = 0;
    result = g_xr.fn.xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, NULL);
    if (XR_FAILED(result) || viewCount != 2)
        return Fail("the stereo view configuration", XR_FAILED(result) ? result : XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    XrViewConfigurationView configViews[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    g_xr.fn.xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, configViews);
    if (g_cfg.foveationReport && g_logEnabled)
        Log("lens: headset %s (OpenXR), the runtime's own render size %ux%u per eye (%ux%u side by side), largest swapchain %ux%u, "
            "orientation tracking %s, position tracking %s",
            systemProperties.systemName, configViews[0].recommendedImageRectWidth, configViews[0].recommendedImageRectHeight,
            configViews[0].recommendedImageRectWidth * 2, configViews[0].recommendedImageRectHeight,
            systemProperties.graphicsProperties.maxSwapchainImageWidth, systemProperties.graphicsProperties.maxSwapchainImageHeight,
            systemProperties.trackingProperties.orientationTracking ? "yes" : "no", systemProperties.trackingProperties.positionTracking ? "yes" : "no");
    else
        Log("OpenXR: system %s", systemProperties.systemName);

    XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    result = g_xr.getD3D11Requirements(g_xr.instance, g_xr.systemId, &requirements);
    if (XR_FAILED(result))
        return Fail("xrGetD3D11GraphicsRequirementsKHR", result);
    if (!CreateDevice(requirements)) {
        Abandon();
        return false;
    }

    XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_vr.d3d11Device;
    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_xr.systemId;
    result = g_xr.fn.xrCreateSession(g_xr.instance, &sessionInfo, &g_xr.session);
    if (XR_FAILED(result))
        return Fail("xrCreateSession", result);
    XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; // the seated space: its origin is where the player sat down or recentred
    result = g_xr.fn.xrCreateReferenceSpace(g_xr.session, &spaceInfo, &g_xr.local);
    if (XR_FAILED(result))
        return Fail("creating the local space", result);
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    result = g_xr.fn.xrCreateReferenceSpace(g_xr.session, &spaceInfo, &g_xr.view);
    if (XR_FAILED(result))
        return Fail("creating the view space", result);

    // the session becomes READY by itself; the first events begin it (HandleSessionState)
    for (DWORD waitStart = GetTickCount(); !g_xr.running && !g_xr.lost && GetTickCount() - waitStart < 15000;) {
        PollEvents(NULL);
        if (!g_xr.running)
            Sleep(10);
    }
    if (!g_xr.running) {
        Log("OpenXR: the session did not become ready (state %s)", StateName(g_xr.sessionState));
        Abandon();
        return false;
    }
    if (!ReadViews(rawTangents, eyeToHead)) {
        Abandon();
        return false;
    }
    Log("OpenXR: session running");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------------------------------------------------------
bool XrRuntimeNextPose(XrPoseResult* result)
{
    memset(result, 0, sizeof(*result));
    PollEvents(result);
    if (g_xr.frameBegun) // the submit of the last frame did not end it
        EndFrame(NULL, 0);
    if (!g_xr.running || g_xr.lost || !BeginFrame())
        return false;
    static int skipped = 0;
    if (!g_xr.shouldRender) {
        if (skipped++ < 6)
            Log("OpenXR: the runtime says not to render this frame (session %s)", StateName(g_xr.sessionState));
        return false;
    }
    XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
    XrResult located = g_xr.fn.xrLocateSpace(g_xr.view, g_xr.local, g_xr.displayTime, &head);
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (XR_FAILED(located) || (head.locationFlags & needed) != needed) {
        if (skipped++ < 6)
            Log("OpenXR: the head in the local space is not usable: xrLocateSpace %s, flags 0x%x (session %s)", ResultText(located),
                (unsigned)head.locationFlags, StateName(g_xr.sessionState));
        return false;
    }
    XrView eyesInHead[2], eyesInLocal[2];
    if (!LocateViews(g_xr.view, eyesInHead, "view") || !LocateViews(g_xr.local, eyesInLocal, "local"))
        return false;
    PoseToMatrix(head.pose, result->head);
    for (int eye = 0; eye < 2; eye++) {
        PoseToMatrix(eyesInHead[eye].pose, g_xr.eyeToHead[eye]);
        g_xr.eyePose[eye] = eyesInLocal[eye].pose;
    }
    g_xr.posesLocated = true;
    return true;
}
void XrRuntimeEyeToHead(int eye, float m[3][4])
{
    memcpy(m, g_xr.eyeToHead[eye & 1], sizeof(g_xr.eyeToHead[0]));
}
bool XrRuntimeCovered()
{
    return g_xr.sessionState == XR_SESSION_STATE_VISIBLE;
}
bool XrRuntimeFocused()
{
    return g_xr.sessionState == XR_SESSION_STATE_FOCUSED;
}
double XrRuntimeRefreshHz()
{
    return g_xr.displayPeriod > 0 ? 1e9 / (double)g_xr.displayPeriod : 0.0;
}
const char* XrRuntimeSessionState()
{
    return StateName(g_xr.sessionState);
}

static void DestroySwapchain()
{
    if (g_xr.swapchain != XR_NULL_HANDLE)
        g_xr.fn.xrDestroySwapchain(g_xr.swapchain);
    g_xr.swapchain = XR_NULL_HANDLE;
    g_xr.images.clear();
    g_xr.swapchainWidth = g_xr.swapchainHeight = 0;
}
// One swapchain for everything: the race picture is side by side and each eye reads its half, the menu reads all of it.
static bool EnsureSwapchain(UINT width, UINT height)
{
    if (g_xr.swapchain != XR_NULL_HANDLE && g_xr.swapchainWidth == width && g_xr.swapchainHeight == height)
        return true;
    DestroySwapchain();
    if (!g_xr.swapchainFormat) {
        uint32_t count = 0;
        g_xr.fn.xrEnumerateSwapchainFormats(g_xr.session, 0, &count, NULL);
        std::vector<int64_t> formats(count);
        if (XR_FAILED(g_xr.fn.xrEnumerateSwapchainFormats(g_xr.session, count, &count, formats.data())))
            return false;
        // The picture is gamma-encoded B8G8R8A8: the sRGB format makes the runtime decode it, and it is in the same family as the picture's
        // own format, which CopyResource needs.
        for (int64_t want : {(int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM})
            for (int64_t offered : formats)
                if (offered == want && !g_xr.swapchainFormat)
                    g_xr.swapchainFormat = want;
        std::string list;
        for (int64_t offered : formats)
            list += " " + std::to_string((long long)offered);
        Log("OpenXR: swapchain formats offered:%s -> %d", list.c_str(), (int)g_xr.swapchainFormat);
        if (!g_xr.swapchainFormat) {
            static bool logged = false;
            if (!logged)
                Log("OpenXR: the runtime offers no B8G8R8A8 swapchain format (87 or 91), which is the only kind this build can copy the picture into");
            logged = true;
            return false;
        }
    }
    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = g_xr.swapchainFormat;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrResult result = g_xr.fn.xrCreateSwapchain(g_xr.session, &info, &g_xr.swapchain);
    if (XR_FAILED(result)) {
        static int failures = 0;
        if (failures++ < 5)
            Log("OpenXR: xrCreateSwapchain %ux%u failed: %s", width, height, ResultText(result));
        g_xr.swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t imageCount = 0;
    g_xr.fn.xrEnumerateSwapchainImages(g_xr.swapchain, 0, &imageCount, NULL);
    std::vector<XrSwapchainImageD3D11KHR> images(imageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(g_xr.fn.xrEnumerateSwapchainImages(g_xr.swapchain, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data()))) {
        DestroySwapchain();
        return false;
    }
    for (const XrSwapchainImageD3D11KHR& image : images)
        g_xr.images.push_back(image.texture);
    g_xr.swapchainWidth = width;
    g_xr.swapchainHeight = height;
    Log("OpenXR: swapchain %ux%u, %u images", width, height, imageCount);
    return true;
}
// The finished picture into the next swapchain image, on the GPU.
static bool CopyToSwapchain(ID3D11Texture2D* picture, UINT width, UINT height)
{
    if (!picture || !EnsureSwapchain(width, height))
        return false;
    XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    XrResult result = g_xr.fn.xrAcquireSwapchainImage(g_xr.swapchain, &acquire, &index);
    if (XR_FAILED(result) || index >= g_xr.images.size()) {
        static int failures = 0;
        if (failures++ < 5)
            Log("OpenXR: xrAcquireSwapchainImage failed: %s", ResultText(result));
        DestroySwapchain();
        return false;
    }
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100000000; // 100 ms, a few times
    bool ready = false;
    for (int tries = 0; tries < 3 && !ready; tries++) {
        result = g_xr.fn.xrWaitSwapchainImage(g_xr.swapchain, &wait);
        if (result == XR_SUCCESS)
            ready = true;
        else if (XR_FAILED(result))
            break;
    }
    if (!ready) {
        static int failures = 0;
        if (failures++ < 5)
            Log("OpenXR: xrWaitSwapchainImage: %s", ResultText(result));
        DestroySwapchain();
        return false;
    }
    static bool formatsLogged = false;
    if (!formatsLogged) {
        formatsLogged = true;
        D3D11_TEXTURE2D_DESC from, to;
        picture->GetDesc(&from);
        g_xr.images[index]->GetDesc(&to);
        Log("OpenXR: first copy, picture %ux%u format %d -> image %ux%u format %d", from.Width, from.Height, (int)from.Format, to.Width, to.Height,
            (int)to.Format);
    }
    g_vr.d3d11Context->CopyResource(g_xr.images[index], picture);
    g_vr.d3d11Context->Flush();
    XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    result = g_xr.fn.xrReleaseSwapchainImage(g_xr.swapchain, &release);
    if (XR_FAILED(result)) {
        static int failures = 0;
        if (failures++ < 5)
            Log("OpenXR: xrReleaseSwapchainImage failed: %s", ResultText(result));
        DestroySwapchain();
        return false;
    }
    return true;
}

void XrRuntimeSubmitScene(ID3D11Texture2D* picture, UINT width, UINT height)
{
    if (!g_xr.frameBegun || !g_xr.shouldRender || !g_xr.posesLocated || !CopyToSwapchain(picture, width, height))
        return; // (XrRuntimeFinishFrame ends the frame with no layers)
    XrCompositionLayerProjectionView views[2];
    for (int eye = 0; eye < 2; eye++) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = g_xr.eyePose[eye];
        views[eye].fov = g_xr.fov[eye]; // the field of view the runtime reported, not the one scaled by FovScale: the picture is drawn for the scaled one and shown over the whole
        views[eye].subImage.swapchain = g_xr.swapchain;
        views[eye].subImage.imageRect.offset = {(int32_t)(eye * (width / 2)), 0};
        views[eye].subImage.imageRect.extent = {(int32_t)(width / 2), (int32_t)height};
        views[eye].subImage.imageArrayIndex = 0;
    }
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_xr.local;
    layer.viewCount = 2;
    layer.views = views;
    const XrCompositionLayerBaseHeader* layers[1] = {(const XrCompositionLayerBaseHeader*)&layer};
    EndFrame(layers, 1);
}
void XrRuntimeSubmitMenu(ID3D11Texture2D* picture, UINT width, UINT height, const float menuPose[3][4], double widthMetres)
{
    if (!g_xr.frameBegun || !g_xr.shouldRender || !width || !height || !CopyToSwapchain(picture, width, height))
        return;
    XrCompositionLayerQuad layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space = g_xr.local;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = g_xr.swapchain;
    layer.subImage.imageRect.offset = {0, 0};
    layer.subImage.imageRect.extent = {(int32_t)width, (int32_t)height};
    layer.subImage.imageArrayIndex = 0;
    layer.pose = MatrixToPose(menuPose);
    layer.size = {(float)widthMetres, (float)(widthMetres * height / width)};
    const XrCompositionLayerBaseHeader* layers[1] = {(const XrCompositionLayerBaseHeader*)&layer};
    EndFrame(layers, 1);
}
void XrRuntimeFinishFrame()
{
    if (g_xr.frameBegun && g_xr.running)
        EndFrame(NULL, 0);
}
