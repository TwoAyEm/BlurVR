// Fan levels past the game's 25.  The game keeps its fan status table as a vector of 32-byte entries {int64 fans needed, name hash, unlock list, unlock count, ...}
// (entry n = level n, entry 0 a placeholder, so 26 entries) behind a {data, count} object, and every function that uses it reads the count from there: the level for a
// fan total is the last entry whose fans are reached, "max level" is count-1, and a level without unlocks is skipped by the loops that build the unlock lists.  The fan
// total itself is a 64-bit number in the profile.  So a longer table is all it takes: this finds the loaded table by its contents (nothing about a build is hard-coded),
// copies the 26 entries into a bigger array, appends the new levels and points the {data, count} object at it.
#include "common.h"
#include "config.h"
#include "core.h"
#include "fan_levels.h"
#include "inline_hook.h"

namespace {

#pragma pack(push, 1)
struct RankEntry {
    long long fans; // fans needed to reach this level
    unsigned hash;  // the name hash of the level (its title)
    unsigned unlocks;
    int unlockCount;
    unsigned pad[3];
};
struct RankVector {
    RankEntry* data;
    int count;
};
#pragma pack(pop)
static_assert(sizeof(RankEntry) == 32, "the game's rank entries are 32 bytes");

const int kStockCount = 26; // entries in the game's table: levels 1 to 25 and the placeholder in front
const long long kStockFans[kStockCount] = {0,     0,     700,    3350,   6300,   9500,   14300,  18450,  22880,
                                           27500, 32600, 37950,  43450,  53900,  60600,  67700,  75100,  82800,
                                           90850, 99250, 116900, 126750, 136750, 147150, 158000, 170300};

// The name of a level is a hash the game looks up in its text (level 25 is "Fan Status 25"), and the text only goes to 25, so a level above it would be named
// like level 25 on the results screen.  The new levels get names of their own: hashes in a range that is not the game's, and a hook on the game's text lookup
// (hash -> wide string) that answers them with the text of level 25 and its number changed.
const unsigned kNameBase = 0xFA17E000u; // + the level
const int kNameLevels = 1000;
typedef const wchar_t*(__fastcall* GetTextFn)(void* self, void* edx, unsigned hash);
GetTextFn g_origGetText = NULL;
unsigned g_templateHash = 0; // the name hash of level 25
const wchar_t* g_missingText = NULL; // what the lookup answers for a hash it does not have
wchar_t g_names[kNameLevels][48];
bool g_nameMade[kNameLevels];

bool IsDigit(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

// `source` with its last number replaced by `level` ("Fan Status 25" -> "Fan Status 26"); a text without a number gets one added.
void MakeLevelName(const wchar_t* source, unsigned level, wchar_t* out)
{
    wchar_t text[48];
    size_t n = 0;
    while (source[n] && n < 40) {
        text[n] = source[n];
        n++;
    }
    size_t end = n;
    while (end > 0 && !IsDigit(text[end - 1]))
        end--;
    size_t start = end;
    while (start > 0 && IsDigit(text[start - 1]))
        start--;
    size_t w = 0;
    if (end == start) { // no number in the text
        memcpy(out, text, n * sizeof(wchar_t));
        w = n;
        out[w++] = L' ';
        end = n;
    } else {
        memcpy(out, text, start * sizeof(wchar_t));
        w = start;
    }
    wchar_t digits[8];
    int len = 0;
    for (unsigned v = level; v; v /= 10)
        digits[len++] = (wchar_t)(L'0' + v % 10);
    while (len > 0)
        out[w++] = digits[--len];
    memcpy(out + w, text + end, (n - end) * sizeof(wchar_t));
    out[w + (n - end)] = 0;
}

const wchar_t* __fastcall H_GetText(void* self, void* edx, unsigned hash)
{
    unsigned level = hash - kNameBase;
    if (level < (unsigned)kNameLevels && g_templateHash) {
        static unsigned asked = 0;
        if (asked < 40) {
            asked++;
            Log("fan levels: the game asked for the name of level %u (%s)", level, g_nameMade[level] ? "made before" : "made now");
        }
        if (!g_nameMade[level]) {
            const wchar_t* source = g_origGetText(self, edx, g_templateHash);
            if (!source)
                return NULL;
            MakeLevelName(source, level, g_names[level]);
            g_nameMade[level] = true;
        }
        return g_names[level];
    }
    const wchar_t* text = g_origGetText(self, edx, hash);
    if (g_cfg.fanTraceScripts && text && text == g_missingText) {
        static unsigned misses = 0;
        if (misses++ < 80)
            Log("fan levels: the game asked for text %08x and there is none", hash);
    }
    return text;
}

// (the call targets at 26 and 65 and the address of the "missing text" string at 89 are not compared)
bool IsWildcard(size_t k)
{
    return (k >= 26 && k < 30) || (k >= 65 && k < 69) || (k >= 89 && k < 93);
}

// Some text is asked for by its key ("SP_RANK_28" for the level-up stamp), which the game turns into a hash with a lookup that finds nothing for a level above 25;
// the hook answers those with the new level's hash.
typedef unsigned(__fastcall* StringIdFn)(void* self, void* edx, const char* key);
StringIdFn g_origStringId = NULL;
unsigned __fastcall H_StringId(void* self, void* edx, const char* key)
{
    unsigned id = g_origStringId(self, edx, key);
    if (!id && key && !strncmp(key, "SP_RANK_", 8)) {
        unsigned level = 0;
        const char* c = key + 8;
        while (*c >= '0' && *c <= '9' && level < 100000)
            level = level * 10 + (unsigned)(*c++ - '0');
        if (!*c && c != key + 8 && level >= (unsigned)kStockCount && level < (unsigned)kNameLevels && g_templateHash) {
            static unsigned asked = 0;
            if (asked++ < 20)
                Log("fan levels: the game asked for the text key %s (answered with the name of level %u)", key, level);
            return kNameBase + level;
        }
    }
    return id;
}

bool InstallKeyHook()
{
    static const BYTE bytes[] = {0x56, 0x8B, 0x71, 0x08, 0x57, 0x85, 0xF6, 0x74, 0x21, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0x4E, 0x0C, 0x8D, 0x46, 0x0C, 0x81, 0xC1, 0x24,
                                 0x01, 0x00, 0x00, 0x57, 0xE8, 0,    0,    0,    0,    0x85, 0xC0, 0x75, 0x09, 0x8B, 0x76, 0x04, 0x85, 0xF6, 0x75, 0xE3, 0x33, 0xC0,
                                 0x5F, 0x5E, 0xC2, 0x04};
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    unsigned matches = 0;
    BYTE* found = NULL;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[s].Name, ".bind", 8))
            continue;
        BYTE* code = base + sections[s].VirtualAddress;
        DWORD size = sections[s].Misc.VirtualSize ? sections[s].Misc.VirtualSize : sections[s].SizeOfRawData;
        for (DWORD i = 0; i + sizeof(bytes) <= size; i++) {
            if (code[i] != bytes[0] || code[i + 1] != bytes[1])
                continue;
            bool same = true;
            for (size_t k = 2; k < sizeof(bytes) && same; k++)
                if (!(k >= 27 && k < 31) && code[i + k] != bytes[k])
                    same = false;
            if (same) {
                matches++;
                found = code + i;
            }
        }
    }
    if (matches != 1)
        return false;
    g_origStringId = (StringIdFn)InstallInlineHook(found, bytes, 7, (void*)H_StringId);
    if (g_origStringId)
        Log("fan levels: the game's text key lookup is at %p", (void*)found);
    return g_origStringId != NULL;
}

