// The window on the desktop with [VR] DesktopView=off (NOTES.md section 20): "Playing in VR" and the game's logo.  Instead of the game's picture every Present, a
// small picture made with GDI once per window size goes into the corner of the back buffer and is presented now and then, so the window costs almost nothing.
#include "desktop_message.h"
#include "config.h"
#include "core.h"
#include "display_modes.h"
#include "vr_runtime.h"

namespace {

const DWORD kPresentEveryMs = 500; // the message is presented this often (the window keeps what it was given in between)
const wchar_t kMessage[] = L"Playing in VR";
const int kLogoSource = 32; // the game's icon is 32 by 32

IDirect3DSurface9* g_picture = NULL; // the message at the presented size: a default-pool surface in the back buffer's format
UINT g_pictureWidth = 0, g_pictureHeight = 0;
D3DFORMAT g_pictureFormat = D3DFMT_UNKNOWN;
HICON g_logo = NULL;
bool g_logoTried = false;
bool g_presentedOnce = false;
UINT g_lastWidth = 0, g_lastHeight = 0;
DWORD g_lastTick = 0;

// The window's client size as it really is (this module's own GetClientRect: the game's is told another size, window_spoof.cpp).
bool WindowSize(IDirect3DDevice9* device, UINT* width, UINT* height)
{
    D3DDEVICE_CREATION_PARAMETERS creation;
    RECT client;
    if (FAILED(device->GetCreationParameters(&creation)) || !creation.hFocusWindow || !GetClientRect(creation.hFocusWindow, &client))
        return false;
    *width = (UINT)(client.right - client.left);
    *height = (UINT)(client.bottom - client.top);
    return *width >= 64 && *height >= 64; // (smaller: minimised)
}

int Clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

unsigned Background(int y, int height)
{
    const double t = height > 1 ? (double)y / (height - 1) : 0.0; // dark green-grey, a little lighter at the top
    const int r = (int)(26 + (10 - 26) * t), g = (int)(34 + (13 - 34) * t), b = (int)(31 + (12 - 31) * t);
    return 0xFF000000u | (unsigned)(r << 16) | (unsigned)(g << 8) | (unsigned)b;
}

// The icon as colour and coverage: drawn on black and on white, which tells the two apart for any kind of icon (an AND mask or an alpha channel).  `colour` is
// premultiplied (what was drawn on black), `coverage` 0 to 1.
bool IconPixels(HICON icon, float colour[kLogoSource * kLogoSource][3], float coverage[kLogoSource * kLogoSource])
{
    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kLogoSource;
    info.bmiHeader.biHeight = -kLogoSource;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    void* bits = NULL;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    bool ok = dc && bitmap && bits;
    unsigned onBlack[kLogoSource * kLogoSource], onWhite[kLogoSource * kLogoSource];
    if (ok) {
        HGDIOBJ old = SelectObject(dc, bitmap);
        unsigned* px = (unsigned*)bits;
        for (int pass = 0; pass < 2 && ok; pass++) {
            for (int i = 0; i < kLogoSource * kLogoSource; i++)
                px[i] = pass ? 0x00FFFFFFu : 0x00000000u;
            ok = DrawIconEx(dc, 0, 0, icon, kLogoSource, kLogoSource, 0, NULL, DI_NORMAL) != FALSE;
            GdiFlush();
            memcpy(pass ? onWhite : onBlack, px, sizeof(onBlack));
        }
        SelectObject(dc, old);
    }
    if (bitmap)
        DeleteObject(bitmap);
    if (dc)
        DeleteDC(dc);
    if (!ok)
        return false;
    for (int i = 0; i < kLogoSource * kLogoSource; i++) {
        const unsigned b = onBlack[i], w = onWhite[i];
        float apart = 0; // how much lighter the white background came out: 0 where the icon covers it, 255 per channel where it does not
        for (int shift = 0; shift <= 16; shift += 8) {
            const int cb = (int)((b >> shift) & 255), cw = (int)((w >> shift) & 255);
            apart += (float)(cw - cb);
            colour[i][shift / 8] = (float)cb;
        }
        coverage[i] = 1.0f - apart / (3 * 255.0f);
        if (coverage[i] < 0)
            coverage[i] = 0;
        if (coverage[i] > 1)
            coverage[i] = 1;
    }
    return true;
}

// The icon scaled up to size by size pixels (bilinear: it is 32 pixels, so a smooth enlargement is the best that can be had) and laid over the picture at (x, y).
void DrawLogo(unsigned* pixels, int width, int height, HICON icon, int x, int y, int size)
{
    static float colour[kLogoSource * kLogoSource][3], coverage[kLogoSource * kLogoSource];
    if (!IconPixels(icon, colour, coverage))
        return;
    for (int dy = 0; dy < size; dy++) {
        const float fy = (dy + 0.5f) * kLogoSource / size - 0.5f;
        const int y0 = Clamp((int)floorf(fy), 0, kLogoSource - 1), y1 = Clamp(y0 + 1, 0, kLogoSource - 1);
        const float ty = fy < 0 ? 0.0f : fy - floorf(fy);
        for (int dx = 0; dx < size; dx++) {
            const int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= width || py >= height)
                continue;
            const float fx = (dx + 0.5f) * kLogoSource / size - 0.5f;
            const int x0 = Clamp((int)floorf(fx), 0, kLogoSource - 1), x1 = Clamp(x0 + 1, 0, kLogoSource - 1);
            const float tx = fx < 0 ? 0.0f : fx - floorf(fx);
            const int index[4] = {y0 * kLogoSource + x0, y0 * kLogoSource + x1, y1 * kLogoSource + x0, y1 * kLogoSource + x1};
            const float weight[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
            float c[3] = {0, 0, 0}, a = 0;
            for (int k = 0; k < 4; k++) {
                a += coverage[index[k]] * weight[k];
                for (int ch = 0; ch < 3; ch++)
                    c[ch] += colour[index[k]][ch] * weight[k];
            }
            unsigned& target = pixels[(size_t)py * width + px];
            unsigned out = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                const float under = (float)((target >> (ch * 8)) & 255);
                out |= (unsigned)Clamp((int)(c[ch] + under * (1 - a) + 0.5f), 0, 255) << (ch * 8);
            }
            target = out;
        }
    }
}

