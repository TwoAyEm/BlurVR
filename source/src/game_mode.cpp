// Menu / race detection: the game opening files below "levels\" tells us which scene is loading.
#include "game_mode.h"
#include "core.h"

volatile bool g_levelFilesSeen = false;
int g_modeOverride = 0; // 0 auto, 1 menu, 2 race
volatile bool g_raceStartPhase = false;
unsigned long long g_raceStartTime =
    0; // just switched to 3D: a loading video may still be drawn into the HUD layer
unsigned long long g_levelLoadStartTime = 0;
volatile bool g_levelLoading =
    true; // a level is being loaded: show the floating screen until a 3D scene is drawing
unsigned g_sceneProbeDraws = 0;
int g_sceneProbeFrames = 0;
bool InMenuScene()
{
    if (g_modeOverride == 1)
        return true;
    if (g_modeOverride == 2)
        return false;
    return !g_levelFilesSeen || strcmp(g_currentLevel, "showroom") == 0;
}
bool IsRaceMode()
{
    return g_modeOverride == 2 || (g_modeOverride == 0 && g_levelFilesSeen && !g_levelLoading);
}

typedef HANDLE(WINAPI* PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_CreateFileA g_realCreateFileA;
static PFN_CreateFileW g_realCreateFileW;
static int g_levelLogCount;

char g_currentLevel[64];

volatile unsigned long long g_photoOpenTime = 0;

// 0 = no movie, 1 = a movie file was opened (waiting for the 3D scene to stop), 2 = the movie is on screen
static volatile int g_moviePhase = 0;
static volatile unsigned long long g_movieOpenTime = 0;
static int g_movieLogCount;

bool IsMovieFrame()
{
    return g_moviePhase == 2;
}

void UpdateMovieState(bool sceneDrawn)
{
    if (g_moviePhase == 1) {
        if (!sceneDrawn) {
            g_moviePhase = 2;
            Log("movie: the 3D scene stopped, the UI stays hidden while the movie plays");
        } else if (GetTickCount64() - g_movieOpenTime > 8000)
            g_moviePhase = 0; // the movie never took over the screen
    } else if (g_moviePhase == 2 && sceneDrawn) {
        g_moviePhase = 0;
        Log("movie: over (the 3D scene is back)");
    }
}

static void OnGameFileOpen(const char* path)
{
    if (!path || !path[0])
        return;
    char lowered[300];
    size_t length = 0;
    for (; path[length] && length < sizeof(lowered) - 1; length++) {
        char character = path[length];
        if (character == '/')
            character = '\\';
        lowered[length] = (char)tolower((unsigned char)character);
    }
    lowered[length] = 0;
    if (length > 4 && !strcmp(lowered + length - 4, ".bik")) { // a Bink movie: tutorial videos, cutscenes, credits
        g_movieOpenTime = GetTickCount64();
        if (g_moviePhase == 0)
            g_moviePhase = 1;
        if (g_movieLogCount++ < 100)
            Log("movie opened: %s", lowered);
        return;
    }
    if (strstr(lowered, "\\photo\\")) { // the game's photo folder (captured side-by-side images)
        g_photoOpenTime = GetTickCount64();
        static int photoLogCount = 0;
        if (photoLogCount++ < 50)
            Log("photo file opened: %s", lowered);
    }
    const char* levelPath = NULL;
    const char* levelsDir = strstr(lowered, "\\levels\\");
    if (levelsDir)
        levelPath = levelsDir + 8;
    else if (!strncmp(lowered, "levels\\", 7))
        levelPath = lowered + 7;
    if (!levelPath)
        return;
    char levelName[64];
    size_t nameLength = 0;
    while (levelPath[nameLength] && levelPath[nameLength] != '\\' && nameLength < sizeof(levelName) - 1) {
        levelName[nameLength] = levelPath[nameLength];
        nameLength++;
    }
    levelName[nameLength] = 0;
    if (!nameLength || !levelPath[nameLength] /*file directly in levels\*/ || strchr(levelName, '.'))
        return;
    if (strcmp(levelName, g_currentLevel) != 0) {
        strcpy(g_currentLevel, levelName);
        g_levelFilesSeen =
            true; // every level (showroom included) is a 3D scene; only the loading screen is shown boxed
        g_levelLoading = true;
        g_levelLoadStartTime = GetTickCount64();
        g_sceneProbeFrames = 0;
        if (g_levelLogCount++ < 200)
            Log("level files: \"%s\" -> loading (floating screen until the 3D scene is up) (from %s)", levelName,
                lowered);
    }
}
static HANDLE WINAPI H_CreateFileA(LPCSTR f, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD fl, HANDLE t)
{
    OnGameFileOpen(f);
    return g_realCreateFileA(f, a, s, sa, d, fl, t);
}
static HANDLE WINAPI H_CreateFileW(LPCWSTR f, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD fl, HANDLE t)
{
    if (f) {
        char tmp[300];
        int n = WideCharToMultiByte(CP_ACP, 0, f, -1, tmp, sizeof(tmp) - 1, NULL, NULL);
        if (n > 0) {
            tmp[n < 300 ? n : 299] = 0;
            OnGameFileOpen(tmp);
        }
    }
    return g_realCreateFileW(f, a, s, sa, d, fl, t);
}

static void PatchModuleImports(HMODULE mod)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva)
        return;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva); imp->Name; imp++) {
        const char* down = (const char*)(base + imp->Name);
        if (_strnicmp(down, "kernel32", 8) && _strnicmp(down, "api-ms-win-core-file", 20))
            continue;
        if (!imp->OriginalFirstThunk)
            continue;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))
                continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            void* hook = NULL;
            if (!strcmp((const char*)ibn->Name, "CreateFileA"))
                hook = (void*)H_CreateFileA;
            else if (!strcmp((const char*)ibn->Name, "CreateFileW"))
                hook = (void*)H_CreateFileW;
            if (!hook)
                continue;
            DWORD old;
            if (VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                ft->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            }
        }
    }
}

void InstallFileOpenHooks()
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_realCreateFileA = (PFN_CreateFileA)GetProcAddress(k32, "CreateFileA");
    g_realCreateFileW = (PFN_CreateFileW)GetProcAddress(k32, "CreateFileW");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    int n = 0;
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me;
        me.dwSize = sizeof(me);
        for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
            if (me.hModule == g_selfModule || me.hModule == k32)
                continue;
            PatchModuleImports(me.hModule);
            n++;
        }
        CloseHandle(snap);
    }
    Log("file-open hooks installed in %d modules", n);
}