// The game's text lookup is found by its bytes (the call targets and the address of its "missing text" string are wildcards).
bool InstallNameHook()
{
    static const BYTE bytes[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x04, 0x85, 0xC0, 0x74, 0x1A, 0x8D, 0xB0, 0x24, 0x01,
                                 0x00, 0x00, 0x53, 0x8B, 0xCE, 0xE8, 0,    0,    0,    0,    0x85, 0xC0, 0x74, 0x08, 0x8B, 0x40, 0x04, 0x03, 0x46, 0x24,
                                 0x75, 0x33, 0x8B, 0x7F, 0x08, 0x85, 0xFF, 0x74, 0x27, 0x8B, 0x77, 0x0C, 0x8D, 0x47, 0x0C, 0x81, 0xC6, 0x24, 0x01, 0x00,
                                 0x00, 0x53, 0x8B, 0xCE, 0xE8, 0,    0,    0,    0,    0x85, 0xC0, 0x74, 0x08, 0x8B, 0x40, 0x04, 0x03, 0x46, 0x24, 0x75,
                                 0x0C, 0x8B, 0x7F, 0x04, 0x85, 0xFF, 0x75, 0xD9, 0xB8, 0,    0,    0,    0,    0x5F, 0x5E, 0x5B, 0xC2, 0x04, 0x00};
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    unsigned matches = 0;
    BYTE* found = NULL;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[s].Name, ".bind", 8))
            continue;
        BYTE* code = base + sections[s].VirtualAddress;
        DWORD size = sections[s].Misc.VirtualSize ? sections[s].Misc.VirtualSize : sections[s].SizeOfRawData;
        for (DWORD i = 0; i + sizeof(bytes) <= size; i++) {
            if (code[i] != bytes[0] || code[i + 1] != bytes[1])
                continue;
            bool same = true;
            for (size_t k = 2; k < sizeof(bytes) && same; k++)
                if (!IsWildcard(k) && code[i + k] != bytes[k])
                    same = false;
            if (same) {
                matches++;
                found = code + i;
            }
        }
    }
    if (matches != 1)
        return false;
    memcpy(&g_missingText, found + 89, 4); // (the immediate of its "mov eax, <missing text>")
    g_origGetText = (GetTextFn)InstallInlineHook(found, bytes, 9, (void*)H_GetText);
    if (!g_origGetText) {
        Log("fan levels: could not hook the game's text lookup at %p", (void*)found);
        return false;
    }
    Log("fan levels: the game's text lookup is at %p (the new levels get names of their own)", (void*)found);
    return true;
}

