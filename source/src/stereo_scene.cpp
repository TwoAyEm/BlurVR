// Stereo duplication of main-pass draws: rewriting the tracked shader matrices for one eye.
#include "stereo_scene.h"
#include "camera.h"
#include "config.h"
#include "d3d_hooks.h"
#include "device_state.h"
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
        if (flipMode < 0)
            flipMode = (viewDeterminant > 0) ? 1 : 0;
        if (flipMode > 0) {
            flippedDelta = *delta;
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
            delta = &flippedDelta;
            flipSlot = flipMode > 3 ? 0 : flipMode;
        }
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
