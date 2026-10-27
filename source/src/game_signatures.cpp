// Finding the game's camera hand-over functions in the code of Blur.exe by what they look like (NOTES.md section 25).
#include "game_signatures.h"
#include <stdint.h>
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

// SetCamera again, for a build in which the function was rewritten and moved (an obfuscating build rewrites some functions: jumps get longer, calls turn into
// push / push / ret, constants are loaded from a table, junk instructions are put in; the instructions with registers and memory operands stay as they were).
// These are the function's instructions without an immediate or an absolute address, in order: each must be found within kStepGap bytes of the one before
// it, the first within kStepGap bytes of the start.  The size of the description is the `mov ecx, <dwords>` between the 4th and the 5th (or the same
// constant loaded from a table: mov ecx, [table entry]).
const int kSetCameraStepCount = 14;
const size_t kStepGap = 64;
const char* const kSetCameraSteps[kSetCameraStepCount] = {
    "8B C1",                     // mov eax,ecx
    "8B 5C 24 08",               // mov ebx,[esp+8]
    "8D 50 30",                  // lea edx,[eax+0x30]
    "8B F3",                     // mov esi,ebx
    "8B FA",                     // mov edi,edx        (the size is loaded between the two: mov ecx,0xAC)
    "F3 A5",                     // rep movsd
    "F3 0F 7E 43 10",            // movq xmm0,[ebx+0x10]
    "8D 4B 10",                  // lea ecx,[ebx+0x10]
    "66 0F D6 80 C0 06 00 00",   // movq [eax+0x6C0],xmm0
    "F3 0F 7E 41 08",            // movq xmm0,[ecx+8]
    "8B 4C 24 14",               // mov ecx,[esp+0x14]
    "66 0F D6 80 C8 06 00 00",   // movq [eax+0x6C8],xmm0
    "F3 0F 11 80 DC 06 00 00",   // movss [eax+0x6DC],xmm0
    "89 88 F4 06 00 00"};        // mov [eax+0x6F4],ecx

// UIAiHud's per-frame function (vtable slot +0x30 of the class at 0x00EEB2E0 in the Steam build, function at 0x00C54180): push ebp / mov ebp,esp /
// and esp,-16 / sub esp,0x184 / push ebx / push esi / push edi / mov edi,ecx / cmp byte [edi+0x144],0 / je / cmp byte [edi+0x190],0 / jne /
// cmp dword [edi+0x1AC],-1 / je / call <view> / mov ebx,eax / mov [esp+0x24],ebx.  Matches once in the exe (checked on a dump of the Steam build).
const PatternSource kAiHudDraw = {
    "55 8B EC 83 E4 F0 81 EC 84 01 00 00 53 56 57 8B F9 80 BF 44 01 00 00 00 0F 84 ?? ?? ?? ?? 80 BF 90 01 00 00 00 0F 85 ?? ?? ?? ?? 83 BF AC 01 00 "
    "00 FF 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B D8 89 5C 24 24",
    -1};
