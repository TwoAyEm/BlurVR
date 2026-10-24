// Host check of src/desktop_message.cpp (NOTES.md section 20, [VR] DesktopView=off): the "Playing in VR" picture (layout, the logo taken from the game's executable) and, on a
// real Direct3D 9 device in a hidden window, the path that puts it into the back buffer (a surface in the back buffer's format, StretchRect into the corner, the rectangle
// Present takes, when a Present is due).  Built and run by tools/desktop_message_check.py, which passes the game's executable and a folder for the pictures.
#include <stdio.h>
#include "config.h"
#include "core.h"
#include "desktop_message.h"
#include "display_modes.h"
#include "vr_runtime.h"

// ---- what the module takes from the rest of the mod ---------------------------------------------------------------------------------------------------------
Config g_cfg;
VrState g_vr;
bool g_desktopRectUsable = false, g_desktopRectFailed = false;
void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  log: ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

static int g_failures = 0;
#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL line %d: %s -- ", __LINE__, #cond);       \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static bool WriteBmp(const char* path, int width, int height, const std::vector<unsigned>& pixels)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + (DWORD)(pixels.size() * 4);
    fwrite(&file, sizeof(file), 1, f);
    fwrite(&info, sizeof(info), 1, f);
    fwrite(pixels.data(), 4, pixels.size(), f);
    fclose(f);
    return true;
}

// the bounding box of the pixels in rows [y0, y1) that are far from the row's background
struct Box {
    int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1;
    bool empty() const { return right < 0; }
};
static int Distance(unsigned a, unsigned b)
{
    int d = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int x = (int)((a >> shift) & 255) - (int)((b >> shift) & 255);
        d += x < 0 ? -x : x;
    }
    return d;
}
static Box Content(const std::vector<unsigned>& pixels, int width, int y0, int y1, int threshold)
{
    Box box;
    for (int y = y0; y < y1; y++) {
        const unsigned background = pixels[(size_t)y * width]; // (the picture's left edge is background)
        for (int x = 0; x < width; x++)
            if (Distance(pixels[(size_t)y * width + x], background) > threshold) {
                box.left = x < box.left ? x : box.left;
                box.right = x > box.right ? x : box.right;
                box.top = y < box.top ? y : box.top;
                box.bottom = y > box.bottom ? y : box.bottom;
            }
    }
    return box;
}

static void CheckPicture(int width, int height, HICON logo, const char* name, const char* folder)
{
    std::vector<unsigned> pixels;
    CHECK(DesktopMessageBuildPicture(width, height, logo, pixels) && pixels.size() == (size_t)width * height, "%s: picture built", name);
    if (pixels.size() != (size_t)width * height)
        return;
    bool opaque = true;
    for (unsigned p : pixels)
        opaque = opaque && (p >> 24) == 0xFF;
    CHECK(opaque, "%s: every pixel has the top byte set", name);
    const Box all = Content(pixels, width, 0, height, 40);
    CHECK(!all.empty(), "%s: something is drawn", name);
    if (all.empty())
        return;
    const int marginLeft = all.left, marginRight = width - 1 - all.right, marginTop = all.top, marginBottom = height - 1 - all.bottom;
    CHECK(abs(marginLeft - marginRight) <= width / 3, "%s: content is in the middle sideways (%d | %d)", name, marginLeft, marginRight);
    CHECK(abs(marginTop - marginBottom) <= height / 12, "%s: content is in the middle vertically (%d | %d)", name, marginTop, marginBottom); // (the text's box has room for descenders)
    // the text is the widest part: its box must be centred
    const Box text = Content(pixels, width, all.bottom - height / 12 > all.top ? all.bottom - height / 12 : all.top, all.bottom + 1, 120);
    CHECK(!text.empty() && abs(text.left - (width - 1 - text.right)) <= width / 100 + 4, "%s: the text is centred (%d | %d)", name, text.left, width - 1 - text.right);
    if (logo) {
        // above the text: a box of the logo's own, centred, brighter than the background somewhere
        const Box top = Content(pixels, width, all.top, all.top + (all.bottom - all.top) * 11 / 20, 60);
        CHECK(!top.empty() && abs(top.left - (width - 1 - top.right)) <= 3, "%s: the logo is centred (%d | %d)", name, top.left, width - 1 - top.right);
        CHECK(!top.empty() && top.right - top.left >= 48 && top.bottom - top.top >= 48, "%s: the logo is at least 48 pixels (%d x %d)", name, top.right - top.left, top.bottom - top.top);
    }
    if (folder) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s.bmp", folder, name);
        WriteBmp(path, width, height, pixels);
    }
}

