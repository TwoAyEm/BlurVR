// Stereo duplication of main-pass draws: rewriting the tracked shader matrices for one eye.
#include "stereo_scene.h"
#include "camera.h"
#include "config.h"
#include "d3d_hooks.h"
#include "device_state.h"
#include "game_camera.h"
#include "render_state.h"
#include "shader_tracking.h"
#include "vr_runtime.h"

int g_decomposeFailLogs = 0;

// What a rewrite for one eye depends on besides the draw's own matrices: the game's projection, the eye's tangent box and the
// camera delta.  The products built from them are cached under this key, so a frame's draws share one computation.
struct RewriteKey {
    double projection[4]; // the game's projection: x scale, y scale, depth scale, depth bias
    EyeBox box;
    int stereo, padding;
    Matrix4 delta;
};
static void MakeKey(RewriteKey& key, const EyeBox& box, const Matrix4& delta)
{
    key.projection[0] = g_projXScale;
    key.projection[1] = g_projYScale;
    key.projection[2] = g_projDepthScale;
    key.projection[3] = g_projDepthBias;
    key.box = box;
    key.stereo = g_cfg.stereo;
    key.padding = 0;
    key.delta = delta;
}
// True when two blocks of doubles/ints (a multiple of 8 bytes) are bit for bit the same.  (memcmp on a 200-byte key was the
// slowest part of the cached lookup.)
static inline bool SameBits(const void* a, const void* b, size_t bytes)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    unsigned long long difference = 0;
    for (size_t i = 0; i < bytes; i += 8) {
        unsigned long long x, y;
        memcpy(&x, p + i, 8);
        memcpy(&y, q + i, 8);
        difference |= x ^ y;
    }
    return difference == 0;
}
// Does `key` describe a rewrite for this projection, box and delta?
static inline bool KeyMatches(const RewriteKey& key, const double projection[4], const EyeBox& box, int stereo,
                              const Matrix4& delta)
{
    return key.stereo == stereo && SameBits(key.projection, projection, sizeof(key.projection)) &&
           SameBits(&key.box, &box, sizeof(box)) && SameBits(&key.delta, &delta, sizeof(delta));
}
struct EyeRewrite {
    bool valid;
    RewriteKey key;
    Matrix4 newProj;   // the eye's projection
    Matrix4 clipRemap; // inverse(game's projection) * delta * newProj
};
static EyeRewrite g_rewrites[3][4]; // by eye (2 = centre) and reflection flip mode

// Per-pass record of the view-projection matrix (x, y, w columns) together with the projection and per-eye delta used
// to render it, so the main pass can later apply the identical transform to its reflection-lookup matrix
// (g_ReflectionViewMatrix), whatever size the reflection pass was.
struct ReflectionRecord {
    float col[12];
    double projXScale, projYScale, projDepthScale, projDepthBias;
    Matrix4 eyeDelta[2];
    bool valid[2];
    UINT width, height;
    struct {
        bool valid, ok;
        RewriteKey key;
        Matrix4 remap; // inverse(record's projection) * record's delta * the eye's projection
    } remap[2];
};
static ReflectionRecord g_reflectionRecords[24];
static int g_reflectionRecordCount = 0;
static int g_reflectionRecordNext = 0;
static int g_reflectionRecordLast = -1;
static bool ReflectionColumnsMatch(const float* a, const float* b)
{
    for (int i = 0; i < 12; i++) {
        float t = 1e-3f * fmaxf(1.0f, fabsf(a[i]));
        if (fabsf(a[i] - b[i]) > t)
            return false;
    }
    return true;
}

static Matrix4 ProjectionMatrix(double xScale, double yScale, double depthScale, double depthBias)
{
    Matrix4 projection = Identity();
    projection.m[0][0] = xScale;
    projection.m[1][1] = yScale;
    projection.m[2][2] = depthScale;
    projection.m[2][3] = 1;
    projection.m[3][2] = depthBias;
    projection.m[3][3] = 0;
    return projection;
}