// Two places inside that function, at fixed offsets from its start, checked against the bytes around them (call and jump targets are wildcards):
//  +0x3FA ... the plane test of a car: xorps / comiss / mov [esp+0x38],eax / ja <skip> / xor ecx,ecx / mov edx,0x40000 / mov eax,[esi+ecx*4]  (the ja is at +0x404)
//  +0x68B ... the store of the root's world position and the calls that draw the targeting elements: movss [eax+8],xmm0 / mov ecx,[esi+0x14] / test ecx,ecx / je / ...  (the
//             instructions after the store, at +0x690, are the 5 bytes a detour can take)
const PatternSource kAiHudPlane = {"0F 57 C9 0F 2F C8 89 44 24 38 0F 87 ?? ?? ?? ?? 33 C9 BA 00 00 04 00 8B 04 8E", -1};
//  +0x6EF ... the choice of the marker's yaw: cmp byte [UI.HUD.BillboardToCarInsteadOfCamera],0 / je <the camera's yaw> / mov ecx,[esp+0x1C] / cmp dword [ecx+0xE0],5 / je ... / mov eax,[esp+0x38]
//             (the flag's address is the operand of the first instruction)
const PatternSource kAiHudBillboard = {"80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 8B 4C 24 1C 83 B9 E0 00 00 00 05 0F 84 ?? ?? ?? ?? 8B 44 24 38", -1};
const PatternSource kAiHudRoot = {"F3 0F 11 40 08 8B 4E 14 85 C9 74 07 8B 01 8B 50 28 FF D2 8B 4E 18 85 C9 74 07 8B 01 8B 50 28 FF D2 8B 4E 1C", -1};
// HudDummy's update (the HUD's root gadget; UI.Viewport.* tweakables): sub esp,0x28 / push esi / mov esi,ecx / cmp byte [esi+0x144],0 / je / mov ecx,[esi+0x1A0] /
// test ecx,ecx / je / mov eax,[ecx] / movss xmm0,[...] / mulss xmm0,[...] / mov edx,[eax+0x4C] / movss [esp+4],xmm0.  Its end, at +0x297, is checked too: the stores of
// the gadget's position (+0x70, +0x74 from +0x190, +0x194), rotation (+0x84 from +0x19C) and scale (+0x88, +0x8C from +0x198), which is what a hook puts back to rest.
const PatternSource kViewportStart = {"83 EC 28 56 8B F1 80 BE 44 01 00 00 00 0F 84 ?? ?? ?? ?? 8B 8E A0 01 00 00 85 C9 0F 84 ?? ?? ?? ?? 8B 01 F3 0F 10 05 ?? ?? ?? ?? F3 0F 59 05 ?? ?? ?? ?? 8B 50 4C F3 0F 11 44 24 04", -1};
const PatternSource kViewportEnd = {"0F 28 EC F3 0F 59 E8 F3 0F 58 AE 90 01 00 00 F3 0F 10 86 70 01 00 00 F3 0F 11 6E 70 0F 28 E8 F3 0F 59 EC F3 0F 59 EA F3 0F 58 AE 94 01 00 00 F3 0F 11 96 84 01 00 00 F3 0F 10 96 98 01 00 00", -1};
const size_t kViewportEndAt = 0x297, kViewportStolen = 6; // (sub esp,0x28 / push esi / mov esi,ecx)
const size_t kAiHudPlaneAt = 0x3FA, kAiHudPlaneJump = 0x404, kAiHudRootAt = 0x68B, kAiHudRootStore = 0x690, kAiHudBillboardAt = 0x6EF;
const size_t kAiHudDrawStolen = 12; // push ebp / mov ebp,esp / and esp,-16 / sub esp,imm32: whole instructions, no relative operand

const size_t kMaxPattern = 96;

struct Pattern {
    unsigned char bytes[kMaxPattern], mask[kMaxPattern]; // mask 0xFF = must match
    size_t length;
    int immOffset;
};

Pattern g_patterns[2][2];
Pattern g_cameraSteps[kSetCameraStepCount];
Pattern g_aiHudPattern, g_aiHudPlanePattern, g_aiHudRootPattern, g_aiHudBillboardPattern, g_viewportStart, g_viewportEnd;
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
    for (int step = 0; step < kSetCameraStepCount; step++) {
        const PatternSource source = {kSetCameraSteps[step], -1};
        Parse(source, g_cameraSteps[step]);
    }
    Parse(kAiHudDraw, g_aiHudPattern);
    Parse(kAiHudPlane, g_aiHudPlanePattern);
    Parse(kAiHudRoot, g_aiHudRootPattern);
    Parse(kAiHudBillboard, g_aiHudBillboardPattern);
    Parse(kViewportStart, g_viewportStart);
    Parse(kViewportEnd, g_viewportEnd);
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

