#include "frame_dump.h"
#include <stdio.h>
#include "config.h"
#include "core.h"
#include "game_mode.h"
#include "hud.h"
#include "render_state.h"
#include "vr_runtime.h"

#pragma pack(push, 1)
struct BmpHeader {
    unsigned short type;
    unsigned fileSize;
    unsigned short reserved1, reserved2;
    unsigned dataOffset;
    unsigned headerSize;
    int width, height; // a negative height = rows from the top
    unsigned short planes, bitCount;
    unsigned compression, imageSize;
    int xPixelsPerMeter, yPixelsPerMeter;
    unsigned colorsUsed, colorsImportant;
};
#pragma pack(pop)

// Save a 32-bit surface as a top-down .bmp (the alpha channel is stored too; every second pixel and row of a picture wider than 2000
// pixels, to keep the files small).  Goes through a plain render target first: the back buffer may be multisampled, and a layer is not
// lockable.
static bool SaveSurface(IDirect3DDevice9* device, IDirect3DSurface9* source, const char* path)
{
    if (!source)
        return false;
    D3DSURFACE_DESC desc;
    if (FAILED(source->GetDesc(&desc)) || (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8))
        return false;
    IDirect3DSurface9* copy = NULL;
    IDirect3DSurface9* system = NULL;
    bool saved = false;
    if (SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &copy, NULL)) &&
        SUCCEEDED(device->StretchRect(source, NULL, copy, NULL, D3DTEXF_NONE)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, NULL)) &&
        SUCCEEDED(device->GetRenderTargetData(copy, system))) {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(system->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
            FILE* file = fopen(path, "wb");
            UINT step = desc.Width > 2000 ? 2 : 1;
            if (file) {
                BmpHeader header;
                memset(&header, 0, sizeof(header));
                header.type = 0x4D42;
                header.dataOffset = sizeof(header);
                header.headerSize = sizeof(header) - 14;
                UINT outWidth = desc.Width / step, outHeight = desc.Height / step;
                header.width = (int)outWidth;
                header.height = -(int)outHeight;
                header.planes = 1;
                header.bitCount = 32;
                header.imageSize = outWidth * outHeight * 4;
                header.fileSize = sizeof(header) + header.imageSize;
                fwrite(&header, sizeof(header), 1, file);
                for (UINT y = 0; y < outHeight; y++) {
                    const unsigned* row = (const unsigned*)((const BYTE*)locked.pBits + (size_t)(y * step) * locked.Pitch);
                    if (step == 1)
                        fwrite(row, 4, outWidth, file);
                    else
                        for (UINT x = 0; x < outWidth; x++)
                            fwrite(&row[x * step], 4, 1, file);
                }
                fclose(file);
                saved = true;
            }
            system->UnlockRect();
        }
    }
    if (system)
        system->Release();
    if (copy)
        copy->Release();
    return saved;
}

void FrameDumpPoll(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer)
{
    static bool keyWasDown = false;
    static int dumps = 0;
    bool keyDown = g_cfg.keyDump && (GetAsyncKeyState(g_cfg.keyDump) & 0x8000) != 0;
    bool byKey = keyDown && !keyWasDown;
    keyWasDown = keyDown;
    if (!byKey || dumps >= 12)
        return;
    dumps++;
    SYSTEMTIME now;
    GetLocalTime(&now);
    char stamp[48], path[MAX_PATH + 96];
    snprintf(stamp, sizeof(stamp), "%02d%02d%02d", now.wHour, now.wMinute, now.wSecond);
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    snprintf(path, sizeof(path), "%sBlurVR_dump_%s_picture_%ux%u.bmp", g_modDir, stamp, desc.Width, desc.Height);
    bool picture = SaveSurface(device, backBuffer, path);
    Log("frame dump (%s): picture %s -> %s, back buffer %ux%u, level %s, scene draws %u, hud used %d, top layer %d, mirror %d/%d, "
        "eye boxes L %.3f %.3f %.3f %.3f  R %.3f %.3f %.3f %.3f",
        "key", path, picture ? "saved" : "FAILED", desc.Width, desc.Height, g_currentLevel,
        g_sceneDrawsThisFrame, (int)g_hudUsed, (int)g_topLayerUsed, (int)g_mirrorUsed[0], (int)g_mirrorUsed[1],
        g_vr.eyeBox[0].left, g_vr.eyeBox[0].right, g_vr.eyeBox[0].up, g_vr.eyeBox[0].down, g_vr.eyeBox[1].left,
        g_vr.eyeBox[1].right, g_vr.eyeBox[1].up, g_vr.eyeBox[1].down);
    struct Layer {
        const char* name;
        IDirect3DSurface9* surface;
        bool used;
    } layers[] = {{"hud", g_hudSurface, true},
                  {"top", g_topLayerSurface, true},
                  {"multiply", g_multiplySurface, true},
                  {"mirror0", g_mirrorSurface[0], g_mirrorUsed[0]},
                  {"mirror1", g_mirrorSurface[1], g_mirrorUsed[1]}};
    for (const Layer& layer : layers) {
        if (!layer.surface || !layer.used)
            continue;
        snprintf(path, sizeof(path), "%sBlurVR_dump_%s_%s.bmp", g_modDir, stamp, layer.name);
        Log("frame dump: layer %s -> %s", layer.name, SaveSurface(device, layer.surface, path) ? "saved" : "FAILED");
    }
}
