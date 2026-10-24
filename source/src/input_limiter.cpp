// Menu key repeat.  See input_limiter.h.
#include "input_limiter.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "draw_dispatch.h"
#include "game_mode.h"
#include "inline_hook.h"

static const double kRepeatDelayMs = 500.0; // how long a held key waits before it repeats

bool KeyRepeatShownDown(double heldMs, double intervalMs, double frameMs)
{
    double tap = intervalMs * 0.5;
    if (tap < 2.0 * frameMs)
        tap = 2.0 * frameMs;
    if (heldMs < tap)
        return true; // the press itself
    if (heldMs < kRepeatDelayMs)
        return false;
    return fmod(heldMs - kRepeatDelayMs, 2.0 * tap) < tap;
}

static double NowMs()
{
    static LARGE_INTEGER frequency = {};
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static double g_frameMs = 4.0;  // smoothed time between presents
static bool g_inMenus = true;   // the front end is up (a pause menu in a race is not recognised)
static int g_intervalMs = 60;

// What the game is shown for each key of one source (DirectInput scan codes or virtual-key codes).
class KeyFilter {
public:
    KeyFilter() { Reset(); }
    void Reset()
    {
        for (double& since : downSince_)
            since = -1.0;
    }
    bool Apply(int key, bool physicalDown)
    {
        if (!physicalDown) {
            downSince_[key] = -1.0;
            return false;
        }
        double now = NowMs();
        if (downSince_[key] < 0.0)
            downSince_[key] = now;
        return KeyRepeatShownDown(now - downSince_[key], (double)g_intervalMs, g_frameMs);
    }

private:
    double downSince_[256];
};
static KeyFilter g_dikFilter, g_vkFilter;
static KeyFilter g_padFilter[4]; // XInput, per controller: ids 0-15 = the buttons (bit numbers of wButtons), 16-19 = the stick left / right / up / down

void InputLimiterFrame()
{
    static double last = 0.0;
    double now = NowMs();
    if (last > 0.0) {
        double dt = now - last;
        if (dt > 100.0)
            dt = 100.0; // (a stall is not the frame rate)
        g_frameMs += (dt - g_frameMs) * 0.1;
    }
    last = now;
    RaceMenuOverlayEndFrame();
    if (g_cfg.enabled && !g_cfg.vr && g_cfg.modeKey) {
        // Flat there is no VR code to read the mode key (F8): it cycles auto -> force menu -> force race for the limiter, a way out when the
        // pause menu in a race is not recognised (force menu while it is up, auto again after)
        static bool previous = false;
        bool down = (GetAsyncKeyState(g_cfg.modeKey) & 0x8000) != 0;
        if (down && !previous) {
            g_modeOverride = (g_modeOverride + 1) % 3;
            Log("input: mode key: menu key repeat limit is now %s", g_modeOverride == 0 ? "automatic" : g_modeOverride == 1 ? "forced on" : "forced off");
        }
        previous = down;
    }
    // the front end, or a pause / results menu on top of a race (the mode key can force either way)
    bool menus = g_modeOverride == 0 ? (InMenuScene() || RaceMenuOverlay()) : InMenuScene();
    if (menus != g_inMenus) { // a key held across the change starts over: it is down for the game from the start in a race, a new press in a menu
        g_dikFilter.Reset();
        g_vkFilter.Reset();
        for (KeyFilter& filter : g_padFilter)
            filter.Reset();
        g_inMenus = menus;
    }
}

// Keys that are never filtered: the modifiers and locks, and the mouse buttons (GetKeyState also answers for them).
static bool LimitedDik(int sc)
{
    switch (sc) {
    case 0x1D: case 0x9D: // control
    case 0x2A: case 0x36: // shift
    case 0x38: case 0xB8: // alt
    case 0xDB: case 0xDC: case 0xDD: // windows, menu
    case 0x3A: case 0x45: case 0x46: // caps, num, scroll lock
        return false;
    }
    return true;
}
static bool LimitedVk(int vk)
{
    if (vk >= 0x01 && vk <= 0x06) // mouse buttons
        return false;
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
        return false;
    }
    return vk >= 0;
}

