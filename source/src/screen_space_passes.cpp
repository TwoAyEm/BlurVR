// Per-eye versions of the game's screen-space passes: motion blur, shadow decals, deferred lights, and the bloom switch.
#include "screen_space_passes.h"
#include <stdarg.h>
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
// with the matrix re-expressed in that eye's own frame (BlurMatrixForEyeRawDepth, BlurMatrixForEye).

// The register of one of the blur shader's constants, from its constant table (the fallback when the shader does not name it).
static UINT BlurConstantRegister(const char* name, UINT fallback)
{
    if (g_currentPsInfo)
        for (const ShaderConstant& constant : g_currentPsInfo->constants)
            if (constant.reg < 250 && NamesEqual(constant.name.c_str(), name))
                return constant.reg;
    return fallback;
}

// [VR] MotionBlur=0: the pass is drawn as the game's own blur draws a picture in which nothing moves, which is a picture without blur. The pass cannot just be skipped: it writes
// the texture the final pass reads. What the blur writes is the colour (taps along the motion) and, in the shaders that take (u, v, depth, 1) (the one the game uses at its default
// settings: it runs on half the size of the picture), the length of the blur as its alpha, which the final pass takes the blurred picture by: so a copy with an alpha of 1 shows the
// whole picture at half its resolution. For a pixel that does not move those shaders write the colour image times g_vDepthScale.w (saturated) and an alpha of 0. That is what this shader
// writes, for one texture read a pixel instead of the blur's twelve. Sampler 0 is the colour image, c4 g_vDepthScale, and the vertex shader it is paired with only has to pass the
// texture coordinate on. Hand assembled (tools/motion_blur_check.cpp decodes it):
//   ps_3_0 / def c1, 0, 0, 0, 0 / dcl_texcoord v0.xy / dcl_2d s0 / texld r0, v0, s0 / mul_sat r0.xyz, r0, c4.w / mov r0.w, c1.x / mov oC0, r0
static const DWORD kMotionBlurStillShader30[] = {
    0xFFFF0300,
    0x05000051, 0xA00F0001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // def c1, 0, 0, 0, 0
    0x0200001F, 0x80000005, 0x90030000,                                     // dcl_texcoord v0.xy
    0x0200001F, 0x90000000, 0xA00F0800,                                     // dcl_2d s0
    0x03000042, 0x800F0000, 0x90E40000, 0xA0E40800,                         // texld r0, v0, s0
    0x03000005, 0x80170000, 0x80E40000, 0xA0FF0004,                         // mul_sat r0.xyz, r0, c4.w
    0x02000001, 0x80080000, 0xA0000001,                                     // mov r0.w, c1.x
    0x02000001, 0x800F0800, 0x80E40000,                                     // mov oC0, r0
    0x0000FFFF};
static IDirect3DDevice9* g_stillShaderDevice = NULL; // the device the shader below belongs to
static IDirect3DPixelShader9* g_stillShader = NULL;
static bool g_stillShaderFailed = false;

static IDirect3DPixelShader9* StillShader(IDirect3DDevice9* device)
{
    if (device != g_stillShaderDevice) { // (a shader belongs to the device that made it)
        g_stillShader = NULL;
        g_stillShaderFailed = false;
        g_stillShaderDevice = device;
    }
    if (!g_stillShader && !g_stillShaderFailed) {
        PFN_CreatePixelShader createPixelShader = (PFN_CreatePixelShader)g_deviceOriginals[VT_CreatePixelShader];
        if (FAILED(createPixelShader(device, kMotionBlurStillShader30, &g_stillShader)) || !g_stillShader) {
            g_stillShader = NULL;
            g_stillShaderFailed = true;
            Log("motion blur: the shader for MotionBlur=0 could not be created: the blur is drawn with a matrix that moves nothing instead");
        }
    }
    return g_stillShader;
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
    Log("motion blur: pixel shader %d (ps_%d_0, %d constants%s, %s) %s", info->id, info->shaderMajor, (int)info->constants.size(),
        filter ? ", with the g_vFilter taps" : "", info->blurDepthRaw ? "takes the depth as the buffer holds it" : "turns the depth into a distance first", how);
}

