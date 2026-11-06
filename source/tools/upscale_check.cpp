// Host check of src/upscale_d3d11.cpp (the upscaling of the race picture on the Direct3D 11 side, NOTES.md section 32), run on the software rasteriser (WARP): the shaders
// compile, a smooth gradient stays a gradient at the new size, a step edge stays one step, the two eyes do not leak into each other, a flat picture stays flat, and the
// sharpening does not overshoot.  Built and run by tools/upscale_check.py.
#include <stdio.h>
#include <vector>
#include "../src/upscale_d3d11.cpp"

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
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL line %d: %s -- ", __LINE__, #cond); \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
            g_failures++;                                    \
        }                                                    \
    } while (0)

struct Picture {
    UINT w, h;
    std::vector<unsigned> px; // B8G8R8A8: 0xAARRGGBB
};

static ID3D11Device* g_device = NULL;
static ID3D11DeviceContext* g_context = NULL;

static ID3D11Texture2D* MakeTexture(const Picture& p)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = p.w;
    td.Height = p.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data = {p.px.data(), p.w * 4, 0};
    ID3D11Texture2D* t = NULL;
    g_device->CreateTexture2D(&td, &data, &t);
    return t;
}

static Picture Readback(ID3D11Texture2D* t, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = NULL;
    g_device->CreateTexture2D(&td, NULL, &staging);
    g_context->CopyResource(staging, t);
    D3D11_MAPPED_SUBRESOURCE m;
    Picture out = {w, h, std::vector<unsigned>((size_t)w * h)};
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < h; y++)
            memcpy(&out.px[(size_t)y * w], (BYTE*)m.pData + (size_t)y * m.RowPitch, w * 4);
        g_context->Unmap(staging, 0);
    }
    staging->Release();
    return out;
}