void Fall(const char* why)
{
    Log("DesktopView=off: %s; the window shows the left eye's view instead", why);
    g_cfg.desktopView = 1;
    ReleaseDesktopMessage();
}

// The picture as a default-pool surface the size of what is presented, made again when the size or the back buffer's format changes.
bool UpdatePicture(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format)
{
    if (g_picture && g_pictureWidth == width && g_pictureHeight == height && g_pictureFormat == format)
        return true;
    ReleaseDesktopMessage();
    if (!g_logoTried) {
        g_logoTried = true;
        g_logo = DesktopMessageLoadLogo(NULL);
        Log("DesktopView=off: %s", g_logo ? "the game's icon is the logo" : "the game has no icon, the message is shown without a logo");
    }
    std::vector<unsigned> pixels;
    if (!DesktopMessageBuildPicture((int)width, (int)height, g_logo, pixels))
        return false;
    IDirect3DSurface9 *system = NULL, *gpu = NULL;
    bool ok = SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &system, NULL)) && system &&
              SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_DEFAULT, &gpu, NULL)) && gpu;
    D3DLOCKED_RECT locked;
    if (ok && SUCCEEDED(system->LockRect(&locked, NULL, 0))) {
        for (UINT y = 0; y < height; y++)
            memcpy((BYTE*)locked.pBits + (size_t)y * locked.Pitch, &pixels[(size_t)y * width], (size_t)width * 4);
        system->UnlockRect();
    } else
        ok = false;
    ok = ok && SUCCEEDED(device->UpdateSurface(system, NULL, gpu, NULL));
    if (system)
        system->Release();
    if (!ok) {
        if (gpu)
            gpu->Release();
        return false;
    }
    g_picture = gpu;
    g_pictureWidth = width;
    g_pictureHeight = height;
    g_pictureFormat = format;
    return true;
}

BOOL CALLBACK FirstIconGroup(HMODULE, LPCWSTR, LPWSTR found, LONG_PTR param)
{
    *(LPWSTR*)param = found;
    return FALSE; // (the first icon of the executable is the one Explorer shows)
}

} // namespace

HICON DesktopMessageLoadLogo(HMODULE module)
{
    HMODULE exe = module ? module : GetModuleHandleW(NULL);
    LPWSTR name = NULL;
    EnumResourceNamesW(exe, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */, FirstIconGroup, (LONG_PTR)&name);
    return name ? (HICON)LoadImageW(exe, name, IMAGE_ICON, kLogoSource, kLogoSource, LR_DEFAULTCOLOR) : NULL;
}

