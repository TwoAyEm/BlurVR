// Hooks on the game's own camera hand-over: the game culls with a frustum around its own camera, and this gives it one around the head.
// The two functions are found by signature in whatever build of Blur.exe is running (game_signatures.cpp) and the frustum layout is
// checked against the game's own planes before anything is changed (NOTES.md section 25).
#include "game_camera.h"
#include <cmath>
#include "camera.h"
#include "config.h"
#include "core.h"
#include "game_mode.h"
#include "game_signatures.h"
#include "inline_hook.h"
#include "vr_runtime.h"

namespace {

const double kPi = 3.14159265358979323846;
const size_t kScratchSize = 0x1000; // room for any camera description SetCamera copies (its size is read from its code: 0x2b0 in the Steam build)

typedef void(__attribute__((fastcall)) * SetCameraDataFn)(void* self, void* edx, const void* data);
typedef void(__attribute__((fastcall)) * SetCameraFn)(void* self, void* edx, const void* camera, int flag);
SetCameraDataFn g_origSetCameraData = NULL;
SetCameraFn g_origSetCamera = NULL;
size_t g_cameraBytes = 0x2B0;
bool g_hooked = false;

// ---------------------------------------------------------------------------------------------------------------------------------
// The culling frustum (NOTES section 25).  The camera description SetCamera() receives is the world-space frustum the game culls with:
// [0] = 6 (planes in use), a plane through the camera facing forward at +0x10 and its absolute value at +0x20, 16 plane slots of
// {inward normal, d} from +0x30 (left, right, bottom, top, far, near in the first six), -|normal| for each slot from +0x130, and eight
// more points at +0x230 that do not belong to this frustum (left alone).  A point is inside when n . x + d >= 0.  The rendering camera
// is SetCameraData's struct and is not touched: what changes is only what gets drawn.
// ---------------------------------------------------------------------------------------------------------------------------------

// What SetCameraData told us about the game's camera for the view that gets its SetCamera next: position, axes (columns of the
// view matrix at T+0x40, row vectors), near and far distance, field of view and aspect.
struct CameraSnapshot {
    void* view = NULL;
    const void* data = NULL; // the struct itself (identifies the camera from call to call)
    bool valid = false;      // looks like camera data
    bool isMain = false;     // and is the camera the player sees
    double mirrorDepth = 0;  // (for the log) how far under the main camera it is
    int mirrorAxis = -1;     // not the main camera but its mirror in a planar reflection: the axis it turns round (MirrorFlipAxis), else -1
    double pos[3] = {0, 0, 0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};
    double nearDist = 0.1, farDist = 1000, fov = 1, aspect = 1;
    double projX = 1, projY = 1; // P[0][0], P[1][1] (cot of the half angles)
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
    out.fov = t[0];
    out.aspect = t[1];
    out.projX = t[0xC0 / 4];
    out.projY = t[(0xC0 + 0x14) / 4];
    return out.nearDist > 0 && out.farDist > out.nearDist && out.farDist < 1e6;
}

// The camera a frustum description belongs to: ComputeCullPlanes backwards, for a symmetric frustum with no head and no margin.  The planar reflection view is given
// its frustum (SetCamera, Blur.exe 0x4279ED) before its camera data (SetCameraData, 0x4279FE), so at the time of the first call the description is all there is
// of this frame's reflection camera.  The apex is where the left, right and bottom planes meet, the axes come from the plane normals (left minus right is the x
// axis, bottom minus top the y axis, the forward plane's normal the z axis) and the field of view from their slopes.  False when the planes are not such a frustum.
bool SnapshotFromDescription(const float* d, CameraSnapshot& out)
{
    int planesInUse;
    memcpy(&planesInUse, d, 4);
    if (planesInUse != 6)
        return false;
    double n[7][3], dist[7];
    static const int offsets[7] = {0x30, 0x40, 0x50, 0x60, 0x10, 0x70, 0x80}; // left, right, bottom, top, forward, far, near
    for (int k = 0; k < 7; k++)
        for (int i = 0; i < 4; i++) {
            float v = d[offsets[k] / 4 + i];
            if (!std::isfinite(v) || fabsf(v) > 1e7f)
                return false;
            if (i < 3)
                n[k][i] = v;
            else
                dist[k] = v;
        }
    for (int k = 0; k < 7; k++)
        if (fabs(Dot3(n[k], n[k]) - 1) > 0.02)
            return false;
    double r[3], u[3], f[3] = {n[4][0], n[4][1], n[4][2]};
    for (int i = 0; i < 3; i++) {
        r[i] = n[0][i] - n[1][i];
        u[i] = n[2][i] - n[3][i];
    }
    double rl = sqrt(Dot3(r, r)), ul = sqrt(Dot3(u, u));
    if (rl < 1e-3 || ul < 1e-3)
        return false;
    for (int i = 0; i < 3; i++) {
        r[i] /= rl;
        u[i] /= ul;
    }
    if (fabs(Dot3(r, u)) > 0.02 || fabs(Dot3(r, f)) > 0.02 || fabs(Dot3(u, f)) > 0.02)
        return false;
    double tanH = Dot3(n[0], f) / Dot3(n[0], r), tanV = Dot3(n[2], f) / Dot3(n[2], u);
    if (!(tanH > 0.05 && tanH < 20 && tanV > 0.05 && tanV < 20))
        return false;
    // the apex: left, right and bottom planes meet there (Cramer's rule)
    const double* a = n[0];
    const double* b = n[1];
    const double* c = n[2];
    double rhs[3] = {-dist[0], -dist[1], -dist[2]};
    double det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    if (fabs(det) < 1e-6)
        return false;
    double pos[3];
    for (int axis = 0; axis < 3; axis++) {
        double m[3][3];
        for (int row = 0; row < 3; row++) {
            const double* source = row == 0 ? a : (row == 1 ? b : c);
            for (int col = 0; col < 3; col++)
                m[row][col] = (col == axis) ? rhs[row] : source[col];
        }
        pos[axis] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
    }
    double tolerance = 0.05 + 1e-4 * fmax(fabs(pos[0]), fmax(fabs(pos[1]), fabs(pos[2])));
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || fabs(Dot3(n[3], pos) + dist[3]) > tolerance || fabs(Dot3(f, pos) + dist[4]) > tolerance)
        return false;
    for (int i = 0; i < 3; i++) {
        out.right[i] = r[i];
        out.up[i] = u[i];
        out.fwd[i] = f[i];
        out.pos[i] = pos[i];
    }
    out.fov = 2 * atan(tanV);
    out.aspect = tanH / tanV;
    out.nearDist = -dist[6] - Dot3(f, pos);
    out.farDist = dist[5] - Dot3(f, pos);
    out.valid = out.nearDist > 0 && out.farDist > out.nearDist && out.farDist < 1e6;
    return out.valid;
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

// ---------------------------------------------------------------------------------------------------------------------------------
// Is this layout what we think it is?  Rebuild the game's own frustum from its camera data (identity head, its own field of view, no
// margin, no pull back) and compare it, number by number, with the frustum the game passed.  A build whose structs are laid out
// differently does not pass, and then nothing is written.
// ---------------------------------------------------------------------------------------------------------------------------------
struct LayoutCheck {
    bool ok;
    double worstError; // relative, over the compared numbers
    int worstOffset;   // byte offset in the description (-1: a different kind of mismatch)
    const char* reason;
};

LayoutCheck CheckFrustumLayout(const CameraSnapshot& cam, const float* description)
{
    LayoutCheck result = {false, 0, -1, ""};
    if (!(cam.fov > 0.05 && cam.fov < 3.0 && cam.aspect > 0.2 && cam.aspect < 6.0)) {
        result.reason = "the camera data has no field of view and aspect where expected";
        return result;
    }
    int planesInUse;
    memcpy(&planesInUse, description, 4);
    if (planesInUse != 6) {
        result.reason = "the frustum does not say it has 6 planes";
        return result;
    }
    double tanV = tan(cam.fov / 2), tanH = tanV * cam.aspect;
    EyeBox box = {-tanH, tanH, tanV, tanV};
    EyeBox boxes[2] = {box, box};
    Matrix4 identity = Identity();
    Matrix4 eyes[2] = {identity, identity};
    CullSettings none = {0, 0};
    CullPlanes planes;
    if (!ComputeCullPlanes(cam, identity, eyes, boxes, none, planes)) {
        result.reason = "the game's own frustum could not be rebuilt";
        return result;
    }
    float rebuilt[0x2B0 / 4];
    memcpy(rebuilt, description, sizeof(rebuilt));
    WriteCullPlanes(rebuilt, planes);
    static const int offsets[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x130, 0x140, 0x150, 0x160, 0x170, 0x180};
    for (int offset : offsets)
        for (int k = 0; k < 4; k++) {
            double game = description[offset / 4 + k], ours = rebuilt[offset / 4 + k];
            double error = fabs(game - ours) / fmax(1.0, fabs(game));
            if (!std::isfinite(error))
                error = 1e9;
            if (error > result.worstError) {
                result.worstError = error;
                result.worstOffset = offset + 4 * k;
            }
        }
    result.ok = result.worstError <= 2e-3;
    result.reason = result.ok ? "" : "the planes do not match the camera's own";
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Which camera is the one the player sees?  The mod knows the main pass camera from its draws (g_mainCamera); the camera data that
// looks like it - not mirrored, same direction, same projection - is it.  The views fed from the same struct follow it at once.
// ---------------------------------------------------------------------------------------------------------------------------------
bool LooksLikeMainCamera(const CameraSnapshot& cam)
{
    if (!g_mainCamera.valid)
        return false;
    double upCrossFwd[3] = {cam.up[1] * cam.fwd[2] - cam.up[2] * cam.fwd[1], cam.up[2] * cam.fwd[0] - cam.up[0] * cam.fwd[2],
                            cam.up[0] * cam.fwd[1] - cam.up[1] * cam.fwd[0]};
    if (Dot3(cam.right, upCrossFwd) < 0.5) // a mirrored camera (the planar reflection) has the other handedness
        return false;
    if (Dot3(cam.fwd, g_mainCamera.axes.forward) < 0.98 || Dot3(cam.up, g_mainCamera.axes.up) < 0.98) // (the rear-view camera looks backwards)
        return false;
    // the main pass alternates between 60 and 62.4 degrees: 10 % covers that and excludes the other views' projections
    return fabs(cam.projX - g_mainCamera.xScale) < 0.1 * g_mainCamera.xScale && fabs(cam.projY - g_mainCamera.yScale) < 0.1 * g_mainCamera.yScale;
}

struct Counters {
    unsigned dataCalls, dataValid, dataMain, cameraCalls, rewrites, layoutFailures, reflectionViews, reflectionRewrites, reflectionLayoutFailures;
} g_counters;

// The pose of the main camera, published for other modules (ai_hud.cpp).  A sequence lock: the writer is the game's thread, the readers are
// the thread that presents (or the game's own thread, for the hooks of ai_hud.cpp).
volatile LONG g_poseSeq = 0;
MainCameraPose g_pose;
bool g_poseValid = false;

void PublishPose(const CameraSnapshot& cam)
{
    MainCameraPose pose;
    for (int i = 0; i < 3; i++) {
        pose.pos[i] = cam.pos[i];
        pose.right[i] = cam.right[i];
        pose.up[i] = cam.up[i];
        pose.fwd[i] = cam.fwd[i];
    }
    pose.projX = cam.projX;
    pose.projY = cam.projY;
    pose.frame = g_vr.frameCount;
    PublishMainCameraPose(pose);
}

const void* g_mainData = NULL;    // the camera data struct last accepted as the main camera
unsigned g_mainVerifiedAt = 0;    // data call number of that acceptance
const unsigned kReverifyEvery = 600; // data calls

struct LayoutRecord {
    const void* description;
    bool ok;
};
LayoutRecord g_layouts[8];
unsigned g_layoutNext = 0;

bool HeadFrustumWanted()
{
    return g_vr.active && g_vr.poseValid && IsRaceMode();
}

// The planar reflection (water, wet roads): the game gives its own view a camera that is the main camera mirrored in a horizontal plane under it, with the
// position and the forward axis mirrored and the up axis turned round (so that the rotation stays proper), and a wider field of view.  That view is drawn
// per eye like the main one (stereo_scene.cpp) but culled with the game's own frustum around that one mirrored camera, so with the head turned the water
// showed the sky where a wall should be, with a straight edge where the frustum ends.  Seen from the main camera's pose (the last one published) it is
// recognised by its axes, and the culling is done around the mirrored head.
int g_reflectionAxis = -1;
unsigned long long g_reflectionAxisFrame = 0;

int ReflectionAxisOf(const CameraSnapshot& cam)
{
    MainCameraPose main;
    if (!GetMainCameraPose(main) || g_vr.frameCount > main.frame + 3)
        return -1;
    if (cam.pos[1] > main.pos[1] - 0.3 || fabs(cam.pos[0] - main.pos[0]) > 15 || fabs(cam.pos[2] - main.pos[2]) > 15)
        return -1; // (the mirror image is straight under the camera, the water under the car)
    const double mirrored[3][3] = {{cam.right[0], cam.right[1], cam.right[2]}, {cam.up[0], cam.up[1], cam.up[2]}, {cam.fwd[0], cam.fwd[1], cam.fwd[2]}};
    const double camera[3][3] = {{main.right[0], main.right[1], main.right[2]}, {main.up[0], main.up[1], main.up[2]}, {main.fwd[0], main.fwd[1], main.fwd[2]}};
    return MirrorFlipAxis(mirrored, camera);
}

// The description for the reflection view's SetCamera: a frustum around the mirrored head.  The reflection pass draws each eye through the mirrored eye
// camera (the head's delta conjugated by the flip, stereo_scene.cpp), so the frustum is the one ComputeCullPlanes gives for the reflection camera with the
// conjugated deltas.
const void* ReflectionCameraForCulling(const CameraSnapshot& cam, const void* camera, BYTE* scratch)
{
    static unsigned uses = 0, failures = 0;
    if (!HeadFrustumWanted() || cam.mirrorAxis < 0)
        return camera;
    LayoutRecord* record = NULL;
    for (LayoutRecord& r : g_layouts)
        if (r.description == camera)
            record = &r;
    if (!record) {
        LayoutCheck check = CheckFrustumLayout(cam, (const float*)camera);
        record = &g_layouts[g_layoutNext++ % 8];
        record->description = camera;
        record->ok = check.ok;
        if (check.ok)
            Log("reflection frustum: layout verified against the game's own planes (largest difference %.1e), description %p", check.worstError, camera);
        else {
            g_counters.reflectionLayoutFailures++;
            Log("reflection frustum: the layout of the reflection camera is not the expected one (%s; largest difference %.3g at +0x%x of the description %p), the "
                "game culls the reflection with its own frustum",
                check.reason, check.worstError, check.worstOffset < 0 ? 0 : check.worstOffset, camera);
        }
    }
    if (!record->ok)
        return camera;
    const int axis = cam.mirrorAxis;
    Matrix4 head = FlipConjugate(YawOnlyDelta(g_vr.eyeCameraDelta[2]), axis),
            eyes[2] = {FlipConjugate(YawOnlyDelta(g_vr.eyeCameraDelta[0]), axis), FlipConjugate(YawOnlyDelta(g_vr.eyeCameraDelta[1]), axis)}; // (what the pass is drawn with, stereo_scene.cpp)
    CullSettings settings = {g_cfg.cullMarginDeg, g_cfg.cullPullBack};
    CullPlanes planes;
    if (!ComputeCullPlanes(cam, head, eyes, g_vr.eyeBox, settings, planes)) {
        if (failures++ < 5)
            Log("reflection frustum: could not be worked out from the mirrored head pose, the game's own frustum is used");
        return camera;
    }
    memcpy(scratch, camera, g_cameraBytes);
    WriteCullPlanes((float*)scratch, planes);
    g_counters.reflectionRewrites++;
    if (uses++ < 3 || uses % 3000 == 0)
        Log("reflection frustum (use %u): mirrored camera turned round in axis %c, %.1f m under the main camera, head %.1f deg off it, tangents x %.3f..%.3f y %.3f..%.3f", uses,
            "XYZ"[axis], cam.mirrorDepth, planes.yawDeg, planes.tangents[0], planes.tangents[1], planes.tangents[2], planes.tangents[3]);
    return scratch;
}

// The camera description for a main-camera SetCamera call: the game's own unless the VR half is running, the layout is the expected one
// and it works out.
const void* CameraForCulling(void* view, const void* camera, BYTE* scratch)
{
    static unsigned uses = 0, failures = 0;
    if (!HeadFrustumWanted() || !g_snapshot.valid || !g_snapshot.isMain || g_snapshot.view != view)
        return camera;
    LayoutRecord* record = NULL;
    for (LayoutRecord& r : g_layouts)
        if (r.description == camera)
            record = &r;
    if (!record) {
        LayoutCheck check = CheckFrustumLayout(g_snapshot, (const float*)camera);
        record = &g_layouts[g_layoutNext++ % 8];
        record->description = camera;
        record->ok = check.ok;
        if (check.ok)
            Log("head frustum: layout verified against the game's own planes (largest difference %.1e), description %p", check.worstError, camera);
        else {
            g_counters.layoutFailures++;
            Log("head frustum: the layout of this build is not the expected one (%s; largest difference %.3g at +0x%x of the description %p), "
                "the game culls with its own frustum for this camera",
                check.reason, check.worstError, check.worstOffset < 0 ? 0 : check.worstOffset, camera);
        }
    }
    if (!record->ok)
        return camera;
    CullSettings settings = {g_cfg.cullMarginDeg, g_cfg.cullPullBack};
    CullPlanes planes;
    if (!ComputeCullPlanes(g_snapshot, g_vr.eyeCameraDelta[2], g_vr.eyeCameraDelta, g_vr.eyeBox, settings, planes)) {
        if (failures++ < 5)
            Log("head frustum: could not be worked out from the head pose, the game's own frustum is used");
        return camera;
    }
    memcpy(scratch, camera, g_cameraBytes);
    WriteCullPlanes((float*)scratch, planes);
    g_counters.rewrites++;
    if (uses++ < 3 || uses % 3000 == 0)
        Log("head frustum (use %u): head %.1f deg off the game camera, tangents x %.3f..%.3f y %.3f..%.3f, margin %.1f deg, pull back %.2f m", uses,
            planes.yawDeg, planes.tangents[0], planes.tangents[1], planes.tangents[2], planes.tangents[3], g_cfg.cullMarginDeg, g_cfg.cullPullBack);
    return scratch;
}

void __attribute__((fastcall)) H_SetCameraData(void* self, void* edx, const void* data)
{
    CameraSnapshot snapshot;
    snapshot.view = self;
    snapshot.data = data;
    snapshot.valid = SnapshotFromCameraData((const float*)data, snapshot);
    g_counters.dataCalls++;
    if (snapshot.valid) {
        g_counters.dataValid++;
        if (data == g_mainData && g_counters.dataCalls - g_mainVerifiedAt < kReverifyEvery)
            snapshot.isMain = true; // fed from the struct that was accepted a moment ago
        else {
            snapshot.isMain = LooksLikeMainCamera(snapshot);
            if (snapshot.isMain) {
                g_mainData = data;
                g_mainVerifiedAt = g_counters.dataCalls;
            } else if (data == g_mainData)
                g_mainData = NULL;
        }
        if (snapshot.isMain) {
            g_counters.dataMain++;
            PublishPose(snapshot); // (only ai_hud.cpp reads the pose)
        }
    }
    g_snapshot = snapshot;
    g_origSetCameraData(self, edx, data);
}

void __attribute__((fastcall)) H_SetCamera(void* self, void* edx, const void* camera, int flag)
{
    g_counters.cameraCalls++;
    if (g_snapshot.valid && g_snapshot.isMain && g_snapshot.view == self) {
        alignas(16) static BYTE scratch[kScratchSize]; // (the callee copies it at once; the game's render thread is the only caller)
        camera = CameraForCulling(self, camera, scratch);
    } else if (HeadFrustumWanted()) {
        // a view that is given its frustum before its camera data, or any other view: is it the planar reflection?  (a mirror of the main camera, found from the frustum alone)
        CameraSnapshot candidate;
        if (SnapshotFromDescription((const float*)camera, candidate)) {
            candidate.mirrorAxis = ReflectionAxisOf(candidate);
            if (candidate.mirrorAxis >= 0) {
                MainCameraPose pose;
                if (GetMainCameraPose(pose))
                    candidate.mirrorDepth = pose.pos[1] - candidate.pos[1];
                g_reflectionAxis = candidate.mirrorAxis;
                g_reflectionAxisFrame = g_vr.frameCount;
                g_counters.reflectionViews++;
                alignas(16) static BYTE scratch[kScratchSize];
                camera = ReflectionCameraForCulling(candidate, camera, scratch);
            }
        }
    }
    g_snapshot.valid = false; // the snapshot is for the SetCamera that follows its SetCameraData, nothing else
    g_origSetCamera(self, edx, camera, flag);
}

// Every executable section of the loaded Blur.exe except the extra one of the Steam build.
void ScanModule(GameCameraScan& scan, DWORD& imageSize, DWORD& timeStamp)
{
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    imageSize = nt->OptionalHeader.SizeOfImage;
    timeStamp = nt->FileHeader.TimeDateStamp;
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[i].Name, ".bind", 8))
            continue;
        DWORD size = sections[i].Misc.VirtualSize ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;
        ScanForGameCameraCode(base + sections[i].VirtualAddress, size, scan);
    }
}

} // namespace

