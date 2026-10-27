// Runs the camera-function scan of src/game_signatures.cpp on a Blur.exe (or on a dump of the running module) and prints what it finds,
// so a build can be looked at without starting the game.  Built and run by tools/camera_sig_check.py.
//
//     camera_sig_check [--call-sites] <Blur.exe | dump.exe> [...]
//
// --call-sites also runs the second way of finding SetCamera (through the calls to SetCameraData) on a build whose SetCamera the patterns found, and says
// whether the two agree: the way a build with rewritten code is found, tried on one that is not.
//
// Sections are mapped as the file says (raw data at the section's virtual address), so a dump written with every section at its own
// address in the file works the same as an ordinary executable.  An executable that is encrypted on disk shows as high entropy and
// finds nothing: dump it from the running game instead.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "../src/game_signatures.h"

static unsigned ReadU16(const unsigned char* p)
{
    return p[0] | (p[1] << 8);
}
static unsigned ReadU32(const unsigned char* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static double Entropy(const unsigned char* p, size_t n)
{
    if (!n)
        return 0;
    size_t count[256] = {0};
    for (size_t i = 0; i < n; i++)
        count[p[i]]++;
    double e = 0;
    for (size_t c : count)
        if (c) {
            double q = (double)c / (double)n;
            e -= q * log2(q);
        }
    return e;
}

struct Section {
    std::string name;
    unsigned va, virtualSize, flags;
    std::vector<unsigned char> bytes;
};

static bool g_callSites = false;

static int Check(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("%s: cannot open\n", path);
        return 1;
    }
    std::vector<unsigned char> file;
    unsigned char chunk[1 << 16];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
        file.insert(file.end(), chunk, chunk + got);
    fclose(f);
    if (file.size() < 0x200 || file[0] != 'M' || file[1] != 'Z') {
        printf("%s: not a PE file\n", path);
        return 1;
    }
    unsigned nt = ReadU32(&file[0x3c]);
    if (nt + 0xf8 > file.size() || memcmp(&file[nt], "PE\0\0", 4) != 0) {
        printf("%s: no PE header\n", path);
        return 1;
    }
    unsigned sectionCount = ReadU16(&file[nt + 6]), optionalSize = ReadU16(&file[nt + 20]);
    unsigned timeStamp = ReadU32(&file[nt + 8]), entry = ReadU32(&file[nt + 24 + 16]), imageBase = ReadU32(&file[nt + 24 + 28]);
    unsigned imageSize = ReadU32(&file[nt + 24 + 56]);
    printf("%s\n  %zu bytes on disk, image 0x%x, PE time stamp 0x%08x, image base 0x%08x, entry rva 0x%x, %u sections\n", path, file.size(), imageSize, timeStamp,
           imageBase, entry, sectionCount);
    std::vector<Section> sections;
    unsigned table = nt + 24 + optionalSize;
    for (unsigned i = 0; i < sectionCount && table + 40 * (i + 1) <= file.size(); i++) {
        const unsigned char* s = &file[table + 40 * i];
        Section section;
        section.name.assign((const char*)s, strnlen((const char*)s, 8));
        unsigned virtualSize = ReadU32(s + 8), raw = ReadU32(s + 16), rawPointer = ReadU32(s + 20);
        section.va = ReadU32(s + 12);
        section.flags = ReadU32(s + 36);
        section.virtualSize = virtualSize ? virtualSize : raw;
        size_t size = raw < section.virtualSize ? raw : section.virtualSize;
        if (rawPointer < file.size())
            section.bytes.assign(file.begin() + rawPointer, file.begin() + rawPointer + (rawPointer + size <= file.size() ? size : file.size() - rawPointer));
        section.bytes.resize(section.virtualSize, 0);
        bool exec = (section.flags & 0x20000000) != 0;
        printf("  %-8s va 0x%08x size 0x%08x raw 0x%08x %s entropy %.2f\n", section.name.c_str(), section.va, section.virtualSize, raw, exec ? "code" : "data",
               Entropy(section.bytes.data(), section.bytes.size() < (1u << 20) ? section.bytes.size() : (1u << 20)));
        sections.push_back(section);
    }
    GameCameraScan scan;
    InitGameCameraScan(scan);
    AiHudScan aiScan;
    InitAiHudScan(aiScan);
    bool encrypted = false;
    for (const Section& s : sections) {
        if (!(s.flags & 0x20000000) || s.name == ".bind")
            continue;
        if (Entropy(s.bytes.data(), s.bytes.size() < (1u << 20) ? s.bytes.size() : (1u << 20)) > 7.5)
            encrypted = true;
        ScanForGameCameraCode(s.bytes.data(), s.bytes.size(), scan, (unsigned long long)imageBase + s.va);
        ScanForAiHudCode(s.bytes.data(), s.bytes.size(), aiScan);
    }
    if (encrypted)
        printf("  WARNING: a code section has the entropy of encrypted or packed data; this file cannot be scanned, dump the running module instead\n");
    auto va = [&](const unsigned char* p) -> unsigned {
        for (const Section& s : sections)
            if (p >= s.bytes.data() && p < s.bytes.data() + s.bytes.size())
                return imageBase + s.va + (unsigned)(p - s.bytes.data());
        return 0;
    };
    const char* names[2] = {"SetCameraData", "SetCamera"};
    for (int fn = 0; fn < 2; fn++) {
        char first[2][24] = {"", ""};
        for (int form = 0; form < 2; form++)
            if (scan.count[fn][form])
                snprintf(first[form], sizeof(first[form]), " first at 0x%08x", va(scan.first[fn][form]));
        printf("  %-13s tight %u match(es)%s, loose %u match(es)%s\n", names[fn], scan.count[fn][0], first[0], scan.count[fn][1], first[1]);
    }
    // the AI HUD function (ai_hud.cpp) is optional: without it the markers stay on the flat HUD panel, so it does not change the result
    AiHudCode aiCode;
    char aiWhy[128];
    if (ResolveAiHudCode(aiScan, aiCode, aiWhy, sizeof(aiWhy)))
        printf("  AI HUD: UIAiHud update 0x%08x (%s), hook bytes %zu, plane test jump %s, root store %s, HUD sway update %s, billboard flag %s\n", va(aiCode.draw), aiWhy,
               aiCode.drawStolen, aiCode.planeJump ? "found" : "NOT FOUND", aiCode.rootStore ? "found" : "NOT FOUND", aiCode.viewportUpdate ? "found" : "NOT FOUND",
               aiCode.billboardFlag ? "found" : "NOT FOUND");
    if (aiCode.billboardFlag)
        printf("    UI.HUD.BillboardToCarInsteadOfCamera at 0x%08x\n", (unsigned)(uintptr_t)aiCode.billboardFlag);
    else
        printf("  AI HUD: not found (%s)\n", aiWhy);
    GameCameraCode code;
    char why[768];
    if (!ResolveGameCameraCode(scan, code, why, sizeof(why))) {
        printf("  NOT RESOLVED: %s\n", why);
        return 1;
    }
    const char* forms[3] = {"tight pattern", "loose pattern", "call sites"};
    printf("  RESOLVED: SetCameraData 0x%08x (%s pattern), SetCamera 0x%08x (%s), camera description 0x%x bytes, hook bytes %zu and %zu\n", va(code.setCameraData),
           code.dataForm ? "loose" : "tight", va(code.setCamera), forms[code.cameraForm < 0 || code.cameraForm > 2 ? 0 : code.cameraForm], (unsigned)code.cameraBytes,
           code.setCameraDataStolen, code.setCameraStolen);
    if (code.cameraForm == 2)
        printf("  SetCamera was %s\n", code.how);
    if (code.cameraStub)
        printf("  the hook rewrites %s at 0x%08x, which names the body 0x%08x\n", code.cameraStub == 1 ? "the table entry" : code.cameraStub == 2 ? "the jump" : "the push",
               va(code.cameraOperand), code.cameraBody);
    if (g_callSites && code.cameraForm != 2) {
        GameCameraCode other;
        char note[420];
        bool ok = FindSetCameraThroughCallSites(scan, code.setCameraData, other, note, sizeof(note));
        printf("  call-site search: %s%s\n", ok ? "" : "NOT FOUND: ", note);
        if (ok && !other.cameraStub && va(other.setCamera) != va(code.setCamera))
            printf("  call-site search DISAGREES: it names 0x%08x, the pattern 0x%08x\n", va(other.setCamera), va(code.setCamera));
        else if (ok && other.cameraBytes != code.cameraBytes)
            printf("  call-site search DISAGREES about the description: 0x%x bytes against 0x%x\n", (unsigned)other.cameraBytes, (unsigned)code.cameraBytes);
        else if (ok)
            printf("  call-site search agrees (description 0x%x bytes)\n", (unsigned)other.cameraBytes);
        return ok ? 0 : 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: camera_sig_check <Blur.exe or dump> [...]\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--call-sites")) {
            g_callSites = true;
            continue;
        }
        failures += Check(argv[i]);
    }
    return failures ? 1 : 0;
}