// Inverse of the game's projection, rebuilt only when the projection changes.  NULL when it cannot be inverted.
static const Matrix4* GameProjectionInverse()
{
    static double key[4];
    static bool haveKey = false, invertible = false;
    static Matrix4 inverse;
    double now[4] = {g_projXScale, g_projYScale, g_projDepthScale, g_projDepthBias};
    if (!haveKey || memcmp(key, now, sizeof(now)) != 0) {
        memcpy(key, now, sizeof(now));
        haveKey = true;
        invertible = Invert(ProjectionMatrix(now[0], now[1], now[2], now[3]), inverse);
    }
    return invertible ? &inverse : NULL;
}

static const EyeRewrite& GetRewrite(int eye, int flipSlot, const EyeBox& box, const Matrix4& delta,
                                    const Matrix4& gameProjectionInverse)
{
    const double projection[4] = {g_projXScale, g_projYScale, g_projDepthScale, g_projDepthBias};
    EyeRewrite& rewrite = g_rewrites[eye][flipSlot];
    if (!rewrite.valid || !KeyMatches(rewrite.key, projection, box, g_cfg.stereo, delta)) {
        MakeKey(rewrite.key, box, delta);
        rewrite.newProj = BuildEyeProjection(box, eye);
        rewrite.clipRemap = Multiply(Multiply(gameProjectionInverse, delta), rewrite.newProj);
        rewrite.valid = true;
    }
    return rewrite;
}

// The transform that takes the reflection lookup matrix recorded for `record` to this eye; NULL when the record's projection
// cannot be inverted.
static const Matrix4* RecordRemap(ReflectionRecord& record, int eye)
{
    const double projection[4] = {record.projXScale, record.projYScale, record.projDepthScale, record.projDepthBias};
    auto& cache = record.remap[eye];
    if (!cache.valid || !KeyMatches(cache.key, projection, g_vr.eyeBox[eye], g_cfg.stereo, record.eyeDelta[eye])) {
        cache.key.projection[0] = projection[0];
        cache.key.projection[1] = projection[1];
        cache.key.projection[2] = projection[2];
        cache.key.projection[3] = projection[3];
        cache.key.box = g_vr.eyeBox[eye];
        cache.key.stereo = g_cfg.stereo;
        cache.key.padding = 0;
        cache.key.delta = record.eyeDelta[eye];
        cache.valid = true;
        Matrix4 recordProjInverse;
        cache.ok = Invert(ProjectionMatrix(record.projXScale, record.projYScale, record.projDepthScale, record.projDepthBias),
                          recordProjInverse);
        if (cache.ok)
            cache.remap = Multiply(Multiply(recordProjInverse, record.eyeDelta[eye]), BuildEyeProjection(g_vr.eyeBox[eye], eye));
    }
    return cache.ok ? &cache.remap : NULL;
}

static const int kMaxWrites = 16;

// Where the eye's camera sits in world space relative to the game's camera, for a draw seen through `view` (world to camera,
// row vectors) and moved to the eye by `delta` (camera space to eye camera space).  The eye's origin in camera space is the p
// with p * delta = 0, i.e. -t * transpose(R) for delta = [R 0; t 1]; it is turned into a world direction with the transpose of
// the view's rotation.  False when the view's rotation is not orthonormal (not a plain camera).
static bool EyeWorldOffset(const Matrix4& view, const Matrix4& delta, double offset[3])
{
    for (int i = 0; i < 3; i++) {
        double length = view.m[i][0] * view.m[i][0] + view.m[i][1] * view.m[i][1] + view.m[i][2] * view.m[i][2];
        if (fabs(length - 1.0) > 0.03)
            return false;
    }
    double p[3];
    for (int i = 0; i < 3; i++)
        p[i] = -(delta.m[3][0] * delta.m[i][0] + delta.m[3][1] * delta.m[i][1] + delta.m[3][2] * delta.m[i][2]);
    for (int j = 0; j < 3; j++)
        offset[j] = p[0] * view.m[j][0] + p[1] * view.m[j][1] + p[2] * view.m[j][2];
    return true;
}

