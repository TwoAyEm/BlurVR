// Host check of src/fps_counter.cpp (NOTES.md section 27, [VR] FpsCounter): how the frames are counted (steady rates, a gap, the limits), the colours, the font, and on a real
// Direct3D 9 device that the panel is drawn where it should be in both eyes, stays there when the head moves, reads back as the figure, touches nothing outside itself and puts the
// device state back.  Built and run by tools/fps_counter_check.py, which can pass a folder for pictures of the panel.
#include <stdio.h>
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "fps_counter.h"
#include "matrix.h"
#include "render_state.h"
#include "vr_runtime.h"

// ---- what the module takes from the rest of the mod ---------------------------------------------------------------------------------------------------------
Config g_cfg;
VrState g_vr;
void* g_deviceOriginals[128];
UINT g_backBufferWidth = 0, g_backBufferHeight = 0;
D3DVIEWPORT9 g_gameViewport;
BOOL g_gameScissorEnabled = FALSE;
RECT g_gameScissorRect = {0, 0, 0, 0};
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

// ---- counting frames ----------------------------------------------------------------------------------------------------------------------------------------
static const long long kPerSecond = 10000000; // (a clock of 10 MHz)
static long long g_clock = 5 * kPerSecond;
static void Frames(int count, long long interval)
{
    for (int i = 0; i < count; i++) {
        g_clock += interval;
        FpsCounterFrameAt(g_clock, kPerSecond);
    }
}
static void StartOver() // a gap of more than two seconds: the next frame opens a new window and the figure goes
{
    g_clock += 10 * kPerSecond;
    FpsCounterFrameAt(g_clock, kPerSecond);
}
static int SteadyRate(long long interval, int frames)
{
    StartOver();
    Frames(frames, interval);
    return FpsCounterShown();
}
// the figure the panel shows next, made by frames of that rate
static void ShowFigure(int fps)
{
    StartOver();
    Frames(fps * 2 + 8, kPerSecond / fps);
}

static void CheckCounting()
{
    printf("counting frames:\n");
    StartOver();
    CHECK(FpsCounterShown() == 0, "no figure at the start");
    Frames(20, kPerSecond / 60);
    CHECK(FpsCounterShown() == 0, "no figure before half a second has passed (%d)", FpsCounterShown());
    Frames(20, kPerSecond / 60);
    CHECK(FpsCounterShown() == 60, "60 frames a second show 60 (%d)", FpsCounterShown());
    CHECK(SteadyRate(kPerSecond / 90, 120) == 90, "90 frames a second show 90 (%d)", FpsCounterShown());
    CHECK(SteadyRate(kPerSecond / 72, 120) == 72, "72 frames a second show 72 (%d)", FpsCounterShown());
    CHECK(SteadyRate(kPerSecond / 45, 120) == 45, "45 frames a second show 45 (%d)", FpsCounterShown());
    CHECK(SteadyRate(kPerSecond / 7, 20) == 7, "7 frames a second show 7 (%d)", FpsCounterShown());
    CHECK(SteadyRate(kPerSecond / 144, 300) == 144, "144 frames a second show 144 (%d)", FpsCounterShown());
    // uneven frames: 10 ms and 20 ms alternate, 15 ms on average = 66.7
    StartOver();
    for (int i = 0; i < 100; i++)
        Frames(1, i % 2 ? kPerSecond / 100 : kPerSecond / 50);
    CHECK(FpsCounterShown() == 67, "10 ms and 20 ms frames alternating show 67 (%d)", FpsCounterShown());
    // the limit
    CHECK(SteadyRate(kPerSecond / 5000, 6000) == 999, "5000 frames a second show 999 (%d)", FpsCounterShown());
    // a long gap (the counter was off, a level was loading): nothing until a window is full again
    SteadyRate(kPerSecond / 60, 120);
    CHECK(FpsCounterShown() == 60, "60 before the gap (%d)", FpsCounterShown());
    g_clock += 3 * kPerSecond;
    FpsCounterFrameAt(g_clock, kPerSecond);
    CHECK(FpsCounterShown() == 0, "a gap of three seconds takes the figure away (%d)", FpsCounterShown());
    Frames(10, kPerSecond / 60);
    CHECK(FpsCounterShown() == 0, "and none comes before a window is full (%d)", FpsCounterShown());
    Frames(40, kPerSecond / 60);
    CHECK(FpsCounterShown() == 60, "then 60 again (%d)", FpsCounterShown());
    // a hitch of a second and a half counts: the figure drops, it does not vanish
    Frames(1, kPerSecond * 3 / 2);
    CHECK(FpsCounterShown() > 0 && FpsCounterShown() < 60, "a hitch of 1.5 s lowers the figure (%d)", FpsCounterShown());
}

