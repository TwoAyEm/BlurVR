// Frame timing probe (BlurVR.ini [Perf] Enabled, numpad 7 in game): GPU time per rendering phase from D3D9 timestamp
// queries, CPU time of the draw hooks and of the present path, summarised in BlurVR.log once a second.  A branch per
// call while it is off.
#pragma once
#include "common.h"

enum PerfPhase {
    PH_OFFSCREEN,   // shadow maps, small or oddly shaped targets, anything not recognised
    PH_SCENE_MAIN,  // 3D geometry into main-sized targets (the stereo-duplicated pass)
    PH_SCENE_HALF,  // 3D geometry into half/quarter-size targets (planar reflection, glow sprites)
    PH_LIGHTS,      // deferred lights, full-screen and volumes
    PH_MOTION_BLUR,
    PH_MAIN_OTHER,  // other draws into main-sized targets (composite, tone mapping)
    PH_POST_LOW,    // draws into reduced targets of the same shape (bloom chain, blurs)
    PH_HUD,         // the game's UI draws, diverted into the HUD layers
    PH_HUD_COMPOSE, // our HUD composition into the eyes
    PH_COPY,        // copy of the finished frame for the compositor
    PH_COUNT
};
enum PerfSection { // CPU sections of the present path
    PS_COMPOSE,
    PS_READBACK,
    PS_UPLOAD,
    PS_SUBMIT,
    PS_PRESENT,
    PS_POSE,
    PS_COUNT
};

extern bool g_perfActive;

// Numpad key (or the ini) flips the probe.
void PerfToggle();
// Switch the GPU phase by hand (a CPU-side stage the draw classifier cannot see); it stays until the frame ends.
void PerfSetPhase(IDirect3DDevice9* device, PerfPhase phase);
// Close the frame being recorded; sceneFrame = it was a race frame (others are measured but not reported).
void PerfFrameClose(IDirect3DDevice9* device, bool sceneFrame);
// Collect finished GPU frames and write the once-a-second summary.
void PerfPoll();
void ReleasePerf();

long long PerfNow();
void PerfAddSection(PerfSection section, long long ticks);
void PerfClassifyDraw(IDirect3DDevice9* device, int plan);
void PerfAddDraw(long long hookTicks);
void PerfDrawEnd(IDirect3DDevice9* device);
void PerfAddDriver(long long driverTicks);

// Times a CPU section of the present path.
class PerfScope {
public:
    explicit PerfScope(PerfSection section) : section_(section), start_(g_perfActive ? PerfNow() : 0) {}
    ~PerfScope()
    {
        if (start_)
            PerfAddSection(section_, PerfNow() - start_);
    }

private:
    PerfSection section_;
    long long start_;
};

// Around one intercepted draw call (RunDraw): its CPU time, and its phase once the dispatcher has classified it.
class PerfDrawScope {
public:
    explicit PerfDrawScope(IDirect3DDevice9* device) : device_(device), start_(g_perfActive ? PerfNow() : 0) {}
    void Classified(int plan)
    {
        if (start_)
            PerfClassifyDraw(device_, plan);
    }
    ~PerfDrawScope()
    {
        if (start_) {
            PerfDrawEnd(device_);
            PerfAddDraw(PerfNow() - start_);
        }
    }

private:
    IDirect3DDevice9* device_;
    long long start_;
};

// Around the game's own draw call inside a hook: the part of the time that is the driver's.
class PerfDriverScope {
public:
    PerfDriverScope() : start_(g_perfActive ? PerfNow() : 0) {}
    ~PerfDriverScope()
    {
        if (start_)
            PerfAddDriver(PerfNow() - start_);
    }

private:
    long long start_;
};