// The eye's world offset of the last ApplyEyeConstants (for the camera position in the pixel shader constants).
static double g_eyeWorldOffset[3];
static bool g_eyeWorldOffsetValid = false;
static bool g_eyeMainCamera = false; // that draw was seen through the main camera (not a planar reflection)

void ApplyEyeConstants(IDirect3DDevice9* device, int eye)
{
    const EyeBox& box = (eye == 2) ? g_vr.unionBox : g_vr.eyeBox[eye];
    const Matrix4* gameProjectionInverse = GameProjectionInverse();
    if (!gameProjectionInverse) {
        DeviceSetVsOverrides(device, NULL, 0);
        return;
    }
    const Matrix4* delta = &g_vr.eyeCameraDelta[eye];
    Matrix4 flippedDelta;
    int flipSlot = 0;
    g_eyeWorldOffsetValid = false;
    g_eyeMainCamera = !(IsHalfResPass() && IsReflectionSizedPass());
    if (IsHalfResPass() && IsReflectionSizedPass() && g_matrixLayout) {
        int flipMode = g_cfg.reflectionFlipMode;
        double viewDeterminant = 1;
        for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
            if (tracked.reg + 4 > 256)
                continue;
            Matrix4 original;
            LoadMatrix(&g_vsConstants[tracked.reg * 4], g_matrixLayout, original);
            Matrix4 view = original;
            if (tracked.kind == TK_ViewProj || tracked.kind == TK_WorldViewProj)
                view = Multiply(original, *gameProjectionInverse);
            else if (tracked.kind != TK_View && tracked.kind != TK_WorldView)
                continue;
            viewDeterminant = view.m[0][0] * (view.m[1][1] * view.m[2][2] - view.m[1][2] * view.m[2][1]) -
                              view.m[0][1] * (view.m[1][0] * view.m[2][2] - view.m[1][2] * view.m[2][0]) +
                              view.m[0][2] * (view.m[1][0] * view.m[2][1] - view.m[1][1] * view.m[2][0]);
            break;
        }
        if (flipMode < 0) {
            int axis = ReflectionFlipAxis(); // the axis the game's own reflection camera has turned round, seen by the hooks of game_camera.cpp
            flipMode = axis >= 0 ? axis + 1 : ((viewDeterminant > 0) ? 1 : 0);
        }
        // The reflection is drawn as if the head looked straight ahead (only its yaw kept, the eyes where they are): the game lays a fixed band of ground haze below the horizon of its
        // own camera over the reflection, so a pitched reflection ran into it (the far water looked up into the dark band as the head nodded).
        flippedDelta = YawOnlyDelta(*delta);
        if (flipMode > 0) {
            int flipAxis = flipMode - 1;
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) {
                    double sign = 1;
                    if (i < 3 && i == flipAxis)
                        sign = -sign;
                    if (j < 3 && j == flipAxis)
                        sign = -sign;
                    flippedDelta.m[i][j] *= sign;
                }
            flipSlot = flipMode > 3 ? 0 : flipMode;
        }
        delta = &flippedDelta;
    }
    if (eye < 2 && g_matrixLayout) {
        for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
            if (tracked.kind != TK_ViewProj || tracked.reg + 4 > 256)
                continue;
            Matrix4 original;
            LoadMatrix(&g_vsConstants[tracked.reg * 4], g_matrixLayout, original);
            float col[12];
            for (int i = 0; i < 4; i++) {
                col[i] = (float)original.m[i][0];
                col[4 + i] = (float)original.m[i][1];
                col[8 + i] = (float)original.m[i][3];
            }
            int recordIndex = -1;
            if (g_reflectionRecordLast >= 0 &&
                ReflectionColumnsMatch(g_reflectionRecords[g_reflectionRecordLast].col, col))
                recordIndex = g_reflectionRecordLast;
            else {
                for (int recordSearch = 0; recordSearch < g_reflectionRecordCount; recordSearch++)
                    if (ReflectionColumnsMatch(g_reflectionRecords[recordSearch].col, col)) {
                        recordIndex = recordSearch;
                        break;
                    }
            }
            if (recordIndex < 0) {
                recordIndex = g_reflectionRecordNext;
                g_reflectionRecordNext = (g_reflectionRecordNext + 1) % 24;
                if (g_reflectionRecordCount < 24)
                    g_reflectionRecordCount++;
                memcpy(g_reflectionRecords[recordIndex].col, col, sizeof(col));
                g_reflectionRecords[recordIndex].valid[0] = g_reflectionRecords[recordIndex].valid[1] = false;
            }
            ReflectionRecord& record = g_reflectionRecords[recordIndex];
            record.projXScale = g_projXScale;
            record.projYScale = g_projYScale;
            record.projDepthScale = g_projDepthScale;
            record.projDepthBias = g_projDepthBias;
            record.eyeDelta[eye] = *delta;
            record.valid[eye] = true;
            record.width = g_rtWidth;
            record.height = g_rtHeight;
            g_reflectionRecordLast = recordIndex;
            break;
        }
    }
    const EyeRewrite& rewrite = GetRewrite(eye, flipSlot, box, *delta, *gameProjectionInverse);
    const Matrix4& newProj = rewrite.newProj;
    const Matrix4& clipRemap = rewrite.clipRemap;
    VsWrite writes[kMaxWrites];
    float values[kMaxWrites][16];
    int writeCount = 0;
    for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
        if (writeCount >= kMaxWrites)
            break;
        UINT count = (tracked.kind == TK_View3x3) ? 3 : 4;
        if (tracked.reg + count > 256)
            continue;
        const float* registerData = &g_vsConstants[tracked.reg * 4];
        float* out = values[writeCount];
        if (tracked.kind == TK_View3x3) {
            if (!g_cfg.adjustViewRot || !g_matrixLayout)
                continue;
            if (g_matrixLayout == 1) {
                for (int r = 0; r < 3; r++) {
                    for (int c = 0; c < 3; c++) {
                        double s = 0;
                        for (int k = 0; k < 3; k++)
                            s += registerData[r * 4 + k] * delta->m[k][c];
                        out[r * 4 + c] = (float)s;
                    }
                    out[r * 4 + 3] = registerData[r * 4 + 3];
                }
            } else {
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 4; c++) {
                        double s = 0;
                        for (int k = 0; k < 3; k++)
                            s += delta->m[k][r] * registerData[k * 4 + c];
                        out[r * 4 + c] = (float)s;
                    }
            }
        } else {
            if (!g_matrixLayout)
                continue;
            Matrix4 original, remapped;
            LoadMatrix(registerData, g_matrixLayout, original);
            switch (tracked.kind) {
                case TK_WorldViewProj:
                case TK_ViewProj:
                    remapped = Multiply(original, clipRemap);
                    break;
                case TK_WorldView:
                case TK_View:
                    remapped = Multiply(original, *delta);
                    break;
                case TK_Projection:
                    remapped = newProj;
                    break;
                default:
                    continue;
            }
            StoreMatrix(remapped, g_matrixLayout, out);
        }
        writes[writeCount] = {tracked.reg, count, out};
        writeCount++;
    }
    // The camera's world position (g_vViewPosition): view vectors for specular, reflections and fog, billboards turned to face
    // the camera.  Left alone it keeps all of that at the game's camera while the geometry moves with the head, so the shading
    // slides over the surfaces as the head moves.  Moved to the eye through the draw's own camera (the reflection pass has the
    // mirrored one, with the mirrored delta).
    if (g_matrixLayout && writeCount < kMaxWrites) {
        for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
            if ((tracked.kind != TK_View && tracked.kind != TK_ViewProj) || tracked.reg + 4 > 256)
                continue;
            Matrix4 view;
            LoadMatrix(&g_vsConstants[tracked.reg * 4], g_matrixLayout, view);
            if (tracked.kind == TK_ViewProj)
                view = Multiply(view, *gameProjectionInverse);
            g_eyeWorldOffsetValid = EyeWorldOffset(view, *delta, g_eyeWorldOffset);
            break;
        }
        UINT reg = g_currentVsInfo->viewPositionReg;
        if (g_eyeWorldOffsetValid && reg < 256) {
            float* out = values[writeCount];
            const float* game = &g_vsConstants[reg * 4];
            for (int i = 0; i < 3; i++)
                out[i] = (float)(game[i] + g_eyeWorldOffset[i]);
            out[3] = game[3];
            writes[writeCount] = {reg, 1, out};
            writeCount++;
        }
    }
    // Main-pass shaders find the planar-reflection texel from the reflection camera's matrix (x, y, w columns in
    // g_ReflectionViewMatrix0/1/3). The reflection texture was rendered through the per-eye head-tracked camera, so the
    // lookup matrix must get the same per-eye transform.
    if (g_cfg.fixReflectionLookup && eye < 2 && g_currentVsInfo->reflectionRegs[0] != 0xFFFF &&
        g_currentVsInfo->reflectionRegs[1] != 0xFFFF && g_currentVsInfo->reflectionRegs[3] != 0xFFFF) {
        UINT r0 = g_currentVsInfo->reflectionRegs[0], r1 = g_currentVsInfo->reflectionRegs[1],
             r3 = g_currentVsInfo->reflectionRegs[3];
        if (r0 < 256 && r1 < 256 && r3 < 256 && writeCount + 3 <= kMaxWrites) {
            const float* a0 = &g_vsConstants[r0 * 4];
            const float* a1 = &g_vsConstants[r1 * 4];
            const float* a3 = &g_vsConstants[r3 * 4];
            float col[12];
            for (int i = 0; i < 4; i++) {
                col[i] = a0[i];
                col[4 + i] = a1[i];
                col[8 + i] = a3[i];
            }
            int recordIndex = -1;
            for (int recordSearch = 0; recordSearch < g_reflectionRecordCount; recordSearch++)
                if (g_reflectionRecords[recordSearch].valid[eye] &&
                    ReflectionColumnsMatch(g_reflectionRecords[recordSearch].col, col)) {
                    recordIndex = recordSearch;
                    break;
                }
            if (recordIndex >= 0) {
                ReflectionRecord& record = g_reflectionRecords[recordIndex];
                const Matrix4* recordRemap = RecordRemap(record, eye);
                if (recordRemap) {
                    Matrix4 original = Identity();
                    for (int i = 0; i < 4; i++) {
                        original.m[i][0] = a0[i];
                        original.m[i][1] = a1[i];
                        original.m[i][3] = a3[i];
                        original.m[i][2] =
                            record.projDepthScale * a3[i] +
                            (i == 3 ? record.projDepthBias : 0.0); // depth column of a standard projection
                    }
                    Matrix4 remapped = Multiply(original, *recordRemap);
                    UINT regs[3] = {r0, r1, r3};
                    float* out = values[writeCount]; // the three columns, one register each, one after the other
                    for (int column = 0; column < 3; column++) {
                        int source = (column == 2) ? 3 : column;
                        for (int i = 0; i < 4; i++)
                            out[column * 4 + i] = (float)remapped.m[i][source];
                    }
                    if (r1 == r0 + 1 && r3 == r1 + 1) { // consecutive registers: one write
                        writes[writeCount] = {r0, 3, out};
                        writeCount++;
                    } else
                        for (int column = 0; column < 3; column++) {
                            writes[writeCount] = {regs[column], 1, out + column * 4};
                            writeCount++;
                        }
                }
            }
        }
    }
    DeviceSetVsOverrides(device, writes, writeCount);
}

