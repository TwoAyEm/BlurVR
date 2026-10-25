// Stand-in game for tools/openxr_check.py: links src/openxr_runtime.cpp (with core.cpp and config.cpp) and runs it as the mod would.
//
//   openxr_check <BlurVR folder> probe   load openxr_loader.dll from the folder and ask the active 32-bit OpenXR runtime for its extensions
//                                        (no instance is created, so no runtime is started)
//   openxr_check <BlurVR folder> full    XrRuntimeInit as VrInit calls it, then frames until the runtime delivers a pose, then 60 race frames and 30 menu frames of a test picture
//
// The folder holds BlurVR.ini and openxr_loader.dll like the mod's own BlurVR\ folder; BlurVR.log is written there.  Exit code 0 = it worked,
// 1 = OpenXR could not be started (the reason is in the log), anything else = a crash or a mistake.
#include "config.h"
#include "core.h"
#include "openxr_runtime.h"
#include "openxr_wrapper.h"
#include "vr_runtime.h"

VrState g_vr; // (the real one is in vr_runtime.cpp, which this check does not link)

static int Probe()
{
    std::string path = std::string(g_modDir) + "openxr_loader.dll";
    HMODULE loader = LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader) {
        printf("could not load %s (error %lu)\n", path.c_str(), GetLastError());
        return 1;
    }
    PFN_xrGetInstanceProcAddr getProc = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(loader, "xrGetInstanceProcAddr");
    PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
    if (!getProc || XR_FAILED(getProc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate)) || !enumerate) {
        printf("the loader has no xrEnumerateInstanceExtensionProperties\n");
        return 1;
    }
    uint32_t count = 0;
    XrResult result = enumerate(NULL, 0, &count, NULL);
    if (XR_FAILED(result)) {
        printf("xrEnumerateInstanceExtensionProperties failed: %d (no OpenXR runtime for 32-bit applications?)\n", (int)result);
        return 1;
    }
    std::vector<XrExtensionProperties> extensions(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    enumerate(NULL, count, &count, extensions.data());
    bool d3d11 = false;
    for (const XrExtensionProperties& extension : extensions)
        if (!strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
            d3d11 = true;
    printf("the 32-bit OpenXR runtime offers %u extensions, %s %s\n", count, XR_KHR_D3D11_ENABLE_EXTENSION_NAME, d3d11 ? "yes" : "NO");
    return d3d11 ? 0 : 1;
}

static int Full()
{
    float tangents[2][4], eyeToHead[2][3][4];
    if (!XrRuntimeInit(tangents, eyeToHead)) {
        printf("XrRuntimeInit failed\n");
        return 1;
    }
    for (int eye = 0; eye < 2; eye++)
        printf("eye %d tangents l=%.3f r=%.3f up=%.3f down=%.3f, eye-to-head offset %.1f %.1f %.1f mm\n", eye, tangents[eye][0], tangents[eye][1],
               tangents[eye][2], tangents[eye][3], eyeToHead[eye][0][3] * 1000, eyeToHead[eye][1][3] * 1000, eyeToHead[eye][2][3] * 1000);
    const UINT width = 2160, height = 1200;
    std::vector<unsigned> pixels((size_t)width * height);
    for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++)
            pixels[(size_t)y * width + x] = 0xFF000000u | ((x * 255 / width) << 16) | ((y * 255 / height) << 8) | (x < width / 2 ? 0x40u : 0xC0u);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {pixels.data(), width * 4, 0};
    ID3D11Texture2D* picture = NULL;
    if (FAILED(g_vr.d3d11Device->CreateTexture2D(&desc, &data, &picture))) {
        printf("could not create the test picture\n");
        return 2;
    }
    // The runtime shows a new application only after a while (the session stays "synchronized", with no frame to render, until then): frames go on
    // with no layers until the first pose arrives (at most 25 s), then 60 race frames and 30 menu frames of the test picture are submitted.
    const float menuPose[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -2}};
    int frames = 0, withPose = 0, sceneFrames = 0, menuFrames = 0;
    DWORD start = GetTickCount(), firstPoseAfter = 0;
    while (menuFrames < 30 && GetTickCount() - start < 25000) {
        XrPoseResult pose;
        frames++;
        if (XrRuntimeNextPose(&pose) && ++withPose == 1) {
            firstPoseAfter = GetTickCount() - start;
            printf("first head pose %.3f %.3f %.3f after %lu ms, %d frames\n", pose.head[0][3], pose.head[1][3], pose.head[2][3], (unsigned long)firstPoseAfter,
                   frames);
        }
        if (withPose && sceneFrames < 60) {
            XrRuntimeSubmitScene(picture, width, height);
            sceneFrames++;
        } else if (withPose) {
            XrRuntimeSubmitMenu(picture, width, height, menuPose, 2.6);
            menuFrames++;
        }
        XrRuntimeFinishFrame();
    }
    printf("frames: %d, with a pose: %d, race frames submitted: %d, menu frames submitted: %d\n", frames, withPose, sceneFrames, menuFrames);
    printf("the runtime's refresh rate (predicted display period): %.1f Hz\n", XrRuntimeRefreshHz());
    // information only: does the runtime give this application the focus once it has shown frames?  (race frames for up to 20 s)
    DWORD focusStart = GetTickCount();
    int focusFrames = 0;
    while (!XrRuntimeFocused() && GetTickCount() - focusStart < 20000) {
        XrPoseResult pose;
        if (XrRuntimeNextPose(&pose))
            XrRuntimeSubmitScene(picture, width, height);
        XrRuntimeFinishFrame();
        focusFrames++;
    }
    DWORD focusMs = GetTickCount() - focusStart;
    printf("%d more race frames in %lu ms (%.1f frames a second); session state at the end: %s\n", focusFrames, (unsigned long)focusMs,
           focusMs ? focusFrames * 1000.0 / focusMs : 0.0, XrRuntimeSessionState());
    if (XrRuntimeFocused())
        printf("the session became focused %lu ms later\n", (unsigned long)focusMs);
    else
        printf("the session was not focused 20 s later (a dashboard or SteamVR Home may have the focus; the state changes are in the log)\n");
    return sceneFrames == 60 && menuFrames == 30 ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    snprintf(g_modDir, MAX_PATH, "%s\\", argv[1]);
    snprintf(g_logPath, MAX_PATH, "%sBlurVR.log", g_modDir);
    FILE* truncate = fopen(g_logPath, "w");
    if (truncate) {
        fclose(truncate);
        g_logEnabled = true;
    }
    LoadConfig();
    int code = !strcmp(argv[2], "probe") ? Probe() : !strcmp(argv[2], "full") ? Full() : 2;
    fflush(stdout);
    return code;
}
