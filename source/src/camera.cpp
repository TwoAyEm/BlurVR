// Recovering the game camera/projection from g_mViewProj and building the headset projection for each eye.
#include "camera.h"
#include "config.h"
#include "core.h"

int g_matrixLayout = 0;

// Decompose a view-projection matrix  X = V * P  (V rigid, P = [[a,0,0,0],[0,b,0,0],[0,0,c,1],[0,0,d,0]], left-handed).
//   X[:,3]   = V[:,2]                       (unit forward vector + translation z)
//   X[:,0]   = a * V[:,0],  X[:,1] = b * V[:,1],  X[:,2] = c * V[:,2] + d * e3
static bool DecomposeViewProjection(const Matrix4& viewProj, double& xScale, double& yScale, double& depthScale,
                                    double& depthBias)
{
    double forward[3] = {viewProj.m[0][3], viewProj.m[1][3], viewProj.m[2][3]};
    double col0[3] = {viewProj.m[0][0], viewProj.m[1][0], viewProj.m[2][0]};
    double col1[3] = {viewProj.m[0][1], viewProj.m[1][1], viewProj.m[2][1]};
    double col2[3] = {viewProj.m[0][2], viewProj.m[1][2], viewProj.m[2][2]};
    auto dot = [](const double* p, const double* q) {
        return p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
    };
    double forwardLength = sqrt(dot(forward, forward));
    if (fabs(forwardLength - 1.0) > 0.03)
        return false;
    xScale = sqrt(dot(col0, col0));
    yScale = sqrt(dot(col1, col1));
    if (xScale < 0.05 || yScale < 0.05 || xScale > 50 || yScale > 50)
        return false;
    if (fabs(dot(col0, col1)) > 0.03 * xScale * yScale)
        return false;
    if (fabs(dot(col0, forward)) > 0.03 * xScale)
        return false;
    if (fabs(dot(col1, forward)) > 0.03 * yScale)
        return false;
    double crossProduct[3] = {col0[1] * col1[2] - col0[2] * col1[1], col0[2] * col1[0] - col0[0] * col1[2],
                              col0[0] * col1[1] - col0[1] * col1[0]};
    if (dot(crossProduct, forward) <= 0)
        return false;
    depthScale = dot(col2, forward);
    double residual[3] = {col2[0] - depthScale * forward[0], col2[1] - depthScale * forward[1],
                          col2[2] - depthScale * forward[2]};
    if (sqrt(dot(residual, residual)) > 0.03 * fmax(1.0, fabs(depthScale)))
        return false;
    depthBias = viewProj.m[3][2] - depthScale * viewProj.m[3][3];
    // Blur uses a reversed depth range (c ~ 0, d > 0), so do not insist on the usual signs.
    return fabs(depthScale) < 1e4 && fabs(depthBias) > 1e-9 && fabs(depthBias) < 1e7;
}

CameraAxes g_lastCameraAxes;

bool RecoverCamera(const float* registers, double& xScale, double& yScale, double& depthScale, double& depthBias,
                   CameraAxes& axes)
{
    if (!g_matrixLayout)
        return false;
    Matrix4 viewProj;
    LoadMatrix(registers, g_matrixLayout, viewProj);
    if (!DecomposeViewProjection(viewProj, xScale, yScale, depthScale, depthBias))
        return false;
    for (int i = 0; i < 3; i++) {
        axes.right[i] = viewProj.m[i][0] / xScale;
        axes.up[i] = viewProj.m[i][1] / yScale;
        axes.forward[i] = viewProj.m[i][3];
    }
    axes.valid = true;
    return true;
}

bool g_haveProjection = false;
double g_projXScale = 1;  // game projection (a, b, c, d)
double g_projYScale = 1;
double g_projDepthScale = 1;
double g_projDepthBias = -1;

bool TryRecoverProjection(const float* registers)
{
    double xScale, yScale, depthScale, depthBias;
    Matrix4 viewProj;
    if (g_matrixLayout) {
        LoadMatrix(registers, g_matrixLayout, viewProj);
        if (!DecomposeViewProjection(viewProj, xScale, yScale, depthScale, depthBias))
            return false;
    } else {
        int layoutsToTry[2] = {1, 2};
        bool ok = false;
        for (int i = 0; i < 2 && !ok; i++) {
            if (g_cfg.matrixLayoutOverride && g_cfg.matrixLayoutOverride != layoutsToTry[i])
                continue;
            LoadMatrix(registers, layoutsToTry[i], viewProj);
            if (DecomposeViewProjection(viewProj, xScale, yScale, depthScale, depthBias)) {
                g_matrixLayout = layoutsToTry[i];
                ok = true;
                Log("matrix layout detected from g_mViewProj: %d (1 = row, 2 = transposed)", g_matrixLayout);
            }
        }
        if (!ok)
            return false;
    }
    g_projXScale = xScale;
    g_projYScale = yScale;
    g_projDepthScale = depthScale;
    g_projDepthBias = depthBias;
    g_haveProjection = true;
    for (int i = 0; i < 3; i++) {
        g_lastCameraAxes.right[i] = viewProj.m[i][0] / xScale;
        g_lastCameraAxes.up[i] = viewProj.m[i][1] / yScale;
        g_lastCameraAxes.forward[i] = viewProj.m[i][3];
    }
    g_lastCameraAxes.valid = true;
    static int logCount = 0;
    static double lastYScale = 0;
    if (logCount < 6 && fabs(yScale - lastYScale) > 1e-4) {
        lastYScale = yScale;
        logCount++;
        Log("main projection: vfov %.1f deg, hfov %.1f deg, a=%.4f b=%.4f c=%.5f d=%.5f",
            2 * atan(1.0 / yScale) * 57.29578, 2 * atan(1.0 / xScale) * 57.29578, xScale, yScale, depthScale,
            depthBias);
    }
    return true;
}

Matrix4 BuildEyeProjection(const EyeBox& box, int eye)
{
    Matrix4 proj;
    memset(&proj, 0, sizeof(proj));
    double width = box.right - box.left, height = box.up + box.down;
    if (g_cfg.stereo && eye < 2) { // eye image occupies one half of clip space: left [-1,0], right [0,1]
        proj.m[0][0] = 1.0 / width;
        proj.m[2][0] = (eye == 0) ? -box.right / width : -box.left / width;
    } else {
        proj.m[0][0] = 2.0 / width;
        proj.m[2][0] = -(box.right + box.left) / width;
    }
    proj.m[1][1] = 2.0 / height;
    proj.m[2][1] = -(box.up - box.down) / height;
    proj.m[2][2] = g_projDepthScale;
    proj.m[2][3] = 1.0;
    proj.m[3][2] = g_projDepthBias;
    return proj;
}
