// 4x4 matrices in double precision, row-vector convention (like D3DX).
#pragma once
#include "common.h"

struct Matrix4 {
    double m[4][4];
};

// The identity matrix.
Matrix4 Identity();
// a * b  (row vectors: apply a first, then b).
Matrix4 Multiply(const Matrix4& a, const Matrix4& b);
// out = inverse of a; false when a is singular.
bool Invert(const Matrix4& a, Matrix4& out);
// Convert a game float[16] (in the detected memory layout) to a Matrix4.
void LoadMatrix(const float* a, int layout, Matrix4& x);
// Write a Matrix4 back to a game float[16] in the same layout it was read from.
void StoreMatrix(const Matrix4& x, int layout, float* a);
