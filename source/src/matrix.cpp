// 4x4 matrices in double precision, row-vector convention (like D3DX).
#include "matrix.h"

Matrix4 Identity()
{
    Matrix4 r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 4; i++)
        r.m[i][i] = 1;
    return r;
}
Matrix4 Multiply(const Matrix4& a, const Matrix4& b)
{
    Matrix4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
bool Invert(const Matrix4& a, Matrix4& out)
{
    double work[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            work[i][j] = a.m[i][j];
            work[i][j + 4] = (i == j) ? 1.0 : 0.0;
        }
    for (int column = 0; column < 4; column++) {
        int pivotRow = column;
        for (int row = column + 1; row < 4; row++)
            if (fabs(work[row][column]) > fabs(work[pivotRow][column]))
                pivotRow = row;
        if (fabs(work[pivotRow][column]) < 1e-12)
            return false;
        if (pivotRow != column)
            for (int j = 0; j < 8; j++) {
                double swapTmp = work[column][j];
                work[column][j] = work[pivotRow][j];
                work[pivotRow][j] = swapTmp;
            }
        double pivot = work[column][column];
        for (int j = 0; j < 8; j++)
            work[column][j] /= pivot;
        for (int row = 0; row < 4; row++) {
            if (row == column)
                continue;
            double factor = work[row][column];
            if (factor != 0)
                for (int j = 0; j < 8; j++)
                    work[row][j] -= factor * work[column][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out.m[i][j] = work[i][j + 4];
    return true;
}

// Uploaded constant array A (16 floats, register-major) <-> row-vector math matrix X.
// layout 1: X = A.  layout 2: X = A^T.

void LoadMatrix(const float* a, int layout, Matrix4& x)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            x.m[r][c] = (layout == 2) ? a[c * 4 + r] : a[r * 4 + c];
}
void StoreMatrix(const Matrix4& x, int layout, float* a)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = (float)x.m[r][c];
            if (layout == 2)
                a[c * 4 + r] = v;
            else
                a[r * 4 + c] = v;
        }
}