// A level above 25 has nothing to unlock, but the game still lists a rank-up unlock for it after the race and shows its unlock screen (empty).  The function that
// collects the unlocks of a race (the one that fills the list the unlock screen shows) loops over the levels reached and adds {type 3, level} to that list for each; a
// detour at that point leaves the entry out for levels above 25, and the "ranked up" flag it also sets (the screen's other trigger).
bool InstallUnlockSuppress()
{
    static const BYTE sig[] = {0x3B, 0x7C, 0x24, 0x20, 0x0F, 0x9E, 0xC0, 0x88, 0x44, 0x24, 0x24, 0x84, 0xC0, 0x74, 0x54, 0x8B, 0x44, 0x24, 0x54,
                               0x39, 0x38, 0x7D, 0x4C, 0x8B, 0x43, 0x08, 0x8B, 0x4B, 0x04, 0xBD, 0x03, 0x00, 0x00, 0x00, 0x3B, 0xC1, 0x75, 0x20};
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    unsigned matches = 0;
    BYTE* found = NULL;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[s].Name, ".bind", 8))
            continue;
        BYTE* code = base + sections[s].VirtualAddress;
        DWORD size = sections[s].Misc.VirtualSize ? sections[s].Misc.VirtualSize : sections[s].SizeOfRawData;
        for (DWORD i = 0; i + sizeof(sig) <= size; i++)
            if (code[i] == sig[0] && !memcmp(code + i, sig, sizeof(sig))) {
                matches++;
                found = code + i;
            }
    }
    if (matches != 1)
        return false;
    BYTE* site = found + 15;  // mov eax,[esp+0x54] / cmp [eax],edi / jge skip   (8 bytes)
    BYTE* skip = site + 0x54; // where the game goes when the level adds nothing
    BYTE* resume = site + 8;  // the rest of the original: the entry is added
    BYTE* detour = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!detour)
        return false;
    BYTE* w = detour;
    auto emit = [&](std::initializer_list<BYTE> bytes) {
        for (BYTE b : bytes)
            *w++ = b;
    };
    auto jumpTo = [&](BYTE opcode1, BYTE opcode2, BYTE* target) { // E9 rel32 or 0F 8x rel32
        *w++ = opcode1;
        if (opcode2)
            *w++ = opcode2;
        int rel = (int)(target - (w + 4));
        memcpy(w, &rel, 4);
        w += 4;
    };
    emit({0x83, 0xFF, (BYTE)(kStockCount - 1)}); // cmp edi, 25 (edi = the level)
    jumpTo(0x0F, 0x8F, skip);                    // jg skip: nothing is added for it, and the "ranked up" flag is not set
    emit({0x8B, 0x44, 0x24, 0x54, 0x39, 0x38}); // the game's own: mov eax,[esp+0x54] / cmp [eax],edi
    jumpTo(0x0F, 0x8D, skip);                    // jge skip
    jumpTo(0xE9, 0, resume);                     // jmp resume
    DWORD old = 0;
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(detour, 0, MEM_RELEASE);
        return false;
    }
    site[0] = 0xE9;
    int rel = (int)(detour - (site + 5));
    memcpy(site + 1, &rel, 4);
    site[5] = site[6] = site[7] = 0x90;
    VirtualProtect(site, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    FlushInstructionCache(GetCurrentProcess(), detour, 64);
    Log("fan levels: the game's unlock list code is at %p (levels above 25 add no unlock)", (void*)found);
    return true;
}

