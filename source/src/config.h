// BlurVR.ini settings.  Every key has a default; see the settings reference in NOTES.md.
#pragma once
#include "common.h"

struct Config {
    int enabled = 1, log = 0; // log: BlurVR.log is only written with Log=1 under [General]
    int recenterKey = 0x78;     // F9
    int headTrackingKey = 0x79; // F10
    int modeKey = 0x77;         // F8: auto -> force menu -> force race
    int trackRotation = 1, trackPosition = 1;
    double positionScale = 1.0; // game units per metre
    double ipdScale = 1.0;
    int oldVerticalTangents = 0; // [Tracking] OldVerticalTangents: 1 = read GetProjectionRaw's top/bottom the old (swapped) way
    int eyeTransform = 1; // [Tracking] EyeTransform: each eye's pose from SteamVR's eye-to-head transform (rotation included); 0 = half the IPD along x
    int stereo = 1;
    double fovScale = 1.0;
    double mainPassMinWidthFraction = 0.9;
    int requireDepthBuffer = 1;
    int matrixLayoutOverride = 0; // 0 auto, 1 row (X=A), 2 transposed (X=A^T)
    double hudXOffset = 0.0;      // extra inward (convergence) shift per eye, fraction of eye width
    int reflectionFlipMode =
        -1; // reflection-pass delta: -1 auto(by view determinant), 0 none, 1 conj Y, 2 conj X, 3 conj Z
    int skipLensFlares = 0; // drop lens flares: the flare sprites (rear-view mirror) and the bloom/glow around lights
    int fixReflectionLookup =
        1;                // adjust g_ReflectionViewMatrix (reflection-texture lookup) per eye like the reflection pass
    int deferVrStart = 1; // keep the game 2D (SteamVR theatre) until the first level starts loading, then start VR
    int threadedVrInit =
        1; // initialise OpenVR on a worker thread (avoids SteamVR's 'application not responding' dashboard at launch)
    int hideFullscreenOverlay = 1; // don't float an opaque full-screen HUD layer (loading videos) over the race
    int customWidth = 2160,
        customHeight =
            1200; // extra display mode offered to the game (default 2160x1200, the Vive's native side-by-side size); 0 = off
    int veilResultsScreen = 36;   // 0-255 veil over the results screen itself (no popup open)
    int loadingGlow = 1;          // 1 = keep the game's glow on the loading screen, 0 = drop it
    int uiGlowPct = 100;          // 0-200 the game's own glow around menu UI and popups, added over the UI layers (showroom), 0 = off
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
    int maxFps = 0;       // frame rate cap, 0 = none (the headset sets the pace); 60 avoids audio stutter some get at high frame rates
    int forceNoVsync = 1; // 1 = present without V-Sync (the headset sets the frame rate, the game's own V-Sync would cap it at the monitor's)
    int autoRecenter = 1; // 1 = recentre (SteamVR zero pose) every time a race starts; 0 = only on the recentre key
    int perfEnabled = 0;  // [Perf] Enabled: log GPU and CPU frame timing once a second (perf_probe.cpp); the key below flips it live
    int keyPerf = 0x67;   // numpad 7
    int keyVerticalTangents = 0x69; // [Debug] KeyVerticalTangents, numpad 9: OldVerticalTangents on/off
    int keyDump = 0x68;   // [Debug] KeyDump, numpad 8: save the picture sent to the headset and the UI layers as .bmp files (frame_dump.cpp)
    int sharedTransport = 0; // [Render] SharedTransport: 1 = run the game on a Direct3D 9Ex device and hand the finished frame to SteamVR as a shared texture (no read back through the CPU)
    int startAtSavedSize = 1; // [Render] StartAtSavedSize: a windowed start that Windows clamped to the desktop gets the game's saved video mode as its back buffer (display_modes.cpp)
    int startWidth = 0, startHeight = 0; // [Render] StartWidth/StartHeight: the size a windowed start should have (0 = the video mode in the game's settings.xml)
    int coverageEvery = 8; // [Hud] CoverageEvery: a UI draw's screen coverage (an occlusion query the CPU waits for) is measured again every this many frames, staggered; 1 = every draw as before
    int fitDesktopWindow = 1; // [Render] FitDesktopWindow: the game's window is never made bigger than the work area of its monitor (it is only a mirror of the headset)
    int desktopView = 1; // [Render] DesktopView: what the window on the desktop shows in a race: 1 = the left eye's view, 2 = the right eye's, 0 = the whole side-by-side picture
    int desktopPresentEvery = 1; // [Render] DesktopPresentEvery: in a race the game's window gets a real Present only every this many frames (1 = every frame as the game wants)
    int cullHeadFrustum = 1; // [Culling] HeadFrustum: 1 = the game culls with a frustum around the head (both eyes' field of view, the head's orientation), game_camera.cpp
    double cullMarginDeg = 6.0, cullPullBack = 0.3; // [Culling] MarginDeg (added to every side) and PullBack (metres the frustum's apex is moved back)
    int foveationReport = 1; // [Foveation] Report: at VR start, log how much of each eye's image the lenses can show (lens_model.cpp)
};

// The active settings, filled in by LoadConfig().
extern Config g_cfg;
// Read BlurVR.ini from the mod folder; keys that are missing keep their defaults.
void LoadConfig();
