// Shader bookkeeping: constant tables of every vertex/pixel shader the game creates, the constants it uploads, and the
// shaders currently bound.
#pragma once
#include "common.h"

struct ShaderConstant {
    std::string name;
    UINT reg, count;
    WORD regSet;
};
struct PixelShaderInfo {
    int id = 0;
    int shaderMajor = 0; // 2 for ps_2_0, 3 for ps_3_0: a pixel shader that stands in for this one must be of the same model (it is paired with the same vertex shader)
    std::vector<ShaderConstant> constants;
    bool motionBlur = false;
    int shadowMatrixReg = -1;
    int lightPosReg = -1, lightPosCount = 0, viewUpReg = -1, viewRayScaleReg = -1, lightMatrixReg = -1;
    // The game's camera as the main view hands it to pixel shaders (GameWorld/View/Main.cpp): its world position
    // (g_vViewPositionPS, and g_vViewPosition in the vehicle and fog shaders), and the view and view-projection matrices
    // one column per register (g_vViewMatrix0..3, g_vViewProjMatrix0..3; the first register of the four, -1 = not used).
    int viewPositionRegs[2] = {-1, -1};
    int viewMatrixBase = -1, viewProjBase = -1;
    unsigned viewMatrixUsed = 0, viewProjUsed = 0; // which of the four registers the shader declares (bit n = ...Matrix<n>)
    bool eyeConstants = false; // any of the above: the per-eye draws rewrite them (ApplyEyePixelConstants)
};

enum TrackedKind { TK_WorldViewProj, TK_ViewProj, TK_WorldView, TK_View, TK_Projection, TK_View3x3 };
struct TrackedConstant {
    TrackedKind kind;
    UINT reg;
};
struct VertexShaderInfo {
    std::vector<TrackedConstant> tracked;
    int id = 0;
    std::string otherConstantNames;
    bool ui = false; // otherConstantNames holds a g_Ui... constant: a 2D user-interface shader
    UINT reflectionRegs[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    UINT viewPositionReg = 0xFFFF; // g_vViewPosition / g_vCameraPosition: the camera's world position (view vectors, billboards)
    std::vector<ShaderConstant> constants;
};

// Every pixel shader the game has created.
extern std::unordered_map<void*, PixelShaderInfo*> g_pixelShaders;
// Info for the pixel shader that is currently bound (NULL if unknown).
extern PixelShaderInfo* g_currentPsInfo;
// The pixel shader that is currently bound, as the game set it (NULL if none or unknown).
extern void* g_currentPsHandle;
// Shadow copy of the pixel shader float constants.
extern float g_psConstants[256 * 4];
extern int g_pixelShaderCount;
// Every vertex shader the game has created.
extern std::unordered_map<void*, VertexShaderInfo*> g_vertexShaders;
// Info for the vertex shader that is currently bound (NULL if unknown).
extern VertexShaderInfo* g_currentVsInfo;
extern void* g_currentVsHandle;
extern int g_vertexShaderCount;
// Case-sensitive comparison of constant-table names.
bool NamesEqual(const char* a, const char* b);
// Read the constant table (CTAB) of a vertex shader and note which registers hold the world/view/projection matrices.
void ParseVertexShaderConstants(const DWORD* fn, VertexShaderInfo* info, int id);
// Shadow copy of the vertex shader float constants the game has uploaded (256 registers x 4).
extern float g_vsConstants[256 * 4];
// Read the constant table of a pixel shader.
void ParsePixelShaderConstants(const DWORD* fn, PixelShaderInfo* info);