bool ReadSelf(const void* from, void* to, size_t size);

// With Log=1: what the function that collects a race's unlocks put in the list the unlock screen shows ({kind, id} pairs: 3 = a level reached, 4 = a car, 5 = a mod ...),
// and the rank state around it.  Found by its first bytes (the arguments are the rank state and the list).
typedef int(__cdecl* CollectUnlocksFn)(void* state, void* list);
CollectUnlocksFn g_origCollect = NULL;
int __cdecl H_CollectUnlocks(void* state, void* list)
{
    static unsigned logged = 0;
    unsigned before[12] = {};
    ReadSelf(state, before, sizeof(before));
    int result = g_origCollect(state, list);
    if (logged < 30) {
        unsigned after[12] = {};
        struct {
            const unsigned* data;
            int size;
        } v = {};
        ReadSelf(state, after, sizeof(after));
        ReadSelf(list, &v, sizeof(v));
        char text[600];
        int n = snprintf(text, sizeof(text), "fan levels: unlock list after a race: level %u -> %u, fans %u, %d entries:", before[0], after[0], after[2], v.size);
        for (int i = 0; i < v.size && i < 24 && n < (int)sizeof(text) - 24; i++) {
            unsigned e[2] = {};
            ReadSelf(v.data + 2 * i, e, sizeof(e));
            n += snprintf(text + n, sizeof(text) - n, " (%u,%u)", e[0], e[1]);
        }
        Log("%s", text);
        logged++;
    }
    return result;
}

bool InstallCollectLog()
{
    static const BYTE sig[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x40, 0x85, 0xC0, 0x0F, 0x84, 0, 0, 0, 0, 0x56, 0x8B, 0x35, 0, 0, 0, 0, 0x85, 0xF6, 0x0F, 0x84};
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    unsigned matches = 0;
    BYTE* found = NULL;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[s].Name, ".bind", 8))
            continue;
        BYTE* code = base + sections[s].VirtualAddress;
        DWORD size = sections[s].Misc.VirtualSize ? sections[s].Misc.VirtualSize : sections[s].SizeOfRawData;
        for (DWORD i = 0; i + sizeof(sig) <= size; i++) {
            if (code[i] != sig[0] || code[i + 1] != sig[1])
                continue;
            bool same = true;
            for (size_t k = 2; k < sizeof(sig) && same; k++)
                if (!((k >= 11 && k < 15) || (k >= 18 && k < 22)) && code[i + k] != sig[k])
                    same = false;
            if (same) {
                matches++;
                found = code + i;
            }
        }
    }
    if (matches != 1)
        return false;
    g_origCollect = (CollectUnlocksFn)InstallInlineHook(found, sig, 7, (void*)H_CollectUnlocks);
    if (g_origCollect)
        Log("fan levels: the game's unlock collector is at %p (logged)", (void*)found);
    return g_origCollect != NULL;
}

