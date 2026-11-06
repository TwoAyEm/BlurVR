// Host check of src/lua_patch.cpp (the patch of the game's fan screen script, NOTES.md section 31).  The real script (Shared\Unlocks.lua, Lua 5.1 bytecode) is taken out
// of the game's image by tools/lua_patch_check.py and passed as a file.  The module is included whole so that its loader hook can be called with a made-up reader state.
#include <stdio.h>
#include <algorithm>
#include "../src/lua_patch.cpp"

Config g_cfg;
bool g_logEnabled = true;
void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  log: ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}
void* InstallInlineHook(void*, const BYTE*, size_t, void*)
{
    return NULL;
}

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL line %d: %s -- ", __LINE__, #cond); \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
            g_failures++;                                    \
        }                                                    \
    } while (0)

static const BYTE* g_seen = NULL;
static size_t g_seenSize = 0;
static LuaReaderFn g_seenReader = NULL;
static void* __cdecl FakeUndump(void*, Zio* z, void*, const char*)
{
    g_seen = z->p;
    g_seenSize = z->n;
    g_seenReader = z->reader;
    return (void*)1;
}

// the game's way: a script is given in pieces of 512 bytes
struct Pieces {
    const std::vector<BYTE>* bytes;
    size_t at;
};
static const char* __cdecl PieceReader(void*, void* data, size_t* size)
{
    Pieces* p = (Pieces*)data;
    if (p->at >= p->bytes->size()) {
        *size = 0;
        return NULL;
    }
    *size = std::min<size_t>(512, p->bytes->size() - p->at);
    const char* r = (const char*)p->bytes->data() + p->at;
    p->at += *size;
    return r;
}
static Zio MakeZio(const std::vector<BYTE>& bytes, Pieces& pieces)
{
    pieces.bytes = &bytes;
    pieces.at = std::min<size_t>(512, bytes.size());
    Zio z = {};
    z.n = pieces.at;
    z.p = bytes.data();
    z.reader = PieceReader;
    z.data = &pieces;
    return z;
}

static int CountReads(const Fn& f, const char* key, const char* keyName)
{
    (void)keyName;
    int n = 0;
    int at = FindString(f, key);
    if (at >= 0)
        for (unsigned ins : f.code)
            if ((ins & 63) == kOpGetTable && ((ins >> 14) & 511) == (256u | (unsigned)at))
                n++;
    for (const Fn& p : f.protos)
        n += CountReads(p, key, keyName);
    return n;
}

static bool SameExceptPatched(const Fn& a, const Fn& b, int* changedWords)
{
    if (a.head != b.head || a.tail != b.tail || a.code.size() != b.code.size() || a.protos.size() != b.protos.size())
        return false;
    for (size_t i = 0; i < a.code.size(); i++)
        if (a.code[i] != b.code[i])
            (*changedWords)++;
    for (size_t i = 0; i < a.protos.size(); i++)
        if (!SameExceptPatched(a.protos[i], b.protos[i], changedWords))
            return false;
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: lua_patch_check <Unlocks.lua bytecode file>\n");
        return 2;
    }
    FILE* fp = fopen(argv[1], "rb");
    if (!fp) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<BYTE> original;
    BYTE buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0)
        original.insert(original.end(), buf, buf + got);
    fclose(fp);
    printf("script: %u bytes\n", (unsigned)original.size());

    // the module parses and writes back the same bytes when nothing is changed
    {
        Reader r = {original.data() + 12, original.data() + original.size()};
        Fn top;
        CHECK(ParseFn(r, top, 0), "parse");
        CHECK(r.p == r.end, "all bytes read: %d left", (int)(r.end - r.p));
        std::vector<BYTE> again(original.begin(), original.begin() + 12);
        WriteFn(top, again);
        CHECK(again == original, "write back gives the same bytes (%u vs %u)", (unsigned)again.size(), (unsigned)original.size());
        CHECK(CountReads(top, "ranked_up", "") == 2, "the script has two reads of ranked_up (%d)", CountReads(top, "ranked_up", ""));
    }

    g_origUndump = FakeUndump;
    g_cfg.fanMaxLevel = 99;
    Pieces pieces;
    Zio z = MakeZio(original, pieces);
    H_Undump(NULL, &z, NULL, "@Screens\\Shared\\Unlocks.lua");
    CHECK(g_seen != NULL, "the original loader was called");
    CHECK(g_seen != original.data(), "the loader was given a rewritten script");
    CHECK(z.p == g_seen && z.n == g_seenSize, "the reader state holds the rewritten script");
    CHECK(g_seenReader == EmptyReader, "the reader has nothing more to give");
    if (g_seen && g_seen != original.data()) {
        Reader r = {g_seen + 12, g_seen + g_seenSize};
        Fn patched;
        CHECK(ParseFn(r, patched, 0), "the rewritten script parses");
        CHECK(r.p == r.end, "all bytes of it read");
        Reader r0 = {original.data() + 12, original.data() + original.size()};
        Fn before;
        ParseFn(r0, before, 0);
        int changed = 0;
        CHECK(SameExceptPatched(before, patched, &changed), "same functions, same heads and tails");
        CHECK(changed == 2, "two instruction words changed (%d)", changed);
        CHECK(CountReads(patched, "ranked_up", "") == 0, "no read of ranked_up is left");
        CHECK(CountReads(patched, "hasUnlockedVehicles", "") >= 2, "two reads of hasUnlockedVehicles (%d)", CountReads(patched, "hasUnlockedVehicles", ""));
        CHECK(TreeHasString(patched, "SP_RANK_99") && !TreeHasString(patched, "SP_RANK_25"), "the highest level is named");
        CHECK(TreeHasString(patched, "ranked_up"), "ranked_up is still written by the script");
        CHECK(!memcmp(g_seen, original.data(), 12), "header kept");
    }

    // a cut chunk, and a script with another name, are passed on as they are
    {
        std::vector<BYTE> cut(original.begin(), original.begin() + original.size() / 2);
        Pieces p2;
        Zio z2 = MakeZio(cut, p2);
        g_seen = NULL;
        H_Undump(NULL, &z2, NULL, "@Screens\\Shared\\Unlocks.lua");
        CHECK(g_seen != NULL && g_seenSize == cut.size() && !memcmp(g_seen, cut.data(), cut.size()), "a cut chunk is passed on whole and unchanged (%u of %u)", (unsigned)g_seenSize,
              (unsigned)cut.size());
        Pieces p3;
        Zio z3 = MakeZio(original, p3);
        g_seen = NULL;
        H_Undump(NULL, &z3, NULL, "@Screens\\Shared\\Options.lua");
        CHECK(g_seen == original.data() && g_seenSize == 512 && g_seenReader == PieceReader && p3.at == 512, "another script is not touched");
    }

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
