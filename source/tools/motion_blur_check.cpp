// Host check of the motion blur (NOTES.md section 5). First the per-eye matrices (BlurMatrixForEyeRawDepth and BlurMatrixForEye, one for each kind of blur shader the game has) against a reprojection worked out from first principles, for random game cameras, head poses and
// points, and the point straight ahead of the car at different head turns. Then the switch ([VR] MotionBlur): the mod is linked as it is (every src\*.cpp except dllmain.cpp) and MotionBlurPerEye is run on a real
// Direct3D 9 device with a stand-in for the game's blur shader (a shader of either model whose constant table names g_mMatrix and g_vStepScale and that paints everything red).
// With MotionBlur=0 the pass has to write what the blur writes for a picture that does not move (the colour times c4.w with an alpha of 0, or the blur's own shader with a matrix that moves
// nothing), with the blur's own shader and matrix back afterwards and the tracking untouched;
// with MotionBlur=1, or when the bound shader is not known, the stand-in must be drawn as before, once per eye.  Built and run by tools/motion_blur_check.py.
#include <stdio.h>
#include <algorithm>
#include "camera.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "render_state.h"
#include "screen_space_passes.h"
#include "shader_tracking.h"
#include "vr_runtime.h"

static int g_failures = 0;
#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL line %d: %s -- ", __LINE__, #cond);       \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static const int kW = 64, kH = 32;
static const float kStillScale = 2.0f; // what c4.w is in the check (above 1, so that the saturation counts)
static const unsigned kRed = 0xFFFF0000;

// ---- a stand-in for the game's blur shader: the real constant table (so the mod's parser finds the blur), and a body that paints red -------------------------------------
static std::vector<DWORD> StandInBlur(DWORD version, bool convertsDepth = false) // (convertsDepth: an add with c4 in the body, as the game's blur shaders that turn the depth into a distance have)
{
    const char* names[] = {"g_mMatrix", "g_vStepScale"};
    const WORD regs[] = {0, 4}, counts[] = {4, 1};
    std::vector<BYTE> blob(28 + 2 * 20, 0);
    DWORD offsets[2];
    for (int i = 0; i < 2; i++) {
        offsets[i] = (DWORD)blob.size();
        for (const char* p = names[i]; *p; p++)
            blob.push_back((BYTE)*p);
        blob.push_back(0);
    }
    while (blob.size() % 4)
        blob.push_back(0);
    *(DWORD*)&blob[0] = 28;
    *(DWORD*)&blob[12] = 2;
    *(DWORD*)&blob[16] = 28;
    for (int i = 0; i < 2; i++) {
        BYTE* entry = &blob[28 + i * 20];
        *(DWORD*)(entry + 0) = offsets[i];
        *(WORD*)(entry + 4) = 2; // D3DXRS_FLOAT4
        *(WORD*)(entry + 6) = regs[i];
        *(WORD*)(entry + 8) = counts[i];
    }
    std::vector<DWORD> tokens;
    tokens.push_back(version);
    tokens.push_back(((DWORD)(1 + blob.size() / 4) << 16) | 0xFFFE);
    tokens.push_back(0x42415443); // 'CTAB'
    for (size_t i = 0; i < blob.size(); i += 4)
        tokens.push_back(*(DWORD*)&blob[i]);
    const DWORD define[] = {0x05000051, 0xA00F0014, 0x3F800000, 0x00000000, 0x00000000, 0x3F800000}; // def c20, 1, 0, 0, 1
    const DWORD declare[] = {0x0200001F, 0x80000005, 0x90030000};                                      // dcl_texcoord v0.xy (ps_3_0 wants its inputs declared)
    const DWORD body[] = {0x02000001, 0x800F0800, 0xA0E40014,                                          // mov oC0, c20
                          0x0000FFFF};
    tokens.insert(tokens.end(), define, define + 6);
    if ((version & 0xFF00) == 0x0300)
        tokens.insert(tokens.end(), declare, declare + 3);
    if (convertsDepth) {
        const DWORD add[] = {0x03000002, 0x800F0000, 0xA0E40004, 0xA0E40004}; // add r0, c4, c4
        tokens.insert(tokens.end(), add, add + 4);
    }
    tokens.insert(tokens.end(), body, body + 4);
    return tokens;
}
// the vertex shaders that go with the two pixel shader models (Direct3D 9 pairs a 3.0 pixel shader with a 3.0 vertex shader and a 2.0 one with a 2.0 one): position and texture coordinate passed on
static const DWORD kPassVertexShader30[] = {0xFFFE0300,
                                            0x0200001F, 0x80000000, 0x900F0000, // dcl_position v0
                                            0x0200001F, 0x80000005, 0x900F0001, // dcl_texcoord v1
                                            0x0200001F, 0x80000000, 0xE00F0000, // dcl_position o0
                                            0x0200001F, 0x80000005, 0xE0030001, // dcl_texcoord o1.xy
                                            0x02000001, 0xE00F0000, 0x90E40000, // mov o0, v0
                                            0x02000001, 0xE0030001, 0x90E40001, // mov o1.xy, v1
                                            0x0000FFFF};
