// Finding the game's camera hand-over functions in the code of Blur.exe by what they look like (NOTES.md section 25).
#include "game_signatures.h"
#include <stdio.h>
#include <string.h>

namespace {

// A byte pattern as hex text with "??" for a wildcard.  The patterns are the first instructions of the two functions as they are in the
// Steam build; the loose form of each also lets every displacement and immediate vary, so a build whose class layout moved still matches
// (the frustum layout is checked separately, at run time, before anything is changed).
struct PatternSource {
    const char* text;
    int immOffset; // offset of an imm32 that must be read from the code (the struct size in dwords), or -1
};

// SetCameraData: push ebp / mov ebp,esp / and esp,-16 / sub esp,0x18 / push esi / push edi / mov edi,[ebp+8] / mov esi,ecx /
// push edi / lea ecx,[esi+0x2e0] / call <copy T> / movss xmm0,[edi+0x48] / movss xmm1,[<1.0>] / movss [esp+0x10],xmm0 / ...
const PatternSource kSetCameraData[2] = {
    {"55 8B EC 83 E4 F0 83 EC 18 56 57 8B 7D 08 8B F1 57 8D 8E E0 02 00 00 E8 ?? ?? ?? ?? F3 0F 10 47 48 F3 0F 10 0D ?? ?? ?? ?? F3 0F 11 44 "
     "24 10 F3 0F 10 47 58 F3 0F 11 44 24 14 F3 0F 10 47 68 F3 0F 11 44 24 18 F3 0F 10 47 78",
     -1},
    {"55 8B EC 83 E4 F0 83 EC 18 56 57 8B 7D ?? 8B F1 57 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 47 ?? F3 0F 10 0D ?? ?? ?? ?? F3 0F 11 44 "
     "24 ?? F3 0F 10 47 ?? F3 0F 11 44 24 ?? F3 0F 10 47 ?? F3 0F 11 44 24 ?? F3 0F 10 47 ??",
     -1}};

// SetCamera: mov eax,ecx / push ebx / mov ebx,[esp+8] / push esi / push edi / lea edx,[eax+0x30] / mov esi,ebx / mov ecx,0xAC /
// mov edi,edx / rep movsd / movq xmm0,[ebx+0x10] / lea ecx,[ebx+0x10] / movq [eax+0x6c0],xmm0 / ...
const PatternSource kSetCamera[2] = {
    {"8B C1 53 8B 5C 24 08 56 57 8D 50 30 8B F3 B9 AC 00 00 00 8B FA F3 A5 F3 0F 7E 43 10 8D 4B 10 66 0F D6 80 C0 06 00 00 F3 0F 7E 41 08 8B 4C 24 "
     "14 5F 66 0F D6 80 C8 06 00 00 F3 0F 10 05 ?? ?? ?? ??",
     15},
    {"8B C1 53 8B 5C 24 ?? 56 57 8D 50 ?? 8B F3 B9 ?? ?? ?? ?? 8B FA F3 A5 F3 0F 7E 43 ?? 8D 4B ?? 66 0F D6 80 ?? ?? ?? ?? F3 0F 7E 41 ?? 8B 4C "
     "24 ?? 5F 66 0F D6 80 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ??",
     15}};

const size_t kMaxPattern = 96;

struct Pattern {
    unsigned char bytes[kMaxPattern], mask[kMaxPattern]; // mask 0xFF = must match
    size_t length;
    int immOffset;
};

Pattern g_patterns[2][2];
bool g_parsed = false;

int HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

void Parse(const PatternSource& source, Pattern& out)
{
    out.length = 0;
    out.immOffset = source.immOffset;
    for (const char* p = source.text; *p && out.length < kMaxPattern;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (p[0] == '?' && p[1] == '?') {
            out.bytes[out.length] = 0;
            out.mask[out.length++] = 0;
        } else {
            out.bytes[out.length] = (unsigned char)(HexValue(p[0]) * 16 + HexValue(p[1]));
            out.mask[out.length++] = 0xFF;
        }
        p += 2;
    }
}

void ParseAll()
{
    if (g_parsed)
        return;
    for (int form = 0; form < 2; form++) {
        Parse(kSetCameraData[form], g_patterns[0][form]);
        Parse(kSetCamera[form], g_patterns[1][form]);
    }
    g_parsed = true;
}

bool Matches(const unsigned char* at, const Pattern& pattern)
{
    for (size_t i = 0; i < pattern.length; i++)
        if ((at[i] & pattern.mask[i]) != pattern.bytes[i])
            return false;
    return true;
}

unsigned ReadU32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

} // namespace