// ---- GetKeyState
static SHORT(WINAPI* g_realGetKeyState)(int) = NULL;
static SHORT WINAPI H_GetKeyState(int vk)
{
    SHORT state = g_realGetKeyState(vk);
    if (vk < 0 || vk > 255 || !LimitedVk(vk))
        return state;
    static unsigned char logged[256];
    if (!logged[vk] && g_logEnabled) {
        static int count = 0;
        if (count++ < 24) {
            logged[vk] = 1;
            Log("input: the game polls GetKeyState(0x%02x)", vk);
        }
    }
    if (!g_inMenus)
        return state;
    bool down = (state & 0x8000) != 0;
    bool shown = g_vkFilter.Apply(vk, down);
    if (down && !shown)
        state = (SHORT)(state & 0x7FFF);
    return state;
}

// ---- XInput (the pad).  Blur imports XInputGetState (ordinal 2 of XINPUT1_3.dll) and polls it every frame like the keyboard, with the same per-frame
// hold counter behind its menus, so the same rule applies: a held button, D-pad direction or push of the left stick reaches the game as a tap, then a tap
// every KeyRepeatMs.  The triggers and the right stick are left alone.
struct XPad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XPad Gamepad;
};
static DWORD(WINAPI* g_realXInputGetState)(DWORD, XState*) = NULL;
static DWORD g_padPacketExtra[4], g_padPicture[4];
static const WORD kPadButtons = 0xF3FF; // the buttons XInput defines (bits 10 and 11 are not used)
static const int kStickThreshold = 7849; // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE

static void LimitPadState(DWORD index, XState* state)
{
    KeyFilter& filter = g_padFilter[index];
    XPad& pad = state->Gamepad;
    WORD shown = pad.wButtons;
    for (int bit = 0; bit < 16; bit++) {
        WORD mask = (WORD)(1u << bit);
        if (!(kPadButtons & mask))
            continue;
        bool down = (pad.wButtons & mask) != 0;
        if (!filter.Apply(bit, down) && down)
            shown = (WORD)(shown & ~mask);
    }
    pad.wButtons = shown;
    // the stick is analog: a push beyond the dead zone is a direction held, and while that direction is not shown the axis reads centred
    bool left = pad.sThumbLX < -kStickThreshold, right = pad.sThumbLX > kStickThreshold;
    bool up = pad.sThumbLY > kStickThreshold, down = pad.sThumbLY < -kStickThreshold;
    bool showLeft = filter.Apply(16, left), showRight = filter.Apply(17, right), showUp = filter.Apply(18, up), showDown = filter.Apply(19, down);
    bool xCut = (left && !showLeft) || (right && !showRight), yCut = (up && !showUp) || (down && !showDown);
    if (xCut)
        pad.sThumbLX = 0;
    if (yCut)
        pad.sThumbLY = 0;
    // the packet number says the state changed: it has to change when what the game is shown does
    DWORD picture = shown | (xCut ? 0x10000u : 0) | (yCut ? 0x20000u : 0);
    if (picture != g_padPicture[index]) {
        g_padPicture[index] = picture;
        g_padPacketExtra[index]++;
    }
}
static DWORD WINAPI H_XInputGetState(DWORD index, XState* state)
{
    DWORD result = g_realXInputGetState(index, state);
    if (result != ERROR_SUCCESS || !state || index > 3)
        return result;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("input: the game polls the XInput pad (XInputGetState, controller %lu)", (unsigned long)index);
    }
    if (g_logEnabled) {
        // (Log=1) the pad's button presses next to what the limiter believes, to follow it through a menu
        static WORD previous[4];
        static int count = 0;
        WORD edges = (WORD)(state->Gamepad.wButtons & ~previous[index]);
        if (edges && count++ < 80)
            Log("input: pad button 0x%04x pressed (limiter %s, race menu %s)", edges, g_inMenus ? "on" : "off", RaceMenuOverlay() ? "up" : "not seen");
        previous[index] = state->Gamepad.wButtons;
    }
    if (g_inMenus)
        LimitPadState(index, state);
    state->dwPacketNumber += g_padPacketExtra[index];
    return result;
}