void RestoreEyeConstants(IDirect3DDevice9* device)
{
    DeviceSetVsOverrides(device, NULL, 0);
}

void ApplyCameraShift(IDirect3DDevice9* device, const Matrix4& oldProj, const Matrix4& delta, const Matrix4& newProj)
{
    Matrix4 oldProjInverse;
    if (!g_matrixLayout || !Invert(oldProj, oldProjInverse)) {
        DeviceSetVsOverrides(device, NULL, 0);
        return;
    }
    Matrix4 clipRemap = Multiply(Multiply(oldProjInverse, delta), newProj);
    VsWrite writes[kMaxWrites];
    float values[kMaxWrites][16];
    int writeCount = 0;
    for (const TrackedConstant& tracked : g_currentVsInfo->tracked) {
        if (writeCount >= kMaxWrites)
            break;
        if (tracked.kind == TK_View3x3 || tracked.reg + 4 > 256)
            continue; // (a pure shift leaves the 3x3 view rotation alone)
        Matrix4 original, remapped;
        LoadMatrix(&g_vsConstants[tracked.reg * 4], g_matrixLayout, original);
        switch (tracked.kind) {
            case TK_WorldViewProj:
            case TK_ViewProj:
                remapped = Multiply(original, clipRemap);
                break;
            case TK_WorldView:
            case TK_View:
                remapped = Multiply(original, delta);
                break;
            case TK_Projection:
                remapped = newProj;
                break;
            default:
                continue;
        }
        StoreMatrix(remapped, g_matrixLayout, values[writeCount]);
        writes[writeCount] = {tracked.reg, 4, values[writeCount]};
        writeCount++;
    }
    DeviceSetVsOverrides(device, writes, writeCount);
}

