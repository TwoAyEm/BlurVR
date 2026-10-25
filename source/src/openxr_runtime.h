// OpenXR session ([VR] Api=openxr): the other half of vr_runtime.cpp.  It does what the OpenVR calls do there - start the runtime, give the head and
// eye poses once per frame, take the finished picture - and nothing else; vr_runtime.cpp keeps the maths and the game side.  This header
// has no OpenXR types so that only openxr_runtime.cpp needs the OpenXR headers.
#pragma once
#include "common.h"

struct XrPoseResult {
    float head[3][4];       // the head in the tracking space, OpenVR's layout: [R | t], column vectors, right-handed, -Z forward
    bool menuReplace;       // the floating menu screen has to be placed again (dashboard opened or closed, space recentred)
    bool recenter;          // the runtime moved the tracking space (its recentre): take a new reference
};

// Load openxr_loader.dll, create the instance, session and spaces, make the Direct3D 11 device the runtime wants (g_vr.d3d11Device and
// d3d11Context) and read the field of view.  rawTangents: per eye left, right, up, down (up and down positive); eyeToHead: per eye the pose
// in OpenVR's layout.  False, with the reason in the log, when OpenXR cannot be used.
bool XrRuntimeInit(float rawTangents[2][4], float eyeToHead[2][3][4]);
// Once per frame, after the previous frame was submitted: handle the runtime's events, wait for the next frame, begin it and locate the head
// and the eyes.  False: no usable pose this frame (the frame, when begun, is still ended by XrRuntimeFinishFrame).
bool XrRuntimeNextPose(XrPoseResult* result);
// The eye-to-head transform of the last located frame, OpenVR's layout.
void XrRuntimeEyeToHead(int eye, float m[3][4]);
// True while the headset shows this application but something else has the focus (a dashboard or a system menu is in front of it).  Not true in
// SYNCHRONIZED, where SteamVR holds the session while the headset is not worn.
bool XrRuntimeCovered();
// True while the application has the focus.
bool XrRuntimeFocused();
// The session's state as a word ("synchronized", "visible", "focused", ...), for logs.
const char* XrRuntimeSessionState();
// The finished race picture (both eyes side by side, width x height) to the headset as a projection layer.
void XrRuntimeSubmitScene(ID3D11Texture2D* picture, UINT width, UINT height);
// A flat picture on a screen floating in the tracking space: menuPose is its pose (OpenVR's layout, the screen faces +Z), widthMetres its width.
void XrRuntimeSubmitMenu(ID3D11Texture2D* picture, UINT width, UINT height, const float menuPose[3][4], double widthMetres);
// End the frame XrRuntimeNextPose began, with no layers when nothing was submitted.  Safe to call when no frame is open.
void XrRuntimeFinishFrame();
