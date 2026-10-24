// The window on the desktop with [VR] DesktopView=off: "Playing in VR" and the game's logo instead of the game's picture (NOTES.md section 20).
#pragma once
#include "common.h"

// True while the window shows the message: DesktopView=off, the headset is running, and Present can take a source rectangle (a windowed device).
bool DesktopMessageWanted();
// Is a Present of the message due?  The first one, one when the window's size changed, then one every half second; the game's own Presents in between are skipped.
// False while the window is minimised (nothing to show).
bool DesktopMessageDue(IDirect3DDevice9* device);
// Paints the message into the top-left corner of the back buffer at the window's own size and returns that rectangle, to be Present's source rectangle.  When it cannot
// (a back buffer format it does not know, a surface that cannot be made) the setting falls back to the left eye's view and this returns false.
bool DesktopMessagePaint(IDirect3DDevice9* device, RECT* presented);
// The picture as pixels (top-down, 0x00RRGGBB with the top byte 0xFF), `logo` an icon or NULL.  Exposed for tools/desktop_message_check.cpp.
bool DesktopMessageBuildPicture(int width, int height, HICON logo, std::vector<unsigned>& pixels);
// The game's own icon: the first icon of the executable (`module` NULL) or of another module, or NULL.
HICON DesktopMessageLoadLogo(HMODULE module);
// The surfaces belong to the device: released before every Reset.
void ReleaseDesktopMessage();
