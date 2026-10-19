// Lock pitch of the textures that SharedTransport moved out of the managed pool.  See texture_pitch.h.
#include "texture_pitch.h"
#include "core.h"
#include "d3d_hooks.h"

typedef HRESULT(STDMETHODCALLTYPE* PFN_TexLockRect)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_TexUnlockRect)(IDirect3DTexture9*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CubeLockRect)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*,
                                                    const RECT*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CubeUnlockRect)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_VolLockBox)(IDirect3DVolumeTexture9*, UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_VolUnlockBox)(IDirect3DVolumeTexture9*, UINT);

static const int kLockRectSlot = 19, kUnlockRectSlot = 20; // IDirect3DTexture9 and IDirect3DCubeTexture9

struct TextureInfo {
    UINT width, height, bytesPerPixel;
    UINT depth; // volume textures; 0 for the others
};
struct PendingLock {
    BYTE* realBase;  // the surface's first pixel in the memory the driver locked
    INT realPitch, realSlice;
    BYTE* staging;   // what the game writes into, tight pitch
    UINT tightPitch, rows, slices;
    bool readOnly;
};
static std::unordered_map<void*, TextureInfo> g_textures;      // remapped textures by pointer
static std::unordered_map<unsigned long long, PendingLock> g_pending; // locks in progress, by texture and level
static PFN_TexLockRect g_texLock = NULL;
static PFN_TexUnlockRect g_texUnlock = NULL;
static PFN_CubeLockRect g_cubeLock = NULL;
static PFN_CubeUnlockRect g_cubeUnlock = NULL;
static PFN_VolLockBox g_volLock = NULL;
static PFN_VolUnlockBox g_volUnlock = NULL;
static unsigned g_stagedLocks = 0;

// Bytes per pixel of the uncompressed formats a texture loader is likely to write by hand; 0 = compressed or not known (left alone).
static UINT BytesPerPixel(D3DFORMAT format)
{
    switch (format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_G16R16:
        case D3DFMT_R32F:
        case D3DFMT_G16R16F:
            return 4;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8L8:
        case D3DFMT_V8U8:
        case D3DFMT_L16:
        case D3DFMT_R16F:
        case D3DFMT_A8R3G3B2:
            return 2;
        case D3DFMT_L8:
        case D3DFMT_A8:
        case D3DFMT_P8:
        case D3DFMT_R3G3B2:
        case D3DFMT_A4L4:
            return 1;
        case D3DFMT_A16B16G16R16:
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_G32R32F:
            return 8;
        case D3DFMT_A32B32G32R32F:
            return 16;
        default:
            return 0;
    }
}

static unsigned long long PendingKey(void* texture, UINT face, UINT level)
{
    return ((unsigned long long)(uintptr_t)texture << 32) ^ ((unsigned long long)face << 16) ^ level;
}

// After the real lock succeeded: if its pitch is not the tight one, hand the game a tight staging copy of the level.
static void Tighten(const TextureInfo& info, void* texture, UINT face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                    DWORD flags)
{
    UINT levelWidth = info.width >> level, levelHeight = info.height >> level;
    if (levelWidth < 1)
        levelWidth = 1;
    if (levelHeight < 1)
        levelHeight = 1;
    UINT tight = levelWidth * info.bytesPerPixel;
    if ((UINT)locked->Pitch == tight)
        return;
    BYTE* base = (BYTE*)locked->pBits;
    if (rect)
        base -= (size_t)rect->top * locked->Pitch + (size_t)rect->left * info.bytesPerPixel;
    BYTE* staging = (BYTE*)malloc((size_t)tight * levelHeight);
    if (!staging)
        return;
    if (!(flags & D3DLOCK_DISCARD))
        for (UINT y = 0; y < levelHeight; y++)
            memcpy(staging + (size_t)y * tight, base + (size_t)y * locked->Pitch, tight);
    PendingLock pending = {base, locked->Pitch, 0, staging, tight, levelHeight, 1, (flags & D3DLOCK_READONLY) != 0};
    auto previous = g_pending.find(PendingKey(texture, face, level));
    if (previous != g_pending.end()) {
        free(previous->second.staging);
        previous->second = pending;
    } else
        g_pending[PendingKey(texture, face, level)] = pending;
    locked->pBits = staging + (rect ? (size_t)rect->top * tight + (size_t)rect->left * info.bytesPerPixel : 0);
    locked->Pitch = (INT)tight;
    if (g_stagedLocks++ == 0)
        Log("SharedTransport: a texture level was locked with a different row pitch than the managed pool gave (real %d, "
            "tight %u): such locks now go through a staging buffer",
            (int)pending.realPitch, tight);
}

