// Per-eye versions of the game's screen-space passes: motion blur, shadow decals, deferred lights, and the bloom switch.
#include "screen_space_passes.h"
#include "camera.h"
#include "core.h"
#include "config.h"
#include "d3d_hooks.h"
#include "matrix.h"
#include "render_state.h"
#include "shader_tracking.h"
#include "vr_runtime.h"

// The "glow" around lights and flares on the car comes from the quarter-size bloom texture that the final composite
// adds; NoLensFlare=1 swaps it for opaque black.
static IDirect3DTexture9* g_blackTexture = NULL;
HRESULT DrawWithoutBloom(IDirect3DDevice9* device, const DrawCall& draw)
{
    int fullSizeSlot = -1, quarterSizeSlot = -1;
    for (int t = 0; t < 8; t++) {
        IDirect3DBaseTexture9* boundTexture = g_boundTextures[t];
        if (!boundTexture || !g_renderTargetTextures.count((void*)boundTexture) ||
            boundTexture->GetType() != D3DRTYPE_TEXTURE)
            continue;
        D3DSURFACE_DESC surfaceDesc;
        if (FAILED(((IDirect3DTexture9*)boundTexture)->GetLevelDesc(0, &surfaceDesc)))
            continue;
        if (surfaceDesc.Width >= g_backBufferWidth * 0.9)
            fullSizeSlot = t;
        else if (surfaceDesc.Width >= g_backBufferWidth * 0.2 && surfaceDesc.Width <= g_backBufferWidth * 0.3)
            quarterSizeSlot = t; // the last one of the two quarter-size inputs
    }
    if (fullSizeSlot < 0 || quarterSizeSlot < 0)
        return draw();
    if (!g_blackTexture) {
        PFN_CreateTexture createTexture = (PFN_CreateTexture)g_deviceOriginals[VT_CreateTexture];
        // (an Ex device, SharedTransport, has no managed pool: a lockable default-pool texture does the same)
        if (SUCCEEDED(createTexture(device, 4, 4, 1, g_deviceIsEx ? D3DUSAGE_DYNAMIC : 0, D3DFMT_A8R8G8B8,
                                    g_deviceIsEx ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &g_blackTexture, NULL)) &&
            g_blackTexture) {
            D3DLOCKED_RECT lockedRect;
            if (SUCCEEDED(g_blackTexture->LockRect(0, &lockedRect, NULL, 0))) {
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        ((DWORD*)((BYTE*)lockedRect.pBits + y * lockedRect.Pitch))[x] = 0xFF000000;
                g_blackTexture->UnlockRect(0);
            }
        }
    }
    if (!g_blackTexture)
        return draw();
    PFN_SetTexture setTexture = (PFN_SetTexture)g_deviceOriginals[VT_SetTexture];
    IDirect3DBaseTexture9* savedTexture = g_boundTextures[quarterSizeSlot];
    setTexture(device, quarterSizeSlot, g_blackTexture);
    HRESULT hr = draw();
    setTexture(device, quarterSizeSlot, savedTexture);
    return hr;
}

// The game's motion-blur pass reprojects every pixel with one camera matrix expressed in the texture coordinates of the
// whole (side-by-side) image, so its radial centre lands on the seam between the eyes. Run it once per eye instead,
// with the matrix re-expressed in that eye's own frame.

// [VR] MotionBlur=0: the blur's pixel shader is swapped for one that copies the colour image, so the pass still writes the picture the
// composite reads but costs one texture read a pixel instead of the blur's many. Sampler 0 is the colour image in every variant of the
// blur (ps_2_0 and ps_3_0 alike), and the vertex shader it is paired with only has to pass the texture coordinate on. A pixel shader has
// to be of the model of its vertex shader, so there is one of each model. Hand assembled (tools/motion_blur_check.cpp decodes both):
//   ps_2_0 / ps_3_0, def c0, 1, 1, 1, 1 / dcl t0.xy (v0.xy with texcoord0) / dcl_2d s0 / texld r0, t0 (v0), s0 / mov r0.w, c0.x / mov oC0, r0
static const DWORD kMotionBlurCopyShader20[] = {
    0xFFFF0200,
    0x05000051, 0xA00F0000, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000, // def c0, 1, 1, 1, 1
    0x0200001F, 0x80000000, 0xB0030000,                                     // dcl t0.xy
    0x0200001F, 0x90000000, 0xA00F0800,                                     // dcl_2d s0
    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,                         // texld r0, t0, s0
    0x02000001, 0x80080000, 0xA0000000,                                     // mov r0.w, c0.x
    0x02000001, 0x800F0800, 0x80E40000,                                     // mov oC0, r0
    0x0000FFFF};
