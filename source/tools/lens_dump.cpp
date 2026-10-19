// Dumps the headset's distortion function and hidden-area mesh to CSV files for offline layout studies
// (tools/lens_layouts.py).  SteamVR has to be running; the game does not.
//
//   python -m ziglang c++ -target x86-windows-gnu -O2 -std=c++17 -Isrc -Ithird_party tools/lens_dump.cpp -o lens_dump.exe
//   lens_dump.exe <output folder> [path\to\openvr_api.dll]
//
// Writes lens_eye0.csv / lens_eye1.csv ("# tangents ..." line, then panel_u,panel_v,tex_u,tex_v for a 65x65 grid, tex_u of -1e9
// = no value) and hidden_eye0.csv / hidden_eye1.csv (one triangle vertex u,v per line).
#include <windows.h>
#include <stdio.h>
#include <string>
#include "openvr_wrapper.h"

typedef intptr_t (*PFN_Init)(EVRInitError*, EVRApplicationType);
typedef intptr_t (*PFN_GetInterface)(const char*, EVRInitError*);
typedef void (*PFN_Shutdown)();
// the 8-byte struct comes back in EDX:EAX on 32-bit x86 (see lens_model.cpp)
typedef unsigned long long(__stdcall* PFN_HiddenAreaMeshRaw)(EVREye, EHiddenAreaMeshType);

static const int kGrid = 64;

int main(int argc, char** argv)
{
    std::string outDir = argc > 1 ? argv[1] : ".";
    const char* dllPath = argc > 2 ? argv[2] : "openvr_api.dll";
    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll) {
        fprintf(stderr, "cannot load %s\n", dllPath);
        return 1;
    }
    PFN_Init init = (PFN_Init)GetProcAddress(dll, "VR_InitInternal");
    PFN_GetInterface getInterface = (PFN_GetInterface)GetProcAddress(dll, "VR_GetGenericInterface");
    PFN_Shutdown shutdown = (PFN_Shutdown)GetProcAddress(dll, "VR_ShutdownInternal");
    if (!init || !getInterface) {
        fprintf(stderr, "openvr_api.dll has no VR_InitInternal / VR_GetGenericInterface\n");
        return 1;
    }
    EVRInitError error = EVRInitError_VRInitError_None;
    init(&error, EVRApplicationType_VRApplication_Utility);
    if (error != EVRInitError_VRInitError_None) {
        fprintf(stderr, "VR_InitInternal failed: %d (is SteamVR running?)\n", (int)error);
        return 1;
    }
    VR_IVRSystem_FnTable* system =
        (VR_IVRSystem_FnTable*)getInterface((std::string("FnTable:") + IVRSystem_Version).c_str(), &error);
    if (!system) {
        fprintf(stderr, "no IVRSystem: %d\n", (int)error);
        return 1;
    }
    for (int eye = 0; eye < 2; eye++) {
        float left, right, top, bottom;
        system->GetProjectionRaw((EVREye)eye, &left, &right, &top, &bottom);
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\lens_eye%d.csv", outDir.c_str(), eye);
        FILE* csv = fopen(path, "w");
        if (!csv) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        fprintf(csv, "# tangents left %.6f right %.6f top %.6f bottom %.6f grid %d\n", left, right, top, bottom, kGrid);
        fprintf(csv, "panel_u,panel_v,tex_u,tex_v\n");
        for (int i = 0; i <= kGrid; i++)
            for (int j = 0; j <= kGrid; j++) {
                DistortionCoordinates_t coordinates;
                memset(&coordinates, 0, sizeof(coordinates));
                float u = (float)j / kGrid, v = (float)i / kGrid;
                if (system->ComputeDistortion((EVREye)eye, u, v, &coordinates))
                    fprintf(csv, "%.6f,%.6f,%.6f,%.6f\n", u, v, coordinates.rfGreen[0], coordinates.rfGreen[1]);
                else
                    fprintf(csv, "%.6f,%.6f,-1e9,-1e9\n", u, v);
            }
        fclose(csv);

        PFN_HiddenAreaMeshRaw getMesh = (PFN_HiddenAreaMeshRaw)(void*)system->GetHiddenAreaMesh;
        unsigned long long raw = getMesh((EVREye)eye, EHiddenAreaMeshType_k_eHiddenAreaMesh_Standard);
        const HmdVector2_t* vertices = (const HmdVector2_t*)(uintptr_t)(unsigned)(raw & 0xFFFFFFFFull);
        unsigned triangles = (unsigned)(raw >> 32);
        snprintf(path, sizeof(path), "%s\\hidden_eye%d.csv", outDir.c_str(), eye);
        FILE* mesh = fopen(path, "w");
        if (mesh && vertices && triangles && triangles < 100000) {
            fprintf(mesh, "u,v\n");
            for (unsigned k = 0; k < triangles * 3; k++)
                fprintf(mesh, "%.6f,%.6f\n", vertices[k].v[0], vertices[k].v[1]);
        }
        if (mesh)
            fclose(mesh);
        printf("eye %d: tangents l %.3f r %.3f t %.3f b %.3f, hidden area %u triangles\n", eye, left, right, top, bottom,
               triangles);
    }
    if (shutdown)
        shutdown();
    return 0;
}