// With Log=1 and [Fans] TraceScripts=1 (debugging, the Steam build only): the order in which the unlock and rank screens' scripts call the game's rank functions, to
// see which call starts the unlock screens.  The game copies its script functions when it starts, so their code is hooked (the first instructions are compared first).
const char* g_lastScriptCall = ""; // the name of the last traced function the scripts called
void __cdecl LogLuaCall(const char* name)
{
    static unsigned n = 0;
    g_lastScriptCall = name;
    if (g_cfg.fanTraceScripts && n++ < 600)
        Log("fan levels: script calls %s", name);
}

struct TracedFunction {
    const char* name;
    unsigned rva;
    unsigned length;
    BYTE bytes[8];
};
const TracedFunction kTraced[] = {
    {"GameInfo", 0x7fce60, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"TierInfo", 0x8011a0, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"EventInfo", 0x800a60, 5, {0x56, 0x8B, 0x74, 0x24, 0x08}},
    {"VehicleInfo", 0x7fdec0, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"ModInfo", 0x7fdee0, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"RankInfo", 0x7fd800, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"BossInfo", 0x7ff9f0, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"ClearUnlocks", 0x7fe3a0, 5, {0xA1, 0xB0, 0x03, 0x24, 0x01}},
    {"ProcessUnlocks", 0x800ab0, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"VehicleUnlocks", 0x800ae0, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"UpcomingUnlocks", 0x7ffa10, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"ClearLastSessionInfo", 0x7fced0, 5, {0xA1, 0xAC, 0x03, 0x24, 0x01}},
    {"CompleteEvent", 0x7ffa30, 7, {0x56, 0x8B, 0x35, 0xAC, 0x03, 0x24, 0x01}},
    {"MarkNotNew", 0x7fdf70, 5, {0x56, 0x8B, 0x74, 0x24, 0x08}},
    {"GetNewUnlocks", 0x867a20, 5, {0x83, 0xEC, 0x08, 0x53, 0x55}},
    {"GetUnlocksForLevel", 0x867140, 5, {0x56, 0x8B, 0x74, 0x24, 0x08}},
    {"GetProgressionForRank", 0x86b3c0, 8, {0x83, 0xEC, 0x60, 0x53, 0x8B, 0x5C, 0x24, 0x68}},
    {"LevelProgressionData", 0x863100, 5, {0x56, 0x8B, 0x74, 0x24, 0x08}},
    {"ProfileProgression", 0x862fc0, 5, {0x83, 0xEC, 0x0C, 0x53, 0x55}},
    {"GetFeatureUnlocked", 0x85f6e0, 5, {0x56, 0x8B, 0x74, 0x24, 0x08}},
    {"GetFeatureUnlockedMap", 0x8658b0, 6, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0}},
    {"GetMaxRank", 0x8602b0, 5, {0xA1, 0x90, 0xBF, 0x0F, 0x01}},
    {"UnlockedLegend", 0x860250, 5, {0xB9, 0xD8, 0x34, 0x21, 0x01}},
    {"GetNewCarsForClass", 0x867e80, 8, {0x83, 0xEC, 0x10, 0x56, 0x8B, 0x74, 0x24, 0x18}},
    {"GetNewAbilitiesMap", 0x864470, 7, {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10}},
    {"GetNewChallengePacksMap", 0x8644d0, 8, {0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14}},
    {"GetNewCarChallengesMap", 0x867da0, 7, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x08}},
    {"FanStatusForTooltip", 0x7fd880, 5, {0x83, 0xEC, 0x0C, 0x53, 0x56}},
    {"GetAbilityUnlockMap", 0x866e80, 5, {0x83, 0xEC, 0x0C, 0x53, 0x56}},
    {"AddToTotalFans", 0x7fcef0, 7, {0x56, 0x8B, 0x35, 0xAC, 0x03, 0x24, 0x01}},
    {"NewSaveGame", 0x801170, 6, {0x8B, 0x0D, 0xAC, 0x03, 0x24, 0x01}},
};

