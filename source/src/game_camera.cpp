// Hooks on the game's own camera hand-over: the game culls with a frustum around its own camera, and this gives it one around the head.
#include "game_camera.h"
#include <cmath>
#include "config.h"
#include "core.h"
#include "game_mode.h"
#include "inline_hook.h"
#include "vr_runtime.h"

namespace {

// Addresses in the Steam build of Blur.exe (image size, the Steam stub's entry point and the linker time stamp are checked).
const DWORD kImageSize = 0x00EB1000, kEntryRva = 0x00E5B2ED, kTimeStamp = 0x4BFC8EA9;

// The render view class' two camera setters.  SetCameraData(view, T) copies the 0x3d0-byte camera data T (matrices, ...) into the
// view; SetCamera(view, camera, flag) copies a 0x2b0-byte camera description into it.  The main view is given its camera by the pair
// of calls at 0x42524a / 0x425258, the depth pre-pass view by 0x4252d3 / 0x4252f0 (NOTES section 25).
const DWORD kSetCameraDataVa = 0x007A2CB0, kSetCameraVa = 0x007A2C40;
const BYTE kSetCameraDataBytes[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x18}; // push ebp / mov ebp,esp / and esp,-16 / sub esp,0x18
const BYTE kSetCameraBytes[7] = {0x8B, 0xC1, 0x53, 0x8B, 0x5C, 0x24, 0x08};                 // mov eax,ecx / push ebx / mov ebx,[esp+8]
const size_t kCameraSize = 0x2B0;
const double kPi = 3.14159265358979323846;

typedef void(__attribute__((fastcall)) * SetCameraDataFn)(void* self, void* edx, const void* data);
typedef void(__attribute__((fastcall)) * SetCameraFn)(void* self, void* edx, const void* camera, int flag);
SetCameraDataFn g_origSetCameraData = NULL;
SetCameraFn g_origSetCamera = NULL;

bool IsKnownBlurBuild()
{
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage == kImageSize && nt->OptionalHeader.AddressOfEntryPoint == kEntryRva &&
           nt->FileHeader.TimeDateStamp == kTimeStamp;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The culling frustum (NOTES section 25).  The 0x2b0-byte camera description SetCamera() receives is the world-space frustum the game
// culls with: [0] = 6 (planes in use), a plane through the camera facing forward at +0x10 and its absolute value at +0x20, 16 plane
// slots of {inward normal, d} from +0x30 (left, right, bottom, top, far, near in the first six), -|normal| for each slot from +0x130,
// and eight more points at +0x230 that do not belong to this frustum (left alone).  A point is inside when n . x + d >= 0.
// The rendering camera is SetCameraData's struct and is not touched: what changes is only what gets drawn.
// ---------------------------------------------------------------------------------------------------------------------------------
const DWORD kRetCameraDataMain[] = {0x0042524F, 0x004252D8, 0x00C30713}; // after SetCameraData: main view, depth pre-pass, a third view of the same camera
const DWORD kRetCameraMain[] = {0x0042525D, 0x004252F5, 0x00C30720};     // after SetCamera, same three views

bool IsOneOf(const void* ret, const DWORD* list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if ((DWORD)(uintptr_t)ret == list[i])
            return true;
    return false;
}

// What SetCameraData told us about the game's camera for the view that gets its SetCamera next: position, axes (columns of the
// view matrix at T+0x40, row vectors), near and far distance.
struct CameraSnapshot {
    void* view = NULL;
    bool valid = false;
    double pos[3] = {0, 0, 0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};
    double nearDist = 0.1, farDist = 1000;
} g_snapshot;

double Dot3(const double* a, const double* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// From the float struct SetCameraData receives.  False when it does not look like a camera.
bool SnapshotFromCameraData(const float* t, CameraSnapshot& out)
{
    const float* v = t + 0x40 / 4; // the view matrix: p_camera = p_world * R + t, so the camera's axes are the columns of R
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(v[i]) || fabsf(v[i]) > 1e7f)
            return false;
    for (int i = 0; i < 3; i++) {
        out.right[i] = v[0 + i * 4];
        out.up[i] = v[1 + i * 4];
        out.fwd[i] = v[2 + i * 4];
        out.pos[i] = t[0x14 / 4 + i];
    }
    if (!std::isfinite(out.pos[0]) || !std::isfinite(out.pos[1]) || !std::isfinite(out.pos[2]))
        return false;
    if (fabs(Dot3(out.right, out.right) - 1) > 0.02 || fabs(Dot3(out.up, out.up) - 1) > 0.02 || fabs(Dot3(out.fwd, out.fwd) - 1) > 0.02 ||
        fabs(Dot3(out.right, out.up)) > 0.02 || fabs(Dot3(out.right, out.fwd)) > 0.02 || fabs(Dot3(out.up, out.fwd)) > 0.02)
        return false;
    out.nearDist = t[3];
    out.farDist = t[4];
    return out.nearDist > 0 && out.farDist > out.nearDist && out.farDist < 1e6;
}

struct CullPlanes {
    double normal[6][3], d[6]; // left, right, bottom, top, far, near (inward normals, n . x + d >= 0)
    double forward[3], forwardD; // the forward plane through the head
    double yawDeg, tangents[4];  // for the log: the head's yaw against the game camera; min x, max x, min y, max y after the margin
};

struct CullSettings {
    double marginDeg, pullBack;
};

void Column(const Matrix4& m, int j, double* v)
{
    v[0] = m.m[0][j];
    v[1] = m.m[1][j];
    v[2] = m.m[2][j];
}

// The frustum around the head: the axes of the head pose (eyeCameraDelta[2]: the columns of its 3x3 are the head's axes in the game
// camera's space) and the extent that both eyes need (their four corner rays, seen from the head, widened by the margin).  The apex
// is moved back by pullBack so that the eyes' positions and a lean are covered too.
bool ComputeCullPlanes(const CameraSnapshot& cam, const Matrix4& head, const Matrix4 eyes[2], const EyeBox boxes[2], const CullSettings& s, CullPlanes& out)
{
    double hx[3], hy[3], hz[3];
    Column(head, 0, hx);
    Column(head, 1, hy);
    Column(head, 2, hz);
    double t[3] = {head.m[3][0], head.m[3][1], head.m[3][2]}, hp[3]; // head position in the game camera's space: -t * R^T
    for (int k = 0; k < 3; k++)
        hp[k] = -(t[0] * head.m[k][0] + t[1] * head.m[k][1] + t[2] * head.m[k][2]);
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (int e = 0; e < 2; e++) {
        double ex[3], ey[3], ez[3];
        Column(eyes[e], 0, ex);
        Column(eyes[e], 1, ey);
        Column(eyes[e], 2, ez);
        for (int cx = 0; cx < 2; cx++)
            for (int cy = 0; cy < 2; cy++) {
                double tx = cx ? boxes[e].right : boxes[e].left, ty = cy ? boxes[e].up : -boxes[e].down, ray[3];
                for (int i = 0; i < 3; i++)
                    ray[i] = tx * ex[i] + ty * ey[i] + ez[i];
                double x = Dot3(ray, hx), y = Dot3(ray, hy), z = Dot3(ray, hz);
                if (z < 0.1)
                    return false;
                minX = fmin(minX, x / z);
                maxX = fmax(maxX, x / z);
                minY = fmin(minY, y / z);
                maxY = fmax(maxY, y / z);
            }
    }
    const double limit = 85.0 * kPi / 180.0, margin = s.marginDeg * kPi / 180.0;
    auto widen = [&](double tangent, double sign) { return tan(fmax(-limit, fmin(limit, atan(tangent) + sign * margin))); };
    minX = widen(minX, -1);
    maxX = widen(maxX, 1);
    minY = widen(minY, -1);
    maxY = widen(maxY, 1);
    out.tangents[0] = minX;
    out.tangents[1] = maxX;
    out.tangents[2] = minY;
    out.tangents[3] = maxY;
    // the head's axes and position in the world
    double wx[3], wy[3], wz[3], pos[3];
    for (int i = 0; i < 3; i++) {
        wx[i] = hx[0] * cam.right[i] + hx[1] * cam.up[i] + hx[2] * cam.fwd[i];
        wy[i] = hy[0] * cam.right[i] + hy[1] * cam.up[i] + hy[2] * cam.fwd[i];
        wz[i] = hz[0] * cam.right[i] + hz[1] * cam.up[i] + hz[2] * cam.fwd[i];
        pos[i] = cam.pos[i] + hp[0] * cam.right[i] + hp[1] * cam.up[i] + hp[2] * cam.fwd[i];
    }
    out.yawDeg = acos(fmax(-1.0, fmin(1.0, Dot3(wz, cam.fwd)))) * 180.0 / kPi;
    double apex[3];
    for (int i = 0; i < 3; i++)
        apex[i] = pos[i] - wz[i] * s.pullBack;
    // normals in the head's frame: inside is x - minX z >= 0, maxX z - x >= 0, y - minY z >= 0, maxY z - y >= 0
    const double headNormals[4][3] = {{1, 0, -minX}, {-1, 0, maxX}, {0, 1, -minY}, {0, -1, maxY}};
    for (int p = 0; p < 4; p++) {
        double len = sqrt(Dot3(headNormals[p], headNormals[p]));
        for (int i = 0; i < 3; i++)
            out.normal[p][i] = (headNormals[p][0] * wx[i] + headNormals[p][1] * wy[i] + headNormals[p][2] * wz[i]) / len;
        out.d[p] = -Dot3(out.normal[p], apex);
    }
    for (int i = 0; i < 3; i++) {
        out.normal[4][i] = -wz[i]; // far
        out.normal[5][i] = wz[i];  // near
        out.forward[i] = wz[i];
    }
    out.d[4] = Dot3(wz, pos) + cam.farDist;
    out.d[5] = -Dot3(wz, pos) - (cam.nearDist - s.pullBack);
    out.forwardD = -Dot3(wz, pos);
    return true;
}

void WritePlane(float* camera, int offset, const double* n, double d)
{
    float* p = camera + offset / 4;
    p[0] = (float)n[0];
    p[1] = (float)n[1];
    p[2] = (float)n[2];
    p[3] = (float)d;
}

void WriteNegAbs(float* camera, int slot, const double* n)
{
    float* p = camera + (0x130 + 0x10 * slot) / 4;
    p[0] = -(float)fabs(n[0]);
    p[1] = -(float)fabs(n[1]);
    p[2] = -(float)fabs(n[2]);
    p[3] = 0;
}

void WriteCullPlanes(float* camera, const CullPlanes& planes)
{
    WritePlane(camera, 0x10, planes.forward, planes.forwardD);
    float* abs = camera + 0x20 / 4;
    abs[0] = fabsf(camera[0x10 / 4]);
    abs[1] = fabsf(camera[0x10 / 4 + 1]);
    abs[2] = fabsf(camera[0x10 / 4 + 2]);
    abs[3] = fabsf(camera[0x10 / 4 + 3]);
    for (int p = 0; p < 6; p++) {
        WritePlane(camera, 0x30 + 0x10 * p, planes.normal[p], planes.d[p]);
        WriteNegAbs(camera, p, planes.normal[p]);
    }
}

bool HeadFrustumWanted()
{
    return g_cfg.cullHeadFrustum && g_vr.active && g_vr.poseValid && IsRaceMode();
}

// The camera description for a main-camera SetCamera call: the game's own unless the head frustum is on and works out.
const void* CameraForCulling(void* view, const void* camera, BYTE* scratch)
{
    static unsigned uses = 0, failures = 0;
    if (!HeadFrustumWanted() || !g_snapshot.valid || g_snapshot.view != view)
        return camera;
    CullSettings settings = {g_cfg.cullMarginDeg, g_cfg.cullPullBack};
    CullPlanes planes;
    if (!ComputeCullPlanes(g_snapshot, g_vr.eyeCameraDelta[2], g_vr.eyeCameraDelta, g_vr.eyeBox, settings, planes)) {
        if (failures++ < 5)
            Log("head frustum: could not be worked out from the head pose, the game's own frustum is used");
        return camera;
    }
    memcpy(scratch, camera, kCameraSize);
    WriteCullPlanes((float*)scratch, planes);
    if (uses++ < 3 || uses % 3000 == 0)
        Log("head frustum (use %u): head %.1f deg off the game camera, tangents x %.3f..%.3f y %.3f..%.3f, margin %.1f deg, pull back %.2f m", uses,
            planes.yawDeg, planes.tangents[0], planes.tangents[1], planes.tangents[2], planes.tangents[3], g_cfg.cullMarginDeg, g_cfg.cullPullBack);
    return scratch;
}

void __attribute__((fastcall)) H_SetCameraData(void* self, void* edx, const void* data)
{
    const void* ret = __builtin_return_address(0);
    if (IsOneOf(ret, kRetCameraDataMain, sizeof(kRetCameraDataMain) / sizeof(DWORD))) {
        CameraSnapshot snapshot;
        snapshot.view = self;
        snapshot.valid = SnapshotFromCameraData((const float*)data, snapshot);
        g_snapshot = snapshot;
    }
    g_origSetCameraData(self, edx, data);
}

void __attribute__((fastcall)) H_SetCamera(void* self, void* edx, const void* camera, int flag)
{
    const void* ret = __builtin_return_address(0);
    if (IsOneOf(ret, kRetCameraMain, sizeof(kRetCameraMain) / sizeof(DWORD))) {
        alignas(16) static BYTE scratch[kCameraSize]; // (the callee copies it at once; the game's render thread is the only caller)
        camera = CameraForCulling(self, camera, scratch);
    }
    g_origSetCamera(self, edx, camera, flag);
}

} // namespace

void GameCameraPoll()
{
    static bool tried = false;
    if (tried || !g_cfg.cullHeadFrustum || !g_cfg.enabled)
        return;
    tried = true;
    if (!IsKnownBlurBuild()) {
        Log("game camera: this Blur.exe is not the build the addresses were read from, the game's own culling is left as it is");
        return;
    }
    g_origSetCameraData = (SetCameraDataFn)InstallInlineHook((void*)(uintptr_t)kSetCameraDataVa, kSetCameraDataBytes,
                                                              sizeof(kSetCameraDataBytes), (void*)H_SetCameraData);
    g_origSetCamera = (SetCameraFn)InstallInlineHook((void*)(uintptr_t)kSetCameraVa, kSetCameraBytes, sizeof(kSetCameraBytes), (void*)H_SetCamera);
    Log("game camera: SetCameraData hook %s, SetCamera hook %s", g_origSetCameraData ? "installed" : "FAILED", g_origSetCamera ? "installed" : "FAILED");
}
