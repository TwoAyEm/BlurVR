// A minimal x86 inline hook for functions in the game's own code (Blur.exe is 32-bit; no hooking library is used), and the patch of an entry in a module's import table.
#pragma once
#include "common.h"

// Redirect the function at `target` to `hook` with a 5-byte jump.  `expected` are the `length` bytes (at least 5) that must be at
// `target` right now: whole instructions, position independent, because they are copied to a trampoline.  Returns the address that
// runs the original function (call it like the original), or NULL when the bytes are not the expected ones or the patch failed;
// nothing is changed in that case.
void* InstallInlineHook(void* target, const BYTE* expected, size_t length, void* hook);

// Point the import-table entry of `module` (the executable) for function `name` of `dll` at `hook`; *original gets what it pointed at.  True when the entry is
// hooked (also when it was already), false when the module does not import it.
bool PatchImport(HMODULE module, const char* dll, const char* name, void* hook, void** original);