void InitGameCameraScan(GameCameraScan& scan)
{
    memset(&scan, 0, sizeof(scan));
}

void ScanForGameCameraCode(const unsigned char* code, size_t size, GameCameraScan& scan)
{
    ParseAll();
    for (int fn = 0; fn < 2; fn++) {
        // the loose form matches wherever the tight one does, so the loose pattern is the one searched; the tight one is checked on a hit
        const Pattern& loose = g_patterns[fn][1];
        const Pattern& tight = g_patterns[fn][0];
        if (size < loose.length)
            continue;
        unsigned char first = loose.bytes[0]; // (the first byte of both patterns is fixed)
        for (size_t i = 0; i + loose.length <= size; i++) {
            if (code[i] != first || !Matches(code + i, loose))
                continue;
            bool isTight = Matches(code + i, tight);
            if (!scan.count[fn][1]++)
                scan.first[fn][1] = code + i;
            if (isTight && !scan.count[fn][0]++)
                scan.first[fn][0] = code + i;
        }
    }
}

bool ResolveGameCameraCode(const GameCameraScan& scan, GameCameraCode& out, char* why, size_t whySize)
{
    ParseAll();
    memset(&out, 0, sizeof(out));
    const unsigned char* found[2] = {NULL, NULL};
    int form[2] = {0, 0};
    char text[2][96];
    const char* names[2] = {"SetCameraData", "SetCamera"};
    for (int fn = 0; fn < 2; fn++) {
        unsigned tight = scan.count[fn][0], loose = scan.count[fn][1];
        if (tight == 1) {
            found[fn] = scan.first[fn][0];
            form[fn] = 0;
        } else if (tight == 0 && loose == 1) {
            found[fn] = scan.first[fn][1];
            form[fn] = 1;
        }
        snprintf(text[fn], sizeof(text[fn]), "%s: %u tight / %u loose match%s", names[fn], tight, loose, found[fn] ? "" : " (not usable)");
    }
    if (why && whySize)
        snprintf(why, whySize, "%s; %s", text[0], text[1]);
    if (!found[0] || !found[1])
        return false;
    out.setCameraData = found[0];
    out.setCamera = found[1];
    out.dataForm = form[0];
    out.cameraForm = form[1];
    // stolen bytes: SetCameraData starts with push ebp / mov ebp,esp / and esp,-16 / sub esp,0x18 (9 bytes), SetCamera with mov eax,ecx /
    // push ebx / mov ebx,[esp+8] (7 bytes); none has a relative operand, and they are whole instructions in both forms
    out.setCameraDataStolen = 9;
    out.setCameraStolen = 7;
    if (out.setCameraDataStolen > g_patterns[0][1].length || out.setCameraStolen > g_patterns[1][1].length)
        return false;
    const Pattern& cameraPattern = g_patterns[1][1];
    if (cameraPattern.immOffset >= 0) {
        unsigned dwords = ReadU32(found[1] + cameraPattern.immOffset);
        out.cameraBytes = (size_t)dwords * 4;
    }
    if (out.cameraBytes < 0x2B0 || out.cameraBytes > 0x1000) {
        if (why && whySize)
            snprintf(why, whySize, "%s; %s; the camera description is 0x%x bytes there (expected 0x2b0 or a little more)", text[0], text[1],
                     (unsigned)out.cameraBytes);
        return false;
    }
    return true;
}