void PublishMainCameraPose(const MainCameraPose& pose)
{
    InterlockedIncrement(&g_poseSeq); // odd: being written
    MemoryBarrier();
    g_pose = pose;
    g_poseValid = true;
    MemoryBarrier();
    InterlockedIncrement(&g_poseSeq);
}

bool GetMainCameraPose(MainCameraPose& out)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        LONG before = g_poseSeq;
        if (before & 1)
            continue; // being written
        MemoryBarrier();
        MainCameraPose copy = g_pose;
        bool valid = g_poseValid;
        MemoryBarrier();
        if (before == g_poseSeq) {
            out = copy;
            return valid;
        }
    }
    return false;
}

int ReflectionFlipAxis()
{
    return (g_reflectionAxis >= 0 && g_vr.frameCount <= g_reflectionAxisFrame + 2) ? g_reflectionAxis : -1;
}

void GameCameraPoll()
{
    static unsigned frame = 0, tries = 0, nextTry = 1, reports = 0;
    frame++;
    if (g_hooked) { // a line now and then saying what the hooks saw: on another build this is what shows whether the camera is recognised
        if ((frame == 600 || frame % 12000 == 0) && reports++ < 6)
            Log("game camera: %u SetCameraData calls (%u looked like camera data, %u taken for the main camera), %u SetCamera calls, %u frustums "
                "rewritten, %u layout failures; reflection cameras seen %u, their frustums rewritten %u, layout failures %u",
                g_counters.dataCalls, g_counters.dataValid, g_counters.dataMain, g_counters.cameraCalls, g_counters.rewrites, g_counters.layoutFailures,
                g_counters.reflectionViews, g_counters.reflectionRewrites, g_counters.reflectionLayoutFailures);
        return;
    }
    if (!g_cfg.vr || tries >= 5 || frame < nextTry)
        return;
    tries++;
    nextTry = frame + 120; // (a build that unpacks its code late is looked at again a little later)
    GameCameraScan scan;
    InitGameCameraScan(scan);
    DWORD imageSize = 0, timeStamp = 0;
    ScanModule(scan, imageSize, timeStamp);
    GameCameraCode code;
    char why[768] = "";
    if (!ResolveGameCameraCode(scan, code, why, sizeof(why))) {
        Log("game camera: Blur.exe image 0x%x bytes, PE time stamp 0x%08x: the camera code was not found (%s) [try %u of 5]; the game culls with its own frustum",
            (unsigned)imageSize, (unsigned)timeStamp, why, tries);
        return;
    }
    BYTE expectedData[16], expectedCamera[16] = {};
    memcpy(expectedData, code.setCameraData, code.setCameraDataStolen);
    if (!code.cameraStub)
        memcpy(expectedCamera, code.setCamera, code.setCameraStolen);
    tries = 5; // found: no more looking, whatever the hooks do
    g_cameraBytes = code.cameraBytes;
    g_origSetCameraData = (SetCameraDataFn)InstallInlineHook((void*)code.setCameraData, expectedData, code.setCameraDataStolen, (void*)H_SetCameraData);
    if (code.cameraStub) // (the game's calls reach SetCamera through a stub: its target is rewritten, the body is called as it is)
        g_origSetCamera = (SetCameraFn)InstallStubHook(code.cameraStub, (BYTE*)code.cameraOperand, code.cameraBody, (void*)H_SetCamera);
    else
        g_origSetCamera = (SetCameraFn)InstallInlineHook((void*)code.setCamera, expectedCamera, code.setCameraStolen, (void*)H_SetCamera);
    static const char* const formNames[3] = {"tight pattern", "loose pattern", "call sites"};
    Log("game camera: Blur.exe image 0x%x bytes, PE time stamp 0x%08x: SetCameraData at %p (%s pattern) %s, SetCamera at %p (%s, "
        "description 0x%x bytes) %s",
        (unsigned)imageSize, (unsigned)timeStamp, code.setCameraData, code.dataForm ? "loose" : "tight", g_origSetCameraData ? "hooked" : "NOT HOOKED",
        code.setCamera, formNames[code.cameraForm < 0 || code.cameraForm > 2 ? 0 : code.cameraForm], (unsigned)code.cameraBytes, g_origSetCamera ? "hooked" : "NOT HOOKED");
    if (code.cameraForm == 2)
        Log("game camera: SetCamera was %s", code.how);
    if (g_origSetCameraData && g_origSetCamera)
        g_hooked = true;
    else if (g_origSetCameraData || g_origSetCamera) // one of the two only: the pairing breaks, undo nothing but never use half
        Log("game camera: only one of the two hooks could be installed; it forwards to the game unchanged");
}