static const DWORD kMotionBlurCopyShader30[] = {
    0xFFFF0300,
    0x05000051, 0xA00F0000, 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000, // def c0, 1, 1, 1, 1
    0x0200001F, 0x80000005, 0x90030000,                                     // dcl_texcoord v0.xy
    0x0200001F, 0x90000000, 0xA00F0800,                                     // dcl_2d s0
    0x03000042, 0x800F0000, 0x90E40000, 0xA0E40800,                         // texld r0, v0, s0
    0x02000001, 0x80080000, 0xA0000000,                                     // mov r0.w, c0.x
    0x02000001, 0x800F0800, 0x80E40000,                                     // mov oC0, r0
    0x0000FFFF};
static IDirect3DDevice9* g_copyShaderDevice = NULL; // the device the copy shaders below belong to
static IDirect3DPixelShader9* g_copyShader[2] = {NULL, NULL}; // ps_2_0, ps_3_0
static bool g_copyShaderFailed[2] = {false, false};

static IDirect3DPixelShader9* CopyShaderFor(IDirect3DDevice9* device, int shaderMajor)
{
    if (device != g_copyShaderDevice) { // (a shader belongs to the device that made it)
        g_copyShader[0] = g_copyShader[1] = NULL;
        g_copyShaderFailed[0] = g_copyShaderFailed[1] = false;
        g_copyShaderDevice = device;
    }
    int slot = shaderMajor >= 3 ? 1 : 0;
    if (!g_copyShader[slot] && !g_copyShaderFailed[slot]) {
        PFN_CreatePixelShader createPixelShader = (PFN_CreatePixelShader)g_deviceOriginals[VT_CreatePixelShader];
        if (FAILED(createPixelShader(device, slot ? kMotionBlurCopyShader30 : kMotionBlurCopyShader20, &g_copyShader[slot])) ||
            !g_copyShader[slot]) {
            g_copyShader[slot] = NULL;
            g_copyShaderFailed[slot] = true;
            Log("motion blur: the copy shader (ps_%d_0) could not be created: MotionBlur=0 leaves the blur as the game draws it", slot ? 3 : 2);
        }
    }
    return g_copyShader[slot];
}

// The log says once for each blur shader the game uses (it has 26 variants: sample counts, a filter spread) which one this run draws.
static void NoteMotionBlurShader(const PixelShaderInfo* info, const char* how)
{
    static int seen[8];
    static int seenCount = 0;
    for (int i = 0; i < seenCount; i++)
        if (seen[i] == info->id)
            return;
    if (seenCount >= 8)
        return;
    seen[seenCount++] = info->id;
    bool filter = false;
    for (const ShaderConstant& constant : info->constants)
        filter = filter || NamesEqual(constant.name.c_str(), "g_vFilter");
    Log("motion blur: pixel shader %d (ps_%d_0, %d constants%s) %s", info->id, info->shaderMajor, (int)info->constants.size(),
        filter ? ", with the g_vFilter taps" : "", how);
}

// Draw the pass once with the copy shader in place of the blur's; false when it cannot be done (the caller then blurs as the game does).
static bool DrawMotionBlurAsCopy(IDirect3DDevice9* device, const DrawCall& draw, HRESULT* result)
{
    if (!g_currentPsHandle || !g_currentPsInfo)
        return false;
    IDirect3DPixelShader9* copyShader = CopyShaderFor(device, g_currentPsInfo->shaderMajor);
    if (!copyShader)
        return false;
    NoteMotionBlurShader(g_currentPsInfo, "drawn as a plain copy (MotionBlur=0)");
    PFN_SetPixelShader setPixelShader = (PFN_SetPixelShader)g_deviceOriginals[VT_SetPixelShader]; // (not the hooked one: the tracking keeps the blur's info)
    setPixelShader(device, copyShader);
    *result = draw();
    setPixelShader(device, (IDirect3DPixelShader9*)g_currentPsHandle);
    return true;
}

