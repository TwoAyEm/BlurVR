// Frame rate counter in the headset.  See fps_counter.h.
#include "fps_counter.h"
#include "config.h"
#include "core.h"
#include "d3d_hooks.h"
#include "matrix.h"
#include "render_state.h"
#include "vr_runtime.h"

// ---- the font: 5 x 7 pixels, one byte per row, the leftmost pixel in bit 4 -------------------------------------------------------------------------------
static const char kGlyphChars[] = "0123456789FPS";
static const unsigned char kGlyphs[13][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // S
};

bool FpsCounterGlyph(char c, unsigned char rows[7])
{
    const char* at = c ? strchr(kGlyphChars, c) : NULL;
    if (!at)
        return false;
    memcpy(rows, kGlyphs[at - kGlyphChars], 7);
    return true;
}

// ---- the figure -------------------------------------------------------------------------------------------------------------------------------------------
static long long g_windowStart = 0; // the frame that opened the window being counted
static long long g_lastFrame = 0;
static int g_windowFrames = 0;      // frames since then
static int g_shown = 0;             // frames a second, as the last window measured them (0 = none yet)
static double g_refreshHz = 0;      // the headset's, read whenever a new figure is taken (0 = not known)

void FpsCounterFrameAt(long long ticks, long long perSecond)
{
    // The first frame, or one after a gap of two seconds (the counter was off, a level was loading): not a frame rate, so start over and show nothing until a window is full.
    if (!g_lastFrame || ticks - g_lastFrame >= 2 * perSecond) {
        g_windowStart = g_lastFrame = ticks;
        g_windowFrames = 0;
        g_shown = 0;
        return;
    }
    g_lastFrame = ticks;
    g_windowFrames++;
    const long long elapsed = ticks - g_windowStart;
    if (elapsed >= perSecond / 2) { // (the window is a number of whole frame intervals: from the frame that opened it to this one)
        const int fps = (int)((double)g_windowFrames * (double)perSecond / (double)elapsed + 0.5);
        g_shown = fps < 999 ? fps : 999;
        g_windowStart = ticks;
        g_windowFrames = 0;
    }
}

int FpsCounterShown()
{
    return g_shown;
}

static double HeadsetRefreshHz()
{
    if (!g_vr.system)
        return 0;
    ETrackedPropertyError error = ETrackedPropertyError_TrackedProp_Success;
    const float hz = g_vr.system->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &error);
    return error == ETrackedPropertyError_TrackedProp_Success && hz >= 20.f && hz <= 1000.f ? hz : 0;
}

void FpsCounterFrame()
{
    if (!g_cfg.fpsCounter)
        return;
    LARGE_INTEGER now, frequency;
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return;
    FpsCounterFrameAt(now.QuadPart, frequency.QuadPart);
    if (g_windowFrames == 0 && g_shown > 0) // (a new figure was just taken)
        g_refreshHz = HeadsetRefreshHz();
}

D3DCOLOR FpsCounterColor(int fps, double refreshHz)
{
    if (refreshHz <= 0)
        return D3DCOLOR_ARGB(255, 255, 255, 255);
    if (fps >= refreshHz * 0.95)
        return D3DCOLOR_ARGB(255, 90, 235, 110);
    if (fps >= refreshHz * 0.7)
        return D3DCOLOR_ARGB(255, 255, 215, 60);
    return D3DCOLOR_ARGB(255, 255, 85, 70);
}

// ---- the panel --------------------------------------------------------------------------------------------------------------------------------------------
struct PanelVertex {
    float x, y, z, rhw;
    DWORD color;
};
struct FontRect { // font pixels, from the plate's top left corner
    int x0, y0, x1, y1;
};
static const int kMaxTextRects = 6 * kFpsGlyphHeight * 3; // (a row of a glyph has at most 3 runs of lit pixels)
static FontRect g_textRects[kMaxTextRects];
static PanelVertex g_vertices[(1 + kMaxTextRects) * 6];
static IDirect3DStateBlock9* g_stateBlock = NULL;
static bool g_failed = false, g_announced = false, g_drawFailureLogged = false;

