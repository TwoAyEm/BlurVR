// Hooks on the game's own code where the main view is given its camera (NOTES.md section 25).  The game culls with the frustum it
// hands over there, long before the first Direct3D call, so this is the place to make it cull for the head instead of its own camera.
#pragma once
#include "common.h"

// Called once per presented frame.  Installs the hooks the first time, with Stereo=1 and when this is the Blur.exe
// build the addresses were read from.
void GameCameraPoll();

// The last camera the player sees, as SetCameraData handed it to the main view: its position in the world, its axes in the world (the
// columns of the view rotation: p_camera = ((p - pos) . right, (p - pos) . up, (p - pos) . fwd)) and the cot of its half angles.  Only
// available while the hooks above are installed.
struct MainCameraPose {
    double pos[3], right[3], up[3], fwd[3];
    double projX, projY;
    unsigned long long frame; // g_vr.frameCount when it was taken
};
// A consistent copy of the pose (the hooks run on the game's thread).  False until a main camera has been seen.
bool GetMainCameraPose(MainCameraPose& out);
// The axis (0 right, 1 up, 2 forward) that the game's planar reflection camera of this frame has turned round against the main camera, as the hooks
// found it when the reflection view was given its camera; -1 when no reflection camera was seen lately or the hooks are not installed.  The reflection
// pass is drawn with the head's delta conjugated by that flip (stereo_scene.cpp) and culled with a frustum made the same way.
int ReflectionFlipAxis();
// What the hook does with each main camera it sees (and what a test does to give the pose).
void PublishMainCameraPose(const MainCameraPose& pose);
