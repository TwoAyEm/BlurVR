// Makes the game see the video mode it saved as the size of its window, until it has created its device.
//
// The game asks Windows for a window of its saved size (settings.xml), Windows shrinks a window to the desktop (two 1080p monitors: 3854x1071 of
// client area), the game reads that client area and builds its device and every render target from it, so a saved 3840x2160 or 4936x2740 started as
// a 3.6:1 picture.  Selecting the same size in the options menu works because the game then passes the size itself.  Answering the game's
// GetClientRect with that size (and the same in its WM_SIZE messages, which it answers with a Reset to the window's size 0.9 s after the start)
// gives it the same state at the start and keeps it.  Only the game's own big top-level window is touched, and only while it is under 90 percent
// of the size the game last asked for in either direction.
#pragma once
#include "common.h"

// The saved video mode (width, height) from the game's settings.xml, and whether it is a windowed mode; false when it cannot be read.
bool ReadSavedVideoSize(UINT* width, UINT* height, bool* windowed = NULL);
// Patch the user32 imports of the game's executable (DllMain) or of every loaded module (Direct3DCreate9).  Safe to repeat.
void InstallWindowSizeSpoof(bool allModules);
// The size of the picture the game last asked for (CreateDevice / Reset, windowed, explicit): what its window should look like to it.
void WindowSpoofIntendedSize(UINT width, UINT height);
// Windowed or full screen, as the game last asked: the window is only kept within its monitor while the game is windowed.
void WindowSpoofWindowed(bool windowed);
// The device exists (only changes what is logged).
void WindowSpoofDeviceCreated();

// [Render] FitDesktopWindow: the window is only a mirror of the headset, so it is kept within the work area of its monitor (the game asks for
// the size of its video mode, 3840x2160 and more, and Windows would make it as large as the whole desktop): made so when it is created, and
// every later size the game asks for (WM_WINDOWPOSCHANGING) is cut to it.  The game is not told: it still sees the size it asked for.