bool InstallLuaTrace()
{
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return false;
    unsigned hooked = 0;
    for (const TracedFunction& f : kTraced) {
        BYTE* at = base + f.rva;
        BYTE* thunk = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!thunk)
            break;
        BYTE* w = thunk;
        *w++ = 0x9C; // pushfd
        *w++ = 0x60; // pushad
        *w++ = 0x68; // push name
        const void* np = f.name;
        memcpy(w, &np, 4);
        w += 4;
        *w++ = 0xB8; // mov eax, LogLuaCall
        void* lf = (void*)LogLuaCall;
        memcpy(w, &lf, 4);
        w += 4;
        *w++ = 0xFF; // call eax
        *w++ = 0xD0;
        *w++ = 0x83; // add esp, 4
        *w++ = 0xC4;
        *w++ = 0x04;
        *w++ = 0x61; // popad
        *w++ = 0x9D; // popfd
        *w++ = 0x68; // push (the trampoline of the function)
        BYTE* slot = w;
        w += 4;
        *w++ = 0xC3; // ret
        void* tramp = InstallInlineHook(at, f.bytes, f.length, thunk);
        if (!tramp) {
            VirtualFree(thunk, 0, MEM_RELEASE);
            continue;
        }
        memcpy(slot, &tramp, 4);
        FlushInstructionCache(GetCurrentProcess(), thunk, 32);
        hooked++;
    }
    Log("fan levels: %u script functions are traced", hooked);
    return true;
}

RankEntry* g_ours = NULL;       // the longer table (never freed: the game may read it until it exits)
RankVector* g_holder = NULL;    // the game's {data, count} object that points at it
bool g_started = false;

bool ReadSelf(const void* from, void* to, size_t size)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, size, &got) && got == size;
}

bool WriteSelf(void* to, const void* from, size_t size)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), to, from, size, &got) && got == size;
}

bool IsStockTable(const RankEntry* at)
{
    RankEntry e[kStockCount];
    if (!ReadSelf(at, e, sizeof(e)))
        return false;
    for (int i = 0; i < kStockCount; i++)
        if (e[i].fans != kStockFans[i])
            return false;
    return true;
}

// Call `visit(address, bytes, size)` for every chunk of the committed read-write memory of the process (the game's heap is there); stop when it returns true.  The
// chunks overlap by `overlap` bytes so that something that straddles two of them is seen whole.  Memory is read with ReadProcessMemory, which fails instead of
// crashing when the game frees a page meanwhile.
template <class F> bool ScanMemory(size_t overlap, F visit)
{
    static BYTE buffer[4 << 20];
    MEMORY_BASIC_INFORMATION mbi;
    BYTE* addr = (BYTE*)0x10000;
    while (addr < (BYTE*)0x7FFE0000 && VirtualQuery(addr, &mbi, sizeof(mbi))) {
        BYTE* base = (BYTE*)mbi.BaseAddress;
        BYTE* next = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            size_t done = 0;
            while (done < mbi.RegionSize) {
                size_t take = mbi.RegionSize - done;
                if (take > sizeof(buffer))
                    take = sizeof(buffer);
                if (!ReadSelf(base + done, buffer, take))
                    break;
                if (visit(base + done, buffer, take))
                    return true;
                if (done + take >= mbi.RegionSize)
                    break;
                done += take > overlap ? take - overlap : take;
            }
        }
        addr = next;
    }
    return false;
}

// The game's table: the array whose 26 entries have the stock fan numbers, and the {data, count = 26} object that points at it.
bool FindGameTable(RankEntry** array, RankVector** holder)
{
    RankEntry* found = NULL;
    ScanMemory(sizeof(RankEntry) * kStockCount, [&](BYTE* addr, const BYTE* bytes, size_t size) {
        // entry 2 (700 fans) is the first with a number that is not 0
        for (size_t i = 2 * sizeof(RankEntry); i + 4 <= size; i += 4) {
            if (*(const unsigned*)(bytes + i) != 700 || *(const unsigned*)(bytes + i + 4) != 0)
                continue;
            RankEntry* at = (RankEntry*)(addr + i - 2 * sizeof(RankEntry));
            if (at == g_ours)
                continue;
            if (IsStockTable(at)) {
                found = at;
                return true;
            }
        }
        return false;
    });
    if (!found)
        return false;
    RankVector* object = NULL;
    ScanMemory(sizeof(RankVector), [&](BYTE* addr, const BYTE* bytes, size_t size) {
        for (size_t i = 0; i + sizeof(RankVector) <= size; i += 4) {
            const RankVector* v = (const RankVector*)(bytes + i);
            if (v->data == found && v->count == kStockCount) {
                object = (RankVector*)(addr + i);
                return true;
            }
        }
        return false;
    });
    if (!object)
        return false;
    *array = found;
    *holder = object;
    return true;
}

