// Headset facts for foveation planning (BlurVR.ini [Foveation] Report=1): model, refresh rate, SteamVR's own render size,
// whether eye tracking is offered, and how much of the flat image the lens hides.  Only logs.  The density study of the lens
// itself (which part of the flat image needs how many pixels, and what a tile layout would cost) is offline:
// tools/lens_dump.cpp dumps the runtime's distortion function while SteamVR is running, tools/lens_layouts.py evaluates it.
#include "lens_model.h"
#include "config.h"
#include "core.h"
#include "vr_runtime.h"

// GetHiddenAreaMesh returns an 8-byte struct (pointer, count) by value; on 32-bit x86 that comes back in EDX:EAX, so the
// function is called as returning a 64-bit integer.
typedef unsigned long long(__stdcall* PFN_HiddenAreaMeshRaw)(EVREye, EHiddenAreaMeshType);

// Share of the flat image (0..1) that the SteamVR hidden-area mesh covers.
static double HiddenAreaFraction(int eye)
{
    PFN_HiddenAreaMeshRaw getMesh = (PFN_HiddenAreaMeshRaw)(void*)g_vr.system->GetHiddenAreaMesh;
    unsigned long long raw = getMesh((EVREye)eye, EHiddenAreaMeshType_k_eHiddenAreaMesh_Standard);
    const HmdVector2_t* vertices = (const HmdVector2_t*)(uintptr_t)(unsigned)(raw & 0xFFFFFFFFull);
    unsigned triangles = (unsigned)(raw >> 32);
    if (!vertices || triangles == 0 || triangles > 100000)
        return 0.0;
    double area = 0;
    for (unsigned t = 0; t < triangles; t++) {
        const HmdVector2_t* a = &vertices[t * 3];
        const HmdVector2_t* b = a + 1;
        const HmdVector2_t* c = a + 2;
        area += 0.5 * fabs((double)(b->v[0] - a->v[0]) * (c->v[1] - a->v[1]) -
                           (double)(c->v[0] - a->v[0]) * (b->v[1] - a->v[1]));
    }
    return area;
}

static DWORD WINAPI LensReportThread(LPVOID)
{
    if (!g_vr.system)
        return 0;
    char model[64] = "?", maker[64] = "?";
    ETrackedPropertyError error;
    g_vr.system->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd,
                                                ETrackedDeviceProperty_Prop_ModelNumber_String, model, sizeof(model),
                                                &error);
    g_vr.system->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd,
                                                ETrackedDeviceProperty_Prop_ManufacturerName_String, maker,
                                                sizeof(maker), &error);
    float refresh = g_vr.system->GetFloatTrackedDeviceProperty(
        k_unTrackedDeviceIndex_Hmd, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &error);
    bool gazeInteraction = g_vr.system->GetBoolTrackedDeviceProperty(
        k_unTrackedDeviceIndex_Hmd, ETrackedDeviceProperty_Prop_SupportsXrEyeGazeInteraction_Bool, &error);
    uint32_t recommendedWidth = 0, recommendedHeight = 0;
    g_vr.system->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);
    HmdVector2_t gazeLeft = {{0, 0}}, gazeRight = {{0, 0}};
    bool gazeCentre = g_vr.system->GetEyeTrackedFoveationCenter &&
                      g_vr.system->GetEyeTrackedFoveationCenter(&gazeLeft, &gazeRight);
    Log("lens: headset %s %s, %.0f Hz, SteamVR's own render size %ux%u per eye (%ux%u side by side), eye-gaze interaction "
        "%s, eye-tracked foveation centre %s, hidden area %.0f%% / %.0f%% of the flat image (left / right)",
        maker, model, refresh, recommendedWidth, recommendedHeight, recommendedWidth * 2, recommendedHeight,
        gazeInteraction ? "yes" : "no", gazeCentre ? "available" : "not available", HiddenAreaFraction(0) * 100.0,
        HiddenAreaFraction(1) * 100.0);
    return 0;
}

void StartLensReportThread()
{
    if (!g_cfg.foveationReport || !g_logEnabled) // (it only writes a log line)
        return;
    HANDLE thread = CreateThread(NULL, 0, LensReportThread, NULL, 0, NULL);
    if (thread)
        CloseHandle(thread);
}