// The main view's camera in the pixel shader constants, one column per register (GameWorld/View/Main.cpp): load it from the
// game's values, false when they are not there or not of the expected kind.
static bool LoadPixelColumns(int base, Matrix4& matrix)
{
    if (base < 0 || base + 4 > 256)
        return false;
    for (int column = 0; column < 4; column++)
        for (int row = 0; row < 4; row++)
            matrix.m[row][column] = g_psConstants[(base + column) * 4 + row];
    return true;
}
static void StorePixelColumns(const Matrix4& matrix, int base, unsigned used, PFN_SetShaderConstantF setConstants,
                              IDirect3DDevice9* device)
{
    for (int column = 0; column < 4; column++) {
        if (!(used & (1u << column)))
            continue;
        float value[4];
        for (int row = 0; row < 4; row++)
            value[row] = (float)matrix.m[row][column];
        setConstants(device, (UINT)(base + column), value, 1);
    }
}
static bool IsRigid(const Matrix4& view)
{
    for (int i = 0; i < 3; i++) {
        for (int j = i; j < 3; j++) {
            double d = view.m[i][0] * view.m[j][0] + view.m[i][1] * view.m[j][1] + view.m[i][2] * view.m[j][2];
            if (fabs(d - (i == j ? 1.0 : 0.0)) > 0.03)
                return false;
        }
        if (fabs(view.m[i][3]) > 1e-3)
            return false;
    }
    return fabs(view.m[3][3] - 1.0) < 1e-3;
}