static void CheckColours()
{
    printf("colours:\n");
    auto channel = [](D3DCOLOR c, int shift) { return (int)((c >> shift) & 255); };
    const D3DCOLOR white = FpsCounterColor(60, 0), green = FpsCounterColor(90, 90), nearly = FpsCounterColor(86, 90), yellow = FpsCounterColor(72, 90), low = FpsCounterColor(64, 90),
                   red = FpsCounterColor(45, 90);
    CHECK(white == 0xFFFFFFFF, "white when the refresh rate is not known (%08x)", (unsigned)white);
    CHECK(channel(green, 8) > channel(green, 16) + 80 && channel(green, 8) > channel(green, 0) + 80, "90 of 90 is green (%08x)", (unsigned)green);
    CHECK(nearly == green, "86 of 90 is still green");
    CHECK(channel(yellow, 16) > 200 && channel(yellow, 8) > 180 && channel(yellow, 0) < 100, "72 of 90 is yellow (%08x)", (unsigned)yellow);
    CHECK(low == yellow, "64 of 90 is still yellow");
    CHECK(channel(red, 16) > 200 && channel(red, 8) < 120, "45 of 90 is red (%08x)", (unsigned)red);
    CHECK(FpsCounterColor(120, 120) == green && FpsCounterColor(100, 120) == yellow && FpsCounterColor(60, 120) == red, "the same at 120 Hz");
    CHECK((green >> 24) == 255 && (red >> 24) == 255, "opaque");
}

static void CheckFont()
{
    printf("font:\n");
    const char* all = "0123456789FPS";
    unsigned char rows[13][7];
    for (int i = 0; i < 13; i++) {
        CHECK(FpsCounterGlyph(all[i], rows[i]), "glyph %c exists", all[i]);
        int lit = 0;
        for (int y = 0; y < 7; y++) {
            CHECK((rows[i][y] & ~0x1F) == 0, "glyph %c row %d is 5 pixels wide (%02x)", all[i], y, rows[i][y]);
            for (int x = 0; x < 5; x++)
                lit += (rows[i][y] >> x) & 1;
        }
        CHECK(lit >= 10 && lit <= 26, "glyph %c has a sensible number of lit pixels (%d)", all[i], lit);
        CHECK((rows[i][0] | rows[i][1] | rows[i][2] | rows[i][3] | rows[i][4] | rows[i][5] | rows[i][6]) != 0, "glyph %c is not empty", all[i]);
        for (int j = 0; j < i; j++)
            CHECK(memcmp(rows[i], rows[j], 7) != 0, "glyphs %c and %c differ", all[i], all[j]);
    }
    unsigned char scratch[7];
    CHECK(!FpsCounterGlyph(' ', scratch) && !FpsCounterGlyph('A', scratch) && !FpsCounterGlyph(0, scratch), "other characters have no glyph");
    CHECK(kFpsPlateWidth == 41 && kFpsPlateHeight == 11, "the plate is 41 x 11 font pixels (%d x %d)", kFpsPlateWidth, kFpsPlateHeight);
    // each glyph touches the top and the bottom row and the left or right column: the layout rests on them being 5 x 7
    for (int i = 0; i < 13; i++)
        CHECK(rows[i][0] != 0 && rows[i][6] != 0, "glyph %c fills its first and last row", all[i]);
}

// ---- a real device ------------------------------------------------------------------------------------------------------------------------------------------
static const int kBackW = 3200, kBackH = 1600;
static const double kHalfIpd = 0.0315;
static const unsigned kGrey = 0x00808080;

