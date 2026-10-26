// What the device holds versus what the game believes, for the state the per-eye draws change: the vertex-shader float
// constants and the scissor test.
//
// A draw that is repeated per eye rewrites tracked matrices and the scissor rectangle.  Instead of putting the game's values
// back after every eye, the mod keeps track of what it has overridden and puts the game's values back only when something
// other than another per-eye draw is about to use the device (DeviceRestoreAll), and it skips every write that would not
// change what the device holds.  At every draw the device holds exactly what it would have held when the game's values are
// restored after each eye; only the number of calls is smaller.
#pragma once
#include "common.h"

struct VsWrite {
    UINT reg, count;
    const float* data;
};

// The game wrote vertex constants [start, start + count) (its copy g_vsConstants is already updated, the call goes on to the device).
void DeviceNoteGameVsConstants(UINT start, UINT count);
// From now on the device holds the game's vertex constants except for these registers, which hold the given values.
void DeviceSetVsOverrides(IDirect3DDevice9* device, const VsWrite* writes, int count);

// The game set clip plane 0 (a, b, c, d in the clip space of the camera it was drawn through: the vertex shaders' output is clipped to
// a*x + b*y + c*z + d*w >= 0) or switched clip planes on or off (the call goes on to the device).  The reflection passes use one.
void DeviceNoteGameClipPlane(const float* plane);
void DeviceNoteGameClipPlaneEnable(DWORD enable);
// Is the game's clip plane 0 on (and has it set one)?  And the game's plane.
bool DeviceGameClipPlaneActive();
const float* DeviceGameClipPlane();
// From now on the device holds `plane` as clip plane 0 (the game's is put back by DeviceRestoreAll).
void DeviceSetEyeClipPlane(IDirect3DDevice9* device, const float* plane);

// The game set the scissor test or its rectangle (the call goes on to the device; g_gameScissor* are already updated).
void DeviceNoteGameScissorEnable(bool enabled);
void DeviceNoteGameScissorRect(const RECT& rect);
// From now on the scissor test is on with this rectangle (and the game's values are restored by DeviceRestoreAll).
void DeviceSetEyeScissor(IDirect3DDevice9* device, const RECT& rect);
// The scissor test is switched off or on for the mod's own use (the game's value is restored by DeviceRestoreAll).
void DeviceSetScissorEnabled(IDirect3DDevice9* device, bool enabled);

// Put the game's vertex constants, scissor state and clip plane back on the device.  Call it before anything other than a per-eye draw
// uses the device: other draws, clears, a render target change, Present.
void DeviceRestoreAll(IDirect3DDevice9* device);
// The device was reset: its state is back to the defaults and the game uploads everything again.
void DeviceStateReset();
