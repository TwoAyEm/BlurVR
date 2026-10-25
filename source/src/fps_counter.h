// Frame rate counter in the headset ([VR] FpsCounter, numpad 9 in game): how many frames the game presents a second, painted into both eyes of a 3D frame (a race or the showroom) as a
// small panel that stays where you look, with a graph of the last seconds under the figure.  NOTES.md section 27.
#pragma once
#include "common.h"

// Called once per presented frame (VrSubmitFrame).  Counts the frame and, twice a second, takes the figure the panel shows (and four times a second a sample for the graph).  A branch
// while the counter is off.
void FpsCounterFrame();
// 3D frames (races, the showroom), once the HUD is composed and before the picture goes to the headset: paints the panel into both eyes of the back buffer.  Does nothing while the
// counter is off or has no figure yet.
void FpsCounterDraw(IDirect3DDevice9* device);
// The state block belongs to the device: released before every Reset.
void ReleaseFpsCounter();

// ---- exposed for tools/fps_counter_check.cpp --------------------------------------------------------------------------------------------------------------
// A frame presented at `ticks` of a clock that runs `perSecond` ticks a second (FpsCounterFrame reads QueryPerformanceCounter and calls this).
void FpsCounterFrameAt(long long ticks, long long perSecond);
// The figure the panel shows (frames a second, 0 = none yet).
int FpsCounterShown();
// The samples of the graph, the oldest first: the frames a second of each quarter of a second.  Copies at most `capacity` of them (the newest ones) and returns how many there are.
int FpsCounterGraph(float* samples, int capacity);
// The colour of the digits: green at the headset's refresh rate, yellow below it, red far below (a headset at 90 Hz that is held to 45 has half its frames).  White when the
// refresh rate is not known.
D3DCOLOR FpsCounterColor(int fps, double refreshHz);
// The 5 x 7 pixels of a character of the font (0-9, F, P, S), one byte per row with the leftmost pixel in bit 4; false for any other character.
bool FpsCounterGlyph(char c, unsigned char rows[7]);
// The layout, in font pixels (the plate behind the text first, then the text, which is right-aligned digits and the label; the graph has a plate of the same size under it):
const int kFpsGlyphWidth = 5, kFpsGlyphHeight = 7;
const int kFpsMargin = 2;     // the plate reaches this far round the text
const int kFpsAdvance = 6;    // from one character to the next
const int kFpsDigits = 3;     // room for 999
const int kFpsLabelGap = 2;   // after the last digit's slot
const int kFpsPlateWidth = 2 * kFpsMargin + kFpsDigits * kFpsAdvance + kFpsLabelGap + 3 * kFpsAdvance - 1;
const int kFpsPlateHeight = 2 * kFpsMargin + kFpsGlyphHeight;
// The graph is drawn in cells of half a font pixel, one column of cells for each sample, the newest at the right: the line is kFpsGraphThickness cells thick and can be at
// kFpsGraphLevels heights, the lowest sample of those in the graph at the bottom and the highest at the top.  A rate that hardly changes would look like a storm, so the scale
// spans at least kFpsGraphMinSpan of the highest sample.  The colour of the line is the colour of the figure for the rate at that point (the lower of two samples joined by a step).
const int kFpsGraphCell = 2;                                                              // cells to a font pixel
const int kFpsGraphMarginX = kFpsMargin, kFpsGraphMarginY = 1;                            // font pixels from the plate's edge to the plot
const int kFpsGraphColumns = (kFpsPlateWidth - 2 * kFpsGraphMarginX) * kFpsGraphCell;    // 74 samples, a little over 18 seconds
const int kFpsGraphRows = (kFpsPlateHeight - 2 * kFpsGraphMarginY) * kFpsGraphCell;      // 18 cells
const int kFpsGraphThickness = 2;
const int kFpsGraphLevels = kFpsGraphRows - kFpsGraphThickness + 1;                       // 17
const double kFpsGraphSampleSeconds = 0.25, kFpsGraphMinSpan = 0.1;
// Where the panel is: its centre as tangents of the angle from straight ahead (+x right, +y up), how far away it seems to be (metres) and how big a font pixel is (a tangent).
const double kFpsCentreX = -0.34, kFpsCentreY = -0.30, kFpsDistance = 1.5, kFpsPixelTan = 0.005;
