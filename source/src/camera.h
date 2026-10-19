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
// Decompose a view-projection matrix without touching the global "main projection" state (needs g_matrixLayout).
bool RecoverCamera(const float* registers, double& xScale, double& yScale, double& depthScale, double& depthBias,
                   CameraAxes& axes);
Matrix4 BuildEyeProjection(const EyeBox& box, int eye);
