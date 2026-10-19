// Shared includes and small helpers used by every BlurVR source file.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// A draw call that has been intercepted but not issued yet.  The hooks wrap the game's original call in one of
// these so the dispatcher can run it once, once per eye, redirect it to another render target, or skip it.
// It only borrows the callable, so it must not outlive the full expression that created it.
class DrawCall {
public:
    template <typename Callable>
    DrawCall(const Callable& callable)
        : context_(&callable), invoke_([](const void* ctx) { return (*static_cast<const Callable*>(ctx))(); }) {}

    HRESULT operator()() const { return invoke_(context_); }

private:
    const void* context_;
    HRESULT (*invoke_)(const void*);
};
