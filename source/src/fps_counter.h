// Frame rate counter in the headset ([VR] FpsCounter, numpad 9 in game): how many frames the game presents a second, painted into both eyes of a race frame as a
// small panel that stays where you look.  NOTES.md section 27.
#pragma once
#include "common.h"

// Called once per presented frame (VrSubmitFrame).  Counts the frame and, twice a second, takes the figure the panel shows.  A branch while the counter is off.
void FpsCounterFrame();
// Race frames, once the HUD is composed and before the picture goes to the headset: paints the panel into both eyes of the back buffer.  Does nothing while the
// counter is off or has no figure yet.
void FpsCounterDraw(IDirect3DDevice9* device);
// The state block belongs to the device: released before every Reset.
void ReleaseFpsCounter();

// ---- exposed for tools/fps_counter_check.cpp --------------------------------------------------------------------------------------------------------------
// A frame presented at `ticks` of a clock that runs `perSecond` ticks a second (FpsCounterFrame reads QueryPerformanceCounter and calls this).
void FpsCounterFrameAt(long long ticks, long long perSecond);
// The figure the panel shows (frames a second, 0 = none yet).
int FpsCounterShown();
// The colour of the digits: green at the headset's refresh rate, yellow below it, red far below (a headset at 90 Hz that is held to 45 has half its frames).  White when the
// refresh rate is not known.
D3DCOLOR FpsCounterColor(int fps, double refreshHz);
// The 5 x 7 pixels of a character of the font (0-9, F, P, S), one byte per row with the leftmost pixel in bit 4; false for any other character.
bool FpsCounterGlyph(char c, unsigned char rows[7]);
// The layout, in font pixels (the plate behind the text first, then the text, which is right-aligned digits and the label):
const int kFpsGlyphWidth = 5, kFpsGlyphHeight = 7;
const int kFpsMargin = 2;     // the plate reaches this far round the text
const int kFpsAdvance = 6;    // from one character to the next
const int kFpsDigits = 3;     // room for 999
const int kFpsLabelGap = 2;   // after the last digit's slot
const int kFpsPlateWidth = 2 * kFpsMargin + kFpsDigits * kFpsAdvance + kFpsLabelGap + 3 * kFpsAdvance - 1;
const int kFpsPlateHeight = 2 * kFpsMargin + kFpsGlyphHeight;
// Where the panel is: its centre as tangents of the angle from straight ahead (+x right, +y up), how far away it seems to be (metres) and how big a font pixel is (a tangent).
const double kFpsCentreX = -0.34, kFpsCentreY = -0.30, kFpsDistance = 1.5, kFpsPixelTan = 0.005;