// the eyes: 90 degrees across and as much up and down, the head still (moved = false) or turned and moved
static void SetPose(bool moved)
{
    g_vr.eyeBox[0] = g_vr.eyeBox[1] = {-1.0, 1.0, 1.0, 1.0};
    Matrix4 head = Identity();
    if (moved) { // turned 20 degrees about the vertical axis and 12 about the horizontal one, moved 10 cm to the side, 5 up and 20 forward
        const double a = 20.0 * 3.14159265358979 / 180.0, b = 12.0 * 3.14159265358979 / 180.0;
        Matrix4 yaw = Identity(), pitch = Identity();
        yaw.m[0][0] = cos(a);
        yaw.m[0][2] = -sin(a);
        yaw.m[2][0] = sin(a);
        yaw.m[2][2] = cos(a);
        pitch.m[1][1] = cos(b);
        pitch.m[1][2] = sin(b);
        pitch.m[2][1] = -sin(b);
        pitch.m[2][2] = cos(b);
        head = Multiply(yaw, pitch);
        head.m[3][0] = -0.10;
        head.m[3][1] = -0.05;
        head.m[3][2] = -0.20;
    }
    g_vr.eyeCameraDelta[2] = head;
    for (int eye = 0; eye < 2; eye++) {
        Matrix4 headToEye = Identity();
        headToEye.m[3][0] = eye == 0 ? kHalfIpd : -kHalfIpd; // (the left eye sits at -half in head space: view-space shift +half)
        g_vr.eyeCameraDelta[eye] = Multiply(head, headToEye);
    }
    g_vr.poseValid = true;
}

// where a corner of a font pixel should land in the picture (image coordinates in pixels), computed straight from the layout without the module's matrices
static void Expected(int eye, double cellX, double cellY, double* sx, double* sy)
{
    const double z = kFpsDistance;
    const double x = (kFpsCentreX + (cellX - kFpsPlateWidth / 2.0) * kFpsPixelTan) * z;
    const double y = (kFpsCentreY - (cellY - kFpsPlateHeight / 2.0) * kFpsPixelTan) * z;
    const double vx = x + (eye == 0 ? kHalfIpd : -kHalfIpd);
    const EyeBox& box = g_vr.eyeBox[eye];
    *sx = eye * (kBackW / 2.0) + (vx / z - box.left) / (box.right - box.left) * (kBackW / 2.0);
    *sy = (box.up - y / z) / (box.up + box.down) * kBackH;
}

static bool ReadBack(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, IDirect3DSurface9* copy, std::vector<unsigned>& pixels)
{
    D3DLOCKED_RECT locked;
    if (FAILED(device->GetRenderTargetData(backBuffer, copy)) || FAILED(copy->LockRect(&locked, NULL, D3DLOCK_READONLY)))
        return false;
    pixels.resize((size_t)kBackW * kBackH);
    for (int y = 0; y < kBackH; y++)
        memcpy(&pixels[(size_t)y * kBackW], (const BYTE*)locked.pBits + (size_t)y * locked.Pitch, (size_t)kBackW * 4);
    copy->UnlockRect();
    for (unsigned& p : pixels)
        p &= 0x00FFFFFF;
    return true;
}

static unsigned PixelAt(const std::vector<unsigned>& pixels, double sx, double sy)
{
    const int x = (int)floor(sx), y = (int)floor(sy);
    return x >= 0 && x < kBackW && y >= 0 && y < kBackH ? pixels[(size_t)y * kBackW + x] : 0xFFFFFFFFu;
}