// On unlock: copy what the game wrote back into the real surface.
static void CopyBack(void* texture, UINT face, UINT level)
{
    auto it = g_pending.find(PendingKey(texture, face, level));
    if (it == g_pending.end())
        return;
    PendingLock& pending = it->second;
    if (!pending.readOnly)
        for (UINT z = 0; z < pending.slices; z++)
            for (UINT y = 0; y < pending.rows; y++)
                memcpy(pending.realBase + (size_t)z * pending.realSlice + (size_t)y * pending.realPitch,
                       pending.staging + ((size_t)z * pending.rows + y) * pending.tightPitch, pending.tightPitch);
    free(pending.staging);
    g_pending.erase(it);
}

// The same for a volume texture: tight rows and tight slices.
static void TightenVolume(const TextureInfo& info, void* texture, UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box,
                          DWORD flags)
{
    UINT w = info.width >> level, h = info.height >> level, d = info.depth >> level;
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
    d = d < 1 ? 1 : d;
    UINT tight = w * info.bytesPerPixel;
    if ((UINT)locked->RowPitch == tight && (UINT)locked->SlicePitch == tight * h)
        return;
    BYTE* base = (BYTE*)locked->pBits;
    if (box)
        base -= (size_t)box->Front * locked->SlicePitch + (size_t)box->Top * locked->RowPitch +
                (size_t)box->Left * info.bytesPerPixel;
    BYTE* staging = (BYTE*)malloc((size_t)tight * h * d);
    if (!staging)
        return;
    if (!(flags & D3DLOCK_DISCARD))
        for (UINT z = 0; z < d; z++)
            for (UINT y = 0; y < h; y++)
                memcpy(staging + ((size_t)z * h + y) * tight, base + (size_t)z * locked->SlicePitch + (size_t)y * locked->RowPitch,
                       tight);
    PendingLock pending = {base, locked->RowPitch, locked->SlicePitch, staging, tight, h, d, (flags & D3DLOCK_READONLY) != 0};
    auto previous = g_pending.find(PendingKey(texture, 0x7000, level));
    if (previous != g_pending.end()) {
        free(previous->second.staging);
        previous->second = pending;
    } else
        g_pending[PendingKey(texture, 0x7000, level)] = pending;
    locked->pBits = staging + (box ? ((size_t)box->Front * h + box->Top) * tight + (size_t)box->Left * info.bytesPerPixel : 0);
    locked->RowPitch = (INT)tight;
    locked->SlicePitch = (INT)(tight * h);
    if (g_stagedLocks++ == 0)
        Log("SharedTransport: a texture level was locked with a different row pitch than the managed pool gave: such locks now "
            "go through a staging buffer");
}

