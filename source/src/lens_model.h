// Headset facts for foveation planning.
#pragma once
#include "common.h"

// Log the headset model, refresh rate, SteamVR render size, eye-tracking support and hidden-area share (see lens_model.cpp).
// Runs on a worker thread of its own and only writes to BlurVR.log.
void StartLensReportThread();
