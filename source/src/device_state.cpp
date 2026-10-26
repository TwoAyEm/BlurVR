// Lazy restore of the state the per-eye draws change.  See device_state.h.
#include "device_state.h"
#include "d3d_hooks.h"
#include "render_state.h"
#include "shader_tracking.h"
#include <algorithm>

typedef unsigned long long Mask256[4];

static float g_override[256 * 4];   // the values the device holds in the registers flagged in g_overridden
static Mask256 g_overridden;        // registers where the device holds g_override instead of the game's value
static bool g_vsUnknown = true;     // something else may have written vertex constants: the next call writes unconditionally

static bool g_enableOverridden = false; // the device has the scissor test on while the game has it off
static bool g_rectOverridden = false;   // the device has g_overrideRect while the game has g_gameScissorRect
static RECT g_overrideRect;
static bool g_scissorUnknown = true;

static float g_gameClipPlane[4];             // clip plane 0 as the game set it
static bool g_gameClipPlaneSet = false;
static bool g_gameClipPlaneEnabled = false;  // D3DRS_CLIPPLANEENABLE has bit 0 set
static bool g_clipOverridden = false;        // the device has g_overrideClipPlane while the game has g_gameClipPlane
static float g_overrideClipPlane[4];

static inline bool BitSet(const Mask256 mask, UINT reg)
{
    return (mask[reg >> 6] >> (reg & 63)) & 1;
}
static inline void SetBits(Mask256 mask, UINT start, UINT end) // [start, end), end <= 256
{
    UINT n = end - start;
    if (n < 64 - (start & 63)) { // inside one word: the usual case
        mask[start >> 6] |= ((1ull << n) - 1) << (start & 63);
        return;
    }
    for (UINT r = start; r < end;) {
        UINT bit = r & 63, n = std::min<UINT>(end - r, 64 - bit);
        mask[r >> 6] |= (n == 64 ? ~0ull : ((1ull << n) - 1)) << bit;
        r += n;
    }
}
static inline void ClearBits(Mask256 mask, UINT start, UINT end)
{
    for (UINT r = start; r < end;) {
        UINT bit = r & 63, n = std::min<UINT>(end - r, 64 - bit);
        mask[r >> 6] &= ~((n == 64 ? ~0ull : ((1ull << n) - 1)) << bit);
        r += n;
    }
}
static inline bool AnyBit(const Mask256 mask)
{
    return (mask[0] | mask[1] | mask[2] | mask[3]) != 0;
}

// Calls visit(a, b) for every run [a, b) of consecutive set bits.
template <typename Visit>
static void ForEachRun(const Mask256 mask, Visit visit)
{
    UINT runStart = 0, runEnd = 0;
    bool open = false;
    for (UINT word = 0; word < 4; word++) {
        unsigned long long bits = mask[word];
        UINT pos = 0;
        while (pos < 64) {
            unsigned long long rest = bits >> pos;
            if (!rest)
                break;
            UINT start = pos + (UINT)__builtin_ctzll(rest);
            unsigned long long shifted = bits >> start;
            UINT length = (~shifted == 0) ? 64 - start : (UINT)__builtin_ctzll(~shifted);
            UINT a = word * 64 + start, b = a + length;
            if (open && runEnd == a)
                runEnd = b;
            else {
                if (open)
                    visit(runStart, runEnd);
                runStart = a;
                runEnd = b;
                open = true;
            }
            pos = start + length;
        }
    }
    if (open)
        visit(runStart, runEnd);
}

void DeviceNoteGameVsConstants(UINT start, UINT count)
{
    if (start >= 256 || !AnyBit(g_overridden))
        return;
    UINT end = (count > 256 - start) ? 256 : start + count;
    ClearBits(g_overridden, start, end);
}

// Give the registers in `restore` (a subset of the overridden ones) the game's values again.
static void RestoreRegisters(IDirect3DDevice9* device, const Mask256 restore)
{
    PFN_SetShaderConstantF setConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetVertexShaderConstantF];
    Mask256 differ = {0, 0, 0, 0};
    for (UINT word = 0; word < 4; word++)
        for (unsigned long long bits = restore[word]; bits; bits &= bits - 1) {
            UINT reg = word * 64 + (UINT)__builtin_ctzll(bits);
            if (memcmp(&g_override[reg * 4], &g_vsConstants[reg * 4], 16) != 0)
                differ[word] |= 1ull << (reg & 63);
        }
    ForEachRun(differ, [&](UINT a, UINT b) { setConstants(device, a, &g_vsConstants[a * 4], b - a); });
    for (UINT word = 0; word < 4; word++)
        g_overridden[word] &= ~restore[word];
}