// ---- a real device ------------------------------------------------------------------------------------------------------------------------------------------
static LRESULT CALLBACK CheckProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return DefWindowProcA(window, message, w, l);
}

// ex: a Direct3D 9Ex device, which is what the mod runs the game on when SharedTransport is on (the default)
static void CheckDevice(HICON logo, const char* folder, bool ex)
{
    printf("%s device:\n", ex ? "9Ex" : "9");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = CheckProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "desktop_message_check";
    RegisterClassA(&wc);
    RECT outer = {0, 0, 800, 450};
    AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA(wc.lpszClassName, "desktop_message_check", WS_OVERLAPPEDWINDOW, 100, 100, outer.right - outer.left, outer.bottom - outer.top, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    IDirect3D9* d3d = NULL;
    if (ex) {
        IDirect3D9Ex* d3dEx = NULL;
        if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dEx)))
            d3d = d3dEx;
    } else
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = NULL;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY; // (what AdjustPresentParams asks for)
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1600; // (bigger than the window: the corner is what is presented)
    pp.BackBufferHeight = 900;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr = E_FAIL;
    if (d3d && ex) {
        IDirect3DDevice9Ex* deviceEx = NULL;
        hr = ((IDirect3D9Ex*)d3d)->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, NULL, &deviceEx);
        device = deviceEx;
    } else if (d3d)
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (FAILED(hr) || !device) {
        printf("SKIP the device checks: no Direct3D 9 device here (0x%08x)\n", (unsigned)hr);
        if (d3d)
            d3d->Release();
        DestroyWindow(window);
        return;
    }
    g_cfg.vr = 1;
    g_cfg.desktopView = kDesktopViewOff;
    g_vr.active = true;
    g_desktopRectUsable = true;
    g_desktopRectFailed = false;
    CHECK(DesktopMessageWanted(), "wanted: DesktopView=off, VR active, windowed");
    g_vr.active = false;
    CHECK(!DesktopMessageWanted(), "not wanted while the headset is not running");
    g_vr.active = true;
    g_desktopRectUsable = false;
    CHECK(!DesktopMessageWanted(), "not wanted when Present cannot take a rectangle");
    g_desktopRectUsable = true;
    g_cfg.desktopView = 1;
    CHECK(!DesktopMessageWanted(), "not wanted with DesktopView=left");
    g_cfg.desktopView = kDesktopViewOff;

    CHECK(DesktopMessageDue(device), "the first Present is due");
    RECT rect = {0, 0, 0, 0};
    CHECK(DesktopMessagePaint(device, &rect), "painted");
    CHECK(rect.left == 0 && rect.top == 0 && rect.right == 800 && rect.bottom == 450, "the rectangle is the window's client size in the corner (%ld %ld %ld %ld)", rect.left, rect.top, rect.right, rect.bottom);
    CHECK(!DesktopMessageDue(device), "no Present is due right after one");

    // what is in the back buffer: the picture, pixel for pixel
    IDirect3DSurface9 *backBuffer = NULL, *copy = NULL;
    CHECK(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)), "back buffer");
    CHECK(SUCCEEDED(device->CreateOffscreenPlainSurface(1600, 900, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, NULL)), "readback surface");
    if (backBuffer && copy && SUCCEEDED(device->GetRenderTargetData(backBuffer, copy))) {
        std::vector<unsigned> expected;
        DesktopMessageBuildPicture(800, 450, DesktopMessageLoadLogo(NULL), expected);
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(copy->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
            size_t different = 0;
            for (int y = 0; y < 450; y++)
                for (int x = 0; x < 800; x++) {
                    const unsigned got = *(const unsigned*)((const BYTE*)locked.pBits + (size_t)y * locked.Pitch + (size_t)x * 4) & 0x00FFFFFF;
                    if (got != (expected[(size_t)y * 800 + x] & 0x00FFFFFF))
                        different++;
                }
            CHECK(different == 0, "the corner of the back buffer is the picture (%zu pixels differ)", different);
            if (folder) {
                std::vector<unsigned> corner(800 * 450);
                for (int y = 0; y < 450; y++)
                    memcpy(&corner[(size_t)y * 800], (const BYTE*)locked.pBits + (size_t)y * locked.Pitch, 800 * 4);
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s\\device_backbuffer_corner%s.bmp", folder, ex ? "_ex" : "");
                WriteBmp(path, 800, 450, corner);
            }
            const unsigned beyond = *(const unsigned*)((const BYTE*)locked.pBits + (size_t)600 * locked.Pitch + 1200 * 4);
            CHECK(beyond != 0xFFFFFFFFu, "(the rest of the back buffer is not painted: %08x)", beyond);
            copy->UnlockRect();
        }
    } else
        CHECK(false, "the back buffer could not be read back");
    if (copy)
        copy->Release();
    if (backBuffer)
        backBuffer->Release();
    CHECK(SUCCEEDED(device->Present(&rect, NULL, NULL, NULL)), "Present with the rectangle as its source");

    // the window's size changes: due again, and painted at the new size
    SetWindowPos(window, NULL, 0, 0, outer.right - outer.left + 200, outer.bottom - outer.top + 100, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(DesktopMessageDue(device), "due after the window was resized");
    CHECK(DesktopMessagePaint(device, &rect) && rect.right == 1000 && rect.bottom == 550, "painted at the new size (%ld x %ld)", rect.right, rect.bottom);
    // a window bigger than the back buffer: scaled down, the same in both directions
    SetWindowPos(window, NULL, 0, 0, 3200 + (outer.right - outer.left - 800), 900 + (outer.bottom - outer.top - 450), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(DesktopMessagePaint(device, &rect) && rect.right == 1600 && rect.bottom == 450, "a window of 3200 x 900 gives 1600 x 450 (%ld x %ld)", rect.right, rect.bottom);
    // minimised: nothing to show
    ShowWindow(window, SW_MINIMIZE);
    CHECK(!DesktopMessageDue(device), "nothing is due for a minimised window");
    ShowWindow(window, SW_RESTORE);

    // after a Reset the surfaces are made again
    ReleaseDesktopMessage();
    CHECK(DesktopMessageDue(device), "due again after the surfaces were released");
    CHECK(DesktopMessagePaint(device, &rect), "painted again");

    ReleaseDesktopMessage();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
}

int main(int argc, char** argv)
{
    const char* exePath = argc > 1 ? argv[1] : NULL;
    const char* folder = argc > 2 ? argv[2] : NULL;
    HICON logo = NULL;
    if (exePath) {
        HMODULE exe = LoadLibraryExA(exePath, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (exe)
            logo = DesktopMessageLoadLogo(exe);
        printf("logo from %s: %s\n", exePath, logo ? "found" : "none");
    }
    g_cfg.vr = 1;
    CheckPicture(1280, 720, logo, "picture_1280x720_logo", folder);
    CheckPicture(1920, 1080, logo, "picture_1920x1080_logo", folder);
    CheckPicture(800, 450, logo, "picture_800x450_logo", folder);
    CheckPicture(320, 180, logo, "picture_320x180_logo", folder);
    CheckPicture(1280, 720, NULL, "picture_1280x720_nologo", folder);
    CheckPicture(3840, 1080, logo, "picture_3840x1080_logo", folder);
    std::vector<unsigned> none;
    CHECK(!DesktopMessageBuildPicture(32, 32, NULL, none), "a window under 64 pixels gets no picture");
    CheckDevice(logo, folder, false);
    CheckDevice(logo, folder, true);
    if (g_failures)
        printf("desktop_message_check: %d FAILURE(S)\n", g_failures);
    else
        printf("desktop_message_check: all checks passed\n");
    return g_failures ? 1 : 0;
}