// Is the figure there, pixel by pixel of the font?  Returns the number of font pixels that are not as they should be.
static int CheckFigure(const std::vector<unsigned>& pixels, int fps, const char* name)
{
    char digits[8];
    const int length = snprintf(digits, sizeof(digits), "%d", fps);
    int wrong = 0;
    for (int eye = 0; eye < 2; eye++) {
        auto cell = [&](int cellX, int cellY) { // the pixel in the middle of a font pixel
            double sx, sy;
            Expected(eye, cellX + 0.5, cellY + 0.5, &sx, &sy);
            return PixelAt(pixels, sx, sy);
        };
        auto compare = [&](char c, int left) {
            unsigned char rows[7] = {};
            const bool known = c && FpsCounterGlyph(c, rows);
            for (int y = 0; y < 7; y++)
                for (int x = 0; x < 5; x++) {
                    const bool lit = known && (rows[y] & (1 << (4 - x)));
                    const unsigned p = cell(left + x, kFpsMargin + y);
                    const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                    const bool isLit = r >= 240 && g >= 240 && b >= 240;
                    const bool isPlate = abs(r - 53) <= 4 && abs(g - 53) <= 4 && abs(b - 53) <= 4;
                    if (lit ? !isLit : !isPlate) {
                        if (wrong++ < 5)
                            printf("  %s eye %d: character %c, font pixel %d,%d is %06x, should be %s\n", name, eye, c ? c : '_', x, y, p, lit ? "lit" : "plate");
                    }
                }
        };
        for (int slot = 0; slot < kFpsDigits; slot++) {
            const int index = slot - (kFpsDigits - length);
            compare(index >= 0 ? digits[index] : 0, kFpsMargin + slot * kFpsAdvance);
        }
        const int labelLeft = kFpsMargin + kFpsDigits * kFpsAdvance + kFpsLabelGap;
        compare('F', labelLeft);
        compare('P', labelLeft + kFpsAdvance);
        compare('S', labelLeft + 2 * kFpsAdvance);
        // the plate's margin, all round
        const unsigned corners[4] = {cell(0, 0), cell(kFpsPlateWidth - 1, 0), cell(0, kFpsPlateHeight - 1), cell(kFpsPlateWidth - 1, kFpsPlateHeight - 1)};
        for (unsigned p : corners)
            if (abs((int)((p >> 16) & 255) - 53) > 4 || abs((int)(p & 255) - 53) > 4) {
                wrong++;
                printf("  %s eye %d: a corner of the plate is %06x\n", name, eye, p);
            }
    }
    return wrong;
}

// Nothing outside the two plates may have changed; returns how many pixels did.
static size_t ChangedOutsidePlates(const std::vector<unsigned>& pixels, std::vector<unsigned>* before)
{
    RECT plate[2];
    for (int eye = 0; eye < 2; eye++) {
        double x0, y0, x1, y1;
        Expected(eye, 0, 0, &x0, &y0);
        Expected(eye, kFpsPlateWidth, kFpsPlateHeight, &x1, &y1);
        plate[eye] = {(LONG)floor(x0) - 3, (LONG)floor(y0) - 3, (LONG)ceil(x1) + 3, (LONG)ceil(y1) + 3};
    }
    size_t changed = 0;
    for (int y = 0; y < kBackH; y++)
        for (int x = 0; x < kBackW; x++) {
            bool inside = false;
            for (int eye = 0; eye < 2; eye++)
                inside = inside || (x >= plate[eye].left && x < plate[eye].right && y >= plate[eye].top && y < plate[eye].bottom);
            if (!inside && pixels[(size_t)y * kBackW + x] != (before ? (*before)[(size_t)y * kBackW + x] : kGrey))
                changed++;
        }
    return changed;
}

static void SavePanel(const std::vector<unsigned>& pixels, const char* folder, const char* name)
{
    if (!folder)
        return;
    for (int eye = 0; eye < 2; eye++) {
        double x0, y0, x1, y1;
        Expected(eye, 0, 0, &x0, &y0);
        Expected(eye, kFpsPlateWidth, kFpsPlateHeight, &x1, &y1);
        const int left = (int)x0 - 40, top = (int)y0 - 40, width = (int)(x1 - x0) + 80, height = (int)(y1 - y0) + 80;
        std::vector<unsigned> crop((size_t)width * height);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                crop[(size_t)y * width + x] = pixels[(size_t)(top + y) * kBackW + left + x] | 0xFF000000u;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\fps_panel_%s_eye%d.bmp", folder, name, eye);
        WriteBmp(path, width, height, crop);
    }
}

static LRESULT CALLBACK CheckProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return DefWindowProcA(window, message, w, l);
}