void ScanForGameCameraCode(const unsigned char* code, size_t size, GameCameraScan& scan, unsigned long long va)
{
    ParseAll();
    if (scan.blockCount < kMaxScanBlocks) {
        GameCameraScan::Block& block = scan.blocks[scan.blockCount++];
        block.code = code;
        block.size = size;
        block.va = va ? va : (unsigned long long)(uintptr_t)code;
    }
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

// ---- SetCamera through the calls to SetCameraData -----------------------------------------------------------------------------------------------------
namespace {

// The bytes of the scanned code at an address of the game's memory (`need` of them), or NULL.
const unsigned char* BytesAt(const GameCameraScan& scan, unsigned long long va, size_t need)
{
    for (unsigned i = 0; i < scan.blockCount; i++) {
        const GameCameraScan::Block& block = scan.blocks[i];
        if (va >= block.va && va - block.va + need <= block.size)
            return block.code + (size_t)(va - block.va);
    }
    return NULL;
}

// How much code can be read from an address on (to the end of its block).
size_t BytesFrom(const GameCameraScan& scan, unsigned long long va)
{
    for (unsigned i = 0; i < scan.blockCount; i++) {
        const GameCameraScan::Block& block = scan.blocks[i];
        if (va >= block.va && va - block.va < block.size)
            return block.size - (size_t)(va - block.va);
    }
    return 0;
}

bool VaOf(const GameCameraScan& scan, const unsigned char* p, unsigned long long& va)
{
    for (unsigned i = 0; i < scan.blockCount; i++) {
        const GameCameraScan::Block& block = scan.blocks[i];
        if (p >= block.code && p < block.code + block.size) {
            va = block.va + (size_t)(p - block.code);
            return true;
        }
    }
    return false;
}

// A call as the game's code and its obfuscated form make it: call rel32, or push <target> / ret (the push of the return address comes before).
bool CallTargetAt(const unsigned char* p, size_t left, unsigned long long va, unsigned& target)
{
    if (left >= 5 && p[0] == 0xE8) {
        target = (unsigned)(va + 5) + ReadU32(p + 1);
        return true;
    }
    if (left >= 6 && p[0] == 0x68 && p[5] == 0xC3) {
        target = ReadU32(p + 1);
        return true;
    }
    return false;
}

void HexBytes(char* out, size_t outSize, const unsigned char* p, size_t n)
{
    size_t used = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && used + 4 < outSize; i++)
        used += (size_t)snprintf(out + used, outSize - used, "%s%02X", i ? " " : "", p[i]);
}

// Is this SetCamera?  The steps of kSetCameraSteps in order.  On a pass `cameraBytes` is the size of the description it copies; on a failure `failedStep` is
// the step that was not found (14 = all found, the size was not).
bool LooksLikeSetCamera(const GameCameraScan& scan, const unsigned char* body, size_t available, size_t& cameraBytes, int& failedStep)
{
    size_t start[kSetCameraStepCount], end[kSetCameraStepCount], pos = 0;
    for (int step = 0; step < kSetCameraStepCount; step++) {
        const Pattern& pattern = g_cameraSteps[step];
        bool found = false;
        for (size_t at = pos; at <= pos + kStepGap && at + pattern.length <= available; at++)
            if (body[at] == pattern.bytes[0] && Matches(body + at, pattern)) {
                start[step] = at;
                end[step] = at + pattern.length;
                pos = end[step];
                found = true;
                break;
            }
        if (!found) {
            failedStep = step;
            return false;
        }
    }
    failedStep = kSetCameraStepCount;
    for (size_t at = end[3]; at + 5 <= start[4]; at++) {
        unsigned dwords = 0;
        if (body[at] == 0xB9) {
            dwords = ReadU32(body + at + 1);
        } else if (body[at] == 0x8B && body[at + 1] == 0x0D && at + 6 <= start[4]) { // the constant loaded from a table
            const unsigned char* entry = BytesAt(scan, ReadU32(body + at + 2), 4);
            if (entry)
                dwords = ReadU32(entry);
        }
        if (dwords >= 0xAC && dwords <= 0x400) {
            cameraBytes = (size_t)dwords * 4;
            return true;
        }
    }
    return false;
}

} // namespace