// The camera of a view-projection that is given by its x, y and w columns only (a pixel shader declares the columns it uses; the z column of the game's copy is
// not always the one that goes with the others).  The x and y scales of the projection are read from the columns themselves, so a projection with another field
// of view than the one the mod tracks is no trouble: column x is a * (the camera's x axis), column y is b * (its y axis) and column w is its z axis.  False when
// they are not the columns of a camera followed by a perspective projection.
static bool ViewFromProjectionColumns(const Matrix4& columns, Matrix4& view)
{
    double a = sqrt(columns.m[0][0] * columns.m[0][0] + columns.m[1][0] * columns.m[1][0] + columns.m[2][0] * columns.m[2][0]);
    double b = sqrt(columns.m[0][1] * columns.m[0][1] + columns.m[1][1] * columns.m[1][1] + columns.m[2][1] * columns.m[2][1]);
    if (!(a > 1e-6) || !(b > 1e-6))
        return false;
    view = Identity();
    for (int i = 0; i < 4; i++) {
        view.m[i][0] = columns.m[i][0] / a;
        view.m[i][1] = columns.m[i][1] / b;
        view.m[i][2] = columns.m[i][3];
    }
    return IsRigid(view);
}

bool ApplyEyePixelConstants(IDirect3DDevice9* device, int eye)
{
    const PixelShaderInfo* ps = g_currentPsInfo;
    if (!ps || !ps->eyeConstants || !g_eyeMainCamera)
        return false; // (in the reflection pass these still describe the main camera, not the mirrored one being drawn)
    PFN_SetShaderConstantF setConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
    const Matrix4& delta = g_vr.eyeCameraDelta[eye];
    bool wrote = false;
    Matrix4 view;
    bool haveView = LoadPixelColumns(ps->viewMatrixBase, view) && IsRigid(view);
    if (haveView) {
        // view-space normals of the lighting pre-pass (road, ocean): with the game's matrix they stay in the game camera's
        // space while the lights are moved to the eye, and the road's lighting turns with the head
        StorePixelColumns(Multiply(view, delta), ps->viewMatrixBase, ps->viewMatrixUsed, setConstants, device);
        wrote = true;
    }
    Matrix4 viewProj, viewFromColumns;
    if (LoadPixelColumns(ps->viewProjBase, viewProj) && ViewFromProjectionColumns(viewProj, viewFromColumns)) {
        // (water: a reflected point projected to the screen to look up the scene picture, which is the eye's half)
        const EyeBox& box = (eye == 2) ? g_vr.unionBox : g_vr.eyeBox[eye];
        StorePixelColumns(Multiply(Multiply(viewFromColumns, delta), BuildEyeProjection(box, eye)), ps->viewProjBase, ps->viewProjUsed, setConstants, device);
        wrote = true;
    }
    double offset[3];
    bool haveOffset = false;
    if (g_eyeWorldOffsetValid) {
        memcpy(offset, g_eyeWorldOffset, sizeof(offset));
        haveOffset = true;
    } else if (haveView)
        haveOffset = EyeWorldOffset(view, delta, offset);
    for (int slot = 0; slot < 2 && haveOffset; slot++) {
        int reg = ps->viewPositionRegs[slot];
        if (reg < 0 || reg >= 256)
            continue;
        const float* game = &g_psConstants[reg * 4];
        float value[4] = {(float)(game[0] + offset[0]), (float)(game[1] + offset[1]), (float)(game[2] + offset[2]), game[3]};
        setConstants(device, (UINT)reg, value, 1);
        wrote = true;
    }
    return wrote;
}

void RestoreEyePixelConstants(IDirect3DDevice9* device)
{
    const PixelShaderInfo* ps = g_currentPsInfo;
    if (!ps)
        return;
    PFN_SetShaderConstantF setConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
    auto restore = [&](int base, unsigned used) {
        for (int column = 0; column < 4 && base >= 0 && base + 4 <= 256; column++)
            if (used & (1u << column))
                setConstants(device, (UINT)(base + column), &g_psConstants[(base + column) * 4], 1);
    };
    restore(ps->viewMatrixBase, ps->viewMatrixUsed);
    restore(ps->viewProjBase, ps->viewProjUsed);
    for (int slot = 0; slot < 2; slot++)
        if (ps->viewPositionRegs[slot] >= 0 && ps->viewPositionRegs[slot] < 256)
            setConstants(device, (UINT)ps->viewPositionRegs[slot], &g_psConstants[ps->viewPositionRegs[slot] * 4], 1);
}