HRESULT MotionBlurPerEye(IDirect3DDevice9* device, const DrawCall& draw)
{
    if (!g_cfg.motionBlur) {
        HRESULT copied = D3D_OK;
        if (DrawMotionBlurAsCopy(device, draw, &copied))
            return copied;
    } else if (g_currentPsInfo)
        NoteMotionBlurShader(g_currentPsInfo, "drawn per eye");
    PFN_SetShaderConstantF setPixelConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    float originalConstants[16];
    memcpy(originalConstants, g_psConstants,
           sizeof(originalConstants)); // g_mMatrix: c0..c3 (rows u, v, depth, 1; columns x, y, w used)
    HRESULT hr = D3D_OK;
    setRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE);
    for (int eye = 0; eye < 2; eye++) {
        const EyeBox& box = g_vr.eyeBox[eye];
        double xOffset = eye ? 0.5 : 0.0, tanWidth = box.right - box.left, tanHeight = box.up + box.down;
        double a1 = g_projXScale * tanWidth,
               b1 = 0.5 +
                    0.5 * g_projXScale *
                        (box.left -
                         2 * xOffset * tanWidth); // eye texcoord u -> "virtual full-screen" u of the game's own camera
        double a2 = 0.5 * g_projYScale * tanHeight,
               b2 = 0.5 - 0.5 * g_projYScale * box.up; // eye texcoord v -> virtual v
        if (fabs(a1) < 1e-6 || fabs(a2) < 1e-6)
            continue;
        double original[4][4], toVirtual[4][4] = {{a1, 0, 0, 0}, {0, a2, 0, 0}, {0, 0, 1, 0}, {b1, b2, 0, 1}};
        double fromVirtual[4][4] = {{1 / a1, 0, 0, 0}, {0, 1 / a2, 0, 0}, {0, 0, 1, 0}, {-b1 / a1, -b2 / a2, 0, 1}};
        for (int i = 0; i < 4; i++) {
            original[i][0] = originalConstants[i * 4];
            original[i][1] = originalConstants[i * 4 + 1];
            original[i][2] = 0;
            original[i][3] = originalConstants[i * 4 + 3];
        }
        double product[4][4], remapped[4][4];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                double v = 0;
                for (int k = 0; k < 4; k++)
                    v += toVirtual[i][k] * original[k][j];
                product[i][j] = v;
            }
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                double v = 0;
                for (int k = 0; k < 4; k++)
                    v += product[i][k] * fromVirtual[k][j];
                remapped[i][j] = v;
            }
        float newConstants[16];
        for (int i = 0; i < 4; i++) {
            newConstants[i * 4] = (float)remapped[i][0];
            newConstants[i * 4 + 1] = (float)remapped[i][1];
            newConstants[i * 4 + 2] = originalConstants[i * 4 + 2];
            newConstants[i * 4 + 3] = (float)remapped[i][3];
        }
        setPixelConstants(device, 0, newConstants, 4);
        RECT r;
        r.left = (LONG)(eye * (g_rtWidth / 2));
        r.right = (LONG)((eye + 1) * (g_rtWidth / 2));
        r.top = 0;
        r.bottom = (LONG)g_rtHeight;
        setScissorRect(device, &r);
        hr = draw();
    }
    setPixelConstants(device, 0, originalConstants, 4);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
    return hr;
}

