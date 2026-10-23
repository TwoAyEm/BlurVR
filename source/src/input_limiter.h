// Menu key repeat.  Blur makes its own auto-repeat in the menus: it counts how long a key has been held in frames and, after a second, repeats the
// press every frame, so at 300 fps a held arrow key scrolls a menu 300 times a second (the game was tuned for 30).  The keyboard is polled, so the
// cure is at the poll: in the menus a held key reaches the game as a tap, then, after a delay, as a tap every [Input] KeyRepeatMs, whatever the frame
// rate.  The game never sees the key held, so its own repeat never starts.  In a race the keys pass untouched (the arrow keys steer there), except while a pause or results menu is up over it (RaceMenuOverlay).
//
// Blur.exe polls the keyboard through DirectInput (GetDeviceState on the keyboard device) or GetKeyState; both are filtered, and BlurVR.log (Log=1)
// says which one the game uses ("input: ...").  The pad (XInputGetState) gets the same rule for its buttons, D-pad and left stick.
#pragma once
#include "common.h"

// Hook DirectInput8Create and GetKeyState in the game's executable.  DllMain; again from Direct3DCreate9 (idempotent).  Does nothing when
// [Input] KeyRepeatMs is 0 or the mod is disabled.
void InstallInputHooks();
// Once per presented frame: the frame time the taps are sized by, and whether the menus are up.
void InputLimiterFrame();

// The rule itself, for tests: is a key that has been physically down for heldMs shown as down?  The first press is a tap, nothing for the delay, then
// a tap every 2 taps.  A tap is half the interval, at least two frames long (frameMs) so that a poll cannot fall between the taps.
bool KeyRepeatShownDown(double heldMs, double intervalMs, double frameMs);
