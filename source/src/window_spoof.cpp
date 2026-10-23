#include "window_spoof.h"
#include <tlhelp32.h>
#include "config.h"
#include "core.h"

static bool g_deviceCreated = false;
static bool g_deviceWindowed = true; // as the game last asked (before the first device: what its settings say)
static bool g_savedKnown = false;
static UINT g_savedWidth = 0, g_savedHeight = 0;
static UINT g_intendedWidth = 0, g_intendedHeight = 0; // what the game asked for last (starts as the saved mode)
static HWND g_gameWindow = NULL;
static WNDPROC g_gameProc = NULL;
static bool g_gameProcUnicode = false;

// Is a window with this real client size one that Windows made much smaller than the game wanted?
static bool ClampedSize(LONG width, LONG height)
{
    return g_intendedWidth && g_cfg.startAtSavedSize && width >= 640 && height >= 360 &&
           ((UINT)width * 10 < g_intendedWidth * 9 || (UINT)height * 10 < g_intendedHeight * 9);
}

bool ReadSavedVideoSize(UINT* width, UINT* height, bool* windowed)
{
    char appData[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appData, sizeof(appData));
    if (!n || n >= sizeof(appData))
        return false;
    std::string path = std::string(appData) + "/bizarre creations/blur/settings.xml";
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
        return false;
    char text[16384];
    size_t got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = 0;
    char* tag = strstr(text, "<shared_graphical_settings");
    if (!tag)
        return false;
    char* end = strchr(tag, '>');
    if (end)
        *end = 0;
    char* w = strstr(tag, " width=\"");
    char* h = strstr(tag, " height=\"");
    if (!w || !h)
        return false;
    int wi = atoi(w + 8), hi = atoi(h + 9);
    if (wi < 320 || hi < 240 || wi > 16384 || hi > 16384)
        return false;
    *width = (UINT)wi;
    *height = (UINT)hi;
    if (windowed) {
        char* mode = strstr(tag, " windowed=\"");
        *windowed = !mode || atoi(mode + 11) != 0;
    }
    return true;
}

// ---- the hooked user32 functions: the window's size as the game reads it, and the creation of its window
static BOOL(WINAPI* g_realGetClientRect)(HWND, LPRECT);
static HWND(WINAPI* g_realCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static HWND(WINAPI* g_realCreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);

// The work area (the monitor without the taskbar) of the monitor a window, or a point, is on.
static bool WorkAreaOfWindow(HWND window, RECT* work)
{
    MONITORINFO info;
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info))
        return false;
    *work = info.rcWork;
    return true;
}
static bool WorkAreaOfPrimary(RECT* work)
{
    MONITORINFO info;
    info.cbSize = sizeof(info);
    POINT origin = {0, 0};
    if (!GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info))
        return false;
    *work = info.rcWork;
    return true;
}

// The game's window procedure, wrapped: its WM_SIZE says the window is as small as Windows made it, and the game answers with a
// Reset to that size a moment after the start.
static LRESULT CALLBACK GameProcHook(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_WINDOWPOSCHANGING && g_cfg.fitDesktopWindow && g_deviceWindowed && lParam) {
        // a size the game asks for (the video mode, plus the frame): never more than fits on the monitor
        WINDOWPOS* pos = (WINDOWPOS*)lParam;
        RECT work;
        if (!(pos->flags & SWP_NOSIZE) && WorkAreaOfWindow(window, &work)) {
            if (pos->cx > work.right - work.left)
                pos->cx = work.right - work.left;
            if (pos->cy > work.bottom - work.top)
                pos->cy = work.bottom - work.top;
        }
    }
    if (message == WM_SIZE && wParam != SIZE_MINIMIZED && ClampedSize(LOWORD(lParam), HIWORD(lParam)))
        lParam = MAKELPARAM(g_intendedWidth, g_intendedHeight);
    return g_gameProcUnicode ? CallWindowProcW(g_gameProc, window, message, wParam, lParam)
                             : CallWindowProcA(g_gameProc, window, message, wParam, lParam);
}
static void WrapGameWindow(HWND window, DWORD style, int width)
{
    if (!window || g_gameWindow || !g_cfg.startAtSavedSize || !(style & WS_CAPTION) || width < 640)
        return;
    g_gameProcUnicode = IsWindowUnicode(window) != FALSE;
    g_gameProc = (WNDPROC)(g_gameProcUnicode ? SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)GameProcHook)
                                             : SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)GameProcHook));
    g_gameWindow = g_gameProc ? window : NULL;
    Log("game window %p: window procedure wrapped (%s)", (void*)window, g_gameWindow ? "ok" : "FAILED");
}