RankEntry* BuildTable(const RankEntry* stock, int levels)
{
    int count = levels + 1;
    RankEntry* table = (RankEntry*)VirtualAlloc(NULL, sizeof(RankEntry) * count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!table)
        return NULL;
    if (!ReadSelf(stock, table, sizeof(RankEntry) * kStockCount)) {
        VirtualFree(table, 0, MEM_RELEASE);
        return NULL;
    }
    g_templateHash = table[kStockCount - 1].hash;
    long long step = g_cfg.fanFirstStep;
    for (int level = kStockCount; level <= levels; level++) {
        RankEntry& e = table[level];
        memset(&e, 0, sizeof(e));
        e.fans = table[level - 1].fans + step;
        e.hash = g_origGetText ? kNameBase + level : table[kStockCount - 1].hash; // (without the hook: the name of level 25)
        step += g_cfg.fanStepGrowth;
    }
    return table;
}

// The hooks into the game's code, tried on every round of the thread (the code may be unpacked late) until each is in or 40 rounds have passed.
void TryHooks()
{
    static unsigned rounds = 0;
    static bool unlockPatched = false, collectLogged = false, keyHooked = false, traced = false;
    rounds++;
    if (rounds > 40)
        return;
    if (!g_origGetText)
        InstallNameHook();
    if (!keyHooked && g_origGetText)
        keyHooked = InstallKeyHook();
    if (!unlockPatched && g_cfg.fanHideRankUnlock)
        unlockPatched = InstallUnlockSuppress();
    if (!collectLogged && g_logEnabled)
        collectLogged = InstallCollectLog();
    if (!traced && g_logEnabled && g_cfg.fanTraceScripts && rounds >= 3) // (the script tables are set up by then)
        traced = InstallLuaTrace();
}

DWORD WINAPI FanLevelsThread(LPVOID)
{
    int levels = g_cfg.fanMaxLevel;
    unsigned tries = 0;
    for (;;) {
        TryHooks();
        if (g_holder) {
            // patched: look after it, the game may load its data again (the holder then points at a new table)
            MEMORY_BASIC_INFORMATION mbi;
            RankVector now;
            if (VirtualQuery(g_holder, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && ReadSelf(g_holder, &now, sizeof(now)) && now.data == g_ours &&
                now.count == levels + 1) {
                Sleep(5000);
                continue;
            }
            Log("fan levels: the game's table changed, looking for it again");
            g_holder = NULL;
            tries = 0;
        }
        Sleep(tries < 60 ? 1000 : 5000);
        tries++;
        RankEntry* array = NULL;
        RankVector* holder = NULL;
        if (!FindGameTable(&array, &holder))
            continue;
        RankEntry* table = BuildTable(array, levels);
        if (!table) {
            Log("fan levels: could not allocate the table");
            return 0;
        }
        RankEntry* old = g_ours;
        g_ours = table;
        // the new array is complete before the game can see it, and the old count (26) is valid for the new array too
        bool ok = WriteSelf(&holder->data, &table, sizeof(table));
        int count = levels + 1;
        ok = ok && WriteSelf(&holder->count, &count, sizeof(count));
        if (!ok) {
            g_ours = old;
            Log("fan levels: could not write the game's table object at %p", (void*)holder);
            return 0;
        }
        g_holder = holder;
        Log("fan levels: table %p (26 entries) replaced by %p (%d levels, level %d needs %lld fans), object at %p", (void*)array, (void*)table, levels, levels,
            table[levels].fans, (void*)holder);
    }
}

} // namespace

void StartFanLevels()
{
    if (g_started || !g_cfg.enabled || g_cfg.fanMaxLevel <= kStockCount - 1)
        return;
    g_started = true;
    HANDLE thread = CreateThread(NULL, 0, FanLevelsThread, NULL, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else
        Log("fan levels: could not start the thread");
}
