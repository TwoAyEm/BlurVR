// A minimal x86 inline hook for functions in the game's own code (Blur.exe is 32-bit; no hooking library is used).
#pragma once
#include "common.h"

// Redirect the function at `target` to `hook` with a 5-byte jump.  `expected` are the `length` bytes (at least 5) that must be at
// `target` right now: whole instructions, position independent, because they are copied to a trampoline.  Returns the address that
// runs the original function (call it like the original), or NULL when the bytes are not the expected ones or the patch failed;
// nothing is changed in that case.
void* InstallInlineHook(void* target, const BYTE* expected, size_t length, void* hook);
