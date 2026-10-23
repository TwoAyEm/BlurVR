// Extra display mode for the game's resolution list, and the IDirect3D9 hooks.
#pragma once
#include "common.h"

// The desktop mirror (DesktopView) paints a window-sized piece of the back buffer and presents only that piece.  Present takes a source rectangle
// only with the COPY swap effect, which AdjustPresentParams asks for when it can; g_desktopRectUsable says it did, g_desktopRectFailed that the
// driver turned the rectangle down anyway (the whole back buffer is painted and presented then).
extern bool g_desktopRectUsable;
extern bool g_desktopRectFailed;
// Make a request for the custom resolution windowed when the monitor has no such mode.  isReset: the game is switching modes on a running
// device (the options menu), not creating the first one; in VR a mode picked then becomes the Resolution (video_settings.cpp).
void AdjustPresentParams(D3DPRESENT_PARAMETERS* pp, bool isReset = false);
// Patch the IDirect3D9 vtable: extra display mode, CreateDevice.
void HookDirect3D(IDirect3D9* d3d);
