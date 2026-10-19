// Frame rate cap (BlurVR.ini [Render] MaxFps).
#pragma once
#include "common.h"

// Called once per frame, right after Present: waits until 1 / maxFps seconds after the previous frame's slot.  The slots keep
// a steady cadence (a late frame does not make the next one shorter); after a long stall the schedule starts over.  maxFps
// <= 0 does nothing.
void LimitFrameRate(int maxFps);
