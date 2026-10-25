// Recovering the game camera/projection from g_mViewProj and building the headset projection for each eye.
#pragma once
#include "common.h"
#include "matrix.h"
#include "vr_runtime.h"

extern int g_matrixLayout;
extern bool g_haveProjection;
extern double g_projXScale;
extern double g_projYScale;
extern double g_projDepthScale;
extern double g_projDepthBias;
bool TryRecoverProjection(const float* registers);
// Orientation of a camera in world space (unit vectors), read from its view-projection matrix.
struct CameraAxes {
    double right[3], up[3], forward[3];
    bool valid = false;
};
extern CameraAxes g_lastCameraAxes; // of the last view-projection recovered by TryRecoverProjection
// The camera of the last main-pass draw (set by NoteMainCameraAxes): its axes and the x/y scales of its projection (cot of the half
// angles).  The culling code uses it to tell which camera the game hands over is the one the player sees.
struct MainCameraInfo {
    CameraAxes axes;
    double xScale = 1, yScale = 1;
    bool valid = false;
};
extern MainCameraInfo g_mainCamera;
// Decompose a view-projection matrix without touching the global "main projection" state (needs g_matrixLayout).
bool RecoverCamera(const float* registers, double& xScale, double& yScale, double& depthScale, double& depthBias,
                   CameraAxes& axes);
Matrix4 BuildEyeProjection(const EyeBox& box, int eye);
// The camera of a planar reflection against the camera it mirrors (world y up, a horizontal mirror).  Blur builds it with the position and the forward
// axis mirrored and one more axis turned round to keep a proper rotation (its up axis).  `mirrored` and `camera` hold the axes in world space, row 0 the
// right axis, row 1 the up axis, row 2 the forward axis.  Returns the axis that is turned round (0 right, 1 up, 2 forward), or -1 when the two cameras are
// not such a pair (not mirrors of each other, or turned in more than one axis, as the rear-view camera is).
int MirrorFlipAxis(const double mirrored[3][3], const double camera[3][3]);
// The delta of a main camera (camera space to eye camera space) as the mirrored camera sees it: conjugated by the flip of `axis` (0..2).
Matrix4 FlipConjugate(const Matrix4& delta, int axis);
// A delta (camera space to eye camera space, row vectors) with the head's pitch and roll taken out: the eye looks along the horizontal direction it points in (its yaw about the
// camera's up axis), upright, and stays where it was (the position of the eye in camera space is kept).  The planar reflection is drawn with it: the game draws it for a camera that
// looks straight ahead and lays a fixed band of ground haze below the horizon of that camera over it, which a pitched reflection would run into.
Matrix4 YawOnlyDelta(const Matrix4& delta);
