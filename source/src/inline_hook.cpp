// A minimal x86 inline hook for functions in the game's own code.
#include "inline_hook.h"
#include "core.h"

void* InstallInlineHook(void* target, const BYTE* expected, size_t length, void* hook)
{
    if (length < 5 || length > 32)
        return NULL;
    BYTE* at = (BYTE*)target;
    if (memcmp(at, expected, length) != 0) {
        Log("inline hook: bytes at %p are not the expected ones, not hooked", target);
        return NULL;
    }
    // trampoline: the copied instructions, then a jump to the rest of the original function
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return NULL;
    memcpy(tramp, at, length);
    tramp[length] = 0xE9;
    int back = (int)((BYTE*)(at + length) - (tramp + length + 5));
    memcpy(tramp + length + 1, &back, 4);
    DWORD old = 0;
    if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return NULL;
    }
    at[0] = 0xE9;
    int forward = (int)((BYTE*)hook - (at + 5));
    memcpy(at + 1, &forward, 4);
    for (size_t i = 5; i < length; i++)
        at[i] = 0xCC; // (never executed: the jump above leaves the function)
    VirtualProtect(at, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, length);
    FlushInstructionCache(GetCurrentProcess(), tramp, length + 5);
    return tramp;
}