bool DesktopMessageBuildPicture(int width, int height, HICON logo, std::vector<unsigned>& pixels)
{
    if (width < 64 || height < 64 || width > 16384 || height > 16384)
        return false;
    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // (top-down)
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    void* bits = NULL;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!dc || !bitmap || !bits) {
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    unsigned* px = (unsigned*)bits;
    for (int y = 0; y < height; y++) {
        const unsigned row = Background(y, height);
        for (int x = 0; x < width; x++)
            px[(size_t)y * width + x] = row;
    }
    // the layout: the logo above the text, the two together in the middle
    int fontHeight = Clamp((int)(height * 0.085 + 0.5), 20, 72);
    if (fontHeight > width / 9) // (a narrow window: the text has to fit)
        fontHeight = width / 9 > 10 ? width / 9 : 10;
    int logoSize = logo ? Clamp((int)(height * 0.2 + 0.5), 64, 96) : 0;
    if (logoSize > height * 2 / 5)
        logoSize = height * 2 / 5;
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : NULL;
    SIZE extent = {0, 0};
    const int length = (int)(sizeof(kMessage) / sizeof(kMessage[0]) - 1);
    GetTextExtentPoint32W(dc, kMessage, length, &extent);
    const int gap = logoSize ? fontHeight * 7 / 10 : 0;
    const int top = (height - (logoSize + gap + (int)extent.cy)) / 2;
    if (logoSize)
        DrawLogo(px, width, height, logo, (width - logoSize) / 2, top, logoSize);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(236, 241, 237));
    TextOutW(dc, (width - (int)extent.cx) / 2, top + logoSize + gap, kMessage, length);
    GdiFlush();
    pixels.assign(px, px + (size_t)width * height);
    for (unsigned& p : pixels)
        p |= 0xFF000000u;
    if (oldFont)
        SelectObject(dc, oldFont);
    if (font)
        DeleteObject(font);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return true;
}

bool DesktopMessageWanted()
{
    return g_cfg.vr && g_cfg.desktopView == kDesktopViewOff && g_vr.active && g_desktopRectUsable && !g_desktopRectFailed;
}

bool DesktopMessageDue(IDirect3DDevice9* device)
{
    UINT width, height;
    if (!WindowSize(device, &width, &height))
        return false;
    return !g_presentedOnce || width != g_lastWidth || height != g_lastHeight || GetTickCount() - g_lastTick >= kPresentEveryMs;
}

bool DesktopMessagePaint(IDirect3DDevice9* device, RECT* presented)
{
    UINT windowWidth, windowHeight;
    if (!WindowSize(device, &windowWidth, &windowHeight))
        return false;
    IDirect3DSurface9* backBuffer = NULL;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) {
        Fall("the back buffer cannot be read");
        return false;
    }
    D3DSURFACE_DESC desc;
    bool known = SUCCEEDED(backBuffer->GetDesc(&desc)) && desc.MultiSampleType == D3DMULTISAMPLE_NONE && (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8);
    if (!known) {
        backBuffer->Release();
        Fall("the back buffer is not a plain 32-bit one");
        return false;
    }
    // at the window's own size (at most the back buffer: scaled down by the same factor in both directions otherwise), in the corner
    const double scale = fmin(1.0, fmin((double)desc.Width / windowWidth, (double)desc.Height / windowHeight));
    RECT dest = {0, 0, (LONG)(windowWidth * scale), (LONG)(windowHeight * scale)};
    const bool ok = dest.right >= 64 && dest.bottom >= 64 && UpdatePicture(device, (UINT)dest.right, (UINT)dest.bottom, desc.Format) &&
                    SUCCEEDED(device->StretchRect(g_picture, NULL, backBuffer, &dest, D3DTEXF_NONE));
    backBuffer->Release();
    if (!ok) {
        Fall("the message could not be painted");
        return false;
    }
    *presented = dest;
    g_lastWidth = windowWidth;
    g_lastHeight = windowHeight;
    g_lastTick = GetTickCount();
    g_presentedOnce = true;
    return true;
}

void ReleaseDesktopMessage()
{
    if (g_picture) {
        g_picture->Release();
        g_picture = NULL;
    }
    g_pictureWidth = g_pictureHeight = 0;
    g_pictureFormat = D3DFMT_UNKNOWN;
    g_presentedOnce = false; // (the window is given the message again after a Reset)
}