// MotionBlur=0: draw the pass once, without blur; false when it cannot be done (the caller then blurs as the game does).
static bool DrawMotionBlurStill(IDirect3DDevice9* device, const DrawCall& draw, HRESULT* result)
{
    if (!g_currentPsHandle || !g_currentPsInfo)
        return false;
    const PixelShaderInfo* info = g_currentPsInfo;
    if (info->blurDepthRaw && info->shaderMajor == 3 && BlurConstantRegister("g_vDepthScale", 4) == 4)
        if (IDirect3DPixelShader9* still = StillShader(device)) {
            NoteMotionBlurShader(info, "drawn as the picture without blur, from its colour image (MotionBlur=0)");
            PFN_SetPixelShader setPixelShader = (PFN_SetPixelShader)g_deviceOriginals[VT_SetPixelShader]; // (not the hooked one: the tracking keeps the blur's info)
            setPixelShader(device, still);
            *result = draw();
            setPixelShader(device, (IDirect3DPixelShader9*)g_currentPsHandle);
            return true;
        }
    // any other blur shader: its own, once, with a matrix that moves nothing (the identity for the kind that takes (u, v, depth, 1); for the kind that takes (z*u, z*v, z, 1) the
    // one that leaves each pixel where it is). The pass then costs what it did, but writes what the game writes for a picture that does not move.
    static const float kStillRaw[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const float kStillDistance[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    PFN_SetShaderConstantF setPixelConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetPixelShaderConstantF];
    float gameMatrix[16];
    memcpy(gameMatrix, g_psConstants, sizeof(gameMatrix));
    NoteMotionBlurShader(info, "drawn once with a matrix that moves nothing (MotionBlur=0)");
    setPixelConstants(device, 0, info->blurDepthRaw ? kStillRaw : kStillDistance, 4);
    *result = draw();
    setPixelConstants(device, 0, gameMatrix, 4);
    return true;
}

// The game's blur matrix for one eye, for the blur shaders that turn the depth into a distance first (PixelShaderInfo::blurDepthRaw false; 18 of the game's 26, not seen
// in a headset: the shader the game uses at its default settings is of the other kind, BlurMatrixForEyeRawDepth below). The shader rebuilds each pixel's place from the depth buffer as q = (z*u, z*v, z, 1), z the depth along the view axis and
// u, v the texture coordinates, and the game's g_mMatrix (c0..c3, columns x, y, w) takes it to where that point was in the previous frame, (x', y', w'): the
// previous texture coordinate is (x'/w', y'/w') and w' is the point's depth in the previous camera. That is the game's own camera. For an eye the same point is
//   q of the eye -> a point in the eye's camera -> (inverse of the eye delta: the head's pose and the eye's place in it) -> a point in the game's camera -> the game's q
//   -> the game's matrix -> (x', y', w') -> the point in the previous game camera -> (the same delta) -> the previous eye camera -> the eye's previous (x', y', w').
// The head stays where it is between the two frames: only the game camera's motion blurs, a turn of the head does not. And because it is the same world point and the
// game's own motion that go through the eye's view, the blur runs along the car's direction of travel whichever way the head looks (the first version put the
// radial centre of the game's blur at the middle of each eye's picture).
// The translation of the delta acts on the constant 1 which the previous point (X', Y', Z') has no room for in a row vector, so it is added to the constant row.
bool BlurMatrixForEye(const float* original, const EyeBox& box, int eye, const Matrix4& delta, double projX, double projY, float* out)
{
    const double width = box.right - box.left, height = box.up + box.down, offset = eye ? 0.5 : 0.0;
    if (fabs(width) < 1e-6 || fabs(height) < 1e-6 || fabs(projX) < 1e-6 || fabs(projY) < 1e-6)
        return false;
    // (X, Y, Z, 1) of the eye's camera -> (Z*u, Z*v, Z, 1), u and v the texture coordinates in the side-by-side picture (the eye's tangent box spans its half)
    const double eyeIn[16] = {1 / (2 * width), 0, 0, 0, 0, -1 / height, 0, 0, offset - box.left / (2 * width), box.up / height, 1, 0, 0, 0, 0, 1};
    // and the way back out, as the shader reads it: (X, Y, Z, 1) -> (Z*u, Z*v, unused, Z)
    const double eyeOut[16] = {1 / (2 * width), 0, 0, 0, 0, -1 / height, 0, 0, offset - box.left / (2 * width), box.up / height, 0, 1, 0, 0, 0, 0};
    // the same for the game's camera, which has the whole picture
    const double gameIn[16] = {0.5 * projX, 0, 0, 0, 0, -0.5 * projY, 0, 0, 0.5, 0.5, 1, 0, 0, 0, 0, 1};
    // (x', y', unused, w') -> (X', Y', Z', 0): the point the game's matrix finds, in the previous game camera
    const double gameBack[16] = {2 / projX, 0, 0, 0, 0, -2 / projY, 0, 0, 0, 0, 0, 0, -1 / projX, 1 / projY, 1, 0};
    Matrix4 eyeInMatrix, eyeOutMatrix, gameInMatrix, gameBackMatrix, game, eyeInInverse, deltaInverse;
    memcpy(eyeInMatrix.m, eyeIn, sizeof(eyeIn));
    memcpy(eyeOutMatrix.m, eyeOut, sizeof(eyeOut));
    memcpy(gameInMatrix.m, gameIn, sizeof(gameIn));
    memcpy(gameBackMatrix.m, gameBack, sizeof(gameBack));
    if (!Invert(eyeInMatrix, eyeInInverse) || !Invert(delta, deltaInverse))
        return false;
    for (int i = 0; i < 4; i++) {
        game.m[i][0] = original[i * 4];
        game.m[i][1] = original[i * 4 + 1];
        game.m[i][2] = 0;
        game.m[i][3] = original[i * 4 + 3];
    }
    const Matrix4 toGame = Multiply(Multiply(eyeInInverse, deltaInverse), gameInMatrix);
    Matrix4 result = Multiply(Multiply(Multiply(Multiply(toGame, game), gameBackMatrix), delta), eyeOutMatrix);
    for (int j = 0; j < 4; j++)
        result.m[3][j] += delta.m[3][0] * eyeOutMatrix.m[0][j] + delta.m[3][1] * eyeOutMatrix.m[1][j] + delta.m[3][2] * eyeOutMatrix.m[2][j];
    for (int i = 0; i < 4; i++) {
        out[i * 4] = (float)result.m[i][0];
        out[i * 4 + 1] = (float)result.m[i][1];
        out[i * 4 + 2] = original[i * 4 + 2];
        out[i * 4 + 3] = (float)result.m[i][3];
    }
    return true;
}

// The same for the blur shaders that hand the matrix the pixel's (u, v, depth, 1) as the depth buffer holds it (PixelShaderInfo::blurDepthRaw: 8 of the game's 26, among them the
// shader it uses at its default settings; at rest its g_mMatrix is the 4x4 identity and the car's motion is in the depth row and the last column).
// The depth buffer holds depth = c + d/Z (Z the distance along the view axis, c and d the projection's depth terms, g_vDepthScale.xy), so (u, v, depth, 1) is, up to a scale of Z,
// the clip space position of the pixel and the game's matrix is a projective map of clip space: the previous place is (x'/w', y'/w') and w' is Z'/Z. All of it is linear in homogeneous
// coordinates, so the whole chain is one matrix:
//   (u, v, depth, 1) of the eye -> the eye camera's (X, Y, Z, W) up to a scale (tangents, 1, 1/Z) -> (inverse of the delta) -> the game camera's -> (Z*u, Z*v, Z*depth, Z) of the game
//   -> the game's matrix -> (x', y', w') -> the point in the previous game camera -> (the delta) -> the previous eye camera -> the eye's (x', y', w').
// The one thing the game's matrix does not give back is the point's W (it has no column for it); a rigid motion keeps it, so it is carried over as it is. The head stays where it is
// between the two frames: only the game camera's motion blurs. At rest the game's matrix is the identity and so is the result.
bool BlurMatrixForEyeRawDepth(const float* original, const EyeBox& box, int eye, const Matrix4& delta, double projX, double projY, double depthC, double depthD, float* out)
{
    const double width = box.right - box.left, height = box.up + box.down, offset = eye ? 0.5 : 0.0;
    if (fabs(width) < 1e-6 || fabs(height) < 1e-6 || fabs(projX) < 1e-6 || fabs(projY) < 1e-6 || fabs(depthD) < 1e-9)
        return false;
    // (u, v, depth, 1) of the eye -> (tan x, tan y, 1, 1/Z): the direction through the pixel, and the inverse distance, which the depth buffer holds linearly (1/Z = (depth - c) / d)
    const double eyeIn[16] = {2 * width, 0, 0, 0, 0, -height, 0, 0, 0, 0, 0, 1 / depthD, box.left - 2 * width * offset, box.up, 1, -depthC / depthD};
    // (X, Y, Z, W) of the game camera -> (Z*u, Z*v, Z*depth, Z) when W is 1
    const double gameIn[16] = {0.5 * projX, 0, 0, 0, 0, -0.5 * projY, 0, 0, 0.5, 0.5, depthC, 1, 0, 0, depthD, 0};
    // (x', y', unused, w') -> (X', Y', Z', 0): the point the game's matrix finds, in the previous game camera
    const double gameBack[16] = {2 / projX, 0, 0, 0, 0, -2 / projY, 0, 0, 0, 0, 0, 0, -1 / projX, 1 / projY, 1, 0};
    // (X, Y, Z, W) of the eye camera -> (Z*u, Z*v, unused, Z), as the shader reads the matrix
    const double eyeOut[16] = {1 / (2 * width), 0, 0, 0, 0, -1 / height, 0, 0, offset - box.left / (2 * width), box.up / height, 0, 1, 0, 0, 0, 0};
    Matrix4 eyeInMatrix, gameInMatrix, gameBackMatrix, eyeOutMatrix, game, deltaInverse;
    memcpy(eyeInMatrix.m, eyeIn, sizeof(eyeIn));
    memcpy(gameInMatrix.m, gameIn, sizeof(gameIn));
    memcpy(gameBackMatrix.m, gameBack, sizeof(gameBack));
    memcpy(eyeOutMatrix.m, eyeOut, sizeof(eyeOut));
    if (!Invert(delta, deltaInverse))
        return false;
    for (int i = 0; i < 4; i++) {
        game.m[i][0] = original[i * 4];
        game.m[i][1] = original[i * 4 + 1];
        game.m[i][2] = 0;
        game.m[i][3] = original[i * 4 + 3];
    }
    Matrix4 previousPoint = Multiply(Multiply(gameInMatrix, game), gameBackMatrix);
    previousPoint.m[3][3] += 1; // (the W of the point as it was)
    const Matrix4 result = Multiply(Multiply(Multiply(Multiply(eyeInMatrix, deltaInverse), previousPoint), delta), eyeOutMatrix);
    for (int i = 0; i < 4; i++) {
        out[i * 4] = (float)result.m[i][0];
        out[i * 4 + 1] = (float)result.m[i][1];
        out[i * 4 + 2] = original[i * 4 + 2];
        out[i * 4 + 3] = (float)result.m[i][3];
    }
    return true;
}

HRESULT MotionBlurPerEye(IDirect3DDevice9* device, const DrawCall& draw)
{
    if (!g_cfg.motionBlur) {
        HRESULT copied = D3D_OK;
        if (DrawMotionBlurStill(device, draw, &copied))
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
    const bool rawDepth = g_currentPsInfo && g_currentPsInfo->blurDepthRaw;
    const float* depthTerms = &g_psConstants[BlurConstantRegister("g_vDepthScale", 4) * 4]; // c and d of the game's projection: depth = c + d/Z
    setRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE);
    for (int eye = 0; eye < 2; eye++) {
        float newConstants[16];
        const bool made = rawDepth ? BlurMatrixForEyeRawDepth(originalConstants, g_vr.eyeBox[eye], eye, g_vr.eyeCameraDelta[eye], g_projXScale, g_projYScale, depthTerms[0], depthTerms[1], newConstants)
                                   : BlurMatrixForEye(originalConstants, g_vr.eyeBox[eye], eye, g_vr.eyeCameraDelta[eye], g_projXScale, g_projYScale, newConstants);
        if (!made) { // (a degenerate eye: the game's own matrix, the eye is still drawn)
            static int reported = 0;
            if (reported++ < 4)
                Log("motion blur: eye %d: no matrix could be made (tangents l %.3f r %.3f up %.3f down %.3f, projection scales %.4f %.4f): the game's own matrix is used", eye, g_vr.eyeBox[eye].left,
                    g_vr.eyeBox[eye].right, g_vr.eyeBox[eye].up, g_vr.eyeBox[eye].down, g_projXScale, g_projYScale);
            memcpy(newConstants, originalConstants, sizeof(newConstants));
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