// ---- DirectInput.  The interfaces are called through their vtables, so no DirectInput header is needed.
struct DiVtables { // the A and W flavours of the two interfaces have vtables of their own
    void** vtable;
    void* original;
};
struct DevVtables {
    void** vtable;
    void* getDeviceState;
    void* getDeviceData;
};
static DiVtables g_diCreateDevice[2];
static DevVtables g_devVtables[4];
static int g_diCount = 0, g_devCount = 0;

typedef HRESULT(STDMETHODCALLTYPE* PFN_DICreateDevice)(void*, const GUID*, void**, void*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DIGetDeviceState)(void*, DWORD, void*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DIGetDeviceData)(void*, DWORD, void*, DWORD*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DIGetCapabilities)(void*, void*);

static const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kIidDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const GUID kIidDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

static std::vector<void*> g_keyboardDevices; // devices known to be keyboards (for the log)

static const DevVtables* FindDev(void* self)
{
    void** vt = *(void***)self;
    for (int i = 0; i < g_devCount; i++)
        if (g_devVtables[i].vtable == vt)
            return &g_devVtables[i];
    return NULL;
}
static bool IsKnownKeyboard(void* self)
{
    for (void* device : g_keyboardDevices)
        if (device == self)
            return true;
    return false;
}

static HRESULT STDMETHODCALLTYPE H_DIGetDeviceState(void* self, DWORD size, void* data)
{
    const DevVtables* dev = FindDev(self);
    if (!dev)
        return E_FAIL;
    HRESULT hr = ((PFN_DIGetDeviceState)dev->getDeviceState)(self, size, data);
    if (FAILED(hr) || size != 256 || !data) // (the keyboard's state is 256 bytes, one per scan code; the mouse and pads have other sizes)
        return hr;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("input: the game polls the DirectInput keyboard (GetDeviceState)%s", IsKnownKeyboard(self) ? "" : " [a 256-byte device that was not recognised as a keyboard]");
    }
    if (!g_inMenus)
        return hr;
    BYTE* keys = (BYTE*)data;
    if (g_logEnabled) {
        // (Log=1) the pause key and the menu keys, next to what the limiter believes, so a log shows whether it followed the menu
        static bool previous[3];
        static const int probed[3] = {0x01, 0x1C, 0x0E}; // escape, enter, backspace
        for (int i = 0; i < 3; i++) {
            bool now = (keys[probed[i]] & 0x80) != 0;
            if (now && !previous[i])
                Log("input: key 0x%02x pressed (limiter %s, race menu %s)", probed[i], g_inMenus ? "on" : "off", RaceMenuOverlay() ? "up" : "not seen");
            previous[i] = now;
        }
    }
    for (int sc = 0; sc < 256; sc++) {
        if (!LimitedDik(sc))
            continue;
        bool down = (keys[sc] & 0x80) != 0;
        bool shown = g_dikFilter.Apply(sc, down);
        if (down && !shown)
            keys[sc] &= 0x7F;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_DIGetDeviceData(void* self, DWORD objectSize, void* data, DWORD* inOut, DWORD flags)
{
    const DevVtables* dev = FindDev(self);
    if (!dev)
        return E_FAIL;
    static bool logged = false;
    if (!logged && IsKnownKeyboard(self)) {
        logged = true;
        Log("input: the game reads the DirectInput keyboard buffered (GetDeviceData); key repeat is NOT limited on that path");
    }
    return ((PFN_DIGetDeviceData)dev->getDeviceData)(self, objectSize, data, inOut, flags);
}

static void PatchDevice(void* device)
{
    void** vt = *(void***)device;
    if (!FindDev(device) && g_devCount < 4) {
        DevVtables& entry = g_devVtables[g_devCount++];
        entry.vtable = vt;
        entry.getDeviceState = vt[9]; // IDirectInputDevice8: 3 GetCapabilities ... 9 GetDeviceState, 10 GetDeviceData
        entry.getDeviceData = vt[10];
        void* ignored;
        PatchVtable(vt, 9, (void*)H_DIGetDeviceState, &ignored);
        PatchVtable(vt, 10, (void*)H_DIGetDeviceData, &ignored);
    }
}

static HRESULT STDMETHODCALLTYPE H_DICreateDevice(void* self, const GUID* guid, void** device, void* outer)
{
    void** vt = *(void***)self;
    void* original = NULL;
    for (int i = 0; i < g_diCount; i++)
        if (g_diCreateDevice[i].vtable == vt)
            original = g_diCreateDevice[i].original;
    if (!original)
        return E_FAIL;
    HRESULT hr = ((PFN_DICreateDevice)original)(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device) {
        bool keyboard = guid && !memcmp(guid, &kSysKeyboard, sizeof(GUID));
        if (!keyboard) { // an instance GUID from an enumeration: ask the device
            DWORD caps[11] = {44};
            void** dvt = *(void***)*device;
            if (SUCCEEDED(((PFN_DIGetCapabilities)dvt[3])(*device, caps)))
                keyboard = (caps[2] & 0xFF) == 0x13; // DI8DEVTYPE_KEYBOARD
        }
        if (keyboard)
            g_keyboardDevices.push_back(*device);
        Log("input: DirectInput device created%s", keyboard ? " (keyboard)" : "");
        PatchDevice(*device);
    }
    return hr;
}

static HRESULT(WINAPI* g_realDirectInput8Create)(HINSTANCE, DWORD, const GUID*, void**, void*) = NULL;
static HRESULT WINAPI H_DirectInput8Create(HINSTANCE instance, DWORD version, const GUID* iid, void** out, void* outer)
{
    HRESULT hr = g_realDirectInput8Create(instance, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out && iid && (!memcmp(iid, &kIidDirectInput8A, sizeof(GUID)) || !memcmp(iid, &kIidDirectInput8W, sizeof(GUID)))) {
        void** vt = *(void***)*out;
        bool known = false;
        for (int i = 0; i < g_diCount; i++)
            known = known || g_diCreateDevice[i].vtable == vt;
        if (!known && g_diCount < 2) {
            g_diCreateDevice[g_diCount].vtable = vt;
            g_diCreateDevice[g_diCount].original = vt[3]; // IDirectInput8: 3 CreateDevice
            g_diCount++;
            void* ignored;
            PatchVtable(vt, 3, (void*)H_DICreateDevice, &ignored);
        }
    }
    return hr;
}

// ---- import patching (the executable's own import table): PatchImport, inline_hook.cpp

// XInput is imported by ordinal: find the entry that holds XInputGetState by its address (the loader has bound the table by now)
static bool PatchXInput(HMODULE module)
{
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva)
        return false;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva); imp->Name; imp++) {
        const char* dll = (const char*)(base + imp->Name);
        if (_strnicmp(dll, "xinput", 6))
            continue;
        HMODULE xinput = GetModuleHandleA(dll);
        void* target = xinput ? (void*)GetProcAddress(xinput, "XInputGetState") : NULL;
        if (!target)
            continue;
        for (IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); ft->u1.Function; ft++) {
            if ((void*)ft->u1.Function == (void*)H_XInputGetState)
                return true;
            if ((void*)ft->u1.Function != target)
                continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            g_realXInputGetState = (DWORD(WINAPI*)(DWORD, XState*))target;
            ft->u1.Function = (ULONG_PTR)H_XInputGetState;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void InstallInputHooks()
{
    g_intervalMs = g_cfg.keyRepeatMs;
    if (!g_cfg.enabled || g_cfg.keyRepeatMs <= 0)
        return;
    HMODULE exe = GetModuleHandleA(NULL);
    static bool keyStateDone = false, directInputDone = false, padDone = false;
    if (!keyStateDone)
        keyStateDone = PatchImport(exe, "USER32.dll", "GetKeyState", (void*)H_GetKeyState, (void**)&g_realGetKeyState);
    if (!directInputDone)
        directInputDone = PatchImport(exe, "DINPUT8.dll", "DirectInput8Create", (void*)H_DirectInput8Create, (void**)&g_realDirectInput8Create);
    if (!padDone)
        padDone = PatchXInput(exe);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("input: menu key repeat limited to one press every %d ms (GetKeyState %s, DirectInput %s, XInput pad %s)", g_cfg.keyRepeatMs,
            keyStateDone ? "hooked" : "not imported", directInputDone ? "hooked" : "not imported", padDone ? "hooked" : "not imported");
    }
}
