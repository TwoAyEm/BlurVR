// Host check of the planar-reflection culling of src/game_camera.cpp (NOTES.md section 25): the reflection view's camera is recognised as the mirror of the main camera,
// the axis it turns round is found, and the frustum made for it around the mirrored head contains everything the mirrored eye cameras see.
// game_camera.cpp is included, not linked, so that the check can reach its internals; tools/reflection_cull_check.py builds it with the rest of src/.
#include "../src/game_camera.cpp"
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;
#define CHECK(cond, ...)                      \
    do {                                      \
        if (!(cond)) {                        \
            g_failures++;                     \
            printf("  FAIL: " __VA_ARGS__);   \
            printf("\n");                     \
        }                                     \
    } while (0)

static double Rand(double lo, double hi)
{
    return lo + (hi - lo) * (rand() / (double)RAND_MAX);
}
static void Normalize(double* v)
{
    double length = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; i++)
        v[i] /= length;
}
static void Cross(const double* a, const double* b, double* out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// A camera that looks along (cos pitch sin yaw, -sin pitch, cos pitch cos yaw), upright: right = up x forward in the game's left-handed frame.
static void MakeCamera(double yaw, double pitch, double axes[3][3])
{
    double* f = axes[2];
    f[0] = cos(pitch) * sin(yaw);
    f[1] = -sin(pitch);
    f[2] = cos(pitch) * cos(yaw);
    const double worldUp[3] = {0, 1, 0};
    Cross(worldUp, f, axes[0]);
    Normalize(axes[0]);
    Cross(f, axes[0], axes[1]);
    Normalize(axes[1]);
}

// The mirror of a camera in the plane y = h: position and forward mirrored, one axis turned round to keep the rotation proper.
static void Mirror(const double pos[3], const double axes[3][3], double h, int turned, double outPos[3], double out[3][3])
{
    outPos[0] = pos[0];
    outPos[1] = 2 * h - pos[1];
    outPos[2] = pos[2];
    for (int k = 0; k < 3; k++) {
        double sign = (k == turned) ? -1 : 1;
        out[k][0] = sign * axes[k][0];
        out[k][1] = -sign * axes[k][1];
        out[k][2] = sign * axes[k][2];
    }
}

static Matrix4 RigidDelta(double yaw, double pitch, double roll, const double t[3])
{
    // the rotation of a head in the game camera's space, row vectors (p_eye = p * D): rows are the images of the x, y and z axes
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
    double rp[3][3] = {{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}};
    double rr[3][3] = {{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}};
    double a[3][3], r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            a[i][j] = 0;
            for (int k = 0; k < 3; k++)
                a[i][j] += rr[i][k] * rp[k][j];
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            r[i][j] = 0;
            for (int k = 0; k < 3; k++)
                r[i][j] += a[i][k] * ry[k][j];
        }
    Matrix4 d = Identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            d.m[i][j] = r[i][j];
    for (int j = 0; j < 3; j++)
        d.m[3][j] = t[j];
    return d;
}

// The world position and axes of the camera that `delta` (camera space to eye camera space, row vectors) makes of a camera at `pos` with `axes`.
static void EyeWorld(const double pos[3], const double axes[3][3], const Matrix4& delta, double origin[3], double eyeAxes[3][3])
{
    double hp[3];
    for (int k = 0; k < 3; k++)
        hp[k] = -(delta.m[3][0] * delta.m[k][0] + delta.m[3][1] * delta.m[k][1] + delta.m[3][2] * delta.m[k][2]);
    for (int i = 0; i < 3; i++) {
        origin[i] = pos[i] + hp[0] * axes[0][i] + hp[1] * axes[1][i] + hp[2] * axes[2][i];
        for (int k = 0; k < 3; k++)
            eyeAxes[k][i] = delta.m[0][k] * axes[0][i] + delta.m[1][k] * axes[1][i] + delta.m[2][k] * axes[2][i];
    }
}

