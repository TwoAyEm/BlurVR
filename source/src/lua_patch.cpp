// A patch of the game's single-player fan screen script (Shared\Unlocks.lua) for fan levels past 25, made on the Lua 5.1 bytecode as the game loads it.  The game keeps
// its UI scripts compressed in its image and loads each one into its Lua state when a screen needs it; the loader's last step (luaU_undump) is hooked here and gets
// the chunk through the reader state (ZIO: n bytes left, p the next byte, and the reader that gives the rest, 512 bytes at a time), so the script is read whole and rewritten before Lua reads it.  NOTES.md section 31 has the why.
//
// The two changes: (1) the script goes on to the "new item unlocked" screen (Multiplayer\Ingame\MpUnlocks.lua, which in single player lists the cars a race
// unlocked) whenever the player ranked up, car or no car; a level above 25 unlocks none, so that screen came up empty.  The two reads of `GUI.ranked_up` that decide
// it now read `GUI.hasUnlockedVehicles` (the script's own flag for "the race unlocked a car"), which is also what the screen shows.  (2) at the highest level the
// script writes the text `SP_RANK_25` whatever the level is; it now names the highest level.
#include "common.h"
#include "config.h"
#include "core.h"
#include "inline_hook.h"
#include "lua_patch.h"

namespace {

struct Const {
    int type = 0; // 0 nil, 1 boolean, 3 number, 4 string
    BYTE boolean = 0;
    BYTE number[8] = {};
    std::string text; // without the trailing 0
};

struct Fn {
    std::vector<BYTE> head; // source name .. maxstack, copied as it is
    std::vector<unsigned> code;
    std::vector<Const> k;
    std::vector<Fn> protos;
    std::vector<BYTE> tail; // line info, local variables and upvalue names, copied as they are
};

struct Reader {
    const BYTE* p;
    const BYTE* end;
    bool bad = false;
    bool need(size_t n)
    {
        if ((size_t)(end - p) < n)
            bad = true;
        return !bad;
    }
    unsigned u32()
    {
        if (!need(4))
            return 0;
        unsigned v;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    BYTE u8()
    {
        if (!need(1))
            return 0;
        return *p++;
    }
};

void Put32(std::vector<BYTE>& out, unsigned v)
{
    BYTE b[4];
    memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

// a Lua string: size_t length (including the 0) and the characters; the length of its encoding is returned by SkipString
bool SkipString(Reader& r)
{
    unsigned n = r.u32();
    if (!r.need(n))
        return false;
    r.p += n;
    return true;
}

bool ParseFn(Reader& r, Fn& f, int depth)
{
    if (depth > 16)
        return false;
    const BYTE* start = r.p;
    if (!SkipString(r))
        return false;
    if (!r.need(8 + 4))
        return false;
    r.p += 8;                 // linedefined, lastlinedefined
    r.p += 4;                 // nups, numparams, is_vararg, maxstacksize
    f.head.assign(start, r.p);
    unsigned n = r.u32();
    if (r.bad || n > 1000000 || !r.need((size_t)n * 4))
        return false;
    f.code.resize(n);
    if (n)
        memcpy(f.code.data(), r.p, (size_t)n * 4);
    r.p += (size_t)n * 4;
    n = r.u32();
    if (r.bad || n > 1000000)
        return false;
    f.k.resize(n);
    for (unsigned i = 0; i < n; i++) {
        Const& c = f.k[i];
        c.type = r.u8();
        if (c.type == 1) {
            c.boolean = r.u8();
        } else if (c.type == 3) {
            if (!r.need(8))
                return false;
            memcpy(c.number, r.p, 8);
            r.p += 8;
        } else if (c.type == 4) {
            unsigned len = r.u32();
            if (!r.need(len) || len == 0)
                return false;
            c.text.assign((const char*)r.p, len - 1);
            r.p += len;
        } else if (c.type != 0) {
            return false;
        }
        if (r.bad)
            return false;
    }
    n = r.u32();
    if (r.bad || n > 100000)
        return false;
    f.protos.resize(n);
    for (unsigned i = 0; i < n; i++)
        if (!ParseFn(r, f.protos[i], depth + 1))
            return false;
    const BYTE* tailStart = r.p;
    n = r.u32(); // line info
    if (r.bad || !r.need((size_t)n * 4))
        return false;
    r.p += (size_t)n * 4;
    n = r.u32(); // local variables
    if (r.bad || n > 100000)
        return false;
    for (unsigned i = 0; i < n; i++) {
        if (!SkipString(r) || !r.need(8))
            return false;
        r.p += 8;
    }
    n = r.u32(); // upvalue names
    if (r.bad || n > 100000)
        return false;
    for (unsigned i = 0; i < n; i++)
        if (!SkipString(r))
            return false;
    f.tail.assign(tailStart, r.p);
    return !r.bad;
}

void WriteFn(const Fn& f, std::vector<BYTE>& out)
{
    out.insert(out.end(), f.head.begin(), f.head.end());
    Put32(out, (unsigned)f.code.size());
    for (unsigned w : f.code)
        Put32(out, w);
    Put32(out, (unsigned)f.k.size());
    for (const Const& c : f.k) {
        out.push_back((BYTE)c.type);
        if (c.type == 1) {
            out.push_back(c.boolean);
        } else if (c.type == 3) {
            out.insert(out.end(), c.number, c.number + 8);
        } else if (c.type == 4) {
            Put32(out, (unsigned)c.text.size() + 1);
            out.insert(out.end(), c.text.begin(), c.text.end());
            out.push_back(0);
        }
    }
    Put32(out, (unsigned)f.protos.size());
    for (const Fn& p : f.protos)
        WriteFn(p, out);
    out.insert(out.end(), f.tail.begin(), f.tail.end());
}

bool TreeHasString(const Fn& f, const char* text)
{
    for (const Const& c : f.k)
        if (c.type == 4 && c.text == text)
            return true;
    for (const Fn& p : f.protos)
        if (TreeHasString(p, text))
            return true;
    return false;
}

int FindString(const Fn& f, const char* text)
{
    for (size_t i = 0; i < f.k.size(); i++)
        if (f.k[i].type == 4 && f.k[i].text == text)
            return (int)i;
    return -1;
}

int AddString(Fn& f, const char* text)
{
    int at = FindString(f, text);
    if (at >= 0)
        return at;
    Const c;
    c.type = 4;
    c.text = text;
    f.k.push_back(c);
    return (int)f.k.size() - 1;
}

struct PatchCount {
    int reads = 0;
    int names = 0;
    bool failed = false;
};

const unsigned kOpGetTable = 6;

void PatchFn(Fn& f, const char* maxName, PatchCount& count)
{
    const int ranked = FindString(f, "ranked_up");
    if (ranked >= 0 && ranked < 256) {
        for (unsigned& ins : f.code) {
            if ((ins & 63) != kOpGetTable)
                continue;
            const unsigned c = (ins >> 14) & 511;
            if (c != (256u | (unsigned)ranked))
                continue;
            const int to = AddString(f, "hasUnlockedVehicles");
            if (to >= 256) {
                count.failed = true;
                continue;
            }
            ins = (ins & ~(511u << 14)) | ((256u | (unsigned)to) << 14);
            count.reads++;
        }
    }
    const int top = FindString(f, "SP_RANK_25");
    if (top >= 0) {
        f.k[top].text = maxName;
        count.names++;
    }
    for (Fn& p : f.protos)
        PatchFn(p, maxName, count);
}

// Lua's reader state, as this build lays it out (luaZ_read in the game reads n at +0, p at +4, the reader at +8, its data at +0xC and the Lua state at +0x10)
typedef const char*(__cdecl* LuaReaderFn)(void* L, void* data, size_t* size);
struct Zio {
    size_t n;
    const BYTE* p;
    LuaReaderFn reader;
    void* data;
    void* L;
};

typedef void*(__cdecl* UndumpFn)(void* L, Zio* z, void* buff, const char* name);
UndumpFn g_origUndump = NULL;

const BYTE kHeader[12] = {0x1B, 'L', 'u', 'a', 0x51, 0, 1, 4, 4, 4, 8, 0};

// the reader of a script that has been read whole: nothing more
const char* __cdecl EmptyReader(void*, void*, size_t* size)
{
    *size = 0;
    return NULL;
}

// The game hands a script to Lua in pieces of 512 bytes, so the script is read whole first (the pieces are asked for here, and the reader state is given the whole script
// and a reader with nothing more to give).  Only the one script is touched, found by the name Lua is given for it.
void* __cdecl H_Undump(void* L, Zio* z, void* buff, const char* name)
{
    static unsigned calls = 0;
    char shown[64] = {};
    if (name && !IsBadReadPtr(name, 48))
        for (int i = 0; i < 60 && name[i] >= 32 && name[i] < 127; i++)
            shown[i] = name[i];
    if (calls < 400 && g_logEnabled) {
        calls++;
        Log("lua patch: chunk %u: %u bytes in the first piece, name \"%s\"", calls, z ? (unsigned)z->n : 0, shown);
    }
    const bool wanted = strstr(shown, "Shared\\Unlocks.lua") != NULL;
    if (wanted && z && z->p && z->n >= 12 && z->n < (16u << 20) && !IsBadReadPtr(z->p, 12) && !memcmp(z->p, kHeader, sizeof(kHeader))) {
        static bool done = false;
        std::vector<BYTE>* whole = new std::vector<BYTE>(z->p, z->p + z->n); // (kept: Lua reads from it for as long as it loads the chunk)
        if (z->reader && z->reader != EmptyReader) {
            for (int pieces = 0; pieces < 100000 && whole->size() < (16u << 20); pieces++) {
                size_t got = 0;
                const char* piece = z->reader(z->L ? z->L : L, z->data, &got);
                if (!piece || got == 0)
                    break;
                whole->insert(whole->end(), (const BYTE*)piece, (const BYTE*)piece + got);
            }
        }
        z->reader = EmptyReader; // (the reader is spent; whatever happens below, Lua gets the script from the buffer)
        Reader r = {whole->data() + sizeof(kHeader), whole->data() + whole->size()};
        Fn top;
        if (!ParseFn(r, top, 0) || r.p != r.end) {
            Log("lua patch: could not read the unlock screen script (%u bytes), left as it is", (unsigned)whole->size());
        } else if (!TreeHasString(top, "SP_RANK_25") || !TreeHasString(top, "Multiplayer\\Ingame\\MpUnlocks.lua") || !TreeHasString(top, "ranked_up") ||
                   !TreeHasString(top, "hasUnlockedVehicles")) {
            if (!done)
                Log("lua patch: \"%s\" is not the script expected, left as it is", shown);
        } else {
            char maxName[24];
            snprintf(maxName, sizeof(maxName), "SP_RANK_%d", g_cfg.fanMaxLevel);
            PatchCount count;
            PatchFn(top, maxName, count);
            if (count.reads == 0 || count.failed) {
                Log("lua patch: the unlock screen script did not have the reads expected (%d), left as it is", count.reads);
            } else {
                std::vector<BYTE> out(kHeader, kHeader + sizeof(kHeader));
                WriteFn(top, out);
                if (!done)
                    Log("lua patch: unlock screen script patched: %d reads of ranked_up now read hasUnlockedVehicles, %d name(s) %s, %u -> %u bytes", count.reads,
                        count.names, maxName, (unsigned)whole->size(), (unsigned)out.size());
                done = true;
                *whole = out;
            }
        }
        z->p = whole->data();
        z->n = whole->size();
    }
    return g_origUndump(L, z, buff, name);
}

} // namespace

bool InstallLuaChunkPatch()
{
    if (g_origUndump)
        return true;
    // luaU_undump(L, Z, buff, name): reads the first byte of name ('@', '=' or the signature) to choose the chunk's name; 21 bytes, no address in them
    static const BYTE sig[] = {0x8B, 0x4C, 0x24, 0x10, 0x8A, 0x01, 0x83, 0xEC, 0x14, 0x3C, 0x40, 0x74, 0x12, 0x3C, 0x3D, 0x74, 0x0E, 0x3C, 0x1B, 0x75, 0x0B};
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
    if (matches != 1) {
        static unsigned said = 0;
        if (said++ == 5)
            Log("lua patch: the game's Lua loader was not found (%u candidates), the unlock screen script stays as it is", matches);
        return false;
    }
    g_origUndump = (UndumpFn)InstallInlineHook(found, sig, 9, (void*)H_Undump);
    if (g_origUndump)
        Log("lua patch: the game's Lua loader (luaU_undump) is at %p; the unlock screen script will be patched when it loads", (void*)found);
    return g_origUndump != NULL;
}