bool FindSetCameraThroughCallSites(const GameCameraScan& scan, const unsigned char* setCameraData, GameCameraCode& out, char* note, size_t noteSize)
{
    ParseAll();
    memset(&out, 0, sizeof(out));
    if (note && noteSize)
        note[0] = 0;
    unsigned long long dataVa = 0;
    if (!VaOf(scan, setCameraData, dataVa)) {
        if (note && noteSize)
            snprintf(note, noteSize, "no call-site search: SetCameraData is not in a scanned block");
        return false;
    }
    const unsigned dataTarget = (unsigned)dataVa;
    // The calls to SetCameraData, and the calls within kWindow bytes of each.  SetCamera is the function that is called next to (nearly) all of them.
    const size_t kWindow = 48;
    const unsigned kMaxSites = 64, kMaxVotes = 96;
    unsigned sites = 0, voteCount = 0;
    struct Vote {
        unsigned target, sites;
    } votes[kMaxVotes];
    for (unsigned b = 0; b < scan.blockCount; b++) {
        const GameCameraScan::Block& block = scan.blocks[b];
        for (size_t i = 0; i < block.size && sites < kMaxSites; i++) {
            if (block.code[i] != 0xE8 && block.code[i] != 0x68)
                continue;
            unsigned target;
            if (!CallTargetAt(block.code + i, block.size - i, block.va + i, target) || target != dataTarget)
                continue;
            sites++;
            unsigned seen[32], seenCount = 0;
            const size_t from = i > kWindow ? i - kWindow : 0, to = i + kWindow < block.size ? i + kWindow : block.size;
            for (size_t j = from; j < to; j++) {
                unsigned other;
                if (j == i || (block.code[j] != 0xE8 && block.code[j] != 0x68) || !CallTargetAt(block.code + j, block.size - j, block.va + j, other) ||
                    other == dataTarget)
                    continue;
                bool again = false;
                for (unsigned k = 0; k < seenCount; k++)
                    again = again || seen[k] == other;
                if (again || seenCount >= 32)
                    continue;
                seen[seenCount++] = other;
                unsigned v = 0;
                while (v < voteCount && votes[v].target != other)
                    v++;
                if (v == voteCount && voteCount < kMaxVotes) {
                    votes[voteCount].target = other;
                    votes[voteCount++].sites = 0;
                }
                if (v < voteCount)
                    votes[v].sites++;
            }
        }
    }
    if (!sites) {
        if (note && noteSize)
            snprintf(note, noteSize, "no call-site search: no call to SetCameraData was found");
        return false;
    }
    unsigned best = 0, second = 0;
    for (unsigned v = 0; v < voteCount; v++) {
        if (votes[v].sites > votes[best].sites)
            best = v;
    }
    for (unsigned v = 0; v < voteCount; v++)
        if (v != best && votes[v].sites > second)
            second = votes[v].sites;
    char head[160] = "";
    if (voteCount)
        snprintf(head, sizeof(head), "call-site search: SetCameraData is called from %u places, the call next to them is %08X in %u of them (next best %u)", sites,
                 votes[best].target, votes[best].sites, second);
    else
        snprintf(head, sizeof(head), "call-site search: SetCameraData is called from %u places, no other call next to them", sites);
    if (!voteCount || votes[best].sites < 2 || votes[best].sites * 5 < sites * 3 || votes[best].sites <= second) {
        if (note && noteSize)
            snprintf(note, noteSize, "%s: not clear enough", head);
        return false;
    }
    // What is at that address: the function, or the stub the game's calls go through to a body that was moved.
    const unsigned entry = votes[best].target;
    const size_t entryBytes = BytesFrom(scan, entry);
    const unsigned char* at = BytesAt(scan, entry, entryBytes < 8 ? entryBytes : 8);
    char entryHex[64] = "", bodyHex[100] = "";
    if (!at || entryBytes < 8) {
        if (note && noteSize)
            snprintf(note, noteSize, "%s; that address is not in the scanned code", head);
        return false;
    }
    HexBytes(entryHex, sizeof(entryHex), at, 8);
    int stub = 0;
    const unsigned char* operand = NULL;
    unsigned body = entry;
    if (at[0] == 0xFF && at[1] == 0x25) { // jmp dword ptr [slot]
        const unsigned char* slot = BytesAt(scan, ReadU32(at + 2), 4);
        if (slot) {
            stub = 1;
            operand = slot;
            body = ReadU32(slot);
        }
    } else if (at[0] == 0xE9) { // jmp rel32
        stub = 2;
        operand = at + 1;
        body = (unsigned)(entry + 5) + ReadU32(at + 1);
    } else if (at[0] == 0x68 && at[5] == 0xC3) { // push imm32 / ret
        stub = 3;
        operand = at + 1;
        body = ReadU32(at + 1);
    }
    const size_t bodyBytes = BytesFrom(scan, body);
    const size_t look = bodyBytes < 0x400 ? bodyBytes : 0x400;
    const unsigned char* code = look >= 32 ? BytesAt(scan, body, look) : NULL;
    if (!code) {
        if (note && noteSize)
            snprintf(note, noteSize, "%s; its first bytes %s, %s%08X is not in the scanned code", head, entryHex, stub ? "the body at " : "", body);
        return false;
    }
    HexBytes(bodyHex, sizeof(bodyHex), code, 24);
    size_t cameraBytes = 0;
    int failed = 0;
    if (!LooksLikeSetCamera(scan, code, look, cameraBytes, failed)) {
        if (note && noteSize)
            snprintf(note, noteSize, "%s; its first bytes %s%s%s; not SetCamera (%s)", head, entryHex, stub ? ", the body's " : "", stub ? bodyHex : "",
                     failed >= kSetCameraStepCount ? "the size of the description is not there" : "an instruction of it is not in place");
        if (note && noteSize && failed < kSetCameraStepCount) {
            size_t used = strlen(note);
            snprintf(note + used, noteSize - used, ": step %d of %d, \"%s\"", failed + 1, kSetCameraStepCount, kSetCameraSteps[failed]);
        }
        return false;
    }
    // The hook.  A stub is rewritten; the function itself needs its first bytes to be the ones the inline hook steals.
    out.setCamera = at;
    out.cameraBytes = cameraBytes;
    out.cameraForm = 2;
    out.cameraStub = stub;
    if (stub) {
        out.cameraOperand = operand;
        out.cameraBody = body;
    } else {
        const Pattern& prologue = g_patterns[1][0];
        if (look < 7 || memcmp(code, prologue.bytes, 7) != 0) {
            if (note && noteSize)
                snprintf(note, noteSize, "%s; SetCamera is at %08X but its first bytes (%s) are not the ones a hook can take", head, entry, bodyHex);
            return false;
        }
        out.setCameraStolen = 7;
    }
    snprintf(out.how, sizeof(out.how), "found through the calls to SetCameraData: %u of %u places call %08X, %s", votes[best].sites, sites, entry,
             stub == 1 ? "a jump stub through a table entry" : stub == 2 ? "a jump stub" : stub == 3 ? "a push / ret stub" : "the function itself");
    if (note && noteSize)
        snprintf(note, noteSize, "%s", out.how);
    return true;
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
    // SetCameraData found and SetCamera not: a build that rewrote SetCamera.  It is looked for through the calls to SetCameraData.
    GameCameraCode viaCallSites;
    char callSites[420] = "";
    bool throughCalls = false;
    if (found[0] && !found[1])
        throughCalls = FindSetCameraThroughCallSites(scan, found[0], viaCallSites, callSites, sizeof(callSites));
    if (why && whySize)
        snprintf(why, whySize, "%s; %s%s%s", text[0], text[1], callSites[0] ? "; " : "", callSites);
    if (!found[0] || !(found[1] || throughCalls))
        return false;
    out.setCameraData = found[0];
    out.dataForm = form[0];
    // stolen bytes: SetCameraData starts with push ebp / mov ebp,esp / and esp,-16 / sub esp,0x18 (9 bytes), SetCamera with mov eax,ecx /
    // push ebx / mov ebx,[esp+8] (7 bytes); none has a relative operand, and they are whole instructions in both forms
    out.setCameraDataStolen = 9;
    if (throughCalls) {
        out.setCamera = viaCallSites.setCamera;
        out.cameraForm = viaCallSites.cameraForm;
        out.cameraStub = viaCallSites.cameraStub;
        out.cameraOperand = viaCallSites.cameraOperand;
        out.cameraBody = viaCallSites.cameraBody;
        out.setCameraStolen = viaCallSites.setCameraStolen;
        out.cameraBytes = viaCallSites.cameraBytes;
        memcpy(out.how, viaCallSites.how, sizeof(out.how));
        if (out.setCameraDataStolen > g_patterns[0][1].length)
            return false;
    } else {
        out.setCamera = found[1];
        out.cameraForm = form[1];
        out.setCameraStolen = 7;
        if (out.setCameraDataStolen > g_patterns[0][1].length || out.setCameraStolen > g_patterns[1][1].length)
            return false;
    }
    const Pattern& cameraPattern = g_patterns[1][1];
    if (!throughCalls && cameraPattern.immOffset >= 0) {
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

void InitAiHudScan(AiHudScan& scan)
{
    memset(&scan, 0, sizeof(scan));
}

void ScanForAiHudCode(const unsigned char* code, size_t size, AiHudScan& scan)
{
    ParseAll();
    if (size >= g_viewportStart.length)
        for (size_t i = 0; i + g_viewportStart.length <= size; i++)
            if (code[i] == g_viewportStart.bytes[0] && Matches(code + i, g_viewportStart)) {
                if (!scan.viewportCount++) {
                    scan.viewportFirst = code + i;
                    scan.viewportAvailable = size - i;
                }
            }
    const Pattern& pattern = g_aiHudPattern;
    if (size < pattern.length)
        return;
    unsigned char first = pattern.bytes[0]; // (the first byte is fixed)
    for (size_t i = 0; i + pattern.length <= size; i++) {
        if (code[i] != first || !Matches(code + i, pattern))
            continue;
        if (!scan.count++) {
            scan.first = code + i;
            scan.available = size - i;
        }
    }
}

bool ResolveAiHudCode(const AiHudScan& scan, AiHudCode& out, char* why, size_t whySize)
{
    ParseAll();
    memset(&out, 0, sizeof(out));
    if (why && whySize)
        snprintf(why, whySize, "UIAiHud update: %u match%s%s", scan.count, scan.count == 1 ? "" : "es", scan.count == 1 ? "" : " (not usable)");
    if (scan.count != 1 || kAiHudDrawStolen > g_aiHudPattern.length)
        return false;
    out.draw = scan.first;
    out.drawStolen = kAiHudDrawStolen;
    // the two inner sites, by their offsets from the start and the bytes around them: when the function is not the one the offsets were read from they
    // are NULL and the markers stay where the game puts them
    auto site = [&](const Pattern& pattern, size_t at, size_t target) -> const unsigned char* {
        if (scan.available < at + pattern.length || !Matches(scan.first + at, pattern))
            return NULL;
        return scan.first + target;
    };
    out.planeJump = site(g_aiHudPlanePattern, kAiHudPlaneAt, kAiHudPlaneJump);
    out.rootStore = site(g_aiHudRootPattern, kAiHudRootAt, kAiHudRootStore);
    if (scan.available >= kAiHudBillboardAt + g_aiHudBillboardPattern.length && Matches(scan.first + kAiHudBillboardAt, g_aiHudBillboardPattern))
        out.billboardFlag = (const unsigned char*)(uintptr_t)ReadU32(scan.first + kAiHudBillboardAt + 2);
    if (scan.viewportCount == 1 && scan.viewportAvailable >= kViewportEndAt + g_viewportEnd.length && Matches(scan.viewportFirst + kViewportEndAt, g_viewportEnd)) {
        out.viewportUpdate = scan.viewportFirst;
        out.viewportStolen = kViewportStolen;
    }
    return true;
}