static BOOL WINAPI H_GetClientRect(HWND window, LPRECT rect)
{
    BOOL ok = g_realGetClientRect(window, rect);
    if (!ok || !rect)
        return ok;
    LONG width = rect->right - rect->left, height = rect->bottom - rect->top;
    if (GetParent(window) == NULL && ClampedSize(width, height)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("the game's window is %ldx%ld: it is told %ux%u (the size it asked for)", width, height, g_intendedWidth, g_intendedHeight);
        }
        rect->right = rect->left + (LONG)g_intendedWidth;
        rect->bottom = rect->top + (LONG)g_intendedHeight;
    }
    return ok;
}
// The size and place a game window is created with: the requested size, cut to the primary monitor's work area when it is bigger
// (a window with a caption that Windows would otherwise stretch over the whole desktop).
static void FitNewWindow(DWORD style, int* x, int* y, int* w, int* h)
{
    RECT work;
    if (!g_cfg.fitDesktopWindow || !g_deviceWindowed || !(style & WS_CAPTION) || *w < 640 || !WorkAreaOfPrimary(&work))
        return;
    int workW = work.right - work.left, workH = work.bottom - work.top;
    if (*w <= workW && *h <= workH)
        return;
    Log("the game's window (%dx%d requested) is made %dx%d, the work area of the monitor", *w, *h, *w > workW ? workW : *w,
        *h > workH ? workH : *h);
    if (*w > workW)
        *w = workW;
    if (*h > workH)
        *h = workH;
    *x = work.left; // (the position of a window that was too big to fit is the corner of the work area)
    *y = work.top;
}
static HWND WINAPI H_CreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent,
                                     HMENU menu, HINSTANCE inst, LPVOID param)
{
    if (!parent)
        FitNewWindow(style, &x, &y, &w, &h);
    HWND window = g_realCreateWindowExA(exStyle, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (!parent)
        WrapGameWindow(window, style, w);
    return window;
}
static HWND WINAPI H_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h, HWND parent,
                                     HMENU menu, HINSTANCE inst, LPVOID param)
{
    if (!parent)
        FitNewWindow(style, &x, &y, &w, &h);
    HWND window = g_realCreateWindowExW(exStyle, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (!parent)
        WrapGameWindow(window, style, w);
    return window;
}

struct HookEntry {
    const char* name;
    void* hook;
    void** real;
};
static HookEntry g_hooks[] = {
    {"GetClientRect", (void*)H_GetClientRect, (void**)&g_realGetClientRect},
    {"CreateWindowExA", (void*)H_CreateWindowExA, (void**)&g_realCreateWindowExA},
    {"CreateWindowExW", (void*)H_CreateWindowExW, (void**)&g_realCreateWindowExW},
};

static void PatchWindowImports(HMODULE mod)
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
        if (_strnicmp((const char*)(base + imp->Name), "user32", 6) || !imp->OriginalFirstThunk)
            continue;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))
                continue;
            const char* name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData))->Name;
            for (const HookEntry& entry : g_hooks) {
                if (strcmp(name, entry.name) || (void*)ft->u1.Function == entry.hook)
                    continue;
                DWORD old;
                if (VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    ft->u1.Function = (ULONG_PTR)entry.hook;
                    VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
                }
            }
        }
    }
}

void InstallWindowSizeSpoof(bool allModules)
{
    if (!g_cfg.vr || !g_cfg.startAtSavedSize || g_deviceCreated)
        return;
    static bool prepared = false;
    if (!prepared) {
        prepared = true;
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32)
            return;
        for (const HookEntry& entry : g_hooks)
            *entry.real = (void*)GetProcAddress(user32, entry.name);
        g_savedKnown = ReadSavedVideoSize(&g_savedWidth, &g_savedHeight, &g_deviceWindowed);
        if (g_cfg.startWidth >= 640 && g_cfg.startHeight >= 360) { // the game saves whatever size it was last reset to, clamped ones too
            g_savedKnown = true;
            g_savedWidth = (UINT)g_cfg.startWidth;
            g_savedHeight = (UINT)g_cfg.startHeight;
        }
        if (g_savedKnown) {
            g_intendedWidth = g_savedWidth;
            g_intendedHeight = g_savedHeight;
        }
        Log("window size spoof: start size %s (%ux%u)", g_savedKnown ? (g_cfg.startWidth ? "from BlurVR.ini" : "from settings.xml") : "not found",
            g_savedWidth, g_savedHeight);
    }
    for (const HookEntry& entry : g_hooks)
        if (!*entry.real)
            return;
    if (!allModules) {
        PatchWindowImports(GetModuleHandleA(NULL));
        return;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
        if (me.hModule != g_selfModule)
            PatchWindowImports(me.hModule);
    CloseHandle(snap);
}

void WindowSpoofIntendedSize(UINT width, UINT height)
{
    if (width < 320 || height < 240 || (width == g_intendedWidth && height == g_intendedHeight))
        return;
    g_intendedWidth = width;
    g_intendedHeight = height;
    Log("window size spoof: the game's size is now %ux%u", width, height);
}

void WindowSpoofWindowed(bool windowed)
{
    g_deviceWindowed = windowed;
}

void WindowSpoofDeviceCreated()
{
    g_deviceCreated = true;
}
