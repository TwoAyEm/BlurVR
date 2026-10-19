// Shader bookkeeping: constant tables of every vertex/pixel shader the game creates, the constants it uploads, and the shaders currently bound.
#include "shader_tracking.h"

std::unordered_map<void*, PixelShaderInfo*> g_pixelShaders;
PixelShaderInfo* g_currentPsInfo = NULL;
float g_psConstants[256 * 4];
int g_pixelShaderCount = 0;

std::unordered_map<void*, VertexShaderInfo*> g_vertexShaders;
VertexShaderInfo* g_currentVsInfo;
void* g_currentVsHandle = NULL;

int g_vertexShaderCount;

bool NamesEqual(const char* a, const char* b)
{
    return _stricmp(a, b) == 0;
}

void ParseVertexShaderConstants(const DWORD* fn, VertexShaderInfo* info, int id)
{
    info->id = id;
    const DWORD* token = fn + 1; // skip version token
    while ((*token & 0xFFFF) == 0xFFFE) {
        DWORD length = (*token >> 16) & 0x7FFF;
        if (length >= 2 && token[1] == 0x42415443 /*'CTAB'*/) {
            const BYTE* base = (const BYTE*)(token + 2);
            const BYTE* end = (const BYTE*)(token + 1 + length);
            DWORD constantCount = *(const DWORD*)(base + 12);
            DWORD infoOffset = *(const DWORD*)(base + 16);
            UINT viewMatrixRegs[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
                 viewProjRegs[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            for (DWORD i = 0; i < constantCount && i < 256; i++) {
                const BYTE* entry = base + infoOffset + i * 20;
                if (entry + 20 > end)
                    break;
                DWORD nameOffset = *(const DWORD*)(entry + 0);
                WORD regSet = *(const WORD*)(entry + 4), regIndex = *(const WORD*)(entry + 6),
                     regCount = *(const WORD*)(entry + 8);
                if (base + nameOffset >= end)
                    continue;
                const char* name = (const char*)(base + nameOffset);
                if (regSet != 2 /*D3DXRS_FLOAT4*/)
                    continue;
                {
                    ShaderConstant constant;
                    constant.name = name;
                    constant.reg = regIndex;
                    constant.count = regCount;
                    constant.regSet = regSet;
                    info->constants.push_back(constant);
                }
                bool matched = false;
                if (regCount == 4) {
                    TrackedKind kind = TK_WorldViewProj;
                    if (NamesEqual(name, "g_mWorldViewProj")) {
                        kind = TK_WorldViewProj;
                        matched = true;
                    } else if (NamesEqual(name, "g_mViewProj") || NamesEqual(name, "g_ViewProjMatrix")) {
                        kind = TK_ViewProj;
                        matched = true;
                    } else if (NamesEqual(name, "g_mWorldView")) {
                        kind = TK_WorldView;
                        matched = true;
                    } else if (NamesEqual(name, "g_mView")) {
                        kind = TK_View;
                        matched = true;
                    } else if (NamesEqual(name, "g_mProj")) {
                        kind = TK_Projection;
                        matched = true;
                    }
                    if (matched)
                        info->tracked.push_back({kind, regIndex});
                } else if (regCount == 3 && NamesEqual(name, "g_mView")) {
                    info->tracked.push_back({TK_View3x3, regIndex});
                    matched = true;
                }
                size_t nameLength = strlen(name);
                if (nameLength > 3 && name[nameLength - 1] >= '0' && name[nameLength - 1] <= '3' &&
                    regCount == 1) { // g_vViewMatrix0..3 etc.
                    int index = name[nameLength - 1] - '0';
                    std::string stem(name, nameLength - 1);
                    if (NamesEqual(stem.c_str(), "g_vViewMatrix")) {
                        viewMatrixRegs[index] = regIndex;
                        matched = true;
                    } else if (NamesEqual(stem.c_str(), "g_vViewProjMatrix")) {
                        viewProjRegs[index] = regIndex;
                        matched = true;
                    } else if (NamesEqual(stem.c_str(), "g_ReflectionViewMatrix")) {
                        info->reflectionRegs[index] = regIndex;
                    }
                }
                if (!matched && regSet == 2 && info->otherConstantNames.size() < 160) {
                    info->otherConstantNames += name;
                    info->otherConstantNames += ",";
                }
            }
            auto addGroup = [&](UINT* regs, TrackedKind kind) {
                if (regs[0] != 0xFFFF && regs[1] == regs[0] + 1 && regs[2] == regs[0] + 2 && regs[3] == regs[0] + 3)
                    info->tracked.push_back({kind, regs[0]});
            };
            addGroup(viewMatrixRegs, TK_View);
            addGroup(viewProjRegs, TK_ViewProj);
            info->ui = info->otherConstantNames.find("g_Ui") != std::string::npos;
            return;
        }
        token += 1 + length;
    }
}

float g_vsConstants[256 * 4]; // what the game believes is in the VS float constants

void ParsePixelShaderConstants(const DWORD* fn, PixelShaderInfo* info)
{
    // whole token stream (comment blocks are skipped by their length field, instructions by theirs)
    const DWORD* token = fn + 1;
    for (int guard = 0; guard < 20000; guard++) {
        DWORD tokenWord = *token;
        if (tokenWord == 0x0000FFFF) {
            token++;
            break;
        }
        if ((tokenWord & 0xFFFF) == 0xFFFE) {
            DWORD length = (tokenWord >> 16) & 0x7FFF;
            if (length >= 2 && token[1] == 0x42415443) {
                const BYTE* base = (const BYTE*)(token + 2);
                const BYTE* end = (const BYTE*)(token + 1 + length);
                DWORD constantCount = *(const DWORD*)(base + 12), infoOffset = *(const DWORD*)(base + 16);
                for (DWORD i = 0; i < constantCount && i < 128; i++) {
                    const BYTE* entry = base + infoOffset + i * 20;
                    if (entry + 20 > end)
                        break;
                    DWORD nameOffset = *(const DWORD*)(entry + 0);
                    if (base + nameOffset >= end)
                        continue;
                    ShaderConstant constant;
                    constant.name = (const char*)(base + nameOffset);
                    constant.regSet = *(const WORD*)(entry + 4);
                    constant.reg = *(const WORD*)(entry + 6);
                    constant.count = *(const WORD*)(entry + 8);
                    info->constants.push_back(constant);
                }
            }
            token += 1 + length;
        } else
            token += 1 + ((tokenWord >> 24) & 0xF);
    }
    bool hasMatrix = false, hasStepScale = false;
    for (const ShaderConstant& constant : info->constants) {
        if (NamesEqual(constant.name.c_str(), "g_mMatrix"))
            hasMatrix = true;
        if (NamesEqual(constant.name.c_str(), "g_vStepScale"))
            hasStepScale = true;
    }
    info->motionBlur = hasMatrix && hasStepScale;
    for (const ShaderConstant& constant : info->constants) {
        if (constant.reg >= 250)
            continue;
        if (NamesEqual(constant.name.c_str(), "g_ShadowPlaneSpaceMatrix"))
            info->shadowMatrixReg = constant.reg;
        else if (NamesEqual(constant.name.c_str(), "g_vPosition") && constant.count >= 1 && constant.count <= 8) {
            info->lightPosReg = constant.reg;
            info->lightPosCount = constant.count;
        } else if (NamesEqual(constant.name.c_str(), "g_vViewUpVector"))
            info->viewUpReg = constant.reg;
        else if (NamesEqual(constant.name.c_str(), "g_vXY"))
            info->viewRayScaleReg = constant.reg;
        else if (NamesEqual(constant.name.c_str(), "g_mLight"))
            info->lightMatrixReg = constant.reg;
    }
}
