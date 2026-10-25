// BlurVR.ini settings.  Every key has a default; see the settings reference in NOTES.md.
#pragma once
#include "common.h"

// What g_cfg.desktopView is for [VR] DesktopView=off
const int kDesktopViewOff = 3;
// What g_cfg.vrApi is for [VR] Api=openvr / openxr
const int kVrApiOpenVr = 0, kVrApiOpenXr = 1;

struct Config {
    int enabled = 1, log = 0; // enabled: [General] Enabled, the master switch (0 = the mod does nothing); log: BlurVR.log is only written with Log=1 under [General]
    int vr = 1;               // enabled AND stereo: the VR half of the mod runs (SteamVR, the size override, the Ex device, ...).  Enabled=1 Stereo=0 is "flat": only the [Display] and [Input] settings apply
    int recenterKey = 0x78;     // F9
    int headTrackingKey = 0x79; // F10
    int modeKey = 0x77;         // F8: auto -> force menu -> force race
    int trackRotation = 1, trackPosition = 1;
    double positionScale = 1.0; // game units per metre
    double ipdScale = 1.0;
    int eyeTransform = 1; // [Tracking] EyeTransform: each eye's pose from SteamVR's eye-to-head transform (rotation included); 0 = half the IPD along x
    int stereo = 1; // [General] Stereo: 1 = VR, 0 = flat (no SteamVR).  Use g_cfg.vr to ask "is VR wanted"
    double fovScale = 1.0;
    double mainPassMinWidthFraction = 0.9;
    int requireDepthBuffer = 1;
    int matrixLayoutOverride = 0; // 0 auto, 1 row (X=A), 2 transposed (X=A^T)
    double hudXOffset = 0.0;      // extra inward (convergence) shift per eye, fraction of eye width
    int reflectionFlipMode =
        -1; // reflection-pass delta: -1 auto (the axis the game's reflection camera has turned round, else by the view determinant), 0 none, 1 conj X, 2 conj Y, 3 conj Z
    int skipLensFlares = 0; // [VR] NoLensFlare: drop lens flares: the flare sprites (rear-view mirror) and the bloom/glow around lights
    int motionBlur = 1;     // [VR] MotionBlur: 1 = the game's motion blur, drawn per eye; 0 = its pass copies the picture instead of blurring it (screen_space_passes.cpp)
    int fixReflectionLookup =
        1;                // adjust g_ReflectionViewMatrix (reflection-texture lookup) per eye like the reflection pass
    int deferVrStart = 1; // keep the game 2D (SteamVR theatre) until the first level starts loading, then start VR
    int threadedVrInit =
        1; // initialise OpenVR on a worker thread (avoids SteamVR's 'application not responding' dashboard at launch)
    int hideFullscreenOverlay = 1; // don't float an opaque full-screen HUD layer (loading videos) over the race
    int vrWidth = 2160, vrHeight = 1200; // [VR] Resolution: the VR game's picture size (the default is the Vive's native side-by-side size); BlurVR.ini keeps it, see video_settings.cpp
    int veilResultsScreen = 36;   // 0-255 veil over the results screen itself (no popup open)
    int bloomPct = 100;           // [VR] Bloom, 0-200 %: the game's own glow around menu UI, popups and loading screens (100 = as the game draws it, 0 = none)
    int veilRace = 120;           // 0-255 darkness of the veil drawn under race popups (options / results)
    int veilMenu = 100;           // 0-255 darkness of the veil drawn under menu dialogs (connecting, quit)
    int dimStrongPct = 60;        // 0-100 darkness of the scene dim behind pause/results menus
    int dropBackdropCopies = 1;   // skip full-size blended scene copies drawn into the HUD layer (dim/blur backdrops)
    int stereoHalfResPasses = 1; // also stereo-duplicate half-resolution 3D passes (flares/glows)
    int hudWorldLocked =
        1; // 1: render the HUD to a texture and show it as a flat panel fixed in the world; 0: panel glued to the view
    int seamGutter =
        0;             // pixels of black left free at the inner edge of each eye (stops glow bleeding across the seam)
    double hudScale = 0.6, hudDistance = 2.0, hudOffsetY = 0.0;
    double menuDistance = 2.2, menuWidth = 2.6, menuHeight = 0.0;
    int adjustViewRot = 1;
    int startMode = 0; // 0 auto, 1 menu, 2 race
    int countInDrop = 1; // 1 = hide the race-start letterbox pieces (old behaviour); 0 = draw them into the HUD
    int mirror3d = 1;          // rear-view mirror drawn as its own panel, rendered once per eye
    double mirrorConvergence = 1.5;   // depth (m, in the mirror camera) that sits exactly at the panel; nearer pops out, farther recedes
    double mirrorEyeScale = 1.0;     // scale of the eye separation used for the rear view
    double mirrorHeadParallax = 1.0;  // how much of the head position moves the rear view (0 = only the eye separation)
    // in-game toggle key for the 3D mirror (virtual-key code, 0 = unused); numpad 6 by default
    int keyMirror = 0x66;
    int keyRepeatMs = 60; // [Input] KeyRepeatMs: menu keys repeat at most once per this many ms, whatever the frame rate (input_limiter.cpp); 0 = as the game does it
    int forceNoVsync = 0; // [Display] ForceNoVSync: 1 = present without V-Sync, 0 = the game's own setting.  In VR it is always off: the headset sets the frame rate
    int autoRecenter = 1; // 1 = recentre (SteamVR zero pose) every time a race starts; 0 = only on the recentre key
    int perfEnabled = 0;  // [Perf] Enabled: log GPU and CPU frame timing once a second (perf_probe.cpp); the key below flips it live
    int keyPerf = 0x67;   // numpad 7
    int keyDump = 0x68;   // [Debug] KeyDump, numpad 8: save the picture sent to the headset and the UI layers as .bmp files (frame_dump.cpp)
    int fpsCounter = 0;   // [VR] FpsCounter: 1 = a small panel in the headset shows the frames a second the game presents (fps_counter.cpp); the key below flips it live
    int keyFps = 0x69;    // [Debug] KeyFps, numpad 9
    int sharedTransport = 1; // [VR] SharedTransport (VR only, on by default; falls back to the CPU copy by itself if it cannot work): 1 = run the game on a Direct3D 9Ex device and hand the finished frame to SteamVR as a shared texture (no read back through the CPU)
    int startAtSavedSize = 0; // VR only (set by LoadConfig): a windowed start that Windows clamped to the desktop gets the start size as its back buffer (display_modes.cpp, window_spoof.cpp)
    int startWidth = 0, startHeight = 0; // VR only (set by LoadConfig): the size the start should have, the VR resolution
    int coverageEvery = 8; // [Hud] CoverageEvery: a UI draw's screen coverage (an occlusion query the CPU waits for) is measured again every this many frames, staggered; 1 = every draw as before
    int fitDesktopWindow = 1; // [Render] FitDesktopWindow: the game's window is never made bigger than the work area of its monitor (it is only a mirror of the headset)
    int vrApi = kVrApiOpenVr; // [VR] Api: which API the headset is reached through: openvr (SteamVR, the default) or openxr (the system's active OpenXR runtime; vr_runtime.cpp, openxr_runtime.cpp)
    char openxrRuntime[MAX_PATH] = ""; // [VR] OpenXRRuntime: the .json of the OpenXR runtime to use instead of the system's active one (empty = the system's); only with Api=openxr
    int desktopView = kDesktopViewOff; // [VR] DesktopView (default off since 1.5): what the window on the desktop shows in a race: 1 = the left eye's view, 2 = the right eye's, 0 = the whole side-by-side picture, kDesktopViewOff = "Playing in VR" and the game's logo (desktop_message.cpp)
    int desktopPresentEvery = 1; // [Render] DesktopPresentEvery: in a race the game's window gets a real Present only every this many frames (1 = every frame as the game wants)
    double cullMarginDeg = 6.0, cullPullBack = 0.3; // [Culling] MarginDeg (added to every side) and PullBack (metres the frustum's apex is moved back) of the frustum around the head that the game culls with in VR, game_camera.cpp
    int foveationReport = 1; // [Foveation] Report: at VR start, log how much of each eye's image the lenses can show (lens_model.cpp)
};

// The active settings, filled in by LoadConfig().
extern Config g_cfg;
// Read BlurVR.ini from the mod folder; keys that are missing keep their defaults.
void LoadConfig();