// Projected-shadow decals rebuild each pixel's camera-space position from the depth buffer and the screen position,
// then map it into the decal's plane space with one matrix built from the game's mono camera. Re-express that matrix
// for the eye being drawn: its own projection (screen position = half of the side-by-side image) and its own
// head-tracked camera.
static Matrix4 ScreenZMapping(const Matrix4& proj)
{
    // row vector (X, Y, Z, 1) of a camera-space point  ->  (u*Z, v*Z, 1, Z) with u,v the 0..1 screen position under
    // projection P
    Matrix4 map = Identity();
    for (int i = 0; i < 4; i++) {
        map.m[i][0] = 0.5 * proj.m[i][3] + 0.5 * proj.m[i][0];
        map.m[i][1] = 0.5 * proj.m[i][3] - 0.5 * proj.m[i][1];
        map.m[i][2] = (i == 3) ? 1.0 : 0.0;
        map.m[i][3] = proj.m[i][3];
    }
    return map;
}
bool ApplyShadowDecalForEye(IDirect3DDevice9* device, int eye, float* savedConstants)
{
    if (eye > 1 || !g_haveProjection || !g_currentPsInfo ||
        g_currentPsInfo->shadowMatrixReg < 0)
        return false;
    UINT reg = (UINT)g_currentPsInfo->shadowMatrixReg;
    const EyeBox& box = g_vr.eyeBox[eye];
    Matrix4 oldProj = Identity();
    oldProj.m[0][0] = g_projXScale;
    oldProj.m[1][1] = g_projYScale;
    oldProj.m[2][2] = g_projDepthScale;
    oldProj.m[2][3] = 1;
    oldProj.m[3][2] = g_projDepthBias;
    oldProj.m[3][3] = 0;
    Matrix4 newProj = BuildEyeProjection(box, eye), oldMap = ScreenZMapping(oldProj), newMap = ScreenZMapping(newProj),
            newMapInverse, deltaInverse;
    if (!Invert(newMap, newMapInverse) || !Invert(g_vr.eyeCameraDelta[eye], deltaInverse))
        return false;
    Matrix4 originalMatrix;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            originalMatrix.m[i][j] = g_psConstants[(reg + i) * 4 + j];
    Matrix4 newMatrix = Multiply(Multiply(Multiply(newMapInverse, deltaInverse), oldMap), originalMatrix);
    float out[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (float)newMatrix.m[i][j];
    memcpy(savedConstants, &g_psConstants[reg * 4], 16 * sizeof(float));
    PFN_SetShaderConstantF setPixelConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
    setPixelConstants(device, reg, out, 4);
    return true;
}

// Deferred lighting: the full-screen light passes rebuild each pixel's camera-space position from the depth buffer and
// a view ray interpolated between four frustum corners (g_vFar0..3) of the game's mono camera, then compare it with
// light positions given in that camera's space. Run per eye with that eye's own rays, and move the lights into the
// eye's camera space (the normals in the G-buffer were already rendered there).
static void TransformPoint(const Matrix4& delta, const float* input, float* output)
{
    for (int j = 0; j < 3; j++)
        output[j] =
            (float)(input[0] * delta.m[0][j] + input[1] * delta.m[1][j] + input[2] * delta.m[2][j] + delta.m[3][j]);
    output[3] = input[3];
}
static void TransformDirection(const Matrix4& delta, const float* input, float* output)
{
    for (int j = 0; j < 3; j++)
        output[j] = (float)(input[0] * delta.m[0][j] + input[1] * delta.m[1][j] + input[2] * delta.m[2][j]);
    output[3] = input[3];
}
int FindFarCornerRegister()
{
    if (!g_currentVsInfo)
        return -1;
    for (const ShaderConstant& c : g_currentVsInfo->constants)
        if (NamesEqual(c.name.c_str(), "g_vFar0") && c.reg + 4 <= 256)
            return (int)c.reg;
    return -1;
}
HRESULT DrawLightsPerEye(IDirect3DDevice9* device, const DrawCall& draw, int farReg)
{
    PFN_SetShaderConstantF setPixelConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
    PFN_SetShaderConstantF setVertexConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetVertexShaderConstantF];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    float originalCorners[16];
    memcpy(originalCorners, &g_vsConstants[farReg * 4], sizeof(originalCorners));
    for (int i = 0; i < 4; i++)
        if (fabsf(originalCorners[i * 4]) < 1e-6f || fabsf(originalCorners[i * 4 + 1]) < 1e-6f)
            return draw();
    const PixelShaderInfo* pixelShader = g_currentPsInfo;
    int lightPosRegister = pixelShader->lightPosReg, lightPosCount = pixelShader->lightPosCount,
        upRegister = pixelShader->viewUpReg;
    float originalPositions[32], originalUp[4];
    memcpy(originalPositions, &g_psConstants[lightPosRegister * 4], lightPosCount * 4 * sizeof(float));
    if (upRegister >= 0)
        memcpy(originalUp, &g_psConstants[upRegister * 4], sizeof(originalUp));
    HRESULT hr = D3D_OK;
    setRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE);
    for (int eye = 0; eye < 2; eye++) {
        Matrix4 newProj = BuildEyeProjection(g_vr.eyeBox[eye], eye);
        const Matrix4& delta = g_vr.eyeCameraDelta[eye];
        double xl = (-1 - newProj.m[2][0]) / newProj.m[0][0], xr = (1 - newProj.m[2][0]) / newProj.m[0][0];
        double yb = (-1 - newProj.m[2][1]) / newProj.m[1][1], yt = (1 - newProj.m[2][1]) / newProj.m[1][1];
        float newCorners[16];
        for (int i = 0; i < 4; i++) {
            double cornerDepth = originalCorners[i * 4 + 2];
            newCorners[i * 4] = (float)((originalCorners[i * 4] < 0 ? xl : xr) * cornerDepth);
            newCorners[i * 4 + 1] = (float)((originalCorners[i * 4 + 1] < 0 ? yb : yt) * cornerDepth);
            newCorners[i * 4 + 2] = originalCorners[i * 4 + 2];
            newCorners[i * 4 + 3] = originalCorners[i * 4 + 3];
        }
        setVertexConstants(device, farReg, newCorners, 4);
        float newPositions[32];
        for (int j = 0; j < lightPosCount; j++)
            TransformPoint(delta, &originalPositions[j * 4], &newPositions[j * 4]);
        setPixelConstants(device, lightPosRegister, newPositions, lightPosCount);
        if (upRegister >= 0) {
            float newUp[4];
            TransformDirection(delta, originalUp, newUp);
            setPixelConstants(device, upRegister, newUp, 1);
        }
        RECT r;
        r.left = (LONG)(eye * (g_rtWidth / 2));
        r.right = (LONG)((eye + 1) * (g_rtWidth / 2));
        r.top = 0;
        r.bottom = (LONG)g_rtHeight;
        setScissorRect(device, &r);
        hr = draw();
    }
    setVertexConstants(device, farReg, originalCorners, 4);
    setPixelConstants(device, lightPosRegister, originalPositions, lightPosCount);
    if (upRegister >= 0)
        setPixelConstants(device, upRegister, originalUp, 1);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
    return hr;
}
// light-volume pass (spot/projected lights drawn as geometry): screen position -> view ray slopes (g_vXY), light
// position and volume-texture matrix
bool ApplyLightVolumeForEye(IDirect3DDevice9* device, int eye, float* saved)
{
    if (eye > 1 || !g_currentPsInfo || g_currentPsInfo->viewRayScaleReg < 0 ||
        g_currentPsInfo->lightMatrixReg < 0 || g_currentPsInfo->lightPosReg < 0)
        return false;
    PFN_SetShaderConstantF setPixelConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
    Matrix4 newProj = BuildEyeProjection(g_vr.eyeBox[eye], eye), deltaInverse;
    const Matrix4& delta = g_vr.eyeCameraDelta[eye];
    if (!Invert(delta, deltaInverse))
        return false;
    UINT xyRegister = (UINT)g_currentPsInfo->viewRayScaleReg, matrixRegister = (UINT)g_currentPsInfo->lightMatrixReg,
         positionRegister = (UINT)g_currentPsInfo->lightPosReg;
    memcpy(saved, &g_psConstants[xyRegister * 4], 4 * sizeof(float));
    memcpy(saved + 4, &g_psConstants[matrixRegister * 4], 16 * sizeof(float));
    memcpy(saved + 20, &g_psConstants[positionRegister * 4], 4 * sizeof(float));
    float viewRayScale[4] = {(float)(2.0 / newProj.m[0][0]), (float)(-2.0 / newProj.m[1][1]),
                             (float)((-1 - newProj.m[2][0]) / newProj.m[0][0]),
                             (float)((1 - newProj.m[2][1]) / newProj.m[1][1])};
    Matrix4 originalMatrix;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            originalMatrix.m[i][j] = saved[4 + i * 4 + j];
    Matrix4 newMatrix = Multiply(deltaInverse, originalMatrix);
    float newMatrixData[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            newMatrixData[i * 4 + j] = (float)newMatrix.m[i][j];
    float newPosition[4];
    TransformPoint(delta, saved + 20, newPosition);
    setPixelConstants(device, xyRegister, viewRayScale, 1);
    setPixelConstants(device, matrixRegister, newMatrixData, 4);
    setPixelConstants(device, positionRegister, newPosition, 1);
    return true;
}
