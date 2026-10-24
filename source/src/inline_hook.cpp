// A minimal x86 inline hook for functions in the game's own code, and the import-table patch.
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

bool PatchImport(HMODULE module, const char* dll, const char* name, void* hook, void** original)
{
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva)
        return false;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) || !imp->OriginalFirstThunk)
            continue;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal) || strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData))->Name, name))
                continue;
            if ((void*)ft->u1.Function == hook)
                return true; // already done
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            *original = (void*)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}