void DeviceSetVsOverrides(IDirect3DDevice9* device, const VsWrite* writes, int count)
{
    // registers overridden earlier that this call does not write go back to the game's values
    if (AnyBit(g_overridden)) {
        Mask256 want = {0, 0, 0, 0};
        for (int i = 0; i < count; i++)
            if (writes[i].reg < 256 && writes[i].count <= 256 - writes[i].reg)
                SetBits(want, writes[i].reg, writes[i].reg + writes[i].count);
        Mask256 restore;
        for (UINT word = 0; word < 4; word++)
            restore[word] = g_overridden[word] & ~want[word];
        if (AnyBit(restore))
            RestoreRegisters(device, restore);
    }
    PFN_SetShaderConstantF setConstants = (PFN_SetShaderConstantF)g_deviceOriginals[VT_SetVertexShaderConstantF];
    for (int i = 0; i < count; i++) {
        const VsWrite& w = writes[i];
        if (w.reg >= 256 || w.count > 256 - w.reg)
            continue;
        // the registers of this write that the device does not already hold with these values: one call for the span
        UINT first = w.count, last = 0;
        for (UINT k = 0; k < w.count; k++) {
            UINT reg = w.reg + k;
            const float* held = BitSet(g_overridden, reg) ? &g_override[reg * 4] : &g_vsConstants[reg * 4];
            if (g_vsUnknown || memcmp(held, &w.data[k * 4], 16) != 0) {
                if (first == w.count)
                    first = k;
                last = k;
            }
        }
        if (first == w.count)
            continue;
        UINT reg = w.reg + first, n = last - first + 1;
        setConstants(device, reg, &w.data[first * 4], n);
        memcpy(&g_override[reg * 4], &w.data[first * 4], n * 16);
        SetBits(g_overridden, reg, reg + n);
    }
    if (count > 0)
        g_vsUnknown = false;
}

void DeviceNoteGameClipPlane(const float* plane)
{
    memcpy(g_gameClipPlane, plane, sizeof(g_gameClipPlane));
    g_gameClipPlaneSet = true;
    g_clipOverridden = false;
}
void DeviceNoteGameClipPlaneEnable(DWORD enable)
{
    g_gameClipPlaneEnabled = (enable & 1) != 0;
}
bool DeviceGameClipPlaneActive()
{
    return g_gameClipPlaneEnabled && g_gameClipPlaneSet;
}
const float* DeviceGameClipPlane()
{
    return g_gameClipPlane;
}
void DeviceSetEyeClipPlane(IDirect3DDevice9* device, const float* plane)
{
    const float* held = g_clipOverridden ? g_overrideClipPlane : g_gameClipPlane;
    if (memcmp(held, plane, sizeof(g_gameClipPlane)) != 0)
        ((PFN_SetClipPlane)g_deviceOriginals[VT_SetClipPlane])(device, 0, plane);
    memcpy(g_overrideClipPlane, plane, sizeof(g_overrideClipPlane));
    g_clipOverridden = memcmp(plane, g_gameClipPlane, sizeof(g_gameClipPlane)) != 0;
}

void DeviceNoteGameScissorEnable(bool)
{
    g_enableOverridden = false;
}
void DeviceNoteGameScissorRect(const RECT&)
{
    g_rectOverridden = false;
}

void DeviceSetScissorEnabled(IDirect3DDevice9* device, bool enabled)
{
    bool game = g_gameScissorEnabled != FALSE;
    bool held = game != g_enableOverridden; // overridden means the device holds the opposite of the game's value
    if (g_scissorUnknown || held != enabled)
        ((PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState])(device, D3DRS_SCISSORTESTENABLE, enabled ? TRUE : FALSE);
    g_enableOverridden = enabled != game;
}

void DeviceSetEyeScissor(IDirect3DDevice9* device, const RECT& rect)
{
    DeviceSetScissorEnabled(device, true);
    const RECT& held = g_rectOverridden ? g_overrideRect : g_gameScissorRect;
    if (g_scissorUnknown || memcmp(&held, &rect, sizeof(RECT)) != 0)
        ((PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect])(device, &rect);
    g_scissorUnknown = false;
    g_rectOverridden = memcmp(&rect, &g_gameScissorRect, sizeof(RECT)) != 0;
    g_overrideRect = rect;
}

void DeviceRestoreAll(IDirect3DDevice9* device)
{
    if (AnyBit(g_overridden)) {
        Mask256 all;
        memcpy(all, g_overridden, sizeof(all));
        RestoreRegisters(device, all);
    }
    if (g_enableOverridden) {
        ((PFN_SetRenderState)g_deviceOriginals[VT_SetRenderState])(device, D3DRS_SCISSORTESTENABLE, g_gameScissorEnabled);
        g_enableOverridden = false;
    }
    if (g_rectOverridden) {
        ((PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect])(device, &g_gameScissorRect);
        g_rectOverridden = false;
    }
    if (g_clipOverridden) {
        ((PFN_SetClipPlane)g_deviceOriginals[VT_SetClipPlane])(device, 0, g_gameClipPlane);
        g_clipOverridden = false;
    }
    // whatever runs next may write constants or scissor state on its own: do not trust the skip logic for the next write
    g_vsUnknown = true;
    g_scissorUnknown = true;
}

void DeviceStateReset()
{
    memset(g_overridden, 0, sizeof(g_overridden));
    g_enableOverridden = g_rectOverridden = false;
    g_clipOverridden = g_gameClipPlaneSet = g_gameClipPlaneEnabled = false; // (a reset device has no clip planes)
    g_vsUnknown = g_scissorUnknown = true;
}