static CameraSnapshot Snapshot(const double pos[3], const double axes[3][3], double fov, double aspect)
{
    CameraSnapshot cam;
    for (int i = 0; i < 3; i++) {
        cam.pos[i] = pos[i];
        cam.right[i] = axes[0][i];
        cam.up[i] = axes[1][i];
        cam.fwd[i] = axes[2][i];
    }
    cam.valid = true;
    cam.fov = fov;
    cam.aspect = aspect;
    cam.nearDist = 0.1;
    cam.farDist = 1000;
    return cam;
}

int main()
{
    srand(12345);
    // ------------------------------------------------------------------------------------------------ the axis a mirrored camera turns round
    int found[3] = {0, 0, 0}, wrong = 0;
    for (int n = 0; n < 2000; n++) {
        double axes[3][3], pos[3] = {Rand(-500, 500), Rand(2, 6), Rand(-500, 500)};
        MakeCamera(Rand(-3.1, 3.1), Rand(-0.25, 0.25), axes);
        for (int turned = 0; turned < 3; turned++) {
            double mirroredPos[3], mirrored[3][3];
            Mirror(pos, axes, pos[1] - Rand(0.8, 3), turned, mirroredPos, mirrored);
            int axis = MirrorFlipAxis(mirrored, axes);
            if (axis == turned)
                found[turned]++;
            else
                wrong++;
        }
        // the rear-view camera (looks backwards, right and forward turned round) and a camera that is not a mirror at all
        double rear[3][3];
        for (int i = 0; i < 3; i++) {
            rear[0][i] = -axes[0][i];
            rear[1][i] = axes[1][i];
            rear[2][i] = -axes[2][i];
        }
        CHECK(MirrorFlipAxis(rear, axes) == -1, "the rear-view camera was taken for a mirror (n %d)", n);
        double sideways[3][3];
        MakeCamera(Rand(-3.1, 3.1), Rand(-0.25, 0.25), sideways);
        if (fabs(sideways[2][0] * axes[2][0] + sideways[2][2] * axes[2][2]) < 0.8)
            CHECK(MirrorFlipAxis(sideways, axes) == -1, "a camera turned away was taken for a mirror (n %d)", n);
    }
    printf("mirror axis: found right %d, up %d, forward %d of 2000 each, %d wrong\n", found[0], found[1], found[2], wrong);
    CHECK(found[0] == 2000 && found[1] == 2000 && found[2] == 2000 && wrong == 0, "the axis was not found every time");

    // ------------------------------------------------------------------------------------------------ the mirrored eye cameras and the frustum around them
    EyeBox boxes[2] = {{-1.541, 1.051, 1.407, 1.414}, {-1.050, 1.542, 1.409, 1.410}};
    unsigned trials = 0, positionWrongWithX = 0, outsideWithX = 0, points = 0;
    for (int n = 0; n < 300; n++) {
        double axes[3][3], pos[3] = {Rand(-500, 500), Rand(2, 6), Rand(-500, 500)};
        MakeCamera(Rand(-3.1, 3.1), Rand(-0.2, 0.2), axes);
        double h = pos[1] - Rand(0.8, 2.5), mirroredPos[3], mirrored[3][3];
        const int turned = 1; // Blur's reflection camera turns its up axis round
        Mirror(pos, axes, h, turned, mirroredPos, mirrored);
        double headT[3] = {Rand(-0.3, 0.3), Rand(-0.2, 0.2), Rand(-0.3, 0.3)};
        Matrix4 head = RigidDelta(Rand(-0.9, 0.9), Rand(-0.5, 0.5), Rand(-0.2, 0.2), headT);
        Matrix4 eyes[2];
        for (int e = 0; e < 2; e++) {
            double eyeT[3] = {headT[0] + (e ? -0.035 : 0.035), headT[1], headT[2]}; // (the eye is a little to one side of the head: a shift of the origin)
            eyes[e] = head;
            for (int j = 0; j < 3; j++)
                eyes[e].m[3][j] = eyeT[j];
        }
        for (int axis = 0; axis < 3; axis += 1) {
            if (axis == 2)
                continue; // (the forward flip is the same test as the others, the right and up flips are what matters)
            // positions: the mirrored eye must sit where the mirror image of the eye is
            for (int e = 0; e < 2; e++) {
                double origin[3], eyeAxes[3][3];
                EyeWorld(pos, axes, eyes[e], origin, eyeAxes);
                double expected[3] = {origin[0], 2 * h - origin[1], origin[2]};
                Matrix4 flipped = FlipConjugate(YawOnlyDelta(eyes[e]), axis); // (the eye stays where it is, only its pitch and roll are taken out)
                double mirroredOrigin[3], mirroredAxes[3][3];
                EyeWorld(mirroredPos, mirrored, flipped, mirroredOrigin, mirroredAxes);
                double error = fabs(mirroredOrigin[0] - expected[0]) + fabs(mirroredOrigin[1] - expected[1]) + fabs(mirroredOrigin[2] - expected[2]);
                if (axis == turned)
                    CHECK(error < 1e-6, "the mirrored eye is %.3g m from the mirror image of the eye with the right axis (trial %d)", error, n);
                else if (axis == 0 && error > 1e-3)
                    positionWrongWithX++;
            }
            // the frustum around the mirrored head must contain what the mirrored eye cameras see
            CameraSnapshot cam = Snapshot(mirroredPos, mirrored, 1.18, 1.8);
            Matrix4 mirroredHead = FlipConjugate(YawOnlyDelta(head), axis), mirroredEyes[2] = {FlipConjugate(YawOnlyDelta(eyes[0]), axis), FlipConjugate(YawOnlyDelta(eyes[1]), axis)};
            CullSettings settings = {6.0, 0.3};
            CullPlanes planes;
            CHECK(ComputeCullPlanes(cam, mirroredHead, mirroredEyes, boxes, settings, planes), "no frustum for trial %d", n);
            trials++;
            for (int e = 0; e < 2; e++) {
                double origin[3], eyeAxes[3][3];
                EyeWorld(mirroredPos, mirrored, FlipConjugate(YawOnlyDelta(eyes[e]), turned), origin, eyeAxes); // (the mirrored eye camera the pass draws, whatever axis the frustum was made with)
                for (int s = 0; s < 200; s++) {
                    double tx = Rand(boxes[e].left, boxes[e].right), ty = Rand(-boxes[e].down, boxes[e].up), z = Rand(0.2, 400), p[3];
                    for (int i = 0; i < 3; i++)
                        p[i] = origin[i] + z * (tx * eyeAxes[0][i] + ty * eyeAxes[1][i] + eyeAxes[2][i]);
                    bool inside = true;
                    for (int plane = 0; plane < 6; plane++)
                        if (planes.normal[plane][0] * p[0] + planes.normal[plane][1] * p[1] + planes.normal[plane][2] * p[2] + planes.d[plane] < -1e-3)
                            inside = false;
                    points++;
                    if (axis == turned)
                        CHECK(inside, "a point of the mirrored eye's view is outside the frustum (trial %d, eye %d)", n, e);
                    else if (!inside && axis == 0)
                        outsideWithX++;
                }
            }
        }
    }
    printf("frustum: %u trials, %u points; with the wrong axis (right instead of up) the eye was misplaced in %u of %u cases and %u points fell outside the frustum\n", trials,
           points, positionWrongWithX, 300 * 2, outsideWithX);
    CHECK(positionWrongWithX > 300, "the check would not notice a wrong axis (positions)");
    CHECK(outsideWithX > 0, "the check would not notice a wrong axis (frustum)");

    // ------------------------------------------------------------------------------------------------ the head without its pitch and roll
    {
        unsigned good = 0;
        for (int n = 0; n < 2000; n++) {
            double t[3] = {Rand(-0.5, 0.5), Rand(-0.4, 0.4), Rand(-0.5, 0.5)};
            Matrix4 delta = RigidDelta(Rand(-3.0, 3.0), Rand(-1.3, 1.3), Rand(-0.6, 0.6), t);
            Matrix4 yaw = YawOnlyDelta(delta);
            double origin[3], yawOrigin[3];
            for (int k = 0; k < 3; k++) {
                origin[k] = -(delta.m[3][0] * delta.m[k][0] + delta.m[3][1] * delta.m[k][1] + delta.m[3][2] * delta.m[k][2]);
                yawOrigin[k] = -(yaw.m[3][0] * yaw.m[k][0] + yaw.m[3][1] * yaw.m[k][1] + yaw.m[3][2] * yaw.m[k][2]);
            }
            double originError = fabs(origin[0] - yawOrigin[0]) + fabs(origin[1] - yawOrigin[1]) + fabs(origin[2] - yawOrigin[2]);
            double azimuth = atan2(delta.m[0][2], delta.m[2][2]), yawAzimuth = atan2(yaw.m[0][2], yaw.m[2][2]);
            double azimuthError = fabs(remainder(azimuth - yawAzimuth, 2 * kPi));
            double levelError = fabs(yaw.m[1][2]) + fabs(yaw.m[0][1]) + fabs(yaw.m[2][1]) + fabs(yaw.m[1][0]) + fabs(yaw.m[1][1] - 1); // forward, up and right stay level
            Matrix4 again = YawOnlyDelta(yaw);
            double repeatError = 0;
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    repeatError = fmax(repeatError, fabs(again.m[i][j] - yaw.m[i][j]));
            double orthoError = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    double dot = yaw.m[i][0] * yaw.m[j][0] + yaw.m[i][1] * yaw.m[j][1] + yaw.m[i][2] * yaw.m[j][2];
                    orthoError = fmax(orthoError, fabs(dot - (i == j ? 1.0 : 0.0)));
                }
            if (originError < 1e-9 && azimuthError < 1e-9 && levelError < 1e-12 && repeatError < 1e-9 && orthoError < 1e-9)
                good++;
            else
                CHECK(false, "the head without pitch and roll is wrong (n %d: origin %.2g, azimuth %.2g, level %.2g, repeat %.2g, orthonormal %.2g)", n, originError, azimuthError, levelError,
                      repeatError, orthoError);
        }
        printf("yaw only: %u of 2000 keep the eye's place and direction and lose pitch and roll\n", good);
        const double zero[3] = {0, 0, 0};
        Matrix4 straightUp = RigidDelta(0.3, 1.5707963, 0.0, zero);
        Matrix4 yawUp = YawOnlyDelta(straightUp);
        CHECK(std::isfinite(yawUp.m[0][0]) && std::isfinite(yawUp.m[2][2]), "looking straight up gave no finite result");
    }
    // ------------------------------------------------------------------------------------------------ recognising the reflection view among the cameras
    g_vr.frameCount = 1000;
    {
        double axes[3][3], pos[3] = {100, 3.5, -40};
        MakeCamera(0.7, 0.07, axes);
        MainCameraPose pose;
        for (int i = 0; i < 3; i++) {
            pose.pos[i] = pos[i];
            pose.right[i] = axes[0][i];
            pose.up[i] = axes[1][i];
            pose.fwd[i] = axes[2][i];
        }
        pose.projX = 0.96;
        pose.projY = 1.73;
        pose.frame = 1000;
        PublishMainCameraPose(pose);
        double mirroredPos[3], mirrored[3][3];
        Mirror(pos, axes, 0.4, 1, mirroredPos, mirrored);
        CameraSnapshot reflection = Snapshot(mirroredPos, mirrored, 1.18, 1.8);
        CHECK(ReflectionAxisOf(reflection) == 1, "the reflection camera was not recognised");
        CameraSnapshot same = Snapshot(pos, axes, 1.05, 1.8);
        CHECK(ReflectionAxisOf(same) == -1, "a camera at the main camera's own place was taken for the reflection");
        double rear[3][3];
        for (int i = 0; i < 3; i++) {
            rear[0][i] = -axes[0][i];
            rear[1][i] = axes[1][i];
            rear[2][i] = -axes[2][i];
        }
        CameraSnapshot rearView = Snapshot(pos, rear, 0.9, 5);
        CHECK(ReflectionAxisOf(rearView) == -1, "the rear-view camera was taken for the reflection");
        double farPos[3] = {mirroredPos[0] + 80, mirroredPos[1], mirroredPos[2]};
        CameraSnapshot farAway = Snapshot(farPos, mirrored, 1.18, 1.8);
        CHECK(ReflectionAxisOf(farAway) == -1, "a mirrored camera 80 m away was taken for the reflection");
        g_vr.frameCount = 1010; // the main camera has not been seen for ten frames
        CHECK(ReflectionAxisOf(reflection) == -1, "an old pose was used");
    }
    // ------------------------------------------------------------------------------------------------ the camera of a description, and the whole chain from the game's frustum
    {
        // (the reflection view is given its frustum before its camera data, so the camera has to come back out of the frustum)
        unsigned rebuilt = 0, close = 0;
        for (int n = 0; n < 500; n++) {
            double axes[3][3], pos[3] = {Rand(-900, 900), Rand(-30, 60), Rand(-900, 900)};
            MakeCamera(Rand(-3.1, 3.1), Rand(-0.4, 0.4), axes);
            CameraSnapshot cam = Snapshot(pos, axes, Rand(0.8, 1.4), Rand(1.3, 2.0));
            cam.nearDist = Rand(0.05, 1.0);
            cam.farDist = Rand(200, 3000);
            double tanV = tan(cam.fov / 2), tanH = tanV * cam.aspect;
            EyeBox box = {-tanH, tanH, tanV, tanV}, one[2] = {box, box};
            Matrix4 identity = Identity(), eyes[2] = {identity, identity};
            CullSettings none = {0, 0};
            CullPlanes planes;
            CHECK(ComputeCullPlanes(cam, identity, eyes, one, none, planes), "no planes for a camera");
            float description[0x2B0 / 4];
            memset(description, 0, sizeof(description));
            int six = 6;
            memcpy(description, &six, 4);
            WriteCullPlanes(description, planes);
            CameraSnapshot back;
            if (!SnapshotFromDescription(description, back)) {
                CHECK(false, "the camera did not come back out of its frustum (n %d)", n);
                continue;
            }
            rebuilt++;
            double axisError = 0;
            for (int i = 0; i < 3; i++)
                axisError = fmax(axisError, fmax(fabs(back.right[i] - cam.right[i]), fmax(fabs(back.up[i] - cam.up[i]), fabs(back.fwd[i] - cam.fwd[i]))));
            double posError = fabs(back.pos[0] - cam.pos[0]) + fabs(back.pos[1] - cam.pos[1]) + fabs(back.pos[2] - cam.pos[2]);
            if (axisError < 2e-3 && posError < 0.05 && fabs(back.fov - cam.fov) < 2e-3 && fabs(back.aspect - cam.aspect) < 2e-3 && fabs(back.nearDist - cam.nearDist) < 0.05 &&
                fabs(back.farDist - cam.farDist) < 0.5)
                close++;
            else
                CHECK(false, "the camera that came back differs: axes %.2g, position %.2g m, fov %.4f/%.4f, aspect %.4f/%.4f, near %.3f/%.3f, far %.1f/%.1f", axisError, posError, back.fov,
                      cam.fov, back.aspect, cam.aspect, back.nearDist, cam.nearDist, back.farDist, cam.farDist);
        }
        printf("camera of a description: %u of 500 came back, %u of them equal to the original\n", rebuilt, close);
        float junk[0x2B0 / 4];
        memset(junk, 0, sizeof(junk));
        CameraSnapshot refused;
        CHECK(!SnapshotFromDescription(junk, refused), "an empty description was taken for a camera");
        for (int i = 0; i < 0x2B0 / 4; i++)
            junk[i] = (float)Rand(-5, 5);
        int sixJunk = 6;
        memcpy(junk, &sixJunk, 4);
        CHECK(!SnapshotFromDescription(junk, refused), "random numbers were taken for a camera");

        // the whole chain as the hooks run it: the main camera's pose is published, the reflection view's description arrives, the camera is found in it, recognised as
        // the mirror, and the description handed on is the frustum around the mirrored head
        g_vr.active = true;
        g_vr.poseValid = true;
        g_modeOverride = 2;
        g_vr.frameCount = 5000;
        g_cfg.cullMarginDeg = 6.0;
        g_cfg.cullPullBack = 0.3;
        g_vr.eyeBox[0] = boxes[0];
        g_vr.eyeBox[1] = boxes[1];
        unsigned chains = 0, contained = 0;
        for (int n = 0; n < 200; n++) {
            double axes[3][3], pos[3] = {Rand(-800, 800), Rand(2, 6), Rand(-800, 800)};
            MakeCamera(Rand(-3.1, 3.1), Rand(-0.15, 0.15), axes);
            MainCameraPose pose;
            for (int i = 0; i < 3; i++) {
                pose.pos[i] = pos[i];
                pose.right[i] = axes[0][i];
                pose.up[i] = axes[1][i];
                pose.fwd[i] = axes[2][i];
            }
            pose.projX = 0.96;
            pose.projY = 1.73;
            pose.frame = 5000;
            PublishMainCameraPose(pose);
            double h = pos[1] - Rand(0.8, 2.5), mirroredPos[3], mirrored[3][3];
            Mirror(pos, axes, h, 1, mirroredPos, mirrored);
            CameraSnapshot game = Snapshot(mirroredPos, mirrored, 1.18, 1.8); // (the game's reflection camera: a wider field of view)
            double tanV = tan(game.fov / 2), tanH = tanV * game.aspect;
            EyeBox box = {-tanH, tanH, tanV, tanV}, one[2] = {box, box};
            Matrix4 identity = Identity(), eyesIdentity[2] = {identity, identity};
            CullSettings none = {0, 0};
            CullPlanes gamePlanes;
            ComputeCullPlanes(game, identity, eyesIdentity, one, none, gamePlanes);
            float description[0x2B0 / 4];
            memset(description, 0, sizeof(description));
            int six = 6;
            memcpy(description, &six, 4);
            WriteCullPlanes(description, gamePlanes);
            // a head turned and moved, eyes beside it
            double headT[3] = {Rand(-0.3, 0.3), Rand(-0.2, 0.2), Rand(-0.3, 0.3)};
            Matrix4 head = RigidDelta(Rand(-0.9, 0.9), Rand(-0.5, 0.5), Rand(-0.2, 0.2), headT);
            g_vr.eyeCameraDelta[2] = head;
            for (int e = 0; e < 2; e++) {
                g_vr.eyeCameraDelta[e] = head;
                g_vr.eyeCameraDelta[e].m[3][0] = headT[0] + (e ? -0.035 : 0.035);
            }
            CameraSnapshot found;
            if (!SnapshotFromDescription(description, found)) {
                CHECK(false, "the reflection camera was not found in its frustum (n %d)", n);
                continue;
            }
            found.mirrorAxis = ReflectionAxisOf(found);
            CHECK(found.mirrorAxis == 1, "the reflection camera was not recognised as the mirror (n %d, axis %d)", n, found.mirrorAxis);
            alignas(16) BYTE scratch[kScratchSize];
            const void* handed = ReflectionCameraForCulling(found, description, scratch);
            CHECK(handed == scratch, "the frustum was not rewritten (n %d)", n);
            if (handed != scratch)
                continue;
            chains++;
            const float* out = (const float*)handed;
            bool allInside = true;
            for (int e = 0; e < 2 && allInside; e++) {
                double origin[3], eyeAxes[3][3];
                EyeWorld(mirroredPos, mirrored, FlipConjugate(YawOnlyDelta(g_vr.eyeCameraDelta[e]), 1), origin, eyeAxes);
                for (int s = 0; s < 100 && allInside; s++) {
                    double tx = Rand(boxes[e].left, boxes[e].right), ty = Rand(-boxes[e].down, boxes[e].up), z = Rand(0.2, 300), p[3];
                    for (int i = 0; i < 3; i++)
                        p[i] = origin[i] + z * (tx * eyeAxes[0][i] + ty * eyeAxes[1][i] + eyeAxes[2][i]);
                    for (int plane = 0; plane < 6; plane++) {
                        const float* q = out + (0x30 + 0x10 * plane) / 4;
                        if (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] < -0.05)
                            allInside = false;
                    }
                }
            }
            if (allInside)
                contained++;
            else
                CHECK(false, "the frustum handed to the game misses part of the mirrored eyes' view (n %d)", n);
        }
        printf("chain: %u of 200 frustums rewritten, %u of them contain the mirrored eyes' view\n", chains, contained);
    }
    if (g_failures) {
        printf("reflection_cull_check: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("reflection_cull_check: all checks passed\n");
    return 0;
}