static const DWORD kPassVertexShader20[] = {0xFFFE0200,
                                            0x0200001F, 0x80000000, 0x900F0000, // dcl_position v0
                                            0x0200001F, 0x80000005, 0x900F0001, // dcl_texcoord v1
                                            0x02000001, 0xC00F0000, 0x90E40000, // mov oPos, v0
                                            0x02000001, 0xE0030000, 0x90E40001, // mov oT0.xy, v1
                                            0x0000FFFF};

struct Vertex3 {
    float x, y, z, w, u, v;
};

// what the mod makes on the device: the version token of every pixel shader it creates (the check's own stand-ins go straight to the device)
static void* g_realCreatePixelShader = NULL;
static std::vector<DWORD> g_createdShaders;
static HRESULT STDMETHODCALLTYPE RecordCreatePixelShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DPixelShader9** shader)
{
    g_createdShaders.push_back(function[0]);
    return ((PFN_CreatePixelShader)g_realCreatePixelShader)(device, function, shader);
}

// ---- the per-eye matrix against a reprojection worked out from first principles ------------------------------------------------------------------------------------
struct Rng {
    unsigned long long s = 88172645463325252ull;
    unsigned long long Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    double Uniform(double lo, double hi) { return lo + (hi - lo) * ((Next() >> 11) * (1.0 / 9007199254740992.0)); }
};
static Matrix4 RigidMatrix(Rng& rng, double angle, double travel) // (row vectors: a camera pose, world -> camera)
{
    const double yaw = rng.Uniform(-angle, angle), pitch = rng.Uniform(-angle, angle), roll = rng.Uniform(-angle, angle);
    const double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    Matrix4 m = Identity();
    const double r[3][3] = {{cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr},
                            {-cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr},
                            {sy * cp, -sp, cy * cp}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m.m[i][j] = r[i][j];
    for (int j = 0; j < 3; j++)
        m.m[3][j] = rng.Uniform(-travel, travel);
    return m;
}
static Matrix4 YawMatrix(double degrees)
{
    Matrix4 m = Identity();
    const double a = degrees * 3.14159265358979 / 180, c = cos(a), s = sin(a);
    m.m[0][0] = c;
    m.m[0][2] = -s;
    m.m[2][0] = s;
    m.m[2][2] = c;
    return m;
}
static void Times(const double p[4], const Matrix4& m, double out[4])
{
    for (int j = 0; j < 4; j++)
        out[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + p[3] * m.m[3][j];
}

static const double kProjX = 0.8179, kProjY = 1.6496, kDepthC = -0.00003, kDepthD = 0.11667;
static const EyeBox kEyeBoxes[2] = {{-1.544, 1.047, 1.405, 1.411}, {-1.052, 1.538, 1.404, 1.415}};

// The game's own blur matrix for a camera that moved from `previous` to `current` (world -> camera, row vectors), as the game builds it: the pixel's (z*u, z*v, z, 1) to clip space of the
// current camera, back to the world, into the previous camera's clip space, and out as (x', y', unused, w') with w' the depth in the previous camera.
static Matrix4 GameBlurMatrix(const Matrix4& current, const Matrix4& previous)
{
    Matrix4 projection = Identity();
    projection.m[0][0] = kProjX;
    projection.m[1][1] = kProjY;
    projection.m[2][2] = kDepthC;
    projection.m[2][3] = 1;
    projection.m[3][2] = kDepthD;
    projection.m[3][3] = 0;
    const double toClipRows[16] = {2, 0, 0, 0, 0, -2, 0, 0, -1, 1, kDepthC, 1, 0, 0, kDepthD, 0};
    const double fromClipRows[16] = {0.5, 0, 0, 0, 0, -0.5, 0, 0, 0, 0, 0, 0, 0.5, 0.5, 0, 1};
    Matrix4 toClip, fromClip, currentInverse;
    memcpy(toClip.m, toClipRows, sizeof(toClipRows));
    memcpy(fromClip.m, fromClipRows, sizeof(fromClipRows));
    Invert(Multiply(current, projection), currentInverse);
    return Multiply(Multiply(Multiply(toClip, currentInverse), Multiply(previous, projection)), fromClip);
}
// The same for the blur shaders that take (u, v, depth, 1): the pixel's texture coordinates and the depth as the buffer holds it (c + d/Z) go to the clip space of the current camera
// ((2u - 1, 1 - 2v, depth, 1), which is the clip position divided by Z), back to the world, into the previous camera's clip space, and out as (x', y', z', w') with w' = Z'/Z.
static Matrix4 GameBlurMatrixRaw(const Matrix4& current, const Matrix4& previous)
{
    Matrix4 projection = Identity();
    projection.m[0][0] = kProjX;
    projection.m[1][1] = kProjY;
    projection.m[2][2] = kDepthC;
    projection.m[2][3] = 1;
    projection.m[3][2] = kDepthD;
    projection.m[3][3] = 0;
    const double toClipRows[16] = {2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    const double fromClipRows[16] = {0.5, 0, 0, 0, 0, -0.5, 0, 0, 0, 0, 1, 0, 0.5, 0.5, 0, 1};
    Matrix4 toClip, fromClip, currentInverse;
    memcpy(toClip.m, toClipRows, sizeof(toClipRows));
    memcpy(fromClip.m, fromClipRows, sizeof(fromClipRows));
    Invert(Multiply(current, projection), currentInverse);
    return Multiply(Multiply(Multiply(toClip, currentInverse), Multiply(previous, projection)), fromClip);
}
static void BlurConstants(const Matrix4& game, float* constants) // (c0..c3 as the shader reads them: columns x, y, w)
{
    for (int i = 0; i < 4; i++) {
        constants[i * 4] = (float)game.m[i][0];
        constants[i * 4 + 1] = (float)game.m[i][1];
        constants[i * 4 + 2] = 0;
        constants[i * 4 + 3] = (float)game.m[i][3];
    }
}
// What the blur's shader makes of a pixel: its previous texture coordinate, from the pixel's texture coordinate and depth
static void ShaderPrevious(const float* constants, double u, double v, double z, double* prevU, double* prevV)
{
    const double q[4] = {z * u, z * v, z, 1.0};
    double out[4] = {0, 0, 0, 0};
    for (int j : {0, 1, 3})
        for (int i = 0; i < 4; i++)
            out[j] += q[i] * constants[i * 4 + j];
    *prevU = out[0] / out[3];
    *prevV = out[1] / out[3];
}
static void ShaderPreviousRaw(const float* constants, double u, double v, double depth, double* prevU, double* prevV, double* w = NULL)
{
    const double q[4] = {u, v, depth, 1.0};
    double out[4] = {0, 0, 0, 0};
    for (int j : {0, 1, 3})
        for (int i = 0; i < 4; i++)
            out[j] += q[i] * constants[i * 4 + j];
    *prevU = out[0] / out[3];
    *prevV = out[1] / out[3];
    if (w)
        *w = out[3];
}
static void EyeTexcoord(int eye, const double p[4], double* u, double* v) // a point in the eye's camera -> its texture coordinate in the side-by-side picture
{
    const EyeBox& box = kEyeBoxes[eye];
    *u = 0.5 * eye + 0.5 * (p[0] / p[2] - box.left) / (box.right - box.left);
    *v = (box.up - p[1] / p[2]) / (box.up + box.down);
}

static void CheckBlurMatrix()
{
    printf("per-eye blur matrix against a reprojection from first principles:\n");
    Rng rng;
    int used = 0, wrong = 0;
    double worst = 0;
    for (int trial = 0; trial < 20000; trial++) {
        const Matrix4 previous = RigidMatrix(rng, 0.2, 2.0), current = RigidMatrix(rng, 0.2, 2.0);
        Matrix4 head = RigidMatrix(rng, 1.0, 0.3); // up to 57 degrees about each axis
        head.m[3][0] += 0.03;
        const int eye = trial & 1;
        double point[4] = {rng.Uniform(-30, 30), rng.Uniform(-10, 10), rng.Uniform(1, 120), 1.0}, world[4], inGame[4], inEye[4], previousGame[4], previousEye[4];
        Matrix4 currentInverse;
        Invert(current, currentInverse);
        Times(point, currentInverse, world);
        Times(world, current, inGame);
        Times(inGame, head, inEye);
        Times(world, previous, previousGame);
        Times(previousGame, head, previousEye);
        if (inEye[2] < 0.5 || previousEye[2] < 0.5 || inGame[2] < 0.5 || previousGame[2] < 0.5)
            continue;
        double u, v, wantU, wantV, previousU, previousV;
        EyeTexcoord(eye, inEye, &u, &v);
        const double localU = (u - 0.5 * eye) * 2;
        if (v < 0 || v > 1 || localU < 0 || localU > 1)
            continue;
        EyeTexcoord(eye, previousEye, &wantU, &wantV);
        float gameConstants[16], eyeConstants[16];
        const Matrix4 game = GameBlurMatrix(current, previous);
        BlurConstants(game, gameConstants);
        // the game's own matrix finds the game's own previous place (checks the construction above)
        double gameU = 0.5 + 0.5 * kProjX * inGame[0] / inGame[2], gameV = 0.5 - 0.5 * kProjY * inGame[1] / inGame[2], gameWantU, gameWantV;
        ShaderPrevious(gameConstants, gameU, gameV, inGame[2], &previousU, &previousV);
        gameWantU = 0.5 + 0.5 * kProjX * previousGame[0] / previousGame[2];
        gameWantV = 0.5 - 0.5 * kProjY * previousGame[1] / previousGame[2];
        CHECK(fabs(previousU - gameWantU) < 1e-3 && fabs(previousV - gameWantV) < 1e-3, "the game's own matrix reprojects the game's own point (%.5f, %.5f against %.5f, %.5f)", previousU, previousV, gameWantU, gameWantV);
        CHECK(BlurMatrixForEye(gameConstants, kEyeBoxes[eye], eye, head, kProjX, kProjY, eyeConstants), "the eye matrix is made");
        ShaderPrevious(eyeConstants, u, v, inEye[2], &previousU, &previousV);
        const double error = fmax(fabs(previousU - wantU), fabs(previousV - wantV));
        worst = fmax(worst, error);
        used++;
        if (error > 1e-3) {
            if (wrong++ < 5)
                printf("  trial %d eye %d: the shader finds (%.5f, %.5f), the point was at (%.5f, %.5f)\n", trial, eye, previousU, previousV, wantU, wantV);
        }
    }
    CHECK(used > 3000, "enough random points were used (%d)", used);
    CHECK(wrong == 0, "%d of %d points reproject wrongly through the eye matrix", wrong, used);
    printf("  %d points, worst error %.2e of the picture's width\n", used, worst);

    // driving straight ahead: the blur has no length at the point straight ahead of the car, whichever way the head is turned (the eye sees that point elsewhere in its picture)
    for (double yaw : {0.0, 25.0, -25.0, 40.0, -40.0}) {
        const Matrix4 current = Identity();
        Matrix4 previous = Identity();
        previous.m[3][2] = 1.0; // the camera was a unit further back: world -> camera is moved forward by one
        Matrix4 head = YawMatrix(yaw);
        head.m[3][0] = 0.03;
        for (int eye = 0; eye < 2; eye++) {
            const double ahead[4] = {0, 0, 50, 1};
            double inEye[4];
            Times(ahead, head, inEye);
            double u, v;
            EyeTexcoord(eye, inEye, &u, &v);
            if (u < 0.5 * eye || u > 0.5 * eye + 0.5 || v < 0 || v > 1)
                continue; // (the point is outside this eye's picture)
            float gameConstants[16], eyeConstants[16];
            BlurConstants(GameBlurMatrix(current, previous), gameConstants);
            CHECK(BlurMatrixForEye(gameConstants, kEyeBoxes[eye], eye, head, kProjX, kProjY, eyeConstants), "the eye matrix is made");
            double previousU, previousV;
            ShaderPrevious(eyeConstants, u, v, inEye[2], &previousU, &previousV);
            const double length = hypot(previousU - u, previousV - v);
            // (it is not exactly zero: the eye is 3 hundredths of a unit away from the camera, so the point is not quite at the centre of the expansion)
            CHECK(length < 3e-3, "head turned %.0f degrees, eye %d: the blur at the point straight ahead is %.5f long", yaw, eye, length);
            printf("  head turned %+3.0f degrees, eye %d: the point straight ahead is at (%.3f, %.3f) in the picture and its blur is %.5f long\n", yaw, eye, u, v, length);
        }
    }
}

// the shaders that take (u, v, depth, 1): BlurMatrixForEyeRawDepth against the same reprojection
static void CheckBlurMatrixRaw()
{
    printf("per-eye blur matrix for the shaders that take (u, v, depth, 1), against a reprojection from first principles:\n");
    Rng rng;
    int used = 0, wrong = 0;
    double worst = 0;
    for (int mode = 1; mode <= 2; mode++) // 1: the head's pose, 2: only its turn (the offset taken out of the delta)
        for (int trial = 0; trial < 20000; trial++) {
            const Matrix4 previous = RigidMatrix(rng, 0.2, 2.0), current = RigidMatrix(rng, 0.2, 2.0);
            Matrix4 head = RigidMatrix(rng, 1.0, 0.3);
            head.m[3][0] += 0.03;
            if (mode == 2)
                head.m[3][0] = head.m[3][1] = head.m[3][2] = 0;
            const int eye = trial & 1;
            double point[4] = {rng.Uniform(-30, 30), rng.Uniform(-10, 10), rng.Uniform(1, 120), 1.0}, world[4], inGame[4], inEye[4], previousGame[4], previousEye[4];
            Matrix4 currentInverse;
            Invert(current, currentInverse);
            Times(point, currentInverse, world);
            Times(world, current, inGame);
            Times(inGame, head, inEye);
            Times(world, previous, previousGame);
            Times(previousGame, head, previousEye);
            if (inEye[2] < 0.5 || previousEye[2] < 0.5 || inGame[2] < 0.5 || previousGame[2] < 0.5)
                continue;
            double u, v, wantU, wantV, previousU, previousV;
            EyeTexcoord(eye, inEye, &u, &v);
            const double localU = (u - 0.5 * eye) * 2;
            if (v < 0 || v > 1 || localU < 0 || localU > 1)
                continue;
            EyeTexcoord(eye, previousEye, &wantU, &wantV);
            float gameConstants[16], eyeConstants[16];
            const Matrix4 game = GameBlurMatrixRaw(current, previous);
            BlurConstants(game, gameConstants);
            // the game's own matrix finds the game's own previous place (checks the construction above), and w' is the previous distance over this one
            double gameU = 0.5 + 0.5 * kProjX * inGame[0] / inGame[2], gameV = 0.5 - 0.5 * kProjY * inGame[1] / inGame[2], gameWantU, gameWantV, w;
            ShaderPreviousRaw(gameConstants, gameU, gameV, kDepthC + kDepthD / inGame[2], &previousU, &previousV, &w);
            gameWantU = 0.5 + 0.5 * kProjX * previousGame[0] / previousGame[2];
            gameWantV = 0.5 - 0.5 * kProjY * previousGame[1] / previousGame[2];
            CHECK(fabs(previousU - gameWantU) < 1e-3 && fabs(previousV - gameWantV) < 1e-3, "the game's own matrix reprojects the game's own point (%.5f, %.5f against %.5f, %.5f)", previousU, previousV,
                  gameWantU, gameWantV);
            CHECK(fabs(w - previousGame[2] / inGame[2]) < 1e-3 * fmax(1.0, fabs(w)), "w' is the previous distance over this one (%.5f against %.5f)", w, previousGame[2] / inGame[2]);
            CHECK(BlurMatrixForEyeRawDepth(gameConstants, kEyeBoxes[eye], eye, head, kProjX, kProjY, kDepthC, kDepthD, eyeConstants), "the eye matrix is made");
            ShaderPreviousRaw(eyeConstants, u, v, kDepthC + kDepthD / inEye[2], &previousU, &previousV);
            const double error = fmax(fabs(previousU - wantU), fabs(previousV - wantV));
            worst = fmax(worst, error);
            used++;
            if (error > 1e-3) {
                if (wrong++ < 5)
                    printf("  mode %d trial %d eye %d: the shader finds (%.5f, %.5f), the point was at (%.5f, %.5f)\n", mode, trial, eye, previousU, previousV, wantU, wantV);
            }
        }
    CHECK(used > 6000, "enough random points were used (%d)", used);
    CHECK(wrong == 0, "%d of %d points reproject wrongly through the eye matrix", wrong, used);
    printf("  %d points, worst error %.2e of the picture's width\n", used, worst);

    // a car at rest: the game's matrix is the identity, and no pixel finds a vector, whichever way the head looks
    float rest[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, eyeConstants[16];
    double longest = 0;
    for (double yaw : {0.0, 30.0, -50.0, 80.0}) {
        Matrix4 head = YawMatrix(yaw);
        head.m[3][0] = 0.035;
        for (int eye = 0; eye < 2; eye++) {
            CHECK(BlurMatrixForEyeRawDepth(rest, kEyeBoxes[eye], eye, head, kProjX, kProjY, kDepthC, kDepthD, eyeConstants), "the eye matrix is made");
            for (double fu : {0.1, 0.5, 0.9})
                for (double fv : {0.1, 0.5, 0.9})
                    for (double z : {1.0, 10.0, 300.0}) {
                        double previousU, previousV;
                        const double u = 0.5 * eye + 0.5 * fu;
                        ShaderPreviousRaw(eyeConstants, u, fv, kDepthC + kDepthD / z, &previousU, &previousV);
                        longest = fmax(longest, hypot(previousU - u, previousV - fv));
                    }
        }
    }
    CHECK(longest < 1e-4, "a car at rest has blur vectors up to %.6f long", longest);
    printf("  at rest the longest vector is %.2e\n", longest);

    // driving straight ahead: no blur at the point straight ahead of the car, whichever way the head is turned
    for (double yaw : {0.0, 25.0, -25.0, 40.0, -40.0}) {
        const Matrix4 current = Identity();
        Matrix4 previous = Identity();
        previous.m[3][2] = 1.0;
        Matrix4 head = YawMatrix(yaw);
        head.m[3][0] = 0.03;
        for (int eye = 0; eye < 2; eye++) {
            const double ahead[4] = {0, 0, 50, 1};
            double inEye[4], u, v;
            Times(ahead, head, inEye);
            EyeTexcoord(eye, inEye, &u, &v);
            if (u < 0.5 * eye || u > 0.5 * eye + 0.5 || v < 0 || v > 1)
                continue;
            float gameConstants[16];
            BlurConstants(GameBlurMatrixRaw(current, previous), gameConstants);
            CHECK(BlurMatrixForEyeRawDepth(gameConstants, kEyeBoxes[eye], eye, head, kProjX, kProjY, kDepthC, kDepthD, eyeConstants), "the eye matrix is made");
            double previousU, previousV;
            ShaderPreviousRaw(eyeConstants, u, v, kDepthC + kDepthD / inEye[2], &previousU, &previousV);
            const double length = hypot(previousU - u, previousV - v);
            CHECK(length < 3e-3, "head turned %.0f degrees, eye %d: the blur at the point straight ahead is %.5f long", yaw, eye, length);
            printf("  head turned %+3.0f degrees, eye %d: the point straight ahead is at (%.3f, %.3f) in the picture and its blur is %.5f long\n", yaw, eye, u, v, length);
        }
    }
}

static LRESULT CALLBACK CheckProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return DefWindowProcA(window, message, w, l);
}

static bool ReadTarget(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* copy, std::vector<unsigned>& pixels)
{
    D3DLOCKED_RECT locked;
    if (FAILED(device->GetRenderTargetData(target, copy)) || FAILED(copy->LockRect(&locked, NULL, D3DLOCK_READONLY)))
        return false;
    pixels.resize((size_t)kW * kH);
    for (int y = 0; y < kH; y++)
        memcpy(&pixels[(size_t)y * kW], (const BYTE*)locked.pBits + (size_t)y * locked.Pitch, (size_t)kW * 4);
    copy->UnlockRect();
    return true;
}

static unsigned SourcePixel(int x, int y)
{
    return 0x40000000u | (unsigned)(x * 4) << 16 | (unsigned)(y * 8) << 8 | (unsigned)((x + y) * 3 & 0xFF);
}

int main()
{
    CheckBlurMatrix();
    CheckBlurMatrixRaw();
    WNDCLASSA wc = {};
    wc.lpfnWndProc = CheckProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "motion_blur_check";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "motion_blur_check", WS_OVERLAPPEDWINDOW, 100, 100, 200, 200, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = NULL;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 128;
    pp.BackBufferHeight = 128;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = window;
    HRESULT hr = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device) : E_FAIL;
    if (FAILED(hr) || !device) {
        printf("SKIP: no Direct3D 9 device here (0x%08x)\n", (unsigned)hr);
        return g_failures ? 1 : 0;
    }
    // the mod's hooks are not installed here: the "originals" are the device's own functions (which also checks the vtable numbers of d3d_hooks.h)
    void** vtable = *(void***)device;
    const int used[] = {VT_SetRenderState, VT_SetScissorRect, VT_SetPixelShaderConstantF, VT_CreatePixelShader, VT_SetPixelShader};
    for (int index : used)
        g_deviceOriginals[index] = vtable[index];
    g_realCreatePixelShader = vtable[VT_CreatePixelShader];
    g_deviceOriginals[VT_CreatePixelShader] = (void*)RecordCreatePixelShader;

    g_vr.eyeBox[0] = {-1.544, 1.047, 1.405, 1.411}; // the per-eye path reads the eyes' fields of view and the game's projection
    g_vr.eyeBox[1] = {-1.052, 1.538, 1.404, 1.415};
    g_projXScale = 0.8179;
    g_projYScale = 1.6496;
    for (int i = 0; i < 3; i++) { // the head a little turned and moved, the eyes apart
        g_vr.eyeCameraDelta[i] = YawMatrix(10.0);
        g_vr.eyeCameraDelta[i].m[3][0] = i == 0 ? 0.062 : i == 1 ? 0.0 : 0.031;
    }
    g_rtWidth = kW;
    g_rtHeight = kH;

    IDirect3DTexture9 *source = NULL, *targetTexture = NULL;
    IDirect3DSurface9 *target = NULL, *copy = NULL;
    CHECK(SUCCEEDED(device->CreateTexture(kW, kH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &source, NULL)), "source texture");
    CHECK(SUCCEEDED(device->CreateTexture(kW, kH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &targetTexture, NULL)), "target texture");
    CHECK(SUCCEEDED(device->CreateOffscreenPlainSurface(kW, kH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, NULL)), "readback surface");
    if (!source || !targetTexture || !copy) {
        printf("SKIP: no surfaces\n");
        return 1;
    }
    targetTexture->GetSurfaceLevel(0, &target);
    D3DLOCKED_RECT locked;
    CHECK(SUCCEEDED(source->LockRect(0, &locked, NULL, D3DLOCK_DISCARD)), "lock source");
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            ((unsigned*)((BYTE*)locked.pBits + (size_t)y * locked.Pitch))[x] = SourcePixel(x, y);
    source->UnlockRect(0);

    // the two shader models: the stand-in blur (made on the device, noted the way the mod's hook notes the game's), the geometry that goes with each
    IDirect3DVertexShader9* vertexShaders[4] = {}; // by shader model
    IDirect3DVertexDeclaration9* declaration = NULL;
    const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                          {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                          D3DDECL_END()};
    CHECK(SUCCEEDED(device->CreateVertexShader(kPassVertexShader30, &vertexShaders[3])), "vs_3_0 for the check");
    CHECK(SUCCEEDED(device->CreateVertexShader(kPassVertexShader20, &vertexShaders[2])), "vs_2_0 for the check");
    CHECK(SUCCEEDED(device->CreateVertexDeclaration(elements, &declaration)), "vertex declaration");
    const Vertex3 quad3[4] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};

    for (int model : {3, 2}) {
        printf("ps_%d_0:\n", model);
        const std::vector<DWORD> tokens = StandInBlur(model == 3 ? 0xFFFF0300 : 0xFFFF0200);
        IDirect3DPixelShader9* blurShader = NULL;
        hr = device->CreatePixelShader(tokens.data(), &blurShader);
        CHECK(SUCCEEDED(hr) && blurShader, "the stand-in blur shader is accepted by the runtime (0x%08x)", (unsigned)hr);
        if (!blurShader)
            continue;
        PixelShaderInfo info;
        info.id = 1000 + model;
        ParsePixelShaderConstants(tokens.data(), &info);
        CHECK(info.motionBlur && info.shaderMajor == model, "the mod's parser sees a blur shader of model %d (motionBlur %d, model %d)", model, (int)info.motionBlur, info.shaderMajor);
        CHECK(info.blurDepthRaw, "a blur shader that does not add g_vDepthScale takes the depth as it is");
        const std::vector<DWORD> convertingTokens = StandInBlur(model == 3 ? 0xFFFF0300 : 0xFFFF0200, true);
        PixelShaderInfo converting;
        ParsePixelShaderConstants(convertingTokens.data(), &converting);
        CHECK(converting.motionBlur && !converting.blurDepthRaw, "a blur shader that adds g_vDepthScale (c4) turns the depth into a distance first");
        IDirect3DPixelShader9* convertingShader = NULL;
        CHECK(SUCCEEDED(device->CreatePixelShader(convertingTokens.data(), &convertingShader)) && convertingShader, "the stand-in that adds g_vDepthScale is accepted by the runtime");

        auto setUp = [&]() {
            device->SetRenderTarget(0, target);
            device->SetTexture(0, source);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetPixelShader(blurShader);
            D3DVIEWPORT9 viewport = {0, 0, (DWORD)kW, (DWORD)kH, 0.0f, 1.0f};
            device->SetViewport(&viewport);
            RECT scissor = {3, 4, kW - 5, kH - 6};
            device->SetScissorRect(&scissor);
            g_gameScissorRect = scissor;
            g_gameScissorEnabled = FALSE;
            device->SetVertexShader(vertexShaders[model]);
            device->SetVertexDeclaration(declaration);
            const float marker[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}; // the game's g_mMatrix
            device->SetPixelShaderConstantF(0, marker, 4);
            memcpy(g_psConstants, marker, sizeof(marker));
            const float depthTerms[4] = {(float)kDepthC, (float)kDepthD, 0, kStillScale}; // g_vDepthScale (c4): the depth terms of the game's projection, and the scale the blur gives its colour
            memcpy(&g_psConstants[16], depthTerms, sizeof(depthTerms));
            device->SetPixelShaderConstantF(4, depthTerms, 1);
            g_currentPsInfo = &info;
            g_currentPsHandle = blurShader;
        };
        auto drawQuad = [&]() -> HRESULT {
            if (FAILED(device->BeginScene()))
                return E_FAIL;
            HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad3, sizeof(Vertex3));
            device->EndScene();
            return drawn;
        };
        DrawCall draw(drawQuad); // (a DrawCall only borrows what it is made from)
        int draws = 0;
        auto countQuad = [&]() -> HRESULT {
            draws++;
            return draw();
        };
        DrawCall countedDraw(countQuad);
        auto clear = [&]() { device->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0); }; // (setUp has the target set: setting it again would reset the scissor rectangle)
        auto settled = [&](const char* when) { // everything the pass touched is as the game left it
            IDirect3DPixelShader9* bound = NULL;
            device->GetPixelShader(&bound);
            CHECK(bound == blurShader, "%s: the game's shader is bound again", when);
            if (bound)
                bound->Release();
            CHECK(g_currentPsInfo == &info && g_currentPsHandle == (void*)blurShader, "%s: the mod's tracking of the bound shader is as it was", when);
            float constants[16];
            device->GetPixelShaderConstantF(0, constants, 4);
            bool same = true;
            for (int i = 0; i < 16; i++)
                same = same && constants[i] == (float)(i + 1);
            CHECK(same, "%s: g_mMatrix on the device is as the game set it", when);
            DWORD scissorTest = 7;
            device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
            RECT rect = {};
            device->GetScissorRect(&rect);
            CHECK(scissorTest == (DWORD)g_gameScissorEnabled && rect.left == 3 && rect.top == 4 && rect.right == kW - 5 && rect.bottom == kH - 6, "%s: the game's scissor state is back", when);
        };
        std::vector<unsigned> pixels;

        // MotionBlur=0, the kind of blur shader that takes (u, v, depth, 1) as ps_3_0 (the real ones are): one draw of a shader that writes what the blur writes for a picture that does not move,
        // the colour image times c4.w and an alpha of 0 (a copy with an alpha of 1 showed the half-size picture everywhere), the blur's own shader bound again afterwards
        g_cfg.motionBlur = 0;
        if (model == 3) {
            setUp();
            clear();
            draws = 0;
            hr = MotionBlurPerEye(device, countedDraw);
            CHECK(SUCCEEDED(hr) && draws == 1, "MotionBlur=0: one draw (%d), hr 0x%08x", draws, (unsigned)hr);
            CHECK(ReadTarget(device, target, copy, pixels), "read the target");
            int wrong = 0;
            for (int y = 0; y < kH && pixels.size() == (size_t)kW * kH; y++)
                for (int x = 0; x < kW; x++) {
                    const unsigned source = SourcePixel(x, y), got = pixels[(size_t)y * kW + x];
                    wrong += (got >> 24) != 0; // alpha 0
                    for (int shift : {16, 8, 0}) {
                        const int want = (int)fmin(255.0, floor(((source >> shift) & 0xFF) * kStillScale + 0.5));
                        wrong += abs((int)((got >> shift) & 0xFF) - want) > 1;
                    }
                }
            CHECK(wrong == 0, "MotionBlur=0: %d values of %d pixels are not the colour image times c4.w with an alpha of 0", wrong, kW * kH);
            settled("MotionBlur=0");
            const DWORD wanted = 0xFFFF0300u;
            CHECK(std::count(g_createdShaders.begin(), g_createdShaders.end(), wanted) == 1, "MotionBlur=0: one shader for it made (%d made in all)", (int)g_createdShaders.size());
            clear();
            CHECK(SUCCEEDED(draw()), "draw with the game's shader afterwards");
            CHECK(ReadTarget(device, target, copy, pixels) && pixels[0] == kRed && pixels[kW * kH - 1] == kRed, "MotionBlur=0: afterwards the game's own shader draws (red)");
            // the second frame uses the same shader (nothing is made again)
            const size_t madeBefore = g_createdShaders.size();
            setUp();
            clear();
            CHECK(SUCCEEDED(MotionBlurPerEye(device, draw)) && ReadTarget(device, target, copy, pixels) && (pixels[5 * kW + 7] >> 24) == 0, "MotionBlur=0: the second frame is drawn the same");
            CHECK(g_createdShaders.size() == madeBefore, "MotionBlur=0: the shader is made once, not every frame");
        }

        // MotionBlur=0 for every other blur shader: its own, once, with a matrix that moves nothing, and everything as the game left it afterwards
        const float stillRaw[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float stillDistance[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        auto drawnStill = [&](IDirect3DPixelShader9* shader, PixelShaderInfo* shaderInfo, const float* wantedMatrix, const char* what) {
            setUp();
            device->SetPixelShader(shader);
            g_currentPsInfo = shaderInfo;
            g_currentPsHandle = shader;
            clear();
            float during[16] = {};
            int duringDraws = 0;
            auto observeQuad = [&]() -> HRESULT {
                duringDraws++;
                device->GetPixelShaderConstantF(0, during, 4);
                return draw();
            };
            DrawCall observed(observeQuad);
            hr = MotionBlurPerEye(device, observed);
            CHECK(SUCCEEDED(hr) && duringDraws == 1, "%s, MotionBlur=0: one draw (%d)", what, duringDraws);
            CHECK(memcmp(during, wantedMatrix, sizeof(during)) == 0, "%s, MotionBlur=0: the matrix moves nothing", what);
            CHECK(ReadTarget(device, target, copy, pixels) && pixels[0] == kRed && pixels[kW * kH - 1] == kRed, "%s, MotionBlur=0: the game's own shader drew (red)", what);
            float after[16];
            device->GetPixelShaderConstantF(0, after, 4);
            bool same = true;
            for (int i = 0; i < 16; i++)
                same = same && after[i] == (float)(i + 1);
            CHECK(same, "%s, MotionBlur=0: g_mMatrix on the device is as the game set it", what);
            IDirect3DPixelShader9* bound = NULL;
            device->GetPixelShader(&bound);
            CHECK(bound == shader, "%s, MotionBlur=0: the game's shader is still bound", what);
            if (bound)
                bound->Release();
        };
        if (model != 3)
            drawnStill(blurShader, &info, stillRaw, "the kind that takes (u, v, depth, 1), as ps_2_0");
        if (convertingShader)
            drawnStill(convertingShader, &converting, stillDistance, "the kind that turns the depth into a distance");
        g_currentPsInfo = &info;
        g_currentPsHandle = blurShader;
        device->SetPixelShader(blurShader);

        // MotionBlur=0 but the bound shader is not known: the blur is drawn as the game does (once per eye)
        setUp();
        g_currentPsHandle = NULL;
        clear();
        draws = 0;
        hr = MotionBlurPerEye(device, countedDraw);
        CHECK(SUCCEEDED(hr) && draws == 2, "no shader handle: the blur is drawn once per eye (%d draws)", draws);
        CHECK(ReadTarget(device, target, copy, pixels) && pixels[0] == kRed && pixels[kW * kH - 1] == kRed, "no shader handle: the blur's shader drew");

        // MotionBlur=1: as before, once per eye, both halves painted, state back
        g_cfg.motionBlur = 1;
        setUp();
        clear();
        draws = 0;
        hr = MotionBlurPerEye(device, countedDraw);
        CHECK(SUCCEEDED(hr) && draws == 2, "MotionBlur=1: two draws, one per eye (%d)", draws);
        CHECK(ReadTarget(device, target, copy, pixels), "read the target");
        int notRed = 0;
        for (unsigned p : pixels)
            notRed += p != kRed;
        CHECK(notRed == 0, "MotionBlur=1: %d pixels not painted by the blur's shader", notRed);
        g_currentPsHandle = blurShader;
        settled("MotionBlur=1");

        // the kind of blur shader that turns the depth into a distance first: drawn once per eye as well (its matrix is checked against a calculation above)
        if (convertingShader) {
            setUp();
            device->SetPixelShader(convertingShader);
            g_currentPsInfo = &converting;
            g_currentPsHandle = convertingShader;
            clear();
            draws = 0;
            hr = MotionBlurPerEye(device, countedDraw);
            CHECK(SUCCEEDED(hr) && draws == 2, "a blur shader that turns the depth into a distance: two draws, one per eye (%d)", draws);
            CHECK(ReadTarget(device, target, copy, pixels), "read the target");
            notRed = 0;
            for (unsigned p : pixels)
                notRed += p != kRed;
            CHECK(notRed == 0, "a blur shader that turns the depth into a distance: %d pixels not painted by the blur's shader", notRed);
            device->SetPixelShader(blurShader);
            g_currentPsInfo = &info;
            g_currentPsHandle = blurShader;
        }
        if (convertingShader)
            convertingShader->Release();

        device->SetPixelShader(NULL);
        blurShader->Release();
    }

    if (declaration)
        declaration->Release();
    for (IDirect3DVertexShader9* vertexShader : vertexShaders)
        if (vertexShader)
            vertexShader->Release();
    copy->Release();
    target->Release();
    targetTexture->Release();
    source->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    if (g_failures)
        printf("motion_blur_check: %d FAILURE(S)\n", g_failures);
    else
        printf("motion_blur_check: all checks passed\n");
    return g_failures ? 1 : 0;
}
