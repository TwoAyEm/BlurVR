// Stereo duplication of main-pass draws: rewriting the tracked shader matrices for one eye.
#pragma once
#include "common.h"
#include "matrix.h"

extern int g_decomposeFailLogs;
// Rewrite the tracked view/projection constants for the given eye (0 = left, 1 = right) for the draw about to be issued.
// The device keeps these values until the next call or until DeviceRestoreAll (device_state.h) puts the game's back.
void ApplyEyeConstants(IDirect3DDevice9* device, int eye);
// Put the game's own constants back at once.
void RestoreEyeConstants(IDirect3DDevice9* device);
// Shift the camera of the current draw: rewrite the tracked matrices with `delta` (camera-space change, row vectors)
// and the projection `newProj` instead of the game's `oldProj`.  Undo with RestoreEyeConstants.
void ApplyCameraShift(IDirect3DDevice9* device, const Matrix4& oldProj, const Matrix4& delta, const Matrix4& newProj);