static void CheckDevice(const char* folder, bool ex)
{
    printf("%s device:\n", ex ? "9Ex" : "9");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = CheckProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "fps_counter_check";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "fps_counter_check", WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, NULL, NULL, wc.hInstance, NULL);
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
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = kBackW;
    pp.BackBufferHeight = kBackH;
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
    // the mod's hooks are not installed here: the "originals" are the device's own functions
    void** vtable = *(void***)device;
    const int used[] = {VT_SetRenderTarget, VT_SetViewport, VT_SetRenderState, VT_SetScissorRect, VT_SetTexture, VT_SetVertexShader, VT_SetPixelShader, VT_DrawPrimitiveUP};
    for (int index : used)
        g_deviceOriginals[index] = vtable[index];

    IDirect3DSurface9 *backBuffer = NULL, *copy = NULL;
    CHECK(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)), "back buffer");
    CHECK(SUCCEEDED(device->CreateOffscreenPlainSurface(kBackW, kBackH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, NULL)), "readback surface");
    if (!backBuffer || !copy) {
        printf("SKIP: no surfaces\n");
        return;
    }
    g_backBufferWidth = kBackW;
    g_backBufferHeight = kBackH;
    g_cfg.fpsCounter = 1;
    g_cfg.positionScale = 1.0;
    g_cfg.seamGutter = 0;

    // the game's state when the frame ends: something different from what the counter needs, in places a state block has to bring back
    D3DVIEWPORT9 viewport = {0, 0, 640, 480, 0.0f, 1.0f};
    RECT scissor = {10, 20, 300, 400};
    auto setGameState = [&]() {
        device->SetRenderTarget(0, backBuffer);
        device->SetViewport(&viewport);
        g_gameViewport = viewport;
        device->SetScissorRect(&scissor);
        g_gameScissorRect = scissor;
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        g_gameScissorEnabled = TRUE;
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
    };
    const D3DRENDERSTATETYPE states[] = {D3DRS_SCISSORTESTENABLE, D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_SRCBLEND, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_COLORWRITEENABLE};
    auto stateString = [&]() {
        std::string s;
        char text[64];
        for (D3DRENDERSTATETYPE state : states) {
            DWORD value = 0;
            device->GetRenderState(state, &value);
            snprintf(text, sizeof(text), "%u,", (unsigned)value);
            s += text;
        }
        DWORD fvf = 0, op0 = 0, op1 = 0;
        device->GetFVF(&fvf);
        device->GetTextureStageState(0, D3DTSS_COLOROP, &op0);
        device->GetTextureStageState(1, D3DTSS_COLOROP, &op1);
        D3DVIEWPORT9 now;
        device->GetViewport(&now);
        RECT scissorNow;
        device->GetScissorRect(&scissorNow);
        IDirect3DSurface9* target = NULL;
        device->GetRenderTarget(0, &target);
        snprintf(text, sizeof(text), "fvf %x op %u %u vp %u %u rect %ld %ld %ld %ld rt %s", (unsigned)fvf, (unsigned)op0, (unsigned)op1, (unsigned)now.Width, (unsigned)now.Height, scissorNow.left, scissorNow.top, scissorNow.right, scissorNow.bottom,
                 target == backBuffer ? "back" : "other");
        if (target)
            target->Release();
        return s + text;
    };
    // (Clear obeys the viewport, the scissor rectangle and the colour write mask: the whole buffer is cleared first, then the game's state is set)
    auto clear = [&]() {
        D3DVIEWPORT9 whole = {0, 0, kBackW, kBackH, 0.0f, 1.0f};
        device->SetViewport(&whole);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        return device->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000u | kGrey, 1.0f, 0);
    };
    auto prepare = [&]() {
        clear();
        setGameState();
    };
    std::vector<unsigned> pixels, greyPicture;

    // nothing happens while the counter is off, has no figure, or the head is not tracked
    SetPose(false);
    CHECK(SUCCEEDED(clear()), "clear");
    setGameState();
    CHECK(ReadBack(device, backBuffer, copy, greyPicture), "read back the grey picture");
    ShowFigure(60);
    g_cfg.fpsCounter = 0;
    FpsCounterDraw(device);
    CHECK(ReadBack(device, backBuffer, copy, pixels) && pixels == greyPicture, "the counter off: the picture is as it was");
    g_cfg.fpsCounter = 1;
    g_vr.poseValid = false;
    FpsCounterDraw(device);
    CHECK(ReadBack(device, backBuffer, copy, pixels) && pixels == greyPicture, "no pose yet: the picture is as it was");
    g_vr.poseValid = true;
    StartOver();
    FpsCounterDraw(device);
    CHECK(ReadBack(device, backBuffer, copy, pixels) && pixels == greyPicture, "no figure yet: the picture is as it was");

    // the figures, head still and head moved; the first draw of each is outside a scene (Present comes after EndScene), the second inside one
    const int figures[] = {62, 7, 100, 999, 45};
    for (int moved = 0; moved < 2; moved++) {
        for (int fps : figures) {
            SetPose(moved != 0);
            char name[64];
            snprintf(name, sizeof(name), "%d_%s", fps, moved ? "moved" : "still");
            ShowFigure(fps);
            CHECK(FpsCounterShown() == fps, "the figure %d is shown (%d)", fps, FpsCounterShown());
            for (int inScene = 0; inScene < 2; inScene++) {
                prepare();
                const std::string before = stateString();
                if (inScene)
                    device->BeginScene();
                FpsCounterDraw(device);
                if (inScene)
                    device->EndScene();
                const std::string after = stateString();
                CHECK(before == after, "%s: the device state is put back\n    before %s\n    after  %s", name, before.c_str(), after.c_str());
                CHECK(ReadBack(device, backBuffer, copy, pixels), "read back");
                const int wrong = CheckFigure(pixels, fps, name);
                CHECK(wrong == 0, "%s%s: %d font pixels are not as they should be", name, inScene ? " (in a scene)" : "", wrong);
                const size_t changed = ChangedOutsidePlates(pixels, NULL);
                CHECK(changed == 0, "%s: %zu pixels outside the plates changed", name, changed);
                if (!inScene)
                    SavePanel(pixels, folder, name);
            }
        }
    }

    // the same picture whatever the head does: the panel is where the eyes are, not where the car is
    {
        std::vector<unsigned> still, moved;
        SetPose(false);
        ShowFigure(88);
        prepare();
        FpsCounterDraw(device);
        ReadBack(device, backBuffer, copy, still);
        SetPose(true);
        prepare();
        FpsCounterDraw(device);
        ReadBack(device, backBuffer, copy, moved);
        size_t different = 0;
        for (size_t i = 0; i < still.size(); i++)
            different += still[i] != moved[i];
        CHECK(different < 400, "a turned and moved head gives the same picture (%zu of %zu pixels differ)", different, still.size());
    }

    // the scissor rectangle keeps the panel inside its own eye: a gutter of 40 pixels at the seam only removes nothing from a panel that is far from it
    g_cfg.seamGutter = 40;
    SetPose(false);
    ShowFigure(62);
    prepare();
    FpsCounterDraw(device);
    CHECK(ReadBack(device, backBuffer, copy, pixels) && CheckFigure(pixels, 62, "gutter") == 0, "a seam gutter does not touch a panel far from the seam");
    g_cfg.seamGutter = 0;

    // after a Reset the state block is made again
    ReleaseFpsCounter();
    ShowFigure(62);
    prepare();
    FpsCounterDraw(device);
    CHECK(ReadBack(device, backBuffer, copy, pixels) && CheckFigure(pixels, 62, "after release") == 0, "the panel is drawn again after ReleaseFpsCounter");

    ReleaseFpsCounter();
    copy->Release();
    backBuffer->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
}

int main(int argc, char** argv)
{
    const char* folder = argc > 1 && argv[1][0] ? argv[1] : NULL;
    CheckCounting();
    CheckColours();
    CheckFont();
    CheckDevice(folder, false);
    CheckDevice(folder, true);
    if (g_failures)
        printf("fps_counter_check: %d FAILURE(S)\n", g_failures);
    else
        printf("fps_counter_check: all checks passed\n");
    return g_failures ? 1 : 0;
}