static HRESULT STDMETHODCALLTYPE H_TexLockRect(IDirect3DTexture9* texture, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                                               DWORD flags)
{
    HRESULT hr = g_texLock(texture, level, locked, rect, flags);
    if (SUCCEEDED(hr) && locked) {
        auto it = g_textures.find(texture);
        if (it != g_textures.end())
            Tighten(it->second, texture, 0, level, locked, rect, flags);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_TexUnlockRect(IDirect3DTexture9* texture, UINT level)
{
    if (!g_pending.empty())
        CopyBack(texture, 0, level);
    return g_texUnlock(texture, level);
}
static HRESULT STDMETHODCALLTYPE H_CubeLockRect(IDirect3DCubeTexture9* texture, D3DCUBEMAP_FACES face, UINT level,
                                                D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    HRESULT hr = g_cubeLock(texture, face, level, locked, rect, flags);
    if (SUCCEEDED(hr) && locked) {
        auto it = g_textures.find(texture);
        if (it != g_textures.end())
            Tighten(it->second, texture, (UINT)face + 1, level, locked, rect, flags);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CubeUnlockRect(IDirect3DCubeTexture9* texture, D3DCUBEMAP_FACES face, UINT level)
{
    if (!g_pending.empty())
        CopyBack(texture, (UINT)face + 1, level);
    return g_cubeUnlock(texture, face, level);
}

static HRESULT STDMETHODCALLTYPE H_VolLockBox(IDirect3DVolumeTexture9* texture, UINT level, D3DLOCKED_BOX* locked,
                                              const D3DBOX* box, DWORD flags)
{
    HRESULT hr = g_volLock(texture, level, locked, box, flags);
    if (SUCCEEDED(hr) && locked) {
        auto it = g_textures.find(texture);
        if (it != g_textures.end())
            TightenVolume(it->second, texture, level, locked, box, flags);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_VolUnlockBox(IDirect3DVolumeTexture9* texture, UINT level)
{
    if (!g_pending.empty())
        CopyBack(texture, 0x7000, level);
    return g_volUnlock(texture, level);
}

void NoteTextureCreated(IDirect3DTexture9* texture, UINT width, UINT height, D3DFORMAT format, bool remapped)
{
    if (!texture)
        return;
    UINT bytes = remapped ? BytesPerPixel(format) : 0;
    if (!bytes) {
        g_textures.erase(texture); // (a pointer can be reused by a texture that needs nothing)
        return;
    }
    if (!g_texLock) { // all textures of the device share one vtable
        void** vtable = *(void***)texture;
        PatchVtable(vtable, kLockRectSlot, (void*)H_TexLockRect, (void**)&g_texLock);
        PatchVtable(vtable, kUnlockRectSlot, (void*)H_TexUnlockRect, (void**)&g_texUnlock);
    }
    g_textures[texture] = {width, height, bytes, 0};
}
void NoteVolumeTextureCreated(IDirect3DVolumeTexture9* texture, UINT width, UINT height, UINT depth, D3DFORMAT format,
                              bool remapped)
{
    if (!texture)
        return;
    UINT bytes = remapped ? BytesPerPixel(format) : 0;
    if (!bytes) {
        g_textures.erase(texture);
        return;
    }
    if (!g_volLock) {
        void** vtable = *(void***)texture;
        PatchVtable(vtable, kLockRectSlot, (void*)H_VolLockBox, (void**)&g_volLock); // (LockBox: the same slot numbers)
        PatchVtable(vtable, kUnlockRectSlot, (void*)H_VolUnlockBox, (void**)&g_volUnlock);
    }
    g_textures[texture] = {width, height, bytes, depth};
}
void NoteCubeTextureCreated(IDirect3DCubeTexture9* texture, UINT edge, D3DFORMAT format, bool remapped)
{
    if (!texture)
        return;
    UINT bytes = remapped ? BytesPerPixel(format) : 0;
    if (!bytes) {
        g_textures.erase(texture);
        return;
    }
    if (!g_cubeLock) {
        void** vtable = *(void***)texture;
        PatchVtable(vtable, kLockRectSlot, (void*)H_CubeLockRect, (void**)&g_cubeLock);
        PatchVtable(vtable, kUnlockRectSlot, (void*)H_CubeUnlockRect, (void**)&g_cubeUnlock);
    }
    g_textures[texture] = {edge, edge, bytes, 0};
}

void ReleaseTexturePitchState()
{
    for (auto& item : g_pending)
        free(item.second.staging);
    g_pending.clear();
}