static unsigned Pack(int r, int g, int b)
{
    return 0xFF000000u | (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
}
static int Rc(unsigned p) { return (p >> 16) & 255; }
static int Gc(unsigned p) { return (p >> 8) & 255; }
static int Bc(unsigned p) { return p & 255; }

int main()
{
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_device, &level, &g_context);
    if (FAILED(hr)) {
        printf("no WARP device: 0x%08x\n", (unsigned)hr);
        return 2;
    }
    const UINT srcHalf = 64, srcH = 48, srcW = srcHalf * 2;
    UINT dstW = 0, dstH = 0;
    UpscaleOutputSize(srcW, srcH, 1.5, &dstW, &dstH);
    CHECK(dstW == 192 && dstH == 72, "output size %ux%u", dstW, dstH);
    const UINT dstHalf = dstW / 2;

    // 1. a smooth horizontal gradient in the red of the left eye and in the blue of the right eye (green carries a vertical one)
    Picture grad = {srcW, srcH, std::vector<unsigned>((size_t)srcW * srcH)};
    for (UINT y = 0; y < srcH; y++)
        for (UINT x = 0; x < srcW; x++) {
            const bool right = x >= srcHalf;
            const int v = (int)(255.0 * ((x % srcHalf) / (double)(srcHalf - 1)));
            const int gy = (int)(255.0 * y / (srcH - 1));
            grad.px[(size_t)y * srcW + x] = right ? Pack(0, gy, v) : Pack(v, gy, 0);
        }
    ID3D11Texture2D* src = MakeTexture(grad);
    ID3D11Texture2D* up = UpscaleFrame(g_device, g_context, src, srcW, srcH, dstW, dstH, 0.0);
    CHECK(up != NULL, "the upscale ran");
    if (!up)
        return 1;
    Picture out = Readback(up, dstW, dstH);
    double worst = 0;
    int leaks = 0;
    for (UINT y = 4; y < dstH - 4; y++)
        for (UINT x = 4; x < dstW - 4; x++) {
            const bool right = x >= dstHalf;
            const UINT lx = x % dstHalf;
            if (lx < 4 || lx > dstHalf - 5)
                continue;
            const unsigned p = out.px[(size_t)y * dstW + x];
            // where this output pixel sits in the eye's source, 0..1 along the gradient
            const double expected = 255.0 * (((lx + 0.5) * srcHalf / (double)dstHalf - 0.5) / (srcHalf - 1));
            const int got = right ? Bc(p) : Rc(p);
            worst = fmax(worst, fabs(got - expected));
            if ((right && Rc(p) > 2) || (!right && Bc(p) > 2))
                leaks++;
        }
    CHECK(worst < 3.0, "a smooth gradient stays on its line: worst error %.2f of 255", worst);
    CHECK(leaks == 0, "no red in the right eye and no blue in the left eye (%d pixels)", leaks);
    printf("  gradient: worst error %.2f, leaks %d\n", worst, leaks);

    // 2. the seam: the left eye is white at its last column, the right eye black at its first; the halves must not mix
    Picture seam = {srcW, srcH, std::vector<unsigned>((size_t)srcW * srcH, Pack(255, 255, 255))};
    for (UINT y = 0; y < srcH; y++)
        for (UINT x = srcHalf; x < srcW; x++)
            seam.px[(size_t)y * srcW + x] = Pack(0, 0, 0);
    ID3D11Texture2D* seamTex = MakeTexture(seam);
    for (double sharp : {0.0, 0.75}) {
        up = UpscaleFrame(g_device, g_context, seamTex, srcW, srcH, dstW, dstH, sharp);
        out = Readback(up, dstW, dstH);
        int dark = 0, bright = 0;
        for (UINT y = 0; y < dstH; y++) {
            for (UINT x = 0; x < dstHalf; x++)
                if (Rc(out.px[(size_t)y * dstW + x]) < 250)
                    dark++;
            for (UINT x = dstHalf; x < dstW; x++)
                if (Rc(out.px[(size_t)y * dstW + x]) > 5)
                    bright++;
        }
        CHECK(dark == 0 && bright == 0, "sharpening %.2f: a flat white eye next to a flat black one stays flat (%d dark pixels in the left eye, %d bright in the right)", sharp, dark, bright);
    }
    seamTex->Release();

    // 3. a step edge in each eye (dark to light at column 32): one step, no overshoot, and the sharpening makes it no softer
    Picture step = {srcW, srcH, std::vector<unsigned>((size_t)srcW * srcH)};
    for (UINT y = 0; y < srcH; y++)
        for (UINT x = 0; x < srcW; x++)
            step.px[(size_t)y * srcW + x] = ((x % srcHalf) < 32) ? Pack(40, 40, 40) : Pack(200, 200, 200);
    ID3D11Texture2D* stepTex = MakeTexture(step);
    int widthSoft = 0, widthSharp = 0;
    for (double sharp : {0.0, 0.75}) {
        up = UpscaleFrame(g_device, g_context, stepTex, srcW, srcH, dstW, dstH, sharp);
        out = Readback(up, dstW, dstH);
        int lowest = 255, highest = 0, transition = 0;
        const UINT y = dstH / 2;
        for (UINT x = 4; x < dstHalf - 4; x++) {
            const int v = Rc(out.px[(size_t)y * dstW + x]);
            lowest = std::min(lowest, v);
            highest = std::max(highest, v);
            if (v > 50 && v < 190)
                transition++;
        }
        CHECK(lowest >= 38 && highest <= 202, "sharpening %.2f: the step stays within its two levels (%d..%d of 40..200)", sharp, lowest, highest);
        CHECK(transition <= 3, "sharpening %.2f: the step is %d pixels wide", sharp, transition);
        (sharp > 0 ? widthSharp : widthSoft) = transition;
    }
    CHECK(widthSharp <= widthSoft, "the sharpening does not make the edge wider (%d against %d)", widthSharp, widthSoft);
    stepTex->Release();

    // 4. a flat picture stays flat
    Picture flat = {srcW, srcH, std::vector<unsigned>((size_t)srcW * srcH, Pack(90, 120, 200))};
    ID3D11Texture2D* flatTex = MakeTexture(flat);
    up = UpscaleFrame(g_device, g_context, flatTex, srcW, srcH, dstW, dstH, 1.0);
    out = Readback(up, dstW, dstH);
    int off = 0;
    for (unsigned p : out.px)
        if (abs(Rc(p) - 90) > 1 || abs(Gc(p) - 120) > 1 || abs(Bc(p) - 200) > 1)
            off++;
    CHECK(off == 0, "a flat picture stays flat (%d pixels differ)", off);
    flatTex->Release();

    // 5. the same size (factor 1) gives the gradient back
    up = UpscaleFrame(g_device, g_context, src, srcW, srcH, srcW, srcH, 0.0);
    out = Readback(up, srcW, srcH);
    double worstSame = 0;
    for (UINT y = 2; y < srcH - 2; y++)
        for (UINT x = 2; x < srcW - 2; x++) {
            if (x % srcHalf < 2 || x % srcHalf > srcHalf - 3)
                continue;
            const unsigned a = grad.px[(size_t)y * srcW + x], b = out.px[(size_t)y * srcW + x];
            worstSame = fmax(worstSame, fmax(abs(Rc(a) - Rc(b)), fmax(abs(Gc(a) - Gc(b)), abs(Bc(a) - Bc(b)))));
        }
    CHECK(worstSame <= 2.0, "at the same size the picture comes back: worst difference %.1f", worstSame);

    src->Release();
    ReleaseUpscale();
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