// The lit pixels of the digits and the label as rectangles, each a run of lit pixels in a row.
static int BuildTextRects(int fps, FontRect* out)
{
    int count = 0;
    auto addGlyph = [&](char c, int left) {
        unsigned char rows[7];
        if (!FpsCounterGlyph(c, rows))
            return;
        for (int y = 0; y < kFpsGlyphHeight; y++) {
            int x = 0;
            while (x < kFpsGlyphWidth) {
                if (!(rows[y] & (1 << (kFpsGlyphWidth - 1 - x)))) {
                    x++;
                    continue;
                }
                const int start = x;
                while (x < kFpsGlyphWidth && (rows[y] & (1 << (kFpsGlyphWidth - 1 - x))))
                    x++;
                out[count++] = {left + start, kFpsMargin + y, left + x, kFpsMargin + y + 1};
            }
        }
    };
    char digits[16];
    const int length = snprintf(digits, sizeof(digits), "%d", fps < 0 ? 0 : fps > 999 ? 999 : fps);
    for (int i = 0; i < length; i++) // (right-aligned: the label stays where it is)
        addGlyph(digits[i], kFpsMargin + (kFpsDigits - length + i) * kFpsAdvance);
    const int labelLeft = kFpsMargin + kFpsDigits * kFpsAdvance + kFpsLabelGap;
    addGlyph('F', labelLeft);
    addGlyph('P', labelLeft + kFpsAdvance);
    addGlyph('S', labelLeft + 2 * kFpsAdvance);
    return count;
}

void ReleaseFpsCounter()
{
    if (g_stateBlock) {
        g_stateBlock->Release();
        g_stateBlock = NULL;
    }
    g_failed = false;
}

void FpsCounterDraw(IDirect3DDevice9* device)
{
    if (!g_cfg.fpsCounter || g_failed || !g_shown || !g_vr.poseValid || !g_backBufferWidth || !g_backBufferHeight)
        return;
    // The panel stays where it is in front of the head: a point in head space goes to the game camera's space with the inverse of the head's delta, then into an eye like
    // anything the HUD draws (eyeCameraDelta is game camera -> eye, which holds the head's pose and the eye's place in it).
    Matrix4 headToCamera;
    if (!Invert(g_vr.eyeCameraDelta[2], headToCamera))
        return;
    if (!g_stateBlock && (FAILED(device->CreateStateBlock(D3DSBT_ALL, &g_stateBlock)) || !g_stateBlock)) {
        g_stateBlock = NULL;
        g_failed = true;
        Log("FPS counter: no state block (the counter stays off until the next Reset)");
        return;
    }
    IDirect3DSurface9* backBuffer = NULL;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        return;
    IDirect3DSurface9* savedTarget = NULL;
    device->GetRenderTarget(0, &savedTarget);
    PFN_SetRenderTarget setRenderTarget = (PFN_SetRenderTarget)g_deviceOriginals[VT_SetRenderTarget];
    PFN_SetViewport setViewport = (PFN_SetViewport)g_deviceOriginals[VT_SetViewport];
    PFN_SetRenderState setRenderState = (PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    PFN_SetTexture setTexture = (PFN_SetTexture)g_deviceOriginals[VT_SetTexture];
    PFN_SetVertexShader setVertexShader = (PFN_SetVertexShader)g_deviceOriginals[VT_SetVertexShader];
    PFN_SetPixelShader setPixelShader = (PFN_SetPixelShader)g_deviceOriginals[VT_SetPixelShader];
    PFN_DrawPrimitiveUP drawPrimitiveUP = (PFN_DrawPrimitiveUP)g_deviceOriginals[VT_DrawPrimitiveUP];

    // (the same way ComposeHudLayers draws: everything the game set is captured, the calls the mod tracks go to the original functions so the tracking never sees them, and the
    // state block puts the rest back)
    g_stateBlock->Capture();
    setRenderTarget(device, 0, backBuffer);
    D3DVIEWPORT9 fullViewport;
    fullViewport.X = 0;
    fullViewport.Y = 0;
    fullViewport.Width = g_backBufferWidth;
    fullViewport.Height = g_backBufferHeight;
    fullViewport.MinZ = 0;
    fullViewport.MaxZ = 1;
    setViewport(device, &fullViewport);
    setVertexShader(device, NULL);
    setPixelShader(device, NULL);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    setTexture(device, 0, NULL);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    for (DWORD stage = 1; stage < 4; stage++) {
        setTexture(device, stage, NULL);
        device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    setRenderState(device, D3DRS_FILLMODE, D3DFILL_SOLID);
    setRenderState(device, D3DRS_ZENABLE, FALSE);
    setRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
    setRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    setRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
    setRenderState(device, D3DRS_STENCILENABLE, FALSE);
    setRenderState(device, D3DRS_FOGENABLE, FALSE);
    setRenderState(device, D3DRS_LIGHTING, FALSE);
    setRenderState(device, D3DRS_SRGBWRITEENABLE, FALSE);
    setRenderState(device, D3DRS_COLORWRITEENABLE, 0xF);
    setRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
    setRenderState(device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    setRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    setRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE); // (the picture's own alpha stays as it is: the plate is only a darkening)
    setRenderState(device, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    setRenderState(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
    setRenderState(device, D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE);

    const int textCount = BuildTextRects(g_shown, g_textRects);
    const DWORD plateColor = D3DCOLOR_ARGB(150, 0, 0, 0), textColor = FpsCounterColor(g_shown, g_refreshHz);
    const double scale = g_cfg.positionScale > 0.01 ? g_cfg.positionScale : 1.0; // (head space is in game units: positionScale of them to the metre, like the eye offsets)
    const double bw = g_backBufferWidth, bh = g_backBufferHeight, eyeW = bw / 2.0;
    int drawnEyes = 0;
    bool ownScene = false;
    for (int eye = 0; eye < 2; eye++) {
        const Matrix4 toEye = Multiply(headToCamera, g_vr.eyeCameraDelta[eye]);
        const EyeBox& box = g_vr.eyeBox[eye];
        const double tanWidth = fmax(1e-3, box.right - box.left), tanHeight = fmax(1e-3, box.up + box.down);
        // a font pixel corner -> a pixel of this eye's picture
        bool visible = true;
        auto place = [&](int cellX, int cellY, PanelVertex& v, DWORD color) {
            const double z = kFpsDistance * scale;
            const double x = (kFpsCentreX + (cellX - kFpsPlateWidth / 2.0) * kFpsPixelTan) * z;
            const double y = (kFpsCentreY - (cellY - kFpsPlateHeight / 2.0) * kFpsPixelTan) * z;
            const double vx = x * toEye.m[0][0] + y * toEye.m[1][0] + z * toEye.m[2][0] + toEye.m[3][0];
            const double vy = x * toEye.m[0][1] + y * toEye.m[1][1] + z * toEye.m[2][1] + toEye.m[3][1];
            const double vz = x * toEye.m[0][2] + y * toEye.m[1][2] + z * toEye.m[2][2] + toEye.m[3][2];
            if (vz < 0.08)
                visible = false;
            v.x = (float)(eye * eyeW + (vx / vz - box.left) / tanWidth * eyeW - 0.5);
            v.y = (float)((box.up - vy / vz) / tanHeight * bh - 0.5);
            v.z = 0.0f;
            v.rhw = 1.0f;
            v.color = color;
        };
        int vertexCount = 0;
        auto addRect = [&](const FontRect& r, DWORD color) {
            PanelVertex a, b, c, d; // top left, top right, bottom left, bottom right
            place(r.x0, r.y0, a, color);
            place(r.x1, r.y0, b, color);
            place(r.x0, r.y1, c, color);
            place(r.x1, r.y1, d, color);
            g_vertices[vertexCount++] = a;
            g_vertices[vertexCount++] = b;
            g_vertices[vertexCount++] = c;
            g_vertices[vertexCount++] = b;
            g_vertices[vertexCount++] = d;
            g_vertices[vertexCount++] = c;
        };
        addRect({0, 0, kFpsPlateWidth, kFpsPlateHeight}, plateColor);
        for (int i = 0; i < textCount; i++)
            addRect(g_textRects[i], textColor);
        if (!visible)
            continue;
        RECT clip;
        clip.left = (LONG)(eye * eyeW);
        clip.right = (LONG)((eye + 1) * eyeW);
        clip.top = 0;
        clip.bottom = (LONG)bh;
        if (eye == 0)
            clip.right -= g_cfg.seamGutter;
        else
            clip.left += g_cfg.seamGutter;
        setScissorRect(device, &clip);
        HRESULT hr = drawPrimitiveUP(device, D3DPT_TRIANGLELIST, (UINT)vertexCount / 3, g_vertices, sizeof(PanelVertex));
        if (FAILED(hr) && SUCCEEDED(device->BeginScene())) { // (Present can come after the game's EndScene: the draw then needs a scene of its own)
            hr = drawPrimitiveUP(device, D3DPT_TRIANGLELIST, (UINT)vertexCount / 3, g_vertices, sizeof(PanelVertex));
            device->EndScene();
            ownScene = true;
        }
        if (SUCCEEDED(hr))
            drawnEyes++;
        else if (!g_drawFailureLogged) {
            g_drawFailureLogged = true;
            Log("FPS counter: drawing the panel failed 0x%08x (eye %d)", (unsigned)hr, eye);
        }
    }

    g_stateBlock->Apply();
    setRenderTarget(device, 0, savedTarget ? savedTarget : backBuffer);
    setViewport(device, &g_gameViewport);
    setRenderState(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
    setScissorRect(device, &g_gameScissorRect);
    if (savedTarget)
        savedTarget->Release();
    backBuffer->Release();
    if (!g_announced) {
        g_announced = true;
        Log("FPS counter: first panel drawn (%d fps, headset %.0f Hz, drawn in %d eye(s)%s)", g_shown, g_refreshHz, drawnEyes,
            ownScene ? ", in a scene of its own" : "");
    }
}
